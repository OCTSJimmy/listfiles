# DESIGN v15.5.5 — dev=0 修复与单挂载 NFS 设备熔断保护

> 状态：已实现（v15.5.6，2026-07-28）  
> 目标：修复 `IpcErrorHeader.dev` 恒为 0 的问题，同时确保修复后不会对 NFS 单挂载场景造成灾难性误伤。  
> 版本：v15.5.5（提议）→ v15.5.6（实施）

---

## 1. 背景

### 1.1 当前 dev=0 问题

Worker 上报 `ETIMEDOUT/EIO` 时，`IpcErrorHeader.dev` 被硬编码为 0：

- `src/scan/worker_scanner.c:235` — `IpcErrorHeader eh = { (uint32_t)err_code, 0 };`
- `src/ipc/worker_proc.c:175` — `IpcErrorHeader eh = { ETIMEDOUT, 0 };`

Master 侧 `main_loop_handle_error()` 使用 `err->dev` 调用 `dev_mgr_mark_dead()`，导致设备管理器实际标记的是 **dev=0**，而非真实 `st_dev`。

### 1.2 为什么 dev=0 反而是“保护”

NFS 单挂载点下，整个目录树共享同一个真实 `st_dev`。如果正确上报真实 `st_dev`：

1. 某个目录超时 → `dev_mgr_mark_dead(real_dev)`
2. 后续所有文件/目录的 `st_dev == real_dev`
3. `dev_mgr_is_blacklisted()` 返回 true
4. **整个挂载点被一次性跳过**，覆盖率可能直接归零

因此当前 `dev=0` 的“假熔断”状态，在 NFS 单挂载场景下反而避免了大规模误伤。直接修复 dev=0 而不做保护，会引入比现状更严重的风险。

### 1.3 当前真正的覆盖率损失来源

覆盖率暴跌主要来自：

- Worker 卡死被替换后，当前目录重新入队
- 同一目录连续超时 3 次后被 `circuit_breaker_check()` 永久跳过
- 该目录下所有子树随之丢失
- 高负载期间大量目录触发此路径

设备级熔断因 dev=0 未真正生效，不是覆盖率损失的主因。

---

## 2. 设计目标

1. **审计准确性**：熔断清单中的 `dev` 列记录真实 `st_dev`，便于多设备场景定位问题。
2. **单挂载保护**：NFS 单挂载扫描时，禁止设备级熔断跳过后续任务。
3. **多设备兼容**：本地多磁盘/SAN 场景保留设备级熔断能力。
4. **最小侵入**：不重构 IPC 协议，不新增复杂状态机。

---

## 3. 方案设计

### 3.1 正确上报 st_dev（dev=0 修复）

#### 方案 A：通过 WorkerThreadCtx 传递（推荐）

修改 `include/scan/worker_scanner.h`：

```c
typedef struct {
    // ...
    dev_t current_dev;      // 当前任务所在设备号
} WorkerThreadCtx;
```

修改 `src/ipc/worker_proc.c`：

- 收到 `CMD_SCAN` 时，从 `CmdScanPayload.dev` 读取设备号，设置 `ctx.current_dev`
- DEV_TIMEOUT 上报时使用 `ctx.current_dev` 而非 0

修改 `src/scan/worker_scanner.c`：

- `scan_and_send()` 已执行 `lstat(dir_path, &dir_st)`，直接使用 `dir_st.st_dev` 调用 `send_error_and_empty_batch()`
- `send_error_and_empty_batch()` 增加 `dev` 参数，填充 `IpcErrorHeader.dev`

优点：
- 不改动 IPC 协议格式
- 实现简单，风险低
- 与现有 `CmdScanPayload.dev` 字段复用

缺点：
- 需要改 3 个文件
- DEV_TIMEOUT 依赖 `worker_proc.c` 中的 `ctx.current_dev`，需确保该值在任务开始时已更新

#### 方案 B：通过 IPC 消息携带 dev

修改 `IPC_MSG_SCAN` 消息体，Worker 解析后自行保存 dev。本质与方案 A 相同，只是传递路径不同。方案 A 已利用现有 `CmdScanPayload.dev`，无需额外改动协议。

**结论**：采用方案 A。

### 3.2 单挂载 NFS 设备熔断保护

#### 方案 A：启动时检测并全局禁用（推荐）

在 `main.c` 中，扫描根路径后记录 `root_dev`：

```c
ctx->state.root_dev = root_info.st_dev;
```

