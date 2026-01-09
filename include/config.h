#ifndef CONFIG_H
#define CONFIG_H

#include <stdio.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <zlib.h>
#include <stdint.h> // 新增

struct AsyncWorker;
struct DeviceManager;

// =======================================================
// 全局常量与宏
// =======================================================

#define VERSION "10.0" // 版本号升级
#define MAX_PATH_LENGTH 4096 // 扩大路径支持，防止深层目录截断
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
    FMT_MTIME, FMT_ATIME, FMT_CTIME, // [新增]
    FMT_MODE, FMT_ST_MODE, FMT_TYPE, // [新增] st_mode(八进制), type(字符串)
    FMT_INODE, FMT_UID, FMT_GID,// [新增]
    FMT_XATTR
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

// 全局配置
typedef struct {
    // === 核心身份 ===
    char *target_path;      // -p
    char *output_file;      // -o
    char *output_split_dir; // -O
    bool is_output_file;
    bool is_output_split_dir;

    // === 运行模式 ===
    bool continue_mode;     // -c (断点续传)
    bool runone;            // [新增] --runone (强制全量)
    long skip_interval;     // [新增] --skip-interval (半增量阈值，秒)
    bool sure;              // [新增] --sure (跳过交互确认)
    
    // === 行为策略 ===
    bool archive;           // -Z
    bool clean;             // -C
    char *progress_base;    // -f
    char *resume_file;      // -R
    
    // === 输出格式 ===
    bool csv;               // [新增] --csv (严格模式)
    char *format;           // -F
    bool quote;             // -Q
    
    // === 元数据开关 ===
    bool size;
    bool user;
    bool group;
    bool mtime;
    bool atime;
    bool ctime;             // [新增]
    bool mode;              // mode string (drwxr-xr-x)
    bool st_mode;           // [新增] octal mode (0755) (隐式支持，通过 format)
    bool inode;             // [新增] (隐式支持，通过 format)
    bool xattr;
    bool follow_symlinks;
    bool include_dir;       // -D
    
    // === 其他 ===
    bool print_dir;
    bool verbose;
    int verbose_type;
    int verbose_level;
    unsigned long progress_slice_lines;
    unsigned long output_slice_lines;
    bool decompress;
    bool mute;
    
    // === 内部状态 (预编译格式) ===
    FormatSegment *compiled_format;
    int format_segment_count;
    
    // === 会话一致性校验字段 (从 .config 读取) ===
    time_t last_start_time;
    char *last_cmd_args; 
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
    // [新增] 设备管理器句柄
    struct DeviceManager *dev_mgr;
    // [新增] 全局错误标志
    volatile bool has_error;
} RuntimeState;

// 线程共享状态结构体
typedef struct {
    const Config *cfg;
    const RuntimeState *state;
    struct AsyncWorker *worker;
    volatile int running;
} ThreadSharedState; 

#endif // CONFIG_H