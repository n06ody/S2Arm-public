#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个执行。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-replay-gs-topology"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
test -f "$S2A_VK_LAYER_DIR/VkLayer_khronos_validation.json"
glslc_bin=${GLSLC:-glslc}
shaders="$repo/android/tests/vk/shaders"
for stage in vert frag; do
    "$glslc_bin" --target-env=vulkan1.1 "$shaders/gs_topology.$stage" -o "$out/shaders/gs_topology.$stage.spv"
done
"$glslc_bin" --target-env=vulkan1.1 "$shaders/gs_topology.geom" -o "$out/shaders/triangles.geom.spv"
"$glslc_bin" --target-env=vulkan1.1 -DLINES "$shaders/gs_topology.geom" -o "$out/shaders/lines.geom.spv"

inc=()
for dir in build/c883-ndk android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn \
    graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$dir")
done
src=(
    android/tests/vk/t_replay_gs_topology.c
    graphics/nvn/replay/{rp_state,nvn_unimpl}.c
    graphics/nvn/gfx/nvn/nvn_fmt_expand.c
    android/native/nvn/{backend_vulkan,nvn_pipe_flight,nvn_vk_pipe_once,nvn_vk_format,nvn_vk_bcdec,nvn_vk_driver_cache}.c
    android/native/direct/{vk_device,gs_path,sha256}.c
    android/native/capprobe/capprobe_host.c
)
build_fixture() {
    cc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
        -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
        "${inc[@]}" "${src[@]/#/$repo/}" "$2" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$1"
}

export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export S2A_TEST_GPU_DESWIZZLE_OFF=1
if [ -z "${VK_LAYER_SETTINGS_PATH:-}" ]; then
    export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
fi
valid_log() {
    grep -q '^device: llvmpipe' "$1" && ! grep -Eq 'Validation Error|VUID-|SYNC-HAZARD-' "$1"
}

cc -std=c11 -D_DEFAULT_SOURCE -DS2A_CAPPROBE_STANDALONE "${inc[@]}" \
    "$repo/android/native/capprobe/capprobe.c" -lvulkan -o "$out/capprobe"
timeout 90 "$out/capprobe" all --badlist "$repo/android/native/capprobe/known-bad.txt" > "$out/probe.jsonl" 2>&1
python3 - "$out/probe.jsonl" "$out/gs-probe.json" <<'PY'
import json
import sys

with open(sys.argv[1]) as stream:
    rows = [json.loads(line) for line in stream if line.lstrip().startswith("{")]
required = ("nogsocc", "noq", "occ", "occimp")
arms = {row.get("arm"): row.get("outcome") for row in rows if row.get("type") == "arm"}
if any(arms.get(arm) != "ok" for arm in required) or any(row.get("type") == "blocked" for row in rows):
    raise SystemExit(f"GS probe failed: {arms}")
with open(sys.argv[2], "w") as stream:
    json.dump({"source": "probe", "gs_occlusion": "supported"}, stream, separators=(",", ":"))
print("GS and occlusion probe: 4/4 arms passed")
PY
if grep -Eq 'Validation Error|VUID-|SYNC-HAZARD-' "$out/probe.jsonl"; then cat "$out/probe.jsonl"; exit 1; fi
export S2A_CAPPROBE_RESULT="$out/gs-probe.json"

draw="$repo/graphics/nvn/replay/rp_draw.c"
build_fixture "$out/test" "$draw"
for dq in 0 1; do
    if [ "$dq" = 1 ]; then export S2A_TEST_NVK_DQ=1; else unset S2A_TEST_NVK_DQ; fi
    log="$out/dq$dq.log"
    if ! timeout 60 "$out/test" "$out/shaders" > "$log" 2>&1 || ! valid_log "$log"; then
        cat "$log"
        echo "【红】GS 图元回放 DQ=$dq" >&2
        exit 1
    fi
    grep '^GS topology ' "$log"
done
echo 'PASS GS topology replay (2 recording modes, 0 VUID/SYNC)'

log="$out/no-cap.log"
if ! env -u S2A_CAPPROBE_RESULT timeout 60 "$out/test" "$out/shaders" --no-cap > "$log" 2>&1 ||
   ! valid_log "$log" || ! grep -q '^PASS no GS capability:' "$log"; then cat "$log"; exit 1; fi
grep '^PASS no GS capability:' "$log"

python3 - "$draw" "$out/rp_draw_quad_strip_order.c" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
before = "n = qstrip_expand(src, isz, count, rv, gs, dst)"
assert source.count(before) == 1
Path(sys.argv[2]).write_text(source.replace(before, "n = qstrip_expand(src, isz, count, rv, 0, dst)"))
PY
build_fixture "$out/test-order-control" "$out/rp_draw_quad_strip_order.c"
log="$out/order-control.log"
rc=0
timeout 60 "$out/test-order-control" "$out/shaders" > "$log" 2>&1 || rc=$?
if [ "$rc" -ne 1 ] || ! valid_log "$log" ||
   ! grep -q '^GS topology quad-strip: draws=1 skips=0 bad_pixels=8/32$' "$log" ||
   ! grep -q '^GS topology indexed-quad-strip: draws=1 skips=0 bad_pixels=8/32$' "$log" ||
   [ "$(grep -c '^FAIL ' "$log")" -ne 2 ]; then cat "$log"; exit 1; fi
echo '【负对照开始】'
echo 'PASS GS ordering negative control: native order changed 8/32 pixels in each quad-strip case, 0 VUID/SYNC'
echo '【负对照结束】'
