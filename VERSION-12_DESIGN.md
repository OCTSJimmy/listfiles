# VERSION-12 设计蓝图

> 本文件记录将 listfiles 从旧线程模型重构为进程模型（V11 → V12）的完整设计决策。

## 1. 重构背景与目标

### 1.1 旧模型的问题

旧版采用**多线程共享内存**架构：

- `g_looper_mq` / `g_worker_mq`：带对象池的消息队列
- `g_pending_tasks`：原子计数器
- `g_visited_history` / `g_reference_history`：全局 HashSet

该模型在以下场景下存在致命缺陷：

1. **D-State 死锁**：Worker 线程在执行 `lstat`/`readdir` 时，若底层 NFS/SAN 设备无响应，会陷入不可中断睡眠（D-State）。`pthread_kill` 无法终止 D-State 线程，导致整个进程僵死。
2. **消息队列瓶颈**：带锁的消息队列在高并发目录分发时成为瓶颈。
3. **全局变量污染**：大量全局变量导致状态难以追踪，测试和复用困难。

### 1.2 新模型的核心目标

- **Worker 必须可独立杀死**：使用 `fork()` 创建的独立进程，可通过 `SIGKILL` 强制终止，即使处于 D-State 的进程也可被内核回收。
- **零拷贝共享只读上下文**：利用 Linux COW（写时复制），在 `fork()` 后父进程不再修改共享数据，子进程以只读方式访问 `Config`、`FingerprintSet`、`ReferenceMap`。
- **统一状态管理**：所有模块状态收敛到单一 `AppContext` 结构体中。

## 2. 架构总览

