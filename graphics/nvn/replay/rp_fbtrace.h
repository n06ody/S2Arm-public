// 一次性诊断缝：后端缺席时仍走原来的视图释放，不改变共用契约。
#pragma once
#include "nvn_backend.h"
enum {
    FT_OTHER, FT_EVICT, FT_STORAGE, FT_FINALIZE, FT_ALIAS,
    FT_REGISTER, FT_PRESENT, FT_SHADOW, FT_TEMP, FT_RESET, FT_UNCACHED, FT_REASONS
};
#ifdef S2A_FBTRACE_IMPLEMENT
void nvn_gpu_fbtrace_release(nvn_gpu_view view, unsigned reason);
#else
void nvn_gpu_fbtrace_release(nvn_gpu_view view, unsigned reason) __attribute__((weak));
static inline void rp_fbtrace_release(nvn_gpu_view view, unsigned reason) {
    if (nvn_gpu_fbtrace_release) nvn_gpu_fbtrace_release(view, reason);
    else nvn_gpu_view_release(view);
}
#endif
