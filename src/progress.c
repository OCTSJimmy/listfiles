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

char *get_per_slice_index_filename(const char *base, unsigned long index) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s_%06lu.idx", base, index);
    return name;
}

char *get_fpbin_slice_filename(const char *base, unsigned long index) {
    char *name = safe_malloc(strlen(base) + 48);
    sprintf(name, "%s.fpbin_%06lu", base, index);
    return name;
}

char *get_fpbin_index_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s.fpbin.idx", base);
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
 * Footer 读写与校验
 * ================================================================ */

bool write_pbin_footer(FILE *fp, uint64_t row_count) {
    if (!fp) return false;
    fflush(fp);
    int fd = fileno(fp);
    if (fd < 0) return false;

    PbinFooter f = {
        .magic = PBIN_FOOTER_MAGIC,
        .row_count = row_count,
        .data_crc32 = 0
    };
    /* footer_crc32 覆盖 magic + row_count（前 16 字节） */
    f.footer_crc32 = (uint32_t)crc32(0, (const Bytef *)&f.magic, (uInt)(sizeof(f.magic) + sizeof(f.row_count)));

    ssize_t w = write(fd, &f, sizeof(f));
    if (w != (ssize_t)sizeof(f)) return false;
    if (fsync(fd) != 0) return false;
    return true;
}

bool read_pbin_footer(const char *path, PbinFooter *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    off_t end = lseek(fd, 0, SEEK_END);
    if (end < (off_t)sizeof(PbinFooter)) {
        close(fd);
        return false;
    }

    if (lseek(fd, end - sizeof(PbinFooter), SEEK_SET) < 0) {
        close(fd);
        return false;
    }

    ssize_t n = read(fd, out, sizeof(PbinFooter));
    close(fd);
    if (n != (ssize_t)sizeof(PbinFooter)) return false;

    return verify_pbin_footer(out);
}

bool verify_pbin_footer(const PbinFooter *f) {
    if (f->magic != PBIN_FOOTER_MAGIC) return false;
    uint32_t expected = (uint32_t)crc32(0, (const Bytef *)&f->magic, sizeof(f->magic) + sizeof(f->row_count));
    return f->footer_crc32 == expected;
}

unsigned long get_slice_row_count(const Config *cfg, unsigned long index) {
    char *slice_path = get_slice_filename(cfg->progress_base, index);
    PbinFooter f;
    if (read_pbin_footer(slice_path, &f)) {
        free(slice_path);
        return (unsigned long)f.row_count;
    }
    free(slice_path);

    /* Fallback: 读取按分片 idx */
    char *idx_path = get_per_slice_index_filename(cfg->progress_base, index);
    FILE *fp = fopen(idx_path, "r");
    unsigned long row_count = 0;
    if (fp) {
        if (fscanf(fp, "%lu", &row_count) != 1) row_count = 0;
        fclose(fp);
    }
    free(idx_path);
    return row_count;
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
    if (cfg->clean) return;  /* --clean 模式不保留任何进度文件 */
    if (!state->write_slice_file) {
        char *p = get_slice_filename(cfg->progress_base, state->write_slice_index);
        state->write_slice_file = fopen(p, "wb");
        free(p);
        if (state->write_slice_file) {
            /* 创建活跃分片的草稿 idx */
            char *idx = get_per_slice_index_filename(cfg->progress_base, state->write_slice_index);
            FILE *ifp = fopen(idx, "w");
            if (ifp) { fprintf(ifp, "0\n"); fclose(ifp); }
            free(idx);
        }
    }
    if (!state->write_slice_file) return;
    write_pbin_record(state->write_slice_file, path, info);
    state->line_count++;
    state->processed_count++;
    if (state->line_count >= cfg->progress_slice_lines) {
        /* rotate slice: 先盖钢印(Footer)，再烧草稿(idx) */
        write_pbin_footer(state->write_slice_file, state->line_count);
        fclose(state->write_slice_file);
        state->write_slice_file = NULL;

        /* 删除按分片草稿 idx */
        char *old_idx = get_per_slice_index_filename(cfg->progress_base, state->write_slice_index);
        unlink(old_idx);
        free(old_idx);

        process_old_slice(cfg, state->write_slice_index);
        state->write_slice_index++;
        state->line_count = 0;
        char *p = get_slice_filename(cfg->progress_base, state->write_slice_index);
        state->write_slice_file = fopen(p, "wb");
        free(p);
        if (state->write_slice_file) {
            char *idx = get_per_slice_index_filename(cfg->progress_base, state->write_slice_index);
            FILE *ifp = fopen(idx, "w");
            if (ifp) { fprintf(ifp, "0\n"); fclose(ifp); }
            free(idx);
        }
        atomic_update_index(cfg, state);
    }
}

