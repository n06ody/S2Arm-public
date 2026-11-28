#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# run.sh --image-barrier-batch：核对实际 Vulkan 屏障的阶段、访问掩码、合批次数与逐像素读回。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-image-barrier-batch"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
for shader in tri3.vert feedback_combined.frag; do
    "${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_image_barrier_batch.c")
for f in graphics/nvn/replay/nvn_unimpl.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c \
         android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c \
         android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c android/native/nvn/nvn_vk_bcdec.c \
         android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c; do
    src+=("$repo/$f")
done
cc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" \
    -Wl,--gc-sections -Wl,--wrap=vkGetDeviceProcAddr -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
unset VK_LAYER_ENABLES
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
failed=0
for dq in 0 1; do
    if [ "$dq" = 1 ]; then export S2A_TEST_NVK_DQ=1; else unset S2A_TEST_NVK_DQ; fi
    for layout in 0 1; do
        for arm in 0 1; do
            log="$out/dq$dq-layout$layout-arm$arm.log"
            echo "batch DQ=$dq layout=$layout arm=$arm"
            if ! timeout 90 "$out/test" "$out/shaders" "$arm" "$layout" "$arm" > "$log" 2>&1 ||
               grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
               ! grep -q 'batch Vulkan failures=0' "$log"; then
                cat "$log"
                failed=1
            else
                grep -E '^batch ' "$log"
                echo "batch VUID=0 SYNC-HAZARD=0"
            fi
        done
    done
done
[ "$failed" -eq 0 ]
for dq in 0 1; do
    if [ "$dq" = 1 ]; then export S2A_TEST_NVK_DQ=1; else unset S2A_TEST_NVK_DQ; fi
    log="$out/dq$dq-withdrawn.log"
    rc=0
    timeout 90 "$out/test" "$out/shaders" 0 1 1 > "$log" 2>&1 || rc=$?
    if [ "$rc" -ne 1 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
       [ "$(grep -c 'batch call count' "$log")" -ne 3 ] ||
       ! grep -q 'batch pixels=5120/5120' "$log" || ! grep -q 'batch Vulkan failures=3$' "$log"; then
        cat "$log"
        exit 1
    fi
    echo "batch DQ=$dq withdrawn count-failures=3 pixels=5120/5120 VUID=0 SYNC-HAZARD=0"
done
