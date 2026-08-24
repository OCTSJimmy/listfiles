# fixed_15.6.1_P0_incident_hotfix.md

> **版本**：v15.6.1（VERSION_CODE 202608241500UL）
> **日期**：2026-08-24
> **性质**：生产事故热修复——/public4 全量扫描事故（6/8 Worker 死亡、pending_tasks=-6、
> 主循环空转 54 小时、零日志）的根因修复全集。事故取证全记录见 `Design-todo-v15.6.1.md` §0。
> **验证**：回归 19/19 PASS（含 2 个新增故障注入用例）；6 并发 × 8 轮无干扰压测 48/48；
> 混沌 kill 压测（每轮随机杀 1-5 个 Worker）40/40。构建零警告。

---

## 1. 事故因果链（最终实锤版）

生产现场：`-c` 续传 /public4（NFSv4.1），结果目录在 ParaStor。运行 2d18h 后发现：
W2-W7 `DEAD pid=-1`、W0/W1 IDLE、`pending_tasks=-6`、dispatch_queue=0、全部输出文件
停滞于 40 小时前、屏幕零日志、进程 15 线程全部 S 态（无 D-state）。

取证（/proc + eu-stack/gdb backtrace + strace）+ 代码走查，因果链：

1. **启动期 6 个 Worker 经 RET_EXIT 静默退出**（精确触发点待 -v 复现；候选为
   `worker_proc.c` 的 POLLHUP/malloc 失败等无日志出口）；
2. **cleanup 无条件 `pending--`**：空闲死亡也销账，6 连退直接把账目打到 -6（R2）；
   且 RET_EXIT 走 `cleanup(redispatch_current=false)`：**在途目录不重入队、不写 spbin、
   不留痕**，enqueued_set 阻断重新发现 → 6 棵一级子树静默丢失（R9，1.9 亿 vs 8 亿
   缺口的真凶）；
3. **替换全部静默失败**：主循环每 100ms 走到 step 6 尝试替换，`clone() = -1 ENOMEM`
   ×54h——该机 `vm.overcommit_memory=2`（严格超售），fork 按全额 VSZ（69GB，
   `--estimated-files=10 亿` 预分配所致）计 commit，共享机 commit 紧张 → 必败、
   不自愈、零日志（R8）；
4. **永不退出**：`pending=-6` 使完结条件 `==0` 永久不成立（R4）；backtrace 证明
   主循环活着（cond_timedwait 空转）——"活而无效"，无看门狗（R5）；
5. **全程无声**：DEV_TIMEOUT 链路日志被旧门控码吞掉（R6），RET_EXIT/替换日志为
   log_info 被默认阈值压掉，spawn 失败路径压根没有日志（R8）。

附加实锤（代码走查）：RET_DEAD 的 stale 判定反转——真实死亡 100% 被误吞，崩溃
Worker 永不清理/替换/销账（R1，本次最危险缺陷）；DEV_TIMEOUT 不杀旧进程、
新旧两代并存（R3）；fork 于多线程期的锁继承风险（R7）。

## 2. 修复清单（P0-101 ~ P0-109）

### P0-101 死亡类消息同代校验（修 R1）
- 新增 `RetDeathPayload{reported_pid, epoch}`：RET_DEAD/RET_EXIT 载荷；
  `RetErrorPayload` 扩展 `reported_pid/epoch` 字段（DEV_TIMEOUT 必须，ERROR 顺带）。
- IPC 线程侧：`IpcThreadCtx.current_epoch` 在 CMD_SCAN 时记录、CMD_REPLACE 清零；
  `worker_mark_dead` 在清空 pid 前捕获 (pid, epoch) 随 RET_DEAD 上报。
- Master 侧：`death_msg_current_generation()`（main_loop.c）——仅当
  `reported_pid == slot->pid` 时受理死亡类消息，否则按跨代残留丢弃。
  **RET_ERROR/RET_ENTRY_ERROR 同样纳入校验**（见 §3 实现期缺陷 1）。
- 替换旧 stale 判定（`is_alive && pid != -1 → 丢弃`）——该判定把每条真实死亡都
  误吞，是本事故"Worker 死了却无人知晓"的直接原因之一。

### P0-102 cleanup 销账精确化 + DEV_TIMEOUT 节流（修 R2）
- `cleanup_dead_worker_slot` 销账规则重写：
  - `DT_SCANNING`：销 1（orphaned 不得叠加——单 slot 单在途任务，管道里未消费的
    SCAN 就是当前任务本身；原实现 `1+orphaned` 在此情形双倍销账，见 §3 实现期缺陷 2）；
  - `DT_BATCHES_RECEIVED/PROCESSED`：完成屏障接管（BATCH 已到齐），cleanup 不动
    task_state、不销账、不重入队——屏障照常完结（免重扫），替换动作由主循环 step 6
    延迟到屏障完结之后（否则 slot 复用会让 dpbin 拿到空路径）；
  - `DT_NONE/DT_COMPLETED`：无账可销——空闲 Worker 死亡不得 `pending--`。
