package org.s2arm.android;

/** Host-test stub of the JNI surface SystemTuning references; records the cap calls. */
final class NativeBridge {
    static long periodNs;
    static int capCalls;
    static boolean cap;

    private NativeBridge() { }

    static long presentPeriodNs() { return periodNs; }
    static void setBatteryCap(boolean enabled) { capCalls++; cap = enabled; }
}
