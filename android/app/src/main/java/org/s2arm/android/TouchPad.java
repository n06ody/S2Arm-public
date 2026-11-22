package org.s2arm.android;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.function.Consumer;

/**
 * On-screen gamepad state: layout and multi-touch tracking, no Android types (unit-tested).
 * Same Npad bits and signed stick range as GamepadState. A pointer that lands on a stick
 * keeps it until lifted; other pointers press whatever button or D-pad sector they are over,
 * so a finger can slide between face buttons as on a physical pad.
 */
final class TouchPad {
    static final int BUTTON = 0, DPAD = 1, STICK = 2;
    static final float STICK_DEADZONE = 0.12f;
    static final float DPAD_DEADZONE = 0.25f;

    static final class Control {
        final int kind, bit;   // bit: Npad button (BUTTON); unused for DPAD; 0 = left, 1 = right (STICK)
        final String id, label; // id: F01 button id, "DPAD", or F01 stick id
        float x, y, radius;    // view pixels; radius = half the longer side of a BUTTON
        float homeX, homeY;    // centre from the layout (an editor drag moves x, y only)
        float halfWidth, halfHeight, corner;   // BUTTON: F01 frame (a round key has corner == half side)
        boolean pressed;       // drawing only
        float knobX, knobY;    // STICK: deflection in [-1, 1], y down (drawing only)
        Control(int kind, int bit, String id, String label) {
            this.kind = kind; this.bit = bit; this.id = id; this.label = label;
        }
        /** Within the drawn shape grown by its slack: a disc for sticks and the D-pad, the rounded frame for keys. */
        boolean hit(float px, float py) {
            if (kind != BUTTON) {
                float slack = kind == STICK ? 1.25f : 1.2f;
                float dx = px - x, dy = py - y, r = radius * slack;
                return dx * dx + dy * dy <= r * r;
            }
            // Signed distance to the rounded rectangle; equals |p - c| - r for a round key.
            float qx = Math.abs(px - x) - (halfWidth - corner), qy = Math.abs(py - y) - (halfHeight - corner);
            float outside = (float) Math.hypot(Math.max(qx, 0), Math.max(qy, 0));
            float distance = outside + Math.min(Math.max(qx, qy), 0) - corner;
            return distance <= 0.15f * Math.min(halfWidth, halfHeight);
        }
    }

    private static final String[] DIRECTIONS = {"UP", "DOWN", "LEFT", "RIGHT"};

    private static final String[][] KEYS = {   // F01 id, label, Npad bit
        {"A", "A", "0"}, {"B", "B", "1"}, {"X", "X", "2"}, {"Y", "Y", "3"}, {"LS", "LS", "4"}, {"RS", "RS", "5"},
        {"L", "L", "6"}, {"R", "R", "7"}, {"ZL", "ZL", "8"}, {"ZR", "ZR", "9"}, {"PLUS", "+", "10"}, {"MINUS", "−", "11"}};

    private final List<Control> controls = new ArrayList<>();
    private final Map<Integer, float[]> pointers = new HashMap<>();   // id -> {x, y}
    private final Map<Integer, Control> sticks = new HashMap<>();     // id -> captured stick
    private int buttons;
    private final int[] axes = new int[4];   // lx, ly, rx, ry
    private TouchLayout custom;              // null: Android's built-in layout for the current box
    private TouchLayout current;
    private int layoutGeneration;
    private Consumer<String> layoutTrace;
    private boolean layoutTracePendingInput;
    private final float[] box = new float[4];

    List<Control> controls() { return controls; }

    boolean hit(float x, float y) {
        for (Control c : controls) if (c.hit(x, y)) return true;
        return false;
    }

    /** Use an F01 layout (null = built-in). Takes effect at the next layout(), which drops held contacts. */
    void setLayout(TouchLayout layout) { custom = layout; }

    /** The F01 layout of the last layout(), and the pad box it was projected into (left, top, width, height). */
    TouchLayout layoutModel() { return current; }
    float[] box() { return box.clone(); }

    /** Publish the current layout on enable, then publish each completed generation. */
    void setLayoutTrace(Consumer<String> trace) {
        boolean changed = (layoutTrace == null) != (trace == null);
        layoutTrace = trace;
        if (changed) traceLayout();
    }

    private void traceLayout() {
        layoutTracePendingInput = layoutTrace != null && current != null;
        if (layoutTracePendingInput) logLayout();
    }

    /** The collector may miss startup logs. Replay the current layout once on live input. */
    void traceInput(boolean active) {
        if (!active || !layoutTracePendingInput) return;
        layoutTracePendingInput = false;
        logLayout();
    }