/* ================================================================
 * record_path 批量缓冲
 * ================================================================ */

void record_path_batch_init(RecordBatch *batch) {
    if (!batch) return;
    memset(batch, 0, sizeof(*batch));
}

static void record_path_batch_flush_internal(const Config *cfg, RuntimeState *state, RecordBatch *batch) {
    if (!batch || batch->count == 0) return;
    for (int i = 0; i < batch->count; i++) {
        record_path(cfg, state, batch->paths[i], &batch->stats[i]);
        free(batch->paths[i]);
        batch->paths[i] = NULL;
    }
    batch->count = 0;
    batch->total_bytes = 0;
}

void record_path_batch_flush(const Config *cfg, RuntimeState *state, RecordBatch *batch) {
    record_path_batch_flush_internal(cfg, state, batch);
}

bool record_path_batch_append(const Config *cfg, RuntimeState *state, RecordBatch *batch, const char *path, const struct stat *info) {
    if (!batch || !path) return false;
    
    size_t path_len = strlen(path);
    size_t entry_size = path_len + sizeof(dev_t) + sizeof(ino_t) + sizeof(time_t) + sizeof(unsigned char);
    
    /* 检查是否需要先 flush */
    if (batch->count >= RECORD_BATCH_COUNT ||
        (batch->count > 0 && batch->total_bytes + entry_size >= RECORD_BATCH_BYTES)) {
        record_path_batch_flush_internal(cfg, state, batch);
    }
    
    if (batch->count >= RECORD_BATCH_COUNT) {
        /* flush 后仍然满（理论上不应发生，因为 RECORD_BATCH_COUNT 是硬上限） */
        record_path_batch_flush_internal(cfg, state, batch);
    }
    
    batch->paths[batch->count] = strdup(path);
    if (info) {
        batch->stats[batch->count] = *info;
    } else {
        memset(&batch->stats[batch->count], 0, sizeof(struct stat));
    }
    batch->total_bytes += entry_size;
    batch->count++;
    return true;
}

void record_skip(const Config *cfg, RuntimeState *state, const SpbinEntry *entry) {
    (void)state;
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
                state->line_count,
                state->processed_count,
                state->output_slice_num,
                state->output_line_count);
        fclose(tmp_fp);
        if (rename(tmp_file, idx_file) != 0) unlink(tmp_file);
    }
    free(idx_file);
    free(tmp_file);

    /* 同步更新当前活跃分片的草稿 idx */
    char *per_idx = get_per_slice_index_filename(cfg->progress_base, state->write_slice_index);
    char *per_tmp = safe_malloc(strlen(per_idx) + 64);
    snprintf(per_tmp, strlen(per_idx) + 64, "%s.tmp.%lu", per_idx, (unsigned long)pthread_self());
    FILE *ptf = fopen(per_tmp, "w");
    if (ptf) {
        fprintf(ptf, "%lu\n", state->line_count);
        fclose(ptf);
        if (rename(per_tmp, per_idx) != 0) unlink(per_tmp);
    }
    free(per_idx);
    free(per_tmp);
}

