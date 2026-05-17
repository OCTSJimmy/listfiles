# Fixed 15.1.3 — `fp_shard_insert_internal` 开放寻址探测死循环（内存 corruption 导致）

**日期**: 2026-05-17
**版本**: 15.1.3
**分类**: bug-fix / 并发死锁

---

## 现象

v15.1.2 在 `/public2` 测试仍卡死（运行 9 分 44 秒，CPU 时间 8 分 40 秒）。

- `pending_batches = 5` 永远不归零
- 4 个 thread_pool 工作线程全部 `futex_wait`
- 8 个 Worker 进程 Scanner 子线程也全部 `futex_wait`
- 06:49:39 密集 CMD_SCAN/BATCH/FINISH 后，仅剩 HEARTBEAT
- 输出文件 `000001.txt` 和 `scan_dirs.log` 均为 0 字节

---

## 根因推断

### 1. `batch_dedup_worker` 外层硬超时无效

v15.1.2 增加 `ITERATION_LIMIT = 100000`：

```c
for (int i = 0; i < batch->count; i++) {
    if (i >= ITERATION_LIMIT) { ... return; }
    fp_set_insert(ctx->visited_set, fp);
}
```

如果 `fp_set_insert` → `pthread_mutex_lock(&shard->mutex)` 死锁，或 `fp_shard_insert_internal` 的开放寻址探测进入无限循环，`i` 永远不会增加。**外层硬超时对内层死锁无效。**

### 2. `fp_shard_insert_internal` 无限循环的两种 corruption 路径

**路径 A：`shard->capacity` 被 corruption**
- `shard->capacity` 被篡改为超大非 2 幂值
- `pos = (idx + i) & (capacity - 1)` 的掩码失效，`pos` 越界
- `meta[pos]` 读取到相邻内存的垃圾值，永远不是 0/1/2
- 循环永不退出

**路径 B：`meta[pos]` 被 corruption**
- 即使 `capacity` 正常，某个 `meta[pos]` 被篡改为 3（非 0/1/2）
- `if (m == 0)` / `if (m == 2)` / `if (m == 1)` 全部不命中
- 循环永不退出

### 3. 死锁链

1. 线程 A 在 `fp_shard_insert_internal` 无限循环中，持有 `shard->mutex`
2. 线程 B/C/D 在 `fp_set_insert` 的 `pthread_mutex_lock(&shard->mutex)` 中等同一个 mutex
3. 所有 thread_pool 工作线程卡住，`batch_dedup_worker` 永不返回
4. completed 队列永远为空，`pending_batches` 不归零
5. 无新 CMD_SCAN dispatch，Worker Scanner 线程在 `pthread_cond_wait` 中空闲
6. 全局停摆

---

## 修复方案

### 修改 1：`fp_shard_insert_internal` 增加 `PROBE_LIMIT`

```c
const size_t PROBE_LIMIT = 1000000; /* v15.1.3 */
for (size_t i = 0; i < shard->capacity; i++) {
    if (i >= PROBE_LIMIT) {
        log_fatal("[FPSet] open-addressing probe limit exceeded ...");
        return false;
    }
    ...
}
```

即使 `meta[pos]` 被 corruption 为 3，100 万步后强制退出，释放 mutex。

### 修改 2：`shard->capacity` sanity check

函数入口增加三道检查：

1. `shard->capacity == 0` → `log_fatal` 返回
2. `shard->capacity > (1ULL << 30)` → `log_fatal` 返回
3. `(shard->capacity & (shard->capacity - 1)) != 0` → `log_fatal` 返回（非 2 幂次）

### 修改 3：扩容时溢出检查 + 失败回滚

```c
size_t new_cap = old_cap << 1;
if (new_cap > (1ULL << 30)) { log_fatal(...); return false; }

shard->meta = calloc(new_cap, sizeof(uint8_t));
shard->table = calloc(new_cap, sizeof(Fingerprint));
if (!shard->meta || !shard->table) {
    free(shard->meta); free(shard->table);
    shard->meta = old_meta;
    shard->table = old_table;
    return false;
}
```

防止扩容失败导致 `meta`/`table` 为 NULL，后续访问崩溃或 corruption。

---

## 验证

- 编译通过：`make clean && make`，零警告
- 正常场景：开放寻址探测通常 < 5 步，PROBE_LIMIT 不会误触发
- 异常场景：capacity corruption / meta corruption 时，100 万步内强制退出，释放 mutex，暴露问题

---

## 待确认

下次 `/public2` 测试需观察日志：
- 若出现 `[FPSet] open-addressing probe limit exceeded` → 确认 corruption 假设，需进一步审计内存安全
- 若出现 `[FPSet] shard capacity corrupted` → 确认 corruption 源
- 若不再卡死 → 修复有效

---

## 关联文件

- `src/fingerprint_set.c`：`fp_shard_insert_internal` 核心修改
- `src/main_loop.c`：`batch_dedup_worker` 硬超时（v15.1.2，保留）
- `include/config.h`：版本号 `15.1.3`

---

## 后续方向

如果 v15.1.3 的 `log_fatal` 被触发，说明 `FingerprintShard` 确实存在内存 corruption。需要进一步审计：
- `batch->results` / `batch->paths` 是否有越界写可能
- `parse_batch` 对 IPC payload 的边界检查是否充分
- `ipc_recv` 的 `safe_read` 对 `EINTR` 的处理是否完整
- 是否有其他代码（如 `async_writer`）可能越界写 `visited_set` 所在内存
