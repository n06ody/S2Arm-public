#!/usr/bin/env bash
# 门禁手动：只能 Linux（非 Linux 退 75），要 Mesa lavapipe 并构建 Khronos profiles 层（约 3 分钟，要联网）；由 CI .github/workflows/android-vk.yml 跑（改到安卓 Vulkan 相关目录时 + 每天定时全量）
# 安卓 Vulkan 契约测试的「模拟机型」档（#429）：在 Mesa lavapipe 上用 Khronos VK_LAYER_KHRONOS_profiles
# 只「藏」能力，让 android/tests/vk/run.sh 走到真机上才会走的降级 / 拒绝分支。
# 档的定义在 android/tests/vk/profiles/s2a_sim_tiers.json：只按 Vulkan 规范的特性名 / 格式名手写，不含任何设备报告数据。
#
# 用法：
#   bash android/tests/vk/sim_tiers.sh            # 基线 + 全部模拟档 + 负对照
#   bash android/tests/vk/sim_tiers.sh no_bc ...  # 只跑点名的档（基线照跑）
# 环境：
#   S2A_VK_PROFILE_LAYER_DIR  已构建好的 profiles 层目录（含 VkLayer_khronos_profiles.json）；不给就按下面钉的版本构建
#   S2A_VK_PROFILES_CACHE     构建目录（默认 ${XDG_CACHE_HOME:-~/.cache}/s2arm/vk-profiles-<版本>）
# 退出码：0 全部符合期望；1 有档不符（测试失败）；75 环境不齐（缺系统包 / 层构建不出来），不记成测试失败。
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../.." && pwd)
tiers_json="$repo/android/tests/vk/profiles/s2a_sim_tiers.json"
# Vulkan-Profiles 钉版本（header 1.4.368）。改版本要同时确认各档期望仍成立。
profiles_sha=b502e9b0d7508e06e6300af272548cf0ee32f380
profiles_url=https://github.com/KhronosGroup/Vulkan-Profiles.git

all=(no_bc no_d24 no_vertex_stores no_dual_src_blend no_calibrated_timestamps no_mirror_clamp no_depth_clamp no_minmax)
want=("$@"); [ ${#want[@]} -gt 0 ] || want=("${all[@]}")
for t in "${want[@]}"; do
    case " ${all[*]} " in *" $t "*) ;; *) echo "未知档：${t}（可用：${all[*]}）" >&2; exit 64 ;; esac
done

env_missing() { echo "【环境不齐】$*" >&2; exit 75; }

if [ "$(uname -s)" != Linux ]; then env_missing "模拟档只在 Linux（含 WSL）上跑：要 Mesa lavapipe 与 profiles 层"; fi
any() { local p; for p in "$@"; do [ -e "$p" ] && return 0; done; return 1; }
any /usr/share/vulkan/icd.d/lvp_icd*.json "$HOME"/.local/share/vulkan/icd.d/lvp_icd*.json \
    || [ -n "${S2A_LAVAPIPE_ICD:-}" ] || env_missing "缺 lavapipe ICD（apt: mesa-vulkan-drivers）"
any /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json "$HOME"/.local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json \
    || [ -n "${S2A_VK_LAYER_DIR:-}" ] || env_missing "缺 Khronos validation 层（apt: vulkan-validationlayers）"
for t in cc glslc python3 timeout vulkaninfo; do
    command -v "${t}" >/dev/null || env_missing "缺 ${t}（apt: build-essential glslc python3 coreutils vulkan-tools）"
