#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --timeline-fence
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-timeline-fence"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
cat > "$out/shaders/solid_pc.frag" <<'GLSL'
#version 450
layout(push_constant) uniform PC { vec4 c; } pc;
layout(location = 0) out vec4 color0;
void main() { color0 = pc.c; }
GLSL
"$glslc" --target-env=vulkan1.1 "$out/shaders/solid_pc.frag" -o "$out/shaders/solid_pc.frag.spv"
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=("$repo/android/tests/vk/t_timeline_fence.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
wrap=()
for fn in vkEnumerateDeviceExtensionProperties vkCreateDevice vkCreateSemaphore vkCreateFence vkDestroyFence vkWaitForFences vkQueueSubmit vkGetDeviceProcAddr; do
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
printf 'timeline-fence validation: sync=1 shader_accesses_heuristic=1 settings=%s\n' "$VK_LAYER_SETTINGS_PATH"
failed=0
# 6 configs:
#   dq=0/1 x opt=0/1 with tl=1, hide=0 (active VK_KHR_timeline_semaphore + O(1) batch retirement)
#   dq=0 opt=0 tl=0 hide=0 (A/B off -> regular VkFence)
#   dq=0 opt=0 tl=1 hide=1 (--wrap hides VK_KHR_timeline_semaphore -> zero-regression fallback)
for config in '0 0 1 0' '0 1 1 0' '1 0 1 0' '1 1 1 0' '0 0 0 0' '0 0 1 1'; do
    read -r dq opt tl hidden <<< "$config"
    log="$out/dq$dq-opt$opt-tl$tl-hide$hidden.log"
    set +e
    timeout 120 "$out/test" "$out/shaders" "$dq" "$opt" "$tl" "$hidden" > "$log" 2>&1
    result=$?
    set -e
    cat "$log"
    if [ "$result" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-|fixture failed' "$log" ||
       ! grep -q 'timeline-fence cases=5 failures=0' "$log"; then
        echo "【红】timeline-fence config=$config rc=$result FAIL" >&2
        failed=1
    fi
done
# 负对照：破坏 timeline 提交序号的单调递增（恒发 1），Khronos VVL 必须抓出非单调信号报错。
mut="$out/mut"; mkdir -p "$mut"
python3 - "$repo/android/native/nvn/backend_vulkan.c" "$mut/backend_vulkan.c" <<'PYMUT'
import pathlib, sys
s = pathlib.Path(sys.argv[1]).read_text()
a = 'const uint64_t tl_seq = g.tl_on ? g.tl_next + 1 : 0;'
if s.count(a) != 1: raise SystemExit('timeline signal monotonic mutation anchor changed')
pathlib.Path(sys.argv[2]).write_text(s.replace(a, 'const uint64_t tl_seq = g.tl_on ? 1 : 0;'))
PYMUT
msrc=(); for f in "${src[@]}"; do [ "$f" = "$repo/android/native/nvn/backend_vulkan.c" ] && f="$mut/backend_vulkan.c"; msrc+=("$f"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${msrc[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test-neg"
set +e
timeout 120 "$out/test-neg" "$out/shaders" 0 0 1 0 > "$out/neg-nonmono.log" 2>&1
neg_rc=$?
set -e
neg_vvl=$(grep -cE 'Validation Error|VUID-vkQueueSubmit-pSignalSemaphores' "$out/neg-nonmono.log" || true)
if [ "$neg_rc" -eq 0 ] || [ "$neg_vvl" -eq 0 ]; then
    echo '【红】timeline-fence 负对照：破坏序号单调递增后 VVL 未抓出预期报错' >&2
    tail -n 20 "$out/neg-nonmono.log" >&2
    failed=1
else
    echo '【负对照开始】'
    echo "timeline-fence monotonic negative: PASS (neg_rc=$neg_rc 抓出 $neg_vvl 条非单调 timeline 信号校验报错)"
    echo '【负对照结束】'
fi
exit "$failed"
