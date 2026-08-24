# Changelog

所有显著变更均记录于此文件，格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

---

## [15.6.1] - 2026-08-24

### Fixed：/public4 生产事故热修复（Worker 生命周期可靠性，P0-101~109）

事故摘要：/public4 全量扫描运行 2d18h 后 6/8 Worker 死亡、`pending_tasks=-6`、主循环
空转 54 小时、零日志。取证全记录见 `Design-todo-v15.6.1.md`，实现细节与验证证据见
`fix_documents/fixed_15.6.1_P0_incident_hotfix.md`。

**P0 — Critical（9 项全部闭环）：**
- **P0-101 死亡类消息同代校验**：RET_DEAD/RET_DEV_TIMEOUT/RET_EXIT（及 RET_ERROR/
  RET_ENTRY_ERROR）携带 IPC 线程观测到的 (pid, epoch)，Master 仅受理
  `reported_pid == slot->pid` 的同代消息——修复旧 stale 判定反转导致真实死亡被 100%
  误吞（崩溃 Worker 永不清理/替换/销账）的最重缺陷。
- **P0-102 cleanup 销账精确化 + DEV_TIMEOUT 节流**：仅 DT_SCANNING 由 cleanup 销账
  （orphaned 不叠加，单 slot 单在途任务语义）；屏障接管态（DT_BATCHES_*）由完成屏障
  销账完结、免重扫，替换延迟到屏障完结后；空闲 Worker 死亡不再销账（原实现无条件
  `pending--`，启动期 6 连退直接把账目打成 -6）；Worker 侧同一任务只报一次
  DEV_TIMEOUT（原实现每 5s 重发，是跨代重复销账级联的放大器）。
- **P0-103 DEV_TIMEOUT 先杀后清**：cleanup 先 SIGKILL 并限时收割，消除新旧两代并存
  窗口（原实现靠 CMD_REPLACE 关 fd 让旧 Worker EPIPE 自杀）。
- **P0-104 账目不变量**：`pending_tasks < 0` 立即 log_fatal + _exit(2)；完结条件
  改 `<= 0`。账目 bug 秒级暴露，不得死等。
- **P0-105 有效进展看门狗**：monitor 线程监督 file+dir 计数——无增长且仍有应做工作
  持续超 `--stall-timeout`（默认 900s，须大于最大退避 300s）→ 输出现场并按
  `--stall-action=exit|abort` 终止。主循环 tick 看门狗对"活而无效"无效，故按有效
  进展判定。
- **P0-106 日志门控违规清扫**：DEV_TIMEOUT 全链路、cmd_queue 满丢 SCAN、熔断跳过
  写 spbin、FINISH 发送最终失败、Worker 退出出口（POLLHUP/recv/malloc）、Worker
  替换事件——全部解除门控或补日志，默认级别可见。
- **P0-107 fork 安全三件套**：全部 fork 前移到单线程期 + 巨型指纹集合分配之前
  （严格超售下小 VSZ fork 必成功，多线程 fork 锁继承风险同时消除）；预备役 Worker
  池（启动期预 fork num_workers 个 spare，运行期替换零 fork，spare 耗尽响亮降额）；
  子进程无锁日志模式（规避 flockfile 继承死锁）。
- **P0-108 spawn/fork 失败必须有声**：所有 fork/pipe 失败路径全局 log_error 含
  errno——修复严格超售下 fork ENOMEM 静默 54 小时的根因性无声。
- **P0-109 RET_EXIT 携带在途任务 = 非预期死亡**：在途目录重入队 + 全局 log_error——
  修复原实现丢弃在途目录且不留痕导致的子树静默丢失（1.9 亿 vs 8 亿缺口的真凶）。

**实现期缺陷实录**（混沌压测暴露并同轮修复，详见 fixed 文档 §3）：
- 迟到 RET_ERROR 跨代误销账（已纳入同代校验）；
- orphaned 与在途任务双倍销账（已按单在途语义精确化）。

**验证**：回归 19/19（新增用例 14：kill -9 Worker spare 补位；用例 15：全灭+spare
耗尽看门狗 exit=2）；6 并发 × 8 轮压测 48/48；混沌 kill 压测 40/40。

**⚠️ 运维提示：**
- 生产重跑请从头全量（不用 --continue）；日志务必重定向到文件（`2> run.log`）。
- 严格超售（`vm.overcommit_memory=2`）环境：本版本运行期零 fork，理论免疫 fork
  ENOMEM；启动期请关注 `[Spare]` 日志确认预备役就绪。

**修改的文件**（详见 fixed 文档 §6）：`include/{core,ipc,output,scan,util}/*`、
`src/{core,ipc,output,scan,util}/*` 共 18 个源文件 + `tests/run_regression.sh`。

---

## [15.6.0] - 2026-08-20

### Architecture：12 项 P0 设计闭环（Design-todo-v15.6.0 全部落地）

**P0 — Critical（12 项全部闭环）：**
- **P0-012 epoch 机制 + waitpid 确认**：Worker 替换引入 epoch 代次标记，旧 Worker 残留数据（FINISH/BATCH/心跳）经 waitpid 确认死亡后按 epoch 过滤，杜绝死亡 Worker 的迟到消息污染新 Worker 账目。
- **P0-006 IPC 队列扩容 + MSG_DROP 废除**：IPC 命令队列扩容至 65536，正常路径禁止 MSG_DROP——队列满时阻塞等待而非丢弃重派，消除 MSG_DROP 回队不销账导致的 pending_tasks 永久泄漏。
- **P0-001 目录任务完成屏障**：目录任务以 FINISH 到达 + 该目录全部 BATCH 提交为完成屏障，修复 FINISH/BATCH 竞态下目录被提前记账完成、后续批次无家可归的问题。
- **P0-002 输出三态状态机**：输出条目 DISCOVERED → OUTPUT_QUEUED → OUTPUT_COMMITTED 三态流转，Worker 死亡时未 COMMITTED 的输出批次（`output_pending` 按 slot 计数）随目录重扫回滚，保证输出不多不少。
- **P0-009 目录任务生命周期状态机**：目录任务全生命周期状态机化（发现/入队/派发/完成/出错），所有转移有明确记账，完结校验以状态机账目为准。
- **P0-004 三集合统一队列模型**：visited_set 拆分为 discovered_set（仅目录去重）+ enqueued_set（派发去重）+ completed_set（完成记账），背压/恢复/重扫统一经 `enqueue_dir` 入口做差集剪枝，消除背压竞态。
- **P0-003 fpbin/dfpbin 原子对**：恢复期间新发现子目录隔离写入 fpbin、已完成目录写 dfpbin，二者构成原子对支持二次崩溃恢复——恢复过程本身被 KILL 后仍可再次恢复，不漏扫不重扫。
- **P0-010 Reset 援救机制**：崩溃恢复矩阵补全 Reset 援救路径——恢复中遇到无法对账的残留状态时回退到安全的全量重扫语义，而非带伤续跑。
- **P0-005 spbin 五原因码恢复路径 + 毒丸隔离（含 P1-004）**：spbin 记录携带五类原因码，恢复时按原因码分别走重试/跳过/设备等待路径；正常退出时 spbin compaction（过滤 RECOVERED 条目重写）；毒丸目录隔离，不再阻塞同设备其他目录。
- **P0-007 RET_ERROR 状态机补全**：出错目录不重入队、登记 enqueued_set 后等待设备恢复，`record_path` 绕过集合去重直写 pbin（P0-007 起出错目录的恢复不再依赖重派发）。
- **P0-008 Run manifest + baseline_eligible + archive 原子切换**：新增 `{base}.manifest`（`.new → fsync → rename` 原子写，单行覆盖语义）为唯一权威状态来源；启动即写 status=Running 覆盖上次终态，修复 .config 追加写多行 status 被 strstr 误判的链式基准漏洞；baseline_eligible=1 需同时满足：status=Success、spbin 清零、dspill 排空、无 fpbin/dfpbin 残留、archive 校验通过、输出尾部完整、非盲信运行。
- **P0-011 pbin schema 2 + 盲信纯路径 key + 完整历史 stat 复用 + --reference-base**：pbin 升级为 schema 2（`[path_len][path][d_type][mtime_sec][mtime_nsec][size][uid][gid][mode][dev][ino][flags]`，atime 完全排除）；盲信基准经 `--reference-base` 显式指定（须 baseline_eligible=1 且 pbin_schema_version=2），本轮 `-f` 必须为新空目录（增量与基准物理隔离）；reference_set/map 以纯路径指纹为 key 收录基准完整历史 stat，盲信命中时复用 size/mtime/uid/gid/mode 免 lstat；盲信结果 baseline_eligible=0，不能链式作为下次基准。

**其他变更：**
- **修复 Worker 轮询计数器整数溢出段错误**：`next_dispatch_worker` 由无限自增改为取模回卷——自适应零等待主循环下原实现约 2 分钟即溢出为负，导致 `slots[-15]` 野读 SIGSEGV（回归用例 2 实测偶发崩溃，基线 v15.5.9 潜伏同病但低速循环下不可达）。
- **修复 per-slot 数组越界导致的派发活锁**：`redispatch_backoff_until`/`timeout_paths`/`timeout_counts` 由硬编码 `[8]` 改为 `[MAX_WORKERS=64]`，`--worker-count` 增加上限钳制——此前 wid≥8 时越界读到相邻 `run_id` 的 ASCII 字节被当作巨大退避截止时间，低号 Worker 全忙时主线程在 dispatch 阶段永久空转（单实例约 6%、6 并发压测 32/120 轮复现；修复后 120/120 全通过）。
- **Makefile 增加 `-MMD -MP` 头文件依赖跟踪**：头文件变更（如 AppContext 布局）现在会触发全量重编，杜绝新旧目标文件混用结构体偏移。
- **pbin 全量记录**：record_path 不再要求 `-c` 模式，全量扫描同样记录 pbin，保证任何成功运行都可作为盲信基准/恢复来源。
- **主循环自适应等待**：main_loop 由固定轮询改为条件变量 + 自适应退避等待，修复 cond 丢失唤醒导致的吞吐下降（实测曾降 10 倍），空转 CPU 与吞吐兼得。
- **退出码语义统一（§9.6）**：0=完全完成；1=部分完成（跳过/spbin 残留/dspill 残留/熔断清单非空/输出尾部不完整）；2=严重失败（盲信基准不合格、进度不兼容、初始化致命错误）；3=架构不匹配（预留）。

**⚠️ 破坏性变更：**
- **旧格式进度文件不兼容**：v15.6.0 之前的 pbin/config 进度（无 manifest、schema 1）不能续传、不能作为盲信基准。检测到旧残留时拒绝续传（exit 2），必须使用 `--runone` 重新全量扫描建立新基准。

