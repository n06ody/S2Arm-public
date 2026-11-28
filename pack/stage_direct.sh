#!/bin/bash
# 出包·直连着色器库阶段：pack/stage_direct.sh <款>（双轨 docs/设计_直连架构.md §12.3，着色器线 10-24）
# 只在双轨出包（S2A_DUAL=1）时由 refresh.sh 调：graphics/tools/direct/direct_lib.py 把游戏原始输入（romfs/ + exefs/main + aoc/*）出成直连库，
#   放新版本产物根 shaders/direct/（布局照直连离线链原样：b-<链版本>/<键>_b.metallib/.json + 各表 + keys.tsv）；ipa.sh 只在 S2A_DUAL=1 时把它带进资源包（同路径 shaders/direct/），
#   运行时即 S2A_GAME_DATA/shaders/direct/。通用包不带（动森 iOS 库 332 MB）。
# direct/guest_key.tsv（客体代码 → 直连键，通用模式写 seen 用）不在这里出：它要着色器阶段的 shaders/guest_keys.tsv，归 pack/stage_direct_gk.sh。
#   10-25 维护者「一次出完整包」：直连库与通用翻译各算各的键、并行出（pack/refresh.sh），直连库不再排在通用烘焙后面
# 缓存键 = 游戏输入（romfs / aoc 路径+长度+首尾64KiB抽样、exefs/main 全内容）+ 出库代码（pack/stage_inputs/direct.tsv「入」的文件 + 翻译链号）+ iOS Metal 编译器版本（出库目标是安卓时不带）
#   + 跑局知识里改产物的三张表（配对 / 片元整数输出变体 / 采样模拟变体，审计 #1534 P1-54）；
#   与现行 .stage_direct 相同就跳过。键变了也只重编内容变了的键（direct_lib.py 的 dmsh 内容寻址缓存在 S2A_DIRECT_CACHE）。
# 未登记/外置档先经 direct_profile 准备并校验；所选档内容哈希进键，出库按同一份档。
set -u
G=${1:?款}; H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/.." && pwd); source "$H/roots.sh"; source "$H/stage_keyfiles.sh"; stage_deps_on || exit 1; SELF=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")   # 实际读入进键（pack/stage_keyfiles.sh）
CUR=$(root_cur "$G"); [ -n "$CUR" ] && { [ -d "$CUR/romfs" ] || [ -f "$CUR/romfs.std" ]; } && [ -f "$CUR/exefs/main" ] || { echo "【红】$G 现行产物根没有 romfs/（或镜像 romfs.std）或 exefs/main"; exit 1; }   # RomFS 目录或镜像都行（romfs_list.py image_of）
# Direct invocations need the same profile preflight as refresh. The profile
# stage may publish a provisional candidate; runtime acceptance is separate.
bash "$H/stage_direct_profile.sh" "$G" --require-consumer || exit $?
CUR=$(root_cur "$G")
PY=${S2A_PYTHON:-${S2A_PY:-python3}}
VPY=${S2A_PYTHON:-${S2A_PY:-$(bash "$R/tools/machine.sh" get S2A_PY 2> /dev/null)}}; VPY=${VPY:-python3}
API=$("$VPY" "$R/graphics/nvn/tools/gfx_api.py" "$CUR/exefs" "$CUR/romfs") || { echo "【红】$G 判图形接口失败（graphics/nvn/tools/gfx_api.py）：${API#*	}"; exit 1; }
if [ "${API%%	*}" = vulkan ] || [ "${API%%	*}" = gl ]; then PDIR=; PK=${API%%	*}   # Vulkan / OpenGL 款没有 NVN 配置档（stage_direct_profile.sh 已跳过）；direct_lib.py 走各自的分支
else
PROFILE=$("$PY" "$H/direct_profile_stage.py" "$CUR" --selected) || exit $?   # 退 3 = 配置推导不支持本款（内容缺口），原样交给 refresh 打黄
PDIR=$("$PY" -c 'import json,sys;print(json.loads(sys.argv[1])["profile"])' "$PROFILE")
PK=$("$PY" -c 'import json,sys;print(json.loads(sys.argv[1])["profile_key"])' "$PROFILE")
fi
# 出库代码键（10-26 维护者：原先取 graphics/nvn / graphics/tools/direct / graphics/tools/shbake 整树对象号，宿主运行时代码改一行九款直连库就整份重出，
#   今天 19 次重做里 8 次、4518 秒翻译链号根本没变）：只算 pack/stage_inputs/direct.tsv「入」的文件（出库脚本、翻译链工具与它们的依赖闭包），
#   再加库版本 V（dmsh_chain.py；10-30 起只跟宿主读库的接口版本 ABI 走，翻译器改动由上面「入」的文件内容带进键，库内按单支键 / MSL 摘要增量重出）。表漏没漏由 tests/stage_keys_test.sh 扫依赖闭包核
source "$H/stage_keyfiles.sh"; KT=$(stage_files_key direct "$R") || exit 1
CV=$(cd "$R/translator" && "$PY" -c 'import dmsh_chain; print(dmsh_chain.version())') && [ -n "$CV" ] || { echo "【红】取不到翻译链号（translator/dmsh_chain.py）"; exit 1; }
KT="$KT chain-$CV"
# 抽取缺口进键（11-05）：有 extract/direct_gap.txt = 出运行时现编空库；抽取修好、标记没了（游戏输入不变）要能重出真库，有无标记与其内容都进键
GAPK=none; [ -s "$CUR/extract/direct_gap.txt" ] && GAPK=$(shasum -a 256 < "$CUR/extract/direct_gap.txt" | cut -c1-12)
KT="$KT gap-$GAPK"
KT="$KT api-${API%%	*}"   # 图形接口类别进键（gfx_api.py 在 direct.tsv 记「外」：判法改了而类别没变的款不重出；与 direct_lib.py 同带 romfs，判出的是同一个类别）
# NVN 款另带 GL（gfx_api.py --gl-also，direct_lib.py 据此再出一份 GL 着色器库）同样按判出的结果进键：判断结果变了的款才因这一项重出；不带 GL 的款键里没有这一项
if [ "${API%%	*}" = nvn ]; then GLX=$("$VPY" "$R/graphics/nvn/tools/gfx_api.py" --gl-also "$CUR/exefs" "$CUR/romfs") || { echo "【红】$G 判另带 GL 失败（graphics/nvn/tools/gfx_api.py --gl-also）"; exit 1; }
  [ "${GLX%%	*}" = 1 ] && KT="$KT gl-also"; fi
