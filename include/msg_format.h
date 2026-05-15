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
#define MSG_DROP       15  /* CMD_SCAN dropped during replacement window */

/**
 * @brief  Unified message structure for Master <-> IPC Thread queues
 *
 * All messages are fixed-size (pointer-based) for lock-free queue compatibility.
 * The `data` pointer is malloc'd by sender and free'd by receiver.
 */
typedef struct {
    uint32_t type;      /* CMD_* or RET_* */
    int      slot_id;   /* Worker slot index [0, num_workers-1] */
    void    *data;      /* Type-specific payload (malloc'd) */
    size_t   data_len;  /* Payload length in bytes */
} IpcThreadMsg;

/* ================================================================
 * Payload structures (sent via IpcThreadMsg.data)
 * ================================================================ */

/* CMD_SCAN payload */
typedef struct {
    char     path[4096];
    uint32_t path_len;
    uint64_t dev;       /* device id for tracking */
} CmdScanPayload;

/* CMD_REPLACE payload */
typedef struct {
    int    fd_in;       /* new Worker write end (master writes) */
    int    fd_out;      /* new Worker read end (master reads) */
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

/* MSG_DROP payload */
typedef struct {
    char path[4096];
} DropPayload;

#endif