**修改的文件**（详见 `fix_documents/fixed_15.6.0_P0_all_in_one.md`）：
- `include/core/config.h` — VERSION "15.6.0"，IPC 队列容量 65536，PBIN_SCHEMA_VERSION 2
- `include/core/app_context.h` / `include/output/manifest.h` / `include/output/progress.h` / `include/output/spbin.h` / `include/scan/main_loop.h` / `include/scan/reference_map.h` / `include/ipc/worker_proc.h` / `include/output/async_worker.h`
- `src/core/main.c` / `src/core/cmdline.c` — 盲信门禁、`--reference-base`、manifest 生命周期、退出码语义
- `src/scan/*`（main_loop / dispatch / batch_processor / worker_scanner / thread_pool / reference_map）— epoch、屏障、三态、统一队列、RET_ERROR、自适应等待
- `src/output/*`（manifest / progress / progress_archive / progress_io / output_format / async_worker / monitor）— manifest、fpbin/dfpbin、spbin 恢复、Reset 援救、archive 原子切换
- `tests/run_regression.sh` — 新增用例 10-13（崩溃-续传一致性、二次崩溃 fpbin/dfpbin、盲信门禁、不完整运行不可作基准）

---

### Fixed：NFS 大目录场景下 DEV_TIMEOUT 误判深度加固

**P0 — Critical：**
- **HEARTBEAT_TIMEOUT 30s 对 NFS 大目录过严**：Worker Scanner 在 NFS 上遍历大目录时，`readdir()` 单次 RPC 可能耗时数十秒，`opendir()` 本身也是多 RPC 聚合。30 秒无心跳即被 IPC 线程判定为 DEV_TIMEOUT，Master 替换 Worker 后再次派发同一目录，形成无限循环。
  - 修复：HEARTBEAT_TIMEOUT_SEC 从 30 提高到 120，给 NFS 大目录足够喘息时间。

- **CIRCUIT_BREAKER_THRESHOLD 3 次过严**：目录级熔断原阈值为 3，即 30s*3=90s 后熔断。但对于 7 万+ 文件的 NFS 大目录，90s 远远不够，导致路径过早被熔断跳过，目录丢失。
  - 修复：CIRCUIT_BREAKER_THRESHOLD 从 3 提高到 10，配合 120s 超时，总容忍时间扩大到 1200s（20 分钟）。

- **redispatch 无退避，同一目录被瞬间连续派发**：Worker 因 DEV_TIMEOUT 死亡后，`cleanup_dead_worker_slot` 立即将同一目录 requeue，下一个 Worker 瞬间接手再次卡死，CPU 空转且 dispatch_queue 被快速消耗。
  - 修复：新增 redispatch 指数退避机制。`cleanup_dead_worker_slot` 在 requeue 前，根据该路径已连续超时次数设置退避时间：1 次→30s、2 次→120s、3 次+→300s。退避期间 `dispatch_from_queue` 遇到该 slot 直接跳过，等待时间到期后再尝试。

- **scanner_progress_tick 计数驱动（每 1000 条目）在大目录前段无保护**：NFS 上处理 1000 个条目可能需要数分钟，前 999 条期间 `last_progress` 不更新，IPC 线程随时可能误判。
  - 修复：`scanner_progress_tick` 从计数驱动改为时间驱动（每 5 秒），无论处理多慢，心跳持续更新。参数从 `(int *entry_count, int interval)` 改为 `(time_t *last_tick_time, int interval_sec)`。

- **opendir() 和 send_batch() 期间无心跳保护**：`opendir()` 在 NFS 上是多 RPC 调用，大目录可能阻塞数十秒；`send_batch()` 涉及 IPC 写管道，大 batch 可能阻塞。两者期间 `last_progress` 不更新，被误判卡死。
  - 修复：`opendir()` 成功后立即 tick 一次；`send_batch()` 前后各 tick 一次，确保长耗时操作期间心跳持续。

**修改的文件**：
- `include/core/config.h` — VERSION "15.5.9"，VERSION_CODE 202608090000UL，HEARTBEAT_TIMEOUT_SEC 120
- `include/core/app_context.h` — CIRCUIT_BREAKER_THRESHOLD 10，新增 `redispatch_backoff_until[8]`
- `src/core/main.c` — 初始化 `redispatch_backoff_until` 数组
- `src/scan/dispatch.c` — redispatch 指数退避逻辑 + `dispatch_from_queue` 退避跳过
- `src/scan/worker_scanner.c` — `scanner_progress_tick` 时间驱动重构 + opendir/send_batch 前后 tick

---

## [15.5.3] - 2026-07-06

### Fixed：大目录 readdir 超时误判（DEV_TIMEOUT false-positive）+ 目录级熔断 + 日志版本化

**P0 — Critical：**
- **大目录 readdir 超时误判**：Worker Scanner 遍历大目录（如 72,275 个文件的 `gvcf_stats`）时，`readdir()` 循环可能持续数分钟，但 `last_progress` 只在扫描开始/结束时更新。IPC 线程 30 秒无进度信号即上报 `DEV_TIMEOUT`，Master 替换 Worker 后再次派发同一目录，形成无限循环，最终耗尽 dispatch_queue。
  - 修复：`scan_and_send()` 的 `readdir()` 循环中每处理 1000 个条目调用 `scanner_progress_tick()` 更新 `last_progress`，让 IPC 线程知道 Scanner 仍在正常工作。

- **目录级熔断（Circuit Breaker）**：同一死亡目录被反复 redispatch，所有 Worker 逐个卡死，dispatch_queue 从数万降至 0，扫描完全停滞。
  - 修复：新增 `circuit_breaker_check()`，每个 Worker slot 独立追踪最近 timeout 的路径和连续次数。达到 `CIRCUIT_BREAKER_THRESHOLD(3)` 后跳过该路径，不再重试。不同路径会重置计数器。

**P1 — High：**
- **DEV_TIMEOUT 相关日志在 --mute 下仍刷屏**：`[Worker-N] Scanner stuck`、`[Bus] Worker N DEV_TIMEOUT (scanner stuck), replacing`、`[DispatchQueue] no IDLE worker available, requeue` 等日志为 ERROR/WARN 级别，不受 `--mute` 影响，在 NFS 大目录场景下每秒产生数千行。
  - 修复：所有 DEV_TIMEOUT 相关日志（Worker 侧 `Scanner active` / `Scanner stuck`、Bus 侧 `DEV_TIMEOUT`、dispatch 侧 `no IDLE worker` / `cmd_queue full`）统一应用版本号 `202607030000UL`。当前 `VERSION_CODE=202607061400 > 202607030000`，默认静默；需要排查时通过 `--verbose-version=0` 或 `--verbose-version=202607030000` 重新打开。

**修改的文件**：
- `include/core/config.h` — VERSION "15.5.3"，VERSION_CODE 202607061400UL，新增 CIRCUIT_BREAKER_THRESHOLD
- `include/core/app_context.h` — 新增 `timeout_paths[8][4096]` / `timeout_counts[8]` 熔断状态字段
- `src/scan/worker_scanner.c` — `scanner_progress_tick()` 心跳函数 + readdir 循环中每 1000 条目调用
- `src/scan/dispatch.c` — `circuit_breaker_check()` + `cleanup_dead_worker_slot()` 集成熔断逻辑
- `src/ipc/worker_proc.c` — `Scanner active` / `Scanner stuck` → `log_info_v` / `log_error_v(202607030000UL)`
- `src/scan/main_loop.c` — `[Bus] DEV_TIMEOUT` → `log_error_v(202607030000UL)`
- `src/scan/dispatch.c` — `cmd_queue full` / `no IDLE worker` → `log_warn_v(202607030000UL)`

---

## [15.5.2] - 2026-05-20

### Architecture: pbin sliding window backpressure + dispatch_queue leak fixes

**核心变更**：
- **pbin 滑动窗口背压**：dispatch_queue 内存 hard cap（10 万条）。batch_processor 在 queue 满时停止 push，目录仍通过 `record_path` 写入 pbin。当 queue 降到 3 万条时，`main_loop` 触发 `load_dirs_from_pbin` 从 pbin cursor 顺序加载 5 万条目录回填 queue。保证一次运行完成，不依赖 continue。
- **dispatch_queue_push 失败后 strdup 泄漏修复**：统一 8 处调用点（batch_processor、dispatch.c、progress.c、progress_archive.c），push 失败时释放 path。
- **scan_existing_output_slices 边缘匹配修复**：增加 `.txt` 后缀严格校验，防止 `123.txt.bak` 被误识别为输出分片。
- **`--progress-slice-lines` 参数**：新增命令行参数，允许用户自定义 pbin 分片大小（默认 10 万），支持扩大切片以减少滑动窗口的脉冲读频率。

**修改的文件**：
- `include/core/config.h` — 版本号 15.5.2，VERSION_CODE 202605201552UL，新增 DISPATCH_QUEUE 水位常量
- `include/core/app_context.h` — 新增 `pbin_queue_cursor`
- `src/scan/batch_processor.c` — HIGH_WATER 背压守卫
- `src/scan/dispatch.c` — 新增 `load_dirs_from_pbin`
- `src/scan/main_loop.c` — 加载触发器
- `src/core/cmdline.c` — 新增 `--progress-slice-lines` 参数与 help
- `src/scan/batch_processor.c` / `src/scan/dispatch.c` / `src/output/progress.c` / `src/output/progress_archive.c` / `src/output/output_format.c` — 泄漏修复与边缘匹配修复
- `include/output/progress.h` / `src/output/progress_archive.c` — `read_next_pbin_record` 跨文件可见

---

## [15.5.1] - 2026-05-20

### Fixed：v15.5.0 审计修复（P0/P1/P2）

**P0 — Critical：**
- **终止条件未检查 dispatch_queue 非空**：`main_loop.c` 终止条件追加 `dispatch_queue_count() == 0`，防止 dispatch_queue 中有待派发任务时程序过早终止导致目录丢失。
- **cleanup_dead_worker_slot 导致 pending_tasks 双重计数**：redispatch 入队时不再 `++pending_tasks`，由 `dispatch_from_queue` 中 `send_scan_to_ipc` 成功路径统一计数，消除 Worker 死亡-替换循环中的计数漂移。

**P1 — High：**
- **dispatch_queue push 失败泄漏内存**：`MSG_DROP` requeue 失败时释放 `strdup` 的 path；`push_backlog` realloc 失败时释放所有未转移的 `backlog_paths[i]`。
- **dispatch_queue pop O(n) memmove → O(1) 环形缓冲**：引入 `head` 索引实现环形缓冲区，pop 时不再 memmove 整个数组，积压数万级任务时主循环不再卡顿。
- **输出恢复逻辑（--continue）**：`output_format.c` 新增 `scan_existing_output_slices` 和 `count_file_lines`，resume 时自动扫描 output_split_dir 找到最大分片号、恢复行数；单文件模式同样恢复行数，避免覆盖或重复输出。

