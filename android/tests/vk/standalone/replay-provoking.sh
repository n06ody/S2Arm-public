#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# run.sh --replay-provoking：实际回放状态切换、平直着色读回、扩展与能力负控。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-replay-provoking"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
for shader in provoking.flat.vert provoking.flat.frag; do
    "${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
inc=()
for dir in build/c883-ndk android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn \
    graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$dir")
done
src=(android/tests/vk/t_replay_provoking.c graphics/nvn/replay/{rp_draw,rp_buf,rp_state,nvn_unimpl}.c
     android/native/nvn/{backend_vulkan,nvn_pipe_flight,nvn_vk_pipe_once,nvn_vk_format,nvn_vk_bcdec,nvn_vk_driver_cache}.c
     android/native/direct/{vk_device,gs_path,sha256}.c android/native/capprobe/capprobe_host.c
     graphics/nvn/gfx/nvn/{nvn_fmt_expand,nvn_tex}.c graphics/nvn/gfx/tex/nvn_deswizzle.c)
wrap=()
for fn in nvn_gpu_draw vkEnumerateDeviceExtensionProperties vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceProperties2 vkCreateDevice vkCreateGraphicsPipelines; do
    wrap+=(-Wl,--wrap="$fn")
done
cc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]/#/$repo/}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_VALIDATE_SYNC=1
failed=0
for dq in 0 1; do
    if [ "$dq" = 1 ]; then export S2A_REPLAY_INDIRECT_DQ=1; else unset S2A_REPLAY_INDIRECT_DQ; fi
    for control in 0 1 2 3; do
        log="$out/dq$dq-control$control.log"
        echo "provoking DQ=$dq control=$control"
        if ! timeout 90 "$out/test" "$out/shaders" "$control" > "$log" 2>&1 ||
           grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
            cat "$log"
            failed=1
        else
            grep '^provoking ' "$log"
            echo "provoking VUID=0 SYNC-HAZARD=0"
        fi
    done
done
exit "$failed"
