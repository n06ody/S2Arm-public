#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-copy-t2t-core"
mkdir -p "$out"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
"${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/resolve_sample_depth.vert.spv"
"${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/resolve_sample_depth.frag" -o "$out/resolve_sample_depth.frag.spv"
inc=(-I "$repo/build/c883-ndk")
for d in android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_replay_indirect.c")
for f in graphics/nvn/replay/rp_draw.c graphics/nvn/replay/rp_buf.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c \
         android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c \
         android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c \
         android/native/nvn/nvn_vk_format.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c \
         android/native/direct/sha256.c graphics/nvn/gfx/nvn/nvn_tex.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c graphics/nvn/gfx/tex/nvn_deswizzle.c; do
    src+=("$repo/$f")
done
cc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 -DS2A_COPY_T2T_HIDE_BLIT "${inc[@]}" "${src[@]}" \
    -Wl,--gc-sections -Wl,--wrap=nvn_gpu_draw -Wl,--wrap=s2a_vk_format_properties -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
for dq in 0 1; do
    if [ "$dq" = 1 ]; then export S2A_REPLAY_INDIRECT_DQ=1; else unset S2A_REPLAY_INDIRECT_DQ; fi
    log="$out/dq$dq.log"
    if ! timeout 90 "$out/test" "$out" --copy-t2t-only > "$log" 2>&1 ||
       grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
       ! grep -q '^copy-t2t 14/14 passed$' "$log" ||
       ! grep -q '^copy-t2t rejections: 3/3' "$log"; then
        cat "$log"
        exit 1
    fi
    echo "【绿】copy-t2t Vulkan DQ=$dq pixels=14/14 rejections=3/3 VUID=0 SYNC-HAZARD=0"
done
