#!/bin/bash
# 解码软兜底自动探测（S2A_DAV1D / S2A_VPX 缺省 auto）：本机探测到不需要软解的 Mac 上，软解库整条不进构建——
#   dav1d（AV1）：tools/av1hw.sh（VTIsHardwareDecodeSupported + 真 av1C 建得出 VideoToolbox AV1 会话）→ host / iOS 不链、
#     meson / ninja 不是必需项（prepare_build.py）、出包不放 dav1d 预编库（bundle_engine.py）、codec 单测不链 dav1d 照常跑；
#   libvpx（VP8 / VP9）：tools/vp9hw.sh（注册 supplemental 后真 vpcC 建得出 VP9 会话，#348 判据）→ 不链真身、链全失败的
#     空垫片（services/vpx/stub.c，垫住 C# DirectPInvoke 的静态强引用）、出包不放 vpx 预编库；VP9 默认走系统（C 与 C#
#     都见 s2a_vpx_linked() = 0 开系统路线、逐级回退托管），VP8 无任何系统 / 托管替代——如实报红不出假画面。
#   探测不过的机器（AV1：M1 / M2、A16 及更早；VP9：会话建不出的）与原默认完全一致。S2A_DAV1D / S2A_VPX=1 / 0 显式覆盖
#   （发版机出包要落到这些机器 / 内容上时）。
# 桩探测脚本 + 预置缓存，不碰真机探测、不真编译；门禁覆盖：tools/av1hw.sh、tools/vp9hw.sh、Makefile（S2A_DAV1D_EFF /
#   S2A_VPX_EFF / HOST_CFG）、services/build.mk、launchers/macos/prepare_build.py、launchers/macos/bundle_engine.py、
#   runtime/tests/mk/nvn2_codec.mk
set -euo pipefail
H=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/s2a-codec-auto.XXXXXX")
trap 'rm -rf "$T"' EXIT

fail() { echo "不通：$*" >&2; exit 1; }

# 0) 探测脚本本体：--want 透传开关；缓存命中不出编译器直接答、--want 取反；探测失败保守回 0 且不缓存
probe0() { # $1=脚本 $2=S2A 开关名 $3=缓存目录环境名
  local s="$1" sw="$2" ce="$3" hw want C
  [ "$(env "$sw=1" bash "$H/$s" --want)" = 1 ] || fail "$s --want 没透传 $sw=1"
  [ "$(env "$sw=0" bash "$H/$s" --want)" = 0 ] || fail "$s --want 没透传 $sw=0"
  for hw in 0 1; do
    C="$T/$ce$hw"; mkdir -p "$C/$(shasum -a 256 < "$H/$s" | cut -c1-12)"; printf %s "$hw" > "$C/$(shasum -a 256 < "$H/$s" | cut -c1-12)/result"
    # 缓存命中即答：DEVELOPER_DIR 指到不存在的目录，真要出编译器必败回 0——hw=1 还能答 1 就是没出编译器
    [ "$(env "$ce=$C" DEVELOPER_DIR=$T/no-xcode bash "$H/$s")" = "$hw" ] || fail "$s 缓存命中（hw=${hw}）没直接回"
    want=1; [ "$hw" = 1 ] && want=0
    [ "$(env "$ce=$C" DEVELOPER_DIR=$T/no-xcode bash "$H/$s" --want)" = "$want" ] || fail "$s --want 没把 hw=$hw 翻成 want=$want"
  done
  C="$T/${ce}cold"; mkdir -p "$C"
  [ "$(env "$ce=$C" DEVELOPER_DIR=$T/no-xcode bash "$H/$s")" = 0 ] || fail "$s 探测失败没保守回 0"
  if [ -e "$C/$(shasum -a 256 < "$H/$s" | cut -c1-12)/result" ]; then fail "$s 探测失败的结果被缓存了（装好 Xcode 后应重新探测）"; fi
}
probe0 tools/av1hw.sh S2A_DAV1D S2A_AV1HW_CACHE
probe0 tools/vp9hw.sh S2A_VPX S2A_VPXHW_CACHE

