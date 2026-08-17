# Design-todo-v15.6.0.md 插入内容

> 警告：本文档是 **v15.6.0 规划文档**，包含尚未编码实现的设计变更。当前代码事实请参考 `Design.md`（v15.5.9）。

---

## 0. 待修正项（来自设计评审）

以下修正尚未合并到正文，需在编码前落实。

---

### 0.1 discovered_set 目录级限定（关闭 P0-002 反例）
- `discovered_set` **严格只包含目录**（`d_type == directory`）
- 文件记录不进入目录任务去重集合
- 恢复时从 pbin 只加载目录记录
- 文件输出语义 = **at-least-once**（允许重复行）

### 0.2 pbin 格式扩展
当前：`[path_len][path][dev][ino][mtime][d_type]`
需扩展为：
```
[path_len][path][d_type][mtime_sec][mtime_nsec][size][uid][gid][mode][dev][ino][flags]
```
- flags: 标记盲信安全字段版本
- dev/ino: 仅全量扫描/诊断用，盲信不用
- atime: **完全排除**（NFS 不可信）

### 0.3 盲信扫描信任模型
- **信任本轮 readdir**: 存在性、路径名称、目录成员关系
- **信任历史 pbin**: size, mtime, uid, gid, mode, d_type
- **不信任**: atime, dev+ino（不参与盲信身份判断）
- **skip_interval 改名**: `blind_min_age`（死数据启发式，非变更检测）

### 0.4 两级扫描体系
- **第一层**: 低频全量重扫（~2周），建立/刷新基准
- **第二层**: 高频盲信扫描（2/6/12/24/48h），复用基准
- 盲信结果 **不能链式作为下次基准**（baseline_eligible = false）
- 基准有效期由 **业务层控制**，工具仅记录 `baseline_completed_at` 供审计

### 0.5 dpbin 与输出截断
- 接受 at-least-once，不精确回滚输出到 dpbin offset
- 恢复时仅截断损坏的输出尾部（不完整最后一条记录）
- 未完成目录重扫后，文件条目可能重复输出

### 0.6 d_type 不一致处理
- 路径命中但本轮 `d_type` != 历史 `d_type` → 视为盲信未命中，必须重新 lstat
- 防止"文件变目录、目录变文件"误判

### 0.7 manifest 扩展
`.config` 升级或拆分为独立 `manifest`：
- baseline_run_id, baseline_completed_at, baseline_checksum
- 盲信命中率统计
- pbin_schema_version

---

## P0 阻塞项（进入编码前必须闭环）

> 全部 12 项 Design v1 已确认，可进入编码实现。

---

### P0-001 FINISH/BATCH 竞态 — 目录任务完成屏障

**问题描述**
Worker 先发 BATCH 再发 FINISH，但 `fd_data` 和 `fd_ctrl` 是独立 epoll 通道。Master 可能先收到 FINISH 标记目录完成，后续 BATCH 滞留丢失，子树永久漏扫。

**根因分析**
FINISH 语义不等待 BATCH 全处理。当前设计收到 FINISH 后直接 `pending_tasks--` 并标记目录完成，没有确认该目录产生的全部 BATCH 是否已被接收和处理。

**设计方案**
目录任务生命周期状态机定义 `SCANNING → ALL_BATCHES_RECEIVED → ALL_BATCHES_PROCESSED → OUTPUT_COMMITTED → COMPLETED` 路径：
1. FINISH 仅触发 `ALL_BATCHES_RECEIVED`（收到 Worker 声明的扫描结束信号）
2. 必须等该目录产生的**全部 BATCH** 被 Master 接收并处理
3. 所有子目录完成去重并入队（或写入 dspill）
4. 该目录下所有文件条目经输出线程确认已落盘（`OUTPUT_COMMITTED`）
5. 最后才写 `dpbin`，标记 `COMPLETED`
6. **空目录**（无 BATCH，直接 FINISH）可直通完成

**关键实现点**
- Master 侧维护每个目录任务的 `expected_batch_count` 或批次序号追踪
- 收到 FINISH 后状态机推进到 `ALL_BATCHES_RECEIVED`，但不释放 Worker 也不减 `pending_tasks`
- 去重线程池完成该目录全部 BATCH 处理后，通知主线程推进到 `ALL_BATCHES_PROCESSED`
- 输出线程确认落盘后推进到 `OUTPUT_COMMITTED`
- 只有 `OUTPUT_COMMITTED` 后才能 `dpbin_append` 和 `pending_tasks--`

**关联项**: P0-009 目录任务生命周期状态机

---

### P0-002 输出三态状态机 — DISCOVERED → OUTPUT_QUEUED → OUTPUT_COMMITTED

**问题描述**
pbin 记录"已发现"不代表"已输出"。崩溃后恢复时，已写 pbin 但未写输出的文件条目可能永久丢失（如果输出被截断且重扫时被 discovered_set 抑制）。

**根因分析**
发现态与输出提交态混用。当前设计把"写进 pbin"当作"该条目已处理完毕"，但 pbin 和输出是异步的，存在崩溃窗口。

**设计方案**
目录任务生命周期状态机引入 `OUTPUT_COMMITTED` 态：
1. `DISCOVERED` — batch_processor 处理新条目，写 pbin，入输出队列
2. `OUTPUT_QUEUED` — 条目已提交到异步输出线程队列
3. `OUTPUT_COMMITTED` — 输出线程确认该目录所有文件条目已落盘（fsync 或写入操作系统缓冲区）
4. 只有 `OUTPUT_COMMITTED` 后才能写 `dpbin`

**输出语义**
- 全量扫描/续传：at-least-once，允许重复行
- 盲信扫描：基准回放 + 新增发现
- 恢复时截断损坏尾部，不精确回滚到 dpbin offset

**关联项**: P0-001, P0-009

---

### P0-003 fpbin 二次崩溃恢复路径

**问题描述**
第一次续传期间发现新目录 A 写入 fpbin，旧 pbin 未消费完时再次崩溃。第二次续传不读 fpbin，A 永久漏扫。

**根因分析**
恢复期间父目录扫描完会写 dpbin，崩溃后父目录不在 `pbin - dpbin` 差集中，不会重扫，导致 fpbin 里的子目录永久丢失。

**设计方案**
引入 `fpbin + dfpbin` 原子对作为**可抛弃工作区**：
1. **dfpbin** 记录 fpbin 阶段已完成的父目录（格式同 dpbin）
2. 恢复时检查 `fpbin + dfpbin` 完整性（Footer 校验通过）
   - **完整** → fpbin 内容合并到 pbin，清空 fpbin/dfpbin，进入 `HIST_PUMP_NEW`
   - **不完整** → **整对抛弃**，重新从旧 pbin 恢复（Reset 援救）
3. 不会无限套娃——总是回退到旧 pbin 恢复，不会递归产生新的 fpbin
4. 旧 pbin 全部消费完后 `hist_pump_state = HIST_PUMP_DONE`，fpbin 转正入队

**关键实现点**
- dfpbin 只记录 fpbin 阶段已完成的父目录路径（不是 fpbin 里的子目录）
- Footer 校验不通过 → 视为不完整，整对抛弃
- 重新扫描时，父目录会重新从旧 pbin 入队，重新发现子目录
- 若某父目录总是导致崩溃，会触发目录级熔断/毒丸机制（P0-005），不会无限循环

---

### P0-004 visited_set 背压竞态 — 统一队列模型

**问题描述**
目录因背压写 pbin 后进 visited_set，但未入队，只进 dspill。回填时 visited_set 误判"已访问"而丢弃。

**根因分析**
"已发现"等价于"已入队"。单一套合 `visited_set` 同时承担发现去重和入队去重，导致背压场景下逻辑冲突。

