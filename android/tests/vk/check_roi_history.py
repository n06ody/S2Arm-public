"""Check the exact upload history available when frame 2 samples its 3D image."""
import sys
from pathlib import Path


def events(path):
    for line in Path(path).read_text().splitlines():
        prefix, separator, payload = line.partition("[roi] ")
        if separator:
            yield dict(item.split("=", 1) for item in payload.split() if "=" in item)


def check(off_path, on_path):
    if any(row.get("event") == "volume_history" for row in events(off_path)):
        sys.exit("3D history appeared with only the one-frame probe enabled")
    rows = list(events(on_path))
    sample, = [r for r in rows if r.get("event") == "volume_input"]
    history = [r for r in rows if r.get("event") == "volume_history" and
               r.get("image") == sample["image"] and r.get("kind") == "upload"]
    # t_roi_probe.inc uploads twice per frame. The first three inputs are
    # entirely zero. Only the fourth has the nonzero mark in slice 2.
    expected = [("1", "1", "1"), ("1", "1", "1"), ("2", "2", "1"), ("2", "2", "0")]
    completed = {r["segment"]: r["signal"] for r in rows
                 if r.get("event") == "history_submit" and r.get("outcome") == "complete"}
    observed = [(r.get("writer_frame"), completed.get(r["segment"]), r.get("source_all_zero")) for r in history]
    if observed != expected:
        sys.exit(f"3D sampled storage upload history: expected {expected}, got {observed}")
    versions = [int(r["seq"]) for r in history]
    if any(b <= a for a, b in zip(versions, versions[1:])):
        sys.exit(f"3D upload versions were reused or out of order: {versions}")
    if any(r["mip"] != "0" or r["extent"] != "4,4,4" for r in history):
        sys.exit("3D upload history reported the wrong subresource")

    if history[-1]["first_nonzero"] != "305":
        sys.exit("3D source check missed the only nonzero texel in slice 2")
    guest = [r for r in rows if r.get("event") == "texture" and r.get("serial") in ("700","701")]
    if [r["action"] for r in guest] != ["create","alias"] or guest[0]["image"] != guest[1]["image"]:
        sys.exit(f"guest serials lost their shared storage: {guest}")
    if any(r["offset"] != "128" or r["generation"] != guest[0]["generation"] for r in guest):
        sys.exit("guest alias has wrong range or generation")
    raw, = [r for r in rows if r.get("event") == "upload_source" and r["image"] == guest[0]["image"]]
    if (raw["first_nonzero"], raw["first_value"], raw["reason"]) != ("53", "17", "initial_storage"):
        sys.exit(f"guest pool snapshot disagrees with the bytes supplied: {raw}")
    dispatch, = [r for r in rows if r.get("event") == "cs_record"]
    out, = [r for r in rows if r.get("event") == "cs_output"]
    rejected, = [r for r in rows if r.get("event") == "cs_rejected"]
    writes = [r for r in rows if r.get("event") == "volume_history" and r["kind"] == "dispatch"]
    if (len(writes) != 1 or writes[0]["image"] != out["image"] or rejected["reason"] != "zero_groups" or
            dispatch["groups"] != "1,1,2" or int(dispatch["module"],16) == 0 or
            completed.get(out["segment"]) != "1" or out["image"] == sample["image"]):
        sys.exit("wrong 3D dispatch/skip/storage attribution")
    print("3D history: two frames, four uploads, guest pool alias, first nonzero and real CS/skip verified")


if __name__ == "__main__":
    check(*sys.argv[1:])
