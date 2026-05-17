# v15.1.1 (2026-05-17)

## 补全实现：Worker 状态机完整化

基于 v15.1.0 的 Master 侧显式状态机，补全以下设计与实现不一致项：

### 1. INITIALIZING 状态 + startup_timeout

**设计**：spawn 后 Worker 处于 INITIALIZING，60s 内未收到 READY 则判定死亡并替换。

**实现**：
- `include/worker_proc.h`：增加 `WORKER_STATE_INITIALIZING = 3`
- `src/worker_proc.c`：`worker_pool_spawn` 初始化为 `INITIALIZING`
- `src/ipc_thread.h`：`IpcThreadCtx` 增加 `spawn_time` 字段
- `src/ipc_thread.c`：`CMD_REPLACE` 时记录 `spawn_time = time(NULL)`
- `src/ipc_thread.c`：心跳超时检测区分 startup_timeout（60s）与常规超时（30s）。若 `last_heartbeat == spawn_time`（未收到任何 READY/HEARTBEAT），使用 60s 阈值。

### 2. RET_ERROR 后置 IDLE

**设计**：设备级错误（ETIMEDOUT/EIO）不替换 Worker，Worker 回到 IDLE。

**实现**：
- `src/main_loop.c`：`handle_return_message` 的 `RET_ERROR` 分支，在 `main_loop_handle_error` 后增加 `atomic_store(&slot->state, WORKER_STATE_IDLE)`。

### 3. Monitor 显示每个 Worker 独立状态

**设计**：面板显示每个 Worker 的 INIT/IDLE/BUSY/DEAD 状态及 current_path 截断。

**实现**：
- `src/monitor.c`：`print_progress` 增加 `[Worker States]` 区块，遍历所有 slot，显示 `W{idx}: {STATE} pid={pid} path={...last20chars}`。

### 4. 版本号与编译

- `include/config.h`：VERSION "15.1.1"
- `make clean && make`：通过，零警告。

## 文件变更

| 文件 | 变更 |
|------|------|
| `include/worker_proc.h` | + `WORKER_STATE_INITIALIZING = 3` |
| `src/worker_proc.c` | spawn 初始化 `INITIALIZING` |
| `include/ipc_thread.h` | + `spawn_time` 字段 |
| `src/ipc_thread.c` | REPLACE 记录 spawn_time；心跳检测区分 60s/30s |
| `src/main_loop.c` | RET_READY 无条件置 IDLE；RET_ERROR 后置 IDLE |
| `src/monitor.c` | 增加 `[Worker States]` 面板区块 |
| `include/config.h` | VERSION 15.1.1 |
