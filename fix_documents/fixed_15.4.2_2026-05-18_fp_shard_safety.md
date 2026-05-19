# fixed v15.4.2 — fp_shard_insert_internal 安全加固

日期：2026-05-18

## 问题

1. `fp_set_create()` 中 `expected_count * 2` 溢出导致 per_shard 极小值。
2. `PROBE_LIMIT = 1000000` 截断小容量分片的探测循环。
3. rehash 失败时旧 table 已丢失，数据永久丢失。

## 修复

1. `expected_count > (SIZE_MAX / 4)` 时返回 NULL，饱和防溢出。
2. `PROBE_LIMIT` → `shard->capacity`，消除截断。
3. rehash 失败时完整回滚旧指针、旧 capacity、旧 count/tombstones，数据零丢失。

## 修改文件

- `src/scan/fingerprint_set.c`
- `include/core/config.h`

## 编译

`make clean && make` 零错误零警告。
