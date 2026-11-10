#!/usr/bin/env python3
"""Development runtime library (android/README.md "Development runtime library"): the
manifest generator and the pusher. A development run is no reading and no formal receipt.

  dev_runtime.py manifest --lib <stripped .so> --apk <app-debug.apk> --out <dir>
                          [--aot-host linux-arm64|linux-x64] [--ilc Release|Debug]
      Checks the library the way the app does (AArch64 ELF64, 16 KB LOAD segments, DT_SONAME
      libs2arm_android.so) and that it carries this tree's JNI ABI (native/jni_abi.py), then writes
      <out>/libs2arm_android.so and <out>/dev-runtime.json: libSha256, baseApkSha256 (the APK the
      library is bound to: install exactly that one), sourceCommit (HEAD, 40 hex), sourceTree /
      dirty / diffSha256 (the working tree as git sees it, untracked files included).

  dev_runtime.py push --serial S --package P --console-mode handheld|docked --lib-dir <dir>
                      [--extra s2arm.KEY[=VALUE] ...] [--clean] [--no-start] [--adb ADB]
      1. host: library SHA == manifest libSha256; the ELF checks above; sourceCommit is 40 hex.
      2. device: the package is run-as-able (debuggable); the installed base.apk SHA equals
         baseApkSha256 (Java changed: reinstall that APK first).
      3. stop: --activity-single-top --ez s2arm.finish true (graceful: the shader cache is exported),
         wait for the process to exit; force-stop only after 5 s (recorded as forced_stop).
         With wait_done= the graceful-shutdown dump (frame-log chunks) is awaited first
         (bounded) — force-stopping mid-dump truncates the block.
      4. push to /data/local/tmp, run-as copy into files/dev-runtime/<sha>/ (.tmp then mv), chmod 0444
         the files and 0555 the directory, check the SHA on the device, remove the /data/local/tmp copies.
         --clean first removes the other files/dev-runtime/* (chmod -R u+w, then rm: 0555 directories
         cannot be emptied otherwise).
      5. am start -W --es s2arm.dev_runtime <sha> --es s2arm.console_mode <mode> [extras]; accepted only
         when logcat shows `runtime-library: dev-override sha256=<sha>` within 20 s.
      Writes <lib-dir>/push-<serial>-<UTC>.json. Exit 0 accepted (or pushed with --no-start), 1 refused
      on the device or timed out, 2 a host-side precondition failed. Run it under the device lease of
      your setup (it calls adb directly).
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ANDROID = Path(__file__).resolve().parents[1]
REPO = ANDROID.parents[0]
ACTIVITY = "org.s2arm.android.MainActivity"
SONAME = b"libs2arm_android.so"
SHA = re.compile(r"[0-9a-f]{64}")
EXTRA = re.compile(r"s2arm\.[a-z0-9_]+(=[A-Za-z0-9._-]+)?")


class Refuse(Exception):
    pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def elf_problem(path):
    """RuntimeLibrary.elf in Python: None, or why the app would refuse the library."""
    data = Path(path).read_bytes()
    if data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", data, 18)[0] != 183:
        return "not a little-endian AArch64 ELF64"
    phoff = struct.unpack_from("<Q", data, 32)[0]
    phsize, phcount = struct.unpack_from("<HH", data, 54)
    loads, dynamic = [], None
    for i in range(phcount):
        kind, _, offset, address, _, filesz, _, align = struct.unpack_from("<IIQQQQQQ", data, phoff + i * phsize)
        if kind == 2:
            dynamic = (offset, filesz)
        if kind == 1:
            if align < 16384 or (address - offset) % 16384:
                return "ELF LOAD segment is not 16 KB aligned"
            loads.append((offset, address, filesz))
    if not loads or not dynamic:
        return "ELF has no LOAD segments or no dynamic section"
    strtab = soname = None
    for at in range(dynamic[0], dynamic[0] + dynamic[1] - 15, 16):
        tag, value = struct.unpack_from("<qQ", data, at)
        if tag == 0:
            break
        strtab = value if tag == 5 else strtab
        soname = value if tag == 14 else soname
    if strtab is None or soname is None:
        return "ELF has no DT_SONAME"
    for offset, address, filesz in loads:
        if 0 <= strtab - address < filesz:
            start = offset + strtab - address + soname
            return None if data[start:start + len(SONAME) + 1] == SONAME + b"\0" else "DT_SONAME is not libs2arm_android.so"
    return "DT_SONAME is outside the LOAD segments"


def jni_abi():
    spec = importlib.util.spec_from_file_location("jni_abi", ANDROID / "native/jni_abi.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    java = ANDROID / "app/src/main/java/org/s2arm/android"
    return module.identity([java / "NativeBridge.java", java / "TouchInputBridge.java"])[1]


def git(*args, env=None):
    return subprocess.run(["git", "-C", str(REPO), *args], check=True, capture_output=True, env=env).stdout


def source_identity():
    """HEAD, and the working tree (tracked + untracked-not-ignored) as a git tree via a private index."""
    with tempfile.TemporaryDirectory() as temp:
        index = Path(temp) / "index"
        shutil.copyfile(git("rev-parse", "--path-format=absolute", "--git-path", "index").decode().strip(), index)
        env = {**os.environ, "GIT_INDEX_FILE": str(index)}
        git("add", "-A", ".", env=env)
        tree = git("write-tree", env=env).decode().strip()
    head = git("rev-parse", "HEAD").decode().strip()
    head_tree = git("rev-parse", "HEAD^{tree}").decode().strip()
    diff = git("diff", "--binary", head_tree, tree)
    return {"sourceCommit": head, "sourceTree": tree, "dirty": tree != head_tree,
            "diffSha256": hashlib.sha256(diff).hexdigest()}


def manifest(args):
    lib, apk, out = Path(args.lib), Path(args.apk), Path(args.out)
    problem = elf_problem(lib)
    if problem:
        raise Refuse(f"{lib}: {problem}")
    abi = jni_abi()
    if abi.encode() not in lib.read_bytes():
        raise Refuse(f"{lib} does not carry this tree's JNI ABI {abi[:12]}… (built from other Java native declarations)")
    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(lib, out / "libs2arm_android.so")
    doc = {"libSha256": sha256(out / "libs2arm_android.so"), "baseApkSha256": sha256(apk), **source_identity(),
           "ilcConfig": args.ilc, "aotHost": args.aot_host, "jniAbi": abi,
           "builtAt": datetime.datetime.now(datetime.timezone.utc).isoformat()}
    (out / "dev-runtime.json").write_text(json.dumps(doc, indent=1) + "\n")
    print(json.dumps({"out": str(out), "libSha256": doc["libSha256"], "baseApkSha256": doc["baseApkSha256"]}))
    return 0


class Device:
    def __init__(self, adb, serial):
        self.argv = [*shlex.split(adb), "-s", serial]

    def run(self, *args, timeout=60, allowed=(0,)):
        r = subprocess.run([*self.argv, *args], capture_output=True, timeout=timeout)
        if r.returncode not in allowed:
            raise RuntimeError(f"adb {' '.join(args[:3])} rc={r.returncode}: {(r.stderr or r.stdout).decode(errors='replace')[:300]}")
        return r.stdout.decode(errors="replace")

    def shell(self, command, timeout=60, allowed=(0,)):
        return self.run("shell", command, timeout=timeout, allowed=allowed)

    def run_as(self, package, command, timeout=120):
        return self.shell(f"run-as {package} sh -c {shlex.quote(command)}", timeout=timeout)


def push(args):
    lib_dir = Path(args.lib_dir).resolve()
    lib, manifest_path = lib_dir / "libs2arm_android.so", lib_dir / "dev-runtime.json"
    if not lib.is_file() or not manifest_path.is_file():
        raise Refuse(f"{lib_dir} lacks libs2arm_android.so or dev-runtime.json (make them with: dev_runtime.py manifest)")
    doc = json.loads(manifest_path.read_text())
    sha = sha256(lib)
    if sha != doc.get("libSha256"):
        raise Refuse(f"library SHA {sha[:12]} != dev-runtime.json libSha256 {str(doc.get('libSha256'))[:12]}")
    if not SHA.fullmatch(str(doc.get("baseApkSha256"))) or not re.fullmatch(r"[0-9a-f]{40}", str(doc.get("sourceCommit"))):
        raise Refuse("dev-runtime.json lacks baseApkSha256 or a 40-hex sourceCommit (the app accepts nothing else)")
    problem = elf_problem(lib)
    if problem:
        raise Refuse(f"{lib}: {problem}")
    receipt = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "serial": args.serial,
               "package": args.package, "kind": "DEV_RUNTIME_PUSH_NOT_A_READING", "lib_sha256": sha,
               "base_apk_sha256": doc["baseApkSha256"], "source_commit": doc["sourceCommit"], "dirty": doc.get("dirty")}
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    receipt_path = lib_dir / f"push-{args.serial}-{stamp}.json"
    d, pkg = Device(args.adb, args.serial), args.package
    rc = 1
    try:
        d.run_as(pkg, "id -u")
        base = [b[len("package:"):] for b in d.shell(f"pm path {pkg}").split() if b.endswith("/base.apk")]
        if len(base) != 1:
            raise Refuse(f"pm path gave no single base.apk: {base}")
        installed = d.shell(f"sha256sum {shlex.quote(base[0])}").split()[0]
        receipt["installed_apk_sha256"] = installed
        if installed != doc["baseApkSha256"]:
            raise Refuse(f"installed APK {installed[:12]} is not the library's base {doc['baseApkSha256'][:12]}: install that APK first")
        receipt["stop"] = stop(d, pkg)
        tmp_lib, tmp_json = f"/data/local/tmp/s2a-dev-{sha[:16]}.so", f"/data/local/tmp/s2a-dev-{sha[:16]}.json"
        start = time.monotonic()
        d.run("push", str(lib), tmp_lib, timeout=300)
        d.run("push", str(manifest_path), tmp_json)
        target = f"files/dev-runtime/{sha}"
        clean = (f'for x in files/dev-runtime/*; do [ "$x" = {shlex.quote(target)} ] || '
                 '{ chmod -R u+w "$x"; rm -rf "$x"; }; done; ' if args.clean else "")
        d.run_as(pkg, clean + f"mkdir -p {target} && chmod u+w {target} && "
                 f"rm -f {target}/libs2arm_android.so {target}/dev-runtime.json && "
                 f"cp {tmp_lib} {target}/libs2arm_android.so.tmp && mv {target}/libs2arm_android.so.tmp {target}/libs2arm_android.so && "
                 f"cp {tmp_json} {target}/dev-runtime.json && chmod 0444 {target}/libs2arm_android.so {target}/dev-runtime.json && "
                 f"chmod 0555 {target}")
        d.shell(f"rm -f {tmp_lib} {tmp_json}")
        on_device = d.run_as(pkg, f"sha256sum {target}/libs2arm_android.so").split()[0]
        receipt["push_seconds"] = round(time.monotonic() - start, 2)
        if on_device != sha:
            raise Refuse(f"library on the device has SHA {on_device[:12]}, not {sha[:12]}")
        receipt["pushed"] = target
        if args.no_start:
            rc = 0
        else:
            extras = f" --es s2arm.dev_runtime {sha} --es s2arm.console_mode {args.console_mode}"
            for key in args.extra:
                name, _, value = key.partition("=")
                extras += f" --ez {name} {value or 'true'}" if value in ("", "true", "false") else f" --es {name} {value}"
            d.shell("logcat -c")
            started = time.monotonic()
            receipt["am_start"] = d.shell(f"am start -W -n {pkg}/{ACTIVITY}{extras}").strip().splitlines()[-4:]
            receipt["accepted"], receipt["log"] = verdict(d, sha)
            receipt["start_to_verdict_s"] = round(time.monotonic() - started, 2)
            rc = 0 if receipt["accepted"] else 1
    except Refuse as error:
        receipt["error"] = str(error)
        rc = 2
    except (RuntimeError, IndexError, subprocess.TimeoutExpired) as error:
        receipt["error"] = str(error)
        rc = 1
    receipt_path.write_text(json.dumps(receipt, indent=1, ensure_ascii=False) + "\n")
    print(("accepted (development run: no reading)" if rc == 0 and not args.no_start else "pushed, not started"
           if rc == 0 else "REFUSED: " + receipt.get("error", receipt.get("log", ""))) + f"; receipt {receipt_path}")
    return rc


def stop(d, pkg, wait_done=None, wait_seconds=20):
    pid = d.shell(f"pidof {pkg}", allowed=(0, 1)).strip()
    if not pid:
        return {"was_running": False}
    d.shell(f"am start --activity-single-top -n {pkg}/{ACTIVITY} --ez s2arm.finish true")
    seen = None
    if wait_done is not None:
        # 优雅退出触发适配器 shutdown 转储（帧日志分片）：写完前 force-stop /
        # 进程组随收局退出都会截断块。等 wait_done()（wait_seconds 上限）再验死
        # 强杀；谓词自身负责把「当局进程全退」也当落定（都死了转储不会再长）。
        seen = False
        deadline = time.monotonic() + wait_seconds
        while time.monotonic() < deadline:
            if wait_done():
                seen = True
                break
            time.sleep(0.5)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if not d.shell(f"pidof {pkg}", allowed=(0, 1)).strip():
            result = {"was_running": True, "pid": pid, "forced_stop": False}
            if seen is not None:
                result["dump_done"] = seen
            return result
        time.sleep(0.25)
    d.shell(f"am force-stop {pkg}")
    result = {"was_running": True, "pid": pid, "forced_stop": True}
    if seen is not None:
        result["dump_done"] = seen
    return result


def verdict(d, sha, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        for line in d.shell("logcat -d -s S2Arm:V").splitlines():
            if f"runtime-library: dev-override sha256={sha}" in line:
                return True, line
            if "Runtime library load failed" in line or "Runtime library mismatch" in line:
                return False, line
        time.sleep(0.5)
    return False, f"no runtime-library line in logcat within {seconds} s"


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("manifest")
    m.add_argument("--lib", required=True)
    m.add_argument("--apk", required=True)
    m.add_argument("--out", required=True)
    m.add_argument("--aot-host", choices=("linux-arm64", "linux-x64"), default="linux-arm64")
    m.add_argument("--ilc", choices=("Release", "Debug"), default="Release")
    s = sub.add_parser("push")
    s.add_argument("--serial", required=True)
    s.add_argument("--package", required=True)
    s.add_argument("--console-mode", choices=("handheld", "docked"), required=True)
    s.add_argument("--lib-dir", required=True)
    s.add_argument("--extra", action="append", default=[])
    s.add_argument("--clean", action="store_true")
    s.add_argument("--no-start", action="store_true")
    s.add_argument("--adb", default="adb")
    args = p.parse_args(argv)
    if args.cmd == "push":
        if not re.fullmatch(r"[A-Za-z0-9._:-]+", args.serial) or not re.fullmatch(r"[a-z][a-z0-9_]*(\.[a-z0-9_]+)+", args.package):
            p.error("unsafe serial or package")
        for key in args.extra:
            if not EXTRA.fullmatch(key) or key.startswith(("s2arm.dev_runtime", "s2arm.console_mode")):
                p.error(f"bad extra {key} (dev_runtime / console_mode are set by this tool)")
    try:
        return manifest(args) if args.cmd == "manifest" else push(args)
    except (Refuse, OSError, ValueError) as error:
        print(f"refused: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
