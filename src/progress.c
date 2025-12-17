#include "progress.h"
#include "utils.h"
#include "looper.h"    // 引入 Looper
#include "traversal.h" // 引入 MSG_CHECK_BATCH
#include "idempotency.h" // 引入 idempotency 相关
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <zlib.h>

// ==========================================
// 1. 基础辅助函数
// ==========================================

char *get_index_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s.idx", base);
    return name;
}

char *get_slice_filename(const char *base, unsigned long index) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s_%06lu.pbin", base, index);
    return name;
}

char *get_archive_filename(const char *base, unsigned long index) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s_%06lu.pbin.gz", base, index);
    return name;
}


// ==========================================
// 2. 锁机制 (必须保留)
// ==========================================

// ==========================================
// 2. 锁机制
// ==========================================

int acquire_lock(const Config *cfg, RuntimeState *state) {
    if (!cfg->continue_mode) return 0;
    char *lock_path = safe_malloc(strlen(cfg->progress_base) + 32);
    sprintf(lock_path, "%s.lock", cfg->progress_base);
    state->lock_file_path = lock_path;
    int fd = open(lock_path, O_RDWR | O_CREAT, 0666);
    if (fd == -1) { perror("无法创建锁文件"); return -1; }
    if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
        if (errno == EWOULDBLOCK) fprintf(stderr, "错误: 另一个实例正在运行\n");
        else perror("无法获取文件锁");
        close(fd); return -1;
    }
    state->lock_fd = fd;
    return 0;
}

void release_lock(RuntimeState *state) {
    if (state->lock_fd != -1) {
        flock(state->lock_fd, LOCK_UN);
        close(state->lock_fd);
        state->lock_fd = -1;
    }
    if (state->lock_file_path) {
        unlink(state->lock_file_path);
        free(state->lock_file_path);
        state->lock_file_path = NULL;
    }
}
// ==========================================
// 3. 配置保存
// ==========================================

void save_config_to_disk(const Config *cfg) {
    if (!cfg->continue_mode) return;
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
    FILE *fp = fopen(config_path, "w");
    if (!fp) return;
    fprintf(fp, "path=%s\n", cfg->target_path);
    fprintf(fp, "follow_symlinks=%d\n", cfg->follow_symlinks);
    fclose(fp);
}

// ==========================================
// 4. 进度记录 (核心生产者 - 必须保留)
// ==========================================
static void compress_slice_file(const char *src_path, const char *dst_path) {
    FILE *in = fopen(src_path, "rb");
    if (!in) return;
    gzFile out = gzopen(dst_path, "wb");
    if (!out) { fclose(in); return; }
    char buf[16384];
    size_t len;
    while ((len = fread(buf, 1, sizeof(buf), in)) > 0) {
        gzwrite(out, buf, (unsigned)len);
    }
    gzclose(out);
    fclose(in);
}

static void process_old_slice(const Config *cfg, unsigned long index) {
    char *src_path = get_slice_filename(cfg->progress_base, index);
    if (cfg->archive) {
        char *dst_path = get_archive_filename(cfg->progress_base, index);
        compress_slice_file(src_path, dst_path);
        free(dst_path);
        unlink(src_path);
    } else if (cfg->clean) {
        unlink(src_path);
    }
    free(src_path);
}

void atomic_update_index(const Config *cfg, RuntimeState *state) {
    char *idx_file = get_index_filename(cfg->progress_base);
    char *tmp_file = safe_malloc(strlen(idx_file) + 64);
    snprintf(tmp_file, strlen(idx_file) + 64, "%s.tmp.%lu", idx_file, (unsigned long)pthread_self());
    FILE *tmp_fp = fopen(tmp_file, "w");
    if (tmp_fp) {
        fprintf(tmp_fp, "%lu %lu %lu %lu %lu\n", 
                state->process_slice_index, 
                state->processed_count,
                state->write_slice_index,
                state->output_slice_num,
                state->output_line_count);
        fclose(tmp_fp);
        if (rename(tmp_file, idx_file) != 0) unlink(tmp_file);
    }
    free(idx_file);
    free(tmp_file);
}

void rotate_progress_slice(const Config *cfg, RuntimeState *state) {
    if (state->write_slice_file) {
        fclose(state->write_slice_file);
        state->write_slice_file = NULL;
        process_old_slice(cfg, state->write_slice_index);
    }
    state->write_slice_index++;
    state->line_count = 0; 
    char *path = get_slice_filename(cfg->progress_base, state->write_slice_index);
    state->write_slice_file = fopen(path, "wb");
    if (!state->write_slice_file) perror("无法创建新的进度分片文件");
    free(path);
    atomic_update_index(cfg, state);
}

void record_path(const Config *cfg, RuntimeState *state, const char *path, const struct stat *info) {
    if (!state->write_slice_file) {
        char *p = get_slice_filename(cfg->progress_base, state->write_slice_index);
        state->write_slice_file = fopen(p, "wb"); 
        free(p);
    }
    if (!state->write_slice_file) return;

    size_t path_len = strlen(path);
    dev_t dev = info ? info->st_dev : 0;
    ino_t ino = info ? info->st_ino : 0;

    fwrite(&path_len, sizeof(size_t), 1, state->write_slice_file);
    fwrite(path, 1, path_len, state->write_slice_file);
    fwrite(&dev, sizeof(dev_t), 1, state->write_slice_file);
    fwrite(&ino, sizeof(ino_t), 1, state->write_slice_file);

    state->line_count++; 
    state->processed_count++; 
    if (state->line_count >= cfg->progress_slice_lines) {
        rotate_progress_slice(cfg, state);
    }
}

