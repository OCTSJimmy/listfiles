#include "output.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sys/stat.h>

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

// ==========================================
// 2. 格式化核心
// ==========================================

// [新增] 辅助：获取文件类型字符串
static const char* get_type_str(mode_t mode) {
    if (S_ISREG(mode)) return "FILE";
    if (S_ISDIR(mode)) return "DIR";
    if (S_ISLNK(mode)) return "LINK";
    if (S_ISCHR(mode)) return "CHR";
    if (S_ISBLK(mode)) return "BLK";
    if (S_ISFIFO(mode)) return "FIFO";
    if (S_ISSOCK(mode)) return "SOCK";
    return "UNKNOWN";
}

// [新增] 辅助：打印 CSV 字段 (RFC 4180)
// 规则：始终用双引号包裹，内容中的双引号替换为两个双引号
static void print_csv_field(FILE *fp, const char *str) {
    fputc('"', fp);
    while (*str) {
        if (*str == '"') {
            fputc('"', fp); // Escape " to ""
        }
        fputc(*str, fp);
        str++;
    }
    fputc('"', fp);
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
const char *get_username( RuntimeState *state, uid_t uid) {
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
const char *get_groupname(RuntimeState *state, gid_t gid) {
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

/**********************
 * 格式预编译实现
 **********************/
void precompile_format(Config *cfg) {
    const char *fmt = cfg->format;
    
    if (cfg->csv && !fmt) {
        // [默认 CSV 格式] Inode,Path,Size,User,Group,UID,GID,ModeStr,OctMode,Type,Mtime,Ctime
        fmt = "%i,%p,%s,%u,%g,%U,%G,%o,%O,%t,%m,%c";
    } else if (!fmt) {
        // [默认文本格式]
        fmt = "%p|%s|%m";
    }

    int count = 0;
    int capacity = 32;
    cfg->compiled_format = safe_malloc(sizeof(FormatSegment) * capacity);
    
    const char *p = fmt;
    while (*p) {
        if (*p == '%') {
            p++;
            if (*p == '\0') break;

            FormatSegment seg;
            seg.text = NULL; 

            switch (*p) {
                case 'p': seg.type = FMT_PATH; break;
                case 's': seg.type = FMT_SIZE; break;
                case 'u': seg.type = FMT_USER; break;
                case 'g': seg.type = FMT_GROUP; break;
                case 'U': seg.type = FMT_UID; break; 
                case 'G': seg.type = FMT_GID; break;
                case 'm': seg.type = FMT_MTIME; break;
                case 'a': seg.type = FMT_ATIME; break;
                case 'c': seg.type = FMT_CTIME; break;
                case 't': seg.type = FMT_TYPE; break;
                case 'i': seg.type = FMT_INODE; break;
                case 'o': seg.type = FMT_MODE; break;    
                case 'O': seg.type = FMT_ST_MODE; break; 
                case 'X': seg.type = FMT_XATTR; break;
                default: 
                    seg.type = FMT_TEXT; 
                    char *tmp = safe_malloc(3);
                    tmp[0] = '%'; tmp[1] = *p; tmp[2] = '\0';
                    seg.text = tmp;
                    break;
            }
            p++;
            
            if (count >= capacity) {
                capacity *= 2;
                cfg->compiled_format = realloc(cfg->compiled_format, sizeof(FormatSegment) * capacity);
            }
            cfg->compiled_format[count++] = seg;
        } else {
            const char *start = p;
            while (*p && *p != '%') p++;
            int len = p - start;
            char *text = safe_malloc(len + 1);
            strncpy(text, start, len);
            text[len] = '\0';
            
            if (count >= capacity) {
                capacity *= 2;
                cfg->compiled_format = realloc(cfg->compiled_format, sizeof(FormatSegment) * capacity);
            }
            cfg->compiled_format[count].type = FMT_TEXT;
            cfg->compiled_format[count++].text = text;
        }
    }
    cfg->format_segment_count = count;
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
    // 1. 初始化计数器和状态
    state->output_line_count = 0;
    state->output_slice_num = 1;
    state->start_time = time(NULL);
    state->completed_count = 0;
    state->current_path = NULL;
    state->lock_file_path = NULL;
    
    // 2. 处理主数据输出 (Data Output)
    if (cfg->is_output_split_dir && cfg->output_split_dir) {
        // 模式 A: 分片目录
        if(mkdir(cfg->output_split_dir, 0700) == -1 && errno != EEXIST) {
            perror("无法创建输出目录"); exit(EXIT_FAILURE);
        }
        char slice_path[1024];
        snprintf(slice_path, sizeof(slice_path), "%s/" OUTPUT_SLICE_FORMAT, cfg->output_split_dir, state->output_slice_num);
        state->output_fp = create_output_file(slice_path);
        if (!state->output_fp) state->output_fp = stdout;
    } else if (cfg->is_output_file && cfg->output_file) {
        // 模式 B: 单文件
        state->output_fp = create_output_file(cfg->output_file);
        if (!state->output_fp) state->output_fp = stdout;
    } else {
        // 模式 C: 标准输出
        state->output_fp = stdout;
    }
    if (!state->output_fp) { perror("无法打开输出文件"); exit(EXIT_FAILURE); }

    // 3. [修复重点] 处理目录流输出 (--print-dir)
    // 逻辑：如果数据走文件，目录流也走文件(伴生文件)；如果数据走 stdout，目录流走 stderr。
    if (cfg->print_dir) {
        if (cfg->is_output_split_dir) {
            // 场景 6-Split: 写入 output_dir/scan_dirs.log
            char dir_log_path[1024];
            snprintf(dir_log_path, sizeof(dir_log_path), "%s/scan_dirs.log", cfg->output_split_dir);
            state->dir_info_fp = fopen(dir_log_path, "w"); // 覆盖模式
            if (!state->dir_info_fp) {
                fprintf(stderr, "[警告] 无法创建目录日志文件 %s，回退到 stderr\n", dir_log_path);
                state->dir_info_fp = stderr;
            }
        } else if (cfg->is_output_file) {
            // 场景 6-File: 写入 output_file.dir (例如 data.csv.dir)
            char dir_log_path[1024];
            snprintf(dir_log_path, sizeof(dir_log_path), "%s.dir", cfg->output_file);
            state->dir_info_fp = fopen(dir_log_path, "w"); // 覆盖模式
            if (!state->dir_info_fp) {
                fprintf(stderr, "[警告] 无法创建目录日志文件 %s，回退到 stderr\n", dir_log_path);
                state->dir_info_fp = stderr;
            }
        } else {
            // 场景 2: 数据走 stdout，目录流必须走 stderr，否则数据会乱
            state->dir_info_fp = stderr;
        }
    } else {
        state->dir_info_fp = NULL;
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

// 兼容旧接口的占位函数
// void format_output(const Config *cfg, RuntimeState *state, const char *path, const struct stat *st, char *buffer, size_t size) {
    // 实际逻辑已移至 print_to_stream
    // if (size > 0) buffer[0] = '\0';
// }

// 直接输出到流 (性能更高)
// 直接输出到流
void print_to_stream(const Config *cfg, RuntimeState *state, const char *path, const struct stat *st, FILE *fp) {
    char temp_buf[MAX_PATH_LENGTH]; // 通用缓冲区

    for (int i = 0; i < cfg->format_segment_count; i++) {
        FormatSegment *seg = &cfg->compiled_format[i];
        
        if (seg->type == FMT_TEXT) {
            // CSV 模式下，如果格式串里包含逗号，这里原样输出即可
            // 因为 precompile_format 会保证生成正确的逗号分隔符
            if (seg->text) fputs(seg->text, fp);
            continue;
        }

        const char *val_str = NULL;
        
        switch (seg->type) {
            case FMT_PATH: val_str = path; break;
            case FMT_SIZE: snprintf(temp_buf, sizeof(temp_buf), "%ld", (long)st->st_size); val_str = temp_buf; break;
            case FMT_USER: val_str = get_username(state, st->st_uid); break;
            case FMT_GROUP: val_str = get_groupname(state, st->st_gid); break;
            case FMT_UID: snprintf(temp_buf, sizeof(temp_buf), "%d", st->st_uid); val_str = temp_buf; break;
            case FMT_GID: snprintf(temp_buf, sizeof(temp_buf), "%d", st->st_gid); val_str = temp_buf; break;
            case FMT_MTIME: val_str = format_time(st->st_mtime); break;
            case FMT_ATIME: val_str = format_time(st->st_atime); break;
            case FMT_CTIME: val_str = format_time(st->st_ctime); break;
            case FMT_MODE: format_mode_str(st->st_mode, temp_buf); val_str = temp_buf; break;
            case FMT_ST_MODE: snprintf(temp_buf, sizeof(temp_buf), "0%o", st->st_mode & 0777); val_str = temp_buf; break;
            case FMT_TYPE: val_str = get_type_str(st->st_mode); break;
            case FMT_INODE: snprintf(temp_buf, sizeof(temp_buf), "%lu", (unsigned long)st->st_ino); val_str = temp_buf; break;
            case FMT_XATTR: get_xattr_str(state, path, st, temp_buf); val_str = temp_buf; break;
            default: val_str = "";
        }

        if (cfg->csv) {
            print_csv_field(fp, val_str);
        } else if (cfg->quote) {
            fputc('"', fp);
            if (val_str) fputs(val_str, fp);
            fputc('"', fp);
        } else {
            if (val_str) fputs(val_str, fp);
        }
    }
    fputc('\n', fp);
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
