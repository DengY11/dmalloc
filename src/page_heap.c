#include "../include/page_heap.h"
#include "../include/large_bucket.h"
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

static PageHeap page_heap;
static pthread_mutex_t page_heap_mutex;


/*get current page size in bytes*/
static inline size_t psize(void)
{
    return page_heap.page_size;
}

/* a bucket is a list of free spans which was divided by page_count.
 * spans with same page_count are put in the same bucket.
*/
static inline size_t bucket_index(size_t page_count)
{
    if (!page_count) return 0;
    if (page_count >= MAX_BUCKETS) return MAX_BUCKETS - 1;/*when page_count is too large, 
                                                            put it in the last bucket*/
    return page_count - 1;
}

static inline int is_large_bucket_idx(size_t idx){ return idx == (MAX_BUCKETS - 1); }

static Span* meta_free_list;

/*use mmap to alloc a chunk of memory and cut it into n spans*/
static void* meta_chunk_new(size_t n)
{
    size_t sz = n * sizeof(Span);
    void* p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    char* it = (char*)p;
    for (size_t i = 0; i < n; i++){
        Span* s = (Span*)(it + i * sizeof(Span));
        s->next_free_addr = meta_free_list;
        meta_free_list = s;
    }
    return p;
}

/*get a usable span from meta_free_list*/
static Span* meta_acquire(void)
{
    if (!meta_free_list){
        if (!meta_chunk_new(META_CHUNK_NEW_SIZE)) return NULL;
    }
    Span* s = meta_free_list;
    meta_free_list = meta_free_list->next_free_addr;
    memset(s, 0, sizeof(Span));
    return s;
}

/*put a span back to meta_free_list*/
/*return a Span node back to metadata pool*/
static void meta_release(Span* s)
{
    s->next_free_addr = meta_free_list;
    meta_free_list = s;
}

/*push free span into its size bucket list*/
static void bucket_insert(Span* s)
{
    size_t idx = bucket_index(s->page_count);
    if (is_large_bucket_idx(idx)){
        large_bucket_insert(&page_heap, s);
    }else{
        s->next_free_addr = page_heap.free_buckets[idx];
        page_heap.free_buckets[idx] = s;
    }
}

/*remove a specific span from its size bucket list*/
static void bucket_remove(Span* s)
{
    size_t idx = bucket_index(s->page_count);
    if (is_large_bucket_idx(idx)){
        large_bucket_remove(&page_heap, s);
        return;
    }
    Span* cur = page_heap.free_buckets[idx];
    Span* prev = NULL;
    while (cur){
        if (cur == s){
            if (prev) prev->next_free_addr = cur->next_free_addr;
            else page_heap.free_buckets[idx] = cur->next_free_addr;
            s->next_free_addr = NULL;
            return;
        }
        prev = cur;
        cur = cur->next_free_addr;
    }
}

/*insert span into address-sorted doubly-linked list*/
static void addr_insert_sorted(Span* s)
{
    if (!page_heap.addr_head){
        page_heap.addr_head = s;
        s->prev_addr = NULL;
        s->next_addr = NULL;
        return;
    }
    Span* cur = page_heap.addr_head;
    Span* prev = NULL;
    uintptr_t addr = (uintptr_t)s->start;
    while (cur && (uintptr_t)cur->start < addr){
        prev = cur;
        cur = cur->next_addr;
    }
    s->prev_addr = prev;
    s->next_addr = cur;
    if (prev) prev->next_addr = s; else page_heap.addr_head = s;
    if (cur) cur->prev_addr = s;
}

/*remove span from address-sorted doubly-linked list*/
static void addr_remove(Span* s)
{
    if (s->prev_addr) s->prev_addr->next_addr = s->next_addr;
    else page_heap.addr_head = s->next_addr;
    if (s->next_addr) s->next_addr->prev_addr = s->prev_addr;
    s->prev_addr = NULL;
    s->next_addr = NULL;
}

/*check if two free spans are adjacent and can merge*/
static int can_coalesce(Span* a, Span* b)
{
    if (!a || !b) return 0;
    if (a->in_use || b->in_use) return 0;
    uintptr_t a_end = (uintptr_t)a->start + a->page_count * psize();
    return a_end == (uintptr_t)b->start;
}

