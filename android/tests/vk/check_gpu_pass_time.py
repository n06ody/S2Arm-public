# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
import sys
from math import isclose
from pathlib import Path


def row(line):
    return dict(field.split("=", 1) for field in line.split() if "=" in field)


lines = Path(sys.argv[1]).read_text().splitlines()
windows = [row(line) for line in lines if "nvk-gpu-pass-time window " in line]
assert len(windows) == 1, f"expected one GPU ledger window, got {len(windows)}"
window, = windows
tops = [row(line) for line in lines if "nvk-gpu-pass-time top " in line]
lanes = {r["lane"]: r for line in lines if "nvk-gpu-pass-time lane " in line for r in [row(line)]}
sizes = [row(line) for line in lines if "nvk-gpu-pass-time size " in line]
details = [row(line) for line in lines if "nvk-gpu-pass-time pass " in line]
buckets = ("render", "guest_compute", "bc_decode", "astc_encode", "transfer", "copy", "blit",
           "fb_copy", "clear", "mirror_sync", "other_compute", "land_copy", "land_dep_before",
           "land_dep_after", "cold_guard_compute", "cold_guard_dep", "barrier", "residual")
assert set(lanes) == {"game", "wsi"}, lanes
for key in ("segments", "measured_segments", "missing_queries", "missing_spans", "cap_dropped"):
    assert sum(int(ledger[key]) for ledger in lanes.values()) == int(window[key]), key
for ledger in [window, *lanes.values()]:
    assert isclose(sum(float(ledger[f"{key}_ms"]) for key in buckets), float(ledger["segment_ms"]),
                   abs_tol=0.00002), ledger
assert isclose(sum(float(ledger["segment_ms"]) for ledger in lanes.values()),
               float(window["segment_ms"]), abs_tol=0.00002)
assert isclose(sum(float(size["ms"]) for size in sizes) + float(window["size_unlisted_ms"]),
               float(window["render_ms"]), abs_tol=0.00002)
assert all(float(a["ms"]) >= float(b["ms"]) for a, b in zip(details, details[1:])), details
assert len({(d["segment"], d["span"]) for d in details}) == len(details), details
if sys.argv[2].startswith("cold-"):
    scans = int(window["cold_guard_compute_spans"])
    assert (scans > 0) == (sys.argv[2] == "cold-1"), window
    assert int(window["cold_guard_dep_spans"]) == 2 * scans, window
    assert int(window["missing_queries"]) == int(window["missing_spans"]) == int(window["cap_dropped"]) == 0, window
    sys.exit(0)
if sys.argv[2] == "feedback":
    assert int(window["presents"]) == 0, window
    assert int(window["render_spans"]) == 5, window
    assert int(window["fb_copy_spans"]) == 2 and float(window["fb_copy_ms"]) > 0, window
    assert int(window["missing_spans"]) == int(window["missing_queries"]) == 0, window
    assert sorted((int(top["flags"]), int(top["draws"]), int(top["occurrences"])) for top in tops) == [
        (0, 0, 2), (1, 0, 1), (2, 1, 1), (3, 1, 1),
    ], tops
    stages = [row(line) for line in lines if "nvk-gpu-pass-time shader " in line]
    assert {stage["src_key"] for stage in stages} == {"5000000000000000", "5100000000000000"}, stages
    assert all(len(stage["spirv_digest"]) == 64 and int(stage["spirv_digest"], 16) for stage in stages), stages
    assert len(details) == 5 and all(d["area"] == "0,0,4,4" and d["attachments"] for d in details), details
    assert all(d["programs"] and not d["programs"].startswith("0:") for d in details if d["draws"] == "1"), details
    sys.exit(0)
assert int(window["presents"]) == 2, window
assert int(window["measured_segments"]) == 5, window
assert int(window["missing_queries"]) == int(window["missing_gaps"]) == 0, window
assert int(window["gap_pairs"]) == 4, window
cap = sys.argv[2] == "cap"
assert (int(window["render_spans"]) > 10 if cap else int(window["render_spans"]) == 6), window
assert (int(window["cap_dropped"]) > 0) == cap, window
assert int(window["missing_spans"]) == 0, window
assert int(window["mirror_sync_spans"]) == (1 if cap else 2), window
fixture, = [row(line) for line in lines if "gpu-pass-time fixture " in line]
assert 1 <= int(fixture["pools"]) <= 3 and int(fixture["writes"]) > 0, fixture
assert float(window["render_ms"]) > 0, window
assert float(window["blit_ms"]) > 0, window
assert float(window["copy_ms"]) > 0, window
if not cap:
    assert sorted(float(top["per_present"]) for top in tops) == [1.0, 2.0], tops
    assert all(int(window[key]) == 2 for key in ("land_copy_spans", "land_dep_before_spans", "land_dep_after_spans")), window
    assert float(window["land_copy_ms"]) > 0, window
assert (int(window["land_bytes"]), int(window["land_batches"]), int(window["land_regions"])) == (16, 4, 4), window
assert {top["attachments"].split("/")[2] for top in tops} == {"0", "1"}, tops
assert all(top["size"] == "64x64" and top["draws"] == "0" for top in tops), tops
assert abs(sum(float(top["ms_per_present"]) for top in tops) - float(window["render_ms"])) < 0.00001
assert len(details) == (10 if cap else 6), details
assert all(d["present"] in {"1", "2"} and d["lane"] == "game" and d["size"] == "64x64"
           and d["area"] == "0,0,64,64" and d["attachments"] for d in details), details
assert int(lanes["wsi"]["measured_segments"]) == 2 and int(lanes["game"]["measured_segments"]) == 3, lanes
assert abs(float(window["segment_ms"]) + float(window["gap_ms"]) -
           float(window["overlap_ms"]) - float(window["total_ms"])) < 0.000003
