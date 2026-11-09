package org.s2arm.android;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Handler;
import android.os.SystemClock;
import java.util.function.DoubleConsumer;

/**
 * #48 A5 / #163：应用内电池净放电采样（主线程，1 秒一拍），只在游戏前台时运行。
 * 读数经 Sink 落 logcat（采集的 logcat.txt）：battery=整机电池净放电、
 * rails=Android 15+ CPU/GPU 轨功耗（插电也可用）、event=低频原始值行。
 * 放电判定 = plugged==0 && status==DISCHARGING，且拔电后先等 8 秒让电量计
 * 稳定；充电 / 外接 / 不支持一律 NaN = 不可用。
 * debug.s2a.power_readout 运行时开关，默认开（属性值 "0" 才关）；关掉时不碰
 * BatteryManager / registerReceiver / SystemHealthManager，读数固定不可用。
 */
final class SystemPowerSampler implements Runnable {
    interface Sink {
        void battery(double watts);
        void rails(double watts);
        void event(String line);
    }

    private static final long TICK_MS = 1000;
    private static final long SETTLE_MS = 8000;   // 拔电后电量计稳定窗口（真机实测口径）
    private static final int RAW_EVERY = 30;      // 原始值日志间隔（拍数）
    private final Context context;
    private final Handler handler;
    private final Sink sink;
    private final SystemPowerRails rails;
    private boolean running;
    private int ticks, lastPlugged = -1;
    private long unpluggedAt = Long.MAX_VALUE;

    SystemPowerSampler(Context context, Handler handler, DoubleConsumer batterySink) {
        this(context, handler, new Sink() {
            @Override public void battery(double watts) { batterySink.accept(watts); }
            @Override public void rails(double watts) { }
            @Override public void event(String line) { }
        });
    }

    SystemPowerSampler(Context context, Handler handler, Sink sink) {
        this.context = context;
        this.handler = handler;
        this.sink = sink;
        rails = Build.VERSION.SDK_INT >= 35 ? SystemPowerRails.create(context, sink::event) : null;
    }

    void start() {
        if (running) return;
        running = true;
        handler.post(this);
    }

    void stop() {
        running = false;
        lastPlugged = -1;               // 停表期间设备状态未知，恢复后重新起算 settle
        unpluggedAt = Long.MAX_VALUE;
        handler.removeCallbacks(this);
        if (rails != null) rails.stop();
    }

    private double read() {
        try {
            BatteryManager manager = context.getSystemService(BatteryManager.class);
            Intent state = context.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (manager == null || state == null) return Double.NaN;
            int status = state.getIntExtra(BatteryManager.EXTRA_STATUS, -1);
            int plugged = state.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1);
            int mv = state.getIntExtra(BatteryManager.EXTRA_VOLTAGE, -1);
            long ua = manager.getLongProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW);
            long now = SystemClock.elapsedRealtime();
            if (plugged != lastPlugged) {   // 观察到的拔电沿；读不到时 -1 也按非拔电处理
                lastPlugged = plugged;
                unpluggedAt = plugged == 0 ? now : Long.MAX_VALUE;
            }
            if (++ticks % RAW_EVERY == 0)   // 原始值含电流原号：部分厂商放电报正号，留作真机对账
                sink.event("battery-raw status=" + status + " plugged=" + plugged +
                    " current_ua=" + ua + " voltage_mv=" + mv);
            boolean settled = plugged == 0 && status == BatteryManager.BATTERY_STATUS_DISCHARGING &&
                now - unpluggedAt >= SETTLE_MS;
            return settled ? SystemPower.watts(status, plugged, ua, mv) : Double.NaN;
        } catch (RuntimeException unavailable) {
            return Double.NaN;
        }
    }

    @Override public void run() {
        if (!running) return;   // stop 后迟到的回调不采
        boolean on = debugOn();
        sink.battery(on ? read() : Double.NaN);
        sink.rails(on && rails != null ? rails.poll() : Double.NaN);
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