- Worker 侧 DEV_TIMEOUT 节流：同一任务只报一次（`dev_timeout_reported` 标志，
  新任务复位）。原实现每 5s 重发，是跨代重复销账级联的放大器。

### P0-103 DEV_TIMEOUT 先杀后清（修 R3）
- `cleanup_dead_worker_slot` 在 `redispatch_current=true && pid>0` 时先
  `kill(SIGKILL)` + 100ms 限时 WNOHANG 收割，消除新旧两代并存窗口
  （原实现依赖 CMD_REPLACE 关 fd 让旧 Worker EPIPE/SIGPIPE 自杀）。

### P0-104 完结条件与账目不变量（修 R4）
- 完结条件 `pending_tasks == 0` → `<= 0`；
- 主循环每轮巡检不变量：`pending_tasks < 0` → `log_fatal` + 杀光存活 Worker +
  `_exit(2)`——账目 bug 必须在秒级炸出来，不得死等（本事故的 -6 若有此检查，
  t≈5s 即终止）。

### P0-105 有效进展看门狗（修 R5）
- monitor 线程新增 `stall_watchdog`：file+dir 计数无增长 且（pending_tasks!=0 或
  dispatch_queue 非空或存在 BUSY Worker）持续超阈值 → 全局 `log_error` 输出现场
  （全 slot 状态/账目/队列深度），然后按 `--stall-action` 终止：
  `exit`（默认）= 杀光 Worker 后 `_exit(2)`；`abort` = core dump。
- 新选项：`--stall-timeout=秒`（默认 900，须大于最大退避 300s；0=禁用）、
  `--stall-action=exit|abort`。
- 设计要点：主循环 tick 看门狗对本事故无效（循环一直在转）——必须按有效进展判定；
  设备探测等待/spbin 积压场景 pending==0 且队列空，不误报。

### P0-106 日志门控违规清扫（修 R6）
- 解除门控改全局：`RET_DEV_TIMEOUT`（main_loop.c）、Worker 侧 scanner 卡死上报
  （worker_proc.c）、`cmd_queue 满丢 SCAN`（dispatch.c）、`熔断跳过写 spbin`
  （dispatch.c）。
- 新增全局日志：Worker 侧退出出口（POLLHUP/recv 失败/malloc 失败，原为静默 break）；
  FINISH 发送最终失败（worker_scanner.c，FINISH 丢失 = Master 永不完结该任务）；
  Worker 替换事件（log_info → log_warn）；非预期 EXIT（见 P0-109）。
- 原则复述（既定规则）：可能导致文件元数据被忽略或丢失的日志、error 类日志，
  一律全局（引用 VERSION_CODE 的 log_* 宏），不得版本门控。

### P0-107 fork 安全三件套（修 R7/R8 结构性根治）
- **a) fork 时序前移**：全部 fork（初始 Worker + 预备役）集中在单线程期、巨型指纹
  集合（estimated-files 预分配）之前、一切 pthread_create 之前（含 monitor 线程）。
  严格超售下小 VSZ fork 必成功，ENOMEM 类问题从构造上消除；单线程期 fork 同时
  免疫锁状态继承风险。（盲信模式的 reference_set/map 是 Worker 的 COW 只读上下文，
  须在 fork 前加载——该功能已整体押后，不在本优化覆盖范围。）
- **b) 预备役 Worker 池**：启动期额外 fork num_workers 个 spare（完整 Worker 子进程，
  READY/心跳写入管道缓冲待启用）；运行期替换只从 spare 池取（`worker_pool_replace`
  改为 spare 启用），**运行期零 fork**。spare 耗尽 → 全局 `log_error`（一次）+
  降额运行，由看门狗兜底。
- **c) 子进程无锁日志**：`log_set_forked_child()`（worker_main 第一语句）→
  `log_msg/log_vraw` 跳过 `flockfile`，栈缓冲组装后单次 `write(2)`。
- 死 spare 自愈：spare 在启用前被杀 → 启用后 IPC 线程读 HUP → RET_DEAD（pid 匹配）
  → cleanup → 取下一个 spare。

### P0-108 spawn/fork 失败必须有声（修 R8）
- `spawn_one_worker`（原语抽取）所有失败路径全局 `log_error`（含 errno）；
  探测 fork（monitor.c dispatch_probes）失败同样补全局 `log_error`。
- 初始 fork 部分失败：slot 置 pid=-1 交由运行期 spare 补位自愈；全部失败 → 致命退出。
- 关联评估：`--estimated-files` 预分配吹大 VSZ 是 fork ENOMEM 的放大器——指纹集合
  懒分配/按需扩容列入 v15.7.0 评估项。

