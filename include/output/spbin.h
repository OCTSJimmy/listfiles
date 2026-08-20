#ifndef SPBIN_H
#define SPBIN_H

#include <stdint.h>
#include <time.h>

/* v15.6.0（P0-005）：spbin 跳过原因码
 * 恢复行为：PERMISSION/CIRCUIT_BREAKER/POISON 永久跳过；
 * PROBE_FAIL/TIMEOUT 检查 timestamp + 退避窗口，超窗后敢死队探测该设备。 */
#define SP_REASON_PROBE_FAIL       1  /* 设备探测失败（EIO/ENODEV/ESTALE 及未知 errno 保守归类） */
#define SP_REASON_TIMEOUT          2  /* 设备超时（ETIMEDOUT） */
#define SP_REASON_CIRCUIT_BREAKER  3  /* 目录级熔断（连续 DEV_TIMEOUT 达 CIRCUIT_BREAKER_THRESHOLD） */
#define SP_REASON_PERMISSION       4  /* 权限拒绝（EACCES/EPERM） */
#define SP_REASON_POISON           5  /* 毒丸目录（P1-004：累计致死 Worker 3 次） */

/* 内存条目状态（不持久化） */
#define SP_STATUS_PROBING   0  /* 等待设备探测/恢复（DEVICE_WAITING） */
#define SP_STATUS_CONDEMNED 1  /* 本次运行不再入队（永久跳过或未超窗等待下次会话） */
#define SP_STATUS_RECOVERED 2  /* v15.6.0：设备恢复后已重入队，compaction 时过滤 */

/* v15.6.0（P0-005）：跨会话探测退避窗口，按 retry_count 指数升级（秒）：
 * 30min → 2h → 6h → 24h 封顶 */
#define SPBIN_BACKOFF_STAGES { 30 * 60, 2 * 3600, 6 * 3600, 24 * 3600 }
#define SPBIN_BACKOFF_STAGES_COUNT 4

/* v15.6.0（P0-005）：spbin 内存条目上限，超过则告警并紧急 compaction */
#define SPBIN_MAX_ENTRIES 100000

#define SPBIN_DEVICE_KEY_LEN 64

/*
 * v15.6.0（P0-005）spbin 磁盘记录格式（append-only，流式读写，逐字段 fwrite）：
 *   [path_len:u32][path:N bytes][reason:u8][timestamp:time_t][device_key:64B]
 * device_key 暂为 st_dev 的十进制字符串（NUL 结尾，余量清零）——
 * P1-003 将升级为 (fsid, server, export) 三元组。
 * retry_count 不持久化：恢复时从 0 开始（退避窗口回到 30min 档）。
 */

/* In-memory representation */
typedef struct {
    char    *path;
    uint64_t dev;          /* 运行期设备匹配键（= device_key 解析值） */
    uint8_t  reason;       /* SP_REASON_* */
    time_t   timestamp;    /* 记录时间 / 上次探测失败时间 */
    uint32_t retry_count;  /* 探测失败计数（跨会话退避窗口索引，不持久化） */
    uint8_t  s_status;     /* SP_STATUS_*（仅内存态） */
} SpbinEntry;

#endif
