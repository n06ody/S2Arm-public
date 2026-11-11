#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# Real compute command through production record/frame queue on Linux lavapipe + Khronos VVL.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
if [ "$(uname -s)" = Darwin ]; then
    exec bash "$repo/tests/vulkan/run.sh" android/tests/nvn_vk_real_teardown_test.sh
fi
layer=${S2A_VK_LAYER_DIR:-/usr/share/vulkan/explicit_layer.d}
icd=${S2A_LAVAPIPE_ICD:-}
if [ -z "$icd" ]; then
    for f in /usr/share/vulkan/icd.d/lvp_icd.json /usr/share/vulkan/icd.d/lvp_icd.*.json; do
        if [ -f "$f" ]; then icd=$f; break; fi
    done
fi
[ -f "$icd" ] || { echo 'missing lavapipe ICD; NOT RUN' >&2; exit 2; }
[ -f "$layer/VkLayer_khronos_validation.json" ] || { echo 'missing Khronos validation layer; NOT RUN' >&2; exit 2; }
command -v glslc >/dev/null || { echo 'missing glslc; NOT RUN' >&2; exit 2; }
mkdir -p "$repo/build"
out=$(mktemp -d "$repo/build/dispatch-fq-vvl.XXXXXXXX")
trap 'rm -rf "$out"' EXIT
inc=("$repo/android/tests/vkhost" "$repo/android/tests/hostvk" "$repo/android/native" "$repo/android/native/direct" "$repo/android/native/nvn" "$repo/runtime/include" "$repo/graphics/nvn/gfx/nvn" "$repo/graphics/nvn/gfx/nvn/include" "$repo/graphics/nvn/gfx/tex" "$repo/graphics/nvn/third_party/vulkan-headers/include")
args=(); for dir in "${inc[@]}"; do args+=(-I "$dir"); done
sources=("$repo"/android/native/nvn/{nvn_vk_program_result,nvn_vk_draw_diag,nvn_vk_pipe_manifest,nvn_vk_pipe_recipe,nvn_vk_driver_cache,nvn_vk_query,nvn_vk_session,nvn_vk_lifecycle,nvn_vk_exec_owner,nvn_vk_pipeline,nvn_vk_swapchain,nvn_vk_surface,nvn_vk_texcache,nvn_vk_sampler_cache,nvn_vk_sampler,nvn_vk_format,nvn_vk_mrt,nvn_vk_readback,nvn_vk_readback_gpu,nvn_vk_bcdec,nvn_vk_poolahb,poolahb_cover,poolahb_policy,poolahb_alias,nvn_vk_timeline,nvn_vk_threadbuf,nvn_vk_compute,nvn_vk_display_timing}.c "$repo"/android/native/direct/{vk_record,vk_clear,vk_device,gs_path,t2t_plan,buffer_texture_plan,vk_membuf,vk_resid,program_cache,compile_request,compile_worker,compiler_bind,s2a_direct_abi,sha256,json}.c "$repo/graphics/nvn/gfx/nvn/nvn_tex.c" "$repo/graphics/nvn/gfx/nvn/nvn_dirty.c" "$repo/graphics/nvn/gfx/tex/nvn_deswizzle.c")
cat > "$out/probe.comp" <<'SHADER'
#version 450
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
layout(set=0,binding=0) buffer Out { uint value; } output_data;
void main() { output_data.value=0x13579bdfu; }
SHADER
glslc --target-env=vulkan1.1 "$out/probe.comp" -o "$out/probe.spv"
compile() {
    cc -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-missing-field-initializers -ffunction-sections -fdata-sections "${args[@]}" "$@" "$repo/android/tests/nvn_vk_dispatch_fq_vvl_test.c" "${sources[@]}" -Wl,--gc-sections -l:libvulkan.so.1 -lpthread -lm -ldl
}
export VK_DRIVER_FILES="$icd" VK_LAYER_PATH="$layer" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1 XDG_CACHE_HOME="$out"
compile -o "$out/probe"
"$out/probe" "$out/probe.spv" >"$out/normal.log" 2>&1 || { cat "$out/normal.log" >&2; exit 1; }
if grep -qiE 'validation error|VUID-|UNASSIGNED-|SYNC-HAZARD|ERROR.*VALIDATION' "$out/normal.log"; then cat "$out/normal.log" >&2; exit 1; fi
cat "$out/normal.log"
compile -DS2A_FQ_VVL_NEGATIVE -o "$out/negative"
"$out/negative" "$out/probe.spv" >"$out/negative.log" 2>&1 || { cat "$out/negative.log" >&2; exit 1; }
grep -q 'VUID-VkBufferCreateInfo-size-00912' "$out/negative.log" || { cat "$out/negative.log" >&2; echo 'VVL negative control missed expected error' >&2; exit 1; }
unexpected=$(grep -iE 'validation error|VUID-|UNASSIGNED-|SYNC-HAZARD|ERROR.*VALIDATION' "$out/negative.log" | grep -vc 'VUID-VkBufferCreateInfo-size-00912' || true)
[ "$unexpected" -eq 0 ] || { cat "$out/negative.log" >&2; echo "VVL negative control has $unexpected unexpected diagnostics" >&2; exit 1; }
echo 'PASS VVL observer negative: zero-sized buffer detected, no unrelated validation errors'
