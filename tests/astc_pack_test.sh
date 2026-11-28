#!/bin/bash
# 门禁覆盖：pack/astc_pack.py graphics/tools/astc/bc2astc.c graphics/tools/yaz0dec.c graphics/third_party/astc-encoder android/native/nvn/bc7_astc_pack.inc tests/astc_pack_test.py
set -eu
cd "$(dirname "$0")/.."
python3 -I -B tests/astc_pack_test.py
