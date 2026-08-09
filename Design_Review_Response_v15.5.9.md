# Design.md 评审答复 —— v15.5.9 设计评审报告

> 评审来源：Jimmy（登月者5063）  
> 答复者：Elune  
> 时间：2026-08-09  
> 范围：Design.md §1–§12，以及潜在后续章节已覆盖内容  
> 约束：仅谈原理与设计，不涉代码实现

---

## 一、总评

评审的框架判断正确：主流程没有方向性错误，漏扫风险集中在**边缘语义**——错误分类学、提交顺序、续传边界、增量跳过契约。这些问题确实属于"主流程压测全绿，生产静默缺数据"的类型。

以下逐项给出设计层面的答复或承认缺陷。

---

## 二、漏扫向量清单 —— 逐项答复

### 1. 半增量跳过粒度（严重度：P0）

**评审指出**：目录 mtime 不向上传播，若按"子树根 mtime 未变 → 整棵子树跳过"，深层变更必漏。

**答复**：**承认，设计文档确实未明确跳过粒度。**

当前实现的真实跳过判定是：
- **文件级盲信**：`reference_map` 按 `(fingerprint, mtime)` 比对。若目录内某文件 mtime 变化，该文件会被重新 lstat 并输出。
- **目录级递归**：`readdir` 每个目录不可省，但子目录本身也走同样的盲信检查——即每个目录节点独立判定是否跳过其子树。

但这仍有问题：
- 若 `/a/b/c` 内新增文件，`/a/b` 的 mtime 会变（目录 mtime 在多数文件系统上**会**因子项增删而更新），但 `/a` 的 mtime **不会**变。
- 因此 `/a` 会被盲信跳过，其直接条目（文件）不会重新扫描，但 `/a/b` 作为子目录会被发现并独立判定——这不会漏扫 `/a/b/c` 的新增文件。

**真正的风险**：如果文件系统（或挂载选项）不更新目录 mtime 于子项变动（如某些 NAS 的 noatime 组合），则目录级盲信会漏掉深层变更。

**设计决策修正**：
1. 盲信跳过**默认关闭**，仅在用户显式传入 `--skip-interval` 时开启。
2. 文档需明确声明：盲信跳过依赖"文件系统保证目录 mtime 随子项变动而更新"，若该前提不成立（如 noatime、某些分布式存储），则不应启用。
3. 收益模型修正：半增量的真实收益上限是"省下已存在文件的 lstat + 输出 I/O"，`readdir` 本身确实省不掉。

---

### 2. 提交顺序不变量（严重度：P0）

**评审指出**：若 dpbin/指纹先提交、输出后落盘，崩溃后增量会跳过该目录，导致条目永远丢失。完整性断言核对"扫描数 == dpbin 数"发现不了这种丢失。

**答复**：**承认，这是当前设计最致命的持久化缺陷。**

当前流水线顺序是：
1. batch_processor 解析 BATCH → 去重 → 写 pbin（已发现目录）
2. 新目录 dispatch_queue_push → 后续派发
3. 文件条目 record_path_batch_append → 异步输出线程刷盘
4. process_completed_batch（Worker 返回 FINISH 后）→ dpbin_append（标记完成）

问题在于第 3 步和第 4 步之间没有跨崩溃的原子性保证。async_worker 的 8MB 缓冲区可能未刷盘，但 dpbin 已记完成。

**设计修正方案**：
- **引入输出偏移 checkpoint**：每批输出完成后，async_worker 返回已确认刷盘的文件偏移量。主线程在 `process_completed_batch` 中等待该偏移量 ≥ 本批次最后一条记录的偏移后，才执行 `dpbin_append`。
- **或者**：将 dpbin 提交推迟到 `finalize_archive` 阶段（程序正常终止时统一刷盘），但这会牺牲续传粒度——崩溃后需要重扫更多目录。
- **推荐方案**：采用"偏移量屏障"模式。dpbin 每条记录附加 `(path, output_offset)`，恢复时截断输出文件到最后一个 dpbin 已确认偏移，保证 at-least-once 语义下的幂等性。

**输出契约声明**：当前设计默认 at-least-once，不保证 exactly-once。文档需明确声明这一点，并建议消费者端做去重。

---

### 3. 续传 frontier 的完整性（严重度：P1）

**评审指出**：frontier 分布在 dispatch_queue 内存态、dspill、已派发未完成的、BATCH 在飞的四个地方。若 FSM 只从 pbin/dpbin 重建而漏了 dspill，恢复后永久丢失。

**答复**：**部分承认。**

dspill 的设计意图是"运行级追加文件"，无轮转无删除，字节游标线性读取。正常终止时 dspill 应该被排空到 EOF（v15.5.8 硬性断言）。若异常终止：
- dspill 文件本身不会丢失（追加写，无删除）
- 问题在于恢复逻辑是否**读取** dspill