# 1) 顶层 Makefile + services/build.mk：生效值随 auto 探测 / 显式 1 / 显式 0 变化；空跑（make -n）不出编译器真探测、按要链算
D="$T/mk"
mkdir -p "$D/services/vpx" "$D/services/opus" "$D/services/dav1d" "$D/tools"
cp "$H/Makefile" "$D/Makefile"
cp "$H/services/build.mk" "$D/services/build.mk"
cp "$H/tools/toolchain.env" "$D/tools/toolchain.env"
stub() { printf '#!/bin/sh\nprintf "%%s\\n" "%s"\n' "$2" > "$1"; chmod +x "$1"; }
stub "$D/services/nxaot.sh" "$T/libs2a_cs.a"
stub "$D/services/vpx/build.sh" "$T/libs2a_vpx.a"
stub "$D/services/vpx/build_stub.sh" "$T/libs2a_vpx_stub.a"
stub "$D/services/opus/build.sh" "$T/libs2a_opus.a"
stub "$D/services/dav1d/build.sh" "$T/libs2a_dav1d.a"
# 桩探测脚本（av1hw / vp9hw 同一条口径）：自定位记调用次数（空跑必须零调用）、按同目录 hw 文件答，--want 的开关透传与取反同真脚本
fakeprobe() { # $1=脚本名 $2=hw 文件名
  cat > "$D/tools/$1" <<EOF
#!/bin/sh
d=\$(dirname "\$0")
printf x >> "\$d/$2.calls"
if [ "\$1" = --want ]; then
  case "\${$3:-auto}" in 1|0) echo "\${$3}"; exit 0;; esac
fi
hw=\$(cat "\$d/$2")
if [ "\$1" = --want ]; then
  [ "\$hw" = 1 ] && echo 0 || echo 1
else
  echo "\$hw"
fi
EOF
  chmod +x "$D/tools/$1"
}
fakeprobe av1hw.sh hw1 S2A_DAV1D
fakeprobe vp9hw.sh hw9 S2A_VPX
cat > "$D/extra.mk" <<'EOF'
$(info EFF_DAV1D=$(S2A_DAV1D_EFF) EFF_VPX=$(S2A_VPX_EFF))
.PHONY: print
print:
	@printf 'HOST=%s\n' '$(HOST_LIBS)'
	@printf 'IOS=%s\n' '$(IOS_LIBS)'
