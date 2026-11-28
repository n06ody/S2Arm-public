#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --hot-hash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-hot-hash"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
for shader in pushc.frag feedback.frag tbuf.comp; do
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
    local bv=$1 bin=$2
    cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
        -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
        -I "$repo/android/native/nvn" "${inc[@]}" \
        "$repo/android/tests/vk/t_hot_hash.c" "$repo/graphics/nvn/replay/nvn_unimpl.c" "$bv" "${common_src[@]}" \
        -Wl,--gc-sections -lvulkan -lpthread -lm -o "$bin"
}

build_bin "$repo/android/native/nvn/backend_vulkan.c" "$out/test"

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
    local bin=$1 dq=$2 flag=$3 chk=$4 tag=$5
    local log="$out/${tag}.log"
    set +e
    timeout 120 "$bin" "$out/shaders" "$dq" "$flag" "$chk" 2>&1 | tee "$log"
    local rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-' "$log" ||
       ! grep -q "hot-hash dq=${dq} flag=${flag} chk=${chk} .* cases=3 failures=0" "$log"; then
        echo "【红】hot-hash ${tag}: rc=${rc} or validation failure" >&2
        failed=1
    fi
}

run_cfg "$out/test" 0 0 1 "dq0-f0"
run_cfg "$out/test" 0 1 1 "dq0-f1"
run_cfg "$out/test" 1 0 1 "dq1-f0"
run_cfg "$out/test" 1 1 1 "dq1-f1"

# 100% 强制哈希碰撞压测：将 hot_ptr_hash 与 hot_bytes_hash 全部折叠为常量 0，
# 验证三张哈希表在最坏全碰撞下键等值比对、链表/开放寻址探测、扩容与删除 100% 正确且 0 校验错。
col="$out/collision"
mkdir -p "$col"
python3 - "$repo/android/native/nvn/backend_vulkan.c" "$col/backend_vulkan.c" <<'PY'
import sys
src = open(sys.argv[1], 'r', encoding='utf-8').read()
old1 = '    x ^= x >> 33;\n    return x;'
new1 = '    x ^= x >> 33;\n    (void)x; return 0;'
old2 = '    h ^= h >> 33;\n    return h;'
new2 = '    h ^= h >> 33;\n    (void)h; return 0;'
assert old1 in src and old2 in src, 'hot-hash collision anchors missing'
src = src.replace(old1, new1, 1).replace(old2, new2, 1)
open(sys.argv[2], 'w', encoding='utf-8').write(src)
PY
build_bin "$col/backend_vulkan.c" "$col/test"
run_cfg "$col/test" 0 1 0 "collision-all-zero"

# 延迟索引分配，先让稠密表增长。恢复时要按实际容量重建，继续用原像素与句柄判据。
oom="$out/oom"
mkdir -p "$oom"
python3 - "$repo/android/native/nvn/backend_vulkan.c" "$oom/backend_vulkan.c" <<'PY'
from pathlib import Path
import sys

src = Path(sys.argv[1]).read_text()
for function, ready in (
    ("lay_ht_rehash(struct nvn_gpu_cmd_s *c, uint32_t new_cap)", "c->nlay > 96"),
    ("pc_rp_ht_rehash_locked(uint32_t new_cap)", "g_pc_nrp > 64"),
):
    anchor = function + " {\n    uint32_t *nt = new_cap ?"
    assert src.count(anchor) == 1, function
    src = src.replace(anchor, function + f" {{\n    uint32_t *nt = new_cap && {ready} ?", 1)
Path(sys.argv[2]).write_text(src)
PY
build_bin "$oom/backend_vulkan.c" "$oom/test"
run_cfg "$oom/test" 0 1 0 "oom-recovery"
if ! awk '/^hot-hash / {
    for (i = 1; i <= NF; ++i) {
        if ($i ~ /^rp=/) { split(substr($i, 4), r, "/"); rp = r[2] + 0 }
        if ($i ~ /^lay=/) { split(substr($i, 5), l, "/"); lay = l[2] + 0 }
    }
} END { exit !(rp > 0 && rp < 1152 && lay > 0 && lay < 20000) }' "$out/oom-recovery.log"; then
    echo "【红】hot-hash 索引分配恢复后仍未命中哈希路径" >&2
    failed=1
fi

# 变异负对照：破坏 pc_pass 哈希命中时的全键等值判断（跳过 memcmp 直接视作命中），
# 确认 Khronos 校验层能立刻拦截不匹配的渲染通道复用（命中 00876 规则）。
echo "【负对照开始】hot-hash：破坏 pc_pass 键等值判断（预期校验层拦截，不回流主输出）"
mut="$out/mut"
mkdir -p "$mut"
python3 - "$repo/android/native/nvn/backend_vulkan.c" "$mut/backend_vulkan.c" <<'PY'
import sys
src = open(sys.argv[1], 'r', encoding='utf-8').read()
old = 'const int pc_rp_key_eq = entry->hash == h && !memcmp(entry, &k, offsetof(struct pc_rp, rp));'
new = 'const int pc_rp_key_eq = 1;'
assert old in src, 'hot-hash mutation anchor missing'
open(sys.argv[2], 'w', encoding='utf-8').write(src.replace(old, new, 1))
PY
build_bin "$mut/backend_vulkan.c" "$mut/test"
set +e
timeout 120 "$mut/test" "$out/shaders" 0 1 0 > "$mut/run.log" 2>&1
mut_rc=$?
set -e
mut_vvl=$(grep -cE 'Validation Error|VUID-' "$mut/run.log" || true)
mut_rule=$(grep -c '00876' "$mut/run.log" || true)
if [ "$mut_vvl" -eq 0 ] || [ "$mut_rule" -eq 0 ]; then
    echo "【红】hot-hash 变异负对照未按预期拦截（rc=${mut_rc}, hits=${mut_vvl}, rule00876=${mut_rule}）" >&2
    failed=1
else
    echo "hot-hash 变异负对照 PASS：已拦截（rc=${mut_rc}, 校验命中=${mut_vvl}, 规则00876=${mut_rule}）"
fi
echo "【负对照结束】"

exit "$failed"
