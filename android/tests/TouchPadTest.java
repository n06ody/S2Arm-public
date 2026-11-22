package org.s2arm.android;

import java.util.ArrayList;
import java.util.List;

/** Plain Java checks for the on-screen pad (no Android runtime). */
public final class TouchPadTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        if (!ok) { System.err.println("FAILED: " + what); failures++; }
    }

    private static TouchPad.Control find(TouchPad pad, int kind, String label, int bit) {
        for (TouchPad.Control c : pad.controls())
            if (c.kind == kind && (kind == TouchPad.BUTTON ? c.label.equals(label) : c.bit == bit)) return c;
        throw new AssertionError(label);
    }

    /** The pre-F01 TouchPad table, kept here as the reference the built-in layout must reproduce. */
    private static float[] historical(TouchPad.Control c, float left, float top, float width, float height) {
        float bw = width, bh = height, by = top;
        if (height > width) { bh = width * 0.56f; by = top + height - bh; }
        float u = Math.min(bw, bh) / 10f, rx, ry, r;
        switch (c.kind == TouchPad.BUTTON ? c.label : c.kind == TouchPad.DPAD ? "DPAD" : c.bit == 0 ? "LSTICK" : "RSTICK") {
            case "LSTICK": rx = 0.13f; ry = 0.56f; r = 1.45f; break;
            case "RSTICK": rx = 0.73f; ry = 0.80f; r = 1.20f; break;
            case "DPAD":   rx = 0.27f; ry = 0.82f; r = 1.20f; break;
            case "A":      rx = 0.93f; ry = 0.56f; r = 0.55f; break;
            case "B":      rx = 0.87f; ry = 0.56f + 1.15f * u / bh; r = 0.55f; break;
            case "X":      rx = 0.87f; ry = 0.56f - 1.15f * u / bh; r = 0.55f; break;
            case "Y":      rx = 0.81f; ry = 0.56f; r = 0.55f; break;
            case "LS":     rx = 0.13f + 1.9f * u / bw; ry = 0.56f - 1.3f * u / bh; r = 0.38f; break;
            case "RS":     rx = 0.73f - 1.7f * u / bw; ry = 0.80f - 1.1f * u / bh; r = 0.38f; break;
            case "L":      rx = 0.17f; ry = 0.15f; r = 0.50f; break;
            case "ZL":     rx = 0.07f; ry = 0.15f; r = 0.50f; break;
            case "R":      rx = 0.83f; ry = 0.15f; r = 0.50f; break;
            case "ZR":     rx = 0.93f; ry = 0.15f; r = 0.50f; break;
            case "+":      rx = 0.58f; ry = 0.12f; r = 0.40f; break;
            default:       rx = 0.42f; ry = 0.12f; r = 0.40f; break;   // minus
        }
        return new float[] {left + rx * bw, by + ry * bh, r * u};
    }

    private static void layoutTrace() {
        TouchPad late = new TouchPad();
        List<String> lines = new ArrayList<>();
        late.layout(0, 0, 2400, 1080);
        check(lines.isEmpty(), "trace off is silent");
        late.setLayoutTrace((layout, input) -> lines.add(layout));
        check(lines.size() == 1, "layout before enable: one immediate snapshot");
        if (!lines.isEmpty()) {
            String line = lines.get(0);
            check(line.startsWith("touchpad layout: LSTICK=(312,605,r157) RSTICK=(1752,864,r130) DPAD=")
                    && line.split("=\\(").length - 1 == 15 && line.endsWith(" generation=1"),
                    "complete snapshot preserves the layout log format and includes its generation");
        }
        late.setLayoutTrace((layout, input) -> lines.add(layout));
        late.down(1, 2232, 605);
        check(late.buttons() == 1, "trace leaves input unchanged");
        late.up(1);
        check(lines.size() == 1, "repeat enable does not print a snapshot");

        late.setLayoutTrace(null);
        late.layout(0, 0, 1600, 1200);
        check(lines.size() == 1, "relayout while trace is off is silent");
        late.setLayoutTrace((layout, input) -> lines.add(layout));
        check(lines.size() == 2 && lines.get(1).contains("RSTICK=(1168,960,r144)")
                && lines.get(1).endsWith(" generation=2"), "re-enable: one current snapshot");
        late.setLayoutTrace(null);
        late.setLayoutTrace((layout, input) -> lines.add(layout));
        check(lines.size() == 3 && lines.get(2).equals(lines.get(1)), "reopen trace requests the current generation");

        TouchPad early = new TouchPad();
        lines.clear();
        early.setLayoutTrace((layout, input) -> lines.add(layout));
        check(lines.isEmpty(), "enable before layout: no empty snapshot");
        early.layout(0, 0, 2400, 1080);
        check(lines.size() == 1 && lines.get(0).endsWith(" generation=1"),
                "enable before layout: first generation is published once");
        early.layout(0, 0, 1600, 1200);
        check(lines.size() == 2 && lines.get(1).contains("RSTICK=(1168,960,r144)")
                && lines.get(1).endsWith(" generation=2"), "relayout: one snapshot with current geometry");
    }

    private static void startupLayoutLogLoss() {
        TouchPad pad = new TouchPad();
        List<String> lines = new ArrayList<>();
        List<String> inputs = new ArrayList<>();
        pad.setLayoutTrace((layout, input) -> {
            lines.add(layout);
            if (input != null) inputs.add(input);
        });
        String active = "input: buttons=0x0001 L=(0,0) R=(0,0)";
        pad.traceInput(active);
        check(lines.isEmpty(), "input before layout does not emit an empty snapshot");
        pad.layout(0, 0, 2400, 1080);
        check(lines.size() == 1, "startup snapshot logged");
        String startup = lines.isEmpty() ? "" : lines.get(0);

        // The collector misses startup logs but starts receiving before the user moves a stick.
        lines.clear();
        pad.traceInput(null);
        check(lines.isEmpty(), "neutral input does not replay or consume the snapshot");
        pad.traceInput(active);
        check(lines.equals(List.of(startup)), "surviving first active input carries the full startup snapshot");
        check(inputs.equals(List.of(active)), "first input text and its layout are passed together");
        pad.traceInput(active);
        pad.traceInput(null);
        check(lines.size() == 1, "later inputs do not replay the same layout");

        pad.layout(0, 0, 1600, 1200);
        pad.traceInput(active);
        check(lines.size() == 3 && lines.get(1).equals(lines.get(2))
                && lines.get(2).endsWith(" generation=2"), "first active input also replays the latest generation");
        pad.layout(0, 0, 2400, 1080);
        int beforeDisable = lines.size();
        pad.setLayoutTrace(null);
        pad.traceInput(active);
        check(lines.size() == beforeDisable, "disabling trace cancels the input replay");
        pad.setLayoutTrace((layout, input) -> lines.add(layout));
        pad.traceInput(active);
        check(lines.size() == beforeDisable + 2
                && lines.get(lines.size() - 1).endsWith(" generation=3"), "reopening trace arms the current layout");
    }

    public static void main(String[] args) throws Exception {
        layoutTrace();
        startupLayoutLogLoss();
        TouchPad pad = new TouchPad();
        pad.layout(0, 0, 2400, 1080);
        TouchPad.Control a = find(pad, TouchPad.BUTTON, "A", 0), b = find(pad, TouchPad.BUTTON, "B", 0);
        TouchPad.Control dpad = find(pad, TouchPad.DPAD, "", 0), left = find(pad, TouchPad.STICK, "", 0);

        // Every control inside the view, none overlapping another.
        for (TouchPad.Control c : pad.controls()) {
            check(c.x - c.radius >= 0 && c.x + c.radius <= 2400 && c.y - c.radius >= 0 && c.y + c.radius <= 1080, "inside " + c.label);
            for (TouchPad.Control o : pad.controls())
                if (o != c) check(Math.hypot(c.x - o.x, c.y - o.y) >= c.radius + o.radius, "no overlap " + c.label + "/" + o.label);
        }

        // A press, slide to B (a finger moves between face buttons), release.
        pad.down(1, a.x, a.y);
        check(pad.buttons() == 1, "A pressed");
        pad.move(1, b.x, b.y);
        check(pad.buttons() == 2, "slid to B");
        pad.up(1);
        check(pad.buttons() == 0, "released");

        // D-pad: centre is neutral, up-right presses two directions.
        pad.down(2, dpad.x, dpad.y);
        check(pad.buttons() == 0, "dpad dead zone");
        pad.move(2, dpad.x + dpad.radius * 0.6f, dpad.y - dpad.radius * 0.6f);
        check(pad.buttons() == ((1 << 13) | (1 << 14)), "dpad up-right");
        pad.move(2, dpad.x - dpad.radius * 0.8f, dpad.y);
        check(pad.buttons() == (1 << 12), "dpad left");
        pad.up(2);

        // Left stick: full right, full up (positive), clamped, dead zone; captured while it crosses buttons.
        pad.down(3, left.x, left.y);
        check(pad.leftX() == 0 && pad.leftY() == 0, "stick dead zone");
        pad.move(3, left.x + left.radius, left.y);
        check(pad.leftX() == 32767 && pad.leftY() == 0, "stick right");
        pad.move(3, left.x, left.y - 3 * left.radius);
        check(pad.leftX() == 0 && pad.leftY() == 32767, "stick up, clamped");
        pad.move(3, left.x + left.radius * 0.05f, left.y);
        check(pad.leftX() == 0, "stick small move stays in dead zone");

        // Second finger on A while the stick is held; the stick finger over A does not press it.
        pad.down(4, a.x, a.y);
        check(pad.buttons() == 1 && pad.leftX() == 0, "stick + A");
        pad.move(3, a.x, a.y);
        check(pad.leftX() == 32767 && pad.buttons() == 1, "stick finger keeps the stick");
        pad.cancel();
        check(pad.buttons() == 0 && pad.leftX() == 0 && !pad.active(), "cancel releases everything");

        // Portrait and insets: everything stays inside the area given.
        pad.layout(40, 100, 1080, 2276);
        for (TouchPad.Control c : pad.controls())
            check(c.x - c.radius >= 40 && c.x + c.radius <= 1120 && c.y - c.radius >= 100 && c.y + c.radius <= 2376, "portrait inside " + c.label);

        // The built-in layout now comes from F01 (TouchLayout.androidDefault): same discs as the old table.
        float[][] boxes = {{0, 0, 2400, 1080}, {0, 0, 1600, 1200}, {40, 100, 1080, 2276}, {0, 0, 2000, 1800}};
        for (float[] area : boxes) {
            pad.layout(area[0], area[1], area[2], area[3]);
            for (TouchPad.Control c : pad.controls()) {
                float[] old = historical(c, area[0], area[1], area[2], area[3]);
                check(Math.abs(c.x - old[0]) < 0.01f && Math.abs(c.y - old[1]) < 0.01f
                        && Math.abs(c.radius - old[2]) < 0.01f, "historical geometry " + c.id + " in " + area[2] + "x" + area[3]);
                if (c.kind == TouchPad.BUTTON)
                    check(c.halfWidth == c.halfHeight && Math.abs(c.corner - c.halfWidth) < 0.01f, "round key " + c.id);
            }
        }
        check(pad.controls().size() == 15, "two sticks, one D-pad, twelve keys");

        // A custom F01 layout (the iOS reference: rectangular shoulder keys, half-screen stick regions).
        TouchLayout reference = TouchLayout.read(java.nio.file.Files.readAllBytes(java.nio.file.Path.of(args[0])));
        pad.setLayout(reference);
        pad.layout(0, 0, 2400, 1080);
        check(pad.layoutModel() == reference && pad.controls().size() == 13, "custom layout without LS/RS");
        TouchPad.Control zl = find(pad, TouchPad.BUTTON, "ZL", 0);
        TouchLayout.Rect zf = reference.frameForButton("ZL", 0, 0, 2400, 1080);
        check(Math.abs(zl.halfWidth * 2 - zf.width) < 0.01f && Math.abs(zl.halfHeight * 2 - zf.height) < 0.01f, "rect key frame");
        pad.down(5, zl.x + zl.halfWidth - 1, zl.y);
        check(pad.buttons() == (1 << 8), "rect key end inside the frame presses ZL");
        pad.up(5);
        pad.down(5, zl.x, zl.y + zl.halfHeight * 2.5f);
        check((pad.buttons() & (1 << 8)) == 0, "below the rect key misses ZL");
        pad.up(5);
        TouchPad.Control ls = find(pad, TouchPad.STICK, "", 0);
        double[] lc = reference.stickCentre("left", 0, 0, 2400, 1080);
        check(Math.abs(ls.x - lc[0]) < 0.01f && Math.abs(ls.y - lc[1]) < 0.01f, "fixed stick at activation centre");
        TouchPad.Control d = find(pad, TouchPad.DPAD, "", 0);
        pad.down(6, d.x, d.y - d.radius * 0.8f);
        check(pad.buttons() == (1 << 13), "custom D-pad up");
        pad.up(6);
        pad.setLayout(null);
        pad.layout(0, 0, 2400, 1080);
        check(pad.layoutModel().identifier.equals("default") && pad.controls().size() == 15, "back to built-in");

        // Editor: drag a key, the D-pad and a stick; save; the saved layout lays out where they were dropped.
        pad.setLayout(null);
        pad.layout(0, 0, 2400, 1080);
        check(!pad.edited(), "fresh layout is not edited");
        TouchPad.Control ea = find(pad, TouchPad.BUTTON, "A", 0), ed = find(pad, TouchPad.DPAD, "", 0);
        TouchPad.Control er = find(pad, TouchPad.STICK, "", 1);
        check(pad.controlAt(ea.x, ea.y) == ea && pad.controlAt(er.x, er.y) == er, "editor pick");
        check(pad.controlAt(1200, 700) == null, "editor pick misses empty space");
        float dx = ed.x, dy = ed.y, rx0 = er.x, ry0 = er.y;
        pad.moveControl(ea, 1500, 400);
        pad.moveControl(ed, dx + 100, dy - 80);
        pad.moveControl(er, rx0 - 200, ry0 - 150);
        pad.moveControl(find(pad, TouchPad.BUTTON, "ZR", 0), 9999, -50);   // clamped into the box
        check(pad.edited() && pad.buttons() == 0, "drags change no input");
        TouchLayout mine = pad.editedLayout(TouchLayoutStore.MINE, "My layout");
        check(mine.identifier.equals("mine"), "saved as the player's layout");
        pad.setLayout(mine);
        pad.layout(0, 0, 2400, 1080);
        TouchPad.Control na = find(pad, TouchPad.BUTTON, "A", 0), nd = find(pad, TouchPad.DPAD, "", 0);
        TouchPad.Control nr = find(pad, TouchPad.STICK, "", 1), nz = find(pad, TouchPad.BUTTON, "ZR", 0);
        check(Math.abs(na.x - 1500) < 0.01f && Math.abs(na.y - 400) < 0.01f, "key lands where dropped");
        check(Math.abs(nd.x - (dx + 100)) < 0.01f && Math.abs(nd.y - (dy - 80)) < 0.01f
                && Math.abs(nd.radius - ed.radius) < 0.01f, "D-pad moves as one");
        check(Math.abs(nr.x - (rx0 - 200)) < 0.01f && Math.abs(nr.y - (ry0 - 150)) < 0.01f, "stick lands where dropped");
        check(nz.x + nz.halfWidth <= 2400.01f && nz.y - nz.halfHeight >= -0.01f, "clamped key drawn inside");
        TouchPad fresh = new TouchPad();
        fresh.layout(0, 0, 2400, 1080);
        TouchPad.Control nb = find(pad, TouchPad.BUTTON, "B", 0), fb = find(fresh, TouchPad.BUTTON, "B", 0);
        check(Math.abs(nb.x - fb.x) < 0.01f && Math.abs(nb.y - fb.y) < 0.01f, "undragged key kept");
        pad.down(7, na.x, na.y);
        check(pad.buttons() == 1, "moved A presses A");
        pad.up(7);
        // Dragging the right stick onto the left one is refused (F01 regions may not overlap), model untouched.
        TouchPad.Control nl = find(pad, TouchPad.STICK, "", 0);
        pad.moveControl(nr, nl.x, nl.y);
        boolean refused = false;
        try { pad.editedLayout(TouchLayoutStore.MINE, "x"); } catch (IllegalArgumentException expected) { refused = true; }
        check(refused && pad.layoutModel() == mine, "overlapping sticks refused");

        // Store: per game file, round trip, damaged file reported, reset back to built-in.
        java.nio.file.Path dir = java.nio.file.Files.createTempDirectory("s2a-touch-store");
        TouchLayoutStore store = new TouchLayoutStore(dir.toFile(), "0100abcdef000000");
        check(store.file().getName().equals("0100ABCDEF000000.json"), "per-game file name");
        check(new TouchLayoutStore(dir.toFile(), "../x").file().getName().equals("app.json"), "no path from title");
        check(store.load() == null, "nothing saved");
        store.save(mine);
        TouchLayout loaded = store.load();
        check(loaded != null && java.util.Arrays.equals(loaded.jsonBytes(), mine.jsonBytes()), "store round trip");
        check(!new java.io.File(dir.toFile(), "0100ABCDEF000000.json.tmp").exists(), "no temp file left");
        java.nio.file.Files.write(store.file().toPath(), "{".getBytes(java.nio.charset.StandardCharsets.UTF_8));
        boolean damaged = false;
        try { store.load(); } catch (IllegalArgumentException expected) { damaged = true; }
        check(damaged, "damaged file reported");
        store.reset();
        check(store.load() == null && !store.file().exists(), "reset");
        store.reset();   // idempotent

        // Transfer expectations come from F01 and the literal iOS reference, not a second serializer.
        byte[] classic = java.nio.file.Files.readAllBytes(java.nio.file.Path.of(args[0]));
        TouchLayout exported = TouchLayout.read(store.exportBytes(TouchLayout.androidDefault(2400, 1080)));
        check(exported.identifier.matches("layout-[a-f0-9-]{36}"), "built-in export gets user identity");
        check(exported.button("A").x == 0.93 && exported.buttons.size() == 16, "built-in export geometry");
        check(!store.file().exists(), "export does not save a layout");
        store.importLayout(classic);
        check(store.load().identifier.equals("classic-reference") && store.load().button("A").x == 0.944,
                "iOS reference imports with original identity and geometry");
        check(TouchLayoutStore.exportFilename(classic).equals("classic-reference.json"), "export filename is id.json");
        byte[] transfer = store.exportBytes(TouchLayout.androidDefault(2400, 1080));
        check(TouchLayout.read(transfer).identifier.equals("classic-reference"), "saved export keeps identity");
        TouchLayoutStore other = new TouchLayoutStore(dir.toFile(), "0000000000000001");
        other.importLayout(transfer);
        check(other.load().button("A").x == 0.944 && other.load().buttons.size() == 14, "export import round trip");
        store.importLayout(transfer); // Android's single slot replaces even the same id.
        byte[] before = java.nio.file.Files.readAllBytes(store.file().toPath());
        String text = new String(classic, java.nio.charset.StandardCharsets.UTF_8);
        byte[] boundary = java.util.Arrays.copyOf(classic, 65536);
        java.util.Arrays.fill(boundary, classic.length, boundary.length, (byte) ' ');
        other.importLayout(boundary);
        check(other.load().identifier.equals("classic-reference"), "exactly 64 KiB accepted");
        check(java.util.Arrays.equals(boundary, TouchLayoutStore.readImport(new java.io.ByteArrayInputStream(boundary))),
                "stream accepts exact 64 KiB without truncation");
        java.io.ByteArrayInputStream huge = new java.io.ByteArrayInputStream(new byte[100000]);
        boolean capped = false;
        try { TouchLayoutStore.readImport(huge); } catch (IllegalArgumentException expected) { capped = true; }
        check(capped && huge.available() == 100000 - 65537, "stream stops after cap plus sentinel");
        byte[] badUtf8 = classic.clone();
        for (int i = 0; i < badUtf8.length; i++) {
            if (badUtf8[i] < 0) { badUtf8[i] = (byte) 0xff; break; }
        } // Malformed bytes inside a valid JSON string: replacement decoding would wrongly accept it.
        byte[][] rejected = {
            java.util.Arrays.copyOf(boundary, 65537),
            "{".getBytes(java.nio.charset.StandardCharsets.UTF_8),
            badUtf8,
            text.replace("0.944", "1.944").getBytes(java.nio.charset.StandardCharsets.UTF_8),
            text.replace("\"schemaVersion\": 1", "\"schemaVersion\": true").getBytes(java.nio.charset.StandardCharsets.UTF_8),
            (text + "false").getBytes(java.nio.charset.StandardCharsets.UTF_8)
        };
        for (int i = 0; i < rejected.length; i++) {
            boolean failed = false;
            try { store.importLayout(rejected[i]); } catch (IllegalArgumentException expected) {
                failed = true;
                if (i == 0) check(expected.getMessage().contains("64 KiB"), "oversize reason");
                if (i == 2) check(expected.getMessage().contains("UTF-8"), "UTF-8 reason");
            }
            check(failed, "invalid transfer rejected " + i);
            check(java.util.Arrays.equals(before, java.nio.file.Files.readAllBytes(store.file().toPath())),
                    "failed import preserves bytes " + i);
        }
        // 内置保留名不挡：换新名字照常导入，按键位置与原文件一致（玩家拿到的是同一份布局）
        for (String reserved : new String[] {"default", "touch"}) {
            TouchLayoutStore renamed = new TouchLayoutStore(dir.toFile(), "00000000000000" + (reserved.equals("default") ? "0A" : "0B"));
            TouchLayout got = renamed.importLayout(text.replace("classic-reference", reserved).getBytes(java.nio.charset.StandardCharsets.UTF_8));
            check(got.identifier.startsWith("layout-") && renamed.load() != null
                    && renamed.load().identifier.equals(got.identifier), "reserved identity " + reserved + " imported under a new name");
            check(java.util.Arrays.equals(got.copyAs("classic-reference", got.name).jsonBytes(),
                    TouchLayout.read(classic).copyAs("classic-reference", got.name).jsonBytes()), "renamed import keeps the layout " + reserved);
        }
        java.io.File blockedTemp = new java.io.File(dir.toFile(), store.file().getName() + ".tmp");
        check(blockedTemp.mkdir(), "simulate unavailable temporary output");
        boolean writeFailed = false;
        try { store.importLayout(exported.jsonBytes()); } catch (java.io.IOException expected) { writeFailed = true; }
        check(writeFailed && java.util.Arrays.equals(before, java.nio.file.Files.readAllBytes(store.file().toPath())),
                "I/O failure preserves previous file");
        check(blockedTemp.delete(), "remove simulated output failure");
        store.importLayout(exported.jsonBytes());
        check(store.load().button("A").x == 0.93, "valid transfer replaces previous geometry (negative control)");
        check(TouchLayout.read(store.exportBytes(reference)).button("A").x == 0.93,
                "export chooses saved file over supplied default");
        if (failures > 0) System.exit(1);
        System.out.println("TouchPad: ok");
    }
}
