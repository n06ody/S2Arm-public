#!/usr/bin/env bash
# D8 第一段验收：nvn_gpu_* Vulkan 后端（缓冲 / 提交 / 读回 / 失败 / 保活 / close）在 CPU 参考实现（Mesa lavapipe）
# 上跑契约用例 C01–C08 + C23 粘滞错误，并开 Khronos validation 层。
#
# 用法：
#   bash android/tests/vk/run.sh              # 默认：lavapipe + validation
#   bash android/tests/vk/run.sh --tsan       # 追加 TSan（C04 发布顺序）
#   bash android/tests/vk/run.sh --asan       # 追加 ASan/UBSan
# 环境：
#   S2A_LAVAPIPE_ICD   指定 lavapipe ICD json（默认找 ~/.local / /usr/share/vulkan/icd.d）
#   S2A_VK_LAYER_DIR   指定 validation 层目录（默认找 ~/.local / /usr/share/vulkan/explicit_layer.d）
# 没装 lavapipe / validation 时不静默跳过（#784）：macOS 一律、Linux 宿主缺原生工具时进共享离线容器（tests/vulkan/），
# 容器也没准备好才退出码 2（红）。
# 默认档先在宿主跑真编译器链（android/tests/gs_compiler_chain_test.py → build/696-real-compiler）再跑后端用例。链要 dotnet
# （.NET 10 SDK，在 PATH 上）、固定的 NDK 29.0.14206865（设 ANDROID_NDK_HOME，或放在 $ANDROID_HOME/ndk/、~/Android/Sdk/ndk/ 下）、
# 首次联网还原 NuGet，所以只能在宿主上出（macOS 每次重出，Linux 缺 program.json 才出）；离线容器里跑不了。
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../.." && pwd)
mode=${1:-}
if { [ "$mode" = "--tbuf-compiler-core" ] || [ -z "$mode" ]; } && [ -z "${S2A_VULKAN_IMAGE_ID:-}" ]; then
    python3 "$repo/android/tests/tbuf_compiler_chain_test.py"
fi
if { [ "$mode" = "--shown-only" ] || [ -z "$mode" ] || [[ "$mode" == --tbuf-* ]]; } && [ -z "${S2A_VULKAN_IMAGE_ID:-}" ]; then
    . "$repo/android/tools/ndk_toolchain.sh"
    ndk=$(s2a_ndk_resolve) || { echo 'NDK 29 missing' >&2; exit 2; }
    grep -q '^Pkg.Revision = 29\.0\.14206865$' "$ndk/source.properties" || { echo 'NDK 29.0.14206865 required' >&2; exit 2; }
    tag=$(s2a_ndk_prebuilt_tag "$ndk") || exit 2
    header_dir="$ndk/toolchains/llvm/prebuilt/$tag/sysroot/usr/include/vulkan"
    [ -f "$header_dir/vulkan_core.h" ] || { echo 'NDK 29 Vulkan headers missing' >&2; exit 2; }
    mkdir -p "$repo/build/c883-ndk"
    cp -R "$header_dir" "$repo/build/c883-ndk/"
    cp -R "$(dirname "$header_dir")/vk_video" "$repo/build/c883-ndk/"
fi
# Isolated production replay indirect test. Exit before every legacy test path.
if [ "$mode" = "--indirect-core" ]; then
    out="$repo/build/vk-indirect-core"; mkdir -p "$out/shaders"
    glslc_bin=${GLSLC:-glslc}
    : "${S2A_LAVAPIPE_ICD:?set S2A_LAVAPIPE_ICD for isolated replay test}"
    : "${S2A_VK_LAYER_DIR:?set S2A_VK_LAYER_DIR for isolated replay test}"
    for shader in quad.vert quad.frag count_args.comp replay_drv0.vert replay_drv0.frag replay_drv0_args.comp; do
        "$glslc_bin" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
    done
    inc=(); for d in android/tests/vkhost graphics/nvn/third_party/vulkan-headers/include graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
    src=("$repo/android/tests/vk/t_replay_indirect.c")
    for f in graphics/nvn/replay/rp_draw.c graphics/nvn/replay/rp_buf.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c android/native/nvn/backend_vulkan.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
    cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" -Wl,--gc-sections -Wl,--wrap=nvn_gpu_draw -lvulkan -lpthread -lm -o "$out/replay-indirect"
    export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
    exec "$out/replay-indirect" "$out/shaders"
fi
if [ "$(uname -s)" = Darwin ]; then
    export S2A_VULKAN_CACHE="$repo/build/vulkan-cache"
    if [ -z "$mode" ]; then python3 "$repo/android/tests/gs_compiler_chain_test.py"; fi
    exec bash "$repo/tests/vulkan/run.sh" android/tests/vk/run.sh "$@"
