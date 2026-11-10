#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 运行器设备侧功耗协议与判读（#163 后 android_power.py 是 host 采集，
# 设备侧「循环采样 + === 分块 + 放电门」口径归入 android/tools/run_game.py）。
import sys
from pathlib import Path
import unittest

_ROOT = Path(__file__).resolve().parents[1]
for _p in (_ROOT / "android", _ROOT / "android" / "tools", _ROOT / "tools"):
    sys.path.insert(0, str(_p))
from run_game import (POWER_BLOCK_SEP, discharge_streak_us, power_battery_window,
                      power_charge_corroboration, power_discharging, power_log_blocks,
                      power_parse_sample, power_sample_command)

battery_window = power_battery_window
charge_corroboration = power_charge_corroboration
discharging = power_discharging
parse_sample = power_parse_sample
sample_command = power_sample_command


class BatteryTest(unittest.TestCase):
    def rows(self, n=66):
        """合法放电样本：1A×4V=4W、电量计每拍 -278µAh（1A ≈ 277.8µAh/s）、
        plugged=0、交互屏、非省电、热档 0——所有判读门都该放行。"""
        return [{"boot_us": i * 1_000_000, "current_ua": -1_000_000, "voltage_uv": 4_000_000,
                 "charge_uah": 5_000_000 - i * 278, "status": "Discharging",
                 "plugged": "0", "interactive": "1", "power_save": "0", "thermal_status": 0,
                 "temp_tenths_c": 350, "ac_powered": "false", "usb_powered": "false",
                 "wireless_powered": "false", "dock_powered": "false"} for i in range(n)]

    def test_known_power(self):
        # 分析窗 = [2.5M+15s, 62.5M-5s] = [17.5M, 57.5M] = 40s：4W × 40s = 160J
        result = battery_window(self.rows(), 2_500_000, 62_500_000)
        self.assertEqual(result["averageW"], 4.0)
        self.assertEqual(result["energyJ"], 160.0)
        self.assertEqual(result["durationSeconds"], 40.0)
        self.assertEqual(result["rawCurrentSign"], -1)
        self.assertFalse(result["comparableToIosWholeSystem"])

    def test_trimmed_rows_do_not_invalidate(self):
        # 剔掉的首 15s 里的接电行不在分析窗 [17.5M, 57.5M] 内：row[5]@5M → 不作废
        rows = self.rows(); rows[5]["plugged"] = "1"
        self.assertEqual(battery_window(rows, 2_500_000, 62_500_000)["averageW"], 4.0)
        # 同一面旗标落在窗内（30M）→ 作废
        rows = self.rows(); rows[30]["plugged"] = "1"
        self.assertIsNone(battery_window(rows, 2_500_000, 62_500_000)["averageW"])
        # 尾 5s 剔除同理：row[60]@60M > 57.5M → 不作废
        rows = self.rows(); rows[60]["status"] = "Charging"
        self.assertEqual(battery_window(rows, 2_500_000, 62_500_000)["averageW"], 4.0)

    def test_trim_makes_empty_window(self):
        self.assertIsNone(battery_window(self.rows(), 10_000_000, 18_000_000)["averageW"])

    def test_interpolation_uses_actual_stop(self):
        rows = []
        for i in range(4):
            rows.append({**self.rows(1)[0], "boot_us": i * 1_000_000,
                         "current_ua": -(i + 1) * 500_000,
                         "charge_uah": 5_000_000 - int((i + 1) * (i + 2) / 2 * 139)})
        # Linear 2/4/6/8 W：.5s→2.5s 是 3..7 W → 10J / 2s（trims 关，只验积分核）
        result = battery_window(rows, 500_000, 2_500_000, check_charge=False,
                                trim_start_us=0, trim_end_us=0)
        self.assertEqual((result["averageW"], result["energyJ"], result["durationSeconds"]),
                         (5.0, 10.0, 2.0))
        self.assertNotEqual(battery_window(rows, 500_000, 1_500_000, check_charge=False,
                                           trim_start_us=0, trim_end_us=0)["averageW"],
                            result["averageW"])

    def test_sign_flip_invalidates(self):
        rows = self.rows(); rows[30]["current_ua"] = 1_000_000
        self.assertIsNone(battery_window(rows, 2_500_000, 62_500_000)["averageW"])

    def test_gates(self):
        """窗内任一采样点破了判读门 → 整块作废（fail closed）。"""
        for key, val in (("status", "Charging"), ("status", "Unknown"), ("status", "Not charging"),
                         ("plugged", "1"), ("plugged", ""),
                         ("interactive", "0"), ("interactive", ""),
                         ("power_save", "1"), ("power_save", ""),
                         ("thermal_status", 3), ("thermal_status", ""),
                         ("current_ua", ""), ("current_ua", 0),
                         ("voltage_uv", ""), ("voltage_uv", 500_000),
                         ("charge_uah", ""), ("charge_uah", 0), ("charge_uah", -5)):
            rows = self.rows(); rows[30][key] = val
            self.assertIsNone(battery_window(rows, 2_500_000, 62_500_000)["averageW"], (key, val))

    def test_coverage_required(self):
        # rows 从 20M 起：窗左边界 17.5M 无采样覆盖
        self.assertIsNone(battery_window(self.rows()[20:], 2_500_000, 62_500_000)["averageW"])
        # rows 止于 50M：窗右边界 57.5M 无采样覆盖
        self.assertIsNone(battery_window(self.rows()[:50], 2_500_000, 62_500_000)["averageW"])

    def test_sampling_gap(self):
        rows = self.rows(); del rows[30:35]      # 30M→35M 之间 6s 空洞 > 3s
        self.assertIsNone(battery_window(rows, 2_500_000, 62_500_000)["averageW"])

    def test_duplicate_time(self):
        rows = self.rows(); rows[30]["boot_us"] = rows[29]["boot_us"]
        self.assertIsNone(battery_window(rows, 2_500_000, 62_500_000)["averageW"])

    def test_missing_clock(self):
        self.assertIsNone(battery_window(self.rows(), None, None)["averageW"])

    def test_charge_slope_gate(self):
        """窗内电量计斜率与电流积分比值出 0.5–1.5 → 作废；check_charge=False 关掉该门。"""
        rows = self.rows()
        for r in rows:
            r["charge_uah"] = 5_000_000        # 电量计不动：斜率 0 → ratio 0
        self.assertIsNone(battery_window(rows, 2_500_000, 62_500_000)["averageW"])
        self.assertEqual(battery_window(rows, 2_500_000, 62_500_000,
                                        check_charge=False)["averageW"], 4.0)

    def test_discharge_streak(self):
        rows = self.rows()
        self.assertEqual(discharge_streak_us(rows), 65_000_000)
        rows[60]["plugged"] = "1"              # 60M 接电：尾串退到 61M..65M = 4s
        self.assertEqual(discharge_streak_us(rows), 4_000_000)
        rows = self.rows(); del rows[40:43]    # 39M→43M 空洞 4s > 3s：尾串只剩 43M..65M
        self.assertEqual(discharge_streak_us(rows), 22_000_000)
        rows = self.rows(); rows[65]["status"] = "Charging"
        self.assertEqual(discharge_streak_us(rows), 0)                 # 尾行不在放电
        rows = self.rows()
        self.assertEqual(discharge_streak_us(rows, end_us=70_000_000), 0)   # 尾行离 end 5s > 3s
        self.assertEqual(discharge_streak_us(rows, end_us=50_000_000), 50_000_000)

    def test_discharging(self):
        row = self.rows()[0]
        self.assertTrue(discharging(row))
        for key, val in (("status", "Not charging"), ("status", "Charging"),
                         ("plugged", "1"), ("plugged", "")):
            self.assertFalse(discharging({**row, key: val}), (key, val))

    def test_charge_corroboration_quantum(self):
        """等差递减电量计 → 量子区间核对：积分 18,056µAh 落在 18,070±278 内 → 互证。"""
        co = charge_corroboration(self.rows())
        self.assertTrue(co["corroborated"])
        self.assertEqual(co["method"], "observed_constant_quantum_endpoint_interval")
        self.assertEqual(co["quantum_uah"], 278)
        self.assertGreaterEqual(co["transitions"], 2)

    def test_charge_corroboration_ratio(self):
        """不齐递减 → 整局比值门：掉电 18,070µAh / 积分 18,056 ≈ 1.0 → 互证。"""
        rows = self.rows()
        for i, r in enumerate(rows):
            r["charge_uah"] = 5_000_000 - i * 278 - (i % 5) * 10
        co = charge_corroboration(rows)
        self.assertEqual(co["method"], "whole_run_charge_change")
        self.assertTrue(co["corroborated"])

    def test_charge_corroboration_rejects(self):
        rows = self.rows()
        for r in rows:
            r["charge_uah"] = 5_000_000        # 整局不掉电 → 无单调放电趋势
        self.assertFalse(charge_corroboration(rows)["corroborated"])
        rows = self.rows()
        for r in rows:
            r["charge_uah"] = 5_000_000 - 5000 * (r["boot_us"] // 30_000_000)
        # 每 30 拍才跳一次：非零 delta 只 2 个且相等 → 量子 5000；掉电 10000 对积分 18056 不互证
        co = charge_corroboration(rows)
        self.assertEqual(co["transitions"], 2)
        self.assertFalse(co["corroborated"])
        rows = self.rows(); rows[30]["plugged"] = "1"   # 接电行 → 不互证
        self.assertFalse(charge_corroboration(rows)["corroborated"])

    def test_power_log_blocks(self):
        self.assertEqual(power_log_blocks("a=1\n" + POWER_BLOCK_SEP + "\nb=2\n" + POWER_BLOCK_SEP + "\n尾巴未封闭\n"), ["a=1", "b=2"])
        self.assertEqual(power_log_blocks(""), [])

    def test_parser_denied_values(self):
        row = parse_sample("boot_us=1000000\ncurrent_now=?\nvoltage_now=4000000\nstatus=Discharging\n")
        self.assertEqual(row["current_ua"], "")
        self.assertEqual(row["voltage_uv"], 4_000_000)
        self.assertEqual(row["plugged"], "")     # 无旗标段 → plugged 读不出

    def test_shell_path_validation(self):
        self.assertIn("/sys/class/power_supply/battery/", sample_command("battery"))
        with self.assertRaises(ValueError):
            sample_command("battery;id")

    def service(self, current="327000", voltage="4389", status="3", usb="false", ac="false"):
        return ("boot_us=1000000\ncurrent_now=?\nvoltage_now=?\ncharge_counter=4999909\n"
                "service_current_now=" + current
                + "\nservice_dump_begin\nCurrent Battery Service state:\n  AC powered: " + ac
                + "\n  USB powered: " + usb
                + "\n  Wireless powered: false\n  Dock powered: false\n  status: " + status
                + "\n  voltage: " + voltage + "\n  temperature: 329\n"
                + " The last voltage value sent via the battery changed broadcast: 9999\n"
                "service_dump_end\ninteractive=true\npower_save=false\nthermal_status=0\n")

    def test_service_units_and_positive_discharge(self):
        row = parse_sample(self.service())
        self.assertEqual((row["current_ua"], row["voltage_uv"], row["source"], row["status"]),
                         (327000, 4389000, "battery-service", "Discharging"))
        self.assertEqual((row["plugged"], row["interactive"], row["power_save"],
                          row["thermal_status"]), ("0", "1", "0", 0))
        other = {**row, "boot_us": 2000000, "charge_uah": 4999818}   # -91µAh/s ≈ 327mA
        self.assertAlmostEqual(
            battery_window([row, other], 1000000, 2000000,
                           trim_start_us=0, trim_end_us=0)["averageW"], 1.435203)

    def test_service_status_kept_flags_not_collapsed(self):
        # status 码照实映射、旗标留原列不坍缩（8E 首局 status=4+AC=true 曾被映成 Charging）：
        # 非放电/plugged 状态不积分；「接过电」由判读者看旗标列，解析层不猜。
        for status, want in (("1", "Unknown"), ("4", "Not charging"), ("5", "Full"),
                             ("2", "Charging"), ("7", "Unknown")):
            row = parse_sample(self.service(status=status, ac="true"))
            self.assertEqual((row["status"], row["ac_powered"], row["plugged"]),
                             (want, "true", "1"), status)
            self.assertIsNone(battery_window([row, {**row, "boot_us": 2000000}],
                                             1000000, 2000000, trim_start_us=0,
                                             trim_end_us=0)["averageW"], status)
        # 矛盾行（status=Discharging 但 AC 供电 true，8E 二局实况）：两边都留原文，
        # 坍缩成 Charging 才丢真值；但 plugged=1 → power_battery_window fail closed 不积分
        row = parse_sample(self.service(ac="true"))
        self.assertEqual((row["status"], row["ac_powered"], row["plugged"]),
                         ("Discharging", "true", "1"))
        self.assertIsNone(battery_window([row, {**row, "boot_us": 2000000}],
                                         1000000, 2000000, trim_start_us=0,
                                         trim_end_us=0)["averageW"],
                          "Discharging 但 plugged=1：fail closed，不能照积")
        # 旗标未知（缺行）→ plugged 读不出 → fail closed
        row = parse_sample(self.service().replace("  Dock powered: false\n", ""))
        self.assertEqual((row["status"], row["dock_powered"], row["plugged"]),
                         ("Discharging", "", ""))
        self.assertIsNone(battery_window([row, {**row, "boot_us": 2000000}],
                                         1000000, 2000000, trim_start_us=0,
                                         trim_end_us=0)["averageW"],
                          "plugged 未知：fail closed，不能照积")

    def test_service_failure_never_zero_or_partial_mix(self):
        for value in ("", "Permission denied", "0\n327000"):
            row = parse_sample(self.service(current=value))
            self.assertEqual(row["current_ua"], "")
            self.assertEqual(row["source"], "unavailable")

    def test_frozen_battery_service_is_not_measured_power(self):
        text = self.service(current="1000000", voltage="4000").replace(
            "Current Battery Service state:\n", "Current Battery Service state:\n  (UPDATES STOPPED -- use 'reset' to restart)\n")
        row = parse_sample(text)
        self.assertIsNone(battery_window([row, {**row, "boot_us": 2000000}], 1000000, 2000000,
                                         trim_start_us=0, trim_end_us=0)["averageW"])
        self.assertEqual((row["current_ua"], row["voltage_uv"]), (1000000, 4000000))
        self.assertEqual((row["status"], row["source"]), ("Unknown", "battery-service-frozen"))
        # An unused frozen service dump does not invalidate actual sysfs readings —
        # 但冻结态下旗标不可证：四路 powered 与 plugged 都留空，fail closed 不积分。
        row = parse_sample(text.replace("current_now=?", "current_now=-500000")
                           .replace("voltage_now=?", "voltage_now=4000000"))
        self.assertEqual((row["source"], row["ac_powered"], row["plugged"]), ("sysfs", "", ""))
        self.assertIsNone(battery_window([row, {**row, "boot_us": 2000000}], 1000000, 2000000,
                                         trim_start_us=0, trim_end_us=0)["averageW"])

    def test_sysfs_preferred(self):
        text = self.service().replace("current_now=?", "current_now=-500000\nstatus=Discharging")
        row = parse_sample(text.replace("voltage_now=?", "voltage_now=4000000"))
        self.assertEqual(row["source"], "sysfs")
        self.assertEqual(row["current_ua"], -500000)
        # sysfs 主路也填四路旗标 + plugged（采样命令每行都带 dumpsys 旗标段）
        self.assertEqual((row["ac_powered"], row["usb_powered"], row["wireless_powered"],
                          row["dock_powered"], row["plugged"]),
                         ("false", "false", "false", "false", "0"))
        other = {**row, "boot_us": 2000000, "charge_uah": 4999770}   # -139µAh/s ≈ 500mA
        self.assertAlmostEqual(battery_window([row, other], 1000000, 2000000,
                                              trim_start_us=0, trim_end_us=0)["averageW"], 2.0)


if __name__ == "__main__":
    unittest.main()
