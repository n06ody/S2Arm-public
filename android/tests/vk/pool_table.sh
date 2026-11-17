#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
if [ "$(uname -s)" = Darwin ]; then
    export S2A_VULKAN_CACHE="$repo/build/vulkan-cache"
    exec bash "$repo/android/tests/vk/run.sh" "--pool-table${1:-}"
fi
icd=${S2A_LAVAPIPE_ICD:-/usr/share/vulkan/icd.d/lvp_icd.x86_64.json}
layer=${S2A_VK_LAYER_DIR:-/usr/share/vulkan/explicit_layer.d}
[ -f "$icd" ] && [ -f "$layer/VkLayer_khronos_validation.json" ] || { echo 'missing lavapipe or validation' >&2; exit 2; }
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers not staged' >&2; exit 2; }
out="$repo/build/vk-pool-table"; mkdir -p "$out"
mode=${1:-}
# 竞态用例不建 VkDevice：只链后端与依赖，不跑 lavapipe/VVL（那里 ASan 会撞 LLVM JIT）。
if [ "$mode" = -race ] || [[ "$mode" == -race-neg-* ]]; then
    inc=(); for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
    src=(android/tests/vk/t_pool_race.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c graphics/nvn/replay/nvn_unimpl.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c)
    for i in "${!src[@]}"; do src[$i]="$repo/${src[$i]}"; done
    if [ "$mode" != -race ]; then
        cp "$repo/android/native/nvn/backend_vulkan.c" "$out/race_mutant.c"
        python3 - "$repo/android/native/nvn/backend_vulkan_pool.inc" "$out/backend_vulkan_pool.inc" "$mode" <<'PYRACE'
import pathlib, sys
src, dst, mode = sys.argv[1:]
s = pathlib.Path(src).read_text()
mutations = {
 '-race-neg-atomic': (
   "    if (!p || p->released) { pthread_mutex_unlock(&g_mu); return 0; }\n    p->refs++;\n    pthread_mutex_unlock(&g_mu);\n",
   "    if (!p || p->released) { pthread_mutex_unlock(&g_mu); return 0; }\n    pthread_mutex_unlock(&g_mu);\n#ifdef S2A_NVK_POOL_HOST_TEST\n    if (g_pool_test_hook) { void (*hook)(nvn_gpu_pool_table) = g_pool_test_hook; g_pool_test_hook = NULL; hook(t); }\n#endif\n    pthread_mutex_lock(&g_mu);\n    p->refs++;\n    pthread_mutex_unlock(&g_mu);\n"),
 '-race-neg-released': (
   'if (!p || p->released) { pthread_mutex_unlock(&g_mu); return 0; }',
   'if (!p) { pthread_mutex_unlock(&g_mu); return 0; }'),
 '-race-neg-unref': (
   "static void pool_cmd_unref(struct nvn_gpu_cmd_s *c, nvn_gpu_pool_table t) {\n    if (c->nrefs && c->refs[c->nrefs - 1].obj == t && c->refs[c->nrefs - 1].type == REF_POOL) c->nrefs--;\n    pthread_mutex_lock(&g_mu);\n    unref_locked(&g, t, REF_POOL);\n    pthread_mutex_unlock(&g_mu);\n}",
   "static void pool_cmd_unref(struct nvn_gpu_cmd_s *c, nvn_gpu_pool_table t) { (void)c; (void)t; }"),
}
if mode not in mutations: raise SystemExit('unknown pool race mutation: ' + mode)
a, b = mutations[mode]
if s.count(a) != 1: raise SystemExit('pool race mutation anchor changed: ' + mode)
pathlib.Path(dst).write_text(s.replace(a, b))
PYRACE
        src[1]="$out/race_mutant.c"
    fi
    cc -fsanitize=address -fno-omit-frame-pointer -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 -DS2A_NVK_POOL_HOST_TEST=1 "${inc[@]}" "${src[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/race"
    set +e
    ASAN_OPTIONS=detect_leaks=0 timeout 120 "$out/race" 2>&1 | tee "$out/race.log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$mode" = -race ]; then
        [ "$rc" -eq 0 ] || exit 1
        ! grep -q "AddressSanitizer" "$out/race.log" || exit 1
        exit 0
    fi
    [ "$rc" -ne 0 ] || { echo "mutation $mode unexpectedly passed" >&2; exit 1; }
    case "$mode" in
        -race-neg-atomic) expected='AddressSanitizer|FAIL race bind' ;;
        -race-neg-released) expected='FAIL released table resurrected' ;;
        -race-neg-unref) expected='FAIL layout failure leaked ref' ;;
        *) exit 2 ;;
    esac
    grep -qE "$expected" "$out/race.log" || { echo "mutation $mode failed somewhere else" >&2; exit 1; }
    echo "negative control $mode: expected failure observed"
    exit 0
