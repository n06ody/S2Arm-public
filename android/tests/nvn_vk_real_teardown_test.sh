#!/usr/bin/env bash
# Experimental independent real-Vulkan teardown probe; dependencies are mandatory.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
mode=${1:-}
if [ "$#" -gt 1 ] || { [ -n "$mode" ] && [ "$mode" != --neg ]; }; then
    echo "usage: $0 [--neg]" >&2; exit 64
fi
if [ "$(uname -s)" = Darwin ]; then
    exec bash "$repo/tests/vulkan/run.sh" android/tests/nvn_vk_real_teardown_test.sh "$@"
fi
find_icd() {
    if [ -n "${S2A_LAVAPIPE_ICD:-}" ]; then printf '%s\n' "$S2A_LAVAPIPE_ICD"; return; fi
    for f in "$HOME/.local/share/vulkan/icd.d/lvp_icd.json" "$HOME/.local/share/vulkan/icd.d/lvp_icd."*.json \
             /usr/share/vulkan/icd.d/lvp_icd.json /usr/share/vulkan/icd.d/lvp_icd.*.json; do
        [ -f "$f" ] && { printf '%s\n' "$f"; return; }
    done
}
find_layer() {
    if [ -n "${S2A_VK_LAYER_DIR:-}" ]; then printf '%s\n' "$S2A_VK_LAYER_DIR"; return; fi
    for d in "$HOME/.local/share/vulkan/explicit_layer.d" /usr/share/vulkan/explicit_layer.d; do
        [ -f "$d/VkLayer_khronos_validation.json" ] && { printf '%s\n' "$d"; return; }
    done
}
icd=$(find_icd || true); layer=$(find_layer || true)
[ -f "$icd" ] || { echo 'missing lavapipe ICD; set S2A_LAVAPIPE_ICD; NOT RUN' >&2; exit 2; }
[ -f "$layer/VkLayer_khronos_validation.json" ] || { echo 'missing Khronos validation layer; NOT RUN' >&2; exit 2; }
glslc=${GLSLC:-$(command -v glslc || true)}
[ -x "$glslc" ] || { echo 'missing glslc; NOT RUN' >&2; exit 2; }
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
inc=("$repo/android/tests/vkhost" "$repo/android/tests/hostvk" "$repo/android/native" "$repo/android/native/direct" "$repo/android/native/nvn" "$repo/runtime/include" "$repo/graphics/nvn/gfx/nvn" "$repo/graphics/nvn/gfx/nvn/include" "$repo/graphics/nvn/gfx/tex" "$repo/graphics/nvn/third_party/vulkan-headers/include")
args=(); for dir in "${inc[@]}"; do args+=(-I "$dir"); done
# nvn_vk_compute.c：292b8a24 给 session 收尾加了 nvn_vk_compute_pso_cache_release；
# 链进生产实现（空表即返回），未引用的管线构建面由 --gc-sections 剪掉。
# nvn_vk_display_timing.c：#489 B2 给 swapchain_destroy 加了 nvn_vk_dt_detach——
# presentID/时刻账随 swapchain 作古，同链生产实现（单文件自洽）。
sources=("$repo"/android/native/nvn/{nvn_vk_program_result,nvn_vk_draw_diag,nvn_vk_pipe_manifest,nvn_vk_pipe_recipe,nvn_vk_driver_cache,nvn_vk_query,nvn_vk_session,nvn_vk_lifecycle,nvn_vk_exec_owner,nvn_vk_pipeline,nvn_vk_swapchain,nvn_vk_surface,nvn_vk_texcache,nvn_vk_sampler_cache,nvn_vk_sampler,nvn_vk_format,nvn_vk_mrt,nvn_vk_readback,nvn_vk_readback_gpu,nvn_vk_bcdec,nvn_vk_poolahb,poolahb_cover,poolahb_policy,poolahb_alias,nvn_vk_timeline,nvn_vk_threadbuf,nvn_vk_compute,nvn_vk_display_timing}.c "$repo"/android/native/direct/{vk_record,vk_clear,vk_device,gs_path,vk_membuf,vk_resid,program_cache,compile_request,compile_worker,compiler_bind,s2a_direct_abi,sha256,json}.c "$repo/graphics/nvn/gfx/nvn/nvn_tex.c" "$repo/graphics/nvn/gfx/nvn/nvn_dirty.c" "$repo/graphics/nvn/gfx/tex/nvn_deswizzle.c")
compile() {
  cc -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-missing-field-initializers -ffunction-sections -fdata-sections "${args[@]}" "$@" "${sources[@]}" -Wl,--gc-sections -l:libvulkan.so.1 -lpthread -lm -ldl
}
cat >"$out/vertex.vert" <<'SHADER'
#version 450
void main() { gl_Position=vec4(0.0,0.0,0.0,1.0); }
SHADER
cat >"$out/fragment.frag" <<'SHADER'
#version 450
layout(location=0) out vec4 color; void main() { color=vec4(1.0); }
SHADER
"$glslc" --target-env=vulkan1.1 "$out/vertex.vert" -o "$out/vertex.spv"
"$glslc" --target-env=vulkan1.1 "$out/fragment.frag" -o "$out/fragment.spv"
python3 - "$out" <<'PYJSON'
import base64,json,pathlib,sys
p=pathlib.Path(sys.argv[1]); enc=lambda x:base64.b64encode((p/x).read_bytes()).decode()
program={'schema':'s2a-direct-program-v1','content':{'stages':[{'stage':'vs','spirv':enc('vertex.spv'),'attributeReadMask':0},{'stage':'fs','spirv':enc('fragment.spv')}],'bindings':[]}}
(p/'program.json').write_text(json.dumps(program))
PYJSON
export VK_DRIVER_FILES="$icd" VK_LAYER_PATH="$layer" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1 XDG_CACHE_HOME="$out"
if [ "$mode" != --neg ]; then
    mkdir "$out/cache"
    compile "$repo/android/tests/nvn_vk_real_teardown_test.c" -o "$out/probe"
    "$out/probe" "$out/program.json" "$out/cache" >"$out/normal.log" 2>&1 || { cat "$out/normal.log" >&2; exit 1; }
    if grep -qiE 'validation error|VUID-|UNASSIGNED-|SYNC-HAZARD|ERROR.*VALIDATION' "$out/normal.log"; then cat "$out/normal.log" >&2; exit 1; fi
    cat "$out/normal.log"
    exit 0
