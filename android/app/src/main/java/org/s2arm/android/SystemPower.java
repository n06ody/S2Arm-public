package org.s2arm.android;

import android.os.BatteryManager;

/** Battery-side net discharge, not application energy or a charging-device consumption estimate. */
final class SystemPower {
    private SystemPower() {}

    // 放电判定 = 物理拔电 + 电表报 DISCHARGING；功耗 = |current_now| x voltage——
    // 骁龙 8E 这类机型放电电流报正号（#163），幅值才有意义，符号不做判据。
    // 合理区间：电压 2.5–10 V、电流非 0 且幅值 ≤30 A，越界当不可用。
    // NaN is an unavailable reading, never a zero-watt substitute.
    static double watts(int status, int plugged, long currentMicroamps, int voltageMillivolts) {
        if (status != BatteryManager.BATTERY_STATUS_DISCHARGING || plugged != 0 ||
                currentMicroamps == Long.MIN_VALUE || currentMicroamps == 0 ||
                Math.abs(currentMicroamps) > 30_000_000L ||
                voltageMillivolts < 2_500 || voltageMillivolts > 10_000) return Double.NaN;
        return Math.abs((double) currentMicroamps) * voltageMillivolts / 1_000_000_000.0;
    }
}
