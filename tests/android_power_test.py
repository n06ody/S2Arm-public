#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# Adapted from PR #209 (wdjwxh); main integration for #208.
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from android_power import battery_window, parse_sample, sample_command


class BatteryTest(unittest.TestCase):
    def rows(self):
        return [{"boot_us": i * 1_000_000, "current_ua": -1_000_000, "voltage_uv": 4_000_000,
                 "status": "Discharging", "ac_powered": "false", "usb_powered": "false",
                 "wireless_powered": "false", "dock_powered": "false"} for i in range(66)]

    def test_known_power(self):
        result = battery_window(self.rows(), 2_500_000, 62_500_000, -1)
        self.assertEqual(result["averageW"], 4.0)
        self.assertEqual(result["energyJ"], 240.0)
        self.assertFalse(result["comparableToIosWholeSystem"])

    def test_interpolation_uses_mark_through_actual_stop_not_last_frame(self):
        rows = [{"boot_us": i * 1_000_000, "current_ua": -(i + 1) * 500_000,
                 "voltage_uv": 4_000_000, "status": "Discharging", "ac_powered": "false",
                 "usb_powered": "false", "wireless_powered": "false",
                 "dock_powered": "false"} for i in range(4)]
        # Linear 2/4/6/8 W: MARK=.5 s -> stop=2.5 s is 3..7 W, 10 J / 2 s.
        result = battery_window(rows, 500_000, 2_500_000, -1)
        self.assertEqual((result["averageW"], result["energyJ"], result["durationSeconds"]), (5.0, 10.0, 2.0))
        self.assertNotEqual(battery_window(rows, 500_000, 1_500_000, -1)["averageW"], result["averageW"])

    def test_sign_not_guessed(self):
        for sign in (None, 1):
            self.assertIsNone(battery_window(self.rows(), 2_000_000, 62_000_000, sign)["averageW"])

    def test_charging_not_whole_system(self):
        rows = self.rows(); rows[30]["status"] = "Charging"
        self.assertIsNone(battery_window(rows, 2_000_000, 62_000_000, -1)["averageW"])

    def test_missing_field_not_zero(self):
        rows = self.rows(); rows[30]["current_ua"] = ""
        self.assertIsNone(battery_window(rows, 2_000_000, 62_000_000, -1)["averageW"])

    def test_coverage_required(self):
        self.assertIsNone(battery_window(self.rows()[10:], 2_000_000, 62_000_000, -1)["averageW"])

    def test_sampling_gap(self):
        rows = self.rows(); del rows[20:25]
        self.assertIsNone(battery_window(rows, 2_000_000, 62_000_000, -1)["averageW"])

    def test_duplicate_time(self):
        rows = self.rows(); rows[30]["boot_us"] = rows[29]["boot_us"]
        self.assertIsNone(battery_window(rows, 2_000_000, 62_000_000, -1)["averageW"])

    def test_missing_clock(self):
        self.assertIsNone(battery_window(self.rows(), None, None, -1)["averageW"])

    def test_parser_denied_values(self):
        row = parse_sample("boot_us=1000000\ncurrent_now=?\nvoltage_now=4000000\nstatus=Discharging\n")
        self.assertEqual(row["current_ua"], "")
        self.assertEqual(row["voltage_uv"], 4_000_000)

    def test_shell_path_validation(self):
        self.assertIn("/sys/class/power_supply/battery/", sample_command("battery"))
        with self.assertRaises(ValueError):
            sample_command("battery;id")

    def service(self, current="327000", voltage="4389", status="3", usb="false", ac="false"):
        return ("boot_us=1000000\ncurrent_now=?\nvoltage_now=?\nservice_current_now=" + current
                + "\nservice_dump_begin\nCurrent Battery Service state:\n  AC powered: " + ac
                + "\n  USB powered: " + usb
                + "\n  Wireless powered: false\n  Dock powered: false\n  status: " + status
                + "\n  voltage: " + voltage + "\n  temperature: 329\n"
                + " The last voltage value sent via the battery changed broadcast: 9999\nservice_dump_end\n")

    def test_service_units_and_positive_discharge(self):
        row = parse_sample(self.service())
        self.assertEqual((row["current_ua"], row["voltage_uv"], row["source"], row["status"]),
                         (327000, 4389000, "battery-service", "Discharging"))
        other = {**row, "boot_us": 2000000}
        self.assertAlmostEqual(battery_window([row, other], 1000000, 2000000, 1)["averageW"], 1.435203)
        self.assertIsNone(battery_window([row, other], 1000000, 2000000)["averageW"])

    def test_service_status_kept_flags_not_collapsed(self):
        # status 码照实映射、旗标留原列不坍缩（8E 首局 status=4+AC=true 曾被映成 Charging）：
        # 非放电状态不积分；「接过电」由判读者看旗标列（quality_check.powered()），解析层不猜。
        for status, want in (("1", "Unknown"), ("4", "Not charging"), ("5", "Full"),
                             ("2", "Charging"), ("7", "Unknown")):
            row = parse_sample(self.service(status=status, ac="true"))
            self.assertEqual((row["status"], row["ac_powered"]), (want, "true"), status)
            self.assertIsNone(battery_window([row, {**row, "boot_us": 2000000}],
                                             1000000, 2000000, 1)["averageW"], status)
        # 矛盾行（status=Discharging 但 AC 供电 true，8E 二局实况）：两边都留原文，
        # 坍缩成 Charging 才丢真值；但 battery_window 必须 fail closed 不积分
        row = parse_sample(self.service(ac="true"))
        self.assertEqual((row["status"], row["ac_powered"]), ("Discharging", "true"))
        self.assertIsNone(battery_window([row, {**row, "boot_us": 2000000}],
                                         1000000, 2000000, 1)["averageW"],
                          "Discharging 但 AC=true：fail closed，不能照积 4.0W")
        # 旗标未知（缺行）同样 fail closed：不能拿空旗标当「未接电」
        row = parse_sample(self.service().replace("  Dock powered: false\n", ""))
        self.assertEqual((row["status"], row["dock_powered"]), ("Discharging", ""))
        self.assertIsNone(battery_window([row, {**row, "boot_us": 2000000}],
                                         1000000, 2000000, 1)["averageW"],
                          "旗标缺失/未知：fail closed，不能照积")

    def test_service_failure_never_zero_or_partial_mix(self):
        for value in ("", "Permission denied", "0\n327000"):
            row = parse_sample(self.service(current=value))
            self.assertEqual(row["current_ua"], "")
            self.assertEqual(row["source"], "unavailable")

    def test_frozen_battery_service_is_not_measured_power(self):
        text = self.service(current="1000000", voltage="4000").replace(
            "Current Battery Service state:\n", "Current Battery Service state:\n  (UPDATES STOPPED -- use 'reset' to restart)\n")
        row = parse_sample(text)
        self.assertIsNone(battery_window([row, {**row, "boot_us": 2000000}], 1000000, 2000000, 1)["averageW"])
        self.assertEqual((row["current_ua"], row["voltage_uv"]), (1000000, 4000000))
        self.assertEqual((row["status"], row["source"]), ("Unknown", "battery-service-frozen"))
        # An unused frozen service dump does not invalidate actual sysfs readings —
        # 但冻结态下旗标不可证：四路 powered 留空，battery_window fail closed 不积分。
        row = parse_sample(text.replace("current_now=?", "current_now=-500000").replace("voltage_now=?", "voltage_now=4000000"))
        self.assertEqual((row["source"], row["ac_powered"], row["dock_powered"]), ("sysfs", "", ""))
        self.assertIsNone(battery_window([row, {**row, "boot_us": 2000000}], 1000000, 2000000, -1)["averageW"])

    def test_sysfs_preferred(self):
        text = self.service().replace("current_now=?", "current_now=-500000\nstatus=Discharging")
        row = parse_sample(text.replace("voltage_now=?", "voltage_now=4000000"))
        self.assertEqual(row["source"], "sysfs")
        self.assertEqual(row["current_ua"], -500000)
        # sysfs 主路也填四路旗标（采样命令每行都带 dumpsys 旗标段）：未接电可证才积分。
        self.assertEqual((row["ac_powered"], row["usb_powered"], row["wireless_powered"], row["dock_powered"]),
                         ("false", "false", "false", "false"))
        self.assertAlmostEqual(battery_window([row, {**row, "boot_us": 2000000}],
                                              1000000, 2000000, -1)["averageW"], 2.0)


if __name__ == "__main__":
    unittest.main()
