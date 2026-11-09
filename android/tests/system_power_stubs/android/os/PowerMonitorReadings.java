package android.os;
import java.util.HashMap;
import java.util.Map;
public class PowerMonitorReadings {
    public static final int ENERGY_UNAVAILABLE = -1;
    private final Map<PowerMonitor, long[]> values = new HashMap<>();
    public PowerMonitorReadings put(PowerMonitor monitor, long energy, long timestampMillis) {
        values.put(monitor, new long[] { energy, timestampMillis });
        return this;
    }
    public long getConsumedEnergy(PowerMonitor monitor) {
        long[] value = values.get(monitor);
        return value == null ? ENERGY_UNAVAILABLE : value[0];
    }
    public long getTimestampMillis(PowerMonitor monitor) {
        long[] value = values.get(monitor);
        return value == null ? -1 : value[1];
    }
}