bool load_progress_index(const Config *cfg, RuntimeState *state) {
    char *idx_file = get_index_filename(cfg->progress_base);
    FILE *fp = fopen(idx_file, "r");
    if (!fp) { free(idx_file); return false; }
    int matches = fscanf(fp, "%lu %lu %lu %lu %lu",
            &state->write_slice_index, &state->line_count,
            &state->processed_count, &state->output_slice_num, &state->output_line_count);
    fclose(fp); free(idx_file);
    state->process_slice_index = state->write_slice_index;
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

    long data_size = src_size;
    uint64_t row_count = 0;
    if (block_type != ARCHIVE_BLOCK_SPBIN && src_size >= (long)sizeof(PbinFooter)) {
        PbinFooter *f = (PbinFooter *)(src_buf + src_size - sizeof(PbinFooter));
        if (verify_pbin_footer(f)) {
            data_size = src_size - (long)sizeof(PbinFooter);
            row_count = f->row_count;
        }
    }

    unsigned long dest_len = compressBound((uLong)data_size);
    unsigned char *dest_buf = safe_malloc(dest_len);
    if (compress(dest_buf, &dest_len, src_buf, (uLong)data_size) != Z_OK) {
        fprintf(stderr, "错误: 压缩分片失败\n");
        free(src_buf); free(dest_buf); return;
    }
    free(src_buf);

    char *archive_path = get_archive_filename(cfg->progress_base);
    FILE *out = fopen(archive_path, "ab");
    if (out) {
        ArchiveBlockHeader bh = {
            .uncompressed_size = (uint32_t)data_size,
            .compressed_size = (uint32_t)dest_len,
            .block_type = block_type,
            .row_count = row_count
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
    if (cfg->archive) {
        archive_slice_to_file(cfg, src_path, ARCHIVE_BLOCK_NORMAL);
    } else {
        unlink(src_path);
    }
    free(src_path);
}

void finalize_archive(const Config *cfg, RuntimeState *state) {
    if (state->write_slice_file) {
        /* 正常结束时给活跃分片盖钢印 */
        write_pbin_footer(state->write_slice_file, state->line_count);
        fclose(state->write_slice_file);
        state->write_slice_file = NULL;
        /* 删除按分片草稿 idx */
        char *per_idx = get_per_slice_index_filename(cfg->progress_base, state->write_slice_index);
        unlink(per_idx);
        free(per_idx);

        char *src_path = get_slice_filename(cfg->progress_base, state->write_slice_index);
        if (cfg->archive) {
            archive_slice_to_file(cfg, src_path, ARCHIVE_BLOCK_NORMAL);
        }
        /* If not archiving, keep the current slice file for resume */
        free(src_path);
    }
    /* Archive spbin as the last block */
    char *spbin_path = get_spbin_filename(cfg->progress_base);
    if (access(spbin_path, F_OK) == 0) {
        if (cfg->archive) {
            archive_slice_to_file(cfg, spbin_path, ARCHIVE_BLOCK_SPBIN);
        }
        /* If not archiving, keep spbin for resume */
    }
    free(spbin_path);
}

/* ================================================================
 * 进度恢复 (从 archive 和散落 pbin)
 * ================================================================ */

static void parse_pbin_buffer(const uint8_t *buf, size_t size, uint64_t max_rows,
                              FingerprintSet *visited_set,
                              FingerprintSet *ref_set,
                              ReferenceMap *ref_map) {
    size_t pos = 0;
    uint64_t rows = 0;
    while (pos < size) {
        if (max_rows > 0 && rows >= max_rows) break;
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
        rows++;
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
        /* Sanity check for corrupted or old-format archive */
        if (bh.block_type != ARCHIVE_BLOCK_NORMAL && bh.block_type != ARCHIVE_BLOCK_SPBIN) break;
        if (bh.compressed_size > 512 * 1024 * 1024 || bh.uncompressed_size > 512 * 1024 * 1024) break;

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
                /* 归档块内是纯数据区，无 Footer */
                parse_pbin_buffer(raw_buf, dest_len, 0, visited_set, ref_set, ref_map);
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
            long data_size = fsize;
            if (fsize >= (long)sizeof(PbinFooter)) {
                PbinFooter *f = (PbinFooter *)(buf + fsize - sizeof(PbinFooter));
                if (verify_pbin_footer(f)) {
                    data_size = fsize - (long)sizeof(PbinFooter);
                }
            }
            parse_pbin_buffer(buf, data_size, 0, visited_set, ref_set, ref_map);
            free(buf);
        }
        fclose(slice_fp);
        free(slice_path);
    }
}

/* Count normal (pbin-equivalent) compressed blocks inside the archive file.
 * SPBIN blocks are skipped records, not progress slices, so they are not counted. */
static unsigned long count_archive_blocks(const Config *cfg) {
    char *archive_path = get_archive_filename(cfg->progress_base);
    FILE *fp = fopen(archive_path, "rb");
    if (!fp) { free(archive_path); return 0; }

    unsigned long count = 0;
    ArchiveBlockHeader bh;
    while (fread(&bh, sizeof(bh), 1, fp) == 1) {
        if (bh.block_type != ARCHIVE_BLOCK_NORMAL && bh.block_type != ARCHIVE_BLOCK_SPBIN) break;
        if (bh.compressed_size > 512 * 1024 * 1024) break;
        if (bh.block_type == ARCHIVE_BLOCK_NORMAL)
            count++;
        if (fseek(fp, bh.compressed_size, SEEK_CUR) != 0) break;
    }
    fclose(fp);
    free(archive_path);
    return count;
}

/* Count scattered pbin slices on disk */
static unsigned long count_pbin_slices(const Config *cfg) {
    unsigned long count = 0;
    int consecutive_missing = 0;
    for (unsigned long s_idx = 0; consecutive_missing <= 50; ++s_idx) {
        char *slice_path = get_slice_filename(cfg->progress_base, s_idx);
        if (access(slice_path, F_OK) == 0) {
            count++;
            consecutive_missing = 0;
        } else {
            consecutive_missing++;
        }
        free(slice_path);
    }
    return count;
}

/* ================================================================
 * fpbin Cache (temporary buffer for new sub-dirs during pbin replay)
 * Uses flat array in memory + optional disk overflow file.
 * ================================================================ */

#define FPBIN_MEM_WATERMARK 10000

/* 更新 fpbin.idx：记录当前分片号与行数 */
static void atomic_update_fpbin_index(AppContext *ctx) {
    char *idx_file = get_fpbin_index_filename(ctx->cfg.progress_base);
    char *tmp_file = safe_malloc(strlen(idx_file) + 64);
    snprintf(tmp_file, strlen(idx_file) + 64, "%s.tmp.%lu", idx_file, (unsigned long)pthread_self());

    FILE *tmp_fp = fopen(tmp_file, "w");
    if (tmp_fp) {
        fprintf(tmp_fp, "%lu %lu\n", ctx->fpbin_write_slice_index, ctx->fpbin_line_count);
        fclose(tmp_fp);
        if (rename(tmp_file, idx_file) != 0) unlink(tmp_file);
    }
    free(idx_file);
    free(tmp_file);
}

/* 打开或创建新的 fpbin 活跃分片 */
static void fpbin_open_slice(AppContext *ctx) {
    if (ctx->fpbin_slice_file) {
        fclose(ctx->fpbin_slice_file);
        ctx->fpbin_slice_file = NULL;
    }
    char *p = get_fpbin_slice_filename(ctx->cfg.progress_base, ctx->fpbin_write_slice_index);
    ctx->fpbin_slice_file = fopen(p, "wb");
    free(p);
    ctx->fpbin_line_count = 0;
}

/* rotate fpbin 分片（已满，封口写 Footer） */
static void fpbin_rotate_slice(AppContext *ctx) {
    if (ctx->fpbin_slice_file) {
        write_pbin_footer(ctx->fpbin_slice_file, ctx->fpbin_line_count);
        fclose(ctx->fpbin_slice_file);
        ctx->fpbin_slice_file = NULL;
    }
    ctx->fpbin_write_slice_index++;
    fpbin_open_slice(ctx);
    atomic_update_fpbin_index(ctx);
}

void fpbin_append(AppContext *ctx, const char *path, const struct stat *st) {
    if (!ctx->fpbin_slice_file) {
        fpbin_open_slice(ctx);
    }

    if (ctx->fpbin_count < FPBIN_MEM_WATERMARK) {
        /* Grow array if needed */
        if (ctx->fpbin_count >= ctx->fpbin_capacity) {
            size_t new_cap = ctx->fpbin_capacity ? ctx->fpbin_capacity * 2 : 1024;
            ctx->fpbin_entries = realloc(ctx->fpbin_entries, new_cap * sizeof(char *));
            ctx->fpbin_stats   = realloc(ctx->fpbin_stats,   new_cap * sizeof(struct stat));
            ctx->fpbin_capacity = new_cap;
        }
        ctx->fpbin_entries[ctx->fpbin_count] = strdup(path);
        ctx->fpbin_stats[ctx->fpbin_count]   = *st;
        ctx->fpbin_count++;
    } else {
        /* Flush memory array to current fpbin slice */
        if (ctx->fpbin_slice_file) {
            setvbuf(ctx->fpbin_slice_file, NULL, _IOFBF, 8 * 1024 * 1024);
            for (size_t i = 0; i < ctx->fpbin_count; i++) {
                write_pbin_record(ctx->fpbin_slice_file, ctx->fpbin_entries[i], &ctx->fpbin_stats[i]);
            }
            ctx->fpbin_line_count += ctx->fpbin_count;
        }
        /* Clear memory array */
        for (size_t i = 0; i < ctx->fpbin_count; i++) {
            free(ctx->fpbin_entries[i]);
        }
        ctx->fpbin_count = 0;
        /* Append current record */
        if (ctx->fpbin_slice_file) {
            write_pbin_record(ctx->fpbin_slice_file, path, st);
            ctx->fpbin_line_count++;
        }
        /* Rotate if needed */
        if (ctx->fpbin_line_count >= ctx->cfg.progress_slice_lines) {
            fpbin_rotate_slice(ctx);
        }
    }
}

static void fpbin_clear_mem(AppContext *ctx) {
    for (size_t i = 0; i < ctx->fpbin_count; i++) {
        free(ctx->fpbin_entries[i]);
    }
    ctx->fpbin_count = 0;
}

/* 查找当前最大的 pbin 分片索引 */
static unsigned long find_max_pbin_index(const Config *cfg) {
    unsigned long max_idx = 0;
    int consecutive_missing = 0;
    for (unsigned long i = 0; consecutive_missing <= 50; ++i) {
        char *slice_path = get_slice_filename(cfg->progress_base, i);
        if (access(slice_path, F_OK) == 0) {
            max_idx = i;
            consecutive_missing = 0;
        } else {
            consecutive_missing++;
        }
        free(slice_path);
    }
    return max_idx;
}

/* Promote fpbin to pbin: seal all fpbin slices, rename, verify, cleanup */
static void promote_fpbin_to_pbin(AppContext *ctx) {
    /* 1. Flush any remaining memory entries to current fpbin slice */
    if (ctx->fpbin_slice_file && ctx->fpbin_count > 0) {
        for (size_t i = 0; i < ctx->fpbin_count; i++) {
            write_pbin_record(ctx->fpbin_slice_file, ctx->fpbin_entries[i], &ctx->fpbin_stats[i]);
        }
        ctx->fpbin_line_count += ctx->fpbin_count;
        fpbin_clear_mem(ctx);
    }

    /* 2. Seal the last active fpbin slice with Footer */
    if (ctx->fpbin_slice_file) {
        write_pbin_footer(ctx->fpbin_slice_file, ctx->fpbin_line_count);
        fclose(ctx->fpbin_slice_file);
        ctx->fpbin_slice_file = NULL;
    }

    /* If no fpbin files were ever created but we have memory-only data,
       create a single fpbin slice file first so rename works uniformly. */
    if (ctx->fpbin_write_slice_index == 0 && ctx->fpbin_line_count == 0 && ctx->fpbin_count > 0) {
        /* This branch handles the case where fpbin never hit watermark
           and no slice file was opened. Create slice 0 now. */
        fpbin_open_slice(ctx);
        for (size_t i = 0; i < ctx->fpbin_count; i++) {
            write_pbin_record(ctx->fpbin_slice_file, ctx->fpbin_entries[i], &ctx->fpbin_stats[i]);
        }
        ctx->fpbin_line_count = ctx->fpbin_count;
        fpbin_clear_mem(ctx);
        write_pbin_footer(ctx->fpbin_slice_file, ctx->fpbin_line_count);
        fclose(ctx->fpbin_slice_file);
        ctx->fpbin_slice_file = NULL;
    }

    /* 3. Rename all fpbin slices to pbin */
    unsigned long pbin_start_idx = find_max_pbin_index(&ctx->cfg) + 1;
    unsigned long fpbin_total = ctx->fpbin_write_slice_index + 1;
    for (unsigned long i = 0; i < fpbin_total; i++) {
        char *src = get_fpbin_slice_filename(ctx->cfg.progress_base, i);
        char *dst = get_slice_filename(ctx->cfg.progress_base, pbin_start_idx + i);
        if (rename(src, dst) != 0) {
            fprintf(stderr, "[错误] fpbin 转正 rename 失败: %s -> %s\n", src, dst);
        }
        free(src);
        free(dst);
    }

    /* 4. Verify all promoted pbin slices */
    bool all_ok = true;
    for (unsigned long i = 0; i < fpbin_total; i++) {
        char *path = get_slice_filename(ctx->cfg.progress_base, pbin_start_idx + i);
        PbinFooter f;
        if (!read_pbin_footer(path, &f)) {
            fprintf(stderr, "[错误] 转正后 pbin Footer 校验失败: %s\n", path);
            all_ok = false;
        }
        free(path);
    }
    if (!all_ok) {
        fprintf(stderr, "[致命错误] fpbin 转正校验未通过，建议手动清理后重试\n");
        /* 不删除 fpbin.idx，以便下次恢复时重试转正 */
        ctx->hist_pump_state = HIST_PUMP_DONE;
        return;
    }

    /* 5. Cleanup fpbin artifacts */
    char *fpbin_idx = get_fpbin_index_filename(ctx->cfg.progress_base);
    unlink(fpbin_idx);
    free(fpbin_idx);
    for (unsigned long i = 0; i < fpbin_total + 10; i++) {
        char *src = get_fpbin_slice_filename(ctx->cfg.progress_base, i);
        unlink(src);
        free(src);
    }

    /* 6. Update pump source to first promoted pbin */
    if (ctx->hist_pump_fp) fclose(ctx->hist_pump_fp);
    char *first_pbin = get_slice_filename(ctx->cfg.progress_base, pbin_start_idx);
    ctx->hist_pump_fp = fopen(first_pbin, "rb");
    free(first_pbin);
    ctx->hist_pump_slice_idx = pbin_start_idx;
    ctx->hist_pump_line_no = 0;

    /* 7. Reset pbin write state to after promoted slices */
    ctx->state.write_slice_index = pbin_start_idx + fpbin_total;
    ctx->state.line_count = 0;
    if (ctx->state.write_slice_file) {
        fclose(ctx->state.write_slice_file);
        ctx->state.write_slice_file = NULL;
    }

    ctx->hist_pump_state = HIST_PUMP_NEW;
}

/* Called when current pbin slice is fully consumed */
static void on_pbin_slice_consumed(AppContext *ctx) {
    if (ctx->hist_pump_fp) {
        fclose(ctx->hist_pump_fp);
        ctx->hist_pump_fp = NULL;
    }

    /* Try to open next scattered slice */
    ctx->hist_pump_slice_idx++;
    char *next_path = get_slice_filename(ctx->cfg.progress_base, ctx->hist_pump_slice_idx);
    ctx->hist_pump_fp = fopen(next_path, "rb");
    free(next_path);

    if (ctx->hist_pump_fp) {
        ctx->hist_pump_line_no = 0;
        return;  /* Continue pumping next slice */
    }

    /* No more scattered slices. Check fpbin. */
    if (ctx->fpbin_count > 0 || ctx->fpbin_slice_file) {
        promote_fpbin_to_pbin(ctx);
        return;
    }

    /* Nothing left to pump */
    ctx->hist_pump_state = HIST_PUMP_DONE;
}

/* Read next record from current pbin file, return true on success */
static bool read_next_pbin_record(FILE *fp, char **out_path, struct stat *out_st, unsigned char *out_d_type) {
    size_t path_len;
    if (fread(&path_len, sizeof(size_t), 1, fp) != 1) return false;

    /* 防御性校验：防止读取到 Footer magic 或损坏数据 */
    if (path_len > MAX_PATH_LENGTH) {
        return false;
    }

    char *path = malloc(path_len + 1);
    if (!path) return false;
    if (fread(path, 1, path_len, fp) != path_len) { free(path); return false; }
    path[path_len] = '\0';

    dev_t dev; ino_t ino; time_t mtime; unsigned char d_type;
    if (fread(&dev, sizeof(dev_t), 1, fp) != 1) { free(path); return false; }
    if (fread(&ino, sizeof(ino_t), 1, fp) != 1) { free(path); return false; }
    if (fread(&mtime, sizeof(time_t), 1, fp) != 1) { free(path); return false; }
    if (fread(&d_type, sizeof(unsigned char), 1, fp) != 1) { free(path); return false; }

    memset(out_st, 0, sizeof(*out_st));
    out_st->st_dev = dev;
    out_st->st_ino = ino;
    out_st->st_mtime = mtime;
    out_st->st_mode = (d_type == DT_DIR) ? S_IFDIR :
                      (d_type == DT_REG) ? S_IFREG :
                      (d_type == DT_LNK) ? S_IFLNK :
                      (d_type == DT_CHR) ? S_IFCHR :
                      (d_type == DT_BLK) ? S_IFBLK :
                      (d_type == DT_FIFO) ? S_IFIFO :
                      (d_type == DT_SOCK) ? S_IFSOCK : 0;
    *out_path = path;
    *out_d_type = d_type;
    return true;
}

/* Pump a batch of directories from current pbin to workers */
void pump_pbin_batch(AppContext *ctx, int batch_size) {
    if (!ctx->hist_pump_fp) return;

    int sent = 0;
    while (sent < batch_size) {
        char *path = NULL;
        struct stat st;
        unsigned char d_type;

        if (!read_next_pbin_record(ctx->hist_pump_fp, &path, &st, &d_type)) {
            on_pbin_slice_consumed(ctx);
            return;
        }

        ctx->hist_pump_line_no++;

        /* Load into visited_set to avoid duplicate output */
        uint8_t fp_all[FP_SIZE];
        fp_compute(path, st.st_dev, st.st_ino, fp_all);
        fp_set_insert(ctx->visited_set, fp_all);

        if (d_type == DT_DIR) {
            atomic_fetch_add(&ctx->pending_tasks, 1);
            uint32_t plen = (uint32_t)strlen(path);
            static int next_wid = 0;
            int wid = next_wid % ctx->worker_pool->num_workers;
            next_wid++;
            ipc_send(ctx->worker_pool->slots[wid].fd_in, IPC_MSG_SCAN, path, plen);
            sent++;
        }
        free(path);
    }
}

/* Resume mode: load from archive + pbin slices, set up pump state */
int restore_progress(const Config *cfg, AppContext *ctx) {
    /* 1. Reset fpbin state (keep residual files for potential re-promotion) */
    fpbin_clear_mem(ctx);
    if (ctx->fpbin_slice_file) {
        fclose(ctx->fpbin_slice_file);
        ctx->fpbin_slice_file = NULL;
    }
    ctx->fpbin_write_slice_index = 0;
    ctx->fpbin_line_count = 0;
    ctx->hist_pump_state = HIST_PUMP_DONE;
    ctx->hist_pump_fp = NULL;
    ctx->hist_pump_slice_idx = 0;
    ctx->hist_pump_line_no = 0;

    bool has_idx = load_progress_index(cfg, &ctx->state);
    unsigned long pbin_count  = count_pbin_slices(cfg);
    unsigned long archive_blk = count_archive_blocks(cfg);
    unsigned long total_blocks = pbin_count + archive_blk;

    /* 2. Load archive (completed slices) into visited_set */
    iterate_archive(cfg, ctx, ctx->visited_set, NULL, NULL);

    if (!has_idx) {
        if (total_blocks > 1) {
            verbose_printf(cfg, 1,
                "无索引文件且历史块数 %lu (archive=%lu, pbin=%lu) 超过一个，"
                "执行全量重扫...\n", total_blocks, archive_blk, pbin_count);
            ctx->state.write_slice_index = 0;
            ctx->state.line_count = 0;
            ctx->state.process_slice_index = 0;
            ctx->state.output_slice_num = 0;
            ctx->state.output_line_count = 0;
            /* Open first scattered slice for pumping */
            char *first_slice = get_slice_filename(cfg->progress_base, 0);
            ctx->hist_pump_fp = fopen(first_slice, "rb");
            free(first_slice);
            if (ctx->hist_pump_fp) {
                ctx->hist_pump_state = HIST_PUMP_OLD;
                ctx->hist_pump_slice_idx = 0;
                ctx->hist_pump_line_no = 0;
            }
            return 0;
        }
        verbose_printf(cfg, 1, "无索引文件，历史块数 %lu，尝试恢复...\n", total_blocks);
        ctx->state.write_slice_index = 0;
        ctx->state.line_count = 0;
        ctx->state.process_slice_index = 0;
        ctx->state.output_slice_num = 0;
        ctx->state.output_line_count = 0;
        /* Single block: load scattered slices and done */
        iterate_pbin_slices(cfg, &ctx->state, ctx->visited_set, NULL, NULL);
        return 0;
    }

    verbose_printf(cfg, 1, "开始断点恢复 (slice=%lu, line=%lu)...\n",
                   ctx->state.write_slice_index, ctx->state.line_count);

    /* 3. Load scattered pbin slices with Footer-first recovery */
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

            long data_size = fsize;
            uint64_t row_count = 0;
            bool footer_ok = false;
            if (fsize >= (long)sizeof(PbinFooter)) {
                PbinFooter *f = (PbinFooter *)(buf + fsize - sizeof(PbinFooter));
                if (verify_pbin_footer(f)) {
                    data_size = fsize - (long)sizeof(PbinFooter);
                    row_count = f->row_count;
                    footer_ok = true;
                    /* 钢印清晰则烧草稿: 删除残留按分片 idx */
                    char *per_idx = get_per_slice_index_filename(cfg->progress_base, s_idx);
                    unlink(per_idx);
                    free(per_idx);
                }
            }
            if (!footer_ok) {
                /* Fallback: 读取按分片 idx 或统一 idx */
                row_count = get_slice_row_count(cfg, s_idx);
            }

            if (s_idx < ctx->state.write_slice_index) {
                /* 已完成分片：解析 row_count 行 */
                parse_pbin_buffer(buf, data_size, row_count, ctx->visited_set, NULL, NULL);
            } else if (s_idx == ctx->state.write_slice_index) {
                /* 活跃分片：只解析已处理的 line_count 行 */
                parse_pbin_buffer(buf, data_size, ctx->state.line_count, ctx->visited_set, NULL, NULL);
            }
            free(buf);
        }
        fclose(slice_fp);
        free(slice_path);
    }

    /* 4. Handle residual fpbin (interrupted promotion) */
    char *fpbin_idx_path = get_fpbin_index_filename(cfg->progress_base);
    bool has_fpbin_idx = (access(fpbin_idx_path, F_OK) == 0);
    bool has_fpbin_slice = false;
    for (unsigned long i = 0; i < 1000; i++) {
        char *fp = get_fpbin_slice_filename(cfg->progress_base, i);
        if (access(fp, F_OK) == 0) { has_fpbin_slice = true; free(fp); break; }
        free(fp);
    }
    if (has_fpbin_idx && has_fpbin_slice) {
        verbose_printf(cfg, 1, "检测到残留 fpbin，执行转正恢复...\n");
        FILE *fidx = fopen(fpbin_idx_path, "r");
        if (fidx) {
            unsigned long fsi = 0, flc = 0;
            if (fscanf(fidx, "%lu %lu", &fsi, &flc) == 2) {
                ctx->fpbin_write_slice_index = fsi;
                ctx->fpbin_line_count = flc;
            }
            fclose(fidx);
        }
        promote_fpbin_to_pbin(ctx);
        free(fpbin_idx_path);
        verbose_printf(cfg, 1, "fpbin 转正恢复完成\n");
        return 0;
    }
    /* 清理不匹配的残留 fpbin 文件 */
    if (!has_fpbin_idx || !has_fpbin_slice) {
        for (unsigned long i = 0; i < 1000; i++) {
            char *fp = get_fpbin_slice_filename(cfg->progress_base, i);
            unlink(fp);
            free(fp);
        }
        unlink(fpbin_idx_path);
    }
    free(fpbin_idx_path);

    /* 5. Open current pbin slice for pumping (skip processed lines) */
    char *cur_slice = get_slice_filename(cfg->progress_base, ctx->state.write_slice_index);
    ctx->hist_pump_fp = fopen(cur_slice, "rb");
    free(cur_slice);
    if (ctx->hist_pump_fp) {
        /* Skip already-processed lines */
        for (unsigned long i = 0; i < ctx->state.line_count; i++) {
            char *path = NULL;
            struct stat st;
            unsigned char d_type;
            if (!read_next_pbin_record(ctx->hist_pump_fp, &path, &st, &d_type)) break;
            free(path);
        }

        /* 如果已到达文件末尾，说明该分片已完全处理，无需 pumping */
        int c = fgetc(ctx->hist_pump_fp);
        if (c == EOF) {
            fclose(ctx->hist_pump_fp);
            ctx->hist_pump_fp = NULL;
            ctx->hist_pump_state = HIST_PUMP_DONE;
        } else {
            ungetc(c, ctx->hist_pump_fp);
            ctx->hist_pump_state = HIST_PUMP_OLD;
            ctx->hist_pump_slice_idx = ctx->state.write_slice_index;
            ctx->hist_pump_line_no = ctx->state.line_count;
        }
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
    if (cfg->clean) return; /* --clean mode should not leave any progress files */
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
    if (!cfg->clean) {
        finalize_archive(cfg, state);
        /* Ensure index is written so resume can locate the cursor */
        atomic_update_index(cfg, state);
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
    } else {
        /* --clean mode: do not create any new progress files */
        if (state->write_slice_file) {
            fclose(state->write_slice_file);
            state->write_slice_file = NULL;
            char *src_path = get_slice_filename(cfg->progress_base, state->write_slice_index);
            unlink(src_path);
            free(src_path);
        }
    }
}

