#ifndef APP_CONTEXT_H
#define APP_CONTEXT_H

#include "config.h"
#include "fingerprint_set.h"

/* record_path 批量缓冲 */
#define RECORD_BATCH_COUNT 4096
#define RECORD_BATCH_BYTES (1 * 1024 * 1024)

typedef struct {
    char *paths[RECORD_BATCH_COUNT];
    struct stat stats[RECORD_BATCH_COUNT];
    int count;
    size_t total_bytes;
} RecordBatch;
#include "reference_map.h"
#include "worker_proc.h"
#include "probe_scheduler.h"
#include "device_manager.h"
#include "async_worker.h"
#include "output.h"
#include "spbin.h"
#include "thread_pool.h"
#include "monitor.h"

/* 恢复流程中的历史目录泵送状态 */
typedef enum {
    HIST_PUMP_DONE,      /* 正常扫描，不需要 pump */
    HIST_PUMP_OLD,       /* 正在消费原始 pbin，新子目录 → fpbin */
    HIST_PUMP_NEW        /* 正在消费 fpbin 转正后的新 pbin，新子目录直接入队 */
} HistPumpState;

typedef struct AppContext {
    /* === 配置与运行时状态 === */
    Config        cfg;
    RuntimeState  state;
    
    /* === 去重与参考索引（仅主进程访问） === */
    FingerprintSet *visited_set;      /* 本次任务防环 */
    FingerprintSet *reference_set;    /* 半增量：历史存在性（可能 NULL） */
    ReferenceMap   *reference_map;    /* 半增量：fingerprint -> (mtime, d_type) */
    
    /* === 进程管理 === */
    WorkerPool     *worker_pool;
    ProbeScheduler *probe_scheduler;
    DeviceManager  *dev_mgr;
    
    /* === spbin 内存缓存（设备恢复时重入队用） === */
    SpbinEntry     *spbin_entries;
    size_t          spbin_count;
    size_t          spbin_capacity;
    
    /* === 事件循环 === */
    int             epfd;
    bool            running;
    int             next_requeue_worker;
    int             next_dispatch_worker;   // [新增] 轮询分发 Worker 索引
    
    /* === 丢失任务重入队（Worker 超时死亡时保存） === */
    char          **lost_tasks;
    size_t          lost_count;
    size_t          lost_capacity;
    
    /* === 任务计数 === */
    _Atomic long    pending_tasks;
    _Atomic long    pending_batches;   /* 已提交到线程池但未完成的 batch 数 */
    bool            resume_active;
    
    /* === 输出线程 === */
    AsyncWorker    *async_writer;
    pthread_t       writer_tid;

    /* === 监控线程 === */
    Monitor        *monitor;
    
    /* === 线程池与事件通知 === */
    ThreadPool     *thread_pool;
    int             event_fd;
    
    /* === record_path 批量缓冲 === */
    RecordBatch     record_batch;
    
    /* === 历史目录泵送状态（恢复流程专用） === */
    HistPumpState   hist_pump_state;
    FILE           *hist_pump_fp;           /* 当前正在消费的 pbin 文件 */
    unsigned long   hist_pump_slice_idx;    /* 当前消费的分片编号 */
    unsigned long   hist_pump_line_no;      /* 当前分片内的行号（用于跳过已处理行） */
    
    /* === fpbin 临时缓存（恢复流程专用） === */
    FILE           *fpbin_slice_file;   /* 当前活跃 fpbin 分片文件指针 */
    unsigned long   fpbin_write_slice_index; /* 当前 fpbin 分片号 */
    unsigned long   fpbin_line_count;   /* 当前 fpbin 分片行数 */
    char          **fpbin_entries;      /* 内存中的 fpbin 路径数组 */
    struct stat    *fpbin_stats;        /* 对应的 stat 数组 */
    size_t          fpbin_count;        /* 当前内存中的条目数 */
    size_t          fpbin_capacity;     /* 内存数组容量 */
    
} AppContext;

#endif
