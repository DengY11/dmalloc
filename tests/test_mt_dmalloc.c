#include "../include/dmalloc.h"
#include "../include/page_heap.h"
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int id;
    int iters;
} WorkerArg;

static void* worker(void* argp){
    WorkerArg* arg = (WorkerArg*)argp;
    const int N = 800;
    void* ptrs[N];
    for (int it = 0; it < arg->iters; it++){
        for (int i = 0; i < N; i++){
            size_t sz = (size_t)((i * 13 + arg->id * 7 + it) % 256) + 1;
            void* p = dmalloc(sz);
            assert(p);
            memset(p, 0xEF, sz);
            ptrs[i] = p;
        }
        for (int i = 0; i < N; i += 3){
            size_t ns = (size_t)((i * 17 + arg->id * 11 + it) % 300) + 1;
            void* p2 = drealloc(ptrs[i], ns);
            assert(p2);
            ptrs[i] = p2;
        }
        for (int i = 0; i < N; i++){
            dfree(ptrs[i]);
        }
    }
    return NULL;
}

int main(){
    pageheap_init();
    const int T = 4;
    pthread_t th[T];
    WorkerArg args[T];
    for (int i = 0; i < T; i++){ args[i].id = i; args[i].iters = 40; }
    for (int i = 0; i < T; i++){
        int r = pthread_create(&th[i], NULL, worker, &args[i]);
        assert(r == 0);
    }
    for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
    printf("test_mt_dmalloc OK\n");
    return 0;
}