```
┌─────────────────────────────────────────────────────────────┐
│                        Master Process                        │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │   AppContext │  │  epoll fd   │  │   AsyncWriter Thread│  │
│  │  (all state) │  │             │  │   (output queue)    │  │
│  └──────┬──────┘  └──────┬──────┘  └─────────────────────┘  │
│         │                │                                   │
│  ┌──────▼──────┐  ┌──────▼──────┐  ┌─────────────────────┐  │
│  │  WorkerPool │  │ DeviceManager│  │  ProbeScheduler     │  │
│  │  (fork/pipe)│  │ (state mutex)│  │  (min-heap by time) │  │
│  └──────┬──────┘  └─────────────┘  └─────────────────────┘  │
│         │ pipe TLV IPC                                         │
├─────────┼─────────────────────────────────────────────────────┤
│         ▼                                                     │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │ Worker Child Process (fork)                              │  │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐  │  │
│  │  │  ipc_recv   │  │ scan_dir()  │  │ try_blind_trust │  │  │
│  │  │  IPC_MSG_SCAN│  │ readdir loop│  │ (ref_set/map)   │  │  │
│  │  └─────────────┘  └─────────────┘  └─────────────────┘  │  │
│  └─────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

## 3. 模块设计

### 3.1 FingerprintSet（去重集合）

- **哈希函数**：内嵌公共域 MD5（RFC 1321），对 `path + dev + ino` 计算 128-bit 指纹。
- **冲突解决**：开放寻址线性探测。
- **容量策略**：`next_pow2(expected_count * 2)`，确保负载因子 < 0.5。
- **内存布局**：每个条目固定 16 字节，无指针，无业务数据，纯指纹存储。

### 3.2 ReferenceMap（半增量索引）

- **与 FingerprintSet 同构**：共享相同的哈希函数和容量策略，但每个条目存储 `mtime` 和 `d_type`。
- **用途**：Worker 在 `readdir` 时获取到 `d_ino` 和 `d_type`，若 fingerprint 命中 ReferenceMap 且满足时间阈值，则构造伪 `stat` 跳过 `lstat`。

### 3.3 WorkerPool（进程池）

- **创建**：`pipe2(O_CLOEXEC)` 创建两对管道，`fork()` 后子进程关闭所有无关 fd（Windows 环境使用 `close_range` 的 fallback：遍历 fd 表）。
- **替换**：当 Worker 心跳超时或主动上报错误时，Master 执行 `epoll_ctl(DEL)` → `close(fd)` → `fork()` → `epoll_ctl(ADD)`。
- **COW 安全**：父进程在 `worker_set_context()` 后不再修改 `Config`、`FingerprintSet`、`ReferenceMap`，确保子进程看到的是稳定快照。

### 3.4 IPC 协议（TLV）

```c
IpcMessageHeader { uint32_t msg_type; uint32_t payload_len; }
```

| 消息类型 | 方向 | 说明 |
|---------|------|------|
| `IPC_MSG_SCAN` (1) | M → W | 发送待扫描目录路径 |
| `IPC_MSG_BATCH` (2) | W → M | 目录扫描结果批次 `(count, [(path_len, path, struct stat)...])` |
| `IPC_MSG_HEARTBEAT` (3) | W → M | Worker 定期心跳，含时间戳 |
| `IPC_MSG_ERROR` (4) | W → M | Worker 扫描错误，含 `errno` 和设备 ID |
| `IPC_MSG_EXIT` (5) | W → M | Worker 收到 STOP 后正常退出 |
| `IPC_MSG_STOP` (6) | M → W | 请求 Worker 停止（优雅退出） |

**注意**：`struct stat` 直接通过 `memcpy` 序列化，协议仅在**同一台机器**上使用（不跨架构）。

### 3.5 MainLoop（epoll 事件循环）

- **事件源**：所有 Worker 的 `fd_out` 通过 `epoll_ctl(ADD, EPOLLIN)` 注册。
- **超时**：`epoll_wait(..., 500)`，500ms 超时用于定期执行 Monitor 巡检。
- **消息处理**：
  - `BATCH` → 解析批次 → 指纹去重 → 设备黑名单检查 → 目录入队 / 文件提交输出线程
  - `HEARTBEAT` → 更新 `slot.last_heartbeat`
  - `ERROR` → 若 `errno` 为 `ETIMEDOUT`/`EIO`，触发设备熔断
  - `EXIT` → 标记 Worker 死亡，等待 `waitpid` 收割

### 3.6 Monitor（巡检子系统）

嵌入在 `MainLoop` 的 `epoll_wait` 超时路径中，包含三个子例程：

#### 3.6.1 心跳超时检测（`monitor_check_timeouts`）

- 遍历所有 WorkerSlot，若 `now - last_heartbeat > heartbeat_timeout`（默认 30s），则 `kill(SIGKILL)` 并标记死亡。
- Worker 的当前设备路径被写入 `spbin`，并注册到 `ProbeScheduler`。

#### 3.6.2 敢死队探测发射（`monitor_dispatch_probes`）

- 检查 `ProbeScheduler` 小根堆，若堆顶任务的 `next_probe_time <= now`：
  - fork 子进程，设置 `alarm(PROBE_TIMEOUT_SEC)`，执行 `lstat(probe_path)`。
  - 子进程无论成功失败都立即 `_exit(0)`，返回值不重要——重要的是父进程能否通过 `waitpid` 收割。

#### 3.6.3 探测结果收割（`monitor_reap_probes`）

- `waitpid(WNOHANG)` 检查敢死队进程：
  - **正常退出（`WIFEXITED`）** → 设备恢复 `NORMAL`，从 `spbin` 中提取该设备的积压路径重新入队。
  - **被信号杀死（`WIFSIGNALED`）** → `alarm` 超时触发，设备仍死。重新推入 `ProbeScheduler`，间隔翻倍。

### 3.7 ProbeScheduler（渐进探测调度器）

- **数据结构**：基于小根堆（数组实现），按 `next_probe_time` 排序。
- **退避策略**：初始间隔 5s，每次失败后翻倍，上限 300s。
- **CONDEMNED 状态**：当退避达到上限后，设备被标记为永久跳过，不再探测。

### 3.8 AsyncWorker（输出线程）

- **职责**：将主循环提交的文件信息格式化并写入输出文件。
- **线程安全**：使用 `pthread_mutex_t` + `pthread_cond_t` 实现任务队列。
- **缓冲策略**：`setvbuf(..., _IOFBF, 8*1024*1024)`，8MB 全缓冲，减少系统调用次数。

### 3.9 Progress（进度与归档）

#### 3.9.1 pbin 格式（已处理记录）

```
[path_len: size_t][path: bytes][dev: dev_t][ino: ino_t][mtime: time_t][d_type: uchar]
```

#### 3.9.2 spbin 格式（跳过记录）

```
[path_len: uint32_t][dev: uint64_t][blacklist_time: time_t]
[retry_count: uint32_t][probe_interval: uint32_t][d_type: uint8_t][s_status: uint8_t]
[path: bytes]
```

`s_status`：`0 = PROBING`（还在探测），`1 = CONDEMNED`（永久跳过）。

#### 3.9.3 归档格式（archive）

```
[ArchiveBlockHeader: 9 bytes]
  uncompressed_size : uint32_t
  compressed_size   : uint32_t
  block_type        : uint8_t  (0 = normal pbin, 1 = spbin)
