#!/bin/bash
# 改写器：跳转表基址按控制流到达定义 + 宽屏 asp 点 Xd 写法（runtime/rewriter/rewriter.c，合成 NSO，约 5 秒）：bash tests/rewrite_jt_cfg_base_test.sh
# 门禁覆盖：runtime/rewriter/*
# 取自 #101 大乱斗 13.0.2 main（10-24 s-leeq 报，改前 5 处缺口、退出码 3）。合成代码段从 0x100 起、一个 FDE 覆盖：
#   0x100 adrp x16 + add x16,#0x800（表在 ro 0x800）/ cbz w3 → 0x114 死路径 mov x16,x4（线性往回最近的写 x16 在这条别的路径上）/ cbnz w5 → 0x160 / b 0x120
#   ① 0x120 cmp w18,#2 / b.hi / ldrsw x18,[x16,x18,lsl#2] / add x18,x18,x16 / 0x130 br x18（形态同 main 0x88d70c）
#   ② 0x160 cmp w14,#2 / b.hi / ldrsw x14,[x16,x14,lsl#2] / add x18,x14,x16 / 0x170 br x18（表项在 x14，形态同 main 0xdeab98）
#   ③ 宽屏 asp 点 0x1a0 mov x12,#0x8e39 / movk x12,#0x3fe3,lsl#16（形态同 main 0x354df5c）
#   ⑥ 表基址经寄存器拷贝（10-24 #101 复测剩的 0x2b73f8c：x16 的到达定义里有 mov x16,x12）：0x200 adrp x12 + add x12,#0x800 / cbz w6 → 两条路径各 mov x16,x12 /
#      0x220 cmp w18,#2 / b.hi / ldrsw x18,[x16,x18,lsl#2] / add x18,x18,x16 / 0x230 br x18；反例：一条路径 mov x16,x4（x4 未知）→ 仍记缺口，原因写明那条 mov
#   ⑦ 拷贝成环（10-24 #101 第二次复测，大乱斗 0x2b73f8c：mov x1,x16 存、另一条路径 mov x16,x1 取回，循环回边把两条串成环）：0x300 adrp x16 + add / 0x308 mov x1,x16 /
#      cbz w6 → 0x314 mov x16,x1 / 0x320 cmp / b.hi → 0x334 cbnz w9 → 0x308（回边）/ ldrsw / add / 0x330 br x18——到达值只有一个（0x1800），应认
# 判据：新改写器苹果 / 安卓 0 缺口、退出码 0、br_tab_open 4、widen_asp 1；0x130 / 0x170 / 0x230 / 0x330 分派目标集 = 手算 0x140,0x148,0x150；widen.tsv = 「0 0x1a0 asp 0xd291c72c」；
#   ④ 反例：asp 点两条不同宽（0x1b0 mov w13,#0x8e39 / movk x13,#0x3fe3,lsl#16）——11-02 起不计缺口：丢掉这个点、记黄、退出码 0
# 指令用 clang 汇编（与被测改写器无关），表项与期望目标手算
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; bad=0
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
PY=${PY:-${S2A_PY:-$HOME/.s2arm/venv/bin/python3}}
REW=${REW:-$(python3 "$H/pack/rewriter.py" c-path 2>/dev/null | tail -1)}; [ -x "$REW" ] || { echo "🔴 取不到 C 改写器（python3 pack/rewriter.py c-path）"; exit 1; }
cat > "$T/c.s" <<'EOF'
.text
.p2align 2
      .inst 0xb0000010
      add x16, x16, #0x800
      cbz w3, adead
      cbnz w5, bgo
      b ago
adead: mov x16, x4
      ret
