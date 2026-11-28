#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# run.sh --pass-state-keep：对照跨 Vulkan 通道的实际状态命令与逐像素读回。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-pass-state-keep"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
for shader in tri3.vert pushc.frag; do
    "${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_pass_state_keep.c")
for f in graphics/nvn/replay/nvn_unimpl.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c \
         android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c \
         android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c android/native/nvn/nvn_vk_bcdec.c \
         android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c; do
    src+=("$repo/$f")
done
cc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" \
    -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
failed=0
for dq in 0 1; do
    if [ "$dq" = 1 ]; then export S2A_TEST_NVK_DQ=1; else unset S2A_TEST_NVK_DQ; fi
    for arm in 0 1; do
        log="$out/dq$dq-arm$arm.log"
        echo "pass-state DQ=$dq arm=$arm"
        if ! timeout 90 "$out/test" "$out/shaders" "$arm" "$arm" > "$log" 2>&1 ||
           grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
           ! grep -q 'pass-state Vulkan failures=0' "$log"; then
            cat "$log"
            failed=1
        else
            grep -E '^pass-state (emits|pixels|recorded)' "$log"
            echo "pass-state DQ=$dq arm=$arm VUID=0 SYNC-HAZARD=0"
        fi
    done
done
[ "$failed" -eq 0 ]
# 只撤开关，保持命令、像素与命中预期：相邻同态、变化和内部清除后三项计数回红。
for dq in 0 1; do
    if [ "$dq" = 1 ]; then export S2A_TEST_NVK_DQ=1; else unset S2A_TEST_NVK_DQ; fi
    log="$out/dq$dq-withdrawn.log"
    rc=0
    timeout 90 "$out/test" "$out/shaders" 0 1 > "$log" 2>&1 || rc=$?
    if [ "$rc" -ne 1 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
       [ "$(grep -c 'pass-state count mismatch' "$log")" -ne 3 ] ||
       ! grep -q 'pass-state Vulkan failures=3$' "$log"; then
        cat "$log"
        exit 1
    fi
    echo "pass-state DQ=$dq withdrawn count-failures=3 pixels-correct=20480 VUID=0 SYNC-HAZARD=0"
done
