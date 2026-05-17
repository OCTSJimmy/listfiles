# listfiles 待修复问题清单

**版本**: 12.2.0  
**扫描日期**: 2026-05-07 14:32:47  
**扫描范围**: 全量源码 + 功能测试 + 边界测试 + GDB 调试  
**测试环境**: Linux x86_64, GCC (GNU11), zlib1g-dev  

---

## 1. [严重] 断点续传恢复时读取 Footer 导致崩溃

- **优先级**: P0 — 阻断性缺陷
- **影响模块**: `progress.c`, `main_loop.c`
- **复现命令**:
  ```bash
  ./bin/listfiles --path=/usr/include --yes --continue --progress-file=/tmp/prog --output=/tmp/out.txt
  # 正常完成后再执行一次：
  ./bin/listfiles --path=/usr/include --yes --continue --progress-file=/tmp/prog --output=/tmp/out2.txt
  # 结果：致命错误: 内存分配失败（exit 1）
  ```
- **根因分析**:
  1. 扫描正常结束时，`finalize_archive()` 为最后一个活跃分片写入 `Footer`（`magic=0xDEADBEEF66AAC0FF`, `row_count=N`）。
  2. `atomic_update_index()` 记录 `line_count=N`。
  3. 恢复时，`restore_progress()` 打开该分片，用 `read_next_pbin_record()` 跳过 `line_count` 行。由于 `line_count == row_count`，跳过所有记录后文件指针恰好停在 **Footer 开头**。
  4. `main_loop_run()` 调用 `pump_pbin_batch()`，继续读取时把 Footer 的 8 字节 `magic` 解析为 `size_t path_len`（值约 `0xDEADBEEF66AAC0FF`）。
  5. `safe_malloc(path_len + 1)` 请求约 506 GB 内存，失败后直接 `exit(EXIT_FAILURE)`。
- **修复建议**:
  - **防御性**: 在 `read_next_pbin_record()` 中，读取 `path_len` 后立即校验合理性（如 `path_len > MAX_PATH_LENGTH` 则返回 `false`），避免在分配内存前崩溃。
  - **根本性**: `restore_progress()` 在跳过已处理行后，应检查文件指针是否已到达 EOF / Footer。若已读完所有记录，不应将 `hist_pump_state` 设为 `HIST_PUMP_OLD`，而应设为 `HIST_PUMP_DONE`。
- **涉及文件**: `src/progress.c:859-888`, `src/progress.c:1071-1087`, `src/main_loop.c:317-319`

---

## 2. [高] `--help` 返回非 0 退出码

- **优先级**: P1
- **影响模块**: `cmdline.c`
- **复现命令**: `./bin/listfiles --help; echo $?` → 输出 `1`
- **根因分析**: `cmdline.c:204` `case 'h': default: show_help(); return -1;`，`main.c:174` 将其转换为 `return 1`。
- **修复建议**: `--help` 和 `--version` 应单独处理，返回 `0`。`default` 分支（未知选项）可返回非 0，但不应与 `-h` 共用同一段代码。
- **涉及文件**: `src/cmdline.c:204`, `src/cmdline.c:210`

---

## 3. [高] `--mute` 选项完全未实现

- **优先级**: P1
- **影响模块**: `async_worker.c`, `output.c`, `main_loop.c`, `cmdline.c`
- **复现命令**:
  ```bash
  ./bin/listfiles --path=/tmp/test_scan --yes --mute >/tmp/mute_out.txt 2>/dev/null
  wc -l /tmp/mute_out.txt   # 结果：4 行（应为 0 行）
  ```
- **根因分析**: `cfg->mute` 在 `init_config()` 和 `parse_arguments()` 中被正确解析为 `true`，但 **没有任何输出模块检查该字段**。`async_writer_thread()`、`print_to_stream()`、`process_completed_batch()` 均无条件输出。
- **修复建议**: 在 `async_writer_thread()` 或 `print_to_stream()` 入口处增加 `if (cfg->mute) return;`。同时应抑制 `stderr` 上的 `[System]` 诊断信息（或至少抑制数据输出）。
- **涉及文件**: `src/async_worker.c:30`, `src/output.c:473`, `src/main_loop.c:124-170`

---

## 4. [中] `--follow-symlinks` 在 Worker 进程中未生效