# 改产物的出包环境变量进键（直连线 10-26 核）：出包平台（定只出 iOS 还是两份都出）、调用方给的出库目标
KT="$KT env-${S2A_PACK_PLAT:-ios}/${S2A_DIRECT_TARGETS-}"
# 安卓目标（#783）：Vulkan 侧指纹 VkFp（DshVk.cs + graphics/tools/direct/vklib/ 全部源码，vklib/VkFp.targets）进键。这些文件不进 direct.tsv「入」（改它们不让苹果款重出），
#   不带 VkFp 的话只改 Vulkan 侧代码时安卓键不变，产物根留着旧库、新 App 指纹对不上整库不用。只在出库目标是 android 时算，苹果键不动
#   出库目标缺省随出包平台（同下面 export S2A_DIRECT_TARGETS 那行）；下面 MV 也按它
VKT=; [ "${S2A_DIRECT_TARGETS-$([ "${S2A_PACK_PLAT:-ios}" = android ] && echo android)}" = android ] && VKT=1
if [ -n "$VKT" ]; then
  VKFP=$("$PY" "$R/graphics/tools/direct/vklib/dsh_vklib.py" fp) || { echo "【红】$G 算不出 Vulkan 侧指纹（graphics/tools/direct/vklib/dsh_vklib.py fp）"; exit 1; }
  KT="$KT vk-$VKFP"