当前恢复流程（`restore_progress`）：
1. 加载 archive + 散落 pbin 分片 → 重建已发现目录集合
2. 加载 dpbin → 标记已完成目录
3. `pbin - dpbin = 差集` → pump 到 dispatch_queue
4. **dspill 不在恢复路径中**

这是设计缺陷。dspill 中的目录是"已发现但因背压被跳推的"，它们在 pbin 中已有记录（因为 batch_processor 先写 pbin 再 push dispatch_queue），所以理论上 pbin 已覆盖。但若 pbin 滑动窗口游标追过被轮转删除的分片，这些目录确实可能从 pbin 中"蒸发"——这正是 dspill 存在的原因。

**设计修正**：恢复时必须将 dspill 并集进 frontier。具体：
- `restore_progress` 增加 `load_dspill_entries()` 步骤
- dspill 中的路径与 pbin 差集做并集，统一 pump
- 启动时检查 dspill 非空且未读完 → 警告并自动加载

---

### 4. 错误分类学缺失（严重度：P1）

**评审指出**：EINTR/ESTALE/ETIMEDOUT/EIO/ENOENT 未分类处理，任何"出错就跳过"都是漏扫发生器。

**答复**：**承认，文档 §8 未覆盖错误分类学。**

当前代码中对 errno 的处理是分散且不一致的：
- `ETIMEDOUT/EIO` → 设备级熔断（probe_scheduler）
- `EACCES` → v15.5.7 后作为目录级错误上报（`DIR_ERROR`）
- `ENOENT/ENOTDIR` → 竞态跳过（目录在扫描期间被删除）
- `EINTR` → 部分路径有重试，部分没有
- `ESTALE` → 未单独处理，落入默认路径

**设计修正**：建立统一的 errno 分类矩阵：

| 错误码 | 分类 | 行为 |
|--------|------|------|
| `EINTR` | 瞬态信号中断 | 有界重试（最多 3 次，间隔 1ms） |
| `ESTALE` | NFS stale file handle | 先按父目录 dirfd 重新解析；若仍失败→设备嫌疑 |
| `ETIMEDOUT/EIO` | 设备级故障 | 喂给 probe_scheduler，计数熔断 |
| `EACCES` | 权限拒绝 | 目录级 `DIR_ERROR`，进 spbin 带原因码 |
| `ENOENT/ENOTDIR` | 竞态删除 | 静默跳过（记 debug 日志），不惩罚设备 |
| `ENAMETOOLONG` | 路径过长 | `DIR_ERROR`，不可静默 |
| `ENOMEM` | 本地资源耗尽 | `log_fatal`，终止运行 |

文档 §8.4 需新增"错误分类与处置策略"小节。

---

### 5. 指纹碰撞（严重度：P1）

**评审指出**：64 位指纹在十亿级场景下碰撞概率不可忽略，后果是静默跳过已变更目录。

**答复**：**需澄清——当前指纹是 128 位，不是 64 位。**

`FingerprintSet` 使用的是 128-bit MD5（内嵌 RFC 1321 实现），不是 xxhash64。`include/util/xxhash.h` 中的 xxhash 用于其他场景（如 hash 表内部哈希），不是 fingerprint。

但评审的深层担忧仍有效：128-bit MD5 在密码学上已不推荐，但用于非对抗性的 fingerprint 碰撞检测仍足够（2^128 空间）。更关键的是 fingerprint 的**构成**：

当前 fingerprint = MD5(`path + dev + ino`)

- 若 `path` 相同但内容已变（mtime 不同），`reference_map` 会检测到 mtime 变化，不会盲信跳过。
- 若 `ino` 复用（文件删除后新文件获得相同 inode），但 `path` 不同，MD5 输入不同，fingerprint 不同。
- 真正的碰撞场景：不同 path + 不同 dev + 不同 ino，但 MD5 输出恰好相同——概率 2^-128，可忽略。

**设计确认**：128-bit MD5 足够。但文档需明确声明 fingerprint 的构成和碰撞概率，消除误解。

---

### 6. 熔断跳过的终态（严重度：P1）

**评审指出**："一次运行完成"与"设备熔断"冲突，需要显式选择并配套：退出码区分、跳过原因码、可续传清单。

**答复**：**承认，当前设计未明确定义"完成"的语义。**

当前行为：
- 设备熔断 → 进 spbin → 探测调度器周期性重试 → 判死后永久跳过
- 目录熔断 → 进 `.circuit_breaker` 清单 → 不再重试
- 退出码：v15.5.6 后 `skipped_count > 0` 时非零退出，但不区分跳过原因

