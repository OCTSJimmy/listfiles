#include "config.h"
#include "utils.h"
#include "signals.h"
#include "progress.h"
#include "output.h"
#include "idempotency.h"
#include "smart_queue.h"
#include "async_worker.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <dirent.h>
#include <sys/time.h>
#include <ctype.h>

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
    printf("                          %%m = 修改时间, %%a = 访问时间, %%M = 模式, %%X = 扩展属性\n\n");
    printf("  -o, --output=文件      将结果写入指定文件 (默认: %s)\n", DEFAULT_OUTPUT_FILE);
    printf("  -O, --output-split=目录 将结果按行拆分到指定目录 (默认: %lu)\n",(long unsigned int) DEFAULT_OUTPUT_SPLIT_DIR);
    printf("      --max-slice=行数   每个输出切片的最大行数 (默认: %lu)\n", (long unsigned int) DEFAULT_OUTPUT_SLICE_LINES);
    printf("  -Z, --archive          压缩已处理的进度文件切片, 归档后会删掉原文件\n");
    printf("  -C, --clean            删除已处理的进度文件切片, 不归档\n");
    printf("      --decompress       解压缩归档文件并输出内容\n");
    printf("  -D, --dirs            在输出结果中包含目录 (默认仅包含文件)\n\n");
    printf("  -R, --resume-from=文件 从指定的目录列表文件(如output.dir)恢复任务 (覆盖旧进度)\n");
    printf("  -M, --mute            静默模式，不输出到屏幕，而是刷新到 .status 文件\n\n");
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
    printf("  --follow-symlinks      跟踪符号链接\n\n");
    printf("  --mode                包含文件模式 (如 -rw-r--r--)\n\n");
    printf("  --xattr           包含扩展属性 (lsattr格式, 失败显示 [error])\n\n");
    printf("  -Q, --quote           给所有输出变量加上双引号 (如 \"/path/to/file\")\n\n"); // <--- 新增
    printf("示例:\n");
    printf("  listfiles -p /data --continue --format=\"%%p|%%s|%%u|%%m\"\n");
    printf("  listfiles -p /home --size --user --mtime > 文件列表.csv\n");
}
// === QoS 辅助函数 ===
static long long current_timestamp() {
    struct timeval te; 
    gettimeofday(&te, NULL); // gettimeofday 开销极小
    return te.tv_sec*1000000LL + te.tv_usec;
}