**设计方案**
1. **dspill 是 dispatch_queue 的磁盘扩展**，不是独立缓存池（append-only，字节游标回填）
2. 废除 pbin cursor 运行时回填（v15.5.8 已改用 dspill，文档需同步修正）
3. **visited_set 语义拆分为三态**：
   - `discovered_set`：已发现（已写 pbin），防重复发现
   - `enqueued_set`：已入队（dispatch_queue 或 dspill），防重复入队
   - `completed_set`：已完成（dpbin），防重复扫描
4. **统一 `enqueue()` 调度入口**：所有待扫描目录（恢复 pump、运行时新发现、dspill 回填）都走同一入口
5. dspill 写入策略：内存缓冲池 1000 条 / 1 秒刷盘，崩溃丢失可接受（pbin 是权威持久化）

**背压链路**
```
batch_processor 发现新目录
  → enqueue()
    → dispatch_queue < HIGH_WATER（10万）?
      → 是：入 dispatch_queue，标记 enqueued_set
      → 否：入 dspill 内存缓冲池，标记 enqueued_set
  → 主线程每轮检查 dispatch_queue 水位
    → < LOW_WATER（3万）：从 dspill 游标读取 5000 条 → 回填 dispatch_queue
```

---

### P0-005 spbin 纳入恢复路径

**问题描述**
续传只算 `pbin - dpbin`，不读 spbin。被熔断/跳过的目录可能永久丢失。

**根因分析**
spbin 不在恢复逻辑中。设备恢复后，这些目录无人重新入队。

**设计方案**
1. **spbin 格式扩展**：
   ```
   [path_len][path][reason: uint8_t][timestamp: time_t][device_key: 64 bytes]
   ```
2. **恢复时按 device_key 分组处理**：
   - `PERMISSION(4)` / `CIRCUIT_BREAKER(3)` / `POISON(5)` → 永久跳过
   - `PROBE_FAIL(1)` / `TIMEOUT(2)` → 检查最新 timestamp
     - 未超窗 → 保持跳过
     - 超窗 → 敢死队探测该设备（随机抽样子路径 `opendir → readdir → closedir`）
       - 成功 → 设备标记 NORMAL，整组入队
       - 失败 → timestamp 更新，指数退避（30min → 2h → 6h → 24h）
3. **spbin 保持 append-only**，正常退出时 compaction 清理已恢复条目
4. 探测成功不立即修改 spbin（避免随机写），而是在内存标记 RECOVERED，正常退出时重写 spbin 过滤

**关联项**: P1-002 errno 分类矩阵

---

### P0-006 MSG_DROP 正常路径禁止

**问题描述**
MSG_DROP 只销账不补救，若 BATCH 被 drop 则结果丢失。

**根因分析**
协议层允许丢结果。IPC 线程 → 主线程队列满时生成 MSG_DROP，而不是背压。

**设计方案**
1. **正常路径禁止 drop**，队列满时应背压
2. IPC 队列容量从 1024 扩至 **65536**（约 2MB 内存），使容量瓶颈永远在 dispatch_queue 而非 IPC 层
3. 主线程优先 drain：每次 `epoll_wait` 之前先把 IPC 队列排空
4. **MSG_DROP 降级为 panic 路径**：队列满直接 `log_fatal`，因为这在设计正常时不应发生
5. 两层背压收敛到一处：dispatch_queue + dspill。Worker 发送永远不被丢弃

---

### P0-007 RET_ERROR 状态机补全

**问题描述**
`BUSY -- RET_ERROR --> IDLE`，但 `pending_tasks` 是否减、目录是否重入队、是否写 spbin 均未定义。

**根因分析**
状态机缺漏。RET_ERROR 是设备级错误（EIO/ENODEV），不是目录问题。

**设计方案**
1. RET_ERROR → **pending_tasks--**（Worker 已释放）
2. 当前目录**不重入队**（避免重试风暴）
3. 写入 **spbin**，带原因码 `SP_REASON_PROBE_FAIL`
4. 目录任务状态机推进到 `DEVICE_WAITING`
5. 设备进入 `PROBING` 态，由 probe_scheduler 敢死队探测
6. 设备恢复后，spbin 中该设备所有目录统一重新入队（见 P0-005）

**关键规则**
- 不重试单个目录（设备级错误不是目录问题）
- 不立即重入队（避免重试风暴导致设备更差）
- 由设备恢复后的统一入队机制处理

---

### P0-008 Run manifest + baseline_eligible 原子切换

**问题描述**
上次扫描可能不完整（spbin 非空、dspill 未排空等），但没有机制阻止它成为盲信基准。

**根因分析**
缺少运行完整性标记。当前 `.config` 只存运行参数，不表达"本次运行是否完整"。

**设计方案**
1. `.config` 升级为独立 `{base}.manifest`（文本/JSON 格式，便于审计），包含：
   - `run_id`, `target_path`, `schema_version`, `tool_version`
   - `status`（Running / Success / Incomplete / Failed）
   - `started_at`, `finished_at`
   - 统计：dirs, files, skipped, errors, bytes
   - `spbin_count`, `dspill_offset`
   - `baseline_eligible`（布尔）
   - `baseline_run_id`, `baseline_completed_at`, `baseline_checksum`
   - 盲信命中率统计
   - `pbin_schema_version`
   - `output_checksum`
2. **`baseline_eligible = true` 的严格条件**（必须同时满足）：
   - `status == complete`（正常结束，非 Incomplete）
   - `spbin` 为空（或被熔断目录数 == 0）
   - `dspill` 已排空
   - `fpbin` 不存在或已转正
   - archive 校验通过（gzip footer 校验）
   - 输出尾部完整
3. **盲信扫描启动时**：读取 manifest → `baseline_eligible != true` → 拒绝运行，`exit(2)`
4. **原子切换**：
   - 新 archive 先写 `{base}.archive.new`
   - 校验通过后 `rename()` 覆盖旧 archive
   - 旧基准保留为 `{base}.archive.prev`
   - manifest 同步更新

**关键原则**
- 完整性由工具强制，时效性由业务控制
- 盲信结果 `baseline_eligible = false`（不能链式作为下次基准）

---

### P0-009 目录任务生命周期状态机

**问题描述**
当前只有"派发/完成"二元态，缺少中间状态定义，导致多个 P0 问题的根因无法精确描述。

**设计方案**
引入显式 10 态 + 4 异常分支：

```
                         目录 P 被 readdir 发现
[DISCOVERED] ──────────────────────────────────────────> [PERSISTED]
                                                              │
                                                              │ write_pbin 成功
                                                              ▼
                                                        [ENQUEUED]
                                                              │
                                                              │ dispatch_from_queue 弹出
                                                              ▼
                                                        [DISPATCHED]
                                                              │
                                                              │ CMD_SCAN 发送成功(epoch++)
                                                              ▼
                                                         [SCANNING]
                                                              │
                                    ┌───────────────────────┼───────────────────────┐
                                    │                       │                       │
                                    ▼                       ▼                       ▼
                              [ALL_BATCHES         [RETRYING]              [BACKOFF]
                               _RECEIVED]            (Worker 死亡)            (熔断退避)
                                    │                       │                       │
                                    │ Worker 替换成功        │ 退避到期              │
                                    └───────────────────────┼───────────────────────┘
                                    │                       │
                                    ▼                       ▼
                              [ALL_BATCHES                                        [DEVICE_WAITING]
                               _PROCESSED]                                         (设备 PROBING)
                                    │                                                │
                                    │ 输出线程确认                                    │ 设备恢复
                                    ▼                                                ▼
                              [OUTPUT_COMMITTED]                              [ENQUEUED]（重入队）
                                    │
                                    │ dpbin_append 成功
                                    ▼
                              [COMPLETED]
```

