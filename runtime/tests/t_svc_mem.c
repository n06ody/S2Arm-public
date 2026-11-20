// svc_mem.c 单测：s2a_mem_mapped（C# IsRangeMapped 用）与「逐段 s2a_mem_query、遇权限 0 即否」同判，且不随映射段长变慢。
//   期望值：手写几例（按 Horizon 语义直接写出）+ 随机区间对拍逐段查询的老算法（C# GuestMemory.IsRangeMapped 原循环原样搬来）
// 等事件的上限只当兜底（11-19：原先 1～5 秒，负载高时线程没跑到就判红 → 一律 30 秒，正常时一到就返回）
#include "../src/native.h"
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include "../include/s2a_init_fail.h"
// 初启失败出口（宿主里 host_main.c 的强定义走提示页、不返回）：单测里记下码就返回，svc_mem.c 的弱定义被它顶替
static int g_if_code;
void s2a_init_failed(int code) { g_if_code = code; }
#include <stdatomic.h>
#include <mach/mach.h>
#include <stdlib.h>
#include <string.h>
// This isolated Apple host test uses the shared layout, as the Apple provider does.
int plat_layout_override(const char *p, uint32_t t, uint64_t r, uint64_t id, uint64_t b, uint64_t n,
                         struct s2a_layout *o, uint64_t *need, char *d, size_t cap) {
    (void)p; (void)t; (void)r; (void)id; (void)b; (void)n; (void)o; (void)need; (void)d; (void)cap;
    return ENOTSUP;
}
void s2a_vaguard_open(void);
S2A_TEST_MOD_TABLE(64); int g_nmod;
void s2a_mem_test_setup(uint64_t, uint64_t, uint64_t, uint64_t, void *, uint64_t);
int s2a_snap_page_released(uint64_t hp);   // svc_mem.c：即时存档跳过已释放待清零的页
uint64_t s2a_mem_test_punch_pending(void); void s2a_mem_test_punch_stats(uint64_t *inline_zero, uint64_t *forget);   // #258 懒清零
void s2a_mem_test_subpage(uint64_t a, int on); void s2a_mem_test_map(uint64_t b, uint64_t n);
void s2a_mem_test_map_diag_reset(void); void s2a_mem_test_stack(uint64_t base, uint64_t size);
void s2a_mem_test_astype(uint32_t t);
void s2a_mem_test_host_avail(uint64_t v); void s2a_mem_test_headroom_div(uint64_t d);   // 扩池上限 = 池 + 池 / d（缺省 8；单测调大好用小映射测到边界）   // 应用池按宿主余量扩：测试里的宿主余量（缺省 0 = 宿主给不了，照模拟的池拒）
int s2a_mem_mapped(uint64_t addr, uint64_t size);
int s2a_mem_readable(uint64_t addr, uint64_t size);
uint32_t s2a_mem_tmem_borrow(uint64_t a, uint64_t n, uint32_t perm); void s2a_mem_tmem_unborrow(uint64_t a, uint64_t n); void s2a_mem_tmem_close(uint64_t a, uint64_t n);
void s2a_mem_codemem_free(void *o);
void s2a_mem_test_map_perm(int index, uint32_t perm);
void s2a_mem_test_tlr(uint64_t base, uint64_t next);
void s2a_mem_test_reset_probes(void);
uint64_t s2a_mem_test_probes(void);
void s2a_mem_test_sysres(uint64_t v, uint64_t heap_region); void s2a_mem_test_alias_report(uint64_t v); uint64_t s2a_mem_test_phys(void); int svc_mem(int num, uint64_t *regs, uint64_t orig);
// svc_mem.c 的其余外部依赖（本单测不走到）
void s2a_backtrace(uint64_t fp) { (void)fp; } int s2a_det_shmem_map(uint64_t ga, const void *h, uint64_t n, uint64_t sp, int pr) { (void)ga; (void)h; (void)n; (void)sp; (void)pr; return 0; } uint64_t s2a_det_tick(uint64_t t) { return t; } uint64_t s2a_entropy64(void) { return 0; }
void s2a_host_shutdown(const char *w) { (void)w; }
void s2a_low_holes(uint64_t *t, uint64_t *l) { *t = *l = 0; } uint64_t s2a_process_id(void) { return 0x51; } static uint8_t *g_shm; static uint64_t g_shm_n; void *s2a_shmem_lookup(uint32_t h, uint64_t *s) { if (h != 0x77 || !g_shm) return NULL; *s = g_shm_n; return g_shm; }   // 换页事务单测配
void s2a_svc_frame(uint64_t pc) { (void)pc; } static int g_red_n; void s2a_svc_red(int n, const uint64_t *r, uint64_t o, const char *w) { (void)n; (void)r; (void)o; (void)w; g_red_n++; }   // 族 25 单测数报红次数
// 族 28：s2a_thread_cpu_ns 替身——当前线程伪句柄 0xFFFF8000 累计 625 µs（= 12000 个 19.2 MHz 计数），其余句柄无效
int s2a_thread_alive_count(void) { return 0; } void s2a_thread_set_npdm(int c, int p) { (void)c; (void)p; } void s2a_thread_set_hw_cores(int n) { (void)n; } int s2a_thread_hw_cores(void) { return 4; } static void *g_hobj; uint32_t s2a_native_handle_new(uint32_t s, void *o) { if (s != 2) return 0; g_hobj = o; return 0x66; } void *s2a_native_handle_obj(uint32_t h, uint32_t s) { return h == 0x66 && s == 2 ? g_hobj : NULL; } void s2a_handle_kind_count(uint32_t k, int *c, int *p) { (void)k; *c = *p = 0; } static uint64_t g_tm_a, g_tm_n; int s2a_tmem_get(uint32_t h, uint64_t *a, uint64_t *n, uint32_t *p) { if (h != 0x55 || !g_tm_a) return 0; *a = g_tm_a; *n = g_tm_n; *p = 3; return 1; } void s2a_thread_set_caps(uint64_t c, uint64_t p) { (void)c; (void)p; } uint64_t s2a_thread_cpu_ns(uint32_t h, int *ok) { *ok = h == 0xFFFF8000u; return *ok ? 625000 : 0; } int s2a_thread_cur_core(void) { return 0; }
int s2a_svc_check_perm(const uint64_t perm[3], char *out, size_t cap) { (void)perm; if (cap) out[0] = 0; return 0; } int s2a_svc_ryu_missing(int n) { (void)n; return 0; }   // gate.c：NPDM 许可比对（本测不链 gate.c）
uint64_t guest_tick(void) { return 0; }
static uint64_t g_t_hz = 19200000; void s2a_guest_tick_hz_set(uint64_t hz) { g_t_hz = hz; } uint64_t s2a_guest_tick_hz(void) { return g_t_hz; }   // os.c 替身（本测不链 os.c）
uint64_t s2a_ns_to_tick(uint64_t ns) { return (uint64_t)((unsigned __int128)ns * g_t_hz / 1000000000u); } static module_t g_tmod; module_t *module_of(const void *p) { uint64_t x = (uint64_t)p; return g_tmod.base && x >= (uint64_t)g_tmod.base && x < (uint64_t)g_tmod.base + g_tmod.size ? &g_tmod : NULL; }
void s2a_window_get(uint64_t *b, uint64_t *s) { *b = *s = 0; } void s2a_window_trim(uint64_t s) { (void)s; } int s2a_guest_zero(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }
// #258 懒清零的桩：g_gfile=1 时把 [g_gfile_lo, g_gfile_hi) 当成文件后备窗口（没有文件后备时还页照旧换匿名占位）
static int g_gfile; static uint64_t g_gfile_lo, g_gfile_hi;
int s2a_guest_in_file(uint64_t a, uint64_t n) { return g_gfile && n && a >= g_gfile_lo && a + n <= g_gfile_hi; }
void s2a_guest_commit(uint64_t a, uint64_t n) { (void)a; (void)n; }   // vawin.c 的桩：放开前按块定后备（文件后备溢出模式；本测不链 vawin.c）
uint64_t s2a_module_span(const char *d) { (void)d; return 0; }   // 桩：模块跨度（loader.c；本单测不装模块）

static int bad, checks;
// 远端（far.c）的单测替身：缺省同 svc_mem.c 里的弱定义（判远端放不下、给页失败）；g_far_room=1 时判放得下、给页仍失败（影子区与堆撞上，审计 #806 第 2 条）
static int g_far_room, g_far_maps;
int s2a_far_has(uint64_t a, uint64_t n) { (void)a; (void)n; return g_far_room; }
int s2a_far_map(uint64_t a, uint64_t n) { (void)a; (void)n; g_far_maps++; return -1; }
#define CHECK(c, ...) do { checks++; if (!(c)) { bad++; printf("✗ " __VA_ARGS__); printf("\n"); } } while (0)
// 换页事务（审计 #806 第 5 条续）：假的 begin / end 记下调用时目标首页的内核保护——begin 时还是改之前的、end 时已是改之后的，范围一致；
//   放锁之后不再另报 prot_changed（记账重算在 end 里）。期望按各系统调用的 Horizon 语义直接写（新映射 0 → 读写、解除读写 → 0）
static int kprot(uint64_t a) { vm_address_t ra = (vm_address_t)a; vm_size_t rs = 0; vm_region_basic_info_data_64_t ri; mach_msg_type_number_t rn = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t ro = MACH_PORT_NULL;
    return vm_region_64(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&ri, &rn, &ro) == KERN_SUCCESS && ra <= a ? ri.protection : -1; }
