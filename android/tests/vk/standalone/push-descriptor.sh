#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --push-descriptor
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-push-descriptor"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
cat > "$out/shaders/ubo_only.frag" <<'GLSL'
#version 450
layout(set = 0, binding = 0) uniform U { vec4 c; } u;
layout(location = 0) out vec4 color0;
void main() { color0 = u.c; }
GLSL
cat > "$out/shaders/ubo_set0_tex_set1.frag" <<'GLSL'
#version 450
layout(set = 0, binding = 0) uniform U { vec4 c; } u;
layout(set = 1, binding = 0) uniform texture2D t;
layout(set = 1, binding = 1) uniform sampler s;
layout(location = 0) out vec4 color0;
void main() { color0 = u.c * texture(sampler2D(t, s), vec2(0.5)); }
GLSL
cat > "$out/shaders/ubo_set1_tex_set0.frag" <<'GLSL'
#version 450
layout(set = 1, binding = 0) uniform U { vec4 c; } u;
layout(set = 0, binding = 0) uniform texture2D t;
layout(set = 0, binding = 1) uniform sampler s;
layout(location = 0) out vec4 color0;
void main() { color0 = u.c * texture(sampler2D(t, s), vec2(0.5)); }
GLSL
cat > "$out/shaders/ubo_tex_set0.frag" <<'GLSL'
#version 450
layout(set = 0, binding = 0) uniform U { vec4 c; } u;
layout(set = 0, binding = 1) uniform texture2D t;
layout(set = 0, binding = 2) uniform sampler s;
layout(location = 0) out vec4 color0;
void main() { color0 = u.c * texture(sampler2D(t, s), vec2(0.5)); }
GLSL
cat > "$out/shaders/tbuf_set0.frag" <<'GLSL'
#version 450
layout(set = 0, binding = 0) uniform samplerBuffer tb;
layout(location = 0) out vec4 color0;
void main() { color0 = texelFetch(tb, 0); }
GLSL
cat > "$out/shaders/push_add.comp" <<'GLSL'
#version 450
layout(local_size_x = 1) in;
layout(set = 0, binding = 0) uniform U { vec4 c; } u;
layout(set = 0, binding = 1) buffer S { vec4 outv; } s;
void main() { s.outv = u.c; }
GLSL
cat > "$out/shaders/ubo_set0_pool_set1.frag" <<'GLSL'
#version 450
layout(set = 0, binding = 0) uniform U { vec4 c; } u;
layout(set = 1, binding = 0) uniform texture2D ts[2];
layout(set = 1, binding = 1) uniform sampler s;
layout(location = 0) out vec4 color0;
void main() { color0 = u.c * texture(sampler2D(ts[0], s), vec2(0.5)); }
GLSL
cat > "$out/shaders/over_limit.frag" <<'GLSL'
#version 450
layout(set = 0, binding = 0) uniform texture2D ts[33];
layout(set = 0, binding = 1) uniform sampler s;
layout(location = 0) out vec4 color0;
void main() { color0 = texture(sampler2D(ts[32], s), vec2(0.5)); }
GLSL
for sh in ubo_only.frag ubo_set0_tex_set1.frag ubo_set1_tex_set0.frag ubo_tex_set0.frag tbuf_set0.frag push_add.comp ubo_set0_pool_set1.frag over_limit.frag; do
    "$glslc" --target-env=vulkan1.1 "$out/shaders/$sh" -o "$out/shaders/$sh.spv"
