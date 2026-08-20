#ifndef MANIFEST_H
#define MANIFEST_H

#include <stdbool.h>
#include <time.h>
#include "config.h"

/* v15.6.0（P0-008 / 设计 §0.2/§0.7）：Run manifest 与 pbin 的 schema 版本。
 * 续传/盲信强制校验：manifest 缺失或 pbin_schema_version 不一致一律拒绝
 * （exit 2，提示 --runone 重新全量扫描），不做双格式解析。 */
#define MANIFEST_SCHEMA_VERSION 2
#define PBIN_SCHEMA_VERSION     2

/* manifest 读取结果（只收录校验/审计所需字段；统计字段不回读） */
typedef struct {
    char    run_id[64];                    /* 运行唯一标识（<时间戳>-<pid>） */
    char    target_path[MAX_PATH_LENGTH];  /* 扫描根路径 */
    int     schema_version;                /* manifest schema（当前为 2） */
    char    tool_version[32];              /* 产生该 manifest 的工具版本 */
    char    status[16];                    /* Running / Success / Incomplete / Failed */
    time_t  started_at;
    time_t  finished_at;
    int     baseline_eligible;             /* 0/1：可否作为盲信基准 */
    char    baseline_run_id[64];           /* 盲信运行时记录的其基准 run_id */
    time_t  baseline_completed_at;         /* 盲信基准完成时间（审计用） */
    int     pbin_schema_version;           /* pbin 记录格式版本（当前为 2） */
} ManifestInfo;

struct AppContext;

/* 读取 {base}.manifest（单行覆盖语义）。文件缺失/缺关键字段返回 false */
bool manifest_load(const char *base, ManifestInfo *out);

/* 启动时原子写 status=Running（.new → fsync → rename）；--clean 模式为空操作 */
void manifest_write_running(struct AppContext *ctx);

/* 收尾时原子写终态 manifest，并按严格条件计算 baseline_eligible。
 * dspill_residue_bytes 由调用方在删除 dspill 前统计；archive_ok 为
 * finalize_archive 的原子切换结果（未启用 archive 时传 true）。 */
void manifest_finalize(struct AppContext *ctx, long dspill_residue_bytes, bool archive_ok);

#endif
