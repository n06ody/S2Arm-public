package org.s2arm.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.view.Display;

/**
 * Phone as the handheld's six-axis sensor and rumble motor (#528 item 10, piece 1), as iOS ios/phone_motion.m.
 * Native side: native/phone_feedback.c (ASensor for motion; rumble comes back here through {@link #vibrate}
 * on the rumble mailbox thread, the NDK has no vibrator API). Settings live in the in-game four-finger menu,
 * both on by default like iOS. External controllers' own motors and sensors are piece 2, not here.
 */
final class PhoneFeedback implements DisplayManager.DisplayListener {
    static final String PREF_GYRO = "phone_gyro", PREF_RUMBLE = "phone_rumble";
    private static volatile Vibrator vibrator;
    private static volatile boolean amplitudeControl;
    private final Context context;
    private final SharedPreferences prefs;
    private final DisplayManager displays;
    private final java.util.function.BooleanSupplier gamepad;
    private boolean listening;

    PhoneFeedback(Context context, SharedPreferences prefs, java.util.function.BooleanSupplier gamepad) {
        this.context = context;
        this.prefs = prefs;
        this.gamepad = gamepad;
        this.displays = context.getSystemService(DisplayManager.class);
        Vibrator v;
        if (Build.VERSION.SDK_INT >= 31) {
            android.os.VibratorManager manager = context.getSystemService(android.os.VibratorManager.class);
            v = manager != null ? manager.getDefaultVibrator() : null;
        } else {
            v = context.getSystemService(Vibrator.class);
        }
        vibrator = v != null && v.hasVibrator() ? v : null;
        amplitudeControl = vibrator != null && vibrator.hasAmplitudeControl();
    }

    boolean gyro() { return prefs.getBoolean(PREF_GYRO, true); }
    boolean rumble() { return prefs.getBoolean(PREF_RUMBLE, true); }
    boolean hasVibrator() { return vibrator != null; }

    void setGyro(boolean on) { prefs.edit().putBoolean(PREF_GYRO, on).apply(); publish(); }
    void setRumble(boolean on) { prefs.edit().putBoolean(PREF_RUMBLE, on).apply(); publish(); }

    /** Main thread, game mode, native library loaded: settings + current rotation to native, follow rotation. */
    void resume() {
        publish();
        if (displays != null && !listening) {
            displays.registerDisplayListener(this, null);
            listening = true;
        }
        onDisplayChanged(Display.DEFAULT_DISPLAY);
    }
    void pause() {
        if (displays != null && listening) displays.unregisterDisplayListener(this);
        listening = false;
    }
    /** Also on input device changes: whether a gamepad is attached decides if the phone motor may rumble. */
    void publish() {
        NativeBridge.configurePhoneFeedback(context.getPackageName(), gyro(), rumble(), hasVibrator(), gamepad.getAsBoolean());
    }

    // 180° turns (landscape <-> reverse landscape) keep the surface size, so surfaceChanged cannot be relied on here.
    @Override public void onDisplayChanged(int displayId) {
        if (displayId != Display.DEFAULT_DISPLAY || displays == null) return;
        Display display = displays.getDisplay(displayId);
        if (display != null) NativeBridge.setDisplayRotation(display.getRotation());
    }
    @Override public void onDisplayAdded(int displayId) { }
    @Override public void onDisplayRemoved(int displayId) { }

    /**
     * Called from native (phone_feedback.c, rumble mailbox thread): amplitude 1..255 starts a one-shot of
     * leaseMs (renewed by native while the game keeps rumbling, so a stuck process stops within the lease);
     * 0 cancels. Devices without amplitude control vibrate at their default strength.
     */
    @SuppressWarnings("deprecation")
    static void vibrate(int amplitude, int leaseMs) {
        Vibrator v = vibrator;
        if (v == null) return;
        // On/off motors: weak requests would come out at full default strength, so they stay off (~1/8 and below).
        if (amplitude <= 0 || (!amplitudeControl && amplitude < 32)) { v.cancel(); return; }
        VibrationEffect effect = VibrationEffect.createOneShot(leaseMs,
            amplitudeControl ? Math.min(255, amplitude) : VibrationEffect.DEFAULT_AMPLITUDE);
        if (Build.VERSION.SDK_INT >= 33) {
            v.vibrate(effect, android.os.VibrationAttributes.createForUsage(android.os.VibrationAttributes.USAGE_MEDIA));
        } else {
            v.vibrate(effect, new android.media.AudioAttributes.Builder()
                .setUsage(android.media.AudioAttributes.USAGE_GAME).build());
        }
    }
}
