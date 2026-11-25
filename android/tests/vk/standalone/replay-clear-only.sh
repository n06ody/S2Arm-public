#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 门禁间接：android/tests/vk/run.sh 默认按 standalone/*.sh 通配逐个执行。
# run.sh --replay-clear-only：用真实回放挂起、Vk 通道及读回核对纯清除裁减。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-replay-clear-only"
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
src=("$repo/android/tests/vk/t_replay_clear_only.c")
for f in graphics/nvn/replay/rp_res.c graphics/nvn/replay/rp_pass.c graphics/nvn/replay/rp_clear_tex.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c graphics/nvn/gfx/nvn/nvn_dirty.c \
         android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c \
         android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c \
         android/native/nvn/nvn_vk_format.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c \
         android/native/direct/sha256.c graphics/nvn/gfx/nvn/nvn_tex.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c graphics/nvn/gfx/tex/nvn_deswizzle.c; do
    src+=("$repo/$f")
done
cc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" \
    -Wl,--gc-sections -Wl,--wrap=nvn_gpu_pass_begin -Wl,--wrap=nvn_gpu_clear_view -Wl,--wrap=nvn_gpu_clear_kept_view -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
for dq in 0 1; do
    if [ "$dq" = 1 ]; then export S2A_TEST_NVK_DQ=1; else unset S2A_TEST_NVK_DQ; fi
    for arm in 0 1; do
        log="$out/dq$dq-arm$arm.log"
        if ! timeout 90 "$out/test" "$out/shaders" "$arm" "$arm" > "$log" 2>&1 ||
           grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
           ! grep -q 'clear-only Vulkan cases=6 failures=0' "$log"; then
            cat "$log"
            exit 1
        fi
        echo "clear-only Vulkan: DQ=$dq arm=$arm cases=6 VUID=0 SYNC-HAZARD=0"
        log="$out/fold-dq$dq-arm$arm.log"
        if ! timeout 90 "$out/test" "$out/shaders" "$arm" "$arm" fold > "$log" 2>&1 ||
           grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
           ! grep -q 'clear-fold Vulkan cases=6 failures=0' "$log"; then
            cat "$log"
            exit 1
        fi
        grep '^clear-fold Vulkan case=' "$log"
        echo "clear-fold Vulkan: DQ=$dq arm=$arm cases=6 VUID=0 SYNC-HAZARD=0"
    done
done
# Withdraw the optimization while preserving the workload, pixels and expectations.
set +e
timeout 90 "$out/test" "$out/shaders" 0 1 > "$out/withdrawn.log" 2>&1
rc=$?
set -e
if [ "$rc" -ne 1 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/withdrawn.log" ||
   [ "$(grep -c 'clear-only attachment behavior' "$out/withdrawn.log")" -ne 3 ] ||
   ! grep -q 'clear-only Vulkan cases=6 failures=3' "$out/withdrawn.log"; then
    cat "$out/withdrawn.log"
    exit 1
fi
echo '【绿】纯清除 Vulkan 裁减/回退，双 DQ 读回及撤回负控，VVL 同步校验零错误'

# 关闭贴图折叠后重跑同一图像，必须在录制路径断言报红，读回仍逐像素一致。
echo '【负对照开始】'
rc=0
timeout 90 "$out/test" "$out/shaders" 0 1 fold > "$out/fold-withdrawn.log" 2>&1 || rc=$?
if [ "$rc" -ne 1 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/fold-withdrawn.log" ||
   [ "$(grep -c 'fold routes case=' "$out/fold-withdrawn.log")" -ne 5 ] ||
   ! grep -q 'clear-fold Vulkan cases=6 failures=5' "$out/fold-withdrawn.log"; then
    cat "$out/fold-withdrawn.log"
    exit 1
fi
echo '【负对照结束】'
echo '【绿】贴图整面清除 MRT 折叠与别名、矩形、覆盖、重解释和局部遍回退，双 DQ 及同步校验'
