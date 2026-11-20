#!/usr/bin/env bash
# 宿主单测：android/native/nvn/nvn_vk_border.h（自定义边框色就近取内置三档）。不需要 NDK / 设备 / 外部库。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$repo/build/border-nearest-test"
out=$(mktemp -d "$repo/build/border-nearest-test/run.XXXXXXXX")
trap 'rm -rf "$out"' EXIT
cc -std=c11 -O1 -Wall -Wextra -Werror -I "$repo/android/native/nvn" "$repo/android/tests/nvk_border_nearest_test.c" -o "$out/test"
"$out/test"
