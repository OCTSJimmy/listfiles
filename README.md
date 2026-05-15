# listfiles

> 高性能递归文件系统扫描器，支持智能断点续传、半增量扫描与设备熔断机制。

## 功能概览

`listfiles` 是一个面向 Linux 的底层文件系统遍历工具，旨在解决大规模（数亿文件级）目录树扫描中的几个核心痛点：

- **断点续传**：扫描过程中断后，可从上次位置恢复，无需重新开始。
- **半增量扫描**：基于历史记录的 `blind-trust` 机制，对长时间未变更的子树跳过 `lstat` 调用，显著降低 I/O 开销。
- **设备熔断**：当底层存储设备（如 NFS、 SAN LUN）无响应时，自动将对应设备加入黑名单并跳过，避免整个扫描进程陷入 D-State 不可杀死的阻塞状态。
- **设备渐进探测**：对熔断设备以指数退避策略定时探测，设备恢复后自动将积压任务重新入队。
- **多进程并发**：采用 `fork()` + `pipe` 的独立 Worker 进程模型，利用内核的 COW（写时复制）实现零拷贝共享只读上下文。

## 系统要求

- **Linux**（依赖 `fork`, `epoll`, `pipe2`, `pthread`）
- **GCC** 支持 GNU11 标准（`-std=gnu11`）
- **zlib** 开发库（`-lz`）
- **NFS 挂载建议**：如扫描 NFS 目录，务必使用 `soft,intr,timeo=600` 选项。D-State 线程无法从用户态杀死，`soft` 挂载是避免 Worker 进程永远挂起的唯一手段。

## 构建

```bash
make
```

可执行文件将生成在 `bin/listfiles`。清理构建产物：

```bash
make clean
```

## 基本用法

### 全量扫描

```bash
./bin/listfiles --path=/data
```

### 单文件目标扫描

```bash
./bin/listfiles --path=/data/single_file.txt
```

### 带断点续传的扫描

```bash
./bin/listfiles --path=/data --continue --progress-file=task1
```

中断后再次执行相同命令即可从上次位置恢复。`.idx` 游标文件会记录当前分片与已处理行数，`.pbin` 分片保留未归档的已处理记录供恢复使用。

### 半增量扫描

```bash
# 对超过 7 天未变更的文件/目录启用 blind-trust 跳过
./bin/listfiles --path=/data --continue --skip-interval=604800
```

### CSV 输出

```bash
./bin/listfiles --path=/data --csv --output=files.csv
```

### 自定义格式

```bash
./bin/listfiles --path=/data --format="%p\t%s\t%u\t%g\t%m"
```

### 按元数据开关输出

不指定 `--format` 时，可通过独立开关动态控制默认文本格式：

```bash
./bin/listfiles --path=/data --size --user --mtime
# 输出格式自动变为: path|size|user|mtime
```

## 命令行选项

| 选项 | 说明 |
|------|------|
| `-p, --path=路径` | 要扫描的目标路径（**必需**，支持目录、普通文件或符号链接） |
| `-c, --continue` | 启用智能续传 / 增量模式 |
| `--runone` | 强制全量扫描（忽略历史进度） |
| `-y, --yes` | 跳过启动时的交互式确认 |
| `--skip-interval=秒` | 半增量扫描的时间阈值（默认：0） |
| `--batch-size=数量` | Worker 每批发送的文件数（默认：1024） |
| `--estimated-files=数量` | 预估文件数，用于 HashSet 预分配（默认：10,000,000） |
| `-t, --timeout=秒` | Worker 心跳超时时间（默认：30） |
| `-f, --progress-file=前缀` | 进度文件前缀（默认：`progress`） |
| `-o, --output=文件` | 结果输出文件（默认：`output.txt`） |
| `-O, --output-split=目录` | 按行分片输出到目录 |
| `--csv` | 启用标准 CSV 输出格式 |
| `-Q, --quote` | 对输出字段进行引号包裹 |
| `-D, --dirs` | 在输出中包含目录本身的信息 |
| `-d, --print-dir` | 将当前扫描目录打印到标准错误 |
| `-F, --format=格式` | 自定义输出格式模板 |
| `--size, --user, --group, --mtime, --atime, --mode, --xattr` | 输出对应元数据（动态影响默认文本格式，不与 `--format` 同时生效） |
| `--master-threads=数量` | Master CPU 去重线程数（默认：4） |
| `--follow-symlinks` | 跟踪符号链接（递归遍历指向目录的符号链接） |
| `-M, --mute` | 禁用监控面板和诊断日志（`[System]`、`--verbose` 等），扫描数据正常输出。当不使用 `-o`/`-O` 而靠 stdout 管道化数据时，必须附加此参数。 |
| `-Z, --archive` | 将已处理的进度分片压缩归档 |
| `-C, --clean` | 删除已处理的进度分片（不与 `-Z` 同时使用） |
| `-R, --resume-from=文件` | 仅从指定进度列表文件恢复（**预留，暂未实现**） |
| `-v, --verbose` | 启用详细日志 |
| `-h, --help` | 显示帮助信息 |

