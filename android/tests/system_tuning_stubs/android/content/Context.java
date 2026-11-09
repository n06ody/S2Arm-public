package android.content;
public class Context {
    public android.app.GameManager gameManager = new android.app.GameManager();
    public <T> T getSystemService(Class<T> type) { return type.cast(gameManager); }
}
