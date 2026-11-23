#!/bin/bash
# 出包 Vulkan 产物独立入口 dsh_vklib 单测（#783，graphics/tools/direct/vklib/VkLib.cs；Linux 约 2 分钟，含现编 SPIRV-Cross）：bash graphics/tools/direct/tests/t_dsh_vklib.sh
#   原创合成着色器（VS / PS / CS，同 dsh_vk 的夹具）打成假着色器包 + 配置档，整批走 VkLib.Build，核 ① vk_build.tsv 逐键状态 ② 输出只有 _b.json / _b.spv
#   ③ 每个 _b.spv 与直接调 DshVk.CompileWithSpv 的逐字节相同 ④ spirv-val ⑤ 返回值与 vk_build.tsv 一致；真几何键出 _vg0/1/3、几何包里的顶点代码 skipped、记录越界 failed（见 dsh_vklib/Program.cs 文件头）。
#   Vulkan 侧指纹（vklib/VkFp.targets，#783 直连线 11-02 答 a、#1419 审 1）：⑥ 编进工具的源码部分 VkFp.V = VkFpPrint；⑦ 拷到别的目录不变，DshVk.cs / VkLib.cs / VkFp.targets /
#   工程文件 / 与 dsh_batch 共用的 PkgRead.cs 与 LibPack.cs / libdsx 的 gal_spill.c / 链锁 dmsh_chain.lock / SPIRV-Cross 源码任一份改一字节、vklib/ 里新加源码都变，往 bin/ obj/ 和点文件里加东西不变；
#   ⑨ 完整指纹 VkLib.Fp = 公式（源码部分，S2A.Dsh 翻译器指纹 Xfp），只改翻译器本体（dsh/Dsh.cs，FILES 与 DshXfpInput 里都有）→ Xfp 变、VkLib.Fp 变；
#   ⑧ 苹果侧三处指纹（xlate_fp 的 FILES、DshXfpInput、direct.tsv 的「入」）都不含 vklib/；⑩ VkLib.Pack 打出的离线 Vulkan 库按格式核（见 dsh_vklib/Program.cs 文件头）。
#   direct.tsv 只禁「入」：接进出包时 vklib/ 记「外」、VkFp 单独进安卓目标的直连阶段键（同 gfx_api.py 记「外」、类别另进键），苹果键不动，
#   这样改了 Vulkan 侧代码，安卓的离线库会重出，而不是留着旧库、运行时整库不用。
#   工具与 libdsx 的取法同 t_dsh_vk.sh。
set -u
R=$(cd "$(dirname "$0")/../../../.." && pwd); H=$R/graphics/tools/direct/tests
# 取物理路径：macOS 的 /var 是 /private/var 的软链，msbuild 一边解开软链一边不解，⑨ 的「指纹输入在仓库里」前缀比对会对不上（Linux 无此软链，作者机上绿）
T=$(cd "$(mktemp -d "${TMPDIR:-/tmp}/t_dsh_vklib.XXXXXX")" && pwd -P); trap 'rm -rf "$T"' EXIT
red() { echo "🔴 出包 Vulkan 独立入口：$1"; exit 1; }
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
"$DN" build "$H/dsh_vklib/S2A.DshVkLibTest.csproj" -c Release -p:ArtifactsPath="$T/art" -p:NuGetAudit=false --verbosity quiet > "$T/build.log" 2>&1 \
  || red "测试工程编不过：$(grep -m1 ' error ' "$T/build.log" | cut -c1-200)"
