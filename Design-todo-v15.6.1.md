# Design-todo-v15.6.1.md

> 本文档是 **v15.6.1 热修复规划文档**，源于 2026-08-21 生产环境 /public4 全量扫描事故
> （运行 2d18h 后 6/8 Worker 死亡、`pending_tasks=-6`、主循环空转、屏幕零异常日志）。
>
> **状态（2026-08-25）：全部 9 项 P0（P0-101~109）已编码落地并通过验证**——
> 回归 19/19（新增用例 14/15）、6 并发 × 8 轮压测 48/48、混沌 kill 压测 40/40。
> 实现细节与验证证据归档于 `fix_documents/fixed_15.6.1_P0_incident_hotfix.md`；
> 代码事实已同步至 `Design.md`（v15.6.1）。本文档保留作事故取证档案。
> 下一步：/public4 生产从头全量重跑验证（不用 --continue）。
>
> 标注约定：【实锤】= 代码或活体证据可证明；【待证】= 需 -v 复现钉死精确触发点。

---

## 0. 事故现场（2026-08-24 取证记录）

- 命令行：`listfiles --path=/public4 --continue --yes --batch-size=1024 --estimated-files=1000000000 --progress-file=...public4-progress --output-split=...public4-output --csv --quote --dirs --archive`
- 症状（Aug 24 11:10 快照）：
  - W2-W7 `DEAD pid=-1`，W0/W1 IDLE，Active 2/8
  - `Pending tasks: -6`，pending_batches=0，dispatch_queue=0
  - 全部输出文件停滞于 Aug 22 05:42（pbin_001936），dspill 停滞于 Aug 22 02:24（151MB）
  - 主进程存活、Elapsed 持续走动（monitor 为独立线程，主线程状态不影响状态页刷新）
  - 屏幕零异常日志（日志未重定向）；内存 available 130G、fd 52 —— 排除常规 OOM/资源耗尽
  - 已扫 1.9 亿文件 vs `df -i` 8 亿 —— 缺口解释见 R9（6 棵子树被静默丢弃）
- **关键阴性证据**：结果目录**不存在 `{base}.spbin` 文件** → 本次运行从未持久化任何
  POISON/熔断/RET_ERROR 跳过记录。

### 0.1 活体取证（2026-08-24，/proc + dmesg + mount）

- `/proc/145805/status`：State S、Threads 15；**全部 15 个线程均为 S（睡眠），无 D-state**：
  主线程 `futex_wait_queue_me`；monitor `hrtimer_nanosleep`；IPC×8 `ep_poll`；
  线程池×4 + async writer×1 `futex_wait_queue_me`（等活，正常空闲）。
- mount：扫描目标 `/public4` = NFSv4.1（nfs.public4.ncrcnd.com:/datapool/public4）；
  结果目录 `/public/home/...` 位于 `/public` = **ParaStor** —— 不同后端存储。
- dmesg：Aug 24 09:29 有 knal/ofs 模块（ParaStor 私有客户端）内核 WARN + 全量
  Mem-Info dump，当时 Node0 Normal free 仅 ~111MB；历史 swap 换入换出达 42 亿页次
  —— 该机存在真实的内存紧张 episode（fork ENOMEM 的旁证，见 R8）。
  另有 `wc`/`head` 在 ld-2.17.so 内 segfault at 0 的环境级异常记录（与 listfiles 无
  直接关联；该 ParaStor 私有客户端为 2019 年上架、绑 RDMA/IB + OpenMPI/Slurm 的
  老旧复杂栈，环境风险记入运维须知）。
- monitor 中 W0/W1 的 path "乱码"已澄清：`path_log_mask` 保留各路径分量末字节，
  UTF-8 中文名的末字节是多字节续字节 → 显示 mojibake，**非内存损坏**。
- 推断修正：`[MainLoop]`/`[Replace]` 等周期日志均为 `log_info`，默认 WARN 阈值下本就
  不可见——"屏幕无日志"不能作为"主循环停转"的证据。默认日志级别下本程序运行期
  近乎完全静默，本身即是可观测性缺陷（随 P0-106 一并整改）。

