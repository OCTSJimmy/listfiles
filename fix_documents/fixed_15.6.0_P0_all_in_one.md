# Fixed Document: v15.6.0 — P0 全量闭环（12 项 P0 + 4 个实现期缺陷）

> 日期：2026-08-20
> 设计依据：`Design-todo-v15.6.0.md`（12 项 P0 设计 v1 全部确认）
> 验证：`tests/run_regression.sh` 用例 1-13 全部 PASS（含新增用例 10-13）

## 问题概述

v15.5.x 系列在生产实测与 v15.5.9 外部设计评审中暴露出一组相互纠缠的结构性缺陷：FINISH/BATCH 竞态、输出无状态、fpbin 恢复路径不支持二次崩溃、visited_set 背压竞态、spbin 游离于恢复路径之外、MSG_DROP 正常路径丢任务、RET_ERROR 状态机残缺、进度状态无权威来源（.config 追加写多行 status 被 strstr 误判）、盲信扫描前提条件不受工具强制、旧 Worker 残留消息污染新账目。这些缺陷单点修复无法闭环，v15.6.0 将评审确认的 12 项 P0 一次性编码落地。

修复过程中另实测发现并修复 4 个实现期缺陷（见文末"实现期缺陷实录"）。

---

## P0-001 FINISH/BATCH 竞态 — 目录任务完成屏障

- **问题**：Worker 的 FINISH 与 BATCH 消息经不同通道到达，Master 在 FINISH 到达时即将目录记账完成，尚未处理的 BATCH（该目录的文件条目）无家可归，条目丢失。
- **根因**：目录"完成"没有屏障语义——FINISH 只代表"readdir 结束"，不代表"全部产出已入账"。
- **修复**：目录任务完成屏障：FINISH 到达后 slot 进入 DT_BATCHES_RECEIVED，仅当 batches_processed == batches_received 且该 slot 未 COMMITTED 输出批次计数（`output_pending[slot]`）归零时才推进到完成态并记账。屏障推进仅主线程执行（`advance_task_barriers`）。
- **关键文件**：`src/scan/main_loop.c`、`src/scan/batch_processor.c`、`src/scan/dispatch.c`、`src/scan/thread_pool.c`、`include/ipc/worker_proc.h`
- **验证**：回归用例 1/2/10（基线 diff=0、并发一致性、崩溃-续传一致）。

## P0-002 输出三态状态机

- **问题**：输出条目无状态追踪，Worker 死亡时已发出但未落盘的批次既可能重复（重扫重发）也可能丢失（按已发出记账）。
- **根因**：输出生死依赖"批次是否已提交异步写盘"这一事实，但没有任何账目记录它。
- **修复**：输出三态 DISCOVERED → OUTPUT_QUEUED → OUTPUT_COMMITTED；每 slot 维护未 COMMITTED 批次计数 `output_pending`（按 worker 数动态分配）；Worker 死亡/任务重扫时未 COMMITTED 部分随目录回滚重扫，COMMITTED 部分以 at-least-once 语义截断损坏尾部后续传（设计 §0.5）。
- **关键文件**：`src/output/async_worker.c`、`src/output/output_format.c`、`src/scan/batch_processor.c`、`src/scan/main_loop.c`、`src/core/main.c`、`include/core/app_context.h`、`include/output/async_worker.h`
- **验证**：回归用例 10/11（KILL 后续传 sort -u == 基线，输出不多不少）。

## P0-003 fpbin 二次崩溃恢复路径

- **问题**：恢复过程本身被 KILL（二次崩溃）时，fpbin 里隔离的新发现子目录与已完成目录的对账状态损坏，再次恢复漏扫或重扫混乱。
- **根因**：fpbin 只有"新发现"一半账目，缺少与之配对的"恢复期间完成"日志，无法构成可重入的恢复工作区。
- **修复**：fpbin/dfpbin 原子对：恢复期间新发现子目录写 fpbin、完成目录写 dfpbin；恢复闭环时 dfpbin 合并入 dpbin（`merge_dfpbin_into_dpbin`：封口 → rename → 重置写状态）；fpbin.idx + 分片残留纳入 baseline_eligible 判定。任意一次崩溃后，原子对都能再次进入恢复并收敛。
- **关键文件**：`src/output/progress.c`、`src/output/progress_archive.c`、`src/output/progress_io.c`、`src/scan/batch_processor.c`、`src/scan/main_loop.c`、`include/output/progress.h`
- **验证**：回归用例 11（KILL + KILL + 续传三轮，sort -u == 基线）。