"$DN" "$T/art/bin/S2A.DshVkLibTest/release/S2A.DshVkLibTest.dll" "$XC" "$VAL" "$T/work" --dsx "$LIB" > "$T/test.out" 2>&1; bad=$?
cat "$T/test.out"
ck() { if [ "$2" = "$3" ]; then echo "✓ $1"; else echo "✗ $1：期望 $2，实际 $3"; bad=1; fi; }
ne() { if [ -n "$2" ] && [ -n "$3" ] && [ "$2" != "$3" ]; then echo "✓ $1"; else echo "✗ $1：改前 $2，改后 $3"; bad=1; fi; }
fp() { "$DN" msbuild "$1/graphics/tools/direct/vklib/S2A.DshVkLib.csproj" -t:VkFpPrint -nologo 2> /dev/null | sed -n 's/^ *VKFP=//p'; }
F0=$(fp "$R")
[[ $F0 =~ ^[0-9a-f]{16}$ ]] || red "VkFpPrint 没打出 16 位指纹（实 ${F0}）"
ck "⑥ 编进工具的源码部分与 VkFpPrint 相同（${F0}）" "$F0" "$(sed -n 's/^VKFP_SRC=//p' "$T/test.out")"
C=$T/fpcopy; V=$C/graphics/tools/direct/vklib
copy() { rm -rf "$C" && mkdir -p "$C/graphics/tools/direct/dsh/batch" "$C/graphics/metal" "$C/graphics/dsx" "$C/translator" "$C/graphics/third_party" && cp "$R/graphics/tools/direct/dsh/DshVk.cs" "$C/graphics/tools/direct/dsh/" \
  && cp "$R/graphics/tools/direct/dsh/batch/PkgRead.cs" "$R/graphics/tools/direct/dsh/batch/LibPack.cs" "$C/graphics/tools/direct/dsh/batch/" \
  && cp -R "$R/graphics/tools/direct/vklib" "$V" && rm -rf "$V/bin" "$V/obj" && cp "$R"/graphics/metal/gal_abi.h "$C/graphics/metal/" && cp "$R"/graphics/dsx/dsx_abi.h "$R"/graphics/dsx/gal_spvx.[ch] "$R"/graphics/dsx/gal_spill.[ch] "$R"/graphics/dsx/gal_colorround.[ch] "$C/graphics/dsx/" \
  && cp "$R/translator/dmsh_chain.lock" "$C/translator/" && cp -R "$R/graphics/third_party/spirv_cross" "$C/graphics/third_party/" || red "拷不出指纹输入到 $C"
  mkdir -p "$C/android/native/direct" "$C/android/native/nvn" "$C/graphics/nvn" &&
    cp "$R"/android/native/direct/{libpack*.c,libpack*.h,libpack_stage*.inc,jv.*,json.*,spirv_workgroup.h,spirv_storage.h,graphics.h,s2a_direct.h,sha256.*} "$C/android/native/direct/" &&
    cp "$R"/android/native/nvn/nvn_vk_program_result.* "$C/android/native/nvn/" &&
    cp "$R/graphics/nvn/lib_pack_core.inc" "$C/graphics/nvn/" || red "拷不出 Vulkan 阶段格式输入"; }
copy; ck "⑦ 同一份源码拷到别的目录 → 指纹相同" "$F0" "$(fp "$C")"
for f in graphics/tools/direct/dsh/DshVk.cs graphics/tools/direct/dsh/batch/PkgRead.cs graphics/tools/direct/dsh/batch/LibPack.cs graphics/tools/direct/vklib/VkLib.cs graphics/tools/direct/vklib/VkFp.targets graphics/tools/direct/vklib/S2A.DshVkLib.csproj graphics/dsx/gal_spill.c translator/dmsh_chain.lock graphics/third_party/spirv_cross/spirv_msl.cpp; do
  copy; case $f in *.cs|*.c|*.cpp) echo '// x' >> "$C/$f";; *.lock) echo '# x' >> "$C/$f";; *) echo '<!-- x -->' >> "$C/$f";; esac
  ne "⑦ 改 ${f#graphics/} → 指纹变" "$F0" "$(fp "$C")"
