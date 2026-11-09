#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁手动：辅助脚本（CTest 构建 / 运行入口），由 android/CMakeLists.txt 的 android_direct_nvn_trace_init 调用，经 android/linux_ctest.sh 进安卓 Linux CTest
# Original fixtures only; real production failure paths, no GPU/loader/SDK required.
# 门禁只看：android/native/nvn/nvn_vk_first_draw.c android/native/nvn/nvn_vk_program_result.c android/tests/trace_init_test.c
# 门禁输入：android/native/ graphics/nvn/gfx/nvn/ graphics/nvn/gfx/tex/ graphics/nvn/third_party/vulkan-headers/ android/tests/vkhost/
# 门禁覆盖：android/native/nvn/nvn_vk_first_draw.c android/native/nvn/nvn_vk_program_result.c android/tests/trace_init_test.c
set -euo pipefail
W=$(cd "$(dirname "$0")/../.." && pwd)
E=${S2A_TRACE_TEST_BUILD:-"$W/build/trace_init_test"}; mkdir -p "$E"; E=$(cd "$E" && pwd)
mode=${1:---build-and-run}
case "$mode" in
    --build-and-run|--build-only) ;;
    --run-only) exec "$E/draw" "$E/program.json" ;;
    *) echo "Usage: $0 [--build-and-run|--build-only|--run-only]" >&2; exit 2 ;;
esac

mkdir -p "$E/include/android" "$E/include/sys"
cat > "$E/include/android/log.h" <<'FIXTURE'
#pragma once
#include <stdarg.h>
#include <stdio.h>
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
static inline int __android_log_vprint(int l,const char*t,const char*f,va_list a){(void)l;fprintf(stderr,"%s: ",t);int n=vfprintf(stderr,f,a);fputc(10,stderr);return n;}
static inline int __android_log_print(int l,const char*t,const char*f,...){va_list a;va_start(a,f);int n=__android_log_vprint(l,t,f,a);va_end(a);return n;}

FIXTURE
cat > "$E/include/android/native_window.h" <<'FIXTURE'
#pragma once
#include <stdint.h>
typedef struct ANativeWindow ANativeWindow;
static inline int32_t ANativeWindow_getWidth(ANativeWindow*w){(void)w;return 0;}
static inline int32_t ANativeWindow_getHeight(ANativeWindow*w){(void)w;return 0;}
static inline int32_t ANativeWindow_setBuffersGeometry(ANativeWindow*w,int32_t h,int32_t v,int32_t f){(void)w;(void)h;(void)v;(void)f;return -1;}

FIXTURE
cat > "$E/include/sys/system_properties.h" <<'FIXTURE'
#pragma once
#include <string.h>
#define PROP_VALUE_MAX 92
static inline int __system_property_get(const char*k,char*v){if((!strcmp(k,"debug.s2a.draw_probe") || !strcmp(k,"debug.s2a.query_wait") || !strcmp(k,"debug.s2a.draw_log"))){strcpy(v,"1");return 1;}if(!strcmp(k,"debug.s2a.batch_passes")){strcpy(v,"16");return 2;}v[0]=0;return 0;}

