#!/usr/bin/env python3
"""Reject selection, package framing and the runtime source identity on a synthetic export."""
from hashlib import sha256
import importlib.util
from pathlib import Path
import json
import os
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "graphics/tools/direct"))
from merge_extract import make_rec  # noqa: E402
from vk_request_material import build  # noqa: E402

CHAIN = "0123456789ab"
WIRE_HEADER = struct.Struct("<8sIIQQ32s")
DEVICE_SOURCE = r"""
#include <stdio.h>
#include <string.h>
#include "source_store.h"
#include "nvn_key.h"
#include "sha256.h"

int main(int argc, char **argv) {
    const uint32_t code_bytes = 0x180;
    uint8_t code[0x180], control[0x880];
    if (argc != 2) return 1;
    FILE *in = fopen(argv[1], "rb");
    if (!in) return 1;
    int read_ok = fread(code, 1, code_bytes, in) == code_bytes &&
                  fread(control, 1, sizeof control, in) == sizeof control;
    fclose(in);
    if (!read_ok) return 1;
    uint32_t nctl = nvn_key_ctl_len(control);
    uint8_t normalized[0x880], key[32], sha[32];
    memcpy(normalized, control, nctl);
    nvn_key_ctl_zero(normalized, nctl);
    s2a_sha256 ctx;
    s2a_sha256_init(&ctx);
    s2a_sha256_update(&ctx, normalized, nctl);
    s2a_sha256_update(&ctx, code, code_bytes);
    s2a_sha256_final(&ctx, key);
    uint32_t stored_code, stored_ctl;
    if (s2a_source_keep(key, 1, code, code_bytes, control, nctl) ||
        s2a_source_identity(key, sha, &stored_code, &stored_ctl)) return 2;
    printf("%u %u ", stored_code, stored_ctl);
    for (size_t i = 0; i < sizeof sha; ++i) printf("%02x", sha[i]);
    puts("");
    return 0;
}
"""


def decode(data):
    if len(data) < 64:
        return None
    magic, version, start, nindex, nblob, digest = WIRE_HEADER.unpack_from(data)
    if (magic, version, start) != (b"S2ARQMAT", 1, 64):
        return None
    if nindex + nblob != len(data) - start or sha256(data[start:]).digest() != digest:
        return None
    return json.loads(data[start:start + nindex]), data[start + nindex:]


def stage(n):
    code = bytes([n]) * 0x180
    code = bytes.fromhex("78563412") + code[4:]
    control = bytearray([n] * 0x880)
    struct.pack_into("<I", control, 0x20, 0x7d9)
    normalized = bytearray(control[:0x7d9])
    normalized[0x790:0x798] = bytes(8)
    normalized[0x7d0:0x7d9] = bytes(9)
    digest = sha256(normalized + code).hexdigest()
    return digest, code, control


def get_slice(blob, field):
    offset, length = field
    assert 0 <= offset <= len(blob) and 0 <= length <= len(blob) - offset
    return blob[offset:offset + length]


def check_resource_pack(root, original):
    spec = importlib.util.spec_from_file_location("request_material_make_pack", REPO / "android/tools/make_pack.py")
    pack = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pack)
    for path in ("exefs", "romfs", "ctl", "shaders/direct", "staging"):
        (root / path).mkdir(parents=True, exist_ok=True)
    (root / "exefs/main.npdm").write_bytes(b"synthetic")
    nacp = bytearray(0x4000)
    struct.pack_into("<Q", nacp, 0x3038, 0x0100000000001000)
    (root / "ctl/control.nacp").write_bytes(nacp)
    (root / "s2a_caps.tsv").write_text("plat\tandroid\n")
    material = "shaders/direct/request-material.bin"
    (root / material).write_bytes(original)
    # Android filtering happens before the real resource index hash and copy.
    staging = root / "staging"
    done = subprocess.CompletedProcess([], 0)
    with mock.patch.dict(os.environ, {"S2A_ANDROID_SKIP_APPLE_ARTIFACTS": "1"}), \
            mock.patch.object(pack, "stage_system_data", return_value=None), \
            mock.patch.object(pack, "rewrite"), \
            mock.patch.object(pack, "stage_address_requirements", return_value=str(root / "exefs/main.npdm")), \
            mock.patch.object(pack.subprocess, "run", return_value=done):
        entries = pack.entries_from_root(str(root), str(staging))
    (staging / "main.patched.bin").write_bytes(b"synthetic")
    (staging / "main.fixups.tsv").write_text(f"# TARGET\tandroid\n# REWRITE_ABI\t{pack.REWRITE_ABI}\n")
    (staging / "main.gates.tsv").touch()
    packed = root / "packed"
    pack.write_pack(str(packed), entries)
    assert (packed / material).read_bytes() == original
    rows = [line.split("\t") for line in (packed / "index.tsv").read_text().splitlines()]
    assert [r for r in rows if r[0] == material] == [[material, str(len(original)), sha256(original).hexdigest()]]


