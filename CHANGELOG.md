# Changelog

所有显著变更均记录于此文件，格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

---

## [15.5.3] - 2026-07-06

### Fixed：大目录 readdir 超时误判（DEV_TIMEOUT false-positive）+ 目录级熔断 + 日志版本化

**P0 — Critical：**
- **大目录 readdir 超时误判**：Worker Scanner 遍历大目录（如 72,275 个文件的 `gvcf_stats`）时，`readdir()` 循环可能持续数分钟，但 `last_progress` 只在扫描开始/结束时更新。IPC 线程 30 秒无进度信号即上报 `DEV_TIMEOUT`，Master 替换 Worker 后再次派发同一目录，形成无限循环，最终耗尽 dispatch_queue。
  - 修复：`scan_and_send()` 的 `readdir()` 循环中每处理 1000 个条目调用 `scanner_progress_tick()` 更新 `last_progress`，让 IPC 线程知道 Scanner 仍在正常工作。

- **目录级熔断（Circuit Breaker）**：同一死亡目录被反复 redispatch，所有 Worker 逐个卡死，dispatch_queue 从数万降至 0，扫描完全停滞。
  - 修复：新增 `circuit_breaker_check()`，每个 Worker slot 独立追踪最近 timeout 的路径和连续次数。达到 `CIRCUIT_BREAKER_THRESHOLD(3)` 后跳过该路径，不再重试。不同路径会重置计数器。

**P1 — High：**
- **DEV_TIMEOUT 相关日志在 --mute 下仍刷屏**：`[Worker-N] Scanner stuck`、`[Bus] Worker N DEV_TIMEOUT (scanner stuck), replacing`、`[DispatchQueue] no IDLE worker available, requeue` 等日志为 ERROR/WARN 级别，不受 `--mute` 影响，在 NFS 大目录场景下每秒产生数千行。
  - 修复：所有 DEV_TIMEOUT 相关日志（Worker 侧 `Scanner active` / `Scanner stuck`、Bus 侧 `DEV_TIMEOUT`、dispatch 侧 `no IDLE worker` / `cmd_queue full`）统一应用版本号 `202607030000UL`。当前 `VERSION_CODE=202607061400 > 202607030000`，默认静默；需要排查时通过 `--verbose-version=0` 或 `--verbose-version=202607030000` 重新打开。

**修改的文件**：
- `include/core/config.h` — VERSION "15.5.3"，VERSION_CODE 202607061400UL，新增 CIRCUIT_BREAKER_THRESHOLD
- `include/core/app_context.h` — 新增 `timeout_paths[8][4096]` / `timeout_counts[8]` 熔断状态字段
- `src/scan/worker_scanner.c` — `scanner_progress_tick()` 心跳函数 + readdir 循环中每 1000 条目调用
- `src/scan/dispatch.c` — `circuit_breaker_check()` + `cleanup_dead_worker_slot()` 集成熔断逻辑
- `src/ipc/worker_proc.c` — `Scanner active` / `Scanner stuck` → `log_info_v` / `log_error_v(202607030000UL)`
- `src/scan/main_loop.c` — `[Bus] DEV_TIMEOUT` → `log_error_v(202607030000UL)`
- `src/scan/dispatch.c` — `cmd_queue full` / `no IDLE worker` → `log_warn_v(202607030000UL)`

---

## [15.5.2] - 2026-05-20

### Architecture: pbin sliding window backpressure + dispatch_queue leak fixes