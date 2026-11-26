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
if len(draws)!=4 or any((r["depthTest"], r["depthCompareOp"], r["depthClamp"], r["depthClip"])!=("1","4","1","0") for r in draws):
    sys.exit(f"ROI wrong GREATER/depth clamp pipeline states: {draws}")
if any(r.get("event") == "gap" for r in rows):
    sys.exit("ROI unexpectedly incomplete")
print("ROI driver descriptors: 2D, 3D, replay zero cube, 2D array; GREATER+depth clamp passed")
print("ROI one frame: clear, HDR/depth pass end, pre-present, adjacent pixel and disabled controls passed")
