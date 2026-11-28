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
for shader in tri.vert tri.frag cube.frag cubearr.frag arr.frag img.comp sparse_attachment.frag; do
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
    -Wl,--gc-sections -Wl,--wrap=vkCreateImageView -Wl,--wrap=vkQueueBindSparse \
    -Wl,--wrap=vkCreateImage -Wl,--wrap=vkAllocateMemory -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_VALIDATE_SYNC=1
unset VK_LAYER_ENABLES S2A_TEST_RENDER_AUDIT S2A_TEST_NVK_DQ
if [ "${1:-}" = "--sparse-write" ]; then
    rc=0
    for native in 0 1; do
        for op in attachment storage; do
            for test_case in mip base hole tail; do
                log="$out/sparse-$op-$native-$test_case.log"
                timeout 90 "$out/test" "$out/shaders" sparse-write "$op" "$native" "$test_case" > "$log" 2>&1 || rc=1
                cat "$log"
                prefix="sparse pass $op native=$native"
                case "$test_case" in
                    mip) expected="$prefix write_errors=0/16384 remap_errors=0/16384 hole_errors=0/16384";;
                    base) expected="$prefix base write_errors=0/65536";;
                    hole)
                        if [ "$op" = storage ] && grep -q 'strict=1 enabled_queue=1' "$log"; then expected="$prefix hole write_errors=0/65536"
                        else expected="$prefix hole rejected"; fi;;
                    tail) expected="$prefix tail rejected";;
                esac
                grep -Fxq "$expected" "$log" || rc=1
                if [ "$native" = 1 ] && [ "$op" = attachment ] && { [ "$test_case" = mip ] || [ "$test_case" = base ]; }; then
                    grep -Fxq "sparse attachment migrate $test_case retained=16/16" "$log" || rc=1
                fi
                if grep -Eq 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then rc=1; fi
            done
        done
    done
    log="$out/sparse-attachment-1-raw.log"
    timeout 90 "$out/test" "$out/shaders" sparse-write attachment 1 raw > "$log" 2>&1 || rc=1
    cat "$log"
    grep -Fxq 'sparse pass attachment native=1 raw rejected' "$log" || rc=1
    if grep -Eq 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then rc=1; fi
    for native in 0 1; do
        for op in buffer image clear blit; do
            log="$out/sparse-$op-$native.log"
            timeout 90 "$out/test" "$out/shaders" sparse-write "$op" "$native" > "$log" 2>&1 || rc=1
            cat "$log"
            grep -qx "sparse writer $op native=$native resident_errors=0/32768 hole_errors=0/32768 mip_errors=0/16384" "$log" || rc=1
            grep -qx "sparse remap $op native=$native retained_errors=0/16384 mapping_errors=0/49152" "$log" || rc=1
            if grep -Eq 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then rc=1; fi
        done
    done
    for op in clear decode encode; do
        log="$out/override-$op.log"
        timeout 90 "$out/test" "$out/shaders" override "$op" > "$log" 2>&1 || rc=1
        cat "$log"
        grep -qx "override $op data_errors=0/16 neighbor_errors=0/16" "$log" || rc=1
        if grep -Eq 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then rc=1; fi
    done
    exit "$rc"
fi
for dq in 0 1; do
    if [ "$dq" -eq 1 ]; then export S2A_TEST_NVK_DQ=1; else unset S2A_TEST_NVK_DQ; fi
    timeout 90 "$out/test" "$out/shaders" 2>&1 | tee "$out/run-dq-$dq.log"
    grep -q 'device: llvmpipe' "$out/run-dq-$dq.log"
    grep -q 'replay cube view failures=0' "$out/run-dq-$dq.log"
    if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/run-dq-$dq.log"; then exit 1; fi
done
