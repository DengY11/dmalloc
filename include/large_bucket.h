#ifndef LARGE_BUCKET_H
#define LARGE_BUCKET_H

#include "page_heap.h"

/* initialize skiplist for large bucket */
void large_bucket_init(PageHeap* heap);

/* insert/remove span into/from large bucket skiplist */
void large_bucket_insert(PageHeap* heap, Span* s);
void large_bucket_remove(PageHeap* heap, Span* s);

/* find the first span with page_count >= need (best-fit by size) */
Span* large_bucket_lower_bound(PageHeap* heap, size_t need);

#endif /* LARGE_BUCKET_H */

