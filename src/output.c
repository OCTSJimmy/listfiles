#include "output.h"
#include "utils.h"
#include <string.h>
#include <stdlib.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include "async_worker.h"
#include <unistd.h>

// 辅助函数：将 st_mode 转换为 ls -l 格式的字符串
void format_mode_str(mode_t mode, char *buf) {
    strcpy(buf, "----------");
    const char chars[] = "rwxrwxrwx";
    if (S_ISREG(mode))  buf[0] = '-';
    else if (S_ISDIR(mode))  buf[0] = 'd';
    else if (S_ISLNK(mode))  buf[0] = 'l';
    else if (S_ISCHR(mode))  buf[0] = 'c';
    else if (S_ISBLK(mode))  buf[0] = 'b';
    else if (S_ISFIFO(mode)) buf[0] = 'p';
    else if (S_ISSOCK(mode)) buf[0] = 's';
    else buf[0] = '?';

    for (int i = 0; i < 9; i++) {
        buf[i + 1] = (mode & (1 << (8 - i))) ? chars[i] : '-';
    }
    if (mode & S_ISUID) buf[3] = (mode & S_IXUSR) ? 's' : 'S';
    if (mode & S_ISGID) buf[6] = (mode & S_IXGRP) ? 's' : 'S';
    if (mode & S_ISVTX) buf[9] = (mode & S_IXOTH) ? 't' : 'T';
    buf[10] = '\0';
}
// =======================================================
// lsattr 探测与缓存 (基于 RuntimeState)
// =======================================================
// === 新增：更新统计数据 ===
// 注意：此函数修改了 state，因此需要转换 const 指针
static void update_statistics(const RuntimeState *c_state) {
    RuntimeState *state = (RuntimeState *)c_state; // 移除 const
    Statistics *st = &state->stats;
    
    time_t now = time(NULL);
    
    // 限制采样频率：每秒只采一次，避免计算过于抖动
    if (now - st->last_sample_time < 1) {
        return;
    }
    
    // 1. 存入新样本 (Ring Buffer)
    st->head_idx = (st->head_idx + 1) % RATE_WINDOW_SIZE;
    if (st->head_idx == 0 && st->samples[0].timestamp != 0) {
        st->filled = true;
    }
    
    st->samples[st->head_idx].timestamp = now;
    st->samples[st->head_idx].dir_count = state->dir_count;
    st->samples[st->head_idx].file_count = state->file_count;
    st->samples[st->head_idx].dequeued_count = state->total_dequeued_count; // <--- 记录出队数
    st->last_sample_time = now;
    int tail_idx = st->filled ? (st->head_idx + 1) % RATE_WINDOW_SIZE : 0;
    RateSample *new_s = &st->samples[st->head_idx];
    RateSample *old_s = &st->samples[tail_idx];
    double time_diff = difftime(new_s->timestamp, old_s->timestamp);
    
   
    if (time_diff >= 1.0) {
        // 目录速率
        unsigned long dir_diff = new_s->dir_count - old_s->dir_count;
        st->current_dir_rate = (double)dir_diff / time_diff;
        if (st->current_dir_rate > st->max_dir_rate) st->max_dir_rate = st->current_dir_rate;

        // 文件速率
        unsigned long file_diff = new_s->file_count - old_s->file_count;
        st->current_file_rate = (double)file_diff / time_diff;
        if (st->current_file_rate > st->max_file_rate) st->max_file_rate = st->current_file_rate;

        unsigned long deq_diff = new_s->dequeued_count - old_s->dequeued_count;
        st->current_dequeue_rate = (double)deq_diff / time_diff;
        if (st->current_dequeue_rate > st->max_dequeue_rate) 
            st->max_dequeue_rate = st->current_dequeue_rate;
    } else {
        // 刚启动不足1秒，使用全局平均兜底
        // calculate_rate 是原来的全局算法，可以保留作为 fallback
        st->current_dir_rate = calculate_rate(state->start_time, state->dir_count);
        st->current_file_rate = calculate_rate(state->start_time, state->file_count);
    }
}

