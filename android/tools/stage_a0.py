#!/usr/bin/env python3
"""Stage an Android package into the A0 installed layout (development bridge).

    stage_a0.py <game-root> [--force]

<game-root> is the directory that will hold the game, normally
`files/games/<titleId>`. It currently contains a package produced by
`android/tools/make_pack.py` (manifest.json, index.tsv, modules/,
exefs/, romfs/, ctl/). This tool copies it into
`revisions/<contentRoot>/`, writes `game-build-artifact.json`, then writes
`current.json` last.

This is not the production importer. yyan160's A4 import owns the real
transaction, per-game APK and rollback; this adapter exists so the runtime side
(A0 BootSpec/BootContract) can be exercised before that lands.
"""
import argparse
import datetime as _dt
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
from module_order import load_order   # noqa: E402  Horizon 装载序（main → subsdk0..N → sdk）：排序规则只有这一份

ABI = "android-sp-safe-v1"
HEX = set("0123456789abcdef")


def fail(message):
    sys.exit(f"stage_a0: {message}")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(4 << 20), b""):
            h.update(block)
    return h.hexdigest()


def read_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError) as e:
        fail(f"cannot read {path}: {e}")


def read_nacp(path):
    try:
        data = open(path, "rb").read()
    except OSError as e:
        fail(f"cannot read {path}: {e}")
    if len(data) < 0x3070:
        fail(f"{path} is too short ({len(data)} bytes)")
    name = data[:0x300].split(b"\0", 1)[0].decode("utf-8", "replace").strip()
    title = int.from_bytes(data[0x3038:0x3040], "little")
    version = data[0x3060:0x3070].split(b"\0", 1)[0].decode("utf-8", "replace").strip()
    if title == 0:
        fail(f"{path} has no application id")
    return f"{title:016X}", name or f"{title:016X}", version or "0"


def headers(path):
    out = {}
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                if not line.startswith("# "):
                    continue
                parts = line[2:].rstrip("\n").split("\t")
                if len(parts) >= 2:
                    out[parts[0]] = parts[1]
    except OSError as e:
        fail(f"cannot read {path}: {e}")
    return out


def module_list(root):
    # artifact 的 modules 顺序 = 装载顺序（经 BootContract → load_modules_from_source 到 loader.c 全局符号解析），
    # 必须按 Horizon 序：main → subsdk0..N（数字序）→ sdk；字母序会让 subsdk 里和 sdk 重名的符号错绑到 sdk。
    names = load_order(p.name[: -len(".patched.bin")]
                       for p in Path(root, "modules").glob("*.patched.bin"))
    modules = []
    for name in names:
        fixups = os.path.join(root, "modules", name + ".fixups.tsv")
        gates = os.path.join(root, "modules", name + ".gates.tsv")
        widen = os.path.join(root, "modules", name + ".widen.tsv")
        for need, what in ((fixups, "fixups"), (gates, "gates")):
            if not os.path.isfile(need):
                fail(f"module {name} has no {what}: {need}")
        h = headers(fixups)
        if h.get("TARGET") != "android" or h.get("REWRITE_ABI") != ABI:
            fail(f"module {name} is not {ABI} (TARGET={h.get('TARGET')}, REWRITE_ABI={h.get('REWRITE_ABI')})")
        build = h.get("BUILD_ID", "")
        if build and (len(build) not in (32, 40) or any(c not in HEX for c in build.lower())):
            fail(f"module {name} has an invalid BUILD_ID: {build}")
        m = {
            "name": name,
            "source": "revision",
            "image": f"modules/{name}.patched.bin",
            "fixups": f"modules/{name}.fixups.tsv",
            "gates": f"modules/{name}.gates.tsv",
            "target": "android",
        }
        if os.path.isfile(widen):
            m["widen"] = f"modules/{name}.widen.tsv"
        if build:
            m["buildId"] = build.lower()
        modules.append(m)
    if not any(m["name"] == "main" for m in modules):
        fail("no modules/main.patched.bin")
    return modules


def package_files(root):
    """Every file the runtime may open, relative to the game root."""
    out = []
    for d, _, fs in os.walk(root):
        for f in fs:
            rel = os.path.relpath(os.path.join(d, f), root)
            if rel in ("current.json", "current.json.tmp", "game-build-artifact.json"):
                continue
            if rel == "revisions" or rel.startswith("revisions/"):
                continue
            out.append(rel)
    return sorted(out)


