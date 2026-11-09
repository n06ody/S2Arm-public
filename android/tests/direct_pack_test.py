#!/usr/bin/env python3
"""直连产物 v1：make_pack.py --direct-graphics 的 requiredFeatures/extensions 与 descriptor 字段规则。

只验证出包侧（写什么、拒绝什么）；消费侧由 native direct_graphics_test.c 与 Java BootContractTest 各自覆盖。
"""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('android_make_pack', HERE.parent / 'tools/make_pack.py')
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)
TITLE = '0100000000ABCDEF'
BUILD = '1234567890abcdef1234567890abcdef'
SID = '0100000000000823'
CC = shutil.which('cc') or shutil.which('clang')
NATIVE = HERE.parent / 'native'
INCLUDES = ['-I', str(NATIVE), '-I', str(HERE.parent.parent / 'runtime' / 'include')]
CONSUMER_SOURCES = [str(HERE / 'direct_graphics_test.c'), str(NATIVE / 'direct/graphics.c'),
                    str(NATIVE / 'direct/json.c'), str(NATIVE / 'direct/sha256.c'),
                    str(NATIVE / 'direct/s2a_direct_abi.c')]


def descriptor(**overrides):
    doc = {
        'schema': 's2a-graphics-v1',
        'titleId': TITLE,
        'gameContentRoot': 'sha256-' + 'a' * 64,
        'moduleSetDigest': 'sha256-' + 'b' * 64,
        'frontend': 'nvn',
        'backend': 'vulkan-direct',
        'directABI': 1,
        'translatorClosure': 'd2r-1.0+nvn-2d9c7704',
        'sourceParserVersion': 2,
        'runtimeCompilerABI': 's2a-direct-compile-v1',
        'profiles': [],
        # 文件清单现在会与真实 index/字节核对；这个夹具只验字段规则，所以给空清单
        # （合法：允许离线覆盖为空）。真实库文件的接线在 direct_library_test.py。
        'files': [],
    }
    doc.update(overrides)
    return doc


class DirectPackTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.root = self.base / 'product'
        for directory in ['exefs', 'romfs', 'ctl', 'systemdata/fonts']:
            (self.root / directory).mkdir(parents=True)
        (self.root / 'unpack.tsv').write_text(f'title_id\t{TITLE}\ndisplay_version\tfixture\n')
        main = bytearray(128); main[64:80] = bytes.fromhex(BUILD)
        (self.root / 'exefs/main').write_bytes(main)
        (self.root / 'exefs/main.npdm').write_bytes(b'fixture NPDM')
        nacp = bytearray(0x4000); struct.pack_into('<Q', nacp, 0x3038, int(TITLE, 16))
        (self.root / 'ctl/control.nacp').write_bytes(nacp)
        (self.root / 'romfs/game.txt').write_text('synthetic game data')
        (self.root / 'systemdata/fonts/fixture.bfttf').write_bytes(b'not a real font')
        # make_pack 只消费产品根里已由 refresh sysdata 阶段准备好的系统数据；
        # 这里把共用合成器列出的每个号都造一份带载荷的目录（内容不参与断言）。
        for sid in ('0100000000000802', '0100000000000823'):
            (self.root / 'systemdata' / sid).mkdir(parents=True)
            (self.root / 'systemdata' / sid / 'fixture.bin').write_bytes(b'fixture')
        # 安卓打包前置的能力清单：与共用合成根同一个生成入口（refresh 的 gen_caps.sh 安卓口径）。
        pack.synthetic_caps(str(self.root))
        self.table = self.base / 'needs.tsv'
        self.table.write_text(f'{TITLE}\t{BUILD}\tfixture\t{SID}\tfixture archive\ttest\n')
        self.env = patch.dict(os.environ, {'S2A_SYSDATA_NEED': str(self.table), 'S2A_SYSDATA': ''})
        self.env.start(); self.addCleanup(self.env.stop)
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__(); self.addCleanup(output.__exit__, None, None, None)
        self.staging = self.base / 'stage/modules'
        self.out = self.base / 'package'

    @staticmethod
    def rewrite(exefs, staging):
        p = Path(staging); p.mkdir(parents=True)
        (p / 'main.patched.bin').write_bytes(b'synthetic rewritten module')
        (p / 'main.fixups.tsv').write_text('# TARGET\tandroid\n# REWRITE_ABI\tandroid-sp-safe-v1\n')
        (p / 'main.gates.tsv').write_text('# fixture\n')

    @staticmethod
    def address_requirements(root, staging):
        # This fixture's main is a fake non-NSO; the real addr_need collector is
        # covered by make_pack_rewriter_test/sysdata_pack_test. Direct packing only
        # needs an entry here.
        path = Path(staging) / 'addr_need.tsv'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('fixture addr need\n')
        return str(path)

    def entries(self):
        with patch.object(pack, 'rewrite', self.rewrite), \
             patch.object(pack, 'stage_address_requirements', self.address_requirements):
            return pack.entries_from_root(str(self.root), str(self.staging), '')

    def write_descriptor(self, doc=None, raw=None):
        path = self.base / 'graphics.v1.json'
        path.write_bytes(raw if raw is not None else json.dumps(doc or descriptor()).encode())
        return path

    def build(self, descriptor_path=None):
        entries = self.entries()
        graphics = pack.load_graphics(str(descriptor_path)) if descriptor_path else None
        if graphics:
            entries.append((pack.GRAPHICS_MANIFEST, str(descriptor_path)))
        return pack.write_pack(str(self.out), entries, False, graphics)

    def artifact(self):
        return json.loads((self.out / 'game-build-artifact.json').read_text())

    def test_generic_pack_declares_no_graphics(self):
        self.build()
        artifact = self.artifact()
        self.assertNotIn('requiredFeatures', artifact)
        self.assertNotIn('extensions', artifact)
        self.assertFalse((self.out / pack.GRAPHICS_MANIFEST).exists())

    def test_direct_pack_declares_feature_and_packs_descriptor(self):
        path = self.write_descriptor()
        self.build(path)
        artifact = self.artifact()
        self.assertEqual(artifact['requiredFeatures'], [pack.GRAPHICS_FEATURE])
        extension = artifact['extensions'][pack.GRAPHICS_FEATURE]
        packed = (self.out / pack.GRAPHICS_MANIFEST).read_bytes()
        self.assertEqual(extension['manifest'], pack.GRAPHICS_MANIFEST)
        self.assertEqual(extension['sha256'], hashlib.sha256(packed).hexdigest())
        self.assertEqual(extension['bytes'], len(packed))
        self.assertEqual(packed, path.read_bytes())
        # descriptor 必须在 index.tsv 里（安装器按 index 校验每个文件）
        line = next(x for x in (self.out / 'index.tsv').read_text().splitlines()
                    if x.startswith(pack.GRAPHICS_MANIFEST + '\t'))
        self.assertEqual(line.split('\t')[2], extension['sha256'])
        # 包本身仍然自洽：contentRoot 是 index 的哈希
        self.assertEqual(artifact['contentRoot'],
                         'sha256-' + hashlib.sha256((self.out / 'index.tsv').read_bytes()).hexdigest())

    def test_descriptor_title_must_match_the_packaged_title(self):
        path = self.write_descriptor(descriptor(titleId='0100000000000001'))
        with self.assertRaises(SystemExit):
            self.build(path)
        self.assertFalse((self.out / 'manifest.json').exists(), 'a rejected pack must not look complete')

    def test_descriptor_field_rules(self):
        bad = [
            descriptor(schema='s2a-graphics-v2'),
            descriptor(directABI=2),
            descriptor(frontend='vulkan'),
            descriptor(backend='metal-direct'),
            descriptor(runtimeCompilerABI='s2a-direct-compile-v2'),
            descriptor(gameContentRoot='sha256-xyz'),
            descriptor(moduleSetDigest='a' * 64),
            descriptor(translatorClosure=''),
            descriptor(profiles=[{'profileHash': 'sha256-' + 'c' * 64}]),
            descriptor(profiles=[{'profileHash': 'sha256-' + 'c' * 64}] * 9),
            descriptor(profiles=[{}]),
            descriptor(sourceParserVersion=0),
            descriptor(titleId='0100000000ABCDE'),
        ]
        for doc in bad:
            with self.subTest(doc=doc):
                path = self.write_descriptor(doc)
                with self.assertRaises(SystemExit):
                    pack.load_graphics(str(path))
        missing = descriptor()
        del missing['files']
        path = self.write_descriptor(missing)
        with self.assertRaises(SystemExit):
            pack.load_graphics(str(path))

    def test_packaged_descriptor_is_accepted_by_the_native_consumer(self):
        """出包器写出的 descriptor 必须被 native 严格消费端接受（生产端→消费端对拍）。"""
        if not CC:
            self.skipTest('no C compiler; native consumer check NOT RUN')
        binary = self.base / 'direct_graphics_test'
        subprocess.run([CC, '-O1', '-Wall', '-Wextra', '-Werror', *INCLUDES, *CONSUMER_SOURCES,
                        '-o', str(binary)], check=True)
        self.build(self.write_descriptor())
        # 包里的那份（不是输入文件）：消费者只认包内路径
        consumer = subprocess.run([str(binary), str(self.out / pack.GRAPHICS_MANIFEST)],
                                  capture_output=True, text=True)
        self.assertEqual(consumer.returncode, 0, consumer.stdout + consumer.stderr)
        self.assertIn('direct graphics file: ok', consumer.stdout)
        self.assertIn(f'title={TITLE}', consumer.stdout)
        self.assertIn('abi=1', consumer.stdout)

    def test_write_pack_rejects_bad_modules_before_removing_old_output(self):
        """main 的替换前准入：坏模块不能先删旧包；后验校验不等价。"""
        self.out.mkdir()
        (self.out / 'manifest.json').write_text('{"version": 2}')
        (self.out / 'index.tsv').write_text('old index\n')
        before = {p.name: p.read_bytes() for p in self.out.iterdir()}
        staging = self.base / 'bad-modules'
        staging.mkdir()
        (staging / 'main.patched.bin').write_bytes(b'not empty')
        (staging / 'main.fixups.tsv').write_text('# TARGET\tapple\n# REWRITE_ABI\tandroid-sp-safe-v1\n')
        (staging / 'main.gates.tsv').write_text('# fixture\n')
        with self.assertRaises(SystemExit):
            pack.write_pack(str(self.out), [('modules', str(staging))])
        self.assertEqual({p.name: p.read_bytes() for p in self.out.iterdir()}, before)
        empty = self.base / 'empty-modules'
        empty.mkdir()
        with self.assertRaises(SystemExit):
            pack.write_pack(str(self.out), [('modules', str(empty))])
        self.assertEqual({p.name: p.read_bytes() for p in self.out.iterdir()}, before)

    def test_artifact_modules_survive_glob_metachars_in_out_path(self):
        """出包目录名带 [ ] 等 glob 元字符时，工件 modules 不许静默落空：
        glob.glob(join(out, …)) 会把目录名当字符类（qa-169163 惊奇包 8E 导入前红）。"""
        self.out = self.base / '[meta] out' / 'package'
        self.build()
        artifact = self.artifact()
        self.assertEqual([m['name'] for m in artifact['modules']], ['main'])

    def test_cli_refuses_a_root_that_already_has_the_descriptor(self):
        (self.root / pack.GRAPHICS_MANIFEST).parent.mkdir(parents=True)
        (self.root / pack.GRAPHICS_MANIFEST).write_text('{}')
        path = self.write_descriptor()
        argv = ['make_pack.py', str(self.out), '--root', str(self.root), '--direct-graphics', str(path)]
        with patch.object(sys, 'argv', argv):
            with self.assertRaises(SystemExit):
                pack.main()


if __name__ == '__main__':
    unittest.main()