fi
# 以下是 Linux：在宿主上（原生跑；缺原生工具时转容器，见下），或已在 tests/vulkan/run.sh 起的离线容器里（带 S2A_VULKAN_IMAGE_ID）。
in_container=${S2A_VULKAN_IMAGE_ID:+1}
if [ -z "$mode" ] && [ ! -f "$repo/build/696-real-compiler/compiled/program.json" ]; then
    if [ -n "$in_container" ]; then
        echo "【红】缺 build/696-real-compiler/compiled/program.json：真编译器链要 dotnet、NDK 29、首次联网还原 NuGet，离线容器里跑不了。" >&2
        echo "     先在宿主上出链：python3 android/tests/gs_compiler_chain_test.py；或直接运行 bash android/tests/vk/run.sh（宿主出链、再进容器）。" >&2
        exit 2
    fi
    if ! command -v dotnet >/dev/null 2>&1; then
        echo "【红】找不到 dotnet：真编译器链要 .NET 10 SDK（版本见 compat/tree/global.json），请装好并放到 PATH 上。" >&2
        exit 2
    fi
    if ! PYTHONPATH="$repo/android/core-link" python3 -c 'import sys, link_symbols; sys.exit(0 if link_symbols.find_ndk() else 1)'; then
        echo "【红】找不到 NDK 29.0.14206865：真编译器链要用它的 Vulkan 头文件，不联网下载、不跳过，其他版本的 NDK 不认。" >&2
        echo "     装好该版本后设 ANDROID_NDK_HOME=<NDK 目录>，或放到 \$ANDROID_HOME/ndk/29.0.14206865、~/Android/Sdk/ndk/29.0.14206865。" >&2
        exit 2
    fi
    python3 "$repo/android/tests/gs_compiler_chain_test.py"
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

icd=$(find_icd || true)
layer=$(find_layer || true)
# 宿主缺原生 Vulkan 工具（lavapipe / validation / glslc 缺任一）：同 macOS，链已在上面出好，进共享的离线容器跑。
# 容器没准备好（tests/vulkan/run.sh 退 75）就保持下面原来的红，只多提示一句；容器里跑出的结果（含红）原样退出。
if [ -z "$in_container" ] && { [ -z "$icd" ] || [ -z "$layer" ] || [ -z "${GLSLC:-$(command -v glslc || true)}" ]; }; then
    echo "本机缺原生 Vulkan 工具（lavapipe / validation / glslc），改走共享离线容器（同 macOS）" >&2
    export S2A_VULKAN_CACHE="$repo/build/vulkan-cache"
    rc=0
    bash "$repo/tests/vulkan/run.sh" android/tests/vk/run.sh "$@" || rc=$?
    [ "$rc" = 75 ] || exit "$rc"
    echo "     容器环境也没准备好（见上）：装好原生工具，或先显式运行 bash tests/vulkan/prepare.sh" >&2
fi
if [ -z "$icd" ]; then
    echo "【红】找不到 lavapipe ICD：本用例要求 CPU 参考实现（Mesa lavapipe），不静默跳过。" >&2
    echo "     Arch: 解包 vulkan-swrast 后设 S2A_LAVAPIPE_ICD=<dir>/lvp_icd.json" >&2
    exit 2
fi
if [ -z "$layer" ]; then
    echo "【红】找不到 VK_LAYER_KHRONOS_validation：验收要求 0 条 validation 报错。" >&2
    echo "     Arch: 解包 vulkan-validation-layers 后设 S2A_VK_LAYER_DIR=<dir>" >&2
    exit 2
fi

glslc=${GLSLC:-$(command -v glslc || true)}
if [ -z "$glslc" ]; then
    echo "【红】找不到 glslc：纹理三角形用例要现编测试着色器（glslang / shaderc）。" >&2
    exit 2
fi

mkdir -p "$repo/build/vk-tests"
export TMPDIR="$repo/build/vk-tests"
out=$(mktemp -d "$TMPDIR/s2a-nvn-vk.XXXXXX")
trap 'rm -rf "$out"' EXIT
mkdir -p "$out/shaders"
for s in count_args.comp logic_float.frag logic_uint.frag logic_float_mrt.frag logic_uint_mrt.frag gs_probe.vert gs_probe.geom gs_probe.frag tri.vert tri.frag tri_combined.frag tri3.vert tri_solid.frag tri_invert.frag solid_green.frag solid_blue.frag out1_green.frag tex_u.frag quad.vert quad.frag inst.vert point.vert quad2.frag arr.frag cube.frag cubearr.frag tex3d.frag comp.comp pushc.frag pushc.comp tbuf.comp itbuf.comp itbuf_unknown.comp itbuf_unknown_read.comp img.comp tsamp.comp arr2.frag separate.vert separate.frag; do
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$s" -o "$out/shaders/$s.spv"
done
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/tbuf_replay.frag" -o "$out/shaders/tbuf_replay.frag.spv"

inc=(-I "$repo/android/tests/vkhost"
     -I "$repo/graphics/nvn/third_party/vulkan-headers/include"
     -I "$repo/graphics/nvn"
     -I "$repo/graphics/nvn/replay"
     -I "$repo/graphics/nvn/gfx/nvn"
     -I "$repo/graphics/nvn/gfx/nvn/include"
     -I "$repo/android/native"
     -I "$repo/android/native/nvn"
     -I "$repo/android/native/direct"
     -I "$repo/runtime/include")
