#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# Adapted from PR #209 (wdjwxh); main integration for #208.
"""Android 电池侧原始采样及同 MARK 窗口诊断；不冒充整机功率。"""
import argparse
import csv
import json
from pathlib import Path
import re
import subprocess
import time

FIELDS = ("boot_us", "status", "current_ua", "voltage_uv", "energy_uwh", "charge_uah", "temp_tenths_c",
          "source", "ac_powered", "usb_powered", "wireless_powered", "dock_powered")
SYSFS = {"current_now": "current_ua", "voltage_now": "voltage_uv", "energy_now": "energy_uwh",
         "charge_counter": "charge_uah", "temp": "temp_tenths_c", "status": "status"}


def parse_sample(text):
    raw = dict(line.split("=", 1) for line in text.splitlines() if "=" in line)
    row = {key: "" for key in FIELDS}
    for source, key in {"boot_us": "boot_us", **SYSFS}.items():
        value = raw.get(source, "").strip()
        if key == "status":
            row[key] = value if value in ("Charging", "Discharging", "Full", "Not charging", "Unknown") else ""
        elif re.fullmatch(r"-?[0-9]+", value):
            row[key] = int(value)
    row["source"] = "sysfs"
    if row["current_ua"] == "" or row["voltage_uv"] == "":
        # BatteryService current_now is uA; dumpsys battery voltage is mV.
        # Parse only exact field lines, never the historical broadcast voltage.
        current = text.partition("service_current_now=")[2].partition("\nservice_dump_begin")[0].strip()
        dump = text.partition("service_dump_begin\n")[2].partition("service_dump_end")[0]
        fields = dict(re.findall(r"^[ \t]*([A-Za-z ]+):[ \t]*([^\r\n]+)$", dump, re.M))
        voltage = fields.get("voltage", "").strip()
        if re.fullmatch(r"-?[0-9]+", current) and re.fullmatch(r"[0-9]+", voltage):
            row["current_ua"], row["voltage_uv"] = int(current), int(voltage) * 1000
            row["source"] = "battery-service"
            row["status"] = "Unknown"
            for source, key in (("AC powered", "ac_powered"), ("USB powered", "usb_powered"),
                                ("Wireless powered", "wireless_powered"), ("Dock powered", "dock_powered")):
                value = fields.get(source, "").strip()
                row[key] = value if value in ("true", "false") else ""
            if fields.get("status", "").strip() == "3" and all(row[key] == "false" for key in FIELDS[-4:]):
                row["status"] = "Discharging"
            elif fields.get("status", "").strip() == "2" or any(row[key] == "true" for key in FIELDS[-4:]):
                row["status"] = "Charging"
            temp = fields.get("temperature", "").strip()
            if re.fullmatch(r"-?[0-9]+", temp): row["temp_tenths_c"] = int(temp)
            # AOSP BatteryService.dumpInternal marks set/unplug simulation with
            # UPDATES STOPPED. Keep the numbers as raw evidence, never integrate
            # that frozen service state as a measured discharge sample.
            if "UPDATES STOPPED" in dump:
                row["status"] = "Unknown"
                row["source"] = "battery-service-frozen"
        else:
            row["source"] = "unavailable"
    return row


def sample_command(supply):
    if not re.fullmatch(r"[a-zA-Z0-9_-]+", supply):
        raise ValueError("非法 power_supply 名称")
    return ("good=1; for n in status current_now voltage_now energy_now charge_counter temp; do "
            "v=$(cat /sys/class/power_supply/" + supply + "/\"$n\" 2>/dev/null); "
            "printf '%s=%s\\n' \"$n\" \"$v\"; "
            "if [ \"$n\" = current_now ] || [ \"$n\" = voltage_now ]; then "
            "case \"$v\" in ''|*[!0-9-]*) good=0;; esac; fi; done; "
            "if [ \"$good\" = 0 ]; then printf 'service_current_now='; "
            "cmd battery get -f current_now 2>/dev/null; printf '\\nservice_dump_begin\\n'; "
            "dumpsys battery 2>/dev/null; printf '\\nservice_dump_end\\n'; fi; "
            "printf 'boot_us='; awk '{printf \"%.0f\\n\", $1*1000000}' /proc/uptime")


