#include "../include/dmalloc.h"
#include "../include/page_heap.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

typedef void* (*alloc_fn)(size_t);
typedef void  (*free_fn)(void*);

static void* sys_alloc(size_t n){ return malloc(n); }
static void  sys_free(void* p){ free(p); }
static void* dm_alloc(size_t n){ return dmalloc(n); }
static void  dm_free(void* p){ dfree(p); }

static double now_ms(){ struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec*1000.0 + tv.tv_usec/1000.0; }

static double run_once(alloc_fn af, free_fn ff, size_t sz, int count){
    void** arr = (void**)malloc(sizeof(void*) * count);
    double t0 = now_ms();
    for (int i = 0; i < count; i++){ arr[i] = af(sz); if (!arr[i]) { fprintf(stderr, "alloc failed\n"); exit(1);} memset(arr[i], 0x5A, sz < 64 ? sz : 64); }
    for (int i = 0; i < count; i++){ ff(arr[i]); }
    double t1 = now_ms();
    free(arr);
    return t1 - t0;
}

typedef struct { alloc_fn af; free_fn ff; size_t sz; int count; double ms; } Arg;

static void* worker(void* p){ Arg* a = (Arg*)p; a->ms = run_once(a->af, a->ff, a->sz, a->count); return NULL; }

static void bench_st(const char* name, alloc_fn af, free_fn ff, size_t sz){
    size_t cap = 512ULL * 1024ULL * 1024ULL;
    int count = (int)(cap / (sz ? sz : 1));
    if (count > 200000) count = 200000;
    if (count < 2000) count = 2000;
    double ms = run_once(af, ff, sz, count);
    double ops = (double)count / (ms/1000.0);
    printf("%s ST size=%zu count=%d time=%.2fms ops/s=%.0f\n", name, sz, count, ms, ops);
}

static void bench_mt(const char* name, alloc_fn af, free_fn ff, size_t sz){
    int threads = 8;
    size_t cap = 256ULL * 1024ULL * 1024ULL;
    int count = (int)(cap / (sz ? sz : 1));
    if (count > 80000) count = 80000;
    if (count < 1000) count = 1000;
    pthread_t th[threads];
    Arg args[threads];
    double sum = 0.0;
    for (int i = 0; i < threads; i++){ args[i].af = af; args[i].ff = ff; args[i].sz = sz; args[i].count = count; args[i].ms = 0.0; }
    double t0 = now_ms();
    for (int i = 0; i < threads; i++) pthread_create(&th[i], NULL, worker, &args[i]);
    for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
    double t1 = now_ms();
    for (int i = 0; i < threads; i++) sum += args[i].ms;
    double wall = t1 - t0;
    double total_ops = (double)threads * (double)count;
    double ops_wall = total_ops / (wall/1000.0);
    printf("%s MT size=%zu threads=%d each=%d wall=%.2fms ops/s=%.0f\n", name, sz, threads, count, wall, ops_wall);
}

int main(){
    pageheap_init();
    size_t ps = pageheap_page_size();
    size_t small = 32;
    size_t medium = 512;
    size_t large = ps * 8 + 128;

    bench_st("glibc", sys_alloc, sys_free, small);
    bench_st("dmalloc", dm_alloc, dm_free, small);
    bench_st("glibc", sys_alloc, sys_free, medium);
    bench_st("dmalloc", dm_alloc, dm_free, medium);
    bench_st("glibc", sys_alloc, sys_free, large);
    bench_st("dmalloc", dm_alloc, dm_free, large);

    bench_mt("glibc", sys_alloc, sys_free, small);
    bench_mt("dmalloc", dm_alloc, dm_free, small);
    bench_mt("glibc", sys_alloc, sys_free, medium);
    bench_mt("dmalloc", dm_alloc, dm_free, medium);
    bench_mt("glibc", sys_alloc, sys_free, large);
    bench_mt("dmalloc", dm_alloc, dm_free, large);
    return 0;
}
