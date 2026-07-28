# Fixed Document: v15.5.7 — 2026-07-28

## 问题概述

对 v15.5.6 代码与《listfiles 扫描完整性故障总结》逐条对账后，发现熔断清单 + 非 0 退出码机制（v15.5.4 引入）仍存在四个"静默通道"，可绕开全部可见性机制，使扫描严重不完整但仍以退出码 0 "成功"：

1. **错误上报发错通道（P0）**：`send_error_and_empty_batch()` 把 `IPC_MSG_ERROR` 写到 `fd_data`；
2. **目录级 errno 白名单过窄（P0）**：仅 `ETIMEDOUT/EIO` 上报，`EACCES` 等整棵子树静默缺失；
3. **条目级 `lstat` 失败静默（P0，故障报告 §2.3.5 遗留）**；
4. **`readdir` 中途失败静默（P0）**：超大目录部分条目静默丢失。

---

## 根因分析

### Root Cause 1: `IPC_MSG_ERROR` 误发 `fd_data`

`src/scan/worker_scanner.c` 的 `send_error_and_empty_batch(fd_out, ...)` 中 `fd_out` 即 `ctx->fd_data`（数据通道）。而 Master 侧 `src/ipc/ipc_message_handler.c` 的 `read_data_message()` 只接受 `IPC_MSG_BATCH` 帧，收到其他类型直接 `log_error + drain_fd()`：

```c
if (fsm->hdr.msg_type != IPC_MSG_BATCH) {
    log_error("[IPC-%d] data unexpected type=%u, draining fd", ...);
    drain_fd(ctx->fd_data);
}
```

后果：scanner 自检到的目录级 `ETIMEDOUT/EIO` 错误**永远到不了** `circuit_breaker_record()`；且 `drain_fd()` 会把紧随其后的空批次一并吞掉，可能引发 `pending_tasks` 计数失衡。正确的控制通道是 `fd_ctrl`（`read_ctrl_message()` 路由）。

### Root Cause 2: 目录级 errno 白名单仅 `ETIMEDOUT/EIO`

修复前 `send_error_and_empty_batch()` 仅在这两个 errno 下发送 `IPC_MSG_ERROR`，其余（`EACCES` 权限拒绝、`ESTALE` 等）只发空批次；Master 侧 `main_loop_handle_error()` 同样只处理这两个 errno。任何其他原因的目录级失败都完全无痕——子树缺失、无清单记录、退出码 0。

### Root Cause 3: 条目级 `lstat` 失败直接 `continue`

`scan_and_send()` 的 `readdir` 循环中，条目 `lstat/stat` 失败直接 `continue`，无任何记录。高负载 NFS 上单条目 `EIO/ETIMEDOUT/ESTALE`、或权限问题 `EACCES` 都会静默丢弃真实存在的条目。

### Root Cause 4: `readdir` 返回 NULL 不区分 EOF 与错误

修复前 `while ((entry = readdir(dir)) != NULL)` 循环结束后不检查 `errno`。NFS readdir cookie 失效等中途失败表现为"正常结束"，超大目录（数万~十万级条目）只扫描了一部分却按完整目录处理。这与生产观测到的覆盖率剧烈波动、0.75% 目录条目数不一致高度吻合。

---

## 修复方案

### Fix 1: 错误上报双通道拆分

`send_error_and_empty_batch(int fd_data, int fd_ctrl, ...)`：错误帧改走 `fd_ctrl`，空批次仍走 `fd_data`（保证 `pending_tasks` 计数平衡）。`ipc_send` 失败时 `log_error`（非版本化）。

### Fix 2: 目录级错误全量上报（竞态除外）

Worker 侧上报范围扩展为除 `ENOENT/ENOTDIR` 外的全部 errno（后两者为扫描期间目录被并发删除/替换的正常竞态）。Master 侧 `main_loop_handle_error()` 新增 else 分支：非超时/IO 错误记录 `DIR_ERROR(errno=N)` 到熔断清单，**不触发** `dev_mgr_mark_dead()` / spbin / 探测。

### Fix 3: 条目级错误上报 `IPC_MSG_ENTRY_ERROR`

- 线协议新增 `IPC_MSG_ENTRY_ERROR(10)`（`include/ipc/ipc_protocol.h`），payload 复用 `IpcErrorHeader + uint32 plen + path`；
- 返回类型新增 `RET_ENTRY_ERROR(19)`（`include/ipc/msg_format.h`）；
- `read_ctrl_message()` 合并 case 解析转发；
- Worker 新增 `send_entry_error()`：条目 `lstat/stat` 失败且 errno 非 `ENOENT/ENOTDIR` 时经 `fd_ctrl` 上报；路径截断（`snprintf ≥ 4096`）以 `ENAMETOOLONG` 上报（记录父目录路径）；
- Master `handle_return_message()` 新增 `RET_ENTRY_ERROR` case：记录 `ENTRY_ERROR(errno=N)`，**不触发**设备惩罚/探测/Worker 状态变更（Worker 仍在正常扫描）；
- 新增 `entry_stat()`：条目 stat 带 `EINTR` 重试（≤3 次），避免信号中断慢速 NFS stat 被误判为条目失败。

