#ifndef CORE_CONFIG_H
#define CORE_CONFIG_H

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

#define VERSION "15.6.2"
#define VERSION_NAME "v15.6.2"
#define VERSION_CODE 202608251230UL
/* 版本限定日志的门控码不写宏：直接在调用点写死时间戳字面量（本周期为 202608251200UL），
 * 防止宏值随版本递进被一改全改、旧日志被不断宽限而失去门控意义。
 * **严格遵循**：当程序异常以至于可能导致文件元数据被忽略或者有可能丢失时，需要输出的
 * 日志信息不得被版本门控，必须归属于全局日志（引用 VERSION_CODE 的 log_* 宏）。
 * error 类型的日志不得被版本门控，必须归属于全局日志。 */
#define MAX_PATH_LENGTH 4088 // v15.4.1: PIPE_BUF(4096) - sizeof(IpcMessageHeader)(8) = 4088, ensure atomic pipe writes
/* v15.6.0: Worker 数硬上限——redispatch_backoff_until/timeout_paths/timeout_counts
 * 等 per-slot 数组按此定长。--worker-count 超过此值必须钳制，否则 wid>=数组大小
 * 时越界读写 AppContext 后续字段（实测：越界读到 run_id 的 ASCII 字节被当作
 * 巨大 time_t 退避截止时间，低号 slot 全忙时派发永久活锁）。 */
#define MAX_WORKERS 64
#define PROGRESS_BATCH_SIZE 50
#define DEFAULT_MEM_ITEMS 10000000
#define MAX_SYMLINK_DEPTH 8
#define LOW_WATERMARK_RATIO 0.3
#define BUFFER_BATCH_SIZE 100000
#define DEFAULT_OUTPUT_FILE "output.txt"
#define DEFAULT_OUTPUT_SPLIT_DIR "output_split/"
#define DEFAULT_PROGRESS_SLICE_LINES 100000
#define DEFAULT_OUTPUT_SLICE_LINES 100000

/* v15.5.1: dispatch_queue pbin sliding window backpressure */
#define DISPATCH_QUEUE_HIGH_WATER    100000  /* queue 满，batch_processor 停止 push */
#define DISPATCH_QUEUE_LOW_WATER      30000  /* queue 降到此值，触发 pbin 加载 */
#define DISPATCH_QUEUE_LOAD_BATCH     50000  /* 每次从 pbin 加载的目录数量 */
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


#define HEARTBEAT_TIMEOUT_SEC 120   // v15.5.9: 从30提高到120，NFS大目录场景下30秒极易误判卡死
#define PROBE_TIMEOUT_SEC 5        // 探针5秒不返回视为设备死亡
#define MONITOR_INTERVAL_MS 500    // Monitor 线程主频 (500ms)
#define CHECK_INTERVAL_SEC 1       // 巡检频率 (1秒)

/* v15.6.1（P0-105）：有效进展看门狗。阈值必须大于最大 redispatch 退避（300s），
 * 默认 900s；--stall-timeout=0 禁用。动作：exit=杀 Worker 后 _exit(2)，abort=core。 */
#define DEFAULT_STALL_TIMEOUT_SEC 900
#define STALL_ACTION_EXIT  0
#define STALL_ACTION_ABORT 1

#define DEFAULT_BATCH_SIZE 1024
#define DEFAULT_ESTIMATED_FILES 10000000
#define DEFAULT_MASTER_THREADS 4

/* Pbin / fpbin Footer 常量 */
#define PBIN_FOOTER_MAGIC   0xDEADBEEF66AAC0FFULL
#define PBIN_FOOTER_SIZE    24  /* sizeof(PbinFooter) */


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
    char *reference_base;   // [v15.6.0] --reference-base 盲信基准（旧 progress 路径）
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
    bool strict_nlink;      /* v15.5.8: --strict-nlink 目录完备性 oracle（st_nlink-2 对账，默认关闭） */
    
    // === 其他 ===
    bool print_dir;
    bool verbose;
    int verbose_type;
    int verbose_level;
    unsigned long verbose_version; /* --verbose-version, ULONG_MAX=use default VERSION_CODE */
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

    int heartbeat_timeout;

    /* === v15.6.1（P0-105）：有效进展看门狗 === */
    int stall_timeout;          // --stall-timeout 秒，0=禁用（默认 DEFAULT_STALL_TIMEOUT_SEC）
    int stall_action;           // --stall-action：STALL_ACTION_EXIT / STALL_ACTION_ABORT
    
    /* === 新增：进程模型与性能参数 === */
    int batch_size;             // Worker batch 大小，默认 1024
    unsigned long estimated_files; // 预估文件数，用于预分配 HashSet
    int master_threads;         // Master 去重线程数，默认 4
    int worker_count;           // [新增] Worker 进程数，0 表示自动（默认上限 8）
} Config;

// 运行时状态
typedef struct {
    FILE *progress_file, *index_file;
    int lock_fd;
    unsigned long line_count, processed_count, dir_count, file_count, total_dequeued_count;
    unsigned long skipped_count;  /* 因熔断/超时/EIO 被跳过的路径总数 */
    dev_t root_dev;               /* 扫描根路径所在设备号，用于单挂载保护 */
    UserCacheEntry *uid_cache[UID_CACHE_SIZE];
    size_t uid_cache_count;
    GroupCacheEntry *gid_cache[GID_CACHE_SIZE];
    size_t gid_cache_count;
    unsigned long write_slice_index;
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
