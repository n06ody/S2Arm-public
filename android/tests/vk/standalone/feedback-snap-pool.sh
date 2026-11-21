#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --feedback-snap-pool
# 反馈快照池（快照图跨段复用，backend_vulkan_feedback.inc 的 fbpool_*）：t_feedback_pool.c 六个用例 × 两级回放开 / 关，
# 再用同一份用例跑三个改坏的后端（负对照），每个都必须红在它该红的那一行。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-feedback-snap-pool"
mkdir -p "$out/shaders" "$out/mut"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/feedback.frag" -o "$out/shaders/feedback.frag.spv"
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
build() {   # $1 = 产物名，$2 = 后端目录（正式源码或改坏的副本）
    local inc=(-I "$2")
    for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
    local src=("$repo/android/tests/vk/t_feedback_pool.c" "$repo/graphics/nvn/replay/nvn_unimpl.c" "$2/backend_vulkan.c")
    for f in android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
    cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
        -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
        "${inc[@]}" "${src[@]}" -Wl,--wrap=vkCreateImage -Wl,--wrap=vkAllocateMemory -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/$1"
}
build test "$repo/android/native/nvn"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
failed=0
for dq in 0 1; do
    set +e
    timeout 180 "$out/test" "$out/shaders" "$dq" 2>&1 | tee "$out/dq$dq.log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-' "$out/dq$dq.log" || ! grep -q 'fbpool cases=7 failures=0' "$out/dq$dq.log"; then
        echo "【红】feedback-snap-pool DQ=$dq rc=$rc 或有校验错误" >&2; failed=1
    fi
done
# 负对照：各改坏后端一处（python 精确替换，替换点对不上 = 本脚本红，不静默变成「没改」），用例必须在对应那一行红。
mutate() {   # $1 = 名，$2 = 文件（backend_vulkan.c / backend_vulkan_feedback.inc），其后成对：旧串 新串
    local name=$1 file=$2; shift 2
    rm -rf "$out/mut/$name"; mkdir -p "$out/mut/$name"
    cp "$repo/android/native/nvn/"backend_vulkan*.c "$repo/android/native/nvn/"backend_vulkan*.inc "$repo/android/native/nvn/"backend_vulkan*.h "$out/mut/$name/"
    python3 - "$out/mut/$name/$file" "$@" <<'PY'
import sys
p, pairs = sys.argv[1], sys.argv[2:]
s = open(p).read()
for old, new in zip(pairs[0::2], pairs[1::2]):
    if s.count(old) != 1: sys.exit("mutation anchor count %d: %r" % (s.count(old), old))
    s = s.replace(old, new)
open(p, 'w').write(s)
PY
    build "mut-$name" "$out/mut/$name"
}
negative() {   # $1 = 名，$2 = 期望命中的红行（grep -E）
    set +e
    timeout 180 "$out/mut-$1" "$out/shaders" 0 > "$out/neg-$1.log" 2>&1
    local rc=$?
    set -e
    if [ "$rc" -ne 1 ] || ! grep -qE "$2" "$out/neg-$1.log"; then
        echo "【红】负对照 $1 没有红在预期处（rc=${rc}）" >&2; tail -n 30 "$out/neg-$1.log"; failed=1
    else
        echo "feedback-snap-pool negative $1: PASS（用例红 $(grep -c '^  【红】' "$out/neg-$1.log") 行，含预期行）"
        grep '^  【红】' "$out/neg-$1.log"
    fi
}
# ① 放回不等段执行完：没执行（abort）的段的新建快照也进池
mutate unfenced backend_vulkan.c 'if (executed && f->snap) f->snap->fb_ready = 1;' 'if (f->snap) f->snap->fb_ready = 1;'
negative unfenced '【红】abort：放弃的段的快照不应进池'
# ② 形状键少一项（宽度）：4x4 的快照被拿去给 8x4 的源用
mutate keyw backend_vulkan_feedback.inc 'a->nvn_format == b->nvn_format && a->w == b->w &&' 'a->nvn_format == b->nvn_format &&'
negative keyw '【红】shape：期望 新建 2 / 命中 0'
# ③ 池里取出的图不重新拷：第二段采样到池里的旧内容（红 96 而不是 224）
mutate nocopy backend_vulkan_feedback.inc \
    'const int fresh = !t;   // 新建' 'int mut_pooled = 0; const int fresh = !t;   // 新建' \
    't = fbpool_take(&key, s);' 't = fbpool_take(&key, s); mut_pooled = t != NULL;' \
    'vkCmdCopyImage(c->cb, s->image,' 'if (!mut_pooled) vkCmdCopyImage(c->cb, s->image,'
negative nocopy '【红】hit：第二段像素期望红 224'
exit "$failed"
