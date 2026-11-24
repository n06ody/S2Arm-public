// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
//
// 两级回放队列（android/native/nvn/nvn_vk_dq.inc）的宿主用例：直接包含该文件，用真队列 + 真录制线程，
// 驱动的 vkCmd* 换成「记录桩」（只记下收到的调用序列的滚动哈希与条数），其余 Vulkan 入口换成空桩。
// 期望都不靠被测实现算：每个生产者线程按固定种子的伪随机序列发调用，同时自己累计「发出的序列」的哈希；
// 录制线程（或开关关着时的直调）的桩累计「收到的序列」的哈希；清空队列之后两边必须逐 cb 相等（条数、次序、每个参数、数组内容）。
//
//   C1 多生产者序列：4 个生产者各占一个 cb，混合五种调用（含大小不一的数组，以及带不认识的 pNext、必须直调的屏障），
//      总量超过队列容量 2 倍（环回绕），同时另一条线程不停地清空（vkEndCommandBuffer）。
//   C2 跨线程清空：线程 P 发 K 条调用后用 release 通知线程 W，W 在 acquire 到通知后清空队列；
//      清空返回时 P 通知之前发的每一条都必须已被录制线程做完。
//   C3 背压：录制线程每条慢 1.5 微秒，单生产者发的总量超过队列容量，生产者必须等环里有地方；全部按序做完、不死锁。
//   C5 两条线程同时清空：用记录桩与真实等待握手定次序，先到的先走之后，后到的必须仍被叫醒（不能等满 1 秒的超时）。
//   C4 运行时开关：生产者在跑的时候来回切 nvk_dq（经提交处的开关读取，开 → 关 → 开），每个 cb 的序列仍然一致。
//   C6 录制线程怎么等活（nvk_dq_spin / nvk_dq_poll_us / nvk_dq_wake_kb）：
//      a 积压不到阈值时清空照样立刻等到；b 积压不到阈值不叫、过线就叫（不清空，过线的那一批也要被做掉）；
//      c 定时醒会把没人叫的积压取走；d 空闲很久之后退避封顶（再来一条也在毫秒级被取走）；
//      e 稀疏的生产者下，定时醒 + 按批叫的录制线程 CPU 远低于原来的自旋（同一台机器上两种配置各量一次，比值）；
//      f 环满：阈值够不着、录制线程睡着，生产者写满环也不死锁（环满的叫醒不受阈值限制）；
//      g 三条线程同时清空、录制线程睡着、阈值够不着：三条都立刻被叫醒。
//   C7 等待分布读数（「睡着后第一条记录从发布到被录」和「清空等待」两张直方图，读数行给 p50 / p99 / 最大）：
//      a 每轮由测试线程量墙钟，读数必须不长于它、贴近它，桶与百分位按规格重算后对照；b 清空等待进直方图；c 读数行与直方图一致且取走清零；d 百分位取整的合成用例。
//   C8 暂存填充的代拷：a 代拷新票号时水位不越过还没拷完的旧票号、录制线程跳过被代拷的；b 与正在拷的录制线程抢同一笔只拷一次、等它拷完才返回；
//      c 旁挂表复用前先代拷槽里没开始的；d 按票号等的老路径；e 两级回放关着就地拷；f 并发抢的压力（每笔恰好拷一次、返回时内容对）。
//   C1b / C1c 同 C1，但录制线程用定时醒 + 按批叫（C1b 无清空噪声：积压只靠定时醒 / 过线 / 末尾清空取走）。
// 任何一处卡住由看门狗在 TEST_TIMEOUT_S 秒后以退出码 3 收场（队列死锁不会让 CI 挂着）。
// 带一个参数时只跑那一组（如 `dq_queue_test C6`），变异负控用它直奔被测的阶段。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "nvn_plat.h"
#include "../native/direct/vk_device.h"

#ifndef TEST_TIMEOUT_S
#define TEST_TIMEOUT_S 240
#endif

// ---- 平台桩 ----
static _Atomic int g_flag_dq = 1, g_flag_spin = 200, g_flag_poll = 0, g_flag_wake_kb = 0, g_flag_gputime = 1;   // 后三个等活开关的缺省值就是队列的缺省行为；gputime 开着 = 队列里的读数计数器在计
static _Atomic int g_red_logs, g_red_expected, g_red_ok;   // g_red_ok：这一段里「清空等了超过 1 秒」的自报红是预期的（背压段收尾时环里积压着 1 秒以上的活）
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_dq")) return atomic_load(&g_flag_dq);
    if (!strcmp(name, "nvk_dq_spin")) return atomic_load(&g_flag_spin);
    if (!strcmp(name, "nvk_dq_poll_us")) return atomic_load(&g_flag_poll);
    if (!strcmp(name, "nvk_dq_wake_kb")) return atomic_load(&g_flag_wake_kb);
    if (!strcmp(name, "nvk_gputime")) return atomic_load(&g_flag_gputime);
    return dflt;
}
void nvn_plat_log(int level, const char *cls, const char *text) {
    (void)cls;
    if (level == NVN_PLAT_LOG_RED || strstr(text, "RED") || strstr(text, "【红】")) {
        if (atomic_load(&g_red_ok) && strstr(text, "a drain waits over 1 s")) { atomic_fetch_add(&g_red_expected, 1); return; }
        atomic_fetch_add(&g_red_logs, 1); fprintf(stderr, "队列自报红：%s\n", text);
    }
}

// ---- 不经队列的 Vulkan 入口：空桩（队列的同步包装会调到它们）----
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) { (void)cb; (void)bi; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer cb) { (void)cb; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *cb) { (void)d; (void)ai; (void)cb; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) { (void)d; (void)p; (void)f; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f) { (void)q; (void)n; (void)si; (void)f; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi) { (void)q; (void)pi; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue q) { (void)q; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice d) { (void)d; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) { (void)d; (void)p; (void)a; }
VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice d, VkCommandPool p, uint32_t n, const VkCommandBuffer *cb) { (void)d; (void)p; (void)n; (void)cb; }
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f) { (void)cb; (void)f; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice d, VkDescriptorPool p, uint32_t n, const VkDescriptorSet *s) { (void)d; (void)p; (void)n; (void)s; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice d, VkDescriptorPool p, VkDescriptorPoolResetFlags f) { (void)d; (void)p; (void)f; return VK_SUCCESS; }
#define STUB_DESTROY(f, T) VKAPI_ATTR void VKAPI_CALL f(VkDevice d, T o, const VkAllocationCallbacks *a) { (void)d; (void)o; (void)a; }
STUB_DESTROY(vkDestroyBuffer, VkBuffer)
STUB_DESTROY(vkDestroyImage, VkImage)
STUB_DESTROY(vkDestroyImageView, VkImageView)
STUB_DESTROY(vkDestroyBufferView, VkBufferView)
STUB_DESTROY(vkDestroyFramebuffer, VkFramebuffer)
STUB_DESTROY(vkDestroyRenderPass, VkRenderPass)
STUB_DESTROY(vkDestroyPipeline, VkPipeline)
STUB_DESTROY(vkDestroyPipelineLayout, VkPipelineLayout)
STUB_DESTROY(vkDestroyDescriptorPool, VkDescriptorPool)
STUB_DESTROY(vkDestroyQueryPool, VkQueryPool)
STUB_DESTROY(vkDestroySampler, VkSampler)
STUB_DESTROY(vkFreeMemory, VkDeviceMemory)
STUB_DESTROY(vkDestroySemaphore, VkSemaphore)
STUB_DESTROY(vkDestroyFence, VkFence)
STUB_DESTROY(vkDestroyDescriptorSetLayout, VkDescriptorSetLayout)
STUB_DESTROY(vkDestroyShaderModule, VkShaderModule)