def copy_into(root, revision, force):
    if os.path.exists(revision):
        if not force:
            fail(f"{revision} exists; pass --force to replace it")
        shutil.rmtree(revision)
    os.makedirs(revision)
    for rel in package_files(root):
        src = os.path.join(root, rel)
        dst = os.path.join(revision, rel)
        parent = os.path.dirname(dst)
        if parent:
            os.makedirs(parent, exist_ok=True)
        # Independent copy: a hard link would let a later source edit change an activated revision.
        shutil.copy2(src, dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("game_root")
    ap.add_argument("--force", action="store_true", help="replace an existing revision")
    args = ap.parse_args()
    root = os.path.abspath(args.game_root)
    if not os.path.isdir(root):
        fail(f"{root} is not a directory")

    manifest_path = os.path.join(root, "manifest.json")
    index_path = os.path.join(root, "index.tsv")
    manifest = read_json(manifest_path)
    if manifest.get("version") != 2:
        fail("manifest.json is not version 2")
    try:
        index = open(index_path, "rb").read()
    except OSError as e:
        fail(f"cannot read {index_path}: {e}")
    index_sha = hashlib.sha256(index).hexdigest()
    if manifest.get("index_sha256") != index_sha:
        fail("index.tsv does not match manifest.index_sha256")
    rows = [line.split("\t") for line in index.decode("utf-8").splitlines() if line]
    if len(rows) != manifest.get("files") or sum(int(r[1]) for r in rows) != manifest.get("total"):
        fail("manifest file count / total does not match index.tsv")
    for rel, size, sha in rows:
        if rel.startswith("/") or ".." in rel.split("/") or "\\" in rel:
            fail(f"unsafe path in index.tsv: {rel}")
        p = os.path.join(root, rel)
        if not os.path.isfile(p) or os.path.getsize(p) != int(size) or sha256(p) != sha:
            fail(f"package file is missing or does not match its SHA-256: {rel}")

    title_id, name, version = read_nacp(os.path.join(root, "ctl", "control.nacp"))
    if os.path.basename(root) != title_id:
        fail(f"game root is {os.path.basename(root)}, expected titleId {title_id}")
    modules = module_list(root)
    content_root = "sha256-" + index_sha
    revision_rel = "revisions/" + content_root
    revision = os.path.join(root, revision_rel)
    copy_into(root, revision, args.force)

    now = _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
    artifact = {
        "version": 1,
        "titleId": title_id,
        "contentRoot": content_root,
        "target": "android",
        "guestAbi": "aarch64",
        "rewriteAbi": ABI,
        "runtime": {"version": "0.1.0", "backend": "vulkan"},
        "package": {
            "manifest": "manifest.json",
            "indexSha256": index_sha,
            "files": manifest["files"],
            "bytes": manifest["total"],
        },
        "npdm": "exefs/main.npdm",
        "control": "ctl/control.nacp",
        "romfs": "romfs",
        "modules": modules,
        "createdAt": now,
        "toolchain": {"rewriter": "runtime/rewriter", "packer": "tools/stage_a0.py"},
    }
    for key, rel in (("systemData", "systemdata"), ("shaders", "shaders")):
        if os.path.isdir(os.path.join(revision, rel)):
            artifact[key] = rel
    if os.path.isfile(os.path.join(revision, "aoc.tsv")) and os.path.isdir(os.path.join(revision, "aoc")):
        artifact["aoc"] = {"index": "aoc.tsv", "root": "aoc"}

    artifact_path = os.path.join(revision, "game-build-artifact.json")
    with open(artifact_path, "w", encoding="utf-8") as f:
        json.dump(artifact, f, ensure_ascii=False, indent=1)
        f.write("\n")
    artifact_sha = sha256(artifact_path)

    current = {
        "version": 1,
        "titleId": title_id,
        "contentRoot": content_root,
        "revisionDir": revision_rel,
        "artifact": "game-build-artifact.json",
        "artifactSha256": artifact_sha,
        "activatedAt": now,
        "previousContentRoot": None,
    }
    tmp = os.path.join(root, "current.json.tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(current, f, ensure_ascii=False, indent=1)
        f.write("\n")
    os.replace(tmp, os.path.join(root, "current.json"))
    print(f"staged {name} {version} [{title_id}] -> {revision_rel} ({len(modules)} module(s), {manifest['files']} files)")


if __name__ == "__main__":
    main()
