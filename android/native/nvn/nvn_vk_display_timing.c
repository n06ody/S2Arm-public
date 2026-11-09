// B2：VK_GOOGLE_display_timing 上屏时间计量。见头注释。
#include "nvn_vk_display_timing.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#if defined(__ANDROID__) && !defined(S2A_DT_TEST)
#include <android/log.h>
#include <sys/system_properties.h>
#define dt_log(...) __android_log_print(ANDROID_LOG_INFO, "S2Arm", __VA_ARGS__)
#else
#define dt_log(...) ((void)0)
#endif

#define DT_CAP 256          // nvn_be_presented_time 回看窗（hle 最多回 8，余量对齐 present_at）
#define DT_BATCH 16         // 每帧机会式排干的上限（FIFO 下 1-2 条/帧，满批防欠债雪崩）
#define DT_GAP_CAP 128      // 上屏间隔近窗（QA 读数）

enum { DT_OFF = 0, DT_DISABLED, DT_UNSUPPORTED, DT_ACTIVE };

static int g_state = DT_OFF;
static char g_why[96];
static PFN_vkGetPastPresentationTimingGOOGLE g_past;
static PFN_vkGetRefreshCycleDurationGOOGLE g_refresh;
static VkDevice g_dev;
static uint64_t g_refresh_ns;
static uint32_t g_next_pid = 1;                 // presentID 只发在帧路径上，永不清零回绕到 0
static uint64_t g_tagged, g_received;           // 已挂 id 的 present 数 / 已回报时刻数

// pid→frame：不假设二者连续（挂 id 后 present 仍可能失败丢号）。槽位写 pid 用 release，
// 读者先 acquire pid 再读 frame，乱序回报也按 pid 对号而不是按到达顺序。
struct dt_pidmap { _Atomic uint32_t pid; _Atomic uint64_t frame; };
static struct dt_pidmap g_pidmap[DT_CAP];
// frame→actual：同上，at 先写、frame tag 后写；读者对 tag 防环覆。
struct dt_shown { _Atomic uint64_t frame; _Atomic uint64_t at_ns; };
static struct dt_shown g_shown[DT_CAP];
static double g_gaps[DT_GAP_CAP];               // 相邻两帧都拿到时刻才出样本
static uint64_t g_gap_n;

#ifdef S2A_DT_TEST
static int g_test_gate = -1;
void nvn_vk_dt_test_gate(int on) { g_test_gate = on; }
void nvn_vk_dt_test_reset(void) { nvn_vk_dt_detach(); g_test_gate = -1; }
#endif

