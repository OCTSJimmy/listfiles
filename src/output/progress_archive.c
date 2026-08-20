/**
 * @file progress_archive.c
 * @brief 归档、恢复与 fpbin 转正逻辑
 *
 * 核心设计哲学：
 * - 同构分片：pbin、fpbin、dpbin 采用完全相同的物理格式
 * - 页脚自描述：已封口分片末尾自带 Footer（magic + row_count + crc），无需外部索引
 * - 崩溃恢复：Footer 优先，dpbin 提供差分集合用于续传
 *
 * 进度文件格式（以 --progress-file=task1 为例）：
 * - task1_000000.pbin  已封口的已完成记录分片
 * - task1.dpbin_000000 本次会话的目录完成日志（临时，正常结束后删除）
 * - task1.dfpbin_000XXX HIST_PUMP_OLD 阶段的目录完成日志（v15.6.0，fpbin 原子对）
 * - task1.spbin        跳过记录（熔断设备上的目录）
 * - task1.fpbin_000XXX 恢复期间隔离新发现子目录的临时分片
 * - task1.archive      zlib 压缩的历史分片归档
 * - task1.config       会话配置快照（兼容保留，权威状态以 manifest 为准）
 * - task1.manifest     Run manifest（v15.6.0，P0-008）：运行完整性标记 + baseline_eligible
 */
#include "progress.h"
#include "utils.h"
#include "archive_format.h"
#include "msg_format.h"
#include "msg_queue.h"
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
#include <errno.h>
#include "main_loop.h"
#include "log.h"

/* ================================================================
 * 归档 (archive with block_type)
 * ================================================================ */

/**
 * @brief  将单个分片文件压缩并追加到归档文件
 * @param  cfg         const Config*  全局配置指针，不能为空
 * @param  slice_path  const char*    要归档的分片文件路径，不能为空
 * @param  block_type  uint8_t        块类型，取值范围: ARCHIVE_BLOCK_NORMAL(0) 或 ARCHIVE_BLOCK_SPBIN(1)
 * @return void
 *
 * @note   流程：
 *         1. 读取整个分片到内存
 *         2. 若分片末尾有有效 Footer，则提取 row_count 并从数据区剔除 Footer
 *         3. 使用 zlib compress 压缩数据区
 *         4. 追加 ArchiveBlockHeader + 压缩数据到 {base}.archive.new
 *            （v15.6.0，P0-008：运行期一律写在写副本，finalize 校验通过后才
 *            原子切换覆盖 {base}.archive）
 *         5. 删除原始分片文件（unlink）
 *         若压缩失败或文件打开失败，则保留原始分片不删除。
 */
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
        log_error("压缩分片失败");
        free(src_buf); free(dest_buf); return;
    }
    free(src_buf);

    char *archive_path = get_archive_new_filename(cfg->progress_base);
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

/**
 * @brief  处理已完成的旧分片（归档或删除）
 * @param  cfg    const Config*   全局配置指针，不能为空
 * @param  index  unsigned long   已完成的分片编号，取值范围: >= 0
 * @return void
 *
 * @note   若 cfg->archive 为 true，则调用 archive_slice_to_file 压缩归档；
 *         否则直接 unlink 删除分片文件。
 */
void process_old_slice(const Config *cfg, unsigned long index) {
    char *src_path = get_slice_filename(cfg->progress_base, index);
    if (cfg->archive) {
        archive_slice_to_file(cfg, src_path, ARCHIVE_BLOCK_NORMAL);
    } else {
        unlink(src_path);
    }
    free(src_path);
}

/**
 * @brief  校验（必要时截断）archive 在写副本的每个块（v15.6.0，P0-008）
 * @param  path          const char*  {base}.archive.new 路径，不能为空
 * @param  out_blocks    long*        输出参数，返回校验通过的块数
 * @return bool  返回 true 表示校验通过（含截断撕尾后剩余块全部有效）；
 *               false 表示文件无法打开或首块即损坏（数据不可信）
 *
 * @note   逐块读 ArchiveBlockHeader → sanity check → 读压缩数据 → uncompress 校验。
 *         崩溃撕尾（最后一个块不完整）时 ftruncate 到最后一个有效块末尾，
 *         已 unlink 的原始分片由 Reset 援救兜底重扫（at-least-once）。
 */
static bool archive_verify_or_truncate(const char *path, long *out_blocks) {
    *out_blocks = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    long valid_end = 0;
    long blocks = 0;
    for (;;) {
        ArchiveBlockHeader bh;
        if (fread(&bh, sizeof(bh), 1, fp) != 1) break; /* EOF 或撕尾 */
        if (bh.block_type != ARCHIVE_BLOCK_NORMAL && bh.block_type != ARCHIVE_BLOCK_SPBIN) break;
        if (bh.compressed_size == 0 || bh.compressed_size > 512 * 1024 * 1024
            || bh.uncompressed_size > 512 * 1024 * 1024) break;

        unsigned char *cmp_buf = safe_malloc(bh.compressed_size);
        if (fread(cmp_buf, 1, bh.compressed_size, fp) != bh.compressed_size) {
            free(cmp_buf);
            break; /* 撕尾 */
        }
        unsigned char *raw_buf = safe_malloc(bh.uncompressed_size ? bh.uncompressed_size : 1);
        unsigned long dest_len = bh.uncompressed_size;
        int zrc = uncompress(raw_buf, &dest_len, cmp_buf, bh.compressed_size);
        free(raw_buf);
        free(cmp_buf);
        if (zrc != Z_OK || dest_len != bh.uncompressed_size) break; /* 损坏 */

        valid_end = ftell(fp);
        blocks++;
    }
    fclose(fp);

    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > (off_t)valid_end) {
        /* 截断撕尾/损坏尾部到最后一个有效块 */
        int fd = open(path, O_WRONLY);
        if (fd >= 0) {
            if (ftruncate(fd, (off_t)valid_end) == 0) {
                log_warn("[archive] %s 尾部 %ld 字节损坏/撕尾，已截断（有效块 %ld）",
                         path, (long)(st.st_size - (off_t)valid_end), blocks);
            }
            close(fd);
        }
    }
    *out_blocks = blocks;
    return true;
}

/**
 * @brief  archive 原子切换：校验 {base}.archive.new → rename 覆盖 {base}.archive（v15.6.0，P0-008）
 * @param  base  const char*  进度文件前缀，不能为空
 * @return bool  返回 true 表示切换成功或无新数据可切换；false 表示校验/切换失败
 *
 * @note   流程：
 *         1. .new 不存在 → 本轮无新归档块，无需切换，返回 true；
 *         2. archive_verify_or_truncate 逐块校验（撕尾截断）；
 *         3. 首块即损坏（有效块为 0 且原文件非空）→ 回滚删除 .new，旧基准不动；
 *         4. 校验通过：rename {base}.archive → {base}.archive.prev（保留旧基准），
 *            再 rename .new → {base}.archive。
 *         崩溃安全性：两个 rename 之间崩溃会留下 .prev + .new（无 .archive），
 *         恢复路径（iterate_archive）在 .archive 缺失时回退读 .prev 并继续读 .new，
 *         不丢历史块。
 */
static bool archive_atomic_switch(const char *base) {
    char *new_path = get_archive_new_filename(base);
    char *cur_path = get_archive_filename(base);
    char *prev_path = get_archive_prev_filename(base);

    if (access(new_path, F_OK) != 0) {
        free(new_path); free(cur_path); free(prev_path);
        return true; /* 本轮无新归档块 */
    }

    struct stat nst;
    long new_blocks = 0;
    if (!archive_verify_or_truncate(new_path, &new_blocks)) {
        log_error("[archive] 在写副本校验失败: %s", new_path);
        goto fail;
    }
    if (new_blocks == 0) {
        bool had_data = (stat(new_path, &nst) == 0 && nst.st_size > 0);
        unlink(new_path);
        if (had_data) {
            /* 有数据但首块即损坏：回滚删除 .new，旧基准保留 */
            log_error("[archive] %s 无有效块，回滚删除，旧基准保留", new_path);
            goto fail;
        }
        free(new_path); free(cur_path); free(prev_path);
        return true; /* 空在写副本，无需切换 */
    }

    /* 保留旧基准为 .prev（不存在则忽略 ENOENT） */
    if (rename(cur_path, prev_path) != 0 && errno != ENOENT) {
        log_error("[archive] 保留旧基准 rename 失败: %s -> %s", cur_path, prev_path);
        goto fail;
    }
    if (rename(new_path, cur_path) != 0) {
        log_error("[archive] 原子切换 rename 失败: %s -> %s", new_path, cur_path);
        goto fail;
    }
    free(new_path); free(cur_path); free(prev_path);
    return true;

fail:
    free(new_path); free(cur_path); free(prev_path);
    return false;
}

