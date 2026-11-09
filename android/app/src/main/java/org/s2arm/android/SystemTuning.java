package org.s2arm.android;

import android.app.GameManager;
import android.content.Context;
import android.os.Build;
import android.os.Handler;

/**
 * #48 A3+A4：直连系统层调节轮询（主线程，500 ms 一拍）：
 *   A3 刷新率投票：直连 present_period 登记的最终周期（NativeBridge.presentPeriodNs）换算 fps，
 *     经 FrameRateVote 投给 Surface.setFrameRate；0 / 无窗口 = 清票。投票源是节拍器实际跟的周期，
 *     不是测得的 FPS。
 *   A4 档位限帧：GameManager.getGameMode() == GAME_MODE_BATTERY 时 NativeBridge.setBatteryCap(true)，
 *     直连 present 周期取 max(原周期, 33.3 ms)；游戏模式配置声明两个 supports 为 false，
 *     OEM 厂商干预保留、不由本应用代管（厂商 SDK 未实现）。
 * debug.s2a.frame_vote / debug.s2a.battery_cap 运行时开关，默认开（属性值 "0" 才关）；
 * 关 A3 时清掉已投的票，关 A4 时把 cap 退回 false。
 */
final class SystemTuning implements Runnable {
    private static final long TICK_MS = 500;
    private final Context context;
    private final Handler handler;
    private final FrameRateVote vote = new FrameRateVote();
    private boolean running;
    private Boolean capSent;   // 去重：值不变不重复下 JNI

    SystemTuning(Context context, Handler handler) {
        this.context = context;
        this.handler = handler;
    }

    /** surfaceCreated：投票对象换成新 Surface，清掉上一代的票。 */
    void attach(android.view.Surface surface) { vote.attach(surface); }
    /** surfaceDestroyed：Surface 退出清票。 */
    void detach() { vote.detach(); }

    void start() {
        if (running) return;
        running = true;
        handler.post(this);
    }

    void stop() {
        running = false;
        handler.removeCallbacks(this);
    }

    @Override public void run() {
        if (!running) return;   // stop 后迟到的回调不采
        if (debugOn("debug.s2a.frame_vote")) {
            long ns = NativeBridge.presentPeriodNs();
            vote.target(ns > 0 ? 1_000_000_000f / ns : 0f);
        } else {
            vote.target(0);   // 关掉即清票，不让旧值挂住显示模式
        }
        boolean cap = debugOn("debug.s2a.battery_cap") && gameMode() == GameManager.GAME_MODE_BATTERY;
        if (capSent == null || capSent != cap) {
            NativeBridge.setBatteryCap(cap);
            capSent = cap;
        }
        if (running) handler.postDelayed(this, TICK_MS);
    }

    private int gameMode() {
        if (Build.VERSION.SDK_INT < 31) return GameManager.GAME_MODE_UNSUPPORTED;
        try {
            GameManager manager = context.getSystemService(GameManager.class);
            return manager == null ? GameManager.GAME_MODE_UNSUPPORTED : manager.getGameMode();
        } catch (RuntimeException unavailable) {
            return GameManager.GAME_MODE_UNSUPPORTED;
        }
    }

    private static boolean debugOn(String property) {
        try {
            Class<?> properties = Class.forName("android.os.SystemProperties");
            Object value = properties.getMethod("get", String.class, String.class).invoke(null, property, "");
            return !"0".equals(value);   // 读不到 / 未设 = 默认开；只有显式 "0" 关
        } catch (ReflectiveOperationException unavailable) {
            return true;
        }
    }
}
