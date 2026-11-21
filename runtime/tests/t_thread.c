// 线程层单测（只链 runtime/src/thread.c，其余设施在本文件打桩）。
// ① 条件变量丢唤醒（旧库资源表读写锁卡死的根因，旧库 t_condvar 搬来）：W 持锁调 0x1C；钩子落在 0x1C「放锁之后、挂链之前」，此刻让 R 按 SDK 用户态写法动作：
//    CAS 0→R 抢锁 → 读条件键，非 0 才发 0x1D(键, 1)（SDK InternalConditionVariable::Signal 同此）→ 放锁：锁字仍是 R 的标签则 CAS→0，带等待位则 0x1B。
//    期望（Horizon 语义：等待者先发布「有等待」再放锁）：W 的 0x1C 返回 0；返回时锁字 = W 的标签；条件键 = 0。
// ② 句柄表：C# 种类句柄发放 / 查询 / 关闭回调；旧句柄关后再查无效（序号防复用）；伪句柄。
// ③ 事件：C# 建的事件触发后游戏线程 WaitSynchronization 立即返回下标 0；超时返回 0xEA01。
// ④ SignalToAddress 类型 2（按等待数改值）：期望值按 Horizon 7.0.0+ 规则手算（Atmosphère KAddressArbiter；Ryujinx 全唤醒同为 −2）：
//    没人等 +1；个数 ≤ 0 且有人等 −2；等待数 ≤ 个数 −1；等待数 > 个数 不变。唤醒数 = 个数 ≤ 0 ? 全部 : min(个数, 等待数)。
// ⑤ 争锁压力：4 条游戏线程按 SDK 用户态写法（CAS 抢锁、置等待位后 0x1A、放锁 CAS 不成则 0x1B）各进出临界区 20000 次，
//    期望：临界区内同时只有 1 条（重入计数恒为 0）、非原子计数 = 80000。反例：同样线程不加锁跑，检测器必须报出重叠（证明判据能抓到违规）。
// ⑥ 线程名：主线程登记后名为「nx-<GetThreadId 返回的 id>」（能耗 / sample 拆账按线程名对上线程表）。
#define S2A_LOGCAP_IMPL   // 本单测给 s2a_log_out 强定义：照常写出，另把现场导出的「同步调用」行截下来判（㉙）
#include "../src/native.h"
#include <string.h>
#include <stdatomic.h>
static char g_cap[65536]; static size_t g_capn; static _Atomic int g_cap_on;
#ifndef S2A_LOG_OUT_WEAK   // 安卓暂停夹具（runtime/tests/support/run_pause_wait_host.py）把 thread.c 与本文件拼进同一单元、thread.c 先带进了弱定义：那里不跑 ㉙，不再定义
void s2a_log_out(int fd, const char *p, size_t n) {
    ssize_t w = write(fd, p, n); (void)w;
    if (atomic_load(&g_cap_on) && (strstr(p, "同步调用") || strstr(p, "寄存器 x") || strstr(p, "键周围内存")) && g_capn + n < sizeof g_cap) { memcpy(g_cap + g_capn, p, n); g_capn += n; g_cap[g_capn] = 0; }
}
#endif
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif
#ifdef S2A_THREAD_PLAT_VARIANT
#define main t_thread_main
#endif
// ---- 桩 ----
pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
void sw_gate(void) {}
// 同生产（crash.c）：每条游戏线程装一块备用信号栈，抢占信号（SA_ONSTACK）的信号帧落在它上面、不压游戏栈。
//   原先是空桩：信号帧直接压在游戏栈 sp 以下（Darwin 先跳过 128 字节红区），⑲b 量到的是这个与生产不同的差，量不到 tramp 自己踩没踩
void s2a_crash_thread_init(void) {
    static __thread int done; if (done) return;
    stack_t cur; if (!sigaltstack(NULL, &cur) && !(cur.ss_flags & SS_DISABLE)) { done = 1; return; }   // 已有一块（如 ASan 自己装的）：留着，换掉的话它退线程时 munmap 我们的内存会失败
    stack_t ss = {.ss_sp = malloc(64u << 10), .ss_size = 64u << 10, .ss_flags = 0};
    if (ss.ss_sp && !sigaltstack(&ss, NULL)) done = 1;
}
static uint8_t g_tlr_pool[16][0x200]; static int g_ntlr;
void *s2a_tlr_alloc(void) { return g_tlr_pool[g_ntlr++ & 15]; }
void s2a_tlr_free(void *p) { (void)p; }
// svc_mem.c 传输内存借出 / 还回源的桩（审计 P2-11）：记最近一次参数与次数；g_tb_rc 非 0 时借出失败回它
static _Atomic int g_tb_n, g_tu_n; static uint64_t g_tb_a, g_tb_s, g_tu_a, g_tu_s; static uint32_t g_tb_p, g_tb_rc;
uint32_t s2a_mem_tmem_borrow(uint64_t a, uint64_t n, uint32_t p) { g_tb_n++; g_tb_a = a; g_tb_s = n; g_tb_p = p; return g_tb_rc; }
void s2a_mem_tmem_unborrow(uint64_t a, uint64_t n) { g_tu_n++; g_tu_a = a; g_tu_s = n; }
void s2a_mem_tmem_close(uint64_t a, uint64_t n) { s2a_mem_tmem_unborrow(a, n); }   // 关句柄走它（还不还回由 svc_mem 按映射定，这里只数调用）
uint32_t s2a_native_handle_new(uint32_t sub, void *obj);
static int g_cf_n; static void *g_cf_o; void s2a_mem_codemem_free(void *o) { g_cf_n++; g_cf_o = o; }   // 代码内存最后一个句柄关掉：交 svc_mem 还回源再释放
// vawin.c 窗口尾部分配的桩（照样 mmap / munmap，计次数）：「宿主栈回收」用例看建 / 还是否成对
static _Atomic int g_tm_map, g_tm_unmap;
void *s2a_tail_map(uint64_t len, int user, int prot) { (void)user; atomic_fetch_add(&g_tm_map, 1); void *p = mmap(NULL, len, prot, MAP_PRIVATE | MAP_ANON, -1, 0); return p == MAP_FAILED ? NULL : p; }
void s2a_tail_unmap(void *p, uint64_t len, int user) { (void)user; atomic_fetch_add(&g_tm_unmap, 1); munmap(p, len); }
void s2a_low_holes(uint64_t *total, uint64_t *largest) { *total = *largest = 0; }   // vawin.c：建线程失败时打地址余量
static _Atomic int g_closed_kind; static _Atomic uint64_t g_closed_cookie;
void s2a_cs_handle_closed(uint32_t kind, uint64_t cookie) { g_closed_kind = (int)kind; g_closed_cookie = cookie; }
void s2a_cs_thread_exit(uint32_t t) { (void)t; }
static void (*g_hw_hook)(void);   // 暂停读上下文用：0x33 放开全局锁、写游戏内存前的这一刻（模拟并发恢复）
void s2a_host_write_hook(uint64_t a, uint64_t n) { (void)a; (void)n; if (g_hw_hook) g_hw_hook(); }
// ⑦ 用：空转函数所在的一段地址算「游戏代码」（让 SetThreadActivity 走 Mach 挂起分支）
static void guest_spin(uint64_t arg);
// 冻结代放核用例只在苹果正常构建跑：安卓暂停夹具（run_pause_wait_host.py）把 thread.c 拼进同一单元、关掉了苹果段（没有 s2a_snap_freeze / thaw），
//   那时 native.h 先于本文件定义了 S2A_LOG_OUT_WEAK（本文件正常构建时自己先定义 S2A_LOGCAP_IMPL，不会有它）
#if defined(__APPLE__) && !defined(S2A_LOG_OUT_WEAK)
#define T_FZ 1
static void fz_spin(uint64_t arg);   // 冻结代放核：持核在游戏代码里忙等（module_of 桩认它）
#endif
static void preempt_spin(uint64_t arg);   // ⑫ 用：纯自旋客体线程（时钟抢占测）
static void ss_spin(uint64_t arg);        // ⑮ 用：纯自旋客体线程（调度计数正对照要在游戏代码里暂停它）
static void ua_spin(uint64_t arg);        // ⑰ 用：纯自旋客体线程（游戏代码里暂停 / 恢复它）
static void rp_spin(uint64_t arg);        // ⑱ 用：被连续抢占两次的纯自旋低优先级线程
static void rz_spin(uint64_t arg);        // ⑲ 用：用 sp 下方（红区）存临时值的纯自旋线程（同改写器补丁区桩），被反复抢占
static void bz_spin(uint64_t arg);        // ⑲b 用：sp 下方 4 KB 都是活数据（游戏收回 sp 后还用的变长数组，#515），被反复抢占
static void start_probe(uint64_t arg);    // ⑯ 用：入口算游戏代码（sw_is_host_code 桩对它回 0），进门第一件事读持核
static void mg_spin(uint64_t arg);        // ⑳ 用：高优先级、掩码 0x7 的纯自旋持核者（巫师3 现场 id 126 / 117）
static void hz_spin(uint64_t arg);        // Horizon 时间片轮转用：优先级 59 的纯自旋持核者
static void pm_spin(uint64_t arg);        // P1-49 用：在游戏代码里空转、被 Mach 挂住的线程
int svc_thread(int num, uint64_t *regs);
// 暂停中不释放：在宿主代码里空转（不在 module_of 认的游戏代码段里），收到 g_np_go 才退出
static _Atomic int g_np_ready, g_np_go, g_np_in_pause, g_np_free_in, g_np_free_after; static uint32_t g_np_h; static void *volatile g_np_t;
static void np_host_spin(uint64_t arg) { (void)arg; atomic_store(&g_np_ready, 1); while (!atomic_load(&g_np_go)) { } uint64_t e[31] = {0}; svc_thread(0x0A, e); }
static void np_freed(void *t) { if (t != g_np_t) return; atomic_fetch_add(atomic_load(&g_np_in_pause) ? &g_np_free_in : &g_np_free_after, 1); }
static void np_iter(void *t) {   // 0x32 查完句柄之后：放目标退出（还没设暂停，它照常走 ExitThread），等它退完，再关它的句柄（关句柄见已退出 → 释放对象）
    extern void (*s2a_test_after_lookup)(void *); s2a_test_after_lookup = NULL; g_np_t = t;
    atomic_store(&g_np_go, 1); usleep(200000);
    atomic_store(&g_np_in_pause, 1); uint64_t c[31] = {g_np_h}; svc_thread(0x16, c); atomic_store(&g_np_in_pause, 0); }   // 关句柄这一刻 0x32 还要用对象
static module_t g_fake_mod;
module_t *module_of(const void *p) {
    uint64_t a = (uint64_t)p, lo = (uint64_t)(void *)guest_spin, lo2 = (uint64_t)(void *)preempt_spin;
    if (a >= lo && a < lo + 0x400) { g_fake_mod.base = (uint8_t *)lo; g_fake_mod.patch_off = -1; return &g_fake_mod; }
    if (a >= lo2 && a < lo2 + 0x400) { g_fake_mod.base = (uint8_t *)lo2; g_fake_mod.patch_off = -1; return &g_fake_mod; }
    uint64_t lo6 = (uint64_t)(void *)rz_spin;
    uint64_t lobz = (uint64_t)(void *)bz_spin;
    if (a >= lobz && a < lobz + 0x400) { g_fake_mod.base = (uint8_t *)lobz; g_fake_mod.patch_off = -1; return &g_fake_mod; }
    if (a >= lo6 && a < lo6 + 0x400) { g_fake_mod.base = (uint8_t *)lo6; g_fake_mod.patch_off = -1; return &g_fake_mod; }
    uint64_t lo5 = (uint64_t)(void *)rp_spin;
    if (a >= lo5 && a < lo5 + 0x400) { g_fake_mod.base = (uint8_t *)lo5; g_fake_mod.patch_off = -1; return &g_fake_mod; }
    uint64_t lo4 = (uint64_t)(void *)ua_spin;
    if (a >= lo4 && a < lo4 + 0x400) { g_fake_mod.base = (uint8_t *)lo4; g_fake_mod.patch_off = -1; return &g_fake_mod; }
    uint64_t lo7 = (uint64_t)(void *)mg_spin;
    if (a >= lo7 && a < lo7 + 0x400) { g_fake_mod.base = (uint8_t *)lo7; g_fake_mod.patch_off = -1; return &g_fake_mod; }
    uint64_t lo8 = (uint64_t)(void *)hz_spin;
    if (a >= lo8 && a < lo8 + 0x400) { g_fake_mod.base = (uint8_t *)lo8; g_fake_mod.patch_off = -1; return &g_fake_mod; }
    uint64_t lo9 = (uint64_t)(void *)pm_spin;
    if (a >= lo9 && a < lo9 + 0x400) { g_fake_mod.base = (uint8_t *)lo9; g_fake_mod.patch_off = -1; return &g_fake_mod; }
#ifdef T_FZ
    uint64_t lo10 = (uint64_t)(void *)fz_spin;
    if (a >= lo10 && a < lo10 + 0x400) { g_fake_mod.base = (uint8_t *)lo10; g_fake_mod.patch_off = -1; return &g_fake_mod; }
#endif
    uint64_t lo3 = (uint64_t)(void *)ss_spin;
    if (a >= lo3 && a < lo3 + 0x400) { g_fake_mod.base = (uint8_t *)lo3; g_fake_mod.patch_off = -1; return &g_fake_mod; }
    return NULL;
}
int sw_is_host_code(uint64_t a) { return a != (uint64_t)(void *)start_probe; }
void sw_xstack_thread_init(uint64_t top, uint64_t gtop) { (void)top; (void)gtop; }
// plat.h stubs: the test links thread.c only.  native-thread-plat compiles
// this file with S2A_TEST_REAL_PLAT and links platform/apple/plat_apple.c.
#ifndef S2A_TEST_REAL_PLAT
uint64_t plat_tsd_base(void) { return 0; }
int plat_tsd_offset(uint32_t key, uint64_t *out) { if (!out) return EINVAL; *out = (uint64_t)key * 8; return 0; }
int plat_fault_install(int sig, void (*fn)(int, siginfo_t *, void *), unsigned flags) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = fn; sa.sa_flags = SA_SIGINFO;
    if (flags & 1u) sa.sa_flags |= SA_ONSTACK;
    if (flags & 2u) sa.sa_flags |= SA_RESTART;
    sigemptyset(&sa.sa_mask);
    return sigaction(sig, &sa, NULL) ? errno : 0;
}
void plat_fault_altstack_free(void) {}
#endif
void sw_xstack_thread_fini(void) {}
void sw_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) { (void)sp; ((void (*)(uint64_t))entry)(arg); }
uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
int svc_thread(int num, uint64_t *regs); uint32_t s2a_thread_main_init(void);
void (*s2a_test_cv_after_release)(void);

static int bad;
#define CHECK(c, ...) do { if (!(c)) { printf("✗ " __VA_ARGS__); printf("\n"); bad++; } } while (0)

// ---- ① ----
static _Atomic uint32_t g_mutex, g_key; static uint32_t g_wtag, g_rtag;   // R 的锁标签 = 它的真句柄（锁字持有者必须是线程句柄，否则照 Ryujinx 回 InvalidHandle）
static _Atomic int g_rstate;   // 0 未动 1 已抢到锁 2 将发 0x1D 3 完成
static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_c = PTHREAD_COND_INITIALIZER; static int g_go, g_ready;
static void state_set(int st) { atomic_store(&g_rstate, st); pthread_mutex_lock(&g_m); pthread_cond_broadcast(&g_c); pthread_mutex_unlock(&g_m); }
static _Atomic uint32_t g_tlr110, g_tlr110_me; static _Atomic int g_tlr110_done;
static void tlr110_worker(uint64_t arg) { (void)arg;   // 建出的游戏线程：TLR+0x110 应是自己的句柄（内核 svcCreateThread 语义）
    uint32_t s2a_thread_current(void); void *s2a_thread_tlr(void);
    atomic_store(&g_tlr110, *(uint32_t *)((uint8_t *)s2a_thread_tlr() + 0x110)); atomic_store(&g_tlr110_me, s2a_thread_current());
    atomic_store(&g_tlr110_done, 1); uint64_t e[31] = {0}; svc_thread(0x0A, e); }
static void reader(uint64_t arg) {   // 游戏线程（经 CreateThread 起，入口是宿主函数）
    (void)arg;
    pthread_mutex_lock(&g_m); g_ready = 1; pthread_cond_broadcast(&g_c); while (!g_go) pthread_cond_wait(&g_c, &g_m); pthread_mutex_unlock(&g_m);
    uint32_t z = 0; while (!atomic_compare_exchange_strong(&g_mutex, &z, g_rtag)) { z = 0; sched_yield(); }
    state_set(1);
    if (atomic_load(&g_key)) { state_set(2); uint64_t r[31] = {(uint64_t)&g_key, 1}; svc_thread(0x1D, r); }
    uint32_t v = g_rtag;
    if (!atomic_compare_exchange_strong(&g_mutex, &v, 0)) { uint64_t r[31] = {(uint64_t)&g_mutex}; svc_thread(0x1B, r); }
    state_set(3);
    uint64_t r[31] = {0}; svc_thread(0x0A, r);   // ExitThread
}
static void hook(void) {   // 在 W 的 0x1C 里、持全局锁 G、已放锁未挂链：放 R 去跑，等它「完成」或「将发 0x1D（会卡在 G 上）」
    s2a_test_cv_after_release = NULL;
    pthread_mutex_lock(&g_m); g_go = 1; pthread_cond_broadcast(&g_c);
    while (atomic_load(&g_rstate) != 2 && atomic_load(&g_rstate) != 3) pthread_cond_wait(&g_c, &g_m);
    pthread_mutex_unlock(&g_m);
}
// 看门狗：整个单测超过 TIMEOUT_S 秒就报超时并点名当时在跑的用例（g_case）。只为抓真卡死（丢唤醒是永远不醒），所以上限给宽：
// 10-21 负载 40 时单跑 1.6–5.4 秒，门禁里几支并行能过 20 秒；旧版固定写「5 秒、丢唤醒」，① 早已通过（R 状态 3、锁字 = W 标签）也这么报，误导查因
#define TIMEOUT_S 60
static const char *volatile g_case = "① 条件变量";
static void timeout_kill(int s) { (void)s; printf("✗ 超时 %d 秒，卡在「%s」（① 的现场：R 状态 %d go %d 键 %u 锁字 %#x；R 状态 3 且锁字 = W 标签 = ① 已通过）\n✗ t_thread\n",
    TIMEOUT_S, g_case, atomic_load(&g_rstate), g_go, atomic_load(&g_key), atomic_load(&g_mutex)); fflush(stdout); _exit(1); }

// ---- ④ ----
int s2a_test_waiters(int kind, uint64_t a);
static _Atomic uint32_t g_aw;
static void addr_waiter(uint64_t arg) { (void)arg; uint64_t r[31] = {(uint64_t)&g_aw, 2, 10, 3000000000ull}; svc_thread(0x34, r); uint64_t e[31] = {0}; svc_thread(0x0A, e); }
static _Atomic int g_hs_done[2];
static void hs_exit(uint64_t arg) { atomic_store(&g_hs_done[arg & 1], 1); uint64_t e[31] = {0}; svc_thread(0x0A, e); }
// 超时前被唤醒用：等到等待方真进了等待队列（s2a_test_blocked）再触发事件——按事件同步，不按墙钟假定「10 ms 后它已在等」
struct ev_after_block { uint64_t ev; uint32_t waiter; };
int s2a_test_blocked(uint32_t h); uint32_t s2a_thread_current(void);
static void *ev_signal_after_block(void *p) { struct ev_after_block *a = p; while (!s2a_test_blocked(a->waiter)) usleep(100); s2a_event_signal(a->ev); return NULL; }
// 按时醒判机制（10-26 改）：每次睡 / 带超时的等待，向内核挂的应是 EVFILT_TIMER + NOTE_CRITICAL（不合并、不随 QoS 放宽）+ NOTE_NSECONDS + 一次性，
//   时长 = 剩余时长（≤ 要求、离要求不到 1 ms）。这是我们代码决定的；实际几点醒还叠着调度排队（忙机 utility 线程排不上 CPU，10-26 两次门禁假红），只报数不判。
//   旧做法 nanosleep 不挂定时器 → 计数不涨，判红
#include <sys/event.h>
int s2a_test_kq_timer(struct kevent64_s *out);
static int g_ontime_ok;   // 判过的按时醒项数（进总结行）
static int timer_ok(const char *nm, int n0, int64_t want) {
    struct kevent64_s ev; int n = s2a_test_kq_timer(&ev);
    int ok = n > n0 && ev.filter == EVFILT_TIMER && (ev.flags & EV_ONESHOT) && (ev.fflags & NOTE_CRITICAL) && (ev.fflags & NOTE_NSECONDS)
             && ev.data <= want && ev.data > want - 1000000;
    if (!ok) printf("✗ %s：应挂一次性 EVFILT_TIMER（NOTE_CRITICAL | NOTE_NSECONDS）、时长 ≤ %lld 且差 < 1 ms；实 新挂 %d 个、过滤器 %d、标志 %#x、fflags %#x、时长 %lld\n",
                    nm, (long long)want, n - n0, ev.filter, ev.flags, ev.fflags, (long long)ev.data);
    g_ontime_ok += ok; return ok;
}
static int64_t ns_since(struct timespec a) { struct timespec b; clock_gettime(CLOCK_MONOTONIC, &b); return (b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec); }
static int64_t sleep30(void *p) { (void)p; struct timespec a; clock_gettime(CLOCK_MONOTONIC, &a); uint64_t r[31] = {30000000}; svc_thread(0x0B, r); return ns_since(a) - 30000000; }
struct wait30 { int kind; uint32_t *hs; _Atomic int32_t *ow; int bad_rc; };
static int64_t wait30(void *p) { struct wait30 *w = p; struct timespec a; clock_gettime(CLOCK_MONOTONIC, &a);
    uint64_t r[31] = {0, (uint64_t)w->hs, 1, 30000000};
    if (w->kind) { r[0] = (uint64_t)w->ow; r[1] = 2; r[2] = 7; r[3] = 30000000; svc_thread(0x34, r); } else svc_thread(0x18, r);
    int64_t v = ns_since(a) - 30000000; w->bad_rc += r[0] != 0xEA01; return v; }
static _Alignas(4) _Atomic uint32_t g_sv; static _Atomic int g_sv_go, g_sv_done, g_sv_exit;
// ㉛：停帧现场给等条件的线程打锁 / 键周围 48 字节（#19361b）。结构 16 字节对齐，锁字在 +0x14、键在 +0x18，其余字预先写好
static _Alignas(16) _Atomic uint32_t g_kw[12]; static _Atomic int g_kw_done;
static void kw_waiter(uint64_t arg) { (void)arg;
    atomic_store(&g_kw[5], s2a_thread_current());   // 持锁进 0x1C（SDK 条件变量协议：等之前锁在自己手里）
    uint64_t r[31] = {(uint64_t)(uintptr_t)&g_kw[5], (uint64_t)(uintptr_t)&g_kw[6], s2a_thread_current(), (uint64_t)-1}; svc_thread(0x1C, r);
    atomic_store(&g_kw_done, 1); uint64_t e[31] = {0}; svc_thread(0x0A, e); }
static void sv_spinner(uint64_t arg) { (void)arg;   // ㉙：3 次 SignalToAddress 后在游戏代码里空转（不进系统调用 → 线程表里是「在跑」）
    for (int i = 0; i < 3; i++) { uint64_t r[31] = {(uint64_t)&g_sv, 0, 7, 1}; svc_thread(0x35, r); }
    atomic_store(&g_sv_done, 1); while (!atomic_load(&g_sv_go)) { }
    atomic_store(&g_sv_exit, 1); uint64_t e[31] = {0}; svc_thread(0x0A, e); }