| 状态 | 含义 | 可转移 | dpbin？ |
|------|------|--------|---------|
| `DISCOVERED` | readdir 发现但尚未写 pbin | PERSISTED | 不写 |
| `PERSISTED` | 已写 pbin（崩溃可恢复） | ENQUEUED | 不写 |
| `ENQUEUED` | 在 dispatch_queue 或 dspill 中 | DISPATCHED | 不写 |
| `DISPATCHED` | 已发给 Worker，等待 SCANNING 确认 | SCANNING | 不写 |
| `SCANNING` | Worker 正在扫描 | ALL_BATCHES_RECEIVED / RETRYING / BACKOFF / DEVICE_WAITING | 不写 |
| `ALL_BATCHES_RECEIVED` | 收到该目录所有 BATCH + FINISH | ALL_BATCHES_PROCESSED | 不写 |
| `ALL_BATCHES_PROCESSED` | batch_processor 处理完所有条目 | OUTPUT_COMMITTED | 不写 |
| `OUTPUT_COMMITTED` | 输出线程确认该目录文件已落盘 | COMPLETED | 不写 |
| `COMPLETED` | 完整闭环 | 终态 | **写 dpbin** |
| `RETRYING` | Worker 死亡，等待替换重发 | ENQUEUED | 不写 |
| `BACKOFF` | 目录级熔断，指数退避 | ENQUEUED | 不写 |
| `DEVICE_WAITING` | 设备 PROBING，目录在 spbin 中等待 | ENQUEUED | 不写 |
| `SKIPPED_FAILED` | 永久跳过（PERMISSION/CIRCUIT_BREAKER/毒丸） | 终态 | 不写 |
| `UNKNOWN` | 异常终止，状态丢失 | 恢复时按 Reset 援救处理 | 不写 |

**关键规则**
- 只有 `COMPLETED` 写 dpbin
- `SKIPPED_FAILED` 阻止本次运行标记为完整（`baseline_eligible = false`）
- `OUTPUT_COMMITTED` 之前崩溃 → 恢复时该目录在 `discovered_set - completed_set` 中，重新扫描
- `SCANNING` 中崩溃 → 同上，Worker 的 BATCH 可能部分丢失，重新扫描

**关联项**: P0-001, P0-002

---

### P0-010 崩溃恢复矩阵 — Reset 援救机制

**问题描述**
当前恢复只覆盖 Worker 死亡一种情况，缺少 12+ 个崩溃点的精确恢复行为定义。

**根因分析**
试图枚举每个崩溃点的精确恢复行为，不可穷尽。

**设计方案**
不枚举崩溃点，统一采用 **Reset 援救机制**：
1. **核心原则**：任何目录只要状态 ≠ `COMPLETED`（未写 dpbin），就视为未扫描
2. **恢复时**：这些目录在 `discovered_set - completed_set` 中，重新入队
3. **re-scan 幂等**：已写 pbin 但未完成的目录，重新扫描是安全的
4. **输出截断**：恢复时输出文件截断到最后一个已确认 dpbin 对应的位置（或损坏尾部）
5. **spbin 按 P0-005 处理**：超窗探测，设备活了统一入队
6. **fpbin 按 P0-003 处理**：完整则转正，不完整整对抛弃重来
7. **不需要为每个崩溃点写恢复逻辑**——状态机终态唯一（`COMPLETED = 写 dpbin`），非终态统一重置

**覆盖的崩溃点**（非穷尽，统一处理）：
- Worker 发送 BATCH 前崩溃
- Worker 发送 BATCH 后、Master 读取前崩溃
- Master 读取后、去重前崩溃
- 去重后、pbin 落盘前崩溃
- pbin 落盘后、输出落盘前崩溃
- Worker 发送 FINISH 前崩溃
- FINISH 已处理但最后 BATCH 未处理
- dpbin 已写但子目录未持久化
- fpbin 转正前崩溃
- archive 压缩中崩溃
- 新基准覆盖旧基准时崩溃
- 输出线程崩溃
- Master 收到 SIGTERM/SIGINT

---

### P0-011 半增量跳过前提条件声明

**问题描述**
盲信跳过的前置条件、粒度、收益描述不准确，存在设计矛盾。

**澄清与结论**
1. **目录 mtime 传播性无关紧要**：盲信扫描直接采信 pbin 中记录的**文件级 mtime**，不依赖目录 mtime 是否向上传播
2. **reference_map 内存膨胀可接受**：盲信扫描不更新 reference_map（只读）。全量扫描时重建 reference_map，旧数据自然丢弃
3. **文件级粒度是默认行为**：目录本身仍须 `readdir + lstat`（发现新增/删除），但目录下的**已有文件**可盲信跳过 lstat。两者不冲突
4. **盲信 key 改为完整路径**：不依赖 `dev + ino`（不做 lstat 无法获取），以路径命中 reference_map
5. **两级扫描体系**：低频全量重扫（~2周建基准）+ 高频盲信扫描（2/6/12/24/48h复用基准）
6. **盲信结果不能链式作为基准**：`baseline_eligible = false`

---

### P0-012 epoch + waitpid 确认（旧 Worker 残留数据）

**问题描述**
SIGKILL 后旧 Worker 的 Scanner 线程可能仍在内核中完成最后的 write()，残留数据在 pipe 中。新 Worker 连接同一个 fd_data 后，Master 读到旧 Worker 残留的 BATCH 数据，导致输出污染。

**设计方案**
1. **epoch 机制**：
   - 每个 CMD_SCAN 附带递增 epoch（64 位原子计数器）
   - Worker 返回 BATCH/FINISH 携带 epoch
   - Master 校验 epoch 匹配，不匹配则丢弃（旧 Worker 残留数据）
2. **waitpid 确认**：
   - SIGKILL 后轮询 `waitpid(WNOHANG)` 直到旧进程回收
   - 通常 < 1ms，但必须有确认才 spawn 新 Worker
3. **pipe drain**：
   - CMD_REPLACE 时 IPC 线程先清空旧 pipe 读缓冲区
   - 或者关闭旧 fd，新 Worker 使用新 fd

**关键实现点**
- epoch 是 64 位原子递增，不会回绕
- 过期 epoch 的 BATCH 直接丢弃，不计入任何目录任务
- 丢弃时记 WARN 日志，便于审计
- 与 P0-001 的 ALL_BATCHES_RECEIVED 结合：过期 epoch 的 BATCH 不影响"批次完整性"判断

---

## P1 重要项（不阻塞 P0 编码，但需尽快设计确认）

### P1-001 恢复时加载 dspill 并集
**状态**：已纳入 P0-004 统一队列模型。dspill 是 dispatch_queue 的磁盘扩展，恢复时自动读取回填。无需单独处理。

### P1-002 errno 分类矩阵
**问题**：EINTR/ESTALE/ETIMEDOUT/EIO/ENOENT 未统一分类处理，导致设备级和条目级错误混淆。

**方向**：

| 错误码 | 分类 | 行为 |
|--------|------|------|
| EINTR | 瞬态信号中断 | 有界重试（3 次，间隔 1ms） |
| ESTALE | NFS stale handle | 按父目录 dirfd 重新解析；仍失败→设备嫌疑 |
| ETIMEDOUT/EIO | 设备级故障 | 喂给 probe_scheduler，计数熔断 |
| EACCES | 权限终态 | DIR_ERROR + SP_REASON_PERMISSION，不重试 |
| ENOENT/ENOTDIR | 竞态删除 | 静默跳过，记 debug 日志 |
| ENAMETOOLONG | 覆盖边界 | DIR_ERROR，不可静默 |
| ENOMEM | 本地资源耗尽 | log_fatal，终止运行 |

### P1-003 设备身份主键改为 (fsid, server, export)
**问题**：NFS failover 后 st_dev 变，熔断状态错挂或丢失。

