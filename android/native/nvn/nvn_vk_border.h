// 采样器边框色在设备没有 VK_EXT_custom_border_color 时的就近替代（backend_vulkan.c 的 nvn_gpu_samp_create 用）。
//
// Vulkan 核心只有三档：0 透明黑 (0,0,0,0)、1 不透明黑 (0,0,0,1)、2 不透明白 (1,1,1,1)，浮点 / 整数各一组同名值。
// 没有扩展时按 RGBA 欧氏距离取最近的一档（等距取下标小的），建成采样器、不丢绘制；这是近似，调用方按采样器建成次数计 sampler_border_color_nearest、每局首个打一行信息级日志（不是红行）。
// 整数视图：先按 samp_border_int 的口径把各分量四舍五入成整数，再在整数三档里取最近。
// 有扩展时不走这里（原值建自定义边框色）。
//
// 纯逻辑、无图形 API 类型；宿主单测见 android/tests/nvk_border_nearest_test.c。
#pragma once

// border：NVN 的 RGBA（整数 setter 也按数值存成浮点）。integer：1 = 给整数格式视图的采样器挑。返回 0 / 1 / 2。
static inline int nvk_border_nearest(const float border[4], int integer) {
    static const double preset[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}};
    double v[4];
    for (int i = 0; i < 4; i++) {
        double f = (double)border[i];
        if (f != f) f = 0;   // NaN 按 0
        if (f > 1e12) f = 1e12;   // 只影响距离大小，不改变哪一档最近；也避免整数转换溢出
        if (f < -1e12) f = -1e12;
        if (integer) f = (double)(long long)(f < 0 ? f - 0.5 : f + 0.5);
        v[i] = f;
    }
    int best = 0;
    double best_d = 0;
    for (int k = 0; k < 3; k++) {
        double d = 0;
        for (int i = 0; i < 4; i++) d += (v[i] - preset[k][i]) * (v[i] - preset[k][i]);
        if (k == 0 || d < best_d) { best = k; best_d = d; }
    }
    return best;
}