## P0-004 visited_set 背压竞态 — 统一队列模型

- **问题**：单一 visited_set 同时承担"已发现去重"与"已入队去重"，HIGH_WATER 背压跳推/回填时两种语义相互踩踏，目录被误判已处理而漏扫。
- **根因**：一个集合无法表达"发现/入队/完成"三个不同生命周期阶段。
- **修复**：三集合拆分——discovered_set（严格只含目录，设计 §0.1）、enqueued_set（入队去重）、completed_set（完成记账）；所有待扫目录统一经 `enqueue_dir` 入口（completed 差集剪枝 → enqueued 去重 → 队列优先、dspill 兜底）；恢复泵送、spbin 重入队、dspill 回填全部走同一入口。
- **关键文件**：`src/scan/dispatch.c`（`enqueue_dir`）、`src/scan/batch_processor.c`、`src/scan/main_loop.c`、`src/output/progress_archive.c`、`src/core/main.c`、`include/scan/main_loop.h`
- **验证**：回归用例 1/7（基线 diff=0、dspill 溢出回填完整）。

## P0-005 spbin 纳入恢复路径（含 P1-004 毒丸隔离）

- **问题**：spbin（跳过记录）只写不读——崩溃后跳过目录永久丢失；且毒丸目录（反复崩溃的目录）阻塞同设备其他目录的恢复。
- **根因**：spbin 无原因码，恢复时无法区分"可重试"与"应跳过"；无 compaction，已恢复条目永远占位。
- **修复**：spbin 记录携带五类原因码（`include/output/spbin.h`），恢复时按原因码走重试/跳过/设备等待路径（`spbin_requeue_recovered` 经 enqueue_dir 重入队，绕过集合去重直写 pbin 的出错目录除外）；正常退出时 spbin compaction（过滤 RECOVERED 重写）；毒丸目录隔离到独立账目，不阻塞同设备其余目录（P1-004）。
- **关键文件**：`src/output/progress.c`、`src/output/progress_archive.c`、`src/output/progress_io.c`、`src/output/monitor.c`、`src/scan/dispatch.c`、`src/scan/main_loop.c`、`include/output/spbin.h`
- **验证**：回归用例 13（EACCES → spbin_count>=1 且其余目录不受影响）。

## P0-006 MSG_DROP 正常路径禁止

- **问题**：IPC 命令队列满时 Worker 拒收任务回送 MSG_DROP，Master 重入队但不销账，pending_tasks 永久泄漏（v15.5.8 R3 完结面板挂起非零的直接原因）。
- **根因**：队列容量不足 + 正常路径允许丢弃语义。
- **修复**：IPC 命令队列扩容至 65536；正常路径废除 MSG_DROP——队列满时阻塞等待背压，不再丢弃重派。
- **关键文件**：`include/core/config.h`（队列容量）、`src/scan/dispatch.c`、`src/ipc/*`
- **验证**：回归用例 2（workers 1/8/16 一致性，高并发下无丢任务）。

## P0-007 RET_ERROR 状态机补全

- **问题**：出错目录被简单重入队，反复失败反复派发，设备故障期间 CPU 空转且账目漂移。
- **根因**：RET_ERROR 后目录状态无归属——既不在"完成"也不在"等待"。
- **修复**：出错目录登记 enqueued_set 后不再重入队，等待设备恢复信号统一经 spbin 恢复路径重试；`record_path` 对运行期出错目录绕过 enqueue_dir 集合去重直写 pbin（保证 pbin 有发现记录，恢复闭环可达）。
- **关键文件**：`src/scan/main_loop.c`、`src/scan/batch_processor.c`、`src/scan/worker_scanner.c`、`src/output/progress.c`
- **验证**：回归用例 3/4/13（EACCES 注入：exit=1、熔断清单与 spbin 记账正确）。

## P0-008 Run manifest + baseline_eligible 原子切换

- **问题**：进度状态无权威来源——.config 追加写积累多行 status，strstr/逐行匹配把崩溃残留的旧 "Success" 误判为当前状态，不完整运行链式成为盲信基准。
- **根因**：状态文件无原子写、无单行覆盖语义、无"本次运行是否可当基准"的显式判定。
- **修复**：新增 `{base}.manifest`（`src/output/manifest.c`）：`.new → fflush+fsync → rename` 原子切换，key=value 单行覆盖；启动即写 status=Running 覆盖上次终态；终态由 manifest_finalize 统一判定。baseline_eligible=1 需同时满足：status=Success、spbin 清零、dspill 排空、无 fpbin/dfpbin 残留、archive 校验通过、输出尾部完整（末字节 '\n'）、非盲信运行。archive 走同名原子切换。退出码语义统一：0=完全完成，1=部分完成，2=严重失败，3=架构不匹配（预留）。
- **关键文件**：`src/output/manifest.c`、`src/output/progress_archive.c`、`src/output/progress.c`、`src/core/main.c`、`include/output/manifest.h`
- **验证**：回归用例 12/13（门禁 exit 2、Incomplete 不可作基准）。

