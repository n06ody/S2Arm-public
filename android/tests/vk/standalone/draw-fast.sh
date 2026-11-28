#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --draw-fast
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-draw-fast"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
for shader in ubo_dyn.frag feedback.frag feedback_pool_dyn.frag pushc.comp; do
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done

inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
common_src=()
for f in android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do
    common_src+=("$repo/$f")
done

build_bin() {
    local bv=$1 bin=$2
    cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
        -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
        -I "$repo/android/native/nvn" "${inc[@]}" \
        "$repo/android/tests/vk/t_draw_fast.c" "$repo/graphics/nvn/replay/nvn_unimpl.c" "$bv" "${common_src[@]}" \
        -Wl,--wrap=vkGetDeviceProcAddr -Wl,--gc-sections -lvulkan -lpthread -lm -o "$bin"
}

build_bin "$repo/android/native/nvn/backend_vulkan.c" "$out/test"

export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD"
export VK_LAYER_PATH="$S2A_VK_LAYER_DIR"
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"

failed=0
run_cfg() {
    local bin=$1 dq=$2 opt=$3 flag=$4 tag=$5 combo=${6:-0}
    local cases=4
    if [ "$combo" != 0 ]; then cases=1; fi
    if [ "$combo" = 4 ]; then cases=2; fi
    local log="$out/${tag}.log"
    set +e
    timeout 120 "$bin" "$out/shaders" "$dq" "$opt" "$flag" "$combo" 2>&1 | tee "$log"
    local rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
       ! grep -q "draw-fast dq=${dq} opt=${opt} flag=${flag} combo=${combo} .* cases=$cases failures=0" "$log"; then
        echo "【红】draw-fast ${tag}: rc=${rc} or validation failure" >&2
        failed=1
    fi
}

run_cfg "$out/test" 0 1 0 "dq0-opt1-f0"
run_cfg "$out/test" 0 1 1 "dq0-opt1-f1"
run_cfg "$out/test" 1 1 0 "dq1-opt1-f0"
run_cfg "$out/test" 1 1 1 "dq1-opt1-f1"
run_cfg "$out/test" 0 0 1 "dq0-opt0-f1"
run_cfg "$out/test" 1 0 1 "dq1-opt0-f1"
for dq in 0 1; do
    run_cfg "$out/test" "$dq" 1 1 "dq$dq-combo" 1
    run_cfg "$out/test" "$dq" 1 1 "dq$dq-combo-hot-hash" 2
    run_cfg "$out/test" "$dq" 1 1 "dq$dq-combo-pools" 3
done
run_cfg "$out/test" 1 1 1 "dq1-combo-pools-dq-opt" 4

# 变异负对照：破坏 prepare_bind_layouts 脏位检查（去掉 !(c->layout_dirty & lbit)），
# 确认切换至反馈环贴图或附件布局贴图时用例与 Khronos 校验层能立刻拦截（不回流主输出）。
echo "【负对照开始】draw-fast：破坏 layout_dirty 脏位检查（预期校验层与像素断言拦截，不回流主输出）"
mut="$out/mut"
mkdir -p "$mut"
python3 - "$repo/android/native/nvn/backend_vulkan.c" "$mut/backend_vulkan.c" <<'PY'
import sys
src = open(sys.argv[1], 'r', encoding='utf-8').read()
old = 'const int draw_fast_layout_skip = g.draw_fast && !(c->layout_dirty & lbit) &&'
new = 'const int draw_fast_layout_skip = g.draw_fast &&'
assert old in src, 'draw-fast mutation anchor missing'
open(sys.argv[2], 'w', encoding='utf-8').write(src.replace(old, new, 1))
PY
build_bin "$mut/backend_vulkan.c" "$mut/test"
set +e
timeout 120 "$mut/test" "$out/shaders" 0 1 1 > "$mut/run.log" 2>&1
mut_rc=$?
set -e
mut_vvl=$(grep -cE 'Validation Error|VUID-' "$mut/run.log" || true)
mut_fail=$(grep -c 'FAIL c' "$mut/run.log" || true)
if [ "$mut_rc" -eq 0 ] || [ "$mut_vvl" -eq 0 ]; then
    echo "【红】draw-fast 变异负对照未按预期拦截（rc=${mut_rc}, fail=${mut_fail}, vvl=${mut_vvl}）" >&2
    failed=1
else
    echo "draw-fast 变异负对照 PASS：已拦截（rc=${mut_rc}, 断言命中=${mut_fail}, 校验命中=${mut_vvl}）"
fi
echo "【负对照结束】"

echo "【负对照开始】draw-fast：复用命令段时保留上一段的布局索引"
python3 - "$repo/android/native/nvn/backend_vulkan.c" "$mut/backend_vulkan.c" <<'PY'
import sys
from pathlib import Path
source, mutant = map(Path, sys.argv[1:])
text = source.read_text()
anchor = 'if (c->lay_ht) memset(c->lay_ht, 0, (size_t)c->lay_ht_cap * sizeof *c->lay_ht);'
assert text.count(anchor) == 1, 'pooled layout hash mutation anchor missing'
mutant.write_text(text.replace(anchor, '/* keep stale layout buckets */'))
PY
build_bin "$mut/backend_vulkan.c" "$mut/test-lay-ht"
set +e
timeout 120 "$mut/test-lay-ht" "$out/shaders" 0 1 1 3 > "$mut/lay-ht.log" 2>&1
hash_rc=$?
set -e
hash_vvl=$(grep -cE 'expects VkImage .* to be in layout .*current layout is' "$mut/lay-ht.log" || true)
if [ "$hash_vvl" -eq 0 ]; then
    echo "【红】draw-fast 布局索引负对照未抓到错误布局（rc=$hash_rc）" >&2
    tail -30 "$mut/lay-ht.log" >&2
    failed=1
else
    echo "draw-fast 布局索引负对照 PASS（rc=$hash_rc，布局校验命中=$hash_vvl）"
fi
echo "【负对照结束】"

exit "$failed"
