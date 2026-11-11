// Host regression: fd_fence_wait 把 nvn_vk_first_draw.c 里每一次真等 fence 记进
// frame_log + frame_tsv 双账（每帧 drain 次数+时长），且只在门开时计时记账。
// 正例：门开 → 等待经替身返回后两本账各收一笔 ns；门关 → 两本账都静默；
//       超时/失败结果也照记（被阻塞的时长是事实）；
// 负例：不定义钩子时（NO_HOOK_STUBS，弱引用落空）等待照常、不崩、不记账。
#include "../native/nvn/nvn_vk_first_draw.c"
#include <assert.h>
#include <time.h>
static int waits, wait_rc = VK_SUCCESS, wait_sleep_ns;
static int log_n, tsv_n, gate_log, gate_tsv;
static uint64_t log_ns, tsv_ns;
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice d, uint32_t n, const VkFence *f,
                                             VkBool32 all, uint64_t t) {
    (void)d; (void)n; (void)f; (void)all; (void)t; waits++;
    if (wait_sleep_ns > 0) { struct timespec ts = {0, wait_sleep_ns}; nanosleep(&ts, NULL); }
    return wait_rc;
}
#ifndef NO_HOOK_STUBS
int nvn_vk_frame_log_on(void) { return gate_log; }
void nvn_vk_frame_log_fence_site(uint64_t ns, unsigned site) { (void)site; log_n++; log_ns += ns; }
void nvn_vk_frame_log_fence_context(uint64_t ns, unsigned site, unsigned kind, unsigned reason) { (void)site; (void)kind; (void)reason; log_n++; log_ns += ns; }
int nvn_vk_frame_tsv_on(void) { return gate_tsv; }
void nvn_vk_frame_tsv_fence_wait(uint64_t ns) { tsv_n++; tsv_ns += ns; }
#endif
int __android_log_print(int p, const char *t, const char *f, ...) { (void)p; (void)t; (void)f; return 0; }
int __system_property_get(const char *name, char *value) { (void)name; if (value) value[0] = 0; return 0; }

static void scenario(int kase) {
    static struct s2a_vk_device dev;
    dev.device = (VkDevice)(uintptr_t)1;
    VkFence fence = (VkFence)(uintptr_t)2;
    waits = log_n = tsv_n = 0; log_ns = tsv_ns = 0;
    gate_log = gate_tsv = 0; wait_rc = VK_SUCCESS; wait_sleep_ns = 0;
    if (kase == 0) {         // 双门开 + 成功：两本账各一笔，时长 > 0（替身睡 1ms，阈值留 10x 余量）
        gate_log = gate_tsv = 1; wait_sleep_ns = 1000000;
        assert(fd_fence_wait(&dev, fence, 5000000000ull) == VK_SUCCESS);
        assert(waits == 1 && log_n == 1 && tsv_n == 1 && log_ns >= 100000 && tsv_ns >= 100000);
    } else if (kase == 1) {  // 双门关：等待照走，两本账都静默（门开才计时记账的分支判据）
        VkResult r = fd_fence_wait(&dev, fence, 5000000000ull);
        assert(r == VK_SUCCESS && waits == 1 && log_n == 0 && tsv_n == 0);
    } else if (kase == 2) {  // 超时也照记：返回值原样透传，两本账仍各收一笔（与 submit() 同口径）
        gate_log = gate_tsv = 1; wait_rc = VK_TIMEOUT;
        assert(fd_fence_wait(&dev, fence, 50000000ull) == VK_TIMEOUT);
        assert(waits == 1 && log_n == 1 && tsv_n == 1);
    } else if (kase == 4) {  // 只开 tsv：只记 tsv 账（两门独立判定）
        gate_tsv = 1;
        assert(fd_fence_wait(&dev, fence, 5000000000ull) == VK_SUCCESS);
        assert(waits == 1 && log_n == 0 && tsv_n == 1);
    }
#ifdef NO_HOOK_STUBS
    else if (kase == 3) {    // 钩子缺席（弱引用落空）：等待照常返回，不崩、不记账
        assert(fd_fence_wait(&dev, fence, 1000ull) == VK_SUCCESS);
        assert(waits == 1);
    }
#endif
    else assert(0);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    scenario(atoi(argv[1]));
    puts("fd_fence_wait ledger passed");
}
