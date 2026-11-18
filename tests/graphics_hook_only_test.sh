#!/bin/bash
# 门禁覆盖：graphics/gate.sh
# gfx 钩子「只跑点名子项」单测（GATE_HOOK_ONLY / GATE_HOOK_LIST，不跑任何图形测试，本机负载下约 30 秒）：bash tests/graphics_hook_only_test.sh
# 期望按 graphics/gate.sh 各判据的前缀手写（不跑被测实现推期望）：
#   ① 不设 GATE_HOOK_ONLY = 现状（各判据照原样，rest 总跑）；② 点名一个键只剩它；③ 点名的键本批本就不涉及 → 仍不跑；
#   ④ 各键单独点名的并 = 不设时的集合（多组改动）；⑤ 认不得的键 / 与 GATE_ONLY 冲突 → 红，相同 → 当同一请求；
#   ⑥ GATE_HOOK_LIST=inputs 的前缀（门禁据此算各子步输入）、闭包算不出 → *；⑦ job：「其余」项不在名单只记一行不跑、退出码 0，标记不进被测命令环境；
#   ⑧ 结构：每个判据都在 HOOK_KEYS 里、分道 / 汇总处不再内联 touched、单测池里无判据的项都带 HOOK_REST=1 且带它的都没有判据
set -u; H=$(cd "$(dirname "$0")/.." && pwd); cd "$H"; G=graphics/gate.sh; bad=0; n=0
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
lst() { GATE_CHANGED="$1" GATE_HOOK_ONLY="$2" GATE_HOOK_LIST=1 bash "$G" | tr '\n' ' ' | sed 's/ $//'; }
ok() { n=$((n + 1)); if [ "$2" = "$3" ]; then echo "通 $1 → [$2]"; else echo "不通 $1：期望 [$3] → 真结果 [$2]"; bad=1; fi; }
ALL="track_c track_net shared_gpu_contract vulkan_diag vvl texture_dma_read metal cntdead session m3pack direct_link shbake_net frame_pacing nvn red nvn_tex dsh gl vk present_residency fps_limit present_settings shdist dmsh_giant dsh_vk dsh_vklib compute_outline repair_compute repair_loader rest"
KEYS="track_c track_net shared_gpu_contract vulkan_diag vvl texture_dma_read metal cntdead session m3pack direct_link shbake_net frame_pacing nvn red nvn_tex dsh gl vk present_residency shdist dmsh_giant dsh_vk dsh_vklib compute_outline repair_compute repair_loader rest"
# ① 不设：渲染器改动 = 渲染器 / 计数回调 / 会话 / Metal 3 归档 / 直连链接版组 / 上屏驻留（同 gfx_gate_group_test）+ rest；改动集空 = 全部
ok "不设 ONLY，改 graphics/metal/gal_present.m" "$(lst graphics/metal/gal_present.m '')" "metal cntdead session m3pack direct_link present_residency rest"
ok "不设 ONLY，改动集空" "$(lst '' '')" "$ALL"
ok "不设 ONLY，改 graphics/tools/direct/tests/t_dsh.sh（direct/ 前缀：直连链接版组 / 采样器模拟 / dsh）" "$(lst graphics/tools/direct/tests/t_dsh.sh '')" "direct_link red dsh rest"
ok "不设 ONLY，只改文档" "$(lst docs/x.md '')" "rest"
# ② 点名一个键只剩它
ok "ONLY=dsh，改 t_dsh.sh" "$(lst graphics/tools/direct/tests/t_dsh.sh dsh)" "dsh"
ok "ONLY=rest，改 t_dsh.sh" "$(lst graphics/tools/direct/tests/t_dsh.sh rest)" "rest"
ok "ONLY=dsh，改动集空" "$(lst '' dsh)" "dsh"
ok "ONLY=present_residency，改 runtime/src/hud.c（连带帧率上限矩阵）" "$(lst runtime/src/hud.c present_residency)" "present_residency fps_limit"
ok "ONLY=dsh_vklib，改 graphics/tools/direct/dsh/x.cs（它的判据内部调 need_dsh_vk，按原判据算）" "$(lst graphics/tools/direct/dsh/x.cs dsh_vklib)" "dsh_vklib"
ok "ONLY 两个键，改动集空" "$(lst '' 'nvn gl')" "nvn gl"
# ③ 点名的键本批不涉及 → 仍不跑
ok "ONLY=dsh，改 graphics/metal/gal_present.m" "$(lst graphics/metal/gal_present.m dsh)" ""
ok "ONLY='metal rest'，改 t_dsh.sh" "$(lst graphics/tools/direct/tests/t_dsh.sh 'metal rest')" "rest"
ok "ONLY=present_residency，改 t_dsh.sh" "$(lst graphics/tools/direct/tests/t_dsh.sh present_residency)" ""
# ④ 各键单独点名的并 = 不设时（按不设时的顺序排）
for c in "" graphics/tools/direct/dsh/x.cs "runtime/src/hud.c graphics/nvn/gfx/nvn/nvn_mtl.m" compat/tree/src/Ryujinx.Graphics.Gpu/xxx; do
  u=" "; for k in $KEYS; do for x in $(lst "$c" "$k"); do u="$u$x "; done; done
  whole=$(lst "$c" ''); got=; for x in $whole; do case "$u" in *" $x "*) got="$got $x";; esac; done
  extra=; for x in $u; do case " $whole " in *" $x "*) ;; *) extra="$extra $x";; esac; done
  ok "并集 = 不设 ONLY（改 [${c}]）" "${got# }${extra:+ 多出$extra}" "$whole"
