#include "progress.h"
#include "utils.h"
#include "signals.h" // 需要调用 register_locked_file
#include "idempotency.h"
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <errno.h>
#include <signal.h>

// 获取分片文件名
// 修改进度文件切片命名函数
char *get_slice_filename(const char *base, unsigned long slice_index) {
    char *name = safe_malloc(strlen(base) + 20);
    snprintf(name, strlen(base) + 20, "%s_" PROGRESS_SLICE_FORMAT ".pbin", base, slice_index);
    return name;
}


char *get_index_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 5);
    snprintf(name, strlen(base) + 5, "%s.idx", base);
    return name;
}

char *get_paths_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 7);
    snprintf(name, strlen(base) + 7, "%s.paths", base);
    return name;
}

// 初始化进度系统(支持分片)
void progress_init(const Config *cfg, RuntimeState *state) {
    if (!cfg->continue_mode) return;

    char *idx_file = get_index_filename(cfg->progress_base);
    FILE *index_fp = fopen(idx_file, "r");
    if (index_fp) {
        verbose_printf(cfg, 1, "找到进度索引文件 %s,尝试恢复状态...\n", idx_file);
        // 尝试从索引文件读取所有5个状态值
        if (fscanf(index_fp, "%lu %lu %lu %lu %lu", 
                   &state->process_slice_index, 
                   &state->processed_count,
                   &state->write_slice_index,
                   &state->output_slice_num,
                   &state->output_line_count) == 5) {
            verbose_printf(cfg, 1, "状态恢复成功: 处理点(%lu, %lu), 写入点(%lu), 输出点(%lu, %lu)\n",
                       state->process_slice_index, state->processed_count,
                       state->write_slice_index, state->output_slice_num, state->output_line_count);
        } else {
            verbose_printf(cfg, 1, "索引文件格式不正确,将从头开始。\n");
            // 如果读取失败,则重置为初始状态
            state->process_slice_index = 0;
            state->processed_count = 0;
            state->write_slice_index = 0;
            state->output_slice_num = 1; // 输出文件通常从1开始编号
            state->output_line_count = 0;
        }
        fclose(index_fp);
    } else {
        verbose_printf(cfg, 1, "未找到进度索引文件,将从头开始。\n");
        // 如果文件不存在,则为初始状态
        state->process_slice_index = 0;
        state->processed_count = 0;
        state->write_slice_index = 0;
        state->output_slice_num = 1;
        state->output_line_count = 0;
    }
    // state->process_slice_index = 0;

    // 根据恢复的 write_slice_index 打开正确的进度写入文件
    // 注意：总是以追加模式 'a' 打开,这样即使文件已存在也能继续写入
    char *write_slice_name = get_slice_filename(cfg->progress_base, state->write_slice_index);
    state->write_slice_file = fopen(write_slice_name, "ab");
    if (!state->write_slice_file) {
        perror("无法打开用于写入的进度分片文件");
        free(write_slice_name);
        free(idx_file);
        exit(EXIT_FAILURE);
    }
    verbose_printf(cfg, 1, "准备向进度文件 %s 追加新路径...\n", write_slice_name);
    
    // 初始化总行数,用于切换分片。line_count现在代表当前写入分片的行数。
    // 为了简化,我们可以在每次打开时重新计算,或者在record_path中维护。
    // 这里我们依赖 record_path 中的逻辑,只需确保文件被正确打开。
    fseek(state->write_slice_file, 0, SEEK_END);
    long size = ftell(state->write_slice_file);
    // 这是一个估算,假设平均行长,更精确的做法是在record_path中管理
    state->line_count = (size > 0) ? (state->write_slice_index * cfg->progress_slice_lines + 1) : 0; 

    free(write_slice_name);
    free(idx_file);
}


