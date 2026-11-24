#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 复用附件反馈夹具及其同版 Vulkan 同步校验设置。
set -euo pipefail
export S2A_TEST_PASS_DIAG_ONLY=1
exec bash "$(dirname "$0")/feedback-core.sh"