// ---- 录制这一侧：记录桩（队列开着时在录制线程上跑，关着时在生产者线程上跑）----
enum { MAXCB = 8, CB_BASE = 0x1000 };
static uint64_t g_hash[MAXCB], g_cnt[MAXCB];   // 同一个 cb 同一时刻只有一侧在写（直调前先清空）；主线程在清空之后读（清空返回对录制线程的 release 写有 acquire）
static _Atomic uint64_t g_executed[MAXCB];      // 每做完一条加一（release）
static _Atomic int g_unexpected;                // 桩被调到没登记的调用
static _Atomic int g_c5_entered, g_c5_waiters, g_c5_a_done, g_c5_remaining;
static int c5_drainer_count(void);
static _Thread_local int g_c5_role;
static int g_slow_ns;                           // 录制每条额外耗时（制造背压）；改它只在队列清空之后

static uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); h *= 0xff51afd7ed558ccdull; return h ^ (h >> 29); }
static uint64_t fold(uint64_t h, uint64_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t d) { return mix(mix(mix(mix(mix(h, op), a), b), c), d); }
static uint64_t hash_bytes(const void *p, size_t n) { const uint8_t *s = p; uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= s[i]; h *= 1099511628211ull; } return h; }
static unsigned cb_index(VkCommandBuffer cb) { return (unsigned)((uintptr_t)cb - CB_BASE); }
static VkCommandBuffer cb_of(unsigned i) { return (VkCommandBuffer)(uintptr_t)(CB_BASE + i); }
static void spin_ns(int ns) { struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a); do { clock_gettime(CLOCK_MONOTONIC, &b); } while ((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec) < ns); }

static void rec_note(VkCommandBuffer cb, uint64_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    unsigned i = cb_index(cb);
    if (i >= MAXCB) { atomic_store(&g_unexpected, 1); return; }
    if (g_slow_ns) spin_ns(g_slow_ns);   // 先慢后记：清空若在这一条做完之前就返回，调用方立刻读到的条数 / 已做完数就差一条
    g_hash[i] = fold(g_hash[i], op, a, b, c, d); g_cnt[i]++;
    atomic_fetch_add_explicit(&g_executed[i], 1, memory_order_release);
}
static _Atomic int g_c8_block, g_c8_blocked;   // C8c：录制线程做到 cb 7 上的绘制时停住（不认领任何填充），直到用例清掉 g_c8_block
static VKAPI_ATTR void VKAPI_CALL rec_draw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    if (cb_index(cb) == 7 && atomic_load(&g_c8_block)) { atomic_store(&g_c8_blocked, 1); while (atomic_load(&g_c8_block)) sched_yield(); atomic_store(&g_c8_blocked, 0); }
    rec_note(cb, 1, vc, ic, fv, fi); }
static VKAPI_ATTR void VKAPI_CALL rec_bindpipe(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) { rec_note(cb, 2, (uint64_t)bp, (uint64_t)(uintptr_t)p, 0, 0); }
static VKAPI_ATTR void VKAPI_CALL rec_viewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *vp) { rec_note(cb, 3, first, n, hash_bytes(vp, n * sizeof *vp), 0); }
static VKAPI_ATTR void VKAPI_CALL rec_barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep, uint32_t nm, const VkMemoryBarrier *m,
                                              uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    (void)b;
    rec_note(cb, 4, ((uint64_t)src << 32) | dst, ((uint64_t)nm << 40) | ((uint64_t)nb << 20) | ni, hash_bytes(im, ni * sizeof *im), dep ^ (nm ? hash_bytes(m, nm * sizeof *m) : 0));
}
static VKAPI_ATTR void VKAPI_CALL rec_dispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    if (z == 0x5101u) {
        atomic_store(&g_c5_entered, 1);
        while (atomic_load(&g_c5_waiters) != 3) sched_yield();
    }   // C5 的两条握手记录：z 取不到别处发的 dispatch 会用的值（那些 z ≤ 15）
    else if (z == 0x5102u) {
        while (!atomic_load(&g_c5_a_done)) sched_yield();
        // R2尚未完成，B不能退出；A已返回，所以真实在等者必须恰为1。
        atomic_store(&g_c5_remaining, c5_drainer_count());
    }
    rec_note(cb, 5, x, y, z, 0);
}
static void rec_nop(void) { atomic_store(&g_unexpected, 1); }
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice d, const char *name) {
    (void)d;
    if (!strcmp(name, "vkCmdDraw")) return (PFN_vkVoidFunction)rec_draw;
    if (!strcmp(name, "vkCmdBindPipeline")) return (PFN_vkVoidFunction)rec_bindpipe;
    if (!strcmp(name, "vkCmdSetViewport")) return (PFN_vkVoidFunction)rec_viewport;
    if (!strcmp(name, "vkCmdPipelineBarrier")) return (PFN_vkVoidFunction)rec_barrier;
    if (!strcmp(name, "vkCmdDispatch")) return (PFN_vkVoidFunction)rec_dispatch;
    return (PFN_vkVoidFunction)rec_nop;
}

// ---- 被测：队列本体（包含之后，vkCmd* / vkEndCommandBuffer 等名字走它的包装）----
// 只观察真实条件等待的进入，不伪造返回或唤醒，也不读取被测计数作期望。
static int c5_timedwait(pthread_cond_t *cond, pthread_mutex_t *mu, const struct timespec *dl) {
    if (g_c5_role) atomic_fetch_or(&g_c5_waiters, g_c5_role);
    return pthread_cond_timedwait(cond, mu, dl);
}
#define pthread_cond_timedwait c5_timedwait
// C8 的钩子：录制线程认领一笔填充（状态 0 → 1）之后、开拷之前。记下它拷了哪一笔（按票号计数）；票号等于 g_c8_pause 时停在这里，
// 直到用例放行——这时这一笔的状态是「录制线程正在拷」，录制线程后面的记录也都没做
enum { C8_MAXT = 1 << 16 };
static _Atomic uint32_t g_c8_rec_copies[C8_MAXT];   // 下标 = 票号 % C8_MAXT（单个用例里的票号跨度远小于它）
static _Atomic uint64_t g_c8_pause;
static _Atomic int g_c8_paused, g_c8_release;
static void c8_rec_hook(uint64_t seq) {
    atomic_fetch_add(&g_c8_rec_copies[seq % C8_MAXT], 1);
    if (seq && seq == atomic_load(&g_c8_pause)) { atomic_store(&g_c8_paused, 1); while (!atomic_load(&g_c8_release)) sched_yield(); atomic_store(&g_c8_paused, 0); }
}
#define DQ_FILL_REC_HOOK(a) c8_rec_hook((a)->seq)
#include "nvn_vk_dq.inc"
#undef pthread_cond_timedwait
static int c5_drainer_count(void) { return atomic_load(&g_dq_draining); }

// ---- 检查计数 ----
static int g_checks, g_bad;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_bad++; fprintf(stderr, "【红】" __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } } while (0)

