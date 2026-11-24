#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认按 standalone/*.sh 通配执行。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-bc1-astc"
mkdir -p "$out/negative"
astc="$repo/graphics/third_party/astc-encoder"
"${CXX:-c++}" -std=c++14 -O1 -fPIC -shared -DNDEBUG -DASTCENC_BLOCK_MAX_TEXELS=16 \
    "$astc"/astcenc_*.cpp -o "$out/libastcdecode.so"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
inc=(-I "$repo/build/c883-ndk" -I "$astc")
for d in android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_bc1_astc.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c graphics/nvn/gfx/nvn/nvn_tex.c graphics/nvn/gfx/tex/nvn_deswizzle.c graphics/nvn/gfx/tex/nvn_bcdec.c; do
    src+=("$repo/$f")
done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" \
    -Wl,--gc-sections -L"$out" -Wl,-rpath,"$out" -lastcdecode -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
set +e
timeout 180 "$out/test" 2>&1 | tee "$out/run.log"
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/run.log"; then
    echo "【红】BC1 ASTC fixture rc=$rc" >&2
    exit 1
fi

# 破坏透明块标记，只重新编着色器；像素正控继续通过，整张纹理的回落断言必须红。
python3 - "$repo/android/native/nvn/bc1_astc_shaders/encode.comp" "$out/negative/encode.comp" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
anchor = "        atomicOr(dst.needs_rgba, 1u);"
assert source.count(anchor) == 1, "BC1 ASTC mutation anchor changed"
Path(sys.argv[2]).write_text(source.replace(anchor, ""))
PY
"${GLSLC:-glslc}" --target-env=vulkan1.1 -O "$out/negative/encode.comp" -o "$out/negative/encode.comp.spv"
set +e
timeout 180 "$out/test" "$out/negative" > "$out/negative.log" 2>&1
rc=$?
set -e
if [ "$rc" -ne 1 ] || ! grep -q 'FAIL .*BC1 transparent block must mark the texture for RGBA8' "$out/negative.log" ||
   ! grep -q 'BC1 ASTC shader failures=1' "$out/negative.log" ||
   grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/negative.log"; then
    cat "$out/negative.log"
    echo "【红】BC1 ASTC 透明标记负控未命中 rc=$rc" >&2
    exit 1
fi
echo '【绿】BC1 ASTC 计算编码、透明黑保底、原 BC 回读；撤透明标记负控命中'
