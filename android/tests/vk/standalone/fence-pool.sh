#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --fence-pool
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-fence-pool"
mkdir -p "$out"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }

inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
common_src=()
for f in android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do
    common_src+=("$repo/$f")
done

build_bin() {
    local bv=$1 bin=$2
    cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
        -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
        -I "$repo/android/native/nvn" "${inc[@]}" \
        "$repo/android/tests/vk/t_fence_pool.c" "$repo/graphics/nvn/replay/nvn_unimpl.c" "$bv" "${common_src[@]}" \
        -Wl,--gc-sections \
        -Wl,--wrap=vkCreateFence -Wl,--wrap=vkDestroyFence -Wl,--wrap=vkResetFences -Wl,--wrap=vkQueueSubmit \
        -lvulkan -lpthread -lm -o "$bin"
}

build_bin "$repo/android/native/nvn/backend_vulkan.c" "$out/test"

export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD"
export VK_LAYER_PATH="$S2A_VK_LAYER_DIR"
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
unset VK_LAYER_ENABLES
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"

failed=0
run_cfg() {
    local bin=$1 dq=$2 flag=$3 tag=$4
    local log="$out/${tag}.log"
    set +e
    timeout 120 "$bin" "$dq" "$flag" 2>&1 | tee "$log"
    local rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
       ! grep -q "fence-pool dq=${dq} flag=${flag} .* cases=4 failures=0" "$log"; then
        echo "【红】fence-pool ${tag}: rc=${rc} or validation failure" >&2
        failed=1
    fi
}

run_cfg "$out/test" 0 0 "dq0-f0"
run_cfg "$out/test" 0 1 "dq0-f1"
run_cfg "$out/test" 1 0 "dq1-f0"
run_cfg "$out/test" 1 1 "dq1-f1"

for dq in 0 1; do
    for tl in 0 1; do
        log="$out/wsi-dq$dq-tl$tl.log"
        set +e
        timeout 120 "$out/test" "$dq" 1 "$tl" 2>&1 | tee "$log"
        rc=${PIPESTATUS[0]}
        set -e
        if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
           ! grep -q "fence-pool WSI dq=$dq flag=1 tl=$tl frames=16 .* failures=0" "$log"; then
            echo "【红】fence-pool WSI dq=$dq tl=$tl rc=$rc" >&2
            failed=1
        fi
    done
done

# 变异负对照：在 seg_take_fence 跳过 vkResetFences，使已触发栅栏被二次提交给 vkQueueSubmit，
# 确认 Khronos 校验层（fence 未重置提交）与用例断言立即拦截（不回流主输出）。
echo "【负对照开始】fence-pool：跳过 seg_take_fence 的 vkResetFences（预期校验层与用例断言拦截，不回流主输出）"
mut="$out/mut"
mkdir -p "$mut"
python3 - "$repo/android/native/nvn/backend_vulkan.c" "$mut/backend_vulkan.c" <<'PY'
import sys
src = open(sys.argv[1], 'r', encoding='utf-8').read()
old = 'VkResult rr = vkResetFences(g.d, 1, &f);'
new = 'VkResult rr = VK_SUCCESS;'
assert old in src, 'fence-pool mutation anchor missing'
open(sys.argv[2], 'w', encoding='utf-8').write(src.replace(old, new, 1))
PY
build_bin "$mut/backend_vulkan.c" "$mut/test"
set +e
(timeout 120 "$mut/test" 0 1 > "$mut/run.log" 2>&1) 2>/dev/null
mut_rc=$?
set -e
mut_vvl=$(grep -c 'VUID-vkQueueSubmit-fence-00063' "$mut/run.log" || true)
mut_fail=$(grep -c 'FAIL ' "$mut/run.log" || true)
if [ "$mut_rc" -eq 0 ] || [ "$mut_vvl" -eq 0 ]; then
    echo "【红】fence-pool 变异负对照未按预期拦截（rc=${mut_rc}, fail=${mut_fail}, vvl=${mut_vvl}）" >&2
    failed=1
else
    echo "fence-pool 变异负对照 PASS：已拦截（rc=${mut_rc}, 断言命中=${mut_fail}, 校验命中=${mut_vvl}）"
fi
echo "【负对照结束】"

exit "$failed"