### 0.2 决定性取证（2026-08-24，eu-stack/gdb backtrace + strace）

- backtrace：主线程栈 `pthread_cond_timedwait → wait_for_ipc_messages → main_loop_run`
  ——**主循环活着**，处于每 100ms 的正常条件等待；其余线程状态组合 = "进程正常运行
  但无活可干"。"主线程楔死于互斥锁 / D-state"假设均被推翻。
- strace 实锤终态：**`clone() = -1 ENOMEM` 每 ~100ms × 6 个死 slot 成串出现**
  （pipe2 正常、fd 正常回收）——主循环每轮都走到 step 6 尝试替换，fork 每次确定性
  失败，54 小时约 1900 万次空转与替换尝试，全程零日志（R8）。
- ENOMEM 机制实锤：Master `VmSize=69GB / VmRSS=31GB`（`--estimated-files=1000000000`
  预分配的巨型指纹集合把虚拟地址空间吹大）；该机 `vm.overcommit_memory=2`
  （**严格超售**）——fork 要求 CommitLimit 内有**全额 VSZ（69GB）** 余量，与实际
  RSS 无关；共享机其他负载吃紧 commit → fork 必败。
- 设计教训：循环 tick 看门狗检测不到"活而无效"——P0-105 看门狗必须按**有效进展**
  判定；本病最早的自动捕获点是 P0-104（`pending_tasks<0` 即 log_fatal，t≈5s 触发）。

---

## 1. 根因链（代码取证结论）

### R1【实锤】RET_DEAD 的 stale 判定反转，真实死亡被吞噬（最重）

`main_loop.c:120`：`if (is_alive && pid != -1) break;` —— 意图是丢弃"换代后的残留
RET_DEAD"，但 `is_alive=false` 只会被 `cleanup_dead_worker_slot` / `worker_pool_replace`
设置（IPC 线程侧 `worker_mark_dead` 只动 `IpcThreadCtx`，从不碰 slot）。真实死亡的
第一条 RET_DEAD 到达时 slot 恰好 `is_alive=true, pid>0` → **100% 被误判为残留丢弃**。
后果：崩溃死亡的 Worker 永不清理、永不替换、pending_tasks 永不销账、无日志。
本次事故中能走到 DEAD 显示的 slot 全部只能来自 RET_DEV_TIMEOUT / RET_EXIT 路径。

### R2【实锤】cleanup 销账无前提 + 跨代重复销账 → `pending_tasks = -6`

- `cleanup_dead_worker_slot` 的 `atomic_fetch_sub(&ctx->pending_tasks, 1 + orphaned)`
  （`dispatch.c:326`）**无条件执行**：空闲 Worker 死亡（无在途任务）也销账；
  且对 DT_BATCHES_RECEIVED/PROCESSED 状态的任务会与完成屏障重复销账。
  **启动期 6 个 Worker 依次退出即可直接把 pending 打到 -6**——这是本事故 -6 最直接的
  来源，比跨代级联更早、更简单。
- 级联放大器：epoch 校验只覆盖 RET_BATCH/RET_FINISH（`main_loop.c:41-58`），
  **RET_DEV_TIMEOUT / RET_DEAD / RET_EXIT 无任何代数校验**；Worker 侧 scanner 卡死后
  **每 5 秒重复发送** DEV_TIMEOUT 且无节流（`worker_proc.c:178-207`）→ 第 1 条触发
  cleanup + 换代（`cleanup_done` 被 spawn 清零）→ 滞留的第 2..N 条对新一代 slot 再次
  完整 cleanup，每条再多销一次。

### R3【实锤】DEV_TIMEOUT 清理不杀旧 Worker，新旧两代并存

DEV_TIMEOUT 语义是"scanner 线程卡住"，**进程是活的**。但 cleanup 先置 `is_alive=false`，
导致随后的 `worker_pool_replace` 跳过 kill 分支（`worker_proc.c:425`）——旧 Worker 依赖
CMD_REPLACE 关闭其 fd 后 EPIPE/SIGPIPE 自杀。这中间的窗口期就是 R2 级联的温床。

