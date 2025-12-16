#include "../include/dmalloc.h"
#include "../include/page_heap.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef void* (*alloc_fn)(size_t);
typedef void  (*free_fn)(void*);

typedef struct {
    alloc_fn A;
    free_fn  F;
    size_t   ptr_count;
    size_t   iters;
    size_t   min_sz;
    size_t   max_sz;
    int      id;
    _Atomic int* go;
} WorkerArg;

static inline uint64_t now_ns(){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void* worker(void* p){
    WorkerArg* arg = (WorkerArg*)p;
    while (atomic_load(arg->go) == 0) {}
    void** arr = (void**)malloc(arg->ptr_count * sizeof(void*));
    uint32_t seed = 12345u ^ (uint32_t)arg->id;
    for (size_t it = 0; it < arg->iters; it++){
        for (size_t i = 0; i < arg->ptr_count; i++){
            seed = seed * 1664525u + 1013904223u;
            size_t sz = (size_t)(seed % (arg->max_sz - arg->min_sz + 1)) + arg->min_sz;
            void* q = arg->A(sz);
            if (!q) q = arg->A(arg->min_sz);
            memset(q, 0xA5, sz);
            arr[i] = q;
        }
        for (size_t i = 0; i < arg->ptr_count; i++){
            arg->F(arr[i]);
        }
    }
    free(arr);
    return NULL;
}

static void* sys_alloc(size_t sz){ return malloc(sz); }
static void  sys_free(void* p){ free(p); }
static void* dm_alloc(size_t sz){ return dmalloc(sz); }
static void  dm_free(void* p){ dfree(p); }

static void run_mt(const char* name, alloc_fn A, free_fn F,
                   int threads, size_t ptr_count, size_t iters,
                   size_t min_sz, size_t max_sz)
{
    pthread_t* th = (pthread_t*)malloc(sizeof(pthread_t) * threads);
    WorkerArg* args = (WorkerArg*)malloc(sizeof(WorkerArg) * threads);
    _Atomic int go = 0;
    for (int i = 0; i < threads; i++){
        args[i].A = A; args[i].F = F; args[i].ptr_count = ptr_count;
        args[i].iters = iters; args[i].min_sz = min_sz; args[i].max_sz = max_sz;
        args[i].id = i; args[i].go = &go;
        pthread_create(&th[i], NULL, worker, &args[i]);
    }
    uint64_t t0 = now_ns();
    atomic_store(&go, 1);
    for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
    uint64_t t1 = now_ns();
    double ms = (double)(t1 - t0) / 1e6;
    double ops = (double)(threads) * (double)(ptr_count * iters) * 2.0;
    printf("%s: threads=%d time=%.2f ms ops=%.0f ops/s=%.0f\n", name, threads, ms, ops, (ops/(ms/1000.0)));
    free(th);
    free(args);
}

int main(){
    pageheap_init();
    int T = 8;
    size_t N = 20000;
    size_t I = 40;
    puts("-- MT small (1..128) --");
    run_mt("libc malloc", sys_alloc, sys_free, T, N, I, 1, 128);
    run_mt("dmalloc", dm_alloc, dm_free, T, N, I, 1, 128);
    puts("-- MT medium (1..1024) --");
    run_mt("libc malloc", sys_alloc, sys_free, T, N, I, 1, 1024);
    run_mt("dmalloc", dm_alloc, dm_free, T, N, I, 1, 1024);
    return 0;
}

