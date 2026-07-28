# Fixed Document: v15.5.8 — 2026-07-29

## 问题概述

v15.5.7 已为全部 errno 通道装上监控（目录级/条目级错误、readdir 中途失败、路径截断），但生产实测 R3 仍然 **24.6M 条目（覆盖率 25.28%）+ 熔断清单空 + 退出码 0**——6500 万条目的丢失没有触发任何 errno。《扫描完整性故障总结 v2.0.0》的形态判别给出两条硬证据：

1. **整子树消失**：缺失样本的父目录存活率 0.0%（5000 采样 / 35 个去重父目录零命中）——"条目级 readdir 截断"作为主机制被排除；
2. **"已见未扫"统一签名**（30/30 采样）：边界目录本身作为条目存在于输出（父目录 readdir 看到了它），但其内容在输出中为零后代——目录从未被派发扫描；
3. **完结面板挂起非零**：`Pending tasks: 4 / Pending batches: 1 / Dispatch queue: 0` 下仍然 SUCCESS、退出码 0。

结论：丢失发生在**派发记账层**（机制 B："列而未派"），不产生任何系统调用错误，v15.5.7 的 errno 检测族对此原理性失效。

---

## 根因分析

### Root Cause 1（主谋）：pbin 滑动窗口与分片轮转删除的竞争

v15.5.1 引入的 `load_dirs_from_pbin()` 是队列达到 `DISPATCH_QUEUE_HIGH_WATER`(100000) 时被跳推目录的**唯一兜底**：跳推目录只写入 pbin（进度归档），等队列降到 LOW_WATER 再从 pbin 按分片+字节游标回填。

但 `src/output/progress_archive.c` 的 `process_old_slice()` 在每 10 万条记录封口轮转时**默认 unlink 删除已封口分片**（未开 `-Z` 压缩时）。加载器游标一旦追到已被删除的分片，`fopen` 失败 → `break` → **永久卡死且无日志**——此后所有 HIGH_WATER 跳推目录全部静默丢失。

佐证链：
- R3 仅 37 分钟跑完"1.75 亿文件"（加载器早死、pbin 从未回填重扫，所以快得反常；rclone 遍历同存储需数小时）；
- 覆盖率随存储负载剧烈波动（14%/25%/47%）——队列积压到 HIGH_WATER 的概率取决于 worker 产出与派发消耗的瞬时差；
- 漏采部位各次扫描不一致（负载形态不同，跳推集合不同）。

### Root Cause 2：HIGH_WATER 跳推零记录

跳推目录既不进 dispatch_queue、也不进任何任务账（`pending_tasks` 只在派发成功时 +1），唯一的痕迹是 pbin 里的一行记录——而 pbin 分片会被轮转删除。丢失完全无痕。

### Root Cause 3：MSG_DROP 回队不销账

Worker 在 Replacement 窗口期拒收任务回送 `MSG_DROP` 时，Master 把任务重新入队但**不递减 `pending_tasks`**（派发时已 +1，重派发再 +1，FINISH 只 -1）——计数永久泄漏。R3 完结面板 `Pending tasks: 4` 即此泄漏；泄漏非零时完结检查依赖其他路径侥幸通过，完结校验形同虚设。

### Root Cause 4：完结无硬性断言

扫描结束只看 `pending_tasks==0 && batches==0 && queue==0 && hist==DONE`——跳推目录不在任何账上（见 RC2），"流程完结"不能推出"数据完整"。

---

## 修复方案

### Fix 1：dspill 派发兜底文件，替代 pbin 滑动窗口

新增运行级追加文件 `{progress_base}.dspill`（复用 pbin 记录格式）：

- `dspill_append()`（`src/scan/dispatch.c`）：batch_processor 在队列 ≥ HIGH_WATER 时把跳推目录追加到 dspill（懒打开 "ab"、64KB 缓冲、每条 fflush）。dspill 写失败时记 `DSPILL_IO` 熔断清单并强行入队——**宁可队列膨胀也不丢目录**。
- `load_dirs_from_dspill()`：主循环在队列 ≤ LOW_WATER 时按**字节游标**回填；游标只在记录成功入队后前进，入队失败回退游标下轮重试（v15.5.1 游标越记丢失教训）。
- **无分片轮转、无删除竞争、只含跳推目录**——从设计上消除 Root Cause 1。
- 写端为 batch_processor（线程池线程），读端为主线程，经 `dspill_mutex` 互斥。
- 启动时删除陈旧 dspill（恢复模式下未完成目录会经根目录重扫重新发现，遗留 dspill 会造成重复派发）；成功完结后删除 dspill，有错误时保留供审计。
- 完结时打印 `[Dspill] HIGH_WATER 跳推目录 N 个，回填 M 个` 统计（log_info，`-v` 可见）。

