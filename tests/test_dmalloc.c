#include "../include/dmalloc.h"
#include "../include/page_heap.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(){
    pageheap_init();
    /* small allocations across classes */
    const int N = 2000;
    void* ptrs[N];
    for (int i = 0; i < N; i++){
        size_t sz = (i % 120) + 1; /* 1..120 */
        ptrs[i] = dmalloc(sz);
        assert(ptrs[i]);
        memset(ptrs[i], 0xAB, sz);
    }
    /* reallocate some */
    for (int i = 0; i < N; i += 7){
        size_t ns = ((i % 200) + 50);
        void* p2 = drealloc(ptrs[i], ns);
        assert(p2);
        ptrs[i] = p2;
    }
    /* free all */
    for (int i = 0; i < N; i++) dfree(ptrs[i]);

    /* large alloc path */
    size_t big = pageheap_page_size() * 3 + 123;
    void* L = dmalloc(big);
    assert(L);
    memset(L, 0xCD, big);
    dfree(L);

    printf("test_dmalloc OK\n");
    return 0;
}