// 查找设备状态 (线程安全)
static DeviceStatus get_device_status(dev_t dev, RuntimeState *state) {
    DeviceStatus status = DEV_STATUS_UNKNOWN;
    
    pthread_mutex_lock(&state->dev_cache_mutex);
    for (size_t i = 0; i < state->dev_cache_count; i++) {
        if (state->dev_cache[i].dev == dev) {
            status = state->dev_cache[i].status;
            break;
        }
    }
    pthread_mutex_unlock(&state->dev_cache_mutex);
    
    return status;
}
// 更新设备状态 (线程安全)
static void set_device_status(dev_t dev, DeviceStatus status, RuntimeState *state) {
    pthread_mutex_lock(&state->dev_cache_mutex);
    
    // 双重检查：防止在等待锁的过程中已经被别的线程更新了
    for (size_t i = 0; i < state->dev_cache_count; i++) {
        if (state->dev_cache[i].dev == dev) {
            state->dev_cache[i].status = status;
            pthread_mutex_unlock(&state->dev_cache_mutex);
            return;
        }
    }

    if (state->dev_cache_count < MAX_DEV_CACHE) {
        state->dev_cache[state->dev_cache_count].dev = dev;
        state->dev_cache[state->dev_cache_count].status = status;
        state->dev_cache_count++;
    }
    
    pthread_mutex_unlock(&state->dev_cache_mutex);
}
// 获取 lsattr 字符串 (传入 state)
static void get_xattr_str(RuntimeState *state, const char *path, const struct stat *info, char *buf) {
    // 1. 检查设备缓存
    DeviceStatus ds = get_device_status(info->st_dev, state);
    if (ds == DEV_STATUS_UNSUPPORTED) {
        strcpy(buf, "[unsupported]   ");
        return;
    }

    // 2. 尝试打开
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        strcpy(buf, "[access_denied] "); 
        return;
    }

    // 3. 尝试 ioctl
    int flags = 0;
    if (ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0) {
        // 成功！说明设备支持。如果之前是未知状态，更新为支持
        if (ds == DEV_STATUS_UNKNOWN) {
            set_device_status(info->st_dev, DEV_STATUS_SUPPORTED, state);
        }
        
        // 解析标志位
        strcpy(buf, "----------------");
        if (flags & FS_SECRM_FL)        buf[0] = 's';
        if (flags & FS_UNRM_FL)         buf[1] = 'u';
        if (flags & FS_COMPR_FL)        buf[2] = 'c';
        if (flags & FS_SYNC_FL)         buf[3] = 'S';
        if (flags & FS_IMMUTABLE_FL)    buf[4] = 'i';
        if (flags & FS_APPEND_FL)       buf[5] = 'a';
        if (flags & FS_NODUMP_FL)       buf[6] = 'd';
        if (flags & FS_NOATIME_FL)      buf[7] = 'A';
        if (flags & FS_DIRTY_FL)        buf[8] = 'D';
        if (flags & FS_COMPRBLK_FL)     buf[9] = 'B';
        if (flags & FS_NOCOMP_FL)       buf[10] = 'Z';
        #ifdef FS_ECOMPR_FL
        if (flags & FS_ECOMPR_FL)       buf[11] = 'E';
        #endif
        if (flags & FS_INDEX_FL)        buf[12] = 'I';
        if (flags & FS_IMAGIC_FL)       buf[13] = 'i';
        if (flags & FS_JOURNAL_DATA_FL) buf[14] = 'j';
        if (flags & FS_NOTAIL_FL)       buf[15] = 't';
    } else {
        // ioctl 失败
        if (errno == ENOTTY || errno == EOPNOTSUPP) {
            // 明确不支持，更新缓存
            set_device_status(info->st_dev, DEV_STATUS_UNSUPPORTED, state);
            strcpy(buf, "[unsupported]   ");
        } else {
            strcpy(buf, "[ioctl_error]   ");
        }
    }
    close(fd);
}

