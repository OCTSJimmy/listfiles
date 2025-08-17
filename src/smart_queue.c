#include "smart_queue.h"
#include "progress.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <time.h>
#include <fcntl.h>
#include <stdio.h>
#include "idempotency.h" // smart_enqueue 需要使用


// 将条目加入缓存区(内部函数)
void add_to_buffer(SmartQueue *queue, QueueEntry *entry) {
    entry->next = NULL;
    if (!queue->buffer_rear) {
        queue->buffer_front = queue->buffer_rear = entry;
    } else {
        queue->buffer_rear->next = entry;
        queue->buffer_rear = entry;
    }
    queue->buffer_count++;
}

// 将条目加入执行区(内部函数)
void add_to_active(SmartQueue *queue, QueueEntry *entry) {
    entry->next = NULL;
    if (!queue->active_rear) {
        queue->active_front = queue->active_rear = entry;
    } else {
        queue->active_rear->next = entry;
        queue->active_rear = entry;
    }
    queue->active_count++;
}


// 创建安全的临时目录
char *create_temp_dir(const Config *cfg) {
    char *temp_dir = safe_malloc(1024);
    snprintf(temp_dir, 1024, "/tmp/listfiles_%d_%ld", getpid(), time(NULL));
    verbose_printf(cfg, 1,"准备创建溢出记录目录%s\n", temp_dir);
    if (mkdir(temp_dir, 0700) == -1) {
        perror("无法创建临时目录");
        free(temp_dir);
        return NULL;
    }
    verbose_printf(cfg, 1,"创建溢出记录目录%s成功\n", temp_dir);
    // 创建锁文件防止被清理
    char lock_path[512];
    snprintf(lock_path, sizeof(lock_path), "%s/.lock", temp_dir);
    verbose_printf(cfg, 1,"准备创建溢出记录目录锁文件：%s\n", lock_path);
    int lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
    if (lock_fd == -1) {
        perror("无法创建目录锁");
    } else {
        close(lock_fd);
    }
    verbose_printf(cfg, 1,"准备创建溢出记录目录锁文件：%s成功\n", lock_path);
    return temp_dir;
}


// 将缓存区批量写入磁盘(内部函数)
void flush_buffer_to_disk(const Config *cfg, SmartQueue *queue) {
    if (!queue->buffer_front) return;
    verbose_printf(cfg, 1,"写入溢出记录文件%s\n", queue->overflow_name);
    // 确保有临时目录
    if (!queue->temp_dir) {
        queue->temp_dir = create_temp_dir(cfg);
        if (!queue->temp_dir) {
            exit(EXIT_FAILURE);
        }
    }
    verbose_printf(cfg, 1,"准备写入溢出记录文件，待写入条目数：%d\n", queue->buffer_count);
    if (!queue->overflow_file || queue->current_file_items >= queue->items_per_file) {
        // 关闭当前文件(如果存在)
        if (queue->overflow_file) {
            fclose(queue->overflow_file);
            queue->overflow_file = NULL;
        }

        // 创建新的溢出文件
        queue->overflow_file_count++;
        snprintf(queue->overflow_name, sizeof(queue->overflow_name),
                 "%s/overflow_%d_%ld.tmp", queue->temp_dir,
                 queue->overflow_file_count, time(NULL));
        queue->overflow_file = fopen(queue->overflow_name, "ab");
        if (!queue->overflow_file) {
            perror("无法创建溢出文件");
            exit(EXIT_FAILURE);
        }
        queue->current_file_items = 0;
        verbose_printf(cfg, 1,"创建新的写入溢出记录文件%s\n", queue->overflow_name);
    }

    // 批量写入缓存区所有条目
    QueueEntry *current = queue->buffer_front;
    QueueEntry *next_entry;
    while (current) {
        size_t path_len = strlen(current->path);

        // 1. 写入路径长度
        fwrite(&path_len, sizeof(size_t), 1, queue->overflow_file);
        // 2. 写入路径字符串本身 (不包括末尾的 '\0')
        fwrite(current->path, sizeof(char), path_len, queue->overflow_file);
        // 3. 写入stat结构体
        // fwrite(&current->info, sizeof(struct stat), 1, queue->overflow_file);

        queue->current_file_items++;

        // 释放已写入磁盘的条目内存
        next_entry = current->next;
        free(current->path);
        free(current);
        current = next_entry;
    }
    fflush(queue->overflow_file);
    
    // 更新磁盘计数
    queue->disk_count += queue->buffer_count;
    
    // 清空缓存区
    queue->buffer_front = NULL;
    queue->buffer_rear = NULL;
    queue->buffer_count = 0;
    verbose_printf(cfg, 1,"写入溢出记录文件%s已完成, 磁盘缓冲区:%d\n", queue->overflow_name, queue->disk_count);
}

