#!/bin/bash
# 资源包 v2 + 核对 + 版本化产物根单测（临时目录，秒级）：bash tests/pack_test.sh
# 期望按规则手写：点开头的真文件进包、macOS 垃圾不进；--check 最新退 0、源变了退 1；重打只重算变了的 1 个；删源文件包里也删；
# 包里改字节 / 多文件 → 核对红；产物根：提交后现行指向新版、新版写入不影响旧版（克隆独立）、同版本不许重开、只留 3 版、现行里有软链拒绝开新版
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); bad=0
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
source "$H/pack/ipa_key.sh"
# 包键（审计 P1-54）：进包输入 romfs / aoc / 系统数据变了键必须变；CFLAGS 不设与设成空同键（旧口径）。夹具是最小产物根，期望按「变没变」判，不复述键的拼法
key_commit=$(git -C "$H" rev-parse --short=9 HEAD) || exit 1
key_fixture=$T/key_fixture; mkdir -p "$key_fixture/exefs" "$key_fixture/romfs" "$key_fixture/aoc"
printf 'main' > "$key_fixture/exefs/main"; printf 'AAAA' > "$key_fixture/romfs/a.bin"; printf 'DLC1' > "$key_fixture/aoc/x.bin"
for s_ in rewrite shaders uilayout romfsidx sysdata; do printf '%s fixture\n' "$s_" > "$key_fixture/.stage_$s_"; done
kf() { ipa_pkg_key "$key_commit" "$key_fixture"; }
k0=$(unset CFLAGS; kf); k0e=$(CFLAGS= kf)
ck "CFLAGS 未设与显式空：同一输入同一包键" '[ -n "$k0" ] && [ "$k0" = "$k0e" ]'
printf 'BAAA' > "$key_fixture/romfs/a.bin"; k1=$(unset CFLAGS; kf)
ck "romfs 同路径同长度换了内容 → 包键变（资源索引戳只记路径与字节数，原先整包沿用上一版）" '[ "$k1" != "$k0" ]'
printf 'DLC2' > "$key_fixture/aoc/x.bin"; k2=$(unset CFLAGS; kf)
ck "只换追加内容 → 包键变" '[ "$k2" != "$k1" ]'
printf 'sysdata fixture 2\n' > "$key_fixture/.stage_sysdata"; k3=$(unset CFLAGS; kf)
ck "系统数据阶段戳变（时区来源）→ 包键变（字体 / 0802 / 0823 内容另由 tests/pkg_key_inputs_test.sh 逐文件核）" '[ "$k3" != "$k2" ]'
key_bad=$T/key_bad; mkdir -p "$key_bad"
( ipa_pkg_key "$key_commit" "$key_bad" > /dev/null 2> "$T/key_bad.err" ); rb=$?
ck "资源输入键算不出（没有 exefs/main）→ 失败并报红，不拿缺项的键去比" '[ $rb != 0 ] && grep -q "【红】算进包资源输入键失败" "$T/key_bad.err"'
# 固定翻译器/脚本/SDK等外部输入，直接调用生产key_of；旧键期望逐行手写，不经新实现生成。
# 这是显式目标输入行的兼容性单测；实际编译闭包/脚本内容键变化仍须在冷/热出包中单独核对。
(
  R=$H
  eval "$(sed -n '/^key_of() {/,/^}/p' "$H/graphics/tools/shader_pack.sh")"
  tool_keys() { CS=compiler-fixture; XC=gal-fixture; }; tool_keys_memo() { tool_keys; }   # key_of 经记忆层调 tool_keys（10-30 P4）：这里只测键怎么拼
  input_fp() { printf '%s\n' input-list-fixture main-content-fixture; }
  extrap_of() { echo off; }
  python3() { case "$1" in */key_norm.py) echo script-fixture;; *) command python3 "$@";; esac; }
  xcrun() { case "$*" in *--show-sdk-version*) echo 27.1;; *metal*--version*) echo metal-fixture;; *) return 1;; esac; }
  IPHONEOS_DEPLOYMENT_TARGET=18.0; MACOSX_DEPLOYMENT_TARGET=15.0
  unset S2A_TARGET_DEVICE_CAPS S2A_PSO_ARCHS S2A_PREBUILT_TOOLS
  # 出包目标对（10-29 起进键，pack/jobs/archs.py pairs 推导，推导本身由 pack/jobs/tests/test_archs.py 测）：这里用桩，只测键怎么拼——
  #   不设架构 = 跑局机清单两对；显式架构按给的出（与 archs.py 显式分支同形：架构 × 系统大版本 27）；桩答不出 = 推导失败
  fake_py() { [ "$2" = pairs ] || return 2; [ "${FAKE_PAIRS_FAIL:-0}" = 0 ] || return 2
    if [ -n "${S2A_PSO_ARCHS:-}" ]; then for a in $S2A_PSO_ARCHS; do printf '%s\t27\t4\n' "$a"; done | LC_ALL=C sort -t. -k1,1
    else printf 'applegpu_g18p\t27\t4\napplegpu_g19p\t27\t4\n'; fi; }
  PY=fake_py
  printf '%s\n' input-list-fixture main-content-fixture 'cs compiler-fixture' 'xc gal-fixture' 'extrap off' \
    'scripts script-fixture' 'pso-pairs applegpu_g18p.27.4 applegpu_g19p.27.4 ' 27.1 27.1 metal-fixture 'ios18.0 macos15.0' | shasum -a 256 | cut -c1-64 > "$T/shader_legacy.bytes"
  key_of "$T" > "$T/shader_unset.bytes" || exit 1
  cmp -s "$T/shader_legacy.bytes" "$T/shader_unset.bytes" || exit 1
  S2A_TARGET_DEVICE_CAPS=31,32768 key_of "$T" > "$T/shader_explicit.bytes" || exit 1
  ! cmp -s "$T/shader_unset.bytes" "$T/shader_explicit.bytes" || exit 1
  S2A_TARGET_DEVICE_CAPS= key_of "$T" > "$T/shader_empty.bytes" || exit 1
  ! cmp -s "$T/shader_unset.bytes" "$T/shader_empty.bytes" || exit 1
  S2A_PSO_ARCHS='applegpu_g18p applegpu_g19p' key_of "$T" > "$T/shader_default_arch.bytes" || exit 1
  cmp -s "$T/shader_unset.bytes" "$T/shader_default_arch.bytes" || exit 1
  S2A_PSO_ARCHS=applegpu_g14p key_of "$T" > "$T/shader_arch.bytes" || exit 1
  ! cmp -s "$T/shader_unset.bytes" "$T/shader_arch.bytes" || exit 1
  ! FAKE_PAIRS_FAIL=1 key_of "$T" > /dev/null 2>&1
)
shader_key_ok=$?
ck "着色器目标输入：未设能力/架构时 key_of 与手写期望键（含跑局机清单两对）完整字节一致；显式给的对与缺省推导相同时逐字节相同（键只认推导出的对、不认怎么给的）；显式能力/空能力/别的架构均不复用默认键；对推导不出 → key_of 报红不给键" '[ "$shader_key_ok" = 0 ]'
key_mem_small=$(CFLAGS='-DS2A_MEM_NEED_MB=3584' kf)
key_mem_large=$(CFLAGS='-DS2A_MEM_NEED_MB=4500' kf)
ck "不同编译预算不能复用同一 ipa 包键" '[ "$key_mem_small" != "$key_mem_large" ] && [ "$key_mem_small" != "$k3" ]'
S=$T/data/romfs; mkdir -p $S/sub; echo a > $S/a.bin; echo b > $S/sub/b.bin; echo cfg > $S/.build.config; echo junk > $S/.DS_Store; echo junk > $S/._a.bin
mkdir -p $T/font; echo f > $T/font/f.ttf
PK="python3 $H/pack/pack.py $T/pack romfs=$S sysfont=$T/font"; VF="python3 $H/pack/verify.py $T/pack romfs=$S sysfont=$T/font"
o=$($PK); ck "首次打包 4 个文件（含 .build.config，不含 .DS_Store / ._*）" 'grep -q "^4 个文件" <<< "$o" && grep -q "romfs/.build.config" $T/pack/index.tsv && ! grep -q "DS_Store\|/\._" $T/pack/index.tsv'
ck "核对绿" '$VF > /dev/null'

