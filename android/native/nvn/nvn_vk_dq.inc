// SPDX-License-Identifier: MIT
// 两级回放：驱动的 vkCmd* 调用挪到一条单独的录制线程上做。由 backend_vulkan.c 包含（在它的全部 #include 之后、第一处 Vulkan 调用之前）。
//
// 来源：nx2android（shirai91/nx2android，MIT，Copyright (c) 2026 seedds）nxnative/dm/android/gfx_vk_dq.c；队列、录制线程、各调用的
// 参数拷贝照原文，改接的地方：
//   - 原文在设备入口表里把函数指针换成包装；本后端直接调 Vulkan 符号，所以在本文件末尾把用到的名字重定向到包装（#define vkCmdDraw w_vkCmdDraw …），
//     真入口用 vkGetDeviceProcAddr 取。
//   - 包装恒在，开关 debug.s2a.nvk_dq 运行时可切（dq_poll：每 64 次提交读一次，切换点在提交处、队列已清空）。关着时包装直接调驱动。
//   - 补上 vkCmdBeginQuery / vkCmdEndQuery；销毁命令池 / 释放命令缓冲 / 重置命令缓冲前先清空队列。
//
// 做法（原文注释的意思）：回放照旧准备每一笔绘制（状态、描述符集、管线），但它的 vkCmd* 调用只写进一个队列，由录制线程按同样的次序
// 录进同一个命令缓冲。命令缓冲的开始 / 结束 / 分配、队列提交、呈现先等录制线程把队列里的调用全做完（vk_dq_drain）：命令池要外部同步，
// 一次提交必须带着它之前录的全部内容。参数拷不了的调用（带不认识的 pNext 链）等队列清空后直接调。
// 生产者不止一条线程（回放线程、呈现线程），每条记录在自旋锁里写。
#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#define VKLOG(...) do { char dq_b_[256]; snprintf(dq_b_, sizeof dq_b_, __VA_ARGS__); nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", dq_b_); } while (0)
#define DQ_U __attribute__((unused))

#define DQ_CMDS(X) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdFillBuffer) X(vkCmdUpdateBuffer) \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) X(vkCmdResolveImage) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBeginRendering) \
    X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdSetBlendConstants) X(vkCmdSetStencilReference) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetDepthBias) X(vkCmdSetLineWidth) X(vkCmdDraw) \
    X(vkCmdPushConstants) X(vkCmdDrawIndexed) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDispatch) X(vkCmdDispatchIndirect) X(vkCmdResetQueryPool) \
    X(vkCmdWriteTimestamp) X(vkCmdBindTransformFeedbackBuffersEXT) X(vkCmdBeginTransformFeedbackEXT) X(vkCmdEndTransformFeedbackEXT) X(vkCmdDrawIndirectByteCountEXT) X(vkCmdDrawIndirectCount) X(vkCmdDrawIndexedIndirectCount) X(vkCmdBeginQuery) X(vkCmdEndQuery)
#define DQ_SYNC(X) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkAllocateCommandBuffers) X(vkResetCommandPool) X(vkQueueSubmit) X(vkQueuePresentKHR) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) \
    X(vkDestroyCommandPool) X(vkFreeCommandBuffers) X(vkResetCommandBuffer)
#define DQ_REAL(f) static PFN_##f r_##f;
DQ_CMDS(DQ_REAL)   // 命令缓冲之外的那几个入口（提交、呈现、池）直接调符号：它们没有版本 / 扩展的问题，宿主用例对其中几个打了桩
#define DQ_OP(f) OP_##f,
enum { OP_WRAP, DQ_CMDS(DQ_OP) OP_FILL, OP_N };   // OP_FILL：暂存填充（nvn_gpu_arena_fill），不是 Vulkan 调用
// 诊断读数：每类调用经过包装多少次（不管开关开没开）。backend_vulkan_wsi.inc 随「对象读数」每 300 帧打一行。
#define DQ_NAME(f) #f,
static const char *const g_dq_names[OP_N] = { "wrap", DQ_CMDS(DQ_NAME) "fill" };
static uint64_t g_dq_cnt[OP_N];
#define DQ_COUNT(op) __atomic_fetch_add(&g_dq_cnt[op], 1, __ATOMIC_RELAXED)

// ---- the queue: one producer, one consumer; positions count bytes since the start (the ring index is position % dq_RING), records 16-byte aligned ----
enum { dq_RING = 32 << 20 };
typedef struct { uint32_t op, size; VkCommandBuffer cb; } dq_rec_hdr;   // size: the whole record, header included
static uint8_t *g_dq_ring; static uint64_t g_dq_w;   // the producer's write position
static _Atomic uint64_t g_dq_pub, g_dq_rd;           // published end, the recorder's read position
static _Atomic int g_dq_sleeping;   // the recorder is (about to be) asleep on g_dq_cv
static _Atomic int g_dq_draining;   // how many threads wait in vk_dq_drain's slow path (a count, not a flag: threads drain concurrently and the one that leaves first must not erase the others' mark, or the recorder skips the wake-up and they sit out the 1 s timeout)
static pthread_spinlock_t g_dq_pl;   // the producers' lock, from dq_rec_begin to dq_rec_end
static pthread_mutex_t g_dq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_dq_cv, g_dq_dcv = PTHREAD_COND_INITIALIZER;   // g_dq_cv 在 vk_dq_init 里按单调时钟初始化（录制线程定时醒要用）；录制线程起来之前没有人睡在它上面，也没有人叫它
static _Atomic int g_dq_on;   // 开关：dq_poll 在提交处改，各生产者线程读（relaxed：切换点之后的调用走新路径即可）
#define DQ_ON() atomic_load_explicit(&g_dq_on, memory_order_relaxed)
static uint64_t g_dq_recs, g_dq_drains, g_dq_direct;   // records queued, drains, calls made directly (unknown arguments); 诊断计数，各线程可能同时读写：一律 relaxed 原子访问
static _Atomic uint32_t g_dq_cur_op;                   // 录制线程正在做 / 刚开始做的那条的 op（清空等超时的自报红里指出卡在哪一类调用）
static pthread_t g_dq_rec_th;                          // 录制线程（读它的 CPU 时间用，见 nvn_vk_dbg_dq_thread_cpu_ns）

