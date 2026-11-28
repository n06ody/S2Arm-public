#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认按 standalone/*.sh 通配执行，单跑用 --bc-decoded-alt-view。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
[ -z "${1:-}" ] || exit 64
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
out="$repo/build/vk-bc-decoded-alt-view"
mkdir -p "$out"
"${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/samp_at.comp" -o "$out/samp_at.comp.spv"
spirv-val --target-env vulkan1.1 "$out/samp_at.comp.spv"
inc=()
for d in build/c883-ndk android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn \
         graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_bc_decoded_alt_view.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c \
         android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c \
         android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c \
         android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c graphics/nvn/gfx/nvn/nvn_tex.c \
         graphics/nvn/gfx/tex/nvn_deswizzle.c graphics/nvn/gfx/tex/nvn_bcdec.c; do
    src+=("$repo/$f")
done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 -DS2A_BCDEC_HOST_TEST \
    "${inc[@]}" "${src[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"

export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR"
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
no_validation_errors() {
    grep -q '^device: llvmpipe' "$1" && ! grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$1"
}

log="$out/run.log"
rc=0
S2A_VK_BC_DECODE=force timeout 180 "$out/test" "$out" > "$log" 2>&1 || rc=$?
cat "$log"
[ "$rc" = 0 ] && no_validation_errors "$log" && ! grep -q "^\[bc7-workset\]" "$log" && grep -qx 'bc-decoded-alt-view: failures=0' "$log" || exit 1

# 合成整图：R2 的 Mode 6 常量块与 ASTC void-extent 常量同色。
python3 - "$out/bc7-astc.pack" <<'PY_PACK'
import hashlib
from pathlib import Path
import struct
import sys

value = 0x40 | (3 << 63)
for i, v in enumerate((66, 65, 66, 127)):
    value |= v << (7 + i * 14)
    value |= v << (14 + i * 14)
bc = value.to_bytes(16, "little")
astc = bytes.fromhex("fcfdffffffffffff") + struct.pack("<HHHH", *(c * 257 for c in (133,131,133,255)))
key = hashlib.sha256(b"s2a-bc7-astc-image-v1" + struct.pack("<IIII",4,4,1,1) + bc).digest()
head = struct.pack("<8sIIQQQBBHI16x", b"S2ABCAQ2", 2, 80, 1, 80, 160, 4, 4, 0, 0)
Path(sys.argv[1]).write_bytes(head + astc + key + struct.pack("<QQ",64,16) + hashlib.sha256(astc).digest())
Path(sys.argv[1]).with_suffix(".tsv").write_text(
    "key\twidth\theight\tlevels\tlayers\tbc_bytes\tdecision\n" + key.hex() + "\t4\t4\t1\t1\t16\taccepted\n")
PY_PACK
packed="$out/packed.log"
S2A_VK_BC_DECODE=force timeout 180 "$out/test" "$out" --packed > "$packed" 2>&1 || { cat "$packed"; exit 1; }
no_validation_errors "$packed" && grep -qx 'bc-decoded-alt-view: failures=0' "$packed" || { cat "$packed"; exit 1; }
test "$(grep -c '^BC7-PACK format=' "$packed")" = 2
grep '^BC7-PACK format=' "$packed"
expect_hits=0
if grep -q 'storage=ASTC' "$packed"; then expect_hits=2; fi
python3 "$repo/pack/astc_pack.py" bc7-compare "$packed" "$out/bc7-astc.tsv" > "$out/workset.json"
python3 - "$out/workset.json" "$expect_hits" <<'PY_DIAG'
import json
from pathlib import Path
import sys

report = json.loads(Path(sys.argv[1]).read_text())
s = report["session"]
hits = int(sys.argv[2])
expected = dict(final=1, whole=2, bc_bytes=32, hit=hits, hit_bytes=16*hits, unique=1,
                unique_bytes=16, unique_hit=int(bool(hits)), unique_hit_bytes=16*int(bool(hits)),
                identity_lost=0, non_whole=1, sparse=1, layout=0, low_cap=2-hits)
assert all(s.get(k) == v for k, v in expected.items()), (expected, s)
assert report["offline"]["accepted"] == {"images": 1, "bc_bytes": 16}, report
assert not report["unknown_keys"], report
print("BC7 workset:", json.dumps(report, sort_keys=True))
PY_DIAG

# 负控：把 UNORM->SRGB 采样改用 UNORM 视图，像素断言必须精准报红（0 校验层错误）
neg="$out/wrong-view.log"
rc=0
S2A_VK_BC_DECODE=force timeout 180 "$out/test" "$out" --wrong-view > "$neg" 2>&1 || rc=$?
if [ "$rc" != 1 ] || ! no_validation_errors "$neg" ||
   ! grep -qx 'bc-decoded-alt-view: failures=1' "$neg" ||
   ! grep -q 'BC1_RGBA_UNORM->SRGB alt sample got=' "$neg"; then
    cat "$neg"; exit 1
fi
echo '【负对照开始】'
grep 'BC1_RGBA_UNORM->SRGB alt sample got=' "$neg"
echo '【负对照结束】'
echo '【绿】BC 解码贴图 UNORM<->SRGB、兼容字节类视图采样像素读回与不兼容格式拒收全通，Vulkan 校验 0 条'
