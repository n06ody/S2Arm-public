#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认发现 standalone/*.sh。单跑用 --replay-draw-caps。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-replay-draw-caps"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
for shader in tri3.vert tri_solid.frag; do
    "${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
inc=()
for dir in build/c883-ndk android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn \
    graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$dir")
done
src=(
    android/tests/vk/t_replay_draw_caps.c
    graphics/nvn/replay/{rp_buf,rp_draw,rp_state,nvn_unimpl}.c
    graphics/nvn/gfx/nvn/{nvn_fmt_expand,nvn_tex}.c
    graphics/nvn/gfx/tex/nvn_deswizzle.c
    android/native/nvn/{backend_vulkan,nvn_pipe_flight,nvn_vk_pipe_once,nvn_vk_format,nvn_vk_bcdec,nvn_vk_driver_cache}.c
    android/native/direct/{vk_device,gs_path,sha256}.c
    android/native/capprobe/capprobe_host.c
)
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]/#/$repo/}" -Wl,--gc-sections -Wl,--wrap=nvn_gpu_draw \
    -Wl,--wrap=vkGetPhysicalDeviceFormatProperties -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
rc=0
timeout 90 "$out/test" "$out/shaders" > "$out/run.log" 2>&1 || rc=$?
cat "$out/run.log"
if [ "$rc" -ne 0 ] || ! grep -q '^device: llvmpipe' "$out/run.log" || \
   grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/run.log"; then
    echo "【红】draw gap Vulkan rc=$rc" >&2
    exit 1
fi
rc=0
S2A_DRAW_CAPS_WRONG_VERTEX=1 timeout 90 "$out/test" "$out/shaders" > "$out/wrong-vertex.log" 2>&1 || rc=$?
if [ "$rc" -ne 1 ] || ! grep -Eq '^DRAW_CAPS cpu-i2f pixels=[0-9]+/4096 draws=1 skips=0$' "$out/wrong-vertex.log" || \
   grep -q '^DRAW_CAPS cpu-i2f pixels=4096/4096' "$out/wrong-vertex.log"; then
    cat "$out/wrong-vertex.log"
    exit 1
fi
if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/wrong-vertex.log"; then
    cat "$out/wrong-vertex.log"
    exit 1
fi
grep -E '^DRAW_CAPS (cpu-i2f|failures=)' "$out/wrong-vertex.log"
