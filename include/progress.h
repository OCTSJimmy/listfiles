#ifndef PROGRESS_H
#define PROGRESS_H

#include "config.h"
#include "smart_queue.h"

// 初始化进度系统
void progress_init(const Config *cfg, RuntimeState *state);

// 记录一个路径到进度文件
void record_path(const Config *cfg, RuntimeState *state, const char *path, const struct stat *info);
void refresh_progress(const Config *cfg, RuntimeState *state);

// 从进度文件恢复队列
void restore_progress(const Config *cfg, SmartQueue *queue, RuntimeState *state);

// 清理进度相关文件和状态
void cleanup_progress(const Config *cfg, RuntimeState *state);

// 原子性地更新索引文件
void atomic_update_index(const Config *cfg, RuntimeState *state);

// 归档已完成的进度分片
void archive_slice(const Config *cfg, const char *slice_path);

// 解压缩归档文件
void decompress_archive(const Config *cfg);

// 获取和释放文件锁
int acquire_lock(const Config *cfg, RuntimeState *state);
void release_lock(RuntimeState *state);

void save_config_to_disk(const Config* cfg);
char *get_index_filename(const char *base);

#endif // PROGRESS_H