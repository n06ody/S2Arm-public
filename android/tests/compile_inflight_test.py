#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""宿主门禁（#1501 D②）：安卓现编时翻译器崩溃，崩溃线程正在编的那份请求要原样落盘。

苹果侧早有 s2a_dsh_crash_dump（graphics/nvn/host/dsh_miss.inc，crash.c 非安卓分支里 dlsym 后调）；
安卓的崩溃处理（crash.c android_crash_fatal）原先不调任何编译钩子，现编在 compile_request.c 里同步
进 NativeAOT 编译器，崩了什么输入都留不下。这里：

  ① 用生产 compile_request.c 编宿主二进制跑 compile_inflight_test.c 五个场景（真缺页、正常、
     编译器接住信号、别的线程在编、没配目录）；
  ② 源码核：crash.c 的 android_crash_fatal 在 fatal 行 / 链回之前调 s2a_compile_inflight_dump，
     而且是链接期弱引用（安卓单库里 dlsym(RTLD_DEFAULT) 取不到导出）；两份直连源表都编 compile_request.c。

负控（源副本只在临时目录里改，不碰仓库），每种都必须红在预期断言：
  A. dump 不看在编槽（直接返回）→ ② 真崩溃没落盘；
  B. 返回后不删已落的文件 → ④ 留下假现场；
  C. dump 不认线程、落第 0 槽 → ⑤ 冒领别的线程的请求；
  D. crash.c 去掉调用 → 源码核红。
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent)); import host_link   # 链接清单按符号闭包推导（见 host_link.py）

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'android/native'
DIRECT = NATIVE / 'direct'
TEST = ROOT / 'android/tests/compile_inflight_test.c'
REQUEST_SOURCE = DIRECT / 'compile_request.c'
CRASH = ROOT / 'runtime/src/crash.c'
LINK_SOURCES = ROOT / 'android/core-link/LinkSources.cmake'
LIBS = [DIRECT / name for name in ('compiler_bind.c', 'sha256.c', 's2a_direct_abi.c')]
CC = os.environ.get('CC', 'cc')
BASE = ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-D__ANDROID__', '-Wall', '-Wextra', '-Werror', '-pthread']


def stub_include(tmp):
    sysdir = tmp / 'include/sys'
    sysdir.mkdir(parents=True)
    (sysdir / 'system_properties.h').write_text(
        '#pragma once\n#define PROP_VALUE_MAX 92\n'
        'int __system_property_get(const char *name, char *out);\n', encoding='utf-8')
    return tmp / 'include'


def build_and_run(tmp, include, name, request_source=REQUEST_SOURCE):
    exe = tmp / name
    cmd = [CC, *BASE, '-I', str(include), '-I', str(NATIVE), '-I', str(DIRECT),
           str(TEST), str(request_source), *(str(p) for p in LIBS), '-o', str(exe)]
    host_link.run(cmd, check=True, timeout=60)
    out = tmp / f'{name}-dir'
    out.mkdir()
    return subprocess.run([str(exe), str(out)], capture_output=True, text=True, timeout=60)


def crash_hook_problems(crash_text):
    """android_crash_fatal 里在 s2a_android_fatal 之前调 dump，且声明是弱引用。"""
    problems = []
    m = re.search(r'static void android_crash_fatal\([^)]*\)\s*\{(.*?)\n\}', crash_text, re.S)
    if not m:
        return ['crash.c 找不到 android_crash_fatal']
    body = m.group(1)
    call = body.find('s2a_compile_inflight_dump()')
    fatal = body.find('s2a_android_fatal(')
    if call < 0:
        problems.append('android_crash_fatal 没调 s2a_compile_inflight_dump')
    elif fatal >= 0 and call > fatal:
        problems.append('s2a_compile_inflight_dump 排在 s2a_android_fatal（链回）之后')
    if not re.search(r'extern void s2a_compile_inflight_dump\(void\) __attribute__\(\(weak\)\);', crash_text):
        problems.append('s2a_compile_inflight_dump 不是链接期弱引用声明')
    if 'dlsym(RTLD_DEFAULT, "s2a_compile_inflight_dump")' in crash_text:
        problems.append('安卓单库里不能按 dlsym(RTLD_DEFAULT) 取')
    return problems


def main():
    # ② 源码核（生产源码）
    crash_text = CRASH.read_text(encoding='utf-8')
    problems = crash_hook_problems(crash_text)
    assert not problems, '；'.join(problems)
    link = LINK_SOURCES.read_text(encoding='utf-8')
    lists = [m.group(1) for m in re.finditer(r'function\((s2a_android_(?:nvk|direct)_sources)\b', link)]
    assert set(lists) >= {'s2a_android_nvk_sources', 's2a_android_direct_sources'}, f'源表函数名变了：{lists}'
    assert link.count('native/direct/compile_request.c') >= 2, 'nvk 与旧直连两份源表都要编 compile_request.c'
    print('crash.c: android_crash_fatal 在链回之前弱引用调 s2a_compile_inflight_dump')

    with tempfile.TemporaryDirectory(prefix='compile-inflight-') as td:
        tmp = Path(td)
        include = stub_include(tmp)
        # ① 生产源码
        result = build_and_run(tmp, include, 'prod')
        assert result.returncode == 0, f'生产源码应为绿：\n{result.stdout}{result.stderr}'
        print(result.stdout.strip().splitlines()[-1])

        # 负控
        source = REQUEST_SOURCE.read_text(encoding='utf-8')

        def mutate(name, old, new, expected):
            assert source.count(old) == 1, f'{name}: 锚点不唯一'
            path = tmp / f'{name}.c'
            path.write_text(source.replace(old, new), encoding='utf-8')
            r = build_and_run(tmp, include, name, path)
            assert r.returncode != 0 and expected in r.stdout, \
                f'负控 {name} 应红在「{expected}」：\n{r.stdout}{r.stderr}'
            print(f'负控 {name}: 红在「{expected}」')

        mutate('dump-ignores-slot', '    int k = t_inflight;\n    if (k < 0 || k >= INFLIGHT_SLOTS) return;',
               '    int k = t_inflight;\n    if (k < 0 || k >= INFLIGHT_SLOTS || 1) return;',
               'FAIL: ② 崩溃后落下 compile-inflight-<pid>-0.log')
        mutate('keep-after-survive', '    if (sl->dumped) unlink(sl->path);',
               '    (void)0;', 'FAIL: ④ 编译返回后删掉，不留假现场')
        mutate('any-thread', '    int k = t_inflight;\n    if (k < 0',
               '    int k = 0; (void)t_inflight;\n    if (k < 0', 'FAIL: ⑤ 别的线程正在编的那份不由本线程落盘')

        no_call = crash_text.replace('    if (s2a_compile_inflight_dump) s2a_compile_inflight_dump();', '', 1)
        assert no_call != crash_text, 'crash.c 负控锚点没找到'
        assert any('没调' in p for p in crash_hook_problems(no_call)), 'crash.c 去掉调用后源码核必须红'
        print('负控 crash-no-call: 源码核红')
    print('compile_inflight_test: 全过')


if __name__ == '__main__':
    main()
