# Changelog

所有显著变更均记录于此文件，格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

---

## [Unreleased]

### 紧急修复：主循环 fd 号重用竞争死锁（P0 阻断性）

**问题背景**：v12.2.2 为修复 "worker epoll busy-loop"，移除了 master 读 fd_out 的 `O_NONBLOCK`。当 Worker 超时死亡后，cleanup 关闭 fd_out，但 epoll 上一轮返回的 events 数组中仍包含该 fd 的 `EPOLLIN` 残留事件。主循环处理该残留事件时调用阻塞 `read(fd_out)`，恰好新 Worker spawn 重用了相同 fd 号且尚未写数据，master 永久阻塞在 `pipe_wait` → 主循环 hang 死 → 所有 Worker 空等 → 完美死锁。

**症状**：Monitor 秒表冻结，pbin/scan_dir.log/输出不再更新，进程 hang 死数小时。

#### 修复

- **恢复 fd_out 的 O_NONBLOCK**：`worker_pool_spawn()` 中给 `out_pipe[0]`（master 读端）重新设置 `O_NONBLOCK`，消除阻塞读死锁。
- **ipc_recv_header 处理 EAGAIN**：返回 `-2`（`EAGAIN`/`EWOULDBLOCK`）而非 `-1`，允许调用方区分"暂时无数据"与"致命错误"。
- **epoll 注册 EPOLLERR|EPOLLHUP**：事件循环中优先处理 Worker 死亡事件，避免在残留事件上尝试读取。
- **EAGAIN 安全跳过**：主循环中 `ipc_recv_header` 返回 `-2` 时直接 `continue`，不触发 `cleanup_dead_worker_slot`，防止误杀新 Worker。
- **ipc_send 失败时防幽灵化**：`process_completed_batch()` 和 `dispatch_lost_tasks()`、`flush_worker_backlogs()` 中 `rc == -1`（管道破裂）时递减 `pending_tasks` 并将路径加入 `lost_tasks` 重试队列，避免任务永久丢失导致 `pending_tasks` 不归零。
- **根任务发送失败 fatal**：`main.c` 中根任务 `ipc_send` 失败时打印致命错误并立即退出，防止无任务可执行的空转。

#### 修改的文件

- `src/worker_proc.c`
- `src/main_loop.c`
- `src/main.c`

---

### 紧急修复：双向管道死锁（P0 阻断性）

**问题背景**：Master 与 Worker 通过 `pipe2(O_CLOEXEC)` 双向通信。`ipc_send()` 使用阻塞 `write()`，当 Worker 的 `fd_in` 管道缓冲区（默认 64KB）被 >580 个 SCAN 任务填满时，Master 阻塞在 `write(fd_in)`。与此同时，Worker 可能正在发送大 BATCH 并阻塞在 `write(fd_out)`。双方互相等待，形成**永久死锁**。

#### 修复

- **管道扩容**：`worker_pool_spawn()` 中通过 `fcntl(F_SETPIPE_SZ, 1048576)` 将 `fd_in[1]` 管道容量从 64KB 提升到 1MB，触发死锁的阈值从 ~580 提升到 ~10,000 个 SCAN 任务。
- **非阻塞写**：对 `fd_in[1]` 设置 `O_NONBLOCK`，`ipc_send()` 在管道满时返回 `-2`（`EAGAIN`），而非永久阻塞。
- **Worker 积压队列（Backlog）**：
  - `WorkerSlot` 新增 `backlog_paths[]`、`backlog_count`、`backlog_capacity` 字段。
  - `process_completed_batch()` 中 `ipc_send()` 返回 `-2` 时，将任务 `strdup()` 存入对应 Worker 的 backlog（动态扩容，初始 64，倍增）。
  - 新增 `flush_worker_backlogs()`：主循环每轮 `epoll_wait` 后遍历所有 Worker，尽可能刷出积压任务；仍满时保留至下一轮重试。
  - 积压满时输出 Warning，递减 `pending_tasks`，丢弃任务（1MB 管道下极罕见）。
- **终止条件前刷 backlog**：`main_loop_run()` 在判定 `pending_tasks == 0 && pending_batches == 0` 之前，先调用 `flush_worker_backlogs()`，确保所有积压任务发送完毕后才停止 Worker。

#### 修改的文件

- `src/worker_proc.c`
- `include/worker_proc.h`
- `src/main_loop.c`

---

### 架构修复：Monitor 模块从线程模型恢复并适配进程模型

