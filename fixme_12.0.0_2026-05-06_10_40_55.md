# listfiles 待修复问题清单

> 版本: 12.0.0 (进程模型重构版)
> 测试时间: 2026-05-06 10:40:55
> 测试环境: Linux x86_64, GCC, 本地 ext4 文件系统

---

## 一、编译与构建问题 (Build Breakers)

### [BUG-001] 编译失败：缺少头文件依赖

**严重级别**: 🔴 阻断

**描述**: 干净编译时，`probe_scheduler.c`、`fingerprint_set.c`、`main_loop.c` 因缺少头文件而报错。

**具体错误**:
- `probe_scheduler.h`: 使用 `bool` 但未 `#include <stdbool.h>`
- `probe_scheduler.c`: 使用 `SP_STATUS_CONDEMNED` 但未 `#include "spbin.h"`
- `fingerprint_set.h`: 使用 `size_t` 但未 `#include <stddef.h>`
- `main_loop.c`: 使用 `DT_DIR` 但未 `#include <dirent.h>`

**修复建议**: 在对应头文件/源文件中补充缺少的 `#include`。

---

## 二、核心功能缺陷 (Functional Bugs)

### [BUG-002] 默认输出格式未编译导致空输出

**严重级别**: 🔴 阻断

**描述**: 当不使用 `--format` 参数时，`precompile_format()` 仅在 `cmdline.c` 中的 `if (cfg->format)` 分支内被调用。这意味着默认格式（`%p|%s|%m` 或 CSV 默认格式）永远不会被编译，`cfg->format_segment_count` 保持为 0，`print_to_stream()` 除了输出换行符外不会打印任何文件信息。

**复现**:
```bash
./bin/listfiles --path=/some/dir --yes
# 输出仅有换行符，无路径/大小/时间信息
```

**修复建议**: 在 `parse_arguments()` 末尾无条件调用 `precompile_format(cfg)`，无论用户是否显式指定 `--format`。

---

### [BUG-003] 空目录扫描导致死锁 / Worker 无限超时替换

**严重级别**: 🔴 阻断

**描述**: 扫描空目录时，Worker 的 `readdir` 只读到 `.` 和 `..`，`count` 为 0。`send_batch()` 在 `count <= 0` 时直接返回，不发送任何 `IPC_MSG_BATCH`。Master 的 `pending_tasks` 永远不会减到 0，终止条件无法满足，程序卡死。Monitor 每 30 秒检测到 Worker 心跳超时，不断 `kill(SIGKILL)` 并替换新 Worker，陷入无限循环。

**复现**:
```bash
mkdir -p /tmp/empty_dir
./bin/listfiles --path=/tmp/empty_dir --yes
# 程序永不退出，持续输出 "[Monitor] Worker X heartbeat timeout. Replacing."
```

**修复建议**: Worker 完成目录扫描后，即使 `count == 0` 也应发送一个空的 `IPC_MSG_BATCH`（或新增 `IPC_MSG_DONE` 消息类型），使 Master 能正确减少 `pending_tasks`。

---

### [BUG-004] 权限不足目录导致死锁 / 无限超时替换

**严重级别**: 🔴 阻断

**描述**: 当 Worker 遇到 `opendir()` 返回 `EACCES`（权限不足）时，会发送 `IPC_MSG_ERROR`。Master 将设备标记为 `DEAD` 并注册探测任务，但**不会减少 `pending_tasks`**。由于这不是设备故障（是权限问题），探测永远不会成功，程序陷入与空目录相同的无限循环。

**复现**:
```bash
mkdir -p /tmp/no_perm/sub && touch /tmp/no_perm/sub/file.txt && chmod 000 /tmp/no_perm/sub
./bin/listfiles --path=/tmp/no_perm --yes
# 程序永不退出
```

