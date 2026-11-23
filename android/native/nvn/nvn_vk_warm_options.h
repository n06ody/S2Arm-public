#pragma once

#include "nvn_plat.h"

// 普通程序（不带存储缓冲窗口）补件：随包种子 cohort 目录下 nowindow/ 里的普通编译结果与当局管线配方。
//   缺省开（8E 三判据读数见 PR 说明）；随包种子里没有补件目录 / 补件对不上本局闭包、profile、ABI 时这条路径什么也不做，照原样现编。
#define NVK_WARM_NOWINDOW_DEFAULT 1

static inline int nvk_warm_capture_enabled(void) {
#ifdef S2A_ANDROID_DEBUG_ENV
    return nvn_plat_flag("nvk_warm_capture", 0);
#else
    return 0;
#endif
}

static inline int nvk_warm_nowindow_enabled(void) {
    return nvn_plat_flag("nvk_warm_nowindow", NVK_WARM_NOWINDOW_DEFAULT);
}