if [ "$mode" = "--shown-only" ] || [[ "$mode" == --tbuf-* ]]; then
    [ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 Vulkan headers not staged' >&2; exit 2; }
    inc=(-I "$repo/build/c883-ndk" "${inc[@]}")
fi
src=("$repo/android/tests/vk/t_backend_vk.c"
     "$repo/android/native/nvn/backend_vulkan.c"
     "$repo/android/native/direct/vk_device.c"
     "$repo/android/native/direct/gs_path.c"
     "$repo/android/native/capprobe/capprobe_host.c"
     "$repo/android/native/nvn/nvn_vk_format.c"
     "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c"
     "$repo/android/native/nvn/nvn_vk_bcdec.c"
     "$repo/android/native/nvn/nvn_vk_driver_cache.c"
     "$repo/android/native/direct/sha256.c")
bc_src=("$repo/android/tests/vk/t_bc_decode.c"
     "$repo/android/native/nvn/nvn_vk_bcdec.c"
     "$repo/android/native/direct/vk_device.c"
     "$repo/android/native/direct/gs_path.c"
     "$repo/graphics/nvn/gfx/tex/nvn_bcdec.c")
# 与发行 APK 同一组建设备规则（android/CMakeLists.txt 的 nvn_vk 后端定义此宏）。
cflags=(-D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -DS2A_ANDROID_VK_BACKEND_NVN_VK=1)
case "$mode" in
    --tsan) cflags+=(-fsanitize=thread) ;;
    --asan) cflags+=(-fsanitize=address,undefined -fno-omit-frame-pointer) ;;
    --shown-only) cflags+=(-DS2A_C883_TEST=1) ;;
    ""|--separate-core|--separate-neg-sampler|--tbuf-core|--tbuf-compiler-core|--tbuf-neg-resolve|--tbuf-neg-format|--tbuf-neg-elements|--gs-only|--gs-compiler|--gs-core|--gs-no-cap|--gs-neg-stage|--gs-neg-flags|--gs-neg-cap) ;;
    *) echo "未知参数：${mode}（可用 --tsan / --asan）" >&2; exit 64 ;;
esac

echo "lavapipe: $icd"
echo "validation: $layer"
if [ "$mode" = "--gs-core" ]; then
    src=("$repo/android/tests/vk/t_replay_gs.c" "$repo/graphics/nvn/replay/rp_draw.c" "$repo/graphics/nvn/replay/rp_state.c" "$repo/graphics/nvn/replay/nvn_unimpl.c" "$repo/android/native/gfx_plat/nvk_shader.c"
         "$repo/android/native/nvn/backend_vulkan.c" "$repo/android/native/direct/vk_device.c" "$repo/android/native/direct/gs_path.c" "$repo/android/native/capprobe/capprobe_host.c"
         "$repo/android/native/nvn/nvn_vk_format.c" "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c" "$repo/android/native/nvn/nvn_vk_bcdec.c"
         "$repo/android/native/nvn/nvn_vk_program_result.c" "$repo/android/native/nvn/nvn_vk_compute_request.c"
         "$repo/android/native/nvn/nvn_vk_driver_cache.c"
         "$repo/android/native/direct/source_store.c" "$repo/android/native/direct/control.c" "$repo/android/native/direct/input_identity.c" "$repo/android/native/direct/texture_facts.c"
         "$repo/android/native/direct/sha256.c" "$repo/android/native/direct/json.c" "$repo/android/native/direct/jv.c" "$repo/android/native/direct/compile_request.c"
         "$repo/android/native/direct/offline_library.c" "$repo/android/native/direct/program_cache.c" "$repo/runtime/src/direct_host.c")
    cflags+=(-ffunction-sections -fdata-sections -Wno-unused-function)
fi
if [ "$mode" = "--separate-core" ] || [ "$mode" = "--separate-neg-sampler" ]; then
    src=("$repo/android/tests/vk/t_replay_separate.c" "$repo/graphics/nvn/replay/rp_draw.c" "$repo/graphics/nvn/replay/rp_state.c" "$repo/graphics/nvn/replay/nvn_unimpl.c"
         "$repo/android/native/nvn/backend_vulkan.c" "$repo/android/native/direct/vk_device.c" "$repo/android/native/direct/gs_path.c" "$repo/android/native/capprobe/capprobe_host.c"
         "$repo/android/native/nvn/nvn_vk_format.c" "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c" "$repo/android/native/nvn/nvn_vk_bcdec.c"
         "$repo/android/native/nvn/nvn_vk_driver_cache.c" "$repo/android/native/direct/sha256.c")   # 后端的管线缓存落盘（#856）
    cflags+=(-ffunction-sections -fdata-sections -Wno-unused-function)