**修复建议**:
1. 区分 `EACCES`/`EPERM` 与 `ETIMEDOUT`/`EIO`，前者不应触发设备熔断。
2. 或者，在 `main_loop_handle_error` 中针对可恢复/不可恢复错误正确减少 `pending_tasks`。

---

### [BUG-005] 断点续传无法恢复：缺少 `.idx` 文件且恢复逻辑前置退出

**严重级别**: 🔴 阻断

**描述**:
1. `finalize_progress()` 在任务完成时**从未创建或更新 `.idx` 文件**。
2. `record_path()` 中仅在分片轮转（`line_count >= progress_slice_lines`，默认 100000）时才调用 `atomic_update_index()`。对于中小规模目录，`.idx` 文件永远不会生成。
3. `restore_progress()` 在 `load_progress_index()` 失败时直接返回 0，**完全不尝试读取已存在的 `.archive` 文件**。

**后果**: 第二次运行 `--continue` 时，由于没有 `.idx`，程序直接当作全新任务处理，所有文件重复输出，断点续传完全失效。

**修复建议**:
1. 在 `finalize_progress()` 结束前调用 `atomic_update_index()` 确保 `.idx` 始终存在。
2. 即使 `.idx` 不存在，也应尝试读取 `.archive` 来恢复历史指纹（可能以保守策略跳过未确认的记录）。

---

### [BUG-006] `--archive` / `--clean` 选项语义失效

**严重级别**: 🟡 高

**描述**:
- `process_old_slice()` 和 `finalize_archive()` **无条件**将分片压缩归档到 `.archive`，完全不受 `cfg->archive` 控制。
- `cleanup_progress()` 仅在 `cfg->runone` 为 true 时被调用，导致单独使用 `--clean` 时不会清理旧进度。
- 即使 `--runone --clean` 组合使用，`finalize_progress()` 在运行结束后会重新写入 `.config`，清理不彻底。

**修复建议**: 使归档和清理逻辑真正受对应命令行标志控制，并在 `finalize_progress` 中尊重 `--clean`。

---

### [BUG-007] 独立元数据开关无效

**严重级别**: 🟡 高

**描述**: `--size`, `--user`, `--group`, `--mtime`, `--atime`, `--mode`, `--xattr`, `--inode` 等开关在 `parse_arguments` 中被正确解析并设置 `cfg->size = true` 等，但 `precompile_format()` 的默认格式（`%p|%s|%m`）**不随这些开关变化**。除非用户同时使用 `--format` 手动指定格式，否则这些开关不会产生任何输出差异。

**修复建议**: 在 `precompile_format()` 或 `init_config()` 后，根据各个元数据开关动态拼装默认格式字符串。

---

### [BUG-008] `--resume-from` 选项完全未实现

**严重级别**: 🟡 高

**描述**: `cmdline.c` 解析 `--resume-from=文件` 并将值存入 `cfg->resume_file`，但**整个代码库中没有任何地方读取或使用 `cfg->resume_file`**。

**修复建议**: 实现从指定 `.pbin` / `.archive` 文件直接恢复历史指纹的逻辑；或在实现前将该选项标记为预留/隐藏，避免误导用户。

---

### [BUG-009] 单文件目标被错误拒绝

**严重级别**: 🟡 高

**描述**: `parse_arguments()` 中使用 `stat()` 检查路径，若 `!S_ISDIR(path_stat.st_mode)` 则报错退出。但 `main.c` 中已包含单文件目标的正确处理逻辑：
```c
} else {
    /* Single file target */
    async_writer_submit(ctx.async_writer, ctx.cfg.target_path, &root_info);
    ctx.state.file_count++;
}
```

**修复建议**: 将 `parse_arguments()` 中的目录校验改为允许普通文件（或符号链接，视 `--follow-symlinks` 而定）。

---

### [BUG-010] `--output-split` 产生空分片文件

**严重级别**: 🟢 低

