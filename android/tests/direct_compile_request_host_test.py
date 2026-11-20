#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""宿主门禁：kindCheck 调试开关只走请求 JSON，不走进程环境。

为什么必须钉住：编译器的唯一消费端 graphics/tools/direct/compiler/DirectShaderCompiler.cs:39 只读请求
JSON 的 kindCheck（JsonStrict.OptionalBool，缺省 true），不读进程环境变量。PR #1603 曾把开关改成
execute 里 setenv("S2A_DIRECT_KIND_CHECK")，C# 不读 → 这条调试开关实际失效。所以这里用生产源码编
宿主二进制，从 JSON 端到端验证（不是看 C 测试自己的内部状态）：

  ① 生产默认构建（正式包路径，无调试门宏）：请求 JSON 不得出现 kindCheck 键 = 校验开着；
  ② 调试构建桩矩阵（-D__ANDROID__ -DS2A_ANDROID_DEBUG_ENV=1）：门②关+属性=0 恒开、门开+属性缺省
     仍开、门开+属性=0 才在 sourceParserVersion 与 profile 之间写 "kindCheck":false；
  ③ 正式构建桩矩阵（-D__ANDROID__，无 S2A_ANDROID_DEBUG_ENV）：编译期门关着，两个属性都设上也打不开。

负控（源副本只在临时目录里改，不碰仓库）——三种回退都必须红：
  A. 把 setenv 块加回 execute（PR #1603 原样）→ execute 污染进程环境；
  B. 删掉请求 JSON 里那段 → 门开+属性=0 时不再带 false；
  C. 把门判定改成恒真（if (1)）→ 默认构建也带键，校验被静默关掉。
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent)); import host_link   # 链接清单按符号闭包推导、弱引用自动放行（见 host_link.py）


ROOT = Path(__file__).resolve().parents[2]
ANDROID = ROOT / 'android'
NATIVE = ANDROID / 'native'
DIRECT = NATIVE / 'direct'
TEST = ANDROID / 'tests/direct_compile_request_test.c'
REQUEST_SOURCE = DIRECT / 'compile_request.c'
# 被测源之外的固定链接单元（变异副本会替换 compile_request.c 本身）。
LIBS = [DIRECT / name for name in ('compiler_bind.c', 'sha256.c', 's2a_direct_abi.c')]
CC = os.environ.get('CC', 'cc')
# 宿主 glibc 在 -std=c11 下默认藏起 POSIX 声明；回退负控会用到 setenv，统一打开 POSIX 面，
# 让回退“编得过但跑红”，而不是只剩一个隐式声明编译错误。
BASE = ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror', '-pthread']


def build(exe, request_source=REQUEST_SOURCE, flags=(), include=None):
    cmd = [CC, *BASE, *flags, '-I', str(NATIVE), '-I', str(DIRECT)]
    if include is not None:
        cmd += ['-I', str(include)]
    cmd += [str(TEST), str(request_source), *(str(p) for p in LIBS), '-o', str(exe)]
    host_link.run(cmd, check=True, timeout=60)


def run(exe, *args):
    # 测试自己会断言 execute 不写 S2A_DIRECT_KIND_CHECK；先清掉外部传进来的值，否则“未变”会被误判绿。
    env = dict(os.environ)
    env.pop('S2A_DIRECT_KIND_CHECK', None)
    return subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True,
                          timeout=30, env=env)


def stub_include(tmp):
    # 宿主没有 bionic 的 <sys/system_properties.h>：给一个只声明 __system_property_get 的桩头，
    # 实现由 direct_compile_request_test.c 在 -DS2A_TEST_KIND_CHECK_STUB 下提供。
    sysdir = tmp / 'include/sys'
    sysdir.mkdir(parents=True)
    (sysdir / 'system_properties.h').write_text(
        '#pragma once\n#define PROP_VALUE_MAX 92\n'
        'int __system_property_get(const char *name, char *out);\n', encoding='utf-8')
    return tmp / 'include'


def dump_json(path, what):
    assert path.is_file(), f'{what}: 请求 JSON 没有落盘（{path}）'
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except json.JSONDecodeError as error:
        raise AssertionError(f'{what}: 请求 JSON 不是合法 JSON（{error}）') from error


