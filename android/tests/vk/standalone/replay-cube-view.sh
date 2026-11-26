#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 门禁间接：android/tests/vk/run.sh 默认按 standalone/*.sh 通配逐个执行。
# run.sh --replay-cube-view：用真贴图登记、声明绑定与 Vulkan 像素读回验证立方体层范围。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-replay-cube-view"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
for shader in tri.vert cube.frag cubearr.frag arr.frag compute_read_image.comp; do
    "${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
inc=(-I "$repo/build/c883-ndk")
for d in android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_replay_cube_view.c")
for f in graphics/nvn/replay/rp_res.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c graphics/nvn/gfx/nvn/nvn_dirty.c \
         android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c \
         android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c \
         android/native/nvn/nvn_vk_format.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c \
         android/native/direct/sha256.c graphics/nvn/gfx/nvn/nvn_tex.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c graphics/nvn/gfx/tex/nvn_deswizzle.c; do
    src+=("$repo/$f")
done
cc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" \
    -Wl,--gc-sections -Wl,--wrap=vkCreateImageView -Wl,--wrap=vkCreateSampler \
    -Wl,--wrap=vkUpdateDescriptorSets -Wl,--wrap=vkCmdDispatch -Wl,--wrap=vkGetDeviceProcAddr \
    -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_VALIDATE_SYNC=1
unset VK_LAYER_ENABLES S2A_TEST_RENDER_AUDIT S2A_TEST_NVK_DQ
timeout 90 "$out/test" "$out/shaders" 2>&1 | tee "$out/run.log"
S2A_TEST_RENDER_AUDIT=1 timeout 90 "$out/test" "$out/shaders" 2>&1 | tee "$out/audit.log"
for log in "$out/run.log" "$out/audit.log"; do
    grep -q 'device: llvmpipe' "$log"
    grep -q 'replay cube view failures=0' "$log"
    if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then exit 1; fi
done
if grep -q 'S2A_RENDER_AUDIT' "$out/run.log"; then echo 'audit logged while disabled' >&2; exit 1; fi
grep -q 'kind=descriptor reason=storage_image .*stage=5 set=0 binding=0 element=0 .*VkResult=-1' "$out/audit.log"
grep -q 'kind=view reason=vkCreateImageView .*VkResult=-1' "$out/audit.log"
grep -q 'kind=attachment reason=vkCreateImageView .*binding=0 .*usage=attachment slot=0 .*VkResult=-1' "$out/audit.log"
grep -q 'kind=descriptor reason=pool_sampler_unavailable' "$out/audit.log"
grep -q 'kind=segment reason=segment_failed/rc=-1003 .*fence=1024' "$out/audit.log"
[ "$(grep -c 'kind=descriptor reason=fixture_repeat ' "$out/audit.log")" = 2 ]
[ "$(grep -c 'S2A_RENDER_AUDIT kind=' "$out/audit.log")" = 64 ]
for phase in progress final; do
    grep -qE "phase=$phase presents=300 distinct=64 repeats=[1-9][0-9]* overflow=[1-9][0-9]* empty=2 " "$out/audit.log"
done