// 记录路径（二进制 Inode 方案）
void record_path(const Config *cfg, RuntimeState *state, const char *path, const struct stat *info) {
    if (!state->write_slice_file) return;
    
    // 写入格式: 路径长度 (size_t), 路径字符串 (char*), dev_t, ino_t
    size_t path_len = strlen(path);
    fwrite(&path_len, sizeof(size_t), 1, state->write_slice_file);
    fwrite(path, 1, path_len, state->write_slice_file);
    fwrite(&info->st_dev, sizeof(dev_t), 1, state->write_slice_file);
    fwrite(&info->st_ino, sizeof(ino_t), 1, state->write_slice_file);
    
    fflush(state->write_slice_file);
    state->line_count++; // 这里的 line_count 语义上是 "记录数"
    
    // 检查是否需要切换分片
    if (state->line_count % cfg->progress_slice_lines == 0) {
        fclose(state->write_slice_file);
        state->write_slice_file = NULL;
        
        state->write_slice_index++;
        char *new_slice = get_slice_filename(cfg->progress_base, state->write_slice_index);
        // 以 "ab" (append binary) 模式打开
        state->write_slice_file = fopen(new_slice, "ab"); 
        if (!state->write_slice_file) {
            fprintf(stderr, "无法创建新的进度分片文件%s", new_slice);
            exit(EXIT_FAILURE);
        }
        free(new_slice);
        atomic_update_index(cfg, state);
    }
}

void restore_progress(const Config *cfg, SmartQueue *queue, RuntimeState *state) {
    verbose_printf(cfg, 1, "开始从二进制进度文件恢复任务队列...\n");

    // 阶段一：构建历史对象标识符集合
    verbose_printf(cfg, 2, "阶段一：构建历史对象标识符集合...\n");
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
        
        // === 新增：分片内记录计数器 ===
        unsigned long record_idx_in_slice = 0; 
        // ============================

        while (fread(&path_len, sizeof(size_t), 1, slice_fp) == 1) {
            fseek(slice_fp, path_len, SEEK_CUR);
            
            if (fread(&dev, sizeof(dev_t), 1, slice_fp) == 1 &&
                fread(&ino, sizeof(ino_t), 1, slice_fp) == 1) {
                
                if (g_history_object_set) {
                    // === 核心修复逻辑 ===
                    bool is_truly_processed = false;
                    
                    // 1. 如果是之前的旧分片，那肯定是全处理完了
                    if (s_idx < state->process_slice_index) {
                        is_truly_processed = true;
                    } 
                    // 2. 如果是当前分片，只有索引小于 processed_count 的才是处理完的
                    else if (s_idx == state->process_slice_index) {
                        if (record_idx_in_slice < state->processed_count) {
                            is_truly_processed = true;
                        }
                    }
                    // 3. 未来的分片（如果有）或者当前分片靠后的记录，都是“待处理”的，不能加黑名单！

                    if (is_truly_processed) {
                        ObjectIdentifier id = { .st_dev = dev, .st_ino = ino };
                        hash_set_insert(g_history_object_set, &id);
                    }
                    // ===================
                }
            } else {
                verbose_printf(cfg, 1, "警告: 进度文件 %s 可能已损坏，提前中止读取。\n", slice_path);
                break;
            }
            // 别忘了递增计数器
            record_idx_in_slice++;
        }
        fclose(slice_fp);
        free(slice_path);
    }
    verbose_printf(cfg, 2, "历史对象集合构建完成，总数: %zu\n", g_history_object_set ? g_history_object_set->element_count : 0);

    // 阶段二：将未完成的任务加入队列
    verbose_printf(cfg, 2, "阶段二：加载未处理的任务到队列...\n");
    for (unsigned long current_slice = state->process_slice_index; ; ++current_slice) {
        char *slice_path = get_slice_filename(cfg->progress_base, current_slice);
        FILE *slice_fp = fopen(slice_path, "rb");

        if (!slice_fp) {
            verbose_printf(cfg, 1, "未找到更多进度分片(%s),恢复过程结束。\n", slice_path);
            free(slice_path);
            break;
        }

        verbose_printf(cfg, 1, "正在从分片 %s 恢复...\n", slice_path);
        
        char path_buffer[MAX_PATH_LENGTH];
        size_t path_len;
        dev_t dev;
        ino_t ino;

        // 跳过已处理的记录
        if (current_slice == state->process_slice_index && state->processed_count > 0) {
            for (unsigned long i = 0; i < state->processed_count; ++i) {
                if (fread(&path_len, sizeof(size_t), 1, slice_fp) != 1) goto next_slice;
                fseek(slice_fp, path_len + sizeof(dev_t) + sizeof(ino_t), SEEK_CUR);
            }
        }

        // 读取剩余的待处理任务
        while (fread(&path_len, sizeof(size_t), 1, slice_fp) == 1) {
            if (path_len >= MAX_PATH_LENGTH || 
                fread(path_buffer, 1, path_len, slice_fp) != path_len ||
                fread(&dev, sizeof(dev_t), 1, slice_fp) != 1 ||
                fread(&ino, sizeof(ino_t), 1, slice_fp) != 1) {
                verbose_printf(cfg, 1, "警告: 读取待处理任务失败，文件 %s 可能已损坏。\n", slice_path);
                break;
            }
            path_buffer[path_len] = '\0'; // 添加字符串结束符
            
            ScanNode *entry = safe_malloc(sizeof(ScanNode));
            entry->path = strdup(path_buffer);
            entry->next = NULL;
            add_to_buffer(queue, entry);
        }

    next_slice:
        fclose(slice_fp);
        free(slice_path);
        if(queue->active_count + queue->buffer_count >= queue->max_mem_items) {
            verbose_printf(cfg, 3, "内存队列已满 (active: %zu, buffer: %zu), 刷新缓存区到磁盘...\n",
                           queue->active_count, queue->buffer_count);
            flush_buffer_to_disk(cfg, queue);
        }
        if (queue->active_count < queue->low_watermark) {
            refill_active(cfg, queue);
        }
    }
    
    state->completed_count = state->process_slice_index * cfg->progress_slice_lines + state->processed_count;
    if (queue->active_count < queue->low_watermark) {
        refill_active(cfg, queue);
    }
    verbose_printf(cfg, 1, "进度恢复完成。队列状态: %zu 执行区, %zu 缓存区, %zu 磁盘项。总计已完成 %lu 项。\n",
               queue->active_count, queue->buffer_count, queue->disk_count, state->completed_count);
}

