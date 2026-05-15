# v13.0.0 IPC Thread Isolation Architecture

## Date
2026-05-15

## Problem
v12.x single-threaded Master epoll event loop: one Worker's fd failure (heartbeat timeout, D-State hang, fd reuse race) pollutes the entire loop, causing Monitor freeze, CPU spin, and all Workers pseudo-dead.

## Architecture Changes
- 8 permanent IPC threads, one per Worker slot
- Each IPC thread: independent non-blocking epoll + heartbeat detection + SIGKILL
- Master thread: pure message bus, no fd/epoll/read/write
- Message queues: eventfd + lock-free ring buffer (64-bit atomic CAS head/tail)
- Fault isolation: one Worker's fd problem only affects its own IPC thread

## Files Added
- include/msg_format.h
- include/msg_queue.h
- src/msg_queue.c
- include/ipc_thread.h
- src/ipc_thread.c

## Files Modified
- include/app_context.h (IPC thread fields)
- include/config.h (VERSION "13.0.0")
- include/main_loop.h (expose IPC lifecycle API)
- src/main_loop.c (rewritten as message bus)
- src/main.c (init IPC threads, root task via cmd_queue)
- src/monitor.c (removed Worker heartbeat check)
- Makefile (auto-includes new files)

## Verification
- Compiled with gcc -Wall -Wextra -std=gnu11, zero errors, zero warnings
- Binary: bin/listfiles