在 `batch_processor.c` 的 `dev_mgr_is_blacklisted()` 判断前增加：

```c
if (st->st_dev == ctx->state.root_dev) {
    /* 与根路径同设备，视为单挂载扫描，禁止设备级跳过 */
} else {
    if (dev_mgr_is_blacklisted(ctx->dev_mgr, st->st_dev)) {
        result |= 2;
    }
}
```

优点：
- 实现极简，几乎无副作用
- 单挂载 NFS 绝对安全
- 多设备扫描仍保留设备级熔断

缺点：
- 严格依赖“根路径设备”作为唯一判断，若用户扫描路径跨设备（如 `/` 下多个挂载点），会误保护非根设备

#### 方案 B：配置开关

增加 `--disable-device-breaker` 或 `--nfs-mode`，显式禁用设备级熔断。

优点：
- 用户可控，语义明确

缺点：
- 需要用户记忆并主动启用，违背“最小配置”原则
- 遗漏启用时仍可能误伤

#### 方案 C：运行时多设备探测

统计扫描过程中出现的 `st_dev` 数量。若发现超过 1 个，则启用设备级熔断；否则禁用。

优点：
- 无需用户配置
- 多设备场景自动启用

缺点：
- 实现复杂，需要全局设备计数器
- 探测期间若根设备已熔断，可能短暂误伤

**结论**：采用方案 A（根路径设备保护），兼顾简单与安全。若未来需要更精确的多设备支持，再评估方案 C。

### 3.3 熔断清单增强

修复 dev=0 后，`.circuit_breaker` 中的 `dev` 列将记录真实 `st_dev`。建议：

- 保留现有 TSV 格式
- 在文件头注释中说明 `dev` 为十进制 `st_dev`
- 若 `root_dev` 保护启用，黑名单记录仍可写入（用于审计），但不清空 `skipped_count` 的退出码语义

---

## 4. 行为变更预期

| 场景 | 修复前 | 修复后 |
|------|--------|--------|
| NFS 单挂载 + 目录超时 | dev=0 假熔断，不会整体跳过；路径级熔断可能跳过子树 | 真实 dev 记录清单，但设备级熔断被 root_dev 保护禁用；仍依赖路径级熔断 |
| 多设备本地扫描 | dev=0 假熔断，设备级保护无效 | 真实 dev 生效，设备级熔断正常触发 |
| 熔断清单 dev 列 | 全为 0 | 真实 st_dev |
| 退出码 | 有跳过则 1 | 有跳过则 1（不变） |

---

## 5. 风险与缓解

| 风险 | 缓解 |
|------|------|
| 修复 dev=0 后 NFS 单挂载被整体跳过 | root_dev 保护，同设备不启用设备级黑名单 |
| 多设备场景根路径在设备 A，设备 B 超时未被保护 | root_dev 仅保护根设备；设备 B 正常熔断。这是预期行为 |
| WorkerThreadCtx.current_dev 未及时更新 | 在 `worker_proc.c` 收到 SCAN 时立即设置；Worker 复用时每次任务覆盖 |
| 与现有路径级熔断重复 | 路径级熔断保留，作为单挂载下的主要防线；设备级熔断作为多设备补充 |

---

## 6. 实施步骤（后续 v15.5.5）

1. `include/core/config.h`：增加 `RuntimeState.root_dev`
2. `include/scan/worker_scanner.h`：增加 `WorkerThreadCtx.current_dev`
3. `src/ipc/worker_proc.c`：SCAN 任务设置 `ctx.current_dev`；DEV_TIMEOUT 上报真实 dev
4. `src/scan/worker_scanner.c`：`send_error_and_empty_batch()` 传递真实 dev
5. `src/core/main.c`：记录 `root_dev`
6. `src/scan/batch_processor.c`：增加 root_dev 保护判断
7. 更新 fix_documents、README-BDD、Design.md

---

## 7. 开放问题

1. 是否需要 `--force-device-breaker` 开关，让用户在多设备但已知安全的场景强制启用设备级熔断？
2. `root_dev` 保护是否应该改为“若 `st_dev` 集合大小 == 1 才保护”（即方案 C），以应对用户从 `/` 开始扫描但只涉及一个挂载点的情况？
3. 熔断清单是否需要增加 `root_dev_protected` 标记，便于事后区分“被保护的设备熔断”与“真实跳过的设备熔断”？

---

*本方案仅设计，不立即编码。待方案确认后再进入实现阶段。*
