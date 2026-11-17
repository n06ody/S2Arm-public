#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
out="$repo/build/csf-reference"
mkdir -p "$out"
if [ "$(uname -s)" = Darwin ]; then
    . "$repo/android/tools/ndk_toolchain.sh"
    ndk=$(s2a_ndk_resolve); tag=$(s2a_ndk_prebuilt_tag "$ndk")
    mkdir -p "$out/ndk"
    cp -R "$ndk/toolchains/llvm/prebuilt/$tag/sysroot/usr/include/vulkan" "$out/ndk/"
    cp -R "$ndk/toolchains/llvm/prebuilt/$tag/sysroot/usr/include/vk_video" "$out/ndk/"
    dotnet build "$repo/android/tests/csf_reference/Spill.csproj" -c Release --nologo
    export S2A_VULKAN_CACHE="$out/cache"
    exec bash "$repo/tests/vulkan/run.sh" android/tests/vk/run.sh --csf-reference
fi
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1
c++ -std=c++17 -O1 -Wall -Wextra -Werror -Wno-missing-field-initializers -I "$out/ndk" "$repo/android/tests/csf_reference/runner.cpp" -lvulkan -o "$out/runner"
glslc --target-env=vulkan1.1 "$repo/android/tests/csf_reference/compact.comp" -o "$out/original.spv"
dotnet "$repo/android/tests/csf_reference/bin/Release/net10.0/Spill.dll" "$out/original.spv" "$out/spill.spv" 1 2
spirv-val --target-env vulkan1.1 "$out/original.spv"
spirv-val --target-env vulkan1.1 "$out/spill.spv"
# Unique directories preserve every invocation's input and report.
trial=$(mktemp -d "$out/trial.XXXXXXXX")
python3 "$repo/android/tests/csf_reference/fixture.py" "$trial/fixture"
python3 "$repo/android/tests/csf_reference/compare.py" --runner "$out/runner" --snapshot "$trial/fixture/capture.tsv" --original "$out/original.spv" --spill "$out/spill.spv" --spill-binding 2 --spill-bytes 45056 --out "$trial/result"
python3 - "$trial/result/report.json" <<'PY'
import json,sys
r=json.load(open(sys.argv[1]))
assert r['status']=='equal'
assert all(v['reference_spill_vs_device']['different_bytes']==0 for v in r['outputs'].values()), 'independent compact-list/count oracle mismatch'
print('PoC: deterministic compaction, counts, tail sentinel and texture input match independent oracle')
PY
