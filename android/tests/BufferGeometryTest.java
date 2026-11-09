package org.s2arm.android;

/** B3：BufferGeometry.fit 的宿主回归——等比居中取整与 native publish_touch 同一套口径。 */
public final class BufferGeometryTest {
    private static void eq(int[] fit, int w, int h, String what) {
        if (fit[0] != w || fit[1] != h)
            throw new AssertionError(what + ": got " + fit[0] + "x" + fit[1] + " want " + w + "x" + h);
    }

    public static void main(String[] args) {
        // 16:9 缓冲进 2.2:1 窗口：占满高，两侧留边（Switch 掌机 1280x720 → 2712x1220 → 2169x1220）。
        eq(BufferGeometry.fit(1280, 720, 2712, 1220), 2169, 1220, "16:9 buffer in wide frame letterboxes horizontally");
        // 16:9 缓冲进 16:9 窗口：全占。
        eq(BufferGeometry.fit(1280, 720, 1920, 1080), 1920, 1080, "same aspect fills the frame");
        // 16:9 缓冲进窄窗口（竖屏/分屏）：占满宽，上下留边。
        eq(BufferGeometry.fit(1280, 720, 1220, 2712), 1220, 686, "narrow frame letterboxes vertically");
        // docked 1920x1080 缓冲同规则、同结果。
        eq(BufferGeometry.fit(1920, 1080, 2712, 1220), 2169, 1220, "docked 1080p buffer letterboxes the same");
        // 非 16:9（宽屏裁切 21:9 类）：按自己的比例等比，不为 P7 写死任何比例。
        eq(BufferGeometry.fit(1280, 540, 2712, 1220), 2712, 1144, "21:9-ish buffer keeps its own aspect");
        // 非法输入按 frame 尺寸回落，不出 0/负。
        eq(BufferGeometry.fit(0, 0, 100, 50), 100, 50, "zero buffer falls back to the frame");
        eq(BufferGeometry.fit(160, 90, 0, 0), 0, 0, "unmeasured frame clamps to zero, not negative");
        // 取整：四舍五入而不是截断（与 nvn_vk_swapchain.c 的 fit 一致：89x50 into 100x50）。
        eq(BufferGeometry.fit(160, 90, 100, 50), 89, 50, "rounding matches the native fit");
        System.out.println("BufferGeometryTest: ok");
    }
}
