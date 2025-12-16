# dmalloc
try to build my own malloc, is it faster than glibc malloc? probably not.
```bash
glibc ST size=32 count=200000 time=8.40ms ops/s=23809801
dmalloc ST size=32 count=200000 time=10.01ms ops/s=19980000
glibc ST size=512 count=200000 time=22.58ms ops/s=8856599
dmalloc ST size=512 count=200000 time=19.02ms ops/s=10513617
glibc ST size=131200 count=4092 time=9.45ms ops/s=433152
dmalloc ST size=131200 count=4092 time=10.72ms ops/s=381604
glibc MT size=32 threads=8 each=80000 wall=15.04ms ops/s=42555844
dmalloc MT size=32 threads=8 each=80000 wall=30.56ms ops/s=20939023
glibc MT size=512 threads=8 each=80000 wall=63.18ms ops/s=10129329
dmalloc MT size=512 threads=8 each=80000 wall=99.02ms ops/s=6463276
glibc MT size=131200 threads=8 each=2046 wall=16.36ms ops/s=1000736
dmalloc MT size=131200 threads=8 each=2046 wall=82.78ms ops/s=197739
```