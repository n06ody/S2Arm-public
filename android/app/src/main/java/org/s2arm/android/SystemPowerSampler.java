package org.s2arm.android;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Handler;
import java.util.function.DoubleConsumer;

/**
 * #48 A5：应用内电池净放电采样（主线程，1 秒一拍），只在游戏前台时运行。
 * 读数经 DoubleConsumer 落 logcat（采集的 logcat.txt）；未接电且读数有效才报 W，
 * 充电 / 外接 / 不支持一律 NaN = 不可用。
 * debug.s2a.power_readout 运行时开关，默认开（属性值 "0" 才关）；关掉时不碰
 * BatteryManager / registerReceiver，读数固定不可用。
 */
final class SystemPowerSampler implements Runnable {
    private static final long TICK_MS = 1000;
    private final Context context;
    private final Handler handler;
    private final DoubleConsumer sink;
    private boolean running;

    SystemPowerSampler(Context context, Handler handler, DoubleConsumer sink) {
        this.context = context;
        this.handler = handler;
        this.sink = sink;
    }

    void start() {
        if (running) return;
        running = true;
        handler.post(this);
    }

    void stop() {
        running = false;
        handler.removeCallbacks(this);
    }

    private double read() {
        try {
            BatteryManager manager = context.getSystemService(BatteryManager.class);
            Intent state = context.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (manager == null || state == null) return Double.NaN;
            return SystemPower.watts(state.getIntExtra(BatteryManager.EXTRA_STATUS, -1),
                state.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1),
                manager.getLongProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW),
                state.getIntExtra(BatteryManager.EXTRA_VOLTAGE, -1));
        } catch (RuntimeException unavailable) {
            return Double.NaN;
        }
    }

    @Override public void run() {
        if (!running) return;   // stop 后迟到的回调不采
        sink.accept(debugOn() ? read() : Double.NaN);
        if (running) handler.postDelayed(this, TICK_MS);
    }

    private static boolean debugOn() {
        try {
            Class<?> properties = Class.forName("android.os.SystemProperties");
            Object value = properties.getMethod("get", String.class, String.class)
                .invoke(null, "debug.s2a.power_readout", "");
            return !"0".equals(value);   // 读不到 / 未设 = 默认开；只有显式 "0" 关
        } catch (ReflectiveOperationException unavailable) {
            return true;
        }
    }
}