/**
 * @brief  任务结束时归档所有残留分片并执行 archive 原子切换
 * @param  cfg    const Config*   全局配置指针，不能为空
 * @param  state  RuntimeState*   运行时状态指针，不能为空
 * @return bool  返回 archive 写出校验结果（未启用 archive 时恒 true），
 *               供 manifest_finalize 判定 baseline_eligible（v15.6.0，P0-008）
 *
 * @note   流程：
 *         1. 若存在活跃分片（write_slice_file），先封口写 Footer，再归档或保留
 *         2. 归档 spbin 文件（作为 archive 的最后一个块）
 *         3. v15.6.0（P0-008）：archive 模式下执行 .new → 校验 → rename 原子切换，
 *            旧基准保留为 {base}.archive.prev
 *         若未开启归档模式，保留当前活跃分片和 spbin 以供后续恢复。
 */
bool finalize_archive(const Config *cfg, RuntimeState *state) {
    if (state->write_slice_file) {
        /* 正常结束时给活跃分片盖钢印 */
        write_pbin_footer(state->write_slice_file, state->line_count);
        fclose(state->write_slice_file);
        state->write_slice_file = NULL;
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

    /* v15.6.0（P0-008）：原子切换在写副本，校验失败则回滚删 .new、保留旧基准 */
    if (cfg->archive) {
        return archive_atomic_switch(cfg->progress_base);
    }
    return true;
}

/* ================================================================
 * 进度恢复 (从 archive 和散落 pbin)
 * ================================================================ */

/**
 * @brief  解析 pbin/fpbin 数据缓冲区，提取指纹并插入集合（v15.6.0 schema 2）
 * @param  buf            const uint8_t*    数据缓冲区指针，不能为空
 * @param  size           size_t            缓冲区大小（字节）
 * @param  max_rows       uint64_t          最大解析行数，0 表示无限制
 * @param  discovered_set FingerprintSet*   本次任务的 discovered_set（仅目录去重），允许为 NULL
 * @param  ref_set        FingerprintSet*   盲信的 reference_set，允许为 NULL
 * @param  ref_map        ReferenceMap*     盲信的 reference_map，允许为 NULL
 * @return void
 *
 * @note   按 pbin schema 2 记录格式顺序解析（设计 §0.2）：
 *         path_len → path → d_type → mtime_sec → mtime_nsec → size →
 *         uid → gid → mode → dev → ino → flags。
 *         v15.6.0（P0-004）：discovered_set 仅收录 d_type==DT_DIR 的记录（目录任务
 *         去重集合不含文件，文件输出语义 at-least-once 允许重复行），指纹为
 *         fp_compute(path, dev, ino)（遍历身份）；
 *         v15.6.0（P0-011）：ref_set/ref_map（盲信基准）改用纯路径指纹
 *         fp_compute(path, 0, 0)——盲信不 lstat 拿不到 dev/ino；
 *         ref_map 收录完整历史 stat（mtime/mtime_nsec/size/uid/gid/mode/d_type），
 *         供命中后原样复用，输出不再退化零字段。
 *         当 max_rows > 0 且已解析行数达到 max_rows 时提前停止。
 */
static void parse_pbin_buffer(const uint8_t *buf, size_t size, uint64_t max_rows,
                              FingerprintSet *discovered_set,
                              FingerprintSet *ref_set,
                              ReferenceMap *ref_map) {
    /* schema 2 定长尾部长度（与 write_pbin_record 严格对应） */
    const size_t tail_size = sizeof(unsigned char) + sizeof(time_t) + sizeof(long)
                           + sizeof(off_t) + 3 * sizeof(uint32_t)
                           + 2 * sizeof(uint64_t) + sizeof(uint32_t);
    size_t pos = 0;
    uint64_t rows = 0;
    while (pos < size) {
        if (max_rows > 0 && rows >= max_rows) break;
        if (pos + sizeof(size_t) > size) break;
        size_t path_len;
        memcpy(&path_len, buf + pos, sizeof(size_t));
        pos += sizeof(size_t);

        /* 防御性校验：防止把 Footer magic/损坏数据当作 path_len */
        if (path_len == 0 || path_len >= MAX_PATH_LENGTH) break;
        if (pos + path_len + tail_size > size) break;

        char *path_str = safe_malloc(path_len + 1);
        memcpy(path_str, buf + pos, path_len);
        path_str[path_len] = '\0';
        pos += path_len;

        unsigned char d_type;
        time_t mtime_sec;
        long mtime_nsec;
        off_t fsize;
        uint32_t uid, gid, mode, flags;
        uint64_t dev, ino;
        memcpy(&d_type, buf + pos, sizeof(unsigned char)); pos += sizeof(unsigned char);
        memcpy(&mtime_sec, buf + pos, sizeof(time_t)); pos += sizeof(time_t);
        memcpy(&mtime_nsec, buf + pos, sizeof(long)); pos += sizeof(long);
        memcpy(&fsize, buf + pos, sizeof(off_t)); pos += sizeof(off_t);
        memcpy(&uid, buf + pos, sizeof(uint32_t)); pos += sizeof(uint32_t);
        memcpy(&gid, buf + pos, sizeof(uint32_t)); pos += sizeof(uint32_t);
        memcpy(&mode, buf + pos, sizeof(uint32_t)); pos += sizeof(uint32_t);
        memcpy(&dev, buf + pos, sizeof(uint64_t)); pos += sizeof(uint64_t);
        memcpy(&ino, buf + pos, sizeof(uint64_t)); pos += sizeof(uint64_t);
        memcpy(&flags, buf + pos, sizeof(uint32_t)); pos += sizeof(uint32_t);
        (void)flags; /* 预留字段，当前不解释 */

        /* v15.6.0: discovered_set 仅目录，遍历身份指纹（path+dev+ino） */
        if (discovered_set && d_type == DT_DIR) {
            uint8_t fp[FP_SIZE];
            fp_compute(path_str, dev, ino, fp);
            fp_set_insert(discovered_set, fp);
        }
        /* v15.6.0（P0-011）：盲信基准改用纯路径指纹，收录完整历史 stat */
        if (ref_set || ref_map) {
            uint8_t rfp[FP_SIZE];
            fp_compute(path_str, 0, 0, rfp);
            if (ref_set) fp_set_insert(ref_set, rfp);
            if (ref_map) {
                ref_map_insert(ref_map, rfp, mtime_sec, mtime_nsec, fsize,
                               uid, gid, mode, d_type);
            }
        }
        free(path_str);
        rows++;
    }
}

/**
 * @brief  将一条 spbin 记录合并入内存缓存与 spbin_set（v15.6.0，P0-005）
 * @param  ctx         AppContext*   应用上下文指针，不能为空
 * @param  path        const char*   跳过目录路径，不能为空
 * @param  reason      uint8_t       跳过原因码（SP_REASON_*）
 * @param  timestamp   time_t        记录时间
 * @param  device_key  const char*   设备身份字符串（当前为 st_dev 十进制，NUL 结尾）
 * @return void
 *
 * @note   spbin_set（path-only 指纹）兼作去重依据：同一路径的重复记录
 *         （archive 多个 SPBIN 块 + 独立 {base}.spbin）后读覆盖先读——
 *         append-only 时序上后写更新。retry_count 不持久化，恢复时从 0 开始
 *         （退避窗口回到 30min 档）。s_status 暂置 PROBING，由 restore_spbin
 *         分组时按 reason/退避窗口重新分类。
 */
static void spbin_merge_entry(AppContext *ctx, const char *path, uint8_t reason,
                              time_t timestamp, const char *device_key) {
    if (!ctx->spbin_set) {
        ctx->spbin_set = fp_set_create(4096);
    }
    bool exists = false;
    if (ctx->spbin_set) {
        uint8_t fp[FP_SIZE];
        fp_compute(path, 0, 0, fp);
        exists = fp_set_insert(ctx->spbin_set, fp);
    }
    if (exists) {
        for (size_t i = 0; i < ctx->spbin_count; i++) {
            if (strcmp(ctx->spbin_entries[i].path, path) == 0) {
                ctx->spbin_entries[i].reason = reason;
                ctx->spbin_entries[i].timestamp = timestamp;
                ctx->spbin_entries[i].dev = (uint64_t)strtoull(device_key, NULL, 10);
                return;
            }
        }
        /* spbin_set 有记录但内存条目缺失（理论上不发生）：按新条目追加 */
    }
    SpbinEntry entry = {0};
    entry.path = strdup(path);
    if (!entry.path) return;
    entry.dev = (uint64_t)strtoull(device_key, NULL, 10);
    entry.reason = reason;
    entry.timestamp = timestamp;
    entry.retry_count = 0;
    entry.s_status = SP_STATUS_PROBING;
    spbin_append(ctx, &entry);
}

/**
 * @brief  解析 spbin 数据缓冲区（archive SPBIN 块），合并到内存缓存
 * @param  buf  const uint8_t*  数据缓冲区指针，不能为空
 * @param  size size_t          缓冲区大小（字节）
 * @param  ctx  AppContext*     应用上下文指针，不能为空
 * @return void
 *
 * @note   v15.6.0（P0-005）新格式：[path_len:u32][path][reason:u8][timestamp:time_t]
 *         [device_key:64B]，流式解析，读坏即停。
 *         设备分组/探测动作不在此执行——统一由 restore_progress 的 restore_spbin
 *         步骤在所有来源（archive 块 + 独立文件）合并完成后一次性处理。
 */
static void parse_spbin_buffer(const uint8_t *buf, size_t size,
                               AppContext *ctx) {
    size_t pos = 0;
    while (pos < size) {
        uint32_t path_len;
        if (pos + sizeof(uint32_t) > size) break;
        memcpy(&path_len, buf + pos, sizeof(path_len));
        pos += sizeof(path_len);
        if (path_len == 0 || path_len >= MAX_PATH_LENGTH) break;
        if (pos + path_len > size) break;

        char *path = safe_malloc(path_len + 1);
        memcpy(path, buf + pos, path_len);
        path[path_len] = '\0';
        pos += path_len;

        uint8_t reason;
        time_t timestamp;
        char device_key[SPBIN_DEVICE_KEY_LEN];
        if (pos + sizeof(uint8_t) + sizeof(time_t) + SPBIN_DEVICE_KEY_LEN > size) {
            free(path);
            break;
        }
        memcpy(&reason, buf + pos, sizeof(uint8_t));
        pos += sizeof(uint8_t);
        memcpy(&timestamp, buf + pos, sizeof(time_t));
        pos += sizeof(time_t);
        memcpy(device_key, buf + pos, SPBIN_DEVICE_KEY_LEN);
        pos += SPBIN_DEVICE_KEY_LEN;
        device_key[SPBIN_DEVICE_KEY_LEN - 1] = '\0';

        spbin_merge_entry(ctx, path, reason, timestamp, device_key);
        free(path);
    }
}

/**
 * @brief  恢复模式的 spbin 步骤：读独立 {base}.spbin + 按 device_key 分组处理（v15.6.0，P0-005）
 * @param  cfg  const Config*  全局配置指针，不能为空
 * @param  ctx  AppContext*    应用上下文指针，不能为空
 * @return void
 *
 * @note   调用时机：dpbin 加载完成后、泵送开始前（restore_progress 步骤 5.5）。
 *         流程：
 *         1. 读独立 {base}.spbin（新格式流式读，读坏即停），逐条合并入内存
 *            （archive 的 SPBIN 块已在 iterate_archive 阶段经 parse_spbin_buffer 合并）；
 *         2. 按 reason 分类、按 device_key 分组：
 *            - PERMISSION/CIRCUIT_BREAKER/POISON → 永久跳过（CONDEMNED，本次运行
 *              不再入队，log_info 计数）；
 *            - PROBE_FAIL/TIMEOUT → 检查 timestamp + 退避窗口（按 retry_count
 *              指数：30min→2h→6h→24h 封顶）。未超窗 → 保持跳过（待下次会话）；
 *              超窗 → 对该设备 push 敢死队 probe_task（复用 probe_scheduler/
 *              monitor 现有回调链路），探测成功由 spbin_requeue_recovered 整组
 *              重入队并标记 RECOVERED；失败由 monitor 更新 timestamp/retry_count。
 *         spbin_set 已在合并阶段建立，泵送（pump_pbin_batch）与 dspill 回填据此
 *         拦截熔断/跳过目录，不盲目重入队。
 */
static void restore_spbin(const Config *cfg, AppContext *ctx) {
    /* 1. 读独立 {base}.spbin（非 archive 模式下正常退出/崩溃后残留） */
    char *spbin_path = get_spbin_filename(cfg->progress_base);
    FILE *fp = spbin_path ? fopen(spbin_path, "rb") : NULL;
    if (fp) {
        unsigned long loaded = 0;
        while (1) {
            uint32_t path_len;
            if (fread(&path_len, sizeof(uint32_t), 1, fp) != 1) break;
            if (path_len == 0 || path_len >= MAX_PATH_LENGTH) break;

            char *path = safe_malloc(path_len + 1);
            if (fread(path, 1, path_len, fp) != path_len) { free(path); break; }
            path[path_len] = '\0';

            uint8_t reason;
            time_t timestamp;
            char device_key[SPBIN_DEVICE_KEY_LEN];
            if (fread(&reason, sizeof(uint8_t), 1, fp) != 1
                || fread(&timestamp, sizeof(time_t), 1, fp) != 1
                || fread(device_key, 1, SPBIN_DEVICE_KEY_LEN, fp) != SPBIN_DEVICE_KEY_LEN) {
                free(path);
                break; /* 读坏即停 */
            }
            device_key[SPBIN_DEVICE_KEY_LEN - 1] = '\0';

            spbin_merge_entry(ctx, path, reason, timestamp, device_key);
            free(path);
            loaded++;
        }
        fclose(fp);
        if (loaded > 0) {
            log_info("[restore] spbin 加载 %lu 条跳过记录", loaded);
        }
    }
    free(spbin_path);

    if (ctx->spbin_count == 0) return;

    /* 2. 按 reason 分类 + 按 device_key 分组处理 */
    static const long backoff_stages[] = SPBIN_BACKOFF_STAGES;
    time_t now = time(NULL);
    unsigned long permanent = 0, waiting = 0, probing = 0;
    dev_t *probe_devs = NULL;
    size_t probe_dev_count = 0, probe_dev_cap = 0;

    for (size_t i = 0; i < ctx->spbin_count; i++) {
        SpbinEntry *e = &ctx->spbin_entries[i];

        /* 未知 reason：log_warn 并按 PROBE_FAIL 保守处理 */
        if (e->reason != SP_REASON_PROBE_FAIL && e->reason != SP_REASON_TIMEOUT
            && e->reason != SP_REASON_CIRCUIT_BREAKER && e->reason != SP_REASON_PERMISSION
            && e->reason != SP_REASON_POISON) {
            log_warn("[restore] spbin 未知 reason=%u（%s），按 PROBE_FAIL 保守处理",
                     e->reason, path_log_mask(e->path));
            e->reason = SP_REASON_PROBE_FAIL;
        }

        if (e->reason == SP_REASON_PERMISSION
            || e->reason == SP_REASON_CIRCUIT_BREAKER
            || e->reason == SP_REASON_POISON) {
            /* 永久跳过：本次运行不再入队（CONDEMNED），不进入探测队列 */
            e->s_status = SP_STATUS_CONDEMNED;
            permanent++;
            continue;
        }

        /* PROBE_FAIL/TIMEOUT：检查 timestamp + 退避窗口 */
        uint32_t stage = e->retry_count < SPBIN_BACKOFF_STAGES_COUNT
                         ? e->retry_count : SPBIN_BACKOFF_STAGES_COUNT - 1;
        long window = backoff_stages[stage];
        if (now - e->timestamp < window) {
            /* 未超窗：保持跳过（待下次会话再评估） */
            e->s_status = SP_STATUS_CONDEMNED;
            waiting++;
            continue;
        }

        /* 超窗：保持 PROBING，对该设备 push 敢死队探测（每设备仅一次） */
        e->s_status = SP_STATUS_PROBING;
        probing++;

        dev_t dev = (dev_t)e->dev;
        bool pushed = false;
        for (size_t d = 0; d < probe_dev_count; d++) {
            if (probe_devs[d] == dev) { pushed = true; break; }
        }
        if (pushed) continue;

        if (probe_dev_count >= probe_dev_cap) {
            size_t new_cap = probe_dev_cap ? probe_dev_cap * 2 : 16;
            dev_t *new_arr = realloc(probe_devs, new_cap * sizeof(dev_t));
            if (!new_arr) continue;
            probe_devs = new_arr;
            probe_dev_cap = new_cap;
        }
        probe_devs[probe_dev_count++] = dev;

        if (ctx->dev_mgr && ctx->probe_scheduler) {
            dev_mgr_mark_probing(ctx->dev_mgr, dev);
            ProbeTask task = {0};
            task.dev = dev;
            safe_strcpy(task.probe_path, e->path, sizeof(task.probe_path));
            task.next_probe_time = now; /* 立即到期，由 monitor 线程尽快调度 */
            task.probe_interval = PROBE_INTERVAL_INITIAL;
            task.retry_count = 0;
            task.s_status = SP_STATUS_PROBING;
            probe_scheduler_push(ctx->probe_scheduler, &task);
        }
    }

    log_info("[restore] spbin 分组：永久跳过 %lu，未超窗等待 %lu，超窗待探测 %lu（设备 %zu 个）",
             permanent, waiting, probing, probe_dev_count);
    free(probe_devs);
}

/**
 * @brief  顺序解析单个归档文件的所有块
 * @param  path           const char*      归档文件路径，不能为空
 * @param  ctx            AppContext*      应用上下文指针，不能为空
 * @param  discovered_set FingerprintSet*  本次任务的 discovered_set（仅目录），允许为 NULL
 * @param  ref_set        FingerprintSet*  盲信的 reference_set，允许为 NULL
 * @param  ref_map        ReferenceMap*    盲信的 reference_map，允许为 NULL
 * @return void
 *
 * @note   顺序读取 ArchiveBlockHeader，做 sanity check（block_type、大小上限 512MB）后，
 *         分配缓冲区、读取压缩数据、调用 uncompress 解压，再根据 block_type 分发到
 *         parse_pbin_buffer 或 parse_spbin_buffer。
 *         若某块校验失败或读取不完整，则停止继续解析该文件。
 */
static void iterate_archive_file(const char *path, AppContext *ctx,
                                 FingerprintSet *discovered_set,
                                 FingerprintSet *ref_set,
                                 ReferenceMap *ref_map) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return;

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
                parse_pbin_buffer(raw_buf, dest_len, 0, discovered_set, ref_set, ref_map);
            }
        }
        free(raw_buf);
        free(cmp_buf);
    }
    fclose(fp);
}