**方向**：
1. 设备身份 = `(fsid, server, export_path)`，fsid 来自 `statfs()->f_fsid`
2. st_dev 仅用于运行时快速查找，启动时建立 st_dev → (fsid, server, export) 映射
3. spbin 中记录 (fsid, server, export) 而非仅 st_dev

### P1-004 毒丸目录清单（致死 3 次隔离）
**状态**：已纳入 P0-005 spbin 扩展。spbin reason 码增加 `POISON(5)`，同一目录致死 3 次直接进 POISON，永久跳过，不计入设备级错误统计。

### P1-005 设备级熔断 DEGRADED 灰度态
**问题**：ParaStor 单 OST 故障表现为局部 EIO，设备级不跳、目录级狂跳。

**方向**：
1. 错误聚合：单位时间内设备 EIO/ETIMEDOUT 目录数占比
2. >10% 目录报错 → DEV_STATE_DEGRADED（健康目录正常扫描，报错目录单独探测）
3. >50% → DEV_STATE_DEAD
4. 探活粒度细化：从 spbin 随机抽样多个子路径分别探活

### P1-006 IPC 协议显式字段编码 + protocol_version
**问题**：Header 是 TLV，但 payload 内部用 struct stat memcpy，锁死 ABI。

**方向**：
1. struct stat 展开为显式字段序列（st_ino(8) + st_mode(4) + st_nlink(4) + ...）
2. Header 增加 protocol_version(uint16_t)，当前为 1
3. 接收端校验 protocol_version，不匹配则 log_fatal

### P1-007 输出完整性语义声明
**问题**：输出是精确快照还是近似快照？是否允许重复？删除是否有 tombstone？

**方向**：
1. 全量扫描：at-least-once 快照，不保证 exactly-once
2. 盲信扫描：基准回放 + 新增发现，不保证变更检测、不保证删除可见
3. 退出码区分：0=完全完成、1=部分完成、2=严重失败
4. 失败目录下的旧基准不结转（避免假存在）

### P1-008 输出文件续写语义
**问题**：-c 续传时输出是追加还是覆盖？已写入行如何避免重复？

**方向**：
1. 输出文件续传时追加（基于 output_offset checkpoint）
2. 恢复时截断输出到最后一个已确认提交的 dpbin 偏移
3. 输出文件损坏检测：启动时校验输出文件大小与 dpbin 最后记录一致
4. -O 分片模式：记录每个分片的 output_offset，续传时从原分片继续

### P1-009 pbin 文本格式 → 二进制安全编码
**状态**：**不成立**。实际代码中 pbin 本来就是二进制格式，`path_len` 前缀编码天然支持任意字节。
**衍生问题**：二进制字段宽度平台相关 → 见 P3-001 平台兼容性检查。

### P1-010 盲信目录枚举失败的输出语义
**问题**：目录因设备错误无法 readdir，旧基准结转输出假存在，不结转则大规模漏输出。

**方向**：
1. 该目录及子树标记为 UNKNOWN/INCOMPLETE
2. 不计入 completed_set，不进 dpbin
3. 旧基准不结转（避免假存在）
4. 进 spbin 带原因码
5. 本次运行不能标记为"完整完成"

---

## P2 补充项（待设计确认，不阻塞编码）

### P2-001 一致性模型声明（快照隔离近似）
文档声明——不保证全局一致快照，每个目录的枚举+stat 原子，跨目录不保证。

### P2-002 孤儿 Worker 自裁（PR_SET_PDEATHSIG）
Worker 启动时 `prctl(PR_SET_PDEATHSIG, SIGTERM)`，Master 死后自动退出。

### P2-003 续传配置校验（版本、格式、标志）
续传时校验 target_path、format、VERSION、`--strict-nlink` 等关键字段一致。

### P2-004 进度文件位置声明
文档声明进度文件和输出文件建议放本地磁盘，不要与被扫描目录共用同一 NFS。

### P2-005 容量模型（内存、磁盘、吞吐）
给出每条路径平均长度、每条 stat 大小、12亿条目下内存公式、最低/典型/峰值内存、输出磁盘需求、IPC吞吐目标。

### P2-006 长路径 / 非 UTF-8 文件名 / 换行文件名处理
1. MAX_PATH_LENGTH 不应由 pipe 原子写反推，覆盖能力和协议分包应分离设计
2. 路径长度超限应显式记 DIR_ERROR 而非静默截断
3. 非 UTF-8 字节：保留原始字节，文档声明"原始字节输出"

### P2-007 挂载点 / 符号链接 / bind mount 策略
1. 是否跨越挂载点？是否只扫描根设备？
2. 符号链接作为文件输出还是跟随？
3. bind mount 如何处理？
4. 遍历身份与输出身份分离：遍历防环关注物理身份(dev+ino)，输出去重可包含路径

### P2-008 计数器、游标、偏移全部 64 位
文件数、目录数、字节数、分片序号、dspill 游标、pbin 游标、output_offset、BATCH 累计数、pending 计数全部明确要求 64 位。

### P2-009 全链路资源有界性论证
为每个队列/集合写明"满了怎么办"：fingerprint_set 达 80% 触发分片落盘、spbin_entries 上限 10 万、dspill 单文件上限 1GB 滚动。

### P2-010 自适应并发控制（AIMD）
基于 readdir/lstat 延迟 p50/p99 调整 Worker 数，AIMD 算法。

### P2-011 输出按大小分片
新增 `--output-slice-size` 参数，与 `--output-slice-lines` 二选一。

---

## P3 长期项

### P3-001 平台兼容性检查（续传 / 盲信扫描强制）
**问题**：pbin/fpbin/dpbin 的二进制字段宽度（size_t、dev_t、ino_t、time_t）与平台相关，跨架构恢复会错位。

**方向**：
1. 首次全量扫描时 `{base}.config` 写入系统架构签名：arch、endian、word_size
2. 续传或盲信扫描时读取 `.config` 中的架构签名，与当前环境比对
3. 不一致 → 拒绝续传，返回退出码 3，stderr 输出 `[FATAL] 进度文件架构不兼容...请使用 --runone 重新全量扫描`
4. `.config` 缺失（旧版本进度）→ 视为不兼容，强制 `--runone`
5. `glibc_version` 作为兼容性参考字段，输出 warning 但不拒绝

**状态**：Design.md §12.4 已更新

---

## 文档矛盾修正（不涉及代码，仅 Design.md 表述）

| 编号 | 问题 | 位置 | 修正方向 | 状态 |
|------|------|------|---------|------|
| DOC-001 | IPC 消息方向表错误：§7.2 混了 M→W 和 W→M | §7.2 | 拆成两个方向表 | 待修正 |
| DOC-002 | pbin 写入者前后不一致：§4.1 vs §5.1 | §4.1 / §5.1 | 明确 batch_processor 写 pbin，async_worker 只写输出 | 待修正 |
| DOC-003 | "pbin 记录所有扫描过的条目"应为"已发现" | §8.1 | 修正语义 | 已修正（v15.5.9） |
| DOC-004 | Monitor 与 Main 进程收割职责重复 | §5.1 / §5.4 | 明确唯一 owner | 待修正 |
| DOC-005 | "新文件/变更文件输出"与盲信语义矛盾 | §4.3 | 盲信模式下不存在"变更文件" | 待修正 |
| DOC-006 | "减少 90%+ I/O"没有论证 | §10 | 删除或补模型/测试数据 | 待修正 |
| DOC-007 | timeo=600 单位错误（应为 6000=600秒） | §12 | 修正挂载参数 | **已修正** |
| DOC-008 | intr 在 CentOS 7.4 无效，不应作为关键前提 | §12 | 移除 intr | **已修正** |
| DOC-009 | pbin 格式描述错误："文本格式"实际是二进制 | §8.1 | 修正为二进制格式 + 平台兼容性说明 | **已修正** |

---

---

# listfiles 架构设计文档