**设计修正**：
1. **定义完成语义**：
   - "完全完成"：所有可达目录均已扫描，无任何跳过 → 退出码 0
   - "部分完成"：健康部分已完成，故障部分已记录 → 退出码 1 + manifest
   - "严重失败"：主流程中断 → 退出码 2
2. **spbin 原因码扩展**：
   - `SP_REASON_PROBE_FAIL`：探测失败
   - `SP_REASON_CIRCUIT_BREAKER`：目录级熔断
   - `SP_REASON_PERMISSION`：权限拒绝（EACCES）
   - `SP_REASON_TIMEOUT`：设备超时
   - `SP_REASON_UNKNOWN`：其他
3. **生成跳过 manifest**：程序终止时输出 `{base}.skipped.manifest`，列明每个跳过的路径、原因、最后尝试时间。
4. **`--continue` 精确捡回**：恢复时读取 skipped.manifest，将 `SP_REASON_PROBE_FAIL` 和 `SP_REASON_TIMEOUT` 的条目重新入队（设备可能已恢复），其他原因码保持跳过。

文档 §2.1 需将设计目标改写为："健康部分一次完成，故障部分可枚举、可续"。

---

### 7. 删除的增量语义缺失（严重度：P1）

**评审指出**：纯指纹跳过无法发现"上轮有、这轮没"的条目。输出契约需要明确定义是全量还是增量。

**答复**：**承认，当前设计未定义删除语义。**

当前行为：
- 每轮输出是**该轮扫描到的条目快照**
- 盲信跳过的目录不会出现在输出中（因为不扫描）
- 已变更目录会全量重新扫描，输出包含当前存在的所有条目
- 但"上轮输出中有、这轮盲信跳过目录中已删除的条目"不会出现在任何 tombstone 中

**设计修正——二选一**：

**方案 A：全量快照（推荐）**
- 每轮输出是完整的当前文件系统快照
- 盲信跳过的目录需要一种"结转"机制：将上轮该目录的输出条目复制到本轮输出，除非该目录在本轮被显式扫描并产生新输出
- 这需要跨轮次的输出合并子系统，复杂度极高

**方案 B：增量加 tombstone**
- 输出只包含变更（新增、修改、删除）
- 删除条目输出为 tombstone 行（path + "DELETED" 标记）
- 需要消费者端维护一个"当前状态视图"

**当前推荐**：先明确声明"当前输出是 at-least-once 快照，不保证跨轮次删除可见"。长期引入 `--incremental-with-tombstone` 模式。

---

### 8. PATH_MAX 与 TOCTOU（严重度：P1）

**评审指出**：4096 深度截断即漏扫，符号链接竞争，需 openat 族 fd 相对下降。

**答复**：**部分承认，但受限于运行环境。**

当前实现使用 `readdir` + `lstat(path)`，即基于路径字符串的遍历。这确实存在：
- `PATH_MAX` 截断（Linux 上通常为 4096，但 NFS 可能更小）
- TOCTOU：路径在 `readdir` 和 `lstat` 之间被修改

**约束**：CentOS 7.4 内核 3.10 没有 `openat2`/`RESOLVE_BENEATH`。`openat` 可用，但需要逐层维护 dirfd 栈，且 `readdir` 本身不接受 fd。

**设计修正**：
1. **短期**：在 `worker_scanner.c` 中增加路径长度检查，`path_len > PATH_MAX - 256`（留余量）时记 `DIR_ERROR` 而非静默截断。
2. **中期**：引入 `fts_open()`/`nftw()` 或自研 dirfd 栈遍历器，使用 `openat(O_DIRECTORY|O_NOFOLLOW)` 逐层下降。
3. **符号链接**：当前 `MAX_SYMLINK_DEPTH = 8`，循环检测存在但 TOCTOU 未防护。文档需声明"符号链接在扫描期间被篡改不在保证范围内"。

---

### 9. EACCES（root_squash）静默（严重度：P1）

**评审指出**：root 跑也躲不过 root_squash，权限拒绝必须是带计数的一等终态，不许静默。

**答复**：**部分承认。v15.5.7 已部分覆盖，但不完整。**

v15.5.7 引入了 `IPC_MSG_ENTRY_ERROR` 和 `DIR_ERROR` 记录，但 EACCES 的处理路径：
- 目录级 EACCES（`opendir` 失败）：当前作为 `DIR_ERROR` 上报并记录
- 条目级 EACCES（`lstat` 失败）：v15.5.7 后通过 `ENTRY_ERROR` 上报

但评审的担忧是"某些路径的 EACCES 可能被归类为普通错误而非权限终态"，导致重试风暴或静默跳过。

**设计修正**：
- EACCES 明确分类为**权限终态**，不重试、不熔断设备
- 目录级 EACCES → `DIR_ERROR` + 原因码 `SP_REASON_PERMISSION`
- 条目级 EACCES → `ENTRY_ERROR` + 记日志
- 输出 skipped manifest 时，EACCES 条目单独归类

