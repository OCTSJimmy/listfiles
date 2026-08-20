#ifndef APP_CONTEXT_H
#define APP_CONTEXT_H

#include "config.h"
#include "fingerprint_set.h"

/* record_path 批量缓冲 */
#define RECORD_BATCH_COUNT 4096
#define RECORD_BATCH_BYTES (1 * 1024 * 1024)

/* v15.5.9: 目录级熔断阈值——同一个路径连续 DEV_TIMEOUT 超过此次数后不再重试
 * NFS 大目录场景下 3 次过严（30s*3=90s 对大目录远远不够），提高到 10 */
#define CIRCUIT_BREAKER_THRESHOLD 10

typedef struct {
    char *paths[RECORD_BATCH_COUNT];
    struct stat stats[RECORD_BATCH_COUNT];
    int count;
    size_t total_bytes;
} RecordBatch;
#include "reference_map.h"
#include "worker_proc.h"
#include "msg_queue.h"
#include "ipc_thread.h"
#include "probe_scheduler.h"
#include "device_manager.h"
#include "async_worker.h"
#include "output.h"
#include "spbin.h"
#include "thread_pool.h"
#include "monitor.h"
#include "dispatch_queue.h"

/* 恢复流程中的历史目录泵送状态 */
typedef enum {
    HIST_PUMP_DONE,      /* 正常扫描,不需要 pump */
    HIST_PUMP_OLD,       /* 正在消费原始 pbin,新子目录 → fpbin */
    HIST_PUMP_NEW        /* 正在消费 fpbin 转正后的新 pbin,新子目录直接入队 */
} HistPumpState;

