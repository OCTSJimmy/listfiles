# Fixed Document: v15.5.3 — 2026-07-06

## 问题概述

生产环境扫描 NFS 挂载的大存储（`/temp-sg800t-read`，数亿文件级）时，Worker 进程在遍历大目录（72,275+ 文件）时持续触发 `DEV_TIMEOUT`，导致同一目录被无限重试，最终耗尽 dispatch_queue，扫描完全停滞。

---

## 现象

### 监控面板

```
===== listfiles v15.5.2 =====
Elapsed: 2d 05:53:04

[Workers]
  Active: 8 / 8
  Pending tasks: 1
  Pending batches: 0
  Dispatch queue: 0

[Worker States]
  W4: BUSY pid=212058 path=.../gvcf_stats  ← 持续 BUSY 2天+
  W0,W1,W2,W3,W5,W6,W7: IDLE              ← 有活干但没任务

[Throughput]
  Dir rate:      0.00/s (max: 2148.34)
  File rate:     0.00/s (max: 11781.39)
```

### 日志模式

```
[Worker-2] Scanner active (heartbeat 9)
[Worker-2] Scanner stuck for 30s on /temp-sg800t-read/.../gvcf_stats, reporting to master
[Bus] Worker 2 DEV_TIMEOUT (scanner stuck), replacing
[Replace] Replacing dead worker 2
[Worker-2] Started, cfg=0x... , hb_timeout=30
... 循环重复 ...
```

触发 `DEV_TIMEOUT` 的目录：
- `/temp-sg800t-read/omics/Kailuan_APAC/WGS/vcf_stats/gvcf_stats` — **72,275 个文件**
- `/temp-sg800t-read/backup/.../prd_results(mkt)/prd_niis(mkt)` — **12,598 个文件**
- `/temp-sg800t-read/backup/.../prd_results_c3/c3` — **12,598 个文件**

---

## 根因分析

### Root Cause 1: readdir 大目录超时误判（P0）

`scan_and_send()` 中的 `readdir()` 循环遍历 7万+ 条目时，在 NFS 上可能需要数分钟。但 `last_progress` 只在扫描**开始**和**结束**时更新：

```c
// worker_scanner.c (v15.5.2)
while ((entry = readdir(dir)) != NULL) {
    // ... 处理条目 ...
    // 30+ 秒内没有任何进度更新！
}
```

IPC 线程的 stuck 检测器（30 秒阈值）看到 `last_progress` 没有变化，误判 Scanner 卡死，上报 `DEV_TIMEOUT`。

**实际上 Scanner 在正常工作，只是目录太大遍历太慢。**

### Root Cause 2: 无熔断机制（P0）

`cleanup_dead_worker_slot()` 在 Worker 死亡后总是将 `current_path` 重新入队：

```c
if (redispatch_current && slot->current_path[0] != '\0') {
    char *dup = strdup(slot->current_path);
    dispatch_queue_push(&ctx->dispatch_queue, dup, NULL);
}
```

新 Worker 又被派去同一个大目录 → 再次超时 → 再次替换 → **无限循环**。经过 2 天+ 的循环，dispatch_queue 从 96,697 降至 0，所有正常目录都扫描完了，只剩最后一个死亡目录在无限重试。

### Root Cause 3: DEV_TIMEOUT 日志刷屏（P1）

`[Worker-N] Scanner stuck`、`[Bus] DEV_TIMEOUT`、`[DispatchQueue] no IDLE worker` 等日志为 ERROR/WARN 级别，不受 `--mute` 影响。在大目录场景下每秒产生数千行，即使加了 `--mute` 参数仍然刷屏。

---

## 修复方案

### Fix 1: readdir 循环进度心跳

在 `readdir()` 循环中每 1000 个条目更新一次 `last_progress`：

```c
// worker_scanner.c (v15.5.3)
static void scanner_progress_tick(int *entry_count, int interval,
                                  pthread_mutex_t *mutex, time_t *last_progress,
                                  int worker_id) {
    (*entry_count)++;
    if (*entry_count % interval == 0) {
        pthread_mutex_lock(mutex);
        *last_progress = time(NULL);
        pthread_mutex_unlock(mutex);
    }
}

while ((entry = readdir(dir)) != NULL) {
    scanner_progress_tick(&entry_count, 1000,
                          &task->progress_mutex, &task->last_progress,
                          worker_id);
    // ... 处理条目 ...
}
```

