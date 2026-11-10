#!/bin/bash
# 改写器三种新认形态单测（runtime/rewriter/rewriter.c，合成 NSO，约 5 秒）：bash tests/rewrite_excl_head_jt_adr_test.sh
# 门禁覆盖：runtime/rewriter/*
# 取自王国之泪 1.4.3 main（10-24，改前 17 处缺口、整个模块退出码 3）。合成代码段从 0x100 起：
#   ① 外置重试头：窗口 0x100 ldxr / cmp / b.ne / add w18 / 0x110 stxr / cbnz → 0x140；0x140 ldxr / cmp / 0x148 b.eq → 0x110（形态同 main 0x18598 → 0x181fc）
#   ② adr 作基址的偏移表：0x200 adrp x17 + add x17,#0x800（表在 ro 0x800）/ cbz w3 → 另一路径 mov w17,w4（死路径改了 x17）/ 0x220 cmp w15,#2 / b.hi /
#      mov w16,w15 / 0x22c adr x18,0x22c / ldrsw x0,[x17,x16,lsl#2] / add x18,x18,x0 / 0x238 br x18 → 0x240 / 0x248 / 0x250（形态同 main 0x5f2a9c）
#   ③ 远 adr + 半字表：0x300 adrp x13 + add x13,#0x900 / cmp w12,#2 / b.hi / 0x310 adr x18,0x360 / ldrh w14,[x13,x12,lsl#1] / add x18,x18,x14,lsl#2 /
#      9 条 mov / 0x340 br x18（adr 在 br 前 12 条，形态同 main 0x680528）
# 判据：新改写器苹果 / 安卓 0 缺口、退出码 0、window_head 1 / br_tab_open 1 / br 1；② 窗口两段互跳都落在对方副本里（cbnz → 重试头副本的 ldxr，b.eq → 主段副本的 stxr）；
#   ③ 0x238 的分派目标集 = 手算 0x240,0x248,0x250
# 指令用 clang 汇编（与被测改写器无关），表项与期望目标手算
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; bad=0
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
PY=${PY:-${S2A_PY:-$HOME/.s2arm/venv/bin/python3}}
REW=${REW:-$(python3 "$H/pack/rewriter.py" c-path 2>/dev/null | tail -1)}; [ -x "$REW" ] || { echo "🔴 取不到 C 改写器（python3 pack/rewriter.py c-path）"; exit 1; }
cat > "$T/c.s" <<'EOF'
.text
.p2align 2
w0:   ldxr w17, [x10]
      cmp w15, w17
      b.ne wfail
      add w18, w15, #1
wst:  stxr w17, w18, [x10]
      cbnz w17, whead
      ret
wfail: ret
.org 0x40
whead: ldxr w17, [x10]
      cmp w15, w17
      b.eq wst
      b wfail
.org 0x100
      .inst 0xb0000011
      add x17, x17, #0x800
      cbz w3, p2dead
      b p2go
p2dead: mov w17, w4
      ret
.org 0x120
p2go: cmp w15, #2
      b.hi p2out
      mov w16, w15
