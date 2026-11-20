#!/usr/bin/env bash
# NativeAOT for Android (linux-bionic-arm64), run by docker/build.sh or host/build.sh.
# Uses the .NET SDK named by S2A_ANDROID_DOTNET when set (host build; must report the pinned
# version so the restored ILC/runtime packs match the Docker build), otherwise installs the
# pinned host-RID SDK into the build cache (SHA-512 checked against Microsoft's release
# metadata), then publishes cs/probe into build/android-cs/jniLibs so Gradle packages it.
# NuGet restore needs network; everything else stays in S2A_BUILD_CACHE
# (default $HOME/.cache/s2arm-build) and build/.
set -euo pipefail
cs_dir=$(cd "$(dirname "$0")" && pwd -P)
repo_dir=$(cd "$cs_dir/../.." && pwd -P)
out=$repo_dir/build/android-cs
# 可复现：托管编译把仓库根映射为固定虚拟路径。Roslyn 给 [GeneratedRegex] 的 file 局部类型名
# 带源文件路径哈希（如 <RegexGenerator_g>F…__名字），不映射会随挂载目录/两路而变。
pathmap=$repo_dir=/_/s2a
cache=${S2A_BUILD_CACHE:-$HOME/.cache/s2arm-build}
. "$repo_dir/android/tools/ndk_toolchain.sh"
sdk_version=10.0.403   # compat/tree/global.json
# Pinned SDK tarball per host RID; SHA-512 from Microsoft release metadata (builds.dotnet.microsoft.com).
case "$(uname -s)-$(uname -m)" in
Linux-x86_64)  sdk_rid=linux-x64
    sdk_sha512=b7b3d8461cf98b07d2552ce9199702fa3ad010ac2671a5f8cd5136f7534e50f6855ba7bbb02b11494563f1d04156276a31cda1103b900e5eecd0d9473dc7b9c5 ;;
Darwin-arm64)  sdk_rid=osx-arm64
    sdk_sha512=5893c66b48ea3674c7da18501059d606e96f3672fabe4d5e41762e692c7c83df3ee0b2b9dc5a0786191da2dc413d7335158101c14c935ab217f31ac2529770f3 ;;
*) echo "【红】.NET SDK 引导不支持宿主 $(uname -s)-$(uname -m)（可用 S2A_ANDROID_DOTNET 指定已有 $sdk_version SDK）" >&2; exit 1 ;;
esac
if [ -n "${S2A_ANDROID_DOTNET:-}" ]; then
    dotnet=$S2A_ANDROID_DOTNET
    [ -x "$dotnet" ] || { echo "S2A_ANDROID_DOTNET is not executable: $dotnet" >&2; exit 1; }
    dotnet_actual=$("$dotnet" --version 2>/dev/null || true)
    # 版本不同 → ILC / NativeAOT 运行时包版本不同 → 与 Docker 产物不可比。要么装固定版本，
    # 要么 unset S2A_ANDROID_DOTNET 让脚本引导私有固定 SDK。
    [ "$dotnet_actual" = "$sdk_version" ] \
        || { echo "【红】S2A_ANDROID_DOTNET 是 ${dotnet_actual}，要求固定 ${sdk_version}；unset 可自动引导" >&2; exit 1; }
else
    DOTNET_ROOT=$cache/dotnet/$sdk_version
    export DOTNET_ROOT
    dotnet=$DOTNET_ROOT/dotnet
    if [ ! -x "$dotnet" ]; then
        mkdir -p "$DOTNET_ROOT.tmp"
        curl -sSfL -o "$DOTNET_ROOT.tgz" \
            "https://builds.dotnet.microsoft.com/dotnet/Sdk/$sdk_version/dotnet-sdk-$sdk_version-$sdk_rid.tar.gz"
        echo "$sdk_sha512  $DOTNET_ROOT.tgz" | s2a_sha512_check -
        tar -xzf "$DOTNET_ROOT.tgz" -C "$DOTNET_ROOT.tmp" && rm "$DOTNET_ROOT.tgz" && mv "$DOTNET_ROOT.tmp" "$DOTNET_ROOT"
    fi
