package android.content;
public class Context {
    public android.os.BatteryManager manager = new android.os.BatteryManager();
    public Intent battery = new Intent();
    public int reads;
    public <T> T getSystemService(Class<T> type) { return type.cast(manager); }
    public Intent registerReceiver(Object receiver, IntentFilter filter) { reads++; return battery; }
}
