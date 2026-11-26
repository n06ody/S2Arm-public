#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Join successful Vulkan submissions to the input texel captured before each draw."""
import json
from pathlib import Path
import sys


def last_writers(rows):
    submissions = {}
    writes = {}
    inputs = []
    for row in rows:
        event = row.get("event")
        if event == "volume_submit":
            submissions[row["frame"], row["cmd"]] = int(row["signal"])
        elif event == "volume_write":
            key = row["frame"], row["image"], row["mip"]
            writes.setdefault(key, []).append(row)
        elif event == "volume_input":
            inputs.append(row)

    for read in inputs:
        key = read["frame"], read["image"], read["mip"]
        point = tuple(map(int, read["texel"].split(",")))
        current = int(read["signal"]), int(read["order"])
        best, writer = None, None
        matched = submissions.get((read["frame"], read["cmd"])) == current[0]
        for candidate in writes.get(key, ()):
            signal = submissions.get((candidate["frame"], candidate["cmd"]))
            if signal is None:
                continue
            order = signal, int(candidate["order"])
            origin = map(int, candidate["origin"].split(","))
            extent = map(int, candidate["extent"].split(","))
            if order >= current or (best is not None and order <= best):
                continue
            if all(start <= pos < start + size for pos, start, size in zip(point, origin, extent)):
                best, writer = order, candidate
        if not matched:
            status = "input_submission_unconfirmed"
        elif writer is None:
            status = "prior_frame_or_unobserved"
        elif writer["kind"] == "dispatch":
            # A writable view proves a dispatch could write the texel. Its
            # shader may have skipped the store at this particular coordinate.
            status = "dispatch_candidate"
        else:
            status = "covered_write"
        yield {"input": read, "status": status, "writer": writer if matched else None,
               "writer_signal": best[0] if matched and best is not None else None}


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: roi_volume_writers.py LOG")
    with Path(sys.argv[1]).open(errors="replace") as stream:
        rows = (dict(field.split("=", 1) for field in line.split() if "=" in field)
                for line in stream if "event=volume_" in line)
        for result in last_writers(rows):
            print(json.dumps(result))