static struct { int nb, ne, np, bprot, eprot, committed; uint64_t ba, bn, ea, en; } g_tx;
static int tx_begin(uint64_t a, uint64_t n, uint64_t src) { (void)src; g_tx.nb++; g_tx.ba = a; g_tx.bn = n; g_tx.bprot = kprot(a); return 0; }
static void tx_end(uint64_t a, uint64_t n, int mapped, int committed) { (void)mapped; g_tx.ne++; g_tx.ea = a; g_tx.en = n; g_tx.committed = committed; g_tx.eprot = kprot(a); }
static void tx_prot(uint64_t a, uint64_t n) { (void)a; (void)n; g_tx.np++; }
// 审计 11-01 18:00 P1-48：栈搬移拷贝前的 host_write 替身。g_wb_phase 1 = 映射（源上 +0x10 有「GPU 写的」0x5a 还没回到 CPU），
//   2 = 解除（栈上 +0x20 有 GPU 写的 0xa5）；覆盖到那个地址的回调把值落进内存（= 回写）。
//   回调里另起一条线程查 s2a_mem_mapped（= 回写回调经 IsRangeMapped 查映射）：调用方持着映射表锁时，锁要等回调返回才放，查询必然等满 2 秒上限 → 记「持锁」
static uint64_t g_wb_src, g_wb_dst; static int g_wb_phase, g_wb_hit[3], g_wb_bad_lock, g_wb_in_tx, g_wb_tx_open; static _Atomic int g_wb_q;
static void *wb_query(void *x) { (void)x; s2a_mem_mapped(g_wb_src, 1); atomic_store(&g_wb_q, 1); return NULL; }
static int wb_begin(uint64_t a, uint64_t n, uint64_t s) { (void)a; (void)n; (void)s; g_wb_tx_open++; return 0; }
static void wb_end(uint64_t a, uint64_t n, int m, int c) { (void)a; (void)n; (void)m; (void)c; g_wb_tx_open--; }
static void wb_host_write(uint64_t a, uint64_t n) {
    if (!g_wb_phase) return;
    if (g_wb_tx_open) g_wb_in_tx++;
    atomic_store(&g_wb_q, 0); pthread_t q; pthread_create(&q, NULL, wb_query, NULL);
    for (int i = 0; i < 30000 && !atomic_load(&g_wb_q); i++) usleep(1000);
    if (atomic_load(&g_wb_q)) pthread_join(q, NULL); else { g_wb_bad_lock++; pthread_detach(q); }
    uint64_t gs = g_wb_src + 0x10, gd = g_wb_dst + 0x20;
    if (g_wb_phase == 1 && a <= gs && gs < a + n) { g_wb_hit[0]++; *(uint8_t *)gs = 0x5a; }
    if (g_wb_phase == 2 && a <= gd && gd < a + n) { g_wb_hit[1]++; *(uint8_t *)gd = 0xa5; }
    if (g_wb_phase == 2 && a <= g_wb_src && g_wb_src < a + n) g_wb_hit[2]++;
}
// 老算法（GuestMemory.IsRangeMapped 原循环）
static int old_mapped(uint64_t va, uint64_t size) {
    uint64_t end = va + size;
    while (va < end) { s2a_meminfo m; if (s2a_mem_query(va, &m) != 0 || m.perm == 0 || m.size == 0) return 0; va = m.base + m.size; }
    return 1;
}
// 诊断关闭 fatal 用例的并发查询线程：等 die 进入 write 后查询，完成即往管道写一字节。
static void *query_after_delay(void *fd) {
    usleep(200000); s2a_meminfo m; s2a_mem_query(0x1000, &m);
    if (write(*(int *)fd, "Q", 1) != 1) {}
    return NULL;
}
// 诊断日志里「源账本=…/state=X」的 X；找不到回 UINT32_MAX。
static uint32_t log_src_state(FILE *log) {
    char out[2048]; lseek(fileno(log), 0, SEEK_SET);
    ssize_t got = read(fileno(log), out, sizeof out - 1); if (got < 0) got = 0; out[got] = 0;
    const char *p = strstr(out, "源账本="); unsigned st;
    return p && (p = strstr(p, "/state=")) && sscanf(p, "/state=%x", &st) == 1 ? st : UINT32_MAX;
}
static uint64_t rng = 0x9e3779b97f4a7c15ull; static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static int old_readable(uint64_t a, uint64_t n) {
    if (n > UINT64_MAX - a) return 0;
    uint64_t end = a + n;
    while (a < end) {
        s2a_meminfo info;
        if (s2a_mem_query(a, &info) || !(info.perm & 1) || !info.size || info.base > a ||
            info.size > UINT64_MAX - info.base || info.base + info.size <= a) return 0;
        a = info.base + info.size;
    }
    return 1;
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
// 审计 11-01 P1-10：换页事务进行中（begin 回调里，外层改表串行还持着）另起一条线程调 SetMemoryPermission，看它是否等到事务结束
static _Atomic int g_sp_done; static uint64_t g_sp_addr; static pthread_t g_sp_t; static int g_sp_during = -1;
static void *sp_thread(void *x) { (void)x; uint64_t r[8] = {g_sp_addr, 0x4000, 3}; svc_mem(0x02, r, 0); atomic_store(&g_sp_done, 1); return NULL; }
static int sp_begin(uint64_t a, uint64_t n, uint64_t src) { (void)a; (void)n; (void)src; pthread_create(&g_sp_t, NULL, sp_thread, NULL); usleep(100000); g_sp_during = atomic_load(&g_sp_done); return 0; }

int s2a_va_short_last(uint64_t fa, int *num, uint64_t *a, uint64_t *n, uint64_t *count);
// #947 替身：进程内重开请求（真的在 runtime/src/session.c）。svc_mem 经 dlsym(RTLD_DEFAULT) 找它：记调用次数、按用例回受理 / 拒绝
static int g_restart_calls, g_restart_rc;
int s2a_host_restart(void (*cb)(int, const char *, void *), void *ctx) { (void)cb; (void)ctx; g_restart_calls++; return g_restart_rc; }
int s2a_mem_heap_learned_restart(void);
void s2a_mem_test_heap_floor(uint64_t v);
int main(void) {
    const uint64_t W = 0x7000000000ull, AB = W + 0x200000, AS = 64ull << 20, HEAP = W + 0x10000000, HCUR = 4ull << 20, MAPB = W + 0x20000000, MAPN = 0x8000;
    // 复用宿主地址护栏，先给末尾大堆测试留出真地址，再放行 xzm；不带 MAP_FIXED，不覆盖已有映射。
    const uint64_t large_size = 18ull << 30;
    uint8_t *large = mmap((void *)W, large_size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    s2a_vaguard_open();
    if (large != (void *)W) { if (large != MAP_FAILED) munmap(large, large_size); large = MAP_FAILED; }
    {   // 族 25（缺口扫描 11-01）：已接上的号按语义回失败码，不进报红通道（报红只留给分派里真没接的号）。期望按 Horizon 语义直接写（放在所有内存调用之前：普通日志每号首次必打，后面的用例会把计数推过）：
        //   堆区 4 MB 时 SetHeapSize 6 MB 回 0xD001（内存区不够）；GetProcessId 给普通句柄回 0xE401；GetInfo 类型 99 回 0xF001——三次都打普通日志、报红 0 次
        const uint64_t MB = 1ull << 20, RS = 16 * MB;
        uint8_t *R = mmap(NULL, RS + 2 * MB, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(R != MAP_FAILED, "族 25 单测：预留 16 MB");
        uint64_t b = ((uint64_t)R + 2 * MB - 1) & ~(2 * MB - 1);
        s2a_mem_test_setup(b + 4 * MB, 4 * MB, b, RS, (void *)b, 0); s2a_mem_test_sysres(0x2000000, 4 * MB);
        FILE *log = tmpfile(); CHECK(log != NULL, "族 25 单测：日志临时文件");
        char hn[] = "/tmp/s2a_t_heap_need_XXXXXX"; int hfd = mkstemp(hn); if (hfd >= 0) close(hfd); unlink(hn); setenv("S2A_HEAP_NEED", hn, 1);   // D-NGH9：超堆区时记下要的堆
        if (log) {
            int saved = g_s2a_logfd; g_s2a_logfd = fileno(log); g_red_n = 0;
            uint64_t r[8], rc[3];
            memset(r, 0, sizeof r); r[1] = 6 * MB; svc_mem(0x01, r, 0); rc[0] = r[0];
            memset(r, 0, sizeof r); r[1] = 0x1234; svc_mem(0x24, r, 0); rc[1] = r[0];
            memset(r, 0, sizeof r); r[1] = 99; r[2] = 0xFFFF8001; svc_mem(0x29, r, 0); rc[2] = r[0];
            char out[4096]; lseek(g_s2a_logfd, 0, SEEK_SET); ssize_t got = read(g_s2a_logfd, out, sizeof out - 1); out[got > 0 ? got : 0] = 0;
            g_s2a_logfd = saved; fclose(log);
            int logged = strstr(out, "系统调用 0x1 回 0xd001") && strstr(out, "系统调用 0x24 回 0xe401") && strstr(out, "系统调用 0x29 回 0xf001") && !strstr(out, "【红】");
            CHECK(rc[0] == 0xD001 && rc[1] == 0xE401 && rc[2] == 0xF001 && g_red_n == 0 && logged,
                  "族 25：超堆区 / 普通句柄 / 未知信息类型 期望 回 0xd001 / 0xe401 / 0xf001、报红 0 次、三行普通失败日志 → 实 %#llx / %#llx / %#llx、报红 %d 次、日志%s",
                  (unsigned long long)rc[0], (unsigned long long)rc[1], (unsigned long long)rc[2], g_red_n, logged ? "齐" : "不齐");
            // D-NGH9 / #947：超堆区那次（6 MB）记下要的堆；再要 5 MB 不降（只增不减）；要 8 MB 升到 8 MB。期望手写 6291456 → 6291456 → 8388608
            unsigned long long v[3] = {0}; FILE *hf;
            hf = fopen(hn, "r"); if (hf) { if (fscanf(hf, "%llu", &v[0]) != 1) v[0] = 0; fclose(hf); }
            memset(r, 0, sizeof r); r[1] = 5 * MB; svc_mem(0x01, r, 0);
            hf = fopen(hn, "r"); if (hf) { if (fscanf(hf, "%llu", &v[1]) != 1) v[1] = 0; fclose(hf); }
            memset(r, 0, sizeof r); r[1] = 8 * MB; svc_mem(0x01, r, 0);
            hf = fopen(hn, "r"); if (hf) { if (fscanf(hf, "%llu", &v[2]) != 1) v[2] = 0; fclose(hf); }
            CHECK(v[0] == 6291456 && v[1] == 6291456 && v[2] == 8388608, "超堆区记下要的堆：期望 6291456 / 6291456 / 8388608 → 实 %llu / %llu / %llu", v[0], v[1], v[2]);
            // #947：堆不够已记下 8 MB、本局布局没按任何记录给（单测没走 layout_prepare，按 0）→ 自动进程内重开：请求受理（替身回 0）→ 回 1、请求 1 次；
            //   被拒（替身回 -2，如安卓不支持重建）→ 回 0（调用方改报「再点一次开始」）、请求 2 次。期望手写
            g_restart_calls = 0; g_restart_rc = 0; int ok1 = s2a_mem_heap_learned_restart();
            g_restart_rc = -2; int ok2 = s2a_mem_heap_learned_restart();
            CHECK(ok1 == 1 && ok2 == 0 && g_restart_calls == 2, "堆不够自动重开：受理 / 被拒 期望 回 1 / 0、请求 2 次 → 实 %d / %d、%d 次", ok1, ok2, g_restart_calls);
            // 防循环：本局布局已按 8 MB 给过（= 记下的）仍不够 → 不是堆区给小，不重开、不发请求；按 4 MB 给过（< 记下的 8 MB）→ 重开。期望手写 0 / 1、请求 0 / 1 次
            g_restart_rc = 0; g_restart_calls = 0; s2a_mem_test_heap_floor(8ull << 20); int ok3 = s2a_mem_heap_learned_restart(); int c3 = g_restart_calls;
            s2a_mem_test_heap_floor(4ull << 20); int ok4 = s2a_mem_heap_learned_restart(); s2a_mem_test_heap_floor(0);
            CHECK(ok3 == 0 && c3 == 0 && ok4 == 1 && g_restart_calls == 1, "堆不够自动重开防循环：本局已按 8 / 4 MB 给 期望 回 0 / 1、请求 0 / 1 次 → 实 %d / %d、%d / %d 次", ok3, ok4, c3, g_restart_calls);
            // 小内存 iPad：堆区声明 64 MB 装得下 8 MB，但窗口只剩 16 MB、堆起点 +4 MB → 8 MB 请求越过窗口尾。
            //   期望手写：回 0xD001、把 8388608 记进 heap_need（下一局按它给窗口），不是静默 Break。
            unlink(hn);
            s2a_mem_test_setup(b + 4 * MB, 4 * MB, b, RS, (void *)(b + 10 * MB), 0); s2a_mem_test_sysres(0x2000000, 64 * MB);
            memset(r, 0, sizeof r); r[1] = 8 * MB; svc_mem(0x01, r, 0);
            unsigned long long vw = 0; hf = fopen(hn, "r"); if (hf) { if (fscanf(hf, "%llu", &vw) != 1) vw = 0; fclose(hf); }
            CHECK(r[0] == 0xD001 && vw == 8388608, "窗口越界的堆请求：期望 回 0xd001 且记下 8388608 → 实 %#llx / %llu",
                  (unsigned long long)r[0], vw);
        }
        unsetenv("S2A_HEAP_NEED"); unlink(hn);
        munmap(R, RS + 2 * MB);
    }
    s2a_mem_test_setup(AB, AS, W, 64ull << 30, (void *)HEAP, HCUR);
    s2a_mem_test_map(MAPB, MAPN);
    // 手写：堆内、堆尾越界、映射表、空地址、长度 0
    CHECK(s2a_mem_mapped(HEAP + 8, 8) == 1, "堆内 8 字节应已映射");
    CHECK(s2a_mem_mapped(HEAP + HCUR - 4, 8) == 0, "跨堆尾应未映射");
    CHECK(s2a_mem_mapped(MAPB, MAPN) == 1 && s2a_mem_mapped(MAPB, MAPN + 1) == 0, "映射表整段 = 1、多一字节 = 0");
    CHECK(s2a_mem_mapped(W + 0x30000000, 8) == 0, "空地址应未映射");
    CHECK(s2a_mem_mapped(W + 0x30000000, 0) == 1, "长度 0 恒为已映射（同老循环）");
    // 别名区：子页 [0, 3) 映射、第 3 个空、[4, 8) 映射 ⇒ 跨第 3 个子页的区间为 0，[4,8) 内为 1
    for (int i = 0; i < 8; i++) if (i != 3) s2a_mem_test_subpage(AB + (uint64_t)i * 0x1000, 1);
    CHECK(s2a_mem_mapped(AB, 0x3000) == 1 && s2a_mem_mapped(AB, 0x3001) == 0 && s2a_mem_mapped(AB + 0x4000, 0x4000) == 1, "别名区子页：[0,3) 1、跨空子页 0、[4,8) 1");
    // 随机对拍：别名区随机成段地开 / 关子页，随机区间（8 字节到 256 KB，部分跨堆 / 映射表 / 区外）
    for (int r = 0; r < 600; r++) { uint64_t p = AB + (rnd() % (AS / 0x1000)) * 0x1000; int n = (int)(rnd() % 64) + 1, on = (int)(rnd() & 1);
        for (int i = 0; i < n && p + (uint64_t)i * 0x1000 < AB + AS; i++) s2a_mem_test_subpage(p + (uint64_t)i * 0x1000, on); }
    int diff = 0, ones = 0; const int N = 20000;
    for (int i = 0; i < N; i++) {
        uint64_t base = (rnd() % 4 == 0) ? (rnd() & 1 ? HEAP : MAPB) + rnd() % 0x10000 - 0x8000 : AB + rnd() % AS;
        uint64_t size = (rnd() % 3 == 0) ? 8 : 1 + rnd() % 0x40000;
        int a = s2a_mem_mapped(base, size), b = old_mapped(base, size); ones += a; if (a != b) { if (diff < 5) printf("  不一致：[%#llx, +%#llx) 新 %d 老 %d\n", (unsigned long long)base, (unsigned long long)size, a, b); diff++; }
    }
    CHECK(diff == 0, "随机 %d 个区间与逐段查询对拍：不一致 %d（其中判已映射 %d）", N, diff, ones);
    CHECK(ones > N / 20 && ones < N - N / 20, "随机样本两种结果都要有一定比例（已映射 %d / %d）", ones, N);
    // 速度：别名区整段 64 MB 全映射，中间查 8 字节——老算法每次把整段并出来（约 4096 个宿主页），新的一步
    for (uint64_t a = AB; a < AB + AS; a += 0x1000) s2a_mem_test_subpage(a, 1);
    const int M = 2000; volatile int sink = 0; double t0 = now_ms();
    for (int i = 0; i < M; i++) sink += old_mapped(AB + AS / 2 + (uint64_t)(i % 64) * 8, 8);
    double t1 = now_ms(); uint64_t s2a_mem_mapped_steps(void); uint64_t st0 = s2a_mem_mapped_steps();
    for (int i = 0; i < M; i++) sink += s2a_mem_mapped(AB + AS / 2 + (uint64_t)(i % 64) * 8, 8);
    double t2 = now_ms(), told = t1 - t0, tnew = t2 - t1; uint64_t steps = s2a_mem_mapped_steps() - st0;
    CHECK(sink == 2 * M, "整段映射里查 8 字节都应已映射（%d / %d）", sink, 2 * M);
    // 判步数不判耗时（照 bufreg 的改法）：整段已映射的页掩码满，查 8 字节一步就过；逐页并（老算法约 4096 个宿主页）的实现步数会成千倍。耗时只打印
    CHECK(steps <= 2 * (uint64_t)M, "整段 64 MB 映射里查 8 字节 %d 次：新算法应每次一步到位，共走 %llu 步（上限 %d；耗时新 %.2f ms / 老 %.2f ms 仅供参考）", M, (unsigned long long)steps, 2 * M, tnew, told);
    // Read permission is stricter than merely mapped: preserve the readonly-copy predicate.
    CHECK(s2a_mem_readable(HEAP + 8, 8) && !s2a_mem_readable(HEAP + HCUR - 4, 8), "可读检查：堆与跨尾");
    CHECK(s2a_mem_readable(UINT64_MAX, 0) && !s2a_mem_readable(UINT64_MAX - 3, 8), "可读检查：空范围与溢出");
    for (uint32_t perm = 0; perm < 8; perm++) {
        s2a_mem_test_map_perm(0, perm);
        CHECK(s2a_mem_readable(MAPB + 8, 8) == !!(perm & 1), "映射权限%u按读位判定", perm);
        CHECK(!s2a_mem_readable(MAPB + MAPN - 4, 8), "映射权限%u跨尾拒绝", perm);
    }
    s2a_mem_test_map_perm(0, 3);
    s2a_mem_test_tlr(W + 0x40000000, W + 0x40001200);
    CHECK(s2a_mem_readable(W + 0x40001000, 0x1000), "TLR已分配的4KiB页可读");
    CHECK(!s2a_mem_readable(W + 0x40001fff, 2), "TLR跨未分配页拒绝");
    g_nmod = 1; memset(g_mod, 0, sizeof(g_mod[0]));
    g_mod[0].base = (void *)(W + 0x50000000); g_mod[0].ro_off = 0x1000;
    g_mod[0].data_off = 0x2000; g_mod[0].patch_off = 0x3000; g_mod[0].size = 0x4000;
    CHECK(s2a_mem_readable(W + 0x50000000, 0x3000), "模块RX/RO/RW跨段可读");
    CHECK(!s2a_mem_readable(W + 0x50002fff, 2), "模块补丁Free段不可读");
    g_nmod = 0;
    s2a_mem_test_reset_probes(); CHECK(old_readable(AB + AS / 2, 8), "老可读判据正例");
    uint64_t old_probes = s2a_mem_test_probes();
    s2a_mem_test_reset_probes(); CHECK(s2a_mem_readable(AB + AS / 2, 8), "新可读判据正例");
    uint64_t new_probes = s2a_mem_test_probes();
    CHECK(old_probes > 1000 && new_probes <= 2, "8字节检查不扫64MiB邻居：旧%llu次/新%llu次",
        (unsigned long long)old_probes, (unsigned long long)new_probes);
    printf("可读范围位图探测：64MiB中查8字节，旧%llu次→新%llu次\n", (unsigned long long)old_probes, (unsigned long long)new_probes);
    for (int i = 0; i < 4000; i++) s2a_mem_test_subpage(AB + (rnd() % (AS / 0x1000)) * 0x1000, (int)(rnd() & 1));
    int read_diff = 0;
    for (int i = 0; i < N; i++) {
        uint64_t a = i % 4 == 0 ? MAPB + rnd() % 0x10000 : AB + rnd() % AS;
        uint64_t n = 1 + rnd() % 0x20000;
        s2a_mem_test_map_perm(0, (uint32_t)(rnd() & 7));
        read_diff += s2a_mem_readable(a, n) != old_readable(a, n);
    }
    CHECK(read_diff == 0, "读权限/空洞随机%d区间对拍：不一致%d", N, read_diff);
    // 物理映射与堆同池（S2A_PHYS_LIMIT=1；10-23 异度神剑3 不设限一直映射、常驻冲到 32 GB）。期望按 Switch / Ryujinx 语义手写：
    //   池 3285 MB（0xCD500000）；堆摆成 0xCAA00000（2 MB 对齐）；SystemResourceSize 32 MB；两个模块：
    //   模块 0 补丁区在末尾（patch_off 1 MB、size 3 MB）→ 代码只算 1 MB；模块 1 补丁区在前（patch_off < 0、image_size 1 MB）→ 1 MB
    //   堆外 = 代码 2 MB + SystemResourceSize 32 MB → 物理映射最多 0xCD500000 − 0xCAA00000 − 34 MB = 9 MB（主线程栈本单测为 0）
    {
        setenv("S2A_PHYS_LIMIT", "1", 1);
        const uint64_t POOL = 0xCD500000ull, HEAPC = 0xCAA00000ull, PAS = 64ull << 20, MB = 1ull << 20;
        uint8_t *pa = mmap(NULL, PAS, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        uint8_t *ph = mmap(NULL, 8 * MB, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);   // 堆区：SetHeapSize 不会走到 mprotect（应先被拒），只给一块真地址
        CHECK(pa != MAP_FAILED && ph != MAP_FAILED, "物理映射单测：预留别名区 / 堆区");
        s2a_mem_test_setup((uint64_t)pa, PAS, (uint64_t)pa, PAS, ph, HEAPC);
        s2a_mem_test_sysres(0x2000000, 0x200000000ull);
        g_nmod = 2; memset(g_mod, 0, 2 * sizeof g_mod[0]);
        g_mod[0].patch_off = 0x100000; g_mod[0].size = 0x300000;
        g_mod[1].patch_off = -0x4000; g_mod[1].image_size = 0x100000; g_mod[1].size = 0x100000;
        uint64_t r[8];
        #define PHYS(n, a, sz) (memset(r, 0, sizeof r), r[0] = (uint64_t)(a), r[1] = (sz), svc_mem((n), r, 0), r[0])
        uint64_t b = (uint64_t)pa;
        CHECK(PHYS(0x2C, b, 4 * MB) == 0, "映射 4 MB：应成功");
        // 整宿主页原先全空的新映射本来就是零（占位页 mprotect 放出来）：只改权限、不碰物理页，mincore 应查到 0 页常驻。
        //   11-03 火纹：原先判断用了本轮刚置位的掩码，每个宿主页第 2–4 个子页都 memset，游戏映射多少就提交多少（起局尖峰 9.3 GB）
        {
            size_t ps = (size_t)getpagesize(), np = (size_t)(4 * MB) / ps, res = 0; unsigned char *vec = calloc(np, 1);
            CHECK(vec && mincore((void *)b, 4 * MB, (void *)vec) == 0, "新映射 4 MB：mincore 应成功");
            for (size_t i = 0; vec && i < np; i++) res += vec[i] & 1;
            CHECK(res == 0, "新映射 4 MB（整宿主页原先全空）：不应摸物理页，常驻 %zu / %zu 页", res, np);
            free(vec);
        }
        CHECK(PHYS(0x2C, b + 4 * MB, 4 * MB) == 0 && PHYS(0x2C, b + 8 * MB, 1 * MB) == 0 && s2a_mem_test_phys() == 9 * MB, "再映射 4 MB + 1 MB：应成功、正好打满池（记账 %llu MB）", (unsigned long long)(s2a_mem_test_phys() >> 20));
        CHECK(PHYS(0x2C, b + 9 * MB, 0x1000) == 0x10801 && s2a_mem_test_phys() == 9 * MB, "再要 4 KB：应回 0x10801 且记账不变");
        CHECK(s2a_mem_mapped(b + 9 * MB, 0x1000) == 0, "被拒的 4 KB 不应映射");
        CHECK(PHYS(0x2C, b, 9 * MB) == 0 && s2a_mem_test_phys() == 9 * MB, "重复映射已映射的 9 MB：不新增记账、应成功");
        CHECK(PHYS(0x2D, b, 4 * MB) == 0 && s2a_mem_test_phys() == 5 * MB, "解除 4 MB：记账应减到 5 MB");
        CHECK(PHYS(0x2C, b + 12 * MB, 4 * MB) == 0 && s2a_mem_test_phys() == 9 * MB, "解除后再映射别处 4 MB：应成功");
        memset(r, 0, sizeof r); r[1] = 7; r[2] = 0xFFFF8001; svc_mem(0x29, r, 0);
        CHECK(r[1] == POOL, "已用内存（GetInfo 7）= 堆 + 代码 + SystemResourceSize + 物理映射 = 池 %llu MB：实 %llu MB", (unsigned long long)(POOL >> 20), (unsigned long long)(r[1] >> 20));
        memset(r, 0, sizeof r); r[1] = 22; r[2] = 0xFFFF8001; svc_mem(0x29, r, 0);
        CHECK(r[1] == POOL - 32 * MB, "已用内存不含系统资源（GetInfo 22）应 = 池 − 32 MB：实 %llu MB", (unsigned long long)(r[1] >> 20));
        memset(r, 0, sizeof r); r[1] = 21; r[2] = 0xFFFF8001; svc_mem(0x29, r, 0);
        CHECK(r[1] == POOL - 32 * MB, "总内存不含系统资源（GetInfo 21）应 = 池 − 32 MB：实 %llu MB", (unsigned long long)(r[1] >> 20));
        memset(r, 0, sizeof r); r[1] = 6; r[2] = 0xFFFF8001; svc_mem(0x29, r, 0);
        CHECK(r[1] == POOL, "总内存（GetInfo 6）应 = 池：实 %llu MB", (unsigned long long)(r[1] >> 20));
        // #1441 报给游戏的别名区收小：GetInfo 3 回收小的值；映射仍按内部别名区检查——越过收小处的照常成功（同收小前），并记下本款下次恢复
        {
            char flag[256]; snprintf(flag, sizeof flag, "/tmp/t_svc_mem_alias_full_%d", (int)getpid()); unlink(flag); setenv("S2A_ALIAS_FULL", flag, 1);
            memset(r, 0, sizeof r); r[1] = 3; r[2] = 0xFFFF8001; svc_mem(0x29, r, 0);
            CHECK(r[1] == PAS, "没收小：别名区大小（GetInfo 3）= 内部别名区 %llu MB：实 %llu MB", (unsigned long long)(PAS >> 20), (unsigned long long)(r[1] >> 20));
            s2a_mem_test_alias_report(16 * MB);
            memset(r, 0, sizeof r); r[1] = 3; r[2] = 0xFFFF8001; svc_mem(0x29, r, 0);
            CHECK(r[1] == 16 * MB, "收小到 16 MB：GetInfo 3 应回 16 MB：实 %llu MB", (unsigned long long)(r[1] >> 20));
            CHECK(PHYS(0x2C, b + 12 * MB, 4 * MB) == 0 && access(flag, F_OK) != 0, "收小处以内映射：成功、不记越界");
            CHECK(PHYS(0x2D, b + 12 * MB, 4 * MB) == 0 && PHYS(0x2C, b + 18 * MB, 1 * MB) == 0, "越过收小处映射 1 MB：照常成功（内部别名区以内）");
            CHECK(access(flag, F_OK) == 0, "越过收小处：记下本款（%s）", flag);
            unlink(flag); unsetenv("S2A_ALIAS_FULL"); s2a_mem_test_alias_report(0);
            CHECK(PHYS(0x2D, b + 18 * MB, 1 * MB) == 0 && PHYS(0x2C, b + 12 * MB, 4 * MB) == 0 && s2a_mem_test_phys() == 9 * MB, "还原：池子回到占满 9 MB（后面的用例要）");
        }
        // Switch 2 新类型（switchbrew SVC [S2]）：29–32 远端区地址 / 大小 / 用量 / 峰值 = 0（宿主不给远端区），33 页大小 = Horizon 4 KB；子号非 0 回 InvalidCombination
        { uint64_t ok = 1; for (int t = 29; t <= 33; t++) { memset(r, 0, sizeof r); r[1] = t; r[2] = 0xFFFF8001; r[1] = (uint64_t)t; svc_mem(0x29, r, 0); ok &= r[0] == 0 && r[1] == (t == 33 ? 0x1000u : 0); }
          memset(r, 0, sizeof r); r[1] = 29; r[2] = 0xFFFF8001; r[3] = 1; svc_mem(0x29, r, 0);
          CHECK(ok && r[0] == 0xE801, "GetInfo 29–32 回 0、33 回 0x1000、29 子号 1 回 0xE801：实 ok=%llu rc=%#llx", (unsigned long long)ok, (unsigned long long)r[0]); }
        memset(r, 0, sizeof r); r[1] = HEAPC + 2 * MB; svc_mem(0x01, r, 0);
        CHECK(r[0] == 0x10801, "堆再加 2 MB（物理映射已占满池）：SetHeapSize 应回 0x10801，实回 %#llx", (unsigned long long)r[0]);
        // 失败时出参 x1（堆基址）= 0（同 Ryujinx address = 0；D-ASK2：原先留着请求的大小，不看结果码的游戏拿它当基址写进宿主代码段）
        CHECK(r[1] == 0, "SetHeapSize 失败（池满）：x1 应为 0，实 %#llx", (unsigned long long)r[1]);
        // 应用池按宿主余量扩（#1493）：池已满 9 MB。宿主余量 512 MB + 3 MB（留 512 MB 不动 → 能多给 3 MB）：
        //   再要 4 KB → 成功、GetInfo 6 总量 = 池 + 4 KB、已用 = 池 + 4 KB；接着要 3 MB + 4 KB（超出 3 MB 余量）→ 0x10801、记账不变；余量归 0 后池回模拟值
        {
            s2a_mem_test_host_avail(512 * MB + 3 * MB);
            CHECK(PHYS(0x2C, b + 20 * MB, 0x1000) == 0 && s2a_mem_test_phys() == 9 * MB + 0x1000, "宿主有余量：超池 4 KB 应成功（记账 %llu KB）", (unsigned long long)(s2a_mem_test_phys() >> 10));
            memset(r, 0, sizeof r); r[1] = 6; r[2] = 0xFFFF8001; svc_mem(0x29, r, 0); uint64_t tot = r[1];
            memset(r, 0, sizeof r); r[1] = 7; r[2] = 0xFFFF8001; svc_mem(0x29, r, 0);
            CHECK(tot == POOL + 0x1000 && r[1] == POOL + 0x1000, "扩池后总量 / 已用（GetInfo 6 / 7）都应 = 池 + 4 KB：实 %llu / %llu KB", (unsigned long long)(tot >> 10), (unsigned long long)(r[1] >> 10));
            CHECK(PHYS(0x2C, b + 21 * MB, 3 * MB + 0x1000) == 0x10801 && s2a_mem_test_phys() == 9 * MB + 0x1000, "超出宿主余量（要 3 MB + 4 KB、能给 3 MB）：应回 0x10801 且记账不变");
            CHECK(PHYS(0x2D, b + 20 * MB, 0x1000) == 0 && s2a_mem_test_phys() == 9 * MB, "解除那 4 KB：记账回 9 MB");
            // 扩的上限：模拟的池 + 池 / d（产品 d = 8）。单测设 d = 1024：上限 = 池 + 池/1024（约 3.2 MB，按 4 KB 向下取整）。宿主余量给足（64 GB），池已满：
            //   要上限以内的整 4 KB 数 → 成功；再要 4 KB（超出上限）→ 0x10801、记账不变；d 回 8
            s2a_mem_test_host_avail(64ull << 30); s2a_mem_test_headroom_div(1024);
            uint64_t head = (POOL / 1024) & ~0xfffull, base = b + 32 * MB;
            CHECK(PHYS(0x2C, base, head) == 0 && s2a_mem_test_phys() == 9 * MB + head, "扩到 池 + 池/1024（%llu KB）以内：应成功", (unsigned long long)(head >> 10));
            uint64_t ph = s2a_mem_test_phys();
            CHECK(PHYS(0x2C, base + head, 0x1000) == 0x10801 && s2a_mem_test_phys() == ph, "超出 池 + 池/1024：宿主余量再多也回 0x10801、记账不变");
            CHECK(PHYS(0x2D, base, head) == 0 && s2a_mem_test_phys() == 9 * MB, "解除那段：记账回 9 MB");
            s2a_mem_test_headroom_div(8);
            s2a_mem_test_host_avail(0);
            memset(r, 0, sizeof r); r[1] = 6; r[2] = 0xFFFF8001; svc_mem(0x29, r, 0);
            CHECK(r[1] == POOL, "余量归 0、扩过的池清掉：总量回到池：实 %llu MB", (unsigned long long)(r[1] >> 20));
        }
        memset(r, 0, sizeof r); r[1] = 3 * MB + 0x1000; svc_mem(0x01, r, 0);
        CHECK(r[0] == 0xCA01 && r[1] == 0, "SetHeapSize 大小不对齐：应回 0xCA01 且 x1 为 0，实 %#llx / %#llx", (unsigned long long)r[0], (unsigned long long)r[1]);
        #undef PHYS
        g_nmod = 0;
    }
    // 还回的页再给出去必须是零（Switch / Ryujinx 新给的堆和物理映射都填 0；原先缩堆 / 解除物理映射用 madvise(MADV_FREE) 懒回收，
    //   没被内核收走的页在再长回来时带着旧内容）。期望按语义直接写：长回来的整段每字节 0。旧写法在无内存压力时读回填充值 0xA5
    {
        const uint64_t MB = 1ull << 20, HR = 8 * MB, AZ = 4 * MB, PG = 0x10000;
        uint8_t *hz = mmap(NULL, HR, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0), *az = mmap(NULL, AZ, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(hz != MAP_FAILED && az != MAP_FAILED, "清零单测：预留堆区 / 别名区");
        uint64_t win_lo = (uint64_t)hz < (uint64_t)az ? (uint64_t)hz : (uint64_t)az;
        uint64_t win_hi = (uint64_t)hz + HR > (uint64_t)az + AZ ? (uint64_t)hz + HR : (uint64_t)az + AZ;
        s2a_mem_test_setup((uint64_t)az, AZ, win_lo, win_hi - win_lo, hz, 0);
        s2a_mem_test_sysres(0x2000000, HR);
        uint64_t r[8], rc[3];
        #define HEAPSZ(sz) (memset(r, 0, sizeof r), r[1] = (sz), svc_mem(0x01, r, 0), r[0])
        rc[0] = HEAPSZ(4 * MB); memset(hz, 0xA5, 4 * MB); rc[1] = HEAPSZ(2 * MB); rc[2] = HEAPSZ(4 * MB);
        uint64_t nz = 0; for (uint64_t i = 2 * MB; i < 4 * MB; i++) nz += hz[i] != 0;
        CHECK(!rc[0] && !rc[1] && !rc[2] && nz == 0, "堆 4 MB 写满 0xA5 → 缩到 2 MB → 再长到 4 MB：三次应成功、长回来的 2 MB 非零字节应 0，实 %#llx/%#llx/%#llx、%llu",
              (unsigned long long)rc[0], (unsigned long long)rc[1], (unsigned long long)rc[2], (unsigned long long)nz);
        #define PHYS(n, a, sz) (memset(r, 0, sizeof r), r[0] = (uint64_t)(a), r[1] = (sz), svc_mem((n), r, 0), r[0])
        rc[0] = PHYS(0x2C, az, PG); memset(az, 0xA5, PG); rc[1] = PHYS(0x2D, az, PG); rc[2] = PHYS(0x2C, az, PG);
        nz = 0; for (uint64_t i = 0; i < PG; i++) nz += az[i] != 0;
        CHECK(!rc[0] && !rc[1] && !rc[2] && nz == 0, "物理映射 64 KB 写满 0xA5 → 解除 → 再映射：三次应成功、非零字节应 0，实 %#llx/%#llx/%#llx、%llu",
              (unsigned long long)rc[0], (unsigned long long)rc[1], (unsigned long long)rc[2], (unsigned long long)nz);
        // 10-31 Minecraft：游戏把物理映射的一段设成只读（SetMemoryPermission +0x13000 读），之后解除其中的子页。真机 / Ryujinx 解除只查状态与属性、
        //   不查权限，应成功；原先解除时对仍有邻居的子页 memset 清零，撞上只读宿主页 SIGBUS。期望：一宿主页（16 KB）映射写满 0xA5 → 设只读 →
        //   解除首个 4 KB → 再映射这 4 KB：四次都成功、不崩，再映射的 4 KB 每字节 0
        uint64_t rp[4]; uint8_t *ap = az + 0x10000;
        rp[0] = PHYS(0x2C, ap, 0x4000); memset(ap, 0xA5, 0x4000);
        memset(r, 0, sizeof r); r[0] = (uint64_t)ap; r[1] = 0x4000; r[2] = 1; svc_mem(0x02, r, 0); rp[1] = r[0];
        rp[2] = PHYS(0x2D, ap, 0x1000); rp[3] = PHYS(0x2C, ap, 0x1000);
        nz = 0; for (uint64_t i = 0; i < 0x1000; i++) nz += ap[i] != 0;
        CHECK(!rp[0] && !rp[1] && !rp[2] && !rp[3] && nz == 0, "物理映射 16 KB → 设只读 → 解除首个 4 KB → 再映射：四次应成功、再映射的 4 KB 非零字节应 0，实 %#llx/%#llx/%#llx/%#llx、%llu",
              (unsigned long long)rp[0], (unsigned long long)rp[1], (unsigned long long)rp[2], (unsigned long long)rp[3], (unsigned long long)nz);
        // 11-09 玩家 iPhone 17 Pro Max（01001BA027DD8000）：游戏 DT_INIT 后 SetMemoryPermission 把 main 的 RELRO 改只读，nn::ro 装 NRO 时装载器补绑导入槽直接写 → SIGBUS。
        //   期望（手写）：堆里一个宿主页写好桩值 → 游戏设只读（svc 0x02 读）→ 子进程直接写被信号杀（修前现场）→ s2a_loader_store64 回 0、槽 = 新值 →
        //   子进程直接写仍被杀（权限恢复成只读）、查询仍报游戏设的「读」；可执行页（模拟签名代码页）回 -1、内容不变
        {
            int s2a_loader_store64(uint64_t *where, uint64_t v);
            uint64_t *slot = (uint64_t *)(hz + 0x8000 + 0x8d0); *slot = 0x1111;
            memset(r, 0, sizeof r); r[0] = (uint64_t)(hz + 0x8000); r[1] = 0x4000; r[2] = 1; svc_mem(0x02, r, 0); uint64_t prc = r[0];
            #define DIES_STORE(p) ({ fflush(stdout); pid_t c_ = fork(); if (!c_) { *(volatile uint64_t *)(p) = 0x3333; _exit(0); } int st_ = 0; waitpid(c_, &st_, 0); WIFSIGNALED(st_); })
            int d0 = DIES_STORE(slot), w = s2a_loader_store64(slot, 0x2222); uint64_t got = *slot; int d1 = DIES_STORE(slot);
            s2a_meminfo mi; s2a_mem_query((uint64_t)slot, &mi);
            CHECK(prc == 0 && d0 && w == 0 && got == 0x2222 && d1 && mi.perm == 1,
                  "装载器补绑只读页：设只读 %#llx、修前直接写被杀 %d、装载器写回 %d、槽 %#llx（应 0x2222）、写后直接写仍被杀 %d、游戏看到的权限 %u（应 1）",
                  (unsigned long long)prc, d0, w, (unsigned long long)got, d1, mi.perm);
            uint64_t *xp = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); xp[0] = 0x5555; mprotect(xp, 0x4000, PROT_READ | PROT_EXEC);
            CHECK(s2a_loader_store64(xp, 0x6666) == -1 && xp[0] == 0x5555, "装载器不写可执行页（签名代码页同类）：回 -1、内容不变");
            munmap(xp, 0x4000);
            memset(r, 0, sizeof r); r[0] = (uint64_t)(hz + 0x8000); r[1] = 0x4000; r[2] = 3; svc_mem(0x02, r, 0);
            // #815（0100FA501AF90000，补丁 @rugal7699）：游戏把模块映像里的一段收紧成只读后，游戏 SDK 自己的 nn::ro 还往里写（不经装载器）→ SIGBUS。
            //   期望（手写）：这段落在一个已载入模块的映像里 → 收紧回成功、游戏查到的权限是「读」，但宿主页仍可写（子进程直接写不被杀）；
            //   不在任何模块映像里的同样请求照旧设宿主只读（直接写被杀）；放宽回读写照旧
            strcpy(g_tmod.name, "main"); g_tmod.base = hz + 0x8000; g_tmod.size = 0x8000;
            memset(r, 0, sizeof r); r[0] = (uint64_t)(hz + 0x8000); r[1] = 0x4000; r[2] = 1; svc_mem(0x02, r, 0); uint64_t mrc = r[0];
            s2a_mem_query((uint64_t)slot, &mi);
            int md = DIES_STORE(slot);
            CHECK(mrc == 0 && mi.perm == 1 && !md, "模块映像内收紧只读：回 %#llx、游戏看到的权限 %u（应 1）、宿主直接写被杀 %d（应 0：宿主保持可写）",
                  (unsigned long long)mrc, mi.perm, md);
            memset(r, 0, sizeof r); r[0] = (uint64_t)(hz + 0x8000); r[1] = 0x4000; r[2] = 3; svc_mem(0x02, r, 0);
            g_tmod.base = NULL; g_tmod.size = 0;
            memset(r, 0, sizeof r); r[0] = (uint64_t)(hz + 0x8000); r[1] = 0x4000; r[2] = 1; svc_mem(0x02, r, 0);
            int od = DIES_STORE(slot);
            CHECK(r[0] == 0 && od, "不在模块映像里的收紧照旧设宿主只读：直接写被杀 %d（应 1）", od);
            memset(r, 0, sizeof r); r[0] = (uint64_t)(hz + 0x8000); r[1] = 0x4000; r[2] = 3; svc_mem(0x02, r, 0);
            #undef DIES_STORE
        }
        #undef PHYS
        #undef HEAPSZ
    }
    // 内存类调用参数校验（期望照 Ryujinx Syscall：InvalidAddress 0xCC01、InvalidSize 0xCA01、InvalidMemState 0xD401、InvalidPermission 0xD801、
    //   InvalidMemRange 0xDC01、InvalidCombination 0xE801、InvalidHandle 0xE401）。窗口 = 一块 4 MB 预留区，窗口外另一块当「宿主内存」
    {
        const uint64_t WS = 4ull << 20;
        uint8_t *w = mmap(NULL, WS, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0), *o = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(w != MAP_FAILED && o != MAP_FAILED, "校验单测：预留窗口 / 窗口外页");
        s2a_mem_test_setup((uint64_t)w, 0, (uint64_t)w, WS, w, 0);
        uint64_t r[8], b = (uint64_t)w;
        s2a_mem_test_map(b, 0x4000);   // MapMemory 用例的源：已映射可读（源未提交回 0xD401，审计 P1-11）
        #define SV(n, x0, x1, x2, x3) (memset(r, 0, sizeof r), r[0] = (x0), r[1] = (x1), r[2] = (x2), r[3] = (x3), svc_mem((n), r, 0), r[0])
        CHECK(SV(0x02, b + 8, 0x4000, 3, 0) == 0xCC01, "SetMemoryPermission 地址不对齐：应 InvalidAddress");
        CHECK(SV(0x02, b, 0, 3, 0) == 0xCA01, "SetMemoryPermission 大小 0：应 InvalidSize");
        CHECK(SV(0x02, b, 0x800, 3, 0) == 0xCA01, "SetMemoryPermission 大小不对齐：应 InvalidSize（原先回 InvalidAddress）");
        CHECK(SV(0x02, ~0xfffull, 0x2000, 3, 0) == 0xD401, "SetMemoryPermission 回绕：应 InvalidMemState");
        CHECK(SV(0x02, b, 0x4000, 2, 0) == 0xD801, "SetMemoryPermission 只写：应 InvalidPermission");
        CHECK(SV(0x02, b, 0x4000, 4, 0) == 0xD801, "SetMemoryPermission 权限 4：应 InvalidPermission");
        CHECK(SV(0x02, (uint64_t)o, 0x4000, 1, 0) == 0xD401, "SetMemoryPermission 窗口外宿主内存：应 InvalidMemState（原先照改并回成功）");
        CHECK(SV(0x02, b, 0x4000, 3, 0) == 0, "SetMemoryPermission 窗口内读写：应成功");
        CHECK(SV(0x03, b, 0x4000, 8, 8) == 0, "SetMemoryAttribute 设无缓存：应成功");
        CHECK(SV(0x03, b, 0x4000, 0, 8) == 0xE801, "SetMemoryAttribute 值不在掩码内：应 InvalidCombination");
        CHECK(SV(0x03, b, 0x4000, 1, 1) == 0xE801, "SetMemoryAttribute 不支持的属性（Borrowed）：应 InvalidCombination");
        CHECK(SV(0x03, b, 0x4000, 0x10, 0) == 0xE801, "SetMemoryAttribute 清锁权限：应 InvalidCombination");
        CHECK(SV(0x03, b + 8, 0x4000, 8, 8) == 0xCC01, "SetMemoryAttribute 地址不对齐：应 InvalidAddress");
        CHECK(SV(0x03, (uint64_t)o, 0x4000, 8, 8) == 0xD401, "SetMemoryAttribute 窗口外：应 InvalidMemState");
        // Switch 2 的 0x80（Pokopia：GPU 可访问内存建 / 拆时设 / 清属性位 0x20，按调用点推断）：同 0x03 的校验、多认 0x20
        CHECK(SV(0x80, b, 0x4000, 0x20, 0x20) == 0, "0x80 设属性 0x20（建 GPU 可访问内存）：应成功");
        CHECK(SV(0x80, b, 0x4000, 0x20, 0) == 0, "0x80 清属性 0x20（拆）：应成功");
        CHECK(SV(0x80, b, 0x4000, 0, 0x20) == 0xE801, "0x80 值不在掩码内：应 InvalidCombination");
        CHECK(SV(0x80, b, 0x4000, 0x40, 0x40) == 0xE801, "0x80 不认的属性位 0x40：应 InvalidCombination");
        CHECK(SV(0x80, b + 8, 0x4000, 0x20, 0x20) == 0xCC01, "0x80 地址不对齐：应 InvalidAddress");
        CHECK(SV(0x80, (uint64_t)o, 0x4000, 0x20, 0x20) == 0xD401, "0x80 窗口外：应 InvalidMemState");
        CHECK(SV(0x04, b + 0x4000, b + 8, 0x4000, 0) == 0xCC01, "MapMemory 源不对齐：应 InvalidAddress");
        CHECK(SV(0x04, b + 0x4000, b, 0, 0) == 0xCA01, "MapMemory 大小 0：应 InvalidSize");
        CHECK(SV(0x04, b + 0x4000, (uint64_t)o, 0x4000, 0) == 0xD401, "MapMemory 源在窗口外：应 InvalidMemState");
        CHECK(SV(0x04, b + 0x4000, b, 0x4000, 0) == 0xDC01, "MapMemory 目标不在栈区：应 InvalidMemRange");
        // Break 仅通知（原因最高位，例 0x80000003 = 装动态模块前通知）：照 Ryujinx 继续（svc 返回 1、x0 = 0），日志一行普通行、不含【红】也不含「Break 原因」
        //   （普查按「Break 原因」认第一个失败点，通知不能占位）；原因 0 仍是致命中止，这里不调（会退出进程）
        {
            FILE *bl = tmpfile(); CHECK(bl != NULL, "Break 通知日志临时文件");
            if (bl) {
                int saved = g_s2a_logfd; g_s2a_logfd = fileno(bl);
                memset(r, 0, sizeof r); r[0] = 0x80000003; r[1] = 0x1234; r[2] = 0x80; int cont = svc_mem(0x26, r, 0);
                char out[1024]; lseek(g_s2a_logfd, 0, SEEK_SET); ssize_t got = read(g_s2a_logfd, out, sizeof out - 1); out[got > 0 ? got : 0] = 0;
                g_s2a_logfd = saved; fclose(bl);
                CHECK(cont == 1 && r[0] == 0, "Break 0x80000003（仅通知）：继续、x0 = 0");
                CHECK(strstr(out, "Break 通知") && !strstr(out, "【红】") && !strstr(out, "Break 原因"), "Break 仅通知：普通行、不打红、不写「Break 原因」");
            }
        }
        // 日志只在显式开启后写一次，原结果不变；账本状态在目标重叠时应指出 ST_STACK。
        FILE *log = tmpfile(); CHECK(log != NULL, "MapMemory 诊断日志临时文件");
        if (log) {
            int saved_log = g_s2a_logfd; g_s2a_logfd = fileno(log);
            char out[2048]; ssize_t got;
            unsetenv("S2A_MAP_DIAG"); s2a_mem_test_map_diag_reset();
            uint64_t off = SV(0x04, b + 0x4000, b + 8, 0x4000, 0);
            CHECK(off == 0xCC01 && lseek(g_s2a_logfd, 0, SEEK_END) == 0, "诊断缺省关闭且返回值不变");
            setenv("S2A_MAP_DIAG", "1", 1);
            uint64_t one = SV(0x04, b + 0x4000, b + 8, 0x4000, 0);
            uint64_t two = SV(0x04, b + 0x4000, b, 0, 0);
            lseek(g_s2a_logfd, 0, SEEK_SET); got = read(g_s2a_logfd, out, sizeof out - 1); if (got < 0) got = 0; out[got] = 0;
            CHECK(one == 0xCC01 && two == 0xCA01 && strstr(out, "原因=alignment result=0xcc01") && strstr(out, "采样=post-validation") && !strstr(out, "原因=size"), "只记录首个失败，后续错误仍返回原码");
            ftruncate(g_s2a_logfd, 0); lseek(g_s2a_logfd, 0, SEEK_SET); s2a_mem_test_map_diag_reset();
            s2a_mem_test_stack(b + 0x10000, 0x10000); s2a_mem_test_map(b + 0x10000, 0x4000);
            uint64_t overlap = SV(0x04, b + 0x10000, b, 0x4000, 0);
            lseek(g_s2a_logfd, 0, SEEK_SET); got = read(g_s2a_logfd, out, sizeof out - 1); if (got < 0) got = 0; out[got] = 0;
            CHECK(overlap == 0xD401 && strstr(out, "原因=destination-overlap result=0xd401") && strstr(out, "采样=failure") && strstr(out, "/state=0xb/perm=0x3"), "重叠失败记录目标账本状态");
            // 真实成功路径应复制内容，且即使启用诊断也不写失败日志。
            ftruncate(g_s2a_logfd, 0); lseek(g_s2a_logfd, 0, SEEK_SET); s2a_mem_test_map_diag_reset();
            s2a_mem_test_stack(b + 0x18000, 0x4000);
            memset((void *)b, 0x5a, 0x4000); memset((void *)(b + 0x18000), 0, 0x4000);
            uint64_t success = SV(0x04, b + 0x18000, b, 0x4000, 0);
            CHECK(success == 0 && memcmp((void *)b, (void *)(b + 0x18000), 0x4000) == 0 &&
                  lseek(g_s2a_logfd, 0, SEEK_END) == 0, "MapMemory 成功复制且不记录失败日志");
            CHECK(SV(0x05, b + 0x18000, b, 0x4000, 0) == 0, "MapMemory 成功用例解除：还回源（映射期间源是借出的，不能作下面用例的源）");
            // #4c853c：对齐的共享映射最大权限不含写（iPad 上源在主程序数据段的私有文件映射里）→ 撤回共享映射、改走拷贝：
            //   期望按语义写死：返回 0、目标内容 = 源、目标可写且写目标不改源（拷贝，不是共享）、解除后源拿回目标里写的内容（拷回）
            {
                extern int s2a_mem_test_alias_eacces; s2a_mem_test_alias_eacces = 1;
                s2a_mem_test_stack(b + 0x30000, 0x4000);
                memset((void *)b, 0x3c, 0x4000);
                uint64_t fb = SV(0x04, b + 0x30000, b, 0x4000, 0);
                int same = fb == 0 && memcmp((void *)b, (void *)(b + 0x30000), 0x4000) == 0;
                if (fb == 0) ((volatile uint8_t *)(b + 0x30000))[0x10] = 0x77;
                int copy = fb == 0 && ((volatile uint8_t *)b)[0x10] == 0x3c;
                CHECK(same && copy, "MapMemory 共享映射最大权限不含写：改走拷贝，返回 0、内容一致、写目标不改源（实得 %#llx / %d / %d）", (unsigned long long)fb, same, copy);
                CHECK(SV(0x05, b + 0x30000, b, 0x4000, 0) == 0 && ((volatile uint8_t *)b)[0x10] == 0x77, "拷贝改道的解除：内容拷回源");
                s2a_mem_test_alias_eacces = 0;
            }
            // 子进程解除目标宿主页映射，让生产 mprotect 真正失败；父进程的窗口保持完整。源取 b+4K（不按宿主页对齐）走拷贝路径——对齐的走共享映射、不 mprotect（族 26）
            ftruncate(g_s2a_logfd, 0); lseek(g_s2a_logfd, 0, SEEK_SET); s2a_mem_test_map_diag_reset();
            s2a_mem_test_stack(b + 0x20000, 0x4000);
            pid_t child = fork();
            if (child == 0) {
                struct rlimit no_core = {0, 0}; setrlimit(RLIMIT_CORE, &no_core);
                if (munmap((void *)(b + 0x20000), 0x4000)) _exit(2);
                SV(0x04, b + 0x20000, b + 0x1000, 0x3000, 0);
                _exit(3);
            }
            int status = 0; pid_t waited = child > 0 ? waitpid(child, &status, 0) : -1;
            lseek(g_s2a_logfd, 0, SEEK_SET); got = read(g_s2a_logfd, out, sizeof out - 1); if (got < 0) got = 0; out[got] = 0;
            char expected_errno[32]; snprintf(expected_errno, sizeof expected_errno, "errno=%d", ENOMEM);
            char *first = strstr(out, "MapMemory首失败");
            CHECK(waited == child && child > 0 && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT &&
                  first && !strstr(first + strlen("MapMemory首失败"), "MapMemory首失败") &&
                  strstr(out, "原因=host-mprotect-fatal result=none") && strstr(out, expected_errno) &&
                  strstr(out, "账本可用=1 采样=failure") && strstr(out, "FATAL MapMemory mprotect"),
                  "宿主 mprotect 失败记录一次现场并保留 fatal");
            // 诊断关闭时 fatal 照 main 持 g_mu 终止：日志管道塞满让 die 卡在 write，另一线程的查询 1 s 内不得完成。
            unsetenv("S2A_MAP_DIAG"); s2a_mem_test_map_diag_reset();
            s2a_mem_test_stack(b + 0x28000, 0x4000);
            int lp[2], qp[2];
            if (pipe(lp) == 0 && pipe(qp) == 0) {
                pid_t held = fork();
                if (held == 0) {
                    struct rlimit no_core = {0, 0}; setrlimit(RLIMIT_CORE, &no_core);
                    char junk[4096] = {0}; fcntl(lp[1], F_SETFL, O_NONBLOCK);
                    while (write(lp[1], junk, sizeof junk) > 0) {}
                    fcntl(lp[1], F_SETFL, 0); g_s2a_logfd = lp[1];
                    if (munmap((void *)(b + 0x28000), 0x4000)) _exit(2);
                    pthread_t qt; pthread_create(&qt, NULL, query_after_delay, &qp[1]);
                    SV(0x04, b + 0x28000, b + 0x1000, 0x3000, 0);
                    _exit(3);
                }
                close(qp[1]); close(lp[1]);
                struct pollfd pf = {qp[0], POLLIN, 0};
                int queried = held > 0 && poll(&pf, 1, 30000) > 0;   // 30 秒兜底（原 1.2 秒：负载高时查询线程慢一点就被当成卡在锁上）
                if (held > 0) { kill(held, SIGKILL); waitpid(held, NULL, 0); }
                CHECK(held > 0 && !queried, "诊断关闭时 mprotect fatal 持 g_mu 终止，并发查询 1 s 内不得完成");
                close(qp[0]); close(lp[0]);
            } else CHECK(0, "诊断关闭 fatal 用例：建管道");
            unsetenv("S2A_MAP_DIAG"); g_s2a_logfd = saved_log; fclose(log);
        }

        CHECK(SV(0x13, 0x1234, b, 0x4000, 0) == 0xD801, "MapSharedMemory 权限 0：应 InvalidPermission（先于句柄）");
        CHECK(SV(0x13, 0x1234, b + 8, 0x4000, 1) == 0xCC01, "MapSharedMemory 地址不对齐：应 InvalidAddress");
        CHECK(SV(0x13, 0x1234, b, 0, 1) == 0xCA01, "MapSharedMemory 大小 0：应 InvalidSize");
        CHECK(SV(0x13, 0x1234, b, 0x4000, 1) == 0xE401, "MapSharedMemory 无效句柄：应 InvalidHandle");
        #undef SV
    }
    // 首失败的源账本状态与同地址 QueryMemory 一致（先 MapMemory 后查询，免得查询先把位图建出来）。
    //   别名区 [0x70_0000_0000, +32 GiB) 伸出窗口 [0x70_0000_0000, +16 GiB)；目标在栈区外，失败于 stack-range，不碰这些地址
    {
        const uint64_t AB = 0x7000000000ull, AS = 0x800000000ull, WSZ = 0x400000000ull, ST = 0x7100000000ull;
        FILE *log = tmpfile(); CHECK(log != NULL, "首失败账本状态：日志临时文件");
        if (log) {
            int saved_log = g_s2a_logfd; g_s2a_logfd = fileno(log);
            setenv("S2A_MAP_DIAG", "1", 1);
            struct { const char *name; uint64_t src; int bitmap, on; uint32_t want; } cs[] = {
                {"位图未建、窗口外别名地址", AB + WSZ + 0x200000000ull, 0, 0, 0},
                {"位图已建、子页未映射", AB + 0x200000000ull, 1, 0, 0},
                {"位图已建、子页已映射", AB + 0x200000000ull, 1, 1, 5},
            };
            for (int i = 0; i < 3; i++) {
                s2a_mem_test_setup(AB, AS, AB, WSZ, (void *)0x6000000000ull, 0); s2a_mem_test_stack(ST, 0x10000);
                if (cs[i].bitmap) s2a_mem_test_subpage(cs[i].src, cs[i].on);
                ftruncate(g_s2a_logfd, 0); lseek(g_s2a_logfd, 0, SEEK_SET); s2a_mem_test_map_diag_reset();
                uint64_t r[8] = {AB + 0x300000000ull, cs[i].src, 0x4000};
                svc_mem(0x04, r, 0);
                uint32_t logged = log_src_state(log);
                s2a_meminfo q; s2a_mem_query(cs[i].src, &q);
                CHECK(r[0] == 0xDC01 && logged == q.state && q.state == cs[i].want, "%s：诊断 state=%#x、QueryMemory state=%#x，应都为 %#x（结果 %#llx）",
                      cs[i].name, logged, q.state, cs[i].want, (unsigned long long)r[0]);
            }
            unsetenv("S2A_MAP_DIAG"); g_s2a_logfd = saved_log; fclose(log);
        }
    }
    // 已公布区域后仍按表分配：真实mmap与生产布局/SVC，不再依赖首次申请临时搬区。
    {
        const uint64_t G = 1ull << 30, MB_ = 1ull << 20, WS = 64 * G;
        // 只预留 18 GiB（16 GiB 乙窗口 + 窗口外 1 GiB 远端测试点 + 对齐余量）：本块不触碰 63 GiB 甲布局那段的内存；
        //   macOS 27 的普通 mmap(NULL) 连 18 GiB 也可能放到 2^39 以上；使用 main 开头在护栏保护下预留的低地址段。
        const uint64_t RS = large_size;
        uint8_t *w = large;
        CHECK(w != MAP_FAILED && (uint64_t)w + RS <= (1ull << 39), "大堆单测：2^39 以下预留 18 GB");
        if (w == MAP_FAILED) return 1;
        uint64_t W = ((uint64_t)w + G - 1) & ~(G - 1), AB = W + 2 * MB_ - 0x4000, AS = 63 * G + 0x8000 - 2 * MB_, tail = W + 63 * G + 2 * MB_;
        void s2a_mem_test_vamm(uint64_t, uint64_t, uint64_t);
        uint64_t r[8];
        #define SVH(n, x0, x1, x2, x3) (memset(r, 0, sizeof r), r[0] = (x0), r[1] = (x1), r[2] = (x2), r[3] = (x3), svc_mem((n), r, 0), r[0])
        s2a_mem_test_setup(AB, AS, W, WS, (void *)(tail + 128 * MB_), 0); s2a_mem_test_vamm(0x1800000, tail + 128 * MB_, 32 * MB_);
        void s2a_mem_test_peaks(uint64_t *, uint64_t *);
        uint64_t pk, ap;
        {   // Switch 2 应用池诊断开关（S2A_APP_POOL_MB）：没设 = 9 GiB；9809 → 9809 MiB；0 / 负数 / 乱写 → 照旧 9 GiB（期望手写）
            uint64_t s2a_ounce_pool(void); unsetenv("S2A_APP_POOL_MB"); uint64_t d0 = s2a_ounce_pool();
            setenv("S2A_APP_POOL_MB", "9809", 1); uint64_t d1 = s2a_ounce_pool();
            setenv("S2A_APP_POOL_MB", "-5", 1); uint64_t d2 = s2a_ounce_pool(); setenv("S2A_APP_POOL_MB", "x", 1); uint64_t d3 = s2a_ounce_pool(); unsetenv("S2A_APP_POOL_MB");
            CHECK(d0 == 0x240000000ull && d1 == 9809ull << 20 && d2 == 0x240000000ull && d3 == 0x240000000ull, "Switch 2 应用池开关：缺省 9 GiB、9809 MiB、负数与乱写照旧"); }
        SVH(0x29, 0, 3, 0xFFFF8001, 0);
        CHECK(SVH(0x01, 0, 0x5a800000, 0, 0) == 0xD001, "无表32MiB仍真实拒绝惊奇申请");
        s2a_mem_test_peaks(&pk,&ap); CHECK(pk == 0x5a800000, "失败的合法申请必须计入需求");
        CHECK(SVH(0x01, 0, 0x5a800001, 0, 0) == 0xCA01, "未对齐申请拒绝");
        CHECK(SVH(0x01, 0, UINT64_MAX & ~0x1fffffull, 0, 0) == 0x10801, "超物理池请求拒绝且不溢出");
        s2a_mem_test_peaks(&pk,&ap); CHECK(pk == 0x5a800000, "非法/物理池超限不污染需求");
        SVH(0x29, 0, 4, 0xFFFF8001, 0); CHECK(r[1] == tail+128*MB_, "拒绝后不改堆基址");
        // 出包地址需求表 → 布局提示（11-03）：「别名区上限 不定」回负数（layout.c 不收窗口）；没有这行回正数；程序号不符回 0
        { int s2a_mem_test_alias_hint(const char *npdm, uint64_t pid);
          char dir[] = "/tmp/s2a-alias-hint-XXXXXX"; CHECK(mkdtemp(dir) != NULL, "建临时目录");
          char np[128], an[128]; snprintf(np, sizeof np, "%s/main.npdm", dir); snprintf(an, sizeof an, "%s/addr_need.tsv", dir);
          FILE *t = fopen(an, "w");
          fprintf(t, "版本\t2\n程序号\t0100fc60185ae000\n构建号\t2545826ca04fc82c8cc6cc426281edf3\n别名区GB\t13\n普通堆MB\t3284\n");
          fclose(t);
          int bounded = s2a_mem_test_alias_hint(np, 0x0100FC60185AE000ull);
          t = fopen(an, "a"); fprintf(t, "别名区上限\t不定\n上限不定原因\t只占地址、尺寸非常量的申请 1 处：main+0x123008 读内存（取数@0x2c4a298）\n"); fclose(t);
          int unb = s2a_mem_test_alias_hint(np, 0x0100FC60185AE000ull), other = s2a_mem_test_alias_hint(np, 0x0100000000000001ull);
          CHECK(bounded == 13 && unb == -13 && other == 0, "上限提示 期望 有上限 13 / 不定 −13 / 程序号不符 0 → 实 %d / %d / %d", bounded, unb, other);
          unlink(an); rmdir(dir); }
        // 乙布局（宿主只占到 16 GiB，docs/方案_地址空间根修.md）：堆在前、给满应用池；别名区伸出窗口，窗口外的映射回内存不足、不碰窗口外地址
        s2a_layout layout; uint64_t modb, shb;
        CHECK(s2a_layout_compute(3,0x1000000,0,0,W,16*G,&layout,&modb,&shb)==0, "16 GiB 窗口按需布局应成功");
        s2a_mem_test_setup(layout.alias_base,layout.alias_size,W,layout.win_size,(void *)layout.heap_base,0);
        s2a_mem_test_vamm(0x1000000,layout.heap_base,layout.heap_size);
        uint64_t rc=SVH(0x01,0,0x83a00000,0,0), hb=r[1];
        CHECK(rc==0 && hb==layout.heap_base, "乙布局堆在窗口内，容纳已知大堆申请 0x83a00000");
        if (!rc) { ((volatile uint8_t *)hb)[0]=1; ((volatile uint8_t *)hb)[0x83a00000-1]=2; }
        s2a_mem_test_peaks(&pk,&ap); CHECK(pk==0x83a00000, "成功请求继续提高需求峰值");
        SVH(0x01, 0, 0, 0, 0);
        {   // #01b36b：窗口只有本机给得出的 3 GiB（小地址设备），游戏要的堆在堆区以内、却伸出窗口末端：物理上放不下 → 回 0xD001、出参 0，
            //   并按「设备地址空间不够」停局（s2a_init_failed(S2A_INIT_FAIL_E_DEVICE_ADDRESS)）。原先只回 0xD001，不看结果码的游戏照样往下写进宿主映像 SIGBUS。
            //   期望手写：堆从堆区基址起，请求 = 到窗口末端的长度 + 64 MiB（2 MiB 对齐、不超过堆区）
            s2a_layout l3; uint64_t mb3, sb3;
            CHECK(s2a_layout_compute(3,0,0,0,W,3*G,&l3,&mb3,&sb3)==0, "3 GiB 窗口按需布局应成功（SystemResourceSize 0，同 #01b36b 那款）");
            s2a_mem_test_setup(l3.alias_base,l3.alias_size,W,l3.win_size,(void *)l3.heap_base,0);
            s2a_mem_test_vamm(0x1000000,l3.heap_base,l3.heap_size);
            uint64_t big = ((W + l3.win_size - l3.heap_base) + 64 * MB_ + 2 * MB_ - 1) & ~(2 * MB_ - 1);
            CHECK(l3.heap_base >= W && l3.heap_base < W + l3.win_size && big <= l3.heap_size, "夹具：堆区起点在窗口内、请求 %#llx 不超过堆区 %#llx", (unsigned long long)big, (unsigned long long)l3.heap_size);
            g_if_code = 0; uint64_t r3 = SVH(0x01, 0, big, 0, 0);
            CHECK(r3 == 0xD001 && r[1] == 0 && g_if_code == S2A_INIT_FAIL_E_DEVICE_ADDRESS, "堆伸出本机窗口：回 %#llx、出参 %#llx、停局码 %d（期望 0xD001、0、%d）",
                  (unsigned long long)r3, (unsigned long long)r[1], g_if_code, S2A_INIT_FAIL_E_DEVICE_ADDRESS);
            g_if_code = 0; SVH(0x01, 0, 64 * MB_, 0, 0);
            CHECK(r[0] == 0 && g_if_code == 0, "对照：窗口内放得下的堆照常成功、不停局（回 %#llx、码 %d）", (unsigned long long)r[0], g_if_code);
            SVH(0x01, 0, 0, 0, 0);
            s2a_mem_test_setup(layout.alias_base,layout.alias_size,W,layout.win_size,(void *)layout.heap_base,0);   // 还原 16 GiB 窗口，后面的用例照旧
            s2a_mem_test_vamm(0x1000000,layout.heap_base,layout.heap_size);
        }
        uint64_t a=layout.alias_base+2*G;
        {   // #1143 续：学跨度只认空洞以内的地址。界设在这段映射中间（a − 窗口基址 + 0x2000）：别名区跨度截到界上 = 别名区内偏移 2 GiB + 0x2000，
            //   完整高水位照记 2 GiB + 0x4000（地址需求表用）；界 0 = 不限
            void s2a_mem_span_limit(uint64_t); uint64_t s2a_mem_test_alias_in(void);
            s2a_mem_span_limit(a - W + 0x2000);
            CHECK(SVH(0x2C,a,0x4000,0,0)==0, "跨界的别名映射照常成功");
            CHECK(s2a_mem_test_alias_in()==2*G+0x2000, "别名区学跨度截到界上：期望 %#llx 实 %#llx", (unsigned long long)(2*G+0x2000), (unsigned long long)s2a_mem_test_alias_in());
            s2a_mem_test_peaks(&pk,&ap); CHECK(ap==2*G+0x4000, "完整别名高水位不受界影响");
            CHECK(SVH(0x2D,a,0x4000,0,0)==0, "解除");
            s2a_mem_span_limit(0);
            uint64_t s2a_span_pick(uint64_t, uint64_t, const uint64_t *, int);
            const uint64_t B = 0x7000000000ull, e3[3] = {B + 2*G, B + 73*G, B - 0x1000};
            CHECK(s2a_span_pick(B, 64*G, e3, 3)==2*G, "跨度：超出空洞（73 GiB > 64 GiB）的整项不算、窗口基址以下的不算 → 2 GiB");
            CHECK(s2a_span_pick(B, 0, e3, 3)==73*G, "跨度：界 0 不限 → 73 GiB（旧口径）"); }
        CHECK(SVH(0x2C,a,0x4000,0,0)==0 && SVH(0x2D,a,0x4000,0,0)==0, "窗口内别名映射及解除");
        s2a_mem_test_peaks(&pk,&ap); CHECK(ap==2*G+0x4000, "解除映射后保留别名最高偏移");
        uint64_t far=W+layout.win_size+G;   // 16 GiB 窗口之外的别名区（远端），仍在本块预留里
        CHECK(SVH(0x2C,far,0x4000,0,0)==0xD001, "窗口外别名映射回内存不足");
        {   // 11-14 D-G2JB 崩溃归因：最近一次地址越界的请求记下来，出事地址落在里面才点名（crash.c fault_region_note）
            int vn=0; uint64_t va=0, vl=0, vk=0;
            int in=s2a_va_short_last(far+0x40,&vn,&va,&vl,&vk), past=s2a_va_short_last(far+0x4000,&vn,&va,&vl,&vk), inwin=s2a_va_short_last(W+0x40,&vn,&va,&vl,&vk);
            s2a_va_short_last(far,&vn,&va,&vl,&vk);
            CHECK(in==1 && past==0 && inwin==0 && vn==0x2C && va==far && vl==0x4000 && vk>=1, "地址越界记账：请求内 1 / 请求末尾外 0 / 窗口内 0，记的 svc 0x2C、起点、长度（实 %d %d %d，svc %#x，%#llx +%#llx，次数 %llu）",
                  in, past, inwin, vn, (unsigned long long)va, (unsigned long long)vl, (unsigned long long)vk);
        }
        CHECK(SVH(0x2C,W+layout.win_size-0x4000,0x8000,0,0)==0xD001, "跨窗口末尾的映射整笔拒绝");
        {   // 审计 #806 第 2 条：跨窗口的物理映射，窗口外给影子页失败 → 回内存不足，窗口内那段不许已被放开、不许记账（原先先 mprotect 窗口内再要影子，失败直接返回）
            uint64_t xa=W+layout.win_size-0x4000, ph0=s2a_mem_test_phys();
            g_far_room=1; g_far_maps=0; uint64_t xr=SVH(0x2C,xa,0x8000,0,0); g_far_room=0;
            vm_address_t ra=(vm_address_t)xa; vm_size_t rs=0; vm_region_basic_info_data_64_t ri; mach_msg_type_number_t rn=VM_REGION_BASIC_INFO_COUNT_64; mach_port_t ro=MACH_PORT_NULL;
            int okr = vm_region_64(mach_task_self(),&ra,&rs,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&ri,&rn,&ro)==KERN_SUCCESS && ra<=xa;
            CHECK(xr==0xD001 && g_far_maps==1, "跨窗口映射、窗口外给影子页失败：回内存不足（rc=%#llx，远端给页调了 %d 次）", (unsigned long long)xr, g_far_maps);
            CHECK(okr && ri.protection==0, "给页失败后窗口内那页仍不可访问（保护 %d，期望 0）", okr ? ri.protection : -1);
            CHECK(s2a_mem_test_phys()==ph0, "给页失败后物理映射记账不变（%llu → %llu）", (unsigned long long)ph0, (unsigned long long)s2a_mem_test_phys());
        }
        s2a_meminfo mi; s2a_mem_query(far,&mi);
        CHECK(mi.state==0, "窗口外的别名区仍报空闲（地址承诺还在）");
        s2a_mem_query(layout.alias_base+3*G,&mi);   // SDK 找 62 GB 连续空闲：窗口内这段空闲要一直并到别名区末尾
        CHECK(mi.state==0 && mi.base+mi.size==layout.alias_base+layout.alias_size, "别名区空闲段跨过窗口末尾并到别名区末");
        s2a_mem_query(layout.alias_base+layout.alias_size+G,&mi);
        CHECK(mi.state==0x10, "别名区之外、窗口之外报不可访问");
        { vm_address_t ra=(vm_address_t)far; vm_size_t rs=0; vm_region_basic_info_data_64_t ri; mach_msg_type_number_t rn=VM_REGION_BASIC_INFO_COUNT_64; mach_port_t ro=MACH_PORT_NULL;
          CHECK(vm_region_64(mach_task_self(),&ra,&rs,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&ri,&rn,&ro)==KERN_SUCCESS && ra<=far && ri.protection==0, "窗口外地址没被改成可读写"); }
        munmap(w, RS);
    }
    {   // 换页事务：六类改表系统调用（堆伸缩、Map / UnmapMemory、Map / UnmapSharedMemory、Map / UnmapPhysicalMemory、ControlCodeMemory、Map / UnmapTransferMemory）
        const uint64_t MB = 1ull << 20, RS = 32 * MB;
        uint8_t *R = mmap(NULL, RS + 2 * MB, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(R != MAP_FAILED, "换页事务单测：预留 32 MB");
        uint64_t b = ((uint64_t)R + 2 * MB - 1) & ~(2 * MB - 1), hz = b, az = b + 8 * MB, st = b + 16 * MB, fr = b + 24 * MB;   // 堆 / 别名 / 栈区 / 空闲各 8 MB
        s2a_mem_test_setup(az, 8 * MB, b, RS, (void *)hz, 0); s2a_mem_test_sysres(0x2000000, 8 * MB); s2a_mem_test_stack(st, 8 * MB);
        g_shm_n = 0x8000; g_shm = mmap(NULL, g_shm_n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
        s2a_track_hooks th = { tx_prot, NULL, NULL, NULL }; s2a_register_track_hooks(&th); s2a_register_track_backing(tx_begin, tx_end);
        uint64_t r[8];
        #define TX(name, num, x0, x1, x2, x3, x4, A, N, P0, P1) do { memset(&g_tx, 0, sizeof g_tx); memset(r, 0, sizeof r); r[0] = (x0); r[1] = (x1); r[2] = (x2); r[3] = (x3); r[4] = (x4); svc_mem((num), r, 0); \
            CHECK(r[0] == 0 && g_tx.nb == 1 && g_tx.ne == 1 && g_tx.ba == (A) && g_tx.bn == (N) && g_tx.ea == (A) && g_tx.en == (N) && g_tx.committed == 1 && g_tx.bprot == (P0) && g_tx.eprot == (P1) && g_tx.np == 0, \
              "换页事务 %s：期望 rc 0、begin / end 各 1 次、范围 %#llx+%#llx、begin 时保护 %d → end 时 %d、提交、之后不另报 prot_changed → 实得 rc %#llx、%d / %d 次、%#llx+%#llx / %#llx+%#llx、%d → %d、提交 %d、prot_changed %d 次", \
              name, (unsigned long long)(A), (unsigned long long)(N), (P0), (P1), (unsigned long long)r[0], g_tx.nb, g_tx.ne, (unsigned long long)g_tx.ba, (unsigned long long)g_tx.bn, \
              (unsigned long long)g_tx.ea, (unsigned long long)g_tx.en, g_tx.bprot, g_tx.eprot, g_tx.committed, g_tx.np); } while (0)
        const int NO = 0, RW = VM_PROT_READ | VM_PROT_WRITE;
        TX("SetHeapSize 长 0 → 2 MB", 0x01, 0, 2 * MB, 0, 0, 0, hz, 2 * MB, NO, RW);
        TX("SetHeapSize 缩 2 → 0 MB", 0x01, 0, 0, 0, 0, 0, hz, 2 * MB, RW, NO);
        TX("MapPhysicalMemory 16 KB", 0x2C, az, 0x4000, 0, 0, 0, az, 0x4000, NO, RW);
        TX("UnmapPhysicalMemory 16 KB", 0x2D, az, 0x4000, 0, 0, 0, az, 0x4000, RW, NO);
        memset(&g_tx, 0, sizeof g_tx); memset(r, 0, sizeof r); r[1] = 2 * MB; svc_mem(0x01, r, 0);   // MapMemory 的源：堆
        TX("MapMemory 栈区 32 KB", 0x04, st, hz, 0x8000, 0, 0, st, 0x8000, NO, RW);
        TX("UnmapMemory 栈区 32 KB", 0x05, st, hz, 0x8000, 0, 0, st, 0x8000, RW, NO);   // 对齐 → 共享映射，解除后目标不可访问（族 26）
        TX("MapSharedMemory 32 KB", 0x13, 0x77, fr, 0x8000, 3, 0, fr, 0x8000, NO, RW);
        TX("UnmapSharedMemory 32 KB", 0x14, 0x77, fr, 0x8000, 0, 0, fr, 0x8000, RW, NO);
        {   // 11-03 火炬之光3：共享内存不许落进堆区的未用部分（Ryujinx CanContain(SharedMemory) 排除整个堆区 / 别名区 → InvalidMemRange）。
            //   此刻堆已用 2 MB、堆区 [hz, hz+8 MB)：hz+4 MB 在堆区内、堆顶之上（旧实现只挡已用堆，这里回 0 = 红）
            void s2a_mem_test_vamm(uint64_t, uint64_t, uint64_t); s2a_mem_test_vamm(0x2000000, hz, 8 * MB);
            memset(r, 0, sizeof r); r[0] = 0x77; r[1] = hz + 4 * MB; r[2] = 0x8000; r[3] = 3; svc_mem(0x13, r, 0);
            CHECK(r[0] == 0xDC01, "MapSharedMemory 落在堆区未用部分（堆顶之上）：应 InvalidMemRange 0xDC01 → 实得 %#llx", (unsigned long long)r[0]);
            memset(r, 0, sizeof r); r[0] = 0x77; r[1] = az + MB; r[2] = 0x8000; r[3] = 3; svc_mem(0x13, r, 0);
            CHECK(r[0] == 0xDC01, "MapSharedMemory 落在别名区：应 InvalidMemRange 0xDC01 → 实得 %#llx", (unsigned long long)r[0]);
        }
        memset(r, 0, sizeof r); r[1] = hz + MB; r[2] = 0x8000; svc_mem(0x4b, r, 0); CHECK(r[0] == 0 && r[1] == 0x66, "CreateCodeMemory 堆上 32 KB（句柄 %#llx）", (unsigned long long)r[1]);
        TX("ControlCodeMemory Map 读写", 0x4c, 0x66, 0, fr + MB, 0x8000, 3, fr + MB, 0x8000, NO, RW);
        TX("ControlCodeMemory Unmap", 0x4c, 0x66, 2, fr + MB, 0x8000, 0, fr + MB, 0x8000, RW, NO);
        g_tm_a = hz + 2 * MB - 0x8000; g_tm_n = 0x8000;
        TX("MapTransferMemory 32 KB", 0x51, 0x55, fr + 2 * MB, 0x8000, 3, 0, fr + 2 * MB, 0x8000, NO, RW);
        TX("UnmapTransferMemory 32 KB", 0x52, 0x55, fr + 2 * MB, 0x8000, 0, 0, fr + 2 * MB, 0x8000, RW, NO);
        #undef TX
        s2a_track_hooks z = {0}; s2a_register_track_hooks(&z); s2a_register_track_backing(NULL, NULL); g_shm = NULL; g_tm_a = 0;
    }
    {   // 审计 11-01 N2（内存改表与查询）。期望按 Horizon 语义直接写：改过的权限查询看得到、换页后回来源缺省；源不可读的 MapMemory 回 InvalidMemState；
        //   SetMemoryPermission 与换页事务串行
        const uint64_t MB = 1ull << 20, RS = 16 * MB;
        uint8_t *R = mmap(NULL, RS + 2 * MB, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(R != MAP_FAILED, "N2 单测：预留 16 MB");
        uint64_t b = ((uint64_t)R + 2 * MB - 1) & ~(2 * MB - 1), hz = b, az = b + 4 * MB, st = b + 8 * MB;   // 堆 / 别名 / 栈区各 4 MB
        s2a_mem_test_setup(az, 4 * MB, b, RS, (void *)hz, 0); s2a_mem_test_sysres(0x2000000, 4 * MB); s2a_mem_test_stack(st, 4 * MB);
        uint64_t r[8]; s2a_meminfo q0, q1, q2;
        #define SV(n, x0, x1, x2) (memset(r, 0, sizeof r), r[0] = (x0), r[1] = (x1), r[2] = (x2), svc_mem((n), r, 0), r[0])
        #define QS(q) (unsigned long long)((q).base - hz), (unsigned long long)(q).size, (q).state, (q).perm
        CHECK(SV(0x01, 0, 2 * MB, 0) == 0, "N2：堆长到 2 MB");
        // P2-14：堆中间 [+16K, +32K) 改只读 → 查询切成三段，中间那段读、两边读写
        CHECK(SV(0x02, hz + 0x4000, 0x4000, 1) == 0, "N2 P2-14：堆 +16K 起 16K 改只读应成功");
        s2a_mem_query(hz, &q0); s2a_mem_query(hz + 0x5000, &q1); s2a_mem_query(hz + 0x8000, &q2);
        CHECK(q0.base == hz && q0.size == 0x4000 && q0.state == 5 && q0.perm == 3, "N2 P2-14：查堆首 期望 +0x0 0x4000 state 5 perm 3 → 实 +%#llx %#llx state %u perm %u", QS(q0));
        CHECK(q1.base == hz + 0x4000 && q1.size == 0x4000 && q1.state == 5 && q1.perm == 1, "N2 P2-14：查改过的那段 期望 +0x4000 0x4000 state 5 perm 1 → 实 +%#llx %#llx state %u perm %u", QS(q1));
        CHECK(q2.base == hz + 0x8000 && q2.size == 2 * MB - 0x8000 && q2.perm == 3, "N2 P2-14：查其后 期望 +0x8000 0x1f8000 perm 3 → 实 +%#llx %#llx state %u perm %u", QS(q2));
        CHECK(SV(0x02, hz + 0x4000, 0x1000, 0) == 0, "N2 P2-14：其中首个 4K 改无权限应成功");
        s2a_mem_query(hz + 0x4000, &q0); s2a_mem_query(hz + 0x5000, &q1);
        CHECK(q0.base == hz + 0x4000 && q0.size == 0x1000 && q0.perm == 0 && q1.base == hz + 0x5000 && q1.size == 0x3000 && q1.perm == 1,
              "N2 P2-14：无权限 4K / 只读余下 期望 +0x4000 0x1000 perm 0 / +0x5000 0x3000 perm 1 → 实 +%#llx %#llx perm %u / +%#llx %#llx perm %u",
              (unsigned long long)(q0.base - hz), (unsigned long long)q0.size, q0.perm, (unsigned long long)(q1.base - hz), (unsigned long long)q1.size, q1.perm);
        CHECK(!s2a_mem_readable(hz + 0x3ff8, 16) && s2a_mem_readable(hz + 0x5000, 0x3000) && s2a_mem_readable(hz, 0x4000), "N2 P2-14：可读判定按权限账（跨无权限页否、只读段与未改段是）");
        SV(0x01, 0, 0, 0); SV(0x01, 0, 2 * MB, 0); s2a_mem_query(hz + 0x4000, &q0);
        CHECK(q0.base == hz && q0.size == 2 * MB && q0.perm == 3, "N2 P2-14：堆缩到 0 再长回 2 MB，改过的权限作废 期望 +0x0 0x200000 perm 3 → 实 +%#llx %#llx state %u perm %u", QS(q0));
        // 审计 P2-11：传输内存借出源 = 权限账记成建立时的权限（同 Ryujinx BorrowTransferMemory：源须已映射且读写），还回清掉回读写
        CHECK(s2a_mem_tmem_borrow(hz + 0x8000, 0x2000, 0) == 0, "P2-11：借出堆 +32K 起 8K（权限 0）应成功");
        s2a_mem_query(hz + 0x8000, &q0); s2a_mem_query(hz + 0xA000, &q1);
        CHECK(q0.base == hz + 0x8000 && q0.size == 0x2000 && q0.state == 5 && q0.perm == 0 && q1.base == hz + 0xA000 && q1.perm == 3,
              "P2-11：借出段 / 其后 期望 +0x8000 0x2000 state 5 perm 0 / +0xa000 perm 3 → 实 +%#llx %#llx state %u perm %u / +%#llx perm %u",
              QS(q0), (unsigned long long)(q1.base - hz), q1.perm);
        CHECK(s2a_mem_tmem_borrow(hz + 0x9000, 0x1000, 3) == 0xD401, "P2-11：借出中的源（权限 0）再借应回 InvalidMemState");
        CHECK(s2a_mem_tmem_borrow(hz + 2 * MB - 0x1000, 0x2000, 3) == 0xD401, "P2-11：跨堆尾进空闲的源应回 InvalidMemState");
        s2a_mem_tmem_unborrow(hz + 0x8000, 0x2000); s2a_mem_query(hz + 0x8000, &q0);
        CHECK(q0.base == hz && q0.size == 2 * MB && q0.perm == 3, "P2-11：还回后 期望 +0x0 0x200000 perm 3 → 实 +%#llx %#llx state %u perm %u", QS(q0));
        // 借出属性（Borrowed，同 Ryujinx MemoryAttribute.Borrowed = 1）：借出期间同一段再作传输内存 / MapMemory 源回 InvalidMemState（Ryujinx 借作源入口 CheckRange 要属性 None），
        //   QueryMemory 报属性 1；还回清位后再借成功。旧代码只改权限：读写借出的那段照样能再借（传输内存权限 3 时权限账看不出借出）
        CHECK(s2a_mem_tmem_borrow(hz + 0x8000, 0x4000, 3) == 0, "借出属性：传输内存（权限 3）借出堆 +32K 起 16K 应成功");
        s2a_mem_query(hz + 0x9000, &q0); s2a_mem_query(hz + 0xC000, &q1);
        CHECK(q0.base == hz + 0x8000 && q0.size == 0x4000 && q0.attr == 1 && q0.perm == 3 && q1.base == hz + 0xC000 && q1.attr == 0,
              "借出属性：借出段 / 其后 期望 +0x8000 0x4000 attr 1 perm 3 / +0xc000 attr 0 → 实 +%#llx %#llx attr %u perm %u / +%#llx attr %u",
              (unsigned long long)(q0.base - hz), (unsigned long long)q0.size, q0.attr, q0.perm, (unsigned long long)(q1.base - hz), q1.attr);
        CHECK(s2a_mem_tmem_borrow(hz + 0x9000, 0x1000, 3) == 0xD401, "借出属性：借出中（权限 3）的源再作传输内存源 期望 0xD401");
        { uint64_t e = SV(0x04, st + 0x100000, hz + 0x8000, 0x4000); CHECK(e == 0xD401, "借出属性：借出中的源作 MapMemory 源 期望 0xD401 → 实 %#llx", (unsigned long long)e);
          if (!e) SV(0x05, st + 0x100000, hz + 0x8000, 0x4000); }
        { uint8_t mi[0x28]; memset(r, 0, sizeof r); r[0] = (uint64_t)mi; r[2] = hz + 0x8000; svc_mem(0x06, r, 0); uint32_t at; memcpy(&at, mi + 0x14, 4);
          CHECK(r[0] == 0 && at == 1, "借出属性：QueryMemory（0x06）MemoryInfo +0x14 属性 期望 1 → 实 rc %#llx 属性 %u", (unsigned long long)r[0], at); }
        s2a_mem_tmem_unborrow(hz + 0x8000, 0x4000); s2a_mem_query(hz + 0x8000, &q0);
        CHECK(q0.attr == 0 && s2a_mem_tmem_borrow(hz + 0x8000, 0x4000, 3) == 0, "借出属性：还回后 属性 0、再借成功 → 实 属性 %u", q0.attr);
        s2a_mem_tmem_unborrow(hz + 0x8000, 0x4000);
        // MapMemory 也是借出（Ryujinx KPageTableBase.Map 给源置 Borrowed）：映射期间源再作传输内存 / MapMemory 源回 0xD401，UnmapMemory 后清位
        CHECK(SV(0x04, st + 0x100000, hz + 0x10000, 0x4000) == 0, "借出属性：MapMemory 栈 ← 堆 +64K 16K 应成功");
        s2a_mem_query(hz + 0x10000, &q0);
        CHECK(q0.attr == 1, "借出属性：MapMemory 源 期望 属性 1 → 实 %u", q0.attr);
        CHECK(s2a_mem_tmem_borrow(hz + 0x10000, 0x4000, 3) == 0xD401, "借出属性：MapMemory 源再作传输内存源 期望 0xD401");
        { uint64_t e = SV(0x04, st + 0x140000, hz + 0x10000, 0x4000); CHECK(e == 0xD401, "借出属性：MapMemory 源再作 MapMemory 源 期望 0xD401 → 实 %#llx", (unsigned long long)e);
          if (!e) SV(0x05, st + 0x140000, hz + 0x10000, 0x4000); }
        CHECK(SV(0x05, st + 0x100000, hz + 0x10000, 0x4000) == 0, "借出属性：UnmapMemory 应成功");
        s2a_mem_query(hz + 0x10000, &q0);
        CHECK(q0.attr == 0 && s2a_mem_tmem_borrow(hz + 0x10000, 0x4000, 3) == 0, "借出属性：UnmapMemory 后 属性 0、再借成功 → 实 属性 %u", q0.attr);
        s2a_mem_tmem_unborrow(hz + 0x10000, 0x4000);
        // 代码内存建立即借出（Ryujinx KCodeMemory.Initialize → BorrowCodeMemory，权限不变）；对象释放（没映射时）还回
        memset(r, 0, sizeof r); r[1] = hz + 0x20000; r[2] = 0x4000; svc_mem(0x4b, r, 0); s2a_mem_query(hz + 0x20000, &q0);
        CHECK(r[0] == 0 && q0.attr == 1 && q0.perm == 3, "借出属性：CreateCodeMemory 期望 rc 0、源属性 1 权限 3 → 实 %#llx、属性 %u 权限 %u", (unsigned long long)r[0], q0.attr, q0.perm);
        void *cmo = g_hobj;
        CHECK(s2a_mem_tmem_borrow(hz + 0x20000, 0x1000, 3) == 0xD401, "借出属性：代码内存源再作传输内存源 期望 0xD401");
        memset(r, 0, sizeof r); r[1] = hz + 0x20000; r[2] = 0x4000; svc_mem(0x4b, r, 0);
        CHECK(r[0] == 0xD401, "借出属性：代码内存源再建代码内存 期望 0xD401 → 实 %#llx", (unsigned long long)r[0]);
        { uint64_t e = SV(0x04, st + 0x100000, hz + 0x20000, 0x4000); CHECK(e == 0xD401, "借出属性：代码内存源作 MapMemory 源 期望 0xD401 → 实 %#llx", (unsigned long long)e);
          if (!e) SV(0x05, st + 0x100000, hz + 0x20000, 0x4000); }
        s2a_mem_codemem_free(cmo); s2a_mem_query(hz + 0x20000, &q0);
        CHECK(q0.attr == 0, "借出属性：代码内存对象释放后源属性 0 → 实 %u", q0.attr);
        memset(r, 0, sizeof r); r[1] = hz + 2 * MB - 0x4000; r[2] = 0x4000; svc_mem(0x4b, r, 0);   // 正对照：堆尾 16K（已映射读写）可建，建完即释放还回
        CHECK(r[0] == 0 && (s2a_mem_codemem_free(g_hobj), 1), "借出属性：堆尾 16K 建代码内存应成功");
        // 缩堆：还掉的尾段里有传输内存借出的源 → InvalidMemState（Ryujinx SetHeapSize 缩堆要属性 None）；还回后照缩
        CHECK(s2a_mem_tmem_borrow(hz + 2 * MB - 0x4000, 0x4000, 3) == 0 && SV(0x01, 0, 0, 0) == 0xD401, "借出属性：尾段有传输内存借出的源，缩堆 期望 0xD401");
        s2a_mem_tmem_unborrow(hz + 2 * MB - 0x4000, 0x4000);
        // 解除物理映射：段里有借出中的源 → InvalidMemState、映射不动（Ryujinx UnmapPhysicalMemory 堆块属性须为 None）；还回后照解。旧代码照解、换掉借出的源页
        CHECK(SV(0x2C, az + 0x40000, 0x8000, 0) == 0 && s2a_mem_tmem_borrow(az + 0x44000, 0x4000, 3) == 0, "借出属性：物理映射 32K、借出其后 16K 应成功");
        { uint64_t e = SV(0x2D, az + 0x40000, 0x8000, 0); int rd = s2a_mem_readable(az + 0x40000, 0x8000);
          CHECK(e == 0xD401 && rd, "借出属性：解除含借出源的物理映射 期望 0xD401、32K 仍可读 → 实 %#llx、可读 %d", (unsigned long long)e, rd); }
        s2a_mem_tmem_unborrow(az + 0x44000, 0x4000);
        CHECK(SV(0x2D, az + 0x40000, 0x8000, 0) == 0, "借出属性：还回后解除物理映射应成功");
        // 传输内存关句柄（Ryujinx KTransferMemory.Destroy：映射着不还回）：0x51 映射期间关 → 源保持借出；解除后再关 → 还回。旧代码关句柄一律还回
        g_tm_a = hz + 0x30000; g_tm_n = 0x4000;
        CHECK(s2a_mem_tmem_borrow(g_tm_a, g_tm_n, 3) == 0, "传输内存关句柄：借出堆 +192K 起 16K 应成功");
        memset(r, 0, sizeof r); r[0] = 0x55; r[1] = st + 0x200000; r[2] = 0x4000; r[3] = 3; svc_mem(0x51, r, 0);
        CHECK(r[0] == 0, "传输内存关句柄：MapTransferMemory 到栈区 +2M 应成功 → 实 %#llx", (unsigned long long)r[0]);
        s2a_mem_tmem_close(g_tm_a, g_tm_n); s2a_mem_query(g_tm_a, &q0);
        CHECK(q0.attr == 1, "传输内存关句柄：映射期间关 期望 源属性 1（保持借出）→ 实 %u", q0.attr);
        memset(r, 0, sizeof r); r[0] = 0x55; r[1] = st + 0x200000; r[2] = 0x4000; svc_mem(0x52, r, 0);
        CHECK(r[0] == 0, "传输内存关句柄：UnmapTransferMemory 应成功 → 实 %#llx", (unsigned long long)r[0]);
        s2a_mem_tmem_close(g_tm_a, g_tm_n); s2a_mem_query(g_tm_a, &q0);
        CHECK(q0.attr == 0, "传输内存关句柄：没映射时关 期望 源属性 0（还回）→ 实 %u", q0.attr);
        g_tm_a = 0;
        // 物理映射 16K 设只读 → 解除首个 4K → 再映射它：再映射的 4K 回读写，余下 12K 仍只读
        CHECK(SV(0x2C, az, 0x4000, 0) == 0 && SV(0x02, az, 0x4000, 1) == 0 && SV(0x2D, az, 0x1000, 0) == 0 && SV(0x2C, az, 0x1000, 0) == 0, "N2 P2-14：物理映射 / 设只读 / 解除 4K / 再映射 4K 四次应成功");
        s2a_mem_query(az, &q0); s2a_mem_query(az + 0x1000, &q1);
        CHECK(q0.base == az && q0.size == 0x1000 && q0.perm == 3 && q1.base == az + 0x1000 && q1.size == 0x3000 && q1.perm == 1,
              "N2 P2-14：再映射的 4K / 余下 12K 期望 az+0x0 0x1000 perm 3 / az+0x1000 0x3000 perm 1 → 实 az+%#llx %#llx perm %u / az+%#llx %#llx perm %u",
              (unsigned long long)(q0.base - az), (unsigned long long)q0.size, q0.perm, (unsigned long long)(q1.base - az), (unsigned long long)q1.size, q1.perm);
        // 没映射时记下的权限（本实现不按状态拒 SetMemoryPermission）在物理映射新给页时作废：新页读写
        SV(0x02, az + 0x10000, 0x1000, 1); s2a_mem_query(az + 0x10000, &q0);
        CHECK(SV(0x2C, az + 0x10000, 0x4000, 0) == 0, "N2 P2-14：物理映射新页应成功");
        s2a_mem_query(az + 0x10000, &q1);
        CHECK(q0.state == 0 && q1.base == az + 0x10000 && q1.size == 0x4000 && q1.perm == 3, "N2 P2-14：空闲时改过权限的子页、新映射后 期望 空闲 / az+0x10000 0x4000 perm 3 → 实 state %u / az+%#llx %#llx perm %u",
              q0.state, (unsigned long long)(q1.base - az), (unsigned long long)q1.size, q1.perm);
        // P1-11：源未提交（别名区没映射的子页）/ 源被设成无权限：回 0xD401、不崩（子进程里调，旧代码 memcpy 直接 SIGSEGV）
        CHECK(SV(0x04, st, hz, 0x4000) == 0, "N2 P1-11：正对照 源是堆（读写）MapMemory 应成功");
        fflush(stdout); pid_t c = fork();
        if (c == 0) {
            struct rlimit no_core = {0, 0}; setrlimit(RLIMIT_CORE, &no_core);
            uint64_t e1 = SV(0x04, st + 0x10000, az + MB, 0x4000);
            SV(0x02, hz + 0x10000, 0x4000, 0);
            uint64_t e2 = SV(0x04, st + 0x20000, hz + 0x10000, 0x4000);
            _exit(e1 == 0xD401 && e2 == 0xD401 ? 0 : 1);
        }
        int cs = 0; pid_t cw = c > 0 ? waitpid(c, &cs, 0) : -1;
        CHECK(cw == c && c > 0 && WIFEXITED(cs) && WEXITSTATUS(cs) == 0, "N2 P1-11：源未提交 / 源无权限的 MapMemory 期望 都回 0xD401、正常退出 → 实 退出 %d 信号 %d",
              WIFEXITED(cs) ? WEXITSTATUS(cs) : -1, WIFSIGNALED(cs) ? WTERMSIG(cs) : 0);
        // P1-10：堆 4 MB 缩到 2 MB 的换页事务进行中（begin 回调里等 100 ms），另一线程对还回去那段的 SetMemoryPermission 期望还没返回（事务结束后才做）
        CHECK(SV(0x01, 0, 4 * MB, 0) == 0, "N2 P1-10：堆长到 4 MB");
        g_sp_addr = hz + 2 * MB; atomic_store(&g_sp_done, 0); g_sp_during = -1;
        s2a_register_track_backing(sp_begin, NULL);
        uint64_t sr = SV(0x01, 0, 2 * MB, 0);
        s2a_register_track_backing(NULL, NULL);
        pthread_join(g_sp_t, NULL);
        CHECK(sr == 0 && g_sp_during == 0 && atomic_load(&g_sp_done) == 1, "N2 P1-10：换页事务中另一线程的 SetMemoryPermission 期望 缩堆 0、事务中未返回 0、事务后完成 1 → 实 %#llx、%d、%d",
              (unsigned long long)sr, g_sp_during, atomic_load(&g_sp_done));
        #undef SV
        #undef QS
        munmap(R, RS + 2 * MB);
    }
    {   // 族 28（缺口扫描 11-01）：GetInfo 子号。期望照 Ryujinx Syscall.GetInfo 手算：
        //   类型 25（正式号）与旧号 0xF0000002 同一分支：当前线程伪句柄、子号 -1 → 625 µs × 19.2 MHz = 12000；子号 4 → 0xE801；
        //   类型 26：句柄须 0（0xE401）、子号只许 0x36（0xE801）、值 = NPDM 许可位（关 0 / 开 1）；
        //   类型 17：系统资源 32 MB 时按已映射字节每 2 MB 一张 4 KB 页表——堆 4 MB → 0x2000，再物理映射 16 KB → 0x3000；没有系统资源 → 0
        const uint64_t MB = 1ull << 20, RS = 16 * MB;
        uint8_t *R = mmap(NULL, RS + 2 * MB, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(R != MAP_FAILED, "族 28 单测：预留 16 MB");
        uint64_t b = ((uint64_t)R + 2 * MB - 1) & ~(2 * MB - 1), hz = b, az = b + 4 * MB;
        s2a_mem_test_setup(az, 4 * MB, b, RS, (void *)hz, 0); s2a_mem_test_sysres(0x2000000, 4 * MB);
        void s2a_mem_test_svc_perm(int num, int on);
        uint64_t r[8];
        #define GI(t, h, s_) (memset(r, 0, sizeof r), r[1] = (t), r[2] = (h), r[3] = (uint64_t)(s_), svc_mem(0x29, r, 0), r[0])
        uint64_t e25 = GI(25, 0xFFFF8000u, -1), v25 = r[1], eo = GI(0xF0000002u, 0xFFFF8000u, -1), vo = r[1], e25s = GI(25, 0xFFFF8000u, 4);
        CHECK(e25 == 0 && v25 == 12000 && eo == 0 && vo == 12000 && e25s == 0xE801,
              "族 28 类型 25：期望 0 / 12000、旧号 0 / 12000、子号 4 0xe801 → 实 %#llx / %llu、%#llx / %llu、%#llx",
              (unsigned long long)e25, (unsigned long long)v25, (unsigned long long)eo, (unsigned long long)vo, (unsigned long long)e25s);
        s2a_mem_test_svc_perm(0x36, 0);
        uint64_t eh = GI(26, 0xFFFF8001u, 0x36), es = GI(26, 0, 0x35), e0 = GI(26, 0, 0x36), v0 = r[1];
        s2a_mem_test_svc_perm(0x36, 1);
        uint64_t e1 = GI(26, 0, 0x36), v1 = r[1];
        s2a_mem_test_svc_perm(0x36, 0);
        CHECK(eh == 0xE401 && es == 0xE801 && e0 == 0 && v0 == 0 && e1 == 0 && v1 == 1,
              "族 28 类型 26：期望 句柄非 0 0xe401、子号 0x35 0xe801、未许可 0 / 0、许可 0 / 1 → 实 %#llx、%#llx、%#llx / %llu、%#llx / %llu",
              (unsigned long long)eh, (unsigned long long)es, (unsigned long long)e0, (unsigned long long)v0, (unsigned long long)e1, (unsigned long long)v1);
        memset(r, 0, sizeof r); r[1] = 4 * MB; svc_mem(0x01, r, 0); uint64_t eg = r[0];
        uint64_t e17a = GI(17, 0xFFFF8001u, 0), v17a = r[1];
        memset(r, 0, sizeof r); r[0] = az; r[1] = 0x4000; svc_mem(0x2C, r, 0); uint64_t ep = r[0];
        uint64_t e17b = GI(17, 0xFFFF8001u, 0), v17b = r[1];
        s2a_mem_test_sysres(0, 4 * MB);
        uint64_t e17c = GI(17, 0xFFFF8001u, 0), v17c = r[1];
        CHECK(eg == 0 && ep == 0 && e17a == 0 && v17a == 0x2000 && e17b == 0 && v17b == 0x3000 && e17c == 0 && v17c == 0,
              "族 28 类型 17：期望 堆 4 MB 0x2000、再映射 16 KB 0x3000、无系统资源 0 → 实 长堆 %#llx 映射 %#llx、%#llx / %#llx、%#llx / %#llx、%#llx / %#llx",
              (unsigned long long)eg, (unsigned long long)ep, (unsigned long long)e17a, (unsigned long long)v17a, (unsigned long long)e17b, (unsigned long long)v17b, (unsigned long long)e17c, (unsigned long long)v17c);
        #undef GI
        munmap(R, RS + 2 * MB);
    }
    {   // 族 26（缺口扫描 11-01）：堆和栈搬移。期望按 Horizon 语义直接写：
        //   ① 宿主页对齐的 MapMemory 目标与源是同一份（源写目标见、目标写源见），解除后目标不可访问（保护 0）、内容留在源里；
        //   ② 不对齐的照旧拷贝，解除时栈上写的拷回源；
        //   ③ 两种借出期间，缩堆还掉含源的尾段回 InvalidMemState 0xD401、堆尾不动；解除后同一次缩堆成功。
        //   子进程里跑：旧代码缩堆照还，随后 UnmapMemory 往已还掉的页拷回 → SIGSEGV（判红看退出状态）
        const uint64_t MB = 1ull << 20, RS = 16 * MB;
        uint8_t *R = mmap(NULL, RS + 2 * MB, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(R != MAP_FAILED, "族 26 单测：预留 16 MB");
        uint64_t b = ((uint64_t)R + 2 * MB - 1) & ~(2 * MB - 1), hz = b, az = b + 4 * MB, st = b + 8 * MB;   // 堆 / 别名 / 栈区各 4 MB
        s2a_mem_test_setup(az, 4 * MB, b, RS, (void *)hz, 0); s2a_mem_test_sysres(0x2000000, 4 * MB); s2a_mem_test_stack(st, 4 * MB);
        uint64_t s2a_mem_heap_end(void);
        fflush(stdout); pid_t c = fork();
        if (c == 0) {
            struct rlimit no_core = {0, 0}; setrlimit(RLIMIT_CORE, &no_core);
            bad = 0; uint64_t r[8];
            #define SV(n, x0, x1, x2) (memset(r, 0, sizeof r), r[0] = (x0), r[1] = (x1), r[2] = (x2), svc_mem((n), r, 0), r[0])
            uint8_t *src = (uint8_t *)(hz + 3 * MB), *dst = (uint8_t *)st;
            uint64_t g = SV(0x01, 0, 4 * MB, 0), m = SV(0x04, st, (uint64_t)src, 0x8000);
            src[0] = 0x77; dst[1] = 0x88; uint8_t d0 = dst[0], s1 = src[1];
            uint64_t sh = SV(0x01, 0, 2 * MB, 0), he = s2a_mem_heap_end();
            uint64_t u = SV(0x05, st, (uint64_t)src, 0x8000); int kp = kprot(st); uint8_t s1u = src[1];
            uint64_t sh2 = SV(0x01, 0, 2 * MB, 0);
            CHECK(!g && !m && d0 == 0x77 && s1 == 0x88 && s1u == 0x88 && sh == 0xD401 && he == hz + 4 * MB && !u && kp == 0 && !sh2,
                  "族 26 ①③ 对齐：期望 长堆 0、映射 0、源写目标见 0x77、目标写源见 0x88、解除后源仍 0x88、借出时缩堆 0xd401、堆尾 +4 MB、解除 0、目标保护 0、解除后缩堆 0 → 实 %#llx、%#llx、%#x、%#x、%#x、%#llx、+%llu MB、%#llx、%d、%#llx",
                  (unsigned long long)g, (unsigned long long)m, d0, s1, s1u, (unsigned long long)sh, (unsigned long long)((he - hz) >> 20), (unsigned long long)u, kp, (unsigned long long)sh2);
            src += 0x1000; dst += 0x10000;
            g = SV(0x01, 0, 4 * MB, 0); src[0] = 0x11; m = SV(0x04, (uint64_t)dst, (uint64_t)src, 0x3000);
            uint8_t seen = dst[0]; dst[0] = 0x99;
            sh = SV(0x01, 0, 2 * MB, 0); he = s2a_mem_heap_end();
            u = SV(0x05, (uint64_t)dst, (uint64_t)src, 0x3000); uint8_t back = src[0];
            sh2 = SV(0x01, 0, 2 * MB, 0);
            CHECK(!g && !m && seen == 0x11 && sh == 0xD401 && he == hz + 4 * MB && !u && back == 0x99 && !sh2,
                  "族 26 ②③ 不对齐（拷贝）：期望 长堆 0、映射 0、目标拷到 0x11、借出时缩堆 0xd401、堆尾 +4 MB、解除 0、栈上写的拷回源 0x99、解除后缩堆 0 → 实 %#llx、%#llx、%#x、%#llx、+%llu MB、%#llx、%#x、%#llx",
                  (unsigned long long)g, (unsigned long long)m, seen, (unsigned long long)sh, (unsigned long long)((he - hz) >> 20), (unsigned long long)u, back, (unsigned long long)sh2);
            #undef SV
            fflush(stdout); _exit(bad ? 1 : 0);
        }
        int cs = 0; pid_t cw = c > 0 ? waitpid(c, &cs, 0) : -1;
        CHECK(cw == c && c > 0 && WIFEXITED(cs) && WEXITSTATUS(cs) == 0, "族 26 子进程 期望 正常退出 0 → 实 退出 %d 信号 %d",
              WIFEXITED(cs) ? WEXITSTATUS(cs) : -1, WIFSIGNALED(cs) ? WTERMSIG(cs) : 0);
        munmap(R, RS + 2 * MB);
    }
    {   // 审计 11-01 18:00 P1-48 / P2-9（苹果栈搬移的拷贝式路径：源 / 目标不按 16 KB 宿主页对齐）。期望按语义直接写：
        //   P1-48 拷贝前对要读的一侧先报 host_write（生产 = 写跟踪「GPU 改过的子页先回写、跟踪页先解开」）：映射报源、解除报栈与源。
        //     替身在回调里把「GPU 写的值」落进内存（= 回写）：映射后栈上应见源的 GPU 值 0x5a（不是 CPU 旧值 0x11）、解除后源应见栈上的 GPU 值 0xa5（不是 0x22）；
        //     回调时不持映射表锁（回调里查 s2a_mem_mapped 能返回；持锁 = 自锁卡死，闹钟 5 秒杀子进程判红）、不在换页事务里（回写可能等排空期）。
        //   P2-9 不对齐解除后：完全落在区间里的宿主页保护 0（再读缺页）；两头盖不满的宿主页保持读写（连着邻居）
        const uint64_t MB = 1ull << 20, RS = 16 * MB;
        uint8_t *R = mmap(NULL, RS + 2 * MB, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(R != MAP_FAILED, "P1-48 单测：预留 16 MB");
        uint64_t b = ((uint64_t)R + 2 * MB - 1) & ~(2 * MB - 1), hz = b, az = b + 4 * MB, st = b + 8 * MB;
        s2a_mem_test_setup(az, 4 * MB, b, RS, (void *)hz, 0); s2a_mem_test_sysres(0x2000000, 4 * MB); s2a_mem_test_stack(st, 4 * MB);
        fflush(stdout); pid_t c = fork();
        if (c == 0) {
            struct rlimit no_core = {0, 0}; setrlimit(RLIMIT_CORE, &no_core); alarm(20);
            bad = 0; uint64_t r[8];
            #define SV(n, x0, x1, x2) (memset(r, 0, sizeof r), r[0] = (x0), r[1] = (x1), r[2] = (x2), svc_mem((n), r, 0), r[0])
            g_wb_src = hz + 0x1000; g_wb_dst = st + 0x11000;   // 都只 4 KB 对齐 → 拷贝式；目标 [+0x11000, +0x19000) 完全盖住宿主页 +0x14000
            s2a_track_hooks h = { NULL, wb_host_write, NULL, NULL }; s2a_register_track_hooks(&h); s2a_register_track_backing(wb_begin, wb_end);
            uint64_t g = SV(0x01, 0, 4 * MB, 0);
            uint8_t *src = (uint8_t *)g_wb_src, *dst = (uint8_t *)g_wb_dst;
            src[0x10] = 0x11; g_wb_phase = 1;
            uint64_t m = SV(0x04, g_wb_dst, g_wb_src, 0x8000); uint8_t seen = dst[0x10];
            CHECK(!g && !m && g_wb_hit[0] >= 1 && !g_wb_bad_lock && !g_wb_in_tx && seen == 0x5a,
                  "P1-48 映射：期望 长堆 0、映射 0、拷贝前报源 host_write ≥1 次、回调时不持锁、不在换页事务里、栈上见源的 GPU 值 0x5a → 实 %#llx、%#llx、%d 次、持锁 %d、事务中 %d、%#x",
                  (unsigned long long)g, (unsigned long long)m, g_wb_hit[0], g_wb_bad_lock, g_wb_in_tx, seen);
            g_wb_bad_lock = g_wb_in_tx = 0; dst[0x20] = 0x22; g_wb_phase = 2;
            uint64_t u = SV(0x05, g_wb_dst, g_wb_src, 0x8000); uint8_t back = src[0x20];
            int kfull = kprot(st + 0x14000), klo = kprot(st + 0x10000), khi = kprot(st + 0x18000);
            CHECK(!u && g_wb_hit[1] >= 1 && g_wb_hit[2] >= 1 && !g_wb_bad_lock && !g_wb_in_tx && back == 0xa5,
                  "P1-48 解除：期望 解除 0、拷贝前报栈 ≥1 次、报源 ≥1 次、回调时不持锁、不在换页事务里、源见栈上的 GPU 值 0xa5 → 实 %#llx、%d、%d、持锁 %d 次、事务中 %d、%#x",
                  (unsigned long long)u, g_wb_hit[1], g_wb_hit[2], g_wb_bad_lock, g_wb_in_tx, back);
            CHECK(kfull == 0 && klo == (VM_PROT_READ | VM_PROT_WRITE) && khi == (VM_PROT_READ | VM_PROT_WRITE),
                  "P2-9 不对齐解除：期望 完全盖住的宿主页保护 0、两头盖不满的宿主页保护 3 / 3 → 实 %d、%d / %d", kfull, klo, khi);
            #undef SV
            fflush(stdout); _exit(bad ? 1 : 0);
        }
        int cs = 0; pid_t cw = c > 0 ? waitpid(c, &cs, 0) : -1;
        CHECK(cw == c && c > 0 && WIFEXITED(cs) && WEXITSTATUS(cs) == 0, "P1-48 / P2-9 子进程 期望 正常退出 0 → 实 退出 %d 信号 %d（信号 14 = 闹钟 20 秒）",
              WIFEXITED(cs) ? WEXITSTATUS(cs) : -1, WIFSIGNALED(cs) ? WTERMSIG(cs) : 0);
        munmap(R, RS + 2 * MB);
    }
    {   // 审计 #1534 P1-70：共享内存映射块数没有 64 这种上限（Horizon / Ryujinx 都没有；上游只在内存块表分配不出时回 OutOfResource）。
        //   期望按上游语义直接写：80 块 32 KB 共享内存逐块映射都回 0、再逐块解除都回 0；映射表满时回 OutOfResource 0xCE01，
        //   且目标页不被改（宿主保护仍是 0）。子进程里跑：旧实现第 65 块直接 die（abort）
        const uint64_t MB = 1ull << 20, RS = 32 * MB; const int NSH = 80;
        uint8_t *R = mmap(NULL, RS + 2 * MB, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(R != MAP_FAILED, "P1-70 单测：预留 32 MB");
        uint64_t b = ((uint64_t)R + 2 * MB - 1) & ~(2 * MB - 1), hz = b, az = b + 8 * MB, st = b + 16 * MB, fr = b + 24 * MB;
        g_shm_n = 0x8000; g_shm = mmap(NULL, g_shm_n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
        fflush(stdout); pid_t c = fork();
        if (c == 0) {
            struct rlimit no_core = {0, 0}; setrlimit(RLIMIT_CORE, &no_core);
            s2a_mem_test_setup(az, 8 * MB, b, RS, (void *)hz, 0); s2a_mem_test_sysres(0x2000000, 8 * MB); s2a_mem_test_stack(st, 8 * MB);
            uint64_t r[8]; int mapped = 0, unmapped = 0;
            #define SV(n, x0, x1, x2, x3) (memset(r, 0, sizeof r), r[0] = (x0), r[1] = (x1), r[2] = (x2), r[3] = (x3), svc_mem((n), r, 0), r[0])
            for (int k = 0; k < NSH; k++) { uint64_t e = SV(0x13, 0x77, fr + (uint64_t)k * 0x8000, 0x8000, 3); if (e == 0) mapped++; else { printf("  第 %d 块映射回 %#llx\n", k + 1, (unsigned long long)e); break; } }
            for (int k = 0; k < mapped; k++) if (SV(0x14, 0x77, fr + (uint64_t)k * 0x8000, 0x8000, 0) == 0) unmapped++;
            // 映射表填满（每项 4 KB 记账，落在预留区之外，只占表不碰页），再映射一块共享内存
            for (int k = 0; k < 4096; k++) s2a_mem_test_map(b + 64 * MB + (uint64_t)k * 0x1000, 0x1000);
            uint64_t full = SV(0x13, 0x77, fr, 0x8000, 3);
            int prot = kprot(fr);
            #undef SV
            printf("  P1-70 子进程：映射 %d / %d 块、解除 %d 块、映射表满时回 %#llx、目标页保护 %d\n", mapped, NSH, unmapped, (unsigned long long)full, prot);
            fflush(stdout);
            _exit(mapped == NSH && unmapped == NSH && full == 0xCE01 && prot == 0 ? 0 : 1);
        }
        int cs = 0; pid_t cw = c > 0 ? waitpid(c, &cs, 0) : -1;
        CHECK(cw == c && c > 0 && WIFEXITED(cs) && WEXITSTATUS(cs) == 0, "P1-70：期望 80 块共享内存映射 / 解除全回 0、映射表满回 0xCE01 且目标页不动、子进程正常退出 0 → 实 退出 %d 信号 %d（信号 6 = die / abort）",
              WIFEXITED(cs) ? WEXITSTATUS(cs) : -1, WIFSIGNALED(cs) ? WTERMSIG(cs) : 0);
        munmap(g_shm, g_shm_n); g_shm = NULL; munmap(R, RS + 2 * MB);
    }
    {   // 36 位款（NPDM 地址空间类型 1，11-06 Minecraft: Story Mode）：QueryMemory 的地址空间末照 Ryujinx AddrSpaceEnd = 2^36，之上报不可访问；39 位对照仍到 2^39。
        //   布局取 t_layout 的 36 位手算值（窗口 12G 起 32G、堆 +2G、别名 +8G 6G），纯记账、不碰宿主内存
        const uint64_t G = 1ull << 30, W36 = 0x300000000ull, T36 = 1ull << 36; s2a_meminfo mi;
        s2a_mem_test_setup(W36 + 8 * G, 6 * G, W36, 32 * G, (void *)(W36 + 2 * G), 0); s2a_mem_test_astype(1);
        s2a_mem_query(T36 + 0x4000, &mi);
        CHECK(mi.state == 0x10 && mi.base == T36 && mi.base + mi.size == 0, "36 位：2^36 之上 → 不可访问 [2^36, 2^64)（实 状态 %#x 起 %#llx 长 %#llx）", mi.state, (unsigned long long)mi.base, (unsigned long long)mi.size);
        s2a_mem_query(W36 + 40 * G, &mi);
        CHECK(mi.state == 0x10 && mi.base == W36 + 32 * G && mi.base + mi.size == T36, "36 位：窗口末到 2^36 → 不可访问、止于 2^36（实 起 %#llx 止 %#llx）", (unsigned long long)mi.base, (unsigned long long)(mi.base + mi.size));
        s2a_mem_test_astype(3); s2a_mem_query(T36 + 0x4000, &mi);
        CHECK(mi.state == 0x10 && mi.base == W36 + 32 * G && mi.base + mi.size == 1ull << 39, "39 位对照：窗口末之上止于 2^39（实 起 %#llx 止 %#llx）", (unsigned long long)mi.base, (unsigned long long)(mi.base + mi.size));
        // #1419：36 位款窗口放在 2^36 以上按 2^36 对齐的一块（WH = 7 × 2^36）。期望按定义手写：QueryMemory 的地址空间末 = 这一块的末 WH + 2^36；
        //   IPC 描述符只带地址 0–35 位（老 SDK），补上块基址后是游戏已映射的地址（堆）才补，补不上 / 本来就是游戏地址 / 0 原样；39 位款对照仍只试补第 39 位
        { uint64_t s2a_ipc_addr39(uint64_t); const uint64_t WH = 7ull << 36, HP = WH + 2 * G, HL = 64ull << 20;
          s2a_mem_test_setup(WH + 8 * G, 6 * G, WH, 32 * G, (void *)HP, HL); s2a_mem_test_astype(1);
          s2a_mem_query(WH + 40 * G, &mi);
          CHECK(mi.state == 0x10 && mi.base == WH + 32 * G && mi.base + mi.size == WH + T36, "36 位高位块：窗口末到块末 → 不可访问、止于 %#llx（实 起 %#llx 止 %#llx）", (unsigned long long)(WH + T36), (unsigned long long)mi.base, (unsigned long long)(mi.base + mi.size));
          CHECK(s2a_ipc_addr39((HP + 0x10) & (T36 - 1)) == HP + 0x10, "36 位高位块：描述符丢了 36 位以上的堆地址 → 补回块基址（实 %#llx）", (unsigned long long)s2a_ipc_addr39((HP + 0x10) & (T36 - 1)));
          CHECK(s2a_ipc_addr39(HP + 0x10) == HP + 0x10, "36 位高位块：本来就是游戏地址 → 原样");
          CHECK(s2a_ipc_addr39((HP + HL + 0x1000) & (T36 - 1)) == ((HP + HL + 0x1000) & (T36 - 1)), "36 位高位块：补上也不是已映射的游戏地址 → 原样");
          CHECK(s2a_ipc_addr39(0) == 0, "36 位高位块：0 → 0");
          s2a_mem_test_astype(3);
          CHECK(s2a_ipc_addr39((HP + 0x10) & (T36 - 1)) == ((HP + 0x10) & (T36 - 1)), "39 位对照：不按 2^36 块补（只试第 39 位，补上不是游戏地址 → 原样）");
          s2a_mem_test_setup(W36 + 8 * G, 6 * G, W36, 32 * G, (void *)(W36 + 2 * G), HL); s2a_mem_test_astype(1);
          CHECK(s2a_ipc_addr39(W36 + 2 * G + 0x10) == W36 + 2 * G + 0x10, "36 位窗口在 2^36 以下（原位置）：原样，不补"); }
    }
    printf("%s t_svc_mem：%d / %d 项通过（对拍 %d 区间不一致 %d；64 MB 整段查 8 字节 %d 次 老 %.1f ms → 新 %.2f ms）\n", bad ? "✗" : "✓", checks - bad, checks, N, diff, M, told, tnew);
    {   // #258 文件后备下还页懒清零：解除时锁内只改成不可访问并记「待清零」，不打洞；页重新放出来时就地清零（0x2C / MapMemory 拷贝路径）或只撤销标记（MapMemory 对齐路径盖掉原映射）。
        //   期望按语义手写：解除后页立刻不可访问、待清零计数 = 页数；放出来一律读到零 / 源内容；非文件后备路径行为不变
        const uint64_t HP = 0x4000, AZ = 4ull << 20;
        uint8_t *az = mmap(NULL, AZ, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(az != MAP_FAILED, "#258 懒清零单测：预留别名区");
        uint64_t b = (uint64_t)az, r[8], inl, fg, inl0, fg0;
        #define PHYS(n, a, sz) (memset(r, 0, sizeof r), r[0] = (uint64_t)(a), r[1] = (sz), svc_mem((n), r, 0), r[0])
        #define SV3(n, x0, x1, x2) (memset(r, 0, sizeof r), r[0] = (x0), r[1] = (x1), r[2] = (x2), svc_mem((n), r, 0), r[0])
        #define NZ(p, n) ({ uint64_t _z = 0; for (uint64_t _i = 0; _i < (n); _i++) _z += ((uint8_t *)(p))[_i] != 0; _z; })
        // 0) 非文件后备（桩判不在窗口）：解除即按原路还页，没有待清零
        g_gfile = 0;
        s2a_mem_test_setup(b, AZ, b, AZ, NULL, 0); s2a_mem_test_sysres(0x2000000, 0);
        uint64_t rc0 = PHYS(0x2C, b, 8 * HP); memset(az, 0xA5, 8 * HP); uint64_t rc1 = PHYS(0x2D, b, 8 * HP);
        CHECK(!rc0 && !rc1 && kprot(b) == 0 && s2a_mem_test_punch_pending() == 0, "#258 非文件后备：解除后不可访问、无待清零（rc %#llx/%#llx 保护 %d 待 %llu）", (unsigned long long)rc0, (unsigned long long)rc1, kprot(b), (unsigned long long)s2a_mem_test_punch_pending());
        // 1) 文件后备：解除 8 页 → 立刻不可访问、待清零 8；映射回来 → 128 KB 全零、就地清零 8 页、待清零 0
        g_gfile = 1; g_gfile_lo = b; g_gfile_hi = b + AZ;
        s2a_mem_test_setup(b, AZ, b, AZ, NULL, 0); s2a_mem_test_sysres(0x2000000, 0); s2a_mem_test_punch_stats(&inl0, &fg0);
        rc0 = PHYS(0x2C, b, 8 * HP); memset(az, 0xA5, 8 * HP); rc1 = PHYS(0x2D, b, 8 * HP);
        uint64_t pend1 = s2a_mem_test_punch_pending(); int kp0 = kprot(b), kp7 = kprot(b + 7 * HP);
        uint64_t rc2 = PHYS(0x2C, b, 8 * HP); uint64_t nz = NZ(az, 8 * HP); s2a_mem_test_punch_stats(&inl, &fg);
        CHECK(!rc0 && !rc1 && !rc2 && pend1 == 8 && kp0 == 0 && kp7 == 0 && nz == 0 && inl == inl0 + 8 && s2a_mem_test_punch_pending() == 0, "#258 解除 8 页再映射回来：立刻不可访问、待清零 8，回来全零、就地清零 8、待 0（rc %#llx/%#llx/%#llx 待 %llu 保护 %d/%d 非零 %llu 清零 %llu→%llu）", (unsigned long long)rc0, (unsigned long long)rc1, (unsigned long long)rc2, (unsigned long long)pend1, kp0, kp7, (unsigned long long)nz, (unsigned long long)inl0, (unsigned long long)inl);
        // 2) 解除前 4 页、只映射回第 1 页：待清零 3，第 1 页全零可访问，第 0 / 2 页仍不可访问；再全部映射回来 → 待 0
        memset(az, 0xA5, 8 * HP); rc1 = PHYS(0x2D, b, 4 * HP); rc2 = PHYS(0x2C, b + HP, HP);
        uint64_t pend3 = s2a_mem_test_punch_pending(); nz = NZ(az + HP, HP); kp0 = kprot(b); int kp1 = kprot(b + HP), kp2 = kprot(b + 2 * HP);
        // 即时存档存游戏内存时按这个跳过已释放待清零的页（snapshot.c dump_mem）：第 0 / 2 / 3 页待清零 → 1；第 1 页已映射回来、第 4 页从没解除 → 0
        int rl0 = s2a_snap_page_released(b), rl1 = s2a_snap_page_released(b + HP), rl2 = s2a_snap_page_released(b + 2 * HP), rl3 = s2a_snap_page_released(b + 3 * HP), rl4 = s2a_snap_page_released(b + 4 * HP);
        CHECK(rl0 == 1 && rl1 == 0 && rl2 == 1 && rl3 == 1 && rl4 == 0, "#258 存档按页查已释放待清零：第 0/1/2/3/4 页期望 1/0/1/1/0，真结果 %d/%d/%d/%d/%d", rl0, rl1, rl2, rl3, rl4);
        uint64_t rc3 = PHYS(0x2C, b, 8 * HP);
        CHECK(s2a_snap_page_released(b) == 0 && s2a_snap_page_released(b + 2 * HP) == 0, "#258 全部映射回来后存档查已释放：期望第 0 / 2 页都 0");
        CHECK(!rc1 && !rc2 && !rc3 && pend3 == 3 && nz == 0 && kp0 == 0 && kp2 == 0 && kp1 != 0 && s2a_mem_test_punch_pending() == 0, "#258 中间一页映射回来：待 3、该页全零可访问、两侧仍不可访问，全映射回来后待 0（rc %#llx/%#llx/%#llx 待 %llu 非零 %llu 保护 %d/%d/%d 待 %llu）", (unsigned long long)rc1, (unsigned long long)rc2, (unsigned long long)rc3, (unsigned long long)pend3, (unsigned long long)nz, kp0, kp1, kp2, (unsigned long long)s2a_mem_test_punch_pending());
        // 3) 栈区 MapMemory 拷贝路径（大小不按宿主页对齐）：UnmapMemory 释放的整宿主页记待清零、不可访问；再 MapMemory 到同处先清零再拷 → 待清零撤销、就地清零 +1、内容等于源
        uint64_t S = b + (1ull << 20); s2a_mem_test_stack(S, 4 * HP);
        memset(az, 0x3C, 2 * HP); s2a_mem_test_punch_stats(&inl0, &fg0);
        rc0 = SV3(0x04, S, b, HP + 0x1000); rc1 = SV3(0x05, S, b, HP + 0x1000);
        uint64_t pendS = s2a_mem_test_punch_pending(); int kpS = kprot(S);
        memset(az, 0x5A, 2 * HP); rc2 = SV3(0x04, S, b, HP + 0x1000); s2a_mem_test_punch_stats(&inl, &fg);
        int eq = memcmp((void *)S, (void *)b, HP + 0x1000) == 0; uint64_t pendS2 = s2a_mem_test_punch_pending();
        rc3 = SV3(0x05, S, b, HP + 0x1000);
        CHECK(!rc0 && !rc1 && !rc2 && !rc3 && pendS == 1 && kpS == 0 && inl == inl0 + 1 && fg == fg0 && eq && pendS2 == 0 && s2a_mem_test_punch_pending() == 1, "#258 栈区拷贝路径：解除后待 1 不可访问，再映射就地清零 +1、内容等于源、待 0，再解除待 1（rc %#llx/%#llx/%#llx/%#llx 待 %llu 保护 %d 清零 %llu→%llu 撤销 %llu→%llu 相等 %d 待 %llu→%llu）", (unsigned long long)rc0, (unsigned long long)rc1, (unsigned long long)rc2, (unsigned long long)rc3, (unsigned long long)pendS, kpS, (unsigned long long)inl0, (unsigned long long)inl, (unsigned long long)fg0, (unsigned long long)fg, eq, (unsigned long long)pendS2, (unsigned long long)s2a_mem_test_punch_pending());
        // 4) 栈区 MapMemory 对齐路径（vm_remap 共享源页盖掉原映射）：上面留下的待清零页只撤销标记、不清零，内容等于源；解除后待清零仍 0
        memset(az, 0x77, HP); s2a_mem_test_punch_stats(&inl0, &fg0);
        rc0 = SV3(0x04, S, b, HP); s2a_mem_test_punch_stats(&inl, &fg); eq = memcmp((void *)S, (void *)b, HP) == 0; pendS = s2a_mem_test_punch_pending();
        rc1 = SV3(0x05, S, b, HP);
        CHECK(!rc0 && !rc1 && fg == fg0 + 1 && inl == inl0 && eq && pendS == 0 && s2a_mem_test_punch_pending() == 0, "#258 栈区对齐路径：待清零只撤销标记 +1、不清零、内容等于源、待 0（rc %#llx/%#llx 撤销 %llu→%llu 清零 %llu→%llu 相等 %d 待 %llu/%llu）", (unsigned long long)rc0, (unsigned long long)rc1, (unsigned long long)fg0, (unsigned long long)fg, (unsigned long long)inl0, (unsigned long long)inl, eq, (unsigned long long)pendS, (unsigned long long)s2a_mem_test_punch_pending());
        PHYS(0x2D, b, 8 * HP);
        #undef NZ
        #undef SV3
        #undef PHYS
        g_gfile = 0; munmap(az, AZ);
    }
    return bad ? 1 : 0;
}