// =======================================================
// 新函数：parse_arguments
// =======================================================
// 返回值：0 表示成功继续执行, -1 表示应立即退出 (如 --help)
static int parse_arguments(int argc, char *argv[], Config *cfg) {
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
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "p:cf:dvVF:ZCX:hO:o:QD", long_options, NULL)) != -1) {
        switch (opt) {
            case 'p': 
                cfg->target_path = strdup(optarg);
                break;
            case 'c':
                cfg->continue_mode = true;
                break;
            case 'f':
                cfg->progress_base = strdup(optarg);
                break;
            case 'd':
                cfg->print_dir = true;
                break;
            case 'v':
                cfg->verbose = true;
                break;
            case 'V':
                show_version();
                return 0;
            case 'F':
                cfg->format = strdup(optarg);
                break;
            case 1: // --size
                cfg->size = true;
                break;
            case 2: // --user
                cfg->user = true;
                break;
            case 3: // --group
                cfg->group = true;
                break;
            case 4: // --mtime
                cfg->mtime = true;
                break;
            case 5: // --atime
                cfg->atime = true;
                break;
            case 6: // --follow-symlinks
                cfg->follow_symlinks = true;
                break;
            case 7: // --max-slice
                cfg->output_slice_lines = atol(optarg);
                if (cfg->output_slice_lines <= 0) {
                    fprintf(stderr, "错误: 分片大小必须大于零\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 8: // --verbose-type
                if (strcmp(optarg, "full") == 0 || strcmp(optarg, "0") == 0) {
                    cfg->verbose_type = VERBOSE_TYPE_FULL;
                } else if (strcmp(optarg, "versioned") == 0 || strcmp(optarg, "1") == 0) {
                    cfg->verbose_type = VERBOSE_TYPE_VERSIONED;
                } else {
                    fprintf(stderr, "无效的verbose类型: %s, 使用默认值: %d\n", optarg, VERBOSE_TYPE_FULL);
                }
                break;
            case 9: // --verbose-level
                cfg->verbose_level = atoi(optarg);
                if (cfg->verbose_level < 0) {
                    fprintf(stderr, "无效的verbose级别: %s, 使用默认值\n", optarg);
                    cfg->verbose_level = DEFAULT_VERBOSE_LEVEL;
                }
                break;
            case 10: // --mode
                cfg->mode = true;
                break;
            case 11: // --xattr
                cfg->xattr = true;
                break;
            case 'Z': // --archive
                cfg->archive = true;
                break;
            case 'C': // --clean
                cfg->clean = true;
                break;
            case 'X': // --decompress
                cfg->decompress = true;
                return 0;
            case 'o':
                cfg->is_output_file = true;
                cfg->output_file = strdup(optarg);
                break;
            case 'O':
                cfg->is_output_split_dir = true;
                cfg->output_split_dir = strdup(optarg);
                break;
            case 'Q': 
                cfg->quote = true;
                break;
            case 'D': // --dirs
                cfg->include_dir = true;
                break;
            case 'R': // <--- 新增
                cfg->resume_file = strdup(optarg);
                // 强制开启断点续传模式，否则无法利用 HashSet 进行父子关系去重
                if (!cfg->continue_mode) {
                    cfg->continue_mode = true;
                    // 如果用户没指定进度文件基名，给个默认的，防止逻辑错误
                    if (!cfg->progress_base) cfg->progress_base = "resume_task";
                }
                break;
            case 'M': // <--- 新增
                cfg->mute = true;
                break;
            case 'h':
            default:
                show_help();
                return -1;
        }
    }
    // --- 参数合法性检查 ---
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
        fprintf(stderr, "错误: -o/--output与-O/--output-split选项不能同时使用\n");
        return -1;
    }

    if (cfg->archive && cfg->clean) {
        fprintf(stderr, "错误: -Z/--archive与-C/--clean选项不能同时使用\n");
        return -1;
    }
    // 如果用户指定了格式字符串，必须先编译它，否则 output 模块无法识别
    if (cfg->format) {
        verbose_printf(cfg, 1, "预编译输出格式: %s\n", cfg->format);
        precompile_format(cfg);
    }
    return 0; // 表示成功
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
    cfg->verbose_type = VERBOSE_TYPE_FULL;  // 默认全量输出
    cfg->verbose_level = DEFAULT_VERBOSE_LEVEL;  // 默认输出所有级别
}
// 判断是否为绝对路径(Windows平台)
bool is_absolute_path(const char* path) {
    return path != NULL && path[0] == '/';
}

// 从文件列表加载任务
static void load_resume_file(const Config *cfg, SmartQueue *queue) {
    FILE *fp = fopen(cfg->resume_file, "r");
    if (!fp) {
        perror("无法打开恢复列表文件");
        exit(EXIT_FAILURE);
    }

    verbose_printf(cfg, 1, "正在从 %s 加载任务列表...\n", cfg->resume_file);

    char line[MAX_PATH_LENGTH + 256];
    unsigned long loaded_count = 0;
    char path_buf[MAX_PATH_LENGTH]; 
    size_t prefix_len = strlen(OUTPUT_DIR_PREFIX);


    while (fgets(line, sizeof(line), fp)) {
        // 1. 去除尾部换行符
        line[strcspn(line, "\n")] = 0;
        
        char *path_start = line;
        while (isspace((unsigned char)*path_start)) path_start++;

        if (strncmp(path_start, OUTPUT_DIR_PREFIX, prefix_len) == 0) {
            path_start += prefix_len;
        }

        size_t len = strlen(path_start);
        if (len > 1 && path_start[0] == '"' && path_start[len-1] == '"') {
            size_t inner_len = len - 2;
            if (inner_len >= sizeof(path_buf)) inner_len = sizeof(path_buf) - 1;
            strncpy(path_buf, path_start + 1, inner_len);
            path_buf[inner_len] = '\0';
            path_start = path_buf;
        }

       if (*path_start == '\0') continue;
        // === 变更点 ===
        // 直接盲入队，不做 lstat，不查重，不记录
        blind_enqueue(queue, path_start);
        loaded_count++;
        // =============
    }

    fclose(fp);
    verbose_printf(cfg, 1, "列表加载完成，队列预热 %lu 项 (将在运行时动态验证)。\n", loaded_count);
    
}

