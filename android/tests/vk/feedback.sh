#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
out="$repo/build/vk-feedback-core"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
for shader in feedback feedback_pool feedback_combined feedback_depth; do
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader.frag" -o "$out/shaders/$shader.frag.spv"
done
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=("$repo/android/tests/vk/t_feedback.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
wrap=()
for fn in vkCreateImageView vkCreateFramebuffer vkCmdBeginRenderPass vkUpdateDescriptorSets vkCmdBindDescriptorSets vkCmdDraw vkCmdCopyImage vkGetDeviceProcAddr vkDestroyImageView vkWaitForFences; do wrap+=(-Wl,--wrap="$fn"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
# run.sh 在调用此独立入口之后才开同步校验，不能依赖父进程或本地包装器。
# 同计算写入负控一样同时打开 shader 访问启发式；仅加载 validation 层不够。
export VK_LAYER_VALIDATE_SYNC=1
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
printf 'feedback validation: sync=1 shader_accesses_heuristic=1 settings=%s\n' "$VK_LAYER_SETTINGS_PATH"
failed=0
for dq in 0 1; do
    for opt in 0 1; do
        set +e
        timeout 120 "$out/test" "$out/shaders" "$dq" "$opt" 1 2>&1 | tee "$out/dq$dq-opt$opt.log"
        rc=${PIPESTATUS[0]}
        set -e
        if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-' "$out/dq$dq-opt$opt.log" ||
           ! grep -q 'feedback cases=15 failures=0' "$out/dq$dq-opt$opt.log"; then
            echo "【红】feedback DQ=$dq OPT=$opt rc=$rc or validation failure" >&2; failed=1
        fi
    done
done
# Negative control disables the whole snapshot path (copy + barriers + descriptor
# substitution), with the same binary and real driver / validation configuration.
set +e
timeout 120 "$out/test" "$out/shaders" 1 0 0 > "$out/negative.log" 2>&1
negative_rc=$?
set -e
if [ "$negative_rc" -ne 1 ] ||
   ! grep -q 'case=3 draws=2 live-overlap=2 copies=0 copy-bad=1 pixel-bad=0' "$out/negative.log" ||
   ! grep -q 'feedback cases=15 failures=' "$out/negative.log" ||
   ! grep -q 'SYNC-HAZARD-READ-AFTER-WRITE' "$out/negative.log"; then
    echo '【红】feedback negative did not reach the expected alias failure' >&2
    tail -n 40 "$out/negative.log";failed=1
else
    echo 'feedback negative: PASS (same binary, copy off, live alias + VVL hazard detected)'
fi
exit "$failed"