**P2 — Medium：**
- **清理 idx 遗留代码与注释**：删除 `RuntimeState` 中死字段 `process_slice_index`；更新 `progress_archive.c`、`progress_io.c` 头部注释为 dpbin 模型；移除孤儿注释 `get_pbin_slice_line_count`。
- **统一日志调试常量为 202605201600UL**：`cleanup_dead_worker_slot` 中使用 `202605150000` 的日志被版本过滤遮蔽，已统一。

**修改的文件**：
- `include/core/config.h` — 版本号 15.5.1，VERSION_CODE 202605201551UL
- `include/core/config.h` — 删除 `process_slice_index`
- `src/scan/main_loop.c` — 终止条件 + MSG_DROP 泄漏修复
- `src/scan/dispatch.c` — cleanup redispatch 计数修复 + 日志常量统一
- `src/scan/dispatch_queue.c` / `include/scan/dispatch_queue.h` — 环形缓冲重构
- `src/output/output_format.c` — 输出恢复逻辑 + dirent.h
- `src/output/progress_archive.c` — 头部注释更新
- `src/output/progress_io.c` — 头部注释 + record_path 注释清理

---

## [15.5.0] - 2026-05-20

### Architecture: SEDA dispatch_queue + pbin/dpbin refactor + blind-trust directory exclusion

**核心变更**：
- **废除 idx 5 字段游标**：恢复为 pbin Footer 自描述 + dpbin 差集恢复模型。idx 文件不再被读取或写入。
- **清理 deprecated idx 代码**：删除 `atomic_update_index`、`load_progress_index`、`get_index_filename`、`get_per_slice_index_filename` 及其所有调用；`cleanup_progress` 和 `finalize_archive` 不再操作 idx 文件；`record_path` 不再创建 per-slice idx 草稿。
- **引入 dpbin（完成日志）**：目录在 `process_completed_batch` 成功派发后写入 dpbin。dpbin 只分片、不归档，正常扫描结束后删除。续传时 `pbin - dpbin = 差集`，只 pumping 未完成目录。
- **引入 dispatch_queue（Stage 3→4 队列）**：`batch_processor` 只 push，`dispatch_from_queue` 只 consume。统一 `pending_tasks` 语义：仅在 `send_scan_to_ipc` 成功后 `++`。
- **pbin salvage**：散落分片截断时，顺序解析保留有效行、重新封口，避免整片丢弃。
- **盲信目录排除**：`try_blind_trust` 中 `DT_DIR` 直接返回 `false`，目录始终走 `lstat`，仅文件享受盲信跳过。

**修改的文件**：
- `include/core/config.h` — 版本号 15.5.0
- `include/core/app_context.h` — `DispatchQueue` 替换 `LostTasksQueue`，新增 `completed_set`
- `include/scan/dispatch_queue.h` / `src/scan/dispatch_queue.c` — 新增（替代 lost_tasks）
- `src/scan/batch_processor.c` — Stage 3 只 push 到 dispatch_queue
- `src/scan/dispatch.c` — `dispatch_from_queue` 消费 + dpbin_append
- `src/scan/main_loop.c` — 调用 `dispatch_from_queue`
- `src/scan/worker_scanner.c` — 目录盲信过滤
- `src/output/progress_io.c` — dpbin 写入/删除 + pbin salvage
- `src/output/progress_archive.c` — 差集恢复逻辑重写
- `src/output/monitor.c` — 显示 dispatch_queue 计数
- `src/output/progress.c` — `finalize_progress` 移除 idx 写入，正常结束时调用 `dpbin_delete_all`，`cleanup_progress` 清理 dpbin
- `include/output/progress.h` / `src/output/progress.c` — 删除 idx 相关 helper 声明与定义
- `src/core/main.c` — `dpbin_delete_all` 正常结束时调用

---

## [15.4.5] - 2026-05-18

### Fixed：续传模式（--continue）丢失深层子目录（P0）

**问题背景**：第一次执行被 Ctrl+C 中断后，续传（`--continue`）时 `batch_dedup_worker` 对所有目录都执行 `visited_set` 去重。由于父目录的 BATCH 结果中的子目录在第一次执行时就已经被插入 `visited_set`（作为"已见条目"），续传 pumping 阶段重新扫描父目录时，这些子目录被标记为 duplicate 跳过，导致其深层子目录永远无法被发现。程序 1 秒完成，但目标存储中仍有大量未扫描文件。

**修复**：
- `batch_dedup_worker()`：在 `HIST_PUMP_OLD` 阶段，对**目录**跳过 `visited_set` 去重（文件仍去重，避免输出重复）。这样 pumping 阶段发现的子目录不会被提前过滤。
- `process_completed_batch()`：在 `HIST_PUMP_OLD` 阶段，对目录统一走 `fpbin_append` + `send_scan_to_ipc()`（之前仅 `fpbin_append`，不派发扫描任务）。这样丢失的目录在 pumping 阶段即被重新扫描，Worker 返回的 BATCH 中深层子目录若在 `visited_set` 中则正常跳过，若不在则继续递归扫描。

**修改的文件**：
- `src/scan/batch_processor.c`

### Fixed：IPC BATCH 消息 PIPE_BUF 大小限制（P0）

**问题背景**：`ipc_send()` 对所有消息类型强制执行 `total_len > 4096` 即返回致命错误。Worker 侧 `scan_and_send()` 按固定 1024 条记录 batch，遇到大目录时 BATCH payload 可达数百 KB，`ipc_send()` 直接拒绝发送，导致 Worker 数据丢失、Master 统计不完整，程序提前退出但标记 Success。

**修复**：
- `ipc_send()`：仅对非 `IPC_MSG_BATCH` 消息保留 `PIPE_BUF(4096)` 原子写检查；BATCH 消息允许超过 4096 bytes。
- 理由：`fd_data` 为单 Writer（Worker Scanner 线程）单 Reader（Master IPC 线程），pipe 上不存在多写者交错风险；`write()` 循环 + `partial_retry` 机制已可安全处理大消息的非阻塞写入。

**修改的文件**：
- `src/ipc/ipc_protocol.c`

---

## [15.4.4] - 2026-05-18

### Fixed：IPC FSM BATCH Footer 读取协议修复（P0）

**问题背景**：v15.4.0 引入的 FSM 续传机制中，`read_data_message()` 的 PAYLOAD 阶段读取了包含 Footer 的全部 `payload_len` bytes，随后 FOOTER 阶段试图再读取 8 bytes Footer，导致管道已空、`poll(100ms)` 超时。BATCH 永远卡在 FOOTER 阶段，主线程收不到任何 BATCH，程序 0 秒退出、0 输出。

**修复**：
- PAYLOAD 阶段：只读取 `payload_len - sizeof(uint64_t)` bytes（payload body，不含 Footer）。
- FOOTER 阶段：单独 `fsm_recv()` 读取 8 bytes Footer，验证魔数后复制到 `fsm->buf` 末尾。
- 转发时 `net_payload_len = payload_len - 8`，保持主线程解析逻辑不变。

**修改的文件**：
- `src/ipc/ipc_message_handler.c`
- `include/core/config.h`（版本号 15.4.4）

---

## [15.4.3] - 2026-05-18

### Fixed：thread_pool completed 链表安全（P2）

**问题背景**：`worker_thread()` 中 `malloc(sizeof(CompletedNode))` 失败时原实现打印 `log_fatal` 后继续循环但不释放 batch，导致内存泄漏且 `pending_batches` 不归零；`poll_completed` 无链表完整性校验，自循环时主线程永久空转；`destroy` 无 drain 上限。

**修复**：
- `worker_thread()`: `node` malloc 失败时释放 batch 全部内存并 `continue`。
- `thread_pool_poll_completed()`: 自循环检测（`node->next == node` 时断开）+ 遍历上限 100000。
- `thread_pool_destroy()`: drain 安全上限 `drained_count < 100000`。

**修改的文件**：
- `src/scan/thread_pool.c`
- `include/core/config.h`（版本号 15.4.3）

---

## [15.4.2] - 2026-05-18

### Fixed：fp_shard_insert_internal 安全加固（P1）

**问题背景**：`fp_set_create()` 中 `expected_count * 2` 溢出导致 per_shard 极小值；`PROBE_LIMIT = 1000000` 截断小容量分片的探测循环；rehash 失败时旧 table 已丢失。

**修复**：
- `fp_set_create()`: `expected_count > (SIZE_MAX / 4)` 时返回 NULL，饱和防溢出。
- `fp_shard_insert_internal()`: `PROBE_LIMIT` → `shard->capacity`，消除截断。
- rehash 失败时完整回滚旧指针、旧 capacity、旧 count/tombstones，数据零丢失。

**修改的文件**：
- `src/scan/fingerprint_set.c`
- `include/core/config.h`（版本号 15.4.2）

---

## [15.4.1] - 2026-05-18

### Fixed：ipc_send 部分写入防御（P0）

**问题背景**：`MAX_PATH_LENGTH = 4096` 与 `PIPE_BUF = 4096` 相同，当 payload 含 4096 字节路径时 `total_len = 4104 > PIPE_BUF`，非阻塞 pipe 可能部分写入导致协议失步。`write()` 返回 `n == 0` 无处理，部分写入后 `EAGAIN` 无限重试。

**修复**：
- `config.h`: `MAX_PATH_LENGTH` 4096 → 4088（`PIPE_BUF - sizeof(IpcMessageHeader)`）。
- `ipc_send()`: 运行时 `total_len > PIPE_BUF` fatal guard；`n == 0` 防御返回 `-1`；部分写入后 `EAGAIN` 增加 1000 次重试上限。

**修改的文件**：
- `include/core/config.h`
- `src/ipc/ipc_protocol.c`

---

## [15.4.0] - 2026-05-18

### 架构：IPC FSM 续传读取 + BATCH Footer 魔数

**问题背景**：v15.0.x ~ v15.3.x 期间 IPC 线程使用一次性 `read()` 读取 Header/Payload，`EAGAIN` 时丢弃部分数据，下次 `epoll_wait` 后从断点继续但调用方已丢失进度，导致协议解析错乱。BATCH 大数据尤其脆弱，反复丢弃 → 无限失步 → payload timeout。

