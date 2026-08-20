#ifndef PROGRESS_H
#define PROGRESS_H

#include "config.h"
#include "archive_format.h"
#include "app_context.h"
#include "spbin.h"

/* pbin / spbin 写入 */
void record_path(const Config *cfg, RuntimeState *state, const char *path, const struct stat *info);
/* v15.6.0（P0-005）：spbin 新格式落盘（[path_len:u32][path][reason:u8][timestamp:time_t][device_key:64B]） */
void spbin_file_write_entry(FILE *fp, const SpbinEntry *entry);
void spbin_file_append(const char *progress_base, const SpbinEntry *entry);
/* v15.6.0（P0-005）：spbin 统一写入入口——内存缓存 + spbin_set + 磁盘 append-only */
void spbin_write_record(AppContext *ctx, const char *path, uint8_t reason, dev_t dev);
/* v15.6.0（P0-005）：spbin compaction——过滤已恢复（RECOVERED）条目重写 {base}.spbin */
void spbin_compact(AppContext *ctx);

/* 批量缓冲 */
void record_path_batch_init(RecordBatch *batch);
void record_path_batch_flush(const Config *cfg, RuntimeState *state, RecordBatch *batch);
bool record_path_batch_append(const Config *cfg, RuntimeState *state, RecordBatch *batch, const char *path, const struct stat *info);

/* 归档与完成 */
void process_old_slice(const Config *cfg, unsigned long index);
/* v15.6.0（P0-008）：返回 archive 原子切换校验结果（未启用 archive 时恒 true） */
bool finalize_archive(const Config *cfg, RuntimeState *state);

/* 恢复 */
int restore_progress(const Config *cfg, AppContext *ctx);
/* v15.6.0（P0-011）：盲信基准加载，base 为基准 progress 前缀（--reference-base） */
void restore_progress_to_memory(const Config *cfg, AppContext *ctx, const char *base);
void pump_pbin_batch(AppContext *ctx, int batch_size);
void fpbin_append(AppContext *ctx, const char *path, const struct stat *st);

/* 配置与生命周期 */
void save_config_to_disk(const Config* cfg);
/* v15.6.0（P0-008）：返回 archive 原子切换校验结果（未启用 archive/--clean 时恒 true），供 manifest 判定 */
bool finalize_progress(const Config *cfg, RuntimeState *state);
void cleanup_progress(const Config *cfg, RuntimeState *state);

/* 锁 */
int acquire_lock(const Config *cfg, RuntimeState *state);
void release_lock(RuntimeState *state);

/* 文件名辅助 */
char *get_slice_filename(const char *base, unsigned long index);
char *get_archive_filename(const char *base);
char *get_archive_new_filename(const char *base);  /* v15.6.0（P0-008）：原子切换在写副本 {base}.archive.new */
char *get_archive_prev_filename(const char *base); /* v15.6.0（P0-008）：旧基准保留 {base}.archive.prev */
char *get_spbin_filename(const char *base);
char *get_fpbin_slice_filename(const char *base, unsigned long index);
char *get_fpbin_index_filename(const char *base);
char *get_dpbin_slice_filename(const char *base, unsigned long index);
char *get_dfpbin_slice_filename(const char *base, unsigned long index); /* v15.6.0: P0-003 dfpbin */
char *get_dspill_filename(const char *base); /* v15.5.8: dspill 派发兜底文件 */

/* Footer 读写与校验 */
bool write_pbin_footer(FILE *fp, uint64_t row_count);
bool pbin_salvage_truncated(const char *path, uint64_t *out_valid_rows);
bool read_pbin_footer(const char *path, PbinFooter *out);
bool verify_pbin_footer(const PbinFooter *f);
unsigned long get_slice_row_count(const Config *cfg, unsigned long index);

/* pbin 底层写入与读取（内部使用，跨文件可见） */
void write_pbin_record(FILE *fp, const char *path, const struct stat *info);
bool read_next_pbin_record(FILE *fp, char **out_path, struct stat *out_st, unsigned char *out_d_type);
void fpbin_open_slice(AppContext *ctx);

/* dpbin 完成日志（本次会话临时） */
void dpbin_open_slice(AppContext *ctx);
void dpbin_rotate_slice(AppContext *ctx);
void dpbin_append(AppContext *ctx, const char *path, const struct stat *st);
void dpbin_delete_all(const char *progress_base);

/* v15.6.0（P0-003）：dfpbin 完成日志（HIST_PUMP_OLD 阶段，fpbin 原子对） */
void dfpbin_append(AppContext *ctx, const char *path, const struct stat *st);
void dfpbin_delete_all(const char *progress_base);
/* v15.6.0（P0-003）：fpbin 内存缓冲 + 活跃分片刷盘接口（dfpbin_append 前必须调用） */
void fpbin_flush(AppContext *ctx);

/* Spbin memory cache */
void spbin_append(AppContext *ctx, const SpbinEntry *entry);
void spbin_requeue_recovered(AppContext *ctx, dev_t dev);

#endif
