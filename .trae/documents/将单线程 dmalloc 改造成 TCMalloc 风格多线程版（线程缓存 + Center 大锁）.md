## 与 TCMalloc 的差异
- 锁粒度：
  - 方案当前采用“一把大锁”保护 Center + PageHeap，以快速实现线程安全；
  - TCMalloc 采用每 size class 独立锁、PageHeap 独立锁与更细粒度的并发控制，争用更低。
- Center 组织：
  - 初版（简化）将对象直接挂到 Center 的对象池；
  - TCMalloc 的 Center 以 Span 为单位管理，每个 Span 有自己的 freelist，线程补充时从多个 Span 的 freelist 批量取；
  - 本次计划改为“Span 为中心”的 Center（见下文），更贴近 TCMalloc。
- 回收与平衡：
  - 初版通过 Center 扫描判断某 SmallSpan 的所有对象回到 Center 后再回收；
  - TCMalloc 还包含 TransferCache 等跨线程平衡机制与动态批量/配额策略；本版先用固定批量与简单回吐，后续迭代再增强。
- 大对象路径：
  - 本版用同一把锁保护 `span_alloc`/`span_free`；TCMalloc 的 PageHeap 有独立锁与更复杂的结构（如 hugepage）。

## 调整后的实现方案（更贴近 TCMalloc）
- ThreadCache（每线程，无锁）：
  - `head[sc]` 持有用户对象的单链表，`count/limit` 维持简单配额；
  - 线程退出析构器批量把各类对象归还到 Center。
- Center（全局，先用一把锁串行）：
  - 每个 size class 维护 `SpanList`：仅登记“仍有可分配对象”的 `SmallSpan`；不直接维护“对象级”的中心池；
  - `SmallSpan` 扩展字段：`free_list`（属于该 span 的可分配对象链），`center_free_count`（当前在 Center 内的自由对象数，便于判断是否全在 Center）；
  - `center_refill(sc, n)`：在锁下从一个或多个 `SmallSpan.free_list` 取出最多 `n` 个对象给线程缓存；若某 `SmallSpan.free_list` 取空则从活跃 `SpanList` 移除；
  - `center_drain(sc, objs...)`：线程缓存回吐批次时，按对象的 `owner==SmallSpan` 将其归还至对应 `free_list`，并更新 `center_free_count`；当 `center_free_count == total_objs` 时，说明该 `SmallSpan` 的所有对象已在 Center，可触发整 Span 回收。
  - `central_grow(sc)`：在锁下向 PageHeap 申请一个 `Span`，切片成 `SmallSpan` 对象，初始化其 `free_list` 为满，然后将该 `SmallSpan` 加入该类 `SpanList`。
- PageHeap：沿用现有实现；所有调用在大锁下执行确保线程安全。

## 代码落地点
- `include/dmalloc.h`
  - 保留 `ObjHdr`/`SmallSpan`，给 `SmallSpan` 增加 `void* free_list; size_t center_free_count;`。
  - 定义 `ThreadCache` 结构（内部使用可不暴露）。
- `src/dmalloc.c`
  - 初始化：`pthread_once` + `pthread_mutex_t center_mu` + `pthread_key_t tcache_key`；
  - 分配：
    - 小对象：先读线程缓存；空则在锁下 `center_refill(sc, batch)` 再弹；
    - 大对象：在锁下走 `span_alloc` 路径；
  - 释放：
    - 大对象：在锁下 `span_free`；
    - 小对象：入线程缓存并维护 `SmallSpan->free_objs`；超过配额在锁下 `center_drain(sc, batch)`；回吐后做一次 `center_try_reclaim(sc)`（在 Center 内部根据 `center_free_count==total_objs` 触发回收）。
- `src/page_heap.c`：无需结构改动，调用由大锁保护。

## 批量与配额（初始值，可后调）
- `TCACHE_BATCH=32`，`TCACHE_LIMIT=128`（每类）；超限回吐一半。

## 验证与迭代
- 保持单线程用例通过（`src/dmalloc.c:90` 路径不变）。
- 新增多线程用例与基准，观察 `pageheap_stats` 与吞吐；
- 后续将 Center 大锁拆分为每类锁，并评估是否为 PageHeap 引入独立锁；再考虑引入 TransferCache。