package android.content;
import android.os.health.SystemHealthManager;
import java.util.concurrent.Executor;
public class Context {
    public android.os.BatteryManager manager = new android.os.BatteryManager();
    public SystemHealthManager health = new SystemHealthManager();
    public Intent battery = new Intent();
    public int reads;
    public final Executor mainExecutor = Runnable::run;
    @SuppressWarnings("unchecked")
    public <T> T getSystemService(Class<T> type) {
        if (type.isInstance(manager)) return (T) manager;
        if (type.isInstance(health)) return (T) health;
        return null;
    }
    public Executor getMainExecutor() { return mainExecutor; }
    public Intent registerReceiver(Object receiver, IntentFilter filter) { reads++; return battery; }
}
