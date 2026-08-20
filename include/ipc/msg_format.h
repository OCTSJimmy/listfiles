#ifndef MSG_FORMAT_H
#define MSG_FORMAT_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>

/* ================================================================
 * v13.0.0 IPC Thread Isolation Architecture
 * Message format for Master Thread <-> IPC Thread communication
 * ================================================================ */

/* Command types: Master Thread -> IPC Thread */
#define CMD_SCAN       1   /* Send SCAN task to Worker */
#define CMD_REPLACE    2   /* Replace Worker with new fd/pid */
#define CMD_STOP       3   /* Stop IPC thread */

/* Return types: IPC Thread -> Master Thread */
#define RET_BATCH      10  /* Worker returned BATCH results */
#define RET_HEARTBEAT  11  /* Worker heartbeat */
#define RET_ERROR      12  /* Worker error (device-level) */
#define RET_DEAD       13  /* Worker died (timeout/epoll error) */
#define RET_EXIT       14  /* Worker normal exit */
#define RET_DEV_TIMEOUT 16  /* Worker scanner self-detected timeout */
#define RET_READY      17  /* Worker initialization complete */
#define RET_FINISH     18  /* Worker task complete */
#define RET_ENTRY_ERROR 19 /* v15.5.7: entry-level error (stat failed / path truncated; no device penalty) */

/**
 * @brief  Unified message structure for Master <-> IPC Thread queues
 *
 * All messages are fixed-size (pointer-based) for ring-buffer queue storage.
 * The `data` pointer is malloc'd by sender and free'd by receiver.
 * v15.6.0: `epoch` 由 RET_BATCH/RET_FINISH 携带（其余消息为 0），
 * Master 校验 epoch 匹配以丢弃旧 Worker 残留数据。
 */
typedef struct {
    uint32_t type;      /* CMD_* or RET_* */
    int      slot_id;   /* Worker slot index [0, num_workers-1] */
    void    *data;      /* Type-specific payload (malloc'd) */
    size_t   data_len;  /* Payload length in bytes */
    uint64_t epoch;     /* v15.6.0: task epoch (RET_BATCH/RET_FINISH only) */
} IpcThreadMsg;

/* ================================================================
 * Payload structures (sent via IpcThreadMsg.data)
 * ================================================================ */

/* CMD_SCAN payload */
typedef struct {
    char     path[4096];
    uint32_t path_len;
    uint64_t dev;       /* device id for tracking */
    uint64_t epoch;     /* v15.6.0: task epoch, forwarded to Worker */
} CmdScanPayload;

/* CMD_REPLACE payload */
typedef struct {
    int    fd_cmd;      /* new Worker cmd read end (master writes) */
    int    fd_data;     /* new Worker data read end (master reads BATCH) */
    int    fd_ctrl;     /* new Worker ctrl read end (master reads signals) */
    pid_t  pid;         /* new Worker process id */
} CmdReplacePayload;

/* RET_BATCH payload: raw IPC batch data (same as current IPC_MSG_BATCH payload) */
/* Reuses existing IpcBatchHeader + records format */

/* RET_HEARTBEAT payload */
typedef struct {
    uint64_t timestamp;
} RetHeartbeatPayload;

/* RET_ERROR payload */
typedef struct {
    uint32_t errno_code;
    uint64_t dev;
    char     path[4096];
} RetErrorPayload;

/* RET_DEAD / RET_EXIT: no payload needed (data = NULL) */

#endif
