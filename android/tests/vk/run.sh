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
if [ "$(uname -s)" = Darwin ]; then
    exec bash "$repo/tests/vulkan/run.sh" android/tests/vk/run.sh "$@"
fi
mode=${1:-}

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
for s in tri.vert tri.frag tri_combined.frag tri3.vert tri_solid.frag tri_invert.frag solid_green.frag solid_blue.frag out1_green.frag tex_u.frag quad.vert quad.frag inst.vert point.vert quad2.frag arr.frag cube.frag cubearr.frag tex3d.frag comp.comp pushc.frag pushc.comp tbuf.comp itbuf.comp itbuf_unknown.comp itbuf_unknown_read.comp img.comp tsamp.comp arr2.frag; do
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
     -I "$repo/android/native/direct")
src=("$repo/android/tests/vk/t_backend_vk.c"
     "$repo/android/native/nvn/backend_vulkan.c"
     "$repo/android/native/direct/vk_device.c"
     "$repo/android/native/direct/gs_path.c"
     "$repo/android/native/nvn/nvn_vk_format.c"
     "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c"
     "$repo/android/native/nvn/nvn_vk_bcdec.c")
bc_src=("$repo/android/tests/vk/t_bc_decode.c"
     "$repo/android/native/nvn/nvn_vk_bcdec.c"
     "$repo/android/native/direct/vk_device.c"
     "$repo/android/native/direct/gs_path.c"
     "$repo/graphics/nvn/gfx/tex/nvn_bcdec.c")
# 与发行 APK 同一组建设备规则（android/CMakeLists.txt 的 nvn_vk 后端定义此宏）。
cflags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -DS2A_ANDROID_VK_BACKEND_NVN_VK=1)
case "$mode" in
    --tsan) cflags+=(-fsanitize=thread) ;;
    --asan) cflags+=(-fsanitize=address,undefined -fno-omit-frame-pointer) ;;
    "") ;;
    *) echo "未知参数：${mode}（可用 --tsan / --asan）" >&2; exit 64 ;;
esac

echo "lavapipe: $icd"
echo "validation: $layer"
cc "${cflags[@]}" "${inc[@]}" "${src[@]}" -Wl,--wrap=vkAllocateDescriptorSets -Wl,--wrap=vkGetPhysicalDeviceFormatProperties -lvulkan -lpthread -lm -o "$out/t_backend_vk"

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
