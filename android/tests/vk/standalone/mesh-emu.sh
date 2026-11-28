#!/usr/bin/env bash
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑，可单跑 --mesh-emu。
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="${S2A_MESH_EMU_OUT:-$repo/build/vk-mesh-emu}"
mkdir -p "$out"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
for s in mesh_emu_task.comp mesh_emu_mesh.comp mesh_emu_mv.vert mesh_emu_fs.frag mesh_emu_ind_write.comp; do
    base="${s%.*}"
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$s" -o "$out/$base.spv"
done

inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/tex graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=(
    "$repo/android/tests/vk/t_replay_mesh_emu.c"
    "$repo/graphics/nvn/replay/rp_draw.c"
    "$repo/graphics/nvn/replay/rp_buf.c"
    "$repo/graphics/nvn/replay/rp_state.c"
    "$repo/graphics/nvn/replay/nvn_unimpl.c"
    "$repo/graphics/nvn/gfx/nvn/nvn_tex.c"
    "$repo/graphics/nvn/gfx/tex/nvn_deswizzle.c"
)
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do
    src+=("$repo/$f")
done

cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]}" \
    -Wl,--wrap=nvn_gpu_caps_get -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"

export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"

for dq in 0 1; do
    log="$out/dq$dq-pass.log"
    timeout 30 "$out/test" "$out" "$dq" > "$log" 2>&1 || { cat "$log"; exit 1; }
    if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
        cat "$log"
        exit 1
    fi
    grep '^mesh-emu ' "$log"
done

log="$out/neg.log"
echo "【负对照开始】"
timeout 30 "$out/test" "$out" 0 neg > "$log" 2>&1 || { cat "$log"; echo "【负对照结束】"; exit 1; }
echo "【负对照结束】"
if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
    cat "$log"
    exit 1
fi
grep '^mesh-emu neg' "$log"
echo "✓ mesh-emu：Vulkan 1.1 任务 / 网格计算模拟正向 5 路 + 辅助核 9 组对账 + 超限/缺能力负对照全过（VVL 同步校验 0 错误）"
