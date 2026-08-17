# v15.5.9 设计评审 fixme 清单

> 来源：外部深度评审（2026-08-10）+ Jimmy 评审（2026-08-09）  
> 状态：待设计确认 → 编码实现  
> 约束：Design.md 是现有设计基线，本清单跟踪所有需修正项，不污染设计文档

---

## P0 阻塞项（进入编码前必须闭环）

### P0-001 [Design v1] FINISH/BATCH 竞态——目录任务完成屏障
- **问题**：Worker 先发 BATCH 再发 FINISH，但两通道独立 epoll，Master 可能先收到 FINISH 标记目录完成，后续 BATCH 滞留丢失，子树永久漏扫
- **根因**：FINISH 语义不等待 BATCH 全处理
- **方案**：目录任务生命周期状态机（P0-009）定义 `SCANNING → ALL_BATCHES_RECEIVED → ALL_BATCHES_PROCESSED → OUTPUT_COMMITTED → COMPLETED` 路径。FINISH 仅触发 `ALL_BATCHES_RECEIVED`（收到所有数据），不直接触发 COMPLETED。必须等所有 BATCH 处理完毕、子目录入队、输出偏移确认后才写 dpbin。空目录可直通完成。
- **关联**：P0-009 目录任务生命周期状态机
- **状态**：**Design v1 已确认，待编码实现**（状态机定义见 P0-009）
- **评审来源**：外部评审 P0-1

### P0-002 [Design v1] 输出三态状态机——DISCOVERED → OUTPUT_QUEUED → OUTPUT_COMMITTED
- **问题**：pbin 记录"已发现"不代表"已输出"。崩溃后增量跳过，文件条目永久丢失
- **根因**：发现态与输出提交态混用
- **方案**：目录任务生命周期状态机（P0-009）定义 `OUTPUT_COMMITTED` 态——输出线程确认该目录所有文件条目已落盘。dpbin_append 需等待 OUTPUT_COMMITTED 后才能写 dpbin。
- **关联**：P0-001, P0-009
- **状态**：Design v1 已确认，待编码实现（状态机定义见 P0-009）
- **评审来源**：Jimmy 评审 / 外部评审 P0-2

### P0-003 [Design v1] fpbin 二次崩溃恢复路径
- **问题**：第一次续传期间发现新目录 A 写入 fpbin，旧 pbin 未消费完时再次崩溃。第二次续传不读 fpbin，A 永久漏扫
- **根因**：恢复期间父目录扫描完会写 dpbin，崩溃后父目录不在 pbin-dpbin 差集中，不会重扫，导致 fpbin 里的子目录永久丢失
- **方案**：引入 `fpbin + dfpbin` 原子对作为**可抛弃工作区**
  1. dfpbin 记录 fpbin 阶段已完成的父目录（格式同 dpbin）
  2. 恢复时检查 fpbin+dfpbin 完整性（Footer 校验通过）
  3. 完整 → fpbin 内容合并到 pbin，清空 fpbin/dfpbin
  4. 不完整 → **整对抛弃**，重新从旧 pbin 恢复
  5. 不会无限套娃——总是回退到旧 pbin 恢复，不会递归产生新的 fpbin
- **状态**：**Design v1 已确认，待编码实现**
- **评审来源**：外部评审 P0-3

### P0-004 [Design v1] visited_set 背压竞态——统一队列模型
- **问题**：目录因背压写 pbin 后进 visited_set，但未入队，只进 dspill。回填时 visited_set 误判"已访问"而丢弃
- **根因**："已发现"等价于"已入队"
- **方案**：
  1. **dspill 是 dispatch_queue 的磁盘扩展**，不是独立缓存池（append-only，字节游标回填）
  2. 废除 pbin cursor 运行时回填（v15.5.8 已改用 dspill，文档需同步修正）
  3. **visited_set 语义拆分**：
     - `discovered_set`：已发现（已写 pbin），防重复发现
     - `enqueued_set`：已入队（dispatch_queue 或 dspill），防重复入队
     - `completed_set`：已完成（dpbin），防重复扫描
  4. **统一 `enqueue()` 调度入口**：所有待扫描目录（恢复 pump、运行时新发现、dspill 回填）都走同一入口
  5. dspill 写入策略：内存缓冲池 1000 条/1 秒刷盘，崩溃丢失可接受（pbin 是权威持久化）