---

### 10. NFS 属性缓存致 mtime 滞后（严重度：P2）

**评审指出**：`ac/lookupcache` 默认缓存目录属性数秒到一分钟，增量比较读到旧 mtime。

**答复**：**承认，文档未覆盖此约束。**

NFS 客户端属性缓存（`actimeo`、`acregmin`、`acdirmin` 等）确实会导致 `stat` 返回缓存值而非服务端最新值。

**设计修正**：
1. 文档 §2.2 "NFS 挂载要求"中增加：`actimeo=0` 或 `noac`（若性能可接受）
2. 若用户启用盲信跳过但挂载带缓存，文档需声明"增量跳过可能因属性缓存而滞后，短间隔增量扫描建议 `actimeo=0`"
3. 程序启动时检测挂载选项（读 `/proc/mounts`），若发现 `ac` 且启用了盲信跳过，输出 warning

---

### 11. 硬链接去重丢路径（严重度：P2）

**评审指出**：若 reference_map 按 (st_dev, st_ino) 去重丢条目，输出不完整。

**答复**：**需澄清——reference_map 仅用于盲信跳过，不用于输出去重。**

输出渲染时，每个文件条目独立输出，不基于 inode 去重。`reference_map` 和 `visited_set` 的用途：
- `visited_set`：防环（同一目录通过不同路径访问）
- `reference_map`：半增量时判断文件是否需重新 lstat
- 两者都不影响输出阶段

但评审的深层担忧——bind mount 或硬链接导致同一 inode 多路径——确实存在：
- 若 `/a/file` 和 `/b/file` 是硬链接，两者都会被扫描、都输出
- 这是**正确行为**，因为用户要的是路径清单，不是 inode 清单
- `visited_set` 按 `(path, dev, ino)` 的 MD5 防环，不是去重

**设计确认**：输出是完整路径清单，不丢路径。文档 §5.3 需明确声明这一点。

---

### 12. 并发改名/移动（严重度：P2）

**评审指出**：结构性漏/重，无法根除，需定义一致性模型。

**答复**：**承认，文档未定义一致性模型。**

在"业务未冻结"（即扫描期间文件系统仍在变动）的前提下，任何遍历工具都无法保证"恰好看到某一时刻的快照"。这是分布式系统/文件系统的基本约束。

**设计修正**：
- 文档需明确定义一致性模型：**快照隔离的近似**
  - 不保证看到全局一致快照
  - 保证每个目录的枚举和 stat 是原子进行的（`readdir` 返回的条目在该调用时刻存在）
  - 跨目录的一致性不保证（扫描 `/a` 时 `/b` 可能已被改名）
- 对于"扫描期间被改名的目录"：
  - 若改名发生在 `opendir` 之前 → `ENOENT`，按竞态处理
  - 若改名发生在 `readdir` 循环中 → 后续 `lstat` 可能 `ENOENT`，按 `ENTRY_ERROR` 处理
  - 若改名发生在扫描完成后 → 不影响本轮输出，下一轮增量会处理

---

## 三、容错机制设计失误 —— 逐项答复

### A. 缺 fencing epoch（严重度：P0）

**评审指出**：Worker 死后目录重派，旧 Worker 的批次可能还能到达，产生重复/错序。

**答复**：**承认，这是当前设计未覆盖的竞态。**

当前替换流程：
1. IPC 线程检测超时 → SIGKILL → send_return(RET_DEAD)
2. 主线程收到 RET_DEAD → cleanup → spawn 新 Worker → send_replace_to_ipc
3. 新 Worker 开始服务

问题：SIGKILL 后旧 Worker 的 Scanner 线程可能仍在内核中完成最后的 `write()`，残留数据在 pipe 中。IPC 线程在 `CMD_REPLACE` 时重置了 FSM，但**旧数据已在内核 pipe 缓冲区中**，新 Worker 的 epoll 可能读到旧数据。

**设计修正**：
1. **epoch 机制**：每个目录派发时附加递增 epoch。Worker 返回的 BATCH 携带 `(dir_path, epoch)`。主线程丢弃 epoch < 当前 expected_epoch 的批次。
2. **waitpid 确认**：`cleanup_dead_worker_slot` 中，SIGKILL 后增加 `waitpid(WNOHANG)` 轮询，确认旧进程已回收后才 spawn 新 Worker。若 D-State 下回收不了，挂起该目录（不进 dispatch_queue，记 `PENDING_ZOMBIE`），等回收后再重派。
3. **pipe 清空**：`CMD_REPLACE` 时 IPC 线程先 `read()` drain 旧 pipe 中所有残留数据，再 epoll ADD 新 fd。

---

