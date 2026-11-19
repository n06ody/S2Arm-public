#!/usr/bin/env bash
# 画质设置来源选择（android/native/gfx_plat/nvk_quality_cfg.h）宿主单测：调试门开 + 调试属性非空 → 属性，否则环境。纯逻辑
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
cc="${CC:-cc}"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
"$cc" -std=gnu11 -O1 -Wall -Wextra -Werror -I "$repo/android/native/gfx_plat" -o "$tmp/t" "$repo/android/tests/nvk_quality_cfg_test.c"
"$tmp/t"
