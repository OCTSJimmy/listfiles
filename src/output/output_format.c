/**
 * @file output_format.c
 * @brief 格式预编译与输出文件管理
 *
 * 负责格式模板预编译（解析为 FormatSegment 数组）
 * 以及输出文件的创建、打开、关闭和切片轮转管理。
 */
#include "output.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include "log.h"

void cleanup_compiled_format(Config *cfg) {
    if (!cfg->compiled_format) return;
    
    for (int i = 0; i < cfg->format_segment_count; i++) {
        if (cfg->compiled_format[i].type == FMT_TEXT) {
            free(cfg->compiled_format[i].text);
        }
    }
    free(cfg->compiled_format);
    cfg->compiled_format = NULL;
    cfg->format_segment_count = 0;
}

/**
 * @brief  预编译输出格式模板
 * @param  cfg  Config*  指向配置结构体的指针，不能为空
 * @return void
 *
 * @note   根据 cfg->csv、cfg->format 以及元数据开关（--size、--user 等）
 *         生成 compiled_format 数组：
 *         - CSV 模式默认格式："%%i,%%p,%%s,%%u,%%g,%%U,%%G,%%o,%%O,%%t,%%m,%%c"
 *         - 无格式串时根据元数据开关动态构建默认文本格式
 *         - 无任何开关时默认输出：path|size|mtime
 *         预编译后 print_to_stream 可直接遍历数组输出，无需运行时解析格式字符串。
 */
void precompile_format(Config *cfg) {
    const char *fmt = cfg->format;
    
    if (cfg->csv && !fmt) {
        // [默认 CSV 格式] Inode,Path,Size,User,Group,UID,GID,ModeStr,OctMode,Type,Mtime,Ctime
        fmt = "%i,%p,%s,%u,%g,%U,%G,%o,%O,%t,%m,%c";
    } else if (!fmt) {
        // [默认文本格式] 根据元数据开关动态构建
        char default_fmt[256];
        int pos = 0;
        pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "%%p");
        if (cfg->size)   pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%s");
        if (cfg->user)   pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%u");
        if (cfg->group)  pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%g");
        if (cfg->mtime)  pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%m");
        if (cfg->atime)  pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%a");
        if (cfg->ctime)  pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%c");
        if (cfg->mode)   pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%o");
        if (cfg->inode)  pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%i");
        if (cfg->xattr)  pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "|%%X");
        // 如果没有启用任何元数据开关，默认输出 path|size|mtime
        if (!cfg->size && !cfg->user && !cfg->group && !cfg->mtime && !cfg->atime && !cfg->ctime && !cfg->mode && !cfg->inode && !cfg->xattr) {
            pos = 0;
            pos += snprintf(default_fmt + pos, sizeof(default_fmt) - pos, "%%p|%%s|%%m");
        }
        fmt = default_fmt;
    }

    int count = 0;
    int capacity = 32;
    cfg->compiled_format = safe_malloc(sizeof(FormatSegment) * capacity);
    
    const char *p = fmt;
    while (*p) {
        if (*p == '%') {
            p++;
            if (*p == '\0') break;

            FormatSegment seg;
            seg.text = NULL; 

            switch (*p) {
                case 'p': seg.type = FMT_PATH; break;
                case 's': seg.type = FMT_SIZE; break;
                case 'u': seg.type = FMT_USER; break;
                case 'g': seg.type = FMT_GROUP; break;
                case 'U': seg.type = FMT_UID; break; 
                case 'G': seg.type = FMT_GID; break;
                case 'm': seg.type = FMT_MTIME; break;
                case 'a': seg.type = FMT_ATIME; break;
                case 'c': seg.type = FMT_CTIME; break;
                case 't': seg.type = FMT_TYPE; break;
                case 'i': seg.type = FMT_INODE; break;
                case 'o': seg.type = FMT_MODE; break;    
                case 'O': seg.type = FMT_ST_MODE; break; 
                case 'X': seg.type = FMT_XATTR; break;
                default: 
                    seg.type = FMT_TEXT; 
                    char *tmp = safe_malloc(3);
                    tmp[0] = '%'; tmp[1] = *p; tmp[2] = '\0';
                    seg.text = tmp;
                    break;
            }
            p++;
            
            if (count >= capacity) {
                capacity *= 2;
                cfg->compiled_format = realloc(cfg->compiled_format, sizeof(FormatSegment) * capacity);
            }
            cfg->compiled_format[count++] = seg;
        } else {
            const char *start = p;
            while (*p && *p != '%') p++;
            int len = p - start;
            char *text = safe_malloc(len + 1);
            strncpy(text, start, len);
            text[len] = '\0';
            
            if (count >= capacity) {
                capacity *= 2;
                cfg->compiled_format = realloc(cfg->compiled_format, sizeof(FormatSegment) * capacity);
            }
            cfg->compiled_format[count].type = FMT_TEXT;
            cfg->compiled_format[count++].text = text;
        }
    }
    cfg->format_segment_count = count;
}

