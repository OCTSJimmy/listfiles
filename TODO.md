# 代码注释任务清单

> 目标：为所有 .c 文件添加简体中文注释，包括文件头注释和函数注释（参数类型、作用、取值范围、返回值类型、作用、取值范围）。
> 忽略：.git/ 目录、.gitignore 中列出的目录/文件。

---

## v15.5.8 已修复项（2026-07-29）

- [x] **pbin 滑动窗口兜底卡死（R3 漏采主谋）**：`load_dirs_from_pbin()` 游标追到被 `process_old_slice()` 轮转删除的分片后永久卡死，HIGH_WATER 跳推目录整子树静默丢失。改用运行级追加文件 `.dspill`（无轮转、无删除竞争、字节游标、只含跳推目录）。
- [x] **完结硬性断言**：终止前 dspill 必须排空到 EOF——有产出先回填派发，残留记 `DSPILL_RESIDUE` 熔断清单并非零退出（R3 "pending=4 仍 SUCCESS" 类路径封死）。
- [x] **MSG_DROP 销账**：Worker 拒收回队时 `pending_tasks-1`；回队失败记 `TASK_DROP_LOST`。
- [x] **nlink oracle（`--strict-nlink`，默认关）**：`st_nlink-2` vs readdir 子目录计数，失配记 `NLINK_MISMATCH`——捕获 NFS 无 errno 假空/假 EOF（errno 检测族原理性盲区，纯文件目录除外）。
- [x] **回归测试集**：`tests/` 新增 LD_PRELOAD 无 errno 注入 shim、已知真值 fixture 生成器（7 万条目/深路径/GBK/软链/并发删除）、10 用例回归脚本（全绿）。

## v15.5.7 已修复项（2026-07-28）

- [x] **错误上报通道修复**：`IPC_MSG_ERROR` 从 `fd_data` 改到 `fd_ctrl`（此前被 Master 当垃圾帧 drain，scanner 自检的目录级错误永远到不了熔断清单）。
- [x] **目录级错误全量上报**：除 ENOENT/ENOTDIR 竞态外的 errno 全部上报；Master 记录 `DIR_ERROR(errno=N)`，不触发设备惩罚。
- [x] **条目级错误上报**：新增 `IPC_MSG_ENTRY_ERROR`/`RET_ENTRY_ERROR`；条目 `lstat/stat` 失败（非竞态）与路径截断记录 `ENTRY_ERROR(errno=N)`；条目 stat 带 EINTR 重试。
- [x] **readdir 中途失败检测**：`readdir` errno 检查，中途失败按目录级错误上报（此前超大目录部分条目静默丢失）。

## v15.5.4 已修复项（2026-07-28）

- [x] **熔断清单**：新增 `{progress_base}.circuit_breaker` 独立审计文件，记录所有 `BLACKLIST` / `DEV_TIMEOUT` / `EIO` / `PATH_TIMEOUT` / `CONDEMNED` 跳过路径。
- [x] **退出码与警示**：只要 `skipped_count > 0`，程序返回退出码 1，`stderr` 输出 `[CRITICAL] 扫描不完整...`，`.config` 写入 `Incomplete`。
- [x] **探测指数退避判死**：修复 `reap_probes()` 每次失败后重置 `retry_count` 的问题；`probe_interval` 真正翻倍（上限 300s），达到 `PROBE_MAX_RETRIES`（6 次）后判死 `CONDEMNED`；最后两次重试使用 15s 超时。
- [x] **Monitor 刷屏**：`TERM=dumb` 或非 tty 时不再发送 `\033[2J\033[H`。

## v15.5.4 待办项

- [x] **DEV_TIMEOUT/EIO 上报 dev=0 修复**：Worker/IPC 层正确传递当前任务的 `st_dev` 到 `IpcErrorHeader.dev`（v15.5.6 完成）。
- [x] **单挂载 NFS 设备熔断保护**：`RuntimeState.root_dev` 记录根路径设备号，`batch_processor` 中与根路径同设备时禁用设备级跳过（v15.5.6 完成）。
- [ ] **熔断清单入仓**：把 `.circuit_breaker` 结构化导入 CK 元表，供运维分析（路线 C3）。

