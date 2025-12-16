#ifndef DMALLOC_H
#define DMALLOC_H
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#define D_ALIGN     16
#define MAX_SMALL   1024

typedef struct _ObjHdr {
    void* owner;          /* SmallSpan* for small; Span* for large */
    uint16_t size_class;  /* index for small; 0xFFFF for large */
    uint16_t flags;       /* bit0: large */
} ObjHdr;

typedef struct _CentralFreeList {
    struct _SmallSpan* span_head;
    size_t             obj_size;
} CentralFreeList;

typedef struct _ThreadCache {
    void*  head[(MAX_SMALL / D_ALIGN)];
    size_t count[(MAX_SMALL / D_ALIGN)];
    size_t limit[(MAX_SMALL / D_ALIGN)];
} ThreadCache;

typedef struct _SmallSpan {
    size_t  size_class;
    size_t  total_objs;
    size_t  free_objs;
    void*   free_list;
    struct _SmallSpan* next;
} SmallSpan;

/* small object allocator API */
void* dmalloc(size_t size);
void  dfree(void* ptr);
void* drealloc(void* ptr, size_t size);

#endif /* DMALLOC_H */
