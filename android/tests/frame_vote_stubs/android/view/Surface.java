package android.view;
public class Surface {
    public static final int FRAME_RATE_COMPATIBILITY_DEFAULT = 0;
    public boolean valid = true;
    public int calls, compatibility;
    public float value;
    public boolean isValid() { return valid; }
    public void setFrameRate(float fps, int mode) { calls++; value = fps; compatibility = mode; }
}
