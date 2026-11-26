#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# run.sh --eds12 checks Vulkan pipeline counts and readback for the same cases.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-eds12"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
for shader in tri3.vert solid_green.frag; do
    "${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
inc=(-I "$repo/build/c883-ndk")
for d in android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_eds12.c")
for f in graphics/nvn/replay/nvn_unimpl.c \
         android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c \
         android/native/nvn/nvn_vk_pipe_warm.c \
         android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c \
         android/native/nvn/nvn_vk_format.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c \
         android/native/direct/sha256.c graphics/nvn/gfx/nvn/nvn_tex.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c graphics/nvn/gfx/tex/nvn_deswizzle.c; do
    src+=("$repo/$f")
done
cc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" \
    -Wl,--gc-sections,--wrap=vkEnumerateDeviceExtensionProperties,--wrap=vkGetPhysicalDeviceFeatures2,--wrap=vkGetDeviceProcAddr \
    -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_VALIDATE_SYNC=1
unset VK_LAYER_ENABLES
result=0
for arm in static absent-extension absent-feature absent-command eds-direct eds-queued; do
    vars=()
    case "$arm" in
        static) vars+=('debug.s2a.nvk_eds=0' S2A_EDS_STATIC=1) ;;
        absent-extension) vars+=(S2A_EDS_NO_EXT=1 S2A_EDS_STATIC=1) ;;
        absent-feature) vars+=(S2A_EDS_NO_FEAT=1 S2A_EDS_STATIC=1) ;;
        absent-command) vars+=(S2A_EDS_NO_PROC=1 S2A_EDS_STATIC=1) ;;
        eds-direct) ;;
        eds-queued) vars+=(S2A_TEST_NVK_DQ=1) ;;
    esac
    log="$out/$arm.log"
    echo "EDS arm=$arm"
    if ! timeout 90 env "${vars[@]}" "$out/test" "$out/shaders" 2>&1 | tee "$log"; then result=1; fi
    grep -q 'device: llvmpipe' "$log"
    if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then result=1; fi
done
exit "$result"
