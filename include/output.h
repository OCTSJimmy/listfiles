#ifndef OUTPUT_H
#define OUTPUT_H

#include "config.h"

// 预编译自定义格式字符串
void precompile_format(Config *cfg);

// 清理预编译的格式
void cleanup_compiled_format(Config *cfg);

// 格式化并输出一行结果
void format_output(const Config *cfg, RuntimeState *state, const char *path, const struct stat *info);

// 初始化输出文件（包括普通输出和分片输出）
void init_output_files(const Config *cfg, RuntimeState *state);

// 执行切片轮转
void rotate_output_slice(const Config *cfg, RuntimeState *state);

void cleanup_cache(RuntimeState *state);
void close_output_file(FILE *fp);
void format_mode_str(mode_t mode, char *buf);

#endif // OUTPUT_H