### R4【实锤】完结条件 `pending_tasks == 0` 遇负数永久挂起

`main_loop.c:560` 严格等于零。账目一旦为负，即使全部工作完成、队列全空也永不退出。
本次事故即使替换全部成功，也注定无限空转。

### R5【实锤】终态 = 主循环空转 + 替换静默失败，无"有效进展"看门狗

backtrace + strace（§0.2）证明主循环活着（100ms cond_timedwait 循环），停摆不是锁死、
不是 D-state，而是：每轮 step 6 尝试替换 6 个死 slot → `worker_pool_spawn` fork
ENOMEM（严格超售 + 69GB VSZ）→ 无声（R8）→ 下轮再来。monitor 线程只管展示、
不监督扫描是否在推进 → "活而无效"两天无人知晓。

### R6【实锤】DEV_TIMEOUT 全链路日志被版本门控吞掉（违反既定规则）

`main_loop.c:129`、`worker_proc.c:188` 等使用 `log_error_v(202607030000UL, ...)`，
门控码 < 当前 `VERSION_CODE(202608202300UL)` → 静默。**违反"可能导致元数据忽略的异常
日志与 error 类日志不得被版本门控"的既定规则**——这是事故两天无人察觉的直接原因。
同族问题：RET_EXIT 路径全是 `log_info`（`main_loop.c:134`），默认 WARN 阈值下不可见；
Worker 侧 POLLHUP/malloc 失败等退出出口完全无日志（`worker_proc.c:144/147/175`）。

### R7【实锤机制/待证显形】多线程进程中 fork，子进程非 async-signal-safe

- 启动期：`main.c:484-490` 先起 monitor 线程、再 fork 8 个 Worker；
  运行期替换（`main_loop.c:542`）在全部线程存活时 fork。
- 子进程不 exec，直接 `worker_main` → `pthread_create`/malloc/stdio。glibc 对 malloc
  arena 有 atfork 保护，但 `log.c:59` 的 `flockfile(stderr)` 与 `localtime` 的 tzset
  锁不在保护范围 → fork 瞬间若有线程持锁，子进程继承死锁。
- **显形修正**：fork 伤通常不是秒级猝死，而是**延迟楔死**——子进程 READY/心跳/扫描在
  默认 WARN 阈值下不碰 stderr（`log.c:53` 阈值短路在进锁之前），跑到首次真正需要
  WARN+ 日志时才锁死。fork 伤可能是 Worker 后来"卡死"（DEV_TIMEOUT）的远因之一。
- 【待证】"启动 5-30s 内 6/8 Worker 死亡"的精确触发点：秒级减员且零日志最吻合
  RET_EXIT 静默出口（`worker_proc.c:175` POLLERR|POLLHUP 无日志 break；`:144/147`
  malloc/recv 失败无日志 break），需 -v 复现钉死。

### R8【实锤】spawn/replace 失败完全静默；失败原因 = 严格超售下 fork ENOMEM

`worker_pool_replace` 返回 false 无任何日志；主循环 step 6 每轮静默重试，无上限、
无降级、无告警。strace 实锤本事故中失败原因：`clone() = -1 ENOMEM`——
`vm.overcommit_memory=2` 下 fork 按全额 VSZ（69GB）计 commit，与 RSS 无关；
VSZ 由 `--estimated-files=1000000000` 的指纹集合预分配吹大，共享机 commit 紧张时
fork 必败且**永久不自愈**。

### R9【实锤】RET_EXIT 丢弃在途目录且不留痕 → 子树静默丢失（1.9 亿 vs 8 亿缺口的
最可能解释）

RET_EXIT 走 `cleanup_dead_worker_slot(ctx, slot, /*redispatch_current=*/false)`
（`main_loop.c:133-136`）：**在途目录不重入队、不写 spbin、不记熔断**。而该目录入队时
已登记 `enqueued_set`——父目录重扫时会被防重阻断，永不重新发现。若 6 个 Worker 在
启动后各领了 1 个一级子目录任务再退出，这 6 棵子树即从本轮扫描中**静默消失**，
W0/W1 扫完剩余 2 棵后 frontier 耗尽（05:42 输出停滞），账目为负（R2）又使完结
永不触发——挂起状态反而**掩盖了覆盖损失**。
"正常退出"只应发生在收到 STOP 之后；携带在途任务的 EXIT 必须视同非预期死亡。

