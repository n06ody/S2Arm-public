package org.s2arm.android;

/**
 * B3（debug.s2a.buffer_geometry）：交换链缓冲按游戏输出尺寸出图后，缓冲被合成器
 * 按 SCALE_TO_WINDOW 拉伸到 SurfaceView 的 frame——没有原生 SCALE_FIT，
 * 宽高比只能靠 frame 本身保持：把 view 布成缓冲等比的最大居中矩形。
 * 与 native publish_touch（nvn_vk_swapchain.c）同一套四舍五入，两处必须一致。
 */
final class BufferGeometry {

    /** 缓冲 bufferW×bufferH 等比放进 frameW×frameH 的居中矩形尺寸（<=frame）。 */
    static int[] fit(int bufferW, int bufferH, int frameW, int frameH) {
        if (bufferW <= 0 || bufferH <= 0 || frameW <= 0 || frameH <= 0)
            return new int[] {Math.max(frameW, 0), Math.max(frameH, 0)};
        int w, h;
        if ((long) bufferW * frameH >= (long) bufferH * frameW) {
            w = frameW;
            h = (int) Math.min(frameH, ((long) bufferH * frameW + bufferW / 2) / bufferW);
        } else {
            h = frameH;
            w = (int) Math.min(frameW, ((long) bufferW * frameH + bufferH / 2) / bufferH);
        }
        return new int[] {w, h};
    }

    private BufferGeometry() {}
}
