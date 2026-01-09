#include "progress.h"
#include "utils.h"
#include "looper.h"
#include "traversal.h" 
#include "idempotency.h"
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
#include <dirent.h> // for DT_REG, DT_DIR

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

char *get_archive_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s.archive", base);
    return name;
}

// 辅助：将 st_mode 转换为 d_type (用于持久化)
static unsigned char mode_to_dtype(mode_t mode) {
    if (S_ISREG(mode)) return DT_REG;
    if (S_ISDIR(mode)) return DT_DIR;
    if (S_ISLNK(mode)) return DT_LNK;
    if (S_ISCHR(mode)) return DT_CHR;
    if (S_ISBLK(mode)) return DT_BLK;
    if (S_ISFIFO(mode)) return DT_FIFO;
    if (S_ISSOCK(mode)) return DT_SOCK;
    return DT_UNKNOWN;
}

// ==========================================
// 2. 归档核心逻辑 (Zlib Append)
// ==========================================

static void archive_slice_to_single_file(const Config *cfg, unsigned long index) {
    char *src_path = get_slice_filename(cfg->progress_base, index);
    
    FILE *in = fopen(src_path, "rb");
    if (!in) {
        free(src_path);
        return; 
    }

    fseek(in, 0, SEEK_END);
    long src_size = ftell(in);
    fseek(in, 0, SEEK_SET);
    
    if (src_size <= 0) {
        fclose(in);
        unlink(src_path);
        free(src_path);
        return;
    }

    unsigned char *src_buf = safe_malloc(src_size);
    if (fread(src_buf, 1, src_size, in) != (size_t)src_size) {
        free(src_buf);
        fclose(in);
        free(src_path);
        return;
    }
    fclose(in);

    unsigned long dest_len = compressBound(src_size);
    unsigned char *dest_buf = safe_malloc(dest_len);
    
    if (compress(dest_buf, &dest_len, src_buf, src_size) != Z_OK) {
        fprintf(stderr, "错误: 压缩分片 %lu 失败\n", index);
        free(src_buf);
        free(dest_buf);
        free(src_path);
        return;
    }
    free(src_buf);

    char *archive_path = get_archive_filename(cfg->progress_base);
    FILE *out = fopen(archive_path, "ab");
    
    if (out) {
        uint32_t u_size = (uint32_t)src_size;
        uint32_t c_size = (uint32_t)dest_len;
        
        fwrite(&u_size, sizeof(uint32_t), 1, out);
        fwrite(&c_size, sizeof(uint32_t), 1, out);
        fwrite(dest_buf, 1, dest_len, out);
        
        fclose(out);
        unlink(src_path); // 成功才删除
    } else {
        perror("无法打开归档文件进行追加");
    }

    free(dest_buf);
    free(src_path);
    free(archive_path);
}

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

// [修改] 写入完整的元数据 (mtime, hash, type)
void record_path(const Config *cfg, RuntimeState *state, const char *path, const struct stat *info) {
    if (!state->write_slice_file) {
        char *p = get_slice_filename(cfg->progress_base, state->write_slice_index);
        state->write_slice_file = fopen(p, "wb");
        free(p);
    }
    if (!state->write_slice_file) return;

    // 准备数据
    size_t path_len = strlen(path);
    dev_t dev = info ? info->st_dev : 0;
    ino_t ino = info ? info->st_ino : 0;
    
    // 新增字段
    time_t mtime = info ? info->st_mtime : 0;
    uint32_t name_hash = calculate_name_hash(path);
    unsigned char d_type = info ? mode_to_dtype(info->st_mode) : DT_UNKNOWN;

    // 写入
    fwrite(&path_len, sizeof(size_t), 1, state->write_slice_file);
    fwrite(path, 1, path_len, state->write_slice_file);
    fwrite(&dev, sizeof(dev_t), 1, state->write_slice_file);
    fwrite(&ino, sizeof(ino_t), 1, state->write_slice_file);
    
    // [新增写入]
    fwrite(&mtime, sizeof(time_t), 1, state->write_slice_file);
    fwrite(&name_hash, sizeof(uint32_t), 1, state->write_slice_file);
    fwrite(&d_type, sizeof(unsigned char), 1, state->write_slice_file);

    state->line_count++; 
    state->processed_count++; 
    if (state->line_count >= cfg->progress_slice_lines) {
        rotate_progress_slice(cfg, state);
    }
}

// ==========================================
// 4. 进度恢复 (消费者 - 支持双模式)
// ==========================================

