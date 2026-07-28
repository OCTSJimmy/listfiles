# listfiles Project Structure

## Directory Layout (after refactor)

```
listfiles/
├── bin/                    # Build output (binary)
├── build/                  # Build artifacts (.o files)
├── docs/                   # Documentation
│   ├── README.md
│   ├── Design.md
│   ├── CHANGELOG.md
│   ├── README-BDD.md
│   ├── AUDIT_12.2.0_plus.md
│   └── fixed_*.md          # Per-fix documentation
├── include/                # Header files (grouped by module)
│   ├── core/               # Core framework
│   │   ├── app_context.h
│   │   ├── circuit_breaker.h
│   │   ├── cmdline.h
│   │   ├── config.h
│   │   ├── signals.h
│   │   └── utils.h
│   ├── ipc/                # Inter-process communication
│   │   ├── ipc_protocol.h
│   │   ├── ipc_thread.h
│   │   ├── msg_format.h
│   │   ├── msg_queue.h
│   │   └── worker_proc.h
│   ├── scan/               # Scan engine
│   │   ├── device_manager.h
│   │   ├── fingerprint_set.h
│   │   ├── lost_tasks.h
│   │   ├── main_loop.h
│   │   ├── probe_scheduler.h
│   │   ├── reference_map.h
│   │   ├── thread_pool.h
│   │   └── worker_scanner.h    # WorkerThreadCtx、scanner 线程接口
│   ├── output/             # Output & progress
│   │   ├── archive_format.h
│   │   ├── async_worker.h
│   │   ├── monitor.h
│   │   ├── output.h
│   │   ├── progress.h
│   │   └── spbin.h
│   └── util/               # Utilities
│       ├── log.h
│       └── xxhash.h
├── lib/                    # Third-party libraries
│   └── zlib/
│       ├── zconf.h
│       └── zlib.h
├── tests/                  # 回归测试集 (v15.5.8)
│   ├── inject_readdir.c    # LD_PRELOAD 无 errno 假空/假 EOF 注入 shim
│   ├── gen_fixture.py      # 已知真值 fixture 生成器（7万条目/深路径/GBK/软链/并发删除）
│   └── run_regression.sh   # 10 用例回归脚本
├── src/                    # Source files (mirrors include/ structure)
│   ├── core/
│   │   ├── main.c
│   │   ├── circuit_breaker.c
│   │   ├── cmdline.c
│   │   ├── signals.c
│   │   └── utils.c
│   ├── ipc/
│   │   ├── ipc_protocol.c    # IPC TLV 消息封装（send/recv/drain）
│   │   ├── ipc_thread.c      # IPC 线程生命周期与 epoll 主循环
│   │   ├── ipc_message_handler.c  # IPC 消息接收与处理（控制/数据/命令）
│   │   ├── ipc_worker_mgmt.c    # Worker 生命周期管理（死亡标记/超时杀掉/返回消息）
│   │   ├── msg_queue.c
│   │   └── worker_proc.c     # Worker 进程池管理与主入口
│   ├── scan/
│   │   ├── main_loop.c         # 主消息总线与调度循环框架
│   │   ├── batch_processor.c   # Batch 解析、去重、完成处理
│   │   ├── dispatch.c          # 任务分发、Worker 清理、IPC send 辅助
│   │   ├── device_manager.c
│   │   ├── probe_scheduler.c
│   │   ├── fingerprint_set.c
│   │   ├── reference_map.c
│   │   ├── thread_pool.c
│   │   ├── lost_tasks.c
│   │   └── worker_scanner.c  # Worker 扫描引擎与 Scanner 线程
│   ├── output/
│   │   ├── output.c            # 核心格式化输出引擎 (print_to_stream, cleanup_cache)
│   │   ├── output_metadata.c   # 元数据辅助函数 (权限/xattr/用户名/组名缓存)
│   │   ├── output_format.c     # 格式预编译与文件管理 (precompile_format/切片轮转)
│   │   ├── progress.c
│   │   ├── progress_io.c
│   │   ├── progress_archive.c
│   │   ├── async_worker.c
│   │   └── monitor.c
│   └── util/
│       ├── log.c
│       └── xxhash.c
├── Makefile
├── .gitignore
└── TODO.md
```

## Naming Conventions

- **Files**: `snake_case.c` / `snake_case.h`
- **Functions**: `module_action()` (e.g., `ipc_thread_init()`)
- **Types**: `ModuleName` (e.g., `IpcThreadCtx`, `WorkerSlot`)
- **Macros**: `MODULE_CONSTANT` (e.g., `IPC_MSG_SCAN`)
- **Include guards**: `MODULE_FILENAME_H` (e.g., `IPC_THREAD_H`)

## Build System

- `make` - Build the project
- `make clean` - Clean build artifacts
- Compiler: `gcc` with `-Wall -Wextra -std=gnu11`
- Include paths: `-Iinclude -Iinclude/core -Iinclude/ipc -Iinclude/scan -Iinclude/output -Iinclude/util -Ilib/zlib`