fi
export NUGET_PACKAGES=${NUGET_PACKAGES:-$cache/nuget}
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1   # the image has no ICU; the SDK itself needs none
# Prepare the pinned SDK separately so the Docker gate can check amd64 .NET
# before starting dependency preparation and the NativeAOT/Gradle build.
case "${1:-}" in
--prepare-sdk) exit 0 ;;
--print-dotnet)
    # 出包 Vulkan 库工具（graphics/tools/direct/vklib/dsh_vklib.py，#783）复用同一份 SDK：同上的解析 / 引导，末行打出 dotnet 路径
    echo "$dotnet"; exit 0 ;;
--check-host)
    "$dotnet" --version >/dev/null
    # --version alone can pass while MSBuild target-framework evaluation fails.
    "$dotnet" msbuild "$cs_dir/probe/S2A.Android.Probe.csproj" -nologo -p:Configuration=Release -p:RuntimeIdentifier=linux-bionic-arm64 -getProperty:TargetFramework >/dev/null
    exit 0 ;;
--direct-identity)
    # 本源码树直连编译器的翻译闭包身份（cli identity，W2.1）。安卓出包器把它写进描述符，
    # 与 APK 里编进的编译器同源同配置；不用手写常量（设备端编译器拒收别的闭包）。
    id_dir=$repo_dir/build/android-direct
    cli=$repo_dir/graphics/tools/direct/compiler/cli/S2A.DirectCompiler.Cli.csproj
    mkdir -p "$id_dir"; rm -f "$id_dir/identity.json" "$id_dir/identity.json.tmp"
    # 先编译、输出进日志，再 --no-build 运行：stdout 只留 identity 打印的 JSON（dotnet run 会把编译警告混进 stdout）。
    # 关 MSBuild 节点复用 / 构建服务器、加超时：无终端容器里常驻节点会让 dotnet 挂住不退，挂住时报红而不是卡死出包。
    export MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_USE_MSBUILD_SERVER=0
    s2a_timeout 900 "$dotnet" build "$cli" -c Release -nologo --disable-build-servers -nodeReuse:false > "$id_dir/build.log" 2>&1 \
        || { tail -n 40 "$id_dir/build.log" >&2; echo "【红】直连编译器构建失败或超时（全文 $id_dir/build.log）" >&2; exit 1; }
    s2a_timeout 120 "$dotnet" run --no-build -c Release --project "$cli" -- identity > "$id_dir/identity.json.tmp" \
        || { echo "【红】cli identity 运行失败或超时" >&2; exit 1; }
    python3 - "$id_dir/identity.json.tmp" <<'PY' || exit 1
import json, sys
doc = json.load(open(sys.argv[1], encoding='utf-8'))
if not isinstance(doc, dict) or doc.get('schema') != 's2a-direct-identity-v1' or not doc.get('translatorClosure'):
    sys.exit('【红】cli identity 输出不是 s2a-direct-identity-v1 的身份 JSON')
PY
    mv "$id_dir/identity.json.tmp" "$id_dir/identity.json"
    exit 0 ;;
*) ;;
esac
# ILCompiler links with plain `clang`; the NDK's clang targets bionic and finds its own sysroot.
# NDK prebuilt/<host> 目录按宿主实选（darwin-arm64 / darwin-x86_64 / linux-x86_64），不写死。
ndk=$(s2a_ndk_resolve) || { echo "【红】找不到固定 NDK（ANDROID_NDK_HOME 或 ANDROID_HOME/ndk/${S2A_ANDROID_NDK}）" >&2; exit 1; }
# 显式 ANDROID_NDK_HOME 可能指到别的修订版：核对 source.properties 的固定版本。
grep -q '^Pkg.Revision = '$S2A_ANDROID_NDK'$' "$ndk/source.properties" \
    || { echo "【红】NDK $ndk 不是固定修订 ${S2A_ANDROID_NDK}（source.properties）" >&2; exit 1; }
