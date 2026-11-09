#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# Adapted from PR #209 (wdjwxh); main integration for #208.
"""消费真实 C# 生产者的合成时钟输出；不另造 frames/host 来冒充生产对拍。"""
import csv
import json
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "android"))
from android_power import battery_window
from collect_run import collect, runtime_files


def main():
    source = Path(sys.argv[1])
    captures = [p for p in source.iterdir() if p.is_dir() and (p / "host.log").exists()]
    assert len(captures) == 1, "need one actual producer output with its captured Console.Error"
    source = captures[0]
    host = source / "host.log"
    files = {"host.log": host, **runtime_files(source, host)}
    runtime = json.loads((source / "runtime-metrics.json").read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix="s2a-metrics-pipeline-") as temporary:
        temporary = Path(temporary)
        battery = temporary / "battery.tsv"
        with battery.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, ["boot_us", "status", "current_ua", "voltage_uv",
                                             "ac_powered", "usb_powered", "wireless_powered",
                                             "dock_powered"], delimiter="\t")
            writer.writeheader()
            for timestamp in range(0, runtime["endBootUs"] + 2_000_000, 1_000_000):
                # 供电旗标必须显式 false——battery_window fail closed：缺列/未知不证未接电
                writer.writerow({"boot_us": timestamp, "status": "Discharging",
                                 "current_ua": -1_000_000, "voltage_uv": 4_000_000,
                                 "ac_powered": "false", "usb_powered": "false",
                                 "wireless_powered": "false", "dock_powered": "false"})
        source_info = temporary / "battery-source.json"
        source_info.write_text(json.dumps({"clock": "CLOCK_BOOTTIME", "scope": "battery", "units": {"current_ua": "uA", "voltage_uv": "uV"}}), encoding="utf-8")
        files.update({"battery.tsv": battery, "battery-source.json": source_info})
        directory = temporary / "run"
        collect(directory, files)
        runtime_files(directory, directory / "host.log")
        for name, original in files.items():
            assert (directory / name).read_bytes() == original.read_bytes(), name
        with battery.open(encoding="utf-8", newline="") as stream:
            samples = list(csv.DictReader(stream, delimiter="\t"))
        report = battery_window(samples, runtime["markBootUs"], runtime["endBootUs"], -1)
        assert report["averageW"] == 4.0
        assert report["comparableToIosWholeSystem"] is False
        assert report["durationSeconds"] == (runtime["endBootUs"] - runtime["markBootUs"]) / 1e6
        assert runtime["endBootUs"] > runtime["lastFrameBootUs"], "tail stall is part of the window"
        assert runtime["markFrame"] == 2
        assert runtime["frames"] == 1904
        print("Actual C# producer -> production contract -> atomic raw export: PASS; 1904 rows unchanged, full stop clock, synthetic battery 4W diagnostic; no shared score")


if __name__ == "__main__":
    main()