### Fix 2：完结硬性断言（终止前 dspill 必须排空到 EOF）

主循环终止检查满足全部静默条件后，若 `dspill_fp` 存在：

1. 最后跑一次 `load_dirs_from_dspill()`——有产出则回填派发、继续扫描，**不得完结**；
2. 无产出则 stat dspill 文件，`st_size > dspill_read_offset` 即残留（游标无法推进：记录损坏/持续入队失败）→ 记 `DSPILL_RESIDUE` 熔断清单 → `skipped_count>0` → `[CRITICAL]` + 退出码 1。

不允许存在"挂起任务非零仍 SUCCESS"的路径。

### Fix 3：MSG_DROP 销账

`MSG_DROP` 回队时 `atomic_fetch_sub(&pending_tasks, 1)`（重派发时会重新 +1）；回队失败（队列满/OOM）属任务真正丢失，记 `TASK_DROP_LOST` 熔断清单。

### Fix 4：nlink oracle（`--strict-nlink`，默认关）

针对故障总结 §3.3 机制 A（NFS 协议层**无 errno 的假空/假 EOF**，任何 errno 检查原理上无法捕获）的唯一客户端可检旁证：

- POSIX：非空目录 `st_nlink = 2 + 直接子目录数`；
- readdir 正常结束（errno=0）后比对"实际读到的子目录数 vs st_nlink−2"，不符即以 `errno_code=0` 的 `IPC_MSG_ENTRY_ERROR` 上报，Master 记录 **`NLINK_MISMATCH`** 熔断清单 → 退出码 1；
- **默认关闭、须显式 `--strict-nlink` 开启**：NFS/btrfs 等文件系统 nlink 语义不可靠，且扫描期间并发增删子目录会误报；条目级异常（`entry_anomalies>0`）或 readdir 出错时对本目录禁用 oracle 防误报。

### 已知原理性盲区（如实说明）

**纯文件目录**的无 errno 假空/截断（无子目录可供 nlink oracle 比对）客户端无法检测，只能靠跨运行对账（同参数重扫 + 集合差）。测试集中用例 5a 将该盲区作为预期行为存档。

---

## 行为变更

| 场景 | 修复前 | 修复后 |
|------|--------|--------|
| 队列 ≥ HIGH_WATER 的跳推目录 | 仅写 pbin，分片被轮转删除后加载器卡死，目录静默丢失 | 追加 dspill（无轮转无删除），LOW_WATER 时按游标回填 |
| dspill 写失败 | —（pbin 路径无此概念） | 记 `DSPILL_IO` + 强行入队，退出码 1 |
| 完结时 dspill 未消费完 | （不可能发生——加载器已卡死） | 回填后继续扫描；无法推进记 `DSPILL_RESIDUE`，退出码 1 |
| MSG_DROP 回队 | pending_tasks 泄漏（R3 面板 pending=4 仍 SUCCESS） | 销账；回队失败记 `TASK_DROP_LOST`，退出码 1 |
| NFS 无 errno 假空/假 EOF（含子目录） | 无感通过（任何版本都无法捕获） | `--strict-nlink` 下记 `NLINK_MISMATCH`，退出码 1 |
| 纯文件目录无 errno 截断 | 无感通过 | 无感通过（原理性盲区，文档存档） |

---

## 测试集（tests/，规格 v2 落地）

- `tests/inject_readdir.c` — LD_PRELOAD shim，劫持 `readdir()` 制造**无 errno 假空/假 EOF**（`LF_TARGET_SUBSTR` 定位目录、`LF_FAKE_EMPTY_AFTER=N` 控制第 N 条后截断）。这是 v15.5.7 本地故障注入（chmod EACCES）只覆盖 errno 路径的核心缺口。
- `tests/gen_fixture.py` — 已知真值 fixture：7 万+ 条目单目录（64 模板硬链轮转，规避 ext4 单 inode 65000 硬链上限）、>4096 深路径（逐级 chdir）、GBK 非法 UTF-8 文件名（bytes 路径）、文件/目录/悬空软链、2000 个并发删除专用文件、8 子目录注入靶点；二进制安全 manifest。
- `tests/run_regression.sh` — 10 用例回归：

