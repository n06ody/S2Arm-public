#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Real graphics REQUEST producer -> unchanged importer; synthetic host inputs only.

--baseline-ref compiles the frozen draw as well, recording its functional red and
comparing exact request/result bytes. Normal tests need no historical Git object.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parent)); import host_link   # 链接清单按符号闭包推导、弱引用自动放行（见 host_link.py）

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'android/native'
DRAW = 'android/native/nvn/nvn_vk_draw.c'
sys.path.insert(0, str(ROOT / 'android/core-link'))
sys.path.insert(0, str(ROOT / 'android/tools'))
import link_symbols
import direct_batch


def run(command, runner=subprocess.run, **kwargs):
    result = runner(command, capture_output=True, text=True, timeout=60, **kwargs)
    assert result.returncode == 0, f'{command}\n{result.stdout}\n{result.stderr}'
    return result


def name_block(source):
    start = source.index('// F4.1：编译请求诊断')
    if '    if (export_open) {' in source[start:]:
        begin = source.index('    if (export_open) {', start)
        end = source.index('    if (export_open && s2a_compile_export_request', begin)
    else:
        begin = source.index('    {\n', start)
        end = source.index('    struct s2a_compile_outcome outcome;', begin)
    return source[begin:end]


def prepare(td):
    ndk = link_symbols.find_ndk()
    if ndk is None:
        raise RuntimeError('graphics REQUEST test needs pinned NDK29 headers; no download or skip')
    host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
    vulkan = ndk / 'toolchains/llvm/prebuilt' / host / 'sysroot/usr/include/vulkan'
    assert (vulkan / 'vulkan.h').is_file(), 'NDK Vulkan headers missing'
    inc = td / 'include'
    (inc / 'android').mkdir(parents=True)
    (inc / 'sys').mkdir()
    (inc / 'vulkan').symlink_to(vulkan)
    if (vulkan.parent / 'vk_video').exists():
        (inc / 'vk_video').symlink_to(vulkan.parent / 'vk_video')
    (inc / 'android/native_window.h').write_text('typedef struct ANativeWindow ANativeWindow;\n')
    (inc / 'android/log.h').write_text(
        '#include <stdarg.h>\n#define ANDROID_LOG_INFO 4\n#define ANDROID_LOG_ERROR 6\n'
        'int __android_log_print(int,const char*,const char*,...);\n'
        'int __android_log_vprint(int,const char*,const char*,va_list);\n')
    (inc / 'sys/system_properties.h').write_text(
        '#define PROP_VALUE_MAX 92\nint __system_property_get(const char*,char*);\n')
    cc = os.environ.get('CC', 'cc')
    flags = [cc, '-DS2A_COMPILE_EXPORT_HOST_TEST=1', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O0', '-g',
             '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
             '-ffunction-sections', '-fdata-sections', '-pthread']
    if 'clang' not in run([cc, '--version']).stdout.lower():
        # Existing unrelated logger warnings; preserve production code unchanged.
        flags += ['-Wno-format-truncation', '-Wno-dangling-pointer']
    flags += ['-I' + str(p) for p in (inc, NATIVE, NATIVE / 'nvn', NATIVE / 'direct',
                                     ROOT / 'graphics/nvn/gfx/nvn', ROOT / 'graphics/nvn/gfx/nvn/include')]
    return flags


def compile_draw(td, flags, label, source):
    path, exe = td / (label + '-draw.c'), td / label
    path.write_text(source)
    sources = [Path(__file__).with_suffix('.c')]
    sources += [NATIVE / 'nvn' / (p + '.c') for p in
                ('nvn_vk_compile_facts', 'nvn_vk_format', 'nvn_vk_threadbuf', 'nvn_vk_program_result',
                 # compile facts 的压缩附件按 bcrt 视图格式取颜色类型（nvn_vk_bcrt_view_format 在
                 # nvn_vk_mrt.c）；清单漏了它 = undefined reference，与本用例断言的图形请求身份无关。
                 'nvn_vk_mrt')]
    sources += [NATIVE / 'direct' / (p + '.c') for p in
                ('source_store', 'input_identity', 'compile_request', 'texture_facts', 'control', 'sha256', 'jv', 'json', 'diag', 'compile_export', 'compile_export_w2', 'w2_single_query')]
    strip = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
    run(flags + ['-DGRAPHICS_DRAW_SOURCE="' + str(path) + '"'] + list(map(str, sources)) +
        [strip] + ['-lm', '-o', str(exe)], runner=host_link.run)
    print(f'compiled production graphics path: {label}', flush=True)
    return exe


