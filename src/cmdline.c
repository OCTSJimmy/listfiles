#include "cmdline.h"
#include "utils.h"
#include "output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/stat.h>

void show_version() {
    printf("listfiles 版本 %s\n", VERSION);
}

void show_help() {
    printf("\n文件列表器 %s\n", VERSION);
    printf("递归列出文件及其元数据,支持断点续传\n\n");
    printf("用法: listfiles --path=路径 [选项]\n\n");
    printf("选项:\n");
    printf("  -p, --path=路径        要扫描的目标目录 (必须)\n");
    printf("  -c, --continue         启用断点续传模式\n");
    printf("  -f, --progress-file=文件 进度文件前缀 (默认: progress)\n");
    printf("  -d, --print-dir        打印目录路径到标准错误\n");
    printf("  -v, --verbose          启用详细输出\n");
    printf("  -V, --version          显示版本信息\n");
    printf("  -F, --format=格式      自定义输出格式,支持占位符:\n");
    printf("                          %%p = 路径, %%s = 大小, %%u = 用户, %%g = 组\n");
    printf("                          %%m = 修改时间, %%a = 访问时间, %%M = 权限模式\n");
    printf("                          %%x = 扩展属性\n\n");
    printf("  -o, --output=文件      将结果写入指定文件 (默认: %s)\n", DEFAULT_OUTPUT_FILE);
    printf("  -O, --output-split=目录 将结果按行拆分到指定目录 (默认: %lu)\n",(long unsigned int) DEFAULT_OUTPUT_SPLIT_DIR);
    printf("      --max-slice=行数   每个输出切片的最大行数 (默认: %lu)\n", (long unsigned int) DEFAULT_OUTPUT_SLICE_LINES);
    printf("  -Z, --archive          压缩已处理的进度文件切片, 归档后会删掉原文件\n");
    printf("  -C, --clean            删除已处理的进度文件切片, 不归档\n");
    printf("      --decompress       解压缩归档文件并输出内容\n");
    printf("  -Q, --quote            对输出结果进行引号包裹\n");
    printf("  -D, --dirs             包含目录信息\n");
    printf("  -R, --resume-from=文件 从指定的进度文件恢复\n");
    printf("  -M, --mute             禁用所有输出\n");
    printf("注意: -o与-O互斥, -Z与-C互斥\n");
    printf("verbose控制:\n");
    printf("  --verbose-type=类型    控制verbose输出类型 (0/1,0: Full, 1:Versioned, Default: 0)\n");
    printf("  --verbose-level=级别   控制verbose输出级别 (0-999999999)\n");
    printf("元数据标志 (被--format覆盖):\n");
    printf("  --size                 包含文件大小\n");
    printf("  --user                 包含所有者用户\n");
    printf("  --group                包含所有者组\n");
    printf("  --mtime                包含修改时间\n");
    printf("  --atime                包含访问时间\n");
    printf("  --mode                 包含权限模式\n");
    printf("  --xattr                包含扩展属性\n");
    printf("  --follow-symlinks      跟踪符号链接\n\n");
    printf("示例:\n");
    printf("  listfiles -p /data --continue --format=\"%%p|%%s|%%u|%%m\"\n");
    printf("  listfiles -p /home --size --user --mtime > 文件列表.csv\n");
    printf("  listfiles -p /home -D --mode --xattr --quote\n");
    printf("  listfiles -p /data -c -R resume_file --output-split=output/\n");
}

void init_config(Config *cfg) {
    cfg->progress_base = "progress";
    cfg->compiled_format = NULL;
    cfg->format_segment_count = 0;
    cfg->is_output_file = false;
    cfg->is_output_split_dir = false;
    cfg->output_file = NULL;
    cfg->output_split_dir = NULL;
    cfg->progress_slice_lines = DEFAULT_PROGRESS_SLICE_LINES;
    cfg->output_slice_lines = DEFAULT_OUTPUT_SLICE_LINES;
    cfg->archive = false;
    cfg->clean = false;
    cfg->decompress = false;
    cfg->verbose_type = VERBOSE_TYPE_FULL;
    cfg->verbose_level = DEFAULT_VERBOSE_LEVEL;
}

