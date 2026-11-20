#!/usr/bin/env bash
# 逐帧时间探针（宿主，确定性）：环语义 / 帧归属 / sub 溢出 / 待决 acquire 结转 /
# 分片转储字节级核回 / 开关开销实测（<0.1 ms/帧的宿主量级证据）。
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
android="$repo/android"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"${CC:-cc}" -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -pthread \
  -I "$android/native" -I "$android/native/nvn" -I "$android/native/direct" \
  -o "$tmp/t" "$android/tests/nvn_ft_probe_test.c" "$android/native/direct/sha256.c"
"$tmp/t"
