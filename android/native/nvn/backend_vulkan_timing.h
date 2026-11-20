// Optional GOOGLE display timing diagnostics for one swapchain generation.
// All timestamps use CLOCK_MONOTONIC, as does Android's actualPresentTime.
#ifndef S2A_BACKEND_VULKAN_TIMING_H
#define S2A_BACKEND_VULKAN_TIMING_H

#define WSI_TIMING_CAP 2048u
#define WSI_TIMING_BATCH 16u
struct wsi_timing {
    PFN_vkGetPastPresentationTimingGOOGLE past;
    const char *reason;
    uint32_t next_id, pending[WSI_TIMING_CAP], pending_count;
    uint64_t start, samples[WSI_TIMING_CAP];
    uint32_t nsamples, late, invalid, lost;
};

static void wsi_timing_init(struct wsi_timing *t, VkDevice dev, int enabled, int supported) {
    memset(t, 0, sizeof *t);
    t->next_id = 1;
    if (!enabled) t->reason = "开关未启用";
    else if (!supported) t->reason = "扩展未启用或不支持";
    else {
        t->past = (PFN_vkGetPastPresentationTimingGOOGLE)vkGetDeviceProcAddr(dev, "vkGetPastPresentationTimingGOOGLE");
        if (!t->past) t->reason = "扩展入口不可用";
    }
}

static uint32_t wsi_timing_id(struct wsi_timing *t) {
    if (t->reason || !t->next_id) return 0;
    uint32_t id = t->next_id++;
    return id;
}

static void wsi_timing_submitted(struct wsi_timing *t, uint32_t id) {
    if (!id) return;
    uint32_t *slot = &t->pending[id % WSI_TIMING_CAP];
    if (*slot) { t->lost++; t->pending_count--; }
    *slot = id; t->pending_count++;
}

// A data call consumes the returned records even on VK_INCOMPLETE.
// Bound work per present; leave any excess in the driver for the next poll.
static void wsi_timing_poll(struct wsi_timing *t, VkDevice dev, VkSwapchainKHR sc) {
    if (t->reason || !t->past) return;
    for (unsigned batch = 0; batch < 8; batch++) {
        VkPastPresentationTimingGOOGLE values[WSI_TIMING_BATCH];
        uint32_t n = WSI_TIMING_BATCH;
        VkResult r = t->past(dev, sc, &n, values);
        // The call can return an event that occurred after polling began.
        uint64_t now = now_ns();
        if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n > WSI_TIMING_BATCH) { t->invalid++; break; }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t id = values[i].presentID, *slot = &t->pending[id % WSI_TIMING_CAP];
            if (!id || *slot != id) { t->invalid++; continue; }
            *slot = 0; t->pending_count--;
            uint64_t at = values[i].actualPresentTime;
            if (!at || at > now) { t->invalid++; continue; }
            if (at < t->start) { t->late++; continue; }
            if (t->nsamples == WSI_TIMING_CAP) { t->lost++; continue; }
            t->samples[t->nsamples++] = at;
            if (s2a_ft_present_actual) s2a_ft_present_actual(id, at);   // 探针：按 presentID 回填 actual_present（同 CLOCK_MONOTONIC）
        }
        if (r == VK_SUCCESS) break;
        if (!n) { t->invalid++; break; }
    }
}

static int wsi_timing_cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void wsi_timing_report(struct wsi_timing *t, uint64_t now) {
    if (!t->start) { t->start = now; return; }
    if (now < t->start || now - t->start < 5000000000ull) return;
    char b[512];
    const char *reason = t->reason;
    if (!reason && !t->next_id) reason = "呈现 ID 耗尽";
    // 迟到 = 上一窗口末尾几帧的上屏记录，回收要晚几帧，窗口切换时必然有；照计数、不算本窗口不完整（真机每窗 3 条左右）。
    if (!reason && (t->lost || t->invalid)) reason = "记录不完整";
    if (!reason && t->nsamples < 2) reason = "样本不足";
    if (reason) {
        snprintf(b, sizeof b, "分时（实际上屏）：帧率= 间隔中位= 间隔P99= 间隔最大= 间隔>100ms=；原因=%s 样本=%u 待回收=%u 迟到=%u 丢失=%u 异常=%u",
                 reason, t->nsamples, t->pending_count, t->late, t->lost, t->invalid);
    } else {
        qsort(t->samples, t->nsamples, sizeof t->samples[0], wsi_timing_cmp_u64);
        uint64_t gaps[WSI_TIMING_CAP - 1]; uint32_t ng = t->nsamples - 1;
        for (uint32_t i = 0; i < ng; i++) gaps[i] = t->samples[i + 1] - t->samples[i];
        uint64_t span = t->samples[t->nsamples - 1] - t->samples[0];
        qsort(gaps, ng, sizeof gaps[0], wsi_timing_cmp_u64);
        uint32_t over = 0; for (uint32_t i = 0; i < ng; i++) over += gaps[i] > 100000000ull;   // 相邻两次实际上屏间隔严格大于 100 ms 的次数（已回收样本）
        if (!span) snprintf(b, sizeof b, "分时（实际上屏）：帧率= 间隔中位= 间隔P99= 间隔最大= 间隔>100ms=；原因=时间戳相同 样本=%u 待回收=%u", t->nsamples, t->pending_count);
        else snprintf(b, sizeof b, "分时（实际上屏，已回收样本）：帧率=%.2f 间隔中位=%.2fms 间隔P99=%.2fms 间隔最大=%.2fms 间隔>100ms=%u；样本=%u 待回收=%u 迟到=%u",
                      (double)ng * 1e9 / (double)span, (double)gaps[(ng - 1) / 2] / 1e6,
                      (double)gaps[((uint64_t)ng * 99 + 99) / 100 - 1] / 1e6,
                      (double)gaps[ng - 1] / 1e6, over, t->nsamples, t->pending_count, t->late);
    }
    nvn_plat_log(NVN_PLAT_LOG_INFO, "present", b);
    t->start = now; t->nsamples = t->late = t->invalid = t->lost = 0;
}
#endif