**设计**：
- `IpcReadFsm` 状态机：为每个 fd 维护独立的 `state`、`nread`、`hdr`、`buf`，支持跨多次 `epoll_wait` 调用的可恢复续传读取。
- `fd_ctrl`（控制通道）：两阶段 FSM（`HDR → PAYLOAD`），处理 HEARTBEAT/ERROR/EXIT/READY/FINISH。
- `fd_data`（数据通道）：三阶段 FSM（`HDR → PAYLOAD → FOOTER`），处理 BATCH 大数据。Footer 魔数 `0xDEADBEEF66AAC0FF` 校验数据完整性。
- `fsm_recv()` 通用续传原语：`poll(100ms)` + `read`，`EAGAIN` 返回 `-2` 但保留 `buf` 和 `nread`，下次继续同阶段读取。
- `CMD_REPLACE` 时彻底重置两个 FSM 并释放 `buf`，防止旧 Worker 状态污染新连接。
- `ipc_msg_type_valid()` 白名单 + `hdr.payload_len > 100MB` 上限，防御性拒绝畸形数据。

**修改的文件**：
- `include/ipc/ipc_protocol.h` — `IPC_FOOTER_MAGIC`、`IpcReadFsm`、`fsm_recv`、`safe_*_fsm`、`ipc_msg_type_valid`
- `include/ipc/ipc_thread.h` — `ctrl_fsm`、`data_fsm`
- `src/ipc/ipc_protocol.c` — `fsm_recv` 实现、三阶段 `safe_*_fsm`、`ipc_msg_type_valid`
- `src/ipc/ipc_thread.c` — FSM 初始化、`CMD_REPLACE` 重置
- `src/ipc/ipc_message_handler.c` — `read_ctrl_message` / `read_data_message` FSM 版本
- `src/scan/worker_scanner.c` — `send_batch` 追加 Footer 魔数
- `include/core/config.h`（版本号 15.4.0）

---

## [15.3.0] - 2026-05-18

### 新增：版本化日志框架（Versioned Logging）+ `--verbose-version`

**问题背景**：v12.x ~ v15.x 期间为排查多次 P0 阻断性问题（fd 号重用死锁、payload timeout 级联、pending_tasks 不归零、IPC 线程卡死等），引入了大量高频追踪日志（`ret_queue send OK`、`received FINISH forwarding`、`Bus Worker X FINISH`、`heartbeat sent`、`ipc_send fd=X` 等）。这些日志在生产环境正常运行时无价值，但默认 verbose 模式下每秒产生数千行，3 小时 41 分钟累积 **987MB / 1000 万行**，严重污染 stderr 并拖慢 I/O。

**设计目标**：
1. 默认运行（不传 `--verbose-version`）时，旧版本引入的调试追踪日志自动静默。
2. 关键日志（启动、完成、错误、Worker 替换、设备熔断）始终输出，不受版本衰减影响。
3. 需要排查历史问题时，可通过 `--verbose-version=TIMESTAMP` 精确打开某个版本之后的所有日志。
4. 与现有 `verbose_level`（0~3）正交共存。

**核心机制**：
- `config.h` 定义 `VERSION_CODE = 202605180903UL`（年月日时分），每次发布递增。
- 日志宏新增 `_v(版本, ...)` 变体：
  - `log_info_v(ver, ...)`、`log_debug_v(ver, ...)` 等。
  - 若 `ver < g_log_version_threshold`，该日志静默。
  - 默认 `threshold = VERSION_CODE`（不传 `--verbose-version` 时）。
  - 传 `--verbose-version=0` 时 `threshold = 0`，所有日志输出。
- 现有 `log_info(...)` 等便捷宏自动等价于 `log_info_v(VERSION_CODE, ...)`，无需逐条迁移。
- 旧高频追踪日志手动降级为 `log_*_v(202605150000, ...)`（v15.0.x ~ v15.1.x 引入），默认被遮蔽。

**命令行新增**：
- `--verbose-version=TIMESTAMP`：只显示版本号 `>= TIMESTAMP` 的日志。

**修改的文件**：
- `include/core/config.h`：`VERSION "15.3.0"`、`VERSION_CODE`
- `include/util/log.h` / `src/util/log.c`：`_v` 宏、`g_log_version_threshold`
- `src/core/cmdline.c`：`--verbose-version` 参数解析
- `src/core/main.c`：threshold 初始化
- `src/ipc/ipc_worker_mgmt.c`、`src/ipc/ipc_message_handler.c`、`src/scan/main_loop.c`、`src/ipc/worker_proc.c`、`src/ipc/ipc_protocol.c`、`src/scan/batch_processor.c`、`src/scan/dispatch.c`：高频追踪日志降级
- `README.md`：`--verbose-version` 文档

---

## [15.2.0] - 2026-05-18

### 架构重构完成：模块化拆分（Phase 2 ~ Phase 8）

**分支**：`v15.2.0-modular-refactor`

**背景**：经过 8 个阶段的重构，将原本 24 个源文件的单体代码库拆分为 32 个模块，按职责边界重新组织。所有超过 500 行的源文件均已拆分完毕。

**拆分总览**：

| Phase | 原文件 | 拆分结果 | 职责 |
|-------|--------|----------|------|
| Phase 2 | 目录结构 | 模块化目录布局（core/ipc/scan/output/util） | — |
| Phase 3 | 多文件 | 风格标准化 + dispatch 辅助函数提取 | — |
| Phase 4 | `progress.c` (1790 行) | `progress.c` + `progress_io.c` + `progress_archive.c` | 进度核心 / 进度 IO / 进度归档 |
| Phase 5 | `worker_proc.c` (951 行) | `ipc_protocol.c` + `worker_scanner.c` + `worker_proc.c` | IPC 协议 / 扫描引擎 / 进程池管理 |
| Phase 6 | `main_loop.c` (823 行) | `batch_processor.c` + `dispatch.c` + `main_loop.c` | Batch 处理 / 任务分发 / 消息总线 |
| Phase 7 | `output.c` (713 行) | `output.c` + `output_metadata.c` + `output_format.c` | 输出渲染 / 元数据缓存 / 格式预编译 |
| Phase 8 | `ipc_thread.c` (550 行) | `ipc_thread.c` + `ipc_message_handler.c` + `ipc_worker_mgmt.c` | epoll 循环 / 消息处理 / Worker 生命周期 |

**新增核心模块职责**：
- `batch_processor.c`：Batch 解析、CPU 去重、完成处理
- `dispatch.c`：任务分发、Worker 清理、IPC send 辅助
- `ipc_protocol.c`：IPC TLV 协议封装（send/recv/drain）
- `worker_scanner.c`：Worker 扫描引擎（scan_and_send / blind_trust）
- `progress_io.c`：进度文件读写（save/load/shard）
- `progress_archive.c`：进度分片压缩归档（gzip/zlib）
- `output_metadata.c`：权限/xattr/用户名/组名缓存
- `output_format.c`：格式预编译与输出文件管理
- `ipc_message_handler.c`：IPC 消息接收与协议处理（HEARTBEAT/ERROR/FINISH/BATCH/CMD）
- `ipc_worker_mgmt.c`：Worker 生命周期管理（死亡标记/超时杀掉/返回消息）

**头文件调整**：
- `include/scan/main_loop.h`：新增 `batch_dedup_worker()`、`send_scan_to_ipc()`、`send_stop_to_ipc()`、`dispatch_lost_tasks()`、`drain_completed_batches()` 声明
- `include/output/output.h`：新增 `get_username()`、`get_groupname()`、`get_xattr_str()` 声明
- `include/ipc/ipc_thread.h`：新增 `worker_mark_dead()`、`worker_timeout_kill()`、`send_return()`、`read_ctrl_message()`、`read_data_message()`、`handle_cmd()` 声明

**接口调整（跨模块可见性）**：
- `batch_dedup_worker`：从 `static` 改为非 static，供 `main_loop.c` 线程池回调注册
- `send_scan_to_ipc` / `send_stop_to_ipc`：从 `main_loop.c` 内 static 改为 `dispatch.c` 导出
- `get_xattr_str`：从 `static` 改为模块间可见，供 `output.c` 调用
- `get_type_str` / `print_csv_field`：从 `output_metadata.c` 移入 `output.c`，保持 `static`
- `read_ctrl_message` / `read_data_message` / `handle_cmd`：从 `static` 改为模块间可见
- `worker_mark_dead` / `worker_timeout_kill` / `send_return`：从 `static` 改为模块间可见
- `safe_ipc_recv_header` / `safe_ipc_recv_payload`：保持 `static`，仅在 `ipc_message_handler.c` 内部使用

**编译**：`make clean && make` 零警告。源码总量约 8000 行（含注释）。

**Git 分支迁移**：
- 重构提交从 `dev` 分支迁移至独立分支 `v15.2.0-modular-refactor`
- `dev` 分支重置为 `origin/dev`（v15.1.5），不再包含重构提交
- 提交节点保持不变，仅迁移分支归属

---

## [15.1.6] - 2026-05-18

### 架构：Phase 6 — 拆分 main_loop.c（823 行 → 3 模块）

**背景**：`main_loop.c` 达 823 行，混合消息路由、Batch 处理、任务分发、Worker 清理、IPC 线程生命周期五种职责。

**拆分结果**：
- `src/scan/batch_processor.c` (~280 行)：Batch 解析 (`parse_batch`)、CPU 去重线程池回调 (`batch_dedup_worker`)、完成处理 (`process_completed_batch`、`drain_completed_batches`)
- `src/scan/dispatch.c` (~180 行)：任务分发 (`dispatch_lost_tasks`)、Worker 清理 (`cleanup_dead_worker_slot`)、IPC send 辅助 (`send_scan_to_ipc`、`send_replace_to_ipc`、`send_stop_to_ipc`)、空闲 Worker 查找 (`dispatch_find_idle_worker`)
- `src/scan/main_loop.c` (~350 行)：消息总线 (`handle_return_message`)、主循环框架 (`main_loop_run`)、IPC 线程生命周期 (`init_ipc_threads` / `destroy_ipc_threads` / `stop_all_ipc_threads`)

**头文件调整**：
- `include/scan/main_loop.h`：更新函数声明 — 添加 `send_scan_to_ipc`、`send_stop_to_ipc`、`dispatch_lost_tasks`、`drain_completed_batches`、`batch_dedup_worker`

**接口解耦**：`batch_dedup_worker` 从 `static` 改为模块内可见，供 `main_loop.c` 创建线程池时传入回调指针。

**编译**：`make clean && make` 零警告。

---

## [15.1.5] - 2026-05-17

### 修复
- `process_completed_batch` 中 Worker 调度循环 `attempts` 计数器未递增，导致所有 Worker 均为 BUSY 时进入无限 `continue` 死循环。
- `dispatch_lost_tasks` 中相同模式的 `attempts` 计数器未递增，一并修复。

### 背景
v15.1.4 `/public2` 测试运行 2 分 5 秒后卡死，`perf top` 仍显示 `process_completed_batch` 占 99.72% CPU。

