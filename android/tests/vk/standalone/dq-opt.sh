#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --dq-opt
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-dq-opt"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
for shader in ubo_dyn.frag pushc.comp; do
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done

inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
common_src=()
for f in android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do
    common_src+=("$repo/$f")
done

build_bin() {
    local inc_dir=$1 bin=$2
    cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
        -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
        -I "$inc_dir" "${inc[@]}" \
        "$repo/android/tests/vk/t_dq_opt.c" "$repo/graphics/nvn/replay/nvn_unimpl.c" \
        "$repo/android/native/nvn/backend_vulkan.c" "${common_src[@]}" \
        -Wl,--gc-sections -lvulkan -lpthread -lm -o "$bin"
}

build_bin "$repo/android/native/nvn" "$out/test"

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
    local bin=$1 flag=$2 tag=$3
    local log="$out/${tag}.log"
    set +e
    timeout 120 "$bin" "$out/shaders" "$flag" 2>&1 | tee "$log"
    local rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
       ! grep -q "dq-opt flag=${flag} .* cases=5 failures=0" "$log"; then
        echo "【红】dq-opt ${tag}: rc=${rc} or validation failure" >&2
        failed=1
    fi
}

run_cfg "$out/test" 0 "f0"
run_cfg "$out/test" 1 "f1"

# 变异负对照：破坏 vk_dq_drain 的未发布尾部强制刷新（跳过 dq_flush_pub），
# 第一段填充留在环里，GPU 收到空命令缓冲，读回必须仍是原值并命中短段断言。
echo "【负对照开始】dq-opt：跳过 vk_dq_drain 的 dq_flush_pub（预期短段读回失败）"
mut="$out/mut"
mkdir -p "$mut"
cp "$repo/android/native/nvn/backend_vulkan.c" "$mut/backend_vulkan.c"
python3 - "$repo/android/native/nvn/nvn_vk_dq.inc" "$mut/nvn_vk_dq.inc" <<'PY'
import sys
src = open(sys.argv[1], 'r', encoding='utf-8').read()
old = 'if (dq_need_flush) dq_flush_pub();'
new = 'if (0 && dq_need_flush) dq_flush_pub();'
assert old in src, 'dq-opt mutation anchor missing'
open(sys.argv[2], 'w', encoding='utf-8').write(src.replace(old, new, 1))
PY
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    -I "$mut" -I "$repo/android/native/nvn" "${inc[@]}" \
    "$repo/android/tests/vk/t_dq_opt.c" "$repo/graphics/nvn/replay/nvn_unimpl.c" \
    "$mut/backend_vulkan.c" "${common_src[@]}" \
    -Wl,--gc-sections -lvulkan -lpthread -lm -o "$mut/test"
set +e
(timeout 60 "$mut/test" "$out/shaders" 1 > "$mut/run.log" 2>&1) 2>/dev/null
mut_rc=$?
set -e
mut_vvl=$(grep -cE 'Validation Error|VUID-' "$mut/run.log" || true)
mut_fail=$(grep -c 'FAIL ' "$mut/run.log" || true)
if [ "$mut_rc" -ne 1 ] || ! grep -q '^FAIL short-tail ' "$mut/run.log"; then
    echo "【红】dq-opt 变异负对照未按预期拦截（rc=${mut_rc}, fail=${mut_fail}, vvl=${mut_vvl}）" >&2
    failed=1
else
    grep '^FAIL short-tail ' "$mut/run.log"
    echo "dq-opt 变异负对照 PASS：已拦截（rc=${mut_rc}, 断言命中=${mut_fail}, 校验命中=${mut_vvl}）"
fi
echo "【负对照结束】"

exit "$failed"
