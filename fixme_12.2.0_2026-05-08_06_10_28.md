# listfiles 待修复问题清单

**版本**: 12.2.x  
**扫描日期**: 2026-05-08 06:10:28  
**扫描范围**: `progress.c`, `main_loop.c`, `app_context.h`, `config.h`, `async_worker.c`, `output.c`  
**测试环境**: Linux x86_64, GCC (GNU11), 代码静态分析  

---

## 1. [严重] `process_slice_index` 为僵尸字段，断点续传缺乏"已处理"游标

- **优先级**: P0 — 架构级缺陷
- **影响模块**: `progress.c`, `config.h`, `main_loop.c`, `app_context.h`
- **根因分析**:
  1. `RuntimeState` 中定义了 `write_slice_index` 和 `process_slice_index`，但 `process_slice_index` **在整个代码库中没有任何读取引用**。
  2. `load_progress_index()` 中硬绑定：`state->process_slice_index = state->write_slice_index;`，之后运行时代码从未更新它。
  3. 这意味着系统无法区分以下三种状态：
     - **已持久化**：已经写入 pbin 并落盘（`write_slice_index`）
     - **已处理**：Master 已去重、已输出、已决定分发的路径
     - **已发送**：已经通过 `ipc_send` 发给 Worker 但尚未返回
  4. 由于只有"已持久化"一级游标，且 `.idx` 更新频率极低（仅分片轮转和正常退出），崩溃恢复时存在巨大的"真空带"——大量已处理但未持久化的记录会被重复扫描和输出。
- **真空带大小估算**:
  ```
  RecordBatch 缓冲:   最多 4096 条（内存缓冲未 flush）
  + 分片内未轮转:     最多 99,999 条（默认 progress_slice_lines=100000）
  + 输出线程缓冲:     最多 256 条（async batch）
  ─────────────────────────────────────────────
  最大真空带:         ~104,351 条记录
  ```
- **复现场景**:
  ```bash
  # 扫描一个 50 万文件的目录树
  ./bin/listfiles --path=/data --continue --progress-file=task1
  # 在运行 30 秒后发送 SIGKILL（模拟断电/OOM killed）
  kill -9 $(pgrep listfiles)
  # 再次恢复
  ./bin/listfiles --path=/data --continue --progress-file=task1
  # 结果：重复输出数万条已处理记录
  ```
- **修复建议（架构级改造）**:
  引入**三级游标**设计，替代当前单级游标：
  | 游标 | 内存字段 | 含义 | 更新时机 |
  |------|---------|------|---------|
  | **Dispatch Cursor** | `dispatch_slice_index` + `dispatch_line` | 已经发给 Worker 的目录位置 | 每次 `ipc_send(IPC_MSG_SCAN)` 后 |
  | **Process Cursor** | `process_slice_index` + `process_line` | Worker 已返回、Master 已去重完成的目录位置 | 每批 `process_completed_batch()` 后 |
  | **Persist Cursor** | `write_slice_index` + `line_count` | 已经写入 pbin 并落盘的位置 | 每次 `record_path()` 后（或至少每批 flush 后） |
  - `atomic_update_index()` 应同时记录三级游标（或至少 Process + Persist 两级）。
  - `.idx` 文件格式扩展为：`dispatch_s d_line process_s p_line write_s w_line output_s o_line`。
  - 恢复时，`visited_set` 应加载到 **Process Cursor** 位置（而非 Persist Cursor），确保已处理的记录不会重复扫描。
- **涉及文件**: `include/config.h:207`, `src/progress.c:285-328`, `src/main_loop.c:101-182`, `include/app_context.h:72-88`

---

## 2. [严重] `.idx` 更新频率过低，崩溃恢复精度极差

- **优先级**: P0 — 与问题 1 强相关
- **影响模块**: `progress.c`, `main_loop.c`
- **根因分析**:
  1. `atomic_update_index()` 仅在两种情况下被调用：
     - `record_path()` 中分片轮转时（`line_count >= progress_slice_lines`，默认 10 万）
     - `finalize_progress()` 正常退出时
  2. 这意味着在扫描一个 9 万文件的目录时，**整个运行期间 `.idx` 一次都不会被更新**。
  3. 如果此时发生异常崩溃（SIGKILL、OOM、断电），恢复时 `.idx` 要么不存在（中小规模首次扫描），要么停留在上轮分片的位置（大规模扫描），导致大量重复处理。
  4. 从 `fixme_12.0.0_2026-05-06_10_40_55.md` 的 **BUG-005** 可知，早期版本甚至 `finalize_progress()` 不调用 `atomic_update_index()`，导致中小规模断点续传完全失效。当前版本的修复只是"补丁式"地在退出前补写一次 `.idx`，并未解决根本问题。
- **修复建议**:
  1. **引入定时/定量 `.idx` 刷新机制**：
     - 每处理 N 条记录（如每 1000 条）或每 T 秒（如每 5 秒），调用 `atomic_update_index()`。
     - 使用 `setitimer` 或主循环超时计数器实现，避免额外线程。
  2. **`record_path_batch_flush()` 后强制刷新 `.idx`**：
     - 当前 `RecordBatch` 满时（4096 条或 1MB）会 flush 到 pbin，但 flush 后**不更新 `.idx`**。
     - 应在 `record_path_batch_flush()` 返回后，立即调用 `atomic_update_index()`。
  3. **输出线程切分后同步 `.idx`**：
     - 当 `output_slice_num` 递增时（`rotate_output_slice`），应触发 `atomic_update_index()`，保证输出游标与进度游标的一致性。