症状：
- `pending_batches=4`，`pending_tasks=9`
- 8 个 Worker 全部为 BUSY
- `Dir rate / File rate / Dequeue = 0.00`

根因：v15.1.0 引入的 `while (attempts < num_workers)` 调度循环中，`attempts` 变量未在 `continue` 路径上递增。当所有 Worker 均为 BUSY（或 DEAD）时，条件永远为真，循环永不退出，CPU 100% 空转。Master 卡死后不处理 ret_queue，Worker FINISH 不被消费，Worker 状态永远 BUSY，形成完美死锁闭环。

### 修复内容
两处 `continue` 前增加 `attempts++`：
1. `process_completed_batch` 中的目录 dispatch 循环
2. `dispatch_lost_tasks` 中的 lost task dispatch 循环

---

## [15.1.4] - 2026-05-17

### 修复
- `main_loop_handle_batch`：增加 `parsed.count` sanity check（0 ~ 1,000,000），超限则 `log_fatal` 丢弃 batch，防止 corrupted IPC payload 创建超大规模 batch。
- `batch_dedup_worker`：入口处增加 `batch->count` sanity check，防止线程池 worker 处理 corrupted batch。
- `process_completed_batch`：入口处增加 `batch->count` sanity check（0 ~ 1,000,000），超限则安全释放并 `atomic_fetch_sub pending_batches`。
- `process_completed_batch`：循环体内增加 `PROC_ITER_LIMIT = 1,000,000` 硬上限，防止主线程在 batch count corruption 时无限 CPU 空转。

### 背景
v15.1.3 `/public2` 测试 Master PID 88601 运行 16+ 小时，`perf top` 显示 `process_completed_batch` 占 99.86% CPU。
症状：
- `pending_batches=3` 不再下降，`pending_tasks=196`
- 8 个 Worker 全部空闲（poll / epoll_wait / futex_wait）
- 无 `log_fatal` 输出，排除 `fp_shard_insert_internal` PROBE_LIMIT 路径
- Master stack 为 `running`（纯用户态死循环，非 v15.1.2 的 `do_wait`）

根因推断：主线程 `process_completed_batch` 中 `for (int i = 0; i < batch->count; i++)` 的 `batch->count` 被内存 corruption 覆盖为巨大正值（如 `INT_MAX`），导致 `int` 溢出后循环永不终止，或循环体内部 `continue` 路径过轻导致 CPU 空转 16+ 小时。

v15.1.4 通过多层防御（parse 时 → dedup worker 时 → process 时 → loop 内）阻断裂口。

---

## [15.1.3] - 2026-05-17

### 修复
- `fp_shard_insert_internal` 开放寻址探测增加 `PROBE_LIMIT = 1,000,000` 硬上限，防止 `shard->capacity` 或 `meta[pos]` 内存 corruption 导致的无限循环死锁
- `fp_shard_insert_internal` 增加 `shard->capacity` sanity check（>0、<=1<<30、必须为 2 的幂次），扩容时增加 `new_cap` 溢出检查
- 扩容失败时回滚旧 table，防止内存泄漏和后续 corruption

### 背景
v15.1.2 在 `/public2` 测试仍卡死，`pending_batches=5` 永远不归零，4 个 thread_pool 工作线程全部 `futex_wait`。根因推断：
- `batch_dedup_worker` 外层硬超时（10万次迭代）对 `fp_set_insert` 内部死锁无效，因为 `i` 不增加
- `fp_shard_insert_internal` 的开放寻址探测在 `shard->capacity` 或 `meta[pos]` corruption 时进入无限循环，持有 `shard->mutex` 永不释放
- 其他 thread_pool 工作线程在 `pthread_mutex_lock(&shard->mutex)` 中死锁

v15.1.3 通过内层循环硬限制和 capacity sanity check 阻断该路径。

---

## [15.1.2] - 2026-05-17

### 修复
- `batch_dedup_worker` 硬超时检测：外层循环超过 10 万次迭代强制中断，防止 thread_pool 工作线程 CPU 死循环
- `fp_shard_insert_internal` 开放寻址探测耗尽 capacity 时输出 `log_fatal`（理论上不应到达）
- 新增 `path_log_mask()`：每级目录保留最后一个字符，其余 `***` 替代，应用于 IPC 线程、主循环、Monitor 的所有路径日志输出

## [15.1.1] - 2026-05-17

### Fixed：补全 Worker 状态机实现（INITIALIZING + startup_timeout + RET_ERROR 恢复 + Monitor 状态显示）

**背景**：v15.1.0 引入 IDLE/BUSY/DEAD 三状态后，与设计文档对比发现 4 项缺失：INITIALIZING 状态、startup_timeout、RET_ERROR 后状态恢复、Monitor 独立 Worker 显示。

**补全内容**：
1. `WorkerSlot` 增加 `WORKER_STATE_INITIALIZING`（spawn 后未 READY 前）。IPC 线程通过 `spawn_time` 区分 startup_timeout（60s）与常规心跳超时（30s）。
2. `RET_ERROR`（设备超时/EIO）处理后置 `STATE_IDLE`，避免 Worker 永久冻结。
3. `RET_READY` 无条件置 `STATE_IDLE`，覆盖 INITIALIZING → IDLE 转换。
4. Monitor 面板增加 `[Worker States]` 区块，逐 slot 显示 INIT/IDLE/BUSY/DEAD + pid + current_path 截断。

**编译验证**：`make clean && make` 通过，零警告。

---

## [15.1.0] - 2026-05-17

### Fixed：Master 侧 Worker 显式状态机（IDLE/BUSY/DEAD），修复多线程 Worker 任务覆盖导致的全局停摆

**问题根因**：v14+ 引入 Worker 多线程（Scanner 子线程）后，Master 侧调度仍使用纯轮询（`next_dispatch_worker % num_workers`），仅以 `is_alive` 判定 Worker 可用性。一个已在处理任务的 Worker 会被再次选中，新任务通过 `fd_cmd` 管道塞入，Worker 主线程非阻塞读走并覆盖 `task_path`。Scanner 只处理最后一个任务，前面所有任务成为"幽灵任务"——Master 认为已下发（`pending_tasks` 已递增），但永远收不到 `FINISH`。

**根因闭环**：8 个 Worker 全部处于 BUSY（Scanner 阻塞在 `pthread_cond_wait`），Master 仍在发但无 IDLE Worker 可处理。主线程等不到 `FINISH`，`pending_tasks` 不归零，全局停摆。

**修复方案**：`WorkerSlot` 增加 `_Atomic int state` 字段，定义 `IDLE/BUSY/DEAD` 三个显式状态。
- 调度目录（`process_completed_batch`）和 `dispatch_lost_tasks` 只选择 `STATE_IDLE` 的 Worker，找到后原子置为 `STATE_BUSY` 再发 `CMD_SCAN`。
- 收到 `RET_FINISH` 后置 `STATE_IDLE`；收到 `RET_READY` 后置 `STATE_IDLE`；`cleanup`/`replace` 后置 `STATE_DEAD`。
- 若无 IDLE Worker，任务入 `lost_tasks` 等待下一轮。

**编译验证**：`make clean && make` 通过，无警告。

---

## [15.0.4] - 2026-05-17

### Debug：增加 IPC 链路追踪日志，定位 FINISH 消息丢失点

**问题背景**：v15.0.3 修复 Worker 侧阻塞写（`fd_data`/`fd_ctrl` 设为 `O_NONBLOCK` + FINISH 重试）后，扫描 `/public2` 仍卡死，`pending_tasks=477` 不归零，8 个 Worker 全部空闲（poll），无新 FINISH 到达主循环。

**当前状态**：根因尚未完全定位。477 个 FINISH 消息在 IPC 链路中的丢失点未知：可能发生在 Worker→IPC 线程（fd_ctrl）、IPC 线程→ret_queue、ret_queue→主线程中的任一环节。

**新增调试点**
- `src/ipc_thread.c`：`read_ctrl_message` 的 `IPC_MSG_FINISH` 分支由 `log_debug` 提升为 `log_info`，确保任何 verbose 级别下都能看到 IPC 线程是否收到 FINISH。
- `src/ipc_thread.c`：`send_return` 成功时增加 `log_info`，追踪 RET_FINISH 是否进入 ret_queue。
- `src/ipc_thread.c`：`handle_cmd` 的 `CMD_SCAN` 成功路径增加 `log_info`，确认 SCAN 是否被成功发给 Worker。
- `src/worker_proc.c`：FINISH 发送的 EAGAIN 重试超过 1000 次时打印 `log_warn`。

**目的**：通过下次 `/public2` 测试的日志，精确判断 FINISH 消息在以下哪一环断裂：
1. Worker 写入 fd_ctrl（无重试/重试耗尽）
2. IPC 线程从 fd_ctrl 读取（safe_ipc_recv_header 超时/失败）
3. IPC 线程 send_return 到 ret_queue（队列满/失败）
4. 主循环 msg_queue_recv 从 ret_queue 取出（head/tail 异常）
5. 主循环 handle_return_message 处理 RET_FINISH（未命中 case）

---

## [15.0.3] - 2026-05-17

### Bugfix：修复 Worker 侧 `fd_data`/`fd_ctrl` 阻塞写导致的卡死

**问题背景**：v15.0.2 修复计数器后，扫描 `/public2` 时 `pending_tasks=143` 仍卡住 3 分钟不下降，Worker 全部空闲（poll），无 FINISH 到达主循环。

**根因：阻塞写死锁**
- `worker_pool_spawn()` 中仅设置了 Master 读端（`data_pipe[0]` / `ctrl_pipe[0]`）和 Master 写端（`cmd_pipe[1]`）的 `O_NONBLOCK`。
- 但 Worker 写端 `data_pipe[1]`（fd_data）和 `ctrl_pipe[1]`（fd_ctrl）仍为**阻塞写**。
- 当 IPC 线程因 cmd_queue drain + `ipc_send` EAGAIN 重试而延迟 epoll_wait 时，fd_data / fd_ctrl 管道积满。
- Worker Scanner 线程写 fd_data 阻塞，无法发 FINISH；Worker 主线程 poll fd_cmd 等 SCAN，形成死锁。

**修复**
- `fork` 前将 `data_pipe[1]` 和 `ctrl_pipe[1]` 设为 `O_NONBLOCK`。
- Worker 侧 `ipc_send` 在 EAGAIN 时 `usleep(1000)` 重试，不再永久阻塞。

---

## [15.0.2] - 2026-05-16

### Bugfix：修复 `pending_tasks` 计数器逻辑混乱 + `fd_cmd_rd` 保存无效 fd