void refresh_progress(const Config *cfg, RuntimeState *state) {
    if (!cfg->continue_mode) return;

    state->processed_count++;
    state->completed_count++;

    // 每处理 PROGRESS_BATCH_SIZE 个条目就更新一次索引文件,以减少断电损失
    if (state->processed_count % PROGRESS_BATCH_SIZE == 0) {
        atomic_update_index(cfg, state);
    }

    // 检查当前处理的切片是否已完成
    if (state->processed_count >= cfg->progress_slice_lines) {
        verbose_printf(cfg, 1, "已完成处理分片 %lu。\n", state->process_slice_index);
        
        // 如果设置了归档或清理,则处理已完成的旧分片
        if (cfg->archive || cfg->clean) {
            char *finished_slice = get_slice_filename(cfg->progress_base, state->process_slice_index);
            if (cfg->archive) {
                archive_slice(cfg, finished_slice);
                verbose_printf(cfg, 1, "已归档分片: %s\n", finished_slice);
            }
            // -Z(archive) 和 -C(clean) 都会删除原文件
            remove(finished_slice);
            verbose_printf(cfg, 1, "已删除分片: %s\n", finished_slice);
            free(finished_slice);
        }

        // 移动到下一个切片,并将处理行号重置为0
        state->process_slice_index++;
        state->processed_count = 0;
        
        // 立即更新索引,记录我们已经进入了新切片的起点
        atomic_update_index(cfg, state);
    }
}


