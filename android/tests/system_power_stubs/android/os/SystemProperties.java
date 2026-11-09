package android.os;
import java.util.HashMap;
import java.util.Map;
public class SystemProperties {
    public static final Map<String, String> values = new HashMap<>();
    public static String get(String key, String fallback) { return values.getOrDefault(key, fallback); }
}
