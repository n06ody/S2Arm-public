#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# Adapted from PR #209 (wdjwxh); main integration for #208.
"""Android 电池侧原始采样及同窗诊断；不冒充整机功率。

判读口径照搬 switch-gpu-mapper 的 docs/power-comparison.md + scripts/analyze_power.py：
- 电流取幅值积分（abs(current)×voltage 梯形），原始符号留在 rawCurrentSign——
  部分 OEM 放电为正；窗口内符号翻转即作废。
- 分析窗对每块剔首 15s / 尾 5s（trim_start_us / trim_end_us）。
- 窗内每个采样点都必须物理放电（plugged==0 且 status=Discharging）、屏幕交互、
  非省电、thermal_status<3、电流/电压/电量计读数合法、采样间隔 ≤3s、
  窗口边界有覆盖——充电 / 掉线 / 降频即整块作废（fail closed 无数据）。
- charge counter 整局交叉核对：等差递减量子 q → 端点差 ±q 区间；细粒计数走
  0.5–1.5 比值门；向下跳变 <2 次 → 不证。核对不过 = 功耗判读未证，不是绝对标定。
"""
import argparse
import csv
import json
from pathlib import Path
import re
import subprocess
import time

FIELDS = ("boot_us", "status", "current_ua", "voltage_uv", "energy_uwh", "charge_uah", "temp_tenths_c",
          "source", "ac_powered", "usb_powered", "wireless_powered", "dock_powered",
          "plugged", "interactive", "power_save", "thermal_status")
SYSFS = {"current_now": "current_ua", "voltage_now": "voltage_uv", "energy_now": "energy_uwh",
         "charge_counter": "charge_uah", "temp": "temp_tenths_c", "status": "status"}
POWERED_FLAGS = ("ac_powered", "usb_powered", "wireless_powered", "dock_powered")
BLOCK_SEP = "==="


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
    # 屏幕交互 / 省电 / 热档（dumpsys power / thermalservice）：整块作废门的证据。
    for key in ("interactive", "power_save"):
        value = raw.get(key, "").strip()
        row[key] = "1" if value == "true" else "0" if value == "false" else ""
    value = raw.get("thermal_status", "").strip()
    row["thermal_status"] = int(value) if re.fullmatch(r"[0-9]+", value) else ""
    # 每次采样都带 dumpsys 旗标段：sysfs 行也只有拿到四路 powered 旗标，
    # battery_window 才能证「未接电」（旗标缺失/未知 fail closed）。
    # UPDATES STOPPED 是模拟置/拔电的冻结态：旗标不可证，留空。
    dump = text.partition("service_dump_begin\n")[2].partition("service_dump_end")[0]
    fields = dict(re.findall(r"^[ \t]*([A-Za-z ]+):[ \t]*([^\r\n]+)$", dump, re.M))
    if "UPDATES STOPPED" not in dump:
        for source, key in (("AC powered", "ac_powered"), ("USB powered", "usb_powered"),
                            ("Wireless powered", "wireless_powered"), ("Dock powered", "dock_powered")):
            value = fields.get(source, "").strip()
            row[key] = value if value in ("true", "false") else ""
    if row["current_ua"] == "" or row["voltage_uv"] == "":
        # BatteryService current_now is uA; dumpsys battery voltage is mV.
        # Parse only exact field lines, never the historical broadcast voltage.
        current = text.partition("service_current_now=")[2].partition("\nservice_dump_begin")[0].strip()
        voltage = fields.get("voltage", "").strip()
        if re.fullmatch(r"-?[0-9]+", current) and re.fullmatch(r"[0-9]+", voltage):
            row["current_ua"], row["voltage_uv"] = int(current), int(voltage) * 1000
            row["source"] = "battery-service"
            # BatteryManager status 码照实映射、供电旗标留原列，不坍缩（8E 首局
            # status=4 NOT_CHARGING + AC powered=true 曾被映成 Charging 丢真值）；
            # 状态与旗标矛盾时两边都留原文，接没接电由判读者看旗标列，这里不猜。
            row["status"] = {"2": "Charging", "3": "Discharging", "4": "Not charging",
                             "5": "Full"}.get(fields.get("status", "").strip(), "Unknown")
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
    flags = [row[key] for key in POWERED_FLAGS]
    row["plugged"] = ("1" if "true" in flags else
                      "0" if all(value == "false" for value in flags) else "")
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
            "cmd battery get -f current_now 2>/dev/null; printf '\\n'; fi; "
            "printf 'service_dump_begin\\n'; dumpsys battery 2>/dev/null; printf '\\nservice_dump_end\\n'; "
            "dp=$(dumpsys power 2>/dev/null); "
            "printf 'interactive=%s\\n' \"$(printf '%s' \"$dp\" | sed -n 's/.*mInteractive=\\([a-z]*\\).*/\\1/p' | head -1)\"; "
            "printf 'power_save=%s\\n' \"$(printf '%s' \"$dp\" | sed -n 's/.*mPowerSaveMode=\\([a-z]*\\).*/\\1/p' | head -1)\"; "
            "printf 'thermal_status=%s\\n' \"$(dumpsys thermalservice 2>/dev/null | "
            "sed -n 's/.*Current Status:[ ]*\\([0-9]*\\).*/\\1/p' | head -1)\"; "
            "printf 'boot_us='; awk '{printf \"%.0f\\n\", $1*1000000}' /proc/uptime")


