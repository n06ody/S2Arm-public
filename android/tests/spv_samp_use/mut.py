# 一次性诊断夹具：把一份合法 SPIR-V 改坏，用法 mut.py <输入.spv> <输出.spv> <方式>
#   img_oob：OpImage 结果号改成 bound；load_oob：OpLoad 结果号改成 bound；ptr_short：OpTypePointer 少 pointee（3 字 + OpNop 补齐）；
#   ptr_tail：同上但把短指针挪到模块末尾（外层字数合法、后面没有字）；dup_id：OpVariable %o 的结果号改成 %t 的；dup_dec：Binding 装饰重复一条
import struct, sys
src, dst, mode = sys.argv[1:]
w = list(struct.unpack('<%dI' % (len(open(src, 'rb').read()) // 4), open(src, 'rb').read()))
ins = []; pos = 5
while pos < len(w):
    c = w[pos] >> 16; ins.append((pos, w[pos] & 0xffff, w[pos:pos + c])); pos += c
def first(op): return next(i for i in ins if i[1] == op)
bound = w[3]
if mode == 'img_oob': p, _, q = first(100); w[p + 2] = bound
elif mode == 'load_oob': p, _, q = first(61); w[p + 2] = bound
elif mode in ('ptr_short', 'ptr_tail'):
    var = first(59); ptr_id = var[2][1]
    p, _, q = next(i for i in ins if i[1] == 32 and i[2][1] == ptr_id)
    short = [(3 << 16) | 32, q[1], q[2]]
    if mode == 'ptr_short': w[p:p + 4] = short + [(1 << 16) | 0]
    else: w[p:p + 4] = [(1 << 16) | 0] * 4; w += short
elif mode == 'dup_id':
    vs = [i for i in ins if i[1] == 59]; t = next(i for i in vs if i[2][3] == 0); o = next(i for i in vs if i[2][3] != 0); w[o[0] + 2] = t[2][2]
elif mode == 'dup_dec':
    p, _, q = next(i for i in ins if i[1] == 71 and i[2][2] == 33); w[p + len(q):p + len(q)] = q
else: sys.exit('未知方式 ' + mode)
open(dst, 'wb').write(struct.pack('<%dI' % len(w), *w))
