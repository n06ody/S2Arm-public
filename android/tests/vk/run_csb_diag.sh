#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# Linux real Vulkan diagnostic fixture; stage the genuine NDK 29 headers in
# build/csb-diag/ndk before entering an offline container.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out="$repo/build/csb-diag"
mkdir -p "$out/shaders" "$out/tmp" "$out/cache" "$out/run"
export TMPDIR="$out/tmp" XDG_CACHE_HOME="$out/cache" XDG_RUNTIME_DIR="$out/run"
find_icd() {
    if [ -n "${S2A_LAVAPIPE_ICD:-}" ]; then printf '%s\n' "$S2A_LAVAPIPE_ICD"; return; fi
    for f in "$HOME/.local/share/vulkan/icd.d/lvp_icd.json" "$HOME/.local/share/vulkan/icd.d/lvp_icd."*.json \
             /usr/share/vulkan/icd.d/lvp_icd.json /usr/share/vulkan/icd.d/lvp_icd.*.json; do
        [ -f "$f" ] && { printf '%s\n' "$f"; return; }
    done
}
find_layer() {
    if [ -n "${S2A_VK_LAYER_DIR:-}" ]; then printf '%s\n' "$S2A_VK_LAYER_DIR"; return; fi
    for d in "$HOME/.local/share/vulkan/explicit_layer.d" /usr/share/vulkan/explicit_layer.d; do
        [ -f "$d/VkLayer_khronos_validation.json" ] && { printf '%s\n' "$d"; return; }
    done
}
icd=$(find_icd || true); layer=$(find_layer || true)
test -f "$icd" && test -f "$layer/VkLayer_khronos_validation.json"
test -f "$out/ndk/vulkan/vulkan_core.h"
export VK_DRIVER_FILES="$icd" VK_LAYER_PATH="$layer" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1
printf 'ICD=%s VVL=%s\n' "$icd" "$layer"
python3 - "$layer/VkLayer_khronos_validation.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))['layer']; print('VVL api='+x['api_version'])
PY
glslc --target-env=vulkan1.1 android/tests/vk/shaders/comp.comp -o "$out/shaders/comp.comp.spv"
inc=(-I "$out/ndk")
for d in android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=(android/tests/vk/t_csb_diag.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c graphics/nvn/replay/nvn_unimpl.c)
wrap=()
for f in vkAllocateDescriptorSets vkGetPhysicalDeviceFormatProperties vkCreatePipelineCache vkGetPipelineCacheData vkCreateGraphicsPipelines vkCreateComputePipelines vkCmdPipelineBarrier vkCmdDispatch vkCmdDispatchIndirect vkQueueSubmit vkWaitForFences vkGetDeviceProcAddr; do wrap+=(-Wl,--wrap="$f"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
set +e
timeout 180 "$out/test" "$out/shaders" 2>&1 | tee "$out/test.log"
status=${PIPESTATUS[0]}
set -e
if grep -Eq 'Validation Error|VUID-|SYNC-HAZARD-' "$out/test.log"; then echo 'CSB validation error'; exit 1; fi
grep -q '^device: llvmpipe' "$out/test.log"
test "$status" = 0
grep -q '^CSB PASS failures=0$' "$out/test.log"
