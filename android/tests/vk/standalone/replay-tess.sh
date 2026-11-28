#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认发现 standalone/*.sh。单跑用 --replay-tess。
# 端到端宿主 Vulkan 细分着色器验证（TCS / TES / PATCHES / OP_SET_PATCH_SIZE + VVL syncval 0 错 + 缺特性负对照）。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-replay-tess"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
for shader in tess_patch.vert tess_patch.tesc tess_patch.tese tess_quad.tesc tess_quad.tese tess_patch.frag; do
    "${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
inc=(-I "$repo/build/c883-ndk")
for d in android/tests/vkhost android/tests/vk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn \
    graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=(
    "$repo/android/tests/vk/t_replay_tess.c"
    "$repo/graphics/nvn/replay/rp_draw.c"
    "$repo/graphics/nvn/replay/rp_buf.c"
    "$repo/graphics/nvn/replay/rp_state.c"
    "$repo/graphics/nvn/replay/nvn_unimpl.c"
    "$repo/android/native/nvn/backend_vulkan.c"
    "$repo/android/native/nvn/nvn_pipe_flight.c"
    "$repo/android/native/nvn/nvn_vk_pipe_once.c"
    "$repo/android/native/direct/vk_device.c"
    "$repo/android/native/direct/gs_path.c"
    "$repo/android/native/capprobe/capprobe_host.c"
    "$repo/android/native/nvn/nvn_vk_format.c"
    "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c"
    "$repo/android/native/nvn/nvn_vk_bcdec.c"
    "$repo/android/native/nvn/nvn_vk_driver_cache.c"
    "$repo/android/native/direct/sha256.c"
    "$repo/graphics/nvn/gfx/nvn/nvn_tex.c"
    "$repo/graphics/nvn/gfx/tex/nvn_deswizzle.c"
)
cc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" \
    -Wl,--gc-sections -Wl,--wrap=nvn_gpu_draw -Wl,--wrap=vkGetPhysicalDeviceFeatures -Wl,--wrap=vkGetPhysicalDeviceFeatures2 \
    -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"

for arm in 0 1; do
    log="$out/arm-$arm.log"
    rc=0
    timeout 120 "$out/test" "$out/shaders" "$arm" > "$log" 2>&1 || rc=$?
    if [ "$arm" -eq 1 ]; then echo '【负对照开始】'; fi
    cat "$log"
    if [ "$arm" -eq 1 ]; then echo '【负对照结束】'; fi
    if [ "$rc" -ne 0 ] || ! grep -q '^device: llvmpipe' "$log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
        echo "【红】细分着色器 Vulkan 验证失败 arm=$arm rc=$rc" >&2
        exit 1
    fi
done
grep -q '^replay-tess arm=0 .*tri_subdiv=ok quad_cp4=ok rejections=ok failures=0$' "$out/arm-0.log" || {
    echo '【红】arm=0 未输出预期通过摘要' >&2; exit 1;
}
grep -q '^replay-tess arm=1 missing_feature_unsupported=ok failures=0$' "$out/arm-1.log" || {
    echo '【红】arm=1 缺特性负对照未输出预期通过摘要' >&2; exit 1;
}
echo '【绿】细分着色器端到端（TCS+TES 真实细分 + OP_SET_PATCH_SIZE 3/4 + VVL syncval 0 错 + 缺特性负对照）全通过'