/**
 * @brief  遍历归档文件，解压并解析所有块
 * @param  base           const char*      进度文件前缀（盲信时为 --reference-base），不能为空
 * @param  ctx            AppContext*      应用上下文指针，不能为空
 * @param  discovered_set FingerprintSet*  本次任务的 discovered_set（仅目录），允许为 NULL
 * @param  ref_set        FingerprintSet*  盲信的 reference_set，允许为 NULL
 * @param  ref_map        ReferenceMap*    盲信的 reference_map，允许为 NULL
 * @return void
 *
 * @note   v15.6.0（P0-008 原子切换）：依次解析
 *         1. {base}.archive（已切换的正式归档；缺失时回退读 {base}.archive.prev——
 *            覆盖上一次切换在两个 rename 之间崩溃的窗口）；
 *         2. {base}.archive.new（未切换的在写副本，崩溃中断的归档在此）。
 *         集合/映射插入天然幂等，重复块无害。
 */
static void iterate_archive(const char *base, AppContext *ctx,
                            FingerprintSet *discovered_set,
                            FingerprintSet *ref_set,
                            ReferenceMap *ref_map) {
    char *archive_path = get_archive_filename(base);
    if (access(archive_path, F_OK) == 0) {
        iterate_archive_file(archive_path, ctx, discovered_set, ref_set, ref_map);
    } else {
        /* .archive 缺失但 .prev 存在：原子切换两 rename 之间崩溃的残留 */
        char *prev_path = get_archive_prev_filename(base);
        if (access(prev_path, F_OK) == 0) {
            iterate_archive_file(prev_path, ctx, discovered_set, ref_set, ref_map);
        }
        free(prev_path);
    }
    free(archive_path);

    char *new_path = get_archive_new_filename(base);
    if (access(new_path, F_OK) == 0) {
        iterate_archive_file(new_path, ctx, discovered_set, ref_set, ref_map);
    }
    free(new_path);
}