> 文档版本：v15.6.0  
> 最后更新：2026-08-11  
> 对应代码版本：`dev` 分支 `53ff3d6`  
> 前序版本：v15.5.9  

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

`listfiles` 是一个面向 **PB 级分布式存储 / 十亿级文件** 场景的递归目录扫描工具。核心功能是将目录树遍历结果（路径、stat 元数据等）输出为 CSV 或自定义格式文本，同时支持断点续传、半增量扫描、设备熔断、进度归档等生产级特性。

运行环境默认为 **NFS 挂载的分布式存储**（曙光 ParaStor、Ceph 等），网络抖动、设备离线、元数据节点过载是常态。设计以 **容错优先、可观测性优先** 为第一原则。

---

## 2. 核心架构框架

三句话说完骨架：

1. **进程隔离模型**：Master 进程管理 8 个独立 Worker 进程，通过三通道 pipe 通信。Worker 进程内 Scanner 线程负责阻塞 IO（readdir/lstat），IPC 线程负责心跳与通信。Worker 卡死在 NFS D-State 时可被 SIGKILL 替换，不拖垮 Master。

2. **SEDA 五阶段流水线**：目录发现 → 子目录枚举（Worker Scanner）→ Batch 去重处理（CPU 线程池）→ 任务分发（dispatch_queue）→ 输出写入（异步线程）。阶段间通过队列/管道解耦，Master 主线程充当纯消息总线。

3. **类 BFS 按层扫描**：扫描以"目录"为粒度单位，Worker 每次消费一个目录任务，返回该目录下所有子目录（进入下一轮队列）和文件（直接输出）。整棵树按层推进，不是 DFS 深度递归。

核心约束：
- **NFS soft,timeo=6000,retrans=3 挂载**（hard 挂载下 D-State 不可杀，`intr` 在 CentOS 7.4 内核无效）
- **同机同架构运行**（进度文件不跨架构、不跨端序，见 §12.4）
- **CentOS 7.4 / Bash 4.2 兼容**
- **25PB / 12 亿文件是基线**

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

---

## 4. 场景化流程

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
   │   └── CMD_SCAN(path, epoch) → Worker Scanner 线程
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
   │   ├── 去重: fingerprint(path+dev+ino) 查 discovered_set
   │   ├── 新目录: write_pbin() + enqueue()（统一入队入口）
   │   └── 新文件: record_path_batch_append() → 异步输出线程刷盘
   │
   ├── Main 收到 RET_FINISH
   │   ├── 校验 epoch 匹配，丢弃过期 epoch
   │   ├── pending_tasks--, Worker→IDLE
   │   └── 目录状态机推进 → ALL_BATCHES_RECEIVED → ... → COMPLETED → dpbin_append
   │
   └── 循环直到: pending_tasks==0 && dispatch_queue 空

4. 终止
   ├── stop 所有 Worker
   ├── finalize_archive()（压缩归档 pbin + spbin）
   ├── 删除 dpbin（本次完成，不再需要）
   ├── spbin compaction（清理已恢复条目）
   └── 输出统计、退出
```

### 4.2 初次扫描未完成的续传

**前提**：上次扫描异常终止（崩溃、OOM、SIGKILL），留下了未完成的进度文件。用户带 `-c` 启动。

**流程**：

```
1. 初始化（同 4.1）

2. 加载进度（restore_progress）
   ├── 读取 archive / 散落 pbin 分片 → 重建 discovered_set（已发现条目）
   ├── 读取 dpbin → 重建 completed_set（已完成目录）
   ├── 检查 fpbin + dfpbin 完整性（Footer 校验）
   │   ├── 完整 → fpbin 合并到 pbin，清空 fpbin/dfpbin
   │   └── 不完整 → 整对抛弃，dpbin 也清空（Reset 援救）
   ├── 读取 spbin → 按 device_key 分组，PERMISSION/CIRCUIT_BREAKER 永久跳过
   ├── 读取 dspill → 回填 dispatch_queue
   └── 差集 = discovered_set - completed_set = "待扫描目录" → dispatch_queue

3. 主循环运行
   ├── dispatch_from_queue(): 先消费恢复的目录
   ├── 运行中 Worker 返回新子目录 → 写 fpbin（恢复阶段）
   ├── 旧 pbin 消费完 → fpbin 转正 → 新子目录直接入队
   └── 逻辑同 4.1 的终止条件

4. 终止（同 4.1）
```

**关键差异**：
- `fpbin + dfpbin` 是可抛弃工作区。恢复期间新发现的目录先写入 fpbin，旧 pbin 消费完才转正
- 若 fpbin+dfpbin 不完整（崩溃中断），整对抛弃，重新从旧 pbin 恢复（不会无限套娃）
- Reset 援救：任何目录只要没写 dpbin（状态 ≠ COMPLETED），就视为未扫描

### 4.3 具备完整首次扫描结果后的盲信扫描

**前提**：已有一次完整扫描，且 `.config` 中 `baseline_eligible=true`。用户带 `-c --skip-interval=<秒>` 启动，**必须指定新的 progress 路径**。

**流程**：

```
1. 初始化
   ├── 解析命令行参数
   ├── 新 progress 路径必须为空（不存在 .config/.pbin 等）
   │   └── 非空 → exit(2)，提示"盲信扫描要求空 progress 目录，请指定新路径"
   └── 加载旧基准（--reference-base 或推断上次 progress）
       └── 从旧 pbin 重建 reference_map（路径 → {mtime, size, mode, d_type}）

2. 主循环运行（带盲信检查）
   ├── dispatch_from_queue(): 弹出目录
   ├── CMD_SCAN(path, epoch) → Worker
   │
   ├── Worker 扫描该目录
   │   ├── opendir(path) → readdir
   │   ├── 对每个条目:
   │   │   ├── 构造完整路径
   │   │   ├── 查 reference_map:
   │   │   │   ├── 命中:
   │   │   │   │   └── 文件: 不执行 lstat，复用 reference_map 中记录的旧 stat 数据
   │   │   │   │   └── 目录: 仍须 readdir 枚举子目录（子目录发现不可盲信跳过）
   │   │   │   └── 未命中 或 mtime 在 skip_interval 内:
   │   │   │       └── 正常 lstat，新数据进入 BATCH
   │   └── batch 满 → send_batch
   │
   ├── Main 收到 RET_BATCH
   │   ├── 去重（discovered_set，防环）
   │   ├── 新目录: write_pbin(增量) + enqueue()
   │   └── 新文件/变更文件: record_path_batch_append() → 输出
   │
   └── 终止条件同 4.1

3. 终止
   ├── finalize_archive()（只归档增量 pbin）
   ├── 旧基准不更新（reference_map 来自旧 pbin，本轮只读）
   ├── 新 .config baseline_eligible = false（盲信结果不能作为下次盲信基准）
   └── 输出包含：本轮新增/变更条目 + 盲信跳过的旧条目
```

**关键差异**：
- **新 progress 路径隔离**：盲信扫描的增量 pbin 与旧基准物理分离，防止污染
- **纯路径 reference_map**：盲信时不 lstat，无法获取 dev+ino，key 从 fingerprint 改为完整路径
- **skip_interval 文件级粒度**：每个文件独立判断是否盲信（比较 pbin 记录的上次 mtime），目录本身仍须 readdir + lstat
- **输出是近似快照**：不检测已有条目变更、不检测删除、不保证 exactly-once

### 4.4 忽略原始进度的再次全新扫描

**前提**：用户想重新全量扫描。

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
                                  │                           SIGKILL + waitpid + drain + replace
                                  │ CMD_SCAN (send success, epoch++)
                                  ▼
                                [BUSY] ──────────────> [DEAD]  (heartbeat_timeout)
                                  │      RET_DEV_TIMEOUT         SIGKILL + waitpid + drain + replace
                                  │      (Scanner self-detected)
                                  │
                                  │ RET_BATCH (multiple, 携带 epoch)
                                  │ RET_FINISH (携带 epoch)
                                  ▼
                                [IDLE]
                                  │
                                  │ RET_ERROR
                                  ▼
                                [IDLE]  (device fuse, 目录写 spbin, 不重入队)
```

