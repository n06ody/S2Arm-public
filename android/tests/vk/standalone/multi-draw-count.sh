#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 执行，单跑用 --multi-draw-count。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
ndk="$repo/build/c883-ndk"
[ -f "$ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 Vulkan headers missing' >&2; exit 2; }
out="$repo/build/vk-multi-draw-count"
mkdir -p "$out/shaders"
for shader in quad.vert quad.frag count_range.comp; do
    "${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
inc=(-I "$ndk")
for dir in android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/tex \
           graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$dir")
done
sources=(android/tests/vk/t_replay_multi_draw_count.c
         graphics/nvn/replay/{rp_draw,rp_buf,rp_exec,rp_state,nvn_unimpl}.c
         android/native/nvn/{backend_vulkan,nvn_pipe_flight,nvn_vk_pipe_once,nvn_vk_format,nvn_vk_bcdec,nvn_vk_driver_cache}.c
         android/native/direct/{vk_device,gs_path,sha256}.c android/native/capprobe/capprobe_host.c
         graphics/nvn/gfx/nvn/{nvn_fmt_expand,nvn_tex}.c graphics/nvn/gfx/tex/nvn_deswizzle.c)
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections \
    -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${sources[@]/#/$repo/}" \
    -Wl,--gc-sections -Wl,--wrap=nvn_gpu_draw -Wl,--wrap=nvn_gpu_cmd_begin \
    -Wl,--wrap=vkEnumerateDeviceExtensionProperties -Wl,--wrap=vkGetDeviceProcAddr \
    -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR"
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1
failed=0
for dq in 0 1; do
    if [ "$dq" = 1 ]; then export S2A_REPLAY_INDIRECT_DQ=1; else unset S2A_REPLAY_INDIRECT_DQ; fi
    for ext in khr hidden; do
        args=()
        if [ "$ext" = hidden ]; then args=(--hide-khr); fi
        log="$out/dq-$dq-$ext.log"
        rc=0
        timeout 120 "$out/test" "$out/shaders" "${args[@]}" >"$log" 2>&1 || rc=$?
        cat "$log"
        if [ "$rc" != 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then failed=1; fi
    done
done
if [ "$failed" = 0 ]; then
    echo 'multi-draw-count PASS: KHR visible/hidden × DQ on/off, pixels and ordered submissions match, no VVL/SyncVal errors'
fi
exit "$failed"
