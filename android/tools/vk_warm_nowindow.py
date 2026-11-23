#!/usr/bin/env python3
"""从一次关窗冷局录制，重编普通程序并配对当局管线。输出作为 S2A_VK_WARM_SOURCES 的一个来源。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

import direct_batch
import direct_library as library
import vk_warm_seed as seed


def digest(data):
    return hashlib.sha256(data).digest()


def recording(path, seed_header, decoder):
    name = path.stem
    if (not re.fullmatch('[0-9a-f]{64}', name) or path.is_symlink() or not path.is_file()
            or path.stat().st_size > 76 + 16384 + library.MAX_REQUEST):
        raise ValueError(f'{path.name}: 非法请求文件')
    raw = path.read_bytes()
    if len(raw) < 76 or raw[:8] != b'S2AWREQ1':
        raise ValueError(f'{name}: 请求头损坏')
    size, = struct.unpack_from('<I', raw, 8)
    preimage, body = raw[76:76+size], raw[76+size:]
    if (size > 16384 or not body or not preimage.startswith(b'S2AI1') or len(preimage) < 14
            or digest(preimage).hex() != name or digest(raw[:44] + raw[76:]) != raw[44:76]):
        raise ValueError(f'{name}: 请求或身份摘要不符')
    subprocess.run([decoder, "--check-canonical"], input=preimage, check=True)
    req = library.validate_request(body, name)
    abi, length = struct.unpack_from('<II', preimage, 6)
    closure = req['_translatorClosure'].encode()
    at = 14 + len(closure)
    if (preimage[5] not in (3, 4, 5) or length != len(closure) or preimage[14:at] != closure
            or digest(closure)[:8] != seed_header[52:60] or abi != req['_directABI']
            or preimage[at:at+32] != seed_header[16:48] or struct.pack('<I', abi) != seed_header[48:52]):
        raise ValueError(f'{name}: 身份原文与请求或种子的闭包、ABI、profile 不符')
    # compile_request.c embeds the provider's bytes verbatim, including any
    # leading/trailing whitespace. A parsed/reserialized profile has a different SHA.
    _, marker, fields = body.partition(b'"profile":')
    raw_profile, end, _ = fields.partition(b',"program":')
    if not marker or not end or digest(raw_profile) != seed_header[16:48]:
        raise ValueError(f'{name}: 请求中的完整 profile 不符')
    return req, body, raw[12:44].hex()


def manifests(folder):
    names = list(folder.glob('programs-v1-*.bin'))
    if len(names) != 1:
        raise ValueError(f'{folder}: 需要单一来源的程序清单')
    name = names[0].name
    program = names[0].read_bytes()
    records = {}
    seed.prog_check(name, program, records)
    pipes = seed.pipe_check((folder / seed.PIPE_NAME).read_bytes())
    return name, program, records, [e for e in pipes if e[:32] in records]


def same_program(base, off):
    a, ac1 = base
    b, bc1 = off
    # 比对身份、阶段、键、c1 状态/长度与常量原文；库来源、使用次数和顺序是每局的观测值。
    return a[:61] == b[:61] and a[64:76] == b[64:76] and ac1 == bc1


def stage_companion(source, destination, name, original, original_pipes):
    root = Path(source)
    receipt = json.loads((root / 'index.json').read_bytes())
    if (receipt.get('schema') != 's2a-vk-warm-nowindow-v1' or not original_pipes
            or receipt['programSeed'] != digest(original).hex()
            or receipt['pipeSeed'] != digest(original_pipes).hex()):
        raise ValueError('非窗口补件与原种子不配对')
    _, _, ids = seed.prog_check(name, original)
    files = receipt['files']
    if not isinstance(files, dict) or seed.PIPE_NAME not in files or len(files) < 2:
        raise ValueError('非窗口补件缺程序或管线')
    if set(os.listdir(root)) != set(files) | {'index.json'}:
        raise ValueError('非窗口补件文件集不完整')
    # Verify a whole bundle before publishing any of it. The receipt stays with
    # the host source; the player only needs recipes and immutable programs.
    for filename, expected in files.items():
        if filename != seed.PIPE_NAME and not re.fullmatch('[0-9a-f]{64}\\.program', filename):
            raise ValueError('补件文件名不合法')
        path = root / filename
        if path.is_symlink() or not path.is_file():
            raise ValueError('补件必须为普通文件')
        raw = path.read_bytes()
        if digest(raw).hex() != expected:
            raise ValueError(f'{filename}: 补件摘要不符')
        if filename != seed.PIPE_NAME:
            if bytes.fromhex(path.stem) not in ids:
                raise ValueError('补件程序不在原种子')
            payload, _ = library.decode_container(raw, path.stem, filename)
            result = library.validate_result(payload, filename)
            ident = result['content']['identity']
            if (struct.pack('<I', ident['directABI']) != original[48:52]
                    or digest(ident['translatorClosure'].encode())[:8] != original[52:60]
                    or ident['profileHash'] != receipt['profileHash']):
                raise ValueError(f'{filename}: 结果与种子档不符')
    pipes = seed.pipe_check((root / seed.PIPE_NAME).read_bytes())
    pipe_ids = {e[:32].hex() for e in pipes}
    if not pipe_ids <= {i.hex() for i in ids} or not {p[:-8] for p in files if p.endswith('.program')} <= pipe_ids:
        raise ValueError('补件管线与程序身份不配对')
    Path(destination).mkdir()
    for filename in files:
        shutil.copyfile(root / filename, Path(destination) / filename)


def build(base, capture, output, cli, compiler_identity):
    if os.path.lexists(output):
        raise ValueError('输出已存在，请指定新目录')
    name, original, programs, before = manifests(base)
    off_name, off, observed, after = manifests(capture)
    if off_name != name or original[16:60] != off[16:60]:
        raise ValueError('采集与原种子的完整 profile、ABI 或闭包不符')
    command = direct_batch.cli_command(cli)
    closure = direct_batch.load_identity(compiler_identity)['closure']
    wanted = {i for i, (r, _) in programs.items() if r[61:63] == bytes([2, 1])}
    seen_pipes = {e[:32] for e in after}
    accepted = set()
    cohort = None
    deferred = 0
    with tempfile.TemporaryDirectory(dir=output.parent, prefix='.warm-build-') as tmp:
        scratch = Path(tmp)
        donor = scratch / 'donor'
        alternate = donor / 'nowindow'
        alternate.mkdir(parents=True)
        (scratch / 'results').mkdir()
        decoder = scratch / 'direct_identity_host'
        subprocess.run(['bash', Path(__file__).with_name('build_direct_identity_host.sh'), decoder], check=True)
        for n, path in enumerate(sorted((capture / 'warm-nowindow-capture').glob('*.request'))):
            req, body, content_hash = recording(path, original, decoder)
            current = (req['session'], req['generation'], req['_profileHash'])
            if (cohort is not None and current != cohort) or req['_translatorClosure'] != closure:
                raise ValueError('录制混局或编译器闭包不符')
            cohort = current
            identity = bytes.fromhex(path.stem)
            if identity not in wanted or identity not in seen_pipes or identity not in observed:
                continue
            if not same_program(programs[identity], observed[identity]):
                raise ValueError(f'{path.stem}: 同身份的阶段或 c1 原文不符')
            if req['_kind'] != 'graphics' or req['program']['graphicsState']['hasUnalignedStorageBuffer'] is not True:
                raise ValueError(f'{path.stem}: 缺少真实非对齐绘制事实')
            request = scratch / 'request.json'
            request.write_bytes(body)
            result = direct_batch.compile_one(command, {'index': n, 'request': request}, scratch)
            if result['status'] == 'deferred':
                deferred += 1
                continue
            if result['status'] != 'compiled':
                raise ValueError(f'{path.stem}: 宿主翻译失败 {result["code"]}')
            payload = result['result'].read_bytes()
            checked = library.validate_result(payload, path.stem, request=req)
            if checked['contentHash'] != 'sha256-' + content_hash:
                raise ValueError(f'{path.stem}: 宿主结果与真机普通编译结果不同')
            (alternate / (path.stem + '.program')).write_bytes(library.encode_container(path.stem, payload))
            accepted.add(identity)
        if not accepted:
            raise ValueError(f'交集为空（种子窗口身份 {len(wanted)}，延后 {deferred}）')
        replacement = [e for e in before if e[:32] not in accepted] + [e for e in after if e[:32] in accepted]
        alt_pipes = seed.pipe_bytes(replacement)
        seed.pipe_check(alt_pipes)
        (alternate / seed.PIPE_NAME).write_bytes(alt_pipes)
        (donor / name).write_bytes(original)
        base_pipes = seed.pipe_bytes(before)
        (donor / seed.PIPE_NAME).write_bytes(base_pipes)
        receipt = dict(schema='s2a-vk-warm-nowindow-v1', programSeed=digest(original).hex(),
                       pipeSeed=digest(base_pipes).hex(), profileHash=cohort[2],
                       files={p.name: digest(p.read_bytes()).hex() for p in sorted(alternate.iterdir())})
        (alternate / 'index.json').write_text(json.dumps(receipt, indent=2) + '\n')
        stage_companion(alternate, scratch / 'verified', name, original, base_pipes)
        donor.rename(output)
    return f'非窗口种子：窗口目标 {len(wanted)}，生成 {len(accepted)}，未覆盖 {len(wanted)-len(accepted)}，延后 {deferred}，替换后管线 {len(replacement)}'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ('seed', 'capture', 'out', 'cli', 'identity'):
        parser.add_argument('--' + arg, type=Path, required=True)
    args = parser.parse_args()
    print(build(args.seed, args.capture, args.out, args.cli, args.identity))
