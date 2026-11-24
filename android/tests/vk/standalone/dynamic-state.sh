#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --<名>
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-dynamic-state"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/solid_green.frag" -o "$out/shaders/solid_green.frag.spv"
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/dynamic_input.vert" -o "$out/shaders/dynamic_input.vert.spv"
inc=()
for d in android/tests/vkhost graphics/nvn/third_party/vulkan-headers/include graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=("$repo/android/tests/vk/t_dynamic_state.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/nvn/nvn_vk_pipe_warm.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
wrap=()
for fn in vkGetPhysicalDeviceFeatures2 vkCreateDevice vkCreateGraphicsPipelines vkCreateImageView vkCreateFramebuffer vkCmdBeginRenderPass vkUpdateDescriptorSets vkCmdBindDescriptorSets vkCmdDraw vkCmdCopyImage vkGetDeviceProcAddr vkDestroyImageView vkWaitForFences; do wrap+=(-Wl,--wrap="$fn"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
# run.sh 在调用此独立入口之后才开同步校验，不能依赖父进程或本地包装器。
# 同计算写入负控一样同时打开 shader 访问启发式；仅加载 validation 层不够。
export VK_LAYER_VALIDATE_SYNC=1
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
run_case() {
    local dq=$1 eds1=$2 eds2=$3 vi=$4 want=$5 control=${6:-}
    local arm="dq${dq}-${eds1}${eds2}${vi}${control}"
    local requested=$((eds1 + eds2 * 2 + vi * 4)) rc=0
    local feature_env=()
    if [ "$control" = "-missing" ]; then feature_env+=(S2A_TEST_HIDE_EDS_FEATURES=1); fi
    env "${feature_env[@]}" "debug.s2a.nvk_eds1=$eds1" "debug.s2a.nvk_eds2=$eds2" "debug.s2a.nvk_vertex_input=$vi" \
        timeout 120 "$out/test" "$out/shaders" "$dq" "$want" "$out/$arm.rgba" "$requested" > "$out/$arm.log" 2>&1 || rc=$?
    if grep -qE 'Validation Error|VUID-' "$out/$arm.log"; then
        tail -40 "$out/$arm.log" >&2
        return 1
    fi
    if [ "$control" = "-withdraw" ]; then
        if [ "$rc" != 1 ] || ! grep -Fq 'dynamic state PSO count: expected=2 actual=12' "$out/$arm.log"; then
            tail -40 "$out/$arm.log" >&2
            return 1
        fi
    elif [ "$rc" != 0 ]; then
        tail -40 "$out/$arm.log" >&2
        return 1
    fi
    grep '^dynamic state:' "$out/$arm.log"
    cmp "$out/dq${dq}-000.rgba" "$out/$arm.rgba"
    if [ "$control" = "-withdraw" ]; then
        printf 'dynamic state negative dq=%s: PSO count rejected, both images match\n' "$dq"
    fi
}
for dq in 0 1; do
    run_case "$dq" 0 0 0 12
    run_case "$dq" 1 1 1 2
    run_case "$dq" 1 0 0 5
    run_case "$dq" 0 1 0 10
    run_case "$dq" 0 0 1 10
    run_case "$dq" 1 1 1 12 -missing
    run_case "$dq" 0 0 0 2 -withdraw
done
