#!/bin/bash
# B-5 R2 负载实验复跑计数器 (在已批准 R2 预算内扩展): 重复 3 轮负载实验,
# 给两个新异常签名计命中数:
#   签名 1 = full_suite 中 [  FAILED  ] 行数 (RetentionCleanup 家族: 文件可见性异常)
#   签名 2 = "Segmentation fault" 出现次数 (stderr 捕获)
# 每轮归档 session/full_suite/stderr 到 r2_runN/ 供事后取证; 目标用例 HITS 应保持 0.
set -u
BASE=/d/Project/Android_Project/LogX/test_review_tmp_b5
for run in 2 3 4; do
  ARC="$BASE/r2_run$run"
  mkdir -p "$ARC"
  bash "$BASE/r2_loop.sh" > "$ARC/stdout.log" 2> "$ARC/stderr.log"
  cp "$BASE/r2/session.log" "$ARC/" 2>/dev/null
  cp "$BASE/r2"/full_suite_*.log "$ARC/" 2>/dev/null
  cp "$BASE/r2/HITS.txt" "$ARC/" 2>/dev/null
  n_fail=$(grep -h "\[  FAILED  \]" "$ARC"/full_suite_*.log 2>/dev/null | wc -l)
  n_seg=$(grep -c "Segmentation fault" "$ARC/stderr.log" 2>/dev/null)
  n_hit=$(grep -c "HIT" "$ARC/session.log" 2>/dev/null)
  echo "run$run: FAILED_tests=$n_fail segfaults=$n_seg target_hits=$n_hit"
done
echo "=== repeat done $(date +%F_%T)"
