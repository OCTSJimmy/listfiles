#ifndef PROGRESS_H
#define PROGRESS_H

#include "config.h"
#include "archive_format.h"
#include "app_context.h"
#include "spbin.h"

/* pbin / spbin 写入 */
void record_path(const Config *cfg, RuntimeState *state, const char *path, const struct stat *info);
void record_skip(const Config *cfg, RuntimeState *state, const SpbinEntry *entry);

/* 批量缓冲 */
void record_path_batch_init(RecordBatch *batch);
void record_path_batch_flush(const Config *cfg, RuntimeState *state, RecordBatch *batch);
bool record_path_batch_append(const Config *cfg, RuntimeState *state, RecordBatch *batch, const char *path, const struct stat *info);

/* 归档与完成 */
void process_old_slice(const Config *cfg, unsigned long index);
void finalize_archive(const Config *cfg, RuntimeState *state);

/* 恢复 */
int restore_progress(const Config *cfg, AppContext *ctx);
void restore_progress_to_memory(const Config *cfg, AppContext *ctx);
void pump_pbin_batch(AppContext *ctx, int batch_size);
void fpbin_append(AppContext *ctx, const char *path, const struct stat *st);

/* 配置与生命周期 */
void save_config_to_disk(const Config* cfg);
void finalize_progress(const Config *cfg, RuntimeState *state);
void cleanup_progress(const Config *cfg, RuntimeState *state);

/* 锁 */
int acquire_lock(const Config *cfg, RuntimeState *state);
void release_lock(RuntimeState *state);

/* 文件名辅助 */
char *get_slice_filename(const char *base, unsigned long index);
char *get_archive_filename(const char *base);
char *get_spbin_filename(const char *base);
char *get_fpbin_slice_filename(const char *base, unsigned long index);
char *get_fpbin_index_filename(const char *base);
char *get_dpbin_slice_filename(const char *base, unsigned long index);

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

/* Spbin memory cache */
void spbin_append(AppContext *ctx, const SpbinEntry *entry);
void spbin_requeue_recovered(AppContext *ctx, dev_t dev);

#endif
