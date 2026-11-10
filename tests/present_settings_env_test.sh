#!/bin/bash
# 门禁只看：graphics/nvn/tests/t_present_settings.m graphics/nvn/tests/t_present_settings.sh tests/present_settings_test.sh tests/present_settings_env_test.sh
# 门禁预算：600
# 门禁显卡：独占
# t_present_settings 的宿主显示环境探针（屏睡/无活动屏 → WindowServer 撤 NSWindowOcclusionStateVisible
#   位，与焦点无关——无人值守跑门禁必中，11-11 实测双屏 Display Asleep、CGGetActiveDisplayList=0 时
#   18 格全死在该检查）：
#   ① S2A_PS_ENV=sleep → 建窗前早探针退 77、打「未证」；② awake+S2A_PS_WINDOW=occluded → 探针过、
#   原检查照旧红退 1；③ offspace → 500ms 晚探针退 77；④ .sh 全程 sleep → 18 格全未证 rc=0、
#   跑前清 edge 图使跨跑残留假图不复活
set -u
cd "$(dirname "$0")/.."
root=$PWD
# 本测要完整跑 .sh（会清洗 edge 图、逐格写 run.log）：门禁单测是并行步，
# 用共享 build/tests/present-settings/ 会和并跑的 present_settings_test 互踩
# （11-11 h-205：④ 的清洗删了真跑 edge-*.bgra、未证 run.log 盖了真日志、
#   另一格日志截成二进制被 FAIL-grep 判红）——全程用 PRESENT_SETTINGS_BUILD 私目录
export PRESENT_SETTINGS_BUILD=$root/build/tests/present-settings-env
out=$PRESENT_SETTINGS_BUILD
bin=$out/t_present_settings
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fails=0
ck() { local n="$1"; shift; if "$@"; then echo "PASS $n"; else echo "FAIL $n"; fails=$((fails+1)); fi; }

PRESENT_SETTINGS_BUILD_ONLY=1 bash tests/present_settings_test.sh >"$T/build.log" 2>&1
ck "build-only 编译通过" test -x "$bin"
[ -x "$bin" ] || { echo "编译失败，日志 $T/build.log"; exit 1; }

cell_env=(env S2A_GFX=direct S2A_GAME_DATA="$T" S2A_DOCS="$T" S2A_GAME_NAME="psenv" \
    S2A_INPUT_GUIDE_PREFS="$T/ig.json" DM_PRESENT_LINK=0 S2A_WIDEN_NARROW=1 \
    S2A_ANTI_ALIASING=None S2A_SCALING_FILTER=Nearest S2A_RES_SCALE=1 S2A_FPS=0)
run() { mkdir -p "$T/$1"; local d="$1"; shift; "${cell_env[@]}" "$@" "$bin" "$T/$d" >"$T/$d/log" 2>&1; }

run r1 S2A_PS_ENV=sleep; s=$?
ck "① 缝 sleep → 退 77（未证）" test "$s" = 77
ck "① 打「未证：宿主显示休眠」" grep -q '未证：宿主显示休眠' "$T/r1/log"
ck "① 未建窗口（无 CAMetalLayer 行）" bash -c '! grep -q CAMetalLayer "$1"' _ "$T/r1/log"

run r2 S2A_PS_ENV=awake S2A_PS_WINDOW=occluded; s=$?
ck "② 钉醒+钉真不可见 → 退 1（红路仍通）" test "$s" = 1
ck "② 红在「FAIL production window is visible」" grep -q 'FAIL production window is visible' "$T/r2/log"

run r3 S2A_PS_ENV=offspace; s=$?
ck "③ 缝 offspace → 退 77（晚探针）" test "$s" = 77
ck "③ 打「未证」" grep -q '未证' "$T/r3/log"

# ④ .sh 端到端：先塞同内容假 edge 图（同字节 → 没清也没跳时会判 not-distinct 红），再全程 sleep
for d in "$out"/mtl3-None-Nearest "$out"/mtl3-SmaaMedium-Nearest "$out"/mtl3-SmaaHigh-Nearest \
         "$out"/mtl4-None-Nearest "$out"/mtl4-SmaaMedium-Nearest "$out"/mtl4-SmaaHigh-Nearest \
         "$out"/mtl3-None-Bilinear "$out"/mtl4-None-Bilinear "$out"/mtl3-None-MetalFX "$out"/mtl4-None-MetalFX \
         "$out"/mtl3-SmaaMedium-Bilinear "$out"/mtl4-SmaaMedium-Bilinear "$out"/mtl3-SmaaHigh-Bilinear "$out"/mtl4-SmaaHigh-Bilinear \
         "$out"/mtl3-SmaaMedium-MetalFX "$out"/mtl4-SmaaMedium-MetalFX "$out"/mtl3-SmaaHigh-MetalFX "$out"/mtl4-SmaaHigh-MetalFX; do
    mkdir -p "$d"; printf x > "$d/edge-native.bgra"; printf x > "$d/edge-scaled.bgra"
done
S2A_PS_ENV=sleep bash tests/present_settings_test.sh >"$T/sh.log" 2>&1; s=$?
ck "④ .sh 全程未证 → rc=0" test "$s" = 0
ck "④ 18 格全 UNPROVEN" bash -c '[ "$(grep -c "^UNPROVEN " "$1")" = 18 ]' _ "$T/sh.log"
ck "④ 尾行单列未证数" grep -q '18 cells 未证' "$T/sh.log"
ck "④ 无 FAIL 行（假图不复活成 distinctions 红）" bash -c '! grep -q "^FAIL" "$1"' _ "$T/sh.log"
ck "④ 假 edge 图已被清掉" bash -c '! find "$1" -name "edge-*.bgra" -size +0 -print -quit | grep -q .' _ "$out"

echo "present_settings_env_test：$((13-fails))/13 通过"
exit $((fails>0))
