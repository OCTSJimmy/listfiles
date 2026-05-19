# Fixed 15.1.4 — process_completed_batch CPU 空转防御

## 问题现象

v15.1.3 `/public2` 测试，Master PID 88601 运行 16+ 小时：

- `perf top -p 88601` 显示 `process_completed_batch` 占 **99.86%** CPU
- `pending_batches=3` 不再下降，`pending_tasks=196`
- 8 个 Worker 全部空闲（poll / epoll_wait / futex_wait）
- `errlogs.err` 69K 行，**零 FATAL / ERROR**，排除 `fp_shard_insert_internal` PROBE_LIMIT 路径
- Master stack 为 `running`（纯用户态死循环）
- `/proc/88601/syscall` 频繁变化（非阻塞系统调用），确认不是内核态阻塞

## 根因推断

主线程卡在 `drain_completed_batches` → `thread_pool_poll_completed` → `process_completed_batch` 调用链中。

`process_completed_batch` 的唯一 CPU 密集循环：

```c
for (int i = 0; i < batch->count; i++) {
    ...
}
```

若 `batch->count` 被内存 corruption 覆盖为巨大正值（如 `INT_MAX`），则：
- 循环迭代 2,147,483,647 次
- `i` 溢出回绕后 `i < batch->count` 永远为真 → **无限循环**
- 循环体若以轻量 `continue` 为主（如 `batch->results[i]` 随机值 bit0 被置位），CPU 空转可达数小时至数十小时
- `atomic_fetch_sub(&ctx->pending_batches, 1)` 位于循环之后，**永远不会执行**

### 为什么不是 `batch->count` 正常但 loop body 卡死？

loop body 中的函数调用（`fpbin_append`、`send_scan_to_ipc`、`record_path_batch_append`、`async_writer_submit_batch`、`lost_tasks_push`）均会在 `perf top` 中显示为独立符号。但 `perf top` 截图中 `process_completed_batch` 独占 99.86%，**无子调用符号**，说明循环体以轻量路径为主（大量 `continue`），或循环条件本身永不退出。

### 为什么不是 `fp_shard_insert_internal`？

v15.1.3 已引入 `PROBE_LIMIT = 1,000,000`。若触发，会产生 `[FPSet] open-addressing probe limit exceeded` FATAL 日志。日志中**无任何 FATAL**，排除该路径。

## 防御修复（v15.1.4）

四层防线：

| 防线 | 位置 | 检查内容 | 超限行为 |
|---|---|---|---|
| 1 | `main_loop_handle_batch` | `parsed.count < 0 \|\| > 1,000,000` | `log_fatal` + `parsed_batch_free` + return |
| 2 | `batch_dedup_worker` | `batch->count < 0 \|\| > 1,000,000` | `log_fatal` + return |
| 3 | `process_completed_batch` 入口 | `batch->count < 0 \|\| > 1,000,000` | `log_fatal` + 安全释放 batch + `atomic_fetch_sub pending_batches` |
| 4 | `process_completed_batch` 循环内 | `i >= PROC_ITER_LIMIT (1,000,000)` | `log_fatal` + `break` |

### 代码变更

文件：`src/main_loop.c`

1. `main_loop_handle_batch` 中 `parse_batch` 成功后增加 `parsed.count` sanity check
2. `batch_dedup_worker` 入口增加 `batch->count` sanity check
3. `process_completed_batch` 入口增加 `batch->count` sanity check + 安全释放路径
4. `process_completed_batch` 循环增加 `PROC_ITER_LIMIT`

## 验证

```bash
cd /root/listfiles
make clean && make
# 零警告，编译通过
```

## 后续行动

1. 在 `/public2` 运行 v15.1.4，观察 `errlogs.err` 是否出现 `[Batch] batch count out of range` 或 `[Batch] iteration limit exceeded` FATAL 日志
2. 若出现，记录 `count` 值和 `worker_id`，进一步定位 corruption 来源（`batch->results` 越界写、`parse_batch` 边界漏洞、`ipc_recv` 完整性、或 `async_writer` 越界写）
3. 若未出现但仍卡死，使用 `perf record -g --pid <pid>` 获取精确调用栈，确认热点指令位置

## 版本

- 修复版本：v15.1.4
- 提交：`待提交`
- 日期：2026-05-17
