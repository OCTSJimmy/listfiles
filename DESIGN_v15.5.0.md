# listfiles v15.5.0 设计总结

> 基于 SEDA 架构缺陷修复、进度系统简化、盲信机制明确的重构设计。

---

## 1. 架构层面：SEDA 阶段间队列修复

### 当前问题
- `lost_tasks` 是全局溢出桶，不是 proper queue
- Stage 3 (`batch_processor`) 同步调用 `send_scan_to_ipc`，Worker 饱和时 3.2M requeues
- `pending_tasks` 语义三处不一致：`batch_processor` ++ 在尝试前，`dispatch.c` ++ 在成功后，`progress_archive.c` 自行 ++/--

### 设计决策
- **引入 `dispatch_queue`**：Stage 3 (batch_processor) 只 push，Stage 4 (main_loop) 只 consume
- **`pending_tasks` 统一语义**："已成功派发到 IPC 且 in-flight 的任务数"
  - 唯一 ++ 点：`send_scan_to_ipc` 成功后
  - 唯一 -- 点：`RET_FINISH` 处理时
  - `process_completed_batch` 中移除 "++ 在尝试前" 逻辑

---

## 2. 进度系统：废除 idx，引入 dpbin

### 核心认知
- pbin 是 **pre-order 发现日志**：目录被父级 `readdir` 发现时写入，不是完成时写入
- 多 Worker 并发导致完成顺序与发现顺序不一致，线性游标无法表达"已完成集合"
- 当前 5 字段 idx (`write_slice_index line_count processed_count output_slice_num output_line_count`) 是 v12.x patchwork，与设计意图背离

### 文件职责

| 文件 | 内容 | 生命周期 | 写入时机 |
|------|------|---------|---------|
| **pbin** | 发现日志（path, dev, ino, mtime, d_type） | 永久，完整分片归档 | 目录/文件被发现时 |
| **dpbin** | 完成日志（目录的 path, dev, ino） | 临时（本次会话） | `process_completed_batch` 成功派发后 |
| **archive** | zlib 压缩的历史 pbin 分片 | 永久 | 正常结束时散落 pbin 封口后追加 |
| ~~idx~~ | ~~5 字段游标~~ | ~~废除~~ | ~~无需~~ |

### dpbin 设计要点
- **只分片，不归档**：不需要长期保存，正常完成后直接删除
- **恢复时加载**：与 pbin 求差集（`pbin - dpbin` = 未完成目录）
- **只 pumping 差集**：避免全量重扫
- **中断后保留**：下次恢复时作为 completed_set 基准

### pbin 散落分片处理（Salvage）
- 不完整分片（Footer 缺失/损坏）：顺序解析找到最后一条完整记录，截断后重新封口
- 完全损坏分片：直接删除，内容在差集中被重扫（最多浪费一个分片）
- 正常分片：封口后追加到 archive

### 恢复流程（续传）
```
1. 解压 archive → visited_set
2. 加载散落 pbin（salvage 有效行）→ visited_set
3. 加载 dpbin → completed_set
4. 差集 = visited_set - completed_set
5. 只 pumping 差集目录
6. 新发现目录 → 写入新 pbin
7. 完成目录 → 写入 dpbin
8. 全部完成后：pbin 归档，dpbin 删除
```

### 输出恢复
- `output_split_dir`：扫描目录取 `max(output_*.txt) + 1`，新建分片
- 单文件输出：直接 `append`，无需恢复行数

---

## 3. 盲信机制（blind-trust）明确

### 启用条件
- **仅在上次完整扫描且正确封口后启用**
- **续传过程禁用盲信**（pbin 不完整，无法确定完整快照）
- 首次扫描禁用盲信

### 信任对象
- **只信任文件**：跳过 `lstat`，直接复用历史 `mtime`
- **不信任目录**：`DT_DIR` 在 `try_blind_trust` 中直接返回 `false`

### 目的
- 不只是时间优化，更是**设备寿命保护**：
  - 减少 HDD 磁头寻道次数
  - 降低 SSD FTL 元数据读放大
  - 减轻 NAS/分布式存储 MDS 节点压力

### 数据流
- 历史 pbin 中解析 `(mtime, d_type)` → `reference_map`
- Worker 通过 COW 共享只读访问
- `readdir` 后：文件且 `now - mtime > skip_interval` → 跳过 `lstat`

---

## 4. 版本信息

| 字段 | 值 |
|------|-----|
| 版本号 | v15.5.0 |
| VERSION_CODE | 202605201550UL |
| 调试日志常量 | 202605201600UL |

---

## 5. 变更范围预估

### 删除/简化
- `idx` 5 字段格式及相关解析逻辑
- `atomic_update_index` 复杂逻辑
- `process_slice_index`、`line_count`、`processed_count`、`output_slice_num`、`output_line_count` 的跨模块传递

### 新增
- `dpbin` 写入模块（`dpbin_append`, `dpbin_rotate_slice`, `dpbin_open_slice`）
- `dpbin` 加载模块（`load_dpbin_to_completed_set`）
- `pbin_salvage` 不完整分片修复逻辑
- `dispatch_queue`（Stage 3→4 队列）

### 修改
- `process_completed_batch`：写入 dpbin 时机、pending_tasks ++ 时机
- `restore_progress`：废除 idx 恢复，改为 archive + pbin salvage + dpbin 差集
- `main_loop`：pumping 逻辑改为差集遍历
- `try_blind_trust`：增加 `DT_DIR` 硬过滤
- `shutdown_progress` / 正常结束路径：pbin 归档 + dpbin 删除

### 文件清单
- `include/core/config.h` — VERSION 更新
- `src/output/progress_io.c` — dpbin 写入、pbin salvage
- `src/output/progress_archive.c` — 恢复逻辑重写
- `src/scan/batch_processor.c` — dpbin 写入点、pending_tasks 修正
- `src/scan/dispatch.c` — dispatch_queue 引入
- `src/scan/main_loop.c` — 差集 pumping、dpbin 删除
- `src/scan/worker_scanner.c` — 目录盲信过滤
- `CHANGELOG.md` / `Design.md` — 文档同步
