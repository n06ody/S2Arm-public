package org.s2arm.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

/**
 * Draws TouchPad over the game surface and feeds it the touches. Built in code only (no XML
 * constructor), and a gamepad surface rather than a clickable control: every touch is pad input,
 * so there is no click to report.
 */
@android.annotation.SuppressLint({"ViewConstructor", "ClickableViewAccessibility"})
final class TouchPadView extends View {
    interface Listener {
        void onTouchPad(TouchPad pad);
        /** Exactly four fingers held for MENU_HOLD_MS (the iOS gesture): the in-game touch menu. */
        default void onTouchMenu() { }
    }
    static final long MENU_HOLD_MS = 1500;

    private final TouchPad pad = new TouchPad();
    private final Listener listener;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ring = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private int lastButtons;
    private final long[] viewport = new long[8];
    private final GameTouch gameTouch = new GameTouch(this::publishGameTouch);
    private boolean controlsVisible = true;
    private boolean trace;
    private long firstLayoutNs, latestLayoutNs;
    private boolean editing;
    private int opacityPercent = 100;

    int opacityPercent() { return opacityPercent; }
    void setOpacityPercent(int percent) {
        opacityPercent = Math.max(0, Math.min(100, percent));
        invalidate();
    }

    void scaleEditingButtons(double factor) {
        if (!editing) return;
        // Snapshot drags before rebuilding controls; Save/Cancel still own persistence.
        setLayout(pad.editedLayout(TouchLayoutStore.MINE, "My layout").withButtonScale(factor));
    }

    private int appearanceColor(int color) {
        if (editing) return color; // keep controls discoverable even at zero runtime opacity
        int alpha = ((color >>> 24) * opacityPercent + 50) / 100;
        return (color & 0x00FFFFFF) | (alpha << 24);
    }
    private TouchPad.Control dragging;
    private int dragPointer = -1;
    private float dragX, dragY;
    interface EditDragListener { void onEditDrag(boolean dragging); }
    private EditDragListener editDrag = dragging -> { };
    /** The editor bar fades while a control is dragged, so a control can pass under it. */
    void setEditDragListener(EditDragListener listener) { editDrag = listener; }
    private final Runnable menuHold = this::openMenu;
    void setTrace(boolean enabled) { trace = enabled; TouchInputBridge.touchTrace(enabled); }

    void layoutEvidence(String event, String detail) {
        if (!trace) return;
        android.util.Log.i("S2ArmTouchLayout", event + " monotonic_ns=" + System.nanoTime()
            + " view=" + System.identityHashCode(this) + " generation=" + pad.layouts
            + " controls=" + pad.controls().size() + " first_layout_ns=" + firstLayoutNs
            + " latest_layout_ns=" + latestLayoutNs + " " + detail);
    }

    boolean gameTouchEnabled() { return gameTouch.enabled(); }
    boolean controlsVisible() { return controlsVisible; }
    void setControlsVisible(boolean visible) {
        if (controlsVisible == visible) return;
        release();
        controlsVisible = visible;
        invalidate();
    }

    void setGameTouchEnabled(boolean enabled) {
        release();
        int result = TouchInputBridge.touchEnable(enabled);
        if (result != 0) throw new IllegalStateException("experimental touch unavailable: " + result);
        gameTouch.enable(enabled);
    }

    // A-W6 candidate: Switch touch region, excluding widescreen extensions.
    // Handheld mode only; compositor supplies geometry/rotation, never guessed here.
    // The diagnostic presenter intentionally never calls it.
    void setGameViewport(long generation, long revision, float left, float top, float width, float height) {
        gameTouch.viewport(generation, revision, left, top, width, height);
    }

    void revokeGameViewport() {
        gameTouch.revokeViewport();
        TouchInputBridge.touchViewportRevoke();
    }

    void retireGameViewport() {
        gameTouch.revokeViewport();
        TouchInputBridge.touchViewportRetire();
    }

    private int publishGameTouch(int[] points, int count, int flags, long ns) {
        int result = TouchInputBridge.touchFrame(points, count, flags, ns, viewport[0], viewport[1]);
        if (trace) android.util.Log.i("S2Arm", "touch publish: count=" + count + " flags=" + flags
            + " result=" + result + (count == 0 ? "" : " first=" + points[0] + ":" + points[1] + "," + points[2]));
        // A compositor change raced this event. Native code already cancelled
        // the old contacts; do not publish coordinates from the previous image.
        if (result == -3) return 0;
        return result;
    }

