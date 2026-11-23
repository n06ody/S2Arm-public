#!/usr/bin/env python3
"""在 lavapipe 产生真实请求，宿主重编后原样返回。每个对照臂使用独立进程和空私有目录。"""
import argparse
import base64
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from direct_batch_test import built_tools

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'android/tools'))
import vk_warm_nowindow as companion
import vk_warm_seed as seed


def run(runner):
    build_root = ROOT / 'build'
    build_root.mkdir(exist_ok=True)
    # 留存本次请求、清单及逐臂校验日志，便于定位断言。
    root = Path(tempfile.mkdtemp(prefix='warm-nowindow-vk-', dir=build_root))
    print(f'Vulkan 夹具证据 {root}', flush=True)
    def gpu(phase):
        args = ['--warm-nowindow-proof', str(root.relative_to(ROOT)), phase]
        subprocess.run(['bash', *runner, *args], cwd=ROOT, check=True)

    tests, cli, identity = built_tools()
    inputs = root / 'source'
    subprocess.run(['dotnet', tests, '--emit-requests', inputs], check=True)
    example = json.loads((inputs / 'graphics.json').read_bytes())
    fs, = [s for s in example['program']['stages'] if s['stage'] == 'fs']
    (root / 'fs.bin').write_bytes(base64.b64decode(fs['code'], validate=True))
    shutil.copyfile(inputs / 'profile.json', root / 'profile.json')
    (root / 'closure.txt').write_text(identity['translatorClosure'])
    identity_file = root / 'identity.json'
    identity_file.write_text(json.dumps(identity))
    subprocess.run(['bash', ROOT / 'graphics/tools/direct/tests/t_dsh_vk.sh'], check=True,
                   env=dict(os.environ, S2A_DSH_VK_EMIT_DIR=str(root / 'stages')))
    gpu('request')
    req = json.loads((root / 'request.json').read_bytes())
    assert req['program']['graphicsState']['hasUnalignedStorageBuffer'] is True
    subprocess.run(['dotnet', cli, 'compile', '--request', root / 'request.json',
                    '--output', root / 'compiled'], check=True)
    gpu('original')
    gpu('reference')
    source = root / 'cohort'
    print(companion.build(root / 'original', root / 'reference', source, cli, identity_file))
    staged = root / 'lp-original/shaders/direct/warm-vk'
    (key,) = seed.build(staged, [source])
    gpu('disabled')
    gpu('enabled')
    alt = staged / key / 'nowindow' / seed.PIPE_NAME
    rows = [bytearray(e) for e in seed.pipe_check(alt.read_bytes())]
    assert len(rows) == 1
    rows[0][32+349] ^= 1
    alt.write_bytes(seed.pipe_bytes(rows))
    gpu('wrong-pipe')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--parity-runner', type=Path)
    args = parser.parse_args()
    run([args.parity_runner, ROOT] if args.parity_runner else
        [ROOT / 'tests/vulkan/run.sh', 'android/tests/vk/run.sh'])
