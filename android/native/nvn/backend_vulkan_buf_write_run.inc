// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 连续暂存写共享首道全屏障。目标均为独立分配或同一零偏移绑定内的不相交区间。
// 生产端唯一续接令牌；另一生产线程或另一段的命令可以保守打断。只比较地址，跨线程从不访问 TLS 表。
enum { BW_RUN_LIMIT = 16 };
enum bw_result { BW_SKIP, BW_OVERLAP, BW_CAPACITY, BW_UNKNOWN, BW_INTERVENING, BW_SEGMENT, BW_N };
struct bw_range { VkBuffer buffer; VkDeviceMemory memory; uint64_t lo, hi; };
struct bw_run {
    VkCommandBuffer cb;
    unsigned count;
    struct bw_range ranges[BW_RUN_LIMIT];
};
static _Thread_local struct bw_run g_bw_local;
static _Atomic(struct bw_run *) g_bw_tail;
static int g_bw_enabled;
static _Atomic int g_bw_diagnostic;
static _Atomic uint64_t g_bw_counts[BW_N];
static uint64_t g_bw_last_present[BW_N];
static const char *const g_bw_names[BW_N] = {
    "buf_write_bar_skipped", "buf_write_bar_overlap", "buf_write_bar_capacity",
    "buf_write_bar_unknown", "buf_write_bar_intervening", "buf_write_bar_segment",
};

static void bw_count(enum bw_result why) {
    if (atomic_load_explicit(&g_bw_diagnostic, memory_order_relaxed))
        atomic_fetch_add_explicit(&g_bw_counts[why], 1, memory_order_relaxed);
}

static inline void bw_end(enum bw_result why) {
    if (__builtin_expect(atomic_load_explicit(&g_bw_tail, memory_order_relaxed) != NULL, 0) &&
        atomic_exchange_explicit(&g_bw_tail, NULL, memory_order_relaxed))
        bw_count(why);
}

static void bw_open(void) {
    atomic_store_explicit(&g_bw_tail, NULL, memory_order_relaxed);
    g_bw_enabled = nvn_plat_flag("nvk_buf_write_bar_coalesce", 0) != 0;
    atomic_store_explicit(&g_bw_diagnostic, g_bw_enabled && nvn_plat_flag("nvk_buf_write_bar_diag", 0) != 0,
                          memory_order_relaxed);
    for (unsigned i = 0; i < BW_N; i++) atomic_store_explicit(&g_bw_counts[i], 0, memory_order_relaxed);
    memset(g_bw_last_present, 0, sizeof g_bw_last_present);
}

// 消费令牌，临时撤销写串，使自身的全屏障和分块拷贝不被记成间插。只有录完写才重新发布。
static int bw_before(VkCommandBuffer cb, VkBuffer buffer, VkDeviceMemory memory, uint64_t lo, uint64_t bytes) {
    struct bw_run *run = &g_bw_local;
    struct bw_run *tail = run;
    if (run->cb != cb || !atomic_compare_exchange_strong_explicit(&g_bw_tail, &tail, NULL, memory_order_relaxed,
                                                               memory_order_relaxed)) run->count = 0;
    run->cb = cb;
    if (!cb || !buffer || !memory || !bytes || bytes > UINT64_MAX - lo) {
        run->count = 0;
        bw_count(BW_UNKNOWN);
        return 0;
    }
    const uint64_t hi = lo + bytes;
    if (run->count == BW_RUN_LIMIT) {
        run->count = 0;
        bw_count(BW_CAPACITY);
    }
    for (unsigned i = 0; i < run->count; i++) {
        const struct bw_range *prior = &run->ranges[i];
        if (prior->memory != memory) continue;
        if (prior->buffer != buffer) {
            // 同一分配的另一个句柄：没有它的绑定偏移证明。
            run->count = 0;
            bw_count(BW_UNKNOWN);
            return 0;
        }
        if (lo < prior->hi && prior->lo < hi) {
            run->count = 0;
            bw_count(BW_OVERLAP);
            break;
        }
    }
    const int skip = run->count != 0;
    run->ranges[run->count++] = (struct bw_range){buffer, memory, lo, hi};
    return skip;
}

static void bw_after(int skip) {
    if (skip) bw_count(BW_SKIP);
    if (g_bw_local.count) atomic_store_explicit(&g_bw_tail, &g_bw_local, memory_order_relaxed);
}
