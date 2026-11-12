#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 真 Vulkan 回放清除读回：lavapipe + Khronos validation（含同步校验）。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
if [ "$(uname -s)" = Darwin ]; then
    export S2A_VULKAN_CACHE="$repo/build/vulkan-cache"
    exec bash "$repo/tests/vulkan/run.sh" android/tests/vk/run.sh
fi
icd=
for f in /usr/share/vulkan/icd.d/lvp_icd.json /usr/share/vulkan/icd.d/lvp_icd.*.json "$HOME"/.local/share/vulkan/icd.d/lvp_icd*.json; do
    if [ -f "$f" ]; then icd=$f; break; fi
done
layer=
for d in /usr/share/vulkan/explicit_layer.d "$HOME"/.local/share/vulkan/explicit_layer.d; do
    if [ -f "$d/VkLayer_khronos_validation.json" ]; then layer=$d; break; fi
done
if [ -z "$icd" ] || [ -z "$layer" ]; then echo '【红】缺 lavapipe 或 validation layer' >&2; exit 2; fi
mkdir -p "$repo/build/vk-clear-tmp"
out=$(mktemp -d "$repo/build/vk-clear-tmp/run.XXXXXX")
trap 'rm -rf "$out"' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    -I "$repo/android/tests/vkhost" -I "$repo/graphics/nvn/third_party/vulkan-headers/include" \
    -I "$repo/graphics/nvn" -I "$repo/graphics/nvn/replay" -I "$repo/graphics/nvn/gfx/nvn" \
    -I "$repo/graphics/nvn/gfx/nvn/include" -I "$repo/android/native" -I "$repo/android/native/nvn" -I "$repo/android/native/direct" \
    "$repo/android/tests/vk/t_replay_clear_tex.c" "$repo/graphics/nvn/replay/rp_clear_tex.c" \
    "$repo/android/native/nvn/backend_vulkan.c" "$repo/android/native/direct/vk_device.c" "$repo/android/native/direct/gs_path.c" \
    "$repo/android/native/nvn/nvn_vk_format.c" "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c" "$repo/android/native/nvn/nvn_vk_bcdec.c" \
    -Wl,--wrap=vkAllocateDescriptorSets -Wl,--wrap=vkGetPhysicalDeviceFormatProperties -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES=$icd VK_LAYER_PATH=$layer VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1
set +e
timeout 180 "$out/test" 2>&1 | tee "$out/log"
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" -ne 0 ] || ! grep -q 'device: llvmpipe' "$out/log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/log"; then
    echo "【红】回放清除 Vulkan 验证失败 rc=$rc" >&2; exit 1
fi
