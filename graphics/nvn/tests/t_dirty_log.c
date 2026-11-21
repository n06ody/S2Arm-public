// 脏页追加日志（#1501 A②，nvn_dirty_log_*）：真实 mprotect / 缺页回调 / 宿主写前准备，含「事件先于页变可写」的交错。
// Apple 宿主（16 KB 页）与 Linux 宿主都跑；安卓的回放核心按开关 debug.s2a.nvk_buf_dirtylog 消费这份日志（graphics/nvn/replay/rp_buf.c）。
#include "../gfx/nvn/nvn_dirty.h"
#include "s2a_t_result.h"
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void (*g_prep)(void *, size_t);
static int (*g_fault)(void *, int, void *);
static volatile sig_atomic_t g_fault_count;
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { g_fault = fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { g_prep = fn; }
void dm_log(const char *fmt, ...) { (void)fmt; }
static void on_sig(int sig, siginfo_t *si, void *ctx) {
    if (g_fault && g_fault(si->si_addr, 1, ctx)) { g_fault_count++; return; }
    signal(sig, SIG_DFL);
}
void nvn_dirty_reset_session(void);
extern int (*nvn_dirty_mp_hook)(void *, size_t, int);

static int checks, fails;
#define CK(expr) do { checks++; if (!(expr)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expr); } } while (0)
static _Atomic int g_hold, g_entered, g_release, g_fail_mp;
static _Atomic int g_prepare_waiting;
static _Thread_local int g_observe_prepare_wait;

// The test build redirects nvn_dirty.c's sched_yield only. Observe that the
// second operation really reached busy_wait before allowing the first to finish.
int dirty_test_sched_yield(void) {
    if (g_observe_prepare_wait) atomic_store(&g_prepare_waiting, 1);
    return sched_yield();
}
static int mp_hook(void *addr, size_t len, int prot) {   // 解保护（PROT_WRITE）进 mprotect 之前停住：此刻页还只读，写入还没发生
    (void)addr; (void)len;
    if (atomic_load(&g_fail_mp)) return 1;
    if ((prot & PROT_WRITE) && atomic_load(&g_hold)) {
        atomic_fetch_add(&g_entered, 1);
        while (!atomic_load(&g_release)) sched_yield();
    }
    return 0;
}
static void *writer(void *p) { *(volatile uint8_t *)p = 0x6d; return NULL; }   // 真缺页：回调解保护后重新执行这次写
static void *preparer(void *p) {
    g_observe_prepare_wait = 1;
    g_prep(p, 1);
    return NULL;
}

// 收事件：把 [a, b) 记成页位图（相对 mem，最多 256 页）
static uint8_t *g_mem; static size_t g_pg; static uint8_t g_hit[256]; static int g_events;
static void collect(void *user, uintptr_t a, uintptr_t b) {
    (void)user; g_events++;
    for (uintptr_t x = a; x < b; x += g_pg) { size_t k = (x - (uintptr_t)g_mem) / g_pg; if (x >= (uintptr_t)g_mem && k < 256) g_hit[k] = 1; }
}
static int read_log(const void *span, uint64_t id, uint64_t *cur) { memset(g_hit, 0, sizeof g_hit); g_events = 0; return nvn_dirty_log_read(span, id, cur, collect, NULL); }

int main(void) {
    struct sigaction sa = {0}; sa.sa_sigaction = on_sig; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, NULL); sigaction(SIGSEGV, &sa, NULL);
    size_t pg = (size_t)getpagesize(), total = 320 * pg; g_pg = pg;
    uint8_t *mapping = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mapping == MAP_FAILED) return 2;
    uint8_t *mem = (uint8_t *)(((uintptr_t)mapping + 64 * pg - 1) & ~(uintptr_t)(64 * pg - 1)); g_mem = mem;
    nvn_dirty_mp_hook = mp_hook;
    nvn_dirty_set_chunk_pages(0);   // 逐页保护（Apple 缺省）
    uint64_t id = 0, cur = 0;

    // 缺省关：池没有日志
    CK(nvn_dirty_log_configure(0));
    nvn_dirty_add_range(mem, 256 * pg);
    const void *span = nvn_dirty_span(mem, 256 * pg);
    CK(span && !nvn_dirty_log_open(span, &id, &cur));
    CK(!nvn_dirty_log_configure(1));   // 有活池时不能中途启用
    nvn_dirty_remove_range(mem); nvn_dirty_reset_session();

    // 开：缺页写一页 → 一条事件正好是这一页；写之前的页「只读」，写之后不是
    CK(nvn_dirty_log_configure(1));
    nvn_dirty_set_mp_dedupe(1);
    nvn_dirty_add_range(mem, 256 * pg); span = nvn_dirty_span(mem, 256 * pg);
    CK(span && nvn_dirty_log_open(span, &id, &cur));
    nvn_dirty_protect(mem, 256 * pg);
    CK(nvn_dirty_page_armed(span, mem + 10 * pg));
    CK(read_log(span, id, &cur) && g_events == 0);   // 只加保护不产生事件
    sig_atomic_t f0 = g_fault_count; mem[10 * pg] = 0x31;
    CK(g_fault_count == f0 + 1 && mem[10 * pg] == 0x31);
    CK(!nvn_dirty_page_armed(span, mem + 10 * pg));
    CK(read_log(span, id, &cur) && g_events == 1 && g_hit[10] && !g_hit[9] && !g_hit[11]);
    mem[10 * pg + 1] = 0x32;   // 已可写：不再缺页、不再有事件（两次加保护之间一页至多一条）
    CK(g_fault_count == f0 + 1 && read_log(span, id, &cur) && g_events == 0);
    nvn_dirty_protect(mem + 10 * pg, pg); CK(nvn_dirty_page_armed(span, mem + 10 * pg));

    // 宿主写前准备整段解除：一条事件覆盖整段
    g_prep(mem + 32 * pg, 16 * pg);
    CK(read_log(span, id, &cur) && g_hit[32] && g_hit[47] && !g_hit[48] && !g_hit[31]);

    // 事件先于页变可写：解保护停在 mprotect 之前时，读者已经看得到这一页的事件（写入只可能在它之后）
    atomic_store(&g_hold, 1); atomic_store(&g_entered, 0); atomic_store(&g_release, 0);
    pthread_t t1; if (pthread_create(&t1, NULL, writer, mem + 70 * pg)) return 2;
    while (atomic_load(&g_entered) != 1) sched_yield();
    CK(read_log(span, id, &cur) && g_hit[70]);
    CK(mem[70 * pg] != 0x6d);   // 写入还没发生
    pthread_t t2;
    if (pthread_create(&t2, NULL, preparer, mem + 70 * pg)) return 2;
    while (!atomic_load(&g_prepare_waiting)) sched_yield();
    atomic_store(&g_release, 1);
    pthread_join(t1, NULL); pthread_join(t2, NULL); atomic_store(&g_hold, 0);
    CK(mem[70 * pg] == 0x6d);
    CK(atomic_load(&g_entered) == 1);  // 第一个已经把页改成可写，第二个无需重复 mprotect。
    CK(read_log(span, id, &cur) && g_events == 0);
    nvn_dirty_protect(mem + 70 * pg, pg);
    const uint64_t rearmed = nvn_dirty_now();
    mem[70 * pg + 1] = 0x63;
    CK(nvn_dirty_since_span(span, mem + 70 * pg, pg, rearmed));
    CK(read_log(span, id, &cur) && g_events == 1 && g_hit[70]);

    // 读者游标之后只有自己没读过的：另一个游标（另一份镜像）各读各的
    uint64_t id2 = 0, cur2 = 0; CK(nvn_dirty_log_open(span, &id2, &cur2) && id2 == id);
    nvn_dirty_protect(mem + 80 * pg, pg); mem[80 * pg] = 1;
    CK(read_log(span, id, &cur) && g_hit[80]);
    CK(read_log(span, id2, &cur2) && g_hit[80]);
    CK(read_log(span, id, &cur) && g_events == 0);

    // 环绕：256 页的池日志容量 64 条，落后 70 条的游标判不可用；重新起游标就好
    uint64_t lag_id = id, lag = cur;
    for (int i = 0; i < 70; i++) { nvn_dirty_protect(mem + 100 * pg, pg); mem[100 * pg] = (uint8_t)i; }
    CK(!read_log(span, lag_id, &lag));
    CK(nvn_dirty_log_open(span, &id, &cur) && read_log(span, id, &cur) && g_events == 0);

    // 与别的池共用宿主页：两边的日志都作废
    nvn_dirty_add_range(mem + 255 * pg, 4 * pg);
    CK(!read_log(span, id, &cur) && !nvn_dirty_log_open(span, &id, &cur));
    nvn_dirty_remove_range(mem + 255 * pg);
    nvn_dirty_remove_range(mem);

    // 同址重新登记：新日志、新身份；旧游标对不上
    uint64_t old_id = id;
    nvn_dirty_add_range(mem, 256 * pg); span = nvn_dirty_span(mem, 256 * pg);
    CK(nvn_dirty_log_open(span, &id, &cur) && id != old_id);
    uint64_t c_old = cur; CK(!read_log(span, old_id, &c_old));
    // 换底：日志里没有「整段变了」，作废
    CK(nvn_dirty_replace_range(mem, 256 * pg));
    CK(!read_log(span, id, &cur) && !nvn_dirty_log_open(span, &id, &cur));
    nvn_dirty_remove_range(mem);

    // 跟踪不可靠（mprotect 失败过）：一律不可用、页不算只读。全局状态，放最后
    nvn_dirty_add_range(mem, 256 * pg); span = nvn_dirty_span(mem, 256 * pg);
    CK(nvn_dirty_log_open(span, &id, &cur));
    atomic_store(&g_fail_mp, 1); nvn_dirty_protect(mem, pg); atomic_store(&g_fail_mp, 0);
    CK(!read_log(span, id, &cur) && !nvn_dirty_log_open(span, &id, &cur) && !nvn_dirty_page_armed(span, mem + 5 * pg));
    nvn_dirty_remove_range(mem); nvn_dirty_reset_session();

    printf("t_dirty_log：%d 项检查，%d 项红\n", checks, fails);
    return S2A_T_RESULT("t_dirty_log", fails);
}
