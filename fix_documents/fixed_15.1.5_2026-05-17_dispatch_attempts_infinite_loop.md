# Fixed 15.1.5 — dispatch loop `attempts` counter infinite loop

## 问题现象

v15.1.4 `/public2` 测试，运行 2 分 5 秒后卡死：

- `perf top`：`process_completed_batch` 占 **99.72%** CPU
- `pending_batches=4`，`pending_tasks=9`
- 8 个 Worker **全部为 BUSY**
- `Dir rate / File rate / Dequeue = 0.00/s`
- `errlogs.err` 零 FATAL / ERROR

## 根因

v15.1.0 引入的 Worker 状态机调度代码中存在 `while (attempts < num_workers)` 循环，`attempts` 计数器**未在 `continue` 路径上递增**。

```c
// BUGGY CODE (v15.1.0 ~ v15.1.4)
while (attempts < num_workers) {
    int candidate = ctx->next_dispatch_worker % num_workers;
    ctx->next_dispatch_worker++;
    WorkerSlot *cand_slot = &ctx->worker_pool->slots[candidate];
    if (!atomic_load(&cand_slot->is_alive)) continue;        // attempts 不增加
    if (atomic_load(&cand_slot->state) != WORKER_STATE_IDLE) continue; // attempts 不增加
    wid = candidate;
    break;
}
```

当所有 Worker 均为 BUSY 时：
- `attempts` 永远是 0，`num_workers=8`，条件永真
- 无限 `continue`，CPU 100% 空转
- Master 不处理 ret_queue，Worker FINISH 不被消费
- Worker 状态永远 BUSY
- **死锁闭环**

截图完美印证：所有 Worker `pid=170295~170308`，状态 `BUSY`，路径均为 `/public2/...` 下的深层目录。Master 卡死在处理一个包含多个目录项的 batch。

## 修复

两处 `continue` 前增加 `attempts++`：

1. `process_completed_batch` 中的目录 dispatch 循环（`src/main_loop.c:242-243`）
2. `dispatch_lost_tasks` 中的 lost task dispatch 循环（`src/main_loop.c:349-350`）

```c
// FIXED CODE (v15.1.5)
if (!atomic_load(&cand_slot->is_alive)) { attempts++; continue; }
if (atomic_load(&cand_slot->state) != WORKER_STATE_IDLE) { attempts++; continue; }
```

## 验证

```bash
cd /root/listfiles
make clean && make
# 零警告，编译通过
```

## 版本

- 修复版本：v15.1.5
- 提交：`待提交`
- 日期：2026-05-17
