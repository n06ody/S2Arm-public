#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
set -euo pipefail
variant=${1:-controls}
[ "$variant" = controls ] || [ "$variant" = red ] || [ "$variant" = ceiling ] || { echo 'unknown variant' >&2; exit 2; }
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-external-dependency"; mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/feedback.frag" -o "$out/shaders/feedback.frag.spv"
glslc --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/feedback_depth.frag" -o "$out/shaders/feedback_depth.frag.spv"
glslc --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/solid_green.frag" -o "$out/shaders/solid_green.frag.spv"
glslc --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
inc=();for d in build/c883-ndk android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include;do inc+=(-I "$repo/$d");done
src=("$repo/android/tests/vk/t_external_dependency.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c;do src+=("$repo/$f");done
wrap=();for fn in nvn_plat_flag vkCreateRenderPass vkCreateImageView vkCreateFramebuffer vkCmdBeginRenderPass vkUpdateDescriptorSets vkCmdBindDescriptorSets vkCmdDraw vkCmdCopyImage vkGetDeviceProcAddr vkDestroyImageView vkWaitForFences;do wrap+=(-Wl,--wrap="$fn");done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
failed=0
for dq in 0 1;do
 if [ "$variant" = red ];then modes=(2);elif [ "$variant" = ceiling ];then modes=(8 9 10 11 12 13 14 15 16 17 18);else modes=(0 1 2 3 4 5 6 7);fi
 for omit in "${modes[@]}";do
  log="$out/dq$dq-omit$omit.log";set +e;timeout 120 "$out/test" "$out/shaders" "$dq" "$omit" > "$log" 2>&1;rc=$?;set -e
  raw=$(grep -c 'SYNC-HAZARD-READ-AFTER-WRITE' "$log" || true)
  other=$(grep -E 'Validation Error|VUID-' "$log" | grep -vc 'SYNC-HAZARD-READ-AFTER-WRITE' || true)
  marker=$(grep -E '^(external |ceiling-barriers )' "$log" || true)
  printf 'external dq=%s omit=%s rc=%s RAW=%s other=%s %s\n' "$dq" "$omit" "$rc" "$raw" "$other" "$marker"
  if [ "$rc" != 0 ] || [ "$other" != 0 ] || [ -z "$marker" ] || { [ "$omit" != 1 ] && [ "$raw" != 0 ]; } || { [ "$omit" = 1 ] && [ "$raw" -lt 1 ]; };then
   echo '【红】外部依赖正/负控不符合预期';tail -20 "$log";failed=1
  fi
 done
done
[ "$failed" = 0 ] || exit 1
if [ "$variant" = red ];then echo '【绿】纯颜色通道阶段断言';elif [ "$variant" = ceiling ];then echo '【绿】探测档模板/实际通道阶段及全屏障数量';else echo '【绿】出向依赖正/负控、阶段断言、关闭开关和混合附件';fi