### 格式说明符参考

`--format` 支持以下 `%` 前缀的说明符：

| 说明符 | 含义 | 示例 |
|--------|------|------|
| `%p` | 路径 | `/data/file.txt` |
| `%s` | 大小（字节） | `4096` |
| `%u` | 用户名 | `root(0)` |
| `%g` | 组名 | `root(0)` |
| `%U` | UID | `0` |
| `%G` | GID | `0` |
| `%m` | 修改时间 | `2024-01-15 08:30:00` |
| `%a` | 访问时间 | `2024-01-15 08:30:00` |
| `%c` | 变更时间 | `2024-01-15 08:30:00` |
| `%i` | Inode 号 | `142408` |
| `%o` | 权限字符串 | `-rw-r--r--` |
| `%O` | 权限八进制 | `0644` |
| `%t` | 文件类型 | `FILE` / `DIR` / `LINK` |
| `%X` | 扩展属性（lsattr） | `----------------` |

## 架构设计

### 进程模型

```
+-------------+
|   Master    |  <-- 消息总线，去重、分发、监控
|  Process    |
+--+-----+----+
   |     |
fd_in  fd_out   pipe(TLV IPC)
   |     |
+--+-----+----+
|  Worker N   |  <-- fork() 子进程，独立执行 readdir + lstat
+-------------+
```

Master 进程通过消息总线机制管理所有 Worker：主线程不再直接操作 fd，而是通过 **8 个常驻 IPC 线程** 分别管理每个 Worker 的非阻塞 epoll + 心跳检测 + SIGKILL。主线程自身是纯粹的消息总线，只负责：收消息（从 8 个返回队列轮询）、处理消息（BATCH 去重写文件、DEAD 收尾替换、ERROR 记日志）、发消息（SCAN 任务分发给 IPC 线程）。

故障隔离：一个 Worker 的 fd 出问题 → 只污染它自己的 IPC 线程 → IPC 线程发 DEAD 消息 → 主线程收到后优雅替换 Worker → 其他 7 路完全不受影响。彻底消除了 v12.x 单线程 epoll 架构中"一个 Worker 出问题导致整个 Master 事件循环 hang 死"的瓶颈。

Master 向 Worker 发送 `IPC_MSG_SCAN` 时采用**非阻塞写 + 积压队列**机制：`fd_in` 管道容量被提升至 1MB（默认 64KB），写满时 `ipc_send()` 返回 `EAGAIN`，任务被缓存到对应 Worker 的 `backlog_paths` 动态数组中，由主循环后续轮次重试刷出。这避免了 Master 在管道满时阻塞等待，彻底消除了双向管道死锁风险。

Master 内部另设 **`ThreadPool`**（默认 4 线程），通过 `mutex + cond` 有界队列 + `eventfd` 通知，将 CPU 密集型的指纹计算与设备黑名单检查 offload 到工作线程，避免阻塞主循环。队列满时自动降级为同步处理。

`AsyncWorker` 输出线程采用批量提交（攒 256 条记录一次性入队），将锁竞争降至 1/256。

**`Monitor`** 是独立的监控线程，每 500ms 刷新一次统计面板（输出到 **stdout**），内容包括：运行时间、活跃 Worker 数、待处理任务数、目录/文件/消费速率、输出进度、设备状态（死设备/判死设备数）、探测状态等。监控线程同时负责敢死队探测的调度与收割，使主循环专注处理 IPC 消息。Worker 心跳超时检测已下沉到 IPC 线程。

监控面板输出到 **stdout**，便于用户在终端实时查看扫描进度；其他诊断信息（`[System]` 消息、设备熔断日志、错误日志等）统一输出到 **stderr**。扫描数据输出到文件（通过 `-o` / `-O` 指定）或 **stdout**（未指定输出文件时），便于管道处理。

#### IPC 线程内部循环

```
while (running) {
    // 1. 从主线程消息队列取命令（非阻塞 drain）
    // 2. epoll_wait(fd_out + cmd_queue eventfd, 500ms)
    // 3. 处理 fd_out 事件（非阻塞 read → 解析 BATCH/HEARTBEAT/ERROR/EXIT）
    // 4. 心跳检测：超时 → SIGKILL Worker → 发 RET_DEAD → 等待 REPLACE
}
```

