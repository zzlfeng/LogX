#!/bin/bash
# B-12 方案 A+B 并行负载实验: probe2 定性探针前台长跑 + 全量单测后台陪跑 (复刻命中轮环境)
# 三路计数: probe2 命中(rc=2, 现场保留) / full_suite FAILED(带 [B12] raw_lasterror) / segfault
set -u
BASE=/d/Project/Android_Project/LogX/test_review_tmp_b5
cd /d/Project/Android_Project/LogX/logx || exit 9
UNIT=build-full/tests/unit/logx_unit_tests.exe
OUT="$BASE/r2_probe2"
mkdir -p "$OUT"

powershell -NoProfile -Command "Get-MpComputerStatus | Select-Object RealTimeProtectionEnabled,AntivirusSignatureLastUpdated" > "$OUT/mp_status.txt" 2>&1
echo "=== probe2 load experiment begin $(date +%F_%T)" > "$OUT/session.log"
echo "--- Defender 基线 ---" >> "$OUT/session.log"
cat "$OUT/mp_status.txt" >> "$OUT/session.log"

k=1
while :; do
  "$UNIT" > "$OUT/full_suite_$k.log" 2>> "$OUT/load_stderr.log"
  k=$((k+1))
done &
LOAD_PID=$!

"$BASE/b5_probe2.exe" 60000 > "$OUT/probe2_stdout.log" 2>&1
PROBE_RC=$?
kill $LOAD_PID 2>/dev/null
wait $LOAD_PID 2>/dev/null

n_fail=$(grep -h "\[  FAILED  \]" "$OUT"/full_suite_*.log 2>/dev/null | wc -l)
n_seg=$(grep -ac "Segmentation fault" "$OUT/load_stderr.log" 2>/dev/null)
echo "probe2_rc=$PROBE_RC full_suite_FAILED=$n_fail full_suite_seg=$n_seg" | tee -a "$OUT/session.log"
grep -h "B12\|\[  FAILED  \]" "$OUT"/full_suite_*.log 2>/dev/null | head -6 | tee -a "$OUT/session.log"
tail -2 "$OUT/probe2_stdout.log" >> "$OUT/session.log"
powershell -NoProfile -Command "Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-Windows Defender/Operational'; Id=1006,1116,1117,5001,5007} -MaxEvents 10 | Select-Object TimeCreated,Id | Format-Table" > "$OUT/defender_events.txt" 2>&1
echo "=== end $(date +%F_%T)" >> "$OUT/session.log"
tail -4 "$OUT/session.log"
