#!/usr/bin/env python3
"""取范围索引（#1133）与池查找备忘（#974）的点变异：每个变异体在 build/ 下的临时副本里各编一份，对应的单测必须红在指定的断言上；源码树不动。

  find_span 的有序索引（nvn_dirty.c）→ t_dirty_span_index.c（直接包含变异副本，只编这一个文件）
  池查找的备忘（rp_entry.c）          → t_nvn_replay.c（与 native-test-nvn_replay 同款编法，只把 rp_entry.c 换成变异副本）

放不进来的（宿主上无法确定性触发，或去掉后答案不变）：
  - 写者开工处那道 release 栅栏（table_write_begin 里的 atomic_thread_fence）与读者复查前的 acquire 栅栏（find_span 里）：只约束弱序机器上的可见顺序，
    去掉它们，x86 宿主上答案不变；由 t_dirty_span_index.c 的并发用例（每次登记 / 注销都把永远在的条目整体挪一位）做压力，不在这里声称能杀。
  - 索引条目的 relaxed 原子读写改成普通读写：宿主上结果不变（TSan 才看得出），不在这里声称能杀。
  - 池备忘的可答区间 [lo, lim)：池互不重叠时去掉它答案不变，池嵌套时会把内层地址答成外层的池、或让外层地址每次回落锁内；已由「快路不看可答区间」变异体覆盖。

「红在哪条断言」的判法：期望可以给几条同义断言（元组），任一条出现就算拦在了对的性质上；同一条性质在用例里往往不止一处断言
（例如「装不下就不算命中」有越过池尾、池间空隙两处），不把「必须是哪一条」钉死，用例加项、挪位置时才不会误红。
变异体若在红行之后崩溃，红行要已经刷出到管道里才看得到（t_nvn_replay.c 的 CHECK 红行立刻 fflush；t_dirty_span_index.c 的红行写 stderr，不缓冲）；判红在别处时，
报错会带上进程怎么结束的（退出码 / 被哪个信号杀）和最先的几条红行，不只是输出末尾。

用法：python3 dirty_span_memo_mutation_test.py [dirty|pool]   （缺省两半都跑；dirty = 取范围索引那半，pool = 池备忘那半）
"""
import os
import re
import subprocess
import sys
from pathlib import Path

CRASH = object()   # 期望表里的特殊项：进程被信号杀掉也算拦住（只给会越界写的变异体用）

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/span-memo-mutations'
OUT.mkdir(parents=True, exist_ok=True)
CC = os.environ.get('CC', 'cc')

DIRTY = 'graphics/nvn/gfx/nvn/nvn_dirty.c'
ENTRY = 'graphics/nvn/replay/rp_entry.c'