- 每个 IPC 线程有自己的小 epoll（2 个 fd：fd_out + cmd_queue eventfd）。
- fd 均为 O_NONBLOCK，read 采用 poll(100ms) 超时保护。
- Worker 死亡后 IPC 线程自己 close fd、epoll DEL，不需要主线程介入 cleanup。

#### 消息队列

- **eventfd + 无锁环形队列**（64 位原子 CAS head/tail）。
- 默认容量 1024 条消息/队列，有界设计天然实现背压。
- 零 mutex、零上下文切换开销、支持 64 位原子操作。

#### 消息格式

- **命令（主线程 → IPC 线程）**：
  - `CMD_SCAN`：发送 SCAN 任务路径给 Worker。
  - `CMD_REPLACE`：替换 Worker fd/pid（Worker 死亡后主线程 spawn 新 Worker，发此命令让 IPC 线程换新 fd）。
  - `CMD_STOP`：停止 IPC 线程。
- **返回（IPC 线程 → 主线程）**：
  - `RET_BATCH`：Worker 返回的扫描结果批次。
  - `RET_HEARTBEAT`：Worker 心跳（用于 Monitor 面板显示）。
  - `RET_ERROR`：Worker 遇到的设备级错误（ETIMEDOUT/EIO）。
  - `RET_DEAD`：Worker 死亡（heartbeat 超时或 epoll error/hup）。
  - `RET_EXIT`：Worker 正常退出。

### 核心模块

| 模块 | 职责 |
|------|------|
| `AppContext` | 全局统一上下文，取代所有旧版全局变量 |
| `FingerprintSet` | xxHash3 128-bit 分片开放寻址哈希集合（64 shards），用于去重与存在性判断 |
| `ReferenceMap` | 指纹 → `(mtime, d_type)` 映射，支撑半增量 blind-trust |
| `WorkerPool` | `fork()` + `pipe2(O_CLOEXEC)` 的进程池管理（spawn / replace / stop） |
| `ProbeScheduler` | 基于小根堆的渐进探测调度器，指数退避：5s → 10s → 20s → ... → 300s |
| `DeviceManager` | 设备状态机：`NORMAL` → `PROBING` → `DEAD` → `CONDEMNED` |
| `MainLoop` | `epoll_wait` 循环：处理 `BATCH` / `HEARTBEAT` / `ERROR` / `EXIT` 消息 |
| `ThreadPool` | Master 内嵌 CPU 去重线程池（`mutex + cond + eventfd`），处理指纹计算与黑名单检查 |
| `AsyncWorker` | 独立输出线程，接收主循环批量提交的任务，格式化并写入文件 |
| `Monitor` | 独立监控线程：统计面板输出、Worker 心跳超时检查、敢死队探测调度与收割 |
| `Progress` | pbin（已处理记录）/ spbin（跳过记录）的写入、归档、恢复 |

### 分片生命周期与恢复机制

#### 设计哲学：同构分片 + 页脚自描述 + 两阶段提交

- `pbin` 与 `fpbin` 采用**完全相同的物理格式**，只是生命周期和命名前缀不同。
- 已完成的分片是**自描述对象**：文件末尾自带 `Footer`，记录实际行数与校验信息，不再需要外部 `.idx` 陪伴。
- 活跃分片使用轻量 `.idx` 作为**临时草稿纸**，分片封口时通过 **"先盖钢印、再烧草稿"** 的两阶段提交安全落地。

#### 分片格式（统一结构）

```
[数据记录流]
  [path_len][path][dev][ino][mtime][d_type]
  [path_len][path][dev][ino][mtime][d_type]
  ...

[Footer: 固定 24 字节，文件最末尾]
  magic        : uint64_t  (0xDEADBEEF66AAC0FF)
  row_count    : uint64_t  (该分片实际总行数)
  data_crc32   : uint32_t  (预留：覆盖数据区的 CRC，当前填 0)
  footer_crc32 : uint32_t  (覆盖 Footer 前 16 字节的 CRC32)
```

**关键约束**：
- `Footer` 只在分片**封口（seal）**时一次性 `O_APPEND` 写入，不是持续追加。
- 活跃分片**末尾没有有效 Footer**（或即使有残留也不可信），权威来源是配套的 `.idx`。

#### idx 与 Footer 的职责边界

| 阶段 | 权威来源 | 作用 | 存在形式 |
|------|---------|------|---------|
| **活跃分片**（正在接收记录） | `{base}_00000N.idx` / `{base}.fpbin.idx` | 实时跟踪当前行数，支持原子 `rename` 更新 | 独立小文件 |
| **已封口分片**（历史/归档/转正） | `Footer`（文件末尾） | 自描述行数，随文件迁移、归档、复制 | 内嵌元数据 |
| **崩溃恢复** | Footer 优先，idx 兜底 | Footer 校验通过 → 用 Footer；Footer 残缺 → 用残留 idx | 两者配合 |

