#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 门禁间接：android/tests/vk/run.sh 默认按 standalone/*.sh 通配逐个执行。
# run.sh --replay-tbdr-ds：用真实 Vk 通道和颜色、深度/模板读回核对闲置 DS 裁减与可证的 STORE 省略。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-replay-tbdr-ds"
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
src=("$repo/android/tests/vk/t_replay_tbdr_ds.c")
for f in graphics/nvn/replay/rp_res.c graphics/nvn/replay/rp_pass.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c graphics/nvn/gfx/nvn/nvn_dirty.c \
         android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c \
         android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c \
         android/native/nvn/nvn_vk_format.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c \
         android/native/direct/sha256.c graphics/nvn/gfx/nvn/nvn_tex.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c graphics/nvn/gfx/tex/nvn_deswizzle.c; do
    src+=("$repo/$f")
done
build_test() {
cc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" \
    -Wl,--gc-sections -Wl,--wrap=nvn_gpu_pass_begin -Wl,--wrap=vkCreateRenderPass -lvulkan -lpthread -lm -o "$out/test"
}
build_test
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
for dq in 0 1; do
    if [ "$dq" = 1 ]; then export S2A_TEST_NVK_DQ=1; else unset S2A_TEST_NVK_DQ; fi
    for arm in 0 1 2 3 8 24; do
        log="$out/dq$dq-arm$arm.log"
        if ! timeout 90 "$out/test" "$out/shaders" "$arm" "$((arm & 15))" > "$log" 2>&1 ||
           grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
           ! grep -q 'tbdr-ds Vulkan cases=32 failures=0' "$log"; then
            cat "$log"
            exit 1
        fi
    done
    echo "PASS TBDR DS DQ=$dq: 32 cases per each of six flag combinations, Vulkan and SyncVal errors=0"
done
unset S2A_TEST_NVK_DQ
log="$out/park-disabled.log"
if ! timeout 90 "$out/test" "$out/shaders" 6 0 > "$log" 2>&1 ||
   grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
   ! grep -q 'tbdr-ds Vulkan cases=32 failures=0' "$log"; then
    cat "$log"
    exit 1
fi
echo 'PASS TBDR DS: disabling the ordered-clear path preserves STORE'
log="$out/withdrawn.log"
rc=0
timeout 90 "$out/test" "$out/shaders" 0 3 > "$log" 2>&1 || rc=$?
if [ "$rc" != 1 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
   ! grep -q 'idle DS attachment behavior shape=0 omitted=0' "$log" ||
   ! grep -q 'DS STORE behavior shape=9 discard=0' "$log" ||
   ! grep -q 'idle DS attachment behavior shape=30 omitted=0' "$log" ||
   ! grep -q 'tbdr-ds Vulkan cases=32 failures=3' "$log"; then
    cat "$log"
    exit 1
fi
echo 'PASS TBDR DS withdrawal: both physical attachment assertions red, readback clean'

log="$out/terminal-withdrawn.log"
rc=0
timeout 90 "$out/test" "$out/shaders" 0 8 > "$log" 2>&1 || rc=$?
if [ "$rc" != 1 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-|TBDR pixels' "$log" ||
   ! grep -q 'terminal DS aspect shape=15 depth_dontcare_stencil_store=0' "$log" ||
   ! grep -q 'terminal color shape=17 color_dontcare=0' "$log"; then
    cat "$log"
    exit 1
fi
echo 'PASS terminal discard withdrawal: missing physical DONT_CARE detected, all pixels preserved'

original="${src[2]}"
src[2]="$out/rp_pass-mixed-mutation.c"
sed 's/cw\[i\] != d->w || ch\[i\] != d->h || g_shu\[i\].base ||/g_shu[i].base ||/' "$original" > "${src[2]}"
if cmp -s "$original" "${src[2]}"; then exit 1; fi
build_test
log="$out/mixed-guard-withdrawn.log"
rc=0
timeout 90 "$out/test" "$out/shaders" 8 8 > "$log" 2>&1 || rc=$?
if [ "$rc" != 1 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
   ! grep -q 'terminal DS aspect shape=21 depth_dontcare_stencil_store=1' "$log"; then
    cat "$log"
    exit 1
fi
echo 'PASS terminal discard mixed-size guard withdrawal: unexpected physical DONT_CARE detected'