static void process_directory(Config* cfg, RuntimeState* state, SmartQueue* queue, const char* dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        verbose_printf(cfg, 1, "无法打开目录: %s\n", dir_path);
        return;
    }

    // === QoS 状态变量 ===
    long long current_sleep = START_SLEEP_US; 
    long long op_start, op_end, duration;
    
    struct dirent *child_entry;
    while (true) {
        // QoS: 测量 readdir 耗时作为压力探针
        op_start = current_timestamp();
        child_entry = readdir(dir);
        op_end = current_timestamp();

        if (child_entry == NULL) break;

        // QoS: 动态反馈调节
        duration = op_end - op_start;
        if (duration < 2000) { 
            // <2ms: 存储很闲，加速 (减少5%睡眠)
            current_sleep = current_sleep * 0.95;
            if (current_sleep < MIN_SLEEP_US) current_sleep = MIN_SLEEP_US;
        } else if (duration > 100000) { 
            // >100ms: 存储卡了，急刹车 (翻倍+50ms)
            current_sleep = (current_sleep * 2) + 50000;
        } else if (duration > 10000) {
            // >10ms: 有点压力，缓慢减速 (增加10%)
            current_sleep = current_sleep * 1.1;
        }
        if (current_sleep > MAX_SLEEP_US) current_sleep = MAX_SLEEP_US;

        // 执行限流
        if (current_sleep > 0) usleep(current_sleep);


        // 跳过 . 和 ..
        if (child_entry->d_name[0] == '.' && 
           (child_entry->d_name[1] == '\0' || (child_entry->d_name[1] == '.' && child_entry->d_name[2] == '\0'))) {
            continue;
        }

        char full_path[MAX_PATH_LENGTH];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, child_entry->d_name);
        if (n >= MAX_PATH_LENGTH) continue;

        bool is_dir = false;
        bool need_stat = false;
        struct stat info;
        memset(&info, 0, sizeof(info));

        if (cfg->size || cfg->mtime || cfg->atime || cfg->user || cfg->group || cfg->continue_mode || cfg->format) {
            need_stat = true;
        }

        // d_type 优化
        if (child_entry->d_type != DT_UNKNOWN) {
            if (child_entry->d_type == DT_DIR) is_dir = true;
            if (need_stat) {
                if (lstat(full_path, &info) != 0) {
                    verbose_printf(cfg, 1, "无法获取文件状态: %s\n", full_path);
                    continue;
                }
            }
        } else {
            if (lstat(full_path, &info) != 0) continue;
            if (S_ISDIR(info.st_mode)) is_dir = true;
        }

        if (is_dir) {
            smart_enqueue(cfg, queue, full_path, &info);
            if (cfg->continue_mode) {
                record_path(cfg, state, full_path, &info);
            }
            state->dir_count++;
            if (cfg->print_dir) {
                fprintf(state->dir_info_fp, OUTPUT_DIR_PREFIX "%s\n", full_path);
            }
            if (cfg->include_dir) {
                push_write_task_file(full_path, &info);
            }
        } else {
            // 直接推给 Worker，stat 信息已在 info 中（如果 need_stat 为真）
            // 注意：目前的 push_write_task_file 只接受 path，worker 内部会再次 lstat
            // 为了保持架构一致，这里暂不传递 info，如果追求极致性能，以后可以改造 worker 接受 info
            push_write_task_file(full_path, &info);
        }
    }
    
    closedir(dir);
}