**问题背景**：v15.0.1 小目录测试正常，但 `/public2` 大目录测试 8 分钟后卡死，`pending_tasks=139` 永远不归零，Worker 全部空闲 poll。

**根因 A：`process_completed_batch` 对每个 BATCH 都减 `pending_tasks`**
- 单个目录扫描任务可能产生多个 BATCH，导致 `pending_tasks` 被多减。
- `dispatch_lost_tasks` 重发 SCAN 时**不增加** `pending_tasks`，这些路径完成后 `process_completed_batch` 又减一次，净效果 `pending_tasks` 失真。
- `RET_FINISH` 处理时**不减** `pending_tasks`，全靠 `cleanup_dead_worker_slot` 减。但正常 FINISH 后 Worker 不退出，cleanup 不调用，`pending_tasks` 永远无法归零。

**根因 B：`worker_pool_spawn` 中 `cmd_pipe[0]` 被提前关闭**
- `close(cmd_pipe[0])` 在 `slot->fd_cmd_rd = cmd_pipe[0]` 之前执行，保存的是已关闭的 fd 号。
- `cleanup_dead_worker_slot` 调用 `ipc_drain_and_count_tasks(slot->fd_cmd_rd)` 时，读的是无效 fd，`orphaned` 计数永远是错的。

#### 修复
- **`src/main_loop.c`**：
  - `process_completed_batch` 中移除 `atomic_fetch_sub(&ctx->pending_tasks, 1)`，只负责 `pending_batches`。
  - `dispatch_lost_tasks` 中 `send_scan_to_ipc` 成功后增加 `atomic_fetch_add(&ctx->pending_tasks, 1)`。
  - `handle_return_message` 的 `RET_FINISH` 分支增加 `atomic_fetch_sub(&ctx->pending_tasks, 1)`。
  - 主循环每 10s 打印 `pending_tasks` / `pending_batches` / `lost_tasks` 调试点。
- **`src/worker_proc.c`**：删除 `worker_pool_spawn` 中的 `close(cmd_pipe[0])`，保留读端给 `cleanup_dead_worker_slot` drain 用。
- **`src/progress.c`**：`pump_pbin_batch` 开头/末尾增加 DEBUG 日志。

#### 修改的文件
- `src/main_loop.c`（计数器修复 + 调试点）
- `src/worker_proc.c`（保留 `cmd_pipe[0]`）
- `src/progress.c`（调试点）
- `include/config.h`（版本号 15.0.2）

---

## [15.0.1] - 2026-05-16

### Bugfix：修复 `main_loop_run()` 重复调用 `init_ipc_threads()` 导致的卡死 + 配置初始化补全 + 线程安全日志

**问题背景**：v15.0.0 小目录测试时程序仍卡死，`timeout 5` 后 exit code = 124，`pending_tasks` 永远不归零。

**根因 A：`init_ipc_threads()` 被调用了两次**
- `main.c` 在 `main_loop_run()` 之前已调用 `init_ipc_threads()` 并发送初始 REPLACE。
- 但 `main_loop_run()` 内部又重复调用 `init_ipc_threads()`，生成第二套完全独立的 IPC 线程和消息队列。
- Worker 的 BATCH/FINISH 消息发到第一套 `ret_queue`，主循环 drain 的是第二套 `ret_queue`，消息永久丢失，`pending_tasks` 永远不减。
- 重复发送的 REPLACE 还导致 IPC 线程 `epoll_ctl ADD` 已关闭 fd，触发 `Bad file descriptor`，进而将所有 Worker 标记为 DEAD。

**根因 B：`skip_interval` 未显式初始化**
- `cmdline.c` 中 `init_config()` 遗漏 `skip_interval = 0`，依赖 `app_context_init()` 的 `memset` 间接置 0，存在未来重构风险。

**根因 C：多线程日志交错**
- `log.c` 中 `fprintf(stderr)` 无锁保护，IPC 线程与主循环并发写时日志行错乱，干扰排查。

#### 修复
- **`src/main_loop.c`**：删除 `main_loop_run()` 中的 `init_ipc_threads()` 调用和初始 REPLACE 循环（`main.c` 已负责）。
- **`src/cmdline.c`**：`init_config()` 中显式设置 `skip_interval = 0`。
- **`src/log.c`**：所有 `fprintf(stderr)` 调用包裹 `flockfile(stderr)` / `funlockfile(stderr)`，确保多线程日志原子输出。

#### 修改的文件
- `src/main_loop.c`（删除重复初始化）
- `src/cmdline.c`（补全 `skip_interval` 初始化）
- `src/log.c`（线程安全日志锁）
- `include/config.h`（版本号 15.0.1）

---

## [15.0.0] - 2026-05-16

### 架构重构：三通道分离 + IPC 状态机

**问题背景**：v14.0.x 中 Worker 拆分为 Scanner 线程 + IPC 线程后，两条线程并发写 `fd_out`，引入 `g_fd_out_mutex` 保护。但 mutex 持有者阻塞时（Scanner `write()` 被 pipe 满卡住），IPC 线程卡在 `pthread_mutex_lock` 上，心跳停止。同时多种语义消息（BATCH / HEARTBEAT / ERROR / EXIT / DEV_TIMEOUT）共享同一个 fd，字节交错导致 payload timeout 级联风暴。

**新架构**：每个 Worker 配置三个独立 fd，语义分离：
- `fd_cmd`：M→W 命令通道（SCAN / STOP）
- `fd_data`：W→M 数据通道（BATCH 大 payload），Scanner 线程独占
- `fd_ctrl`：W→M 控制通道（HEARTBEAT / ERROR / EXIT / DEV_TIMEOUT / READY / FINISH），IPC 线程独占

**关键约束**：
- `fd_data` 只有 Scanner 线程写，`fd_ctrl` 只有 IPC 线程写，永不竞争
- IPC 线程 epoll 监听 `fd_data + fd_ctrl + cmd_queue eventfd`
- `fd_ctrl` 消息长度均 < PIPE_BUF（4096），内核保证原子写入

**新增 IPC 消息**：
- `IPC_MSG_READY`（8）：Worker 初始化完成，进入主循环
- `IPC_MSG_FINISH`（9）：Scanner 任务完成

**Master 侧 Worker 状态机**：
- `INITIALIZING` → `IDLE` → `BUSY` → `IDLE` / `DEAD`
- 启动超时（60s）、心跳超时（30s）、任务超时（-t 参数）分离
- `RET_READY` 收到前不检测心跳超时
- `RET_FINISH` 收到后 Worker 回到 IDLE

**修改的文件**：
- `include/config.h` — 版本号 15.0.0
- `include/ipc_protocol.h` — IPC_MSG_READY / IPC_MSG_FINISH
- `include/msg_format.h` — RET_READY / RET_FINISH / CmdReplacePayload 三通道
- `include/worker_proc.h` — WorkerSlot 三通道结构
- `include/ipc_thread.h` — IpcThreadCtx 三通道
- `include/main_loop.h` — send_replace_to_ipc 签名
- `src/worker_proc.c` — spawn/replace/destroy/stop_all + worker_main 三通道化
- `src/ipc_thread.c` — epoll 三通道监听 + read_data_message + read_ctrl_message
- `src/main_loop.c` — handle_return_message READY/FINISH 处理 + 所有 fd 引用更新
- `src/main.c` — send_replace_to_ipc 调用更新

---

## [14.0.1] - 2026-05-16

### Bugfix：修复 v14.0.0 Worker 多线程 fd_out 竞争写入导致的 payload timeout 回归

**问题背景**：v14.0.0 重构 Worker 为多线程（Scanner 线程 + IPC 线程）后，启动即出现大面积 `[IPC-X] payload timeout`，且 4-5 秒后所有 Worker 同时 timeout，循环往复。主进程卡在 `futex_wait`。

**根因**：Scanner 线程与 IPC 线程**同时往 `fd_out` 写数据，没有互斥**。
- Scanner 线程：`send_batch()` → `ipc_send(fd_out, IPC_MSG_BATCH, ...)`
- IPC 线程：心跳 `ipc_send(fd_out, IPC_MSG_HEARTBEAT, ...)`
- 两条线程并发 `write(fd_out)`，内核调度下消息字节交错：
  - IPC 线程的 HEARTBEAT header（8 bytes）插入到 Scanner 线程的 BATCH header 和 BATCH payload 之间
  - Master IPC 线程读到一个 BATCH header（payload_len=1197），但 pipe 里接下来只有 16 字节的 HEARTBEAT 数据，不够 1197
  - `safe_ipc_recv_payload(100ms)` 超时 → `payload timeout`
- 残留的 BATCH payload 字节留在 pipe 中，下一轮被当成新的 Header 解析 → garbage header → 连续 timeout
- 8 个 Worker 同时触发，形成级联 payload timeout 风暴

**修复**：
- 新增 `static pthread_mutex_t g_fd_out_mutex = PTHREAD_MUTEX_INITIALIZER`
- 所有往 `fd_out` 的 `ipc_send` 调用加锁保护：
  - `send_batch()`
  - `send_error_and_empty_batch()`
  - IPC 线程的心跳发送
  - `IPC_MSG_DEV_TIMEOUT` 上报
  - `IPC_MSG_EXIT` 发送
- 确保同一时刻只有一个线程向 `fd_out` 写数据，协议不再错乱。

#### 修改的文件

- `src/worker_proc.c`（g_fd_out_mutex + 所有 fd_out 写入点加锁）
- `include/config.h`（版本号 14.0.1）

---

## [14.0.0] - 2026-05-16

### 架构重构：Worker 多线程化（IPC 线程 + Scanner 线程分离）

**问题背景**：v13.x 中 Worker 是单线程线性设计：阻塞读 fd_in → 阻塞扫描（readdir/lstat 可能卡住很久）→ 发 BATCH → 循环。扫描期间既不响应 STOP、也不发心跳，NFS 卡死时只能等 IPC 线程心跳超时后 SIGKILL。

**新架构**：Worker 进程内部拆分为两条线程：
- **Scanner 线程**：专职执行 readdir/lstat 等阻塞 IO，调用 `scan_and_send()` 直接写 fd_out（阻塞）。
- **IPC 线程（主线程）**：专职维护与 Master 的通信。fd_in 设为非阻塞，通过 `poll(5s)` 循环同时处理读任务、发心跳、响应 STOP。Scanner 卡住不影响心跳节拍。