// 通用解析器：支持 Resume (重放) 和 Incremental (只读加载)
// target_set: 如果不为 NULL，则将对象插入此集合
// replay_queue: 如果不为 NULL，且 global_index 超出 processed_count，则构造任务发往此队列
static void parse_and_process_buffer(RuntimeState *state, 
                                   MessageQueue *replay_queue,  
                                   HashSet *target_set,
                                   unsigned char *buf, 
                                   size_t size, 
                                   unsigned long *global_index) {
    size_t pos = 0;
    TaskBatch *batch = NULL;
    if (replay_queue) batch = batch_create();

    while (pos < size) {
        if (pos + sizeof(size_t) > size) break;
        
        size_t path_len;
        memcpy(&path_len, buf + pos, sizeof(size_t));
        pos += sizeof(size_t);

        // 计算剩余需要的最小长度 (path + dev + ino + mtime + hash + type)
        size_t entry_meta_size = sizeof(dev_t) + sizeof(ino_t) + 
                                 sizeof(time_t) + sizeof(uint32_t) + sizeof(unsigned char);
        
        if (pos + path_len + entry_meta_size > size) break;

        char *path_ptr = (char*)(buf + pos);
        pos += path_len;

        // 读取元数据
        dev_t dev; ino_t ino; time_t mtime; uint32_t name_hash; unsigned char d_type;
        
        memcpy(&dev, buf + pos, sizeof(dev_t)); pos += sizeof(dev_t);
        memcpy(&ino, buf + pos, sizeof(ino_t)); pos += sizeof(ino_t);
        memcpy(&mtime, buf + pos, sizeof(time_t)); pos += sizeof(time_t);
        memcpy(&name_hash, buf + pos, sizeof(uint32_t)); pos += sizeof(uint32_t);
        memcpy(&d_type, buf + pos, sizeof(unsigned char)); pos += sizeof(unsigned char);

        // 逻辑分流
        // 1. 如果提供了 target_set (Reference Set 或 Visited Set)，则插入
        if (target_set) {
            // 注意：Resume 模式下只存 dev/ino 即可，但 Incremental 模式需要 mtime/hash
            // 我们统一存入所有信息
            ObjectIdentifier id = { 
                .st_dev = dev, .st_ino = ino, 
                .mtime = mtime, .name_hash = name_hash, .d_type = d_type 
            };
            hash_set_insert(target_set, &id);
        }

        // 2. 如果需要重放任务 (Resume 模式的尾部数据)
        if (replay_queue) {
            if (*global_index >= state->processed_count) {
                // 需要 path 字符串
                char *path_str = safe_malloc(path_len + 1);
                memcpy(path_str, path_ptr, path_len);
                path_str[path_len] = '\0';
                
                batch_add(batch, path_str, NULL);
                free(path_str); 

                if (batch->count >= BATCH_SIZE) {
                    traversal_add_pending_tasks(1);
                    mq_send(replay_queue, MSG_CHECK_BATCH, batch);
                    batch = batch_create();
                }
            }
        }
        
        if (global_index) (*global_index)++;
    }

    if (replay_queue && batch) {
        if (batch->count > 0) {
            traversal_add_pending_tasks(1);
            mq_send(replay_queue, MSG_CHECK_BATCH, batch);
        } else {
            batch_destroy(batch);
        }
    }
}

// 内部函数：遍历所有 pbin 和 archive
static void iterate_stored_progress(const Config *cfg, RuntimeState *state, 
                                  MessageQueue *mq, HashSet *target_set) {
    unsigned long global_index = 0;
    
    // 1. 加载归档 (archive)
    char *archive_path = get_archive_filename(cfg->progress_base);
    FILE *arch_fp = fopen(archive_path, "rb");
    if (arch_fp) {
        verbose_printf(cfg, 1, "正在加载归档文件: %s ...\n", archive_path);
        uint32_t u_size, c_size;
        while (fread(&u_size, sizeof(uint32_t), 1, arch_fp) == 1 &&
               fread(&c_size, sizeof(uint32_t), 1, arch_fp) == 1) {
            
            unsigned char *cmp_buf = safe_malloc(c_size);
            if (fread(cmp_buf, 1, c_size, arch_fp) != c_size) {
                free(cmp_buf); break;
            }

            unsigned char *raw_buf = safe_malloc(u_size);
            unsigned long dest_len = u_size;
            
            if (uncompress(raw_buf, &dest_len, cmp_buf, c_size) == Z_OK) {
                parse_and_process_buffer(state, mq, target_set, raw_buf, dest_len, &global_index);
            }
            free(raw_buf);
            free(cmp_buf);
        }
        fclose(arch_fp);
    }
    free(archive_path);

    // 2. 加载散落 pbin
    int consecutive_missing = 0;
    for (unsigned long s_idx = 0; ; ++s_idx) {
        char *slice_path = get_slice_filename(cfg->progress_base, s_idx);
        FILE *slice_fp = fopen(slice_path, "rb");
        
        if (!slice_fp) {
            free(slice_path);
            consecutive_missing++;
            if (consecutive_missing > 50 && s_idx > state->process_slice_index) break;
            continue; 
        }
        consecutive_missing = 0;

        fseek(slice_fp, 0, SEEK_END);
        long fsize = ftell(slice_fp);
        fseek(slice_fp, 0, SEEK_SET);
        
        if (fsize > 0) {
            unsigned char *buf = safe_malloc(fsize);
            fread(buf, 1, fsize, slice_fp);
            parse_and_process_buffer(state, mq, target_set, buf, fsize, &global_index);
            free(buf);
        }
        fclose(slice_fp);
        free(slice_path);
    }
    
    verbose_printf(cfg, 1, "进度加载完成，共处理记录: %lu\n", global_index);
}