## P0-009 目录任务生命周期状态机

- **问题**：目录任务散落在各处的隐式状态（pending 计数、slot 标志、集合成员）互不印证，完结校验"流程完结推不出数据完整"。
- **根因**：无单一状态机承载目录任务全生命周期。
- **修复**：目录任务状态机化（发现/入队/派发/BATCH 收齐/输出 COMMITTED/完成/出错），每次转移伴随记账，完结校验以状态机账目为准，与 P0-001 屏障、P0-002 三态咬合。
- **关键文件**：`src/scan/main_loop.c`、`src/scan/batch_processor.c`、`src/scan/dispatch.c`、`include/core/app_context.h`
- **验证**：回归用例 1/10（完结即完整，续传账目收敛）。

## P0-010 崩溃恢复矩阵 — Reset 援救机制

- **问题**：崩溃恢复遇到无法对账的残留状态（分片截断、索引与分片矛盾）时，旧代码带伤续跑，漏扫无痕。
- **根因**：恢复路径只有"乐观续传"一个分支，没有兜底。
- **修复**：Reset 援救：对账失败时回退到安全的全量重扫语义（at-least-once），已 unlink 的原始分片对应子树由 Reset 援救兜底重扫，宁可重复输出也不丢目录。
- **关键文件**：`src/output/progress_archive.c`
- **验证**：回归用例 10/11（任意崩溃点续传结果 == 基线）。

## P0-011 半增量（盲信）跳过前提条件声明

- **问题**：盲信扫描的启动前提（合格基准、schema 匹配、增量与基准隔离）全靠用户自觉，旧格式/不合格基准被静默盲信，结果不可信还链式成为下次基准。
- **根因**：工具不校验前提，且盲信复用的历史 stat 字段不全（schema 1 无 size/uid/gid/mode 全集）。
- **修复**：
  1. pbin 升级 schema 2：`[path_len][path][d_type][mtime_sec][mtime_nsec][size][uid][gid][mode][dev][ino][flags]`，atime 完全排除（NFS 不可信）；
  2. 盲信基准必须经 `--reference-base` 显式指定，manifest baseline_eligible=1 且 pbin_schema_version=2，本轮 `-f` 必须为新空目录——任一不满足 exit 2；
  3. reference_set/map 以纯路径指纹为 key（dev/ino 不参与盲信身份判断），收录基准完整历史 stat，命中时复用 size/mtime/uid/gid/mode 免 lstat；d_type 不一致视为未命中（设计 §0.6）；
  4. 盲信运行 manifest baseline_eligible=0，结果不能链式作为下次基准（设计 §0.4）。
- **关键文件**：`src/core/cmdline.c`（`--reference-base`）、`src/core/main.c`（启动门禁）、`src/scan/reference_map.c`、`src/scan/worker_scanner.c`、`src/output/progress_archive.c`、`include/scan/reference_map.h`
- **验证**：回归用例 12a/12b/12c（无基准拒跑、合格基准输出 == 全量、伪造基准拒跑）。

## P0-012 epoch + waitpid 确认（旧 Worker 残留数据）

- **问题**：Worker 死亡被替换后，旧 Worker 的迟到消息（FINISH/BATCH/心跳）按新 slot 账目入账，计数错乱。
- **根因**：slot 复用无代次概念，消息无法区分来自哪一代 Worker。
- **修复**：每 slot 引入 epoch 代次标记，替换 Worker 前 waitpid 确认旧进程死亡，之后到达的旧 epoch 消息一律过滤。
- **关键文件**：`src/scan/main_loop.c`、`src/scan/dispatch.c`、`include/ipc/worker_proc.h`
- **验证**：回归用例 2/8（高并发替换与并发删除场景下账目一致）。

---

## 附带变更

- **pbin 全量记录**：record_path 不再要求 `-c` 模式，全量扫描同样记录 pbin——任何一次成功运行都可作为恢复来源与盲信基准。
- **主循环自适应等待**：main_loop 改条件变量 + 自适应退避（`wait_for_ipc_messages`），空转 CPU 与吞吐兼得（见实现期缺陷 4）。

