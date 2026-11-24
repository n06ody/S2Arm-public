#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# run.sh --libpack-integer. Only the diagnostic negative bypasses admission.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-libpack-integer"
mkdir -p "$out/shaders"
compiler=${GLSLC:-glslc}
shaderdir="$repo/android/tests/vk/shaders"
"$compiler" --target-env=vulkan1.1 "$shaderdir/replay_front_face.vert" -o "$out/shaders/fullscreen.vert.spv"
for kind in uint float; do
    wrong=0; if [[ $kind = float ]]; then wrong=1; fi
    "$compiler" --target-env=vulkan1.1 -DFLOAT_INPUTS=$wrong "$shaderdir/libpack_integer_inputs.frag" -o "$out/shaders/$kind.frag.spv"
done
python3 - "$repo/android/native/gfx_plat/nvk_shader.c" "$out/withdrawn.c" <<'PY'
from pathlib import Path
import sys
text=Path(sys.argv[1]).read_text()
guard='if (!e->prog.lib_sourced || e->numeric_checked) return 1;'
assert text.count(guard)==1
Path(sys.argv[2]).write_text(text.replace(guard,'if (e->prog.lib_sourced || e->numeric_checked) return 1;'))
PY
flags=(-D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter
       -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1)
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct android/native/gfx_plat runtime/include; do
    flags+=(-I "$repo/$d")
done
objs=()
for f in android/tests/vk/t_libpack_integer.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c android/native/direct/texture_facts.c graphics/nvn/replay/nvn_unimpl.c graphics/nvn/gfx/nvn/nvn_tex.c graphics/nvn/gfx/tex/nvn_deswizzle.c; do
    obj="$out/$(basename "$f" .c).o"
    cc "${flags[@]}" -c "$repo/$f" -o "$obj"
    objs+=("$obj")
done
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR"
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
for arm in withdrawn fixed; do
    extra=()
    if [[ $arm = withdrawn ]]; then extra+=("-DNVK_NUMERIC_PRODUCER=\"$out/withdrawn.c\""); fi
    cc "${flags[@]}" "${extra[@]}" -c "$repo/android/tests/vk/t_libpack_numeric_select.c" -o "$out/selector.o"
    cc "${objs[@]}" "$out/selector.o" -Wl,--gc-sections -lvulkan -lpthread -lm -ldl -o "$out/test"
    log="$out/$arm.log"
    timeout 60 "$out/test" "$out/shaders" "$arm" >"$log" 2>&1 || { cat "$log"; exit 1; }
    if [[ $arm = withdrawn ]]; then
        if ! grep -q 'VUID-vkCmdDraw-format-07753' "$log" ||
           grep -E 'VUID-|Validation Error|SYNC-HAZARD-' "$log" | grep -v 'VUID-vkCmdDraw-format-07753'; then cat "$log"; exit 1; fi
        echo '整数附件撤核对命中 vkCmdDraw 类型校验 07753'
        # The outer container rejects any VUID in stdout. Retain the actual
        # expected errors separately after checking the complete negative log.
        printf 'INTEGER_NEGATIVE_LOG_BASE64='
        grep -F 'VUID-vkCmdDraw-format-07753' "$log" | base64 -w 0
        echo
    elif grep -qE 'VUID-|Validation Error|SYNC-HAZARD-' "$log"; then cat "$log"; exit 1; fi
    grep '^integer attachment arm=' "$log"
done
