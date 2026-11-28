#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --global-priority
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="${S2A_GPRIO_OUT:-$repo/build/vk-global-priority}"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/gprio_check.comp" -o "$out/shaders/gprio_check.comp.spv"

inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=(
    "$repo/android/tests/vk/t_global_priority.c"
    "$repo/graphics/nvn/replay/nvn_unimpl.c"
    "$repo/android/native/nvn/backend_vulkan.c"
    "$repo/android/native/nvn/nvn_pipe_flight.c"
    "$repo/android/native/nvn/nvn_vk_pipe_once.c"
    "$repo/android/native/direct/vk_device.c"
    "$repo/android/native/direct/gs_path.c"
    "$repo/android/native/capprobe/capprobe_host.c"
    "$repo/android/native/nvn/nvn_vk_format.c"
    "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c"
    "$repo/android/native/nvn/nvn_vk_bcdec.c"
    "$repo/android/native/nvn/nvn_vk_driver_cache.c"
    "$repo/android/native/direct/sha256.c"
)
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]}" \
    -Wl,--wrap=vkEnumerateDeviceExtensionProperties \
    -Wl,--wrap=vkCreateDevice \
    -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"

export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"

for mode in default on not-permitted hide-ext; do
    log="$out/$mode.log"
    timeout 60 "$out/test" "$out/shaders" "$mode" > "$log" 2>&1 || { cat "$log"; exit 1; }
    if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
        cat "$log"
        exit 1
    fi
    grep '^global-priority ' "$log"
done

# 负对照（撤退验证）：关闭开关但按启用期望验收，必报红退非零且无 VVL 错
log="$out/withdraw.log"
echo "【负对照开始】"
set +e
timeout 60 "$out/test" "$out/shaders" withdraw > "$log" 2>&1
rc=$?
set -e
grep '^global-priority \|【红】' "$log" || true
echo "【负对照结束】"
if [ "$rc" -eq 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
    cat "$log"
    echo "【红】withdraw negative control unexpectedly succeeded or triggered VVL (rc=$rc)" >&2
    exit 1
fi
echo "PASS: global-priority negative withdraw rejected as expected (rc=$rc)"
