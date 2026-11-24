#!/usr/bin/env python3
"""普通 VS/FS 的真实请求 → 编译 → 调试采集 → 再编译 → 资源根种子。"""
import base64
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from direct_batch_test import built_tools

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'android/tools'))
import direct_library
import vk_warm_nowindow as companion
import vk_warm_seed as seed


class WarmNowindowTest(unittest.TestCase):
    def test_real_compiler_and_package(self):
        tests, cli, identity = built_tools()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            capture = root / 'recording'
            capture.mkdir()
            baseline = root / 'original'
            baseline.mkdir()
            inputs = root / 'inputs'
            subprocess.run(['dotnet', tests, '--emit-requests', inputs], check=True)
            example = json.loads((inputs / 'graphics.json').read_bytes())
            for stage in example['program']['stages']:
                (inputs / (stage['stage'] + '.bin')).write_bytes(base64.b64decode(stage['code']))
            # The C replay path creates the profile-bound identity and original request.
            native = ROOT / 'android/tests/nvk_shader_pool_test.py'
            env = dict(os.environ, S2A_PW_TEST_DIR=str(capture))
            args = [inputs / 'profile.json', identity['translatorClosure'], inputs]
            request = root / 'request.json'
            subprocess.run([sys.executable, native, '--emit-nowindow', *args, request], env=env, check=True)
            req = direct_library.validate_request(request.read_bytes(), 'foreground request')
            self.assertTrue(req['program']['graphicsState']['hasUnalignedStorageBuffer'])
            compiled = root / 'compiled'
            subprocess.run(['dotnet', cli, 'compile', '--request', request, '--output', compiled], check=True)
            payload = (compiled / 'program.json').read_bytes()
            subprocess.run([sys.executable, native, '--execute-nowindow', *args, compiled / 'program.json'],
                           env=env, check=True)
            # 补件各臂（关 / 开 / 别的 profile / 身份被改 / 摘要被改 / 文件缺失）：自带临时目录，不碰 capture 目录
            arms = root / 'arms'
            arms.mkdir()
            env_arms = {k: v for k, v in env.items() if k != 'S2A_PW_TEST_DIR'}
            env_arms['TMPDIR'] = str(arms)
            subprocess.run([sys.executable, native, '--nowindow-arms', *args, compiled / 'program.json'],
                           env=env_arms, check=True)
            (capsule,) = (capture / 'warm-nowindow-capture').glob('*.request')
            (manifest,) = capture.glob('programs-v1-*.bin')
            original = manifest.read_bytes()
            ident = bytes.fromhex(capsule.stem)
            self.assertEqual(original[seed.PROG_HDR:seed.PROG_HDR+32], ident)
            # 使用原生写入器的原件；第 62 字节保留，不能作为是否需要补件的依据。
            self.assertEqual(original[seed.PROG_HDR+62], 0)
            (baseline / manifest.name).write_bytes(original)
            # The two fixture descriptors differ at the real clip_neg1 byte.
            # GPU object creation and guest-memory assertions belong to the Vulkan test.
            off_desc = bytearray(seed.PIPE_DESC)
            off_desc[0] = 3
            off_record = ident + off_desc + struct.pack('<I', 3)
            before = bytearray(off_record)
            before[32+349] = 1
            (baseline / seed.PIPE_NAME).write_bytes(seed.pipe_bytes([before]))
            (capture / seed.PIPE_NAME).write_bytes(seed.pipe_bytes([off_record]))
            identity_path = root / 'identity.json'
            identity_path.write_text(json.dumps(identity))
            source = root / 'source'
            print(companion.build(baseline, capture, source, cli, identity_path))
            packaged = root / 'packaged'
            (cohort,) = seed.build(packaged, [source])
            final = packaged / cohort
            self.assertEqual((final / manifest.name).read_bytes(), original)
            self.assertEqual(seed.pipe_check((final / seed.PIPE_NAME).read_bytes()), [before])
            self.assertEqual(seed.pipe_check((final / 'nowindow' / seed.PIPE_NAME).read_bytes()), [off_record])
            packed, _ = direct_library.decode_container(
                (final / 'nowindow' / (ident.hex() + '.program')).read_bytes(), ident.hex(), 'warm material')
            self.assertEqual(json.loads(packed)['contentHash'], json.loads(payload)['contentHash'])
            self.assertEqual(len(list((final / 'nowindow').iterdir())), 2)
            self.assertFalse(list(packaged.rglob('*.request')))
            # 负控：保持同一原生清单、请求与管线，只把基准候选集合退回旧的两字节选择。
            name, raw, records, pipes = companion.manifests(baseline)
            legacy_records = {key: record for key, record in records.items()
                              if record[0][61:63] == bytes((2, 1))}
            self.assertIn(ident, records)
            self.assertNotIn(ident, legacy_records)
            legacy_source = root / 'legacy-selector'
            with patch.object(companion, 'manifests', side_effect=[
                    (name, raw, legacy_records, pipes), companion.manifests(capture)]):
                with self.assertRaisesRegex(ValueError, '无可封装的同局非对齐程序'):
                    companion.build(baseline, capture, legacy_source, cli, identity_path)
            self.assertFalse(legacy_source.exists())
            stored = next((source / 'nowindow').glob('*.program'))
            bad = bytearray(stored.read_bytes())
            bad[-1] ^= 1
            stored.write_bytes(bad)
            with self.assertRaisesRegex(ValueError, '摘要不符'):
                seed.build(root / 'invalid', [source])
            record = bytearray(capsule.read_bytes())
            record[-1] ^= 1
            capsule.write_bytes(record)
            with self.assertRaisesRegex(ValueError, '摘要不符'):
                companion.build(baseline, capture, root / 'bad', cli, identity_path)
            self.assertFalse((root / 'bad').exists())


if __name__ == '__main__':
    unittest.main()