fi
if [[ "$mode" == --tbuf-* ]]; then
    src=("$repo/android/tests/vk/t_replay_tbuf.c" "$repo/graphics/nvn/replay/rp_draw.c" "$repo/graphics/nvn/replay/rp_buf.c" "$repo/graphics/nvn/replay/rp_state.c" "$repo/graphics/nvn/replay/nvn_unimpl.c"
         "$repo/android/native/nvn/backend_vulkan.c" "$repo/android/native/direct/vk_device.c" "$repo/android/native/direct/gs_path.c" "$repo/android/native/capprobe/capprobe_host.c"
         "$repo/android/native/nvn/nvn_vk_format.c" "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c" "$repo/android/native/nvn/nvn_vk_bcdec.c"
         "$repo/android/native/nvn/nvn_vk_driver_cache.c" "$repo/android/native/direct/sha256.c")
    cflags+=(-ffunction-sections -fdata-sections -Wno-unused-function)
fi
if [ "$mode" = "--tbuf-compiler-core" ]; then
    src=("$repo/android/tests/vk/t_replay_tbuf_compiler.c" "$repo/graphics/nvn/replay/rp_draw.c" "$repo/graphics/nvn/replay/rp_buf.c" "$repo/graphics/nvn/replay/rp_state.c" "$repo/graphics/nvn/replay/nvn_unimpl.c" "$repo/android/native/gfx_plat/nvk_shader.c"
         "$repo/android/native/nvn/backend_vulkan.c" "$repo/android/native/direct/vk_device.c" "$repo/android/native/direct/gs_path.c" "$repo/android/native/capprobe/capprobe_host.c"
         "$repo/android/native/nvn/nvn_vk_format.c" "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c" "$repo/android/native/nvn/nvn_vk_bcdec.c"
         "$repo/android/native/nvn/nvn_vk_program_result.c" "$repo/android/native/nvn/nvn_vk_compute_request.c" "$repo/android/native/nvn/nvn_vk_driver_cache.c"
         "$repo/android/native/direct/source_store.c" "$repo/android/native/direct/control.c" "$repo/android/native/direct/input_identity.c" "$repo/android/native/direct/texture_facts.c"
         "$repo/android/native/direct/sha256.c" "$repo/android/native/direct/json.c" "$repo/android/native/direct/jv.c" "$repo/android/native/direct/compile_request.c"
         "$repo/android/native/direct/offline_library.c" "$repo/android/native/direct/program_cache.c" "$repo/runtime/src/direct_host.c")
fi
if [[ "$mode" == --tbuf-neg-* ]]; then
    python3 - "$mode" "${src[1]}" "$out/tbuf_mutant.c" <<'PYTBUF'
import pathlib, sys
mode, source, target = sys.argv[1:]
s = pathlib.Path(source).read_text()
changes = {
    '--tbuf-neg-resolve': ('if (!rp_buf_resolve(c, (uint64_t)(uintptr_t)t.mem, bytes, &buf, &off)) return skip("tbuf_buffer_unresolved", rec);', 'return skip("tbuf_buffer_unresolved", rec);'),
    '--tbuf-neg-format': ('uint32_t fmt = hv && v.set_format ? v.format : (uint32_t)t.d.format;', 'uint32_t fmt = NVN_FORMAT_RGBA8UI;'),
    '--tbuf-neg-elements': ('uint64_t bytes = (uint64_t)t.d.width * base.bytes;', 'uint64_t bytes = (uint64_t)t.d.width * base.bytes * 2;'),
}
a, b = changes[mode]
if s.count(a) != 1: raise SystemExit('TBUF negative control anchor changed: ' + mode)
pathlib.Path(target).write_text(s.replace(a, b))
PYTBUF
    src[1]="$out/tbuf_mutant.c"
fi
if [ "$mode" = "--separate-neg-sampler" ]; then
    python3 - "$repo/graphics/nvn/replay/rp_draw.c" "$out/rp_draw_mutant.c" <<'PYMUT'
import pathlib, sys
s = pathlib.Path(sys.argv[1]).read_text()
a = 'ts[st][i] = rp_samp_by_id((int)(sh >> 20));'
b = 'ts[st][i] = rp_samp_by_id((int)(h >> 20)); (void)sh;'
if s.count(a) != 1: raise SystemExit('sampler negative control anchor changed')
pathlib.Path(sys.argv[2]).write_text(s.replace(a, b))
PYMUT
    src[1]="$out/rp_draw_mutant.c"
fi
# 负控只修改临时源副本，不关闭 validation，也不改设备特性。
case "$mode" in
    --gs-neg-stage|--gs-neg-flags|--gs-neg-cap)
        python3 - "$mode" "${src[1]}" "$out/backend_mutant.c" <<'PYMUT'
import pathlib, sys
mode, source, target = sys.argv[1:]
s = pathlib.Path(source).read_text()
changes = {
    '--gs-neg-stage': ('if (!d->st[st].sh) continue;', 'if (!d->st[st].sh || st == NVN_GPU_SH_GS) continue;'),
    '--gs-neg-flags': ('if (m[i].stages & (1u << st)) b[nb].stageFlags |= vk_shader_stage((nvn_gpu_stage)st);', 'if (st != NVN_GPU_SH_GS && (m[i].stages & (1u << st))) b[nb].stageFlags |= vk_shader_stage((nvn_gpu_stage)st);'),
    '--gs-neg-cap': ('if (s->stage == NVN_GPU_SH_GS && !(g.caps.caps & NVN_GPU_CAP_GEOMETRY))', 'if (0 && s->stage == NVN_GPU_SH_GS && !(g.caps.caps & NVN_GPU_CAP_GEOMETRY))'),
}
a, b = changes[mode]
if s.count(a) != 1: raise SystemExit('mutation anchor changed: ' + mode)
pathlib.Path(target).write_text(s.replace(a, b))
PYMUT
        src[1]="$out/backend_mutant.c" ;;
