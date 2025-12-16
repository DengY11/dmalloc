#include "../include/dmalloc.h"
#include "../include/page_heap.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(){
    pageheap_init();
    PageHeapStats s0 = pageheap_stats();

    size_t ps = pageheap_page_size();
    size_t big = ps * 8 + 123; /* large object spanning multiple pages */
    void* p = dmalloc(big);
    assert(p);
    memset(p, 0xAA, big);

    PageHeapStats s1 = pageheap_stats();
    /* direct map path does not touch page heap stats */
    assert(s1.page_size == s0.page_size);
    assert(s1.mapped_pages == s0.mapped_pages);
    assert(s1.free_pages == s0.free_pages);
    assert(s1.spans_in_use == s0.spans_in_use);

    dfree(p);
    PageHeapStats s2 = pageheap_stats();
    /* after direct free: page heap stats unchanged */
    assert(s2.page_size == s1.page_size);
    assert(s2.mapped_pages == s1.mapped_pages);
    assert(s2.free_pages == s1.free_pages);
    assert(s2.spans_in_use == s1.spans_in_use);

    size_t released = pageheap_release_empty_spans(1);
    PageHeapStats s3 = pageheap_stats();
    /* direct map path: no empty spans to release from page heap */
    assert(released == 0);
    assert(s3.mapped_pages == s2.mapped_pages);
    assert(s3.free_pages == s2.free_pages);

    printf("test_free_release OK\n");
    return 0;
}
