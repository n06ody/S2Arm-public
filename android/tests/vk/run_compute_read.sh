#!/usr/bin/env bash
# 真 compute → render-pass indirect draw；所有臂要求实际读回正确且 syncval 无错。
# 新修复前仅 automatic 臂应出现计算写 → 间接读取 hazard；manual/no-producer 为控制。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
out="$repo/build/vk-compute-read"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
# 旧层没有计算着色器访存检测能力：独立环境状态 75，既不把漏报判绿，也不伪造同步缺陷红。
# manifest 宣告能力后仍须通过下面真实 Vulkan 正/反控；不能仅凭版本或设置键宣告测试通过。
python3 - "$S2A_VK_LAYER_DIR/VkLayer_khronos_validation.json" <<'DETECTOR'
import json
from pathlib import Path
import sys

key = "syncval_shader_accesses_heuristic"
def declared(value):
    if isinstance(value, dict):
        return value.get("key") == key or any(declared(v) for v in value.values())
    if isinstance(value, list):
        return any(declared(v) for v in value)
    return False

try:
    layer = json.loads(Path(sys.argv[1]).read_text())["layer"]
    if not isinstance(layer, dict):
        raise ValueError("layer metadata is not an object")
except (OSError, ValueError, KeyError) as exc:
    print(f"【未证】检测器能力无法核实，本项交 CI 同版本验：{exc}", file=sys.stderr)
    sys.exit(75)
if layer.get("name") != "VK_LAYER_KHRONOS_validation" or not declared(layer):
    print(f"【未证】检测器能力不足，本项交 CI 同版本验：layer_api={layer.get('api_version', 'unknown')} 未声明 {key}", file=sys.stderr)
    sys.exit(75)
print(f"DETECTOR layer_api={layer.get('api_version', 'unknown')} supports={key}；继续严格实际访存正反控")
DETECTOR
headers="$repo/build/c883-ndk"
[ -f "$headers/vulkan/vulkan_core.h" ] || { echo '【红】缺真实 NDK 29 Vulkan 头；先由宿主出料' >&2; exit 2; }
for shader in compute_read_args.comp compute_read_readonly.comp compute_read_payload.comp compute_read_index.comp compute_read_image.comp compute_read_vertex.vert compute_read_buffer.vert compute_read_buffer.frag compute_read_texel.frag compute_read_image.frag solid_green.frag; do
    glslc --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
glslc --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
inc=(-I "$headers")
for dir in android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$dir"); done
src=("$repo/android/tests/vk/t_replay_compute_read.c")
for file in graphics/nvn/replay/rp_draw.c graphics/nvn/replay/rp_buf.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$file"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" -Wl,--gc-sections -Wl,--wrap=vkGetDeviceProcAddr -Wl,--wrap=vkCreateInstance -Wl,--wrap=vkQueueSubmit -Wl,--wrap=vkWaitForFences -Wl,--wrap=vkQueueWaitIdle -Wl,--wrap=vkDeviceWaitIdle -Wl,--wrap=vkCreateRenderPass -Wl,--wrap=vkUpdateDescriptorSets -lvulkan -lpthread -lm -o "$out/test"
src[0]="$repo/android/tests/vk/t_replay_compute_resources.c"
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" -Wl,--gc-sections -Wl,--wrap=vkGetDeviceProcAddr -Wl,--wrap=vkCreateInstance -Wl,--wrap=vkQueueSubmit -Wl,--wrap=vkWaitForFences -Wl,--wrap=vkQueueWaitIdle -Wl,--wrap=vkDeviceWaitIdle -Wl,--wrap=vkCreateRenderPass -Wl,--wrap=vkUpdateDescriptorSets -lvulkan -lpthread -lm -o "$out/resources"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -I "$headers" "$repo/android/tests/vk/t_compute_read_raw.c" -lvulkan -o "$out/raw"
failed=0
for arm in env transfer-env sync-only explicit external external-graphics external-graphics-manual manual transfer; do
    log="$out/raw-$arm.log"
    set +e
    timeout 120 "$out/raw" "$out/shaders" "$arm" > "$log" 2>&1
    rc=$?
    set -e
    marker=$(grep '^RAW_COMPUTE_READ ' "$log" || true)
    printf '%s\n' "$marker"
    if [ "$rc" != 0 ] || [ -z "$marker" ]; then failed=1; tail -n 15 "$log"; fi
    if { [ "$arm" = explicit ] || [ "$arm" = transfer ] || [ "$arm" = external-graphics ]; } && ! grep -q 'RAW=[1-9]' <<< "$marker"; then
        echo "【红】裸 Vulkan 阳性对照 $arm 未检出 hazard"; failed=1
    fi
    if { [ "$arm" = manual ] || [ "$arm" = external-graphics-manual ]; } && ! grep -q 'RAW=0 other_errors=0' <<< "$marker"; then
        echo '【红】裸 Vulkan 精确屏障控制未绿'; failed=1
    fi
