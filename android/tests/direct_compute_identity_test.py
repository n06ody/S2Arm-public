#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Host-only identity checks plus three independent rejection controls. No device/SDK."""
import os
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "android/native"
TESTS = ROOT / "android/tests"
SOURCE = NATIVE / "direct/input_identity.c"


def main():
    with tempfile.TemporaryDirectory(prefix="s2a-cs-identity-") as directory:
        temp = Path(directory)

        def run(name, source, existing=False):
            binary = temp / name
            test = "direct_input_identity_test.c" if existing else "direct_compute_identity_test.c"
            command = shlex.split(os.environ.get("CC", "cc")) + [
                "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I" + str(NATIVE), "-I" + str(NATIVE / "direct"),
                "-I" + str(ROOT / "graphics/nvn/gfx/nvn/include"),
                str(TESTS / test), str(source), str(NATIVE / "direct/texture_facts.c"),
            ]
            if existing:
                command.append(str(NATIVE / "direct/sha256.c"))
            subprocess.run(command + ["-o", str(binary)], check=True, timeout=30)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            print(json.dumps({"case": name, "argv": [str(binary)],
                              "returncode": result.returncode,
                              "stdout": result.stdout, "stderr": result.stderr}))
            diagnostics = result.stdout + result.stderr
            if result.stderr or any(marker in diagnostics for marker in
                    ("AddressSanitizer", "UndefinedBehaviorSanitizer", "LeakSanitizer", "runtime error:")):
                raise SystemExit(name + " emitted unexpected runtime diagnostics")
            print(name + ": " + result.stdout.strip())
            return result

        for name, existing in [("canonical-positive", False), ("existing-regression", True)]:
            result = run(name, SOURCE, existing)
            if result.returncode:
                raise SystemExit(name + " failed")

        source = SOURCE.read_text()
        cs = '        else if (t->stage && !strcmp(t->stage, "cs")) stage_idx = 2;\n'
        unknown = '        else return -1;   // 缺 stage 不等同 vs：NULL 与 "vs" 不能编成同一个输入\n'
        if source.count(cs) != 1 or source.count(unknown) != 1:
            raise SystemExit("mutation anchors changed; review the controls")
        mutations = [
            ("omit-alignment", source.replace("wflag(&w, in->compute_unaligned_ssbo);", "wflag(&w, 0);"), "FAIL: cs canonical bytes"),
            ("reverted-cs", source.replace(cs, ""), "FAIL: cs canonical bytes"),
            ("cs-fs-alias", source.replace(cs, cs.replace("= 2", "= 1")), "FAIL: cs canonical bytes"),
            ("unknown-accepted", source.replace(unknown, "        else stage_idx = 0;\n"),
             "FAIL: unknown stage rejected"),
        ]
        for name, mutated, failure in mutations:
            path = temp / (name + ".c")
            path.write_text(mutated)
            result = run(name, path)
            if result.returncode != 1 or failure not in result.stdout:
                raise SystemExit(name + " did not fail for the expected assertion")
        print("PASS: 2 positive suites, 4 mutation controls; no device adoption claim")


if __name__ == "__main__":
    main()
