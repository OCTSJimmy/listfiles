# fixed 15.6.2 — 非法 UTF-8 文件名治理（NFSv4 EINVAL 误分类与探测假恢复循环）

> **版本**：v15.6.2（VERSION_CODE 202608251230UL）
> **日期**：2026-08-25
> **性质**：生产观察驱动的健壮性热修复（非崩溃事故；错误处理链缺陷）
> **脱敏说明**：生产环境特征已按约定脱敏——不含 IP 地址、详细路径、用户名；
> 存储与挂载细节仅以"某 NFSv4.1 挂载"指代。

---

## 1. 生产现象

某 NFSv4.1 挂载的全量扫描运行中，错误日志出现如下记录（路径已脱敏）：

```
[WARN]  [W2-Scanner] lstat failed on <目录路径> Invalid argument
[WARN]  [Error] Worker 2 unknown errno=22 on /***..., 按 PROBE_FAIL 保守处理
[ERROR] [Monitor] Worker 2 error on dev <dev>: /***... (errno=22, reason=1)
```

对现场目录名字节级取证（`ls | xxd`）确认：名字末尾为**被截断的 UTF-8 序列**
（三字节汉字的引导字节 + 单个延续字节后名字即结束，最后一个字只剩半个）。
终端显示为 `?` 即为解码失败。

## 2. 根因定性

- NFSv4 协议强制文件名必须为合法 UTF-8（RFC 7530/8881），服务端对
  LOOKUP/GETATTR 中的非法 UTF-8 名字返回 `NFS4ERR_INVAL` → 客户端
  `lstat/stat/opendir` 得到 `EINVAL(22)`。
- READDIR 不逐名校验编码，故 `readdir` 能列出名字但永远 stat 不到——
  "条目级 lstat 曾成功、目录级任务才失败"的两相现象可由 NFS readdirplus
  属性缓存解释（首次 stat 命中缓存未发 LOOKUP，缓存过期后真实 LOOKUP 才被拒）。
- 这类坏名通常是早年经 SMB/Windows 客户端或 NFSv3 写入的（v3 不校验编码）。
- **结论：这不是扫描工具自身的 bug——在 NFSv4 挂载下这些条目物理不可读，
  任何客户端代码改动都救不了 stat。治本只能在存储侧改名或经 NFSv3 挂载访问。**

## 3. 处理链缺陷（本次修复对象）

虽然数据本身不可读，但 listfiles 对该情况的处理有三个真实缺陷：

### 缺陷 A：错误分类错误 → 设备级误伤
目录级 `lstat` EINVAL 落入 `main_loop_handle_error` 的 default 分支，按
PROBE_FAIL 保守处理且 `device_level=true` → 整台设备 `dev_mgr_mark_probing` +
派敢死队探测。**单个坏文件名惩罚整台设备的调度**。

### 缺陷 B：探测子进程"恒成功" → 无限假恢复循环（最重）
探测子进程原实现为：

```c
alarm(timeout_sec);
(void)lstat(task.probe_path, &st);   /* 不看结果 */
_exit(0);                            /* 恒"成功" */
```

坏名路径的 lstat 立即返回 EINVAL（不卡住），子进程恒 `_exit(0)` → 父进程判
"设备恢复" → `spbin_requeue_recovered` 绕过 enqueued_set 整组重入队 → 再扫
再 EINVAL → 再写 spbin → 再探测……**每个坏名目录形成永不终止的循环**：

- 敢死队探测槽全局一次一个，被假探测长期霸占，真设备故障的探测被饿死延迟；
- spbin 磁盘文件 append-only，内存有 spbin_set 去重但磁盘不去重，无限膨胀；
- `skipped_count` 每轮循环 +1 持续虚增，熔断清单反复记录同一路径，统计失真；
- Worker 槽位被无效任务反复占用，日志周期性刷屏。

**对"设备会不会被误拉黑"的专项核查结论：不会。** 拉黑（CONDEMNED）的唯一路径
是探测子进程被信号杀死（alarm 超时）且重试达 `PROBE_MAX_RETRIES`——坏名 lstat
立即返回，永远攒不到判死条件；`dev_mgr_mark_dead` 为无调用点的遗产代码；
PATH_TIMEOUT 熔断与 POISON 毒丸只统计 DEV_TIMEOUT 与 Worker 死亡；条目级
ENTRY_ERROR 只写熔断清单不碰设备管理器。真实风险方向相反：不是拉黑，而是
永不消停。

### 缺陷 C：坏名条目连名字都不进输出，无法审计
条目级 `entry_stat` 失败后 `continue`——readdir 已经拿到的名字/d_type/d_ino
被一并丢弃，输出名单中完全没有该条目。事后想提取全量坏名清单去改名，只能
grep 日志。

## 4. 修复内容