### B. 心跳与背压耦合（严重度：P1）

**评审指出**：数据通道满 → Worker 阻塞写 → 心跳停 → 健康 Worker 被误杀。心跳应判"计数是否前进"而非固定超时。

**答复**：**承认，v15.5.9 的时间驱动心跳部分缓解但未根治。**

v15.5.9 的改动：
- `HEARTBEAT_TIMEOUT` 120s（给大目录喘息时间）
- `scanner_progress_tick` 每 5s 更新（即使处理慢）
- 但 IPC 线程的心跳（`IPC_MSG_HEARTBEAT`）与 Scanner 的 `last_progress` 是两个不同机制

当前问题：
- IPC 线程的心跳是独立循环，不受 Scanner 阻塞影响（因为 fd_ctrl 独立通道）
- 但 Scanner 的 `last_progress` 更新依赖 `scanner_progress_tick`，若 `send_batch()` 阻塞在 fd_data（Master 消费慢），tick 仍会在 5s 间隔更新——因为 tick 是时间驱动，不依赖 batch 发送完成
- **真正的背压场景**：若 fd_data 的 pipe 满，Scanner `write()` 阻塞，此时 tick 仍在更新（因为 tick 在 write 前后调用），所以 `last_progress` 不会过时

但评审的担忧更深层：若 Master 侧完全卡住（如去重线程池满、async_writer 阻塞），所有 Worker 的 fd_data 都会满，所有 Scanner 都会阻塞在 `write()`。此时：
- tick 仍更新（write 前后都有 tick）
- IPC 线程心跳仍发送（fd_ctrl 独立）
- 但没有任何实际进展

**设计修正**：
1. **心跳携带计数**：`IPC_MSG_HEARTBEAT` 携带 Scanner 已处理的条目计数。Master 判活条件：心跳到达 **且** 计数在前进。
2. **区分"D-State 卡住"与"背压阻塞"**：读 `/proc/<pid>/wchan`：
   - `wchan = "rpc_wait_bit_killable"` → NFS D-State，该杀
   - `wchan = "pipe_write"` → 背压阻塞，不该杀（杀之加剧问题）
3. **背压反压**：若 Master 检测到多数 Worker 因背压阻塞，应主动加速消费（如扩大 async_writer 刷盘频率、临时增加去重线程）

---

### C. 设备身份不能只用 st_dev（严重度：P1）

**评审指出**：NFS 重挂载/failover 后 st_dev 变，熔断状态错挂或丢失。

**答复**：**承认。**

当前 `DeviceManager` 以 `st_dev` 为主键。在以下场景会失效：
- NFS 服务端 failover → 客户端重连 → `st_dev` 变化
- 同一 export 多次挂载到不同挂载点 → 不同 `st_dev`
- 容器/namespace 中 `st_dev` 可能映射不同

**设计修正**：
1. **主键扩展**：设备身份 = `(fsid, server, export_path)`，其中 `fsid` 来自 `statfs()->f_fsid`，`server/export` 来自 `/proc/mounts` 解析。
2. **st_dev 作为运行时映射**：`st_dev` 仅用于运行时快速查找，程序启动时建立 `st_dev → (fsid, server, export)` 映射表。
3. **持久化**：spbin 中记录 `(fsid, server, export)` 而非仅 `st_dev`，保证跨运行期一致。

---

### D. 毒丸目录（严重度：P1）

**评审指出**：某个目录内存在必崩条目 → Worker 反复被杀 → 重试风暴 + 拖垮设备熔断统计。

**答复**：**部分承认。v15.5.3 的目录级熔断已部分覆盖，但阈值和判死逻辑需细化。**

当前目录级熔断：`CIRCUIT_BREAKER_THRESHOLD = 10`，同一目录连续 DEV_TIMEOUT 10 次后跳过。

问题：
- 若目录内有"必 EIO 的条目"，Worker 不是 DEV_TIMEOUT 而是 `RET_ERROR` → 设备级熔断，不触发目录级熔断
- 设备级熔断会惩罚整个设备，可能误伤健康目录
- 10 次阈值对"必崩"场景来说太高（10 × 120s = 20 分钟才熔断）

**设计修正**：
1. **目录级致死计数**：同一目录因**任何原因**（DEV_TIMEOUT、ERROR、ENTRY_ERROR 中的 EIO/ETIMEDOUT）导致 Worker 死亡累计达到 3 次，直接进隔离清单，不再重试。
2. **隔离清单与 spbin 分离**：毒丸目录进入 `{base}.poison` 清单，带原因码（`POISON_IO_ERROR`、`POISON_TIMEOUT`、`POISON_UNKNOWN`）。
3. **设备级熔断不受影响**：毒丸目录不计入设备级错误统计，防止误伤。

---

### E. 熔断粒度（严重度：P1）

