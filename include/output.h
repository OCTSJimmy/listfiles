#ifndef OUTPUT_H
#define OUTPUT_H

#include "config.h"

// 预编译自定义格式字符串
void precompile_format(Config *cfg);

// 清理预编译的格式
void cleanup_compiled_format(Config *cfg);

// [核心接口] 直接将文件信息格式化并输出到文件流
// 替代旧的 format_output，避免中间缓冲区拷贝，支持 CSV 转义
void print_to_stream(const Config *cfg, RuntimeState *state, const char *path, const struct stat *st, FILE *fp);

// 格式化并输出一行结果
void format_output(const Config *cfg, RuntimeState *state, const char *path, const struct stat *st, char *buffer, size_t size);

// 初始化输出文件（包括普通输出和分片输出）
void init_output_files(const Config *cfg, RuntimeState *state);

// 执行切片轮转
void rotate_output_slice(const Config *cfg, RuntimeState *state);

void cleanup_cache(RuntimeState *state);
void close_output_file(FILE *fp);
void format_mode_str(mode_t mode, char *buf);

#endif // OUTPUT_H