void cleanup_progress(const Config *cfg, RuntimeState *state) {
    char *idx_path = get_index_filename(cfg->progress_base);
    unlink(idx_path);
    free(idx_path);

    /* Always clean up slice files on --clean; on --archive they were already archived */
    if (cfg->clean || cfg->archive) {
        for (unsigned long i = 0; i <= state->write_slice_index + 200; i++) {
            char *slice_path = get_slice_filename(cfg->progress_base, i);
            unlink(slice_path);
            free(slice_path);
            /* 同时清理按分片草稿 idx */
            char *per_idx = get_per_slice_index_filename(cfg->progress_base, i);
            unlink(per_idx);
            free(per_idx);
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

    /* 清理残留 fpbin（基于 progress_base） */
    char *fpbin_idx = get_fpbin_index_filename(cfg->progress_base);
    unlink(fpbin_idx);
    free(fpbin_idx);
    for (unsigned long i = 0; i < 1000; i++) {
        char *fp = get_fpbin_slice_filename(cfg->progress_base, i);
        unlink(fp);
        free(fp);
    }
    /* 兼容旧版本残留 */
    unlink("progress.fpbin");
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
            int wid = ctx->next_requeue_worker % ctx->worker_pool->num_workers;
            ctx->next_requeue_worker++;
            ipc_send(ctx->worker_pool->slots[wid].fd_in, IPC_MSG_SCAN,
                     ctx->spbin_entries[i].path, plen);
        }
    }
}