### P0-109 RET_EXIT 携带在途任务 = 非预期死亡（修 R9）
- RET_EXIT 受理时若 slot 有在途任务（DT_SCANNING/DT_BATCHES_RECEIVED/DT_BATCHES_PROCESSED）
  → 全局 `log_error` + `cleanup(redispatch_current=true)`（在途目录重入队/写 spbin 留痕）。
- "正常退出"的合法时机收紧为仅 STOP 之后；其余 EXIT 一律 WARN 以上可见。

## 3. 实现期缺陷实录（混沌压测暴露，同轮修复）

1. **迟到 RET_ERROR 跨代误销账**（首轮混沌压测 B 段 1/10 复现 exit=2）：Worker 被杀前
   发出的 ERROR 晚于替换到达，对新一代 slot 再销一次账。修复：RET_ERROR/RET_ENTRY_ERROR
   纳入 P0-101 同代校验。此后 40/40。
2. **orphaned 与在途任务双倍销账**：原 `pending -= 1 + orphaned` 在"SCAN 未读入即死亡"
   场景对同一任务销两次（orphaned 与 +1 是同一任务）。修复：单 slot 单在途任务语义下
   orphaned>0 必然等价于 DT_SCANNING，销账取 1；orphaned>0 而 task_state 非 DT_SCANNING
   记设计外异常 WARN 且不销账。

## 4. 验证证据

- `make clean && make`：零警告（-Wall -Wextra）。
- `tests/run_regression.sh`：**19/19 PASS**（既有 17 断言 + 新增用例 14/15）。
  - 用例 14：扫描中段 kill -9 3 个 Worker → spare 补位，输出与基线逐行一致，
    stderr 有 DEAD/预备役日志，无 INVARIANT。
  - 用例 15：杀光全部子进程（8 初始 + 8 spare）→ spare 耗尽告警 → 看门狗
    （--stall-timeout=5）触发 exit=2，无负账目、无静默挂起。
- 压测：6 并发 × 8 轮无干扰 48/48 输出一致；混沌 kill（每轮随机杀 1-5 个 Worker，
  含 spare）40/40 输出与 manifest 逐行一致、无 INVARIANT。

## 5. 生产部署须知（/public4 重跑前必读）

1. **从头全量重跑，不用 --continue**（用户决策：隔离变量；续传验证押后）。
2. 该机 `vm.overcommit_memory=2`（严格超售）：v15.6.1 已将全部 fork 前移到小 VSZ
   窗口，运行期零 fork，理论上免疫；但请关注启动期的 `[Spare]` 日志确认预备役就绪。
3. **日志必须重定向到文件**（`2> run.log`）——本事故零日志是两天无人察觉的直接原因；
   v15.6.1 起生命周期关键事件（DEAD/DEV_TIMEOUT/非预期 EXIT/替换/spare 耗尽/看门狗）
   在默认级别全部可见。
4. ParaStor 私有客户端（2019 底子 + RDMA/IB）为环境风险项：dmesg 有 knal/ofs 模块
   WARN 记录；如有条件，结果目录与扫描目标错开故障域。
5. 环境内存紧张 episode 存在（dmesg Mem-Info dump）——大规模运行建议预留内存余量。

## 6. 修改的文件

- `include/core/config.h` — VERSION 15.6.1 / VERSION_CODE 202608241500UL；看门狗默认值
- `include/ipc/msg_format.h` — RetDeathPayload；RetErrorPayload 扩展 pid/epoch
- `include/ipc/ipc_thread.h` — IpcThreadCtx.current_epoch
- `include/ipc/worker_proc.h` — SpareWorker、WorkerPool spare 字段、spawn_spares 声明
- `include/output/monitor.h` — 看门狗基线字段
- `include/scan/worker_scanner.h` — dev_timeout_reported
- `include/util/log.h` / `src/util/log.c` — 子进程无锁日志模式
- `src/ipc/worker_proc.c` — spawn_one_worker 原语（失败有声）、预备役池、replace 改
  spare 启用、worker_main 无锁日志/DEV_TIMEOUT 节流/退出出口留痕
- `src/ipc/ipc_worker_mgmt.c` — RET_DEAD 携带 (pid, epoch)
- `src/ipc/ipc_message_handler.c` — current_epoch 跟踪；EXIT/DEV_TIMEOUT/ERROR 载荷填充
- `src/scan/dispatch.c` — cleanup 先杀后清 + 销账精确化；门控清扫
- `src/scan/main_loop.c` — death_msg_current_generation 同代校验；RET_EXIT 非预期死亡
  处理；完结条件 + 账目不变量；替换门（屏障接管态延迟替换）
- `src/scan/worker_scanner.c` — FINISH 发送最终失败全局日志
- `src/output/monitor.c` — stall_watchdog；探测 fork 失败留痕
- `src/core/main.c` — fork 时序前移（P0-107a 启动序）
- `src/core/cmdline.c` — --stall-timeout / --stall-action
- `tests/run_regression.sh` — 用例 14/15
