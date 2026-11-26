#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-replay-event-wait"
mkdir -p "$out"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
[ -f "$S2A_VK_LAYER_DIR/VkLayer_khronos_validation.json" ] || { echo 'Khronos layer missing' >&2; exit 2; }
inc=()
for dir in . build/c883-ndk android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn \
    graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$dir")
done
src=(
    android/tests/vk/t_replay_event_wait.c
    graphics/nvn/replay/{rp_exec,rp_entry,rp_state,rp_buf,rp_res,rp_pass,rp_present,rp_draw,rp_copy_t2b,rp_clear_tex,rp_draw_tex,nvn_unimpl}.c
    graphics/nvn/gfx/nvn/{nvn_dirty,nvn_fmt_expand,nvn_tex}.c
    graphics/nvn/gfx/tex/nvn_deswizzle.c
    android/native/nvn/{backend_vulkan,nvn_pipe_flight,nvn_vk_pipe_once,nvn_vk_format,nvn_vk_bcdec,nvn_vk_driver_cache}.c
    android/native/direct/{vk_device,gs_path,sha256}.c
    android/native/capprobe/capprobe_host.c
)
cc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]/#/$repo/}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
for dq in 0 1; do
    if [ "$dq" = 1 ]; then export S2A_TEST_NVK_DQ=1; else unset S2A_TEST_NVK_DQ; fi
    for arm in normal skip-wait; do
        args=()
        if [ "$arm" = skip-wait ]; then args+=("$arm"); fi
        log="$out/dq$dq-$arm.log"
        status=0
        timeout 25 "$out/test" "${args[@]}" > "$log" 2>&1 || status=$?
        want_rc=0 want_value=22 want_fail=0
        if [ "$arm" = skip-wait ]; then want_rc=1 want_value=11 want_fail=64; fi
        if [ "$status" -ne "$want_rc" ] || ! grep -q '^device: llvmpipe' "$log" ||
            ! grep -qx "EVENT_ORDER producer=22 consumer=$want_value expected=22 failures=$want_fail" "$log" ||
            grep -qE 'Validation Error|VUID-|SYNC-HAZARD' "$log"; then
            cat "$log"
            echo "event wait: DQ=$dq arm=$arm unexpected rc=$status"
            exit 1
        fi
        printf 'event wait: DQ=%s arm=%s ' "$dq" "$arm"
        grep '^EVENT_ORDER ' "$log"
    done
done
