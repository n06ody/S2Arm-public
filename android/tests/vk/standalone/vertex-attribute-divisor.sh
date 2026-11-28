#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-vertex-attribute-divisor"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
for shader in divisor_stripes.vert quad.frag; do
    "${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn \
         graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=(android/tests/vk/t_vertex_attribute_divisor.c graphics/nvn/replay/nvn_unimpl.c
     android/native/nvn/{backend_vulkan,nvn_pipe_flight,nvn_vk_pipe_once,nvn_vk_format,nvn_vk_bcdec,nvn_vk_driver_cache}.c
     android/native/direct/{vk_device,gs_path,sha256}.c android/native/capprobe/capprobe_host.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c)
wrap=()
for fn in vkCreateImageView vkCreateFramebuffer vkCmdBeginRenderPass vkUpdateDescriptorSets vkCmdBindDescriptorSets \
          vkCmdDraw vkCmdCopyImage vkGetDeviceProcAddr vkDestroyImageView vkWaitForFences vkCreateDevice \
          vkEnumerateDeviceExtensionProperties vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceProperties2; do
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
for dq in 0 1; do
    for vids in 0 1; do
        for control in 0 1 2 3 4 5; do
            log="$out/dq$dq-vids$vids-control$control.log"
            if ! timeout 90 env "debug.s2a.nvk_vertex_input=$vids" "$out/test" "$out/shaders" "$control" "$dq" > "$log" 2>&1 ||
               ! grep -q "^vertex-divisor VIDS=$vids canonical-psos=" "$log" ||
               grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
                cat "$log"
                failed=1
            else
                grep '^vertex-divisor ' "$log"
                printf 'vertex-divisor control=%s DQ=%s VIDS=%s VUID=0 SyncVal=0\n' "$control" "$dq" "$vids"
            fi
        done
    done
done
exit "$failed"
