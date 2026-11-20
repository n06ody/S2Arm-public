"""Exercise production G05 CLI and G06 Execute using real LibHac directory saves.

The payload is public repository text, not game progress. No Context substitutes,
hand-encoded IMKV records, player data, keys or network access are used.
"""

import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import uuid
import warnings
import zipfile

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "pack/jobs"))
import layout
import saves

DOTNET, DRIVER, AOT_DRIVER, *space_faults = sys.argv[1:]
GAME_ID = "0100000000000000"
USER = "00000000000000010000000000000000"
OTHER_USER = "00000000000000020000000000000000"
PLAYER2 = "5a1e0b7c3d9f4e2a8b6c1d0e9f8a7b6c"   # 本地玩家（#623）的第二个玩家：用户号由宿主按名字算，这里取一个固定值


def write_json(path, value):
    path.write_text(json.dumps(value))
    path.chmod(0o600)


def snapshot(root, exported=False):
    def selected(p):
        name = p.relative_to(root).as_posix()
        return not exported or saves.allowed(name, directory=p.is_dir()) or (
            p.is_dir() and name in ("bis", "bis/user", "bis/system", "system"))
    return (
        sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_dir() and selected(p)),
        {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
         for p in root.rglob("*") if p.is_file() and selected(p)},
    )


def zip64(data, *, count=None, size=None, sentinels=False):
    end = len(data) - 22
    assert data[end:end + 4] == b"PK\x05\x06"
    _, _, _, _, n, length, offset, _ = struct.unpack_from("<4s4H2IH", data, end)
    record = struct.pack("<4sQ2H2I4Q", b"PK\x06\x06", 44, 45, 45, 0, 0,
                         n if count is None else count, n if count is None else count,
                         length if size is None else size, offset)
    tail = bytearray(data[end:])
    if sentinels:
        struct.pack_into("<2H2I", tail, 8, 0xffff, 0xffff, 0xffffffff, 0xffffffff)
    return data[:end] + record + struct.pack("<4sIQI", b"PK\x06\x07", 0, end, 1) + tail


