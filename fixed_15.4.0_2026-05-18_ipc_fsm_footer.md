# fixed v15.4.0 — IPC FSM 续传读取 + BATCH Footer 魔数

日期：2026-05-18

## 问题

IPC 线程使用一次性 `read()` 读取 Header/Payload，`EAGAIN` 时丢弃部分数据，下次 `epoll_wait` 后从断点继续但调用方已丢失进度，导致协议解析错乱。BATCH 大数据反复丢弃 → 无限失步 → payload timeout。

## 修复

1. `IpcReadFsm` 状态机：为每个 fd 维护独立的 `state`、`nread`、`hdr`、`buf`，支持跨多次 `epoll_wait` 调用的可恢复续传读取。
2. `fd_ctrl`：两阶段 FSM（`HDR → PAYLOAD`）。
3. `fd_data`：三阶段 FSM（`HDR → PAYLOAD → FOOTER`），Footer 魔数 `0xDEADBEEF66AAC0FF` 校验数据完整性。
4. `fsm_recv()` 通用续传原语：`poll(100ms)` + `read`，`EAGAIN` 返回 `-2` 但保留 `buf` 和 `nread`。
5. `CMD_REPLACE` 时彻底重置两个 FSM 并释放 `buf`。
6. `ipc_msg_type_valid()` 白名单 + `hdr.payload_len > 100MB` 上限。

## 修改文件

- `include/ipc/ipc_protocol.h`
- `include/ipc/ipc_thread.h`
- `src/ipc/ipc_protocol.c`
- `src/ipc/ipc_thread.c`
- `src/ipc/ipc_message_handler.c`
- `src/scan/worker_scanner.c`
- `include/core/config.h`

## 编译

`make clean && make` 零错误零警告。