**评审指出**：ParaStor 单 OST 故障表现为局部 EIO，statfs 探活是通的——设备级不跳、目录级狂跳。需要聚合指标驱动"部分降级"态。

**答复**：**承认，当前设计是二元态（正常/熔断），缺少"部分降级"灰度态。**

当前设备级熔断：
- 单个目录 EIO → `RET_ERROR` → `dev_mgr_mark_probing(dev)`
- 探活成功 → `DEV_STATE_NORMAL`
- 探活失败 → `DEV_STATE_DEAD` → 所有该设备目录跳过

ParaStor 等分布式存储的问题：
- 单 OST（对象存储目标）故障 → 仅影响该 OST 上的目录 → 其他目录正常
- `statfs` 探活（根路径）可能正常，因为根路径在健康 OST 上
- 结果：设备级不熔断，但大量目录级错误 → 目录级熔断清单膨胀 → 扫描残缺

**设计修正**：
1. **错误聚合指标**：
   - 单位时间内该设备的 `EIO/ETIMEDOUT` 目录数占比
   - 若 > 10% 目录报错 → 进入 `DEV_STATE_DEGRADED`（部分降级）
   - 若 > 50% → 进入 `DEV_STATE_DEAD`
2. **部分降级行为**：
   - 健康目录正常扫描
   - 报错目录进入 probe_scheduler 单独探测
   - Monitor 面板显示降级状态
3. **探活粒度细化**：探活路径不应只是根路径，应从 spbin 中随机抽样多个子路径分别探活。

---

### F. 孤儿 Worker 自裁（严重度：P2）

**评审指出**：Master 被杀后旧 Worker 继续写，污染下一轮。

**答复**：**承认，当前设计未覆盖 Master 异常退出的 Worker 清理。**

当前 Worker 进程无自裁机制。若 Master 被 SIGKILL（如 OOM），Worker 进程成为孤儿，继续运行：
- Scanner 线程继续扫描（但 pipe 已断，write 会 SIGPIPE/EPIPE）
- IPC 线程可能继续心跳（但无接收方）

**设计修正**：
1. **`PR_SET_PDEATHSIG`**：Worker 启动时设置 `prctl(PR_SET_PDEATHSIG, SIGTERM)`，Master 死后 Worker 自动收到 SIGTERM。
2. **管道 EOF 检测**：Worker 的 IPC 线程检测 `fd_cmd` EOF → 设置 `stop_flag` → Scanner 优雅退出。
3. **超时自裁**：若 Worker 成为孤儿后 30s 内未收到任何命令，自动 `exit(1)`。

---

### G. 双跑防护（严重度：P2）

**评审指出**：输出/进度目录要 flock 防双跑，续传时校验 roots/格式/flag/版本。

**答复**：**部分承认。**

当前已有：
- `acquire_lock()`：进度目录的 `.lock` 文件 flock
- `save_config_to_disk()`：扫描配置写入 `{base}.config`

但校验不完整：
- 续传时读取 `.config`，但仅比对 `target_path` 和 `last_cmd_args`
- 未校验输出格式字符串、版本号、`--strict-nlink` 标志等

**设计修正**：
1. 续传时校验 `.config` 中的所有关键字段：
   - `target_path` 必须一致
   - `format` 必须一致（否则输出格式突变）
   - `VERSION` 必须兼容（主版本号相同）
   - `--strict-nlink` 标志必须一致（否则 nlink oracle 基准不同）
2. 不一致时拒绝续传，强制 `--runone` 全量扫描。

---

### H. 进度文件位置（严重度：P2）

**评审指出**：pbin/dpbin 与 CSV 输出不得放在被扫描的故障 NFS 上。

**答复**：**承认，文档未声明此约束。**

当前设计允许 `-f`（进度前缀）和 `-o`/`-O`（输出）放在任意路径，包括与被扫描目录相同的 NFS。

**风险**：
- 若 NFS 故障，扫描停滞 + 进度无法写入 = 双重打击
- 若 NFS 部分故障（单 OST），进度写入可能成功但扫描目录失败，或反之

**设计修正**：
1. 文档 §11 明确声明："进度文件和输出文件建议放置于本地磁盘或独立的可靠存储，不要与被扫描目录共用同一 NFS 挂载"
2. 程序启动时检测：若 `target_path` 和 `progress_base` 在同一 `st_dev`，输出 warning
3. 输出文件和进度文件分开放置：输出文件放本地高速盘，进度文件放持久化存储

---

## 四、完整性与其他问题

### 资源有界性（严重度：P1）

**评审指出**：需对全链路给出有界性证明。

**答复**：**承认，文档 §9 性能考量中未覆盖有界性论证。**

