/**
 * @file cmdline.c
 * @brief 命令行参数解析与配置初始化模块
 *
 * 负责解析用户传入的命令行选项，填充 Config 结构体，并进行基础合法性校验。
 * 支持 GNU getopt_long 长/短选项，提供 --help 和 --version 的快速响应。
 * 所有字符串型配置项均通过 strdup 动态分配，生命周期由 main 函数统一释放。
 */
#include "cmdline.h"
#include "utils.h"
#include "output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/stat.h>

/**
 * @brief  打印程序版本号到标准输出
 * @return void
 */
void show_version() {
    printf("listfiles 版本 %s\n", VERSION);
}

/**
 * @brief  打印程序帮助信息到标准输出
 * @return void
 *
 * @note   包含所有支持的命令行选项说明、默认值以及格式说明符参考。
 *         在 parse_arguments 中遇到 -h/--help 或参数错误时自动调用。
 */
void show_help() {
    printf("\n文件列表器 %s\n", VERSION);
    printf("递归列出文件及其元数据, 支持智能断点续传与半增量扫描\n\n");
    printf("用法: listfiles --path=路径 [选项]\n\n");
    printf("核心选项:\n");
    printf("  -p, --path=路径        要扫描的目标目录 (必须)\n");
    printf("  -c, --continue         启用智能续传/增量模式\n");
    printf("      --runone           强制全量扫描 (忽略历史进度)\n");
    printf("  -y, --yes              跳过启动时的交互式确认\n");
    printf("      --skip-interval=秒 设置半增量扫描的时间阈值 (默认: 0)\n");
    printf("      --batch-size=数量  Worker batch 大小 (默认: %d)\n", DEFAULT_BATCH_SIZE);
    printf("      --estimated-files=数量 预估文件数,用于预分配内存 (默认: %u)\n", (unsigned)DEFAULT_ESTIMATED_FILES);
    printf("      --master-threads=数量  Master 去重线程数 (默认: %d)\n", DEFAULT_MASTER_THREADS);
    printf("      --worker-count=数量  Worker 进程数 (默认: 自动, 上限 8)\n");
    printf("  -t, --timeout=秒       心跳超时时间 (默认: %d)\n", HEARTBEAT_TIMEOUT_SEC);
    printf("\n输出控制:\n");
    printf("  -f, --progress-file=文件 进度文件/历史记录前缀 (默认: progress)\n");
    printf("  -o, --output=文件      将结果写入指定文件 (默认: %s)\n", DEFAULT_OUTPUT_FILE);
    printf("  -O, --output-split=目录 将结果按行拆分到指定目录\n");
    printf("      --csv              启用标准 CSV 输出格式\n");
    printf("  -Q, --quote            对输出结果进行引号包裹\n");
    printf("  -D, --dirs             包含目录本身的信息\n");
    printf("  -d, --print-dir        打印目录路径到标准错误\n");
    printf("  -M, --mute             禁用所有输出\n");
    printf("\n格式化与元数据:\n");
    printf("  -F, --format=格式      自定义输出格式\n");
    printf("  --size, --user, --group, --mtime, --atime, --mode, --xattr\n");
    printf("  --follow-symlinks      跟踪符号链接\n");
    printf("\n高级/维护:\n");
    printf("  -Z, --archive          压缩已处理的进度分片\n");
    printf("  -C, --clean            删除已处理的进度分片\n");
    printf("  -R, --resume-from=文件 仅从指定的进度列表文件恢复 (预留，暂未实现)\n");
    printf("  --max-slice=行数       每个输出切片的最大行数\n");
    printf("  -v, --verbose          启用详细日志\n");
    printf("  -h, --help             显示此帮助信息\n");
}

/**
 * @brief  初始化配置结构体为默认值
 * @param  cfg  Config*  指向要初始化的配置结构体的指针，不能为空
 * @return void
 *
 * @note   所有指针型字段初始为 NULL 或由 strdup 分配默认字符串。
 *         数值型字段采用编译期宏定义的默认值（如 DEFAULT_BATCH_SIZE、DEFAULT_MASTER_THREADS 等）。
 *         调用本函数后，cfg 可直接传入 parse_arguments 进行覆盖更新。
 */
