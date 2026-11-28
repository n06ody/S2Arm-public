#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 图形 LibPack 的只有顶点阶段的深度程序（0x1）：真容器 → 生产取库合成 → 生产解析 / 布局 → 回放绘制核心 → lavapipe + 校验层，读回深度。
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑，单独跑用 run.sh --libpack-vsdepth
# 撤改负控（各自必须红在指定断言）：--negative-synth 把出库合成的入口闸改回只放 vs+fs / vs+gs+fs；--negative-fetch 把取库入口（nvk_shader.c）的闸改回只放 0x3 / 0x7。
# --negative-clip-control：从绘制复用键撤掉近裁模式，只应破坏模式切换中间一笔的深度读回。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
mode=${1:-}
case "$mode" in ''|--negative-synth|--negative-fetch|--negative-output|--negative-clip-control|--negative-mrt-memo) ;; *) exit 64 ;; esac
out="$repo/build/vk-libpack-vsdepth"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
glslc_bin=${GLSLC:-glslc}
for s in libpack_depth.vert libpack_clip_control.vert libpack_support.frag; do
    "$glslc_bin" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$s" -o "$out/shaders/$s.spv"
done
variant=0
for kind in float uint sint mixed; do
    "$glslc_bin" --target-env=vulkan1.1 "-DMRT_CLASS=$variant" "$repo/android/tests/vk/shaders/libpack_mrt_outputs.frag" -o "$out/shaders/mrt-$kind.spv"
    variant=$((variant+1))
done
python3 - "$out/shaders" <<'PYMRT'
import base64, json, sys
from pathlib import Path
root=Path(sys.argv[1])
def stage(name, path):
    return dict(stage=name, spirv=base64.b64encode((root/path).read_bytes()).decode())
program=dict(contract='s2a-direct-program-content-v1',kind='graphics',c0Reads=[],supportReads=[],
             stages=[stage('vs','libpack_depth.vert.spv'),stage('fs','mrt-uint.spv')],bindings=[])
(root/'mrt-ordinary.json').write_text(json.dumps(dict(schema='s2a-direct-program-v1',content=program)))
PYMRT
inc=()
for d in android/native/gfx_plat android/tests/vkhost graphics/nvn/third_party/vulkan-headers/include graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/tex \
         graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_replay_libpack_vsdepth.c" "$repo/graphics/nvn/replay/rp_draw.c" "$repo/graphics/nvn/replay/rp_state.c" "$repo/graphics/nvn/replay/nvn_unimpl.c"
     "$repo/android/native/gfx_plat/nvk_shader.c" "$repo/android/native/nvn/nvn_vk_prog_warm.c" "$repo/android/native/direct/libpack_vk.c"
     "$repo/android/native/nvn/backend_vulkan.c" "$repo/android/native/nvn/nvn_pipe_flight.c" "$repo/android/native/nvn/nvn_vk_pipe_once.c"
     "$repo/android/native/direct/vk_device.c" "$repo/android/native/direct/gs_path.c" "$repo/android/native/capprobe/capprobe_host.c"
     "$repo/android/native/nvn/nvn_vk_format.c" "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c" "$repo/android/native/nvn/nvn_vk_bcdec.c"
     "$repo/android/native/nvn/nvn_vk_program_result.c" "$repo/android/native/nvn/nvn_vk_compute_request.c" "$repo/android/native/nvn/nvn_vk_driver_cache.c"
     "$repo/android/native/direct/source_store.c" "$repo/android/native/direct/control.c" "$repo/android/native/direct/input_identity.c" "$repo/android/native/direct/texture_facts.c"
     "$repo/android/native/direct/sha256.c" "$repo/android/native/direct/json.c" "$repo/android/native/direct/jv.c" "$repo/android/native/direct/compile_request.c" "$repo/android/native/direct/compile_worker.c"
     "$repo/android/native/direct/offline_library.c" "$repo/android/native/direct/program_cache.c" "$repo/runtime/src/direct_host.c"
     "$repo/graphics/nvn/gfx/nvn/nvn_tex.c" "$repo/graphics/nvn/gfx/tex/nvn_deswizzle.c" "$repo/android/native/nvn/nvn_vk_pipe_warm.c")