/**********************
 * 主遍历函数
 **********************/

void traverse_files(Config *cfg, RuntimeState *state) {
    SmartQueue queue;
    init_smart_queue(&queue);

    // ===【修复点1：启动异步线程】===
    // 必须在开始生产数据前启动消费者
    verbose_printf(cfg, 1, "启动异步写入流水线...\n");
    async_worker_init(cfg, state);

    ThreadSharedState shared = {
        .cfg = cfg,
        .state = state,
        .queue = &queue,
        .running = 1
    };
    pthread_t status_thread;
    int thread_rc = pthread_create(&status_thread, NULL, status_thread_func, &shared);
    if (thread_rc != 0) {
        fprintf(stderr, "创建监控线程失败: %d\n", thread_rc);
        shared.running = 0;
    }

    if (cfg->continue_mode) {
        progress_init(cfg, state);
        restore_progress(cfg, &queue, state);
    }
    
    struct stat root_info;
    if (lstat(cfg->target_path, &root_info) != 0) {
        perror("无法访问目标路径");
        exit(EXIT_FAILURE);
    }

    if (cfg->continue_mode) {
        if (cfg->resume_file) {
            // 场景 A: 导入模式 (-R)
            // 1. 彻底清理旧的进度文件 (Abandon old progress)
            verbose_printf(cfg, 1, "检测到导入模式 (-R)，正在清理旧的进度文件...\n");
            cleanup_progress(cfg, state);
            
            // 2. 重置内存状态 (保险起见)
            state->process_slice_index = 0;
            state->processed_count = 0;
            state->write_slice_index = 0;
            // 注意：不要重置 output_slice_num，因为那属于输出文件的逻辑，
            // 但如果用户希望全量重跑，通常也会清理 output。
            // 这里我们只负责重置“进度”相关的。
            
            // 3. 初始化全新的进度系统 (Create new .idx / .pbin)
            progress_init(cfg, state);
            
            // 4. 加载并转录
            load_resume_file(cfg, &queue); // <--- 传入 state
            
        } else {
            // 场景 B: 常规续传模式
            progress_init(cfg, state);
            restore_progress(cfg, &queue, state);
        }
    }
    // ===========================
    
    // === 根目录处理逻辑调整 ===
    // 如果是 -R 模式，根目录已经在 load_resume_file 里被处理了（如果它在列表里）
    // 如果列表里没有根目录，通常意味着不需要扫根目录
    // 所以这里只需要处理“非 -R” 的情况
    
    if (!cfg->resume_file) { // <--- 只有非 -R 模式才去扫 -p 参数指定的根
        struct stat root_info;
        if (lstat(cfg->target_path, &root_info) != 0) { /* ... */ }

        if (S_ISDIR(root_info.st_mode)) {
            // ... (入队根目录)
        } else {
            // ... (处理单文件)
        }
    }
    while (true) {
        ScanNode *entry = smart_dequeue(cfg, &queue, state);
        if (!entry) break;
// ===【新增】运行时二次安检 (Lazy Validation) ===
        if (!entry->pre_checked) {
            // 这是一个“盲注”节点，必须补票：lstat + 哈希检查 + 转录
            struct stat info;
            
            // 1. 补做 lstat (IO 操作，自然受 worker 单线程速度限制)
            if (lstat(entry->path, &info) != 0) {
                // 文件可能被删了，跳过
                verbose_printf(cfg, 2, "跳过失效的恢复路径: %s\n", entry->path);
                recycle_scan_node(&queue, entry);
                continue; 
            }

            // 2. 补做去重
            ObjectIdentifier id = { .st_dev = info.st_dev, .st_ino = info.st_ino };
            if (hash_set_contains(g_history_object_set, &id)) {
                // 已经处理过了 (比如父目录已经扫到了它)，跳过
                recycle_scan_node(&queue, entry);
                continue;
            }
            hash_set_insert(g_history_object_set, &id);

            // 3. 补做转录 (如果开启断点续传)
            // 因为之前盲加载没写 .pbin，现在确认有效了，赶紧写进去
            if (cfg->continue_mode) {
                record_path(cfg, state, entry->path, &info);
            }
            
            // 4. 补做目录输出 (如果开启 -D)
            // 之前 blind_enqueue 没做这步
            if (cfg->include_dir) {
                push_write_task_file(entry->path, &info);
            }
        }
        state->current_path = entry->path;
        process_directory(cfg, state, &queue, entry->path);
        
        // ===【架构修改：提交进度快照】===
        // 当一个目录处理完，且 continue_mode 开启时，我们生成一个检查点
        // 这个检查点携带了当前的 state->process_slice_index 等进度信息
        // 它会进入队列排队，排在刚才 process_directory 产生的所有文件之后
        if (cfg->continue_mode) {
            push_write_task_checkpoint(state);
        }

        recycle_scan_node(&queue, entry);
    }
    verbose_printf(cfg, 1, "遍历完成，等待数据落盘...\n");

    // ===【修复点2：关闭并刷盘】===
    async_worker_shutdown();

    shared.running = 0;
    if (thread_rc == 0) pthread_join(status_thread, NULL);

    cleanup_smart_queue(&queue);
    cleanup_cache(state);
    
    if (cfg->continue_mode) {
        cleanup_progress(cfg, state);
        release_lock(state);
    }
}


