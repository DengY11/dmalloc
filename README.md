# dmalloc
try to build my own malloc, is it faster than glibc malloc? probably not.
```bash
glibc ST size=32 count=200000 time=9.91ms ops/s=20187782
dmalloc ST size=32 count=200000 time=7.36ms ops/s=27178024
glibc ST size=512 count=200000 time=26.74ms ops/s=7479707
dmalloc ST size=512 count=200000 time=17.39ms ops/s=11500934
glibc ST size=131200 count=4092 time=11.34ms ops/s=360758
dmalloc ST size=131200 count=4092 time=10.73ms ops/s=381327
glibc MT size=32 threads=8 each=80000 wall=33.36ms ops/s=19182906
dmalloc MT size=32 threads=8 each=80000 wall=29.96ms ops/s=21359581
glibc MT size=512 threads=8 each=80000 wall=67.38ms ops/s=9498074
dmalloc MT size=512 threads=8 each=80000 wall=33.45ms ops/s=19130689
glibc MT size=131200 threads=8 each=2046 wall=15.98ms ops/s=1024595
dmalloc MT size=131200 threads=8 each=2046 wall=74.68ms ops/s=219178
```