static int dt_gate(void) {
#ifdef S2A_DT_TEST
    if (g_test_gate >= 0) return g_test_gate;
#endif
#if defined(__ANDROID__) && !defined(S2A_DT_TEST)
    char v[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.s2a.display_timing", v);
    return v[0] != '0';   // 默认开计量，显式 0 才关
#else
    return 0;
#endif
}

void nvn_vk_dt_attach(VkDevice device, int supported,
                      PFN_vkGetPastPresentationTimingGOOGLE past_fn,
                      PFN_vkGetRefreshCycleDurationGOOGLE refresh_fn) {
    nvn_vk_dt_detach();
    g_dev = device;
    g_past = past_fn;
    g_refresh = refresh_fn;
    if (!dt_gate()) {
        g_state = DT_DISABLED;
        snprintf(g_why, sizeof g_why, "disabled (debug.s2a.display_timing=0)");
    } else if (!supported) {
        g_state = DT_UNSUPPORTED;
        snprintf(g_why, sizeof g_why, "unsupported (VK_GOOGLE_display_timing absent)");
    } else if (!g_past) {
        g_state = DT_UNSUPPORTED;
        snprintf(g_why, sizeof g_why, "unsupported (vkGetPastPresentationTimingGOOGLE missing)");
    } else {
        g_state = DT_ACTIVE;
        snprintf(g_why, sizeof g_why, "active");
    }
    dt_log("display timing: %s", g_why);
}

void nvn_vk_dt_detach(void) {
    g_state = DT_OFF;
    g_past = NULL;
    g_refresh = NULL;
    g_dev = VK_NULL_HANDLE;
    g_refresh_ns = 0;
    g_next_pid = 1;
    g_tagged = g_received = g_gap_n = 0;
    memset(g_pidmap, 0, sizeof g_pidmap);
    memset(g_shown, 0, sizeof g_shown);
    memset(g_gaps, 0, sizeof g_gaps);
}

int nvn_vk_dt_active(void) { return g_state == DT_ACTIVE; }
const char *nvn_vk_dt_state(void) {
    return g_state == DT_ACTIVE ? "active" : g_state == DT_DISABLED ? g_why :
           g_state == DT_UNSUPPORTED ? g_why : "off";
}

int nvn_vk_dt_chain(VkPresentInfoKHR *pi, VkPresentTimesInfoGOOGLE *ti,
                    VkPresentTimeGOOGLE *t, uint64_t frame) {
    if (g_state != DT_ACTIVE || !pi || !ti || !t || !frame) return 0;
    uint32_t pid = g_next_pid++;
    // frame tag 先写、pid 后写（release）：poll 侧 acquire pid 后读 frame 不会拿到半成品。
    atomic_store_explicit(&g_pidmap[pid % DT_CAP].frame, frame, memory_order_relaxed);
    atomic_store_explicit(&g_pidmap[pid % DT_CAP].pid, pid, memory_order_release);
    t->presentID = pid;
    t->desiredPresentTime = 0;      // 0 = 最早可排：本件只计量，排队另起（与 nvn_vsync 节拍源配合）
    ti->sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE;
    ti->swapchainCount = pi->swapchainCount;
    ti->pTimes = t;
    ti->pNext = pi->pNext;          // 挂进既有链头（present fence 等 pNext 不丢）
    pi->pNext = ti;
    g_tagged++;
    return 1;
}

// 记一条「frame 在 at_ns 上屏」；frame tag 后写防环覆误配。顺带产出相邻帧间隔样本。
static void dt_note(uint64_t frame, uint64_t at_ns) {
    struct dt_shown *e = &g_shown[frame % DT_CAP];
    atomic_store_explicit(&e->at_ns, at_ns, memory_order_relaxed);
    atomic_store_explicit(&e->frame, frame, memory_order_release);
    struct dt_shown *prev = &g_shown[(frame - 1) % DT_CAP];
    if (frame > 1 && atomic_load_explicit(&prev->frame, memory_order_acquire) == frame - 1) {
        uint64_t p = atomic_load_explicit(&prev->at_ns, memory_order_relaxed);
        if (p && at_ns > p) g_gaps[g_gap_n++ % DT_GAP_CAP] = (double)(at_ns - p) / 1e6;
    }
    g_received++;
}

void nvn_vk_dt_poll(VkDevice device, VkSwapchainKHR sc) {
    if (g_state != DT_ACTIVE || !g_past || device != g_dev) return;
    if (g_refresh && !g_refresh_ns) {
        VkRefreshCycleDurationGOOGLE rc;
        memset(&rc, 0, sizeof rc);
        if (g_refresh(device, sc, &rc) == VK_SUCCESS) g_refresh_ns = rc.refreshDuration;
    }
    for (int round = 0; round < 4; round++) {
        uint32_t count = 0;
        if (g_past(device, sc, &count, NULL) != VK_SUCCESS || !count) return;
        VkPastPresentationTimingGOOGLE ts[DT_BATCH];
        uint32_t n = count < DT_BATCH ? count : DT_BATCH;
        if (g_past(device, sc, &n, ts) != VK_SUCCESS || !n) return;
        for (uint32_t i = 0; i < n; i++) {
            struct dt_pidmap *m = &g_pidmap[ts[i].presentID % DT_CAP];
            if (atomic_load_explicit(&m->pid, memory_order_acquire) != ts[i].presentID) continue;
            uint64_t frame = atomic_load_explicit(&m->frame, memory_order_relaxed);
            if (frame) dt_note(frame, ts[i].actualPresentTime);
        }
        if (n >= count) return;     // 已到时刻取尽
    }
}

double nvn_vk_dt_shown(uint64_t n) {
    struct dt_shown *e = &g_shown[n % DT_CAP];
    if (!n || atomic_load_explicit(&e->frame, memory_order_acquire) != n) return 0;
    return (double)atomic_load_explicit(&e->at_ns, memory_order_relaxed) / 1e9;
}

void nvn_vk_dt_stats(uint64_t *tagged, uint64_t *received, uint64_t *pending,
                     double *gap_p50_ms, double *gap_p95_ms, double *gap_max_ms, double *refresh_ns) {
    if (tagged) *tagged = g_tagged;
    if (received) *received = g_received;
    if (pending) *pending = g_tagged > g_received ? g_tagged - g_received : 0;
    if (refresh_ns) *refresh_ns = (double)g_refresh_ns;
    uint64_t ns = g_gap_n < DT_GAP_CAP ? g_gap_n : DT_GAP_CAP;
    double sorted[DT_GAP_CAP], max = 0;
    for (uint64_t i = 0; i < ns; i++) {
        double v = g_gaps[i];
        if (v > max) max = v;
        uint64_t j = i;
        while (j > 0 && sorted[j - 1] > v) { sorted[j] = sorted[j - 1]; j--; }
        sorted[j] = v;
    }
    if (gap_p50_ms) *gap_p50_ms = ns ? sorted[(ns + 1) / 2 - 1] : 0;
    if (gap_p95_ms) *gap_p95_ms = ns ? sorted[((ns * 95 + 99) / 100 - 1) < ns ? (ns * 95 + 99) / 100 - 1 : ns - 1] : 0;
    if (gap_max_ms) *gap_max_ms = max;
}
