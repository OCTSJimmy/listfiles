# Design.md — listfiles 架构设计文档

> 本文档记录 listfiles 的核心架构决策、版本演进与关键修复。与 README.md（用户指南）和 CHANGELOG.md（变更日志）互补。

---

## 版本

当前设计版本：**v14.0.1**（基于 v13.0.1 IPC 线程隔离架构，叠加 v14.0.0 Worker 多线程化 + v14.0.1 fd_out 互斥修复）

---

## 目录

1. [架构演进](#架构演进)
2. [v13.0.0：IPC 线程隔离](#v1300ipc-线程隔离)
3. [v13.0.1：启动崩溃修复](#v1301启动崩溃修复)
4. [消息协议](#消息协议)
5. [故障隔离模型](#故障隔离模型)
6. [已知问题与待办](#已知问题与待办)

---

## 架构演进

| 版本 | 架构 | 核心问题 | 解决方式 |
|------|------|---------|---------|
| v11.x | 多线程共享内存 (`pthread` + 消息队列) | D-State 不可杀死、锁竞争 | — |
| v12.0.0 | **进程模型** (`fork()` + `pipe` + `epoll`) | D-State 可 `SIGKILL`、COW 零拷贝 | 线程 → 进程 |
| v12.1.x | **fpbin 隔离** + **同构分片** | 恢复期间 pbin 读写冲突 | fpbin 临时缓存 + Footer 自描述 |
| v12.2.x | **双向管道死锁修复** + **O_NONBLOCK** + **poll 超时** + **数据竞争修复** | 管道死锁、fd 重用竞争、active_count 数据竞争 | 管道扩容、非阻塞、原子化 |
| v12.2.15 | **cleanup_done 重置** + **统一日志** + **dispatch 轮询修复** | cleanup_done 未重置导致 epoll 污染、无时间戳日志 | 重置标志、log.c 模块 |
| **v13.0.0** | **IPC 线程隔离** | 单线程 epoll 的架构瓶颈：一个 Worker 出问题导致整个 Master hang 死 | 8 个 IPC 线程常驻，主线程 = 消息总线 |
| **v13.0.1** | **IPC 协议原子写入** + **栈值污染防御** | `ipc_send` 两次 write 导致 Header 孤悬；`va_list` 异常导致历史块数打印错误 | 合并写入、noinline、sanity check |
| **v14.0.0** | **Worker 多线程化**（Scanner 线程 + IPC 线程分离） | Worker 单线程阻塞扫描期间不响应 STOP、不发心跳、NFS 卡死只能等 SIGKILL | Worker 内部分线程：Scanner 阻塞 IO + IPC 线程 poll 循环维持心跳与通信 |
| **v14.0.1** | **fd_out 互斥锁** | v14.0.0 拆分线程后 Scanner 与 IPC 线程并发写 fd_out 导致协议错乱、级联 payload timeout | `g_fd_out_mutex` 保护所有 fd_out 写入点 |

---

## v13.0.0：IPC 线程隔离

### 问题背景

v12.x 架构中，Master 主线程通过单线程 epoll 统一监听所有 8 个 Worker 的 `fd_out`。一个 Worker 的 fd 出问题（heartbeat 超时、D-State 卡死、fd 号重用竞争）会导致 epoll 反复返回 `EPOLLERR|EPOLLHUP`，污染整个 Master 事件循环，造成 Monitor 秒表冻结、CPU 空转、所有 Worker 假死。v12.2.15 修复了 cleanup_done 和日志问题，但单线程 epoll 的架构瓶颈仍然存在。

### 新架构核心

- **8 个 IPC 线程常驻**，生命周期与 Master 进程相同。每个 IPC 线程管理一个 Worker 的非阻塞 epoll + 心跳检测 + SIGKILL。
- **主线程 = 纯消息总线**，不再直接操作任何 fd、不再 epoll、不再 read/write。只负责：收消息（从 8 个返回队列轮询）、处理消息（BATCH 去重写文件、DEAD 收尾替换、ERROR 记日志）、发消息（SCAN 任务分发给 IPC 线程）。
- **故障隔离**：一个 Worker 的 fd 出问题 → 只污染它自己的 IPC 线程 → IPC 线程发 DEAD 消息 → 主线程收到后优雅替换 Worker → 其他 7 路完全不受影响。

### 消息队列

- **eventfd + 无锁环形队列**（64 位原子 CAS head/tail）。
- 默认容量 1024 条消息/队列，有界设计天然实现背压。
- 生产者（主线程/IPC 线程）：CAS 写尾指针 → 写入数据 → 写 eventfd 通知。
- 消费者：读 eventfd → CAS 读头指针 → 读取数据。
- 零 mutex、零上下文切换开销、支持 64 位原子操作。

### 消息格式

- **命令（主线程 → IPC 线程）**：
  - `CMD_SCAN`：发送 SCAN 任务路径给 Worker。
  - `CMD_REPLACE`：替换 Worker fd/pid（Worker 死亡后主线程 spawn 新 Worker，发此命令让 IPC 线程换新 fd）。
  - `CMD_STOP`：停止 IPC 线程。
- **返回（IPC 线程 → 主线程）**：
  - `RET_BATCH`：Worker 返回的扫描结果批次。
  - `RET_HEARTBEAT`：Worker 心跳（用于 Monitor 面板显示）。
  - `RET_ERROR`：Worker 遇到的设备级错误（ETIMEDOUT/EIO）。
  - `RET_DEAD`：Worker 死亡（heartbeat 超时或 epoll error/hup）。
  - `RET_EXIT`：Worker 正常退出。

### IPC 线程内部循环

```
while (running) {
    // 1. 从主线程消息队列取命令（非阻塞 drain）
    // 2. epoll_wait(fd_out + cmd_queue eventfd, 500ms)
    // 3. 处理 fd_out 事件（非阻塞 read → 解析 BATCH/HEARTBEAT/ERROR/EXIT）
    // 4. 心跳检测：超时 → SIGKILL Worker → 发 RET_DEAD → 等待 REPLACE
}
```

- 每个 IPC 线程有自己的小 epoll（2 个 fd：`fd_out` + `cmd_queue eventfd`）。
- fd 均为 `O_NONBLOCK`，read 采用 `poll(100ms)` 超时保护。
- Worker 死亡后 IPC 线程自己 close fd、epoll DEL，不需要主线程介入 cleanup。

### 主线程消息总线循环

```
while (running) {
    // 1. bus_epoll_wait(500ms) 监听所有 ret_queue eventfd + 线程池 event_fd
    // 2. 处理返回消息（BATCH → 线程池去重；DEAD → 替换 Worker；ERROR → 设备熔断）
    // 3. drain_completed_batches（线程池完成通知）
    // 4. 泵送历史 pbin 目录
    // 5. 收割僵尸进程
    // 6. 替换死亡 Worker（spawn → send REPLACE）
    // 7. dispatch_lost_tasks（通过 cmd_queue 发 CMD_SCAN）
    // 8. 终止条件检查
}
```

### Monitor 线程精简

- **删除 `check_workers_health`**：Worker 心跳超时检测完全下沉到 IPC 线程。
- **保留职责**：进度面板输出（秒表、速率、Worker 状态）、敢死队探测调度、探测进程收割。
- Monitor 不再直接操作 Worker fd 或发送 SIGKILL。

### 新增文件

- `include/msg_format.h` — 消息格式定义（CMD/RET 类型、Payload 结构体）。
- `include/msg_queue.h` — 无锁环形队列 API（eventfd 通知、CAS head/tail）。
- `src/msg_queue.c` — 无锁队列实现（64 位原子操作、select-based recv_wait）。
- `include/ipc_thread.h` — IPC 线程上下文和生命周期 API。
- `src/ipc_thread.c` — IPC 线程主循环（独立 epoll、心跳检测、消息处理）。

### 修改的文件

- `include/app_context.h` — 添加 IPC 线程数组、消息队列指针、主线程 cond/mutex。
- `include/config.h` — `VERSION "13.0.0"`。
- `include/main_loop.h` — 暴露 IPC 线程生命周期 API（init_ipc_threads、destroy_ipc_threads、send_replace_to_ipc）。
- `src/main_loop.c` — **重写**：从单线程 epoll 驱动改为消息总线驱动。保留 BATCH 去重、lost_tasks 派发、Worker 替换、设备熔断、历史 pbin 泵送等所有现有功能。
- `src/main.c` — 初始化 IPC 线程、发送初始 REPLACE、根任务改为通过 cmd_queue 发送 CMD_SCAN。
- `src/monitor.c` — 删除 Worker 心跳超时检测，保留进度面板和敢死队探测。
- `Makefile` — 自动包含新源文件（msg_queue.c、ipc_thread.c）。

---

## v13.0.1：启动崩溃修复

### 问题描述

v13.0.0 重构后的首次运行时出现两个独立阻断性问题：

1. `[IPC-0] payload timeout (len=42)` — 启动即死，IPC 线程在 `fd_out` 上读到孤悬 Header 后 poll payload 超时。
2. `无索引文件，历史块数 140734670153448` — `total_blocks` 打印出异常大值（`0x7FFF5805A6E8`，x86_64 栈地址特征），导致恢复逻辑走入错误分支。

### 根因 A：IPC 协议 Header/Payload 断裂

`ipc_send()` 原实现分两次 `write()`：
1. `write(fd, &hdr, sizeof(hdr))` — 写 8 字节 Header
2. `write(fd, payload, payload_len)` — 写 payload

第二次 `write()` 遇 `EAGAIN` 时，第一次的 Header 已留在 pipe 中。函数返回 `-2`，调用方（Worker 的 `scan_and_send`）重试时重新发送 Header + payload。结果：pipe 中出现多个孤悬 Header。IPC 线程读到第一个 Header 后等待 payload，永远等不到，poll 超时。

**修复**：`ipc_send()` 合并为单次原子写入。分配 `sizeof(IpcMessageHeader) + payload_len` 连续缓冲区，一次 `write()` 发送全部数据。对 SCAN 等小消息（<< `PIPE_BUF=4096`）天然原子，彻底消除 Header 孤悬问题。

### 根因 B：`total_blocks` 栈值污染 / `va_list` 传递异常

`count_archive_blocks()` 与 `count_pbin_slices()` 均显式初始化为 0，返回值理论上为 0。`0x7FFF5805A6E8` 是 x86_64 下未初始化栈内存 / `va_list` 因编译器优化/ABI 边界传递异常时读取到的栈残留值。非野指针，是栈值污染。

`verbose_printf` → `log_vraw` → `vfprintf` 的 `va_list` 跨编译单元传递在特定栈布局下可能读取错误位置。`log_vraw` 调用 `log_timestamp`（使用 `localtime_r`）后执行 `va_copy`，在某些编译器/优化级别下 `va_list` 的寄存器保存区可能被污染。

**修复**：
1. `verbose_printf` 和 `log_vraw` 添加 `__attribute__((noinline))`，防止编译器内联导致 `va_start` / `va_copy` 的寄存器保存区布局异常。
2. `restore_progress()` 增加 `total_blocks` sanity check：若 `total_blocks > 1000000000UL`，打印警告并强制置为 0。

### 根因 C：`compressed_size == 0` 未防呆

`count_archive_blocks()` 中若 `.archive` 文件损坏且 `bh.compressed_size == 0`，`fseek` 不移动，`fread` 原地重复读取同一 Header，`count` 无限增加。

**修复**：`count_archive_blocks()` 增加 `compressed_size == 0` 检查：`bh.compressed_size == 0 || bh.compressed_size > 512MB` 时 `break`。

### 修改的文件

- `src/worker_proc.c` — `ipc_send()` 合并写入
- `src/progress.c` — `count_archive_blocks` 防呆 + `total_blocks` sanity check
- `src/utils.c` — `verbose_printf` noinline
- `src/log.c` — `log_vraw` noinline
- `include/config.h` — 版本号 `13.0.1`

---

## 消息协议

### IPC 消息头（8 字节，packed）

```c
typedef struct __attribute__((packed)) {
    uint32_t msg_type;     // 消息类型
    uint32_t payload_len;  // payload 字节数
} IpcMessageHeader;
```

### Worker ↔ Master 消息类型

| 值 | 名称 | 方向 | 说明 |
|----|------|------|------|
| 1 | `IPC_MSG_SCAN` | M → W | 扫描任务，payload = 路径字符串 |
| 2 | `IPC_MSG_BATCH` | W → M | 扫描结果批次，payload = 序列化记录 |
| 3 | `IPC_MSG_HEARTBEAT` | W → M | 心跳，payload = 空 |
| 4 | `IPC_MSG_ERROR` | W → M | 设备错误，payload = 错误信息 |
| 5 | `IPC_MSG_EXIT` | W → M | 正常退出，payload = 空 |
| 6 | `IPC_MSG_STOP` | M → W | 停止 Worker，payload = 空 |
| 7 | `IPC_MSG_DEV_TIMEOUT` | W → M | Scanner 自检测超时，payload = 错误信息 |
| 7 | `IPC_MSG_DEV_TIMEOUT` | W → M | Scanner 自检测超时，payload = 错误信息 |

### IPC 线程 ↔ 主线程 消息类型

| 值 | 名称 | 方向 | 说明 |
|----|------|------|------|
| 1 | `CMD_SCAN` | Main → IPC | 发送 SCAN 任务 |
| 2 | `CMD_REPLACE` | Main → IPC | 替换 Worker fd/pid |
| 3 | `CMD_STOP` | Main → IPC | 停止 IPC 线程 |
| 11 | `RET_BATCH` | IPC → Main | Worker 返回批次 |
| 12 | `RET_HEARTBEAT` | IPC → Main | Worker 心跳 |
| 13 | `RET_ERROR` | IPC → Main | Worker 错误 |
| 14 | `RET_DEAD` | IPC → Main | Worker 死亡 |
| 15 | `RET_EXIT` | IPC → Main | Worker 正常退出 |
| 16 | `RET_DEV_TIMEOUT` | IPC → Main | Worker Scanner 自检测超时 |

---

## 故障隔离模型

```
Worker N 死亡
    │
    ▼
IPC Thread N 检测到 epoll error/hup 或 heartbeat 超时
    │
    ├── SIGKILL Worker（如需要）
    ├── close(fd_out), epoll DEL
    ├── 发 RET_DEAD → Main 的 ret_queue[N]
    │
    ▼
Main Thread 从 ret_queue[N] 收到 RET_DEAD
    │
    ├── worker_pool_replace() — spawn 新 Worker
    ├── send_replace_to_ipc(N, new_fd_in, new_fd_out, new_pid)
    │
    ▼
IPC Thread N 收到 CMD_REPLACE
    ├── close(old fd_in/fd_out)
    ├── 更新 fd_in/fd_out/pid
    ├── epoll ADD new fd_out
    └── 继续服务

其他 7 个 IPC 线程全程不受影响。
```

---

## v14.0.0：Worker 多线程化（Scanner 线程 + IPC 线程分离）

### 问题背景

v13.x 中 Worker 是单线程线性设计：阻塞读 fd_in → 阻塞扫描（readdir/lstat 可能卡住很久）→ 发 BATCH → 循环。扫描期间既不响应 STOP、也不发心跳，NFS 卡死时只能等 IPC 线程心跳超时后 SIGKILL。Worker 的 lstat 可能卡在 NFS soft timeout 上数分钟甚至数小时，期间既不响应 STOP、也不发心跳。

### 新架构核心

Worker 进程内部拆分为两条线程：
- **Scanner 线程**：专职执行 readdir/lstat 等阻塞 IO，调用 `scan_and_send()` 直接写 fd_out（阻塞）。
- **IPC 线程（主线程）**：专职维护与 Master 的通信。fd_in 设为非阻塞，通过 `poll(5s)` 循环同时处理读任务、发心跳、响应 STOP。Scanner 卡住不影响心跳节拍。

### Scanner 超时检测（双层超时）

Worker IPC 线程监控 Scanner 的 `last_progress` 时间戳：
- 若超过 `heartbeat_timeout`（默认 30s，`-t` 参数可调）无进展，发送 **`IPC_MSG_DEV_TIMEOUT`** 上报 Master
- Master 收到 **`RET_DEV_TIMEOUT`** 后直接按超时逻辑处置：`cleanup_dead_worker_slot(..., true)`（SIGKILL + 路径重发）
- Master 侧心跳超时检测仍然保留，作为最终兜底

### 新增/修改的文件

- `src/worker_proc.c`（WorkerThreadCtx + worker_scanner_thread + worker_main 多线程重写）
- `include/config.h`（版本号 14.0.0）

---

## v14.0.1：fd_out 互斥锁修复

### 问题背景

v14.0.0 拆分线程后，Scanner 线程与 IPC 线程并发往 `fd_out` 写数据，没有互斥保护。导致：
- IPC 线程的 HEARTBEAT header 插入到 Scanner 线程的 BATCH header 和 payload 之间
- Master IPC 线程读到 BATCH header（payload_len=1197）后，pipe 中只有 16 字节 HEARTBEAT 数据 → `safe_ipc_recv_payload(100ms)` 超时
- 8 个 Worker 同时触发，形成级联 payload timeout 风暴
- 主进程卡在 `futex_wait`（ret_queue 消息风暴导致某种阻塞）

### 修复

- 新增 `static pthread_mutex_t g_fd_out_mutex = PTHREAD_MUTEX_INITIALIZER`
- 所有往 `fd_out` 的 `ipc_send` 调用加锁保护：
  - `send_batch()`、`send_error_and_empty_batch()`
  - IPC 线程的心跳发送
  - `IPC_MSG_DEV_TIMEOUT` 上报
  - `IPC_MSG_EXIT` 发送

### 协议更新

- `ipc_protocol.h` 新增 `IPC_MSG_DEV_TIMEOUT`（7）
- `msg_format.h` 新增 `RET_DEV_TIMEOUT`（16）

### 修改的文件

- `src/worker_proc.c`（g_fd_out_mutex + 所有 fd_out 写入点加锁）
- `include/config.h`（版本号 14.0.1）

---

## v15.0.0：三通道分离 + IPC 状态机

### 问题背景

v14.0.x 中 Worker 拆分为 Scanner 线程 + IPC 线程后，两条线程并发写 `fd_out`，引入 `g_fd_out_mutex` 保护。但 mutex 持有者阻塞时（Scanner `write()` 被 pipe 满卡住），IPC 线程卡在 `pthread_mutex_lock` 上，心跳停止。同时多种语义消息（BATCH / HEARTBEAT / ERROR / EXIT / DEV_TIMEOUT）共享同一个 fd，字节交错导致 payload timeout 级联风暴。

### 新架构核心

每个 Worker 配置 **三个独立 fd**，语义分离：

| fd | 方向 | 语义 | 写入者 | 特性 |
|---|---|---|---|---|
| `fd_cmd[0/1]` | M→W | SCAN / STOP | Master | 阻塞写，非阻塞读 |
| `fd_data[0/1]` | W→M | BATCH（大 payload） | Scanner 线程 | 阻塞写，非阻塞读 |
| `fd_ctrl[0/1]` | W→M | HEARTBEAT / ERROR / EXIT / DEV_TIMEOUT / READY / FINISH | IPC 线程 | 阻塞写，非阻塞读 |

**关键约束**：
- `fd_data` 只有 Scanner 线程写，`fd_ctrl` 只有 IPC 线程写，**永不竞争**
- IPC 线程 epoll 监听 `fd_data + fd_ctrl + cmd_queue eventfd`
- `fd_ctrl` 消息长度均 < PIPE_BUF（4096），内核保证原子写入

### 新增 IPC 消息

| 值 | 消息 | 方向 | 说明 | 通道 |
|---|---|---|---|---|
| 8 | `IPC_MSG_READY` | W→M | Worker 初始化完成，进入主循环 | fd_ctrl |
| 9 | `IPC_MSG_FINISH` | W→M | 当前 SCAN 任务完成 | fd_ctrl |

### Master 侧 Worker 状态机

```
[UNSPAWNED]
   │
   │ spawn()
   ▼
[INITIALIZING] ──startup_timeout=60s──► [DEAD] → replace
   │
   │ ◄── RET_READY
   ▼
[IDLE] ──heartbeat_timeout=30s──► [DEAD] → replace
   │
   │ send CMD_SCAN
   ▼
[BUSY] ──heartbeat_timeout=30s──► [DEAD] → replace（IPC线程挂了）
   │      ──task_timeout=-t参数──► [DEAD] → replace（任务太久）
   │      ◄── IPC_MSG_DEV_TIMEOUT
   │      ◄── RET_DEV_TIMEOUT
   │
   │ ◄── RET_BATCH（可能有多个）
   │ ◄── RET_FINISH
   ▼
[IDLE]
```

**状态转换表**：

| 当前状态 | 触发条件 | 下一状态 | 动作 |
|---|---|---|---|
| INITIALIZING | startup_timeout | DEAD | SIGKILL + replace |
| INITIALIZING | RET_READY | IDLE | 开始心跳计时 |
| IDLE | CMD_SCAN | BUSY | 发送任务，开始 task_timeout |
| IDLE | heartbeat_timeout | DEAD | SIGKILL + replace |
| BUSY | RET_FINISH | IDLE | 停止 task_timeout，分发下一任务 |
| BUSY | heartbeat_timeout | DEAD | SIGKILL + replace |
| BUSY | task_timeout | DEAD | SIGKILL + replace |
| BUSY | RET_DEV_TIMEOUT | DEAD | SIGKILL + replace |
| BUSY | RET_ERROR | IDLE | 设备熔断，不替换 Worker |
| ANY | RET_EXIT | UNSPAWNED | 正常退出 |
| DEAD | cleanup + replace | INITIALIZING | spawn 新 Worker |

### Monitor 显示状态

每个 Worker 显示真实状态：
- `INIT`：INITIALIZING（刚 spawn，等 READY）
- `IDLE`：空闲，等待任务
- `BUSY`：正在扫描（显示 current_path 截断）
- `DEAD`：已判定死亡，等待替换

### 修改的文件

- `include/config.h` — 版本号 15.0.0
- `include/ipc_protocol.h` — 新增 IPC_MSG_READY / IPC_MSG_FINISH
- `include/msg_format.h` — 新增 RET_READY / RET_FINISH
- `include/worker_pool.h` / `src/worker_pool.c` — 三通道 pipe 创建（fd_cmd / fd_data / fd_ctrl）
- `src/ipc_thread.c` — epoll 监听 fd_data + fd_ctrl，READY/FINISH 转发
- `src/worker_proc.c` — Scanner 写 fd_data，IPC 写 fd_ctrl，移除 g_fd_out_mutex
- `src/main_loop.c` — 状态机实现（INITIALIZING/IDLE/BUSY/DEAD），READY/FINISH/DEV_TIMEOUT 处理
- `src/monitor.c` — 显示 Worker 真实状态

---

## 已知问题与待办

1. **write() 部分写入**：v13.0.1 将 `ipc_send()` 改为单次 `write()`，但非阻塞 pipe 的 `write()` 仍可能部分写入（返回正数 < 请求长度）。当前实现通过 `while` 循环处理部分写入，但 `EAGAIN` 时返回 `-2`。如果 `write()` 部分写入后下一次调用返回 `EAGAIN`，对端读到不完整数据。由于 SCAN 消息通常 << `PIPE_BUF=4096`，实践中很少触发。未来可考虑 `writev()` 或 `sendmsg()` 配合 `MSG_DONTWAIT`。

2. **Monitor 秒表依赖**：Monitor 的秒表计时基于 `gettimeofday()`，如果 IPC 线程或主线程 hang 死，Monitor 线程本身不受影响，但秒表反映的是"Wall Clock"而非"有效处理时间"。

3. **va_list 边界**：`noinline` 是 workaround 而非根治。如果未来遇到更多 `va_list` 相关问题，应考虑将 `verbose_printf` 的实现改为直接 `vsnprintf` 到栈缓冲区后输出，避免 `va_list` 跨函数传递。

4. **fpbin 转正中断**：如果 fpbin 转正（封口 + rename）过程中进程崩溃，下次启动时会重新执行完整转正流程，但已转正的 pbin 不会被覆盖（`find_max_pbin_index()` 防呆）。

5. **NFS 软挂载前提**：扫描 NFS 目录时，`soft,intr,timeo=600` 挂载选项是避免 D-State 不可杀死的唯一手段。`hard` 挂载下 `SIGKILL` 仍然无效。
