#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-separate-ds-layouts"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
"${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
"${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/solid_blue.frag" -o "$out/shaders/solid_blue.frag.spv"

inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn \
         graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=(android/tests/vk/t_separate_ds_layouts.c graphics/nvn/replay/nvn_unimpl.c
     android/native/nvn/{backend_vulkan,nvn_pipe_flight,nvn_vk_pipe_once,nvn_vk_format,nvn_vk_bcdec,nvn_vk_driver_cache}.c
     android/native/direct/{vk_device,gs_path,sha256}.c android/native/capprobe/capprobe_host.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c)
wrap=()
for fn in vkCreateImageView vkCreateFramebuffer vkCmdBeginRenderPass vkUpdateDescriptorSets vkCmdBindDescriptorSets \
          vkCmdDraw vkCmdCopyImage vkGetDeviceProcAddr vkDestroyImageView vkWaitForFences vkCreateRenderPass \
          vkCmdPipelineBarrier vkCreateDevice vkEnumerateDeviceExtensionProperties vkGetPhysicalDeviceFeatures2; do
    wrap+=(-Wl,--wrap="$fn")
done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]/#/$repo/}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"

export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
failed=0
run_case() {
    local dq=$1 opt=$2 control=$3 sync2=$4
    local log="$out/dq$dq-opt$opt-control$control-sync2$sync2.log"
    if ! timeout 90 env "S2A_SEPARATE_DS_SYNC2=$sync2" "$out/test" "$out/shaders" "$control" "$dq" "$opt" > "$log" 2>&1 ||
       grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
        cat "$log"
        failed=1
    else
        grep '^separate-ds ' "$log"
        printf 'separate-ds control=%s DQ=%s OPT=%s Sync2=%s VUID=0 SyncVal=0\n' "$control" "$dq" "$opt" "$sync2"
    fi
}
for dq in 0 1; do
    for opt in 0 1; do
        for control in 0 1 2 3 4 5; do
            run_case "$dq" "$opt" "$control" 0
        done
        run_case "$dq" "$opt" 4 1
        run_case "$dq" "$opt" 5 1
    done
done
exit "$failed"