// [Resume模式]
int restore_progress(const Config *cfg, MessageQueue *worker_mq, RuntimeState *state) {
    verbose_printf(cfg, 1, "开始断点恢复 (目标: %lu)...\n", state->processed_count);
    // 恢复时，我们把历史记录加载到 g_visited_history (防止环路)，并重放未完成任务
    iterate_stored_progress(cfg, state, worker_mq, g_visited_history);
    return 0;
}

// [Incremental模式]
void restore_progress_to_memory(const Config *cfg, HashSet *target_set) {
    verbose_printf(cfg, 1, "开始加载半增量索引到内存...\n");
    // 增量模式下，不需要重放任务 (mq=NULL)，只需要填充 target_set
    // global_index 在这里不影响逻辑，但 iterate_stored_progress 会维护它
    iterate_stored_progress(cfg, (RuntimeState*)NULL, NULL, target_set);
}

// ==========================================
// 5. 辅助功能
// ==========================================

bool load_progress_index(const Config *cfg, RuntimeState *state) {
    char *idx_file = get_index_filename(cfg->progress_base);
    FILE *fp = fopen(idx_file, "r");
    if (!fp) { free(idx_file); return false; }
    // 注意：这里我们增加了校验，确保读取成功
    int matches = fscanf(fp, "%lu %lu %lu %lu %lu", 
            &state->process_slice_index, &state->processed_count,
            &state->write_slice_index, &state->output_slice_num, &state->output_line_count);
    fclose(fp); free(idx_file);
    return matches == 5;
}

int acquire_lock(const Config *cfg, RuntimeState *state) {
    // 即使不是 continue 模式，为了防止并发运行同一任务，建议也加锁，
    // 但这里保持原有逻辑，仅在 continue 时加锁
    if (!cfg->continue_mode) return 0;
    
    char *lock_path = safe_malloc(strlen(cfg->progress_base) + 32);
    sprintf(lock_path, "%s.lock", cfg->progress_base);
    state->lock_file_path = lock_path;
    
    int fd = open(lock_path, O_RDWR | O_CREAT, 0666);
    if (fd == -1) return -1;
    
    if (flock(fd, LOCK_EX | LOCK_NB) == -1) { 
        close(fd); 
        return -1; 
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

void save_config_to_disk(const Config *cfg) {
    if (!cfg->progress_base) return;
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
    FILE *fp = fopen(config_path, "w");
    if (!fp) return;
    
    // 保存关键会话信息
    fprintf(fp, "path=%s\n", cfg->target_path);
    if (cfg->output_file) fprintf(fp, "output=%s\n", cfg->output_file);
    if (cfg->output_split_dir) fprintf(fp, "output_split=%s\n", cfg->output_split_dir);
    
    fprintf(fp, "start_time=%ld\n", time(NULL));
    fprintf(fp, "archive=%d\n", cfg->archive);
    fprintf(fp, "clean=%d\n", cfg->clean);
    fprintf(fp, "csv=%d\n", cfg->csv);
    // 默认状态为 Running
    fprintf(fp, "status=Running\n");
    
    fclose(fp);
}

void cleanup_progress(const Config *cfg, RuntimeState *state) {
    char *idx_path = get_index_filename(cfg->progress_base);
    unlink(idx_path);
    free(idx_path);

    // 清理散落的 pbin
    // 逻辑：如果开启了 Clean 或 Archive，则清理 pbin
    if (cfg->clean || cfg->archive) {
        for (unsigned long i = 0; i <= state->write_slice_index + 200; i++) { 
            char *slice_path = get_slice_filename(cfg->progress_base, i);
            unlink(slice_path);
            free(slice_path);
        }
    }
    
    char *arch_path = get_archive_filename(cfg->progress_base);
    // [修复] 只有在 explicitly clean 模式下才删除 archive
    if (cfg->clean) {
        unlink(arch_path);
    }
    free(arch_path);

    char error_log[1024];
    snprintf(error_log, sizeof(error_log), "%s.error.log", cfg->progress_base);
    unlink(error_log);
    
    // config 文件通常保留，除非 clean
    if (cfg->clean) {
        char config_path[1024];
        snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
        unlink(config_path);
    }
}

void finalize_progress(const Config *cfg, RuntimeState *state) {
    // 1. 关闭分片文件
    if (state->write_slice_file) {
        fclose(state->write_slice_file);
        state->write_slice_file = NULL;
        process_old_slice(cfg, state->write_slice_index);
    }
    
    // 2. 更新 config 状态
    if (cfg->progress_base) {
        char config_path[1024];
        snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
        FILE *fp = fopen(config_path, "a");
        if (fp) {
            // [修改] 根据 has_error 决定最终状态
            if (state->has_error) {
                fprintf(fp, "status=Incomplete\n"); // 标记为未完成，下次触发 Resume
                fprintf(fp, "error=DeviceMeltdown\n");
            } else {
                fprintf(fp, "status=Success\n");
            }
            fprintf(fp, "end_time=%ld\n", time(NULL));
            fclose(fp);
        }
    }
}