#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Real request/worker transport and verbatim adapter callback, host-only.

The 25 original synthetic cases retain exact JSON allocation/attempt binding,
raw failure bytes, query parsing and worker cancellation assertions. The facts
owner and managed compiler are explicit fixture boundaries: this does not prove
source-store ownership, canonical identity, GPU behavior or W2 acceptance.

adapter_worker_compile is extracted verbatim from mtl_vk_adapter.c, with a unique
signature and a complete C body required. Request, worker, SHA and parser are
linked from production. This is not a full adapter translation-unit link.
The normal run also compiles three request mutations and requires their specific
behavioral assertion failures. Build failure, signal and timeout are never red
controls. No NDK, device, game input, online download or historical Git required.
"""

import argparse
import hashlib
import os
import shlex
import subprocess
import tempfile
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import host_link   # 链接清单按符号闭包推导、弱引用自动放行（见 host_link.py）

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "android/native"
HERE = Path(__file__).with_suffix(".c")
CASES = (
    "raw", "worker", "copy", "replay", "gate_closed", "no_emit", "multi_emit",
    "success", "bad_method", "bad_coordinate", "trailing_message", "duplicate_key",
    "malformed", "timeout", "stop", "dedup", "pause_failure", "not_running",
    "sampler_method", "buffer_method", "format_method", "sampled_format_method", "escaped_message",
    "parser_missing", "stale_return", "thread_exit",
)


def function_body(text, signature):
    """Return a unique complete source body, ignoring braces in strings/comments."""
    if text.count(signature) != 1:
        raise RuntimeError("source extraction signature changed: " + signature)
    start = text.index(signature)
    opening = text.index("{", start)
    depth, index = 0, opening
    quote = None
    while index < len(text):
        char = text[index]
        if quote:
            if char == "\\":
                index += 2
                continue
            if char == quote:
                quote = None
        elif text.startswith("//", index):
            end = text.find("\n", index + 2)
            if end < 0:
                break
            index = end
            continue
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            if end < 0:
                break
            index = end + 2
            continue
        elif char in ('"', "'"):
            quote = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if not depth:
                return text[start:index + 1] + "\n"
        index += 1
    raise RuntimeError("source extraction body is incomplete: " + signature)


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError("mutation anchor changed: " + old)
    return text.replace(old, new, 1)


def run(directory):
    callback = function_body(
        (NATIVE / "nvn/mtl_vk_adapter.c").read_text(), "static int adapter_worker_compile("
    )
    (directory / "adapter_wrapper.inc").write_text(callback)
    print("actual adapter wrapper SHA256", hashlib.sha256(callback.encode()).hexdigest(), flush=True)
    source = NATIVE / "direct/compile_request.c"
    original = source.read_text()
    cc = shlex.split(os.environ.get("CC", "cc"))
    flags = shlex.split(os.environ.get("CFLAGS", ""))
    common = cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread"] + flags
    common += ["-I" + str(path) for path in (
        NATIVE, NATIVE / "direct", directory, ROOT / "graphics/nvn/gfx/nvn/include"
    )]

    def build(name, request=source, parser=True):
        executable = directory / name
        units = [str(HERE), str(request)]
        units += [str(NATIVE / "direct" / (unit + ".c"))
                  for unit in ("compile_worker", "sha256")]
        if parser:
            units.append(str(NATIVE / "direct/json.c"))
        command = common + units + ["-o", str(executable)]
        result = host_link.run(command, capture_output=True, text=True, timeout=40)
        (directory / (name + "-build.log")).write_text(
            shlex.join(command) + "\n" + result.stdout + result.stderr
        )
        if result.returncode:
            raise RuntimeError(name + ": compile failure is not a negative control\n" + result.stderr)
        return executable

    def invoke(executable, case):
        result = subprocess.run(
            [str(executable), case], capture_output=True, text=True, timeout=12
        )
        output = result.stdout + result.stderr
        (directory / (executable.name + "-" + case + ".log")).write_text(
            "exit=" + str(result.returncode) + "\n" + output
        )
        print(output, end="", flush=True)
        return result

    positive = build("production-request-worker")
    missing_parser = build("production-request-worker-no-parser", parser=False)
    for case in CASES:
        result = invoke(missing_parser if case == "parser_missing" else positive, case)
        if result.returncode or case + ": PASS " not in result.stdout:
            raise RuntimeError("production case failed: " + case)

    controls = (
        ("allow-copied-allocation", "json != a->original_json || ", "", "copy",
         "expected exact diagnostic publication count"),
        ("accept-multiple-emits", "emits != 1", "emits < 1", "multi_emit",
         "expected exact diagnostic publication count"),
        ("keep-old-returned-receipt", "if (nvk_w2_capsule_returned)",
         "if (a && nvk_w2_capsule_returned)", "stale_return",
         "new unattributed invocation cannot reuse old published receipt"),
    )
    for name, old, new, case, expected in controls:
        mutant = directory / (name + ".c")
        mutant.write_text(replace_once(original, old, new))
        result = invoke(build(name, mutant), case)
        if result.returncode != 1 or "ASSERT " not in result.stderr or expected not in result.stderr:
            raise RuntimeError(name + ": expected functional assertion red was not observed")
        print(name + ": expected functional red", flush=True)
    print(
        f"PASS: {len(CASES)}/{len(CASES)} request/worker cases, "
        f"{len(controls)}/{len(controls)} assertion-based negative controls; "
        "synthetic facts/compiler boundaries, no GPU/device/acceptance claim",
        flush=True,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--keep-output", type=Path, help="new directory for binaries and evidence")
    args = parser.parse_args()
    if args.keep_output:
        directory = args.keep_output.resolve()
        directory.mkdir(parents=True, exist_ok=False)
        run(directory)
    else:
        with tempfile.TemporaryDirectory(prefix="w2-request-worker-") as temporary:
            run(Path(temporary))


if __name__ == "__main__":
    main()
