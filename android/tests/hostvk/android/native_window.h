// 宿主回归用的 Android 头桩：只提供 nvn_vk_swapchain.c 在宿主编译所需的最小声明。
// 真机/NDK 构建永远优先使用 NDK sysroot 的同名头，不会进入本目录。
#pragma once
#include <stdint.h>

struct ANativeWindow;
typedef struct ANativeWindow ANativeWindow;

int32_t ANativeWindow_getWidth(ANativeWindow *window);
int32_t ANativeWindow_getHeight(ANativeWindow *window);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t width, int32_t height, int32_t format);
