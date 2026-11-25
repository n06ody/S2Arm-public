#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# run.sh --replay-pass-area：真实回放、Vk 开遍参数、整面像素与逐守卫撤回。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-replay-pass-area"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}" "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
for shader in tri3.vert solid_green.frag solid_blue.frag; do
    "${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
inc=(-I "$repo/build/c883-ndk")
for d in android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$d")
done
src=("$repo/android/tests/vk/t_replay_pass_area.c")
for f in graphics/nvn/replay/rp_res.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c graphics/nvn/gfx/nvn/nvn_dirty.c \
         android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c \
         android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c \
         android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c \
         graphics/nvn/gfx/nvn/nvn_tex.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c graphics/nvn/gfx/tex/nvn_deswizzle.c; do
    src+=("$repo/$f")
done
compile() {
    local rp=$1 vk=$2 binary=$3
    cc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
        -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" "$rp" "$vk" \
        -Wl,--gc-sections -Wl,--wrap=vkGetDeviceProcAddr -Wl,--wrap=vkGetRenderAreaGranularity \
        -lvulkan -lpthread -lm -o "$binary"
}
rp="$repo/graphics/nvn/replay/rp_pass.c"
vk="$repo/android/native/nvn/backend_vulkan.c"
compile "$rp" "$vk" "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
for dq in 0 1; do
    if [ "$dq" = 1 ]; then export S2A_TEST_NVK_DQ=1; else unset S2A_TEST_NVK_DQ; fi
    for arm in 0 1; do
        log="$out/dq$dq-arm$arm.log"
        if ! timeout 90 "$out/test" "$out/shaders" "$arm" > "$log" 2>&1 ||
           grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log" ||
           ! grep -q 'pass-area Vulkan cases=9 failures=0' "$log"; then
            cat "$log"
            exit 1
        fi
        echo "pass-area Vulkan: DQ=$dq arm=$arm cases=9 VUID=0 SYNC-HAZARD=0"
    done
done

# 编译同一份源码的撤回版本，逐局只撤一条守卫。有两层保护的用例，两处一起撤。
python3 - "$rp" "$vk" "$out" <<'PY'
import sys
from pathlib import Path

rp, vk, out = map(Path, sys.argv[1:])
helper = '''
static int omit_area_guard(const char *guard) {
    const char *mode = getenv("S2A_TEST_OMIT_AREA_GUARD");
    return mode && !strcmp(mode, guard);
}
'''
def replace_once(src, old, new):
    if src.count(old) != 1:
        raise SystemExit(f"pass-area mutation anchor changed: {old}")
    return src.replace(old, new)

core = rp.read_text()
for old, new in (
    ('static int pass_area_contains(', helper + 'static int pass_area_contains('),
    ('if (P.area_on && (!known || !pass_area_contains',
     'if (!omit_area_guard("draw") && P.area_on && (!known || !pass_area_contains'),
    ('if (P.area_on && !pass_area_contains(cl->x',
     'if (!omit_area_guard("clear") && P.area_on && !pass_area_contains(cl->x'),
    ('if (p->serial == t->serial ||\n            (address <= other ? other - address < t->L.size : address - other < p->L.size)) return 0;',
     'if (!omit_area_guard("alias") && (p->serial == t->serial ||\n            (address <= other ? other - address < t->L.size : address - other < p->L.size))) return 0;'),
    ('if (d->color[i].load != NVN_GPU_LOAD_LOAD || d->color[i].store != NVN_GPU_STORE_STORE ||',
     'if ((!omit_area_guard("zero") && d->color[i].load != NVN_GPU_LOAD_LOAD) || d->color[i].store != NVN_GPU_STORE_STORE ||'),
):
    core = replace_once(core, old, new)
backend = vk.read_text()
for old, new in (
    ('static VkRenderPass make_pass(', helper + 'static VkRenderPass make_pass('),
    ('if (att[i].samples != VK_SAMPLE_COUNT_1_BIT || att_tex[i]->target',
     'if ((!omit_area_guard("msaa") && att[i].samples != VK_SAMPLE_COUNT_1_BIT) || att_tex[i]->target'),
    ('(att[i].loadOp != VK_ATTACHMENT_LOAD_OP_LOAD || att[i].storeOp != VK_ATTACHMENT_STORE_OP_STORE)',
     '((!omit_area_guard("zero") && att[i].loadOp != VK_ATTACHMENT_LOAD_OP_LOAD) || att[i].storeOp != VK_ATTACHMENT_STORE_OP_STORE)'),
    ('x0 -= x0 % granularity.width; y0 -= y0 % granularity.height;',
     'if (omit_area_guard("align")) return (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1-x0), (uint32_t)(y1-y0)}};\n    x0 -= x0 % granularity.width; y0 -= y0 % granularity.height;'),
    ('if (!d->area_w || !d->area_h) return 1;',
     'if (omit_area_guard("draw") || !d->area_w || !d->area_h) return 1;'),
):
    backend = replace_once(backend, old, new)
(out / "rp_pass_withdrawn.c").write_text("#include <stdlib.h>\n" + core)
(out / "backend_vulkan_withdrawn.c").write_text(backend)
PY
compile "$out/rp_pass_withdrawn.c" "$out/backend_vulkan_withdrawn.c" "$out/test-withdrawn"
unset S2A_TEST_NVK_DQ
while read -r guard case_id expected; do
    log="$out/withdrawn-$guard-$case_id.log"
    rc=0
    S2A_TEST_OMIT_AREA_GUARD="$guard" timeout 90 "$out/test-withdrawn" "$out/shaders" 1 "$case_id" > "$log" 2>&1 || rc=$?
    if [ "$rc" != 1 ] || ! grep -qE "^FAIL .*($expected)" "$log"; then
        cat "$log"
        exit 1
    fi
    echo "pass-area withdrawal: guard=$guard case=$case_id expected assertion red"
done <<'CASES'
align 0 area case=0 pass=0
draw 1 case=1 pass count
draw 2 case=2 pass count
clear 3 case=3 draw count
clear 4 case=4 draw count
zero 5 area case=5 pass=0
alias 6 area case=6 pass=0
msaa 7 area case=7 pass=1
draw 8 case=8 pass count
CASES
echo '【绿】局部 renderArea：真实开遍参数、整面读回、DQ 双档与逐守卫撤回'
