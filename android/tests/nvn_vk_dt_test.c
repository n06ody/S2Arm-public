// B2 nvn_vk_display_timing 宿主回归：VK_GOOGLE_display_timing 替身。
// 场景：支持→挂 id/回读真值；不支持→明确 unavailable；回报乱序→按 pid 对号；
// 丢帧（时刻永不到）→该帧 UNKNOWN 不冒名；门关闭→不挂不查；ring 环覆→旧帧安全 0。
// 修前红对照：天真实现按「到达顺序」回填时刻（乱序场景红）/或返回 CPU now（真值场景红）。
#define S2A_DT_TEST 1
#include "../native/nvn/nvn_vk_display_timing.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL[%d]: %s\n", __LINE__, what); fails++; } } while (0)

// ---- vkGetPastPresentationTimingGOOGLE 替身：到达队列按真实语义消费（查过不再报） ----
static VkPastPresentationTimingGOOGLE fq[16];
static int fq_n;
static int fq_fail;
static VkResult fake_past(VkDevice dev, VkSwapchainKHR sc, uint32_t *count,
                          VkPastPresentationTimingGOOGLE *out) {
    (void)dev; (void)sc;
    if (fq_fail) return VK_ERROR_UNKNOWN;
    if (!out) { *count = (uint32_t)fq_n; return VK_SUCCESS; }
    uint32_t n = *count < (uint32_t)fq_n ? *count : (uint32_t)fq_n;
    memcpy(out, fq, n * sizeof *out);
    memmove(fq, fq + n, (size_t)(fq_n - (int)n) * sizeof *out);
    fq_n -= (int)n;
    *count = n;
    return VK_SUCCESS;
}
static VkResult fake_refresh(VkDevice dev, VkSwapchainKHR sc, VkRefreshCycleDurationGOOGLE *rc) {
    (void)dev; (void)sc;
    rc->refreshDuration = 16666666;
    return VK_SUCCESS;
}
static void fq_push(uint32_t pid, uint64_t at_ns) {
    fq[fq_n].presentID = pid; fq[fq_n].actualPresentTime = at_ns;
    fq[fq_n].desiredPresentTime = 0; fq[fq_n].earliestPresentTime = 0; fq[fq_n].presentMargin = 0;
    fq_n++;
}

static const VkDevice DEV = (VkDevice)0x1;
static const VkSwapchainKHR SC = (VkSwapchainKHR)0x2;

static VkPresentInfoKHR mk_pi(void) {
    VkPresentInfoKHR pi;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.swapchainCount = 1;
    return pi;
}

static void attach_on(void) {
    nvn_vk_dt_test_reset();
    nvn_vk_dt_test_gate(1);
    nvn_vk_dt_attach(DEV, 1, fake_past, fake_refresh);
    fq_n = 0; fq_fail = 0;
}
// 模拟一帧 present：pi 经 chain 挂 id 后排干回报
static int fake_present(uint64_t frame, VkPresentInfoKHR *pi, VkPresentTimesInfoGOOGLE *ti,
                        VkPresentTimeGOOGLE *t) {
    return nvn_vk_dt_chain(pi, ti, t, frame);
}