// 录制线程怎么等活（调试开关，开会话时读、之后每 64 次提交再读；缺省都是原行为）：
//   nvk_dq_spin     没活时先 sched_yield 自旋几次再睡。缺省 200；0 = 直接睡。
//   nvk_dq_poll_us  睡法：0 = 睡到被叫醒；>0 = 定时醒（起始间隔这么多微秒，醒来没活就翻倍退避，封顶 DQ_POLL_MAX_US），靠定时醒把积压的活取走。
//   nvk_dq_wake_kb  生产者何时叫醒睡着的录制线程：0 = 每发布一条记录都叫；>0 = 积压不到这么多 KB 不叫（清空、环满、定时醒不受此限，积压最多等到下一次定时醒 / 过线 / 清空）。
static _Atomic int g_dq_spin = 200;
static _Atomic int g_dq_poll_us;
static _Atomic uint64_t g_dq_wake_bytes;
enum { DQ_POLL_MAX_US = 2000 };
// 诊断读数（nvk_gputime 开着才计；backend_vulkan_wsi.inc 随「对象读数」每 300 帧打一行）
static _Atomic int g_dq_stat;
enum { DQS_SLEEP, DQS_TMO, DQS_TMO_WORK, DQS_WAKE, DQS_DRAIN_WAIT, DQS_DRAIN_NS, DQS_N };   // 录制线程进睡眠次数 / 定时醒次数 / 其中醒来发现有活的次数 / 生产者真去叫醒的次数 / 清空要等的次数 / 清空等待总纳秒
static uint64_t g_dq_st[DQS_N];
#define DQ_ST(i, v) do { if (atomic_load_explicit(&g_dq_stat, memory_order_relaxed)) __atomic_fetch_add(&g_dq_st[i], (uint64_t)(v), __ATOMIC_RELAXED); } while (0)
static uint64_t dq_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
// 等待分布读数（同样 nvk_gputime 开着才计）：两张直方图——
//   睡着后的等活：录制线程睡着期间，第一条被发布的记录从发布到它醒来开始录的间隔（叫醒、定时醒、调度的全部等待；录制线程醒着时发布的记录没有「睡」带来的等待，不计）；
//   清空等待：vk_dq_drain 从进来到队列清空的时长（回放线程在这段时间里干等）。
// 桶上界（微秒）20 / 50 / 100 / 200 / 500 / 1000 / 2000 / 5000，最后一桶 ≥ 5000；另记最大值（纳秒）。
enum { DQH_N = 9 };
static const uint32_t g_dq_hb_us[DQH_N - 1] = { 20, 50, 100, 200, 500, 1000, 2000, 5000 };
static const char *const g_dq_hb_name[DQH_N] = { "<20", "<50", "<100", "<200", "<500", "<1000", "<2000", "<5000", ">=5000" };
static uint64_t g_dq_hw[DQH_N], g_dq_hd[DQH_N];   // 睡着后的等活 / 清空等待：各桶次数
static uint64_t g_dq_hw_max, g_dq_hd_max;         // 对应的最大值（纳秒）
static _Atomic uint64_t g_dq_t0;                  // 录制线程睡着期间发布的第一条记录的时刻（纳秒，0 = 还没有）；生产者写、录制线程醒来取走
static void dq_hist_add(uint64_t *h, uint64_t *mx, uint64_t ns) {
    unsigned i = 0; while (i < DQH_N - 1 && ns >= (uint64_t)g_dq_hb_us[i] * 1000u) i++;
    __atomic_fetch_add(&h[i], 1, __ATOMIC_RELAXED);
    uint64_t m = __atomic_load_n(mx, __ATOMIC_RELAXED);
    while (ns > m && !__atomic_compare_exchange_n(mx, &m, ns, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
static unsigned dq_hist_pct(const uint64_t *h, uint64_t total, unsigned pct) {   // 第 pct 百分位落在哪个桶（total = 0 时返回 0）
    uint64_t need = (total * pct + 99) / 100, acc = 0; if (!need) need = 1;
    for (unsigned i = 0; i < DQH_N; i++) { acc += h[i]; if (acc >= need) return i; }
    return DQH_N - 1;
}
// 读数行：取走（清零）两张直方图，写成一行。backend_vulkan_wsi.inc 每 300 帧调一次
static DQ_U void dq_lat_line(char *b, size_t n) {
    uint64_t w[DQH_N], d[DQH_N], wt = 0, dt = 0;
    for (int i = 0; i < DQH_N; i++) { w[i] = __atomic_exchange_n(&g_dq_hw[i], 0, __ATOMIC_RELAXED); wt += w[i]; d[i] = __atomic_exchange_n(&g_dq_hd[i], 0, __ATOMIC_RELAXED); dt += d[i]; }
    const uint64_t wm = __atomic_exchange_n(&g_dq_hw_max, 0, __ATOMIC_RELAXED), dm = __atomic_exchange_n(&g_dq_hd_max, 0, __ATOMIC_RELAXED);
    snprintf(b, n, "两级回放等待分布（近 300 帧，微秒）：录制线程睡着后第一条记录从发布到被录 %llu 次，p50 %s、p99 %s、最大 %.0f；清空等待 %llu 次，p50 %s、p99 %s、最大 %.0f",
        (unsigned long long)wt, wt ? g_dq_hb_name[dq_hist_pct(w, wt, 50)] : "-", wt ? g_dq_hb_name[dq_hist_pct(w, wt, 99)] : "-", (double)wm / 1000.0,
        (unsigned long long)dt, dt ? g_dq_hb_name[dq_hist_pct(d, dt, 50)] : "-", dt ? g_dq_hb_name[dq_hist_pct(d, dt, 99)] : "-", (double)dm / 1000.0);
}

static void dq_wake(void) { if (atomic_load(&g_dq_sleeping)) { DQ_ST(DQS_WAKE, 1); pthread_mutex_lock(&g_dq_mu); pthread_cond_signal(&g_dq_cv); pthread_mutex_unlock(&g_dq_mu); } }
static void vk_dq_drain(void) {
    if (!g_dq_ring) return;
    uint64_t w = atomic_load(&g_dq_pub);   // 到此刻已经写完的记录（原文在自旋锁里读写位置；这里读已发布的位置，免得开关关着时每个调用都过一次锁。别的线程正写到一半的那条不算，它那次调用还没返回）
    if (atomic_load_explicit(&g_dq_rd, memory_order_acquire) >= w) return;
    const uint64_t t0 = atomic_load_explicit(&g_dq_stat, memory_order_relaxed) ? dq_now_ns() : 0;
    __atomic_fetch_add(&g_dq_drains, 1, __ATOMIC_RELAXED); dq_wake();
    for (int i = 0; i < 2000 && atomic_load_explicit(&g_dq_rd, memory_order_acquire) < w; i++) sched_yield();   // the recorder is usually a few calls behind
    if (atomic_load_explicit(&g_dq_rd, memory_order_acquire) >= w) goto done;
    pthread_mutex_lock(&g_dq_mu); atomic_fetch_add(&g_dq_draining, 1);
    while (atomic_load_explicit(&g_dq_rd, memory_order_acquire) < w) { pthread_cond_signal(&g_dq_cv);
        struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += 1;
        if (pthread_cond_timedwait(&g_dq_dcv, &g_dq_mu, &dl)) { uint64_t rd = atomic_load(&g_dq_rd); static _Atomic int n; unsigned cur = atomic_load_explicit(&g_dq_cur_op, memory_order_relaxed);
            if (atomic_fetch_add_explicit(&n, 1, memory_order_relaxed) < 20) VKLOG("[RED] two-stage replay: a drain waits over 1 s: written %llu, published %llu, read %llu; the recorder is at op %u (%s)",
                (unsigned long long)w, (unsigned long long)atomic_load(&g_dq_pub), (unsigned long long)rd, cur, cur < OP_N ? g_dq_names[cur] : "?"); } }
    atomic_fetch_sub(&g_dq_draining, 1); pthread_mutex_unlock(&g_dq_mu);
done:
    if (t0) { const uint64_t ns = dq_now_ns() - t0; DQ_ST(DQS_DRAIN_WAIT, 1); DQ_ST(DQS_DRAIN_NS, ns); dq_hist_add(g_dq_hd, &g_dq_hd_max, ns); }
}
static void *dq_rec_begin(uint32_t op, VkCommandBuffer cb, size_t n) {   // room for a record with n bytes after its header
    size_t need = (sizeof(dq_rec_hdr) + n + 15) & ~(size_t)15;   // 16-byte records: the ring's end always has room for a whole header
    if (need > dq_RING / 4) return NULL;   // (no call copies that much: such a call is made directly)
    pthread_spin_lock(&g_dq_pl);   // released by dq_rec_end
    size_t off = (size_t)(g_dq_w % dq_RING);
    if (off + need > dq_RING) {   // the rest of the ring is skipped
        while (g_dq_w + (dq_RING - off) - atomic_load_explicit(&g_dq_rd, memory_order_acquire) > dq_RING) { dq_wake(); sched_yield(); }
        dq_rec_hdr *h = (dq_rec_hdr *)(g_dq_ring + off); h->op = OP_WRAP; h->size = (uint32_t)(dq_RING - off); g_dq_w += dq_RING - off; atomic_store_explicit(&g_dq_pub, g_dq_w, memory_order_release); off = 0;
    }
    while (g_dq_w + need - atomic_load_explicit(&g_dq_rd, memory_order_acquire) > dq_RING) { dq_wake(); sched_yield(); }
    dq_rec_hdr *h = (dq_rec_hdr *)(g_dq_ring + off); h->op = op; h->size = (uint32_t)need; h->cb = cb;
    return h + 1;
}
static void dq_rec_end(void *payload) {
    dq_rec_hdr *h = (dq_rec_hdr *)payload - 1; g_dq_w += h->size; __atomic_store_n(&g_dq_recs, g_dq_recs + 1, __ATOMIC_RELAXED);   // 自旋锁里：写者只有一个，读者（用例 / 读数）用原子读
    const uint64_t w = g_dq_w;
    atomic_store(&g_dq_pub, w);   // (seq_cst, against the recorder's sleep flag: either it sees this record or this sees it asleep)
    pthread_spin_unlock(&g_dq_pl);
    if (atomic_load(&g_dq_sleeping)) {   // 录制线程醒着就不用叫：它做完手头的会看到这一条
        if (atomic_load_explicit(&g_dq_stat, memory_order_relaxed) && !atomic_load_explicit(&g_dq_t0, memory_order_relaxed)) {   // 读数：记下睡着期间第一条记录发布的时刻（只在睡着时记，醒着时发布的记录不读时钟）
            uint64_t z = 0; atomic_compare_exchange_strong_explicit(&g_dq_t0, &z, dq_now_ns(), memory_order_relaxed, memory_order_relaxed);
        }
        const uint64_t wb = atomic_load_explicit(&g_dq_wake_bytes, memory_order_relaxed), rd = atomic_load_explicit(&g_dq_rd, memory_order_acquire);   // 睡着时 g_dq_rd 不动，读它不会和录制线程抢缓存行
        if (!wb || (w > rd && w - rd >= wb)) dq_wake();   // 按批叫（wb > 0）：积压不够就让它睡着，等定时醒 / 下一次过线 / 清空来叫（w <= rd：别的生产者发布了更靠后的记录、已被取走，这一条也做完了，积压为零）
    }
}
// copying helpers: a record is a fixed struct followed by its arrays
#define DQ_ARR(T, p, n) ((n) && (p) ? (size_t)(n) * sizeof(T) : 0)
static uint8_t *dq_put(uint8_t **at, const void *src, size_t n) { uint8_t *d = *at; if (n) memcpy(d, src, n); *at += n; return d; }

// ---- the recorder thread ----
typedef struct { VkPipelineStageFlags src, dst; VkDependencyFlags dep; uint32_t nm, nb, ni; } a_barrier;
typedef struct { VkBuffer s, d; uint32_t n; } a_copybuf;
typedef struct { VkBuffer b; VkImage i; VkImageLayout l; uint32_t n; } a_b2i;
typedef struct { VkImage i; VkImageLayout l; VkBuffer b; uint32_t n; } a_i2b;
typedef struct { VkImage si; VkImageLayout sl; VkImage di; VkImageLayout dl; uint32_t n; VkFilter f; } a_img2;
typedef struct { VkBuffer b; VkDeviceSize off, size; uint32_t data; } a_fill;
typedef struct { VkBuffer b; VkDeviceSize off, size; } a_upd;
typedef struct { VkImage i; VkImageLayout l; VkClearColorValue c; VkClearDepthStencilValue ds; uint32_t n; } a_clrimg;
typedef struct { uint32_t na, nr; } a_clratt;
typedef struct { VkRenderPassBeginInfo bi; VkRenderPassAttachmentBeginInfo ab; int has_ab; VkSubpassContents c; } a_rpbegin;
typedef struct { VkRenderingInfo ri; int has_d, has_s; VkRenderingAttachmentInfo d, s; } a_rbegin;
typedef struct { VkPipelineBindPoint bp; VkPipeline p; } a_bindpipe;
typedef struct { VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t first, n, ndyn; } a_sets;
typedef struct { uint32_t first, n; } a_range;
typedef struct { VkBuffer b; VkDeviceSize off; VkIndexType t; } a_index;
typedef struct { float c[4]; } a_blend;
typedef struct { VkStencilFaceFlags f; uint32_t v; } a_stencil;
typedef struct { float c, clamp, slope; } a_bias;
typedef struct { uint32_t a, b, c, d; int32_t e; uint32_t f; } a_draw;
typedef struct { VkPipelineLayout l; VkShaderStageFlags st; uint32_t off, size; } a_push;
typedef struct { VkBuffer b; VkDeviceSize off; uint32_t n, stride; } a_indirect;
typedef struct { VkQueryPool p; uint32_t first, n; } a_qreset;
typedef struct { VkPipelineStageFlagBits st; VkQueryPool p; uint32_t q; } a_ts;
typedef struct { uint32_t first, n; int has_sizes, has_bufs, has_offs; } a_xfb;
typedef struct { uint32_t inst, first_inst; VkBuffer b; VkDeviceSize off; uint32_t ctr_off, stride; } a_bytecount;
typedef struct { VkBuffer b; VkDeviceSize off; VkBuffer cb; VkDeviceSize coff; uint32_t max, stride; } a_indcount;
typedef struct { VkQueryPool p; uint32_t q; VkQueryControlFlags fl; } a_query;
// 暂存填充（对照 iOS nvn_rec.h 的常量缓冲拷贝线程 ubc_*）：核心把常量缓冲拷进本段临时上传片，一款大型开放世界游戏（01007EF00011E000）神庙场景每帧约 1.1 万次、44 MB，占回放线程可观的一块。
// 这份拷贝排进同一个队列由录制线程按序做：GPU 只在提交之后读上传片，而 vkEndCommandBuffer 先清空队列，所以提交前一定拷完；
// 核心改来源内存之前按票号等（nvn_gpu_arena_fill_wait）。票号在生产者自旋锁里递增，录制线程按记录顺序做，所以「已做完」单调。
typedef struct { void *d; const void *s; uint64_t n, seq; } a_memfill;
static _Atomic uint64_t g_fill_seq, g_fill_done;
// 代拷（nvn_gpu_arena_fill_take）：核心改来源内存之前，相交、还没开始的那几笔由核心自己拷，不再干等录制线程把它前面的活全做完。
// 一台骁龙 8 至尊版手机上的读数（一款大型开放世界游戏洞内走动，每帧）：交异步填约 1,400 次、改内存前等填完约 260 次，
// 游戏线程 15.1% 时间耗在等待里的 sched_yield，录制线程 58.6% 在 sched_yield 空转——等的那一笔前面排着的绘制记录才是大头。
// 每笔填充一个状态字（按票号取模的旁挂表），字里带票号：w = 票号 << 2 | 状态，状态 0 未开始 / 1 有人在拷 / 2 拷完。
//   拷贝权：谁把 (票号, 0) CAS 成 (票号, 1) 谁拷，拷完 release 存 (票号, 2)。录制线程处理 OP_FILL 时 CAS 失败：
//     看到 (票号, 1) = 生产者正拷，等到 2 再往下（很少见，生产者拷的就是这一笔）；看到 (票号, 2) 或别的票号 = 生产者拷完了，跳过拷贝。
//   g_fill_done 只由录制线程按记录顺序推进：每一条都确认「已拷完」（自己拷完，或 acquire 读到 2）之后才 release 存它的票号，
//     所以「票号 ≤ g_fill_done 的都已拷完」不变；代拷不动 g_fill_done（新票号被代拷、旧票号还在排队时，水位不能越过旧票号）。
//   槽复用：生产者给新票号占槽时，槽里的旧票号若还不是 2 就先代拷 / 等它到 2（容量 FILL_TAB 远大于核心同时挂着的 UBF_N = 256，正常碰不到）；
//     所以槽里换成别的票号意味着旧票号已拷完，录制线程据此跳过。字里带票号，迟到的 CAS 不会误改复用后的槽。
//   内存序：拷贝者的 memcpy →（release 存 2）→ 另一方 acquire 读到 2 / CAS 失败时 acquire 读到的值，所以
//     ① 核心 rp_ub_fill_fence 返回时，相交的每一笔的读来源都 happens-before 核心随后改来源的写（内容是填充那一刻的）；
//     ② 录制线程推进 g_fill_done 与之后的提交（vkEndCommandBuffer 先清空队列）都在拷贝之后，GPU 读上传片时内容已在；
//     ③ 槽里的 d / s / n 由生产者在 release 存 (票号, 0) 之前写，代拷方 CAS 成功（acquire）后读，录制线程只用记录里自己那份。
enum { FILL_TAB = 1024, FILL_TODO = 0, FILL_BUSY = 1, FILL_DONE = 2 };   // FILL_TAB 是 2 的幂
#define FILL_W(seq, st) ((uint64_t)(seq) << 2 | (uint64_t)(st))
typedef struct { _Atomic uint64_t w; void *d; const void *s; uint64_t n; } dq_fill_slot;
static dq_fill_slot g_fill_tab[FILL_TAB];
#ifndef DQ_FILL_REC_HOOK
#define DQ_FILL_REC_HOOK(a) ((void)0)   // 用例钩子：录制线程认领了一笔、开拷之前（宿主用例在这里停住录制线程，造「录制线程正在拷」）
#endif
static void dq_fill_run(const a_memfill *a) {   // 录制线程
    _Atomic uint64_t *w = &g_fill_tab[a->seq & (FILL_TAB - 1)].w;
    uint64_t e = FILL_W(a->seq, FILL_TODO);
    if (atomic_compare_exchange_strong_explicit(w, &e, FILL_W(a->seq, FILL_BUSY), memory_order_acquire, memory_order_acquire)) {
        DQ_FILL_REC_HOOK(a);
        memcpy(a->d, a->s, (size_t)a->n);
        atomic_store_explicit(w, FILL_W(a->seq, FILL_DONE), memory_order_release);
    } else if (e == FILL_W(a->seq, FILL_BUSY)) {
        while (atomic_load_explicit(w, memory_order_acquire) == FILL_W(a->seq, FILL_BUSY)) sched_yield();   // 生产者正拷这一笔：拷完才能推进水位
    }   // 否则生产者已拷完（2，或槽已换成更新的票号）：跳过
    atomic_store_explicit(&g_fill_done, a->seq, memory_order_release);
}
// 让票号 t 这一笔拷完（生产者）。返回 0 = 早已拷完；1 = 本线程拷的；2 = 录制线程正在拷，等它拷完
static int dq_fill_take(uint64_t t) {
    if (!t || atomic_load_explicit(&g_fill_done, memory_order_acquire) >= t) return 0;
    dq_fill_slot *sl = &g_fill_tab[t & (FILL_TAB - 1)];
    uint64_t e = FILL_W(t, FILL_TODO);
    if (atomic_compare_exchange_strong_explicit(&sl->w, &e, FILL_W(t, FILL_BUSY), memory_order_acquire, memory_order_acquire)) {
        memcpy(sl->d, sl->s, (size_t)sl->n);
        atomic_store_explicit(&sl->w, FILL_W(t, FILL_DONE), memory_order_release);
        return 1;
    }
    if (e != FILL_W(t, FILL_BUSY)) return 0;
    while (atomic_load_explicit(&sl->w, memory_order_acquire) == FILL_W(t, FILL_BUSY)) sched_yield();   // 录制线程拷到一半：等的只是这一笔的 memcpy
    return 2;
}

static void dq_run(const dq_rec_hdr *h) {
    const uint8_t *p = (const uint8_t *)(h + 1); VkCommandBuffer cb = h->cb;
    switch (h->op) {
    case OP_vkCmdPipelineBarrier: { const a_barrier *a = (const void *)p; const uint8_t *q = p + sizeof *a;
        const VkMemoryBarrier *m = (const void *)q; q += a->nm * sizeof *m; const VkBufferMemoryBarrier *b = (const void *)q; q += a->nb * sizeof *b; const VkImageMemoryBarrier *im = (const void *)q;
        r_vkCmdPipelineBarrier(cb, a->src, a->dst, a->dep, a->nm, a->nm ? m : NULL, a->nb, a->nb ? b : NULL, a->ni, a->ni ? im : NULL); break; }
    case OP_vkCmdCopyBuffer: { const a_copybuf *a = (const void *)p; r_vkCmdCopyBuffer(cb, a->s, a->d, a->n, (const VkBufferCopy *)(p + sizeof *a)); break; }
    case OP_vkCmdCopyBufferToImage: { const a_b2i *a = (const void *)p; r_vkCmdCopyBufferToImage(cb, a->b, a->i, a->l, a->n, (const VkBufferImageCopy *)(p + sizeof *a)); break; }
    case OP_vkCmdCopyImageToBuffer: { const a_i2b *a = (const void *)p; r_vkCmdCopyImageToBuffer(cb, a->i, a->l, a->b, a->n, (const VkBufferImageCopy *)(p + sizeof *a)); break; }
    case OP_vkCmdCopyImage: { const a_img2 *a = (const void *)p; r_vkCmdCopyImage(cb, a->si, a->sl, a->di, a->dl, a->n, (const VkImageCopy *)(p + sizeof *a)); break; }
    case OP_vkCmdBlitImage: { const a_img2 *a = (const void *)p; r_vkCmdBlitImage(cb, a->si, a->sl, a->di, a->dl, a->n, (const VkImageBlit *)(p + sizeof *a), a->f); break; }
    case OP_vkCmdResolveImage: { const a_img2 *a = (const void *)p; r_vkCmdResolveImage(cb, a->si, a->sl, a->di, a->dl, a->n, (const VkImageResolve *)(p + sizeof *a)); break; }
    case OP_vkCmdFillBuffer: { const a_fill *a = (const void *)p; r_vkCmdFillBuffer(cb, a->b, a->off, a->size, a->data); break; }
    case OP_vkCmdUpdateBuffer: { const a_upd *a = (const void *)p; r_vkCmdUpdateBuffer(cb, a->b, a->off, a->size, p + sizeof *a); break; }
    case OP_vkCmdClearColorImage: { const a_clrimg *a = (const void *)p; r_vkCmdClearColorImage(cb, a->i, a->l, &a->c, a->n, (const VkImageSubresourceRange *)(p + sizeof *a)); break; }
    case OP_vkCmdClearDepthStencilImage: { const a_clrimg *a = (const void *)p; r_vkCmdClearDepthStencilImage(cb, a->i, a->l, &a->ds, a->n, (const VkImageSubresourceRange *)(p + sizeof *a)); break; }
    case OP_vkCmdClearAttachments: { const a_clratt *a = (const void *)p; const VkClearAttachment *at = (const void *)(p + sizeof *a);
        r_vkCmdClearAttachments(cb, a->na, at, a->nr, (const VkClearRect *)(at + a->na)); break; }
    case OP_vkCmdBeginRenderPass: { a_rpbegin a; memcpy(&a, p, sizeof a); const uint8_t *q = p + sizeof a;
        a.bi.pClearValues = a.bi.clearValueCount ? (const VkClearValue *)q : NULL; q += a.bi.clearValueCount * sizeof(VkClearValue);
        if (a.has_ab) { a.ab.pAttachments = (const VkImageView *)q; a.bi.pNext = &a.ab; } else a.bi.pNext = NULL;
        r_vkCmdBeginRenderPass(cb, &a.bi, a.c); break; }
    case OP_vkCmdEndRenderPass: r_vkCmdEndRenderPass(cb); break;
    case OP_vkCmdBeginRendering: { a_rbegin a; memcpy(&a, p, sizeof a);
        a.ri.pColorAttachments = a.ri.colorAttachmentCount ? (const VkRenderingAttachmentInfo *)(p + sizeof a) : NULL;
        a.ri.pDepthAttachment = a.has_d ? &a.d : NULL; a.ri.pStencilAttachment = a.has_s ? &a.s : NULL; r_vkCmdBeginRendering(cb, &a.ri); break; }
    case OP_vkCmdEndRendering: r_vkCmdEndRendering(cb); break;
    case OP_vkCmdBindPipeline: { const a_bindpipe *a = (const void *)p; r_vkCmdBindPipeline(cb, a->bp, a->p); break; }
    case OP_vkCmdBindDescriptorSets: { const a_sets *a = (const void *)p; const VkDescriptorSet *s = (const void *)(p + sizeof *a);
        r_vkCmdBindDescriptorSets(cb, a->bp, a->l, a->first, a->n, s, a->ndyn, a->ndyn ? (const uint32_t *)(s + a->n) : NULL); break; }
    case OP_vkCmdBindVertexBuffers: { const a_range *a = (const void *)p; const VkBuffer *b = (const void *)(p + sizeof *a);
        r_vkCmdBindVertexBuffers(cb, a->first, a->n, b, (const VkDeviceSize *)(b + a->n)); break; }
    case OP_vkCmdBindIndexBuffer: { const a_index *a = (const void *)p; r_vkCmdBindIndexBuffer(cb, a->b, a->off, a->t); break; }
    case OP_vkCmdSetViewport: { const a_range *a = (const void *)p; r_vkCmdSetViewport(cb, a->first, a->n, (const VkViewport *)(p + sizeof *a)); break; }
    case OP_vkCmdSetScissor: { const a_range *a = (const void *)p; r_vkCmdSetScissor(cb, a->first, a->n, (const VkRect2D *)(p + sizeof *a)); break; }
    case OP_vkCmdSetBlendConstants: { const a_blend *a = (const void *)p; r_vkCmdSetBlendConstants(cb, a->c); break; }
    case OP_vkCmdSetStencilReference: { const a_stencil *a = (const void *)p; r_vkCmdSetStencilReference(cb, a->f, a->v); break; }
    case OP_vkCmdSetStencilCompareMask: { const a_stencil *a = (const void *)p; r_vkCmdSetStencilCompareMask(cb, a->f, a->v); break; }
    case OP_vkCmdSetStencilWriteMask: { const a_stencil *a = (const void *)p; r_vkCmdSetStencilWriteMask(cb, a->f, a->v); break; }
    case OP_vkCmdSetDepthBias: { const a_bias *a = (const void *)p; r_vkCmdSetDepthBias(cb, a->c, a->clamp, a->slope); break; }
    case OP_vkCmdSetLineWidth: { float w; memcpy(&w, p, sizeof w); r_vkCmdSetLineWidth(cb, w); break; }
    case OP_vkCmdDraw: { const a_draw *a = (const void *)p; r_vkCmdDraw(cb, a->a, a->b, a->c, a->d); break; }
    case OP_vkCmdDrawIndexed: { const a_draw *a = (const void *)p; r_vkCmdDrawIndexed(cb, a->a, a->b, a->c, a->e, a->f); break; }
    case OP_vkCmdPushConstants: { const a_push *a = (const void *)p; r_vkCmdPushConstants(cb, a->l, a->st, a->off, a->size, p + sizeof *a); break; }
    case OP_vkCmdDrawIndirect: { const a_indirect *a = (const void *)p; r_vkCmdDrawIndirect(cb, a->b, a->off, a->n, a->stride); break; }
    case OP_vkCmdDrawIndexedIndirect: { const a_indirect *a = (const void *)p; r_vkCmdDrawIndexedIndirect(cb, a->b, a->off, a->n, a->stride); break; }
    case OP_vkCmdDispatch: { const a_draw *a = (const void *)p; r_vkCmdDispatch(cb, a->a, a->b, a->c); break; }
    case OP_vkCmdDispatchIndirect: { const a_indirect *a = (const void *)p; r_vkCmdDispatchIndirect(cb, a->b, a->off); break; }
    case OP_vkCmdResetQueryPool: { const a_qreset *a = (const void *)p; r_vkCmdResetQueryPool(cb, a->p, a->first, a->n); break; }
    case OP_vkCmdWriteTimestamp: { const a_ts *a = (const void *)p; r_vkCmdWriteTimestamp(cb, a->st, a->p, a->q); break; }
    case OP_vkCmdBindTransformFeedbackBuffersEXT: { const a_xfb *a = (const void *)p; const VkBuffer *b = (const void *)(p + sizeof *a); const VkDeviceSize *o = (const void *)(b + a->n);
        r_vkCmdBindTransformFeedbackBuffersEXT(cb, a->first, a->n, b, o, a->has_sizes ? o + a->n : NULL); break; }
    case OP_vkCmdBeginTransformFeedbackEXT: case OP_vkCmdEndTransformFeedbackEXT: { const a_xfb *a = (const void *)p; const VkBuffer *b = (const void *)(p + sizeof *a);
        const VkDeviceSize *o = (const void *)(b + (a->has_bufs ? a->n : 0));
        (h->op == OP_vkCmdBeginTransformFeedbackEXT ? r_vkCmdBeginTransformFeedbackEXT : r_vkCmdEndTransformFeedbackEXT)(cb, a->first, a->n, a->has_bufs ? b : NULL, a->has_offs ? o : NULL); break; }
    case OP_vkCmdDrawIndirectCount: { const a_indcount *a = (const void *)p; r_vkCmdDrawIndirectCount(cb, a->b, a->off, a->cb, a->coff, a->max, a->stride); break; }
    case OP_vkCmdDrawIndexedIndirectCount: { const a_indcount *a = (const void *)p; r_vkCmdDrawIndexedIndirectCount(cb, a->b, a->off, a->cb, a->coff, a->max, a->stride); break; }
    case OP_vkCmdDrawIndirectByteCountEXT: { const a_bytecount *a = (const void *)p; r_vkCmdDrawIndirectByteCountEXT(cb, a->inst, a->first_inst, a->b, a->off, a->ctr_off, a->stride); break; }
    case OP_vkCmdBeginQuery: { const a_query *a = (const void *)p; r_vkCmdBeginQuery(cb, a->p, a->q, a->fl); break; }
    case OP_vkCmdEndQuery: { const a_query *a = (const void *)p; r_vkCmdEndQuery(cb, a->p, a->q); break; }
    case OP_FILL: dq_fill_run((const a_memfill *)(const void *)p); break;
    default: break;
    }
}
#ifdef __ANDROID__
// 录制线程是每帧必经的帧线程：常驻进 ADPF 会话（android/native/adpf_hint.c），不靠忙线程扫描的阈值。弱引用：没链 ADPF 的夹具地址为空
void s2a_adpf_pin_self(void) __attribute__((weak));
#endif
static void *dq_recorder_main(void *arg) {
    (void)arg; 
#ifdef __ANDROID__
    pthread_setname_np(pthread_self(), "s2a-vk-record");
    if (s2a_adpf_pin_self) s2a_adpf_pin_self();
#endif
#ifdef __linux__
    prctl(PR_SET_TIMERSLACK, 1000UL, 0UL, 0UL, 0UL);   // 定时醒（nvk_dq_poll_us）别被默认 50 微秒的定时器松弛拖长；只作用于本线程
#endif
    uint64_t r = 0;
    for (;;) {
        uint64_t pub = atomic_load(&g_dq_pub);
        if (r == pub) {
            const int spin = atomic_load_explicit(&g_dq_spin, memory_order_relaxed);
            for (int i = 0; i < spin && (pub = atomic_load(&g_dq_pub)) == r; i++) sched_yield();   // the producer usually queues the next call within microseconds
            if (pub == r) {
                int wait_us = 0, tmo = 0;   // wait_us：这一觉的定时醒间隔，每次进睡眠从 nvk_dq_poll_us 起，醒来没活就翻倍
                pthread_mutex_lock(&g_dq_mu);
                if (atomic_load(&g_dq_draining)) pthread_cond_broadcast(&g_dq_dcv);
                atomic_store_explicit(&g_dq_t0, 0, memory_order_relaxed);   // 上一觉遗留的时间戳（生产者在我醒来之后才落下的）作废；下一行的 seq_cst 存储把它排在「睡着」标志之前
                atomic_store(&g_dq_sleeping, 1); DQ_ST(DQS_SLEEP, 1);
                while ((pub = atomic_load(&g_dq_pub)) == r) {
                    const int poll_us = atomic_load_explicit(&g_dq_poll_us, memory_order_relaxed);   // 每圈都读：开关变了，dq_tune 叫醒一下，这里就按新的睡
                    if (poll_us <= 0) { wait_us = 0; pthread_cond_wait(&g_dq_cv, &g_dq_mu); continue; }
                    if (wait_us <= 0) wait_us = poll_us;
                    struct timespec dl; clock_gettime(CLOCK_MONOTONIC, &dl);   // g_dq_cv 按单调时钟初始化：墙钟被改也不影响
                    dl.tv_nsec += (long)wait_us * 1000L; if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
                    if (pthread_cond_timedwait(&g_dq_cv, &g_dq_mu, &dl) == ETIMEDOUT) { tmo = 1; DQ_ST(DQS_TMO, 1); if (wait_us < DQ_POLL_MAX_US) wait_us *= 2; } else tmo = 0;   // 醒来没活：下次睡久一点（生产者停了就别白醒）
                }
                if (tmo) DQ_ST(DQS_TMO_WORK, 1);   // 是定时醒取走的活，不是谁叫醒的
                atomic_store(&g_dq_sleeping, 0); pthread_mutex_unlock(&g_dq_mu);
                if (atomic_load_explicit(&g_dq_stat, memory_order_relaxed)) { const uint64_t t0 = atomic_exchange_explicit(&g_dq_t0, 0, memory_order_relaxed); if (t0) dq_hist_add(g_dq_hw, &g_dq_hw_max, dq_now_ns() - t0); }   // 读数：睡着后第一条记录等了多久
            }
        }
        while (r != pub) { const dq_rec_hdr *h = (const dq_rec_hdr *)(g_dq_ring + r % dq_RING); atomic_store_explicit(&g_dq_cur_op, h->op, memory_order_relaxed); dq_run(h); r += h->size; atomic_store(&g_dq_rd, r); }   // seq_cst (a plain stlr on arm64): the read position is stored, then g_dq_draining is read, against the drainer's "count it, then read the position"
        if (atomic_load(&g_dq_draining)) { pthread_mutex_lock(&g_dq_mu); pthread_cond_broadcast(&g_dq_dcv); pthread_mutex_unlock(&g_dq_mu); }
    }
    return NULL;
}

// ---- 暂存填充（生产者：回放线程）----
// 两级回放关着（或队列还没建）就在本线程直接拷，返回 0。
static DQ_U uint64_t vk_dq_fill(void *d, const void *s, uint64_t n) {
    DQ_COUNT(OP_FILL);
    a_memfill *a = DQ_ON() && g_dq_ring ? dq_rec_begin(OP_FILL, VK_NULL_HANDLE, sizeof *a) : NULL;
    if (!a) { memcpy(d, s, (size_t)n); return 0; }
    const uint64_t seq = atomic_load_explicit(&g_fill_seq, memory_order_relaxed) + 1;   // 自旋锁里：按记录顺序递增
    atomic_store_explicit(&g_fill_seq, seq, memory_order_relaxed);
    dq_fill_slot *sl = &g_fill_tab[seq & (FILL_TAB - 1)];
    const uint64_t ow = atomic_load_explicit(&sl->w, memory_order_acquire);
    if ((ow >> 2) && (ow & 3) != FILL_DONE) dq_fill_take(ow >> 2);   // 槽里的旧票号还没拷完（录制线程落后 FILL_TAB 笔以上）：先让它拷完再复用
    sl->d = d; sl->s = s; sl->n = n;
    atomic_store_explicit(&sl->w, FILL_W(seq, FILL_TODO), memory_order_release);
    a->d = d; a->s = s; a->n = n; a->seq = seq;
    dq_rec_end(a);
    return seq;
}
static DQ_U int vk_dq_fill_take(uint64_t t) { return dq_fill_take(t); }
// 等票号 ≤ t 的填充做完：录制线程通常只落后几条，先叫醒它、让出几轮；还没到就走清空（它有睡眠与超时自报红），这条记录已发布，清空一定覆盖它
static DQ_U uint64_t vk_dq_fill_wait(uint64_t t) {
    uint64_t d = atomic_load_explicit(&g_fill_done, memory_order_acquire);
    if (!t || d >= t) return d;
    dq_wake();
    for (int i = 0; i < 2000 && (d = atomic_load_explicit(&g_fill_done, memory_order_acquire)) < t; i++) sched_yield();
    if (d < t) { vk_dq_drain(); d = atomic_load_explicit(&g_fill_done, memory_order_acquire); }
    return d;
}

// ---- the wrappers (the producer) ----
#ifndef NVK_DQ_WRITE_BOUNDARY
#define NVK_DQ_WRITE_BOUNDARY() ((void)0)
#define NVK_DQ_SEGMENT_BOUNDARY() ((void)0)
#endif
// 查询时间戳只写独立查询池，不访问缓冲。其它录制命令保守结束暂存写串。
#define DQ_WRITE_BOUNDARY(op) do { if ((op) != OP_vkCmdWriteTimestamp) NVK_DQ_WRITE_BOUNDARY(); } while (0)
#define DQ_DIRECT(f, ...) do { DQ_WRITE_BOUNDARY(OP_##f); vk_dq_drain(); __atomic_fetch_add(&g_dq_direct, 1, __ATOMIC_RELAXED); r_##f(__VA_ARGS__); } while (0)
#define DQ_BEGIN(op, T, extra) DQ_COUNT(op); uint8_t *base_ = DQ_ON() ? dq_rec_begin(op, cb, sizeof(T) + (extra)) : NULL; if (!base_) goto direct; DQ_WRITE_BOUNDARY(op); T *a = (T *)base_; uint8_t *at_ = base_ + sizeof(T)
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep, uint32_t nm, const VkMemoryBarrier *m,
                                                         uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    for (uint32_t i = 0; i < nm; i++) if (m[i].pNext) goto direct;
    for (uint32_t i = 0; i < nb; i++) if (b[i].pNext) goto direct;
    for (uint32_t i = 0; i < ni; i++) if (im[i].pNext) goto direct;
    { DQ_BEGIN(OP_vkCmdPipelineBarrier, a_barrier, DQ_ARR(VkMemoryBarrier, m, nm) + DQ_ARR(VkBufferMemoryBarrier, b, nb) + DQ_ARR(VkImageMemoryBarrier, im, ni));
      *a = (a_barrier){src, dst, dep, m ? nm : 0, b ? nb : 0, im ? ni : 0}; dq_put(&at_, m, DQ_ARR(VkMemoryBarrier, m, nm)); dq_put(&at_, b, DQ_ARR(VkBufferMemoryBarrier, b, nb)); dq_put(&at_, im, DQ_ARR(VkImageMemoryBarrier, im, ni));
      dq_rec_end(base_); return; }
direct: DQ_DIRECT(vkCmdPipelineBarrier, cb, src, dst, dep, nm, m, nb, b, ni, im);
}
#define W_REGIONS(f, OP, T, R, sig, init, call) \
    static DQ_U VKAPI_ATTR void VKAPI_CALL w_##f sig { { DQ_BEGIN(OP, T, DQ_ARR(R, rg, n)); *a = init; dq_put(&at_, rg, DQ_ARR(R, rg, n)); dq_rec_end(base_); return; } direct: DQ_DIRECT call; }
W_REGIONS(vkCmdCopyBuffer, OP_vkCmdCopyBuffer, a_copybuf, VkBufferCopy, (VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *rg), ((a_copybuf){s, d, n}), (vkCmdCopyBuffer, cb, s, d, n, rg))
W_REGIONS(vkCmdCopyBufferToImage, OP_vkCmdCopyBufferToImage, a_b2i, VkBufferImageCopy, (VkCommandBuffer cb, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy *rg),
          ((a_b2i){b, i, l, n}), (vkCmdCopyBufferToImage, cb, b, i, l, n, rg))
W_REGIONS(vkCmdCopyImageToBuffer, OP_vkCmdCopyImageToBuffer, a_i2b, VkBufferImageCopy, (VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *rg),
          ((a_i2b){i, l, b, n}), (vkCmdCopyImageToBuffer, cb, i, l, b, n, rg))
W_REGIONS(vkCmdCopyImage, OP_vkCmdCopyImage, a_img2, VkImageCopy, (VkCommandBuffer cb, VkImage si, VkImageLayout sl, VkImage di, VkImageLayout dl, uint32_t n, const VkImageCopy *rg),
          ((a_img2){si, sl, di, dl, n, VK_FILTER_NEAREST}), (vkCmdCopyImage, cb, si, sl, di, dl, n, rg))
W_REGIONS(vkCmdBlitImage, OP_vkCmdBlitImage, a_img2, VkImageBlit, (VkCommandBuffer cb, VkImage si, VkImageLayout sl, VkImage di, VkImageLayout dl, uint32_t n, const VkImageBlit *rg, VkFilter f),
          ((a_img2){si, sl, di, dl, n, f}), (vkCmdBlitImage, cb, si, sl, di, dl, n, rg, f))
W_REGIONS(vkCmdResolveImage, OP_vkCmdResolveImage, a_img2, VkImageResolve, (VkCommandBuffer cb, VkImage si, VkImageLayout sl, VkImage di, VkImageLayout dl, uint32_t n, const VkImageResolve *rg),
          ((a_img2){si, sl, di, dl, n, VK_FILTER_NEAREST}), (vkCmdResolveImage, cb, si, sl, di, dl, n, rg))
W_REGIONS(vkCmdSetViewport, OP_vkCmdSetViewport, a_range, VkViewport, (VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *rg), ((a_range){first, n}), (vkCmdSetViewport, cb, first, n, rg))
W_REGIONS(vkCmdSetScissor, OP_vkCmdSetScissor, a_range, VkRect2D, (VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *rg), ((a_range){first, n}), (vkCmdSetScissor, cb, first, n, rg))
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdFillBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize size, uint32_t data) {
    { DQ_BEGIN(OP_vkCmdFillBuffer, a_fill, 0); *a = (a_fill){b, off, size, data}; (void)at_; dq_rec_end(base_); return; } direct: DQ_DIRECT(vkCmdFillBuffer, cb, b, off, size, data); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdUpdateBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize size, const void *data) {
    { DQ_BEGIN(OP_vkCmdUpdateBuffer, a_upd, (size_t)size); *a = (a_upd){b, off, size}; dq_put(&at_, data, (size_t)size); dq_rec_end(base_); return; } direct: DQ_DIRECT(vkCmdUpdateBuffer, cb, b, off, size, data); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdClearColorImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue *c, uint32_t n, const VkImageSubresourceRange *rg) {
    { DQ_BEGIN(OP_vkCmdClearColorImage, a_clrimg, DQ_ARR(VkImageSubresourceRange, rg, n)); memset(a, 0, sizeof *a); a->i = i; a->l = l; a->c = *c; a->n = n; dq_put(&at_, rg, DQ_ARR(VkImageSubresourceRange, rg, n)); dq_rec_end(base_); return; }
    direct: DQ_DIRECT(vkCmdClearColorImage, cb, i, l, c, n, rg); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue *ds, uint32_t n, const VkImageSubresourceRange *rg) {
    { DQ_BEGIN(OP_vkCmdClearDepthStencilImage, a_clrimg, DQ_ARR(VkImageSubresourceRange, rg, n)); memset(a, 0, sizeof *a); a->i = i; a->l = l; a->ds = *ds; a->n = n; dq_put(&at_, rg, DQ_ARR(VkImageSubresourceRange, rg, n)); dq_rec_end(base_); return; }
    direct: DQ_DIRECT(vkCmdClearDepthStencilImage, cb, i, l, ds, n, rg); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *at, uint32_t nr, const VkClearRect *rc) {
    { DQ_BEGIN(OP_vkCmdClearAttachments, a_clratt, DQ_ARR(VkClearAttachment, at, na) + DQ_ARR(VkClearRect, rc, nr)); *a = (a_clratt){na, nr};
      dq_put(&at_, at, DQ_ARR(VkClearAttachment, at, na)); dq_put(&at_, rc, DQ_ARR(VkClearRect, rc, nr)); dq_rec_end(base_); return; }
    direct: DQ_DIRECT(vkCmdClearAttachments, cb, na, at, nr, rc); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents c) {
    const VkRenderPassAttachmentBeginInfo *ab = NULL;
    if (bi->pNext) { const VkBaseInStructure *x = bi->pNext; if (x->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO || x->pNext) goto direct; ab = (const void *)x; }
    { size_t nc = DQ_ARR(VkClearValue, bi->pClearValues, bi->clearValueCount), nv = ab ? DQ_ARR(VkImageView, ab->pAttachments, ab->attachmentCount) : 0;
      DQ_BEGIN(OP_vkCmdBeginRenderPass, a_rpbegin, nc + nv); memset(a, 0, sizeof *a); a->bi = *bi; a->c = c;
      if (!bi->pClearValues) a->bi.clearValueCount = 0;
      if (ab) { a->ab = *ab; a->has_ab = 1; }
      dq_put(&at_, bi->pClearValues, nc); dq_put(&at_, ab ? ab->pAttachments : NULL, nv); dq_rec_end(base_); return; }
direct: DQ_DIRECT(vkCmdBeginRenderPass, cb, bi, c);
}
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    if (ri->pNext || (ri->pDepthAttachment && ri->pDepthAttachment->pNext) || (ri->pStencilAttachment && ri->pStencilAttachment->pNext)) goto direct;
    for (uint32_t i = 0; i < ri->colorAttachmentCount; i++) if (ri->pColorAttachments[i].pNext) goto direct;
    { DQ_BEGIN(OP_vkCmdBeginRendering, a_rbegin, DQ_ARR(VkRenderingAttachmentInfo, ri->pColorAttachments, ri->colorAttachmentCount)); memset(a, 0, sizeof *a); a->ri = *ri;
      if (!ri->pColorAttachments) a->ri.colorAttachmentCount = 0;
      if (ri->pDepthAttachment) { a->d = *ri->pDepthAttachment; a->has_d = 1; }
      if (ri->pStencilAttachment) { a->s = *ri->pStencilAttachment; a->has_s = 1; }
      dq_put(&at_, ri->pColorAttachments, DQ_ARR(VkRenderingAttachmentInfo, ri->pColorAttachments, ri->colorAttachmentCount)); dq_rec_end(base_); return; }
direct: DQ_DIRECT(vkCmdBeginRendering, cb, ri);
}
#define W_NOARG(f) static DQ_U VKAPI_ATTR void VKAPI_CALL w_##f(VkCommandBuffer cb) { DQ_COUNT(OP_##f); void *a = DQ_ON() ? dq_rec_begin(OP_##f, cb, 0) : NULL; if (!a) { DQ_DIRECT(f, cb); return; } DQ_WRITE_BOUNDARY(OP_##f); dq_rec_end(a); }
W_NOARG(vkCmdEndRenderPass) W_NOARG(vkCmdEndRendering)
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pl) {
    { DQ_BEGIN(OP_vkCmdBindPipeline, a_bindpipe, 0); *a = (a_bindpipe){bp, pl}; (void)at_; dq_rec_end(base_); return; } direct: DQ_DIRECT(vkCmdBindPipeline, cb, bp, pl); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *s, uint32_t nd, const uint32_t *d) {
    { DQ_BEGIN(OP_vkCmdBindDescriptorSets, a_sets, DQ_ARR(VkDescriptorSet, s, n) + DQ_ARR(uint32_t, d, nd)); *a = (a_sets){bp, l, first, n, d ? nd : 0};
      dq_put(&at_, s, DQ_ARR(VkDescriptorSet, s, n)); dq_put(&at_, d, DQ_ARR(uint32_t, d, nd)); dq_rec_end(base_); return; }
    direct: DQ_DIRECT(vkCmdBindDescriptorSets, cb, bp, l, first, n, s, nd, d); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    { DQ_BEGIN(OP_vkCmdBindVertexBuffers, a_range, DQ_ARR(VkBuffer, b, n) + DQ_ARR(VkDeviceSize, o, n)); *a = (a_range){first, n}; dq_put(&at_, b, DQ_ARR(VkBuffer, b, n)); dq_put(&at_, o, DQ_ARR(VkDeviceSize, o, n)); dq_rec_end(base_); return; }
    direct: DQ_DIRECT(vkCmdBindVertexBuffers, cb, first, n, b, o); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkIndexType t) {
    { DQ_BEGIN(OP_vkCmdBindIndexBuffer, a_index, 0); *a = (a_index){b, off, t}; (void)at_; dq_rec_end(base_); return; } direct: DQ_DIRECT(vkCmdBindIndexBuffer, cb, b, off, t); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) {
    { DQ_BEGIN(OP_vkCmdSetBlendConstants, a_blend, 0); memcpy(a->c, c, sizeof a->c); (void)at_; dq_rec_end(base_); return; } direct: DQ_DIRECT(vkCmdSetBlendConstants, cb, c); }
