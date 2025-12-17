#include "../include/dmalloc.h"
#include "../include/page_heap.h"
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>



#define CENTRAL_SHARDS 16
static CentralFreeList central[ CENTRAL_SHARDS ][ (MAX_SMALL / D_ALIGN) ];
static pthread_mutex_t central_lock[ CENTRAL_SHARDS ][ (MAX_SMALL / D_ALIGN) ];
/* optional stats */
#ifdef DMALLOC_STATS
static atomic_ulong stat_fetch_tries[ CENTRAL_SHARDS ][ (MAX_SMALL / D_ALIGN) ];
static atomic_ulong stat_fetch_batches[ CENTRAL_SHARDS ][ (MAX_SMALL / D_ALIGN) ];
#endif
static __thread ThreadCache* tls_tc;
static atomic_ulong dfree_counter = ATOMIC_VAR_INIT(0);

static inline size_t tcache_max(void){ return 128; }
static inline size_t tcache_refill_batch_for_sc(int sc){
    size_t obj = central[0][sc].obj_size;
    return (obj <= 64) ? 512 : 256;
}
static inline size_t tcache_release_batch(void){ return 256; }

static inline size_t round_up(size_t x, size_t a){ return (x + a - 1) & ~(a - 1); }

static inline int size_class_for(size_t size){
    if (size == 0) size = 1;
    size_t need = round_up(size, D_ALIGN);
    if (need > MAX_SMALL) return -1;
    return (int)(need / D_ALIGN) - 1;
}

static inline size_t obj_header_size(void){ return round_up(sizeof(ObjHdr), D_ALIGN); }
static inline size_t small_span_header_size(void){ return round_up(sizeof(SmallSpan), D_ALIGN); }

static inline int shard_index(void){
    uintptr_t h = (uintptr_t)tls_tc;
    if (!h) h = (uintptr_t)pthread_self();
    return (int)(((h >> 12) & (CENTRAL_SHARDS - 1)));
}

static void central_init_once(void)
{
    /* init_state: 0=uninitialized, 1=initializing, 2=initialized */
    static atomic_int init_state = ATOMIC_VAR_INIT(0);
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&init_state, &expected, 1,
                                               memory_order_acq_rel, memory_order_acquire)){
        /* we won the initialization */
        for (size_t s = 0; s < CENTRAL_SHARDS; s++){
            for (size_t i = 0; i < (MAX_SMALL / D_ALIGN); i++){
                central[s][i].head = NULL;
                central[s][i].obj_size = (i + 1) * D_ALIGN;
                pthread_mutex_init(&central_lock[s][i], NULL);
            }
        }
        atomic_store_explicit(&init_state, 2, memory_order_release);
        return;
    }
    /* another thread is initializing or already initialized; wait until done */
    while (atomic_load_explicit(&init_state, memory_order_acquire) != 2) {
        /* spin */
    }
}

static void central_grow(int sc, int shard)
{
    size_t ps = pageheap_page_size();
    size_t payload = central[0][sc].obj_size;
    size_t hdr = obj_header_size();
    size_t slot = hdr + payload;
    size_t span_hdr = small_span_header_size();

    /* choose pages so that we have at least TARGET objects */
    size_t npages = 1;
    const size_t TARGET = 512;
    while ((npages * ps) < (span_hdr + slot)) npages++;
    while (((npages * ps) - span_hdr) / slot < TARGET) npages++;

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

    void* chain = NULL;
    for (size_t i = 0; i < capacity; i++){
        uint8_t* p = base + offset + i * slot;
        ObjHdr* h = (ObjHdr*)p;
        h->owner = (void*)ss;
        h->size_class = (uint16_t)sc;
        h->flags = 0;
        void* user = (void*)(p + hdr);
        *(void**)user = chain;
        chain = user;
        ss->total_objs++;
        ss->free_objs++;
    }
    pthread_mutex_lock(&central_lock[shard][sc]);
    while (chain){
        void* u = chain;
        chain = *(void**)u;
        *(void**)u = central[shard][sc].head;
        central[shard][sc].head = u;
    }
    pthread_mutex_unlock(&central_lock[shard][sc]);
}