    private void refreshGameViewport() {
        if (!gameTouch.enabled()) return;
        int result = TouchInputBridge.touchViewport(viewport);
        if (trace) android.util.Log.i("S2Arm", "touch viewport: result=" + result + " generation=" + viewport[0]
            + " revision=" + viewport[1] + " rect=" + viewport[2] + "," + viewport[3] + "," + viewport[4] + "," + viewport[5]
            + " buffer=" + viewport[6] + "x" + viewport[7] + " view=" + getWidth() + "x" + getHeight());
        // viewport[6,7] 是发布坐标系 = 画面 surface 的像素尺寸。本 view 铺满整个窗口
        // （虚拟键留在两侧黑边上），画面 surface 在窗口里居中，所以发布的矩形平移
        // 半个边差即本 view 坐标；surface 还没被布进窗口时撤销等下一次。
        long bw = viewport[6], bh = viewport[7];
        if (result == -4 || bw <= 0 || bh <= 0 || bw > getWidth() || bh > getHeight()) {
            gameTouch.revokeViewport();
            return;
        }
        if (result != 0) throw new IllegalStateException("touch viewport unavailable: " + result);
        setGameViewport(viewport[0], viewport[1],
                viewport[2] + (getWidth() - bw) / 2, viewport[3] + (getHeight() - bh) / 2,
                viewport[4], viewport[5]);
    }

    private void movePointer(int id, float x, float y) {
        TouchRouting.move(pad, gameTouch, id, x, y);
    }

    TouchPadView(Context context, Listener listener) {
        super(context);
        this.listener = listener;
        ring.setStyle(Paint.Style.STROKE);
        ring.setStrokeWidth(3 * context.getResources().getDisplayMetrics().density);
        ring.setColor(0x99FFFFFF);
        text.setColor(0xCCFFFFFF);
        text.setTextAlign(Paint.Align.CENTER);
    }

    private int insetLeft, insetTop, insetRight, insetBottom;

    TouchPad pad() { return pad; }

    /** Use a player layout (null = built-in) and lay it out now. */
    void setLayout(TouchLayout layout) {
        pad.setLayout(layout);
        relayout();
    }

    boolean editing() { return editing; }

    /** Drag mode: every control can be moved; nothing reaches the game or the pad's input state. */
    void beginEditing() {
        release();
        editing = true;
        invalidate();
    }

    /** Leave drag mode; the caller saves pad().editedLayout(...) first, or drops the drags with setLayout. */
    void endEditing() {
        editing = false;
        dragging = null;
        dragPointer = -1;
        relayout();
    }

