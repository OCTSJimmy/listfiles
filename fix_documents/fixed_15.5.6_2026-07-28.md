# Fixed Document: v15.5.6 — 2026-07-28

## 问题概述

v15.5.4 已引入熔断清单与非 0 退出码，但 Worker 上报 `ETIMEDOUT/EIO` 时 `IpcErrorHeader.dev` 仍被硬编码为 0，导致：

- 熔断清单中的 `dev` 列无审计价值；
- 设备级熔断逻辑名存实亡（标记的是 dev=0，而非真实 `st_dev`）。

若直接修复 dev=0，NFS 单挂载点下一旦有一个目录超时，整个挂载点会被 `dev_mgr_is_blacklisted()` 跳过，覆盖率可能直接归零。因此必须同时做单挂载保护。

---

## 根因分析

### Root Cause 1: Worker 侧硬编码 dev=0

- `src/scan/worker_scanner.c:235` — `IpcErrorHeader eh = { (uint32_t)err_code, 0 };`
- `src/ipc/worker_proc.c:175` — `IpcErrorHeader eh = { ETIMEDOUT, 0 };`

Master 侧 `main_loop_handle_error()` 使用 `err->dev` 调用 `dev_mgr_mark_dead()`，实际标记的是 dev=0。

### Root Cause 2: 单挂载 NFS 场景下设备级熔断不可直接用

NFS 单挂载点共享同一个真实 `st_dev`。修复 dev=0 后，任一目录超时都会把该设备标记为 `DEAD`，导致 `dev_mgr_is_blacklisted()` 对整个挂载点生效。

---

## 修复方案

### Fix 1: 正确上报真实 `st_dev`

1. `include/scan/worker_scanner.h`：`WorkerThreadCtx` 增加 `current_dev` 字段。
2. `src/ipc/worker_proc.c`：
   - 初始化 `current_dev = 0`
   - DEV_TIMEOUT 上报时使用 `ctx.current_dev` 而非 0
3. `src/scan/worker_scanner.c`：
   - `send_error_and_empty_batch()` 增加 `dev_t dev` 参数，填充 `IpcErrorHeader.dev`
   - `scan_and_send()` 中 `lstat` 成功后设置 `task->current_dev = dir_st.st_dev`
   - `lstat` 失败时仍使用 `task->current_dev`（若为 0 表示尚未扫描）

### Fix 2: 单挂载保护

1. `include/core/config.h`：`RuntimeState` 增加 `root_dev`
2. `src/core/main.c`：扫描根路径后记录 `ctx.state.root_dev = root_info.st_dev`
3. `src/scan/batch_processor.c`：
   ```c
   if (st->st_dev != ctx->state.root_dev) {
       if (dev_mgr_is_blacklisted(ctx->dev_mgr, st->st_dev)) {
           result |= 2; /* blacklisted */
       }
   }
   ```
   与根路径同设备时，禁用设备级跳过，仅保留路径级熔断。

### Fix 3: 熔断清单 dev 列恢复真实值

修复后 `.circuit_breaker` 中的 `dev` 列记录真实 `st_dev`，便于多设备场景定位问题。

---

## 行为变更

| 场景 | 修复前 | 修复后 |
|------|--------|--------|
| NFS 单挂载 + 目录超时 | dev=0 假熔断，不会整体跳过；路径级熔断可能跳过子树 | 真实 dev 记录清单，但设备级熔断被 root_dev 保护禁用；仍依赖路径级熔断 |
| 多设备本地扫描 | dev=0 假熔断，设备级保护无效 | 真实 dev 生效，非根设备超时后设备级熔断正常触发 |
| 熔断清单 dev 列 | 全为 0 | 真实 st_dev |
| 退出码 | 有跳过则 1 | 有跳过则 1（不变） |

---

## 验证方法

### 编译验证

```bash
make clean && make
./listfiles --version
# 应显示: listfiles 版本 15.5.6
```

### 行为验证

1. **dev 上报验证**：人为触发目录超时/EIO，检查 `.circuit_breaker` 中 `dev` 列是否为真实 `st_dev`（非 0）。
2. **单挂载保护验证**：NFS 单挂载扫描时，触发一个目录超时，确认后续其他目录仍正常输出，不被整体跳过。
3. **多设备熔断验证**：跨设备扫描时，对非根设备制造超时，确认该设备上的后续路径被设备级熔断跳过。
4. **退出码验证**：任何跳过产生后，程序仍返回 1，`stderr` 输出 `[CRITICAL] 扫描不完整...`。

---

## 文件变更

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `include/core/config.h` | 修改 | VERSION → 15.5.6, VERSION_CODE → 202607281000UL, 新增 root_dev |
| `include/scan/worker_scanner.h` | 修改 | 新增 WorkerThreadCtx.current_dev |
| `src/ipc/worker_proc.c` | 修改 | current_dev 初始化；DEV_TIMEOUT 上报真实 dev |
| `src/scan/worker_scanner.c` | 修改 | send_error_and_empty_batch() 上报真实 dev；scan_and_send() 设置 current_dev |
| `src/core/main.c` | 修改 | 记录 root_dev |
| `src/scan/batch_processor.c` | 修改 | root_dev 单挂载保护 |

---

## 已知遗留问题

- **路径级熔断阈值固定为 3**：`CIRCUIT_BREAKER_THRESHOLD` 仍为 3，未来可考虑按目录大小/负载自适应调整。
- **熔断清单入仓**：`.circuit_breaker` 尚未结构化导入 CK 元表（路线 C3）。

---

## 相关环境

- **NFS 挂载选项**：`hard,nolock,proto=tcp,timeo=600,retrans=2`
- **存储规模**：约 1.75 亿文件 / 670TB
- **Worker 数**：8
- **触发条件**：存储高元数据负载（如大规模删除后的后台 GC）