done
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=("$repo/android/tests/vk/t_push_descriptor.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
wrap=()
for fn in vkCreateDevice vkEnumerateDeviceExtensionProperties vkCreateDescriptorSetLayout vkAllocateDescriptorSets vkUpdateDescriptorSets vkCmdBindDescriptorSets vkGetDeviceProcAddr; do
    wrap+=(-Wl,--wrap="$fn")
done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
unset VK_LAYER_ENABLES
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
printf 'push-descriptor validation: sync=1 shader_accesses_heuristic=1 settings=%s\n' "$VK_LAYER_SETTINGS_PATH"
failed=0
# 10 configs:
#   dq=0/1 x opt=0/1 with push=1, hide=0 (active VK_KHR_push_descriptor)
#   dq=0 opt=0 push=0 hide=0 (A/B off -> regular descriptor sets)
#   dq=0 opt=0 push=1 hide=1 (--wrap hides VK_KHR_push_descriptor -> simulates 9000 zero-regression fallback)
#   push=1 template=1 verifies both update paths in the same draw, with DQ off and on
#   repeat the combined update paths with bind dedup enabled to cover both sides of the push slot
for config in '0 0 1 0 0 0' '0 1 1 0 0 0' '1 0 1 0 0 0' '1 1 1 0 0 0' '0 0 0 0 0 0' '0 0 1 1 0 0' '0 0 1 0 1 0' '1 0 1 0 1 0' '0 0 1 0 1 1' '1 0 1 0 1 1'; do
    read -r dq opt push hidden template dedup <<< "$config"
    log="$out/dq$dq-opt$opt-push$push-hide$hidden-tpl$template-dedup$dedup.log"
    set +e
    timeout 120 "$out/test" "$out/shaders" "$dq" "$opt" "$push" "$hidden" "$template" "$dedup" > "$log" 2>&1
    result=$?
    set -e
    cat "$log"
    if [ "$opt" -eq 1 ]; then want_cases=8; else want_cases=9; fi
    if [ "$result" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-|fixture failed' "$log" ||
       ! grep -q "push-descriptor cases=$want_cases failures=0" "$log"; then
        echo "【红】push-descriptor config=$config rc=$result FAIL" >&2
        failed=1
    fi
done
# 负对照：去掉 maxPushDescriptors 上限门控，gfx-over-limit-fallback（34 > 32）必须被 VVL 抓出 VUID 报错。
mut="$out/mut"; mkdir -p "$mut"
python3 - "$repo/android/native/nvn/backend_vulkan.c" "$mut/backend_vulkan.c" <<'PYMUT'
import pathlib, sys
s = pathlib.Path(sys.argv[1]).read_text()
a = 'const uint32_t lim = g.dev->max_push_descriptors;'
if s.count(a) != 1: raise SystemExit('pick_push_set mutation anchor changed')
pathlib.Path(sys.argv[2]).write_text(s.replace(a, 'const uint32_t lim = 1024;'))
PYMUT
msrc=(); for f in "${src[@]}"; do [ "$f" = "$repo/android/native/nvn/backend_vulkan.c" ] && f="$mut/backend_vulkan.c"; msrc+=("$f"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${msrc[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test-neg"
set +e
timeout 120 "$out/test-neg" "$out/shaders" 0 0 1 0 > "$out/neg-over-limit.log" 2>&1
neg_rc=$?
set -e
neg_vvl=$(grep -cE 'Validation Error|VUID-VkDescriptorSetLayoutCreateInfo-flags-00281' "$out/neg-over-limit.log" || true)
if [ "$neg_rc" -eq 0 ] || [ "$neg_vvl" -eq 0 ] ||
   ! grep -q 'VUID-VkDescriptorSetLayoutCreateInfo-flags-00281' "$out/neg-over-limit.log"; then
    echo '【红】push-descriptor 负对照：放开 maxPushDescriptors 后未抓出预期 VUID-VkDescriptorSetLayoutCreateInfo-flags-00281' >&2
    tail -n 20 "$out/neg-over-limit.log" >&2
    failed=1
else
    echo '【负对照开始】'
    echo "push-descriptor limit negative: PASS (neg_rc=$neg_rc 抓出 $neg_vvl 条 maxPushDescriptors 超限校验报错: flags-00281)"
    echo '【负对照结束】'
fi
exit "$failed"
