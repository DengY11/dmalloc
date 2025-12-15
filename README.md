# dmalloc
bench mark result:
Macos Apple M3pro
```bash
-- Small sizes (1..128) --
libc malloc: time=92.09 ms ops=2000000 ops/s=21718356 MB=61.53
dmalloc: time=160.56 ms ops=2000000 ops/s=12456325 MB=61.50
-- Medium sizes (1..1024) --
libc malloc: time=200.46 ms ops=2000000 ops/s=9977152 MB=489.09
dmalloc: time=319.50 ms ops=2000000 ops/s=6259781 MB=489.44
-- Large sizes (~pages) --
libc malloc: time=739.75 ms ops=400000 ops/s=540721 MB=6249.89
dmalloc: time=264.28 ms ops=400000 ops/s=1513541 MB=6248.87
dmalloc stats: page_size=16384 mapped=25216 free=25216 in_use_spans=0 free_spans=15
```
