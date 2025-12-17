# dmalloc
try to build my own malloc, is it faster than glibc malloc? probably not.
```bash
glibc ST size=32 count=200000 time=9.96ms ops/s=20086308
dmalloc ST size=32 count=200000 time=8.81ms ops/s=22696293
glibc ST size=512 count=200000 time=23.43ms ops/s=8535023
dmalloc ST size=512 count=200000 time=18.59ms ops/s=10758421
glibc ST size=131200 count=4092 time=10.57ms ops/s=387211
dmalloc ST size=131200 count=4092 time=10.50ms ops/s=389751
glibc MT size=32 threads=8 each=80000 wall=18.35ms ops/s=34884625
dmalloc MT size=32 threads=8 each=80000 wall=30.09ms ops/s=21268772
glibc MT size=512 threads=8 each=80000 wall=68.85ms ops/s=9296117
dmalloc MT size=512 threads=8 each=80000 wall=32.18ms ops/s=19886814
glibc MT size=131200 threads=8 each=2046 wall=16.89ms ops/s=968862
dmalloc MT size=131200 threads=8 each=2046 wall=84.24ms ops/s=194302
```