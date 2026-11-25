# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
import sys
from pathlib import Path


def row(line):
    return dict(field.split("=", 1) for field in line.split() if "=" in field)


lines = Path(sys.argv[1]).read_text().splitlines()
window, = [row(line) for line in lines if "nvk-gpu-pass-time window " in line]
tops = [row(line) for line in lines if "nvk-gpu-pass-time top " in line]
if sys.argv[2] == "feedback":
    assert int(window["presents"]) == 0, window
    assert int(window["render_spans"]) == 5, window
    assert int(window["fb_copy_spans"]) == 2 and float(window["fb_copy_ms"]) > 0, window
    assert int(window["missing_spans"]) == int(window["missing_queries"]) == 0, window
    assert sorted((int(top["flags"]), int(top["draws"]), int(top["occurrences"])) for top in tops) == [
        (0, 0, 2), (1, 0, 1), (2, 1, 1), (3, 1, 1),
    ], tops
    sys.exit(0)
assert int(window["presents"]) == 2, window
assert int(window["measured_segments"]) == 5, window
assert int(window["missing_queries"]) == int(window["missing_gaps"]) == 0, window
assert int(window["gap_pairs"]) == 4, window
cap = sys.argv[2] == "cap"
assert int(window["render_spans"]) == (258 if cap else 6), window
assert int(window["cap_dropped"]) == (6 if cap else 0), window
assert int(window["missing_spans"]) == 0, window
assert int(window["mirror_sync_spans"]) == (1 if cap else 2), window
fixture, = [row(line) for line in lines if "gpu-pass-time fixture " in line]
assert 1 <= int(fixture["pools"]) <= 3 and int(fixture["writes"]) > 0, fixture
assert float(window["render_ms"]) > 0, window
assert float(window["blit_ms"]) > 0, window
assert float(window["copy_ms"]) > 0, window
assert sorted(float(top["per_present"]) for top in tops) == ([0.5, 128.5] if cap else [1.0, 2.0]), tops
assert {top["attachments"].split("/")[2] for top in tops} == {"0", "1"}, tops
assert all(top["size"] == "64x64" and top["draws"] == "0" for top in tops), tops
assert abs(sum(float(top["ms_per_present"]) for top in tops) - float(window["render_ms"])) < 0.00001
buckets = ("render", "guest_compute", "bc_decode", "astc_encode", "transfer", "copy", "blit",
           "fb_copy", "clear", "mirror_sync", "other_compute", "residual")
assert abs(sum(float(window[key + "_ms"]) for key in buckets) - float(window["segment_ms"])) < 0.00002
assert abs(float(window["segment_ms"]) + float(window["gap_ms"]) -
           float(window["overlap_ms"]) - float(window["total_ms"])) < 0.000003