done
for dq in 0 1; do
export S2A_COMPUTE_READ_DQ=$dq
for arm in no-producer manual automatic-env automatic manual-normalized automatic-normalized unrelated-normalized repeat-normalized zero-normalized disabled-normalized cross-segment-normalized game-barrier-normalized rewrite-normalized readonly-normalized; do
    log="$out/dq$dq-$arm.log"
    set +e
    timeout 120 "$out/test" "$out/shaders" "$arm" > "$log" 2>&1
    rc=$?
    set -e
    errors=$(grep -cE 'Validation Error|VUID-|SYNC-HAZARD' "$log" || true)
    hazards=$(grep -c 'SYNC-HAZARD-READ-AFTER-WRITE' "$log" || true)
    marker=$(grep "^COMPUTE_READ arm=$arm " "$log" || true)
    printf '%s\n' "$marker"
    printf 'compute-read dq=%s arm=%s rc=%s validation_lines=%s read_after_write=%s\n' "$dq" "$arm" "$rc" "$errors" "$hazards"
    if [ "$arm" = disabled-normalized ]; then
        other=$(grep -E 'Validation Error|VUID-' "$log" | grep -vc 'SYNC-HAZARD-READ-AFTER-WRITE' || true)
        if [ "$rc" = 0 ] && [ "$hazards" -ge 1 ] && [ "$other" = 0 ] && [ -n "$marker" ]; then
            echo '【绿】负控：仅关计算写读跟踪，重新报目标 RAW'; continue
        fi
        failed=1; echo '【红】关跟踪负控未命中目标'; tail -n 15 "$log"; continue
    fi
    if [ "$rc" != 0 ] || [ "$errors" != 0 ] || [ -z "$marker" ]; then
        failed=1
        echo "【红】计算写→图形读 dq=$dq ${arm}；原始记录 build/vk-compute-read/dq$dq-$arm.log"
        # 仅失败时给出定位，不隐藏编译或运行错误；完整原始日志保留。
        tail -n 14 "$log"
    fi
done
# 每个消费者都有同输入、仅关闭跟踪的负控；间接参数在 CPU 写且不相交。
for kind in vertex index vsbuf fsbuf tbuf image image-disjoint; do
 for suffix in enabled disabled; do
    arm="$kind-$suffix"; log="$out/dq$dq-$arm.log"
    set +e
    timeout 120 "$out/resources" "$out/shaders" "$arm" > "$log" 2>&1
    rc=$?
    set -e
    errors=$(grep -cE 'Validation Error|VUID-|SYNC-HAZARD' "$log" || true)
    hazards=$(grep -c 'SYNC-HAZARD-READ-AFTER-WRITE' "$log" || true)
    other=$(grep -E 'Validation Error|VUID-' "$log" | grep -vc 'SYNC-HAZARD-READ-AFTER-WRITE' || true)
    marker=$(grep "^COMPUTE_RESOURCE arm=$arm " "$log" || true)
    printf '%s\n' "$marker"
    printf 'compute-resource dq=%s arm=%s rc=%s validation_lines=%s RAW=%s other=%s\n' "$dq" "$arm" "$rc" "$errors" "$hazards" "$other"
    if [ "$suffix" = disabled ]; then
        if [ "$rc" = 0 ] && [ "$hazards" -ge 1 ] && [ "$other" = 0 ] && [ -n "$marker" ]; then continue; fi
    elif [ "$rc" = 0 ] && [ "$errors" = 0 ] && [ -n "$marker" ]; then continue
    fi
    failed=1; echo "【红】资源依赖 $arm 未满足断言"; tail -n 18 "$log"
 done
done
done
unset S2A_COMPUTE_READ_DQ
set +e
timeout 120 "$out/resources" "$out/shaders" metadata > "$out/metadata.log" 2>&1
meta_rc=$?
set -e
if [ "$meta_rc" != 0 ] || grep -qE 'Validation Error|VUID-' "$out/metadata.log" || ! grep -q 'STORAGE_SPAN lifecycle/buffer-alias/3d-depth: PASS' "$out/metadata.log"; then
    failed=1; echo '【红】存储范围元数据不符合真实视图'; tail -n 15 "$out/metadata.log"
else grep '^STORAGE_SPAN ' "$out/metadata.log"
fi
# 验证共享入口用的文件式配置；env 臂不注入 API 设置，必须捕捉同一个计算 RAW。
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
cat "$out/vk_layer_settings.txt"
for arm in env manual-env; do
    log="$out/file-$arm.log"
    set +e
    VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt" timeout 120 "$out/raw" "$out/shaders" "$arm" > "$log" 2>&1
    rc=$?
    set -e
    marker=$(grep '^RAW_COMPUTE_READ ' "$log" || true)
    printf 'FILE_CONFIG %s\n' "$marker"
    if [ "$rc" != 0 ] || [ -z "$marker" ]; then failed=1; tail -n 15 "$log"; fi
    if [ "$arm" = env ] && ! grep -q 'RAW=[1-9].*other_errors=0' <<< "$marker"; then failed=1; echo '【红】文件式配置未通过计算阳性对照'; fi
    if [ "$arm" = manual-env ] && ! grep -q 'RAW=0 other_errors=0' <<< "$marker"; then failed=1; echo '【红】文件式配置屏障控制失败'; fi
done
[ "$failed" = 0 ] || exit 1
echo '【绿】计算写→图形读：DQ 关/开各 14 个间接参数臂（含跨段、游戏屏障、再写、只读派发）及 7 类资源 × 开/关 14 臂，共 56 臂，4096 像素及参数正确，裸 Vulkan 检测器与文件配置对照符合断言'