#### 进度文件格式

以 `--progress-file=task1` 为例：

| 文件 | 说明 |
|------|------|
| `task1.idx` | 原子更新的统一游标索引（当前分片号、处理行数、输出切片状态等） |
| `task1_000000.pbin` | 已封口的已完成记录分片（末尾自带 Footer，自描述行数） |
| `task1_00000N.idx` | 活跃分片的临时草稿索引（仅当该分片为活跃分片时存在，封口后删除） |
| `task1.spbin` | 跳过记录（熔断设备上的目录），附在归档末尾 |
| `task1.fpbin_000XXX` | 恢复期间隔离新发现子目录的临时分片（同构格式，支持多分片） |
| `task1.fpbin.idx` | fpbin 分片的游标索引（记录当前 fpbin 分片号与行数） |
| `task1.archive` | zlib 压缩的历史分片归档，块头含 `block_type` 与 `row_count` 元数据 |
| `task1.config` | 会话配置快照，用于一致性校验 |

#### fpbin 生命周期与转正流程

**隔离阶段（HIST_PUMP_OLD）**：
- Master 从历史 `pbin` 分片 pump 任务给 Worker。
- Worker 返回的新发现子目录**不入队、不混写 pbin**，而是追加到 `task1.fpbin_000XXX` 分片。
- `task1.fpbin.idx` 实时记录当前 fpbin 分片号与行数。

**触发扫尾（到达截止游标）**：
当最后一个历史 `pbin` 分片消费完毕：
1. **冻结 fpbin**：不再接收新发现。
2. **封口每个 fpbin 分片**：打开分片，`O_APPEND` 写入 `Footer`（行数来自 `fpbin.idx`）；`fsync` 确保落盘；关闭 fd。
3. `rename(task1.fpbin_000XXX → task1_00000N.pbin)`。
4. **校验**：以 `O_RDONLY` 重新打开所有转正后的 `pbin`，`seek(EOF - sizeof(Footer))` 读取并校验 `magic + crc`。
5. **回收**：全部校验通过后，删除 `task1.fpbin.idx`（以及可能的残留 fpbin 临时文件）。

**后续阶段**：
- 新发现直接写入新的 `pbin` 活跃分片（延续原有 `task1.idx` 逻辑）。
- `fpbin` 机制关闭，直到下一次 `--continue` 恢复时按需重新创建。

#### 崩溃恢复策略

```
restore_progress()
    │
    ├── 扫描目录，识别所有 *.pbin 与 *.fpbin_*
    │
    ├── 对每个已完成的历史 pbin 分片：
    │      seek(EOF - sizeof(Footer))
    │      读取 Footer → 校验 magic + crc
    │      ├─ 通过 → row_count = footer.row_count，加载到 visited_set
    │      └─ 失败 → row_count = 残留 idx（如果有），加载到 visited_set
    │
    ├── 识别当前活跃分片（匹配 task1.idx 中的 write_slice_index）
    │      行数以 task1.idx 为准（活跃分片无有效 Footer）
    │
    └── 如果存在残留 fpbin 分片且 fpbin.idx 有效：
           视为上次转正中断，重新执行封口 + rename + 校验
```

**自动清理**：恢复时若发现某个 `pbin` 的 `Footer` 有效但旁边残留了同名 `.idx`，直接 `unlink` 该残留 idx（**钢印清晰则烧草稿**）。

#### 归档与读取

- 压缩 `pbin` 分片前，先 `seek` 读 `Footer` 获取 `row_count`，写入 `ArchiveBlockHeader` 作为元数据。
- 恢复归档时：解压后得到临时文件，再次读取 `Footer` 校验，双重确认。
- 无硬算假设：彻底删除代码中所有 `slice_index * BATCH_SIZE` 或 `slice_index * SLICE_ROWS` 的推断逻辑。每个分片的行数必须来自其自身的 `Footer` 或 `idx`，绝不假设固定。

### 设备熔断与恢复流程

1. Worker 在 `readdir` 或 `lstat` 时遇到 `ETIMEDOUT` / `EIO`。
2. Worker 通过 `IPC_MSG_ERROR` 上报，Master 将该设备标记为 `DEAD`。
3. 对应目录被记录到 `spbin`，同时向 `ProbeScheduler` 注册探测任务。
4. Master 以指数退避间隔 fork 敢死队进程，对设备执行 `lstat` 探测。
5. 探测成功 → 设备恢复为 `NORMAL`，`spbin` 中该设备的积压路径重新入队扫描。
6. 探测持续失败直至退避达到上限 → 设备被标记为 `CONDEMNED`，永久跳过不再探测。

## 版本

当前版本：**13.0.1**

## 许可证

本项目为内部工具代码，按原项目许可证分发。
