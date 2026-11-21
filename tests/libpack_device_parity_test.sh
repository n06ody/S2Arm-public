#!/bin/bash
# 出库 × 现编双翻等价门禁（#1836 PR-1，宿主 ≤60 秒）：同一合成请求语料过两条翻译路，比对 SPIR-V 接口事实面
# （能力 / 图像类型 / 执行模式 / IO 位置 / 描述符计数 / cs Workgroup 字节）；未命中白名单的差异即红。
#   语料由 S2A.DirectCompiler.Tests --emit-requests 产（合成、无游戏字节）；设备档是真机 caps 加最低支持档
#   （graphics/tools/direct/profiles/s2a-device-caps-min.json，脚本内并入 devices 轮），经
#   DirectProfileComposer 组档注入；出库侧 imgload_raw 取同一组档 profile 的 supportsImageLoadFormatted。
#   白名单记已知形态差（键 + 一句事实性原因），只减不增。
#   用法：bash tests/libpack_device_parity_test.sh
# 门禁覆盖：graphics/tools/direct/tests/parity_corpus/* tests/libpack_device_parity_test.sh graphics/tools/direct/tests/parity/* graphics/tools/direct/dsh/* graphics/tools/direct/compiler/* android/tests/vk/profiles/devices/* graphics/tools/direct/profiles/s2a-device-caps-min.json
set -u
R=$(cd "$(dirname "$0")/.." && pwd)
T=$(cd "$(mktemp -d "${TMPDIR:-/tmp}/parity.XXXXXX")" && pwd -P); trap 'rm -rf "$T"' EXIT
red() { echo "🔴 双翻等价门禁：$1"; exit 1; }
CORPUS=$R/graphics/tools/direct/tests/parity_corpus
DEVICES=$R/android/tests/vk/profiles/devices
MINPROF=$R/graphics/tools/direct/profiles/s2a-device-caps-min.json
[ -f "$CORPUS/whitelist.tsv" ] || red "缺 graphics/tools/direct/tests/parity_corpus/whitelist.tsv"
ls "$CORPUS"/requests/*.json > /dev/null 2>&1 || red "缺 graphics/tools/direct/tests/parity_corpus/requests/*.json 语料"
ls "$DEVICES"/*.json > /dev/null 2>&1 || red "缺 android/tests/vk/profiles/devices/*.json 设备档"
# 真机档之外并入最低支持档（shaderStorageImageReadWithoutFormat=false）：出库与现编在该档
# 都应声明具体格式，最低档轮红即「出库写死 true」型回归（#1774 ② / #490275）。
mkdir -p "$T/devices"; cp "$DEVICES"/*.json "$T/devices/"; cp "$MINPROF" "$T/devices/minimum.json"
DEVICES=$T/devices
DN=${DOTNET:-$HOME/.dotnet/dotnet}; [ -x "$DN" ] || DN=$(command -v dotnet) || red "没有 dotnet（.NET 10）"
XC=${SPIRV_CROSS:-$(command -v spirv-cross || true)}
if [ -z "$XC" ]; then
  S=$R/graphics/third_party/spirv_cross; XC=$T/spirv-cross
  "${CXX:-c++}" -std=c++17 -O1 -o "$XC" "$S/main.cpp" "$S/spirv_cfg.cpp" "$S/spirv_cross.cpp" "$S/spirv_cross_parsed_ir.cpp" "$S/spirv_cross_util.cpp" \
    "$S/spirv_glsl.cpp" "$S/spirv_hlsl.cpp" "$S/spirv_msl.cpp" "$S/spirv_parser.cpp" "$S/spirv_cpp.cpp" "$S/spirv_reflect.cpp" > "$T/xc.log" 2>&1 \
    || red "仓库内 SPIRV-Cross 编不过：$(grep -m1 -i error "$T/xc.log" | cut -c1-200)"
fi
if [ "$(uname -s)" = Darwin ]; then LIB=$R/build/dsx/libdsx.dylib; else LIB=$R/build/dsx/linux/libdsx.so; fi
make -s -C "$R/graphics/dsx" "$LIB" > "$T/make.log" 2>&1 || red "libdsx 编不过：$(grep -m1 -i error "$T/make.log" | cut -c1-200)"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
"$DN" build "$R/graphics/tools/direct/tests/parity/S2A.Parity.Tests.csproj" -c Release -p:ArtifactsPath="$T/art" -p:NuGetAudit=false --verbosity quiet > "$T/build.log" 2>&1 \
  || red "比较器工程编不过：$(grep -m1 ' error ' "$T/build.log" | cut -c1-200)"
"$DN" "$T/art/bin/S2A.Parity.Tests/release/S2A.Parity.Tests.dll" "$CORPUS" "$DEVICES" "$XC" "$T/work" --dsx "$LIB" || exit 1
echo "✓ 双翻等价门禁过"