| # | 用例 | 验收 |
|---|------|------|
| 1 | 已知真值基线 | 输出与 manifest sorted diff=0，exit 0 |
| 2 | 并发一致性 | workers=1/8/16 三方 **sorted** 输出一致（规格原文"逐字节一致"——多 worker 分片写出顺序本就不确定，按 sorted 全文一致验收） |
| 3 | EACCES opendir（chmod 000） | exit 1 + `DIR_ERROR(errno=13)` |
| 4 | EACCES 条目 lstat（chmod 444） | exit 1 + `ENTRY_ERROR(errno=13)` |
| 5a | 假空 readdir 注入，无 `--strict-nlink` | exit 0（盲区存档证明） |
| 5b | 假空 readdir 注入 + `--strict-nlink` | exit 1 + `NLINK_MISMATCH` |
| 6 | 中途假 EOF（第 5 条后）+ `--strict-nlink` | exit 1 + `NLINK_MISMATCH` |
| 7 | dspill 压力（标桩构建 HIGH=8/LOW=4/BATCH=16，2000 目录） | 输出与 manifest diff=0、`[Dspill]` 统计出现、完结后 dspill 已删除、exit 0 |
| 8 | 并发删除豁免（扫描期间后台 rm churn/） | exit 0、清单无记录、输出 ⊆ manifest |
| 9 | 深路径（>4096） | exit 1 + `ENTRY_ERROR(errno=36)` |

---

## 验证方法

```bash
make clean && make
./bin/listfiles --version        # 应显示 15.5.8
tests/run_regression.sh          # 全量回归
```

### 实际验证结果（2026-07-29，全部通过）

- 冒烟：88 条目小树 diff=0；`--strict-nlink` 正常树零误报；EACCES 注入 exit=1 + `DIR_ERROR(errno=13)`；
- dspill 手动实测（标桩 HIGH=8）：200 目录 fixture 溢出 192 → 回填 192，`[DspillLoader]`/`[Dspill]` 日志齐全，dspill 完结后自删，exit=0；
- 回归全绿：**PASS=10 FAIL=0**（用例 5b 实测日志：`NLINK_MISMATCH on .../inject_target: st_nlink=10 (expect 8 subdirs) but readdir saw 0`；用例 7 实测跳推 1984 全部回填）。

### 测试过程中的副产品发现

回归脚本复用工作目录时曾出现"空扫描"：`load_session_config()` 对 `.config status=Success` **无需 `-c` 即自动开启续传**，上一轮的 pbin 记录使本轮被 completed_set 剪成空扫描。`run_regression.sh` 已改为每轮强制全新工作目录（带防呆标记）。这同时提醒生产用法：同前缀重扫即为续传/差量语义，要全量重扫须换全新 `-f` 前缀或 `--runone`。

---

## 文件变更

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `include/core/config.h` | 修改 | VERSION → 15.5.8，VERSION_CODE → 202607290900UL；Config 新增 `strict_nlink` |
| `include/core/app_context.h` | 修改 | pbin 游标字段替换为 dspill 四字段 + `dspill_mutex` |
| `include/output/progress.h` | 修改 | `get_dspill_filename()` 声明 |
| `include/scan/main_loop.h` | 修改 | `load_dirs_from_pbin` 声明替换为 `dspill_append`/`load_dirs_from_dspill` |
| `src/output/progress.c` | 修改 | `get_dspill_filename()` 实现 |
| `src/scan/batch_processor.c` | 修改 | HIGH_WATER 跳推改投 dspill（无 progress_base 时强行入队） |
| `src/scan/dispatch.c` | 修改 | `dspill_append()`/`load_dirs_from_dspill()`（替代 `load_dirs_from_pbin`） |
| `src/scan/main_loop.c` | 修改 | 7.5 加载点改 dspill；完结硬性断言 + `DSPILL_RESIDUE`；MSG_DROP 销账 + `TASK_DROP_LOST`；`RET_ENTRY_ERROR` 区分 `NLINK_MISMATCH`（errno_code=0） |
| `src/scan/worker_scanner.c` | 修改 | nlink oracle（`subdir_count`/`entry_anomalies`，`--strict-nlink` 门控） |
| `src/core/main.c` | 修改 | dspill_mutex init/destroy；启动清理陈旧 dspill；完结统计与成功自删；destroy 关闭 dspill_fp |
| `src/core/cmdline.c` | 修改 | `--strict-nlink` 选项 + help |
| `tests/inject_readdir.c` | 新增 | LD_PRELOAD 无 errno 假空/假 EOF 注入 shim |
| `tests/gen_fixture.py` | 新增 | 已知真值 fixture 生成器 |
| `tests/run_regression.sh` | 新增 | 10 用例回归脚本 |

---

## 对生产环境的提示

- dspill 溢出目录现在会得到**真正回填**（R3 从未发生）。极端负载下扫描时间会比 v15.5.7 的"假象"更长——这是完整性的代价，不是性能回退。
- 建议迁移/处置管线的扫描一律加 `--strict-nlink`（源端 ParaStor NFS 若 nlink 语义可靠）；若出现 `NLINK_MISMATCH` 误报（并发增删子目录所致），去掉该参数重扫并以跨运行集合差对账。
- `-f` 进度前缀复用旧前缀时，dspill 不参与恢复（未完成目录经根目录重扫重新发现）；全新扫描请用全新前缀。