**描述**: 当总文件数恰好是 `--max-slice` 的整数倍时，最后会多创建一个空的切片文件。例如 50 个文件 + `--max-slice=10` 会产生 6 个文件，其中第 6 个为空。

**修复建议**: 在 `rotate_output_slice()` 中检查当前切片是否有内容写入，或在程序退出时删除最后为空的切片文件。

---

## 三、文档与实现不一致

### [BUG-011] README 中 `--format` 语法描述错误

**严重级别**: 🟡 高

**描述**: README 示例：
```bash
./bin/listfiles --path=/data --format="{path}\t{size}\t{user}\t{group}\t{mtime}"
```
实际代码中 `precompile_format()` 仅支持 `%p`, `%s`, `%u`, `%g`, `%m` 等 `%` 前缀的格式说明符，不支持 `{path}` 花括号语法。

**修复建议**: 统一文档与实际实现，或扩展解析器支持花括号语法。

---

### [BUG-012] 版本号不一致

**严重级别**: 🟢 低

**描述**:
- `include/config.h`: `#define VERSION "11.0"`
- `README.md`: "当前版本：11.0"
- `CHANGELOG.md`: "[12.0.0] - 2026-04-29"

**修复建议**: 将 `config.h` 和 `README.md` 中的版本号更新为 `12.0.0`。

---

## 四、工程与代码质量问题

### [BUG-013] 系统消息污染 stdout

**严重级别**: 🟡 高

**描述**: `main.c` 中使用 `printf()` 输出 `[System] 任务开始...` 和 `[System] 任务完成...`，这些消息会进入 stdout。当用户不使用 `-o`/`-O`（即数据输出到 stdout）时，系统消息与 CSV/文本数据混合，破坏下游管道处理。

**修复建议**: 所有诊断/进度信息统一输出到 `stderr`（`fprintf(stderr, ...)`）。

---

### [BUG-014] 致命信号处理函数不安全

**严重级别**: 🟡 高

**描述**: `handle_fatal_signal()` 对 `SIGSEGV` 注册自定义处理器，在其中执行循环遍历全局数组和 `unlink()`。`SIGSEGV` 发生时栈可能已损坏，任何非 trivial 逻辑都极不安全。

**修复建议**: 对致命信号（`SIGSEGV`, `SIGABRT`）直接调用 `_exit(128 + sig)` 或 `raise(sig)`，不做额外清理。对 `SIGINT`/`SIGTERM` 可做有限的锁文件清理。

---

### [BUG-015] Worker 子进程关闭 fd 方式原始低效

**严重级别**: 🟢 低

**描述**: `worker_pool_spawn()` 中：
```c
int max_fd = (int)sysconf(_SC_OPEN_MAX);
if (max_fd < 0) max_fd = 65536;
for (int fd = 3; fd < max_fd; fd++) {
    if (fd != in_pipe[0] && fd != out_pipe[1]) {
        close(fd);
    }
}
```
在现代 Linux 上 `_SC_OPEN_MAX` 常返回 1,048,576，每次 `fork()` 都要执行百万次 `close()` 系统调用。

**修复建议**: Linux 5.9+ 使用 `close_range()`；老内核回退到遍历 `/proc/self/fd/`。

---

### [BUG-016] Monitor 与主循环存在竞态条件

**严重级别**: 🟡 高

**描述**:
1. `monitor_check_timeouts()` 中 `kill(slot->pid, SIGKILL)` 后未从 `epoll` 移除 fd、未 `close(fd_in/fd_out)`。
2. 主循环的 "Replace dead workers" 段会再次执行 `epoll_ctl(DEL)`、`close()`、`worker_pool_replace()`，可能 double-close 或操作已失效的 fd。
3. `spbin_requeue_recovered()` 中使用未保护的 `static int next_worker = 0`，虽然当前主循环是单线程，但破坏了封装且未来扩展危险。

