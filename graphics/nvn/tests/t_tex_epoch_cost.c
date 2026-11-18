// 贴图内容纪元在「无关写」下的真实代价（真 nvn_dirty.c：真写保护、真缺页、真逐页比代号）。
// 复现 9000 的「取贴图视图」持锁段：K 张已传过、内容没变的 CPU 贴图放在一个被写跟踪的池里；每笔绘制前游戏写一次同池里别处的一页
// （常量缓冲那类：真缺页 → 解保护 → 全局写代号 +1），回放随后把它重新加保护（同 rp_buf 同步后的做法）。
// 每笔绘制把 K 张各取一次，按 rp_res.c 的键（上次核对时的 nvn_dirty_now()）判能否跳过：
//   A（现状）：键没命中就 nvn_dirty_since 整张逐页比；
//   B（修法方向原型）：每张贴图在池的脏页追加日志上留一个游标，只读游标之后的写事件，和自己的范围求交；日志不可用时退回 A。
// 断言：两条路判出来的「脏 / 不脏」逐次一致；A 每笔每张都没命中、比的页数 = K × 每张页数；B 读到的事件数 = 无关写的次数、一张贴图都不判脏。
// 最后单独写一张贴图里的一页：A、B 都必须判它脏、其余不脏（负对照：B 不能因为只看事件就漏掉真写）。耗时只打印、不断言。
#include "../gfx/nvn/nvn_dirty.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int (*g_fault)(void *, int, void *);
static void (*g_prep)(void *, size_t);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { g_fault = fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { g_prep = fn; }
void dm_log(const char *fmt, ...) { (void)fmt; }
static volatile sig_atomic_t g_faults;
static void on_sig(int sig, siginfo_t *si, void *ctx) {
    if (g_fault && g_fault(si->si_addr, 1, ctx)) { g_faults++; return; }
    signal(sig, SIG_DFL);
}

static int checks, fails;
#define CK(expr) do { checks++; if (!(expr)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expr); } } while (0)
static uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }

enum { K = 16, D = 64 };
struct tex { uint8_t *mem; size_t len; uint64_t stamp, check_epoch; uint64_t log_cur; int dirty_b; };
struct hit { uintptr_t a, b; int hit; };
static void on_event(void *user, uintptr_t a, uintptr_t b) { struct hit *h = user; if (a < h->b && b > h->a) h->hit = 1; }

