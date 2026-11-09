package org.s2arm.android;

import android.content.Context;
import android.os.BatteryManager;
import android.os.Handler;
import android.os.SystemClock;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.function.DoubleConsumer;

/** Exercise actual sampler with Android service and looper boundaries replaced. */
public final class SystemPowerSamplerTest {
    private static int failures;
    private static void check(boolean value, String what) { if (!value) { System.err.println("FAIL " + what); failures++; } }
    private static Context discharging() {
        Context context = new Context();
        context.battery.putExtra(BatteryManager.EXTRA_STATUS, BatteryManager.BATTERY_STATUS_DISCHARGING)
            .putExtra(BatteryManager.EXTRA_PLUGGED, 0)
            .putExtra(BatteryManager.EXTRA_VOLTAGE, 4000);
        context.manager.current = -1000000;
        return context;
    }
    public static void main(String[] args) throws Exception {
        Class<?> type;
        try { type = Class.forName("org.s2arm.android.SystemPowerSampler"); }
        catch (ClassNotFoundException absent) { throw new AssertionError("BatteryManager sampler is missing", absent); }
        Constructor<?> ctor = type.getDeclaredConstructor(Context.class, Handler.class, DoubleConsumer.class);
        ctor.setAccessible(true);
        Method start = type.getDeclaredMethod("start"), stop = type.getDeclaredMethod("stop");
        start.setAccessible(true); stop.setAccessible(true);
        Context context = discharging();
        Handler handler = new Handler(); ArrayList<Double> output = new ArrayList<>();
        Object sampler = ctor.newInstance(context, handler, (DoubleConsumer) output::add);

        // 拔电后电量计要稳 8 秒：观察到的第一次读数按刚拔电处理
        SystemClock.now = 1000;
        start.invoke(sampler); handler.runPending();
        check(output.size() == 1 && Double.isNaN(output.get(0)),
            "settle window: first unplugged read unavailable");
        SystemClock.now = 8999;
        handler.runPending();
        check(output.size() == 2 && Double.isNaN(output.get(1)), "still settling at 8 s minus 1");
        SystemClock.now = 9000;
        handler.runPending();
        check(output.size() == 3 && Math.abs(output.get(2) - 4.0) < 0.000001, "settled read converts");

        // 8E 机型：放电电流报正号，幅值算功耗
        context.manager.current = 1500000;
        handler.runPending();
        check(output.size() == 4 && Math.abs(output.get(3) - 6.0) < 0.000001,
            "positive current while discharging");

        // 插电中断：不可用；再拔电重新起算 8 秒
        context.battery.putExtra(BatteryManager.EXTRA_PLUGGED, 1);
        SystemClock.now = 9500;
        handler.runPending();
        check(output.size() == 5 && Double.isNaN(output.get(4)), "plugged invalidates power");
        context.battery.putExtra(BatteryManager.EXTRA_PLUGGED, 0);
        SystemClock.now = 10000;
        handler.runPending();
        check(output.size() == 6 && Double.isNaN(output.get(5)), "re-unplug restarts settle clock");
        SystemClock.now = 18000;
        handler.runPending();
        check(output.size() == 7 && Math.abs(output.get(6) - 6.0) < 0.000001, "second settle completes");

        // 停表不采；迟到的回调被忽略
        Runnable stale = handler.pending; int reads = context.reads;
        stop.invoke(sampler);
        check(handler.pending == null, "pause removes callback");
        stale.run();
        check(context.reads == reads && output.size() == 7, "late callback does not sample");

        // 恢复采样重起 settle（停表期间状态未知）
        start.invoke(sampler); handler.runPending();
        check(output.size() == 8 && Double.isNaN(output.get(7)), "resume restarts settle");
        SystemClock.now = 26000;
        handler.runPending();
        check(output.size() == 9 && Math.abs(output.get(8) - 6.0) < 0.000001, "resumed settled read");

        // 属性不可用：电表不支持 current_now
        context.manager.current = Long.MIN_VALUE;
        handler.runPending();
        check(output.size() == 10 && Double.isNaN(output.get(9)), "unsupported property stays unavailable");

        // debug.s2a.power_readout=0：不碰 BatteryManager / registerReceiver，读数固定不可用
        android.os.SystemProperties.values.put("debug.s2a.power_readout", "0");
        context.manager.current = -1000000;
        int reads0 = context.reads;
        handler.runPending();
        check(output.size() == 11 && Double.isNaN(output.get(10)) && context.reads == reads0,
            "power_readout=0 publishes unavailable without reading battery");
        android.os.SystemProperties.values.clear();
        handler.runPending();
        check(output.size() == 12 && Math.abs(output.get(11) - 4.0) < 0.000001, "switch back on resumes real reads");
        stop.invoke(sampler);
        if (failures != 0) throw new AssertionError(failures + " sampler checks failed");
        System.out.println("BatteryManager settle / sign / plug transitions / stop / resume / readout switch passed");
    }
}