def _num(row, key):
    try:
        return int(str(row.get(key, "")).strip())
    except (TypeError, ValueError):
        return None


def discharging(row):
    """物理放电证据行：BatteryService 放电状态 + 四路供电旗标全证 false（plugged==0）。"""
    return row.get("status") == "Discharging" and str(row.get("plugged")) == "0"


def discharge_streak_us(rows, end_us=None):
    """截至 end_us（缺省=最后一条观测）尾部那一串「连续物理放电」采样覆盖的时长。
    尾部末行须在 end_us 的 3s 内且行间隔 ≤3s、行序单调；断链即只计尾段。"""
    obs = sorted((_num(r, "boot_us"), r) for r in rows if _num(r, "boot_us") is not None)
    if not obs:
        return 0
    if end_us is None:
        end_us = obs[-1][0]
    pre = [(b, r) for b, r in obs if b <= end_us]
    if not pre or end_us - pre[-1][0] > 3_000_000 or not discharging(pre[-1][1]):
        return 0
    span, prev = 0, pre[-1]
    for b, r in reversed(pre[:-1]):
        if not discharging(r) or prev[0] - b > 3_000_000:
            break
        span += prev[0] - b
        prev = (b, r)
    return span


def _slope(points):
    """最小二乘斜率（x 单位 µs → 每 µs 值变化）。"""
    xs = [x for x, _ in points]
    ys = [y for _, y in points]
    xm, ym = sum(xs) / len(xs), sum(ys) / len(ys)
    denom = sum((x - xm) ** 2 for x in xs)
    return sum((x - xm) * (y - ym) for x, y in points) / denom if denom else 0.0


