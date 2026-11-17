// 写跟踪摘要：真实 mprotect / 宿主写前准备，含 Apple 逐页保护和并发交错。
#include "../gfx/nvn/nvn_dirty.h"
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
extern void (*nvn_dirty_block_test_after_now)(void);
extern int nvn_dirty_block_test_fail_alloc;

static int checks, fails;
#define CK(expr) do { checks++; if (!(expr)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expr); } } while (0)
static _Atomic int g_hold, g_entered, g_release, g_fail_mp;
static _Atomic int g_after_entered, g_after_release;
static void after_now(void) {
    atomic_store(&g_after_entered, 1);
    while (!atomic_load(&g_after_release)) sched_yield();
}
static int mp_hook(void *addr, size_t len, int prot) {
    (void)addr; (void)len;
    if (atomic_load(&g_fail_mp)) return 1;
    if ((prot & PROT_WRITE) && atomic_load(&g_hold)) {
        atomic_fetch_add(&g_entered, 1);
        while (!atomic_load(&g_release)) sched_yield();
    }
    return 0;
}
static void *writer(void *p) {
    g_prep(p, 1);
    *(uint8_t *)p = 0x6d;
    return NULL;
}
static int snap(const void *span, const void *p, size_t n, uint64_t *v, uint64_t *e) {
    return nvn_dirty_block_snapshot(span, p, n, v, e);
}
int main(void) {
    struct sigaction sa = {0}; sa.sa_sigaction = on_sig; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, NULL); sigaction(SIGSEGV, &sa, NULL);
    size_t pg = (size_t)getpagesize(), align = 64 * pg, total = 320 * pg;
    uint8_t *mapping = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mapping == MAP_FAILED) return 2;
    uint8_t *mem = (uint8_t *)(((uintptr_t)mapping + align - 1) & ~(uintptr_t)(align - 1));
    nvn_dirty_mp_hook = mp_hook;
    nvn_dirty_set_chunk_pages(0); // Apple 路径：保持逐页保护及现有宿主写前合并语义。

    CK(nvn_dirty_blocks_configure(0));
    nvn_dirty_add_range(mem, 256 * pg);
    const void *span = nvn_dirty_span(mem, 256 * pg);
    uint64_t v = 0, e = 0;
    CK(span && !snap(span, mem, pg, &v, &e)); // 默认关闭时没有摘要
    CK(!nvn_dirty_blocks_configure(1));         // 活池存在时不能中途启用
    nvn_dirty_protect(mem, 64 * pg);
    uint64_t c0, p0, ns, c1, p1;
    nvn_dirty_mp_stats(&c0, &p0, &ns); g_prep(mem, 64 * pg); nvn_dirty_mp_stats(&c1, &p1, &ns);
    CK(c1 - c0 == 1 && p1 - p0 == 64); // 关闭：原 Apple 路径一次合并解除
    nvn_dirty_remove_range(mem); nvn_dirty_reset_session();

    CK(nvn_dirty_blocks_configure(1));
    nvn_dirty_add_range(mem, 256 * pg); span = nvn_dirty_span(mem, 256 * pg);
    nvn_dirty_protect(mem, 64 * pg);
    nvn_dirty_mp_stats(&c0, &p0, &ns); g_prep(mem, 64 * pg); nvn_dirty_mp_stats(&c1, &p1, &ns);
    CK(c1 - c0 == 1 && p1 - p0 == 64); // 开启摘要也不改变 Apple 保护合并
    uint64_t v0 = 0, e0 = 0, v1 = 0, e1 = 0, v2 = 0, e2 = 0;
    CK(span && snap(span, mem, pg, &v0, &e0));
    CK(snap(span, mem + 70 * pg, pg, &v1, &e1) && e1 == e0);
    CK(!snap(span, mem + 63 * pg, 2 * pg, &v, &e)); // 跨块不能伪造快照
    nvn_dirty_protect(mem + 70 * pg, 2 * pg);
    atomic_store(&g_hold, 1); atomic_store(&g_entered, 0); atomic_store(&g_release, 0);
    pthread_t t1, t2;
    if (pthread_create(&t1, NULL, writer, mem + 70 * pg) || pthread_create(&t2, NULL, writer, mem + 71 * pg)) return 2;
    while (atomic_load(&g_entered) != 2) sched_yield(); // 两个解除操作已到 mprotect 之前
    CK(!snap(span, mem + 70 * pg, pg, &v, &e));
    CK(snap(span, mem, pg, &v, &e) && v == v0 && e == e0); // 未相交块不受影响
    atomic_store(&g_release, 1); pthread_join(t1, NULL); pthread_join(t2, NULL); atomic_store(&g_hold, 0);
    CK(mem[70 * pg] == 0x6d && mem[71 * pg] == 0x6d);
    CK(snap(span, mem + 70 * pg, pg, &v2, &e2) && v2 == v1 + 2 && e2 == e1);

    // 另一块走真正的缺页回调，而非宿主写前准备。
    CK(snap(span, mem + 130 * pg, pg, &v1, &e1));
    nvn_dirty_protect(mem + 130 * pg, pg);
    sig_atomic_t faults = g_fault_count; mem[130 * pg] = 0x52;
    CK(g_fault_count == faults + 1 && mem[130 * pg] == 0x52);
    CK(snap(span, mem + 130 * pg, pg, &v2, &e2) && v2 == v1 + 1 && e2 == e1);

    // 代号先推进、页代号后发布：中间态不能被快照认证为干净。
    nvn_dirty_protect(mem + 160 * pg, pg);
    uint64_t old_now = nvn_dirty_now(), old_page = 0, during_page = 0;
    CK(nvn_dirty_page_gens(span, mem + 160 * pg, pg, &old_page, 1) == 1);
    atomic_store(&g_after_entered, 0); atomic_store(&g_after_release, 0);
    nvn_dirty_block_test_after_now = after_now;
    pthread_t t3; if (pthread_create(&t3, NULL, writer, mem + 160 * pg)) return 2;
    while (!atomic_load(&g_after_entered)) sched_yield();
    CK(nvn_dirty_now() > old_now && nvn_dirty_page_gens(span, mem + 160 * pg, pg, &during_page, 1) == 1 && during_page == old_page);
    CK(!snap(span, mem + 160 * pg, pg, &v, &e));
    atomic_store(&g_after_release, 1); pthread_join(t3, NULL); nvn_dirty_block_test_after_now = NULL;
    CK(mem[160 * pg] == 0x6d && snap(span, mem + 160 * pg, pg, &v, &e));

    CK(nvn_dirty_replace_range(mem, 256 * pg));
    CK(snap(span, mem, pg, &v, &e) && e != e0); // 换底变更身份
    nvn_dirty_remove_range(mem);
    CK(!snap(span, mem, pg, &v, &e));
    nvn_dirty_add_range(mem, 256 * pg);
    const void *again = nvn_dirty_span(mem, 256 * pg);
    CK(again && snap(again, mem, pg, &v1, &e1) && e1 != e0); // 同址槽复用不复用身份
    nvn_dirty_add_range(mem + pg, 2 * pg); // 宿主页有别名：整池摘要保守停用
    CK(!snap(again, mem, pg, &v, &e));
    nvn_dirty_remove_range(mem);
    const void *alias = nvn_dirty_span(mem + pg, pg);
    CK(alias && !snap(alias, mem + pg, pg, &v, &e)); // 剩下的另一池同样不可认证
    nvn_dirty_remove_range(mem + pg);

    // 摘要分配失败：旧逐页跟踪仍然生效，不能把缺失元数据解释成干净。
    nvn_dirty_block_test_fail_alloc = 1;
    nvn_dirty_add_range(mem, 256 * pg); span = nvn_dirty_span(mem, 256 * pg);
    CK(span && !snap(span, mem, pg, &v, &e));
    nvn_dirty_protect(mem, pg); uint64_t before = nvn_dirty_now();
    g_prep(mem, 1); mem[0] = 0x4a;
    CK(nvn_dirty_since(mem, pg, before) == 1 && mem[0] == 0x4a);
    nvn_dirty_remove_range(mem); nvn_dirty_block_test_fail_alloc = 0;

    nvn_dirty_add_range(mem, 256 * pg); span = nvn_dirty_span(mem, 256 * pg);
    CK(snap(span, mem, pg, &v, &e));
    atomic_store(&g_fail_mp, 1); nvn_dirty_protect(mem, pg); atomic_store(&g_fail_mp, 0);
    CK(nvn_dirty_tracking_unreliable() && !snap(span, mem, pg, &v, &e));
    nvn_dirty_remove_range(mem); nvn_dirty_reset_session();
    nvn_dirty_mp_hook = NULL; munmap(mapping, total);
    printf("%s t_dirty_blocks: %d checks, %d failures\n", fails ? "✗" : "✓", checks, fails);
    return fails ? 1 : 0;
}
