# dmalloc
try to build my own malloc, is it faster than glibc malloc? probably not.
```bash
glibc ST size=32 count=200000 time=12.53ms ops/s=15966943
dmalloc ST size=32 count=200000 time=10.11ms ops/s=19772634
glibc ST size=512 count=200000 time=27.52ms ops/s=7267953
dmalloc ST size=512 count=200000 time=19.50ms ops/s=10258594
glibc ST size=131200 count=4092 time=7.70ms ops/s=531567
dmalloc ST size=131200 count=4092 time=11.48ms ops/s=356507
glibc MT size=32 threads=8 each=80000 wall=10.30ms ops/s=62135628
dmalloc MT size=32 threads=8 each=80000 wall=31.62ms ops/s=20241686
glibc MT size=512 threads=8 each=80000 wall=66.83ms ops/s=9575827
dmalloc MT size=512 threads=8 each=80000 wall=48.52ms ops/s=13190166
glibc MT size=131200 threads=8 each=2046 wall=14.17ms ops/s=1155282
dmalloc MT size=131200 threads=8 each=2046 wall=82.38ms ops/s=198692
```