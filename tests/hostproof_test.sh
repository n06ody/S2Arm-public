#!/bin/bash
# 宿主项单测（tools/gate.sh 收「待宿主证 / 已证宿主项」行；假仓库，约 3 秒）：bash tests/hostproof_test.sh
# 期望（按协议手写，10-30 维护者：安卓 APK 这类门禁由具备的宿主证，与「待 Mac 证」对称）：
#   ① 钩子打「待宿主证<TAB>安卓宿主<TAB>apk」且自身退 0 → 门禁退 0、出机读行「待宿主证<TAB>安卓宿主<TAB>apk<TAB>钩子 hp/gate.sh」、
#      ⏭ 行写「本机不是安卓宿主，待安卓宿主证」、末行以「门禁 本机绿、待宿主证」开头且写「待安卓宿主证 1 项」——不许是「门禁 绿」
#   ② 钩子打「已证宿主项<TAB>安卓宿主<TAB>apk」→ 原样出机读行「已证宿主项<TAB>安卓宿主<TAB>apk<TAB>钩子 hp/gate.sh」、末行「门禁 绿」
#   ③ 钩子打待证行但自身红（退 1）→ 门禁照红（退 1）：待证不掩盖红
#   ④ 宿主行不当成该步的结论末行（🟢 行末尾仍是钩子自己的最后一句）
#   ⑤ 负控：LANG=en_US.UTF-8 下跑 ② 的已证场景——BSD awk 在 UTF-8 下 $1 == "待宿主证" 对「已证宿主项」判真（strcoll 塌缩），旧代码这条红、修后绿
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; bad=0
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
mk() { rm -rf "$T/r"; mkdir -p "$T/r/tools" "$T/r/hp"; cp "$H/tools/gate.sh" "$H/tools/gate_plan.sh" "$T/r/tools/"; printf '%s\n' "$1" > "$T/r/hp/gate.sh"
  (cd "$T/r" && git init -q && git add -A && git -c user.name=t -c user.email=t@t commit -qm x); }
run() { (cd "$T/r" && S2A_GATE_CPU_IDLE_MIN=0 GATE_HOST=local GATE_TIER=full bash tools/gate.sh 2>&1); }
mk 'printf "待宿主证\t安卓宿主\tapk\n"; echo 钩子说完'; o=$(run); r=$?
ck "① 待证：退 0（rc=${r}）、机读行带步名" '[ $r = 0 ] && grep -qx "$(printf "待宿主证\t安卓宿主\tapk\t钩子 hp/gate.sh")" <<< "$o"'
ck "① ⏭ 行写待安卓宿主证" 'grep -q "^⏭ apk（钩子 hp/gate.sh）：本机不是安卓宿主，待安卓宿主证" <<< "$o"'
ck "① 末行「门禁 本机绿、待宿主证 … 待安卓宿主证 1 项」（$(tail -1 <<< "$o" | cut -c1-80)）" '[[ "$(tail -1 <<< "$o")" == "门禁 本机绿、待宿主证："*"待安卓宿主证 1 项"* ]]'
ck "④ 宿主行不当成该步末行" 'grep -q "^🟢 钩子 hp/gate.sh .*：钩子说完$" <<< "$o"'
mk 'printf "已证宿主项\t安卓宿主\tapk\n"'; o=$(run); r=$?
ck "② 已证：退 0、机读行原样、末行门禁 绿" '[ $r = 0 ] && grep -qx "$(printf "已证宿主项\t安卓宿主\tapk\t钩子 hp/gate.sh")" <<< "$o" && [[ "$(tail -1 <<< "$o")" == "门禁 绿："* ]]'
mk 'printf "待宿主证\t安卓宿主\tapk\n"; exit 1'; o=$(run); r=$?
ck "③ 待证 + 自身红 → 门禁红（rc=${r}）" '[ $r = 1 ] && [[ "$(tail -1 <<< "$o")" == "门禁 红："* ]]'
# ⑤ 负控：钉 UTF-8 locale——BSD awk 在 en_US.UTF-8 下 $1 == "待宿主证" 对「已证宿主项」判真（strcoll 塌缩），旧代码这条红
mk 'printf "已证宿主项\t安卓宿主\tapk\n"'; o=$(LANG=en_US.UTF-8 LC_ALL= run); r=$?
ck "⑤ UTF-8 下已证不误计待证（旧代码红）" '[ $r = 0 ] && [[ "$(tail -1 <<< "$o")" == "门禁 绿："* ]]'
[ $bad = 0 ] && echo "hostproof 单测全过"; exit $bad
