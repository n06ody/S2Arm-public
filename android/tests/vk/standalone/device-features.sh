#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --<名>
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
mode=${1:-}
case "$mode" in ''|--baseline|--negative-timeline) ;; *) exit 64 ;; esac
out="$repo/build/vk-device-features"
mkdir -p "$out"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
source_file="$repo/android/native/direct/vk_device.c"
variant=${mode#--}; variant=${variant:-default}
binary="$out/test-$variant"
if [ "$mode" = --baseline ]; then
    : "${2:?upstream main ref required}"
    git -C "$repo" show "$2:android/native/direct/vk_device.c" > "$out/baseline.c"
    source_file="$out/baseline.c"
elif [ "$mode" = --negative-timeline ]; then
    python3 - "$source_file" "$out/negative-timeline.c" <<'PY'
import pathlib, sys
source = pathlib.Path(sys.argv[1]).read_text()
before = 'draw_parameters.pNext = has_timeline ? (VkBaseInStructure *)&timeline'
assert source.count(before) == 1, 'timeline mutation anchor changed'
pathlib.Path(sys.argv[2]).write_text(source.replace(before, 'draw_parameters.pNext = 1 ? (VkBaseInStructure *)&timeline'))
PY
    source_file="$out/negative-timeline.c"
fi
inc=(-I "$repo/build/c883-ndk")
for d in android/tests/vkhost android/native/direct android/native runtime/include; do inc+=(-I "$repo/$d"); done
wrap=()
for fn in vkCreateInstance vkCreateDevice vkDestroyInstance vkEnumerateInstanceExtensionProperties \
          vkEnumerateDeviceExtensionProperties vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures2; do wrap+=(-Wl,--wrap="$fn"); done
cc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections \
    "${inc[@]}" "$repo/android/tests/vk/standalone/device-features.c" "$source_file" \
    "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$binary"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD"
export VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
log="$out/${mode#--}.log"; [ -n "$mode" ] || log="$out/run.log"
set +e
timeout 120 "$binary" > "$log" 2>&1
rc=$?
set -e
if [ "$mode" = --negative-timeline ]; then
    if [ "$rc" != 1 ] || ! grep -q 'FAIL timeline-hidden: timeline create chain/extension mismatch' "$log" ||
       ! grep -q 'device-features cases=11 failures=4 validation-errors=0' "$log"; then cat "$log"; exit 1; fi
    # 预期红只留在独立日志；模拟档失败诊断会扫描整套输出中的 ^FAIL，不能把负控冒充正向失败。
    echo "device-features negative: PASS (timeline-hidden rejected; 4 expected failures; log=$log)"
    exit 0
fi
cat "$log"
[ "$rc" = 0 ] || exit "$rc"
! grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"
grep -q 'device-features cases=11 failures=0 validation-errors=0' "$log"
if [ -z "$mode" ]; then bash "$0" --negative-timeline; fi
