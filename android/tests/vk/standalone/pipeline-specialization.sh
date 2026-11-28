#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档使用 standalone/*.sh，单跑 --pipeline-specialization。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-pipeline-specialization"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
for stage in vert frag geom comp; do
    "${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/spec_pipe.$stage" -o "$out/shaders/spec_pipe.$stage.spv"
done
"${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/depth_state.vert.spv"
inc=(-I "$repo")
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/tex graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_pipeline_specialization.c")
for f in graphics/nvn/replay/{rp_draw,rp_buf,rp_state,nvn_unimpl}.c android/native/nvn/{backend_vulkan,nvn_pipe_flight,nvn_vk_pipe_once,nvn_vk_format,nvn_vk_bcdec,nvn_vk_driver_cache}.c android/native/direct/{vk_device,gs_path,sha256}.c android/native/capprobe/capprobe_host.c graphics/nvn/gfx/nvn/{nvn_fmt_expand,nvn_tex}.c graphics/nvn/gfx/tex/nvn_deswizzle.c; do
    src+=("$repo/$f")
done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
   -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" \
   -Wl,--wrap=nvn_gpu_draw -Wl,--wrap=nvn_plat_flag -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_VALIDATE_SYNC=1 VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
printf '%s\n' 'khronos_validation.validate_sync = true' 'khronos_validation.syncval_shader_accesses_heuristic = true' > "$VK_LAYER_SETTINGS_PATH"
# Native GS is enabled only after the same four real probe arms as the normal gate.
cc -std=c11 -D_DEFAULT_SOURCE -DS2A_CAPPROBE_STANDALONE "${inc[@]}" \
   "$repo/android/native/capprobe/capprobe.c" -lvulkan -o "$out/capprobe"
timeout 90 "$out/capprobe" all --badlist "$repo/android/native/capprobe/known-bad.txt" > "$out/probe.jsonl"
python3 - "$out/probe.jsonl" "$out/probe-result.json" <<'PYPROBE'
import json
from pathlib import Path
import sys

rows = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines() if line.startswith("{")]
fingerprints = [row for row in rows if row.get("type") == "fingerprint" and "vendorID" in row and "driverVersion" in row]
arms = {row.get("arm"): row.get("outcome") for row in rows if row.get("type") == "arm"}
required = ("nogsocc", "noq", "occ", "occimp")
if len(fingerprints) != 1 or any(arms.get(arm) != "ok" for arm in required) or any(row.get("type") == "blocked" for row in rows):
    raise SystemExit("native GS probe failed: " + repr(rows))
# Match the compact fields read by capprobe_host.c.

Path(sys.argv[2]).write_text(json.dumps({"source": "probe", "gs_occlusion": "supported"}, separators=(",", ":")))
print("native GS probe: " + ", ".join(f"{arm}={arms[arm]}" for arm in required))
PYPROBE
export S2A_CAPPROBE_RESULT="$out/probe-result.json"
for dq in 0 1; do
    log="$out/dq-$dq.log"
    rc=0
    timeout 120 "$out/test" "$out/shaders" "$dq" 2>&1 | tee "$log" || rc=$?
    if grep -qE 'Validation Error|VUID-|SYNC-HAZARD' "$log"; then
        echo '【红】管线特化与视口重排 Vulkan 校验失败' >&2
        exit 2
    fi
    if [ "$rc" -ne 0 ]; then exit "$rc"; fi
done
