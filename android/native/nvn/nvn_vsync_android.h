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
// NDK 真类型：设备编进 NDK 真头；宿主由 CMake -idirafter 本机 NDK sysroot 编同一份真头
// （签名不一致即编译红；真头不 vendored 进仓库）。
#include <android/choreographer.h>
#include <android/looper.h>
struct s2a_vs_ops {                                        // dlsym 解析出的平台符号表（字段名 = NDK 真名）
    ALooper *(*ALooper_prepare)(int opts);
    int      (*ALooper_pollOnce)(int timeoutMillis, int *outFd, int *outEvents, void **outData);
    void     (*ALooper_wake)(ALooper *looper);
    AChoreographer *(*AChoreographer_getInstance)(void);
    void (*AChoreographer_postVsyncCallback)(AChoreographer *, AChoreographer_vsyncCallback, void *);   // API 33 首选
    void (*AChoreographer_postFrameCallback64)(AChoreographer *, AChoreographer_frameCallback64, void *); // API 29 次选
    void (*AChoreographer_postFrameCallback)(AChoreographer *, AChoreographer_frameCallback, void *);   // API 24 兜底
    int64_t (*AChoreographerFrameCallbackData_getFrameTimeNanos)(const AChoreographerFrameCallbackData *);
    size_t  (*AChoreographerFrameCallbackData_getFrameTimelinesLength)(const AChoreographerFrameCallbackData *);
    size_t  (*AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex)(const AChoreographerFrameCallbackData *);
    int64_t (*AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos)(
            const AChoreographerFrameCallbackData *, size_t index);
};
#ifdef S2A_VSYNC_TEST
int s2a_vsync_test_enabled(void);                // 替身门值（1/0）
void *(*s2a_vsync_test_dlsym(void))(const char *name); // 替身符号查找（NULL=源缺席；表项名须为真头声明过的标识符）
#endif

#ifdef __cplusplus
}
#endif
