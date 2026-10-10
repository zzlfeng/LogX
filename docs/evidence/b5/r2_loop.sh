#!/bin/bash
# B-5 前置协议 R2: 并行负载复现 (陪跑负载: 全量单测 + stress + hotpath 循环跑;
# 前台 filtered 300 轮). 复刻 2026-09-13 全量矩阵并行场景的磁盘/AV/索引器负载面.
# 负载进程以前台循环结束为停; kill 留下的残留测试目录由 session.log 清点记录.
set -u
cd /d/Project/Android_Project/LogX/logx || exit 9
UNIT=build-full/tests/unit/logx_unit_tests.exe
STRESS=build-full/tests/stress/logx_stress_tests.exe
HOTPATH=build-full/tests/unit/logx_hotpath_alloc_tests.exe
OUT=/d/Project/Android_Project/LogX/test_review_tmp_b5/r2
FILTER='Robustness.PalRmdirErrnoDistinction_NotEmptyVsMissing'
mkdir -p "$OUT"
echo "=== R2 begin $(date +%F_%T) cwd=$(pwd)" > "$OUT/session.log"
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
echo "=== R2 end hits=$hits $(date +%F_%T)" >> "$OUT/session.log"
tail -3 "$OUT/session.log"
