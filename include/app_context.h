#ifndef APP_CONTEXT_H
#define APP_CONTEXT_H

#include "config.h"
#include "fingerprint_set.h"
#include "reference_map.h"
#include "worker_proc.h"
#include "probe_scheduler.h"
#include "device_manager.h"
#include "async_worker.h"
#include "output.h"
#include "spbin.h"

typedef struct {
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
    
    /* === 任务计数 === */
    _Atomic long    pending_tasks;
    bool            resume_active;
    
    /* === 输出线程 === */
    AsyncWorker    *async_writer;
    pthread_t       writer_tid;
    
} AppContext;

#endif