#define W_STENCIL(f) static DQ_U VKAPI_ATTR void VKAPI_CALL w_##f(VkCommandBuffer cb, VkStencilFaceFlags fl, uint32_t v) { \
    { DQ_BEGIN(OP_##f, a_stencil, 0); *a = (a_stencil){fl, v}; (void)at_; dq_rec_end(base_); return; } direct: DQ_DIRECT(f, cb, fl, v); }
W_STENCIL(vkCmdSetStencilReference) W_STENCIL(vkCmdSetStencilCompareMask) W_STENCIL(vkCmdSetStencilWriteMask)
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdSetDepthBias(VkCommandBuffer cb, float c, float clamp, float slope) {
    { DQ_BEGIN(OP_vkCmdSetDepthBias, a_bias, 0); *a = (a_bias){c, clamp, slope}; (void)at_; dq_rec_end(base_); return; } direct: DQ_DIRECT(vkCmdSetDepthBias, cb, c, clamp, slope); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdSetLineWidth(VkCommandBuffer cb, float w) {
    { DQ_BEGIN(OP_vkCmdSetLineWidth, float, 0); *a = w; (void)at_; dq_rec_end(base_); return; } direct: DQ_DIRECT(vkCmdSetLineWidth, cb, w); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    { DQ_BEGIN(OP_vkCmdDraw, a_draw, 0); *a = (a_draw){vc, ic, fv, fi, 0, 0}; (void)at_; dq_rec_end(base_); return; } direct: DQ_DIRECT(vkCmdDraw, cb, vc, ic, fv, fi); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t n, uint32_t fi, int32_t vo, uint32_t fin) {
    { DQ_BEGIN(OP_vkCmdDrawIndexed, a_draw, 0); *a = (a_draw){ic, n, fi, 0, vo, fin}; (void)at_; dq_rec_end(base_); return; } direct: DQ_DIRECT(vkCmdDrawIndexed, cb, ic, n, fi, vo, fin); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void *v) {
    { DQ_BEGIN(OP_vkCmdPushConstants, a_push, size); *a = (a_push){l, st, off, size}; dq_put(&at_, v, size); dq_rec_end(base_); return; } direct: DQ_DIRECT(vkCmdPushConstants, cb, l, st, off, size, v); }
#define W_INDIRECT(f) static DQ_U VKAPI_ATTR void VKAPI_CALL w_##f(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, uint32_t n, uint32_t stride) { \
    { DQ_BEGIN(OP_##f, a_indirect, 0); *a = (a_indirect){b, off, n, stride}; (void)at_; dq_rec_end(base_); return; } direct: DQ_DIRECT(f, cb, b, off, n, stride); }
W_INDIRECT(vkCmdDrawIndirect) W_INDIRECT(vkCmdDrawIndexedIndirect)
#define W_INDCOUNT(f) static DQ_U VKAPI_ATTR void VKAPI_CALL w_##f(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkBuffer c, VkDeviceSize coff, uint32_t max, uint32_t stride) { \
    { DQ_BEGIN(OP_##f, a_indcount, 0); *a = (a_indcount){b, off, c, coff, max, stride}; (void)at_; dq_rec_end(base_); return; } direct: DQ_DIRECT(f, cb, b, off, c, coff, max, stride); }
W_INDCOUNT(vkCmdDrawIndirectCount) W_INDCOUNT(vkCmdDrawIndexedIndirectCount)
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    { DQ_BEGIN(OP_vkCmdDispatch, a_draw, 0); *a = (a_draw){x, y, z, 0, 0, 0}; (void)at_; dq_rec_end(base_); return; } direct: DQ_DIRECT(vkCmdDispatch, cb, x, y, z); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdDispatchIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off) {
    { DQ_BEGIN(OP_vkCmdDispatchIndirect, a_indirect, 0); *a = (a_indirect){b, off, 0, 0}; (void)at_; dq_rec_end(base_); return; } direct: DQ_DIRECT(vkCmdDispatchIndirect, cb, b, off); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool p, uint32_t first, uint32_t n) {
    { DQ_BEGIN(OP_vkCmdResetQueryPool, a_qreset, 0); *a = (a_qreset){p, first, n}; (void)at_; dq_rec_end(base_); return; } direct: DQ_DIRECT(vkCmdResetQueryPool, cb, p, first, n); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits st, VkQueryPool p, uint32_t q) {
    { DQ_BEGIN(OP_vkCmdWriteTimestamp, a_ts, 0); *a = (a_ts){st, p, q}; (void)at_; dq_rec_end(base_); return; } direct: DQ_DIRECT(vkCmdWriteTimestamp, cb, st, p, q); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdBindTransformFeedbackBuffersEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o, const VkDeviceSize *sz) {
    { DQ_BEGIN(OP_vkCmdBindTransformFeedbackBuffersEXT, a_xfb, DQ_ARR(VkBuffer, b, n) + DQ_ARR(VkDeviceSize, o, n) + DQ_ARR(VkDeviceSize, sz, n)); *a = (a_xfb){first, n, sz != NULL, 1, 1};
      dq_put(&at_, b, DQ_ARR(VkBuffer, b, n)); dq_put(&at_, o, DQ_ARR(VkDeviceSize, o, n)); dq_put(&at_, sz, DQ_ARR(VkDeviceSize, sz, n)); dq_rec_end(base_); return; }
    direct: DQ_DIRECT(vkCmdBindTransformFeedbackBuffersEXT, cb, first, n, b, o, sz); }
#define W_XFB(f) static DQ_U VKAPI_ATTR void VKAPI_CALL w_##f(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) { \
    { DQ_BEGIN(OP_##f, a_xfb, DQ_ARR(VkBuffer, b, n) + DQ_ARR(VkDeviceSize, o, n)); *a = (a_xfb){first, n, 0, b != NULL, o != NULL}; \
      dq_put(&at_, b, DQ_ARR(VkBuffer, b, n)); dq_put(&at_, o, DQ_ARR(VkDeviceSize, o, n)); dq_rec_end(base_); return; } direct: DQ_DIRECT(f, cb, first, n, b, o); }
W_XFB(vkCmdBeginTransformFeedbackEXT) W_XFB(vkCmdEndTransformFeedbackEXT)
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdDrawIndirectByteCountEXT(VkCommandBuffer cb, uint32_t inst, uint32_t first_inst, VkBuffer b, VkDeviceSize off, uint32_t ctr_off, uint32_t stride) {
    { DQ_BEGIN(OP_vkCmdDrawIndirectByteCountEXT, a_bytecount, 0); *a = (a_bytecount){inst, first_inst, b, off, ctr_off, stride}; (void)at_; dq_rec_end(base_); return; }
    direct: DQ_DIRECT(vkCmdDrawIndirectByteCountEXT, cb, inst, first_inst, b, off, ctr_off, stride); }

static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdBeginQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags fl) {
    { DQ_BEGIN(OP_vkCmdBeginQuery, a_query, 0); *a = (a_query){p, q, fl}; (void)at_; dq_rec_end(base_); return; } direct: DQ_DIRECT(vkCmdBeginQuery, cb, p, q, fl); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkCmdEndQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q) {
    { DQ_BEGIN(OP_vkCmdEndQuery, a_query, 0); *a = (a_query){p, q, 0}; (void)at_; dq_rec_end(base_); return; } direct: DQ_DIRECT(vkCmdEndQuery, cb, p, q); }

// 占着命令池或队列的调用：先等队列里的调用全做完
static void dq_poll(void);
static DQ_U VKAPI_ATTR VkResult VKAPI_CALL w_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) { NVK_DQ_SEGMENT_BOUNDARY(); vk_dq_drain(); return vkBeginCommandBuffer(cb, bi); }
static DQ_U VKAPI_ATTR VkResult VKAPI_CALL w_vkEndCommandBuffer(VkCommandBuffer cb) { NVK_DQ_SEGMENT_BOUNDARY(); vk_dq_drain(); return vkEndCommandBuffer(cb); }
static DQ_U VKAPI_ATTR VkResult VKAPI_CALL w_vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *cb) { vk_dq_drain(); return vkAllocateCommandBuffers(d, ai, cb); }
static DQ_U VKAPI_ATTR VkResult VKAPI_CALL w_vkResetCommandPool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) { vk_dq_drain(); return vkResetCommandPool(d, p, f); }
static DQ_U VKAPI_ATTR VkResult VKAPI_CALL w_vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f) { vk_dq_drain(); dq_poll(); return vkQueueSubmit(q, n, si, f); }
static DQ_U VKAPI_ATTR VkResult VKAPI_CALL w_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi) { vk_dq_drain(); return vkQueuePresentKHR(q, pi); }
static DQ_U VKAPI_ATTR VkResult VKAPI_CALL w_vkQueueWaitIdle(VkQueue q) { vk_dq_drain(); return vkQueueWaitIdle(q); }
static DQ_U VKAPI_ATTR VkResult VKAPI_CALL w_vkDeviceWaitIdle(VkDevice d) { vk_dq_drain(); return vkDeviceWaitIdle(d); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkDestroyCommandPool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) { vk_dq_drain(); vkDestroyCommandPool(d, p, a); }
static DQ_U VKAPI_ATTR void VKAPI_CALL w_vkFreeCommandBuffers(VkDevice d, VkCommandPool p, uint32_t n, const VkCommandBuffer *cb) { vk_dq_drain(); vkFreeCommandBuffers(d, p, n, cb); }
static DQ_U VKAPI_ATTR VkResult VKAPI_CALL w_vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f) { NVK_DQ_SEGMENT_BOUNDARY(); vk_dq_drain(); return vkResetCommandBuffer(cb, f); }