- **优先级**: P2
- **影响模块**: `worker_proc.c`
- **复现命令**:
  ```bash
  mkdir -p /tmp/target_dir/subdir && echo hello > /tmp/target_dir/subdir/file.txt
  mkdir -p /tmp/link_test && ln -s /tmp/target_dir /tmp/link_test/link_to_dir
  ./bin/listfiles --path=/tmp/link_test --yes --follow-symlinks
  # 结果：只输出 link_to_dir 本身，未递归输出 subdir/file.txt
  ```
- **根因分析**: `worker_proc.c:164-233` `scan_and_send()` 始终使用 `lstat()`（line 204），**从未读取** `g_worker_cfg->follow_symlinks`。符号链接指向的目录被当作 `S_IFLNK` 处理，仅作为文件输出，不会入队为待扫描目录。
- **修复建议**: 在 `scan_and_send()` 中根据 `g_worker_cfg->follow_symlinks` 决定调用 `lstat()` 还是 `stat()`。同时需在 `try_blind_trust()` 中保证 `d_type` 与 `stat()` 后的文件类型一致。
- **涉及文件**: `src/worker_proc.c:198-206`

---

## 5. [中] `--clean` 模式清理不彻底且仍会生成中间进度文件

- **优先级**: P2
- **影响模块**: `main.c`, `progress.c`
- **复现命令**:
  ```bash
  ./bin/listfiles --path=/tmp/test_scan --yes --continue --progress-file=/tmp/clean_test
  ./bin/listfiles --path=/tmp/test_scan --yes --clean --progress-file=/tmp/clean_test
  ls -la /tmp/clean_test*   # .config, .idx, .pbin 等仍然存在
  ```
- **根因分析**:
  1. `main.c:179-183` 仅在启动时调用 `cleanup_progress(&temp)`，传入的 `temp` 是零初始化的 `RuntimeState`，`write_slice_index = 0`，导致 `cleanup_progress()` 中循环 `i <= 0 + 200` 最多清理 200 个历史分片。
  2. `record_path()` 不检查 `cfg->clean`，扫描过程中**仍会写入新的 `.pbin`**。
  3. `finalize_progress()` 的 `--clean` 分支只删除当前 `write_slice_file`，不删除之前轮转的切片、archive、idx、config 等。
- **修复建议**:
  - 在 `record_path()` 开头增加 `if (cfg->clean) return;`，避免生成任何进度文件。
  - 或在 `finalize_progress()` 的 `else`（clean）分支中统一清理所有 `progress_base` 相关文件。
- **涉及文件**: `src/main.c:179-183`, `src/progress.c:165-207`, `src/progress.c:1123-1153`

---

## 6. [中] 命令行参数字符串内存泄漏

- **优先级**: P2
- **影响模块**: `cmdline.c`, `main.c`
- **根因分析**: `parse_arguments()` 中对多个选项使用 `strdup()`：`target_path`, `progress_base`, `format`, `output_file`, `output_split_dir`, `resume_file`。程序结束前没有任何地方 `free` 这些内存。
- **修复建议**: 在 `app_context_destroy()` 或新增 `cleanup_config()` 中统一释放。
- **涉及文件**: `src/cmdline.c:123-172`, `src/main.c:282-283`

---

## 7. [中] `safe_malloc` 失败即退出，无错误恢复

- **优先级**: P2
- **影响模块**: `utils.c`（全局）
- **根因分析**: `safe_malloc()` 在 `malloc` 返回 `NULL` 时直接 `fprintf(stderr)` + `exit(EXIT_FAILURE)`。调用者（如 `read_next_pbin_record`、`send_batch`）没有机会释放已分配资源或返回优雅错误码。
- **修复建议**: 将 `safe_malloc` 改为返回 `NULL` 并让调用者处理；或在关键路径（如 `read_next_pbin_record`）中不再使用 `safe_malloc`，改用普通 `malloc` + 错误返回。
- **涉及文件**: `src/utils.c:31-38`

---

## 8. [低] 终止条件 `all_idle` 恒为 `true`

- **优先级**: P3
- **影响模块**: `main_loop.c`
- **根因分析**: `main_loop.c:339-351` 中，`all_idle` 初始为 `true`，遍历 Worker 时内部只有一条 `TODO` 注释 `/* TODO: check if worker is actually idle */`，没有任何状态检查逻辑。因此只要 `pending_tasks == 0`，就会立即判定为全部空闲并停止。
- **影响**: 在极端高并发场景下，可能尚有 batch 正在线程池处理但尚未回调，主循环就提前终止，导致部分目录未被扫描。
- **修复建议**: 增加 `ctx->thread_pool->active_batch_count` 或 `ctx->resume_active` 等标志位的检查，确保线程池无正在处理的任务后再终止。
- **涉及文件**: `src/main_loop.c:339-351`

