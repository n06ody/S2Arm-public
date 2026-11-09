package org.s2arm.android;

import android.os.Build;
import android.view.Surface;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;

/** The production voter sees controlled platform Surface and API-level boundaries. */
public final class FrameRateVoteTest {
    private static void check(boolean value, String what) { if (!value) throw new AssertionError(what); }
    public static void main(String[] args) throws Exception {
        Class<?> type;
        try { type = Class.forName("org.s2arm.android.FrameRateVote"); }
        catch (ClassNotFoundException absent) { throw new AssertionError("surface frame-rate voter is missing", absent); }
        Constructor<?> ctor = type.getDeclaredConstructor(); ctor.setAccessible(true);
        Method attach = type.getDeclaredMethod("attach", Surface.class), target = type.getDeclaredMethod("target", float.class),
            detach = type.getDeclaredMethod("detach");
        attach.setAccessible(true); target.setAccessible(true); detach.setAccessible(true);
        Object vote = ctor.newInstance(); Surface first = new Surface();
        attach.invoke(vote, first); target.invoke(vote, 60f);
        check(first.calls == 1 && first.value == 60f && first.compatibility == 0, "game frame rate and default compatibility");
        target.invoke(vote, 60f); check(first.calls == 1, "unchanged vote is not repeated");
        target.invoke(vote, 30f); check(first.calls == 2 && first.value == 30f, "request changes to 30");
        target.invoke(vote, 0f); check(first.calls == 3 && first.value == 0f, "unknown clears vote");
        target.invoke(vote, 30f); detach.invoke(vote);
        check(first.calls == 5 && first.value == 0f, "detach clears live surface");
        Surface second = new Surface(); Build.VERSION.SDK_INT = 29;
        attach.invoke(vote, second); target.invoke(vote, 60f); detach.invoke(vote);
        check(second.calls == 0, "pre-30 does not call unavailable API");
        System.out.println("frame vote / unknown / detach / pre-30 passed");
    }
}