**效果**：IPC 线程每 ~1000 个条目都会看到进度更新，不会再对大目录误判为 `DEV_TIMEOUT`。

### Fix 2: 目录级熔断（Circuit Breaker）

新增 `circuit_breaker_check()`，每个 Worker slot 独立追踪：

```c
// app_context.h
#define CIRCUIT_BREAKER_THRESHOLD 3

typedef struct AppContext {
    // ...
    char timeout_paths[8][4096];   // 每个 Worker slot 最近 timeout 的路径
    int  timeout_counts[8];        // 该路径连续 timeout 次数
} AppContext;
```

```c
// dispatch.c
static bool circuit_breaker_check(AppContext *ctx, int wid, const char *path) {
    if (strcmp(ctx->timeout_paths[wid], path) == 0) {
        ctx->timeout_counts[wid]++;
    } else {
        safe_strcpy(ctx->timeout_paths[wid], path, sizeof(ctx->timeout_paths[wid]));
        ctx->timeout_counts[wid] = 1;
    }

    if (ctx->timeout_counts[wid] >= CIRCUIT_BREAKER_THRESHOLD) {
        log_warn_v(202607030000UL, "[CircuitBreaker] Path timed out %d times, skipping: %s",
                   ctx->timeout_counts[wid], path_log_mask(path));
        return true;  // 熔断：不再重试
    }
    return false;  // 未熔断：允许重试
}
```

**效果**：同一路径最多重试 3 次，之后被跳过，不再陷入无限循环。

### Fix 3: 日志版本化

所有 DEV_TIMEOUT 相关日志统一应用版本号 `202607030000UL`：

| 日志位置 | 原日志级别 | 修复后 |
|---------|-----------|--------|
| `worker_proc.c` | `[Worker-N] Scanner active (heartbeat)` | `log_info_v(202607030000UL, ...)` |
| `worker_proc.c` | `[Worker-N] Scanner stuck for ...` | `log_error_v(202607030000UL, ...)` |
| `main_loop.c` | `[Bus] Worker N DEV_TIMEOUT ...` | `log_error_v(202607030000UL, ...)` |
| `dispatch.c` | `[DispatchQueue] no IDLE worker...` | `log_warn_v(202607030000UL, ...)` |
| `dispatch.c` | `[Dispatch] cmd_queue full...` | `log_warn_v(202607030000UL, ...)` |

当前 `VERSION_CODE = 202607061400 > 202607030000`，所以默认静默。需要排查时：

```bash
./listfiles ... --verbose-version=202607030000
```

---

## 验证方法

### 编译验证

```bash
cd /path/to/listfiles
git pull origin dev
make clean && make
./listfiles --version
# 应显示: listfiles v15.5.3 (VERSION_CODE: 202607061400)
```

### 行为验证

1. **大目录不再误判超时**：扫描包含 7万+ 文件的目录时，Worker 不再触发 `DEV_TIMEOUT`。
2. **日志静默**：不加 `--verbose-version` 时，不应看到 `Scanner stuck`、`DEV_TIMEOUT`、`no IDLE worker` 等日志。
3. **熔断生效**：如果某个目录真的有问题（如 NFS 服务端挂死），连续 3 次超时后会被跳过，日志显示 `[CircuitBreaker] Path timed out 3 times, skipping`。

---

## 文件变更

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `include/core/config.h` | 修改 | VERSION → "15.5.3", VERSION_CODE → 202607061400UL, 新增 CIRCUIT_BREAKER_THRESHOLD |
| `include/core/app_context.h` | 修改 | 新增 timeout_paths[8][4096] / timeout_counts[8] |
| `src/scan/worker_scanner.c` | 修改 | 新增 scanner_progress_tick(), readdir 循环中每 1000 条目调用 |
| `src/scan/dispatch.c` | 修改 | 新增 circuit_breaker_check(), 集成到 cleanup_dead_worker_slot() |
| `src/ipc/worker_proc.c` | 修改 | Scanner active/stuck 日志版本化 |
| `src/scan/main_loop.c` | 修改 | [Bus] DEV_TIMEOUT 日志版本化 |
| `src/scan/dispatch.c` | 修改 | dispatch 相关 WARN 日志版本化 |

---

## 相关环境

- **NFS 挂载选项**：`hard,nolock,proto=tcp,timeo=600,retrans=2`
- **存储规模**：预估 2 亿文件
- **Worker 数**：8
- **触发目录特征**：深度嵌套路径 + 大量小文件（NFS 目录项缓存压力大）
