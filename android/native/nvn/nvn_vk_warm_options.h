#pragma once

#include "nvn_plat.h"

static inline int nvk_warm_capture_enabled(void) {
#ifdef S2A_ANDROID_DEBUG_ENV
    return nvn_plat_flag("nvk_warm_capture", 0) && !nvn_plat_flag("nvk_libpack_ssbo_window", 1);
#else
    return 0;
#endif
}

static inline int nvk_warm_nowindow_enabled(void) {
    return nvn_plat_flag("nvk_warm_nowindow", 0) && !nvn_plat_flag("nvk_libpack_ssbo_window", 1);
}