// =======================================================
// 修改后的 main 函数
// =======================================================
int main(int argc, char *argv[]) {
    Config cfg;
    memset(&cfg, 0, sizeof(Config));
    init_config(&cfg);
    
    // 1. 解析参数
    if (parse_arguments(argc, argv, &cfg) != 0) {
        return 1; // 参数解析失败或遇到 --help, 退出
    }

    // 2. 初始化
    setup_signal_handlers();
    
    if (cfg.continue_mode) {
        verbose_printf(&cfg, 1, "断点续传模式：初始化历史路径指纹集...\n");
        g_history_object_set = hash_set_create(HASH_SET_INITIAL_SIZE);
    }

    // 3. 核心业务逻辑
    RuntimeState state;
    memset(&state, 0, sizeof(RuntimeState));
    // ===【新增：必须初始化锁！】===
    pthread_mutex_init(&state.dev_cache_mutex, NULL);
    // ===========================
    if (acquire_lock(&cfg, &state) == -1) {
        pthread_mutex_destroy(&state.dev_cache_mutex); // 退出前清理
        fprintf(stderr, "无法获取锁,退出\n");
        return 1;
    }

    save_config_to_disk(&cfg);
    init_output_files(&cfg, &state);
    // ===【核心修复点：在这里启动异步写入线程！】===
    // 必须在开始遍历之前启动，否则数据全堆在内存里发不出去
    // ===========================================

    setvbuf(stdout, NULL, _IONBF, 0); // (这行之前建议删掉，如果没删也没事，因为现在主要靠 async_worker 写文件)
    
    // 开始干活
    traverse_files(&cfg, &state);

    // ===【核心修复点：在这里关闭并等待刷盘！】===
    // 遍历结束后，必须通知工人下班，并把最后残留在内存里的数据刷入磁盘
    // ===========================================

    // 4. 清理工作
    if (cfg.format) {
        cleanup_compiled_format(&cfg);
    }
    if (state.output_fp && state.output_fp != stdout) {
        close_output_file(state.output_fp);
    }
    if (state.dir_info_fp && state.dir_info_fp != stderr) {
        close_output_file(state.dir_info_fp);
    }
    release_lock(&state);

    if (g_history_object_set) {
        verbose_printf(&cfg, 1, "清理历史对象标识符集...\n");
        hash_set_destroy(g_history_object_set);
        g_history_object_set = NULL;
    }
    // ===【新增：程序结束前销毁锁】===
    pthread_mutex_destroy(&state.dev_cache_mutex);
    // =============================    
    // === 新增：关闭状态文件 ===
    if (state.status_file_fp && state.status_file_fp != stdout) {
        fclose(state.status_file_fp);
    }
    // ========================
    printf("\033[11;0H");
    return 0;
}

int compare_strings(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}
