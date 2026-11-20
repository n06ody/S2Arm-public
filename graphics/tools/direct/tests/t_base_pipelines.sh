#!/bin/bash
# 拉取基础管线随包归档单测（graphics/tools/direct/base_pipelines.py + pack/stage_direct_cpso.sh；要本机 Metal 工具链与 Metal 4 GPU，约 30 秒）：bash graphics/tools/direct/tests/t_base_pipelines.sh
# 夹具：合成直连库目录。库容器（lib.idx + lib.000.pak，格式同 graphics/nvn/lib_pack_core.inc）：
#   顶点键 A、B、N、L 各有种类 55 拉取库（入口 main0 / main0_z，不带 [[stage_in]]，A 与 B 的 main0 函数体不同）与反射（种类 1）：A、B 带 pull {main0, slot 16}，
#   N 没有 pull，L 带 pull 但 writes_layer；片元键 P、Q、R 的种类 0 指向同一段批库（s<P>_main0 不声明函数常量、s<Q>_main0 声明一个带缺省的常量、s<R>_main0 不用），
#   反射 fn_prefix「s<键>_」+ entry main0。对的两个来源（base_pipelines.py read_pairs：并集、去重）：
#   离线配对表 shader_pairs.tsv：A P（两边都有）、B Q（只在配对表）、一行 # 注释、再一行 B Q（重复）；
#   种子表 pso_manifest.tsv 的 R 行：A P（两边都有）、A Q、B P（只在种子）、N P、L P、A M（M 不在库里）、再一行 A P（重复）、一行 # 注释、一行 S。
# 期望（不跑被测实现就写得出）：
#   ① --plat macos：出 4 个对文件，名字 = 「p」+ sha256("<顶点键>_b\t<片元键>_b\tmain0") 前 40 位 + .mtl4archive（shasum 现算，同设备 nvn_hybrid.inc hybrid_pkg_name）：
#      A×P（两边都有）、B×Q（只在配对表）、A×Q / B×P（只在种子）各一份，外加首查预热对 s2a_warm.metallib + s2a_warm.mtl4archive，没有 .lzfse（Mac 不压）；
#      读数「着色器对 7 对（只在配对表 1、只在种子 5、两边都有 1；跳过：顶点没有拉取库 1、写层号 1、片元不在库里 1），出 4 对」
#   ①b 归档只带本对两个函数：A×P 的归档里没有 s<Q>_main0 / s<R>_main0 / main0_z 这些串，读数「按整段库带函数 0 次」
#   ② 设备同一描述（t_base_pipelines.m：拉取库与批库整份载入、8 个颜色附件留空、采样 1）：A×P、A×Q（片元带常量 → 特化描述）、B×P、B×Q 都命中，且能按画布特化；
#      预热对按同一描述（s2a_warm_v + s2a_warm_f，随包那份库）命中
#   ③ 反证：A×P 的归档 + B 的顶点 / + Q 的片元 / + A 的 main0_z 不中；附件 1–7 写 Invalid（改之前的设备写法）不中；采样 4 不中；按上一个系统大版本出的不中
#   ④ 再跑同参数：缓存命中 4、现编 0、预热对缓存命中，文件逐字节相同
#   ⑤ metal-tt 失败（S2A_METAL_TT 换成退 1 的假工具、空缓存）：退出 3、0 个文件、读数「失败 4 对」「首查预热对 没编成」
#   ⑥ 阶段 pack/stage_direct_cpso.sh（假产物根、S2A_PACK_PLAT=macos、S2A_PSO_ARCHS=本机架构）：现行根 pipelines/ 里恰是这 4 个 .mtl4archive + 预热对两份（夹具没有计算键）、记键，
#      出的 A×P 按设备描述命中；缓存 5 条（4 对 + 预热对）落在直连缓存 store/cpso/
#   ⑦ --plat ios 26、架构 applegpu_g16p（出包目标表 pack/pso_targets.tsv 里的 iPhone 行；iOS 的 metal-tt 不收本机 Mac 架构）（metal-tt 换成转调真 metal-tt、把它写出的原样另存一份的包装）：出 4 个对文件 + 预热对，全是 .mtl4archive.lzfse、没有原样 .mtl4archive；
#      每份 compression_tool -decode 解开后与 metal-tt 写出的原样逐字节相同，且比原样小；读数「逐对 lzfse 压缩」；再跑缓存命中 4、文件逐字节相同。
#      iPhone 归档在本机打不开（11-19 实测：newArchiveWithURL 报「deployment target … is not compatible with current OS」），设备上解压后命中由 t_nvn_hybrid ⑦ 在本机用 Mac 归档的 .lzfse 测
#   ⑧ 全装（11-20：撤掉原件 10% 预算）：假 metal-tt 每对写 1000 字节、预热对写 100 字节，-j 1。夹具：顶点 V1–V8 都用 A 的拉取库，片元 P、Q；
#      种子表 R 行 V1×P，配对表 11 对（见下），并集 12 对：12 个对文件 + 预热对两份全部随包，读数「出 12 对」、随包 12100 + W 字节（W = 预热库字节），
#      读数里不再有「预算」「原件」字样。反证：把上限加回去（只出前 4 对）的实现，这条红。
set -u; H=$(cd "$(dirname "$0")/../../../.." && pwd); T=$(mktemp -d "${TMPDIR:-/tmp}/t_base_pipelines.XXXXXX"); trap 'rm -rf "$T"' EXIT
n=0; fail=0
ck() { n=$((n + 1)); if eval "$2"; then echo "✓ $1"; else echo "✗ $1"; fail=$((fail + 1)); fi; }
die() { echo "✗ $1"; echo "t_base_pipelines：0 项，失败 1"; exit 1; }
S2A_TC_QUIET=1 source "$H/tools/toolchain.sh" > /dev/null 2>&1 || die "统一工具链解析失败（tools/toolchain.sh）"
for t in metal metallib metal-tt; do xcrun -sdk macosx -f "$t" > /dev/null 2>&1 || die "本机没有 Metal 工具链（${t}）"; done
clang -fobjc-arc -O1 -o "$T/gpu_arch" "$H/graphics/tools/shbake/gpu_arch.m" -framework Metal -framework Foundation 2> /dev/null && ARCH=$("$T/gpu_arch") && [ -n "$ARCH" ] || die "取不到本机 GPU 架构"
clang -fobjc-arc -O1 -o "$T/probe" "$H/graphics/tools/direct/tests/t_base_pipelines.m" -framework Metal -framework Foundation 2> "$T/cc.log" || die "编探针失败：$(head -3 "$T/cc.log")"
MAJ=$(sw_vers -productVersion | cut -d. -f1)
[ "$MAJ" -ge 26 ] || die "本机 macOS ${MAJ} 没有 Metal 4（要 26 起）"
A=00000a0000000001 B=00000b0000000002 N=00000e0000000005 L=00000f0000000006 P=00000c0000000003 Q=00000c0000000004 R=00000c0000000007 M=00000d0000000008
F=$T/fx/shaders/direct; FI=$T/fxi/shaders/direct; mkdir -p "$F/b-t1" "$FI/b-t1"   # FI：同一夹具的 iPhone 版（库按 iphoneos 编，⑦ 用：iOS 的 metal-tt 不收 Mac 库）
SDK=macosx MIN=-mmacosx-version-min=15.0 SUF=
bp() {   # 拉取库：main0 读槽 16 的 float4 数组（乘 $2），main0_z 另一个函数体
  printf '#include <metal_stdlib>\nusing namespace metal;\nstruct VO { float4 p [[position]]; float4 a [[user(locn0)]]; };\n' > "$T/$1.metal"
  printf 'vertex VO main0(constant float4 *dm_lay [[buffer(16)]], uint v [[vertex_id]]) { VO o; o.p = float4(dm_lay[v].xy, 0, 1); o.a = dm_lay[v] * %s; return o; }\n' "$2" >> "$T/$1.metal"
  printf 'vertex VO main0_z(constant float4 *dm_lay [[buffer(16)]], uint v [[vertex_id]]) { VO o; o.p = float4(dm_lay[v].xy, 0.5, 1); o.a = dm_lay[v]; return o; }\n' >> "$T/$1.metal"
  xcrun -sdk $SDK metal $MIN -c "$T/$1.metal" -o "$T/$1$SUF.air" && xcrun -sdk $SDK metallib "$T/$1$SUF.air" -o "$T/$1$SUF.metallib"; }
