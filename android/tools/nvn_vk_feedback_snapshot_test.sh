#!/usr/bin/env bash
# 反馈采样快照子资源拷贝：宿主回归（真 Vulkan / Mesa lavapipe）。
#
# 只编生产实现：nvn_vk_mrt.c（计划 / 拷贝区域 / 绑定替换判定）+ nvn_vk_format.c + vk_device.c。
# 用例：3D 纹理深度切片、3D + mip≠0、2D 数组层、pass/快照交集、多级×多层同构快照逐子资源拷贝；
# 修前必红（旧实现 3D 按 array layer 选；多层视图自采样 LEAVE 直通活绑定）。
# 缺 lavapipe 时退 2（红，不静默跳过）：子资源寻址必须在真实 Vulkan 上验字节，不能只做纯逻辑对拍。
#
# 真机（Adreno）验收用同源 tools/nvn_vk_feedback_snapshot_probe.sh（A 级，同一份用例源码）。
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
android="$repo/android"
find_icd() {
    if [ -n "${S2A_LAVAPIPE_ICD:-}" ]; then printf '%s\n' "$S2A_LAVAPIPE_ICD"; return; fi
    for f in "$HOME/.local/share/vulkan/icd.d/lvp_icd.json" "$HOME/.local/share/vulkan/icd.d/lvp_icd."*.json \
             /usr/share/vulkan/icd.d/lvp_icd.json /usr/share/vulkan/icd.d/lvp_icd.*.json; do
        [ -f "$f" ] && { printf '%s\n' "$f"; return; }
    done
}
icd=$(find_icd || true)
if [ -z "$icd" ]; then
    echo "【红】找不到 lavapipe ICD：本用例要求 CPU Vulkan 参考实现，不静默跳过。" >&2
    exit 2
fi
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cc_bin="${CC:-cc}"
inc=(-I "$android/native" -I "$android/native/direct" -I "$android/native/nvn"
     -I "$android/tests/vkhost" -I "$android/tests/hostvk"
     -I "$repo/graphics/nvn/third_party/vulkan-headers/include"
     -I "$repo/graphics/nvn/gfx/nvn" -I "$repo/graphics/nvn/gfx/nvn/include" -I "$repo/graphics/nvn"
     -I "$repo/graphics/nvn/gfx/tex" -I "$repo/runtime/include" -I "$repo/runtime/src")
"$cc_bin" -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
  "${inc[@]}" \
  -o "$tmp/nvn_vk_feedback_snapshot_test" \
  "$android/tests/nvn_vk_feedback_snapshot_test.c" \
  "$android/native/nvn/nvn_vk_mrt.c" \
  "$android/native/nvn/nvn_vk_format.c" \
  "$android/native/direct/vk_device.c" \
  -lvulkan -lm -pthread
export VK_DRIVER_FILES=$icd
echo "lavapipe: $icd"
"$tmp/nvn_vk_feedback_snapshot_test"
echo "nvn-vk-feedback-snapshot: 全部用例通过"