当前已知边界：
- `dispatch_queue`：hard cap 10 万条（HIGH_WATER 背压）
- `visited_set`：预分配 `estimated_files`（默认 1000 万），但运行时可能超限 → rehash
- `fpbin_entries`：内存数组，容量动态增长，无硬 cap
- BATCH 在飞量：Worker 数 × batch_size = 8 × 1024 = 8192 条
- `record_batch`：4096 条 / 1MB

**缺失边界**：
- `fingerprint_set` 在恶意场景（如全硬链接）下可能无界增长
- `spbin_entries` 内存缓存（设备恢复时重入队用）无容量上限
- `dspill` 文件大小无上限（append-only）

**设计修正**：
1. 为每个队列/集合写明"满了怎么办"：
   - `fingerprint_set`：达到 80% 容量时触发分片落盘（mmap 或写入临时文件）
   - `spbin_entries`：上限 10 万条，超限后最早条目写入 spbin 文件、从内存释放
   - `dspill`：单文件上限 1GB，超限后滚动到 `.dspill.1`、`.dspill.2`
2. 文档 §9 增加"资源有界性保证"小节，逐模块列出上限值和超限行为。

---

### 协议自相矛盾（严重度：P1）

**评审指出**：TLV 的意义是演进性，`struct stat` memcpy 锁死 ABI——两者并存等于没有 TLV。

**答复**：**承认。**

当前 IPC 协议：
- Header 是 TLV（`msg_type + payload_len`）
- 但 payload 内部使用 `struct stat` 的 `memcpy` 序列化

问题：
- `struct stat` 布局因架构（x86_64 vs aarch64）、glibc 版本、编译选项（`_FILE_OFFSET_BITS=64`）而异
- 不是 TLV，是隐式 ABI 依赖

**设计修正**：
1. **显式字段编码**：将 `struct stat` 展开为显式字段序列：
   ```
   st_ino(8) + st_mode(4) + st_nlink(4) + st_uid(4) + st_gid(4) +
   st_size(8) + st_blocks(8) + st_atime(8) + st_mtime(8) + st_ctime(8) + ...
   ```
2. **协议版本号**：Header 中增加 `protocol_version` 字段（如 `uint16_t`），当前为 1。
3. 接收端校验 protocol_version，不匹配则 `log_fatal`。

---

### MDS 保护（严重度：P2）

**评审指出**：固定并发数可能压垮元数据节点，触发自己的熔断。

**答复**：**承认。**

当前 Worker 数固定为 8（或用户指定），没有根据 MDS 负载动态调整。

**设计修正**：
1. **自适应并发**：基于 `readdir`/`lstat` 的延迟分位数（p50/p99）调整 Worker 数：
   - p99 延迟 < 100ms → 维持当前并发
   - p99 延迟 100ms~1s → 减少 1 个 Worker
   - p99 延迟 > 1s → 减少至 2 个 Worker（最低保证进度）
2. **AIMD 算法**：Additive Increase（每 30s 尝试增加 1 个 Worker）、Multiplicative Decrease（延迟 spike 时减半）。
3. 文档 §9 增加"自适应并发控制"设计。

---

### 大目录长尾（严重度：P2）

**评审指出**：60M 条目目录单 Worker 串行数小时是物理上限，需声明为"已接受限制"。

**答复**：**承认。**

当前 Scanner 是单线程串行 `readdir` + `lstat`，大目录无并行加速。

**设计修正**：
1. 文档 §2.1 明确声明："单目录条目数超过数千万时，扫描时间可能达数小时，这是 NFS 协议和通用文件系统的物理限制。本工具不承诺单目录并行加速。"
2. Monitor 面板对当前扫描目录显示 ETA（基于已处理条目数和 `st_nlink` 预估）。
3. 与 §8.4 防误判联动：大目录场景自动延长 heartbeat timeout（已做，120s）。

---

### 输出契约细节（严重度：P2）

**评审指出**：文件名任意字节需定义转义；单文件 240GB 建议按大小分片；xattr 的 ERANGE 竞态。

**答复**：**部分承认。**

当前输出格式：
- CSV 模式（`--csv`）：使用 RFC 4180 转义（引号、逗号、换行）
- 自定义格式（`-F`）：用户自定义，无自动转义

**缺失**：
- 非 UTF-8 字节序列的处理未定义
- 输出分片只有按行数（`output_slice_lines`），无按大小分片
- xattr 的 `listxattr`/`getxattr` 之间长度竞态（文件 xattr 在两者之间被修改）

**设计修正**：
1. **转义策略**：
   - 非 UTF-8 字节：按 `%XX` URL 编码或保留原始字节（文档声明为"原始字节输出，消费者需处理"）
   - 换行符：CSV 模式下转义为 `"\n"`，文本模式下保留原始换行（文档声明"输出可能含多行字段"）
