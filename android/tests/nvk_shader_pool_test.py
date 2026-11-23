#!/usr/bin/env python3
"""Real new-layer shader producer and REQUEST builders; synthetic source/compiler boundary."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'android/tests'))
import host_link
sys.path.insert(0, str(ROOT / 'android/core-link'))
import link_symbols

def main():
    out = ROOT / 'build/nvk-shader-pool-test'
    out.mkdir(parents=True, exist_ok=True)
    # host_link's transient objects must also remain in this worktree.
    tempfile.tempdir = str(out)
    native = ROOT / 'android/native'
    gfx = ROOT / 'graphics/nvn'
    ndk = link_symbols.find_ndk()
    if ndk is None:
        raise RuntimeError('requires pinned NDK 29.0.14206865 headers (no download or skip): set ANDROID_NDK_HOME to it, '
                           'or install it at $ANDROID_HOME/ndk/29.0.14206865 or ~/Android/Sdk/ndk/29.0.14206865')
    host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
    sysinc = ndk/'toolchains/llvm/prebuilt'/host/'sysroot/usr/include'
    if not (sysinc/'vulkan/vulkan.h').is_file():
        raise RuntimeError('NDK Vulkan headers missing')
    include = out/'include'
    include.mkdir(exist_ok=True)
    for name in ('vulkan', 'vk_video'):
        if (sysinc/name).exists() and not (include/name).exists():
            (include/name).symlink_to(sysinc/name)
    inc = (include, ROOT/'runtime/include', native, native/'nvn', native/'direct', gfx, gfx/'replay', gfx/'gfx/nvn',
           gfx/'gfx/nvn/include', ROOT/'android/tests/vkhost', native/'gfx_plat')
    libs = [gfx/'replay/nvn_unimpl.c', gfx/'replay/rp_state.c', native/'nvn/nvn_vk_compute_request.c',
            native/'nvn/nvn_vk_prog_warm.c']   # nvk_shader.c 的预热发布/清单记账在 nvn_vk_prog_warm.c
    libs += [ROOT/'runtime/src/direct_host.c']
    libs += [native/'direct'/f for f in ('source_store.c', 'control.c', 'input_identity.c',
                                        'texture_facts.c', 'sha256.c', 'json.c', 'compile_request.c')]
    strip = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
    base = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-ffunction-sections', '-fdata-sections', '-pthread']
    # glibc 在 -std=c11 下不暴露 POSIX.1-2008 声明（O_CLOEXEC / openat / fstatat / strdup 等），
    # host_link 自动补链的 offline_library.c、program_cache.c 要用。宏只加在测试侧、只加给非 macOS：
    # 这两个文件还要给 NDK 和苹果宿主编，在源文件里定义会改变 macOS 上的可见声明；macOS 的参数与原来逐字相同。
    if sys.platform != 'darwin':
        # glibc 在 -std=c11 下不暴露 POSIX 声明（O_CLOEXEC / openat / fstatat / strdup / realpath 等），
        # host_link 自动补链的 offline_library.c、program_cache.c 要用。用 _GNU_SOURCE（POSIX 宏的超集：
        # 新版 glibc 里 realpath 不算在 _POSIX_C_SOURCE 200809 的可见集里）。宏只加在测试侧、只加给非 macOS：
        # 这两个文件还要给 NDK 和苹果宿主编，在源文件里定义会改变 macOS 上的可见声明。
        base.append('-D_GNU_SOURCE')
    base += ['-I'+str(p) for p in inc]
    original = native/'gfx_plat/nvk_shader.c'

    def run(name, source, extra_libs=None, args=(), env=None, extra_flags=()):
        exe = out/name
        src = [ROOT/'android/tests/nvk_shader_pool_test.c', source, *(extra_libs if extra_libs is not None else libs)]
        host_link.run(base + list(extra_flags) + [str(p) for p in src] + [strip, '-lm', '-o', str(exe)], check=True, timeout=60)
        return subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=30, env=env)

    live_modes = ('--live-compile-thread', '--live-compile-pending', '--live-compile-stop', '--cs-libpack')
    if len(sys.argv) == 2 and sys.argv[1] in (*live_modes, '--gs-libpack', '--gs-libpack-ssbo'):
        result = run(sys.argv[1][2:], original, args=(sys.argv[1],), env=dict(os.environ, TMPDIR=str(out)))
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        return result.returncode

    if sys.argv[1:] == ['--link-refused-file']:
        for mode, flags in [('player', []), ('debug', ['-DS2A_ANDROID_DEBUG_ENV=1'])]:
            # 编译实际生产翻译单元。夹具也知道开关和文件名，整份测试可执行文件不能用于这个判据。
            obj = out / ('link-refused-' + mode + '.o')
            subprocess.run(base + flags + ['-c', str(original), '-o', str(obj)], check=True, timeout=60)
            code = obj.read_bytes()
            for marker in (b'nvk_link_refused_file', b'libpack-link-refused-v1.tsv'):
                if (marker in code) != (mode == 'debug'):
                    raise AssertionError(f'{mode} shader object has unexpected diagnostic marker {marker!r}')
            result = run('link-refused-' + mode, original, args=('--link-refused-file',), extra_flags=flags)
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)
            if result.returncode:
                return result.returncode
        return 0

    if sys.argv[1:] == ['--pair-libpack']:
        result = run('pair-stages', original, args=('--cs-libpack',),
                     extra_flags=['-DS2A_LIBPACK_PACKER', '-DS2A_ANDROID_DEBUG_ENV=1'])
        if result.returncode:
            sys.stderr.write(result.stdout + result.stderr)
            return result.returncode
        for case, binary, enabled in [('valid', 'valid', True), ('valid', 'bad-version', True),
                                      ('bad-json', 'valid', True), ('other-producer', 'valid', True),
                                      ('valid', 'bad-version', False)]:
            env = dict(os.environ, S2A_TEST_PAIR_CASE=case, S2A_TEST_BINARY_STAGE=binary)
            if enabled:
                env['S2A_TEST_PAIR_ON'] = '1'
            else:
                env.pop('S2A_TEST_PAIR_ON', None)
            result = subprocess.run([str(out/'pair-stages'), '--cs-libpack'], env=env,
                                    text=True, capture_output=True, timeout=30)
            sys.stdout.write(result.stdout)
            if result.returncode:
                sys.stderr.write(result.stderr)
                return result.returncode
            rows = [dict(field.split('=', 1) for field in line.split()[1:])
                    for line in result.stdout.splitlines() if line.startswith('S2A_LIBPAIR ')]
            if len(rows) != (2 if enabled else 0):
                raise AssertionError(f'{case}/{binary}: expected paired events, got {rows}')
            for i, row in enumerate(rows):
                assert row['first'] == ('json', 's2sb')[i] and row['id'] == rows[0]['id']
                assert row['role'] == 'fg' and row['st'] == '32'
                assert int(row['same_source']) == (case != 'other-producer')
                assert (int(row['json_rc']) == 0) == (case != 'bad-json')
                assert (int(row['s2sb_rc']) == 0) == (binary == 'valid')
                assert int(row['end']) - int(row['start']) == int(row['json_us']) + int(row['s2sb_us'])
        graphics = subprocess.run([str(out/'pair-stages'), '--gs-libpack-ssbo'],
                                  env=dict(os.environ, S2A_TEST_PAIR_GRAPHICS='1', S2A_TEST_BINARY_STAGE='valid'),
                                  text=True, capture_output=True, timeout=30)
        sys.stdout.write(graphics.stdout)
        if graphics.returncode:
            sys.stderr.write(graphics.stderr)
            return graphics.returncode
        rows = [dict(field.split('=', 1) for field in line.split()[1:])
                for line in graphics.stdout.splitlines() if line.startswith('S2A_LIBPAIR ')]
        hits = {(r['role'], r['st']) for r in rows
                if r['same_source'] == '1' and r['json_rc'] == '0' and r['s2sb_rc'] == '0'}
        assert {('fg', '1'), ('fg', '3'), ('fg', '7'), ('warm', '3')} <= hits, hits
        return 0

    if sys.argv[1:] == ['--binary-libpack']:
        first = run('binary-stages', original, args=('--cs-libpack',), extra_flags=['-DS2A_LIBPACK_PACKER'],
                    env=dict(os.environ, S2A_TEST_BINARY_STAGE='valid'))
        sys.stdout.write(first.stdout)
        sys.stderr.write(first.stderr)
        if first.returncode:
            return first.returncode
        for mode, case in (('--cs-libpack', 'bad-version'), ('--gs-libpack', 'valid'),
                           ('--gs-libpack', 'missing-varying'), ('--gs-libpack-ssbo', 'valid')):
            subprocess.run([str(out/'binary-stages'), mode], check=True, timeout=30,
                           env=dict(os.environ, S2A_TEST_BINARY_STAGE=case))
        return 0

    if sys.argv[1:] == ['--live-compile-mutants']:
        text = original.read_text()
        mutants = [
            ('default-off', 'nvn_plat_flag("nvk_live_compile_async", 1)', 'nvn_plat_flag("nvk_live_compile_async", 0)',
             '--live-compile-thread', 'compiler ran on the submitting thread'),
            ('inline', '    if (g_live_compile != 1) {', '    if (1) {',
             '--live-compile-thread', 'compiler ran on the submitting thread'),
            ('request-bytes-ignored', '    s2a_sha256_update(&hash, json, len);', '    (void)json; (void)len;',
             '--live-compile-thread', 'compute first use publishes the real compiled program'),
            ('pending-marked-failed', '        return LIVE_PENDING;', '        return LIVE_ABORT;',
             '--live-compile-pending', 'exhausted safe budget must defer'),
        ]
        for name, old, new, mode, expected in mutants:
            assert text.count(old) == 1, (name, 'mutation anchor')
            mutant = out / ('live-mutant-' + name + '.c')
            mutant.write_text(text.replace(old, new))
            result = run('live-mutant-' + name, mutant, args=(mode,))
            assert result.returncode == 1 and expected in result.stderr, (name, result.stdout, result.stderr)
            print(f'{name}: expected assertion failure: {expected}', flush=True)
        print(f'PASS: {len(mutants)} live compile negative controls')
        return 0

    if sys.argv[1:] == ['--ssbo-window-mutants']:
        # 存储缓冲窗口的准入与预热：各撤一处，对应断言必红（红在被修的那一处，不是编译 / 链接错）
        text = original.read_text()
        mutants = [
            ('old-state-gate', 'return g->has_unaligned_ssbo && g->unaligned_ssbo ? LIBPACK_SSBO_WINDOW : LIBPACK_ALIGNED;',
             'return g->has_unaligned_ssbo && g->unaligned_ssbo ? LIBPACK_OTHER_STATE : LIBPACK_ALIGNED;',
             '--gs-libpack-ssbo', 'graphics SSBO window admission (shape 0 ABI=1 unaligned=1): live compile calls=1'),
            ('record-forgets-window', 'rec->lp_ssbo_window = state == LIBPACK_SSBO_WINDOW && prog->lib_sourced;',
             'rec->lp_ssbo_window = 0;', '--gs-libpack-ssbo', 'warm record state (ABI=1 unaligned=1)'),
            ('warm-ignores-window', 'rec->lp_ok == 2 ? (rec->lp_ssbo_window ? LIBPACK_SSBO_WINDOW : LIBPACK_ALIGNED) : LIBPACK_OTHER_STATE',
             'rec->lp_ok == 2 ? LIBPACK_ALIGNED : LIBPACK_OTHER_STATE', '--gs-libpack',
             'warm record wants the window contract, old library: library not used'),
        ]
        for name, old, new, mode, expected in mutants:
            assert text.count(old) == 1, (name, 'mutation anchor')
            path = out / ('ssbo-window-mutant-' + name + '.c')
            path.write_text(text.replace(old, new))
            result = run('ssbo-window-' + name, path, args=(mode,), env=dict(os.environ, TMPDIR=str(out)))
            assert result.returncode != 0 and expected in result.stderr, (name, result.stdout, result.stderr)
            print(f'{name}: expected assertion failure: {expected}', flush=True)
        print(f'PASS: {len(mutants)} SSBO window negative controls')
        return 0

    if sys.argv[1:] == ['--compute-warm-mutants']:
        text = original.read_text()
        mutants = [
            ('no-record', '    rec.lp_ok = unaligned ? 0 : 2;\n    nvk_prog_warm_note(&rec);',
             '    rec.lp_ok = unaligned ? 0 : 2;\n    (void)rec;', '--compute-warm', 'compute program must be recorded in warm manifest'),
            ('wrong-stage', 'memcpy(rec.identity, p->identity, 32); rec.stage_mask = 32;',
             'memcpy(rec.identity, p->identity, 32); rec.stage_mask = 1;', '--compute-warm', 'persisted compute manifest retains identity'),
            ('wrong-build', 'struct entry *e = compute ? build_compute(bytes, len, &views[0], rec->identity, rec->keys[0], 0, NULL) : build(bytes, len, views, nst, rec->identity, 0, NULL);',
             'struct entry *e = build(bytes, len, views, nst, rec->identity, 0, NULL);', '--compute-warm', 'compute program warm builds from prior cache'),
            ('flag-ignored', 'if (!nvn_plat_flag("nvk_compute_warm", 0)) return;',
             '(void)nvn_plat_flag("nvk_compute_warm", 0);', '--compute-warm-off', 'compute warm off must not record programs'),
        ]
        for name, old, new, mode, expected in mutants:
            assert text.count(old) == 1, (name, 'mutation anchor')
            path = out / ('cs-warm-mutant-' + name + '.c')
            path.write_text(text.replace(old, new))
            with tempfile.TemporaryDirectory(prefix='cs-warm-mutant-', dir=out) as td:
                manifest = Path(td) / 'manifest'; manifest.mkdir()
                cache = Path(td) / 'cache'; cache.mkdir()
                env = dict(os.environ, S2A_COMPUTE_WARM='1', S2A_PW_TEST_DIR=str(manifest), S2A_DIRECT_CACHE_DIR=str(cache))
                if mode == '--compute-warm-off':
                    baseline = run('cs-warm-prepare', original, args=('--compute-warm',), env=env)
                    assert baseline.returncode == 0, (name, baseline.stdout, baseline.stderr)
                    env['S2A_COMPUTE_WARM'] = '0'
                result = run('cs-warm-' + name, path, args=(mode,), env=env)
                assert result.returncode != 0 and expected in result.stderr, (name, result.stdout, result.stderr)
                print(f'{name}: expected assertion failure: {expected}', flush=True)
        print(f'PASS: {len(mutants)} compute warm negative controls')
        return 0

    if sys.argv[1:] == ['--diag-mutants']:
        code = original.read_text()
        old = 'const uint64_t trace = diag_begin(identity, stages, 1);'
        assert code.count(old) == 1
        path = out / 'trace-mutant.c'
        path.write_text(code.replace(old, 'const uint64_t trace = 0; (void)identity; (void)stages; (void)diag_begin;'))
        result = run('trace-mutant', path, args=('--diag-compile',))
        assert result.returncode == 1 and 'error:' not in result.stderr, (result.stdout, result.stderr)
        for stage in ('graphics', 'compute'):
            assert f'{stage} live compile emits paired event' in result.stderr, result.stderr
            print(f'PASS: {stage} event removal produces expected behavior failure')
        # 配对接口表诊断（nvk_link_map_diag）：不请求 / 不打印 / 请求不设上限，各撤一处，主用例对应断言必红
        for name, old, new, expected in (
            ('link-map-no-request', 'in.link_map_diag = in.requires_linked_varying_interface && link_map_diag_want();',
             'in.link_map_diag = 0 && link_map_diag_want();', 'link map diag on: paired request asks for the map'),
            ('link-map-no-print', 'if (in.link_map_diag) link_map_note(', 'if (0 && in.link_map_diag) link_map_note(',
             'link map diag on: one line with program key and both sides'),
            ('link-map-level-ignored', 'if (level >= 2 && !link_map_interesting(vs, fs, level)) return;',
             'if (0 && level >= 2 && !link_map_interesting(vs, fs, level)) return;', 'link map diag level 3 skips matching pairs'),
            ('link-map-no-limit', 'memory_order_relaxed) < LINK_MAP_DIAG_MAX;', 'memory_order_relaxed) < 1000;',
             'link map diag stops after 40 pairs and stops asking'),
        ):
            assert code.count(old) == 1, name
            path = out / ('trace-mutant-' + name + '.c')
            path.write_text(code.replace(old, new))
            result = run('trace-mutant-' + name, path)
            assert result.returncode and expected in result.stderr and 'error:' not in result.stderr, (result.stdout, result.stderr)
            print(f'PASS: {name} produces expected behavior failure')
        return 0

    if sys.argv[1:] == ['--gs-libpack-mutants']:
        # 负对照：图形（三段 / 只有顶点）程序查出包预编库的接线各撤一处，--gs-libpack 必红
        code = original.read_text()
        for name, old, new, expected in (
            ('mask-0x3', 'lrc = s2a_libpack_program_v1(lp, keys, stages, &g,', 'lrc = s2a_libpack_program_v1(lp, keys, 0x3, &g,',
             'hit from the library, compiler not called'),
            ('no-topology', '.topology = topology, .profile_json = pc->profile_json,',
             '.topology = NULL, .profile_json = pc->profile_json,',
             'hit from the library, compiler not called'),
            # 取库入口的闸（nvk_shader.c libpack_fetch）：放 0x1 / 0x3 / 0x7。去掉 0x7u：三段（含几何）程序被挡在库外，必须红；
            #   去掉 0x1u：只有顶点的深度程序被挡在库外，必须红在 VS-only 那一例
            ('vs-fs-only', '(!ckey && stages != 0x1u && stages != 0x3u && stages != 0x7u)', '(!ckey && stages != 0x1u && stages != 0x3u)',
             'hit from the library, compiler not called'),
            ('no-vs-only', '(!ckey && stages != 0x1u && stages != 0x3u && stages != 0x7u)', '(!ckey && stages != 0x3u && stages != 0x7u)',
             'libpack VS-only: hit from the library, compiler not called'),
        ):
            assert code.count(old) == 1, (name, 'mutation anchor')
            path = out / ('gs-libpack-mutant-' + name + '.c')
            path.write_text(code.replace(old, new))
            result = run('gs-libpack-' + name, path, args=('--gs-libpack',))
            assert result.returncode != 0 and expected in result.stderr, (name, result.stdout, result.stderr)
            print(f'{name}: expected assertion failure: {expected}', flush=True)
        print('PASS: 4 libpack GS negative controls')
        return 0

    if sys.argv[1:] == ['--cs-libpack-mutants']:
        # 负对照：计算出库的非对齐闸撤掉（state_ok 对 ckey 不生效），--cs-libpack 必红
        code = original.read_text()
        for name, old, new, expected in (
            ('unaligned-gate', 'if (!root || !state_ok || (!ckey && stages != 0x1u && stages != 0x3u && stages != 0x7u) || nvn_plat_flag("no_libpack", 0)) {',
             'if (!root || (!ckey && ((stages != 0x1u && stages != 0x3u && stages != 0x7u) || !state_ok)) || nvn_plat_flag("no_libpack", 0)) {',
             'unaligned CS storage refuses the library'),
            # 诊断（对齐闸拒查日志 / 没走库原因备忘）：各撤一处，--cs-libpack 必红在对应断言
            ('no-summary-line', 'else if (!((n + 1) & 63)) {', 'else if (0) {', '141 refusals log two summary lines'),
            ('no-unaligned-note', '            cs_note_set(ckey, CSN_UNALIGNED);\n', '', 'unaligned refuse is remembered'),
            ('shared-not-named', 'strstr(lw, "exceeds device limit") ? CSN_SHARED :', '0 ? CSN_SHARED :', 'shared-memory gate refusal is remembered'),
            ('hit-keeps-note', 'if (ckey) cs_note_set(ckey, lrc == S2A_LIBPACK_HIT ? CSN_NONE :', 'if (ckey) cs_note_set(ckey, lrc == S2A_LIBPACK_HIT ? CSN_UNALIGNED :', 'a library hit clears the remembered reason'),
        ):
            assert code.count(old) == 1, (name, 'mutation anchor')
            path = out / ('cs-libpack-mutant-' + name + '.c')
            path.write_text(code.replace(old, new))
            result = run('cs-libpack-' + name, path, args=('--cs-libpack',))
            assert result.returncode != 0 and expected in result.stderr, (name, result.stdout, result.stderr)
            print(f'{name}: expected assertion failure: {expected}', flush=True)
        print('PASS: 5 libpack CS negative controls')
        return 0

    if sys.argv[1:] == ['--graphics-image-mutants']:
        # 负对照（#999）：图形阶段存储图像的接线各改坏一处，graphics_image_layout_test 必红。
        #   解析的几处改的是 nvn_vk_program_result.c（拷贝顶替原文件进链接）；声明那处改的是 nvk_shader.c（测试里的 #include 指到拷贝）
        test_c = ROOT/'android/tests/graphics_image_layout_test.c'
        parse_text = (native/'nvn/nvn_vk_program_result.c').read_text(encoding='utf-8')
        shader_text = original.read_text(encoding='utf-8')
        gate = ('if (dst->kind == 3 && dst->count == 1 && dst->slot >= 72 && dst->slot < 88 && !(dst->slot & 1))\n'
                '                dst->tex_slot = (int16_t)((dst->slot - 72) / 2);')
        decl = 'if (le->src_kind == NVN_RB_IMG) sampled_decl('
        cases = [
            ('parse-slot-unset', 'parse', gate, 'if (0)\n                dst->tex_slot = (int16_t)((dst->slot - 72) / 2);', 'maps to image slot 0'),
            ('parse-slot-formula', 'parse', gate, gate.replace('(dst->slot - 72) / 2', '(dst->slot - 70) / 2'), 'maps to image slot 0'),
            ('parse-odd-word', 'parse', gate, gate.replace(' && !(dst->slot & 1)', ''), 'odd word 73: tex_slot stays -1'),
            ('parse-table-widened', 'parse', gate, gate.replace('dst->slot < 88', 'dst->slot < 90'), 'word 88 above the image table: tex_slot stays -1'),
            ('parse-array-accepted', 'parse', gate, gate.replace(' && dst->count == 1', ''), 'image array at word 72: tex_slot stays -1'),
            ('build-decl-tbuf-only', 'shader', decl, 'if (le->cls == NVN_GPU_RES_IMAGE_TBUF) sampled_decl(', 'image 0: cls 5 reg 0 target 0 class 0 format 0'),
        ]
        for name, which, old, new, expected in cases:
            text = parse_text if which == 'parse' else shader_text
            assert text.count(old) == 1, (name, 'mutation anchor')
            mutated = out/('gfx-image-mutant-' + name + '.c')
            mutated.write_text(text.replace(old, new), encoding='utf-8')
            exe = out/('gfx-image-' + name)
            if which == 'parse':
                files = [test_c, mutated, *libs]
            else:
                fixture = out/('gfx-image-mutant-' + name + '-test.c')
                fixture.write_text(test_c.read_text(encoding='utf-8').replace('#include "../native/gfx_plat/nvk_shader.c"', '#include "' + str(mutated) + '"')
                                   .replace('#include "tbuf_spirv_fixtures.h"', '#include "' + str(ROOT/'android/tests/tbuf_spirv_fixtures.h') + '"'), encoding='utf-8')
                files = [fixture, *libs]
            host_link.run(base + [str(p) for p in files] + [strip, '-lm', '-o', str(exe)], check=True, timeout=120)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            assert result.returncode != 0 and expected in result.stderr, (name, result.stdout, result.stderr)
            print(f'{name}: expected assertion failure: {expected}', flush=True)
        print(f'PASS: {len(cases)} graphics image negative controls')
        return 0

    if sys.argv[1:] == ['--cs-ubo-sampler-mutants']:
        # 负对照（#1594 片 10）：计算程序装载时把游戏 UBO 形态的类型 3 句柄放进程序的 usrc 表（build_compute 的 usrc_put），撤掉 / 改成对所有项都放，默认流程里的用例必红
        code = original.read_text()
        guard = '        if (le->src_kind == NVN_RB_TEXU && !usrc_put(&e->prog, b, le)) { red1("too_many_uniform_handles", "cs"); e->failed = 1; break; }\n'
        for name, new, expected in (
            ('no-usrc-put', '', 'compute UBO constant sampler layout entry'),
            ('usrc-put-for-all', '        if (!usrc_put(&e->prog, b, le)) { red1("too_many_uniform_handles", "cs"); e->failed = 1; break; }\n',
             'the c2 constant sampler entry of the same program stays a plain TEX entry'),
        ):
            assert code.count(guard) == 1, (name, 'mutation anchor')
            path = out / ('cs-ubo-sampler-mutant-' + name + '.c')
            path.write_text(code.replace(guard, new))
            result = run('cs-ubo-sampler-' + name, path)
            assert result.returncode != 0 and expected in result.stderr, (name, result.stdout, result.stderr)
            print(f'{name}: expected assertion failure: {expected}', flush=True)
        print('PASS: 2 compute UBO constant sampler negative controls')
        return 0

    if len(sys.argv) in (3, 4) and sys.argv[1] == '--tbuf-layout':
        exe = out/'tbuf-layout'
        src = [ROOT/'android/tests/tbuf_layout_test.c', *libs]
        host_link.run(base + [str(p) for p in src] + [strip, '-lm', '-o', str(exe)], check=True, timeout=60)
        result = subprocess.run([str(exe), *sys.argv[2:]], capture_output=True, text=True, timeout=30)
        sys.stdout.write(result.stdout); sys.stderr.write(result.stderr)
        return result.returncode

    if len(sys.argv) == 6 and sys.argv[1] in ('--emit-high-slot', '--execute-high-slot', '--emit-reuse-texture', '--emit-reuse-rt', '--emit-reuse-inactive', '--emit-real', '--emit-real-tbuf', '--execute-tbuf', '--emit-compute-tbuf', '--execute-compute-tbuf', '--reject-graphics-tbufu', '--reject-compute-tbufu'):
        result = run('emit-real', original, args=sys.argv[1:])
        sys.stdout.write(result.stdout); sys.stderr.write(result.stderr)
        return result.returncode

    if len(sys.argv) == 7 and sys.argv[1] == '--tbuf-withdraw':
        profile, closure, src_dir, gfx_result, cs_result = sys.argv[2:]
        code = original.read_text(encoding='utf-8')
        layout_anchor = 'if (b->kind == 2 && b->texel_buffer && b->count == 1 && b->tex_slot >= 0)'
        assert code.count(layout_anchor) == 1
        mutated = out/'tbuf-withdraw-layout.c'
        mutated.write_text(code.replace(layout_anchor, layout_anchor.replace('&& b->texel_buffer', '&& 0 && b->texel_buffer')))
        fixture = out/'tbuf-withdraw-layout-test.c'
        fixture.write_text((ROOT/'android/tests/tbuf_layout_test.c').read_text().replace(
            '#include "../native/gfx_plat/nvk_shader.c"', '#include "' + str(mutated) + '"'))
        exe = out/'tbuf-withdraw-layout-test'
        host_link.run(base + [str(p) for p in [fixture, *libs]] + [strip, '-lm', '-o', str(exe)], check=True, timeout=60)
        for path, extra in [(gfx_result, ()), (cs_result, ('--compute',))]:
            result = subprocess.run([str(exe), path, *extra], capture_output=True, text=True, timeout=30)
            assert result.returncode == 1 and 'FAIL: BUFFER layout must be TBUF' in result.stderr, result.stderr
        print('NEGATIVE CONTROL PASS: removing TBUF layout fails graphics and compute assertions')
        type_anchor = 'else if (dim == 5 && !arrayed && !ms) target = NVN_TEXTURE_TARGET_BUFFER;'
        assert code.count(type_anchor) == 1
        mutated = out/'tbuf-withdraw-type.c'
        mutated.write_text(code.replace(type_anchor, type_anchor.replace('dim == 5', '0 && dim == 5')))
        for mode, path in [('--execute-tbuf', gfx_result), ('--execute-compute-tbuf', cs_result)]:
            result = run('tbuf-withdraw-type', mutated, args=(mode, profile, closure, src_dir, path))
            assert result.returncode == 1 and 'FAIL: real' in result.stderr and 'declared TBUF layout' in result.stderr, result.stderr
        print('NEGATIVE CONTROL PASS: unproven TBUF shader type rejects graphics and compute')
        # Mutate the actual dependency cache header independently for each
        # guard; the compiler sees the copied producer and copied header.
        cache = (native/'gfx_plat/nvk_shader_fact_cache.h').read_text()
        base.append('-I'+str(native/'gfx_plat'))
        for name, before, after in [
            ('input', ' || t->separate || t->is_buffer) return 0;', ' || t->separate) return 0;'),
            ('reflection', 'e->src_kind == NVN_RB_TBUF || ', ''),
        ]:
            assert cache.count(before) == 1, (name, 'mutation anchor')
            directory = out/('tbuf-cache-' + name)
            directory.mkdir(exist_ok=True)
            shader = directory/'nvk_shader.c'
            shader.write_text(code)
            (directory/'nvk_shader_fact_cache.h').write_text(cache.replace(before, after))
            if name == 'input':
                result = run('tbuf-cache-input-test', shader)
                expected = 'TBUF input fact prevents dependency identity reuse'
            else:
                fixture = directory/'tbuf-layout-test.c'
                fixture.write_text((ROOT/'android/tests/tbuf_layout_test.c').read_text().replace(
                    '#include "../native/gfx_plat/nvk_shader.c"', '#include "' + str(shader) + '"'))
                exe = directory/'test'
                host_link.run(base + [str(p) for p in [fixture, *libs]] + [strip, '-lm', '-o', str(exe)], check=True, timeout=60)
                result = subprocess.run([str(exe), gfx_result], capture_output=True, text=True, timeout=30)
                expected = 'reflected TBUF prevents dependency identity reuse'
            assert result.returncode == 1 and expected in result.stderr, (name, result.stdout, result.stderr)
            print('NEGATIVE CONTROL PASS: TBUF cache ' + name + ' guard -> ' + expected, flush=True)
        return 0

    if len(sys.argv) == 6 and sys.argv[1] == '--execute-reuse':
        result = run('execute-reuse', original, args=tuple(sys.argv[1:]))
        sys.stdout.write(result.stdout); sys.stderr.write(result.stderr)
        return result.returncode
    if sys.argv[1:] == ['--withdraw']:
        text = original.read_text(encoding='utf-8')
        changes = [
            # 出包预编库缺失 / 坏要说出来（--gs-libpack 单独进程；offline-shader-miss 修法）
            ('libpack-missing-silent', "                nvn_plat_log(NVN_PLAT_LOG_WARN, \"shader\", b);\n            }\n        }\n    }\n    return lp;",
             "                (void)b;\n            }\n        }\n    }\n    return lp;", 1, 'missing libpack directory is one yellow line', ('--gs-libpack',)),
            ('libpack-missing-every-call', 'else if (strcmp(said_root, root)) {', 'else if (1) {', 1, 'missing libpack directory is one yellow line', ('--gs-libpack',)),
            ('libpack-state-not-set', 'atomic_store(&g_lp_state, orc < 0 ? LP_ST_BAD : LP_ST_MISSING);', '(void)0;', 1, 'libpack tally says the library is missing', ('--gs-libpack',)),
            ('libpack-bad-as-missing', 'atomic_store(&g_lp_state, orc < 0 ? LP_ST_BAD : LP_ST_MISSING);', 'atomic_store(&g_lp_state, LP_ST_MISSING);', 1, 'broken container: tally says bad', ('--gs-libpack',)),
            ('gs-source-epoch', 'MIX(s2a_source_epoch());\n    MIX(stages);', '(void)s2a_source_epoch();\n    MIX(stages);', 1, 'poisoned GS source cannot hit fast cache'),
            ('cs-query-proof-omitted', 'e->prog.sampler_unused[0][e->prog.nent[0]] = (uint8_t)s2a_spirv_sampler_unused(',
             'e->prog.sampler_unused[0][e->prog.nent[0]] = 0 && (uint8_t)s2a_spirv_sampler_unused(', 1,
             'production compute builder retains sampler use proof: query=1'),
            ('cs-query-proof-forced', 'e->prog.sampler_unused[0][e->prog.nent[0]] = (uint8_t)s2a_spirv_sampler_unused(',
             'e->prog.sampler_unused[0][e->prog.nent[0]] = 1 || (uint8_t)s2a_spirv_sampler_unused(', 1,
             'production compute builder retains sampler use proof: query=0'),
            ('gs-slot', '(const char *const[]){"vs", "fs", "gs"}[st]',
             '(const char *const[]){"vs", "fs", "fs"}[st]', 3, 'GS production request reaches compiler'),
            ('ubo-learn-reported-red', 'g_ubo_learned = 1;\n            else red1("compile_failed", oc.summary);',
             'g_ubo_learned = 1;\n            red1("compile_failed", oc.summary);', 1, 'a rejection that taught a coordinate is a learning step'),
            ('gs-wrong-stage', 'src.stage = (nvn_gpu_stage)st;',
             'src.stage = st == 2 ? NVN_GPU_SH_FS : (nvn_gpu_stage)st;', 1, 'result loads VS FS and GS'),
            ('gs-key-omitted', '((stages & 4u) ? 3 : 2)',
             '((stages & 4u) ? 2 : 2)', 1, 'changed GS key recompiles'),
            ('gs-texture-omitted', 'if (stages & 4u) MIX(rp_tex_fact_sig(rp_S.tex[2], 32, rp_S.tex_base, rp_S.tex_pool_n));',
             'if (0) MIX(rp_tex_fact_sig(rp_S.tex[2], 32, rp_S.tex_base, rp_S.tex_pool_n));', 1, 'changed GS texture fact recompiles'),
            ('gs-separate-fast-omitted', 'if (stages & 4u) MIX(sep_fact_sig(2));',
             'if (0) MIX(sep_fact_sig(2));', 1, 'GS separate texture fact reaches compiler'),
            ('gs-separate-generation-omitted', 'const uint64_t sg = rp_tex_sep_gen[st], rg = rp_tex_reg_gen();',
             'const uint64_t sg = st == 2 ? 0 : rp_tex_sep_gen[st], rg = rp_tex_reg_gen();', 1, 'changed GS separate slot invalidates cached signature and recompiles'),
            ('gs-separate-stage-wrong', '(const char *const[]){"vs", "fs", "gs"}[st], (uint32_t)(90',
             '(const char *const[]){"vs", "fs", "fs"}[st], (uint32_t)(90', 1, 'GS separate texture fact reaches compiler'),
            ('gs-cap-ignored', 'if (!(caps.caps & NVN_GPU_CAP_GEOMETRY))',
             'if (0 && !(caps.caps & NVN_GPU_CAP_GEOMETRY))', 1, 'disabled GS fails before compiler'),
            ('withdraw-guard', 'if (!shader_texture_pool_ready()) return -1;',
             'if (0 && !shader_texture_pool_ready()) return -1;', 2, 'expected accounting=texture_pool_over_limit'),
            ('wrong-gfx-length', 'in.texture_array_length = s->tex_pool_n;',
             'in.texture_array_length = 1;', 1, 'gfx n=7 request retains exact capacity'),
            ('wrong-cs-length', 'st.tex_pool_n = (uint32_t)rp_S.tex_pool_n;',
             'st.tex_pool_n = 1;', 1, 'cs n=7 request retains exact capacity'),
            ('wrong-identity', 'id.texture_array_length = (uint32_t)rp_S.tex_pool_n;',
             'id.texture_array_length = 1;', 1, 'rebind changes request and identity'),
            ('drop-debug-log', 'nvn_plat_flag("nvk_texpool_log", 0)',
             '(0 && nvn_plat_flag("nvk_texpool_log", 0))', 1, 'debug one line records first bound value'),
            ('wrong-array-red', 'nvn_unimpl("shader", "texture_pool_array_unsupported", NULL);',
             'nvn_unimpl("shader", "texture_pool_array_wrong", NULL);', 1,
             'reports the dedicated unimplemented'),
            ('drop-final-summary', 's2a_direct_summary_register(pool_final_summary);',
             '(void)pool_final_summary;', 1, 'normal process-exit callback emits final stats'),
            ('timeout-is-not-final', 'if (normal && (g_pool_programs || g_pool_request_rejected || g_pool_reason_count)) pool_stats("final");',
             'if (g_pool_programs || g_pool_request_rejected || g_pool_reason_count) pool_stats("final");', 1, 'timeout summary is never labelled final'),
            ('flatten-missing-fact', 'else if (!strcmp(code, "MISSING_FACT")) {',
             'else if (0 && !strcmp(code, "MISSING_FACT")) {', 1, 'compiler reason counted accurately'),
            ('drop-request-count', 'g_pool_request_rejected++;',
             '(void)g_pool_request_rejected;', 1, 'request failure is counted separately'),
            ('drop-separate-vs', 'for (int st = 0; st < nst; st++) for (int slot = 0; slot < 128; slot++) {',
             'for (int st = 0; st < nst; st++) for (int slot = 0; slot < 0; slot++) {', 1, 'separate VS registered view fact is sent'),
            ('drop-separate-cs', 'for (int slot = 0; slot < 128; slot++) {\n        uint64_t h = rp_S.tex[5][128 + slot];',
             'for (int slot = 0; slot < 0; slot++) {\n        uint64_t h = rp_S.tex[5][128 + slot];', 1, 'separate CS registered texture fact is sent'),
            ('drop-separate-fast-identity', 'MIX(sep_fact_sig(0));',
             '(void)rp_S.tex[0][128];', 1, 'separate texture rebind invalidates fast identity'),
            # 计算程序快路：摘要里每一项各去掉一次，对应的负对照用例必须变红；快路本身关掉，「换地址仍复用」那条必须变红。
            ('cs-fast-path-disabled', 'struct last_ent *le = fast ? last_find(key, pc.session, pc.generation) : NULL;',
             'struct last_ent *le = (fast && 0) ? last_find(key, pc.session, pc.generation) : NULL;', 1,
             'CS new addresses and same-fact handles reuse the program without a request'),
            ('cs-epoch-omitted', 'MIX(0xC5C5C5C5C5C5C5C5ull); MIX(s2a_source_epoch());',
             'MIX(0xC5C5C5C5C5C5C5C5ull); (void)s2a_source_epoch();', 1, 'CS source poison advances the epoch'),
            ('cs-source-key-omitted', '{ uint64_t k; memcpy(&k, rp_S.skey[5], 8); MIX(k); }',
             '{ (void)rp_S.skey[5]; }', 1, 'changed CS source key recompiles'),
            ('cs-ubo-use-omitted', '{ uint32_t m = 0; for (int i = 0; i < 16; i++) if (rp_S.ubo[5][i].size) m |= 1u << i; MIX(m); }',
             '{ (void)rp_S.ubo[5]; }', 1, 'changed CS constant-buffer use recompiles'),
            ('cs-unaligned-omitted', 'rp_S.ssbo[5][i].addr % pc->storage_buffer_offset_alignment) un = 1; MIX(un);',
             'rp_S.ssbo[5][i].addr % pc->storage_buffer_offset_alignment) un = 1; MIX(un & 0);', 1, 'unaligned CS storage address recompiles'),
            ('cs-pool-omitted', 'MIX(rp_S.tex_pool_n); MIX(rp_S.samp_pool_n); MIX(rp_S.tex_base);',
             'MIX(0);', 1, 'CS pool size update recompiles'),
            ('cs-combined-omitted', 'MIX(combined_fact_sig(5));', '(void)0;', 1, 'changed CS texture fact recompiles'),
            ('cs-separate-omitted', 'MIX(sep_fact_sig(5));', '(void)0;', 1, 'changed CS separate texture fact recompiles'),
            ('cs-separate-generation-omitted', 'const uint64_t sg = rp_tex_sep_gen[st], rg = rp_tex_reg_gen();',
             'const uint64_t sg = st == 5 ? 0 : rp_tex_sep_gen[st], rg = rp_tex_reg_gen();', 1, 'changed CS separate texture fact recompiles'),
            # 图形 / 计算共用的快表（#1146）：桶数退回 512，「同一旧桶里的 5 个键共存」必须变红；收尾不释放表，「会话收尾释放」必须变红；
            # 存项时不分配（表永远建不起来），所有访问都落空，同一条必须变红。
            ('last-table-512', 'enum { LAST_BITS = 12,', 'enum { LAST_BITS = 9,', 1, 'coexist in the fast table'),
            ('last-table-kept', 'free(g_last_tab); g_last_tab = NULL;', '(void)g_last_tab;', 1, 'released at session retirement'),
            ('last-table-never-built', 'if (!g_last_tab && !(g_last_tab = calloc(1, sizeof *g_last_tab))) return;',
             'if (!g_last_tab) return;', 1, 'coexist in the fast table'),
            # 结果合并（#1355）前台路径：每一道判据撤一次，对应的用例必须变红
            ('alias-fg-disabled', '    if (alias_on()) {\n        int a_adopted = 0;', '    if (0 && alias_on()) {\n        int a_adopted = 0;', 1,
             'same result -> same program object'),
            ('alias-result-ignored', 'memcpy(head, res, 32); n += 32;', 'memset(head, 0, 32); n += 32;', 1,
             'different result is not merged'),
            ('alias-c1-ignored', 's2a_sha256_update(&h, len4, 4); s2a_sha256_update(&h, k->c1[st], k->c1_len[st]);',
             '(void)len4;', 1, 'different c1 is not merged'),
            ('alias-fact-registration-omitted', 'if (reusable && ae->fact_ok) fact_cache_keep_mask(&id, fact_base, ae->fact_mask, &ae->prog);',
             '(void)ae->fact_mask;', 1, 'reuses the program (no live compile'),
            # 预热线程建的那份也要算事实掩码：撤掉后，合并命中它的前台身份登记不上事实复用（真机现编 3 → 198 次的另一半）
            # 事实复用（#1187）：身份不同、编译器读到的事实相同 → 复用同一程序，不再编
            ('fact-reuse-disabled', 'if (reusable && (*out = fact_cache_find(&id, fact_base))',
             'if (0 && reusable && (*out = fact_cache_find(&id, fact_base))', 1, 'unqueried combined texture reuses compiled program'),
            # 高槽（c2 字 72–86）事实、签名与图像优先：撤掉任一处，对应断言必须变红
            ('high-facts-withdraw', 'for (int slot = 0; slot < 8; slot++) {\n            long id = handles[slot]',
             'for (int slot = 0; slot < 0; slot++) {\n            long id = handles[slot]', 1, 'high-slot texture fact'),
            ('high-signature-withdraw', 'MIX(graphics_high_fact_sig(st));', '(void)graphics_high_fact_sig;', 1,
             'high-slot image precedence invalidates identity'),
            ('high-image-precedence-withdraw', 'rp_S.img[st][slot] ? rp_S.img[st][slot] : rp_S.tex[st][32 + slot]',
             'rp_S.tex[st][32 + slot]', 1, 'high-slot image precedence invalidates identity'),
            # 会话重置清空事实复用的别名表与依赖索引（rp_shader_reset → fact_cache_reset）
            ('fact-reset-withdraw', '    fact_cache_reset();', '    (void)fact_cache_reset;', 1, 'reset clears aliases and dependency index'),
            # 只有活动阶段的 UBO 使用进图形请求事实（不活动阶段的 UBO 不改身份）
            ('inactive-ubo-withdraw', 'for (int st = 0; st < nst; st++) { uint32_t m = 0;', 'for (int st = 0; st < 6; st++) { uint32_t m = 0;', 1,
             'inactive stage UBO keeps graphics identity'),
            ('alias-bg-fact-mask-omitted', 'if (!compute && e && !e->failed && alias_on()) e->fact_ok = (uint8_t)fact_cache_mask(bytes, len, &e->prog, e->fact_mask);',
             '(void)0;', 1, 'alias fact-bg[on] s2: identity differing only in an unread texture fact reuses the program'),
            # 开关关闭态（--alias-off 另起进程）：开关读了不用，关闭态下 B 就会被合并，「B 读自己的文件」必须变红
            ('alias-flag-ignored', 'if (v < 0) v = nvn_plat_flag("nvk_alias", 1) ? 1 : 0; return v;',
             'if (v < 0) { (void)nvn_plat_flag("nvk_alias", 1); v = 1; } return v;', 1, 'alias[off] s2: B reads its own file', ('--alias-off',)),
            # 编译被拒时的上下文日志（#999，只加日志）：每个分类、每个打印项、每道预算各撤一次，对应用例必须变红
            ('img-unbound-as-no-texture', 'miss->why = handles[slot] ? IMGF_NO_TEXTURE : IMGF_UNBOUND; continue;',
             'miss->why = IMGF_NO_TEXTURE; continue;', 1, 'slot unbound, pool entry 0 not registered: counter image_fact_unbound'),
            ('img-no-texture-as-unbound', 'miss->why = handles[slot] ? IMGF_NO_TEXTURE : IMGF_UNBOUND; continue;',
             'miss->why = IMGF_UNBOUND; continue;', 1, 'image handle names an unregistered texture: counter image_fact_no_texture'),
            ('img-no-fact-dropped', '{ miss->why = IMGF_NO_FACT; continue; }', '{ continue; }', 1,
             'registered texture whose format has no fact: counter image_fact_no_fact'),
            ('img-format-not-printed', '(unsigned)m->fmt, (unsigned)m->target, (unsigned)m->layers', '0u, (unsigned)m->target, (unsigned)m->layers', 1,
             'registered texture whose format has no fact: image line says why'),
            ('img-texture-not-printed', '" handle=0x%x texture=%d", (unsigned)m->handle, (int)m->id', '" handle=0x%x texture=%d", (unsigned)m->handle, 0', 1,
             'image handle names an unregistered texture: image line says why'),
            ('reject-context-never-called', '    if (snap) reject_context(snap, reason, message);', '    if (0 && snap) reject_context(snap, reason, message);', 1,
             'one program line and one image line, both with the program key'),
            ('learning-step-logs', 'compiler_rejected(oc.summary, learned ? NULL : s);', 'compiler_rejected(oc.summary, s);', 1,
             'a rejection that only taught a coordinate prints no program-key line'),
            ('final-rejection-silent', 'compiler_rejected(oc.summary, learned ? NULL : s);', 'compiler_rejected(oc.summary, learned ? NULL : NULL);', 1,
             'a rejection nothing can be learned from prints its program key'),
            ('key-from-stage-0', 'o += snprintf(key + o, sizeof key - (size_t)o, "%02x", s->keys[st][b]);', 'o += snprintf(key + o, sizeof key - (size_t)o, "%02x", s->keys[0][b]);', 1,
             'one program line and one image line, both with the program key'),
            ('program-line-budget-removed', 'if (g_reject_lines < REJECT_LINES_MAX) {', 'if (1) {', 1, 'each kind of line stops at 128 per session'),
            ('image-line-budget-removed', 'if (g_img_reject_lines >= REJECT_LINES_MAX) return;', 'if (0) return;', 1, 'each kind of line stops at 128 per session'),
            ('line-budget-halved', 'enum { REJECT_LINES_MAX = 128 };', 'enum { REJECT_LINES_MAX = 64 };', 1, 'each kind of line stops at 128 per session'),
            ('line-budget-not-reset', '    g_reject_lines = g_img_reject_lines = g_reject_total = g_img_reject_total = 0;\n', '    (void)g_reject_lines;\n', 1, 'a new session gets its line budget back'),
            ('reject-total-not-reset', '    g_reject_lines = g_img_reject_lines = g_reject_total = g_img_reject_total = 0;\n', '    g_reject_lines = g_img_reject_lines = 0;\n', 1,
             'a new session gets its line budget back (and no stale truncation notice)'),
            # 超预算时的「已截断，共 N 条」：读数整条撤掉 / 两个总数各撤一次，对应用例必须变红
            ('truncation-notice-dropped', 'if (g_reject_total > g_reject_lines || g_img_reject_total > g_img_reject_lines) {', 'if (0) {', 1,
             'a truncated session says how many rejections there were'),
            ('program-total-not-counted', '    g_reject_total++;\n', '', 1, 'a truncated session says how many rejections there were'),
            ('image-total-not-counted', '    g_img_reject_total++;\n', '', 1, 'a truncated session says how many rejections there were'),
            # 常量缓冲槽号：只有 2 才是 c2 图像句柄表；槽号检查撤掉 / 其它槽也算出 c2 槽位 / 行里不带槽号，对应用例必须变红
            ('cbuf-slot-ignored', 'const int c2 = cbuf == 2;', 'const int c2 = 1;', 1, 'user cbuf slot 3, word 74: counter image_fact_cbuf_other'),
            ('cbuf-other-slot-computed', 'const int slot = c2 && w && word >= 72', 'const int slot = w && word >= 72', 1,
             'user cbuf slot 3, word 74: the image line names the cbuf slot and reads no c2 record'),
            ('cbuf-not-printed', 'st == 2 ? "gs" : "?", cbuf, slot, word, key);', 'st == 2 ? "gs" : "?", 2ul, slot, word, key);', 1,
             'user cbuf slot 3, word 74: the image line names the cbuf slot and reads no c2 record'),
            ('word-range-widened', 'word >= 72 && word <= 86 && !(word & 1)', 'word >= 72 && word <= 90 && !(word & 1)', 1, 'word 88 is outside the image table: counter'),
            ('odd-word-accepted', 'word >= 72 && word <= 86 && !(word & 1)', 'word >= 72 && word <= 86', 1, 'odd word: counter'),
            ('stage-prefix-ignored', 'const int st = !strncmp(message, "vs:", 3) ? 0 : !strncmp(message, "fs:", 3) ? 1 : !strncmp(message, "gs:", 3) ? 2 : -1;', 'const int st = 1;', 1,
             'vertex stage slot 0 (word 72)'),
            ('texture-kind-counted-as-image', 'const char *img = strstr(message, "[kind=image ");', 'const char *img = strstr(message, "[kind=");', 1,
             'a texture-kind rejection prints only the program line'),
            ('image-counter-not-recorded', '    nvn_unimpl("shader", name, NULL);\n    g_img_reject_total++;',
             '    (void)0;\n    g_img_reject_total++;', 1, 'slot unbound, pool entry 0 not registered: counter image_fact_unbound'),
        ]
        for name, before, after, count, expected, *more in changes:
            assert text.count(before) == count, (name, 'mutation anchor')
            path = out/(name+'.c')
            path.write_text(text.replace(before, after), encoding='utf-8')
            result = run(name, path, args=(more[0] if more else ()))
            assert result.returncode != 0 and expected in result.stderr, (name, result.stdout, result.stderr)
            print(f'{name}: expected assertion failure: {expected}', flush=True)
        print(f'PASS: {len(changes)} negative controls')
        return 0
    if sys.argv[1:]:
        raise ValueError('usage: nvk_shader_pool_test.py [--withdraw | --gs-libpack-mutants | --cs-libpack-mutants | --compute-warm-mutants | --diag-mutants | --live-compile-mutants | --graphics-image-mutants | --cs-ubo-sampler-mutants]'
                         '（其余参数是 gs_compiler_chain_test.py / tbuf_compiler_chain_test.py 的子命令）')
    # 存储图像的声明（数值类 + SPIR-V Image Format）落程序记录：生产 sampled_decl，手搭 SPIR-V
    exe = out/'image-decl'
    host_link.run(base + [str(ROOT/'android/tests/image_decl_test.c'), *[str(p) for p in libs]] + [strip, '-lm', '-o', str(exe)], check=True, timeout=60)
    decl = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    sys.stdout.write(decl.stdout); sys.stderr.write(decl.stderr)
    if decl.returncode:
        return decl.returncode
    # #1594：类型 3 句柄（常量采样器）的布局映射：真实结果解析 + 生产 layout_ent（编码 / 下标 0 与 4095 / 其它来源不变）
    exe = out/'const-sampler-layout'
    host_link.run(base + [str(ROOT/'android/tests/const_sampler_layout_test.c'), *[str(p) for p in libs]] + [strip, '-lm', '-o', str(exe)], check=True, timeout=60)
    csl = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    sys.stdout.write(csl.stdout); sys.stderr.write(csl.stderr)
    if csl.returncode:
        return csl.returncode
    # #999：图形阶段的存储图像：真实结果解析 + 生产 layout_ent + 生产 build（句柄字 → 图像槽 / 声明）；负对照在 --graphics-image-mutants
    exe = out/'graphics-image-layout'
    host_link.run(base + [str(ROOT/'android/tests/graphics_image_layout_test.c'), *[str(p) for p in libs]] + [strip, '-lm', '-o', str(exe)], check=True, timeout=120)
    gil = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    sys.stdout.write(gil.stdout); sys.stderr.write(gil.stderr)
    if gil.returncode:
        return gil.returncode
    diag = run('diag-compile', original, args=('--diag-compile',))
    sys.stdout.write(diag.stdout); sys.stderr.write(diag.stderr)
    if diag.returncode:
        return diag.returncode
    result = run('test', original)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    live_rc = 0
    for mode in live_modes:
        live = subprocess.run([str(out/'test'), mode], capture_output=True, text=True, timeout=30)
        sys.stdout.write(live.stdout); sys.stderr.write(live.stderr)
        live_rc = live_rc or live.returncode
    # 三段程序（vs + fs + gs）查出包预编库：环境变量与深度能力缓存是进程级的，单独起一个进程
    gsl = run('test-gs-libpack', original, args=('--gs-libpack',))
    sys.stdout.write(gsl.stdout); sys.stderr.write(gsl.stderr)
    # 同一份夹具再带一份有窗口合同的库（存储缓冲窗口：准入放行 + 预热从库取），同样进程级
    gss = run('test-gs-libpack-ssbo', original, args=('--gs-libpack-ssbo',))
    sys.stdout.write(gss.stdout); sys.stderr.write(gss.stderr)
    # 计算程序查出包预编库的非对齐闸（#496085）：库根环境变量进程级，单独起一个进程
    csl2 = run('test-cs-libpack', original, args=('--cs-libpack',))
    sys.stdout.write(csl2.stdout); sys.stderr.write(csl2.stderr)
    # 结果合并开关关闭态（debug.s2a.nvk_alias=0）：开关在进程内只读一次，单独起一个进程
    off = run('test-alias-off', original, args=('--alias-off',))
    sys.stdout.write(off.stdout)
    sys.stderr.write(off.stderr)
    with tempfile.TemporaryDirectory(prefix='compute-warm-', dir=out) as td:
        warm_dir = Path(td) / 'manifest'; warm_dir.mkdir()
        cache_on = Path(td) / 'cache-on'; cache_on.mkdir()
        cache_off = Path(td) / 'cache-off'; cache_off.mkdir()
        env = dict(os.environ, S2A_COMPUTE_WARM='1', S2A_PW_TEST_DIR=str(warm_dir), S2A_DIRECT_CACHE_DIR=str(cache_on))
        on = run('test-compute-warm', original, args=('--compute-warm',), env=env)
        sys.stdout.write(on.stdout); sys.stderr.write(on.stderr)
        env.update(S2A_COMPUTE_WARM='0', S2A_DIRECT_CACHE_DIR=str(cache_off))
        cs_off = run('test-compute-warm-off', original, args=('--compute-warm-off',), env=env)
        sys.stdout.write(cs_off.stdout); sys.stderr.write(cs_off.stderr)
    return result.returncode or live_rc or gsl.returncode or gss.returncode or csl2.returncode or off.returncode or on.returncode or cs_off.returncode

if __name__ == '__main__':
    raise SystemExit(main())