def battery_window(rows, begin_us, end_us, check_charge=True,
                   trim_start_us=15_000_000, trim_end_us=5_000_000):
    """电池侧梯形积分：分析窗 = [begin+trim_start, end-trim_end]（每块剔首 15s/尾 5s）。
    返回 dict：averageW/energyJ/durationSeconds + rawCurrentSign + chargeCounterW/
    chargeToCurrentRatio + samples + tempRangeC + reason；任一作废门不过 → averageW=None。"""
    result = {"scope": "battery", "comparableToIosWholeSystem": False, "averageW": None,
              "rawCurrentSign": None, "reason": ""}

    def fail(reason):
        result["reason"] = reason
        return result

    if begin_us is None or end_us is None:
        return fail("缺少窗口边界时钟")
    begin, end = begin_us + trim_start_us, end_us - trim_end_us
    if end <= begin:
        return fail("剔首尾后分析窗为空（块太短）")
    obs = [{"boot": _num(r, "boot_us"), "current": _num(r, "current_ua"),
            "voltage": _num(r, "voltage_uv"), "charge": _num(r, "charge_uah"),
            "status": r.get("status"), "plugged": str(r.get("plugged") or ""),
            "interactive": str(r.get("interactive") or ""), "power_save": str(r.get("power_save") or ""),
            "thermal": _num(r, "thermal_status"), "temp": _num(r, "temp_tenths_c"),
            "raw": r} for r in rows]
    if len(obs) < 2 or any(o["boot"] is None for o in obs):
        return fail("boot_us 缺行/不可读")
    if any(b["boot"] <= a["boot"] for a, b in zip(obs, obs[1:])):
        return fail("电池采样时间重复或倒退")
    pairs = [(a, b) for a, b in zip(obs, obs[1:]) if a["boot"] < end and b["boot"] > begin]
    if not pairs or pairs[0][0]["boot"] > begin or pairs[-1][1]["boot"] < end:
        return fail("窗口边界无采样覆盖")
    points = [pairs[0][0]] + [b for a, b in pairs]
    signs = set()
    for o in points:
        if not (o["plugged"] == "0" and o["status"] == "Discharging"):
            return fail("窗口内存在接电/非放电行（plugged 或 status 证不出放电，整块作废）")
        if o["interactive"] != "1" or o["power_save"] != "0":
            return fail("屏幕交互/省电态在窗口内变化或读不出（整块作废）")
        if o["thermal"] is None or o["thermal"] >= 3:
            return fail("热档严重降频或读不出（thermal_status≥3，整块作废）")
        if o["current"] is None or not 0 < abs(o["current"]) <= 30_000_000:
            return fail("电流读数缺失或为 0 或越界（整块作废）")
        if o["voltage"] is None or not 2_500_000 <= o["voltage"] <= 10_000_000:
            return fail("电压读数缺失或越界 2.5–10V（整块作废）")
        if o["charge"] is None or o["charge"] <= 0:
            return fail("电量计读数缺失（charge_counter≤0，整块作废）")
        signs.add(1 if o["current"] > 0 else -1)
    if len(signs) != 1:
        return fail("窗口内电流符号翻转（放电方向不自洽，整块作废）")
    energy_ws, covered = 0.0, 0
    for a, b in pairs:
        if b["boot"] - a["boot"] > 3_000_000:
            return fail("超过3秒采样空洞（掉线，整块作废）")
        lo, hi = max(begin, a["boot"]), min(end, b["boot"])
        wa, wb = abs(a["current"]) * a["voltage"] / 1e12, abs(b["current"]) * b["voltage"] / 1e12
        wl = wa + (wb - wa) * (lo - a["boot"]) / (b["boot"] - a["boot"])
        wr = wa + (wb - wa) * (hi - a["boot"]) / (b["boot"] - a["boot"])
        energy_ws += (wl + wr) / 2 * (hi - lo) / 1e6
        covered += hi - lo
    if covered != end - begin:
        return fail("采样未覆盖完整分析窗")
    watts = energy_ws / (covered / 1e6)
    charge_slope = _slope([(o["boot"], o["charge"]) for o in points])
    charge_w = -charge_slope * sum(o["voltage"] for o in points) / len(points) * 3.6e-3
    ratio = charge_w / watts if watts else None
    if check_charge and not (ratio is not None and 0.5 <= ratio <= 1.5):
        return fail(f"电流积分与电量计斜率趋势不合（ratio={ratio}，要 0.5–1.5）")
    temps = [o["temp"] for o in points if o["temp"] is not None]
    result.update(averageW=watts, energyJ=energy_ws, durationSeconds=covered / 1e6,
                  rawCurrentSign=next(iter(signs)), samples=len(points),
                  chargeCounterW=charge_w, chargeToCurrentRatio=ratio,
                  tempRangeC=[min(temps) / 10, max(temps) / 10] if temps else None,
                  reason="电池侧电流幅值×电压梯形积分（剔首尾），未经外部校准")
    return result


