#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 图形 LibPack 存储缓冲槽的编号对账：出库 _b.json 的 sbuf reg ↔ 回放按 rp_S.ssbo[阶段][槽] 绑的槽。
#   真容器 → 生产取库合成 → 生产解析 / 布局 → 回放绘制核心 → lavapipe + 校验层，读回像素；同时用库侧 s2a_libpack_stage_sbuf_slots 的答案与
#   回放真去解析的存储地址逐位对账，再检查「只看程序真用到的槽」的引擎判定（nvk_shader.c gfx_ssbo_align）与同一套编号对得上。
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑，单独跑用 run.sh --libpack-sbuf-slots
# 撤改负控（各自必须红在指定断言）：
#   --negative-replay 把回放绑存储缓冲的槽号整体挪一位（rp_draw.c 三处 rp_S.ssbo[阶段][src_reg]）；
#   --negative-lib    把库侧报的槽集合挪一位（libpack_vk.c s2a_libpack_stage_sbuf_slots）；
#   --negative-engine 把引擎记「哪个槽非对齐」的位挪一位（nvk_shader.c gfx_ssbo_align）；
#   --negative-stage  把引擎读库时的阶段号换成另一段（nvk_shader.c gfx_ssbo_align）。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
mode=${1:-}
case "$mode" in ''|--negative-replay|--negative-lib|--negative-engine|--negative-stage) ;; *) exit 64 ;; esac
out="$repo/build/vk-libpack-sbuf-slots"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
glslc_bin=${GLSLC:-glslc}
for s in libpack_full.vert libpack_sbuf.frag; do
    "$glslc_bin" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$s" -o "$out/shaders/$s.spv"
done
inc=()
for d in android/native/gfx_plat android/tests/vkhost graphics/nvn/third_party/vulkan-headers/include graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/tex \
         graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_replay_libpack_sbuf_slots.c" "$repo/graphics/nvn/replay/rp_draw.c" "$repo/graphics/nvn/replay/rp_state.c" "$repo/graphics/nvn/replay/nvn_unimpl.c"
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
if mode == '--negative-replay':
    path, name = repo / 'graphics/nvn/replay/rp_draw.c', 'rp_draw_mutant.c'
    before, after, count = '&rp_S.ssbo[st][e->src_reg]', '&rp_S.ssbo[st][(e->src_reg + 1) & 15]', 3
elif mode == '--negative-lib':
    path, name = repo / 'android/native/direct/libpack_vk.c', 'libpack_mutant.c'
    before, after, count = '            mask |= 1u << reg;', '            mask |= 1u << ((reg + 1) & 15);', 1
elif mode == '--negative-engine':
    path, name = repo / 'android/native/gfx_plat/nvk_shader.c', 'nvk_shader_mutant.c'
    before, after, count = '{ un = 1; um[st] |= 1u << i; }', '{ un = 1; um[st] |= 1u << ((i + 1) & 15); }', 1
else:
    path, name = repo / 'android/native/gfx_plat/nvk_shader.c', 'nvk_shader_mutant.c'
    before, after, count = 'sbu_stage_slots(key, (unsigned)st, q->prim, &used)', 'sbu_stage_slots(key, (unsigned)(st ^ 1), q->prim, &used)', 1
s = path.read_text()
assert s.count(before) == count, 'mutation anchor changed: ' + mode
(out / name).write_text(s.replace(before, after))
PY
    case "$mode" in
        --negative-replay) src[1]="$out/rp_draw_mutant.c" ;;
        --negative-lib) src[6]="$out/libpack_mutant.c" ;;
        *) src[4]="$out/nvk_shader_mutant.c" ;;
    esac
    binary="$out/test-${mode#--}"; log="$out/${mode#--}.log"
fi
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections \
    -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$binary"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD"
export VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
set +e
timeout 120 "$binary" "$out/shaders" > "$log" 2>&1
rc=$?
set -e
if [ -n "$mode" ]; then
    # 撤改后必须红在断言上（不是编译 / 设备 / 校验层错）：期望的断言名对不上就不算击中
    case "$mode" in
        --negative-replay) expect='replay slots (aligned)' ;;
        --negative-lib) expect='library slots (aligned)' ;;
        --negative-engine) expect='gate used-unaligned-9: used slot unaligned' ;;
        *) expect='gate unused-unaligned: library hit' ;;
    esac
    if [ "$rc" = 0 ] || ! grep -q 'device: llvmpipe' "$log" || ! grep -q "FAIL .*: $expect" "$log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then cat "$log"; exit 1; fi
    echo '【负对照开始】'; echo "libpack-sbuf-slots $mode: PASS（撤改后红在「${expect}」）"; echo '【负对照结束】'   # 撤改按设计红出的断言不进门禁红步摘要（真失败上面已 cat 日志并退出，在标记之外）
    exit 0
fi
cat "$log"
if [ "$rc" != 0 ] || ! grep -q 'device: llvmpipe' "$log" || ! grep -q 'libpack sbuf slots numbering: failures=0' "$log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
    echo '【红】图形 LibPack 存储缓冲槽编号对账失败或有 Vulkan 校验错误' >&2; exit 1
fi
echo '【绿】图形 LibPack 存储缓冲槽编号：库里的 sbuf reg = 回放按 rp_S.ssbo 绑的槽（像素与解析集合都对上），只看用到的槽的判定同一套编号，validation 0 条'
for mutant in replay lib engine stage; do bash "$0" "--negative-$mutant"; done