done
# ⑤ 认不得的键 / GATE_ONLY 冲突
n=$((n + 1)); o=$(GATE_HOOK_ONLY=fps_limit GATE_HOOK_LIST=1 bash "$G" 2>&1); r=$?
if [ $r = 1 ] && grep -q "子项键 fps_limit 认不得" <<< "$o"; then echo "通 认不得的键 fps_limit（只是 present_residency 的加跑判据）→ 红"; else echo "不通 认不得的键：退出 ${r}，输出 $o"; bad=1; fi
n=$((n + 1)); o=$(GATE_HOOK_ONLY=dsh GATE_ONLY=gl GATE_HOOK_LIST=1 bash "$G" 2>&1); r=$?
if [ $r = 1 ] && grep -q "同设且不同" <<< "$o"; then echo "通 GATE_ONLY 与 GATE_HOOK_ONLY 不同 → 红"; else echo "不通 GATE_ONLY 冲突：退出 ${r}，输出 $o"; bad=1; fi
ok "GATE_ONLY 与 GATE_HOOK_ONLY 相同（tools/gate.sh 转发时两个都放）→ 当同一请求" "$(GATE_ONLY=dsh GATE_HOOK_ONLY=dsh GATE_CHANGED= GATE_HOOK_LIST=1 bash "$G" | tr '\n' ' ' | sed 's/ $//')" "dsh"
# ⑥ GATE_HOOK_LIST=inputs：前缀 = 判据里 touched 的参数（多分支的每一支），rest / 闭包算不出 = *
inp=$(GATE_HOOK_LIST=inputs bash "$G"); line() { awk -F'\t' -v k="$1" '$1 == k { print $2 }' <<< "$inp"; }
ok "inputs dsh" "$(line dsh)" "graphics/nvn/gfx/nvn/dmsh_chain.h graphics/tools/direct/ translator/"
ok "inputs nvn" "$(line nvn)" "graphics/nvn/"
ok "inputs nvn_tex" "$(line nvn_tex)" "graphics/nvn/gfx/nvn/nvn_tex. graphics/nvn/gfx/tex/"
ok "inputs repair_loader" "$(line repair_loader)" "graphics/nvn/gfx/nvn/nvn_mtl.m graphics/nvn/lib_pack graphics/tools/direct/repair_compute.py graphics/tools/direct/tests/t_repair_loader"
ok "inputs rest" "$(line rest)" "*"
ok "inputs 键数（每个键一行）" "$(awk -F'\t' '{ print $1 }' <<< "$inp" | tr '\n' ' ' | sed 's/ $//')" "$KEYS"
pr=" $(line present_residency) "; miss=; for p in runtime/src/hud.c graphics/nvn/gfx/nvn/nvn_fps. graphics/metal/shaders/aa_ graphics/nvn/gfx/nvn/nvn_present_aa.inc graphics/nvn/host/ tools/toolchain.env; do case "$pr" in *" $p "*) ;; *) miss="$miss $p";; esac; done
ok "inputs present_residency 含帧率上限 / 画面设置两个加跑判据的前缀" "${miss:-无缺}" "无缺"
vl=" $(line dsh_vklib) "; miss=; for p in $(line dsh_vk) graphics/tools/direct/vklib/ pack/stage_inputs/direct.tsv; do case "$vl" in *" $p "*) ;; *) miss="$miss $p";; esac; done
ok "inputs dsh_vklib ⊇ dsh_vk（|| 两支都记）+ vklib 自己的" "${miss:-无缺}" "无缺"
tn=" $(line track_net) "; miss=; for p in graphics/track/ graphics/gate.sh graphics/track/tests/ryucmp/ compat/tree/src/Ryujinx.Graphics.Gpu/; do case "$tn" in *" $p "*) ;; *) miss="$miss $p";; esac; done
ok "inputs track_net 含另加前缀、ALL_PFX 与 .csproj 闭包（引到 Ryujinx.Graphics.Gpu）" "${miss:-无缺}" "无缺"
printf '#!/bin/sh\nexit 1\n' > "$T/python3"; chmod +x "$T/python3"
ok "闭包算不出（python3 失败）→ .NET 键打 *" "$(PATH="$T:$PATH" GATE_HOOK_LIST=inputs bash "$G" | awk -F'\t' '$2 == "*" { printf "%s%s", s, $1; s = " " }')" "track_net shared_gpu_contract vulkan_diag vvl texture_dma_read cntdead session shbake_net frame_pacing rest"
# ⑦ job 的「其余」分支：抽出 has / hook_sel / hook_rest_off / job 单独跑
eval "$(grep -E '^(has|hook_sel|hook_rest_off|job)\(\) ' "$G")"
LOG=$T/log; JOBD=$T/jobs; mkdir -p "$LOG" "$JOBD"; : > "$LOG/jobs.tsv"
rj() { rm -rf "$JOBD"/*; GATE_HOOK_ONLY="$1" HOOK_REST="$2" job probe sh -c 'echo "跑了 HOOK_REST=[${HOOK_REST-未设}]"'; echo "$(cat "$JOBD/probe.rc")|$(cat "$LOG/probe.log")"; }
ok "job：HOOK_REST=1、名单不含 rest → 不跑、退出码 0" "$(rj dsh 1)" "0|本步只跑点名子项（GATE_HOOK_ONLY=dsh），「其余」不在名单，不跑"
ok "job：HOOK_REST=1、名单含 rest → 照跑，标记不进被测命令环境" "$(rj 'dsh rest' 1)" "0|跑了 HOOK_REST=[未设]"
ok "job：HOOK_REST=1、不设名单 → 照跑" "$(rj '' 1)" "0|跑了 HOOK_REST=[未设]"
ok "job：没带 HOOK_REST（有判据的项）、名单不含 rest → 照跑" "$(rj dsh '')" "0|跑了 HOOK_REST=[未设]"
# ⑧ 结构检查（防以后加项时漏挂）：判据全在 HOOK_KEYS；touched / net_touched 只在判据定义里调；单测池 job 行：无判据 ⇔ 带 HOOK_REST=1
miss=; for p in $(grep -oE '^need_[a-z_0-9]+\(\)' "$G" | sed 's/^need_//; s/()$//'); do case " $KEYS fps_limit present_settings " in *" $p "*) ;; *) miss="$miss $p";; esac; done
ok "每个 need_* 判据都在 HOOK_KEYS（或是 present_residency 的加跑判据）" "${miss:-无缺}" "无缺"
ok "判据定义以外不内联 touched / net_touched" "$(sed 's/^#.*//; s/[[:space:]]#.*//' "$G" | grep -nE '(^|[^_a-z])(net_)?touched ' | grep -vE '^[0-9]+:[[:space:]]*(need_[a-z_0-9]+|touched|net_touched)\(\)' | cut -d: -f1 | tr '\n' ' ' | sed 's/ $//')" ""
pyb=$(awk '/^py_items\(\) \{/ { f = 1; next } f && /^\}/ { exit } f' "$G")
viol=$(awk '{ sub(/#.*/, "") } /^[[:space:]]*if / { d++ } /^[[:space:]]*fi([[:space:];]|$)/ { d-- }
  / job / || /^[[:space:]]*(HOOK_REST=1 )?job / { r = ($0 ~ /HOOK_REST=1 job /); g = (d > 0 || $0 ~ /&& *(HOOK_REST=1 )?job /);
    if (r == g) print NR ": " substr($0, 1, 60) }' <<< "$pyb")
ok "单测池：无判据的 job 都带 HOOK_REST=1、带它的都没有判据" "$viol" ""
echo "期望 ${n} 项全对 → 真结果 $([ $bad = 0 ] && echo "${n} 项全对 ✓" || echo "有不符 ✗")"
[ $bad = 0 ]