## 破坏性变更

- **旧格式进度文件不兼容**：v15.6.0 之前的进度（无 manifest、pbin schema 1）不能续传、不能作盲信基准；检测到旧残留拒绝续传（exit 2），必须 `--runone` 重新全量扫描。

---

## 实现期缺陷实录（编码过程中回归实测发现并已修复）

### 缺陷 1：enqueue_dir 的 completed 剪枝剪掉根目录

- **现象**：非续传运行崩溃后再恢复，整棵子树静默漏扫。
- **根因**：`enqueue_dir` 初版的差集剪枝只查 completed_set。崩溃后根目录可能残留命中 completed_set，但它不在 discovered_set（pbin 无其发现记录）——被剪枝后没有任何路径能重新发现它，整棵子树永久丢失。
- **修复**：completed 剪枝增加前置条件——仅当该目录**同时在 discovered_set**（pbin 有其发现记录，子树可由泵送/重扫闭环）才剪枝（`src/scan/dispatch.c` `enqueue_dir`）。

### 缺陷 2：dpbin 完成项无 pbin 记录兜底，差集剪枝漏扫

- **现象**：特定崩溃时序下（目录已完成并写入 dpbin，但 pbin 发现记录尚未落盘），恢复后该目录静默漏扫——回归实测。
- **根因**：恢复差集语义为"pbin − dpbin"，但初版剪枝只查 dpbin 加载出的 completed_set：dpbin 有完成记录而 pbin 无发现记录的目录，既不被泵送（pbin 里没有）又被剪枝（completed_set 命中），两边不沾，永久丢失。
- **修复**：与缺陷 1 同一守卫——completed 剪枝以"同时在 discovered_set"为前提：dpbin 完成项若无 pbin 记录兜底则不采信、重新入队重扫（at-least-once，宁重复不丢失）（`src/scan/dispatch.c` `enqueue_dir`）。

### 缺陷 3：find_max_pbin_index 空进度歧义导致泵送失效

- **现象**：空进度续传场景，已发现但未完成的目录永久丢失（回归实测漏扫）。
- **根因**：`find_max_pbin_index` 无法区分"无分片"与"最大序号为 0"——恢复时空进度把首个写入分片定为 1，而泵送仍从 0 开始，打不开即不泵。
- **修复**：以 `find_pbin_index_bounds`（`src/output/progress_archive.c`）显式区分两种情形，恢复路径同时确定泵送起点（首个现存分片）与写入序号（最大序号之后）。

### 缺陷 4：cond 丢失唤醒导致吞吐降 10 倍

- **现象**：主循环吞吐相比预期下降约 10 倍，CPU 占用却不高。
- **根因**：主循环等待与消息到达之间的信号丢失——消息在条件变量 signal 之前已入队，主循环仍睡满整个等待周期，每个周期只处理一批消息。
- **修复**：主循环改为条件变量 + 自适应退避等待（`wait_for_ipc_messages`，`src/scan/main_loop.c`），所有消息入队点持锁 signal；等待前复查队列状态，消除"入队与 wait 之间"的丢失唤醒窗口。

### 缺陷 5：next_dispatch_worker 有符号 int 溢出 → slots[负下标] 野读段错误

- **现象**：多 Worker 长扫描中 Master 偶发 SIGSEGV（回归用例 2 实测，wc=8/16 均出现，约 1/20 概率；崩溃点 `dispatch.c` `dispatch_find_idle_worker`）。
- **根因**：`ctx->next_dispatch_worker` 为 plain int，每次派发尝试 +1 永不回卷。缺陷 4 的自适应零等待主循环使"全部 Worker 忙 + 队列非空"的重入队空转以内存速度进行，约 2 分钟即可累满 2^31 溢出为负（实测崩溃瞬间值 0x80000001），`candidate = 负数 % 16 = -15` → `slots[-15]` 野读 → 段错误。该字段在 v15.5.9 已存在（基线同带病），但旧主循环 100ms 粒度限速下需数月才溢出，属潜伏 bug；v15.6.0 提速后变为可达。
- **修复**：计数器取模回卷——`candidate = next % num_workers; next = (candidate + 1) % num_workers`，值域恒为 [0, num_workers)，永不溢出（`src/scan/dispatch.c` `dispatch_find_idle_worker`）。
- **定位方法备忘**：无 gdb/core 环境（WSL core 被 wsl-capture-crash 截获、ptrace_scope=1），通过 SA_SIGINFO 处理器打印 si_addr/RIP + 函数内现场值全局变量 + 主循环阶段哨兵逐层收窄确认；si_addr 页内偏移恒定 0x628 是"固定下标野读"的关键指纹。

