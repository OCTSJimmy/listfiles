# Changelog

所有显著变更均记录于此文件，格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

---

## [12.1.1] - 2026-05-06

### 高优先级问题修复 (High-Priority Bug Fixes)

#### 修复

- **单文件目标被错误拒绝 (BUG-009)**：`parse_arguments()` 中的目录校验放宽为允许普通文件与符号链接，`main.c` 中的单文件处理逻辑得以生效。
- **系统消息污染 stdout (BUG-013)**：所有 `[System]` 诊断信息统一改为 `fprintf(stderr, ...)`，避免破坏管道下游处理。
- **`--archive` / `--clean` 选项语义失效 (BUG-006)**：归档与清理逻辑现在真正受命令行标志控制。
  - `process_old_slice()` / `finalize_archive()`：仅在 `--archive` 时执行 zlib 压缩归档；否则保留当前分片供恢复使用。
  - `finalize_progress()`：在 `--clean` 模式下不再创建或追加 `.config` / `.idx`。
  - `save_config_to_disk()`：在 `--clean` 模式下跳过写入。
  - `cleanup_progress()`：`--clean` 组合使用时彻底清理所有残留进度文件。
- **独立元数据开关无效 (BUG-007)**：`precompile_format()` 现在根据 `--size`、`--user`、`--group`、`--mtime`、`--atime`、`--mode`、`--inode`、`--xattr` 等开关动态拼装默认文本格式；未指定任何开关时回退到 `%p|%s|%m`。
- **致命信号处理函数不安全 (BUG-014)**：`SIGSEGV` / `SIGABRT` 的处理器改为仅执行 `_exit` 等价的最小操作（避免栈损坏后执行复杂逻辑）；`SIGINT` / `SIGTERM` / `SIGQUIT` 保留有限的锁文件清理。
- **Monitor 与主循环竞态条件 (BUG-016)**：
  - `monitor_check_timeouts()` 中 Worker 被 `SIGKILL` 后，将 `slot->pid` 设为 `-1` 作为"待替换"标记，避免主循环与 Monitor 对同一 fd 执行 double-close。
  - 主循环 "Replace dead workers" 段统一检查 `pid == -1`，确保 `epoll_ctl(DEL)` → `close(fd_in/fd_out)` → `worker_pool_replace()` 的原子性。
  - `spbin_requeue_recovered()` 中的 `static int next_worker` 改为 `ctx->next_requeue_worker`，消除未保护静态变量的封装破坏与未来线程安全风险。
- **IPC payload 静默截断风险 (BUG-017)**：`send_batch()` 中增加 `total > UINT32_MAX` 上限检查，超限则丢弃 batch 并输出错误日志，防止 `payload_len` 静默截断导致 Master 解析损坏数据。
- **`.idx` 游标未记录 `line_count` (BUG-005 残留)**：`atomic_update_index()` 将 `.idx` 格式从冗余的 `write_slice_index processed_count write_slice_index ...` 修正为 `write_slice_index line_count processed_count output_slice_num output_line_count`；`load_progress_index()` 同步读取，使断点续传能正确识别当前分片内已处理的行数，避免重复输出。

#### 文档与工程

- **README `--format` 语法描述错误 (BUG-011)**：将 `{path}\t{size}...` 花括号示例修正为实际支持的 `%p\t%s...` 百分号语法。
- **版本号不一致 (BUG-012)**：`include/config.h` 与 `README.md` 中的版本号统一更新为 `12.1.0`。
- **`--resume-from` 选项未实现 (BUG-008)**：帮助信息中标注为 "(预留，暂未实现)"。
- **编译警告清零**：修正 `show_help()` 中 `%lu` → `%u` 格式不匹配、`record_skip()` 未使用参数、`read_next_pbin_record()` 未使用局部变量。

#### 修改的文件

- `src/cmdline.c`
- `src/main.c`
- `src/main_loop.c`
- `src/output.c`
- `src/progress.c`
- `src/signals.c`
- `src/worker_proc.c`
- `include/app_context.h`
- `include/config.h`
- `README.md`

---

## [12.1.0] - 2026-05-06

### 架构修复：fpbin（Future-Pbin）临时缓存机制

**问题背景**：在 `--continue` 断点续传恢复流程中，Master 一边从历史 `pbin` 分片读取未完成的目录泵送给 Worker，一边又将 Worker 返回的新发现子目录直接写入当前 `pbin` 分片。这导致：

