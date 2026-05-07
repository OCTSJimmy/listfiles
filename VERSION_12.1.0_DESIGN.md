# VERSION-12.1.0 设计蓝图

> 本文件记录 listfiles V12 进程模型与 fpbin 恢复机制的完整设计决策。基于 V12.0.0 进程模型重构与 V12.1.0 fpbin 临时缓存修复。

---

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
- **启动时预 fork（Prefork）安全**：`fork()` 只发生在启动阶段单线程上下文中，运行时 Master 可自由使用多线程（AsyncWriter、epoll），彻底消除 fork-unsafe 风险。
- **容量自动降级**：`FingerprintSet` / `ReferenceMap` 在内存不足时自动降级为 `mmap` 磁盘映射，避免因 `--estimated-files` 预估不准导致 OOM。
- **统一状态管理**：所有模块状态收敛到单一 `AppContext` 结构体中。

---

## 2. 架构总览

### 2.1 进程模型图

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
│  │  ┌─────────┐  ┌─────────┐  ┌─────────────────────────┐  │  │
│  │  │ipc_recv │  │scan_dir │  │    try_blind_trust      │  │  │
│  │  │MSG_SCAN │  │readdir  │  │  (ref_set + ref_map)    │  │  │
│  │  └─────────┘  └─────────┘  └─────────────────────────┘  │  │
│  └─────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 主循环时序图

```
Master:         Worker 0:         Worker N:        Probe (fork):
  │                │                 │                  │
  │──fork()───────►│                 │                  │
  │──epoll_ctl ADD►│                 │                  │
  │                │                 │                  │
  │──IPC_MSG_SCAN─►│                 │                  │
  │                │──readdir()─────►│                  │
  │                │──lstat()────────►│                  │
  │                │                 │                  │
  │◄─IPC_MSG_BATCH─│                 │                  │
  │  (parse batch) │                 │                  │
  │  (dedup, new dirs -> pending++) │                  │
  │──IPC_MSG_SCAN─►│ (new subdirs)   │                  │
  │                │                 │                  │
  │◄─HEARTBEAT─────│                 │                  │
  │                │                 │                  │
  │◄─MSG_ERROR─────│ (ETIMEDOUT)     │                  │
  │  mark_dead()   │                 │                  │
  │  spbin_append()│                 │                  │
  │  probe_sched_push()               │                  │
  │                │                 │                  │
  │──epoll_wait(500ms timeout)────────►│                 │
  │  monitor_check_timeouts()         │                  │
  │  monitor_dispatch_probes()        │                  │
  │  monitor_reap_probes()            │                  │
  │                │                 │                  │
  │                │                 │                  │◄─fork()
  │                │                 │                  │──lstat()
  │                │                 │                  │──_exit()
  │                │                 │                  │
  │◄─waitpid───────│─────────────────│─────────────────►│
  │  mark_alive()  │                 │                  │
  │  spbin_requeue()│                 │                  │
```

### 2.3 生命周期流程图

```
【单线程阶段】fork 安全窗口
    │
    ▼
┌─────────┐    ┌─────────────┐    ┌─────────────────┐    ┌──────────┐
│  main() │───►│app_context_ │───►│ parse_arguments │───►│ load_    │
│         │    │  init()     │    │                 │    │ session  │
└─────────┘    └─────────────┘    └─────────────────┘    │ _config  │
                                                         └────┬─────┘
                                                              │
    ┌─────────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────────────┐
│ 【安全窗口】启动时单线程，无其他线程存在                      │
│  ├─► init_output_files()                                    │
│  ├─► fp_set_create(estimated_files)  [预分配，内存或mmap]    │
│  ├─► ref_map_create(estimated_files) [预分配，内存或mmap]    │
│  ├─► dev_mgr_create()                                       │
│  └─► worker_pool_spawn(N_active + N_spare)                  │
│       一次性 fork 所有活跃Worker + 备用池进程                 │
│       COW 快照在此刻冻结，子进程看到稳定的数据                │
└─────────────────────────────────────────────────────────────┘
    │
    ▼
【多线程阶段】运行时不再 fork
    │
    ▼
┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
│ Async_   │───►│ restore_ │───►│ seed root│───►│main_loop │
│ Writer   │    │ progress │    │  task    │    │  _run()  │
│  init()  │    │ (若启用) │    │          │    │          │
└──────────┘    └──────────┘    └──────────┘    └────┬─────┘
                                                     │
    ┌────────────────────────────────────────────────┘
    │
    ▼
┌──────────┐    ┌──────────┐    ┌──────────┐
│finalize_ │───►│app_context│───►│  exit    │
│progress()│    │_destroy() │    │          │
└──────────┘    └──────────┘    └──────────┘
```

