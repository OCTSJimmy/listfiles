# fixed v15.4.3 — thread_pool completed 链表安全

日期：2026-05-18

## 问题

1. `worker_thread()` 中 `node` malloc 失败时泄漏 batch，`pending_batches` 不归零。
2. `poll_completed()` 无链表完整性校验，自循环时主线程永久空转。
3. `destroy()` 无 drain 上限，链表循环时销毁无限阻塞。

## 修复

1. `node` malloc 失败时释放 batch 全部内存（`paths[i]`、`paths`、`stats`、`results`、`batch`）并 `continue`。
2. `poll_completed()`: 自循环检测（`node->next == node` 时断开）+ 遍历上限 100000。
3. `destroy()`: drain 安全上限 `drained_count < 100000`。

## 修改文件

- `src/scan/thread_pool.c`
- `include/core/config.h`

## 编译

`make clean && make` 零错误零警告。
