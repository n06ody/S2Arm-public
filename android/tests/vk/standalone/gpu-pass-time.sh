#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-pass-time-math"
mkdir -p "$out"
cc -std=c11 -Wall -Wextra -Werror -I "$repo/android/native/nvn" \
    "$repo/android/tests/vk/t_pass_time_math.c" -o "$out/test"
"$out/test"
bash "$(dirname "$0")/wsi-extent.sh" --gpu-pass-time
bash "$(dirname "$0")/feedback-core.sh" --gpu-pass-time
echo '【绿】GPU 逐遍计时：真实呈现、反馈拆遍、汇总对平、查询上限及不可用负控；同步校验零错误'
