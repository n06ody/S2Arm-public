package android.os;
public class PowerMonitor {
    public static final int POWER_MONITOR_TYPE_CONSUMER = 0;
    public static final int POWER_MONITOR_TYPE_MEASUREMENT = 1;
    private final String name;
    private final int type;
    public PowerMonitor(String name, int type) { this.name = name; this.type = type; }
    public String getName() { return name; }
    public int getType() { return type; }
}