---

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

#### 3.3.1 预 fork + 双池结构

```
WorkerPool {
    active[N]   // 活跃 Worker，参与扫描，fd_out 注册在 epoll 中
    spare[K]    // 备用 Worker，阻塞等待任务，fd_out 不注册在 epoll 中
}

N = num_cores * 2     // 并发工作数
K = N                 // 备用数（A-1 方案：备用 = 活跃）
```

**启动阶段（单线程安全窗口）**：

```c
for (i = 0; i < N + K; i++) {
    pipe2(in_pipe, O_CLOEXEC);
    pipe2(out_pipe, O_CLOEXEC);
    pid = fork();
    if (pid == 0) {
        // Worker 子进程
        close_all_fds_except_pipes();
        worker_main(fd_in, fd_out, id);
        _exit(0);
    }
    // 父进程登记 slot
    if (i < N) active[i] = {pid, fd_in, fd_out};
    else       spare[i-N] = {pid, fd_in, fd_out};
}
```

**运行时替换（不再 fork）**：

```
Monitor 检测到 Worker[i] 心跳超时
    │
    ├──► kill(active[i].pid, SIGKILL)
    ├──► waitpid(WNOHANG) 收割僵尸
    ├──► epoll_ctl(DEL, active[i].fd_out)
    ├──► close(active[i].fd_in), close(active[i].fd_out)
    │
    ├──► 从 spare[] 取最后一个备用 Worker[j]
    │      active[i].pid  = spare[j].pid    // 移交
    │      active[i].fd_in  = spare[j].fd_in
    │      active[i].fd_out = spare[j].fd_out
    │      active[i].is_alive = true
    │      spare_count--
    │
    ├──► epoll_ctl(ADD, active[i].fd_out)
    └──► ipc_send(active[i].fd_in, IPC_MSG_SCAN, first_pending_dir)
```

**备用池耗尽（优雅降级）**：

```
spare_count == 0 && active Worker 继续死亡
    │
    ├──► 活跃数减少，吞吐量下降
    ├──► 若活跃数降至 0 且 pending_tasks > 0：
    │      扫描停滞，但程序不崩溃
    │      敢死队探测继续运行
    │      输出降级日志，提示用户重启恢复
    │      finalize_progress() 保存已完成的进度
    └──► 主循环优雅退出
```

#### 3.3.2 COW 安全策略

- **启动时 `fork()`**：Master 只有主线程，所有锁、fd 状态完全可控。
- **运行时绝不再 `fork()`**：AsyncWriter、epoll、Monitor 巡检自由运行。
- **Worker 只读承诺**：子进程仅调用 `fp_set_contains()`、`ref_map_lookup()`、`try_blind_trust()`，绝不写入共享数据结构。
- **FingerprintSet 扩容**：Master 运行时可能触发 `realloc()` 扩容。Worker 进程通过 COW 页表仍映射到旧的物理页，Linux 不会因父进程 `free()` 而回收子进程仍引用的页。但为了严格安全，引入 **mmap 后备（B-2）** 机制（见 3.10）。
- **信号隔离**：Worker `main()` 开头重置 `SIGINT`/`SIGTERM`/`SIGQUIT` 为 `SIG_DFL`，避免继承 Master 的锁清理处理器。

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
| `IPC_MSG_STOP` (6) | M → W | 请求 Worker 停止（优雅退出）|

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

- 遍历所有 WorkerSlot，若 `now - last_heartbeat > heartbeat_timeout`（默认 30s），则 `kill(SIGKILL)`。
- `waitpid(WNOHANG)` 收割僵尸。
- `slot->is_alive = false`，`slot->pid = -1`（标记为"待替换"，由主循环统一执行 epoll_del → close → replace）。
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

### 3.10 MmapArena（内存降级组件）

为解除 `--estimated-files` 的固定容量限制，引入 `MmapArena` 抽象层，供 `FingerprintSet` 与 `ReferenceMap` 复用。

#### 3.10.1 结构

```c
typedef struct {
    void   *base;           // 映射基地址
    size_t  capacity;       // 当前容量（条目数）
    size_t  elem_size;      // 单条目大小
    bool    is_mmap;        // 是否使用 mmap
    int     mmap_fd;        // memfd_create 返回的 fd（或 -1）
} MmapArena;
```

