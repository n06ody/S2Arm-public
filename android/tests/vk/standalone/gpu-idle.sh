#!/usr/bin/env bash
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 执行，复用无窗口交换链夹具。
set -eu
exec bash "$(dirname "$0")/wsi-extent.sh" --gpu-idle
