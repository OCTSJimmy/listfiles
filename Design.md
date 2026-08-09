# listfiles 架构设计文档

> 文档版本：v15.5.9  
> 最后更新：2026-08-09  
> 对应代码版本：`dev` 分支 `c83c1b9`  

---

## 目录

1. [概述](#1-概述)
2. [设计目标与约束](#2-设计目标与约束)
3. [高层架构](#3-高层架构)
   - 3.1 [模块拓扑](#31-模块拓扑)
   - 3.2 [SEDA 五阶段流水线](#32-seda-五阶段流水线)
   - 3.3 [数据流](#33-数据流)
4. [运行时模型](#4-运行时模型)
   - 4.1 [进程与线程模型](#41-进程与线程模型)
   - 4.2 [Worker 状态机](#42-worker-状态机)
   - 4.3 [IPC 线程循环](#43-ipc-线程循环)
   - 4.4 [主线程消息总线](#44-主线程消息总线)
5. [模块详细设计](#5-模块详细设计)
   - 5.1 [core — 配置与生命周期](#51-core--配置与生命周期)
   - 5.2 [ipc — 进程间通信](#52-ipc--进程间通信)
   - 5.3 [scan — 扫描引擎](#53-scan--扫描引擎)
   - 5.4 [output — 输出与监控](#54-output--输出与监控)
6. [通信协议](#6-通信协议)
   - 6.1 [三通道语义分离](#61-三通道语义分离)
   - 6.2 [IPC 消息格式](#62-ipc-消息格式)
   - 6.3 [Master ↔ IPC 线程消息](#63-master--ipc-线程消息)
   - 6.4 [FSM 续传协议](#64-fsm-续传协议)
7. [数据持久化模型](#7-数据持久化模型)
   - 7.1 [pbin — 进度分片](#71-pbin--进度分片)
   - 7.2 [dpbin — 完成日志](#72-dpbin--完成日志)
   - 7.3 [fpbin — 恢复临时缓存](#73-fpbin--恢复临时缓存)
   - 7.4 [spbin — 跳过记录](#74-spbin--跳过记录)
   - 7.5 [dspill — 派发兜底](#75-dspill--派发兜底)
   - 7.6 [archive — 压缩归档](#76-archive--压缩归档)
8. [故障处理与容错](#8-故障处理与容错)
   - 8.1 [Worker 死亡与替换](#81-worker-死亡与替换)
   - 8.2 [设备级熔断](#82-设备级熔断)
   - 8.3 [目录级熔断与退避](#83-目录级熔断与退避)
   - 8.4 [NFS 大目录防误判](#84-nfs-大目录防误判)
   - 8.5 [扫描完整性断言](#85-扫描完整性断言)
9. [性能考量](#9-性能考量)
10. [安全考量](#10-安全考量)
11. [部署与运维](#11-部署与运维)
12. [附录：版本演进摘要](#12-附录版本演进摘要)

---

## 1. 概述

`listfiles` 是一个面向 **PB 级分布式存储 / 十亿级文件** 场景的高性能递归目录扫描工具。核心功能是将目录树遍历结果（路径、stat 元数据、xattr 等）输出为 CSV 或自定义格式文本，同时支持断点续传、半增量扫描、设备熔断、进度归档等生产级特性。

运行环境默认为 **NFS 挂载的分布式存储**（曙光 ParaStor、Ceph 等），网络抖动、设备离线、元数据节点过载是常态而非异常。因此设计以 **容错优先、可观测性优先** 为第一原则。

---

## 2. 设计目标与约束

### 2.1 设计目标

| 目标 | 说明 |
|------|------|
| **一次运行完成** | 不依赖 `--continue` 续传也能在合理时间内完成全部扫描 |
| **断点续传** | 异常终止后可从上次进度恢复，不重复扫描已完成目录 |
| **半增量扫描** | 基于 mtime + fingerprint 跳过未变更目录，减少重复 I/O |
| **设备故障隔离** | 单个存储节点/挂载点故障不拖垮整个扫描任务 |
| **可观测性** | 实时监控进度、Worker 状态、设备健康度、预估剩余时间 |
| **资源可控** | 内存占用有界（不随目录树规模线性膨胀），CPU 不空转 |

### 2.2 核心约束

| 约束 | 来源 |
|------|------|
| **NFS soft,intr,timeo=600 挂载** | hard 挂载下 D-State 不可杀，Worker 僵死后无法替换 |
| **同机运行** | IPC 协议中 `struct stat` 直接 `memcpy` 序列化，不跨机器/不跨架构 |
| **CentOS 7.4 / Bash 4.2 兼容** | 生产环境内核版本锁定 |
| **单文件 6000 万条目是常态** | 任何数据结构必须在此规模下稳定工作 |
| **25PB / 12 亿文件是基线** | 不是极端场景，是默认设计规模 |

---

## 3. 高层架构

### 3.1 模块拓扑

代码库按职责拆分为 5 个顶层目录、32 个模块、约 8000 行源码（含注释）：

```
include/          src/
├── core/         ├── core/
│   ├── app_context.h         main.c            (生命周期编排)
│   ├── config.h              cmdline.c         (命令行解析)
│   ├── cmdline.h             signals.c         (信号处理)
│   ├── signals.h             utils.c           (通用工具)
│   ├── utils.h               circuit_breaker.c (目录级熔断)
│   └── circuit_breaker.h
├── ipc/          ├── ipc/
│   ├── ipc_protocol.h        ipc_protocol.c        (TLV 协议封装)
│   ├── ipc_thread.h          ipc_thread.c          (IPC 线程主循环)
│   ├── msg_format.h          ipc_message_handler.c (消息接收处理)
│   ├── msg_queue.h           ipc_worker_mgmt.c     (Worker 生命周期)
│   └── worker_proc.h         worker_proc.c         (进程池管理)
│                             msg_queue.c           (无锁队列)
├── scan/         ├── scan/
│   ├── main_loop.h           main_loop.c        (主线程消息总线)
│   ├── dispatch_queue.h      dispatch.c         (任务分发)
│   ├── device_manager.h      dispatch_queue.c   (队列实现)
│   ├── fingerprint_set.h     batch_processor.c  (Batch 去重)
│   ├── reference_map.h       device_manager.c   (设备管理)
│   ├── probe_scheduler.h     probe_scheduler.c  (探测调度)
│   ├── thread_pool.h         thread_pool.c      (去重线程池)
│   └── worker_scanner.h      worker_scanner.c   (扫描引擎)
├── output/       ├── output/
│   ├── output.h              output.c           (输出渲染)
│   ├── progress.h            progress.c         (进度核心)
│   ├── async_worker.h        progress_io.c      (进度 IO)
│   ├── monitor.h             progress_archive.c (进度归档)
│   ├── archive_format.h      output_format.c    (格式预编译)
│   └── spbin.h               output_metadata.c  (元数据缓存)
│                             async_worker.c     (异步输出)
│                             monitor.c          (监控面板)
└── util/         └── util/
    ├── log.h                   log.c            (日志)
    └── xxhash.h                xxhash.c         (哈希)
```

### 3.2 SEDA 五阶段流水线

架构采用 **SEDA（Staged Event-Driven Architecture）** 将扫描流程解耦为五个阶段，每阶段通过队列/事件解耦：

```
Stage 1: 目录发现    Stage 2: 子目录枚举      Stage 3: Batch 处理      Stage 4: 任务分发      Stage 5: 输出写入
┌─────────┐         ┌─────────────┐         ┌──────────────┐         ┌─────────────┐         ┌─────────────┐
│ 根目录  │ ──SCAN──>│ Worker      │ ──BATCH─>│ batch_       │ ──push──>│ dispatch_    │ ──pop───>│ async_      │
│ 入队    │         │ Scanner     │         │ processor    │         │ queue        │         │ worker      │
│         │         │ (readdir/   │         │ (CPU去重/    │         │ (Stage3→4    │         │ (写文件/    │
│         │         │  lstat)     │         │  写pbin)     │         │  队列)       │         │  归档)      │
└─────────┘         └─────────────┘         └──────────────┘         └─────────────┘         └─────────────┘
     ▲                                                                    │
     │                                                                    │
     └──────────────── 历史 pbin 泵送（恢复时）─────────────────────────────┘
```

**关键设计决策**：
- Stage 2 与 Stage 3 之间通过 **IPC 管道** 解耦（跨进程）。
- Stage 3 与 Stage 4 之间通过 **`DispatchQueue`** 解耦（同进程内存队列，带背压）。
- Stage 5 由独立 **异步输出线程** 执行，不阻塞主循环。

### 3.3 数据流

```
输入：目录路径（来自命令行 / pbin 恢复 / dispatch_queue 重发）
  │
  ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           Master 进程（单线程消息总线）                        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐ │
│  │ 主循环        │  │ dispatch_queue│  │ 线程池去重    │  │ 异步输出线程      │ │
│  │ (消息路由)    │  │ (任务缓冲)    │  │ (CPU 并行)    │  │ (写文件)         │ │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └──────────────────┘ │
│         │                 │                 │                                │
│         ▼                 ▼                 ▼                                │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐                        │
│  │ IPC Thread 0 │  │ IPC Thread 1 │  │ ...          │  IPC Threads (8路常驻)  │
│  │ (epoll+心跳) │  │ (epoll+心跳) │  │ IPC Thread 7 │                        │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘                        │
└─────────┼─────────────────┼─────────────────┼────────────────────────────────┘
          │                 │                 │
          ▼                 ▼                 ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           Worker 进程 (per IPC Thread)                       │
│  ┌─────────────────┐    ┌─────────────────┐                                 │
│  │ Scanner 线程     │    │ IPC 线程         │                                 │
│  │ (readdir/lstat) │───>│ (poll 5s 心跳)   │                                 │
│  │ 写 fd_data      │    │ 写 fd_ctrl       │                                 │
│  └─────────────────┘    └─────────────────┘                                 │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 4. 运行时模型

### 4.1 进程与线程模型

| 层级 | 数量 | 职责 | 生命周期 |
|------|------|------|----------|
| **Master 进程** | 1 | 消息总线、任务调度、进度管理、监控 | 整个运行期 |
| **IPC 线程** | 8（固定） | 每 Worker 一个，独立 epoll + 心跳 + SIGKILL | 与 Master 同寿 |
| **Worker 进程** | 8（默认） | 执行实际扫描 | 动态替换 |
| **Scanner 线程** | 8（每 Worker 一个） | 阻塞 IO（readdir/lstat） | 与 Worker 同寿 |
| **去重线程池** | 4（默认） | CPU 密集型 batch 去重 | 与 Master 同寿 |
| **异步输出线程** | 1 | 写输出文件 / 进度文件 | 与 Master 同寿 |
| **Monitor 线程** | 1 | 秒表面板、探测调度、进程收割 | 与 Master 同寿 |

**关键约束**：
- IPC 线程与 Worker 进程 **1:1 绑定**，Worker 替换时 IPC 线程不换，只更新 fd/pid。
- Scanner 线程与 IPC 线程 **同进程但不同线程**，共享地址空间，通过 `last_progress` 时间戳 + mutex 同步。

### 4.2 Worker 状态机

Master 侧维护每个 Worker slot 的显式状态机：

```
                    spawn()
[DEAD/UNSPAWNED] ──────────────> [INITIALIZING]
                                        │
                                        │ RET_READY (60s startup_timeout 内)
                                        ▼
                                [IDLE] ──────────────> [DEAD]  (heartbeat_timeout)
                                  │                           SIGKILL + replace
                                  │ CMD_SCAN (send success)
                                  ▼
                                [BUSY] ──────────────> [DEAD]  (heartbeat_timeout)
                                  │      RET_DEV_TIMEOUT         SIGKILL + replace
                                  │      (Scanner self-detected)
                                  │
                                  │ RET_BATCH (multiple)
                                  │ RET_FINISH
                                  ▼
                                [IDLE]
                                  │
                                  │ RET_ERROR
                                  ▼
                                [IDLE]  (device fuse, no replace)
```

**状态常量**：
- `WORKER_STATE_INITIALIZING (3)`：刚 spawn，等待 READY
- `WORKER_STATE_IDLE (0)`：可接收任务
- `WORKER_STATE_BUSY (1)`：已分配任务，等待 FINISH
- `WORKER_STATE_DEAD (2)`：已死亡或正在替换

**状态转换表**：

| 当前状态 | 触发条件 | 下一状态 | 动作 |
|---------|---------|---------|------|
| INITIALIZING | startup_timeout (60s) | DEAD | SIGKILL + replace |
| INITIALIZING | RET_READY | IDLE | 开始心跳计时 |
| IDLE | CMD_SCAN (发送成功) | BUSY | pending_tasks++ |
| IDLE | heartbeat_timeout (120s) | DEAD | SIGKILL + replace |
| BUSY | RET_FINISH | IDLE | pending_tasks-- |
| BUSY | RET_ERROR | IDLE | device fuse，不替换 |
| BUSY | heartbeat_timeout (120s) | DEAD | SIGKILL + replace |
| BUSY | RET_DEV_TIMEOUT | DEAD | SIGKILL + replace |
| DEAD | cleanup + replace | INITIALIZING | spawn 新 Worker |

### 4.3 IPC 线程循环

每个 IPC 线程独立运行以下循环：

```
while (running) {
    // 1. 非阻塞 drain 主线程命令队列 (CMD_SCAN / CMD_REPLACE / CMD_STOP)
    // 2. epoll_wait(fd_data + fd_ctrl + cmd_queue_eventfd, 500ms)
    // 3. 处理 fd_data 事件：FSM 续传读取 BATCH → 完整后 send_return(RET_BATCH)
    // 4. 处理 fd_ctrl 事件：FSM 续传读取 HEARTBEAT/ERROR/EXIT/READY/FINISH → 转发到 ret_queue
    // 5. 心跳检测：last_heartbeat > heartbeat_timeout ? SIGKILL + send_return(RET_DEAD)
}
```

**故障隔离**：一个 Worker 的 fd 出问题只污染它自己的 IPC 线程，其他 7 路完全不受影响。

### 4.4 主线程消息总线

```
while (running) {
    // 1. bus_epoll_wait(500ms) 监听所有 ret_queue eventfd + thread_pool event_fd
    // 2. 处理返回消息：
    //    - RET_BATCH  → thread_pool 提交去重
    //    - RET_FINISH → pending_tasks--, Worker→IDLE
    //    - RET_DEAD   → cleanup + spawn + send_replace_to_ipc
    //    - RET_ERROR  → device_mgr_mark_dead / probe_scheduler_push
    //    - RET_DEV_TIMEOUT → 同 RET_DEAD
    //    - RET_READY  → Worker→IDLE
    // 3. drain_completed_batches（线程池完成回调）
    // 4. 泵送历史 pbin 目录（恢复时）
    // 5. 收割僵尸进程
    // 6. dispatch_from_queue（派发 dispatch_queue 中的任务）
    // 7. 终止条件检查：pending_tasks==0 && dispatch_queue_count==0 && 无历史可泵送
}
```

---

## 5. 模块详细设计

### 5.1 core — 配置与生命周期

#### Config（全局配置）

```c
typedef struct {
    char *target_path;           // -p 扫描根路径
    char *output_file;           // -o 输出文件
    char *output_split_dir;      // -O 分片输出目录
    bool continue_mode;          // -c 断点续传
    long skip_interval;          // --skip-interval 半增量阈值（秒）
    bool archive;                // -Z 压缩归档
    char *progress_base;         // -f 进度文件前缀
    char *format;                // -F 输出格式字符串
    bool csv;                    // --csv 严格 CSV 模式
    bool strict_nlink;           // --strict-nlink nlink oracle（v15.5.8）
    int batch_size;              // --batch-size Worker batch 大小（默认 1024）
    unsigned long estimated_files; // --estimated-files HashSet 预分配（默认 1000 万）
    int master_threads;          // Master 去重线程数（默认 4）
    int worker_count;            // Worker 进程数（0=自动，上限 8）
    int heartbeat_timeout;       // -t 心跳超时（默认 120s，v15.5.9）
    unsigned long verbose_version; // --verbose-version 日志版本阈值
    // ... 其他字段见 config.h
} Config;
```

#### AppContext（运行时上下文）

`AppContext` 是贯穿整个生命周期的统一上下文，取代旧版全局变量。核心字段：

- **去重集合**：`visited_set`（本次防环）、`completed_set`（dpbin 恢复时）、`reference_set` / `reference_map`（半增量）
- **进程管理**：`worker_pool`、`probe_scheduler`、`dev_mgr`
- **事件循环**：`epfd`、`dispatch_queue`
- **IPC 基础设施**：`ipc_cmd_queues[8]`、`ipc_ret_queues[8]`、`ipc_threads[8]`
- **任务计数**：`pending_tasks`（原子）、`pending_batches`（原子）
- **进度文件**：`hist_pump_fp`、`fpbin_slice_file`、`dpbin_slice_file`、`dspill_fp`
- **熔断状态**：`timeout_paths[8][4096]`、`timeout_counts[8]`、`redispatch_backoff_until[8]`

### 5.2 ipc — 进程间通信

#### 三通道 Pipe 模型（v15.0.0）

每个 Worker 拥有三个独立 pipe，彻底消除 Scanner 线程与 IPC 线程的写竞争：

| 通道 | 方向 | 语义 | 写入者 | 读取者 | 特性 |
|------|------|------|--------|--------|------|
| `fd_cmd` | M→W | SCAN / STOP | Master | IPC 线程 | 阻塞写，非阻塞读 |
| `fd_data` | W→M | BATCH（大 payload） | Scanner 线程 | IPC 线程 | 阻塞写，非阻塞读 |
| `fd_ctrl` | W→M | HEARTBEAT / ERROR / EXIT / READY / FINISH / DEV_TIMEOUT | IPC 线程 | IPC 线程 | 阻塞写，非阻塞读，消息 < PIPE_BUF |

#### 无锁消息队列（v13.0.0）

Master ↔ IPC 线程之间通过 **eventfd + 无锁环形队列** 通信：

- 容量：1024 条/队列（有界，天然背压）
- 原子操作：64 位 CAS head/tail，零 mutex
- 消息结构：`IpcThreadMsg { type, slot_id, data*, data_len }`

#### IPC FSM 续传（v15.4.0）

跨 `epoll_wait` 调用的可恢复读取状态机，解决 `EAGAIN` 时数据丢失问题：

```c
typedef enum {
    IPC_READ_IDLE,      // 空闲
    IPC_READ_HDR,       // 读取 8 字节 Header
    IPC_READ_PAYLOAD,   // 读取 payload
    IPC_READ_FOOTER     // 读取 8 字节 Footer 魔数（仅 BATCH）
} IpcReadState;
```

- `fsm_recv()`：内部 `poll(100ms) + read`，`EAGAIN` 返回 `-2` 但**不释放 buf、不重置 nread**
- `CMD_REPLACE` 时彻底重置 FSM，防止旧 Worker 状态污染新连接
- BATCH Footer 魔数：`0xDEADBEEF66AAC0FF`，校验完整性

### 5.3 scan — 扫描引擎

#### Worker Scanner（worker_scanner.c）

Scanner 线程执行实际的目录遍历：

```
opendir(path) → readdir 循环 → lstat 每个条目 → 区分文件/目录
  ├── 文件：收集 path + stat → 满 batch_size 时 send_batch(fd_data)
  ├── 目录：record_path_batch_append() → 由 batch_processor 后续处理
  └── 特殊文件（ symlink / device 等）：按配置处理
```

**关键优化**：
- **盲信（blind-trust）**：对于已存在于 `reference_map` 中的文件，若 mtime 未变且 `d_type == DT_REG`，跳过 `lstat`，减少 I/O。
- **目录排除**：`DT_DIR` 始终走 `lstat`，不盲信（v15.5.0），防止 mtime 不可靠导致子树丢失。
- **时间驱动心跳**：`scanner_progress_tick()` 每 5 秒更新 `last_progress`（v15.5.9），替代旧版的每 1000 条目计数驱动。
- **opendir/send_batch 前后 tick**：防止 NFS RPC 阻塞期间被误判卡死。

#### Batch Processor（batch_processor.c）

Stage 3 的核心模块，由主线程调用：

1. 解析 Worker 返回的 BATCH 数据
2. CPU 去重：计算 `path + dev + ino` 的 128-bit MD5 fingerprint，查询 `visited_set`
3. 新目录：写入 pbin（持久化）+ `dispatch_queue_push()`（Stage 4 输入）
4. 新文件：通过 `record_path_batch_append()` 缓冲，满 4096 条或 1MB 时刷盘

**背压机制**（v15.5.2）：
- `dispatch_queue.count > DISPATCH_QUEUE_HIGH_WATER (10万)`：batch_processor 停止 push，目录继续写入 pbin
- `dispatch_queue.count < DISPATCH_QUEUE_LOW_WATER (3万)`：主循环触发 `load_dirs_from_pbin()`，从 pbin cursor 加载 5 万条回填 queue

#### Dispatch（dispatch.c）

Stage 4 任务分发：

```
dispatch_from_queue():
  while dispatch_queue 非空:
    轮询找 IDLE Worker
    若该 Worker 处于 redispatch_backoff_until 退避期 → skip
    send_scan_to_ipc() → CMD_SCAN → pending_tasks++
```

**cleanup_dead_worker_slot()**：
- Worker 死亡时，若当前任务未失败（非正常 FINISH），将 `current_path` requeue 回 dispatch_queue
- 目录级熔断检查：同一路径连续 DEV_TIMEOUT 超过 `CIRCUIT_BREAKER_THRESHOLD (10)` 则不再重试
- 指数退避：1 次→30s、2 次→120s、3 次+→300s（v15.5.9）

#### Device Manager（device_manager.c）

设备级熔断管理：

| 状态 | 含义 | 转换条件 |
|------|------|----------|
| `DEV_STATE_NORMAL` | 正常 | 初始状态 |
| `DEV_STATE_PROBING` | 正在探测（嫌疑） | 首次 ERROR 后进入 |
| `DEV_STATE_DEAD` | 已熔断（黑名单） | 探测失败 |
| `DEV_STATE_CONDEMNED` | 已判死（永久跳过） | `PROBE_MAX_RETRIES` 次探测失败后 |

- 无锁读路径：`dev_mgr_get_state()` 使用 `_Atomic` 读取
- 渐进探测调度器：`probe_scheduler.c`，指数退避（5s→10s→20s→...→300s）

### 5.4 output — 输出与监控

#### 输出渲染（output.c / output_format.c / output_metadata.c）

支持两种输出模式：

| 模式 | 说明 |
|------|------|
| **单文件** | `-o output.txt`，全量写入一个文件 |
| **分片** | `-O output_split/`，每 `output_slice_lines` 行自动切分新文件 |

格式字符串预编译：`compile_format()` 将 `-F "{path},{size},{mtime}"` 解析为 `FormatSegment[]` 数组，运行时直接遍历渲染，避免反复解析。

#### 异步输出线程（async_worker.c）

- 线程安全任务队列（mutex + cond）
- 输出文件 8MB 全缓冲（`setvbuf(..., _IOFBF, 8*1024*1024)`）
- 批量刷盘，减少 `write` 系统调用次数

#### Monitor 面板（monitor.c）

500ms 刷新一次，输出格式：

```
[MM:SS] dirs: 12345 files: 67890 rate: 123.4/s active: 8/8 pending: 47 devs: 1 probing 0 dead
```

- `TERM=dumb` 时禁用 ANSI 转义，兼容管道/日志文件
- 显示每个 Worker 的真实状态（IDLE/BUSY/DEAD + 当前路径）

---

## 6. 通信协议

### 6.1 三通道语义分离

```
Master                                    Worker
   │                                       │
   ├──── fd_cmd ────> [SCAN path]          │
   ├──── fd_cmd ────> [STOP]               │
   │                                       │
   │<──── fd_data ─── [BATCH records]      │ Scanner 线程
   │<──── fd_data ─── [BATCH records]      │
   │                                       │
   │<──── fd_ctrl ─── [HEARTBEAT]          │ IPC 线程
   │<──── fd_ctrl ─── [READY]              │
   │<──── fd_ctrl ─── [FINISH]             │
   │<──── fd_ctrl ─── [ERROR errno dev path]│
   │<──── fd_ctrl ─── [DEV_TIMEOUT]        │
   │<──── fd_ctrl ─── [EXIT]               │
```

### 6.2 IPC 消息格式

**Header（8 字节，packed）**：

```c
typedef struct __attribute__((packed)) {
    uint32_t msg_type;     // 消息类型
    uint32_t payload_len;  // payload 字节数
} IpcMessageHeader;
```

**Worker → Master 消息类型**：

| 值 | 名称 | 通道 | 说明 |
|----|------|------|------|
| 1 | `IPC_MSG_SCAN` | fd_cmd | M→W 扫描任务 |
| 2 | `IPC_MSG_BATCH` | fd_data | W→M 扫描结果批次 |
| 3 | `IPC_MSG_HEARTBEAT` | fd_ctrl | 心跳 |
| 4 | `IPC_MSG_ERROR` | fd_ctrl | 设备级错误 |
| 5 | `IPC_MSG_EXIT` | fd_ctrl | 正常退出 |
| 6 | `IPC_MSG_STOP` | fd_cmd | M→W 停止 |
| 7 | `IPC_MSG_DEV_TIMEOUT` | fd_ctrl | Scanner 自检测超时 |
| 8 | `IPC_MSG_READY` | fd_ctrl | Worker 初始化完成 |
| 9 | `IPC_MSG_FINISH` | fd_ctrl | 当前任务完成 |
| 10 | `IPC_MSG_ENTRY_ERROR` | fd_ctrl | 条目级错误（v15.5.7） |

**BATCH payload 结构**：

```
[IpcBatchHeader: count=uint32_t]
[count × {
    uint32_t path_len,
    char path[path_len],
    struct stat st
}]
[uint64_t footer_magic = 0xDEADBEEF66AAC0FF]
```

### 6.3 Master ↔ IPC 线程消息

**命令（Main → IPC）**：

| 值 | 名称 | 说明 |
|----|------|------|
| 1 | `CMD_SCAN` | 发送 SCAN 任务 |
| 2 | `CMD_REPLACE` | 替换 Worker fd/pid |
| 3 | `CMD_STOP` | 停止 IPC 线程 |

**返回（IPC → Main）**：

| 值 | 名称 | 说明 |
|----|------|------|
| 10 | `RET_BATCH` | Worker 返回批次 |
| 11 | `RET_HEARTBEAT` | 心跳 |
| 12 | `RET_ERROR` | 设备级错误 |
| 13 | `RET_DEAD` | Worker 死亡 |
| 14 | `RET_EXIT` | 正常退出 |
| 15 | `MSG_DROP` | SCAN 在替换窗口期被丢弃 |
| 16 | `RET_DEV_TIMEOUT` | Scanner 自检测超时 |
| 17 | `RET_READY` | Worker 初始化完成 |
| 18 | `RET_FINISH` | 任务完成 |
| 19 | `RET_ENTRY_ERROR` | 条目级错误（v15.5.7） |

### 6.4 FSM 续传协议

解决 `EAGAIN` 时跨 `epoll_wait` 调用数据丢失：

```
IPC_READ_IDLE ──epoll返回EPOLLIN──> IPC_READ_HDR
  ▲                                      │
  │                                      │ 读满8字节
  │                                      ▼
  │                              IPC_READ_PAYLOAD
  │                                      │
  │                                      │ 读满payload_len-8字节
  │                                      ▼
  │                              IPC_READ_FOOTER
  │                                      │
  │                                      │ 读满8字节Footer
  └──────────────────────────────────────┘  校验魔数 → 消息完整
```

---

## 7. 数据持久化模型

### 7.1 pbin — 进度分片

**作用**：记录所有已发现但尚未扫描的目录，是断点续传的核心数据源。

**格式**：文本文件，每行一条记录：

```
<path>\0<stat_binary>\n
# Footer（24 字节，文件末尾）
magic(8) + row_count(8) + data_crc32(4) + footer_crc32(4)
```

**分片策略**：每 `progress_slice_lines`（默认 10 万）行切分一个新文件，命名格式 `{base}.pbin.{NNNNNN}`。

**Footer 自描述**：每个分片独立记录自己的行数，支持截断恢复（`pbin_salvage_truncated()`）。

### 7.2 dpbin — 完成日志

**作用**：本次会话的"完成目录集合"，恢复时用于计算差集（`pbin - dpbin = 待扫描目录`）。

**特性**：
- 只分片、不归档
- 正常扫描结束后自动删除
- 格式同 pbin，但无 Footer（不需要截断恢复）

### 7.3 fpbin — 恢复临时缓存

**作用**：恢复期间，新发现的子目录写入 fpbin（而非直接入队），避免恢复阶段与正常扫描阶段混淆。

**状态转换**：

```
恢复开始: HIST_PUMP_OLD（消费原始 pbin，新子目录 → fpbin）
     │
     │ 原始 pbin 消费完毕
     ▼
HIST_PUMP_NEW（fpbin 转正为新 pbin，新子目录直接入队）
     │
     │ fpbin 消费完毕
     ▼
HIST_PUMP_DONE（正常扫描模式）
```

### 7.4 spbin — 跳过记录

**作用**：记录被熔断/探测跳过的目录，支持设备恢复后重入队。

**磁盘格式**（二进制）：

```c
typedef struct __attribute__((packed)) {
    uint32_t path_len;
    uint64_t dev;
    time_t   blacklist_time;
    uint32_t retry_count;
    uint32_t probe_interval;
    uint8_t  d_type;
    uint8_t  s_status;   // SP_STATUS_PROBING or SP_STATUS_CONDEMNED
} SpbinRecordHeader;
// 后接 path 字节
```

**归档**：spbin 块固定位于 `.archive` 文件末尾，`block_type = 1`。

### 7.5 dspill — 派发兜底（v15.5.8）

**问题背景**：pbin 滑动窗口的加载器游标可能追到已被 `process_old_slice` 轮转删除的分片后永久卡死；HIGH_WATER 跳推的目录整子树静默丢失。

**设计**：
- 运行级追加文件 `{base}.dspill`，无分片轮转、无删除竞争
- 队列达到 HIGH_WATER 时，跳推的目录追加写入 dspill
- 主线程按**字节游标**从 dspill 读取回填
- 只含跳推目录，不混正常 pbin 数据

### 7.6 archive — 压缩归档

**格式**：

```
[ArchiveBlockHeader: uncompressed_size + compressed_size + block_type + row_count]
[gzip compressed data]
[ArchiveBlockHeader ...]
...
[最后一块：block_type = 1 (spbin)]
```

- `block_type = 0`：normal pbin
- `block_type = 1`：spbin（固定位于末尾）

---

## 8. 故障处理与容错

### 8.1 Worker 死亡与替换

```
IPC 线程检测到 heartbeat 超时 / epoll error/hup
    │
    ├── SIGKILL Worker（如需要）
    ├── close(fd_cmd/fd_data/fd_ctrl), epoll DEL
    ├── 发 RET_DEAD → Main 的 ret_queue[slot]
    │
    ▼
Main 收到 RET_DEAD
    ├── cleanup_dead_worker_slot(slot, redispatch_current=true)
    │   ├── 若 current_path 非空 → dispatch_queue_push(requeue)
    │   ├── pending_tasks--（若 redispatch 成功则后续 send_scan 再 ++）
    │   └── 目录级熔断检查
    ├── worker_pool_replace(slot) → spawn 新 Worker
    └── send_replace_to_ipc(slot, new_fd_cmd, new_fd_data, new_fd_ctrl, new_pid)
        ▼
    IPC 线程收到 CMD_REPLACE
        ├── close(old fds)
        ├── 更新 fd_cmd/fd_data/fd_ctrl/pid
        ├── 重置 FSM 状态为 IPC_READ_IDLE
        └── epoll ADD new fds
```

### 8.2 设备级熔断

```
Worker 返回 RET_ERROR (errno=ETIMEDOUT/EIO, dev=X)
    │
    ▼
Main: dev_mgr_mark_probing(dev=X)
    probe_scheduler_push(dev=X, interval=5s)
    │
    ▼
Monitor: 到探测时间 → spawn 敢死队进程扫描 test 路径
    │
    ├── 成功 → dev_mgr_mark_alive(dev=X) → spbin_requeue_recovered(dev=X)
    │
    └── 失败 → interval *= 2（max 300s）
              超过 PROBE_MAX_RETRIES → dev_mgr_mark_condemned(dev=X)
```

### 8.3 目录级熔断与退避

**目录级熔断**（v15.5.3）：同一目录连续 DEV_TIMEOUT 超过阈值后不再重试，防止所有 Worker 逐个卡死。

**redispatch 指数退避**（v15.5.9）：Worker 死亡后，cleanup 阶段根据该路径已连续超时次数设置退避时间：

| 连续超时次数 | 退避时间 |
|-------------|---------|
| 1 | 30s |
| 2 | 120s |
| ≥3 | 300s |

退避期间 `dispatch_from_queue` 遇到该 slot 直接跳过，不派发任务。

### 8.4 NFS 大目录防误判

**多层防御**：

| 层级 | 机制 | 版本 |
|------|------|------|
| L1 | HEARTBEAT_TIMEOUT 120s（原为 30s） | v15.5.9 |
| L2 | CIRCUIT_BREAKER_THRESHOLD 10（原为 3） | v15.5.9 |
| L3 | scanner_progress_tick 时间驱动 5s | v15.5.9 |
| L4 | opendir() 后立即 tick | v15.5.9 |
| L5 | send_batch() 前后各 tick | v15.5.9 |
| L6 | redispatch 指数退避 | v15.5.9 |

### 8.5 扫描完整性断言

**nlink oracle**（v15.5.8，`--strict-nlink`）：
- 目录的 `st_nlink - 2` 应为子目录数（`.` 和 `..` 除外）
- 扫描完成后，若实际子目录数 ≠ `st_nlink - 2`，记 `NLINK_MISMATCH`
- 捕获无 errno 的假空/假 EOF（目录被截断或 NFS 返回不完整）

**完结硬性断言**：
- dspill 必须排空到 EOF，残留记 `DSPILL_RESIDUE`，非零退出
- MSG_DROP 销账，丢失记 `TASK_DROP_LOST`
- 熔断清单非空 → 非零退出码

---

## 9. 性能考量

| 优化点 | 实现 | 效果 |
|--------|------|------|
| **盲信跳过** | `reference_map` 缓存历史文件 mtime，未变更跳过 lstat | 半增量场景减少 90%+ I/O |
| **HashSet 预分配** | `estimated_files` 参数预分配指纹集合 | 避免运行时频繁 rehash |
| **8MB 输出缓冲** | `setvbuf(..., _IOFBF, 8MB)` | 减少 write 系统调用次数 |
| **批量 record_path** | 4096 条/1MB 批量缓冲后统一写入 pbin | 减少 fwrite 次数 |
| **dispatch_queue 环形缓冲** | head 索引实现 O(1) pop | 积压数万时不卡顿 |
| **去重线程池** | 4 线程并行 CPU 去重 | 充分利用多核 |
| **滑动窗口背压** | HIGH_WATER 停止 push，LOW_WATER 触发加载 | 内存 hard cap 10 万条 |

---

## 10. 安全考量

| 方面 | 措施 |
|------|------|
| **路径长度限制** | `MAX_PATH_LENGTH = 4088`，确保 `total_len ≤ PIPE_BUF`，原子写入 |
| **payload 长度校验** | `hdr.payload_len > 100MB` 时 `log_fatal`，防止畸形 Header 导致超大 malloc |
| **FD 泄漏防护** | Worker 替换时 IPC 线程负责 close 旧 fd，主线程只设 `-1` |
| **重复初始化防护** | `main_loop_run()` 不重复调用 `init_ipc_threads()`，统一由 `main.c` 负责 |
| **僵尸进程收割** | 主循环定期 `waitpid(-1, NULL, WNOHANG)` |
| **spbin 目录安全** | spbin 归档块固定位于 `.archive` 末尾，恢复逻辑依赖此顺序 |

---

## 11. 部署与运维

### 11.1 编译

```bash
cd /root/listfiles
make clean && make
```

要求：GCC 支持 `-std=gnu11`，`_Atomic`、`_GNU_SOURCE`。

### 11.2 典型运行参数

```bash
# 全量扫描
./listfiles -p /public2/data -o /tmp/output.txt -f /tmp/progress

# 断点续传
./listfiles -p /public2/data -o /tmp/output.txt -f /tmp/progress -c

# 半增量（跳过 7 天内未变更的文件）
./listfiles -p /public2/data -o /tmp/output.txt -f /tmp/progress -c --skip-interval=604800

# 大目录加固模式（启用 nlink oracle）
./listfiles -p /public2/data -o /tmp/output.txt -f /tmp/progress --strict-nlink
```

### 11.3 监控与日志

```bash
# 生产运行（静默）
./listfiles ... --mute

# 排查问题（打开 v15.5.9 追踪日志）
./listfiles ... --verbose-level=2 --verbose-version=202608090000

# 打开所有历史日志
./listfiles ... --verbose-level=3 --verbose-version=0
```

### 11.4 NFS 挂载要求

```bash
mount -t nfs -o soft,intr,timeo=600,retrans=3 server:/public2 /public2
```

**严禁 hard 挂载**：D-State 下 `SIGKILL` 无效，Worker 僵死后无法替换。

---

## 12. 附录：版本演进摘要

| 版本 | 时间 | 架构调整 | 核心解决的问题 |
|------|------|---------|--------------|
| v11.x | 2025 | 基线：多线程共享内存 | — |
| **v12.0.0** | 2026-04 | **线程 → 进程** | D-State 不可杀死 |
| v12.1.x | 2026-04 | fpbin 隔离 + Footer 自描述 | 恢复期间 pbin 读写冲突 |
| v12.2.x | 2026-05 | 管道死锁修复 + O_NONBLOCK | 双向管道死锁、fd 重用竞争 |
| **v13.0.0** | 2026-05 | **IPC 线程隔离** | 单线程 epoll 瓶颈 |
| v13.0.1~3 | 2026-05 | 协议原子写入 + fd 生命周期 | Header 孤悬、double free |
| **v14.0.0** | 2026-05 | **Worker 多线程化** | 扫描阻塞期间不响应/心跳中断 |
| v14.0.1 | 2026-05 | fd_out 互斥锁 | Scanner + IPC 线程写竞争 |
| **v15.0.0** | 2026-05 | **三通道分离 + IPC 状态机** | mutex 阻塞心跳、消息字节交错 |
| v15.0.1~4 | 2026-05 | 重复初始化修复 + 阻塞写修复 + IPC 链路追踪 | pending_tasks 不归零 |
| v15.1.x | 2026-05 | Master Worker 状态机 | 反复向卡死 Worker 发 SCAN |
| v15.2.0 | 2026-05 | 模块化拆分（24→32 文件） | 单体文件过大、职责混杂 |
| v15.3.0 | 2026-05 | 版本化日志框架 | 高频追踪日志污染 stderr |
| v15.4.x | 2026-05 | IPC FSM 续传 + BATCH Footer | EAGAIN 跨 epoll 数据丢失 |
| v15.5.0 | 2026-05 | SEDA dispatch_queue + dpbin | lost_tasks 语义混乱、idx 废除 |
| v15.5.2 | 2026-05 | pbin 滑动窗口背压 | dispatch_queue 内存无界 |
| v15.5.3 | 2026-07 | 目录级熔断 + 日志版本化 | 大目录 readdir 超时误判 |
| v15.5.6 | 2026-07 | 熔断清单 + 探测指数退避 | 扫描不完整但退出码 0 |
| v15.5.7 | 2026-07 | 扫描完整性加固 | 条目级/目录级错误静默丢失 |
| v15.5.8 | 2026-07 | dspill 派发兜底 + nlink oracle | pbin 滑动窗口卡死、假空目录 |
| **v15.5.9** | 2026-08 | **NFS 大目录防误判深度加固** | HEARTBEAT_TIMEOUT 过严、熔断阈值过低、无退避 |

---

> 本文档为架构设计层描述，具体修复细节、Bug 根因分析、版本间兼容性说明请参阅 `CHANGELOG.md`。
