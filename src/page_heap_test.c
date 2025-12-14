#include "../include/page_heap.h"
#include <stdio.h>

int main(){
    pageheap_init();
    size_t ps = pageheap_page_size();
    printf("page_size=%zu\n", ps);
    PageHeapStats st0 = pageheap_stats();
    printf("mapped=%zu free=%zu in_use=%zu spans_free=%zu\n", st0.mapped_pages, st0.free_pages, st0.spans_in_use, st0.spans_free);
    Span* s1 = span_alloc(2);
    Span* s2 = span_alloc(10);
    PageHeapStats st1 = pageheap_stats();
    printf("after alloc mapped=%zu free=%zu in_use=%zu spans_free=%zu\n", st1.mapped_pages, st1.free_pages, st1.spans_in_use, st1.spans_free);
    if (!s1 || !s2){
        printf("alloc failed\n");
        return 1;
    }
    span_free(s1);
    span_free(s2);
    PageHeapStats st2 = pageheap_stats();
    printf("after free mapped=%zu free=%zu in_use=%zu spans_free=%zu\n", st2.mapped_pages, st2.free_pages, st2.spans_in_use, st2.spans_free);
    return 0;
}

