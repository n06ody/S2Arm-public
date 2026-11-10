#!/usr/bin/env bash
# 直连逐帧 TSV（宿主，确定性）：表头只收源存在的列、逐 present 差分、gpuq_us 本模块 fence 账、
# 开关关/无目录不出文件。天真实现（写死 iOS 全列表头或 0 冒充）会在断言处红。
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
android="$repo/android"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"${CC:-cc}" -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -pthread \
  -DS2A_TSV_TEST=1 \
  -I "$android/native" -I "$android/native/nvn" -I "$android/native/direct" \
  -o "$tmp/t" "$android/tests/nvn_vk_frame_tsv_test.c"
"$tmp/t"