**修复建议**: 将 Worker 状态变更统一到一个函数中，确保 kill → waitpid → epoll_del → close → replace 的原子性；移除 `spbin_requeue_recovered` 中的静态变量，改为传入或使用线程安全计数器。

---

### [BUG-017] IPC payload 存在静默截断风险

**严重级别**: 🟡 高

**描述**: `send_batch()` 中 `total` 是 `size_t`（64-bit），但 `ipc_send()` 的 `payload_len` 是 `uint32_t`。若单个目录下文件极多导致序列化后 batch 超过 4GB，`payload_len` 会静默截断，Master 解析时可能读到损坏的数据或越界。

**修复建议**: 在 `send_batch()` 中增加上限检查（如 `total > UINT32_MAX / 2` 时拆分为多个 batch）；或在 `ipc_send` 中增加断言。

---

### [BUG-018] 进度文件序列化可移植性差

**严重级别**: 🟢 低

**描述**: `pbin`/`spbin` 格式直接使用平台相关类型（`size_t`, `time_t`, `dev_t`, `ino_t`）进行 `fwrite`/`fread`。同一台机器上 32-bit 与 64-bit 编译产物互不兼容。`time_t` 在 Y2038 问题后可能从 32-bit 变为 64-bit，进一步破坏历史进度文件。

**修复建议**: 使用固定宽度的 `uint64_t` / `int64_t` 进行显式序列化，并统一采用小端字节序（或记录 magic + version 头）。

---

### [BUG-019] MD5 哈希算法成为 CPU 热点

**严重级别**: 🟢 低

**描述**: 对每个文件/目录计算完整 MD5（`path + dev + ino`）。MD5 是为加密设计的复杂算法，此处仅用于哈希去重，完全可用更轻量的非加密哈希（如 xxHash64、CityHash、SipHash）获得数倍性能提升。

**修复建议**: 将 `fp_compute` 中的 MD5 替换为 xxHash64 等现代非加密哈希，同时保持 128-bit 指纹宽度（可组合两个 64-bit 哈希）。

---

### [BUG-020] getpwuid/getgrgid 可能在输出线程阻塞

**严重级别**: 🟢 低

**描述**: `get_username()` / `get_groupname()` 在 `async_writer_thread` 中直接调用 `getpwuid()` / `getgrgid()`。若系统配置为 LDAP/NIS/NSS 远程查询，缓存未命中时可能阻塞数秒，卡住整个输出队列。

**修复建议**: 在 Worker 进程中预解析 UID/GID，或通过独立线程池异步执行 NSS 查询。

---

### [BUG-021] `show_help()` 中格式说明符与类型不匹配

**严重级别**: 🟢 低

**描述**: `cmdline.c` 第 26 行：
```c
printf("      --estimated-files=数量 预估文件数,用于预分配内存 (默认: %lu)\n", DEFAULT_ESTIMATED_FILES);
```
`DEFAULT_ESTIMATED_FILES` 的定义是 `10000000`（`int` 类型），但格式符是 `%lu`（`long unsigned int`）。

**修复建议**: 将格式符改为 `%u`，或确保常量定义为 `unsigned long`。

---

## 五、总结

| 类别 | 数量 | 说明 |
|------|------|------|
| 阻断级 (🔴) | 5 | 编译失败、空输出、空目录/权限不足死锁、断点续传失效 |
| 高优先级 (🟡) | 10 | 选项语义错误、文档不一致、竞态条件、信号安全、消息污染 stdout |
| 低优先级 (🟢) | 6 | 性能优化、空分片、版本号、格式说明符、可移植性 |

**建议修复顺序**:
1. 先修复 **BUG-001 ~ BUG-005**（编译 + 核心功能），使工具达到可用状态。
2. 再修复 **BUG-006 ~ BUG-009**（选项语义与单文件目标），完善用户体验。
3. 最后处理工程质量问题 **BUG-013 ~ BUG-021**，提升鲁棒性和性能。
