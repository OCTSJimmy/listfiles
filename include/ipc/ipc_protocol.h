#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

#include <stdint.h>
#include <sys/stat.h>

#define IPC_MSG_SCAN       1
#define IPC_MSG_BATCH      2
#define IPC_MSG_HEARTBEAT  3
#define IPC_MSG_ERROR      4
#define IPC_MSG_EXIT       5
#define IPC_MSG_STOP       6
#define IPC_MSG_DEV_TIMEOUT 7  /* Scanner self-detected timeout */
#define IPC_MSG_READY       8  /* Worker initialization complete */
#define IPC_MSG_FINISH      9  /* Scanner task complete */

typedef struct __attribute__((packed)) {
    uint32_t msg_type;
    uint32_t payload_len;
} IpcMessageHeader;

/* MSG_BATCH payload header, followed by count records:
 *   [uint32_t path_len][char path[path_len]][struct stat st]
 */
typedef struct __attribute__((packed)) {
    uint32_t count;
} IpcBatchHeader;

/* MSG_ERROR payload header, followed by path string */
typedef struct __attribute__((packed)) {
    uint32_t errno_code;
    uint64_t dev;
} IpcErrorHeader;

/* MSG_HEARTBEAT payload */
typedef struct __attribute__((packed)) {
    uint64_t timestamp;
} IpcHeartbeatPayload;

/* MSG_FINISH payload */
typedef struct __attribute__((packed)) {
    uint32_t status;       /* 0=OK, 1=ERROR, 2=TIMEOUT, 3=EMPTY */
    uint32_t path_len;
    /* char path[path_len] follows */
} IpcFinishPayload;

/* IPC 协议函数 */
int ipc_send(int fd, uint32_t msg_type, const void *payload, uint32_t payload_len);
int ipc_recv_header(int fd, IpcMessageHeader *hdr);
int ipc_recv_payload(int fd, void *buf, uint32_t len);
int ipc_drain_and_count_tasks(int fd_in);

#endif