// 录制线程等活的调试开关（见 g_dq_spin 处）：开会话时读一次，之后跟着 dq_poll 每 64 次提交再读（用例也靠这个在运行中切）
static void dq_tune(void) {
    atomic_store_explicit(&g_dq_spin, nvn_plat_flag("nvk_dq_spin", 200), memory_order_relaxed);
    const int poll_us = nvn_plat_flag("nvk_dq_poll_us", 0);
    if (atomic_exchange_explicit(&g_dq_poll_us, poll_us, memory_order_relaxed) != poll_us) dq_wake();   // 睡法变了：叫醒睡着的录制线程，它每圈重读睡法，醒来就按新的睡
    const int kb = nvn_plat_flag("nvk_dq_wake_kb", 0);
    atomic_store_explicit(&g_dq_wake_bytes, kb > 0 ? (uint64_t)kb << 10 : 0, memory_order_relaxed);
    atomic_store_explicit(&g_dq_stat, nvn_plat_flag("nvk_gputime", 0) != 0, memory_order_relaxed);
}
// 开关（调试属性 nvk_dq，缺省开——11-17 旷野之息神庙场景调试包关 / 开 / 关交错：回放每帧 39.2 / 28.1 / 39.0 ms、帧率 18.4 / 23.5 / 18.4）：在提交处读——那里队列刚清空；别的线程这时还有没做完的记录也没关系，它之后的直接调用会先清空队列。
static void dq_poll(void) {
    static _Atomic uint32_t n; if (!g_dq_ring || (atomic_fetch_add_explicit(&n, 1, memory_order_relaxed) & 63)) return;
    dq_tune();
    int on = nvn_plat_flag("nvk_dq", 1) != 0;
    if (on != DQ_ON()) { atomic_store_explicit(&g_dq_on, on, memory_order_relaxed); VKLOG("两级回放%s（debug.s2a.nvk_dq）", on ? "开：vkCmd* 调用交给录制线程" : "关：回放线程自己录"); }
}
// 设备建好之后、第一处 Vulkan 命令之前调一次。取不到环内存就一直按「关」走。
static void vk_dq_init(VkDevice dev) {
#define DQ_GET(f) r_##f = (PFN_##f)vkGetDeviceProcAddr(dev, #f);
    DQ_CMDS(DQ_GET)
    // The instance requests Vulkan 1.1: use the explicitly enabled KHR aliases.
    r_vkCmdDrawIndirectCount = (PFN_vkCmdDrawIndirectCount)vkGetDeviceProcAddr(dev, "vkCmdDrawIndirectCountKHR");
    r_vkCmdDrawIndexedIndirectCount = (PFN_vkCmdDrawIndexedIndirectCount)vkGetDeviceProcAddr(dev, "vkCmdDrawIndexedIndirectCountKHR");
    dq_tune();   // 每次开会话都读
    if (g_dq_ring) return;   // 会话重开：队列和线程沿用
    pthread_spin_init(&g_dq_pl, PTHREAD_PROCESS_PRIVATE);
    uint8_t *ring = malloc(dq_RING);
    if (!ring) { VKLOG("【红】两级回放：队列内存分配失败，回放线程自己录"); return; }
    { pthread_condattr_t ca; pthread_condattr_init(&ca); pthread_condattr_setclock(&ca, CLOCK_MONOTONIC); pthread_cond_init(&g_dq_cv, &ca); pthread_condattr_destroy(&ca); }   // 录制线程起来之前：没有人睡在它上面
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &at, dq_recorder_main, NULL);
    pthread_attr_destroy(&at);
    if (rc) { free(ring); VKLOG("【红】两级回放：录制线程起不来，回放线程自己录"); return; }
    g_dq_rec_th = t;
    g_dq_ring = ring;
    atomic_store_explicit(&g_dq_on, nvn_plat_flag("nvk_dq", 1) != 0, memory_order_relaxed);   // 起始值；之后由 dq_poll 在提交处跟着开关变
}