fi
# Mutation is generated only under $out. Remove the #1472 production destroy
# calls, never modify the source tree; prove the validation observer sees leaks.
python3 - "$repo/android/native/nvn/nvn_vk_first_draw.c" "$out/mutant.c" <<'PYMUT'
import pathlib,sys
s=pathlib.Path(sys.argv[1]).read_text()
def mutate(start,end,names):
    global s
    a=s.index(start); b=s.index(end,a); part=s[a:b]
    for name in names:
        token=name+'('
        if token not in part: raise SystemExit('missing mutation target '+name)
        part=part.replace(token,'if (0) '+token)
    s=s[:a]+part+s[b:]
mutate('static void fd_prog_obj_destroy(', 'static struct fd_prog_obj *fd_prog_obj_find(', ['vkDestroyShaderModule','vkDestroyDescriptorSetLayout','vkDestroyPipelineLayout'])
mutate('void nvn_vk_first_draw_release_device(', 'static int64_t fd_now_ns(', ['vkDestroyPipeline','vkDestroyPipelineCache','vkDestroyDescriptorPool','vkDestroyBuffer'])
old='if (c->mem) fd_devmem_release_dev(d->device, c->mem);'
if s.count(old) != 1: raise SystemExit('missing unique arena release target')
s=s.replace(old, 'if (0 && c->mem) fd_devmem_release_dev(d->device, c->mem);')
pathlib.Path(sys.argv[2]).write_text(s)
PYMUT
compile -DTEARDOWN_NEGATIVE_CONTROL "-DREAL_FIRST_DRAW=\"$out/mutant.c\"" "$repo/android/tests/nvn_vk_real_teardown_test.c" -o "$out/mutant"
mkdir "$out/mutant-cache"
"$out/mutant" "$out/program.json" "$out/mutant-cache" >"$out/mutant.log" 2>&1 || { cat "$out/mutant.log" >&2; exit 1; }
# Do not emit expected VUID lines: the enclosing Vulkan harness rejects all VUID
# text. Reject every unrelated validation error locally before reporting counts.
count=$(grep -c 'Validation Error.*VUID-vkDestroyDevice-device-05137' "$out/mutant.log" || true)
unexpected=$(grep -iE 'validation error|VUID-|UNASSIGNED-|SYNC-HAZARD|ERROR.*VALIDATION' "$out/mutant.log" | grep -vc 'VUID-vkDestroyDevice-device-05137' || true)
# One pipeline, two shaders, four set layouts, one pipeline layout, two pools,
# one buffer, one allocation and one pipeline cache: thirteen intentionally leaked children.
[ "$count" -eq 13 ] && [ "$unexpected" -eq 0 ] || { cat "$out/mutant.log" >&2; echo 'negative control failed' >&2; exit 1; }
echo "negative control: expected device-child leak errors=$count, unexpected=$unexpected"