EOF
mkprint() { make -s -C "$D" -f Makefile -f extra.mk S2A_DEVELOPER_DIR=/usr/bin S2A_CS=1 "$@" print; }
# $1=说明 $2=S2A_DAV1D 传参（空=不设，缺省链） $3=S2A_VPX 传参 $4=hw1 $5=hw9 $6=期望 dav1d EFF $7=期望 vpx EFF
case_check() {
  local out d="$6" x="$7"
  echo "$4" > "$D/tools/hw1"; echo "$5" > "$D/tools/hw9"
  : > "$D/tools/hw1.calls"; : > "$D/tools/hw9.calls"
  out=$(mkprint ${2:+S2A_DAV1D=$2} ${3:+S2A_VPX=$3} 2>&1) || fail "$1：make 失败：$out"
  grep -qx "EFF_DAV1D=$d EFF_VPX=$x" <<<"$out" || fail "$1：生效值应为 dav1d=${d} vpx=${x}：$out"
  case "$out" in *libs2a_dav1d.a*) [ "$d" = 1 ] || fail "$1：不应链 dav1d：$out";;
                  *) [ "$d" = 0 ] || fail "$1：host/iOS 没链 dav1d：$out";; esac
  case "$out" in *libs2a_vpx.a*) [ "$x" = 1 ] || fail "$1：不应链真 vpx：$out";;
                *libs2a_vpx_stub.a*) [ "$x" = 0 ] || fail "$1：没链 vpx 空垫片：$out";;
                *) fail "$1：vpx 既没链真身也没链空垫片：$out";; esac
  for lib in libs2a_opus.a libs2a_cs.a; do
    grep -q "$lib" <<<"$out" || fail "$1：丢了 ${lib}：$out"
  done
}
# 缺省（不设）= 链：构建机出的二进制会分发到别的机器，探测构建机判不了玩家设备；auto 只在显式设时探测
case_check "缺省：探测过也照链"    ""  "" 1 1 1 1
case_check "auto 都探测不过"     auto auto 0 0 1 1
case_check "auto 都探测过"       auto auto 1 1 0 0
case_check "dav1d 显式 1 盖过探测" 1  auto 1 1 1 0
case_check "vpx 显式 1 盖过探测"  auto  1 1 1 0 1
case_check "都显式 0"            0   0 0 0 0 0
n=$(grep -o 'libs2a_dav1d.a' <<<"$(echo 0 > "$D/tools/hw1"; echo 0 > "$D/tools/hw9"; : > "$D/tools/hw1.calls"; : > "$D/tools/hw9.calls"; mkprint S2A_DAV1D=auto S2A_VPX=auto)" | wc -l | tr -d ' ')
[ "$n" = 2 ] || fail "auto 探测不过时 dav1d 应同时链 host / iOS（2 处），实际 $n"
# 空跑（make -n）：不出编译器真探测（两个探测脚本都零调用）、按要链算、链接命令带占位库
echo 1 > "$D/tools/hw1"; echo 1 > "$D/tools/hw9"; : > "$D/tools/hw1.calls"; : > "$D/tools/hw9.calls"
out=$(make -n -C "$D" -f Makefile -f extra.mk S2A_DEVELOPER_DIR=/usr/bin S2A_CS=1 print 2>&1) || fail "make -n 失败：$out"
[ -s "$D/tools/hw1.calls" ] && fail "空跑（make -n）也真探测了 av1hw（应零调用）"
[ -s "$D/tools/hw9.calls" ] && fail "空跑（make -n）也真探测了 vp9hw（应零调用）"
grep -qx "EFF_DAV1D=1 EFF_VPX=1" <<<"$out" || fail "空跑应按要链算（EFF 均 1）：$out"
grep -q "dry-run-dav1d.a" <<<"$out" || fail "空跑的链接命令应带 dav1d 占位库：$out"
grep -q "dry-run-vpx.a" <<<"$out" || fail "空跑的链接命令应带 vpx 占位库：$out"

# 2) prepare_build.py：两个 wanted 随开关直说 / auto 探测（桩缓存，不出编译器）
# 缺省（不设）= 要链，不探测（构建机判不了玩家设备）：探测缓存说「有硬解」也照要
for fn in dav1d_wanted vpx_wanted; do
  C="$T/pb-default"; for s in tools/av1hw.sh tools/vp9hw.sh; do K=$(shasum -a 256 < "$H/$s" | cut -c1-12); mkdir -p "$C/$K"; printf 1 > "$C/$K/result"; done
  env -u S2A_DAV1D -u S2A_VPX "S2A_AV1HW_CACHE=$C" "S2A_VPXHW_CACHE=$C" python3 - "$H" "$fn" <<'PY' || fail "prepare_build 缺省应要链"
