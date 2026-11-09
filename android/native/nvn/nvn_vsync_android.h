// 直连上屏节拍助手（nvn_be_link_* 契约的安卓实现）：架在
// graphics/nvn/gfx/nvn/nvn_vsync.h 的 AChoreographer 节拍源之上。
// debug.s2a.vsync（默认关，等设备 A/B）门内懒启动节拍源；
// wait_tick 按契约数拍（vsyncs>0 数 60Hz 槽位，0 数一拍实际显示拍）。
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  s2a_android_link_active(void);                              // 门开且节拍源可用
int  s2a_android_link_wait_tick(int vsyncs, uint64_t timeout_ns); // 1 = 等到了；0 = 超时/不可用（调用方回退自己的 60Hz 时钟）
void s2a_android_link_stop(void);                                // 停节拍（退后台/停局）；没在跑则无操作

// ---- 宿主测试替身（-DS2A_VSYNC_TEST 时模块调用这两个符号）----
// 回调签名与 NDK 一致（经 ops 表转发）：
typedef void (*s2a_vs_vsync_cb)(const void *data, void *user);     // AChoreographer_vsyncCallback，API 33
typedef void (*s2a_vs_frame64_cb)(int64_t frame_ns, void *user);   // AChoreographer_frameCallback64，API 29
typedef void (*s2a_vs_frame_cb)(long frame_ns, void *user);        // AChoreographer_frameCallback，API 24
struct s2a_vs_ops {                                                // dlsym 解析出的平台符号表
    void *(*looper_prepare)(int flags);                            // ALooper_prepare
    int   (*looper_poll_once)(int ms, int *fd, int *ev, void **data); // ALooper_pollOnce（-1 阻塞到有派发）
    void  (*looper_wake)(void *looper);                            // ALooper_wake
    void *(*choreo_get)(void);                                     // AChoreographer_getInstance
    void  (*post_vsync64)(void *choreo, s2a_vs_vsync_cb cb, void *user);   // API 33 首选
    void  (*post_frame64)(void *choreo, s2a_vs_frame64_cb cb, void *user); // API 29 次选
    void  (*post_frame)(void *choreo, s2a_vs_frame_cb cb, void *user);     // API 24 兜底
    int64_t (*frame_time_ns)(const void *data);                    // AChoreographerFrameCallbackData_getFrameTimeNanos
    size_t  (*tl_len)(const void *data);                           // ..._getFrameTimelinesLength
    int64_t (*tl_vsync_ns)(const void *data, size_t i);            // ..._getFrameTimelineVsyncNanos
    int64_t (*tl_deadline_ns)(const void *data, size_t i);         // ..._getFrameTimelineDeadlineNanos（= 目标呈现时刻）
};
#ifdef S2A_VSYNC_TEST
int s2a_vsync_test_enabled(void);                // 替身门值（1/0）
const struct s2a_vs_ops *s2a_vsync_test_ops(void); // 替身 ops；NULL = 走真实 dlsym 探测
#endif

#ifdef __cplusplus
}
#endif
