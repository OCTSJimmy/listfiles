# listfiles 12.2.0+ 审计报告

**审计范围**: 52fd728 (v12.2.0) .. HEAD (v12.2.8)，共 15 个 commit
**审计日期**: 2026-05-13
**审计人**: Elune

---

## 一、commit 质量逐条审计

| Commit | 版本 | 标题 | 质量评级 | 核心问题 |
|--------|------|------|----------|----------|
| cd48f84 | 12.2.1 | process-model rewrite + pipe deadlock fix | ⚠️ 治标 | 扩容 1MB + backlog 只是把死锁阈值从 ~580 提升到 ~10000；backlog 满时丢弃任务并减 pending_tasks，可能丢失工作 |
| cc29bf9 | 12.2.1 | bidirectional pipe deadlock fix | ⚠️ 同上 | 非阻塞 fd_in + backlog，同 cd48f84 |
| fa8b6be | 12.2.1 | restore Monitor thread | ✅ 合格 | 从 .bak 恢复 monitor 线程，分离主循环职责，但 ProbeScheduler 锁粒度较粗 |
| 87985ee | 12.2.1 | 修复 P0-P3 共 11 项缺陷 | ⚠️ 补丁式 | Footer 崩溃、--help 返回码、--mute 未实现等，多为防御性补丁，未触及根因（如 .idx 真空带） |
| ccb4121 | 12.2.2 | worker epoll busy-loop | 🔴 严重回归 | **移除了 master 读 fd_out 的 O_NONBLOCK**。master 的 ipc_recv_header/payload 变为阻塞读，若 worker 发送不完整即死亡，主循环永久 hang 住 |
| 65671bb | 12.2.3 | worker task distribution | ✅ 合格 | 轮询分发，避免单 worker 堆积 |
| e1c5a21 | 12.2.3 | add verbose logging | ✅ 合格 | 诊断增强，无害 |
| 6c5c7d1 | 12.2.3 | 修复 Worker 超时级联死锁 | ✅ 合格 | monitor 中 kill 后保存 current_path 到 lost_tasks，避免任务丢失 |
| 510bbf0 | 12.2.3 | 修正 -M 语义 | ✅ 合格 | 监控面板改走 stdout，诊断走 stderr |
| 5ba8674 | 12.2.4 | close fd on worker timeout + intra-scan heartbeat | ⚠️ 有遗漏 | monitor kill 后 close fd_in/fd_out 但未从 epoll 移除；intra-scan heartbeat 间隔 256 entry 对超大规模目录可能仍不够 |
| 82c6638 | 12.2.5 | resume output file append | ✅ 合格 | 顺序调整，小修 |
| 937b477 | 12.2.6 | drain orphaned tasks from fd_in_rd | ✅ 合格 | 引入 ipc_drain_and_count_tasks，防止 pending_tasks 幽灵化 |
| ab1226f | 12.2.7 | add init output file log | ✅ 无害 | 仅增加一行日志 |
| 1340c37 | 12.2.8 | unify cleanup_dead_worker_slot | ✅ 较好 | 统一死亡路径清理，消除重复代码。但 cleanup 中 `atomic_fetch_sub(&pending_tasks, 1 + orphaned)` 若被多路径重复调用可能导致 pending_tasks 为负 |

---

## 二、最严重发现：v12.2.2 (ccb4121) 引入了主循环阻塞回归

**问题**: ccb4121 为修复 "worker epoll busy-loop"，**移除了 master 读 fd_out 的 O_NONBLOCK**。

```diff
-    flags = fcntl(out_pipe[0], F_GETFL);
-    fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);
```

**后果**:
- master 的 `ipc_recv_header()` 和 `ipc_recv_payload()` 对 fd_out 使用**阻塞 read**
- 若 epoll 报告 EPOLLIN 后，worker 在发送过程中死亡（被 SIGKILL / OOM / 异常退出），master 可能只读到部分 header 或部分 payload，然后**永久阻塞在 read()**
- 主循环一旦阻塞在 IPC 读取，后续所有 epoll 事件、worker 替换、任务分发全部停止
- 表现为：monitor 面板数字冻结、pbin/scan_dir.log/输出不再更新
- 这与用户描述的"卡住"症状高度吻合

**为什么 v12.2.4~v12.2.8 的后续修复没解决这个问题**:
- v12.2.4 加了 monitor kill 后 close fd，但主循环阻塞时 monitor 的 kill 无法让主循环恢复
- v12.2.8 统一了 cleanup，但 cleanup 在主循环中被调用，主循环阻塞后 cleanup 不会执行

---

## 三、卡住问题（大规模单层目录）根因分析

### 症状
- monitor 面板数字不更新
- pbin / scan_dir.log / 输出文件 / idx 均无写入
- verbose 中**无** work 超时 kill 日志

### 最可能根因排序

#### 根因 A：主线程 lstat(target_path) 进入 D-State（概率最高）

main.c 中 monitor 线程已启动，之后才执行：
```c
lstat(ctx.cfg.target_path, &root_info);
```

如果 target_path 是 **NFS hard 挂载** 的大规模目录，lstat 可能进入不可中断睡眠（D-State）。此时：
- 主线程 hang 在 lstat，永远不进入 main_loop_run
- worker 已 fork，阻塞在 ipc_recv_header（等待根任务）
- monitor 每 1s 检查心跳，发现超时，输出 kill 日志到 stderr
- 但 print_progress 会显示 "Pending tasks: 1" 且不变
- pbin / scan_dir.log / 输出均无更新（因为 main_loop_run 没运行）