cat > "$T/fb.metal" <<EOF
#include <metal_stdlib>
using namespace metal;
constant bool S2A_AT [[function_constant(7)]];
constant bool at_on = is_function_constant_defined(S2A_AT) ? S2A_AT : false;
struct F0 { float4 a [[user(locn0)]]; };
fragment float4 s${P}_main0(F0 i [[stage_in]]) { return i.a; }
fragment float4 s${Q}_main0(F0 i [[stage_in]]) { if (at_on && i.a.w < 0.5) discard_fragment(); return i.a * 0.5; }
fragment float4 s${R}_main0(F0 i [[stage_in]]) { return i.a.zyxw; }
EOF
fx() {   # 按 $SDK 编夹具库、写库容器与两张表到 $1
bp vA 1.0 && bp vB 2.0 && bp vN 3.0 && bp vL 4.0 || die "夹具拉取库编不出"
xcrun -sdk $SDK metal $MIN -c "$T/fb.metal" -o "$T/fb$SUF.air" && xcrun -sdk $SDK metallib "$T/fb$SUF.air" -o "$T/fb$SUF.metallib" || die "夹具批库编不出"
F=$1
: > "$F/shader_map.tsv"   # 没有计算键：⑥ 阶段里计算管线那半出 0 支
# 管线种子表（对的来源，base_pipelines.py read_pairs）：R 行 = pso_skey 十六进制，前 24 字节顶点键串「<键>_b」、再 24 字节片元键串，余下补零到 432 字节
rrow() { python3 -c 'import sys; v, f = sys.argv[1:3]; print("R\t" + ((v + "_b").encode().ljust(24, b"\0") + (f + "_b").encode().ljust(24, b"\0") + bytes(384)).hex())' "$1" "$2"; }
{ echo '# 注释'; for pr in "$A $P" "$A $Q" "$B $P" "$N $P" "$L $P" "$A $M" "$A $P"; do rrow $pr; done; printf 'S\t%s_b\t0\n' $A; } > "$F/pso_manifest.tsv"
printf '%s\t%s\n# 注释\n%s\t%s\n%s\t%s\n' $A $P $B $Q $B $Q > "$F/shader_pairs.tsv"   # 离线配对表：A×P 两边都有、B×Q 只在这里（重复一行）
python3 - "$F/b-t1" "$T" "$SUF" $A $B $N $L $P $Q $R <<'EOF'
import json, struct, sys
out, T, SUF = sys.argv[1:4]; A, B, N, L, P, Q, R = sys.argv[4:]
pak, recs = bytearray(), []
def put(key, kind, data):
    recs.append((int(key, 16), kind, len(pak), len(data))); pak.extend(data)
pull = {'entry': 'main0', 'entry_z': 'main0_z', 'slot': 16, 'inputs': [0]}
for k, tag, j in ((A, 'vA', {'pull': pull}), (B, 'vB', {'pull': pull}), (N, 'vN', {}), (L, 'vL', {'pull': pull, 'writes_layer': True})):
    put(k, 55, open(f'{T}/{tag}{SUF}.metallib', 'rb').read())
    put(k, 1, json.dumps(dict(j, stage='vs', entry='main0', entry_z='main0_z', fn_prefix=f's{k}_')).encode())
seg = open(f'{T}/fb{SUF}.metallib', 'rb').read(); off = len(pak); pak.extend(seg)
for k in (P, Q, R):
    recs.append((int(k, 16), 0, off, len(seg)))
    put(k, 1, json.dumps({'stage': 'ps', 'entry': 'main0', 'fn_prefix': f's{k}_'}).encode())
recs.sort()
hd = bytearray(48); hd[:8] = b'S2ALIB\0\0'; struct.pack_into('<III', hd, 8, 1, len(recs), 1); hd[24:26] = b't1'
idx = hd + struct.pack('<QQ', len(pak), 0) + b''.join(struct.pack('<QBBHIQ', k, kind, 0, 0, ln, off) for k, kind, off, ln in recs)
open(out + '/lib.idx', 'wb').write(idx); open(out + '/lib.000.pak', 'wb').write(pak)
EOF
[ -s "$F/b-t1/lib.idx" ] || die "夹具库容器没写出来"
}
FM=$F; SDK=iphoneos MIN=-mios-version-min=26.0 SUF=.ios fx "$FI"; SDK=macosx MIN=-mmacosx-version-min=15.0 SUF= fx "$FM"; F=$FM
pfile() { printf '%s_b\t%s_b\tmain0' "$1" "$2" | shasum -a 256 | cut -c1-40 | sed 's/^/p/;s/$/.mtl4archive/'; }
FAP=$(pfile $A $P) FAQ=$(pfile $A $Q) FBP=$(pfile $B $P) FBQ=$(pfile $B $Q)
WF='s2a_warm.metallib s2a_warm.mtl4archive'   # 首查预热对（Mac 原样）
want() { printf '%s\n' "$FAP" "$FAQ" "$FBP" "$FBQ" $WF | sort; }
BP() { python3 "$H/graphics/tools/direct/base_pipelines.py" "$F" "$1" --plat macos "${@:2}"; }
probe() { "$T/probe" "$1" "$T/v$2.metallib" "$3" "$T/fb.metallib" "s$4_main0" "${5:-u}" "${6:-1}" 2>> "$T/probe.err"; }   # 打不开的原因进 probe.err，输出只留一行