esac
# 管线缓存四个入口的 --wrap 只给默认档：--gs-core 档的测试源（t_replay_gs.c）没有对应的 __wrap 定义，带上会链接失败。
pc_wrap=(-Wl,--wrap=vkCreatePipelineCache -Wl,--wrap=vkGetPipelineCacheData
         -Wl,--wrap=vkCreateGraphicsPipelines -Wl,--wrap=vkCreateComputePipelines)
# --gs-core / --separate-* 档的测试源没有这四个 __wrap 定义（只有默认档的 t_backend_vk.c 有），带上会链接失败
case "$mode" in --gs-core|--separate-core|--separate-neg-sampler|--tbuf-*) pc_wrap=() ;; esac
if [ "$mode" = "--shown-only" ]; then pc_wrap+=(-Wl,--wrap=vkGetDeviceProcAddr); fi
cc "${cflags[@]}" "${inc[@]}" "${src[@]}" -Wl,--wrap=vkAllocateDescriptorSets -Wl,--wrap=vkGetPhysicalDeviceFormatProperties \
    ${pc_wrap[@]+"${pc_wrap[@]}"} -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/t_backend_vk"

export VK_DRIVER_FILES=$icd
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
export TSAN_OPTIONS="halt_on_error=0 suppressions=$repo/android/tests/vk/tsan.supp ${TSAN_OPTIONS:-}"
# TSan 档不开 validation 层：层内部用 rwlock + 延迟释放，在 TSan 下会报层自身的竞争，
# 淹没被测代码的报告。validation 由默认档（不带 TSan）单独跑。
if [ "$mode" != "--tsan" ]; then
    export VK_LAYER_PATH=$layer
    export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
    export VK_LAYER_VALIDATE_SYNC=1   # 同步校验：缺屏障会报 SYNC-HAZARD-*
fi
# 机型能力模拟（可选）：S2A_VK_PROFILE_LAYER_DIR 指 VK_LAYER_KHRONOS_profiles 所在目录时，把它排在 validation
# 之下（离驱动更近），让 lavapipe 按 VK_KHRONOS_PROFILES_* 指定的机型能力档上报；validation 看到的是模拟后的能力。
# 成套的模拟机型档（无 BC / 无 D24 / 无顶点存储 / 无双源混合 / 无校准时间戳 / 负对照）见 sim_tiers.sh（#429）。
# 设了 VK_LAYER_PATH 时加载器不认 VK_ADD_LAYER_PATH / VK_LOADER_LAYERS_ENABLE，只能在这里拼。
if [ -n "${S2A_VK_PROFILE_LAYER_DIR:-}" ]; then
    export VK_LAYER_PATH=${VK_LAYER_PATH:+$VK_LAYER_PATH:}$S2A_VK_PROFILE_LAYER_DIR
    export VK_INSTANCE_LAYERS=${VK_INSTANCE_LAYERS:+$VK_INSTANCE_LAYERS:}VK_LAYER_KHRONOS_profiles
fi

# 真实探测：独立进程执行控制、GS 单独和两个 GS+遮挡臂；只有四臂全 ok
# 才生成供生产 gs_path 读取的结果。此判定是生产 verdict 的严格子集。
cc -std=c11 -D_DEFAULT_SOURCE -DS2A_CAPPROBE_STANDALONE "${inc[@]}" \
    "$repo/android/native/capprobe/capprobe.c" -lvulkan -o "$out/capprobe"
timeout 90 "$out/capprobe" all --badlist "$repo/android/native/capprobe/known-bad.txt" > "$out/probe.jsonl"
python3 - "$out/probe.jsonl" "$out/probe-result.json" <<'PYPROBE'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1]) if line.strip().startswith('{')]
fp = [r for r in rows if r.get('type') == 'fingerprint' and 'vendorID' in r and 'driverVersion' in r]
arms = {r.get('arm'): r.get('outcome') for r in rows if r.get('type') == 'arm'}
required = ('nogsocc', 'noq', 'occ', 'occimp')
if len(fp) != 1 or any(arms.get(a) != 'ok' for a in required) or any(r.get('type') == 'blocked' for r in rows):
    raise SystemExit('真实 GS 探测未通过: ' + repr(rows))
print('真实 GS 探测：' + ', '.join(a + '=' + arms[a] for a in required))
with open(sys.argv[2], 'w') as f:
    json.dump({'source': 'probe', 'gs_occlusion': 'supported'}, f, separators=(',', ':'))
PYPROBE
export S2A_CAPPROBE_RESULT="$out/probe-result.json"

