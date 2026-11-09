package org.s2arm.android;

import android.os.Build;
import android.view.Surface;

/** Main-thread display-mode hint. This vote does not throttle rendering. */
final class FrameRateVote {
    private Surface surface;
    private float last = Float.NaN;

    void attach(Surface next) {
        detach();
        surface = next;
        last = Float.NaN;
    }

    void target(float fps) {
        if (surface == null || !surface.isValid() || Build.VERSION.SDK_INT < 30) return;
        float valid = Float.isNaN(fps) || Float.isInfinite(fps) || fps <= 0 ? 0 : fps;
        if (Float.compare(last, valid) == 0) return;
        try {
            surface.setFrameRate(valid, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
            last = valid;
        } catch (RuntimeException error) {
            System.err.println("[s2a] display frame-rate vote failed: " + error);
        }
    }

    void detach() {
        if (surface != null && !Float.isNaN(last) && last > 0) target(0);
        surface = null;
        last = Float.NaN;
    }
}