done
copy; echo '// x' > "$V/Extra.cs"; ne "⑦ vklib/ 里新加一份源码 → 指纹变" "$F0" "$(fp "$C")"
copy; mkdir -p "$V/bin" "$V/obj" && echo x > "$V/bin/x" && echo x > "$V/obj/x" && echo x > "$V/.x" || red "写不进 $V"; ck "⑦ 往 bin/ obj/ 与点文件里加东西 → 指纹不变" "$F0" "$(fp "$C")"
# ⑨ 翻译器本体：在 S2A.Dsh 的最小副本上跑它自己的 DshChainConst（只算 Xfp、不编），改一字节 dsh/Dsh.cs 前后各一次；完整指纹按 VkLib.Fp 的公式重算
fpf() { python3 -c 'import hashlib, sys; print(hashlib.sha256(("vkfp 1\0" + sys.argv[1] + "\0" + sys.argv[2]).encode()).hexdigest()[:16])' "$1" "$2"; }
ck "⑨ 完整指纹 = 公式（源码部分 ${F0}，翻译器指纹 $(sed -n 's/^XFP=//p' "$T/test.out")）" "$(fpf "$F0" "$(sed -n 's/^XFP=//p' "$T/test.out")")" "$(sed -n 's/^VKFP=//p' "$T/test.out")"
X=$T/xfpcopy; D=$X/graphics/tools/direct/dsh
# 最小副本 = 翻译器指纹的权威输入清单（dmsh_chain.xfp_files()，即 DshXfpInput）+ csproj 本身，不手挑（11-04：手挑清单漏了后加的 batch/BuildXlate.cs，⑨ 读空）
mkdir -p "$D" "$X/graphics/nvn/gfx/nvn" && cp "$R/graphics/tools/direct/dsh/S2A.Dsh.csproj" "$D/" && cp "$R/graphics/nvn/gfx/nvn/dmsh_chain.h" "$X/graphics/nvn/gfx/nvn/" || red "拷不出 S2A.Dsh.csproj / dmsh_chain.h（csproj 求值时读 DshChainHeader）到 $X"
while IFS= read -r f; do [ -n "$f" ] || continue; mkdir -p "$X/$(dirname "$f")" && cp "$R/$f" "$X/$f" || red "拷不出指纹输入 $f 到 $X"; done < <(cd "$R/translator" && python3 -c 'import dmsh_chain; print("\n".join(dmsh_chain.xfp_files()))')
xfp() { "$DN" msbuild "$D/S2A.Dsh.csproj" -t:DshChainConst -nologo -p:IntermediateOutputPath="$X/obj/" > "$X/m.log" 2>&1 && sed -n 's/.*Xfp = "\([0-9a-f]*\)".*/\1/p' "$X/obj/DshChain.g.cs"; }
X0=$(xfp); echo '// x' >> "$D/Dsh.cs"; X1=$(xfp)
ne "⑨ 只改翻译器本体 dsh/Dsh.cs → S2A.Dsh 翻译器指纹变" "$X0" "$X1"
ne "⑨ 只改翻译器本体 dsh/Dsh.cs → 完整指纹变（源码部分不变）" "$(fpf "$F0" "$X0")" "$(fpf "$F0" "$X1")"
XF=$(cd "$R/translator" && python3 -c 'import dmsh_chain; print("\n".join(dmsh_chain.xfp_files()))')
ck "⑧ xlate_fp 输入含 dsh/Dsh.cs（正对照）" 1 "$(grep -c '/dsh/Dsh\.cs$' <<< "$XF")"
ck "⑧ xlate_fp 输入不含 vklib/" 0 "$(grep -c '/vklib/' <<< "$XF")"
XI=$(cd "$R/graphics/tools/direct/dsh" && "$DN" msbuild S2A.Dsh.csproj -getItem:DshXfpInput 2> /dev/null | python3 -c 'import json, sys; print("\n".join(i["FullPath"] for i in json.load(sys.stdin)["Items"]["DshXfpInput"]))') || XI=""
ck "⑧ 翻译器指纹 DshXfpInput 含 Dsh.cs（正对照）" 1 "$(grep -c '/dsh/Dsh\.cs$' <<< "$XI")"
ck "⑧ 翻译器指纹 DshXfpInput 不含 vklib/" 0 "$(grep -c '/vklib/' <<< "$XI")"
ck "⑧ 出包直连阶段键的「入」里没有 vklib/（只看第二列路径；「外」可以）" 0 "$(LC_ALL=C awk -F'\t' '$1 == "入" && $2 ~ /\/vklib\//' "$R/pack/stage_inputs/direct.tsv" | wc -l | tr -d ' ')"
# 用真实 CLI 子进程验证逐键失败 / 全成功的退出码（带和不带 --lib）。
python3 "$H/t_vklib_exit.py" --dotnet "$DN" --dll "$T/art/bin/S2A.DshVkLib/release/dsh_vklib.dll" \
  --dsx "$LIB" --spirv-cross "$XC" || bad=1
[ $bad = 0 ] && echo "dsh_vklib: 全过" || echo "dsh_vklib: 有不通"; exit $bad