| 当前状态 | 触发条件 | 下一状态 | 动作 |
|---------|---------|---------|------|
| INITIALIZING | startup_timeout (60s) | DEAD | SIGKILL + waitpid + drain + replace |
| INITIALIZING | RET_READY | IDLE | 开始心跳计时 |
| IDLE | CMD_SCAN (发送成功) | BUSY | pending_tasks++, epoch++ |
| IDLE | heartbeat_timeout (120s) | DEAD | SIGKILL + waitpid + drain + replace |
| BUSY | RET_FINISH + epoch 匹配 | IDLE | pending_tasks-- |
| BUSY | RET_ERROR | IDLE | device fuse，目录写 spbin(PROBE_FAIL)，设备进 PROBING，不重入队 |
| BUSY | heartbeat_timeout (120s) | DEAD | SIGKILL + waitpid + drain + replace |
| BUSY | RET_DEV_TIMEOUT | DEAD | SIGKILL + waitpid + drain + replace |
| DEAD | cleanup + replace | INITIALIZING | spawn 新 Worker，IPC 线程重置 FSM |

**关键变更（v15.6.0）**：
- **epoch 机制**：每个 CMD_SCAN 附带递增 epoch，Worker 返回 BATCH/FINISH 携带 epoch。Master 丢弃过期 epoch，防止旧 Worker 残留数据污染
- **waitpid + drain**：SIGKILL 后轮询 waitpid(WNOHANG) 直到旧进程回收，然后 drain 旧 pipe 残留
- **RET_ERROR 不重入队**：设备级错误（EIO/ENODEV）触发 spbin 记录，设备进 PROBING，目录不立即重试

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
    //    - RET_BATCH(epoch)  → epoch 匹配 ? thread_pool 提交去重 : 丢弃
    //    - RET_FINISH(epoch) → epoch 匹配 ? pending_tasks--, 状态机推进 : 丢弃
    //    - RET_DEAD   → cleanup + waitpid + drain + spawn + send_replace_to_ipc
    //    - RET_ERROR  → device_mgr_mark_probing, 目录写 spbin, 不重入队
    //    - RET_DEV_TIMEOUT → 同 RET_DEAD
    //    - RET_READY  → Worker→IDLE
    // 3. drain_completed_batches（线程池完成回调）
    // 4. 泵送历史 pbin 目录（恢复时）
    // 5. 收割僵尸进程
    // 6. dispatch_from_queue（派发 dispatch_queue 中的任务）
    // 7. dspill 回填（dispatch_queue < LOW_WATER 时）
    // 8. 终止条件检查：pending_tasks==0 && dispatch_queue 空 && dspill 排空 && 无历史可泵送
}
```

### 5.5 目录任务生命周期状态机

```
                         目录 P 被 readdir 发现
[DISCOVERED] ──────────────────────────────────────────> [PERSISTED]
                                                              │
                                                              │ write_pbin 成功
                                                              ▼
                                                        [ENQUEUED]
                                                              │
                                                              │ dispatch_from_queue 弹出
                                                              ▼
                                                        [DISPATCHED]
                                                              │
                                                              │ CMD_SCAN 发送成功(epoch++)
                                                              ▼
                                                         [SCANNING]
                                                              │
                                    ┌───────────────────────┼───────────────────────┐
                                    │                       │                       │
                                    ▼                       ▼                       ▼
                              [ALL_BATCHES         [RETRYING]              [BACKOFF]
                               _RECEIVED]            (Worker 死亡)            (熔断退避)
                                    │                       │                       │
                                    │ Worker 替换成功        │ 退避到期              │
                                    └───────────────────────┼───────────────────────┘
                                    │                       │
                                    ▼                       ▼
                              [ALL_BATCHES                                        [DEVICE_WAITING]
                               _PROCESSED]                                         (设备 PROBING)
                                    │                                                │
                                    │ 输出线程确认                                    │ 设备恢复
                                    ▼                                                ▼
                              [OUTPUT_COMMITTED]                              [ENQUEUED]（重入队）
                                    │
                                    │ dpbin_append 成功
                                    ▼
                              [COMPLETED]