# ---------- find_span 有序索引（nvn_dirty.c）：(名字, [(旧, 新) …], 期望单测里出现的断言文字) ----------
HIT_GUARD = 'if (!(epoch & 1) && !atomic_load_explicit(&g_span_off, memory_order_relaxed)) {   // 奇数 = 有写者正在改表（含索引）：不信索引'
VALIDATE = 'if (atomic_load_explicit(&g_table_epoch, memory_order_relaxed) == epoch) {   // 整个查询期间表没变：答案（含「没有」）与线性扫在这张表上的逐位相同'
LEN_GUARD = 'if (len && a + len >= a) {'
SM_ROW = 'atomic_store_explicit(&g_span_n, n - 1, memory_order_release);\n    span_sm_build(0, n - 1);'
INDEX_BAD = '索引与槽位表对不上'
WRONG_ANSWER = '答案与线性扫不一致'
dirty_mutations = [
    # 读者：复查与退路
    ('查询时不复查代际', [(VALIDATE, 'if (1) {   // 变异：不复查')], '不能返回已注销的范围'),
    ('奇数代际也信索引', [(HIT_GUARD, 'if (!atomic_load_explicit(&g_span_off, memory_order_relaxed)) {')], '奇数代际：不用索引'),
    ('索引停用被无视', [(HIT_GUARD, 'if (!(epoch & 1)) {')], '停用之后一律线性扫，不再用索引'),
    ('零长度也走索引', [(LEN_GUARD, 'if (a + len >= a) {')], '零长度与回绕走线性扫'),
    ('地址回绕也走索引', [(LEN_GUARD, 'if (len) {')], '零长度与回绕走线性扫'),
    # 读者：树的查询
    ('剪枝差一（sm == 末尾也剪）', [('if (atomic_load_explicit(&e->sm, memory_order_relaxed) < endq) break;', 'if (atomic_load_explicit(&e->sm, memory_order_relaxed) <= endq) break;')],
     '刚好装满：起点 = 范围起点、末尾 = 范围末尾'),
    ('起点比较差一（起点 == a 也排除）', [('if (span_base(e) > a) { hi = mid; continue; }', 'if (span_base(e) >= a) { hi = mid; continue; }')],
     '刚好装满：起点 = 范围起点、末尾 = 范围末尾'),
    ('起点在 a 之后还当候选', [('if (span_base(e) > a) { hi = mid; continue; }', 'if (span_base(e) > a) { hi = mid; }')],
     '起点在范围前一字节：不算在内'),
    ('装得下判断差一（end == 末尾也算装不下）', [('if (span_end(e) >= endq) {', 'if (span_end(e) > endq) {')],
     '刚好装满：起点 = 范围起点、末尾 = 范围末尾'),
    ('取先找到的而不是槽号最小的', [('const uint32_t s = span_slot(e); if (s < best) best = s; }', 'const uint32_t s = span_slot(e); if (best == SPAN_NONE) best = s; }')],
     '槽号小的优先'),
    ('右子树不看', [('if (mid + 1 < hi) { slo[sp] = mid + 1; shi[sp++] = hi; }', '')], WRONG_ANSWER),
    ('左子树不看', [('hi = mid;   // 左子树的起点都 ≤ 根的起点 ≤ a，继续往左', 'lo = hi;   // 变异：不看左子树')], WRONG_ANSWER),
    # 写者：索引的增删
    ('登记不入索引', [('    span_insert(base, end, (uint32_t)slot);\n', '')], (INDEX_BAD, WRONG_ANSWER)),
    ('注销不出索引', [('span_remove(r->base, (uint32_t)slot); }\n', '}\n')], INDEX_BAD),
    ('排序不看槽号（同起点的顺序乱）', [('if (eb < base || (eb == base && span_slot(e) < slot)) lo = mid + 1; else hi = mid;', 'if (eb < base) lo = mid + 1; else hi = mid;')], INDEX_BAD),
    ('插入位置总在末尾', [('const int p = span_lower(base, slot, n);\n    for (int i = n; i > p; i--)', 'const int p = n;\n    for (int i = n; i > p; i--)')], INDEX_BAD),
    ('挪条目时漏拷槽号', [('    atomic_store_explicit(&to->slot, span_slot(from), memory_order_relaxed);\n', '')], INDEX_BAD),
    ('sm 不含子树的最大 end', [('    if (l > m) m = l;\n    if (r > m) m = r;\n', '')], INDEX_BAD),
    ('登记后不重算 sm', [('atomic_store_explicit(&g_span_n, n + 1, memory_order_release);\n    span_sm_build(0, n + 1);', 'atomic_store_explicit(&g_span_n, n + 1, memory_order_release);')], INDEX_BAD),
    ('注销后不重算 sm', [(SM_ROW, 'atomic_store_explicit(&g_span_n, n - 1, memory_order_release);')], INDEX_BAD),
    ('注销时按错位置找条目（索引悄悄停用）', [('p = span_lower(base, slot, n);\n    if (p >= n ||', 'p = 0;\n    if (p >= n ||')], '索引意外停用'),
    # 上限、分配失败、重置会话
    # 去掉上限判断后第 SPAN_IDX_MAX + 1 条写到块数组外面（未定义行为）：Linux 上通常还能走到断言，macOS 上当场被 SIGSEGV 杀掉——两种都是被拦住
    ('上限不判', [('    if (n >= SPAN_IDX_MAX) { span_off("现存范围超过索引上限"); return; }\n', '')], ('超过上限：索引停用', CRASH)),
    ('分配失败只返回、不停用索引', [('if (!nb) { span_off("内存分配失败"); return; }', 'if (!nb) return;')], '分配失败：索引停用'),
    ('重置会话不清索引', [('    span_reset();\n    for (int b = 0; b < NBLK; b++) {', '    for (int b = 0; b < NBLK; b++) {')], '重置会话之后索引是空的'),
    ('重置会话不恢复停用', [('atomic_store(&g_span_n, 0); atomic_store(&g_span_off, 0);', 'atomic_store(&g_span_n, 0);')], '重置会话：索引恢复、清空'),
    # 写者纪律：开工 / 收工的代际括号
    ('登记收工不加代际', [('span_insert(base, end, (uint32_t)slot);\n    table_write_end();', 'span_insert(base, end, (uint32_t)slot);')], '写者「登记」收工后代际'),
    ('注销收工不加代际', [('span_remove(r->base, (uint32_t)slot); }\n    table_write_end();', 'span_remove(r->base, (uint32_t)slot); }')], '写者「注销」收工后代际'),
    ('重置会话收工不加代际', [('atomic_store(&g_hooks_on, 0);\n    table_write_end();', 'atomic_store(&g_hooks_on, 0);')], '写者「重置会话」收工后代际'),
    ('原地换底收工不加代际',
     [('    table_write_end();\n    pthread_mutex_unlock(&g_slot_mu);\n    return 1;\n}\nuint64_t nvn_dirty_mapping_generation',
       '    pthread_mutex_unlock(&g_slot_mu);\n    return 1;\n}\nuint64_t nvn_dirty_mapping_generation')], '写者「原地换底」收工后代际'),
    ('登记完全不动代际', [('pthread_mutex_lock(&g_slot_mu);\n    table_write_begin();\n    int n = atomic_load(&g_nr);\n    int slot = -1;', 'pthread_mutex_lock(&g_slot_mu);\n    int n = atomic_load(&g_nr);\n    int slot = -1;'),
                          ('span_insert(base, end, (uint32_t)slot);\n    table_write_end();', 'span_insert(base, end, (uint32_t)slot);')],
     '写者「登记」收工后代际'),
    ('注销完全不动代际', [('pthread_mutex_lock(&g_slot_mu);\n    table_write_begin();\n    range_t *r = NULL; int n = atomic_load(&g_nr), slot = -1;', 'pthread_mutex_lock(&g_slot_mu);\n    range_t *r = NULL; int n = atomic_load(&g_nr), slot = -1;'),
                          ('span_remove(r->base, (uint32_t)slot); }\n    table_write_end();', 'span_remove(r->base, (uint32_t)slot); }')],
     '写者「注销」收工后代际'),
]

