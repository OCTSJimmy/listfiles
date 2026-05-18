# fixed v15.4.4 — IPC FSM BATCH Footer 读取协议修复

日期：2026-05-18

## 问题

v15.4.0 引入的 IPC FSM 续传机制中，`read_data_message()` 的 PAYLOAD/FOOTER 阶段边界与 Worker `send_batch()` 的协议格式不一致，导致 BATCH 消息永远卡在 FOOTER 阶段，无法转发给主线程。

**协议两端不一致：**

- **Worker 发送端**：`ipc_send(fd_data, IPC_MSG_BATCH, buf, total)` 中 `buf` = `IpcBatchHeader` + records + `Footer(8B)`，`payload_len` = `total`（**已包含 Footer**）。
- **IPC 接收端（原实现）**：
  - PAYLOAD 阶段：调用 `safe_ipc_recv_payload_fsm()` 读取 `payload_len` bytes（**把整个 Payload+Footer 全部读完了**）。
  - FOOTER 阶段：调用 `safe_ipc_recv_footer_fsm()` 试图**再读取 8 bytes**。此时管道已空，`poll(100ms)` 超时返回 `-2`。

**后果**：FOOTER 阶段永远等不到数据，FSM 卡在 `IPC_READ_FOOTER`；BATCH 被锁死在 IPC 线程；主线程只收到 `FINISH` 收不到 `BATCH`；`pending_tasks` 归零后程序 0 秒退出、0 输出。

## 修复

修改 `read_data_message()`：

1. **PAYLOAD 阶段**：只读取 `payload_len - sizeof(uint64_t)` bytes（payload body，不含 Footer）。
2. **FOOTER 阶段**：单独 `fsm_recv()` 读取 8 bytes Footer 到局部缓冲区，验证 `IPC_FOOTER_MAGIC` 后，将其复制到 `fsm->buf` 末尾，保持 Worker 侧内存格式一致。
3. 转发时 `net_payload_len = payload_len - sizeof(uint64_t)`，主线程解析不受 Footer 影响。

## 修改文件

- `src/ipc/ipc_message_handler.c` — `read_data_message()` PAYLOAD/FOOTER 阶段边界修正
- `include/core/config.h` — 版本号 15.4.4

## 编译

`make clean && make` 零错误零警告。