/**
 * @brief  遍历磁盘上散落的 pbin 分片并解析（v15.6.0：补 Footer 校验 + salvage）
 * @param  base           const char*      进度文件前缀（盲信时为 --reference-base），不能为空
 * @param  state          RuntimeState*    运行时状态指针（当前未使用，保留接口一致性）
 * @param  discovered_set FingerprintSet*  本次任务的 discovered_set（仅目录），允许为 NULL
 * @param  ref_set        FingerprintSet*  盲信的 reference_set，允许为 NULL
 * @param  ref_map        ReferenceMap*    盲信的 reference_map，允许为 NULL
 * @return void
 *
 * @note   从分片 0 开始顺序尝试打开，连续缺失超过 50 个且已超过 write_slice_index 时停止。
 *         对每个存在的分片：先校验末尾 Footer；Footer 无效时执行与 restore_progress
 *         同款的 salvage（顺序解析保留有效行、截断、重写 Footer，v15.6.0 P0-011
 *         补齐盲信加载路径），salvage 失败则跳过该分片。
 *         盲信场景解析的是基准（--reference-base），本函数绝不删除基准文件。
 */
static void iterate_pbin_slices(const char *base, RuntimeState *state,
                                FingerprintSet *discovered_set,
                                FingerprintSet *ref_set,
                                ReferenceMap *ref_map) {
    int consecutive_missing = 0;
    for (unsigned long s_idx = 0; ; ++s_idx) {
        char *slice_path = get_slice_filename(base, s_idx);
        FILE *slice_fp = fopen(slice_path, "rb");
        if (!slice_fp) {
            free(slice_path);
            consecutive_missing++;
            if (consecutive_missing > 50 && s_idx > state->write_slice_index) break;
            continue;
        }
        consecutive_missing = 0;

        /* v15.6.0（P0-011）：Footer 校验，损坏则 salvage 截断封口（盲信加载此前
         * 只验 Footer 不 salvage，撕尾分片会被连同有效行一起误解析/丢弃） */
        PbinFooter footer;
        if (!read_pbin_footer(slice_path, &footer)) {
            uint64_t valid_rows = 0;
            if (pbin_salvage_truncated(slice_path, &valid_rows)) {
                log_info("[restore] Salvaged pbin slice %lu, %lu valid rows", s_idx, valid_rows);
            } else {
                log_warn("[restore] pbin slice %lu corrupted and unsalvageable, skipped", s_idx);
                fclose(slice_fp);
                free(slice_path);
                continue;
            }
        }

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
            parse_pbin_buffer(buf, data_size, 0, discovered_set, ref_set, ref_map);
            free(buf);
        }
        fclose(slice_fp);
        free(slice_path);
    }
}

/**
 * @brief  加载 dpbin 分片到 completed_set
 * @param  cfg           const Config*  全局配置指针，不能为空
 * @param  completed_set FingerprintSet*  已完成目录集合，不能为空
 * @return void
 *
 * @note   遍历所有 dpbin_*. 分片，解析记录并计算 fingerprint 插入 completed_set。
 *         dpbin 格式与 pbin 同构，复用 parse_pbin_buffer 解析。
 */
static void load_dpbin_to_completed_set(const Config *cfg, FingerprintSet *completed_set) {
    int consecutive_missing = 0;
    for (unsigned long s_idx = 0; ; ++s_idx) {
        char *slice_path = get_dpbin_slice_filename(cfg->progress_base, s_idx);
        FILE *slice_fp = fopen(slice_path, "rb");
        if (!slice_fp) {
            free(slice_path);
            consecutive_missing++;
            if (consecutive_missing > 50) break;
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
            parse_pbin_buffer(buf, data_size, 0, completed_set, NULL, NULL);
            free(buf);
        }
        fclose(slice_fp);
        free(slice_path);
    }
}

/**
 * @brief  统计单个归档文件中 Normal 块的数量（SPBIN 块不计入）
 * @param  path  const char*  归档文件路径，不能为空
 * @return unsigned long  Normal 块的数量
 */
static unsigned long count_archive_blocks_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;

    unsigned long count = 0;
    ArchiveBlockHeader bh;
    while (fread(&bh, sizeof(bh), 1, fp) == 1) {
        if (bh.block_type != ARCHIVE_BLOCK_NORMAL && bh.block_type != ARCHIVE_BLOCK_SPBIN) break;
        if (bh.compressed_size == 0 || bh.compressed_size > 512 * 1024 * 1024) break;
        if (bh.block_type == ARCHIVE_BLOCK_NORMAL)
            count++;
        if (fseek(fp, bh.compressed_size, SEEK_CUR) != 0) break;
    }
    fclose(fp);
    return count;
}

/**
 * @brief  统计归档（正式 + 在写副本，必要时含旧基准保留件）中 Normal 块的数量
 * @param  base  const char*  进度文件前缀，不能为空
 * @return unsigned long  Normal 块的总数量
 *
 * @note   v15.6.0（P0-008 原子切换）：统计口径与 iterate_archive 一致——
 *         {base}.archive（缺失时回退 .prev）+ {base}.archive.new。
 */
static unsigned long count_archive_blocks(const char *base) {
    unsigned long count = 0;
    char *archive_path = get_archive_filename(base);
    if (access(archive_path, F_OK) == 0) {
        count += count_archive_blocks_file(archive_path);
    } else {
        char *prev_path = get_archive_prev_filename(base);
        count += count_archive_blocks_file(prev_path);
        free(prev_path);
    }
    free(archive_path);

    char *new_path = get_archive_new_filename(base);
    count += count_archive_blocks_file(new_path);
    free(new_path);
    return count;
}

