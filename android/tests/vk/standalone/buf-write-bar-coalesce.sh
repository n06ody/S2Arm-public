#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-buf-write-bar-coalesce"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
for shader in comp.comp tri3.vert solid_green.frag; do
    "${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
inc=(-I "$repo/build/c883-ndk")
for d in android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_buf_write_bar.c")
for f in android/native/nvn/{backend_vulkan,nvn_pipe_flight,nvn_vk_pipe_once,nvn_vk_format,nvn_vk_bcdec,nvn_vk_driver_cache}.c \
         android/native/direct/{vk_device,gs_path,sha256}.c android/native/capprobe/capprobe_host.c \
         graphics/nvn/gfx/nvn/{nvn_tex,nvn_fmt_expand}.c graphics/nvn/gfx/tex/nvn_deswizzle.c graphics/nvn/replay/nvn_unimpl.c; do
    src+=("$repo/$f")
done
cc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" \
    -Wl,--gc-sections -Wl,--wrap=vkGetDeviceProcAddr -lvulkan -lpthread -lm -o "$out/test"
policy="$repo/android/tests/vk/t_buf_write_run_policy.c"
core="$repo/android/native/nvn/backend_vulkan_buf_write_run.inc"
policy_flags=(-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter)
cc "${policy_flags[@]}" "${inc[@]}" "$policy" -o "$out/policy"
"$out/policy"
mkdir -p "$out/mutant"
for control in first overlap alias capacity interleave default; do
    case "$control" in
        first) pattern='s/const int skip = run->count != 0;/const int skip = 1;/'; fail='first write retains barrier' ;;
        overlap) pattern='s/if (lo < prior->hi/if (0 \&\& lo < prior->hi/'; fail='overlap with earlier range' ;;
        alias) pattern='s/if (prior->buffer != buffer)/if (0 \&\& prior->buffer != buffer)/'; fail='unproved alias binding' ;;
        capacity) pattern='s/count == BW_RUN_LIMIT/count == BW_RUN_LIMIT - 1/'; fail='capacity boundary at 16' ;;
        interleave) pattern='s/static inline void bw_end(enum bw_result why) {/static inline void bw_end(enum bw_result why) { if (why == BW_INTERVENING) return; /'; fail='intervening command ends run' ;;
        default) pattern='s/"nvk_buf_write_bar_coalesce", 0/"nvk_buf_write_bar_coalesce", 1/'; fail='default off' ;;
    esac
    sed "$pattern" "$core" >"$out/mutant/$(basename "$core")"
    cc "${policy_flags[@]}" -I "$out/mutant" "${inc[@]}" "$policy" -o "$out/policy-$control"
    rc=0
    "$out/policy-$control" >"$out/policy-$control.log" 2>&1 || rc=$?
    if [ "$rc" != 1 ] || ! grep -qF "FAIL write policy: $fail" "$out/policy-$control.log"; then
        cat "$out/policy-$control.log"
        echo "【红】write policy mutant $control did not trip its assertion"
        exit 1
    fi
    echo "【绿】write policy mutant $control rejected"
done

export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
unset VK_LAYER_ENABLES
cat >"$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
for dq in 0 1; do
    if [ "$dq" = 1 ]; then export S2A_TEST_NVK_DQ=1; else unset S2A_TEST_NVK_DQ; fi
    for arm in -1 1; do
        log="$out/dq$dq-arm$arm.log"
        expect=$((arm == 1))
        if ! timeout 90 "$out/test" "$arm" "$expect" "$out/shaders" >"$log" 2>&1 || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
            cat "$log"
            exit 1
        fi
        grep '^write run ' "$log"
        echo "write run Vulkan: DQ=$dq arm=$arm VUID=0 SYNC-HAZARD=0"
    done
done
