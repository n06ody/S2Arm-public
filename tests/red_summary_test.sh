#!/bin/bash
# 门禁覆盖：tools/gate.sh android/tools/gate_step.sh android/gate.sh graphics/gate.sh
# 红步摘要只取失败子项自己的结论行、反证段不算（11-23 维护者：反证输出混进红行摘要，误导两轮定位）。期望手写：
#   ① 243e72aa 那局的形状（android/gate.sh 聚合钩子：provider 缺导出场景的【红】、save_atomic 变异的 ✗、caps_shape 策略反证的 FAIL、
#      pipe_pend 负对照的 FAIL 都出自通过的子项；唯一失败子项是 demand 的「stop stays bounded near 500ms」）→ 红步摘要的首行与明细只剩 demand 那条，
#      上面四类一条都不出现；② 不经子项聚合的单测：【负对照开始】…【负对照结束】之间的 FAIL 不算，段外的真 FAIL 照算；
#   ③ 反证：layer_edge ⑥ 那条真红日志（没有标记、没有失败子项摘要）重算仍是红、首行仍是「不通 ⑥ 真仓库」
#   ⑤ graphics/gate.sh 红项证据：子项日志里带缩进的「  ✗ ⑦ …」进「判红行」段（11-23：dmsh_reuse 1 例不符，末 30 行全是 ✓、不符条目没留下）；
#      反证：全 ✓ 的日志不出「判红行」段
set -uo pipefail
H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d "${TMPDIR:-/tmp}/t_red_sum.XXXXXX"); trap 'rm -rf "$T"' EXIT; bad=0
ok() { echo "通 $1"; }; no() { echo "不通 $1"; bad=1; }
eval "$(sed -n '/^# >>> show/,/^# <<< show/p' "$H/tools/gate.sh")"
gl_put() { :; }; CUR=x; L=$T/L; mkdir -p "$L"
sum() { printf '%s\n' "$2" > "$L/s.rc"; cp "$1" "$L/s.log"; show "" s "钩子 测试" 1800; }

# ① 聚合钩子：经真实 gate_step.sh 跑出日志
cat > "$T/agg.sh" <<'SH'
set -euo pipefail
source "$1"
s2a_gate_init
s2a_gate_step bash -c 'echo "【红】原生层没有 s2a_clock_unseen_ns / s2a_clock_register_catchup（接口 v2.14）"; echo "PASS 11 isolated processes"'
s2a_gate_step bash -c 'echo "✗ save_atomic_poll：2/3 例不符"; echo "✓ save_atomic_poll 负对照：缺出口答 0 → 缺出口用例红"'
s2a_gate_step bash -c 'echo "FAIL: [minimal-1.1] tex.fmt_list 与现状选择不同：期望 0x0 → 实测 0x1"; echo "PASS: 策略负对照 红在 tex.fmt_list"'
s2a_gate_step bash -c 'echo "FAIL: second enter follows (line 33)"; echo "RED (2 failures)"; echo "pend: GREEN + negative control RED (ok)"'
rc=0; s2a_gate_step bash -c 'echo "GREEN (0 failures, builds=5)"; echo "FAIL: stop stays bounded near 500ms while build is held (line 277)"; echo "RED (1 failures, builds=5)"; exit 1' || rc=$?
s2a_gate_finish "$rc"
SH
bash "$T/agg.sh" "$H/android/tools/gate_step.sh" > "$T/agg.log" 2>&1
o=$(sum "$T/agg.log" "1 1562")
head1=$(head -1 <<< "$o")
[[ "$head1" == *"stop stays bounded near 500ms"* ]] && ok "① 首行是失败子项 demand 的红（实得：${head1:0:120}）" || no "① 首行应是 demand 计时红，实得：${head1:0:160}"
for k in s2a_clock_unseen_ns save_atomic_poll tex.fmt_list "second enter follows"; do
  grep -qF "$k" <<< "$o" && no "① 摘要里不该有通过子项的「${k}」" || ok "① 摘要里没有通过子项的「${k}」"; done

# ② 单测日志：标记内不算、标记外照算
printf '%s\n' '【负对照开始】' 'FAIL: 变异后该红的用例' '【负对照结束】' 'FAIL: 真坏的用例' 'RED (1 failures)' > "$T/one.log"
o=$(sum "$T/one.log" "1 3")
{ grep -q "FAIL: 真坏的用例" <<< "$o" && ! grep -q "变异后该红" <<< "$o"; } && ok "② 标记内的反证红不进摘要，段外真红照进" || no "② 实得：$o"

# ③ 反证：layer_edge ⑥ 真红日志原样重算仍红
printf '%s\n' '通 ① 越界边报出' '不通 ⑥ 真仓库：未登记 0、豁免过时 0（用时 0.3 秒，只作读数）（分层边：越界边 133 处；未登记 1、豁免过时 0）' '通 ⑦ 路由' 'layer_edge 单测有 1 项不通' > "$T/le.log"
o=$(sum "$T/le.log" "1 1")
{ [[ "$(head -1 <<< "$o")" == *"不通 ⑥ 真仓库"* ]] && [[ "$o" == 🔴* ]]; } && ok "③ 反证：layer_edge ⑥ 真红重算仍红、首行不变" || no "③ 实得：$o"
# ④ 真入口接上了：android/gate.sh 跑整套前 s2a_gate_init、收尾 s2a_gate_finish（11-23：原先 exec 直跑整套，子项隔离从没生效）
g=$(grep -n 's2a_gate_init\|s2a_gate_finish\|bash android/test.sh' "$H/android/gate.sh")
{ grep -q 's2a_gate_init' <<< "$g" && grep -q "trap 's2a_gate_finish" <<< "$g" && ! grep -q '^exec bash android/test.sh' "$H/android/gate.sh" && \
  [ "$(grep -n '^s2a_gate_init' "$H/android/gate.sh" | cut -d: -f1)" -lt "$(grep -n '^bash android/test.sh' "$H/android/gate.sh" | cut -d: -f1)" ]; } \
  && ok "④ android/gate.sh 跑整套前初始化子项隔离、收尾输出失败子项摘要" || no "④ android/gate.sh 没接子项隔离（${g}）"
# ⑤ graphics/gate.sh 红项证据：缩进的 ✗ 行要进「判红行」
eval "$(sed -n '/^evidence() {/,/done; }$/p' "$H/graphics/gate.sh")"
{ echo "  ✓ ① 好"; echo "  ✗ ⑦ 只出 iOS 拿两份齐的仓：命中 / 重编 (2, 1)（期望 (3, 0)）"; for i in $(seq 1 35); do echo "  ✓ 第 $i 项"; done; echo "t_x：1 例不符"; } > "$T/sub.log"
o=$(REDLOGS=" $T/sub.log"; evidence)
{ grep -q "判红行" <<< "$o" && grep -q "2:  ✗ ⑦ 只出 iOS 拿两份齐的仓" <<< "$o"; } && ok "⑤ 缩进的 ✗ 条目留进红项证据" || no "⑤ 红项证据里没有不符条目：$o"
{ echo "  ✓ ① 好"; echo "t_y：0 例不符"; } > "$T/ok.log"
o=$(REDLOGS=" $T/ok.log"; evidence)
grep -q "判红行" <<< "$o" && no "⑤ 反证：全 ✓ 的日志不该有判红行段：$o" || ok "⑤ 反证：全 ✓ 的日志没有判红行段"
[ $bad = 0 ] && echo "red_summary 单测全过" || echo "red_summary 单测有不通"; exit $bad
