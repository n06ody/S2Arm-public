#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 图形 LibPack 存储缓冲窗口补偿：出包库顶点段（翻译器重译、带窗口掩码）→ 生产取库合成（按设备对齐 A 固化掩码）→ 生产解析 / 布局 →
#   回放绘制核心（c0 写原始基址、窗口从 B & -A 起）→ 真实 rp_buf → lavapipe + 校验层（含同步校验）→ 回写客体内存，逐字节比对。
# 料：build/libpack-ssbo-window/stages（宿主出料：S2A_DSH_VK_EMIT_DIR=<仓库>/build/libpack-ssbo-window/stages bash graphics/tools/direct/tests/t_dsh_vk.sh，
#   要 dotnet，容器里没有，由 android/tests/vk/run.sh 在宿主先出；缺料不静默跳过，红）。单独跑用 run.sh --libpack-ssbo-window
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑
# 撤改负控（各自必须红在「guest bytes」断言，且没有校验层错）：
#   --negative-mask-double 固化的掩码按 2A 取（错档）；--negative-mask-half 按 A/2 取；--negative-window 回放把窗口起点挪出一个 A（基址跨窗）；
#   --negative-cs-gate 计算取库把「非对齐」仍当闸（修前口径）：计算档的非对齐基址不再命中库，红在「library hit」。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
mode=${1:-}
case "$mode" in ''|--negative-mask-double|--negative-mask-half|--negative-window|--negative-cs-gate) ;; *) exit 64 ;; esac
stages="${S2A_SSBO_WINDOW_STAGES:-$repo/build/libpack-ssbo-window/stages}"
[ -f "${stages}/window_vs.key" ] || { echo "【红】缺宿主出料 ${stages}（见本脚本文件头）" >&2; exit 2; }
out="$repo/build/vk-libpack-ssbo-window"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
glslc_bin=${GLSLC:-glslc}
for f in libpack_window libpack_window_real; do
    "$glslc_bin" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$f.frag" -o "$out/shaders/$f.frag.spv"
done
"$glslc_bin" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/libpack_full.vert" -o "$out/shaders/libpack_full.vert.spv"
# 可选的私有真实程序料（只放工作树的 build/ 下、不入仓；格式同 stages：window_vs.key + <键>_b.json / _b.spv）：有就多跑一档真实顶点段的颜色比对
real="${S2A_SSBO_WINDOW_REAL_STAGES:-$repo/build/libpack-ssbo-window/real-stages}"
real_arg=(); if [ -f "$real/window_vs.key" ]; then real_arg=("$real"); echo "真实程序档：$real"; fi
inc=()
for d in android/tests/vkhost graphics/nvn/third_party/vulkan-headers/include graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/tex \
         graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct android/native/gfx_plat runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_replay_libpack_ssbo_window.c" "$repo/graphics/nvn/replay/rp_draw.c" "$repo/graphics/nvn/replay/rp_buf.c" "$repo/graphics/nvn/replay/rp_state.c" "$repo/graphics/nvn/replay/nvn_unimpl.c"
     "$repo/android/native/gfx_plat/nvk_shader.c" "$repo/android/native/nvn/nvn_vk_prog_warm.c" "$repo/android/native/direct/libpack_vk.c"
     "$repo/android/native/nvn/backend_vulkan.c" "$repo/android/native/nvn/nvn_pipe_flight.c" "$repo/android/native/nvn/nvn_vk_pipe_once.c"
     "$repo/android/native/direct/vk_device.c" "$repo/android/native/direct/gs_path.c" "$repo/android/native/capprobe/capprobe_host.c"
     "$repo/android/native/nvn/nvn_vk_format.c" "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c" "$repo/android/native/nvn/nvn_vk_bcdec.c"
     "$repo/android/native/nvn/nvn_vk_program_result.c" "$repo/android/native/nvn/nvn_vk_compute_request.c" "$repo/android/native/nvn/nvn_vk_driver_cache.c"
     "$repo/android/native/direct/source_store.c" "$repo/android/native/direct/control.c" "$repo/android/native/direct/input_identity.c" "$repo/android/native/direct/texture_facts.c"
     "$repo/android/native/direct/sha256.c" "$repo/android/native/direct/json.c" "$repo/android/native/direct/jv.c" "$repo/android/native/direct/compile_request.c" "$repo/android/native/direct/compile_worker.c"
     "$repo/android/native/direct/offline_library.c" "$repo/android/native/direct/program_cache.c" "$repo/runtime/src/direct_host.c"
     "$repo/graphics/nvn/gfx/nvn/nvn_tex.c" "$repo/graphics/nvn/gfx/tex/nvn_deswizzle.c")
