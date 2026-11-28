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
for shader in zero_mask_mrt.frag zero_mask_side_effect.frag tri3.vert pdc_scope_ubo.frag; do
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/depth_state.vert.spv"
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=("$repo/android/tests/vk/t_replay_zero_mask.c")
for f in graphics/nvn/replay/rp_pass.c graphics/nvn/replay/rp_draw.c graphics/nvn/replay/rp_buf.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]}" -Wl,--gc-sections -Wl,--wrap=nvn_gpu_draw -Wl,--wrap=nvn_gpu_pass_begin -Wl,--wrap=vkCreateRenderPass \
    -lvulkan -lpthread -lm -o "$out/test"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
for dq in 0 1; do
    set +e
    timeout 120 "$out/test" "$out/shaders" "$dq" 2>&1 | tee "$out/dq$dq.log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/dq$dq.log" ||
       ! grep -q 'zero-mask cases=7 pixel-failures=0' "$out/dq$dq.log" ||
       ! grep -q 'pdc scope cases=9 failures=0' "$out/dq$dq.log"; then
        echo "【红】zero-mask DQ=$dq rc=$rc or validation failure" >&2; exit 1
    fi
done
rc=0
S2A_TEST_PDC_WITHDRAW=1 timeout 120 "$out/test" "$out/shaders" 0 > "$out/pdc-withdraw.log" 2>&1 || rc=$?
if [ "$rc" -ne 1 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/pdc-withdraw.log" ||
   ! grep -q 'pdc scope cases=9 failures=4' "$out/pdc-withdraw.log" ||
   [ "$(grep -c 'store_dont_care=0 expected=1 pixels=2048 bad=0' "$out/pdc-withdraw.log")" -ne 4 ]; then
    cat "$out/pdc-withdraw.log"
    exit 1
fi
echo 'PASS PDC scope withdrawal: all four eligible physical stores retained, all pixels preserved'
