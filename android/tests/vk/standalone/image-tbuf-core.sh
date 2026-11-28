#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --image-tbuf-core
# 图像槽里的缓冲贴图（imageBuffer，IMAGE_TBUF）：真 rp_dispatch + 池镜像 + 真 Vulkan 后端，lavapipe + VVL（含同步校验），读回逐字节核对。
# 正控三臂（对齐原地视图 / 不对齐走对齐副本 + 拷回 / 不对齐的图像原子）；负控只改 rp_draw.c 的临时副本，各撤一条新路径，必须命中对应断言变红。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-image-tbuf-core"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
for shader in image_tbuf_rmw.comp image_tbuf_atomic.comp; do
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
inc=(-I "$repo/build/c883-ndk")
for d in android/tests/vkhost android/tests/vk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
build() {   # $1 rp_draw.c 的路径，$2 产物名
    local src=("$repo/android/tests/vk/t_replay_image_tbuf.c" "$1")
    for f in graphics/nvn/replay/rp_buf.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
    cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function \
        -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
        "${inc[@]}" "${src[@]}" -Wl,--gc-sections -Wl,--wrap=vkAllocateDescriptorSets -Wl,--wrap=vkGetPhysicalDeviceFormatProperties \
        -lvulkan -lpthread -lm -o "$out/$2"
}
mutate() {   # $1 名字，$2 原文，$3 替换：只改临时副本，锚点不唯一就红（防止负控悄悄失效）
    python3 - "$repo/graphics/nvn/replay/rp_draw.c" "$out/rp_draw_$1.c" "$2" "$3" <<'PY'
import pathlib, sys
src, dst, a, b = sys.argv[1:]
s = pathlib.Path(src).read_text()
if s.count(a) != 1: raise SystemExit('image-tbuf negative control anchor changed: ' + a)
pathlib.Path(dst).write_text(s.replace(a, b))
PY
}
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
run() {   # $1 产物名，$2 可选 env（如 debug.s2a.*） → 设 rc，日志 $out/$1.log
    set +e
    if [ $# -ge 2 ] && [ -n "$2" ]; then
        env "$2" timeout 120 "$out/$1" "$out/shaders" > "$out/$1.log" 2>&1
    else
        timeout 120 "$out/$1" "$out/shaders" > "$out/$1.log" 2>&1
    fi
    rc=$?
    set -e
}
build "$repo/graphics/nvn/replay/rp_draw.c" core
run core
grep -E '^(device|minTexel|IMAGE_TBUF)' "$out/core.log" || true
if [ "$rc" -ne 0 ] || ! grep -q llvmpipe "$out/core.log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/core.log" ||
   ! grep -q 'IMAGE_TBUF replay: 3 arms, failures=0' "$out/core.log"; then
    tail -n 30 "$out/core.log"; echo "【红】imageBuffer 回放 rc=$rc 或 validation 有报错；完整日志 build/vk-image-tbuf-core/core.log" >&2; exit 1
fi
# 负控：各撤一条新路径。期望：rc ≠ 0、命中对应断言行、validation 0 条（只改回放逻辑，不关校验）
neg() {   # $1 名字，$2 期望命中的断言，$3 可选 env
    build "$out/rp_draw_$1.c" "neg-$1"; run "neg-$1" "${3:-}"
    if [ "$rc" -eq 0 ] || ! grep -q "$2" "$out/neg-$1.log" || grep -qE 'Validation Error|VUID-' "$out/neg-$1.log"; then
        tail -n 20 "$out/neg-$1.log"; echo "【红】负控 $1 未命中「$2」（rc=${rc}）" >&2; exit 1
    fi
    echo "负控 $1：rc=${rc}，命中「$2」"
}
mutate route 'if (e->src_kind == NVN_RB_IMG && e->cls == NVN_GPU_RES_IMAGE_TBUF) {   // 计算 imageBuffer' 'if (0) {   // 计算 imageBuffer'
neg route 'image_tbuf_rmw.comp @256: dispatch skipped'
mutate shadow 'if (al > 1 && (off % al) && !(off % elem)) {' 'if (0) {'
# shadow 负控验证「无 VK_EXT_texel_buffer_alignment 时，撤掉回放对齐副本后端必拒收 off=516」
neg shadow 'image_tbuf_rmw.comp @516: dispatch skipped' 'debug.s2a.nvk_texel_buffer_alignment=0'
mutate writeback '    texel_wb_flush(c);   // imageBuffer 的对齐副本：派发写完拷回池镜像' '    g_ntwb = 0;'
neg writeback 'image_tbuf_rmw.comp @516 element 0: 10 expected 31'
echo '【绿】imageBuffer 回放：对齐 / 不对齐副本 / 不对齐原子三臂逐字节对、护栏不动，validation 0 条；撤路由 / 撤副本 / 撤拷回三负控各自变红'