fi
"${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/pool_table.comp" -o "$out/pool_table.comp.spv"
"${GLSLC:-glslc}" --target-env=vulkan1.1 -DPOOL_ARRAY_LEN=64 "$repo/android/tests/vk/shaders/pool_table.comp" -o "$out/pool_table_small.comp.spv"
inc=(); for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=(android/tests/vk/t_pool_table.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c graphics/nvn/replay/nvn_unimpl.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c)
for i in "${!src[@]}"; do src[$i]="$repo/${src[$i]}"; done
if [ -n "$mode" ]; then
    cp "$repo/android/native/nvn/backend_vulkan.c" "$out/mutant.c"
    python3 - "$repo/android/native/nvn/backend_vulkan_pool.inc" "$out/backend_vulkan_pool.inc" "$mode" <<'PYMUT'
import pathlib, sys
src, dst, mode = sys.argv[1:]
s = pathlib.Path(src).read_text()
mutations = {
 '-neg-image': ('t->images[i].imageView = v->view;', 't->images[i].imageView = (i == 64 ? dv : v)->view;'),
 '-neg-sampler': ('t->images[i].sampler = d->sampler_integer ? s->int_sampler : s->sampler;', 't->images[i].sampler = (i == 64 ? ds : s)->sampler;'),
 '-neg-empty': ('t->objects[i] = v; ref_bump(v, REF_VIEW);', 'if (state == NVN_GPU_POOL_EMPTY && i < d->count) { v = t->objects[0]; } t->objects[i] = v; ref_bump(v, REF_VIEW);'),
 '-neg-type': ('v->tex->samples != dv->tex->samples) v = dv;', 'v->tex->samples != dv->tex->samples) v = t->objects[0];'),
 '-neg-numeric': ('else if (got != cls || v->type != dv->type', 'else if (got != cls) v = t->objects[0]; else if (v->type != dv->type'),
 '-neg-bounds': ('t->objects[i] = v; ref_bump(v, REF_VIEW);', 'if (i == d->count) { v = t->objects[0]; } t->objects[i] = v; ref_bump(v, REF_VIEW);'),
 '-neg-cap': ('if (!g.dev->descriptor_indexing_enabled)', 'if (0 && !g.dev->descriptor_indexing_enabled)'),
 '-neg-limit': ('(uint64_t)d->count + 1 > lim1', '(uint64_t)d->count + 1 > (lim1 | UINT32_MAX)'),
 '-neg-unavailable': ('if (state == NVN_GPU_POOL_UNAVAILABLE) {', 'if (state == NVN_GPU_POOL_UNAVAILABLE) { state = NVN_GPU_POOL_EMPTY; } if (state == NVN_GPU_POOL_UNAVAILABLE) {'),
 '-neg-rebind': ('c->pool_binds[st][ent] = t; c->pool_active = 1; c->desc_dirty', 'if (!c->pool_binds[st][ent]) { c->pool_binds[st][ent] = t; } c->pool_active = 1; c->desc_dirty'),
 '-neg-fence': ('    p->refs++;\n    pthread_mutex_unlock(&g_mu);\n    return cmd_ref_append(c, t, REF_POOL);', '    /* mutation: 不取引用 */\n    pthread_mutex_unlock(&g_mu);\n    return cmd_ref_append(c, t, REF_POOL);'),
 '-neg-aggregate': ('if (limit) { pool_cmd_unref(c, t); return pool_bind_error(c, err, NVN_GPU_E_UNSUPPORTED, limit); }', 'if (0 && limit) { pool_cmd_unref(c, t); return pool_bind_error(c, err, NVN_GPU_E_UNSUPPORTED, limit); }'),
 '-neg-sampler-resource': ('d->kind == NVN_GPU_POOL_IMAGE && (uint64_t)d->count + 1 > l->maxPerStageResources', '(uint64_t)d->count + 1 > l->maxPerStageResources'),
 '-neg-preflight': ('if (pool_shader_source(compute)) return pool_pipeline_limit(p, graphics);', 'if (0 && pool_shader_source(compute)) return pool_pipeline_limit(p, graphics);'),
}
if mode not in mutations: raise SystemExit('unknown pool mutation: ' + mode)
a, b = mutations[mode]
if s.count(a) != 1: raise SystemExit('pool mutation anchor changed: ' + mode)
pathlib.Path(dst).write_text(s.replace(a, b))
PYMUT
    src[1]="$out/mutant.c"
