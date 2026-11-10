#!/bin/bash
# 改写器自旋区识别单测（runtime/rewriter/rewriter.c「自旋等待」，合成 NSO，约 3 秒）：bash tests/rewrite_spin_test.sh
# 门禁覆盖：runtime/rewriter/*
# 合成代码段从 0x100 起（期望按 AArch64 语义手算，不跑被测实现）：
#   A 0x100 ldr w8,[x0] / 0x104 cbz w8,0x100                         只读、离不开 → 自旋区，头 0x100、回边 0x104
#   B 0x108 ldr w9,[x1] / 0x10c str w9,[x2] / 0x110 cbz w9,0x108     有写内存 → 不是
#   C 0x114 sub w3,w3,#1 / 0x118 cbnz w3,0x114                      计数（T 处活着的 w3 每圈被写）→ 不是
#   E 0x11c ldr w10,[x0] / 0x120 cmp w10,#1 / 0x124 b.ne 0x11c      只读、离不开 → 自旋区，头 0x11c、回边 0x124
#   0x128 ret
# ① 缺省（不设 S2A_REWRITE_SPIN）：退出码 0，sites.tsv 无 S 行，gates.tsv 无 spin，回边原字不动
# ② S2A_REWRITE_SPIN=1：退出码 0，S 行正好 0x100 / 0x11c 两个（回边 0x104 / 0x124），gates.tsv 两个 spin（原址 0x100 / 0x11c）
# ③ 拷贝形状（A）：0x104 改成 B → 跳板「cbz w8,<拷贝头>; b 0x108」；拷贝头「ldr w8,[x0]; cbz w8,<闸>」；闸以 stp x29,x30,[sp,#-16]! 开头、末条 b 回拷贝头
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; bad=0
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
PY=${PY:-${S2A_PY:-$HOME/.s2arm/venv/bin/python3}}   # 改写器钉死的解释器（tools/rewriter-requirements.txt）
REW=${REW:-$(python3 "$H/pack/rewriter.py" c-path 2>/dev/null | tail -1)}; [ -x "$REW" ] || { echo "不通 取不到 C 改写器（python3 pack/rewriter.py c-path）"; exit 1; }
mkdir -p "$T/m/exefs"; $PY - "$T/m/exefs/main" <<'PY'
import struct, sys
code = [0xb9400008, 0x34ffffe8,                 # A：ldr w8,[x0]; cbz w8,-4
        0xb9400029, 0xb9000049, 0x34ffffc9,     # B：ldr w9,[x1]; str w9,[x2]; cbz w9,-8
        0x51000463, 0x35ffffe3,                 # C：sub w3,w3,#1; cbnz w3,-4
        0xb940000a, 0x7100055f, 0x54ffffc1,     # E：ldr w10,[x0]; cmp w10,#1; b.ne -8
        0xd65f03c0]                             # ret
# 模块骨架同 tests/rewrite_jt_open_test.sh：text[4] = MOD0 偏移，MOD0 + 16 = eh_frame_hdr（一个 FDE 覆盖这段代码）
text = bytearray(0x1000); struct.pack_into('<II', text, 0, 0x14000040, 0x1000); struct.pack_into('<%dI' % len(code), text, 0x100, *code)
ro = bytearray(0x1000); EH, EF = 0x100, 0x200
struct.pack_into('<4sIIIii', ro, 0, b'MOD0', 0, 0, 0, EH, EH + 0x20)
struct.pack_into('<BBBBiIii', ro, EH, 1, 0x1B, 0x03, 0x3B, (0x1000 + EF) - (0x1000 + EH + 4), 1, 0x100 - (0x1000 + EH), (0x1000 + EF + 0x18) - (0x1000 + EH))
cie = struct.pack('<II', 0x14, 0) + bytes([1]) + b'zR\0' + bytes([4, 0x78, 30, 1, 0x1B]) + bytes(7)
ro[EF:EF + 0x18] = cie; f = 0x1000 + EF + 0x18
struct.pack_into('<IIii', ro, EF + 0x18, 0x10, (f + 4) - (0x1000 + EF), 0x100 - (f + 8), 4 * len(code))
text, ro, data = bytes(text), bytes(ro), bytes(0x1000)
h = bytearray(0x100); h[:4] = b'NSO0'; fo = 0x100
for k, (mo, seg) in enumerate(((0, text), (0x1000, ro), (0x2000, data))):
    struct.pack_into('<III', h, 0x10 + 0x10 * k, fo + 0x1000 * k, mo, len(seg)); struct.pack_into('<I', h, 0x60 + 4 * k, len(seg))
