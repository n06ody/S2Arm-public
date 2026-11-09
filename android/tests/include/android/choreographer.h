// 宿主测试用的 android/choreographer.h 真值镜像：签名逐字摘自 NDK r29
// sysroot/usr/include/android/choreographer.h（AOSP frameworks/native/...，Apache-2.0）。
// 只保留本仓用到的类型与函数声明；__INTRODUCED_IN / __DEPRECATED_IN 注解原样保留、
// 在本文件内定义为空调用（注解是平台元数据，不是签名本体）。
// 用途：测试夹具与 nvn_vsync_android.c 在宿主编译时取真类型/真名字——
// 本文件与真头不一致即编译红（替身函数签名错 → 与声明冲突；名字拼错 → 未声明标识符）。
// 维护：NDK 升级或新增符号使用时，按本机 NDK 真头逐字更新对应行，不许凭记忆写名。
#ifndef S2A_HOSTTEST_ANDROID_CHOREOGRAPHER_H
#define S2A_HOSTTEST_ANDROID_CHOREOGRAPHER_H

#include <stddef.h>
#include <stdint.h>
#ifndef __INTRODUCED_IN
#define __INTRODUCED_IN(api_level)
#endif
#ifndef __DEPRECATED_IN
#define __DEPRECATED_IN(api_level, msg)
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct AChoreographer;
typedef struct AChoreographer AChoreographer;
typedef int64_t AVsyncId;
struct AChoreographerFrameCallbackData;
typedef struct AChoreographerFrameCallbackData AChoreographerFrameCallbackData;

typedef void (*AChoreographer_frameCallback)(long frameTimeNanos, void* data);
typedef void (*AChoreographer_frameCallback64)(int64_t frameTimeNanos, void* data);
typedef void (*AChoreographer_vsyncCallback)(
        const AChoreographerFrameCallbackData* callbackData, void* data);

AChoreographer* AChoreographer_getInstance() __INTRODUCED_IN(24);

void AChoreographer_postFrameCallback(AChoreographer* choreographer,
                                      AChoreographer_frameCallback callback, void* data)
        __INTRODUCED_IN(24) __DEPRECATED_IN(29, "Use AChoreographer_postFrameCallback64 instead");

void AChoreographer_postFrameCallback64(AChoreographer* choreographer,
                                        AChoreographer_frameCallback64 callback, void* data)
        __INTRODUCED_IN(29);

void AChoreographer_postVsyncCallback(AChoreographer* choreographer,
                                        AChoreographer_vsyncCallback callback, void* data)
        __INTRODUCED_IN(33);

int64_t AChoreographerFrameCallbackData_getFrameTimeNanos(
        const AChoreographerFrameCallbackData* data) __INTRODUCED_IN(33);

size_t AChoreographerFrameCallbackData_getFrameTimelinesLength(
        const AChoreographerFrameCallbackData* data) __INTRODUCED_IN(33);

size_t AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(
        const AChoreographerFrameCallbackData* data) __INTRODUCED_IN(33);

AVsyncId AChoreographerFrameCallbackData_getFrameTimelineVsyncId(
        const AChoreographerFrameCallbackData* data, size_t index) __INTRODUCED_IN(33);

int64_t AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(
        const AChoreographerFrameCallbackData* data, size_t index) __INTRODUCED_IN(33);

int64_t AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos(
        const AChoreographerFrameCallbackData* data, size_t index) __INTRODUCED_IN(33);

#ifdef __cplusplus
}
#endif
#endif // S2A_HOSTTEST_ANDROID_CHOREOGRAPHER_H