# 包内的 macOS 元数据可由 Finder 重生；只跳过约定名，不能放过真正的清单外文件（#269 / #206）。
cp "$T/pack/index.tsv" "$T/index-before-metadata.tsv"
printf '%s\n' noise > "$T/pack/.DS_Store"; printf '%s\n' noise > "$T/pack/._x"
ck "包根 .DS_Store / ._x 重生 → 核对绿且 index 字节不变" '$VF > "$T/metadata-root.log" 2>&1 && cmp -s "$T/index-before-metadata.tsv" "$T/pack/index.tsv"'
printf '%s\n' noise > "$T/pack/romfs/sub/.DS_Store"; printf '%s\n' noise > "$T/pack/romfs/sub/._x"
ck "包内子目录 .DS_Store / ._x 重生 → 核对绿且 index 字节不变" '$VF > "$T/metadata-nested.log" 2>&1 && cmp -s "$T/index-before-metadata.tsv" "$T/pack/index.tsv"'
printf '%s\n' unexpected > "$T/pack/unlisted.bin"
$VF > "$T/metadata-stray.log" 2>&1; metadata_rc=$?
ck "macOS 元数据共存时普通清单外文件 → 退出 1 且指出 unlisted.bin" '[ "$metadata_rc" = 1 ] && grep -q "清单外的文件 1 个" "$T/metadata-stray.log" && grep -q "unlisted.bin" "$T/metadata-stray.log"'
rm "$T/pack/unlisted.bin"
printf '%s\n' unexpected > "$T/pack/.user.config"
$VF > "$T/metadata-hidden-stray.log" 2>&1; metadata_rc=$?
ck "非 macOS 的点开头清单外文件 → 退出 1 且指出 .user.config" '[ "$metadata_rc" = 1 ] && grep -q "清单外的文件 1 个" "$T/metadata-hidden-stray.log" && grep -q "\.user\.config" "$T/metadata-hidden-stray.log"'
rm "$T/pack/.user.config" "$T/pack/.DS_Store" "$T/pack/._x" "$T/pack/romfs/sub/.DS_Store" "$T/pack/romfs/sub/._x"
ck "清掉合成多余文件后 → 核对恢复绿、index 字节仍不变" '$VF > /dev/null && cmp -s "$T/index-before-metadata.tsv" "$T/pack/index.tsv"'
ck "--check：最新 → 退出 0" '$PK --check > /dev/null'
sleep 0.05; echo a2 > $S/a.bin
ck "--check：源变了 → 退出 1" '! $PK --check > /dev/null'
o=$($PK); ck "重打只重算 1 个" 'grep -q "克隆 / 重算 1 个" <<< "$o" && $VF > /dev/null'
rm $S/sub/b.bin; $PK > /dev/null; ck "删源文件 → 包里也删、核对绿" '[ ! -e $T/pack/romfs/sub/b.bin ] && $VF > /dev/null'
python3 -c "f=open('$T/pack/romfs/a.bin','r+b'); f.write(b'X'); f.close()"; $VF > "$T/flip.log" 2>&1; flip=$?
ck "包里改一个字节（同长）→ 核对红、点名 SHA 与 index 不符、源没被改（克隆独立）" '[ "$flip" = 1 ] && grep -q "romfs/a.bin（SHA 与 index 不符）" "$T/flip.log" && [ "$(cat $S/a.bin)" = a2 ]'
$PK > /dev/null; rm -f $T/pack.state.tsv; $PK > /dev/null; ck "丢状态表后重打 → 核对绿" '$VF > /dev/null'
# 核对只读包里那份（11-06 维护者「少读一遍」）：普通源文件设成不可读（stat 仍可取）→ 核对照绿（不读源内容）；按全内容进包键的源不可读 → 红（照旧逐字节比源）
rm -f "$T/pack.verify.tsv"; chmod 000 $S/a.bin; $VF > "$T/oneread.log" 2>&1; oneread=$?; chmod 644 $S/a.bin
ck "核对不读普通源的内容：romfs/a.bin 源不可读、核对照绿" '[ "$oneread" = 0 ]'
rm -f "$T/pack.verify.tsv"; chmod 000 $T/font/f.ttf; $VF > "$T/fullread.log" 2>&1; fullread=$?; chmod 644 $T/font/f.ttf
ck "按全内容进包键的照旧读源比：sysfont/f.ttf 源不可读 → 核对红" '[ "$fullread" = 1 ]'
$VF > /dev/null   # 恢复核对记录（只在全绿时写），后面的增量用例要它
o=$($VF); ck "核对增量：romfs 下源 / 包都没变的 2 个不再逐字节比；sysfont/f.ttf 按全内容进包键、每次都比（审计 #1534 P1-54）" 'grep -q "核 1 个，2 个与上次核对时源 / 包都没变" <<< "$o"'
sleep 0.05; echo a3 > $S/a.bin; $PK > /dev/null; o=$($VF); ck "源改 1 个、重打后核对：逐字节比 romfs/a.bin 与 sysfont/f.ttf 共 2 个、绿" 'grep -q "核 2 个，1 个" <<< "$o"'
# 审计 #1534 P1-54 第二道门：按全内容进包键的源（这里 sysfont/，同系统数据 / 字体 / 0802 / 0823）只换内容、保住大小与修改时间 →
#   包键会变（tests/pkg_key_inputs_test.sh），打包必须换成新字节、核对必须按内容比；期望按「包里是不是新字节」判
fmt=$(python3 -c "import os;print(os.stat('$T/font/f.ttf').st_mtime_ns)")
python3 -c "import os;open('$T/font/f.ttf','wb').write(b'g\\n');os.utime('$T/font/f.ttf',ns=($fmt,$fmt))"
$VF > "$T/p54-verify-old.log" 2>&1; p54v=$?
ck "源只换内容（f → g，同长、修改时间改回）、包还没重打：有旧核对记录也照样红（原先签名全同、按记录跳过、绿）" '[ "$p54v" = 1 ] && grep -q "sysfont/f.ttf（内容与源不同）" "$T/p54-verify-old.log"'
o=$($PK); ck "重打：sysfont/f.ttf 按内容判过期、重克隆 1 个，包里换成新字节（原先「重算 0 个」、包里仍是 f）" 'grep -q "克隆 / 重算 1 个" <<< "$o" && [ "$(cat $T/pack/sysfont/f.ttf)" = g ] && grep -q "^sysfont/f.ttf	2	$(shasum -a 256 $T/font/f.ttf | cut -c1-64)$" $T/pack/index.tsv'
ck "重打后核对绿；--check 认最新" '$VF > /dev/null && $PK --check > /dev/null'
python3 -c "import os;open('$T/font/f.ttf','wb').write(b'h\\n');os.utime('$T/font/f.ttf',ns=($fmt,$fmt))"
ck "--check：源只换内容、大小与修改时间不变 → 退出 1（过期）" '! $PK --check > /dev/null'
$PK > /dev/null
rmt=$(python3 -c "import os;print(os.stat('$S/a.bin').st_mtime_ns)")
python3 -c "import os;open('$S/a.bin','wb').write(b'a4\\n');os.utime('$S/a.bin',ns=($rmt,$rmt))"
o=$($PK); ck "romfs 下只换内容、保住大小与修改时间（原地写 + utime 改回，ctime 变）→ 按内容判过期、重算 1 个，包里换成新字节（审计 #1592 P1-54；原先「重算 0 个」、包里仍是旧字节）" 'grep -q "克隆 / 重算 1 个" <<< "$o" && [ "$(cat $T/pack/romfs/a.bin)" = a4 ]'
o=$($PK); ck "身份没变再打：不重读、重算 0 个" 'grep -q "克隆 / 重算 0 个" <<< "$o"'
cp -p $S/a.bin $S/.a.new && printf 'a5\n' > $S/.a.new && python3 -c "import os;os.utime('$S/.a.new',ns=($rmt,$rmt))" && mv $S/.a.new $S/a.bin
o=$($PK); ck "换成另一个同大小同 mtime 的文件（新 inode，同 tar / rsync -a 覆盖）→ 重算 1 个、包里是新字节" 'grep -q "克隆 / 重算 1 个" <<< "$o" && [ "$(cat $T/pack/romfs/a.bin)" = a5 ]'
cp -p $S/a.bin $S/.a.cp && mv $S/.a.cp $S/a.bin
o=$($PK); ck "换成内容相同的副本（新 inode、同 mtime，同新版本根克隆来的 aoc）→ 只重读比内容、不重拷，重算 0 个" 'grep -q "克隆 / 重算 0 个" <<< "$o" && $VF > /dev/null'
echo a3 > $S/a.bin; $PK > /dev/null
python3 -c "import os;f=open('$T/pack/romfs/.build.config','r+b'); f.write(b'X'); f.close()"; ck "核对通过后包里改字节（同长，mtime 变）→ 照样核对红" '! $VF > /dev/null'; $PK > /dev/null; rm -f $T/pack.state.tsv; $PK > /dev/null
echo x > $T/pack/stray.bin; ck "包里多文件 → 核对红" '! $VF > /dev/null'; rm $T/pack/stray.bin
echo npdm > $T/main.npdm; PF="python3 $H/pack/pack.py $T/pack2 exefs/main.npdm=$T/main.npdm romfs=$S"
o=$($PF); ck "单文件源：exefs/main.npdm 进包、核对绿" 'grep -q "^exefs/main.npdm	5	" $T/pack2/index.tsv && python3 $H/pack/verify.py $T/pack2 exefs/main.npdm=$T/main.npdm romfs=$S > /dev/null'
ck "目录源用多级前缀（a/b=目录）→ 进包路径 a/b/…、核对绿" 'python3 $H/pack/pack.py $T/pack3 a/b=$S > /dev/null && grep -q "^a/b/a.bin	" $T/pack3/index.tsv && python3 $H/pack/verify.py $T/pack3 a/b=$S > /dev/null'
printf 'exefs/main.npdm=%s\n\nromfs=%s\n' $T/main.npdm $S > $T/srcs.txt
ck "@<清单文件>（每行一项、空行跳过）→ 与逐项参数出的 index 逐字节相同、核对也认 @ 清单（10-24 巫师3 着色器 8687 个参数超 ARG_MAX）" 'python3 $H/pack/pack.py $T/pack4 @$T/srcs.txt > /dev/null && cmp -s $T/pack2/index.tsv $T/pack4/index.tsv && python3 $H/pack/verify.py $T/pack4 @$T/srcs.txt > /dev/null'
mkdir -p $T/ov/Layout; printf 'widened-layout' > $T/ov/Layout/x.zs; mkdir -p $S/Layout; printf 'orig' > $S/Layout/x.zs
python3 $H/pack/pack.py $T/pack5 romfs=$S romfs=$T/ov > /dev/null 2>&1; r=$?
ck "界面布局覆盖层（同包内路径给两次，后者优先）：索引里 romfs/Layout/x.zs 只一行、大小与 SHA 是覆盖层那份、包里内容是覆盖层的、核对绿（11-06）" '[ $r = 0 ] && [ "$(grep -c "^romfs/Layout/x.zs	" $T/pack5/index.tsv)" = 1 ] && grep -q "^romfs/Layout/x.zs	14	$(shasum -a 256 < $T/ov/Layout/x.zs | cut -c1-64)" $T/pack5/index.tsv && [ "$(cat $T/pack5/romfs/Layout/x.zs)" = widened-layout ] && python3 $H/pack/verify.py $T/pack5 romfs=$S romfs=$T/ov > /dev/null'
ck "覆盖层不给 → 包里是原版（同一份源、只是少了覆盖层）" 'python3 $H/pack/pack.py $T/pack6 romfs=$S > /dev/null 2>&1 && [ "$(cat $T/pack6/romfs/Layout/x.zs)" = orig ]'
rm -rf $S/Layout
ck "包内路径带 .. → 拒绝" '! python3 $H/pack/pack.py $T/pack3 ../x=$T/main.npdm > /dev/null 2>&1'
export S2A_ROOTS=$T/roots; source $H/pack/roots.sh
# root_pso0（管线归档 0 KB 判据，10-21）：有分片 + 0 KB → 是坏的、打分片数；有分片有大小 / 没分片 / 没有 json / 旧版无字段 → 不算
mkdir -p $T/p0/a/shaders $T/p0/b/shaders $T/p0/c/shaders $T/p0/d/shaders $T/p0/e
echo '{"pso": {"chunks": 27}, "pso_archive_kb": 0}' > $T/p0/a/shaders/shader_pack.json; echo '{"pso": {"chunks": 27}, "pso_archive_kb": 547731}' > $T/p0/b/shaders/shader_pack.json
echo '{"pso": {"chunks": 0}, "pso_archive_kb": 0}' > $T/p0/c/shaders/shader_pack.json; echo '{"red": {}}' > $T/p0/d/shaders/shader_pack.json
mkdir -p $T/p0/f/shaders/direct; cp $T/p0/a/shaders/shader_pack.json $T/p0/f/shaders/; echo k > $T/p0/f/shaders/direct/keys.tsv   # 直连款：同样 27 片 0 KB，但包里有直连库
# 离线 0 支的三种直连形态（11-05）：keys.tsv 空 + vk 表头 / gl 表头 / runtime_only.txt，同样 27 片 0 KB → 不算坏；keys.tsv 空、三样都没有 → 照判坏（反证）
for k in g v r n; do mkdir -p $T/p0/$k/shaders/direct; cp $T/p0/a/shaders/shader_pack.json $T/p0/$k/shaders/; : > $T/p0/$k/shaders/direct/keys.tsv; done
mkdir -p $T/p0/g/shaders/direct/gl $T/p0/v/shaders/direct/vk; echo '# s2arm-gl-lib 2' > $T/p0/g/shaders/direct/gl/gl_shaders.tsv; echo '# s2arm-vk-lib 3' > $T/p0/v/shaders/direct/vk/vk_shaders.tsv; echo 缺口 > $T/p0/r/shaders/direct/runtime_only.txt
ck "root_pso0：只有 vk 表头 / gl 表头 / 运行时现编空库的直连款 → 不算坏；keys.tsv 空且三样都没有 → 照判坏（打 27）" '! root_pso0 $T/p0/v && ! root_pso0 $T/p0/g && ! root_pso0 $T/p0/r && [ "$(root_pso0 $T/p0/n)" = 27 ]'
ck "root_pso0：27 片 0 KB → 坏（打 27）；有大小 / 0 片 / 旧版无字段 / 没有 json / 直连款（通用按口径不出归档）→ 不算" '[ "$(root_pso0 $T/p0/a)" = 27 ] && ! root_pso0 $T/p0/b && ! root_pso0 $T/p0/c && ! root_pso0 $T/p0/d && ! root_pso0 $T/p0/e && ! root_pso0 $T/p0/f'
g1=$(unset S2A_ALLOW_PSO0; root_pso0_gate $T/p0/a d3); r1=$?; g2=$(S2A_ALLOW_PSO0=仅录制 root_pso0_gate $T/p0/a d3); r2=$?; g3=$(unset S2A_ALLOW_PSO0; root_pso0_gate $T/p0/b acnh); r3=$?
ck "登记闸门：0 KB 缺省判红不放（rc=${r1}）；S2A_ALLOW_PSO0=仅录制 → 🟡 例外放行带原因（rc=${r2}）；好包静默放行（rc=${r3}）" '[ $r1 = 1 ] && [[ "$g1" == 【红】d3\ 管线归档\ 0\ KB* ]] && [ $r2 = 0 ] && [[ "$g2" == 🟡*例外登记——仅录制 ]] && [ $r3 = 0 ] && [ -z "$g3" ]'
W=$(root_begin acnh c1) && echo v1 > $W/out.txt && root_commit acnh c1; ck "c1 提交后现行 → c1" '[ "$(basename "$(root_cur acnh)")" = acnh@c1 ] && [ "$(cat $(root_cur acnh)/out.txt)" = v1 ]'
W=$(root_begin acnh c2); ck "c2 从现行克隆（带 c1 内容）" '[ "$(cat $W/out.txt)" = v1 ]'
echo v2 > $W/out.txt; ck "提交前现行仍是 c1" '[ "$(cat $(root_cur acnh)/out.txt)" = v1 ]'
root_commit acnh c2; ck "c2 提交：现行 → c2，c1 内容不变" '[ "$(cat $(root_cur acnh)/out.txt)" = v2 ] && [ "$(cat $T/roots/acnh@c1/out.txt)" = v1 ]'
ck "已提交的 c1 不许重开" '! root_begin acnh c1 2>/dev/null'
# 直连库分片清点日志（11-01 究极4 分片丢在出包之后某步）：另起一款 dlg（不动 acnh 的版本序列）：k1 提交时带 1 个分片的直连库；k2 从现行克隆后提交——
#   root_begin 克隆现行 / root_commit 提交前 / 提交后各记一行到 $S2A_ROOTS/.shard_inventory.log，都是 1/1 齐
W=$(root_begin dlg k1) && mkdir -p $W/shaders/direct/b-zz && python3 - $W/shaders/direct/b-zz <<'PYE'
import struct, sys
d = sys.argv[1]; h = b'S2ALIB\0\0' + struct.pack('<IIII', 1, 0, 1, 0) + b'zz'.ljust(12, b'\0') + bytes(12) + struct.pack('<QQ', 50, 0)
open(d + '/lib.idx', 'wb').write(h); open(d + '/lib.000.pak', 'wb').write(bytes(50))
PYE
root_commit dlg k1; rm -f $T/roots/.shard_inventory.log; W=$(root_begin dlg k2) && root_commit dlg k2
ck "分片清点：克隆现行 / 提交前 / 提交后各记一行，都是 1/1 齐" '[ "$(grep -c "在且大小相符 1/1" $T/roots/.shard_inventory.log)" = 3 ] && grep -q "克隆现行(root_begin)" $T/roots/.shard_inventory.log && grep -q "提交前(root_commit)" $T/roots/.shard_inventory.log && grep -q "提交后(root_commit)" $T/roots/.shard_inventory.log'

