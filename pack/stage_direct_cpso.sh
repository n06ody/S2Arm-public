#!/bin/bash
# 出包·直连计算管线归档阶段：pack/stage_direct_cpso.sh <款>（照 pack/stage_direct_seed.sh 的轻阶段写法，排在它之后）
# 出 shaders/direct/pipelines/<键>.metalarchive：直连库里每个计算着色器的计算管线，按出包目标 GPU 架构离线编好（graphics/tools/direct/cs_pipelines.py，metal-tt），
#   设备首次派发直接命中随包归档（nvn_mtl.m par_compute_wait → pso_archive.h par_bundle_for），不在提交线程现编（诊断里 5–60 秒的停顿）。
# 为什么单独一段、不进直连库阶段：直连库与目标设备无关（一包通用，tests/stage_direct_test.sh 换目标架构不重出），归档却按「GPU 架构 × 系统大版本」出——
#   换目标设备只重跑这一段（逐支内容寻址缓存，换回来的设备直接命中）。
# 目标对 = pack/jobs/archs.py pairs <平台> pso（显式 S2A_PSO_ARCHS / S2A_PSO_OS > S2A_PSO_ARCHS_ALL=1 > 用户包按目标设备 > 跑局机清单），不自己猜、不出全集。
#   一个文件里同一架构只能装一个系统大版本（metal-tt 胖文件按架构分切片），目标对跨几个大版本时只出最高那个，其余大版本的设备照旧首遇现编（打黄写明）。
# 键 = 直连库阶段键 + 目标对 + 平台 + metal-tt 版本 + 生成代码（pack/stage_inputs/direct_cpso.tsv「入」）；与现行 .stage_direct_cpso 相同就跳过。
# 归档是锦上添花（同种子表）：没有 Metal 工具链、推不出目标对、工具失败都只打【黄】、不记键（下次刷新重试）、退出 0；部分支编不出照收其余、打黄、记键。
# 同一阶段还出每对着色器的拉取基础管线（graphics/tools/direct/base_pipelines.py：离线配对表 shader_pairs.tsv ∪ 管线种子表 pso_manifest.tsv 每对一个 p<键>.mtl4archive，
#   Metal 4 归档；iPhone 包逐对 lzfse 压成 .mtl4archive.lzfse、Mac 包原样；另带一对首查预热 s2a_warm.*），
#   设备混合管线首遇先查它（nvn_hybrid.inc hybrid_get），命中直接特化、不在设备上后台建基础管线。与计算管线同目标对、同一个缓存目录（store/cpso/：键带各自格式号，不会互相命中；直连缓存回收照样认得）、同一次提交；
#   放同一段：输入（直连库 + 目标对 + metal-tt 版本）与产物目录都相同，分两段只多一份键和一次产物根提交。
#   plan 能出的对全部随包，不设体积上限（11-20 维护者：玩家玩着不卡更重要；原先原件 10% 预算见 graphics/tools/direct/base_pipelines.py 文件头）。
# 不出：安卓（S2A_PACK_PLAT=android）、Vulkan / OpenGL / 运行时现编空库（没有 Metal 库容器 b-<链>/lib.idx）。
set -u
G=${1:?款}; H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/.." && pwd); source "$H/roots.sh"; source "$H/stage_keyfiles.sh"; source "$H/srcs.sh"; stage_deps_on || exit 1; SELF=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
CUR=$(root_cur "$G"); [ -n "$CUR" ] || { echo "【红】$G 没有现行产物根"; exit 1; }
PY=${S2A_PYTHON:-${S2A_PY:-python3}}
PLAT=${S2A_PACK_PLAT:-ios}
skip() { echo "🟡 $G 计算管线归档不出：$1，跳过"; exit 0; }
[ "$PLAT" = ios ] || [ "$PLAT" = macos ] || skip "出包平台 ${PLAT} 不是苹果"
D=$CUR/shaders/direct
LIBS=; for x in "$D"/b-*/lib.idx; do [ -f "$x" ] && [ "${x%-vk/lib.idx}" = "$x" ] && LIBS="$LIBS$x "; done   # 不在 $( ) 里写 case：bash 3.2 把分支的 ) 当成命令替换结束
[ -n "$LIBS" ] || skip "没有 NVN Metal 直连库容器（Vulkan / OpenGL / 运行时现编空库）"
soft() { echo "【黄】$G 计算管线归档这次没出：$1；包照常出，缺的是计算着色器首遇免现编（首遇照旧现编），下次刷新重试"; exit 0; }
[ "$(uname -s)" = Darwin ] || soft "本机不是 Mac，没有 Metal 工具链"
S2A_TC_QUIET=1 source "$R/tools/toolchain.sh" > /dev/null 2>&1 || soft "统一工具链解析失败（tools/toolchain.sh）"
SDK=iphoneos; [ "$PLAT" = macos ] && SDK=macosx
TTV=${S2A_METAL_TT:+fake}; [ -n "$TTV" ] || TTV=$(xcrun -sdk "$SDK" metal-tt -version 2> /dev/null | head -1)
[ -n "$TTV" ] || soft "xcrun -sdk $SDK 找不到 metal-tt（要完整的 Xcode）"
PAIRS=$("$PY" "$R/pack/jobs/archs.py" pairs "$PLAT" pso 2> "${TMPDIR:-/tmp}/cpso_pairs.$$") || { e=$(tail -1 "${TMPDIR:-/tmp}/cpso_pairs.$$" | cut -c1-200); rm -f "${TMPDIR:-/tmp}/cpso_pairs.$$"; soft "推不出目标 GPU 架构（pack/jobs/archs.py：${e}）"; }
rm -f "${TMPDIR:-/tmp}/cpso_pairs.$$"
OSV=$(cut -f2 <<< "$PAIRS" | sort -n | tail -1)
ARCHS=$(awk -F'\t' -v v="$OSV" '$2 == v { print $1 }' <<< "$PAIRS" | sort -u | tr '\n' ' '); ARCHS=${ARCHS% }
OTHER=$(cut -f2 <<< "$PAIRS" | sort -nu | grep -vx "$OSV" | tr '\n' ' '); OTHER=${OTHER% }
[ -n "$OSV" ] && [ -n "$ARCHS" ] || soft "目标对是空的"
KT=$(stage_files_key direct_cpso "$R") || exit 1
K=$(printf '%s\n%s\n%s\n%s\n%s\n' "$(cat "$CUR/.stage_direct" 2> /dev/null)" "$PLAT $OSV $ARCHS" "$TTV" "$KT" "$(cat "$D/pso_manifest.tsv" "$D/shader_pairs.tsv" 2> /dev/null | shasum -a 256 | cut -c1-16)" | shasum -a 256 | cut -c1-16)   # 种子表与配对表进键：基础管线的对取自两者之并（base_pipelines.py read_pairs），换了这一段要重出；原件大小进键：预算随它变
KB=$K; K=$(stage_deps_key "$CUR" direct_cpso "$KB" "$SELF") || exit 1
root_stage_done "$CUR" direct_cpso "$K" && { echo "🟢 $G 现行已是计算管线归档 ${K}，跳过"; exit 0; }
TID=$(prog_id "$CUR")
C=${S2A_DIRECT_CACHE:-${S2A_RUN_ROOT:-$HOME/.s2arm/run}/s2arm/cache/direct/${TID:-unknown}}
TAG=$(root_tag "$G" "cp-${K:0:8}"); W=$(root_begin "$G" "$TAG") || exit 1
P=$W/shaders/direct/pipelines; rm -rf "$P"
o=$("$PY" "$R/graphics/tools/direct/cs_pipelines.py" "$W/shaders/direct" "$P" --plat "$PLAT" --os "$OSV" $(printf -- '--arch %s ' $ARCHS) --cache "$C/store/cpso" -j "${J:-$(getconf _NPROCESSORS_ONLN)}" 2>&1); rc=$?
if [ "$rc" != 0 ] && [ "$rc" != 3 ]; then root_abort "$G" "$TAG"; soft "cs_pipelines.py 退出 ${rc}：$(tail -1 <<< "$o" | cut -c1-200)"; fi
ob=$("$PY" "$R/graphics/tools/direct/base_pipelines.py" "$W/shaders/direct" "$P" --plat "$PLAT" --os "$OSV" $(printf -- '--arch %s ' $ARCHS) --cache "$C/store/cpso" -j "${J:-$(getconf _NPROCESSORS_ONLN)}" 2>&1); rb=$?
if [ "$rb" != 0 ] && [ "$rb" != 3 ]; then root_abort "$G" "$TAG"; soft "base_pipelines.py 退出 ${rb}：$(tail -1 <<< "$ob" | cut -c1-200)"; fi
NOTE=; [ "$rc" = 3 ] && NOTE="；🟡 有支没编成，照旧首遇现编（$(grep -c '^【黄】计算管线' <<< "$o") 条示例见日志）"
[ "$rb" = 3 ] && NOTE="${NOTE}；🟡 有对的拉取基础管线没编成，照旧设备后台建（$(grep -c '^【黄】拉取基础管线' <<< "$ob") 条示例见日志）"
[ -z "$OTHER" ] || NOTE="${NOTE}；🟡 目标对另有系统大版本 ${OTHER}：一个归档文件同一架构只装一个大版本，那些设备照旧首遇现编"
stage_deps_save "$W" direct_cpso "$KB" "$SELF" > "$W/.stage_direct_cpso" || { root_abort "$G" "$TAG"; exit 1; }
ROOT_REPLAY='shaders/direct/pipelines .stage_direct_cpso .stage_direct_cpso.deps' root_commit "$G" "$TAG" || exit 1
grep '^【黄】' <<< "$o" | head -8; grep '^【黄】' <<< "$ob" | head -8
echo "🟢 $G $(tail -1 <<< "$ob")"
echo "🟢 $G $(tail -1 <<< "$o")${NOTE}（现行 → $(basename "$(root_cur "$G")")）"