def charge_corroboration(rows):
    """整局电量计交叉核对（兄弟项目 charge_corroboration 同式移植）：
    电量计须单调下降；≥2 次等差递减 → 量子 q，端点差 ±q 区间收电流积分；
    否则走整局 0.5–1.5 比值门（也要 ≥2 次向下跳变）。这是趋势一致性核对，
    不是绝对功率标定；不 corroborate → 功耗判读未证。"""
    obs = [{"boot": _num(r, "boot_us"), "current": _num(r, "current_ua"),
            "charge": _num(r, "charge_uah"), "row": r} for r in rows]
    base = {"corroborated": False, "precise_calibration": False}
    if len(obs) < 3:
        return {**base, "error": "电量观测不足 3 行"}
    if any(o["boot"] is None or o["charge"] is None or o["current"] is None for o in obs):
        return {**base, "error": "电量/电流/时钟读数缺行"}
    if any(b["boot"] <= a["boot"] for a, b in zip(obs, obs[1:])):
        return {**base, "error": "电量计观测时钟非单调"}
    deltas = [a["charge"] - b["charge"] for a, b in zip(obs, obs[1:])]
    nonzero = [v for v in deltas if v != 0]
    if not nonzero or not all(v > 0 for v in nonzero):
        return {**base, "error": "电量计无单调放电趋势"}
    consumed = obs[0]["charge"] - obs[-1]["charge"]
    integrated = 0.0
    for a, b in zip(obs, obs[1:]):
        seconds = (b["boot"] - a["boot"]) / 1e6
        if not 0 < seconds <= 3:
            return {**base, "error": "电量计观测采样间隔 >3s（掉线）"}
        if not (discharging(a["row"]) and discharging(b["row"])):
            return {**base, "error": "电量计核对要求整段物理放电（有接电/非放电行）"}
        integrated += (abs(a["current"]) + abs(b["current"])) / 2 * seconds / 3600
    out = {**base, "transitions": len(nonzero), "consumed_uah": consumed,
           "integrated_current_uah": integrated}
    if len(nonzero) >= 2 and len(set(nonzero)) == 1:
        quantum = nonzero[0]
        low, high = max(0, consumed - quantum), consumed + quantum
        return {**out, "method": "observed_constant_quantum_endpoint_interval",
                "quantum_uah": quantum, "counter_interval_uah": [low, high],
                "corroborated": low <= integrated <= high}
    ratio = consumed / integrated if integrated else None
    return {**out, "method": "whole_run_charge_change", "charge_to_current_ratio": ratio,
            "corroborated": len(nonzero) >= 2 and ratio is not None and 0.5 <= ratio <= 1.5}


def power_log_blocks(text):
    """设备侧采样文件 → 完整采样块列表（=== 分隔；末段未封闭的尾巴不算）。"""
    return [block for block in text.split("\n" + BLOCK_SEP + "\n")[:-1]]


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
                  "fields": {key: "raw device report" for key in FIELDS}}
        with args.output.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, ["t", *FIELDS], delimiter="\t")
            writer.writeheader()
            for index in range(args.seconds):
                tick = time.monotonic()
                row = parse_sample(subprocess.run([args.adb, "-s", args.serial, "shell", command],
                                                  capture_output=True, text=True, timeout=20).stdout)
                writer.writerow({"t": index, **row})
                stream.flush()
                time.sleep(max(0, 1 - (time.monotonic() - tick)))
        Path(str(args.output) + ".source.json").write_text(json.dumps(source, indent=2) + "\n", encoding="utf-8")
        print(f"采完 {args.seconds}s → {args.output}（{args.supply}，判读口径见模块 docstring）")
    except (OSError, subprocess.SubprocessError) as error:
        parser.exit(2, f"采样失败：{error}\n")


if __name__ == "__main__":
    main()
