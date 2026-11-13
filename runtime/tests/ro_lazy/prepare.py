"""nn::ro 懒绑定交接夹具（#858）：合成一个自有 NSO（不含游戏代码 / 数据），交出包缺省 C 改写器改写成 main。
模块布局照游戏 SDK 的 nn::ro（rtld 交接看的那几样），各函数体只放交接用得到的指令：
  代码 0x100 target：mov w0,#7; ret（JUMP_SLOT 引用的函数）
       0x200 PLT 表头：stp x16,x30,[sp,#-16]!; ret（槽的链接时原值指这里）
       0x300 nn::ro::detail::RoModule::Initialize(对象, 基址, 大小, 动态段, 布尔)：同 SDK 记基址 / 动态段、把 +0xc8 清零
       0x400 nn::ro::detail::RoModule::RestoreFunctions：ldr x8,[x0,#0xc8]; str x8,[x12,x13]; ret（SDK 把函数槽改回懒绑定入口的那两条）
       0x500 nn::ro::BindEntry：ret
  只读 0x1000 MOD0（+0x18 = 模块对象相对 MOD0 的偏移 → 数据 0x2100）、0x1300 动态段（DT_JMPREL / DT_PLTRELSZ / DT_PLTGOT = 0x2000）、
       0x1500 一条 JUMP_SLOT（槽 0x2018 = GOT[3] → target）、0x1600 符号表、紧跟字符串表
  数据 0x2000 GOT（GOT[3] 文件原值 0x200 = PLT 表头）、0x2100 模块对象（0xd0 字节）、0x2200 起五个交接全局
用法：prepare.py <输出目录> [old|early]；输出目录写 main.* 改写产物，另写 layout.tsv（单测读的期望偏移）
  old：老 SDK 的形态（11-13 一款第一方游戏的 SDK）——函数名单数 RoModule::RestoreFunction、懒绑定入口字段在 +0xb0（新 SDK 复数名、+0xc8）
  early：新 SDK 另一种块布局（11-14 本机 6 款里 4 款的 sdk 如此）——RestoreFunctions 开头先是「表为空直接返回」的早退 ret，
         ldr / str 在它后面：cbnz x1, +8; ret; ldr x8,[x0,#0xc8]; str x8,[x12,x13]; ret
  各函数符号的 st_size 写真实长度（SDK 导出的函数符号带真实大小，交接按它圈定扫描范围）"""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

root = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
MODE = sys.argv[2] if len(sys.argv) > 2 else ''
assert MODE in ('', 'old', 'early'), MODE
OLD = MODE == 'old'
STUB = 0xb0 if OLD else 0xc8   # 模块对象里懒绑定入口字段的偏移
shutil.rmtree(out, ignore_errors=True); out.mkdir(parents=True)
RET = 0xd65f03c0
TEXT, RO, DATA = 0, 0x1000, 0x2000
TARGET, PLT0, INIT, RESTORE, BIND = 0x100, 0x200, 0x300, 0x400, 0x500
GOT, OBJ, GLOBALS = DATA, DATA + 0x100, DATA + 0x200
SLOT = GOT + 0x18

