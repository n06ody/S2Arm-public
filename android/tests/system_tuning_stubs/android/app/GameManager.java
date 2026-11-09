package android.app;
public class GameManager {
    public static final int GAME_MODE_UNSUPPORTED = 0;
    public static final int GAME_MODE_STANDARD = 1;
    public static final int GAME_MODE_PERFORMANCE = 2;
    public static final int GAME_MODE_BATTERY = 3;
    public int mode = GAME_MODE_STANDARD;
    public int getGameMode() { return mode; }
}
