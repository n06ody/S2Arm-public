package org.s2arm.android;

import android.os.Build;
import android.os.Handler;
import android.view.Surface;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;

/** 接线后的红绿：真实 SystemTuning 轮询驱动投票与省电 cap（Android 边界全部由桩替代）。 */
public final class SystemTuningTest {
    private static void check(boolean value, String what) { if (!value) throw new AssertionError(what); }
    public static void main(String[] args) throws Exception {
        Class<?> type;
        try { type = Class.forName("org.s2arm.android.SystemTuning"); }
        catch (ClassNotFoundException absent) { throw new AssertionError("system tuning poller is missing", absent); }
        Constructor<?> ctor = type.getDeclaredConstructor(android.content.Context.class, Handler.class);
        ctor.setAccessible(true);
        Method attach = type.getDeclaredMethod("attach", Surface.class), start = type.getDeclaredMethod("start"),
            stop = type.getDeclaredMethod("stop"), detach = type.getDeclaredMethod("detach");
        attach.setAccessible(true); start.setAccessible(true); stop.setAccessible(true); detach.setAccessible(true);

        Build.VERSION.SDK_INT = 31;
        android.content.Context context = new android.content.Context();
        Handler handler = new Handler();
        Object tuning = ctor.newInstance(context, handler);
        Surface surface = new Surface();
        attach.invoke(tuning, surface);

        // A3：登记的直连最终周期 → fps 投票；0 = 清票；去重不重复投
        NativeBridge.periodNs = 16666666;
        start.invoke(tuning);
        handler.runPending();
        check(surface.calls == 1 && Math.abs(surface.value - 60f) < 0.01f, "present period votes 60");
        handler.runPending();
        check(surface.calls == 1, "same period does not revote");
        NativeBridge.periodNs = 33333334;
        handler.runPending();
        check(surface.calls == 2 && Math.abs(surface.value - 30f) < 0.01f, "capped period votes 30");
        NativeBridge.periodNs = 0;
        handler.runPending();
        check(surface.calls == 3 && surface.value == 0f, "no presents clears vote");

        // A4：省电档 → cap true 一次；不变不重复下 JNI；回到标准档退回 false
        int calls0 = NativeBridge.capCalls;
        context.gameManager.mode = android.app.GameManager.GAME_MODE_BATTERY;
        handler.runPending();
        check(NativeBridge.cap && NativeBridge.capCalls == calls0 + 1, "battery mode sets cap once");
        handler.runPending();
        check(NativeBridge.capCalls == calls0 + 1, "unchanged mode does not rewrite cap");
        context.gameManager.mode = android.app.GameManager.GAME_MODE_STANDARD;
        handler.runPending();
        check(!NativeBridge.cap && NativeBridge.capCalls == calls0 + 2, "standard mode releases cap");

        // 运行时开关：frame_vote=0 清票；battery_cap=0 省电档也退回 false
        android.os.SystemProperties.values.put("debug.s2a.frame_vote", "0");
        NativeBridge.periodNs = 16666666;
        handler.runPending();
        check(surface.value == 0f, "frame_vote=0 clears the vote");
        // 先让省电档把 cap 打成 true，再关 battery_cap：cap 被强制退回 false 并下一次 JNI
        context.gameManager.mode = android.app.GameManager.GAME_MODE_BATTERY;
        int calls1 = NativeBridge.capCalls;
        handler.runPending();
        check(NativeBridge.cap && NativeBridge.capCalls == calls1 + 1, "battery mode caps again");
        android.os.SystemProperties.values.put("debug.s2a.battery_cap", "0");
        handler.runPending();
        check(!NativeBridge.cap && NativeBridge.capCalls == calls1 + 2, "battery_cap=0 forces cap off");
        android.os.SystemProperties.values.clear();

        // API<31 不读 GameManager：省电档不可见 → cap 保持 false
        Build.VERSION.SDK_INT = 30;
        context.gameManager.mode = android.app.GameManager.GAME_MODE_BATTERY;
        handler.runPending();
        check(!NativeBridge.cap, "pre-31 does not consult GameManager");
        Build.VERSION.SDK_INT = 31;

        // stop 后迟到的回调不采：不投票、不改 cap、不重排
        handler.runPending();
        Runnable stale = handler.pending;
        stop.invoke(tuning);
        check(handler.pending == null, "stop removes pending tick");
        int calls2 = NativeBridge.capCalls, votes = surface.calls;
        stale.run();
        check(NativeBridge.capCalls == calls2 && surface.calls == votes && handler.pending == null,
            "late callback does not sample or reschedule");

        // 恢复：再次 start 后正常采（换周期让票值变化，能看到新投票）
        context.gameManager.mode = android.app.GameManager.GAME_MODE_STANDARD;
        NativeBridge.periodNs = 33333334;
        start.invoke(tuning);
        handler.runPending();
        check(surface.calls == votes + 1 && Math.abs(surface.value - 30f) < 0.01f, "resume samples again");

        detach.invoke(tuning);
        check(surface.value == 0f, "detach clears the vote");
        System.out.println("system tuning poll / vote / cap / switches / stop-resume passed");
    }
}