#### 3.10.2 分配策略

| 场景 | 行为 | 性能 |
|------|------|------|
| 内存充足 | `calloc()` 分配纯内存 | 与现在相同 |
| 容量超过 `MEMORY_THRESHOLD` | 首次降级：`memfd_create()` + `mmap(MAP_SHARED)` | 热点页在内存 page cache，与内存相近 |
| 已在 mmap 且需扩容 | `ftruncate()` 扩展文件 + `mremap()` 扩展映射 | 缺页时磁盘 I/O，但不会 OOM |

#### 3.10.3 对 COW 的特殊优势

`malloc` + `free` 方案中，Worker 通过 COW 访问的 `FingerprintSet` 旧页可能在 Master `realloc()` + `free()` 后被回收（依赖内核实现细节）。

`mmap(MAP_SHARED)` 方案中：
- 页面由匿名文件（`memfd_create`）后备，不受 `free()` 影响。
- Master 写入触发 COW，获得私有副本；Worker 的页表仍指向旧的共享页。
- Worker 读取安全且确定，不依赖 Linux 内存回收的未定义行为。

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

#### 3.9.4 游标索引（idx）

格式（文本，5 个 `unsigned long`）：
```
write_slice_index line_count processed_count output_slice_num output_line_count
```

原子更新策略：先写入 `.idx.tmp.<tid>`，再 `rename()` 覆盖目标文件。

---

## 4. 恢复流程与 fpbin 机制

### 4.1 问题背景

在 `--continue` 断点续传恢复流程中，Master 一边从历史 `pbin` 分片读取未完成的目录泵送给 Worker，一边又将 Worker 返回的新发现子目录直接写入当前 `pbin` 分片。这导致：

1. **pbin 读写冲突**：同一个分片文件被并发读取（恢复泵送）和写入（记录新目录）。
2. **管道死锁风险**：恢复期间新子目录直接抢占 Worker 队列，与历史目录争夺有限的 pipe 缓冲区。
3. **游标污染**：新目录混入正在消费的历史分片，崩溃后无法区分"已处理的历史记录"和"新发现的目录"。

### 4.2 解决方案：fpbin（Future-Pbin）临时缓存

引入 **fpbin 临时缓存**，作为恢复期间新子目录的"隔离缓冲区"。

#### 设计原理

- **阶段一（HIST_PUMP_OLD）**：Master 从历史 pbin 分片逐批泵送目录给 Worker。Worker 返回的批次中，**所有新发现的子目录不再直接入队**，而是进入 fpbin 临时缓存。
- **阶段二（fpbin 转正）**：当最后一个历史 pbin 分片消费完毕后，fpbin 缓存被"晋升"为一个全新的 pbin 分片（延续当前索引编号）。此时 fpbin 机制**永久关闭**。
- **阶段三（HIST_PUMP_NEW / DONE）**：fpbin 转正后的新分片继续被泵送消费；消费完毕后，`hist_pump_state` 切换为 `HIST_PUMP_DONE`，后续扫描完全恢复为正常的直接入队 BFS。
- **崩溃安全**：如果在恢复期间进程崩溃，下次启动时 `restore_progress()` 会无条件丢弃残留的 `progress.fpbin`，重新从原始历史 pbin 开始恢复，避免脏数据污染。

#### fpbin 缓存结构

| 层级 | 容量 | 行为 |
|---|---|---|
| 内存数组 | 约 10,000 条（`FPBIN_MEM_WATERMARK`） | 动态扩容的 `char**` 路径数组 + `struct stat*` 元数据数组 |
| 磁盘溢出 | 无上限 | 内存满后，先刷写内存数组到 `progress.fpbin`，再追加新记录 |

内存数组使用 `realloc` 动态扩容（初始 1024，倍增策略）。磁盘溢出文件使用 `O_APPEND` 原子追加。

### 4.3 恢复流程时序图