### Fix 4: `readdir` errno 检查

循环改为每次调用前 `errno = 0`，返回 NULL 时保存 `errno`；循环结束后非 0 则：先 flush 已收集的有效条目 → `closedir` → 按目录级错误 `send_error_and_empty_batch()` 上报。

### 与既有机制的关系

所有记录经 `circuit_breaker_record()` → `skipped_count > 0` → `stderr` 输出 `[CRITICAL] 扫描不完整...` + `.config` 写 `Incomplete` + **退出码 1**。本版本未新增任何版本化日志——按设计要求，审计通道为独立熔断清单而非日志（避免日志污染与被刷掉）。

---

## 行为变更

| 场景 | 修复前 | 修复后 |
|------|--------|--------|
| scanner 自检目录 ETIMEDOUT/EIO | 错误帧被 drain，熔断清单无记录 | 经 `fd_ctrl` 正常路由，记录 `DEV_TIMEOUT`/`EIO` |
| 目录 opendir/lstat 失败 EACCES | 静默，退出码 0 | 记录 `DIR_ERROR(errno=13)`，退出码 1 |
| 条目 lstat 失败 EACCES/ESTALE/EIO | 静默，退出码 0 | 记录 `ENTRY_ERROR(errno=N)`，退出码 1 |
| 条目在 readdir 后被并发删除（ENOENT/ENOTDIR） | 静默 | 静默（正常竞态，不上报） |
| readdir 中途失败（超大目录） | 部分条目静默丢失，退出码 0 | 已收集条目保留 + 目录级错误上报，退出码 1 |
| 路径长度 ≥ 4096 截断 | 静默跳过 | 记录 `ENTRY_ERROR(errno=36)`，退出码 1 |

---

## 验证方法

### 编译验证

```bash
make clean && make
./listfiles --version   # 应显示 15.5.7
```

### 行为验证（已通过）

```bash
# 故障注入：secret 目录 r--（readdir 可、条目 lstat EACCES）；noaccess 目录 ---（opendir EACCES）
chmod 0444 /tmp/lf_test/secret
chmod 0000 /tmp/lf_test/noaccess
./listfiles -p /tmp/lf_test -O /tmp/lf_o1 --yes --worker-count 2 -f /tmp/lf_p1 --mute
# 预期：退出码 1；/tmp/lf_p1.circuit_breaker 记录：
#   ENTRY_ERROR(errno=13)  /tmp/lf_test/secret/x.txt
#   ENTRY_ERROR(errno=13)  /tmp/lf_test/secret/y.txt
#   DIR_ERROR(errno=13)    /tmp/lf_test/noaccess
# 且正常目录条目完整输出。

# 对照组：恢复权限后全新前缀重扫
./listfiles -p /tmp/lf_test -O /tmp/lf_o2 --yes --worker-count 2 -f /tmp/lf_p2 --mute
# 预期：退出码 0；.circuit_breaker 仅表头；6 个文件全部列出。
```

实际验证结果与预期一致。

---

## 文件变更

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `include/core/config.h` | 修改 | VERSION → 15.5.7，VERSION_CODE → 202607281100UL |
| `include/ipc/ipc_protocol.h` | 修改 | 新增 `IPC_MSG_ENTRY_ERROR(10)` |
| `src/ipc/ipc_protocol.c` | 修改 | `ipc_msg_type_valid()` 白名单加入新类型 |
| `include/ipc/msg_format.h` | 修改 | 新增 `RET_ENTRY_ERROR(19)` |
| `src/ipc/ipc_message_handler.c` | 修改 | `IPC_MSG_ENTRY_ERROR` 解析转发 `RET_ENTRY_ERROR` |
| `src/scan/worker_scanner.c` | 修改 | 双通道错误上报；`send_entry_error()`；`entry_stat()` EINTR 重试；`readdir` errno 检查；截断上报 |
| `src/scan/main_loop.c` | 修改 | `RET_ENTRY_ERROR` 路由记录 `ENTRY_ERROR`；非超时目录错误记录 `DIR_ERROR` |

---

## 已知遗留问题

- **熔断清单入仓**：`.circuit_breaker` 尚未结构化导入 CK 元表（路线 C3）。
- **路径级熔断阈值固定为 3**：`CIRCUIT_BREAKER_THRESHOLD` 未自适应。
- **条目级错误风暴**：若整个超大目录全部条目失败（灾难场景），将产生等量 IPC 消息与清单行。此时目录级/设备级机制通常已先行触发，故未做限流。

---

## 相关环境

- **NFS 挂载选项**：`hard,nolock,proto=tcp,timeo=600,retrans=2`
- **存储规模**：约 1.75 亿文件 / 670TB
- **触发条件**：存储高元数据负载（如大规模删除后的后台 GC）