.org 0x20
ago:  cmp w18, #2
      b.hi aout
      ldrsw x18, [x16, x18, lsl #2]
      add x18, x18, x16
      br x18
      nop
.org 0x40
      ret
      nop
      ret
      nop
      ret
aout: ret
.org 0x60
bgo:  cmp w14, #2
      b.hi bout
      ldrsw x14, [x16, x14, lsl #2]
      add x18, x14, x16
      br x18
      nop
.org 0x80
      ret
      nop
      ret
      nop
      ret
bout: ret
.org 0xa0
      mov x12, #0x8e39
      movk x12, #0x3fe3, lsl #16
      ret
      nop
.org 0xb0
      mov w13, #0x8e39
      movk x13, #0x3fe3, lsl #16
      ret
.org 0x100
      .inst 0xb000000c
      add x12, x12, #0x800
      cbz w6, c2
      mov x16, x12
      b csw
c2:   mov x16, x12
.org 0x120
csw:  cmp w18, #2
      b.hi cout
      ldrsw x18, [x16, x18, lsl #2]
      add x18, x18, x16
      br x18
cout: ret
.org 0x200
      .inst 0xb0000010
      add x16, x16, #0x800
cyl:  mov x1, x16
      cbz w6, cy2
      b cysw
cy2:  mov x16, x1
.org 0x220
cysw: cmp w18, #2
      b.hi cyout
      ldrsw x18, [x16, x18, lsl #2]
      add x18, x18, x16
      br x18
cyout: cbnz w9, cyl
      ret
EOF
xcrun clang -arch arm64 -c "$T/c.s" -o "$T/c.o" || { echo "🔴 汇编失败"; exit 1; }
otool -t "$T/c.o" | tail -n +3 | awk '{for (i = 2; i <= NF; i++) print $i}' > "$T/words"
$PY - "$T/words" "$T/exefs_main" <<'EOF'
import struct, sys
code = [int(w, 16) for w in open(sys.argv[1]).read().split()]
# 模块骨架同 tests/rewrite_excl_head_jt_adr_test.sh：text[4] = MOD0 偏移，MOD0 + 16 = eh_frame_hdr（一个 FDE 覆盖这段代码）
text = bytearray(0x1000); struct.pack_into('<II', text, 0, 0x14000040, 0x1000); struct.pack_into('<%dI' % len(code), text, 0x100, *code)
ro = bytearray(0x1000); EH, EF = 0x100, 0x200
struct.pack_into('<4sIIIii', ro, 0, b'MOD0', 0, 0, 0, EH, EH + 0x20)
struct.pack_into('<BBBBiIii', ro, EH, 1, 0x1B, 0x03, 0x3B, (0x1000 + EF) - (0x1000 + EH + 4), 1, 0x100 - (0x1000 + EH), (0x1000 + EF + 0x18) - (0x1000 + EH))
cie = struct.pack('<II', 0x14, 0) + bytes([1]) + b'zR\0' + bytes([4, 0x78, 30, 1, 0x1B]) + bytes(7)
ro[EF:EF + 0x18] = cie; f = 0x1000 + EF + 0x18
struct.pack_into('<IIii', ro, EF + 0x18, 0x10, (f + 4) - (0x1000 + EF), 0x100 - (f + 8), 4 * len(code))
# 表（ro 0x800 = 映像 0x1800）：项 = 目标 − 表基址；第 4 项放出函数的哨兵，超集读到它即停
struct.pack_into('<4i', ro, 0x800, 0x140 - 0x1800, 0x148 - 0x1800, 0x150 - 0x1800, 0x7ffffff0)
text, ro, data = bytes(text), bytes(ro), bytes(0x1000)
h = bytearray(0x100); h[:4] = b'NSO0'; fo = 0x100
for k, (mo, seg) in enumerate(((0, text), (0x1000, ro), (0x2000, data))):
    struct.pack_into('<III', h, 0x10 + 0x10 * k, fo + 0x1000 * k, mo, len(seg)); struct.pack_into('<I', h, 0x60 + 4 * k, len(seg))
h[0x40:0x50] = bytes(range(16))
open(sys.argv[2], 'wb').write(bytes(h) + text + ro + data)
EOF
printf '模块\t偏移\t种类\t证据\nmain\t0x1a0\tasp\tx\n' > "$T/pos.tsv"; printf '模块\t偏移\t种类\t证据\nmain\t0x1b0\tasp\tx\n' > "$T/neg.tsv"
rew() { local r=$1 o=$2 tsv=$3; shift 3; mkdir -p "$T/$o"; env "$@" $PY "$H/tests/rewriter_exec.py" "$r" "$T/exefs_main" "$T/$o" main "$tsv" > "$T/$o.log" 2>&1; echo $?; }
ra=$(rew "$REW" apple "$T/pos.tsv" S2A_TARGET=apple); rn=$(rew "$REW" android "$T/pos.tsv" S2A_TARGET=android)
for o in apple android; do
  r=$([ $o = apple ] && echo "$ra" || echo "$rn"); [ "$r" = 0 ] || tail -5 "$T/$o.log"
  ng=$(grep -c '^GAP' "$T/$o.log"); st=$(grep -oE "'(br_tab_open|widen_asp)': [0-9]+" "$T/$o.log" | LC_ALL=C sort | tr '\n' ' ')
  wt=$(tr '\t\n' ' ,' < "$T/$o/main.widen.tsv" 2>/dev/null)
  ck "① ${o}：退出码 ${r}、缺口 ${ng}、${st}、widen.tsv「${wt}」" '[ "$r" = 0 ] && [ "$ng" = 0 ] && [ "$st" = "'"'"'br_tab_open'"'"': 4 '"'"'widen_asp'"'"': 1 " ] && [ "$wt" = "0 0x1a0 asp 0xd291c72c," ]'
done
out=$([ "$ra" = 0 ] && $PY - "$T/apple/main.patched.bin" <<'EOF' 2>&1
import struct, sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
b = open(sys.argv[1], 'rb').read(); md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
def tgt(a):
    w, = struct.unpack_from('<I', b, a); imm = w & 0x3ffffff
    return a + 4 * (imm - (1 << 26) if imm >> 25 else imm) if w >> 26 == 0b000101 else None
for site in (0x130, 0x170, 0x230, 0x330):
    t = tgt(site); out = []; pg = None
    for i in md.disasm(b[t:t + 512], t):
        if i.mnemonic == 'adrp' and i.op_str.startswith('x17,'): pg = i.operands[1].imm
        elif i.mnemonic == 'add' and i.op_str.startswith('x17, x17, #') and pg is not None: out.append(pg + i.operands[2].imm); pg = None
        elif i.mnemonic == 'brk': break
    print('通' if out == [0x140, 0x148, 0x150] else '不通', f'{site:#x} 分派目标', ','.join(f'{x:#x}' for x in out), '（手算 0x140,0x148,0x150）')
EOF
); echo "$out" | sed 's/^/  /'
ck "② 四处分派目标都对（脚本出错也算不通）" '[ "$(echo "$out" | grep -c "^通")" = 4 ]'
rg=$(rew "$REW" neg "$T/neg.tsv" S2A_TARGET=apple); gn=$(grep '^GAP' "$T/neg.log" | cut -f3,4 | cut -d: -f1 | tr '\t' ' ' | tr '\n' ';'); sk=$(grep -c '满屏候选 0x1b0 跳过（不计缺口）' "$T/neg.log")
# 11-02 起坏的满屏候选不计缺口（丹干V3 原生档出包因此整单失败）：丢掉这个点、记黄，改写照常出
ck "③ 反例 asp 两条不同宽：退出码 ${rg}、缺口「${gn}」、0x1b0 记黄跳过 ${sk} 行" '[ "$rg" = 0 ] && [ -z "$gn" ] && [ "$sk" = 1 ]'
# ⑤ 反例：⑥ 的一条路径改成 mov x16,x4（x4 静态未知）→ 0x230 仍记缺口，原因写明「到达定义 0x… mov x16, x4」
sed 's/^c2:   mov x16, x12$/c2:   mov x16, x4/' "$T/c.s" > "$T/n.s"
xcrun clang -arch arm64 -c "$T/n.s" -o "$T/n.o" && otool -t "$T/n.o" | tail -n +3 | awk '{for (i = 2; i <= NF; i++) print $i}' > "$T/words"
PYCODE=$(sed -n '/^\$PY - "\$T\/words" "\$T\/exefs_main" <<.EOF.$/,/^EOF$/p' "$0" | sed '1d;$d')
$PY - "$T/words" "$T/exefs_main" <<< "$PYCODE"
rn5=$(rew "$REW" neg5 "$T/pos.tsv" S2A_TARGET=apple); g5=$(grep '^GAP' "$T/neg5.log" | cut -f3,4 | cut -d: -f1 | tr '\t' ' ' | tr '\n' ';')
ck "⑤ 反例 一条路径 mov x16,x4：退出码 ${rn5}、缺口「${g5}」、原因带那条 mov" '[ "$rn5" = 3 ] && [ "$g5" = "0x230 br_x18;" ] && grep "^GAP" "$T/neg5.log" | grep -q "mov x16, x4"'
[ $bad = 0 ] && echo "✅ 跳转表到达定义基址 / asp Xd 写法全通" || { echo "🔴 跳转表到达定义基址 / asp Xd 写法有不通项"; exit 1; }