---

## 2. P0 修复项（v15.6.1）

### P0-101 RET_DEAD 按 (pid, epoch) 校验，修复 stale 判定反转
- RET_DEAD / RET_DEV_TIMEOUT / RET_EXIT 消息携带 IPC 线程观测到的死亡 pid 与 slot
  当前 epoch。
- Master 受理条件：`pid == slot->pid`（同代）才执行 cleanup；否则按残留丢弃并记 debug。
- 验收：kill -9 任意 Worker → 100% 被清理、替换、任务重入队，有全局日志。

### P0-102 cleanup 销账精确化 + DEV_TIMEOUT 节流 + 幂等键升级
- **销账前提**：`cleanup_dead_worker_slot` 仅在 slot 确有在途任务且屏障未接管时销账
  （精确规则：仅 `task_state == DT_SCANNING` 时由 cleanup 销 `1 + orphaned`；
  DT_BATCHES_RECEIVED/PROCESSED 由完成屏障销账；DT_NONE 不销账）。
- Worker：同一任务只报一次 DEV_TIMEOUT（报后置标志，任务切换时复位）。
- Master：cleanup 幂等键由 `cleanup_done` 标志升级为 (pid, epoch)——换代 spawn 不得
  清零旧代死亡的判定依据。
- 验收：注入 scanner 卡死（`tests/inject_readdir.c` 扩展 sleep 模式）→ 每任务恰好
  销账一次；启动期 Worker 空闲退出 → pending_tasks 不变；全程 `pending_tasks >= 0`。

### P0-103 DEV_TIMEOUT 路径先杀后清
- `cleanup_dead_worker_slot` 在 `redispatch_current=true` 且 `pid>0` 时先
  `kill(pid, SIGKILL)` 并 WNOHANG 收割（沿用 `worker_pool_replace` 的 100ms 轮询），
  消除新旧两代并存窗口。

### P0-104 完结条件与账目不变量
- `pending_tasks == 0` 改 `<= 0`；`pending_tasks < 0` 立即 `log_fatal`（账目 bug 必须
  暴露，不得死等）。主循环每 N 秒巡检一次不变量。
- 验收：人为制造一次错误销账（故障注入）→ 进程 log_fatal 退出而非挂起。

### P0-105 "有效进展"看门狗
- monitor 线程监督**有效进展**：`(files+dirs 计数) 无增长 且 (pending_tasks != 0 或
  dispatch_queue 非空 或存在 BUSY Worker)` 持续超过阈值（建议 300s）→ 全局 `log_error`
  输出现场（各 slot 状态、pending、队列深度）并以非零码退出（可配
  `--stall-action=abort|exit`，默认 exit 2）。
- 注意（§0.2 教训）：主循环 tick 看门狗**不够**——本事故中主循环一直在转。
- 中期（可推迟到 v15.7.0）：`advance_task_barriers` 的 flush 移出主线程。
- 验收：故障注入"Worker 全灭 + spawn 失败"→ 看门狗在阈值内报警退出，不无限空转。

### P0-106 解除事故链路的日志门控违规 + 可观测性整改
- DEV_TIMEOUT 全链路（`main_loop.c:129`、`worker_proc.c:188` 等所有 `202607030000UL`
  的 error/warn 级调用）改全局 `log_error`/`log_warn`。
- RET_EXIT / Worker 非预期退出路径由 `log_info` 提升为 `log_error`（R9）；
  Worker 侧退出出口（POLLHUP/recv 失败/malloc 失败）全部补日志。
- Worker 替换失败/成功、任务重入队等生命周期关键事件在默认级别可见（WARN+）。
- 原则复述（既定规则）：可能导致文件元数据被忽略或丢失的日志、error 类日志，
  一律全局，不得版本门控。

