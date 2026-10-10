#!/bin/bash
# B-5 R2 扩展计数批: 轮次范围 $1..$2, 每轮负载实验 + 归档 + 双签名计数
#   签名 1 = full_suite 中 [  FAILED  ] 行数
#   签名 2 = stderr 中 "Segmentation fault" 次数
# 目标用例 target_hits 应保持 0 (B-5 主目标另行判定).
set -u
BASE=/d/Project/Android_Project/LogX/test_review_tmp_b5
start=${1:-5}
end=${2:-12}
tot_fail=0
tot_seg=0
for run in $(seq "$start" "$end"); do
  ARC="$BASE/r2_run$run"
  mkdir -p "$ARC"
  bash "$BASE/r2_loop.sh" > "$ARC/stdout.log" 2> "$ARC/stderr.log"
  cp "$BASE/r2/session.log" "$ARC/" 2>/dev/null
  cp "$BASE/r2"/full_suite_*.log "$ARC/" 2>/dev/null
  cp "$BASE/r2/HITS.txt" "$ARC/" 2>/dev/null
  n_fail=$(grep -h "\[  FAILED  \]" "$ARC"/full_suite_*.log 2>/dev/null | wc -l)
  n_seg=$(grep -c "Segmentation fault" "$ARC/stderr.log" 2>/dev/null)
  n_hit=$(grep -c "HIT" "$ARC/session.log" 2>/dev/null)
  tot_fail=$((tot_fail + n_fail))
  tot_seg=$((tot_seg + n_seg))
  echo "run$run: FAILED_tests=$n_fail segfaults=$n_seg target_hits=$n_hit"
done
echo "=== batch($start..$end) done $(date +%F_%T) totals: FAILED=$tot_fail seg=$tot_seg"