// 从磁盘加载一批条目到执行区(内部函数) - 修正版
void load_batch_from_disk(const Config *cfg, SmartQueue *queue) {
    if (queue->disk_count == 0) return;
    
    // 如果当前文件句柄为空 (例如首次加载或上一个文件已读完)，则查找并打开下一个文件
    if (!queue->overflow_file) {
        verbose_printf(cfg, 5, "当前无打开的溢出文件，开始查找下一个...\n");
        // 查找下一个未处理的溢出文件 (选择最旧的一个以保证顺序)
        DIR *dir = opendir(queue->temp_dir);
        if (dir) {
            struct dirent *entry;
            char oldest_file[1024] = {0};
            time_t oldest_time = LLONG_MAX;

            while ((entry = readdir(dir)) != NULL) {
                if (strstr(entry->d_name, "overflow_") != NULL &&
                    strstr(entry->d_name, ".tmp") != NULL) {
                    
                    char file_path[2048];
                    snprintf(file_path, sizeof(file_path), "%s/%s",
                             queue->temp_dir, entry->d_name);

                    struct stat st;
                    if (stat(file_path, &st) == 0) {
                        if (st.st_mtime < oldest_time) {
                            oldest_time = st.st_mtime;
                            safe_strcpy(oldest_file, file_path, sizeof(oldest_file));
                        }
                    }
                }
            }
            closedir(dir);

            if (oldest_file[0] != '\0') {
                verbose_printf(cfg, 5, "找到最旧的溢出文件: %s\n", oldest_file);
                queue->overflow_file = fopen(oldest_file, "rb");
                if (queue->overflow_file) {
                    verbose_printf(cfg, 5, "成功打开溢出文件: %s\n", oldest_file);
                    safe_strcpy(queue->overflow_name, oldest_file, sizeof(queue->overflow_name));
                } else {
                    // 如果打开失败，打印错误并退出，防止无限循环
                    perror("无法打开找到的溢出文件");
                    return;
                }
            }
        }

        // 如果经过查找后，依然没有可打开的文件，说明磁盘上没有待处理项了
        if (!queue->overflow_file) {
            verbose_printf(cfg, 5, "没有更多未处理的溢出文件了。\n");
            // 确保磁盘计数清零
            if (queue->disk_count > 0) {
                verbose_printf(cfg, 1, "警告: 磁盘计数为 %zu 但未找到溢出文件，计数将重置为0。\n", queue->disk_count);
                queue->disk_count = 0;
            }
            return;
        }
    }

    verbose_printf(cfg, 5, "开始从 %s 加载数据到执行区...\n", queue->overflow_name);
    
    size_t loaded = 0;
    size_t batch_size = min_size(BUFFER_BATCH_SIZE, queue->disk_count);
    verbose_printf(cfg, 5, "本批次最大加载量: %zu\n", batch_size);
    
    size_t path_len;
    // ====================== 核心修改点 ======================
    // 将 fread 操作作为循环条件，这是处理二进制文件读取最健壮的方式。
    // 只有在成功读取一个 size_t 长度后，才会进入循环。
    while (loaded < batch_size && queue->disk_count > 0 && 
           fread(&path_len, sizeof(size_t), 1, queue->overflow_file) == 1) {
        
        QueueEntry *entry = safe_malloc(sizeof(QueueEntry));
        entry->path = safe_malloc(path_len + 1); // 为 '\0' 分配空间

        // 读取路径字符串和 stat 结构体
        if (fread(entry->path, sizeof(char), path_len, queue->overflow_file) != path_len) {
            
            verbose_printf(cfg, 1, "错误: 读取路径失败，文件 %s 可能已损坏。\n", queue->overflow_name);
            // 读取不完整，释放内存并跳出
            free(entry->path);
            free(entry);
            break; // 退出循环，外层逻辑会关闭并删除这个损坏的文件
        }
        
        entry->path[path_len] = '\0'; // 手动添加字符串结束符
        entry->next = NULL;
        
        // verbose_printf(cfg, 6, "加载条目: %s\n", entry->path); // 级别可以调高，避免刷屏
        add_to_active(queue, entry);
        loaded++;
        queue->disk_count--;
    }
    // =======================================================
    
    verbose_printf(cfg, 5, "本批次加载完成，共加载 %zu 条记录。\n", loaded);

    // 如果文件已经读到了末尾，则关闭并删除它
    if (feof(queue->overflow_file)) {
        verbose_printf(cfg, 5, "溢出文件 %s 已读到末尾，准备关闭并删除。\n", queue->overflow_name);
        fclose(queue->overflow_file);
        if (unlink(queue->overflow_name) == 0) {
             verbose_printf(cfg, 5, "成功删除文件: %s\n", queue->overflow_name);
        } else {
             perror("删除溢出文件失败");
        }
        queue->overflow_file = NULL;
        queue->overflow_name[0] = '\0';
    }
}