/**
 * @brief  统计磁盘上散落 pbin 分片的数量
 * @param  cfg  const Config*  全局配置指针，不能为空
 * @return unsigned long  存在的分片数量
 *
 * @note   从分片 0 开始顺序检查，连续缺失超过 50 个时停止。
 */
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

static void fpbin_clear_mem(AppContext *ctx) {
    for (size_t i = 0; i < ctx->fpbin_count; i++) {
        free(ctx->fpbin_entries[i]);
    }
    ctx->fpbin_count = 0;
}


/**
 * @brief  查找首个与最大现存 pbin 分片序号（v15.6.0）
 * @param  cfg      const Config*    全局配置指针
 * @param  min_idx  unsigned long*  输出首个现存分片序号，允许 NULL
 * @param  max_idx  unsigned long*  输出最大现存分片序号，允许 NULL
 * @return bool     存在任何分片返回 true；一个都没有返回 false
 *
 * @note   find_max_pbin_index 无法区分"无分片"与"最大序号为 0"，
 *         导致恢复时空进度把首个写入分片定为 1、泵送却仍从 0 开始
 *         （打不开即不泵，已发现未完成目录永久丢失——回归实测漏扫）。
 *         本函数显式区分两种情形，供恢复路径同时确定泵送起点与写入序号。
 */
static bool find_pbin_index_bounds(const Config *cfg, unsigned long *min_idx, unsigned long *max_idx) {
    bool found = false;
    unsigned long lo = 0, hi = 0;
    int consecutive_missing = 0;
    for (unsigned long i = 0; consecutive_missing <= 50; ++i) {
        char *slice_path = get_slice_filename(cfg->progress_base, i);
        if (access(slice_path, F_OK) == 0) {
            if (!found) { lo = i; found = true; }
            hi = i;
            consecutive_missing = 0;
        } else {
            consecutive_missing++;
        }
        free(slice_path);
    }
    if (found) {
        if (min_idx) *min_idx = lo;
        if (max_idx) *max_idx = hi;
    }
    return found;
}

/**
 * @brief  查找当前最大的 dpbin 分片索引
 * @param  base  const char*  进度文件基础名，不能为空
 * @return unsigned long  最大存在的分片编号；无分片时返回 0
 *
 * @note   从 0 开始顺序检查，连续缺失超过 50 个时停止。
 */
