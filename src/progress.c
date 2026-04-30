#include "progress.h"
#include "utils.h"
#include "archive_format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <zlib.h>
#include <stdint.h>
#include <dirent.h>
#include <stdatomic.h>

/* ================================================================
 * Filename helpers
 * ================================================================ */
char *get_index_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s.idx", base);
    return name;
}

char *get_slice_filename(const char *base, unsigned long index) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s_%06lu.pbin", base, index);
    return name;
}

char *get_archive_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s.archive", base);
    return name;
}

char *get_spbin_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s.spbin", base);
    return name;
}

static unsigned char mode_to_dtype(mode_t mode) {
    if (S_ISREG(mode)) return DT_REG;
    if (S_ISDIR(mode)) return DT_DIR;
    if (S_ISLNK(mode)) return DT_LNK;
    if (S_ISCHR(mode)) return DT_CHR;
    if (S_ISBLK(mode)) return DT_BLK;
    if (S_ISFIFO(mode)) return DT_FIFO;
    if (S_ISSOCK(mode)) return DT_SOCK;
    return DT_UNKNOWN;
}

/* ================================================================
 * pbin / spbin 写入
 * ================================================================ */

static void write_pbin_record(FILE *fp, const char *path, const struct stat *info) {
    size_t path_len = strlen(path);
    dev_t dev = info ? info->st_dev : 0;
    ino_t ino = info ? info->st_ino : 0;
    time_t mtime = info ? info->st_mtime : 0;
    unsigned char d_type = info ? mode_to_dtype(info->st_mode) : DT_UNKNOWN;

    fwrite(&path_len, sizeof(size_t), 1, fp);
    fwrite(path, 1, path_len, fp);
    fwrite(&dev, sizeof(dev_t), 1, fp);
    fwrite(&ino, sizeof(ino_t), 1, fp);
    fwrite(&mtime, sizeof(time_t), 1, fp);
    fwrite(&d_type, sizeof(unsigned char), 1, fp);
}

void record_path(const Config *cfg, RuntimeState *state, const char *path, const struct stat *info) {
    if (!state->write_slice_file) {
        char *p = get_slice_filename(cfg->progress_base, state->write_slice_index);
        state->write_slice_file = fopen(p, "wb");
        free(p);
    }
    if (!state->write_slice_file) return;
    write_pbin_record(state->write_slice_file, path, info);
    state->line_count++;
    state->processed_count++;
    if (state->line_count >= cfg->progress_slice_lines) {
        /* rotate slice */
        fclose(state->write_slice_file);
        state->write_slice_file = NULL;
        process_old_slice(cfg, state->write_slice_index);
        state->write_slice_index++;
        state->line_count = 0;
        char *p = get_slice_filename(cfg->progress_base, state->write_slice_index);
        state->write_slice_file = fopen(p, "wb");
        free(p);
        atomic_update_index(cfg, state);
    }
}

void record_skip(const Config *cfg, RuntimeState *state, const SpbinEntry *entry) {
    FILE *fp = fopen(get_spbin_filename(cfg->progress_base), "ab");
    if (!fp) return;

    SpbinRecordHeader hdr = {
        .path_len = (uint32_t)strlen(entry->path),
        .dev = entry->dev,
        .blacklist_time = entry->blacklist_time,
        .retry_count = entry->retry_count,
        .probe_interval = entry->probe_interval,
        .d_type = entry->d_type,
        .s_status = entry->s_status
    };
    fwrite(&hdr, sizeof(hdr), 1, fp);
    fwrite(entry->path, 1, hdr.path_len, fp);
    fclose(fp);
}

/* ================================================================
 * 索引与游标
 * ================================================================ */

void atomic_update_index(const Config *cfg, RuntimeState *state) {
    char *idx_file = get_index_filename(cfg->progress_base);
    char *tmp_file = safe_malloc(strlen(idx_file) + 64);
    snprintf(tmp_file, strlen(idx_file) + 64, "%s.tmp.%lu", idx_file, (unsigned long)pthread_self());

    FILE *tmp_fp = fopen(tmp_file, "w");
    if (tmp_fp) {
        fprintf(tmp_fp, "%lu %lu %lu %lu %lu\n",
                state->write_slice_index,
                state->processed_count,
                state->write_slice_index,
                state->output_slice_num,
                state->output_line_count);
        fclose(tmp_fp);
        if (rename(tmp_file, idx_file) != 0) unlink(tmp_file);
    }
    free(idx_file);
    free(tmp_file);
}

