#!/usr/bin/env bash
# 门禁间接：run.sh discovers standalone fixtures; reuse the real headless swapchain setup.
set -eu
exec bash "$(dirname "$0")/wsi-extent.sh" --gpu-idle
