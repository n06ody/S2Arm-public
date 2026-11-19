#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-inbound-indirect";mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2;exit 2; }
for shader in inbound_indirect_write.vert inbound_indirect_write.frag inbound_black.frag solid_green.frag;do glslc --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv";done
glslc --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
inc=();for d in build/c883-ndk android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include;do inc+=(-I "$repo/$d");done
src=("$repo/android/tests/vk/t_inbound_indirect.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c;do src+=("$repo/$f");done
wrap=();for fn in vkCreateRenderPass vkCreateImageView vkCreateFramebuffer vkCmdBeginRenderPass vkCmdEndRenderPass vkUpdateDescriptorSets vkCmdBindDescriptorSets vkCmdDraw vkCmdCopyImage vkGetDeviceProcAddr vkDestroyImageView vkWaitForFences;do wrap+=(-Wl,--wrap="$fn");done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
failed=0
for dq in 0 1; do
 for src in 0 1; do
  for xseg in 0 1; do
   for omit in 0 1 2; do
    log="$out/dq${dq}-src${src}-xseg${xseg}-omit${omit}.log";set +e
    timeout 120 "$out/test" "$out/shaders" "$dq" "$src" "$omit" "$xseg" > "$log" 2>&1;rc=$?;set -e
    raw=$(grep -c 'SYNC-HAZARD-READ-AFTER-WRITE' "$log" || true)
    other=$(grep -E 'Validation Error|VUID-' "$log" | grep -vc 'SYNC-HAZARD-READ-AFTER-WRITE' || true)
    marker=$(grep '^indirect ' "$log" || true)
    printf '%s rc=%s RAW=%s other=%s\n' "$marker" "$rc" "$raw" "$other"
    if [ "$rc" != 0 ] || [ "$other" != 0 ] || [ -z "$marker" ] || { [ "$omit" = 0 ] && [ "$raw" != 0 ]; } || { [ "$omit" != 0 ] && [ "$raw" -lt 1 ]; };then
     echo '【红】间接 stage 检测对照不符合预期';tail -20 "$log";failed=1
    fi
   done
  done
 done
done
[ "$failed" = 0 ] || exit 1
echo '【绿】去入向依赖：图形着色器写→间接命令读'