text = bytearray(0x1000)
code_size = {}   # 函数入口 → 真实字节数（写进符号的 st_size）
struct.pack_into('<II', text, 0, 0x14000040, RO)   # 入口跳过表头；text[4] = MOD0 偏移
for off, code in ((TARGET, [0x528000e0, RET]),                          # mov w0, #7
                  (PLT0, [0xa9bf7bf0, RET]),                            # stp x16, x30, [sp, #-16]!
                  (INIT, [0xf9000801, 0xf9000c03, 0xf900001f | (STUB // 8) << 10, RET]),   # str x1,[x0,#0x10]; str x3,[x0,#0x18]; str xzr,[x0,#STUB]
                  (RESTORE, ([0xb5000041, RET] if MODE == 'early' else [])   # early：cbnz x1, +8; ret（早退路径在前）
                            + [0xf9400008 | (STUB // 8) << 10, 0xf82d6988, RET]),         # ldr x8,[x0,#STUB]; str x8,[x12,x13]
                  (BIND, [RET])):
    struct.pack_into('<%dI' % len(code), text, off, *code)
    code_size[off] = 4 * len(code)

ro = bytearray(0x1000)
EH, EF, DYN, RELA, SYM = 0x100, 0x200, 0x300, 0x500, 0x600
struct.pack_into('<4sIIIiii', ro, 0, b'MOD0', DYN, 0, 0, EH, EH + 0x20, OBJ - RO)
# eh_frame_hdr + 一个 CIE + 一个 FDE 覆盖 0x100..0x600（同 android/tools/selftest_nso.py 的骨架）
struct.pack_into('<BBBBiIii', ro, EH, 1, 0x1B, 0x03, 0x3B, (RO + EF) - (RO + EH + 4), 1, TARGET - (RO + EH), (RO + EF + 0x18) - (RO + EH))
ro[EF:EF + 0x18] = struct.pack('<II', 0x14, 0) + bytes([1]) + b'zR\0' + bytes([4, 0x78, 30, 1, 0x1B]) + bytes(7)
f = RO + EF + 0x18
struct.pack_into('<IIii', ro, EF + 0x18, 0x10, (f + 4) - (RO + EF), TARGET - (f + 8), BIND + 4 - TARGET)

names = [('target', 0x12, 1, TARGET),
         ('_ZN2nn2ro6detail8RoModule10InitializeEmmPKNS1_3Elf5Elf643DynEb', 0x12, 1, INIT),
         ('_ZN2nn2ro6detail8RoModule15RestoreFunctionEPS2_' if OLD else '_ZN2nn2ro6detail8RoModule16RestoreFunctionsEPS2_', 0x12, 1, RESTORE),
         ('_ZN2nn2ro9BindEntryEv', 0x12, 1, BIND),
         ('_ZN2nn2ro6detail15g_pAutoLoadListE', 0x11, 3, GLOBALS),
         ('_ZN2nn2ro6detail17g_pManualLoadListE', 0x11, 3, GLOBALS + 8),
         ('_ZN2nn2ro6detail14g_pRoDebugFlagE', 0x11, 3, GLOBALS + 0x10),
         ('_ZN2nn2ro6detail34g_pLookupGlobalAutoFunctionPointerE', 0x11, 3, GLOBALS + 0x18),
         ('_ZN2nn2ro6detail36g_pLookupGlobalManualFunctionPointerE', 0x11, 3, GLOBALS + 0x20)]
strings = bytearray(b'\0'); syms = [bytes(24)]
for name, info, shndx, value in names:
    syms.append(struct.pack('<IBBHQQ', len(strings), info, 0, shndx, value, code_size[value] if shndx == 1 else 8)); strings += name.encode() + b'\0'
symtab = b''.join(syms); STR = SYM + len(symtab)
assert STR + len(strings) <= 0x1000
ro[SYM:STR] = symtab; ro[STR:STR + len(strings)] = strings
# 动态段：DT_SYMTAB 6、DT_STRTAB 5、DT_JMPREL 23、DT_PLTRELSZ 2、DT_PLTGOT 3、DT_NULL
struct.pack_into('<12Q', ro, DYN, 6, RO + SYM, 5, RO + STR, 23, RO + RELA, 2, 24, 3, GOT, 0, 0)
struct.pack_into('<QQq', ro, RELA, SLOT, (1 << 32) | 1026, 0)   # R_AARCH64_JUMP_SLOT → 符号 1（target）

data = bytearray(0x1000)
struct.pack_into('<Q', data, SLOT - DATA, PLT0)   # 链接时原值 = PLT 表头

h = bytearray(0x100); h[:4] = b'NSO0'
for k, (mo, seg) in enumerate(((TEXT, text), (RO, ro), (DATA, data))):
    struct.pack_into('<III', h, 0x10 + 0x10 * k, 0x100 + 0x1000 * k, mo, len(seg))
    struct.pack_into('<I', h, 0x60 + 4 * k, len(seg))
nso = out / 'ro_lazy.nso'
nso.write_bytes(bytes(h) + bytes(text) + bytes(ro) + bytes(data))
rewriter = subprocess.check_output([sys.executable, str(root / 'pack/rewriter.py'), 'c-path'], text=True).strip()
subprocess.run([rewriter, str(nso), str(out), 'main'], env=dict(os.environ, S2A_TARGET='apple'), check=True, stdout=subprocess.DEVNULL)
with open(out / 'layout.tsv', 'w') as fo:
    for k, v in (('obj', OBJ), ('plt0', PLT0), ('bind', BIND), ('got', GOT), ('slot', SLOT), ('target', TARGET), ('stub', STUB)): fo.write(f'{k}\t{v:#x}\n')