### 缺陷 6：per-slot 数组硬编码 [8] 越界读 → 假退避派发活锁（P0-008 字段布局激活潜伏 bug）

- **现象**：`--worker-count 16` 时扫描偶发永久挂起——主线程 100% CPU 空转、Worker 全部空闲等命令、队列剩 1 个任务永不派发（单实例约 6%，6 并发压测 32/120 轮复现；看门狗 32 次全部指认 stage=dispatch）。
- **根因**：`redispatch_backoff_until[8]`（以及 `timeout_paths[8]`、`timeout_counts[8]`）按 8 定长，但 `--worker-count` 命令行从未强制上限（帮助文本写"上限 8"却无校验）。`dispatch_from_queue` 的退避检查 `redispatch_backoff_until[wid]` 对 wid≥8 越界读；v15.5.9 时数组后邻是恒零的 `timeout_paths`，越界读得 0 无害；**v15.6.0 P0-008 在数组正后方插入 `run_id[64]`**，越界读到 ASCII 数字串（如 "1787236142" 的小端 time_t ≈ 3.5×10^18）→ 退避"截止于遥不可及的将来"→ 任务被无限 requeue-continue。当低号 slot（0-7）全部 BUSY（其 FINISH 滞留未读，因主线程已卡死在 dispatch 阶段不再回到 drain）而仅剩 wid≥8 空闲时，形成永久活锁。
- **修复**：定义 `MAX_WORKERS 64`（`include/core/config.h`），三个 per-slot 数组全部改为 `[MAX_WORKERS]`；`--worker-count` 超上限时钳制并告警（`src/core/cmdline.c`）；`circuit_breaker_check` 的宽限守卫同步改为 `MAX_WORKERS`。帮助文本上限 8 → 64。
- **注意**：该潜伏越界在基线 v15.5.9 同样存在（只是彼时读到的恒为 0），生产 v15.5.x 若在 P0-008 类字段变更后使用 >8 Worker 必踩——这是本期"修复后反而更不稳"错觉的来源之一。

---

## 验证方式汇总

- `tests/run_regression.sh` 用例 1-9（存量）+ 用例 10-13（本期新增，覆盖 P0-001/002/003/008/010/011 的端到端行为）全部 PASS；
- 并发压力：修复缺陷 5/6 后，6 并发实例 × 20 轮 `--worker-count 16` 全量扫描 0 崩溃 0 挂起（修复前同场景 32/120 轮挂起 + 偶发段错误）；
- 用例 10：KILL 首轮 + `-c` 续传，sort -u 与干净全量基线逐行一致，续传 manifest status=Success 且 baseline_eligible=1；
- 用例 11：KILL 两轮 + 第三轮续传收敛，sort -u == 基线（fpbin/dfpbin 二次崩溃路径）；
- 用例 12：盲信门禁三态——无 --reference-base exit 2、合格基准 exit 0 且输出 == 基线且 eligible=0、伪造基准 exit 2；
- 用例 13：EACCES 全量 exit 1 + status=Incomplete + spbin_count>=1，以其为基准盲信 exit 2。

---

## 收尾工程变更（2026-08-20/21，非 P0 设计项但随本期落地）

- **版本号统一**：`VERSION "15.6.0"` + 新增 `VERSION_NAME "v15.6.0"`；`VERSION_CODE = 202608202300UL`（`include/core/config.h`）。
- **版本限定日志门控码写死调用点**：本期新增的 4 处版本限定日志（CMD_SCAN 暂存 / CircuitBreaker 退避 / Barrier 完结 / FINISH EAGAIN 重试）直接写死时间戳字面量 `202608202330UL`，不定义宏——防止宏值随版本递进被一改全改、旧日志被不断宽限而失去门控意义。**严格遵循**（已写入 config.h 注释）：可能导致文件元数据被忽略或丢失的异常日志、以及 error 类型日志，不得被版本门控，必须归属于全局日志（引用 VERSION_CODE 的 `log_*` 宏）；`log_error`/`log_fatal` 在宏定义层固定引用 VERSION_CODE，结构上无法被门控。
- **Makefile 增加 `-MMD -MP` 头文件依赖跟踪**：此前修改 `app_context.h` 结构体布局后增量 make 产生新旧偏移混用的二进制（缺陷 6 排查期间一度出现"修复无效"假象），现已根治。

