#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# Narrow production-backend check; called by vk/run.sh after Vulkan discovery.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
mode=${1:-}
[ -z "$mode" ] || [ "$mode" = --negative ] || exit 64
out="$repo/build/vk-workgroup"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
compile() {
    local name=$1 x=$2 y=$3 z=$4; shift 4
    "$glslc" --target-env=vulkan1.1 -DWX="$x" -DWY="$y" -DWZ="$z" "$@" \
        "$repo/android/tests/vk/shaders/workgroup.comp" -o "$out/shaders/$name.spv"
}
compile one 1 1 1
compile boundary 16 8 1
compile boundary3d 8 8 2
compile product 16 16 1
compile product2 128 2 1
compile axis_x 129 1 1
compile axis_y 1 129 1
compile axis_z 1 1 64
compile spec_boundary 16 8 1 -DSIZE_SPEC=1
compile spec_product 16 16 1 -DSIZE_SPEC=1
compile shared8k 1 1 1 -DSHARED_WORDS=2048
inc=(-I "$repo/build/c883-ndk")
for d in android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn \
         graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_compute_workgroup.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c \
         android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c \
         android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c \
         android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c graphics/nvn/replay/nvn_unimpl.c; do
    src+=("$repo/$f")
done
if [ "$mode" = --negative ]; then
    python3 - "$repo/android/native/nvn/backend_vulkan.c" "$out/backend_mutant.c" <<'PY'
import pathlib, sys
source = pathlib.Path(sys.argv[1]).read_text()
before = 'if (s->stage == NVN_GPU_SH_CS) {   // 计算工作组预检：'
assert source.count(before) == 1, 'workgroup mutation anchor changed'
pathlib.Path(sys.argv[2]).write_text(source.replace(before, 'if (0 && s->stage == NVN_GPU_SH_CS) {   // 计算工作组预检：'))
PY
    src[1]="$out/backend_mutant.c"
fi
binary="$out/test"
[ "$mode" != --negative ] || binary="$out/test-negative"
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" \
    -Wl,--wrap=vkCreateShaderModule -Wl,--wrap=vkCreateComputePipelines -Wl,--gc-sections \
    -lvulkan -lpthread -lm -o "$binary"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD"
export VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
args=(--guard-edges); log="$out/run.log"
if [ "$mode" = --negative ]; then args=(); log="$out/negative.log"; fi
set +e
timeout 120 "$binary" "$out/shaders" ${args[@]+"${args[@]}"} > "$log" 2>&1
rc=$?
set -e
if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then cat "$log"; exit 1; fi
if [ "$mode" = --negative ]; then
    if [ "$rc" != 1 ] || ! grep -q 'workgroup cases=17 failures=10' "$log" ||
       ! grep -q 'case=product expected=reject:maxComputeWorkGroupInvocations actual_rc=0 modules=1 pipelines=0 bad=1' "$log" ||
       ! grep -q 'case=shared_over expected=reject:本机 maxComputeSharedMemorySize actual_rc=0' "$log"; then
        cat "$log"; echo '【红】workgroup negative control missed' >&2; exit 1
    fi
    echo 'workgroup negative: PASS (10 invalid shapes reached driver; 7 legal controls passed)'; exit 0
fi
cat "$log"
if [ "$rc" -ne 0 ]; then
    echo "【红】compute workgroup checks (rc=$rc)" >&2; exit 1
fi
grep -q 'workgroup cases=24 failures=0' "$out/run.log"
echo '【绿】compute workgroup: 24 cases, real compile/dispatch, validation 0 errors'
bash "$0" --negative