with tempfile.TemporaryDirectory(prefix="s2arm-public-save-interop-") as temporary:
    scratch = Path(temporary).resolve()
    library = scratch / "library"
    private = scratch / "private"
    request_path = scratch / "request.json"
    config = dict(schema=1, privateRoot=str(private), engineDir=str(REPO),
                  library=str(library), developerDir="/Applications/Xcode.app/Contents/Developer")

    def python_call(operation, *, expected=None, **extra):
        write_json(request_path, config | dict(operation=operation, gameId=GAME_ID,
                                             taskId=uuid.uuid4().hex) | extra)
        process = subprocess.run([sys.executable, str(REPO / "pack/jobs/main.py"),
                                  "--request", str(request_path)], capture_output=True, text=True, timeout=60)
        events = [json.loads(line) for line in process.stdout.splitlines() if line.startswith("{")]
        results = [event for event in events if event.get("type") == "result"]
        assert results, (process.stdout, process.stderr)
        result = results[-1]
        if expected:
            codes = [d.get("code") for d in result.get("diagnostics", [])]
            assert process.returncode == 1 and expected in codes, (expected, result, process.stderr)
            return expected
        assert process.returncode == 0 and result["state"] == "succeeded", (result, process.stderr)
        return result["result"]

    config["libraryId"] = python_call("setup")["libraryId"]
    # A test application identity for public file payloads. The content digest is
    # computed from the actual repository file; this is never an imported game.
    game = dict(id=GAME_ID, name="Public-file LibHac interoperability", packages=[
        dict(type="base", titleId=GAME_ID, baseId=GAME_ID, version=1,
             sha256=hashlib.sha256((REPO / "README.md").read_bytes()).hexdigest(),
             path=str(REPO / "README.md"))])
    write_json(library / "catalog.json", dict(schema=1, games=[game]))
    identity = saves.identity(game)
    # 游戏库结构 v3：存档在这款的文件夹 games/<英文名>/saves 下，game.json 记游戏号（pack/jobs/layout.py）
    folder = library / "games" / layout.folder_name(game["name"], GAME_ID)
    folder.mkdir(parents=True)
    write_json(folder / layout.GAME_JSON, dict(id=GAME_ID))
    source = folder / "saves"
    subprocess.run([DOTNET, DRIVER, "--create", str(source), str(REPO), USER], check=True, timeout=60)
    assert next(source.glob("bis/user/save/*/0/nested/readme.md")).read_bytes() == (REPO / "README.md").read_bytes()
    payloads = list(source.glob("bis/user/save/*/0/contributing.md"))
    assert len(payloads) == 2 and all(p.read_bytes() == (REPO / "CONTRIBUTING.md").read_bytes() for p in payloads)
    assert len(list(source.glob("bis/user/save/*/0/empty"))) == 1
    expected = snapshot(source)
    exported = snapshot(source, exported=True)

    def csharp_call(operation, path=None, *, root=source, game_identity=identity,
                    expected_code=None, env=None, expected_stderr=(), forbidden_stderr=(), native_aot=False, **extra):
        request = dict(operation=operation, dataRoot=str(root), workRoot=str(scratch / "csharp-work"),
                       game=game_identity, path=str(path) if path else None) | extra
        write_json(request_path, request)
        command = ([AOT_DRIVER, "--request", str(request_path), "--require-aot"] if native_aot
                   else [DOTNET, DRIVER, "--request", str(request_path)])
        process = subprocess.run(command, capture_output=True, text=True, check=True, timeout=60,
                                 env=None if env is None else os.environ | env)
        response = json.loads(process.stdout)
        for line in expected_stderr:
            assert line in process.stderr, (line, response, process.stderr)
        for text in forbidden_stderr:
            assert text not in process.stderr, (text, response, process.stderr)
        if expected_code:
            assert not response["ok"] and response["error"]["code"] == expected_code, (response, process.stderr)
            return expected_code
        assert response["ok"], (response, process.stderr)
        return response["result"]

    python_archive, csharp_archive = scratch / "g05.s2asave", scratch / "g06.s2asave"
    python_call("saveBackup", path=str(python_archive))
    p = python_call("saveInspect", path=str(python_archive))
    c = csharp_call("inspect", python_archive)
    assert p["manifest"] == c["manifest"]
    assert {e["type"] for e in c["manifest"]["saveEntries"]} == {"account", "device"}
    assert c["manifest"]["users"]["ids"] == [USER]
    # G05 writes force_zip64=True local headers even for these small files.
    with zipfile.ZipFile(python_archive) as archive:
        first = archive.infolist()[0]
        local = python_archive.read_bytes()[first.header_offset:]
        assert struct.unpack_from("<2I", local, 18) == (0xffffffff, 0xffffffff)
    print("PASS G05 force_zip64 export -> G06 inspect; manifests, account/device ownership agree")

    for name, transformed in (
        ("zip64", zip64(python_archive.read_bytes())),
        ("zip64-sentinels", zip64(python_archive.read_bytes(), sentinels=True)),
    ):
        path = scratch / (name + ".s2asave")
        path.write_bytes(transformed)
        assert python_call("saveInspect", path=str(path))["manifest"] == csharp_call("inspect", path)["manifest"]
        print("PASS both accept " + name)

    target = scratch / "csharp-target"
    preview = csharp_call("inspect", python_archive, root=target)
    csharp_call("restore", python_archive, root=target, archiveSha256=preview["archiveSha256"],
                targetFingerprint=preview["targetFingerprint"])
    assert snapshot(target) == exported
    csharp_call("export", csharp_archive, root=target)
    preview = python_call("saveInspect", path=str(csharp_archive))
    assert preview["manifest"]["files"] == p["manifest"]["files"]
    assert preview["manifest"]["directories"] == p["manifest"]["directories"]
    assert preview["manifest"]["saveEntries"] == p["manifest"]["saveEntries"]
    # Real file change followed by Python's confirmed restore tests replacement
    # and preservation of non-exported saveMeta, not just archive readability.
    changed = next(source.glob("bis/user/save/*/0/nested/readme.md"))
    changed.write_bytes((REPO / "CONTRIBUTING.md").read_bytes())
    preview = python_call("saveInspect", path=str(csharp_archive))
    result = python_call("saveRestore", path=str(csharp_archive), **preview)
    assert result["committed"] and snapshot(source) == expected
    assert snapshot(target) == exported
    print(f"PASS G05 -> G06 restore/export -> G05 restore; {p['fileCount']} files, directories, SHA256 and owners match")

    def compare(path, code="save_invalid", *, root=source, **extra):
        assert python_call("saveInspect", path=str(path), expected=code, **extra) == csharp_call(
            "inspect", path, root=root, expected_code=code, **extra)

    # 维护者 11-17：版本 / DLC 组合、账号归属对不上不再拒绝手动恢复——两种实现都照常检查、各给一句提示（作业层 warning 诊断 + notice；
    #   服务层 warnings）；带 sync（云存档自动同步）两边照旧以同一个码拒绝
    def compare_warn(path, code, *, root=source):
        compare(path, code, root=root, sync=True)
        py = python_call("saveInspect", path=str(path))
        assert py["notice"], py
        assert len(csharp_call("inspect", path, root=root)["warnings"]) == 1

    def rewrite(name, mutate=None, extra=None, duplicate=False):
        path = scratch / (name + ".s2asave")
        with zipfile.ZipFile(python_archive) as src, zipfile.ZipFile(path, "w") as dst:
            for entry in src.infolist():
                content = src.read(entry)
                if entry.filename == "manifest.json" and mutate:
                    manifest = json.loads(content)
                    mutate(manifest)
                    content = json.dumps(manifest).encode()
                dst.writestr(entry.filename, content)
            if extra:
                dst.writestr(extra, (REPO / "README.md").read_bytes())
            if duplicate:
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore", UserWarning)
                    dst.writestr("manifest.json", src.read("manifest.json"))
        return path

    compare(rewrite("wrong-game", lambda m: m["game"].update(id="0100000000000001")), "save_wrong_game")
    compare_warn(rewrite("wrong-version", lambda m: m["game"].update(version=2)), "save_version")
    compare_warn(rewrite("wrong-content", lambda m: m["game"].update(contentIdentity="0" * 64)), "save_version")
    compare(rewrite("path-traversal", extra="payload/../escape"))
    compare(rewrite("duplicate-path", duplicate=True))
    print("PASS both implementations reject wrong game, traversal and duplicate paths with identical reason codes; version/content mismatch warns (sync rejects alike)")

    # 维护者 11-17：备份里个别存档文件 CRC / SHA 坏了不再整份拒绝。期望手写：坏一个用户存档文件 → 两种实现都照常检查、各给一句提示
    #   （作业层 notice 点名它；服务层 warnings 一条）；带 sync 两边照旧拒绝（save_invalid）；坏账号索引两边照旧拒绝
    def corrupt(name, pick):
        path = scratch / (name + ".s2asave")
        with zipfile.ZipFile(python_archive) as src, zipfile.ZipFile(path, "w") as dst:
            target = pick([e.filename for e in src.infolist()])
            for entry in src.infolist():
                content = src.read(entry)
                if entry.filename == target:
                    content = content[:-1] + bytes([content[-1] ^ 0xFF])
                dst.writestr(entry.filename, content)
        return path, target[len("payload/"):]
    bad, bad_name = corrupt("damaged-save", lambda names: next(n for n in names if n.startswith("payload/bis/user/save/") and n.endswith(".md")))
    compare(bad, "save_damaged", sync=True)
    assert bad_name in python_call("saveInspect", path=str(bad))["notice"]
    assert len(csharp_call("inspect", bad)["warnings"]) == 1
    compare(corrupt("damaged-index", lambda names: "payload/bis/system/save/8000000000000000/0/imkvdb.arc")[0])
    print("PASS both warn on one damaged save file (sync rejects alike) and reject a damaged account index")

    other = scratch / "other-user"
    subprocess.run([DOTNET, DRIVER, "--create", str(other), str(REPO), OTHER_USER], check=True, timeout=60)
    original = scratch / "original"
    shutil.move(source, original)
    shutil.copytree(other, source)
    compare_warn(python_archive, "save_wrong_user", root=other)
    shutil.rmtree(source)
    shutil.move(original, source)
    print("PASS both warn on independently API-created different account owner (sync rejects alike: save_wrong_user)")

    android_root = scratch / "android-state"
    shutil.copytree(source, android_root)
    previous = next(android_root.glob("bis/user/save/*/0/nested/readme.md"))
    previous.write_bytes(b"previous synthetic Android progress")
    old_android = snapshot(android_root)
    android_identity = identity | dict(version=0, contentIdentity="e" * 64)
    android = dict(root=android_root, game_identity=android_identity, androidRevisionIdentity=True,
                   unattendedRestore=True, workRoot=str(scratch / "android-work"))
    for archive, code, overrides in (
        (rewrite("android-wrong-title", lambda m: m["game"].update(id="0100000000000001")), "save_wrong_game", {}),
        (bad, "save_damaged", {}),
        (python_archive, "save_wrong_user", {"root": other}),
    ):
        csharp_call("inspect", archive, expected_code=code, **(android | overrides))
    preview = csharp_call("inspect", python_archive, **android)
    aot_preview = csharp_call("inspect", python_archive, native_aot=True, **android)
    assert aot_preview == preview and len(aot_preview["warnings"]) == 1, aot_preview
    print("PASS NativeAOT Android inspect accepts a valid archive and returns the version warning")
    for archive, options in ((bad, {}), (python_archive, {"root": other})):
        aot_warning = csharp_call("inspect", archive, native_aot=True, **options)
        assert len(aot_warning["warnings"]) == 1 and aot_warning == csharp_call("inspect", archive, **options)
    print("PASS NativeAOT inspect returns the actual damaged-file and different-owner warnings")
    confirmation = dict(archiveSha256=preview["archiveSha256"], targetFingerprint=preview["targetFingerprint"])
    if os.geteuid() != 0:
        lock_file = Path(android["workRoot"]) / "save-transfer.lock"
        original_mode = lock_file.stat().st_mode & 0o777
        lock_file.chmod(0)
        try:
            csharp_call("inspect", python_archive, expected_code="save_invalid",
                        expected_stderr=("code=save_invalid", "type=UnauthorizedAccessException", "SaveTransfer.Execute"),
                        forbidden_stderr=(str(scratch),), **android)
        finally:
            lock_file.chmod(original_mode)
        assert snapshot(android_root) == old_android
        print("PASS Android inspect reports denied transfer-lock open with exception site and no path")
    csharp_call("restore", python_archive, expected_code="save_changed",
                **(confirmation | {"targetFingerprint": "0" * 64}), **android)
    assert snapshot(android_root) == old_android
    csharp_call("restore", python_archive, expected_code="save_invalid",
                env={"SAVE_TRANSFER_FAIL_AT": "oldMoved"}, **confirmation, **android)
    assert snapshot(android_root) == old_android
    no_mounts = {"DYLD_INSERT_LIBRARIES": space_faults[0]} if space_faults else None
    result = csharp_call("restore", python_archive, env=no_mounts, **confirmation, **android)
    assert result["committed"] and snapshot(android_root, exported=True) == exported
    if no_mounts:
        print("PASS Android restore commits with mount enumeration denied")
    backup = Path(result["previousBackup"])
    with zipfile.ZipFile(backup) as saved:
        assert saved.read("payload/" + previous.relative_to(android_root).as_posix()) == b"previous synthetic Android progress"
        assert json.loads(saved.read("manifest.json"))["platform"] == "android"
    csharp_call("recover", **android)
    assert backup.is_file()
    print("PASS Android restore shares G06 verification: title, owner, damage, stale preview, rollback, backup")

    if no_mounts:
        preview = csharp_call("inspect", python_archive, **android)
        confirmation = dict(archiveSha256=preview["archiveSha256"], targetFingerprint=preview["targetFingerprint"])
        before = snapshot(android_root), snapshot(Path(android["workRoot"]))
        for mode, code, log in (
            ("denied", "save_space_unknown", ("fixture: statfs denied", "UnauthorizedAccessException")),
            ("full", "save_space", ("fixture: statfs no available blocks",)),
        ):
            csharp_call("restore", python_archive, expected_code=code, expected_stderr=log,
                        env=no_mounts | {"S2A_TEST_SPACE_PATH": android["workRoot"], "S2A_TEST_SPACE_MODE": mode},
                        **confirmation, **android)
            assert (snapshot(android_root), snapshot(Path(android["workRoot"]))) == before
            print(f"PASS Android restore returns {code} and leaves saves and transactions unchanged")

    # 本地玩家（#623）：一边多一个玩家（账号集合是包含关系：另一台设备还没建这个玩家、恢复建玩家之前的备份）→ 两种实现都照常检查、C# 照常恢复；
    #   上面「各有对方没有的账号」照旧拒绝。期望手写：恢复后目标的账号就是备份里的那几个
    two = scratch / "two-players"
    subprocess.run([DOTNET, DRIVER, "--create", str(two), str(REPO), USER + "," + PLAYER2], check=True, timeout=60)
    two_archive = scratch / "two.s2asave"
    csharp_call("export", two_archive, root=two)
    assert csharp_call("inspect", two_archive)["manifest"]["users"]["ids"] == sorted([USER, PLAYER2])
    assert python_call("saveInspect", path=str(two_archive))["manifest"]["users"]["ids"] == sorted([USER, PLAYER2])   # 两人的备份 → 只有第一个玩家的数据根
    original = scratch / "original"
    shutil.move(source, original)
    shutil.copytree(two, source)
    python_call("saveInspect", path=str(python_archive))   # 一个人的备份 → 两个玩家的数据根
    csharp_call("inspect", python_archive, root=two)
    shutil.rmtree(source)
    shutil.move(original, source)
    one = scratch / "one-player"
    shutil.copytree(source, one)
    preview = csharp_call("inspect", two_archive, root=one)
    csharp_call("restore", two_archive, root=one, archiveSha256=preview["archiveSha256"], targetFingerprint=preview["targetFingerprint"])
    status = csharp_call("status", root=one)
    assert status["users"]["ids"] == sorted([USER, PLAYER2]), status
    assert len(list(one.glob("bis/user/save/*/0/readme.md"))) == 1   # 第二个玩家那份（README.md）跟着恢复进来
    print("PASS both accept a backup and a target whose players differ by one added player (subset), C# restores both players")

    data = csharp_archive.read_bytes()
    # A real pipe is unseekable, so ZipFile must emit data descriptors. This
    # exercises the reader with an actual streamed archive, without fake streams.
    descriptor = scratch / "descriptor.s2asave"
    read_fd, write_fd = os.pipe()
    def collect():
        with os.fdopen(read_fd, "rb") as src, descriptor.open("wb") as dst:
            shutil.copyfileobj(src, dst)
    reader = threading.Thread(target=collect)
    reader.start()
    with os.fdopen(write_fd, "wb") as stream, zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED) as dst:
        with zipfile.ZipFile(python_archive) as src:
            for entry in src.infolist():
                dst.writestr(entry.filename, src.read(entry))
    reader.join()
    assert python_call("saveInspect", path=str(descriptor))["manifest"] == csharp_call("inspect", descriptor)["manifest"]
    print("PASS both accept archive written to real pipe with data descriptors")
    cases = {
        "zip64-count-limit": zip64(data, count=20002),
        "zip64-directory-limit": zip64(data, size=(32 << 20) + 1),
    }
    local_name = bytearray(data)
    local_name[30] ^= 1
    cases["local-central-name-mismatch"] = local_name
    missing_zip64 = bytearray(data)
    struct.pack_into("<2H2I", missing_zip64, len(data) - 22 + 8,
                     0xffff, 0xffff, 0xffffffff, 0xffffffff)
    cases["zip64-missing-record"] = missing_zip64
    for name, field, value, fmt in (
        ("eocd-count-mismatch", 8, 1, "<2H"),
        ("eocd-directory-limit", 12, (32 << 20) + 1, "<I"),
    ):
        transformed = bytearray(data)
        struct.pack_into(fmt, transformed, len(data) - 22 + field,
                         *([value, value] if fmt == "<2H" else [value]))
        cases[name] = transformed
    stored = rewrite("stored")
    transformed = bytearray(stored.read_bytes())
    with zipfile.ZipFile(stored) as archive:
        entry = next(e for e in archive.infolist() if e.file_size > 0)
        offset = entry.header_offset
        names, extras = struct.unpack_from("<2H", transformed, offset + 26)
        transformed[offset + 30 + names + extras] ^= 1
    cases["payload-crc-corruption"] = transformed
    for name, transformed in cases.items():
        path = scratch / (name + ".s2asave")
        path.write_bytes(transformed)
        # payload-crc-corruption 坏的是第一个非空条目，即账号索引 imkvdb.arc：账号元数据坏了照旧整份拒绝（普通存档文件坏了只提示，见上面 damaged-save）
        compare(path)
        print("PASS both reject " + name + ": save_invalid")
    assert snapshot(source) == expected
    # 审计 P2-11：失败原因转人话。期望手算：errno 28 / 69 → 空间不足（save_space）；无权限类型或 errno 13 → 没有权限；
    #   InvalidData / Json → 备份损坏；其它 IOException（errno 5）→ 读写出错；未分类异常 → 内部错误；说明都不带异常类型名
    want = [("IOException", "save_space", "空间不足"), ("IOException", "save_space", "空间不足"),
            ("UnauthorizedAccessException", "save_invalid", "没有权限"), ("IOException", "save_invalid", "没有权限"),
            ("InvalidDataException", "save_invalid", "损坏"), ("JsonException", "save_invalid", "损坏"),
            ("IOException", "save_invalid", "读写存档时出错"), ("ArgumentException", "save_internal", "内部错误"),
            ("InvalidOperationException", "save_internal", "内部错误")]
    got = [json.loads(l) for l in subprocess.run([DOTNET, DRIVER, "--describe"], check=True, timeout=60, capture_output=True, text=True).stdout.splitlines()]
    assert len(got) == len(want), got
    for g, (t, code, word) in zip(got, want):
        assert (g["type"], g["code"]) == (t, code) and word in g["message"] and t not in g["message"], (g, t, code, word)
        print(f"PASS 失败原因人话 {t} → {code}：{g['message']}")
    # #316：请求带 language 时说明按它出。期望手写：zh-Hans / 不带 = 上面同一组中文原句（逐字相同）；en = 同码、同条数、一句中文都没有、
    #   且与中文一一对应（8 条里相同中文句对应相同英文句，不同中文句对应不同英文句）；英文也不带异常类型名
    cjk = lambda s: any("\u4e00" <= ch <= "\u9fff" for ch in s)
    run = lambda *a: [json.loads(l) for l in subprocess.run([DOTNET, DRIVER, "--describe", *a], check=True, timeout=60, capture_output=True, text=True).stdout.splitlines()]
    zh_named, en_got = run("zh-Hans"), run("en")
    assert [g["message"] for g in zh_named] == [g["message"] for g in got], (zh_named, got)
    assert [g["code"] for g in en_got] == [g["code"] for g in got], en_got
    assert all(not cjk(g["message"]) and g["type"] not in g["message"] and g["message"].strip() for g in en_got), en_got
    pairs = {(z["message"], e["message"]) for z, e in zip(got, en_got)}
    assert len({z for z, _ in pairs}) == len({e for _, e in pairs}) == len(pairs), pairs
    print(f"PASS 失败原因按 language 出英文：{len(en_got)} 条同码、无中文、与中文 {len(pairs)} 句一一对应")
    # 整个请求走一遍：同一份坏备份，不带 language 回中文、带 en 回英文，码相同
    bad = scratch / "language-probe.s2asave"; bad.write_bytes(b"not a zip")
    csharp_call("inspect", bad, expected_code="save_invalid",
                expected_stderr=("code=save_invalid", "type=Failure", "ZipBounds:"),
                forbidden_stderr=(str(scratch),))
    def reject(**extra):
        request = dict(operation="inspect", dataRoot=str(source), workRoot=str(scratch / "csharp-work"), game=identity, path=str(bad)) | extra
        write_json(request_path, request)
        r = json.loads(subprocess.run([DOTNET, DRIVER, "--request", str(request_path)], capture_output=True, text=True, check=True, timeout=60).stdout)
        assert not r["ok"], r
        return r["error"]
    plain, english = reject(), reject(language="en")
    assert plain["code"] == english["code"] and cjk(plain["message"]) and not cjk(english["message"]), (plain, english)
    print(f"PASS 请求带 language=en：{english['code']} 回英文「{english['message']}」，不带回中文「{plain['message']}」")
    # 审计 P2-24：恢复复制数据根时跳过存档三处、其余文件走 APFS 克隆。期望手算：512 MiB 非存档文件原样出现在目标、内容一致；
    #   存档文件（bis/user/save 下）不出现；走的是 APFS 克隆：克隆出的文件写入前与源共用物理块，首块物理设备偏移（fcntl F_LOG2PHYS）
    #   与源相同，流式拷贝则不同。11-02 起按这个事实判，不按耗时判（原判据 < 200 ms，gate-host 负载高时克隆调用本身 385 ms 误红）
    big_root = scratch / "copytree-src"; big_dst = scratch / "copytree-dst"
    (big_root / "bis/user/save/0000000000000001/0").mkdir(parents=True)
    (big_root / "bis/user/save/0000000000000001/0/progress").write_bytes(b"save-bytes")
    (big_root / "system").mkdir(); (big_root / "system/Profiles.json").write_text("{}")
    (big_root / "games/shader").mkdir(parents=True)
    blob = os.urandom(1 << 20) * 512
    (big_root / "games/shader/cache.bin").write_bytes(blob)
    out = subprocess.run([DOTNET, DRIVER, "--copytree", str(big_root), str(big_dst)], check=True, timeout=120, capture_output=True, text=True).stdout
    ms = json.loads(out.strip().splitlines()[-1])["ms"]
    assert (big_dst / "games/shader/cache.bin").read_bytes() == blob, "非存档文件内容不一致"
    assert not (big_dst / "bis/user/save/0000000000000001/0/progress").exists() and not (big_dst / "system/Profiles.json").exists(), "存档三处不该被复制"
    if sys.platform == "darwin":
        def phys(p):   # 首块物理设备偏移：struct log2phys {u32 flags; off_t contigbytes; off_t devoffset;}（pack 4，共 20 字节）
            fd = os.open(p, os.O_RDONLY)
            try: return struct.unpack("<Iqq", fcntl.fcntl(fd, 49, b"\0" * 20))[2]   # 49 = F_LOG2PHYS
            finally: os.close(fd)
        src_at, dst_at = phys(big_root / "games/shader/cache.bin"), phys(big_dst / "games/shader/cache.bin")
        assert src_at == dst_at, f"512 MiB 非存档文件没走克隆：首块物理偏移 源 {src_at} / 目标 {dst_at}（{ms} ms）"
        print(f"PASS 恢复复制数据根：跳过存档三处、512 MiB 非存档文件走 APFS 克隆（首块物理偏移同 {src_at}，{ms} ms）")
    else:
        print(f"PASS 恢复复制数据根：跳过存档三处、512 MiB 非存档文件内容一致（{ms} ms；非 macOS 不核克隆）")
    # 审计 P2-175：目标指纹只算存档三处。期望手算：数据根里加一份非存档文件（着色器缓存）→ 指纹不变；改一个存档文件 → 指纹变
    fp_root = scratch / "fingerprint-root"
    shutil.copytree(source, fp_root)
    fp0 = csharp_call("inspect", python_archive, root=fp_root)["targetFingerprint"]
    (fp_root / "games/shader").mkdir(parents=True)
    (fp_root / "games/shader/cache.bin").write_bytes(os.urandom(4096))
    fp1 = csharp_call("inspect", python_archive, root=fp_root)["targetFingerprint"]
    save_file = next(fp_root.glob("bis/user/save/*/0/contributing.md"))
    save_file.write_bytes(save_file.read_bytes() + b"x")
    fp2 = csharp_call("inspect", python_archive, root=fp_root)["targetFingerprint"]
    assert fp1 == fp0 and fp2 != fp0, (fp0, fp1, fp2)
    print("PASS 目标指纹：加着色器缓存不变、改存档文件变（审计 P2-175）")

    # 审计 P2-177：恢复在事务 / 备份目录建好、恢复记录写出之前失败。期望手算：报错；save-transactions 与 save-backups/<游戏号>
    #   下的子目录与调用前相同（本次建的都删了，前面用例正常恢复留下的备份不算）；活存档根逐文件不变
    tx_root = scratch / "abandon-root"
    shutil.copytree(source, tx_root)
    before = snapshot(tx_root)
    work = scratch / "csharp-work"
    def subdirs():
        return sorted(p for d in (work / "save-transactions", work / "save-backups" / GAME_ID) if d.exists() for p in d.iterdir() if p.is_dir())
    dirs_before = subdirs()
    preview = csharp_call("inspect", python_archive, root=tx_root)
    csharp_call("restore", python_archive, root=tx_root, archiveSha256=preview["archiveSha256"],
                targetFingerprint=preview["targetFingerprint"], expected_code="save_invalid",
                env=dict(SAVE_TRANSFER_FAIL_AT="dirsCreated"))
    left = subdirs()
    assert left == dirs_before and snapshot(tx_root) == before, (dirs_before, left, snapshot(tx_root) == before)
    print("PASS 恢复记录写出前失败：事务 / 备份目录不残留、活存档根不变（审计 P2-177）")
    print("Public-file G05/G06 interoperability passed; no game or player progress was tested.")
