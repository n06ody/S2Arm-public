#!/bin/bash
# 门禁覆盖：tests/visual/crash_keep.sh tests/visual/widen_gate.sh
# 回放器崩溃留证单测（tests/visual/crash_keep.sh，不稳定项 15）：bash tests/crash_keep_test.sh
# 期望按定义写死：只收「开局之后」且名字以进程名开头的崩溃报告；日志一并拷；没报告时打「等了 N 秒没等到」；
#   派 gate-host（GATE_OUT 非空）落到 <仓库>/$GATE_OUT/crash/<标签>/（随结果目录拉回）；本机落运行区 evidence/gate_crash/。不碰真系统目录（CRASH_DIAG_DIRS 换假目录）
set -u
H=$(cd "$(dirname "$0")/.." && pwd); source "$H/tests/visual/crash_keep.sh"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT; bad=0
ck() { if [ "$2" = "$3" ]; then echo "✓ $1：$3"; else echo "✗ $1：期望 $2 → 真结果 $3"; bad=$((bad + 1)); fi; }
D=$W/diag; mkdir -p "$D"; t0=$(($(date +%s) - 5))
echo old > "$D/galplay-2026-10-26-195429.ips"; touch -t 202610261954 "$D/galplay-2026-10-26-195429.ips"   # 开局前：不收
echo new > "$D/galplay-2026-11-05-071500.ips"                                                          # 开局后、同名：收
echo other > "$D/dotnet-2026-11-05-071500.ips"                                                         # 开局后、别的进程：不收
echo log > "$W/w.log"
out=$(CRASH_DIAG_DIRS=$D CRASH_KEEP_DIR=$W/keep CRASH_KEEP_WAIT=0 crash_keep widen_witcher3_W "$t0" galplay "$W/w.log" "$W/没有的文件")
ck "收开局后的同名报告与日志" "galplay-2026-11-05-071500.ips w.log" "$(ls "$W/keep/widen_witcher3_W" | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//')"
ck "红行下打出证据路径与份数" 1 "$(echo "$out" | grep -c "崩溃证据：$W/keep/widen_witcher3_W（崩溃报告 1 份）")"
out=$(CRASH_DIAG_DIRS=$W/空:$W/也空 CRASH_KEEP_DIR=$W/keep2 CRASH_KEEP_WAIT=1 crash_keep t2 "$t0" galplay "$W/w.log")
ck "没报告：等满再报没等到、点名查过的目录" 1 "$(echo "$out" | grep -c "崩溃报告 0 份：等了 1 秒没等到 galplay\*.ips，查过 $W/空:$W/也空")"
G=$W/repo; mkdir -p "$G"; git -C "$G" init -q
out=$( cd "$G" && CRASH_DIAG_DIRS=$D GATE_OUT=build/gate_out/x CRASH_KEEP_WAIT=0 crash_keep t3 "$t0" galplay "$W/w.log" )
ck "派 gate-host：落到随结果拉回的门禁输出目录" "galplay-2026-11-05-071500.ips w.log" "$(ls "$G/build/gate_out/x/crash/t3" 2> /dev/null | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//')"
out=$(CRASH_DIAG_DIRS=$D S2A_RUN_ROOT=$W/run CRASH_KEEP_WAIT=0 GATE_OUT= crash_keep t4 "$t0" galplay "$W/w.log")
ck "本机：落运行区 evidence/gate_crash/<时刻>_<标签>" 1 "$(ls -d "$W"/run/s2arm/evidence/gate_crash/*_t4 2> /dev/null | wc -l | tr -d ' ')"
echo "$([ $bad = 0 ] && echo 🟢 || echo 🔴) crash_keep 单测：5 项，不符 $bad"
exit $((bad > 0))
