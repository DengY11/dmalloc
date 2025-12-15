#include "../include/dmalloc.h"
#include "../include/page_heap.h"
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>



static CentralFreeList central[ (MAX_SMALL / D_ALIGN) ];

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
            pthread_mutex_init(&central[i].lock, NULL);
            central[i].head = NULL;
            central[i].obj_size = (i + 1) * D_ALIGN;
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

    pthread_mutex_lock(&central[sc].lock);
    for (size_t i = 0; i < capacity; i++){
        uint8_t* p = base + offset + i * slot;
        ObjHdr* h = (ObjHdr*)p;
        h->owner = (void*)ss;
        h->size_class = (uint16_t)sc;
        h->flags = 0;
        void* user = (void*)(p + hdr);
        /* push into central freelist */
        *(void**)user = central[sc].head;
        central[sc].head = user;
        ss->total_objs++;
        ss->free_objs++;
    }
    pthread_mutex_unlock(&central[sc].lock);
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
        Span* sp = span_alloc(npages);
        if (!sp) return NULL;
        uint8_t* base = (uint8_t*)span_ptr(sp);
        ObjHdr* h = (ObjHdr*)base;
        h->owner = (void*)sp;
        h->size_class = 0xFFFFu;
        h->flags = 1; /* large */
        return (void*)(base + obj_header_size());
    }
    pthread_mutex_lock(&central[sc].lock);
    if (!central[sc].head){
        pthread_mutex_unlock(&central[sc].lock);
        central_grow(sc);
        pthread_mutex_lock(&central[sc].lock);
    }
    if (!central[sc].head){
        pthread_mutex_unlock(&central[sc].lock);
        return NULL;
    }
    void* user = central[sc].head;
    central[sc].head = *(void**)user;
    ObjHdr* h = (ObjHdr*)((uint8_t*)user - obj_header_size());
    SmallSpan* ss = (SmallSpan*)h->owner;
    if (ss) ss->free_objs--;
    pthread_mutex_unlock(&central[sc].lock);
    return user;
}

void dfree(void* ptr)
{
    if (!ptr) return;
    ObjHdr* h = (ObjHdr*)((uint8_t*)ptr - obj_header_size());
    if (h->flags & 1){
        /* large: free span back to page heap */
        Span* sp = (Span*)h->owner;
        span_free(sp);
        return;
    }
    /* small: push back to central list */
    int sc = (int)h->size_class;
    pthread_mutex_lock(&central[sc].lock);
    *(void**)ptr = central[sc].head;
    central[sc].head = ptr;
    SmallSpan* ss = (SmallSpan*)h->owner;
    if (ss) ss->free_objs++;
    /* if this SmallSpan is fully free, reclaim the whole span */
    if (ss && ss->free_objs == ss->total_objs){
        /* remove all nodes from central list that belong to this SmallSpan */
        void* cur = central[sc].head;
        void* prev = NULL;
        while (cur){
            ObjHdr* oh = (ObjHdr*)((uint8_t*)cur - obj_header_size());
            void* next = *(void**)cur;
            if (oh->owner == (void*)ss){
                /* unlink cur */
                if (prev){
                    *(void**)prev = next;
                } else {
                    central[sc].head = next;
                }
                cur = next;
                continue;
            }
            prev = cur;
            cur = next;
        }
        pthread_mutex_unlock(&central[sc].lock);
        /* find backing Span by address and free it to page heap */
        Span* backing = pageheap_span_for_addr((void*)ss);
        if (backing){
            span_free(backing);
        }
        /* Note: ss memory is part of the span and will be invalid after span_free */
        return;
    }
    pthread_mutex_unlock(&central[sc].lock);
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
