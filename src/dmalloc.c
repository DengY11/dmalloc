#include "../include/dmalloc.h"
#include "../include/page_heap.h"
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>



static CentralFreeList central[ (MAX_SMALL / D_ALIGN) ];
static pthread_mutex_t central_lock[ (MAX_SMALL / D_ALIGN) ];
static pthread_key_t tc_key;
static pthread_once_t tc_once = PTHREAD_ONCE_INIT;

static inline size_t tcache_max(void){ return 64; }
static inline size_t tcache_refill_batch(void){ return 32; }
static inline size_t tcache_release_batch(void){ return 64; }

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
            central[i].head = NULL;
            central[i].obj_size = (i + 1) * D_ALIGN;
            pthread_mutex_init(&central_lock[i], NULL);
        }
        atomic_store_explicit(&init_state, 2, memory_order_release);
        return;
    }
    /* another thread is initializing or already initialized; wait until done */
    while (atomic_load_explicit(&init_state, memory_order_acquire) != 2) {
        /* spin */
    }
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

    Span* sp = span_alloc(npages);
    if (!sp) return;
    uint8_t* base = (uint8_t*)span_ptr(sp);
    size_t   bytes = span_page_count(sp) * ps;

    /* place SmallSpan header at the beginning of span */
    SmallSpan* ss = (SmallSpan*)base;
    ss->size_class = (size_t)sc;
    ss->total_objs = 0;
    ss->free_objs  = 0;

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
        *(void**)user = central[sc].head;
        central[sc].head = user;
        ss->total_objs++;
        ss->free_objs++;
    }
}

static size_t central_fetch_batch(int sc, void** out, size_t n)
{
    size_t got = 0;
    pthread_mutex_lock(&central_lock[sc]);
    if (!central[sc].head) central_grow(sc);
    while (central[sc].head && got < n){
        void* user = central[sc].head;
        central[sc].head = *(void**)user;
        ObjHdr* h = (ObjHdr*)((uint8_t*)user - obj_header_size());
        SmallSpan* ss = (SmallSpan*)h->owner;
        if (ss) ss->free_objs--;
        out[got++] = user;
    }
    pthread_mutex_unlock(&central_lock[sc]);
    return got;
}

static void central_release_batch(int sc, void** list, size_t n)
{
    pthread_mutex_lock(&central_lock[sc]);
    for (size_t i = 0; i < n; i++){
        void* ptr = list[i];
        *(void**)ptr = central[sc].head;
        central[sc].head = ptr;
        ObjHdr* h = (ObjHdr*)((uint8_t*)ptr - obj_header_size());
        SmallSpan* ss = (SmallSpan*)h->owner;
        if (ss) ss->free_objs++;
    }
    pthread_mutex_unlock(&central_lock[sc]);
}

static void tc_destroy(void* p)
{
    ThreadCache* tc = (ThreadCache*)p;
    if (!tc) return;
    for (size_t i = 0; i < (MAX_SMALL / D_ALIGN); i++){
        size_t sc = i;
        size_t batch = tcache_release_batch();
        void** tmp = (void**)malloc(sizeof(void*) * batch);
        while (tc->lists[i].head){
            size_t n = 0;
            while (tc->lists[i].head && n < batch){
                void* user = tc->lists[i].head;
                tc->lists[i].head = *(void**)user;
                n++;
                tmp[n-1] = user;
            }
            if (n) central_release_batch((int)sc, tmp, n);
        }
        free(tmp);
        tc->lists[i].count = 0;
    }
    free(tc);
}

static void tc_init_key(void)
{
    pthread_key_create(&tc_key, tc_destroy);
}

static ThreadCache* tc_get(void)
{
    pthread_once(&tc_once, tc_init_key);
    ThreadCache* tc = (ThreadCache*)pthread_getspecific(tc_key);
    if (!tc){
        tc = (ThreadCache*)malloc(sizeof(ThreadCache));
        if (!tc) return NULL;
        memset(tc, 0, sizeof(ThreadCache));
        size_t pages[LARGE_BUCKET_COUNT] = {4,8,16,32,64,128,256,512};
        for (int i = 0; i < LARGE_BUCKET_COUNT; i++){
            tc->lbuckets[i].head = NULL;
            tc->lbuckets[i].count = 0;
            tc->lbuckets[i].target = 16;
            tc->lbuckets[i].pages = pages[i];
        }
        pthread_setspecific(tc_key, tc);
    }
    return tc;
}

void* dmalloc(size_t size)
{
    central_init_once();
    int sc = size_class_for(size);
    if (sc < 0){
        if (!pageheap_page_size()) pageheap_init();
        size_t ps = pageheap_page_size();
        size_t need = round_up(size + obj_header_size(), D_ALIGN);
        size_t npages = (need + ps - 1) / ps;
        size_t bytes = npages * ps;
        void* mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) return NULL;
        uint8_t* base = (uint8_t*)mem;
        ObjHdr* h = (ObjHdr*)base;
        h->owner = NULL;
        h->size_class = npages;
        h->flags = 3;
        return (void*)(base + obj_header_size());
    }
    ThreadCache* tc = tc_get();
    if (!tc) return NULL;
    TCacheList* list = &tc->lists[sc];
    if (!list->head){
        size_t batch = tcache_refill_batch();
        void** tmp = (void**)malloc(sizeof(void*) * batch);
        if (!tmp) return NULL;
        size_t got = central_fetch_batch(sc, tmp, batch);
        for (size_t i = 0; i < got; i++){
            void* p = tmp[i];
            *(void**)p = list->head;
            list->head = p;
            list->count++;
        }
        free(tmp);
        if (!list->head) return NULL;
    }
    void* user = list->head;
    list->head = *(void**)user;
    if (list->count) list->count--;
    return user;
}

void dfree(void* ptr)
{
    if (!ptr) return;
    ObjHdr* h = (ObjHdr*)((uint8_t*)ptr - obj_header_size());
    if (h->flags & 1){
        if (h->flags & 2){
            size_t ps = pageheap_page_size();
            size_t npages = h->size_class;
            uint8_t* base = (uint8_t*)h;
            size_t bytes = npages * ps;
            munmap(base, bytes);
            return;
        } else {
            Span* sp = (Span*)h->owner;
            span_free(sp);
            return;
        }
    }
    int sc = (int)h->size_class;
    ThreadCache* tc = tc_get();
    if (!tc){
        void* one = ptr;
        central_release_batch(sc, &one, 1);
        return;
    }
    TCacheList* list = &tc->lists[sc];
    *(void**)ptr = list->head;
    list->head = ptr;
    list->count++;
    if (list->count > tcache_max()){
        size_t batch = tcache_release_batch();
        void** tmp = (void**)malloc(sizeof(void*) * batch);
        size_t n = 0;
        while (list->head && n < batch){
            void* p = list->head;
            list->head = *(void**)p;
            tmp[n++] = p;
        }
        central_release_batch(sc, tmp, n);
        free(tmp);
        if (list->count >= n) list->count -= n; else list->count = 0;
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
        size_t ps = pageheap_page_size();
        size_t total;
        if (h->flags & 2){
            size_t npages = h->size_class;
            total = npages * ps;
        } else {
            Span* sp = (Span*)h->owner;
            total = span_page_count(sp) * ps;
        }
        old_payload = (total > obj_header_size()) ? (total - obj_header_size()) : 0;
    } else {
        old_payload = central[h->size_class].obj_size;
    }
    size_t copy = old_payload < size ? old_payload : size;
    memcpy(n, ptr, copy);
    dfree(ptr);
    return n;
}