/**
 * @brief  创建输出文件
 * @param  path  const char*  输出文件路径，不能为空
 * @return FILE*  成功返回打开的文件指针；失败返回 NULL
 */
FILE* create_output_file(const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        log_error("创建输出文件%s失败", path);
        return NULL;
    }
    return fp;
}

FILE* open_output_file_append(const char *path) {
    FILE *fp = fopen(path, "a");
    if (!fp) {
        log_error("打开输出文件%s失败", path);
        return NULL;
    }
    return fp;
}

/**
 * @brief  关闭输出文件
 * @param  fp  FILE*  要关闭的文件指针，允许传入 NULL、stdout 或 stderr（空操作）
 * @return void
 */
void close_output_file(FILE *fp) {
    if (!fp || fp == stdout || fp == stderr) return;
    fclose(fp);
}

/**
 * @brief  初始化所有输出文件和流
 * @param  cfg    const Config*  全局配置指针，不能为空
 * @param  state  RuntimeState*  运行时状态指针，不能为空
 * @return void
 *
 * @note   根据配置选择三种输出模式之一：
 *         - 分片目录模式（-O）：创建目录并按 PROGRESS_SLICE_FORMAT 命名切片文件
 *         - 单文件模式（-o）：直接打开指定文件
 *         - 标准输出模式（默认）：output_fp = stdout
 *         同时处理 --print-dir 的目录信息输出流：
 *         若数据走文件，目录流走伴生文件；若数据走 stdout，目录流走 stderr。
 *         所有文件流均启用大块全缓冲（8MB/1MB）。
 */