done
any /usr/lib/*/libvulkan.so /usr/lib/libvulkan.so || env_missing "缺 Vulkan 加载器开发包（apt: libvulkan-dev）"

layer_dir=${S2A_VK_PROFILE_LAYER_DIR:-}
if [ -z "$layer_dir" ]; then
    for t in git cmake ninja c++; do
        command -v "${t}" >/dev/null || env_missing "构建 profiles 层缺 ${t}（apt: git cmake ninja-build build-essential）"
    done
    python3 -c 'import jsonschema, pyparsing' 2>/dev/null || env_missing "构建 profiles 层缺 Python 模块（apt: python3-jsonschema python3-pyparsing）"
    [ -f /usr/include/json/json.h ] || [ -f /usr/include/jsoncpp/json/json.h ] || env_missing "构建 profiles 层缺 jsoncpp（apt: libjsoncpp-dev）"
    src=${S2A_VK_PROFILES_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/s2arm/vk-profiles-${profiles_sha:0:12}}
    if [ ! -f "$src/build/layer/VkLayer_khronos_profiles.json" ]; then
        echo "构建 VK_LAYER_KHRONOS_profiles @ ${profiles_sha:0:12} → $src"
        {
            mkdir -p "$src" && cd "$src" &&
            { [ -d .git ] || git init -q; } &&
            git fetch -q --depth 1 "$profiles_url" "$profiles_sha" &&
            git checkout -q FETCH_HEAD &&
            cmake -S . -B build -G Ninja -D CMAKE_BUILD_TYPE=Release -D BUILD_TESTS=OFF -D UPDATE_DEPS=ON >build.log 2>&1 &&
            cmake --build build -j"$(nproc)" >>build.log 2>&1
        } || { tail -20 "$src/build.log" >&2 2>/dev/null || true; env_missing "profiles 层构建失败（见上面的构建日志尾部；常见原因：没网拉不到依赖）"; }
        cd "$repo"
    fi
    layer_dir=$src/build/layer
fi
[ -f "$layer_dir/VkLayer_khronos_profiles.json" ] || env_missing "$layer_dir 下没有 VkLayer_khronos_profiles.json"

out=$(mktemp -d "${TMPDIR:-/tmp}/s2a-vk-sim.XXXXXX")
trap 'rm -rf "$out"' EXIT
prof_dir=$out/profiles
mkdir -p "$prof_dir"
# 手写档只列要藏的格式；格式表其余部分按宿主 lavapipe 自己的补全（见 sim_profile.py）。
icd=${S2A_LAVAPIPE_ICD:-}
if [ -z "$icd" ]; then
    for f in "$HOME"/.local/share/vulkan/icd.d/lvp_icd*.json /usr/share/vulkan/icd.d/lvp_icd*.json; do
        [ -f "$f" ] && { icd=$f; break; }
    done
fi
VK_DRIVER_FILES=$icd VK_ICD_FILENAMES=$icd VK_INSTANCE_LAYERS= vulkaninfo --json -o "$out/host.json" >"$out/vulkaninfo.log" 2>&1 \
    || { cat "$out/vulkaninfo.log" >&2; env_missing "vulkaninfo --json 在 lavapipe 上跑不出宿主档"; }
python3 "$repo/android/tests/vk/sim_profile.py" "$out/host.json" "$tiers_json" "$prof_dir/s2a_sim_tiers.json" \
    || { echo "【红】展开模拟档失败" >&2; exit 1; }

fails=0
red() { echo "【红】$1：$2" >&2; fails=$((fails + 1)); }
has() { grep -qF -- "$2" "$1"; }

# $1 档名（空 = 基线，不挂层）$2 日志；其余参数是额外环境变量
run_tier() {
    local tier=$1 log=$2; shift 2
    set +e
    if [ -z "$tier" ]; then
        (cd "$repo" && env "$@" bash android/tests/vk/run.sh) >"$log" 2>&1
    else
        (cd "$repo" && env S2A_VK_PROFILE_LAYER_DIR="$layer_dir" \
            VK_KHRONOS_PROFILES_PROFILE_DIRS="$prof_dir" \
            VK_KHRONOS_PROFILES_PROFILE_NAME="VP_S2A_SIM_$tier" \
            VK_KHRONOS_PROFILES_SIMULATE_CAPABILITIES=SIMULATE_FEATURES_BIT,SIMULATE_FORMATS_BIT \
            VK_KHRONOS_PROFILES_DEBUG_REPORTS=DEBUG_REPORT_ERROR_BIT \
            "$@" bash android/tests/vk/run.sh) >"$log" 2>&1
    fi
    local rc=$?
    set -e
    if grep -q "PROFILES ERROR" "$log"; then
        red "${tier:-基线}" "profiles 层报错，档没有按定义生效："
        grep "PROFILES ERROR" "$log" | head -5 >&2
    fi
    return $rc
}

# 档 → 期望。每档都核两件事：层确实藏掉了能力（日志里的路由 / 能力行），代码走对了分支（run.sh 退出码）。
check_tier() {
    local tier=$1 log="$out/$1.log" rc=0
    case "$tier" in
        no_bc)
            run_tier no_bc "$log" || rc=$?
            [ "$rc" -eq 0 ] || red no_bc "run.sh rc=${rc}，期望契约绿"
            has "$log" "vk BC route: native=0x0000 compute=0xffff" || red no_bc "没看到 BC 全走计算解码（native=0x0000 compute=0xffff）" ;;
        no_d24)
            run_tier no_d24 "$log" || rc=$?
            [ "$rc" -eq 0 ] || red no_d24 "run.sh rc=${rc}，期望契约绿、0 条 validation"
            has "$log" "vk depth route: D24S8->D32S8 X8D24->D32" || red no_d24 "没看到 D24 换 D32 存储（#422）" ;;
        no_vertex_stores)
            run_tier no_vertex_stores "$log" || rc=$?
            [ "$rc" -eq 0 ] || red no_vertex_stores "run.sh rc=${rc}，期望发行宏下仍建得了设备、契约绿"
            has "$log" "vertexPipelineStoresAndAtomics=0" || red no_vertex_stores "层没藏住 vertexPipelineStoresAndAtomics" ;;
        no_dual_src_blend)
            run_tier no_dual_src_blend "$log" || rc=$?
            [ "$rc" -eq 0 ] || red no_dual_src_blend "run.sh rc=$rc"
            has "$log" "C69 双源混合：设备 dualSrcBlend=0" || red no_dual_src_blend "C69 没在缺 dualSrcBlend 的设备上跑（应按规定报 UNSUPPORTED）" ;;
        no_calibrated_timestamps)
            run_tier no_calibrated_timestamps "$log" \
                VK_KHRONOS_PROFILES_EXCLUDE_DEVICE_EXTENSIONS=VK_KHR_calibrated_timestamps,VK_EXT_calibrated_timestamps || rc=$?
            [ "$rc" -eq 0 ] || red no_calibrated_timestamps "run.sh rc=$rc"
            has "$log" "calibrated_timestamps=0" || red no_calibrated_timestamps "层没去掉校准时间戳扩展（期望走完成点兜底）" ;;
        no_mirror_clamp)
            run_tier no_mirror_clamp "$log" VK_KHRONOS_PROFILES_EXCLUDE_DEVICE_EXTENSIONS=VK_KHR_sampler_mirror_clamp_to_edge || rc=$?
            [ "$rc" -eq 0 ] || red no_mirror_clamp "run.sh rc=${rc}，期望契约绿"
            has "$log" "C976m 镜像夹取：设备 mirror_clamp_to_edge=0" || red no_mirror_clamp "层没去掉 VK_KHR_sampler_mirror_clamp_to_edge"
            has "$log" "C976m 没有扩展：MIRROR_CLAMP_TO_EDGE 建采样器 rc=-1002（sampler_mirror_clamp_to_edge" || red no_mirror_clamp "缺扩展时 MIRROR_CLAMP_TO_EDGE 没回 UNSUPPORTED（#976：不静默换别的环绕）" ;;
        no_minmax)
            run_tier no_minmax "$log" VK_KHRONOS_PROFILES_EXCLUDE_DEVICE_EXTENSIONS=VK_EXT_sampler_filter_minmax || rc=$?
            [ "$rc" -eq 0 ] || red no_minmax "run.sh rc=$rc"
            has "$log" "vk sampler minmax: enabled=0" || red no_minmax "层没去掉 VK_EXT_sampler_filter_minmax"
            has "$log" "C976r 没有扩展：min 归约建采样器 rc=-1002（sampler_reduction_min：" || red no_minmax "C976r 没在缺扩展的设备上具名报 UNSUPPORTED" ;;
        no_depth_clamp)
            run_tier no_depth_clamp "$log" || rc=$?
            [ "$rc" -ne 0 ] || red no_depth_clamp "负对照竟然绿了：缺硬门槛 depthClamp 应建设备失败"
            has "$log" "required feature missing: depthClamp" || red no_depth_clamp "没看到拒绝原因 required feature missing: depthClamp" ;;
        *) echo "未知档：${tier}（可用：${all[*]}）" >&2; exit 64 ;;
    esac
    local t=$((SECONDS - start))
    if [ "$fails" -eq "$before" ]; then echo "【绿】${tier}（${t}s）"; else echo "  日志：${log}（失败时末尾 40 行如下）" >&2; tail -40 "$log" >&2; fi
}


start=$SECONDS; before=$fails
rc=0; run_tier "" "$out/baseline.log" || rc=$?
if [ "$rc" -eq 2 ] && grep -q "找不到" "$out/baseline.log"; then
    cat "$out/baseline.log" >&2; env_missing "run.sh 找不到依赖（见上）"
fi
if [ "$rc" -ne 0 ]; then red 基线 "run.sh rc=$rc"; tail -40 "$out/baseline.log" >&2; else echo "【绿】基线（不挂层，$((SECONDS - start))s）"; fi

for tier in "${want[@]}"; do
    start=$SECONDS; before=$fails
    check_tier "$tier"
done

if [ "$fails" -ne 0 ]; then echo "【红】模拟机型档：$fails 处不符" >&2; exit 1; fi
echo "【绿】模拟机型档：基线 + ${want[*]} 全部符合期望（profiles 层 ${profiles_sha:0:12}）"
