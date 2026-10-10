#!/bin/bash
# B-5 前置协议 R1: 零改动过滤循环 (预算 200 轮, 命中即停)
# 纪律: 命中即 break——保留该轮 iter 日志 + 工作目录残留的 logx_test_p03_* 现场不清理;
#       未命中轮日志即时删除防膨胀; 前后各清点一次残留目录.
set -u
cd /d/Project/Android_Project/LogX/logx || exit 9
EXE=build-full/tests/unit/logx_unit_tests.exe
OUT=/d/Project/Android_Project/LogX/test_review_tmp_b5/r1
FILTER='Robustness.PalRmdirErrnoDistinction_NotEmptyVsMissing'
mkdir -p "$OUT"
echo "=== R1 begin $(date +%F_%T) cwd=$(pwd)" > "$OUT/session.log"
echo "--- 预清点: 残留目录 ---" >> "$OUT/session.log"
ls -d logx_test_p03_rmdir_* 2>/dev/null >> "$OUT/session.log" || echo "(无残留)" >> "$OUT/session.log"
hits=0
for i in $(seq 1 200); do
  "$EXE" --gtest_filter="$FILTER" > "$OUT/iter_$i.log" 2>&1
  rc=$?
  if [ $rc -ne 0 ]; then
    hits=$((hits+1))
    echo "HIT iter=$i rc=$rc $(date +%T.%N)" | tee -a "$OUT/HITS.txt" "$OUT/session.log"
    break
  fi
  rm -f "$OUT/iter_$i.log"
done
echo "--- 后清点: 残留目录 ---" >> "$OUT/session.log"
ls -d logx_test_p03_rmdir_* 2>/dev/null >> "$OUT/session.log" || echo "(无残留)" >> "$OUT/session.log"
echo "=== R1 end hits=$hits $(date +%F_%T)" >> "$OUT/session.log"
tail -3 "$OUT/session.log"