---

## 模块划分与进度

### 1. 工具与杂项模块 ✅
- [x] `src/xxhash.c` — xxHash3 128-bit 哈希实现封装 (3 行)
- [x] `src/utils.c` — 通用工具函数 (44 行)
- [x] `src/signals.c` — 信号处理 (95 行)

### 2. 数据结构模块 ✅
- [x] `src/reference_map.c` — 指纹→(mtime,d_type) 映射，支撑半增量 blind-trust (108 行)
- [x] `src/fingerprint_set.c` — xxHash3 128-bit 分片开放寻址哈希集合，用于去重 (168 行)

### 3. 设备管理模块 ✅
- [x] `src/device_manager.c` — 设备状态机管理 (111 行)
- [x] `src/probe_scheduler.c` — 渐进探测调度器，指数退避策略 (145 行)

### 4. 并发与线程模块 ✅
- [x] `src/async_worker.c` — 异步输出工作线程 (114 行)
- [x] `src/thread_pool.c` — Master 内嵌 CPU 去重线程池 (180 行)

### 5. 命令行与入口模块 ✅
- [x] `src/cmdline.c` — 命令行参数解析 (243 行)
- [x] `src/main.c` — 程序主入口，初始化与资源清理 (316 行)

### 6. Worker 与主循环模块 ✅
- [x] `src/worker_proc.c` — Worker 子进程实现，目录遍历与 IPC (428 行)
- [x] `src/main_loop.c` — Master epoll 主循环，处理 IPC 消息 (492 行)

### 7. 输出与监控模块 ✅
- [x] `src/output.c` — 格式化输出引擎 (542 行)
- [x] `src/monitor.c` — 监控线程，统计面板与心跳检查 (294 行)

### 8. 进度与归档模块 ✅
- [x] `src/progress.c` — 进度文件（pbin/spbin/fpbin）的写入、归档、恢复 (1264 行)

---
✅ 全部完成！总计 16 个 .c 文件，约 4547 行代码。

---

# 平台化路线图（采集端 → 数据资产 → 治理平台）

> 上下文：listfiles 是「元数据采集管理分析平台」的采集端，下游链路为
> `listfiles → Bash 胶水 → ClickHouse 集群 → Superset → AI/Agent/MCP`。
> 本节记录基于该链路的设计决策与待办事项，作为后续版本演进的指导。

## A. CSV 输出与 ClickHouse 兼容性（采集端契约层）

listfiles 的 CSV 是整条数据管线的契约，必须严格兼容 ClickHouse `INSERT FORMAT CSV` / `CSVWithNames` 的 RFC 4180 语义。

- [ ] **A1. `--csv` 默认输出 `CSVWithNames`**：首行字段名，CK 直接 `INSERT FORMAT CSVWithNames` 灌入，胶水层无需维护字段顺序。
- [ ] **A2. 新增 `--format-preset=clickhouse`**：固化标准字段组合，避免每个采集任务手写格式串。建议字段：`%p\t%s\t%u\t%U\t%g\t%G\t%m\t%c\t%i\t%O\t%t`。
- [ ] **A3. 时间字段双输出**：`mtime_ts`（Unix epoch，CK 用）+ `mtime_iso`（人可读）；或仅输出 epoch 由 CK `toDateTime()` 转换。
- [ ] **A4. 路径硬过滤**：CSV 严格模式遇到含 `\0` 等致命控制字符的路径，跳过并写 stderr，避免整批 CK reject。
- [ ] **A5. `--emit-ddl`**：根据当前 `--format` 输出对应 CK `CREATE TABLE` 语句，让胶水层无脑 `clickhouse-client < ddl.sql`。
- [ ] **A6. 字段语义防漂移**：`--format` 串与 CK DDL 由同一生成器产出，禁止两边手写。