if [ "$mode" = "--separate-neg-sampler" ]; then
    set +e
    timeout 60 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/separate-neg-log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -eq 0 ] || ! grep -q 'round 0 pixel 0: 0,255,0,255' "$out/separate-neg-log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-|commit round|wait round' "$out/separate-neg-log"; then
        echo '【红】分离采样器来源负控未击中像素断言，或 validation 有报错' >&2; exit 1
    fi
    echo 'PASS: 改错采样器来源命中像素负控，validation 0 条'; exit 0
fi
if [ "$mode" = "--separate-core" ]; then
    set +e
    timeout 60 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/separate-log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || ! grep -q llvmpipe "$out/separate-log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/separate-log"; then
        cp "$out/separate-log" "$repo/build/vk-separate-failure.log"
        echo "【红】分离来源回放采样 rc=$rc 或 validation 有报错；完整日志 build/vk-separate-failure.log" >&2; exit 1
    fi
    echo '【绿】分离来源回放采样：repeat 红、clamp 绿，validation 0 条'; exit 0
fi
if [ "$mode" = "--tbuf-compiler-core" ]; then
    rm -rf "$repo/build/tbuf-real-red/core-compiler"
    set +e
    timeout 90 "$out/t_backend_vk" "$repo/build/tbuf-real-red" 2>&1 | tee "$out/tbuf-compiler-log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || ! grep -q llvmpipe "$out/tbuf-compiler-log" ||
       ! grep -q 'core Maxwell TBUF: expected 17,34,51,255 on 16 pixels, failures=0' "$out/tbuf-compiler-log" ||
       grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/tbuf-compiler-log"; then
        echo "【红】真实 Maxwell TBUF 回放 rc=$rc 或 validation 有报错" >&2; exit 1
    fi
    echo 'PASS: 真实生产请求、Maxwell 编译、池镜像与 Vulkan 16 像素，validation 0 条'; exit 0
fi
if [[ "$mode" == --tbuf-* ]]; then
    set +e
    timeout 60 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/tbuf-log"
    rc=${PIPESTATUS[0]}
    set -e
    if ! grep -q llvmpipe "$out/tbuf-log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/tbuf-log"; then
        echo '【红】TBUF 回放设备非 lavapipe 或 validation 有报错' >&2; exit 1
    fi
    case "$mode" in
        --tbuf-core) [ "$rc" -eq 0 ] && grep -q 'TBUF replay: expected 9,13,4,255 on 16 pixels, failures=0' "$out/tbuf-log" ;;
        --tbuf-neg-resolve) [ "$rc" -ne 0 ] && grep -q 'two TBUF draws without skips' "$out/tbuf-log" ;;
        --tbuf-neg-format) [ "$rc" -ne 0 ] && grep -q 'TBUF pixel 0: 0,0,4,255 expected 9,13,4,255' "$out/tbuf-log" ;;
        --tbuf-neg-elements) [ "$rc" -ne 0 ] && grep -q 'TBUF pixel 0: 9,13,8,255 expected 9,13,4,255' "$out/tbuf-log" ;;
    esac || { echo "【红】$mode 未命中预期" >&2; exit 1; }
    echo "PASS: $mode 命中预期，validation 0 条"; exit 0
fi
if [ "$mode" = "--shown-only" ]; then
    for shown_mode in --shown-only --shown-unsupported; do
        set +e
        timeout 90 "$out/t_backend_vk" "$out/shaders" "$shown_mode" 2>&1 | tee "$out/shown-log"
        rc=${PIPESTATUS[0]}
        set -e
        if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/shown-log"; then exit 1; fi
    done
    echo 'C883 WSI validation 0 条'; exit 0
fi

if [ "$mode" = "--gs-core" ]; then
    rm -rf "$repo/build/696-real-compiler/core-compiler"
    set +e
    timeout 60 "$out/t_backend_vk" "$repo/build/696-real-compiler" 2>&1 | tee "$out/gs-log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/gs-log"; then echo "【红】GS 核心整链 rc=$rc" >&2; exit 1; fi
    echo '【绿】GS 核心整链 validation 0 条'; exit 0
fi
if [ "$mode" = "--gs-no-cap" ] || [ "$mode" = "--gs-neg-cap" ]; then
    set +e
    env -u S2A_CAPPROBE_RESULT timeout 60 "$out/t_backend_vk" "$out/shaders" --gs-no-cap 2>&1 | tee "$out/gs-log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$mode" = "--gs-neg-cap" ]; then
        if [ "$rc" -eq 0 ] || ! grep -q 'no capability refuses GS shader before Vulkan' "$out/gs-log"; then echo '【红】无能力负控未击中' >&2; exit 1; fi
        echo 'PASS: 忽略无能力时目标断言红'; exit 0
    fi
    if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/gs-log"; then echo '【红】无探测拒绝失败' >&2; exit 1; fi
    echo 'PASS: 无探测时 GS 未启用且 shader 拒绝'; exit 0