h[0x40:0x50] = bytes(range(16))
open(sys.argv[1], 'wb').write(bytes(h) + text + ro + data)
PY
rew() { local o=$1; shift; mkdir -p "$T/$o"; env -u S2A_REWRITE_SPIN "$@" $PY "$H/tests/rewriter_exec.py" "$REW" "$T/m/exefs/main" "$T/$o" main > "$T/$o.log" 2>&1; echo $?; }
r0=$(rew off); r1=$(rew on S2A_REWRITE_SPIN=1)
[ "$r0" = 0 ] || tail -5 "$T/off.log"; [ "$r1" = 0 ] || tail -5 "$T/on.log"
w104() { $PY -c "import struct,sys;print(hex(struct.unpack_from('<I',open(sys.argv[1],'rb').read(),0x104)[0]))" "$T/$1/main.patched.bin"; }
s0=$(grep -c $'\tS\t' "$T/off/main.sites.tsv"); g0=$(grep -c $'\tspin\t' "$T/off/main.gates.tsv"); o0=$(w104 off)
ck "① 缺省：退出码 ${r0}、S 行 ${s0}、spin 门 ${g0}、0x104 原字 ${o0}" '[ "${r0}" = 0 ] && [ "${s0}" = 0 ] && [ "${g0}" = 0 ] && [ "${o0}" = 0x34ffffe8 ]'
sx=$(grep $'\tS\t' "$T/on/main.sites.tsv" | tr '\t' ' ' | tr '\n' ';'); gx=$(awk -F'\t' '$2=="spin"{print $4}' "$T/on/main.gates.tsv" | LC_ALL=C sort | tr '\n' ' ')
ck "② 开：退出码 ${r1}、S 行「${sx}」、spin 门原址「${gx}」" '[ "${r1}" = 0 ] && [ "${sx}" = "0x100 S 0x104;0x11c S 0x124;" ] && [ "${gx}" = "0x100 0x11c " ]'
shape=$($PY - "$T/on/main.patched.bin" <<'PY'
import struct, sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
b = open(sys.argv[1], 'rb').read(); md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
def ins(a): return next(md.disasm(b[a:a + 4], a))
def tgt(i): return int(i.op_str.split('#')[-1], 16)
i = ins(0x104); assert i.mnemonic == 'b', i.mnemonic
st = tgt(i); a, c = ins(st), ins(st + 4)
assert a.mnemonic == 'cbz' and a.op_str.startswith('w8,') and c.mnemonic == 'b' and tgt(c) == 0x108, (a.mnemonic, a.op_str, c.op_str)
cp = tgt(a); l, z = ins(cp), ins(cp + 4)
assert l.mnemonic == 'ldr' and l.op_str == 'w8, [x0]' and z.mnemonic == 'cbz' and z.op_str.startswith('w8,'), (l.op_str, z.op_str)
g = tgt(z); s = ins(g)
assert s.mnemonic == 'stp' and s.op_str == 'x29, x30, [sp, #-0x10]!', s.op_str
for k in range(1, 40):
    e = ins(g + 4 * k)
    if e.mnemonic == 'b': assert tgt(e) == cp, hex(tgt(e)); break
else: raise SystemExit('闸末没有 b 回拷贝头')
print('ok')
PY
)
ck "③ 拷贝形状：$shape" '[ "$shape" = ok ]'
[ $bad = 0 ] && echo "✅ 自旋区识别全通" || { echo "🔴 自旋区识别有不通项"; exit 1; }