// 获取用户名(哈希表实现)
const char *get_username(uid_t uid, RuntimeState *state) {
    // 计算哈希桶索引
    unsigned bucket = uid % UID_CACHE_SIZE;
    
    // 在桶中查找
    UserCacheEntry *entry = state->uid_cache[bucket];
    while (entry) {
        if (entry->uid == uid) {
            return entry->name;
        }
        entry = entry->next;
    }
    
    // 缓存未命中,查询系统
    struct passwd *pw = getpwuid(uid);
char temp_buf[256]; // 临时缓冲区，足够容纳 name(uid)

    if (pw) {
        // 找到了：格式化为 "root(0)" 或 "www-data(33)"
        snprintf(temp_buf, sizeof(temp_buf), "%s(%u)", pw->pw_name, (unsigned int)uid);
    } else {
        // 没找到：直接保留原始 UID，例如 "1005"
        snprintf(temp_buf, sizeof(temp_buf), "%u", (unsigned int)uid);
    }
    
    // 创建新缓存项
    UserCacheEntry *new_entry = safe_malloc(sizeof(UserCacheEntry));
    new_entry->uid = uid;
    new_entry->name = safe_malloc(strlen(temp_buf) + 1);
    strcpy(new_entry->name, temp_buf);
    
    // 添加到桶的头部
    new_entry->next = state->uid_cache[bucket];
    state->uid_cache[bucket] = new_entry;
    state->uid_cache_count++;
    
    return new_entry->name;
}

// 获取组名(哈希表实现)
const char *get_groupname(gid_t gid, RuntimeState *state) {
    // 计算哈希桶索引
    unsigned bucket = gid % GID_CACHE_SIZE;
    
    // 在桶中查找
    GroupCacheEntry *entry = state->gid_cache[bucket];
    while (entry) {
        if (entry->gid == gid) {
            return entry->name;
        }
        entry = entry->next;
    }
    
    // 缓存未命中,查询系统
    struct group *gr = getgrgid(gid);
    char temp_buf[256];
    if (gr) {
        // 找到了：格式化为 "root(0)"
        snprintf(temp_buf, sizeof(temp_buf), "%s(%u)", gr->gr_name, (unsigned int)gid);
    } else {
        // 没找到：保留原始 GID，例如 "1005"
        snprintf(temp_buf, sizeof(temp_buf), "%u", (unsigned int)gid);
    }
    
    // 创建新缓存项
    GroupCacheEntry *new_entry = safe_malloc(sizeof(GroupCacheEntry));
    new_entry->gid = gid;
    new_entry->name = safe_malloc(strlen(temp_buf) + 1);
    strcpy(new_entry->name, temp_buf);
    
    // 添加到桶的头部
    new_entry->next = state->gid_cache[bucket];
    state->gid_cache[bucket] = new_entry;
    state->gid_cache_count++;
    
    return new_entry->name;
}



/**********************
 * 格式清理函数
 **********************/
void cleanup_compiled_format(Config *cfg) {
    if (!cfg->compiled_format) return;
    
    for (int i = 0; i < cfg->format_segment_count; i++) {
        if (cfg->compiled_format[i].type == FMT_TEXT) {
            free(cfg->compiled_format[i].text);
        }
    }
    free(cfg->compiled_format);
    cfg->compiled_format = NULL;
    cfg->format_segment_count = 0;
}

// 入队效率
double calculate_rate(time_t start_time, unsigned long count) {
    time_t current_time = time(NULL);
    double elapsed = difftime(current_time, start_time);
    if (elapsed < 1.0) return 0.0;  // 避免除以零
    return (double)count / elapsed;
}


// 计算已执行时间并格式化
static void format_elapsed_time(time_t start_time, char *buffer, size_t buf_size) {
    time_t current_time = time(NULL);
    long elapsed = difftime(current_time, start_time);
    
    int days = elapsed / 86400;
    elapsed %= 86400;
    int hours = elapsed / 3600;
    elapsed %= 3600;
    int minutes = elapsed / 60;
    int seconds = elapsed % 60;
    
    snprintf(buffer, buf_size, "%d:%02d:%02d:%02d", days, hours, minutes, seconds);
}

/**********************
 * 格式预编译实现
 **********************/
