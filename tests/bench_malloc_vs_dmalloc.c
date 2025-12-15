#include "../include/dmalloc.h"
#include "../include/page_heap.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

typedef void* (*alloc_fn)(size_t);
typedef void  (*free_fn)(void*);

static inline uint64_t now_ns(){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void run_workload(const char* name, alloc_fn A, free_fn F,
                         size_t ptr_count, size_t iters,
                         size_t min_sz, size_t max_sz)
{
    void** arr = (void**)malloc(ptr_count * sizeof(void*));
    if (!arr){ fprintf(stderr, "alloc arr failed\n"); return; }
    uint64_t t0 = now_ns();
    size_t total_bytes = 0;

    for (size_t it = 0; it < iters; it++){
        /* allocate */
        for (size_t i = 0; i < ptr_count; i++){
            size_t sz = (rand() % (max_sz - min_sz + 1)) + min_sz;
            arr[i] = A(sz);
            if (!arr[i]){ fprintf(stderr, "%s alloc failed at i=%zu\n", name, i); exit(1); }
            memset(arr[i], 0xA5, sz);
            total_bytes += sz;
        }
        /* random free order */
        for (size_t i = 0; i < ptr_count; i++){
            size_t j = (rand() % (ptr_count - i)) + i;
            void* tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
        }
        for (size_t i = 0; i < ptr_count; i++){
            F(arr[i]);
        }
    }

    uint64_t t1 = now_ns();
    double ms = (double)(t1 - t0) / 1e6;
    double ops = (double)(ptr_count * iters * 2); /* alloc+free */
    printf("%s: time=%.2f ms ops=%.0f ops/s=%.0f MB=%.2f\n",
           name, ms, ops, (ops / (ms/1000.0)), (double)total_bytes / (1024.0*1024.0));
    free(arr);
}

static void* sys_alloc(size_t sz){ return malloc(sz); }
static void  sys_free(void* p){ free(p); }
static void* dm_alloc(size_t sz){ return dmalloc(sz); }
static void  dm_free(void* p){ dfree(p); }

int main(){
    srand(12345);
    pageheap_init();

    size_t N = 50000;
    size_t I = 20; /* total ops ~2M */

    puts("-- Small sizes (1..128) --");
    run_workload("libc malloc", sys_alloc, sys_free, N, I, 1, 128);
    run_workload("dmalloc", dm_alloc, dm_free, N, I, 1, 128);

    puts("-- Medium sizes (1..1024) --");
    run_workload("libc malloc", sys_alloc, sys_free, N, I, 1, 1024);
    run_workload("dmalloc", dm_alloc, dm_free, N, I, 1, 1024);

    size_t ps = pageheap_page_size();
    puts("-- Large sizes (~pages) --");
    run_workload("libc malloc", sys_alloc, sys_free, N/5, I, ps, ps*3);
    run_workload("dmalloc", dm_alloc, dm_free, N/5, I, ps, ps*3);

    /* show dmalloc heap stats */
    PageHeapStats st = pageheap_stats();
    printf("dmalloc stats: page_size=%zu mapped=%zu free=%zu in_use_spans=%zu free_spans=%zu\n",
           st.page_size, st.mapped_pages, st.free_pages, st.spans_in_use, st.spans_free);

    return 0;
}