```
restore_progress()
    │
    ├──► unlink("progress.fpbin")      /* 丢弃上一次崩溃的残留 */
    │
    ├──► load .idx → write_slice_index, line_count
    │
    ├──► iterate_archive()             /* 加载已归档的历史记录到 visited_set */
    │
    ├──► iterate_pbin_slices()         /* 加载已完成的分片到 visited_set */
    │      对于当前分片 (write_slice_index):
    │      仅加载 line_no < line_count 的记录
    │      line_no >= line_count 的记录留给 pump 消费
    │
    ├──► fopen(current_slice, "rb")
    │      fseek 跳过已处理的 line_count 行
    │      hist_pump_state = HIST_PUMP_OLD
    │
    └──► 返回，主循环开始 pump_pbin_batch()

main_loop_run() — 每次 epoll_wait timeout:
    │
    ├──► pump_pbin_batch(batch_size)
    │      从 hist_pump_fp 读取目录
    │      插入 visited_set（防止后续重复输出）
    │      IPC_MSG_SCAN → Worker
    │      读完当前分片 → on_pbin_slice_consumed()
    │           打开下一个散落分片，或
    │           promote_fpbin_to_pbin() 将 fpbin → 新 pbin 分片
    │
    ├──► main_loop_handle_batch()
    │      若 hist_pump_state == HIST_PUMP_OLD:
    │          新子目录 → fpbin_append()   /* 隔离 */
    │          不直接 IPC 入队
    │          跳过 record_path()          /* 不写入当前 pbin */
    │      若 hist_pump_state != HIST_PUMP_OLD:
    │          新子目录 → 正常 IPC 入队 + record_path()
    │
    └──► 当所有历史分片 + fpbin 分片消费完毕:
              hist_pump_state = HIST_PUMP_DONE
              后续扫描恢复正常 BFS
```

---

## 5. 设备熔断与恢复流程

### 5.1 设备状态机

```
          ┌─────────────┐
          │   NORMAL    │
          └──────┬──────┘
                 │ Worker reports ETIMEDOUT / EIO
                 ▼
          ┌─────────────┐
          │    DEAD     │◄─────────────────────────┐
          └──────┬──────┘                          │
                 │ register ProbeTask              │
                 │ (interval=5s)                   │
                 ▼                                 │
          ┌─────────────┐     probe success       │
          │  PROBING    │─────────────────────────┘
          └──────┬──────┘
                 │ probe fails, interval *= 2
                 │ (max 300s)
                 ▼
          ┌─────────────┐
          │ CONDEMNED   │
          └─────────────┘
                 │
                 └── 永久跳过，不再探测
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

---

## 6. 数据流图

### 6.1 正常扫描流程

```
Master: epoll_wait → recv BATCH → fp_compute → visited_set.insert?
  ├─ 已存在 → 丢弃
  └─ 新文件
       ├─ S_ISDIR → pending_tasks++ → ipc_send(MSG_SCAN) → record_path(pbin)
       └─ 其他    → async_writer_submit → record_path(pbin)
```

### 6.2 恢复期间扫描流程（HIST_PUMP_OLD）

```
Master: pump_pbin_batch → IPC_MSG_SCAN → Worker
  Worker: scan_dir → readdir + lstat
    → ipc_send(BATCH)

Master: recv BATCH → fp_compute → visited_set.insert?
  ├─ 已存在 → 丢弃
  └─ 新目录
       ├─ hist_pump_state == HIST_PUMP_OLD
       │      → fpbin_append()          /* 隔离到 fpbin */
       │      → 不直接 IPC 入队
       │      → 跳过 record_path()
       └─ hist_pump_state != HIST_PUMP_OLD
              → pending_tasks++ → ipc_send(MSG_SCAN) → record_path(pbin)
