#!/usr/bin/env python3
"""Publish optional Android compile inputs for the Vulkan library's rejected stages."""
from hashlib import sha256
import json
from mmap import ACCESS_READ, mmap
from pathlib import Path
import re
import struct
import sys
from tempfile import NamedTemporaryFile, TemporaryFile

from merge_extract import nvn_key, ns2, read_map, record_spans, split_rec

MAGIC = b"S2ARQMAT"
VERSION = 1
HEADER = struct.Struct("<8sIIQQ32s")
STAGES = {"vs": "vs", "ps": "fs", "fs": "fs", "gs": "gs", "cs": "cs"}
KEY = re.compile(r"[a-f0-9]{16}")


def read_rejections(table):
    rejected, unsupported = set(), 0
    for line in table.read_text(encoding="utf-8").split("\n"):
        if not line:
            continue
        key, stage, status, _ = line.split("\t", 3)
        if status == "ok":
            continue
        if status not in {"failed", "skipped"}:
            raise ValueError("vk_build.tsv 有未知的出库状态")
        if stage not in STAGES:
            unsupported += 1
        else:
            if not KEY.fullmatch(key):
                raise ValueError("vk_build.tsv 有无效的拒收键")
            rejected.add((STAGES[stage], key))
    return rejected, unsupported


def request_pairs(pairs, rejected):
    """Use the extracted VS/FS pairs. Never expand a rejected stage into guessed combinations."""
    requests = {((stage, key),) for stage, key in rejected if stage == "cs"}
    with pairs.open(encoding="utf-8") as rows:
        for row in rows:
            if row.startswith("#"):
                continue
            fields = row.split()
            if len(fields) < 2 or not all(KEY.fullmatch(key) for key in fields[:2]):
                continue
            pair = (("vs", fields[0]), ("fs", fields[1]))
            if rejected.intersection(pair):
                requests.add(pair)
    return sorted(requests)


def source_identity(record):
    code, control = split_rec(record)
    if not code or len(control) < 0x24 or ns2.is_nvn2(control):
        return None
    nctl = nvn_key.ctl_len(control)
    if len(control) < nctl:
        return None
    control = control[:nctl]
    identity = sha256(nvn_key.key_bytes(control))
    identity.update(code)
    return code, control, identity.hexdigest()


def collect_sources(package, shader_map, wanted, blob):
    locations = {}
    for row in read_map(shader_map)[1]:
        name, ordinal, _, key = row[:4]
        label = STAGES.get(name[:2]), key
        if label in wanted and name.endswith(".bin") and Path(name).name == name:
            locations.setdefault(label, (name, int(ordinal)))
    by_file = {}
    for label, (name, ordinal) in sorted(locations.items()):
        by_file.setdefault(name, {}).setdefault(ordinal, []).append(label)

    sources, refs = [], {}
    for name, wanted_records in sorted(by_file.items()):
        path = package / name
        if not path.is_file() or not path.stat().st_size:
            continue
        last_record = max(wanted_records)
        with path.open("rb") as raw, mmap(raw.fileno(), 0, access=ACCESS_READ) as data:
            for ordinal, (start, end) in enumerate(record_spans(data)):
                if ordinal > last_record:
                    break
                if ordinal not in wanted_records:
                    continue
                try:
                    item = source_identity(data[start:end])
                except (ValueError, struct.error):
                    continue
                if item is None:
                    continue
                code, control, identity = item
                for stage, key in wanted_records[ordinal]:
                    if not identity.startswith(key):
                        continue
                    code_offset = blob.tell()
                    blob.write(code)
                    control_offset = blob.tell()
                    blob.write(control)
                    refs[stage, key] = len(sources)
                    sources.append({"stage": stage, "key": key, "sourceSha": identity,
                                    "code": [code_offset, len(code)], "control": [control_offset, len(control)]})
    return sources, refs


def publish(path, index, blob, spans):
    index_data = json.dumps(index, separators=(",", ":")).encode("utf-8")
    body_hash = sha256(index_data)
    blob_bytes = sum(length for _, length in spans)
    with NamedTemporaryFile(dir=path.parent, delete=False) as temp:
        temporary = Path(temp.name)
        try:
            temp.seek(HEADER.size)
            temp.write(index_data)
            for offset, length in spans:
                blob.seek(offset)
                while length:
                    chunk = blob.read(min(length, 1 << 20))
                    if not chunk:
                        raise ValueError("请求原料暂存数据不完整")
                    temp.write(chunk)
                    body_hash.update(chunk)
                    length -= len(chunk)
            temp.seek(0)
            temp.write(HEADER.pack(MAGIC, VERSION, HEADER.size, len(index_data), blob_bytes, body_hash.digest()))
            temp.close()
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)


def build(package, shader_map, pairs, rejects, chain, output):
    if not re.fullmatch(r"[a-f0-9]{12}", chain):
        raise ValueError("请求原料的库链号无效")
    rejected, unsupported = read_rejections(rejects)
    if not rejected:
        output.unlink(missing_ok=True)
        return 0, 0, 0, unsupported

    requests = request_pairs(pairs, rejected)
    wanted = rejected.union(*requests)
    with TemporaryFile(dir=output.parent) as blob:
        sources, refs = collect_sources(package, shader_map, wanted, blob)
        complete = [request for request in requests if all(label in refs for label in request)]
        included = rejected.union(*complete).intersection(refs)
        sources_out, spans, out_refs = [], [], {}
        offset = 0
        for label in sorted(included):
            source = sources[refs[label]]
            code_start, code_bytes = source["code"]
            _, control_bytes = source["control"]
            out_refs[label] = len(sources_out)
            sources_out.append({**source, "code": [offset, code_bytes],
                                "control": [offset + code_bytes, control_bytes]})
            length = code_bytes + control_bytes
            spans.append((code_start, length))
            offset += length
        index = {"chain": chain, "sources": sources_out,
                 "rejects": [{"stage": stage, "key": key, "source": out_refs.get((stage, key))}
                             for stage, key in sorted(rejected)],
                 "requests": [{"sources": [out_refs[label] for label in request]} for request in complete]}
        publish(output, index, blob, spans)
    return len(rejected), sum(label in refs for label in rejected), len(index["requests"]), unsupported


def main(args):
    package, shader_map, pairs, rejects, chain, output = args
    n, ready, requests, unsupported = build(Path(package), Path(shader_map), Path(pairs), Path(rejects),
                                            chain, Path(output))
    print(f"请求原料 v{VERSION}：拒收 {n}，有源 {ready}，缺源 {n - ready}，已知组合 {requests}，范围外阶段 {unsupported}")


if __name__ == "__main__":
    main(sys.argv[1:])