```

| 状态 | 含义 | 可转移 | dpbin？ |
|------|------|--------|---------|
| `DISCOVERED` | readdir 发现但尚未写 pbin | PERSISTED | 不写 |
| `PERSISTED` | 已写 pbin（崩溃可恢复） | ENQUEUED | 不写 |
| `ENQUEUED` | 在 dispatch_queue 或 dspill 中 | DISPATCHED | 不写 |
| `DISPATCHED` | 已发给 Worker，等待 SCANNING 确认 | SCANNING | 不写 |
| `SCANNING` | Worker 正在扫描 | ALL_BATCHES_RECEIVED / RETRYING / BACKOFF / DEVICE_WAITING | 不写 |
| `ALL_BATCHES_RECEIVED` | 收到该目录所有 BATCH + FINISH | ALL_BATCHES_PROCESSED | 不写 |
| `ALL_BATCHES_PROCESSED` | batch_processor 处理完所有条目 | OUTPUT_COMMITTED | 不写 |
| `OUTPUT_COMMITTED` | 输出线程确认该目录文件已落盘 | COMPLETED | 不写 |
| `COMPLETED` | 完整闭环 | 终态 | **写 dpbin** |
| `RETRYING` | Worker 死亡，等待替换重发 | ENQUEUED | 不写 |
| `BACKOFF` | 目录级熔断，指数退避 | ENQUEUED | 不写 |
| `DEVICE_WAITING` | 设备 PROBING，目录在 spbin 中等待 | ENQUEUED | 不写 |
| `SKIPPED_FAILED` | 永久跳过（PERMISSION/CIRCUIT_BREAKER/毒丸） | 终态 | 不写 |
| `UNKNOWN` | 异常终止，状态丢失 | 恢复时按 Reset 援救处理 | 不写 |

**关键规则**：
- **只有 COMPLETED 写 dpbin**
- **SKIPPED_FAILED 阻止本次运行标记为完整**（baseline_eligible = false）
- **OUTPUT_COMMITTED 之前崩溃** → 恢复时该目录在 `discovered_set - completed_set` 中，重新扫描
- **SCANNING 中崩溃** → 同上，Worker 的 BATCH 可能部分丢失，重新扫描

---

## 6. 模块详细设计

### 6.1 core — 配置与生命周期

**Config** 全局配置字段（关键项）：

| 字段 | 说明 | 默认值 |
|------|------|--------|
| `target_path` | 扫描根路径 | — |
| `output_file` | 输出文件 | — |
| `progress_base` | 进度文件前缀 | — |
| `reference_base` | 盲信基准路径（旧 progress） | — |
| `continue_mode` | 断点续传 | false |
| `skip_interval` | 盲信扫描阈值（秒） | 0（关闭） |
| `batch_size` | Worker batch 大小 | 1024 |
| `estimated_files` | HashSet 预分配 | 1000 万 |
| `worker_count` | Worker 数 | 8 |
| `heartbeat_timeout` | 心跳超时 | 120s |
| `strict_nlink` | nlink oracle | false |

**AppContext** 运行时上下文核心字段：
- `discovered_set`：已发现（已写 pbin），防重复发现
- `enqueued_set`：已入队（dispatch_queue 或 dspill），防重复入队
- `completed_set`：dpbin 恢复时的已完成集合
- `reference_map` / `reference_set`：盲信扫描基准（路径 → stat 数据）
- `worker_pool`、`probe_scheduler`、`dev_mgr`：进程/设备管理
- `dispatch_queue`：Stage 3→4 队列
- `ipc_cmd_queues[8]`、`ipc_ret_queues[8]`：无锁消息队列
- `pending_tasks`（原子）、`pending_batches`（原子）：任务计数
- `dspill_fp`：派发兜底文件
- `dspill_buf`：dspill 内存缓冲池（1000 条 / 1 秒刷盘）
- `redispatch_backoff_until[8]`：退避时间戳
- `epoch_counter`（原子 64 位）：目录派发 epoch 计数器

### 6.2 ipc — 进程间通信

**三通道 Pipe 模型**（v15.0.0）：

| 通道 | 方向 | 语义 | 写入者 | 阻塞策略 |
|------|------|------|--------|----------|
| `fd_cmd` | M→W | SCAN / STOP | Master | 阻塞写，非阻塞读 |
| `fd_data` | W→M | BATCH（大 payload） | Scanner 线程 | 阻塞写，非阻塞读 |
| `fd_ctrl` | W→M | HEARTBEAT/ERROR/EXIT/READY/FINISH | IPC 线程 | 阻塞写，非阻塞读，< PIPE_BUF |

**无锁队列**（Master ↔ IPC 线程）：容量 **65536** 条，64 位 CAS head/tail。

**FSM 续传**（v15.4.0）：跨 `epoll_wait` 的可恢复读取，状态 `IPC_READ_IDLE → HDR → PAYLOAD → FOOTER`。

**epoch 机制**（v15.6.0）：
- 每个 CMD_SCAN 附带递增 epoch（64 位原子计数器）
- Worker 返回 BATCH/FINISH 携带 epoch
- Master 校验 epoch 匹配，不匹配则丢弃（旧 Worker 残留数据）

### 6.3 scan — 扫描引擎

**Worker Scanner**（`worker_scanner.c`）：
- `opendir(path) → readdir 循环 → lstat 每个条目 → 区分文件/目录`
- 文件：收集进 batch → 满 `batch_size` 时 `send_batch(fd_data, epoch)`
- 目录：收集进 batch → 由 batch_processor 后续处理
- 盲信检查（若启用）：`reference_map` 中路径存在且 mtime 超窗 → 不执行 lstat，直接复用旧 stat 数据
- `scanner_progress_tick()` 每 5s 更新，防止 NFS 大目录误判

**Batch Processor**（`batch_processor.c`）：
1. 解析 BATCH 数据（校验 epoch）
2. 计算 `path+dev+ino` 的 128-bit MD5 fingerprint，查 `discovered_set` 防环
3. 新目录：`write_pbin()` + `enqueue()`（统一入队入口）
4. 新文件：`record_path_batch_append()` → 异步输出线程

**统一队列模型**（v15.6.0）：
- `enqueue()` 是所有待扫描目录的唯一入口
- 查 `enqueued_set` 去重
- dispatch_queue < HIGH_WATER（10万）→ 入内存队列
- dispatch_queue ≥ HIGH_WATER → 写 dspill（内存缓冲 1000 条 / 1 秒刷盘）
- dspill 是 dispatch_queue 的磁盘扩展，append-only，字节游标回填

**Dispatch**（`dispatch.c`）：
- `dispatch_from_queue()`：轮询 IDLE Worker，遇退避期 skip
- `cleanup_dead_worker_slot()`：死亡 Worker 的 current_path 按状态机推进（RETRYING → ENQUEUED）

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
   ├──── fd_cmd ────> [SCAN path, epoch]   │
   ├──── fd_cmd ────> [STOP]               │
   │                                       │
   │<──── fd_data ─── [BATCH records]      │ Scanner 线程
   │<──── fd_data ─── [BATCH records]      │
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

**返回（IPC → Main）**：`RET_BATCH`、`RET_HEARTBEAT`、`RET_ERROR`、`RET_DEAD`、`RET_EXIT`、`RET_DEV_TIMEOUT`、`RET_READY`、`RET_FINISH`、`RET_ENTRY_ERROR`

**注意**：v15.6.0 已废除 `MSG_DROP`。IPC 队列容量从 1024 扩至 65536，满时触发 `log_fatal`（设计正常时不应发生）。

### 7.4 FSM 续传协议

跨 `epoll_wait` 调用的可恢复读取：`IPC_READ_IDLE → HDR → PAYLOAD → FOOTER`。`EAGAIN` 时不释放 buf、不重置 `nread`。`CMD_REPLACE` 时重置 FSM。

---

## 8. 数据持久化模型

### 8.1 pbin — 进度分片（全量扫描记录）

记录**所有扫描过的条目**（目录 + 文件），每条包含：路径、设备号、inode、mtime、d_type。**二进制格式**，每条记录结构为：
```
[path_len: size_t][path: N bytes][dev: dev_t][ino: ino_t][mtime: time_t][d_type: unsigned char]
```
每 10 万行切分，命名 `{base}_000000.pbin`。分片末尾写 Footer 自描述（记录数 + 校验）。

**平台兼容性约束**：pbin/fpbin/dpbin 的二进制字段宽度（`size_t`、`dev_t`、`ino_t`、`time_t`）与平台相关。**进度文件仅在同架构、同机器上可续传**，不保证跨 x86_64/aarch64、不保证跨 32/64 位、不保证跨大端/小端。

**为什么同时记录文件**：pbin 是盲信扫描的基准来源。盲信时从旧 pbin 重建 `reference_map`（路径 → mtime/d_type）。若 pbin 不记录文件，盲信对文件无从谈起。

**目录与文件的分流策略**：
- 正常扫描阶段（`HIST_PUMP_NEW/DONE`）：目录和文件都写入 pbin
- 恢复旧 pbin 阶段（`HIST_PUMP_OLD`）：新发现的目录写入 fpbin，文件仍写入 pbin
- 盲信扫描阶段：命中 reference_map 的旧条目不写新 pbin；未命中/变更条目写入增量 pbin

### 8.2 dpbin — 完成日志

本次会话的"已完成目录集合"。恢复时 `discovered_set - completed_set = 待扫描目录`。只分片、不归档，正常结束后删除。

**关键变更（v15.6.0）**：dpbin 写入时机从"收到 FINISH"延迟到"目录任务状态机达到 COMPLETED"（即 OUTPUT_COMMITTED 确认后）。

### 8.3 fpbin / dfpbin — 恢复临时缓存原子对

**fpbin**：恢复期间新发现的子目录先写入 fpbin（而非直接入队）。
**dfpbin**：记录 fpbin 阶段已完成的父目录（格式同 dpbin）。

**流转**：
```
恢复阶段：
  1. 检查 fpbin + dfpbin 完整性（Footer 校验通过）
     ├── 完整 → fpbin 合并到 pbin，清空 fpbin/dfpbin，hist_pump_state = HIST_PUMP_NEW
     └── 不完整 → 整对抛弃，从旧 pbin 重新恢复
  2. 旧 pbin 消费完 → hist_pump_state = HIST_PUMP_DONE
  3. fpbin 内容全部转入 dispatch_queue（转正）
  4. 此后新发现的子目录直接入队（不走 fpbin）
