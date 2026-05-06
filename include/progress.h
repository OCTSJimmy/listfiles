#ifndef PROGRESS_H
#define PROGRESS_H

#include "config.h"
#include "archive_format.h"
#include "app_context.h"
#include "spbin.h"

/* pbin / spbin 写入 */
void record_path(const Config *cfg, RuntimeState *state, const char *path, const struct stat *info);
void record_skip(const Config *cfg, RuntimeState *state, const SpbinEntry *entry);

/* 索引与游标 */
void atomic_update_index(const Config *cfg, RuntimeState *state);
bool load_progress_index(const Config *cfg, RuntimeState *state);

/* 归档 */
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
char *get_index_filename(const char *base);
char *get_slice_filename(const char *base, unsigned long index);
char *get_archive_filename(const char *base);
char *get_spbin_filename(const char *base);
char *get_per_slice_index_filename(const char *base, unsigned long index);
char *get_fpbin_slice_filename(const char *base, unsigned long index);
char *get_fpbin_index_filename(const char *base);

/* Footer 读写与校验 */
bool write_pbin_footer(FILE *fp, uint64_t row_count);
bool read_pbin_footer(const char *path, PbinFooter *out);
bool verify_pbin_footer(const PbinFooter *f);
unsigned long get_slice_row_count(const Config *cfg, unsigned long index);

/* Spbin memory cache */
void spbin_append(AppContext *ctx, const SpbinEntry *entry);
void spbin_requeue_recovered(AppContext *ctx, dev_t dev);

#endif
