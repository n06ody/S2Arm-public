#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 图形 LibPack 的 FS c0 / c2 驱动常量缓冲（drvcb bank 0 / 2）：真容器 → 生产取库合成 → 生产解析 / 布局 → 回放绘制核心 → lavapipe + 校验层，读回像素。
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑，单独跑用 run.sh --libpack-c0
# 撤改负控（各自必须红在指定断言）：--negative-synth 撤掉出库合成对图形 bank 0 / 2 的映射；--negative-drv0 把回放的 FS 存储地址表挪个位置；--negative-bank2 把 bank 2 错挂成 slot 0（读到 c0 而不是句柄表）。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
mode=${1:-}
case "$mode" in ''|--negative-synth|--negative-drv0|--negative-bank2) ;; *) exit 64 ;; esac
out="$repo/build/vk-libpack-c0"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
glslc_bin=${GLSLC:-glslc}
for s in libpack_full.vert libpack_c0.frag; do
    "$glslc_bin" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$s" -o "$out/shaders/$s.spv"
done
inc=()
for d in android/tests/vkhost graphics/nvn/third_party/vulkan-headers/include graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/tex \
         graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_replay_libpack_c0.c" "$repo/graphics/nvn/replay/rp_draw.c" "$repo/graphics/nvn/replay/rp_state.c" "$repo/graphics/nvn/replay/nvn_unimpl.c"
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
if mode == '--negative-synth':
    path, name = repo / 'android/native/direct/libpack_vk.c', 'libpack_mutant.c'
    before, after = 'else if (!compute && has_bank && (bank == 0 || bank == 2)) {', 'else if (0 && !compute && has_bank && (bank == 0 || bank == 2)) {'
elif mode == '--negative-bank2':
    path, name = repo / 'android/native/direct/libpack_vk.c', 'libpack_mutant.c'
    before, after = 'slot = bank; has_slot = 1;\n            }', 'slot = 0; has_slot = 1;\n            }'
else:
    path, name = repo / 'graphics/nvn/replay/rp_draw.c', 'rp_draw_mutant.c'
    before, after = 'static const int k_drv0_base[5] = {0x110, 0x510,', 'static const int k_drv0_base[5] = {0x110, 0x500,'
s = path.read_text()
assert s.count(before) == 1, 'mutation anchor changed: ' + mode
(out / name).write_text(s.replace(before, after))
PY
    if [ "$mode" = --negative-synth ] || [ "$mode" = --negative-bank2 ]; then src[6]="$out/libpack_mutant.c"; else src[1]="$out/rp_draw_mutant.c"; fi
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
    case "$mode" in --negative-synth) expect='bank 0 c0: library hit' ;; --negative-bank2) expect='bank 2 c2 pixel' ;; *) expect='bank 0 c0 pixel' ;; esac
    if [ "$rc" = 0 ] || ! grep -q 'device: llvmpipe' "$log" || ! grep -q "FAIL .*: $expect" "$log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then cat "$log"; exit 1; fi
    echo "libpack-c0 $mode: PASS（撤改后红在「${expect}」）"
    exit 0
fi
cat "$log"
if [ "$rc" != 0 ] || ! grep -q 'device: llvmpipe' "$log" || ! grep -q 'libpack FS c0 drvcb: failures=0' "$log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
    echo '【红】图形 LibPack FS c0 回放失败或有 Vulkan 校验错误' >&2; exit 1
fi
echo '【绿】图形 LibPack FS c0：bank 0 读到槽 15 地址字节、bank 2 读到句柄表里的采样器句柄，错绑定号具名拒，validation 0 条'
for mutant in synth drv0 bank2; do bash "$0" "--negative-$mutant"; done
