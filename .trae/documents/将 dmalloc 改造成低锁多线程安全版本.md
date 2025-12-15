## 现状问题点（线程不安全定位）
- `src/dmalloc.c:9` 全局 `central[]` 无并发保护；在 `central_grow/alloc/free` 中对 `head` 进行非原子读写
- `src/dmalloc.c:75–87` 在增长时批量 `central[sc].head = user` 推入自由链表，存在并发覆盖
- `src/dmalloc.c:108–116` 分配时对 `central[sc].head` 非原子弹出，存在数据竞争
- `src/dmalloc.c:132–151` 释放时对 `central[sc].head` 非原子入栈，存在数据竞争；同时在同一链表上执行“整段回收”遍历删除
- `src/dmalloc.c:85–87,116,135` 对 `SmallSpan.free_objs/total_objs` 的增减为普通整型，存在并发竞态
- `src/page_heap.c:9` 全局 `page_heap` 无锁；所有桶头、地址链、统计均为非原子
- `src/page_heap.c:31,49–57,62–66` `meta_free_list` 元数据栈非原子 push/pop
- `src/page_heap.c:69–100` 桶插入/删除对 `page_heap.free_buckets[idx]` 和链表操作无锁
- `src/page_heap.c:103–132` 地址有序双向链表插入/删除无锁
- `src/page_heap.c:145–166,194–214,233–279` 合并邻居、增长、分配、释放均修改共享结构且无锁
- `src/page_heap.c:308–323,326–374` 在并发修改下对地址链遍历/munmap/madvise 无同步
- `src/large_bucket.c:36–42` 使用 `srand/rand` 的全局 RNG 非线程安全
- `src/large_bucket.c:44–90` 大桶跳表插入/删除/查找无锁

## 改造目标
- 小对象路径（≤`MAX_SMALL`）：无锁/原子化的中心自由链表，避免全局大锁
- 大对象路径与页堆：细粒度锁+原子统计，减少争用；必要处使用轻量自旋锁
- 初始化与统计查询：一次性原子初始化；统计使用原子变量以支持并发读

## 技术方案
- 中心自由链表（按 size class）
  - 将 `CentralFreeList.head` 改为 `_Atomic(void*)`（或 `atomic_uintptr_t`）
  - `dmalloc` 弹出：CAS 循环 `old = head; next = *(void**)old; CAS(head, old, next)`
  - `dfree` 压入：CAS 循环 `old = head; *(void**)obj = old; CAS(head, old, obj)`
  - `SmallSpan.free_objs` 改为 `atomic_size_t`；`total_objs` 仅初始化可保持普通整型
  - “整段回收”流程只在 `SmallSpan` 全空时触发：为该 size class 增设一个极少使用的微锁（`atomic_flag` 自旋或 `pthread_mutex_t`），仅用于回收阶段的链表扫描与 `span_free`，正常分配/释放不持锁
  - `central_grow` 批量填充：可逐个 CAS 压入，或构造本地栈后一次性 CAS 发布（将本地尾接到当前 `head` 再 CAS 设置新头）

- 页堆（`PageHeap`）
  - 统计字段改为 `atomic_size_t`：`mapped_pages/free_pages/spans_in_use/spans_free`
  - 元数据池 `meta_free_list` 改为 `_Atomic(Span*)`，push/pop 采用 CAS 循环
  - 桶结构：为 `free_buckets[i]` 引入每桶微锁（建议 `atomic_flag` 自旋锁），`bucket_insert/remove` 在对应桶锁保护下修改
  - 地址有序链表：引入一个地址链微锁（自旋或 `pthread_rwlock_t`），`addr_insert_sorted/addr_remove/coalesce_neighbors` 在写锁保护下执行；`pageheap_span_for_addr` 可用读锁
  - 大桶跳表：引入独立微锁保护 `large_skip_head` 的插入/删除；查找允许在无锁或读锁下进行（权衡实现复杂度）
  - `pageheap_init` 改为原子一次性初始化（类似 `central_init_once`）
  - `find_suitable/span_alloc/span_free/pageheap_grow/pageheap_release_*` 分别在最小必要锁域内操作（先锁桶或地址链，再更新统计为原子）

- 随机层级生成
  - 用线程局部 RNG 替换 `rand/srand`：如 `_Thread_local unsigned int seed;` + `rand_r(&seed)`；或实现轻量 `xorshift` 的每线程状态

## 兼容性与风险控制
- ABA 风险：中心无锁栈在“整段回收”时可能出现 ABA；通过在回收阶段对该 size class 施加微锁，确保回收与并发 push/pop 不交错，降低 ABA 风险
- 性能权衡：小对象路径保持无锁；页堆采用微锁分区，避免全局大锁；统计原子化使查询免锁
- 可回退：微锁使用 `pthread_mutex_t` 时更稳健；如需极致性能可后续替换为 `atomic_flag` 自旋并配合 `sched_yield`

## 实施步骤
- 结构体与头文件
  - 更新 `include/dmalloc.h`：将 `CentralFreeList.head` 改为原子指针；`SmallSpan.free_objs` 改为原子整型
  - 更新 `include/page_heap.h`：为统计字段使用原子；为桶与地址链预留锁对象声明
  - 引入 `stdatomic.h`，以及必要的 `pthread` 头（若选用 `pthread` 锁）

- 中心链表改造
  - 修改 `dmalloc.c` 的 `central_grow/dmalloc/dfree` 为 CAS 无锁栈；保留 `central_init_once`
  - 新增 size-class 级微锁，仅在整段回收时持锁执行 `dfree` 中的清理与 `pageheap_span_for_addr/span_free`

- 页堆改造
  - 实现 `pageheap_init_once`
  - 将 `meta_acquire/meta_release` 改为 CAS 栈
  - 为 `free_buckets[i]`、`addr_head`、`large_skip_head` 引入并使用微锁；调整 `bucket_insert/remove/addr_insert_sorted/addr_remove/coalesce_neighbors`、`find_suitable/span_alloc/span_free/pageheap_grow` 的锁域
  - 将统计更新改为原子加减

- RNG 改造
  - 在 `large_bucket.c` 使用线程局部种子替换 `srand/rand`

- 测试与验证
  - 单元测试：
    - 多线程小对象压测：并发 `dmalloc/dfree` 不崩溃、对象内容正确、`free_objs` 与统计一致
    - 多线程大对象压测：并发 `span_alloc/span_free/pageheap_grow` 不死锁、不越界
    - 极端整段回收：高并发下促发 `SmallSpan` 全空回收，验证回收后的指针不再被弹出
  - 压测对比：与当前单线程版本的吞吐与延迟对比，观察锁争用

## 交付产物
- 原子与微锁改造后的源代码
- 新增并发单元测试与压力测试脚本
- 简短的设计说明与使用指南（初始化、可配置项）

如确认以上方案，我将开始按上述步骤在代码中实施改造，并补充测试与验证。