    private void logLayout() {
        StringBuilder line = new StringBuilder("touchpad layout:");
        for (Control c : controls) {
            String name = c.label;
            if (c.kind == STICK) name = c.bit == 0 ? "LSTICK" : "RSTICK";
            if (c.kind == DPAD) name = "DPAD";
            line.append(String.format(Locale.ROOT, " %s=(%.0f,%.0f,r%.0f)", name, c.x, c.y, c.radius));
        }
        layoutTrace.accept(line.append(" generation=").append(layoutGeneration).toString());
    }

    /**
     * Lay the controls out in the area left, top, width x height (view pixels, inside the system
     * bars). Landscape uses the whole area; portrait puts the pad in a landscape-shaped box at
     * the bottom. Geometry is the F01 layout projected into that box; the four direction keys
     * become one D-pad disc and sticks sit fixed at their activation centres.
     */
    void layout(float left, float top, float width, float height) {
        float bw = width, bh = height, by = top;
        if (height > width) { bh = width * 0.56f; by = top + height - bh; }
        box[0] = left; box[1] = by; box[2] = bw; box[3] = bh;
        current = custom != null ? custom : TouchLayout.androidDefault(bw, bh);
        double unit = Math.min(bw, bh);
        pointers.clear();
        sticks.clear();
        controls.clear();
        for (TouchLayout.Stick s : current.sticks) {
            boolean leftStick = "left".equals(s.id);
            Control c = new Control(STICK, leftStick ? 0 : 1, s.id, leftStick ? "L" : "R");
            double[] p = current.stickCentre(s.id, left, by, bw, bh);
            c.x = (float) p[0]; c.y = (float) p[1]; c.radius = (float) (s.radius * unit);
            add(c);
        }
        boolean dpad = false;
        for (TouchLayout.Button b : current.buttons) {
            if (b.id.equals("UP") || b.id.equals("DOWN") || b.id.equals("LEFT") || b.id.equals("RIGHT")) {
                if (dpad) continue;
                dpad = true;
                double[] d = current.dpad(left, by, bw, bh);
                Control c = new Control(DPAD, 0, "DPAD", "");
                c.x = (float) d[0]; c.y = (float) d[1]; c.radius = (float) d[2];
                add(c);
                continue;
            }
            String[] key = null;
            for (String[] k : KEYS) if (k[0].equals(b.id)) key = k;
            TouchLayout.Rect f = current.frameForButton(b.id, left, by, bw, bh);
            Control c = new Control(BUTTON, Integer.parseInt(key[2]), b.id, key[1]);
            c.halfWidth = (float) (f.width / 2); c.halfHeight = (float) (f.height / 2);
            c.x = (float) (f.x + f.width / 2); c.y = (float) (f.y + f.height / 2);
            c.corner = (float) Math.min(b.cornerRadius * unit, Math.min(f.width, f.height) / 2);
            c.radius = Math.max(c.halfWidth, c.halfHeight);
            add(c);
        }
        update();
        layoutGeneration++;
        traceLayout();
    }

    private void add(Control c) {
        c.homeX = c.x; c.homeY = c.y;
        controls.add(c);
    }

    /** Editor pick: keys and the D-pad first (they are small and may sit inside a stick's reach), then sticks. */
    Control controlAt(float x, float y) {
        for (Control c : controls) if (c.kind != STICK && c.hit(x, y)) return c;
        for (Control c : controls) if (c.kind == STICK && c.hit(x, y)) return c;
        return null;
    }

    /** Editor drag: keep every direction frame in bounds when translating the D-pad as a group. */
    void moveControl(Control c, float x, float y) {
        if (c.kind == DPAD) {
            double minDx = -box[2], maxDx = box[2], minDy = -box[3], maxDy = box[3];
            for (String key : DIRECTIONS) {
                TouchLayout.Rect frame = current.frameForButton(key, box[0], box[1], box[2], box[3]);
                minDx = Math.max(minDx, box[0] - frame.x);
                maxDx = Math.min(maxDx, (double) box[0] + box[2] - frame.x - frame.width);
                minDy = Math.max(minDy, box[1] - frame.y);
                maxDy = Math.min(maxDy, (double) box[1] + box[3] - frame.y - frame.height);
            }
            c.x = (float) (c.homeX + Math.max(minDx, Math.min(maxDx, (double) x - c.homeX)));
            c.y = (float) (c.homeY + Math.max(minDy, Math.min(maxDy, (double) y - c.homeY)));
        } else {
            c.x = Math.max(box[0], Math.min(box[0] + box[2], x));
            c.y = Math.max(box[1], Math.min(box[1] + box[3], y));
        }
    }

    boolean edited() {
        for (Control c : controls) if (c.x != c.homeX || c.y != c.homeY) return true;
        return false;
    }