void append_buffer_to_active_O1(SmartQueue *queue) {
    // 如果 buffer 为空，什么也不做
    if (queue->buffer_count == 0) {
        return;
    }

    // 如果 active 队列不为空，则将其尾部连接到 buffer 的头部
    if (queue->active_rear) {
        queue->active_rear->next = queue->buffer_front;
    } else {
        // 如果 active 队列为空，则 buffer 直接成为 active 队列
        queue->active_front = queue->buffer_front;
    }

    // 更新 active 队列的尾部为 buffer 的尾部
    queue->active_rear = queue->buffer_rear;

    // 更新计数
    queue->active_count += queue->buffer_count;

    // 清空 buffer 队列的指针和计数（现在它已经“嫁接”到 active 队列了）
    queue->buffer_front = NULL;
    queue->buffer_rear = NULL;
    queue->buffer_count = 0;
}


// 检查并补充执行区(内部函数)
void refill_active(const Config *cfg, SmartQueue *queue) {
    // verbose_printf(cfg, 5, "检查并补充执行区 (active: %zu, buffer: %zu, disk: %zu)\n",
    //               queue->active_count, queue->buffer_count, queue->disk_count);

    // 循环补充，直到 active 队列达到低水位线，或者没有更多源可补充
    while (queue->active_count < queue->low_watermark) {
        // 优先从内存缓存区移动
        if (queue->buffer_count > 0) {
            append_buffer_to_active_O1(queue);
        } 
        // 如果缓存区为空，再从磁盘加载
        else if (queue->disk_count > 0) {
            load_batch_from_disk(cfg, queue);
            // 如果加载后 active 和 buffer 仍然为空，说明磁盘上的文件已读完或损坏，跳出循环
            if (queue->active_count == 0 && queue->buffer_count == 0) {
                break;
            }
        } 
        // 如果缓存和磁盘都为空，则无法补充，跳出循环
        else {
            break;
        }
    }
    // verbose_printf(cfg, 5, "补充执行区完成 (active: %zu)\n", queue->active_count);
}