/*merge with left/right free neighbors and reinsert into bucket*/
static void coalesce_neighbors(Span* s)
{
    Span* left = s->prev_addr;
    if (can_coalesce(left, s)){
        bucket_remove(left);
        left->page_count += s->page_count;
        left->next_addr = s->next_addr;
        if (s->next_addr) s->next_addr->prev_addr = left;
        meta_release(s);
        s = left;
        page_heap.spans_free -= 1;
    }
    Span* right = s->next_addr;
    if (can_coalesce(s, right)){
        bucket_remove(right);
        s->page_count += right->page_count;
        s->next_addr = right->next_addr;
        if (right->next_addr) right->next_addr->prev_addr = s;
        meta_release(right);
        page_heap.spans_free -= 1;
    }
    bucket_insert(s);
}

/*initialize page heap state and metadata pool*/
void pageheap_init(void)
{
    memset(&page_heap, 0, sizeof(page_heap));
    page_heap.page_size = (size_t)sysconf(_SC_PAGESIZE);
    meta_free_list = NULL;
    /* initialize large bucket skiplist */
    large_bucket_init(&page_heap);
    pthread_mutex_init(&page_heap_mutex, NULL);
}

/*return page size for external queries*/
size_t pageheap_page_size(void)
{
    return psize();
}

/*acquire a blank Span metadata and set basic fields*/
static Span* span_create(void* start, size_t in_use)
{
    Span* s = meta_acquire();
    if (!s) return NULL;
    s->start = start;
    s->in_use = in_use;
    return s;
}

/*map more pages from OS and publish one free span*/
static int pageheap_grow_nolock(size_t page_count)
{
    if (!page_heap.page_size) pageheap_init();
    if (!page_count) page_count = DEFAULT_GROW_PAGES;
    size_t bytes = page_count * psize();
    void* p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
    Span* s = span_create(p, 0);
    if (!s){
        munmap(p, bytes);
        return -1;
    }
    s->page_count = page_count;
    addr_insert_sorted(s);
    bucket_insert(s);
    page_heap.mapped_pages += page_count;
    page_heap.free_pages += page_count;
    page_heap.spans_free += 1;
    return 0;
}

int pageheap_grow(size_t page_count)
{
    if (!page_heap.page_size) pageheap_init();
    pthread_mutex_lock(&page_heap_mutex);
    int r = pageheap_grow_nolock(page_count);
    pthread_mutex_unlock(&page_heap_mutex);
    return r;
}

/*search buckets for span with >= requested pages*/
static Span* find_suitable(size_t page_count) {
    size_t idx = bucket_index(page_count);
    if (idx < MAX_BUCKETS - 1) {
        Span* head = page_heap.free_buckets[idx];
        if (head) return head; /*in this bucket, all spans have same page_count*/
        /*not found in this bucket, try larger buckets*/
        for (size_t i = idx + 1; i < MAX_BUCKETS - 1; i++) {
            Span* h = page_heap.free_buckets[i];
            if (h) return h; 
        }
    }
    /* last bucket: use skiplist lower_bound */
    return large_bucket_lower_bound(&page_heap, page_count);
}

/*allocate a span; split if larger; grow if needed*/
Span* span_alloc(size_t page_count)
{
    if (!page_heap.page_size) pageheap_init();
    if (!page_count) return NULL;
    pthread_mutex_lock(&page_heap_mutex);
    Span* s = find_suitable(page_count);
    if (!s){
        size_t grow = page_count;
        if (page_count < DEFAULT_GROW_PAGES && page_count < 32) grow = DEFAULT_GROW_PAGES;
        if (pageheap_grow_nolock(grow) != 0){ pthread_mutex_unlock(&page_heap_mutex); return NULL; }
        s = find_suitable(page_count);
        if (!s){ pthread_mutex_unlock(&page_heap_mutex); return NULL; }
    }
    bucket_remove(s);
    if (s->page_count == page_count){
        s->in_use = 1;
        page_heap.free_pages -= s->page_count;
        page_heap.spans_free -= 1;
        page_heap.spans_in_use += 1;
        pthread_mutex_unlock(&page_heap_mutex);
        return s;
    }
    size_t remain = s->page_count - page_count;
    void* remain_start = (void*)((uintptr_t)s->start + page_count * psize());
    s->page_count = page_count;
    s->in_use = 1;
    Span* r = span_create(remain_start, 0);
    if (!r){ pthread_mutex_unlock(&page_heap_mutex); return NULL; }
    r->page_count = remain;
    r->next_addr = s->next_addr;
    r->prev_addr = s;
    if (s->next_addr) s->next_addr->prev_addr = r;
    s->next_addr = r;
    bucket_insert(r);
    page_heap.spans_in_use += 1;
    page_heap.free_pages -= page_count;
    pthread_mutex_unlock(&page_heap_mutex);
    return s;
}

