# Fixed: v12.2.15 — cleanup_done 未重置 + 日志系统 + dispatch 轮询缺陷

**状态**: 已修复 ✅  
**修复日期**: 2026-05-15  
**版本**: 12.2.15  
**严重程度**: P0 阻断性

---

## 问题描述

v12.2.14 修复了数据竞争，但运行在生产环境（/public2，300万+文件）时仍然出现 Monitor 秒表冻结、所有 Worker 阻塞在 `pipe_wait`、主进程 throughput 归零的现象。`errlogs.err` 中 `[Epoll] Worker 1 fd error/hup` 重复 54万+次，`active=8->7` 始终不变。

---

## 根因分析

### 根因 A：cleanup_done 未重置（核心）

当 Worker 超时死亡后，`cleanup_dead_worker_slot()` 通过 `atomic_flag_test_and_set()` 设置 `cleanup_done = true`。

随后 `worker_pool_replace()` → `worker_pool_spawn()` 创建新 Worker，但**未重置 `cleanup_done`**。

新 Worker 若再次死亡（或 epoll 报告残留事件），cleanup 因 `cleanup_done = true` 直接 return，不关闭 fd、不从 epoll 移除。

fd 持续打开 → epoll 每次 `epoll_wait` 都返回 `EPOLLERR|EPOLLHUP` → 54万+次重复日志，CPU 空转。

### 根因 B：dispatch_lost_tasks 轮询缺陷

`dispatch_lost_tasks()` 中 `ipc_send()` 返回 `EAGAIN(-2)` 时直接 `break`，仅尝试了一个 Worker 就放弃。

若轮询恰好选到 fd_in 满的 Worker，所有 `lost_tasks` 都发不出去，其他空闲 Worker 永远收不到任务。

### 根因 C：日志无时间戳，无法定位故障时刻

大量 `fprintf(stderr, ...)` 分散在各文件中，格式不统一，无时间戳，无法从日志推断卡死发生时刻。调试日志与错误日志混为一谈，verbose 开关无法精细控制。

---

## 修复内容

### 1. worker_pool_spawn 重置 cleanup_done

```c
slot->backlog_paths = NULL;
slot->backlog_count = 0;
slot->backlog_capacity = 0;
atomic_flag_clear(&slot->cleanup_done);  /* [FIX] 重置 cleanup_done */
atomic_fetch_add(&pool->active_count, 1);
```

### 2. dispatch_lost_tasks 不 break，继续轮询

```c
int rc = ipc_send(slot->fd_in, IPC_MSG_SCAN, path, plen);
if (rc == -2) {
    lost_tasks_push(&ctx->lost_tasks, path);
    continue;  /* [FIX] 尝试下一个 Worker */
}
```

### 3. 统一日志模块 log.c / log.h

- 所有 `fprintf(stderr, ...)` 替换为 `log_fatal / log_error / log_warn / log_info / log_debug / log_trace`。
- 自动添加 `[YYYY-MM-DD HH:MM:SS] [LEVEL]` 前缀。
- 日志固定输出到 stderr，不污染 stdout。
- `verbose` 和 `verbose_level` 控制 DEBUG/TRACE 级别日志。
- FATAL/ERROR/WARN 始终输出，不受 verbose 开关影响。

---

## 新增文件

- `src/log.c`
- `include/log.h`

## 修改的文件

- `src/worker_proc.c`
- `src/main_loop.c`
- `src/monitor.c`
- `src/main.c`
- `src/cmdline.c`
- `src/output.c`
- `src/progress.c`
- `src/device_manager.c`
- `src/thread_pool.c`
- `src/lost_tasks.c`
- `src/utils.c`
- `include/config.h`（版本号 → 12.2.15）

---

## 验证

- [x] 编译通过，无警告
- [x] 日志输出带时间戳和级别前缀
- [x] cleanup_done 在 spawn 时被重置
- [x] dispatch_lost_tasks 遇到 EAGAIN 时 continue 而非 break
- [x] stderr 输出不受 stdout 污染

---

## 剩余问题

- `worker_pool_stop_all` 读取 `is_alive` 未用 `atomic_load` — 仅 shutdown 调用，风险极低。
- `slot->current_path` 和 `slot->current_dev` 仍为 plain 类型 — 仅影响日志输出，不影响扫描正确性。