def main():
    with TemporaryDirectory(prefix="s2a-request-material-") as td:
        root = Path(td)
        entries = {name: stage(n) for n, name in enumerate(("vs", "vs_orphan", "fs", "cs", "gs"), 1)}
        digest = {label: fields[0] for label, fields in entries.items()}
        keys = {label: value[:16] for label, value in digest.items()}
        keys["missing"] = "0123456789abcdef"
        shader_map = root / "shader_map.tsv"
        rows = []
        for label, (identity, code, control) in entries.items():
            name = f'{"ps" if label == "fs" else label}_0.bin'
            (root/name).write_bytes(make_rec(code, control))
            rows.append(f"{name}\t0\t-\t{identity[:16]}\t-\n")
        rows.append(f"ps_0.bin\t100\t-\t{keys['missing']}\t-\n")
        shader_map.write_text("".join(rows))
        pairs = root / "shader_pairs.tsv"
        pairs.write_text(f"{keys['vs']}\t{keys['fs']}\n{keys['vs_orphan']}\t{keys['missing']}\n")
        refused = root / "vk_build.tsv"
        refused.write_text(
            f"{keys['vs']}\tvs\tok\t\n{keys['vs_orphan']}\tvs\tok\t\n"
            f"{keys['fs']}\tps\tfailed\trejected\n{keys['cs']}\tcs\tfailed\trejected\n"
            f"{keys['gs']}\tgs\tskipped\tno offline GS\n{keys['missing']}\tps\tfailed\tmissing\n"
            f"{'f'*16}\ths\tskipped\tunsupported\n")
        out = root / "request-material.bin"
        assert build(root, shader_map, pairs, refused, CHAIN, out) == (4, 3, 2, 1)
        original = out.read_bytes()
        index, blob = decode(original)
        assert index["chain"] == CHAIN
        assert {(r["stage"], r["key"]) for r in index["rejects"]} == {
            ("fs", keys["fs"]), ("fs", keys["missing"]), ("gs", keys["gs"]), ("cs", keys["cs"])}
        assert [r["source"] for r in index["rejects"] if r["key"] == keys["missing"]] == [None]
        assert {tuple(index["sources"][s]["stage"] for s in r["sources"]) for r in index["requests"]} == {
            ("vs", "fs"), ("cs",)}
        assert all("facts" not in r for r in index["requests"])
        assert len(index["sources"]) == 4 and keys["vs_orphan"] not in {r["key"] for r in index["sources"]}
        for src in index["sources"]:
            label = src["stage"]
            assert src["key"] == keys[label] and src["sourceSha"] == digest[label]
            assert get_slice(blob, src["code"]) == entries[label][1]
            assert get_slice(blob, src["control"]) == entries[label][2][:0x7d9]
        assert len(blob) == sum(sum(s[field][1] for field in ("code", "control")) for s in index["sources"])
        assert decode(original[:8] + struct.pack("<I", 2) + original[12:]) is None
        assert decode(original[:-1]) is None
        assert decode(original[:-1] + bytes([original[-1] ^ 1])) is None
        assert build(root, shader_map, pairs, refused, CHAIN, out) == (4, 3, 2, 1)
        assert out.read_bytes() == original
        found = next(s for s in index["sources"] if s["stage"] == "fs")

        raw = root / "source.raw"
        raw.write_bytes(entries["fs"][1] + entries["fs"][2])
        checker = root / "device_source.c"
        checker.write_text(DEVICE_SOURCE)
        direct = REPO / "android/native/direct"
        program = root / "device_source"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-pthread", "-I", str(direct),
                        "-I", str(REPO / "graphics/nvn/gfx/nvn"),
                        str(checker), str(direct/"source_store.c"), str(direct/"sha256.c"),
                        "-o", str(program)], check=True)
        got = subprocess.check_output([program, raw], text=True).split()
        assert got == [str(found["code"][1]), str(found["control"][1]), found["sourceSha"]], got
        refused.write_text(f"{keys['vs']}\tvs\tok\t\n")
        assert build(root, shader_map, pairs, refused, CHAIN, out) == (0, 0, 0, 0)
        assert not out.exists()
        check_resource_pack(root / "product", original)
    print("请求原料：4 拒收逐项对应，2 已知组合，3 源齐全，坏版本/短读/篡改拒绝，设备身份同一，安卓资源索引含原料原字节")


if __name__ == "__main__":
    main()
