#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --<名>
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-nvnbar"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
for shader in solid_green.frag feedback.frag compute_read_buffer.frag nvnbar_fragment_write.frag \
              compute_read_vertex.vert compute_read_args.comp compute_read_readonly.comp; do
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=("$repo/android/tests/vk/t_nvnbar.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
wrap=()
for fn in nvn_plat_flag vkCreateRenderPass vkCmdPipelineBarrier vkCreateImageView vkCreateFramebuffer vkCmdBeginRenderPass vkUpdateDescriptorSets vkCmdBindDescriptorSets vkCmdDraw vkCmdCopyImage vkGetDeviceProcAddr vkDestroyImageView vkWaitForFences; do wrap+=(-Wl,--wrap="$fn"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
# run.sh 在调用此独立入口之后才开同步校验，不能依赖父进程或本地包装器。
# 同计算写入负控一样同时打开 shader 访问启发式；仅加载 validation 层不够。
unset VK_LAYER_ENABLES
export VK_LAYER_VALIDATE_SYNC=1
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
printf 'nvnbar validation: sync=1 shader_accesses_heuristic=1 settings=%s\n' "$VK_LAYER_SETTINGS_PATH"
failed=0
run() {
    local mode=$1 dq=$2 arm=$3 expected=$4 rc=0
    local log="$out/${mode}-${dq}-${arm}.log"
    S2A_TEST_NVK_NVNBAR=0 timeout 120 "$out/test" "$out/shaders" "$mode" "$dq" "$arm" >"$log" 2>&1 || rc=$?
    local raw other
    raw=$(grep -c 'SYNC-HAZARD-READ-AFTER-WRITE:' "$log" || true)
    # VVL emits Validation Error headers with a closing ], and other loggers use ID:.
    [ "$raw" -ne 0 ] || raw=$(grep -c 'Validation Error:.*SYNC-HAZARD-READ-AFTER-WRITE' "$log" || true)
    other=$(grep -E 'Validation Error|VUID-|SYNC-HAZARD-' "$log" | grep -vc 'SYNC-HAZARD-READ-AFTER-WRITE' || true)
    printf 'nvnbar validation arm=%s mode=%s dq=%s rc=%s RAW=%s other=%s\n' "$arm" "$mode" "$dq" "$rc" "$raw" "$other"
    if [ "$rc" != 0 ] || [ "$other" != 0 ] || ! grep -q "^nvnbar pass: arm=$arm " "$log" || \
       { [ "$expected" = clean ] && [ "$raw" != 0 ]; } || \
       { [ "$expected" = raw ] && [ "$raw" = 0 ]; }; then
        tail -40 "$log"
        failed=1
    fi
}
for dq in 0 1; do
    run 0 "$dq" shape clean
    for arm in shape abab sample vertex indirect fragment dispatch game-fragment game-fragment-plain; do run 1 "$dq" "$arm" clean; done
    for arm in no-incoming compute-no-incoming no-fragment no-transfer no-game-fragment; do run 1 "$dq" "$arm" raw; done
done
[ "$failed" = 0 ]
echo '【绿】NVN 屏障阶段、跨通道取样、传输 / 计算入向、片元依赖和同步负控'