// 销毁：队列里可能还有引用这个对象的调用没做（录了命令、没提交就放弃的那一段——同步模型下录完再销毁无害，这里得先把队列做完）。
// 队列空时 vk_dq_drain 只是一次比较。原文没有这一层：它的引擎把资源销毁都推迟到槽退役之后。
#define DQ_DESTROY(f, T) static DQ_U VKAPI_ATTR void VKAPI_CALL w_##f(VkDevice d, T o, const VkAllocationCallbacks *a) { vk_dq_drain(); f(d, o, a); }
DQ_DESTROY(vkDestroyBuffer, VkBuffer)
DQ_DESTROY(vkDestroyImage, VkImage)
DQ_DESTROY(vkDestroyImageView, VkImageView)
DQ_DESTROY(vkDestroyBufferView, VkBufferView)
DQ_DESTROY(vkDestroyFramebuffer, VkFramebuffer)
DQ_DESTROY(vkDestroyRenderPass, VkRenderPass)
DQ_DESTROY(vkDestroyPipeline, VkPipeline)
DQ_DESTROY(vkDestroyPipelineLayout, VkPipelineLayout)
DQ_DESTROY(vkDestroyDescriptorPool, VkDescriptorPool)
DQ_DESTROY(vkDestroyQueryPool, VkQueryPool)
DQ_DESTROY(vkDestroySampler, VkSampler)
DQ_DESTROY(vkFreeMemory, VkDeviceMemory)
DQ_DESTROY(vkDestroySemaphore, VkSemaphore)
DQ_DESTROY(vkDestroyFence, VkFence)
DQ_DESTROY(vkDestroyDescriptorSetLayout, VkDescriptorSetLayout)
DQ_DESTROY(vkDestroyShaderModule, VkShaderModule)
static DQ_U VKAPI_ATTR VkResult VKAPI_CALL w_vkFreeDescriptorSets(VkDevice d, VkDescriptorPool p, uint32_t n, const VkDescriptorSet *s) { vk_dq_drain(); return vkFreeDescriptorSets(d, p, n, s); }
static DQ_U VKAPI_ATTR VkResult VKAPI_CALL w_vkResetDescriptorPool(VkDevice d, VkDescriptorPool p, VkDescriptorPoolResetFlags f) { vk_dq_drain(); return vkResetDescriptorPool(d, p, f); }

