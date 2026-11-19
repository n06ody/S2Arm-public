#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 真实回放的阶段范围、回退及遗漏阶段负控；由默认 Vulkan 套件与单模式入口共用。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
for mode in --barrier-narrow --barrier-narrow-off --barrier-narrow-stage-neg \
            --barrier-chain --barrier-chain-off --barrier-chain-neg; do
    bash "$repo/android/tests/vk/run.sh" "$mode"
done
