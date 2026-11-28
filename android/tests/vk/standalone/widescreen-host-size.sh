#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑，单跑用 run.sh --widescreen-host-size
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-widescreen-host-size"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/solid_green.frag" -o "$out/shaders/solid_green.frag.spv"
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/solid_blue.frag" -o "$out/shaders/solid_blue.frag.spv"

inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
common_src=(
    "$repo/android/tests/vk/t_widescreen_host_size.c"
    "$repo/graphics/nvn/replay/nvn_unimpl.c"
    "$repo/android/native/nvn/nvn_pipe_flight.c"
    "$repo/android/native/nvn/nvn_vk_pipe_once.c"
    "$repo/android/native/direct/vk_device.c"
    "$repo/android/native/direct/gs_path.c"
    "$repo/android/native/capprobe/capprobe_host.c"
    "$repo/android/native/nvn/nvn_vk_format.c"
    "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c"
    "$repo/android/native/nvn/nvn_vk_bcdec.c"
    "$repo/android/native/nvn/nvn_vk_driver_cache.c"
    "$repo/android/native/direct/sha256.c"
)

cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "$repo/android/native/nvn/backend_vulkan.c" "${common_src[@]}" \
    -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"

export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"

for dq in 0 1; do
    for opt in 0 1; do
        log="$out/dq${dq}-opt${opt}.log"
        set +e
        timeout 120 "$out/test" "$out/shaders" "$dq" "$opt" > "$log" 2>&1
        rc=$?
        set -e
        if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
           ! grep -q "widescreen-host-size dq=${dq} opt=${opt} cases=4 failures=0" "$log"; then
            cat "$log"
            echo "【红】widescreen-host-size dq=$dq opt=$opt failed (rc=$rc)" >&2
            exit 1
        fi
        grep '^widescreen-host-size ' "$log"
    done
done

# 负对照（突变验证）：把 host_w/host_h 放宽退回原描述尺寸，断言用例必须红退非零
mut="$out/mut"
mkdir -p "$mut"
python3 - "$repo/android/native/nvn/backend_vulkan.c" "$mut/backend_vulkan.c" <<'PY'
import sys
src = open(sys.argv[1], encoding="utf-8").read()
old = "    const uint32_t hw = d->host_w ? d->host_w : (uint32_t)d->d.width;"
new = "    const uint32_t hw = (uint32_t)d->d.width; (void)d->host_w;"
assert src.count(old) == 1, f"mutation anchor count={src.count(old)}"
open(sys.argv[2], "w", encoding="utf-8").write(src.replace(old, new, 1))
PY
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "$mut/backend_vulkan.c" "${common_src[@]}" \
    -Wl,--gc-sections -lvulkan -lpthread -lm -o "$mut/test"

echo "【负对照开始】"
set +e
timeout 120 "$mut/test" "$out/shaders" 0 0 > "$mut/run.log" 2>&1
rc=$?
set -e
echo "【负对照结束】"
if [ "$rc" -eq 0 ]; then
    cat "$mut/run.log"
    echo "【红】widescreen-host-size mutation unexpectedly passed (rc=0)" >&2
    exit 1
fi
echo "PASS: widescreen-host-size mutation rejected as expected (rc=$rc)"
