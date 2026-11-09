package org.s2arm.android;

/** Battery-side net discharge, not application energy or a charging-device consumption estimate. */
final class SystemPower {
    private SystemPower() {}

    // Android BATTERY_STATUS_DISCHARGING=3; current is signed microamperes and voltage millivolts.
    // NaN is an unavailable reading, never a zero-watt substitute.
    static double watts(int status, int plugged, long currentMicroamps, int voltageMillivolts) {
        if (status != 3 || plugged != 0 || currentMicroamps == Long.MIN_VALUE ||
                currentMicroamps >= 0 || voltageMillivolts <= 0) return Double.NaN;
        return -(double) currentMicroamps * voltageMillivolts / 1_000_000_000.0;
    }
}