1. **pbin 读写冲突**：同一个分片文件被并发读取（恢复泵送）和写入（记录新目录）。
2. **管道死锁风险**：恢复期间新子目录直接抢占 Worker 队列，与历史目录争夺有限的 pipe 缓冲区，可能形成循环依赖。
3. **游标污染**：新目录混入正在消费的历史分片，崩溃后无法区分"已处理的历史记录"和"新发现的目录"。

**解决方案**：引入 **fpbin（future-pbin）临时缓存**，作为恢复期间新子目录的"隔离缓冲区"。

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

内存数组使用 `realloc` 动态扩容（初始 1024，倍增策略）。磁盘溢出文件使用 `O_APPEND` 原子追加，避免并发写入问题。

#### 修改的文件

- **`include/app_context.h`**
  - 新增 `HistPumpState` 枚举：`HIST_PUMP_DONE` / `HIST_PUMP_OLD` / `HIST_PUMP_NEW`
  - 新增历史泵送状态字段：`hist_pump_fp`、`hist_pump_slice_idx`、`hist_pump_line_no`
  - 新增 fpbin 缓存字段：`fpbin_fd`（磁盘文件描述符）、`fpbin_entries`（内存路径数组）、`fpbin_stats`（对应 `struct stat` 数组）、`fpbin_count` / `fpbin_capacity`

- **`src/progress.c`**
  - `fpbin_append()`：将新子目录追加到 fpbin。内存未满时直接入数组；内存满时刷写到 `progress.fpbin` 磁盘文件。
  - `fpbin_clear_mem()`：释放内存数组中的路径字符串并重置计数器。
  - `promote_fpbin_to_pbin()`：将 fpbin 内存数组 + 磁盘文件合并写入一个新的 pbin 分片，更新泵送源，关闭 fpbin 机制。
  - `on_pbin_slice_consumed()`：当当前 pbin 分片消费完毕时，自动打开下一个散落分片；若无更多分片且 fpbin 非空，则触发 `promote_fpbin_to_pbin()`。
  - `pump_pbin_batch()`：从当前历史 pbin 文件批量读取目录记录，加载到 `visited_set` 后分发给空闲 Worker。
  - `read_next_pbin_record()`：从 pbin 二进制流解析单条记录（路径 + `dev` / `ino` / `mtime` / `d_type`）。
  - `restore_progress()`：启动时无条件 `unlink("progress.fpbin")` 并清空内存，丢弃上一次崩溃的残留缓存。
  - `cleanup_progress()`：清理阶段删除 `progress.fpbin`，防止残留。

- **`src/main_loop.c`**
  - `main_loop_handle_batch()`：当 `hist_pump_state == HIST_PUMP_OLD` 时，Worker 返回的子目录（`S_ISDIR`）调用 `fpbin_append()` 入缓存，**不直接 IPC 入队**；同时跳过这些目录的 `record_path()`，避免重复写入当前 pbin。
  - `main_loop_run()` 的 `epoll_wait` 超时路径中，增加 `pump_pbin_batch()` 调用，确保历史目录持续泵送。

- **`src/main.c`**
  - `app_context_init()`：初始化 `fpbin_fd = -1`、`hist_pump_state = HIST_PUMP_DONE`。
  - `app_context_destroy()`：释放 `fpbin_entries` / `fpbin_stats` 数组，关闭 `fpbin_fd`。

- **`include/progress.h`**
  - 新增 `pump_pbin_batch()` 和 `fpbin_append()` 的函数声明。

#### 修复

- **pbin 读写冲突**：恢复期间新子目录不再写入正在消费的历史分片，彻底消除读写竞争。
- **管道死锁风险**：恢复期间 Worker 队列深度完全由 `pump_pbin_batch` 控制，新子目录被隔离在 fpbin 中，不会与历史目录争夺 pipe 缓冲区。
- **游标丢失后的安全恢复**：`cleanup_progress()` 现在会清理 `progress.fpbin`，`restore_progress()` 启动时也会主动丢弃残留 fpbin，避免脏缓存导致重复扫描或遗漏。

---

## [12.0.0] - 2026-04-29

### 重大重构：线程模型 → 进程模型

为解决 Worker 线程在 D-State 不可杀死的问题，整个运行时从多线程共享内存架构重构为 **Master-Worker 多进程模型**（`fork()` + `pipe` + `epoll`）。

#### 新增

- **进程管理模块**
  - `src/worker_proc.c` / `include/worker_proc.h`：基于 `fork()` + `pipe2(O_CLOEXEC)` 的 Worker 进程池，支持动态 spawn / replace / stop。
  - `src/probe_scheduler.c` / `include/probe_scheduler.h`：基于小根堆的渐进探测调度器，指数退避策略（5s → 10s → 20s → ... → 300s）。
