#include "progress.h"
#include "utils.h"
#include "looper.h"    // 引入 Looper
#include "traversal.h" // 引入 MSG_CHECK_BATCH
#include "idempotency.h" // 引入 idempotency 相关
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <zlib.h>
#include <stdint.h>

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

// [新增] 单一归档文件路径
char *get_archive_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s.archive", base);
    return name;
}
// ==========================================
// 2. 归档核心逻辑 (Zlib Append)
// ==========================================

// 将单个 pbin 切片压缩并追加到 archive 文件
static void archive_slice_to_single_file(const Config *cfg, unsigned long index) {
    char *src_path = get_slice_filename(cfg->progress_base, index);
    
    FILE *in = fopen(src_path, "rb");
    if (!in) {
        free(src_path);
        return; // 文件可能已经被处理过或不存在
    }

    // 1. 读取原始数据
    fseek(in, 0, SEEK_END);
    long src_size = ftell(in);
    fseek(in, 0, SEEK_SET);
    
    if (src_size <= 0) {
        fclose(in);
        unlink(src_path); // 空文件直接删除
        free(src_path);
        return;
    }

    unsigned char *src_buf = safe_malloc(src_size);
    if (fread(src_buf, 1, src_size, in) != (size_t)src_size) {
        free(src_buf);
        fclose(in);
        free(src_path);
        return; // 读取失败，保留文件下次再试
    }
    fclose(in);

    // 2. 使用 zlib 压缩
    unsigned long dest_len = compressBound(src_size);
    unsigned char *dest_buf = safe_malloc(dest_len);
    
    if (compress(dest_buf, &dest_len, src_buf, src_size) != Z_OK) {
        fprintf(stderr, "错误: 压缩分片 %lu 失败\n", index);
        free(src_buf);
        free(dest_buf);
        free(src_path);
        return;
    }
    free(src_buf); // 释放原始数据

    // 3. 追加写入 archive 文件
    // 格式: [UncompressedSize (4)] [CompressedSize (4)] [Data...]
    char *archive_path = get_archive_filename(cfg->progress_base);
    FILE *out = fopen(archive_path, "ab"); // Append Binary
    
    if (out) {
        uint32_t u_size = (uint32_t)src_size;
        uint32_t c_size = (uint32_t)dest_len;
        
        fwrite(&u_size, sizeof(uint32_t), 1, out);
        fwrite(&c_size, sizeof(uint32_t), 1, out);
        fwrite(dest_buf, 1, dest_len, out);
        
        fclose(out);
        
        // 4. 只有写入成功才删除源文件
        unlink(src_path);
    } else {
        perror("无法打开归档文件进行追加");
    }

    free(dest_buf);
    free(src_path);
    free(archive_path);
}

