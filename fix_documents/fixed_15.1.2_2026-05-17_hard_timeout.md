# v15.1.2 修复说明

## 发布时间
2026-05-17

## 修复内容

### 1. batch_dedup_worker 硬超时检测

**问题**：v15.1.0 测试出现 Master 线程 99% CPU 满负载，但 stack/syscall 显示为纯用户态空跑，所有 Worker IDLE。

**推断**：`batch_dedup_worker`（thread_pool 工作线程）内部死循环，导致 `pending_batches` 卡死，程序无法终止。

**修复**：
- `batch_dedup_worker` 外层 for 循环增加硬上限：`i >= 100000` 时强制中断，剩余条目标记为 duplicate 并返回
- `fp_shard_insert_internal` 内层开放寻址探测循环结束后，若未找到空槽/匹配项，输出 `log_fatal` 并返回 false（不应到达）

### 2. 日志路径脱敏

**需求**：调试日志中所有 `path=` 输出需脱敏，每级目录保留最后一个字符，其余用 `***` 替代。

**实现**：
- `utils.c` 新增 `path_log_mask(const char *path)` 函数，使用 `static __thread` 线程局部缓冲区
- 示例：`/public2/hw/a/b/c` → `/***2/***w/***a/***b/***c`
- 已应用到核心高频日志点：
  - `ipc_thread.c`: `CMD_SCAN sent to worker (path=...)`
  - `main_loop.c`: `cmd_queue full dropping`, `no IDLE worker`, `LostTasks dispatched`, `MSG_DROP requeue failed`
  - `monitor.c`: `path=` 显示（截断前先做脱敏）

## 待验证

硬超时检测是否能暴露或解决 v15.1.0 的 Master CPU 100% 问题，需要下次 `/public2` 测试确认。

## 版本

- 版本号：`15.1.2`
