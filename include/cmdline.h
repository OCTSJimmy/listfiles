#ifndef CMDLINE_H
#define CMDLINE_H

#include "config.h"

// 初始化配置默认值
void init_config(Config *cfg);

// 解析命令行参数
// 返回值: 0 表示成功, -1 表示应退出 (如 --help 或参数错误)
int parse_arguments(int argc, char *argv[], Config *cfg);

void show_version();
void show_help();

#endif // CMDLINE_H