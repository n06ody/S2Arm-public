#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 门禁间接：android/tests/vk/run.sh 默认运行 standalone/*.sh。可单跑 --sparse-write。
set -euo pipefail
exec bash "$(dirname "$0")/replay-cube-view.sh" --sparse-write
