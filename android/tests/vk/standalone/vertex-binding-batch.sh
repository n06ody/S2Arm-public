#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-vertex-binding-batch"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/solid_green.frag" -o "$out/shaders/solid_green.frag.spv"
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=("$repo/android/tests/vk/t_vertex_binding_batch.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
wrap=()
for fn in nvn_plat_flag vkCreateBuffer vkCreateImageView vkCreateFramebuffer vkCmdBeginRenderPass vkUpdateDescriptorSets vkCmdBindDescriptorSets vkCmdDraw vkCmdCopyImage vkGetDeviceProcAddr vkDestroyImageView vkWaitForFences; do wrap+=(-Wl,--wrap="$fn"); done
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
failed=0
for dq in 0 1; do
    for batch in 0 1; do
        log="$out/dq$dq-batch$batch.log"
        if ! timeout 120 "$out/test" "$out/shaders" "$dq" "$batch" > "$log" 2>&1 ||
           grep -qE 'Validation Error|VUID-' "$log"; then
            echo "【红】顶点绑定 DQ=$dq batch=$batch" >&2
            tail -40 "$log" >&2
            failed=1
        else
            grep '^vertex binding ' "$log"
        fi
    done
    # Same binary, opt out, but keep the assertions for grouped recording.
    log="$out/dq$dq-withdraw.log"
    rc=0
    timeout 120 "$out/test" "$out/shaders" "$dq" 2 > "$log" 2>&1 || rc=$?
    if [ "$rc" != 2 ] || grep -qE 'Validation Error|VUID-' "$log" ||
       ! grep -q 'calls=5/0/4/0/16 draw-state=ok' "$log" || ! grep -Fq 'want_fast : want_legacy' "$log"; then
        echo "【红】顶点绑定撤回对照 DQ=$dq" >&2
        tail -40 "$log" >&2
        failed=1
    else
        printf 'vertex binding negative dq=%s: call count rejected; draw state ok\n' "$dq"
    fi
done
exit "$failed"
