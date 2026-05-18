#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
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

#define IPC_FOOTER_MAGIC  0xDEADBEEF66AAC0FFULL  /* v15.4.0: BATCH payload footer */

typedef struct __attribute__((packed)) {
    uint32_t msg_type;
    uint32_t payload_len;
} IpcMessageHeader;

/* IPC read FSM states — v15.4.0 cross-epoll resumable reads */
typedef enum {
    IPC_READ_IDLE = 0,
    IPC_READ_HDR,
    IPC_READ_PAYLOAD,
    IPC_READ_FOOTER     /* fd_data (BATCH) only */
} IpcReadState;

typedef struct {
    IpcReadState state;
    IpcMessageHeader hdr;
    void *buf;          /* payload buffer */
    size_t nread;       /* bytes already read in current state */
} IpcReadFsm;

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

/* v15.4.0: FSM-based resumable IPC receive */
int fsm_recv(int fd, void *dst, size_t total, size_t *nread);
int safe_ipc_recv_header_fsm(int fd, IpcReadFsm *fsm);
int safe_ipc_recv_payload_fsm(int fd, IpcReadFsm *fsm);
int safe_ipc_recv_footer_fsm(int fd, IpcReadFsm *fsm, uint64_t *out_magic);
bool ipc_msg_type_valid(uint32_t msg_type);

#endif
