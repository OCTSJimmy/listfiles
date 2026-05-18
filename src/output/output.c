/**
 * @file output.c
 * @brief 格式化输出引擎
 *
 * 负责将扫描结果按照预编译格式输出到流。
 * 包含 CSV 转义、类型字符串转换、核心输出循环及缓存清理。
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
