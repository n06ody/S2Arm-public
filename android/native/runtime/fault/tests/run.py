#!/usr/bin/env python3
"""Isolated Linux/AArch64 signal test; --objects-only also accepts NDK/Bionic.

No device/Android application build. Output must be outside the checkout.
The callbacks and xstack registration are explicitly fixture providers.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess


FAULT = Path(__file__).resolve().parents[1]
NATIVE = FAULT.parent.parent
ROOT = FAULT.parents[3]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--objects-only", action="store_true")
    parser.add_argument("--land-probe", action="store_true",
                        help="run the direct NVN page probe through the same real Linux XSTACK")
    args = parser.parse_args()
    if args.objects_only and args.land_probe:
        parser.error("--land-probe requires Linux AArch64 execution")
    out = args.out.resolve()
    if out == ROOT or ROOT in out.parents:
        parser.error("--out must be outside the checkout")
    out.mkdir(parents=True, exist_ok=True)
    sources = [FAULT / "fault.c", FAULT / "trampoline.S", FAULT / "tests/fault_test.c",
               FAULT / "tests/register_probe.S", FAULT / "tests/guest_stack.S",
               NATIVE / "guest_signal.c", NATIVE / "redzone.S"]
    inputs = sources + [FAULT / "fault.h", FAULT / "sigframe_compat.h", NATIVE / "guest_signal.h", Path(__file__).resolve()]
    if args.land_probe:
        sources += [ROOT / "graphics/nvn/gfx/nvn/nvn_dirty.c",
                    NATIVE / "nvn/nvn_host_hooks.c", ROOT / "runtime/src/direct_host.c"]
        inputs += sources[-3:] + [ROOT / "graphics/nvn/gfx/nvn/nvn_dirty.h",
                                  ROOT / "runtime/include/s2a_direct_host.h"]
    before = {str(p.relative_to(ROOT)): digest(p) for p in inputs}
    receipt = {"kind": "object-compile" if args.objects_only else "linux-aarch64-signal-fixture",
               "source_sha256": before, "host": platform.platform(), "commands": [],
               "managed_callback_executed": False, "android_executed": False}

    def run(command, name, expected=0, contains=None):
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        (out / (name + ".log")).write_text(result.stdout)
        valid = result.returncode == expected and (contains is None or contains in result.stdout)
        receipt["commands"].append({"command": [str(x) for x in command], "exit": result.returncode,
                                    "expected_exit": expected, "log": name + ".log", "pass": valid})
        (out / "results.json").write_text(json.dumps(receipt, indent=2) + "\n")
        if not valid:
            raise RuntimeError(f"{name}: unexpected result ({result.returncode}); see {out}")
        return result

    compiler = shutil.which(args.cc)
    if not compiler:
        raise RuntimeError("compiler not found")
    run([compiler, "--version"], "compiler")
    language = "-std=gnu11" if args.land_probe else "-std=c11"
    flags = [compiler, language, "-O2", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(FAULT)]
    if args.land_probe:
        flags += ["-DS2A_NVN_PROBE_FIXTURE", "-I", str(ROOT), "-I", str(ROOT / "runtime/include"),
                  "-I", str(ROOT / "graphics/track")]
    if args.objects_only:
        for i, source in enumerate(sources):
            run(flags + ["-c", str(source), "-o", str(out / f"{i}-{source.name}.o")], f"compile-{i}")
    else:
        if platform.system() != "Linux" or platform.machine() not in ("aarch64", "arm64"):
            raise RuntimeError("execution requires a native Linux ARM64 host; use --objects-only for NDK")
        binary = out / "fault-test"
        run(flags + [str(p) for p in sources] + ["-o", str(binary)], "compile")
        run([str(binary)], "full", contains="PASS ")
        run([str(binary), "real-only"], "real-only", contains="PASS ")
        original = sources[0].read_text()
        changes = [
            ("simd-restore-broken", "memcpy(context, &state->saved, sizeof *context);",
             "memcpy(context, &state->saved, sizeof *context);\n"
             "    ((struct fpsimd_context *)context->uc_mcontext.__reserved)->vregs[0] = 0;",
             "FAIL real all 32 SIMD"),
            ("cursor-restore-missing", "state->slot->cur = state->old_cursor;", "/* deliberately omitted */",
             "FAIL real kernel returns restored xstack cursor"),
        ]
        for name, old, new, message in changes:
            if original.count(old) != 1:
                raise RuntimeError(f"mutation target no longer unique: {name}")
            mutant = out / (name + ".c")
            mutant.write_text(original.replace(old, new))
            exe = out / name
            run(flags + [str(mutant)] + [str(p) for p in sources[1:]] + ["-o", str(exe)], name + "-compile")
            run([str(exe), "real-only"], name, expected=1, contains=message)
        name = "probe-dispatch-missing"
        old = "((ucontext_t *)context)->uc_mcontext.pc == (uintptr_t)s2a_android_fault_probe_store"
        if original.count(old) != 1:
            raise RuntimeError("probe mutation target no longer unique")
        mutant = out / (name + ".c")
        mutant.write_text(original.replace(old, "0 /* deliberately missing probe dispatch */"))
        exe = out / name
        run(flags + [str(mutant)] + [str(p) for p in sources[1:]] + ["-o", str(exe)], name + "-compile")
        run([str(exe), "real-only"], name, expected=124)
    after = {str(p.relative_to(ROOT)): digest(p) for p in inputs}
    receipt["source_unchanged"] = before == after
    receipt["pass"] = before == after and all(c["pass"] for c in receipt["commands"])
    receipt["artifacts_sha256"] = {p.name: digest(p) for p in out.iterdir()
                                   if p.is_file() and p.suffix != ".json"}
    (out / "results.json").write_text(json.dumps(receipt, indent=2) + "\n")
    if not receipt["pass"]:
        raise RuntimeError("source changed during validation")
    print(f"PASS {receipt['kind']}; receipt {out / 'results.json'}")


if __name__ == "__main__":
    main()