bool load_progress_index(const Config *cfg, RuntimeState *state) {
    char *idx_file = get_index_filename(cfg->progress_base);
    FILE *fp = fopen(idx_file, "r");
    if (!fp) { free(idx_file); return false; }
    int matches = fscanf(fp, "%lu %lu %lu %lu %lu",
            &state->process_slice_index, &state->processed_count,
            &state->write_slice_index, &state->output_slice_num, &state->output_line_count);
    fclose(fp); free(idx_file);
    return matches == 5;
}

/* ================================================================
 * 归档 (archive with block_type)
 * ================================================================ */

static void archive_slice_to_file(const Config *cfg, const char *slice_path, uint8_t block_type) {
    FILE *in = fopen(slice_path, "rb");
    if (!in) return;

    fseek(in, 0, SEEK_END);
    long src_size = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (src_size <= 0) { fclose(in); unlink(slice_path); return; }

    unsigned char *src_buf = safe_malloc(src_size);
    if (fread(src_buf, 1, src_size, in) != (size_t)src_size) {
        free(src_buf); fclose(in); return;
    }
    fclose(in);

    unsigned long dest_len = compressBound(src_size);
    unsigned char *dest_buf = safe_malloc(dest_len);
    if (compress(dest_buf, &dest_len, src_buf, src_size) != Z_OK) {
        fprintf(stderr, "错误: 压缩分片失败\n");
        free(src_buf); free(dest_buf); return;
    }
    free(src_buf);

    char *archive_path = get_archive_filename(cfg->progress_base);
    FILE *out = fopen(archive_path, "ab");
    if (out) {
        ArchiveBlockHeader bh = {
            .uncompressed_size = (uint32_t)src_size,
            .compressed_size = (uint32_t)dest_len,
            .block_type = block_type
        };
        fwrite(&bh, sizeof(bh), 1, out);
        fwrite(dest_buf, 1, dest_len, out);
        fclose(out);
        unlink(slice_path);
    } else {
        perror("无法打开归档文件");
    }
    free(dest_buf);
    free(archive_path);
}

void process_old_slice(const Config *cfg, unsigned long index) {
    char *src_path = get_slice_filename(cfg->progress_base, index);
    archive_slice_to_file(cfg, src_path, ARCHIVE_BLOCK_NORMAL);
    free(src_path);
}

void finalize_archive(const Config *cfg, RuntimeState *state) {
    if (state->write_slice_file) {
        fclose(state->write_slice_file);
        state->write_slice_file = NULL;
        char *src_path = get_slice_filename(cfg->progress_base, state->write_slice_index);
        archive_slice_to_file(cfg, src_path, ARCHIVE_BLOCK_NORMAL);
        free(src_path);
    }
    /* Archive spbin as the last block */
    char *spbin_path = get_spbin_filename(cfg->progress_base);
    if (access(spbin_path, F_OK) == 0) {
        archive_slice_to_file(cfg, spbin_path, ARCHIVE_BLOCK_SPBIN);
    }
    free(spbin_path);
}

/* ================================================================
 * 进度恢复 (从 archive 和散落 pbin)
 * ================================================================ */

static void parse_pbin_buffer(const uint8_t *buf, size_t size,
                              FingerprintSet *visited_set,
                              FingerprintSet *ref_set,
                              ReferenceMap *ref_map) {
    size_t pos = 0;
    while (pos < size) {
        if (pos + sizeof(size_t) > size) break;
        size_t path_len;
        memcpy(&path_len, buf + pos, sizeof(size_t));
        pos += sizeof(size_t);

        size_t entry_size = path_len + sizeof(dev_t) + sizeof(ino_t) + sizeof(time_t) + sizeof(unsigned char);
        if (pos + entry_size > size) break;

        char *path_str = safe_malloc(path_len + 1);
        memcpy(path_str, buf + pos, path_len);
        path_str[path_len] = '\0';
        pos += path_len;

        dev_t dev; ino_t ino; time_t mtime; unsigned char d_type;
        memcpy(&dev, buf + pos, sizeof(dev_t)); pos += sizeof(dev_t);
        memcpy(&ino, buf + pos, sizeof(ino_t)); pos += sizeof(ino_t);
        memcpy(&mtime, buf + pos, sizeof(time_t)); pos += sizeof(time_t);
        memcpy(&d_type, buf + pos, sizeof(unsigned char)); pos += sizeof(unsigned char);

        uint8_t fp[FP_SIZE];
        fp_compute(path_str, dev, ino, fp);
        free(path_str);

        if (visited_set) fp_set_insert(visited_set, fp);
        if (ref_set) fp_set_insert(ref_set, fp);
        if (ref_map) ref_map_insert(ref_map, fp, mtime, d_type);
    }
}

