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
# 没装 lavapipe / validation 时退出码 2（红），不静默跳过（#784）。
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../.." && pwd)
mode=${1:-}
if [ "$(uname -s)" = Darwin ]; then
    export S2A_VULKAN_CACHE="$repo/build/vulkan-cache"
    if [ -z "$mode" ]; then python3 "$repo/android/tests/gs_compiler_chain_test.py"; fi
    exec bash "$repo/tests/vulkan/run.sh" android/tests/vk/run.sh "$@"
fi
if [ -z "$mode" ] && [ ! -f "$repo/build/696-real-compiler/compiled/program.json" ]; then
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
for s in gs_probe.vert gs_probe.geom gs_probe.frag tri.vert tri.frag tri_combined.frag tri3.vert tri_solid.frag tri_invert.frag solid_green.frag solid_blue.frag out1_green.frag tex_u.frag quad.vert quad.frag inst.vert point.vert quad2.frag arr.frag cube.frag cubearr.frag tex3d.frag comp.comp pushc.frag pushc.comp tbuf.comp itbuf.comp itbuf_unknown.comp itbuf_unknown_read.comp img.comp tsamp.comp arr2.frag; do
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$s" -o "$out/shaders/$s.spv"
done

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
src=("$repo/android/tests/vk/t_backend_vk.c"
     "$repo/android/native/nvn/backend_vulkan.c"
     "$repo/android/native/direct/vk_device.c"
     "$repo/android/native/direct/gs_path.c"
     "$repo/android/native/capprobe/capprobe_host.c"
     "$repo/android/native/nvn/nvn_vk_format.c"
     "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c"
     "$repo/android/native/nvn/nvn_vk_bcdec.c")
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
    ""|--gs-only|--gs-compiler|--gs-core|--gs-no-cap|--gs-neg-stage|--gs-neg-flags|--gs-neg-cap) ;;
    *) echo "未知参数：${mode}（可用 --tsan / --asan）" >&2; exit 64 ;;
esac

echo "lavapipe: $icd"
echo "validation: $layer"
if [ "$mode" = "--gs-core" ]; then
    src=("$repo/android/tests/vk/t_replay_gs.c" "$repo/graphics/nvn/replay/rp_draw.c" "$repo/graphics/nvn/replay/rp_state.c" "$repo/graphics/nvn/replay/nvn_unimpl.c" "$repo/android/native/gfx_plat/nvk_shader.c"
         "$repo/android/native/nvn/backend_vulkan.c" "$repo/android/native/direct/vk_device.c" "$repo/android/native/direct/gs_path.c" "$repo/android/native/capprobe/capprobe_host.c"
         "$repo/android/native/nvn/nvn_vk_format.c" "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c" "$repo/android/native/nvn/nvn_vk_bcdec.c"
         "$repo/android/native/nvn/nvn_vk_program_result.c" "$repo/android/native/nvn/nvn_vk_compute_request.c"
         "$repo/android/native/direct/source_store.c" "$repo/android/native/direct/control.c" "$repo/android/native/direct/input_identity.c" "$repo/android/native/direct/texture_facts.c"
         "$repo/android/native/direct/sha256.c" "$repo/android/native/direct/json.c" "$repo/android/native/direct/jv.c" "$repo/android/native/direct/compile_request.c"
         "$repo/android/native/direct/offline_library.c" "$repo/android/native/direct/program_cache.c" "$repo/runtime/src/direct_host.c")
    cflags+=(-ffunction-sections -fdata-sections -Wno-unused-function)
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
cc "${cflags[@]}" "${inc[@]}" "${src[@]}" -Wl,--wrap=vkAllocateDescriptorSets -Wl,--wrap=vkGetPhysicalDeviceFormatProperties -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/t_backend_vk"

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
if [ "$mode" = "" ]; then bash "$repo/android/tests/vk/run_replay_copy_t2b.sh"; fi
if [ "$mode" = "" ]; then bash "$repo/android/tests/vk/run_replay_clear_tex.sh"; fi

set +e
timeout 180 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/log"
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
bad=$(grep -cE "Validation Error|VUID-" "$out/log" || true)
if [ "$bad" -ne 0 ]; then
    echo "【红】validation 报错 $bad 条：" >&2
    grep -E "Validation Error|VUID-" "$out/log" | head -20 >&2
    exit 1
fi

# 同一套用例在「两级回放开」（vkCmd* 调用交给录制线程）下再跑一遍：像素期望和校验层判据不变。
set +e
S2A_TEST_NVK_DQ=1 timeout 240 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/log_dq"
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
S2A_TEST_DESC_MEMO=1 timeout 240 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/log_memo"
rc=${PIPESTATUS[0]}
set -e
bad=$(grep -cE "Validation Error|VUID-" "$out/log_memo" || true)
if [ "$rc" -ne 0 ] || [ "$bad" -ne 0 ]; then
    echo "【红】描述符组备忘开着的那一遍：rc=${rc}，validation 报错 $bad 条" >&2
    grep -E "Validation Error|VUID-" "$out/log_memo" | head -20 >&2
    exit 1
fi

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