// ---- 生产者：按固定种子发混合调用，同时累计「发出的序列」的哈希 ----
typedef struct { unsigned cb; uint64_t seed, n, hash; int only_dispatch; } prod_t;
static uint64_t rnd(uint64_t *s) { uint64_t x = *s; x ^= x >> 12; x ^= x << 25; x ^= x >> 27; *s = x; return x * 0x2545F4914F6CDD1Dull; }
static void issue_one(prod_t *p) {
    VkCommandBuffer cb = cb_of(p->cb);
    uint64_t r = rnd(&p->seed); unsigned k = p->only_dispatch ? 99 : (unsigned)(r % 100);
    if (k < 45) {
        uint32_t vc = (uint32_t)(r >> 8), ic = (uint32_t)(r >> 40) & 0xff, fv = (uint32_t)(r >> 16) & 0xffff, fi = (uint32_t)(r >> 3) & 0xfff;
        vkCmdDraw(cb, vc, ic, fv, fi);
        p->hash = fold(p->hash, 1, vc, ic, fv, fi);
    } else if (k < 65) {
        VkPipelineBindPoint bp = (VkPipelineBindPoint)(r & 1); VkPipeline pl = (VkPipeline)(uintptr_t)(rnd(&p->seed) | 1);
        vkCmdBindPipeline(cb, bp, pl);
        p->hash = fold(p->hash, 2, (uint64_t)bp, (uint64_t)(uintptr_t)pl, 0, 0);
    } else if (k < 80) {
        uint32_t n = 1 + (uint32_t)(r >> 7) % 8, first = (uint32_t)(r >> 20) & 7; VkViewport vp[8];
        for (uint32_t i = 0; i < n; i++) { vp[i].x = (float)(int32_t)rnd(&p->seed); vp[i].y = (float)(r & 0xfff); vp[i].width = (float)(i + 1); vp[i].height = (float)(r >> 50); vp[i].minDepth = 0.f; vp[i].maxDepth = 1.f; }
        vkCmdSetViewport(cb, first, n, vp);
        p->hash = fold(p->hash, 3, first, n, hash_bytes(vp, n * sizeof *vp), 0);
    } else if (k < 90) {
        uint32_t ni = (uint32_t)(r >> 9) % 41; VkImageMemoryBarrier im[40];
        memset(im, 0, sizeof im);
        for (uint32_t i = 0; i < ni; i++) { im[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; im[i].srcAccessMask = (VkAccessFlags)rnd(&p->seed); im[i].dstAccessMask = (VkAccessFlags)(r >> 5); im[i].oldLayout = (VkImageLayout)(i & 7);
            im[i].newLayout = (VkImageLayout)((r >> 13) & 7); im[i].image = (VkImage)(uintptr_t)(rnd(&p->seed) | 1); im[i].subresourceRange.levelCount = i + 1; }
        VkPipelineStageFlags src = (VkPipelineStageFlags)(r >> 11) & 0xffff, dst = (VkPipelineStageFlags)(r >> 29) & 0xffff; VkDependencyFlags dep = (VkDependencyFlags)(r & 3);
        uint32_t nm = 0; VkMemoryBarrier mb; memset(&mb, 0, sizeof mb);
        if (((r >> 17) & 7) == 0) {   // 八分之一的屏障带不认识的 pNext：队列不拷这种调用，得等队列清空后直调——次序不能乱
            static const int dummy_next = 0; mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb.pNext = &dummy_next; mb.srcAccessMask = (VkAccessFlags)(r >> 3); mb.dstAccessMask = (VkAccessFlags)(r >> 21); nm = 1;
        }
        vkCmdPipelineBarrier(cb, src, dst, dep, nm, nm ? &mb : NULL, 0, NULL, ni, ni ? im : NULL);
        p->hash = fold(p->hash, 4, ((uint64_t)src << 32) | dst, ((uint64_t)nm << 40) | ni, hash_bytes(im, ni * sizeof *im), dep ^ (nm ? hash_bytes(&mb, sizeof mb) : 0));
    } else {
        uint32_t x = (uint32_t)(r >> 8) & 0xffff, y = (uint32_t)(r >> 24) & 0xff, z = (uint32_t)(r >> 40) & 0xf;
        vkCmdDispatch(cb, x, y, z);
        p->hash = fold(p->hash, 5, x, y, z, 0);
    }
    p->n++;
}
static void *producer_main(void *arg) { prod_t *p = arg; uint64_t target = p->n; p->n = 0; for (uint64_t i = 0; i < target; i++) issue_one(p); return NULL; }   // 进来时 n = 目标条数，出去时 n = 实际发出的条数

// ---- 看门狗 ----
static void *watchdog_main(void *arg) { (void)arg; sleep(TEST_TIMEOUT_S); fprintf(stderr, "【红】超时：用例 %d 秒内没跑完（队列卡住 / 死锁？）\n", TEST_TIMEOUT_S); fflush(stderr); _exit(3); }

// ---- 公用：切开关（经提交处读取：每 64 次提交读一次，64 次里必有一次），并确认切到了 ----
static void set_dq(int on) {
    atomic_store(&g_flag_dq, on);
    for (int i = 0; i < 64; i++) vkQueueSubmit(NULL, 0, NULL, VK_NULL_HANDLE);
    CHECK((g_dq_on != 0) == (on != 0), "两级回放开关没切过去：期望 %d → %d", on, g_dq_on);
}
// 切录制线程的等活开关（同样经提交处的读取），并确认队列里的值跟上了
static void set_wait(int spin, int poll_us, int wake_kb) {
    atomic_store(&g_flag_spin, spin); atomic_store(&g_flag_poll, poll_us); atomic_store(&g_flag_wake_kb, wake_kb);
    for (int i = 0; i < 64; i++) vkQueueSubmit(NULL, 0, NULL, VK_NULL_HANDLE);
    CHECK(atomic_load(&g_dq_spin) == spin && atomic_load(&g_dq_poll_us) == poll_us && (long)(atomic_load(&g_dq_wake_bytes) >> 10) == wake_kb,
          "等活开关没切过去：期望 spin=%d poll_us=%d wake_kb=%d", spin, poll_us, wake_kb);
}
static void reset_sinks(void) { vkDeviceWaitIdle(NULL); memset(g_hash, 0, sizeof g_hash); memset(g_cnt, 0, sizeof g_cnt); for (int i = 0; i < MAXCB; i++) atomic_store(&g_executed[i], 0); }
static void phase(const char *name) { printf("阶段开始：%s\n", name); fflush(stdout); }   // 崩溃 / 超时时，日志里最后一行告诉你卡在哪个阶段

// ---- C1 / C4：多生产者序列 ----
static _Atomic int g_noise_run;
static void *noise_main(void *arg) {   // 另一条线程不停地清空：清空与生产者的写入互相穿插
    (void)arg; unsigned k = 0;
    while (atomic_load(&g_noise_run)) { vkEndCommandBuffer(cb_of(MAXCB - 1)); if (!(++k & 7)) sched_yield(); }
    return NULL;
}
static _Atomic int g_toggle_run;
static void *toggler_main(void *arg) {   // C4：生产者在跑，来回切开关
    (void)arg; unsigned k = 0;
    while (atomic_load(&g_toggle_run)) { set_dq(k++ & 1 ? 1 : 0); usleep(300); }
    return NULL;
}
static void run_seq_phase(const char *name, int nprod, uint64_t per_prod, int noise, int toggle) {
    phase(name);
    set_dq(1); reset_sinks();
    uint64_t w0 = g_dq_w, recs0 = nvn_vk_dbg_dq_recs();
    prod_t pr[MAXCB - 1]; pthread_t th[MAXCB - 1], nz = 0, tg = 0;
    for (int i = 0; i < nprod; i++) { pr[i] = (prod_t){ .cb = (unsigned)i, .seed = 0x1234567ull * (uint64_t)(i + 1) + 99, .n = per_prod, .hash = 0, .only_dispatch = 0 }; }
    if (noise) { atomic_store(&g_noise_run, 1); pthread_create(&nz, NULL, noise_main, NULL); }
    if (toggle) { atomic_store(&g_toggle_run, 1); pthread_create(&tg, NULL, toggler_main, NULL); }
    for (int i = 0; i < nprod; i++) pthread_create(&th[i], NULL, producer_main, &pr[i]);
    for (int i = 0; i < nprod; i++) pthread_join(th[i], NULL);
    if (noise) { atomic_store(&g_noise_run, 0); pthread_join(nz, NULL); }
    if (toggle) { atomic_store(&g_toggle_run, 0); pthread_join(tg, NULL); }
    vkQueueWaitIdle(NULL);   // 清空
    for (int i = 0; i < nprod; i++) {
        CHECK(g_cnt[i] == pr[i].n, "%s：cb %d 条数不符：发 %llu → 收 %llu", name, i, (unsigned long long)pr[i].n, (unsigned long long)g_cnt[i]);
        CHECK(g_hash[i] == pr[i].hash, "%s：cb %d 序列哈希不符（次序 / 参数 / 数组内容有出入）", name, i);
    }
    CHECK(!atomic_load(&g_unexpected), "%s：录制做了没登记的调用", name);
    CHECK(nvn_vk_dbg_dq_recs() > recs0, "%s：没有一条调用经过队列", name);
    double mb = (double)(g_dq_w - w0) / 1048576.0;
    printf("  %-38s %d 个生产者共 %llu 条，写入 %.1f MB%s\n", name, nprod, (unsigned long long)(per_prod * (uint64_t)nprod), mb, mb > 2.0 * (double)dq_RING / 1048576.0 ? "（环已回绕 2 次以上）" : "");
}

// ---- C2：跨线程清空 ----
static _Atomic uint64_t g_issued0;
static _Atomic int g_round_flag, g_round_ack;
static int g_rounds;
static void *p_main(void *arg) {
    (void)arg; prod_t p = { .cb = 0, .seed = 0xabcdef12345ull, .n = 0, .hash = 0, .only_dispatch = 0 }; uint64_t total = 0, s = 0x777ull;
    for (int r = 1; r <= g_rounds; r++) {
        uint32_t k = 1 + (uint32_t)(rnd(&s) % ((r & 1) ? 12 : 300));   // 奇数轮条数少，偶数轮条数多
        for (uint32_t i = 0; i < k; i++) issue_one(&p);
        total += k;
        atomic_store_explicit(&g_issued0, total, memory_order_release);
        atomic_store_explicit(&g_round_flag, r, memory_order_release);
        while (atomic_load_explicit(&g_round_ack, memory_order_acquire) != r) sched_yield();
    }
    return NULL;
}
static void run_drain_phase(int rounds) {
    phase("C2 跨线程清空");
    set_dq(1); reset_sinks(); g_slow_ns = 2000; g_rounds = rounds; atomic_store(&g_round_flag, 0); atomic_store(&g_round_ack, 0); atomic_store(&g_issued0, 0);
    pthread_t t; pthread_create(&t, NULL, p_main, NULL);
    int viol = 0;
    for (int r = 1; r <= rounds; r++) {
        while (atomic_load_explicit(&g_round_flag, memory_order_acquire) != r) sched_yield();
        uint64_t issued = atomic_load_explicit(&g_issued0, memory_order_acquire);
        vkEndCommandBuffer(cb_of(1));   // 别的线程清空：P 通知之前发的调用都得做完
        uint64_t ex = atomic_load_explicit(&g_executed[0], memory_order_acquire);
        if (ex < issued) viol++;
        atomic_store_explicit(&g_round_ack, r, memory_order_release);
    }
    pthread_join(t, NULL);
    vkQueueWaitIdle(NULL); g_slow_ns = 0;
    CHECK(viol == 0, "C2：%d / %d 轮清空返回时仍有通知之前发的调用没做完", viol, rounds);
    printf("  %-38s %d 轮，清空后欠账的轮数 %d\n", "C2 跨线程清空", rounds, viol);
}

// ---- C5：两条线程同时清空，先到的先走 ----
// 回归：清空线程的「在等」标记曾是个标志位——先走的那条把它抹掉，后到的还在等，录制线程做完最后一条见标记是 0，不再叫醒它，它只能等满 1 秒的超时。
// R1进入记录桩后才发布R2，保证是两次发布快照。A进入真实条件等待后再发布R2；
// R1等A/B都进入真实等待再完成；R2等A实际返回再完成，确保撤改标志位时漏掉B的唤醒。
// 所有握手受整局看门狗保护；原600ms判据保持。
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void *c5_a_main(void *arg) { (void)arg; g_c5_role = 1; vkEndCommandBuffer(cb_of(1)); atomic_store(&g_c5_a_done, 1); return NULL; }
static void run_two_drainers_phase(void) {
    phase("C5 两条线程同时清空");
    set_dq(1); reset_sinks();
    atomic_store(&g_c5_entered, 0); atomic_store(&g_c5_waiters, 0); atomic_store(&g_c5_a_done, 0); atomic_store(&g_c5_remaining, -1);
    vkCmdDispatch(cb_of(0), 1, 1, 0x5101);   // R1
    while (!atomic_load(&g_c5_entered)) sched_yield();
    pthread_t ta; pthread_create(&ta, NULL, c5_a_main, NULL);   // A：快照在 R1 之后
    while (!(atomic_load(&g_c5_waiters) & 1)) sched_yield();
    vkCmdDispatch(cb_of(0), 2, 2, 0x5102);   // R2，排在 R1 后面
    uint64_t t0 = now_ns();
    g_c5_role = 2;
    vkEndCommandBuffer(cb_of(1));   // B：快照在 R2 之后
    g_c5_role = 0;
    double b_ms = (double)(now_ns() - t0) / 1e6;
    pthread_join(ta, NULL);
    CHECK(atomic_load(&g_c5_remaining) == 1, "C5：先到的清空已返回、后到的尚未完成，等待者应为1，实际%d", atomic_load(&g_c5_remaining));
    CHECK(b_ms < 600.0, "C5：后到的清空等了 %.0f 毫秒（应小于600毫秒；等满 1 秒的超时 = 先走的那条把「在等」标记抹掉了，没人叫醒它）", b_ms);
    CHECK(g_cnt[0] == 2, "C5：条数不符：发 2 → 收 %llu", (unsigned long long)g_cnt[0]);
    printf("  %-38s 后到的清空等了 %.0f 毫秒\n", "C5 两条线程同时清空", b_ms);
}

// ---- C3：背压 ----
static void run_pressure_phase(uint64_t n) {
    phase("C3 背压");
    set_dq(1); reset_sinks();
    g_slow_ns = 1500;
    prod_t p = { .cb = 0, .seed = 0x5eed5eedull, .n = n, .hash = 0, .only_dispatch = 1 };
    uint64_t w0 = g_dq_w;
    producer_main(&p);   // 本线程当生产者：队列满了它就得等
    atomic_store(&g_red_ok, 1);
    vkQueueWaitIdle(NULL);   // 环里还积压着一大截：这一次清空会等很久
    atomic_store(&g_red_ok, 0);
    g_slow_ns = 0;
    CHECK(g_cnt[0] == p.n, "C3：条数不符：发 %llu → 收 %llu", (unsigned long long)p.n, (unsigned long long)g_cnt[0]);
    CHECK(g_hash[0] == p.hash, "C3：序列哈希不符");
    double mb = (double)(g_dq_w - w0) / 1048576.0;
    CHECK(mb > 1.5 * (double)dq_RING / 1048576.0, "C3：写入量 %.1f MB 没超过队列容量，没有真正压到背压", mb);
    printf("  %-38s %llu 条，写入 %.1f MB（队列 %d MB）\n", "C3 背压（录制每条慢 1.5 微秒）", (unsigned long long)n, mb, dq_RING >> 20);
}

// ---- C6：录制线程怎么等活 ----
static int wait_executed(unsigned cb, uint64_t want, int timeout_ms) {   // 等 cb 上做完的条数到 want；返回等了多少毫秒，超时返回 -1
    uint64_t t0 = now_ns();
    while (atomic_load_explicit(&g_executed[cb], memory_order_acquire) < want) {
        if ((now_ns() - t0) / 1000000ull >= (uint64_t)timeout_ms) return -1;
        usleep(200);
    }
    return (int)((now_ns() - t0) / 1000000ull);
}
static void issue_draws(unsigned cb, int n) { for (int i = 0; i < n; i++) vkCmdDraw(cb_of(cb), 3, 1, (uint32_t)i, 0); }   // 每条记录 48 字节
static uint64_t rec_cpu_ns(void) { uint64_t ns = 0; return nvn_vk_dbg_dq_thread_cpu_ns(&ns) ? 0 : ns; }
static uint64_t trickle_cpu_ns(int ms) {   // 稀疏的单生产者（每约 20 微秒一条）持续 ms 毫秒；返回这段时间里录制线程用掉的 CPU 纳秒
    reset_sinks();
    uint64_t c0 = rec_cpu_ns(), t0 = now_ns();
    while (now_ns() - t0 < (uint64_t)ms * 1000000ull) { vkCmdDraw(cb_of(0), 3, 1, 0, 0); spin_ns(20000); }
    vkEndCommandBuffer(cb_of(1));
    return rec_cpu_ns() - c0;
}
static void *drainer_main(void *arg) { const uint64_t t0 = now_ns(); vkEndCommandBuffer(cb_of(1)); *(uint64_t *)arg = now_ns() - t0; return NULL; }   // 清空一次，量它等了多久
static void run_wait_phase(void) {
    phase("C6 录制线程怎么等活");
    set_dq(1);
    // a 积压不到阈值：清空照样立刻等到
    set_wait(0, 0, 64); reset_sinks(); usleep(20000);
    issue_draws(0, 3);
    uint64_t t0 = now_ns(); vkEndCommandBuffer(cb_of(1)); double ms = (double)(now_ns() - t0) / 1e6;
    CHECK(ms < 300.0, "C6a：积压不到阈值时清空等了 %.0f 毫秒（应在毫秒级；等满 1 秒的超时 = 清空没把睡着的录制线程叫醒）", ms);
    CHECK(g_cnt[0] == 3, "C6a：条数不符：发 3 → 收 %llu", (unsigned long long)g_cnt[0]);
    // b 不过线不叫、过线就叫（只按批叫：poll_us = 0，睡着的录制线程只有生产者能叫）
    set_wait(0, 0, 4); reset_sinks(); usleep(20000);
    uint64_t wk0 = nvn_vk_dbg_dq_stat(DQS_WAKE);
    issue_draws(0, 20);   // 960 字节，不到 4 KB
    usleep(30000);
    uint64_t ex = atomic_load_explicit(&g_executed[0], memory_order_acquire);
    CHECK(ex == 0, "C6b：积压 960 字节（阈值 4 KB），不清空，30 毫秒内录制线程却做了 %llu 条——没过线就叫醒了", (unsigned long long)ex);
    CHECK(nvn_vk_dbg_dq_stat(DQS_WAKE) == wk0, "C6b：没过线却去叫醒了 %llu 次", (unsigned long long)(nvn_vk_dbg_dq_stat(DQS_WAKE) - wk0));
    issue_draws(0, 180);   // 共 200 条 = 9.6 KB，过线
    int wms = wait_executed(0, 80, 500);
    ex = atomic_load_explicit(&g_executed[0], memory_order_acquire);
    CHECK(wms >= 0, "C6b：共写了 200 条（9.6 KB，阈值 4 KB），不清空，500 毫秒内录制线程只做了 %llu 条——过线没人叫醒", (unsigned long long)ex);
    CHECK(nvn_vk_dbg_dq_stat(DQS_WAKE) > wk0, "C6b：过了线，生产者却没有去叫醒");
    vkEndCommandBuffer(cb_of(1));
    CHECK(g_cnt[0] == 200, "C6b：清空后条数不符：发 200 → 收 %llu", (unsigned long long)g_cnt[0]);
    // c 定时醒：谁也不叫（阈值 1 MB），积压靠定时醒取走
    set_wait(0, 500, 1024); reset_sinks(); usleep(20000);
    uint64_t tw0 = nvn_vk_dbg_dq_stat(DQS_TMO_WORK);
    issue_draws(0, 5);
    wms = wait_executed(0, 5, 200);
    ex = atomic_load_explicit(&g_executed[0], memory_order_acquire);
    CHECK(wms >= 0, "C6c：定时醒（起始 500 微秒）下 5 条记录 200 毫秒内没被取走（实际 %llu 条）", (unsigned long long)ex);
    CHECK(nvn_vk_dbg_dq_stat(DQS_TMO_WORK) > tw0, "C6c：记录被取走了，却不是定时醒取走的");
    // d 空闲很久之后：退避封顶，再来一条仍在毫秒级被取走（不封顶的话 400 毫秒后下一次醒要在 250 毫秒以后）
    set_wait(0, 500, 1024); reset_sinks();
    usleep(400000);
    issue_draws(0, 1);
    wms = wait_executed(0, 1, 60);
    CHECK(wms >= 0, "C6d：空闲 400 毫秒后来 1 条，60 毫秒内没被取走（退避没封顶？封顶 %d 微秒）", (int)DQ_POLL_MAX_US);
    // e 稀疏的生产者：定时醒 + 按批叫的录制线程不再空转。同一台机器上各量一次，比值（绝对值跟机器走）
    set_wait(200, 0, 0);
    uint64_t cpu_spin = trickle_cpu_ns(300);   // 原来的行为：没活先 sched_yield 自旋 200 次
    set_wait(0, 100, 4);
    uint64_t cpu_new = trickle_cpu_ns(300);
    CHECK(cpu_spin > 0 && cpu_new * 10 < cpu_spin * 6, "C6e：稀疏生产者 300 毫秒里录制线程 CPU：自旋 %.1f 毫秒、定时醒 + 按批叫 %.1f 毫秒（要求后者不到前者的 60%%）", (double)cpu_spin / 1e6, (double)cpu_new / 1e6);
    printf("  %-38s 稀疏生产者 300 毫秒：录制线程 CPU 自旋 %.1f 毫秒 → 定时醒 + 按批叫 %.1f 毫秒\n", "C6 录制线程怎么等活", (double)cpu_spin / 1e6, (double)cpu_new / 1e6);
    // f 环满：阈值设到够不着（1 GB > 队列 32 MB），睡着的录制线程只剩「环满」这一条叫醒的路。先写半个环：不叫、一条没做；再写到 1.3 倍环容量：不能死锁，做完的数一条不少
    set_wait(0, 0, 1 << 20); reset_sinks(); usleep(20000);
    const int n_half = (int)((uint64_t)dq_RING / 2 / 48), n_full = (int)((uint64_t)dq_RING * 13 / 10 / 48);   // 每条 draw 记录 48 字节
    uint64_t wk_f = nvn_vk_dbg_dq_stat(DQS_WAKE), w_f = g_dq_w;
    issue_draws(0, n_half);
    usleep(30000);
    ex = atomic_load_explicit(&g_executed[0], memory_order_acquire);
    CHECK(ex == 0, "C6f：积压 %.0f MB（阈值 1 GB）、没清空、没环满，录制线程却做了 %llu 条", (double)(g_dq_w - w_f) / 1048576.0, (unsigned long long)ex);
    CHECK(nvn_vk_dbg_dq_stat(DQS_WAKE) == wk_f, "C6f：积压不到阈值也不是环满，生产者却去叫醒了 %llu 次", (unsigned long long)(nvn_vk_dbg_dq_stat(DQS_WAKE) - wk_f));
    issue_draws(0, n_full - n_half);   // 环写满：这里要靠环满的叫醒才能继续（没有它，生产者永远等不到位置）
    CHECK(nvn_vk_dbg_dq_stat(DQS_WAKE) > wk_f, "C6f：环写满了，生产者却没有叫醒录制线程");
    vkEndCommandBuffer(cb_of(1));
    CHECK(g_cnt[0] == (uint64_t)n_full, "C6f：清空后条数不符：发 %d → 收 %llu", n_full, (unsigned long long)g_cnt[0]);
    CHECK(g_dq_w - w_f > (uint64_t)dq_RING + (uint64_t)dq_RING / 4, "C6f：写入量 %.1f MB 没超过队列容量，没有真正写满环", (double)(g_dq_w - w_f) / 1048576.0);
    // g 三条线程同时清空、录制线程睡着、阈值够不着：三条都要被叫醒，不能等满 1 秒的超时
    set_wait(0, 0, 1 << 20); reset_sinks(); usleep(20000);
    issue_draws(0, 3);
    pthread_t dth[3]; uint64_t ddur[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) pthread_create(&dth[i], NULL, drainer_main, &ddur[i]);
    for (int i = 0; i < 3; i++) pthread_join(dth[i], NULL);
    for (int i = 0; i < 3; i++) CHECK(ddur[i] < 300ull * 1000000ull, "C6g：第 %d 条清空线程等了 %.0f 毫秒（应在毫秒级；等满 1 秒的超时 = 睡着的录制线程没被叫醒）", i, (double)ddur[i] / 1e6);
    CHECK(g_cnt[0] == 3, "C6g：条数不符：发 3 → 收 %llu", (unsigned long long)g_cnt[0]);
    printf("  %-38s 环满叫醒：写了 %.1f MB（队列 %d MB）一条不少；三条线程同时清空：最慢 %.0f 微秒\n", "C6f/g 环满与并发清空（睡着 + 阈值够不着）", (double)(g_dq_w - w_f) / 1048576.0, dq_RING >> 20,
           (double)(ddur[0] > ddur[1] ? (ddur[0] > ddur[2] ? ddur[0] : ddur[2]) : (ddur[1] > ddur[2] ? ddur[1] : ddur[2])) / 1e3);
    set_wait(200, 0, 0);   // 还原缺省
}

// ---- C7：等待分布读数（定时醒 / 按批叫以后，一条记录最多等多久要看得见）----
// 期望不靠被测实现算：每一轮由测试线程自己量「发布 → 做完」的墙钟，读数里记的「睡着后第一条记录从发布到被录」必须不长于它（它还含做完这一条）、而且贴近它；
// 桶、百分位也由测试自己按规格重算后对照。
static unsigned c7_bucket(uint64_t ns) { static const uint64_t ub[8] = { 20000, 50000, 100000, 200000, 500000, 1000000, 2000000, 5000000 }; unsigned i = 0; while (i < 8 && ns >= ub[i]) i++; return i; }
static int u64_cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static uint64_t hist_total(const uint64_t *h) { uint64_t t = 0; for (int i = 0; i < DQH_N; i++) t += __atomic_load_n(&h[i], __ATOMIC_RELAXED); return t; }
static void run_latency_phase(void) {
    phase("C7 等待分布读数");
    set_dq(1);
    char line[512]; dq_lat_line(line, sizeof line);   // 先取走前面阶段留下的读数
    // a 睡着后的等活：定时醒（100 微秒起，退避封顶 2 毫秒）、阈值 1 MB（生产者不叫）——一条记录最多等一个定时醒间隔
    set_wait(0, 100, 1024);
    enum { N = 40 };
    uint64_t rec[N], wall[N], hw0[DQH_N]; int got = 0;
    for (int i = 0; i < DQH_N; i++) hw0[i] = __atomic_load_n(&g_dq_hw[i], __ATOMIC_RELAXED);
    unsigned want_bucket[DQH_N] = {0};
    for (int k = 0; k < N; k++) {
        reset_sinks(); usleep(15000);   // 空闲 15 毫秒：退避早已到封顶，录制线程睡着
        __atomic_exchange_n(&g_dq_hw_max, 0, __ATOMIC_RELAXED);   // 取走旧的最大值：这一轮只有一个样本，最大值就是它
        const uint64_t t0 = now_ns();
        issue_draws(0, 1);
        while (atomic_load_explicit(&g_executed[0], memory_order_acquire) < 1) { }   // 空转等：量的是墙钟
        wall[k] = now_ns() - t0;
        rec[k] = __atomic_exchange_n(&g_dq_hw_max, 0, __ATOMIC_RELAXED);
        if (rec[k]) { got++; want_bucket[c7_bucket(rec[k])]++; }
    }
    CHECK(got >= 36, "C7a：40 轮「空闲后来 1 条」里只有 %d 轮记到了等活延迟（每轮都应有一个样本）", got);
    uint64_t ge[N], gw[N]; int m = 0, over = 0;   // 只看有样本的轮
    for (int k = 0; k < N; k++) if (rec[k]) { ge[m] = rec[k]; gw[m] = wall[k] > rec[k] ? wall[k] - rec[k] : 0; if (rec[k] > wall[k] + 1000) over++; m++; }
    CHECK(over == 0, "C7a：%d 轮记下的等活延迟比自己量的「发布 → 做完」墙钟还长（记错了起点 / 终点）", over);
    qsort(gw, (size_t)m, sizeof gw[0], u64_cmp);
    CHECK(m == 0 || gw[m / 2] < 200000ull, "C7a：「发布 → 做完」比记下的等活延迟平均多出 %.0f 微秒（中位）——读数没有贴住真实的等待", m ? (double)gw[m / 2] / 1e3 : 0.0);
    qsort(ge, (size_t)m, sizeof ge[0], u64_cmp);
    const uint64_t emax = m ? ge[m - 1] : 0;
    CHECK(emax < 10ull * 1000000ull, "C7a：定时醒封顶 %d 微秒，一条记录却等了 %.0f 微秒", (int)DQ_POLL_MAX_US, (double)emax / 1e3);
    CHECK(emax == 0 || nvn_vk_dbg_dq_stat(DQS_TMO_WORK) > 0, "C7a：记录是定时醒取走的，却没有定时醒取走的计数");
    uint64_t dh[DQH_N]; for (int i = 0; i < DQH_N; i++) dh[i] = __atomic_load_n(&g_dq_hw[i], __ATOMIC_RELAXED) - hw0[i];
    for (int i = 0; i < DQH_N; i++) CHECK(dh[i] == want_bucket[i], "C7a：直方图第 %d 桶：测试按规格算 %u 个、实现记了 %llu 个", i, want_bucket[i], (unsigned long long)dh[i]);
    const unsigned pe50 = m ? c7_bucket(ge[(size_t)((m * 50 + 99) / 100 - 1)]) : 0, pe99 = m ? c7_bucket(ge[(size_t)((m * 99 + 99) / 100 - 1)]) : 0;
    CHECK(dq_hist_pct(dh, (uint64_t)got, 50) == pe50 && dq_hist_pct(dh, (uint64_t)got, 99) == pe99, "C7a：p50 / p99 落桶：测试算 %u / %u，实现给 %u / %u", pe50, pe99, dq_hist_pct(dh, (uint64_t)got, 50), dq_hist_pct(dh, (uint64_t)got, 99));
    // b 清空等待：回放线程干等的那一段也进直方图（慢记录 100 微秒一条 × 30 条，录制线程睡着、阈值够不着，要靠清空叫醒）
    set_wait(0, 0, 1024); reset_sinks(); usleep(20000);
    uint64_t hd0 = hist_total(g_dq_hd); __atomic_exchange_n(&g_dq_hd_max, 0, __ATOMIC_RELAXED);
    g_slow_ns = 100000;
    issue_draws(0, 30);
    vkEndCommandBuffer(cb_of(1));
    g_slow_ns = 0;
    const uint64_t dmax = __atomic_load_n(&g_dq_hd_max, __ATOMIC_RELAXED);
    CHECK(hist_total(g_dq_hd) - hd0 == 1, "C7b：做了 1 次要等的清空，清空等待直方图却多了 %llu 个样本", (unsigned long long)(hist_total(g_dq_hd) - hd0));
    CHECK(dmax >= 2500000ull && dmax < 500ull * 1000000ull, "C7b：30 条各慢 100 微秒（共 ≥ 3 毫秒）的积压，清空等待的最大值读到 %.0f 微秒", (double)dmax / 1e3);
    // c 读数行：数字和直方图一致
    const uint64_t wt = hist_total(g_dq_hw), dt = hist_total(g_dq_hd);
    dq_lat_line(line, sizeof line);
    unsigned long long lw = 0, ld = 0; const char *pw = strstr(line, "被录 "), *pd = strstr(line, "清空等待 ");
    CHECK(pw && pd && sscanf(pw + strlen("被录 "), "%llu", &lw) == 1 && sscanf(pd + strlen("清空等待 "), "%llu", &ld) == 1 && lw == wt && ld == dt && strstr(line, "p99 ") && strstr(line, "最大 "),
          "C7c：读数行与直方图对不上（直方图 %llu / %llu 个样本）：%s", (unsigned long long)wt, (unsigned long long)dt, line);
    CHECK(hist_total(g_dq_hw) == 0 && hist_total(g_dq_hd) == 0 && g_dq_hw_max == 0 && g_dq_hd_max == 0, "C7c：读数行没把直方图取走（清零）");
    // d 百分位的取整：101 个样本、99 个在第 2 桶、2 个在最后一桶——两个已经占 1.98%（> 1%），p99 必须落在最后一桶（向下取整会错落在第 2 桶）
    { uint64_t h[DQH_N] = {0}; h[2] = 99; h[DQH_N - 1] = 2;
      CHECK(dq_hist_pct(h, 101, 99) == DQH_N - 1 && dq_hist_pct(h, 101, 50) == 2, "C7d：合成直方图（99 个在 <100 桶、2 个在 ≥5000 桶）：p99 落在第 %u 桶（应为 %d）、p50 落在第 %u 桶（应为 2）", dq_hist_pct(h, 101, 99), DQH_N - 1, dq_hist_pct(h, 101, 50)); }
    printf("  %-38s 空闲后 1 条记录（定时醒 100 微秒起封顶 %d 微秒）：%d 轮有样本，中位等 %.0f 微秒，最大 %.0f 微秒；清空等待最大 %.0f 微秒\n", "C7 等待分布读数", (int)DQ_POLL_MAX_US, got, m ? (double)ge[m / 2] / 1e3 : 0.0, (double)emax / 1e3, (double)dmax / 1e3);
    set_wait(200, 0, 0);   // 还原缺省
}

// ---- C8：暂存填充的代拷（nvn_gpu_arena_fill_take → dq_fill_take；录制线程 dq_fill_run）----
// 期望全是手算的常量与「谁拷了几次」的计数，不读被测的水位来定期望（水位本身是被测量，和手算值对）。
static void c8_wait_paused(void) { while (!atomic_load(&g_c8_paused)) sched_yield(); }   // 卡住由看门狗收场
static void c8_release(void) { atomic_store(&g_c8_release, 1); while (atomic_load(&g_c8_paused)) sched_yield(); atomic_store(&g_c8_pause, 0); atomic_store(&g_c8_release, 0); }
static int all_eq(const uint8_t *p, size_t n, uint8_t v) { for (size_t i = 0; i < n; i++) if (p[i] != v) return 0; return 1; }
static _Atomic int g_c8b_released;
static void *c8b_releaser(void *arg) { (void)arg; struct timespec ts = { 0, 20 * 1000000L }; nanosleep(&ts, NULL); atomic_store(&g_c8b_released, 1); c8_release(); return NULL; }
static uint8_t g_c8_src[1100][256], g_c8_dst[1100][256];
static void run_fill_steal_phase(void) {
    vk_dq_drain();
    // C8a：录制线程停在 t1（正拷），t2、t3 排在后面。代拷 t3 → 1、内容是代拷那一刻的；水位仍是 t1 - 1（不能越过还没拷完的 t1、t2）；
    //      放行后录制线程拷 t1、t2，轮到 t3 跳过（来源已改成 0x5A，再拷就会把 0x5A 带进暂存）；清空后水位 = t3
    phase("C8a 代拷最新一笔、旧的还在排队");
    const uint64_t t0 = atomic_load(&g_fill_seq);
    for (int i = 0; i < 3; i++) { memset(g_c8_src[i], 0xA1 + i, 256); memset(g_c8_dst[i], 0, 256); }
    atomic_store(&g_c8_pause, t0 + 1);
    const uint64_t t1 = vk_dq_fill(g_c8_dst[0], g_c8_src[0], 256), t2 = vk_dq_fill(g_c8_dst[1], g_c8_src[1], 256), t3 = vk_dq_fill(g_c8_dst[2], g_c8_src[2], 256);
    CHECK(t1 == t0 + 1 && t2 == t0 + 2 && t3 == t0 + 3, "C8a：票号不是连续递增：%llu %llu %llu（上一张 %llu）", (unsigned long long)t1, (unsigned long long)t2, (unsigned long long)t3, (unsigned long long)t0);
    c8_wait_paused();
    const int r3 = vk_dq_fill_take(t3);
    CHECK(r3 == 1, "C8a：代拷还没开始的 t3 应返回 1（本线程拷），实为 %d", r3);
    CHECK(all_eq(g_c8_dst[2], 256, 0xA3), "C8a：代拷后 t3 的暂存不是来源内容 0xA3");
    memset(g_c8_src[2], 0x5A, 256);   // 核心代拷完就改来源
    const uint64_t d_mid = vk_dq_fill_wait(0);
    CHECK(d_mid == t0, "C8a：t1 正拷、t2 排队时代拷了 t3，水位应停在 %llu，实为 %llu（越过了还没拷完的旧票号）", (unsigned long long)t0, (unsigned long long)d_mid);
    CHECK(vk_dq_fill_take(t3) == 0, "C8a：t3 已代拷，再代拷应返回 0");
    c8_release();
    vk_dq_drain();
    const uint64_t d_end = vk_dq_fill_wait(0);
    CHECK(d_end == t3, "C8a：清空后水位应为 t3 = %llu，实为 %llu", (unsigned long long)t3, (unsigned long long)d_end);
    CHECK(all_eq(g_c8_dst[0], 256, 0xA1) && all_eq(g_c8_dst[1], 256, 0xA2), "C8a：录制线程拷的 t1 / t2 内容不对");
    CHECK(all_eq(g_c8_dst[2], 256, 0xA3), "C8a：t3 的暂存被改了（录制线程没跳过被代拷的记录，把改过的来源 0x5A 又拷了一遍）");
    CHECK(g_c8_rec_copies[t1 % C8_MAXT] == 1 && g_c8_rec_copies[t2 % C8_MAXT] == 1 && g_c8_rec_copies[t3 % C8_MAXT] == 0,
          "C8a：录制线程拷的次数 t1/t2/t3 应为 1/1/0，实为 %u/%u/%u", g_c8_rec_copies[t1 % C8_MAXT], g_c8_rec_copies[t2 % C8_MAXT], g_c8_rec_copies[t3 % C8_MAXT]);

    // C8b：录制线程正拷 t（停在钩子里），核心代拷同一笔：必须返回 2、只在录制线程拷完后返回、全程只拷一次
    phase("C8b 与录制线程抢同一笔");
    memset(g_c8_src[3], 0xB1, 256); memset(g_c8_dst[3], 0, 256);
    atomic_store(&g_c8_pause, atomic_load(&g_fill_seq) + 1);
    const uint64_t tb = vk_dq_fill(g_c8_dst[3], g_c8_src[3], 256);
    c8_wait_paused();
    atomic_store(&g_c8b_released, 0);
    pthread_t rl; pthread_create(&rl, NULL, c8b_releaser, NULL);
    const int rb = vk_dq_fill_take(tb);
    const int released_before = atomic_load(&g_c8b_released);
    CHECK(rb == 2, "C8b：录制线程正在拷时代拷应返回 2（等它拷完），实为 %d", rb);
    CHECK(released_before == 1, "C8b：代拷在录制线程拷完之前就返回了");
    CHECK(all_eq(g_c8_dst[3], 256, 0xB1), "C8b：代拷返回时暂存不是 0xB1");
    memset(g_c8_src[3], 0x00, 256);
    pthread_join(rl, NULL);
    vk_dq_drain();
    CHECK(all_eq(g_c8_dst[3], 256, 0xB1) && g_c8_rec_copies[tb % C8_MAXT] == 1, "C8b：录制线程拷了 %u 次（应 1 次）或暂存被改", g_c8_rec_copies[tb % C8_MAXT]);
    CHECK(vk_dq_fill_wait(0) == tb, "C8b：清空后水位应为 %llu", (unsigned long long)tb);

    // C8c：旁挂表复用。录制线程停在 cb 7 的一条绘制上（一笔填充都没认领），再交 FILL_TAB + 5 笔：最后 5 笔占的槽里是最早的 5 笔（还没开始），
    //      必须先代拷它们再复用——录制线程还停着时这 5 笔已拷好、第 6 笔还没拷；放行后全部内容对，最早 5 笔录制线程拷 0 次、其余 1 次
    phase("C8c 旁挂表满了复用槽");
    enum { C8C_N = FILL_TAB + 5 };
    static uint8_t src16[C8C_N][16], dst16[C8C_N][16];
    for (int i = 0; i < C8C_N; i++) { memset(src16[i], (uint8_t)(i * 7 + 1), 16); memset(dst16[i], 0, 16); }
    atomic_store(&g_c8_block, 1);
    vkCmdDraw(cb_of(7), 3, 1, 0, 0);
    while (!atomic_load(&g_c8_blocked)) sched_yield();
    const uint64_t tp = atomic_load(&g_fill_seq) + 1;
    for (int i = 0; i < C8C_N; i++) { const uint64_t t = vk_dq_fill(dst16[i], src16[i], 16); if (t != tp + (uint64_t)i) { CHECK(0, "C8c：票号不连续"); break; } }
    int stolen_early = 0; for (int i = 0; i < 5; i++) stolen_early += all_eq(dst16[i], 16, (uint8_t)(i * 7 + 1));
    CHECK(stolen_early == 5 && all_eq(dst16[5], 16, 0), "C8c：复用槽之前应先代拷槽里没开始的 5 笔（录制线程还停着时已拷 %d 笔，应 5），第 6 笔应还没拷", stolen_early);
    for (int i = 0; i < 5; i++) memset(src16[i], 0xEE, 16);   // 已代拷：改来源不该再进暂存
    atomic_store(&g_c8_block, 0);
    vk_dq_drain();
    int bad = 0, extra = 0;
    for (int i = 0; i < C8C_N; i++) {
        if (!all_eq(dst16[i], 16, (uint8_t)(i * 7 + 1))) bad++;
        const uint32_t rc = g_c8_rec_copies[(tp + (uint64_t)i) % C8_MAXT], want = i < 5 ? 0 : 1;
        if (rc != want) extra++;
    }
    CHECK(bad == 0, "C8c：%d 笔暂存内容不对", bad);
    CHECK(extra == 0, "C8c：%d 笔录制线程拷的次数不对（被代拷的应 0 次、其余 1 次）", extra);
    CHECK(vk_dq_fill_wait(0) == tp + C8C_N - 1, "C8c：清空后水位不对");

    // C8d：等待路径不变（nvk_ub_fill_steal = 0 时核心走它）：交 3 笔、等最后一笔，返回时三笔都拷完、水位 ≥ 最后一笔
    phase("C8d 关代拷：按票号等");
    for (int i = 0; i < 3; i++) { memset(g_c8_src[10 + i], 0xC1 + i, 256); memset(g_c8_dst[10 + i], 0, 256); }
    uint64_t tw = 0; for (int i = 0; i < 3; i++) tw = vk_dq_fill(g_c8_dst[10 + i], g_c8_src[10 + i], 256);
    const uint64_t dw = vk_dq_fill_wait(tw);
    CHECK(dw >= tw && all_eq(g_c8_dst[10], 256, 0xC1) && all_eq(g_c8_dst[11], 256, 0xC2) && all_eq(g_c8_dst[12], 256, 0xC3), "C8d：等到票号 %llu 返回时三笔没拷完（水位 %llu）", (unsigned long long)tw, (unsigned long long)dw);

    // C8e：两级回放关着：直接拷、返回 0，代拷 0 号也返回 0
    phase("C8e 两级回放关着");
    set_dq(0);
    memset(g_c8_src[20], 0xD1, 256); memset(g_c8_dst[20], 0, 256);
    CHECK(vk_dq_fill(g_c8_dst[20], g_c8_src[20], 256) == 0 && all_eq(g_c8_dst[20], 256, 0xD1) && vk_dq_fill_take(0) == 0, "C8e：关着时应就地拷、返回 0");
    set_dq(1);

    // C8f：并发抢：录制线程自由跑，核心每交一笔就立刻代拷它、随即改来源。每笔必须恰好拷一次（录制线程 + 核心），代拷返回时内容对
    phase("C8f 并发抢同一笔（压力）");
    enum { C8F_N = 30000, C8F_K = 1024 };
    static uint8_t prod_copied[C8F_N];
    memset(prod_copied, 0, sizeof prod_copied);
    vk_dq_drain();
    const uint64_t tf0 = atomic_load(&g_fill_seq) + 1;
    int wrong = 0, n1 = 0, n2 = 0, n0 = 0; uint64_t rs = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < C8F_N; i++) {
        const int k = i % C8F_K; const uint8_t v = (uint8_t)(i * 13 + 5);
        memset(g_c8_src[k], v, 256);
        const uint64_t t = vk_dq_fill(g_c8_dst[k], g_c8_src[k], 256);
        if (t != tf0 + (uint64_t)i) { CHECK(0, "C8f：票号不连续"); break; }
        for (int sp = (int)(rnd(&rs) % 64); sp > 0; sp--) __asm__ volatile("" ::: "memory");   // 抖一抖，让录制线程有时先到、有时后到
        const int r = vk_dq_fill_take(t);
        if (r == 1) { prod_copied[i] = 1; n1++; } else if (r == 2) n2++; else n0++;
        if (!all_eq(g_c8_dst[k], 256, v)) wrong++;
        memset(g_c8_src[k], (uint8_t)~v, 256);   // 核心改来源
    }
    vk_dq_drain();
    int dup = 0, miss = 0;
    for (int i = 0; i < C8F_N; i++) { const uint32_t c = g_c8_rec_copies[(tf0 + (uint64_t)i) % C8_MAXT] + prod_copied[i]; if (c > 1) dup++; if (c < 1) miss++; }
    CHECK(wrong == 0, "C8f：%d 笔代拷返回时暂存内容不对", wrong);
    CHECK(dup == 0 && miss == 0, "C8f：%d 笔拷了不止一次、%d 笔一次都没拷", dup, miss);
    CHECK(vk_dq_fill_wait(0) == tf0 + C8F_N - 1, "C8f：清空后水位不对");
    printf("  %-38s 代拷 %d 笔、撞上录制线程正在拷 %d 次、录制线程已拷完 %d 次（共 %d 笔，每笔恰好拷一次）\n", "C8 暂存填充代拷", n1, n2, n0, C8F_N);
}

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : NULL;
#define WANT(id) (!only || !strcmp(only, id))
    pthread_t wd; pthread_create(&wd, NULL, watchdog_main, NULL); pthread_detach(wd);
    vk_dq_init((VkDevice)(uintptr_t)0x1);
    CHECK(g_dq_ring != NULL && g_dq_on, "队列没起来（环内存 / 录制线程）");
    if (!g_dq_ring) { printf("dq_queue_test：队列没起来\n"); return 1; }
    if (argc == 2 && !strcmp(argv[1], "--two-drainers-only")) {
        run_two_drainers_phase();
        CHECK(atomic_load(&g_red_logs) == 0, "队列自己报过红");
        return g_bad ? 1 : 0;
    }
    printf("两级回放队列宿主用例（真队列 + 真录制线程 + 记录桩）\n");
    if (WANT("C1")) run_seq_phase("C1 多生产者序列（另有线程不停清空）", 4, 160000, 1, 0);
    if (WANT("C2")) run_drain_phase(3000);
    if (WANT("C5")) run_two_drainers_phase();
    if (WANT("C3")) run_pressure_phase(1200000);
    if (WANT("C4")) run_seq_phase("C4 运行时来回切 nvk_dq", 3, 200000, 0, 1);
    if (WANT("C6")) run_wait_phase();
    if (WANT("C7")) run_latency_phase();
    if (WANT("C1b")) { set_wait(0, 100, 4); run_seq_phase("C1b 多生产者序列（定时醒 + 按批叫，无清空噪声）", 4, 160000, 0, 0); set_wait(200, 0, 0); }
    if (WANT("C8")) run_fill_steal_phase();
    if (WANT("C1c")) { set_wait(0, 0, 16); run_seq_phase("C1c 多生产者序列（只按批叫 + 清空噪声）", 4, 160000, 1, 0); set_wait(200, 0, 0); }
    CHECK(atomic_load(&g_red_logs) == 0, "队列自己报过 %d 次红", atomic_load(&g_red_logs));
    printf("dq_queue_test：%d 项检查，%d 项红（交给录制线程 %llu 条，直调 %llu 次）\n", g_checks, g_bad, (unsigned long long)nvn_vk_dbg_dq_recs(), (unsigned long long)g_dq_direct);
    return g_bad ? 1 : 0;
}