fi
# 跑局知识进键（审计 #1534 P1-54 B1/B2）：graphics/tools/direct/run_knowledge.py 把 pso_seed/<程序号>/（或 S2A_PSO_SOURCES）整理成归一化快照，本阶段只认改产物的三张——
#   配对 pairs.tsv（vs_pad 补顶点输出）、片元整数输出变体 fout_want.tsv（Metal _ft、Vulkan _vf）、采样模拟变体 se_want.tsv（仅 Metal _se）。
#   只在学到新配对 / 新变体时变；重复行、行序、同内容的重复文件、修改时间、来源路径 / 机器都不变键。变了也只重编受影响的几支（direct_lib.py 的内容寻址缓存）。
#   管线种子（seen.tsv，只改随包的 pso_manifest.tsv 一张表）不进本键，归轻阶段 pack/stage_direct_seed.sh。出库用的就是算键的这一份（S2A_RUN_KNOWLEDGE），不再边出库边读活文件
KNK=none; KND=
if [ "${API%%	*}" != gl ]; then   # OpenGL 款出库不读跑局知识（direct_lib.py 在读它之前就分走了）；Vulkan 款只读快照里的 vk_spv/（设备现编存下的 SPIR-V，11-05）
  KND=$(mktemp -d "${TMPDIR:-/tmp}/s2a_knowledge.XXXXXX") || { echo "【红】$G 建不了跑局知识快照的临时目录"; exit 1; }
  trap 'rm -rf "$KND"' EXIT
  "$PY" "$R/graphics/tools/direct/run_knowledge.py" "$KND/t" "$CUR" > "$KND/log" 2>&1 || { echo "【红】$G 跑局知识快照出不来（graphics/tools/direct/run_knowledge.py：$(tail -1 "$KND/log" | cut -c1-200)）"; exit 1; }
  KNT="pairs fout_want se_want"; [ -z "$VKT" ] || KNT="pairs fout_want"
  [ "${API%%	*}" != vulkan ] || KNT=   # Vulkan 款：三张表不用，只认 vk_spv/（文件名 = 内容键，名单即内容）
  KNK=$({ for f in $KNT; do printf '%s %s\n' "$(shasum -a 256 < "$KND/t/$f.tsv" | cut -c1-64)" "$f"; done; ls "$KND/t/vk_spv" 2> /dev/null | sed 's/^/vk_spv /'; } | shasum -a 256 | cut -c1-16)
fi
KT="$KT know-$KNK"
# 编译器版本与目标 iOS 大版本进键：只读 tools/toolchain.sh 解析一次的结果（10-25：各处各取，一键出包两次 refresh 键不同、直连库整份出两遍）
# 安卓在 Linux 出包机上没有 Xcode：同 pack/refresh.sh，只在不是安卓或在 macOS 上时解析（macOS 上下面编 libdsx.dylib 用它导出的 DEVELOPER_DIR，graphics/dsx/Makefile）
if [ "${S2A_PACK_PLAT:-ios}" != android ] || [ "$(uname -s)" = Darwin ]; then
  S2A_TC_QUIET=1 source "$R/tools/toolchain.sh" || exit 2   # 上游（refresh / 出包）已解析就直接沿用；本阶段不再打工具链那一行
