#!/bin/bash
# B-12 闭环 R2 负载复跑: 形态 1:1 复刻 B-5 R2 rig (test_review_tmp_b5/r2_loop.sh), 仅 OUT 换至
# 独立 scratch, 不触碰原 r2_run1..12 命中证据。背景: full suite + hotpath + stress 死循环陪跑;
# 前台: filtered 300 轮 (PalRmdirErrnoDistinction, 同原形态)。B-12 观测面 = 背景 full_suite 日志。
set -u
cd /d/Project/Android_Project/LogX/logx || exit 9
UNIT=build-full/tests/unit/logx_unit_tests.exe
STRESS=build-full/tests/stress/logx_stress_tests.exe
HOTPATH=build-full/tests/unit/logx_hotpath_alloc_tests.exe
OUT=/d/Project/Android_Project/LogX/test_review_tmp_b12/r2
FILTER='Robustness.PalRmdirErrnoDistinction_NotEmptyVsMissing'
mkdir -p "$OUT"
echo "=== R2c begin $(date +%F_%T) cwd=$(pwd)" > "$OUT/session.log"
echo "--- 预清点: 残留目录 ---" >> "$OUT/session.log"
ls -d logx_test_* 2>/dev/null >> "$OUT/session.log" || echo "(无残留)" >> "$OUT/session.log"

k=1
while :; do
  "$UNIT"  > "$OUT/full_suite_$k.log" 2>&1
  "$HOTPATH" > "$OUT/hotpath_$k.log" 2>&1
  "$STRESS" > "$OUT/stress_$k.log" 2>&1
  k=$((k+1))
done &
LOAD_PID=$!

hits=0
for i in $(seq 1 300); do
  "$UNIT" --gtest_filter="$FILTER" > "$OUT/iter_$i.log" 2>&1
  rc=$?
  if [ $rc -ne 0 ]; then
    hits=$((hits+1))
    echo "HIT iter=$i rc=$rc $(date +%T.%N)" | tee -a "$OUT/HITS.txt" "$OUT/session.log"
    break
  fi
  rm -f "$OUT/iter_$i.log"
done

kill $LOAD_PID 2>/dev/null
wait $LOAD_PID 2>/dev/null
echo "--- 后清点: 残留目录 ---" >> "$OUT/session.log"
ls -d logx_test_* 2>/dev/null >> "$OUT/session.log" || echo "(无残留)" >> "$OUT/session.log"
echo "=== R2c end hits=$hits $(date +%F_%T)" >> "$OUT/session.log"
tail -3 "$OUT/session.log"