p2L:  adr x18, p2L
      ldrsw x0, [x17, x16, lsl #2]
      add x18, x18, x0
      br x18
      nop
.org 0x140
      ret
      nop
      ret
      nop
      ret
p2out: ret
.org 0x200
      .inst 0xb000000d
      add x13, x13, #0x900
      cmp w12, #2
      b.hi p3out
      adr x18, p3B
      ldrh w14, [x13, x12, lsl #1]
      add x18, x18, x14, lsl #2
      mov w0, w4
      mov w1, w4
      mov w2, w4
      mov w3, w4
      mov w5, w4
      mov w6, w4
      mov w7, w4
      mov w8, w4
      mov w9, w4
      br x18
      nop
      nop
      nop
      nop
      nop
      nop
      nop
p3B:  ret
      nop
      ret
      nop
      ret
p3out: ret
EOF
xcrun clang -arch arm64 -c "$T/c.s" -o "$T/c.o" || { echo "🔴 汇编失败"; exit 1; }
otool -t "$T/c.o" | tail -n +3 | awk '{for (i = 2; i <= NF; i++) print $i}' > "$T/words"
$PY - "$T/words" "$T/exefs_main" <<'EOF'
import struct, sys
code = [int(w, 16) for w in open(sys.argv[1]).read().split()]
# 模块骨架同 tests/rewrite_jt_open_test.sh：text[4] = MOD0 偏移，MOD0 + 16 = eh_frame_hdr（一个 FDE 覆盖这段代码）
text = bytearray(0x1000); struct.pack_into('<II', text, 0, 0x14000040, 0x1000); struct.pack_into('<%dI' % len(code), text, 0x100, *code)
ro = bytearray(0x1000); EH, EF = 0x100, 0x200
struct.pack_into('<4sIIIii', ro, 0, b'MOD0', 0, 0, 0, EH, EH + 0x20)
struct.pack_into('<BBBBiIii', ro, EH, 1, 0x1B, 0x03, 0x3B, (0x1000 + EF) - (0x1000 + EH + 4), 1, 0x100 - (0x1000 + EH), (0x1000 + EF + 0x18) - (0x1000 + EH))
cie = struct.pack('<II', 0x14, 0) + bytes([1]) + b'zR\0' + bytes([4, 0x78, 30, 1, 0x1B]) + bytes(7)
ro[EF:EF + 0x18] = cie; f = 0x1000 + EF + 0x18
struct.pack_into('<IIii', ro, EF + 0x18, 0x10, (f + 4) - (0x1000 + EF), 0x100 - (f + 8), 4 * len(code))
# ② 表（ro 0x800）：项 = 目标 − adr 处 0x22c；第 4 项放出函数的哨兵，超集读到它即停
struct.pack_into('<4i', ro, 0x800, 0x240 - 0x22c, 0x248 - 0x22c, 0x250 - 0x22c, 0x7ffffff0)
# ③ 半字表（ro 0x900）：目标 = 0x360 + 4 × 项
struct.pack_into('<3H', ro, 0x900, 0, 2, 4)
text, ro, data = bytes(text), bytes(ro), bytes(0x1000)
h = bytearray(0x100); h[:4] = b'NSO0'; fo = 0x100
for k, (mo, seg) in enumerate(((0, text), (0x1000, ro), (0x2000, data))):
    struct.pack_into('<III', h, 0x10 + 0x10 * k, fo + 0x1000 * k, mo, len(seg)); struct.pack_into('<I', h, 0x60 + 4 * k, len(seg))
h[0x40:0x50] = bytes(range(16))
open(sys.argv[2], 'wb').write(bytes(h) + text + ro + data)
EOF
rew() { local r=$1 o=$2; shift 2; mkdir -p "$T/$o"; env "$@" $PY "$H/tests/rewriter_exec.py" "$r" "$T/exefs_main" "$T/$o" main > "$T/$o.log" 2>&1; echo $?; }
ra=$(rew "$REW" apple S2A_TARGET=apple); rn=$(rew "$REW" android S2A_TARGET=android)
for o in apple android; do
  r=$([ $o = apple ] && echo "$ra" || echo "$rn"); [ "$r" = 0 ] || tail -5 "$T/$o.log"
  ng=$(grep -c '^GAP' "$T/$o.log"); st=$(grep -oE "'(window_head|br_tab_open|br)': [0-9]+" "$T/$o.log" | LC_ALL=C sort | tr '\n' ' ')
  ck "① ${o}：退出码 ${r}、缺口 ${ng}、${st}" '[ "$r" = 0 ] && [ "$ng" = 0 ] && [ "$st" = "'"'"'br'"'"': 1 '"'"'br_tab_open'"'"': 1 '"'"'window_head'"'"': 1 " ]'
done
chk() { $PY - "$T/apple/main.patched.bin" <<'EOF'
import struct, sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
b = open(sys.argv[1], 'rb').read(); md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
def tgt(a):
    w, = struct.unpack_from('<I', b, a); imm = w & 0x3ffffff
    return a + 4 * (imm - (1 << 26) if imm >> 25 else imm) if w >> 26 == 0b000101 else None
def first(a, *mn):   # 独占存补 release（rel_upgrade）后副本里是 stlxr：两种写法都认
    for i in md.disasm(b[a:a + 256], a):
        if i.mnemonic in mn: return i
p0, p1 = tgt(0x100), tgt(0x140)
cb, ld1, beq, st0 = first(p0, 'cbnz'), first(p1, 'ldxr'), first(p1, 'b.eq'), first(p0, 'stxr', 'stlxr')
ok = p0 and p1 and int(cb.op_str.split('#')[-1], 16) == ld1.address and int(beq.op_str.split('#')[-1], 16) == st0.address
print('通' if ok else '不通', f'主段入口 {p0:#x}、重试头入口 {p1:#x}；cbnz → {cb.op_str}（重试头副本 ldxr {ld1.address:#x}），b.eq → {beq.op_str}（主段副本 stxr {st0.address:#x}）')
t = tgt(0x238); out = []; pg = None
for i in md.disasm(b[t:t + 512], t):
    if i.mnemonic == 'adrp' and i.op_str.startswith('x17,'): pg = i.operands[1].imm
    elif i.mnemonic == 'add' and i.op_str.startswith('x17, x17, #') and pg is not None: out.append(pg + i.operands[2].imm); pg = None
    elif i.mnemonic == 'brk': break
print('通' if out == [0x240, 0x248, 0x250] else '不通', '0x238 分派目标', ','.join(f'{x:#x}' for x in out), '（手算 0x240,0x248,0x250）')
EOF
}
out=$([ "$ra" = 0 ] && chk 2>&1); echo "$out" | sed 's/^/  /'
ck "②③ 副本互跳与分派目标两项都通（脚本出错也算不通）" '[ "$(echo "$out" | grep -c "^通")" = 2 ]'
# ④ 反例（10-24 补）：远 adr + 半字表，但 add 之后 mov x18,x0 又改了 x18 → br 跳的不是表算出的地址，必须仍记缺口、退出码 3。
cat > "$T/n.s" <<'EOF2'
.text
.p2align 2
.org 0x200
      .inst 0xb000000d
      add x13, x13, #0x900
      cmp w12, #2
      b.hi nout
      adr x18, nB
      ldrh w14, [x13, x12, lsl #1]
      add x18, x18, x14, lsl #2
      mov x18, x0
      mov w1, w4
      mov w2, w4
      mov w3, w4
      mov w5, w4
      mov w6, w4
      mov w7, w4
      mov w8, w4
      br x18
nB:   ret
nout: ret
EOF2
xcrun clang -arch arm64 -c "$T/n.s" -o "$T/n.o" && otool -t "$T/n.o" | tail -n +3 | awk '{for (i = 2; i <= NF; i++) print $i}' > "$T/words"
cp "$T/exefs_main" "$T/pos_main"
PYCODE=$(sed -n '/^\$PY - "\$T\/words" "\$T\/exefs_main" <<.EOF.$/,/^EOF$/p' "$0" | sed '1d;$d')
$PY - "$T/words" "$T/exefs_main" <<< "$PYCODE"
rn5=$(rew "$REW" neg5 S2A_TARGET=apple); g5=$(grep '^GAP' "$T/neg5.log" | cut -f3,4 | cut -d: -f1 | tr '\t' ' ')
ck "④ 反例 x18 中途被改：退出码 ${rn5}、缺口「${g5}」" '[ "$rn5" = 3 ] && [ "$g5" = "0x33c br_x18" ]'
[ $bad = 0 ] && echo "✅ 外置重试头 / adr 基址跳转表识别全通" || { echo "🔴 外置重试头 / adr 基址跳转表有不通项"; exit 1; }
