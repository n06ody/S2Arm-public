#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认按 standalone/*.sh 通配执行，单跑用 --downsample-fmt-depth。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
[ -z "${1:-}" ] || exit 64
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
out="$repo/build/vk-downsample-fmt-depth"
mkdir -p "$out"
inc=()
for dir in build/c883-ndk android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn \
    graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$dir")
done
src=(
    android/tests/vk/t_downsample_fmt_depth.c
    graphics/nvn/replay/{rp_buf,rp_clear_tex,rp_entry,rp_pass,rp_res,rp_state,nvn_unimpl}.c
    graphics/nvn/gfx/nvn/{nvn_dirty,nvn_fmt_expand,nvn_tex}.c
    graphics/nvn/gfx/tex/nvn_deswizzle.c
    android/native/nvn/{backend_vulkan,nvn_pipe_flight,nvn_vk_pipe_once,nvn_vk_format,nvn_vk_bcdec,nvn_vk_driver_cache}.c
    android/native/direct/{vk_device,gs_path,sha256}.c
    android/native/capprobe/capprobe_host.c
)
cc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]/#/$repo/}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"

export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR"
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
printf '%s\n' 'khronos_validation.validate_sync = true' 'khronos_validation.syncval_shader_accesses_heuristic = true' > "$out/vk_layer_settings.txt"
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
no_validation_errors() {
    grep -q '^device: llvmpipe' "$1" && ! grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$1"
}

log="$out/run.log"
rc=0
timeout 120 "$out/test" > "$log" 2>&1 || rc=$?
cat "$log"
[ "$rc" = 0 ] && no_validation_errors "$log" && grep -qx 'downsample-fmt-depth: failures=0' "$log" || exit 1

neg="$out/negative.log"
rc=0
timeout 120 "$out/test" --negative > "$neg" 2>&1 || rc=$?
if [ "$rc" != 1 ] || ! no_validation_errors "$neg" ||
   ! grep -qx 'downsample-fmt-depth: failures=4' "$neg" ||
   ! grep -q '1x RGBA8->BGRA8 downsample mismatch=' "$neg" ||
   ! grep -q 'D32F 4x->1x sample0 depth resolve mismatch=' "$neg"; then
    cat "$neg"; exit 1
fi
echo '【负对照开始】'
grep -E '1x RGBA8->BGRA8 downsample mismatch=|D32F 4x->1x sample0 depth resolve mismatch=' "$neg"
echo '【负对照结束】'
echo '【绿】Downsample 1x 换格式 blit、D32F/D16 MSAA->1x sample0 深度 resolve、角裁切 MSAA 与负控全通，Vulkan 校验 0 条'