def exercise(td, exe, mode=1, failure=0, variant=0):
    out = td / f'{exe.name}-{mode}-{failure}-{variant}'
    out.mkdir()
    # Exercise the unchanged dump_fail sibling too, using an actual property-sized path.
    with tempfile.TemporaryDirectory(prefix='gfx-dump-') as dump:
        env = dict(os.environ, GRAPHICS_DUMP_FAIL_DIR=dump)
        identity = run([str(exe), str(out), str(mode), str(failure), str(variant)], env=env).stdout.strip()
        request = (out / 'compiler-request.json').read_bytes()
        request_id = json.loads(request)['requestId']
        sibling = Path(dump) / f'{request_id}-{identity}.compile-request.json'
        assert sibling.read_bytes() == request, 'dump_fail identity/body disagrees'
    assert identity == hashlib.sha256((out / 'canonical.bin').read_bytes()).hexdigest()
    assert hashlib.sha256(request).hexdigest() != identity, 'fixture must distinguish REQUEST SHA'
    return out, identity, request


def verify_import(out, identity, request):
    capture = out / 'capture'
    doc = json.loads(request)
    expected = f'{doc["requestId"]}-{identity}.compile-request.json'
    names = sorted(p.name for p in capture.iterdir() if p.name.endswith('.compile-request.json'))
    assert names == [expected], f'full canonical filename mismatch: {names} != {[expected]}'
    assert (capture / expected).read_bytes() == request, 'diagnostic changed REQUEST bytes'
    manifest = direct_batch.import_requests(capture, out / 'imported')
    assert len(manifest['entries']) == 1
    entry = manifest['entries'][0]
    assert entry['identity'] == identity
    assert (out / 'imported' / entry['request']).read_bytes() == request
    sets = list(capture.glob('requests-*'))
    assert len(sets) == 1, sets
    unique = direct_batch.import_requests(sets[0], out / 'unique-imported')
    assert len(unique['entries']) == 1 and unique['entries'][0]['identity'] == identity
    assert (out / 'unique-imported' / unique['entries'][0]['request']).read_bytes() == request


def compile_seam(td, flags, label, source):
    # Extract only the exact production block, not a rewritten naming algorithm.
    c = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "direct/diag.h"