uint64_t nvn_vk_dbg_dq_recs(void) { return __atomic_load_n(&g_dq_recs, __ATOMIC_RELAXED); }   // 用例读：至今交给录制线程的调用数
uint64_t nvn_vk_dbg_dq_stat(int i) { return i >= 0 && i < DQS_N ? __atomic_load_n(&g_dq_st[i], __ATOMIC_RELAXED) : 0; }   // 用例读：等活读数（见 DQS_*；nvk_gputime 开着才计）
int nvn_vk_dbg_dq_thread_cpu_ns(uint64_t *ns) {   // 用例读：录制线程至今用掉的 CPU 纳秒（没起来返回 -1）
    clockid_t cid; struct timespec ts;
    if (!g_dq_ring || pthread_getcpuclockid(g_dq_rec_th, &cid) || clock_gettime(cid, &ts)) return -1;
    *ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; return 0;
}

// ---- 从这里往下，后端里这些名字都走包装 ----
#define vkBeginCommandBuffer w_vkBeginCommandBuffer
#define vkEndCommandBuffer w_vkEndCommandBuffer
#define vkAllocateCommandBuffers w_vkAllocateCommandBuffers
#define vkResetCommandPool w_vkResetCommandPool
#define vkQueueSubmit w_vkQueueSubmit
#define vkQueuePresentKHR w_vkQueuePresentKHR
#define vkQueueWaitIdle w_vkQueueWaitIdle
#define vkDeviceWaitIdle w_vkDeviceWaitIdle
#define vkDestroyCommandPool w_vkDestroyCommandPool
#define vkFreeCommandBuffers w_vkFreeCommandBuffers
#define vkResetCommandBuffer w_vkResetCommandBuffer
#define vkCmdPipelineBarrier w_vkCmdPipelineBarrier
#define vkCmdWriteTimestamp w_vkCmdWriteTimestamp
#define vkCmdPushConstants w_vkCmdPushConstants
#define vkCmdBlitImage w_vkCmdBlitImage
#define vkCmdClearColorImage w_vkCmdClearColorImage
#define vkCmdBindPipeline w_vkCmdBindPipeline
#define vkCmdResetQueryPool w_vkCmdResetQueryPool
#define vkCmdEndQuery w_vkCmdEndQuery
#define vkCmdDrawIndirectCount w_vkCmdDrawIndirectCount
#define vkCmdDrawIndexedIndirectCount w_vkCmdDrawIndexedIndirectCount
#define vkCmdDrawIndirect w_vkCmdDrawIndirect
#define vkCmdDrawIndexedIndirect w_vkCmdDrawIndexedIndirect
#define vkCmdDispatch w_vkCmdDispatch
#define vkCmdCopyImage w_vkCmdCopyImage
#define vkCmdCopyBufferToImage w_vkCmdCopyBufferToImage
#define vkCmdCopyBuffer w_vkCmdCopyBuffer
#define vkCmdBindDescriptorSets w_vkCmdBindDescriptorSets
#define vkCmdSetViewport w_vkCmdSetViewport
#define vkCmdSetStencilWriteMask w_vkCmdSetStencilWriteMask
#define vkCmdSetStencilReference w_vkCmdSetStencilReference
#define vkCmdSetStencilCompareMask w_vkCmdSetStencilCompareMask
#define vkCmdSetScissor w_vkCmdSetScissor
#define vkCmdSetDepthBias w_vkCmdSetDepthBias
#define vkCmdSetBlendConstants w_vkCmdSetBlendConstants
#define vkCmdResolveImage w_vkCmdResolveImage
#define vkCmdFillBuffer w_vkCmdFillBuffer
#define vkCmdEndRenderPass w_vkCmdEndRenderPass
#define vkCmdDrawIndexed w_vkCmdDrawIndexed
#define vkCmdDraw w_vkCmdDraw
#define vkCmdDispatchIndirect w_vkCmdDispatchIndirect
#define vkCmdCopyImageToBuffer w_vkCmdCopyImageToBuffer
#define vkCmdClearDepthStencilImage w_vkCmdClearDepthStencilImage
#define vkCmdBindVertexBuffers w_vkCmdBindVertexBuffers
#define vkCmdBindIndexBuffer w_vkCmdBindIndexBuffer
#define vkCmdBeginRenderPass w_vkCmdBeginRenderPass
#define vkCmdBeginQuery w_vkCmdBeginQuery
#define vkCmdUpdateBuffer w_vkCmdUpdateBuffer
#define vkCmdClearAttachments w_vkCmdClearAttachments
#define vkCmdBeginRendering w_vkCmdBeginRendering
#define vkCmdEndRendering w_vkCmdEndRendering
#define vkCmdBindTransformFeedbackBuffersEXT w_vkCmdBindTransformFeedbackBuffersEXT
#define vkCmdBeginTransformFeedbackEXT w_vkCmdBeginTransformFeedbackEXT
#define vkCmdEndTransformFeedbackEXT w_vkCmdEndTransformFeedbackEXT
#define vkCmdDrawIndirectByteCountEXT w_vkCmdDrawIndirectByteCountEXT
#define vkCmdSetLineWidth w_vkCmdSetLineWidth
#define vkDestroyBuffer w_vkDestroyBuffer
#define vkDestroyImage w_vkDestroyImage
#define vkDestroyImageView w_vkDestroyImageView
#define vkDestroyBufferView w_vkDestroyBufferView
#define vkDestroyFramebuffer w_vkDestroyFramebuffer
#define vkDestroyRenderPass w_vkDestroyRenderPass
#define vkDestroyPipeline w_vkDestroyPipeline
#define vkDestroyPipelineLayout w_vkDestroyPipelineLayout
#define vkDestroyDescriptorPool w_vkDestroyDescriptorPool
#define vkDestroyQueryPool w_vkDestroyQueryPool
#define vkDestroySampler w_vkDestroySampler
#define vkFreeMemory w_vkFreeMemory
#define vkDestroySemaphore w_vkDestroySemaphore
#define vkDestroyFence w_vkDestroyFence
#define vkDestroyDescriptorSetLayout w_vkDestroyDescriptorSetLayout
#define vkDestroyShaderModule w_vkDestroyShaderModule
#define vkFreeDescriptorSets w_vkFreeDescriptorSets
#define vkResetDescriptorPool w_vkResetDescriptorPool
