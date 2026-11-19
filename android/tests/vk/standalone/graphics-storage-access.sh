#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-graphics-storage";mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
python3 "$repo/android/tests/vk/syncval_capability.py" "$S2A_VK_LAYER_DIR/VkLayer_khronos_validation.json"
headers="$repo/build/c883-ndk"
[ -f "$headers/vulkan/vulkan_core.h" ] || { echo '【红】缺真实 NDK 29 Vulkan 头；先由宿主出料' >&2; exit 2; }
for shader in compute_read_args.comp compute_read_readonly.comp compute_read_payload.comp compute_read_index.comp compute_read_image.comp compute_read_vertex.vert compute_read_buffer.vert compute_read_buffer.frag compute_multi_fragment.frag cgr_graphics_write.frag inbound_storage_write.vert inbound_storage_read.vert inbound_storage_write2.vert inbound_storage_write2.frag inbound_storage_write_bind1.frag inbound_black.frag inbound_varying.frag compute_read_texel.frag compute_read_image.frag solid_green.frag; do
    glslc --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
glslc --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
inc=(-I "$headers")
for dir in android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$dir"); done
src=("$repo/android/tests/vk/t_replay_graphics_storage.c")
for file in graphics/nvn/replay/rp_draw.c graphics/nvn/replay/rp_buf.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$file"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" -Wl,--gc-sections -Wl,--wrap=vkGetDeviceProcAddr -Wl,--wrap=vkCreateInstance -Wl,--wrap=vkCreateDevice -Wl,--wrap=vkQueueSubmit -Wl,--wrap=vkWaitForFences -Wl,--wrap=vkQueueWaitIdle -Wl,--wrap=vkDeviceWaitIdle -Wl,--wrap=vkCreateRenderPass -Wl,--wrap=vkUpdateDescriptorSets -lvulkan -lpthread -lm -o "$out/resources"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
export S2A_CGR_REMOVE_INCOMING=1
failed=0
modes="0 1 2 3 4 5 6 7 8 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26"
for xseg in 0 1;do
 if [ "$xseg" = 1 ]; then export S2A_GFX_XSEG=1; else unset S2A_GFX_XSEG; fi
 for dq in 0 1;do
 export S2A_COMPUTE_READ_DQ=$dq
 for source in VS FS;do
  export S2A_GFX_ACCESS_SOURCE=$source
  for dest in VS FS;do
   export S2A_GFX_ACCESS_DEST=$dest
   if [ "$dest" = VS ];then stage=VERTEX;else stage=FRAGMENT;fi
   for mode in $modes;do
    if [ "$xseg" = 1 ] && { [ "$mode" = 14 ] || [ "$mode" = 16 ] || [ "$mode" -ge 17 ]; };then continue;fi
    if [ "$mode" = 16 ] && [ "$source" != VS ];then continue;fi
    if [ "$mode" -ge 22 ] && { [ "$source" != VS ] || [ "$dest" != VS ]; };then continue;fi
    export S2A_GFX_ACCESS_MODE=$mode
    if [ "$mode" = 4 ] || [ "$mode" = 5 ] || [ "$mode" = 6 ] || [ "$mode" = 14 ] || [ "$mode" = 16 ] || [ "$mode" -ge 17 ]; then export S2A_GFX_NARROW=1; else export S2A_GFX_NARROW=0; fi
    if [ "$mode" -ge 17 ];then export S2A_INBOUND_DIAG=1;else unset S2A_INBOUND_DIAG;fi
    if [ "$mode" = 12 ] || [ "$mode" = 13 ] || [ "$mode" = 14 ] || [ "$mode" = 16 ] || { [ "$mode" -ge 17 ] && [ "$mode" != 20 ]; }; then export S2A_INBOUND_FINAL=1 S2A_INBOUND_NARROW=1; unset S2A_CGR_REMOVE_INCOMING S2A_FORCE_NARROW_INCOMING;
    elif [ "$mode" = 15 ] || [ "$mode" = 20 ]; then export S2A_INBOUND_FINAL=1 S2A_INBOUND_NARROW=0; unset S2A_CGR_REMOVE_INCOMING S2A_FORCE_NARROW_INCOMING;
    else unset S2A_INBOUND_FINAL S2A_FORCE_NARROW_INCOMING S2A_INBOUND_NARROW; if [ "$mode" = 7 ]; then unset S2A_CGR_REMOVE_INCOMING; else export S2A_CGR_REMOVE_INCOMING=1; fi; fi
    log="$out/xseg${xseg}-dq${dq}-${source}-${dest}-${mode}.log";set +e
    timeout 120 "$out/resources" "$out/shaders" "$source" "$dest" > "$log" 2>&1;rc=$?;set -e
    raw=$(grep -c 'SYNC-HAZARD-READ-AFTER-WRITE' "$log" || true)
    other=$(grep -E 'Validation Error|VUID-' "$log" | grep -vc 'SYNC-HAZARD-READ-AFTER-WRITE' || true)
    marker=$(grep '^GFX_STORAGE ' "$log" || true)
    printf 'xseg=%s dq=%s src=%s dst=%s mode=%s rc=%s RAW=%s other=%s %s\n' "$xseg" "$dq" "$source" "$dest" "$mode" "$rc" "$raw" "$other" "$marker"
    if [ "$rc" != 0 ] || [ "$other" != 0 ] || [ -z "$marker" ] || { { [ "$mode" = 1 ] || [ "$mode" = 4 ] || [ "$mode" = 7 ] || [ "$mode" = 12 ] || [ "$mode" = 14 ] || [ "$mode" = 15 ] || [ "$mode" = 16 ] || [ "$mode" -ge 17 ]; } && [ "$raw" != 0 ]; } || { [ "$mode" != 1 ] && [ "$mode" != 4 ] && [ "$mode" != 7 ] && [ "$mode" != 12 ] && [ "$mode" != 14 ] && [ "$mode" != 15 ] && [ "$mode" != 16 ] && [ "$mode" -lt 17 ] && [ "$raw" -lt 1 ]; };then
     echo 'graphics storage detection control failed';tail -15 "$log";failed=1
    elif [ "$mode" != 1 ] && [ "$mode" != 4 ] && [ "$mode" != 7 ] && [ "$mode" != 12 ] && [ "$mode" != 14 ] && [ "$mode" != 15 ] && [ "$mode" != 16 ] && [ "$mode" -lt 17 ] && ! { grep -q "Shader stage VK_SHADER_STAGE_${stage}_BIT reads" "$log" || grep -q "VK_ACCESS_2_SHADER_STORAGE_READ_BIT accesses at VK_PIPELINE_STAGE_2_${stage}_SHADER_BIT" "$log"; };then
     echo 'graphics storage target stage not detected';failed=1
    fi
   done
  done
 done
done
done
[ "$failed" = 0 ] || exit 1
echo 'graphics storage stage detection controls passed'
