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

### 带断点续传的扫描

```bash
./bin/listfiles --path=/data --continue --progress-file=task1
```

中断后再次执行相同命令即可从上次位置恢复。

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
./bin/listfiles --path=/data --format="{path}\t{size}\t{user}\t{group}\t{mtime}"
```

## 命令行选项

| 选项 | 说明 |
|------|------|
| `-p, --path=路径` | 要扫描的目标目录（**必需**） |
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
| `--size, --user, --group, --mtime, --atime, --mode, --xattr` | 输出对应元数据 |
| `--follow-symlinks` | 跟踪符号链接 |
| `-Z, --archive` | 将已处理的进度分片压缩归档 |
| `-C, --clean` | 删除已处理的进度分片（不与 `-Z` 同时使用） |
| `-R, --resume-from=文件` | 仅从指定进度列表文件恢复 |
| `-v, --verbose` | 启用详细日志 |
| `-h, --help` | 显示帮助信息 |

## 架构设计

### 进程模型

```
+-------------+
|   Master    |  <-- epoll 事件循环，去重、分发、监控
|  Process    |
+--+-----+----+
   |     |
fd_in  fd_out   pipe(TLV IPC)
   |     |
+--+-----+----+
|  Worker N   |  <-- fork() 子进程，独立执行 readdir + lstat
+-------------+
```

Master 进程通过 `epoll` 监听所有 Worker 的 `fd_out`，Worker 子进程阻塞读取 `fd_in` 上的扫描任务。Worker 完成目录遍历后，将结果批次（`IPC_MSG_BATCH`）写回 Master。

### 核心模块

| 模块 | 职责 |
|------|------|
| `AppContext` | 全局统一上下文，取代所有旧版全局变量 |
| `FingerprintSet` | 128-bit MD5 开放寻址哈希集合，用于去重与存在性判断 |
| `ReferenceMap` | 指纹 → `(mtime, d_type)` 映射，支撑半增量 blind-trust |
| `WorkerPool` | `fork()` + `pipe2(O_CLOEXEC)` 的进程池管理（spawn / replace / stop） |
| `ProbeScheduler` | 基于小根堆的渐进探测调度器，指数退避：5s → 10s → 20s → ... → 300s |
| `DeviceManager` | 设备状态机：`NORMAL` → `PROBING` → `DEAD` → `CONDEMNED` |
| `MainLoop` | `epoll_wait` 循环：处理 `BATCH` / `HEARTBEAT` / `ERROR` / `EXIT` 消息 |
| `AsyncWorker` | 独立输出线程，接收主循环提交的任务，格式化并写入文件 |
| `Progress` | pbin（已处理记录）/ spbin（跳过记录）的写入、归档、恢复 |

### 进度文件格式

以 `--progress-file=task1` 为例：

| 文件 | 说明 |
|------|------|
| `task1.idx` | 原子更新的游标索引（当前分片号、处理行数等） |
| `task1_000000.pbin` | 当前正在写入的已处理记录分片 |
| `task1.spbin` | 跳过记录（熔断设备上的目录），附在归档末尾 |
| `task1.archive` | zlib 压缩的历史分片归档，块头含 `block_type` 标记 |
| `task1.config` | 会话配置快照，用于一致性校验 |

### 设备熔断与恢复流程

1. Worker 在 `readdir` 或 `lstat` 时遇到 `ETIMEDOUT` / `EIO`。
2. Worker 通过 `IPC_MSG_ERROR` 上报，Master 将该设备标记为 `DEAD`。
3. 对应目录被记录到 `spbin`，同时向 `ProbeScheduler` 注册探测任务。
4. Master 以指数退避间隔 fork 敢死队进程，对设备执行 `lstat` 探测。
5. 探测成功 → 设备恢复为 `NORMAL`，`spbin` 中该设备的积压路径重新入队扫描。
6. 探测持续失败直至退避达到上限 → 设备被标记为 `CONDEMNED`，永久跳过不再探测。

## 版本

当前版本：**11.0**

## 许可证

本项目为内部工具代码，按原项目许可证分发。