- **涉及文件**: `src/progress.c:206`, `src/progress.c:230-260`, `src/main_loop.c:351`, `src/output.c:438-463`

---

## 3. [高] `process_slice_index` 与 `write_slice_index` 硬绑定导致语义混乱

- **优先级**: P1
- **影响模块**: `config.h`, `progress.c`
- **根因分析**:
  1. 在 `load_progress_index()` 中：`state->process_slice_index = state->write_slice_index;`
  2. 开发者意图可能是区分"逻辑处理位置"和"物理写入位置"，但实现时放弃了这个区分，直接把两者绑定。
  3. 这导致后续任何想利用 `process_slice_index` 做精细恢复的人都无从下手——它不是独立的游标，只是 `write_slice_index` 的别名。
- **修复建议**:
  - 如果短期内不做问题 1 的三级游标改造，至少应该**删除 `process_slice_index`**，避免误导后续维护者。
  - 或者将其真正用起来：在 `process_completed_batch()` 末尾（每批处理完成后）更新 `process_slice_index = write_slice_index`，并在 `line_count` 变化时同步更新。这样即使与 `write_slice_index` 接近，也至少表达了"这批已经处理完"的语义。
- **涉及文件**: `src/progress.c:326`, `include/config.h:207`

---

## 4. [高] 恢复时 `visited_set` 加载量与 `.idx` 不同步

- **优先级**: P1
- **影响模块**: `progress.c`
- **根因分析**:
  1. `restore_progress()` 中，对于活跃分片（`s_idx == write_slice_index`）：
     ```c
     parse_pbin_buffer(buf, data_size, ctx->state.line_count, ctx->visited_set, NULL, NULL);
     ```
  2. 这里用 `.idx` 中的 `line_count` 来限制加载到 `visited_set` 的记录数。
  3. 但 `line_count` 反映的是**已持久化**的行数，而实际**已处理**的行数可能远超这个值（因为 `RecordBatch` 缓冲、输出线程缓冲、分片内未轮转）。
  4. 结果是：`visited_set` 中缺少大量实际上已经处理过的记录，恢复后这些记录会被当作"新发现"重新入队扫描，导致**重复输出**。
- **修复建议**:
  - 引入独立的 **Process Cursor**（见问题 1），`visited_set` 加载到 Process Cursor 位置。
  - 如果没有 Process Cursor，作为折中方案：恢复时将整个活跃分片（甚至整个 pbin）全部加载到 `visited_set`。这会导致该分片内的所有记录被当作"已访问"而跳过，可能漏扫崩溃前尚未实际处理的几条记录，但比"重复扫描数万条"要好得多。
- **涉及文件**: `src/progress.c:1031-1034`

---

## 5. [中] `RecordBatch` flush 与 `.idx` 更新之间存在原子性缺口

- **优先级**: P2
- **影响模块**: `progress.c`
- **根因分析**:
  1. `record_path_batch_flush_internal()` 遍历 batch，逐个调用 `record_path()`，后者执行 `fwrite` 到 `write_slice_file`。
  2. `fwrite` 是缓冲 IO（stdio），数据可能仍在 libc 缓冲区中，未真正到达内核页缓存。
  3. 如果此时崩溃，pbin 文件中实际落盘的数据可能少于 `line_count` 所声称的数量。
  4. 恢复时，`.idx` 声称有 N 条记录，但 pbin 实际只有 M 条（M < N），`parse_pbin_buffer` 读到 EOF 时可能把不完整的记录解析为垃圾数据。
- **修复建议**:
  - `record_path_batch_flush()` 返回后，调用 `fflush(state->write_slice_file)` 强制刷出 stdio 缓冲区。
  - 在 `atomic_update_index()` 中，先 `fflush` 活跃分片文件，再写 `.idx`。
  - 或者，使用 `setvbuf(_IONBF)` 对进度文件禁用缓冲，以性能换一致性。
- **涉及文件**: `src/progress.c:219-228`, `src/progress.c:285-316`

---

## 总结

| 问题 | 优先级 | 本质 |
|------|--------|------|
| 1. 僵尸 `process_slice_index` / 缺乏多级游标 | P0 | 架构缺陷：只有一级游标，无法区分"已发送/已处理/已持久化" |
| 2. `.idx` 更新频率过低 | P0 | 工程缺陷：崩溃恢复时真空带可达 10 万+ 条记录 |
| 3. `process`/`write` 硬绑定 | P1 | 语义混乱：字段存在但无独立语义 |
| 4. `visited_set` 加载与 `.idx` 不同步 | P1 | 恢复缺陷：重复扫描的根源 |
| 5. Batch flush 与 idx 原子性缺口 | P2 | 一致性风险：stdio 缓冲导致 pbin 与 idx 不一致 |

**建议修复顺序**:
1. **立即（P0）**: 如果短期内不做架构改造，至少应删除 `process_slice_index` 避免误导，并在 `record_path_batch_flush()` 后强制调用 `atomic_update_index()`，将真空带从 10 万条降低到 4096 条。
2. **本周（P0）**: 设计并实施三级游标（或至少两级：Process + Persist），这是断点续传功能"可用"与"可靠"的分水岭。
3. **下次迭代（P1-P2）**: 修复 flush 原子性和 `visited_set` 加载策略。
