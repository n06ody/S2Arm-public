#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 渲染分辨率倍率（片 1）：真 rp_pass / rp_draw / rp_res / rp_buf + 真 Vulkan 后端，同步校验，两级录制两档。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-res-scale-core"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/res_scale_coord.frag" -o "$out/shaders/res_scale_coord.frag.spv"
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/depth_state.vert.spv"
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=("$repo/android/tests/vk/t_replay_res_scale.c")
for f in graphics/nvn/replay/rp_pass.c graphics/nvn/replay/rp_draw.c graphics/nvn/replay/rp_res.c graphics/nvn/replay/rp_buf.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c graphics/nvn/gfx/nvn/nvn_tex.c graphics/nvn/gfx/tex/nvn_deswizzle.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]}" -Wl,--gc-sections -Wl,--wrap=nvn_gpu_draw \
    -lvulkan -lpthread -lm -o "$out/test"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
for dq in 0 1; do
    set +e
    timeout 120 "$out/test" "$out/shaders" "$dq" 2>&1 | tee "$out/dq$dq.log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-' "$out/dq$dq.log" ||
       ! grep -q 'res-scale cases=5 failures=0' "$out/dq$dq.log"; then
        echo "【红】res-scale DQ=$dq rc=$rc or validation failure" >&2; exit 1
    fi
done