void cleanup_progress(const Config *cfg, RuntimeState *state) {
    if (!cfg->continue_mode) return;
    
    if (state->progress_file) {
        fclose(state->progress_file);
        state->progress_file = NULL;
    }
    
    atomic_update_index(cfg, state);
    
    if (state->lock_fd != -1) {
        struct flock fl = { .l_type = F_UNLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
        fcntl(state->lock_fd, F_SETLK, &fl);
        close(state->lock_fd);
        state->lock_fd = -1;
    }
    
    char *paths_file = get_paths_filename(cfg->progress_base);
    char *idx_file = get_index_filename(cfg->progress_base);
    
    remove(paths_file);
    remove(idx_file);
    
    free(paths_file);
    free(idx_file);
}


// 修改索引文件更新函数
// 索引格式：处理切片号 处理切片行号 写入切片号 输出文件号 输出文件行号
void atomic_update_index(const Config *cfg, RuntimeState *state) {
    char *idx_file = get_index_filename(cfg->progress_base);
    char *tmp_file = safe_malloc(strlen(idx_file) + 5);
    snprintf(tmp_file, strlen(idx_file) + 5, "%s.tmp", idx_file);
    
    FILE *tmp_fp = fopen(tmp_file, "w");
    if (tmp_fp) {
        // 索引格式：处理切片号 处理切片行号 写入切片号 输出文件号 输出文件行号
        fprintf(tmp_fp, "%lu %lu %lu %lu %lu\n", 
                state->process_slice_index, 
                state->processed_count, // 注意：此行号是当前正在处理的切片内的行号
                state->write_slice_index,
                state->output_slice_num,
                state->output_line_count);
        fclose(tmp_fp);
        
        // 原子性重命名
        if (rename(tmp_file, idx_file) != 0) {
            perror("无法重命名索引文件");
        }
    } else {
        perror("无法创建临时索引文件");
    }
    
    free(idx_file);
    free(tmp_file);
}


// 归档分片文件
void archive_slice(const Config *cfg, const char *slice_path) {
    char archive_path[1024];
    snprintf(archive_path, sizeof(archive_path), "%s.paths.gz", cfg->progress_base);
    
    gzFile archive = gzopen(archive_path, "ab");  // 追加模式
    if (!archive) {
        perror("无法打开归档文件");
        return;
    }
    
    FILE *slice = fopen(slice_path, "rb");
    if (!slice) {
        perror("无法打开分片文件");
        gzclose(archive);
        return;
    }
    
    char buffer[16384];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), slice)) > 0) {
        if (gzwrite(archive, buffer, bytes_read) == 0) {
            fprintf(stderr, "写入归档文件失败: %s\n", gzerror(archive, NULL));
            break;
        }
    }
    
    fclose(slice);
    gzclose(archive);
}

// 解压缩工具功能
void decompress_archive(const Config *cfg) {
    char archive_path[1024];
    snprintf(archive_path, sizeof(archive_path), "%s.paths.gz", cfg->progress_base);
    
    gzFile archive = gzopen(archive_path, "rb");
    if (!archive) {
        perror("无法打开归档文件");
        return;
    }
    
    char buffer[16384];
    int bytes_read;
    while ((bytes_read = gzread(archive, buffer, sizeof(buffer))) > 0) {
        if (fwrite(buffer, 1, bytes_read, stdout) != (size_t)bytes_read) {
            perror("写入输出失败");
            break;
        }
    }
    
    if (bytes_read < 0) {
        fprintf(stderr, "解压缩失败: %s\n", gzerror(archive, NULL));
    }
    
    gzclose(archive);
}


// acquire_lock 函数 - PID检查与flock混合实现
int acquire_lock(const Config *cfg, RuntimeState *state) {
    // 步骤 0: 确定锁文件路径
    char *lock_file_path = get_paths_filename(cfg->progress_base); 
    int fd = open(lock_file_path, O_RDWR | O_CREAT, 0644);

    if (fd == -1) {
        perror("无法创建或打开锁文件");
        free(lock_file_path);
        return -1;
    }

    // 步骤 1: PID检查 (现在是唯一的仲裁机制)
    lseek(fd, 0, SEEK_SET); 
    char buffer[32] = {0};
    ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);

    if (bytes_read > 0) {
        pid_t old_pid = atoi(buffer);
        if (old_pid > 0 && old_pid != getpid()) {
            if (kill(old_pid, 0) == 0 || errno == EPERM) {
                fprintf(stderr, "错误: 检测到另一个实例正在运行 (PID: %d)，退出。\n", old_pid);
                close(fd);
                free(lock_file_path);
                return -1;
            } else if (errno == ESRCH) {
                verbose_printf(cfg, 1, "检测到由已崩溃进程 (PID: %d) 留下的僵尸锁。\n", old_pid);
            }
        }
    }

    // 步骤 2: fcntl 锁逻辑已根据您的请求移除。

    // 步骤 3: 成功通过PID检查，清空并写入新的PID
    if (ftruncate(fd, 0) != 0) {
         perror("无法清空锁文件");
         close(fd);
         free(lock_file_path);
         return -1;
    }

    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
    lseek(fd, 0, SEEK_SET); // 写之前回到文件头
    if (write(fd, pid_str, strlen(pid_str)) == -1) {
        perror("无法写入PID到锁文件");
        close(fd);
        free(lock_file_path);
        return -1;
    }
    
    verbose_printf(cfg, 1, "成功获取PID文件锁 (PID: %d)\n", getpid());
    state->lock_file_path = lock_file_path;
    register_locked_file(fd, state->lock_file_path, true); // 仅注册主锁，用于信号处理
    return fd;
}