int main(void) {
    // 1) 门关闭：不 attach 成功语义 → disabled 状态、不挂 id
    nvn_vk_dt_test_reset();
    nvn_vk_dt_test_gate(0);
    nvn_vk_dt_attach(DEV, 1, fake_past, fake_refresh);
    CHECK(!nvn_vk_dt_active(), "gate off → inactive");
    CHECK(strstr(nvn_vk_dt_state(), "disabled") != NULL, "state reports disabled");
    {
        VkPresentInfoKHR pi = mk_pi();
        VkPresentTimesInfoGOOGLE ti; VkPresentTimeGOOGLE t;
        pi.swapchainCount = 1;
        CHECK(!fake_present(1, &pi, &ti, &t), "gate off → no presentID attached");
    }

    // 2) 扩展缺席：明确 unsupported，链原样
    nvn_vk_dt_test_reset();
    nvn_vk_dt_test_gate(1);
    nvn_vk_dt_attach(DEV, 0, fake_past, fake_refresh);
    CHECK(!nvn_vk_dt_active(), "unsupported → inactive");
    CHECK(strstr(nvn_vk_dt_state(), "unsupported") != NULL, "state reports unavailable reason");
    {
        VkPresentInfoKHR pi = mk_pi();
        int sentinel; pi.pNext = &sentinel; pi.swapchainCount = 1;
        VkPresentTimesInfoGOOGLE ti; VkPresentTimeGOOGLE t;
        CHECK(!fake_present(1, &pi, &ti, &t), "unsupported → no chain");
        CHECK(pi.pNext == &sentinel, "unsupported → pNext untouched");
    }
    CHECK(nvn_vk_dt_shown(1) == 0, "unsupported → shown unknown");

    // 3) 支持：挂 id、保持既有 pNext、回报真值（秒）
    attach_on();
    {
        VkPresentInfoKHR pi = mk_pi();
        int fence_info; pi.pNext = &fence_info; pi.swapchainCount = 1;
        VkPresentTimesInfoGOOGLE ti; VkPresentTimeGOOGLE t;
        CHECK(fake_present(1, &pi, &ti, &t), "active → attached");
        CHECK(t.presentID == 1, "pid starts at 1");
        CHECK(t.desiredPresentTime == 0, "measurement only: desired=earliest");
        CHECK(pi.pNext == &ti && ti.pNext == &fence_info, "chain preserves existing pNext");
        CHECK(ti.swapchainCount == 1, "times info swapchainCount mirrors present");
        CHECK(nvn_vk_dt_shown(1) == 0, "pending → unknown (no CPU-time impersonation)");
        fq_push(1, 1000000000ull);
        nvn_vk_dt_poll(DEV, SC);
        double at = nvn_vk_dt_shown(1);
        CHECK(at > 0.999 && at < 1.001, "actual present time returned in host seconds");
    }

    // 4) 回报乱序：pid 3 先于 pid 2 到 → 按 pid 对号，不按到达顺序
    {
        VkPresentInfoKHR pi = mk_pi();
        VkPresentTimesInfoGOOGLE ti; VkPresentTimeGOOGLE t;
        fake_present(2, &pi, &ti, &t);   // pid 2 → frame 2
        fake_present(3, &pi, &ti, &t);   // pid 3 → frame 3
        fq_push(3, 1030000000ull);       // 乱序：3 先回报
        fq_push(2, 1020000000ull);
        nvn_vk_dt_poll(DEV, SC);
        double a2 = nvn_vk_dt_shown(2), a3 = nvn_vk_dt_shown(3);
        CHECK(a2 > 1.019 && a2 < 1.021, "out-of-order: frame2 maps its own pid");
        CHECK(a3 > 1.029 && a3 < 1.031, "out-of-order: frame3 maps its own pid");
    }

    // 5) 丢帧：pid 时刻永不到 → 该帧 UNKNOWN，邻帧不受影响
    {
        VkPresentInfoKHR pi = mk_pi();
        VkPresentTimesInfoGOOGLE ti; VkPresentTimeGOOGLE t;
        fake_present(4, &pi, &ti, &t);   // pid 4
        fake_present(5, &pi, &ti, &t);   // pid 5（丢：永不回报）
        fake_present(6, &pi, &ti, &t);   // pid 6
        fq_push(4, 1040000000ull);
        fq_push(6, 1060000000ull);       // 5 缺席
        nvn_vk_dt_poll(DEV, SC);
        CHECK(nvn_vk_dt_shown(5) == 0, "dropped frame → UNKNOWN, not fabricated");
        CHECK(nvn_vk_dt_shown(4) > 0 && nvn_vk_dt_shown(6) > 0, "neighbors still resolve");
        uint64_t pd = 0;
        nvn_vk_dt_stats(NULL, NULL, &pd, NULL, NULL, NULL, NULL);
        CHECK(pd == 1, "pending counts the dropped tag, not invented");

        // 6) 迟到回报仍可补：下一帧 poll 时 pid5 到 → frame5 补真值
        fq_push(5, 1050000000ull);
        nvn_vk_dt_poll(DEV, SC);
        CHECK(nvn_vk_dt_shown(5) > 1.049 && nvn_vk_dt_shown(5) < 1.051, "late timing still binds by pid");
    }

    // 7) frame=0（无 shim / 诊断 present）：不挂 id、不消耗序号
    {
        uint64_t tagged0 = 0;
        nvn_vk_dt_stats(&tagged0, NULL, NULL, NULL, NULL, NULL, NULL);
        VkPresentInfoKHR pi = mk_pi();
        VkPresentTimesInfoGOOGLE ti; VkPresentTimeGOOGLE t;
        CHECK(!fake_present(0, &pi, &ti, &t), "frame 0 → untagged");
        uint64_t tagged1 = 0;
        nvn_vk_dt_stats(&tagged1, NULL, NULL, NULL, NULL, NULL, NULL);
        CHECK(tagged1 == tagged0, "untagged present consumes no pid");
        CHECK(pi.pNext == NULL, "untagged → chain untouched");
    }

    // 8) 间隔读数：相邻帧真值差进统计窗；refresh 上报
    {
        double p50 = 0, p95 = 0, mx = 0, rf = 0;
        nvn_vk_dt_stats(NULL, NULL, NULL, &p50, &p95, &mx, &rf);
        CHECK(rf > 16000000.0 && rf < 17000000.0, "refresh cycle reported in ns");
        // 已知相邻帧间隔：f1→f2 = 20ms, f2→f3 = 10ms, f3→f4 = 10ms, f4→f6 不相邻(5丢过又补：4→5=10,5→6=10)
        CHECK(mx > 19.0 && mx < 21.0, "gap max from real shown deltas (20ms pair)");
        CHECK(p50 > 9.0 && p50 <= 20.0, "gap p50 within measured range");
    }

    // 9) 环覆：>DT_CAP 后旧帧时刻安全 0 而非错配
    {
        nvn_vk_dt_test_reset();
        nvn_vk_dt_test_gate(1);
        nvn_vk_dt_attach(DEV, 1, fake_past, fake_refresh);
        fq_n = 0;
        VkPresentInfoKHR pi = mk_pi();
        VkPresentTimesInfoGOOGLE ti; VkPresentTimeGOOGLE t;
        for (uint64_t f = 1; f <= 300; f++) {
            fake_present(f, &pi, &ti, &t);
            fq_push(t.presentID, 1000000000ull + f * 16000000ull);
            nvn_vk_dt_poll(DEV, SC);
        }
        CHECK(nvn_vk_dt_shown(1) == 0, "wrapped ring: oldest frame evicted, not mismatched");
        CHECK(nvn_vk_dt_shown(300) > 0, "wrapped ring: newest frame still resolves");
        // 300 帧过 256 槽：被逐出的恰是 ≤44；帧 45 的槽位 tag 仍匹配（不被错配成帧 301）。
        CHECK(nvn_vk_dt_shown(44) == 0 && nvn_vk_dt_shown(45) > 0, "eviction boundary exact: 44 out, 45 in");
    }

    // 10) detach 复位：时刻账清零
    nvn_vk_dt_detach();
    CHECK(!nvn_vk_dt_active(), "detach → inactive");
    CHECK(nvn_vk_dt_shown(300) == 0, "detach clears shown book");

    if (!fails) printf("nvn_vk_dt: passed\n");
    return fails ? 1 : 0;
}