static void parse_spbin_buffer(const uint8_t *buf, size_t size,
                               AppContext *ctx) {
    size_t pos = 0;
    while (pos < size) {
        if (pos + sizeof(SpbinRecordHeader) > size) break;
        SpbinRecordHeader hdr;
        memcpy(&hdr, buf + pos, sizeof(hdr));
        pos += sizeof(hdr);

        if (pos + hdr.path_len > size) break;
        char *path = safe_malloc(hdr.path_len + 1);
        memcpy(path, buf + pos, hdr.path_len);
        path[hdr.path_len] = '\0';
        pos += hdr.path_len;

        SpbinEntry entry = {
            .path = path,
            .dev = hdr.dev,
            .blacklist_time = hdr.blacklist_time,
            .retry_count = hdr.retry_count,
            .probe_interval = hdr.probe_interval,
            .d_type = hdr.d_type,
            .s_status = hdr.s_status
        };
        spbin_append(ctx, &entry);

        if (ctx->dev_mgr) {
            if (hdr.s_status == SP_STATUS_CONDEMNED) {
                dev_mgr_mark_condemned(ctx->dev_mgr, hdr.dev);
            } else {
                dev_mgr_mark_dead(ctx->dev_mgr, hdr.dev);
                ProbeTask task = {
                    .dev = hdr.dev,
                    .next_probe_time = time(NULL) + hdr.probe_interval,
                    .probe_interval = hdr.probe_interval,
                    .retry_count = hdr.retry_count,
                    .s_status = hdr.s_status
                };
                safe_strcpy(task.probe_path, path, sizeof(task.probe_path));
                probe_scheduler_push(ctx->probe_scheduler, &task);
            }
        }
    }
}

static void iterate_archive(const Config *cfg, AppContext *ctx,
                            FingerprintSet *visited_set,
                            FingerprintSet *ref_set,
                            ReferenceMap *ref_map) {
    char *archive_path = get_archive_filename(cfg->progress_base);
    FILE *fp = fopen(archive_path, "rb");
    if (!fp) { free(archive_path); return; }

    ArchiveBlockHeader bh;
    while (fread(&bh, sizeof(bh), 1, fp) == 1) {
        unsigned char *cmp_buf = safe_malloc(bh.compressed_size);
        if (fread(cmp_buf, 1, bh.compressed_size, fp) != bh.compressed_size) {
            free(cmp_buf); break;
        }

        unsigned char *raw_buf = safe_malloc(bh.uncompressed_size);
        unsigned long dest_len = bh.uncompressed_size;
        if (uncompress(raw_buf, &dest_len, cmp_buf, bh.compressed_size) == Z_OK) {
            if (bh.block_type == ARCHIVE_BLOCK_SPBIN) {
                parse_spbin_buffer(raw_buf, dest_len, ctx);
            } else {
                parse_pbin_buffer(raw_buf, dest_len, visited_set, ref_set, ref_map);
            }
        }
        free(raw_buf);
        free(cmp_buf);
    }
    fclose(fp);
    free(archive_path);
}

static void iterate_pbin_slices(const Config *cfg, RuntimeState *state,
                                FingerprintSet *visited_set,
                                FingerprintSet *ref_set,
                                ReferenceMap *ref_map) {
    int consecutive_missing = 0;
    for (unsigned long s_idx = 0; ; ++s_idx) {
        char *slice_path = get_slice_filename(cfg->progress_base, s_idx);
        FILE *slice_fp = fopen(slice_path, "rb");
        if (!slice_fp) {
            free(slice_path);
            consecutive_missing++;
            if (consecutive_missing > 50 && s_idx > state->write_slice_index) break;
            continue;
        }
        consecutive_missing = 0;

        fseek(slice_fp, 0, SEEK_END);
        long fsize = ftell(slice_fp);
        fseek(slice_fp, 0, SEEK_SET);
        if (fsize > 0) {
            unsigned char *buf = safe_malloc(fsize);
            fread(buf, 1, fsize, slice_fp);
            parse_pbin_buffer(buf, fsize, visited_set, ref_set, ref_map);
            free(buf);
        }
        fclose(slice_fp);
        free(slice_path);
    }
}

