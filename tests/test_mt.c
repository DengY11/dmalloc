#include "../include/dmalloc.h"
#include "../include/page_heap.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>

typedef struct {
    int id;
} ThreadArg;

static void* worker(void* arg)
{
    (void)arg;
    const int N = 4000;
    void* ptrs[N];
    for (int i = 0; i < N; i++){
        size_t sz = (i % 256) + 1;
        ptrs[i] = dmalloc(sz);
        assert(ptrs[i]);
        memset(ptrs[i], 0xEF, sz);
    }
    for (int i = 0; i < N; i += 5){
        size_t ns = ((i % 300) + 32);
        void* p2 = drealloc(ptrs[i], ns);
        assert(p2);
        ptrs[i] = p2;
    }
    for (int i = 0; i < N; i++) dfree(ptrs[i]);
    return NULL;
}

int main(){
    pageheap_init();
    const int T = 8;
    pthread_t th[T];
    ThreadArg args[T];
    for (int i = 0; i < T; i++){
        args[i].id = i;
        pthread_create(&th[i], NULL, worker, &args[i]);
    }
    for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
    return 0;
}