| # | 修复 | 位置 |
|---|------|------|
| 1 | 分类矩阵新增 `SP_REASON_INVALID_NAME(6)`：EINVAL/EILSEQ → 条目级永久问题，`device_level=false`，CONDEMNED 永久跳过，**不触发设备探测** | `include/output/spbin.h`、`src/scan/main_loop.c`、`src/output/progress_archive.c`（恢复侧同步归类永久跳过） |
| 2 | 探测子进程退出码携带 lstat errno；父进程分类：0/ENOENT/ENOTDIR → 设备恢复整组重入队；EINVAL/EILSEQ → 设备存活，该路径改判 INVALID_NAME 永久跳过（不重入队），同设备其余路径正常恢复；其余 errno/超时 → 维持退避/判死 | `src/output/monitor.c`、`include/output/monitor.h`（新增 `active_probe_path` 留档） |
| 3 | 条目级 EINVAL/EILSEQ **退化输出**：条目写入批次（mode 取自 d_type、ino 取自 dirent，其余字段清零），照常 ENTRY_ERROR 上报（skipped_count 照计、非零退出码不变）——名单完整可审计 | `src/scan/worker_scanner.c` |
| 4 | 坏名错误附**路径字节级 hex 转储**日志（上限 256 字节）；涉及元数据可能被忽略，按既定规则全局可见、不版本门控 | `src/scan/worker_scanner.c`（`log_invalid_name_hex`） |

修复 2 同时兜底存量数据：旧版本（v15.6.1 及之前）写入 spbin 的坏名目录记录
（reason=PROBE_FAIL）在续传恢复超窗探测时，会被探测退出码机制正确改判为
INVALID_NAME 而终结循环，无需人工清理 spbin。

## 5. 日志与版本约定

- 版本号：`VERSION "15.6.2"`、`VERSION_NAME "v15.6.2"`、
  `VERSION_CODE 202608251230UL`（`include/core/config.h`）。
- 本期新增版本限定调试日志（探测退出码明细）写死调用点字面量
  `202608251200UL`，不定义宏（沿用既定规则）。
- 坏名相关 WARN/ERROR（含 hex 转储）属"元数据可能被忽略/丢失"类，一律
  全局日志（引用 VERSION_CODE 的 `log_*` 宏），不得版本门控。

## 6. 验证

- **构建**：`make` 零警告。
- **回归**：`tests/run_regression.sh` **21/21 PASS**。新增故障注入 shim
  `tests/inject_lstat.c`（LD_PRELOAD 劫持 `lstat/stat/__xstat/__lxstat`，
  按路径子串注入指定 errno；glibc < 2.33 须劫持带版本号的 `__xstat/__lxstat`，
  本期实测发现并修正），新增两个用例：
  - **用例 16（条目级）**：EINVAL 注入单文件 → exit=1 +
    `ENTRY_ERROR(errno=22)` 入熔断清单 + 坏名条目零字段退化输出 +
    hex 转储日志 + 其余条目完整；
  - **用例 17（目录级）**：EINVAL 注入子目录 → exit=1 +
    `DIR_ERROR(errno=22)` + spbin reason=6（INVALID_NAME）永久跳过 +
    无任何 `[Probe]` 活动 + 60s 超时内正常完结（无重试循环）+
    坏名子树缺席（符合物理不可读事实）+ 同树其余目录完整。

## 7. 运维侧治本建议

坏名条目在 NFSv4 挂载下不可读，只能靠：

1. **改名**：在存储服务端、或经 NFSv3 挂载（v3 不校验编码）将坏名 rename
   为合法 UTF-8；改名后重扫（或以 NFSv3 挂载整体扫描）即可完整覆盖。
2. **提取全量坏名清单**：本版本起可直接从扫描输出中筛零字段条目，或
   `grep "errno=22"` 运行日志（hex 转储精确定位原始字节）。

## 8. 修改文件清单

| 文件 | 变更 |
|------|------|
| `include/output/spbin.h` | 新增 `SP_REASON_INVALID_NAME(6)` |
| `include/output/monitor.h` | `Monitor` 新增 `active_probe_path[4096]` |
| `include/core/config.h` | VERSION 15.6.2 / VERSION_NAME v15.6.2 / VERSION_CODE 202608251230UL |
| `src/scan/main_loop.c` | 分类矩阵：EINVAL/EILSEQ → INVALID_NAME（条目级，不探测设备） |
| `src/scan/worker_scanner.c` | `log_invalid_name_hex()`；条目级 EINVAL 退化输出；目录级坏名 hex 日志 |
| `src/output/monitor.c` | 探测子进程退出码携带 errno；父进程按退出码分类（含 INVALID_NAME 改判） |
| `src/output/progress.c` | spbin_write_record 注释同步（INVALID_NAME → CONDEMNED） |
| `src/output/progress_archive.c` | 恢复侧：INVALID_NAME 归入永久跳过 |
| `tests/inject_lstat.c` | 新增：lstat/stat 故障注入 shim |
| `tests/run_regression.sh` | 新增用例 16/17 |
