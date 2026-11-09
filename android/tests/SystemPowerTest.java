package org.s2arm.android;

import java.lang.reflect.Method;

/** Power is battery-side net discharge; unavailable readings must not become zero watts. */
public final class SystemPowerTest {
    private static int failures;
    private static void check(Method sample, int status, int plugged, long ua, int mv, double expected, String name) throws Exception {
        double got = (double) sample.invoke(null, status, plugged, ua, mv);
        boolean ok = Double.isNaN(expected) ? Double.isNaN(got) : Math.abs(got - expected) < 0.000001;
        if (!ok) { System.err.println(name + ": expected " + expected + ", got " + got); failures++; }
    }
    public static void main(String[] args) throws Exception {
        Class<?> power;
        try { power = Class.forName("org.s2arm.android.SystemPower"); }
        catch (ClassNotFoundException absent) { throw new AssertionError("battery net discharge sampler is missing", absent); }
        Method sample = power.getDeclaredMethod("watts", int.class, int.class, long.class, int.class);
        sample.setAccessible(true);
        check(sample, 3, 0, -1000000, 4000, 4.0, "1 A x 4 V discharge");
        // 骁龙 8E 这类机型放电电流报正号：幅值算功耗，符号不再当判据
        check(sample, 3, 0, 1000000, 4000, 4.0, "positive current while discharging");
        check(sample, 3, 0, -125000, 3800, 0.475, "units and fractional watts");
        check(sample, 3, 0, 125000, 3800, 0.475, "sign-insensitive magnitude");
        check(sample, 2, 1, 1000000, 4000, Double.NaN, "charging");
        check(sample, 3, 1, -1000000, 4000, Double.NaN, "plugged AC while discharging status");
        check(sample, 3, 2, -1000000, 4000, Double.NaN, "plugged USB while discharging status");
        check(sample, 2, 0, 1000000, 4000, Double.NaN, "unplugged but meter says charging");
        check(sample, 4, 0, -1000000, 4000, Double.NaN, "unplugged but meter says not charging");
        check(sample, 1, 0, -1000000, 4000, Double.NaN, "unknown status");
        check(sample, 3, 0, Long.MIN_VALUE, 4000, Double.NaN, "unsupported current");
        check(sample, 3, 0, 0, 4000, Double.NaN, "zero current");
        check(sample, 3, 0, 31000000, 4000, Double.NaN, "above 30 A");
        check(sample, 3, 0, -31000000, 4000, Double.NaN, "below -30 A");
        check(sample, 3, 0, 30000000, 4000, 120.0, "30 A boundary accepted");
        check(sample, 3, 0, -1000000, 2499, Double.NaN, "voltage below 2.5 V");
        check(sample, 3, 0, -1000000, 2500, 2.5, "2.5 V boundary accepted");
        check(sample, 3, 0, -1000000, 10000, 10.0, "10 V boundary accepted");
        check(sample, 3, 0, -1000000, 10001, Double.NaN, "voltage above 10 V");
        check(sample, 3, 0, -1000000, 0, Double.NaN, "missing voltage");
        if (failures != 0) throw new AssertionError(failures + " battery power checks failed");
        System.out.println("battery net discharge conversion / unavailable states passed");
    }
}