static void draw_log(const char *fmt, ...) { (void)fmt; }
static int checked_snprintf(char *out, size_t cap, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int n = vsnprintf(out, cap, fmt, ap); va_end(ap);
    if (getenv("GRAPHICS_NAME_FORMAT_ERROR") && strstr(fmt, ".compile-request.json")) return -1;
    return n;
}
#undef snprintf  /* macOS <stdio.h> 把 snprintf 定义成 __builtin___snprintf_chk 宏，-Werror 下重定义报错；Linux 上无此宏，undef 为空操作 */
#define snprintf checked_snprintf
int main(int argc, char **argv) {
    assert(argc == 6); assert(!setenv("S2A_DIRECT_DIAG_DIR", argv[1], 1));
    const int export_open = 1;
    const char *ident = argv[2];
    struct { uint8_t input_sha[32], request_sha[32]; } value, *entry = &value;
    for (unsigned i = 0, v; i < 32; i++) {
        assert(sscanf(argv[3] + i * 2, "%2x", &v) == 1); value.input_sha[i] = (uint8_t)v;
        assert(sscanf(argv[4] + i * 2, "%2x", &v) == 1); value.request_sha[i] = (uint8_t)v;
    }
    FILE *f = fopen(argv[5], "rb"); assert(f);
    char json[32768]; size_t json_len = fread(json, 1, sizeof json, f); assert(!ferror(f)); fclose(f);
    assert(json_len < sizeof json);
SEAM
    assert(fwrite(json, 1, json_len, stdout) == json_len);
    return 0;
}
'''.replace('SEAM', name_block(source))
    path, exe = td / (label + '-seam.c'), td / (label + '-seam')
    path.write_text(c)
    run(flags + [str(path), str(NATIVE / 'direct/diag.c'), str(NATIVE / 'direct/sha256.c'), '-o', str(exe)], runner=host_link.run)
    return exe


def seam_cases(td, exe, request):
    doc = json.loads(request)
    prefix = doc['requestId']
    # Same first 32 hex characters, different final 32; also the final byte alone.
    identities = ['12' * 16 + '34' * 16, '12' * 16 + '56' * 16, '12' * 16 + '34' * 15 + '35']
    request_sha = hashlib.sha256(request).hexdigest()
    capture = td / (exe.name + '-tails')
    raw = td / (exe.name + '-request.json'); raw.write_bytes(request)
    def invoke(root, ident, identity, error=False):
        env = dict(os.environ)
        if error: env['GRAPHICS_NAME_FORMAT_ERROR'] = '1'
        result = run([str(exe), str(root), ident, identity, request_sha, str(raw)], env=env)
        assert result.stdout.encode() == request, 'naming block did not preserve bytes/fall through'
    for identity in identities: invoke(capture, prefix, identity)
    assert capture.is_dir(), 'diagnostic filename missing'
    manifest = direct_batch.import_requests(capture, td / (exe.name + '-imported'))
    assert sorted(e['identity'] for e in manifest['entries']) == sorted(identities), 'tail identities collapsed'
    for entry in manifest['entries']:
        assert (td / (exe.name + '-imported') / entry['request']).read_bytes() == request
    # A 160-byte buffer fits exactly 73 + 1 + 64 + 21 + NUL, rejects the next byte.
    exact = td / (exe.name + '-exact')
    invoke(exact, 'a' * 73, identities[0])
    assert [p.name for p in exact.iterdir()] == ['a' * 73 + '-' + identities[0] + '.compile-request.json']
    for label, size, error in [('overflow', 74, False), ('format-error', 32, True)]:
        root = td / (exe.name + '-' + label)
        invoke(root, 'a' * size, identities[0], error)
        assert not root.exists(), f'{label}: rejected name reached diagnostic writer'
    wrong = td / (exe.name + '-wrong-prefix')
    invoke(wrong, 'wrong', identities[0])
    try:
        direct_batch.import_requests(wrong, td / (exe.name + '-wrong-out'))
    except direct_batch.BatchError as error:
        assert 'prefix does not match requestId' in str(error)
    else:
        raise AssertionError('importer accepted wrong requestId prefix')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline-ref')
    parser.add_argument('--keep-output', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='graphics-request-name-') as temporary:
        td = args.keep_output.resolve() if args.keep_output else Path(temporary)
        if args.keep_output: td.mkdir(parents=True, exist_ok=False)
        flags = prepare(td)
        current = (ROOT / DRAW).read_text()
        exe = compile_draw(td, flags, 'current', current)
        baseline = None
        if args.baseline_ref:
            old = run(['git', '-C', str(ROOT), 'show', args.baseline_ref + ':' + DRAW]).stdout
            baseline = compile_draw(td, flags, 'baseline', old)
        compared = 0
        for mode in (0, 1, 2):
            for failure in (0, 1):
                for variant in (0, 1):
                    out, identity, request = exercise(td, exe, mode, failure, variant)
                    if baseline:
                        before, old_identity, old_request = exercise(td, baseline, mode, failure, variant)
                        assert identity == old_identity and request == old_request
                        for name in ('entry.bin', 'canonical.bin', 'program.bin'):
                            assert (out / name).exists() == (before / name).exists()
                            if (out / name).exists(): assert (out / name).read_bytes() == (before / name).read_bytes(), name
                        compared += 1
                        if mode == 1:
                            try: verify_import(before, identity, request)
                            except AssertionError as error:
                                print('baseline functional red:', error)
                            else: raise AssertionError('baseline unexpectedly accepted full name')
                    if mode == 1: verify_import(out, identity, request)
        # Gate-closed real draw must still compile, with neither diagnostic nor dump payload.
        with tempfile.TemporaryDirectory(prefix='gfx-gate-closed-') as dump:
            closed = td / 'closed-gate'; closed.mkdir()
            env = dict(os.environ, GRAPHICS_DUMP_FAIL_DIR=dump, GRAPHICS_EXPORT_GATE='0')
            run([str(exe), str(closed), '1', '0', '0'], env=env)
            assert not (closed / 'capture').exists()
            assert not list(Path(dump).iterdir())
            anchor = 'const int export_open = s2a_compile_export_open();'
            assert current.count(anchor) == 1
            mutant = compile_draw(td, flags, 'bypass-export-gate', current.replace(anchor, 'const int export_open = 1;'))
            negative = td / 'negative-gate'; negative.mkdir()
            run([str(mutant), str(negative), '1', '0', '0'], env=env)
            assert (negative / 'capture').exists(), 'gate bypass mutation survived'
            print('closed export gate: PASS; bypass negative: expected failure')
        with tempfile.TemporaryDirectory(prefix='w2-') as dump:
            w2out = td / 'w2-unwired'; w2out.mkdir()
            env = dict(os.environ, GRAPHICS_DUMP_FAIL_DIR=dump, GRAPHICS_EXPECT_UNWIRED='1')
            run([str(exe), str(w2out), '1', '1', '0'], env=env)
            anchor = '"【红】W2 单坐标诊断未接线：缺少冻结来源与原始坐标"'
            assert current.count(anchor) == 1
            mutant = compile_draw(td, flags, 'omit-unwired-report', current.replace(anchor, '"W2 diagnostic unavailable"'))
            negative = td / 'w2-negative'; negative.mkdir()
            result = subprocess.run([str(mutant), str(negative), '1', '1', '0'], env=env, capture_output=True, text=True, timeout=60)
            assert result.returncode != 0 and 'unwired_reports == 1' in result.stderr
            print('W2 unwired report: PASS; omitted-report negative: expected assertion failure')
            registered = td / 'w2-provider'; registered.mkdir()
            env = dict(os.environ, GRAPHICS_DUMP_FAIL_DIR=dump, GRAPHICS_EXPECT_PROVIDER='1')
            run([str(exe), str(registered), '1', '1', '0'], env=env)
            print('W2 draw provider receives exact session, generation, and identity: PASS')
        seam = compile_seam(td, flags, 'positive', current)
        seam_cases(td, seam, request)
        block = name_block(current)
        mutations = {
            'request-sha-as-identity': block.replace('entry->input_sha[b]', 'entry->request_sha[b]'),
            'short-name-buffer': block.replace('char dname[160]', 'char dname[96]'),
            'omit-truncation-check': block.replace('nname >= 0 && (size_t)nname < sizeof dname', 'nname >= 0'),
            'omit-format-error-check': block.replace('nname >= 0 && (size_t)nname < sizeof dname', 'nname < (int)sizeof dname'),
        }
        failures = {
            'request-sha-as-identity': 'tail identities collapsed',
            'short-name-buffer': 'diagnostic filename missing',
            'omit-truncation-check': 'overflow: rejected name reached diagnostic writer',
            'omit-format-error-check': 'format-error: rejected name reached diagnostic writer',
        }
        for label, mutated in mutations.items():
            assert mutated != block, f'{label}: mutation anchor missing'
            negative = compile_seam(td, flags, label, current.replace(block, mutated))
            try: seam_cases(td, negative, request)
            except AssertionError as error:
                assert failures[label] in str(error), f'{label}: unrelated failure: {error}'
                print(f'{label}: compiled, functional red: {error}')
            else: raise AssertionError(f'{label}: negative control survived')
        print(f'PASS: 12 full compile paths; 4 producer/importer roundtrips; 3 tail identities; '
              f'exact/overflow/format-error boundaries; wrong prefix rejected; 4 compiled negative controls; '
              f'{compared} frozen REQUEST/result byte comparisons')


if __name__ == '__main__':
    main()
