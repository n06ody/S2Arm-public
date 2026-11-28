#!/usr/bin/env bash
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑，可单跑 --pipe-cache-ctl。
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="${S2A_PIPE_CACHE_CTL_OUT:-$repo/build/vk-pipe-cache-ctl}"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/tri3.vert.spv"
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/solid_green.frag" -o "$out/shaders/solid_green.frag.spv"
cat > "$out/shaders/ubo.frag" <<'GLSL'
#version 450
layout(set = 0, binding = 0) uniform Color { vec4 value; } color;
layout(location = 0) out vec4 rgba;
void main() { rgba = color.value; }
GLSL
"$glslc" --target-env=vulkan1.1 "$out/shaders/ubo.frag" -o "$out/shaders/ubo.frag.spv"

inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=(
    "$repo/android/tests/vk/t_pipe_cache_ctl.c"
    "$repo/graphics/nvn/replay/nvn_unimpl.c"
    "$repo/android/native/nvn/nvn_vk_pipe_demand.c"
    "$repo/android/native/nvn/nvn_pipe_pend.c"
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
)
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]}" \
    -Wl,--wrap=vkEnumerateDeviceExtensionProperties \
    -Wl,--wrap=vkGetPhysicalDeviceFeatures2 \
    -Wl,--wrap=vkCreateDevice \
    -Wl,--wrap=vkCreateGraphicsPipelines \
    -Wl,--wrap=vkGetDeviceProcAddr \
    -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"

export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
unset VK_LAYER_ENABLES
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"

for mode in on off hide-ext hide-feat no-plc serial-busy combo combo-dq; do
    cdir="$out/cache-$mode"
    rm -rf "$cdir" && mkdir -p "$cdir"
    log="$out/$mode.log"
    timeout 60 "$out/test" "$out/shaders" "$cdir" "$mode" > "$log" 2>&1 || { cat "$log"; exit 1; }
    if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
        cat "$log"
        exit 1
    fi
    grep '^pipe-cache-ctl ' "$log"
done

# 负对照（撤退验证）：开关关闭却按启用期望验收，必须报红退出非零
cdir="$out/cache-withdraw"
rm -rf "$cdir" && mkdir -p "$cdir"
log="$out/withdraw.log"
echo "【负对照开始】"
set +e
timeout 60 "$out/test" "$out/shaders" "$cdir" withdraw > "$log" 2>&1
rc=$?
set -e
grep '^pipe-cache-ctl \|【红】' "$log" || true
echo "【负对照结束】"
if [ "$rc" -ne 1 ] || ! grep -q 'cache control missed expectation:' "$log" ||
   grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
    cat "$log"
    echo "【红】withdraw negative control unexpectedly succeeded or triggered VVL (rc=$rc)" >&2
    exit 1
fi
echo "PASS: pipe-cache-ctl negative withdraw rejected as expected (rc=$rc)"
