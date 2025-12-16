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

// ==========================================
// 2. 锁机制 (必须保留)
// ==========================================

// 获取文件锁，防止多实例运行
int acquire_lock(const Config *cfg, RuntimeState *state) {
    if (!cfg->continue_mode) return 0;

    char *lock_path = safe_malloc(strlen(cfg->progress_base) + 32);
    sprintf(lock_path, "%s.lock", cfg->progress_base);
    state->lock_file_path = lock_path;

    int fd = open(lock_path, O_RDWR | O_CREAT, 0666);
    if (fd == -1) {
        perror("无法创建锁文件");
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr, "错误: 另一个实例正在运行 (锁文件被占用)\n");
        } else {
            perror("无法获取文件锁");
        }
        close(fd);
        return -1;
    }

    state->lock_fd = fd;
    return 0;
}

// 释放锁
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
// 3. 配置保存 (必须保留)
// ==========================================

void save_config_to_disk(const Config *cfg) {
    if (!cfg->continue_mode) return;

    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
    
    FILE *fp = fopen(config_path, "w");
    if (!fp) return;

    // 保存关键配置用于校验
    fprintf(fp, "path=%s\n", cfg->target_path);
    fprintf(fp, "follow_symlinks=%d\n", cfg->follow_symlinks);
    // 可以根据需要添加更多字段
    
    fclose(fp);
}

// ==========================================
// 4. 进度记录 (核心生产者 - 必须保留)
// ==========================================

// 原子更新索引文件 (线程安全版)
void atomic_update_index(const Config *cfg, RuntimeState *state) {
    char *idx_file = get_index_filename(cfg->progress_base);
    char *tmp_file = safe_malloc(strlen(idx_file) + 64);
    
    // 使用线程ID防止冲突
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
        
        if (rename(tmp_file, idx_file) != 0) {
            unlink(tmp_file);
        }
    }
    free(idx_file);
    free(tmp_file);
}

// 轮转分片文件 (当当前分片写满时调用)
void rotate_progress_slice(const Config *cfg, RuntimeState *state) {
    if (state->write_slice_file) {
        fclose(state->write_slice_file);
    }

    state->write_slice_index++;
    state->line_count = 0; // 重置当前分片的行数计数
    
    char *path = get_slice_filename(cfg->progress_base, state->write_slice_index);
    state->write_slice_file = fopen(path, "wb");
    if (!state->write_slice_file) {
        perror("无法创建新的进度分片文件");
        // 严重错误，可能需要退出
    }
    free(path);

    // 每次轮转都更新一次索引
    atomic_update_index(cfg, state);
}

// 【保留并修改】记录路径到进度文件
// 这是“生产者”，它负责把处理完的路径写到磁盘，供下次 restore_progress 读取
void record_path(const Config *cfg, RuntimeState *state, const char *path, const struct stat *info) {
    // 懒加载：第一次写入时打开文件
    if (!state->write_slice_file) {
        char *p = get_slice_filename(cfg->progress_base, state->write_slice_index);
        state->write_slice_file = fopen(p, "wb"); // append or write
        free(p);
    }

    if (!state->write_slice_file) return;

    size_t path_len = strlen(path);
    dev_t dev = info ? info->st_dev : 0;
    ino_t ino = info ? info->st_ino : 0;

    // 二进制格式写入: [len][path][dev][ino]
    fwrite(&path_len, sizeof(size_t), 1, state->write_slice_file);
    fwrite(path, 1, path_len, state->write_slice_file);
    fwrite(&dev, sizeof(dev_t), 1, state->write_slice_file);
    fwrite(&ino, sizeof(ino_t), 1, state->write_slice_file);

    state->line_count++; // 记录当前分片已写的条数
    state->processed_count++; // 记录总处理条数

    // 检查是否需要切分
    if (state->line_count >= cfg->progress_slice_lines) {
        rotate_progress_slice(cfg, state);
    }
}

// ==========================================
// 5. 进度恢复 (核心消费者 - 新增/重写)
// ==========================================