ndk_bin=$(s2a_ndk_host_bin "$ndk") || { echo "【红】NDK $ndk 没有本宿主可用的 prebuilt bin" >&2; exit 1; }
export PATH=$ndk_bin${DOTNET_ROOT:+:$DOTNET_ROOT}:$PATH
# 静态模式产物缓存（门禁提速，11-16）：两次 dotnet publish + libunwind 改名只取决于 C# 编译闭包与固定工具，
#   按内容算键（cs/static_cache.py），命中就放回 static/、jniLibs/、运行时许可证，跳过从零编（约 164 秒）；
#   codecs 照常现追加（它有自己的内容缓存）。键算不出就冷编；S2A_CS_STATIC_CACHE=0 强制冷编。
codecs_link() {
    bash "$cs_dir/../codecs/build.sh" >> "$out/static/link.txt"
    codec_count=$(grep -c 'libs2a_\(opus\|vpx\|avc\)\.a$' "$out/static/link.txt")
    [ "$codec_count" = 3 ] || { echo "Expected three codec archives, got $codec_count" >&2; exit 1; }
    echo "codecs: $codec_count archives linked"
    echo "NativeAOT services (static): $out/static/libs2a_cs.a"
}
cs_key=
if [ "${S2ARM_ANDROID_CS_SERVICES:-0}" = static ] && [ "${S2A_CS_STATIC_CACHE:-1}" != 0 ]; then
    cs_key=$(python3 "$cs_dir/static_cache.py" key --repo "$repo_dir" --extra "sdk=$sdk_version" --extra "rid=$sdk_rid" \
        --extra "ndk=$S2A_ANDROID_NDK" --extra "pathmap=/_/s2a") || cs_key=
    if [ -n "$cs_key" ]; then
        rm -rf "$out/jniLibs" "$out/assets"
        if python3 "$cs_dir/static_cache.py" restore --out "$out" --cache "$cache/cs-static" --key "$cs_key"; then
            echo "NativeAOT services (static): 缓存命中 ${cs_key}，跳过 dotnet publish"
            codecs_link
            exit 0
        fi
    fi
fi
"$dotnet" publish "$cs_dir/probe/S2A.Android.Probe.csproj" -c Release -r linux-bionic-arm64 \
    -p:PathMap="$pathmap" \
    -p:BaseOutputPath="$out/probe/bin/" -p:BaseIntermediateOutputPath="$out/probe/obj/" \
    -o "$out/probe/publish" -v quiet -nologo
# 复审第 5 条：还原出的 ILCompiler / runtime 包逐包对钉表 SHA-512 才准用。
python3 "$cs_dir/nuget_verify.py" --assets "$out/probe/obj/project.assets.json"
# jniLibs and assets are rebuilt from scratch each time: nothing stale from another mode or branch gets packaged.
rm -rf "$out/jniLibs" "$out/assets" && mkdir -p "$out/jniLibs/arm64-v8a" "$out/assets/licenses"
# Every mode links the .NET runtime pack into the APK: ship the license and third-party notices of
# the exact pack version this build restored (same lookup as services/nxaot.sh).
runtime_version=$(python3 -c 'import json, sys
j = json.load(open(sys.argv[1]))
print(next(d["version"].strip("[]").split(",")[0] for fw in j["project"]["frameworks"].values()
           for d in fw.get("downloadDependencies", []) if d["name"] == "Microsoft.NETCore.App.Runtime.NativeAOT.linux-bionic-arm64"))' \
    "$out/probe/obj/project.assets.json")
for notice in LICENSE.TXT THIRD-PARTY-NOTICES.TXT; do
    cp "$NUGET_PACKAGES/microsoft.netcore.app.runtime.nativeaot.linux-bionic-arm64/$runtime_version/$notice" \
        "$out/assets/licenses/dotnet-runtime-$notice"
done
cp "$out/probe/publish/libs2a_cs_probe.so" "$out/jniLibs/arm64-v8a/"
echo "NativeAOT probe: $out/jniLibs/arm64-v8a/libs2a_cs_probe.so"
# Opt-in (about 2.5 min cold, about +37 MB APK): the service layer + Ryujinx HLE + Vulkan backend.
#   S2ARM_ANDROID_CS_SERVICES=1       its own libs2a_cs.so; the preview starts that runtime
#   S2ARM_ANDROID_CS_SERVICES=static  a static library + build/android-cs/static/link.txt that
#                                     CMake links into libs2arm_android.so (one library, as on iOS)
# A process holds one NativeAOT runtime, so the static mode packages no other .NET library.
publish_services() {
    local log="$out/services-publish.log"
    # Roslyn reports a source path with line/column. NativeAOT may only report a method name.
    local warning='warning IL(2[0-9]{3}|3050):'
    local source='[/\\]S2A\.Services[/\\]SaveTransfer[^/\\:]*\.cs(\([0-9,]+\))?:'
    "$dotnet" publish "$cs_dir/aot/S2A.Android.Aot.csproj" -c Release -r linux-bionic-arm64 \
        -p:S2AAndroidHost=true -p:PathMap="$pathmap" -v quiet -nologo "$@" 2>&1 | tee "$log" || return
    if grep -E "$source .*$warning|$warning S2A\.Services\.SaveTransfer[.+]" "$log"; then
        echo "【红】存档服务 NativeAOT 裁剪或动态代码告警，拒绝发布" >&2
        return 1
    elif [ "$?" -ne 1 ]; then
        return 1
    fi
}
rm -rf "$out/jniLibs/arm64-v8a/libs2a_cs.so" "$out/static"
case ${S2ARM_ANDROID_CS_SERVICES:-0} in
0) ;;
1)
    publish_services -o "$out/aot/publish"
    python3 "$cs_dir/nuget_verify.py" --assets "$cs_dir/aot/obj/project.assets.json"
    cp "$out/aot/publish/libs2a_cs.so" "$out/jniLibs/arm64-v8a/"
    echo "NativeAOT services: $out/jniLibs/arm64-v8a/libs2a_cs.so" ;;