void precompile_format(Config *cfg) {
    if (!cfg->format) return;
    
    char *fmt = cfg->format;
    cfg->compiled_format = safe_malloc(256 * sizeof(FormatSegment)); // 最多256段
    cfg->format_segment_count = 0;
    
    char *segment_start = fmt;
    int segment_length = 0;
    
    while (*fmt) {
        if (*fmt == '%') {
            // 处理之前的文本段
            if (segment_length > 0) {
                FormatSegment seg;
                seg.type = FMT_TEXT;
                seg.text = safe_malloc(segment_length + 1);
                strncpy(seg.text, segment_start, segment_length);
                seg.text[segment_length] = '\0';
                cfg->compiled_format[cfg->format_segment_count++] = seg;
                segment_length = 0;
            }
            
            // 处理占位符
            FormatSegment seg;
            switch (*(++fmt)) {
                case 'p': seg.type = FMT_PATH; break;
                case 's': seg.type = FMT_SIZE; break;
                case 'u': seg.type = FMT_USER; break;
                case 'g': seg.type = FMT_GROUP; break;
                case 'm': seg.type = FMT_MTIME; break;
                case 'a': seg.type = FMT_ATIME; break;
                case 'M': seg.type = FMT_MODE; break;
                case 'X': seg.type = FMT_XATTR; break;
                default:  // 无效占位符当作普通文本
                    seg.type = FMT_TEXT;
                    seg.text = safe_malloc(3);
                    seg.text[0] = '%';
                    seg.text[1] = *fmt;
                    seg.text[2] = '\0';
                    cfg->compiled_format[cfg->format_segment_count++] = seg;
                    fmt++;
                    segment_start = fmt;
                    continue;
            }
            seg.text = NULL;
            cfg->compiled_format[cfg->format_segment_count++] = seg;
            segment_start = ++fmt;
        } else {
            segment_length++;
            fmt++;
        }
    }
    
    // 处理最后的文本段
    if (segment_length > 0) {
        FormatSegment seg;
        seg.type = FMT_TEXT;
        seg.text = safe_malloc(segment_length + 1);
        strncpy(seg.text, segment_start, segment_length);
        seg.text[segment_length] = '\0';
        cfg->compiled_format[cfg->format_segment_count++] = seg;
    }
}

// 在创建新输出文件或切换切片时加锁
FILE* create_output_file(const char *path) {
    FILE *fp = fopen(path, "a");
    if (!fp) {
        fprintf(stderr, "创建输出文件%s失败", path);
        return NULL;
    }
    return fp;
}

// 关闭文件前解锁(或依赖自动解锁,显式解锁更安全)
void close_output_file(FILE *fp) {
    if (!fp || fp == stdout || fp == stderr) return;
    fclose(fp);
}

// 新增输出文件管理函数
void init_output_files(const Config *cfg, RuntimeState *state) {
    // 处理目录信息输出文件
    // 初始化输出文件
    state->output_line_count = 0;
    state->output_slice_num = 1;
    state->start_time = time(NULL);
    state->completed_count = 0;
    state->current_path = NULL;
    state->lock_file_path = NULL;

    if (cfg->is_output_split_dir && cfg->output_split_dir) {
        // 创建输出目录
        
        if(mkdir(cfg->output_split_dir, 0700) == -1) {
            // 检查错误类型,如果是目录已存在则继续执行
            if (errno != EEXIST) {
                perror("无法创建输出目录");
                exit(EXIT_FAILURE);
            }
            // 目录已存在,输出提示信息
            verbose_printf(cfg, 1, "输出目录已存在: %s\n", cfg->output_split_dir);
        } else {
            // 目录创建成功
            verbose_printf(cfg, 1, "成功创建输出目录: %s\n", cfg->output_split_dir);
        }
        // 打开第一个输出切片
        char slice_path[1024];
        snprintf(slice_path, sizeof(slice_path), "%s/" OUTPUT_SLICE_FORMAT,
                cfg->output_split_dir, state->output_slice_num);
        state->output_fp = create_output_file(slice_path);
        if (state->output_fp) {
            verbose_printf(cfg, 1,"打开输出文件: %s\n", slice_path);
        } else {
            verbose_printf(cfg, 1,"打开输出文件: %s失败, 自动转为屏幕(stdout)输出\n", slice_path);
            state->output_fp = stdout;
        }
    } else if (cfg->is_output_file && cfg->output_file) {
        state->output_fp = create_output_file(cfg->output_file);
        if(state->output_fp) {
            verbose_printf(cfg, 1,"打开输出文件: %s\n", cfg->output_file);
        } else {
            verbose_printf(cfg, 1,"打开输出文件: %s失败, 自动转为屏幕(stdout)输出\n", cfg->output_file);
            state->output_fp = stdout;
        }
    } else {
        verbose_printf(cfg, 1,"使用标准输出\n");
        state->output_fp = stdout;  // 默认输出到标准输出
    }

    if (!state->output_fp) {
        perror("无法打开输出文件");
        exit(EXIT_FAILURE);
    }


    if (cfg->print_dir) {
        char *dir_filename;
        if (cfg->is_output_file && cfg->output_file) {
            dir_filename = safe_malloc(strlen(cfg->output_file) + 5);
            snprintf(dir_filename, strlen(cfg->output_file) + 5, "%s.dir", cfg->output_file);
            verbose_printf(cfg, 1,"目录信息文件: %s\n", dir_filename);
            state->dir_info_fp = create_output_file(dir_filename);
            if(state->dir_info_fp) {
                verbose_printf(cfg, 1,"打开目录信息文件: %s\n", dir_filename);
            } else {
                verbose_printf(cfg, 1,"打开目录信息文件: %s失败, 自动转为标准错误(stderr)输出\n", dir_filename);
                state->dir_info_fp = stderr;
            }
            free(dir_filename);
        } else if (cfg->is_output_split_dir && cfg->output_split_dir) {
            dir_filename = safe_malloc(strlen(cfg->output_split_dir) + 20);
            snprintf(dir_filename, strlen(cfg->output_split_dir) + 20, 
                    "%s/output.dir", cfg->output_split_dir);
            verbose_printf(cfg, 1,"目录信息文件: %s\n", dir_filename);
            state->dir_info_fp = create_output_file(dir_filename);
            if(state->dir_info_fp) {
                verbose_printf(cfg, 1,"打开目录信息文件: %s\n", dir_filename);
            } else {
                verbose_printf(cfg, 1,"打开目录信息文件: %s失败, 自动转为标准错误(stderr)输出\n", dir_filename);
                state->dir_info_fp = stderr;
            }
            free(dir_filename);
        } else {
            // dir_filename = strdup("output.dir");
            verbose_printf(cfg, 1, "使用标准错误输出\n");
            state->dir_info_fp = stderr;
        }
        // verbose_printf(cfg, 1,"目录信息文件: %s\n", dir_filename);
    }
}