2. **按大小分片**：增加 `--output-slice-size` 参数，与 `--output-slice-lines` 二选一。
3. **xattr ERANGE**：`get_xattr_str()` 中增加有界重试（最多 3 次，间隔 1ms），超限后输出 `"<xattr-too-long>"` 标记。

---

### 工具链前提（严重度：P2）

**评审指出**：CentOS 7.4 默认 gcc 4.8 对 `_Atomic` 支持有限。

**答复**：**需澄清——当前 Makefile 使用 `-std=gnu11`，gcc 4.8 支持 C11 原子操作（`_Atomic` 关键字）。**

但 gcc 4.8 的 C11 支持不完整：
- `_Atomic` 类型限定符：支持
- `<stdatomic.h>`：支持（需 `-std=c11` 或 `-std=gnu11`）
- `_Thread_local`：支持
- 部分原子操作的内存序参数：gcc 4.8 可能不支持所有 memory order 常量

**设计修正**：
1. 文档 §11.1 增加编译器要求："GCC ≥ 4.8 with `-std=gnu11`。若使用更低版本，需升级至 devtoolset-7+。"
2. 在 `config.h` 中增加编译时检查：
   ```c
   #if __GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 8)
   #error "GCC 4.8+ required for C11 _Atomic support"
   #endif
   ```

---

## 五、盲信跳过的前置条件（补充要求）

**评审要求**：文档中的盲信跳过必须加前置条件说明，明确盲信仅针对除第一次之后的快速非完整扫描，并且需要显式开关参数。

**答复**：**同意。**

当前 `--skip-interval` 就是显式开关，但文档 §2.1 和 §5.3 的描述不够严格。

**设计修正**：
1. 文档 §2.1 设计目标中，将"半增量扫描"改为：
   > "盲信跳过（`--skip-interval`）：仅适用于**已有一次完整基准扫描**后的后续快速检查。首次扫描必须全量（不带 `--skip-interval`），建立 reference_map 基准。盲信跳过依赖文件系统保证目录 mtime 随子项变动而更新；若该前提不成立（如 noatime、某些分布式存储的弱一致性 mtime），则不应启用。"

2. 文档 §5.3 Worker Scanner 中增加：
   > "盲信检查流程：
   > 1. 计算当前目录/文件的 fingerprint（path + dev + ino 的 128-bit MD5）
   > 2. 查询 reference_map（上一轮扫描建立的基准）
   > 3. 若 fingerprint 存在且 mtime 未变 → 跳过 lstat（文件）或跳过子树扫描（目录，但子目录仍须递归发现）
   > 4. 若 fingerprint 不存在或 mtime 已变 → 全量扫描
   > 5. 本轮扫描结束后，reference_map 不自动更新——需下一轮全量扫描或显式 `--runone` 重建基准。"

3. 程序启动时检测：若 `--skip-interval` 启用但 reference_map 为空（无基准），拒绝运行并提示"请先执行一次全量扫描建立基准"。

---

## 六、待办清单（按优先级排序）

| 优先级 | 事项 | 所属章节 | 性质 |
|--------|------|---------|------|
| P0 | 定义输出落盘与 dpbin 提交的顺序不变量（偏移量屏障） | §7 / §8 | 新增设计 |
| P0 | 为目录派发引入 epoch + waitpid 确认 | §6 / §8 | 新增设计 |
| P0 | 半增量跳过粒度与前提条件声明 | §2 / §5 | 文档修正 |
| P1 | 恢复时加载 dspill 并集 | §7 | 设计修正 |
| P1 | 建立 errno 分类矩阵 | §8 | 新增设计 |
| P1 | 设备身份主键改为 (fsid, server, export) | §5 / §8 | 设计修正 |
| P1 | 引入目录级致死计数与毒丸清单 | §8 | 新增设计 |
| P1 | 设备级熔断增加 DEGRADED 灰度态 | §8 | 设计修正 |
| P1 | IPC 协议显式字段编码 + 版本号 | §6 | 设计修正 |
| P1 | 全链路资源有界性论证 | §9 | 文档新增 |
| P2 | 定义一致性模型（快照隔离近似） | §1 / §2 | 文档新增 |
| P2 | 孤儿 Worker 自裁（PR_SET_PDEATHSIG） | §4 | 设计修正 |
| P2 | 续传配置校验（版本、格式、标志） | §7 | 设计修正 |
| P2 | 进度文件位置声明（建议本地盘） | §11 | 文档修正 |
| P2 | 自适应并发控制（AIMD） | §9 | 新增设计 |
| P2 | 输出按大小分片 + xattr ERANGE 重试 | §5 / §11 | 设计修正 |

---

> 本评审答复文档针对 Design.md 的设计层面问题逐一回应，不涉及具体代码实现。待办清单中的事项需在进入实现阶段前完成设计确认。
