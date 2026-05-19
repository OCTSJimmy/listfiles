# fixed v15.4.1 — ipc_send 部分写入防御

日期：2026-05-18

## 问题

`MAX_PATH_LENGTH = 4096` 与 `PIPE_BUF = 4096` 相同，当 payload 含 4096 字节路径时 `total_len = 4104 > PIPE_BUF`，非阻塞 pipe 可能部分写入导致协议失步。`write()` 返回 `n == 0` 无处理，部分写入后 `EAGAIN` 无限重试。

## 修复

1. `MAX_PATH_LENGTH` 4096 → 4088（`PIPE_BUF - sizeof(IpcMessageHeader)`）。
2. `ipc_send()` 运行时 `total_len > PIPE_BUF` fatal guard。
3. `write()` 返回 `n == 0` 时防御性返回 `-1`。
4. 部分写入后 `EAGAIN` 增加 1000 次重试上限（每次 1ms `usleep`）。

## 修改文件

- `include/core/config.h`
- `src/ipc/ipc_protocol.c`

## 编译

`make clean && make` 零错误零警告。