**问题背景**：12.0.0 从线程模型重构为进程模型时，`src/monitor.c` 被整体移入 `.bak`，监控功能被临时内联到 `main_loop.c` 中。这导致：
1. `include/monitor.h` 中基于 `pthread_t` 的 `WorkerHeartbeat` 注册机制完全失效；
2. 主循环同时承担 IPC 消息处理和监控巡检双重职责；
3. 监控面板（统计输出）功能完全丢失。

#### 修复

- **Monitor 模块恢复**：
  - 重写 `include/monitor.h`：移除 `WorkerHeartbeat` 注册机制，改为直接读取 `WorkerPool->slots`。
  - 新建 `src/monitor.c`：独立的监控线程，每 500ms 刷新统计面板到 `stderr`（运行时间、Worker 活跃度、吞吐速率、进度、设备状态）。
  - 面板使用 ANSI 清屏（终端环境下），非终端环境下顺序输出。
- **监控线程职责分离**：
  - 心跳超时检查：每秒遍历 `WorkerPool`，kill 超时 Worker 并标记 `pid=-1`。
  - 敢死队探测调度/收割：fork 子进程探测死设备，waitpid 收割并恢复或重调度。
  - 主循环 (`main_loop.c`) 专注处理 IPC 消息，不再内联监控逻辑。
- **`ProbeScheduler` 线程安全化**：增加 `pthread_mutex_t`，支持主循环（push 探测任务）与监控线程（peek/remove 调度探测）并发访问。
- **`AppContext` 结构体命名**：给匿名 `struct` 加上 `AppContext` 标签，修复 `monitor.h` 前向声明与 typedef 的类型不匹配。

#### 修改的文件

- `include/monitor.h`
- `src/monitor.c`（新建）
- `src/main_loop.c`
- `src/main.c`
- `include/app_context.h`
- `include/probe_scheduler.h`
- `src/probe_scheduler.c`

---

## [12.2.1] - 2026-05-07

### 紧急修复与稳定性增强

本次更新修复了 12.2.0 中发现的 11 个缺陷，涵盖阻断性崩溃、命令行行为、输出控制和资源管理。

#### 修复

- **断点续传恢复崩溃（P0 阻断性）**：
  - `restore_progress()` 在活跃分片已完全处理时（`line_count == row_count`），不再错误地将其标记为 `HIST_PUMP_OLD` 继续 pumping，避免 `read_next_pbin_record()` 读到 Footer magic（`0xDEADBEEF66AAC0FF`）作为 `path_len`。
  - `read_next_pbin_record()` 增加 `path_len > MAX_PATH_LENGTH` 防御性校验，读取到异常长度时立即返回 `false`。
  - `read_next_pbin_record()` 改用普通 `malloc` + 错误返回，替代 `safe_malloc`，防止不可信文件数据触发强制进程退出。

- **`--help` / `--version` 返回码（P1）**：
  - `parse_arguments()` 中 `-h` / `-V` 返回特殊码 `2`，`main()` 识别后直接返回 `0`，符合 POSIX 惯例。
  - `default` 分支独立输出 `错误: 未知选项`，与 `--help` 分离处理。

- **`--mute` 选项未实现（P1）**：
  - `async_worker_thread()`：静默时跳过 `print_to_stream` 和切片计数。
  - `process_completed_batch()`：静默时跳过 `--print-dir` 的目录日志输出。
  - `main.c`：`[System] 任务开始/完成` 诊断信息受 `--mute` 控制。

- **`--follow-symlinks` 未生效（P2）**：
  - `worker_proc.c:scan_and_send()` 根据 `g_worker_cfg->follow_symlinks` 动态选择 `stat()`（跟踪）或 `lstat()`（不跟踪），此前硬编码为 `lstat()`。

- **`--clean` 仍生成进度文件（P2）**：
  - `record_path()` 入口增加 `if (cfg->clean) return;`，确保 `--clean` 模式下不创建任何 `.pbin` / `.idx` / `.config` 中间文件。

- **命令行参数内存泄漏（P2）**：
  - `init_config()` 中默认 `progress_base` 改为 `strdup("progress")`，确保所有参数字符串均为堆分配。
  - `main()` 退出前统一 `free`：`target_path`, `output_file`, `output_split_dir`, `progress_base`, `format`, `resume_file`。

- **`create_output_file` 追加模式导致重复输出（P3）**：
  - `fopen(path, "a")` 改为 `fopen(path, "w")`，每次运行覆盖输出文件，避免历史残留数据混入。