    /**
     * Editor save: the current layout with every dragged control at its new place, as a new user
     * layout (TouchLayout.withButtonCenters). A dragged D-pad moves its four keys together; a dragged
     * stick moves its activation region with it, kept inside the box. Throws IllegalArgumentException
     * when the result is not a valid F01 layout (e.g. stick regions dragged onto each other).
     */
    TouchLayout editedLayout(String id, String name) {
        double left = box[0], top = box[1], width = box[2], height = box[3];
        Map<String, double[]> centres = new HashMap<>();
        for (Control c : controls) {
            if (c.x == c.homeX && c.y == c.homeY) continue;
            if (c.kind == BUTTON) centres.put(c.id, new double[] {c.x, c.y});
            else if (c.kind == DPAD)
                for (String key : DIRECTIONS) {
                    TouchLayout.Rect f = current.frameForButton(key, left, top, width, height);
                    centres.put(key, new double[] {f.x + f.width / 2 + (c.x - c.homeX), f.y + f.height / 2 + (c.y - c.homeY)});
                }
        }
        TouchLayout out = current.withButtonCenters(centres, left, top, width, height, id, name);
        for (Control c : controls) {
            if (c.kind != STICK || (c.x == c.homeX && c.y == c.homeY)) continue;
            TouchLayout.Stick s = out.stick(c.id);
            TouchLayout.Rect a = s.activation;
            double x = Math.max(0, Math.min(1 - a.width, a.x + (c.x - c.homeX) / width));
            double y = Math.max(0, Math.min(1 - a.height, a.y + (c.y - c.homeY) / height));
            out = out.withStick(c.id, x, y, a.width, a.height, s.radius);
        }
        return out;
    }

    void down(int id, float x, float y) {
        pointers.put(id, new float[] {x, y});
        for (Control c : controls)
            if (c.kind == STICK && c.hit(x, y) && !sticks.containsValue(c)) { sticks.put(id, c); break; }
        update();
    }

    void move(int id, float x, float y) {
        float[] p = pointers.get(id);
        if (p == null) return;
        p[0] = x; p[1] = y;
        update();
    }

    void up(int id) {
        pointers.remove(id);
        sticks.remove(id);
        update();
    }

    void cancel() {
        pointers.clear();
        sticks.clear();
        update();
    }

    boolean active() { return !pointers.isEmpty(); }
    int buttons() { return buttons; }
    int leftX() { return axes[0]; }
    int leftY() { return axes[1]; }
    int rightX() { return axes[2]; }
    int rightY() { return axes[3]; }

    private static int axis(float v) { return Math.round(Math.max(-1f, Math.min(1f, v)) * 32767f); }

    private static int dpadBits(Control d, float x, float y) {
        float dx = (x - d.x) / d.radius, dy = (y - d.y) / d.radius;
        if (dx * dx + dy * dy < DPAD_DEADZONE * DPAD_DEADZONE) return 0;
        double angle = Math.toDegrees(Math.atan2(-dy, dx));   // 0 = right, 90 = up
        int sector = (int) Math.floor(((angle + 360 + 22.5) % 360) / 45);   // 0 = right, counter-clockwise
        final int left = 1 << 12, up = 1 << 13, right = 1 << 14, down = 1 << 15;
        switch (sector) {
            case 0: return right;
            case 1: return right | up;
            case 2: return up;
            case 3: return up | left;
            case 4: return left;
            case 5: return left | down;
            case 6: return down;
            default: return down | right;
        }
    }

    private void update() {
        buttons = 0;
        java.util.Arrays.fill(axes, 0);
        for (Control c : controls) { c.pressed = false; c.knobX = c.knobY = 0; }
        for (Map.Entry<Integer, float[]> e : pointers.entrySet()) {
            float x = e.getValue()[0], y = e.getValue()[1];
            Control stick = sticks.get(e.getKey());
            if (stick != null) {
                float dx = (x - stick.x) / stick.radius, dy = (y - stick.y) / stick.radius;
                float len = (float) Math.sqrt(dx * dx + dy * dy);
                if (len > 1) { dx /= len; dy /= len; len = 1; }
                stick.pressed = true; stick.knobX = dx; stick.knobY = dy;
                if (len > STICK_DEADZONE) {
                    axes[2 * stick.bit] = axis(dx);
                    axes[2 * stick.bit + 1] = axis(-dy);   // up is positive
                }
                continue;
            }
            for (Control c : controls) {
                if (c.kind == BUTTON && c.hit(x, y)) { buttons |= 1 << c.bit; c.pressed = true; break; }
                if (c.kind == DPAD && c.hit(x, y)) {
                    int bits = dpadBits(c, x, y);
                    buttons |= bits; c.pressed = bits != 0;
                    c.knobX = (x - c.x) / c.radius; c.knobY = (y - c.y) / c.radius;
                    break;
                }
            }
        }
    }
}
