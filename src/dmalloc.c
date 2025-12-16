#include "../include/dmalloc.h"
#include "../include/page_heap.h"
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>




/* Center holds per-class central freelists; page-heap operations protected by one big lock */
static CentralFreeList central[ (MAX_SMALL / D_ALIGN) ];
static pthread_mutex_t span_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t center_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t   tcache_key;
static pthread_once_t  once_key = PTHREAD_ONCE_INIT;

static inline size_t round_up(size_t x, size_t a){ return (x + a - 1) & ~(a - 1); }

static inline int size_class_for(size_t size){
    if (size == 0) size = 1;
    size_t need = round_up(size, D_ALIGN);
    if (need > MAX_SMALL) return -1;
    return (int)(need / D_ALIGN) - 1;
}

static inline size_t obj_header_size(void){ return round_up(sizeof(ObjHdr), D_ALIGN); }
static inline size_t small_span_header_size(void){ return round_up(sizeof(SmallSpan), D_ALIGN); }

static void central_init_once(void)
{
    /* init_state: 0=uninitialized, 1=initializing, 2=initialized */
    static atomic_int init_state = ATOMIC_VAR_INIT(0);
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&init_state, &expected, 1,
                                               memory_order_acq_rel, memory_order_acquire)){
        /* we won the initialization */
        for (size_t i = 0; i < (MAX_SMALL / D_ALIGN); i++){
            central[i].span_head = NULL;
            central[i].obj_size = (i + 1) * D_ALIGN;
        }
        pthread_key_create(&tcache_key, NULL);
        atomic_store_explicit(&init_state, 2, memory_order_release);
        return;
    }
    /* another thread is initializing or already initialized; wait until done */
    while (atomic_load_explicit(&init_state, memory_order_acquire) != 2) {
        /* spin */
    }
}

static ThreadCache* get_tcache(void)
{
    pthread_once(&once_key, (void(*)(void))central_init_once);
    ThreadCache* tc = (ThreadCache*)pthread_getspecific(tcache_key);
    if (!tc){
        tc = (ThreadCache*)calloc(1, sizeof(ThreadCache));
        for (size_t i = 0; i < (MAX_SMALL / D_ALIGN); i++) tc->limit[i] = 128;
        pthread_setspecific(tcache_key, tc);
    }
    return tc;
}

static void central_grow(int sc)
{
    size_t ps = pageheap_page_size();
    size_t payload = central[sc].obj_size;
    size_t hdr = obj_header_size();
    size_t slot = hdr + payload;
    size_t span_hdr = small_span_header_size();

    /* choose pages so that we have at least one object */
    size_t npages = 1;
    while ((npages * ps) < (span_hdr + slot)) npages++;

    pthread_mutex_lock(&span_mu);
    Span* sp = span_alloc(npages);
    pthread_mutex_unlock(&span_mu);
    if (!sp) return;
    uint8_t* base = (uint8_t*)span_ptr(sp);
    size_t   bytes = span_page_count(sp) * ps;

    SmallSpan* ss = (SmallSpan*)base;
    ss->size_class = (size_t)sc;
    ss->total_objs = 0;
    ss->free_objs  = 0;
    ss->free_list  = NULL;
    ss->next       = NULL;

    size_t offset = span_hdr;
    size_t capacity = (bytes > offset) ? ((bytes - offset) / slot) : 0;
    if (capacity == 0){
        /* pathological: release span and bail */
        span_free(sp);
        return;
    }

    for (size_t i = 0; i < capacity; i++){
        uint8_t* p = base + offset + i * slot;
        ObjHdr* h = (ObjHdr*)p;
        h->owner = (void*)ss;
        h->size_class = (uint16_t)sc;
        h->flags = 0;
        void* user = (void*)(p + hdr);
        *(void**)user = ss->free_list;
        ss->free_list = user;
        ss->total_objs++;
        ss->free_objs++;
    }
    pthread_mutex_lock(&center_mu);
    ss->next = central[sc].span_head;
    central[sc].span_head = ss;
    pthread_mutex_unlock(&center_mu);
}