static unsigned long find_max_dpbin_index(const char *base) {
    unsigned long max_idx = 0;
    int consecutive_missing = 0;
    for (unsigned long i = 0; consecutive_missing <= 50; ++i) {
        char *slice_path = get_dpbin_slice_filename(base, i);
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

/**
 * @brief  将 dfpbin 分片合并入 dpbin（v15.6.0，P0-003 原子对同步转正）
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 *
 * @note   流程：
 *         1. 封口当前活跃 dfpbin 与 dpbin 分片（写 Footer，避免序号冲突）
 *         2. 所有非空 dfpbin 分片 rename 到 dpbin 最大序号之后
 *         3. 重置 dfpbin/dpbin 写状态（后续 dfpbin_append 从分片 0 重新开始）
 *         fpbin 转正成功（或 OLD 阶段无 fpbin 产出而终结）时调用；
 *         无 dfpbin 数据时为 no-op。
 */
static void merge_dfpbin_into_dpbin(AppContext *ctx) {
    /* 1. Seal active dfpbin slice */
    if (ctx->dfpbin_slice_file) {
        write_pbin_footer(ctx->dfpbin_slice_file, ctx->dfpbin_line_count);
        fclose(ctx->dfpbin_slice_file);
        ctx->dfpbin_slice_file = NULL;
    }

    /* 2. Seal active dpbin slice as well, so renamed dfpbin slices never
     *    collide with a later dpbin rotation (rotation = write_slice_index+1). */
    if (ctx->dpbin_slice_file) {
        write_pbin_footer(ctx->dpbin_slice_file, ctx->dpbin_line_count);
        fclose(ctx->dpbin_slice_file);
        ctx->dpbin_slice_file = NULL;
    }

    /* 3. Rename all non-empty dfpbin slices after the max dpbin index */
    unsigned long dpbin_next = find_max_dpbin_index(ctx->cfg.progress_base) + 1;
    bool any_dpbin = false;
    {
        char *p0 = get_dpbin_slice_filename(ctx->cfg.progress_base, 0);
        any_dpbin = (access(p0, F_OK) == 0);
        free(p0);
    }
    if (!any_dpbin) dpbin_next = 0; /* 无 dpbin 分片时从 0 开始 */
    for (unsigned long i = 0; i < 1024; i++) {
        char *src = get_dfpbin_slice_filename(ctx->cfg.progress_base, i);
        struct stat fst;
        if (stat(src, &fst) == 0) {
            if (fst.st_size > 0) {
                char *dst = get_dpbin_slice_filename(ctx->cfg.progress_base, dpbin_next++);
                if (rename(src, dst) != 0) {
                    log_error("dfpbin 合并 rename 失败: %s -> %s", src, dst);
                }
                free(dst);
            } else {
                unlink(src); /* 空分片（open 后未写即崩溃）直接删除 */
            }
        }
        free(src);
    }

    /* 4. Reset write states */
    ctx->dfpbin_write_slice_index = 0;
    ctx->dfpbin_line_count = 0;
    ctx->dpbin_write_slice_index = dpbin_next;
    ctx->dpbin_line_count = 0;
}

/**
 * @brief  将 fpbin 临时分片转正为正式 pbin 分片
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 *
 * @note   转正流程（fpbin → pbin）：
 *         1. 将内存中残留条目刷出到当前 fpbin 分片
 *         2. 封口最后一个 fpbin 分片（写 Footer）
 *         3. 若从未创建过 fpbin 文件但有内存数据，创建 slice 0 并写入
 *         4. 统计磁盘上非空 fpbin 分片（空分片直接删除——v15.6.0 修复：
 *            无 fpbin 数据时转正是合法 no-op，不得 log_fatal）
 *         5. 计算 pbin 起始编号（max_pbin_index + 1），rename 所有有效 fpbin 分片
 *         6. 逐个读取转正后 pbin 的 Footer 进行校验
 *         7. 校验全部通过后删除 fpbin.idx 和残留 fpbin 文件，
 *            并同步将 dfpbin 合并入 dpbin（P0-003 原子对）
 *         8. 更新 pump 源到第一个转正后的 pbin，状态切换为 HIST_PUMP_NEW
 *         若校验失败，保留 fpbin.idx 以便下次恢复时重试转正。
 */
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

    /* 3. v15.6.0: 从磁盘统计实际存在的非空 fpbin 分片（不再依赖 fpbin.idx
     *    推算个数）。空分片（fopen 后未写即崩溃，无 Footer 可校验）直接删除，
     *    不参与 rename——无 fpbin 数据时转正是合法 no-op，不得 log_fatal。 */
    unsigned long valid_idx[1024];
    unsigned long fpbin_total = 0;
    for (unsigned long i = 0; i < 1024; i++) {
        char *src = get_fpbin_slice_filename(ctx->cfg.progress_base, i);
        struct stat fst;
        if (stat(src, &fst) == 0) {
            if (fst.st_size > 0) {
                valid_idx[fpbin_total++] = i;
            } else {
                unlink(src);
            }
        }
        free(src);
    }
    if (fpbin_total == 0) {
        /* 无 fpbin 数据：清理残留 idx，同步合并 dfpbin 后结束泵送 */
        char *fpbin_idx = get_fpbin_index_filename(ctx->cfg.progress_base);
        unlink(fpbin_idx);
        free(fpbin_idx);
        merge_dfpbin_into_dpbin(ctx);
        ctx->hist_pump_state = HIST_PUMP_DONE;
        return;
    }

    /* 4. Rename all valid fpbin slices to pbin */
    unsigned long pmin = 0, pmax = 0;
    unsigned long pbin_start_idx = find_pbin_index_bounds(&ctx->cfg, &pmin, &pmax) ? pmax + 1 : 0;
    for (unsigned long j = 0; j < fpbin_total; j++) {
        char *src = get_fpbin_slice_filename(ctx->cfg.progress_base, valid_idx[j]);
        char *dst = get_slice_filename(ctx->cfg.progress_base, pbin_start_idx + j);
        if (rename(src, dst) != 0) {
            log_error("fpbin 转正 rename 失败: %s -> %s", src, dst);
        }
        free(src);
        free(dst);
    }

    /* 5. Verify all promoted pbin slices */
    bool all_ok = true;
    for (unsigned long j = 0; j < fpbin_total; j++) {
        char *path = get_slice_filename(ctx->cfg.progress_base, pbin_start_idx + j);
        PbinFooter f;
        if (!read_pbin_footer(path, &f)) {
            log_error("转正后 pbin Footer 校验失败: %s", path);
            all_ok = false;
        }
        free(path);
    }
    if (!all_ok) {
        log_fatal("fpbin 转正校验未通过，建议手动清理后重试");
        /* 不删除 fpbin.idx，以便下次恢复时重试转正 */
        ctx->hist_pump_state = HIST_PUMP_DONE;
        return;
    }

    /* 6. Cleanup fpbin artifacts */
    char *fpbin_idx = get_fpbin_index_filename(ctx->cfg.progress_base);
    unlink(fpbin_idx);
    free(fpbin_idx);
    for (unsigned long i = 0; i < 1024; i++) {
        char *src = get_fpbin_slice_filename(ctx->cfg.progress_base, i);
        unlink(src);
        free(src);
    }

    /* 7. v15.6.0（P0-003）：fpbin 转正成功，同步将 dfpbin 合并入 dpbin。
     *    此后不再存在 fpbin/dfpbin 残留，下次恢复走普通 dpbin 差集路径。 */
    merge_dfpbin_into_dpbin(ctx);

    /* 8. Update pump source to first promoted pbin */
    if (ctx->hist_pump_fp) fclose(ctx->hist_pump_fp);
    char *first_pbin = get_slice_filename(ctx->cfg.progress_base, pbin_start_idx);
    ctx->hist_pump_fp = fopen(first_pbin, "rb");
    free(first_pbin);
    ctx->hist_pump_slice_idx = pbin_start_idx;
    ctx->hist_pump_line_no = 0;

    /* 9. Reset pbin write state to after promoted slices */
    ctx->state.write_slice_index = pbin_start_idx + fpbin_total;
    ctx->state.line_count = 0;
    if (ctx->state.write_slice_file) {
        fclose(ctx->state.write_slice_file);
        ctx->state.write_slice_file = NULL;
    }

    ctx->hist_pump_state = HIST_PUMP_NEW;
}

/**
 * @brief  当前 pbin 分片消费完毕时的回调
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 *
 * @note   关闭当前分片，尝试打开下一个散落分片；若无更多散落分片则检查 fpbin；
 *         若 fpbin 存在则触发转正流程；否则标记泵送完成（HIST_PUMP_DONE）。
 */
static void on_pbin_slice_consumed(AppContext *ctx) {
    if (ctx->hist_pump_fp) {
        fclose(ctx->hist_pump_fp);
        ctx->hist_pump_fp = NULL;
    }

    /* Try to open next scattered slice, skipping gaps (archived slices) */
    int missing = 0;
    while (missing <= 50) {
        ctx->hist_pump_slice_idx++;
        char *next_path = get_slice_filename(ctx->cfg.progress_base, ctx->hist_pump_slice_idx);
        ctx->hist_pump_fp = fopen(next_path, "rb");
        free(next_path);
        if (ctx->hist_pump_fp) {
            ctx->hist_pump_line_no = 0;
            return;  /* Continue pumping next slice */
        }
        missing++;
    }

    /* No more scattered slices. Check fpbin. */
    if (ctx->fpbin_count > 0 || ctx->fpbin_slice_file) {
        promote_fpbin_to_pbin(ctx);
        return;
    }

    /* v15.6.0（P0-003）：OLD 阶段终结且无 fpbin 产出——仍将 dfpbin 并入 dpbin，
     * 避免 dfpbin 单侧残留导致下次恢复误判 fpbin/dfpbin 原子对不完整。
     * 无 dfpbin 数据时为 no-op。 */
    merge_dfpbin_into_dpbin(ctx);

    /* Nothing left to pump */
    ctx->hist_pump_state = HIST_PUMP_DONE;
}

/**
 * @brief  从当前 pbin 文件读取下一条记录（v15.6.0 schema 2，设计 §0.2）
 * @param  fp          FILE*              已打开的 pbin 文件指针，不能为空
 * @param  out_path    char**             输出路径字符串指针的指针，成功时指向新分配的字符串，调用方负责 free
 * @param  out_st      struct stat*       输出 stat 结构体指针，不能为空
 * @param  out_d_type  unsigned char*     输出文件类型指针，不能为空
 * @return bool  返回 true 表示读取成功；false 表示 EOF 或格式错误
 *
 * @note   对 path_len 做防御性校验（> MAX_PATH_LENGTH 则视为损坏数据）。
 *         schema 2 记录携带完整 stat 字段（mtime/mtime_nsec/size/uid/gid/mode/
 *         dev/ino），out_st 全量回填；mode 为 0（空 stat 记录）时按 d_type
 *         回填文件类型位兜底。atime 不记录（NFS 不可信）。
 */
bool read_next_pbin_record(FILE *fp, char **out_path, struct stat *out_st, unsigned char *out_d_type) {
    size_t path_len;
    if (fread(&path_len, sizeof(size_t), 1, fp) != 1) return false;

    /* 防御性校验：防止读取到 Footer magic 或损坏数据 */
    if (path_len == 0 || path_len > MAX_PATH_LENGTH) {
        return false;
    }

    char *path = malloc(path_len + 1);
    if (!path) return false;
    if (fread(path, 1, path_len, fp) != path_len) { free(path); return false; }
    path[path_len] = '\0';

    unsigned char d_type;
    time_t mtime_sec;
    long mtime_nsec;
    off_t fsize;
    uint32_t uid, gid, mode, flags;
    uint64_t dev, ino;
    if (fread(&d_type, sizeof(unsigned char), 1, fp) != 1) { free(path); return false; }
    if (fread(&mtime_sec, sizeof(time_t), 1, fp) != 1) { free(path); return false; }
    if (fread(&mtime_nsec, sizeof(long), 1, fp) != 1) { free(path); return false; }
    if (fread(&fsize, sizeof(off_t), 1, fp) != 1) { free(path); return false; }
    if (fread(&uid, sizeof(uint32_t), 1, fp) != 1) { free(path); return false; }
    if (fread(&gid, sizeof(uint32_t), 1, fp) != 1) { free(path); return false; }
    if (fread(&mode, sizeof(uint32_t), 1, fp) != 1) { free(path); return false; }
    if (fread(&dev, sizeof(uint64_t), 1, fp) != 1) { free(path); return false; }
    if (fread(&ino, sizeof(uint64_t), 1, fp) != 1) { free(path); return false; }
    if (fread(&flags, sizeof(uint32_t), 1, fp) != 1) { free(path); return false; }
    (void)flags; /* 预留字段，当前不解释 */

    memset(out_st, 0, sizeof(*out_st));
    out_st->st_dev = (dev_t)dev;
    out_st->st_ino = (ino_t)ino;
    out_st->st_mode = (mode_t)mode;
    out_st->st_size = fsize;
    out_st->st_uid = (uid_t)uid;
    out_st->st_gid = (gid_t)gid;
    out_st->st_mtim.tv_sec = mtime_sec;
    out_st->st_mtim.tv_nsec = mtime_nsec;
    if (out_st->st_mode == 0) {
        /* 空 stat 记录兜底：按 d_type 回填文件类型位 */
        out_st->st_mode = (d_type == DT_DIR) ? S_IFDIR :
                          (d_type == DT_REG) ? S_IFREG :
                          (d_type == DT_LNK) ? S_IFLNK :
                          (d_type == DT_CHR) ? S_IFCHR :
                          (d_type == DT_BLK) ? S_IFBLK :
                          (d_type == DT_FIFO) ? S_IFIFO :
                          (d_type == DT_SOCK) ? S_IFSOCK : 0;
    }
    *out_path = path;
    *out_d_type = d_type;
    return true;
}

/**
 * @brief  从历史 pbin 分片中泵送一批目录给 Worker
 * @param  ctx        AppContext*  应用上下文指针，不能为空
 * @param  batch_size int          每批发送的目录数量，取值范围: > 0
 * @return void
 *
 * @note   从 hist_pump_fp 顺序读取记录，仅对 DT_DIR 类型的条目创建扫描任务并发送给 Worker。
 *         每批最多发送 batch_size 个目录。读取的目录会重新计算指纹并插入 discovered_set 防重复发现。
 *         当当前分片读完后自动调用 on_pbin_slice_consumed 切换到下一片或结束泵送。
 */
void pump_pbin_batch(AppContext *ctx, int batch_size) {
    if (!ctx->hist_pump_fp) return;

    log_debug("[Pump] pump_pbin_batch start (hist_state=%d, line=%zu)", ctx->hist_pump_state, ctx->hist_pump_line_no);

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

        if (d_type == DT_DIR) {
            /* v15.6.0（P0-005）：spbin 中的熔断/跳过目录不经泵送盲目重入队——
             * 它们处于 DEVICE_WAITING（等待设备探测恢复）或永久跳过。
             * 设备恢复后由 spbin_requeue_recovered 统一重入队（绕过集合去重，
             * 因为其首次发现时已登记 enqueued_set）。 */
            uint8_t fp0[FP_SIZE];
            fp_compute(path, 0, 0, fp0);
            if (ctx->spbin_set && fp_set_contains(ctx->spbin_set, fp0)) {
                free(path);
                continue;
            }

            /* v15.6.0（P0-004）：目录指纹入 discovered_set 防重复发现；
             * 文件不进入目录任务去重集合（输出语义 at-least-once）。 */
            uint8_t fp_dir[FP_SIZE];
            fp_compute(path, st.st_dev, st.st_ino, fp_dir);
            fp_set_insert(ctx->discovered_set, fp_dir);

            /* v15.6.0（P0-004）：统一 enqueue_dir 入口——completed_set 差集剪枝、
             * enqueued_set 防重复入队、dispatch_queue/dspill 分流均在其内完成。
             * 不旁路直发 CMD_SCAN——旁路不占 BUSY 态、不登记 epoch，且轮询 wid
             * 可能向同一 slot 叠加多个在途任务，破坏 epoch 校验（BATCH/FINISH 被
             * 误判残留丢弃 → pending_tasks 泄漏卡死）。由 dispatch_from_queue 负责
             * pending_tasks++、epoch 分配、Worker 状态与 dpbin/dfpbin。 */
            enqueue_dir(ctx, path, &st);
            sent++;
        }
        free(path);
    }
    log_debug("[Pump] pump_pbin_batch done, sent=%d, pending_tasks=%ld", sent, atomic_load(&ctx->pending_tasks));
}