void init_output_files(const Config *cfg, RuntimeState *state) {
    // 1. 初始化计数器和状态
    if (!cfg->continue_mode) {
        state->output_line_count = 0;
    }
    if (state->output_slice_num == 0) {
        state->output_slice_num = 1;  /* 仅当未从索引恢复时才重置 */
    }
    state->start_time = time(NULL);
    state->completed_count = 0;
    state->current_path = NULL;
    state->lock_file_path = NULL;
    
    // 2. 处理主数据输出 (Data Output)
    if (cfg->is_output_split_dir && cfg->output_split_dir) {
        // 模式 A: 分片目录
        if(mkdir(cfg->output_split_dir, 0700) == -1 && errno != EEXIST) {
            perror("无法创建输出目录"); exit(EXIT_FAILURE);
        }
        char slice_path[1024];
        snprintf(slice_path, sizeof(slice_path), "%s/" OUTPUT_SLICE_FORMAT, cfg->output_split_dir, state->output_slice_num);
        if (cfg->continue_mode && access(slice_path, F_OK) == 0) {
            state->output_fp = open_output_file_append(slice_path);
            verbose_printf(cfg, 1, "恢复输出切片文件: %s\n", slice_path);
        } else {
            state->output_fp = create_output_file(slice_path);
            verbose_printf(cfg, 1, "打开新切片文件: %s\n", slice_path);
        }
        if (!state->output_fp) state->output_fp = stdout;
    } else if (cfg->is_output_file && cfg->output_file) {
        // 模式 B: 单文件
        if (cfg->continue_mode && access(cfg->output_file, F_OK) == 0) {
            state->output_fp = open_output_file_append(cfg->output_file);
            verbose_printf(cfg, 1, "恢复输出文件: %s\n", cfg->output_file);
        } else {
            state->output_fp = create_output_file(cfg->output_file);
            verbose_printf(cfg, 1, "打开输出文件: %s\n", cfg->output_file);
        }
        if (!state->output_fp) state->output_fp = stdout;
    } else {
        // 模式 C: 标准输出
        state->output_fp = stdout;
    }
    if (!state->output_fp) { perror("无法打开输出文件"); exit(EXIT_FAILURE); }

    // 启用大块缓冲以减少系统调用
    if (state->output_fp && state->output_fp != stdout) {
        setvbuf(state->output_fp, NULL, _IOFBF, 8 * 1024 * 1024);
    }

    // 3. [修复重点] 处理目录流输出 (--print-dir)
    // 逻辑：如果数据走文件，目录流也走文件(伴生文件)；如果数据走 stdout，目录流走 stderr。
    if (cfg->print_dir) {
        if (cfg->is_output_split_dir) {
            // 场景 6-Split: 写入 output_dir/scan_dirs.log
            char dir_log_path[1024];
            snprintf(dir_log_path, sizeof(dir_log_path), "%s/scan_dirs.log", cfg->output_split_dir);
            state->dir_info_fp = fopen(dir_log_path, "a"); // 追加模式
            if (!state->dir_info_fp) {
                log_warn("无法创建目录日志文件 %s，回退到 stderr", dir_log_path);
                state->dir_info_fp = stderr;
            }
        } else if (cfg->is_output_file) {
            // 场景 6-File: 写入 output_file.dir (例如 data.csv.dir)
            char dir_log_path[1024];
            snprintf(dir_log_path, sizeof(dir_log_path), "%s.dir", cfg->output_file);
            state->dir_info_fp = fopen(dir_log_path, "a"); // 追加模式
            if (!state->dir_info_fp) {
                log_warn("无法创建目录日志文件 %s，回退到 stderr", dir_log_path);
                state->dir_info_fp = stderr;
            }
        } else {
            // 场景 2: 数据走 stdout，目录流必须走 stderr，否则数据会乱
            state->dir_info_fp = stderr;
        }
    } else {
        state->dir_info_fp = NULL;
    }

    if (state->dir_info_fp && state->dir_info_fp != stderr && state->dir_info_fp != stdout) {
        setvbuf(state->dir_info_fp, NULL, _IOFBF, 1 * 1024 * 1024);
    }
}

/**
 * @brief  执行输出切片轮转（关闭当前切片并创建新文件）
 * @param  cfg    const Config*  全局配置指针，不能为空
 * @param  state  RuntimeState*  运行时状态指针，不能为空
 * @return void
 *
 * @note   仅在 is_output_split_dir 模式下生效。
 *         递增 output_slice_num，按 OUTPUT_SLICE_FORMAT 生成新文件名。
 *         若创建新文件失败则调用 perror 并 exit(EXIT_FAILURE)。
 */
void rotate_output_slice(const Config *cfg, RuntimeState *state) {
    if (!cfg->is_output_split_dir) return;
    verbose_printf(cfg, 1,"切换输出切片文件\n");
    // 关闭当前切片
    if (state->output_fp) {
        // 解锁文件
        close_output_file(state->output_fp);
        state->output_fp = NULL;
        verbose_printf(cfg, 1,"关闭当前输出切片文件\n");
    }

    // 递增切片编号并创建新文件
    state->output_slice_num++;
    verbose_printf(cfg, 1,"递增切片编号: %lu\n", state->output_slice_num);
    char slice_path[1024];
    snprintf(slice_path, sizeof(slice_path), "%s/" OUTPUT_SLICE_FORMAT,
            cfg->output_split_dir, state->output_slice_num);
    state->output_fp = create_output_file(slice_path);
    if (!state->output_fp) {
        perror("无法创建新的输出切片文件");
        exit(EXIT_FAILURE);
    } else {
        verbose_printf(cfg, 1,"打开新切片文件: %s\n", slice_path);
    }
    state->output_line_count = 0;
}
