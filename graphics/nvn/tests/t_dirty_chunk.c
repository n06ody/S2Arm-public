// 写跟踪「分块保护」单测：块内共享、跨块写入、保护失败保守路径、mprotect 区间必须块对齐。
// 反例来源（真机）：32 MiB AHB 初始 1 条 VMA，逐页 mprotect 后 65 条且恢复不合并，
// 世界里 VMA 涨到 vm.max_map_count 后 mmap 失败。这里用钩子核对“实际下发的 mprotect 区间”。
#include "../gfx/nvn/nvn_dirty.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static int (*g_hook)(void *, int, void *);
static void (*g_prep)(void *, size_t);
static volatile sig_atomic_t g_read_operation;
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) { g_hook = fn; }
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) { g_prep = fn; }
static void host_prepare(void *addr, size_t len) { if (g_prep) g_prep(addr, len); }   // 宿主写前解保护入口（生产是 dm_guest_write_prepare）
void dm_log(const char *fmt, ...) { (void)fmt; }
static void on_sig(int s, siginfo_t *si, void *uc) { if (g_hook && g_hook(si->si_addr, !g_read_operation, uc)) return; signal(s, SIG_DFL); }

extern int (*nvn_dirty_mp_hook)(void *addr, size_t len, int prot);

static int fails, checks;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// 记录 mprotect 区间；可注入失败
static uintptr_t rec_addr[256], rec_end[256];
static int rec_n, inject_fail;
static int rec_hook(void *addr, size_t len, int prot) {
    (void)prot;
    uintptr_t a = (uintptr_t)addr;
    // 失败在真正的 mprotect 之前就返回，避免宿主上把内存改成只读造成误判
    if (inject_fail) return 1;
    if (rec_n < 256) { rec_addr[rec_n] = a; rec_end[rec_n] = a + len; rec_n++; }
    return 0;
}

