# Fixme: 单线程 Master 性能瓶颈修复设计

**版本**: 12.2.0  
**日期**: 2026-05-07  
**状态**: 设计已批准，编码实施中（FingerprintSet 分片 + xxHash + DeviceManager 无锁化已完成，Thread Pool + main_loop 改造待续）  
**优先级**: 高  
**影响模块**: `fingerprint_set`, `device_manager`, `main_loop`, `async_worker`, `progress`, `config`, `main`

---

## 1. 问题诊断

当前 Master 进程是单线程 `epoll` 事件循环，`main_loop_handle_batch()` 对 Worker 返回的每个 batch 中的条目**完全串行处理**。每条记录的热路径开销如下：

| 步骤 | 当前开销 | 说明 |
|------|---------|------|
| `fp_compute` | **高** | 完整 RFC 1321 MD5（64 rounds）+ `strlen(path)` |
| `fp_set_insert` | **中高** | 开放寻址 + 线性探测；扩容时全表重排；单线程无法并行 |
| `dev_mgr_is_blacklisted` | **中** | 每条记录获取 `pthread_mutex_lock` + 线性扫描 entries[] |
| `async_writer_submit` | **中** | 每条非重复记录获取 `pthread_mutex_lock` + `pthread_cond_signal` |
| `record_path` | **中** | 同步 `fwrite`，rotate 时触发 `fsync` + zlib 压缩归档 |

**根本矛盾**：Worker 是多进程并行扫描，但 Master 单线程串行去重+分发。当 Worker 总产出速率超过 Master 单线程处理能力时，Worker 被迫等待，CPU 空转。

---

## 2. 已完成的修改

### 2.1 FingerprintSet 分片改造（Sharding HashSet）

- 将单个大 HashSet 拆分为 **64 个独立 shard**
- 每个 shard 有自己的 `pthread_mutex_t` + 独立开放寻址表
- `fp_hash` 结果的高 6 位决定 shard index：`shard = (fp_hash >> 58) & 0x3F`
- 查重时只锁对应 shard，64 个线程并发查重的冲突概率极低
- 预分配：根据 `estimated_files` 预分配每个 shard 的容量，彻底避免运行时扩容

**涉及文件**: `include/fingerprint_set.h`, `src/fingerprint_set.c`

### 2.2 xxHash3 替代 MD5

- `fp_compute` 改用 `XXH3_128bits`，输入 path + dev + ino
- xxHash3 速度约为 MD5 的 **5-10 倍**
- 保持 128-bit 指纹宽度，兼容 `FingerprintSet` 的 16-byte `memcmp`
- 引入 `include/xxhash.h`（单头文件库）+ `src/xxhash.c`（定义 `XXH_IMPLEMENTATION`）

**涉及文件**: `src/fingerprint_set.c`, `include/xxhash.h`（新增）, `src/xxhash.c`（新增）

### 2.3 DeviceManager 无锁化

- `DeviceEntry.state` 改为 `_Atomic uint32_t`
- `dev_mgr_get_state` / `dev_mgr_is_blacklisted` 改为**无锁读路径**：直接遍历原子状态数组
- 写路径（`mark_dead`/`mark_alive` 等）仍用 `pthread_mutex_lock` 保护，但写频率极低（秒级）
- `count` 也改为 `_Atomic size_t`

**涉及文件**: `include/device_manager.h`, `src/device_manager.c`

---

## 3. 待完成的修改

### 3.1 Batch 处理线程池（核心）

**目标**：将 batch 中 CPU 密集的去重阶段（MD5 + HashSet）并行化到后台线程池；副作用（IPC 发送、进度写入、输出提交）仍在主线程串行执行。

