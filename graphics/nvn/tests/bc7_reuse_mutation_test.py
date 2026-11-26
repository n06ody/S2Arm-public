#!/usr/bin/env python3
"""重编真实资源回放的两个变异体，分别核对动态重入和晋升后源字节缓存。"""
import shlex
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[3]
out = root / "build/bc7-reuse-mutations"
out.mkdir(parents=True, exist_ok=True)
resource = "graphics/nvn/replay/rp_res.c"
source = (root / resource).read_text()
recipe = subprocess.check_output(["make", "-n", "native-test-nvn_res"], cwd=root, text=True)
compile_line = next(line for line in recipe.replace("\\\n", " ").splitlines() if resource in line)
command = shlex.split(compile_line)
cases = [
    ("requeue", "if (bc7 && x->ever_uploaded)", "if (0 && bc7 && x->ever_uploaded)",
     "--bc7-rebind", "== plain && !g_bc7_staged"),
    ("discard-cache", "tex_replace_storage(x, next, 1)", "tex_replace_storage(x, next, 0)",
     "--bc-byte-equal", "== promoted && g_bc_snapshots == initial_uploads"),
]
for name, old, new, mode, assertion in cases:
    if source.count(old) != 1:
        raise SystemExit(f"{name} 变异锚点失配")
    mutated = out / f"{name}.c"
    mutated.write_text(source.replace(old, new))
    exe = out / name
    args = command.copy()
    args[args.index("-o") + 1] = str(exe)
    args[args.index(resource)] = str(mutated)
    subprocess.run(args, cwd=root, check=True)
    result = subprocess.run([exe, mode], cwd=root, text=True, capture_output=True, timeout=30)
    failures = [line for line in result.stdout.splitlines() if "【红】" in line]
    if result.returncode != 1 or not any(assertion in line for line in failures):
        raise SystemExit(f"{name} 未命中目标行为，rc={result.returncode}\n{result.stdout}{result.stderr}")
    print(f"PASS {name}: {next(line for line in failures if assertion in line)}", flush=True)
