# listfiles 架构设计文档

> 文档版本：v15.5.9  
> 最后更新：2026-08-09  
> 对应代码版本：`dev` 分支 `c83c1b9`  

---

## 目录

1. [概述](#1-概述)
2. [核心架构框架](#2-核心架构框架)
3. [扫描算法：类 BFS 按层处理](#3-扫描算法类-bfs-按层处理)
4. [场景化流程](#4-场景化流程)
   - 4.1 [初次扫描](#41-初次扫描)
   - 4.2 [初次扫描未完成的续传](#42-初次扫描未完成的续传)
   - 4.3 [具备完整首次扫描结果后的盲信扫描](#43-具备完整首次扫描结果后的盲信扫描)
   - 4.4 [忽略原始进度的再次全新扫描](#44-忽略原始进度的再次全新扫描)
5. [运行时模型](#5-运行时模型)
   - 5.1 [进程与线程模型](#51-进程与线程模型)
   - 5.2 [Worker 状态机](#52-worker-状态机)
   - 5.3 [IPC 线程循环](#53-ipc-线程循环)
   - 5.4 [主线程消息总线](#54-主线程消息总线)
6. [模块详细设计](#6-模块详细设计)
   - 6.1 [core — 配置与生命周期](#61-core--配置与生命周期)
   - 6.2 [ipc — 进程间通信](#62-ipc--进程间通信)
   - 6.3 [scan — 扫描引擎](#63-scan--扫描引擎)
   - 6.4 [output — 输出与监控](#64-output--输出与监控)
7. [通信协议](#7-通信协议)
   - 7.1 [三通道语义分离](#71-三通道语义分离)
   - 7.2 [IPC 消息格式](#72-ipc-消息格式)
   - 7.3 [Master ↔ IPC 线程消息](#73-master--ipc-线程消息)
   - 7.4 [FSM 续传协议](#74-fsm-续传协议)
8. [数据持久化模型](#8-数据持久化模型)
   - 8.1 [pbin — 进度分片](#81-pbin--进度分片)
   - 8.2 [dpbin — 完成日志](#82-dpbin--完成日志)
   - 8.3 [fpbin — 恢复临时缓存](#83-fpbin--恢复临时缓存)
   - 8.4 [spbin — 跳过记录](#84-spbin--跳过记录)
   - 8.5 [dspill — 派发兜底](#85-dspill--派发兜底)
   - 8.6 [archive — 压缩归档](#86-archive--压缩归档)
9. [故障处理与容错](#9-故障处理与容错)
   - 9.1 [Worker 死亡与替换](#91-worker-死亡与替换)
   - 9.2 [设备级熔断](#92-设备级熔断)
   - 9.3 [目录级熔断与退避](#93-目录级熔断与退避)
   - 9.4 [NFS 大目录防误判](#94-nfs-大目录防误判)
   - 9.5 [扫描完整性断言](#95-扫描完整性断言)
10. [性能考量](#10-性能考量)
11. [安全考量](#11-安全考量)
12. [部署与运维](#12-部署与运维)
13. [附录：版本演进摘要](#13-附录版本演进摘要)

---

## 1. 概述

`listfiles` 是一个面向 **PB 级分布式存储 / 十亿级文件** 场景的递归目录扫描工具。核心功能是将目录树遍历结果（路径、stat 元数据等）输出为 CSV 或自定义格式文本，同时支持断点续传、半增量扫描、设备熔断、进度归档等生产级特性。

运行环境默认为 **NFS 挂载的分布式存储**（曙光 ParaStor、Ceph 等），网络抖动、设备离线、元数据节点过载是常态。设计以 **容错优先、可观测性优先** 为第一原则。

---

## 2. 核心架构框架

三句话说完骨架：

1. **进程隔离模型**：Master 进程管理 8 个独立 Worker 进程，通过三通道 pipe 通信。Worker 进程内 Scanner 线程负责阻塞 IO（readdir/lstat），IPC 线程负责心跳与通信。Worker 卡死在 NFS D-State 时可被 SIGKILL 替换，不拖垮 Master。

2. **SEDA 五阶段流水线**：目录发现 → 子目录枚举（Worker Scanner）→ Batch 去重处理（CPU 线程池）→ 任务分发（dispatch_queue）→ 输出写入（异步线程）。阶段间通过队列/管道解耦，Master 主线程充当纯消息总线。

3. **类 BFS 按层扫描**：扫描以"目录"为粒度单位，Worker 每次消费一个目录任务，返回该目录下所有子目录（进入下一轮队列）和文件（直接输出）。整棵树按层推进，不是 DFS 深度递归。

核心约束：
- **NFS soft,intr,timeo=600 挂载**（hard 挂载下 D-State 不可杀）
- **同机运行**（IPC 协议不跨机器/不跨架构）
- **CentOS 7.4 / Bash 4.2 兼容**
- **25PB / 12 亿文件是基线**

模块拓扑（5 目录、32 模块、~8000 行）：

```
core/     — 生命周期、配置、命令行、信号、熔断辅助
ipc/      — 三通道 pipe、TLV 协议、IPC 线程、无锁队列、进程池
scan/     — 主循环消息总线、任务分发、队列、去重、设备管理、探测调度、线程池、扫描引擎
output/   — 输出渲染、进度核心、进度 IO、进度归档、异步输出、监控面板
util/     — 日志、xxhash
```

---

## 3. 扫描算法：类 BFS 按层处理

**不是 DFS。** 本工具从设计之初就不是"一条道走到黑"的递归模式。原因：
- DFS 深度递归在 PB 级存储上会产生极长的调用栈，且单一路径阻塞会冻结整个扫描流程。
- NFS 上目录层级可能极深，递归模式不可控。

**类 BFS 的核心语义**：

```
第 0 层: 根目录 /
         │
         ▼ Worker 扫描 /
         发现子目录: /a, /b, /c
         文件: /file1, /file2
         
第 1 层: /a, /b, /c 进入 dispatch_queue
         │
         ▼ 3 个 Worker 并行扫描
         /a 发现: /a/x, /a/y  + 文件
         /b 发现: /b/z        + 文件
         /c 发现: (空目录)     + 文件
         
第 2 层: /a/x, /a/y, /b/z 进入 dispatch_queue
         │
         ▼ 继续...
```

**关键设计**：
- **目录是任务单位**：dispatch_queue 中存储的是目录路径，不是文件路径。Worker 消费一个目录任务后，只负责枚举该目录的直接子项。
- **子目录入队、文件输出**：Worker 扫描一个目录后，将发现的子目录返回给 Master（经过去重后入 dispatch_queue），文件条目直接交给异步输出线程写入结果。
- **层与层之间天然并行**：同一层的多个目录可由多个 Worker 并行扫描，互不阻塞。
- **无递归栈**：Worker 的 `scan_and_send()` 只处理一个目录就返回，不递归进入子目录。子目录由 Master 统一调度。

**这为什么不是严格 BFS**：
- 严格 BFS 要求"第 n 层全部处理完才处理第 n+1 层"。本工具不保证这一点：dispatch_queue 是 FIFO，但 Worker 完成顺序不确定，所以层边界是模糊的。
- 更准确的描述是"**以目录为粒度的任务队列驱动遍历**"，队列顺序接近 BFS，但不严格保证层序。

---

## 4. 场景化流程

以下四个场景的运行流程截然不同，混在一起描述会导致理解偏差，因此分节独立说明。

---

### 4.1 初次扫描

**前提**：目标目录从未被扫描过，或用户明确不续传（不带 `-c`）。

**流程**：

```
1. 初始化
   ├── 解析命令行参数
   ├── 创建 AppContext（进程池、队列、集合、线程池）
   ├── 创建进度文件目录（pbin/dpbin/spbin 等前缀路径）
   └── spawn 8 个 Worker 进程 + 8 个 IPC 线程

2. 启动根任务
   └── dispatch_queue_push("/target/path", root_stat)

3. 主循环运行（SEDA 流水线全速运转）
   ├── dispatch_from_queue(): 弹出目录 → 发给 IDLE Worker
   │   └── CMD_SCAN → Worker Scanner 线程
   │
   ├── Worker 扫描该目录
   │   ├── opendir(path) → readdir 循环
   │   ├── 对每个条目 lstat
   │   ├── 文件: 收集进 BATCH（内存缓冲）
   │   ├── 目录: 收集进 BATCH（内存缓冲）
   │   ├── batch 满 batch_size → send_batch(fd_data) → IPC 线程 → RET_BATCH → Main
   │   └── readdir 结束 → send FINISH(fd_ctrl) → IPC 线程 → RET_FINISH → Main
   │
   ├── Main 收到 RET_BATCH
   │   ├── thread_pool 提交 batch_processor
   │   ├── 去重: fingerprint(path+dev+ino) 查 visited_set
   │   ├── 新目录: write_pbin() + dispatch_queue_push()
   │   └── 新文件: record_path_batch_append() → 异步输出线程刷盘
   │
   ├── Main 收到 RET_FINISH
   │   └── Worker→IDLE, pending_tasks--
   │
   └── 循环直到: pending_tasks==0 && dispatch_queue 空

4. 终止
   ├── stop 所有 Worker
   ├── finalize_archive()（压缩归档 pbin + spbin）
   ├── 删除 dpbin（本次完成，不再需要）
   └── 输出统计、退出
```

**关键数据流**：
- 目录路径：dispatch_queue → Worker → BATCH → batch_processor → pbin（持久化）+ dispatch_queue（新任务）
- 文件路径：BATCH → batch_processor → record_batch → async_worker → 输出文件

---

### 4.2 初次扫描未完成的续传

**前提**：上次扫描异常终止（崩溃、OOM、SIGKILL），留下了未完成的进度文件。用户带 `-c` 启动。

**流程**：

```
1. 初始化（同 4.1）

2. 加载进度（restore_progress）
   ├── 读取 archive / 散落 pbin 分片 → 重建"已发现目录集合"
   ├── 读取 dpbin → 重建"已完成目录集合"
   ├── 差集 = 已发现 - 已完成 = "待扫描目录"
   └── 泵送（pump）差集到 dispatch_queue

3. 主循环运行（与 4.1 相同，但 dispatch_queue 初始非空）
   ├── dispatch_from_queue(): 先消费恢复的目录
   ├── 运行中 Worker 返回新子目录 → pbin + dispatch_queue
   └── 同时 pump 继续从历史 pbin 加载更多目录回填 queue

4. 恢复完成判定
   ├── pbin 全部消费完毕 → hist_pump_state = HIST_PUMP_DONE
   ├── 此后新发现的子目录直接入队（不走 fpbin）
   └── 逻辑同 4.1 的终止条件

5. 终止（同 4.1）
```

**与 4.1 的关键差异**：
- 初始 dispatch_queue 非空（来自恢复的 pbin 差集）
- 运行中 batch_processor 写 pbin 的同时，pump 从旧 pbin 读取 → 存在**读写并发**
- fpbin 机制：恢复期间新发现的子目录先写入 fpbin（而非直接入队），防止恢复阶段与正常扫描阶段混淆。旧 pbin 消费完毕后 fpbin 转正。

---

### 4.3 具备完整首次扫描结果后的盲信扫描

**前提**：已有一次完整扫描，建立了 `reference_map`（存储每个目录/文件的 fingerprint + mtime 基准）。用户带 `-c --skip-interval=<秒>` 启动。

**流程**：

```
1. 初始化（同 4.1）

2. 加载历史基准（restore_progress_to_memory）
   ├── 读取上次扫描的 pbin 和 archive
   ├── 重建 reference_map（fingerprint → mtime）
   └── 重建 reference_set（用于输出合并的指纹集合）

3. 主循环运行（带盲信检查）
   ├── dispatch_from_queue(): 弹出目录
   ├── CMD_SCAN → Worker
   │
   ├── Worker 扫描该目录（与 4.1 相同，但增加了盲信判断）
   │   ├── opendir(path) → readdir
   │   ├── 对每个条目:
   │   │   ├── 计算 fingerprint(path, dev, ino)
   │   │   ├── 查 reference_map:
   │   │   │   ├── 存在:
   │   │   │   │   └── 文件: 不执行 lstat，直接复用 reference_map 中记录的上次 stat 数据填入 BATCH
   │   │   │   │   └── 目录: 仍须 readdir 枚举子目录（子目录发现不可盲信跳过）
   │   │   │   └── 不存在:
   │   │   │       └── 正常 lstat，新数据进入 BATCH
   │   └── batch 满 → send_batch
   │
   ├── Main 收到 RET_BATCH
   │   ├── 去重（visited_set，防环）
   │   ├── 新目录: write_pbin() + dispatch_queue_push()
   │   └── 新文件/变更文件: record_path_batch_append() → 输出
   │
   └── 终止条件同 4.1

4. 终止
   ├── finalize_archive()
   ├── reference_map 不自动更新（基准保持上次完整扫描的结果）
   └── 输出包含：本轮变更条目 + 盲信跳过的旧条目（由 reference_set 结转）
```

**与 4.1/4.2 的关键差异**：
- **盲信即不验证**：Worker 对 `reference_map` 中存在的条目**不执行 lstat**，直接复用上次记录的 stat 数据（来自首次完整扫描的 pbin）。本轮输出中这些条目带着的是**上次扫描时的 mtime**，不是当前值。**不需要读取文件当前的 mtime**。
- `reference_map` 在本次运行中**只读**，不写入新条目。只有全量扫描（不带 `--skip-interval`）才会更新 reference_map 和 pbin。
- 盲信扫描仍能发现**新增条目**（readdir 发现了 reference_map 中没有的条目），但不会发现**已有条目的变更**（因为不 lstat）。
- 盲信扫描的输出是"上次全量结果 + 本轮新增条目"的混合。已变更条目不会反映最新状态。
- 删除的条目：若 readdir 中仍可见则输出（旧数据），若已不可见则自然消失。无 tombstone 机制。

**盲信扫描的正确性前提**：
1. 必须已有至少一次完整扫描建立的 pbin 基准（reference_map 从 pbin 重建）。
2. **盲信扫描期间文件系统应无变更**。若有变更（文件修改、属性变化），盲信扫描**不会检测到**——因为完全不读取当前状态。
3. 盲信跳过是**显式开关**（`--skip-interval`），默认关闭。`skip_interval` 参数的含义是"上次全量扫描距今不超过此秒数则允许盲信"，即只在确认近期无变更时使用。
4. 盲信扫描的输出是**近似快照**，不是精确当前状态。用于快速复现上次结果或处理新增条目，不用于变更检测。

---

### 4.4 忽略原始进度的再次全新扫描

**前提**：用户想重新全量扫描，但可能复用同一进度目录（覆盖旧进度），或指定新进度前缀。

**流程**：

```
1. 参数解析
   ├── 若 -f 指定的进度目录已存在:
   │   ├── 不带 --runone: 提示"进度目录已存在，是否覆盖？"或拒绝
   │   └── 带 --runone: 删除旧进度文件，重新开始
   └── 若 -f 指定新路径: 创建新进度目录

2. 初始化（同 4.1，但 reference_map 为空）

3. 主循环运行（同 4.1 初次扫描，无盲信）

4. 终止
   └── 输出全新全量结果
```

**与 4.1 的差异**：
- 如果复用进度目录，需要显式 `--runone` 强制覆盖，防止误操作。
- 不存在 reference_map 加载，所有文件全量 lstat + 输出。

---

## 5. 运行时模型

### 5.1 进程与线程模型

| 层级 | 数量 | 职责 | 生命周期 |
|------|------|------|----------|
| **Master 进程** | 1 | 消息总线、任务调度、进度管理、监控 | 整个运行期 |
| **IPC 线程** | 8（固定） | 每 Worker 一个，独立 epoll + 心跳 + SIGKILL | 与 Master 同寿 |
| **Worker 进程** | 8（默认） | 执行实际扫描 | 动态替换 |
| **Scanner 线程** | 8（每 Worker 一个） | 阻塞 IO（readdir/lstat） | 与 Worker 同寿 |
| **去重线程池** | 4（默认） | CPU 密集型 batch 去重 | 与 Master 同寿 |
| **异步输出线程** | 1 | 写输出文件 / 进度文件 | 与 Master 同寿 |
| **Monitor 线程** | 1 | 秒表面板、探测调度、进程收割 | 与 Master 同寿 |

### 5.2 Worker 状态机

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

### 5.3 IPC 线程循环

```
while (running) {
    // 1. 非阻塞 drain 主线程命令队列 (CMD_SCAN / CMD_REPLACE / CMD_STOP)
    // 2. epoll_wait(fd_data + fd_ctrl + cmd_queue_eventfd, 500ms)
    // 3. 处理 fd_data 事件：FSM 续传读取 BATCH → 完整后 send_return(RET_BATCH)
    // 4. 处理 fd_ctrl 事件：FSM 续传读取 HEARTBEAT/ERROR/EXIT/READY/FINISH → 转发到 ret_queue
    // 5. 心跳检测：last_heartbeat > heartbeat_timeout ? SIGKILL + send_return(RET_DEAD)
}
```

### 5.4 主线程消息总线

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

## 6. 模块详细设计

### 6.1 core — 配置与生命周期

**Config** 全局配置字段（关键项）：

| 字段 | 说明 | 默认值 |
|------|------|--------|
| `target_path` | 扫描根路径 | — |
| `output_file` | 输出文件 | — |
| `progress_base` | 进度文件前缀 | — |
| `continue_mode` | 断点续传 | false |
| `skip_interval` | 盲信扫描阈值（秒） | 0（关闭） |
| `batch_size` | Worker batch 大小 | 1024 |
| `estimated_files` | HashSet 预分配 | 1000 万 |
| `worker_count` | Worker 数 | 8 |
| `heartbeat_timeout` | 心跳超时 | 120s |
| `strict_nlink` | nlink oracle | false |

**AppContext** 运行时上下文核心字段：
- `visited_set`：本次防环（128-bit MD5）
- `completed_set`：dpbin 恢复时的已完成集合
- `reference_map` / `reference_set`：盲信扫描基准
- `worker_pool`、`probe_scheduler`、`dev_mgr`：进程/设备管理
- `dispatch_queue`：Stage 3→4 队列
- `ipc_cmd_queues[8]`、`ipc_ret_queues[8]`：无锁消息队列
- `pending_tasks`（原子）、`pending_batches`（原子）：任务计数
- `dspill_fp`：派发兜底文件
- `redispatch_backoff_until[8]`：退避时间戳

### 6.2 ipc — 进程间通信

**三通道 Pipe 模型**（v15.0.0）：

| 通道 | 方向 | 语义 | 写入者 | 阻塞策略 |
|------|------|------|--------|----------|
| `fd_cmd` | M→W | SCAN / STOP | Master | 阻塞写，非阻塞读 |
| `fd_data` | W→M | BATCH（大 payload） | Scanner 线程 | 阻塞写，非阻塞读 |
| `fd_ctrl` | W→M | HEARTBEAT/ERROR/EXIT/READY/FINISH | IPC 线程 | 阻塞写，非阻塞读，< PIPE_BUF |

**无锁队列**（Master ↔ IPC 线程）：容量 1024 条，64 位 CAS head/tail。

**FSM 续传**（v15.4.0）：跨 `epoll_wait` 的可恢复读取，状态 `IPC_READ_IDLE → HDR → PAYLOAD → FOOTER`。

### 6.3 scan — 扫描引擎

**Worker Scanner**（`worker_scanner.c`）：
- `opendir(path) → readdir 循环 → lstat 每个条目 → 区分文件/目录`
- 文件：收集进 batch → 满 `batch_size` 时 `send_batch(fd_data)`
- 目录：收集进 batch → 由 batch_processor 后续处理
- 盲信检查（若启用）：`reference_map` 中存在 → 不执行 lstat，直接复用上次记录的 stat 数据
- `scanner_progress_tick()` 每 5s 更新（v15.5.9），防止 NFS 大目录误判

**Batch Processor**（`batch_processor.c`）：
1. 解析 BATCH 数据
2. 计算 `path+dev+ino` 的 128-bit MD5 fingerprint，查 `visited_set` 防环
3. 新目录：`write_pbin()` + `dispatch_queue_push()`
4. 新文件：`record_path_batch_append()` → 异步输出线程

**背压**（v15.5.2）：`dispatch_queue.count > 10万` 时停止 push，目录仍写 pbin；`< 3万` 时从 pbin cursor 加载 5 万条回填。

**Dispatch**（`dispatch.c`）：
- `dispatch_from_queue()`：轮询 IDLE Worker，遇退避期 skip
- `cleanup_dead_worker_slot()`：死亡 Worker 的 current_path requeue，目录级熔断检查

**Device Manager**：
- 状态：`NORMAL → PROBING → DEAD → CONDEMNED`
- 无锁读（`_Atomic`），mutex 保护写
- 渐进探测：指数退避 5s→10s→20s→...→300s

### 6.4 output — 输出与监控

**输出模式**：单文件（`-o`）或按行数分片（`-O`）。

**异步输出线程**：线程安全队列，8MB 全缓冲，批量刷盘。

**Monitor**：500ms 刷新，`[MM:SS] dirs: X files: Y rate: Z/s active: A/B pending: P devs: D probing E dead`。

---

## 7. 通信协议

### 7.1 三通道语义分离

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
   │<──── fd_ctrl ─── [ERROR]              │
   │<──── fd_ctrl ─── [DEV_TIMEOUT]        │
   │<──── fd_ctrl ─── [EXIT]               │
```

### 7.2 IPC 消息格式

**Header（8 字节，packed）**：`msg_type(4) + payload_len(4)`

**Worker → Master 消息类型**：

| 值 | 名称 | 通道 | 说明 |
|----|------|------|------|
| 1 | `IPC_MSG_SCAN` | fd_cmd | M→W 扫描任务 |
| 2 | `IPC_MSG_BATCH` | fd_data | 扫描结果批次 |
| 3 | `IPC_MSG_HEARTBEAT` | fd_ctrl | 心跳 |
| 4 | `IPC_MSG_ERROR` | fd_ctrl | 设备级错误 |
| 5 | `IPC_MSG_EXIT` | fd_ctrl | 正常退出 |
| 6 | `IPC_MSG_STOP` | fd_cmd | M→W 停止 |
| 7 | `IPC_MSG_DEV_TIMEOUT` | fd_ctrl | Scanner 自检测超时 |
| 8 | `IPC_MSG_READY` | fd_ctrl | Worker 初始化完成 |
| 9 | `IPC_MSG_FINISH` | fd_ctrl | 当前任务完成 |
| 10 | `IPC_MSG_ENTRY_ERROR` | fd_ctrl | 条目级错误（v15.5.7） |

**BATCH payload**：`[IpcBatchHeader: count] + [count × {path_len, path, stat}] + [footer_magic]`

### 7.3 Master ↔ IPC 线程消息

**命令（Main → IPC）**：`CMD_SCAN`、`CMD_REPLACE`、`CMD_STOP`

**返回（IPC → Main）**：`RET_BATCH`、`RET_HEARTBEAT`、`RET_ERROR`、`RET_DEAD`、`RET_EXIT`、`MSG_DROP`、`RET_DEV_TIMEOUT`、`RET_READY`、`RET_FINISH`、`RET_ENTRY_ERROR`

### 7.4 FSM 续传协议

跨 `epoll_wait` 调用的可恢复读取：`IPC_READ_IDLE → HDR → PAYLOAD → FOOTER`。`EAGAIN` 时不释放 buf、不重置 `nread`。`CMD_REPLACE` 时重置 FSM。

---

## 8. 数据持久化模型

### 8.1 pbin — 进度分片

记录所有已发现但尚未扫描的目录。文本格式 + Footer 自描述。每 10 万行切分，命名 `{base}.pbin.{NNNNNN}`。

### 8.2 dpbin — 完成日志

本次会话的"已完成目录集合"。恢复时 `pbin - dpbin = 待扫描目录`。只分片、不归档，正常结束后删除。

### 8.3 fpbin — 恢复临时缓存

恢复期间新发现的子目录先写入 fpbin（而非直接入队）。旧 pbin 消费完毕后 fpbin 转正。状态：`HIST_PUMP_OLD → HIST_PUMP_NEW → HIST_PUMP_DONE`。

### 8.4 spbin — 跳过记录

二进制格式记录被熔断/探测跳过的目录。设备恢复后可重入队。归档块固定位于 `.archive` 末尾。

### 8.5 dspill — 派发兜底（v15.5.8）

运行级追加文件 `{base}.dspill`。背压跳推的目录追加写入，主线程按字节游标读取回填。无轮转无删除。

### 8.6 archive — 压缩归档

gzip 压缩的 pbin 块 + spbin 块。`block_type = 0/1` 区分。

---

## 9. 故障处理与容错

### 9.1 Worker 死亡与替换

```
IPC 线程检测超时/error/hup
    ├── SIGKILL Worker
    ├── close fds, epoll DEL
    └── send_return(RET_DEAD)
        ▼
Main: cleanup_dead_worker_slot() → requeue current_path
      worker_pool_replace() → spawn 新 Worker
      send_replace_to_ipc() → IPC 线程更新 fd/pid，重置 FSM
```

### 9.2 设备级熔断

`RET_ERROR` → `dev_mgr_mark_probing()` → 敢死队探活 → 成功则 alive，失败则指数退避 → `PROBE_MAX_RETRIES` 次后 `CONDEMNED`。

### 9.3 目录级熔断与退避

同一目录连续 DEV_TIMEOUT 超过 `CIRCUIT_BREAKER_THRESHOLD (10)` 则跳过。redispatch 指数退避：1 次 30s、2 次 120s、≥3 次 300s。

### 9.4 NFS 大目录防误判

六层防御：HEARTBEAT_TIMEOUT 120s、熔断阈值 10、时间驱动 tick 5s、opendir tick、send_batch tick、redispatch 退避。

### 9.5 扫描完整性断言

- nlink oracle（`--strict-nlink`）：`st_nlink - 2` 应等于子目录数
- dspill 必须排空到 EOF
- MSG_DROP 销账
- 熔断清单非空 → 非零退出码

---

## 10. 性能考量

| 优化点 | 效果 |
|--------|------|
| 盲信跳过（有基准时） | 减少 90%+ I/O |
| HashSet 预分配 | 避免 rehash |
| 8MB 输出缓冲 | 减少 write syscall |
| 批量 record_path | 减少 fwrite |
| dispatch_queue 环形缓冲 | O(1) pop |
| 去重线程池 | CPU 并行 |
| 滑动窗口背压 | 内存 hard cap 10 万条 |

---

## 11. 安全考量

- `MAX_PATH_LENGTH = 4088`，确保原子写入
- `payload_len > 100MB` 时 `log_fatal`
- Worker 替换时 IPC 线程 close fd
- 僵尸进程 `waitpid(-1, NULL, WNOHANG)`

---

## 12. 部署与运维

### 编译
```bash
cd /root/listfiles && make clean && make
```

### 典型参数
```bash
# 全量扫描
./listfiles -p /public2/data -o /tmp/output.txt -f /tmp/progress

# 续传
./listfiles -p /public2/data -o /tmp/output.txt -f /tmp/progress -c

# 盲信扫描（需已有完整基准）
./listfiles -p /public2/data -o /tmp/output.txt -f /tmp/progress -c --skip-interval=604800
```

### NFS 挂载要求
```bash
mount -t nfs -o soft,intr,timeo=600,retrans=3 server:/public2 /public2
```

---

## 13. 附录：版本演进摘要

| 版本 | 时间 | 架构调整 | 解决的问题 |
|------|------|---------|-----------|
| v12.0.0 | 2026-04 | 线程 → 进程 | D-State 不可杀死 |
| v13.0.0 | 2026-05 | IPC 线程隔离 | 单线程 epoll 瓶颈 |
| v14.0.0 | 2026-05 | Worker 多线程化 | 扫描阻塞不响应 |
| v15.0.0 | 2026-05 | 三通道分离 + IPC 状态机 | mutex 阻塞心跳 |
| v15.5.9 | 2026-08 | NFS 大目录防误判加固 | HEARTBEAT_TIMEOUT 过严 |