## B. 多格式输出后端（中期）

- [ ] **B1. writer 抽象层**：在 `output_format.c` 增加格式后端接口。
- [ ] **B2. `--format=jsonl`**：xattr / ACL 多值场景。
- [ ] **B3. `--format=rowbinary`**：直怼 CK Native，省 50%+ 体积与解析 CPU。
- [ ] **B4. `--format=parquet`**：作为离线分析/数据湖归档格式（评估 Apache Arrow C 解码或外部 stage）。

## C. AI/MCP 接入与采集元元数据（长期）

- [ ] **C1. fingerprint 直接外露**：将 xxHash3-128 作为 `path_fp UInt128` 列输出，CK 中两次扫描差集变 O(n) anti-join。
- [ ] **C2. 采集任务 ID**：每次扫描生成 UUID，所有行携带 `task_id`；Superset/AI 按任务切片。
- [ ] **C3. dpbin/pbin/spbin 入仓**：把"采集元元数据"（任务、起止时间、设备熔断列表、扫描行数、Footer CRC）入 CK 元表，供运维与 AI 分析"为什么这次少了 800 万文件"使用。
- [ ] **C4. MCP 工具集**：暴露 `query_fs_metadata(sql)` / `trigger_scan(path, opts)` / `diff_scans(task_a, task_b)` / `get_scan_health(task_id)`，让 Agent 能闭环动手。

## D. 下游中间件选型决策（基线方案）

基线保持 `listfiles → Bash → CK` 直管，**不为"未来可能"提前引入中间件**。中间件触发条件：

- [ ] **D1. Kafka**：仅当出现 ≥2 个数据消费方（CK + ES + 数据湖等）、跨机房汇聚、或 CK 频繁停机影响采集可用性时引入。**不要让 listfiles 自己写 Kafka SDK**，由 filebeat/vector 旁路读取 CSV split 推 Kafka。
- [ ] **D2. Flink**：仅当业务方提出"分钟级实时变更感知"、CK 撑不住差集计算、或需要 CEP 异常告警时引入；必须配合 Kafka。
- [ ] **D3. RabbitMQ/RocketMQ**：**永不引入**。AMQP 模型不适合 log-stream 元数据；如有人坚持，先反问"用 Kafka 不行吗"。

## E. CK 数据资产化（≥10 亿行后启用）

### E1. 基础设施
- [ ] **E1.1** 分区与排序键：`PARTITION BY (toYYYYMM(scan_time), host)`、`ORDER BY (host, mount_point, path_fp)`。
- [ ] **E1.2** 路径检索索引：
  - `ngrambf_v1(4, 1024, 3, 42)` 任意子串
  - `tokenbf_v1(32768, 3, 0)` 分词
  - 物化列 `ext` + `bloom_filter` 索引
- [ ] **E1.3** TTL 冷数据下沉至 S3 兼容存储；AI 查询走 readonly 副本，主集群只接受批量灌入。
- [ ] **E1.4** 中文检索：路径关键 token 拼音化预处理（胶水层 pypinyin），新增 `path_pinyin` 列。

### E2. 标准 SQL 报表（沉淀为 Superset 仪表盘）
- [ ] **E2.1 容量画像**：TOP 大文件、目录容量重心、owner 容量榜。
- [ ] **E2.2 冷热识别**：90/180/365 天 atime 分桶，归档候选清单。
- [ ] **E2.3 文件类型分布**：后缀×容量矩阵，识别误存。
- [ ] **E2.4 重复粗检**：size + mtime + basename 同值聚合。
- [ ] **E2.5 安全审计**：SUID/SGID、world-writable、孤儿文件、敏感目录权限漂移。
- [ ] **E2.6 跨次差集**：基于 `task_id` + `path_fp` 的新增/删除/变更，目录维度异常激增检测（勒索病毒征兆）。
- [ ] **E2.7 SRE 健康**：设备熔断热力图、扫描完整性、扫描耗时趋势、inode 接近耗尽预警。
- [ ] **E2.8 容量预测**：6 个月 owner 容量线性回归预测，用于配额/预算。

