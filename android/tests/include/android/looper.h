// 宿主测试用的 android/looper.h 真值镜像：签名逐字摘自 NDK r29
// sysroot/usr/include/android/looper.h（Apache-2.0）。只保留本仓用到的三函数与
// ALooper 不透明类型；规则同 choreographer.h（与真头不一致即编译红）。
#ifndef S2A_HOSTTEST_ANDROID_LOOPER_H
#define S2A_HOSTTEST_ANDROID_LOOPER_H

#ifdef __cplusplus
extern "C" {
#endif

struct ALooper;
typedef struct ALooper ALooper;

enum { ALOOPER_PREPARE_ALLOW_NON_CALLBACKS = 1<<0 };

ALooper* ALooper_prepare(int opts);
int ALooper_pollOnce(int timeoutMillis, int* outFd, int* outEvents, void** outData);
void ALooper_wake(ALooper* looper);

#ifdef __cplusplus
}
#endif
#endif // S2A_HOSTTEST_ANDROID_LOOPER_H