fi
if [ "$mode" = "--gs-neg-stage" ] || [ "$mode" = "--gs-neg-flags" ]; then
    set +e
    timeout 60 "$out/t_backend_vk" "$out/shaders" --gs-only > "$out/gs-log" 2>&1
    rc=$?
    set -e
    mkdir -p "$repo/build/696-tests"
    cp "$out/gs-log" "$repo/build/696-tests/${mode#--}.log"
    if [ "$mode" = "--gs-neg-stage" ]; then expected='GS pass'; else expected='VUID-'; fi
    if ! grep -q "$expected" "$out/gs-log" || { [ "$rc" -eq 0 ] && ! grep -qE 'Validation Error|VUID-' "$out/gs-log"; }; then
        echo "【红】$mode 负控未击中预期" >&2; exit 1
    fi
    echo "PASS: $mode 命中预期红"; exit 0
fi
if [ "$mode" = "--gs-only" ] || [ "$mode" = "--gs-compiler" ]; then
    shader_dir="$out/shaders"
    if [ "$mode" = "--gs-compiler" ]; then shader_dir="$repo/build/696-real-compiler"; fi
    set +e
    timeout 60 "$out/t_backend_vk" "$shader_dir" "$mode" 2>&1 | tee "$out/gs-log"
    rc=${PIPESTATUS[0]}
    set -e
    bad=$(grep -cE "Validation Error|VUID-" "$out/gs-log" || true)
    if [ "$rc" -ne 0 ] || [ "$bad" -ne 0 ]; then
        echo "【红】GS 用例 rc=$rc validation=$bad" >&2; exit 1
    fi
    echo "【绿】GS 用例：validation 0 条"
    exit 0
fi

if [ "$mode" = "" ]; then bash "$repo/android/tests/vk/run.sh" --gs-core; fi
if [ "$mode" = "" ]; then bash "$repo/android/tests/vk/run.sh" --separate-core; fi
if [ "$mode" = "" ]; then bash "$repo/android/tests/vk/run.sh" --tbuf-core; fi
if [ "$mode" = "" ]; then bash "$repo/android/tests/vk/run.sh" --tbuf-compiler-core; fi
if [ "$mode" = "" ]; then bash "$repo/android/tests/vk/run_replay_copy_t2b.sh"; fi
if [ "$mode" = "" ]; then bash "$repo/android/tests/vk/run_replay_clear_tex.sh"; fi

set +e
# 整套三遍的超时只用来发现卡死，不量性能：负载高的机器（列车机同时跑多份同类用例，软件渲染）上单遍已接近原先的 180 / 240 秒，
# 主线自己也会被误判超时。放到 600 秒。
timeout 600 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/log"
rc=${PIPESTATUS[0]}
set -e

if [ "$rc" -ne 0 ]; then
    echo "【红】用例失败（rc=${rc}）" >&2
    exit 1
fi
if ! grep -q "llvmpipe" "$out/log"; then
    echo "【红】设备不是 lavapipe（CPU 参考实现）：" >&2
    grep '^device:' "$out/log" >&2 || true
    exit 1
fi
if [ "$(grep -c "C697e 小 mip 整块区域：已核对" "$out/log")" -lt 2 ]; then
    echo "【红】C697e（小 mip 整块区域，原生 / 现解两条路径）没跑全" >&2
    exit 1
fi
if ! grep -q "C697f 3D BC 按切片号上传：已核对" "$out/log"; then
    echo "【红】没跑到 C697f（缓冲 → 3D BC 现解贴图按切片号上传）" >&2
    exit 1
fi
if ! grep -q "C907 保留路径管线缓存落盘：已核对" "$out/log"; then
    echo "【红】没跑到 C907（停局保留会话时的管线缓存落盘）" >&2
    exit 1
fi
bad=$(grep -cE "Validation Error|VUID-" "$out/log" || true)
if [ "$bad" -ne 0 ]; then
    echo "【红】validation 报错 $bad 条：" >&2
    grep -E "Validation Error|VUID-" "$out/log" | head -20 >&2
    exit 1
fi

# 同一套用例在「两级回放开」（vkCmd* 调用交给录制线程）下再跑一遍：像素期望和校验层判据不变。
set +e
S2A_TEST_NVK_DQ=1 timeout 600 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/log_dq"
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" -ne 0 ]; then
    echo "【红】两级回放开着的那一遍用例失败（rc=${rc}）" >&2
    exit 1
fi
bad=$(grep -cE "Validation Error|VUID-" "$out/log_dq" || true)
if [ "$bad" -ne 0 ] || ! grep -q "两级回放：交给录制线程" "$out/log_dq"; then
    echo "【红】两级回放开着的那一遍：validation 报错 $bad 条，或没有调用进队列：" >&2
    grep -E "Validation Error|VUID-" "$out/log_dq" | head -20 >&2
    exit 1
fi