```

---

## 7. 边界情况处理

### 7.1 Worker 在 send_batch 时卡死

- Master 通过心跳超时检测（30s 无 `IPC_MSG_HEARTBEAT`）。
- `kill(SIGKILL)` 后，`waitpid(WNOHANG)` 收割僵尸，`slot->pid = -1` 标记待替换。
- 主循环替换段执行 `epoll_ctl(DEL)` → `close(fd_in/fd_out)` → `worker_pool_replace()` → `epoll_ctl(ADD)`。

### 7.2 敢死队进程也卡死

- `alarm(PROBE_TIMEOUT_SEC)` 确保子进程最多运行 5 秒。
- 若 `alarm` 也失效（极端内核 bug），父进程下一轮 Monitor 巡检会正常继续。

### 7.3 同一个设备多次报错

- `dev_mgr_mark_dead` 幂等：若设备已经是 `DEAD` 或 `CONDEMNED`，不会重复创建 `spbin` 条目和探测任务。

### 7.4 归档文件损坏

- `iterate_archive` 在读取每个 `ArchiveBlockHeader` 和压缩 payload 时都做长度校验，损坏的块会被静默跳过，不影响后续块解析。

### 7.5 空目录与权限不足目录

- **空目录**：Worker `readdir` 只读到 `.` 和 `..`，`count == 0`。`scan_and_send()` 在 `else` 分支发送 empty batch（`send_batch(fd_out, NULL, NULL, 0)`），Master 的 `pending_tasks` 正确递减。
- **权限不足（`EACCES`）**：`send_error_and_empty_batch()` 始终发送 empty batch（无论错误码是否为 `ETIMEDOUT`/`EIO`），确保 `pending_tasks` 正确递减，程序不会死锁。

### 7.6 备用池耗尽

当故障设备（如 NFS）导致 Worker 陆续卡死，备用池可能被耗尽：

```
活跃 Worker: 16 → 8 → 4 → 1 → 0
备用 Worker: 16 → 12 → 8 → 4 → 0
```

**系统行为**：
1. 吞吐量逐步下降，但已完成的进度持续落盘（`.pbin` + `.idx`）。
2. 当 `active_count == 0 && spare_count == 0 && pending_tasks > 0` 时，主循环进入**优雅停滞**：
   - 不再尝试替换 Worker。
   - 敢死队探测继续运行（`monitor_dispatch_probes` + `monitor_reap_probes`）。
   - 向 stderr 输出降级诊断日志：
     ```
     [System] 所有 Worker 与备用池均已耗尽，扫描停滞。
     [System] 故障设备: dev=XX, 积压任务: YY 个。
     [System] 建议：检查存储设备后重启任务，将从上次进度自动恢复。
     ```
   - 保持 `epoll_wait` 循环响应 `SIGTERM`。
   - 收到终止信号后，`finalize_progress()` 保存当前进度，优雅退出。
3. 用户重启后，`restore_progress()` 从 `.idx` + `.pbin` 恢复，继续未完成的任务。

### 7.6 IPC payload 超限

- `send_batch()` 中增加 `total > UINT32_MAX` 上限检查。若单个目录下文件极多导致序列化后 batch 超过 4GB，则丢弃该 batch 并输出错误日志，防止 `payload_len` 静默截断导致 Master 解析损坏数据。

---

## 8. 设计约束

- **同机限制**：IPC 协议中 `struct stat` 直接通过 `memcpy` 序列化，协议**不跨机器/不跨架构**。
- **NFS 挂载前提**：扫描 NFS 目录时，必须采用 `soft,intr,timeo=600` 挂载选项。`hard` 挂载下 D-State 仍不可杀。
- **spbin 归档位置**：`spbin` 块必须是 `.archive` 文件中的**最后一个块**，恢复逻辑依赖此顺序。
- **信号安全**：`SIGSEGV`/`SIGABRT` 的处理器仅执行最小操作（`write` + `raise`），不做任何可能触发页错误的复杂逻辑。

---

## 9. 遗留与未来工作

### 9.1 已移除的旧模块

以下旧线程模型文件已备份为 `.bak`，不再参与编译：

- `traversal.c`（旧 Worker 线程逻辑）
- `looper.c`（旧消息队列与批处理）
- `monitor.c`（旧 Monitor 线程）
- `idempotency.c`（旧去重逻辑，已被 FingerprintSet 替代）

### 9.2 未来可扩展点

| 扩展点 | 说明 | 状态 |
|--------|------|------|
| `mmap` fallback | 当 `FingerprintSet` / `ReferenceMap` 内存不足时，自动降级为 `mmap` 磁盘映射 | ✅ V12.1.0 设计已定，待实现 |
| 跨架构 IPC | 当前 `struct stat` 直接 `memcpy`，未来可扩展为显式字段序列化 | 预留 |
| 多机并行 | Worker 可扩展为通过 TCP 连接到远程 Agent，实现集群扫描 | 预留 |
| 实时统计 HTTP API | 在 Master 中嵌入轻量 HTTP 服务，暴露 `/stats` 端点 | 预留 |
| `close_range()` | Linux 5.9+ 使用 `close_range()` 替代遍历 fd 表关闭，提升 fork 性能 | 预留 |

### 9.3 参考

- Linux `fork()` COW 机制：`man 2 fork`
- `epoll` 边缘触发 vs 水平触发：`man 7 epoll`
- NFS `soft` vs `hard` 挂载：`man 5 nfs`
- RFC 1321: The MD5 Message-Digest Algorithm
