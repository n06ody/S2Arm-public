#!/usr/bin/env bash
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配执行，这里复用无头交换链夹具。
set -eu
bash "$(dirname "$0")/wsi-extent.sh" --gpu-idle
exec bash "$(dirname "$0")/wsi-extent.sh" --gpu-idle-passes
