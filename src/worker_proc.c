#define _GNU_SOURCE
#include "worker_proc.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>

/* Read-only context inherited via fork (COW, never modified by parent after fork) */
static const Config *g_worker_cfg = NULL;
static const FingerprintSet *g_worker_ref_set = NULL;
static const ReferenceMap *g_worker_ref_map = NULL;

void worker_set_context(const Config *cfg, const FingerprintSet *ref_set, const ReferenceMap *ref_map) {
    g_worker_cfg = cfg;
    g_worker_ref_set = ref_set;
    g_worker_ref_map = ref_map;
}

/* ================================================================
 * IPC helpers
 * ================================================================ */
int ipc_send(int fd, uint32_t msg_type, const void *payload, uint32_t payload_len) {
    IpcMessageHeader hdr = { msg_type, payload_len };
    size_t written = 0;
    while (written < sizeof(hdr)) {
        ssize_t n = write(fd, (const char*)&hdr + written, sizeof(hdr) - written);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        written += n;
    }
    if (payload_len > 0 && payload) {
        written = 0;
        while (written < payload_len) {
            ssize_t n = write(fd, (const char*)payload + written, payload_len - written);
            if (n < 0) { if (errno == EINTR) continue; return -1; }
            written += n;
        }
    }
    return 0;
}

int ipc_recv_header(int fd, IpcMessageHeader *hdr) {
    size_t nread = 0;
    while (nread < sizeof(*hdr)) {
        ssize_t n = read(fd, (char*)hdr + nread, sizeof(*hdr) - nread);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1; /* EOF */
        nread += n;
    }
    return 0;
}

int ipc_recv_payload(int fd, void *buf, uint32_t len) {
    if (len == 0) return 0;
    size_t nread = 0;
    while (nread < len) {
        ssize_t n = read(fd, (char*)buf + nread, len - nread);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        nread += n;
    }
    return 0;
}

/* ================================================================
 * Worker-side scan logic
 * ================================================================ */

static mode_t dt_to_mode(unsigned char d_type) {
    switch (d_type) {
        case DT_REG:  return S_IFREG;
        case DT_DIR:  return S_IFDIR;
        case DT_LNK:  return S_IFLNK;
        case DT_CHR:  return S_IFCHR;
        case DT_BLK:  return S_IFBLK;
        case DT_FIFO: return S_IFIFO;
        case DT_SOCK: return S_IFSOCK;
        default:      return 0;
    }
}

static bool try_blind_trust(const char *full_path, uint64_t dir_dev, uint64_t d_ino,
                            unsigned char d_type, struct stat *out_st) {
    if (!g_worker_ref_set || !g_worker_ref_map) return false;
    if (d_type == DT_UNKNOWN || d_ino == 0) return false;

    uint8_t fp[FP_SIZE];
    fp_compute(full_path, dir_dev, d_ino, fp);

    if (!fp_set_contains(g_worker_ref_set, fp)) return false;

    const ReferenceEntry *ref = ref_map_lookup(g_worker_ref_map, fp);
    if (!ref || ref->d_type != d_type) return false;

    time_t now = time(NULL);
    if (g_worker_cfg->skip_interval <= 0) return false;
    if (now - ref->mtime <= g_worker_cfg->skip_interval) return false;

    memset(out_st, 0, sizeof(*out_st));
    out_st->st_dev   = dir_dev;
    out_st->st_ino   = d_ino;
    out_st->st_mtime = ref->mtime;
    out_st->st_mode  = dt_to_mode(d_type);
    return true;
}

