# v15.1.0 (2026-05-17)

## 修复内容

### Master 侧 Worker 显式状态机 (v15.1.0)

**问题根因**：v14+ 引入 Worker 多线程（Scanner 子线程）后，Worker 主线程通过 `poll()` 非阻塞读取 `fd_cmd`，可连续接收多个 `CMD_SCAN` 并逐次 `cond_signal` 唤醒 Scanner。但 Scanner 一次只能处理一个任务，导致后续任务覆盖 `task_path`，产生"幽灵任务"——Master 认为已下发但永远收不到 `FINISH`。

**根因闭环**：Master 侧调度逻辑使用纯轮询（`next_dispatch_worker % num_workers`），仅以 `is_alive` 判定 Worker 是否可用，**没有任何 BUSY/IDLE 状态跟踪**。一个已经在处理任务的 Worker 会被再次选中，新任务通过 IPC 线程写入 `fd_cmd` 管道，Worker 主线程立刻读走并覆盖。最终 8 个 Worker 全部处于 BUSY（Scanner 阻塞在 `pthread_cond_wait`），Master 仍在发但无 IDLE Worker 可处理，主线程等不到 `FINISH`，全局停摆。

**修复方案**：在 `WorkerSlot` 中增加 `_Atomic int state` 字段，定义三个显式状态：
- `WORKER_STATE_IDLE (0)`：可接收新任务
- `WORKER_STATE_BUSY (1)`：已分配任务，等 `FINISH`
- `WORKER_STATE_DEAD (2)`：Worker 已死或正在替换

**修改点**：
1. `include/worker_proc.h`：`WorkerSlot` 增加 `_Atomic int state`，定义三个状态常量。
2. `src/worker_proc.c`：`worker_pool_spawn` 初始化 `state = IDLE`；`worker_pool_replace` 杀死旧 Worker 后设 `state = DEAD`。
3. `src/main_loop.c`：
   - `process_completed_batch` 调度目录时：遍历寻找 `STATE_IDLE` 的 Worker，找到后原子置为 `STATE_BUSY` 再发 `CMD_SCAN`。若无 IDLE Worker，任务入 `lost_tasks`。
   - `dispatch_lost_tasks`：同样只选择 `STATE_IDLE` Worker。
   - `RET_FINISH`：收到后将对应 Worker `state` 置为 `STATE_IDLE`。
   - `RET_READY`：新 Worker 初始化完成后置 `STATE_IDLE`。
   - `cleanup_dead_worker_slot`：置 `STATE_DEAD`。
4. `include/config.h`：版本号更新为 `15.1.0`。

**编译验证**：`make clean && make` 通过，无警告。

## 文件变更

| 文件 | 变更 |
|------|------|
| `include/worker_proc.h` | + `state` 字段 + `WORKER_STATE_*` 常量 |
| `src/worker_proc.c` | spawn 初始化 IDLE；replace 设 DEAD |
| `src/main_loop.c` | 调度逻辑 state 过滤；状态转换点 |
| `include/config.h` | VERSION "15.1.0" |