void init_config(Config *cfg) {
    memset(cfg, 0, sizeof(Config));
    cfg->progress_base = strdup("progress");
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
    cfg->mute = false;
    cfg->sure = false;
    cfg->runone = false;
    cfg->csv = false;
    cfg->quote = false;
    cfg->include_dir = false;
    cfg->heartbeat_timeout = HEARTBEAT_TIMEOUT_SEC;
    cfg->batch_size = DEFAULT_BATCH_SIZE;
    cfg->estimated_files = DEFAULT_ESTIMATED_FILES;
    cfg->master_threads = DEFAULT_MASTER_THREADS;
    cfg->worker_count = 0;
}

/**
 * @brief  解析命令行参数并填充 Config 结构体
 * @param  argc  int      命令行参数个数，取值范围: >= 1（argv[0] 为程序名）
 * @param  argv  char**   命令行参数字符串数组，不能为空
 * @param  cfg   Config*  指向要填充的配置结构体的指针，不能为空；调用前应先执行 init_config
 * @return int   返回 0 表示解析成功；返回 2 表示遇到 --help 或 --version（已输出信息，应正常退出）；
 *               返回 -1 表示参数错误（已输出错误信息，应异常退出）
 *
 * @note   本函数会对目标路径做 stat 校验（必须存在且为目录、普通文件或符号链接）。
 *         互斥选项校验：-o 与 -O 不能同时使用；-Z 与 -C 不能同时使用。
 *         若指定了 --format，解析结束后会自动调用 precompile_format 进行格式预编译。
 */
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
        {"batch-size", required_argument, 0, 23},
        {"estimated-files", required_argument, 0, 24},
        {"master-threads", required_argument, 0, 25},
        {"worker-count", required_argument, 0, 26},
        {"timeout", required_argument, 0, 't'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:cf:dvVF:ZCX:hO:o:QDRMyt:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'p': cfg->target_path = strdup(optarg); break;
            case 'c': cfg->continue_mode = true; break;
            case 'f': cfg->progress_base = strdup(optarg); break;
            case 'd': cfg->print_dir = true; break;
            case 'v': cfg->verbose = true; break;
            case 'V': show_version(); return 2;
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
            case 8:
                if (strcmp(optarg, "full") == 0 || strcmp(optarg, "0") == 0) {
                    cfg->verbose_type = VERBOSE_TYPE_FULL;
                } else if (strcmp(optarg, "versioned") == 0 || strcmp(optarg, "1") == 0) {
                    cfg->verbose_type = VERBOSE_TYPE_VERSIONED;
                } else {
                    fprintf(stderr, "无效的verbose类型: %s, 使用默认值\n", optarg);
                }
                break;
            case 9:
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
            case 20: cfg->runone = true; break;
            case 'y': cfg->sure = true; break;
            case 21:
                cfg->skip_interval = atol(optarg);
                break;
            case 22: cfg->csv = true; break;
            case 23:
                cfg->batch_size = atoi(optarg);
                if (cfg->batch_size <= 0) cfg->batch_size = DEFAULT_BATCH_SIZE;
                break;
            case 24:
                cfg->estimated_files = atol(optarg);
                if (cfg->estimated_files == 0) cfg->estimated_files = DEFAULT_ESTIMATED_FILES;
                break;
            case 25:
                cfg->master_threads = atoi(optarg);
                if (cfg->master_threads < 1) cfg->master_threads = DEFAULT_MASTER_THREADS;
                break;
            case 26:
                cfg->worker_count = atoi(optarg);
                if (cfg->worker_count < 1) cfg->worker_count = 0;
                break;
            case 't':
                cfg->heartbeat_timeout = atol(optarg);
                if (cfg->heartbeat_timeout <= 0) {
                    fprintf(stderr, "Error: Timeout must be positive.\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'h': show_help(); return 2;
            default:
                fprintf(stderr, "错误: 未知选项\n");
                show_help();
                return -1;
        }
    }

    if (!cfg->target_path) {
        fprintf(stderr, "错误: 必须指定目标路径\n");
        show_help();
        return -1;
    }

    struct stat path_stat;
    if (stat(cfg->target_path, &path_stat) != 0) {
        fprintf(stderr, "错误: 无法访问目标路径: %s\n", cfg->target_path);
        return -1;
    }
    if (!S_ISDIR(path_stat.st_mode) && !S_ISREG(path_stat.st_mode) && !(S_ISLNK(path_stat.st_mode) && cfg->follow_symlinks)) {
        fprintf(stderr, "错误: 无效的目标路径: %s (必须是目录、普通文件或符号链接)\n", cfg->target_path);
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
    }
    precompile_format(cfg);
    return 0;
}