- **状态**：Design v1 已确认，待编码实现
- **评审来源**：外部评审 P0-4
- **关联**：P0-003 dspill 恢复
- **评审来源**：外部评审 P0-4

### P0-005 [Design v1] spbin 纳入恢复路径
- **问题**：续传只算 `pbin-dpbin`，不读 spbin。被熔断目录永久丢失
- **根因**：spbin 不在恢复逻辑中
- **方案**：
  1. spbin 格式扩展：`[path][reason: uint8_t][timestamp: time_t][device_key: 64 bytes]`
  2. 恢复时按 device_key 分组：PERMISSION/CIRCUIT_BREAKER 永久跳过；PROBE_FAIL/TIMEOUT 超窗后敢死队探测
  3. 探测成功 → 设备标记 NORMAL，spbin 该设备条目删除；探测失败 → timestamp 更新，指数退避（30min→2h→6h→24h）
  4. spbin 保持 append-only，正常退出时 compaction 清理已恢复条目
- **关联**：P1-002 errno 分类矩阵
- **状态**：Design v1 已确认，待编码实现
- **评审来源**：外部评审 P0-5

### P0-006 [Design v1] MSG_DROP 正常路径禁止
- **问题**：MSG_DROP 只销账不补救，若 BATCH 被 drop 则结果丢失
- **根因**：协议层允许丢结果
- **方向**：
  1. 正常路径禁止 drop，队列满时应背压
  2. 若确实 drop：BATCH drop → 整目录任务重扫或标记失败；ERROR drop → 终止运行
  3. 任何 drop 发生后，本次运行不能标记为"完整完成"
- **评审来源**：外部评审 P0-6

### P0-007 [Design v1] RET_ERROR 状态机补全
- **问题**：`BUSY -- RET_ERROR --> IDLE`，但 pending_tasks 是否减、目录是否重入队、是否写 spbin 均未定义
- **根因**：状态机缺漏
- **方向**：
  1. RET_ERROR → pending_tasks--（Worker 已释放）
  2. 当前目录**不重入队**（避免重试风暴）
  3. 写入 spbin，带原因码 SP_REASON_PROBE_FAIL
  4. 设备进入 PROBING 态
- **评审来源**：外部评审 P0-7

### P0-008 [Design v1] Run manifest + baseline_eligible 原子切换
- **问题**：上次扫描可能不完整（spbin 非空、dspill 未排空等），但没有机制阻止它成为盲信基准
- **根因**：缺少运行完整性标记
- **方向**：
  1. 每次运行生成 `{base}.manifest`，含 run_id、target_path、schema_version、status、stats、cursors、checksum、baseline_eligible
  2. `baseline_eligible = true` 条件：status==complete、skipped==0、dspill 排空、fpbin 转正、archive 校验通过
  3. 盲信扫描启动时 baseline_eligible!=true 则拒绝运行
  4. finalize_archive() 保留旧基准，原子切换新基准（新 archive 写完校验通过后替换，旧基准保留为 `.archive.prev`）
- **状态**：**Design v1 已确认，待编码实现**
- **评审来源**：外部评审 P0-8

### P0-009 [Design v1] 目录任务生命周期状态机
- **问题**：当前只有"派发/完成"二元态，缺少中间状态定义
- **方向**：引入显式状态：
  ```
  DISCOVERED → PERSISTED → ENQUEUED → DISPATCHED → SCANNING 
    → ALL_BATCHES_RECEIVED → ALL_BATCHES_PROCESSED → OUTPUT_COMMITTED → COMPLETED
  ```
  异常分支：RETRYING / BACKOFF / DEVICE_WAITING / SKIPPED_FAILED / UNKNOWN
  只有 COMPLETED 才能进 dpbin；SKIPPED_FAILED 阻止结果标记为完整
- **关联**：P0-001, P0-002
- **评审来源**：外部评审 §6.1

