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
                 "status": "Discharging"} for i in range(66)]

    def test_known_power(self):
        result = battery_window(self.rows(), 2_500_000, 62_500_000, -1)
        self.assertEqual(result["averageW"], 4.0)
        self.assertEqual(result["energyJ"], 240.0)
        self.assertFalse(result["comparableToIosWholeSystem"])

    def test_interpolation_uses_mark_through_actual_stop_not_last_frame(self):
        rows = [{"boot_us": i * 1_000_000, "current_ua": -(i + 1) * 500_000,
                 "voltage_uv": 4_000_000, "status": "Discharging"} for i in range(4)]
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

    def service(self, current="327000", voltage="4389", status="3", usb="false"):
        return ("boot_us=1000000\ncurrent_now=?\nvoltage_now=?\nservice_current_now=" + current
                + "\nservice_dump_begin\nCurrent Battery Service state:\n  AC powered: false\n  USB powered: " + usb
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

    def test_service_charging_or_unknown_sources_rejected(self):
        for raw in (self.service(usb="true"), self.service(status="1"),
                    self.service().replace("  Dock powered: false\n", "")):
            row = parse_sample(raw)
            self.assertNotEqual(row["status"], "Discharging")
            self.assertIsNone(battery_window([row, {**row, "boot_us": 2000000}], 1000000, 2000000, 1)["averageW"])

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
        # An unused frozen service dump does not invalidate actual sysfs readings.
        row = parse_sample(text.replace("current_now=?", "current_now=-500000").replace("voltage_now=?", "voltage_now=4000000"))
        self.assertEqual(row["source"], "sysfs")

    def test_sysfs_preferred(self):
        row = parse_sample(self.service().replace("current_now=?", "current_now=-500000")
                           .replace("voltage_now=?", "voltage_now=4000000"))
        self.assertEqual(row["source"], "sysfs")
        self.assertEqual(row["current_ua"], -500000)


if __name__ == "__main__":
    unittest.main()