- **主循环终止条件不完善（P3）**：
  - `AppContext` 新增 `_Atomic long pending_batches`，精确跟踪已提交到线程池但未完成的 batch 数量。
  - 终止条件增加 `pending_batches == 0` 检查，确保线程池无残留任务后才优雅停止。
  - 移除对未分配任务 Worker `is_alive` 状态的检查（此类 Worker 永远不会发送 EXIT，会导致 `all_idle` 恒为 false）。

- **`precompile_format` 线程安全（P3）**：
  - `static char default_fmt[256]` 改为普通栈局部变量，消除潜在的多线程静态状态风险。

- **`send_batch` 内存分配失败静默丢弃（P3）**：
  - `malloc(total)` 失败时发送空 batch（`count=0`），确保 Master 的 `pending_tasks` 计数能正确递减，避免扫描 hang 住。

#### 修改的文件

- `src/cmdline.c`
- `src/main.c`
- `src/main_loop.c`
- `src/async_worker.c`
- `src/worker_proc.c`
- `src/progress.c`
- `src/output.c`
- `include/app_context.h`
- `README.md`

---

## [12.2.0] - 2026-05-07

### 重大设计升级：同构分片 + 页脚自描述 + 两阶段提交

**设计目标**：让 `fpbin` 从"崩溃即丢弃的临时垃圾"变成"可转正、可恢复、可自描述的一等进度分片"；同时让已完成的分片彻底摆脱对外部 `.idx` 的依赖，实现"随文件迁移、归档、复制"的自描述能力。

#### 核心哲学

- **`pbin` 与 `fpbin` 采用完全相同的物理格式**，只是生命周期和命名前缀不同。
- **已完成的分片是自描述对象**：文件末尾自带 `Footer`（24 字节），记录实际行数与校验信息，不再需要外部 `.idx` 陪伴。
- **活跃分片使用轻量 `.idx` 作为临时草稿纸**，分片封口时通过 **"先盖钢印、再烧草稿"** 的两阶段提交安全落地。

#### 分片格式（统一结构）

```
[数据记录流]
  [path_len][path][dev][ino][mtime][d_type]
  ...

[Footer: 固定 24 字节，文件最末尾]
  magic        : uint64_t  (0xDEADBEEF66AAC0FF)
  row_count    : uint64_t  (该分片实际总行数)
  data_crc32   : uint32_t  (预留，当前填 0)
  footer_crc32 : uint32_t  (覆盖 magic + row_count 的 CRC32)
```

**关键约束**：
- `Footer` 只在分片**封口（seal）**时一次性 `O_APPEND` 写入，不是持续追加。
- 活跃分片**末尾没有有效 Footer**（或即使有残留也不可信），权威来源是配套的 `.idx`。

#### idx 与 Footer 的职责边界

| 阶段 | 权威来源 | 作用 | 存在形式 |
|------|---------|------|---------|
| **活跃分片**（正在接收记录） | `{base}_00000N.idx` / `{base}.fpbin.idx` | 实时跟踪当前行数，支持原子 `rename` 更新 | 独立小文件 |
| **已封口分片**（历史/归档/转正） | `Footer`（文件末尾） | 自描述行数，随文件迁移、归档、复制 | 内嵌元数据 |
| **崩溃恢复** | Footer 优先，idx 兜底 | Footer 校验通过 → 用 Footer；Footer 残缺 → 用残留 idx | 两者配合 |

#### fpbin 生命周期与转正流程

**隔离阶段（HIST_PUMP_OLD）**：
- Master 从历史 `pbin` 分片 pump 任务给 Worker。
- Worker 返回的新发现子目录**不入队、不混写 pbin**，而是追加到 `{base}.fpbin_000XXX` 分片。
- `{base}.fpbin.idx` 实时记录当前 fpbin 分片号与行数。

**触发扫尾（到达截止游标）**：
- 当最后一个历史 `pbin` 分片消费完毕：
  1. **冻结 fpbin**：不再接收新发现。
  2. **封口每个 fpbin 分片**：打开分片，`O_APPEND` 写入 `Footer`（行数来自 `fpbin.idx`）；`fsync` 确保落盘；关闭 fd。
  3. `rename({base}.fpbin_000XXX → {base}_00000N.pbin)`。
  4. **校验**：以 `O_RDONLY` 重新打开所有转正后的 `pbin`，`seek(EOF - 24)` 读取并校验 `magic + crc`。
  5. **回收**：全部校验通过后，删除 `{base}.fpbin.idx`（以及可能的残留 fpbin 临时文件）。

