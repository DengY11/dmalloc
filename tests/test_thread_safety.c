#include "../include/dmalloc.h"
#include "../include/page_heap.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    void** slots;
    size_t nslots;
    unsigned seed;
} Work;

static void* worker(void* arg){
    Work* w = (Work*)arg;
    for (size_t it = 0; it < w->nslots * 100; it++){
        size_t idx = rand_r(&w->seed) % w->nslots;
        if (!w->slots[idx]){
            size_t small = (size_t)(rand_r(&w->seed) % (MAX_SMALL + 1));
            size_t large = (size_t)(rand_r(&w->seed) % (1 << 20));
            size_t sz = (rand_r(&w->seed) & 1) ? small : (MAX_SMALL + large);
            void* p = dmalloc(sz);
            w->slots[idx] = p;
        } else {
            if (rand_r(&w->seed) & 1){
                size_t newsz = (size_t)(rand_r(&w->seed) % (MAX_SMALL + (1<<20)));
                void* np = drealloc(w->slots[idx], newsz);
                if (np) w->slots[idx] = np;
            } else {
                dfree(w->slots[idx]);
                w->slots[idx] = NULL;
            }
        }
    }
    for (size_t i = 0; i < w->nslots; i++){
        if (w->slots[i]){ dfree(w->slots[i]); w->slots[i] = NULL; }
    }
    return NULL;
}

int main(void){
    const size_t threads = 8;
    const size_t slots = 1024;
    pthread_t th[threads];
    Work w[threads];
    srand((unsigned)time(NULL));
    for (size_t t = 0; t < threads; t++){
        w[t].nslots = slots;
        w[t].seed = (unsigned)rand();
        w[t].slots = (void**)calloc(slots, sizeof(void*));
        pthread_create(&th[t], NULL, worker, &w[t]);
    }
    for (size_t t = 0; t < threads; t++) pthread_join(th[t], NULL);
    size_t released = pageheap_release_empty_spans(1);
    printf("released pages: %zu\n", released);
    for (size_t t = 0; t < threads; t++) free(w[t].slots);
    return 0;
}