### P0-010 [Design v1] 崩溃恢复矩阵——统一 Reset 援救机制
- **问题**：当前恢复只覆盖 Worker 死亡一种情况，缺少 12+ 个崩溃点的覆盖
- **根因**：试图枚举每个崩溃点的精确恢复行为，不可穷尽
- **方案**：不枚举崩溃点，统一采用 **Reset 援救机制**：
  - **核心原则**：任何目录只要没写 dpbin（状态 ≠ COMPLETED），就视为未扫描，恢复时重新入队
  - **输出截断**：恢复时输出文件截断到最后一个已确认 dpbin 对应的 offset
  - **pbin 幂等**：已写 pbin 但未完成的目录，恢复时重新扫描是安全的（re-scan 幂等）
  - **spbin 按 P0-005 处理**：超窗探测，设备活了统一入队
  - **fpbin 按 P0-003 处理**：完整则转正，不完整整对抛弃重来
  - 不需要为每个崩溃点写恢复逻辑——状态机终态唯一（COMPLETED = 写 dpbin），非终态统一重置
- **状态**：Design v1 已确认，待编码实现
- **评审来源**：外部评审 §6.2

### P0-011 [Design v1] 半增量跳过前提条件声明
- **问题**：盲信跳过的前置条件、粒度、收益描述不准确
- **澄清与结论**：
  1. **目录 mtime 传播性无关紧要**：盲信扫描直接采信 pbin 中记录的**文件级 mtime**，不依赖目录 mtime 是否向上传播
  2. **reference_map 内存膨胀可接受**：盲信扫描不更新 reference_map（只读）。全量扫描时重建 reference_map，旧数据自然丢弃。若用户指定 `--runone` 但目标目录非空，exit(2) 提示清理
  3. **文件级粒度是默认行为**：目录本身仍须 readdir + lstat（发现新增/删除），但目录下的**已有文件**可盲信跳过 lstat。两者不冲突
- **状态**：Design v1 已确认（前提澄清完毕）
- **评审来源**：Jimmy 评审 / 外部评审

### P0-012 [Design v1] epoch + waitpid 确认（旧 Worker 残留数据）
- **问题**：SIGKILL 后旧 Worker 的 Scanner 线程可能仍在内核中完成最后的 write()，残留数据在 pipe 中
- **方案**：
  1. **epoch 机制**：每个 CMD_SCAN 附带递增 epoch（64 位原子计数器），Worker 返回 BATCH 携带 epoch，Master 丢弃过期 epoch
  2. **waitpid 确认**：SIGKILL 后轮询 waitpid(WNOHANG) 直到旧进程回收，通常 < 1ms
  3. **pipe drain**：CMD_REPLACE 时 IPC 线程先清空旧 pipe 读缓冲区
- **状态**：**Design v1 已确认，待编码实现**
- **评审来源**：Jimmy 评审 A

---

## P1 重要项（不阻塞但需尽快）

### P1-001 [Covered by P0-004] 恢复时加载 dspill 并集
- **问题**：恢复逻辑未读取 dspill，背压跳推的目录可能丢失
- **状态**：已纳入 P0-004 统一队列模型。dspill 是 dispatch_queue 的磁盘扩展，恢复时自动读取回填。无需单独处理。
- **评审来源**：Jimmy 评审

### P1-002 [WIP] errno 分类矩阵
- **问题**：EINTR/ESTALE/ETIMEDOUT/EIO/ENOENT 未统一分类处理
- **方向**：
  | 错误码 | 分类 | 行为 |
  |--------|------|------|
  | EINTR | 瞬态信号中断 | 有界重试（3 次，间隔 1ms） |
  | ESTALE | NFS stale handle | 按父目录 dirfd 重新解析；仍失败→设备嫌疑 |
  | ETIMEDOUT/EIO | 设备级故障 | 喂给 probe_scheduler，计数熔断 |
  | EACCES | 权限终态 | DIR_ERROR + SP_REASON_PERMISSION，不重试 |
  | ENOENT/ENOTDIR | 竞态删除 | 静默跳过，记 debug 日志 |
  | ENAMETOOLONG | 覆盖边界 | DIR_ERROR，不可静默 |
  | ENOMEM | 本地资源耗尽 | log_fatal，终止运行 |
- **状态**：已在 todo，待设计确认
- **评审来源**：Jimmy 评审 / 外部评审

### P1-003 [WIP] 设备身份主键改为 (fsid, server, export)
- **问题**：NFS failover 后 st_dev 变，熔断状态错挂或丢失
- **方向**：
  1. 设备身份 = `(fsid, server, export_path)`，fsid 来自 statfs()->f_fsid
  2. st_dev 仅用于运行时快速查找，启动时建立 st_dev → (fsid, server, export) 映射
  3. spbin 中记录 (fsid, server, export) 而非仅 st_dev