/**
 * @brief  校验 fpbin/dfpbin 原子对单侧的所有分片（v15.6.0，P0-003）
 * @param  base         const char*  进度文件基础名，不能为空
 * @param  is_fpbin     bool         true=检查 fpbin 分片；false=检查 dfpbin 分片
 * @param  has_residue  bool*        输出参数，返回该侧是否存在任何残留分片
 * @return bool  返回 true 表示该侧可信（所有存在的分片 Footer 校验通过，
 *               或经 salvage 截断封口后有效）；false 表示存在不可信分片
 *
 * @note   崩溃时活跃分片通常无 Footer（封口仅发生在轮转/转正时），
 *         因此对 Footer 缺失的分片按 restore 对 pbin 的同款策略执行 salvage：
 *         顺序解析保留有效行、截断、重写 Footer。salvage 也失败（0 有效行，
 *         如 fopen 后未写即崩溃的空分片）才判为不可信。
 */
static bool check_pair_side_slices(const char *base, bool is_fpbin, bool *has_residue) {
    *has_residue = false;
    bool ok = true;
    for (unsigned long i = 0; i < 1024; i++) {
        char *path = is_fpbin ? get_fpbin_slice_filename(base, i)
                              : get_dfpbin_slice_filename(base, i);
        if (access(path, F_OK) == 0) {
            *has_residue = true;
            PbinFooter f;
            if (!read_pbin_footer(path, &f)) {
                uint64_t valid_rows = 0;
                if (pbin_salvage_truncated(path, &valid_rows)) {
                    log_info("[restore] Salvaged %s slice %lu, %lu valid rows",
                             is_fpbin ? "fpbin" : "dfpbin", i, valid_rows);
                } else {
                    log_warn("[restore] %s slice %lu corrupted and unsalvageable",
                             is_fpbin ? "fpbin" : "dfpbin", i);
                    ok = false;
                }
            }
        }
        free(path);
    }
    return ok;
}

/**
 * @brief  恢复模式：从归档和散落 pbin 加载进度并设置泵送状态
 * @param  cfg  const Config*  全局配置指针，不能为空
 * @param  ctx  AppContext*     应用上下文指针，不能为空
 * @return int  返回 0 表示恢复成功（或无需恢复）
 *
 * @note   v15.6.0（P0-010）Reset 援救原则：不枚举崩溃点。凡目录指纹 ∉
 *         completed_set（未写 dpbin/dfpbin 合并集，即状态 ≠ COMPLETED）一律视为
 *         未扫描，经泵送差集重新入队。重扫幂等性由 discovered_set（防重复发现）、
 *         enqueued_set（防重复入队）与输出 at-least-once（允许重复行）共同保证。
 *
 *         恢复流程：
 *         1. 重置 fpbin/dfpbin 和 pump 状态
 *         2. 统计归档块数和散落分片数
 *         3. 加载归档文件内容到 discovered_set（仅目录记录）
 *         4. 加载散落 pbin 分片（Footer 优先，损坏则 salvage 截断封口）
 *         5. fpbin + dfpbin 原子对校验（P0-003）：
 *            - 两侧均无残留 → 正常路径
 *            - 两侧分片 Footer 全部校验通过（含 salvage 封口）→ fpbin 转正合并入
 *              pbin、dfpbin 合并入 dpbin，清空残留
 *            - 任一侧缺失/校验失败 → 整对抛弃（unlink fpbin+dfpbin 全部残留），
 *              并清空 dpbin（completed_set 随之为空），从旧 pbin 全量差集恢复。
 *              本路径不产生新 fpbin（不会无限套娃）
 *         6. 加载 dpbin（含合并的 dfpbin）到 completed_set（差集剪枝）
 *         6.5 spbin 纳入恢复（P0-005）：读 {base}.spbin，按 device_key 分组——
 *            PERMISSION/CIRCUIT_BREAKER/POISON 永久跳过；PROBE_FAIL/TIMEOUT
 *            按 timestamp + 退避窗口（30min→2h→6h→24h）决定保持跳过或敢死队探测；
 *            spbin_set 拦截泵送/dspill 对熔断目录的盲目重入队
 *         7. 读取 dspill 兜底文件，DT_DIR 记录经 enqueue_dir 回填（P0-004/P1-001）
 *         8. 打开新的 pbin 活跃分片
 *         9. 打开第一个 pbin 分片，设置 pump 状态（HIST_PUMP_OLD）
 */