- **数据结构与去重**
  - `src/fingerprint_set.c` / `include/fingerprint_set.h`：128-bit MD5 开放寻址哈希集合，内嵌 RFC 1321 MD5 实现。纯指纹存储，16 字节/条目。
  - `src/reference_map.c` / `include/reference_map.h`：与 HashSet 同构的 `fingerprint → (mtime, d_type)` 映射，支撑半增量 blind-trust。
- **IPC 协议**
  - `include/ipc_protocol.h`：Master ↔ Worker 的 TLV 消息协议，支持 `SCAN` / `BATCH` / `HEARTBEAT` / `ERROR` / `EXIT` / `STOP` 六种消息类型。
- **核心运行时**
  - `src/main_loop.c` / `include/main_loop.h`：基于 `epoll` 的单线程事件循环，集成消息分发、心跳监控、敢死队探测发射/收割。
  - `include/app_context.h`：统一上下文结构体 `AppContext`，取代所有旧版全局变量。
  - `src/async_worker.c` / `include/async_worker.h`：简化版独立输出线程，线程安全任务队列。
- **进度与归档格式**
  - `include/spbin.h`：跳过记录格式（设备熔断目录），支持 `PROBING` 和 `CONDEMNED` 两种状态。
  - `include/archive_format.h`：归档块头，含 `block_type` 字段（`0 = normal pbin`，`1 = spbin`），spbin 块固定位于归档流末尾。
- **命令行参数**
  - `--batch-size=数量`：Worker 每批发送的文件数（默认 1024）。
  - `--estimated-files=数量`：预估文件数，用于 HashSet 预分配（默认 10,000,000）。
- **输出优化**
  - `src/output.c`：增加 8MB 全缓冲（`setvbuf(..., _IOFBF, 8*1024*1024)`），显著减少 `write` 系统调用次数。

#### 变更

- **主入口重构**：`src/main.c` 完全重写，使用 `AppContext` 统一初始化、运行、销毁生命周期。
- **命令行扩展**：`src/cmdline.c` 新增 `--batch-size` 和 `--estimated-files` 长选项。
- **进度系统重写**：`src/progress.c` 全面适配新模型，支持：
  - `pbin` 分片写入与轮转
  - `spbin` 追加记录
  - `archive` 压缩归档（按 `block_type` 区分 normal / spbin）
  - 从归档 + 散落分片的游标恢复
- **设备管理器扩展**：`src/device_manager.c` / `include/device_manager.h` 增加 `DEV_STATE_CONDEMNED`（永久判死）状态。
- **Makefile 更新**：增加 `-std=gnu11` 编译选项以支持 `_Atomic`。

#### 移除

- **旧线程模型文件**（已备份为 `.bak`，不再参与编译）：
  - `src/traversal.c` → `src/traversal.c.bak`
  - `src/looper.c` → `src/looper.c.bak`
  - `src/monitor.c` → `src/monitor.c.bak`
  - `src/idempotency.c` → `src/idempotency.c.bak`
- **全局变量**：以下旧版全局变量已彻底从活动代码中移除：
  - `g_looper_mq`
  - `g_worker_mq`
  - `g_pending_tasks`
  - `g_visited_history`
  - `g_reference_history`

#### 修复

- **D-State 不可杀死问题**：Worker 从线程改为独立进程后，`SIGKILL` 可在内核层面强制终止，即使进程处于 D-State。
- **消息队列锁竞争**：移除带锁的消息队列，改用内核 pipe 的流式 IPC，消除高并发下的锁瓶颈。
- **假阳性去重风险**：使用 `path + dev + ino` 的 128-bit MD5 指纹，而非仅 inode，避免 inode 复用导致的误判。
- **Worker 心跳丢失误判**：`epoll_wait` 超时路径中集成 Monitor 巡检，500ms  granularity 确保快速发现卡死 Worker。

#### 设计约束

- **同机限制**：IPC 协议中 `struct stat` 直接通过 `memcpy` 序列化，协议**不跨机器/不跨架构**。
- **NFS 挂载前提**：扫描 NFS 目录时，必须采用 `soft,intr,timeo=600` 挂载选项。`hard` 挂载下 D-State 仍不可杀。
- **spbin 归档位置**：`spbin` 块必须是 `.archive` 文件中的**最后一个块**，恢复逻辑依赖此顺序。

---

## [11.0.0] - 2025-XX-XX

### 旧线程模型基线版本

- 多线程共享内存架构（`pthread` + 消息队列）。
- 基础断点续传（`--continue`）。
- 基础设备黑名单（`DeviceManager`）。
- CSV / 自定义格式输出。
- zlib 进度归档（`-Z` / `--archive`）。