# 描述符组备忘开着（开关 nvk_desc_memo）再跑一遍：像素期望不变、校验层 0 条。
set +e
S2A_TEST_DESC_MEMO=1 timeout 600 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/log_memo"
rc=${PIPESTATUS[0]}
set -e
bad=$(grep -cE "Validation Error|VUID-" "$out/log_memo" || true)
if [ "$rc" -ne 0 ] || [ "$bad" -ne 0 ]; then
    echo "【红】描述符组备忘开着的那一遍：rc=${rc}，validation 报错 $bad 条" >&2
    grep -E "Validation Error|VUID-" "$out/log_memo" | head -20 >&2
    exit 1
fi

# 提交失败的首因（#889）：同步返回与回调给出同一首错原因；独立会话单跑。
set +e
S2A_TEST_ERROR697_ONLY=1 timeout 120 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/log_e697"
rc=${PIPESTATUS[0]}
set -e
bad=$(grep -cE "Validation Error|VUID-" "$out/log_e697" || true)
if [ "$rc" -ne 0 ] || [ "$bad" -ne 0 ] || ! grep -q "E697 isolated failures=0" "$out/log_e697"; then
    echo "【红】提交失败首因那一遍（#889）：rc=${rc}，validation 报错 $bad 条" >&2
    exit 1
fi

# 生产回放的普通间接绘制与单次 DRV0（#941）：隔离用例（真 rp_draw / rp_buf + 真后端），同一容器里两级回放关 / 开各跑一遍
for dq in 0 1; do
    set +e
    if [ "$dq" = 1 ]; then export S2A_REPLAY_INDIRECT_DQ=1 S2A_TEST_NVK_DQ=1; else unset S2A_REPLAY_INDIRECT_DQ S2A_TEST_NVK_DQ; fi
    S2A_LAVAPIPE_ICD="$icd" S2A_VK_LAYER_DIR="$layer" GLSLC="$glslc" timeout 300 bash "$repo/android/tests/vk/run.sh" --indirect-core 2>&1 | tee "$out/log_indirect_$dq"
    rc=${PIPESTATUS[0]}
    set -e
    unset S2A_REPLAY_INDIRECT_DQ S2A_TEST_NVK_DQ
    bad=$(grep -cE "Validation Error|VUID-" "$out/log_indirect_$dq" || true)
    if [ "$rc" -ne 0 ] || [ "$bad" -ne 0 ] || ! grep -q "pixel-failures0" "$out/log_indirect_$dq"; then
        echo "【红】间接绘制隔离用例（#941，两级回放 ${dq}）：rc=${rc}，validation 报错 $bad 条" >&2
        exit 1
    fi
done

# BC1–BC7 compute 解码逐像素用例（#1199）：16 格式 × 2 尺寸 + 反证；同一 validation 环境。
# S2A_BCDEC_HOST_TEST 使 S2A_VK_BC_DECODE=force 在主机用例里生效（等同 debug 构建门）。
cc "${cflags[@]}" -DS2A_BCDEC_HOST_TEST "${inc[@]}" "${bc_src[@]}" -lvulkan -lpthread -lm -o "$out/t_bc_decode"
set +e
S2A_VK_BC_DECODE=force timeout 180 "$out/t_bc_decode" 2>&1 | tee "$out/log_bc"
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" -ne 0 ]; then
    echo "【红】BC 解码用例失败（rc=${rc}）" >&2
    exit 1
fi
bad=$(grep -cE "Validation Error|VUID-" "$out/log_bc" || true)
if [ "$bad" -ne 0 ]; then
    echo "【红】BC 解码 validation 报错 $bad 条：" >&2
    grep -E "Validation Error|VUID-" "$out/log_bc" | head -20 >&2
    exit 1
fi
# #968: draw-local texture-coordinate support bitmap. The helper uses the
# same host headers, loader, glslc and ICD checked above. It does not sample
# textures, and is not itself sanitizer-instrumented in --asan/--tsan modes.
set +e
(
    unset FIRST_DRAW_SOURCE VULKAN_RUNNER VULKAN_INCLUDE
    export GLSLC="$glslc"
    timeout 180 bash "$repo/android/tests/first_draw_unnorm_test.sh"
) 2>&1 | tee "$out/log_unnorm"
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" -ne 0 ]; then
    echo "【红】#968 support bitmap fixture failed (rc=${rc})" >&2
    exit "$rc"
fi
bad=$(grep -cE "Validation Error|VUID-" "$out/log_unnorm" || true)
if [ "$bad" -ne 0 ]; then
    echo "【红】#968 validation reported $bad errors" >&2
    exit 1
fi
if [ "$mode" = "--tsan" ]; then
    echo "【绿】C01–C08 + C23/C25/C26/C27/C28/C29/C30/C31/C32/C33/C34/C35/C36-55/C70/C71/C72/C73 + 清屏/复制/深度/纹理三角形：TSan 0 报告（validation 由默认档单独跑）；设备 $(grep '^device:' "$out/log" | head -1)"
else
    echo "【绿】C01–C08 + C23/C25/C26/C27/C28/C29/C30/C31/C32/C33/C34/C35/C36-55/C70/C71/C72/C73 + 清屏/复制/深度/纹理三角形 + BC1–BC7 逐像素解码：validation 0 条；设备 $(grep '^device:' "$out/log" | head -1)"
fi
