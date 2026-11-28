#!/usr/bin/env python3
"""直连 NVN 函数覆盖全表（10-25 维护者第 4 步）：直连层认得的全部函数名（graphics/nvn/gfx/nvn/names.txt）逐个归类，
   已实现 = hle_nvn.c 有 NVN(名, …) 或 NVN_NOPROTO(名, …) 定义（两个宏都登记进 __DATA,swnvn，nvn_init_slots 填槽）；部分 = 定义体里的红行写明有情形没实现（not implemented / nothing drawn / not captured，
   如 DrawTransformFeedback、高级混合、MapVirtual 的非连续映射；参数校验的红行不算），或 BACKEND_PARTIAL 明列的回放层限制；
   空操作 = g_nops 名单（语义确实可空）；未实现 = 其余（调用即报红、记直连异常（不切换），hle_nvn.c nvn_unimpl_c）。
   给了局目录就并上调用频度：各局 host.log 里「NVN unimplemented function called: 名」与部分实现的红行各数一遍。
用法：nvn_func_table.py [--md] [局目录 ...]   不给 --md 出 TSV（名 / 类 / 被调的局数）"""
import os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__)); NVN = os.path.join(HERE, '..', 'gfx', 'nvn')
# 这些入口只录制参数；限制在 nvn_mtl.m:clear_tex 的 RED_ONCE(44)，不在 NVN 宏体内。
# 目前只支持整 mip、格式所有通道，局部区域或部分通道仍报红。
BACKEND_PARTIAL = {
    'nvnCommandBufferClearTexture',
    'nvnCommandBufferClearTexturei',
    'nvnCommandBufferClearTextureui',
    'nvnCommandBufferSetProvokingVertexMode',  # Android Vulkan 可用，Metal 仍是桩
}

def classes():
    names = [l.strip() for l in open(os.path.join(NVN, 'names.txt')) if l.strip()]
    src = open(os.path.join(NVN, 'hle_nvn.c')).read()
    body = {}
    for m in re.finditer(r'^NVN(?:_NOPROTO)?\((\w+),', src, re.M):   # 定义体 = 到下一个 NVN( / NVN_NOPROTO( 或空行为止
        end = re.search(r'^(?:NVN(?:_NOPROTO)?\(|\s*$)', src[m.end():], re.M)
        body[m.group(1)] = src[m.start():m.end() + (end.start() if end else len(src))]
    nops = set(re.findall(r'"(nvn\w+)"', src[src.index('g_nops[] = {'):src.index('};', src.index('g_nops[] = {'))]))
    out = {}
    for n in names:
        part = n in body and (n in BACKEND_PARTIAL or any(re.search(r'not implemented|nothing (?:drawn|captured)|not captured', r) for r in re.findall(r'"【红】[^"]*', body[n])))
        out[n] = '空操作' if n in nops else ('部分' if part else '已实现') if n in body else '未实现'
    return out

def calls(dirs):
    hit = {}
    for d in dirs:
        try: log = open(os.path.join(d, 'host.log'), errors='replace').read()
        except OSError: continue
        for n in set(re.findall(r'NVN unimplemented function called: (nvn\w+)', log)): hit.setdefault(n, set()).add(os.path.basename(d.rstrip('/')))
        if 'DrawTransformFeedback: drawing the captured vertex count is not implemented' in log: hit.setdefault('nvnCommandBufferDrawTransformFeedback', set()).add(os.path.basename(d.rstrip('/')))
    return hit

def main(a):
    md = '--md' in a; dirs = [x for x in a if x != '--md']
    c = classes(); h = calls(dirs)
    if not md:
        for n, k in c.items(): print(f'{n}\t{k}\t{len(h.get(n, ()))}')
        return
    cnt = {k: sum(v == k for v in c.values()) for k in ('已实现', '部分', '空操作', '未实现')}
    print(f'共 {len(c)} 个：' + '、'.join(f'{k} {v}' for k, v in cnt.items()) + f'（扫了 {len(dirs)} 局日志）\n')
    print('| 函数 | 类 | 被调到的局 |\n|---|---|---|')
    for n, k in sorted(c.items(), key=lambda x: (-len(h.get(x[0], ())), x[1] != '部分', x[0])):
        if k in ('部分',) or n in h: print(f'| {n} | {k} | {"、".join(sorted(h.get(n, ()))) or "—"} |')

if __name__ == '__main__': main(sys.argv[1:])