# ① ①b ②
o=$(BP "$T/o1" --os "$MAJ" --arch "$ARCH" --cache "$T/cache" -j 2 2>&1); r=$?
ck "① 退出 0、恰好 A×P / B×Q / A×Q / B×P 4 个设备名 + 预热对两份、没有 .lzfse（实：$(ls "$T/o1" 2> /dev/null | tr '\n' ' ')）" '[ $r = 0 ] && [ "$(ls "$T/o1" | sort)" = "$(want)" ]'
ck "① 读数：7 对（只在配对表 1、只在种子 5、两边都有 1）、跳过 3、出 4 对、原样不压（$(tail -1 <<< "$o" | cut -c1-200)）" \
   '[[ "$(tail -1 <<< "$o")" == *"着色器对 7 对（只在配对表 1、只在种子 5、两边都有 1；跳过：顶点没有拉取库 1、写层号 1、片元不在库里 1），出 4 对"* ]] && [[ "$(tail -1 <<< "$o")" == *"原样不压"* ]]'
ck "①b A×P 的归档只带本对函数：有 s<P>_main0，没有 s<Q>_main0 / s<R>_main0 / main0_z；读数「按整段库带函数 0 次」" \
   'grep -aq "s${P}_main0" "$T/o1/$FAP" && ! grep -aq "s${Q}_main0" "$T/o1/$FAP" && ! grep -aq "s${R}_main0" "$T/o1/$FAP" && ! grep -aq "main0_z" "$T/o1/$FAP" && [[ "$(tail -1 <<< "$o")" == *"按整段库带函数 0 次"* ]]'
