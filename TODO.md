# 代码注释任务清单

> 目标：为所有 .c 文件添加简体中文注释，包括文件头注释和函数注释（参数类型、作用、取值范围、返回值类型、作用、取值范围）。
> 忽略：.git/ 目录、.gitignore 中列出的目录/文件。

## 模块划分与进度

### 1. 工具与杂项模块 ✅
- [x] `src/xxhash.c` — xxHash3 128-bit 哈希实现封装 (3 行)
- [x] `src/utils.c` — 通用工具函数 (44 行)
- [x] `src/signals.c` — 信号处理 (95 行)

### 2. 数据结构模块 ✅
- [x] `src/reference_map.c` — 指纹→(mtime,d_type) 映射，支撑半增量 blind-trust (108 行)
- [x] `src/fingerprint_set.c` — xxHash3 128-bit 分片开放寻址哈希集合，用于去重 (168 行)

### 3. 设备管理模块 ✅
- [x] `src/device_manager.c` — 设备状态机管理 (111 行)
- [x] `src/probe_scheduler.c` — 渐进探测调度器，指数退避策略 (145 行)

### 4. 并发与线程模块 ✅
- [x] `src/async_worker.c` — 异步输出工作线程 (114 行)
- [x] `src/thread_pool.c` — Master 内嵌 CPU 去重线程池 (180 行)

### 5. 命令行与入口模块 ✅
- [x] `src/cmdline.c` — 命令行参数解析 (243 行)
- [x] `src/main.c` — 程序主入口，初始化与资源清理 (316 行)

### 6. Worker 与主循环模块 ✅
- [x] `src/worker_proc.c` — Worker 子进程实现，目录遍历与 IPC (428 行)
- [x] `src/main_loop.c` — Master epoll 主循环，处理 IPC 消息 (492 行)

### 7. 输出与监控模块 ✅
- [x] `src/output.c` — 格式化输出引擎 (542 行)
- [x] `src/monitor.c` — 监控线程，统计面板与心跳检查 (294 行)

### 8. 进度与归档模块 ✅
- [x] `src/progress.c` — 进度文件（pbin/spbin/fpbin）的写入、归档、恢复 (1264 行)

---
✅ 全部完成！总计 16 个 .c 文件，约 4547 行代码。
