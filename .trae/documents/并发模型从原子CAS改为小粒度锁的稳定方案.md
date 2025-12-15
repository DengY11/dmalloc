## 问题回顾
- 现实现引入多把自旋锁（`atomic_flag`）并在函数中交叉持锁，出现锁顺序反转：
  - `src/page_heap.c:326–354` 在持有地址链锁时调用 `bucket_remove`，而 `bucket_remove` 会再拿桶锁
  - `src/page_heap.c:144–166` 合并邻居时同样先拿地址锁再操作桶，路径和 `span_alloc`/`bucket_insert` 的拿锁顺序可能相反
- 心跳“进度为 0”且进程被系统终止，强烈指向死锁或活锁
- 原子无锁路径复杂度高（ABA、回收与并发入栈交错），维护风险大

## 目标与原则
- 用小粒度、可读性高的互斥锁替换自旋+原子，大幅降低死锁风险
- 明确统一的锁顺序，避免反转；尽量短持锁、避免在持锁时调用系统接口（如 `munmap`）
- 简单场景继续使用原子（如统计字段），不影响正确性

## 锁设计
- 中央自由链表（每个 size class 一把 `pthread_mutex_t`）：
  - `central_mu[sc]` 保护 `central[sc].head`、增长与整段回收
  - `SmallSpan.free_objs/total_objs` 在持锁下更新；统计仍可用原子或在锁下读取
- 页堆：
  - 桶锁：`pthread_mutex_t bucket_mu[MAX_BUCKETS]`，每个桶一把锁
  - 地址链锁：`pthread_rwlock_t addr_rw`，读遍历拿读锁，插入/删除/合并拿写锁
  - 大桶跳表锁：`pthread_mutex_t large_mu`（作为“最后一个桶”的专用锁）
  - 元数据池：`pthread_mutex_t meta_mu` 保护 `meta_free_list`
- 统计字段：继续用原子型 `atomic_size_t`（简单场景），或在持相应锁后非原子更新，二选一均可

## 统一锁顺序（所有路径强制遵守）
1. 桶相关操作先按需要获取对应的“桶锁”（含大桶的 `large_mu`）
2. 然后获取地址链的写锁（如需修改地址链）；遍历查询只需读锁
3. 释放锁时反向：先释放地址链锁，再释放桶锁
4. 不在持任意锁时调用 `mmap/munmap/madvise`，避免长时间阻塞；必要时先记录操作，释放锁后执行系统调用

## 具体改造清单
- 头文件 `include/page_heap.h`
  - 移除 `atomic_flag` 锁声明，改为 `pthread_mutex_t bucket_mu[MAX_BUCKETS]`、`pthread_rwlock_t addr_rw`、`pthread_mutex_t large_mu`、`pthread_mutex_t meta_mu`
  - 保留或移除原子统计，选保留（简单场景使用原子）
- `src/page_heap.c`
  - `pageheap_init` 初始化所有互斥与读写锁
  - `meta_chunk_new/meta_acquire/meta_release` 改为在 `meta_mu` 下 push/pop
  - `bucket_insert/remove` 在对应 `bucket_mu[idx]` 或 `large_mu` 下操作；不再从地址锁里调用它们
  - `addr_insert_sorted/addr_remove` 使用 `addr_rw` 写锁
  - `coalesce_neighbors`：
    - 拿写锁修改地址链，并记录需要从桶移除/插入的节点集合
    - 释放写锁后分别在对应桶锁下执行 `bucket_remove/insert`
    - 更新统计使用原子加减，或在持桶锁时更新
  - `pageheap_grow`：先 `mmap`，再在写锁+桶锁下发布；统计用原子
  - `find_suitable/span_alloc/span_free`：
    - `find_suitable` 在持桶锁的快速检查（不持地址锁）或在读锁下遍历大桶
    - `span_alloc`：先在桶锁下取出候选，后在写锁下改地址链，再在桶锁下插回剩余部分；更新统计原子
    - `span_free`：在写锁下改地址链，随后按前述流程合并与桶操作
  - `pageheap_release_empty_spans/pageheap_madvise_idle_spans`：
    - 在读锁下找到候选；释放锁后做 `munmap/madvise`
    - 完成后拿写锁+桶锁进行移除并更新统计
- `src/large_bucket.c`
  - 保留线程局部 RNG
  - 在 `large_bucket_insert/remove/lower_bound` 内部不再自持锁，由调用方统一在 `large_mu` 下调用
- 中心路径 `src/dmalloc.c`
  - 恢复指针为普通指针，移除 `draining` 原子标记
  - 新增 `pthread_mutex_t central_mu[]`；`central_grow/dmalloc/dfree` 在该锁下操作
  - 整段回收时：先在 `central_mu[sc]` 下从 freelist 过滤，再调用页堆接口（页堆内部按顺序持锁）

## 调试与验证
- 心跳工具：保留 `tests/bench_mt.c` 的原子进度打印，确认无锁顺序反转后进度持续增长
- 单测：并发小对象/大对象/整段回收三套测试继续运行
- 死锁自检：在所有路径加断言检查当前持锁组合是否符合顺序（可选）

## 交付
- 完成上述替换与重构，消除死锁与活锁
- 更新文档：说明锁顺序、持锁范围、系统调用位置
- 保留少量原子（统计），其它路径走小粒度锁

确认后我将按此方案重构并提交代码与测试，随后用心跳和单测验证无死锁且性能稳定。