/* Resume mode: load from archive + pbin slices, replay unfinished tasks */
int restore_progress(const Config *cfg, AppContext *ctx) {
    if (!load_progress_index(cfg, &ctx->state)) {
        verbose_printf(cfg, 1, "无索引文件，从零开始恢复\n");
        return 0;
    }

    verbose_printf(cfg, 1, "开始断点恢复 (slice=%lu, line=%lu)...\n",
                   ctx->state.write_slice_index, ctx->state.line_count);

    /* Archive: all completed, load fingerprints only */
    iterate_archive(cfg, ctx, ctx->visited_set, NULL, NULL);

    /* Scattered pbin slices */
    int consecutive_missing = 0;
    for (unsigned long s_idx = 0; ; ++s_idx) {
        char *slice_path = get_slice_filename(cfg->progress_base, s_idx);
        FILE *slice_fp = fopen(slice_path, "rb");
        if (!slice_fp) {
            free(slice_path);
            consecutive_missing++;
            if (consecutive_missing > 50 && s_idx > ctx->state.write_slice_index) break;
            continue;
        }
        consecutive_missing = 0;

        fseek(slice_fp, 0, SEEK_END);
        long fsize = ftell(slice_fp);
        fseek(slice_fp, 0, SEEK_SET);

        if (fsize > 0) {
            unsigned char *buf = safe_malloc(fsize);
            fread(buf, 1, fsize, slice_fp);

            if (s_idx < ctx->state.write_slice_index) {
                /* Entire slice completed */
                parse_pbin_buffer(buf, fsize, ctx->visited_set, NULL, NULL);
            } else if (s_idx == ctx->state.write_slice_index) {
                /* Current slice: skip processed lines, replay the rest */
                size_t pos = 0;
                unsigned long line_no = 0;
                while (pos < (size_t)fsize) {
                    if (pos + sizeof(size_t) > (size_t)fsize) break;
                    size_t path_len;
                    memcpy(&path_len, buf + pos, sizeof(size_t));
                    pos += sizeof(size_t);

                    size_t entry_size = path_len + sizeof(dev_t) + sizeof(ino_t) +
                                        sizeof(time_t) + sizeof(unsigned char);
                    if (pos + entry_size > (size_t)fsize) break;

                    char *path_str = safe_malloc(path_len + 1);
                    memcpy(path_str, buf + pos, path_len);
                    path_str[path_len] = '\0';
                    pos += path_len;

                    dev_t dev; ino_t ino; time_t mtime; unsigned char d_type;
                    memcpy(&dev, buf + pos, sizeof(dev_t)); pos += sizeof(dev_t);
                    memcpy(&ino, buf + pos, sizeof(ino_t)); pos += sizeof(ino_t);
                    memcpy(&mtime, buf + pos, sizeof(time_t)); pos += sizeof(time_t);
                    memcpy(&d_type, buf + pos, sizeof(unsigned char)); pos += sizeof(unsigned char);

                    uint8_t fp[FP_SIZE];
                    fp_compute(path_str, dev, ino, fp);

                    if (line_no < ctx->state.line_count) {
                        /* Already processed */
                        fp_set_insert(ctx->visited_set, fp);
                    } else {
                        /* Not processed: if directory, re-dispatch to workers */
                        if (d_type == DT_DIR) {
                            atomic_fetch_add(&ctx->pending_tasks, 1);
                            uint32_t plen = (uint32_t)strlen(path_str);
                            int wid = (int)(line_no % ctx->worker_pool->num_workers);
                            ipc_send(ctx->worker_pool->slots[wid].fd_in, IPC_MSG_SCAN, path_str, plen);
                        }
                        /* Files are ignored: they will be re-discovered when parent dir is scanned */
                    }

                    free(path_str);
                    line_no++;
                }
            } else {
                parse_pbin_buffer(buf, fsize, ctx->visited_set, NULL, NULL);
            }
            free(buf);
        }
        fclose(slice_fp);
        free(slice_path);
    }

    verbose_printf(cfg, 1, "进度加载完成\n");
    return 0;
}

/* Incremental mode: load reference set/map from archive */
void restore_progress_to_memory(const Config *cfg, AppContext *ctx) {
    verbose_printf(cfg, 1, "开始加载半增量索引...\n");
    iterate_archive(cfg, ctx, NULL, ctx->reference_set, ctx->reference_map);
    iterate_pbin_slices(cfg, &ctx->state, NULL, ctx->reference_set, ctx->reference_map);
    verbose_printf(cfg, 1, "历史索引加载完成\n");
}

/* ================================================================
 * 其他辅助
 * ================================================================ */