ck "② A×P：命中、能特化（$(probe "$T/o1/$FAP" A main0 $P)）" '[ "$(probe "$T/o1/$FAP" A main0 $P)" = "hit=1 spec=1" ]'
ck "② A×Q（片元声明函数常量 → 特化描述）：命中、能特化（$(probe "$T/o1/$FAQ" A main0 $Q)）" '[ "$(probe "$T/o1/$FAQ" A main0 $Q)" = "hit=1 spec=1" ]'
ck "② B×P：命中、能特化（$(probe "$T/o1/$FBP" B main0 $P)）" '[ "$(probe "$T/o1/$FBP" B main0 $P)" = "hit=1 spec=1" ]'
ck "② B×Q（只在配对表的对）：命中、能特化（$(probe "$T/o1/$FBQ" B main0 $Q)）" '[ "$(probe "$T/o1/$FBQ" B main0 $Q)" = "hit=1 spec=1" ]'
wprobe() { "$T/probe" "$1/s2a_warm.mtl4archive" "$1/s2a_warm.metallib" s2a_warm_v "$1/s2a_warm.metallib" s2a_warm_f u 1 2>> "$T/probe.err"; }
ck "② 首查预热对按设备描述命中（$(wprobe "$T/o1")）" '[ "$(wprobe "$T/o1")" = "hit=1 spec=1" ]'
# ③
ck "③ 反证：A×P 的归档 + B 的顶点不中（$(probe "$T/o1/$FAP" B main0 $P)）" '[ "$(probe "$T/o1/$FAP" B main0 $P)" = "hit=0 spec=-" ]'
ck "③ 反证：A×P 的归档 + Q 的片元不中（$(probe "$T/o1/$FAP" A main0 $Q)）" '[ "$(probe "$T/o1/$FAP" A main0 $Q)" = "hit=0 spec=-" ]'
ck "③ 反证：A×P 的归档 + A 的 main0_z 不中（$(probe "$T/o1/$FAP" A main0_z $P)）" '[ "$(probe "$T/o1/$FAP" A main0_z $P)" = "hit=0 spec=-" ]'
ck "③ 反证：附件 1–7 写 Invalid 的描述不中（$(probe "$T/o1/$FAP" A main0 $P i)）" '[ "$(probe "$T/o1/$FAP" A main0 $P i)" = "hit=0 spec=-" ]'
ck "③ 反证：采样 4 的描述不中（$(probe "$T/o1/$FAP" A main0 $P u 4)）" '[ "$(probe "$T/o1/$FAP" A main0 $P u 4)" = "hit=0 spec=-" ]'
BP "$T/o2" --os $((MAJ - 1)) --arch "$ARCH" > /dev/null 2>&1
ck "③ 反证：按系统大版本 $((MAJ - 1)) 出的不中（$(probe "$T/o2/$FAP" A main0 $P)）" '[ -s "$T/o2/$FAP" ] && [ "$(probe "$T/o2/$FAP" A main0 $P)" = "hit=0 spec=-" ]'
# ④
o=$(BP "$T/o3" --os "$MAJ" --arch "$ARCH" --cache "$T/cache" 2>&1); r=$?
same() { local f; for f in $(want); do cmp -s "$1/$f" "$2/$f" || return 1; done; }
ck "④ 再跑：缓存命中 4、现编 0、预热对缓存命中、文件逐字节相同（$(tail -1 <<< "$o" | grep -o '缓存命中[^）]*')；$(tail -1 <<< "$o" | grep -o '首查预热对[^；]*')）" \
   '[ $r = 0 ] && [[ "$o" == *"缓存命中 4、现编 0"* ]] && [[ "$o" == *"首查预热对 1 份（缓存命中）"* ]] && same "$T/o1" "$T/o3"'
