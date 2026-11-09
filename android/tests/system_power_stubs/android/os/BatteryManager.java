package android.os;
public class BatteryManager {
    public static final int BATTERY_PROPERTY_CURRENT_NOW = 2;
    public static final String EXTRA_STATUS = "status", EXTRA_PLUGGED = "plugged", EXTRA_VOLTAGE = "voltage";
    public long current = -1000000;
    public long getLongProperty(int property) { return current; }
}