[zlib compressed payload]
```

**约束**：`spbin` 块必须是归档流中的**最后一个块**，以便恢复时顺序解析。

#### 3.9.4 恢复流程

1. 读取 `.idx` 文件获取当前游标。
2. 遍历 `.archive` 中的每个块：
   - `block_type = 0`（normal）：解压 → 解析 pbin → 填充 `visited_set` / `reference_set` / `reference_map`。
   - `block_type = 1`（spbin）：解压 → 解析 spbin → 填充 `spbin_entries`，并根据 `s_status` 恢复设备状态和探测任务。
3. 解析散落的 `.pbin` 分片（当前未归档的部分）。

## 4. 关键设计决策

### 4.1 为什么不用线程而改用进程？

| 维度 | 线程模型 | 进程模型 |
|------|---------|---------|
| D-State 可杀性 | ❌ `pthread_kill` 无效 | ✅ `SIGKILL` 可终止 |
| 上下文共享 | 共享地址空间，需锁 | COW 只读，无锁 |
| 消息传递 | 共享内存队列 | pipe TLV，内核缓冲 |
| 故障隔离 | 一个线程崩溃影响全局 | Worker 崩溃仅影响单个 slot |

### 4.2 为什么用开放寻址而非链表法？

- **CPU 缓存友好**：开放寻址的条目连续存储，探测时缓存命中率高。
- **无指针**：每个条目固定大小，节省内存管理开销。
- **简单可预测**：扩容时只需 `realloc` + 重新哈希，无链表遍历。

### 4.3 为什么不用 Bloom Filter？

Bloom Filter 存在假阳性，对于“已访问过”这类**必须精确**的判定场景不适用。本项目采用精确 HashSet（128-bit MD5，误判率可忽略）。

### 4.4 为什么不用数据库或外部 KV？

- **无外部依赖**：不引入 SQLite、LevelDB 等第三方库，保持单二进制文件。
- **内存优先**：对于数亿级文件，FingerprintSet（16 字节/条目）+ ReferenceMap（~32 字节/条目）在 64GB 内存服务器上完全可行。
- **mmap fallback 预留**：若内存不足，未来可扩展为 `mmap` 到 `/tmp` 的磁盘映射。

### 4.5 为什么 IPC 用 pipe 而非 shared memory？

- **简单可靠**：`pipe` 是字节流，天然支持 `read/write` 原子性（小于 `PIPE_BUF` 的消息）。
- **无需同步原语**：不需要额外的 mutex/condition variable 来保护共享内存。
- **可调试**：可通过 `strace -e read,write` 直接观察 IPC 流量。

### 4.6 blind-trust 的条件

Worker 执行 `try_blind_trust` 时必须**同时满足**：

1. `reference_set` 和 `reference_map` 不为 NULL（半增量模式启用）。
2. `dirent` 提供有效的 `d_ino` 和 `d_type`（DT_UNKNOWN 跳过）。
3. fingerprint 命中 `reference_set`。
4. `reference_map` 中对应的 `d_type` 与当前 `dirent.d_type` 一致。
5. `now - mtime > skip_interval`（超过用户指定的跳过阈值）。

## 5. 数据流图

### 5.1 正常扫描流程

```
Master: epoll_wait → recv BATCH → fp_compute → visited_set.insert?
  ├─ 已存在 → 丢弃
  └─ 新文件
       ├─ S_ISDIR → pending_tasks++ → ipc_send(MSG_SCAN) → record_path(pbin)
       └─ 其他    → async_writer_submit → record_path(pbin)
