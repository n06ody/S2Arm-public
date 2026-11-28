#!/usr/bin/env bash
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑，可单跑 --image-2d-view-of-3d。
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="${S2A_IMAGE_2D_VIEW_OF_3D_OUT:-$repo/build/vk-image-2d-view-of-3d}"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/tri.vert" -o "$out/shaders/tri.vert.spv"
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/tri_combined.frag" -o "$out/shaders/tri_combined.frag.spv"
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/img_rgba8.comp" -o "$out/shaders/img_rgba8.comp.spv"

inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=(
    "$repo/android/tests/vk/t_image_2d_view_of_3d.c"
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
    -Wl,--wrap=vkGetPhysicalDeviceFeatures2 \
    -Wl,--wrap=vkCreateDevice \
    -Wl,--wrap=vkCreateImage \
    -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"

export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"

for arm in on off hide-ext hide-feat on-hot-hash; do
    mode=$arm
    flags=()
    if [ "$arm" = on-hot-hash ]; then
        mode=on
        flags+=(S2A_TEST_2D3D_HOT_HASH=1)
    fi
    log="$out/$arm.log"
    env -u S2A_TEST_2D3D_HOT_HASH "${flags[@]}" timeout 60 "$out/test" "$out/shaders" "$mode" > "$log" 2>&1 || { cat "$log"; exit 1; }
    if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
        cat "$log"
        exit 1
    fi
    grep '^image-2d-view-of-3d ' "$log" | sed "s/$/ arm=$arm/"
done

# 负对照（撤退验证）：开关关闭却按启用期望验收，必须报红退出非零
log="$out/withdraw.log"
echo "【负对照开始】"
set +e
timeout 60 "$out/test" "$out/shaders" withdraw > "$log" 2>&1
rc=$?
set -e
grep '^image-2d-view-of-3d \|【红】' "$log" || true
echo "【负对照结束】"
if [ "$rc" -eq 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
    cat "$log"
    echo "【红】withdraw negative control unexpectedly succeeded or triggered VVL (rc=$rc)" >&2
    exit 1
fi
echo "PASS: image-2d-view-of-3d negative withdraw rejected as expected (rc=$rc)"