void rotate_output_slice(const Config *cfg, RuntimeState *state) {
    if (!cfg->is_output_split_dir) return;
    verbose_printf(cfg, 1,"切换输出切片文件\n");
    // 关闭当前切片
    if (state->output_fp) {
        // 解锁文件
        close_output_file(state->output_fp);
        state->output_fp = NULL;
        verbose_printf(cfg, 1,"关闭当前输出切片文件\n");
    }

    // 递增切片编号并创建新文件
    state->output_slice_num++;
    verbose_printf(cfg, 1,"递增切片编号: %lu\n", state->output_slice_num);
    char slice_path[1024];
    snprintf(slice_path, sizeof(slice_path), "%s/" OUTPUT_SLICE_FORMAT,
            cfg->output_split_dir, state->output_slice_num);
    state->output_fp = create_output_file(slice_path);
    if (!state->output_fp) {
        perror("无法创建新的输出切片文件");
        exit(EXIT_FAILURE);
    } else {
        verbose_printf(cfg, 1,"打开新切片文件: %s\n", slice_path);
    }
    state->output_line_count = 0;
}


void format_output(const Config *cfg, RuntimeState *state, const char *path, const struct stat *info) {
    FILE *output = state->output_fp ? state->output_fp : stdout;
    // verbose_printf(cfg, 1, "输出装置：%s(%s)\n", state->output_fp ? "文件" : "屏幕", state->output_fp ? cfg->output_file : "stdout");
    // 使用预编译格式输出
// 在写入文件前加写锁
    bool q = cfg->quote;
    if (cfg->compiled_format) {
for (int i = 0; i < cfg->format_segment_count; i++) {
            switch (cfg->compiled_format[i].type) {
                case FMT_TEXT:
                    // 文本段（分隔符）不加引号
                    fputs(cfg->compiled_format[i].text, output);
                    break;
                case FMT_PATH:
                    if (q) fputc('"', output);
                    fputs(path, output);
                    if (q) fputc('"', output);
                    break;
                case FMT_SIZE:
                    if (q) fputc('"', output);
                    fprintf(output, "%lld", (long long)info->st_size);
                    if (q) fputc('"', output);
                    break;
                case FMT_USER:
                    if (q) fputc('"', output);
                    fputs(get_username(info->st_uid, state), output);
                    if (q) fputc('"', output);
                    break;
                case FMT_GROUP:
                    if (q) fputc('"', output);
                    fputs(get_groupname(info->st_gid, state), output);
                    if (q) fputc('"', output);
                    break;
                case FMT_MTIME:
                    if (q) fputc('"', output);
                    fputs(format_time(info->st_mtime), output);
                    if (q) fputc('"', output);
                    break;
                case FMT_ATIME:
                    if (q) fputc('"', output);
                    fputs(format_time(info->st_atime), output);
                    if (q) fputc('"', output);
                    break;
                case FMT_MODE: {
                    char mode_str[11];   
                    format_mode_str(info->st_mode, mode_str);
                    if (q) fputc('"', output);
                    fprintf(output, "%s", mode_str);
                    if (q) fputc('"', output);
                    break;
                }
                case FMT_XATTR: {
                    char attr_str[32];
                    get_xattr_str(state, path, info, attr_str);
                    if (q) fputc('"', output);
                    fputs(attr_str, output);
                    if (q) fputc('"', output);
                    break;
                }
            }
        }
        fputc('\n', output);
    } else {
// 默认模式：按需加引号
        if (q) fprintf(output, "\"%s\"", path);
        else fprintf(output, "%s", path);

        if (cfg->size) {
            if (q) fprintf(output, " \"%lld\"", (long long)info->st_size);
            else fprintf(output, " %lld", (long long)info->st_size);
        }
        if (cfg->user) {
            if (q) fprintf(output, " \"%s\"", get_username(info->st_uid, state));
            else fprintf(output, " %s", get_username(info->st_uid, state));
        }
        if (cfg->group) {
            if (q) fprintf(output, " \"%s\"", get_groupname(info->st_gid, state));
            else fprintf(output, " %s", get_groupname(info->st_gid, state));
        }
        if (cfg->mtime) {
            if (q) fprintf(output, " \"%s\"", format_time(info->st_mtime));
            else fprintf(output, " %s", format_time(info->st_mtime));
        }
        if (cfg->atime) {
            if (q) fprintf(output, " \"%s\"", format_time(info->st_atime));
            else fprintf(output, " %s", format_time(info->st_atime));
        }
        if (cfg->mode) {
            char mode_str[11];
            format_mode_str(info->st_mode, mode_str);
            if (q) fprintf(output, " \"%s\"", mode_str);
            else fprintf(output, " %s", mode_str);
        }
        if (cfg->xattr) {
            char xattr_buf[32];
            get_xattr_str(state, path, info, xattr_buf);
            if (q) fprintf(output, " \"%s\"", xattr_buf);
            else fprintf(output, " %s", xattr_buf);
        }
        fputc('\n', output);
    }

    // 检查是否需要切换输出切片
    state->output_line_count++;
    if (cfg->output_split_dir && 
        state->output_line_count >= cfg->output_slice_lines) {
        rotate_output_slice(cfg, state);
    }
    if (output != NULL) {
       fflush(output);
    } else {
       perror("Attempted to fflush NULL pointer");
       exit(EXIT_FAILURE);
    }
}

