#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --<名>
# Production pass/draw replay, actual Vulkan images, validation required.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-zero-mask-core"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
for shader in zero_mask_mrt.frag zero_mask_side_effect.frag no_attachment_side_effect.frag; do
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/depth_state.vert.spv"
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=("$repo/android/tests/vk/t_replay_zero_mask.c")
for f in graphics/nvn/replay/rp_pass.c graphics/nvn/replay/rp_draw.c graphics/nvn/replay/rp_buf.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]}" -Wl,--gc-sections -Wl,--wrap=nvn_gpu_draw -Wl,--wrap=nvn_gpu_pass_begin \
    -lvulkan -lpthread -lm -o "$out/test"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
for dq in 0 1; do
    set +e
    timeout 120 "$out/test" "$out/shaders" "$dq" 2>&1 | tee "$out/dq$dq.log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-' "$out/dq$dq.log" ||
       ! grep -q 'zero-mask cases=7 pixel-failures=0' "$out/dq$dq.log"; then
        echo "【红】zero-mask DQ=$dq rc=$rc or validation failure" >&2; exit 1
    fi
    # Keep the no-output shader on its own run. The existing MRT fixture deliberately
    # leaves declared outputs unbound and can produce unused-output warnings.
    log="$out/no-attachment-dq$dq.log"
    set +e
    timeout 120 "$out/test" "$out/shaders" "$dq" no-attachment 2>&1 | tee "$log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || grep -qE 'Validation (Error|Warning)|VUID-|SYNC-HAZARD' "$log" ||
       ! grep -qx "no-attachment dq=$dq mode=0 draws=0 passes=0 fragments=0 skipped=1" "$log" ||
       ! grep -qx "no-attachment dq=$dq mode=1 draws=1 passes=1 fragments=768 skipped=0" "$log" ||
       ! grep -q 'noatt: requested=1 enabled=1' "$log" ||
       ! grep -q 'noatt: summary recorded_draws=1$' "$log"; then
        echo "【红】no-attachment DQ=$dq rc=$rc or validation failure" >&2; exit 1
    fi
done
