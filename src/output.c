/**
 * @file output.c
 * @brief 格式化输出引擎
 *
 * 负责将扫描结果按照用户指定的格式（自定义格式串、CSV、或动态元数据开关）
 * 格式化并写入输出流。包含以下核心能力：
 * - 格式预编译：将 "%p|%s|%u" 等格式模板解析为 FormatSegment 数组，避免运行时重复解析
 * - CSV/RFC 4180 转义：自动处理字段内的双引号
 * - 用户/组名缓存：基于 UID/GID 的哈希链表缓存，减少重复 getpwuid/getgrgid 调用
 * - lsattr 扩展属性探测：通过 ioctl(FS_IOC_GETFLAGS) 获取文件属性字符串
 * - 输出分片：按行数自动切分输出文件（output_split_dir 模式）
 */
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
#include "log.h"

/**
 * @brief  将 st_mode 转换为 ls -l 格式的权限字符串
 * @param  mode  mode_t  文件模式位，取值范围: 任意有效的 st_mode 值
 * @param  buf   char*   输出缓冲区，长度至少为 11 字节（10 个字符 + '\0'），不能为空
 * @return void
 *
 * @note   输出格式示例："-rw-r--r--"、"drwxr-xr-x"、"lrwxrwxrwx"。
 *         特殊位（SUID/SGID/Sticky）显示为 s/S、t/T。
 */
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

/* =======================================================
 * lsattr 探测与缓存 (基于 RuntimeState)
 * ======================================================= */

/**
 * @brief  获取文件类型字符串
 * @param  mode  mode_t  文件模式位
 * @return const char*  文件类型描述字符串，如 "FILE"、"DIR"、"LINK"、"UNKNOWN" 等
 *
 * @note   根据 S_ISREG/S_ISDIR/S_ISLNK 等宏判断，未知类型返回 "UNKNOWN"。
 */
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

/**
 * @brief  按 RFC 4180 标准打印 CSV 字段（始终用双引号包裹）
 * @param  fp   FILE*       输出文件指针，不能为空
 * @param  str  const char* 要输出的字符串，允许为 NULL（按空串处理）
 * @return void
 *
 * @note   规则：字段整体用双引号包裹，内容中的每个双引号替换为两个双引号。
 *         示例：输入 `He said "hi"` → 输出 `"He said ""hi"""`。
 */
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

/**
 * @brief  从设备缓存中查找指定设备的状态（线程安全）
 * @param  dev    dev_t          设备号
 * @param  state  RuntimeState*  运行时状态指针，不能为空
 * @return DeviceStatus  设备状态，取值范围: {DEV_STATUS_UNKNOWN, DEV_STATUS_SUPPORTED, DEV_STATUS_UNSUPPORTED}
 *
 * @note   内部加锁遍历 dev_cache 数组，时间复杂度 O(MAX_DEV_CACHE)。
 *         未找到时返回 DEV_STATUS_UNKNOWN。
 */
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

/**
 * @brief  更新或新增设备状态到缓存（线程安全）
 * @param  dev     dev_t          设备号
 * @param  status  DeviceStatus   新的设备状态
 * @param  state   RuntimeState*  运行时状态指针，不能为空
 * @return void
 *
 * @note   采用双重检查模式：加锁后再次查找，若已存在则更新；
 *         若不存在且缓存未满，则追加新条目。
 *         缓存上限为 MAX_DEV_CACHE（64），超出时静默丢弃。
 */
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

/**
 * @brief  获取文件的 lsattr 扩展属性字符串
 * @param  state  RuntimeState*       运行时状态指针，不能为空
 * @param  path   const char*         文件路径，不能为空
 * @param  info   const struct stat*  文件 stat 信息指针，不能为空（用于获取设备号）
 * @param  buf    char*               输出缓冲区，长度至少为 16 字节，不能为空
 * @return void
 *
 * @note   流程：
 *         1. 检查设备缓存：若已知不支持则直接返回 "[unsupported]"
 *         2. 尝试 open(O_RDONLY | O_NONBLOCK) 获取 fd
 *         3. 尝试 ioctl(fd, FS_IOC_GETFLAGS, &flags)
 *            - 成功：解析 flags 为 16 字符属性串（如 "-------------e--"）
 *            - 失败且 errno 为 ENOTTY/EOPNOTSUPP：标记设备为不支持
 *            - 其他错误：返回 "[ioctl_error]"
 *         4. 关闭 fd
 *         该操作涉及系统调用，性能开销较大，仅在用户指定 --xattr 或格式串含 %%X 时执行。
 */
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

/**
 * @brief  获取用户名（含 UID 缓存哈希表）
 * @param  state  RuntimeState*  运行时状态指针，不能为空
 * @param  uid    uid_t          用户 ID，取值范围: 任意有效 Linux UID
 * @return const char*  格式化后的用户名字符串，如 "root(0)" 或 "1005"
 *
 * @note   使用哈希链表缓存（桶数 UID_CACHE_SIZE=4096），缓存命中时 O(1) 返回。
 *         缓存未命中时调用 getpwuid 查询系统，格式化为 "name(uid)" 或纯数字 UID。
 *         返回的指针指向缓存节点，无需释放，但生命周期与 RuntimeState 一致。
 */
