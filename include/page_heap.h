#ifndef PAGE_HEAP_H
#define PAGE_HEAP_H
#include <stddef.h>
#include <stdint.h>

#define MAX_BUCKETS (64)
#define DEFAULT_GROW_PAGES (64)
#define MAX_SKIP_LEVELS (16)
#define META_CHUNK_NEW_SIZE (1024)

typedef struct _Span{
    void *start;
    size_t page_count;
    size_t in_use;
    struct _Span* next_addr;
    struct _Span* prev_addr;
    struct _Span* next_free_addr;

    /* skiplist fields for large bucket */
    struct _Span* skip_next[MAX_SKIP_LEVELS];
    unsigned char skip_level;
} Span;

typedef struct _PageHeap{
    size_t page_size;
    Span* free_buckets[MAX_BUCKETS];
    Span* addr_head;
    /* skiplist head for large bucket */
    Span* large_skip_head;
    size_t mapped_pages;
    size_t free_pages;
    size_t spans_in_use;
    size_t spans_free;
} PageHeap;

typedef struct _PageHeapStats {
    size_t page_size;
    size_t mapped_pages;
    size_t free_pages;
    size_t spans_in_use;
    size_t spans_free;
} PageHeapStats;

void pageheap_init(void);
size_t pageheap_page_size(void);

/*alloc at least page_count pages*/
Span* span_alloc(size_t page_count);
void span_free(Span* span);

/*increase page heap capacity from OS*/
int pageheap_grow(size_t page_count);

/*read Span metadata*/
void* span_ptr(Span* span);
size_t span_page_count(Span* span);

PageHeapStats pageheap_stats(void);



#endif