**好处**：
1. 心跳连续性：即使 Scanner 卡在某个 lstat 上 30 分钟，IPC 线程每 5s 仍发心跳，IPC 线程不会误判 Worker 死亡。
2. 可响应 STOP：IPC 线程收到 IPC_MSG_STOP 后立即设置 stop_flag、唤醒 Scanner、等待其退出后发送 EXIT。
3. **Scanner 超时检测**：IPC 线程监控 Scanner 的 `last_progress` 时间戳，若超过 `heartbeat_timeout`（默认 30s，`-t` 参数可调）无进展，则发送 **`IPC_MSG_DEV_TIMEOUT`** 上报 Master。Master 收到 **`RET_DEV_TIMEOUT`** 后直接按超时逻辑处置：`cleanup_dead_worker_slot(..., true)`（SIGKILL 路径重发）。Worker 无需停止心跳，心跳继续直到 Master 完成替换。
4. **双层超时架构**：Master 侧心跳超时兜底 + Worker 侧 Scanner 进度超时自报（`DEV_TIMEOUT` 协议）。

#### 修改的文件

- `src/worker_proc.c`（WorkerThreadCtx + worker_scanner_thread + worker_main 多线程重写 + Scanner 超时检测）
- `src/ipc_thread.c`（IPC_MSG_DEV_TIMEOUT 转发为 RET_DEV_TIMEOUT）
- `src/main_loop.c`（RET_DEV_TIMEOUT 超时逻辑处置）
- `include/ipc_protocol.h`（IPC_MSG_DEV_TIMEOUT 定义）
- `include/msg_format.h`（RET_DEV_TIMEOUT 定义）
- `include/config.h`（版本号 14.0.0）

---

## [13.0.3] - 2026-05-16

### Bugfix：修复 Worker 写端非阻塞导致的 payload timeout 竞态

**问题背景**：v13.0.2 运行时启动即出现 `[IPC-0] payload timeout (len=1197)`，IPC 线程读到 Header 后 100ms 内等不齐 payload。

**根因**：`worker_pool_spawn()` 中 `out_pipe`（Worker → Master）被 `pipe2(O_NONBLOCK)` 设为非阻塞。Worker 写 BATCH 时，`write()` 可能部分写入后遇 `EAGAIN`；v13.0.2 的 `ipc_send()` retry 逻辑（`usleep(1ms)`）让 Worker 反复尝试。但 IPC 线程通过 epoll 已检测到 `fd_out` 可读，读取 Header 后 `poll_payload(100ms)` 等待剩余 payload，形成竞态——Worker 还没写完，IPC 线程已超时。

**修复**：
- `worker_pool_spawn()`：`out_pipe` 创建时不再带 `O_NONBLOCK`。
- Worker 写端 `out_pipe[1]` 恢复阻塞模式，`ipc_send()` 的 `write()` 会一次性写完或阻塞到写完，不会出现 Header 到了 payload 没到的情况。
- Master 读端 `out_pipe[0]` 显式加 `O_NONBLOCK`，保持 IPC 线程 epoll 响应性。

#### 修改的文件

- `src/worker_proc.c`（`worker_pool_spawn` out_pipe 阻塞化 + 读端单独加 O_NONBLOCK）
- `include/config.h`（版本号 13.0.3）

---

## [13.0.2] - 2026-05-16

### Bugfix：修复 double free / fd 生命周期混乱 + 重复 RET_DEAD + ipc_send 部分写入协议不同步

**问题背景**：v13.0.1 运行时出现 `double free or corruption (fasttop)` 崩溃，发生在所有 8 个 Worker 同时 heartbeat timeout 被 SIGKILL 之后、主线程继续输出切片切换阶段。

**根因 A：fd_in double close**
- `worker_mark_dead()`（IPC 线程）与 `cleanup_dead_worker_slot()`（主线程）各自 `close(slot->fd_in)`。
- `send_replace_to_ipc()` 将 `slot->fd_in` 直接赋值给 `ctx->fd_in`，二者**共享同一个内核 fd 号**，没有 `dup`。
- IPC 线程先 close → fd 号释放 → async_writer 打开新切片文件时复用该 fd 号 → 主线程再次 close → 意外关闭 async_writer 的合法文件描述符 → `FILE*` 内部缓冲区指针被 double free。

**根因 B：延迟/重复 RET_DEAD 清理新 Worker**
- `worker_mark_dead()` 未将 `ctx->pid` 设为 `-1`，导致 heartbeat timeout check 可能再次触发，发送第二个 RET_DEAD。
- 主线程收到第一个 RET_DEAD 后 spawn 新 Worker、重置 `cleanup_done`、发送 REPLACE。
- 第二个延迟的 RET_DEAD 到达时，`cleanup_dead_worker_slot()` 因 `cleanup_done` 已重置，错误地再次清理新 Worker，关闭其 fd、释放其 backlog、扣减 pending_tasks。

**根因 C：ipc_send 部分写入后 EAGAIN 导致协议不同步**
- v13.0.1 将 `ipc_send()` 改为单次 `write()`，但对大于 `PIPE_BUF` 的 BATCH 消息，`write()` 可能部分写入后下一次返回 `EAGAIN`。
- 此时函数返回 `-2`，但 pipe 中已残留不完整数据，再次调用时追加新消息，IPC 线程解析错乱。

#### 修复

- **`ipc_thread.c: worker_mark_dead()`**：
  - 增加 `ctx->pid = -1;`，彻底阻止重复 heartbeat timeout 和重复 RET_DEAD。
- **`main_loop.c: cleanup_dead_worker_slot()`**：
  - 不再 `close(slot->fd_in)`，只设为 `-1`（IPC 线程已负责关闭）。
- **`main_loop.c: handle_return_message()` (RET_DEAD)**：
  - 增加 guard：`if (is_alive && pid != -1) break;`，忽略 REPLACE 之后才到达的延迟 RET_DEAD。
- **`worker_proc.c: ipc_send()`**：
  - 如果 `written > 0` 时遇到 `EAGAIN`，不再返回 `-2`，而是 `usleep(1ms)` 后继续重试，避免 pipe 中残留不完整数据。

#### 修改的文件

- `src/ipc_thread.c`（`worker_mark_dead` 设 pid=-1）
- `src/main_loop.c`（`cleanup_dead_worker_slot` 不再 close fd_in + RET_DEAD guard）
- `src/worker_proc.c`（`ipc_send` 部分写入 retry）
- `include/config.h`（版本号 13.0.2）

---

## [13.0.1] - 2026-05-15

### Bugfix：IPC 协议失步 + 栈值污染/历史块数异常 + archive 无限循环防呆

**问题背景**：v13.0.0 重构后的首次运行时出现两个独立阻断性问题：
1. `[IPC-0] payload timeout (len=42)` — 启动即死，IPC 线程在 fd_out 上读到孤悬 Header 后 poll payload 超时。
2. `无索引文件，历史块数 140734670153448` — `total_blocks` 打印出异常大值（`0x7FFF5805A6E8`，x86_64 栈地址特征），导致恢复逻辑走入错误分支。

**根因 A：IPC 协议 Header/Payload 断裂**
- `ipc_send()` 分两次 `write()`（先 8 字节 Header，再 payload）。
- 第二次 `write()` 遇 `EAGAIN` 时，第一次的 Header 已留在 pipe 中。函数返回 `-2`，调用方重试时重新发送 Header + payload。
- 结果：pipe 中出现多个孤悬 Header。IPC 线程读到第一个 Header 后等待 payload，永远等不到。

**根因 B：`total_blocks` 栈值污染 / `va_list` 传递异常**
- `count_archive_blocks()` 与 `count_pbin_slices()` 均显式初始化为 0，返回值理论上为 0。
- `0x7FFF5805A6E8` 是 x86_64 下未初始化栈内存 / `va_list` 因编译器优化/ABI 边界传递异常时读取到的栈残留值。
- 非野指针，是栈值污染。`verbose_printf` → `log_vraw` → `vfprintf` 的 `va_list` 跨编译单元传递在特定栈布局下可能读取错误位置。

**根因 C：`compressed_size == 0` 未防呆**
- `count_archive_blocks()` 中若 `.archive` 文件损坏且 `bh.compressed_size == 0`，`fseek` 不移动，`fread` 原地重复读取同一 Header，`count` 无限增加。
- 虽然单秒内不可能达到 `140734670153448`，但作为防御性修复必须堵住。

#### 修复

- **`ipc_send()` 合并为单次原子写入**：
  - 分配 `sizeof(IpcMessageHeader) + payload_len` 连续缓冲区，一次 `write()` 发送全部数据。
  - 对 SCAN 等小消息（<< `PIPE_BUF=4096`）天然原子，彻底消除 Header 孤悬问题。
- **`count_archive_blocks()` 增加 `compressed_size == 0` 检查**：
  - `bh.compressed_size == 0 || bh.compressed_size > 512MB` 时 `break`，防止无限循环。
- **`restore_progress()` 增加 `total_blocks` sanity check**：
  - 若 `total_blocks > 1000000000UL`，打印警告并强制置为 0，防止异常值触发错误的全量重扫分支。
- **`verbose_printf` / `log_vraw` 添加 `__attribute__((noinline))`**：
  - 防止编译器内联导致 `va_start` / `va_copy` 的寄存器保存区布局异常，确保 `va_list` 跨编译单元传递的 ABI 稳定性。

#### 修改的文件

- `src/worker_proc.c`（`ipc_send()` 合并写入）
- `src/progress.c`（`count_archive_blocks` 防呆 + `total_blocks` sanity check）
- `src/utils.c`（`verbose_printf` noinline）
- `src/log.c`（`log_vraw` noinline）
- `include/config.h`（版本号 13.0.1）

---

## [13.0.0] - 2026-05-15

### 架构重构：IPC 线程隔离（v13.0.0）

**问题背景**：v12.x 架构中，Master 主线程通过单线程 epoll 统一监听所有 8 个 Worker 的 fd_out。一个 Worker 的 fd 出问题（heartbeat 超时、D-State 卡死、fd 号重用竞争）会导致 epoll 反复返回 EPOLLERR|EPOLLHUP，污染整个 Master 事件循环，造成 Monitor 秒表冻结、CPU 空转、所有 Worker 假死。v12.2.15 修复了 cleanup_done 和日志问题，但单线程 epoll 的架构瓶颈仍然存在。

**新架构核心**：
- **8 个 IPC 线程常驻**，生命周期与 Master 进程相同。每个 IPC 线程管理一个 Worker 的非阻塞 epoll + 心跳检测 + SIGKILL。
- **主线程 = 纯消息总线**，不再直接操作任何 fd、不再 epoll、不再 read/write。只负责：收消息（从 8 个返回队列轮询）、处理消息（BATCH 去重写文件、DEAD 收尾替换、ERROR 记日志）、发消息（SCAN 任务分发给 IPC 线程）。
- **故障隔离**：一个 Worker 的 fd 出问题 → 只污染它自己的 IPC 线程 → IPC 线程发 DEAD 消息 → 主线程收到后优雅替换 Worker → 其他 7 路完全不受影响。

#### 消息队列