fi
MV="${S2A_TC_METAL:-}"   # 一包通用（10-30 维护者）：不带目标 iOS 大版本——iOS 库固定按 -mios-version-min=18.0 编（graphics/nvn/ios/tools/ios_metal.py，「入」文件进键），换目标大版本产物逐字节相同
# 出库目标是安卓时 MV 置空（#1425 审）：安卓库不经 Metal 编译器，macOS 上出安卓包、或上游导出了 S2A_TC_METAL 时，Xcode 升级不该让安卓库重出一遍
[ -z "$VKT" ] || MV=
KR=
# 只复用本轮已验证的不可变根版本；未知根、工具版本变化或损坏memo一律回到同一helper核算。
if [ -n "${S2A_RESOURCE_KEY_ONCE_FILE:-}" ] && [ -f "$S2A_RESOURCE_KEY_ONCE_FILE" ]; then
  read -r RK_V RK_ROOT RK_KEY RK_SUM RK_HELPER RK_EXTRA < "$S2A_RESOURCE_KEY_ONCE_FILE" || :
  RK_ID=$(stat -c '%d:%i' "$CUR" 2>/dev/null || stat -f '%d:%i' "$CUR" 2>/dev/null) || RK_ID=
  RK_MEMO="$(dirname "$CUR")/.resource-input-cache/${RK_ID/:/-}.json"
  if [ "${RK_V:-}" = s2a-resource-once-v2 ] && [ "${RK_ROOT:-}" = "$RK_ID" ] && [ -z "${RK_EXTRA:-}" ] && [ -f "$RK_MEMO" ]; then
    case ${RK_KEY:-} in ''|*[!0-9a-f]*) ;; *)
      [ ${#RK_KEY} = 16 ] && [ "$(shasum -a 256 < "$RK_MEMO" | cut -d' ' -f1)" = "${RK_SUM:-}" ] && [ "$(shasum -a 256 < "$H/resource_input_key.py" | cut -d' ' -f1)" = "${RK_HELPER:-}" ] && KR=$RK_KEY;;
    esac
  fi
fi
[ -n "$KR" ] || KR=$("$PY" "$H/resource_input_key.py" "$CUR") || { echo "【红】$G 算游戏输入键失败"; exit 1; }
KRT=$(cd "$CUR" 2> /dev/null && for f in rt_shaders/*; do [ ! -f "$f" ] || printf '%s %s\n' "$(shasum -a 256 < "$f" | cut -c1-64)" "$f"; done)   # 真机转储的着色器（direct_lib.py 并进库）：增减要重出库，不动抽取
K=$(printf '%s\n%s\n%s\n%s\n%s\n%s\n' "$KR" "$KT" "$MV" "$PK" "$(cat "$CUR/.stage_extract" 2>/dev/null)" "$KRT" | shasum -a 256 | cut -c1-16)   # 抽取阶段键进键：抽取变了要重出
KB=$K; K=$(stage_deps_key "$CUR" direct "$KB" "$SELF") || exit 1
root_stage_done "$CUR" direct "$K" && { echo "🟢 $G 现行已是直连库 ${K}，跳过"; exit 0; }
TAG=$(root_tag "$G" "dl-${K:0:8}"); W=$(root_begin "$G" "$TAG") || { root_abort "$G" "$TAG"; exit 1; }
t0=$SECONDS; rm -rf "$W/shaders/direct" "$W/direct.tmp"
rm -rf "$W/knowledge/direct"; if [ -n "$KND" ]; then   # 算键的那份快照随产物根留档（knowledge/direct/），出库只读它
  mkdir -p "$W/knowledge/direct" && cp "$KND/t/pairs.tsv" "$KND/t/fout_want.tsv" "$KND/t/se_want.tsv" "$W/knowledge/direct/" || { echo "【红】$G 跑局知识快照拷不进工作根"; root_abort "$G" "$TAG"; exit 1; }
  [ ! -d "$KND/t/vk_spv" ] || cp -R "$KND/t/vk_spv" "$W/knowledge/direct/" || { echo "【红】$G 跑局知识快照（vk_spv）拷不进工作根"; root_abort "$G" "$TAG"; exit 1; }
  export S2A_RUN_KNOWLEDGE=$W/knowledge/direct S2A_PSO_SEED_STAGE=1   # 种子表归轻阶段 pack/stage_direct_seed.sh（它自己按 seen.tsv 进键），出库不生成
fi
# 抽着色器在前面的抽取阶段（pack/stage_extract.sh → <产物根>/extract/，含 Unreal 存档 ue_pak），direct_lib.py 直接克隆来用
# 直连库要直连翻译库 libdsx（graphics/dsx：曲面阶段翻译 nxgal_dmsh_xc_tess 等，11-08 从渲染器 libnxgal 拆出）：全新工作树第一次出包时它还不在
#   （10-25 性能攻坚线：产物被静默跳过、阶段又判已完成，之后再出也不补）。这里先编本树的；调用方给了 DSX_LIB 就用给的
# 启动器（S2A_PREBUILT_TOOLS = App 内 Binaries/Prepare，launchers/macos/bundle_engine.py 随发版带同一份源码编好的 dsx/libdsx.dylib）直接用内置的，
#   同 graphics/tools/shader_pack.sh 的内置工具口径（11-06 出包速度线：玩家首次出包原先在关键链上现编直连库 / libspvc，Spare-host 约 25 秒）
if [ -z "${DSX_LIB:-}" ] && [ "$(uname -s)" = Darwin ] && [ -n "${S2A_PREBUILT_TOOLS:-}" ] && [ -f "$S2A_PREBUILT_TOOLS/dsx/libdsx.dylib" ]; then
  export DSX_LIB="$S2A_PREBUILT_TOOLS/dsx/libdsx.dylib"
fi
if [ -z "${DSX_LIB:-}" ]; then
  # 引擎在带空格的目录（启动器：~/Library/Application Support）时 make 拆路径：-C / ROOT / 目标都用无空格别名（tools/nospace.sh）
  source "$R/tools/nospace.sh"; RN=$(s2a_nospace "$R") || { echo "【红】$G 建不了引擎目录的无空格别名"; exit 1; }
  export DSX_LIB="$RN/build/dsx/libdsx.dylib"
  [ "$(uname -s)" = Darwin ] || DSX_LIB="$RN/build/dsx/linux/libdsx.so"   # 安卓目标在 Linux 出包机：同一源码集合 + SPIRV-Cross
  case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) DSX_LIB="$RN/build/dsx/win/dsx.dll";; esac   # Windows 出包机（#1100 W6）：没有 make，由 graphics/dsx/dsx_win.py 编同一源码集合
  { if [[ "$DSX_LIB" == *.dll ]]; then "$PY" "$RN/graphics/dsx/dsx_win.py" "$DSX_LIB"; else make -s -j"${J:-${S2A_JOBS:-1}}" -C "$RN/graphics/dsx" ROOT="$RN" "$DSX_LIB"; fi; } > "$W/dsx_build.log" 2>&1 \
    || { echo "【红】$G 直连库要的 libdsx 编不出来（$W/dsx_build.log）"; mkdir -p "$S2A_ROOTS/.failed" && cp "$W/dsx_build.log" "$S2A_ROOTS/.failed/${G}_dsx_build.log"; root_abort "$G" "$TAG"; exit 1; }
fi
# 并行度 J：调用方给的 J / S2A_CPU_BAKE（tools/cores.sh 按本机核数与内存定），都没有就是本机核数（10-25 维护者「多核要适配机器的核心数」），direct_lib.py 各子步骤都按它开路数
J=${J:-${S2A_CPU_BAKE:-${S2A_JOBS:-$(getconf _NPROCESSORS_ONLN)}}}; [ "$J" -ge 1 ] 2> /dev/null || { echo "【红】$G 并行度读不出（J / S2A_JOBS / getconf 核数）"; root_abort "$G" "$TAG"; exit 1; }; export J
# 只出目标平台的库（10-25 维护者拍板）：出 iOS 包缺省只出 iOS 库，Mac 那份不编 metallib（运行时不读，direct_lib.py 头注）；出 Mac 版 App（S2A_PACK_PLAT=macos）只出 Mac 库（10-31）。调用方给了 S2A_DIRECT_TARGETS 就用调用方的
if [ "${S2A_PACK_PLAT:-ios}" = macos ]; then export S2A_DIRECT_TARGETS=${S2A_DIRECT_TARGETS-mac}; elif [ "${S2A_PACK_PLAT:-ios}" = android ]; then export S2A_DIRECT_TARGETS=${S2A_DIRECT_TARGETS-android}; else export S2A_DIRECT_TARGETS=${S2A_DIRECT_TARGETS-ios}; fi   # 10-31 Mac 版也只出目标平台（原先两份都出，iOS 那份无人读）；安卓（#783）只出 Vulkan 库
# 发布哪一份进包（#897 wwmogu：--target mac 的包带的是 iOS 库）：只按出包平台定、不收调用方的值（出包平台已在键里 env-<平台>，发布目标随它走，键不用另加）
if [ "${S2A_PACK_PLAT:-ios}" = macos ]; then export S2A_DIRECT_PUBLISH=mac; elif [ "${S2A_PACK_PLAT:-ios}" = android ]; then export S2A_DIRECT_PUBLISH=android; else export S2A_DIRECT_PUBLISH=ios; fi
# 出库退出码（10-25 维护者）：0 = 出成（可部分出库，黄行列没出成的类别与计数）；3 = 内容缺口、本款这次出不了直连库（打黄、refresh 走通用）；其余 = 基础设施出错（报红停住）
# 抽取缺口（stage_extract.sh 写的 extract/direct_gap.txt，11-05）：NVN 款出一个合法的空直连库，运行时首遇现编——写法在 graphics/tools/direct/runtime_only_lib.sh
#   （11-06 收成一份：Windows 出包机链 direct_lib.py 现场抽取退 3 时调同一个）。同 OpenGL 款离线 0 支的先例（下面「keys.tsv 为空也算出了库」）
if [ -s "$CUR/extract/direct_gap.txt" ] && [ "${API%%	*}" != vulkan ] && [ "${API%%	*}" != gl ]; then   # Vulkan / OpenGL 款不抽 NVN 着色器，不会有这个标记
  D=$W/direct.tmp; rm -rf "$D"; TID=$(awk -F'\t' '$1 == "title_id" {print toupper($2); exit}' "$CUR/unpack.tsv" 2> /dev/null) && [ -n "$TID" ] \
    && bash "$R/graphics/tools/direct/runtime_only_lib.sh" "$D" "$CV" "$TID" "${K:0:12}" "$CUR/extract/direct_gap.txt" "${PDIR:+$PDIR/game.tsv}" \
    || { echo "【红】$G 出运行时现编空直连库失败（程序号 / 写文件）"; root_abort "$G" "$TAG"; exit 1; }
  echo "[直连库] 🟡 ${TID} 离线 0 支、运行时首遇现编（$(head -1 "$CUR/extract/direct_gap.txt" | cut -c1-200)）" > "$W/direct_lib.log"; drc=0
else
DM_GAMES_DIR="${PDIR:+$(dirname "$PDIR")}${DM_GAMES_DIR:+${PDIR:+:}$DM_GAMES_DIR}" DM_GAMES_FIRST=1 "$VPY" "$R/graphics/tools/direct/direct_lib.py" "$W" "$W/direct.tmp" > "$W/direct_lib.log" 2>&1; drc=$?
fi
if [ "$drc" != 0 ]; then
  mkdir -p "$S2A_ROOTS/.failed" && cp "$W/direct_lib.log" "$S2A_ROOTS/.failed/${G}_direct_lib.log"; root_abort "$G" "$TAG"
  [ "$drc" = 3 ] && { echo "【黄】$G 直连缺口：$(sed -n 's/.*直连缺口：//p' "$S2A_ROOTS/.failed/${G}_direct_lib.log" | tail -1)"; exit 3; }
  # 带上直连库那行 🔴 的全文（含失败子命令名、退出码与原文末尾；direct_lib.py red）：作业层错误摘要取这一行（原先截 200 字，原文常被截掉，#1730）。
  #   按字节封顶 2000，iconv -c 去掉截断处的半个字
  F=$S2A_ROOTS/.failed/${G}_direct_lib.log; WHY=$(grep -a '🔴' "$F" | tail -1); [ -n "$WHY" ] || WHY=$(grep -a '直连库\]' "$F" | tail -1)
  echo "【红】$G 直连库出库失败（rc=${drc}，$(printf '%s' "$WHY" | head -c 2000 | iconv -c -f UTF-8 -t UTF-8)）"; exit 1
fi
# OpenGL 款离线 0 支（运行时拼 GLSL）：keys.tsv 为空、只有 gl/gl_shaders.tsv 表头，也算出了库（宿主首遇现编，direct_lib.py GL 段）；
#   Vulkan 款 RomFS 没有 SPIR-V（运行时生成，11-05 出包不起局后）同理：keys.tsv 为空、只有 vk/vk_shaders.tsv 表头（direct_lib.py Vulkan 段 --header-only）
if [ -s "$W/direct.tmp/keys.tsv" ] || [ -s "$W/direct.tmp/gl/gl_shaders.tsv" ] || [ -s "$W/direct.tmp/vk/vk_shaders.tsv" ] || [ -f "$W/direct.tmp/runtime_only.txt" ]; then mkdir -p "$W/shaders" && mv "$W/direct.tmp" "$W/shaders/direct"
else rm -rf "$W/direct.tmp"; fi
K=$(stage_deps_save "$W" direct "$KB" "$SELF") || { root_abort "$G" "$TAG"; exit 1; }
echo "$K" > "$W/.stage_direct"; ROOT_REPLAY='shaders/direct knowledge/direct direct_lib.log degrade.direct.tsv .stage_direct .stage_direct.deps' root_commit "$G" "$TAG" || exit 1
L=$(grep '直连库\]' "$(root_cur "$G")/direct_lib.log" | tail -1 | sed 's/^\[直连库\] //')
if [ -d "$(root_cur "$G")/shaders/direct" ]; then echo "${L}；库 $(du -sh "$(root_cur "$G")/shaders/direct" | cut -f1)，阶段 $((SECONDS - t0)) 秒，现行 → $(basename "$(root_cur "$G")")"
else echo "🟡 $G 不出直连库：${L}（现行 → $(basename "$(root_cur "$G")")）"; fi