- **状态**：已在 todo，待设计确认
- **评审来源**：Jimmy 评审 C

### P1-004 [Covered by P0-005] 毒丸目录清单（致死 3 次隔离）
- **问题**：某目录内存在必崩条目 → Worker 反复被杀 → 拖垮设备熔断统计
- **状态**：已纳入 P0-005 spbin 扩展。spbin reason 码增加 `POISON(5)`，同一目录致死 3 次直接进 POISON，永久跳过，不计入设备级错误统计。
- **评审来源**：Jimmy 评审 D

### P1-005 [WIP] 设备级熔断 DEGRADED 灰度态
- **问题**：ParaStor 单 OST 故障表现为局部 EIO，设备级不跳、目录级狂跳
- **方向**：
  1. 错误聚合：单位时间内设备 EIO/ETIMEDOUT 目录数占比
  2. >10% 目录报错 → DEV_STATE_DEGRADED（健康目录正常扫描，报错目录单独探测）
  3. >50% → DEV_STATE_DEAD
  4. 探活粒度细化：从 spbin 随机抽样多个子路径分别探活
- **状态**：已在 todo，待设计确认
- **评审来源**：Jimmy 评审 E

### P1-006 [WIP] IPC 协议显式字段编码 + protocol_version
- **问题**：Header 是 TLV，但 payload 内部用 struct stat memcpy，锁死 ABI
- **方向**：
  1. struct stat 展开为显式字段序列（st_ino(8) + st_mode(4) + st_nlink(4) + ...）
  2. Header 增加 protocol_version(uint16_t)，当前为 1
  3. 接收端校验 protocol_version，不匹配则 log_fatal
- **状态**：已在 todo，待设计确认
- **评审来源**：Jimmy 评审 F

### P1-007 [NEW] 输出完整性语义声明
- **问题**：输出是精确快照还是近似快照？是否允许重复？删除是否有 tombstone？
- **方向**：
  1. 全量扫描：at-least-once 快照，不保证 exactly-once
  2. 盲信扫描：基准回放 + 新增发现，不保证变更检测、不保证删除可见
  3. 退出码区分：0=完全完成、1=部分完成、2=严重失败
  4. 失败目录下的旧基准不结转（避免假存在）
- **评审来源**：外部评审 §6.4

### P1-008 [NEW] 输出文件续写语义
- **问题**：-c 续传时输出是追加还是覆盖？已写入行如何避免重复？
- **方向**：
  1. 输出文件续传时追加（基于 output_offset checkpoint）
  2. 恢复时截断输出到最后一个已确认提交的 dpbin 偏移
  3. 输出文件损坏检测：启动时校验输出文件大小与 dpbin 最后记录一致
  4. -O 分片模式：记录每个分片的 output_offset，续传时从原分片继续
- **评审来源**：外部评审

### P1-009 [OBSOLETE] pbin 文本格式 → 二进制安全编码
- **问题**：Linux 文件名可含换行/控制字符，pbin 一行一条记录模型被破坏
- **状态**：**不成立**。实际代码中 pbin 本来就是二进制格式：`[path_len: size_t][path: N bytes][dev: dev_t][ino: ino_t][mtime: time_t][d_type: unsigned char]`，`path_len` 前缀编码天然支持任意字节（换行、控制字符、非 UTF-8）。
- **但衍生问题**：二进制字段宽度（`size_t`、`dev_t`、`ino_t`、`time_t`）平台相关，跨架构恢复会错位 → 见 **P3-001 平台兼容性检查**
- **评审来源**：外部评审

### P1-010 [NEW] 盲信目录枚举失败的输出语义
- **问题**：目录因设备错误无法 readdir，旧基准结转输出假存在，不结转则大规模漏输出
- **方向**：目录枚举失败时：
  1. 该目录及子树标记为 UNKNOWN/INCOMPLETE
  2. 不计入 completed_set，不进 dpbin
  3. 旧基准不结转（避免假存在）
  4. 进 spbin 带原因码
  5. 本次运行不能标记为"完整完成"
- **评审来源**：外部评审

---

## P2 补充项

