// 手机六轴 / 震动的纯换算（android/native/phone_feedback_policy.h + 共用 runtime/src/phone_axes.c）。
// 期望值按物理姿态手算，不跑被测实现：NS2 原始坐标 = （屏幕右、屏幕上、出屏），加速度报支撑反力，单位 g、度/秒。
#include "phone_feedback_policy.h"
#include "phone_axes.h"
#include <math.h>
#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
static int near(float a, float b) { return fabsf(a - b) < 1e-3f; }

// 安卓原始读数（加速度 m/s² 支撑反力、角速度 rad/s，传感器坐标对齐自然方向）→ NS2 原始坐标
static void convert(int rotation, const float acc[3], const float gyr[3], float a[3], float g[3]) {
    double acc_dev[3], gyr_dev[3] = {gyr[0], gyr[1], gyr[2]};
    int right[2], up[2];
    s2a_android_accel_to_coremotion(acc, acc_dev);
    s2a_android_rotation_basis(rotation, right, up);
    s2a_phone_axes_basis(right, up, acc_dev, gyr_dev, a, g);
}

static void axes(void) {
    const float g0 = 9.80665f, none[3] = {0, 0, 0};
    float a[3], g[3];
    // 平放屏幕朝上：不论哪个方向，NS2 平放读 z = +1 g（真报文 report05 ≈ (−0.02, 0.14, 1.01)）
    for (int r = 0; r < 4; r++) {
        const float flat[3] = {0, 0, g0};
        convert(r, flat, none, a, g);
        CHECK(near(a[0], 0) && near(a[1], 0) && near(a[2], 1));
    }
    // ROTATION_90（机身逆时针转 90°、顶端朝左，横着竖起来屏幕对人）：机身 x 朝上，支撑反力 = +x；NS2 上沿朝上读 y = +1
    { const float up_x[3] = {g0, 0, 0}; convert(1, up_x, none, a, g); CHECK(near(a[0], 0) && near(a[1], 1) && near(a[2], 0)); }
    // ROTATION_270（顶端朝右）：机身 −x 朝上
    { const float up_nx[3] = {-g0, 0, 0}; convert(3, up_nx, none, a, g); CHECK(near(a[0], 0) && near(a[1], 1) && near(a[2], 0)); }
    // ROTATION_0（竖着拿）：机身 y 朝上
    { const float up_y[3] = {0, g0, 0}; convert(0, up_y, none, a, g); CHECK(near(a[0], 0) && near(a[1], 1) && near(a[2], 0)); }
    // ROTATION_180（倒着竖拿）：机身 −y 朝上
    { const float up_ny[3] = {0, -g0, 0}; convert(2, up_ny, none, a, g); CHECK(near(a[0], 0) && near(a[1], 1) && near(a[2], 0)); }
    // 绕出屏轴逆时针转 1 rad/s：不论方向 NS2 z 都是 +57.3°/s（出屏轴不随屏幕方向变）
    for (int r = 0; r < 4; r++) {
        const float spin_z[3] = {0, 0, 1};
        convert(r, none, spin_z, a, g);
        CHECK(near(g[0], 0) && near(g[1], 0) && near(g[2], 57.29578f));
    }
    // ROTATION_90：屏幕上 = 机身 +x，绕机身 x 转 = 绕 NS2 y 转；屏幕右 = 机身 −y，绕机身 y 转 = 绕 NS2 −x 转
    { const float spin_x[3] = {1, 0, 0}; convert(1, none, spin_x, a, g); CHECK(near(g[0], 0) && near(g[1], 57.29578f) && near(g[2], 0)); }
    { const float spin_y[3] = {0, 1, 0}; convert(1, none, spin_y, a, g); CHECK(near(g[0], -57.29578f) && near(g[1], 0) && near(g[2], 0)); }
    // ROTATION_270：屏幕右 = 机身 +y
    { const float spin_y[3] = {0, 1, 0}; convert(3, none, spin_y, a, g); CHECK(near(g[0], 57.29578f) && near(g[1], 0) && near(g[2], 0)); }
    // 越界的方向值按低两位取，不读出表外
    { int r2[2], u2[2]; s2a_android_rotation_basis(5, r2, u2); CHECK(r2[0] == 0 && r2[1] == -1 && u2[0] == 1 && u2[1] == 0); }
}

static void level(void) {
    s2a_pad_vibration v[2] = {{0}};
    CHECK(s2a_android_rumble_level(v) == 0);
    v[1].high_amp = 0.005f; CHECK(s2a_android_rumble_level(v) == 0);         // ≤ 0.01 当停（同 iOS）
    v[1].high_amp = 0.5f; v[0].low_amp = 0.2f; CHECK(s2a_android_rumble_level(v) == 128);   // 四路取最大：0.5 × 255 四舍五入
    v[0].low_amp = 3; CHECK(s2a_android_rumble_level(v) == 255);              // 超过 1 封顶
    v[0].low_amp = NAN; v[1].high_amp = 0; CHECK(s2a_android_rumble_level(v) == 0);   // NaN 不震
    v[0].low_amp = 0.011f; CHECK(s2a_android_rumble_level(v) == 3);
}

static void step(void) {
    const uint64_t ms = 1000000;
    s2a_android_rumble_state s = {0};
    CHECK(s2a_android_rumble_step(&s, 0, 1000 * ms) == -1);    // 没在震时停：不下发
    CHECK(s2a_android_rumble_step(&s, 100, 1000 * ms) == 100); // 起震：下发一段租约
    CHECK(s2a_android_rumble_step(&s, 100, 1250 * ms) == -1);  // 服务层 250 ms 续：没到 500 ms 不重发
    CHECK(s2a_android_rumble_step(&s, 100, 1500 * ms) == 100); // 满 500 ms：重发，租约（1000 ms）不断
    CHECK(s2a_android_rumble_step(&s, 200, 1510 * ms) == -1);  // 20 ms 内变振幅：节流
    CHECK(s2a_android_rumble_step(&s, 200, 1520 * ms) == 200); // 过了 20 ms：按新振幅下发
    CHECK(s2a_android_rumble_step(&s, 0, 1521 * ms) == 0);     // 停：立刻取消，不受节流
    CHECK(s2a_android_rumble_step(&s, 0, 1600 * ms) == -1);    // 再停：不重复取消
    CHECK(s2a_android_rumble_step(&s, 50, 1601 * ms) == 50);   // 停后紧接着再起：不受节流
    CHECK(S2A_ANDROID_RUMBLE_RENEW_MS + 250 < S2A_ANDROID_RUMBLE_LEASE_MS);   // 服务层 250 ms 一续时，下一次重发一定落在租约到期前
}

int main(void) {
    axes();
    level();
    step();
    if (failures) { fprintf(stderr, "phone_feedback_test: %d failed\n", failures); return 1; }
    puts("phone_feedback_test: ok");
    return 0;
}
