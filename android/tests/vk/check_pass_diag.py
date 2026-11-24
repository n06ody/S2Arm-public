# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Check exact timing coverage and copy payloads in the real Vulkan fixture."""
import re
import sys
from pathlib import Path


def need(condition, reason):
    if not condition:
        sys.exit(f"GPU pass diagnostics: {reason}")


arm, path = sys.argv[1:]
log = Path(path).read_text()
need("feedback cases=4 failures=0" in log, "fixture did not complete")
need(not re.search(r"Validation Error|SYNC-HAZARD|VUID-", log), "validation failed")
cases = re.split(r"^feedback begin .* case=(\d+)$", log, flags=re.M)
needs_pass = arm in ("pass", "both")
needs_snap = arm in ("snapshot", "both")
need(cases[1::2] == ["0", "3", "13", "14"], "fixture coverage")
for case, body in zip(cases[1::2], cases[2::2]):
    passes = {"0": 3, "3": 5, "13": 3, "14": 0}[case]
    copies = {"0": 0, "3": 2, "13": 2, "14": 0}[case]
    segments = re.findall(r"nvk-pass-segment .*?passes=(\d+) timed=(\d+)", body)
    tops = re.findall(r"nvk-pass-top (.+)", body)
    durations = re.findall(r"nvk-pass-ms .*? ms=([\d.,NA]+)", body)
    summaries = re.findall(r"nvk-fb-frame .*?snapshots=(\d+) regions=(\d+) bytes=(\d+) unknown=(\d+)", body)
    sites = re.findall(r"nvk-fb-site .*?stage=(\d+) set=(\d+) binding=(\d+).*?bytes=(\d+)", body)
    if needs_pass and passes:
        need(segments == [(str(passes), str(passes))], f"case {case}: expected {passes} timed passes, got {segments}")
        need("submit_timed=1" in body, f"case {case}: missing submission endpoints")
        need(len(tops) == passes, f"case {case}: got {len(tops)} top entries, want {passes}")
        ranked = [float(re.search(r" ms=([\d.]+)", top)[1]) for top in tops]
        need(ranked == sorted(ranked, reverse=True), f"case {case}: top passes out of order")
        need(all("size=4x4" in top for top in tops), f"case {case}: render extent mismatch")
        need(any("fmt=37" in top and "load=1 store=0" in top for top in tops), f"case {case}: missing RGBA8 CLEAR/STORE attachment")
        if case == "13":
            need(any("fmt=130" in top and "load=1 store=0 stencil_load=2 stencil_store=0" in top for top in tops),
                 "case 13: missing D32S8 depth-only attachment operations")
        values = [v for group in durations for v in group.split(",")]
        need(len(values) == passes and "NA" not in values, f"case {case}: missing individual durations")
        need(sorted(map(float, values), reverse=True) == ranked, f"case {case}: top durations differ from pass list")
    else:
        need(not (segments or tops or durations), f"case {case}: unexpected timing output")
    if needs_snap and passes:
        need(summaries == [(str(copies), str(copies), str(copies * 64), "0")],
             f"case {case}: expected {copies} snapshots and {copies * 64} bytes, got {summaries}")
        need(sites == ([("1", "0", "0", "128")] if copies else []),
             f"case {case}: snapshot sites: {sites}")
    else:
        need(not (summaries or sites), f"case {case}: unexpected snapshot output")
print(f"GPU pass diagnostics: arm={arm} passes and snapshot payloads match four cases")