int main(int argc, char **argv) {
    size_t tex_mb = argc > 1 ? (size_t)atoi(argv[1]) : 4;   // 每张贴图大小（MB），缺省 4 MB（2048x512 RGBA8 一级的量级）
    struct sigaction sa = {0}; sa.sa_sigaction = on_sig; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, NULL); sigaction(SIGSEGV, &sa, NULL);
    const size_t pg = (size_t)getpagesize(), tex_len = tex_mb << 20, pool = K * tex_len + 64 * pg;
    uint8_t *map = mmap(NULL, pool + 64 * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (map == MAP_FAILED) return 2;
    uint8_t *mem = (uint8_t *)(((uintptr_t)map + 64 * pg - 1) & ~(uintptr_t)(64 * pg - 1));
    nvn_dirty_set_chunk_pages(0);
    CK(nvn_dirty_log_configure(1));
    nvn_dirty_add_range(mem, pool);
    const void *span = nvn_dirty_span(mem, pool);
    uint64_t log_id = 0, log_cur0 = 0;
    CK(span && nvn_dirty_log_open(span, &log_id, &log_cur0));
    for (size_t o = 0; o < pool; o += pg) mem[o] = 0x5a;   // 每页先写一次（加保护前：这是「上传前」的内容；也让页真的落地，缺页才是真缺页）
    uint8_t *ubo = mem + K * tex_len;                      // 同池里的「常量缓冲」页
    nvn_dirty_protect(mem, pool);
    struct tex t[K];
    for (int i = 0; i < K; i++) { t[i] = (struct tex){ mem + (size_t)i * tex_len, tex_len, nvn_dirty_now(), nvn_dirty_now(), log_cur0, 0 }; }
    const uint64_t pages_per = tex_len / pg;

    uint64_t miss_a = 0, pages_a = 0, ns_a = 0, ev_b = 0, ns_b = 0, dirty_a = 0, dirty_b = 0, disagree = 0, fallback_b = 0;
    for (int d = 0; d < D; d++) {
        sig_atomic_t f0 = g_faults; ubo[(size_t)(d % 32) * pg] = (uint8_t)d;   // 无关写：真缺页，全局写代号 +1
        CK(g_faults == f0 + 1);
        nvn_dirty_protect(ubo, 32 * pg);                   // 回放同步完常量缓冲后重新加保护
        const uint64_t now = nvn_dirty_now();
        for (int i = 0; i < K; i++) {
            int da = 0, db = 0;
            uint64_t a0 = now_ns();
            if (t[i].check_epoch != now) { miss_a++; pages_a += pages_per; da = nvn_dirty_since(t[i].mem, t[i].len, t[i].stamp); t[i].check_epoch = now; }
            uint64_t a1 = now_ns();
            struct hit h = { (uintptr_t)t[i].mem, (uintptr_t)t[i].mem + t[i].len, 0 };
            uint64_t before = t[i].log_cur;
            if (nvn_dirty_log_read(span, log_id, &t[i].log_cur, on_event, &h)) { db = h.hit; ev_b += t[i].log_cur - before; }
            else { fallback_b++; db = nvn_dirty_since(t[i].mem, t[i].len, t[i].stamp); }
            uint64_t a2 = now_ns();
            ns_a += a1 - a0; ns_b += a2 - a1; dirty_a += (uint64_t)da; dirty_b += (uint64_t)db; disagree += da != db;
        }
    }
    CK(miss_a == (uint64_t)K * D && pages_a == (uint64_t)K * D * pages_per);   // 现状：每笔每张都没命中、整张逐页比
    CK(dirty_a == 0 && dirty_b == 0 && disagree == 0 && fallback_b == 0);       // 没有一张真被写：两条路都判不脏
    CK(ev_b == (uint64_t)K * D);                                                // 原型：每张每笔只读到那一条无关写事件
    printf("tex-epoch cost (real nvn_dirty, host page %zu B, %d textures x %zu MB = %llu pages each, %d draws):\n", pg, K, tex_mb, (unsigned long long)pages_per, D);
    printf("  A current: epoch_miss=%llu pages_compared=%llu total=%.2f ms  per_miss=%.1f us  per_page=%.2f ns\n",
           (unsigned long long)miss_a, (unsigned long long)pages_a, ns_a / 1e6, ns_a / 1e3 / (double)miss_a, (double)ns_a / (double)pages_a);
    printf("  B dirty-log cursor: events_read=%llu total=%.2f ms  per_check=%.2f us  fallback=%llu\n",
           (unsigned long long)ev_b, ns_b / 1e6, ns_b / 1e3 / (double)(K * D), (unsigned long long)fallback_b);

    // 负对照：真写贴图 3 的一页 → A、B 都只判贴图 3 脏
    sig_atomic_t f0 = g_faults; t[3].mem[5 * pg] = 0x77; CK(g_faults == f0 + 1);
    const uint64_t now = nvn_dirty_now();
    for (int i = 0; i < K; i++) {
        int da = nvn_dirty_since(t[i].mem, t[i].len, t[i].stamp);
        struct hit h = { (uintptr_t)t[i].mem, (uintptr_t)t[i].mem + t[i].len, 0 };
        int ok = nvn_dirty_log_read(span, log_id, &t[i].log_cur, on_event, &h);
        CK(ok && da == (i == 3) && h.hit == (i == 3));
        t[i].check_epoch = now;
    }
    nvn_dirty_remove_range(mem);
    printf("t_tex_epoch_cost：%d 项检查，%d 项红\n", checks, fails);
    return fails ? 1 : 0;
}
