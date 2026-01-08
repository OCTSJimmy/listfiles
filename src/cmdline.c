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

// [修改点 1]：更新帮助信息，补充新功能的说明
void show_help() {
    printf("\n文件列表器 %s\n", VERSION);
    printf("递归列出文件及其元数据, 支持智能断点续传与半增量扫描\n\n");
    printf("用法: listfiles --path=路径 [选项]\n\n");
    
    printf("核心选项:\n");
    printf("  -p, --path=路径        要扫描的目标目录 (必须)\n");
    printf("  -c, --continue         启用智能续传/增量模式:\n");
    printf("                          - 若上次任务未完成: 继续扫描 (Resume)\n");
    printf("                          - 若上次任务已成功: 执行半增量扫描 (Incremental)\n");
    printf("      --runone           强制全量扫描 (忽略历史进度，相当于 Fresh Start)\n");
    printf("  -y, --yes              跳过启动时的交互式确认 (Non-interactive)\n");
    printf("      --skip-interval=秒 设置半增量扫描的时间阈值 (默认: 0)\n");
    printf("                          - 若文件元数据与历史一致且修改时间超过此阈值，则跳过lstat\n");
    
    printf("\n输出控制:\n");
    printf("  -f, --progress-file=文件 进度文件/历史记录前缀 (默认: progress)\n");
    printf("  -o, --output=文件      将结果写入指定文件 (默认: %s)\n", DEFAULT_OUTPUT_FILE);
    printf("  -O, --output-split=目录 将结果按行拆分到指定目录\n");
    printf("      --csv              启用标准 CSV 输出格式 (Quote all fields)\n");
    printf("  -Q, --quote            对输出结果进行引号包裹 (非 CSV 模式下的简单包裹)\n");
    printf("  -D, --dirs             包含目录本身的信息\n");
    printf("  -d, --print-dir        打印目录路径到标准错误 (实时进度)\n");
    printf("  -M, --mute             禁用所有输出\n");

    printf("\n格式化与元数据:\n");
    printf("  -F, --format=格式      自定义输出格式 (如 \"%%p|%%s|%%m\")\n");
    printf("                          %%p=路径, %%s=大小, %%u=用户, %%g=组, %%m=mtime\n");
    printf("  --size, --user, --group, --mtime, --atime, --mode, --xattr  启用特定元数据列\n");
    printf("  --follow-symlinks      跟踪符号链接\n");

    printf("\n高级/维护:\n");
    printf("  -Z, --archive          压缩已处理的进度分片 (归档)\n");
    printf("  -C, --clean            删除已处理的进度分片 (清理)\n");
    printf("  -R, --resume-from=文件 仅从指定的进度列表文件恢复 (旧版兼容模式)\n");
    printf("  --max-slice=行数       每个输出切片的最大行数\n");
    printf("  -v, --verbose          启用详细日志\n");
    printf("  -h, --help             显示此帮助信息\n");
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
        {"runone", no_argument, 0, 20},
        {"yes", no_argument, 0, 'y'},
        {"skip-interval", required_argument, 0, 21},
        {"csv", no_argument, 0, 22},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "p:cf:dvVF:ZCX:hO:o:QDRMy", long_options, NULL)) != -1) {
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
            case 'M': cfg->mute = true; break;             // [修改点 4]：处理新参数
            case 20: cfg->runone = true; break;            // --runone
            case 'y': cfg->sure = true; break;             // --yes
            case 21:                                       // --skip-interval
                cfg->skip_interval = atol(optarg);
                break;
            case 22: cfg->csv = true; break;               // --csv
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