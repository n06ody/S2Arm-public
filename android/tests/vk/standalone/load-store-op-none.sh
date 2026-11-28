#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --load-store-op-none
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-load-store-op-none"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
cat > "$out/shaders/pos_z.vert" <<'GLSL'
#version 450
layout(push_constant) uniform PC { float z; } pc;
void main() {
    vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex], pc.z, 1.0);
}
GLSL
cat > "$out/shaders/solid_pc.frag" <<'GLSL'
#version 450
layout(push_constant) uniform PC { layout(offset = 16) vec4 col; } pc;
layout(location = 0) out vec4 color0;
void main() { color0 = pc.col; }
GLSL
cat > "$out/shaders/split_depth.frag" <<'GLSL'
#version 450
layout(location = 0) out vec4 color0;
void main() {
    gl_FragDepth = gl_FragCoord.x < 2.0 ? 0.25 : 0.75;
    color0 = vec4(0.25, 0.0, 0.0, 1.0);
}
GLSL
cat > "$out/shaders/solid_mrt2.frag" <<'GLSL'
#version 450
layout(push_constant) uniform PC { layout(offset = 16) vec4 col; } pc;
layout(location = 0) out vec4 color0;
layout(location = 1) out vec4 color1;
void main() { color0 = pc.col; color1 = vec4(0.0, 0.0, 1.0, 1.0); }
GLSL
for sh in pos_z.vert solid_pc.frag split_depth.frag solid_mrt2.frag; do
    "$glslc" --target-env=vulkan1.1 "$out/shaders/$sh" -o "$out/shaders/$sh.spv"
done
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=("$repo/android/tests/vk/t_load_store_op_none.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
wrap=()
for fn in vkCreateDevice vkEnumerateDeviceExtensionProperties vkCreateRenderPass vkCreateGraphicsPipelines vkCmdBeginRenderPass vkCmdBindPipeline vkCmdSetStencilWriteMask vkCmdDraw vkGetDeviceProcAddr; do
    wrap+=(-Wl,--wrap="$fn")
done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
printf 'load-store-op-none validation: sync=1 shader_accesses_heuristic=1 settings=%s\n' "$VK_LAYER_SETTINGS_PATH"
failed=0
# 6 configs:
#   dq=0/1 x opt=0/1 with ls_none=1, hide=0 (NONE only at unused pass close)
#   dq=0 opt=0 ls_none=0 hide=0 (A/B off -> LOAD/STORE)
#   dq=0 opt=0 ls_none=1 hide=1 (--wrap hides VK_EXT_load_store_op_none -> zero-regression fallback)
for config in '0 0 1 0' '0 1 1 0' '1 0 1 0' '1 1 1 0' '0 0 0 0' '0 0 1 1'; do
    read -r dq opt ls_none hidden <<< "$config"
    log="$out/dq$dq-opt$opt-ls$ls_none-hide$hidden.log"
    set +e
    timeout 120 "$out/test" "$out/shaders" "$dq" "$opt" "$ls_none" "$hidden" > "$log" 2>&1
    result=$?
    set -e
    cat "$log"
    if [ "$result" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-|fixture failed' "$log" ||
       ! grep -q 'load-store-op-none cases=10 failures=0' "$log"; then
        echo "【红】load-store-op-none config=$config rc=$result FAIL" >&2
        failed=1
    fi
done
# 负对照：撤掉收口闲置面优化，夹具须在只清颜色的通道抓到物理 DS 载存未减。
mut="$out/mut"; mkdir -p "$mut"
python3 - "$repo/android/native/nvn/backend_vulkan.c" "$mut/backend_vulkan.c" <<'PYMUT'
import pathlib, sys
s = pathlib.Path(sys.argv[1]).read_text()
a = 'const uint16_t idle = pass_idle_at_close(c);'
if s.count(a) != 1: raise SystemExit('pass_idle_at_close mutation anchor changed')
pathlib.Path(sys.argv[2]).write_text(s.replace(a, 'const uint16_t idle = pass_idle_at_close(c) & 0u;'))
PYMUT
msrc=(); for f in "${src[@]}"; do [ "$f" = "$repo/android/native/nvn/backend_vulkan.c" ] && f="$mut/backend_vulkan.c"; msrc+=("$f"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${msrc[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test-neg"
set +e
printf '【负对照开始】\n'
timeout 120 "$out/test-neg" "$out/shaders" 0 0 1 0 > "$out/neg-no-close-none.log" 2>&1
neg_rc=$?
grep -E 'case=clear-only-pass-idle-depth|cases=10 failures=' "$out/neg-no-close-none.log" || true
printf '【负对照结束】\n'
set -e
if [ "$neg_rc" != 1 ] || ! grep -qE 'case=clear-only-pass-idle-depth.*op_bad=2 px_bad=0 unsafe_w=0 unsafe_r=0 bad=1' "$out/neg-no-close-none.log" ||
   ! grep -q 'cases=10 failures=1' "$out/neg-no-close-none.log" ||
   grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/neg-no-close-none.log"; then
    echo '【红】load-store-op-none 负对照：撤回收口优化未在预期载存断言失败' >&2
    tail -n 20 "$out/neg-no-close-none.log" >&2
    failed=1
else
    echo 'load-store-op-none 负对照 PASS（收口撤回只改变实际载存）'
fi
exit "$failed"
