package android.os;
public class BatteryManager {
    public static final int BATTERY_PROPERTY_CURRENT_NOW = 2;
    public static final int BATTERY_STATUS_UNKNOWN = 1;
    public static final int BATTERY_STATUS_CHARGING = 2;
    public static final int BATTERY_STATUS_DISCHARGING = 3;
    public static final int BATTERY_STATUS_NOT_CHARGING = 4;
    public static final int BATTERY_STATUS_FULL = 5;
    public static final String EXTRA_STATUS = "status", EXTRA_PLUGGED = "plugged", EXTRA_VOLTAGE = "voltage";
    public long current = -1000000;
    public long getLongProperty(int property) { return current; }
}