**为什么用户没看到 kill 日志**: stderr 可能被重定向，或用户观察时间 < 30s（HEARTBEAT_TIMEOUT_SEC）。

#### 根因 B：v12.2.2 O_NONBLOCK 移除导致主循环阻塞（概率中高）

如第二节所述。触发条件：worker 在发送 IPC 消息过程中死亡，master 阻塞在 read。

**为什么没有 kill 日志**：主循环阻塞后，monitor 线程仍然独立运行，会输出 kill 日志。但如果 stderr 被 mute 或重定向，用户可能看不到。

#### 根因 C：初始根任务 ipc_send 未检查返回值（概率中）

main.c 中：
```c
ipc_send(slot->fd_in, IPC_MSG_SCAN, ctx.cfg.target_path, plen);
```

**未检查返回值**。如果 worker_pool_spawn 失败（fork 失败、pipe 创建失败），fd_in 可能是 -1 或未初始化。ipc_send 返回 -1，但 main.c 不处理。pending_tasks = 1，但 worker 没收到任务。主循环 epoll 永远不会收到该 worker 的消息，termination 条件永远不满足，程序永久 hang。

**为什么没有 kill 日志**：worker 阻塞在 ipc_recv_header，心跳不更新，monitor 会 kill。但新 worker 替换后如果 fork 仍然失败，会重复 kill。

---

## 四、调试点建议

为定位具体根因，建议在以下位置增加日志（写 stderr，不受 mute 影响）：

### 1. main.c - lstat 前后
```c
fprintf(stderr, "[Main] lstat target_path=%s ...\n", ctx.cfg.target_path);
if (lstat(ctx.cfg.target_path, &root_info) == 0) {
    fprintf(stderr, "[Main] lstat OK, mode=0%o\n", root_info.st_mode);
    ...
}
```

### 2. main.c - 根任务 ipc_send 后
```c
int rc = ipc_send(slot->fd_in, IPC_MSG_SCAN, ctx.cfg.target_path, plen);
fprintf(stderr, "[Main] root task sent to worker 0: rc=%d, fd_in=%d\n", rc, slot->fd_in);
```

### 3. worker_proc.c - worker_main 入口
```c
fprintf(stderr, "[Worker %d] started, fd_in=%d fd_out=%d\n", worker_id, fd_in, fd_out);
```

### 4. worker_proc.c - scan_and_send 入口和出口
```c
fprintf(stderr, "[Worker] scan_and_send begin: %s\n", dir_path);
... // scan
fprintf(stderr, "[Worker] scan_and_send end: %s, count=%d\n", dir_path, count);
```

### 5. main_loop.c - epoll_wait 前后
```c
fprintf(stderr, "[MainLoop] epoll_wait begin...\n");
int nfds = epoll_wait(ctx->epfd, events, 64, 500);
fprintf(stderr, "[MainLoop] epoll_wait return: nfds=%d\n", nfds);
```

### 6. main_loop.c - ipc_recv_header 前后
```c
fprintf(stderr, "[MainLoop] recv_header from worker %u, fd=%d...\n", slot_id, fd);
int ret = ipc_recv_header(fd, &hdr);
fprintf(stderr, "[MainLoop] recv_header ret=%d\n", ret);
```

### 7. monitor.c - check_workers_health 每次遍历
```c
for (...) {
    if (!slot->is_alive) continue;
    fprintf(stderr, "[Monitor] Worker %d: alive, last_hb=%ld, now=%ld, diff=%ld\n",
            i, slot->last_heartbeat, now, now - slot->last_heartbeat);
    if (now - slot->last_heartbeat > timeout) {
        fprintf(stderr, "[Monitor] Worker %d TIMEOUT, killing pid=%d\n", i, slot->pid);
        ...
    }
}
```

### 8. worker_proc.c - ipc_send 返回值检查
```c
// 在 ipc_send 中添加 verbose 日志
if (rc < 0) {
    fprintf(stderr, "[IPC] send failed: fd=%d, errno=%d (%s)\n", fd, errno, strerror(errno));
}
```

---

## 五、修复建议优先级

1. **P0 - 恢复 master fd_out 的 O_NONBLOCK**：在 worker_pool_spawn 中重新设置 `fcntl(out_pipe[0], O_NONBLOCK)`，防止主循环阻塞
2. **P0 - main.c 中 ipc_send 检查返回值**：根任务发送失败时应打印致命错误并退出
3. **P1 - 增加上述调试点**：至少增加 main.c lstat 前后、worker_main 入口、epoll_wait 前后的日志
4. **P1 - monitor 的 fd close 与 epoll 移除同步**：kill worker 后应从 epoll 移除 fd_out（v12.2.4 只 close 了 fd，没 epoll_del）
5. **P2 - cleanup_dead_worker_slot 防重复调用**：增加一个 `slot->cleaned_up` 标志，避免 monitor + epoll + exit 三路径重复减 pending_tasks

---

## 六、结论

12.2.0+ 的更新中，v12.2.1 的 pipe deadlock 修复和 v12.2.8 的 cleanup 统一是有效改进，但 **v12.2.2 移除了 master fd_out 的 O_NONBLOCK，引入了一个严重的、可能导致主循环永久阻塞的回归**。该回归在后续 6 个 commit 中未被修复。

用户遇到的"大规模单层目录卡住且无更新"现象，**最可能是主线程 lstat 进入 D-State（NFS hard 挂载）或 v12.2.2 的 O_NONBLOCK 回归触发主循环阻塞**。

建议立即恢复 O_NONBLOCK，并在主线程关键路径增加调试点。
