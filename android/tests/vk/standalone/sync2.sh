#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --<名>
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-sync2"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
for shader in solid_green.frag ubo_dyn.frag feedback.frag compute_read_payload.comp compute_read_vertex.vert compute_read_buffer.frag; do
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done

inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_sync2.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do
    src+=("$repo/$f")
done
wrap=()
for fn in vkEnumerateDeviceExtensionProperties vkCreateDevice vkGetDeviceProcAddr vkCmdPipelineBarrier; do
    wrap+=(-Wl,--wrap="$fn")
done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"

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
    local dq=$1 opt=$2 flag=$3 hide=$4
    local log="$out/dq${dq}-opt${opt}-f${flag}-h${hide}.log"
    set +e
    timeout 120 "$out/test" "$out/shaders" "$dq" "$opt" "$flag" "$hide" 2>&1 | tee "$log"
    local rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-' "$log" ||
       ! grep -q "sync2 dq=${dq} opt=${opt} flag=${flag} hide=${hide} .* cases=6 failures=0" "$log"; then
        echo "【红】sync2 dq=${dq} opt=${opt} flag=${flag} hide=${hide}: rc=${rc} or validation failure" >&2
        failed=1
    fi
}

run_cfg 0 0 1 0
run_cfg 0 1 1 0
run_cfg 1 0 1 0
run_cfg 1 1 1 0
run_cfg 0 0 0 0
run_cfg 0 0 1 1

# 变异负对照：破坏 sync2 UNDEFINED->GENERAL 的 srcAccessMask=NONE 约束（改写成 TRANSFER_WRITE_BIT_KHR），
# 确认 Khronos 校验层真的在检查 vkCmdPipelineBarrier2KHR（命中 03915 规则）。
echo "【负对照开始】sync2：破坏 NONE 源阶段访问掩码约束（预期校验层拦截，不回流主输出）"
mut="$out/mut"
mkdir -p "$mut"
python3 - "$repo/android/native/nvn/backend_vulkan.c" "$mut/backend_vulkan.c" <<'PY'
import sys
src = open(sys.argv[1], 'r', encoding='utf-8').read()
old = 'const VkAccessFlags2 s2_init_src_acc = VK_ACCESS_2_NONE_KHR;'
new = 'const VkAccessFlags2 s2_init_src_acc = VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR;'
assert old in src, 'sync2 mutation anchor missing'
open(sys.argv[2], 'w', encoding='utf-8').write(src.replace(old, new, 1))
PY
mut_src=("$repo/android/tests/vk/t_sync2.c" "$repo/graphics/nvn/replay/nvn_unimpl.c" "$mut/backend_vulkan.c")
for f in android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do
    mut_src+=("$repo/$f")
done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    -I "$repo/android/native/nvn" "${inc[@]}" "${mut_src[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$mut/test"
set +e
timeout 120 "$mut/test" "$out/shaders" 0 0 1 0 > "$mut/run.log" 2>&1
mut_rc=$?
set -e
mut_vvl=$(grep -cE 'Validation Error|VUID-' "$mut/run.log" || true)
mut_rule=$(grep -c '03915' "$mut/run.log" || true)
if [ "$mut_vvl" -eq 0 ] || [ "$mut_rule" -eq 0 ] || [ "$mut_rc" -eq 0 ]; then
    echo "【红】sync2 变异负对照未按预期拦截（rc=${mut_rc}, hits=${mut_vvl}, rule03915=${mut_rule}）" >&2
    failed=1
else
    echo "sync2 变异负对照 PASS：已拦截（rc=${mut_rc}, 校验命中=${mut_vvl}, 规则03915=${mut_rule}）"
fi
echo "【负对照结束】"

exit "$failed"