// 清理缓存
void cleanup_cache(RuntimeState *state) {
    for (int i = 0; i < UID_CACHE_SIZE; i++) {
        UserCacheEntry *entry = state->uid_cache[i];
        while (entry) {
            UserCacheEntry *next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
        state->uid_cache[i] = NULL;
    }
    
    for (int i = 0; i < GID_CACHE_SIZE; i++) {
        GroupCacheEntry *entry = state->gid_cache[i];
        while (entry) {
            GroupCacheEntry *next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
        state->gid_cache[i] = NULL;
    }
}

// === 修改：display_status ===
void display_status(const ThreadSharedState *shared) {
    const Config *cfg = shared->cfg;
    const RuntimeState *state = shared->state; // 注意：这里是 const，无法修改 state->status_file_fp
    // 为了写入 status_file_fp，我们需要去 const
    RuntimeState *mutable_state = (RuntimeState *)state;
    const SmartQueue *queue = shared->queue;

    // 如果没开 output 也没开 mute (即不需要状态文件)，则直接返回
    if (!cfg->is_output_file && !cfg->is_output_split_dir && !cfg->mute) {
        return;
    }

    update_statistics(mutable_state);

    size_t async_pending = async_worker_get_queue_size();
    size_t dir_queued = queue->active_count + queue->buffer_count + queue->disk_count;

    // === 确定输出目标 ===
    FILE *target = stdout;
    bool use_ansi = true;

    if (cfg->mute) {
        // 如果是 Mute 模式，输出到状态文件
        // 懒加载打开文件 (如果尚未打开)
        if (!mutable_state->status_file_fp) {
            char status_path[1024];
            snprintf(status_path, sizeof(status_path), "%s.status", cfg->progress_base);
            mutable_state->status_file_fp = fopen(status_path, "w"); // "w" 模式打开
            if (!mutable_state->status_file_fp) {
                // 打开失败降级到 stdout
                mutable_state->status_file_fp = stdout; 
            }
        }
        target = mutable_state->status_file_fp;
        
        // 如果目标不是 stdout/stderr，则不使用 ANSI 颜色/清屏码，并重置文件指针
        if (target != stdout && target != stderr) {
            use_ansi = false;
            rewind(target); // 回到文件头
            ftruncate(fileno(target), 0); // 截断文件，清除旧内容
        }
    }

    // === 开始输出 ===
    if (use_ansi) fprintf(target, "\033[0;0H\033[J"); // ANSI 清屏
    
    fprintf(target, "===== 异步流水线状态 (v%s) =====\n", VERSION);
    
    char time_str[32];
    format_elapsed_time(state->start_time, time_str, sizeof(time_str));
    fprintf(target, "运行时间: %s\n", time_str);

fprintf(target, "\n[生产者: 目录扫描]\n");
    fprintf(target, "├── 队列堆积: %zu (内存:%zu, 磁盘:%zu)\n", 
           dir_queued, queue->active_count + queue->buffer_count, queue->disk_count);
    
    // 显示 发现速率 vs 消费速率
    fprintf(target, "├── 发现速率: %.2f 个/秒 (峰值: %.2f) [入队]\n", 
           state->stats.current_dir_rate, state->stats.max_dir_rate);
    fprintf(target, "└── 消费速率: %.2f 个/秒 (峰值: %.2f) [出队]\n", 
           state->stats.current_dequeue_rate, state->stats.max_dequeue_rate);

    // 计算压力趋势
    double trend = state->stats.current_dir_rate - state->stats.current_dequeue_rate;
    fprintf(target, "    └── 压力趋势: %s%.2f/s %s\n", 
            trend > 0 ? "+" : "", trend, 
            trend > 0 ? "(积压中)" : "(消化中)");

    fprintf(target, "\n[消费者: 结果写入]\n");
    fprintf(target, "├── 待写入文件数: %zu (Output Buffer)\n", async_pending);
    fprintf(target, "└── 文件产出速率: %.2f 个/秒 (峰值: %.2f)\n", 
           state->stats.current_file_rate, state->stats.max_file_rate);

    if (cfg->is_output_split_dir) {
        fprintf(target, "\n[落盘状态]\n");
        fprintf(target, "当前分片: " OUTPUT_SLICE_FORMAT " (行数: %lu / %lu)\n", 
               state->output_slice_num, state->output_line_count, cfg->output_slice_lines);
        fprintf(target, "总产出量: %lu 文件\n", state->file_count);
    } else {
        fprintf(target, "\n[落盘状态]\n");
        fprintf(target, "总产出量: %lu 文件\n", state->file_count);
    }

    if (cfg->continue_mode) {
        fprintf(target, "\n[断点保护]\n");
        fprintf(target, "进度记录: 分片 " PROGRESS_SLICE_FORMAT " (已记录目录数: %lu)\n", 
               state->write_slice_index, state->line_count % DEFAULT_PROGRESS_SLICE_LINES);
    }
    
    fprintf(target, "\n当前扫描目录: %s\n", state->current_path ? state->current_path : "...");
    
    fflush(target);
}

// 线程函数
void *status_thread_func(void *arg) {
    ThreadSharedState *shared = (ThreadSharedState *)arg;

    // 每500ms更新一次状态
    const useconds_t update_interval = 500000;

    while (shared->running) {
        // 加锁保护共享数据读取
        // pthread_mutex_lock(&shared->mutex);
        display_status(shared);
        // pthread_mutex_unlock(&shared->mutex);

        // 等待指定时间
        usleep(update_interval);
    }

    return NULL;
}