int restore_progress(const Config *cfg, AppContext *ctx) {
    /* 1. Reset fpbin/dfpbin state (keep residual files for pair validation) */
    fpbin_clear_mem(ctx);
    if (ctx->fpbin_slice_file) {
        fclose(ctx->fpbin_slice_file);
        ctx->fpbin_slice_file = NULL;
    }
    ctx->fpbin_write_slice_index = 0;
    ctx->fpbin_line_count = 0;
    if (ctx->dfpbin_slice_file) {
        fclose(ctx->dfpbin_slice_file);
        ctx->dfpbin_slice_file = NULL;
    }
    ctx->dfpbin_write_slice_index = 0;
    ctx->dfpbin_line_count = 0;
    ctx->hist_pump_state = HIST_PUMP_DONE;
    ctx->hist_pump_fp = NULL;
    ctx->hist_pump_slice_idx = 0;
    ctx->hist_pump_line_no = 0;

    unsigned long pbin_count  = count_pbin_slices(cfg);
    unsigned long archive_blk = count_archive_blocks(cfg->progress_base);
    unsigned long total_blocks = pbin_count + archive_blk;

    /* Sanity check: total_blocks should never exceed a reasonable limit. */
    if (total_blocks > 1000000000UL) {
        log_warn("[restore] total_blocks=%lu exceeds sanity limit, forcing full rescan", total_blocks);
        total_blocks = 0;
        pbin_count = 0;
        archive_blk = 0;
    }

    /* 2. Load archive (completed slices) into discovered_set (dirs only) */
    iterate_archive(cfg->progress_base, ctx, ctx->discovered_set, NULL, NULL);

    /* 3. Load scattered pbin slices with Footer-first recovery and salvage */
    int consecutive_missing = 0;
    for (unsigned long s_idx = 0; ; ++s_idx) {
        char *slice_path = get_slice_filename(cfg->progress_base, s_idx);
        FILE *slice_fp = fopen(slice_path, "rb");
        if (!slice_fp) {
            free(slice_path);
            consecutive_missing++;
            if (consecutive_missing > 50) break;
            continue;
        }
        consecutive_missing = 0;

        /* Check Footer, salvage if truncated */
        PbinFooter footer;
        bool footer_ok = read_pbin_footer(slice_path, &footer);
        if (!footer_ok) {
            uint64_t valid_rows;
            if (pbin_salvage_truncated(slice_path, &valid_rows)) {
                log_info("[restore] Salvaged pbin slice %lu, %lu valid rows", s_idx, valid_rows);
                footer_ok = true;
            } else {
                log_warn("[restore] pbin slice %lu corrupted and unrecoverable, deleting", s_idx);
                unlink(slice_path);
                free(slice_path);
                fclose(slice_fp);
                continue;
            }
        }

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
            parse_pbin_buffer(buf, data_size, 0, ctx->discovered_set, NULL, NULL);
            free(buf);
        }
        fclose(slice_fp);
        free(slice_path);
    }

    /* 4. v15.6.0（P0-003）：fpbin + dfpbin 原子对校验。
     *    - 两侧均无残留 → 正常路径；
     *    - 两侧所有分片 Footer 校验通过（含 salvage 封口）→ fpbin 转正合并入
     *      pbin、dfpbin 合并入 dpbin（在 promote_fpbin_to_pbin 内同步完成）；
     *    - 任一侧缺失/校验失败 → 整对抛弃，并清空 dpbin（Reset 援救）：上一
     *      续传会话的完成记录不再可信，completed_set 置空，从旧 pbin 全量差集
     *      重新恢复（at-least-once 允许重复输出）。本路径不产生新 fpbin。 */
    {
        bool fpbin_slices = false, dfpbin_residue = false;
        bool fpbin_ok = check_pair_side_slices(cfg->progress_base, true, &fpbin_slices);
        bool dfpbin_ok = check_pair_side_slices(cfg->progress_base, false, &dfpbin_residue);
        char *fpbin_idx_path = get_fpbin_index_filename(cfg->progress_base);
        bool fpbin_residue = fpbin_slices || (access(fpbin_idx_path, F_OK) == 0);

        if (!fpbin_residue && !dfpbin_residue) {
            /* 无残留：正常路径 */
            free(fpbin_idx_path);
        } else if (fpbin_slices && fpbin_ok && dfpbin_residue && dfpbin_ok) {
            /* 完整对（idx 不作为完整性的充分条件：仅有 idx 而无有效分片视为缺失） */
            verbose_printf(cfg, 1, "检测到完整 fpbin+dfpbin 原子对，执行转正恢复...\n");
            promote_fpbin_to_pbin(ctx);
            free(fpbin_idx_path);
            verbose_printf(cfg, 1, "fpbin/dfpbin 转正恢复完成\n");
            /* After promotion, load the newly promoted pbin slices into discovered_set */
            iterate_pbin_slices(cfg->progress_base, &ctx->state, ctx->discovered_set, NULL, NULL);
        } else {
            log_warn("[restore] fpbin/dfpbin 原子对不完整（fpbin 残留=%d 可信=%d，"
                     "dfpbin 残留=%d 可信=%d），整对抛弃并清空 dpbin，从旧 pbin 全量差集恢复",
                     fpbin_residue, fpbin_ok, dfpbin_residue, dfpbin_ok);
            for (unsigned long i = 0; i < 1024; i++) {
                char *fp = get_fpbin_slice_filename(cfg->progress_base, i);
                unlink(fp);
                free(fp);
            }
            unlink(fpbin_idx_path);
            free(fpbin_idx_path);
            dfpbin_delete_all(cfg->progress_base);
            dpbin_delete_all(cfg->progress_base);
            /* completed_set 在下一步才创建，dpbin 已清空 → 自然为空集 */
        }
    }

    /* 5. Load dpbin (incl. merged dfpbin) into completed_set (differential resume) */
    ctx->completed_set = fp_set_create(cfg->estimated_files);
    load_dpbin_to_completed_set(cfg, ctx->completed_set);

    /* 5.5 v15.6.0（P0-005）：spbin 纳入恢复——读独立 {base}.spbin 并入内存
     *    （archive SPBIN 块已在步骤 2 经 parse_spbin_buffer 合并），按 reason 分类、
     *    按 device_key 分组：PERMISSION/CIRCUIT_BREAKER/POISON 永久跳过；
     *    PROBE_FAIL/TIMEOUT 按 timestamp + 退避窗口决定保持跳过或敢死队探测。
     *    必须先于 dspill 回填与泵送：spbin_set 是它们的拦截依据。 */
    restore_spbin(cfg, ctx);

    /* 6. v15.6.0（P0-004/P1-001）：dspill 纳入恢复——崩溃前 HIGH_WATER 跳推的
     *    目录经 enqueue_dir 回填（enqueued_set 自动去重：与泵送差集重叠的目录
     *    只会入队一次）。pbin 记录格式流式读，读坏即停。
     *    安全性：dspill 条目在发现时已写 pbin（或父目录未完成会被重扫重新发现），
     *    故 dspill 读取失败/截断不丢目录——pbin 泵送与 Reset 援救兜底。
     *    先 unlink 再读：入队溢出重写 dspill 时会重建同名新文件，
     *    已 unlink 的旧文件句柄不受影响，新文件留给运行时加载器正常回填。 */
    {
        char *spill_path = get_dspill_filename(cfg->progress_base);
        FILE *spill_fp = spill_path ? fopen(spill_path, "rb") : NULL;
        if (spill_fp) {
            unsigned long requeued = 0;
            unlink(spill_path);
            while (1) {
                char *path = NULL;
                struct stat st;
                unsigned char d_type;
                if (!read_next_pbin_record(spill_fp, &path, &st, &d_type)) break;
                if (d_type == DT_DIR) {
                    /* v15.6.0（P0-005）：spbin 中的熔断/跳过目录不经 dspill 盲目重入队 */
                    uint8_t fp0[FP_SIZE];
                    fp_compute(path, 0, 0, fp0);
                    if (ctx->spbin_set && fp_set_contains(ctx->spbin_set, fp0)) {
                        free(path);
                        continue;
                    }
                    enqueue_dir(ctx, path, &st);
                    requeued++;
                }
                free(path);
            }
            fclose(spill_fp);
            if (requeued > 0) {
                log_info("[restore] dspill 回填 %lu 个跳推目录", requeued);
            }
        }
        free(spill_path);
    }

    /* 7. Open new pbin slice for writing（无现存分片时从 0 开始，不能用 max+1——
     *    find_max 无法区分"无分片"与"最大为 0"，否则首个分片被定为 1 而泵送
     *    从 0 开始打不开，已发现未完成目录永久丢失，回归实测漏扫） */
    unsigned long pbin_min = 0, pbin_max = 0;
    bool pbin_any = find_pbin_index_bounds(cfg, &pbin_min, &pbin_max);
    ctx->state.write_slice_index = pbin_any ? pbin_max + 1 : 0;
    ctx->state.line_count = 0;
    if (ctx->state.write_slice_file) {
        fclose(ctx->state.write_slice_file);
        ctx->state.write_slice_file = NULL;
    }
    char *new_slice = get_slice_filename(cfg->progress_base, ctx->state.write_slice_index);
    ctx->state.write_slice_file = fopen(new_slice, "wb");
    free(new_slice);

    /* 8. Open first EXISTING pbin slice for pumping (differential: skip completed in enqueue_dir) */
    if (ctx->hist_pump_fp) {
        /* 转正路径已打开首个转正分片——统一从首个现存分片起泵（OLD），先关闭防 fd 泄漏 */
        fclose(ctx->hist_pump_fp);
        ctx->hist_pump_fp = NULL;
    }
    if (pbin_any) {
        char *first_slice = get_slice_filename(cfg->progress_base, pbin_min);
        ctx->hist_pump_fp = fopen(first_slice, "rb");
        free(first_slice);
        if (ctx->hist_pump_fp) {
            ctx->hist_pump_state = HIST_PUMP_OLD;
            ctx->hist_pump_slice_idx = pbin_min;
            ctx->hist_pump_line_no = 0;
        }
    }

    verbose_printf(cfg, 1, "进度加载完成\n");
    return 0;
}

/**
 * @brief  盲信模式：将基准索引加载到内存中的 reference_set/map（v15.6.0，P0-011）
 * @param  cfg  const Config*  全局配置指针，不能为空
 * @param  ctx  AppContext*    应用上下文指针，不能为空
 * @param  base const char*    盲信基准 progress 前缀（--reference-base），不能为空
 * @return void
 *
 * @note   遍历基准的归档文件（.archive/.prev/.archive.new）和散落 pbin 分片，
 *         将纯路径指纹（xxHash3-128(path)，不带 dev/ino）插入 reference_set，
 *         将完整历史 stat（mtime/mtime_nsec/size/uid/gid/mode/d_type）插入
 *         reference_map，供 Worker try_blind_trust 命中后原样复用。
 *         分片加载经 iterate_pbin_slices 统一补 salvage（Footer 损坏截断封口），
 *         基准文件只读不删（salvage 仅截断撕尾）。
 */
void restore_progress_to_memory(const Config *cfg, AppContext *ctx, const char *base) {
    verbose_printf(cfg, 1, "开始加载盲信基准索引（%s）...\n", base);
    iterate_archive(base, ctx, NULL, ctx->reference_set, ctx->reference_map);
    iterate_pbin_slices(base, &ctx->state, NULL, ctx->reference_set, ctx->reference_map);
    verbose_printf(cfg, 1, "历史索引加载完成\n");
}