```

**可抛弃语义**：`fpbin + dfpbin` 是临时工作区。不完整时整对抛弃、重新恢复，不会无限套娃（总是回退到旧 pbin）。

### 8.4 spbin — 跳过记录

**格式**（v15.6.0 扩展）：
```
[path_len: size_t][path: N bytes][reason: uint8_t][timestamp: time_t][device_key: 64 bytes]
```

| reason 值 | 含义 | 恢复行为 |
|-----------|------|---------|
| `PROBE_FAIL(1)` | 设备探测失败 | 超窗后敢死队探测，设备活则入队 |
| `TIMEOUT(2)` | 心跳超时 | 同上 |
| `CIRCUIT_BREAKER(3)` | 目录级熔断 | 永久跳过 |
| `PERMISSION(4)` | 权限拒绝 | 永久跳过 |
| `POISON(5)` | 毒丸目录（致死 3 次） | 永久跳过 |

**恢复行为**：
1. 按 `device_key` 分组
2. PERMISSION/CIRCUIT_BREAKER/POISON → 永久跳过
3. PROBE_FAIL/TIMEOUT → 检查最新 timestamp
   - 未超窗 → 保持跳过
   - 超窗 → 敢死队探测该设备（随机抽样子路径）
     - 成功 → 设备标记 NORMAL，整组入队
     - 失败 → timestamp 更新，指数退避（30min→2h→6h→24h）

**spbin 保持 append-only**，正常退出时 compaction 清理已恢复条目。

### 8.5 dspill — 派发兜底（v15.5.8 → v15.6.0 统一队列模型）

**dspill 是 dispatch_queue 的磁盘扩展**，不是独立缓存池。

**数据流**：
```
batch_processor 发现新目录
  → enqueue()
    → dispatch_queue < HIGH_WATER（10万）?
      → 是：入 dispatch_queue，标记 enqueued_set
      → 否：入 dspill 内存缓冲池（1000条/1秒刷盘），标记 enqueued_set
  → 主线程每轮检查 dispatch_queue 水位
    → < LOW_WATER（3万）：从 dspill 游标读取 5000 条 → 回填 dispatch_queue
```

**特性**：
- append-only，无轮转无删除
- 崩溃后从头读，`enqueued_set` 去重
- 不需要 Footer（流式读取，EOF 即排空）

### 8.6 archive — 压缩归档

gzip 压缩的 pbin 块 + spbin 块。`block_type = 0/1` 区分。

**平台兼容性约束**：archive 中的 pbin 块继承同平台的二进制字段宽度，恢复时必须校验架构一致性（见 §12.4）。

---

## 9. 故障处理与容错

### 9.1 Worker 死亡与替换

```
IPC 线程检测超时/error/hup
    ├── SIGKILL Worker
    ├── waitpid(WNOHANG) 轮询直到回收
    ├── drain 旧 pipe 残留
    ├── close fds, epoll DEL
    └── send_return(RET_DEAD)
        ▼
Main: cleanup_dead_worker_slot() → current_path 状态机推进 RETRYING
      worker_pool_replace() → spawn 新 Worker
      send_replace_to_ipc() → IPC 线程更新 fd/pid，重置 FSM
```

### 9.2 设备级熔断

`RET_ERROR` → `dev_mgr_mark_probing()` → 敢死队探活 → 成功则 alive，失败则指数退避 → `PROBE_MAX_RETRIES` 次后 `CONDEMNED`。

**RET_ERROR 处理（v15.6.0）**：
- pending_tasks--（Worker 已释放）
- 当前目录**不重入队**
- 写入 spbin，带原因码 `SP_REASON_PROBE_FAIL`
- 设备进入 `PROBING` 态
- 目录任务状态机推进到 `DEVICE_WAITING`

### 9.3 目录级熔断与退避

同一目录连续 DEV_TIMEOUT 超过 `CIRCUIT_BREAKER_THRESHOLD (10)` 则跳过。redispatch 指数退避：1 次 30s、2 次 120s、≥3 次 300s。

毒丸机制（v15.6.0）：同一目录因任何原因（DEV_TIMEOUT、ERROR、ENTRY_ERROR 中的 EIO/ETIMEDOUT）导致 Worker 死亡累计 3 次，直接进隔离清单（`POISON`），不计入设备级错误统计。

### 9.4 NFS 大目录防误判

六层防御：HEARTBEAT_TIMEOUT 120s、熔断阈值 10、时间驱动 tick 5s、opendir tick、send_batch tick、redispatch 退避。

### 9.5 扫描完整性断言

- nlink oracle（`--strict-nlink`）：`st_nlink - 2` 应等于子目录数
- dspill 必须排空到 EOF
- 熔断清单非空 → 非零退出码
- spbin 残留未恢复 → baseline_eligible = false

### 9.6 Reset 援救机制（v15.6.0）

**核心原则**：不枚举崩溃点，统一采用 Reset 援救。

```
任何目录只要状态 ≠ COMPLETED（未写 dpbin）
  → 恢复时视为未扫描
  → 从 discovered_set 重新入队
  → 重新扫描（re-scan 幂等，pbin 去重保证不重复输出）

输出截断：
  → 恢复时输出文件截断到最后一个已确认 dpbin 对应的 offset

spbin 按 §8.4 处理：超窗探测，设备活了统一入队
fpbin 按 §8.3 处理：完整则转正，不完整整对抛弃重来
```

**退出码语义**：
- `0`：完全完成（所有目录 COMPLETED，spbin 空，dspill 排空）
- `1`：部分完成（有 SKIPPED_FAILED，baseline_eligible = false）
- `2`：严重失败（Master 崩溃、无法恢复）
- `3`：架构不匹配（§12.4）

---

## 10. 性能考量

| 优化点 | 效果 |
|--------|------|
| 盲信跳过（有基准时） | 减少已存在文件的 lstat I/O |
| HashSet 预分配 | 避免 rehash |
| 8MB 输出缓冲 | 减少 write syscall |
| 批量 record_path | 减少 fwrite |
| dispatch_queue 环形缓冲 | O(1) pop |
| 去重线程池 | CPU 并行 |
| dspill 统一背压 | 内存 hard cap 10 万条，磁盘兜底 |
| IPC 队列 65536 | 消除 MSG_DROP，两层背压收敛到一处 |

---

## 11. 安全考量

- `MAX_PATH_LENGTH = 4088`，确保原子写入
- `payload_len > 100MB` 时 `log_fatal`
- Worker 替换时 IPC 线程 close fd + drain 残留
- 僵尸进程 `waitpid(-1, NULL, WNOHANG)`
- 进度文件仅同架构可读（§12.4 强制校验）

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

# 续传（同机同架构）
./listfiles -p /public2/data -o /tmp/output.txt -f /tmp/progress -c

# 盲信扫描（需已有完整基准，同机同架构，新 progress 路径）
./listfiles -p /public2/data -o /tmp/output.txt -f /tmp/progress_blind -c --skip-interval=604800 --reference-base=/tmp/progress

# 强制重跑（覆盖旧进度）
./listfiles -p /public2/data -o /tmp/output.txt -f /tmp/progress --runone
```

### 平台兼容性检查（续传 / 盲信扫描强制）

续传或盲信扫描时，程序读取 `{base}.config` 中的系统架构签名，与当前运行环境比对：

| 字段 | 说明 | 不一致时的行为 |
|------|------|---------------|
| `arch` | CPU 架构（x86_64 / aarch64） | 拒绝续传，exit(3)，要求 `--runone` |
| `endian` | 字节序（little / big） | 拒绝续传，exit(3)，要求 `--runone` |
| `word_size` | 字长（32 / 64） | 拒绝续传，exit(3)，要求 `--runone` |
| `glibc_version` | glibc 版本（兼容性参考） | 输出 warning，不拒绝 |

`.config` 在首次全量扫描时自动生成。若文件缺失（旧版本进度），视为不兼容，强制 `--runone`。

**退出码 3**：架构不匹配时 stderr 输出 `[FATAL] 进度文件架构不兼容：期望 x86_64/little/64，实际 aarch64/little/64。请使用 --runone 重新全量扫描。`

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
| **v15.6.0** | **2026-08** | **目录任务生命周期状态机 + fpbin/dfpbin 原子对 + 统一队列模型 + epoch 机制 + 平台兼容性检查** | **漏扫风险闭环、背压收敛、残留数据防护、跨架构安全** |
