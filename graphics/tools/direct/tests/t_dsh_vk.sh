#!/bin/bash
# 出包 Vulkan 产物单测（#783，graphics/tools/direct/dsh/DshVk.cs 的 CompileWithSpv / BindToArgIds；Linux 约 2 分钟，含现编 SPIRV-Cross）：bash graphics/tools/direct/tests/t_dsh_vk.sh
#   原创合成着色器（VS / PS / CS，见 dsh_vk/Program.cs 文件头）同键出 _b.json + _b.spv，核 ① 键集合相等 ② 绑定号 = 同名成员的 arg_id
#   （组合纹理取 texture 成员；入口没取用、json 里也没有的变量在全部 arg_id 之后）③ spirv-val；反证 ④ 未改写的 .spv 对不上 ⑤ 缺成员报红 ⑥ 改 arg_id 结果跟着变。
#   工具：.NET 10（DOTNET / ~/.dotnet / PATH）；spirv-cross（SPIRV_CROSS / PATH；都没有就把仓库内 graphics/third_party/spirv_cross 现编到临时目录）；
#   spirv-val（PATH，或 ANDROID_NDK_HOME / ANDROID_HOME / /opt/android-sdk 的 shader-tools）。缺工具报红，不下载。
#   直连翻译库 libdsx（Compile 对所有阶段先经它加固、片元再经它做颜色出口改写）：Mac 编 build/dsx/libdsx.dylib，其它平台编 build/dsx/linux/libdsx.so
#   （graphics/dsx，#783），经 --dsx 交给测试，三个键都走 CompileWithSpv。
set -u
R=$(cd "$(dirname "$0")/../../../.." && pwd); H=$R/graphics/tools/direct/tests
T=$(mktemp -d "${TMPDIR:-/tmp}/t_dsh_vk.XXXXXX"); trap 'rm -rf "$T"' EXIT
red() { echo "🔴 出包 Vulkan 产物：$1"; exit 1; }
DN=${DOTNET:-$HOME/.dotnet/dotnet}; [ -x "$DN" ] || DN=$(command -v dotnet) || red "没有 dotnet（.NET 10）"
XC=${SPIRV_CROSS:-$(command -v spirv-cross || true)}
if [ -z "$XC" ]; then
  S=$R/graphics/third_party/spirv_cross; XC=$T/spirv-cross
  "${CXX:-c++}" -std=c++17 -O1 -o "$XC" "$S/main.cpp" "$S/spirv_cfg.cpp" "$S/spirv_cross.cpp" "$S/spirv_cross_parsed_ir.cpp" "$S/spirv_cross_util.cpp" \
    "$S/spirv_glsl.cpp" "$S/spirv_hlsl.cpp" "$S/spirv_msl.cpp" "$S/spirv_parser.cpp" "$S/spirv_cpp.cpp" "$S/spirv_reflect.cpp" > "$T/xc.log" 2>&1 \
    || red "仓库内 SPIRV-Cross 编不过：$(grep -m1 -i error "$T/xc.log" | cut -c1-200)"
fi
VAL=$(command -v spirv-val || true)
if [ -z "$VAL" ]; then
  for d in "${ANDROID_NDK_HOME:-}" "${ANDROID_HOME:-/nonexistent}"/ndk/* /opt/android-sdk/ndk/*; do
    for host in linux-x86_64 darwin-x86_64; do [ -x "$d/shader-tools/$host/spirv-val" ] && VAL=$d/shader-tools/$host/spirv-val && break 2; done
  done
fi
[ -n "$VAL" ] || red "没有 spirv-val（SPIRV-Tools 或 NDK shader-tools）"
if [ "$(uname -s)" = Darwin ]; then LIB=$R/build/dsx/libdsx.dylib; else LIB=$R/build/dsx/linux/libdsx.so; fi   # 直连翻译库 libdsx（11-08 从 libnxgal 拆出）
make -s -C "$R/graphics/dsx" "$LIB" > "$T/make.log" 2>&1 || red "libdsx 编不过（${LIB}）：$(grep -m1 -i error "$T/make.log" | cut -c1-200)"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
"$DN" build "$H/dsh_vk/S2A.DshVkTest.csproj" -c Release -p:ArtifactsPath="$T/art" -p:NuGetAudit=false --verbosity quiet > "$T/build.log" 2>&1 \
  || red "测试工程编不过：$(grep -m1 ' error ' "$T/build.log" | cut -c1-200)"
# S2A_DSH_VK_EMIT_DIR：另出存储缓冲窗口读回夹具料（见 dsh_vk/Program.cs ⑭）；目录必须在仓库检出里（容器里的夹具按仓库路径读）
EMIT=(); [ -z "${S2A_DSH_VK_EMIT_DIR:-}" ] || EMIT=(--emit "$S2A_DSH_VK_EMIT_DIR")
"$DN" "$T/art/bin/S2A.DshVkTest/release/S2A.DshVkTest.dll" "$XC" "$VAL" "$T/work" --dsx "$LIB" ${EMIT[@]+"${EMIT[@]}"}