# ---------- 池查找备忘（rp_entry.c） ----------
pool_mutations = [
    ('池备忘不比代际', [('if (m->epoch != ep || addr < m->lo || addr >= m->lim) continue;', 'if (!m->epoch || ep == 0 || addr < m->lo || addr >= m->lim) continue;')],   # 代际不再比对（ep 从 1 起，ep == 0 恒假，只为不留未用变量告警）
     '0x200 + 0x10, 8) && rp_pool_find'),
    ('池备忘装不下也算命中',
     [('if (addr - m->a > m->n || len > m->n - (addr - m->a)) continue;', '/* 变异：不判装不装得下 */')],
     ('pm_check(B + 0xf80, 0x100)',    # 越过 P0 尾
      'pm_check(B + 0x1800, 0x10)')),  # P0 与 P1 之间的空隙：候选是 P0、装不下；两条都是「候选是它但装不下」
    ('池备忘给错长度', [('if (size) *size = m->n;', 'if (size) *size = m->n + 1;')], 'pm_check(B + 0x800, 0x100)'),
    ('池备忘关着也查', [('if (!on) return 0;', '/* 变异：关着也查 */')], 'pm_hits() == hz && pm_lookups() == lz'),
    ('快路不看可答区间', [('if (addr < lo || addr >= lim) return 0;   // outside the answer interval: the locked search decides', 'if (0 && (addr < lo || addr >= lim)) return 0;   // 变异：不看区间')],
     'rp_pool_find(B + SMALL_OFF + 0x20, 0x40'),
    ('池查找下界不并入跨过的小池',
     [('for (int k = ci + 1; k <= cand; k++) {',
       'for (int k = ci + 1; k <= cand && 0; k++) {   // 变异：lo 回退旧公式 [a, 下一起点)')],
     'm == B + SMALL_OFF && n == SMALL'),
]

