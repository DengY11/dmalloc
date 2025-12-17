#include "../include/page_heap.h"
#include "../include/large_bucket.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>

static inline int skip_less(const Span* a, const Span* b){
    if (a->page_count != b->page_count) return a->page_count < b->page_count;
    return (uintptr_t)a->start < (uintptr_t)b->start;
}

static __thread uint64_t rng_state;
static inline uint64_t mix64(uint64_t x){
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}
static inline uint32_t tls_rand(void){
    if (rng_state == 0){
        uintptr_t t = (uintptr_t)pthread_self();
        rng_state = mix64(t ^ (uintptr_t)&rng_state);
        if (rng_state == 0) rng_state = 0x9e3779b97f4a7c15ULL;
    }
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)rng_state;
}
static int random_level(void){
    int lvl = 1;
    while ((tls_rand() & 1u) && lvl < MAX_SKIP_LEVELS) lvl++;
    return lvl;
}

/* internal: ensure sentinel head exists */
static void ensure_head(PageHeap* heap)
{
    if (heap->large_skip_head) return;
    /* allocate a Span node via mmap as skiplist head (not freed) */
    size_t sz = sizeof(Span);
    void* mem = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return;
    Span* head = (Span*)mem;
    if (!head) return;
    memset(head, 0, sizeof(Span));
    head->skip_level = MAX_SKIP_LEVELS;
    for (int i = 0; i < MAX_SKIP_LEVELS; i++) head->skip_next[i] = NULL;
    heap->large_skip_head = head;
}

void large_bucket_init(PageHeap* heap)
{
    heap->large_skip_head = NULL;
    ensure_head(heap);
}

void large_bucket_insert(PageHeap* heap, Span* s)
{
    ensure_head(heap);
    Span* update[MAX_SKIP_LEVELS];
    Span* x = heap->large_skip_head;
    for (int i = MAX_SKIP_LEVELS - 1; i >= 0; i--){
        while (x->skip_next[i] && skip_less(x->skip_next[i], s)) x = x->skip_next[i];
        update[i] = x;
    }
    int lvl = random_level();
    s->skip_level = (unsigned char)lvl;
    for (int i = 0; i < lvl; i++){
        s->skip_next[i] = update[i]->skip_next[i];
        update[i]->skip_next[i] = s;
    }
}

void large_bucket_remove(PageHeap* heap, Span* s)
{
    if (!heap->large_skip_head || !s) return;
    Span* update[MAX_SKIP_LEVELS];
    Span* x = heap->large_skip_head;
    for (int i = MAX_SKIP_LEVELS - 1; i >= 0; i--){
        while (x->skip_next[i] && skip_less(x->skip_next[i], s)) x = x->skip_next[i];
        update[i] = x;
    }
    Span* target = x->skip_next[0];
    if (target != s){
        /* not found; do nothing */
        return;
    }
    for (int i = 0; i < MAX_SKIP_LEVELS; i++){
        if (update[i]->skip_next[i] == s) update[i]->skip_next[i] = s->skip_next[i];
    }
    for (int i = 0; i < MAX_SKIP_LEVELS; i++) s->skip_next[i] = NULL;
    s->skip_level = 0;
}

Span* large_bucket_lower_bound(PageHeap* heap, size_t need)
{
    if (!heap->large_skip_head) return NULL;
    Span* x = heap->large_skip_head;
    for (int i = MAX_SKIP_LEVELS - 1; i >= 0; i--){
        while (x->skip_next[i] && x->skip_next[i]->page_count < need) x = x->skip_next[i];
    }
    return x->skip_next[0];
}
