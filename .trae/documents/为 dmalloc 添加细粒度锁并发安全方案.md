## 目标
- 在不改变现有分配/释放算法前提下，加细粒度锁确保并发安全
- 锁开销尽可能小，避免全局大锁；严格统一加锁顺序避免死锁
- 保持已有一次性初始化逻辑，补齐堆与锁的并发初始化

## 锁设计
- `central_lock[NUM_CLASSES]`：每个大小类一把互斥锁，保护 `central[]` 单链及对应 `SmallSpan` 计数（`include/dmalloc.h:14-23`, `src/dmalloc.c:9, 31-34`）
- `addr_lock`：保护地址有序双向链表与邻接合并（`src/page_heap.c:102-132, 134-166, 269-279, 307-323`）
- `bucket_lock[MAX_BUCKETS]`：每个页数精确桶一把锁，保护桶内单链（`src/page_heap.c:68-86`）
- `skiplist_lock`：保护“大桶”跳表所有操作与其 `rand()` 层级生成（`src/large_bucket.c:21-43`）
- `meta_lock`：保护 `meta_free_list` 元数据池获取/归还（`src/page_heap.c:31, 34-66`）
- `stats_lock`（可选）：若统计在并发下需要强一致，对 `PageHeapStats` 更新与快照加锁（`include/page_heap.h:36-42`, `src/page_heap.c:375+`）

## 统一加锁顺序（全局不变）
- 任何会同时涉及多个子系统的操作，按以下顺序加锁：`addr_lock` → `bucket_lock[低索引] → bucket_lock[高索引]` → `skiplist_lock` → `meta_lock` → `central_lock[sc]`
- 若仅操作单一结构，仅持对应锁；严禁反序嵌套，必要时释放并重试

## 组件改造与覆盖范围
- 小对象中心链（`src/dmalloc.c`）
  - `dmalloc()`：按大小类 `sc` 加 `central_lock[sc]`，`head` 弹出与 `SmallSpan.free_objs--` 置于锁内（`src/dmalloc.c:90-117`）
  - `dfree()`：按所属 `SmallSpan.sc` 加 `central_lock[sc]`，推回单链并 `free_objs++`；若满空，持锁内剔除该 `SmallSpan` 所有节点，释放锁后交给页堆回收（`src/dmalloc.c:120-163`）
  - `central_grow(sc)`：先向页堆申请 `Span`（不持 `central_lock`），再加 `central_lock[sc]` 切分挂链，避免锁嵌套（`src/dmalloc.c:44-88`）
  - `central_init_once()`：保留原子一次性，新增对 `central_lock[]` 初始化（`src/dmalloc.c:23-42`）
- 页堆（`src/page_heap.c`）
  - `pageheap_init()`：初始化 `addr_lock`、`bucket_lock[]`、`skiplist_lock`、`meta_lock`、`stats_lock`（`src/page_heap.c:9-30, 194-214`）
  - `bucket_insert/remove()`：仅持对应 `bucket_lock[i]`；大桶改为持 `skiplist_lock`（`src/page_heap.c:68-86`，配合 `src/large_bucket.c`）
  - `addr_insert_sorted/addr_remove()`：持 `addr_lock`（`src/page_heap.c:102-132, 307-323`）
  - `coalesce_neighbors()`：持 `addr_lock`，对涉及桶按索引升序逐个持锁调整（`src/page_heap.c:134-166, 269-279`）
  - `span_alloc(pages)`：
    - 精确桶：持 `bucket_lock[p]` 选择与可能拆分；若拆分或需触达地址链，先持 `addr_lock` 再持桶锁，遵循顺序
    - 大桶：持 `skiplist_lock` 查询下界，必要时与 `addr_lock` 配合拆分
  - `span_free(span)`：持 `addr_lock` 合并邻居；按需要持目标桶锁或 `skiplist_lock` 完成插入
  - `pageheap_grow()`：持 `meta_lock` 创建 `Span` 元数据，持 `addr_lock` 插入地址链，再持对应桶锁挂入（`src/page_heap.c:194-214`）
  - 查询与统计：读路径可无锁或读锁；若需强一致，持 `addr_lock`/`stats_lock`（`pageheap_stats`, `pageheap_span_for_addr`）
- 大桶跳表（`src/large_bucket.c`）
  - `large_bucket_*` 全部统一在 `skiplist_lock` 保护下；`random_level()` 在锁内调用以规避 `rand()` 竞态（`src/large_bucket.c:14-18, 36-43`）

## 关键并发情形与处理
- 小对象释放触发整段回收：先在 `central_lock[sc]` 下将该段对象从中心链剔除，释放锁后再进入页堆的 `span_free()`（跨锁分段，避免死锁）
- `span` 拆分/合并：任何改动地址链必须在 `addr_lock` 下；对应桶迁移遵循桶锁升序获取
- 大桶与精确桶交互：若从大桶拆出一部分进入精确桶，锁顺序为 `addr_lock → skiplist_lock → bucket_lock[p]`

## 初始化与销毁
- 初始化：在 `pageheap_init()` 与 `central_init_once()` 中统一初始化所有互斥锁；若构建系统支持，入口增加 `pthread_once` 包装 `pageheap_init`
- 销毁：测试或工具程序中可提供 `pageheap_destroy()` 以销毁锁（生产可不暴露）

## 性能与可扩展
- 细粒度锁避免全堆大锁；热点主要在 `central_lock[sc]` 与少数小页桶
- 后续可演进为 `threadcache` 缓解中心链竞争，但本次不改算法

## 测试与验证
- 新增并发压力测试：`tests/test_thread_safety.c`，多线程循环 `dmalloc/dfree/drealloc`，交叉大小类和大对象
- 静态检查：开启 TSAN/ASAN 构建变体确保无数据竞争与越界
- 运行现有单测：`test_dmalloc.c`, `test_page_heap.c`, `test_large_bucket.c` 在 `-pthread` 下通过

## 变更文件
- `include/dmalloc.h`：为 `CentralFreeList` 增加锁定义；声明初始化接口
- `src/dmalloc.c`：在 `dmalloc/dfree/central_grow/central_init_once` 接入 `central_lock[]`
- `include/page_heap.h`：为 `PageHeap` 增加锁字段；必要的 API 说明
- `src/page_heap.c`：在桶/地址链/合并/增长/释放路径按方案加锁
- `include/large_bucket.h` 与 `src/large_bucket.c`：跳表在单锁下运行
- `tests/test_thread_safety.c`：新增并发测试；构建时链接 `-pthread`

## 接入与编译
- 构建参数增加 `-pthread`；若使用 `CMake`/`Makefile`，在目标链接与编译选项添加
- 保留对非并发环境的 ABI 不变，锁为透明实现细节
