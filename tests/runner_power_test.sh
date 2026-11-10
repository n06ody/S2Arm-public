#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁覆盖：android/tools/run_game.py 功耗段（设备侧采样协议与判读）
set -euo pipefail
H=$(cd "$(dirname "$0")/.." && pwd)
exec "${PYTHON:-python3}" "$H/tests/runner_power_test.py"