# ⑤
printf '#!/bin/sh\necho "fake metal-tt: broken" >&2; exit 1\n' > "$T/badtt"; chmod +x "$T/badtt"
o=$(S2A_METAL_TT=$T/badtt BP "$T/o4" --os "$MAJ" --arch "$ARCH" --cache "$T/cache4" 2>&1); r=$?
ck "⑤ metal-tt 失败：退出 3、0 个文件、读数失败 4 对、预热对没编成（退出 ${r}，$(ls "$T/o4" 2> /dev/null | wc -l | tr -d ' ') 个文件）" \
   '[ $r = 3 ] && [ -z "$(ls "$T/o4")" ] && [[ "$(tail -1 <<< "$o")" == *"失败 4 对；首查预热对 没编成"* ]]'
# ⑥ 阶段：假产物根（只有直连库目录与直连库阶段键），私有根
RT=$T/roots; mkdir -p "$RT/fx@v1/shaders"; cp -R "$F" "$RT/fx@v1/shaders/"; echo k1 > "$RT/fx@v1/.stage_direct"; printf 'title_id\t0100aaaa00000000\n' > "$RT/fx@v1/unpack.tsv"; ln -s fx@v1 "$RT/fx@现行"
cur() { cd -P "$RT/fx@现行" && pwd; }
stage() { S2A_ROOTS=$RT S2A_ROOTS_SHARED=$T/shared S2A_RUN_ROOT=$T/run S2A_PACK_PLAT=macos S2A_MAC_OS=$MAJ S2A_PSO_ARCHS=$ARCH J=2 bash "$H/pack/stage_direct_cpso.sh" fx 2>&1; }
o=$(stage); r=$?; [ $r = 0 ] || echo "$o" | tail -5
ck "⑥ 阶段：退出 ${r}、现行根 pipelines/ 恰是这 4 个 + 预热对两份、记了键（$(grep -o '拉取基础管线归档[^；]*' <<< "$o" | head -1 | cut -c1-100)）" \
   '[ $r = 0 ] && [ "$(ls "$(cur)/shaders/direct/pipelines" 2> /dev/null | sort)" = "$(want)" ] && [ -s "$(cur)/.stage_direct_cpso" ]'
