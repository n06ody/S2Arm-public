#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --imageless-fb
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-imageless-fb"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
cat > "$out/shaders/fullscreen_z.vert" <<'GLSL'
#version 450
layout(push_constant) uniform PC { vec4 z; } pc;
void main() {
    vec2 p = vec2((gl_VertexIndex & 1) * 4 - 1, (gl_VertexIndex & 2) * 2 - 1);
    gl_Position = vec4(p, pc.z.x, 1.0);
}
GLSL
cat > "$out/shaders/solid_pc.frag" <<'GLSL'
#version 450
layout(push_constant) uniform PC { vec4 c; } pc;
layout(location = 0) out vec4 color0;
void main() { color0 = pc.c; }
GLSL
cat > "$out/shaders/mrt_pc.frag" <<'GLSL'
#version 450
layout(push_constant) uniform PC { layout(offset = 16) vec4 c; } pc;
layout(location = 0) out vec4 color0;
layout(location = 1) out vec4 color1;
void main() {
    color0 = pc.c;
    color1 = vec4(pc.c.z, pc.c.x, pc.c.y, 1.0);
}
GLSL
for sh in fullscreen_z.vert solid_pc.frag mrt_pc.frag; do
    "$glslc" --target-env=vulkan1.1 "$out/shaders/$sh" -o "$out/shaders/$sh.spv"
done
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=("$repo/android/tests/vk/t_imageless_fb.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
wrap=()
for fn in vkEnumerateDeviceExtensionProperties vkCreateDevice vkCreateFramebuffer vkDestroyFramebuffer vkCmdBeginRenderPass vkGetDeviceProcAddr; do
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
printf 'imageless-fb validation: sync=1 shader_accesses_heuristic=1 settings=%s\n' "$VK_LAYER_SETTINGS_PATH"
failed=0
# 6 configs:
#   dq=0/1 x opt=0/1 with ifb=1, hide=0 (active VK_KHR_imageless_framebuffer + DQ AttachmentBeginInfo deep-copy)
#   dq=0 opt=0 ifb=0 hide=0 (A/B off -> regular pc_fb)
#   dq=0 opt=0 ifb=1 hide=1 (--wrap hides VK_KHR_imageless_framebuffer -> zero-regression fallback)
for config in '0 0 1 0' '0 1 1 0' '1 0 1 0' '1 1 1 0' '0 0 0 0' '0 0 1 1'; do
    read -r dq opt ifb hidden <<< "$config"
    log="$out/dq$dq-opt$opt-ifb$ifb-hide$hidden.log"
    set +e
    timeout 120 "$out/test" "$out/shaders" "$dq" "$opt" "$ifb" "$hidden" > "$log" 2>&1
    result=$?
    set -e
    cat "$log"
    if [ "$result" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-|fixture failed' "$log" ||
       ! grep -q 'imageless-fb cases=6 failures=0' "$log"; then
        echo "【红】imageless-fb config=$config rc=$result FAIL" >&2
        failed=1
    fi
done
# 负对照：去掉 VkFramebufferAttachmentImageInfoKHR.flags 透传（强置 0），MUTABLE_FORMAT 附件必须被 VVL 抓出 VUID-VkRenderPassBeginInfo-framebuffer-03209。
mut="$out/mut"; mkdir -p "$mut"
python3 - "$repo/android/native/nvn/backend_vulkan.c" "$mut/backend_vulkan.c" <<'PYMUT'
import pathlib, sys
s = pathlib.Path(sys.argv[1]).read_text()
a = 'const VkImageCreateFlags ifb_flags = atts[i].flags;'
if s.count(a) != 1: raise SystemExit('pc_ifb flags mutation anchor changed')
pathlib.Path(sys.argv[2]).write_text(s.replace(a, 'const VkImageCreateFlags ifb_flags = 0;'))
PYMUT
msrc=(); for f in "${src[@]}"; do [ "$f" = "$repo/android/native/nvn/backend_vulkan.c" ] && f="$mut/backend_vulkan.c"; msrc+=("$f"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${msrc[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test-neg"
set +e
timeout 120 "$out/test-neg" "$out/shaders" 0 0 1 0 > "$out/neg-flags.log" 2>&1
neg_rc=$?
set -e
neg_vvl=$(grep -cE 'VUID-VkRenderPassBeginInfo-framebuffer-03209' "$out/neg-flags.log" || true)
if [ "$neg_vvl" -eq 0 ] || ! grep -q 'VUID-VkRenderPassBeginInfo-framebuffer-03209' "$out/neg-flags.log"; then
    echo '【红】imageless-fb 负对照：抹掉 attachment create_flags 后 VVL 未抓出预期 framebuffer-03209' >&2
    tail -n 20 "$out/neg-flags.log" >&2
    failed=1
else
    echo '【负对照开始】'
    echo "imageless-fb flags negative: PASS (neg_rc=$neg_rc 抓出 $neg_vvl 条 create_flags 失配校验报错: framebuffer-03209)"
    echo '【负对照结束】'
fi
exit "$failed"
