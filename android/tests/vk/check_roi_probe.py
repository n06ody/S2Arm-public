"""Compare samples emitted by the one-frame probe with the fixture's known texels."""
import sys
from pathlib import Path


def records(path):
    rows = []
    for line in Path(path).read_text().splitlines():
        if "[roi] " in line:
            rows.append(dict(item.split("=", 1) for item in line.split("[roi] ", 1)[1].split() if "=" in item))
    return rows


if records(sys.argv[1]):
    sys.exit("ROI disabled: emitted records")
rows = records(sys.argv[2])
samples = [r for r in rows if r.get("event") == "sample"]
if not samples:
    sys.exit("ROI frame=2: missing GPU pixel samples after three successful presents")
if any((r.get("frame"), r.get("x"), r.get("y")) != ("2", "3", "5") for r in samples):
    sys.exit("ROI sampled an adjacent frame or wrong pixel")


def require(site, channel, values):
    for r in samples:
        if r.get("site") == site and channel in r:
            if tuple(float(v) for v in r[channel].split(",")) == values:
                return
    sys.exit(f"ROI frame=2 {site}: missing {channel}={values}")


require("clear", "rgba", (0, 0, 0, 1))
require("clear", "depth", (0.75,))
require("pass_end", "rgba", (2, -0.5, 0.25, 1))
require("pass_end", "depth", (0.125,))
require("present", "rgba", (0, 0, 1, 1))
actual = {
    line.split("=")[-1]
    for line in Path(sys.argv[2]).read_text().splitlines()
    if line.startswith("ROI actual sampled view=")
}
bindings = [r for r in rows if r.get("event") == "binding"]
if {(r["type"], r["zero"]) for r in bindings} != {("1","0"),("2","0"),("3","1"),("5","0")}:
    sys.exit(f"ROI failed to distinguish 2D/3D/typed zero cube/2D array: {bindings}")
for b in bindings:
    if b["view"] not in actual or b["set"] != "0" or b["binding"] != "0" or b["format"] == "0":
        sys.exit(f"ROI did not report a sampler view actually written to the descriptor: {b}")
draws = [r for r in rows if r.get("event") == "draw"]
depth_draws = [r for r in draws if r["depthTest"] == "1"]
if len(draws) != 6 or len(depth_draws) != 4 or any((r["depthCompareOp"], r["depthClamp"], r["depthClip"]) != ("4","1","0") for r in depth_draws):
    sys.exit(f"ROI wrong GREATER/depth clamp pipeline states: {draws}")
if any(r.get("event") == "gap" for r in rows):
    sys.exit("ROI unexpectedly incomplete")
print("ROI driver descriptors: 2D, 3D, replay zero cube, 2D array; GREATER+depth clamp passed")
print("ROI one frame: clear, HDR/depth pass end, pre-present, adjacent pixel and disabled controls passed")

# The volume draw uses a distinct shader, a 64x64 viewport,
# and a 4x4x4 volume. Reporting the screen coordinate as source UV, reading a
# neighbouring Z slice, or returning the pre-overwrite version must fail.
target, = [r for r in draws if r["pass"] == "3" and r["depthTest"] == "0"]
def target_rows(event):
    return [r for r in rows if r.get("event") == event and
            all(r.get(k) == target[k] for k in ("frame", "pass", "draw"))]

bound, = [r for r in target_rows("binding") if r["type"] == "2"]
def floats(s):
    return tuple(map(float, s.split(",")))

expected = (0.5, -0.25, 2.0, 1.0)
volume, = target_rows("volume_input") or sys.exit("ROI missing representative texel at selected draw")
if (volume["image"] != bound["image"] or volume["view"] != bound["view"] or volume["texel"] != "2,1,2" or
        floats(volume["rgba"]) != expected):
    sys.exit(f"ROI wrong volume input {volume}")
writers = [r for r in rows if r.get("event") == "volume_write" and r["cmd"] == volume["cmd"] and
           r["image"] == bound["image"] and r["mip"] == volume["mip"] and int(r["order"]) < int(volume["order"])]
if len(writers) != 2 or any(r["kind"] != "upload" or r["extent"] != "4,4,4" for r in writers):
    sys.exit(f"ROI missing successive uploads of the true input volume: {writers}")
if not any(r.get("event") == "volume_submit" and r["cmd"] == volume["cmd"] and
           r["signal"] == volume["signal"] for r in rows):
    sys.exit("ROI unsignaled input volume")
print("ROI successive upload versions, actual volume descriptor and signed representative texel passed")
if not any(r.get("view") == bound["view"] and r.get("pixel") == "0,1" and
           floats(r.get("coord", "nan")) == (0.5 + 1.5/64, 0.25 + 0.5/64, 0.625) and
           floats(r.get("rgba", "nan")) == expected for r in target_rows("shader_sample")):
    sys.exit("ROI missing executed fragment sample coordinates or signed pre-output value")
if not any(r.get("set") == "0" and r.get("binding") == "2" and
           r.get("offset") == "140" and float(r.get("value", "nan")) == 0.25
           for r in target_rows("shader_constant")):
    sys.exit("ROI missing actual fragment constant load at byte 140")

uv, = [r for r in rows if r.get("event") == "shader_sample" and r["pass"] == "4"]
source, = [r for r in bindings if r["pass"] == "4"]
if (uv["frame"], uv["pixel"], uv["view"], floats(uv["coord"])) != (
        "2", "3,5", source["view"], (3.5 / 64, 5.5 / 64)):
    sys.exit(f"ROI failed downstream screen-to-source UV mapping: {uv}")
if tuple(int(16 * v) for v in floats(uv["coord"])) != (0, 1):
    sys.exit("ROI source fragment does not correspond to the downstream UV")
print("ROI real 2D UV, upstream 3D sample coordinates, signed float return and scalar load passed")

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from roi_volume_writers import last_writers

case, = last_writers(rows)
last = max(writers, key=lambda w: int(w["order"]))
if case["status"] != "covered_write" or case["writer"] != last:
    sys.exit("ROI resolved the wrong upload version")
unsubmitted = [r for r in rows if r.get("event") != "volume_submit"]
if next(last_writers(unsubmitted))["status"] != "input_submission_unconfirmed":
    sys.exit("ROI accepted an unconfirmed submission")
prior = [r for r in rows if r.get("event") != "volume_write"]
if next(last_writers(prior))["status"] != "prior_frame_or_unobserved":
    sys.exit("ROI invented a writer from a previous frame")