const char *get_username(RuntimeState *state, uid_t uid) {
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

/**
 * @brief  获取组名（含 GID 缓存哈希表）
 * @param  state  RuntimeState*  运行时状态指针，不能为空
 * @param  gid    gid_t          组 ID，取值范围: 任意有效 Linux GID
 * @return const char*  格式化后的组名字符串，如 "root(0)" 或 "1005"
 *
 * @note   逻辑与 get_username 完全相同，使用 GID_CACHE_SIZE=4096 的哈希链表缓存。
 *         缓存未命中时调用 getgrgid 查询系统。
 */
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

/**
 * @brief  清理预编译的格式数组并释放内存
 * @param  cfg  Config*  指向配置结构体的指针，不能为空
 * @return void
 *
 * @note   释放 compiled_format 数组中所有 FMT_TEXT 类型的 text 字符串，
 *         然后释放 compiled_format 数组本身，最后清零相关字段。
 *         通常在程序退出前或重新预编译格式时调用。
 */
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

/**
 * @brief  预编译输出格式模板
 * @param  cfg  Config*  指向配置结构体的指针，不能为空
 * @return void
 *
 * @note   根据 cfg->csv、cfg->format 以及元数据开关（--size、--user 等）
 *         生成 compiled_format 数组：
 *         - CSV 模式默认格式："%%i,%%p,%%s,%%u,%%g,%%U,%%G,%%o,%%O,%%t,%%m,%%c"
 *         - 无格式串时根据元数据开关动态构建默认文本格式
 *         - 无任何开关时默认输出：path|size|mtime
 *         预编译后 print_to_stream 可直接遍历数组输出，无需运行时解析格式字符串。
 */
void precompile_format(Config *cfg) {
    const char *fmt = cfg->format;
    
    if (cfg->csv && !fmt) {
        // [默认 CSV 格式] Inode,Path,Size,User,Group,UID,GID,ModeStr,OctMode,Type,Mtime,Ctime
        fmt = "%i,%p,%s,%u,%g,%U,%G,%o,%O,%t,%m,%c";
    } else if (!fmt) {
        // [默认文本格式] 根据元数据开关动态构建
        char default_fmt[256];
        int pos = 0;
        pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "%%p");
        if (cfg->size)   pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%s");
        if (cfg->user)   pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%u");
        if (cfg->group)  pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%g");
        if (cfg->mtime)  pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%m");
        if (cfg->atime)  pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%a");
        if (cfg->ctime)  pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%c");
        if (cfg->mode)   pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%o");
        if (cfg->inode)  pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%i");
        if (cfg->xattr)  pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%X");
        // 如果没有启用任何元数据开关，默认输出 path|size|mtime
        if (!cfg->size && !cfg->user && !cfg->group && !cfg->mtime && !cfg->atime && !cfg->ctime && !cfg->mode && !cfg->inode && !cfg->xattr) {
            pos = 0;
            pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "%%p|%%s|%%m");
        }
        fmt = default_fmt;
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

/**
 * @brief  创建输出文件
 * @param  path  const char*  输出文件路径，不能为空
 * @return FILE*  成功返回打开的文件指针；失败返回 NULL
 */
FILE* create_output_file(const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        log_error("创建输出文件%s失败", path);
        return NULL;
    }
    return fp;
}

FILE* open_output_file_append(const char *path) {
    FILE *fp = fopen(path, "a");
    if (!fp) {
        log_error("打开输出文件%s失败", path);
        return NULL;
    }
    return fp;
}

/**
 * @brief  关闭输出文件
 * @param  fp  FILE*  要关闭的文件指针，允许传入 NULL、stdout 或 stderr（空操作）
 * @return void
 */
void close_output_file(FILE *fp) {
    if (!fp || fp == stdout || fp == stderr) return;
    fclose(fp);
}

/**
 * @brief  初始化所有输出文件和流
 * @param  cfg    const Config*  全局配置指针，不能为空
 * @param  state  RuntimeState*  运行时状态指针，不能为空
 * @return void
 *
 * @note   根据配置选择三种输出模式之一：
 *         - 分片目录模式（-O）：创建目录并按 PROGRESS_SLICE_FORMAT 命名切片文件
 *         - 单文件模式（-o）：直接打开指定文件
 *         - 标准输出模式（默认）：output_fp = stdout
 *         同时处理 --print-dir 的目录信息输出流：
 *         若数据走文件，目录流走伴生文件；若数据走 stdout，目录流走 stderr。
 *         所有文件流均启用大块全缓冲（8MB/1MB）。
 */
