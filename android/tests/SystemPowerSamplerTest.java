package org.s2arm.android;

import android.content.Context;
import android.os.BatteryManager;
import android.os.Handler;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.function.DoubleConsumer;

/** Exercise actual sampler with Android service and looper boundaries replaced. */
public final class SystemPowerSamplerTest {
    private static void check(boolean value, String what) { if (!value) throw new AssertionError(what); }
    public static void main(String[] args) throws Exception {
        Class<?> type;
        try { type = Class.forName("org.s2arm.android.SystemPowerSampler"); }
        catch (ClassNotFoundException absent) { throw new AssertionError("BatteryManager sampler is missing", absent); }
        Constructor<?> ctor = type.getDeclaredConstructor(Context.class, Handler.class, DoubleConsumer.class);
        ctor.setAccessible(true);
        Method start = type.getDeclaredMethod("start"), stop = type.getDeclaredMethod("stop");
        start.setAccessible(true); stop.setAccessible(true);
        Context context = new Context(); Handler handler = new Handler(); ArrayList<Double> output = new ArrayList<>();
        context.battery.putExtra(BatteryManager.EXTRA_STATUS, 3).putExtra(BatteryManager.EXTRA_PLUGGED, 0)
            .putExtra(BatteryManager.EXTRA_VOLTAGE, 4000);
        Object sampler = ctor.newInstance(context, handler, (DoubleConsumer) output::add);
        start.invoke(sampler); handler.runPending();
        check(output.size() == 1 && Math.abs(output.get(0) - 4.0) < 0.000001, "real adapter read / unit conversion");
        context.battery.putExtra(BatteryManager.EXTRA_PLUGGED, 1);
        handler.runPending();
        check(output.size() == 2 && Double.isNaN(output.get(1)), "plugged invalidates power");
        Runnable stale = handler.pending; int reads = context.reads;
        stop.invoke(sampler);
        check(handler.pending == null, "pause removes callback");
        stale.run();
        check(context.reads == reads && output.size() == 2, "late callback does not sample");
        context.battery.putExtra(BatteryManager.EXTRA_PLUGGED, 0);
        context.manager.current = Long.MIN_VALUE;
        start.invoke(sampler); handler.runPending();
        check(output.size() == 3 && Double.isNaN(output.get(2)), "unsupported property stays unavailable");
        // debug.s2a.power_readout=0：不碰 BatteryManager / registerReceiver，读数固定不可用
        android.os.SystemProperties.values.put("debug.s2a.power_readout", "0");
        context.manager.current = -1000000;
        int reads0 = context.reads;
        handler.runPending();
        check(output.size() == 4 && Double.isNaN(output.get(3)) && context.reads == reads0,
            "power_readout=0 publishes unavailable without reading battery");
        android.os.SystemProperties.values.clear();
        handler.runPending();
        check(output.size() == 5 && Math.abs(output.get(4) - 4.0) < 0.000001, "switch back on resumes real reads");
        stop.invoke(sampler);
        System.out.println("BatteryManager sampling / charging / stop / resume / readout switch passed");
    }
}
