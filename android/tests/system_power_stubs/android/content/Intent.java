package android.content;
public class Intent {
    public static final String ACTION_BATTERY_CHANGED = "android.intent.action.BATTERY_CHANGED";
    private final java.util.Map<String, Integer> values = new java.util.HashMap<>();
    public Intent putExtra(String key, int value) { values.put(key, value); return this; }
    public int getIntExtra(String key, int fallback) { Integer value = values.get(key); return value == null ? fallback : value; }
}