void save_config_to_disk(const Config* cfg) {
    if (!cfg->progress_base) return;
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
    FILE *fp = fopen(config_path, "w");
    if (!fp) return;
    fprintf(fp, "path=%s\n", cfg->target_path);
    if (cfg->output_file) fprintf(fp, "output=%s\n", cfg->output_file);
    if (cfg->output_split_dir) fprintf(fp, "output_split=%s\n", cfg->output_split_dir);
    fprintf(fp, "start_time=%ld\n", time(NULL));
    fprintf(fp, "archive=%d\n", cfg->archive);
    fprintf(fp, "clean=%d\n", cfg->clean);
    fprintf(fp, "csv=%d\n", cfg->csv);
    fprintf(fp, "status=Running\n");
    fclose(fp);
}

void finalize_progress(const Config *cfg, RuntimeState *state) {
    finalize_archive(cfg, state);
    if (cfg->progress_base) {
        char config_path[1024];
        snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
        FILE *fp = fopen(config_path, "a");
        if (fp) {
            if (state->has_error) {
                fprintf(fp, "status=Incomplete\n");
                fprintf(fp, "error=DeviceMeltdown\n");
            } else {
                fprintf(fp, "status=Success\n");
            }
            fprintf(fp, "end_time=%ld\n", time(NULL));
            fclose(fp);
        }
    }
}

void cleanup_progress(const Config *cfg, RuntimeState *state) {
    char *idx_path = get_index_filename(cfg->progress_base);
    unlink(idx_path);
    free(idx_path);

    if (cfg->clean || cfg->archive) {
        for (unsigned long i = 0; i <= state->write_slice_index + 200; i++) {
            char *slice_path = get_slice_filename(cfg->progress_base, i);
            unlink(slice_path);
            free(slice_path);
        }
    }

    char *arch_path = get_archive_filename(cfg->progress_base);
    if (cfg->clean) unlink(arch_path);
    free(arch_path);

    char *spbin_path = get_spbin_filename(cfg->progress_base);
    unlink(spbin_path);
    free(spbin_path);

    char error_log[1024];
    snprintf(error_log, sizeof(error_log), "%s.error.log", cfg->progress_base);
    unlink(error_log);

    if (cfg->clean) {
        char config_path[1024];
        snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
        unlink(config_path);
    }
}

int acquire_lock(const Config *cfg, RuntimeState *state) {
    if (!cfg->continue_mode) return 0;
    char *lock_path = safe_malloc(strlen(cfg->progress_base) + 32);
    sprintf(lock_path, "%s.lock", cfg->progress_base);
    state->lock_file_path = lock_path;
    int fd = open(lock_path, O_RDWR | O_CREAT, 0666);
    if (fd == -1) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) == -1) { close(fd); return -1; }
    state->lock_fd = fd;
    return 0;
}

void release_lock(RuntimeState *state) {
    if (state->lock_fd != -1) { flock(state->lock_fd, LOCK_UN); close(state->lock_fd); state->lock_fd = -1; }
    if (state->lock_file_path) { unlink(state->lock_file_path); free(state->lock_file_path); state->lock_file_path = NULL; }
}

/* ================================================================
 * Spbin memory cache
 * ================================================================ */

void spbin_append(AppContext *ctx, const SpbinEntry *entry) {
    if (ctx->spbin_count >= ctx->spbin_capacity) {
        size_t new_cap = ctx->spbin_capacity ? ctx->spbin_capacity * 2 : 64;
        SpbinEntry *new_arr = realloc(ctx->spbin_entries, new_cap * sizeof(SpbinEntry));
        if (!new_arr) return;
        ctx->spbin_entries = new_arr;
        ctx->spbin_capacity = new_cap;
    }
    ctx->spbin_entries[ctx->spbin_count] = *entry;
    ctx->spbin_count++;
}

void spbin_requeue_recovered(AppContext *ctx, dev_t dev) {
    for (size_t i = 0; i < ctx->spbin_count; i++) {
        if (ctx->spbin_entries[i].dev == dev && ctx->spbin_entries[i].s_status != SP_STATUS_CONDEMNED) {
            atomic_fetch_add(&ctx->pending_tasks, 1);
            uint32_t plen = (uint32_t)strlen(ctx->spbin_entries[i].path);
            static int next_worker = 0;
            int wid = next_worker % ctx->worker_pool->num_workers;
            next_worker++;
            ipc_send(ctx->worker_pool->slots[wid].fd_in, IPC_MSG_SCAN,
                     ctx->spbin_entries[i].path, plen);
        }
    }
}