W=$(root_begin acnh c3); root_abort acnh c3; ck "中止：删 .tmp、现行不动" '[ ! -e $T/roots/acnh@c3.tmp ] && [ "$(basename "$(root_cur acnh)")" = acnh@c2 ]'
for c in c3 c4 c5; do W=$(root_begin acnh $c) && root_commit acnh $c; sleep 0.05; done
ck "只留现行 + 上一版（c4 c5），现行 → c5" '[ "$(ls -d $T/roots/acnh@c* | wc -l | tr -d " ")" = 2 ] && [ ! -e $T/roots/acnh@c3 ] && [ "$(basename "$(root_cur acnh)")" = acnh@c5 ]'
W=$(root_begin acnh c6) && mkdir -p $W/ro/sub && echo x > $W/ro/sub/f && chmod a-w $W/ro/sub $W/ro && root_commit acnh c6; for c in c7 c8 c9; do W=$(root_begin acnh $c) && root_commit acnh $c; sleep 0.05; done
ck "旧版本里有只读目录也能剪掉（只留 2 版）" '[ "$(ls -d $T/roots/acnh@c* | wc -l | tr -d " ")" = 2 ] && [ ! -e $T/roots/acnh@c6 ]'
pc=$(S2A_ROOTS_NO_BOOT= S2A_ROOTS_SHARED=$T/roots S2A_ROOTS_PRIV=$T/priv S2A_ROOTS=$T/priv/x bash -c "source $H/pack/roots.sh; root_cur acnh")
ck "私有根起步：从共享现行克隆同名版本（$(basename "$pc")），现行链接可解析（10-20 拼成 acnh@acnh@… 的 bug）" '[ "$(basename "$pc")" = "$(basename "$(root_cur acnh)")" ] && [ -d "$T/priv/x/$(basename "$(root_cur acnh)")" ]'
W=$(root_begin acnh m0) && echo m > $W/m.txt && root_commit acnh m0
A=$(root_begin acnh pa); B=$(root_begin acnh pb); echo a > $A/a.txt; root_commit acnh pa
ck "两段同时开工、后提交的没声明产物路径 → 拒绝、现行留在先提交的" '! root_commit acnh pb 2>/dev/null && [ "$(basename "$(root_cur acnh)")" = acnh@pa ] && [ ! -e $T/roots/acnh@pb ]'
A=$(root_begin acnh ra); B=$(root_begin acnh rb); echo a2 > $A/a.txt; mkdir -p $B/sysd && echo f > $B/sysd/f; rm $B/m.txt; root_commit acnh ra
ROOT_REPLAY='sysd m.txt' root_commit acnh rb 2>/dev/null
ck "声明了产物路径 → 重放到新现行：先提交的 a.txt=a2 在、本段 sysd/f 在、本段删掉的 m.txt 也删了" 'c=$(root_cur acnh); [ "$(basename "$c")" = acnh@rb ] && [ "$(cat $c/a.txt)" = a2 ] && [ -f $c/sysd/f ] && [ ! -e $c/m.txt ]'
# 套着的别的阶段产物（10-25 测试设备1 马车8）：着色器阶段 shaders/ 里有直连阶段的 shaders/direct。两段同时开工，直连段先提交新库（b-x/ + shader_map.tsv），
#   着色器段后提交：声明 'shaders !shaders/direct' → 着色器新文件在、直连新库保住；只声明 'shaders'（旧写法）→ 直连库退回开工时的旧版（shader_map.tsv 没了、b-x/ 还在）
W=$(root_begin acnh d0) && mkdir -p $W/shaders/direct/b-x && echo old > $W/shaders/direct/b-x/f && echo g0 > $W/shaders/gal.idx && root_commit acnh d0
A=$(root_begin acnh dsh); B=$(root_begin acnh ddl); echo new > $B/shaders/direct/b-x/f; echo m > $B/shaders/direct/shader_map.tsv; ROOT_REPLAY='shaders/direct .stage_direct' root_commit acnh ddl 2>/dev/null
echo g1 > $A/shaders/gal.idx; ROOT_REPLAY='shaders !shaders/direct .stage_shaders' root_commit acnh dsh 2>/dev/null
ck "着色器段 'shaders !shaders/direct' 后提交：gal.idx=g1、直连新库 b-x/f=new 与 shader_map.tsv 都在" 'c=$(root_cur acnh); [ "$(basename "$c")" = acnh@dsh ] && [ "$(cat $c/shaders/gal.idx)" = g1 ] && [ "$(cat $c/shaders/direct/b-x/f)" = new ] && [ -f $c/shaders/direct/shader_map.tsv ]'
A=$(root_begin acnh esh); B=$(root_begin acnh edl); echo m2 > $B/shaders/direct/shader_map.tsv; ROOT_REPLAY='shaders/direct .stage_direct' root_commit acnh edl 2>/dev/null
rm -f $A/shaders/direct/shader_map.tsv; echo g2 > $A/shaders/gal.idx; ROOT_REPLAY='shaders .stage_shaders' root_commit acnh esh 2>/dev/null
ck "反证：只声明 'shaders'（10-25 前的写法）→ 直连新表被退回（shader_map.tsv 没了、b-x/ 还在），即事故形态" 'c=$(root_cur acnh); [ "$(cat $c/shaders/gal.idx)" = g2 ] && [ ! -e $c/shaders/direct/shader_map.tsv ] && [ -f $c/shaders/direct/b-x/f ]'
A=$(root_begin acnh copy_a); B=$(root_begin acnh copy_b); echo incomplete > "$B/copy_failure.txt"; root_commit acnh copy_a
copy_rejected=0
( cp() { return 1; }; mv() { return 1; }; ROOT_REPLAY=copy_failure.txt root_commit acnh copy_b ) > "$T/copy_failure.log" 2>&1 || copy_rejected=1   # 顶层路径改为整个挪（mv，11-05）：拷 / 挪都打桩成失败
ck "重放复制失败 → 中止、不切现行、不提交缺失产物" '[ "$copy_rejected" = 1 ] && [ "$(basename "$(root_cur acnh)")" = acnh@copy_a ] && [ ! -e "$T/roots/acnh@copy_b" ] && [ ! -e "$T/roots/acnh@copy_b.tmp" ] && [ ! -e "$T/roots/acnh@copy_b.tmp.rb" ]'
# 共享根只许往前（10-24 宽屏线报：各线工作树停在不同 main 轮流刷共享现行，键来回变、反复重烘）：两棵版本不同的树（来源 OLD 是 NEW 的祖先）先后对同一款开工——
#   ① 新树先提交了同一阶段 k（.stage_k 变了）→ 旧树后提交 k 被拒、丢弃、不重放，现行仍是新树的 k、来源 NEW（下次新树刷时键相同、不重烘）；
#   ② 新树先提交的是别的阶段 j（k 没动过）→ 旧树的 k 照常重放上去，j 也在，来源留 NEW 不退回（10-24 首版不分阶段一律作废：合入几分钟一笔、烘焙 20 分钟，
#      持槽烘完的着色器总被新树的改写提交顶掉，一款都烘不上）；③ 旧树先、新树后 → 照常前进。期望按规则手写；私有根不受这条限制（上面各用例）
OLD=$(git -C $H rev-parse HEAD~1); NEW=$(git -C $H rev-parse HEAD); SR=$T/sroots
base() { rm -rf $SR; mkdir -p $SR/acnh@base && echo base > $SR/acnh@base/k.txt && echo Kb > $SR/acnh@base/.stage_k && echo Jb > $SR/acnh@base/.stage_j && echo $OLD > $SR/acnh@base/.root_src && ln -sfn acnh@base $SR/acnh@现行; }
fwd() {   # fwd <版本名> <来源> <阶段 k|j> <产物内容>：在共享根 $SR 里模拟从 acnh@base 开工的一个阶段 .tmp（不走 root_begin，它要本树在 origin/main 上）
  cp -R $SR/acnh@base $SR/acnh@$1.tmp && printf '.stage_j\tJb\n.stage_k\tKb\n' > $SR/acnh@$1.tmp/.root_base_stages && echo $2 > $SR/acnh@$1.tmp/.root_src && echo acnh@base > $SR/acnh@$1.tmp/.root_base
  echo $4 > $SR/acnh@$1.tmp/$3.txt && echo "$4" > $SR/acnh@$1.tmp/.stage_$3; }