    /** System bars / cutouts: keep the controls out from under them. */
    void setInsets(int left, int top, int right, int bottom) {
        insetLeft = left; insetTop = top; insetRight = right; insetBottom = bottom;
        relayout();
    }

    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) { relayout(); }

    private void relayout() {
        dragging = null;   // layout() rebuilds the controls; an editor drag restarts from the new places
        dragPointer = -1;
        if (getWidth() == 0 || getHeight() == 0) return;
        pad.cancel();
        revokeGameViewport();
        pad.layout(insetLeft, insetTop, getWidth() - insetLeft - insetRight, getHeight() - insetTop - insetBottom);
        latestLayoutNs = System.nanoTime();
        if (pad.layouts == 1) firstLayoutNs = latestLayoutNs;
        layoutEvidence("built", "");
        listener.onTouchPad(pad);
        invalidate();
    }

    @Override public boolean onTouchEvent(MotionEvent e) {
        if (editing) return handleEditEvent(e);
        watchMenuHold(e);
        try {
            return handleTouchEvent(e);
        } catch (IllegalArgumentException | IllegalStateException error) {
            // Invalid/overflowing contacts or a failed JNI publisher cancel the
            // experiment; they must not crash the Activity or leave controls held.
            gameTouch.enable(false);
            TouchInputBridge.touchEnable(false);
            release();
            android.util.Log.e("S2Arm", "Experimental touch disabled: " + error.getMessage());
            return true;
        }
    }

    private boolean handleTouchEvent(MotionEvent e) {
        refreshGameViewport();
        int index = e.getActionIndex();
        // Pointer transitions also carry updated positions of the other contacts.
        if (gameTouch.enabled() && e.getActionMasked() != MotionEvent.ACTION_MOVE)
            for (int i = 0; i < e.getPointerCount(); i++)
                if (gameTouch.owns(e.getPointerId(i)))
                    gameTouch.move(e.getPointerId(i), e.getX(i), e.getY(i));
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                TouchRouting.down(pad, gameTouch, controlsVisible,
                        e.getPointerId(index), e.getX(index), e.getY(index));
                break;
            case MotionEvent.ACTION_MOVE:
                for (int h = 0; gameTouch.enabled() && h < e.getHistorySize(); h++) {
                    for (int i = 0; i < e.getPointerCount(); i++)
                        movePointer(e.getPointerId(i), e.getHistoricalX(i, h), e.getHistoricalY(i, h));
                    gameTouch.publish(e.getHistoricalEventTime(h) * 1_000_000L);
                }
                for (int i = 0; i < e.getPointerCount(); i++) movePointer(e.getPointerId(i), e.getX(i), e.getY(i));
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                TouchRouting.up(pad, gameTouch, e.getPointerId(index));
                break;
            case MotionEvent.ACTION_CANCEL:
                android.util.Log.i("S2Arm", "touch MotionEvent ACTION_CANCEL");
                release();
                return true;
            default:
                return true;
        }
        gameTouch.publish(e.getEventTime() * 1_000_000L);
        int pressed = pad.buttons() & ~lastButtons;
        if (pressed != 0) performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        lastButtons = pad.buttons();
        listener.onTouchPad(pad);
        invalidate();
        return true;
    }

    private void openMenu() {
        // The four fingers may be on the game (passthrough): lift them there before the menu opens.
        release();
        listener.onTouchMenu();
    }

    private void watchMenuHold(MotionEvent e) {
        int action = e.getActionMasked();
        int down = e.getPointerCount() - (action == MotionEvent.ACTION_POINTER_UP || action == MotionEvent.ACTION_UP ? 1 : 0);
        if (action == MotionEvent.ACTION_POINTER_DOWN && down == 4) postDelayed(menuHold, MENU_HOLD_MS);
        else if (down != 4 || action == MotionEvent.ACTION_CANCEL) removeCallbacks(menuHold);
    }

    private boolean handleEditEvent(MotionEvent e) {
        int index = e.getActionIndex();
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                if (dragging != null) break;   // one control at a time
                dragging = pad.controlAt(e.getX(index), e.getY(index));
                if (dragging == null) break;
                dragPointer = e.getPointerId(index);
                dragX = dragging.x - e.getX(index); dragY = dragging.y - e.getY(index);
                editDrag.onEditDrag(true);
                performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
                break;
            case MotionEvent.ACTION_MOVE: {
                int i = dragging == null ? -1 : e.findPointerIndex(dragPointer);
                if (i >= 0) pad.moveControl(dragging, e.getX(i) + dragX, e.getY(i) + dragY);
                break;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                if (e.getPointerId(index) == dragPointer) { dragging = null; dragPointer = -1; editDrag.onEditDrag(false); }
                break;
            case MotionEvent.ACTION_CANCEL:
                dragging = null; dragPointer = -1;
                editDrag.onEditDrag(false);
                break;
            default:
                break;
        }
        invalidate();
        return true;
    }

    /** Lifted fingers we never saw (focus loss, pause): release everything. */
    void release() {
        removeCallbacks(menuHold);
        pad.cancel();
        gameTouch.cancel();
        lastButtons = 0;
        listener.onTouchPad(pad);
        invalidate();
    }

    @Override protected void onDraw(Canvas canvas) {
        if (!controlsVisible && !editing) return;
        if (editing) canvas.drawColor(0x66000000);   // the game dims; every control can be dragged
        ring.setColor(appearanceColor(0x99FFFFFF));
        text.setColor(appearanceColor(0xCCFFFFFF));
        for (TouchPad.Control c : pad.controls()) {
            fill.setColor(appearanceColor(editing ? (c == dragging ? 0x8800C8FF : 0x40FFFFFF) : c.pressed ? 0x66FFFFFF : 0x26FFFFFF));
            if (c.kind == TouchPad.BUTTON) {
                // F01 key frame; a round key (corner == half side) draws the same disc as before.
                canvas.drawRoundRect(c.x - c.halfWidth, c.y - c.halfHeight, c.x + c.halfWidth, c.y + c.halfHeight,
                        c.corner, c.corner, fill);
                canvas.drawRoundRect(c.x - c.halfWidth, c.y - c.halfHeight, c.x + c.halfWidth, c.y + c.halfHeight,
                        c.corner, c.corner, ring);
            } else {
                canvas.drawCircle(c.x, c.y, c.radius, fill);
                canvas.drawCircle(c.x, c.y, c.radius, ring);
            }
            if (c.kind == TouchPad.STICK) {
                fill.setColor(appearanceColor(c.pressed ? 0xAAFFFFFF : 0x55FFFFFF));
                canvas.drawCircle(c.x + c.knobX * c.radius, c.y + c.knobY * c.radius, c.radius * 0.45f, fill);
            } else if (c.kind == TouchPad.DPAD) {
                float a = c.radius * 0.55f;
                text.setTextSize(c.radius * 0.45f);
                canvas.drawText("▲", c.x, c.y - a + text.getTextSize() / 3, text);
                canvas.drawText("▼", c.x, c.y + a + text.getTextSize() / 3, text);
                canvas.drawText("◀", c.x - a, c.y + text.getTextSize() / 3, text);
                canvas.drawText("▶", c.x + a, c.y + text.getTextSize() / 3, text);
            } else {
                text.setTextSize(Math.min(c.halfWidth, c.halfHeight) * 0.8f);
                canvas.drawText(c.label, c.x, c.y + text.getTextSize() / 3, text);
            }
        }
    }
}