void* dmalloc(size_t size)
{
    central_init_once();
    int sc = size_class_for(size);
    if (sc < 0){
        /* large object: allocate span of sufficient pages */
        size_t ps = pageheap_page_size();
        size_t need = round_up(size + obj_header_size(), D_ALIGN);
        size_t npages = (need + ps - 1) / ps;
        pthread_mutex_lock(&span_mu);
        Span* sp = span_alloc(npages);
        pthread_mutex_unlock(&span_mu);
        if (!sp) return NULL;
        uint8_t* base = (uint8_t*)span_ptr(sp);
        ObjHdr* h = (ObjHdr*)base;
        h->owner = (void*)sp;
        h->size_class = 0xFFFFu;
        h->flags = 1; /* large */
        return (void*)(base + obj_header_size());
    }
    ThreadCache* tc = get_tcache();
    if (!tc->head[sc]){
        pthread_mutex_lock(&center_mu);
        SmallSpan* prev = NULL;
        SmallSpan* cur  = central[sc].span_head;
        while (cur && !cur->free_list){ prev = cur; cur = cur->next; }
        if (!cur){
            pthread_mutex_unlock(&center_mu);
            central_grow(sc);
            pthread_mutex_lock(&center_mu);
            prev = NULL;
            cur  = central[sc].span_head;
            while (cur && !cur->free_list){ prev = cur; cur = cur->next; }
        }
        int batch = 32;
        while (cur && cur->free_list && batch--){
            void* user = cur->free_list;
            cur->free_list = *(void**)user;
            *(void**)user = tc->head[sc];
            tc->head[sc] = user;
            tc->count[sc]++;
        }
        if (cur && !cur->free_list){
            if (prev) prev->next = cur->next; else central[sc].span_head = cur->next;
            cur->next = NULL;
        }
        pthread_mutex_unlock(&center_mu);
    }
    if (!tc->head[sc]) return NULL;
    void* user = tc->head[sc];
    tc->head[sc] = *(void**)user;
    tc->count[sc]--;
    ObjHdr* h = (ObjHdr*)((uint8_t*)user - obj_header_size());
    SmallSpan* ss = (SmallSpan*)h->owner;
    if (ss) ss->free_objs--; /* defensive */
    return user;
}

void dfree(void* ptr)
{
    if (!ptr) return;
    ObjHdr* h = (ObjHdr*)((uint8_t*)ptr - obj_header_size());
    if (h->flags & 1){
        /* large: free span back to page heap */
        Span* sp = (Span*)h->owner;
        pthread_mutex_lock(&span_mu);
        span_free(sp);
        pthread_mutex_unlock(&span_mu);
        return;
    }
    /* small: push to thread cache; drain if exceeds limit */
    int sc = (int)h->size_class;
    ThreadCache* tc = get_tcache();
    *(void**)ptr = tc->head[sc];
    tc->head[sc] = ptr;
    tc->count[sc]++;
    SmallSpan* ss = (SmallSpan*)h->owner;
    if (ss) ss->free_objs++;
    if (tc->count[sc] > tc->limit[sc]){
        size_t target = tc->limit[sc] / 2;
        pthread_mutex_lock(&center_mu);
        while (tc->count[sc] > target){
            void* user = tc->head[sc];
            tc->head[sc] = *(void**)user;
            tc->count[sc]--;
            ObjHdr* h = (ObjHdr*)((uint8_t*)user - obj_header_size());
            SmallSpan* ss = (SmallSpan*)h->owner;
            int need_insert = ss->free_list == NULL;
            *(void**)user = ss->free_list;
            ss->free_list = user;
            if (need_insert){
                SmallSpan* it = central[sc].span_head;
                int exists = 0;
                while (it){ if (it == ss){ exists = 1; break; } it = it->next; }
                if (!exists){ ss->next = central[sc].span_head; central[sc].span_head = ss; }
            }
        }
        pthread_mutex_unlock(&center_mu);
    }
}

void* drealloc(void* ptr, size_t size)
{
    if (!ptr) return dmalloc(size);
    ObjHdr* h = (ObjHdr*)((uint8_t*)ptr - obj_header_size());
    /* if small and same class, return as is */
    int new_sc = size_class_for(size);
    if (!(h->flags & 1)){
        int old_sc = (int)h->size_class;
        if (old_sc == new_sc && old_sc >= 0) return ptr;
    }
    void* n = dmalloc(size);
    if (!n) return NULL;
    /* copy min(old_size, new_size) */
    size_t old_payload;
    if (h->flags & 1){
        /* large: compute bytes from span size */
        Span* sp = (Span*)h->owner;
        size_t ps = pageheap_page_size();
        size_t total = span_page_count(sp) * ps;
        old_payload = (total > obj_header_size()) ? (total - obj_header_size()) : 0;
    } else {
        old_payload = central[h->size_class].obj_size;
    }
    size_t copy = old_payload < size ? old_payload : size;
    memcpy(n, ptr, copy);
    dfree(ptr);
    return n;
}
