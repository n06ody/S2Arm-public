#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""结果合并（#1355）宿主单测的编译 / 运行 / 负对照驱动。

无参数：
  编译 android/tests/nvk_shader_alias_test.c（链生产 nvk_shader.c 与真 program_cache.c，ASan），
  开关打开、关闭各跑一遍，必须都通过。
--mutants：
  对生产源码逐个撤掉一处防线，对应用例必须红（编译失败不算红，必须是断言失败或 ASan 报错）：
  · nvk_shader.c：比对键的五项（结果摘要 / 负载长度 / 阶段数 / c1 有无位 / c1 内容）/ 静默约束 / 收尾去重（两种撤法）/ 收尾清结果表 / 结果登记 / 失败项三道闸 / 开关缺省；
  · program_cache.c 的只读头探针：内部身份比对 / 文件长度比对 / 摘要取位 / 不计 miss / 输出清零
    （用 android/tests/direct_program_cache_test.c 判）。
"""
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'android/tests'))
sys.path.insert(0, str(ROOT / 'android/core-link'))
import host_link  # noqa: E402
import link_symbols  # noqa: E402

NATIVE = ROOT / 'android/native'
GFX = ROOT / 'graphics/nvn'
CC = os.environ.get('CC', 'cc')
SHADER = NATIVE / 'gfx_plat/nvk_shader.c'
CACHE = NATIVE / 'direct/program_cache.c'


def vulkan_sysinc():
    ndk = link_symbols.find_ndk()
    if ndk is None:
        raise RuntimeError('requires pinned NDK 29.0.14206865 headers (no download or skip): set ANDROID_NDK_HOME to it, '
                           'or install it at $ANDROID_HOME/ndk/29.0.14206865 or ~/Android/Sdk/ndk/29.0.14206865')
    host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
    sysinc = ndk / 'toolchains/llvm/prebuilt' / host / 'sysroot/usr/include'
    if not (sysinc / 'vulkan/vulkan.h').is_file():
        raise RuntimeError('NDK Vulkan headers missing')
    return sysinc


def alias_base(sysinc):
    inc = [ROOT / 'runtime/include', NATIVE, NATIVE / 'nvn', NATIVE / 'direct', NATIVE / 'gfx_plat',
           GFX, GFX / 'replay', GFX / 'gfx/nvn', GFX / 'gfx/nvn/include', ROOT / 'android/tests/vkhost']
    base = [CC, '-std=c11', '-O1', '-g', '-pthread', '-D_GNU_SOURCE', '-fsanitize=address',
            '-Wno-unused-parameter', '-Wno-unused-function', '-ffunction-sections', '-fdata-sections']
    base += ['-I' + str(p) for p in inc]
    base += ['-idirafter', str(sysinc)]   # libc 头优先取宿主，NDK 只补 vulkan/*
    return base


# nvk_shader.c 的纯逻辑依赖（与 android/tools/nvk_shader_lease_test.sh 同一份清单；program_cache.c 由 host_link 自动补链）
ALIAS_LIBS = [ROOT / 'runtime/src/direct_host.c'] + [
    NATIVE / 'direct' / f for f in ('source_store.c', 'control.c', 'input_identity.c', 'texture_facts.c',
                                     'sha256.c', 'json.c', 'compile_request.c', 'compiler_bind.c')]


def build_alias(base, shader_src, exe):
    strip = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'   # 苹果 ld 不认 --gc-sections
    cmd = base + [str(p) for p in [ROOT / 'android/tests/nvk_shader_alias_test.c', shader_src, *ALIAS_LIBS]] + [strip, '-lm', '-o', str(exe)]
    host_link.run(cmd, check=True, timeout=300, cwd=str(ROOT))


def run_alias(exe, mode):
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
    return subprocess.run([str(exe)] + ([mode] if mode else []), capture_output=True, text=True, timeout=60, env=env)


def mutate(text, edits):
    out = text
    for anchor, repl, count in edits:
        found = out.count(anchor)
        assert found == count, f'anchor expected {count}x, found {found}x: {anchor[:70]!r}'
        out = out.replace(anchor, repl)
    assert out != text
    return out


# (名称, 编辑 [(锚点, 替换, 期望出现次数)], 跑哪个模式, 说明)
ALIAS_MUTANTS = [
    ('probe-nofile-as-bad',
     [('if (prc > 0) { atomic_fetch_add_explicit(&g_n_alias_nofile, 1, memory_order_relaxed); return NULL; }',
       'if (prc > 9) { atomic_fetch_add_explicit(&g_n_alias_nofile, 1, memory_order_relaxed); return NULL; }', 1)], '',
     '读头「没有文件」算进「缓存文件坏」：换闭包后的第一局又被说成文件坏'),
    ('probe-bad-as-nofile',
     [('    if (prc < 0) {   // 有文件但头不对', '    if (prc < -9) {   // 有文件但头不对', 1),
      ('    if (prc > 0) { atomic_fetch_add_explicit(&g_n_alias_nofile', '    if (prc != 0) { atomic_fetch_add_explicit(&g_n_alias_nofile', 1)], '',
     '读头「文件坏」算进「没有文件」、也不打原因'),
    ('no-result-digest-in-key',
     [('memcpy(buf, res, 32); n += 32;', 'memset(buf, 0, 32); n += 32;', 1)], '',
     '比对键里不放结果摘要：结果不同的身份被合并（①）'),
    ('no-length-in-key',
     [('for (int i = 0; i < 8; i++) buf[n++] = (uint8_t)(rlen >> (8 * i));', '', 1)], '',
     '比对键里不放负载长度：头里摘要相同而长度不同的身份被合并（③）'),
    ('no-nst-in-key',
     [('buf[n++] = (uint8_t)k->nst;', '', 1)], '',
     '比对键里不放阶段数：只有阶段数不同的身份被合并（⑧）'),
    ('no-c1-presence-in-key',
     [('buf[n++] = (uint8_t)(k->c1_ok & mask);', '', 1)], '',
     '比对键里不放 c1 有无位：同样的 c1 内容只是挪到另一个阶段的身份被合并（⑧）'),
    ('no-c1-content-in-key',
     [('if (k->c1_ok & (1u << st)) { memcpy(buf + n, k->c1[st], 0x100); n += 0x100; }', ';', 1)], '',
     '比对键里不放 c1 内容：只有 c1 内容不同的身份被合并（⑧）'),
    ('no-quiesce-guard',
     [('epoch != g_progs_epoch || (bg && g_warm_quiesced)) { /* 停局', 'epoch != g_progs_epoch) { /* 停局', 1)], '',
     '撤掉静默约束：预热已静默后结果合并仍登记（④）'),
    ('no-owner-dedupe',
     [('if (e->seen) e->seen = 0; else g_progs.vals[i] = NULL;', 'e->seen = 0;', 1)], '',
     '撤掉收尾去重：同一份记录按身份键各释放一次 → 重复释放（①）'),
    ('dedupe-only-when-released',
     [('{   // 结果合并：同一个 entry 可能挂在多个身份键下', 'if (!bs.borrowed) {   // 结果合并：同一个 entry 可能挂在多个身份键下', 1)], '',
     '收尾去重只在释放路径做：借用未还时「保留」按身份键数而不是记录数（⑥）'),
    ('stale-result-table',
     [('free(g_res_tab.keys); free(g_res_tab.vals); nvn_hm_init(&g_res_tab, 32);', '', 1)], '',
     '收尾不清结果表：下一局合并到上一局已释放的记录（②，ASan 判 UAF）'),
    ('no-result-registration',
     [('if (alias_on() && e->res_ok && !e->failed && !nvn_hm_get(&g_res_tab, e->res_key)) (void)nvn_hm_put(&g_res_tab, e->res_key, e);', ';', 1)], '',
     '发布时不登记结果：永远合并不上（①）'),
    ('failed-entries-aliased',
     [('e && !e->failed && have_res && alias_on())', 'e && have_res && alias_on())', 2),   # 不带「if (」：c59eae4a8 起预热那一处前面多了「!compute &&」
      ('e->res_ok && !e->failed &&', 'e->res_ok &&', 1),
      ('else if (!r || r->failed)', 'else if (!r)', 1)], '',
     '失败项也挂比对键、也登记、也被合并：同结果的另一个身份被合并成失败项（⑤；三道闸一起撤，单撤任一道仍被另两道挡住）'),
    ('alias-always-on',
     [('static int alias_on(void) { static int v = -1; if (v < 0) v = nvn_plat_flag("nvk_alias", 1) ? 1 : 0; return v; }',
       'static int alias_on(void) { return 1; }', 1)], 'off',
     '开关不再读属性：退路（debug.s2a.nvk_alias=0）失效，仍在合并（⑦）'),
    ('default-off',
     [('v = nvn_plat_flag("nvk_alias", 1) ? 1 : 0;', 'v = nvn_plat_flag("nvk_alias", 0) ? 1 : 0;', 1)], 'default',
     '生产缺省改成关闭：不设开关时没有合并（缺省必须打开）'),
]

# 只读头探针的变异：只改 s2a_program_cache_probe 函数体（切片内唯一匹配），用 direct_program_cache_test.c 判
PROBE_BEGIN, PROBE_END = 'int s2a_program_cache_probe(', 'int s2a_program_cache_store('
PROBE_MUTANTS = [
    ('probe-no-identity-check',
     [('if (memcmp(head + 16, identity, 32) != 0) { setwhy(why, why_size, "内部身份与查询身份不符"); return -1; }', '', 1)],
     '探针不比内部身份：被串了身份的文件会被当成这个身份的结果'),
    ('probe-no-size-check',
     [('if ((uint64_t)st.st_size != (uint64_t)PCACHE_HEADER + n) {', 'if (0) {', 1)],
     '探针不比文件长度：截断 / 多出尾巴的文件被当成完整'),
    ('probe-wrong-digest-offset',
     [('memcpy(result_sha, head + 48, 32);', 'memcpy(result_sha, head + 16, 32);', 1)],
     '探针从错误偏移取结果摘要'),
    ('probe-counts-miss',
     [('if (errno == ENOENT) { setwhy(why, why_size, "未命中"); return 1; }', 'if (errno == ENOENT) { atomic_fetch_add(&g_misses, 1); setwhy(why, why_size, "未命中"); return 1; }', 1)],
     '探针把未命中记进正式读取的 miss 计数'),
    ('probe-missing-as-bad',
     [('if (errno == ENOENT) { setwhy(why, why_size, "未命中"); return 1; }', 'if (errno == ENOENT) { setwhy(why, why_size, "未命中"); return -1; }', 1)],
     '探针把「没有文件」报成「文件坏」：结果合并的两个计数分不开'),
    ('probe-bad-as-missing',
     [('if (memcmp(head, PCACHE_MAGIC, 8) != 0) { setwhy(why, why_size, "magic 不符（不是本缓存格式）"); return -1; }',
       'if (memcmp(head, PCACHE_MAGIC, 8) != 0) { setwhy(why, why_size, "magic 不符（不是本缓存格式）"); return 1; }', 1)],
     '探针把「文件坏」报成「没有文件」'),
    ('probe-no-zero-outputs',
     [('if (result_sha) memset(result_sha, 0, 32);', '', 1)],
     '探针失败时不清输出：调用方可能读到上一次的残留摘要'),
]


def probe_slice_mutate(text, edits):
    a, b = text.index(PROBE_BEGIN), text.index(PROBE_END)
    head, body, tail = text[:a], text[a:b], text[b:]
    return head + mutate(body, edits) + tail


def build_cache_test(cache_src, exe):
    cmd = [CC, '-std=c11', '-Wall', '-Wextra', '-Werror', '-D_GNU_SOURCE', '-DS2A_DIRECT_TEST_FAULTS',
           '-I' + str(NATIVE), '-I' + str(NATIVE / 'direct'), '-I' + str(ROOT / 'runtime/include'),
           str(ROOT / 'android/tests/direct_program_cache_test.c'), str(cache_src), str(NATIVE / 'direct/sha256.c'),
           '-pthread', '-o', str(exe)]
    subprocess.run(cmd, check=True, capture_output=True, text=True, timeout=120)


def first_red_line(proc):
    text = (proc.stderr or '') + (proc.stdout or '')
    for line in text.splitlines():
        if line.startswith('FAIL') or 'AddressSanitizer' in line:
            return line.strip()[:150]
    return text.strip().splitlines()[0][:150] if text.strip() else '(无输出)'


def is_red(proc):
    text = (proc.stderr or '') + (proc.stdout or '')
    return proc.returncode != 0 and ('FAIL' in text or 'AddressSanitizer' in text)


def main():
    mutants = '--mutants' in sys.argv[1:]
    unknown = [a for a in sys.argv[1:] if a != '--mutants']
    if unknown:
        raise SystemExit('usage: nvk_shader_alias_test.py [--mutants]')
    sysinc = vulkan_sysinc()
    base = alias_base(sysinc)
    out = ROOT / 'build/nvk-shader-alias-test'
    out.mkdir(parents=True, exist_ok=True)
    tempfile.tempdir = str(out)   # host_link 的临时对象也留在本工作树里

    if not mutants:
        exe = out / 'positive'
        build_alias(base, SHADER, exe)
        for mode, label in (('', '开关显式打开'), ('default', '开关取生产缺省（须为打开）'), ('off', '开关显式关闭（退路）')):
            proc = run_alias(exe, mode)
            sys.stdout.write(proc.stdout)
            if proc.returncode != 0:
                sys.stderr.write(proc.stderr)
                raise SystemExit(f'结果合并宿主单测（{label}）红')
        print('PASS: 结果合并宿主单测（显式打开 / 生产缺省 / 显式关闭）')
        return 0

    text = SHADER.read_text(encoding='utf-8')
    ctext = CACHE.read_text(encoding='utf-8')

    def alias_mutant(item):
        name, edits, mode, why = item
        src = out / f'mut-{name}.c'
        src.write_text(mutate(text, edits), encoding='utf-8')
        exe = out / f'mut-{name}'
        try:
            build_alias(base, src, exe)
        except subprocess.CalledProcessError as e:
            return name, why, 'compile', str(getattr(e, 'stderr', '') or e)
        proc = run_alias(exe, mode)
        if not is_red(proc):
            return name, why, 'green', f'rc={proc.returncode}\n{proc.stdout}{proc.stderr}'
        return name, why, 'red', first_red_line(proc)

    def probe_mutant(item):
        name, edits, why = item
        src = out / f'mut-{name}.c'
        src.write_text(probe_slice_mutate(ctext, edits), encoding='utf-8')
        exe = out / f'mut-{name}'
        try:
            build_cache_test(src, exe)
        except subprocess.CalledProcessError as e:
            return name, why, 'compile', str(e.stderr)
        proc = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        if proc.returncode == 0 or 'FAIL' not in proc.stdout:
            return name, why, 'green', f'rc={proc.returncode}\n{proc.stdout}{proc.stderr}'
        return name, why, 'red', first_red_line(proc)

    # 各变异体互相独立（各写各的源文件 / 可执行文件），并行跑；输出仍按清单顺序。
    # host_link 的「提供方索引」是惰性建的、没有加锁：并行前先在主线程建好，否则别的线程会读到只建了一半的索引而补链失败。
    host_link.candidates('s2a_program_cache_load')
    jobs = [(alias_mutant, it) for it in ALIAS_MUTANTS] + [(probe_mutant, it) for it in PROBE_MUTANTS]
    with ThreadPoolExecutor(max_workers=3) as ex:
        futures = [ex.submit(fn, it) for fn, it in jobs]
        for fut in futures:
            name, why, verdict, detail = fut.result()
            if verdict == 'compile':
                raise SystemExit(f'{name}: 变异体编译失败（编译失败不算红）\n{detail}')
            if verdict == 'green':
                raise SystemExit(f'{name}: 变异体没有变红——{why}\n{detail}')
            print(f'负对照 {name} 变红：{detail}')
    print(f'PASS: 结果合并负对照 {len(ALIAS_MUTANTS) + len(PROBE_MUTANTS)} 项全部变红')
    return 0


if __name__ == '__main__':
    sys.exit(main())