static void send_batch(int fd_out, char **paths, struct stat *stats, int count) {
    /* Always send a batch (even count==0) so Master can decrement pending_tasks */

    /* Calculate total payload size */
    size_t total = sizeof(IpcBatchHeader);
    for (int i = 0; i < count; i++) {
        total += sizeof(uint32_t);
        total += strlen(paths[i]);
        total += sizeof(struct stat);
    }

    if (total > UINT32_MAX) {
        fprintf(stderr, "[Worker] Batch payload too large (%zu), aborting.\n", total);
        return;
    }

    uint8_t *buf = malloc(total);
    if (!buf) {
        /* 内存不足时发送空 batch，确保 Master 能正确递减 pending_tasks */
        send_batch(fd_out, NULL, NULL, 0);
        return;
    }

    uint8_t *p = buf;
    IpcBatchHeader bh = { (uint32_t)count };
    memcpy(p, &bh, sizeof(bh)); p += sizeof(bh);

    for (int i = 0; i < count; i++) {
        uint32_t plen = (uint32_t)strlen(paths[i]);
        memcpy(p, &plen, sizeof(plen)); p += sizeof(plen);
        memcpy(p, paths[i], plen);      p += plen;
        memcpy(p, &stats[i], sizeof(struct stat)); p += sizeof(struct stat);
    }

    ipc_send(fd_out, IPC_MSG_BATCH, buf, (uint32_t)total);
    free(buf);
}

/* Helper: send ERROR for device-level failures, then an empty batch */
static void send_error_and_empty_batch(int fd_out, int err_code, const char *path) {
    if (err_code == ETIMEDOUT || err_code == EIO) {
        IpcErrorHeader eh = { (uint32_t)err_code, 0 };
        uint32_t plen = (uint32_t)strlen(path);
        uint8_t *buf = malloc(sizeof(eh) + sizeof(plen) + plen);
        if (buf) {
            memcpy(buf, &eh, sizeof(eh));
            memcpy(buf + sizeof(eh), &plen, sizeof(plen));
            memcpy(buf + sizeof(eh) + sizeof(plen), path, plen);
            ipc_send(fd_out, IPC_MSG_ERROR, buf, (uint32_t)(sizeof(eh) + sizeof(plen) + plen));
            free(buf);
        }
    }
    send_batch(fd_out, NULL, NULL, 0);
}

static void scan_and_send(int fd_out, const char *dir_path, int worker_id) {
    (void)worker_id;
    struct stat dir_st;
    if (lstat(dir_path, &dir_st) != 0) {
        send_error_and_empty_batch(fd_out, errno, dir_path);
        return;
    }

    uint64_t dir_dev = dir_st.st_dev;
    int batch_size = 1024;
    if (g_worker_cfg && g_worker_cfg->batch_size > 0)
        batch_size = g_worker_cfg->batch_size;

    char **paths = calloc(batch_size, sizeof(char*));
    struct stat *stats = calloc(batch_size, sizeof(struct stat));
    int count = 0;

    DIR *dir = opendir(dir_path);
    if (!dir) {
        send_error_and_empty_batch(fd_out, errno, dir_path);
        goto cleanup;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        char full_path[4096];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (n >= (int)sizeof(full_path)) continue;

        struct stat st;
        bool got = false;

        if (try_blind_trust(full_path, dir_dev, entry->d_ino, entry->d_type, &st)) {
            got = true;
        } else {
            if (g_worker_cfg && g_worker_cfg->follow_symlinks) {
                if (stat(full_path, &st) != 0) continue;
            } else {
                if (lstat(full_path, &st) != 0) continue;
            }
            got = true;
        }

        if (got) {
            paths[count] = strdup(full_path);
            stats[count] = st;
            count++;
        }

        if (count >= batch_size) {
            send_batch(fd_out, paths, stats, count);
            for (int i = 0; i < count; i++) free(paths[i]);
            count = 0;
        }
    }

    if (count > 0) {
        send_batch(fd_out, paths, stats, count);
        for (int i = 0; i < count; i++) free(paths[i]);
    } else {
        /* Empty directory: send empty batch so Master decrements pending_tasks */
        send_batch(fd_out, NULL, NULL, 0);
    }

    closedir(dir);
cleanup:
    free(paths);
    free(stats);
}

/* ================================================================
 * Worker process entry
 * ================================================================ */
