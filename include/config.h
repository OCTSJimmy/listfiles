#ifndef CONFIG_H
#define CONFIG_H

#include <stdio.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <zlib.h>

// =======================================================
// 全局常量与宏
// =======================================================

#define VERSION "7.4"
#define MAX_PATH_LENGTH 2046
#define PROGRESS_BATCH_SIZE 50
#define DEFAULT_MEM_ITEMS 10000000
#define MAX_SYMLINK_DEPTH 8
#define LOW_WATERMARK_RATIO 0.3
#define BUFFER_BATCH_SIZE 100000
#define DEFAULT_OUTPUT_FILE "output.txt"
#define DEFAULT_OUTPUT_SPLIT_DIR "output_split/"
#define DEFAULT_PROGRESS_SLICE_LINES 100000
#define DEFAULT_OUTPUT_SLICE_LINES 100000
#define PROGRESS_SLICE_FORMAT "%06lu"
#define OUTPUT_SLICE_FORMAT "%06lu.txt"
#define VERBOSE_TYPE_FULL 0
#define VERBOSE_TYPE_VERSIONED 1
#define DEFAULT_VERBOSE_LEVEL 0
#define UID_CACHE_SIZE 4096
#define GID_CACHE_SIZE 4096
#define HASH_SET_INITIAL_SIZE 2000003 
// 定义初始休眠时间：50ms (极度保守，探测冰面)
#define START_SLEEP_US 50000 
#define MIN_SLEEP_US 0
#define MAX_SLEEP_US 500000
#define BATCH_FLUSH_SIZE 5000 
#define FLUSH_INTERVAL_SEC 5

#define min_size(a, b) ((a) < (b) ? (a) : (b))
#define max_size(a, b) ((a) > (b) ? (a) : (b))

// =======================================================
// 枚举类型定义
// =======================================================

// 格式预编译类型
typedef enum {
    FMT_TEXT,
    FMT_PATH,
    FMT_SIZE,
    FMT_USER,
    FMT_GROUP,
    FMT_MTIME,
    FMT_ATIME,
    FMT_MODE
} FormatType;

// =======================================================
// 核心数据结构定义
// =======================================================

// 格式段结构
typedef struct {
    FormatType type;
    char *text;
} FormatSegment;

// 用户缓存结构
typedef struct UserCacheEntry {
    uid_t uid;
    char *name;
    struct UserCacheEntry *next;
} UserCacheEntry;

// 组缓存结构
typedef struct GroupCacheEntry {
    gid_t gid;
    char *name;
    struct GroupCacheEntry *next;
} GroupCacheEntry;

// 智能队列项
typedef struct QueueEntry {
    char *path;
    struct QueueEntry *next;
} QueueEntry;

// 智能队列结构
typedef struct {
    QueueEntry *active_front, *active_rear;
    QueueEntry *buffer_front, *buffer_rear;
    size_t active_count, buffer_count, disk_count;
    FILE *overflow_file;
    char overflow_name[256];
    size_t max_mem_items, low_watermark;
    char *temp_dir;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int overflow_file_count;
    size_t items_per_file;
    size_t current_file_items;
// === 新增：空闲节点池 ===
    QueueEntry *free_list_head;  // 回收站的链表头
    size_t free_list_count;      // (可选) 统计池子里有多少闲置节点，防止无限膨胀
} SmartQueue;

// 全局配置
typedef struct {
    bool continue_mode, print_dir, verbose, size, user, group, mtime, atime, follow_symlinks;
    int verbose_type, verbose_level;
    char *format, *progress_base, *target_path;
    FormatSegment *compiled_format;
    int format_segment_count;
    unsigned long progress_slice_lines;
    bool archive, clean, is_output_file, is_output_split_dir;
    char *output_file, *output_split_dir;
    unsigned long output_slice_lines;
    bool decompress;
} Config;

// 运行时状态
typedef struct {
    FILE *progress_file, *index_file;
    int lock_fd;
    unsigned long line_count, processed_count, dir_count, file_count;
    UserCacheEntry *uid_cache[UID_CACHE_SIZE];
    size_t uid_cache_count;
    GroupCacheEntry *gid_cache[GID_CACHE_SIZE];
    size_t gid_cache_count;
    unsigned long write_slice_index, process_slice_index;
    FILE *write_slice_file, *output_fp, *dir_info_fp;
    unsigned long output_line_count, output_slice_num;
    time_t start_time;
    unsigned long completed_count;
    const char *current_path;
    char *lock_file_path;
} RuntimeState;

// 线程共享状态结构体
typedef struct {
    const Config *cfg;
    const RuntimeState *state;
    const SmartQueue *queue;
    volatile int running;
} ThreadSharedState;

#endif // CONFIG_H