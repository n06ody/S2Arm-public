package org.s2arm.android;

import android.content.Context;
import android.os.Build;
import android.os.BatteryManager;
import android.os.Handler;
import android.os.PowerMonitor;
import android.os.PowerMonitorReadings;
import android.os.SystemClock;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Consumer;

/** SystemHealthManager CPU/GPU consumer rails: discovery selection, energy delta, staleness. */
public final class SystemPowerRailsTest {
    private static int failures;
    private static void check(boolean value, String what) { if (!value) { System.err.println("FAIL " + what); failures++; } }
    private static void checkNear(double got, double expected, String what) {
        check(Math.abs(got - expected) < 0.000001, what + " expected " + expected + ", got " + got);
    }
    public static void main(String[] args) throws Exception {
        Class<?> railsType;
        try { railsType = Class.forName("org.s2arm.android.SystemPowerRails"); }
        catch (ClassNotFoundException absent) { throw new AssertionError("SystemHealthManager rails path is missing", absent); }
        Method create = railsType.getDeclaredMethod("create", Context.class, Consumer.class);
        Method poll = railsType.getDeclaredMethod("poll"), stop = railsType.getDeclaredMethod("stop");
        create.setAccessible(true); poll.setAccessible(true); stop.setAccessible(true);
        Build.VERSION.SDK_INT = 35;

        // 监视器清单：CPU+GPU 消费轨才入选，MEASUREMENT 与其它名字排除
        Context context = new Context();
        ArrayList<String> events = new ArrayList<>();
        PowerMonitor cpu0 = new PowerMonitor("[CPU/0]", PowerMonitor.POWER_MONITOR_TYPE_CONSUMER);
        PowerMonitor cpu1 = new PowerMonitor("[CPU/1]", PowerMonitor.POWER_MONITOR_TYPE_CONSUMER);
        PowerMonitor gpu0 = new PowerMonitor("[GPU/0]", PowerMonitor.POWER_MONITOR_TYPE_CONSUMER);
        context.health.monitors.add(cpu0);
        context.health.monitors.add(cpu1);
        context.health.monitors.add(gpu0);
        context.health.monitors.add(new PowerMonitor("ODPM_VDD", PowerMonitor.POWER_MONITOR_TYPE_MEASUREMENT));
        context.health.monitors.add(new PowerMonitor("[CPU/9]", PowerMonitor.POWER_MONITOR_TYPE_MEASUREMENT));
        context.health.monitors.add(new PowerMonitor("[OTHER/0]", PowerMonitor.POWER_MONITOR_TYPE_CONSUMER));
        Object rails = create.invoke(null, context, (Consumer<String>) events::add);
        SystemClock.now = 1000;

        check(Double.isNaN((double) poll.invoke(rails)), "first poll issues discovery");
        check(context.health.supportedCalled && context.health.pendingDiscovery != null, "discovery requested");
        check(context.health.readingsCalls == 0, "no readings before discovery completes");
        context.health.flushDiscovery();
        check(events.stream().anyMatch(l -> l.contains("power-rails")), "discovery event logged");
        check(events.get(0).contains("cpu=2") && events.get(0).contains("gpu=1"), "selection counts in event");

        // 读数流程：首帧只建基线，第二帧才有功率
        check(Double.isNaN((double) poll.invoke(rails)), "poll requests readings, still no watts");
        check(context.health.readingsCalls == 1, "one readings request in flight");
        context.health.results.add(new PowerMonitorReadings()
            .put(cpu0, 1000000, 1000).put(cpu1, 500000, 1000).put(gpu0, 300000, 1000));
        context.health.flushReadings();
        check(Double.isNaN((double) poll.invoke(rails)), "baseline sample carries no watts");
        context.health.results.add(new PowerMonitorReadings()
            .put(cpu0, 1600000, 2000).put(cpu1, 560000, 2000).put(gpu0, 600000, 2000));
        context.health.flushReadings();
        checkNear((double) poll.invoke(rails), 0.96, "delta energy over delta time sums rails");

        // 插电状态下 rails 路径照常工作，且不碰电池广播
        check(context.reads == 0, "rails path never touches battery broadcast");

        // 样本过旧（>6 s）回落不可用
        SystemClock.now = 9100;
        check(Double.isNaN((double) poll.invoke(rails)), "stale watts fall back to unavailable");

        // 读数错误清掉基线，下一轮重采
        context.health.results.add(new RuntimeException("rail read failed"));
        context.health.flushReadings();
        check(Double.isNaN((double) poll.invoke(rails)), "readings error clears watts");

        // 缺 GPU 轨：发现即判死，不再发读数请求
        Context cpuOnly = new Context();
        cpuOnly.health.monitors.add(new PowerMonitor("[CPU/0]", PowerMonitor.POWER_MONITOR_TYPE_CONSUMER));
        Object dead = create.invoke(null, cpuOnly, (Consumer<String>) s -> { });
        poll.invoke(dead);
        cpuOnly.health.flushDiscovery();
        check(cpuOnly.health.readingsCalls == 0 && Double.isNaN((double) poll.invoke(dead)),
            "missing GPU rail disables the path");
        check(cpuOnly.health.readingsCalls == 0, "dead path never requests readings");

        // 停表后晚到的发现回调必须丢弃：否则恢复后同一轨登记两次、功率翻倍
        Context late = new Context();
        late.health.monitors.add(cpu0);
        late.health.monitors.add(cpu1);
        late.health.monitors.add(gpu0);
        Object rails2 = create.invoke(null, late, (Consumer<String>) events::add);
        poll.invoke(rails2);
        java.util.function.Consumer<List<PowerMonitor>> stale = late.health.pendingDiscovery;
        late.health.pendingDiscovery = null;
        stop.invoke(rails2);
        poll.invoke(rails2);                        // 恢复后重新发现
        late.health.flushDiscovery();               // 新代次回调正常入选 3 轨
        stale.accept(late.health.monitors);         // 旧代次晚到：应被丢弃
        poll.invoke(rails2);
        late.health.results.add(new PowerMonitorReadings()
            .put(cpu0, 1000000, 1000).put(cpu1, 500000, 1000).put(gpu0, 300000, 1000));
        late.health.flushReadings();
        poll.invoke(rails2);
        late.health.results.add(new PowerMonitorReadings()
            .put(cpu0, 1600000, 2000).put(cpu1, 560000, 2000).put(gpu0, 600000, 2000));
        late.health.flushReadings();
        checkNear((double) poll.invoke(rails2), 0.96,
            "stale discovery callback after stop must not re-register rails");

        // 采样器 Sink 走线：低频原始电流/状态日志 + rails 通道逐拍下发
        Class<?> samplerType = Class.forName("org.s2arm.android.SystemPowerSampler");
        Class<?> sinkType = Class.forName("org.s2arm.android.SystemPowerSampler$Sink");
        ArrayList<Double> batteryOut = new ArrayList<>(), railsOut = new ArrayList<>();
        events.clear();
        Object sink = Proxy.newProxyInstance(SystemPowerRailsTest.class.getClassLoader(),
            new Class<?>[] { sinkType }, (proxy, method, argv) -> {
                switch (method.getName()) {
                    case "battery": batteryOut.add((Double) argv[0]); return null;
                    case "rails": railsOut.add((Double) argv[0]); return null;
                    case "event": events.add((String) argv[0]); return null;
                    default: return null;
                }
            });
        Constructor<?> ctor = samplerType.getDeclaredConstructor(Context.class, Handler.class, sinkType);
        ctor.setAccessible(true);
        Context ctx = new Context();
        ctx.battery.putExtra(BatteryManager.EXTRA_STATUS, BatteryManager.BATTERY_STATUS_DISCHARGING)
            .putExtra(BatteryManager.EXTRA_PLUGGED, 0)
            .putExtra(BatteryManager.EXTRA_VOLTAGE, 4000);
        ctx.manager.current = 1500000;
        Handler handler = new Handler();
        Object sampler = ctor.newInstance(ctx, handler, sink);
        Method start = samplerType.getDeclaredMethod("start"); start.setAccessible(true);
        SystemClock.now = 0;
        start.invoke(sampler);
        for (int i = 0; i < 30; i++) { SystemClock.now = i * 1000L; handler.runPending(); }
        check(events.stream().anyMatch(l -> l.contains("battery-raw")
                && l.contains("current_ua=1500000") && l.contains("status=3") && l.contains("plugged=0")),
            "low-frequency raw status line carries original current sign");
        check(railsOut.size() == 30, "rails channel emits every tick");
        check(batteryOut.get(0).isNaN(), "battery channel still settle-gated");

        if (failures != 0) throw new AssertionError(failures + " rails checks failed");
        System.out.println("SystemHealthManager rails discovery / delta / staleness / error / sink wiring passed");
    }
}
