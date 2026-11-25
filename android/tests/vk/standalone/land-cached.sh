#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档通过 standalone/*.sh，单独跑 --land-cached。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-land-cached"
mkdir -p "$out"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
"${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/compute_read_args.comp" -o "$out/storage.comp.spv"
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
backend="$repo/android/native/nvn"
src=()
for f in android/tests/vk/t_land_cached.c graphics/nvn/replay/nvn_unimpl.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do
    src+=("$repo/$f")
done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]}" "$backend/backend_vulkan.c" -Wl,--gc-sections \
    -Wl,--wrap=vkInvalidateMappedMemoryRanges -Wl,--wrap=vkQueueSubmit \
    -Wl,--wrap=vkFlushMappedMemoryRanges -Wl,--wrap=vkWaitForFences -lvulkan -lpthread -lm -o "$out/test"

export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_VALIDATE_SYNC=1
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
for dq in 0 1; do
    log="$out/dq$dq.log"
    if ! timeout 30 "$out/test" "$dq" "$out/storage.comp.spv" > "$log" 2>&1 ||
       grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
        echo "【红】land-cached dq=$dq" >&2
        cat "$log" >&2
        exit 1
    fi
    grep '^land-cached ' "$log"
done
echo 'RESULT: PASS（缓存镜像：22 个 Vulkan 场景，开启同步校验）'
