# Fixed: v15.4.4 — IPC BATCH 消息 PIPE_BUF 大小限制

## 日期
2026-05-18

## 问题描述

运行 v15.4.4（含 Footer read fix）后，程序不再 0 秒退出，但仍工作不正常：
- `errlogs.err` 中出现大量 `FATAL [ipc_send] total_len=... exceeds PIPE_BUF(4096)`
- `ERROR [Worker] send_batch FAILED`
- 运行 13 秒后标记 Success，但输出数据明显不完整（大量 batch 发送失败导致数据丢失）

## 根因分析

`src/ipc/ipc_protocol.c` 中的 `ipc_send()` 对所有消息类型统一执行：
```c
if (total_len > 4096) { log_fatal(...); return -1; }
```

Worker 侧 `scan_and_send()` 按固定 `batch_size = 1024` 条记录攒批。当目录下文件较多时，单条 batch 的 payload 可达数百 KB（如 `total_len=200846`），远超 PIPE_BUF(4096)。`ipc_send()` 直接拒绝发送，Worker 打印 ERROR 后放弃该 batch 数据，继续扫描并发送 FINISH，导致 Master 侧数据严重缺失。

## 为什么可以放宽限制

- `fd_data` / `fd_ctrl` 为**单 Writer（Worker Scanner 线程）单 Reader（Master IPC 线程）**的独占 pipe。
- Linux pipe 的 `PIPE_BUF(4096)` 原子写限制仅针对**多写者并发**时的交错风险；单写者场景下，超过 4096 的写不会与其他写交错，只是可能分多次 `read` 完成。
- `ipc_send()` 内部已实现 `while(written < total_len)` 循环写 + `partial_retry` 重试机制，可安全处理非阻塞 pipe 上的大消息写入。
- Master 侧 `read_data_message()` 已配置 `payload_len` 上限 16MB 的 sanity check，可防御异常大消息。

## 修复内容

### `src/ipc/ipc_protocol.c`

```c
// 修改前
if (total_len > 4096) {
    log_fatal("[ipc_send] total_len=%zu exceeds PIPE_BUF(4096)...");
    return -1;
}

// 修改后
if (total_len > 4096 && msg_type != IPC_MSG_BATCH) {
    log_fatal("[ipc_send] total_len=%zu exceeds PIPE_BUF(4096)...");
    return -1;
}
```

同时对 `ipc_send` 内部 4 处 `log_debug_v(202605150000, ...)` 更新为 `log_debug_v(202605181600UL, ...)`，与当前调试日志版本戳保持一致。

## 验证方法

1. 重新编译：`make clean && make`
2. 再次运行扫描大目录场景
3. 观察 `errlogs.err` 中不再出现 `exceeds PIPE_BUF(4096)` 的 FATAL 日志
4. 输出文件行数应与扫描的文件+目录总数匹配

## 相关文件

- `src/ipc/ipc_protocol.c`
- `CHANGELOG.md`