### P2-001 [WIP] 一致性模型声明（快照隔离近似）
- **方向**：文档声明——不保证全局一致快照，每个目录的枚举+stat 原子，跨目录不保证
- **状态**：已在 todo
- **评审来源**：Jimmy 评审 / 外部评审

### P2-002 [WIP] 孤儿 Worker 自裁（PR_SET_PDEATHSIG）
- **方向**：Worker 启动时 prctl(PR_SET_PDEATHSIG, SIGTERM)，Master 死后自动退出
- **状态**：已在 todo
- **评审来源**：Jimmy 评审 G

### P2-003 [WIP] 续传配置校验（版本、格式、标志）
- **方向**：续传时校验 target_path、format、VERSION、--strict-nlink 等关键字段一致
- **状态**：已在 todo
- **评审来源**：Jimmy 评审 G

### P2-004 [WIP] 进度文件位置声明
- **方向**：文档声明进度文件和输出文件建议放本地磁盘，不要与被扫描目录共用同一 NFS
- **状态**：已在 todo
- **评审来源**：Jimmy 评审 H

### P2-005 [NEW] 容量模型（内存、磁盘、吞吐）
- **方向**：给出每条路径平均长度、每条 stat 大小、12亿条目下内存公式、最低/典型/峰值内存、输出磁盘需求、IPC吞吐目标
- **评审来源**：外部评审 §6.5

### P2-006 [NEW] 长路径 / 非 UTF-8 文件名 / 换行文件名处理
- **方向**：
  1. MAX_PATH_LENGTH 不应由 pipe 原子写反推，覆盖能力和协议分包应分离设计
  2. 路径长度超限应显式记 DIR_ERROR 而非静默截断
  3. 非 UTF-8 字节：保留原始字节，文档声明"原始字节输出"
- **评审来源**：外部评审

### P2-007 [NEW] 挂载点 / 符号链接 / bind mount 策略
- **方向**：
  1. 是否跨越挂载点？是否只扫描根设备？
  2. 符号链接作为文件输出还是跟随？
  3. bind mount 如何处理？
  4. 遍历身份与输出身份分离：遍历防环关注物理身份(dev+ino)，输出去重可包含路径
- **评审来源**：外部评审

### P2-008 [NEW] 计数器、游标、偏移全部 64 位
- **方向**：文件数、目录数、字节数、分片序号、dspill 游标、pbin 游标、output_offset、BATCH 累计数、pending 计数全部明确要求 64 位
- **评审来源**：外部评审

### P2-009 [WIP] 全链路资源有界性论证
- **方向**：为每个队列/集合写明"满了怎么办"：fingerprint_set 达 80% 触发分片落盘、spbin_entries 上限 10 万、dspill 单文件上限 1GB 滚动
- **状态**：已在 todo
- **评审来源**：Jimmy 评审

### P2-010 [WIP] 自适应并发控制（AIMD）
- **方向**：基于 readdir/lstat 延迟 p50/p99 调整 Worker 数，AIMD 算法
- **状态**：已在 todo
- **评审来源**：Jimmy 评审

### P2-011 [WIP] 输出按大小分片
- **方向**：新增 --output-slice-size 参数，与 --output-slice-lines 二选一
- **状态**：已在 todo
- **评审来源**：Jimmy 评审 / 外部评审

---

## P3 补充项（长期）

### P3-001 平台兼容性检查（续传 / 盲信扫描强制）
- **问题**：pbin/fpbin/dpbin 的二进制字段宽度（`size_t`、`dev_t`、`ino_t`、`time_t`）与平台相关，跨 x86_64/aarch64、跨 32/64 位、跨大端/小端恢复会错位
- **方向**：
  1. 首次全量扫描时 `{base}.config` 写入系统架构签名：`arch`（x86_64/aarch64）、`endian`（little/big）、`word_size`（32/64）
  2. 续传或盲信扫描时读取 `.config` 中的架构签名，与当前环境比对
  3. 不一致 → 拒绝续传，返回退出码 3，stderr 输出 `[FATAL] 进度文件架构不兼容：期望...实际...请使用 --runone 重新全量扫描`
  4. `.config` 缺失（旧版本进度）→ 视为不兼容，强制 `--runone`
  5. `glibc_version` 作为兼容性参考字段，输出 warning 但不拒绝
- **状态**：Design.md §12.4 已更新
- **评审来源**：本次讨论