**设计要点**：
- **线程数**：默认 4（可配置），远小于 Worker 数
- **任务队列**：主线程从 epoll 收到 batch 后，将整个 batch 放入有界 MPMC 队列
- **工作线程**：从队列取出 batch，并行执行 `fp_compute` → `fp_set_insert`（只竞争对应 shard mutex）→ `dev_mgr_is_blacklisted`（无锁）→ 结果写入 `uint8_t results[]` 掩码
- **同步通知**：工作线程完成后通过 `eventfd` 写入完成标记，主线程的 `epoll` 自然唤醒
- **副作用串行执行**：主线程遍历 `results[]`，按顺序执行 `ipc_send` / `record_path` / `async_writer_submit`

**新增文件**：
- `include/thread_pool.h`：线程池 + MPMC 队列 + eventfd 通知接口
- `src/thread_pool.c`：线程池实现

**修改文件**：
- `src/main_loop.c`：`main_loop_handle_batch` 提交到线程池 + 结果串行执行
- `src/main.c`：线程池初始化/销毁
- `include/config.h`：新增 `master_threads` 配置项

### 3.2 async_writer 批量提交

- 主线程维护局部批量缓冲区（256 个 `OutputTask`）
- 缓冲区满或超时时，一次性批量提交到 async_writer
- 批量提交仍用 mutex，但频率降低为 1/256
- async_writer 线程批量 dequeue 并处理

**涉及文件**: `include/async_worker.h`, `src/async_worker.c`, `src/main_loop.c`

### 3.3 record_path 批量缓冲

- `record_path` 改为显式内存缓冲：累积 4096 条记录或 1MB 数据后一次性 `fwrite`
- `line_count` 在内存中累加，达到 `progress_slice_lines` 时才执行 rotate
- 减少 fwrite 系统调用次数

**涉及文件**: `src/progress.c`, `include/progress.h`

---

## 4. 关键设计决策记录

### 为什么不把副作用也并行化？

`ipc_send`（pipe 写）、`record_path`（FILE* 写）、`async_writer_submit`（队列写）都涉及非线程安全的状态ful对象。并行化需要：
- pipe fd 加锁（或每个 Worker 一个发送线程）
- `record_path` 的 `line_count`/`write_slice_index` 加锁或原子化
- 输出顺序可能乱序

复杂度远超收益。去重阶段才是 CPU 瓶颈，副作用阶段是 IO/锁瓶颈，批量提交即可缓解。

### 为什么分片数是 64？

- 经验值：4 个工作线程 × 16 个 shard 竞争概率极低
- 64 是 2 的幂，位运算取模极快
- 每个 shard 的 mutex 内存开销可忽略（64 × ~40 bytes = 2.5KB）

### 为什么用 eventfd 而不是条件变量？

`eventfd` 可以像普通 fd 一样被 `epoll` 监听，完美融入现有的 `epoll_wait` 事件循环，无需额外的同步机制。

---

## 5. 验证指标

修复完成后应达到以下性能指标（以 16 Worker、本地 SSD、1 亿文件为基准）：

| 指标 | 当前 | 目标 |
|------|------|------|
| Master 单条处理耗时 | ~1.0 μs | ~0.2 μs |
| Master 理论峰值吞吐 | ~100 万条/秒 | ~500 万条/秒 |
| async_writer mutex 频率 | 每条记录 | 每 256 条（降低 256x）|
| dev_mgr 锁竞争 | 每条记录 | 零 |
| MD5 耗时占比 | ~30-40% | ~0%（xxHash）|

---

## 6. 环境依赖

编译本版本需要：
- `gcc` 支持 GNU11（`-std=gnu11`）
- `zlib1g-dev`（头文件 `/usr/include/zlib.h`）
- `pthread`

如果环境中缺少 `zlib.h`，请执行：
```bash
sudo apt-get update
sudo apt-get install -y zlib1g-dev
```

---

## 7. 相关设计文档

- `VERSION_12.2.0_DESIGN.md`：fpbin/pbin Footer 与两阶段提交设计（已完成并编码）
- `README.md`：已更新分片生命周期与恢复机制章节
- `CHANGELOG.md`：已记录 [12.2.0] 变更
