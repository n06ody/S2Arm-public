#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
set -eu
root=$(cd "$(dirname "$0")/../../../.." && pwd)
out=$1
mkdir -p "$(dirname "$out")"
src="$root/android/native"
case "$(uname -s)" in
  Darwin) flags=(-dynamiclib) ;;
  Linux) flags=(-shared -fPIC) ;;
  *) echo "unsupported Vulkan pack host" >&2; exit 1 ;;
esac
"${CC:-cc}" -std=c11 -O2 -D_GNU_SOURCE -DS2A_LIBPACK_PACKER "${flags[@]}" \
  -I"$src" -I"$src/direct" -I"$src/nvn" -I"$root/runtime/include" \
  "$src/direct/libpack_vk.c" "$src/direct/json.c" "$src/direct/jv.c" \
  "$src/direct/sha256.c" "$src/nvn/nvn_vk_program_result.c" -o "$out"
