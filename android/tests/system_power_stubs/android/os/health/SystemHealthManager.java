package android.os.health;
import android.os.OutcomeReceiver;
import android.os.PowerMonitor;
import android.os.PowerMonitorReadings;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.function.Consumer;
public class SystemHealthManager {
    public List<PowerMonitor> monitors = new ArrayList<>();
    public final Deque<Object> results = new ArrayDeque<>();
    public boolean supportedCalled;
    public int readingsCalls;
    public Consumer<List<PowerMonitor>> pendingDiscovery;
    public OutcomeReceiver<PowerMonitorReadings, RuntimeException> pendingReadings;
    public void getSupportedPowerMonitors(Executor executor, Consumer<List<PowerMonitor>> receiver) {
        supportedCalled = true;
        pendingDiscovery = receiver;
    }
    public void getPowerMonitorReadings(List<PowerMonitor> monitors,
            Executor executor, OutcomeReceiver<PowerMonitorReadings, RuntimeException> receiver) {
        readingsCalls++;
        pendingReadings = receiver;
    }
    // 测试驱动：把编好的结果喂给挂起的回调
    public void flushDiscovery() {
        Consumer<List<PowerMonitor>> receiver = pendingDiscovery;
        pendingDiscovery = null;
        if (receiver != null) receiver.accept(monitors);
    }
    public void flushReadings() {
        OutcomeReceiver<PowerMonitorReadings, RuntimeException> receiver = pendingReadings;
        pendingReadings = null;
        Object result = results.poll();
        if (receiver == null || result == null) return;
        if (result instanceof RuntimeException) receiver.onError((RuntimeException) result);
        else receiver.onResult((PowerMonitorReadings) result);
    }
}
