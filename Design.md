# Design.md — listfiles 架构设计文档

> 本文档记录 listfiles 的核心架构决策、版本演进与关键修复。与 README.md（用户指南）和 CHANGELOG.md（变更日志）互补。

---

## 版本

当前设计版本：**v15.4.3**（IPC FSM 续传 + 部分写入防御 + Shard 溢出修复 + 线程池安全上限）

---

## 目录

1. [架构演进](#架构演进)
2. [v13.0.0：IPC 线程隔离](#v1300ipc-线程隔离)
3. [v13.0.1：启动崩溃修复](#v1301启动崩溃修复)
4. [消息协议](#消息协议)
5. [故障隔离模型](#故障隔离模型)
6. [诊断与日志：版本化衰减](#诊断与日志版本化衰减)
7. [已知问题与待办](#已知问题与待办)

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
| **v15.0.0** | **三通道分离 + IPC 状态机** | v14.0.1 单 fd 多语义竞争：Scanner 与 IPC 线程共享 fd_out，mutex 持有者阻塞时心跳停止；多种消息字节交错导致 payload timeout | `fd_cmd` / `fd_data` / `fd_ctrl` 三通道语义分离，IPC 线程独立 epoll，Worker READY/FINISH 状态机 |
| **v15.0.1** | **重复初始化修复** | `main_loop_run()` 重复调用 `init_ipc_threads()` 导致两套消息队列，BATCH 消息发到第一套但主循环 drain 第二套，`pending_tasks` 永远不归零 | 删除 `main_loop_run()` 中的重复初始化，统一由 `main.c` 负责；补全 `skip_interval` 初始化；日志加 `flockfile` |
| **v15.0.2** | **计数器修复 + fd_cmd_rd 保留** | `process_completed_batch` 对每个 BATCH 都减 `pending_tasks`（单个 SCAN 可产生多个 BATCH），`dispatch_lost_tasks` 重发时不增，`RET_FINISH` 不减，`fd_cmd_rd` 被提前关闭 → `pending_tasks` 永久滞留，程序无法终止 | `process_completed_batch` 只负责 `pending_batches`；`dispatch_lost_tasks` 发 SCAN 时 +1；`RET_FINISH` 时 -1；保留 `cmd_pipe[0]` 给 cleanup drain 用 |
| **v15.0.3** | **阻塞写修复** | `fd_data`（Worker → Master）和 `fd_ctrl`（Worker → Master）在 Worker 侧为阻塞写；当 IPC 线程处理 cmd_queue 延迟时，Worker Scanner 线程写 fd_data 阻塞，不发 FINISH，pending_tasks 无法归零 | `fork` 前将 `data_pipe[1]` 和 `ctrl_pipe[1]` 设为 `O_NONBLOCK`，使 Worker 侧写操作在 EAGAIN 时 `usleep` 重试而非永久阻塞 |
| **v15.0.4** | **IPC 链路追踪** | v15.0.3 修复阻塞写后仍卡死，`pending_tasks=477` 不归零。根因未知，需定位 FINISH 在 Worker→IPC→ret_queue→主循环哪一环丢失 | 在 `read_ctrl_message` FINISH 分支、`send_return`、CMD_SCAN 成功路径增加 `log_info` 级追踪；Worker FINISH EAGAIN 超 1000 次打印 `log_warn` |
| **v15.1.0** | **Master Worker 状态机** | Master 不知道 Worker 在做什么，反复向卡死 Worker 发 SCAN；replace 后 cleanup 不运行 | IDLE / BUSY / DEAD 三状态 + `process_replace` 强制 cleanup |
| **v15.1.1** | **完整状态机** | INITIALIZING 缺失、startup_timeout 缺失、RET_ERROR 后状态恢复缺失、Monitor 无 Worker 显示 | 四状态 + 60s startup_timeout + RET_ERROR→WAITING + Monitor 独立 Worker 状态 |
| **v15.1.2** | **硬超时** | `batch_dedup_worker` 被 1000 万文件卡住，`pending_batches` 不归零 | `clock_gettime(CLOCK_MONOTONIC)` 硬超时 + `log_error` 强制退出 |
| **v15.1.3** | **Shard 无限循环防御** | `fp_shard_insert_internal` probe 超过 INT_MAX 次，`fpbin` 被覆盖 | `PROBE_LIMIT` + `capacity` sanity check + resize rollback |
| **v15.1.4** | **`process_completed_batch` 防御** | 队列状态不一致导致无限循环 | count sanity check + iteration hard stop |
| **v15.1.5** | **dispatch attempts 修复** | `while (attempts < num_workers)` 中 `continue` 不递增 `attempts`，CPU 100% 空转 | `continue` 前 `attempts++` |
| **v15.2.0** | **模块化重构完成** | 源码文件过大（>500行），职责混杂，维护困难 | 8 个 Phase 拆分：24 文件 → 32 文件，按 core/ipc/scan/output/util 职责边界组织 |
| **v15.3.0** | **版本化日志框架** | 高频追踪日志污染 stderr（987MB/3h） | `VERSION_CODE` + `_v(ver, ...)` 宏，与 `verbose_level` 正交 |
| **v15.4.0** | **IPC FSM 续传 + BATCH Footer 魔数** | `EAGAIN` 时跨 `epoll_wait` 调用丢失读取进度；BATCH 大数据无完整性校验 | `IpcReadFsm` 状态机 + `IPC_FOOTER_MAGIC` 数据完整性校验 |
| **v15.4.1** | **ipc_send 部分写入防御** | `MAX_PATH_LENGTH=4096` 导致 `total_len > PIPE_BUF`，非阻塞 pipe 部分写入后协议不同步 | `MAX_PATH_LENGTH` 4088 + 运行时 `PIPE_BUF` guard + 1000 次重试上限 |
| **v15.4.2** | **fp_shard_insert_internal 安全加固** | `expected_count*2` 溢出、`PROBE_LIMIT` 截断探测、rehash 失败无回滚 | 饱和乘法、`PROBE_LIMIT=capacity`、rehash 失败完整回滚 |
| **v15.4.3** | **thread_pool completed 链表安全** | `node` malloc 失败泄漏 batch、`completed` 链表自循环、`destroy` 无限 drain | malloc 失败释放 batch、自循环检测+断开、drain 安全上限 |
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
[DEAD/UNSPAWNED]
   │
   │ spawn()
   ▼
[INITIALIZING] ──startup_timeout=60s──► [DEAD] → replace
   │
   │ ◄── RET_READY
   ▼
[IDLE] ──heartbeat_timeout=30s──► [DEAD] → replace
   │
   │ send CMD_SCAN (only if STATE_IDLE)
   ▼
[BUSY] ──heartbeat_timeout=30s──► [DEAD] → replace（IPC线程挂了）
   │      ──task_timeout: Worker自检测DEV_TIMEOUT上报──► [DEAD] → replace
   │      ◄── IPC_MSG_DEV_TIMEOUT → RET_DEV_TIMEOUT
   │
   │ ◄── RET_BATCH（可能有多个）
   │ ◄── RET_FINISH
   ▼
[IDLE]
```

**状态常量**（实现命名）：
- `WORKER_STATE_INITIALIZING (3)`：刚 spawn，等 READY
- `WORKER_STATE_IDLE (0)`：可接收任务
- `WORKER_STATE_BUSY (1)`：已分配任务，等 FINISH
- `WORKER_STATE_DEAD (2)`：Worker 已死或正在替换

**状态转换表**：

| 当前状态 | 触发条件 | 下一状态 | 动作 |
|---|---|---|---|
| INITIALIZING | startup_timeout (60s) | DEAD | SIGKILL + replace |
| INITIALIZING | RET_READY | IDLE | 开始心跳计时 |
| IDLE | CMD_SCAN (选中且发送成功) | BUSY | 发送任务 |
| IDLE | heartbeat_timeout (30s) | DEAD | SIGKILL + replace |
| BUSY | RET_FINISH | IDLE | 停止 task_timeout |
| BUSY | RET_ERROR | IDLE | 设备熔断，不替换 Worker |
| BUSY | heartbeat_timeout (30s) | DEAD | SIGKILL + replace |
| BUSY | RET_DEV_TIMEOUT | DEAD | SIGKILL + replace（Worker自检测超时） |
| DEAD | cleanup + replace | INITIALIZING | spawn 新 Worker |

**任务超时说明**：Master 侧不独立维护 task_timeout 计时器。Worker Scanner 线程通过 `last_progress` 自检测停滞，超时发送 `IPC_MSG_DEV_TIMEOUT` → IPC 线程转发 `RET_DEV_TIMEOUT` → Master 按 DEAD 处理。

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

## 诊断与日志：版本化衰减

### 问题背景

v12.x ~ v15.x 期间为排查多次 P0 阻断性问题（fd 号重用死锁、payload timeout 级联、pending_tasks 不归零、IPC 线程卡死等），代码中引入了大量高频追踪日志。这些日志在排障阶段不可或缺，但在生产环境正常运行时每秒产生数千行，3 小时 41 分钟可累积 **987MB / 1000 万行**，严重污染 stderr 并拖慢 I/O。

传统 `verbose_level`（0~3）按**重要性**过滤：ERROR/FATAL 始终输出，INFO/DEBUG/TRACE 按级别遮蔽。但追踪日志与关键状态日志往往同级（均为 `log_info` 或 `log_debug`），导致无法单独关闭追踪而不影响状态观察。

### 设计：VERSION_CODE + 版本化阈值

引入第二个过滤维度——**引入版本**（`VERSION_CODE`），与 `verbose_level` 正交共存：

| 维度 | 控制粒度 | 作用 |
|------|---------|------|
| `verbose_level` | 粗（按级别） | 区分 ERROR / WARN / INFO / DEBUG / TRACE |
| `verbose_version` | 细（按版本） | 区分 "当前版本日志" vs "旧版本追踪日志" |

**核心规则**：
- `config.h` 中定义 `VERSION_CODE = YYYYMMDDHHMM`（如 `202605180903`），每次发布递增。
- 每条日志可带一个**版本标记**（通过 `log_info_v(ver, ...)` 宏）。
- 全局阈值 `g_log_version_threshold`：
  - **默认** = `VERSION_CODE`（不传 `--verbose-version` 时）。
  - 只输出 `ver >= threshold` 的日志。
  - 传 `--verbose-version=0` 时 `threshold = 0`，所有日志输出。
- 现有 `log_info(...)` 等便捷宏自动等价于 `log_info_v(VERSION_CODE, ...)`，新日志无需显式传版本。

**衰减策略**：
- 关键日志（启动、完成、Worker 替换、设备熔断、错误）标记为当前 `VERSION_CODE`，始终输出。
- 旧排障追踪日志（v15.0.x ~ v15.1.x 引入的 IPC 链路追踪、心跳、batch 处理细节）降级为 `log_*_v(202605150000, ...)`，默认被遮蔽。
- 需要排查历史问题时，传 `--verbose-version=202605150000` 精确打开该版本之后的所有日志。

### 与 verbose_level 的配合

```
生产运行（默认）：--verbose-level=0，不传 --verbose-version
  → 只输出 ERROR/FATAL + 关键状态变化（VERSION_CODE 标记）

日常观察：--verbose-level=1，不传 --verbose-version
  → + INFO 级关键日志（VERSION_CODE 标记）

问题排查：--verbose-level=2 --verbose-version=202605150000
  → + DEBUG 级 + v15.0.x 之后所有追踪日志

全开审计：--verbose-level=3 --verbose-version=0
  → 所有 TRACE + 所有历史版本日志
```

### 已知限制

- 旧日志的"引入版本"需要手动标记，无法自动追溯。但新追踪日志一旦完成使命，可在下次发布时批量降级版本号。
- `VERSION_CODE` 以"分钟"为粒度，同一分钟内多次提交的日志版本相同。实践中足够区分发布周期。

---

## v15.4.0：IPC FSM 续传 + BATCH Footer 魔数

### 问题背景

v15.0.x ~ v15.3.x 期间 IPC 线程使用一次性 `read()` 读取 Header 和 Payload。当 `fd_data`/`fd_ctrl` 的 `O_NONBLOCK` 读取遇 `EAGAIN` 时，函数返回 `-2`，调用方丢弃已读取的部分数据。下次 `epoll_wait` 返回 `EPOLLIN` 时，read 从上次断点继续，但调用方已丢失之前读到的部分 Header/Payload 字节，导致协议解析错乱（读到半个 Header 当成完整 Header，payload_len 异常）。

BATCH 消息（大 payload）尤其脆弱：Worker 发送数万条路径记录，单次 `epoll_wait` 内可能无法读完整个 payload。反复丢弃部分数据 → 协议无限失步 → IPC 线程 hang 死在 `payload timeout`。

### 设计：跨 `epoll_wait` 调用的可恢复状态机

引入 `IpcReadFsm` 结构体，为每个 fd 维护独立的读取状态：

```c
typedef enum {
    IPC_READ_IDLE,      // 空闲，等待新消息
    IPC_READ_HDR,       // 正在读取 8 字节 Header
    IPC_READ_PAYLOAD,   // 正在读取 payload（长度由 Header 指定）
    IPC_READ_FOOTER     // 正在读取 8 字节 Footer 魔数（仅 BATCH）
} IpcReadState;

typedef struct {
    IpcReadState state;     // 当前读取阶段
    size_t nread;           // 当前阶段已读取字节数
    IpcMessageHeader hdr;   // 已读取的 Header（HDR 阶段完成后有效）
    char *buf;              // payload 缓冲区（PAYLOAD 阶段分配）
} IpcReadFsm;
```

**关键约束**：
- `fd_ctrl`（控制通道）：两阶段 FSM（`HDR → PAYLOAD`），处理 HEARTBEAT/ERROR/EXIT/READY/FINISH。
- `fd_data`（数据通道）：三阶段 FSM（`HDR → PAYLOAD → FOOTER`），处理 BATCH 大数据。Footer 为 `uint64_t` 固定魔数 `0xDEADBEEF66AAC0FF`，接收端校验通过后才认为 BATCH 完整。
- `fsm_recv()` 通用续传原语：内部 `poll(100ms)` + `read`，`EAGAIN` 返回 `-2` 但**不释放 `buf`、不重置 `nread`**。调用方保存 FSM 状态，下次 `epoll_wait` 后继续同阶段读取。
- `CMD_REPLACE` 时彻底重置两个 FSM 状态为 `IPC_READ_IDLE`、`nread=0`、`free(buf)`，防止旧 Worker 的读取状态污染新连接的数据流。

### 防御性校验

- `ipc_msg_type_valid()` 白名单：只接受 `IPC_MSG_SCAN/BATCH/HEARTBEAT/ERROR/EXIT/STOP/DEV_TIMEOUT/READY/FINISH`，非法 `msg_type` 直接返回 false，避免异常大内存分配。
- `safe_ipc_recv_header_fsm()` 中 `hdr.payload_len > 100MB` 时 `log_fatal`，防止畸形 Header 导致超大 malloc。

### 修改的文件

- `include/ipc/ipc_protocol.h` — `IPC_FOOTER_MAGIC`、`IpcReadState`、`IpcReadFsm`、`fsm_recv`、`safe_*_fsm`、`ipc_msg_type_valid` 声明
- `include/ipc/ipc_thread.h` — `IpcThreadCtx` 增加 `ctrl_fsm`、`data_fsm`
- `src/ipc/ipc_protocol.c` — `fsm_recv` 实现、`safe_ipc_recv_header_fsm`、`safe_ipc_recv_payload_fsm`、`safe_ipc_recv_footer_fsm`、`ipc_msg_type_valid` 实现
- `src/ipc/ipc_thread.c` — `ipc_thread_ctx_create` FSM 初始化；`CMD_REPLACE` 彻底重置 FSM
- `src/ipc/ipc_message_handler.c` — `read_ctrl_message` / `read_data_message` 整块替换为 FSM 版本
- `src/scan/worker_scanner.c` — `send_batch` 追加 8 字节 Footer 魔数
- `include/core/config.h` — 版本号 15.4.0

---

## v15.4.1：ipc_send 部分写入防御

### 问题背景

`MAX_PATH_LENGTH` 原为 4096，与 `PIPE_BUF`（4096）相同。`ipc_send()` 中 `total_len = sizeof(IpcMessageHeader) + payload_len`，当 payload 含 4096 字节路径时，`total_len = 4104 > PIPE_BUF`。非阻塞 pipe 下内核不保证原子写入，`write()` 可能部分写入后返回正数或 `EAGAIN`，对端读到不完整数据 → 协议失步。

### 修复

- `config.h`: `MAX_PATH_LENGTH` 4096 → **4088**（`PIPE_BUF - sizeof(IpcMessageHeader) = 4096 - 8 = 4088`），确保所有 `fd_cmd` 消息 `total_len ≤ PIPE_BUF`，内核保证原子写入。
- `ipc_send()`: 运行时增加 `total_len > 4096` fatal guard，超限立即 `log_fatal` 退出，防止未来意外突破上限。
- `write()` 返回 `n == 0` 时防御性返回 `-1`（EOF/对端关闭）。
- `write()` 部分写入后遇 `EAGAIN`：增加 **1000 次重试上限**（每次 1ms `usleep`），防止 IPC 线程永久空转。

### 修改的文件

- `include/core/config.h` — `MAX_PATH_LENGTH` 4088、版本号 15.4.1
- `src/ipc/ipc_protocol.c` — `ipc_send` 运行时 guard + `n==0` 处理 + 1000 次重试上限

---

## v15.4.2：fp_shard_insert_internal 安全加固

### 问题背景

`fp_set_create()` 中 `expected_count * 2` 在 `expected_count > SIZE_MAX/2` 时溢出，导致 per_shard 容量计算为极小值，后续插入触发无限 resize 循环。

`PROBE_LIMIT = 1000000` 为任意常数，当 `shard->capacity < PROBE_LIMIT` 时，探测循环在 `i >= capacity` 时本应自然结束，但 `PROBE_LIMIT` 截断了这一逻辑，反而在扩容后 rehash 时可能导致误判。

rehash 失败（如 `fp_shard_insert_internal` 递归调用返回 false）时，旧 table 已被覆盖为新分配的空白 table，数据永久丢失。

### 修复

- `fp_set_create()`: `expected_count * 2` 增加溢出饱和：`expected_count > (SIZE_MAX / 4)` 时返回 NULL，防止 per_shard 计算为 0 或极小值。
- `fp_shard_insert_internal()`: `PROBE_LIMIT = 1000000` → **`shard->capacity`**，探测上限与物理容量绑定，消除截断。
- rehash 失败时完整回滚：保存 `old_count`、`old_tombstones`，rehash 失败时 `free(new_meta/table)` 并恢复旧指针、旧 capacity、旧 count/tombstones，数据零丢失。

### 修改的文件

- `src/scan/fingerprint_set.c` — 饱和乘法、`PROBE_LIMIT` 动态化、rehash 回滚
- `include/core/config.h` — 版本号 15.4.2

---

## v15.4.3：thread_pool completed 链表安全

### 问题背景

`worker_thread()` 中 `malloc(sizeof(CompletedNode))` 失败时原实现打印 `log_fatal` 后**继续循环但不释放 batch**，导致 batch 内存泄漏且主线程永远收不到该 batch 的完成通知，`pending_batches` 不归零。

`thread_pool_poll_completed()` 无链表完整性校验，若 `node->next` 因内存 corruption 指向自身（自循环），`tp->completed_head = node->next` 后 head 不变，`while` 循环在主线程中永久空转。

`thread_pool_destroy()` 中 `while ((batch = thread_pool_poll_completed(tp)) != NULL)` 无上限，若 completed 链表因 corruption 形成循环，销毁时永久阻塞。

### 修复

- `worker_thread()`: `node` malloc 失败时**释放 batch 全部内存**（`paths[i]`、`paths`、`stats`、`results`、`batch`）并 `continue`，防止泄漏。
- `thread_pool_poll_completed()`: 增加自循环检测 — 若 `node->next == node`，`log_fatal` 断开循环（`node->next = NULL`）；增加遍历上限 100000，超限 `log_fatal` 断开。
- `thread_pool_destroy()`: drain 循环增加安全上限 `drained_count < 100000`，超限 `log_fatal` 退出，防止销毁时无限阻塞。

### 修改的文件

- `src/scan/thread_pool.c` — node-oom 释放 batch、自循环检测、drain 上限
- `include/core/config.h` — 版本号 15.4.3

---

## 已知问题与待办

1. ~~write() 部分写入~~：**v15.4.1 已修复**。`MAX_PATH_LENGTH` 降至 4088（≤`PIPE_BUF`），运行时增加 `total_len > PIPE_BUF` fatal guard，1000 次重试上限防止 IPC 线程空转。

2. **Monitor 秒表依赖**：Monitor 的秒表计时基于 `gettimeofday()`，如果 IPC 线程或主线程 hang 死，Monitor 线程本身不受影响，但秒表反映的是"Wall Clock"而非"有效处理时间"。

3. **va_list 边界**：`noinline` 是 workaround 而非根治。如果未来遇到更多 `va_list` 相关问题，应考虑将 `verbose_printf` 的实现改为直接 `vsnprintf` 到栈缓冲区后输出，避免 `va_list` 跨函数传递。

4. ~~`fp_shard_insert_internal` 内存 corruption~~：**v15.4.2 已修复**。`expected_count*2` 饱和防溢出、`PROBE_LIMIT=capacity` 消除截断、rehash 失败完整回滚。若仍有 `log_fatal` 触发，需进一步审计 `batch->results` / `batch->paths` 越界写、`parse_batch` 边界、`ipc_recv` 完整性、以及 `async_writer` 是否可能越界写 `visited_set` 所在内存。

5. **fpbin 转正中断**：如果 fpbin 转正（封口 + rename）过程中进程崩溃，下次启动时会重新执行完整转正流程，但已转正的 pbin 不会被覆盖（`find_max_pbin_index()` 防呆）。

6. **NFS 软挂载前提**：扫描 NFS 目录时，`soft,intr,timeo=600` 挂载选项是避免 D-State 不可杀死的唯一手段。`hard` 挂载下 `SIGKILL` 仍然无效。