// 加载索引文件
bool load_progress_index(const Config *cfg, RuntimeState *state) {
    char *idx_file = get_index_filename(cfg->progress_base);
    FILE *fp = fopen(idx_file, "r");
    
    if (!fp) {
        free(idx_file);
        return false;
    }

    int matches = fscanf(fp, "%lu %lu %lu %lu %lu", 
            &state->process_slice_index, 
            &state->processed_count,
            &state->write_slice_index,
            &state->output_slice_num,
            &state->output_line_count);

    fclose(fp);
    free(idx_file);

    if (matches != 5) {
        fprintf(stderr, "警告: 进度索引文件格式错误或已损坏，可能无法正确恢复。\n");
        return false;
    }
    
    // 恢复时，写入分片索引应该接续在处理分片之后，或者等于记录的写入分片
    // 这里我们信任 idx 文件里的 write_slice_index
    return true;
}

// 从分片恢复任务队列 (适配 Looper 架构)
int restore_progress(const Config *cfg, MessageQueue *worker_mq, RuntimeState *state) {
    int batch_count = 0;
    verbose_printf(cfg, 1, "开始从二进制进度文件恢复任务队列...\n");

    // 阶段一：构建已完成任务的去重集合 (黑名单)
    verbose_printf(cfg, 2, "阶段一：构建已完成任务的去重集合...\n");
    for (unsigned long s_idx = 0; ; ++s_idx) {
        char *slice_path = get_slice_filename(cfg->progress_base, s_idx);
        FILE *slice_fp = fopen(slice_path, "rb");
        if (!slice_fp) {
            free(slice_path);
            break; 
        }

        size_t path_len;
        dev_t dev;
        ino_t ino;
        unsigned long record_idx_in_slice = 0;

        while (fread(&path_len, sizeof(size_t), 1, slice_fp) == 1) {
            fseek(slice_fp, path_len, SEEK_CUR); // 跳过路径
            
            if (fread(&dev, sizeof(dev_t), 1, slice_fp) == 1 &&
                fread(&ino, sizeof(ino_t), 1, slice_fp) == 1) {
                
                if (g_history_object_set) {
                    bool is_truly_processed = false;
                    // 如果该分片完全在已处理进度之前
                    if (s_idx < state->process_slice_index) {
                        is_truly_processed = true;
                    } 
                    // 如果是当前分片，且记录在游标之前
                    else if (s_idx == state->process_slice_index) {
                        if (record_idx_in_slice < state->processed_count) {
                            is_truly_processed = true;
                        }
                    }

                    if (is_truly_processed) {
                        ObjectIdentifier id = { .st_dev = dev, .st_ino = ino };
                        hash_set_insert(g_history_object_set, &id);
                    }
                }
            }
            record_idx_in_slice++;
        }
        fclose(slice_fp);
        free(slice_path);
    }

    // 阶段二：加载未处理的任务到 Worker 队列
    verbose_printf(cfg, 2, "阶段二：加载未处理的任务到 Worker 队列...\n");
    
    for (unsigned long s_idx = state->process_slice_index; ; ++s_idx) {
        char *slice_path = get_slice_filename(cfg->progress_base, s_idx);
        FILE *slice_fp = fopen(slice_path, "rb");
        if (!slice_fp) {
            free(slice_path);
            break;
        }

        size_t path_len;
        dev_t dev;
        ino_t ino;
        char path_buf[MAX_PATH_LENGTH];
        unsigned long record_idx_in_slice = 0;
        unsigned long loaded_count = 0;

        TaskBatch *batch = batch_create();

        while (fread(&path_len, sizeof(size_t), 1, slice_fp) == 1) {
            if (path_len >= sizeof(path_buf)) {
                fseek(slice_fp, path_len, SEEK_CUR);
                fread(&dev, sizeof(dev_t), 1, slice_fp);
                fread(&ino, sizeof(ino_t), 1, slice_fp);
                record_idx_in_slice++;
                continue;
            }

            if (fread(path_buf, 1, path_len, slice_fp) != path_len) break;
            path_buf[path_len] = '\0';

            fread(&dev, sizeof(dev_t), 1, slice_fp);
            fread(&ino, sizeof(ino_t), 1, slice_fp);

            bool is_pending = true;
            if (s_idx == state->process_slice_index) {
                if (record_idx_in_slice < state->processed_count) {
                    is_pending = false;
                }
            }

            if (is_pending) {
                // 放入 Batch, 发送 MSG_CHECK_BATCH
                batch_add(batch, path_buf, NULL);
                loaded_count++;

                if (batch->count >= BATCH_SIZE) {
                    // 注意：在 Looper 模式下，这些都是发给 Worker 去 stat 的
                    // 必须配合 run_main_looper 里的 atomic_fetch_add 使用
                    // 这里我们假设 run_main_looper 会处理计数，或者我们只负责入队
                    // *修正*: traversal.c 中的 run_main_looper 负责增加 pending_tasks
                    // 但这里是在 looper 运行 *之前* 还是 *之中* 调用的？
                    // 答：restore_progress 在 Looper 启动前调用。
                    // 因此这里不能直接用 g_pending_tasks。
                    // 方案：改为由 traversal.c 处理计数，或者这里仅仅是把初始任务塞进去。
                    // 鉴于 g_pending_tasks 是 static 的，外部访问不到。
                    // 我们只需 send，Looper 启动时会处理队列。
                    // 但 pending_tasks 计数必须准确！
                    // 所以：restore_progress 最好不要直接 send，或者 traversal.c 需要提供一个 helper
                    // 简单做法：我们直接 send，Worker 收到 CHECK_BATCH 处理完会发 TASK_DONE
                    // 但初始的 pending_tasks 谁来加？
                    // 答：run_main_looper 里的 atomic_store(&g_pending_tasks, 0) 会重置它。
                    // 这会导致问题！
                    
                    // 【修正】：我们应该在发送时增加计数。
                    // 由于 g_pending_tasks 是 traversal.c 私有的，
                    // 我们需要一个 extern 接口，或者让 restore_progress 返回加载的任务数。
                    // 为了简单，我们假定 traversal.c 会把 g_pending_tasks 暴露出来，或者提供接口。
                    // **最简单的方案**：让 traversal.c 的 `dispatch_resume_file` 逻辑通用化。
                    // 现在先保持 mq_send，我们在 traversal.c 里解决计数问题。
                    traversal_add_pending_tasks(1);
                    mq_send(worker_mq, MSG_CHECK_BATCH, batch);
                    batch = batch_create();
                    batch_count++;
                }
            }
            record_idx_in_slice++;
        }
        
        if (batch->count > 0) {
            traversal_add_pending_tasks(1);
            mq_send(worker_mq, MSG_CHECK_BATCH, batch);
        } else {
            batch_destroy(batch);
        }

        fclose(slice_fp);
        free(slice_path);
        
        if (loaded_count > 0) {
            verbose_printf(cfg, 2, "  - 分片 %lu 加载了 %lu 项任务\n", s_idx, loaded_count);
        }
    }
    
    verbose_printf(cfg, 1, "进度恢复完成。\n");
    return batch_count;
}

// ==========================================
// 6. 清理 (必须保留)
// ==========================================

void cleanup_progress(const Config *cfg, RuntimeState *state) {
    char *idx_path = get_index_filename(cfg->progress_base);
    unlink(idx_path);
    free(idx_path);

    // 清理所有分片
    // 注意：这里用 write_slice_index 作为上限是安全的，因为恢复时会更新它
    for (unsigned long i = 0; i <= state->write_slice_index + 1; i++) { 
        char *slice_path = get_slice_filename(cfg->progress_base, i);
        unlink(slice_path);
        free(slice_path);
    }
    
    char error_log[1024];
    snprintf(error_log, sizeof(error_log), "%s.error.log", cfg->progress_base);
    unlink(error_log);
    
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
    unlink(config_path);
}