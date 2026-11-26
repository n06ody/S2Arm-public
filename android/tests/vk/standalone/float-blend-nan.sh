#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# android/tests/vk/run.sh --float-blend-nan, also included in the full suite.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
[ -z "${1:-}" ] || exit 64
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
ndk_headers="$repo/build/c883-ndk"
[ -f "$ndk_headers/vulkan/vulkan_core.h" ] || { echo 'NDK 29 Vulkan headers not staged' >&2; exit 2; }
out="$repo/build/vk-float-blend-nan"
mkdir -p "$out/shaders"
for shader in libpack_depth.vert float_blend_nan.frag; do
    "${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
    spirv-val --target-env vulkan1.1 "$out/shaders/$shader.spv"
done
inc=(-I "$ndk_headers")
for dir in android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/tex \
           graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$dir")
done
sources=(android/tests/vk/t_float_blend_nan.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c
         android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c
         android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c android/native/nvn/nvn_vk_bcdec.c
         android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c graphics/nvn/replay/nvn_unimpl.c
         graphics/nvn/gfx/nvn/nvn_fmt_expand.c graphics/nvn/gfx/nvn/nvn_tex.c graphics/nvn/gfx/tex/nvn_deswizzle.c)
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections \
    -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${sources[@]/#/$repo/}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR"
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
no_validation_errors() {
    grep -q '^device: llvmpipe' "$1" && ! grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$1"
}
log="$out/guard.log"
rc=0
timeout 120 "$out/test" "$out/shaders" > "$log" 2>&1 || rc=$?
cat "$log"
[ "$rc" = 0 ] && no_validation_errors "$log" && grep -qx 'float-blend-nan: failures=0' "$log" || exit 1
log="$out/original-module.log"
rc=0
timeout 120 "$out/test" "$out/shaders" --original-module > "$log" 2>&1 || rc=$?
if [ "$rc" != 1 ] || ! no_validation_errors "$log" ||
   ! grep -qx 'float-blend-nan: failures=4' "$log" ||
   [ "$(grep -c 'zero-alpha RGB preservation arm .*: 0/8' "$log")" != 4 ]; then
    cat "$log"; exit 1
fi
echo '【负对照开始】'
grep 'zero-alpha RGB preservation' "$log"
echo '【负对照结束】'
echo '【绿】RGBA16F 透明源、有限源、非零 A、MRT 与预热命中，Vulkan 校验 0 条'
