# Fixed Document: v15.5.4 — 2026-07-28

## 问题概述

生产环境（NFS hard 挂载 + 高元数据负载）多次观察到 listfiles 扫描覆盖率剧烈波动（47% → 14%），但工具仍返回退出码 0、生成 `SCAN_COMPLETE.flag`、chunk 校验 PASS，导致下游迁移/删除管线将不完整清单当作全量清单消费。

---

## 现象

### 覆盖率异常

| 扫描 | 时间 | 清单条目 | 对照真值 | 覆盖率 |
|---|---|---|---|---|
| 源端首次扫描 | 2026-07-12~14 | 82,247,759 | df -i 推算 175,362,928 | ~47% |
| 源端第二次扫描 | 2026-07-27 22:15 | 13,314,887 | df -i IUsed 93,161,669 | ~14% |
| rclone 独立遍历 | 2026-07-28 | 83,711,108+ | 趋势收敛于 df -i | — |

### 成功假象

- 所有不完整扫描均正常退出（退出码 0）
- chunk 校验脚本（行数/文件大小/编号连续性）全部 PASS
- 默认日志中无任何跳过/熔断/超时记录

---

## 根因分析

### Root Cause 1: 熔断/超时无独立审计记录（P0）

`ETIMEDOUT/EIO`、设备黑名单命中、路径级熔断跳过均只依赖日志输出，且部分日志被版本化阈值默认静默（`202607030000 < VERSION_CODE`）。无独立、持久化的跳过路径清单，事后无法审计。

### Root Cause 2: 探测退避被重置，永不判死（P0）

`src/output/monitor.c` 的 `reap_probes()` 每次探测失败后：

```c
ProbeTask task = {0};
task.probe_interval = PROBE_INTERVAL_INITIAL;  // 5s
task.retry_count = 0;                          // 重置为 0
```

导致 `retry_count` 永远为 0，无法触发指数退避，也永远不会 `CONDEMNED`。

### Root Cause 3: Monitor 刷屏（P1）

`src/output/monitor.c:152` 仅判断 `isatty()`，在 `TERM=dumb` 或日志重定向场景下 `\033[2J\033[H` 无限滚动。

### Root Cause 4: 扫描不完整却返回 0（P0）

`RuntimeState` 无跳过计数，`main()` 只检查 `has_error`（部分路径未设置），导致大量跳过未触发非 0 退出。

---

## 修复方案

### Fix 1: 独立熔断清单 `{progress_base}.circuit_breaker`

新增 `src/core/circuit_breaker.c` / `include/core/circuit_breaker.h`：

- 以追加模式打开 `{progress_base}.circuit_breaker`
- 每次因 `BLACKLIST` / `DEV_TIMEOUT` / `EIO` / `PATH_TIMEOUT` / `CONDEMNED` 跳过路径时写入 TSV 记录并立即 `fflush`
- 即使文件无法打开，也原子累加 `skipped_count`，确保退出码非 0

调用点：
- `src/scan/batch_processor.c` — 黑名单命中
- `src/scan/main_loop.c` — `ETIMEDOUT/EIO` 错误
- `src/scan/dispatch.c` — 路径级熔断触发
- `src/output/monitor.c` — 设备判死

### Fix 2: 退出码与退出警示

`src/core/main.c` 结束阶段：

```c
if (ctx.state.skipped_count > 0) {
    fprintf(stderr, "[CRITICAL] 扫描不完整：已跳过 %lu 个路径。详见 %s.circuit_breaker\n",
            ctx.state.skipped_count, ctx.cfg.progress_base);
    ctx.state.has_error = true;
}
return ctx.state.has_error ? 1 : 0;
```

`.config` 复用已有逻辑写入 `status=Incomplete`、`error=DeviceMeltdown`。

### Fix 3: 探测真正指数退避 + 判死

`src/output/monitor.c`：

- `dispatch_probes()` 保存当前任务的 `retry_count` / `probe_interval`
- `reap_probes()` 探测失败后：
  - `retry_count++`
  - `probe_interval *= 2`（上限 300s）
  - `retry_count >= PROBE_MAX_RETRIES`（6）时判死
  - 最后两次重试使用 15s 超时（而非 5s）

### Fix 4: Monitor 刷屏最小修复

```c
const char *term = getenv("TERM");
if (isatty(fileno(fp)) && term && strcmp(term, "dumb") != 0) {
    fprintf(fp, "\033[2J\033[H");
}
```

管道场景用户自行使用 `--mute`。

---

## 验证方法

### 编译验证

```bash
make clean && make
./listfiles --version
# 应显示: listfiles 版本 15.5.4
```

### 行为验证

1. **清单文件生成**：扫描后检查 `{progress_base}.circuit_breaker` 存在且含表头。
2. **退出码**：人为制造目录超时/黑名单后，程序应返回 1，`stderr` 输出 `[CRITICAL] 扫描不完整...`。
3. **指数退避**：观察 `[Probe] dev ... retry N scheduled after ...` 日志，确认 interval 翻倍。
4. **判死**：达到 6 次失败后应出现 `dev ... condemned after 6 retries`，且 `.circuit_breaker` 记录 `CONDEMNED`。
5. **Monitor 刷屏**：`TERM=dumb` 或非 tty 时，不应看到面板重复输出。

---

## 文件变更

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `include/core/circuit_breaker.h` | 新增 | 熔断清单模块头文件 |
| `src/core/circuit_breaker.c` | 新增 | 清单文件初始化/记录/关闭 |
| `include/core/config.h` | 修改 | VERSION → 15.5.4, VERSION_CODE → 202607280900, LOG_VERSION_CODE → 202607280930, 新增 skipped_count |
| `include/core/app_context.h` | 修改 | 新增 circuit_breaker_fp / circuit_breaker_mutex |
| `include/scan/probe_scheduler.h` | 修改 | 新增 PROBE_MAX_RETRIES |
| `include/output/monitor.h` | 修改 | 新增 active_probe_retry_count / active_probe_interval |
| `src/scan/batch_processor.c` | 修改 | 黑名单命中时记录清单 |
| `src/scan/main_loop.c` | 修改 | ETIMEDOUT/EIO 时记录清单 |
| `src/scan/dispatch.c` | 修改 | 路径级熔断触发时记录清单 |
| `src/output/monitor.c` | 修改 | TERM 判断 + 指数退避 + 判死 + CONDEMNED 记录 |
| `src/core/main.c` | 修改 | 清单初始化/关闭、退出警示、退出码 |

---

## 已知遗留问题

- **DEV_TIMEOUT/EIO 上报 dev=0**：`src/scan/worker_scanner.c:235` 和 `src/ipc/worker_proc.c:175` 硬编码 `IpcErrorHeader.dev = 0`。当前清单已记录路径，单挂载 NFS 场景下影响有限。若修复需同时做“单挂载保护”，否则真实设备号被黑名单后可能误伤整个 NFS 挂载。
- **单挂载 NFS 设备熔断保护**：当前设备级熔断以 `st_dev` 为粒度。修复 dev=0 前必须增加单挂载保护逻辑，防止整个挂载点被一次性跳过。

---

## 相关环境

- **NFS 挂载选项**：`hard,nolock,proto=tcp,timeo=600,retrans=2`
- **存储规模**：约 1.75 亿文件 / 670TB
- **Worker 数**：8
- **触发条件**：存储高元数据负载（如大规模删除后的后台 GC）
