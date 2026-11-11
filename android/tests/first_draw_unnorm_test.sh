#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# Host-only production first_draw + synthetic resources. The shaders do not sample.
# Requires Vulkan headers, loader and a software ICD; GLSLC and VULKAN_INCLUDE may be set.
set -euo pipefail
W=$(cd "$(dirname "$0")/../.." && pwd)
cd "$W"
: "${GLSLC:=glslc}"
: "${VULKAN_INCLUDE:=$W/graphics/nvn/third_party/vulkan-headers/include}"
[[ -f "$VULKAN_INCLUDE/vulkan/vulkan_core.h" ]] || { echo "missing Vulkan headers: $VULKAN_INCLUDE" >&2; exit 2; }
command -v "$GLSLC" >/dev/null || { echo "missing glslc: $GLSLC" >&2; exit 2; }
E=$(mktemp -d "${TMPDIR:-/tmp}/s2a-unnorm.XXXXXX")
trap 'rm -r "$E"' EXIT
mkdir -p "$E/include/android" "$E/include/sys" "$E/obj"
cat > "$E/include/android/log.h" <<'HEADER'
#pragma once
#include <stdarg.h>
#include <stdio.h>
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
static inline int __android_log_vprint(int l,const char*t,const char*f,va_list a){(void)l;fprintf(stderr,"%s: ",t);int n=vfprintf(stderr,f,a);fputc(10,stderr);return n;}
static inline int __android_log_print(int l,const char*t,const char*f,...){va_list a;va_start(a,f);int n=__android_log_vprint(l,t,f,a);va_end(a);return n;}
HEADER
cat > "$E/include/android/native_window.h" <<'HEADER'
#pragma once
#include <stdint.h>
typedef struct ANativeWindow ANativeWindow;
static inline int32_t ANativeWindow_getWidth(ANativeWindow*w){(void)w;return 0;}
static inline int32_t ANativeWindow_getHeight(ANativeWindow*w){(void)w;return 0;}
static inline int32_t ANativeWindow_setBuffersGeometry(ANativeWindow*w,int32_t wd,int32_t h,int32_t f){(void)w;(void)wd;(void)h;(void)f;return 0;}
HEADER
cat > "$E/include/sys/system_properties.h" <<'HEADER'
#pragma once
#define PROP_VALUE_MAX 92
static inline int __system_property_get(const char*k,char*v){(void)k;v[0]=0;return 0;}
HEADER
"$GLSLC" android/tests/first_draw_unnorm.vert -o "$E/vert.spv"
"$GLSLC" android/tests/first_draw_unnorm.frag -o "$E/frag.spv"
sources=(android/tests/first_draw_unnorm_test.c
android/native/nvn/{mtl_vk_adapter,nvn_vk_program_result,nvn_vk_draw_diag,nvn_vk_pipe_manifest,nvn_vk_pipe_recipe,nvn_vk_driver_cache,nvn_vk_query,nvn_vk_session,nvn_vk_lifecycle,nvn_vk_exec_owner,nvn_vk_pipeline,nvn_vk_swapchain,nvn_vk_surface,nvn_vk_texcache,nvn_vk_sampler_cache,nvn_vk_sampler,nvn_vk_format,nvn_vk_mrt,nvn_vk_bcdec}.c "${FIRST_DRAW_SOURCE:-android/native/nvn/nvn_vk_first_draw.c}"
android/native/direct/{vk_record,vk_clear,vk_device,gs_path,vk_membuf,vk_resid,program_cache,compile_request,compile_worker,compiler_bind,s2a_direct_abi,sha256,jv,json}.c
graphics/nvn/gfx/nvn/nvn_tex.c graphics/nvn/gfx/nvn/nvn_dirty.c graphics/nvn/gfx/tex/nvn_deswizzle.c)
objects=()
for src in "${sources[@]}"; do
  obj="$E/obj/$(basename "$src" .c).o"
  "${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 -O0 -ffunction-sections -fdata-sections \
    -I"$E/include" -Iandroid/tests/vkhost -I"$VULKAN_INCLUDE" -Iandroid/native \
    -Iandroid/native/direct -Iandroid/native/nvn -Iruntime/include \
    -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/gfx/tex -c "$src" -o "$obj"
  objects+=("$obj")
done
"${CC:-cc}" "${objects[@]}" -Wl,--gc-sections \
  -Wl,--wrap=s2a_vk_adapter_tex -Wl,--wrap=s2a_vk_adapter_attachment_view \
  -Wl,--wrap=s2a_vk_adapter_tex_by_id -Wl,--wrap=s2a_vk_adapter_samp_by_id \
  -Wl,--wrap=s2a_vk_adapter_tex_view_by_id -Wl,--wrap=s2a_vk_adapter_tex_view \
  -Wl,--wrap=vkMapMemory -Wl,--wrap=vkAllocateMemory -Wl,--wrap=vkBindBufferMemory \
  -Wl,--wrap=vkUpdateDescriptorSets -Wl,--wrap=vkCmdDraw \
  -Wl,--wrap=vkDestroyImage -Wl,--wrap=vkDestroyImageView -Wl,--wrap=vkFreeMemory -Wl,-l:libvulkan.so.1 -lm -pthread -o "$E/draw"
run() { if [[ -n ${VULKAN_RUNNER:-} ]]; then "$VULKAN_RUNNER" "$E/draw" "$E/program.json"; else "$E/draw" "$E/program.json"; fi; }
make_program() {
  python3 - "$E" "$1" "$2" <<'PY'
import base64,json,pathlib,sys
p=pathlib.Path(sys.argv[1]); b0,b1=map(int,sys.argv[2:]); encode=lambda f:base64.b64encode((p/f).read_bytes()).decode()
content={'stages':[{'stage':'vs','spirv':encode('vert.spv'),'attributeReadMask':0},{'stage':'fs','spirv':encode('frag.spv')}], 'bindings':[{'stage':'program','kind':'support','set':0,'binding':0},{'stage':'fs','kind':'texture','set':1,'binding':b0,'slot':8},{'stage':'fs','kind':'texture','set':1,'binding':b1,'slot':10}]}
(p/'program.json').write_text(json.dumps({'schema':'s2a-direct-program-v1','content':content}))
PY
}
# Each row is binding0 binding1 lane, and manually calculated draw1/draw2 masks.
while read -r b0 b1 lane m0 m1; do
  make_program "$b0" "$b1"
  echo "mapping: bindings=$b0,$b1 lane=$lane expected=$m0,$m1"
  BIND=1 LANE="$lane" M0="$m0" M1="$m1" run
done <<'CASES'
0 1 0 1 2
30 31 0 1073741824 2147483648
32 33 0 0 0
126 127 0 0 0
128 129 1 1 2
158 159 1 1073741824 2147483648
160 161 1 0 0
254 255 1 0 0
256 257 2 1 2
286 287 2 1073741824 2147483648
288 289 2 0 0
382 383 2 0 0
384 385 2 0 0
CASES
make_program 128 129
BIND=1 BUFFER=1 LANE=1 M0=0 M1=0 run
run
PARTIAL=1 run