### E3. 标准 Superset 仪表盘
- [ ] 容量总览（CTO/IT 总监）
- [ ] 冷数据治理（存储管理员）
- [ ] 变更追踪（SRE/安全）
- [ ] 安全审计（安全团队）
- [ ] 存储健康（运维）
- [ ] 预测面板（财务/容量规划）

### E4. AI/Agent 闭环
- [ ] **E4.1 NL2SQL 问数**：schema + few-shot 喂 LLM；强制走 readonly user + query timeout + memory limit。
- [ ] **E4.2 异常检测 Agent**：每日 24h 差集喂检测 prompt，输出疑似异常清单 + 钉钉/飞书告警。
- [ ] **E4.3 向量化路径检索**：高频目录 embedding 入向量库，支持"机器学习模型 checkpoint 相关目录"语义搜索。

## F. 临床研究/医学影像数据治理（业务上层）

> 把 listfiles 从「采集器」升级为「临床数据资产管理平台」入口的核心工作。

### F1. 三级目录识别（Project / Site / Subject）— 三层管线

架构：**规则引擎（80% 命中）→ 项目代号知识库（canonicalization）→ LLM 兜底（low-confidence 入人工审核）→ 审核结果回流知识库**。

- [ ] **F1.1 规则引擎**：YAML/正则可热更新规则集，存 CK `path_classification_rules` 表；预置 `standard_clinical` / `ich_gcp_style` / `legacy_chinese` 等模式。
- [ ] **F1.2 物化列**：`project` / `site` / `subject` 由 CK `MATERIALIZED extract(path, ...)` 直接生成，**不改 listfiles**。
- [ ] **F1.3 项目代号字典**：`project_dict (canonical, alias, type=exact|typo|abbr)`，消除多种叫法/拼写错误。
- [ ] **F1.4 LLM 兜底**：本地 Qwen2.5-7B / Llama3.1-8B 跑批，输出 `(project, site, subject, confidence)`；conf>0.9 自动入库，0.6–0.9 入审核队列，<0.6 标 unknown。**禁止用 GPT-4 跑全量**。
- [ ] **F1.5 人工审核回流**：审核结果沉淀回规则/字典，每次审核都让规则覆盖率提高。
- [ ] **F1.6 标准报表**：项目维度汇总、项目→分中心分布、分中心→受试者数据量。

### F2. 影像知识层（DICOM 序列完整度）

- [ ] **F2.1 三层文件分类**：L1 后缀+大小启发（SQL）→ L2 文件头魔数（DICM at offset 128）→ L3 DICOM Tag 深度解析（pydicom/dcmtk）。
- [ ] **F2.2 listfiles 增加 `--magic-probe`**：对 size 在合理区间的可疑文件做 L2 探测（只读前 1KB），把 magic 类型作为新字段输出 CSV；亿级文件按 ~10% 候选率，IO 增量可控。
- [ ] **F2.3 DICOM 元数据表**：`dicom_metadata (file_path, project, site, subject, study_uid, series_uid, instance_number, modality, rows, cols, series_description)`，按 `(project, site, subject, study_uid, series_uid, instance_number)` 排序。
- [ ] **F2.4 序列完整度 SQL**：基于 `instance_number` 连号断裂检测；输出每个 incomplete series 缺哪几张。
- [ ] **F2.5 受试者数据质量记分卡**：`completeness_score` + `modalities_present` + 关键模态缺失标记，给 PI / 数据经理决定"可入库锁定 vs 需补扫"。

### F3. 超大规模关键字检索（10 亿行级）

