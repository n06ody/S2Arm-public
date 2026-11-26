#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# Default Vulkan gate picks up standalone/*.sh automatically.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-replay-ubo-ptr"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$S2A_VK_LAYER_DIR/VkLayer_khronos_validation.json" ]
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ]
glslc=${GLSLC:-glslc}
shaders="$repo/android/tests/vk/shaders"
"$glslc" --target-env=vulkan1.1 "$shaders/quad.vert" -o "$out/shaders/quad.vert.spv"
"$glslc" --target-env=vulkan1.1 "$shaders/replay_ubo_ptr.frag" -o "$out/shaders/reader.frag.spv"
"$glslc" --target-env=vulkan1.1 -DWRITABLE=1 "$shaders/replay_ubo_ptr.frag" -o "$out/shaders/writable.frag.spv"
"$glslc" --target-env=vulkan1.1 "$shaders/replay_ubo_ptr.comp" -o "$out/shaders/reader.comp.spv"
"$glslc" --target-env=vulkan1.1 -DPRODUCER=1 "$shaders/replay_ubo_ptr.comp" -o "$out/shaders/producer.comp.spv"
inc=()
for dir in build/c883-ndk android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn \
    graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$dir")
done
src=(
    android/tests/vk/t_replay_ubo_ptr.c
    graphics/nvn/replay/{rp_buf,rp_draw,rp_entry,rp_state,nvn_unimpl}.c
    graphics/nvn/gfx/nvn/{nvn_dirty,nvn_fmt_expand,nvn_tex}.c
    graphics/nvn/gfx/tex/nvn_deswizzle.c
    android/native/nvn/{backend_vulkan,nvn_pipe_flight,nvn_vk_pipe_once,nvn_vk_format,nvn_vk_bcdec,nvn_vk_driver_cache}.c
    android/native/direct/{vk_device,gs_path,sha256}.c
    android/native/capprobe/capprobe_host.c
)
cc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]/#/$repo/}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
if [ -z "${VK_LAYER_SETTINGS_PATH:-}" ]; then
    export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
else
    unset VK_LAYER_ENABLES
fi
failed=0
for dq in 0 1; do
    if [ "$dq" = 1 ]; then export S2A_TEST_NVK_DQ=1; else unset S2A_TEST_NVK_DQ; fi
    for arm in cpu inline gpu inline-gpu compute compute-inline repeat-rewrite writable; do
        log="$out/dq$dq-$arm.log"
        rc=0
        timeout 30 "$out/test" "$out/shaders" "$arm" > "$log" 2>&1 || rc=$?
        grep '^UBOA ' "$log" || true
        if [ "$rc" -ne 0 ] || ! grep -q '^device: llvmpipe' "$log" ||
           ! grep -Eq "^UBOA $arm .* bad=0$" "$log" || grep -Eq 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
            echo "【红】间接 SSBO 指针 DQ=$dq arm=$arm rc=$rc" >&2
            cat "$log" >&2
            failed=1
        fi
    done
done
[ "$failed" -eq 0 ] && echo 'PASS replay indirect SSBO pointer (2 recording modes)'
exit "$failed"