void release_lock(RuntimeState *state) {
    if (state->lock_fd != -1) {
        // fcntl 解锁逻辑已根据您的请求移除。
        
        // 关闭文件描述符
        close(state->lock_fd);
        state->lock_fd = -1;

        // 删除 PID 文件以释放应用层锁
        if (state->lock_file_path) {
            unregister_locked_file(state->lock_fd); // 在删除文件前注销
            if (unlink(state->lock_file_path) != 0) {
                perror("警告：无法删除锁文件");
            }
            free(state->lock_file_path);
            state->lock_file_path = NULL;
        }
    }
}


int is_symlink_loop(const Config *cfg, const char *path, dev_t dev, ino_t ino) {
    static struct {
        dev_t dev;
        ino_t ino;
    } visited[MAX_SYMLINK_DEPTH];
    
    static int depth = 0;
    
    for (int i = 0; i < depth; i++) {
        if (visited[i].dev == dev && visited[i].ino == ino) {
            verbose_printf(cfg, 1,"符号链接环路: %s\n", path);
            return 1;
        }
    }
    
    if (depth >= MAX_SYMLINK_DEPTH) {
        verbose_printf(cfg, 1, "符号链接深度超过限制: %s\n", path);
        return 1;
    }
    
    visited[depth].dev = dev;
    visited[depth].ino = ino;
    depth++;
    
    return 0;
}

// 获取配置文件的路径
char* get_config_filename(const char* base) {
    char* name = safe_malloc(strlen(base) + 5);
    snprintf(name, strlen(base) + 5, "%s.cfg", base);
    return name;
}


// 将关键配置保存到磁盘
void save_config_to_disk(const Config* cfg) {
    char* config_path = get_config_filename(cfg->progress_base);
    FILE* fp = fopen(config_path, "w");
    if (!fp) {
        perror("警告：无法写入配置文件，强制解锁功能可能受限");
        free(config_path);
        return;
    }

    fprintf(fp, "progress_base=%s\n", cfg->progress_base);
    fprintf(fp, "is_output_file=%d\n", cfg->is_output_file);
    if (cfg->output_file) {
        fprintf(fp, "output_file=%s\n", cfg->output_file);
    }
    fprintf(fp, "is_output_split_dir=%d\n", cfg->is_output_split_dir);
    if (cfg->output_split_dir) {
        fprintf(fp, "output_split_dir=%s\n", cfg->output_split_dir);
    }
    
    fclose(fp);
    free(config_path);
}

// 从磁盘加载配置
void load_config_from_disk(Config* cfg) {
    char* config_path = get_config_filename(cfg->progress_base);
    FILE* fp = fopen(config_path, "r");
    if (!fp) {
        // 如果配置文件不存在，可能是首次运行或被清理，这很正常。
        // 解锁工具将仅基于命令行提供的 -f 参数工作。
        printf("信息：未找到配置文件 %s，将仅根据命令行参数进行操作。\n", config_path);
        free(config_path);
        return;
    }

    printf("从 %s 加载上次运行的配置...\n", config_path);

    char line[1024];
    char key[256];
    char value[768];

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%[^=]=%[^\n]", key, value) == 2) {
            if (strcmp(key, "is_output_file") == 0) {
                cfg->is_output_file = atoi(value);
            } else if (strcmp(key, "output_file") == 0) {
                if(cfg->output_file) free(cfg->output_file);
                cfg->output_file = strdup(value);
            } else if (strcmp(key, "is_output_split_dir") == 0) {
                cfg->is_output_split_dir = atoi(value);
            } else if (strcmp(key, "output_split_dir") == 0) {
                if(cfg->output_split_dir) free(cfg->output_split_dir);
                cfg->output_split_dir = strdup(value);
            }
            // progress_base 通常由命令行传入，无需覆盖
        }
    }

    fclose(fp);
    free(config_path);
}