// 初始化智能队列(优化版)
void init_smart_queue(SmartQueue *queue) {
    queue->active_front = NULL;
    queue->active_rear = NULL;
    queue->buffer_front = NULL;
    queue->buffer_rear = NULL;
    
    queue->active_count = 0;
    queue->buffer_count = 0;
    queue->disk_count = 0;
    
    queue->overflow_file = NULL;
    queue->overflow_name[0] = '\0';
    
    queue->max_mem_items = DEFAULT_MEM_ITEMS;
    queue->low_watermark = (size_t)(DEFAULT_MEM_ITEMS * LOW_WATERMARK_RATIO);
    queue->temp_dir = NULL;
    queue->overflow_file_count = 0;
    queue->items_per_file = 100000;  // 每个文件最多10万条记录
    queue->current_file_items = 0;
}


// 智能入队(优化版)
// void smart_enqueue(const Config *cfg, SmartQueue *queue, const char *path, const struct stat *info) {
void smart_enqueue(const Config *cfg, SmartQueue *queue, const char *path, const struct stat *info) {

    if (cfg->continue_mode && g_history_object_set) {
        ObjectIdentifier id = { .st_dev = info->st_dev, .st_ino = info->st_ino };
        
        // 检查对象标识符是否存在于历史集合中
        if (hash_set_contains(g_history_object_set, &id)) {
            verbose_printf(cfg, 5, "Idempotency check: Skipping duplicate object (dev:%ld, ino:%ld) at path %s\n", 
                           (long)id.st_dev, (long)id.st_ino, path);
            return;
        }
        // 如果不存在，将其添加到当前运行的历史集合中
        hash_set_insert(g_history_object_set, &id);
    }

    // verbose_printf(cfg, 5, "智能入队: %s\n", path);
    QueueEntry *entry = safe_malloc(sizeof(QueueEntry));
    entry->path = safe_malloc(strlen(path)+1);
    safe_strcpy(entry->path, path, strlen(path)+1);
    // memcpy(&entry->info, info, sizeof(struct stat));
    entry->next = NULL;

    add_to_buffer(queue, entry);
    // 新增逻辑：当内存中的条目总数超过阈值时，将缓存区刷新到磁盘
    if ((queue->active_count + queue->buffer_count) >= queue->max_mem_items) {
        verbose_printf(cfg, 3, "内存队列已满 (active: %zu, buffer: %zu), 刷新缓存区到磁盘...\n",
                       queue->active_count, queue->buffer_count);
        flush_buffer_to_disk(cfg, queue);
    }
}


// 智能出队(优化版)
QueueEntry *smart_dequeue(const Config *cfg, SmartQueue *queue, RuntimeState *state) {
    // 仅当 active 队列低于低水位线时，才尝试补充
    if (queue->active_count < queue->low_watermark) {
        refill_active(cfg, queue);
    }

    // 如果经过补充后，active 队列仍然为空，说明所有任务都已处理完毕
    if (!queue->active_front) {
        return NULL;
    }

    // 从 active 队列头部取出一个条目
    QueueEntry *entry = queue->active_front;
    queue->active_front = entry->next;
    if (!queue->active_front) {
        queue->active_rear = NULL;
    }
    queue->active_count--;

    // 更新进度
    if (cfg->continue_mode) {
        refresh_progress(cfg, state);
    }
    
    return entry;
}


// 清理智能队列(优化版)
void cleanup_smart_queue(SmartQueue *queue) {
    while (queue->active_front) {
        QueueEntry *entry = queue->active_front;
        queue->active_front = entry->next;
        free(entry->path);
        free(entry);
    }
    
    while (queue->buffer_front) {
        QueueEntry *entry = queue->buffer_front;
        queue->buffer_front = entry->next;
        free(entry->path);
        free(entry);
    }
    
    if (queue->overflow_file) {
        fclose(queue->overflow_file);
        unlink(queue->overflow_name);
    }

    if (queue->temp_dir) {
        DIR *dir = opendir(queue->temp_dir);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 || 
                    strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                
                char file_path[512];
                snprintf(file_path, sizeof(file_path), "%s/%s", 
                         queue->temp_dir, entry->d_name);
                remove(file_path);
            }
            closedir(dir);
        }
        
        remove(queue->temp_dir);
        free(queue->temp_dir);
        queue->temp_dir = NULL;
    }
}