---

## 文档矛盾修正（不涉及代码，仅 Design.md 表述）

| 编号 | 问题 | 位置 | 修正方向 | 状态 |
|------|------|------|---------|------|
| DOC-001 | IPC 消息方向表错误：§7.2 混了 M→W 和 W→M | §7.2 | 拆成两个方向表 | 待修正 |
| DOC-002 | pbin 写入者前后不一致：§4.1 vs §5.1 | §4.1 / §5.1 | 明确 batch_processor 写 pbin，async_worker 只写输出 | 待修正 |
| DOC-003 | "pbin 记录所有扫描过的条目"应为"已发现" | §8.1 | 修正语义 | 已修正（v15.5.9 已更新） |
| DOC-004 | Monitor 与 Main 进程收割职责重复 | §5.1 / §5.4 | 明确唯一 owner | 待修正 |
| DOC-005 | "新文件/变更文件输出"与盲信语义矛盾 | §4.3 | 盲信模式下不存在"变更文件" | 待修正 |
| DOC-006 | "减少 90%+ I/O"没有论证 | §10 | 删除或补模型/测试数据 | 待修正 |
| DOC-007 | `timeo=600` 单位错误（应为 6000=600秒） | §12 | 修正挂载参数 | **已修正** |
| DOC-008 | `intr` 在 CentOS 7.4 无效，不应作为关键前提 | §12 | 移除 intr | **已修正** |
---

## 状态汇总（2026-08-17 更新）

### P0 阻塞项（12项）

| 编号 | 问题 | 状态 | 备注 |
|------|------|------|------|
| P0-001 | FINISH/BATCH 竞态 — 目录任务完成屏障 | **Design v1 已确认** | 状态机路径定义完毕 |
| P0-002 | 输出三态状态机 | **Design v1 已确认** | OUTPUT_COMMITTED 态已定义 |
| P0-003 | fpbin 二次崩溃恢复 | **Design v1 已确认** | fpbin+dfpbin 原子对方案 |
| P0-004 | visited_set 背压竞态 — 统一队列模型 | **Design v1 已确认** | 三态拆分 + dspill 统一队列 |
| P0-005 | spbin 纳入恢复路径 | **Design v1 已确认** | 时间窗口 + 敢死队探测 |
| P0-006 | MSG_DROP 正常路径禁止 | **Design v1 已确认** | IPC 队列扩至 65536 |
| P0-007 | RET_ERROR 状态机补全 | **Design v1 已确认** | 写 spbin + 设备级退避 |
| P0-008 | Run manifest + baseline_eligible 原子切换 | **Design v1 已确认** | manifest 升级方案 |
| P0-009 | 目录任务生命周期状态机 | **Design v1 已确认** | 10 态 + 4 异常分支 |
| P0-010 | 崩溃恢复矩阵 — Reset 援救机制 | **Design v1 已确认** | 非终态统一重来 |
| P0-011 | 半增量跳过前提条件声明 | **Design v1 已确认** | 信任模型和两级体系已定义 |
| P0-012 | epoch + waitpid 确认（旧 Worker 残留数据） | **Design v1 已确认** | epoch + waitpid + pipe drain |

**P0 全部 12 项 Design v1 已确认，可进入编码阶段。**

### P1 重要项（10项）
- P1-001 [Covered by P0-004] 已覆盖
- P1-002 [WIP] errno 分类矩阵
- P1-003 [WIP] 设备身份主键改为 (fsid, server, export)
- P1-004 [Covered by P0-005] 已覆盖
- P1-005 [WIP] 设备级熔断 DEGRADED 灰度态
- P1-006 [WIP] IPC 协议显式字段编码 + protocol_version
- P1-007 [NEW] 输出完整性语义声明
- P1-008 [NEW] 输出文件续写语义
- P1-009 [OBSOLETE] 不成立
- P1-010 [NEW] 盲信目录枚举失败的输出语义

### P2 补充项（11项）
全部待设计确认，不阻塞 P0 编码。

### P3 长期项（1项）
- P3-001 [Design.md §12.4 已更新] 平台兼容性检查

### 文档矛盾（9项）
- DOC-007 / DOC-008 / DOC-009 已修正
- DOC-001 / DOC-002 / DOC-004 / DOC-005 / DOC-006 待修正