ck "⑥ 阶段出的 A×P 按设备描述命中（$(probe "$(cur)/shaders/direct/pipelines/$FAP" A main0 $P)）、缓存 5 条（4 对 + 预热对）落在直连缓存 store/cpso/" \
   '[ "$(probe "$(cur)/shaders/direct/pipelines/$FAP" A main0 $P)" = "hit=1 spec=1" ] && [ "$(find "$T/run/s2arm/cache/direct/0100AAAA00000000/store/cpso" -name "*.e" | wc -l | tr -d " ")" = 5 ]'
# ⑦ iPhone 包：逐对 lzfse。metal-tt 换成包装：转调真 metal-tt，把它写出的原样（-o 那份）另存到 $T/raw/ 作对照
mkdir -p "$T/raw"
printf '#!/bin/bash\nxcrun -sdk iphoneos metal-tt "$@"; rc=$?\nwhile [ $# -gt 1 ]; do [ "$1" = -o ] && { [ -f "$2" ] && cp "$2" "%s/raw/$(basename "$2")"; break; }; shift; done\nexit $rc\n' "$T" > "$T/teett"; chmod +x "$T/teett"
o=$(S2A_METAL_TT=$T/teett python3 "$H/graphics/tools/direct/base_pipelines.py" "$FI" "$T/o5" --plat ios --os 26 --arch applegpu_g16p --cache "$T/cache5" 2>&1); r=$?
wanti() { printf '%s.lzfse\n' "$FAP" "$FAQ" "$FBP" "$FBQ" s2a_warm.mtl4archive; echo s2a_warm.metallib; }
ck "⑦ --plat ios：退出 ${r}、恰好 4 个对 + 预热对的 .mtl4archive.lzfse 与预热库、没有原样 .mtl4archive（实：$(ls "$T/o5" 2> /dev/null | tr '\n' ' ' | cut -c1-200)）" \
   '[ $r = 0 ] && [ "$(ls "$T/o5" | sort)" = "$(wanti | sort)" ] && [ -z "$(ls "$T/o5" | grep "\.mtl4archive$")" ]'