def main():
    with tempfile.TemporaryDirectory(prefix='direct-request-host-') as td:
        tmp = Path(td)
        include = stub_include(tmp)

        # ① 生产默认构建：正式包路径不带 kindCheck 键（缺省 = 校验开着）。
        default = tmp / 'default'
        build(default)
        default_json = dump_json_pair(default, tmp, 'default')
        assert 'kindCheck' not in default_json, '默认构建的请求 JSON 不应出现 kindCheck 键'
        assert default_json['schema'] == 's2a-direct-compile-v1'
        assert 'sourceParserVersion' in default_json and 'profile' in default_json
        # debugMode 恒显式写（含缺省 false）：sha256 覆盖整个请求 JSON，缺省不带键的旧格式请求
        # 与「编译器 DebugMode 恒开」时期逐字节相同、会命中旧的膨胀产物缓存——显式写让翻译旗
        # 口径落进身份，旧缓存条目（无此键的请求 sha）在新版本下自然 miss。
        assert default_json.get('debugMode') is False, \
            '请求 JSON 必须恒显式写 debugMode（缺省 false），否则旧 DebugMode 产物缓存继续命中'
        print('default build: no kindCheck key (validation stays on); '
              'debugMode:false explicit (identity carries the flag)')

        # ② 调试构建桩矩阵：只有门开 + 属性=0 才关，且以 JSON 的 false 交给编译器。
        debug = tmp / 'debug'
        build(debug, flags=['-D__ANDROID__', '-DS2A_ANDROID_DEBUG_ENV=1',
                            '-DS2A_TEST_KIND_CHECK_STUB'], include=include)
        off_path = tmp / 'kindcheck-off.json'
        result = run(debug, tmp / 'debug-main.json', tmp / 'debug-pool.json', off_path)
        assert result.returncode == 0, f'调试构建桩矩阵应为绿：\n{result.stdout}{result.stderr}'
        flagged = dump_json(off_path, 'debug gate open + kind_check=0')
        keys = list(flagged)
        assert flagged.get('kindCheck') is False, '门开 + 属性=0 时请求 JSON 必须带 kindCheck:false'
        assert keys.index('kindCheck') == keys.index('sourceParserVersion') + 1, \
            f'kindCheck 必须紧跟 sourceParserVersion（现顺序 {keys}）'
        assert keys[keys.index('kindCheck') + 1] == 'profile', 'kindCheck 必须在 profile 之前'
        assert flagged.get('debugMode') is False, 'xlate_debug 未设时请求 JSON 带 debugMode:false'
        print('debug gate open + kind_check=0: '
              '"kindCheck":false between sourceParserVersion and profile')

        # ③ 正式构建桩矩阵：没有 S2A_ANDROID_DEBUG_ENV，门①编译期关着，adb 设属性也打不开。
        release = tmp / 'release'
        build(release, flags=['-D__ANDROID__', '-DS2A_TEST_KIND_CHECK_STUB'], include=include)
        result = run(release)
        assert result.returncode == 0, f'正式构建桩矩阵应为绿：\n{result.stdout}{result.stderr}'
        print('release build (no S2A_ANDROID_DEBUG_ENV): properties cannot open the debug gate')

        # ④ 负控：三种回退都必须红（源副本在临时目录，仓库不动）。
        source = REQUEST_SOURCE.read_text(encoding='utf-8')

        def mutated(name, flags, transform, expected, include_dir=include):
            path = tmp / f'{name}.c'
            path.write_text(transform(source), encoding='utf-8')
            exe = tmp / name
            build(exe, request_source=path, flags=flags, include=include_dir)
            return run(exe, tmp / f'{name}-main.json', tmp / f'{name}-pool.json',
                       tmp / f'{name}-off.json'), expected

        execute_comment = '    // 种类校验开关不再在这里读环境变量/属性'
        assert source.count(execute_comment) == 1, 'execute 回退负控的锚点不唯一'
        result, expected = mutated(
            'revert-setenv',
            ['-D__ANDROID__', '-DS2A_ANDROID_DEBUG_ENV=1', '-DS2A_TEST_KIND_CHECK_STUB'],
            lambda s: s.replace(execute_comment,
                                '    setenv("S2A_DIRECT_KIND_CHECK", "1", 1);\n' + execute_comment),
            'setenv S2A_DIRECT_KIND_CHECK')
        assert result.returncode != 0 and expected in result.stdout, \
            f'把 setenv 加回 execute 必须红：\n{result.stdout}{result.stderr}'
        print('revert-setenv: expected assertion failure')

        json_block = ('    if (!s2a_direct_kind_check_enabled()) {\n'
                      '        sb_puts(&b, ",");\n'
                      '        sb_json_string(&b, "kindCheck"); sb_puts(&b, ":false");\n'
                      '    }\n')
        assert source.count(json_block) == 1, 'JSON 回退负控的锚点不唯一'
        result, expected = mutated(
            'drop-json-key',
            ['-D__ANDROID__', '-DS2A_ANDROID_DEBUG_ENV=1', '-DS2A_TEST_KIND_CHECK_STUB'],
            lambda s: s.replace(json_block, ''),
            'carries kindCheck:false')
        assert result.returncode != 0 and expected in result.stdout, \
            f'删掉请求 JSON 的 kindCheck 必须红：\n{result.stdout}{result.stderr}'
        print('drop-json-key: expected assertion failure')

        gate = '    if (!s2a_direct_kind_check_enabled()) {'
        assert source.count(gate) == 1, '门判定回退负控的锚点不唯一'
        result, expected = mutated('ignore-gate', (), lambda s: s.replace(gate, '    if (1) {'),
                                   'carries no kindCheck key', include_dir=None)
        assert result.returncode != 0 and expected in result.stdout, \
            f'忽略调试门恒写 false 必须红：\n{result.stdout}{result.stderr}'
        print('ignore-gate: expected assertion failure')

        print('PASS: 3 host builds (default / debug-gate matrix / release build) + '
              '3 negative controls (setenv in execute / drop JSON key / ignore gate)')


def dump_json_pair(exe, tmp, name):
    """默认构建把主请求 JSON 落盘到 argv[1]（C 测试既有落盘口），这里解析它。"""
    path = tmp / f'{name}-main.json'
    result = run(exe, path)
    assert result.returncode == 0, f'{name} 构建的运行应为绿：\n{result.stdout}{result.stderr}'
    return dump_json(path, name)


if __name__ == '__main__':
    main()