// 轮转时的回调
static void process_old_slice(const Config *cfg, unsigned long index) {
    if (cfg->archive) {
        archive_slice_to_single_file(cfg, index);
    } else if (cfg->clean) {
        char *path = get_slice_filename(cfg->progress_base, index);
        unlink(path);
        free(path);
    }
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

// ==========================================
// 3. 进度记录 (生产者)
// ==========================================

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
        // 触发归档逻辑
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
// 4. 进度恢复 (消费者 - 支持归档)
// ==========================================

// 通用 Buffer 解析器：无需 IO，纯内存操作，极快
static void parse_and_process_buffer(const Config *cfg, MessageQueue *mq, RuntimeState *state, 
                                   unsigned char *buf, size_t size, unsigned long *global_index) {
    size_t pos = 0;
    TaskBatch *batch = batch_create();

    while (pos < size) {
        // 安全检查
        if (pos + sizeof(size_t) > size) break;
        
        size_t path_len;
        memcpy(&path_len, buf + pos, sizeof(size_t));
        pos += sizeof(size_t);

        if (pos + path_len + sizeof(dev_t) + sizeof(ino_t) > size) break;

        char *path_ptr = (char*)(buf + pos);
        pos += path_len;

        dev_t dev;
        ino_t ino;
        memcpy(&dev, buf + pos, sizeof(dev_t));
        pos += sizeof(dev_t);
        memcpy(&ino, buf + pos, sizeof(ino_t));
        pos += sizeof(ino_t);

        // 构造临时 0 结尾字符串 (仅用于 batch_add 需要)
        // 优化：这里只有在需要 "重放" 时才需要 path 字符串
        // 对于 "黑名单"，我们只需要 dev/ino，甚至不需要 path_str
        
        if (*global_index < state->processed_count) {
            // === 情况 A: 历史记录 -> 进黑名单 ===
            // 纯内存操作，零 IO，不需要 path 字符串
            if (g_history_object_set) {
                ObjectIdentifier id = { .st_dev = dev, .st_ino = ino };
                hash_set_insert(g_history_object_set, &id);
            }
        } else {
            // === 情况 B: 尾部记录 -> 重新入队 ===
            // 需要 path 字符串
            char *path_str = safe_malloc(path_len + 1);
            memcpy(path_str, path_ptr, path_len);
            path_str[path_len] = '\0';
            
            batch_add(batch, path_str, NULL);
            free(path_str); // batch_add 会 strdup

            if (batch->count >= BATCH_SIZE) {
                traversal_add_pending_tasks(1);
                mq_send(mq, MSG_CHECK_BATCH, batch);
                batch = batch_create();
            }
        }
        
        (*global_index)++;
    }

    if (batch->count > 0) {
        traversal_add_pending_tasks(1);
        mq_send(mq, MSG_CHECK_BATCH, batch);
    } else {
        batch_destroy(batch);
    }
}

int restore_progress(const Config *cfg, MessageQueue *worker_mq, RuntimeState *state) {
    unsigned long global_index = 0;
    verbose_printf(cfg, 1, "开始恢复进度 (Watermark: %lu)...\n", state->processed_count);

    // -------------------------------------------------
    // 第一步：加载归档文件 (progress.archive)
    // -------------------------------------------------
    char *archive_path = get_archive_filename(cfg->progress_base);
    FILE *arch_fp = fopen(archive_path, "rb");
    if (arch_fp) {
        verbose_printf(cfg, 1, "正在加载归档文件: %s ...\n", archive_path);
        
        uint32_t u_size, c_size;
        // 循环读取块头: [UncompressedSize][CompressedSize]
        while (fread(&u_size, sizeof(uint32_t), 1, arch_fp) == 1 &&
               fread(&c_size, sizeof(uint32_t), 1, arch_fp) == 1) {
            
            // 读压缩数据
            unsigned char *cmp_buf = safe_malloc(c_size);
            if (fread(cmp_buf, 1, c_size, arch_fp) != c_size) {
                free(cmp_buf);
                break; // 文件截断
            }

            // 解压
            unsigned char *raw_buf = safe_malloc(u_size);
            unsigned long dest_len = u_size;
            
            if (uncompress(raw_buf, &dest_len, cmp_buf, c_size) == Z_OK) {
                // 解析并处理
                parse_and_process_buffer(cfg, worker_mq, state, raw_buf, dest_len, &global_index);
            } else {
                fprintf(stderr, "警告: 归档块解压失败，跳过该块\n");
            }

            free(raw_buf);
            free(cmp_buf);
        }
        fclose(arch_fp);
    }
    free(archive_path);

    // -------------------------------------------------
    // 第二步：加载散落的 .pbin 文件
    // -------------------------------------------------
    // 我们从 0 开始尝试，虽然大部分可能已被归档并删除。
    // 这是一个快速扫描，用于捕获那些还没来得及归档的尾部切片。
    
    int consecutive_missing = 0;
    for (unsigned long s_idx = 0; ; ++s_idx) {
        char *slice_path = get_slice_filename(cfg->progress_base, s_idx);
        FILE *slice_fp = fopen(slice_path, "rb");
        
        if (!slice_fp) {
            free(slice_path);
            consecutive_missing++;
            // 如果连续 100 个切片都不存在，且我们已经读过了归档
            // 且 index 已经很大了，说明后面没东西了
            if (consecutive_missing > 100 && s_idx > state->process_slice_index) {
                break;
            }
            continue; 
        }
        consecutive_missing = 0; // 重置计数器

        // 读取 pbin 内容
        fseek(slice_fp, 0, SEEK_END);
        long fsize = ftell(slice_fp);
        fseek(slice_fp, 0, SEEK_SET);
        
        if (fsize > 0) {
            unsigned char *buf = safe_malloc(fsize);
            fread(buf, 1, fsize, slice_fp);
            parse_and_process_buffer(cfg, worker_mq, state, buf, fsize, &global_index);
            free(buf);
        }

        fclose(slice_fp);
        free(slice_path);
    }

    verbose_printf(cfg, 1, "恢复完成。总记录: %lu, 重放任务: %lu\n", 
        global_index,
        global_index > state->processed_count ? global_index - state->processed_count : 0);
    return 0; // batch_count 已不再重要
}
// ==========================================
// 5. 辅助功能
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

int acquire_lock(const Config *cfg, RuntimeState *state) {
    if (!cfg->continue_mode) return 0;
    char *lock_path = safe_malloc(strlen(cfg->progress_base) + 32);
    sprintf(lock_path, "%s.lock", cfg->progress_base);
    state->lock_file_path = lock_path;
    int fd = open(lock_path, O_RDWR | O_CREAT, 0666);
    if (fd == -1) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) == -1) { close(fd); return -1; }
    state->lock_fd = fd;
    return 0;
}

void release_lock(RuntimeState *state) {
    if (state->lock_fd != -1) { flock(state->lock_fd, LOCK_UN); close(state->lock_fd); state->lock_fd = -1; }
    if (state->lock_file_path) { unlink(state->lock_file_path); free(state->lock_file_path); state->lock_file_path = NULL; }
}

void save_config_to_disk(const Config *cfg) {
    if (!cfg->continue_mode) return;
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
    FILE *fp = fopen(config_path, "w");
    if (!fp) return;
    fprintf(fp, "path=%s\n", cfg->target_path);
    fclose(fp);
}

void cleanup_progress(const Config *cfg, RuntimeState *state) {
    char *idx_path = get_index_filename(cfg->progress_base);
    unlink(idx_path);
    free(idx_path);

    // 清理散落的 pbin
    for (unsigned long i = 0; i <= state->write_slice_index + 200; i++) { 
        char *slice_path = get_slice_filename(cfg->progress_base, i);
        unlink(slice_path);
        free(slice_path);
    }
    
    // [新增] 清理 archive
    char *arch_path = get_archive_filename(cfg->progress_base);
    unlink(arch_path);
    free(arch_path);

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
        // 退出时也尝试归档最后一片
        process_old_slice(cfg, state->write_slice_index);
    }
}