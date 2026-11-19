#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-cgr-inbound";mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
python3 "$repo/android/tests/vk/syncval_capability.py" "$S2A_VK_LAYER_DIR/VkLayer_khronos_validation.json"
headers="$repo/build/c883-ndk"
[ -f "$headers/vulkan/vulkan_core.h" ] || { echo '【红】缺真实 NDK 29 Vulkan 头；先由宿主出料' >&2; exit 2; }
for shader in compute_read_args.comp compute_read_readonly.comp compute_read_payload.comp compute_read_index.comp compute_read_image.comp compute_read_vertex.vert compute_read_buffer.vert compute_read_buffer.frag compute_multi_fragment.frag cgr_graphics_write.frag compute_read_texel.frag compute_read_image.frag solid_green.frag; do
    glslc --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
glslc --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
inc=(-I "$headers")
for dir in android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$dir"); done
src=("$repo/android/tests/vk/t_replay_compute_resources.c")
for file in graphics/nvn/replay/rp_draw.c graphics/nvn/replay/rp_buf.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$file"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" -Wl,--gc-sections -Wl,--wrap=vkGetDeviceProcAddr -Wl,--wrap=vkCreateInstance -Wl,--wrap=vkQueueSubmit -Wl,--wrap=vkWaitForFences -Wl,--wrap=vkQueueWaitIdle -Wl,--wrap=vkDeviceWaitIdle -Wl,--wrap=vkCreateRenderPass -Wl,--wrap=vkUpdateDescriptorSets -lvulkan -lpthread -lm -o "$out/resources"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
if [ "${S2A_CGR_NARROW:-1}" = 0 ]; then export S2A_CGR_EXPECT_LEGACY=1; unset S2A_CGR_EXPECT_NARROW || :
else export S2A_CGR_EXPECT_NARROW=1; unset S2A_CGR_EXPECT_LEGACY || :; fi
export S2A_INBOUND_NARROW=1 S2A_CGR_INBOUND_CHECK=1
failed=0
for dq in 0 1; do
 export S2A_COMPUTE_READ_DQ=$dq
 for kind in vsbuf fsbuf; do
  for suffix in enabled disabled omitted; do
   if [ "$suffix" = omitted ]; then export S2A_CGR_OMIT_FULL=1; else unset S2A_CGR_OMIT_FULL; fi
   if { [ "$kind" = renew ] || [ "$kind" = producer ]; } && [ "${S2A_CGR_NARROW:-1}" = 0 ]; then continue; fi
   arm="$kind-$suffix"; if [ "$suffix" = omitted ]; then arm="$kind-disabled"; fi; log="$out/dq$dq-$arm.log";set +e
   timeout 120 "$out/resources" "$out/shaders" "$arm" > "$log" 2>&1;rc=$?;set -e
   raw=$(grep -c 'SYNC-HAZARD-READ-AFTER-WRITE' "$log" || true)
   other=$(grep -E 'Validation Error|VUID-' "$log" | grep -vc 'SYNC-HAZARD-READ-AFTER-WRITE' || true)
   marker=$(grep '^COMPUTE_RESOURCE ' "$log" || true);scope=$(grep -E '^CGR_(SCOPE|MULTI|RENEW) ' "$log" || true)
   printf 'dq=%s arm=%s rc=%s RAW=%s other=%s %s %s\n' "$dq" "$arm" "$rc" "$raw" "$other" "$scope" "$marker"
   if [ "$rc" != 0 ] || [ "$other" != 0 ] || [ -z "$marker" ] || { [ "$suffix" != omitted ] && [ "$raw" != 0 ]; } || { [ "$suffix" = enabled ] && [ -z "$scope" ]; } || { [ "$suffix" = omitted ] && [ "$raw" -lt 1 ]; };then
    echo "【红】计算入向窄路 / 回退不符：$arm";tail -12 "$log";failed=1
   fi
  done
 done
done
[ "$failed" = 0 ] || exit 1
echo '【绿】计算写入向窄路 / 未跟踪回退及遗漏负控'
