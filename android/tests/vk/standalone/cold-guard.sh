#!/usr/bin/env bash
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑，可单跑 --cold-guard。
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="${S2A_COLD_GUARD_OUT:-$repo/build/vk-cold-guard}"
mkdir -p "$out"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/cold_guard.vert" -o "$out/cold-guard-vs.spv"
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/cold_guard.frag" -o "$out/cold-guard-a.spv"
fixture_identity=0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20
python3 "$repo/android/tools/cold_guard_spirv.py" "$out/cold-guard-a.spv" "$out/proof" --program-identity "$fixture_identity"
cp "$out/proof/s2a-cold-guard-v1/$fixture_identity.bin" "$out/cold-guard.asset"
python3 - "$out/proof/proof.json" <<'PYFIXTURE'
import json
from pathlib import Path
import sys

proof = json.loads(Path(sys.argv[1]).read_text())
assert len(proof["selected_regions"]) == 1
domain = proof["selected_domain"]
assert (domain["set"], domain["binding"], domain["field_byte"],
        domain["stride_bytes"], domain["record_max"]) == (0, 0, 12, 20, 64)
prefix = domain["prefix"]
assert (prefix["set"], prefix["binding"], prefix["ubo_byte"], prefix["mask"]) == (0, 1, 16, 15)
PYFIXTURE
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/cold_guard_producer.comp" -o "$out/cold-guard-producer.spv"
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=("$repo/android/tests/vk/t_cold_guard.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    -DS2A_NVK_COLD_GUARD_HOST_TEST=1 "${inc[@]}" "${src[@]}" \
    -Wl,--wrap=vkGetDeviceProcAddr -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
for dq in 0 1; do
    for enabled in 0 1; do
        log="$out/dq$dq-enabled$enabled.log"
        timeout 30 "$out/test" "$out" "$dq" "$enabled" > "$log" 2>&1 || { cat "$log"; exit 1; }
        if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
            cat "$log"
            exit 1
        fi
        grep '^cold-guard ' "$log"
        python3 "$repo/android/tests/vk/check_gpu_pass_time.py" "$log" "cold-$enabled"
    done
done

cc -std=c11 -O2 -Wall -Wextra -Werror -I "$repo/android/native/nvn" \
    "$repo/android/tests/vk/t_cold_ledger.c" -o "$out/ledger"
"$out/ledger"

# The shared replay must form this exact plan before opening a physical pass,
# then bind the identical SSBO and driver slice via the normal draw path.
replay_src=("$repo/android/tests/vk/t_cold_replay.c" "$repo/graphics/nvn/tests/nvn_clear_view_unused.c")
for f in rp_exec rp_copy_t2b rp_clear_tex rp_draw_tex rp_state rp_entry rp_res rp_buf rp_pass rp_present rp_draw nvn_unimpl; do
    replay_src+=("$repo/graphics/nvn/replay/$f.c")
done
replay_src+=("$repo/graphics/nvn/gfx/nvn/nvn_tex.c" "$repo/graphics/nvn/gfx/tex/nvn_deswizzle.c")
cc -D_DEFAULT_SOURCE -DS2A_TEST_TS_ALLOC -DS2A_GPU_LAND_TEST -DS2A_TEST_TREG_SCRUB \
    -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -I "$repo" -I "$repo/graphics/nvn/gfx/tex" \
    "${inc[@]}" "${replay_src[@]}" -pthread -lm -o "$out/replay"
"$out/replay"
