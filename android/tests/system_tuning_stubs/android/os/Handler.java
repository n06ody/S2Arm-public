package android.os;
public class Handler {
    public Runnable pending;
    public long delay;
    public boolean post(Runnable task) { pending = task; delay = 0; return true; }
    public boolean postDelayed(Runnable task, long delay) { pending = task; this.delay = delay; return true; }
    public void removeCallbacks(Runnable task) { if (pending == task) pending = null; }
    public void runPending() { Runnable task = pending; pending = null; if (task != null) task.run(); }
}