void worker_main(int fd_in, int fd_out, int worker_id) {
    (void)worker_id;
    while (1) {
        IpcMessageHeader hdr;
        if (ipc_recv_header(fd_in, &hdr) != 0) break;

        if (hdr.msg_type == IPC_MSG_STOP) {
            ipc_send(fd_out, IPC_MSG_EXIT, NULL, 0);
            break;
        }

        if (hdr.msg_type != IPC_MSG_SCAN) {
            /* drain unknown payload */
            if (hdr.payload_len > 0) {
                void *tmp = malloc(hdr.payload_len);
                ipc_recv_payload(fd_in, tmp, hdr.payload_len);
                free(tmp);
            }
            continue;
        }

        char *dir_path = malloc(hdr.payload_len + 1);
        if (!dir_path) break;
        if (ipc_recv_payload(fd_in, dir_path, hdr.payload_len) != 0) {
            free(dir_path);
            break;
        }
        dir_path[hdr.payload_len] = '\0';

        /* heartbeat before scan */
        IpcHeartbeatPayload hb = { (uint64_t)time(NULL) };
        ipc_send(fd_out, IPC_MSG_HEARTBEAT, &hb, sizeof(hb));

        scan_and_send(fd_out, dir_path, worker_id);

        /* heartbeat after scan */
        hb.timestamp = (uint64_t)time(NULL);
        ipc_send(fd_out, IPC_MSG_HEARTBEAT, &hb, sizeof(hb));

        free(dir_path);
    }
}

/* ================================================================
 * Master-side worker pool management
 * ================================================================ */
WorkerPool* worker_pool_create(int num_workers) {
    WorkerPool *pool = calloc(1, sizeof(WorkerPool));
    if (!pool) return NULL;
    pool->slots = calloc(num_workers, sizeof(WorkerSlot));
    if (!pool->slots) { free(pool); return NULL; }
    pool->num_workers = num_workers;
    pool->active_count = 0;
    return pool;
}

void worker_pool_destroy(WorkerPool *pool) {
    if (!pool) return;
    for (int i = 0; i < pool->num_workers; i++) {
        if (pool->slots[i].is_alive) {
            kill(pool->slots[i].pid, SIGKILL);
            close(pool->slots[i].fd_in);
            close(pool->slots[i].fd_out);
        }
    }
    /* Non-blocking reap of any zombie children */
    for (int i = 0; i < pool->num_workers * 3; i++) {
        if (waitpid(-1, NULL, WNOHANG) <= 0) break;
    }
    free(pool->slots);
    free(pool);
}

bool worker_pool_spawn(WorkerPool *pool, int slot_id) {
    int in_pipe[2], out_pipe[2];
    if (pipe2(in_pipe, O_CLOEXEC) != 0) return false;
    if (pipe2(out_pipe, O_CLOEXEC) != 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return false;
    }

    if (pid == 0) {
        /* Child */
        close(in_pipe[1]);
        close(out_pipe[0]);

        /* Close all inherited fds except our pipes */
        int max_fd = (int)sysconf(_SC_OPEN_MAX);
        if (max_fd < 0) max_fd = 65536;
        for (int fd = 3; fd < max_fd; fd++) {
            if (fd != in_pipe[0] && fd != out_pipe[1]) {
                close(fd);
            }
        }

        worker_main(in_pipe[0], out_pipe[1], slot_id);
        _exit(0);
    }

    /* Parent */
    close(in_pipe[0]);
    close(out_pipe[1]);

    WorkerSlot *slot = &pool->slots[slot_id];
    slot->pid = pid;
    slot->fd_in = in_pipe[1];
    slot->fd_out = out_pipe[0];
    slot->is_alive = true;
    slot->last_heartbeat = time(NULL);
    slot->current_dev = 0;
    slot->current_path[0] = '\0';
    pool->active_count++;
    return true;
}

bool worker_pool_replace(WorkerPool *pool, int slot_id) {
    WorkerSlot *slot = &pool->slots[slot_id];
    if (slot->is_alive) {
        kill(slot->pid, SIGKILL);
        /* Do NOT block on waitpid: process may be stuck in D-state.
         * Zombie will be reaped later by main loop's periodic waitpid(-1, WNOHANG). */
        close(slot->fd_in);
        close(slot->fd_out);
        slot->is_alive = false;
        pool->active_count--;
    }
    return worker_pool_spawn(pool, slot_id);
}

void worker_pool_stop_all(WorkerPool *pool) {
    for (int i = 0; i < pool->num_workers; i++) {
        if (pool->slots[i].is_alive) {
            ipc_send(pool->slots[i].fd_in, IPC_MSG_STOP, NULL, 0);
        }
    }
}
