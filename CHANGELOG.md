# Changelog

所有显著变更均记录于此文件，格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

---

## [12.0.0] - 2026-04-29

### 重大重构：线程模型 → 进程模型

为解决 Worker 线程在 D-State 不可杀死的问题，整个运行时从多线程共享内存架构重构为 **Master-Worker 多进程模型**（`fork()` + `pipe` + `epoll`）。

#### 新增

- **进程管理模块**
  - `src/worker_proc.c` / `include/worker_proc.h`：基于 `fork()` + `pipe2(O_CLOEXEC)` 的 Worker 进程池，支持动态 spawn / replace / stop。
  - `src/probe_scheduler.c` / `include/probe_scheduler.h`：基于小根堆的渐进探测调度器，指数退避策略（5s → 10s → 20s → ... → 300s）。
- **数据结构与去重**
  - `src/fingerprint_set.c` / `include/fingerprint_set.h`：128-bit MD5 开放寻址哈希集合，内嵌 RFC 1321 MD5 实现。纯指纹存储，16 字节/条目。
  - `src/reference_map.c` / `include/reference_map.h`：与 HashSet 同构的 `fingerprint → (mtime, d_type)` 映射，支撑半增量 blind-trust。
- **IPC 协议**
  - `include/ipc_protocol.h`：Master ↔ Worker 的 TLV 消息协议，支持 `SCAN` / `BATCH` / `HEARTBEAT` / `ERROR` / `EXIT` / `STOP` 六种消息类型。
- **核心运行时**
  - `src/main_loop.c` / `include/main_loop.h`：基于 `epoll` 的单线程事件循环，集成消息分发、心跳监控、敢死队探测发射/收割。
  - `include/app_context.h`：统一上下文结构体 `AppContext`，取代所有旧版全局变量。
  - `src/async_worker.c` / `include/async_worker.h`：简化版独立输出线程，线程安全任务队列。
- **进度与归档格式**
  - `include/spbin.h`：跳过记录格式（设备熔断目录），支持 `PROBING` 和 `CONDEMNED` 两种状态。
  - `include/archive_format.h`：归档块头，含 `block_type` 字段（`0 = normal pbin`，`1 = spbin`），spbin 块固定位于归档流末尾。
- **命令行参数**
  - `--batch-size=数量`：Worker 每批发送的文件数（默认 1024）。
  - `--estimated-files=数量`：预估文件数，用于 HashSet 预分配（默认 10,000,000）。
- **输出优化**
  - `src/output.c`：增加 8MB 全缓冲（`setvbuf(..., _IOFBF, 8*1024*1024)`），显著减少 `write` 系统调用次数。

#### 变更

- **主入口重构**：`src/main.c` 完全重写，使用 `AppContext` 统一初始化、运行、销毁生命周期。
- **命令行扩展**：`src/cmdline.c` 新增 `--batch-size` 和 `--estimated-files` 长选项。
- **进度系统重写**：`src/progress.c` 全面适配新模型，支持：
  - `pbin` 分片写入与轮转
  - `spbin` 追加记录
  - `archive` 压缩归档（按 `block_type` 区分 normal / spbin）
  - 从归档 + 散落分片的游标恢复
- **设备管理器扩展**：`src/device_manager.c` / `include/device_manager.h` 增加 `DEV_STATE_CONDEMNED`（永久判死）状态。
- **Makefile 更新**：增加 `-std=gnu11` 编译选项以支持 `_Atomic`。

#### 移除

- **旧线程模型文件**（已备份为 `.bak`，不再参与编译）：
  - `src/traversal.c` → `src/traversal.c.bak`
  - `src/looper.c` → `src/looper.c.bak`
  - `src/monitor.c` → `src/monitor.c.bak`
  - `src/idempotency.c` → `src/idempotency.c.bak`
- **全局变量**：以下旧版全局变量已彻底从活动代码中移除：
  - `g_looper_mq`
  - `g_worker_mq`
  - `g_pending_tasks`
  - `g_visited_history`
  - `g_reference_history`

#### 修复

- **D-State 不可杀死问题**：Worker 从线程改为独立进程后，`SIGKILL` 可在内核层面强制终止，即使进程处于 D-State。
- **消息队列锁竞争**：移除带锁的消息队列，改用内核 pipe 的流式 IPC，消除高并发下的锁瓶颈。
- **假阳性去重风险**：使用 `path + dev + ino` 的 128-bit MD5 指纹，而非仅 inode，避免 inode 复用导致的误判。
- **Worker 心跳丢失误判**：`epoll_wait` 超时路径中集成 Monitor 巡检，500ms  granularity 确保快速发现卡死 Worker。

#### 设计约束

- **同机限制**：IPC 协议中 `struct stat` 直接通过 `memcpy` 序列化，协议**不跨机器/不跨架构**。
- **NFS 挂载前提**：扫描 NFS 目录时，必须采用 `soft,intr,timeo=600` 挂载选项。`hard` 挂载下 D-State 仍不可杀。
- **spbin 归档位置**：`spbin` 块必须是 `.archive` 文件中的**最后一个块**，恢复逻辑依赖此顺序。

---

## [11.0.0] - 2025-XX-XX

### 旧线程模型基线版本

- 多线程共享内存架构（`pthread` + 消息队列）。
- 基础断点续传（`--continue`）。
- 基础设备黑名单（`DeviceManager`）。
- CSV / 自定义格式输出。
- zlib 进度归档（`-Z` / `--archive`）。