static size_t central_fetch_batch(int sc, void** out, size_t n)
{
    size_t got = 0;
    int shard = shard_index();
    pthread_mutex_lock(&central_lock[shard][sc]);
    if (!central[shard][sc].head){
        int tries = 0;
        while (!central[shard][sc].head && tries < 3){
            pthread_mutex_unlock(&central_lock[shard][sc]);
            central_grow(sc, shard);
            pthread_mutex_lock(&central_lock[shard][sc]);
            tries++;
        }
        #ifdef DMALLOC_STATS
        atomic_fetch_add_explicit(&stat_fetch_tries[shard][sc], tries, memory_order_relaxed);
        #endif
    }
    while (central[shard][sc].head && got < n){
        void* user = central[shard][sc].head;
        __builtin_prefetch(*(void**)user, 0, 1);
        central[shard][sc].head = *(void**)user;
        ObjHdr* h = (ObjHdr*)((uint8_t*)user - obj_header_size());
        SmallSpan* ss = (SmallSpan*)h->owner;
        if (__builtin_expect(!!ss, 1)) ss->free_objs--;
        out[got++] = user;
    }
    #ifdef DMALLOC_STATS
    if (got) atomic_fetch_add_explicit(&stat_fetch_batches[shard][sc], 1, memory_order_relaxed);
    #endif
    pthread_mutex_unlock(&central_lock[shard][sc]);
    return got;
}

static void central_release_batch(int sc, void** list, size_t n)
{
    int shard = shard_index();
    pthread_mutex_lock(&central_lock[shard][sc]);
    for (size_t i = 0; i < n; i++){
        void* ptr = list[i];
        *(void**)ptr = central[shard][sc].head;
        central[shard][sc].head = ptr;
        ObjHdr* h = (ObjHdr*)((uint8_t*)ptr - obj_header_size());
        SmallSpan* ss = (SmallSpan*)h->owner;
        if (ss) ss->free_objs++;
    }
    pthread_mutex_unlock(&central_lock[shard][sc]);
}


static ThreadCache* tc_get(void)
{
    ThreadCache* tc = tls_tc;
    if (!tc){
        void* mem = mmap(NULL, sizeof(ThreadCache), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) return NULL;
        tc = (ThreadCache*)mem;
        memset(tc, 0, sizeof(ThreadCache));
        size_t pages[LARGE_BUCKET_COUNT] = {4,6,8,9,10,12,16,20,24,32,48,64,96,128,192,256};
        for (int i = 0; i < LARGE_BUCKET_COUNT; i++){
            tc->lbuckets[i].head = NULL;
            tc->lbuckets[i].count = 0;
            tc->lbuckets[i].target = 16;
            tc->lbuckets[i].pages = pages[i];
        }
        tls_tc = tc;
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
        ThreadCache* tc = tc_get();
        if (tc){
            for (int i = 0; i < LARGE_BUCKET_COUNT; i++){
                LargeBucket* lb = &tc->lbuckets[i];
                if (lb->pages == npages && lb->head){
                    ObjHdr* h = (ObjHdr*)lb->head;
                    lb->head = ((ObjHdr*)lb->head)->owner;
                    if (lb->count) lb->count--;
                    h->owner = NULL;
                    h->size_class = npages;
                    h->flags = 3;
                    return (void*)((uint8_t*)h + obj_header_size());
                }
            }
        }
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
        size_t batch = tcache_refill_batch_for_sc(sc);
        void* tmp[ batch ];
        size_t got = central_fetch_batch(sc, (void**)tmp, batch);
        for (size_t i = 0; i < got; i++){
            void* p = ((void**)tmp)[i];
            *(void**)p = list->head;
            list->head = p;
            list->count++;
        }
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
            ThreadCache* tc = tc_get();
            if (tc){
                for (int i = 0; i < LARGE_BUCKET_COUNT; i++){
                    LargeBucket* lb = &tc->lbuckets[i];
                    if (lb->pages == npages && lb->count < lb->target){
                        h->owner = lb->head;
                        lb->head = h;
                        lb->count++;
                        return;
                    }
                }
            }
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
        void* tmp[ batch ];
        size_t n = 0;
        while (list->head && n < batch){
            void* p = list->head;
            list->head = *(void**)p;
            tmp[n++] = p;
        }
        if (n){
            central_release_batch(sc, (void**)tmp, n);
            if (list->count >= n) list->count -= n; else list->count = 0;
        }
    }
    unsigned long c = atomic_fetch_add_explicit(&dfree_counter, 1, memory_order_relaxed) + 1;
    if ((c & 0x7FFFFFFUL) == 0){
        pageheap_madvise_idle_spans(32);
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
        old_payload = central[0][h->size_class].obj_size;
    }
    size_t copy = old_payload < size ? old_payload : size;
    memcpy(n, ptr, copy);
    dfree(ptr);
    return n;
}

void dmalloc_init(void)
{
    central_init_once();
    if (!pageheap_page_size()) pageheap_init();
}

__attribute__((constructor)) static void dmalloc_constructor(void)
{
    dmalloc_init();
}
