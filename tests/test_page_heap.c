#include "../include/page_heap.h"
#include <assert.h>
#include <stdio.h>

int main() {
    pageheap_init();
    PageHeapStats st = pageheap_stats();
    assert(st.mapped_pages == 0);
    assert(st.free_pages == 0);

    // grow a free span of 64 pages
    assert(pageheap_grow(64) == 0);
    st = pageheap_stats();
    assert(st.mapped_pages == 64);
    assert(st.free_pages == 64);
    assert(st.spans_free == 1);

    // allocate and free several spans to exercise split and coalesce
    Span* a = span_alloc(10);
    Span* b = span_alloc(20);
    Span* c = span_alloc(5);
    assert(a && b && c);
    st = pageheap_stats();
    assert(st.spans_in_use == 3);

    span_free(b);
    span_free(a);
    span_free(c);
    st = pageheap_stats();
    // After freeing and coalescing, expect one free span of 64 pages
    assert(st.spans_in_use == 0);
    assert(st.free_pages == 64);
    assert(st.spans_free == 1);

    // hard release: free spans >= 64 pages should be munmap'ed
    size_t released = pageheap_release_empty_spans(64);
    st = pageheap_stats();
    assert(released == 64);
    assert(st.mapped_pages == 0);
    assert(st.free_pages == 0);
    assert(st.spans_free == 0);

    // grow again and soft reclaim
    assert(pageheap_grow(32) == 0);
    st = pageheap_stats();
    assert(st.free_pages == 32);
    size_t advised = pageheap_madvise_idle_spans(16);
    assert(advised == 32);
    // madvise does not change stats
    PageHeapStats st2 = pageheap_stats();
    assert(st2.free_pages == 32);
    assert(st2.mapped_pages == st.mapped_pages);

    printf("test_page_heap OK\n");
    return 0;
}

