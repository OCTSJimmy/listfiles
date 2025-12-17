#ifndef CONFIG_H
#define CONFIG_H

#include <stdio.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <zlib.h>


struct AsyncWorker;

// =======================================================
// 全局常量与宏
// =======================================================

#define VERSION "9.0"
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
#define START_SLEEP_US 50000 
#define MIN_SLEEP_US 0
#define MAX_SLEEP_US 500000
#define BATCH_FLUSH_SIZE 5000 
#define FLUSH_INTERVAL_SEC 5
#define MAX_DEV_CACHE 64
#define RATE_WINDOW_SIZE 60
#define SAMPLE_INTERVAL_MS 1000
#define OUTPUT_DIR_PREFIX "目录: "

#define min_size(a, b) ((a) < (b) ? (a) : (b))
#define max_size(a, b) ((a) > (b) ? (a) : (b))

// =======================================================
// 枚举类型定义
// =======================================================

typedef enum {
    FMT_TEXT, FMT_PATH, FMT_SIZE, FMT_USER, FMT_GROUP,
    FMT_MTIME, FMT_ATIME, FMT_MODE, FMT_XATTR
} FormatType;

typedef enum {
    DEV_STATUS_UNKNOWN = 0,
    DEV_STATUS_SUPPORTED,
    DEV_STATUS_UNSUPPORTED
} DeviceStatus;

// 速率采样点
typedef struct {
    time_t timestamp;
    unsigned long dir_count;
    unsigned long file_count;
    unsigned long dequeued_count;
} RateSample;

// 统计状态
typedef struct {
    RateSample samples[RATE_WINDOW_SIZE];
    int head_idx;
    bool filled;
    time_t last_sample_time;
    
    double current_dir_rate;
    double max_dir_rate;
    double current_file_rate;
    double max_file_rate;
    double current_dequeue_rate;
    double max_dequeue_rate;
} Statistics;

typedef struct {
    dev_t dev;
    DeviceStatus status;
} DeviceCapEntry;

// =======================================================
// 核心数据结构定义
// =======================================================

typedef struct {
    FormatType type;
    char *text;
} FormatSegment;

typedef struct UserCacheEntry {
    uid_t uid;
    char *name;
    struct UserCacheEntry *next;
} UserCacheEntry;

typedef struct GroupCacheEntry {
    gid_t gid;
    char *name;
    struct GroupCacheEntry *next;
} GroupCacheEntry;

// 【删除】ScanNode 和 SmartQueue 结构体定义已移除

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
    bool xattr;
    bool mode;
    bool quote;
    bool include_dir;
    char *resume_file;
    bool mute;
} Config;

// 运行时状态
typedef struct {
    FILE *progress_file, *index_file;
    int lock_fd;
    unsigned long line_count, processed_count, dir_count, file_count, total_dequeued_count;
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
    DeviceCapEntry dev_cache[MAX_DEV_CACHE];
    size_t dev_cache_count;
    pthread_mutex_t dev_cache_mutex;
    FILE *status_file_fp; 
    Statistics stats;
} RuntimeState;

// 线程共享状态结构体
typedef struct {
    const Config *cfg;
    const RuntimeState *state;
    struct AsyncWorker *worker; // [新增]：持有 worker 句柄
    volatile int running;
} ThreadSharedState; 

#endif // CONFIG_H