```

### 5.2 设备熔断流程

```
Worker: readdir/lstat → ETIMEDOUT
  → ipc_send(MSG_ERROR, errno=ETIMEDOUT, dev=X)

Master: recv ERROR → dev_mgr_mark_dead(X)
  → spbin_append(path=X, dev=X, status=PROBING)
  → probe_scheduler_push(dev=X, interval=5s)
```

### 5.3 设备恢复流程

```
Master: epoll_timeout → probe_scheduler_peek → due?
  → fork() daredevil → lstat(probe_path) → alarm timeout / return

Master: waitpid → WIFEXITED?
  ├─ YES → dev_mgr_mark_alive(X)
  │        → spbin_requeue_recovered(X)  /* 重新扫描积压目录 */
  └─ NO  → probe_scheduler_push(interval *= 2)
```

## 6. 边界情况处理

### 6.1 Worker 在 send_batch 时卡死

- Master 通过心跳超时检测（30s 无 `IPC_MSG_HEARTBEAT`）。
- `kill(SIGKILL)` 后，pipe 的 `fd_out` 会收到 EOF，`epoll` 触发 `read` 返回 0，Master 标记 Worker 死亡并替换。

### 6.2 敢死队进程也卡死

- `alarm(PROBE_TIMEOUT_SEC)` 确保子进程最多运行 5 秒。
- 若 `alarm` 也失效（极端内核 bug），父进程的心跳超时最终会触发新一轮敢死队替换。

### 6.3 同一个设备多次报错

- `dev_mgr_mark_dead` 幂等：若设备已经是 `DEAD` 或 `CONDEMNED`，不会重复创建 `spbin` 条目和探测任务。

### 6.4 归档文件损坏

- `iterate_archive` 在读取每个 `ArchiveBlockHeader` 和压缩 payload 时都做长度校验，损坏的块会被静默跳过，不影响后续块解析。

## 7. 遗留与未来工作

### 7.1 已移除的旧模块

以下旧线程模型文件已备份为 `.bak`，不再参与编译：

- `traversal.c`（旧 Worker 线程逻辑）
- `looper.c`（旧消息队列与批处理）
- `monitor.c`（旧 Monitor 线程）
- `idempotency.c`（旧去重逻辑，已被 FingerprintSet 替代）

### 7.2 未来可扩展点

| 扩展点 | 说明 |
|--------|------|
| `mmap` fallback | 当 `FingerprintSet` 内存不足时，自动 `mmap` 到 `/tmp` 临时文件 |
| 跨架构 IPC | 当前 `struct stat` 直接 `memcpy`，未来可扩展为显式字段序列化 |
| 多机并行 | Worker 可扩展为通过 TCP 连接到远程 Agent，实现集群扫描 |
| 实时统计 HTTP API | 在 Master 中嵌入轻量 HTTP 服务，暴露 `/stats` 端点 |

## 8. 参考

- Linux `fork()` COW 机制：`man 2 fork`
- `epoll` 边缘触发 vs 水平触发：`man 7 epoll`
- NFS `soft` vs `hard` 挂载：`man 5 nfs`
- RFC 1321: The MD5 Message-Digest Algorithm
