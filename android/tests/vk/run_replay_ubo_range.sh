#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 真 Vulkan 回放：常量缓冲长于着色器声明（64 KiB）且长于设备 maxUniformBufferRange——lavapipe + Khronos validation（含同步校验）。
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
glslc=${GLSLC:-$(command -v glslc || true)}
if [ -z "$glslc" ]; then echo '【红】缺 glslc' >&2; exit 2; fi
mkdir -p "$repo/build/vk-ubo-range-tmp"
out=$(mktemp -d "$repo/build/vk-ubo-range-tmp/run.XXXXXX")
trap 'rm -rf "$out"' EXIT
for s in quad.vert replay_ubo_range.frag; do "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$s" -o "$out/$s.spv"; done
inc=(); for d in android/tests/vkhost graphics/nvn/third_party/vulkan-headers/include graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=("$repo/android/tests/vk/t_replay_ubo_range.c")
for f in graphics/nvn/replay/rp_draw.c graphics/nvn/replay/rp_buf.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c android/native/nvn/backend_vulkan.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" -Wl,--gc-sections -Wl,--wrap=nvn_gpu_draw -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES=$icd VK_LAYER_PATH=$layer VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1
set +e
timeout 180 "$out/test" "$out" 2>&1 | tee "$out/log"
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" -ne 0 ] || ! grep -q 'device: llvmpipe' "$out/log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/log"; then
    echo "【红】超长常量缓冲回放 Vulkan 验证失败 rc=$rc" >&2; exit 1
fi
echo '【绿】超长常量缓冲回放：只绑前 64 KiB、段照常提交、像素来自第 64 KiB 末尾，validation 0 条'