// ==========================================
// 5. 进度恢复 (核心修复：单遍流式恢复)
// ==========================================

bool load_progress_index(const Config *cfg, RuntimeState *state) {
    char *idx_file = get_index_filename(cfg->progress_base);
    FILE *fp = fopen(idx_file, "r");
    if (!fp) { free(idx_file); return false; }
    int matches = fscanf(fp, "%lu %lu %lu %lu %lu", 
            &state->process_slice_index, &state->processed_count,
            &state->write_slice_index, &state->output_slice_num, &state->output_line_count);
    fclose(fp); free(idx_file);
    return matches == 5;
}

int restore_progress(const Config *cfg, MessageQueue *worker_mq, RuntimeState *state) {
    int batch_count = 0;
    unsigned long global_index = 0; // 全局计数器：追踪当前是第几个条目

    verbose_printf(cfg, 1, "开始恢复任务队列 (Global Processed: %lu)...\n", state->processed_count);

    // 从第 0 个分片开始，一直读到文件不存在为止
    // 这样可以确保我们构建出完整的去重黑名单
    for (unsigned long s_idx = 0; ; ++s_idx) {
        char *slice_path = get_slice_filename(cfg->progress_base, s_idx);
        FILE *slice_fp = fopen(slice_path, "rb");
        if (!slice_fp) {
            free(slice_path);
            break; // 读完了所有历史记录
        }

        size_t path_len;
        dev_t dev;
        ino_t ino;
        char path_buf[MAX_PATH_LENGTH];
        unsigned long loaded_in_slice = 0;
        
        TaskBatch *batch = batch_create();

        while (fread(&path_len, sizeof(size_t), 1, slice_fp) == 1) {
            // 路径过长保护
            if (path_len >= sizeof(path_buf)) {
                fseek(slice_fp, path_len, SEEK_CUR); // 跳过 path
                fread(&dev, sizeof(dev_t), 1, slice_fp);
                fread(&ino, sizeof(ino_t), 1, slice_fp);
                global_index++;
                continue;
            }

            // 读取路径
            if (fread(path_buf, 1, path_len, slice_fp) != path_len) break;
            path_buf[path_len] = '\0';
            
            // 读取元数据
            fread(&dev, sizeof(dev_t), 1, slice_fp);
            fread(&ino, sizeof(ino_t), 1, slice_fp);

            // === 核心修复逻辑 ===
            // 使用 global_index 与 state->processed_count 比较
            // 任何 global_index < processed_count 的记录，都是上次确认处理完的 -> 进黑名单
            // 任何 global_index >= processed_count 的记录，都是上次没处理完的 -> 重新入队
            
            if (global_index < state->processed_count) {
                // 已处理 -> 加黑名单
                if (g_history_object_set) {
                    ObjectIdentifier id = { .st_dev = dev, .st_ino = ino };
                    hash_set_insert(g_history_object_set, &id);
                }
            } else {
                // 未处理 (Tail) -> 重新入队
                // 注意：这里不需要 stat，因为是 CHECK_BATCH 任务
                batch_add(batch, path_buf, NULL);
                loaded_in_slice++;

                if (batch->count >= BATCH_SIZE) {
                    // 需要外部提供计数接口，或者直接操作 atomic (如果可见)
                    // 这里假设 traversal.h 提供了 traversal_add_pending_tasks
                    traversal_add_pending_tasks(1);
                    mq_send(worker_mq, MSG_CHECK_BATCH, batch);
                    batch = batch_create();
                    batch_count++;
                }
            }
            global_index++;
        }
        
        // 发送剩余批次
        if (batch->count > 0) {
            traversal_add_pending_tasks(1);
            mq_send(worker_mq, MSG_CHECK_BATCH, batch);
        } else {
            batch_destroy(batch);
        }

        fclose(slice_fp);
        free(slice_path);
        
        // 可选：打印进度日志
        if (s_idx % 10 == 0) {
            verbose_printf(cfg, 2, "  - 已扫描分片 %lu, 累计记录 %lu\n", s_idx, global_index);
        }
    }
    
    verbose_printf(cfg, 1, "进度恢复完成。重放任务数: %lu\n", global_index > state->processed_count ? global_index - state->processed_count : 0);
    return batch_count;
}

// ==========================================
// 6. 清理
// ==========================================

void cleanup_progress(const Config *cfg, RuntimeState *state) {
    char *idx_path = get_index_filename(cfg->progress_base);
    unlink(idx_path);
    free(idx_path);

    for (unsigned long i = 0; i <= state->write_slice_index + 1; i++) { 
        char *slice_path = get_slice_filename(cfg->progress_base, i);
        unlink(slice_path);
        free(slice_path);
        if (!cfg->archive) {
            char *gz_path = get_archive_filename(cfg->progress_base, i);
            unlink(gz_path);
            free(gz_path);
        }
    }
    char error_log[1024];
    snprintf(error_log, sizeof(error_log), "%s.error.log", cfg->progress_base);
    unlink(error_log);
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
    unlink(config_path);
}

void finalize_progress(const Config *cfg, RuntimeState *state) {
    if (!cfg->continue_mode) return;
    if (state->write_slice_file) {
        fclose(state->write_slice_file);
        state->write_slice_file = NULL;
        process_old_slice(cfg, state->write_slice_index);
    }
}

