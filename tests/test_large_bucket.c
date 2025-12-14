#include "../include/page_heap.h"
#include <assert.h>
#include <stdio.h>

int main(){
    pageheap_init();
    // create two large free spans (go to last bucket / skiplist)
    assert(pageheap_grow(128) == 0);
    assert(pageheap_grow(96) == 0);
    PageHeapStats st = pageheap_stats();
    assert(st.free_pages == 224);
    assert(st.spans_free >= 2);

    // allocate 100 pages: should come from 128 via skiplist lower_bound
    Span* s1 = span_alloc(100);
    assert(s1);
    st = pageheap_stats();
    assert(st.spans_in_use == 1);
    // free pages reduce by exactly 100
    assert(st.free_pages == 124);

    // allocate 96 pages: should use the remaining 96 large span
    Span* s2 = span_alloc(96);
    assert(s2);
    st = pageheap_stats();
    assert(st.spans_in_use == 2);
    assert(st.free_pages == 28);

    // free both, then release empty spans >= 64
    span_free(s1);
    span_free(s2);
    st = pageheap_stats();
    assert(st.spans_in_use == 0);
    assert(st.free_pages == 224);
    size_t released = pageheap_release_empty_spans(64);
    assert(released == 224);
    st = pageheap_stats();
    assert(st.mapped_pages == 0);
    assert(st.free_pages == 0);

    printf("test_large_bucket OK\n");
    return 0;
}

