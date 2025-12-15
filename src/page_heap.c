#include "../include/page_heap.h"
#include "../include/large_bucket.h"
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>

static PageHeap page_heap;
static pthread_once_t pageheap_once = PTHREAD_ONCE_INIT;


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
    pthread_mutex_lock(&page_heap.meta_lock);
    if (!meta_free_list){
        if (!meta_chunk_new(META_CHUNK_NEW_SIZE)){ pthread_mutex_unlock(&page_heap.meta_lock); return NULL; }
    }
    Span* s = meta_free_list;
    meta_free_list = meta_free_list->next_free_addr;
    memset(s, 0, sizeof(Span));
    pthread_mutex_unlock(&page_heap.meta_lock);
    return s;
}

/*put a span back to meta_free_list*/
/*return a Span node back to metadata pool*/
static void meta_release(Span* s)
{
    pthread_mutex_lock(&page_heap.meta_lock);
    s->next_free_addr = meta_free_list;
    meta_free_list = s;
    pthread_mutex_unlock(&page_heap.meta_lock);
}

/*push free span into its size bucket list*/
static void bucket_insert(Span* s)
{
    size_t idx = bucket_index(s->page_count);
    if (is_large_bucket_idx(idx)){
        large_bucket_insert(&page_heap, s);
    }else{
        pthread_mutex_lock(&page_heap.bucket_lock[idx]);
        s->next_free_addr = page_heap.free_buckets[idx];
        page_heap.free_buckets[idx] = s;
        pthread_mutex_unlock(&page_heap.bucket_lock[idx]);
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
    pthread_mutex_lock(&page_heap.bucket_lock[idx]);
    Span* cur = page_heap.free_buckets[idx];
    Span* prev = NULL;
    while (cur){
        if (cur == s){
            if (prev) prev->next_free_addr = cur->next_free_addr;
            else page_heap.free_buckets[idx] = cur->next_free_addr;
            s->next_free_addr = NULL;
            pthread_mutex_unlock(&page_heap.bucket_lock[idx]);
            return;
        }
        prev = cur;
        cur = cur->next_free_addr;
    }
    pthread_mutex_unlock(&page_heap.bucket_lock[idx]);
}

/*insert span into address-sorted doubly-linked list*/
static void addr_insert_sorted(Span* s)
{
    pthread_mutex_lock(&page_heap.addr_lock);
    if (!page_heap.addr_head){
        page_heap.addr_head = s;
        s->prev_addr = NULL;
        s->next_addr = NULL;
        pthread_mutex_unlock(&page_heap.addr_lock);
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
    pthread_mutex_unlock(&page_heap.addr_lock);
}

/*remove span from address-sorted doubly-linked list*/
static void addr_remove(Span* s)
{
    pthread_mutex_lock(&page_heap.addr_lock);
    if (s->prev_addr) s->prev_addr->next_addr = s->next_addr;
    else page_heap.addr_head = s->next_addr;
    if (s->next_addr) s->next_addr->prev_addr = s->prev_addr;
    s->prev_addr = NULL;
    s->next_addr = NULL;
    pthread_mutex_unlock(&page_heap.addr_lock);
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
    pthread_mutex_lock(&page_heap.addr_lock);
    Span* left = s->prev_addr;
    if (can_coalesce(left, s)){
        bucket_remove(left);
        left->page_count += s->page_count;
        left->next_addr = s->next_addr;
        if (s->next_addr) s->next_addr->prev_addr = left;
        meta_release(s);
        s = left;
        pthread_mutex_lock(&page_heap.stats_lock);
        page_heap.spans_free -= 1;
        pthread_mutex_unlock(&page_heap.stats_lock);
    }
    Span* right = s->next_addr;
    if (can_coalesce(s, right)){
        bucket_remove(right);
        s->page_count += right->page_count;
        s->next_addr = right->next_addr;
        if (right->next_addr) right->next_addr->prev_addr = s;
        meta_release(right);
        pthread_mutex_lock(&page_heap.stats_lock);
        page_heap.spans_free -= 1;
        pthread_mutex_unlock(&page_heap.stats_lock);
    }
    bucket_insert(s);
    pthread_mutex_unlock(&page_heap.addr_lock);
}

/*initialize page heap state and metadata pool*/
void pageheap_init(void)
{
    memset(&page_heap, 0, sizeof(page_heap));
    page_heap.page_size = (size_t)sysconf(_SC_PAGESIZE);
    meta_free_list = NULL;
    pthread_mutex_init(&page_heap.addr_lock, NULL);
    pthread_mutex_init(&page_heap.skiplist_lock, NULL);
    pthread_mutex_init(&page_heap.meta_lock, NULL);
    pthread_mutex_init(&page_heap.stats_lock, NULL);
    for (size_t i = 0; i < MAX_BUCKETS; i++){
        pthread_mutex_init(&page_heap.bucket_lock[i], NULL);
    }
    /* initialize large bucket skiplist */
    large_bucket_init(&page_heap);
}

void pageheap_ensure_init(void)
{
    pthread_once(&pageheap_once, pageheap_init);
}

/*return page size for external queries*/
size_t pageheap_page_size(void)
{
    pageheap_ensure_init();
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
int pageheap_grow(size_t page_count)
{
    pageheap_ensure_init();
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
    pthread_mutex_lock(&page_heap.stats_lock);
    page_heap.mapped_pages += page_count;
    page_heap.free_pages += page_count;
    page_heap.spans_free += 1;
    pthread_mutex_unlock(&page_heap.stats_lock);
    return 0;
}

/*search buckets for span with >= requested pages*/
static Span* select_span(size_t page_count) {
    size_t idx = bucket_index(page_count);
    if (idx < MAX_BUCKETS - 1) {
        for (size_t i = idx; i < MAX_BUCKETS - 1; i++){
            pthread_mutex_lock(&page_heap.bucket_lock[i]);
            Span* h = page_heap.free_buckets[i];
            if (h){
                page_heap.free_buckets[i] = h->next_free_addr;
                h->next_free_addr = NULL;
                pthread_mutex_unlock(&page_heap.bucket_lock[i]);
                return h;
            }
            pthread_mutex_unlock(&page_heap.bucket_lock[i]);
        }
    }
    pthread_mutex_lock(&page_heap.skiplist_lock);
    Span* head = page_heap.large_skip_head;
    if (!head){
        pthread_mutex_unlock(&page_heap.skiplist_lock);
        return NULL;
    }
    Span* update[MAX_SKIP_LEVELS];
    Span* x = head;
    for (int i = MAX_SKIP_LEVELS - 1; i >= 0; i--){
        while (x->skip_next[i] && x->skip_next[i]->page_count < page_count) x = x->skip_next[i];
        update[i] = x;
    }
    Span* cand = x->skip_next[0];
    if (!cand){
        pthread_mutex_unlock(&page_heap.skiplist_lock);
        return NULL;
    }
    for (int i = 0; i < (int)cand->skip_level; i++){
        if (update[i]->skip_next[i] == cand) update[i]->skip_next[i] = cand->skip_next[i];
    }
    for (int i = 0; i < MAX_SKIP_LEVELS; i++) cand->skip_next[i] = NULL;
    cand->skip_level = 0;
    pthread_mutex_unlock(&page_heap.skiplist_lock);
    return cand;
}

/*allocate a span; split if larger; grow if needed*/
Span* span_alloc(size_t page_count)
{
    pageheap_ensure_init();
    if (!page_count) return NULL;
    Span* s = select_span(page_count);
    if (!s){
        size_t grow = page_count < DEFAULT_GROW_PAGES ? DEFAULT_GROW_PAGES : page_count;
        if (pageheap_grow(grow) != 0) return NULL;
        s = select_span(page_count);
        if (!s) return NULL;
    }
    if (s->page_count == page_count){
        s->in_use = 1;
        pthread_mutex_lock(&page_heap.stats_lock);
        page_heap.free_pages -= s->page_count;
        page_heap.spans_free -= 1;
        page_heap.spans_in_use += 1;
        pthread_mutex_unlock(&page_heap.stats_lock);
        return s;
    }
    size_t remain = s->page_count - page_count;
    void* remain_start = (void*)((uintptr_t)s->start + page_count * psize());
    s->page_count = page_count;
    s->in_use = 1;
    Span* r = span_create(remain_start, 0);
    if (!r) return NULL;
    r->page_count = remain;
    pthread_mutex_lock(&page_heap.addr_lock);
    r->next_addr = s->next_addr;
    r->prev_addr = s;
    if (s->next_addr) s->next_addr->prev_addr = r;
    s->next_addr = r;
    bucket_insert(r);
    pthread_mutex_lock(&page_heap.stats_lock);
    page_heap.spans_in_use += 1;
    page_heap.free_pages -= page_count;
    pthread_mutex_unlock(&page_heap.stats_lock);
    pthread_mutex_unlock(&page_heap.addr_lock);
    return s;
}

/*mark span free, update stats, and coalesce neighbors*/
void span_free(Span* s)
{
    if (!s) return;
    if (!s->in_use) return;
    s->in_use = 0;
    pthread_mutex_lock(&page_heap.stats_lock);
    page_heap.spans_in_use -= 1;
    page_heap.free_pages += s->page_count;
    page_heap.spans_free += 1;
    pthread_mutex_unlock(&page_heap.stats_lock);
    coalesce_neighbors(s);
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
    pageheap_ensure_init();
    pthread_mutex_lock(&page_heap.stats_lock);
    st.page_size = page_heap.page_size;
    st.mapped_pages = page_heap.mapped_pages;
    st.free_pages = page_heap.free_pages;
    st.spans_in_use = page_heap.spans_in_use;
    st.spans_free = page_heap.spans_free;
    pthread_mutex_unlock(&page_heap.stats_lock);
    return st;
}

/* find the Span that contains the given address */
Span* pageheap_span_for_addr(void* addr)
{
    if (!addr) return NULL;
    uintptr_t a = (uintptr_t)addr;
    pthread_mutex_lock(&page_heap.addr_lock);
    Span* cur = page_heap.addr_head;
    size_t ps = psize();
    while (cur){
        uintptr_t s = (uintptr_t)cur->start;
        uintptr_t e = s + cur->page_count * ps;
        if (a >= s && a < e){
            pthread_mutex_unlock(&page_heap.addr_lock);
            return cur;
        }
        /* address-sorted list allows early break */
        if (a < s) break;
        cur = cur->next_addr;
    }
    pthread_mutex_unlock(&page_heap.addr_lock);
    return NULL;
}

/*release fully free spans with page_count >= min_pages using munmap; returns released pages*/
size_t pageheap_release_empty_spans(size_t min_pages)
{
    pageheap_ensure_init();
    if (min_pages == 0) min_pages = 1;
    size_t released_pages = 0;
    pthread_mutex_lock(&page_heap.addr_lock);
    Span* cur = page_heap.addr_head;
    while (cur){
        Span* next = cur->next_addr; /* save next since cur may be removed */
        if (!cur->in_use && cur->page_count >= min_pages){
            size_t bytes = cur->page_count * psize();
            /* unlink from bucket and addr list under addr_lock to keep lock order */
            bucket_remove(cur);
            if (cur->prev_addr) cur->prev_addr->next_addr = cur->next_addr;
            else page_heap.addr_head = cur->next_addr;
            if (cur->next_addr) cur->next_addr->prev_addr = cur->prev_addr;
            cur->prev_addr = NULL;
            cur->next_addr = NULL;
            pthread_mutex_unlock(&page_heap.addr_lock);
            if (munmap(cur->start, bytes) == 0){
                pthread_mutex_lock(&page_heap.stats_lock);
                page_heap.mapped_pages -= cur->page_count;
                page_heap.free_pages   -= cur->page_count;
                page_heap.spans_free   -= 1;
                pthread_mutex_unlock(&page_heap.stats_lock);
                released_pages         += cur->page_count;
                meta_release(cur);
            } else {
                /* rollback: reinsert into lists if munmap failed */
                pthread_mutex_lock(&page_heap.addr_lock);
                if (page_heap.addr_head){
                    cur->next_addr = page_heap.addr_head;
                    page_heap.addr_head->prev_addr = cur;
                    page_heap.addr_head = cur;
                } else {
                    page_heap.addr_head = cur;
                }
                pthread_mutex_unlock(&page_heap.addr_lock);
                bucket_insert(cur);
            }
            pthread_mutex_lock(&page_heap.addr_lock);
        }
        cur = next;
    }
    pthread_mutex_unlock(&page_heap.addr_lock);
    return released_pages;
}

/*soft reclaim free spans with page_count >= min_pages using madvise(DONTNEED); returns advised pages*/
size_t pageheap_madvise_idle_spans(size_t min_pages)
{
    pageheap_ensure_init();
    if (min_pages == 0) min_pages = 1;
    size_t advised_pages = 0;
    Span* cur = page_heap.addr_head;
    while (cur){
        Span* next = cur->next_addr;
        if (!cur->in_use && cur->page_count >= min_pages){
            size_t bytes = cur->page_count * psize();
            /* ignore errors; madvise may fail on some OS */
            (void)madvise(cur->start, bytes, MADV_DONTNEED);
            advised_pages += cur->page_count;
        }
        cur = next;
    }
    return advised_pages;
}