binary="$out/test"; log="$out/run.log"
args=()
if [ -n "$mode" ]; then
    python3 - "$repo" "$out" "$mode" <<'PY'
import pathlib, sys
repo, out, mode = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3]
if mode == '--negative-synth':
    path, name = repo / 'android/native/direct/libpack_vk.c', 'libpack_mutant.c'
    before, after = 'if (stages != 0x1 && stages != 0x3 && stages != 0x7) {', 'if (stages != 0x3 && stages != 0x7) {'
elif mode == '--negative-output':
    path, name = repo / 'android/native/gfx_plat/nvk_shader.c', 'nvk_shader_mutant.c'
    before, after = 'uint32_t written = f->color_writes & e->outputs.components;', 'uint32_t written = 0;'
elif mode == '--negative-mrt-memo':
    path, name = repo / 'android/native/gfx_plat/nvk_shader.c', 'nvk_shader_mutant.c'
    before, after = 'if (color && (color->uint_locations | color->sint_locations)) {', 'if (0 && color && (color->uint_locations | color->sint_locations)) {'
elif mode == '--negative-fetch':
    path, name = repo / 'android/native/gfx_plat/nvk_shader.c', 'nvk_shader_mutant.c'
    before, after = '(!ckey && stages != 0x1u && stages != 0x3u && stages != 0x7u)', '(!ckey && stages != 0x3u && stages != 0x7u)'
else:
    path, name = repo / 'graphics/nvn/replay/rp_draw.c', 'rp_draw_mutant.c'
    before, after = ' | (clip_neg1 << 2);', ';'
s = path.read_text()
assert s.count(before) == 1, 'mutation anchor changed: ' + mode
(out / name).write_text(s.replace(before, after))
PY
    case "$mode" in
        --negative-synth) src[6]="$out/libpack_mutant.c" ;;
        --negative-fetch) src[4]="$out/nvk_shader_mutant.c" ;;
        --negative-output) src[4]="$out/nvk_shader_mutant.c"; args=(mrt-float) ;;
        --negative-mrt-memo) src[4]="$out/nvk_shader_mutant.c"; args=(mrt-vf) ;;
        --negative-clip-control) src[1]="$out/rp_draw_mutant.c"; args=(--clip-control) ;;
    esac
    binary="$out/test-${mode#--}"; log="$out/${mode#--}.log"
fi
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections \
    -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" -Wl,--gc-sections -Wl,--wrap=vkCreateGraphicsPipelines -lvulkan -lpthread -lm -o "$binary"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD"
export VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
set +e
timeout 120 "$binary" "$out/shaders" "${args[@]}" > "$log" 2>&1
rc=$?
set -e
if [ -z "$mode" ] && [ "$rc" = 0 ]; then
    timeout 120 "$binary" "$out/shaders" --clip-control >> "$log" 2>&1 || rc=$?
fi
if [ -n "$mode" ]; then
    # 撤改后必须红在断言上（不是编译 / 设备 / 校验层错）：期望的断言名对不上就不算击中
    expect='vs-only: library hit'
    if [ "$mode" = --negative-clip-control ]; then
        expect='clipControl pass 1 depth pixels differ'
        if ! grep -qx 'libpack clip control: failures=1' "$log"; then cat "$log"; exit 1; fi
    fi
    if [[ $mode == --negative-output ]]; then expect='MRT admission: compatible=0 rc=0 compiler=0'; fi
    if [[ $mode == --negative-mrt-memo ]]; then expect='MRT variants: attachment set 1 hits LibPack without compiling'; fi
    if [ "$rc" = 0 ] || ! grep -q 'device: llvmpipe' "$log" || ! grep -q "FAIL .*: $expect" "$log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then cat "$log"; exit 1; fi
    echo '【负对照开始】'
    echo "libpack-vsdepth $mode: PASS（撤改后红在「${expect}」）"
    echo '【负对照结束】'
    exit 0