unz_ok() {   # 每份解开 = metal-tt 写出的原样（包装另存的 <名>.bin），且压过的比原样小
  local f n=0 b
  for f in "$T"/o5/*.lzfse; do b=$(basename "$f" .mtl4archive.lzfse); [ -s "$T/raw/$b.bin" ] || return 1
    compression_tool -decode -a lzfse -i "$f" -o "$T/unz.$b" 2> /dev/null && cmp -s "$T/unz.$b" "$T/raw/$b.bin" && [ "$(wc -c < "$f")" -lt "$(wc -c < "$T/raw/$b.bin")" ] || return 1; n=$((n + 1)); done
  [ $n = 5 ]; }
ck "⑦ 5 份 .lzfse 用 compression_tool 解开都与 metal-tt 写出的原样逐字节相同、且比原样小（$(du -k "$T/raw" | tail -1 | cut -f1) KB 原样 → $(cat "$T"/o5/*.lzfse | wc -c | tr -d ' ') 字节）；读数「逐对 lzfse 压缩」" \
   'unz_ok && [[ "$(tail -1 <<< "$o")" == *"逐对 lzfse 压缩"* ]]'
o=$(S2A_METAL_TT=$T/teett python3 "$H/graphics/tools/direct/base_pipelines.py" "$FI" "$T/o6" --plat ios --os 26 --arch applegpu_g16p --cache "$T/cache5" 2>&1); r=$?
samei() { local f; for f in $(wanti); do cmp -s "$T/o5/$f" "$T/o6/$f" || return 1; done; }
ck "⑦ --plat ios 再跑：缓存命中 4、现编 0、文件逐字节相同（$(tail -1 <<< "$o" | grep -o '缓存命中[^）]*')）" '[ $r = 0 ] && [[ "$o" == *"缓存命中 4、现编 0"* ]] && samei'
# ⑧ 全装：假 metal-tt 写已知字节数，夹具 FB（库容器用 A 的拉取库与同一段批库，键 V1–V8 / P / Q）
V1=0000010000000001 V2=0000010000000002 V3=0000010000000003 V4=0000010000000004 V5=0000010000000005 V6=0000010000000006 V7=0000010000000007 V8=0000010000000008
FB=$T/fb/shaders/direct; mkdir -p "$FB/b-t1"
python3 - "$FB/b-t1" "$T" $P $Q $V1 $V2 $V3 $V4 $V5 $V6 $V7 $V8 <<'EOF'
import json, struct, sys
out, T, P, Q = sys.argv[1:5]; vs = sys.argv[5:]
pak, recs = bytearray(), []
def put(key, kind, data):
    recs.append((int(key, 16), kind, len(pak), len(data))); pak.extend(data)
pull = {'entry': 'main0', 'entry_z': 'main0_z', 'slot': 16, 'inputs': [0]}
va = open(f'{T}/vA.metallib', 'rb').read()
for k in vs:
    put(k, 55, va); put(k, 1, json.dumps({'pull': pull, 'stage': 'vs', 'entry': 'main0', 'entry_z': 'main0_z', 'fn_prefix': f's{k}_'}).encode())
seg = open(f'{T}/fb.metallib', 'rb').read(); off = len(pak); pak.extend(seg)
for k in (P, Q):
    recs.append((int(k, 16), 0, off, len(seg))); put(k, 1, json.dumps({'stage': 'ps', 'entry': 'main0', 'fn_prefix': f's{k}_'}).encode())
recs.sort()
hd = bytearray(48); hd[:8] = b'S2ALIB\0\0'; struct.pack_into('<III', hd, 8, 1, len(recs), 1); hd[24:26] = b't1'
idx = hd + struct.pack('<QQ', len(pak), 0) + b''.join(struct.pack('<QBBHIQ', k, kind, 0, 0, ln, off) for k, kind, off, ln in recs)
open(out + '/lib.idx', 'wb').write(idx); open(out + '/lib.000.pak', 'wb').write(pak)
EOF
{ printf 'package\tindex\tstage\tkey\tsource\n'
  printf 'vs_x.bin\t0\tvs\t%s\t/d/u.bundle:CAB-1#Seed/S#var19.0\n' $V1
  printf 'vs_x.bin\t%s\tvs\t%s\t/e/a.ptcl#BNSH@0x1000#var%s\n' 1 $V2 0 2 $V3 1 3 $V4 2
  printf 'vs_x.bin\t%s\tvs\t%s\t/e/b.ptcl#BNSH@0x2000#var%s\n' 4 $V5 0 5 $V8 1
  printf 'vs_x.bin\t6\tvs\t%s\tmain#GLSLC@0x10\n' $V6
  printf 'ps_x.bin\t%s\tps\t%s\t/e/a.ptcl#BNSH@0x1000#var%s\n' 0 $P 0 1 $Q 1; } > "$FB/shader_map.tsv"
{ echo '# 种子：只有 V1×P'; rrow $V1 $P; } > "$FB/pso_manifest.tsv"
printf '%s\t%s\n' $V1 $Q $V7 $Q $V6 $P $V6 $Q $V5 $P $V5 $Q $V8 $P $V2 $P $V3 $P $V4 $P $V2 $Q > "$FB/shader_pairs.tsv"
printf '#!/bin/bash\nwhile [ $# -gt 1 ]; do case $1 in -o) o=$2;; *.mtl4-json) js=$1;; esac; shift; done\nn=1000; grep -q s2a_warm_v "$js" && n=100\nhead -c $n /dev/zero > "$o"\n' > "$T/sizett"; chmod +x "$T/sizett"
BB() { S2A_METAL_TT=$T/sizett python3 "$H/graphics/tools/direct/base_pipelines.py" "$FB" "$1" --plat macos --os "$MAJ" --arch "$ARCH" -j 1 "${@:2}"; }
grp() { local v f; for pr in "$@"; do v=${pr% *} f=${pr#* }; pfile $v $f; done; }
S_G="$V1 $P|$V1 $Q" V7_G="$V7 $Q" V6_G="$V6 $P|$V6 $Q" B_G="$V5 $P|$V5 $Q|$V8 $P" A_G="$V2 $P|$V3 $P|$V4 $P|$V2 $Q"
files() { local IFS='|'; grp $* | sort; }   # 这些组的对文件名
pfiles() { ls "$1" 2> /dev/null | grep '^p' | sort; }
o=$(BB "$T/b1" 2>&1); r=$?
W=$(wc -c < "$T/b1/s2a_warm.metallib" 2> /dev/null | tr -d ' '); W=${W:-0}
ck "⑧ 全装：退出 ${r}、12 个对文件 + 预热对两份、随包 $((12100 + W)) 字节（W=${W}）、读数没有预算字样（$(tail -1 <<< "$o" | grep -o '出 [0-9]* 对[^；]*')）" \
   '[ $r = 0 ] && [ "$(pfiles "$T/b1")" = "$(files "$S_G|$V7_G|$V6_G|$B_G|$A_G")" ] && [ -s "$T/b1/s2a_warm.mtl4archive" ] && [ "$W" -gt 0 ] \
    && [[ "$(tail -1 <<< "$o")" == *"出 12 对"* ]] && [[ "$o" == *"随包共 $((12100 + W)) 字节"* ]] && [[ "$(tail -1 <<< "$o")" != *预算* ]] && [[ "$(tail -1 <<< "$o")" != *原件* ]]'
echo "t_base_pipelines：$n 项，失败 $fail"
[ $fail = 0 ]
