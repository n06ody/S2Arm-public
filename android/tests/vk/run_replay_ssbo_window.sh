#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 真 Vulkan 回放：存储缓冲基址不对齐 minStorageBufferOffsetAlignment、长度超 maxStorageBufferRange——lavapipe + Khronos validation（含同步校验）。
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
mkdir -p "$repo/build/vk-ssbo-window-tmp"
out=$(mktemp -d "$repo/build/vk-ssbo-window-tmp/run.XXXXXX")
trap 'rm -rf "$out"' EXIT
for s in quad.vert quad.frag replay_ssbo_window.frag; do "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$s" -o "$out/$s.spv"; done
inc=(); for d in android/tests/vkhost graphics/nvn/third_party/vulkan-headers/include graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=("$repo/android/tests/vk/t_replay_ssbo_window.c")
for f in graphics/nvn/replay/rp_draw.c graphics/nvn/replay/rp_buf.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" -Wl,--gc-sections -Wl,--wrap=nvn_gpu_draw -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES=$icd VK_LAYER_PATH=$layer VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
if [ -n "${S2A_VK_PROFILE_LAYER_DIR:-}" ]; then
    export VK_LAYER_PATH="$VK_LAYER_PATH:$S2A_VK_PROFILE_LAYER_DIR"
    export VK_INSTANCE_LAYERS="$VK_INSTANCE_LAYERS:VK_LAYER_KHRONOS_profiles"
fi
# vk_dq_init 会跨会话保留已启动的录制线程及开关，用独立进程验证两档。
failed=0
for dq in 0 1; do
    args=("$@")
    if [ "$dq" = 0 ]; then args=(--rename-only); fi
    set +e
    S2A_REPLAY_SSBO_DQ=$dq timeout 180 "$out/test" "$out" "${args[@]}" 2>&1 | tee "$out/log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || ! grep -q 'device: llvmpipe' "$out/log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/log"; then
        echo "【红】存储缓冲回放 Vulkan 验证失败 dq=$dq rc=$rc" >&2
        failed=1
    fi
done
[ "$failed" = 0 ] || exit 1
echo '【绿】存储缓冲回放：窗口及切片用例像素正确，DQ 关开 validation 各 0 条'