typedef struct AppContext {
    /* === 配置与运行时状态 === */
    Config        cfg;
    RuntimeState  state;

    /* === 去重与参考索引(仅主进程访问) ===
     * v15.6.0（P0-004 统一队列模型）：原 visited_set 拆分为三态集合——
     * discovered_set：已发现（仅目录，防环/防重复发现；文件不进入，输出 at-least-once）
     * enqueued_set：  已入队（在 dispatch_queue 或 dspill 中，防重复入队）
     * completed_set： 已完成（dpbin/dfpbin 合并集加载，差集剪枝） */
    FingerprintSet *discovered_set;   /* 本次任务防环（仅目录） */
    FingerprintSet *enqueued_set;     /* 已入队目录（dispatch_queue 或 dspill） */
    FingerprintSet *completed_set;    /* v15.5.0: dpbin 加载的已完成目录集合(恢复时) */
    FingerprintSet *reference_set;    /* 半增量:历史存在性(可能 NULL) */
    ReferenceMap   *reference_map;    /* 半增量:fingerprint -> (mtime, d_type) */

    /* === 进程管理 === */
    WorkerPool     *worker_pool;
    ProbeScheduler *probe_scheduler;
    DeviceManager  *dev_mgr;

    /* === spbin 内存缓存(设备恢复时重入队用) === */
    SpbinEntry     *spbin_entries;
    size_t          spbin_count;
    size_t          spbin_capacity;
    /* v15.6.0（P0-005）：spbin 路径指纹集合（path-only 指纹，fp_compute(path,0,0)）。
     * 恢复泵送/dspill 回填时拦截熔断/跳过目录，防止盲目重入队；懒创建。 */
    FingerprintSet *spbin_set;

    /* === v15.6.0（P1-004）：毒丸目录致死计数 ===
     * Worker 死亡时正在扫描的目录 path→count（死亡是稀有事件，小型动态数组即可）。
     * 同一路径累计致死 POISON_DEATH_THRESHOLD 次 → 写 spbin POISON 永久隔离。 */
    char   **poison_paths;
    int     *poison_counts;
    size_t   poison_count;
    size_t   poison_capacity;

    /* === 事件循环 === */
    int             epfd;
    bool            running;
    int             next_requeue_worker;
    int             next_dispatch_worker;   // [新增] 轮询分发 Worker 索引

    DispatchQueue   dispatch_queue;
    
    /* === v13.0.0 IPC Thread Isolation === */
    MsgQueue       **ipc_cmd_queues;    /* Master -> IPC threads */
    MsgQueue       **ipc_ret_queues;    /* IPC threads -> Master */
    IpcThreadCtx   **ipc_threads;        /* IPC thread contexts */
    pthread_t       *ipc_tids;           /* IPC thread handles */
    pthread_mutex_t  main_mutex;         /* Main thread cond mutex */
    pthread_cond_t   main_cond;          /* Main thread wake condition */
    _Atomic bool     main_wakeup;        /* Flag to wake main thread */

    /* === 任务计数 === */
    _Atomic long    pending_tasks;
    _Atomic long    pending_batches;   /* 已提交到线程池但未完成的 batch 数 */
    _Atomic uint64_t epoch_counter;    /* v15.6.0: 目录派发 epoch 计数器（每次成功派发 +1） */

    /* === v15.6.0: 输出三态（P0-002）——每 Worker slot 未 COMMITTED 的输出 batch 数 ===
     * 主线程提交输出 batch 前 +1（OUTPUT_QUEUED），输出线程 fflush 后 -1（OUTPUT_COMMITTED），
     * 目录完成屏障要求归零才允许 dpbin。按 num_workers 动态分配（main.c），
     * 无任务归属的输出（单文件目标等）以 slot_id=-1 提交、不计数。 */
    _Atomic long   *output_pending;

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

    /* === 历史目录泵送状态(恢复流程专用) === */
    HistPumpState   hist_pump_state;
    FILE           *hist_pump_fp;           /* 当前正在消费的 pbin 文件 */
    unsigned long   hist_pump_slice_idx;    /* 当前消费的分片编号 */
    unsigned long   hist_pump_line_no;      /* 当前分片内的行号(用于跳过已处理行) */

    /* === fpbin 临时缓存(恢复流程专用) === */
    FILE           *fpbin_slice_file;   /* 当前活跃 fpbin 分片文件指针 */
    unsigned long   fpbin_write_slice_index; /* 当前 fpbin 分片号 */
    unsigned long   fpbin_line_count;   /* 当前 fpbin 分片行数 */
    char          **fpbin_entries;      /* 内存中的 fpbin 路径数组 */
    struct stat    *fpbin_stats;        /* 对应的 stat 数组 */
    size_t          fpbin_count;        /* 当前内存中的条目数 */
    size_t          fpbin_capacity;     /* 内存数组容量 */

    /* === dpbin 完成日志(本次会话临时) === */
    FILE           *dpbin_slice_file;       /* 当前活跃 dpbin 分片文件指针 */
    unsigned long   dpbin_write_slice_index;/* 当前 dpbin 分片号 */
    unsigned long   dpbin_line_count;       /* 当前 dpbin 分片行数 */

    /* === v15.6.0: dfpbin 完成日志（P0-003，fpbin 原子对） ===
     * HIST_PUMP_OLD 阶段完成的父目录写 dfpbin 而非 dpbin；
     * fpbin 转正时同步合并入 dpbin，恢复时与 fpbin 成对校验，不完整则整对抛弃。 */
    FILE           *dfpbin_slice_file;       /* 当前活跃 dfpbin 分片文件指针 */
    unsigned long   dfpbin_write_slice_index;/* 当前 dfpbin 分片号 */
    unsigned long   dfpbin_line_count;       /* 当前 dfpbin 分片行数 */

    /* === v15.5.8: dspill 派发兜底（运行级追加文件，替代 pbin 滑动窗口） ===
     * 队列达到 HIGH_WATER 时被跳推的目录追加写入 {base}.dspill；
     * 加载器按字节游标回填。无分片轮转、无删除竞争、只含跳推目录。
     * 写端为 batch_processor（线程池线程），读端为主线程，须持 dspill_mutex。 */
    FILE           *dspill_fp;          /* 追加写句柄（懒打开） */
    long            dspill_read_offset; /* 加载游标（字节偏移） */
    unsigned long   dspill_appended;    /* 累计跳推（溢出）目录数 */
    unsigned long   dspill_loaded;      /* 累计从 dspill 回填目录数 */
    unsigned long   dspill_pending;     /* v15.6.0: 距上次刷盘积累的待刷条数 */
    time_t          dspill_last_flush;  /* v15.6.0: 上次刷盘时间（1 秒定时刷盘用） */
    pthread_mutex_t dspill_mutex;       /* 写端/读端互斥 */

    /* === v15.5.9: redispatch 指数退避（NFS大目录防连续快速失败） === */
    time_t redispatch_backoff_until[MAX_WORKERS];  /* 每个 slot 的退避截止时间 */

    /* === v15.6.0（P0-008）：Run manifest 状态 === */
    char     run_id[64];              /* 本轮 run_id（<时间戳>-<pid>），启动时生成 */
    bool     blind_trust;             /* 本轮为盲信扫描（manifest 恒 baseline_eligible=0） */
    char     baseline_run_id[64];     /* 盲信基准 run_id（非盲信为空串） */
    time_t   baseline_completed_at;   /* 盲信基准完成时间（审计用） */

    /* === v15.5.3: per-slot DEV_TIMEOUT circuit breaker === */
    char timeout_paths[MAX_WORKERS][4096];   /* 每个 Worker slot 最近 timeout 的路径 */
    int  timeout_counts[MAX_WORKERS];        /* 该路径连续 timeout 次数 */

    /* === 熔断清单 === */
    FILE           *circuit_breaker_fp;
    pthread_mutex_t circuit_breaker_mutex;

} AppContext;

#endif
