#!/bin/bash
# B-12 闭环 R2 复跑计数批: 负载轮 $1..$2, 每轮负载实验 + 归档 r2c_runN + 三面判读
#   面 1 = full_suite 中 [  FAILED  ] 行数 (B-12 原命中面, 原判定门 2/12 轮)
#   面 2 = RetentionCleanup_ClockJumpForward20Years 定向失败 (B-12 目标用例, 含 [B12] 取证消息)
#   面 3 = 前台 filtered HIT (B-5 目标, 原同环境 3600+ 轮 0 命中, 连续确认)
set -u
BASE=/d/Project/Android_Project/LogX/test_review_tmp_b12
start=${1:-1}
end=${2:-12}
tot_fail=0
tot_b12=0
tot_hit=0
for run in $(seq "$start" "$end"); do
  ARC="$BASE/r2c_run$run"
  mkdir -p "$ARC"
  bash "$BASE/r2_loop.sh" > "$ARC/stdout.log" 2> "$ARC/stderr.log"
  cp "$BASE/r2/session.log" "$ARC/" 2>/dev/null
  cp "$BASE/r2"/full_suite_*.log "$ARC/" 2>/dev/null
  cp "$BASE/r2"/HITS.txt "$ARC/" 2>/dev/null
  n_fail=$(grep -h "\[  FAILED  \]" "$ARC"/full_suite_*.log 2>/dev/null | wc -l)
  n_b12=$(grep -h "RetentionCleanup_ClockJumpForward20Years" "$ARC"/full_suite_*.log 2>/dev/null | grep -c "FAILED\|\[B12\]")
  n_hit=$(grep -c "HIT" "$ARC/session.log" 2>/dev/null)
  tot_fail=$((tot_fail + n_fail))
  tot_b12=$((tot_b12 + n_b12))
  tot_hit=$((tot_hit + n_hit))
  echo "run$run: FAILED_tests=$n_fail b12_targeted=$n_b12 fg_hits=$n_hit"
done
echo "=== batch($start..$end) done $(date +%F_%T) totals: FAILED=$tot_fail b12_targeted=$tot_b12 fg_hits=$tot_hit"