**后续阶段**：
- 新发现直接写入新的 `pbin` 活跃分片（延续原有 `{base}.idx` 逻辑）。
- `fpbin` 机制关闭，直到下一次 `--continue` 恢复时按需重新创建。

#### 崩溃恢复策略

```
restore_progress()
    ├── 扫描目录，识别所有 *.pbin 与 *.fpbin_*
    ├── 对每个已完成的历史 pbin 分片：
    │      seek(EOF - sizeof(Footer))
    │      读取 Footer → 校验 magic + crc
    │      ├─ 通过 → row_count = footer.row_count，加载到 visited_set
    │      └─ 失败 → row_count = 残留 idx（如果有），加载到 visited_set
    ├── 识别当前活跃分片（匹配 {base}.idx 中的 write_slice_index）
    │      行数以 {base}.idx 为准（活跃分片无有效 Footer）
    └── 如果存在残留 fpbin 分片且 fpbin.idx 有效：
           视为上次转正中断，重新执行封口 + rename + 校验
```

**自动清理**：恢复时若发现某个 `pbin` 的 `Footer` 有效但旁边残留了同名 `.idx`，直接 `unlink` 该残留 idx（**钢印清晰则烧草稿**）。

#### 归档增强

- 压缩 `pbin` 分片前，先 `seek` 读 `Footer` 获取 `row_count`，写入 `ArchiveBlockHeader` 作为元数据。
- 解压后得到临时文件，再次读取 `Footer` 校验，双重确认。
- 彻底删除代码中所有 `slice_index * BATCH_SIZE` 或 `slice_index * SLICE_ROWS` 的推断逻辑。每个分片的行数必须来自其自身的 `Footer` 或 `idx`，绝不假设固定。

#### 修改的文件

- `include/archive_format.h`
  - 新增 `PbinFooter`（packed，24 字节）：`magic` + `row_count` + `data_crc32` + `footer_crc32`
  - `ArchiveBlockHeader` 增加 `uint64_t row_count` 字段，用于归档块元数据

- `include/config.h`
  - 新增 `PBIN_FOOTER_MAGIC` 和 `PBIN_FOOTER_SIZE` 常量

- `include/app_context.h`
  - `int fpbin_fd` 改为 `FILE *fpbin_slice_file`
  - 新增 `fpbin_write_slice_index`、`fpbin_line_count`

- `include/progress.h`
  - 新增 Footer 读写校验函数：`write_pbin_footer()`、`read_pbin_footer()`、`verify_pbin_footer()`、`get_slice_row_count()`
  - 新增 fpbin 文件名辅助：`get_fpbin_slice_filename()`、`get_fpbin_index_filename()`
  - 新增按分片 idx 文件名辅助：`get_per_slice_index_filename()`
  - 显式包含 `archive_format.h`

- `src/progress.c`
  - **pbin 封口**：`record_path()` rotate 时先写 Footer，再删按分片 idx；`finalize_archive()` 正常退出时也给最后活跃分片盖 Footer
  - **按分片草稿 idx**：`atomic_update_index()` 同步更新统一 idx 与 `{base}_00000N.idx`；rotate 后旧分片 idx 被删除
  - **fpbin 多分片**：`fpbin_append()` 支持基于 `{base}.fpbin_000XXX` 的多分片写入；`fpbin_rotate_slice()` 在分片满时封口写 Footer
  - **转正流程重写**：`promote_fpbin_to_pbin()` 实现"封口 → rename → 校验 → 清理"完整两阶段提交；`find_max_pbin_index()` 防止覆盖已转正分片
  - **恢复逻辑重写**：`restore_progress()` 引入 Footer-first 扫描；支持残留 fpbin 自动重新转正；已完成分片优先读 Footer，无效时 fallback 到按分片 idx，再失败则全量解析兜底
  - **归档增强**：`archive_slice_to_file()` 分离数据区与 Footer，只压缩数据区；将 `row_count` 写入 `ArchiveBlockHeader`
  - **解析增强**：`parse_pbin_buffer()` 新增 `max_rows` 参数，精确控制解析行数；`iterate_pbin_slices()` 自动跳过 Footer 区域
  - **sanity check**：`iterate_archive()` 与 `count_archive_blocks()` 增加 `block_type` 与大小上限校验，防止旧格式或损坏 archive 导致崩溃
  - **清理增强**：`cleanup_progress()` 覆盖 fpbin 文件与按分片 idx；兼容删除旧版本 `"progress.fpbin"`

- `src/main.c`
  - `app_context_init()` / `app_context_destroy()` 适配新的 `fpbin_slice_file` 字段

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