static void spawn(void (*f)(uint64_t), uint64_t arg, uint32_t *h) {
    uint64_t r[31] = {0, (uint64_t)f, arg, 0, 44, 0}; svc_thread(0x08, r); if (h) *h = (uint32_t)r[1];
    uint64_t s_[31] = {r[1]}; svc_thread(0x09, s_);
}
// WaitForAddress 类型 3（WaitIfEqual64，HOS 19+；Switch 2 SDK nn::os::WaitForAddress(long const*, long) 只发这一种）：期望值按内核语义手写
static _Alignas(8) _Atomic uint64_t g_aw64; static _Atomic uint64_t g_aw64_rc = 1;
static void addr_waiter64(uint64_t arg) { (void)arg; uint64_t r[31] = {(uint64_t)&g_aw64, 3, 0x123456789ull, 3000000000ull}; svc_thread(0x34, r); atomic_store(&g_aw64_rc, r[0]); uint64_t e[31] = {0}; svc_thread(0x0A, e); }
static void arb64_case(void) {
    // ① 高 32 位不同、低 32 位相同（只比 32 位的错法会误等）→ InvalidState 0xFA01，立即返回
    atomic_store(&g_aw64, 0x223456789ull);
    uint64_t a[31] = {(uint64_t)&g_aw64, 3, 0x123456789ull, 1000000000ull}; svc_thread(0x34, a);
    CHECK(a[0] == 0xFA01, "WaitIfEqual64 值不等（高 32 位不同）：应回 0xFA01，实 %#llx", (unsigned long long)a[0]);
    // ② 只 4 字节对齐 → InvalidAddress 0xCC01；③ 类型 4 → InvalidEnumValue 0xF001
    uint64_t b[31] = {(uint64_t)&g_aw64 + 4, 3, 0, 0}; svc_thread(0x34, b);
    uint64_t c[31] = {(uint64_t)&g_aw64, 4, 0, 0}; svc_thread(0x34, c);
    CHECK(b[0] == 0xCC01 && c[0] == 0xF001, "WaitIfEqual64 非 8 字节对齐应回 0xCC01（实 %#llx）；类型 4 应回 0xF001（实 %#llx）", (unsigned long long)b[0], (unsigned long long)c[0]);
    // ④ 64 位值相等 → 挂起；SignalToAddress 类型 0 唤醒后返回 0
    atomic_store(&g_aw64, 0x123456789ull); atomic_store(&g_aw64_rc, 1);
    spawn(addr_waiter64, 0, NULL);
    uint64_t t0 = now_ns(); while (s2a_test_waiters(3, (uint64_t)&g_aw64) < 1 && now_ns() - t0 < 30000000000ull) sched_yield();
    int nw = s2a_test_waiters(3, (uint64_t)&g_aw64);
    uint64_t s_[31] = {(uint64_t)&g_aw64, 0, 0, 1}; svc_thread(0x35, s_);
    t0 = now_ns(); while (atomic_load(&g_aw64_rc) == 1 && now_ns() - t0 < 30000000000ull) sched_yield();
    CHECK(nw == 1 && s_[0] == 0 && atomic_load(&g_aw64_rc) == 0, "WaitIfEqual64 相等则挂起、被类型 0 唤醒：挂起 %d 个（期望 1）、发信号回 %#llx、等待方回 %#llx（期望 0）", nw, (unsigned long long)s_[0], (unsigned long long)atomic_load(&g_aw64_rc));
}
static void arb_case(int nw, int cnt, int32_t want, int want_woken) {
    atomic_store(&g_aw, 10);
    for (int i = 0; i < nw; i++) spawn(addr_waiter, 0, NULL);
    uint64_t t0 = now_ns(); while (s2a_test_waiters(3, (uint64_t)&g_aw) < nw && now_ns() - t0 < 2000000000ull) sched_yield();
    uint64_t r[31] = {(uint64_t)&g_aw, 2, 10, (uint64_t)(int64_t)cnt}; svc_thread(0x35, r);
    int left = s2a_test_waiters(3, (uint64_t)&g_aw);
    CHECK(r[0] == 0 && (int32_t)atomic_load(&g_aw) == want && nw - left == want_woken,
          "SignalToAddress 类型 2：等待 %d 个数 %d → 返回 %#llx 值 %d（期望 %d）唤醒 %d（期望 %d）", nw, cnt, r[0], (int32_t)atomic_load(&g_aw), want, nw - left, want_woken);
    uint64_t c[31] = {(uint64_t)&g_aw, 0, 0, (uint64_t)-1}; svc_thread(0x35, c);   // 放走剩下的
    t0 = now_ns(); while (s2a_test_waiters(3, (uint64_t)&g_aw) && now_ns() - t0 < 2000000000ull) sched_yield();
}
// ---- ⑤ ----
#define WAITER 0x40000000u
#define NTH 4
#define NIT 20000
static _Atomic uint32_t g_lk; static _Atomic int g_inside, g_overlap, g_done, g_use_lock; static volatile long g_count; static uint32_t g_tags[NTH];
static void sdk_lock(uint32_t tag) {
    for (;;) {
        uint32_t v = 0; if (atomic_compare_exchange_strong(&g_lk, &v, tag)) return;
        if (!(v & WAITER)) { if (!atomic_compare_exchange_strong(&g_lk, &v, v | WAITER)) continue; v |= WAITER; }
        uint64_t r[31] = {v & ~WAITER, (uint64_t)&g_lk, tag}; svc_thread(0x1A, r);
        if ((atomic_load(&g_lk) & ~WAITER) == tag) return;
    }
}
static void sdk_unlock(uint32_t tag) { uint32_t v = tag; if (!atomic_compare_exchange_strong(&g_lk, &v, 0)) { uint64_t r[31] = {(uint64_t)&g_lk}; svc_thread(0x1B, r); } }
static void locker(uint64_t i) {
    while (!g_tags[i]) sched_yield();   // 主线程拿到句柄后才填标签
    uint32_t tag = g_tags[i]; int lock = atomic_load(&g_use_lock);
    for (int k = 0; k < NIT; k++) {
        if (lock) sdk_lock(tag);
        if (atomic_fetch_add(&g_inside, 1) != 0) atomic_fetch_add(&g_overlap, 1);
        // 反例（不加锁）在临界区里让出一次：保证别的线程此时能进来——原先靠 4 线程恰好并行交错，机器负载 60 时排不到一起、重叠 0 次判据失效（10-21 画幅线报偶发红）
        if (!lock) sched_yield();
        long c = g_count; for (volatile int d = 0; d < 8; d++) {} g_count = c + 1;
        atomic_fetch_sub(&g_inside, 1);
        if (lock) sdk_unlock(tag);
    }
    atomic_fetch_add(&g_done, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void stress(int lock) {
    atomic_store(&g_use_lock, lock); atomic_store(&g_lk, 0); atomic_store(&g_done, 0); atomic_store(&g_overlap, 0); g_count = 0;
    for (int i = 0; i < NTH; i++) g_tags[i] = 0;
    for (int i = 0; i < NTH; i++) { uint32_t h; spawn(locker, (uint64_t)i, &h); g_tags[i] = h; }
    // 本线程（W，游戏线程身份）在宿主侧等工作线程：先放开虚拟核（S2A_CORES=1 时同核工作线程要用；真游戏只经系统调用阻塞，不会这样持核等）
    void s2a_core_leave(void); s2a_core_leave();
    while (atomic_load(&g_done) < NTH) usleep(1000);
}

// ---- ⑦ 虚拟核 + 游戏代码里被暂停：暂停方代放核，同核别的线程照常拿到核（S2A_CORES=1 才有核锁；关时同样跑通）----
static _Atomic int g_spin_ready, g_spin_stop, g_t_done;
static __attribute__((noinline)) void guest_spin(uint64_t arg) {
    (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);   // 一次系统调用：返回游戏前取到本核（0 号）
    atomic_store(&g_spin_ready, 1);
    while (!atomic_load(&g_spin_stop)) __asm__ volatile("" ::: "memory");   // 纯游戏代码空转，不进系统调用
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static uint32_t g_ctx_h; static int g_ctx_unpaused;
static void ctx_unpause_hook(void) { uint64_t ur[31] = {g_ctx_h, 0}; svc_thread(0x32, ur); if (ur[0] == 0) g_ctx_unpaused++; }
static void core_taker(uint64_t arg) {
    (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);   // 要取 0 号核
    atomic_store(&g_t_done, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
// ---- P1-49（审计 11-01 18:00）苹果暂停的停稳条件：只有目标真停得住才回 0 ----
//   ① C# 系统调用里被暂停：回 0 后放它出 C#，它必须停在出口（自己停进 pause_park），不得回到游戏代码；恢复后才往下走。
//   ② svc_exit 取完核、清 in_svc 之前（原先最后一次认暂停已经过了）被暂停：同样必须停在出口。
//   ③ Mach 挂住目标、读到停在游戏代码、发布之前，另一条线程把暂停恢复了：暂停方必须自己放开目标、回 0xFA01（不再停住），目标不得永久挂着。
//   ①② 用钩子 / 标志会合定时刻，不按墙钟猜；「停住」按目标自己发 ack（s2a_test_parked）判，「没停住」按它越过出口置的标志判，两者必居其一
#if defined(__APPLE__)
int s2a_test_parked(uint32_t h); void s2a_svc_ctx_fill(s2a_svc_ctx *c, int num, uint64_t *regs); void s2a_svc_ctx_done(void);
static _Atomic int g_pz_in, g_pz_go, g_pz_after; static _Atomic uint32_t g_pz_h;
static void pz_cs_target(uint64_t arg) {   // ①：进 C# 系统调用（0x21 会话请求的出入口），在「C# 里」等放行，出口之后置 after
    (void)arg; uint64_t r[31] = {0}; svc_thread(0x25, r);
    s2a_svc_ctx c; uint64_t regs[31] = {0}; s2a_svc_ctx_fill(&c, 0x21, regs);
    atomic_store(&g_pz_in, 1); while (!atomic_load(&g_pz_go)) usleep(100);
    s2a_svc_ctx_done(); atomic_store(&g_pz_after, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void pz_leave_hook(void *t) {   // ②：只对目标线程、只一次：在 svc_exit 清 in_svc 之前停下，等主线程发完暂停
    (void)t; extern void (*s2a_test_svc_leave)(void *);
    if (s2a_thread_current() != atomic_load(&g_pz_h) || atomic_load(&g_pz_in)) return;
    atomic_store(&g_pz_in, 1); while (!atomic_load(&g_pz_go)) usleep(100);
}
static void pz_svc_target(uint64_t arg) {
    (void)arg; while (!atomic_load(&g_pz_h)) usleep(100);
    uint64_t r[31] = {0}; svc_thread(0x25, r); atomic_store(&g_pz_after, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static _Atomic int g_pm_ready, g_pm_stop; static _Atomic uint64_t g_pm_n; static mach_port_t g_pm_port; static uint32_t g_pm_h; static uint64_t g_pm_unpause_rc = 0xdead;
static __attribute__((noinline)) void pm_spin(uint64_t arg) {
    (void)arg; uint64_t r[31] = {0}; svc_thread(0x25, r);
    g_pm_port = mach_thread_self(); atomic_store(&g_pm_ready, 1);
    while (!atomic_load(&g_pm_stop)) { atomic_fetch_add(&g_pm_n, 1); __asm__ volatile("" ::: "memory"); }
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void pm_hook(void *t) {   // ③：暂停方已 Mach 挂住目标、还没发布——此刻另一个调用方恢复它
    (void)t; extern void (*s2a_test_pause_suspended)(void *); s2a_test_pause_suspended = NULL;
    uint64_t ur[31] = {g_pm_h, 0}; svc_thread(0x32, ur); g_pm_unpause_rc = ur[0];
}
static int pm_suspend_count(void) { struct thread_basic_info bi; mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    return thread_info(g_pm_port, THREAD_BASIC_INFO, (thread_info_t)&bi, &n) == KERN_SUCCESS ? bi.suspend_count : -1; }
#endif
// ---- ㉑ 直连阻塞放核（s2a_core_block_begin / end，直连层等 GPU / 上屏处调）：A 持 0 号核，在宿主图形层里阻塞等 B；B 同核，要取到 0 号核才能给 A 发信号。
//      阻塞时放核 → B 拿到核、A 等到；旧代码（阻塞不放核，10-24 真机直连 152 秒才到 MARK 的根因）B 一直等核、A 等满 3 秒超时（反证）----
static _Atomic int g_bk_in, g_bk_go, g_bk_done;
static pthread_mutex_t g_bk_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_bk_cv = PTHREAD_COND_INITIALIZER;
static void bk_waiter(uint64_t arg) { (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);   // 一次系统调用：返回游戏前取到本核（0 号）
    void s2a_core_block_begin(void), s2a_core_block_end(void);
    s2a_core_block_begin(); s2a_core_block_begin();   // 嵌套：只有最外层放 / 取
    pthread_mutex_lock(&g_bk_mu); atomic_store(&g_bk_in, 1);
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += 3;
    while (!atomic_load(&g_bk_go)) if (pthread_cond_timedwait(&g_bk_cv, &g_bk_mu, &dl)) break;
    pthread_mutex_unlock(&g_bk_mu);
    s2a_core_block_end(); s2a_core_block_end();
    if (atomic_load(&g_bk_go)) atomic_store(&g_bk_done, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void bk_signaler(uint64_t arg) { (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);   // 要取 0 号核
    pthread_mutex_lock(&g_bk_mu); atomic_store(&g_bk_go, 1); pthread_cond_broadcast(&g_bk_cv); pthread_mutex_unlock(&g_bk_mu);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
// ---- ㉗ 宿主区持核被时钟抢占（10-30 实现缺口：preempt_req 置了没人消费、preempting 又归零让看门狗每 0.5 ms 空转发一次信号，
//      宿主区线程持核时抢占 100% 无效）。A（prio 48、掩码 0x2）持 1 号核停在宿主代码里，B（prio 44、掩码 0x2）同核等核。
//      路径二（本段）：A 在「宿主区干一小段活 ↔ 回一次系统调用」之间往复，让核点在 svc_exit（消费 preempt_req）；
//      关掉消费（s2a_test_preempt_req_consume(0)）→ B 必须超时，证明判据抓得住「没修」。----
int s2a_test_held_core(uint32_t h);
static _Atomic int g_hp_ready, g_hp_stop, g_hp_done, g_hp_acore = -9, g_hp_wcore = -9;
static __attribute__((noinline)) void hp_host_hold(uint64_t arg) {   // 路径一：svc 处理中取核 1 后一直不返回（preempt_req 没人消费）
    (void)arg;
    void s2a_test_host_hold_svc(void); s2a_test_host_hold_svc();   // in_svc=1 + 取核 1：卡在系统调用处理里、回不到 svc_exit
    atomic_store(&g_hp_acore, s2a_test_held_core(S2A_H_CUR_THREAD));
    atomic_store(&g_hp_ready, 1);
    while (!atomic_load(&g_hp_stop)) __asm__ volatile("" ::: "memory");   // 持核、宿主代码自旋（module_of 不认 → 抢占走宿主分支）
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static __attribute__((noinline)) void hp_host_loop(uint64_t arg) {   // 路径二：宿主区与 svc 往复，每轮一次系统调用（svc_exit 是 preempt_req 的消费点）
    (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);                       // 进门取核 1（掩码 0x2）
    atomic_store(&g_hp_acore, s2a_test_held_core(S2A_H_CUR_THREAD));
    atomic_store(&g_hp_ready, 1);
    while (!atomic_load(&g_hp_stop)) {
        for (volatile int i = 0; i < 400; i++) { }                    // 宿主区里干一小段活（PC 不在游戏代码段 → 抢占走宿主分支）
        uint64_t q[31] = {0}; svc_thread(0x25, q);
    }
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void hp_waiter(uint64_t arg) {                                 // B：同核等核（core_acquire 排队）
    (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);
    atomic_store(&g_hp_wcore, s2a_test_held_core(S2A_H_CUR_THREAD));
    atomic_store(&g_hp_done, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
// ---- ⑮ 调度计数（s2a_sched_stat_log 的数据源）：干净时「同核在跑 ≥2」「不持核在跑」「同核同优先级同时在跑」都 0；
//      正对照：把 A 的核直接收走（s2a_test_steal_core，A 照常在游戏代码里跑）→ 同核 B 上核 → 三项都 >0 ----
static _Atomic int g_ss_ready[2], g_ss_stop;
static __attribute__((noinline)) void ss_spin(uint64_t arg) {
    uint64_t r[31] = {0}; svc_thread(0x25, r);   // 一次系统调用：返回游戏前取核 0
    atomic_store(&g_ss_ready[arg], 1);
    while (!atomic_load(&g_ss_stop)) __asm__ volatile("" ::: "memory");
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
// ---- ㉑ 宿主侧持核长跑：宿主代码里持核自旋（module_of 认不出这个地址 → 采样按「宿主侧」记）。
//      这是「持核线程在宿主 C 代码里等 GPU / 等锁」的最小模型（botw nx-120 的 h_nvnTextureFinalize → mtl_vc_drop 就是它）----
static _Atomic int g_hc_ready, g_hc_stop, g_hc_left;
// 按采样器真采了几次来等，不按毫秒数赌（11-01 合入线：负载高时看门狗醒得晚，120 ms 窗口里 0 次采样、这段的结算拖到负对照窗口里落账，㉑ 两条一起红）。
//   至少 min_ns 墙钟且采样计数涨够 k 次才返回；上限 10 秒（再久说明采样器停了，交给判据报红）
void s2a_test_sched_stat(uint64_t *, int *, uint64_t *, uint64_t *, uint64_t *);
static uint64_t sched_samples(void) { uint64_t n, m, c, p; int pk; s2a_test_sched_stat(&n, &pk, &m, &c, &p); return n; }
static void wait_samples(uint64_t k, uint64_t min_ns) {
    uint64_t from = sched_samples(), t0 = now_ns();
    while ((sched_samples() - from < k || now_ns() - t0 < min_ns) && now_ns() - t0 < 10000000000ull) usleep(1000);
}
static __attribute__((noinline)) void hc_host_spin(uint64_t arg) {
    (void)arg;
    void s2a_core_enter(void);
    s2a_core_enter();          // 以持核状态进宿主代码（同直连图形层的宿主调用：不经 svc、不放核）
    atomic_store(&g_hc_ready, 1);
    while (!atomic_load(&g_hc_stop)) __asm__ volatile("" ::: "memory");
    s2a_core_leave();
    atomic_store(&g_hc_left, 1);   // 已放核：之后的采样必然看到它不在宿主侧持核，这段才会结算
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
// ---- ⑰ 游戏代码里暂停 → 恢复：恢复后先按优先级取核再跑（同优先级 B 持核自旋时 A 等核，不持核在跑 0）；旧代码恢复后不持核在跑 ----
static _Atomic int g_ua_ready[2], g_ua_stop[2];
static __attribute__((noinline)) void ua_spin(uint64_t arg) {
    uint64_t r[31] = {0}; svc_thread(0x25, r);
    atomic_store(&g_ua_ready[arg], 1);
    while (!atomic_load(&g_ua_stop[arg])) __asm__ volatile("" ::: "memory");
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
// ---- ⑱ 同一条线程被时钟抢占两次：L 纯自旋持核 0；H1 抢占 → H1 退出 → L 回核接着转；H2 再抢占也要成功。
//      tramp 里屏蔽抢占信号再 sigsuspend，二次信号返回时内核恢复的若是 sigsuspend 前的掩码，L 就永远屏蔽着抢占信号、第二次抢不动 ----
static _Atomic int g_rp_ready, g_rp_stop, g_rp_hdone[2];
static __attribute__((noinline)) void rp_spin(uint64_t arg) { (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);
    atomic_store(&g_rp_ready, 1);
    while (!atomic_load(&g_rp_stop)) __asm__ volatile("" ::: "memory");
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
// ---- ⑳ 空闲核迁移不看优先级（10-23 巫师3 过场活锁：Mac 与测试设备2 现场同构）：H 优先级 32、掩码 0x7，持 1 号核纯自旋；
//      W 优先级 44、掩码只含 1 号核，等核；0 / 2 号核空。Horizon：空闲核把「核上还有人等着」的持核者拉过去（被迁者照跑，只换核），
//      所以期望 W 2 秒内拿到 1 号核、H 被迁到 0 或 2 号核。旧规则「不迁走比自己优先级高的持核者」连迁到空闲核也拦 → W 永远等（反证）。
//      两个只能用 1 号核的等待者 W1 / W2（维护者 10-24 判据）：H 被迁走后 W1 先上核、退出放核，W2 接着上核，两个都要在 2 秒内拿到 1 号核。----
int s2a_test_held_core(uint32_t h);
static _Atomic int g_mg_hready, g_mg_wcore[2] = {-9, -9}, g_mg_wdone, g_mg_stop, g_mg_hcore_after = -9;
static __attribute__((noinline)) void mg_spin(uint64_t arg) { (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);                 // 进门取核（理想核 1）
    atomic_store(&g_mg_hready, 1);
    while (!atomic_load(&g_mg_stop)) __asm__ volatile("" ::: "memory");   // 纯客体自旋，不进 svc
    atomic_store(&g_mg_hcore_after, s2a_test_held_core(S2A_H_CUR_THREAD));
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void mg_lo(uint64_t arg) {   // arg = 等待者编号 0 / 1
    uint64_t r[31] = {0}; svc_thread(0x25, r);                 // 只能用 1 号核：靠 H 被迁走才拿得到
    atomic_store(&g_mg_wcore[arg & 1], s2a_test_held_core(S2A_H_CUR_THREAD)); atomic_fetch_add(&g_mg_wdone, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static uint32_t spawn_core_mask(void (*f)(uint64_t), uint64_t arg, int prio, int core, uint64_t mask) {   // 在 core 上建、启动前把掩码改成 mask
    uint64_t r[31] = {0, (uint64_t)f, arg, 0, (uint64_t)prio, (uint64_t)core}; svc_thread(0x08, r);
    uint64_t m[31] = {r[1], (uint64_t)(int64_t)-3, mask}; svc_thread(0x0F, m);   // -3 = 不改理想核
    uint64_t s[31] = {r[1]}; svc_thread(0x09, s);
    return (uint32_t)r[1];
}
// ---- ⑲ 抢占不许碰被抢线程 sp 下方：改写器补丁区桩用 stp x0, x2, [sp, #-0x10] … ldp x0, x2, [sp, #-0x10] 暂存寄存器（不挪 sp）。
//      旧代码：SIGURG 信号帧压在游戏栈上、tramp 的 C 栈帧也从原 sp 往下压 → 盖掉这 16 字节（10-23 Mac 塞尔达 x2 被读成 lr、SIGBUS）。
//      L 在汇编里往 [sp-16] 写标记、空转、读回比较；H 反复同核抢占 L。期望标记一次都没坏 ----
static _Atomic int g_rz_ready, g_rz_stop, g_rz_hdone; static _Atomic uint64_t g_rz_bad, g_rz_loops;
static __attribute__((noinline)) void rz_spin(uint64_t arg) { (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);
    atomic_store(&g_rz_ready, 1);
    uint64_t bad = 0, n = 0;
    while (!atomic_load(&g_rz_stop)) {
        uint64_t v1, v2;
        __asm__ volatile("movz x9, #0x5a5a\n movk x9, #0xa5a5, lsl #16\n movk x9, #0x1234, lsl #32\n stp x9, x9, [sp, #-16]\n"
                         "mov x10, #4000\n 1: subs x10, x10, #1\n b.ne 1b\n ldp %0, %1, [sp, #-16]" : "=r"(v1), "=r"(v2) :: "x9", "x10", "memory");
        if (v1 != 0x1234a5a55a5aull || v2 != 0x1234a5a55a5aull) bad++;
        n++;
    }
    atomic_store(&g_rz_bad, bad); atomic_store(&g_rz_loops, n);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
// ---- ⑲b 抢占不碰被抢线程 sp 下方的大块活数据（#515：游戏开变长数组后 mov sp, x29 收回 sp，之后照读写 sp 以下的数组；旧 tramp 跑在游戏栈上、
//      只从原 sp 让 256 字节，数组超出的部分被 tramp 的栈帧盖掉 → 野指针段错误）。L 在汇编里把 [sp-4096, sp) 512 个 8 字节全写成标记、空转、逐个读回比，
//      H 反复同核抢占 L。期望 = 标记一个都没坏（标记是写进去的常数，与被测实现无关） ----
static _Atomic int g_bz_ready, g_bz_stop, g_bz_hdone; static _Atomic uint64_t g_bz_bad, g_bz_loops;
static __attribute__((noinline)) void bz_spin(uint64_t arg) { (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);
    atomic_store(&g_bz_ready, 1);
    uint64_t bad = 0, n = 0;
    while (!atomic_load(&g_bz_stop)) {
        uint64_t miss;
        __asm__ volatile("movz x9, #0x5a5a\n movk x9, #0xa5a5, lsl #16\n movk x9, #0x4321, lsl #32\n"
                         "sub x11, sp, #4096\n mov x10, #512\n 1: str x9, [x11], #8\n subs x10, x10, #1\n b.ne 1b\n"
                         "mov x10, #4000\n 2: subs x10, x10, #1\n b.ne 2b\n"
                         "sub x11, sp, #4096\n mov x10, #512\n mov %0, #0\n 3: ldr x12, [x11], #8\n cmp x12, x9\n cinc %0, %0, ne\n subs x10, x10, #1\n b.ne 3b"
                         : "=&r"(miss) :: "x9", "x10", "x11", "x12", "cc", "memory");
        bad += miss; n++;
    }
    atomic_store(&g_bz_bad, bad); atomic_store(&g_bz_loops, n);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void bz_hi(uint64_t arg) { (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);   // 高优先级等核 0：靠抢占 L
    atomic_fetch_add(&g_bz_hdone, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void rz_hi(uint64_t arg) { (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);   // 高优先级等核 0：靠抢占 L
    atomic_fetch_add(&g_rz_hdone, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void rp_hi(uint64_t arg) {
    uint64_t r[31] = {0}; svc_thread(0x25, r);   // 高优先级等核 0：靠抢占 L
    atomic_store(&g_rp_hdone[arg], 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void spawn_prio_arg(void (*f)(uint64_t), uint64_t arg, int prio) {
    uint64_t r[31] = {0, (uint64_t)f, arg, 0, (uint64_t)prio, 0}; svc_thread(0x08, r);
    uint64_t s[31] = {r[1]}; svc_thread(0x09, s);
}
// ---- ⑯ 新线程进游戏代码前已持核：入口第一条读持核号（不先走系统调用）应为 0 号核；旧代码到第一次系统调用才取核 → -1 ----
static _Atomic int g_sp_core = -9, g_sp_done;
int s2a_test_held_core(uint32_t h);
static void start_probe(uint64_t arg) { (void)arg;
    atomic_store(&g_sp_core, s2a_test_held_core(S2A_H_CUR_THREAD)); atomic_store(&g_sp_done, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
// ---- ⑧ 掩码内换核：理想核取进程缺省（-2，掩码 0~2）的线程 A 拿到核后在游戏代码里空转；同掩码的线程 B 须拿到另一个核（Horizon 把就绪线程放到掩码内空闲核）。
//      10-20 九款回归：原「只认一个核」时全挤 0 号核，暗黑2 主线程空转等工作线程、工作线程排同一核 → 卡死 ----
static _Atomic int g_m_ready, g_m_stop, g_m_done;
static void mask_spin(uint64_t arg) {
    (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);
    atomic_store(&g_m_ready, 1);
    while (!atomic_load(&g_m_stop)) __asm__ volatile("" ::: "memory");
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void mask_taker(uint64_t arg) {
    (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);
    atomic_store(&g_m_done, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
// ---- ⑨ 空闲核迁移（同 Horizon KScheduler：空闲核把别核正在跑、掩码也含本核的线程拿过来，让那个核上等着的线程跑）：
//      A（掩码 0~2）拿到某核 x 后在游戏代码里空转；B（掩码只含 x）要 x——A 须被挪到掩码里空着的别的核、B 拿到 x。
//      10-20 八方2 开虚拟核：渲染线程（掩码只含 2 号核）等核，2 号核被掩码 0~2 的异步加载线程空转占着、0 / 1 号核空着 → 互等，卡死检测自杀。旧代码 B 永远拿不到核 ----
static _Atomic int g_g_ready, g_g_stop, g_g_done, g_g_acore = -9, g_g_bcore = -9;
int s2a_test_held_core(uint32_t h);
static void mig_spin(uint64_t arg) {
    (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);
    atomic_store(&g_g_acore, s2a_test_held_core(S2A_H_CUR_THREAD)); atomic_store(&g_g_ready, 1);
    while (!atomic_load(&g_g_stop)) __asm__ volatile("" ::: "memory");
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void mig_taker(uint64_t arg) {
    (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);
    atomic_store(&g_g_bcore, s2a_test_held_core(S2A_H_CUR_THREAD)); atomic_store(&g_g_done, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void spawn_core(void (*f)(uint64_t), int core) {
    uint64_t r[31] = {0, (uint64_t)f, 0, 0, 44, (uint64_t)(int64_t)core}; svc_thread(0x08, r);
    uint64_t s_[31] = {r[1]}; svc_thread(0x09, s_);
}
// ---- 锁持有者句柄无效（期望照 Ryujinx KAddressArbiter：ArbitrateLock 回 InvalidHandle 0xE401；条件变量被唤醒去拿锁、锁字持有者无效 → 以 0xE401 醒来）
//      旧代码两处都挂上去永远等不到交锁、无报红 → 反证不过 ----
static _Atomic uint32_t g_bl_lock, g_bl_key; static uint32_t g_bl_h; static _Atomic int g_bl_in, g_bl_done; static _Atomic uint64_t g_bl_rc = 1;
static void bl_cv(uint64_t arg) { (void)arg;
    atomic_store(&g_bl_lock, g_bl_h); atomic_store(&g_bl_in, 1);
    uint64_t r[31] = {(uint64_t)&g_bl_lock, (uint64_t)&g_bl_key, g_bl_h, (uint64_t)-1}; svc_thread(0x1C, r);
    atomic_store(&g_bl_rc, r[0]); atomic_store(&g_bl_done, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e); }
// ---- ArbitrateLock 快路径（期望照 Horizon：锁字已不是「持有者 | 等待位」→ 回 0，游戏回用户态重抢）：主线程持着全局锁 G，
//   另一条游戏线程拿对不上的锁字调 0x1A，应 1 秒内回 0（不碰 G）。反证：去掉快路径，它卡在 glock() 上，1 秒内回不来
void s2a_test_glock_hold(int on);
static _Atomic uint32_t g_fp_lock; static _Atomic int g_fp_go, g_fp_done, g_fp_ready; static _Atomic uint64_t g_fp_rc = 1; static uint32_t g_fp_h;
static void fp_arb(uint64_t arg) { (void)arg;
    atomic_store(&g_fp_ready, 1);   // 线程起来（启动登记要用 G）之后主线程才拿 G
    while (!atomic_load(&g_fp_go)) usleep(100);
    uint64_t r[31] = {0x1234u, (uint64_t)&g_fp_lock, g_fp_h}; svc_thread(0x1A, r);   // 锁字 0（没人持有），对不上 0x1234 | 等待位
    atomic_store(&g_fp_rc, r[0]); atomic_store(&g_fp_done, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e); }
// ---- Horizon 时间片轮转：优先级 59 的两条线程都只能用 0 号核，A 空转，B 等核 ----
static _Atomic int g_hz_ready, g_hz_done, g_hz_stop;
__attribute__((noinline)) static void hz_spin(uint64_t arg) { (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r); atomic_store(&g_hz_ready, 1);
    while (!atomic_load(&g_hz_stop)) __asm__ volatile("" ::: "memory");
    uint64_t e[31] = {0}; svc_thread(0x0A, e); }
static void hz_taker(uint64_t arg) { (void)arg; uint64_t r[31] = {0}; svc_thread(0x25, r); atomic_store(&g_hz_done, 1); uint64_t e[31] = {0}; svc_thread(0x0A, e); }
// 等标志：调用处的 ms 只当「等不到判红」的兜底。11-19：原先 2～3 秒的上限在负载高时（线程起来 / 拿核晚）先到，假红；
//   等「会发生」的事一律给 WAIT_LONG（30 秒）——正常时一置位就返回、不多花时间；等「不该发生」的负对照照旧用短窗（只会假绿不会假红）
#define WAIT_LONG 30000
static int wait_flag(_Atomic int *f, int ms) { for (int i = 0; i < ms && !atomic_load(f); i++) usleep(1000); return atomic_load(f); }
// ---- 冻结代放核（即时存档 s2a_snap_freeze，虚拟核开）：S 持核在游戏代码里忙等（不进门，冻结 0.5 秒后被 Mach 挂住当安全点），
//   T 与 S 同核、进系统调用后卡在出口取核（in_svc 还是 1、不算安全点）。期望按语义：冻结方代放 S 的核 → T 拿到核、停进出口 → 冻结在时限内判齐；
//   解冻后 S 先取核再回游戏代码、接着计数；T 最终拿到核走完。修前：S 被挂住仍持核，T 永远到不了安全点，冻结超时（回 -1）
#ifdef T_FZ
int s2a_snap_freeze(int timeout_ms, int *nthr, int *nst); void s2a_snap_thaw(void);
static _Atomic int g_fz_ready, g_fz_stop, g_fz_tdone, g_fz_rc = 99; static _Atomic uint64_t g_fz_n;
// 冻结判安全点要看门帧 +816 的切栈槽（产品门帧 832 字节，单测别处的 31 字寄存器数组读到那里是越界垃圾）：本用例进门一律用整帧、切栈槽置非零
#define FZ_FRAME(v) uint64_t v[104] = {0}; v[102] = 1
static __attribute__((noinline)) void fz_spin(uint64_t arg) {
    (void)arg; FZ_FRAME(r); svc_thread(0x25, r);   // 进一次门：出口取到 0 号核
    atomic_store(&g_fz_ready, 1);
    while (!atomic_load(&g_fz_stop)) atomic_fetch_add(&g_fz_n, 1);   // 持核、纯游戏代码忙等
    FZ_FRAME(e); svc_thread(0x0A, e);
}
static void fz_taker(uint64_t arg) { (void)arg; FZ_FRAME(r); svc_thread(0x25, r); atomic_store(&g_fz_tdone, 1); FZ_FRAME(e); svc_thread(0x0A, e); }
static void *fz_freezer(void *a) {   // 宿主线程发起冻结（同产品：存档线程不是游戏线程）
    (void)a; int n = 0, nst[8] = {0}; int rc = s2a_snap_freeze(30000, &n, nst); atomic_store(&g_fz_rc, rc); if (rc >= 0) s2a_snap_thaw(); return NULL; }   // 冻结时限只当兜底（原 3 秒：负载高时同核线程到安全点慢 → 假红）
#endif
// ---- ⑩ yield 同优先级轮转：A/B 同优先级、同一单核（掩码 0x1）；A 持核空转，B 等核；A SleepThread(0) 让核 → 同优先级的 B 拿到核（Switch yield 轮转）。
//      旧代码（yield 后立即重取、同优先级不让）B 拿不到 → 反证不过 ----
static _Atomic int g_y_aready, g_y_bgo, g_y_bdone, g_y_bheld = -9, g_y_stop;
static void yld_a(uint64_t arg) { (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);                 // 取唯一核
    atomic_store(&g_y_aready, 1);
    while (!atomic_load(&g_y_bgo)) usleep(500); usleep(10000); // 等 B 登记到等核队列
    uint64_t s[31] = {0}; svc_thread(0x0B, s);                 // SleepThread(0) 让核：新版在此阻塞重取、B 先拿；旧版立即拿回
    while (!atomic_load(&g_y_stop)) __asm__ volatile("" ::: "memory");
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void yld_b(uint64_t arg) { (void)arg;
    atomic_store(&g_y_bgo, 1);
    uint64_t r[31] = {0}; svc_thread(0x25, r);                 // 等 A 让核
    atomic_store(&g_y_bheld, s2a_test_held_core(S2A_H_CUR_THREAD)); atomic_store(&g_y_bdone, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void spawn_mask(void (*f)(uint64_t), unsigned mask) {   // 建线程 + 设核掩码（单核）+ 启动
    uint64_t r[31] = {0, (uint64_t)f, 0, 0, 44, 0}; svc_thread(0x08, r);
    uint64_t m[31] = {r[1], 0, mask}; svc_thread(0x0F, m);
    uint64_t s[31] = {r[1]}; svc_thread(0x09, s);
}
// ---- ⑫ 纯自旋时钟抢占：L=preempt_spin 低优先级持核后纯客体自旋（module_of 桩认此函数→抢占走客体区 tramp）；
//      H=preempt_hi 高优先级同核等核。看门狗抢占 L → H 上核。无抢占时 L 纯自旋永持核、H 饿死（反证不过）。----
static _Atomic int g_pe_lready, g_pe_lcore = -9, g_pe_hdone, g_pe_hcore = -9, g_pe_stop;
static _Atomic uint64_t g_pe_ht1;   // profile：H 上核时刻（量抢占延迟分布用）
static __attribute__((noinline)) void preempt_spin(uint64_t arg) { (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);                 // 取核 0
    atomic_store(&g_pe_lcore, s2a_test_held_core(S2A_H_CUR_THREAD));
    atomic_store(&g_pe_lready, 1);
    while (!atomic_load(&g_pe_stop)) __asm__ volatile("" ::: "memory");   // 纯客体自旋、不进 svc（唯一让核途径＝被时钟抢占）
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void preempt_hi(uint64_t arg) { (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);                 // 高优先级等核 0（靠抢占 L 让出才拿得到）
    atomic_store(&g_pe_ht1, now_ns());                         // profile：H 上核时刻
    atomic_store(&g_pe_hcore, s2a_test_held_core(S2A_H_CUR_THREAD)); atomic_store(&g_pe_hdone, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void spawn_prio(void (*f)(uint64_t), int prio, uint32_t *h) {   // CreateThread 直接设优先级 + 核 0（regs[4]=prio regs[5]=0）+ 启动
    uint64_t r[31] = {0, (uint64_t)f, 0, 0, (uint64_t)prio, 0}; svc_thread(0x08, r); if (h) *h = (uint32_t)r[1];
    uint64_t s[31] = {r[1]}; svc_thread(0x09, s);
}
// 抢占延迟分布测量（S2A_PREEMPT_PROFILE=N）：确定性重复 N 次「L 纯自旋持核 0 → spawn H 高优先级同核 → 看门狗抢占 → H 上核」，量 spawn H 到 H 上核的延迟（＝⑫ 判据口径）。
static int lat_cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y); }
static void preempt_profile(void) {
    void s2a_core_leave(void);
    void s2a_test_preempt_diag(uint64_t *, uint64_t *, uint64_t *, uint64_t *, int *);
    int N = atoi(getenv("S2A_PREEMPT_PROFILE")); if (N <= 0) N = 300;
    uint64_t *lat = calloc(N, sizeof *lat); int got = 0, fail = 0;
    s2a_core_leave();   // W 让出核，L / H 用核 0
    for (int it = 0; it < N; it++) {
        atomic_store(&g_pe_lready, 0); atomic_store(&g_pe_lcore, -9);
        atomic_store(&g_pe_hdone, 0); atomic_store(&g_pe_hcore, -9);
        atomic_store(&g_pe_ht1, 0); atomic_store(&g_pe_stop, 0);
        uint64_t s0 = 0, r0 = 0; s2a_test_preempt_diag(0, &s0, &r0, 0, 0);   // 本轮起点 SIGURG 发 / 达（算本轮增量）
        uint32_t lh = 0, hh = 0;
        spawn_prio(preempt_spin, 48, &lh); s2a_core_leave();   // W 让核（和 ⑫ 用例一致；否则 svc 入口 W 占核 0、L/H 上不来）
        if (!wait_flag(&g_pe_lready, WAIT_LONG)) {
            fail++; printf("  ⑫诊断[it=%d]：L 没上核（lready 超时）、L 持核 %d\n", it, s2a_test_held_core(lh));
            atomic_store(&g_pe_stop, 1); usleep(3000);
            { uint64_t c[31] = {lh}; svc_thread(0x16, c); }   // 关句柄回收线程对象（否则累积到 EVMAP 4096 限）
            continue;
        }
        uint64_t t0 = now_ns();
        spawn_prio(preempt_hi, 40, &hh); s2a_core_leave();
        if (!wait_flag(&g_pe_hdone, WAIT_LONG)) {
            fail++;
            uint64_t wd = 0, s1 = 0, r1 = 0, lpc = 0; int lg = 0; s2a_test_preempt_diag(&wd, &s1, &r1, &lpc, &lg);
            printf("  ⑫诊断[it=%d]：本轮 SIGURG 发 %llu 达 %llu（本轮抢占%s发生）、watchdog 最后扫距今 %.1f ms、L 持核 %d、H 持核 %d、最后目标 PC %#llx 客体区 %d\n",
                it, (unsigned long long)(s1 - s0), (unsigned long long)(r1 - r0), (s1 > s0) ? "" : "没", wd ? (double)(now_ns() - wd) / 1e6 : -1.0,
                s2a_test_held_core(lh), s2a_test_held_core(hh), (unsigned long long)lpc, lg);
            atomic_store(&g_pe_stop, 1); usleep(3000);
            { uint64_t c[31] = {lh}; svc_thread(0x16, c); c[0] = hh; svc_thread(0x16, c); }
            continue;
        }
        lat[got++] = atomic_load(&g_pe_ht1) - t0;
        atomic_store(&g_pe_stop, 1);
        usleep(2000);   // 等 L/H 退出让核
        { uint64_t c[31] = {lh}; svc_thread(0x16, c); c[0] = hh; svc_thread(0x16, c); }   // 关句柄回收线程对象，不累积
    }
    qsort(lat, got, sizeof *lat, lat_cmp);
    uint64_t sum = 0; for (int i = 0; i < got; i++) sum += lat[i];
    printf("抢占延迟分布（成功 %d / 失败 %d，单位 ms）：min %.3f p50 %.3f p90 %.3f p99 %.3f max %.3f mean %.3f\n",
        got, fail, got ? lat[0] / 1e6 : 0.0, got ? lat[got / 2] / 1e6 : 0.0,
        got ? lat[(int)(got * 0.90)] / 1e6 : 0.0, got ? lat[(int)(got * 0.99)] / 1e6 : 0.0,
        got ? lat[got - 1] / 1e6 : 0.0, got ? (double)sum / got / 1e6 : 0.0);
    free(lat);
}
// ---- ⑬ 宿主持锁被抢占信号打断不死锁（硬约束：on_preempt 宿主区只置原子标志、不碰任何宿主锁，持锁期间收信号也安全）----
static _Atomic int g_hl_done, g_hl_stop; static pthread_mutex_t g_hl_m = PTHREAD_MUTEX_INITIALIZER;
static void hostlock_worker(uint64_t arg) { (void)arg;
    uint64_t r[31] = {0}; svc_thread(0x25, r);                 // 取核（PC 在宿主区代码）
    for (int i = 0; i < 20000 && !atomic_load(&g_hl_stop); i++) {
        pthread_mutex_lock(&g_hl_m);
        pthread_kill(pthread_self(), SIGURG);                  // 持宿主锁时收抢占信号：on_preempt 宿主区只置标志、不碰锁 → 不死锁
        pthread_mutex_unlock(&g_hl_m);
    }
    atomic_store(&g_hl_done, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}

// ---- ㉓㉔㉕ 轻量调度（10-24，docs/研究/虚拟核与轻量调度语义.md）：全部参与（缺省）/ 按需 / S2A_CORES=0 各在一个子进程里跑（cores_mode 进程内只读一次）----
// ㉓ 同核高优先级唤醒让先：B（优先级 30、掩码只含核 0）等事件；W（主线程 44、理想核 0）发信号后立刻记序号。Horizon：B 抢占 W 跑到再阻塞 → B 的序号先于 W。
//    关（S2A_CORES=0）时不让：W 不等 B，W 先记（反证）。期望按「谁先拿到序号」手算，与时序无关的那一方必须一致
// ㉔ 读过核号的线程按核互斥：P1 / P2 同优先级 44、掩码只含核 0，各调 0x10 取核号 c，对 g_lc[c] 做非原子读-改-写 200 万次、让步，5 轮。
//    期望（按需）：总数 = 2 × 5 × 200 万 = 20,000,000（同核互斥，不丢更新），同核诊断「丙」= 0；关时两线程宿主真并行，丢更新、丙 > 0（反证）
// ㉕ 没读过核号的线程不取核：按需模式下 Q（掩码 0x1、从不调 0x10）在游戏代码里自旋时持核号 = -1（不参与核互斥）；全部 / 关模式不判
//    10-25 根修（压满核偶发红）：㉓ 原先 usleep 20 ms 假定 B 已进 0x18、唤醒让先的 5 ms 安全阀按墙钟计——改为查 B 真在等待队列里、子进程钉住单调钟（安全阀不触发，
//    序号只由语义定）；㉔㉖ 的「不互斥」反证原先要「丢更新 + 同核诊断采样到丙」，两样都看宿主当时有没有真并行、采样碰没碰上——改为会合：
//    两条同核线程各在游戏代码里（系统调用之外）等对方也进来，双方都等到 = 没有互斥（有互斥则必然等不到、按超时判红）
uint64_t s2a_test_samecore_c(void); void s2a_test_peer_yield(uint64_t *n, uint64_t *timeout); int s2a_test_held_core(uint32_t h); int s2a_test_blocked(uint32_t h); void s2a_test_mono_set(int64_t);
static _Atomic int g_lt_seq; static _Atomic int g_lt_b, g_lt_w, g_lt_bready; static uint32_t g_lt_evr;
static void lt_waiter(uint64_t arg) { (void)arg;
    atomic_store(&g_lt_bready, 1);
    uint64_t r[31] = {0, (uint64_t)&g_lt_evr, 1, (uint64_t)-1}; svc_thread(0x18, r);
    atomic_store(&g_lt_b, atomic_fetch_add(&g_lt_seq, 1) + 1);
    uint64_t y[31] = {0}; svc_thread(0x0B, y);   // 再进系统调用：唤醒方的「等它跑到下一次系统调用」在这里满足
    uint64_t e[31] = {0}; svc_thread(0x0A, e); }
// ③b（#1520 D-BJZ6）：等待中途游戏关掉被等的事件读端句柄。Horizon 等的是对象：事件还被写端持着，从写端触发照样唤醒（甲）；不触发就按超时返回（乙），都不回句柄无效
static uint32_t g_cw_h; static _Atomic int g_cw_done; static uint64_t g_cw_r0, g_cw_r1;
static void cw_waiter(uint64_t arg) { uint32_t hs[1] = {g_cw_h}; uint64_t r[31] = {0, (uint64_t)hs, 1, arg}; svc_thread(0x18, r);
    g_cw_r0 = r[0]; g_cw_r1 = r[1]; atomic_store(&g_cw_done, 1); uint64_t e[31] = {0}; svc_thread(0x0A, e); }
static void cw_case(int signal_after, uint64_t timeout_ns, uint64_t want) {
    uint64_t ev[31] = {0}; svc_thread(0x45, ev); uint32_t hw = (uint32_t)ev[1]; g_cw_h = (uint32_t)ev[2]; atomic_store(&g_cw_done, 0);
    uint64_t r[31] = {0, (uint64_t)cw_waiter, timeout_ns, 0, 44, 0}; svc_thread(0x08, r); uint64_t s_[31] = {r[1]}; svc_thread(0x09, s_);
    while (!s2a_test_blocked(r[1])) usleep(100);   // 真进了 0x18
    uint64_t cl[31] = {g_cw_h}; svc_thread(0x16, cl);   // 游戏关掉被等的读端
    if (signal_after) { uint64_t sg[31] = {hw}; svc_thread(0x11, sg); }
    while (!atomic_load(&g_cw_done)) usleep(1000);
    CHECK(g_cw_r0 == want && (want || g_cw_r1 == 0), "③b 等待中关读端%s：期望 %#llx → %#llx（下标 %llu；0xe401 = 句柄无效，旧实现醒来按句柄重查）",
          signal_after ? "、再从写端触发" : "、不触发", (unsigned long long)want, (unsigned long long)g_cw_r0, (unsigned long long)g_cw_r1);
    uint64_t cw[31] = {hw}; svc_thread(0x16, cw); }
static volatile long g_lc[4]; static _Atomic int g_lc_done;
static void lc_worker(uint64_t arg) { (void)arg;
    for (int round = 0; round < 5; round++) {
        uint64_t q[31] = {0}; svc_thread(0x10, q); int c = (int)q[0] & 3;
        for (int i = 0; i < 2000000; i++) { long v = g_lc[c]; __asm__ volatile("" ::: "memory"); g_lc[c] = v + 1; }
        uint64_t y[31] = {0}; svc_thread(0x0B, y);
    }
    atomic_fetch_add(&g_lc_done, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e); }
static volatile long g_pin_cnt; static _Atomic int g_pin_done;
static void pin_worker(uint64_t arg) { (void)arg;   // ㉖：掩码只含核 0、从不调 0x10，直接改「核 0 的数据」
    { uint64_t y[31] = {0}; svc_thread(0x0B, y); }   // 桩里入口算宿主代码、进门不取核；先过一次系统调用出口取核（同真游戏线程进游戏代码前取核）
    for (int round = 0; round < 5; round++) { for (int i = 0; i < 2000000; i++) { long v = g_pin_cnt; __asm__ volatile("" ::: "memory"); g_pin_cnt = v + 1; }
        uint64_t y[31] = {0}; svc_thread(0x0B, y); }
    atomic_fetch_add(&g_pin_done, 1); uint64_t e[31] = {0}; svc_thread(0x0A, e); }
static _Atomic int g_rv_in, g_rv_met, g_rv_done;   // ㉔㉖ 反证会合：进门数、等到对方的线程数、退出数
static void rv_meet(void) {   // 在游戏代码里（系统调用之外）等另一条也进来；30 s 内等不到就放弃（计入 met 不足 → 判红）
    atomic_fetch_add(&g_rv_in, 1); uint64_t t0 = now_ns();
    while (atomic_load(&g_rv_in) < 2 && now_ns() - t0 < 30000000000ull) {}
    if (atomic_load(&g_rv_in) >= 2) atomic_fetch_add(&g_rv_met, 1); }
static void rv_reader(uint64_t arg) { (void)arg; uint64_t q[31] = {0}; svc_thread(0x10, q); rv_meet();   // ㉔：读过核号
    atomic_fetch_add(&g_rv_done, 1); uint64_t e[31] = {0}; svc_thread(0x0A, e); }
static void rv_pinned(uint64_t arg) { (void)arg; uint64_t y[31] = {0}; svc_thread(0x0B, y); rv_meet();   // ㉖：钉核、不读核号；先过一次系统调用出口（同 pin_worker）
    atomic_fetch_add(&g_rv_done, 1); uint64_t e[31] = {0}; svc_thread(0x0A, e); }
static int rv_case(void (*fn)(uint64_t)) {   // 起两条掩码只含核 0、优先级 44 的线程会合，返回等到对方的条数（期望 2）
    atomic_store(&g_rv_in, 0); atomic_store(&g_rv_met, 0); atomic_store(&g_rv_done, 0);
    uint64_t r1[31] = {0, (uint64_t)fn, 0, 0, 44, 0}; svc_thread(0x08, r1); uint64_t r2[31] = {0, (uint64_t)fn, 0, 0, 44, 0}; svc_thread(0x08, r2);
    uint64_t s1[31] = {r1[1]}; svc_thread(0x09, s1); uint64_t s2[31] = {r2[1]}; svc_thread(0x09, s2);
    while (atomic_load(&g_rv_done) < 2) usleep(1000);   // 两条都会在 30 s 内退出（会合成功或放弃），不设墙钟判据
    return atomic_load(&g_rv_met); }
static _Atomic int g_q_ready, g_q_stop;
static void q_free(uint64_t arg) { (void)arg; atomic_store(&g_q_ready, 1); while (!atomic_load(&g_q_stop)) {} uint64_t e[31] = {0}; svc_thread(0x0A, e); }
static int lite_case(int mode) {   // 子进程里跑；mode 1 按需（缺省）/ 0 关；返回失败数
    bad = 0;   // fork 时继承了父进程的计数
    // 0 / 3 测「关」档语义：显式 S2A_CORES=0（11-19 起苹果平台不设时缺省是按需，靠缺省会测成按需）
    if (mode == 2) setenv("S2A_CORES", "1", 1); else if (mode == 1) setenv("S2A_CORES", "ondemand", 1); else if (mode == 4) unsetenv("S2A_CORES"); else setenv("S2A_CORES", "0", 1);   // 4：不设 = 缺省（苹果按需、其它关）
    if (mode == 3) setenv("S2A_PEER_YIELD", "0", 1);   // 3：反证——唤醒让先也关
    setenv("S2A_SAMECORE_DIAG", "1", 1);
    s2a_thread_main_init();
    #if defined(__APPLE__)
    const int dflt_excl = 1;   // 11-19 维护者定：苹果平台缺省按需（同核互斥）
#else
    const int dflt_excl = 0;
#endif
    const char *mn = mode == 4 ? "不设（缺省）" : mode == 2 ? "全部参与" : mode == 1 ? "按需" : mode == 0 ? "关（S2A_CORES=0）" : "关 + 不让先";
    int excl = mode == 1 || mode == 2 || (mode == 4 && dflt_excl), peer = mode != 3;
    s2a_test_mono_set(0);   // 钉钟：唤醒让先的 5 ms 安全阀不触发、让步退避不睡（本子进程只判序号 / 互斥语义）
    // ㉓
    { uint64_t ev[31] = {0}; svc_thread(0x45, ev); uint32_t hw = (uint32_t)ev[1]; g_lt_evr = (uint32_t)ev[2];
      uint64_t r[31] = {0, (uint64_t)lt_waiter, 0, 0, 30, 0}; svc_thread(0x08, r); uint64_t s_[31] = {r[1]}; svc_thread(0x09, s_);
      while (!s2a_test_blocked(r[1])) usleep(100);   // B 真进了 0x18、在等待队列里（不按墙钟假定）
      uint64_t sg[31] = {hw}; svc_thread(0x11, sg);
      atomic_store(&g_lt_w, atomic_fetch_add(&g_lt_seq, 1) + 1);
      while (!atomic_load(&g_lt_b)) usleep(1000);   // B 被唤醒后必然记序号（超时由整局 alarm 兜底）
      uint64_t pn = 0, pt = 0; s2a_test_peer_yield(&pn, &pt);
      printf("  ㉓ %s：B 序号 %d、W 序号 %d、让先 %llu 次、超时 %llu 次\n", mn, atomic_load(&g_lt_b), atomic_load(&g_lt_w), (unsigned long long)pn, (unsigned long long)pt);
      if (peer) CHECK(atomic_load(&g_lt_b) == 1 && atomic_load(&g_lt_w) == 2 && pn >= 1 && pt == 0, "㉓ %s：同核高优先级被唤醒先跑：期望 B 序号 1、W 序号 2、让先 ≥1 次、超时 0 → B %d、W %d、让先 %llu、超时 %llu", mn, atomic_load(&g_lt_b), atomic_load(&g_lt_w), pn, pt);
      // 反证只判「没让先」（计数）：不让先时 W 不等 B，B 在另一个宿主核上与 W 并行抢序号，谁先是时序（10-25 gate-host 门禁实测 W 2、B 1），不是可判的事实
      else CHECK(pn == 0, "㉓ %s（反证）：不让先：期望让先 0 次 → 让先 %llu（序号 W %d、B %d 只报数）", mn, pn, atomic_load(&g_lt_w), atomic_load(&g_lt_b)); }
    // ㉔
    { uint32_t h1, h2; uint64_t r1[31] = {0, (uint64_t)lc_worker, 0, 0, 44, 0}; svc_thread(0x08, r1); h1 = (uint32_t)r1[1];
      uint64_t r2[31] = {0, (uint64_t)lc_worker, 0, 0, 44, 0}; svc_thread(0x08, r2); h2 = (uint32_t)r2[1];
      uint64_t s1[31] = {h1}; svc_thread(0x09, s1); uint64_t s2[31] = {h2}; svc_thread(0x09, s2);
      for (int i = 0; i < 30000 && atomic_load(&g_lc_done) < 2; i++) usleep(1000);
      long tot = g_lc[0] + g_lc[1] + g_lc[2] + g_lc[3]; uint64_t sc = s2a_test_samecore_c();
      printf("  ㉔ %s：总数 %ld（全不丢 = 20000000）、同核诊断丙 %llu 次\n", mn, tot, (unsigned long long)sc);
      if (excl) CHECK(atomic_load(&g_lc_done) == 2 && tot == 20000000 && sc == 0, "㉔ %s：读过核号的同核线程互斥：期望总数 20000000、丙 0 → %ld、丙 %llu（完成 %d）", mn, tot, (unsigned long long)sc, atomic_load(&g_lc_done));
      else { int met = rv_case(rv_reader); printf("  ㉔ %s（不互斥）：会合等到对方 %d 条\n", mn, met);
        CHECK(atomic_load(&g_lc_done) == 2 && met == 2, "㉔ %s（不互斥）：两条读过核号的同核线程能同时在游戏代码里：期望会合 2 条 → %d（计数线程完成 %d）", mn, met, atomic_load(&g_lc_done)); } }
    // ㉖ 钉在单核上的线程不调 0x10 也参与（它知道自己在哪个核）：两条掩码只含核 0 的线程各改同一计数 5×200 万次
    { uint64_t r1[31] = {0, (uint64_t)pin_worker, 0, 0, 44, 0}; svc_thread(0x08, r1); uint64_t r2[31] = {0, (uint64_t)pin_worker, 0, 0, 44, 0}; svc_thread(0x08, r2);
      uint64_t s1[31] = {r1[1]}; svc_thread(0x09, s1); uint64_t s2[31] = {r2[1]}; svc_thread(0x09, s2);
      for (int i = 0; i < 30000 && atomic_load(&g_pin_done) < 2; i++) usleep(1000);
      printf("  ㉖ %s：钉核线程总数 %ld（全不丢 = 20000000）\n", mn, g_pin_cnt);
      if (excl) CHECK(atomic_load(&g_pin_done) == 2 && g_pin_cnt == 20000000, "㉖ %s：钉在单核、不读核号的线程也互斥：期望 20000000 → %ld", mn, g_pin_cnt);
      else { int met = rv_case(rv_pinned); printf("  ㉖ %s（不互斥）：会合等到对方 %d 条\n", mn, met);
        CHECK(atomic_load(&g_pin_done) == 2 && met == 2, "㉖ %s（不互斥）：两条钉核线程能同时在游戏代码里：期望会合 2 条 → %d", mn, met); } }
    // ㉕
    if (mode == 1) { uint32_t hq; uint64_t r[31] = {0, (uint64_t)q_free, 0, 0, 44, -2}; svc_thread(0x08, r); hq = (uint32_t)r[1]; uint64_t s_[31] = {hq}; svc_thread(0x09, s_);
      while (!atomic_load(&g_q_ready)) usleep(100); int hc = s2a_test_held_core(hq); atomic_store(&g_q_stop, 1);
      printf("  ㉕ 按需：没读过核号、不钉核的线程持核号 %d（不参与 = -1）\n", hc);
      CHECK(hc == -1, "㉕ 按需：没读过核号、不钉核（掩码 0x7）的线程不取核：期望持核号 -1 → %d", hc); }
    return bad;
}
// Run in a fresh process: the guest-thread key has not been created. On
// platforms where key zero is not reserved, occupy it with unrelated TSD so
// the old unguarded lookup cannot accidentally pass by returning NULL.
static int foreign_tsd_before_init(void) {
    uint32_t s2a_thread_current(void); void *s2a_thread_tlr(void);
    void s2a_core_block_begin(void), s2a_core_block_end(void);
    static unsigned char unrelated[512];
    if (!pthread_getspecific((pthread_key_t)0)) {
        pthread_key_t key;
        if (pthread_key_create(&key, NULL) || key != (pthread_key_t)0) {
            fprintf(stderr, "cannot establish unrelated key zero fixture\n"); return 2;
        }
        memset(unrelated, 0x5a, sizeof unrelated);
        if (pthread_setspecific(key, unrelated)) return 2;
    }
    if (s2a_thread_current() != 0) {
        fprintf(stderr, "foreign TSD was mistaken for a guest thread\n"); return 1;
    }
    if (s2a_thread_tlr() != NULL) return 1;
    s2a_core_block_begin(); s2a_core_block_begin();
    s2a_core_block_end(); s2a_core_block_end();
    puts("foreign TSD before guest initialization: no guest owner");
    return 0;
}
static void *foreign_tsd_after_init(void *unused) {
    (void)unused;
    uint32_t s2a_thread_current(void); void *s2a_thread_tlr(void);
    void s2a_core_block_begin(void), s2a_core_block_end(void);
    if (s2a_thread_current() || s2a_thread_tlr()) return (void *)1;
    s2a_core_block_begin(); s2a_core_block_begin();
    s2a_core_block_end(); s2a_core_block_end();
    return NULL;
}
// ---- ㉗㉘㉙㉚ 惰性取核（S2A_LAZY_CORE=1；thread.c 的 s2a_guest_return + entry.S xs_entry 的 blr 之后那个钩子）----
//   取核时点从「DM_BLOCK_END 当场取」挪到「回客体代码之前」：block_end 之后到真正回客体之间（宿主调用栈还在、可能还持着宿主锁）不持核，
//   于是「持宿主锁等虚拟核」这类 ABBA（botw：nx-82 持 g_rmu 等核 / nx-120 持核等 g_rmu）在机制上不再可能成立。
//   本文件只链 runtime/src/thread.c（entry.S 不进单测），钩子在 C 侧就是 s2a_guest_return()：这里直接调它，当作「回客体代码那一刻」。
//   开关直接写 g_lazy_core（非 static，entry.S 也读它）：不设 S2A_LAZY_CORE 的旧行为由全量门禁（118 组，S2A_LAZY_CORE 未设）覆盖。
extern int g_lazy_core;
void s2a_core_block_begin(void), s2a_core_block_end(void), s2a_guest_return(void);
void s2a_guest_freeze(int on);
uint32_t s2a_thread_current(void);
int s2a_test_off_guest(uint32_t h);
int s2a_test_unpaired(void);
int s2a_test_core_lendable(uint32_t h); void s2a_test_soft_counts(uint64_t *keeps, uint64_t *steals); extern int g_vcore_soft;
static _Atomic int g_lz_end_core = -9, g_lz_end_off = -9, g_lz_ret_core = -9, g_lz_ret_off = -9;
static _Atomic int g_lz_ready, g_lz_go, g_lz_done;
static _Atomic int g_lz2_in[3] = {-9, -9, -9}, g_lz2_out = -9, g_lz2_ready, g_lz2_go, g_lz2_done;
static _Atomic int g_lz3_ready, g_lz3_hook, g_lz3_go, g_lz3_core = -9, g_lz3_done;
static _Atomic int g_lz_h_ok, g_lz_h_done;
static void lz_worker(uint64_t arg) { (void)arg;    // ㉗：一次宿主阻塞 → 回客体
    uint64_t r[31] = {0}; svc_thread(0x25, r);                 // 一次系统调用：返回游戏前取到本核（0 号）
    s2a_core_block_begin(); s2a_core_block_end();              // 宿主阻塞：只放核；惰性化后不在这里取回
    atomic_store(&g_lz_end_core, s2a_test_core_lendable(S2A_H_CUR_THREAD) == 1 ? -1 : s2a_test_held_core(S2A_H_CUR_THREAD));   // 可借（不持 / 持着可借）记 -1
    atomic_store(&g_lz_end_off, s2a_test_off_guest(S2A_H_CUR_THREAD));
    atomic_store(&g_lz_ready, 1);
    while (!atomic_load(&g_lz_go)) usleep(1000);
    s2a_guest_return();                                        // = entry.S 里 blr 之后、ret 回客体之前那一刻
    atomic_store(&g_lz_ret_core, s2a_test_held_core(S2A_H_CUR_THREAD));
    atomic_store(&g_lz_ret_off, s2a_test_off_guest(S2A_H_CUR_THREAD));
    atomic_store(&g_lz_done, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void lz_nested(uint64_t arg) { (void)arg;    // ㉘：外层宿主调用里套内层 DM_BLOCK（up_wait_zero 的实际形态）
    uint64_t r[31] = {0}; svc_thread(0x25, r);
    s2a_core_block_begin();                                    // 外层：调用点还持着宿主锁（本例用「还没配平」代表）
    s2a_guest_return();                                        // 没配平就回客体？不许取核、打红早报（本例随后立刻配平，所以这条线程不会真被饿死）
    atomic_store(&g_lz2_in[0], s2a_test_core_lendable(S2A_H_CUR_THREAD) == 1 ? -1 : s2a_test_held_core(S2A_H_CUR_THREAD));
    s2a_core_block_begin(); s2a_core_block_end();              // 内层：只减到 1
    atomic_store(&g_lz2_in[1], s2a_test_core_lendable(S2A_H_CUR_THREAD) == 1 ? -1 : s2a_test_held_core(S2A_H_CUR_THREAD));
    s2a_core_block_end();                                      // 最外层结束：仍不取核（要等回客体）
    atomic_store(&g_lz2_in[2], s2a_test_core_lendable(S2A_H_CUR_THREAD) == 1 ? -1 : s2a_test_held_core(S2A_H_CUR_THREAD));
    atomic_store(&g_lz2_ready, 1);
    while (!atomic_load(&g_lz2_go)) usleep(1000);
    s2a_guest_return();
    atomic_store(&g_lz2_out, s2a_test_held_core(S2A_H_CUR_THREAD));
    atomic_store(&g_lz2_done, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
static void *lz_host_thread(void *p) { (void)p;    // ㉙：无槽的纯宿主线程
    uint32_t a = s2a_thread_current();
    s2a_guest_return();                                        // 没有线程对象 → 立刻返回（不能崩、不能取核）
    uint32_t b = s2a_thread_current();
    atomic_store(&g_lz_h_ok, a == 0 && b == 0);
    atomic_store(&g_lz_h_done, 1);
    return NULL;
}
static void lz_freeze_worker(uint64_t arg) { (void)arg;    // ㉚：冻结期间回客体不取核
    uint64_t r[31] = {0}; svc_thread(0x25, r);
    s2a_core_block_begin(); s2a_core_block_end();
    atomic_store(&g_lz3_ready, 1);                                // 冻结必须等这条线程出过系统调用再开：否则它停在 svc 里 park，走不到钩子
    while (!atomic_load(&g_lz3_go)) usleep(1000);
    atomic_store(&g_lz3_hook, 1);
    s2a_guest_return();                                        // 冻结中：先 park（放核等恢复），恢复后才取核
    atomic_store(&g_lz3_core, s2a_test_held_core(S2A_H_CUR_THREAD));
    atomic_store(&g_lz3_done, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
// ---- ㉛ 宿主侧可借（S2A_VCORE_SOFT，#1842）：进宿主没人等核时不放核、只置可借，回客体一次 CAS 拿回；有人等核就被取走。期望按构造手算：
//   ① 单线程（独占核 0）进出宿主 N 次：每次回客体都拿回原核 → 免锁计数恰涨 N、被取走 0；
//   ② 两条线程掩码都只含核 0，各做 N 次「回客体 → 非原子读改写共享计数 → 进宿主」：同核同一时刻只有一条在跑客体代码 → 计数恰为 2N（丢一次就是互斥破了）；
//      两条争一个核 → 被取走 > 0；③ 计时 A/B（只报数）：同一循环 S2A_VCORE_SOFT 关（每次放核取核都进 g_cmu）与开各跑一遍
enum { SC_N = 200000 };
#include <sys/resource.h>
static uint64_t cpu_ns(void) { struct rusage u; getrusage(RUSAGE_SELF, &u); return (uint64_t)(u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1000000000ull + (uint64_t)(u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1000ull; }
static long g_sc_cnt; static _Atomic int g_sc_done, g_sc_go;   // spawn_mask 不带参数：圈数用常量 SC_N
static void sc_loop(uint64_t arg) {
    uint64_t r[31] = {0}; svc_thread(0x25, r);   // 系统调用出口取到核 0
    while (!atomic_load(&g_sc_go)) usleep(100);
    s2a_core_block_begin(); s2a_core_block_end();
    (void)arg;
    for (int i = 0; i < SC_N; i++) {
        s2a_guest_return();                                  // 回客体：持核
        long v = g_sc_cnt; __asm__ volatile("" ::: "memory"); g_sc_cnt = v + 1;   // 客体代码：同核互斥才不丢
        s2a_core_block_begin(); s2a_core_block_end();        // 进宿主（直连调用）
    }
    s2a_guest_return();
    atomic_fetch_add(&g_sc_done, 1);
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
// ---- ㉜ 调度诊断玩家局缺省关（#1842）：判定纯函数 显式 > 开发设施 > 关；关着时有人等核、看门狗醒着也不采样、不做持核长跑探测（不挂起线程）；
//      开着照旧采样。A 持核 0 纯自旋、B 同核等核（同 ⑮ 的搭法），期望按构造：关 300 ms 采样 +0、长跑读数全不变；开后采样 ≥ 5 次 ----
static _Atomic int g_sd_ready, g_sd_stop;
static __attribute__((noinline)) void sd_spin(uint64_t arg) {
    uint64_t r[31] = {0}; svc_thread(0x25, r);
    if (!arg) atomic_store(&g_sd_ready, 1);
    while (!atomic_load(&g_sd_stop)) __asm__ volatile("" ::: "memory");
    uint64_t e[31] = {0}; svc_thread(0x0A, e);
}
int s2a_sched_diag_decide(const char *env, int dev); extern int g_sched_diag; void s2a_test_hostcore(uint64_t *, uint64_t *, uint64_t *, uint64_t *, uint64_t *, uint64_t *);
int main(void) {
    setenv("S2A_SCHED_DIAG", "1", 1);   // 调度计数 / 宿主侧持核长跑的用例（⑮ 等）要它：玩家局缺省关（#1842），本单测显式开；缺省关的行为由 ㉜ 单独测
    if (getenv("S2A_TEST_FOREIGN_TSD")) return foreign_tsd_before_init();
    // Isolate the deliberately occupied host key from the normal guest setup.
    { fflush(stdout); pid_t pid = fork();
      if (pid == 0) _exit(foreign_tsd_before_init());
      int st = 0; CHECK(pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0,
                        "uninitialized guest TSD treated an unrelated host key as a guest owner"); }
    pthread_key_create(&g_key18, NULL); pthread_key_create(&g_keytls, NULL); pthread_key_create(&g_keytlr, NULL); pthread_key_create(&g_keygate, NULL);
    signal(SIGALRM, timeout_kill); alarm(TIMEOUT_S);
    for (int mode = 4; mode >= 0; mode--) {   // 4：不设 S2A_CORES（缺省档，苹果应同按需）   // ㉓㉔㉕㉖：关 + 不让先（反证）/ 全部参与（S2A_CORES=1）/ 按需（=ondemand）/ 缺省（关、让先开）各一个子进程（cores_mode 进程内只读一次，必须在下面设 S2A_CORES=1 之前分出去）
        fflush(stdout); pid_t pid = fork();
        if (!pid) { g_case = "㉓㉔㉕㉖"; int rc_ = lite_case(mode); fflush(stdout); _exit(rc_ ? 1 : 0); }
        int st = 0; waitpid(pid, &st, 0); CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "㉓㉔㉕ 子进程（模式 %d：4 不设（缺省）/ 3 关+不让先 / 2 全部参与 / 1 按需 / 0 关）失败或异常退出（状态 %#x）", mode, st);
    }
    // 虚拟核开着跑全部用例（⑦⑧⑨ 只在开时有核锁可测；缺省关时它们空过、判据绿着失效）：S2A_CORES 缺省开前后都要在核锁下成立
    setenv("S2A_CORES", "1", 1);
    setenv("S2A_SAMECORE_DIAG", "1", 1);   // ⑮ 同核并发诊断：开虚拟核时按构造为 0，收走核后必须计到
    g_wtag = s2a_thread_main_init();   // 本线程 = W
    { uint32_t s2a_thread_current(void); pthread_t foreign; void *result = (void *)1;
      CHECK(s2a_thread_current() == g_wtag, "bound guest thread lost its identity");
      { void *s2a_thread_tlr(void); uint8_t *tlr = s2a_thread_tlr();
        CHECK(tlr && *(uint32_t *)(tlr + 0x110) == g_wtag, "主线程 TLR+0x110 应是本线程句柄 %#x，实为 %#x", g_wtag, tlr ? *(uint32_t *)(tlr + 0x110) : 0); }
      { uint64_t c[31] = {0, (uint64_t)tlr110_worker, 0, 0, 44, 0}; svc_thread(0x08, c); uint64_t st_[31] = {c[1]}; svc_thread(0x09, st_);
        while (!atomic_load(&g_tlr110_done)) usleep(1000);
        CHECK(c[0] == 0 && atomic_load(&g_tlr110) == (uint32_t)c[1] && atomic_load(&g_tlr110_me) == (uint32_t)c[1] && (uint32_t)c[1] != g_wtag,
              "建出线程 TLR+0x110 应是它自己的句柄 %#x（≠ 主线程 %#x），实为 %#x", (uint32_t)c[1], g_wtag, atomic_load(&g_tlr110)); }
      int rc = pthread_create(&foreign, NULL, foreign_tsd_after_init, NULL);
      if (!rc) rc = pthread_join(foreign, &result);
      CHECK(!rc && !result, "unbound host thread acquired a guest identity after initialization"); }
    // W 的掩码显式设成 0~2（理想核 0）：S2A_HZ_CORE 开时主线程按 Horizon 只给缺省核（掩码 0x1），后面的用例都按「W 可用 0~2 号核」排的局面，开关两边一致
    { uint64_t m[31] = {g_wtag, 0, 0x7}; svc_thread(0x0F, m); }
#ifdef T_FZ
    {   // 冻结代放核（见 fz_spin 处说明）：虚拟核开（本段 S2A_CORES=1）
        g_case = "冻结代放核";
        void s2a_core_leave(void);
        spawn(fz_spin, 0, NULL); s2a_core_leave();
        CHECK(wait_flag(&g_fz_ready, WAIT_LONG), "冻结代放核：忙等线程没起来");
        spawn(fz_taker, 0, NULL); s2a_core_leave(); usleep(300000);
        CHECK(!atomic_load(&g_fz_tdone), "冻结代放核：前提不成立——同核线程没被挡在出口（忙等线程没持核）");
        pthread_t fz; pthread_create(&fz, NULL, fz_freezer, NULL);
        FZ_FRAME(sl); sl[0] = 4000000000ull; svc_thread(0x0B, sl);   // W 睡在门里（安全点），等冻结 / 解冻走完
        pthread_join(fz, NULL);
        int rc = atomic_load(&g_fz_rc);
        CHECK(rc >= 0, "冻结代放核：冻结 30 秒没判齐（rc %d）——挂住的忙等线程仍持核，同核等核线程到不了安全点", rc);
        uint64_t n1 = atomic_load(&g_fz_n), n2 = n1;   // 等计数涨（原先睡 200 ms 再比：负载高时忙等线程还没被调度到 → 假红）
        for (int k = 0; k < WAIT_LONG && n2 == n1; k++) { usleep(1000); n2 = atomic_load(&g_fz_n); }
        CHECK(n2 > n1, "冻结代放核：解冻后忙等线程没接着跑（计数 %llu → %llu）", (unsigned long long)n1, (unsigned long long)n2);
        atomic_store(&g_fz_stop, 1);
        CHECK(wait_flag(&g_fz_tdone, WAIT_LONG), "冻结代放核：同核等核线程解冻后 30 秒没拿到核走完");
    }
#endif
    {   // 理想核 -1 / -3（照 Ryujinx KThread.SetCoreAndAffinityMask；Blade II 起局：设（-1, 0x2）后把读回值原样设给新线程，原先 -1 记成 0 → 读回（0, 0x2）→ InvalidCombination、Break）
        g_case = "理想核 -1 / -3";
        uint64_t a[31] = {0, (uint64_t)reader, 0, 0, 44, 0}; svc_thread(0x08, a);
        uint64_t b[31] = {0, (uint64_t)reader, 0, 0, 44, 0}; svc_thread(0x08, b);
        uint64_t s1[31] = {a[1], (uint64_t)(int64_t)-1, 0x2}; svc_thread(0x0F, s1);
        uint64_t q[31] = {0, 0, a[1]}; svc_thread(0x0E, q);
        CHECK(s1[0] == 0 && q[0] == 0 && (int32_t)q[1] == -1 && q[2] == 0x2, "设（-1, 0x2）后读回应是（-1, 0x2）：设 rc %#llx、读 rc %#llx 核 %d 掩码 %#llx",
              (unsigned long long)s1[0], (unsigned long long)q[0], (int32_t)q[1], (unsigned long long)q[2]);
        uint64_t s2[31] = {b[1], (uint64_t)(int64_t)(int32_t)q[1], q[2]}; svc_thread(0x0F, s2);
        CHECK(s2[0] == 0, "读回值原样设给另一条线程应成功：rc %#llx", (unsigned long long)s2[0]);
        uint64_t s3[31] = {b[1], 0, 0x1}; svc_thread(0x0F, s3);   // b 理想核 0、掩码 0x1
        uint64_t s4[31] = {b[1], (uint64_t)(int64_t)-3, 0x2}; svc_thread(0x0F, s4);
        uint64_t q2[31] = {0, 0, b[1]}; svc_thread(0x0E, q2);
        CHECK(s3[0] == 0 && s4[0] == 0xE801 && q2[1] == 0 && q2[2] == 0x1, "-3 不改理想核、原理想核 0 不在新掩码 0x2 → InvalidCombination 0xE801 且不改：rc %#llx、读回核 %d 掩码 %#llx",
              (unsigned long long)s4[0], (int32_t)q2[1], (unsigned long long)q2[2]);
        uint64_t s5[31] = {b[1], (uint64_t)(int64_t)-3, 0x3}; svc_thread(0x0F, s5);
        uint64_t q3[31] = {0, 0, b[1]}; svc_thread(0x0E, q3);
        CHECK(s5[0] == 0 && q3[1] == 0 && q3[2] == 0x3, "-3、原理想核 0 在新掩码 0x3 里 → 成功、理想核仍 0：rc %#llx、读回核 %d 掩码 %#llx",
              (unsigned long long)s5[0], (int32_t)q3[1], (unsigned long long)q3[2]);
        uint64_t s6[31] = {a[1], (uint64_t)(int64_t)-3, 0x2}; svc_thread(0x0F, s6);   // a 的理想核是 -1：照 Ryujinx（1UL << -1 = 第 63 位）→ InvalidCombination
        uint64_t q4[31] = {0, 0, a[1]}; svc_thread(0x0E, q4);
        CHECK(s6[0] == 0xE801 && (int32_t)q4[1] == -1 && q4[2] == 0x2, "-3、原理想核 -1 → InvalidCombination 0xE801 且不改：rc %#llx、读回核 %d 掩码 %#llx",
              (unsigned long long)s6[0], (int32_t)q4[1], (unsigned long long)q4[2]);
        { uint64_t c[31] = {a[1]}; svc_thread(0x16, c); } { uint64_t c[31] = {b[1]}; svc_thread(0x16, c); }   // 没启动，关掉
        g_case = "① 条件变量";
    }
    { uint64_t r[31] = {0, g_wtag}; svc_thread(0x25, r); char want[24], got[64] = ""; snprintf(want, sizeof want, "nx-%llu", (unsigned long long)r[1]);   // ⑥ 线程名 = nx-<GetThreadId>
      pthread_getname_np(pthread_self(), got, sizeof got); CHECK(!strcmp(want, got), "线程名：期望 %s → %s", want, got); }
    { uint64_t r[31] = {0, (uint64_t)reader, 0, 0, 44, 0}; svc_thread(0x08, r); CHECK(r[0] == 0, "CreateThread rc %#llx", r[0]); g_rtag = (uint32_t)r[1];
      uint64_t s[31] = {r[1]}; svc_thread(0x09, s); CHECK(s[0] == 0, "StartThread rc %#llx", s[0]); }
    pthread_mutex_lock(&g_m); while (!g_ready) pthread_cond_wait(&g_c, &g_m); pthread_mutex_unlock(&g_m);   // R 已过启动（不再要全局锁）、停在等 go 处
    uint32_t z = 0; atomic_compare_exchange_strong(&g_mutex, &z, g_wtag);
    s2a_test_cv_after_release = hook;
    uint64_t r[31] = {(uint64_t)&g_mutex, (uint64_t)&g_key, g_wtag, (uint64_t)-1};
    svc_thread(0x1C, r);
    CHECK(r[0] == 0 && atomic_load(&g_mutex) == g_wtag && atomic_load(&g_key) == 0,
          "条件变量：返回 %#llx（期望 0），锁字 %#x（期望 W 标签 %#x），条件键 %u（期望 0），R 状态 %d", r[0], atomic_load(&g_mutex), g_wtag, atomic_load(&g_key), atomic_load(&g_rstate));

    // 抢占延迟分布测量（S2A_PREEMPT_PROFILE=N 触发，量完即退；不设则跳过、对门禁零成本）
    if (getenv("S2A_PREEMPT_PROFILE")) { preempt_profile(); return 0; }

    // ② 句柄表
    g_case = "② 句柄表";
    uint32_t h = s2a_handle_new(S2A_OBJ_SESSION, 0xC0FFEE); uint32_t k = 0; uint64_t ck = 0;
    CHECK(h && s2a_handle_get(h, &k, &ck) && k == S2A_OBJ_SESSION && ck == 0xC0FFEE, "会话句柄 %#x 查回 种类 %u cookie %#llx", h, k, ck);
    CHECK(s2a_handle_new(S2A_OBJ_THREAD, 1) == 0, "C# 不许发线程句柄");
    uint64_t c[31] = {h}; svc_thread(0x16, c);
    CHECK(c[0] == 0 && g_closed_kind == S2A_OBJ_SESSION && g_closed_cookie == 0xC0FFEE, "游戏关会话句柄 → handle_closed 回调（种类 %d cookie %#llx）", g_closed_kind, g_closed_cookie);
    CHECK(!s2a_handle_get(h, &k, &ck), "关掉的句柄 %#x 仍能查到", h);
    uint32_t h2 = s2a_handle_new(S2A_OBJ_SESSION, 2);
    CHECK((h2 & 0x7fff) == (h & 0x7fff) && h2 != h && !s2a_handle_get(h, &k, &ck), "复用格子后旧句柄应失效（新 %#x 旧 %#x）", h2, h);
    CHECK(s2a_handle_get(S2A_H_CUR_THREAD, &k, &ck) && k == S2A_OBJ_THREAD && ck == g_wtag, "当前线程伪句柄");

    CHECK(s2a_handle_get(S2A_H_CUR_PROCESS, &k, &ck) && k == S2A_OBJ_PROCESS && ck == s2a_process_id(), "进程伪句柄 → PROCESS 种类、cookie = 进程号");
    { static uint8_t al[0x8000] __attribute__((aligned(0x4000)));
      CHECK(s2a_shmem_wrap(al + 0x1000, 0x1000) == 0, "非 16 KB 对齐的后备应拒");
      uint32_t hs = s2a_shmem_wrap(al, 0x4000); uint64_t sz = 0; void *s2a_shmem_lookup(uint32_t, uint64_t *);
      CHECK(hs && s2a_shmem_lookup(hs, &sz) == al && sz == 0x4000, "包装共享内存：查回同一后备");
      uint32_t kk; uint64_t cc; CHECK(s2a_handle_get(hs, &kk, &cc) && kk == S2A_OBJ_SHMEM && cc == (uint64_t)(uintptr_t)al, "共享内存句柄 cookie 应为宿主指针"); }

    // 动森 M0 根因的最小复现（10-19）：C# 经 Horizon CreateEvent 建事件、关句柄——关成功应回 1，关不存在的句柄回 0
    { uint32_t r_, w_; uint64_t e_ = s2a_event_create(&r_, &w_); s2a_event_release(e_);   // 只要句柄：立刻放 C# 那份
      CHECK(s2a_handle_close(r_) == 1 && s2a_handle_close(w_) == 1, "关事件两端句柄应回 1");
      CHECK(s2a_handle_close(r_) == 0, "重复关同一句柄应回 0"); }

    // ③ 事件
    g_case = "③ 事件";
    uint32_t hr; uint64_t ev = s2a_event_create(&hr, NULL);
    uint32_t hs[1] = {hr}; uint64_t w[31] = {0, (uint64_t)hs, 1, 0};
    svc_thread(0x18, w); CHECK(w[0] == 0xEA01, "未触发、超时 0：期望 0xEA01 → %#llx", w[0]);
    s2a_event_signal(ev);
    uint64_t w2[31] = {0, (uint64_t)hs, 1, 1000000000ull}; uint64_t t0 = now_ns(); svc_thread(0x18, w2);
    CHECK(w2[0] == 0 && w2[1] == 0, "已触发：期望不挂起、回 0 下标 0（挂起的话没人再触发、只会超时回 0xEA01）→ %#llx 下标 %llu，%.1f ms", w2[0], w2[1], (now_ns() - t0) / 1e6);
    uint32_t hd = s2a_event_dup_read(ev); uint64_t cl[31] = {hr}; svc_thread(0x16, cl);
    uint32_t hs2[1] = {hd}; uint64_t w3[31] = {0, (uint64_t)hs2, 1, 0}; svc_thread(0x18, w3);
    CHECK(w3[0] == 0, "复制的读端在原读端关后仍可等：%#llx", w3[0]);
    { uint32_t kk; uint64_t cc; CHECK(s2a_handle_get(hd, &kk, &cc) && kk == S2A_OBJ_EVENT_R && cc == ev, "事件句柄 cookie 应为事件号");
      s2a_event_retain(cc); uint64_t cl2[31] = {hd}; svc_thread(0x16, cl2); s2a_event_release(ev);   // 游戏关掉句柄、C# 释放自己那份后，retain 的仍有效
      s2a_event_signal(cc); s2a_event_release(cc); }
    CHECK(s2a_handle_get(S2A_H_CUR_THREAD, &k, &ck) && ck == g_wtag, "当前线程伪句柄 cookie 仍为句柄（伪句柄约定不变）");

    g_case = "③b 等待中关句柄";
    cw_case(1, 5000000000ull, 0); cw_case(0, 300000000ull, 0xEA01);
    // ④
    g_case = "④ SignalToAddress 类型 2";
    arb64_case(); arb_case(0, 0, 11, 0); arb_case(2, 0, 8, 2); arb_case(2, -1, 8, 2); arb_case(1, 1, 9, 1); arb_case(2, 2, 9, 2); arb_case(3, 2, 10, 2); arb_case(2, 1, 10, 1);
    // ⑤
    g_case = "⑤ 争锁压力";
    uint64_t t5 = now_ns(); stress(1);
    CHECK(g_overlap == 0 && g_count == (long)NTH * NIT && atomic_load(&g_lk) == 0, "争锁：重叠 %d 次（期望 0），计数 %ld（期望 %d），收尾锁字 %#x（期望 0）", atomic_load(&g_overlap), g_count, NTH * NIT, atomic_load(&g_lk));
    double ms5 = (now_ns() - t5) / 1e6;
    stress(0);
    CHECK(g_overlap > 0, "反例（不加锁）：检测器没抓到重叠（重叠 %d 次、计数 %ld）——判据失效", atomic_load(&g_overlap), g_count);
    int ov0 = atomic_load(&g_overlap);

    {   // ㉙ 停帧现场：「在跑」的线程打最近的同步类系统调用（号、参数、地址处的值、返回）。期望手写：游戏线程对 g_sv 发 3 次
        //   SignalToAddress（类型 0、数 1，无人等待 → 返回 0，值不变 7）后空转；导出现场应恰有 3 行是 svc 0x35、参数首个 = &g_sv、调前调后都是 7、返回 0。
        //   反证：去掉 thread.c 里打印这几行的那段 → 0 行
        atomic_store(&g_sv, 7); atomic_store(&g_sv_go, 0); atomic_store(&g_sv_done, 0);
        g_case = "㉙ 停帧现场的同步调用";
        void s2a_core_leave(void);
        spawn(sv_spinner, 0, NULL); s2a_core_leave();   // 同 ⑦：主线程放核，空转线程拿核跑；导出现场是宿主侧、不用核
        int ok = wait_flag(&g_sv_done, WAIT_LONG);
        g_capn = 0; g_cap[0] = 0; atomic_store(&g_cap_on, 1); void s2a_thread_dump(void); s2a_thread_dump(); atomic_store(&g_cap_on, 0);
        char want[160]; snprintf(want, sizeof want, "svc 0x35 参数 %#llx 0 0x7 0x1，地址处值 调前 0x7 调后 0x7，返回 0", (unsigned long long)(uintptr_t)&g_sv);
        int hits = 0; for (const char *q = g_cap; (q = strstr(q, want)); q++) hits++;
        CHECK(ok && hits == 3, "㉙ 停帧现场没打出在跑线程最近的同步调用（期望恰 3 行含「%s」，实 %d 行；空转线程跑到 %d），截到：%s", want, hits, ok, g_cap);
        // 同一份现场：在跑线程还要打通用寄存器（x0–x28 共 8 行）。反证：去掉寄存器那段 → 0 行
        int regl = 0; for (const char *q = g_cap; (q = strstr(q, "寄存器 x28 ")); q++) regl++;
        CHECK(regl >= 1, "㉙ 停帧现场没打在跑线程的通用寄存器（寄存器 x28 行 %d 个）", regl);
        atomic_store(&g_sv_go, 1); wait_flag(&g_sv_exit, WAIT_LONG);   // 空转线程退出、放核后主线程再往下
    }
    {   // ㉛ 停帧现场：等条件的线程打锁 / 键周围原样内存（从锁地址按 16 对齐往前 16 字节起 48 字节）。期望手写：
        //   等待中锁字已放（0）、键置 1，其余 10 个字是预先写的样式；导出现场应有一行「键周围内存 <结构首址>：」接这 12 个字。
        //   反证：去掉 thread.c 里打这一行的那段 → 0 行
        g_case = "㉛ 停帧现场的键周围内存";
        static const uint32_t pat[12] = {0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x0badf00d, 0, 0, 0xcafe0001, 0x55555555, 0x66666666, 0x77777777, 0x88888888};
        for (int i = 0; i < 12; i++) atomic_store(&g_kw[i], pat[i]);
        atomic_store(&g_kw_done, 0);
        void s2a_core_leave(void);
        spawn(kw_waiter, 0, NULL); s2a_core_leave();
        int blocked = 0; for (int k = 0; k < 5000 && !(blocked = s2a_test_waiters(2, (uint64_t)(uintptr_t)&g_kw[6]) > 0); k++) usleep(1000);
        g_capn = 0; g_cap[0] = 0; atomic_store(&g_cap_on, 1); void s2a_thread_dump(void); s2a_thread_dump(); atomic_store(&g_cap_on, 0);
        char want[200]; snprintf(want, sizeof want, "键周围内存 %#llx：11111111 22222222 33333333 44444444 | 0badf00d 00000000 00000001 cafe0001 | 55555555 66666666 77777777 88888888",
                                 (unsigned long long)(uintptr_t)&g_kw[0]);
        CHECK(blocked && strstr(g_cap, want), "㉛ 停帧现场没打出等条件线程的键周围内存（期望含「%s」；等待者挂上 %d），截到：%s", want, blocked, g_cap);
        uint64_t sg[31] = {(uint64_t)(uintptr_t)&g_kw[6], 1}; svc_thread(0x1D, sg); wait_flag(&g_kw_done, WAIT_LONG);   // 放它走：拿回锁、退出
    }

    {   // ⑦
        g_case = "⑦ 暂停空转线程放核";
        void s2a_core_leave(void);
        uint32_t hs; spawn(guest_spin, 0, &hs); s2a_core_leave();
        CHECK(wait_flag(&g_spin_ready, WAIT_LONG), "⑦ 空转线程没起来");
        uint64_t pr[31] = {hs, 1}; svc_thread(0x32, pr); s2a_core_leave();
        CHECK(pr[0] == 0, "⑦ 暂停在游戏代码里空转的线程：rc %#llx", pr[0]);
        spawn(core_taker, 0, NULL); s2a_core_leave();
        CHECK(wait_flag(&g_t_done, WAIT_LONG), "⑦ 同核线程 30 秒拿不到核（被暂停的线程仍持核）");
#if defined(__APPLE__)
        // 暂停读上下文（审计 P2-15）：0x33 放开全局锁、写出参之前另一条线程恢复目标（thr_unpause 持锁清 pmach 再 thread_resume）。
        //   期望按语义定：返回的是暂停期间拍下的那份——rc 0、pc 落在 guest_spin 的「游戏代码」段 [guest_spin, +0x400)（module_of 桩同口径）
        g_case = "⑦ 暂停读上下文";
        g_ctx_h = hs; g_hw_hook = ctx_unpause_hook;
        uint8_t ctx[0x320]; memset(ctx, 0xA5, sizeof ctx);
        uint64_t cr[31] = {(uint64_t)(uintptr_t)ctx, hs}; svc_thread(0x33, cr); s2a_core_leave();
        g_hw_hook = NULL;
        uint64_t cpc; memcpy(&cpc, ctx + 0x100, 8);
        uint64_t glo = (uint64_t)(void *)guest_spin;
        CHECK(g_ctx_unpaused == 1 && cr[0] == 0 && cpc >= glo && cpc < glo + 0x400,
              "⑦ 暂停读上下文：并发恢复后应仍得暂停时快照 rc 0、pc ∈ [%#llx, +0x400)；实 恢复 %d 次 rc %#llx pc %#llx", glo, g_ctx_unpaused, cr[0], cpc);
#endif
        uint64_t ur[31] = {hs, 0}; svc_thread(0x32, ur); s2a_core_leave();
        atomic_store(&g_spin_stop, 1);
    }
#if defined(__APPLE__)
    {   // 暂停中不释放（审计 #859 F-apple-pause-nopin）：主线程对一条在宿主代码里空转的线程发 SetThreadActivity（0x32）暂停；
        //   0x32 查完句柄、放开全局锁之后，让目标退出、再关它的句柄。期望按「系统调用用完之前线程对象不能释放」定：关句柄那一刻（0x32 还要用对象）释放 0 次；0x32 用完后释放 1 次（不漏）
        g_case = "暂停中不释放";
        extern void (*s2a_test_thr_freed)(void *); extern void (*s2a_test_after_lookup)(void *);
        atomic_store(&g_np_go, 0); atomic_store(&g_np_in_pause, 0); atomic_store(&g_np_free_in, 0); atomic_store(&g_np_free_after, 0); g_np_t = NULL;
        uint32_t hn; spawn(np_host_spin, 0, &hn); void s2a_core_leave(void); s2a_core_leave();
        CHECK(wait_flag(&g_np_ready, WAIT_LONG), "暂停中不释放：目标线程没起来");
        g_np_h = hn; s2a_test_thr_freed = np_freed; s2a_test_after_lookup = np_iter;
        uint64_t pr[31] = {hn, 1}; svc_thread(0x32, pr); s2a_core_leave();
        s2a_test_after_lookup = NULL;
        CHECK(atomic_load(&g_np_free_in) == 0, "暂停中不释放：0x32 还在用对象时（关句柄那一刻）被释放 %d 次（期望 0）", atomic_load(&g_np_free_in));
        CHECK(pr[0] == 0xFA01, "暂停中不释放：暂停开始前目标已退出，SetThreadActivity 期望回 0xfa01（InvalidState，审计 P1-49：原先回 0 当成已暂停）→ 实 %#llx", (unsigned long long)pr[0]);
        CHECK(atomic_load(&g_np_free_in) + atomic_load(&g_np_free_after) == 1, "暂停中不释放：对象总共释放 %d 次（期望 1，不漏）", atomic_load(&g_np_free_in) + atomic_load(&g_np_free_after));
        s2a_test_thr_freed = NULL;
    }
#endif
#if defined(__APPLE__)
    {   // P1-49 ①
        g_case = "P1-49 ① C# 系统调用里被暂停";
        void s2a_core_leave(void);
        uint32_t h; spawn(pz_cs_target, 0, &h); s2a_core_leave();
        CHECK(wait_flag(&g_pz_in, WAIT_LONG), "P1-49 ① 目标没进 C# 系统调用");
        uint64_t pr[31] = {h, 1}; svc_thread(0x32, pr); s2a_core_leave();
        atomic_store(&g_pz_go, 1);
        for (int i = 0; i < 2000 && !s2a_test_parked(h) && !atomic_load(&g_pz_after); i++) usleep(1000);
        int parked = s2a_test_parked(h), after = atomic_load(&g_pz_after);
        uint64_t ur[31] = {h, 0}; svc_thread(0x32, ur); s2a_core_leave();
        int after2 = wait_flag(&g_pz_after, WAIT_LONG);
        CHECK(pr[0] == 0 && parked && !after && ur[0] == 0 && after2,
              "P1-49 ① 期望 暂停 rc 0、出 C# 后停在出口（自己停进 pause_park 1、越过出口 0）、恢复 rc 0、恢复后越过出口 1 → 实 %#llx、%d、%d、%#llx、%d",
              (unsigned long long)pr[0], parked, after, (unsigned long long)ur[0], after2);
    }
    {   // P1-49 ②
        g_case = "P1-49 ② svc_exit 清 in_svc 前被暂停";
        extern void (*s2a_test_svc_leave)(void *); void s2a_core_leave(void);
        atomic_store(&g_pz_in, 0); atomic_store(&g_pz_go, 0); atomic_store(&g_pz_after, 0); atomic_store(&g_pz_h, 0);
        s2a_test_svc_leave = pz_leave_hook;
        uint32_t h; spawn(pz_svc_target, 0, &h); s2a_core_leave(); atomic_store(&g_pz_h, h);
        CHECK(wait_flag(&g_pz_in, WAIT_LONG), "P1-49 ② 目标没走到 svc_exit 的清 in_svc 之前");
        uint64_t pr[31] = {h, 1}; svc_thread(0x32, pr); s2a_core_leave();
        atomic_store(&g_pz_go, 1);
        for (int i = 0; i < 2000 && !s2a_test_parked(h) && !atomic_load(&g_pz_after); i++) usleep(1000);
        int parked = s2a_test_parked(h), after = atomic_load(&g_pz_after);
        s2a_test_svc_leave = NULL;
        uint64_t ur[31] = {h, 0}; svc_thread(0x32, ur); s2a_core_leave();
        int after2 = wait_flag(&g_pz_after, WAIT_LONG);
        CHECK(pr[0] == 0 && parked && !after && ur[0] == 0 && after2,
              "P1-49 ② 期望 暂停 rc 0、停在出口（停进 pause_park 1、回到游戏代码 0）、恢复 rc 0、恢复后回到游戏代码 1 → 实 %#llx、%d、%d、%#llx、%d",
              (unsigned long long)pr[0], parked, after, (unsigned long long)ur[0], after2);
    }
    {   // P1-49 ③
        g_case = "P1-49 ③ Mach 挂住后、发布前被恢复";
        extern void (*s2a_test_pause_suspended)(void *); void s2a_core_leave(void);
        spawn(pm_spin, 0, &g_pm_h); s2a_core_leave();
        CHECK(wait_flag(&g_pm_ready, WAIT_LONG), "P1-49 ③ 空转线程没起来");
        s2a_test_pause_suspended = pm_hook;
        uint64_t pr[31] = {g_pm_h, 1}; svc_thread(0x32, pr); s2a_core_leave();
        s2a_test_pause_suspended = NULL;
        int sc = pm_suspend_count(); uint64_t n0 = atomic_load(&g_pm_n); int moved = 0;
        for (int i = 0; i < 2000 && !(moved = atomic_load(&g_pm_n) != n0); i++) usleep(1000);
        CHECK(g_pm_unpause_rc == 0 && pr[0] == 0xFA01 && sc == 0 && moved,
              "P1-49 ③ 期望 中途恢复 rc 0、暂停 rc 0xfa01（没停住）、目标 Mach 挂起计数 0、目标继续在跑 1 → 实 %#llx、%#llx、%d、%d",
              (unsigned long long)g_pm_unpause_rc, (unsigned long long)pr[0], sc, moved);
        for (int k = pm_suspend_count(); k > 0; k--) thread_resume(g_pm_port);   // 红时也放开，免得退出卡住
        atomic_store(&g_pm_stop, 1);
    }
#endif
    {   // ㉑
        g_case = "㉑ 直连阻塞放核";
        void s2a_core_leave(void);
        spawn(bk_waiter, 0, NULL); s2a_core_leave();
        CHECK(wait_flag(&g_bk_in, WAIT_LONG), "㉑ 阻塞线程没起来");
        spawn(bk_signaler, 0, NULL); s2a_core_leave();
        CHECK(wait_flag(&g_bk_done, WAIT_LONG), "㉑ A 在 s2a_core_block_begin / end 之间阻塞时，同核 B 2.5 秒拿不到核（阻塞不放核）");
    }
    {   // ㉗ 惰性取核（重定）：block_end 之后仍不持核且 off_guest=1，回客体代码那一刻才取核
        g_case = "㉗ 惰性取核：block_end 后不持核、回客体后持核";
        void s2a_core_leave(void);
        // 负对照（开关关 = 旧行为）：同一段代码在 block_end 里就取核 —— 证明下面「-1 / 1」的判据分得开这两种行为
        g_lazy_core = 0;
        { uint32_t h0; atomic_store(&g_lz_end_core, -9); atomic_store(&g_lz_end_off, -9);
          atomic_store(&g_lz_ready, 0); atomic_store(&g_lz_go, 0); atomic_store(&g_lz_done, 0);
          spawn(lz_worker, 0, &h0); s2a_core_leave();
          CHECK(wait_flag(&g_lz_ready, WAIT_LONG), "㉗ 负对照线程没起来");
          CHECK(atomic_load(&g_lz_end_core) >= 0 && atomic_load(&g_lz_end_off) == 0,
                "㉗ 负对照（开关关）：block_end 里就应已取核、off_guest 0 → %d、%d", atomic_load(&g_lz_end_core), atomic_load(&g_lz_end_off));
          atomic_store(&g_lz_go, 1); CHECK(wait_flag(&g_lz_done, WAIT_LONG), "㉗ 负对照线程没走完"); }
        atomic_store(&g_lz_end_core, -9); atomic_store(&g_lz_end_off, -9); atomic_store(&g_lz_ret_core, -9); atomic_store(&g_lz_ret_off, -9);
        atomic_store(&g_lz_ready, 0); atomic_store(&g_lz_go, 0); atomic_store(&g_lz_done, 0);
        g_lazy_core = 1;
        uint32_t hl; spawn(lz_worker, 0, &hl); s2a_core_leave();
        CHECK(wait_flag(&g_lz_ready, WAIT_LONG), "㉗ 阻塞线程没起来");
        { int lend = s2a_test_core_lendable(hl), og = s2a_test_off_guest(hl);   // 宿主侧可借（S2A_VCORE_SOFT）：持着但等核者能直接取走，与不持核同样不构成「持宿主锁等核」的环
          CHECK(lend == 1 && og == 1, "㉗ block_end 之后（还没回客体）核应可借（不持 / 持着可借）、off_guest=1：期望 1、1 → %d、%d", lend, og); }
        CHECK(atomic_load(&g_lz_end_core) == -1 && atomic_load(&g_lz_end_off) == 1, "㉗ （线程内自读）期望持核 -1、off_guest 1 → %d、%d",
              atomic_load(&g_lz_end_core), atomic_load(&g_lz_end_off));
        atomic_store(&g_lz_go, 1);
        CHECK(wait_flag(&g_lz_done, WAIT_LONG), "㉗ 线程没走完");
        CHECK(atomic_load(&g_lz_ret_core) >= 0 && atomic_load(&g_lz_ret_core) <= 2 && atomic_load(&g_lz_ret_off) == 0,
              "㉗ 回到客体代码后必须持核：期望持核 0~2、off_guest 0 → %d、%d", atomic_load(&g_lz_ret_core), atomic_load(&g_lz_ret_off));
    }
    {   // ㉘ 嵌套（up_wait_zero 的形态）：外层还没配平 / 内层 end 之后都不得取核，直到最外层宿主调用返回客体代码
        g_case = "㉘ 惰性取核：嵌套 block 只在回客体后取核";
        void s2a_core_leave(void);
        int u0 = s2a_test_unpaired();
        uint32_t hn; spawn(lz_nested, 0, &hn); s2a_core_leave();
        CHECK(wait_flag(&g_lz2_ready, WAIT_LONG), "㉘ 嵌套线程没起来");
        // 未配对时那次 s2a_guest_return() 走的是「不取核 + 打红」分支：判据是计数涨 1（日志只在前 8 次打，别靠看日志）
        CHECK(s2a_test_unpaired() - u0 == 1, "㉘ 外层未配平时回客体：应打红 1 次（不取核）→ %d 次", s2a_test_unpaired() - u0);
        CHECK(atomic_load(&g_lz2_in[0]) == -1 && atomic_load(&g_lz2_in[1]) == -1 && atomic_load(&g_lz2_in[2]) == -1,
              "㉘ 外层未配平 / 内层 end 后 / 最外层 end 后都应不持核：期望 -1 -1 -1 → %d %d %d",
              atomic_load(&g_lz2_in[0]), atomic_load(&g_lz2_in[1]), atomic_load(&g_lz2_in[2]));
        atomic_store(&g_lz2_go, 1);
        CHECK(wait_flag(&g_lz2_done, WAIT_LONG), "㉘ 嵌套线程没走完");
        CHECK(atomic_load(&g_lz2_out) >= 0 && atomic_load(&g_lz2_out) <= 2, "㉘ 最外层宿主调用返回客体代码后应持核：→ %d", atomic_load(&g_lz2_out));
    }
    {   // ㉙ 无槽的纯宿主线程调钩子 = 空操作。出图 / 计数 / 音频 / dm-encode / nvn-present 这几处宿主回调走 xs_stubs 的 1: br x16 尾跳
        //   （无槽或宿主→宿主嵌套），根本进不了 entry.S 里那个钩子；这里是同一条判断在 C 侧的双保险：没有线程对象就直接返回
        g_case = "㉙ 惰性取核：纯宿主线程空操作";
        pthread_t th; int cr = pthread_create(&th, NULL, lz_host_thread, NULL);
        CHECK(cr == 0, "㉙ 建宿主线程失败 rc=%d", cr);
        CHECK(wait_flag(&g_lz_h_done, WAIT_LONG), "㉙ 纯宿主线程调 s2a_guest_return() 没返回（应立刻返回）");
        if (!cr) pthread_join(th, NULL);
        CHECK(atomic_load(&g_lz_h_ok) == 1, "㉙ 纯宿主线程调钩子后不该有线程身份：ok %d", atomic_load(&g_lz_h_ok));
    }
    {   // ㉚ 暂停 / 冻结期间不得取核（判据与 svc_exit 同一处：t->pause || g_frz。这里用冻结 g_frz 这条分支，t->pause 分支由 ⑦ 的 0x32 用例覆盖）
        g_case = "㉚ 惰性取核：冻结期间不取核";
        void s2a_core_leave(void); void s2a_guest_freeze(int on);
        uint32_t hz; spawn(lz_freeze_worker, 0, &hz); s2a_core_leave();
        CHECK(wait_flag(&g_lz3_ready, WAIT_LONG), "㉚ 冻结线程没起来");
        s2a_guest_freeze(1);
        atomic_store(&g_lz3_go, 1);
        CHECK(wait_flag(&g_lz3_hook, WAIT_LONG), "㉚ 线程没走到回客体钩子");
        usleep(150000);
        { int hc = s2a_test_held_core(hz);
          CHECK(!atomic_load(&g_lz3_done) && hc == -1 && atomic_load(&g_lz3_core) == -9,
                "㉚ 冻结期间回客体：应 park 住不取核 → 完成 %d、持核 %d、钩子后持核 %d",
                atomic_load(&g_lz3_done), hc, atomic_load(&g_lz3_core)); }
        s2a_guest_freeze(0);
        CHECK(wait_flag(&g_lz3_done, WAIT_LONG), "㉚ 解冻后线程没走完");
        CHECK(atomic_load(&g_lz3_core) >= 0 && atomic_load(&g_lz3_core) <= 2, "㉚ 解冻后应取到核：→ %d", atomic_load(&g_lz3_core));
        g_lazy_core = 0;   // 收尾恢复旧行为（后面的用例不依赖惰性；本条用例之后也没有 block_* 调用者）
    }
    {   // ㉜ 调度诊断玩家局缺省关（见 sd_spin 处说明）
        g_case = "㉜ 调度诊断缺省关";
        CHECK(s2a_sched_diag_decide(NULL, 0) == 0 && s2a_sched_diag_decide("", 0) == 0, "㉜ 不设、开发设施关 → 关");
        CHECK(s2a_sched_diag_decide(NULL, 1) == 1, "㉜ 不设、开发设施开 → 开");
        CHECK(s2a_sched_diag_decide("1", 0) == 1 && s2a_sched_diag_decide("0", 1) == 0, "㉜ 显式最优先：=1 开、=0 关");
        void s2a_core_leave(void);
        int saved = g_sched_diag; g_sched_diag = 0;
        atomic_store(&g_sd_ready, 0); atomic_store(&g_sd_stop, 0);
        spawn(sd_spin, 0, NULL); s2a_core_leave();
        CHECK(wait_flag(&g_sd_ready, WAIT_LONG), "㉜ A 没起来");
        spawn(sd_spin, 1, NULL); s2a_core_leave();   // B 同核等核：看门狗醒着（有等核者）
        usleep(50000);
        uint64_t n0 = sched_samples(), h0[6], h1[6]; s2a_test_hostcore(&h0[0], &h0[1], &h0[2], &h0[3], &h0[4], &h0[5]);
        usleep(300000);
        uint64_t n1 = sched_samples(); s2a_test_hostcore(&h1[0], &h1[1], &h1[2], &h1[3], &h1[4], &h1[5]);
        int same = 1; for (int i = 0; i < 6; i++) same &= h0[i] == h1[i];
        CHECK(n1 == n0 && same, "㉜ 关：有人等核 300 ms，采样应 +0、持核长跑读数不变 → 采样 +%llu、长跑%s", (unsigned long long)(n1 - n0), same ? "不变" : "变了");
        g_sched_diag = 1;
        uint64_t n2 = n1; for (int k = 0; k < WAIT_LONG && n2 - n1 < 5; k++) { usleep(1000); n2 = sched_samples(); }
        CHECK(n2 - n1 >= 5, "㉜ 开：采样应恢复（≥ 5 次）→ +%llu", (unsigned long long)(n2 - n1));
        atomic_store(&g_sd_stop, 1); usleep(20000);
        g_sched_diag = saved;
    }
    {   // ㉛ 宿主侧可借（见 sc_loop 处说明）
        g_case = "㉛ 宿主侧可借：免锁拿回 / 被取走 / 同核互斥";
        void s2a_core_leave(void);
        g_lazy_core = 1; uint64_t k0, s0, k1, s1, k2, s2;
        double ns_off = 0, ns_on = 0;
        for (int pass = 0; pass < 2; pass++) {   // 0：关（每次进 g_cmu）、1：开；单线程，同一循环
            g_vcore_soft = pass; g_sc_cnt = 0; atomic_store(&g_sc_done, 0); atomic_store(&g_sc_go, 0);
            spawn_mask(sc_loop, 1); s2a_core_leave();
            usleep(20000); s2a_test_soft_counts(&k0, &s0);   // 计数窗从「工作线程已取到核、等开跑」起：不含主线程放核 / 工作线程首次取核
            uint64_t t0 = now_ns(); atomic_store(&g_sc_go, 1);
            CHECK(wait_flag(&g_sc_done, WAIT_LONG), "㉛ 单线程循环没走完（%s）", pass ? "开" : "关");
            double ns = (double)(now_ns() - t0) / SC_N; if (pass) ns_on = ns; else ns_off = ns;
            s2a_test_soft_counts(&k1, &s1);
            if (pass) CHECK(k1 - k0 == SC_N + 1 && s1 - s0 == 0 && g_sc_cnt == SC_N, "㉛① 开、独占核 0：免锁拿回应恰 %d、被取走 0、计数 %d → %llu、%llu、%ld", SC_N + 1, SC_N, (unsigned long long)(k1 - k0), (unsigned long long)(s1 - s0), g_sc_cnt);
            else CHECK(k1 - k0 == 0, "㉛① 关：不该有免锁拿回 → %llu", (unsigned long long)(k1 - k0));
        }
        printf("  ㉛③ 进出宿主一次（含回客体）：关 %.0f ns、开 %.0f ns（单线程、只报数）\n", ns_off, ns_on);
        for (int pass = 0; pass < 2; pass++) {   // ㉛④（只报数）：4 条线程各占一个核（掩码 1 / 2 / 4 / 8）同时进出宿主——放核取核都抢同一把 g_cmu、放核还广播
            g_vcore_soft = pass; g_sc_cnt = 0; atomic_store(&g_sc_done, 0); atomic_store(&g_sc_go, 0);
            for (int c = 0; c < 4; c++) { spawn_mask(sc_loop, 1u << c); s2a_core_leave(); }
            usleep(50000); uint64_t t0 = now_ns(), c0 = cpu_ns(); atomic_store(&g_sc_go, 1);
            { uint64_t w = now_ns(); while (atomic_load(&g_sc_done) < 4 && now_ns() - w < 60000000000ull) usleep(500); }
            printf("  ㉛④ 4 线程各占一核同时进出宿主 %d 次：%s 墙钟 %.1f ms、进程 CPU %.1f ms（每次 %.0f ns CPU）\n", SC_N, pass ? "开" : "关",
                   (now_ns() - t0) / 1e6, (cpu_ns() - c0) / 1e6, (double)(cpu_ns() - c0) / (4.0 * SC_N));
        }
        g_vcore_soft = 1; g_sc_cnt = 0; atomic_store(&g_sc_done, 0); atomic_store(&g_sc_go, 0);
        spawn_mask(sc_loop, 1); s2a_core_leave(); spawn_mask(sc_loop, 1); s2a_core_leave();
        usleep(20000); s2a_test_soft_counts(&k1, &s1); atomic_store(&g_sc_go, 1);
        { uint64_t t0 = now_ns(); while (atomic_load(&g_sc_done) < 2 && now_ns() - t0 < 60000000000ull) usleep(1000); }
        s2a_test_soft_counts(&k2, &s2);
        CHECK(atomic_load(&g_sc_done) == 2, "㉛② 两条同核线程没走完（%d / 2）", atomic_load(&g_sc_done));
        CHECK(g_sc_cnt == 2L * SC_N, "㉛② 同核互斥：共享计数应恰 %d → %ld（少了 = 两条同时在跑客体代码）", 2 * SC_N, g_sc_cnt);
        CHECK(s2 - s1 > 0, "㉛② 两条争核 0：应有被取走 → 免锁 %llu、取走 %llu", (unsigned long long)(k2 - k1), (unsigned long long)(s2 - s1));
        printf("  ㉛② 两线程同核：免锁拿回 %llu 次、被取走 %llu 次\n", (unsigned long long)(k2 - k1), (unsigned long long)(s2 - s1));
        g_lazy_core = 0;
    }
    {   // ㉒ 没人等核时看门狗不轮询（10-24 测试设备2：常年 0.5 ms 轮询每秒 2000 次唤醒，直连整机多 1.16 W）：200 ms 里主动扫描 ≤ 2 次；旧代码约 400 次（反证）
        g_case = "㉒ 看门狗空闲不轮询";
        uint64_t s2a_test_wd_scans(void); void s2a_core_leave(void);
        s2a_core_leave();   // 本线程放核；前面用例的线程退完就没有等核者（原先只睡 50 ms 就当退完了，负载高时还有等核者、看门狗在扫 → 假红）
        uint64_t a = 0, b = 99;   // 反复取 200 ms 窗，直到某窗扫描 ≤ 2 次（负载高只会让一窗里扫得更少）；150 窗（约 30 秒）都在扫 = 真在空转轮询
        for (int k = 0; k < 150 && b - a > 2; k++) { a = s2a_test_wd_scans(); usleep(200000); b = s2a_test_wd_scans(); }
        CHECK(b - a <= 2, "㉒ 没人等核的 200 ms 里看门狗扫了 %llu 次（期望 ≤ 2）", (unsigned long long)(b - a));
    }
    {   // ㉗ 路径二：宿主区与 svc 往复 → svc_exit 消费 preempt_req（放核后按优先级重取）
        g_case = "㉗ 宿主区持核被抢占（svc_exit 消费）";
        void s2a_core_leave(void); void s2a_test_preempt_req_consume(int); void s2a_test_preempt_host_release(int);
        s2a_test_preempt_host_release(0);   // 关掉路径一（on_preempt 当场放核）：这一段只让 svc_exit 的消费起作用，两条路径各自隔离
        for (int neg = 0; neg < 2; neg++) {   // neg = 1：负对照——关掉消费，B 必须超时（否则判据证明不了修好了）
            s2a_test_preempt_req_consume(!neg);
            atomic_store(&g_hp_ready, 0); atomic_store(&g_hp_stop, 0); atomic_store(&g_hp_done, 0);
            atomic_store(&g_hp_acore, -9); atomic_store(&g_hp_wcore, -9);
            uint32_t ha = spawn_core_mask(hp_host_loop, 0, 48, 1, 0x2); s2a_core_leave(); s2a_core_leave();
            CHECK(wait_flag(&g_hp_ready, WAIT_LONG) && atomic_load(&g_hp_acore) == 1,
                  "㉗ A（prio 48、掩码 0x2）应持 1 号核：起床 %d、持核 %d", wait_flag(&g_hp_ready, 0), atomic_load(&g_hp_acore));
            uint32_t hb = spawn_core_mask(hp_waiter, 0, 44, 1, 0x2); s2a_core_leave(); s2a_core_leave();
            int got = wait_flag(&g_hp_done, neg ? 500 : WAIT_LONG);   // 正向等它拿到核（30 秒兜底）；负对照 500 ms 里仍拿不到即可
            CHECK(neg ? !got : got,
                  "㉗%s A 在宿主区与 svc 之间往复时，同核高优先级 B %s（持核 %d；关掉 svc_exit 的 preempt_req 消费后应一直被挡）",
                  neg ? "（负对照）" : "", got ? "拿到核" : "拿不到核", atomic_load(&g_hp_wcore));
            atomic_store(&g_hp_stop, 1); usleep(20000);
            uint64_t c[31] = {0}; c[0] = ha; svc_thread(0x16, c); c[0] = hb; svc_thread(0x16, c);
        }
        s2a_test_preempt_req_consume(1); s2a_test_preempt_host_release(1);
    }
    {   // ㉗ 路径一：A 长住宿主区、永不回 svc_exit —— preempt_req 没人消费，只能靠 on_preempt 当场 trylock 放核
        g_case = "㉗ 宿主区持核被抢占（on_preempt 当场放核）";
        void s2a_core_leave(void); void s2a_test_preempt_host_release(int); void s2a_test_preempt_req_consume(int);
        s2a_test_preempt_req_consume(1);
        for (int neg = 0; neg < 2; neg++) {   // neg = 1：负对照——关掉 on_preempt 的当场放核，B 必须超时
            s2a_test_preempt_host_release(!neg);
            atomic_store(&g_hp_ready, 0); atomic_store(&g_hp_stop, 0); atomic_store(&g_hp_done, 0);
            atomic_store(&g_hp_acore, -9); atomic_store(&g_hp_wcore, -9);
            uint32_t ha = spawn_core_mask(hp_host_hold, 0, 48, 1, 0x2); s2a_core_leave(); s2a_core_leave();
            CHECK(wait_flag(&g_hp_ready, WAIT_LONG) && atomic_load(&g_hp_acore) == 1,
                  "㉗ A（prio 48、掩码 0x2）应持 1 号核：起床 %d、持核 %d", wait_flag(&g_hp_ready, 0), atomic_load(&g_hp_acore));
            uint32_t hb = spawn_core_mask(hp_waiter, 0, 44, 1, 0x2); s2a_core_leave(); s2a_core_leave();
            int got = wait_flag(&g_hp_done, neg ? 500 : WAIT_LONG);   // 正向等它拿到核（30 秒兜底）；负对照 500 ms 里仍拿不到即可
            CHECK(neg ? !got : got,
                  "㉗%s A 卡在宿主区不返回时，同核高优先级 B %s（持核 %d；关掉 on_preempt 的当场放核后应一直被挡）",
                  neg ? "（负对照）" : "", got ? "拿到核" : "拿不到核", atomic_load(&g_hp_wcore));
            atomic_store(&g_hp_stop, 1);
            wait_flag(&g_hp_done, WAIT_LONG); usleep(20000);
            uint64_t c[31] = {0}; c[0] = ha; svc_thread(0x16, c); c[0] = hb; svc_thread(0x16, c);
        }
        s2a_test_preempt_host_release(1);
    }
    {   // ⑧（掩码 0~2 显式设：在 0 号核建、启动前设掩码 0x7——不靠 -2 的语义，S2A_HZ_CORE 开关两边测的都是「掩码内换核」）
        g_case = "⑧ 同掩码换核";
        void s2a_core_leave(void);
        spawn_core_mask(mask_spin, 0, 44, 0, 0x7); s2a_core_leave();
        CHECK(wait_flag(&g_m_ready, WAIT_LONG), "⑧ 空转线程没起来");
        spawn_core_mask(mask_taker, 0, 44, 0, 0x7); s2a_core_leave();
        CHECK(wait_flag(&g_m_done, WAIT_LONG), "⑧ 同掩码（0~2）线程 30 秒拿不到别的核（全挤在理想核上）");
        atomic_store(&g_m_stop, 1);
    }
    {   // ⑨
        g_case = "⑨ 迁移";
        void s2a_core_leave(void);
        uint32_t ha9 = spawn_core_mask(mig_spin, 0, 44, 0, 0x7); s2a_core_leave();   // 掩码 0~2 显式设（不靠 -2 的语义）
        CHECK(wait_flag(&g_g_ready, WAIT_LONG), "⑨ 空转线程 A 没起来");
        int x = atomic_load(&g_g_acore);
        CHECK(x >= 0 && x <= 2, "⑨ A（掩码 0~2）应持 0~2 号核 → %d", x);
        if (x >= 0) {
            spawn_core(mig_taker, x); s2a_core_leave();
            CHECK(wait_flag(&g_g_done, WAIT_LONG), "⑨ 掩码只含 %d 号核的线程 B 30 秒拿不到核（A 占着空转、别的核空着却没把 A 挪走）", x);
            int ab = s2a_test_held_core(ha9);
            CHECK(atomic_load(&g_g_bcore) == x && ab >= 0 && ab <= 2 && ab != x, "⑨ 迁移后：期望 B 持 %d 号核、A 持 0~2 中别的核 → B %d、A %d", x, atomic_load(&g_g_bcore), ab);
        }
        atomic_store(&g_g_stop, 1);
    }
    {   // ⑩ yield 同优先级轮转
        g_case = "⑩ yield 同优先级轮转";
        void s2a_core_leave(void);
        spawn_mask(yld_a, 0x1); s2a_core_leave();
        CHECK(wait_flag(&g_y_aready, WAIT_LONG), "⑩ A 没取到核");
        spawn_mask(yld_b, 0x1); s2a_core_leave();
        CHECK(wait_flag(&g_y_bdone, WAIT_LONG) && atomic_load(&g_y_bheld) == 0,
            "⑩ A yield 后同优先级 B 没在有界时间拿到核（旧版 yield 立即拿回）→ B done %d 持核 %d", atomic_load(&g_y_bdone), atomic_load(&g_y_bheld));
        atomic_store(&g_y_stop, 1);
    }
    {   // ⑪ yield 轮转决策点：确定性反证（纯函数、不走并发、不受 svc 0x0B 里 sched_yield 的软让干扰）
        g_case = "⑪ yield 轮转决策";
        int s2a_test_yield_claims(void);
        int yc = s2a_test_yield_claims();
        CHECK(yc == 1, "⑪ core_claimed 未按 yield 轮转挡让出者（期望 1：让出者被同优先级、登记更早者挡住重取核，未让出时不挡；旧版不看 yielded 返 0，反证不过）→ 得 %d", yc);
    }
    {   // ⑫ 纯自旋时钟抢占：L 低优先级持核后纯自旋（不走 svc），H 高优先级同核等核；看门狗抢占 L → H 有界时间上核
        g_case = "⑫ 纯自旋时钟抢占";
        void s2a_core_leave(void);
        void s2a_test_preempt_diag(uint64_t *, uint64_t *, uint64_t *, uint64_t *, int *);
        uint32_t pe_lh = 0, pe_hh = 0;
        spawn_prio(preempt_spin, 48, &pe_lh); s2a_core_leave();            // L 低优先级（prio 数值大）
        CHECK(wait_flag(&g_pe_lready, WAIT_LONG) && atomic_load(&g_pe_lcore) == 0, "⑫ L 没取到核 0 纯自旋");
        uint64_t pe_s0 = 0, pe_r0 = 0; s2a_test_preempt_diag(0, &pe_s0, &pe_r0, 0, 0);   // 起点 SIGURG 发 / 达（失败时算本例增量）
        spawn_prio(preempt_hi, 40, &pe_hh); s2a_core_leave();              // H 高优先级（prio 数值小）同核 0 等核
        // 门禁只判「抢占最终发生、不死锁」：抢占延迟受系统负载影响（gate-host 超载 / 整机卡时 CPU 看门狗被挤、可能很慢），
        // 用「X ms 内抢占」这类时间阈值在超载 gate-host 上必然 flaky（和 t_det 计时判一个毛病）。给 10 秒宽超时——正常抢占亚毫秒级，
        // 10 秒只为抓「永不抢占」的真死锁、并给超载余量；抢占延迟快慢交 S2A_PREEMPT_PROFILE=N 报数、不在门禁判红。
        uint64_t pe_t0 = now_ns(); int pe_ok = wait_flag(&g_pe_hdone, WAIT_LONG);
        printf("  ⑫ 抢占延迟 %.1f ms（仅报数、不判红；延迟分布见 S2A_PREEMPT_PROFILE=N build/tests/t_thread）\n", (double)(now_ns() - pe_t0) / 1e6);
        if (!(pe_ok && atomic_load(&g_pe_hcore) == 0)) {
            // 失败现场（真门禁偶发才走到；profile 5000 次压不出）：看门狗没扫 / 信号发了没达 / 达了但 PC 不在客体区，三者一眼分开
            uint64_t wd = 0, s1 = 0, r1 = 0, lpc = 0; int lg = 0; s2a_test_preempt_diag(&wd, &s1, &r1, &lpc, &lg);
            printf("  ⑫诊断：本例 SIGURG 发 %llu 达 %llu、看门狗最后扫距今 %.1f ms、L 持核 %d、H 持核 %d、最后目标 PC %#llx 客体区 %d\n",
                (unsigned long long)(s1 - pe_s0), (unsigned long long)(r1 - pe_r0), wd ? (double)(now_ns() - wd) / 1e6 : -1.0,
                s2a_test_held_core(pe_lh), s2a_test_held_core(pe_hh), (unsigned long long)lpc, lg);
            void s2a_thread_dump(void); s2a_thread_dump();
        }
        CHECK(pe_ok && atomic_load(&g_pe_hcore) == 0,
            "⑫ 看门狗 10 秒内没抢占纯自旋的低优先级 L、高优先级 H 拿不到核（真死锁；无抢占时 L 纯自旋永持核、H 饿死反证不过）→ H done %d 核 %d", atomic_load(&g_pe_hdone), atomic_load(&g_pe_hcore));
        atomic_store(&g_pe_stop, 1);
    }
    {   // ⑬ 宿主持锁被抢占信号打断不死锁（on_preempt 宿主区若碰锁会死锁）
        g_case = "⑬ 宿主持锁不死锁";
        void s2a_core_leave(void);
        spawn_prio(hostlock_worker, 44, NULL); s2a_core_leave();
        CHECK(wait_flag(&g_hl_done, WAIT_LONG), "⑬ 持宿主锁的线程被抢占信号轰炸后没在有界时间完成（疑死锁：on_preempt 宿主区碰了锁）");
        atomic_store(&g_hl_stop, 1);
    }
    {   // ⑭ 空闲核迁移（纯函数）：H 高优先级持核 0、掩码 0/1，L 只能核 0，核 1 空 → Horizon：H 迁到核 1 照跑，L 上核 0
        g_case = "⑭ 空闲核迁移";
        int s2a_test_migrate_guard(void);
        int mg = s2a_test_migrate_guard();
        CHECK(mg == 1, "⑭ 空闲核 1 应把 H 拉过去、L 上核 0（Horizon 语义）→ 得 %d（1=符合 0=不符）", mg);
    }
    {   // ⑮
        g_case = "⑮ 调度计数";
        void s2a_core_leave(void); void s2a_test_sched_stat(uint64_t *, int *, uint64_t *, uint64_t *, uint64_t *);
        uint64_t n0, m0, c0, p0, n1, m1, c1, p1, n2, m2, c2, p2; int pk;
        void s2a_test_samecore(uint64_t *, uint64_t *, uint64_t *); uint64_t sn0, sa0, sb0, sn1, sa1, sb1, sn2, sa2, sb2;
        usleep(20000); s2a_test_sched_stat(&n0, &pk, &m0, &c0, &p0); s2a_test_samecore(&sn0, &sa0, &sb0);
        uint32_t ha; spawn(ss_spin, 0, &ha); s2a_core_leave();
        CHECK(wait_flag(&g_ss_ready[0], WAIT_LONG), "⑮ A 没起来");
        spawn(ss_spin, 1, NULL); s2a_core_leave();   // B 同优先级同核：A 纯自旋不让核，B 等核（同优先级不抢占）
        // 等两路采样各满 10 次再读（原先睡 100 ms 当「够 10 次」，负载高时采样线程没跑够 → 假红）
        for (int k = 0; k < WAIT_LONG; k++) { s2a_test_sched_stat(&n1, &pk, &m1, &c1, &p1); s2a_test_samecore(&sn1, &sa1, &sb1); if (n1 - n0 >= 10 && sn1 - sn0 >= 10) break; usleep(1000); }
        CHECK(sn1 - sn0 >= 10 && sa1 == sa0 && sb1 == sb0, "⑮ 同核并发诊断干净段（开虚拟核）：采样 %llu 次、甲 +%llu、乙 +%llu（应 ≥10 / 0 / 0）",
            (unsigned long long)(sn1 - sn0), (unsigned long long)(sa1 - sa0), (unsigned long long)(sb1 - sb0));
        CHECK(n1 - n0 >= 10 && m1 == m0 && c1 == c0 && p1 == p0, "⑮ 干净段：采样 %llu 次、同核≥2 +%llu、不持核在跑 +%llu、同核同优先级 +%llu（应 ≥10 / 0 / 0 / 0）",
            (unsigned long long)(n1 - n0), (unsigned long long)(m1 - m0), (unsigned long long)(c1 - c0), (unsigned long long)(p1 - p0));
        void s2a_test_steal_core(uint32_t); s2a_test_steal_core(ha);   // 收走 A 的核：A 不持核接着自旋 → B 上核
        CHECK(wait_flag(&g_ss_ready[1], WAIT_LONG), "⑮ 收走 A 的核后 B 没拿到核");
        // 正对照：等各项计数都涨过再读（原先睡 100 ms）
        for (int k = 0; k < WAIT_LONG; k++) { s2a_test_sched_stat(&n2, &pk, &m2, &c2, &p2); s2a_test_samecore(&sn2, &sa2, &sb2);
            if (sa2 > sa1 && sb2 > sb1 && m2 > m1 && c2 > c1 && p2 > p1 && pk >= 2) break; usleep(1000); }
        CHECK(sa2 > sa1 && sb2 > sb1, "⑮ 同核并发诊断正对照（A、B 同在核 0 掩码 0x1，收走 A 的核后两条同时在跑）没计到：甲 +%llu、乙 +%llu",
            (unsigned long long)(sa2 - sa1), (unsigned long long)(sb2 - sb1));
        CHECK(m2 > m1 && c2 > c1 && p2 > p1 && pk >= 2, "⑮ 正对照（收走核后不持核在跑）没被计到：同核≥2 +%llu、不持核在跑 +%llu、同核同优先级 +%llu、峰值 %d",
            (unsigned long long)(m2 - m1), (unsigned long long)(c2 - c1), (unsigned long long)(p2 - p1), pk);
        printf("  ⑮ 干净段采样 %llu 次全 0；正对照段同核≥2 %llu 次、不持核在跑 %llu 次、峰值 %d\n", (unsigned long long)(n1 - n0),
            (unsigned long long)(m2 - m1), (unsigned long long)(c2 - c1), pk);
        printf("  ⑮ 同核并发诊断：干净段采样 %llu 次 甲 %llu 乙 %llu；正对照段 甲 %llu 乙 %llu\n", (unsigned long long)(sn1 - sn0),
            (unsigned long long)(sa1 - sa0), (unsigned long long)(sb1 - sb0), (unsigned long long)(sa2 - sa1), (unsigned long long)(sb2 - sb1));
        atomic_store(&g_ss_stop, 1);
    }
    {   // ㉑ 宿主侧持核长跑（判据：持核 + 不在系统调用 + 没放核 + PC 不在游戏模块）。
        //   要的是「连续持核多久」而不是「采到多少次」——每帧短暂过路会积累很多命中却不是隐患，段长与最长才是
        g_case = "㉑ 宿主侧持核长跑";
        void s2a_test_hostcore(uint64_t *, uint64_t *, uint64_t *, uint64_t *, uint64_t *, uint64_t *);
        uint64_t s0, h0, o0, sh0, mx0, ps0, s1, h1, o1, sh1, mx1, ps1;
        s2a_test_hostcore(&s0, &h0, &o0, &sh0, &mx0, &ps0);
        atomic_store(&g_hc_stop, 0); atomic_store(&g_hc_left, 0);
        spawn(hc_host_spin, 0, NULL); s2a_core_leave();
        CHECK(wait_flag(&g_hc_ready, WAIT_LONG), "㉑ 宿主侧持核线程没起来");
        wait_samples(6, 120000000ull);   // 持核期间采样器至少采 6 次（原 usleep 120 ms 假定按时采到）
        atomic_store(&g_hc_stop, 1);
        // 设了停止不等于线程已停：负载高时持核线程几个采样周期都分不到 CPU、看不到停止，仍在持核，「再采两次」就落空，
        //   这段拖到负对照窗口里才结算、两条一起红（11-01 22:1x 负载 40–47，main 上同样复现）。先等它确认已放核，再数采样
        CHECK(wait_flag(&g_hc_left, WAIT_LONG), "㉑ 宿主侧持核线程 10 秒没放核");
        wait_samples(2, 0);              // 放核后再采两次：这段在下一次采样时结算
        s2a_test_hostcore(&s1, &h1, &o1, &sh1, &mx1, &ps1);
        // 判这一段本身的长度（最近结算段），不看最长值的增量：最长是累计的，前序用例已有更长段时增量恒为 0，结果随用例顺序与负载变（10-31 #1196 n06ody 查明）
        uint64_t s2a_test_hostcore_last_us(void); uint64_t hc_last = s2a_test_hostcore_last_us();
        CHECK(s1 > s0 && h1 > h0 && o1 > o0 && hc_last >= 2000, "㉑ 宿主代码里持核 120 ms 没记成长跑段：段 +%llu 命中 +%llu 超阈值 +%llu 本段 %llu us（最长 %llu us）",
              (unsigned long long)(s1 - s0), (unsigned long long)(h1 - h0), (unsigned long long)(o1 - o0), (unsigned long long)hc_last, (unsigned long long)mx1);
        {   // 负对照：同样持核、同样不在系统调用，但 PC 在游戏代码里（ss_spin）→ 不算宿主侧长跑
            atomic_store(&g_ss_stop, 0);
            spawn(ss_spin, 0, NULL); s2a_core_leave();
            CHECK(wait_flag(&g_ss_ready[0], WAIT_LONG), "㉑ 负对照线程没起来");
            wait_samples(6, 120000000ull);   // 负对照窗口里采样器也要真采过，否则「没计到」不能说明判对了
            atomic_store(&g_ss_stop, 1);
            wait_samples(2, 0);
            uint64_t s2, h2, o2, sh2, mx2, ps2; s2a_test_hostcore(&s2, &h2, &o2, &sh2, &mx2, &ps2);
            CHECK(s2 == s1 && h2 == h1 && o2 == o1, "㉑ 在游戏代码里持核被当成宿主侧长跑：段 +%llu 命中 +%llu 超阈值 +%llu（应 0 / 0 / 0）",
                  (unsigned long long)(s2 - s1), (unsigned long long)(h2 - h1), (unsigned long long)(o2 - o1));
        }
        printf("  ㉑ 宿主侧持核：长跑段 +%llu、命中 +%llu、最长 %llu ms；过路命中 %llu 次（一采即走不计）\n",
               (unsigned long long)(s1 - s0), (unsigned long long)(h1 - h0), (unsigned long long)((mx1 - mx0) / 1000), (unsigned long long)(sh1 - sh0));
    }
    {   // ⑯
        g_case = "⑯ 新线程进门持核";
        void s2a_core_leave(void);
        spawn(start_probe, 0, NULL); s2a_core_leave();
        CHECK(wait_flag(&g_sp_done, WAIT_LONG) && atomic_load(&g_sp_core) == 0, "⑯ 新线程进游戏代码时不持核（入口第一条读持核 %d，应 0；done %d）", atomic_load(&g_sp_core), atomic_load(&g_sp_done));
    }
    {   // ⑰
        g_case = "⑰ 恢复后先取核";
        void s2a_core_leave(void); void s2a_test_sched_stat(uint64_t *, int *, uint64_t *, uint64_t *, uint64_t *);
        uint64_t c0, c1; uint32_t ha;
        spawn(ua_spin, 0, &ha); s2a_core_leave();
        CHECK(wait_flag(&g_ua_ready[0], WAIT_LONG), "⑰ A 没起来");
        spawn(ua_spin, 1, NULL); s2a_core_leave();   // B 同优先级同核等核
        uint64_t pr[31] = {ha, 1}; svc_thread(0x32, pr); s2a_core_leave();   // 暂停 A（暂停方代放核）→ B 上核
        CHECK(pr[0] == 0 && wait_flag(&g_ua_ready[1], WAIT_LONG), "⑰ 暂停 A 后 B 没拿到核（rc %#llx）", pr[0]);
        // 同步判定（10-23 改：原「恢复后 100 ms 内全局采样计数不涨」依赖看门狗采样时机，还会把别的线程 / 恢复窗口算进来，门禁负载下偶发红）：
        //   按采样器同一判定式直接问 A——恢复调用一返回问一次（恢复窗口），A 进入等核后再问一次（稳态）；两次都不能是「在跑却不持核」。
        //   旧代码（恢复后不挂抢占信号）A 恢复即不持核在跑，两次都是 1；恢复窗口未标宿主侧的版本第一次可能是 1
        int s2a_test_thr_nocore(uint32_t, int *); int wc = 0;
        uint64_t ur[31] = {ha, 0}; svc_thread(0x32, ur); s2a_core_leave();   // 恢复 A：B 持核自旋，A 应等核
        int n0 = s2a_test_thr_nocore(ha, NULL);
        int n1 = 1; for (int i = 0; i < 2000; i++) { n1 = s2a_test_thr_nocore(ha, &wc); if (wc || n1) break; usleep(1000); }
        CHECK(n0 == 0 && n1 == 0 && wc == 1, "⑰ 恢复后 A 不持核在跑：恢复返回时 %d、之后 %d（都应 0），等核 %d（应 1）", n0, n1, wc);
        (void)c0; (void)c1;
        atomic_store(&g_ua_stop[1], 1);   // B 退出放核 → A 拿到核
        usleep(20000); atomic_store(&g_ua_stop[0], 1);
    }
    {   // ⑱
        g_case = "⑱ 同一线程连续两次被抢占";
        void s2a_core_leave(void);
        spawn_prio_arg(rp_spin, 0, 48); s2a_core_leave();
        CHECK(wait_flag(&g_rp_ready, WAIT_LONG), "⑱ L 没起来");
        spawn_prio_arg(rp_hi, 0, 40); s2a_core_leave();
        int h1 = wait_flag(&g_rp_hdone[0], WAIT_LONG);
        usleep(20000);   // H1 退出放核，L 回核接着自旋
        spawn_prio_arg(rp_hi, 1, 40); s2a_core_leave();
        int h2 = wait_flag(&g_rp_hdone[1], WAIT_LONG);
        CHECK(h1 && h2, "⑱ 第一次抢占 %s、第二次抢占 %s（期望都成功；第二次失败 = 被抢过一次的线程之后收不到抢占信号）", h1 ? "成功" : "失败", h2 ? "成功" : "失败");
        atomic_store(&g_rp_stop, 1);
    }
    {   // ⑲
        g_case = "⑲ 抢占不碰被抢线程 sp 下方";
        void s2a_core_leave(void);
        spawn_prio_arg(rz_spin, 0, 48); s2a_core_leave();
        CHECK(wait_flag(&g_rz_ready, WAIT_LONG), "⑲ L 没起来");
        int got = 0;
        for (int i = 0; i < 40; i++) { int want = i + 1; spawn_prio_arg(rz_hi, 0, 40); s2a_core_leave();
            for (int k = 0; k < 3000 && atomic_load(&g_rz_hdone) < want; k++) usleep(1000);
            if (atomic_load(&g_rz_hdone) >= want) got++; usleep(2000); }
        atomic_store(&g_rz_stop, 1);
        for (int k = 0; k < 2000 && !atomic_load(&g_rz_loops); k++) usleep(1000);
        CHECK(got == 40 && atomic_load(&g_rz_bad) == 0, "⑲ 抢占 %d / 40 次成功；sp 下方标记被改 %llu 次（应 0，共转 %llu 圈）", got, (unsigned long long)atomic_load(&g_rz_bad), (unsigned long long)atomic_load(&g_rz_loops));
    }
    {   // ⑲b
        g_case = "⑲b 抢占不碰被抢线程 sp 下方 4 KB";
        void s2a_core_leave(void);
        spawn_prio_arg(bz_spin, 0, 48); s2a_core_leave();
        CHECK(wait_flag(&g_bz_ready, WAIT_LONG), "⑲b L 没起来");
        int got = 0;
        for (int i = 0; i < 40; i++) { int want = i + 1; spawn_prio_arg(bz_hi, 0, 40); s2a_core_leave();
            for (int k = 0; k < 3000 && atomic_load(&g_bz_hdone) < want; k++) usleep(1000);
            if (atomic_load(&g_bz_hdone) >= want) got++; usleep(2000); }
        atomic_store(&g_bz_stop, 1);
        for (int k = 0; k < 2000 && !atomic_load(&g_bz_loops); k++) usleep(1000);
        CHECK(got == 40 && atomic_load(&g_bz_bad) == 0, "⑲b 抢占 %d / 40 次成功；sp 下方 4 KB 标记被改 %llu 个（应 0，共转 %llu 圈）", got, (unsigned long long)atomic_load(&g_bz_bad), (unsigned long long)atomic_load(&g_bz_loops));
    }
    {   // ⑳
        g_case = "⑳ 空闲核迁移不看优先级";
        void s2a_core_leave(void);
        spawn_core_mask(mg_spin, 0, 32, 1, 0x7); s2a_core_leave();
        CHECK(wait_flag(&g_mg_hready, WAIT_LONG), "⑳ H 没起来");
        spawn_core_mask(mg_lo, 0, 44, 1, 0x2); s2a_core_leave();
        spawn_core_mask(mg_lo, 1, 44, 1, 0x2); s2a_core_leave();
        for (int k = 0; k < WAIT_LONG && atomic_load(&g_mg_wdone) < 2; k++) usleep(1000);
        int ok = atomic_load(&g_mg_wdone) == 2;
        CHECK(ok && atomic_load(&g_mg_wcore[0]) == 1 && atomic_load(&g_mg_wcore[1]) == 1, "⑳ W1 / W2（优先级 44、只能 1 号核）应都拿到 1 号核：拿到 %d 个，持核 %d / %d（旧规则：H 优先级 32 持 1 号核自旋、0/2 号核空，W 永远等）", atomic_load(&g_mg_wdone), atomic_load(&g_mg_wcore[0]), atomic_load(&g_mg_wcore[1]));
        atomic_store(&g_mg_stop, 1);
        for (int k = 0; k < WAIT_LONG && atomic_load(&g_mg_hcore_after) == -9; k++) usleep(1000);
        int hc = atomic_load(&g_mg_hcore_after);
        CHECK(!ok || hc == 0 || hc == 2, "⑳ H 应被迁到空闲的 0 或 2 号核继续跑，实持核 %d", hc);
    }
    {
        g_case = "锁持有者句柄无效";
        const uint32_t BOGUS = 0x7ff0u << 15 | 0x1fffu;   // 从没分配过的句柄
        atomic_store(&g_bl_lock, BOGUS | WAITER);
        uint64_t r[31] = {BOGUS, (uint64_t)&g_bl_lock, g_wtag}; svc_thread(0x1A, r);
        CHECK(r[0] == 0xE401, "ArbitrateLock 持有者句柄无效：应回 InvalidHandle 0xE401，实 %#llx", (unsigned long long)r[0]);
        spawn(bl_cv, 0, &g_bl_h);
        uint64_t t0 = now_ns(); while (s2a_test_waiters(2, (uint64_t)&g_bl_key) < 1 && now_ns() - t0 < 30000000000ull) usleep(1000);
        atomic_store(&g_bl_lock, BOGUS);   // 等待者已放锁、挂上条件变量；把锁字改成无效持有者
        uint64_t sg[31] = {(uint64_t)&g_bl_key, 1}; svc_thread(0x1D, sg);
        CHECK(wait_flag(&g_bl_done, WAIT_LONG) && atomic_load(&g_bl_rc) == 0xE401, "条件变量唤醒后锁字持有者无效：等待者应以 0xE401 醒来（%s，rc %#llx）",
              atomic_load(&g_bl_done) ? "醒了" : "30 秒没醒", (unsigned long long)atomic_load(&g_bl_rc));
    }
    {
        g_case = "ArbitrateLock 快路径";
        atomic_store(&g_fp_lock, 0); spawn(fp_arb, 0, &g_fp_h); wait_flag(&g_fp_ready, WAIT_LONG); usleep(20000);
        s2a_test_glock_hold(1); atomic_store(&g_fp_go, 1);
        int done = wait_flag(&g_fp_done, WAIT_LONG);   // 持着 G 等：快路径不拿 G 就一定回得来；拿 G 就回不来，30 秒兜底判红（原 1 秒是墙钟）
        s2a_test_glock_hold(0);
        if (!done) wait_flag(&g_fp_done, WAIT_LONG);   // 反证路径：放了 G 它才回来，等它收尾
        CHECK(done && atomic_load(&g_fp_rc) == 0, "锁字对不上时 ArbitrateLock 应不拿全局锁、持 G 期间照样回 0（%s，rc %#llx）",
              done ? "回来了" : "持 G 期间 30 秒没回（卡在全局锁上）", (unsigned long long)atomic_load(&g_fp_rc));
    }
    if (getenv("S2A_SCHED_HZ") || getenv("S2A_HZ_CORE")) {   // Horizon 核号 -2 / 时间片轮转（只在 S2A_HZ_CORE 开时跑；期望照 Ryujinx KThread.Initialize / KScheduler.RotateScheduledQueue）
        g_case = "Horizon 核号 -2 / 时间片轮转";
        void s2a_core_leave(void);
        uint64_t ra[31] = {0, (uint64_t)hz_spin, 0, 0, 44, (uint64_t)(int64_t)-2}; svc_thread(0x08, ra);
        uint64_t qa[31] = {0, 0, ra[1]}; svc_thread(0x0E, qa);
        CHECK(qa[0] == 0 && qa[1] == 0 && qa[2] == 1, "-2 建的线程应是缺省核 0、掩码 0x1：实 rc %#llx 核 %llu 掩码 %#llx", (unsigned long long)qa[0], (unsigned long long)qa[1], (unsigned long long)qa[2]);
        { uint64_t c[31] = {ra[1]}; svc_thread(0x16, c); }   // 没启动，关掉
        atomic_store(&g_hz_stop, 0);
        spawn_core_mask(hz_spin, 0, 59, 0, 0x1); s2a_core_leave();
        CHECK(wait_flag(&g_hz_ready, WAIT_LONG), "时间片轮转：空转线程没起来");
        uint64_t t0 = now_ns();
        spawn_core_mask(hz_taker, 0, 59, 0, 0x1); s2a_core_leave();
        int ok = wait_flag(&g_hz_done, WAIT_LONG); uint64_t ms = (now_ns() - t0) / 1000000;
        CHECK(ok && ms <= 200, "时间片轮转：同核同优先级 59 的等核者应在 200 ms 内拿到核（10 ms 一轮）：%s，%llu ms", ok ? "拿到" : "没拿到", (unsigned long long)ms);
        atomic_store(&g_hz_stop, 1);
    }
    {   // 让步退避（S2A_YIELD_BACKOFF）。期望按设计手算：连续让步、中间无别的系统调用、x19–x28 不变，满 2 ms（单调钟）后每次让步都睡。
        //   钉钟（s2a_test_mono_set）：每次让步前把单调钟推进 10 µs，第 k 次（从 0 数）让步时已连续 k × 10 µs → k ≥ 200 的都睡：
        //   400 次 → 恰 200 次；100 次（1 ms）→ 0；x19 每次变 → 0；关 → 0。
        //   10-25 起夹不阻塞的系统调用仍算同一轮（哈迪斯联网监视线程每圈夹服务调用、寄存器不变）：夹 GetThreadId → 同样恰 200 次；
        //   夹一次真阻塞的等待（等未触发的事件 1 µs）→ 每次新一轮、0 次
        //   10-25 前用墙钟跑 20 ms、判「约 72 次（5–80）」：测试线程被宿主切走、nanosleep 超时都改变次数，压满核时偶发红（实 3）
        g_case = "让步退避";
        void s2a_test_yield_bo(int); uint64_t s2a_test_yield_sleeps(void); void s2a_test_mono_set(int64_t);
        uint64_t yev[31] = {0}; svc_thread(0x45, yev); uint32_t yevr = (uint32_t)yev[2];   // a7 用：一直不触发的事件（读端）
        #define YLOOPS(n, step, mut) ({ { uint64_t g_[31] = {0}; g_[19] = 0xdead; svc_thread(0x0B, g_); } uint64_t s0 = s2a_test_yield_sleeps(); \
            for (uint64_t k = 0; k < (n); k++) { s2a_test_mono_set((int64_t)(k * (step))); uint64_t r[31] = {0}; r[19] = 0x1234; r[28] = 0x5678; mut; svc_thread(0x0B, r); } \
            s2a_test_mono_set(-1); s2a_test_yield_sleeps() - s0; })
        #define YLOOP(n, mut) YLOOPS(n, 10000, mut)
        s2a_test_yield_bo(1);
        uint64_t a1 = YLOOP(400, (void)0);
        printf("  让步退避：连续让步 400 次（钉钟每次 +10 µs）退避睡 %llu 次\n", (unsigned long long)a1);
        CHECK(a1 == 200, "连续让步 400 次（每次 +10 µs、寄存器不变、无别的系统调用）：第 200 次起满 2 ms，应睡 200 次，实 %llu", (unsigned long long)a1);
        uint64_t a3 = YLOOP(100, (void)0);
        CHECK(a3 == 0, "切开后连续让步 100 次（1 ms，不到 2 ms）：不应睡，实 %llu", (unsigned long long)a3);
        // 11-04 能耗线（卡比 探索发现 3.33 亿次让步、退避 0 次）：寄存器变了但同一调用点、两次之间 ≤ 20 µs 仍是同一轮轮询 → 恰 200；
        //   两次之间 30 µs（> 20 µs，循环在干活）→ 每次新一轮、0（改前判据：寄存器一变就新一轮，两条都 0）
        uint64_t a4 = YLOOP(400, r[19] = k);
        CHECK(a4 == 200, "x19 每次都变、同一调用点、每次 +10 µs（≤ 20 µs）：仍是轮询，应睡 200 次，实 %llu", (unsigned long long)a4);
        uint64_t a4b = YLOOPS(400, 30000, r[19] = k);
        CHECK(a4b == 0, "x19 每次都变、每次 +30 µs（> 20 µs，循环在干活）：不应睡，实 %llu", (unsigned long long)a4b);
        uint64_t a5 = YLOOP(400, { uint64_t g[31] = {0}; svc_thread(0x25, g); });
        CHECK(a5 == 200, "让步之间夹不阻塞的系统调用（GetThreadId）、寄存器不变：仍是同一轮轮询，应睡 200 次，实 %llu", (unsigned long long)a5);
        uint64_t a7 = YLOOP(400, { uint64_t w[31] = {0}; w[1] = (uint64_t)&yevr; w[2] = 1; w[3] = 1000; svc_thread(0x18, w); });
        CHECK(a7 == 0, "让步之间真阻塞过（等未触发的事件 1 µs）：每次新一轮，不应睡，实 %llu", (unsigned long long)a7);
        // 短睡按让出（10-29，照 the reference port）：SleepThread(100 ns) 与 SleepThread(0) 同走退避 → 400 次寄存器不变、每次 +10 µs 恰睡 200 次，短睡计数 +400；
        //   SleepThread(20 µs) 是真睡，短睡计数不变
        uint64_t s2a_test_sleep_short(void);
        uint64_t sh0 = s2a_test_sleep_short(); uint64_t a8 = YLOOP(400, r[0] = 100); uint64_t sh1 = s2a_test_sleep_short() - sh0;
        { uint64_t g[31] = {0}; g[0] = 20000; svc_thread(0x0B, g); }
        uint64_t sh2 = s2a_test_sleep_short() - sh0 - sh1;
        CHECK(a8 == 200 && sh1 == 400 && sh2 == 0, "短睡按让出：SleepThread(100 ns)×400 退避睡 %llu（应 200）、短睡计 %llu（应 400）；SleepThread(20 µs) 短睡计 %llu（应 0）",
              (unsigned long long)a8, (unsigned long long)sh1, (unsigned long long)sh2);
        // 短睡轮询退避：SleepThread(50 µs)×400、寄存器不变、钉钟每次 +10 µs → 第 200 次起满 2 ms，run/8 ≥ 250 µs > 50 µs，恰拉长 200 次；
        //   x19 每次变 → 仍 200（判轮不看寄存器：丝之歌每圈寄存器都变）；短睡之间夹 GetThreadId → 每次新一轮 0；SleepThread(2 ms)（≥ 1 ms 不算短睡）→ 0
        uint64_t s2a_test_sleep_bo(void);
        uint64_t q0 = s2a_test_sleep_bo(); YLOOP(400, r[0] = 50000); uint64_t q1 = s2a_test_sleep_bo() - q0;
        {   // 被拉长的那次睡走可合并定时器（S2A_SLEEP_LEEWAY 缺省开）：第 399 次已连等 3.99 ms → run/8 = 498.75 µs，封顶 250 µs，余量 = 一半 125 µs
            struct kevent64_s lev; s2a_test_kq_timer(&lev);
            CHECK((lev.fflags & NOTE_LEEWAY) && !(lev.fflags & NOTE_CRITICAL) && lev.ext[1] == 125000, "短睡退避的睡：可合并定时器 fflags %#x（应含 NOTE_LEEWAY、不含 NOTE_CRITICAL）余量 %llu ns（应 125000）",
                  lev.fflags, (unsigned long long)lev.ext[1]); }
        q0 = s2a_test_sleep_bo(); YLOOP(400, { r[0] = 50000; r[19] = k; }); uint64_t q4 = s2a_test_sleep_bo() - q0;
        q0 = s2a_test_sleep_bo(); YLOOP(400, { r[0] = 50000; uint64_t g[31] = {0}; svc_thread(0x25, g); }); uint64_t q2 = s2a_test_sleep_bo() - q0;
        q0 = s2a_test_sleep_bo(); YLOOP(250, r[0] = 2000000); uint64_t q3 = s2a_test_sleep_bo() - q0;
        CHECK(q1 == 200 && q4 == 200 && q2 == 0 && q3 == 0, "短睡轮询退避：50 µs×400 拉长 %llu（应 200）/ x19 每次变 %llu（应 200）/ 夹系统调用 %llu（应 0）/ 2 ms×250 %llu（应 0）",
              (unsigned long long)q1, (unsigned long long)q4, (unsigned long long)q2, (unsigned long long)q3);
        // 有进展即返回（S2A_YIELD_FAST）：新一轮的让步直接回、不进 sched_yield。期望手算（YLOOP 开头切开的那次让步与上一轮寄存器不同，也算 1 次新一轮）：
        //   x19 每次变（同一调用点、每次 +10 µs）→ 11-04 起与寄存器不变相同：切开 1 + 第 1 次 → 2、睡 200；寄存器不变 → 2、睡 200；开关关 → 0
        void s2a_test_yield_fast(int); uint64_t s2a_test_yield_fastret(void);
        s2a_test_yield_fast(1);
        uint64_t f0 = s2a_test_yield_fastret(); uint64_t b1 = YLOOP(400, r[19] = k); uint64_t f1 = s2a_test_yield_fastret() - f0;
        f0 = s2a_test_yield_fastret(); uint64_t b2 = YLOOP(400, (void)0); uint64_t f2 = s2a_test_yield_fastret() - f0;
        s2a_test_yield_fast(0);
        f0 = s2a_test_yield_fastret(); YLOOP(400, r[19] = k); uint64_t f3 = s2a_test_yield_fastret() - f0;
        CHECK(f1 == 2 && b1 == 200 && f2 == 2 && b2 == 200 && f3 == 0, "有进展即返回：寄存器每次变（同一调用点、≤ 20 µs，11-04 起算同一轮）直接返回 %llu（应 2）睡 %llu（应 200）/ 不变 直接返回 %llu（应 2）睡 %llu（应 200）/ 关 直接返回 %llu（应 0）",
              (unsigned long long)f1, (unsigned long long)b1, (unsigned long long)f2, (unsigned long long)b2, (unsigned long long)f3);
        s2a_test_yield_bo(0);
        uint64_t a6 = YLOOP(400, (void)0);
        CHECK(a6 == 0, "开关关：不应睡，实 %llu", (unsigned long long)a6);
        #undef YLOOP
        #undef YLOOPS
    }
    {   // 自旋闸（S2A_SPIN）。期望按设计手算：连续过闸、中间无系统调用、两次过闸间隔 ≤ 20 µs（单调钟），满 2 ms 后每圈都睡。
        //   钉钟：第 k 次过闸时单调钟 = k × 步长。步长 10 µs、400 次 → k ≥ 200 的睡，恰 200 次；步长 50 µs（> 20 µs：游戏代码在干活）→ 每次新一轮、0；
        //   过闸之间夹系统调用 → 0；关 → 0。10-25 前用墙钟跑 20 ms：测试线程被切走 > 20 µs 即被判成新一轮，压满核时偶发红（实 0 / 2 / 3）
        g_case = "自旋闸";
        void s2a_spin_wait(uint64_t); void s2a_test_spin(int); uint64_t s2a_test_spin_sleeps(void); void s2a_test_mono_set(int64_t);
        #define SLOOP(n, step, mid) ({ { uint64_t g_[31] = {0}; svc_thread(0x25, g_); } uint64_t s0 = s2a_test_spin_sleeps(); \
            for (uint64_t k = 0; k < (n); k++) { s2a_test_mono_set((int64_t)(k * (step))); s2a_spin_wait(0x100); mid; } s2a_test_mono_set(-1); s2a_test_spin_sleeps() - s0; })
        s2a_test_spin(1);
        uint64_t b1 = SLOOP(400, 10000, (void)0);
        printf("  自旋闸：连续过闸 400 次（钉钟每次 +10 µs）睡 %llu 次\n", (unsigned long long)b1);
        CHECK(b1 == 200, "连续过闸 400 次（每次 +10 µs、无系统调用）：第 200 次起满 2 ms，应睡 200 次，实 %llu", (unsigned long long)b1);
        uint64_t b2 = SLOOP(400, 50000, (void)0);
        CHECK(b2 == 0, "两次过闸间隔 50 µs（游戏代码在干活）：不应睡，实 %llu", (unsigned long long)b2);
        uint64_t b3 = SLOOP(400, 10000, { uint64_t g[31] = {0}; svc_thread(0x25, g); });
        CHECK(b3 == 0, "过闸之间夹系统调用：不应睡，实 %llu", (unsigned long long)b3);
        s2a_test_spin(0);
        uint64_t b4 = SLOOP(400, 10000, (void)0);
        CHECK(b4 == 0, "开关关：不应睡，实 %llu", (unsigned long long)b4);
        #undef SLOOP
    }
    {   // 读时钟闸（S2A_TICK_WAIT）。期望按设计手算：同一调用点连续读时钟、中间无系统调用、两次读间隔 ≤ 20 µs（钉钟），满 2 ms 后每次都睡。
        //   步长 10 µs、400 次 → k ≥ 200 的睡，恰 200 次；两个调用点交替（给任务计时的起止两处）→ 每次换点、0；步长 50 µs → 0；夹系统调用 → 0；关 → 0
        g_case = "读时钟闸";
        void s2a_tick_wait(uint64_t); void s2a_test_tick_wait(int); uint64_t s2a_test_tick_sleeps(void); void s2a_test_mono_set(int64_t);
        #define TLOOP(n, step, key, mid) ({ { uint64_t g_[31] = {0}; svc_thread(0x25, g_); } uint64_t s0 = s2a_test_tick_sleeps(); \
            for (uint64_t k = 0; k < (n); k++) { s2a_test_mono_set((int64_t)(k * (step))); s2a_tick_wait(key); mid; } s2a_test_mono_set(-1); s2a_test_tick_sleeps() - s0; })
        s2a_test_tick_wait(1);
        uint64_t c1 = TLOOP(400, 10000, 0x200, (void)0);
        printf("  读时钟闸：同一调用点连续读 400 次（钉钟每次 +10 µs）睡 %llu 次\n", (unsigned long long)c1);
        CHECK(c1 == 200, "同一调用点连续读 400 次（每次 +10 µs、无系统调用）：第 200 次起满 2 ms，应睡 200 次，实 %llu", (unsigned long long)c1);
        uint64_t c2 = TLOOP(400, 10000, 0x200 + (k & 1) * 0x10, (void)0);
        CHECK(c2 == 0, "两个调用点交替读（任务计时起止）：不应睡，实 %llu", (unsigned long long)c2);
        uint64_t c3 = TLOOP(400, 50000, 0x200, (void)0);
        CHECK(c3 == 0, "两次读间隔 50 µs（游戏代码在干活）：不应睡，实 %llu", (unsigned long long)c3);
        uint64_t c4 = TLOOP(400, 10000, 0x200, { uint64_t g[31] = {0}; svc_thread(0x25, g); });
        CHECK(c4 == 0, "两次读之间夹系统调用：不应睡，实 %llu", (unsigned long long)c4);
        s2a_test_tick_wait(0);
        uint64_t c5 = TLOOP(400, 10000, 0x200, (void)0);
        CHECK(c5 == 0, "开关关：不应睡，实 %llu", (unsigned long long)c5);
        #undef TLOOP
    }
    {   // 宿主栈回收（10-24 宿主栈放窗口尾部，自己映射、自己还）：线程退出后，下一次建线程把它汇合并还栈；建一个还一个
        g_case = "宿主栈回收";
        void s2a_core_leave(void);
        int m0 = atomic_load(&g_tm_map);
        spawn(hs_exit, 0, NULL); s2a_core_leave();
        CHECK(wait_flag(&g_hs_done[0], WAIT_LONG), "宿主栈回收：第一条线程没跑到退出前");
        usleep(20000); int u1 = atomic_load(&g_tm_unmap);
        spawn(hs_exit, 1, NULL); s2a_core_leave();
        CHECK(wait_flag(&g_hs_done[1], WAIT_LONG), "宿主栈回收：第二条线程没跑到退出前");
        int m2 = atomic_load(&g_tm_map), u2 = atomic_load(&g_tm_unmap);
        CHECK(m2 - m0 == 2 && u2 - u1 >= 1, "宿主栈回收：建两条线程应映射 2 次栈（得 %d），建第二条时应至少还 1 次（第一条已退出；得 %d）", m2 - m0, u2 - u1);
        printf("  宿主栈回收：映射 %d 次，建第二条时还 %d 次\n", m2 - m0, u2 - u1);
    }
    {   // 资源上限与参数校验（期望照 Ryujinx：应用线程 608、传输内存 128、句柄表 1024；返回码 InvalidCpuCore 0xE201、InvalidPriority 0xE001、
        //   ResLimitExceeded 0x10801、HandleTableFull 0xD201、InvalidAddress 0xCC01、InvalidSize 0xCA01、InvalidMemState 0xD401、InvalidPermission 0xD801）
        g_case = "资源上限";
        void s2a_thread_set_caps(uint64_t, uint64_t); int s2a_thread_alive_count(void); int s2a_test_handle_used(void);
        s2a_thread_set_caps(0x7, ((1ull << 32) - 1) << 28);   // NPDM 常见：核 0–2、优先级 28–59
        #define SV(n, ...) (r_ = (uint64_t[31]){__VA_ARGS__}, svc_thread((n), r_), r_)
        uint64_t *r_;
        CHECK(SV(0x08, 0, (uint64_t)reader, 0, 0, 44, 3)[0] == 0xE201, "核 3 不在 NPDM 掩码 0x7：应回 InvalidCpuCore");
        CHECK(SV(0x08, 0, (uint64_t)reader, 0, 0, 44, 4)[0] == 0xE201, "核 4（≥4）：应回 InvalidCpuCore");
        {   // Switch 2（硬件 8 核，NPDM 掩码 0x3f）：核 4 / 5 可建（不回 InvalidCpuCore）、核 6 不在掩码、核 8 越界；GetCurrentProcessorNumber 类读数不受影响。测完复原 4 核 / 掩码 0x7
            void s2a_thread_set_hw_cores(int); s2a_thread_set_hw_cores(8); s2a_thread_set_caps(0x3f, ((1ull << 32) - 1) << 28);
            uint64_t e6 = SV(0x08, 0, (uint64_t)reader, 0, 0, 44, 6)[0], e8 = SV(0x08, 0, (uint64_t)reader, 0, 0, 44, 8)[0];
            uint64_t m5 = SV(0x0F, 0xFFFF8000, 5, 0x30)[0], m6 = SV(0x0F, 0xFFFF8000, 6, 0x40)[0];
            s2a_thread_set_hw_cores(4); s2a_thread_set_caps(0x7, ((1ull << 32) - 1) << 28);
            CHECK(e6 == 0xE201 && e8 == 0xE201 && m5 == 0 && m6 == 0xE201, "Switch 2 8 核 / 掩码 0x3f：建线程核 6 / 8 回 0xE201、理想核 5 掩码 0x30 成功、核 6 掩码超出回 0xE201：实 %#llx %#llx %#llx %#llx",
                  (unsigned long long)e6, (unsigned long long)e8, (unsigned long long)m5, (unsigned long long)m6);
            SV(0x0F, 0xFFFF8000, 0, 0x1); }
        CHECK(SV(0x08, 0, (uint64_t)reader, 0, 0, 20, 0)[0] == 0xE001, "优先级 20 不在 28–59：应回 InvalidPriority");
        CHECK(SV(0x08, 0, (uint64_t)reader, 0, 0, 64, 0)[0] == 0xE001, "优先级 64（≥64）：应回 InvalidPriority");
        {   // 族 27（缺口扫描 11-01）：SetThreadPriority 照 Horizon 先核优先级（同 0x08 的 NPDM 掩码），非法回 InvalidPriority 且优先级不变
            uint64_t p0 = SV(0x0C, 0, g_wtag)[1];
            uint64_t e20 = SV(0x0D, g_wtag, 20)[0], q20 = SV(0x0C, 0, g_wtag)[1], e64 = SV(0x0D, g_wtag, 64)[0], q64 = SV(0x0C, 0, g_wtag)[1];
            uint64_t e40 = SV(0x0D, g_wtag, 40)[0], q40 = SV(0x0C, 0, g_wtag)[1], ebh = SV(0x0D, 0x12345, 40)[0];
            SV(0x0D, g_wtag, p0);
            CHECK(e20 == 0xE001 && q20 == p0 && e64 == 0xE001 && q64 == p0 && e40 == 0 && q40 == 40 && ebh == 0xE401,
                  "SetThreadPriority：期望 20（不在 28–59）0xe001 不变、64 0xe001 不变、40 成功读回 40、坏句柄 0xe401 → 实 %#llx/%llu、%#llx/%llu、%#llx/%llu、%#llx",
                  (unsigned long long)e20, (unsigned long long)q20, (unsigned long long)e64, (unsigned long long)q64, (unsigned long long)e40, (unsigned long long)q40, (unsigned long long)ebh);
        }
        int before = s2a_thread_alive_count(), made = 0; uint64_t rc = 0;
        for (int i = 0; i < 700; i++) { uint64_t *r = SV(0x08, 0, (uint64_t)reader, 0, 0, 44, (uint64_t)-2); if (r[0]) { rc = r[0]; break; } made++; }   // 只建不启动
        CHECK(made == 608 - before && rc == 0x10801, "线程上限：已有 %d 条，应再建成 %d 条后回 0x10801；实建成 %d 条、回 %#llx", before, 608 - before, made, (unsigned long long)rc);
        CHECK(SV(0x08, 0, (uint64_t)reader, 0, 0, 20, 0)[0] == 0xE001, "到线程上限时优先级非法：应先回 InvalidPriority（校验在上限前）");
        // 传输内存参数校验（在上限检查之前）
        static uint8_t tbuf[0x2000] __attribute__((aligned(0x1000)));
        uint64_t ta = (uint64_t)tbuf;
        CHECK(SV(0x15, 0, ta + 8, 0x1000, 3)[0] == 0xCC01, "传输内存地址不按 4 KB 对齐：应回 InvalidAddress");
        CHECK(SV(0x15, 0, ta, 0, 3)[0] == 0xCA01, "传输内存大小 0：应回 InvalidSize");
        CHECK(SV(0x15, 0, ta, 0x800, 3)[0] == 0xCA01, "传输内存大小不按 4 KB 对齐：应回 InvalidSize");
        CHECK(SV(0x15, 0, ~0xfffull, 0x2000, 3)[0] == 0xD401, "传输内存地址 + 大小回绕：应回 InvalidMemState");
        CHECK(SV(0x15, 0, ta, 0x1000, 2)[0] == 0xD801, "传输内存权限只写（2）：应回 InvalidPermission");
        CHECK(SV(0x15, 0, ta, 0x1000, 4)[0] == 0xD801, "传输内存权限 4：应回 InvalidPermission");
        {   // 审计 P2-11：建立时借出源（参数原样交 svc_mem）、借出失败原样回且不发句柄、关句柄还回同一段。期望从调用参数直接写
            int u0 = s2a_test_handle_used(), b0 = g_tb_n, r0 = g_tu_n;
            g_tb_rc = 0xD401; uint64_t fe = SV(0x15, 0, ta, 0x2000, 1)[0]; g_tb_rc = 0;
            CHECK(fe == 0xD401 && g_tb_n == b0 + 1 && s2a_test_handle_used() == u0 && g_tu_n == r0,
                  "P2-11 源借不出：期望 回 0xd401、借 1 次、句柄不增、不还 → 实 %#llx、借 %d 次、句柄 %+d、还 %d 次", (unsigned long long)fe, g_tb_n - b0, s2a_test_handle_used() - u0, g_tu_n - r0);
            uint64_t *r = SV(0x15, 0, ta, 0x2000, 1); uint64_t ok = r[0]; uint32_t th = (uint32_t)r[1];
            CHECK(ok == 0 && g_tb_n == b0 + 2 && g_tb_a == ta && g_tb_s == 0x2000 && g_tb_p == 1 && g_tu_n == r0,
                  "P2-11 建立：期望 回 0、借出 [ta, +0x2000) 权限 1、未还 → 实 %#llx、借 %d 次 [ta%+lld, +%#llx) 权限 %u、还 %d 次",
                  (unsigned long long)ok, g_tb_n - b0, (long long)(g_tb_a - ta), (unsigned long long)g_tb_s, g_tb_p, g_tu_n - r0);
            uint64_t ce = ok ? 1 : SV(0x16, th)[0];
            CHECK(ce == 0 && g_tu_n == r0 + 1 && g_tu_a == ta && g_tu_s == 0x2000 && s2a_test_handle_used() == u0,
                  "P2-11 关句柄：期望 回 0、还回 [ta, +0x2000) 1 次、句柄回原数 → 实 %#llx、还 %d 次 [ta%+lld, +%#llx)、句柄 %+d",
                  (unsigned long long)ce, g_tu_n - r0, (long long)(g_tu_a - ta), (unsigned long long)g_tu_s, s2a_test_handle_used() - u0);
        }
        {   // 借出属性：代码内存对象（原生引用对象子号 2）最后一个句柄关掉交 s2a_mem_codemem_free（还回源），资源上限（子号 1）照旧直接释放、不交
            int c0 = g_cf_n; int *cm = calloc(1, 64), *rl = calloc(1, 64);
            uint32_t hc = s2a_native_handle_new(2, cm), hr = s2a_native_handle_new(1, rl);
            uint64_t e1 = SV(0x16, hc)[0]; int n1 = g_cf_n - c0; void *o1 = g_cf_o; uint64_t e2 = SV(0x16, hr)[0];
            CHECK(hc && hr && e1 == 0 && n1 == 1 && o1 == cm && e2 == 0 && g_cf_n - c0 == 1,
                  "借出属性 关代码内存句柄：期望 回 0、交释放 1 次（对象本身）、关资源上限不交 → 实 %#llx、%d 次 %s、%#llx、共 %d 次",
                  (unsigned long long)e1, n1, o1 == cm ? "同对象" : "别的对象", (unsigned long long)e2, g_cf_n - c0);
            free(cm);
        }
        // 句柄表：一直建事件到建不出；剩一格时补一个传输内存，正好 1024 格
        static uint32_t ev[2][1024]; int ne = 0;
        for (; ne < 1024; ne++) { uint64_t *r = SV(0x45, 0); if (r[0]) break; ev[0][ne] = (uint32_t)r[1]; ev[1][ne] = (uint32_t)r[2]; }
        uint32_t extra = 0; if (s2a_test_handle_used() == 1023) { uint64_t *r = SV(0x15, 0, ta, 0x1000, 3); if (!r[0]) extra = (uint32_t)r[1]; }
        CHECK(s2a_test_handle_used() == 1024, "句柄表：建到建不出时在用应 1024 格，实 %d", s2a_test_handle_used());
        CHECK(SV(0x15, 0, ta, 0x1000, 3)[0] == 0xD201, "句柄表满：再建传输内存应回 HandleTableFull");
        CHECK(SV(0x16, ev[0][0])[0] == 0 && SV(0x16, ev[1][0])[0] == 0 && s2a_test_handle_used() == 1022, "关一个事件两端：在用应 1022");
        CHECK(SV(0x45, 0)[0] == 0 && SV(0x45, 0)[0] == 0xD201 && s2a_test_handle_used() == 1024, "关后再建：一个事件成、下一个回 HandleTableFull，在用回到 1024");
        // 传输内存上限：腾出句柄（关掉 200 个事件），一直建传输内存到建不出
        for (int i = 1; i <= 200; i++) { SV(0x16, ev[0][i]); SV(0x16, ev[1][i]); }
        int nt = extra ? 1 : 0; rc = 0;
        for (int i = 0; i < 300; i++) { uint64_t *r = SV(0x15, 0, ta, 0x1000, 3); if (r[0]) { rc = r[0]; break; } nt++; }
        CHECK(nt == 128 && rc == 0x10801, "传输内存上限：应同时存在 128 个后回 0x10801；实 %d 个、回 %#llx", nt, (unsigned long long)rc);
        #undef SV
    }
    {   // 3 号核游戏线程的 QoS（11-12 一款 Switch 2 游戏 0400486024DEC000 长帧）：性能核 ≥ 4 缺省与其余游戏线程同档、不足 4 个缺省 UTILITY；
        //   S2A_SYSCORE_QOS=same / utility 两头强制。期望按「本机性能核数」手写分支（hw.perflevel0.physicalcpu）
        g_case = "3 号核线程 QoS";
        qos_class_t s2a_test_guest_qos(int, uint64_t);
        unsetenv("S2A_SYSCORE_QOS"); unsetenv("S2A_GUEST_QOS");
        int pc = 0; size_t pcs = sizeof pc; if (sysctlbyname("hw.perflevel0.physicalcpu", &pc, &pcs, NULL, 0) || pc <= 0) { pcs = sizeof pc; if (sysctlbyname("hw.physicalcpu", &pc, &pcs, NULL, 0) || pc <= 0) pc = 8; }
        qos_class_t other = s2a_test_guest_qos(0, 0x7), core3 = s2a_test_guest_qos(0, 1ull << 3);
        if (pc >= 4) CHECK(core3 == other && core3 != QOS_CLASS_UTILITY, "缺省（性能核 %d ≥ 4）：3 号核线程应与其余游戏线程同档（%#x），实 %#x", pc, other, core3);
        else CHECK(core3 == QOS_CLASS_UTILITY, "缺省（性能核 %d < 4）：3 号核线程应为 UTILITY，实 %#x", pc, core3);
        setenv("S2A_SYSCORE_QOS", "same", 1);
        CHECK(s2a_test_guest_qos(0, 1ull << 3) == other, "S2A_SYSCORE_QOS=same：3 号核线程应与其余游戏线程同档（%#x），实 %#x", other, s2a_test_guest_qos(0, 1ull << 3));
        setenv("S2A_SYSCORE_QOS", "utility", 1);
        CHECK(s2a_test_guest_qos(0, 1ull << 3) == QOS_CLASS_UTILITY, "S2A_SYSCORE_QOS=utility：3 号核线程应为 UTILITY，实 %#x", s2a_test_guest_qos(0, 1ull << 3));
        CHECK(s2a_test_guest_qos(0, 0x7) == other, "S2A_SYSCORE_QOS=utility 不该动别的核掩码（%#x），实 %#x", other, s2a_test_guest_qos(0, 0x7));
        unsetenv("S2A_SYSCORE_QOS");
    }
    {   // SleepThread 按时醒（10-25 数码宝贝断续：utility QoS 下 nanosleep 睡 31 ms 平均多睡 103 ms）：本线程降 utility，睡 30 ms，
        //   判挂的定时器（见 timer_ok）；实测多睡只报数。放最后：本线程 QoS 改了不影响前面各例
        g_case = "SleepThread 按时醒";
        qos_class_t q0 = QOS_CLASS_UNSPECIFIED; int rp0 = 0; pthread_get_qos_class_np(pthread_self(), &q0, &rp0);
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        struct kevent64_s ev0; int n0 = s2a_test_kq_timer(&ev0);
        int64_t ov = sleep30(NULL);
        CHECK(timer_ok("SleepThread 30 ms", n0, 30000000) && ov >= 0, "utility 下睡 30 ms：定时器不对（见上一行）或早醒（多睡 %.3f ms）", ov / 1e6);
        printf("  SleepThread 按时醒：实测多睡 %.3f ms（只报数，含调度排队）\n", ov / 1e6);
        // 带超时的等待同理（到点就超时）：WaitSynchronization 等不触发的事件、WaitForAddress（等于则等）各 30 ms，应回超时、挂同样的定时器
        g_case = "带超时等待按时醒";
        uint32_t hr9; uint64_t ev9 = s2a_event_create(&hr9, NULL); uint32_t hs9[1] = {hr9};
        static _Atomic int32_t g_ow = 7;
        for (int kind = 0; kind < 2; kind++) {
            struct wait30 w = {kind, hs9, &g_ow, 0}; int m0 = s2a_test_kq_timer(&ev0);
            int64_t o9 = wait30(&w);
            const char *nm = kind ? "WaitForAddress" : "WaitSynchronization";
            CHECK(!w.bad_rc, "utility 下 %s 超时 30 ms：应回 0xEA01（%d 次不是）", nm, w.bad_rc);
            CHECK(timer_ok(nm, m0, 30000000) && o9 >= 0, "utility 下 %s 超时 30 ms：定时器不对（见上一行）或早醒（多睡 %.3f ms）", nm, o9 / 1e6);
            printf("  %s 超时按时醒：实测多睡 %.3f ms（只报数）\n", nm, o9 / 1e6);
        }
        // 超时前被唤醒（唤醒方走 kqueue 用户事件）：宿主线程等本线程真阻塞在 WaitSynchronization 里再触发事件，应回 0（被唤醒；超时回 0xEA01）。
        //   只按返回值判：本线程此时是 utility QoS，忙机排不上 CPU，醒来要多久是调度排队、不是我们的代码（10-30 整组并行 155.8 ms 假红）——耗时只报数；
        //   上限 10 秒只是兜底，不是判据
        { pthread_t sg; struct timespec a, b; struct ev_after_block ab = {ev9, s2a_thread_current()}; clock_gettime(CLOCK_MONOTONIC, &a);
          pthread_create(&sg, NULL, ev_signal_after_block, &ab);
          uint64_t r[31] = {0, (uint64_t)hs9, 1, 10000000000ull}; svc_thread(0x18, r); clock_gettime(CLOCK_MONOTONIC, &b); pthread_join(sg, NULL);
          double ms = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 1e6;
          CHECK(r[0] == 0, "超时前被唤醒：应回 0（阻塞中被事件唤醒），实 %#llx、%.1f ms", (unsigned long long)r[0], ms);
          printf("  超时前被唤醒：%.1f ms 返回（只报数，含调度排队）\n", ms); }
        s2a_event_release(ev9);
        if (q0 != QOS_CLASS_UNSPECIFIED) pthread_set_qos_class_self_np(q0, rp0);
    }
    printf("%s t_thread：%d 处不符（争锁 %d×%d 次 %.0f ms 重叠 0；反例不加锁重叠 %d 次；按时醒定时器判过 %d / 3 项）\n", bad ? "✗" : "✓", bad, NTH, NIT, ms5, ov0, g_ontime_ok);
    fflush(stdout); _exit(bad != 0);
}