def battery_window(rows, start_us, end_us, discharge_sign=None):
    """电流符号必须由设备验证后显式指定；仅计算未充电的电池侧梯形积分。"""
    result = {"scope": "battery", "comparableToIosWholeSystem": False, "averageW": None,
              "dischargeSign": discharge_sign,
              "reason": "缺少 MARK/收局时钟、电流符号或完整未充电采样窗口"}
    if start_us is None or end_us is None or start_us < 0 or end_us <= start_us or discharge_sign not in (-1, 1):
        return result
    values = []
    try:
        for row in rows:
            timestamp = int(row["boot_us"])
            current = int(row["current_ua"]) * discharge_sign
            voltage = int(row["voltage_uv"])
            values.append((timestamp, current, voltage, row["status"]))
    except (ValueError, KeyError, TypeError):
        result["reason"] = "有缺失/不可读的电池原始值，未补零"
        return result
    if len(values) < 2 or any(b[0] <= a[0] for a, b in zip(values, values[1:])):
        result["reason"] = "电池采样时间重复、倒退或不足"
        return result
    energy_ws, covered_us = 0.0, 0
    for a, b in zip(values, values[1:]):
        lo, hi = max(start_us, a[0]), min(end_us, b[0])
        if hi <= lo:
            continue
        if b[0] - a[0] > 3_000_000 or any(v[3] != "Discharging" or v[1] < 0 or not 0 < v[2] <= 30_000_000 for v in (a, b)):
            result["reason"] = "窗口内存在充电/未知状态、符号不符、非法电压或超过3秒采样空洞"
            return result
        pa, pb = a[1] * a[2] / 1e12, b[1] * b[2] / 1e12
        left = pa + (pb - pa) * (lo - a[0]) / (b[0] - a[0])
        right = pa + (pb - pa) * (hi - a[0]) / (b[0] - a[0])
        energy_ws += (left + right) / 2 * (hi - lo) / 1e6
        covered_us += hi - lo
    if covered_us != end_us - start_us:
        result["reason"] = "采样未覆盖完整 MARK 到收局窗口"
        return result
    result.update(averageW=energy_ws / (covered_us / 1e6), durationSeconds=covered_us / 1e6,
                  energyJ=energy_ws, reason="电池侧电流×电压积分，仅作诊断；未经设备校准，不用于3.7W整机判格")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=90)
    parser.add_argument("--supply", default="battery")
    args = parser.parse_args()
    if not 1 <= args.seconds <= 7200:
        parser.error("seconds 必须在1到7200之间")
    command = sample_command(args.supply)
    try:
        if args.output.exists() or Path(str(args.output) + ".source.json").exists():
            raise FileExistsError("输出或来源文件已存在，请为本次采样指定新路径")
        source = {"version": 1, "scope": "battery", "clock": "CLOCK_BOOTTIME", "serial": args.serial,
                  "source": "sysfs with BatteryService fallback; per-row source column",
                  "sysfs": f"/sys/class/power_supply/{args.supply}",
                  "fallback": "cmd battery get -f current_now + dumpsys battery voltage (mV multiplied by 1000)",
                  "sampleTime": "device uptime at end of sample; sequential current/voltage reads, not simultaneous",
                  "units": {"current_ua": "uA", "voltage_uv": "uV"}}
        with Path(str(args.output) + ".source.json").open("x", encoding="utf-8") as stream:
            json.dump(source, stream)
        with args.output.open("x", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, FIELDS, delimiter="\t")
            writer.writeheader(); stream.flush()
            deadline = time.monotonic() + args.seconds
            while time.monotonic() < deadline:
                start = time.monotonic()
                capture = subprocess.run([args.adb, "-s", args.serial, "shell", command],
                                         capture_output=True, text=True, timeout=10, check=True)
                row = parse_sample(capture.stdout)
                if row["boot_us"] == "":
                    raise ValueError("设备未返回启动时钟；不能对齐窗口")
                writer.writerow(row); stream.flush()
                time.sleep(max(0, 1 - (time.monotonic() - start)))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(2, f"电池采样失败（已采样部分保留）：{error}\n")
    print(json.dumps({"output": str(args.output), "scope": "battery", "clock": "CLOCK_BOOTTIME",
                      "source": "per-row sysfs/battery-service/unavailable", "missingValues": "空字段，不补零"}, ensure_ascii=False))


if __name__ == "__main__":
    main()