FIXTURE
# Original parser-only SPIR-V header: no GPU execution is attempted.
python3 - "$E" <<'PYJSON'
import sys,json,base64,pathlib,struct
p=pathlib.Path(sys.argv[1])
h=base64.b64encode(struct.pack('<5I',0x07230203,0x10000,0,1,0)).decode()
j={'schema':'s2a-direct-program-v1','content':{'stages':[{'stage':'vs','spirv':h,'attributeReadMask':0},{'stage':'fs','spirv':h}],'bindings':[]}}
(p/'program.json').write_text(json.dumps(j))
PYJSON
cd "$W"
sources=(android/tests/trace_init_test.c \
android/native/nvn/{mtl_vk_adapter,nvn_vk_program_result,nvn_vk_draw_diag,nvn_vk_pipe_manifest,nvn_vk_pipe_recipe,nvn_vk_driver_cache,nvn_vk_query,nvn_vk_session,nvn_vk_lifecycle,nvn_vk_exec_owner,nvn_vk_pipeline,nvn_vk_swapchain,nvn_vk_surface,nvn_vk_texcache,nvn_vk_sampler_cache,nvn_vk_sampler,nvn_vk_format,nvn_vk_mrt,nvn_vk_bcdec,nvn_vk_poolahb,poolahb_cover,poolahb_policy,poolahb_alias,nvn_vk_timeline,nvn_vk_threadbuf}.c \
android/native/direct/{vk_record,vk_clear,vk_device,gs_path,vk_membuf,vk_resid,program_cache,compile_request,compile_worker,compiler_bind,s2a_direct_abi,sha256,json}.c graphics/nvn/gfx/nvn/nvn_tex.c graphics/nvn/gfx/nvn/nvn_dirty.c graphics/nvn/gfx/tex/nvn_deswizzle.c "${FIRST_DRAW_SOURCE:-android/native/nvn/nvn_vk_first_draw.c}")
objects=()
for src in "${sources[@]}"; do
    obj="$E/$(basename "$src" .c).o"
    "${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -O0 -g -ftrivial-auto-var-init=pattern -ffunction-sections -fdata-sections -Wall -Wextra -Wno-missing-field-initializers -Wno-nonnull -I"$E/include" -Iandroid/tests/vkhost -Igraphics/nvn/third_party/vulkan-headers/include -Iandroid/native -Iandroid/native/direct -Iandroid/native/nvn -Iruntime/include -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/gfx/tex -c "$src" -o "$obj"
    objects+=("$obj")
done
# Trap every reachable Vulkan API except the three cleanup observers. Prototypes
# come from the pinned public header; no hand-written success stubs or GPU calls.
nm -u "${objects[@]}" > "$E/undefined.txt"
python3 - "$W" "$E" <<'PYSTUB'
import pathlib,re,sys
w,e=map(pathlib.Path,sys.argv[1:])
needed=set(re.findall(r'^\s*U\s+_?(vk\w+)\s*$',(e/'undefined.txt').read_text(),re.M))
needed-= {'vkUnmapMemory','vkDestroyBuffer','vkFreeMemory'}
# 时间戳录制路径的假实现由 trace_init_test.c 提供（#1350：首笔 draw 前记录）。
needed-= {'vkResetCommandBuffer','vkAllocateCommandBuffers','vkBeginCommandBuffer',
          'vkEndCommandBuffer','vkCmdResetQueryPool','vkCmdWriteTimestamp'}
h=(w/'graphics/nvn/third_party/vulkan-headers/include/vulkan/vulkan_core.h').read_text()
prototypes={m[1]:m[0] for m in re.finditer(r'VKAPI_ATTR\s+[^;{}]+?VKAPI_CALL\s+(vk\w+)\s*\([^;{}]*\);',h)}
missing=needed-prototypes.keys()
if missing: raise SystemExit(f'missing Vulkan trap prototypes: {sorted(missing)}')
s=['#include <vulkan/vulkan.h>','#include <stdio.h>','#include <stdlib.h>']
for name in sorted(needed):
    s.append(prototypes[name][:-1]+' { fputs("unexpected Vulkan API: '+name+'\\n", stderr); abort(); }')
(e/'vulkan_traps.c').write_text('\n'.join(s)+'\n')
PYSTUB
"${CC:-cc}" -Iandroid/tests/vkhost -Igraphics/nvn/third_party/vulkan-headers/include -c "$E/vulkan_traps.c" -o "$E/vulkan_traps.o"
"${CC:-cc}" "${objects[@]}" "$E/vulkan_traps.o" -Wl,--gc-sections -lm -pthread -o "$E/draw"

[[ "$mode" == --build-only ]] || "$E/draw" "$E/program.json"