import importlib.util, os, sys
h, fn = sys.argv[1], sys.argv[2]
spec = importlib.util.spec_from_file_location("prepare_build", os.path.join(h, "launchers/macos/prepare_build.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
want, _ = getattr(m, fn)(None)
if not want: raise SystemExit("缺省（不设）时 %s 应为 True" % fn)
PY
done
for pair in "dav1d_wanted S2A_DAV1D tools/av1hw.sh" "vpx_wanted S2A_VPX tools/vp9hw.sh"; do
  set -- $pair; fn=$1; sw=$2; s=$3
  for v in 0 1; do
    env "$sw=$v" python3 - "$H" "$fn" "$v" <<'PY'
import importlib.util
import os
import sys
h, fn, v = sys.argv[1], sys.argv[2], sys.argv[3]
spec = importlib.util.spec_from_file_location("prepare_build", os.path.join(h, "launchers/macos/prepare_build.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
want, _ = getattr(m, fn)(None)
if want != (v == "1"):
    raise SystemExit("%s(%s) = %s" % (fn, v, want))
PY
  done
  for hw in 0 1; do
    C="$T/pb-$(basename "$s" .sh)$hw"; K=$(shasum -a 256 < "$H/$s" | cut -c1-12); mkdir -p "$C/$K"; printf %s "$hw" > "$C/$K/result"
    env "S2A_DAV1D=auto" "S2A_VPX=auto" "S2A_AV1HW_CACHE=$C" "S2A_VPXHW_CACHE=$C" python3 - "$H" "$fn" "$hw" <<'PY'
import importlib.util
import os
import sys
h, fn, hw = sys.argv[1], sys.argv[2], sys.argv[3]
spec = importlib.util.spec_from_file_location("prepare_build", os.path.join(h, "launchers/macos/prepare_build.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
want, _ = getattr(m, fn)(None)
if want != (hw == "0"):
    raise SystemExit("auto 探测 hw=%s 时 %s=%s" % (hw, fn, want))
PY
  done
done
S2A_DAV1D=1 python3 - "$H" <<'PY'
import importlib.util, os, sys
h = sys.argv[1]
spec = importlib.util.spec_from_file_location("prepare_build", os.path.join(h, "launchers/macos/prepare_build.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
if "meson" in m.FORMULAS or "ninja" in m.FORMULAS:
    raise SystemExit("FORMULAS 不该再含 meson / ninja（dav1d 那份归 DAV1D_FORMULAS）")
PY

# 3) bundle_engine.py：两个 wanted / prebuilt_libs 清单
for pair in "dav1d_wanted S2A_DAV1D tools/av1hw.sh" "vpx_wanted S2A_VPX tools/vp9hw.sh"; do
  set -- $pair; fn=$1; sw=$2; s=$3
  for v in 0 1; do
    env "$sw=$v" python3 - "$H" "$fn" "$v" <<'PY'
import sys
import types
h, fn, v = sys.argv[1], sys.argv[2], sys.argv[3]
for name in ("core", "build", "toolchain"):
    m = types.ModuleType(name)
    if name == "core":
        m.Context = m.atomic = m.digest = m.tree_hash = object
    elif name == "build":
        m.compile_host = m.nospace = object
    sys.modules[name] = m
sys.path.insert(0, h + "/launchers/macos")
import bundle_engine
if getattr(bundle_engine, fn)({"S2A_DAV1D": v, "S2A_VPX": v}) != (v == "1"):
    raise SystemExit("%s = %s 时不符" % (fn, v))
PY
  done
  K=$(shasum -a 256 < "$H/$s" | cut -c1-12); C="$T/be-$(basename "$s" .sh)"; mkdir -p "$C/$K"; printf 1 > "$C/$K/result"
  env "S2A_DAV1D=auto" "S2A_VPX=auto" "S2A_AV1HW_CACHE=$C" "S2A_VPXHW_CACHE=$C" python3 - "$H" "$fn" <<'PY'
import os
import sys
import types
h, fn = sys.argv[1], sys.argv[2]
for name in ("core", "build", "toolchain"):
    m = types.ModuleType(name)
    if name == "core":
        m.Context = m.atomic = m.digest = m.tree_hash = object
    elif name == "build":
        m.compile_host = m.nospace = object
    sys.modules[name] = m
sys.path.insert(0, h + "/launchers/macos")
import bundle_engine
if getattr(bundle_engine, fn)({}):
    raise SystemExit("auto 探测缓存说可走系统（hw=1 → want=0）时 %s 应为 False" % fn)
PY
done
python3 - "$H" <<'PY'
import os
import sys
import types
h = sys.argv[1]
for name in ("core", "build", "toolchain"):
    m = types.ModuleType(name)
    if name == "core":
        m.Context = m.atomic = m.digest = m.tree_hash = object
    elif name == "build":
        m.compile_host = m.nospace = object
    sys.modules[name] = m
sys.path.insert(0, h + "/launchers/macos")
import bundle_engine
for wd, wx in ((True, True), (True, False), (False, True), (False, False)):
    names = [n for n, _ in bundle_engine.prebuilt_libs(wd, wx)]
    if ("dav1d" in names) != wd or ("vpx" in names) != wx:
        raise SystemExit("prebuilt_libs(%s, %s) = %s" % (wd, wx, names))
if bundle_engine.prebuilt_libs(False, False):
    raise SystemExit("prebuilt_libs(False, False) 应为空清单")
PY

# 4) runtime/tests/mk/nvn2_codec.mk：目标在 Darwin 恒注册；libvpx 按 EFF 链真身 / 空垫片、dav1d 只在要链时进链接命令（make -n 读命令行）
if [ "$(uname -s)" = Darwin ]; then
  D2="$T/tests-mk"
  mkdir -p "$D2/runtime/tests/mk"
  cp "$H/runtime/tests/mk/nvn2_codec.mk" "$D2/runtime/tests/mk/nvn2_codec.mk"
  cat > "$D2/Makefile" <<'EOF'
BUILD := build
CC := clang
S2A_UNAME := Darwin
S2A_DAV1D_EFF ?= 1
S2A_VPX_EFF ?= 1
NATIVE_INC :=
NATIVE_TEST_OWN :=
TEST_TARGETS :=
include runtime/tests/mk/nvn2_codec.mk
.PHONY: print
print:
	@printf '%s\n' '$(TEST_TARGETS)'
EOF
  for eff in 0 1; do
    out=$(make -s -C "$D2" S2A_DAV1D_EFF=$eff S2A_VPX_EFF=$eff print)
    case "$out" in *native-test-nvn2_codec*) ;; *) fail "EFF=$eff 没注册 codec 单测（应恒注册）：$out";; esac
    # 命令文本各分支都在（shell 的 if），判据看展开后的条件
    out=$(make -n -C "$D2" S2A_DAV1D_EFF=$eff S2A_VPX_EFF=$eff native-test-nvn2_codec 2>&1)
    pat='if [ "'"$eff"'" != 0 ]; then dav1d=$(bash services/dav1d/build.sh'
    case "$out" in
      *"$pat"*) ;;
      *) fail "dav1d EFF=$eff 的单测命令条件不对（应 \\\"$eff\\\" != 0 才取 dav1d）：$out";;
    esac
    if [ "$eff" = 1 ]; then
      grep -q 'vpxlib=$(bash services/vpx/build.sh osx-arm64 | tail -1)' <<<"$out" || fail "vpx EFF=1 应链真身 build.sh：$out"
    else
      grep -q 'vpxlib=$(bash services/vpx/build_stub.sh osx-arm64 | tail -1)' <<<"$out" || fail "vpx EFF=0 应链空垫片 build_stub.sh：$out"
    fi
  done
fi

# 5) applenv.py：source_build_checks 的 meson 行随 S2A_DAV1D（auto 探测的判据在 0) 已单测，这里只测直说）
for v in 0 1; do
  S2A_DAV1D=$v python3 - "$H" "$v" <<'PY'
import os
import sys
h, v = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(h, "pack/jobs"))
import applenv
names = [r["name"] for r in applenv.source_build_checks(os.environ["PATH"], "/synthetic/Xcode")]
if ("meson" in names) != (v == "1"):
    raise SystemExit("S2A_DAV1D=%s 时 meson 行在 %s" % (v, names))
PY
done

echo "解码软兜底自动探测单测全过"