cm() { S2A_ROOTS=$SR S2A_ROOTS_SHARED=$SR bash -c "source $H/pack/roots.sh; ROOT_REPLAY='$2.txt .stage_$2' root_commit acnh $1" 2>&1; }
cur() { cat "$(cd -P $SR/acnh@现行 && pwd)/$1"; }
base; fwd new $NEW k K_NEW; fwd old $OLD k K_OLD; o1=$(cm new k); r1=$?; o2=$(cm old k); r2=$?
ck "① 新树先提交阶段 k（rc=${r1}）、旧树后提交同一阶段被拒（rc=${r2}）——现行 k = K_NEW、来源 NEW，旧树 .tmp 已丢" '[ $r1 = 0 ] && [ $r2 = 1 ] && [ "$(cur k.txt)" = K_NEW ] && [ "$(cur .root_src)" = $NEW ] && [ ! -e $SR/acnh@old.tmp ] && [ ! -e $SR/acnh@old ] && [[ "$o2" == *只许往前* ]]'
base; fwd newj $NEW j J_NEW; fwd oldk $OLD k K_OLD; cm newj j > /dev/null; r3=$?; cm oldk k > /dev/null; r4=$?
ck "② 新树先提交的是别的阶段 j（rc=${r3}）、旧树的 k 照常重放（rc=${r4}）——现行 j = J_NEW、k = K_OLD、来源留 NEW 不退回" '[ $r3 = 0 ] && [ $r4 = 0 ] && [ "$(cur j.txt)" = J_NEW ] && [ "$(cur k.txt)" = K_OLD ] && [ "$(cur .root_src)" = $NEW ]'
base; fwd old3 $OLD k K_OLD; fwd new3 $NEW k K_NEW3; cm old3 k > /dev/null; r5=$?; cm new3 k > /dev/null; r6=$?
ck "③ 旧树先提交（rc=${r5}）、新树后提交照常前进（rc=${r6}）——现行 k = K_NEW3、来源 NEW" '[ $r5 = 0 ] && [ $r6 = 0 ] && [ "$(cur k.txt)" = K_NEW3 ] && [ "$(cur .root_src)" = $NEW ]'
for k in 1 2 3; do ( W=$(root_begin acnh q$k) && mkdir -p $W/d$k && echo $k > $W/d$k/f && sleep 0.2 && ROOT_REPLAY="d$k" root_commit acnh q$k 2>/dev/null ) & done; wait
ck "三段真并发提交（提交锁串行）→ 三份产物都在现行里" 'c=$(root_cur acnh); [ -f $c/d1/f ] && [ -f $c/d2/f ] && [ -f $c/d3/f ] && [ ! -d $T/roots/.acnh.commit ]'
ln -s /etc $(root_cur acnh)/link; ck "现行里有软链 → 拒绝开新版" '! root_begin acnh c10 2>/dev/null && [ ! -e $T/roots/acnh@c10.tmp ]'
# 一包通用（10-30）：ipa.sh 逐项带直连库、不带按机型的死目录 link/（直连链接版已删，旧根残留 dl.<架构>.ios<版本> / pso.<架构>.*.bin）
W=$T/adw; mkdir -p $W/shaders/direct/b-x $W/shaders/direct/link && echo k > $W/shaders/direct/keys.tsv && echo m > $W/shaders/direct/shader_map.tsv && : > $W/shaders/direct/link/dl.applegpu_g18p.ios27
got=$(S2A_DUAL=1 ipa_pkg_srcs $W | grep '^shaders/' | sed "s#=.*##" | LC_ALL=C sort | tr '\n' ' ')   # 源清单只有一份（pack/ipa_key.sh ipa_pkg_srcs，ipa.sh 打包与包键共用）
ck "直连库进包逐项带、跳过 link/（实得：${got}）" '[ "$got" = "shaders/direct/b-x shaders/direct/keys.tsv shaders/direct/shader_map.tsv " ]'
# 补丁随包（部署方案 v2，满屏补丁线 11-04）：产物根有 patch.tsv → 进资源包源清单；verified_builds 含本构建号才随包（段内同名键不算）
: > $W/patch.tsv; got=$(ipa_pkg_srcs $W | grep '^patch.tsv=' | sed "s#=.*##"); rm $W/patch.tsv
ck "产物根有 patch.tsv（非空才算）→ 空文件不进包（实得：${got:-无}）" '[ -z "$got" ]'
echo x > $W/patch.tsv; got=$(ipa_pkg_srcs $W | grep '^patch.tsv=' | sed "s#=.*##")
ck "产物根有 patch.tsv → 资源包源清单带 patch.tsv（实得：${got:-无}）" '[ "$got" = patch.tsv ]'
source "$H/pack/patch_ship.sh"; B=d1ed438c8bdaf4368b30daec53ed8e9b; printf 'schema\t1\nverified_builds\tFFFF D1ED438C8BDAF4368B30DAEC53ED8E9B\n[caps]\n' > $W/p1; printf 'schema\t1\n[caps]\nverified_builds\t%s\n' $B > $W/p2
ck "verified_builds 含本构建号（大小写不同）→ 随包" 'ipa_patch_ship $W/p1 $B'
ck "构建号差一位 → 不随包" '! ipa_patch_ship $W/p1 ${B%b}c'
ck "verified_builds 写在段里（不是文件头）→ 不随包" '! ipa_patch_ship $W/p2 $B'
chmod -R u+w $T; rm -rf $T; [ $bad = 0 ] && echo "pack 单测全过" || { echo "pack 单测有不通"; exit 1; }