static void test_land_probe(void) {
    size_t pg = (size_t)getpagesize();
    uint8_t *m = mmap(NULL, 8 * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CK(m != MAP_FAILED, "read probe mapping");
    if (m == MAP_FAILED) return;
    memset(m, 0x51, 8 * pg);
    nvn_dirty_add_range(m, 8 * pg);
    nvn_dirty_protect(m, 8 * pg);
    const uint64_t stamp = nvn_dirty_now();
    nvn_dirty_land_probe_arm(m + 17, 2 * pg, 0);   // host page 1 only
    volatile uint8_t *read_ptr = m;
    g_read_operation = 1;
    uint8_t outside = read_ptr[17], first = read_ptr[pg + 17], second = read_ptr[pg + 19];
    g_read_operation = 0;
    uint64_t counts[5] = {0};
    nvn_dirty_land_probe_counts(counts);
    CK(outside == 0x51 && first == 0x51 && second == 0x51, "probe read preserves the original guest byte");
    CK(counts[0] == 1 && counts[1] == 1 && counts[2] == 0 && counts[3] == 1, "one real read fault on the single wholly covered page");
    CK(!nvn_dirty_since(m + pg, pg, stamp), "read did not mark the page dirty");
    m[pg] = 0x33;
    CK(nvn_dirty_since(m + pg, pg, stamp), "write still faults and stamps after a probe read");
    nvn_dirty_land_probe_arm(m + 4 * pg, 2 * pg, 0);
    nvn_dirty_land_probe_finish();
    nvn_dirty_land_probe_counts(counts);
    CK(counts[1] == 2 && counts[3] == 1 && counts[4] == 1, "unobserved probe expires without counting a read");
    g_read_operation = 1;
    CK(read_ptr[4 * pg] == 0x51, "window end restores readability");
    g_read_operation = 0;
    nvn_dirty_remove_range(m);
    munmap(m, 8 * pg);
}

int main(void) {
    struct sigaction sa = {0}; sa.sa_sigaction = on_sig; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, NULL); sigaction(SIGSEGV, &sa, NULL);
    test_land_probe();
    nvn_dirty_mp_hook = rec_hook;

    const size_t pg = (size_t)getpagesize();
    // 分块 = 4 页（测试用小值；安卓生产默认 1 MiB，共享默认 0 = 逐页）
    nvn_dirty_set_chunk_pages(4);
    CK(nvn_dirty_chunk_pages() == 4, "分块页数可设置");

    uint8_t *m = mmap(NULL, 64 * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    nvn_dirty_add_range(m, 64 * pg);
    const void *span = nvn_dirty_span(m, 64 * pg);
    CK(span != NULL, "登记跟踪范围");

    // --- 反例1：两张纹理共块，写 A 后 B 必须视为脏 ---
    uint8_t *texA = m + 0 * pg;          // 块0 内
    uint8_t *texB = m + 2 * pg;          // 同一块0（4 页一块）
    nvn_dirty_protect(texA, pg);
    nvn_dirty_protect(texB, pg);
    CK(rec_n >= 1, "下发了 mprotect");
    // 每个下发区间都必须按块对齐、且不越界
    int aligned = 1;
    for (int i = 0; i < rec_n; i++) {
        if ((rec_addr[i] - (uintptr_t)m) % (4 * pg) != 0) aligned = 0;
        if (rec_addr[i] < (uintptr_t)m || rec_end[i] > (uintptr_t)m + 64 * pg) aligned = 0;
    }
    CK(aligned, "所有 mprotect 区间都按分块边界对齐且在映射内");
    uint64_t stamp = nvn_dirty_now();
    texA[0] = 0x5A;                       // 触发缺页 → 整块解除 + 整块记脏
    CK(nvn_dirty_since(texA, pg, stamp) == 1, "写 A 后 A 变脏");
    CK(nvn_dirty_since(texB, pg, stamp) == 1, "共用同一块的 B 也必须变脏（否则 B 会读到旧内容）");

    // --- 反例2：跨块写入，两块都要脏 ---
    uint64_t st2 = nvn_dirty_now();
    uint8_t *cross = m + 3 * pg;          // 覆盖块0末页与块1首数页
    nvn_dirty_protect(m + 4 * pg, 8 * pg);
    nvn_dirty_protect(m + 3 * pg, pg);
    for (size_t i = 0; i < 6 * pg; i++) cross[i] = (uint8_t)i;
    CK(nvn_dirty_since(m + 3 * pg, pg, st2) == 1, "跨块写入：块0 末页变脏");
    CK(nvn_dirty_since(m + 4 * pg, pg, st2) == 1, "跨块写入：块1 首 4 页变脏");

        // 负对照：只换底、不 replace（旧行为）→ 写入不触发异常、判脏=否。
        // 这条断言用来证明反例对缺陷敏感，而不是“怎么改都过”。
        uint8_t *nc = mmap(NULL, 16 * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        nvn_dirty_add_range(nc, 16 * pg);
        nvn_dirty_protect(nc, 16 * pg);
        uint64_t stn = nvn_dirty_now();
        mprotect(nc, 16 * pg, PROT_READ | PROT_WRITE);   // 换底后新映射可读写，但没重建保护
        nc[0] = 0x44;
        CK(nvn_dirty_since(nc, pg, stn) == 0, "负对照：不 replace 时写入漏检（旧缺陷可复现）");
        nvn_dirty_remove_range(nc);
        munmap(nc, 16 * pg);

    // --- 反例4：AHB 换底（旧映射曾保护 → 新映射实际可读写）必须重建保护、重新判脏 ---
    {
        // 前提：此时跟踪必须仍可靠。否则“换底后判脏”会被恒脏状态掩盖，测试会误报通过
        // （维护者把 replace_range 换成空实现后测试仍全绿，就是这个原因）。
        CK(nvn_dirty_tracking_unreliable() == 0, "换底反例前提：跟踪必须仍可靠");
        uint8_t *sw = mmap(NULL, 16 * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        nvn_dirty_add_range(sw, 16 * pg);
        nvn_dirty_protect(sw, 16 * pg);
        uint64_t stx = nvn_dirty_now();
        // 换底：新映射是可读写的（对应 MAP_FIXED 换成 AHB 之后的状态）
        mprotect(sw, 16 * pg, PROT_READ | PROT_WRITE);
        CK(nvn_dirty_replace_range(sw, 16 * pg) == 1, "换底替换已有跟踪条目（不是追加）");
        CK(nvn_dirty_since(sw, pg, stx) == 1, "换底后重新验证为脏");
        sw[0] = 0x33;    // 必须重新触发写异常（旧实现这里不触发，代际不变）
        CK(nvn_dirty_since(sw, pg, stx) == 1, "换底后写入必须被检测到（漏更新缺陷回归）");
        nvn_dirty_remove_range(sw);
        munmap(sw, 16 * pg);

    }

    // --- 反例6：同址重复登记只追加、不重加保护（共享 add_range 不再隐式转向 replace）---
    {
        uint8_t *dup = mmap(NULL, 16 * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        nvn_dirty_add_range(dup, 16 * pg);
        nvn_dirty_protect(dup, 16 * pg);
        int rec_before = rec_n;
        nvn_dirty_add_range(dup, 16 * pg);   // 同址第二次登记
        CK(nvn_dirty_span(dup, 16 * pg) != NULL, "同地址重复登记后仍可查到范围");
        CK(rec_n == rec_before, "同地址重复登记不改变保护（mprotect 0 次）");
        nvn_dirty_remove_range(dup); nvn_dirty_remove_range(dup);   // 两条都清掉
        munmap(dup, 16 * pg);
    }

    // --- 反例5：换底长度与登记长度不同：不能按旧 end 重加保护，必须删除 + 重新登记 ---
    {
        uint8_t *rs = mmap(NULL, 32 * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        nvn_dirty_add_range(rs, 16 * pg);
        nvn_dirty_protect(rs, 16 * pg);
        CK(nvn_dirty_replace_range(rs, 32 * pg) == 1, "换底长度不等：走删除 + 重新登记");
        CK(nvn_dirty_tracked(rs, 32 * pg) == 1, "重新登记后新长度整段可查");
        // 重新登记后按新长度加保护，写入必须能检测到（不能被旧 end 的只读区间挡掉）
        nvn_dirty_protect(rs, 32 * pg);
        uint64_t st5 = nvn_dirty_now();
        rs[0] = 0x11;
        CK(nvn_dirty_since(rs, pg, st5) == 1, "重新登记并加保护后写入可检测");
        nvn_dirty_remove_range(rs);
        munmap(rs, 32 * pg);
    }

    // --- 反例7：分块 = 0（苹果路径）时，宿主整段写前解保护必须合并成一次 mprotect ---
    // 维护方 Mac 实跑：旧实现逐块调 unprotect_span_pages，分块=0 时 64 页整段写变成 64 次 mprotect；
    // main 的连续只读页合并是 1 次。安卓 1 MiB 分块下连续块也必须合并。
    {
        nvn_dirty_set_chunk_pages(0);
        uint8_t *bm = mmap(NULL, 64 * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        nvn_dirty_add_range(bm, 64 * pg);
        nvn_dirty_protect(bm, 64 * pg);
        uint64_t c0 = 0, p0 = 0, t0 = 0;
        nvn_dirty_mp_stats(&c0, &p0, &t0);
        host_prepare(bm, 64 * pg);
        uint64_t c1 = 0, p1 = 0, t1 = 0;
        nvn_dirty_mp_stats(&c1, &p1, &t1);
        CK(c1 - c0 == 1, "分块=0：整段宿主写前解保护只调 1 次 mprotect（当前逐页会是 64 次）");
        CK(p1 - p0 == 64, "分块=0：这一次 mprotect 覆盖 64 页");
        nvn_dirty_remove_range(bm);
        munmap(bm, 64 * pg);
        nvn_dirty_set_chunk_pages(4);
    }

    // --- 反例3：保护失败后，所有查询走保守路径 ---
    nvn_dirty_protect(m + 16 * pg, 4 * pg);
    inject_fail = 1;
    nvn_dirty_protect(m + 32 * pg, 4 * pg);   // 这次加保护会“失败”
    inject_fail = 0;
    CK(nvn_dirty_tracking_unreliable() == 1, "保护失败后置“跟踪不可靠”");
    CK(nvn_dirty_since(m + 32 * pg, pg, nvn_dirty_now()) == 1, "不可靠时查询一律视为脏（保守）");
    uint64_t gens[4];
    CK(nvn_dirty_page_gens(NULL, m + 32 * pg, 4 * pg, gens, 4) == -1, "不可靠时代际查询返回 -1");
    {
        uint64_t wb[4] = {0};
        CK(nvn_dirty_written_pages(NULL, m + 32 * pg, 4 * pg, 0, wb, 4 * 64) == -1,
           "不可靠时脏页位图返回未知（调用方必须整张上传，不能拿空位图跳过）");
    }

    nvn_dirty_remove_range(m);
    // 门禁按「✓ t_」认结论行（runtime/gate.sh）；失败走「✗ t_」。
    if (fails) { printf("✗ t_dirty_chunk：%d 处不符\n", fails); return 1; }
    printf("✓ t_dirty_chunk: %d checks, 0 failures\n", checks);
    return 0;
}
