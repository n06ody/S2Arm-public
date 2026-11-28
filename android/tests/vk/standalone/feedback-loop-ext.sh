#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --feedback-loop-ext
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-feedback-loop-ext"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
for shader in feedback feedback_depth; do
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader.frag" -o "$out/shaders/$shader.frag.spv"
done
cat > "$out/shaders/write_color.frag" <<'GLSL'
#version 450
layout(location=0) out vec4 color;
void main() { color = vec4(0.5, 0.0, 0.0, 1.0); }
GLSL
"$glslc" --target-env=vulkan1.1 "$out/shaders/write_color.frag" -o "$out/shaders/write_color.frag.spv"
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=("$repo/android/tests/vk/t_feedback_loop_ext.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
wrap=()
for fn in vkCreateDevice vkEnumerateDeviceExtensionProperties vkCreateImage vkCreateGraphicsPipelines vkCreateRenderPass vkCmdPipelineBarrier vkCreateImageView vkCreateFramebuffer vkCmdBeginRenderPass vkUpdateDescriptorSets vkCmdBindDescriptorSets vkCmdDraw vkCmdCopyImage vkGetDeviceProcAddr vkDestroyImageView vkWaitForFences; do
    wrap+=(-Wl,--wrap="$fn")
done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
printf 'feedback-loop-ext validation: sync=1 shader_accesses_heuristic=1 settings=%s\n' "$VK_LAYER_SETTINGS_PATH"
failed=0
# 各列为录制、布局、反馈环、屏蔽扩展和开关掩码（1 = 屏障合批，2 = 附件 NONE）。
configs=(
    '0 0 1 0 0' '0 1 1 0 0' '1 0 1 0 0' '1 1 1 0 0'
    '0 0 0 0 0' '0 0 1 1 0'
    '0 0 1 0 1' '0 1 1 0 1' '1 0 1 0 1' '1 1 1 0 1'
    '0 0 1 0 3' '0 1 1 0 3' '1 0 1 0 3' '1 1 1 0 3'
)
for config in "${configs[@]}"; do
    read -r dq opt loop hidden switches <<< "$config"
    log="$out/dq$dq-opt$opt-loop$loop-hide$hidden-switch$switches.log"
    set +e
    timeout 120 "$out/test" "$out/shaders" "$dq" "$opt" "$loop" "$hidden" "$switches" > "$log" 2>&1
    result=$?
    set -e
    cat "$log"
    if [ "$result" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-|fixture failed' "$log" ||
       ! grep -q 'feedback-loop-ext cases=8 failures=0' "$log"; then
        echo "【红】feedback-loop-ext config=$config rc=$result FAIL" >&2
        failed=1
    fi
done
# 负对照：去掉遍内自依赖屏障（feedback_loop_barrier 置空），同一夹具与同版 VVL/SyncVal 必须抓出 SYNC-HAZARD。
mut="$out/mut"; mkdir -p "$mut"
cp "$repo/android/native/nvn/backend_vulkan.c" "$mut/backend_vulkan.c"
python3 - "$repo/android/native/nvn/backend_vulkan_feedback.inc" "$mut/backend_vulkan_feedback.inc" <<'PYMUT'
import pathlib, sys
s = pathlib.Path(sys.argv[1]).read_text()
a = 'static void feedback_loop_barrier(struct nvn_gpu_cmd_s *c) {'
if s.count(a) != 1: raise SystemExit('feedback_loop_barrier mutation anchor changed')
pathlib.Path(sys.argv[2]).write_text(s.replace(a, a + ' (void)c; return;'))
PYMUT
msrc=(); for f in "${src[@]}"; do [ "$f" = "$repo/android/native/nvn/backend_vulkan.c" ] && f="$mut/backend_vulkan.c"; msrc+=("$f"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${msrc[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test-neg"
set +e
timeout 120 "$out/test-neg" "$out/shaders" 0 0 1 0 > "$out/neg-no-barrier.log" 2>&1
neg_rc=$?
set -e
if [ "$neg_rc" -eq 0 ] && ! grep -qE 'SYNC-HAZARD-' "$out/neg-no-barrier.log"; then
    echo '【红】feedback-loop-ext 负对照：去掉遍内屏障后未抓到 SYNC-HAZARD' >&2
    tail -n 20 "$out/neg-no-barrier.log" >&2
    failed=1
elif ! grep -qE 'SYNC-HAZARD-' "$out/neg-no-barrier.log"; then
    echo '【红】feedback-loop-ext 负对照：失败但不是预期 SYNC-HAZARD' >&2
    tail -n 20 "$out/neg-no-barrier.log" >&2
    failed=1
else
    echo "feedback-loop-ext barrier negative: PASS ($(grep -c 'SYNC-HAZARD-' "$out/neg-no-barrier.log") 条 SYNC-HAZARD 被抓出)"
fi
exit "$failed"