compile_flags = ['-O2', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter']


def mutate(source, edits, name):
    for old, new in edits:
        if source.count(old) != 1:
            raise SystemExit('%s：变异锚点不唯一或不存在（%d 处）：%r' % (name, source.count(old), old[:70]))
        source = source.replace(old, new)
    return source


def run_dirty(name, code):
    src = OUT / ('dirty_%s.c' % name)
    src.write_text(code, encoding='utf-8')
    exe = OUT / ('dirty_%s' % name)
    flags = compile_flags if name == 'positive' else [*compile_flags, '-w']   # 变异副本必然带「没用到」类告警（删了调用点），只有正对照的告警有意义
    subprocess.run([CC, *flags, '-I' + str(ROOT / 'runtime/include'), '-I' + str(ROOT / 'graphics/nvn/gfx/nvn'), '-pthread',
                    '-DNVN_DIRTY_SPAN_SOURCE="%s"' % src, '-o', str(exe), str(ROOT / 'graphics/nvn/tests/t_dirty_span_index.c')],
                   check=True, cwd=ROOT)
    return subprocess.run([str(exe)], capture_output=True, text=True, cwd=ROOT, timeout=300)


pool_other = ['graphics/nvn/tests/t_nvn_replay.c', 'graphics/nvn/tests/nvn_clear_view_unused.c',
              *('graphics/nvn/replay/' + x for x in ('rp_exec.c', 'rp_copy_t2b.c', 'rp_clear_tex.c', 'rp_state.c', 'rp_res.c', 'rp_buf.c', 'rp_pass.c',
                                                      'rp_sdx.c', 'rp_present.c', 'rp_draw.c', 'nvn_unimpl.c')),
              'graphics/nvn/gfx/nvn/nvn_tex.c', 'graphics/nvn/gfx/tex/nvn_deswizzle.c']
pool_flags = [*compile_flags, '-std=c11', '-D_DEFAULT_SOURCE', '-Werror',
              *('-I' + str(ROOT / x) for x in ('.', 'graphics/nvn/gfx/nvn', 'graphics/nvn/gfx/nvn/include', 'graphics/nvn/gfx/tex', 'graphics/nvn/replay'))]
pool_objs = []
# 回放核心对平台 / 后端可选件的弱引用：苹果 ld 默认不认未定义弱符号，逐个允许缺；清单只从源码登记生成（tools/weak_link_decl.py flags replay）
pool_link = subprocess.run([sys.executable, str(ROOT / 'tools/weak_link_decl.py'), 'flags', 'replay'], check=True, capture_output=True, text=True).stdout.split() if sys.platform == 'darwin' else []


def pool_build_others():
    for x in pool_other:   # 不变的部分只编一次，每个变异体只重编 rp_entry.c 再链接
        o = OUT / ('pool_obj_%s.o' % Path(x).stem)
        subprocess.run([CC, *pool_flags, '-c', str(ROOT / x), '-o', str(o)], check=True, cwd=ROOT)
        pool_objs.append(str(o))


def run_pool(name, code):
    src = OUT / ('pool_%s.c' % name)
    src.write_text(code, encoding='utf-8')
    obj = OUT / ('pool_%s.o' % name)
    subprocess.run([CC, *pool_flags, '-c', str(src), '-o', str(obj)], check=True, cwd=ROOT)
    exe = OUT / ('pool_%s' % name)
    subprocess.run([CC, *pool_objs, str(obj), *pool_link, '-lm', '-o', str(exe)], check=True, cwd=ROOT)
    return subprocess.run([str(exe), str(ROOT / 'docs/直连_Vulkan回放接线表.tsv')], capture_output=True, text=True, cwd=ROOT, timeout=180)


def expect_red(name, r, want):
    out = r.stdout + r.stderr
    wants = (want,) if isinstance(want, str) else tuple(want)
    if r.returncode == 0:
        raise SystemExit(name + '：变异体居然通过了（单测没拦住）')
    seen = next((w for w in wants if w is not CRASH and w in out), None)
    if seen is None and CRASH in wants and r.returncode < 0:   # 只对标了 CRASH 的变异体：越界写被信号杀掉也算拦住
        seen = '被信号 %d 杀掉（越界写）' % -r.returncode
    if seen is None:
        ended = '被信号 %d 杀掉' % -r.returncode if r.returncode < 0 else '退出码 %d' % r.returncode
        reds = [l for l in out.splitlines() if '【红】' in l or l.startswith('FAIL')]
        raise SystemExit('%s：红在别处（进程%s，输出里共 %d 条红行），期望断言含 %s\n  最先的几条红行：\n%s\n  输出末尾：\n%s'
                         % (name, ended, len(reds), ' 或 '.join(repr(w) for w in wants),
                            '\n'.join('    ' + l for l in reds[:6]) or '    （没有）', out[-800:]))
    print('  %s：已拦住（红在「%s」）' % (name, seen), flush=True)


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else ''
    done = 0
    if only != 'pool':
        dirty_src = (ROOT / DIRTY).read_text(encoding='utf-8')
        pos = run_dirty('positive', dirty_src)
        if pos.returncode:
            raise SystemExit('find_span 索引：正对照没过\n' + pos.stdout + pos.stderr)
        print('find_span 索引 正对照：', pos.stdout.strip().splitlines()[-1], flush=True)
        for i, (name, edits, want) in enumerate(dirty_mutations):
            expect_red(name, run_dirty('m%d' % i, mutate(dirty_src, edits, name)), want)
            done += 1
    if only != 'dirty':
        entry_src = (ROOT / ENTRY).read_text(encoding='utf-8')
        pool_build_others()
        ppos = run_pool('positive', entry_src)
        if ppos.returncode:
            raise SystemExit('池查找备忘：正对照没过\n' + ppos.stdout + ppos.stderr)
        print('池查找备忘 正对照：', [l for l in ppos.stdout.splitlines() if '项检查' in l][-1:], flush=True)
        for i, (name, edits, want) in enumerate(pool_mutations):
            expect_red(name, run_pool('m%d' % i, mutate(entry_src, edits, name)), want)
            done += 1
    what = '取范围索引' if only == 'dirty' else '池查找备忘' if only == 'pool' else '取范围索引与池查找备忘'
    print('PASS: %s共 %d 个变异体全部被单测拦住' % (what, done))


main()