binary="$out/test"; log="$out/run.log"
if [ -n "$mode" ]; then
    python3 - "$repo" "$out" "$mode" <<'PY'
import pathlib, sys
repo, out, mode = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3]
if mode == '--negative-cs-gate':
    path, name = repo / 'android/native/gfx_plat/nvk_shader.c', 'nvk_shader_mutant.c'
    before = 'unaligned ? LIBPACK_SSBO_WINDOW : LIBPACK_ALIGNED, NULL, &pc, &bytes, &len)'
    after = 'unaligned ? LIBPACK_OTHER_STATE : LIBPACK_ALIGNED, NULL, &pc, &bytes, &len)'
elif mode == '--negative-window':
    path, name = repo / 'graphics/nvn/replay/rp_draw.c', 'rp_draw_mutant.c'
    before = 'if (align_down && cp->min_ssbo_align > 1) { const uint64_t r = a & (uint64_t)(cp->min_ssbo_align - 1); a -= r; l += r; }'
    after = 'if (align_down && cp->min_ssbo_align > 1) { const uint64_t r = a & (uint64_t)(cp->min_ssbo_align - 1); a -= r; l += r; a += cp->min_ssbo_align; l -= cp->min_ssbo_align; }'
else:
    path, name = repo / 'android/native/direct/libpack_vk.c', 'libpack_mutant.c'
    before = 'w[constant + 3] = 0u - alignment;'
    after = 'w[constant + 3] = 0u - (alignment * 2u);' if mode == '--negative-mask-double' else 'w[constant + 3] = 0u - (alignment / 2u);'
s = path.read_text()
assert s.count(before) == 1, 'mutation anchor changed: ' + mode
(out / name).write_text(s.replace(before, after))
PY
    case "$mode" in --negative-window) src[1]="$out/rp_draw_mutant.c" ;; --negative-cs-gate) src[5]="$out/nvk_shader_mutant.c" ;; *) src[7]="$out/libpack_mutant.c" ;; esac
    binary="$out/test-${mode#--}"; log="$out/${mode#--}.log"
fi
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections \
    -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$binary"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD"
export VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
set +e
timeout 600 "$binary" "$out/shaders" "$stages" ${real_arg[@]+"${real_arg[@]}"} > "$log" 2>&1
rc=$?
set -e
if [ -n "$mode" ]; then
    # 撤改后必须红在字节断言上（不是编译 / 设备 / 校验层错），而且不能全红：对齐且余数 0 的用例里错掩码仍可能碰巧对，所以只要求有断言红
    if [ "$mode" = --negative-cs-gate ]; then pat='FAIL .*: cs-A[0-9]+-rem[0-9]+ library hit'; what='library hit'; else pat='FAIL .*: A[0-9]+-rem[0-9]+ guest bytes'; what='guest bytes'; fi
    if [ "$rc" = 0 ] || ! grep -q 'device: llvmpipe' "$log" || ! grep -qE "$pat" "$log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then cat "$log"; exit 1; fi
    echo "libpack-ssbo-window $mode: PASS（撤改后红在「${what}」：$(grep -cE "$pat" "$log") 档）"
    exit 0
fi
cat "$log"
if [ "$rc" != 0 ] || ! grep -q 'device: llvmpipe' "$log" || ! grep -q 'libpack SSBO window: failures=0' "$log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
    echo '【红】图形 LibPack 存储缓冲窗口回放失败或有 Vulkan 校验错误' >&2; exit 1
fi
echo '【绿】图形 LibPack 存储缓冲窗口：A=16/64/256 × 对齐 / 非对齐基址 12 档客体字节与手算一致，旧库非对齐拒库，坏合同具名拒，validation 0 条'
for mutant in mask-double mask-half window cs-gate; do bash "$0" "--negative-$mutant"; done
