#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# The shared Vulkan image accepts the real-teardown entry; it also runs this clear probe.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
if [ "$(uname -s)" = Darwin ]; then
    exec bash "$repo/tests/vulkan/run.sh" android/tests/nvn_vk_real_teardown_test.sh
fi
exec bash "$repo/android/tests/nvn_vk_real_teardown_test.sh"