/*mark span free, update stats, and coalesce neighbors*/
void span_free(Span* s)
{
    if (!s) return;
    if (!s->in_use) return;
    pthread_mutex_lock(&page_heap_mutex);
    s->in_use = 0;
    page_heap.spans_in_use -= 1;
    page_heap.free_pages += s->page_count;
    page_heap.spans_free += 1;
    coalesce_neighbors(s);
    pthread_mutex_unlock(&page_heap_mutex);
}

/*get start address of span*/
void* span_ptr(Span* span)
{
    if (!span) return NULL;
    return span->start;
}

/*get number of pages in span*/
size_t span_page_count(Span* span)
{
    if (!span) return 0;
    return span->page_count;
}

/*collect current statistics snapshot*/
PageHeapStats pageheap_stats(void)
{
    PageHeapStats st;
    st.page_size = page_heap.page_size;
    st.mapped_pages = page_heap.mapped_pages;
    st.free_pages = page_heap.free_pages;
    st.spans_in_use = page_heap.spans_in_use;
    st.spans_free = page_heap.spans_free;
    return st;
}

/*release fully free spans with page_count >= min_pages using munmap; returns released pages*/
size_t pageheap_release_empty_spans(size_t min_pages)
{
    if (!page_heap.page_size) pageheap_init();
    if (min_pages == 0) min_pages = 1;
    size_t released_pages = 0;
    typedef struct { void* addr; size_t bytes; } Rel;
    Rel* rels = NULL; size_t cap = 0, n = 0;
    pthread_mutex_lock(&page_heap_mutex);
    Span* cur = page_heap.addr_head;
    while (cur){
        Span* next = cur->next_addr; /* save next since cur may be removed */
        if (!cur->in_use && cur->page_count >= min_pages){
            size_t bytes = cur->page_count * psize();
            /* unlink from bucket and addr list */
            bucket_remove(cur);
            addr_remove(cur);
            /* update stats */
            page_heap.mapped_pages -= cur->page_count;
            page_heap.free_pages   -= cur->page_count;
            page_heap.spans_free   -= 1;
            released_pages         += cur->page_count;
            /* record for system call outside lock */
            if (n == cap){
                size_t newcap = cap ? cap * 2 : 16;
                Rel* tmp = (Rel*)realloc(rels, newcap * sizeof(Rel));
                if (tmp){ rels = tmp; cap = newcap; }
            }
            if (n < cap){ rels[n].addr = cur->start; rels[n].bytes = bytes; n++; }
            else {
                /* fallback: perform munmap while holding lock if allocation failed */
                (void)munmap(cur->start, bytes);
            }
            /* recycle metadata */
            meta_release(cur);
        }
        cur = next;
    }
    pthread_mutex_unlock(&page_heap_mutex);
    /* perform system calls outside lock */
    for (size_t i = 0; i < n; i++) (void)munmap(rels[i].addr, rels[i].bytes);
    free(rels);
    return released_pages;
}

/*soft reclaim free spans with page_count >= min_pages using madvise(DONTNEED); returns advised pages*/
size_t pageheap_madvise_idle_spans(size_t min_pages)
{
    if (!page_heap.page_size) pageheap_init();
    if (min_pages == 0) min_pages = 1;
    size_t advised_pages = 0;
    typedef struct { void* addr; size_t bytes; } Adv;
    Adv* advs = NULL; size_t cap = 0, n = 0;
    pthread_mutex_lock(&page_heap_mutex);
    Span* cur = page_heap.addr_head;
    while (cur){
        Span* next = cur->next_addr;
        if (!cur->in_use && cur->page_count >= min_pages){
            size_t bytes = cur->page_count * psize();
            advised_pages += cur->page_count;
            if (n == cap){
                size_t newcap = cap ? cap * 2 : 16;
                Adv* tmp = (Adv*)realloc(advs, newcap * sizeof(Adv));
                if (tmp){ advs = tmp; cap = newcap; }
            }
            if (n < cap){ advs[n].addr = cur->start; advs[n].bytes = bytes; n++; }
            else {
                /* fallback: perform madvise while holding lock */
                (void)madvise(cur->start, bytes, MADV_DONTNEED);
            }
        }
        cur = next;
    }
    pthread_mutex_unlock(&page_heap_mutex);
    for (size_t i = 0; i < n; i++) (void)madvise(advs[i].addr, advs[i].bytes, MADV_DONTNEED);
    free(advs);
    return advised_pages;
}