- [ ] **F3.1** 前缀 → minmax 索引 + ORDER BY（ms 级）。
- [ ] **F3.2** 后缀/扩展名 → 物化列 + bloom_filter。
- [ ] **F3.3** 任意子串 → `ngrambf_v1(4, ...)` 跳数索引。
- [ ] **F3.4** 分词模糊 → `tokenbf_v1` 或外接 ES。
- [ ] **F3.5** 语义检索 → 路径 embedding 入 Milvus / CK vector search。
- [ ] **F3.6** 中文医学拼音/缩写检索 → `path_pinyin` 物化列。

### F4. 主备控制与备份策略（最具战略价值）

- [ ] **F4.1 主备一致性比对**：主备存储都跑 listfiles，写同一表多一列 `storage_tier`；SQL 直出"备份缺失 / 误删检测 / size·mtime 不一致"日报。
- [ ] **F4.2 分层备份策略**：基于 atime/mtime/project 自动打 `tier1_realtime` ~ `tier4_archive_only` 标签；量化"原 1PB/天 → 50TB 实时 + 200TB 每日 + 750TB 冷"的收益。
- [ ] **F4.3 GCP 保留期合规**：每个项目的 retention_until + 合规存储覆盖率（vault_coverage），稽查直接出证据。
- [ ] **F4.4 Policy-driven 引擎**：CK 元数据 → SQL 规则集 → 决策事件（归档/删除/锁定/同步）→ 执行层（rsync/aws cli/Iceberg）；Policy 全部 SQL 定义、版本化在 git。

### F5. PHI 隐私扫描

- [ ] **F5.1** 路径 PHI 检测：中文姓名+日期、身份证号、病案号正则；首次发现脱敏漏洞优先处置。
- [ ] **F5.2** GCP/HIPAA 合规报告自动生成。

### F6. 跨研究中心幽灵数据检测

- [ ] **F6.1 跨项目重名受试者**：同 subject 出现在多个 project 下的混淆检测。
- [ ] **F6.2 野生项目代号**：不在白名单的项目代号清单（数据漂移/测试数据未删/编码错误）。

## G. 落地优先级（建议执行顺序）

| 优先级 | 工作 | 工期 | 依赖 |
|---|---|---|---|
| P0 | 三层目录规则引擎 + 物化列（F1.1–F1.2、F1.6） | 1 周 | 无 |
| P0 | 项目代号字典 + canonicalization（F1.3） | 1 周 | 业务方提供项目清单 |
| P0 | ngram/token 索引（F3.3–F3.4） | 2 天 | 无 |
| P0 | CK 基础设施（E1.1–E1.3） | 1 周 | 无 |
| P0 | CSV 默认 CSVWithNames + format-preset（A1–A2） | 3 天 | 无 |
| P1 | 主备一致性比对（F4.1） | 1 周 | 备份机也跑 listfiles |
| P1 | DICOM 元数据采集 + 序列完整度（F2.1–F2.5） | 2–3 周 | pydicom 解析侧链 |
| P1 | 标准 SQL 报表 + Superset 仪表盘（E2、E3） | 2 周 | E1 完成 |
| P1 | 采集任务 ID + dpbin 入仓（C2–C3） | 1 周 | 无 |
| P2 | AI 兜底标注 + 知识库回流（F1.4–F1.5） | 1 个月 | 本地 LLM 部署 |
| P2 | 备份分层策略引擎（F4.2–F4.4） | 2 周 | F4.1 完成 |
| P2 | NL2SQL 问数（E4.1） | 2 周 | E1、E2 完成 |
| P3 | PHI 扫描 + 幽灵数据检测（F5、F6） | 1 周 | 安全/合规需求触发 |
| P3 | 异常检测 Agent + MCP 工具集（E4.2、C4） | 1 个月 | NL2SQL 完成 |
| P3 | 多格式输出后端（B） | 按需 | writer 抽象先行 |

---

> 备注：以上路线的目标是把"采集 + 存储 + 治理"做成一条独立可控的链路，对标商业 CRO 数据管理平台与 NetApp xcp 等专有方案，**用 listfiles + CK + Superset + AI 实现 70% 功能**。基础设施（规则引擎 + 项目字典 + CK 索引）一旦立住，上层应用扩展成本极低。
