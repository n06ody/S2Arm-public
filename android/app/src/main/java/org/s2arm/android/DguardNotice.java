package org.s2arm.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.util.Log;
import java.util.concurrent.CountDownLatch;

/**
 * 直连安全网提示页（安卓侧 UI 胶水；判定与记录在共用 runtime/src/dguard.c）：
 * 连续失败满限时 native 的 s2a_android_dguard_prompt 经 JNI 调进来，游戏线程阻塞
 * 在这里，玩家选「仍然启动」才返回——同 iOS 提示页只有一个出口。
 * 标题与按钮文案与 runtime/include/s2a_dguard.h 对齐（提示语是安卓版路径，
 * 共用头里那条是 Mac/iPhone 的安装口径）；spec 检查只认 S2A_DPROMPT_* / S2A_DGUARD_*
 * 宏，平台差异文案不进共用头。
 */
final class DguardNotice {
    private static final String TITLE = "这款游戏最近几次都没能启动";
    private static final String MESSAGE = "请把 S2Arm 更新到最新版再进入这款游戏；还是不行，请点「复制诊断」把诊断信息发给我们反馈。";
    private static final String COPY = "复制诊断";
    private static final String GO = "仍然启动";

    private final Activity activity;
    private final Object lock = new Object();
    private boolean active = true;
    private CountDownLatch pending;
    private AlertDialog dialog;   // UI thread only

    DguardNotice(Activity activity) { this.activity = activity; }

    void resume() { synchronized (lock) { active = true; } }
    void pause() {
        CountDownLatch latch;
        synchronized (lock) { active = false; latch = pending; }
        if (latch != null) latch.countDown();   // Activity 退走就当「仍然启动」继续：提示是提醒不是闸门
        activity.runOnUiThread(() -> { if (dialog != null) dialog.dismiss(); });
    }

    /** Blocks the caller (game thread) until the player taps 仍然启动 or the Activity pauses. */
    void show(String diag) {
        if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) return;   // 主线程等闩死锁，不该到这里
        final CountDownLatch done = new CountDownLatch(1);
        synchronized (lock) {
            if (!active) return;
            pending = done;
        }
        activity.runOnUiThread(() -> {
            synchronized (lock) {
                if (!active || pending != done || activity.isFinishing() || activity.isDestroyed()) {
                    done.countDown();
                    return;
                }
            }
            try {
                AlertDialog d = new AlertDialog.Builder(activity)
                    .setTitle(TITLE)
                    .setMessage(MESSAGE)
                    .setNeutralButton(COPY, null)   // 复制不关页：下面覆盖成不 dismiss 的按钮
                    .setPositiveButton(GO, (which, button) -> done.countDown())
                    .create();
                d.setOnDismissListener(which -> { dialog = null; done.countDown(); });
                d.setCancelable(false);
                dialog = d;
                d.show();
                d.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> {
                    ClipboardManager clipboard = (ClipboardManager) activity.getSystemService(Context.CLIPBOARD_SERVICE);
                    if (clipboard != null) clipboard.setPrimaryClip(ClipData.newPlainText("S2Arm dguard", diag));
                    android.widget.Toast.makeText(activity, COPY, android.widget.Toast.LENGTH_SHORT).show();
                });
            } catch (RuntimeException error) {
                dialog = null;
                done.countDown();
                Log.e("S2Arm", "dguard notice could not be shown", error);
            }
        });
        try { done.await(); }
        catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); }
        synchronized (lock) { if (pending == done) pending = null; }
    }
}
