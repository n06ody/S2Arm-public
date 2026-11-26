#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认按 standalone/*.sh 通配执行，单跑用 --bc-3d。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-bc-3d"
mkdir -p "$out"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
"${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/bc_volume.comp" -o "$out/bc_volume.comp.spv"
inc=()
for d in build/c883-ndk android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_bc_3d.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c graphics/nvn/gfx/nvn/nvn_tex.c graphics/nvn/gfx/tex/nvn_deswizzle.c graphics/nvn/gfx/tex/nvn_bcdec.c; do
    src+=("$repo/$f")
done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 -DS2A_BCDEC_HOST_TEST \
    "${inc[@]}" "${src[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
set +e
S2A_VK_BC_DECODE=force timeout 180 "$out/test" "$out" 2>&1 | tee "$out/run.log"
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" -ne 0 ] || ! grep -q '^device: llvmpipe' "$out/run.log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/run.log"; then
    echo "【红】3D BC 解码夹具 rc=$rc" >&2
    exit 1
fi

# 采样故意换成同一 mip 的下一片。命令合法，压缩影子仍匹配，只有纹素断言应报红。
mkdir -p "$out/wrong-slice"
"${GLSLC:-glslc}" --target-env=vulkan1.1 -DS2A_BC_TEST_WRONG_SLICE \
    "$repo/android/tests/vk/shaders/bc_volume.comp" -o "$out/wrong-slice/bc_volume.comp.spv"
set +e
S2A_VK_BC_DECODE=force timeout 180 "$out/test" "$out/wrong-slice" > "$out/wrong-slice.log" 2>&1
rc=$?
set -e
if [ "$rc" -ne 1 ] || ! grep -qE 'BC3D fmt=0x49 CPU sample mismatches=[1-9][0-9]* shadow mismatches=0' "$out/wrong-slice.log" ||
   grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/wrong-slice.log"; then
    cat "$out/wrong-slice.log"
    echo "【红】3D BC 错片负控未命中 rc=$rc" >&2
    exit 1
fi
grep -E 'BC3D fmt=0x49 (CPU|GPU) pixels=' "$out/wrong-slice.log"
echo '【绿】3D BC 错片负控仅触发纹素不符，压缩影子不变'
