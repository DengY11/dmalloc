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
    /* after allocation: spans_in_use increased by 1; mapped_pages non-decreasing */
    assert(s1.mapped_pages >= s0.mapped_pages);
    assert(s1.spans_in_use == s0.spans_in_use + 1);

    dfree(p);
    PageHeapStats s2 = pageheap_stats();
    /* compute pages used by allocation */
    size_t hdr = ((sizeof(ObjHdr) + D_ALIGN - 1) & ~(D_ALIGN - 1));
    size_t need = ((big + hdr + D_ALIGN - 1) & ~(D_ALIGN - 1));
    size_t npages = (need + ps - 1) / ps;
    /* after free: free_pages increased exactly by npages; mapped_pages unchanged */
    assert(s2.free_pages == s1.free_pages + npages);
    assert(s2.mapped_pages == s1.mapped_pages);

    size_t released = pageheap_release_empty_spans(1);
    PageHeapStats s3 = pageheap_stats();
    /* after releasing empty spans: mapped_pages and free_pages decreased by released */
    assert(released > 0);
    assert(s3.mapped_pages + released == s2.mapped_pages);
    assert(s3.free_pages + released == s2.free_pages);

    printf("test_free_release OK\n");
    return 0;
}
