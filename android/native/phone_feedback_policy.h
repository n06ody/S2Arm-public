#pragma once
// 安卓手机当掌机的六轴 / 震动（#528 第 10 项）里不碰设备的那部分：纯函数，宿主单测 android/tests/phone_feedback_test.c。
//   设备侧（ASensor / Vibrator）在 phone_feedback.c，换算规矩与 iOS ios/phone_motion.m 同一套，坐标换算本身复用 runtime/src/phone_axes.c。
#include <stdint.h>
#include "s2a_pad.h"

// Display.getRotation()（ROTATION_0..270，画面相对自然方向顺时针转了几个 90°，即机身逆时针转了几个 90°）
//   → 屏幕「右」「上」在传感器坐标（x 自然方向右、y 自然方向上、z 出屏，同 CoreMotion 机身坐标）里的方向，交给 s2a_phone_axes_basis。
//   安卓传感器坐标始终对齐自然方向（横向自然方向的平板也一样），所以不需要 iOS 的面板补转（s2a_phone_panel_fix）。
//   ROTATION_90 = 机身逆时针转 90°（顶端朝左）：右 = −y、上 = x，与 iOS 横屏右的写死换算相同。
static inline void s2a_android_rotation_basis(int rotation, int right[2], int up[2]) {
    static const int r[4][2] = {{1, 0}, {0, -1}, {-1, 0}, {0, 1}}, u[4][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
    int i = rotation & 3;
    right[0] = r[i][0]; right[1] = r[i][1]; up[0] = u[i][0]; up[1] = u[i][1];
}

// 安卓加速度计：m/s²、报支撑反力（静止平放屏幕朝上 z = +9.81）；CoreMotion 报重力方向、单位 g（平放 z = −1）。
//   s2a_phone_axes_basis 按 CoreMotion 口径取反，所以这里先换成 CoreMotion 口径：acc_dev = −a / g。角速度两边都是右手系 rad/s，原样。
#define S2A_ANDROID_STANDARD_GRAVITY 9.80665
static inline void s2a_android_accel_to_coremotion(const float a[3], double out[3]) {
    for (int i = 0; i < 3; i++) out[i] = -(double)a[i] / S2A_ANDROID_STANDARD_GRAVITY;
}

// 一次左右合成的震动 → 安卓 VibrationEffect 振幅（1..255；0 = 停）。同 iOS hp_apply：四路振幅取最大，≤ 0.01 当停。
//   频率在安卓上没有通用的控制口（Vibrator 频率档 API 34 起才有、多数机型不给），只按振幅震。
static inline int s2a_android_rumble_level(const s2a_pad_vibration v[2]) {
    float amp = 0;
    for (int i = 0; i < 2; i++) {
        if (v[i].low_amp > amp) amp = v[i].low_amp;
        if (v[i].high_amp > amp) amp = v[i].high_amp;
    }
    if (!(amp > 0.01f)) return 0;   // 也挡 NaN
    if (amp > 1) amp = 1;
    int level = (int)(amp * 255 + 0.5f);
    return level < 1 ? 1 : level;
}

// 马达节奏：每次下发是一段 S2A_ANDROID_RUMBLE_LEASE_MS 的单次震动（租约，进程卡住 / 漏了停止也最多震这么久，不会一直震下去）。
//   服务层（ControllerFeedback）震着时每 250 ms 续一次，这里离上次下发满 RENEW 才重发，振幅变了才提前重发（20 ms 节流，同 iOS）。
enum { S2A_ANDROID_RUMBLE_LEASE_MS = 1000, S2A_ANDROID_RUMBLE_RENEW_MS = 500, S2A_ANDROID_RUMBLE_THROTTLE_MS = 20 };
typedef struct { int level; uint64_t issued_ns; } s2a_android_rumble_state;   // level 0 = 没在震
// 返回这一次要下发的振幅：-1 = 什么都不做；0 = 取消；1..255 = 下发一段租约。state 随之更新
static inline int s2a_android_rumble_step(s2a_android_rumble_state *s, int level, uint64_t now_ns) {
    if (level <= 0) {
        if (!s->level) return -1;
        s->level = 0; s->issued_ns = 0;
        return 0;
    }
    uint64_t age = now_ns - s->issued_ns;
    if (s->level) {
        if (level == s->level && age < (uint64_t)S2A_ANDROID_RUMBLE_RENEW_MS * 1000000u) return -1;
        if (level != s->level && age < (uint64_t)S2A_ANDROID_RUMBLE_THROTTLE_MS * 1000000u) return -1;
    }
    s->level = level; s->issued_ns = now_ns;
    return level;
}
