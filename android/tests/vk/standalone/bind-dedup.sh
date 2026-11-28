#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --bind-dedup
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-bind-dedup"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
for shader in ubo_dyn.frag ubo_dyn34.frag pushc.comp pushc.frag; do
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done

inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_bind_dedup.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do
    src+=("$repo/$f")
done
wrap=(-Wl,--wrap=vkGetDeviceProcAddr -Wl,--wrap=vkCmdBindDescriptorSets -Wl,--wrap=vkCmdBindPipeline -Wl,--wrap=vkCmdPushConstants)
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"

export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD"
export VK_LAYER_PATH="$S2A_VK_LAYER_DIR"
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"

failed=0
run_cfg() {
    local dq=$1 memo=$2 flag=$3
    local log="$out/dq${dq}-memo${memo}-f${flag}.log"
    set +e
    timeout 120 "$out/test" "$out/shaders" "$dq" "$memo" "$flag" 2>&1 | tee "$log"
    local rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-' "$log" ||
       ! grep -q "bind-dedup dq=${dq} memo=${memo} flag=${flag} .* cases=6 failures=0" "$log"; then
        echo "【红】bind-dedup dq=${dq} memo=${memo} flag=${flag}: rc=${rc} or validation failure" >&2
        failed=1
    fi
}

run_cfg 0 1 0
run_cfg 0 1 1
run_cfg 1 1 0
run_cfg 1 1 1
run_cfg 0 0 1
run_cfg 1 0 1

# 变异负对照：破坏最小脏区间绑定起点 first_set（lo -> lo + 1），
# 确认 Khronos 校验层真的在检查 vkCmdBindDescriptorSets 的局部区间参数（命中 00360 规则）。
echo "【负对照开始】bind-dedup：破坏最小脏区间 first_set（预期校验层拦截，不回流主输出）"
mut="$out/mut"
mkdir -p "$mut"
python3 - "$repo/android/native/nvn/backend_vulkan.c" "$mut/backend_vulkan.c" <<'PY'
import sys
src = open(sys.argv[1], 'r', encoding='utf-8').read()
old = 'const uint32_t first_set = lo;'
new = 'const uint32_t first_set = lo + 1;'
assert old in src, 'bind-dedup mutation anchor missing'
open(sys.argv[2], 'w', encoding='utf-8').write(src.replace(old, new, 1))
PY
mut_src=("$repo/android/tests/vk/t_bind_dedup.c" "$repo/graphics/nvn/replay/nvn_unimpl.c" "$mut/backend_vulkan.c")
for f in android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do
    mut_src+=("$repo/$f")
done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    -I "$repo/android/native/nvn" "${inc[@]}" "${mut_src[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$mut/test"
set +e
timeout 120 "$mut/test" "$out/shaders" 0 1 1 > "$mut/run.log" 2>&1
mut_rc=$?
set -e
mut_vvl=$(grep -cE 'Validation Error|VUID-' "$mut/run.log" || true)
mut_rule=$(grep -c '00360' "$mut/run.log" || true)
if [ "$mut_vvl" -eq 0 ] || [ "$mut_rule" -eq 0 ]; then
    echo "【红】bind-dedup 变异负对照未按预期拦截（rc=${mut_rc}, hits=${mut_vvl}, rule00360=${mut_rule}）" >&2
    failed=1
else
    echo "bind-dedup 变异负对照 PASS：已拦截（rc=${mut_rc}, 校验命中=${mut_vvl}, 规则00360=${mut_rule}）"
fi
echo "【负对照结束】"

exit "$failed"
