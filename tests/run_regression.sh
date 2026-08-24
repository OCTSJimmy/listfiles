#!/usr/bin/env bash
# run_regression.sh — listfiles 扫描完整性回归测试集（故障总结 v2.0.0 §四 规格 v2）
#
# 用法: tests/run_regression.sh [工作目录(默认 /tmp/lf_regression)]
#
# 覆盖：
#   1. 已知真值 fixture 基线：输出与 manifest diff=0（7万+条目单目录/GBK文件名/软链）
#   2. 并发一致性：workers=1/8/16 三方 sorted 输出一致
#      （规格原文"逐字节一致"——多 worker 下分片写出顺序本就不确定，
#        逐字节一致在小概率下也无法满足，故按 sorted 全文一致验收）
#   3. EACCES opendir 注入：exit!=0 + 熔断清单 DIR_ERROR(errno=13)
#   4. EACCES 条目 lstat 注入：exit!=0 + ENTRY_ERROR(errno=13)
#   5. LD_PRELOAD 假空 readdir（errno=0）：不加 --strict-nlink 无感通过（盲区存档证明）；
#      加 --strict-nlink 必须 exit!=0 + NLINK_MISMATCH
#   6. LD_PRELOAD 中途假 EOF：--strict-nlink 下 exit!=0 + NLINK_MISMATCH
#   7. dspill 派发兜底压力：低水位标桩构建（HIGH=64/LOW=16/BATCH=32），
#      3000 目录溢出跳推→回填，输出与 manifest diff=0，[Dspill] 统计出现，完结后 dspill 已删除
#   8. 并发删除豁免：扫描期间后台 rm churn/，exit=0、熔断清单无记录、输出 ⊆ manifest
#   9. 深路径（>4096）：exit!=0 + ENAMETOOLONG
#  10. 崩溃-续传一致性（P0-001/002/010）：KILL 首轮 + -c 续传，sort -u 逐行 == 基线，
#      续传 manifest status=Success 且 baseline_eligible=1
#  11. 二次崩溃 fpbin/dfpbin（P0-003）：KILL 首轮 + KILL 二轮 + 三轮续传到完成，
#      sort -u == 基线
#  12. 盲信扫描门禁（P0-008/011）：无 --reference-base → exit 2；合格基准 + 新空 -f
#      → exit 0 且输出 == 基线且新 manifest baseline_eligible=0；伪造基准
#      （baseline_eligible=0）→ exit 2
#  13. 不完整运行不可作基准（P0-008）：EACCES 子目录 → exit 1 + status=Incomplete
#      + baseline_eligible=0 + spbin_count>=1；以它为基准盲信 → exit 2
#  14. kill -9 Worker 中段（v15.6.1 P0-101/102/103/107）：spare 池补位，
#      输出 == 基线，stderr 有 DEAD/预备役日志，无 INVARIANT（账目不为负）
#  15. Worker 全灭 + spare 耗尽（v15.6.1 P0-105/108）：有效进展看门狗
#      --stall-timeout=5 触发，exit=2，有 [Watchdog]/耗尽日志，无静默挂起
#
# 注意（用例 10/11）：kill 时序在慢机器上可能偏早/偏晚——断言只依赖"最终输出
# == 基线"，不依赖崩溃具体位置；若首轮在 kill 前已跑完（小树上可能），该用例
# 仍应 PASS（结果等价）。
#
# 注意：用例 5 的"无感通过"是已知原理性盲区——纯文件目录的无 errno 假空/截断
# 客户端无法检测（无子目录可供 nlink oracle 比对），只能靠跨运行对账。
set -u
cd "$(dirname "$0")/.."
PROJECT_ROOT=$PWD
LF=$PROJECT_ROOT/bin/listfiles
WORK=${1:-/tmp/lf_regression}
FIX=$WORK/fixture
PASS=0; FAIL=0; FAILED_CASES=""

