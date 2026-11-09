package org.s2arm.android;

import android.content.Context;
import android.os.OutcomeReceiver;
import android.os.PowerMonitor;
import android.os.PowerMonitorReadings;
import android.os.SystemClock;
import android.os.health.SystemHealthManager;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.function.Consumer;

/**
 * #163 Android 15+（API 35）：SystemHealthManager 的 CPU/GPU 功耗轨读数。
 * 只选名字形如 [CPU/n]、[GPU/n] 的 CONSUMER 监视器——是轨功耗不是整机功耗，
 * 两类缺一就整条路径不用（宁缺毋滥）。轨功耗插电也可读，是 8E 这类
 * 电池电流报正号机型上的主要读数通道；读回的是能量差分/时间 = 平均功率。
 */
final class SystemPowerRails {
    private static final long STALE_MS = 6000;    // 上一次成功样本超过 6 秒按不可用报
    private final SystemHealthManager health;
    private final Executor executor;
    private final Consumer<String> events;
    private final List<PowerMonitor> selected = new ArrayList<>();
    private PowerMonitorReadings previous;
    private boolean discovered, pending, dead;
    private int generation;    // 停表 +1：作废晚到的 select/readings 回调（按旧代次丢弃）
    private double watts = Double.NaN;
    private long sampledAt;

    private SystemPowerRails(SystemHealthManager health, Executor executor, Consumer<String> events) {
        this.health = health;
        this.executor = executor;
        this.events = events;
    }

    static SystemPowerRails create(Context context, Consumer<String> events) {
        return new SystemPowerRails(context.getSystemService(SystemHealthManager.class),
            context.getMainExecutor(), events);
    }

    void stop() {
        generation++;
        selected.clear();
        previous = null;
        discovered = pending = dead = false;
        watts = Double.NaN;
    }

    // 每秒一拍：首次发现监视器 → 以后每拍发一次读数请求 → 报最近一次样本（6 秒内）。
    double poll() {
        if (health == null || dead) return Double.NaN;
        if (!discovered) {
            discovered = true;
            int gen = generation;
            health.getSupportedPowerMonitors(executor,
                monitors -> { if (gen == generation) select(monitors); });
            return Double.NaN;
        }
        if (selected.isEmpty() || pending) return last();
        pending = true;
        int gen = generation;
        health.getPowerMonitorReadings(selected, executor,
            new OutcomeReceiver<PowerMonitorReadings, RuntimeException>() {
                @Override public void onResult(PowerMonitorReadings readings) {
                    if (gen != generation) return;
                    pending = false;
                    onReadings(readings);
                }
                @Override public void onError(RuntimeException error) {
                    if (gen != generation) return;
                    pending = false;
                    previous = null;    // 能量计数可能回绕，丢一次基线重采
                    watts = Double.NaN;
                }
            });
        return last();
    }

    private double last() {
        if (!Double.isFinite(watts)) return Double.NaN;
        return SystemClock.elapsedRealtime() - sampledAt < STALE_MS ? watts : Double.NaN;
    }

    private void select(List<PowerMonitor> monitors) {
        int cpu = 0, gpu = 0;
        StringBuilder names = new StringBuilder();
        for (PowerMonitor monitor : monitors) {
            names.append(monitor.getName()).append(' ');
            if (monitor.getType() != PowerMonitor.POWER_MONITOR_TYPE_CONSUMER) continue;
            if (monitor.getName().matches("\\[CPU/\\d+\\]")) { selected.add(monitor); cpu++; }
            else if (monitor.getName().matches("\\[GPU/\\d+\\]")) { selected.add(monitor); gpu++; }
        }
        if (cpu == 0 || gpu == 0) { selected.clear(); dead = true; }
        events.accept("power-rails monitors=" + names + "selected cpu=" + cpu + " gpu=" + gpu);
    }

    // 能量差分 / 各轨自己的采样间隔 = 该轨平均功率；任何一轨无效整帧丢
    private void onReadings(PowerMonitorReadings readings) {
        if (previous == null) { previous = readings; return; }
        double sum = 0;
        for (PowerMonitor monitor : selected) {
            long energy = readings.getConsumedEnergy(monitor);
            long old = previous.getConsumedEnergy(monitor);
            long elapsed = readings.getTimestampMillis(monitor) - previous.getTimestampMillis(monitor);
            if (energy < 0 || old < 0 || energy < old || elapsed <= 0) {
                previous = readings;
                watts = Double.NaN;
                return;
            }
            sum += (double) (energy - old) / (elapsed * 1000.0);   // µJ / (ms·1000) = W
        }
        previous = readings;
        watts = sum;
        sampledAt = SystemClock.elapsedRealtime();
    }
}