fi
cat "$log"
if [ "$rc" != 0 ] || ! grep -q 'device: llvmpipe' "$log" || ! grep -q 'libpack vs-only depth: failures=0' "$log" ||
   ! grep -q 'libpack clip control: failures=0' "$log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
    echo '【红】图形 LibPack 只有 vs 的深度程序或同段裁剪模式切换失败，或有 Vulkan 校验错误' >&2; exit 1
fi
echo '【绿】图形 LibPack 只有 vs 的深度程序：取库命中、深度读回 0.5、前一笔的片元绑定不带过来，validation 0 条'
echo '【绿】同段只切近裁模式的深度读回 1/0.25/1，只切深度夹取的对照为 1/0/1'
for kind in float uint; do
    log="$out/mrt-$kind.log"
    S2A_TEST_FOUT_CAPTURE=1 timeout 120 "$binary" "$out/shaders" "mrt-$kind" > "$log" 2>&1 || { cat "$log"; exit 1; }
    if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then cat "$log"; exit 1; fi
    grep '^libpack MRT .*integer_pixels=48 failures=0$' "$log"
    if [ "$kind" = float ]; then
        grep -Eq $'libpack_fout_want\t[0-9a-f]{16}\te\t0$' "$log"
    elif grep -q 'libpack_fout_want' "$log"; then
        echo '【红】整数输出库件已匹配，仍错误记录为缺失变体' >&2
        exit 1
    fi
done
log="$out/mrt-vf.log"
timeout 120 "$binary" "$out/shaders" mrt-vf > "$log" 2>&1 || { cat "$log"; exit 1; }
if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then cat "$log"; exit 1; fi
grep -qx 'libpack MRT variants: draws=7 pixels=336 compiler=0 failures=0' "$log"
tail -n 1 "$log"
work=$(mktemp -d "$out/warm.XXXXXX")
cp "$out/shaders/libpack_depth.vert.spv" "$out/shaders/libpack_support.frag.spv" "$work"
python3 - "$work" <<'PYWARM'
import base64
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
spirv = base64.b64encode((root / 'libpack_depth.vert.spv').read_bytes()).decode()
identity = dict(directABI=1, translatorClosure='libpack-c0-test', profileHash='sha256-'+'a'*64)
program = dict(contract='s2a-direct-program-content-v1', kind='graphics', identity=identity,
               stages=[dict(stage='vs', spirv=spirv)], bindings=[], c0Reads=[], supportReads=[])
(root / 'warm-ordinary.json').write_text(json.dumps(dict(schema='s2a-direct-program-v1', content=program)))
PYWARM
for slots in 1 2 4; do
    log="$out/submit-$slots.log"
    timeout 120 "$binary" "$work" "submit-$slots" > "$log" 2>&1 || { cat "$log"; exit 1; }
    if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then cat "$log"; exit 1; fi
    grep -qx "submit compile slots=$slots depth_pixels=16 failures=0" "$log"
    echo "【绿】提交前瞻 $slots 槽：只现编一次、零跳画、深度读回 16/16，validation 0 条"
done
for arm in donor late aligned different reuse; do
    log="$out/warm-$arm.log"
    timeout 120 "$binary" "$work" "warm-$arm" > "$log" 2>&1 || { cat "$log"; exit 1; }
    if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-|resolve_fast_path_mismatch|快路径给的程序与慢路径不一致' "$log"; then cat "$log"; exit 1; fi
    grep '^libpack warm .*depth_pixels=16 failures=0$' "$log"
done
for mutant in synth fetch output clip-control mrt-memo; do bash "$0" "--negative-$mutant"; done