int parse_arguments(int argc, char *argv[], Config *cfg) {
    static struct option long_options[] = {
        {"path", required_argument, 0, 'p'},
        {"continue", no_argument, 0, 'c'},
        {"progress-file", required_argument, 0, 'f'},
        {"print-dir", no_argument, 0, 'd'},
        {"verbose", no_argument, 0, 'v'},
        {"version", no_argument, 0, 'V'},
        {"format", required_argument, 0, 'F'},
        {"size", no_argument, 0, 1},
        {"user", no_argument, 0, 2},
        {"group", no_argument, 0, 3},
        {"mtime", no_argument, 0, 4},
        {"atime", no_argument, 0, 5},
        {"follow-symlinks", no_argument, 0, 6},
        {"help", no_argument, 0, 'h'},
        {"max-slice", required_argument, 0, 7},
        {"archive", no_argument, 0, 'Z'},
        {"clean", no_argument, 0, 'C'},
        {"decompress", no_argument, 0, 'X'},
        {"output", required_argument, 0, 'o'},
        {"output-split", required_argument, 0, 'O'},
        {"verbose-type", required_argument, 0, 8},
        {"verbose-level", required_argument, 0, 9},
        {"mode", no_argument, 0, 10},
        {"xattr", no_argument, 0, 11},
        {"quote", no_argument, 0, 'Q'},
        {"dirs", no_argument, 0, 'D'},
        {"resume-from", required_argument, 0, 'R'},
        {"mute", no_argument, 0, 'M'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "p:cf:dvVF:ZCX:hO:o:QDRM", long_options, NULL)) != -1) {
        switch (opt) {
            case 'p': cfg->target_path = strdup(optarg); break;
            case 'c': cfg->continue_mode = true; break;
            case 'f': cfg->progress_base = strdup(optarg); break;
            case 'd': cfg->print_dir = true; break;
            case 'v': cfg->verbose = true; break;
            case 'V': show_version(); return -1; // -1 表示调用者应直接退出
            case 'F': cfg->format = strdup(optarg); break;
            case 1: cfg->size = true; break;
            case 2: cfg->user = true; break;
            case 3: cfg->group = true; break;
            case 4: cfg->mtime = true; break;
            case 5: cfg->atime = true; break;
            case 6: cfg->follow_symlinks = true; break;
            case 7: 
                cfg->output_slice_lines = atol(optarg);
                if (cfg->output_slice_lines <= 0) {
                    fprintf(stderr, "错误: 分片大小必须大于零\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 8: // verbose-type
                if (strcmp(optarg, "full") == 0 || strcmp(optarg, "0") == 0) {
                    cfg->verbose_type = VERBOSE_TYPE_FULL;
                } else if (strcmp(optarg, "versioned") == 0 || strcmp(optarg, "1") == 0) {
                    cfg->verbose_type = VERBOSE_TYPE_VERSIONED;
                } else {
                    fprintf(stderr, "无效的verbose类型: %s, 使用默认值\n", optarg);
                }
                break;
            case 9: // verbose-level
                cfg->verbose_level = atoi(optarg);
                if (cfg->verbose_level < 0) cfg->verbose_level = DEFAULT_VERBOSE_LEVEL;
                break;
            case 10: cfg->mode = true; break;
            case 11: cfg->xattr = true; break;
            case 'Z': cfg->archive = true; break;
            case 'C': cfg->clean = true; break;
            case 'X': cfg->decompress = true; return -1;
            case 'o': 
                cfg->is_output_file = true; 
                cfg->output_file = strdup(optarg); 
                break;
            case 'O': 
                cfg->is_output_split_dir = true; 
                cfg->output_split_dir = strdup(optarg); 
                break;
            case 'Q': cfg->quote = true; break;
            case 'D': cfg->include_dir = true; break;
            case 'R': 
                cfg->resume_file = strdup(optarg);
                if (!cfg->continue_mode) {
                    cfg->continue_mode = true;
                    if (!cfg->progress_base) cfg->progress_base = "resume_task";
                }
                break;
            case 'M': cfg->mute = true; break;
            case 'h': default: show_help(); return -1;
        }
    }

    if (!cfg->target_path) {
        fprintf(stderr, "错误: 必须指定目标路径\n");
        show_help();
        return -1;
    }

    struct stat path_stat;
    if (stat(cfg->target_path, &path_stat) != 0 || !S_ISDIR(path_stat.st_mode)) {
        fprintf(stderr, "错误: 无效的目标路径: %s\n", cfg->target_path);
        return -1;
    }

    if (cfg->is_output_file && cfg->is_output_split_dir) {
        fprintf(stderr, "错误: -o 与 -O 不能同时使用\n");
        return -1;
    }

    if (cfg->archive && cfg->clean) {
        fprintf(stderr, "错误: -Z 与 -C 不能同时使用\n");
        return -1;
    }

    if (cfg->format) {
        verbose_printf(cfg, 1, "预编译输出格式: %s\n", cfg->format);
        precompile_format(cfg);
    }
    return 0;
}