# Fixed: v15.4.4 — 续传模式丢失深层子目录

## 日期
2026-05-18

## 问题描述

第一次执行扫描了约 150 万条记录后被 Ctrl+C 中断。续传（`--continue`）时程序 1 秒完成，标记 Success，但目标存储中仍有约 4 亿文件未扫描。

## 根因分析

### 1. `visited_set` 过早去重

`batch_dedup_worker()` 对 BATCH 结果中的**所有条目**（文件+目录）都执行 `fp_set_insert(ctx->visited_set, fp)`。这意味着：
- 父目录被扫描后，Worker 返回的 BATCH 中包含的子目录作为"已见条目"被插入 `visited_set`。
- 即使这些子目录的进一步扫描任务在第一次执行时因中断而丢失，它们本身已经在 `visited_set` 中了。

### 2. 续传 pumping 阶段跳过"已见"子目录

续传时 `restore_progress()` 从归档加载旧分片到 `visited_set`，然后 pumping 重新扫描当前分片中的父目录。Worker 返回的子目录若已在 `visited_set` 中，被标记 `result |= 1` (duplicate)。`process_completed_batch()` 中 `if (result & 1) continue;` 直接跳过。

因此，这些子目录永远不会被重新扫描，其深层子目录永远无法被发现。

### 3. `HIST_PUMP_OLD` 阶段不派发扫描任务

`process_completed_batch()` 中：
```c
if (ctx->hist_pump_state == HIST_PUMP_OLD) {
    fpbin_append(ctx, path, st);  // 只追加到 fpbin，不发送 CMD_SCAN
}
```

即使某些子目录不在 `visited_set` 中（首次发现），也仅被追加到 fpbin，等待 fpbin 转正后再扫描。但若大部分子目录已在 `visited_set` 中被跳过，fpbin 为空， pumping 立即结束。

## 修复内容

### `src/scan/batch_processor.c`

**1. `batch_dedup_worker()` — pumping 阶段对目录跳过 `visited_set` 去重**

```c
/* v15.4.4: In HIST_PUMP_OLD phase, skip visited_set dedup for directories
 * so that re-scanning can discover sub-directories that were lost during
 * the previous interrupted run. Files are still deduped to avoid duplicate
 * output entries. */
bool is_dir = S_ISDIR(st->st_mode);
if (!is_dir || ctx->hist_pump_state != HIST_PUMP_OLD) {
    if (fp_set_insert(ctx->visited_set, fp)) {
        result |= 1; /* duplicate */
    }
}
```

- 正常扫描时：文件和目录均去重（行为不变）。
- `HIST_PUMP_OLD` 阶段：文件去重（避免输出重复），目录不去重（确保丢失的目录能被重新发现）。

**2. `process_completed_batch()` — pumping 阶段对目录统一派发 CMD_SCAN**

```c
if (S_ISDIR(st->st_mode)) {
    if (ctx->hist_pump_state == HIST_PUMP_OLD) {
        fpbin_append(ctx, path, st);
    }
    /* Unified dispatch: send CMD_SCAN for all non-duplicate directories */
    atomic_fetch_add(&ctx->pending_tasks, 1);
    // ... send_scan_to_ipc ...
}
```

- `HIST_PUMP_OLD` 阶段：目录既 `fpbin_append`（保证进度持久化），又立即发送 `CMD_SCAN`（恢复扫描）。
- 正常扫描阶段：行为不变（仅发送 `CMD_SCAN`）。

**3. 日志版本戳统一更新**

`batch_processor.c` 中 6 处 `log_debug_v(202605150000, ...)` 更新为 `log_debug_v(202605181600UL, ...)`。

## 验证方法

1. 重新编译：`make clean && make`
2. 在已中断的进度目录上执行 `--continue` 续传
3. 观察程序是否重新开始扫描（`pending_tasks` 增加，Worker 持续返回 BATCH）
4. 输出文件应有新增记录，而非 1 秒立即 Success 退出

## 相关文件

- `src/scan/batch_processor.c`
- `CHANGELOG.md`
