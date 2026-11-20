#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
set -euo pipefail
android=$(cd "$(dirname "$0")/.." && pwd)
source "$android/tools/host_out.sh"
out=$(s2a_host_out)
trap 'rm -rf "$out"' EXIT
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -I "$android/native/direct" -DS2A_TEST_CF_DICT_COMPARE -DS2A_ANDROID_VK_BACKEND_NVK=1 \
    "$android/tests/cf_dictionary_test.c" "$android/native/nvn/portable.c" \
    "$android/native/direct/sha256.c" -o "$out/cf_dictionary_test"
"$out/cf_dictionary_test"
"$out/cf_dictionary_test" off
rc=0
"$out/cf_dictionary_test" require-fast > "$out/withdraw.log" 2>&1 || rc=$?
if [ "$rc" != 1 ] || ! grep -q 'lookup exceeded eight comparisons' "$out/withdraw.log"; then
    cat "$out/withdraw.log" >&2
    exit 1
fi
echo 'CFDictionary negative: linear lookup rejected; iteration ok'
