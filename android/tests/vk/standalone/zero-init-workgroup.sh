#!/usr/bin/env bash
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑，可单跑 --zero-init-workgroup。
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="${S2A_ZERO_INIT_WORKGROUP_OUT:-$repo/build/vk-zero-init-workgroup}"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/zero_init_wg.comp" -o "$out/shaders/zero_init_wg_raw.spv"
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/plain_wg.comp" -o "$out/shaders/plain_wg.comp.spv"

# 给 Workgroup 变量的 OpVariable 补上 OpConstantNull 初始化器（Vulkan 规范要求 VK_KHR_zero_initialize_workgroup_memory）
python3 - "$out/shaders/zero_init_wg_raw.spv" "$out/shaders/zero_init_wg.comp.spv" <<'PY'
import struct, sys
raw = open(sys.argv[1], 'rb').read()
assert len(raw) >= 20 and len(raw) % 4 == 0
words = list(struct.unpack('<' + 'I' * (len(raw) // 4), raw))
assert words[0] == 0x07230203
ptr_pointee = {}
i = 5
while i < len(words):
    w0 = words[i]; ln = w0 >> 16; op = w0 & 0xffff
    if op == 32 and ln == 4:  # OpTypePointer
        ptr_pointee[words[i + 1]] = words[i + 3]
    i += ln
null_id = words[3]
words[3] += 1
out = words[:5]
patched = 0
i = 5
while i < len(words):
    w0 = words[i]; ln = w0 >> 16; op = w0 & 0xffff
    inst = words[i:i + ln]
    if op == 59 and ln == 4 and inst[3] == 4:  # OpVariable ... Workgroup
        arr_ty = ptr_pointee[inst[1]]
        out.extend([(3 << 16) | 46, arr_ty, null_id])  # OpConstantNull
        out.extend([(5 << 16) | 59, inst[1], inst[2], inst[3], null_id])
        patched += 1
    else:
        out.extend(inst)
    i += ln
assert patched == 1, f'expected 1 Workgroup OpVariable, got {patched}'
open(sys.argv[2], 'wb').write(struct.pack('<' + 'I' * len(out), *out))
PY
if command -v spirv-val >/dev/null 2>&1; then
    spirv-val --target-env vulkan1.1 "$out/shaders/zero_init_wg.comp.spv"
fi

inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=(
    "$repo/android/tests/vk/t_zero_init_workgroup.c"
    "$repo/graphics/nvn/replay/nvn_unimpl.c"
    "$repo/android/native/nvn/backend_vulkan.c"
    "$repo/android/native/nvn/nvn_pipe_flight.c"
    "$repo/android/native/nvn/nvn_vk_pipe_once.c"
    "$repo/android/native/direct/vk_device.c"
    "$repo/android/native/direct/gs_path.c"
    "$repo/android/native/capprobe/capprobe_host.c"
    "$repo/android/native/nvn/nvn_vk_format.c"
    "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c"
    "$repo/android/native/nvn/nvn_vk_bcdec.c"
    "$repo/android/native/nvn/nvn_vk_driver_cache.c"
    "$repo/android/native/direct/sha256.c"
)
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]}" \
    -Wl,--wrap=vkEnumerateDeviceExtensionProperties \
    -Wl,--wrap=vkGetPhysicalDeviceFeatures2 \
    -Wl,--wrap=vkCreateDevice \
    -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"

export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
unset VK_LAYER_ENABLES
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"

for mode in on off hide-ext hide-feat; do
    log="$out/$mode.log"
    timeout 60 "$out/test" "$out/shaders" "$mode" > "$log" 2>&1 || { cat "$log"; exit 1; }
    if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
        cat "$log"
        exit 1
    fi
    grep '^zero-init-workgroup ' "$log"
done

# 负对照（撤退验证）：关闭开关但按启用期望验收，必报红退非零且合规守卫拦截不触发 VVL 错
log="$out/withdraw.log"
echo "【负对照开始】"
set +e
timeout 60 "$out/test" "$out/shaders" withdraw > "$log" 2>&1
rc=$?
set -e
grep '^zero-init-workgroup \|【红】' "$log" || true
echo "【负对照结束】"
if [ "$rc" -eq 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
    cat "$log"
    echo "【红】withdraw negative control unexpectedly succeeded or triggered VVL (rc=$rc)" >&2
    exit 1
fi
echo "PASS: zero-init-workgroup negative withdraw rejected as expected (rc=$rc)"
