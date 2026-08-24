# Design-todo-v15.7.0.md

> 本文档是 **v15.7.0 规划文档**，仅保留尚未编码实现的待设计/待修复项。当前代码事实请参考 `Design.md`（v15.6.0）。
>
> **状态（2026-08-21）**：v15.6.0 的 12 项 P0（含 §0 待修正项 0.1-0.7）已于 2026-08-20 全部编码落地，通过回归（`tests/run_regression.sh` 17/17）与并发压测（6 实例 × 20 轮 × worker-count 16，120/120），设计实现细节与实现期缺陷实录归档于 `fix_documents/fixed_15.6.0_P0_all_in_one.md`；原 v15.6.0 规划全文可从 git 历史中的 `Design-todo-v15.6.0.md` 查阅。
>
> P1/P2/P3 按既定策略待生产环境验证 P0 后推进。
>
> **2026-08-24 插单**：生产环境 /public4 全量扫描事故（6/8 Worker 死亡、pending_tasks=-6、
> 主循环停摆）触发的热修复 **v15.6.1 已于 2026-08-25 落地并通过全部验证**
> （根因链 R1-R9 与 P0-101~109 见 `Design-todo-v15.6.1.md`，实现记录见
> `fix_documents/fixed_15.6.1_P0_incident_hotfix.md`）。本文档所有条目顺延至
> v15.6.1 生产验证（/public4 从头全量重跑）通过后推进。
>
> **2026-08-24 用户决策**：盲信（--reference-base / blind_trust）是最不重要的功能之一，
> **整体押后至最后**——其余全部功能（含续传）生产验证 OK 之前，不投入任何盲信相关工作；
> 本文档中涉及盲信的条目（P1-007 部分、P1-010、P3-001 部分等）按此约束排优先级。

---

## 已闭环归档（v15.6.0，不再跟踪）

- §0 待修正项 0.1-0.7（评审修正）——全部随 P0 编码落地。
- P0-001 ~ P0-012——全部落地，见归档文档。
- P1-001 恢复时加载 dspill 并集——已纳入 P0-004 统一队列模型落地。
- P1-004 毒丸目录清单——已纳入 P0-005 spbin 扩展落地（reason 码 POISON，致死 3 次永久隔离）。
- P1-009 pbin 文本格式 → 二进制安全编码——不成立（pbin 本来就是二进制）；衍生的平台宽度问题见 P3-001。

---

## P1 重要项（v15.6.0 P0 已经生产验证后优先推进）

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
| DOC-001 | IPC 消息方向表错误：§7.2 混了 M→W 和 W→M | §7.2 | 拆成两个方向表 | 已修正（v15.6.0 Design.md 同步） |
| DOC-002 | pbin 写入者前后不一致：§4.1 vs §5.1 | §4.1 / §5.1 | 明确 batch_processor 写 pbin，async_worker 只写输出 | 已修正（v15.6.0 Design.md 同步） |
| DOC-003 | "pbin 记录所有扫描过的条目"应为"已发现" | §8.1 | 修正语义 | 已修正（v15.5.9） |
| DOC-004 | Monitor 与 Main 进程收割职责重复 | §5.1 / §5.4 | 明确唯一 owner | 已修正（v15.6.0 Design.md 同步） |
| DOC-005 | "新文件/变更文件输出"与盲信语义矛盾 | §4.3 | 盲信模式下不存在"变更文件" | 已修正（v15.6.0 Design.md 同步） |
| DOC-006 | "减少 90%+ I/O"没有论证 | §10 | 删除或补模型/测试数据 | 已修正（v15.6.0 Design.md 同步） |
| DOC-007 | timeo=600 单位错误（应为 6000=600秒） | §12 | 修正挂载参数 | **已修正** |
| DOC-008 | intr 在 CentOS 7.4 无效，不应作为关键前提 | §12 | 移除 intr | **已修正** |
| DOC-009 | pbin 格式描述错误："文本格式"实际是二进制 | §8.1 | 修正为二进制格式 + 平台兼容性说明 | **已修正** |

---

---
