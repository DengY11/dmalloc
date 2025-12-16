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
    _Atomic(void*) head;  /* lock-free stack of free objects (payload stores next) */
    size_t         obj_size;     /* payload size for this class */
} CentralFreeList;

typedef struct _ThreadCache {
    void*  head[(MAX_SMALL / D_ALIGN)];
    size_t count[(MAX_SMALL / D_ALIGN)];
    size_t limit[(MAX_SMALL / D_ALIGN)];
} ThreadCache;

typedef struct _SmallSpan {
    size_t  size_class;   /* owning size class */
    size_t  total_objs;   /* number of objects in this span */
    size_t  free_objs;    /* number of free objects currently */
} SmallSpan;

/* small object allocator API */
void* dmalloc(size_t size);
void  dfree(void* ptr);
void* drealloc(void* ptr, size_t size);

#endif /* DMALLOC_H */