### P0-107 fork 安全（用户提出，评级 P0）
- a) **fork 时机前移**：初始 Worker + 预备役的 fork 全部移到 `worker_set_context`
  之后、**巨型指纹集合分配之前**（VSZ 最小时刻），且在一切 `pthread_create` 之前
  （含 monitor 线程，`main.c:484-490` 顺序反转）。严格超售下小 VSZ fork 必成功，
  ENOMEM 类问题从构造上消除；单线程期 fork 同时免疫锁状态继承风险（R7）。
- b) **预备役 Worker 池**：单线程期额外 fork N 个 spare（建议 N=num_workers），运行期
  替换只从 spare 池取；spare 耗尽 → 全局 `log_error` + 降额运行，**运行期永不 fork**。
- c) **子进程加固兜底**：fork 后子进程第一语句置 `g_in_forked_child`；`log_msg` 检测到
  该标志时跳过 `flockfile`，走无锁 `write(2)` 组装输出。
- 验收：高日志负载下循环替换压测（数千次，耗尽 spare 后继续）无一楔死；
  严格超售 + 大 estimated-files 环境下启动/替换必成功或响亮降级。

### P0-108 spawn/replace 失败必须有声
- `worker_pool_spawn`/`worker_pool_replace` 失败路径补全局 `log_error`（含 errno、
  VmSize、overcommit 状态）；连续失败达阈值 → `log_fatal`（宁可终止也不空转）。
- 关联评估（结论记入 Design.md，懒分配本身可推迟到 v15.7.0）：指纹集合预分配改
  懒分配/按需扩容可同时降低 VSZ（fork 成本）与启动内存冲击。

### P0-109 RET_EXIT 携带在途任务 = 非预期死亡（修 R9）
- RET_EXIT 处理时若 slot 有在途任务（`task_state != DT_NONE`）：视同非预期死亡——
  在途目录重入队（等价 redispatch_current=true）+ 全局 `log_error`。
- "正常退出"的合法时机收紧为仅 STOP 之后；其余 EXIT 一律 WARN 以上。
- 验收：Worker 带任务退出 → 目录被重扫，无静默丢失，有全局日志。

---

## 3. 验收标准（v15.6.1 总体）

1. `tests/run_regression.sh` 既有 17/17 不回归。
2. 新增故障注入回归用例：
   - kill -9 Worker（扫描中段）→ 替换 + 重扫 + 正常完结；
   - Worker 启动期空闲退出 → pending_tasks 不变负、替换成功、扫描正常完结；
   - Worker 带任务退出（RET_EXIT）→ 在途目录重入队重扫，全局日志可见；
   - scanner 卡死注入（超 heartbeat_timeout）→ 单任务恰好一次 DEV_TIMEOUT、一次销账、
     退避重扫或毒丸隔离，全程有全局日志；
   - "Worker 全灭 + spawn 失败"注入 → 有效进展看门狗报警退出，退出码非零；
   - 启动高并发替换压测（monitor 高频打印中反复替换，含 spare 耗尽路径）。
3. 全程 `pending_tasks >= 0` 断言（debug 构建内嵌）。
4. 生产现场处置顺序：
   ~~strace 取证~~（已完成，见 §0.2）→ 打包备份 progress 目录（留作事后比对材料）
   → kill 现场（`kill -9 145805 145807 145809`）→ **v15.6.1 发布后从头全量重跑
   （不使用 --continue）**。
   决策记录（2026-08-24 用户拍板）：放弃对事故现场的续传——全量逻辑与续传逻辑必须
   隔离变量，先把整体流程跑干净；续传验证押后到全量验证通过之后，届时续传出问题
   才能唯一归因于续传链路。盲信（--reference-base/blind_trust）功能整体押后至最后，
   其余功能全部 OK 之前不考虑。

## 4. 排期

- v15.6.1（本热修复）：P0-101 ~ P0-109 全部。完成后在 /public4 **从头全量重跑**验证
  （不用 --continue）；续传与崩溃恢复的生产验证在其后单独进行。
- v15.7.0：既有 P1-P3 规划不变，待 v15.6.1 生产验证通过后推进；盲信相关条目最后。