fi
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 -DS2A_NVK_POOL_HOST_TEST=1 "${inc[@]}" "${src[@]}" -Wl,--wrap=vkBeginCommandBuffer -Wl,--wrap=vkQueueSubmit -Wl,--wrap=vkAllocateDescriptorSets -Wl,--wrap=vkGetPhysicalDeviceFormatProperties -Wl,--wrap=vkCreatePipelineCache -Wl,--wrap=vkGetPipelineCacheData -Wl,--wrap=vkCreateGraphicsPipelines -Wl,--wrap=vkCreateComputePipelines -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
set +e
ASAN_OPTIONS=detect_leaks=0 VK_DRIVER_FILES="$icd" VK_LAYER_PATH="$layer" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation timeout 120 "$out/test" "$out" 2>&1 | tee "$out/run.log"
rc=${PIPESTATUS[0]}
set -e
if [ -n "$mode" ]; then
    [ "$rc" -ne 0 ] || { echo "mutation $mode unexpectedly passed" >&2; exit 1; }
    case "$mode" in
        -neg-image) expected='pool dispatch output\[10\]' ;;
        -neg-sampler) expected='pool dispatch output\[4\]' ;;
        -neg-empty) expected='pool dispatch output\[13\]' ;;
        -neg-type) expected='pool dispatch output\[17\]' ;;
        -neg-numeric) expected='pool dispatch output\[25\]' ;;
        -neg-bounds) expected='pool dispatch output\[21\]' ;;
        -neg-cap) expected='pool missing feature' ;;
        -neg-limit) expected='exceeds reduced stage sampler limit' ;;
        -neg-unavailable) expected='registered unavailable resource must reject' ;;
        -neg-rebind) expected='replacement output\[9\]' ;;
        -neg-fence) expected='pool command must hold snapshot reference' ;;
        -neg-aggregate) expected='aggregate stage resources 66 images' ;;
        -neg-sampler-resource) expected='samplers must not count against maxPerStageResources' ;;
        -neg-preflight) expected='small pool pipeline aggregate 64 images'  ;;
        *) exit 2 ;;
    esac
    grep -q "FAIL .*${expected}" "$out/run.log" || { echo "mutation $mode failed somewhere else" >&2; exit 1; }
    echo "negative control $mode: expected behavior assertion failed"
    exit 0
fi
if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-' "$out/run.log"; then exit 1; fi
