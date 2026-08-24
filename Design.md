# listfiles 架构设计文档

> 文档版本：v15.6.1  
> 最后更新：2026-08-25  
> 对应代码版本：`dev` 分支工作区（未提交）  
> 前序版本：v15.6.0  
> 实现依据：`fix_documents/fixed_15.6.1_P0_incident_hotfix.md`（生产事故热修复 P0-101~109 + 实现期缺陷实录）；v15.6.0 基线见 `fix_documents/fixed_15.6.0_P0_all_in_one.md`

---

## 目录

1. [概述](#1-概述)
2. [核心架构框架](#2-核心架构框架)
3. [扫描算法：类 BFS 按层处理](#3-扫描算法类-bfs-按层处理)
4. [场景化流程](#4-场景化流程)
   - 4.1 [初次扫描](#41-初次扫描)
   - 4.2 [初次扫描未完成的续传](#42-初次扫描未完成的续传)
   - 4.3 [具备合格基准后的盲信扫描](#43-具备合格基准后的盲信扫描)
   - 4.4 [忽略原始进度的再次全新扫描](#44-忽略原始进度的再次全新扫描)
5. [运行时模型](#5-运行时模型)
   - 5.1 [进程与线程模型](#51-进程与线程模型)
   - 5.2 [Worker 状态机](#52-worker-状态机)
   - 5.3 [IPC 线程循环](#53-ipc-线程循环)
   - 5.4 [主线程消息总线](#54-主线程消息总线)
   - 5.5 [目录任务生命周期状态机](#55-目录任务生命周期状态机)
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
   - 8.3 [fpbin / dfpbin — 恢复临时缓存原子对](#83-fpbin--dfpbin--恢复临时缓存原子对)
   - 8.4 [spbin — 跳过记录](#84-spbin--跳过记录)
   - 8.5 [dspill — 派发兜底](#85-dspill--派发兜底)
   - 8.6 [archive — 压缩归档](#86-archive--压缩归档)
   - 8.7 [Run manifest — 权威运行状态](#87-run-manifest--权威运行状态)
9. [故障处理与容错](#9-故障处理与容错)
   - 9.1 [Worker 死亡与替换](#91-worker-死亡与替换)
   - 9.2 [设备级熔断](#92-设备级熔断)
   - 9.3 [目录级熔断与退避](#93-目录级熔断与退避)
   - 9.4 [NFS 大目录防误判](#94-nfs-大目录防误判)
   - 9.5 [扫描完整性断言](#95-扫描完整性断言)
   - 9.6 [Reset 援救机制](#96-reset-援救机制)
10. [性能考量](#10-性能考量)
11. [安全考量](#11-安全考量)
12. [部署与运维](#12-部署与运维)
13. [附录：版本演进摘要](#13-附录版本演进摘要)

---

## 1. 概述

`listfiles` 是一个面向 **PB 级分布式存储 / 十亿级文件** 场景的递归目录扫描工具。核心功能是将目录树遍历结果（路径、stat 元数据等）输出为 CSV 或自定义格式文本，同时支持断点续传、半增量（盲信）扫描、设备熔断、进度归档等生产级特性。

运行环境默认为 **NFS 挂载的分布式存储**（曙光 ParaStor、Ceph 等），网络抖动、设备离线、元数据节点过载是常态。设计以 **容错优先、可观测性优先** 为第一原则。

v15.6.0 是一次可靠性闭环版本：将外部设计评审确认的 12 项 P0 一次性编码落地——目录任务完成屏障、输出三态状态机、fpbin/dfpbin 原子对、三集合统一队列模型、spbin 恢复路径、MSG_DROP 废除、RET_ERROR 状态机补全、Run manifest 权威状态、目录任务生命周期状态机、Reset 援救、盲信门禁、pbin schema 2 与 epoch 机制。核心目标：**任意崩溃点续传结果与干净全量基线逐行一致（sort -u），完结即完整，不完整运行不能链式成为盲信基准**。

---

## 2. 核心架构框架

三句话说完骨架：

1. **进程隔离模型**：Master 进程管理若干独立 Worker 进程（默认 `min(2×CPU核数, 8)`，`--worker-count` 可显式指定，上限 `MAX_WORKERS=64`），通过三通道 pipe 通信。Worker 进程内 Scanner 线程负责阻塞 IO（readdir/lstat），IPC 线程负责心跳与通信。Worker 卡死在 NFS D-State 时可被 SIGKILL 替换，不拖垮 Master。

2. **SEDA 五阶段流水线**：目录发现 → 子目录枚举（Worker Scanner）→ Batch 去重处理（CPU 线程池）→ 任务分发（dispatch_queue）→ 输出写入（异步线程）。阶段间通过队列/管道解耦，Master 主线程充当纯消息总线。

3. **类 BFS 按层扫描**：扫描以"目录"为粒度单位，Worker 每次消费一个目录任务，返回该目录下所有子目录（进入下一轮队列）和文件（直接输出）。整棵树按层推进，不是 DFS 深度递归。

核心约束：
- **NFS soft,timeo=6000,retrans=3 挂载**（hard 挂载下 D-State 不可杀，`intr` 在 CentOS 7.4 内核无效）
- **同机运行**（IPC 协议与进度二进制文件不跨机器；字段宽度平台相关，不保证跨架构续传）
- **CentOS 7.4 / Bash 4.2 兼容**
- **25PB / 12 亿文件是基线**

模块拓扑：

```
core/     — 生命周期、配置、命令行、信号、熔断辅助
ipc/      — 三通道 pipe、TLV 协议、IPC 线程、消息队列、进程池
scan/     — 主循环消息总线、任务分发、统一入队、去重、设备管理、探测调度、线程池、扫描引擎、盲信基准
output/   — 输出渲染、进度核心、进度 IO、进度归档（含 Reset 援救）、Run manifest、异步输出、监控面板
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
- **子目录入队、文件输出**：Worker 扫描一个目录后，将发现的子目录返回给 Master（经过去重后统一经 `enqueue_dir` 入队），文件条目直接交给异步输出线程写入结果。
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
   ├── 解析命令行参数（--worker-count 钳制到 MAX_WORKERS=64）
   ├── 创建 AppContext（进程池、队列、三集合、线程池）
   ├── 创建进度文件（pbin/dpbin/spbin/manifest 等前缀路径）
   ├── manifest 立即原子写入 status=Running（覆盖上次终态）
   └── spawn N 个 Worker 进程 + N 个 IPC 线程（N = --worker-count 或默认 min(2×CPU, 8)）

2. 启动根任务
   └── enqueue_dir("/target/path") → dispatch_queue

3. 主循环运行（SEDA 流水线全速运转）
   ├── dispatch_from_queue(): 弹出目录 → 发给 IDLE Worker
   │   └── CMD_SCAN(path, epoch) → IPC 线程 → Worker Scanner 线程
   │       （epoch 自增并写入 slot->current_epoch，供返回消息校验）
   │
   ├── Worker 扫描该目录
   │   ├── opendir(path) → readdir 循环
   │   ├── 对每个条目 lstat
   │   ├── 文件/目录: 收集进 BATCH（内存缓冲）
   │   ├── batch 满 batch_size → send_batch(fd_data, epoch) → IPC 线程 → RET_BATCH → Main
   │   └── readdir 结束 → send FINISH(fd_ctrl, epoch) → IPC 线程 → RET_FINISH → Main
   │       （IPC 线程转发 FINISH 前先把 fd_data drain 至 EAGAIN，
   │         保证 Master 见到 FINISH 时该任务全部 BATCH 已入账）
   │
   ├── Main 收到 RET_BATCH（epoch 不匹配则丢弃）
   │   ├── thread_pool 提交 batch_processor
   │   ├── 去重: fingerprint(path+dev+ino) 查 discovered_set
   │   ├── 新目录: write_pbin() + enqueue_dir()（统一入队入口）
   │   └── 新文件: write_pbin() + record_path_batch_append() → 异步输出线程刷盘
   │       （pbin 全量记录，不再要求 -c；任何成功运行都可作恢复来源与盲信基准）
   │
   ├── Main 收到 RET_FINISH（epoch 不匹配则丢弃）
   │   └── slot 进入 DT_BATCHES_RECEIVED——不立即销账、不置 IDLE
   │
   ├── advance_task_barriers（仅主线程）：批次处理完（batches_processed ==
   │   batches_received）且该 slot 未 COMMITTED 输出批次计数 output_pending
   │   归零 → 写 dpbin → pending_tasks-- → Worker 置 IDLE
   │
   └── 循环直到: pending_tasks==0 && dispatch_queue 空 && dspill 排空

4. 终止
   ├── stop 所有 Worker
   ├── spbin compaction（过滤已恢复 RECOVERED 条目重写）
   ├── finalize_archive()（压缩归档 pbin + spbin，.new → 校验 → rename 原子切换）
   ├── manifest_finalize()：写终态 manifest，判定 status 与 baseline_eligible
   ├── 删除 dpbin（本次完成，不再需要）
   ├── 成功完结后删除 dspill（有错误时保留供审计）
   └── 输出统计、退出（exit 0/1/2，见 §9.6）
```

**关键数据流**：
- 目录路径：dispatch_queue → Worker → BATCH → batch_processor → pbin（持久化）+ enqueue_dir（新任务）
- 文件路径：BATCH → batch_processor → pbin（持久化，盲信基准来源）+ record_batch → async_worker → 输出文件

---

### 4.2 初次扫描未完成的续传

**前提**：上次扫描异常终止（崩溃、OOM、SIGKILL），留下了未完成的进度文件。用户带 `-c` 启动。

**流程**：

```
1. 初始化（同 4.1）

2. 兼容性门禁
   ├── 有 pbin/archive 二进制残留但无 .manifest/.config（旧版本进度）
   │   └── 拒绝续传 exit(2)，提示 --runone 重新全量扫描
   ├── manifest/.config 存在：target_path 不一致、archive 策略不一致 → exit(2)
   └── manifest 权威状态加载（见 §8.7）

3. 加载进度（restore_progress）
   ├── 读取 archive / 散落 pbin 分片 → 重建 discovered_set（已发现条目）
   │   （find_pbin_index_bounds 显式区分"无分片"与"最大序号为 0"，
   │     同时确定泵送起点与写入序号）
   ├── 读取 dpbin → 重建 completed_set（已完成目录）
   ├── 检查 fpbin + dfpbin 原子对完整性（Footer 校验）
   │   ├── 完整 → fpbin 转正（泵送入队），dfpbin 合并入 dpbin
   │   │         （merge_dfpbin_into_dpbin：封口 → rename → 重置写状态）
   │   └── 不完整 → 整对抛弃，无法对账的残留由 Reset 援救兜底重扫（§9.6）
   ├── 读取 spbin → 按原因码分流（§8.4）：PERMISSION/CIRCUIT_BREAKER/POISON
   │   永久跳过；PROBE_FAIL/TIMEOUT 超窗后敢死队探测，设备活则经
   │   enqueue_dir 重入队；毒丸目录隔离，不阻塞同设备其余目录
   ├── 读取 dspill → 经 enqueue_dir 回填（enqueued_set 去重）
   └── 差集 = discovered_set − completed_set = "待扫描目录" → 泵送入队

4. 主循环运行（与 4.1 相同，但 dispatch_queue 初始非空）
   ├── dispatch_from_queue(): 先消费恢复的目录
   ├── 恢复阶段（HIST_PUMP_OLD）新发现子目录写 fpbin、完成目录写 dfpbin
   ├── 旧 pbin 消费完 → fpbin 转正 + dfpbin 合并入 dpbin → HIST_PUMP_DONE
   └── 此后新发现的子目录直接入队（不再走 fpbin/dfpbin）

5. 终止（同 4.1）
```

**与 4.1 的关键差异**：
- 初始 dispatch_queue 非空（来自恢复的差集 + dspill 回填 + spbin 重入队）
- 恢复阶段 batch_processor 写 fpbin/dfpbin 的同时，泵送线程从旧 pbin 读取 → 存在**读写并发**
- **fpbin/dfpbin 原子对**：恢复期间新发现子目录写 fpbin、完成目录写 dfpbin，二者构成可重入工作区——恢复过程本身再被 KILL（二次崩溃）后仍可再次进入恢复并收敛
- **enqueue_dir 剪枝守卫**（实现期缺陷 1/2）：completed 差集剪枝以"该目录同时在 discovered_set（pbin 有其发现记录）"为前提——dpbin 完成项若无 pbin 记录兜底则不采信、重新入队重扫（at-least-once，宁重复不丢失）

---

### 4.3 具备合格基准后的盲信扫描

**前提**：已有一次**合格基准**运行（manifest `baseline_eligible=1` 且 `pbin_schema_version=2`）。用户带 `-c --skip-interval=<秒> --reference-base=<旧进度前缀>` 启动。

**启动门禁（任一不满足即 exit 2，拒绝运行）**：

盲信 = 续传（`-c`）+ `skip_interval>0` + 非 `--runone`。v15.6.0 起工具强制校验前提，不再依赖用户自觉：

1. 必须显式指定 `--reference-base`（盲信基准不再隐式推断）；
2. 本轮 `-f` 进度路径必须为**新空目录**（无任何进度文件残留，增量与基准物理隔离——否则 manifest 启动写 Running 会覆盖基准终态）；
3. 基准 manifest 存在且 `baseline_eligible=1`、`pbin_schema_version=2`。

**流程**：

```
1. 初始化 + 门禁校验（exit 2 拒绝不合格运行）

2. 加载历史基准（restore_progress_to_memory(reference_base)）
   ├── 读取基准的 pbin / archive
   ├── 重建 reference_map（纯路径指纹 → 完整历史 stat：
   │   size/mtime/mtime_nsec/uid/gid/mode/d_type；dev/ino 不参与盲信身份判断）
   └── 重建 reference_set（输出合并用指纹集合）

3. 主循环运行（带盲信检查）
   ├── dispatch_from_queue(): 弹出目录
   ├── CMD_SCAN(path, epoch) → Worker
   │
   ├── Worker 扫描该目录
   │   ├── opendir(path) → readdir
   │   ├── 对每个条目:
   │   │   ├── 构造完整路径，计算纯路径指纹
   │   │   ├── 查 reference_map:
   │   │   │   ├── 命中且 d_type 一致:
   │   │   │   │   └── 文件: 不执行 lstat，直接复用基准 stat 数据填入 BATCH
   │   │   │   │   └── 目录: 仍须 readdir 枚举子目录（子目录发现不可盲信跳过）
   │   │   │   └── 未命中 / d_type 不一致 / mtime 在 skip_interval 保鲜期内:
   │   │   │       └── 正常 lstat，新数据进入 BATCH
   │   └── batch 满 → send_batch
   │
   ├── Main 收到 RET_BATCH
   │   ├── 去重（discovered_set，防环）
   │   ├── 新目录: write_pbin() + enqueue_dir()
   │   └── 文件: write_pbin() + record_path_batch_append() → 输出
   │       （盲信运行同样全量记录 pbin，但 baseline_eligible=0，见下）
   │
   └── 终止条件同 4.1

4. 终止
   ├── finalize_archive()
   ├── 基准不更新（reference_map 本轮只读）
   ├── 新 manifest baseline_eligible=0（盲信结果不能链式作为下次基准）
   └── 输出包含：盲信结转的旧条目 + 本轮新增条目
```

**与 4.1/4.2 的关键差异**：
- **盲信即不验证**：Worker 对 `reference_map` 命中且 d_type 一致的文件**不执行 lstat**，直接复用基准 stat 数据（来自基准运行的 pbin schema 2 记录）。输出中这些条目带着的是**基准运行时的元数据**，不是当前值。
- **不存在"变更文件"概念**：盲信模式不读取已有条目的当前状态，因此既不会发现已有条目的变更，输出中也不会有"变更文件"——只有"基准结转条目"与"本轮新增条目"两类。
- **纯路径指纹为 key**：盲信时不 lstat，无法获取 dev/ino，故盲信身份判断只用路径指纹；d_type 不一致视为未命中（防类型替换误判）。
- `skip_interval` 是"保鲜期阈值"：基准记录的 mtime 距今未超过此秒数（近期被修改过）则不盲信、正常 lstat；超过则允许盲信。
- 盲信扫描的输出是**近似快照**：不检测已有条目变更、不检测删除（无 tombstone），用于快速复现上次结果并并入新增条目。
- **结果不可链式**：盲信运行的 manifest `baseline_eligible=0`，下次盲信若以它为基准将被门禁拒绝（exit 2）。要重建合格基准必须回到全量扫描。

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
   └── 输出全新全量结果（status=Success 时 baseline_eligible=1，可作新基准）
```

**与 4.1 的差异**：
- 如果复用进度目录，需要显式 `--runone` 强制覆盖，防止误操作。
- **旧格式进度强制走本场景**：v15.6.0 之前的进度（无 manifest、pbin schema 1）不能续传、不能作盲信基准；检测到旧残留一律 exit 2，必须 `--runone` 重新全量扫描建立新基准。
- 不存在 reference_map 加载，所有文件全量 lstat + 输出。

---

## 5. 运行时模型

### 5.1 进程与线程模型

| 层级 | 数量 | 职责 | 生命周期 |
|------|------|------|----------|
| **Master 进程** | 1 | 消息总线、任务调度、进度管理、监控 | 整个运行期 |
| **IPC 线程** | N（= Worker 数） | 每 Worker 一个，独立 epoll + 心跳 + SIGKILL | 与 Master 同寿 |
| **Worker 进程** | N（默认 `min(2×CPU, 8)`，上限 64） | 执行实际扫描 | 死亡后经预备役补位 |
| **预备役 Worker（spare）** | N（= Worker 数，v15.6.1） | 启动期预 fork 的完整 Worker，待命中 | 随 Master 退出 |
| **Scanner 线程** | N（每 Worker 一个） | 阻塞 IO（readdir/lstat） | 与 Worker 同寿 |
| **去重线程池** | 4（默认） | CPU 密集型 batch 去重 | 与 Master 同寿 |
| **异步输出线程** | 1 | 写输出文件 | 与 Master 同寿 |
| **Monitor 线程** | 1 | 秒表面板、设备探测调度、**敢死队探测进程**收割 | 与 Master 同寿 |

**收割职责唯一 owner**：Worker 僵尸进程由**主线程**收割（主循环每轮 `waitpid(-1, WNOHANG)`，替换前 `waitpid` 确认旧进程死亡）；Monitor 线程只收割自己 fork 的敢死队探测进程（`reap_probes`）。二者不交叉。

**fork 时序约束（v15.6.1，P0-107）**：全部 fork（初始 Worker + spare）集中在**单线程期、巨型指纹集合（estimated-files 预分配）分配之前、一切 pthread_create 之前**；运行期**零 fork**——Worker 替换只从 spare 池启用。刚性依据（生产事故）：严格超售（`vm.overcommit_memory=2`）下 fork 按全额 VSZ 计 commit，大 VSZ 进程运行期 fork 必败（ENOMEM）；多线程进程 fork 的子进程继承锁状态可能永久死锁。Worker 子进程入口第一时间切无锁日志模式（`log_set_forked_child`）。盲信模式的基准索引是 Worker 的 COW 只读上下文，须在 fork 前加载（该功能已整体押后）。

### 5.2 Worker 状态机

```
                    spawn()
[DEAD/UNSPAWNED] ──────────────> [INITIALIZING]
                                        │
                                        │ RET_READY (60s startup_timeout 内)
                                        ▼
                                [IDLE] ──────────────> [DEAD]  (heartbeat_timeout)
                                  │                           SIGKILL + waitpid 确认
                                  │ CMD_SCAN (send success)     + drain + replace
                                  ▼
                                [BUSY] ──────────────> [DEAD]  (heartbeat_timeout)
                                  │      RET_DEV_TIMEOUT        SIGKILL + waitpid 确认
                                  │      (Scanner self-detected)+ drain + replace
                                  │
                                  │ RET_BATCH (multiple, 携带 epoch)
                                  │ RET_FINISH (携带 epoch, 匹配才受理)
                                  ▼
                     slot 任务屏障推进（见 §5.5），完结后回 [IDLE]
                                  │
                                  │ RET_ERROR（错误路径不再发 FINISH）
                                  ▼
                                [IDLE]  (device fuse，目录写 spbin，不重入队)
```

| 当前状态 | 触发条件 | 下一状态 | 动作 |
|---------|---------|---------|------|
| INITIALIZING | startup_timeout (60s) | DEAD | SIGKILL + waitpid + drain + replace |
| INITIALIZING | RET_READY | IDLE | 开始心跳计时 |
| IDLE | CMD_SCAN (发送成功) | BUSY | pending_tasks++，epoch 写入 slot，task_state=DT_SCANNING |
| IDLE | heartbeat_timeout (120s) | DEAD | SIGKILL + waitpid + drain + replace |
| BUSY | RET_FINISH（epoch 匹配） | BUSY（屏障推进中） | task_state→DT_BATCHES_RECEIVED，**不立即销账**；待批次处理完且输出 COMMITTED 后由 advance_task_barriers 统一完结（dpbin、pending_tasks--、IDLE） |
| BUSY | RET_ERROR | IDLE | **错误路径一处销账**：RET_ERROR 处理器内 pending_tasks--、Worker 置 IDLE、目录写 spbin、设备进 PROBING；不再发 FINISH、不经完成屏障、不重入队 |
| BUSY | heartbeat_timeout (120s) | DEAD | SIGKILL + waitpid + drain + replace |
| BUSY | RET_DEV_TIMEOUT（同代校验通过） | DEAD | **先 SIGKILL 旧 Worker** + cleanup + spare 启用（v15.6.1） |
| DEAD | cleanup + replace | INITIALIZING | **从 spare 池启用预 fork Worker（运行期零 fork，v15.6.1）**，IPC 线程更新 fd/pid、重置 FSM |

**关键变更（v15.6.1，P0-101/102/109）**：
- **死亡类消息同代校验**：RET_DEAD/RET_DEV_TIMEOUT/RET_EXIT（及 RET_ERROR/RET_ENTRY_ERROR）携带上报时刻的 (pid, epoch)，Master 仅受理 `reported_pid == slot->pid` 的同代消息——修复旧 stale 判定反转导致真实死亡被 100% 误吞的缺陷；跨代迟到消息一律丢弃。
- **cleanup 销账精确化**：仅 DT_SCANNING 由 cleanup 销账（orphaned 不叠加）；DT_BATCHES_RECEIVED/PROCESSED 由完成屏障照常完结销账（免重扫，替换延迟到屏障完结后）；空闲 Worker 死亡不销账。
- **RET_EXIT 携带在途任务 = 非预期死亡**：在途目录重入队 + 全局告警；"正常退出"的合法时机仅 STOP 之后。

**关键变更（v15.6.0）**：
- **epoch 机制**：每次派发附带递增 epoch（原子计数器），写入 `slot->current_epoch`；Worker 返回的 BATCH/FINISH 携带 epoch，Master 校验不匹配即丢弃——旧 Worker 的迟到消息不会污染新账目。
- **waitpid + drain**：SIGKILL 后先 `waitpid` 确认旧进程死亡，再 drain 旧 pipe 残留至 EAGAIN，之后才 spawn 替换。
- **FINISH 屏障语义**：FINISH 只代表"readdir 结束"，不再代表"目录完成"——完成判定移到目录任务完成屏障（§5.5）。
- **RET_ERROR 不重入队**：设备级错误登记 enqueued_set + 写 spbin 后等待设备恢复信号，统一经 spbin 恢复路径重试，消除反复派发空转。

### 5.3 IPC 线程循环

```
while (running) {
    // 1. 非阻塞 drain 主线程命令队列 (CMD_SCAN / CMD_REPLACE / CMD_STOP)
    // 2. epoll_wait(fd_data + fd_ctrl + cmd_queue_eventfd, 500ms)
    // 3. 处理 fd_data 事件：FSM 续传读取 BATCH → 完整后 send_return(RET_BATCH)
    // 4. 处理 fd_ctrl 事件：FSM 续传读取 HEARTBEAT/ERROR/EXIT/READY/FINISH
    //    - FINISH 特殊处理（v15.6.0）：转发 RET_FINISH 前先把 fd_data
    //      drain 至 EAGAIN——fd_data/fd_ctrl 是独立通道，Worker 协议保证
    //      先写完所有 BATCH 才写 FINISH，排至 EAGAIN 即完整，Master 见到
    //      FINISH 时该任务全部 BATCH 必已入账（屏障前提）
    // 5. 心跳检测：last_heartbeat > heartbeat_timeout ? SIGKILL + send_return(RET_DEAD)
}
```

### 5.4 主线程消息总线

```
while (running) {
    // 1. wait_for_ipc_messages：条件变量 + 自适应退避等待（v15.6.0，
    //    修复 cond 丢失唤醒导致的吞吐塌陷）；所有消息入队点持锁 signal，
    //    等待前复查队列状态
    // 2. 处理返回消息：
    //    - RET_BATCH(epoch)  → epoch 匹配 ? thread_pool 提交去重 : 丢弃
    //    - RET_FINISH(epoch) → epoch 匹配 ? slot→DT_BATCHES_RECEIVED : 丢弃
    //    - RET_DEAD   → 同代校验（pid 匹配才受理，v15.6.1）→ 先杀后清 + cleanup + spare 启用
    //    - RET_ERROR  → 同代校验（v15.6.1）+ 一处销账（pending_tasks--、IDLE）、写 spbin、
    //                   device_mgr_mark_probing，不重入队
    //    - RET_DEV_TIMEOUT → 同 RET_DEAD（Worker 侧同一任务只报一次，v15.6.1 节流）
    //    - RET_EXIT   → 同代校验；携带在途任务 = 非预期死亡（重入队 + 告警，v15.6.1）
    //    - RET_READY  → Worker→IDLE
    // 3. drain_completed_batches（线程池完成回调，推进 batches_processed）
    // 4. advance_task_barriers（仅主线程）：完结已达屏障的目录任务
    //    （写 dpbin、pending_tasks--、Worker 置 IDLE）
    // 5. 泵送历史 pbin 目录（恢复时）
    // 6. 收割 Worker 僵尸进程（waitpid(-1, WNOHANG)，唯一 owner）
    // 6.5 替换死亡 Worker（spare 池；屏障接管态 DT_BATCHES_* 的 slot 延迟替换）
    // 7. dispatch_from_queue（派发 dispatch_queue 中的任务）
    // 8. dspill 回填（dispatch_queue < LOW_WATER 时）
    // 9. 账目不变量：pending_tasks < 0 → log_fatal + _exit(2)（v15.6.1）；
    //    终止条件检查：pending_tasks<=0 && dispatch_queue 空 &&
    //    dspill 排空 && 无历史可泵送（完结硬性断言，见 §9.5）
}
```

### 5.5 目录任务生命周期状态机

目录任务的生命周期由两部分咬合承载：**三集合成员身份**（discovered_set / enqueued_set / completed_set，表达发现/入队/完成宏观阶段）+ **每 slot 在途任务屏障**（`DT_*`，表达单任务从派发到完结的微观阶段）。

**集合视角**：

```
readdir 发现 ──batch_processor 去重──> discovered_set（已写 pbin，崩溃可恢复）
      │
      │ enqueue_dir（completed 差集剪枝* → enqueued 去重 → 队列优先/dspill 兜底）
      ▼
enqueued_set（在 dispatch_queue 或 dspill 中）
      │
      │ dispatch_from_queue 弹出 → CMD_SCAN
      ▼
在途（slot 屏障，见下）
      │
      │ 屏障完结 → dpbin_append
      ▼
completed_set（终态；恢复时差集 = discovered − completed）

* 剪枝守卫：仅当目录同时在 discovered_set（pbin 有发现记录）才采信 completed
  剪枝，否则重新入队重扫（at-least-once，宁重复不丢失）
```

**slot 屏障视角**（`include/ipc/worker_proc.h`，仅主线程写）：

| 状态 | 含义 | 进入条件 |
|------|------|---------|
| `DT_NONE` | 无在途任务 | 初始 / 完结后 |
| `DT_SCANNING` | 已派发，Worker 扫描中 | CMD_SCAN 发送成功（pending_tasks++、epoch 写入） |
| `DT_BATCHES_RECEIVED` | 收到 FINISH，本任务全部 BATCH 已到齐 | RET_FINISH 且 epoch 匹配（IPC 线程已先 drain fd_data） |
| `DT_BATCHES_PROCESSED` | 线程池已处理完本任务全部 BATCH | `batches_processed == batches_received` |
| `DT_COMPLETED` | 输出 COMMITTED，dpbin 已写，任务完结 | `output_pending[slot]` 归零 → 写 dpbin → pending_tasks-- → Worker 置 IDLE |

**完成屏障顺序（P0-001，固定不变）**：FINISH 到达 → 全部 BATCH 处理完 → 输出全部 COMMITTED → **才写 dpbin** 并置 IDLE。FINISH 不再单独构成完成语义，FINISH/BATCH 竞态从结构上消除。

**输出三态（P0-002）**：输出条目 `DISCOVERED → OUTPUT_QUEUED → OUTPUT_COMMITTED`；每 slot 维护未 COMMITTED 批次计数 `output_pending`（按 worker 数动态分配），输出线程 fflush 后递减。Worker 死亡/任务重扫时未 COMMITTED 部分随目录回滚重扫，COMMITTED 部分以 at-least-once 语义截断损坏尾部后续传。

**错误路径**：
- `RET_ERROR`：目录登记 enqueued_set 后**不重入队**（DEVICE_WAITING 语义），写 spbin 等恢复路径重试；`record_path` 对运行期出错目录绕过 enqueue_dir 集合去重直写 pbin，保证恢复闭环可达。
- Worker 死亡：未达 DT_COMPLETED 的目录回滚重扫（current_path 重入队），未 COMMITTED 输出随目录回滚。
- 永久跳过（PERMISSION / CIRCUIT_BREAKER / POISON）：spbin 记账，不阻塞本次运行其他目录，但本次运行 status=Incomplete（baseline_eligible=0）。

**关键规则**：
- **只有 DT_COMPLETED 写 dpbin**
- 完结校验以状态机账目为准——"流程完结"与"数据完整"由同一套账目推出
- 屏障推进仅主线程执行（`advance_task_barriers`），无线程池并发写 dpbin

---

## 6. 模块详细设计

### 6.1 core — 配置与生命周期

**Config** 全局配置字段（关键项）：

| 字段 | 说明 | 默认值 |
|------|------|--------|
| `target_path` | 扫描根路径 | — |
| `output_file` | 输出文件 | — |
| `progress_base` | 进度文件前缀（`-f`） | — |
| `reference_base` | 盲信基准进度前缀（`--reference-base`，v15.6.0） | — |
| `continue_mode` | 断点续传 | false |
| `skip_interval` | 盲信扫描阈值（秒） | 0（关闭） |
| `batch_size` | Worker batch 大小 | 1024 |
| `estimated_files` | HashSet 预分配 | 1000 万 |
| `worker_count` | Worker 数（0=自动 `min(2×CPU, 8)`，上限 64 钳制） | 0（自动） |
| `heartbeat_timeout` | 心跳超时 | 120s |
| `strict_nlink` | nlink oracle | false |

**AppContext** 运行时上下文核心字段：
- `discovered_set`：已发现条目（目录严格只含目录语义见 §6.3），防重复发现
- `enqueued_set`：已入队目录（dispatch_queue 或 dspill），防重复入队
- `completed_set`：dpbin 恢复出的已完成目录集合
- `reference_map` / `reference_set`：盲信基准（纯路径指纹 → 完整历史 stat）
- `spbin_entries` / `spbin_set`：跳过目录内存账目与指纹集合
- `worker_pool`、`probe_scheduler`、`dev_mgr`：进程/设备管理
- `dispatch_queue`：Stage 3→4 队列
- `ipc_cmd_queues[N]`、`ipc_ret_queues[N]`：消息队列（容量 65536）
- `pending_tasks`（原子）、`pending_batches`（原子）：任务计数
- `output_pending[N]`（原子，按 worker 数动态分配）：每 slot 未 COMMITTED 输出批次计数
- `epoch_counter`（原子）：派发 epoch 计数器；`slot->current_epoch` 校验返回消息
- `dspill_fp` / `dspill_read_offset`：派发兜底文件与读游标
- `redispatch_backoff_until[MAX_WORKERS]`、`timeout_paths[MAX_WORKERS]`、`timeout_counts[MAX_WORKERS]`：退避与熔断状态（v15.6.0 统一按 `MAX_WORKERS=64` 定长，修复越界读活锁）
- `next_dispatch_worker`：派发轮询游标，取模回卷（`next = (candidate + 1) % num_workers`），值域恒在 `[0, num_workers)`，修复有符号 int 溢出野读段错误

### 6.2 ipc — 进程间通信

**三通道 Pipe 模型**（v15.0.0）：

| 通道 | 方向 | 语义 | 写入者 | 阻塞策略 |
|------|------|------|--------|----------|
| `fd_cmd` | M→W | SCAN / STOP | Master | 阻塞写，非阻塞读 |
| `fd_data` | W→M | BATCH（大 payload） | Scanner 线程 | 阻塞写，非阻塞读 |
| `fd_ctrl` | W→M | HEARTBEAT/ERROR/EXIT/READY/FINISH | IPC 线程 | 阻塞写，非阻塞读，< PIPE_BUF |

**消息队列**（Master ↔ IPC 线程）：容量 **65536** 条（v15.6.0，原 1024），mutex + eventfd 通知。容量瓶颈收敛在 dispatch_queue 而非 IPC 层。

**MSG_DROP 废除（v15.6.0）**：正常路径不再存在丢弃语义——
- CMD_SCAN 发送遇队列满：任务回队下轮重试；`pending_tasks` 仅在派发成功后 +1，未生效的 epoch 回滚，账实一致，杜绝旧版 MSG_DROP 回队不销账导致的 pending_tasks 永久泄漏。
- CMD_REPLACE 丢失会导致 IPC 线程永久等待新 fd：队列满时短暂重试（100×1ms），仍失败属设计外异常，`log_fatal` 暴露，不静默丢弃。
- ret_queue 满（65536 条返回消息积压）属设计外异常，`log_fatal`。

**FSM 续传**（v15.4.0）：跨 `epoll_wait` 的可恢复读取，状态 `IPC_READ_IDLE → HDR → PAYLOAD → FOOTER`。

### 6.3 scan — 扫描引擎

**Worker Scanner**（`worker_scanner.c`）：
- `opendir(path) → readdir 循环 → lstat 每个条目 → 区分文件/目录`
- 文件：收集进 batch → 满 `batch_size` 时 `send_batch(fd_data, epoch)`
- 目录：收集进 batch → 由 batch_processor 后续处理
- 盲信检查（若启用）：`reference_map` 纯路径指纹命中且 d_type 一致且 mtime 超窗 → 不执行 lstat，直接复用基准 stat 数据
- `scanner_progress_tick()` 每 5s 更新（时间驱动），防止 NFS 大目录误判

**Batch Processor**（`batch_processor.c`）：
1. 解析 BATCH 数据
2. 计算 `path+dev+ino` 的 128-bit 指纹，查 `discovered_set` 防环（目录严格只含目录）
3. 新目录：`write_pbin()` + `enqueue_dir()`（统一入队入口）；恢复阶段（HIST_PUMP_OLD）写 fpbin
4. 新文件：`write_pbin()` + `record_path_batch_append()` → 异步输出线程（**pbin 全量记录，不再要求 `-c`**；`--clean` 模式由 record_path 内部早退）
5. 输出条目提交前 pbin 先记（DISCOVERED → OUTPUT_QUEUED → OUTPUT_COMMITTED 顺序不变）

**统一队列模型**（v15.6.0，`enqueue_dir` 是唯一入口）：
```
enqueue_dir(path):
  → completed 差集剪枝（前提：同时在 discovered_set，缺陷 1/2 守卫）
  → enqueued_set 去重
  → dispatch_queue < HIGH_WATER（10万）? 入内存队列 : 写 dspill 兜底
恢复泵送、spbin 重入队、dspill 回填全部走同一入口
```

**Dispatch**（`dispatch.c`）：
- `dispatch_find_idle_worker()`：轮询游标取模回卷，永不溢出
- `dispatch_from_queue()`：弹出目录 → 遇退避期 skip → CMD_SCAN 成功才 pending_tasks++ / 初始化 slot 屏障（DT_SCANNING）
- `cleanup_dead_worker_slot()`：死亡 Worker 的 current_path 重入队（未完结目录回滚重扫），目录级熔断检查

**Device Manager**：
- 状态：`NORMAL → PROBING → DEAD → CONDEMNED`
- 无锁读（`_Atomic`），mutex 保护写
- 渐进探测：指数退避 5s→10s→20s→...→300s

### 6.4 output — 输出与监控

**输出模式**：单文件（`-o`）或按行数分片（`-O`）。

**异步输出线程**：线程安全队列，8MB 全缓冲，批量刷盘；fflush 后递减 `output_pending`（OUTPUT_COMMITTED 回调），只写输出文件——**pbin/dpbin/fpbin 等进度文件由 batch_processor / 主线程屏障路径写入，不由输出线程代写**。

**Run manifest**（`manifest.c`，v15.6.0）：唯一权威状态来源，见 §8.7。

**Monitor**：500ms 刷新，`[MM:SS] dirs: X files: Y rate: Z/s active: A/B pending: P devs: D probing E dead`；设备探测调度与敢死队探测进程收割（`reap_probes`）。

---

## 7. 通信协议

### 7.1 三通道语义分离

```
Master                                    Worker
   │                                       │
   ├──── fd_cmd ────> [SCAN path, epoch]   │
   ├──── fd_cmd ────> [STOP]               │
   │                                       │
   │<──── fd_data ─── [BATCH records, epoch] │ Scanner 线程
   │<──── fd_data ─── [BATCH records, epoch] │
   │                                       │
   │<──── fd_ctrl ─── [HEARTBEAT]          │ IPC 线程
   │<──── fd_ctrl ─── [READY]              │
   │<──── fd_ctrl ─── [FINISH, epoch]      │
   │<──── fd_ctrl ─── [ERROR]              │
   │<──── fd_ctrl ─── [DEV_TIMEOUT]        │
   │<──── fd_ctrl ─── [EXIT]               │
```

### 7.2 IPC 消息格式

**Header（8 字节，packed）**：`msg_type(4) + payload_len(4)`

**Master → Worker（fd_cmd）**：

| 值 | 名称 | 说明 |
|----|------|------|
| 1 | `IPC_MSG_SCAN` | 扫描任务（携带路径与 epoch） |
| 6 | `IPC_MSG_STOP` | 停止 Worker |

**Worker → Master（fd_data / fd_ctrl）**：

| 值 | 名称 | 通道 | 说明 |
|----|------|------|------|
| 2 | `IPC_MSG_BATCH` | fd_data | 扫描结果批次（携带 epoch） |
| 3 | `IPC_MSG_HEARTBEAT` | fd_ctrl | 心跳 |
| 4 | `IPC_MSG_ERROR` | fd_ctrl | 设备级错误（错误路径，不再伴随 FINISH） |
| 5 | `IPC_MSG_EXIT` | fd_ctrl | 正常退出 |
| 7 | `IPC_MSG_DEV_TIMEOUT` | fd_ctrl | Scanner 自检测超时 |
| 8 | `IPC_MSG_READY` | fd_ctrl | Worker 初始化完成 |
| 9 | `IPC_MSG_FINISH` | fd_ctrl | 当前任务 readdir 结束（携带 epoch；不代表目录完成，见 §5.5） |
| 10 | `IPC_MSG_ENTRY_ERROR` | fd_ctrl | 条目级错误（v15.5.7） |

**BATCH payload**：`[IpcBatchHeader: count] + [count × {path_len, path, stat}] + [footer_magic]`

### 7.3 Master ↔ IPC 线程消息

**命令（Main → IPC）**：`CMD_SCAN`、`CMD_REPLACE`、`CMD_STOP`

**返回（IPC → Main）**：`RET_BATCH`、`RET_HEARTBEAT`、`RET_ERROR`、`RET_DEAD`、`RET_EXIT`、`RET_DEV_TIMEOUT`、`RET_READY`、`RET_FINISH`、`RET_ENTRY_ERROR`

**注意**：v15.6.0 已废除 `MSG_DROP`（见 §6.2）。返回队列容量 65536，满属设计外异常。

### 7.4 FSM 续传协议

跨 `epoll_wait` 调用的可恢复读取：`IPC_READ_IDLE → HDR → PAYLOAD → FOOTER`。`EAGAIN` 时不释放 buf、不重置 `nread`。`CMD_REPLACE` 时重置 FSM。

---

## 8. 数据持久化模型

### 8.1 pbin — 进度分片（全量扫描记录）

记录**所有已发现的条目**（目录 + 文件），**v15.6.0 起全量记录，不再要求 `-c` 模式**——任何一次成功运行都可作为恢复来源与盲信基准。schema 2 二进制格式，每条记录结构为：

```
[path_len: size_t][path: N bytes][d_type: u8][mtime_sec: time_t][mtime_nsec: long]
[size: u64][uid: u32][gid: u32][mode: u32][dev: u64][ino: u64][flags: u32]
```

**atime 完全排除**（NFS 上不可信）。每 10 万行切分（`--progress-slice-lines` 可调），命名 `{base}_000000.pbin`。分片末尾写 Footer 自描述（记录数 + 校验）。

**平台兼容性约束**：pbin/fpbin/dpbin/dfpbin 的二进制字段宽度（`size_t`、`time_t`、`long` 等）与平台相关。**进度文件仅在同架构、同机器上可续传**，不保证跨 x86_64/aarch64、不保证跨 32/64 位、不保证跨大端/小端。

**schema 版本**：manifest 记录 `pbin_schema_version=2`；旧版（schema 1，无 size/uid/gid/mode 全集）不能续传、不能作盲信基准，检测到旧残留 exit 2，须 `--runone`。

**为什么同时记录文件**：pbin 是盲信扫描的基准来源。盲信门禁校验后从基准 pbin 重建 `reference_map`（纯路径指纹 → 完整历史 stat），命中即复用 size/mtime/uid/gid/mode 免 lstat。若 pbin 不记录文件，盲信对文件无从谈起。

**目录与文件的分流策略**：
- 正常扫描阶段（`HIST_PUMP_NEW/DONE`）：目录和文件都写入 pbin
- 恢复旧 pbin 阶段（`HIST_PUMP_OLD`）：新发现的目录写入 fpbin（原子对，见 §8.3），文件仍写入 pbin（文件不参与续传队列，无混淆问题）

### 8.2 dpbin — 完成日志

本次会话的"已完成目录集合"。恢复时 `discovered_set − completed_set = 待扫描目录`。只分片、不归档，正常结束后删除。

**写入时机（v15.6.0，P0-001）**：从"收到 FINISH"延迟到"目录任务屏障达到 DT_COMPLETED"——即 FINISH 到达、全部 BATCH 处理完、输出全部 COMMITTED 之后，由 `advance_task_barriers`（仅主线程）写入。FINISH 不再单独构成完成记账。

### 8.3 fpbin / dfpbin — 恢复临时缓存原子对

**fpbin**：恢复期间（HIST_PUMP_OLD）新发现的子目录先写入 fpbin（而非直接入队）。
**dfpbin**：恢复期间完成的目录写 dfpbin（格式同 dpbin）。

二者构成**原子对**，使恢复工作区可重入——恢复过程本身被 KILL（二次崩溃）后仍能再次进入恢复并收敛。

**流转**：

```
恢复开始：
  1. 检查 fpbin + dfpbin 完整性（Footer 校验；fpbin.idx 与分片残留纳入判定）
     ├── 完整 → fpbin 转正（泵送入队），dfpbin 合并入 dpbin
     └── 不完整 → 整对抛弃；无法对账的残留（如已 unlink 的原始分片
                  对应子树）由 Reset 援救兜底重扫（§9.6，at-least-once）

恢复闭环（旧 pbin 消费完，HIST_PUMP_OLD → DONE）：
  2. fpbin 转正：内容泵送入队，此后新发现子目录直接入队
  3. merge_dfpbin_into_dpbin：封口当前 dfpbin/dpbin 活跃分片（写 Footer
     避免序号冲突）→ 非空 dfpbin 分片 rename 到 dpbin 最大序号之后 →
     重置 dfpbin/dpbin 写状态；无 dfpbin 数据时为 no-op
  4. 此后不再存在 fpbin/dfpbin 残留，下次恢复走普通 dpbin 差集路径
```

**可抛弃语义**：fpbin + dfpbin 是临时工作区。不完整时整对抛弃、重新从旧 pbin 恢复，不会无限套娃。fpbin/dfpbin 残留未闭环是 `baseline_eligible=0` 的判定条件之一（§8.7）。

### 8.4 spbin — 跳过记录

**格式**（v15.6.0 扩展，append-only 流式读写）：
```
[path_len: u32][path: N bytes][reason: u8][timestamp: time_t][device_key: 64 bytes]
```
`device_key` 暂为 st_dev 的十进制字符串（P1-003 将升级为 (fsid, server, export) 三元组）。

**五类原因码**（`include/output/spbin.h`）：

| 值 | 名称 | 含义 | 恢复行为 |
|----|------|------|---------|
| 1 | `SP_REASON_PROBE_FAIL` | 设备探测失败（EIO/ENODEV/ESTALE 及未知 errno 保守归类） | 超窗后敢死队探测，设备活则重入队 |
| 2 | `SP_REASON_TIMEOUT` | 设备超时（ETIMEDOUT） | 同上 |
| 3 | `SP_REASON_CIRCUIT_BREAKER` | 目录级熔断（连续 DEV_TIMEOUT 达阈值） | 永久跳过 |
| 4 | `SP_REASON_PERMISSION` | 权限拒绝（EACCES/EPERM） | 永久跳过 |
| 5 | `SP_REASON_POISON` | 毒丸目录（累计致死 Worker 3 次，P1-004） | 永久跳过，隔离记账 |

**恢复行为**：
1. 按 `device_key` 分组
2. PERMISSION / CIRCUIT_BREAKER / POISON → 永久跳过（本轮不再入队）
3. PROBE_FAIL / TIMEOUT → 检查 timestamp 与退避窗口（按 retry_count 指数升级：30min → 2h → 6h → 24h 封顶，retry_count 不持久化、恢复时从 0 开始）
   - 未超窗 → 保持跳过（CONDEMNED）
   - 超窗 → 敢死队探测该设备
     - 成功 → 设备标记 NORMAL，整组经 `enqueue_dir` 重入队（条目标记 RECOVERED）
     - 失败 → timestamp 更新，退避升级
4. 运行期出错目录的重入队统一经 `spbin_requeue_recovered` → `enqueue_dir`；`record_path` 对出错目录绕过集合去重直写 pbin，保证恢复闭环可达

**毒丸隔离（P1-004）**：毒丸目录隔离到独立账目，不阻塞同设备其余目录的恢复。

**compaction**：正常退出时过滤 RECOVERED 条目重写 spbin（先于 archive，保证归档的是压缩后版本）。内存条目上限 `SPBIN_MAX_ENTRIES=100000`，超过告警并紧急 compaction。compaction 后 spbin 仍非空 = 有永久跳过目录 → 本次运行 `baseline_eligible=0`。

### 8.5 dspill — 派发兜底

**dspill 是 dispatch_queue 的磁盘扩展**（统一队列模型），不是独立缓存池。

**数据流**：
```
enqueue_dir 发现新目录
  → dispatch_queue < HIGH_WATER（10万）?
    → 是：入 dispatch_queue，标记 enqueued_set
    → 否：写 dspill（append-only），标记 enqueued_set
主线程每轮检查水位
  → < LOW_WATER（3万）：按字节游标从 dspill 读取回填 dispatch_queue
```

**特性**：
- append-only；崩溃恢复时从头读、经 `enqueue_dir` 回填（enqueued_set 去重）
- 无 Footer，流式读取，EOF 即排空
- **完结硬性断言**：正常完结时 dspill 必须排空到 EOF（残留字节 = 0）；有残留即部分完成（exit 1，`baseline_eligible=0`）
- 成功完结后删除 dspill 文件；有错误时保留供审计

### 8.6 archive — 压缩归档

gzip 压缩的 pbin 块 + spbin 块。`block_type = 0/1` 区分。

**原子切换（v15.6.0，P0-008）**：运行期所有 archive 块追加到 `{base}.archive.new`；finalize 校验通过后 rename 覆盖 `{base}.archive`，旧基准保留为 `{base}.archive.prev`。校验失败即 `archive_ok=false` → `baseline_eligible=0`。

**平台兼容性约束**：archive 中的 pbin 块继承同平台的二进制字段宽度，进度文件仅同机同架构可用（见 §12 平台兼容性约束）。

### 8.7 Run manifest — 权威运行状态

**`{base}.manifest`（v15.6.0，P0-008）是运行状态的唯一权威来源**，替代旧版 `.config` 追加写——旧实现向 .config 追加积累多行 status，strstr/逐行匹配会把崩溃残留的旧 "Success" 误判为当前状态，不完整运行链式成为盲信基准。

**写入语义**：
- `.new → fflush + fsync → rename` 原子切换，key=value **单行覆盖**
- 启动即写 `status=Running` 覆盖上次终态
- 终态由 `manifest_finalize` 统一判定并原子写入

**关键字段**：`run_id`、`target_path`、`schema_version`、`tool_version`、`status`（Running/Success/Incomplete）、`started_at`、`finished_at`、`dirs`、`files`、`skipped`、`errors`、`spbin_count`、`dspill_residue_bytes`、`baseline_eligible`、`baseline_run_id`、`baseline_completed_at`、`output_offset`、`output_line_count`、`pbin_schema_version`。

**`baseline_eligible=1` 的严格条件（同时满足，任一不满足即为 0）**：
1. 本轮非盲信运行（盲信结果不能链式作为基准）
2. `status=Success`（无 skipped/熔断/设备错误）
3. spbin 残留为 0（compaction 后仍非空 = 有永久跳过目录）
4. dspill 已排空（残留字节为 0）
5. 无 fpbin/dfpbin 残留（恢复工作区已闭环）
6. archive（若启用）写出校验通过
7. 输出尾部完整（最后一字节为 `'\n'`）

**与 .config 的关系**：`.config` 继续按原样**单次写入**（`fopen("w")` 覆盖，兼容保留），内容含 path/output/start_time/archive/clean/csv/status=Running；archive 策略一致性校验仍读 `.config`（manifest 不收录该字段）。**一切状态判断以 manifest 为准**。

---

## 9. 故障处理与容错

### 9.1 Worker 死亡与替换

```
IPC 线程检测超时/error/hup
    ├── SIGKILL Worker（不阻塞等待，避免 D-State 挂起）
    └── send_return(RET_DEAD / RET_DEV_TIMEOUT / RET_EXIT，携带 pid+epoch，v15.6.1)
        ▼
Main: 同代校验（reported_pid == slot->pid，否则按跨代残留丢弃，v15.6.1）
      waitpid 确认旧进程死亡（替换前置条件，v15.6.0；cleanup 内先杀后清，v15.6.1）
      close 前旧 fd 设非阻塞 drain 至 EAGAIN（清除旧 Worker 残留数据）
      cleanup_dead_worker_slot() → current_path 重入队（未完结目录回滚重扫）；
                                   销账精确化：仅 DT_SCANNING 销账，DT_BATCHES_*
                                   由完成屏障照常完结（免重扫），空闲死亡不销账（v15.6.1）
      worker_pool_replace() → 从 spare 池启用预 fork Worker（运行期零 fork，v15.6.1）；
                              spare 耗尽 → 全局告警 + 降额运行，看门狗兜底（§9.5）
      send_replace_to_ipc() → IPC 线程更新 fd/pid，重置 FSM
      （此后旧 epoch 的迟到 BATCH/FINISH/心跳一律被 epoch 校验过滤，
        旧 pid 的死亡类消息一律被同代校验过滤，v15.6.1）
```

### 9.2 设备级熔断

`RET_ERROR` → `dev_mgr_mark_probing()` → 敢死队探活 → 成功则 alive，失败则指数退避 → `PROBE_MAX_RETRIES` 次后 `CONDEMNED`。

**RET_ERROR 处理（v15.6.0，P0-007）**：
- Worker 错误路径不再发 FINISH；**一处销账**：RET_ERROR 处理器内 pending_tasks--、Worker 置 IDLE（不经完成屏障）
- 当前目录**不重入队**（避免重试风暴）
- 先写 spbin（原因码按 errno 归类）后销账——崩溃不一致时宁可 spbin 多记，恢复时多扫不漏扫
- 目录登记 enqueued_set，等设备恢复信号统一经 spbin 恢复路径重试

### 9.3 目录级熔断与退避

同一目录连续 DEV_TIMEOUT 超过 `CIRCUIT_BREAKER_THRESHOLD (10)` 则跳过。redispatch 指数退避：1 次 30s、2 次 120s、≥3 次 300s。熔断/退避状态数组按 `MAX_WORKERS=64` 定长，`circuit_breaker_check` 宽限守卫同步使用 `MAX_WORKERS`。

**毒丸机制（v15.6.0，P1-004）**：同一目录累计致死 Worker 3 次，直接进隔离清单（spbin `SP_REASON_POISON`），不阻塞同设备其他目录。

### 9.4 NFS 大目录防误判

六层防御：HEARTBEAT_TIMEOUT 120s、熔断阈值 10、时间驱动 tick 5s、opendir tick、send_batch tick、redispatch 退避。

### 9.5 扫描完整性断言

- nlink oracle（`--strict-nlink`）：`st_nlink - 2` 应等于子目录数
- **账目不变量（v15.6.1，P0-104）**：主循环每轮巡检 `pending_tasks < 0` → 立即
  `log_fatal` + 杀光存活 Worker + `_exit(2)`——账目 bug 秒级暴露，不得死等
- **有效进展看门狗（v15.6.1，P0-105）**：monitor 线程监督 file+dir 计数——无增长且
  仍有应做工作（pending_tasks!=0 或 dispatch_queue 非空或存在 BUSY Worker）持续超
  `--stall-timeout`（默认 900s，须大于最大退避 300s）→ 输出现场（全 slot 状态/账目/
  队列深度）并按 `--stall-action=exit|abort` 终止。设备探测等待/spbin 积压场景
  pending==0 且队列空，不误报。设计要点：主循环 tick 看门狗对"活而无效"（循环在转
  但无进展）无效，必须按有效进展判定
- **完结硬性断言**：`pending_tasks<=0` && dispatch_queue 空 && dspill 排空到 EOF && 无历史可泵送，任一不满足即部分完成（exit 1）
- 熔断清单非空 → exit 1
- spbin 残留 / fpbin/dfpbin 残留 / archive 校验失败 / 输出尾部不完整 → status=Incomplete、`baseline_eligible=0`（§8.7）

### 9.6 Reset 援救机制

**核心原则**：不枚举崩溃点。恢复中遇到无法对账的残留状态（分片截断、索引与分片矛盾、fpbin/dfpbin 不完整对）时，回退到安全的全量重扫语义（at-least-once），而非带伤续跑——已 unlink 的原始分片对应子树由 Reset 援救兜底重扫，**宁可重复输出也不丢目录**。

```
任何目录只要状态 ≠ 已完成（未写 dpbin / 无 pbin 发现记录兜底）
  → 恢复时视为未扫描，重新入队重扫
  → 重扫幂等：pbin/discovered_set 去重保证不重复入账，输出按
    at-least-once 语义截断损坏尾部后续传

spbin 按 §8.4 处理：按原因码分流，超窗探测，设备活了统一入队
fpbin/dfpbin 按 §8.3 处理：完整则转正合并，不完整整对抛弃重来
```

**退出码语义（v15.6.0 统一）**：

| 退出码 | 含义 | 触发 |
|--------|------|------|
| 0 | 完全完成 | 所有目录完结，spbin 清零，dspill 排空，输出尾部完整 |
| 1 | 部分完成 | 有跳过/spbin 残留/dspill 残留/熔断清单非空/输出尾部不完整（status=Incomplete，不可作基准） |
| 2 | 严重失败 | 盲信基准不合格、盲信 `-f` 非新空目录、进度不兼容（旧格式残留）、初始化致命错误 |
| 3 | 架构不匹配 | **预留**（本期未接入平台检查，见 §12） |

---

## 10. 性能考量

| 优化点 | 效果 |
|--------|------|
| 盲信跳过（有合格基准时） | 命中条目免 lstat，直接复用基准 stat；I/O 降幅取决于变更比例，无固定承诺值 |
| HashSet 预分配 | 避免 rehash |
| 8MB 输出缓冲 | 减少 write syscall |
| 批量 record_path | 减少 fwrite |
| dispatch_queue 环形缓冲 | O(1) pop |
| 去重线程池 | CPU 并行 |
| dspill 统一背压 | 内存 hard cap 10 万条，磁盘兜底 |
| IPC 队列 65536 | 废除 MSG_DROP，背压收敛到 dispatch_queue 一处 |
| 主循环条件变量 + 自适应退避 | 消除空转 CPU 与丢失唤醒（曾致吞吐降约 10 倍） |

---

## 11. 安全考量

- `MAX_PATH_LENGTH = 4088`，确保原子写入
- `payload_len > 100MB` 时 `log_fatal`
- Worker 替换时 waitpid 确认死亡 + drain 旧 fd 残留 + epoch 过滤迟到消息
- 僵尸进程由主线程 `waitpid(-1, NULL, WNOHANG)` 收割（唯一 owner）
- `--worker-count` 超上限钳制到 `MAX_WORKERS=64` 并告警（per-slot 数组按 MAX_WORKERS 定长）
- 盲信基准必须经门禁校验（§4.3），伪造/不合格基准 exit 2
- 版本限定日志的两条门控规则（见 §12 工程约束）：元数据相关异常日志与 error 级日志不得被版本门控

---

## 12. 部署与运维

### 编译
```bash
cd /root/listfiles && make clean && make
```
Makefile 已启用 `-MMD -MP` 头文件依赖跟踪（v15.6.0）：头文件变更（如 AppContext 布局）会触发全量重编，杜绝新旧目标文件混用结构体偏移。

### 典型参数
```bash
# 全量扫描
./listfiles -p /public2/data -o /tmp/output.txt -f /tmp/progress

# 续传（同机同架构；旧格式进度会被拒绝，须 --runone）
./listfiles -p /public2/data -o /tmp/output.txt -f /tmp/progress -c

# 盲信扫描（基准须 baseline_eligible=1 且 schema 2；本轮 -f 必须为新空目录）
./listfiles -p /public2/data -o /tmp/output_blind.txt -f /tmp/progress_blind \
    -c --skip-interval=604800 --reference-base=/tmp/progress

# 强制重跑（覆盖旧进度）
./listfiles -p /public2/data -o /tmp/output.txt -f /tmp/progress --runone

# 显式 Worker 数（上限 64）
./listfiles -p /public2/data -f /tmp/progress --worker-count=16
```

### 平台兼容性约束

pbin/fpbin/dpbin/dfpbin/archive 的二进制字段宽度（`size_t`、`time_t`、`long` 等）与平台相关，**进度文件仅同机同架构可续传/作基准**，不保证跨架构、跨字长、跨端序。退出码 3（架构不匹配）为**预留**：本期未接入运行时平台签名强制校验，跨架构使用进度文件属未定义行为，须 `--runone` 重新建立。

### 工程约束

- **版本号**：`VERSION "15.6.1"`、`VERSION_NAME "v15.6.1"`、`VERSION_CODE 202608241500UL`（`include/core/config.h`）。
- **版本限定日志写死调用点**：v15.6.0 周期的 4 处版本限定日志写死 `202608202330UL`；v15.6.1 新增的版本限定日志写死 `202608241500UL`。均不定义宏——防止宏值随版本递进被一改全改、旧日志被不断宽限而失去门控意义。
- **门控严格遵循规则**（config.h 注释）：
  1. 可能导致文件元数据被忽略或丢失的异常日志，不得被版本门控，必须归属于全局日志（引用 VERSION_CODE 的 `log_*` 宏）；
  2. error 类型日志不得被版本门控——`log_error`/`log_fatal` 在宏定义层固定引用 VERSION_CODE，结构上无法被门控。
  v15.6.1 按此规则清扫了 DEV_TIMEOUT 全链路、cmd_queue 满丢 SCAN、熔断跳过写 spbin、FINISH 发送最终失败等历史违规点（生产事故两天无人察觉的直接原因就是这些日志被旧门控码吞掉）。
- **运行环境注意**：生产运行日志务必重定向到文件（`2> run.log`）；严格超售（`vm.overcommit_memory=2`）环境下 v15.6.1 运行期零 fork，启动期关注 `[Spare]` 日志确认预备役就绪。

### 典型参数补充（v15.6.1）
```bash
# 有效进展看门狗（默认 900s 触发；0=禁用；动作 exit|abort）
./listfiles -p /public2/data -f /tmp/progress --stall-timeout=900 --stall-action=exit
```

### NFS 挂载要求
```bash
mount -t nfs -o soft,timeo=6000,retrans=3 server:/public2 /public2
```

**参数说明**：
- `soft`：元数据操作超时时返回错误（避免 D-State 不可杀）
- `timeo=6000`：超时时间为 600 秒（单位是十分之一秒）
- `retrans=3`：超时后重传 3 次
- **注意**：`intr` 在 CentOS 7.4 内核（3.10）中无效，已移除

---

## 13. 附录：版本演进摘要

| 版本 | 时间 | 架构调整 | 解决的问题 |
|------|------|---------|-----------|
| v12.0.0 | 2026-04 | 线程 → 进程 | D-State 不可杀死 |
| v13.0.0 | 2026-05 | IPC 线程隔离 | 单线程 epoll 瓶颈 |
| v14.0.0 | 2026-05 | Worker 多线程化 | 扫描阻塞不响应 |
| v15.0.0 | 2026-05 | 三通道分离 + IPC 状态机 | mutex 阻塞心跳 |
| v15.5.9 | 2026-08 | NFS 大目录防误判加固 | HEARTBEAT_TIMEOUT 过严 |
| v15.6.0 | 2026-08 | 目录任务完成屏障 + 输出三态 + fpbin/dfpbin 原子对 + 三集合统一队列 + Run manifest + epoch 机制 + spbin 恢复路径 + pbin schema 2 盲信门禁 | FINISH/BATCH 竞态、输出无状态、二次崩溃恢复、背压竞态、MSG_DROP 丢任务、RET_ERROR 空转、状态无权威来源、盲信前提不受强制、旧 Worker 残留污染；另修复 next_dispatch_worker 溢出段错误与 per-slot 数组越界活锁 |
| v15.6.1 | 2026-08 | 死亡类消息 (pid,epoch) 同代校验 + 预备役 Worker 池（运行期零 fork）+ fork 时序前移 + 有效进展看门狗 + 账目不变量 | /public4 生产事故：RET_DEAD stale 判定反转吞噬真实死亡、cleanup 无条件销账致 pending=-6、RET_EXIT 丢在途目录致子树静默丢失、严格超售下 fork ENOMEM 静默 54h、DEV_TIMEOUT 链路日志被门控吞掉、无"活而无效"看门狗 |