say()  { printf '[regression] %s\n' "$*"; }
ok()   { PASS=$((PASS+1)); say "PASS: $*"; }
bad()  { FAIL=$((FAIL+1)); FAILED_CASES="$FAILED_CASES $1"; say "FAIL: $*"; }

# 收集输出并排序（字节序）
collect() { cat "$1"/*.txt 2>/dev/null | LC_ALL=C sort; }

# 运行一次扫描: $1=输出目录 $2=进度前缀 $3..=附加参数
run_lf() {
    local out=$1 prog=$2; shift 2
    rm -rf "$out"; mkdir -p "$out"
    "$LF" -p "$FIX" -F "%p %s" -D -O "$out" -f "$prog" --max-slice 50000 \
          --yes --worker-count 8 --batch-size 1024 "$@"
}

breaker_has() { # $1=进度前缀 $2=模式
    [ -f "$1.circuit_breaker" ] && grep -q "$2" "$1.circuit_breaker"
}
breaker_empty() { # 无文件或仅有表头注释行
    [ ! -f "$1.circuit_breaker" ] || ! grep -qv '^#' "$1.circuit_breaker"
}

say "工作目录: $WORK"
# 每轮必须使用全新进度前缀/输出目录——.config status=Success 会自动开启续传
# （无需 -c），上一轮残留的进度会让本轮扫描被 completed_set 剪成空扫描。
if [ -d "$WORK" ] && [ ! -f "$WORK/.lf_regression_workdir" ] && [ -n "$(ls -A "$WORK")" ]; then
    say "错误: $WORK 非空且非本脚本工作目录（缺少 .lf_regression_workdir 标记），拒绝清空"
    exit 2
fi
rm -rf "$WORK"
mkdir -p "$WORK"
touch "$WORK/.lf_regression_workdir"

[ -x "$LF" ] || { say "bin/listfiles 不存在，先 make"; exit 2; }

# ---------- 准备 fixture ----------
say "生成 main fixture（含 7 万条目大目录）..."
python3 tests/gen_fixture.py "$FIX" --big-count 70000 --manifest "$WORK/manifest.txt"
LC_ALL=C sort "$WORK/manifest.txt" > "$WORK/manifest.sorted"

# LD_PRELOAD shim
gcc -shared -fPIC -O2 -o "$WORK/inject_readdir.so" "$PROJECT_ROOT/tests/inject_readdir.c" -ldl \
    || { say "shim 编译失败"; exit 2; }

# ================================================================
say "== 用例 1: 已知真值基线 diff=0 =="
run_lf "$WORK/out1" "$WORK/prog1" -M
rc=$?
collect "$WORK/out1" > "$WORK/out1.sorted"
if [ $rc -eq 0 ] && cmp -s "$WORK/out1.sorted" "$WORK/manifest.sorted"; then
    ok "用例1 基线一致（$(wc -l < "$WORK/out1.sorted") 行）"
else
    bad "用例1" "基线 diff 非零或退出码=$rc"
    diff "$WORK/out1.sorted" "$WORK/manifest.sorted" | head -5
fi

# ================================================================
say "== 用例 2: workers=1/8/16 并发一致性 =="
run_lf "$WORK/out_w1"  "$WORK/prog_w1"  -M --worker-count 1
run_lf "$WORK/out_w8"  "$WORK/prog_w8"  -M --worker-count 8
run_lf "$WORK/out_w16" "$WORK/prog_w16" -M --worker-count 16
collect "$WORK/out_w1"  > "$WORK/w1.sorted"
collect "$WORK/out_w8"  > "$WORK/w8.sorted"
collect "$WORK/out_w16" > "$WORK/w16.sorted"
if cmp -s "$WORK/w1.sorted" "$WORK/w8.sorted" && cmp -s "$WORK/w8.sorted" "$WORK/w16.sorted"; then
    ok "用例2 三方 sorted 输出一致"
else
    bad "用例2" "workers 1/8/16 输出不一致"
fi

# ================================================================
if [ "$(id -u)" -eq 0 ]; then
    say "== 用例 3/4: root 运行，EACCES 注入无效，跳过 =="
else
    say "== 用例 3: EACCES opendir（chmod 000）=="
    chmod 000 "$FIX/deepnest/a2"
    run_lf "$WORK/out3" "$WORK/prog3" -M; rc=$?
    chmod 755 "$FIX/deepnest/a2"
    if [ $rc -ne 0 ] && breaker_has "$WORK/prog3" 'DIR_ERROR(errno=13)'; then
        ok "用例3 DIR_ERROR(errno=13) 已记录，exit=$rc"
    else
        bad "用例3" "exit=$rc，熔断清单: $(cat "$WORK/prog3.circuit_breaker" 2>/dev/null | tail -2)"
    fi

    say "== 用例 4: EACCES 条目 lstat（chmod 444）=="
    chmod 444 "$FIX/deepnest/a/b/c"
    run_lf "$WORK/out4" "$WORK/prog4" -M; rc=$?
    chmod 755 "$FIX/deepnest/a/b/c"
    if [ $rc -ne 0 ] && breaker_has "$WORK/prog4" 'ENTRY_ERROR(errno=13)'; then
        ok "用例4 ENTRY_ERROR(errno=13) 已记录，exit=$rc"
    else
        bad "用例4" "exit=$rc，熔断清单: $(cat "$WORK/prog4.circuit_breaker" 2>/dev/null | tail -2)"
    fi
fi

# ================================================================
say "== 用例 5a: 假空 readdir 注入，无 --strict-nlink（盲区存档）=="
LF_TARGET_SUBSTR=inject_target LF_FAKE_EMPTY_AFTER=0 LD_PRELOAD="$WORK/inject_readdir.so" \
    run_lf "$WORK/out5a" "$WORK/prog5a" -M; rc=$?
if [ $rc -eq 0 ] && breaker_empty "$WORK/prog5a"; then
    ok "用例5a 无 strict-nlink 时无感通过（已知盲区，符合预期存档）"
else
    bad "用例5a" "exit=$rc（预期 0 = 盲区证明）"
fi

say "== 用例 5b: 假空 readdir 注入 + --strict-nlink（必须捕获）=="
LF_TARGET_SUBSTR=inject_target LF_FAKE_EMPTY_AFTER=0 LD_PRELOAD="$WORK/inject_readdir.so" \
    run_lf "$WORK/out5b" "$WORK/prog5b" -M --strict-nlink; rc=$?
if [ $rc -ne 0 ] && breaker_has "$WORK/prog5b" 'NLINK_MISMATCH'; then
    ok "用例5b NLINK_MISMATCH 已捕获，exit=$rc"
else
    bad "用例5b" "exit=$rc，熔断清单: $(cat "$WORK/prog5b.circuit_breaker" 2>/dev/null | tail -2)"
fi

# ================================================================
say "== 用例 6: 中途假 EOF（第 5 条后）+ --strict-nlink（必须捕获）=="
LF_TARGET_SUBSTR=inject_target LF_FAKE_EMPTY_AFTER=5 LD_PRELOAD="$WORK/inject_readdir.so" \
    run_lf "$WORK/out6" "$WORK/prog6" -M --strict-nlink; rc=$?
if [ $rc -ne 0 ] && breaker_has "$WORK/prog6" 'NLINK_MISMATCH'; then
    ok "用例6 中途假 EOF 已捕获，exit=$rc"
else
    bad "用例6" "exit=$rc，熔断清单: $(cat "$WORK/prog6.circuit_breaker" 2>/dev/null | tail -2)"
fi

# ================================================================
say "== 用例 7: dspill 兜底压力（低水位标桩构建）=="
INST=$WORK/inst_build
rm -rf "$INST"; mkdir -p "$INST"
cp -r "$PROJECT_ROOT/src" "$PROJECT_ROOT/include" "$PROJECT_ROOT/lib" "$PROJECT_ROOT/Makefile" "$INST/"
sed -i \
    -e 's/define DISPATCH_QUEUE_HIGH_WATER.*/define DISPATCH_QUEUE_HIGH_WATER    8/' \
    -e 's/define DISPATCH_QUEUE_LOW_WATER.*/define DISPATCH_QUEUE_LOW_WATER      4/' \
    -e 's/define DISPATCH_QUEUE_LOAD_BATCH.*/define DISPATCH_QUEUE_LOAD_BATCH     16/' \
    "$INST/include/core/config.h"
if make -C "$INST" -j"$(nproc)" > "$WORK/inst_make.log" 2>&1; then
    DSP=$WORK/dspill_case
    rm -rf "$DSP"; mkdir -p "$DSP/tree"
    for i in $(seq 0 1999); do
        d=$(printf '%s/tree/d%04d' "$DSP" "$i"); mkdir "$d"
        for j in 0 1 2 3 4 5 6 7 8 9; do echo x > "$d/f$j"; done
    done
    find "$DSP/tree" -mindepth 1 -printf '%p %s\n' | LC_ALL=C sort > "$DSP/manifest.sorted"
    mkdir -p "$DSP/out"
    "$INST/bin/listfiles" -p "$DSP/tree" -F "%p %s" -D -O "$DSP/out" -f "$DSP/prog" \
        --max-slice 50000 --yes --worker-count 2 --batch-size 32 -v > "$DSP/run.log" 2>&1
    rc=$?
    collect "$DSP/out" > "$DSP/out.sorted"
    if [ $rc -eq 0 ] \
        && cmp -s "$DSP/out.sorted" "$DSP/manifest.sorted" \
        && grep -q '\[Dspill\] HIGH_WATER' "$DSP/run.log" \
        && [ ! -f "$DSP/prog.dspill" ]; then
        ok "用例7 dspill 溢出→回填完整（$(grep -o 'HIGH_WATER 跳推目录 [0-9]*' "$DSP/run.log" | tail -1)）"
    else
        bad "用例7" "exit=$rc；diff: $(diff "$DSP/out.sorted" "$DSP/manifest.sorted" | wc -l) 行；log: $(grep -c Dspill "$DSP/run.log") 处"
    fi
else
    bad "用例7" "标桩构建失败，见 $WORK/inst_make.log"
fi

# ================================================================
say "== 用例 8: 并发删除豁免（churn/ 扫描期间后台 rm）=="
(
    for i in $(seq 0 1999); do
        rm -f "$(printf '%s/churn/del_%04d.txt' "$FIX" "$i")"
        sleep 0.002
    done
) &
DELETER=$!
run_lf "$WORK/out8" "$WORK/prog8" -M; rc=$?
wait $DELETER 2>/dev/null
collect "$WORK/out8" | cut -d' ' -f1 | LC_ALL=C sort -u > "$WORK/out8.paths"
cut -d' ' -f1 "$WORK/manifest.sorted" > "$WORK/manifest.paths"
# 输出路径必须 ⊆ manifest（删除的只是缺席，不得出现 manifest 之外的路径）
extra=$(LC_ALL=C comm -23 "$WORK/out8.paths" "$WORK/manifest.paths" | wc -l)
if [ $rc -eq 0 ] && breaker_empty "$WORK/prog8" && [ "$extra" -eq 0 ]; then
    ok "用例8 并发删除豁免（exit=0，清单干净，输出 ⊆ manifest）"
else
    bad "用例8" "exit=$rc extra=$extra breaker: $(tail -2 "$WORK/prog8.circuit_breaker" 2>/dev/null)"
fi

# ================================================================
say "== 用例 9: 深路径（>4096）ENAMETOOLONG =="
python3 tests/gen_fixture.py "$WORK/deepfix" --mode deep
mkdir -p "$WORK/out9"
"$LF" -p "$WORK/deepfix" -F "%p %s" -D -O "$WORK/out9" -f "$WORK/prog9" \
      --max-slice 50000 --yes --worker-count 2 -M; rc=$?
if [ $rc -ne 0 ] && breaker_has "$WORK/prog9" 'ENTRY_ERROR(errno=36)'; then
    ok "用例9 ENAMETOOLONG(errno=36) 已记录，exit=$rc"
else
    bad "用例9" "exit=$rc，熔断清单: $(cat "$WORK/prog9.circuit_breaker" 2>/dev/null | tail -2)"
fi

# ================================================================
# 用例 10/11 共用树：2000 目录 × 2 文件（dir%05d/{f.txt, sub/g.txt}）
say "生成崩溃-续传 fixture（2000 目录 × 2 文件）..."
CTREE=$WORK/crashtree
python3 - "$CTREE" <<'EOF'
import os, sys
root = sys.argv[1]
for i in range(2000):
    d = os.path.join(root, "dir%05d" % i)
    os.makedirs(os.path.join(d, "sub"))
    with open(os.path.join(d, "f.txt"), "w") as fp:
        fp.write("x\n")
    with open(os.path.join(d, "sub", "g.txt"), "w") as fp:
        fp.write("y\n")
EOF

# 崩溃轮运行：$1=输出目录（不清理，跨轮累积）$2=进度前缀 $3=timeout秒(0=不限时)
run_crash() {
    local out=$1 prog=$2 limit=$3
    mkdir -p "$out"
    if [ "$limit" -gt 0 ]; then
        timeout -s KILL "$limit" "$LF" -p "$CTREE" -F "%p %s" -D -O "$out" -f "$prog" \
            --max-slice 50000 --yes --worker-count 8 --batch-size 1024 -c >/dev/null 2>&1
    else
        "$LF" -p "$CTREE" -F "%p %s" -D -O "$out" -f "$prog" \
            --max-slice 50000 --yes --worker-count 8 --batch-size 1024 -c >/dev/null 2>&1
    fi
    # kill 轮退出码不语义化（124/137/0 均可能），调用方只在最终轮取 $?
}

# ================================================================
say "== 用例 10: 崩溃-续传一致性（P0-001/002/010）=="
# 干净全量基线（prog10 同时是用例 12 的盲信基准前缀，必须保持 baseline_eligible=1）
# 注意：基线必须与崩溃轮扫同一棵树（CTREE），不能用 run_lf（它扫的是 $FIX）
run_crash "$WORK/out10" "$WORK/prog10" 0; rc=$?
collect "$WORK/out10" | LC_ALL=C sort -u > "$WORK/case10_baseline.sorted"
if [ $rc -ne 0 ] || ! grep -q '^baseline_eligible=1$' "$WORK/prog10.manifest"; then
    bad "用例10" "基线运行失败（exit=$rc）或基线 manifest 不合格，跳过后续断言"
else
    # 首轮 KILL（-c 模式）+ 续传到完成；首轮若已跑完，续传等价空跑，断言仍成立
    run_crash "$WORK/out10c" "$WORK/prog10c" 3
    run_crash "$WORK/out10c" "$WORK/prog10c" 0
    rc=$?
    collect "$WORK/out10c" | LC_ALL=C sort -u > "$WORK/out10c.sorted"
    if [ $rc -eq 0 ] && cmp -s "$WORK/out10c.sorted" "$WORK/case10_baseline.sorted" \
        && grep -q '^status=Success$' "$WORK/prog10c.manifest" \
        && grep -q '^baseline_eligible=1$' "$WORK/prog10c.manifest"; then
        ok "用例10 崩溃-续传输出与基线逐行一致，manifest Success/eligible=1"
    else
        bad "用例10" "exit=$rc；diff: $(diff "$WORK/out10c.sorted" "$WORK/case10_baseline.sorted" | wc -l) 行；manifest: $(grep -E '^(status|baseline_eligible)=' "$WORK/prog10c.manifest" 2>/dev/null | tr '\n' ' ')"
    fi
fi

# ================================================================
say "== 用例 11: 二次崩溃 fpbin/dfpbin（P0-003）=="
run_crash "$WORK/out11" "$WORK/prog11" 3
run_crash "$WORK/out11" "$WORK/prog11" 2
run_crash "$WORK/out11" "$WORK/prog11" 0
rc=$?
collect "$WORK/out11" | LC_ALL=C sort -u > "$WORK/out11.sorted"
if [ $rc -eq 0 ] && cmp -s "$WORK/out11.sorted" "$WORK/case10_baseline.sorted"; then
    ok "用例11 二次崩溃后三轮续传输出与基线逐行一致"
else
    bad "用例11" "exit=$rc；diff: $(diff "$WORK/out11.sorted" "$WORK/case10_baseline.sorted" | wc -l) 行"
fi

# ================================================================
say "== 用例 12: 盲信扫描门禁（P0-008/011）=="
# (a) 不带 --reference-base 盲信 → exit 2
run_lf "$WORK/out12a" "$WORK/prog12a" -c --skip-interval=3600 >/dev/null 2>&1; rc=$?
if [ $rc -eq 2 ]; then
    ok "用例12a 无 --reference-base 盲信被拒（exit=2）"
else
    bad "用例12a" "exit=$rc（预期 2）"
fi

# (b) 合格基准（用例10 prog10）+ 新空 -f → exit 0，输出 == 基线，新 manifest eligible=0
# 注意：盲信按纯路径命中基准，必须扫与基准同一棵树（CTREE），不能用 run_lf（$FIX）
rm -rf "$WORK/out12b"; mkdir -p "$WORK/out12b"
"$LF" -p "$CTREE" -F "%p %s" -D -O "$WORK/out12b" -f "$WORK/prog12b" \
    --max-slice 50000 --yes --worker-count 8 --batch-size 1024 \
    -c --skip-interval=3600 --reference-base="$WORK/prog10" >/dev/null 2>&1; rc=$?
collect "$WORK/out12b" | LC_ALL=C sort -u > "$WORK/out12b.sorted"
if [ $rc -eq 0 ] && cmp -s "$WORK/out12b.sorted" "$WORK/case10_baseline.sorted" \
    && grep -q '^baseline_eligible=0$' "$WORK/prog12b.manifest"; then
    ok "用例12b 盲信输出与基线一致，新 manifest baseline_eligible=0（不可链式作基准）"
else
    bad "用例12b" "exit=$rc；diff: $(diff "$WORK/out12b.sorted" "$WORK/case10_baseline.sorted" | wc -l) 行；manifest: $(grep -E '^(status|baseline_eligible)=' "$WORK/prog12b.manifest" 2>/dev/null | tr '\n' ' ')"
fi

# (c) 伪造基准 manifest（baseline_eligible=0）→ exit 2
sed 's/^baseline_eligible=1$/baseline_eligible=0/' "$WORK/prog10.manifest" \
    > "$WORK/prog10fake.manifest"
run_lf "$WORK/out12c" "$WORK/prog12c" -c --skip-interval=3600 \
    --reference-base="$WORK/prog10fake" >/dev/null 2>&1; rc=$?
if [ $rc -eq 2 ]; then
    ok "用例12c 伪造基准（baseline_eligible=0）被拒（exit=2）"
else
    bad "用例12c" "exit=$rc（预期 2）"
fi

# ================================================================
if [ "$(id -u)" -eq 0 ]; then
    say "== 用例 13: root 运行，EACCES 注入无效，跳过 =="
else
    say "== 用例 13: 不完整运行不可作基准（P0-008）=="
    chmod 000 "$CTREE/dir00001"
    run_crash "$WORK/out13" "$WORK/prog13" 0; rc=$?   # 与 EACCES 目录同树（CTREE）
    chmod 755 "$CTREE/dir00001"
    M13="$WORK/prog13.manifest"
    SPBIN_N=$(sed -n 's/^spbin_count=//p' "$M13" 2>/dev/null); SPBIN_N=${SPBIN_N:-0}
    if [ $rc -eq 1 ] && grep -q '^status=Incomplete$' "$M13" \
        && grep -q '^baseline_eligible=0$' "$M13" && [ "$SPBIN_N" -ge 1 ]; then
        ok "用例13 EACCES 全量 exit=1，manifest Incomplete/eligible=0，spbin_count=$SPBIN_N"
    else
        bad "用例13" "exit=$rc（预期 1）；manifest: $(grep -E '^(status|baseline_eligible|spbin_count)=' "$M13" 2>/dev/null | tr '\n' ' ')"
    fi
    # 以不完整运行的进度为基准盲信 → exit 2
    run_lf "$WORK/out13b" "$WORK/prog13b" -c --skip-interval=3600 \
        --reference-base="$WORK/prog13" >/dev/null 2>&1; rc=$?
    if [ $rc -eq 2 ]; then
        ok "用例13b 不完整基准盲信被拒（exit=2）"
    else
        bad "用例13b" "exit=$rc（预期 2）"
    fi
fi

# ================================================================
say "== 用例 14: kill -9 Worker 中段（v15.6.1 P0-101/102/103/107 spare 补位）=="
"$LF" -p "$CTREE" -F "%p %s" -D -O "$WORK/out14" -f "$WORK/prog14" \
    --max-slice 50000 --yes --worker-count 8 --batch-size 256 -M \
    > /dev/null 2> "$WORK/case14.log" &
LF_PID=$!
sleep 1
KILLED=0
for wp in $(pgrep -P $LF_PID); do
    kill -9 $wp 2>/dev/null && KILLED=$((KILLED+1))
    [ $KILLED -ge 3 ] && break
done
wait $LF_PID; rc=$?
collect "$WORK/out14" | LC_ALL=C sort -u > "$WORK/out14.sorted"
if [ $rc -eq 0 ] && [ "$KILLED" -ge 1 ] \
    && cmp -s "$WORK/out14.sorted" "$WORK/case10_baseline.sorted" \
    && grep -q 'DEAD' "$WORK/case14.log" && grep -q '预备役' "$WORK/case14.log" \
    && ! grep -q 'INVARIANT' "$WORK/case14.log"; then
    ok "用例14 kill -9 $KILLED 个 Worker 后 spare 补位，输出与基线一致"
else
    bad "用例14" "exit=$rc killed=$KILLED；diff: $(diff "$WORK/out14.sorted" "$WORK/case10_baseline.sorted" | wc -l) 行；log: $(tail -3 "$WORK/case14.log" | tr '\n' ' ')"
fi

# ================================================================
say "== 用例 15: Worker 全灭 + spare 耗尽 → 看门狗（v15.6.1 P0-105/108）=="
"$LF" -p "$CTREE" -F "%p %s" -D -O "$WORK/out15" -f "$WORK/prog15" \
    --max-slice 50000 --yes --worker-count 8 --batch-size 256 -M \
    --stall-timeout 5 > /dev/null 2> "$WORK/case15.log" &
LF_PID=$!
sleep 0.5
# 反复杀光全部子进程（初始 8 + 预备役 8），直至 spare 耗尽再无子进程
for round in $(seq 1 60); do
    children=$(pgrep -P $LF_PID)
    [ -z "$children" ] && break
    for wp in $children; do kill -9 $wp 2>/dev/null; done
    sleep 0.2
done
wait $LF_PID; rc=$?
if [ $rc -eq 2 ] && grep -q 'Watchdog' "$WORK/case15.log" \
    && grep -q '预备役 Worker 已耗尽' "$WORK/case15.log" \
    && ! grep -q 'INVARIANT' "$WORK/case15.log"; then
    ok "用例15 全灭+spare 耗尽后看门狗 exit=2（响亮终止，无负账目、无静默挂起）"
else
    bad "用例15" "exit=$rc（预期 2）；log 尾部: $(tail -3 "$WORK/case15.log" | tr '\n' ' ')"
fi

# ================================================================
say "=================================================="
say "结果: PASS=$PASS FAIL=$FAIL${FAILED_CASES:+  失败用例:$FAILED_CASES}"
say "产物保留于: $WORK"
[ $FAIL -eq 0 ]