- **eventfd + 无锁环形队列**（64 位原子 CAS head/tail）。
- 默认容量 1024 条消息/队列，有界设计天然实现背压。
- 生产者（主线程/IPC 线程）：CAS 写尾指针 → 写入数据 → 写 eventfd 通知。
- 消费者：读 eventfd → CAS 读头指针 → 读取数据。
- 零 mutex、零上下文切换开销、支持 64 位原子操作。

#### 消息格式

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

#### IPC 线程内部循环

```
while (running) {
    // 1. 从主线程消息队列取命令（非阻塞 drain）
    // 2. epoll_wait(fd_out + cmd_queue eventfd, 500ms)
    // 3. 处理 fd_out 事件（非阻塞 read → 解析 BATCH/HEARTBEAT/ERROR/EXIT）
    // 4. 心跳检测：超时 → SIGKILL Worker → 发 RET_DEAD → 等待 REPLACE
}
```

- 每个 IPC 线程有自己的小 epoll（2 个 fd：fd_out + cmd_queue eventfd）。
- fd 均为 O_NONBLOCK，read 采用 poll(100ms) 超时保护。
- Worker 死亡后 IPC 线程自己 close fd、epoll DEL，不需要主线程介入 cleanup。

#### 主线程消息总线循环

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

#### Monitor 线程精简

- **删除 `check_workers_health`**：Worker 心跳超时检测完全下沉到 IPC 线程。
- **保留职责**：进度面板输出（秒表、速率、Worker 状态）、敢死队探测调度、探测进程收割。
- Monitor 不再直接操作 Worker fd 或发送 SIGKILL。

#### 新增文件

- `include/msg_format.h` — 消息格式定义（CMD/RET 类型、Payload 结构体）。
- `include/msg_queue.h` — 无锁环形队列 API（eventfd 通知、CAS head/tail）。
- `src/msg_queue.c` — 无锁队列实现（64 位原子操作、select-based recv_wait）。
- `include/ipc_thread.h` — IPC 线程上下文和生命周期 API。
- `src/ipc_thread.c` — IPC 线程主循环（独立 epoll、心跳检测、消息处理）。

#### 修改的文件

- `include/app_context.h` — 添加 IPC 线程数组、消息队列指针、主线程 cond/mutex。
- `include/config.h` — `VERSION "13.0.0"`。
- `include/main_loop.h` — 暴露 IPC 线程生命周期 API（init_ipc_threads、destroy_ipc_threads、send_replace_to_ipc）。
- `src/main_loop.c` — **重写**：从单线程 epoll 驱动改为消息总线驱动。保留 BATCH 去重、lost_tasks 派发、Worker 替换、设备熔断、历史 pbin 泵送等所有现有功能。
- `src/main.c` — 初始化 IPC 线程、发送初始 REPLACE、根任务改为通过 cmd_queue 发送 CMD_SCAN。
- `src/monitor.c` — 删除 Worker 心跳超时检测，保留进度面板和敢死队探测。
- `Makefile` — 自动包含新源文件（msg_queue.c、ipc_thread.c）。

---

### 紧急修复：v12.2.15 cleanup_done 未重置 + 日志系统 + dispatch 轮询缺陷（P0 阻断性）

**问题背景**：v12.2.14 修复了数据竞争，但运行在生产环境（/public2，300万+文件）时仍然出现 Monitor 秒表冻结、所有 Worker 阻塞在 `pipe_wait`、主进程 throughput 归零的现象。`errlogs.err` 中 `[Epoll] Worker 1 fd error/hup` 重复 54万+次，`active=8->7` 始终不变。

**根因 A：cleanup_done 未重置（核心）**
- 当 Worker 超时死亡后，`cleanup_dead_worker_slot()` 通过 `atomic_flag_test_and_set()` 设置 `cleanup_done = true`。
- 随后 `worker_pool_replace()` → `worker_pool_spawn()` 创建新 Worker，但**未重置 `cleanup_done`**。
- 新 Worker 若再次死亡（或 epoll 报告残留事件），cleanup 因 `cleanup_done = true` 直接 return，不关闭 fd、不从 epoll 移除。
- fd 持续打开 → epoll 每次 `epoll_wait` 都返回 `EPOLLERR|EPOLLHUP` → 54万+次重复日志，CPU 空转。

**根因 B：dispatch_lost_tasks 轮询缺陷**
- `dispatch_lost_tasks()` 中 `ipc_send()` 返回 `EAGAIN(-2)` 时直接 `break`，仅尝试了一个 Worker 就放弃。
- 若轮询恰好选到 fd_in 满的 Worker，所有 `lost_tasks` 都发不出去，其他空闲 Worker 永远收不到任务。

**根因 C：日志无时间戳，无法定位故障时刻**
- 大量 `fprintf(stderr, ...)` 分散在各文件中，格式不统一，无时间戳，无法从日志推断卡死发生时刻。
- 调试日志与错误日志混为一谈，verbose 开关无法精细控制。

#### 修复

- **worker_pool_spawn 重置 cleanup_done**：`atomic_flag_clear(&slot->cleanup_done)`，确保新 Worker 可被正常清理。
- **dispatch_lost_tasks 不 break**：`EAGAIN` 时 `push back + continue`，轮询尝试下一个 Worker，直到所有 Worker 都试过或成功发送。
- **统一日志模块 `log.c / log.h`**：
  - 所有 `fprintf(stderr, ...)` 替换为 `log_fatal / log_error / log_warn / log_info / log_debug / log_trace`。
  - 自动添加 `[YYYY-MM-DD HH:MM:SS] [LEVEL]` 前缀。
  - 日志固定输出到 stderr，不污染 stdout。
  - `verbose` 和 `verbose_level` 控制 DEBUG/TRACE 级别日志的显示。
- **保留 FATAL/ERROR/WARN 始终输出**：不受 verbose 开关影响，确保错误信息始终可见。

#### 新增文件

- `src/log.c`
- `include/log.h`

#### 修改的文件

- `src/worker_proc.c`（cleanup_done 重置 + 日志替换 + log.h 包含）
- `src/main_loop.c`（dispatch break 修复 + 全部日志替换 + log.h 包含）
- `src/monitor.c`（日志替换 + log.h 包含）
- `src/main.c`（日志替换 + log_init 调用 + log.h 包含）
- `src/cmdline.c`（日志替换 + log.h 包含）
- `src/output.c`（日志替换 + log.h 包含）
- `src/progress.c`（日志替换 + log.h 包含）
- `src/device_manager.c`（日志替换 + log.h 包含）
- `src/thread_pool.c`（日志替换 + log.h 包含）
- `src/lost_tasks.c`（日志替换 + log.h 包含）
- `src/utils.c`（日志替换 + log.h 包含）
- `include/config.h`（版本号 12.2.15）

---

### 紧急修复：v12.2.14 LostTasksQueue 封装 + 原子化 is_alive/last_heartbeat

**根因**：v12.2.13 的 scattered `lost_tasks[]/count/capacity/mutex` 方案，锁操作分散在调用方，容易遗漏。

#### 修复

- `lost_tasks` 封装为独立 `LostTasksQueue` 模块，所有锁操作隐藏在 `lost_tasks.c` 内部。
- `is_alive` → `_Atomic bool`，`last_heartbeat` → `_Atomic time_t`。
- `cleanup_dead_worker_slot` 新增 `redispatch_current` 参数，Worker 超时死亡时自动将 `current_path` 压入 `lost_tasks`。

#### 修改的文件

- `src/lost_tasks.c` / `include/lost_tasks.h`
- `src/main_loop.c`
- `src/worker_proc.c` / `include/worker_proc.h`
- `include/app_context.h`

---

### 紧急修复：v12.2.13 active_count / lost_tasks 数据竞争（P0 阻断性）

**问题背景**：Monitor 线程与 Main 线程并发访问 `active_count`（plain `int`）和 `lost_tasks[]`（无锁数组），导致数据竞争。在长期运行后可能引发计数器漂移、任务丢失或重复释放。

#### 修复

- `active_count` → `_Atomic int`，所有读写点使用 `atomic_load`/`atomic_fetch_add`/`atomic_fetch_sub`/`atomic_store`。
- `lost_tasks` 增加 `pthread_mutex_t` 保护，Monitor 写和 Main 读全部被 mutex 包围。
- `pending_tasks` 幽灵化泄漏：3 处 `atomic_fetch_sub` 修复（ipc_send 返回 -1 时）。

#### 修改的文件

- `src/worker_proc.c` / `include/worker_proc.h`
- `src/main_loop.c`
- `include/app_context.h`
- `src/main.c`

---

### 紧急修复：poll 超时替代阻塞 read，彻底消除 Master hang 死（P0 阻断性）

**问题背景**：v12.2.9 恢复 `fd_out` 的 `O_NONBLOCK` 后，strace 确认标志已生效，但 Master 仍然阻塞在 `pipe_read`。根本原因是：即使 `O_NONBLOCK` 设置正确，当 Worker 写了一半 header 后死亡（或处于特定竞态），Master 的 `ipc_recv_header()` 循环会在 `read()` 上等待剩余字节，导致 epoll 循环停滞，所有 Worker 的 `pipe_write` 因缓冲区满而阻塞。

**v12.2.10 修复**（poll 超时）：
- 新增 `safe_ipc_recv_header()` 和 `safe_ipc_recv_payload()`：在 `read()` 前先执行 `poll(fd, POLLIN, 100ms)` 超时检查。
- 超时返回 `-2`，主循环 `continue` 安全跳过，不阻塞、不触发 cleanup，等待下一轮 epoll。
- `payload` 接收也带 poll 超时，防止部分数据到达后永远等待剩余字节。
- 保留 v12.2.9 的所有修复（`O_NONBLOCK`、`EPOLLERR|EPOLLHUP`、`EAGAIN` 处理、`pending_tasks` 泄漏修复），poll 超时作为第二道防线。

**v12.2.11 修复**（O_NONBLOCK 原生创建）：
- `pipe2(out_pipe, O_CLOEXEC | O_NONBLOCK)`：out_pipe 在创建时就自带 `O_NONBLOCK`，读写两端出生即非阻塞，彻底消除 `fcntl(F_SETFL)` 可能失败或不可靠的问题。
- 移除 `worker_pool_spawn()` 中对 `out_pipe[0]` 的冗余 `fcntl` 调用。
- `fcntl(in_pipe[1])` 增加错误检查，失败时输出警告日志。

#### 修改的文件

- `src/main_loop.c`
- `src/worker_proc.c`
- `include/worker_proc.h`
- `include/main_loop.h`
- `include/config.h`

---

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
3. 监控面板功能完全丢失。

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
- `src/progress.c'
- `src/output.c`
- `include/app_context.h`
- `include/config.h`
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
