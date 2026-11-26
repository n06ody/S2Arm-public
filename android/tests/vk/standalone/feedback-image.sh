#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --<名>
# 图形阶段存储图像与本通道的附件是同一张图（#999）：这一笔具名跳过并计数、不设粘性错误，同一段里后面的笔照常画。
# 真实驱动 + validation 层；同一份夹具上两个改坏项：① 恢复粘性错误 → 时间线失败；② 只计数不跳笔 → 重叠那几列被画上。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-feedback-image"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/feedback_image.frag" -o "$out/shaders/feedback_image.frag.spv"
for case in read snapshot; do
    args=()
    if [ "$case" = snapshot ]; then args=(-DWRITE_STORAGE=1); fi
    "$glslc" --target-env=vulkan1.1 "${args[@]}" "$repo/android/tests/vk/shaders/feedback_image_mixed.frag" \
        -o "$out/shaders/feedback_image_$case.frag.spv"
done
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=("$repo/android/tests/vk/t_feedback_image.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
build() {   # build <输出> <源文件…>（改坏项那两份把 backend_vulkan.c 换成 $mut 里的副本再传进来）
    local o=$1; shift
    cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
        -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
        "${inc[@]}" "$@" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$o"
}
build "$out/test" "${src[@]}"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
# 同 feedback-core：同步校验 + 着色器访问启发式一起开，只加载 validation 层不够
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
printf 'feedback-image validation: sync=1 shader_accesses_heuristic=1 settings=%s\n' "$VK_LAYER_SETTINGS_PATH"
failed=0
for dq in 0 1; do
    for opt in 0 1; do
        set +e
        timeout 120 "$out/test" "$out/shaders" "$dq" "$opt" 2>&1 | tee "$out/dq$dq-opt$opt.log"
        rc=${PIPESTATUS[0]}
        set -e
        if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-' "$out/dq$dq-opt$opt.log" ||
           ! grep -q 'feedback-image cases=1 failures=0' "$out/dq$dq-opt$opt.log" ||
           ! grep -q "feedback-image dq=$dq opt=$opt skips=2 commit-bad=0 wait-bad=0 fence-failed-bad=0 done-bad=0 count-bad=0 pixel-bad=0 guard-bad=0" "$out/dq$dq-opt$opt.log"; then
            echo "【红】feedback-image DQ=$dq OPT=$opt rc=$rc 或校验报错 / 读数不符" >&2; failed=1
        fi
        for case in storage-only storage-read storage-snapshot; do
            log="$out/$case-dq$dq-opt$opt.log"
            set +e
            timeout 120 "$out/test" "$out/shaders" "$dq" "$opt" "$case" 2>&1 | tee "$log"
            rc=${PIPESTATUS[0]}
            set -e
            if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
               ! grep -q "feedback-image $case cases=1 failures=0" "$log"; then
                echo "【红】feedback-image $case DQ=$dq OPT=$opt rc=$rc" >&2
                failed=1
            fi
        done
    done
done
# 改坏项：同一份夹具，只改后端这一处（复制一份 backend_vulkan.c / backend_vulkan_feedback.inc 到 ${mut}，include 先找本目录）。
mut="$out/mut"; mkdir -p "$mut"
cp "$repo/android/native/nvn/backend_vulkan.c" "$mut/backend_vulkan.c"
mutate() {   # mutate <改坏项名>：把「具名跳过 + return 0」这两行换掉，锚点不在就报错
    python3 - "$repo/android/native/nvn/backend_vulkan_feedback.inc" "$mut/backend_vulkan_feedback.inc" "$1" <<'PYMUT'
import pathlib, sys
src, dst, kind = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3]
s = src.read_text()
anchor = '        nvn_unimpl("draw", "graphics_image_feedback_overlap", NULL);\n        return 0;\n'
if s.count(anchor) != 1:
    raise SystemExit('feedback-image 改坏项锚点变了')
new = {
    'sticky': '        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "feedback storage image writes cannot use a sampled snapshot");\n        return 0;\n',
    'noskip': '        nvn_unimpl("draw", "graphics_image_feedback_overlap", NULL);\n',
}[kind]
dst.write_text(s.replace(anchor, new))
PYMUT
}
msrc=(); for f in "${src[@]}"; do [ "$f" = "$repo/android/native/nvn/backend_vulkan.c" ] && f="$mut/backend_vulkan.c"; msrc+=("$f"); done
for kind in sticky noskip; do
    mutate "$kind"
    build "$out/test-$kind" "${msrc[@]}"
    set +e
    timeout 120 "$out/test-$kind" "$out/shaders" 0 0 > "$out/neg-$kind.log" 2>&1
    mrc=$?
    set -e
    case "$kind" in
        sticky) want='commit-bad=1 wait-bad=1 fence-failed-bad=1 done-bad=1 count-bad=1' ;;   # 粘性错误：提交报错、时间线失败终态、done 不是 OK、没有计数
        noskip) want='count-bad=0 pixel-bad=[1-9]' ;;                                         # 计了数但没跳：重叠那两列被画上绿色，像素不符
    esac
    if [ "$mrc" -eq 0 ] || ! grep -qE "feedback-image dq=0 opt=0 .*$want" "$out/neg-$kind.log"; then
        echo "【红】feedback-image 改坏项「${kind}」没有按预期判红（rc=${mrc}）" >&2; tail -n 8 "$out/neg-$kind.log" >&2; failed=1
    else
        echo "feedback-image negative $kind: PASS（$(grep -E 'feedback-image dq=0 opt=0' "$out/neg-$kind.log" | cut -c1-200)）"
    fi
done
exit "$failed"