void init_output_files(const Config *cfg, RuntimeState *state) {
    // 1. 初始化计数器和状态
    if (!cfg->continue_mode) {
        state->output_line_count = 0;
    }
    if (state->output_slice_num == 0) {
        state->output_slice_num = 1;  /* 仅当未从索引恢复时才重置 */
    }
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
        if (cfg->continue_mode && access(slice_path, F_OK) == 0) {
            state->output_fp = open_output_file_append(slice_path);
            verbose_printf(cfg, 1, "恢复输出切片文件: %s\n", slice_path);
        } else {
            state->output_fp = create_output_file(slice_path);
            verbose_printf(cfg, 1, "打开新切片文件: %s\n", slice_path);
        }
        if (!state->output_fp) state->output_fp = stdout;
    } else if (cfg->is_output_file && cfg->output_file) {
        // 模式 B: 单文件
        if (cfg->continue_mode && access(cfg->output_file, F_OK) == 0) {
            state->output_fp = open_output_file_append(cfg->output_file);
            verbose_printf(cfg, 1, "恢复输出文件: %s\n", cfg->output_file);
        } else {
            state->output_fp = create_output_file(cfg->output_file);
            verbose_printf(cfg, 1, "打开输出文件: %s\n", cfg->output_file);
        }
        if (!state->output_fp) state->output_fp = stdout;
    } else {
        // 模式 C: 标准输出
        state->output_fp = stdout;
    }
    if (!state->output_fp) { perror("无法打开输出文件"); exit(EXIT_FAILURE); }

    // 启用大块缓冲以减少系统调用
    if (state->output_fp && state->output_fp != stdout) {
        setvbuf(state->output_fp, NULL, _IOFBF, 8 * 1024 * 1024);
    }

    // 3. [修复重点] 处理目录流输出 (--print-dir)
    // 逻辑：如果数据走文件，目录流也走文件(伴生文件)；如果数据走 stdout，目录流走 stderr。
    if (cfg->print_dir) {
        if (cfg->is_output_split_dir) {
            // 场景 6-Split: 写入 output_dir/scan_dirs.log
            char dir_log_path[1024];
            snprintf(dir_log_path, sizeof(dir_log_path), "%s/scan_dirs.log", cfg->output_split_dir);
            state->dir_info_fp = fopen(dir_log_path, "a"); // 追加模式
            if (!state->dir_info_fp) {
                log_warn("无法创建目录日志文件 %s，回退到 stderr", dir_log_path);
                state->dir_info_fp = stderr;
            }
        } else if (cfg->is_output_file) {
            // 场景 6-File: 写入 output_file.dir (例如 data.csv.dir)
            char dir_log_path[1024];
            snprintf(dir_log_path, sizeof(dir_log_path), "%s.dir", cfg->output_file);
            state->dir_info_fp = fopen(dir_log_path, "a"); // 追加模式
            if (!state->dir_info_fp) {
                log_warn("无法创建目录日志文件 %s，回退到 stderr", dir_log_path);
                state->dir_info_fp = stderr;
            }
        } else {
            // 场景 2: 数据走 stdout，目录流必须走 stderr，否则数据会乱
            state->dir_info_fp = stderr;
        }
    } else {
        state->dir_info_fp = NULL;
    }

    if (state->dir_info_fp && state->dir_info_fp != stderr && state->dir_info_fp != stdout) {
        setvbuf(state->dir_info_fp, NULL, _IOFBF, 1 * 1024 * 1024);
    }
}

/**
 * @brief  执行输出切片轮转（关闭当前切片并创建新文件）
 * @param  cfg    const Config*  全局配置指针，不能为空
 * @param  state  RuntimeState*  运行时状态指针，不能为空
 * @return void
 *
 * @note   仅在 is_output_split_dir 模式下生效。
 *         递增 output_slice_num，按 OUTPUT_SLICE_FORMAT 生成新文件名。
 *         若创建新文件失败则调用 perror 并 exit(EXIT_FAILURE)。
 */
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

/**
 * @brief  将单个文件记录格式化并输出到指定流
 * @param  cfg    const Config*       全局配置指针，不能为空
 * @param  state  RuntimeState*       运行时状态指针，不能为空（用于用户名/组名/扩展属性缓存）
 * @param  path   const char*         文件路径，不能为空
 * @param  st     const struct stat*  文件 stat 信息指针，不能为空
 * @param  fp     FILE*               目标输出流指针，不能为空
 * @return void
 *
 * @note   遍历预编译的 compiled_format 数组，根据每个 FormatSegment 的类型生成对应字段：
 *         - FMT_TEXT: 原样输出文本
 *         - FMT_PATH/FMT_SIZE/FMT_USER/...: 调用对应生成函数
 *         - CSV 模式下所有字段通过 print_csv_field 输出（自动转义）
 *         - quote 模式下用双引号包裹字段
 *         每行末尾输出换行符 '\n'。
 *         使用栈缓冲区 temp_buf 避免频繁堆分配。
 */
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

/**
 * @brief  清理 UID/GID 缓存哈希表并释放所有节点内存
 * @param  state  RuntimeState*  运行时状态指针，不能为空
 * @return void
 *
 * @note   遍历 UID_CACHE_SIZE 和 GID_CACHE_SIZE 个哈希桶，
 *         释放桶内链表的所有节点及其 name 字符串。
 *         通常在程序退出前调用。
 */
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