static)
    mkdir -p "$out/static"
    publish_services -p:NativeLib=Static -p:S2ALinkInputs="$out/static/link.txt" \
        -p:CppLibCreator="$ndk_bin/llvm-ar" -o "$out/static/publish" || { echo "dotnet publish (static) failed" >&2; exit 1; }
    python3 "$cs_dir/nuget_verify.py" --assets "$cs_dir/aot/obj/project.assets.json" || exit 1
    [ -s "$out/static/publish/libs2a_cs.a" ] || { echo "dotnet publish produced no/empty libs2a_cs.a" >&2; exit 1; }
    # CppLibCreator 必须指 NDK llvm-ar：macOS 上落到 BSD ar 会把 ELF 成员静默丢掉、产出空归档。
    # 当场校验成员全是 ELF64/AArch64（doctor.archive_valid 同一判据），别留到 CMake 链接才炸。
    python3 "$cs_dir/archive_check.py" "$out/static/publish/libs2a_cs.a" || exit 1
    cp "$out/static/publish/libs2a_cs.a" "$out/static/"
    rm -f "$out/jniLibs/arm64-v8a/libs2a_cs_probe.so"
    # The runtime archive carries its own LLVM libunwind internals (__unw_*, libunwind::...),
    # and the NDK links a different libunwind version for the host's C++ exceptions: in one
    # library they collide. Give the runtime's copy private names (definitions and
    # references, in copies of every listed input) so each side keeps its own unwinder.
    unw_syms=$out/static/unwind.syms
    rt=$(grep 'libRuntime.WorkstationGC.a$' "$out/static/link.txt")
    llvm-nm -A --defined-only --extern-only "$rt" \
        | awk -F: '$2 ~ /^(libunwind\.cpp|Unwind[A-Za-z0-9_-]*\.(S|c|cpp))\.o$/ {split($3, f, " "); print f[3]}' \
        | sort -u | awk '{print $1, "s2a_rt_" $1}' > "$unw_syms"
    [ -s "$unw_syms" ] || { echo "no libunwind symbols found in $rt" >&2; exit 1; }
    while read -r input; do
        case $input in
        -*) echo "$input" ;;
        *) copy=$out/static/$(basename "$input")
           llvm-objcopy --redefine-syms="$unw_syms" "$input" "$copy"
           echo "$copy" ;;
        esac
    done < "$out/static/link.txt" > "$out/static/link.private.txt"
    mv "$out/static/link.private.txt" "$out/static/link.txt"
    echo "runtime libunwind: $(wc -l < "$unw_syms") symbols made private"
    # 冷编产物进缓存（存不进只告警：下次照常冷编，不影响本次）
    if [ -n "$cs_key" ]; then
        python3 "$cs_dir/static_cache.py" save --out "$out" --cache "$cache/cs-static" --key "$cs_key" \
            || echo "[s2a] C# 静态库缓存存不进（下次冷编）" >&2
    fi
    # Opus / VP8 decoding (DirectPInvoke s2a_opus / s2a_vpx); also copies their licenses into assets.
    codecs_link ;;
*) echo "S2ARM_ANDROID_CS_SERVICES must be 0, 1 or static" >&2; exit 1 ;;
esac