---

## 9. [低] `precompile_format` 使用 `static` 局部缓冲区，非线程安全

- **优先级**: P3
- **影响模块**: `output.c`
- **根因分析**: `output.c:272` `static char default_fmt[256];` 是函数局部静态变量。虽然当前 `precompile_format()` 仅在主线程调用一次，但静态变量在多线程或未来重构中隐含风险。
- **修复建议**: 将 `default_fmt` 改为栈局部数组（非 `static`），或确保调用者提供缓冲区。
- **涉及文件**: `src/output.c:272`

---

## 10. [低] 未知选项误触发 help 而非错误提示

- **优先级**: P3
- **影响模块**: `cmdline.c`
- **根因分析**: `cmdline.c:204` `case 'h': default:` 把 `-h` 和 `default`（未知选项）合并处理，均调用 `show_help()` 后返回 `-1`。
- **修复建议**: 将 `default` 分支独立，输出类似 `错误: 未知选项 --xxx` 后再返回非 0。
- **涉及文件**: `src/cmdline.c:204`

---

## 11. [低] `send_batch` 内存分配失败静默丢弃 batch

- **优先级**: P3
- **影响模块**: `worker_proc.c`
- **根因分析**: `worker_proc.c:129-130` `buf = malloc(total); if (!buf) return;`。分配失败后直接返回，Worker 不会发送 batch，也不会发送错误消息，Master 的 `pending_tasks` 可能无法正确递减，导致扫描 hang 住。
- **修复建议**: 分配失败时调用 `send_batch(fd_out, NULL, NULL, 0)` 发送空 batch，确保 Master 能正确回收任务计数。
- **涉及文件**: `src/worker_proc.c:129-130`

---

## 测试汇总

| 测试项 | 结果 | 备注 |
|--------|------|------|
| 基本目录扫描 | ✅ 通过 | `/tmp/test_scan` 4 个文件正确输出 |
| CSV 输出 | ✅ 通过 | RFC 4180 双引号包裹正确 |
| 自定义 `--format` | ✅ 通过 | `%p\|%s\|%t` 正常 |
| 空目录 | ✅ 通过 | 无输出，正常退出 |
| 权限拒绝目录 | ✅ 通过 | 跳过不可访问子目录 |
| 单文件目标 | ✅ 通过 | 正确输出单文件元数据 |
| 长路径（~4031 字节） | ✅ 通过 | 未截断 |
| 大量文件（1000） | ✅ 通过 | 计数准确 |
| `/dev` 特殊文件 | ✅ 通过 | 设备文件正确输出 |
| `--output-split` | ✅ 通过 | 分片文件生成正常 |
| `--quote` | ✅ 通过 | 字段引号包裹正确 |
| `--max-slice` | ✅ 通过 | 小值切片正常 |
| `--follow-symlinks` | ❌ 失败 | 未递归跟踪符号链接指向的目录 |
| `--mute` | ❌ 失败 | 完全未实现 |
| `--help` 返回码 | ❌ 失败 | 返回 1 而非 0 |
| `--clean` 清理 | ❌ 失败 | 历史进度文件未被删除 |
| 断点续传（已完成→再恢复） | ❌ 崩溃 | `safe_malloc` 读到 Footer 导致 OOM 退出 |
| 中断后恢复（SIGTERM） | ✅ 通过 | 信号处理正常，恢复后成功完成 |
| `--runone` | ✅ 通过 | 忽略历史进度，输出完整 |

---

## 修复优先级建议

1. **立即修复（P0）**: 问题 1（恢复崩溃）—— 这是当前唯一会导致生产环境数据丢失/扫描中断的阻断性缺陷。
2. **本周修复（P1）**: 问题 2（`--help` 返回码）、问题 3（`--mute` 未实现）。
3. **下次迭代（P2）**: 问题 4（`--follow-symlinks`）、问题 5（`--clean`）、问题 6（内存泄漏）、问题 7（`safe_malloc` 错误处理）。
4. **后续优化（P3）**: 问题 8-11。
