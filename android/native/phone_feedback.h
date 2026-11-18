#pragma once
// 手机当掌机的六轴与震动（#528 第 10 项第一块）：导出 runtime/include/s2a_pad.h 里服务层（services/S2A.Services/ControllerFeedback.cs）
//   按符号查的 s2a_pad_rumble_session / s2a_pad_set_rumble_pair / s2a_pad_set_rumble / s2a_pad_stop_rumble /
//   s2a_pad_set_motion_enabled / s2a_pad_read_motion。六轴读 NDK ASensor（加速度计 + 陀螺仪 200 Hz，游戏启用六轴时才开），
//   震动经 runtime/src/phone_rumble_mailbox.c 的反馈线程回调 Java PhoneFeedback.vibrate（NDK 没有马达接口）。
//   外接手柄自己的马达 / 陀螺仪是第二块，不在这里。
#ifdef __cplusplus
extern "C" {
#endif
// 前后台（bridge.cpp setForeground）：退后台关传感器、停马达、换震动会话；回前台按游戏当前的六轴请求重开传感器
void s2a_android_phone_feedback_foreground(int foreground);
#ifdef __cplusplus
}
#endif
