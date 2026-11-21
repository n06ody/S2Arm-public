// Horizon 内核的线程与同步系统调用，原生实现（旧库 hle_thread.c 改后搬）：每个游戏线程 = 一个宿主 pthread，跑在 SDK 给的游戏栈上。
// SDK 的 nn::os 原样运行，只把它下面的「内核」换成宿主：
//   线程   0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D 优先级 0x0E/0x0F 核掩码
//          0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization 0x32 SetThreadActivity 0x33 GetThreadContext3 0x36
//   锁     0x1A ArbitrateLock 0x1B ArbitrateUnlock（标签字 = 持有者句柄 | 0x40000000 有等待者）
//   条件   0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   地址   0x34 WaitForAddress 0x35 SignalToAddress
//   事件   0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
//   内存对象 0x15 CreateTransferMemory（只记 {地址, 大小, 权限}，游戏地址 = 宿主地址）
// 语义按公开 Horizon 内核行为（Atmosphère mesosphère 的 KConditionVariable / KAddressArbiter / KSynchronization）。
// 全进程唯一一张句柄表也在这里（docs/接口_原生层.md §4.2）：C# 的会话等对象只存 cookie。所有等待挂在一把全局锁 G 上，每线程一个条件变量。
// 旧库删掉的：虚拟核锁（缺省关、无实证）、确定时钟 / 时间倍率、停世界快照、等待统计、信号量跟踪、逐线程指令计数（测试 / 诊断专用）。
#include "native.h"
#include "../../platform/plat.h"
#include <sys/mman.h>
#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread/qos.h>
#include "s2a_qos.h"   // 全进程线程调度档一张表
#include <os/workgroup.h>
#endif
#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include "../../android/native/runtime/fault/fault.h"
// OHOS musl signal.h already provides these AArch64 context records.
#if !defined(__OHOS__)
#include <asm/sigcontext.h>
#endif
#include <linux/futex.h>
#include <sys/syscall.h>
#endif
#include <signal.h>
#include <sys/ucontext.h>
#if defined(__APPLE__)
#include <sys/event.h>
#endif

static uint64_t thread_clock(void) {
#if defined(__APPLE__)
    return mach_absolute_time();
#else
    return plat_now_ns();
#endif
}
static uint64_t thread_clock_ns(uint64_t ticks) {
#if defined(__APPLE__)
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    return ticks * tb.numer / tb.denom;
#else
    return ticks;
#endif
}

#define R_TIMEOUT     0xEA01u   // 内核模块 1，描述 117
#define R_CANCELLED   0xEC01u   // 118
#define R_BAD_HANDLE  0xE401u   // 114
#define R_BAD_STATE   0xFA01u   // 125
#define R_BAD_THREAD  0xF401u   // 122 InvalidThread
#define R_OUT_OF_RES  0xD201u   // 105 OutOfHandles
#define WAITERS_BIT   0x40000000u

typedef struct event { int signaled; int refs; uint32_t did; uint64_t trace_id; } event_t;   // refs：句柄数 + C# 持有（s2a_event_create 到 release）；did：确定性档的对象号
typedef struct { uint64_t addr, size; uint32_t perm; } tmem_t;
typedef struct shmem { uint8_t *buf; uint64_t size; int refs, owned; struct shmem *next; } shmem_t;
static shmem_t *g_shmem;
static _Atomic int g_session_stop;
static int (*g_stop_boundary)(void);
#if defined(__APPLE__)
static _Atomic int g_live_threads, g_join_verified;
static uint64_t (*g_stop_sp)(void);
static void stop_signal_init(void);
#endif
void s2a_thread_stop_point(void);

// G 内采样的唤醒目标身份；不跨锁保留 thr 指针，id 防句柄代际回绕后误认新线程。
typedef struct { uint32_t handle; uint64_t id, nsvc; } peer_wait_t;

struct snap_mach_ctx { uint64_t regs[34]; uint8_t neon[32 * 16]; uint32_t fpsr, fpcr; };   // 即时存档：Mach 挂住的线程现场（x0–x28、fp、lr、sp、pc、cpsr；向量寄存器；浮点状态）
typedef struct thr {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top; int prio, core; uint64_t mask;
    void *hstk;                        // 宿主栈（窗口尾部取，含 16 KB 保护页；见 hstk_reap）
    // S2A_CORES：此刻持有的虚拟核号 + 1（0 = 未持有）；别的线程会在 g_cmu 下把它挪到另一核（空闲核迁移），故原子
    _Atomic int held_core;
    // S2A_CORES：此刻在 core_acquire 里等核（现场导出用）
    _Atomic int wcore;
    int started, exited, cancel, closed;
    int pins;                          // 持 G 改：系统调用查到对象后、放开 G 用完前钉住，thr_free 见钉不放（暂停 / 取上下文）
    pthread_t pt; pthread_cond_t cv;
#if defined(__APPLE__)
    mach_port_t mport;
    int kqw, kqfd; // Apple timed waits use this thread-owned kqueue.
    // 即时存档：冻结时停在游戏原代码里（忙等，不进门）的线程被 Mach 挂住，记下完整寄存器现场（恢复时装回、从原指令接着跑）
    int snap_run; struct snap_mach_ctx *snap_mc;   // 即时存档：Mach 挂住时的现场（用时才分配；开关关着恒空）
    // 停稳 ack（同安卓同名字段，审计 P1-49）：持 G 写。目标自己进 pause_park 时置；pwait = 暂停方代置（目标在系统调用里 / Mach 挂在游戏代码里），恢复方收回
    _Atomic int paused_ack; int pwait;
#else
    _Atomic unsigned vm_ack; int vm_wait_safe, vm_admission_wait;
    _Atomic int paused_ack; ucontext_t paused_context;
    int pwait;                         // paused_ack set by thr_pause on the target's behalf: it was blocked in an SVC wait (under G)
    _Atomic unsigned pause_sig_seen, pause_sig_native; // 信号安全诊断计数
#endif
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres;   // 当前等待：wkind 0 无 1 锁 2 条件 3 地址 4 同步对象
    // 0x34 的条件原样留档（现场导出直接写「等的是小于 / 相等、比较值几、超时多少」）：只看「地址处值」定不了
    //   是「进门时条件就成立」还是「挂上后值被改过」，而这两者一个是正常等信号、一个是丢唤醒，处置完全不同
    int wtype; int32_t wval; int64_t wtmo;
    // S2A_WAIT_TRACE=1：等待期间本线程等的键 / 地址被发过几次信号（逐线程计数，不用键表，键表 1024 格直映会撞）
    unsigned nsig;
    // 等地址（0x34）期间「最近一次有谁对这个地址发过信号」：**缺省就记**，不受 S2A_WAIT_TRACE 控制。
    //   LightEvent / Mutex 的 state 字里没有 owner，那是 SDK 的结构、改不了；但 0x35 是我们在实现，入口能记下谁调的——
    //   「有没有人来过、是谁、多久前」只能从这一侧回答，光看 state 永远答不了（10-30 botw 卡死线）
    uint64_t sig_id, sig_tick;
    int yielded;                       // SleepThread(0/-1/-2) 让出：重取核时让同优先级、登记更早的先拿（Switch yield 同优先级轮转）
    uint64_t wseq;                     // 登记等待的先后：同优先级先到先醒（同 Horizon KThread 等待树）
    struct thr *next, *all;            // 等待链 / 全部线程链
    int last_svc; uint64_t last_lr, last_fp, nsvc;
    // 交给 C# 服务层处理的系统调用（s2a_svc_ctx_fill / s2a_svc_cs_returned）：号、会话句柄、开始时刻、回来次数。
    //   即时存档：同步 IPC 在 C# 里阻塞（等网络、等别的服务）时线程不会回门，冻结把它当「入口」安全点（恢复后重发这次请求），见 snap_cs_blocked
    int cs_svc; uint32_t cs_h; uint64_t cs_t0; _Atomic uint32_t cs_ret;
    int cs_seq, snap_cs_seq;   // 正在 C# 里处理的这条请求在服务请求日志里的序号（不进日志的 / 没在处理 < 0）；冻结判齐那一刻另存一份
    int snap_cs; uint32_t snap_cs_ret; uint8_t snap_cs_frame[832], snap_cs_msg[256];   // 冻结判齐那一刻另存的门帧与请求消息（C# 写回复之前）
    // 最近 8 次同步类系统调用（0x1a–0x1d / 0x34 / 0x35）：号、参数、地址处的值（调前 / 调后）、返回码、返回地址。
    //   停帧现场只打「在跑」线程的：同步原语每圈都掉进系统调用的空转（11-14 #35fdbb 166 号 SignalToAddress 11 万次、162 号对空闲锁 ArbitrateLock 2.6 万次）
    //   要看游戏给了什么、我们回了什么，光有最后一个调用号判不了是哪个输入错
    struct { uint64_t a[4], lr; uint32_t num, pre, post, ret; } sv_ring[8]; uint32_t sv_n;
    uint64_t tr_ret_t, tr_cpu_ns, tr_wake_t, tr_waker, tr_lastwake_t; int tr_ret_svc;   // 唤醒追踪（S2A_WAKE_TRACE，见 wt_on）：上次回游戏的时刻与当时的线程 CPU、被唤醒时刻与唤醒方、自己最近一次唤醒别人的时刻
    _Atomic uint64_t nsvc_all;                 // 全部系统调用次数（svc_thread 入口计；nsvc 只计线程类）：让步退避判「两次让步之间有没有别的系统调用」
    uint64_t yrun_blk, yrun_sig, yrun_t0, nblk, y_lr, y_ret;   // Yield runs reset after an actual blocking wait, or changed guest state at another call site / after > SPIN_GAP_NS of guest code (y_lr / y_ret).
    uint64_t sl_lr, sl_seq, sl_t0, sl_ret;
    uint64_t nsl, nsl_bo, sl_bo_ns, sl_up[2]; int is_main;   // sl_up：抽样记的上两层返回地址（帧指针回溯，定游戏哪段代码在轮询）   // 本线程短睡次数、被退避拉长次数、多睡纳秒（收局按线程列，s2a_yield_stat_log）；is_main = 游戏主线程   // 短睡轮询退避：上一次短睡的调用点、当时的 nsvc_all、这一轮起点、上一次醒来时刻
    uint64_t srun_seq, srun_t0, spin_ret;   // 自旋闸：上一次过闸时的 nsvc_all、这一轮等待的起点、上一次出闸的时刻
    uint64_t trun_key, trun_seq, trun_t0, tick_ret;   // 读时钟闸：上一次读时钟的调用点、当时的 nsvc_all、这一轮等待的起点、上一次读完的时刻
    int base_prio;                     // S2A_SCHED_HZ：基础优先级（SetThreadPriority 设的）；prio = 生效优先级（含锁的优先级继承、钉核抬升）
    int pinned, pin_core; uint64_t pin_mask;   // S2A_SCHED_HZ：0x36 禁止抢占——被钉在当前核（钉前的理想核 / 掩码存这里）
    uint64_t wake_t;                   // 诊断：被 wake() 唤醒的时刻（mach 时基），回到游戏前算交接延迟
    uint64_t wait_t0;                  // 诊断：这次开始等待的时刻（mach 时基），线程表打「已等多久」
    _Atomic int in_svc;                // 此刻在系统调用里（含阻塞在等待里）
    _Atomic int pause; int pmach;
    int park_at;                       // 即时存档：最近一次停进 pause_park 的位置（1 系统调用入口、2 出口、3 宿主函数回客体、0 其他）
#if defined(__APPLE__)
    arm_thread_state64_t pst; arm_neon_state64_t pns;
#endif   // SetThreadActivity：在系统调用里的在出入口挂起；在游戏代码里的 Mach 挂起
    uint64_t *svc_regs; uint64_t svc_pc;   // 当前系统调用的门帧与原 svc 指令地址
#if !defined(__APPLE__)   // 读的一端在 android/native/runtime/fault/thread_android.inc（停世界到期的线程明细），苹果不编、不付这份开销
    _Atomic uint64_t sig_pc, sig_lr;   // 诊断：最近一次 SIGURG 落点（宿主区也记）
    _Atomic uint64_t sig_bt[4];          // 诊断：落点往上 4 层帧指针链返回地址（只沿本栈 1 MiB 内的 fp 走）
    _Atomic uint64_t blk_ra;   // 诊断：最外层 DM_BLOCK 的调用点（停世界到期时报出线程在哪段宿主活里）
#endif
    uint64_t tsd;                      // 本宿主线程 TSD 基址：别的线程据此读本线程的 K18 / KTLS / KTLR 槽
    void *tlr;                         // ThreadLocalRegion（窗口 TLS/IO 区，svc_mem.c 分配）
    const uint32_t *wh; int wn;        // WaitSynchronization 在等的句柄数组与个数（现场导出用）
    _Atomic int preempt_req;           // 时钟抢占：宿主区被请求让核（下次 svc_exit 的 core_acquire 处自然让）
    _Atomic int preempting;            // 时钟抢占：已被看门狗发信号、让核未完（看门狗不重复发）
    _Atomic int restoring;             // 时钟抢占（方案 d）：tramp 已让核、等二次信号覆盖 mcontext 回原
    _Atomic int off_guest;             // 放了核在宿主侧干活（IPC 进 C#、写跟踪 touch、抢占 tramp）：调度计数不算「在跑客体」
    _Atomic uint64_t soft_keeps;       // 本线程「回客体时原核还在、免锁」次数（只本线程写；退出时并进 g_soft_keeps，热路径不碰共享计数）
    _Atomic int csoft;                 // 宿主侧可借（S2A_VCORE_SOFT）：0 = 硬持 / 不持；1 = 持着核但在宿主代码里，等核者可直接取走；2 = 已被取走（回客体走取核慢路）
#if defined(__APPLE__)
    _STRUCT_MCONTEXT porig;            // 时钟抢占：被抢点原 mcontext（客体区 sig1 存、二次信号恢复）
    sigset_t pmask;
#endif                    // 时钟抢占：被抢前的信号掩码（sig1 存、二次信号写回 uc_sigmask）
    uint32_t ss_nocore;                // 调度计数：本线程被采到「在跑客体却不持核」的次数（收局按线程列前 5，定位违规来源）
    int ss_last_svc;                   // 同上：最近一次被采到时的 last_svc
    // 宿主侧持核长跑（持核 + 不在系统调用 + 没放核 + PC 不在游戏模块）：核 × 宿主锁 ABBA 的现场读数。
    //   按「连续段」结算：一采即走的短过路只记过路次数，段长 ≥2 个采样周期才判长跑——命中次数是热点，段长才是隐患
    uint32_t hc_run;                   // 本段连续被采到的次数（0 = 不在段里）
    uint32_t hc_host;                  // 本段判定：0 未取过 PC、1 宿主侧（算长跑）、2 客体或 PC 读不到（不算）
    uint32_t hc_probed;                // 本段已取过 PC（每段最多停住它一次，短过路一次都不停）
    uint64_t hc_t0, hc_tlast;          // 段首 / 末次采样命中时刻（thread_clock）
    uint64_t hc_ret; int hc_svc;       // 取 PC 那一刻的返回地址与 last_svc（收局按这对聚合）
    _Atomic int percore;               // 调过 GetCurrentProcessorNumber（0x10）：按需核锁的参与者（见 core_part）
    peer_wait_t peer;                 // 同核高优先级唤醒让先：G 内保存的目标与进度快照（见 peer_yield）
    uint64_t trace_wait, trace_start, trace_block_start; // 显式开启的等待诊断，持 G 读写
#if !defined(__APPLE__)
    uint64_t trace_pause; unsigned trace_pause_conditions; // G 内请求身份
#endif
} thr_t;
static _Atomic uint64_t g_soft_keeps, g_soft_steals;   // 宿主侧可借：退出线程并进来的「免锁拿回」次数 / 被等核者取走的次数（活着的线程各自记 soft_keeps）

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
#ifdef S2A_UNIT_TEST
void s2a_test_glock_hold(int on) { if (on) pthread_mutex_lock(&G); else pthread_mutex_unlock(&G); }   // t_thread：ArbitrateLock 快路径（持着 G 也能回）
#endif
// 诊断：全局锁 G 的争用（S2A_DIAG_STAMP 设了才计；没设直接加锁，不多一次原子操作）。trylock 拿到不计时；拿不到就计等了多久，按量级计数
static _Atomic uint64_t g_gl_try, g_gl_wait, g_gl_b[7], g_gl_max;
static int lw_on(void) { static int v = -1; if (v < 0) v = getenv("S2A_DIAG_STAMP") != NULL; return v; }   // 诊断开关：没设不计数
static void glock(void) {
    if (!lw_on()) { pthread_mutex_lock(&G); return; }
    atomic_fetch_add(&g_gl_try, 1);
    if (!pthread_mutex_trylock(&G)) return;
    uint64_t t0 = thread_clock(); pthread_mutex_lock(&G); uint64_t dt = thread_clock() - t0;
    uint64_t ns = thread_clock_ns(dt); int b = ns < 1000 ? 0 : ns < 10000 ? 1 : ns < 100000 ? 2 : ns < 1000000 ? 3 : ns < 10000000 ? 4 : ns < 100000000 ? 5 : 6;
    atomic_fetch_add(&g_gl_wait, 1); atomic_fetch_add(&g_gl_b[b], 1);
    for (uint64_t o = atomic_load(&g_gl_max); ns > o && !atomic_compare_exchange_weak(&g_gl_max, &o, ns);) {}
}
static thr_t *g_all, *g_wait;
static pthread_key_t g_keythr;
// Key zero can already belong to the host before guest initialization (Darwin
// may expose pthread_self there). Publish only after key creation succeeds;
// callers outside a guest thread must never interpret unrelated TSD as thr_t.
// The Android preemption signal handler also reads this flag; never use an
// atomic implementation that can fall back to a library lock in a handler.
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "guest TSD readiness must be signal-safe");
static _Atomic int g_keythr_ready;
static thr_t *thread_current_or_null(void) {
    return atomic_load_explicit(&g_keythr_ready, memory_order_acquire)
        ? pthread_getspecific(g_keythr) : NULL;
}
static uint64_t g_next_id = 0x51, g_wseq;
#if !defined(__APPLE__)
static void pause_diag_boundary_locked(thr_t *, int);
static void pause_diag_condition_locked(thr_t *, int);
static void sleep_wait_boundary(thr_t *);
#endif
#if !defined(__APPLE__)
#include "../../android/native/runtime/fault/thread_android.inc"
#endif
static int thread_condition_wait(thr_t *t, pthread_cond_t *cv, const struct timespec *deadline) {
#if defined(__APPLE__)
    (void)t;
    return deadline ? pthread_cond_timedwait(cv, &G, deadline) : pthread_cond_wait(cv, &G);
#else
    return android_condition_wait(t, cv, deadline);
#endif
}

// ---------- 句柄表：handle = 序号 << 15 | 下标（同 Horizon），序号每次分配递增，旧句柄复用格子后自然失效 ----------
#define MAX_H 8192
// Switch 应用进程资源上限（Ryujinx ProcessLoaderHelper：线程 608、传输内存 128；KProcess 句柄表 1024 格）。缺省开（10-24 照 Switch 语义：
// 游戏本就受这些上限约束；计数只会比 Switch 松——退出未关句柄的线程、服务还拿着的传输内存不计）；S2A_SVC_LIMITS=0 应急关
#define S2A_OBJ_NATIVE_REF 15   // 原生层内部引用计数对象（见 s2a_native_handle_new）
#define LIM_THREADS 608
#define LIM_TMEM    128
#define LIM_HANDLES 1024
static int lim_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("S2A_SVC_LIMITS"); v = !(e && *e == '0'); } return v; }
static int g_h_used, g_h_peak;   // 持 G：在用句柄数与本局峰值（收局报告打峰值，看离 1024 还有多远）
static int g_kcnt[16], g_kpeak[16];   // 持 G：按种类（kind & 15）的在用句柄数与峰值（资源上限对象的当前值 / 峰值，svc_mem.c 读）
static uint64_t g_cap_core, g_cap_prio;   // NPDM ThreadInfo 允许的核 / 优先级掩码（svc_mem.c 起局时设；0 = 没设、不核）
void s2a_thread_set_caps(uint64_t core_mask, uint64_t prio_mask) { g_cap_core = core_mask; g_cap_prio = prio_mask; }
// ---------- 批 D：Horizon 调度语义（S2A_SCHED_HZ=1 开，缺省关；照 Ryujinx KThread / KProcess / SyscallHandler，影响帧时，待测试设备2 同包 A/B）----------
//   ① 主线程按 NPDM 的主线程优先级与缺省核起（原先写死 44 / 核 0 / 掩码 0x7）；核号 -2 = 进程缺省核、掩码只含它（CreateThread / SetThreadCoreMask）
//   ② 锁的优先级继承：持锁者的生效优先级 = min(基础优先级, 挂在它所持锁上的等待者的生效优先级)，顺锁链往上传（KThread.UpdatePriorityInheritance）
//   ③ 0x36 禁止抢占：进系统调用时 TLR+0x100（禁止计数）非 0、本核没有被钉的线程 → 钉在当前核、优先级抬到「允许的最高优先级 − 1」、TLR+0x102 = 1；
//      线程调 0x36 → 解钉、清标志（SyscallHandler.SvcCall / KThread.Pin / SynchronizePreemptionState）
static int g_def_core, g_main_prio = 44;
// 硬件核数（核号校验的上界，照 Horizon「核号 < 核数」）：Switch 4；Switch 2 8 个（游戏能用哪些另由 NPDM 核掩码定，Pokopia 0x3f = 0–5）。svc_mem.c 起局按 NPDM 设，在 set_npdm 之前
static int g_hw_cores = 4;
void s2a_thread_set_hw_cores(int n) { if (n > 0 && n <= 64) g_hw_cores = n; }
int s2a_thread_hw_cores(void) { return g_hw_cores; }
void s2a_thread_set_npdm(int def_core, int main_prio) { if (def_core >= 0 && def_core < g_hw_cores) g_def_core = def_core; if (main_prio > 0 && main_prio < 64) g_main_prio = main_prio; }
// 三项各自一个开关，S2A_SCHED_HZ=1 = 三项全开（真机 A/B 能分开看是哪一项的影响）：
//   S2A_HZ_CORE ①（含 59 / 63 时间片轮转：-2 只给缺省核后，同核同优先级要靠它轮转，否则空转者饿死同核等待者——t_thread ⑧ 记的 10-20 暗黑2 卡死）、
//   S2A_HZ_PI ②、S2A_HZ_PIN ③
static int hz_flag(const char *name) { const char *e = getenv(name), *a = getenv("S2A_SCHED_HZ"); return (e && !strcmp(e, "1")) || (a && !strcmp(a, "1")); }
static int hz_core(void) { static int v = -1; if (v < 0) { v = hz_flag("S2A_HZ_CORE"); if (v) LOG("Horizon 调度：主线程按 NPDM、核号 -2 = 缺省核、59 / 63 时间片轮转（S2A_HZ_CORE）"); } return v; }
static int hz_pi(void) { static int v = -1; if (v < 0) { v = hz_flag("S2A_HZ_PI"); if (v) LOG("Horizon 调度：锁优先级继承（S2A_HZ_PI）"); } return v; }
static int hz_pin(void) { static int v = -1; if (v < 0) { v = hz_flag("S2A_HZ_PIN"); if (v) LOG("Horizon 调度：0x36 禁止抢占（S2A_HZ_PIN）"); } return v; }
int s2a_handle_peak(void) { return g_h_peak; }
int s2a_test_handle_used(void) { return g_h_used; }
static struct { uint32_t kind; uint16_t gen; void *obj; uint64_t cookie; } g_h[MAX_H];
static uint16_t g_gen;
#ifndef S2A_UNIT_TEST
int s2a_det_mode(void), s2a_det_in_turn(void);
#endif
static uint16_t g_gen_async __attribute__((unused));
// ---------- 即时存档：句柄创建 / 关闭的有序日志（存档模式）与恢复时的强制分配 ----------
// 日志项：类型（0 建、1 关）、句柄值、种类、上下文（≥0 = 服务请求日志序号；-2 = 服务层在请求外建的（初始化 / 宿主线程）；-1 = 游戏系统调用在原生层建的）、
//   关的时候记「此前最后一条记过日志的请求序号」，恢复重放时按这个次序穿插执行关闭
// pin（原填充位）：建的那条被几条留在请求日志里的请求递过（钉住的建 / 关两条不成对删：重放那些请求要靠它把句柄建出来）；uid（原填充位）：建的那条的编号（解钉按它找，句柄值会复用）
typedef struct { uint8_t op, cs; uint16_t pin; uint32_t h, kind; int32_t ctx, after; uint64_t a, b; uint32_t c, uid; } snap_hlog_t;
static snap_hlog_t *g_hlog; static int g_nhlog, g_caphlog; static _Atomic int g_snap_lastseq = -1; static uint32_t g_huid;
static int hlog_last_create(uint32_t h) { for (int i = g_nhlog - 1; i >= 0; i--) if (g_hlog[i].op == 0 && g_hlog[i].h == h) return i; return -1; }
static void hlog_del(int i) { memmove(&g_hlog[i], &g_hlog[i + 1], sizeof *g_hlog * (size_t)(g_nhlog - i - 1)); g_nhlog--; }
static __thread int t_cs_h, t_snap_ctx = -2;
static __thread int t_close_cs;   // 本线程这次关句柄是服务层（C#，s2a_handle_close）关的：关的那条日志记 cs = 1，恢复重放时留给服务自己关（snap_apply_closes）
static __thread const uint32_t *t_force; static __thread int t_nforce, t_iforce;
int s2a_snap_mode(void); int s2a_snap_logging(void);
// 单独链本文件的线程单测（t_thread / t_thread_plat / t_thread_wait_diag，含安卓适配层那份）没有快照引擎：弱缺省 = 快照关、不记；
//   放在苹果专属段外面（安卓适配层把那段整个关掉，hlog_add / svc_thread 照样引用）。宿主 / iOS 壳链进 snapshot.c / snapsw.c 的真定义
__attribute__((weak)) void s2a_snap_mark_old(void) {}
__attribute__((weak)) int s2a_snap_restoring(void) { return 0; }
__attribute__((weak)) int s2a_snap_logging(void) { return 0; }
void s2a_snap_hctx(int seq) { t_snap_ctx = seq; thr_t *t = thread_current_or_null(); if (t) t->cs_seq = seq; }   // C# 请求开始时设本线程的请求序号（结束设 -2）
// 请求日志收住用的四个导出：安卓快照恒关不调，安卓那份由 snapsw.c 给空实现（两边都定义会重复）
#if defined(__APPLE__)
// 请求日志要删第 seq 条之前问：它建的句柄还有活着的吗（活着的要靠重放这条建，不能删）
int s2a_snap_seq_live(int seq) {
    glock(); int live = 0;
    for (int i = 0; i < g_nhlog && !live; i++) if (g_hlog[i].op == 0 && g_hlog[i].cs && g_hlog[i].ctx == seq) {
        live = 1; for (int j = i + 1; j < g_nhlog; j++) if (g_hlog[j].op == 1 && g_hlog[j].h == g_hlog[i].h) { live = 0; break; } }
    pthread_mutex_unlock(&G); return live;
}
// 请求日志删掉了第 seq 条：它建的句柄（都已关）建 / 关两条一起删
void s2a_snap_hlog_forget(int seq) {
    glock();
    for (int i = 0; i < g_nhlog; ) {
        if (g_hlog[i].op == 0 && g_hlog[i].cs && g_hlog[i].ctx == seq && !g_hlog[i].pin) {
            uint32_t h = g_hlog[i].h; hlog_del(i);
            for (int j = i; j < g_nhlog; j++) if (g_hlog[j].op == 1 && g_hlog[j].h == h) { hlog_del(j); break; }
        } else i++;
    }
    pthread_mutex_unlock(&G);
}
// 一条要留在请求日志里的请求在消息里递了句柄 h（复制 / 移交）：钉住建 h 的那条（之后 h 关了也不成对删，重放这条时要靠它把 h 建出来）。
//   回建 h 的请求序号（是游戏系统调用 / 服务层请求外建的回 -1），*uid = 那条的编号（解钉用）；日志里没有建 h 的那条回 -1、*uid = 0
int s2a_snap_handle_use(uint32_t h, uint32_t *uid) {
    glock(); int c = hlog_last_create(h), seq = -1; *uid = 0;
    if (c >= 0) { if (g_hlog[c].pin < UINT16_MAX) g_hlog[c].pin++; *uid = g_hlog[c].uid; if (g_hlog[c].cs && g_hlog[c].ctx >= 0) seq = g_hlog[c].ctx; }
    pthread_mutex_unlock(&G); return seq;
}
// 递 h 的那条请求从日志删了（或当时失败）：解钉；钉数归零、h 已关、又不靠请求重放建的，建 / 关两条成对删（同 hlog_add）
void s2a_snap_handle_unuse(uint32_t uid) {
    if (!uid) return;
    glock();
    for (int i = 0; i < g_nhlog; i++) if (g_hlog[i].op == 0 && g_hlog[i].uid == uid) {
        if (g_hlog[i].pin) g_hlog[i].pin--;
        if (!g_hlog[i].pin && (!g_hlog[i].cs || g_hlog[i].ctx < 0))
            for (int j = i + 1; j < g_nhlog; j++) if (g_hlog[j].op == 1 && g_hlog[j].h == g_hlog[i].h) { hlog_del(j); hlog_del(i); break; }
        break;
    }
    pthread_mutex_unlock(&G);
}
#endif
void s2a_snap_hcommit(int seq) { if (seq >= 0) atomic_store(&g_snap_lastseq, seq); }   // C# 请求完成、追加进日志时设：此后的关闭记在它之后（日志按完成先后，SnapLog.cs）
void s2a_snap_force(const uint32_t *v, int n) { t_force = v; t_nforce = n; t_iforce = 0; }        // 恢复重放：本线程接下来服务层建的句柄按这串值分
int s2a_snap_force_left(void) { return t_nforce - t_iforce; }
// 句柄日志按「此刻活着的」有界（维护者 11-19：不能随游玩时长增长）：一个句柄的建 / 关两条，在建它的是游戏系统调用或服务层请求外（不靠请求重放建）时，
//   关的那一刻成对删掉；服务层请求里建的，跟那条请求同进退——请求从请求日志里删掉时（services SnapLog.cs ReqTable）调 s2a_snap_hlog_forget 一起删
static void hlog_add(int op, uint32_t h, uint32_t kind, int cs, void *obj) {   // 持 G
    if (!s2a_snap_logging()) return;
    if (g_nhlog == g_caphlog) { g_caphlog = g_caphlog ? g_caphlog * 2 : 4096; g_hlog = realloc(g_hlog, sizeof *g_hlog * (size_t)g_caphlog); }
    // 关：记在哪条之后。服务层在处理第 N 条请求的过程中关的（本线程 t_snap_ctx = N）记在 N 之后：恢复重放 N 时它自己会再关一次，
    //   重放完按「还在才关」就跳过；N 当时失败没重放的，这里照样关。原先一律记「此前最后完成的那条」（N−1）：重放 N−1 后先关一次、
    //   重放 N 时服务又关同一个句柄，回 InvalidHandle、恢复中止（大乱斗第 187 条关 AudioDevice 会话）
    int after = op == 1 && t_snap_ctx >= 0 ? t_snap_ctx : atomic_load(&g_snap_lastseq);   // 关调用不带 cs（是不是服务层建的由重放时查建的那条）
    if (op == 1) { int c = hlog_last_create(h); if (c >= 0 && !g_hlog[c].pin && (!g_hlog[c].cs || g_hlog[c].ctx < 0)) { hlog_del(c); return; } }   // 不靠请求重放建的、没被留下的请求递过：建 / 关成对删
    snap_hlog_t x = {(uint8_t)op, (uint8_t)cs, 0, h, kind, op == 0 ? (cs ? t_snap_ctx : -1) : 0, after, 0, 0, 0, op == 0 ? ++g_huid : 0};
    if (op == 0 && (kind & 15) == S2A_OBJ_TMEM && obj) { tmem_t *tm = obj; x.a = tm->addr; x.b = tm->size; x.c = tm->perm; }
    if (op == 0 && ((kind & 15) == S2A_OBJ_EVENT_R || (kind & 15) == S2A_OBJ_EVENT_W)) x.a = (uint64_t)(uintptr_t)obj;   // 事件：A 里的事件对象身份
    g_hlog[g_nhlog++] = x;
}
static uint32_t h_new_raw(uint32_t kind, void *obj, uint64_t cookie);
static uint32_t h_new(uint32_t kind, void *obj, uint64_t cookie) {
    if (t_cs_h && t_iforce < t_nforce) {   // 恢复重放：按 A 的值放
        uint32_t v = t_force[t_iforce++], i = v & 0x7fff; uint16_t g = (uint16_t)(v >> 15);
        if (i && i < MAX_H && !g_h[i].kind) {
            g_h[i].kind = kind; g_h[i].gen = g; g_h[i].obj = obj; g_h[i].cookie = cookie; g_h_used++; g_kcnt[kind & 15]++;
            return v;
        }
        LOG("【红】即时存档恢复：强制分配句柄 %#x 时格已占（种类 %u），改按常规分配", v, i < MAX_H ? g_h[i].kind : 0);
    }
    uint32_t h = h_new_raw(kind, obj, cookie);
    if (h) hlog_add(0, h, kind, t_cs_h, obj);
    return h;
}
static uint32_t h_new_cs(uint32_t kind, void *obj, uint64_t cookie) { t_cs_h = 1; uint32_t h = h_new(kind, obj, cookie); t_cs_h = 0; return h; }
// 存档：句柄日志写出
int s2a_snap_hlog_save(FILE *f) { glock(); int e = fwrite(&g_nhlog, sizeof g_nhlog, 1, f) != 1 || (g_nhlog && fwrite(g_hlog, sizeof *g_hlog, (size_t)g_nhlog, f) != (size_t)g_nhlog); int n = g_nhlog; pthread_mutex_unlock(&G);
    LOG("即时存档：句柄日志 %d 项", n); return e ? -1 : 0; }
static uint32_t h_new_raw(uint32_t kind, void *obj, uint64_t cookie) {   // 持 G；满了返回 0（调用方报红回 OutOfHandles）
    // 确定性档：游戏线程持串行锁时分配的（游戏可见、顺序确定）用前半表与 g_gen；宿主线程异步分配的用后半表与另一个代数——
    // 否则异步分配插队，游戏拿到的句柄号每局不同（10-20 重放第 101 项 ConnectToNamedPort 分叉）
    int lo = 1, hi = MAX_H; uint16_t *gen = &g_gen;
    if (g_h_used >= LIM_HANDLES && lim_on()) {
        static int said; if (!said++) LOG("【红】句柄表到 Switch 上限 %d 格，回 HandleTableFull（同 Switch；只打第一次）", LIM_HANDLES);
        return 0;
    }
#ifndef S2A_UNIT_TEST
    if (s2a_det_mode()) { if (s2a_det_in_turn()) hi = MAX_H / 2; else { lo = MAX_H / 2; gen = &g_gen_async; } }
#endif
    for (int i = lo; i < hi; i++) if (!g_h[i].kind) {
        if (!++*gen || *gen > 0x7fff) *gen = 1;
        g_h[i].kind = kind; g_h[i].gen = *gen; g_h[i].obj = obj; g_h[i].cookie = cookie;
        if (++g_h_used > g_h_peak) g_h_peak = g_h_used;
        if (++g_kcnt[kind & 15] > g_kpeak[kind & 15]) g_kpeak[kind & 15] = g_kcnt[kind & 15];
        return (uint32_t)*gen << 15 | (uint32_t)i;
    }
    int cnt[16] = {0}; for (int i = 1; i < MAX_H; i++) cnt[g_h[i].kind & 15]++;
    LOG("【红】句柄表满（%d 格）：线程 %d 事件读 %d 写 %d 会话 %d 共享内存 %d 传输内存 %d", MAX_H - 1, cnt[S2A_OBJ_THREAD], cnt[S2A_OBJ_EVENT_R], cnt[S2A_OBJ_EVENT_W], cnt[S2A_OBJ_SESSION], cnt[S2A_OBJ_SHMEM], cnt[S2A_OBJ_TMEM]);
    return 0;
}
// 事件对象号 → 对象（确定性档：日志按对象号记宿主线程的触发 / 清除；重放时据此代为生效）
#define EVMAP 4096
static event_t *g_evmap[EVMAP];   // 持 G；按 did 开放寻址
static void __attribute__((unused)) evmap_put(event_t *e) { for (uint32_t i = e->did % EVMAP, k = 0; k < EVMAP; i = (i + 1) % EVMAP, k++) if (!g_evmap[i]) { g_evmap[i] = e; return; } }
static void evmap_del(event_t *e) { for (uint32_t i = e->did % EVMAP, k = 0; k < EVMAP; i = (i + 1) % EVMAP, k++) if (g_evmap[i] == e) { g_evmap[i] = NULL; return; } }
#ifndef S2A_UNIT_TEST
int s2a_det_event_state(uint32_t did, int signaled);   // det.c：记录档记一笔回原值 / 重放档当场取录的状态回它
#endif
static void __attribute__((unused)) ev_expose(event_t *e) {   // 持 G；确定性档：事件第一次对游戏可见（游戏线程持锁时建 / 拿到句柄）——这一刻确定，在这里编号
#ifndef S2A_UNIT_TEST
    static uint32_t n; e->did = ++n; evmap_put(e); e->signaled = s2a_det_event_state(e->did, e->signaled);
#else
    (void)e;
#endif
}
static event_t *evmap_get(uint32_t did) { for (uint32_t i = did % EVMAP, k = 0; k < EVMAP; i = (i + 1) % EVMAP, k++) if (g_evmap[i] && g_evmap[i]->did == did) return g_evmap[i]; return NULL; }
static int h_idx(uint32_t h) {   // 持 G；无效返回 0
    unsigned i = h & 0x7fff, gen = h >> 15;
    if (!i || i >= MAX_H || !g_h[i].kind || g_h[i].gen != gen) return 0;
    return (int)i;
}
static void *h_obj(uint32_t h, uint32_t kind) {   // 持 G
    if (h == S2A_H_CUR_THREAD && kind == S2A_OBJ_THREAD) return thread_current_or_null();
    int i = h_idx(h); return i && g_h[i].kind == kind ? g_h[i].obj : NULL;
}
static void h_free(int i) { if (g_h[i].kind) { g_h_used--; g_kcnt[g_h[i].kind & 15]--; } g_h[i].kind = 0; g_h[i].obj = NULL; g_h[i].cookie = 0; }

static int trace_on(void) { static int tr = -1; if (tr < 0) { const char *e = getenv("S2A_SVC_TRACE"); tr = e && *e == '1'; } return tr; }
static thr_t *cur(void) { thr_t *t = thread_current_or_null(); if (!t) die("非游戏线程调了线程类系统调用"); return t; }
// 诊断打点用（diag_brk.c，信号里可调）：本线程线程类系统调用累计次数与最近一次的调用号；非游戏线程返回 0
int s2a_thread_svc_stat(uint64_t *n) { thr_t *t = thread_current_or_null(); if (!t) { *n = 0; return 0; } *n = t->nsvc; return t->last_svc; }

#include "thread_wait_diag.inc"

// ---------- 等待原语（持 G）----------
static void wait_unlink(thr_t *t) { for (thr_t **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; } t->next = NULL; }
// 按内核顺序挑等待者：优先级数值小者优先，同优先级先登记者优先。kind 1 按锁地址、2 按条件键、3 按地址
static thr_t *pick(int kind, uint64_t a) {
    thr_t *b = NULL;
    for (thr_t *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
static int core_of(const thr_t *t);
// 同核高优先级唤醒让先（缺省开，S2A_PEER_YIELD=0 关）：与虚拟核开关无关——去掉虚拟核后它仍是 Horizon 语义（上游的 core_preempt，只在真唤醒时等）
static int peer_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("S2A_PEER_YIELD"); v = !(e && !strcmp(e, "0")); } return v; }
// 唤醒追踪（S2A_WAKE_TRACE=1，缺省关；时序族量数，见 状态/时序族设计_原生崩溃线_1101.md）：
//   ① 被唤醒方「唤醒 → 回到游戏」与唤醒方「唤醒 → 自己回到游戏」两组延迟分布：比谁先回游戏（真机上被唤醒方要等它的核，宿主上立刻在空闲核真并发）
//   ② 用户态停顿：上次回游戏到这次进系统调用超过 2 ms 的段，按这段里线程 CPU 时间分「在算」与「被宿主停住」（织梦岛：发信号后 19 ms 没走到下一条指令）
//   关着时每处只多一次读静态变量
static int wt_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("S2A_WAKE_TRACE"); v = e && *e == '1'; if (v) LOG("唤醒追踪：开（S2A_WAKE_TRACE）"); } return v; }
static uint64_t wt_cpu_ns(void) { struct timespec ts; return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) ? 0 : (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static _Atomic uint64_t g_wt_b[2][7], g_wt_n[2], g_wt_st[4], g_wt_st_ms[4];   // [0] 被唤醒方 [1] 唤醒方；停顿 [0] 被停住 [1] 在算 [2] 宿主阻塞段（DM_BLOCK，直连等待）[3] 回客体等虚拟核（次数、累计 ms）
static __thread uint64_t t_wt_blk_ns, t_wt_blk_t0, t_wt_core_ns, t_wt_blk_snap, t_wt_core_snap;   // 本线程 DM_BLOCK 段与惰性取核等待的累计墙钟；snap = 上次回游戏时的值
static void wt_bucket(int k, uint64_t ns) { int b = ns < 1000 ? 0 : ns < 10000 ? 1 : ns < 100000 ? 2 : ns < 1000000 ? 3 : ns < 10000000 ? 4 : ns < 100000000 ? 5 : 6; atomic_fetch_add(&g_wt_b[k][b], 1); atomic_fetch_add(&g_wt_n[k], 1); }
static void wake(thr_t *t, int64_t res) {
    wait_diag_wake_locked(t, res);
    if (wt_on()) { thr_t *w = thread_current_or_null(); uint64_t n = thread_clock(); t->tr_wake_t = n; t->tr_waker = w ? w->id : 0; if (w && w != t) w->tr_lastwake_t = n; }
    wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res; t->wake_t = thread_clock(); pthread_cond_signal(&t->cv);
#if defined(__APPLE__)
    if (t->kqw) { struct kevent64_s ev; EV_SET64(&ev, 2, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0, 0, 0); kevent64(t->kqfd, &ev, 1, NULL, 0, 0, NULL); }   // 带超时的等待挂在 kqueue 上（按时醒）
#endif
    // 同核高优先级唤醒让先（Horizon：被唤醒的线程掩码只含本核、优先级更高 → 立即抢占唤醒方，直到它再阻塞；Metroid Dread 播放器靠它保序，
    //   见 docs/研究/虚拟核与轻量调度语义.md §1）。只记下来，唤醒方在系统调用返回游戏前等（peer_yield），持 G 时不等
    thr_t *me = thread_current_or_null();
    if (me && me != t && peer_on() && t->mask == 1ull << core_of(me) && t->prio < me->prio) {
        thr_t *previous = h_obj(me->peer.handle, S2A_OBJ_THREAD);   // 仅 G 内借用；旧目标退出/关句柄后不能压住新的唤醒
        if (!previous || previous->id != me->peer.id || previous->exited || t->prio < previous->prio)
            me->peer = (peer_wait_t){t->handle, t->id, atomic_load_explicit(&t->nsvc_all, memory_order_relaxed)};
    }
}
static void wake_sync_waiters(void) { for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; } }
// S2A_WAIT_TRACE=1（缺省关，热路径只多一次原子读）：等待方逐线程记「等待期间我的键 / 地址被发过几次信号」。
//   键表（g_cvl）是 1024 格直映哈希，键一多就互相覆盖，「从没被发过信号」可能是撞掉的记录；逐线程计数不撞。
//   只算挂上之后的信号（block 里清零），所以「等了 10 秒、期间 0 次」= 真没人发，而不是记录被覆盖
static int wait_trace_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("S2A_WAIT_TRACE"); v = e && *e == '1'; } return v; }
static void wait_trace_bump_cv(uint64_t key) {   // 持 G
    if (!wait_trace_on()) return;
    for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) w->nsig++;
}
#if defined(__APPLE__)
// 按时醒（同 Horizon：SleepThread 睡到点就醒，带超时的等待到点就超时）。nanosleep / pthread_cond_timedwait 的定时器余量随线程 QoS 放宽：
//   直连缺省省电组合把游戏线程设成 utility 后，10-25 Spare-host 数码宝贝的音频线程（优先级 29）每次睡 31 ms，平均多睡 103 ms（最多 150 ms），
//   赶不上 2048 帧一块的节拍，游戏只好交整块静音（听着断续）。改用 kqueue 一次性定时器加 NOTE_CRITICAL（系统尽力按时触发、不合并），
//   与 QoS 无关：Spare-host utility 睡 30 ms 多睡 36 µs（nanosleep 8.5 ms）。每线程一个 kqueue（建时挂好用户事件 2，供唤醒方触发），
//   线程退出时由键析构关掉；建不出或等失败时退回原做法，报红一次
static pthread_key_t g_kq_key; static pthread_once_t g_kq_once = PTHREAD_ONCE_INIT;
static void kq_close(void *p) { close((int)(intptr_t)p - 1); }
static void kq_init(void) { pthread_key_create(&g_kq_key, kq_close); }
static void kq_red(const char *what, int n, int e) { static _Atomic int warned; if (!atomic_fetch_add(&warned, 1)) LOG("【红】按时醒：%s 失败（n=%d errno=%d），退回原做法", what, n, e); }
static int thr_kq(void) {
    pthread_once(&g_kq_once, kq_init);
    int kq = (int)(intptr_t)pthread_getspecific(g_kq_key) - 1;
    if (kq >= 0) return kq;
    if ((kq = kqueue()) < 0) { kq_red("建 kqueue", kq, errno); return -1; }
    struct kevent64_s ev; EV_SET64(&ev, 2, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0, 0, 0);
    if (kevent64(kq, &ev, 1, NULL, 0, 0, NULL) < 0) { kq_red("挂用户事件", -1, errno); close(kq); return -1; }
    pthread_setspecific(g_kq_key, (void *)(intptr_t)(kq + 1));
    return kq;
}
#ifdef S2A_UNIT_TEST
// 单测记下最近一次挂的定时器：判「向内核要的是不是按时、不合并的一次性定时器、时长对不对」——这是我们代码决定的部分，与机器忙不忙无关
//   （实际几点醒还叠着调度排队，忙机上量不准：10-26 两次门禁假红）
static struct kevent64_s g_kq_timer_last; static _Atomic int g_kq_timer_n;
int s2a_test_kq_timer(struct kevent64_s *out) { *out = g_kq_timer_last; return atomic_load(&g_kq_timer_n); }
#endif
// 在本线程 kqueue 上等到 end（now_ns 口径）或用户事件；1 = 到点，0 = 用户事件（或被信号打断，调用方复查），-1 = 失败
// leeway > 0：可合并定时器（NOTE_LEEWAY，ext[1] = 余量纳秒），系统可把它和别的定时器并到同一刻；0 = NOTE_CRITICAL 按时触发
static int kq_wait_until_lw(int kq, uint64_t end, uint64_t leeway) {
    int64_t left = (int64_t)(end - now_ns());
    if (left <= 0) return 1;
    struct kevent64_s ev, out; EV_SET64(&ev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS | (leeway ? NOTE_LEEWAY : NOTE_CRITICAL), left, 0, 0, leeway);
#ifdef S2A_UNIT_TEST
    g_kq_timer_last = ev; atomic_fetch_add(&g_kq_timer_n, 1);
#endif
    int n = kevent64(kq, &ev, 1, &out, 1, 0, NULL);
    if (n == 1 && !(out.flags & EV_ERROR)) return out.filter == EVFILT_TIMER;
    if (n < 0 && errno == EINTR) return 0;
    kq_red("等定时器", n, n < 0 ? errno : (int)out.data); return -1;
}
static int kq_wait_until(int kq, uint64_t end) { return kq_wait_until_lw(kq, end, 0); }
static void sleep_ontime(int64_t ns);
// 图形层出帧节拍用（graphics/vk hle_vk.m vkAcquireNextImageKHR、graphics/gl egl.m 交换）：直连缺省游戏线程 QoS utility，普通 nanosleep 的定时器余量被放宽，
//   10-26 Mac 银河 2 标题页每帧该睡约 16 ms 多睡约 100 ms（帧时中位 118 ms；同包游戏线程 interactive 16.6 ms），改走本线程 kqueue NOTE_CRITICAL 定时器按时醒
static void sleep_ontime_lw(int64_t ns, uint64_t leeway) {
    int kq = thr_kq(); uint64_t end = now_ns() + (uint64_t)ns; int r = 0;
    while (kq >= 0 && (r = kq_wait_until_lw(kq, end, leeway)) == 0) { if (atomic_load(&g_session_stop)) return; }
    if (r == 1) return;
    int64_t left = (int64_t)(end - now_ns());
    if (left > 0) { struct timespec ts = {left / 1000000000, left % 1000000000}; nanosleep(&ts, NULL); }
}
static void sleep_ontime(int64_t ns) { sleep_ontime_lw(ns, 0); }
#else
static void sleep_ontime_until(int64_t ns, thr_t *sleeper) {
    uint64_t end = now_ns() + (uint64_t)ns;
    for (;;) {
        // 只有SleepThread承诺此处不持宿主锁、不访问guest；SIGURG handler只置请求，
        // 必须在正常上下文先ack/停到VM epoch结束，再按原截止时刻算剩余时间。
        if (sleeper) android_vm_barrier_poll(sleeper, sleep_wait_boundary);
        int64_t left = (int64_t)(end - now_ns());
        if (left <= 0) return;
        struct timespec wait = {left / 1000000000, left % 1000000000};
        if (!nanosleep(&wait, NULL) || errno != EINTR) return;
    }
}
static void sleep_ontime(int64_t ns) { sleep_ontime_until(ns, NULL); }
#endif
void s2a_sleep_ns(int64_t ns) { if (ns > 0) sleep_ontime(ns); }
static void sleep_thread_ontime(thr_t *thread, int64_t ns) {
#if defined(__APPLE__)
    (void)thread; sleep_ontime(ns);
#else
    sleep_ontime_until(ns, thread);
#endif
}
// 被短睡轮询退避拉长的那次睡（10-30，调研「出帧线程准时省电」修法 2）：已判定为轮询，晚醒本就允许 → 可合并定时器，余量 = 时长一半，
//   系统可把几条轮询线程和别的定时器的唤醒并到同一刻；S2A_SLEEP_LEEWAY=0 关（仍按时 NOTE_CRITICAL）。没被拉长的睡照旧按时
static int g_sl_leeway = -1;
static void sleep_thread_backoff(thr_t *thread, int64_t ns) {
    if (g_sl_leeway < 0) { const char *e = getenv("S2A_SLEEP_LEEWAY"); g_sl_leeway = !(e && !strcmp(e, "0")); LOG("短睡退避的睡：%s", g_sl_leeway ? "可合并定时器（余量 = 时长一半，S2A_SLEEP_LEEWAY=0 关）" : "按时（S2A_SLEEP_LEEWAY=0）"); }
#if defined(__APPLE__)
    (void)thread; if (g_sl_leeway) sleep_ontime_lw(ns, (uint64_t)ns / 2); else sleep_ontime(ns);
#else
    sleep_ontime_until(ns, thread);
#endif
}
// 挂起直到被唤醒或超时（ns < 0 无限、0 立即超时）；返回 wres 或 R_TIMEOUT
#ifndef S2A_UNIT_TEST
int s2a_det_replaying(void); void s2a_det_acquire(void), s2a_det_release(void); int s2a_det_wait_outcome(int woke, int64_t res, int64_t *want_res); void s2a_det_diverge_note(const char *what);
// 确定性档的阻塞（持 G 进出）：放开串行锁真等（记录 / 串行档）或直接等轮次（重放档）；结局在重新拿到串行锁、轮到自己那一刻定——
// 那时被唤醒了就是唤醒，否则超时。记录档记下结局，重放档按录的结局走：录的唤醒而此刻还没被唤醒（唤醒来自宿主线程，时机不同）就持锁等它来
static int64_t block_det(thr_t *t, int64_t ns) {
    int mode = s2a_det_replaying() ? 3 : s2a_det_mode();   // 分叉后的重放按真等走
    struct timespec dl;
    if (ns > 0) { clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += ns / 1000000000; dl.tv_nsec += ns % 1000000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } }
    pthread_mutex_unlock(&G); s2a_det_release();
    for (int real = mode != 3;; real = 1) {
        if (real) { glock(); while (!t->woke && (thread_condition_wait(t, &t->cv, ns > 0 ? &dl : NULL)) != ETIMEDOUT) {} pthread_mutex_unlock(&G); }
        s2a_det_acquire(); glock();
        if (real || s2a_det_replaying() || t->woke) break;
        pthread_mutex_unlock(&G); s2a_det_release();   // 等轮次时重放分叉了、还没被唤醒：放开真等
    }
    int64_t want = 0; int woke = s2a_det_wait_outcome(t->woke, t->woke ? t->wres : R_TIMEOUT, &want);
    if (mode == 3 && woke && !t->woke) {   // 录的被唤醒：等真唤醒（10 秒上限）
        struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += 10;
        while (!t->woke) if (thread_condition_wait(t, &t->cv, &dl) == ETIMEDOUT) { s2a_det_diverge_note("录的被唤醒，重放 10 秒等不到唤醒"); break; }
    }
    if (mode == 3 && !woke && t->woke && t->wkind != 4) s2a_det_diverge_note("录的超时，重放时已被唤醒（锁 / 条件 / 地址）");
    if (woke && t->woke) return t->wres;
    if (t->wkind) wait_unlink(t);
    t->wkind = 0; return R_TIMEOUT;
}
#endif
// 条件键信号记录（10-24 xc3 第 1300 帧停住：几条线程等条件变量只等过 1 次就再没被唤醒）：每次 0x1D 记「键 → 谁发、何时、唤醒几条、之后还剩几条」，
//   线程表里等条件的线程据此打「这个键最近一次被谁发过信号」或「从没被发过信号」。持 G 写读，直接映射 1024 项（键冲突时覆盖，只用于看现场）
enum { CVL_N = 1024 };
static struct { uint64_t key, t; uint64_t sig_id, n; int woken, left; } g_cvl[CVL_N];
static unsigned cvl_h(uint64_t k) { return (unsigned)((k >> 2) * 0x9E3779B97F4A7C15ull >> 54); }
static void cvl_note(uint64_t key, uint64_t sig_id, int woken, int left) {   // 持 G
    unsigned i = cvl_h(key); if (g_cvl[i].key != key) { g_cvl[i].key = key; g_cvl[i].n = 0; }
    g_cvl[i].t = thread_clock(); g_cvl[i].sig_id = sig_id; g_cvl[i].woken = woken; g_cvl[i].left = left; g_cvl[i].n++;
}
static double mach_ms(uint64_t d) { return (double)thread_clock_ns(d) / 1e6; }
static int64_t block(thr_t *t, int kind, int64_t ns) {
    if (atomic_load(&g_session_stop)) return R_CANCELLED;
    t->wkind = kind; t->woke = 0; t->next = g_wait; g_wait = t; t->wseq = ++g_wseq; t->wait_t0 = thread_clock();
    t->nsig = 0;   // 每次新挂上去重新数被发过几次信号（S2A_WAIT_TRACE）
    if (ns == 0) { wait_diag_block_locked(t, kind, ns, NULL); wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    t->nblk++; // Zero-timeout polling above is not a blocking wait.
#ifndef S2A_UNIT_TEST
    if (s2a_det_mode()) return block_det(t, ns);
#endif
    struct timespec dl;
    if (ns > 0) { clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += ns / 1000000000; dl.tv_nsec += ns % 1000000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } }
    wait_diag_block_locked(t, kind, ns, ns > 0 ? &dl : NULL);
#if defined(__APPLE__)
    // 带超时：在本线程 kqueue 上等按时定时器或唤醒方的用户事件（按时醒，见 thr_kq）；放 G 等、拿回 G 复查 woke，唤醒在 G 下置位，不会丢
    int kq = ns > 0 ? thr_kq() : -1;
    if (kq >= 0) {
        uint64_t end = now_ns() + (uint64_t)ns; int r = 0;
        t->kqfd = kq; t->kqw = 1;
        while (!t->woke) { pthread_mutex_unlock(&G); r = kq_wait_until(kq, end); glock(); wait_diag_condition_locked(t, r == 1 ? ETIMEDOUT : r < 0 ? EIO : 0); if (r) break; }
        t->kqw = 0;
        if (t->woke) { struct kevent64_s ev; EV_SET64(&ev, 1, EVFILT_TIMER, EV_DELETE, 0, 0, 0, 0, 0); kevent64(kq, &ev, 1, NULL, 0, 0, NULL); return t->wres; }   // 撤掉没到点的定时器
        if (r == 1) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
        ns = (int64_t)(end - now_ns()); if (ns <= 0) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }   // kqueue 失败（已报红）：剩下的按原做法等
    }
#endif
    while (!t->woke) {
        int rc = thread_condition_wait(t, &t->cv, ns > 0 ? &dl : NULL);
        wait_diag_condition_locked(t, rc);
        if (rc == ETIMEDOUT && !t->woke) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    }
    return t->wres;
}

// ---------- 锁 ----------
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
// 释放 addr 处的锁：交给等在它上面的第一个线程，或清零
// 生效优先级重算（持 G；S2A_SCHED_HZ）：基础优先级、钉核抬升、挂在它所持锁上的等待者三者取最小（数小 = 优先级高）
static thr_t *lock_owner(uint64_t addr) { return h_obj(ld32(addr) & ~WAITERS_BIT, S2A_OBJ_THREAD); }
static void prio_update(thr_t *o) {
    if (!o) return;
    int p = o->base_prio;
    if (o->pinned) { int hi = g_cap_prio ? __builtin_ctzll(g_cap_prio) - 1 : 0; if (hi < 0) hi = 0; if (hi < p) p = hi; }
    if (hz_pi()) for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 1 && w->prio < p && lock_owner(w->waddr) == o) p = w->prio;
    o->prio = p;
}
// 等待者 prio 挂上 addr 处的锁：持锁者及其沿锁链往上的持锁者生效优先级至少抬到 prio（最多 8 层，防成环）
static void prio_raise(uint64_t addr, int prio) {
    thr_t *o = lock_owner(addr);
    for (int d = 0; o && d < 8 && prio < o->prio; d++) { o->prio = prio; o = o->wkind == 1 ? lock_owner(o->waddr) : NULL; }
}
static void lock_handoff(uint64_t addr) {
    thr_t *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
    if (hz_pi()) { prio_update(thread_current_or_null()); prio_update(w); }   // 交出锁的少了这批等待者；新持锁者继承剩下的
}
// 让 t 去拿 addr 处的锁：空闲就直接拿到并唤醒，否则置等待者位并转为锁等待
static void lock_acquire_for(thr_t *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    // 锁字里的持有者句柄不是线程：没人会来交锁。照 Ryujinx KAddressArbiter.TryAcquireMutex 以 InvalidHandle 唤醒，不挂成锁等待（原先永久挂起、无报红）
    if (!h_obj(v & ~WAITERS_BIT, S2A_OBJ_THREAD)) { wake(t, R_BAD_HANDLE); return; }
    t->wkind = 1; t->waddr = addr; t->wtag = tag; t->wseq = ++g_wseq;   // 仍在 g_wait 里，改为锁等待；同优先级排在已有锁等待者之后
    if (hz_pi()) prio_raise(addr, t->prio);
}

// 锁标签核对：SDK 以「当前线程对象 +0x1B0 的句柄」作锁字标签，应恒等于调用线程自己的句柄；不等 = 锁字会记错持有者（10-20 动森加载卡死：
// 锁字 0x40d40095 指向不存在的句柄，等锁者认不出自己永远等）。前 20 次报红打两边的值与当前线程对象里的句柄字段
static void tag_check(thr_t *me, uint32_t tag, const char *who, uint64_t addr) {
    if (tag == me->handle) return;
    static _Atomic int n; if (atomic_fetch_add(&n, 1) >= 20) return;
    uint64_t tt = me->tlr ? *(uint64_t *)((uint8_t *)me->tlr + 0x1f8) : 0;
    LOG("【红】%s 标签 %#x ≠ 本线程句柄 %#x（id %llu）锁 %#llx；线程对象 %#llx 句柄字段(+0x1b0) %#x", who, tag, me->handle, (unsigned long long)me->id, (unsigned long long)addr,
        (unsigned long long)tt, tt ? *(uint32_t *)(tt + 0x1b0) : 0);
}

// ---------- 线程 ----------
extern void sw_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp);   // entry.S
extern void sw_gate(void);
void s2a_crash_thread_init(void);                                          // crash.c：信号备用栈
void *s2a_tlr_alloc(void); void s2a_tlr_free(void *tlr);                  // svc_mem.c：窗口 TLS/IO 区
uint32_t s2a_mem_tmem_borrow(uint64_t a, uint64_t n, uint32_t perm); void s2a_mem_tmem_unborrow(uint64_t a, uint64_t n); void s2a_mem_tmem_close(uint64_t a, uint64_t n);   // svc_mem.c：传输内存借出 / 还回源 / 关句柄（映射着不还回）
void s2a_mem_codemem_free(void *o);                                        // svc_mem.c：代码内存对象释放（还回借出的源）
void s2a_cs_thread_exit(uint32_t thread);                                  // gate.c
#define TLS_SIZE 8192
#define THREAD_OBJ_SIZE 0x400
static pthread_key_t g_own_key;
// 线程退出时还它的 TLS 块 / 线程对象：宿主 calloc 的直接 free；即时存档档从确定性 arena 分的挂回空闲表复用（#ef04e6：原先不回收，线程起落多的游戏把 TLS/IO 区吃光后
//   「确定性：TLS/IO 区放不下 TLS 块」致命退出）；确定性诊断档（S2A_DET ≥ 2）照旧不回收，保证每局地址相同
static void own_free(void *p) {
    void **b = p; if (!b) return;
    if (!b[2]) { free(b[0]); free(b[1]); }
#ifndef S2A_UNIT_TEST
    else if (s2a_det_mode() < 2) { void s2a_det_arena_free(void *, size_t); s2a_det_arena_free(b[0], TLS_SIZE); s2a_det_arena_free(b[1], THREAD_OBJ_SIZE); }
#endif
    free(b);
}
static pthread_once_t thread_keys_once = PTHREAD_ONCE_INIT;
static int thread_keys_error;
static void thread_keys_create(void) {
    wait_diag_init();
    thread_keys_error = pthread_key_create(&g_keythr, NULL);
    if (!thread_keys_error) thread_keys_error = pthread_key_create(&g_own_key, own_free);
#if defined(__APPLE__)
    if (!thread_keys_error) {
        g_stop_sp = (uint64_t (*)(void))dlsym(RTLD_DEFAULT, "sw_xstack_stop_sp");
        g_stop_boundary = (int (*)(void))dlsym(RTLD_DEFAULT, "sw_xstack_stop_boundary");
        stop_signal_init();
    }
#endif
    if (!thread_keys_error) atomic_store_explicit(&g_keythr_ready, 1, memory_order_release);
}
static int thread_keys_init(void) {
    int error = pthread_once(&thread_keys_once, thread_keys_create);
    return error ? error : thread_keys_error;
}   // b[2] = 1：确定性档从窗口分配，不回收
// 每线程 TSD 槽的唯一设置点：门 = sw_gate、x18 = 0、tpidr_el0 = ELF TLS 块、tpidrro_el0 = TLR。
// TLR+0x1F8 = 当前线程对象指针：SDK 内部临界区从这里取，主线程与宿主借建的线程没人填，给一块零初始化对象（旧库同做法）
// TLR+0x110 = 当前线程句柄：Horizon 内核建线程时无条件写（主线程起进程时 / svcCreateThread）；Switch 2 SDK 的
// InternalCriticalSection::IsLockedByCurrentThread 拿它比互斥锁属主，留 0 则没人持的锁读成「本线程持有」直接 abort（火纹 Fortune's Weave）
uint64_t s2a_guest_x18_now(void) { return (uint64_t)(uintptr_t)pthread_getspecific(g_key18); }   // 本线程游戏 x18 的槽值（crash.c 崩溃现场打，诊断#ff9a04）
static void guest_thread_init(thr_t *t) {
    pthread_setspecific(g_keygate, (void *)sw_gate);
    pthread_setspecific(g_key18, 0);
    void **blk = calloc(3, sizeof *blk);
#ifndef S2A_UNIT_TEST
    int s2a_snap_mode(void);
    if (s2a_det_mode() >= 2 || s2a_snap_mode()) { void *s2a_det_arena_alloc(size_t); blk[0] = s2a_det_arena_alloc(TLS_SIZE); blk[1] = s2a_det_arena_alloc(THREAD_OBJ_SIZE); blk[2] = (void *)1; } else
#endif
    { blk[0] = calloc(1, TLS_SIZE); blk[1] = calloc(1, THREAD_OBJ_SIZE); }
    t->tlr = s2a_tlr_alloc();
    *(void **)((uint8_t *)t->tlr + 0x1f8) = blk[1];
    *(uint32_t *)((uint8_t *)t->tlr + 0x110) = t->handle;
    pthread_setspecific(g_keytls, blk[0]);
    pthread_setspecific(g_keytlr, t->tlr);
    pthread_setspecific(g_own_key, blk);   // 析构不能挂在 g_keytls / g_keytlr 上：游戏自己会改写这两把键的槽
}
// 游戏线程的调度档：性能核 ≥ 4（Mac）一律 USER_INTERACTIVE；性能核 < 4（iPhone 2 个）除主线程外降 USER_INITIATED，排在显卡 / 渲染关键线程之后
// （旧库 10-19 测试设备1 动森 xctrace 实测依据，见旧库 hle_thread.c guest_qos）
#if defined(__APPLE__)
static int perf_cores(void) { return s2a_qos_perf_cores(); }   // 判法在 s2a_qos.h（与 S2A_QOS_GAME_OTHER 同一份）
// 核掩码只给 Switch 系统核（3 号，Horizon 留给系统 / 后台）的线程映射成 UTILITY（帧率能耗线 8a-3 建议：调度器优先放能效核）
// 游戏主线程 QoS（S2A_GAME_MAIN_QOS=interactive|initiated|utility，缺省 interactive）：能耗对照用（10-20 测试设备2 动森整机录像主线程 98% 在性能核；
// 同包交替量瓦数 / 帧率，帧率能耗线 S2A_GPU_QOS 同口径）
static qos_class_t main_qos(void) {
    static qos_class_t q; if (q) return q;
    const char *v = getenv("S2A_GAME_MAIN_QOS"); q = s2a_qos(S2A_QOS_GAME_MAIN);   // 缺省按全进程一张表（runtime/include/s2a_qos.h）
    if (v && !strcmp(v, "initiated")) q = QOS_CLASS_USER_INITIATED;
    else if (v && !strcmp(v, "utility")) q = QOS_CLASS_UTILITY;
    else if (v && *v && strcmp(v, "interactive")) LOG("【红】S2A_GAME_MAIN_QOS=%s 不认（只认 interactive / initiated / utility），按 interactive", v);
    return q;
}
// 其余游戏线程 QoS 覆盖（S2A_GUEST_QOS=interactive|initiated|utility，缺省不设 = 按性能核数定）：10-23 暗黑3 真机条件变量交接
//   「唤醒 → 回到游戏」有 0.056% 拖到 1–3 ms（和 Mac 同量级），疑为 iPhone 上 USER_INITIATED 的被唤醒线程排在 USER_INTERACTIVE 线程后面；同包 A/B 用
static qos_class_t other_qos(void) {
    static qos_class_t q; static int done; if (done) return q;
    const char *v = getenv("S2A_GUEST_QOS"); q = QOS_CLASS_UNSPECIFIED;
    if (v && !strcmp(v, "interactive")) q = QOS_CLASS_USER_INTERACTIVE;
    else if (v && !strcmp(v, "initiated")) q = QOS_CLASS_USER_INITIATED;
    else if (v && !strcmp(v, "utility")) q = QOS_CLASS_UTILITY;
    else if (v && *v) LOG("【红】S2A_GUEST_QOS=%s 不认（只认 interactive / initiated / utility），按缺省", v);
    done = 1; return q;
}
// 核掩码只给 3 号核的游戏线程降 UTILITY：v0.3.1 起缺省关（S2A_SYSCORE_QOS=utility 才开旧行为）。
//   11-12 一款 Switch 2 游戏 0400486024DEC000（M5 Max）：游戏把工作线程挪到 3 号核再挪回，宿主跟着在 USER_INTERACTIVE / UTILITY 之间切，
//   UTILITY 那条被排到能效核、低优先级，等这组工作线程的游戏主逻辑被拖住，>100 ms 长帧全在宿主外、GPU 只忙四成。
//   「3 号核 = Horizon 系统核」只是 Switch 1 的约定，Switch 2 不成立，Switch 1 游戏放在 3 号核的也常是要按帧做完的活。
//   每次现读环境（不缓存）：线程起 / 改核掩码时才调，A/B 与单测同进程切换
//   按性能核数分（11-12 手机 A/B：Duo 上同档后帧率、p99 持平、能耗约 +5%，没有 Mac 的改善）：性能核 ≥ 4（Mac、M 系列 iPad，同 S2A_QOS_GAME_OTHER 的判法）同档，
//   不足 4 个（iPhone）维持 UTILITY——性能核少时多一条 USER_INTERACTIVE 线程只是多抢那两个性能核。S2A_SYSCORE_QOS=utility / same 强制旧 / 新行为（A/B 用）
static int syscore_utility(void) {
    const char *v = getenv("S2A_SYSCORE_QOS");
    if (v && !strcmp(v, "utility")) return 1;
    if (v && !strcmp(v, "same")) return 0;
    return perf_cores() < 4;
}
static qos_class_t guest_qos(int main_thread, uint64_t mask) {
    if (main_thread) return main_qos();
    if (mask == 1ull << 3 && syscore_utility()) return s2a_qos(S2A_QOS_GAME_SYSCORE);
    if (other_qos() != QOS_CLASS_UNSPECIFIED) return other_qos();
    return s2a_qos(S2A_QOS_GAME_OTHER);   // 性能核 ≥ 4 USER_INTERACTIVE、< 4 USER_INITIATED（表内同一判法）
}
qos_class_t s2a_test_guest_qos(int main_thread, uint64_t mask) { return guest_qos(main_thread, mask); }   // 单测入口（t_thread.c）
// 设 QoS 并读回核对：设不上（如 .NET 建的线程 EPERM）或读回不符就报红一次
static void set_qos(qos_class_t want) {
    int rc = pthread_set_qos_class_self_np(want, 0); qos_class_t got = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &got, &rp);
    if (rc || got != want) { static _Atomic int n; if (!atomic_fetch_add(&n, 1)) LOG("【红】游戏线程 QoS 设 %#x 失败：rc=%d 读回 %#x", want, rc, got); }
}
#endif
static void thread_bind_host(thr_t *t) {
#if !defined(__APPLE__)
    // Publish pthread lifetime under the same roster lock as the VM census.
    glock(); t->pt = pthread_self(); pthread_mutex_unlock(&G);
#endif
    if (pthread_setspecific(g_keythr, t)) die("guest thread TSD 失败");
#if !defined(__APPLE__)
    // A child may have been created after the VM census. It must not initialize
    // any guest TLS/stack memory while the replacement epoch is active.
    android_vm_barrier(t);
#endif
    { char nm[24]; snprintf(nm, sizeof nm, "nx-%llu", (unsigned long long)t->id);
#if defined(__APPLE__)
    pthread_setname_np(nm);
#else
    pthread_setname_np(pthread_self(), nm);
#endif
 }   // 线程名 = 游戏线程 id（同线程表）：能耗 / sample 拆账直接对上游戏线程
#if defined(__APPLE__)
    t->mport = pthread_mach_thread_np(pthread_self());
#endif
    t->tsd = plat_tsd_base();
    s2a_crash_thread_init();
    guest_thread_init(t);
}
static void svc_exit(thr_t *t);
static void samecore_init(void);
#if defined(__ANDROID__)
extern void s2a_capacity_guest_enter(void) __attribute__((weak));
#endif
static void *thread_main(void *p) {
    thr_t *t = p;
#if defined(__APPLE__)
    set_qos(guest_qos(0, t->mask));
#endif
    thread_bind_host(t);
    glock(); while (!t->started && !atomic_load(&g_session_stop)) thread_condition_wait(t, &t->cv, NULL); pthread_mutex_unlock(&G);
    // 游戏代码跑在游戏给的栈上（同 Horizon：游戏按「指针在不在某线程栈里」做决定）；宿主代码经切栈桩回本 pthread 栈（xstack.c）
    int hostent = sw_is_host_code(t->entry);
    uint64_t here = ((uint64_t)__builtin_frame_address(0) - 0x400) & ~0xfull;
    sw_xstack_thread_init(here, hostent ? 0 : t->stack_top);
    s2a_thread_stop_point();
#if (defined(__ANDROID__) || defined(__OHOS__))
    int s2a_android_runtime_enter_guest_thread(void);
    if (s2a_android_runtime_enter_guest_thread()) die("Android guest线程准入失败");
    if (!hostent && s2a_capacity_guest_enter) s2a_capacity_guest_enter();
#endif
    // S2A_CORES：进游戏代码前先取核（同 Horizon：线程被调度上核才开始跑）；原先到第一次系统调用才取，期间不持核在跑、同核可 ≥2（调度计数 / t_thread ⑯ 抓到）。
    // 走系统调用出口同一条路：等核期间算在系统调用里（暂停方不去 Mach 挂起它），出来前认暂停
    if (!hostent) svc_exit(t); else atomic_store(&t->in_svc, 0);
#ifndef S2A_UNIT_TEST
    { void s2a_det_set_ord(int); s2a_det_set_ord((int)(t->id - 0x51)); s2a_det_acquire(); }   // 确定性档：游戏线程串行（序号 = 创建先后，主线程 0）
#endif
    sw_thread_enter(t->entry, t->arg, hostent ? here : t->stack_top);   // 不返回：游戏须经 ExitThread 退出
    die("游戏线程入口返回");
}
// 游戏线程宿主栈放窗口尾部（10-24 低区让给 GC）：栈是自己映射的，线程退出时 pthread 不替我们还，要等线程真正结束——
//   线程改为可汇合（joinable），退出前把 (pthread, 栈) 登记进待收表；下次建线程时逐个 pthread_join（返回即栈不再被用）后还给尾部
#define HSTK_SIZE (1u << 20)
#define HSTK_GUARD 16384u
static pthread_mutex_t g_reap_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_reap_cv = PTHREAD_COND_INITIALIZER;
enum { REAP_MAX = 1024 };
enum reap_state { REAP_EMPTY, REAP_PENDING, REAP_JOINING, REAP_FAILED };
static struct { pthread_t pt; void *stk; enum reap_state state; } g_reap[REAP_MAX];
static int g_nreap; // Includes pending, joining and failed records.
static int g_reap_failed;
static int hstk_reap(int drain) {
    for (;;) {
        pthread_mutex_lock(&g_reap_mu);
        if (g_reap_failed || !g_nreap) { int result = g_reap_failed ? -1 : 0; pthread_mutex_unlock(&g_reap_mu); return result; }
        int i = REAP_MAX;
        while (i && g_reap[i - 1].state != REAP_PENDING) i--;
        if (!i) { // Only session retirement must also wait for other reapers.
            if (drain) pthread_cond_wait(&g_reap_cv, &g_reap_mu);
            pthread_mutex_unlock(&g_reap_mu);
            if (!drain) return 0;
            continue;
        }
        i--;
        pthread_t pt = g_reap[i].pt; void *stk = g_reap[i].stk;
        g_reap[i].state = REAP_JOINING;
        pthread_mutex_unlock(&g_reap_mu);
        // Keep the claimed record until join and stack release finish, without
        // holding the enqueue lock while the target runs its TLS destructors.
        int rc = pthread_join(pt, NULL);
        if (rc) {
            pthread_mutex_lock(&g_reap_mu);
            g_reap[i].state = REAP_FAILED; g_reap_failed = 1;
            pthread_cond_broadcast(&g_reap_cv); pthread_mutex_unlock(&g_reap_mu);
            LOG("【红】线程回收 pthread_join 失败：%d，保留记录和宿主栈", rc); return -1;
        }
        if (stk) s2a_tail_unmap(stk, HSTK_SIZE + HSTK_GUARD, S2A_TAIL_STACK);
        pthread_mutex_lock(&g_reap_mu);
        g_reap[i] = (typeof(g_reap[0])){0}; g_nreap--;
        pthread_cond_broadcast(&g_reap_cv); pthread_mutex_unlock(&g_reap_mu);
    }
}
#ifdef S2A_UNIT_TEST
void (*s2a_test_thr_freed)(void *t);       // 线程对象真被释放时（t_thread「暂停中不释放」）
void (*s2a_test_after_lookup)(void *t);   // 苹果 0x32 查完句柄、放开 G 之后（同上：在这一刻让目标退出并关句柄）
void (*s2a_test_svc_leave)(void *t);      // svc_exit 取完核、清 in_svc 之前（t_thread P1-49：在这一刻发暂停）
void (*s2a_test_pause_suspended)(void *t);   // 苹果暂停方 Mach 挂住目标、读到停在游戏代码之后、发布之前（t_thread P1-49：在这一刻恢复目标）
#endif
static void thr_free(thr_t *t) {   // 持 G。被系统调用钉着（pins）就不放：最后一个减钉的放（审计 #859 F-apple-pause-nopin：苹果原先不看钉）
    if (t->pins) return;
#ifdef S2A_UNIT_TEST
    if (s2a_test_thr_freed) s2a_test_thr_freed(t);
#endif
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    pthread_cond_destroy(&t->cv); free(t);
}
static void core_release(thr_t *t);
#if defined(__APPLE__)
static void wg_leave_self(void);   // 帧间隔工作组：线程退出前离开（见 wg_join_self）
#endif
#if (defined(__ANDROID__) || defined(__OHOS__))
extern void s2a_capacity_leave(int) __attribute__((weak));
#endif
static void thread_exit(thr_t *t) {
#if (defined(__ANDROID__) || defined(__OHOS__))
    if (t->is_main && s2a_capacity_leave) s2a_capacity_leave(0);
#endif
#if defined(__APPLE__)
    wg_leave_self();   // 必须在本线程、退出之前：带着工作组退出的线程会被系统库当场中止整个进程（10-30 丝之歌 S2A_WORKGROUP=all 真机两局无日志退出、Mac 复现 SIGILL）
    // 这里不打日志：退出发生在游戏线程自己的栈上（有的只有十几 KiB），带浮点的 printf 会把栈撑爆（旧库真机冒烟 SIGBUS）
    core_release(t);   // 0x0A 里 svc_exit 会重取核：退出前放开，否则带着核退出、同核线程永远等（S2A_CORES 单测抓到）
    s2a_cs_thread_exit(t->handle);
    glock();
    t->exited = 1; atomic_fetch_add_explicit(&g_soft_keeps, atomic_exchange(&t->soft_keeps, 0), memory_order_relaxed); wake_sync_waiters();
    pthread_setspecific(g_keythr, NULL);
    void *tlr = t->tlr; t->tlr = NULL; void *hstk = t->hstk;
    if (t->closed) thr_free(t);
    pthread_mutex_unlock(&G);
    s2a_tlr_free(tlr);
    sw_xstack_thread_fini();
    plat_fault_altstack_free();   // 信号备用栈还回（10-24 前退出线程的备用栈一直留着：Mac xc3 第 180 秒 128 个 17 MB）
#ifndef S2A_UNIT_TEST
    s2a_det_release();
#endif
    {   // 主线程和系统分配栈也必须 join，等待 NativeAOT TLS 析构结束。
        for (;;) {
            pthread_mutex_lock(&g_reap_mu);
            if (g_nreap < REAP_MAX) {
                int i = 0; while (g_reap[i].state != REAP_EMPTY) i++;
                g_reap[i] = (typeof(g_reap[0])){pthread_self(), hstk, REAP_PENDING}; g_nreap++;
                pthread_cond_broadcast(&g_reap_cv); pthread_mutex_unlock(&g_reap_mu);
                break;
            }
            pthread_mutex_unlock(&g_reap_mu);
            // A join error stays latched with its record. Exit cannot return an
            // error or drop this thread: retain its live count until a slot frees.
            (void)hstk_reap(0);
            pthread_mutex_lock(&g_reap_mu);
            while (g_nreap == REAP_MAX) pthread_cond_wait(&g_reap_cv, &g_reap_mu);
            pthread_mutex_unlock(&g_reap_mu);
        }
    }
    atomic_fetch_sub(&g_live_threads, 1);
    pthread_exit(NULL);
#else
    // 这里不打日志：退出发生在游戏线程自己的栈上（有的只有十几 KiB），带浮点的 printf 会把栈撑爆（旧库真机冒烟 SIGBUS）
    core_release(t);   // 0x0A 里 svc_exit 会重取核：退出前放开，否则带着核退出、同核线程永远等（S2A_CORES 单测抓到）
    s2a_cs_thread_exit(t->handle);
    // Finish guest-memory cleanup before publishing exited to the VM census.
    void *tlr = t->tlr; t->tlr = NULL;
    s2a_tlr_free(tlr);
#if !defined(__APPLE__)
    if (s2a_android_fault_thread_fini()) die("Android fault state still active at exit");
#endif
    sw_xstack_thread_fini();
    glock();
    t->exited = 1; atomic_fetch_add_explicit(&g_soft_keeps, atomic_exchange(&t->soft_keeps, 0), memory_order_relaxed); wake_sync_waiters();
    pthread_setspecific(g_keythr, NULL);
    if (t->closed) thr_free(t);
    pthread_mutex_unlock(&G);
#ifndef S2A_UNIT_TEST
    s2a_det_release();
#endif
    pthread_exit(NULL);
#endif
}
void s2a_thread_stop_point(void) {
    if (!atomic_load(&g_session_stop)) return;
    thr_t *t = thread_current_or_null();
    if (t && g_stop_boundary && g_stop_boundary()) thread_exit(t);
}
// Called on the host stack, outside managed/graphics frames, at a terminal
// guest path. The ordinary SVC boundary may still occupy its wrapper frame.
void s2a_thread_terminal_stop_point(void) {
    if (!atomic_load(&g_session_stop)) return;
    thr_t *t = thread_current_or_null();
    if (t) thread_exit(t);
}
#if defined(__APPLE__)
static void stop_tramp(void) { thread_exit(cur()); __builtin_unreachable(); }
#else
void sw_thread_returned(void) { die("游戏线程入口函数返回（Horizon 要求 ExitThread）"); }
#endif

// ---------- 帧间隔工作组（缺省关；S2A_WORKGROUP=1/render/all 可开）：iOS 游戏把每帧开始 / 截止交给系统 ----------
// 渲染线程（S2A.GPU，s2a_frame_presented 首次调时）与游戏线程加入同一个 os_workgroup_interval；渲染线程每次出图结束上一帧间隔、开始下一帧，
// 截止 = 现在 + 帧周期：缺省每帧取标称帧率（ap_host.c s2a_ap_nominal_fps，出图实际跟的节拍，60 帧表 / 帧步进 / 过热封顶切换都跟着变；还没报过按 30），
//   S2A_WG_PERIOD_MS 设了就固定用它（A/B 用）。系统性能控制器知道截止后可按时用低频 / 能效核做完。
// 10-30 真机（丝之歌，c956cf435 包，两台测试设备各一局）：all 档 60.0 帧、窗口 3600 帧慢帧 0、整机 2.78 / 2.99 W；同机未开（老包）5.24 W
// 工作组用 AudioToolbox 的 AudioWorkIntervalCreate 建（公开接口里唯一能建 interval 工作组的；底本旧库 ryu_judge FrameWorkgroup.cs）。任一步失败报红一次、本功能关
#if defined(__APPLE__)
static os_workgroup_interval_t g_wg; static uint64_t g_wg_period; static int g_wg_state;   // 0 未初始化 1 开 -1 关；g_wg_period 非 0 = S2A_WG_PERIOD_MS 固定周期
static mach_timebase_info_data_t g_wg_tb;
static __thread int t_wg_joined;
// 10-30 帧率能耗线分档（维护者 A/B）：S2A_WORKGROUP=1 出图线程 + 游戏主线程（原样）；=render 只有出图线程；=all 再加每条游戏线程
//   （它第一次进线程类系统调用时在自己线程上加入：os_workgroup_join 只能本线程调）。丝之歌：工作组开 P 核 1.9 → 3.5 GHz、慢帧仍 2.6%，
//   留在能效核约 1.4 GHz 的是没进组的 Unity 工作线程（nx-86/87/88），要看它们进组后慢帧动不动
static int g_wg_mode;   // 1 出图 + 主线程，2 只有出图，3 全部游戏线程
static int wg_on(void) {
    if (g_wg_state) return g_wg_state > 0;
    g_wg_state = -1;
    // 缺省关（10-30 丝之歌三条路线干净读数：只开短睡退避 1.69 W；再开工作组 all 2.47 W；工作组 + 退避压到 20 µs 4.83 W，都 60 帧、慢帧 0%）：
    //   工作组把线程钉在性能核高频，功耗只升不降；退避才是省电的那一半。S2A_WORKGROUP=1 / render / all 仍可开（对照用）
    const char *e = getenv("S2A_WORKGROUP"); if (!e || !*e) e = "0";
    g_wg_mode = !strcmp(e, "1") ? 1 : !strcmp(e, "render") ? 2 : !strcmp(e, "all") ? 3 : 0;
    if (!g_wg_mode) { LOG("帧间隔工作组：关（S2A_WORKGROUP=%s）", e); return 0; }
    const char *pm = getenv("S2A_WG_PERIOD_MS"); double ms = pm ? atof(pm) : 0;
    mach_timebase_info(&g_wg_tb); g_wg_period = ms > 0 ? (uint64_t)(ms * 1e6 * g_wg_tb.denom / g_wg_tb.numer) : 0;
    void *h = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_LAZY);
    typedef os_workgroup_interval_t (*mk_t)(const char *, os_clockid_t, os_workgroup_attr_t);
    mk_t mk = h ? (mk_t)dlsym(h, "AudioWorkIntervalCreate") : NULL;
    g_wg = mk ? mk("s2a.frame", OS_CLOCK_MACH_ABSOLUTE_TIME, NULL) : NULL;
    if (!g_wg) { LOG("【红】帧间隔工作组：AudioWorkIntervalCreate %s，本功能关", mk ? "返回空" : "找不到"); return 0; }
    g_wg_state = 1; LOG("帧间隔工作组：开（%s：%s，帧周期 %s）", e, g_wg_mode == 1 ? "出图线程 + 游戏主线程" : g_wg_mode == 2 ? "只有出图线程" : "出图线程 + 全部游戏线程", g_wg_period ? pm : "跟标称帧率"); return 1;
}
static __thread os_workgroup_join_token_s t_wg_tok;   // 本线程加入时拿的令牌：离开必须由同一线程用同一令牌（os_workgroup_leave）
static void wg_join_self(const char *who) {
    if (t_wg_joined || !wg_on()) return;
    int rc = os_workgroup_join((os_workgroup_t)g_wg, &t_wg_tok);
    t_wg_joined = rc == 0 ? 1 : -1;
    if (rc) LOG("【红】帧间隔工作组：%s 加入失败 rc=%d", who, rc); else LOG("帧间隔工作组：%s 已加入", who);
}
static void wg_leave_self(void) {
    if (t_wg_joined != 1) return;
    os_workgroup_leave((os_workgroup_t)g_wg, &t_wg_tok); t_wg_joined = -1;   // -1：不再重新加入（线程正在退出）
}
// 每上屏一帧（ap_host.c s2a_frame_presented，渲染线程）：首次在本线程加入；结束上一帧间隔、开始下一帧。fps = 当前标称帧率（0 = 还没报过，按 30）
void s2a_wg_frame(int fps) {
    if (!wg_on()) return;
    if (!t_wg_joined) wg_join_self("渲染线程");
    if (t_wg_joined < 0) return;
    static int started;
    if (started) { int frc = os_workgroup_interval_finish(g_wg, NULL); if (frc) { static int n; if (!n++) LOG("【红】帧间隔工作组：interval_finish rc=%d", frc); } }
    uint64_t now = thread_clock(), per = g_wg_period;
    if (!per) { per = (uint64_t)(1e9 / (fps > 0 ? fps : 30) * g_wg_tb.denom / g_wg_tb.numer); }
    int rc = os_workgroup_interval_start(g_wg, now, now + per, NULL);
    if (rc) { LOG("【红】帧间隔工作组：interval_start rc=%d，本功能关", rc); g_wg_state = -1; return; }
    started = 1;
}

#else
void s2a_wg_frame(int fps) {
    (void)fps;
    // Apple-only energy hint; not a guest scheduling capability.
    static _Atomic int reported;
    const char *e = getenv("S2A_WORKGROUP");
    if (e && *e == '1' && !atomic_exchange(&reported, 1)) LOG("Android 不支持 Apple interval workgroup");
}
#endif

// 主线程登记，返回其句柄（nnosInitialize 的 x0）
#if (defined(__ANDROID__) || defined(__OHOS__))
extern void s2a_capacity_enter(int) __attribute__((weak));
extern void s2a_capacity_leave(int) __attribute__((weak));
#endif
uint32_t s2a_thread_main_init(void) {
    if (thread_keys_init()) die("pthread_key_create");
    thr_t *t = calloc(1, sizeof *t);
    pthread_cond_init(&t->cv, NULL);
    t->started = 1; t->prio = 44; t->core = 0; t->mask = 7; t->pt = pthread_self(); t->is_main = 1;
    if (hz_core()) { t->prio = g_main_prio; t->core = g_def_core; t->mask = 1ull << g_def_core; }
    t->base_prio = t->prio;
    glock(); t->handle = h_new(S2A_OBJ_THREAD, t, 0); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
    samecore_init();
    if (!t->handle) die("主线程句柄分配失败");
#if defined(__APPLE__)
    set_qos(guest_qos(1, 7));
    if (wg_on() && g_wg_mode != 2) wg_join_self("游戏主线程");
#endif
    thread_bind_host(t);
#if (defined(__ANDROID__) || defined(__OHOS__))
    if (s2a_capacity_enter) s2a_capacity_enter(0);
#endif
#if defined(__APPLE__)
    atomic_store(&g_join_verified, 0);
    atomic_fetch_add(&g_live_threads, 1);
    LOG("线程调度档：性能核 %d 个 → 游戏主线程 %s，其余游戏线程 %s", perf_cores(), main_qos() == QOS_CLASS_USER_INTERACTIVE ? "USER_INTERACTIVE" : main_qos() == QOS_CLASS_USER_INITIATED ? "USER_INITIATED" : "UTILITY", guest_qos(0, 7) == QOS_CLASS_USER_INTERACTIVE ? "USER_INTERACTIVE" : guest_qos(0, 7) == QOS_CLASS_USER_INITIATED ? "USER_INITIATED" : "UTILITY");
    { const char *v = getenv("S2A_SYSCORE_QOS"); LOG("线程调度档：核掩码只含 3 号核的游戏线程 %s（%s）", syscore_utility() ? "UTILITY" : "与其余游戏线程同档", v && (!strcmp(v, "utility") || !strcmp(v, "same")) ? "S2A_SYSCORE_QOS 强制" : perf_cores() >= 4 ? "性能核 ≥ 4 缺省" : "性能核不足 4 个缺省"); }
#endif
    return t->handle;
}

// ---------- 系统调用出入口（门帧登记、SetThreadActivity 的挂起点）----------
static pthread_cond_t g_pause_cv = PTHREAD_COND_INITIALIZER;
// 进后台冻结（iOS 失去前台，app.m）：全部游戏线程在系统调用出入口停住，回前台放开。与游戏自己的 SetThreadActivity 暂停（t->pause）各管各的，互不覆盖；
// 正在游戏代码里算的线程跑到下一次系统调用才停（不 Mach 挂起：停在宿主代码里可能持 malloc 锁，卡住主线程）
static _Atomic int g_frz;
static _Atomic int g_snap_frz;   // 即时存档冻结中（只在门里停：入口 / 出口 / 等待 / 睡眠）
static void preempt_init(void);   // 时钟抢占（S2A_CORES）：cores_on 首次开时注册信号 + 起核抢占看门狗
// ---------- 虚拟核（S2A_CORES=1，修干净清单 #1；帧率能耗线 10-20，底本旧库 sw/host/hle_thread.c:148）----------
// Horizon 上同一个核同一时刻只跑一条线程，换人只在阻塞 / 让出 / 被高优先级抢占时发生；任天堂系引擎依赖它（同核线程轮流用不带锁的数据）。
// 每核一把核锁：线程跑游戏代码时持有自己核的锁；进会阻塞的系统调用（睡眠 / 让出、等待、锁、条件、地址、IPC）前放开，返回游戏前按优先级重取。
// 也减宿主空转：让出写的自旋等待不再在宿主核上真并行地转（测试设备1 nx-129/130 各约 8,450 运行段/秒）。没做：就绪的高优先级线程抢占正在跑的低优先级线程
#define NCORE 4
// 线程可在核掩码里任一核上跑（Horizon 调度器会把就绪线程放到掩码内空闲核）：理想核空着优先，否则取掩码内任一空闲核；
// 某核有更高优先级（数值更小）且也能用此核的线程在等，就让给它。全局一把锁：取 / 放 / 代放（暂停）都在它下面。
// 掩码内没有空核时做空闲核迁移（同 Horizon KScheduler::UpdateHighestPriorityThreadsImpl：空闲核把别核正在跑、掩码也含本核的线程拿过来，
// 条件是那个核上另有线程等着跑）：占着本线程所需核、自己掩码里另有空核的持有者挪到那个空核，所需核让给本线程。虚拟核只是记账，挪核不打断持有者运行。
// 10-20 八方2 开虚拟核：渲染线程（掩码只含 2 号核）等核，2 号核被掩码 0~2 的异步加载线程占着空转等它（0、1 号核空着）→ 互等，游戏卡死检测自杀
// 10-20 九款回归：原「只认一个核」把掩码 0~2 的线程全挤到 0 号核，暗黑2 主线程占 0 号核空转等工作线程、工作线程排 0 号核 → 只出 4 帧
static pthread_mutex_t g_cmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_ccv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_wd_cv = PTHREAD_COND_INITIALIZER;   // 抢占看门狗：没人等核时睡在这里，有线程开始等核时叫醒（core_acquire）
// g_cown_t：该核被现持核者拿到的时刻（59 / 63 时间片轮转用）
static thr_t *g_cown[NCORE]; static uint64_t g_cown_t[NCORE]; enum { CW_MAX = 1024 }; static thr_t *g_cw[CW_MAX]; static int g_ncw;
// 历史：S2A_CORES 10-20 引入（缺省关）→ 10-23 晚翻开又翻回（巫师3 过场活锁，5a75546a 照 Horizon 修迁移）→ 10-24 午翻开 → 10-24 晚缺省关（见下）
// 模式（10-24 晚，维护者 / 维护者定去掉完整虚拟核，docs/研究/虚拟核与轻量调度语义.md §6–8）：缺省关——游戏线程即宿主线程、不限同核并发；
//   让步 / 自旋的能耗由让步退避与自旋闸管，同核高优先级唤醒让先单独开（S2A_PEER_YIELD）。Mac 怪猎同包：关 0 崩到 MARK（同核读核号并发 671 次，155 局历史无一崩溃可归因）、
//   按需 0 崩（并发 0）——按维护者口径「全关 0 崩、计数无害即全关」。S2A_CORES=1 全部参与（10-20 起的完整虚拟核），可选「按需」（S2A_CORES=ondemand）——调过 GetCurrentProcessorNumber（0x10）或掩码只含一个核的线程参与核互斥：
//   Horizon 同核同优先级不互相抢占，游戏据此写「读核号 → 改本核无锁数据」；没读过核号的线程写不出按核数据，自由跑（省掉全员等核 / 交核，直连同包 −1 W 量级）。
//   按需曾在 Mac 怪猎 3 局都在每帧 700+ 渲染遍后 Metal 驱动中止，渲染器命令缓冲上限（fix/cb-pass-cap）修好后同场景 0 崩
// 11-19 维护者定：苹果平台缺省改「按需」。Switch 上同一核同一时刻只跑一条线程，游戏据此「读核号 → 改本核数据」不加锁；缺省关时这个前提每局都被打破
//   （王国之泪 Mac 两局「读过核号的两线程同核同跑」129 / 161 次，按需档 0 次、帧数持平；8 GB Mac 上沿 +0x18 父指针读到正被改写的节点而崩，#9113f9）。
//   显式设的照旧优先：S2A_CORES=0 关、=1 全部参与、=ondemand 按需。安卓侧缺省不变（另行评估）
#if defined(__APPLE__)
enum { CORES_DEFAULT = 1 };
#else
enum { CORES_DEFAULT = 0 };
#endif
static int cores_mode(void) { static int v = -1; if (v < 0) { const char *e = getenv("S2A_CORES");
    v = !e || !*e ? CORES_DEFAULT : !strcmp(e, "1") ? 2 : !strcmp(e, "ondemand") ? 1 : 0;
    LOG("虚拟核：%s%s", v == 2 ? "全部游戏线程参与（S2A_CORES=1）" : v == 1 ? "按需（读过核号 0x10 或钉单核的线程同核互斥）" : "关（游戏线程即宿主线程、不限同核并发）",
        !e || !*e ? "——缺省；S2A_CORES=0 关、=1 全部参与、=ondemand 按需" : "——S2A_CORES 显式设");
    if (v) preempt_init(); } return v; }
static int cores_on(void) { return cores_mode() != 0; }
// 按需参与者：读过核号（0x10），或掩码只含一个核——钉在单核上的线程不用问也知道自己在哪个核，同样能写按核数据
//   （10-24 Mac 怪猎：只认 0x10 时两局都在第 689 / 742 帧前后每帧 700+ 渲染遍、命令分配器涨到 540 / 738 MB 后 Metal 中止；全部参与 / 关都没有）
static int core_part(const thr_t *t) { int m = cores_mode(); if (m != 1) return m == 2; uint64_t k = t->mask & ((1ull << NCORE) - 1); return atomic_load(&t->percore) || (k && !(k & (k - 1))); }
static int core_of(const thr_t *t) { int c = t->core; if (t->mask && (c < 0 || !(t->mask >> c & 1))) c = __builtin_ctzll(t->mask); return c < 0 || c >= NCORE ? 0 : c; }
static unsigned core_mask(const thr_t *t) { unsigned m = (unsigned)(t->mask & ((1u << NCORE) - 1)); return m ? m : 1u << core_of(t); }
// 持 g_cmu：有没有别的等核线程比 t 优先级高、也能用 c 号核
static int core_claimed(int c, const thr_t *t) {
    int ti = -1; for (int i = 0; i < g_ncw; i++) if (g_cw[i] == t) { ti = i; break; }
    for (int i = 0; i < g_ncw; i++) {
        const thr_t *w = g_cw[i]; if (w == t || !(core_mask(w) >> c & 1)) continue;
        if (w->prio < t->prio) return 1;                                      // 更高优先级（数值更小）也能用此核、在等
        if (t->yielded && w->prio == t->prio && ti >= 0 && i < ti) return 1;  // 本线程让出：同优先级、登记更早的先拿（轮转）
    }
    return 0;
}
// 持 g_cmu：有没有别的等核线程能直接用 c 号核（空核先给等着的线程，同 Horizon 空闲核先取就绪队列里的线程，取不到才迁移正在跑的）
static int core_wanted(int c, const thr_t *t) {
    for (int i = 0; i < g_ncw; i++) if (g_cw[i] != t && (core_mask(g_cw[i]) >> c & 1)) return 1;
    return 0;
}
static int core_pick(thr_t *t) {   // 持 g_cmu；返回可取的核或 -1（可能把别的持核线程挪到空核上）
    unsigned m = core_mask(t); int ideal = core_of(t);
    for (int k = -1; k < NCORE; k++) {
        int c = k < 0 ? ideal : k; if (!(m >> c & 1) || g_cown[c]) continue;
        if (!core_claimed(c, t)) return c;
    }
    // 宿主侧可借的核（s2a_core_leave 没人等时只置可借、不放）：持核者此刻在宿主代码里、不跑客体，CAS 1 → 2 抢到就当空核用；
    //   持核者回客体时 CAS 1 → 0 失败，走下面的取核慢路（同原先「进宿主就放核」的结果）。两边只有一方 CAS 得手：同核客体代码仍只有一条在跑
    for (int k = -1; k < NCORE; k++) {
        int c = k < 0 ? ideal : k; thr_t *o = g_cown[c]; int sv = 1;
        if (!(m >> c & 1) || !o || o == t || core_claimed(c, t)) continue;
        if (!atomic_compare_exchange_strong(&o->csoft, &sv, 2)) continue;
        g_cown[c] = NULL; o->held_core = 0; atomic_fetch_add_explicit(&g_soft_steals, 1, memory_order_relaxed);
        return c;
    }
    // 空闲核迁移：Horizon 不迁优先级数值 < 2 的线程（HighestCoreMigrationAllowedPriority）
    for (int k = -1; k < NCORE; k++) {
        int c = k < 0 ? ideal : k; thr_t *o = g_cown[c];
        // 迁走持核者不看它的优先级（Horizon：空闲核把「核上还有人等着」的持核者拉过去，被迁者照跑、只换核，不吃亏）。
        //   10-23 曾加「不迁走比自己优先级高的持核者」：连迁到空闲核也拦，巫师3 过场活锁——优先级 32、掩码 0x7 的 H 持 1 号核自旋，
        //   只能用 1 号核的 W（优先级 44）永远等、0 / 2 号核空着（Mac 与测试设备2 现场同构，t_thread ⑳）。下面只往空闲、且没有别的等待者
        //   要的核迁（core_wanted），H 不会被挤到「还得排队」的核上；更高优先级的等待者认领 c 时不迁（core_claimed）。
        if (!(m >> c & 1) || !o || o->prio < 2 || core_claimed(c, t)) continue;
        unsigned om = core_mask(o);
        for (int c2 = 0; c2 < NCORE; c2++) {
            if (c2 == c || !(om >> c2 & 1) || g_cown[c2] || core_wanted(c2, t)) continue;
            g_cown[c2] = o; o->held_core = c2 + 1; g_cown[c] = NULL;
            return c;
        }
    }
    return -1;
}
static void core_release_locked(thr_t *t) {   // 持 g_cmu：清持有登记并叫醒等核者
    { int sv = 1; atomic_compare_exchange_strong(&t->csoft, &sv, 2); }   // 可借着就记「已取走」：持核者回客体时 CAS 1 → 0 必失败、走取核慢路
    int h = t->held_core; if (h) { g_cown[h - 1] = NULL; t->held_core = 0; pthread_cond_broadcast(&g_ccv); }
}
static void core_release(thr_t *t) {
    if (!t->held_core) return;
    // 持核号在锁下重读：别的线程可能刚把 t 迁到另一核（只会从非零改成非零）
    pthread_mutex_lock(&g_cmu); core_release_locked(t); pthread_mutex_unlock(&g_cmu);
}
// 信号处理器专用（时钟抢占宿主分支）：抢不到 g_cmu 就跳过，由看门狗 0.5 ms 后重来。
//   不能在信号里 pthread_mutex_lock：g_cmu 是 PTHREAD_MUTEX_NORMAL（非递归），本线程正持锁时收信号
//   = 自己等自己 = 永久死锁，且不定时报 EDEADLK；即便没持锁，信号里等 g_cmu 也会和「持 g_cmu 等 G /
//   持 G 等 g_cmu」的宿主锁序互等（thread.c:895 的约束）。trylock 只在这几微秒的临界区里失败，重试即得。
//   返回 1 = 这次放掉了核。
static int core_release_try(thr_t *t) {
    if (!t->held_core) return 0;
    if (pthread_mutex_trylock(&g_cmu)) return 0;
    core_release_locked(t);
    pthread_mutex_unlock(&g_cmu);
    return 1;
}
// 不进锁、不碰共享计数就能确定「已持核且掩码容得下」→ 1（宿主侧可借着就 CAS 1 → 0 拿回；被取走 / 不持 / 掩码变了 → 0，走 core_acquire 的锁路）
static int core_fast(thr_t *t) {
    int sv = atomic_load(&t->csoft);
    if (sv == 2 || (sv == 1 && !atomic_compare_exchange_strong(&t->csoft, &sv, 0))) return 0;
    int h = atomic_load(&t->held_core);
    if (!h || !(core_mask(t) >> (h - 1) & 1)) return 0;
    if (sv == 1) atomic_store_explicit(&t->soft_keeps, atomic_load_explicit(&t->soft_keeps, memory_order_relaxed) + 1, memory_order_relaxed);   // 只本线程写：读改写不必是原子 RMW（不争缓存行）
    return 1;
}
static void core_acquire(thr_t *t) {   // 不持 G 调：持 G 等核会和持核线程进系统调用互相等死
#if !defined(__APPLE__)
    android_vm_barrier(t);
#endif
    if (!core_part(t)) return;
    if (core_fast(t)) return;
    if (atomic_load(&t->csoft) == 2) { pthread_mutex_lock(&g_cmu); atomic_store(&t->csoft, 0); pthread_mutex_unlock(&g_cmu); }   // 被取走：等取走方在 g_cmu 下改完持核登记再往下（held_core 已是 0）
    int h = t->held_core; if (h && (core_mask(t) >> (h - 1) & 1)) return;
    core_release(t);   // 换了掩码（SetThreadCoreMask）：先放旧核
    pthread_mutex_lock(&g_cmu);
    if (g_ncw < CW_MAX) { g_cw[g_ncw] = t; __atomic_store_n(&g_ncw, g_ncw + 1, __ATOMIC_SEQ_CST); }   // 先登记再挑核（core_pick 读可借位），与 s2a_core_leave「先置可借再读 g_ncw」成对
    if (g_ncw == 1) pthread_cond_signal(&g_wd_cv);   // 第一个等核者：叫醒空闲的抢占看门狗
    int c; atomic_store(&t->wcore, 1);
    while ((c = core_pick(t)) < 0 && !atomic_load(&g_session_stop)) pthread_cond_wait(&g_ccv, &g_cmu);
    atomic_store(&t->wcore, 0);
    for (int i = 0; i < g_ncw; i++) if (g_cw[i] == t) { g_cw[i] = g_cw[g_ncw - 1]; __atomic_store_n(&g_ncw, g_ncw - 1, __ATOMIC_SEQ_CST); break; }   // g_ncw 有锁外读者（s2a_core_leave）：改一律原子
    if (c >= 0) { g_cown[c] = t; g_cown_t[c] = now_ns(); t->held_core = c + 1; t->yielded = 0; }
    // 还有别的线程在等：让它们按新局面重挑（本线程可能拿了别人在等的核以外的核，空核 / 可迁移的持有者跟着变了）
    if (g_ncw) pthread_cond_broadcast(&g_ccv);
    pthread_mutex_unlock(&g_cmu);
#if !defined(__APPLE__)
    // wcore was a census-safe wait. Recheck after clearing it, before returning
    // to a caller that can access guest memory.
    android_vm_barrier(t);
#endif
}
// ⑭（纯函数）：H（高 prio 40）持核 0、掩码核 0/1，L（低 prio 48）只能用核 0，核 1 空。
// Horizon：空闲核 1 把「核 0 上还有 L 等着」的 H 拉过去（被迁者照跑、只换核），L 上核 0。
// 返回 1 = L 拿到核 0 且 H 改持核 1；0 = 不符（10-23 前的期望是「挡住 L」，与 Horizon 相反，巫师3 过场活锁即此规则所致）
int s2a_test_migrate_guard(void) {
    if (!cores_on()) return 1;
    pthread_mutex_lock(&g_cmu);
    thr_t *so[NCORE]; for (int c = 0; c < NCORE; c++) so[c] = g_cown[c];
    int sncw = g_ncw; static thr_t *scw_save[CW_MAX]; for (int i = 0; i < sncw; i++) scw_save[i] = g_cw[i];
    for (int c = 0; c < NCORE; c++) g_cown[c] = NULL;
    static thr_t H, L; H = (thr_t){0}; L = (thr_t){0};
    H.prio = 40; H.mask = 0x3; H.held_core = 1;   // H（高）持核 0，掩码核 0/1
    L.prio = 48; L.mask = 0x1;                      // L（低）只能用核 0
    g_cown[0] = &H; g_ncw = 0; g_cw[g_ncw++] = &L;
    int c = core_pick(&L);
    for (int cc = 0; cc < NCORE; cc++) g_cown[cc] = so[cc]; g_ncw = sncw; for (int i = 0; i < sncw; i++) g_cw[i] = scw_save[i];
    pthread_mutex_unlock(&g_cmu);
    int moved = H.held_core == 2;   // core_pick 迁走 H 时把它的持核号改成核 1（下标 + 1）
    return c == 0 && moved;
}
// ---------- 时钟抢占（S2A_CORES；修干净清单 #2：实现「就绪高优先级抢占正在跑的低优先级」，覆盖纯自旋不走 svc 的客体线程）----------
// 客体是 arm64 直接在宿主 arm64 跑（无翻译层可插检查点），纯自旋（不走 svc、不让核）只能异步信号抢占。
// 「最后一跳」（信号打断客体任意指令、让核完无损跳回那点）：AArch64 无内存间接跳转、任意寄存器都可能含客体活值不能破坏，
// setcontext 从信号 mcontext 恢复在 Mac 崩 SIGBUS；改用二次信号 + 内核 sigreturn（scratchpad/sigret_probe.c 验证 206 次往返全对、寄存器完好）。
// 不在信号里碰 g_cmu（会和宿主锁死锁）：信号只改 PC / 置标志，core_release+acquire 在信号返回后的 tramp（普通上下文）里做。
#define S2A_SIG_PREEMPT SIGURG       // SIGUSR2 被 det.c 确定性看门狗占
// ⑫ gate-host 偶发失效诊断（临时）：watchdog 最后扫时间、SIGURG 发出 / 送达计数、on_preempt 最后目标 PC 与客体区命中。
static _Atomic uint64_t g_wd_scans;   // 看门狗主动扫描次数（单测 ㉒：没人等核时不该涨）
uint64_t s2a_test_wd_scans(void) { return atomic_load(&g_wd_scans); }
static _Atomic uint64_t g_wd_last_ns, g_sigurg_sent, g_sigurg_recv, g_preempt_last_pc; static _Atomic int g_preempt_last_guest;
// 单测开关（生产恒 1）：关掉 = 复现修复前的行为，给 t_thread ㉗ 做负对照——判据能抓住「没修」才有意义
static _Atomic int g_t_preempt_consume = 1, g_t_preempt_host_rel = 1;
#ifdef S2A_UNIT_TEST
void s2a_test_preempt_req_consume(int on) { atomic_store(&g_t_preempt_consume, on != 0); }
void s2a_test_preempt_host_release(int on) { atomic_store(&g_t_preempt_host_rel, on != 0); }
#endif
#if defined(__APPLE__)
static void preempt_tramp(void) {    // 客体区被抢：信号返回后在普通客体上下文跑这里（纯 C、不持宿主锁）
    thr_t *me = thread_current_or_null();
    if (me) {
        sigset_t block, old; sigemptyset(&block); sigaddset(&block, S2A_SIG_PREEMPT);
        pthread_sigmask(SIG_BLOCK, &block, &old);      // 让核期间屏蔽抢占信号：tramp 不被二次抢占重入打断，保 porig 不被覆盖 / 不叠加
        atomic_store(&me->off_guest, 1); core_release(me); core_acquire(me); atomic_store(&me->off_guest, 0);   // 让核 + 按优先级重取（高优先级等核者此间上核）
        atomic_store(&me->restoring, 1);
        pthread_kill(me->pt, S2A_SIG_PREEMPT);         // 触发二次信号（此刻被屏蔽、挂起）覆盖 mcontext 回原
        sigset_t wait = old; sigdelset(&wait, S2A_SIG_PREEMPT);
        sigsuspend(&wait);                             // 原子解屏蔽抢占信号 + 等它递送：二次信号内核 sigreturn 恢复原执行流（不返回这）
    }
    for (;;) pause();
}
static void on_preempt(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si;
    thr_t *me = thread_current_or_null(); if (!me) return;
    ucontext_t *uc = (ucontext_t *)ctx;
    if (atomic_load(&me->restoring)) {                 // 二次信号：覆盖 mcontext 回原，正常返回 → 内核无损恢复
        // 掩码也要回原：二次信号是在 tramp 的 sigsuspend 里递送的，帧里存的是 sigsuspend 之前 tramp 屏蔽抢占信号的掩码，
        // sigreturn 按 uc_sigmask 恢复——不改的话线程从此屏蔽抢占信号，再也抢不动，它新建的线程也继承（t_thread ⑱；⑫ 偶发「SIGURG 发 1 达 0」根因）
        *uc->uc_mcontext = me->porig; uc->uc_sigmask = me->pmask; atomic_store(&me->restoring, 0); atomic_store(&me->preempting, 0); return;
    }
    uint64_t pc = uc->uc_mcontext->__ss.__pc;
    if (atomic_load(&g_session_stop)) {
        uint64_t sp = g_stop_sp ? g_stop_sp() : 0;
        if (sp && module_of((void *)pc)) {
            // 正常 sigreturn 后在宿主栈退出，不能从信号处理里杀线程。
            uc->uc_mcontext->__ss.__pc = (uint64_t)(uintptr_t)stop_tramp;
            uc->uc_mcontext->__ss.__sp = sp;
            uc->uc_mcontext->__ss.__fp = 0;
            uc->uc_mcontext->__ss.__lr = 0;
        }
        return;
    }
    atomic_fetch_add(&g_sigurg_recv, 1); atomic_store(&g_preempt_last_pc, pc); atomic_store(&g_preempt_last_guest, module_of((void *)pc) ? 1 : 0);   // ⑫ 诊断：SIGURG 送达、目标 PC、是否客体区
    if (module_of((void *)pc)) {                       // 客体区：改 PC 到 tramp（信号返回后让核）
        me->porig = *uc->uc_mcontext; me->pmask = uc->uc_sigmask;   // 被抢前的掩码（二次信号返回时写回）
        uc->uc_mcontext->__ss.__pc = (uint64_t)(uintptr_t)preempt_tramp;
        // tramp 是 C 代码、跑在被抢线程的游戏栈上：sp 先往下让出 256 字节再进。改写器补丁区的桩把临时寄存器存在 sp 下方不挪 sp
        //   （stp x0, x2, [sp, #-0x10] … ldp x0, x2, [sp, #-0x10]，红区用法），tramp 的栈帧若压在原 sp 上会盖掉它们（10-23 虚拟核缺省开后 Mac 塞尔达
        //   两局 SIGBUS：x2 被读成 lr、拿去当偏移）。原 sp 随 porig 由二次信号恢复
        uc->uc_mcontext->__ss.__sp = (uc->uc_mcontext->__ss.__sp - 256) & ~(uint64_t)15;
    } else {                                           // 宿主区（svc 处理中 / 宿主代码里）
        // 置标志（svc_exit 是它的主场消费点），并当场 trylock 放核——只在正处系统调用处理中（in_svc）时放：
        //   卡在 svc 里回不到 svc_exit 就是这个缺口的现场（preempt_req 没人消费 = 抢占 100% 无效）。
        //   in_svc=0 的宿主代码不放：那种状态本就不该持核（该申报 off_guest，见 s2a_core_block_begin），
        //   无条件放会让「持核者继续跑 + 等核者拿到同核」两条线程在游戏按核数据上真并发
        //   （t_thread ㉔ 转过：按需模式同核 RMW 丢 43 万次更新）。svc 处理内部不执行游戏自己的按核逻辑。
        //   这里只碰原子与 g_cmu 的 trylock：不持锁时不阻塞，正持锁时跳过、0.5 ms 后看门狗再来（见 core_release_try）。
        atomic_store(&me->preempt_req, 1);
        if (atomic_load(&g_t_preempt_host_rel) && atomic_load(&me->in_svc)) core_release_try(me);
        atomic_store(&me->preempting, 0);
    }
}
#else
static void pause_park(thr_t *t);
static int pc_in_guest_text(uint64_t pc);
static void android_preempt_resume(void *argument) {
    thr_t *me = argument;
    atomic_store(&me->off_guest, 1);
    core_release(me);
    // A pause request may arrive after SIGURG took its snapshot. Only the
    // target owns the context kind; a patch-stub PC is retried until real text.
    if ((atomic_load(&me->pause) || atomic_load(&g_frz)) && me->pmach) pause_park(me);
    android_vm_barrier(me);
    core_acquire(me);
    atomic_store(&me->off_guest, 0);
    atomic_store(&me->preempting, 0);
}
static void on_preempt(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si;
    thr_t *me = thread_current_or_null(); if (!me) return;
    ucontext_t *uc = ctx;
    uint64_t pc = uc->uc_mcontext.pc;
    atomic_store_explicit(&me->sig_pc, pc, memory_order_relaxed); atomic_store_explicit(&me->sig_lr, uc->uc_mcontext.regs[30], memory_order_relaxed);
    {
        const uint64_t sp0 = uc->uc_mcontext.sp; uint64_t fp = uc->uc_mcontext.regs[29];
        for (int k = 0; k < 4; ++k) {
            uint64_t ra = 0;
            if (fp >= sp0 && fp < sp0 + (1u << 20) && !(fp & 7)) { ra = ((const uint64_t *)(uintptr_t)fp)[1]; fp = ((const uint64_t *)(uintptr_t)fp)[0]; }
            else fp = 0;
            atomic_store_explicit(&me->sig_bt[k], ra, memory_order_relaxed);
        }
    }
    int guest = module_of((void *)(uintptr_t)pc) != NULL;
    if (atomic_load_explicit(&g_wd_enabled, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&me->pause_sig_seen, 1, memory_order_relaxed);
        if (!guest || atomic_load(&me->in_svc) || atomic_load(&me->off_guest))
            atomic_fetch_add_explicit(&me->pause_sig_native, 1, memory_order_relaxed);
    }
    atomic_fetch_add(&g_sigurg_recv, 1); atomic_store(&g_preempt_last_pc, pc); atomic_store(&g_preempt_last_guest, guest);
    if (!guest || atomic_load(&me->in_svc) || atomic_load(&me->off_guest)) {
        // Native/SVC lock owners must drain to a normal-context boundary.
        // 同 macOS：置标志给 svc_exit 消费，另当场 trylock 放核（宿主区不一定会回到那个边界）。
        atomic_store(&me->preempt_req, 1);
        if (atomic_load(&g_t_preempt_host_rel) && atomic_load(&me->in_svc)) core_release_try(me);
        atomic_store(&me->preempting, 0); return;
    }
    if (atomic_load(&android_vm_epoch) && atomic_load(&android_vm_owner) != me) android_vm_park(me);
    // Capture even ordinary preemption: pause can be requested before the
    // normal-context callback runs. Never export rewritten-stub scratch regs.
    me->pmach = pc_in_guest_text(pc);
    if (me->pmach) memcpy(&me->paused_context, uc, sizeof *uc);
    if (s2a_android_fault_redirect_call(ctx, android_preempt_resume, me) != 1) _exit(126);
}
#endif
// ⑫ gate-host 偶发失效诊断（临时）：访问函数（计数全局定义在 on_preempt 前）。
void s2a_test_preempt_diag(uint64_t *wd, uint64_t *sent, uint64_t *recv, uint64_t *pc, int *guest) {
    if (wd) *wd = atomic_load(&g_wd_last_ns); if (sent) *sent = atomic_load(&g_sigurg_sent);
    if (recv) *recv = atomic_load(&g_sigurg_recv); if (pc) *pc = atomic_load(&g_preempt_last_pc);
    if (guest) *guest = atomic_load(&g_preempt_last_guest);
}
// ---------- 宿主侧持核长跑：核 × 宿主锁 ABBA 的可检查性 ----------
// 调度计数只抓「不持核在跑」；持核线程在宿主 C 代码里长跑（持着核等 GPU / 另一个宿主锁）既不记 nocore 也不算违规。
//   botw 卡死 1500 帧的环路 nx-120 正是这一类：h_nvnTextureFinalize → nvn_be_tex_release → mtl_vc_drop 全程持核、无 DM_BLOCK。
// 判据：held_core > 0 且 in_svc = 0 且 off_guest = 0 且 PC 不在游戏模块（= 线程在宿主 C 代码里，且这期间没人敢放核）。
// 只记「采样命中次数」会把「每帧短暂持核几微秒」的热点算成隐患：这里按【连续段】结算——
//   段内连续被采到 ≥2 次（即连续持核 ≥2 个采样周期）才可能是长跑，此时才短暂停住线程读一次 PC 判「宿主 / 客体」；
//   一采即走的短过路只记过路次数、一次 suspend 都不做。长跑段报【超阈值次数】+【最长一次 ms】+【时长分布】+【按 (last_svc, ret) 前 5】。
// 阈值 S2A_HOSTCORE_MS（缺省 1 ms，0 关）。调度计数每 2 ms 采一次，故实际分辨 2 ms：连续持核 ≥2 ms 才判长跑（日志里写明）。
enum { HC_KEY_MAX = 16 };
static struct { int svc; uint64_t ret; uint32_t segs; uint64_t hits, max_us; } g_hc_key[HC_KEY_MAX];
static int g_hc_nkey;
static uint64_t g_hc_segs, g_hc_hits, g_hc_short, g_hc_pcskip, g_hc_over, g_hc_max_us, g_hc_last_us, g_hc_b[4];   // 全部在 G 内读写
static int hostcore_ms(void) {
    if (!cores_on()) return 0;   // 虚拟核没开就没有「等核」这回事：正式包热路径与日志与改前逐字节相同
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("S2A_HOSTCORE_MS"); v = e ? atoi(e) : 1; if (v < 0) v = 0;
        if (v) LOG("宿主侧持核长跑：开（阈值 %d ms；调度计数 2 ms 采一次，故连续持核 ≥2 ms 才判长跑；S2A_HOSTCORE_MS 可调，0 关）", v);
    }
    return v;
}
#if defined(__APPLE__)
// 读另一条游戏线程此刻的 pc / lr：短暂停住它再放开（调用方持 G 与 g_cmu —— 目标不可能正持这两把锁，不会互等）。
// 与 thr_pause 共用 g_susp_mu：Mach 的 suspend 是计数式的，两处并发 suspend / resume 次数不配平会把目标永久挂住
static pthread_mutex_t g_susp_mu = PTHREAD_MUTEX_INITIALIZER;
static int thr_pc_lr(thr_t *t, uint64_t *pc, uint64_t *lr) {
    if (!t->mport) return 0;
    pthread_mutex_lock(&g_susp_mu);
    if (thread_suspend(t->mport) != KERN_SUCCESS) { pthread_mutex_unlock(&g_susp_mu); return 0; }
    arm_thread_state64_t st; mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    int ok = thread_get_state(t->mport, ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS;
    if (ok) { *pc = arm_thread_state64_get_pc(st); *lr = arm_thread_state64_get_lr(st); }
    thread_resume(t->mport);
    pthread_mutex_unlock(&g_susp_mu);
    return ok;
}
#endif
static void hostcore_agg(int svc, uint64_t ret, uint32_t run, uint64_t us) {   // 持 G
    int i = 0; for (; i < g_hc_nkey; i++) if (g_hc_key[i].svc == svc && g_hc_key[i].ret == ret) break;
    if (i == g_hc_nkey) {
        if (g_hc_nkey >= HC_KEY_MAX) { i = 0; for (int k = 1; k < HC_KEY_MAX; k++) if (g_hc_key[k].segs < g_hc_key[i].segs) i = k; }   // 满了挤掉段数最少的
        else { i = g_hc_nkey++; g_hc_key[i].segs = 0; g_hc_key[i].hits = 0; g_hc_key[i].max_us = 0; }
        g_hc_key[i].svc = svc; g_hc_key[i].ret = ret;
    }
    g_hc_key[i].segs++; g_hc_key[i].hits += run;
    if (us > g_hc_key[i].max_us) g_hc_key[i].max_us = us;
}
static void hostcore_end(thr_t *t) {   // 持 G：本段结束，结算
    if (!t->hc_run) return;
    if (t->hc_run >= 2 && t->hc_host == 1) {
        uint64_t us = thread_clock_ns(t->hc_tlast - t->hc_t0) / 1000;   // 下界：首末采样的确定间隔（真实段尾还多不到一个周期）
        g_hc_segs++; g_hc_hits += t->hc_run;
        g_hc_last_us = us;
        if (us > g_hc_max_us) g_hc_max_us = us;
        if (us >= (uint64_t)hostcore_ms() * 1000) g_hc_over++;
        g_hc_b[us < 4000 ? 0 : us < 8000 ? 1 : us < 16000 ? 2 : 3]++;
        hostcore_agg(t->hc_svc, t->hc_ret, t->hc_run, us);
    } else if (t->hc_run == 1) g_hc_short++;   // 一采即走：过路（热点不是隐患）
    t->hc_run = 0; t->hc_host = 0; t->hc_probed = 0;
}
static void hostcore_probe(thr_t *t, int h) {   // 持 G 与 g_cmu
    if (h <= 0) { hostcore_end(t); return; }
    if (!t->hc_run) { t->hc_run = 1; t->hc_t0 = t->hc_tlast = thread_clock(); t->hc_host = 0; t->hc_probed = 0; }
    else { t->hc_run++; t->hc_tlast = thread_clock(); }
    if (t->hc_probed || t->hc_run < 2) return;   // 短过路不取 PC（不 suspend）；每段只判一次
    t->hc_probed = 1;
#if defined(__APPLE__)
    uint64_t pc = 0, lr = 0;
    if (thr_pc_lr(t, &pc, &lr)) { t->hc_host = module_of((void *)(uintptr_t)pc) ? 2 : 1; t->hc_ret = lr; t->hc_svc = t->last_svc; }
    else { t->hc_host = 2; g_hc_pcskip++; }
#else
    t->hc_host = 2; g_hc_pcskip++;
#endif
}
// ---------- 持锁等核分桶（单点）----------
// 「持着宿主锁去取核」是环的补点：core_block_end 里取核那一刻等多久，按【返回地址】分桶——ra 就是 DM_BLOCK_END 所在函数的返回点。
// culprit 只有一个函数时这一条就够定位，不需要给每把宿主锁加 ID；若读数显示 ra 散在多个函数，再升级到「记宿主锁 ID」那套。
// 无锁写（原子）：这段可能在持着 g_rmu / g_smu 时被调用，这里碰 G 会与「持 G 等那把锁的线程」互等——热路径不许拿锁。
/* 惰性取核那一处的 sentinel：s2a_guest_return 里 __builtin_return_address(0) 恒为 entry.S 那条 bl 的下一条，
   分不出调用点，也不能用 NULL（那会与「拿不到 ra」的其他路径混桶）。0x1 不可能是真实返回地址。 */
#define HC_LAZY_RA 0x1UL
enum { HC_CB_MAX = 16 };
static struct { _Atomic(uintptr_t) ra; _Atomic uint32_t n, n_over; _Atomic uint64_t max_us; } g_cb[HC_CB_MAX];
static _Atomic uint64_t g_cb_all, g_cb_over, g_cb_max_us, g_cb_misc;
static void hostcore_corewait(void *ra, uint64_t ticks) {
    uint64_t us = thread_clock_ns(ticks) / 1000;
    int over = us >= (uint64_t)hostcore_ms() * 1000;
    atomic_fetch_add(&g_cb_all, 1); if (over) atomic_fetch_add(&g_cb_over, 1);
    { uint64_t o = atomic_load(&g_cb_max_us); while (us > o && !atomic_compare_exchange_weak(&g_cb_max_us, &o, us)) {} }
    uintptr_t r = (uintptr_t)ra; int slot = -1;
    for (int k = 0; k < HC_CB_MAX; k++) {
        uintptr_t cur = atomic_load(&g_cb[k].ra);
        if (cur == r) { slot = k; break; }
        if (!cur) { uintptr_t z = 0; if (atomic_compare_exchange_strong(&g_cb[k].ra, &z, r)) { slot = k; break; } if (z == r) { slot = k; break; } }
    }
    if (slot < 0) { atomic_fetch_add(&g_cb_misc, 1); return; }   // 桶满：有更多不同的调用点（本身就是「别处也等核」的信号）
    atomic_fetch_add(&g_cb[slot].n, 1); if (over) atomic_fetch_add(&g_cb[slot].n_over, 1);
    { uint64_t o = atomic_load(&g_cb[slot].max_us); while (us > o && !atomic_compare_exchange_weak(&g_cb[slot].max_us, &o, us)) {} }
}
// 调度计数（S2A_CORES 开时由核抢占看门狗每 2 ms 采一次，关时零开销）：回答「同一虚拟核同时在跑客体线程 ≤1」「同优先级两线程同时在跑」。
// 在跑客体 = 已启动未退出、不在系统调用里、没被暂停、没在等核、没放核去宿主侧干活。持核的按持有核记，不持核却在跑的按理想核记（这就是违规）。
// 线程对象会被释放，遍历要持 G：trylock 抢不到就跳过这次并计数，不拖慢系统调用。收局打进 host.log（s2a_sched_stat_log）
static _Atomic uint64_t g_ss_n, g_ss_skip, g_ss_multi, g_ss_nocore, g_ss_sameprio_core, g_ss_sameprio_any; static _Atomic int g_ss_peak;
static void sched_sample(void) {
    if (pthread_mutex_trylock(&G)) { atomic_fetch_add(&g_ss_skip, 1); return; }
    // 核状态（held_core / g_cown）在 g_cmu 下改（取核 / 放核 / 空闲核迁移）：采样也持 g_cmu，读到的是一致快照。
    //   只持 G 时，迁移中途会读到被迁线程的旧核号与抢核线程的新核号同核，误报「同核 ≥2」（10-23 Mac 缺省开核对）
    if (pthread_mutex_trylock(&g_cmu)) { pthread_mutex_unlock(&G); atomic_fetch_add(&g_ss_skip, 1); return; }
    enum { MAXR = 64 }; int rc[MAXR], rp[MAXR], n = 0, nocore = 0;
    int hc = hostcore_ms();   // 宿主侧持核长跑：与调度计数共用这一轮采样（不额外唤醒、不改周期）
    for (thr_t *t = g_all; t; t = t->all) {
        if (!t->started || t->exited || atomic_load(&t->in_svc) || atomic_load(&t->pause) || atomic_load(&t->wcore) || atomic_load(&t->off_guest)) { if (hc) hostcore_end(t); continue; }
        if (!core_part(t)) { if (hc) hostcore_end(t); continue; }   // 按需模式：只看参与核互斥的线程（没读过核号的不取核，不算违规）
        int h = atomic_load(&t->held_core);
        if (hc) hostcore_probe(t, h);   // 不受 MAXR 截断：尾部线程的段也要正常结算，不然段会跨采样越滚越长
        if (n >= MAXR) continue;
        nocore += !h; rc[n] = h ? h - 1 : core_of(t); rp[n] = t->prio; n++;
        if (!h) { t->ss_nocore++; t->ss_last_svc = t->last_svc; }
    }
    pthread_mutex_unlock(&g_cmu); pthread_mutex_unlock(&G);
    int cnt[NCORE] = {0}, peak = 0, spc = 0, spa = 0;
    for (int i = 0; i < n; i++) { if (++cnt[rc[i]] > peak) peak = cnt[rc[i]]; for (int j = 0; j < i; j++) if (rp[j] == rp[i]) { spa = 1; if (rc[j] == rc[i]) spc = 1; } }
    atomic_fetch_add(&g_ss_n, 1); if (peak > 1) atomic_fetch_add(&g_ss_multi, 1); if (nocore) atomic_fetch_add(&g_ss_nocore, 1);
    if (spc) atomic_fetch_add(&g_ss_sameprio_core, 1); if (spa) atomic_fetch_add(&g_ss_sameprio_any, 1);
    for (int o = atomic_load(&g_ss_peak); peak > o && !atomic_compare_exchange_weak(&g_ss_peak, &o, peak);) {}
}
// 同核并发诊断（S2A_SAMECORE_DIAG=1，缺省关；开不开虚拟核都能用）：每 2 ms 采一次「在跑客体」的线程（判定式同调度计数），统计两种 Switch 上不会发生的同时运行：
//   甲 = 两条亲和掩码只含同一个核的线程同时在跑；乙 = 两条 GetCurrentProcessorNumber 会报同一个核的线程同时在跑（虚拟核关时 0x10 回理想核，按核号分的无锁数据会被同时改）。
//   Switch 每核一次只跑一条线程，两项都应为 0；开虚拟核时持核互斥，甲按构造为 0（t_thread ⑮ 单测证明，收走核造违规做正对照）。收局打进 host.log
//   丙 = 两条读过核号（调过 0x10）的线程报同一个核、同时在跑——按核号分的无锁数据真会被同时改的情形（10-24 维护者验收：按需核锁开后应为 0）
static _Atomic uint64_t g_py_n, g_py_timeout;   // 同核高优先级唤醒让先：次数 / 等满 5 ms 超时次数（peer_yield）
static _Atomic uint64_t g_sc_n, g_sc_skip, g_sc_a, g_sc_b, g_sc_c; static _Atomic uint64_t g_sc_a_pair, g_sc_b_pair, g_sc_c_pair;
static int samecore_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("S2A_SAMECORE_DIAG"); v = e && *e == '1'; } return v; }
static void samecore_sample(void) {
    if (pthread_mutex_trylock(&G)) { atomic_fetch_add(&g_sc_skip, 1); return; }
    if (pthread_mutex_trylock(&g_cmu)) { pthread_mutex_unlock(&G); atomic_fetch_add(&g_sc_skip, 1); return; }
    uint64_t ida[NCORE] = {0}, idb[NCORE] = {0}, idc[NCORE] = {0}; int a = 0, b = 0, cc = 0; uint64_t pa = 0, pb = 0, pc = 0;
    for (thr_t *t = g_all; t; t = t->all) {
        if (!t->started || t->exited || atomic_load(&t->in_svc) || atomic_load(&t->pause) || atomic_load(&t->wcore) || atomic_load(&t->off_guest)) continue;
        unsigned m = core_mask(t);
        if (m && !(m & (m - 1))) { int c = __builtin_ctz(m); if (ida[c]) { a = 1; if (!pa) pa = ida[c] << 32 | t->id; } else ida[c] = t->id; }
        int h = atomic_load(&t->held_core), c = h ? h - 1 : core_of(t);
        if (idb[c]) { b = 1; if (!pb) pb = idb[c] << 32 | t->id; } else idb[c] = t->id;
        if (atomic_load(&t->percore)) { if (idc[c]) { cc = 1; if (!pc) pc = idc[c] << 32 | t->id; } else idc[c] = t->id; }
    }
    pthread_mutex_unlock(&g_cmu); pthread_mutex_unlock(&G);
    atomic_fetch_add(&g_sc_n, 1);
    if (a) { atomic_fetch_add(&g_sc_a, 1); uint64_t z = 0; atomic_compare_exchange_strong(&g_sc_a_pair, &z, pa); }
    if (b) { atomic_fetch_add(&g_sc_b, 1); uint64_t z = 0; atomic_compare_exchange_strong(&g_sc_b_pair, &z, pb); }
    if (cc) { atomic_fetch_add(&g_sc_c, 1); uint64_t z = 0; atomic_compare_exchange_strong(&g_sc_c_pair, &z, pc); }
}
static void *samecore_thread(void *x) { (void)x;
#if defined(__APPLE__)
    pthread_setname_np("s2a-同核并发诊断");
#else
    pthread_setname_np(pthread_self(), "s2a-samecore");
#endif
    for (;;) { struct timespec ts = {0, 2000000}; nanosleep(&ts, NULL); samecore_sample(); } return NULL; }
static void samecore_init(void) { static _Atomic int done; if (!samecore_on() || atomic_exchange(&done, 1)) return;
    LOG("同核并发诊断：开（S2A_SAMECORE_DIAG=1，每 2 ms 采一次；虚拟核%s）", cores_on() ? "开" : "关");
    pthread_t t; pthread_create(&t, NULL, samecore_thread, NULL); pthread_detach(t); }
void s2a_test_samecore(uint64_t *n, uint64_t *a, uint64_t *b) { if (n) *n = atomic_load(&g_sc_n); if (a) *a = atomic_load(&g_sc_a); if (b) *b = atomic_load(&g_sc_b); }
uint64_t s2a_test_samecore_c(void) { return atomic_load(&g_sc_c); }
void s2a_test_peer_yield(uint64_t *n, uint64_t *timeout) { *n = atomic_load(&g_py_n); *timeout = atomic_load(&g_py_timeout); }
static void samecore_log(const char *when) {
    if (!samecore_on()) return;
    uint64_t n = atomic_load(&g_sc_n), pa = atomic_load(&g_sc_a_pair), pb = atomic_load(&g_sc_b_pair), pc = atomic_load(&g_sc_c_pair);
    LOG("同核并发诊断（%s）：采样 %llu 次（每 2 ms，抢不到锁跳过 %llu 次）；甲 同一单核掩码两线程同时在跑 %llu 次（约 %llu ms，首例线程 id %llu / %llu）；"
        "乙 报同一核号两线程同时在跑 %llu 次（约 %llu ms，首例线程 id %llu / %llu）；Switch 上两项都应为 0", when,
        (unsigned long long)n, (unsigned long long)atomic_load(&g_sc_skip),
        (unsigned long long)atomic_load(&g_sc_a), (unsigned long long)atomic_load(&g_sc_a) * 2, (unsigned long long)(pa >> 32), (unsigned long long)(pa & 0xffffffff),
        (unsigned long long)atomic_load(&g_sc_b), (unsigned long long)atomic_load(&g_sc_b) * 2, (unsigned long long)(pb >> 32), (unsigned long long)(pb & 0xffffffff));
    LOG("同核并发诊断（%s）：丙 读过核号（0x10）的两线程报同一核、同时在跑 %llu 次（首例线程 id %llu / %llu，Switch 与按需核锁下应为 0）；同核高优先级唤醒让先 %llu 次、等满 5 ms 超时 %llu 次", when,
        (unsigned long long)atomic_load(&g_sc_c), (unsigned long long)(pc >> 32), (unsigned long long)(pc & 0xffffffff),
        (unsigned long long)atomic_load(&g_py_n), (unsigned long long)atomic_load(&g_py_timeout));
}
static void sched_nocore_top(void);
static void sched_hostcore_top(void);
void s2a_sched_stat_log(const char *when) {
    samecore_log(when);
    if (!cores_on()) {
        // 宿主侧持核长跑读的是「此刻持着哪个虚拟核」：核不开时 held_core 恒 0，这套读数不成立，明确说出来免得当成「一局没采到」
        if (hostcore_ms()) LOG("宿主侧持核长跑（%s）：虚拟核没开（held_core 恒 0），这套读数不生效——出包测它要 S2A_CORES=1", when);
        return;
    }
    LOG("调度计数（%s）：采样 %llu 次（每 2 ms，抢不到锁跳过 %llu 次）；同一虚拟核同时在跑客体线程峰值 %d，≥2 的采样 %llu 次，其中有线程在跑却不持核的 %llu 次；"
        "同核同优先级两线程同时在跑 %llu 次；跨核同优先级同时在跑 %llu 次（Switch 上合法，只作参考）", when,
        (unsigned long long)atomic_load(&g_ss_n), (unsigned long long)atomic_load(&g_ss_skip), atomic_load(&g_ss_peak), (unsigned long long)atomic_load(&g_ss_multi),
        (unsigned long long)atomic_load(&g_ss_nocore), (unsigned long long)atomic_load(&g_ss_sameprio_core), (unsigned long long)atomic_load(&g_ss_sameprio_any));
    sched_nocore_top();
    sched_hostcore_top();
}
static void sched_nocore_top(void) {   // 收局：「在跑客体却不持核」按线程前 5（持 G 遍历）
    thr_t *top[5] = {0};
    glock();
    for (thr_t *t = g_all; t; t = t->all) { if (!t->ss_nocore) continue;
        for (int i = 0; i < 5; i++) if (!top[i] || t->ss_nocore > top[i]->ss_nocore) { for (int k = 4; k > i; k--) top[k] = top[k - 1]; top[i] = t; break; } }
    for (int i = 0; i < 5 && top[i]; i++)
        LOG("调度计数（收局）不持核在跑 第 %d：线程 id %llu 句柄 %#x 优先级 %d 理想核 %d 掩码 %#llx，采样 %u 次，最近被采到时 last_svc %#x（最近 svc %#x，退出 %d）", i + 1,
            (unsigned long long)top[i]->id, top[i]->handle, top[i]->prio, top[i]->core, (unsigned long long)top[i]->mask, top[i]->ss_nocore, top[i]->ss_last_svc, top[i]->last_svc, top[i]->exited);
    pthread_mutex_unlock(&G);
}
static void hc_ret_str(uint64_t ret, char *b, size_t n) {   // 返回地址 + 宿主符号（dladdr 只解析，不加锁）
    Dl_info d;
    if (ret && dladdr((void *)(uintptr_t)ret, &d) && d.dli_sname) {
        const char *im = d.dli_fname ? strrchr(d.dli_fname, '/') : NULL; im = im ? im + 1 : d.dli_fname;
        snprintf(b, n, "%#llx（%s+%#llx，%s）", (unsigned long long)ret, d.dli_sname,
                 (unsigned long long)(ret - (uint64_t)(uintptr_t)d.dli_saddr), im ? im : "?");
    } else snprintf(b, n, "%#llx", (unsigned long long)ret);
}
static void sched_corewait_top(int thr);
static void sched_hostcore_top(void) {   // 收局：宿主侧持核长跑（持 G；核不开时 held_core 恒 0，由调用方先判）
    int thr = hostcore_ms(); if (!thr) return;
    glock();
    uint64_t segs = g_hc_segs, hits = g_hc_hits, over = g_hc_over, max_us = g_hc_max_us, sh = g_hc_short, ps = g_hc_pcskip;
    if (!segs) {
        LOG("宿主侧持核长跑（>%d ms）：一局没采到（过路命中 %llu 次；PC 读不到 %llu 次）", thr, (unsigned long long)sh, (unsigned long long)ps);
    } else {
        LOG("宿主侧持核长跑（>%d ms）：命中 %llu 次（%llu 段，其中超阈值 %llu 段），最长 %.1f ms；过路命中 %llu 次（一采即走，不计入）", thr,
            (unsigned long long)hits, (unsigned long long)segs, (unsigned long long)over, (double)max_us / 1000.0, (unsigned long long)sh);
        LOG("  时长分布（下界；采样 2 ms，段尾实际可能再长不到 2 ms）：2–4 ms %llu 段、4–8 ms %llu 段、8–16 ms %llu 段、≥16 ms %llu 段",
            (unsigned long long)g_hc_b[0], (unsigned long long)g_hc_b[1], (unsigned long long)g_hc_b[2], (unsigned long long)g_hc_b[3]);
        int top[5] = {-1, -1, -1, -1, -1};
        for (int i = 0; i < g_hc_nkey; i++) {
            for (int k = 0; k < 5; k++) if (top[k] < 0 || g_hc_key[i].hits > g_hc_key[top[k]].hits) {
                for (int j = 4; j > k; j--) top[j] = top[j - 1]; top[k] = i; break; }
        }
        for (int k = 0; k < 5 && top[k] >= 0; k++) {
            char b[256]; hc_ret_str(g_hc_key[top[k]].ret, b, sizeof b);
            LOG("  %s前 %d：last_svc=%#x ret=%s 采样 %llu 次（%u 段）最长 %.1f ms", k ? "    " : "", k + 1, g_hc_key[top[k]].svc, b,
                (unsigned long long)g_hc_key[top[k]].hits, g_hc_key[top[k]].segs, (double)g_hc_key[top[k]].max_us / 1000.0);
        }
    }
    sched_corewait_top(thr);
    { uint64_t k = atomic_load(&g_soft_keeps); for (thr_t *t = g_all; t; t = t->all) k += t->soft_keeps;
      LOG("虚拟核宿主侧可借（收局）：回客体时原核还在、免锁 %llu 次，被等核者取走 %llu 次", (unsigned long long)k, (unsigned long long)atomic_load(&g_soft_steals)); }
    pthread_mutex_unlock(&G);
}
static void sched_corewait_top(int thr) {   // 持 G（桶本身是原子写，这里只读）
    uint64_t all = atomic_load(&g_cb_all), ov = atomic_load(&g_cb_over), mx = atomic_load(&g_cb_max_us), misc = atomic_load(&g_cb_misc);
    if (!all) { LOG("持锁等核（DM_BLOCK_END 取核）：一局没采到"); return; }
    LOG("持锁等核（DM_BLOCK_END 取核，>%d ms）：超阈值 %llu 次 / 共 %llu 次，最长 %.1f ms%s", thr,
        (unsigned long long)ov, (unsigned long long)all, (double)mx / 1000.0,
        misc ? "；另有调用点因桶满未单列（说明不止一个 culprit，要升级到记宿主锁 ID）" : "");
    int top[5] = {-1, -1, -1, -1, -1};
    for (int i = 0; i < HC_CB_MAX; i++) {
        if (!atomic_load(&g_cb[i].ra)) continue;
        for (int k = 0; k < 5; k++) {
            uint32_t a = top[k] < 0 ? 0 : atomic_load(&g_cb[top[k]].n_over), b = atomic_load(&g_cb[i].n_over);
            if (top[k] < 0 || b > a || (b == a && atomic_load(&g_cb[i].n) > atomic_load(&g_cb[top[k]].n))) {
                for (int j = 4; j > k; j--) top[j] = top[j - 1]; top[k] = i; break; }
        }
    }
    for (int k = 0; k < 5 && top[k] >= 0; k++) {
        char b[256]; hc_ret_str((uint64_t)(uintptr_t)atomic_load(&g_cb[top[k]].ra), b, sizeof b);
        LOG("  %s前 %d：ra=%s 超阈值 %u 次 共 %u 次 最长 %.1f ms", k ? "    " : "", k + 1, b,
            atomic_load(&g_cb[top[k]].n_over), atomic_load(&g_cb[top[k]].n), (double)atomic_load(&g_cb[top[k]].max_us) / 1000.0);
    }
}
// 单测用：把线程 h 的核直接收走（它照常在游戏代码里跑）——人为造「不持核在跑」，给调度计数做正对照
void s2a_test_steal_core(uint32_t h) {
    glock(); thr_t *t = h_obj(h, S2A_OBJ_THREAD); pthread_mutex_unlock(&G);
    if (t) core_release(t);
}
// 单测用：按调度计数同一判定式，问线程 h「此刻算不算在跑却不持核」（1 算、0 不算、-1 没这个线程）；*wcore 回它是否在等核。同步读，不靠看门狗采样时机
int s2a_test_thr_nocore(uint32_t h, int *wcore) {
    glock(); thr_t *t = h_obj(h, S2A_OBJ_THREAD); int r = -1;
    if (t) { r = t->started && !t->exited && !atomic_load(&t->in_svc) && !atomic_load(&t->pause) && !atomic_load(&t->wcore) && !atomic_load(&t->off_guest) && !atomic_load(&t->held_core);
             if (wcore) *wcore = atomic_load(&t->wcore); }
    pthread_mutex_unlock(&G); return r;
}
// 单测用：宿主侧持核长跑的读数（长跑段数 / 累计采样命中 / 超阈值段 / 过路命中 / 最长 us / PC 读不到次数）
void s2a_test_hostcore(uint64_t *segs, uint64_t *hits, uint64_t *over, uint64_t *short_hits, uint64_t *max_us, uint64_t *pcskip) {
    glock();
    if (segs) *segs = g_hc_segs; if (hits) *hits = g_hc_hits; if (over) *over = g_hc_over;
    if (short_hits) *short_hits = g_hc_short; if (max_us) *max_us = g_hc_max_us; if (pcskip) *pcskip = g_hc_pcskip;
    pthread_mutex_unlock(&G);
}
// 单测用：最近结算的一段宿主侧长跑有多长（us）。最长值是累计的，前面用例已有更长段时新段刷新不了它，判「这一段多长」要看这个
uint64_t s2a_test_hostcore_last_us(void) { glock(); uint64_t v = g_hc_last_us; pthread_mutex_unlock(&G); return v; }
// 单测用：持锁等核分桶的读数（总次数 / 超阈值次数 / 最长 us / 桶满漏计次数 / 用了几个桶）
void s2a_test_hostcore_corewait(uint64_t *all, uint64_t *over, uint64_t *max_us, uint64_t *misc, int *slots) {
    if (all) *all = atomic_load(&g_cb_all); if (over) *over = atomic_load(&g_cb_over);
    if (max_us) *max_us = atomic_load(&g_cb_max_us); if (misc) *misc = atomic_load(&g_cb_misc);
    if (slots) { int n = 0; for (int i = 0; i < HC_CB_MAX; i++) if (atomic_load(&g_cb[i].ra)) n++; *slots = n; }
}
void s2a_test_sched_stat(uint64_t *n, int *peak, uint64_t *multi, uint64_t *nocore, uint64_t *spc) {
    if (n) *n = atomic_load(&g_ss_n); if (peak) *peak = atomic_load(&g_ss_peak); if (multi) *multi = atomic_load(&g_ss_multi);
    if (nocore) *nocore = atomic_load(&g_ss_nocore); if (spc) *spc = atomic_load(&g_ss_sameprio_core);
}
// 持 g_cmu：有没有别的、同优先级、掩码含 c 的等核者（时间片轮转用）
static int same_prio_waiter(int c, const thr_t *o) { for (int i = 0; i < g_ncw; i++) { const thr_t *w = g_cw[i]; if (w != o && w->prio == o->prio && (core_mask(w) >> c & 1)) return 1; } return 0; }
static void *preempt_watchdog(void *a) {
    (void)a;
#if defined(__APPLE__)
    pthread_setname_np("s2a-核抢占看门狗");
#else
    pthread_setname_np(pthread_self(), "s2a-preempt");
#endif
    // 抢占看门狗必须实时：iPhone 上默认 QoS 的后台线程会被系统挪到能效核并降频，0.5 ms 周期跑不准、抢占延迟飙升
    // （gate-host 高负载下 t_thread ⑫ 偶发红就是这个）。设成与游戏主线程同档的最高用户 QoS，保证不被挪能效核。
#if defined(__APPLE__)
    s2a_qos_set_self(S2A_QOS_PREEMPT_WATCHDOG);
#endif
    long per_ns = 500000;              // 周期缺省 0.5 ms；量抢占延迟分布 / 调优时可用 S2A_PREEMPT_PERIOD_US 配（微秒）
    const char *pv = getenv("S2A_PREEMPT_PERIOD_US");
    if (pv) { long us = atol(pv); if (us > 0) per_ns = us * 1000; }
    uint64_t next_ss = 0;
    for (;;) {
        // 没人等核时抢占 / 时间片轮转都无事可做（两者都要有等核者）：睡在 g_wd_cv 上，有线程开始等核即醒；空闲期调度计数降到每 20 ms 一次。
        //   原先不管有没有等核者都 0.5 ms 轮询：每秒 2000 次唤醒、常驻性能核，10-24 测试设备2 动森直连同包虚拟核开 3.754 W / 关 2.597 W（唤醒 2380 / 630 次每秒）
        pthread_mutex_lock(&g_cmu);
        while (!g_ncw) {
            struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 20000000; if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&g_wd_cv, &g_cmu, &dl) == ETIMEDOUT && !g_ncw) { pthread_mutex_unlock(&g_cmu); sched_sample(); pthread_mutex_lock(&g_cmu); }
        }
        pthread_mutex_unlock(&g_cmu);
        struct timespec ts = {per_ns / 1000000000L, per_ns % 1000000000L}; nanosleep(&ts, NULL);   // 有等核者时 0.5 ms 周期 → 抢占延迟上界 ~0.5 ms + 投递
        atomic_fetch_add(&g_wd_scans, 1);
        { uint64_t t = now_ns(); if (t >= next_ss) { next_ss = t + 2000000; sched_sample(); } }   // 调度计数每 2 ms 一次
        atomic_store(&g_wd_last_ns, now_ns());   // ⑫ 诊断：watchdog 最后扫的时间
        pthread_t kp[NCORE]; int nk = 0;
        // Keep thread_exit from publishing/finishing between selecting a
        // pthread handle and delivering SIGURG. Handler never acquires G.
        glock();
        pthread_mutex_lock(&g_cmu);
        for (int c = 0; c < NCORE; c++) {
            thr_t *o = g_cown[c];
            if (o && !atomic_load(&o->preempting) && !atomic_load(&o->restoring) && core_claimed(c, o)) {
                atomic_store(&o->preempting, 1); kp[nk++] = o->pt;     // 该核有更高优先级等核者 → 抢占持核者
            } else if (o && hz_core() && !atomic_load(&o->preempting) && !atomic_load(&o->restoring) && o->prio == (c == NCORE - 1 ? 63 : 59)
                       && now_ns() - g_cown_t[c] >= 10000000ull && same_prio_waiter(c, o)) {
                // Horizon 时间片轮转（Ryujinx KScheduler.PreemptionThreadLoop / RotateScheduledQueue）：0–2 号核优先级 59、3 号核 63 的持核者每 10 ms 让给同优先级等核者；
                //   标「让出」，重取核时排到同优先级等待者后面
                o->yielded = 1; atomic_store(&o->preempting, 1); kp[nk++] = o->pt;
            }
        }
        pthread_mutex_unlock(&g_cmu);
        for (int i = 0; i < nk; i++) { pthread_kill(kp[i], S2A_SIG_PREEMPT); atomic_fetch_add(&g_sigurg_sent, 1); }
        pthread_mutex_unlock(&G);
        // 已放 g_cmu 后发信号；G 保证 pthread 在投递前不会退出并被 join。
    }
    return NULL;
}
#if defined(__APPLE__)
static void stop_signal_init(void) {
    static _Atomic int done; if (atomic_exchange(&done, 1)) return;
    int error = plat_fault_install(S2A_SIG_PREEMPT, on_preempt, PLAT_FAULT_ONSTACK);
    if (error) die("安装 SIGURG 失败 errno=%d", error);
}
#endif
static void preempt_init(void) {                       // cores_on 首次开时：注册信号 + 起看门狗（只一次）
    static _Atomic int done; if (atomic_exchange(&done, 1)) return;
    // SA_ONSTACK：信号帧落在备用信号栈（每条游戏线程 s2a_crash_thread_init 设过），不压在游戏栈上——游戏 / 改写器桩可能正用着 sp 下方（见 on_preempt）
#if defined(__APPLE__)
    stop_signal_init();
#else
    { int e = s2a_android_guest_writers_init(); if (e) die("Android SIGURG unavailable errno=%d", e); }
#endif
    pthread_t t;
#if defined(__APPLE__)
    pthread_create(&t, NULL, preempt_watchdog, NULL);
#else
    int error = pthread_create(&t, NULL, preempt_watchdog, NULL);
    if (error) die("Android preempt watchdog create errno=%d", error);
#endif
    pthread_detach(t);
}
// gate.c：走 C# 的 IPC 前后（阻塞在服务线程上）
// 宿主侧可借（11-21 #1842）：按需档缺省开后，每次直连调用进宿主（DM_BLOCK_BEGIN）放核、回客体取核，两头都拿全局 g_cmu、放核还广播叫醒等核者——
//   一款开放世界游戏 iPhone 一局 15 分钟取核 4.8 亿次（约 53 万次 / 秒），CPU 95% → 231%（虚拟核关的 0.5.8 对 0.6.1，同机同款，D-5H47）。
//   没人等核时不放：只置「可借」，回客体时一次 CAS 拿回；有人等（g_ncw > 0，或之后来的等核者在 core_pick 里看见可借）才走原先的放核。S2A_VCORE_SOFT=0 关（原行为）
int g_vcore_soft = -1;   // -1 未读、0 关、1 开（缺省）；非 static：单测 t_thread ㉛ 同一进程里 A/B
static int vcore_soft(void) {
    int v = g_vcore_soft;
    if (v < 0) { const char *e = getenv("S2A_VCORE_SOFT"); v = !(e && *e == '0'); g_vcore_soft = v; if (cores_on()) LOG("虚拟核：宿主侧可借 %s（S2A_VCORE_SOFT）", v ? "开" : "关"); }
    return v;
}
void s2a_core_leave(void) {
    thr_t *t = thread_current_or_null(); if (!t) return;
    atomic_store(&t->off_guest, 1);
    if (t->held_core && vcore_soft()) {
        atomic_store(&t->csoft, 1);                                   // 先置可借（顺序一致）…
        if (!__atomic_load_n(&g_ncw, __ATOMIC_SEQ_CST)) return;      // …再读等核数：两边至少一方看得见另一方（等核者先登记再在 core_pick 里读可借位）
    }
    core_release(t);
}
void s2a_core_enter(void) { thr_t *t = thread_current_or_null(); if (t) { core_acquire(t); atomic_store(&t->off_guest, 0); } }
// 直连图形层里游戏线程要阻塞等 GPU / 上屏 / 管线时放核、等完取核（同 IPC 的 svc 门）：那些等待不经 svc，持核睡会让同核游戏线程全等核
// （10-24 测试设备2 动森直连：虚拟核开着 152 秒才到 MARK、3.5 W，关掉 49 秒）。可嵌套，只有最外层放 / 取；非游戏线程空操作
// 取核时点（10-30 惰性取核）：原来 block_end 当场取核，等于假定「调用点这会儿不再持宿主锁」——可这个知识只有调用点有，机制没有。
//   botw 卡死就是这条假定破的：nx-82 持 g_rmu 在 DM_BLOCK_END 里等核、nx-120 持核在 nvnTextureFinalize → mtl_vc_drop 里等 g_rmu，ABBA。
//   现在 block_end 只减计数；取核挂到「回客体代码」这个结构性唯一时点（entry.S xs_entry 的 blr 之后 → s2a_guest_return）。
//   S2A_LAZY_CORE=1 开、未设 / 0 = 旧行为（同一二进制运行期 A/B）。
// entry.S 用 adrp/add 直接寻址它：ELF（Android 共享库）里这个对象不能被预占，否则 ld.lld 报
//   relocation R_AARCH64_ADR_PREL_PG_HI21 cannot be used against symbol 'g_lazy_core'（同 xstack.c 的 g_xs_fn / g_xs_off）。
#if (defined(__ANDROID__) || defined(__OHOS__))
#define S2A_ASM_INTERNAL __attribute__((visibility("hidden")))
#else
#define S2A_ASM_INTERNAL
#endif
// 11-19 苹果平台缺省开：虚拟核缺省改按需（cores_mode）后，block_end 当场取核的旧行为在按需档复现了同一类锁环（一款赛车游戏 Mac 6 局 1 局：
//   nx-82 持 g_rmu 在 up_wait_zero 的 DM_BLOCK_END 里等核 1、nx-86 持核 1 在 nvn_be_reg_tex 里等 g_rmu）。惰性取核是这类环的结构性解，随按需一起成缺省
#if defined(__APPLE__)
enum { LAZY_DEFAULT = 1 };
#else
enum { LAZY_DEFAULT = 0 };
#endif
S2A_ASM_INTERNAL int g_lazy_core = -1;   // -1 未读、0 关（缺省旧行为）、1 开。非 static：entry.S 的钩子先读它，读到 0 就一条 cbz 走开
static int lazy_core(void) {
    int v = g_lazy_core;
    if (v < 0) { const char *e = getenv("S2A_LAZY_CORE");
                 v = e && *e ? strcmp(e, "0") != 0 : LAZY_DEFAULT;   // 不设取缺省（苹果开，见 LAZY_DEFAULT）
                 g_lazy_core = v; LOG("虚拟核：惰性取核 %s（S2A_LAZY_CORE）", v ? "开" : "关"); }
    return v;
}
static __thread int t_blk;
// 唤醒追踪：宿主阻塞段按（调用点、线程）分账——时序族要看高优先级线程放核是阻塞在哪个直连调用里（等 GPU / 出图是真机也阻塞的，录制 / 编码是真机上很快返回的）
#define WT_BLK_N 1024
static struct { _Atomic uint64_t key, ns, n; } g_wt_blk[WT_BLK_N];   // key = 调用点 ^ (线程 id << 48)
static __thread uint64_t t_wt_blk_ra;
static void wt_blk_add(uint64_t ra, uint64_t tid, uint64_t ns) {
    uint64_t k = ra ^ (tid << 48); if (!k) return;
    for (uint32_t i = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 54), j = 0; j < WT_BLK_N; j++, i = (i + 1) % WT_BLK_N) {
        uint64_t e = 0;
        if (atomic_load(&g_wt_blk[i].key) == k || atomic_compare_exchange_strong(&g_wt_blk[i].key, &e, k) || e == k) {
            atomic_fetch_add(&g_wt_blk[i].ns, ns); atomic_fetch_add(&g_wt_blk[i].n, 1); return; } }
}
__attribute__((noinline)) void s2a_core_block_begin(void) { if (!t_blk++) {
#if !defined(__APPLE__)
    { thr_t *bt = thread_current_or_null(); if (bt) atomic_store_explicit(&bt->blk_ra, (uint64_t)__builtin_return_address(0), memory_order_relaxed); }
#endif
    if (wt_on()) { t_wt_blk_t0 = thread_clock(); t_wt_blk_ra = (uint64_t)__builtin_return_address(0); } s2a_core_leave(); } }
// noinline：__builtin_return_address(0) 要拿到「DM_BLOCK_END 所在函数的返回点」，被内联进来就变成上一层调用点
__attribute__((noinline)) void s2a_core_block_end(void) {
    if (!(t_blk > 0 && !--t_blk)) return;
    if (t_wt_blk_t0) { uint64_t d = thread_clock_ns(thread_clock() - t_wt_blk_t0); t_wt_blk_ns += d; t_wt_blk_t0 = 0;
        thr_t *ct = thread_current_or_null(); if (d > 100000) wt_blk_add(t_wt_blk_ra, ct ? ct->id : 0, d); }   // 只计 >0.1 ms 的段：短段不放核也一样
    if (lazy_core()) return;   // 惰性取核：取核挪到 s2a_guest_return（那里另计时，见 HC_LAZY_RA）
    int on = hostcore_ms();
    uint64_t t0 = 0; void *ra = NULL;
    if (on) { t0 = thread_clock(); ra = __builtin_return_address(0); }
    s2a_core_enter();
    if (on) hostcore_corewait(ra, thread_clock() - t0);   // 取核期间等了多久：持着宿主锁等核就是环的那一半
}
// 暂停 / 进后台冻结期间不占核（S2A_CORES）
static void pause_park(thr_t *t) {
    core_release(t);
    glock();
#if !defined(__APPLE__)
    if (atomic_load(&t->in_svc)) t->pmach = 0;
#endif
    t->pwait = 0;
    atomic_store_explicit(&t->paused_ack, 1, memory_order_release);
#if !defined(__APPLE__)
    pause_diag_boundary_locked(t, 11);
#endif
    while (!atomic_load(&g_session_stop) && (atomic_load(&t->pause) || atomic_load(&g_frz))) thread_condition_wait(t, &g_pause_cv, NULL);
    atomic_store(&t->paused_ack, 0);
    pthread_mutex_unlock(&G); }
#if defined(__APPLE__)
// 出系统调用回游戏代码前的最后一次认暂停（苹果，审计 P1-49）：清 in_svc 之后再查一次 pause / 冻结。
//   暂停方持 G「先置 pause、再读 in_svc」，看到 in_svc=1 就当场代发 ack（目标在系统调用里，门帧即上下文）；这里「先清 in_svc、再读 pause」
//   与之成一对：两边都是顺序一致的原子，至少一边看得见另一边——暂停方看到 in_svc=1，这里就一定看到 pause=1、停进 pause_park，不会带着「已暂停」回游戏代码。
//   原先只在取核之前查一次：查完到清 in_svc 之间（等核可以很久）来的暂停回了成功，线程照跑游戏代码；C# 系统调用出口（s2a_svc_ctx_done）连一次都不查
static void svc_leave_check(thr_t *t) {
    while (!atomic_load(&g_session_stop) && (atomic_load(&t->pause) || atomic_load(&g_frz))) {
        int had = t->held_core != 0;   // 只本线程从 0 改成非 0：出口持着核的停完重取，收局停住（park_forever 已放核）的不取
        atomic_store(&t->in_svc, 1);
        t->park_at = 2; pause_park(t);
        if (had) core_acquire(t);
        atomic_store(&t->in_svc, 0);
    }
}
#endif
static _Atomic int g_unpaired_n;   // 回客体时发现 DM_BLOCK 未配对的次数（打红用；t_thread ㉘ 断言它涨 1，日志只在前 8 次打）
// entry.S 钩子：xs_entry 里「切过客体栈 → blr 宿主函数 → 回客体代码之前」调一次（x0..x7 / q0..q3 由调用方在宿主栈帧里存好）。
//   只有那条路径回客体代码：1: br x16 的尾跳（无槽的纯宿主线程、宿主→宿主嵌套）不回，所以钩子不挂在那里 —— 挂上去等于在机制内部
//   重新制造环路（外层宿主函数还持着锁时取核），比原本的 bug 更隐蔽。
//   纯宿主线程（无槽）走尾跳、根本不进这里；下面的 thread_current_or_null 是双保险。
//   先 park 再取核，同 svc_exit：park 内先 core_release 再等恢复，反了就是暂停期间白占一个核。
void s2a_guest_return(void) {
    if (!lazy_core()) return;
    thr_t *t = thread_current_or_null(); if (!t) return;
    // 即时存档冻结时不在这里停：这里是宿主函数回客体的半路，现场不在门帧里，存不下；放它跑到下一次系统调用入口再停
    if (atomic_load(&t->pause) || (atomic_load(&g_frz) && !atomic_load(&g_snap_frz))) { t->park_at = 3; pause_park(t); }
    if (t_blk > 0) {   // 有 DM_BLOCK 没配对：不取核（惰性化后不再崩，而是让这条线程饿死自己），打红早报
        if (atomic_fetch_add(&g_unpaired_n, 1) < 8) LOG("【红】回客体时 DM_BLOCK 未配对（t_blk=%d），不取核", t_blk);
        return; }
    // 惰性取核的等待也进「持锁等核分桶」：这里 ra 恒为 entry.S 那条 bl 的下一条，分不出调用点，
    //   所以用一个不会是真实返回地址的 sentinel（0x1）单独占一个桶，读数里一眼认出是「惰性取核」那一类。
    if (!core_part(t) || core_fast(t)) { atomic_store(&t->off_guest, 0); return; }   // 不参与虚拟核（按需档里没读过核号的线程）/ 原核还在：不计时、不进「持锁等核分桶」——那里只记真等过核的   // 原核还在（宿主侧可借没被取走 / 一直硬持）：不计时、不进「持锁等核分桶」——那里只记真等过核的
    int on = hostcore_ms(), wt = wt_on();
    uint64_t t0 = 0;
    if (on || wt) t0 = thread_clock();
    core_acquire(t);   // 自带「已持且掩码容得下就返回」，不必自己判 held_core
    if (on) hostcore_corewait((void *)HC_LAZY_RA, thread_clock() - t0);
    if (wt) t_wt_core_ns += thread_clock_ns(thread_clock() - t0);
    atomic_store(&t->off_guest, 0);
}
#if !defined(__APPLE__)
static void sleep_wait_boundary(thr_t *t) {
    // 复用既有pause/ack/CV合同；已ack的暂停正在条件等待返回屏障中时，不嵌套暂停。
    if (!atomic_load(&t->paused_ack) && (atomic_load(&t->pause) || atomic_load(&g_frz))) pause_park(t);
}
#endif
// 同步类系统调用操作的游戏地址（地址就是宿主地址）：ArbitrateLock 在 x1，ArbitrateUnlock / WaitProcessWideKeyAtomic / WaitForAddress / SignalToAddress 在 x0
static uint32_t *sync_svc_addr(int num, const uint64_t *regs) {
    uint64_t a = num == 0x1A ? regs[1] : (num == 0x1B || num == 0x1C || num == 0x34 || num == 0x35) ? regs[0] : 0;
    return a && !(a & 3) ? (uint32_t *)(uintptr_t)a : NULL;
}
static void svc_enter(thr_t *t, uint64_t *regs, int num) {
    s2a_thread_stop_point();
    if (wt_on() && t->tr_ret_t) {   // 用户态段：上次回游戏 → 这次进系统调用
        uint64_t gap = thread_clock_ns(thread_clock() - t->tr_ret_t);
        if (gap > 2000000) {   // 先扣掉宿主阻塞段（直连调用里的 DM_BLOCK 等待）与回客体等虚拟核：那两段不是 macOS 停线程，0 CPU 是应该的
            uint64_t cpu = wt_cpu_ns() - t->tr_cpu_ns, blk = t_wt_blk_ns - t_wt_blk_snap, cw = t_wt_core_ns - t_wt_core_snap, rest = gap > blk + cw ? gap - blk - cw : 0;
            int k = blk >= gap / 2 ? 2 : cw >= gap / 2 ? 3 : cpu < rest / 2 ? 0 : 1; static _Atomic int nlog;
            static const char *const kn[4] = { "多半被宿主停住", "在算", "宿主阻塞段为主", "等虚拟核为主" };
            atomic_fetch_add(&g_wt_st[k], 1); atomic_fetch_add(&g_wt_st_ms[k], gap / 1000000);
            if (atomic_fetch_add(&nlog, 1) < 300) LOG("[唤醒追踪] 线程 id %llu 用户态 %.1f ms（线程 CPU %.1f、宿主阻塞段 %.1f、等虚拟核 %.1f ms，%s），上次回游戏自 svc %#x，这次进 svc %#x，lr %#llx",
                (unsigned long long)t->id, gap / 1e6, cpu / 1e6, blk / 1e6, cw / 1e6, kn[k], t->tr_ret_svc, num, (unsigned long long)regs[30]); }
    }
    t->svc_regs = regs; t->last_svc = num; t->last_lr = regs[30]; t->last_fp = regs[29]; t->nsvc++;
    atomic_store(&t->in_svc, 1); if (atomic_load(&t->pause) || atomic_load(&g_frz)) { t->park_at = 1; pause_park(t); }
}
// 时钟抢占：宿主区被请求让核时（on_preempt 宿主分支置 preempt_req），这里是它的主场消费点。
//   atomic_exchange 取走请求再显式放核：core_acquire 只看「已持有核且掩码含它」就直接返回、不会重新排队，
//   所以必须先放，这一次 acquire 才会按优先级重新竞争——更高优先级的等核者在这之间上核（thread.c 116 行注释描述的就是这里）。
static void svc_exit(thr_t *t) {
    s2a_thread_stop_point();
    if (atomic_load(&t->pause) || atomic_load(&g_frz)) { t->park_at = 2; pause_park(t); }
    if (atomic_load(&g_t_preempt_consume) && atomic_exchange(&t->preempt_req, 0)) core_release(t);
    core_acquire(t);
    s2a_thread_stop_point();
#ifdef S2A_UNIT_TEST
    if (s2a_test_svc_leave) s2a_test_svc_leave(t);
#endif
    atomic_store(&t->in_svc, 0);
#if defined(__APPLE__)
    svc_leave_check(t);
#endif
    if (wt_on()) { uint64_t n = thread_clock();
        if (t->tr_wake_t) { wt_bucket(0, thread_clock_ns(n - t->tr_wake_t)); t->tr_wake_t = 0; }          // 被唤醒方：唤醒 → 回到游戏
        if (t->tr_lastwake_t) { wt_bucket(1, thread_clock_ns(n - t->tr_lastwake_t)); t->tr_lastwake_t = 0; }   // 唤醒方：唤醒别人 → 自己回到游戏
        t->tr_ret_t = n; t->tr_ret_svc = t->last_svc; t->tr_cpu_ns = wt_cpu_ns(); t_wt_blk_snap = t_wt_blk_ns; t_wt_core_snap = t_wt_core_ns; }
}
#if (defined(__ANDROID__) || defined(__OHOS__))
// gate只为实际native-memory处理者配对；未处理号恢复原状态，不重复thread/C#的计数。
static _Thread_local struct { int in_svc, last_svc; uint64_t *regs, last_lr; } android_memory_previous;
void s2a_android_memory_svc_enter(int num, uint64_t *regs) {
    if (android_vm_memory_scope) die("Android memory SVC重复进入");
    android_vm_memory_scope = 1;
    android_vm_svc_num = num;
    android_vm_svc_args[0] = regs[0]; android_vm_svc_args[1] = regs[1]; android_vm_svc_args[2] = regs[2];
    thr_t *t = thread_current_or_null();
    if (!t) return;
    android_memory_previous.in_svc = atomic_load(&t->in_svc);
    android_memory_previous.regs = t->svc_regs;
    android_memory_previous.last_svc = t->last_svc;
    android_memory_previous.last_lr = t->last_lr;
    t->svc_regs = regs; t->last_svc = num; t->last_lr = regs[30];
    atomic_store(&t->in_svc, 1);
    if (atomic_load(&t->pause) || atomic_load(&g_frz)) pause_park(t);
}
void s2a_android_memory_svc_unhandled(void) {
    if (!android_vm_memory_scope) die("Android memory SVC撤销没有入口");
    thr_t *t = thread_current_or_null();
    if (t) {
        t->svc_regs = android_memory_previous.regs;
        t->last_svc = android_memory_previous.last_svc;
        t->last_lr = android_memory_previous.last_lr;
        atomic_store(&t->in_svc, android_memory_previous.in_svc);
    }
    android_vm_memory_scope = 0;
}
void s2a_android_memory_svc_finish(void) {
    if (!android_vm_memory_scope) return;
    if (android_vm_active || android_vm_beginning) die("Android memory SVC返回时writer仍活动");
    thr_t *t = thread_current_or_null();
    if (t) svc_exit(t); // owner已结束，才过epoch barrier并重取核；包含查询和参数错误返回。
    android_vm_memory_scope = 0;
}
#endif
int s2a_guest_frozen(void) { return atomic_load(&g_frz); }   // ap_host.c 出图停住看门：iOS 进后台冻结期间不算停住
void s2a_guest_freeze(int on) { glock(); atomic_store(&g_frz, on ? 1 : 0); if (!on) pthread_cond_broadcast(&g_pause_cv); pthread_mutex_unlock(&G); }

// ---------- 同步对象 ----------
static int sig_obj(uint32_t h, int *bad) {   // 1 = 已触发（持 G）
    if (h == S2A_H_CUR_THREAD) return 0;
    int i = h_idx(h); if (!i) { *bad = 1; return 0; }
    switch (g_h[i].kind) {
    case S2A_OBJ_THREAD: return ((thr_t *)g_h[i].obj)->exited;
    case S2A_OBJ_EVENT_R: { event_t *e = g_h[i].obj;
#ifndef S2A_UNIT_TEST
        if (!e->did && s2a_det_in_turn()) ev_expose(e);   // 确定性档：游戏第一次等它 = 第一次对游戏可见（异步建、建时就把句柄给了游戏的事件）
#endif
        return e->signaled; }
    default: *bad = 1; return 0;
    }
}
// ---------- 让步退避（10-24 起缺省开，S2A_YIELD_BACKOFF=0 关；借鉴上游 hle_thread.c 的 yield back-off，MIT，见 docs/第三方说明）----------
// Switch 上没别的线程可让时，让步立即返回、几乎不花钱；宿主上「查时钟 + 让步」的轮询（帧限速、空闲工人等活）却把一个核转满。
// 一轮连续让步 = 每次让步时 x19–x28 都没变（SDK 的 YieldThread 经尾调用直达 svc，这些是轮询循环自己的寄存器：循环每步在推进就会动它们），
// 且两次让步之间本线程没有真阻塞过（等事件 / 锁 / 条件；超时 0 的轮询不算）。中间夹不阻塞的系统调用（服务查询、TryLock、ResetSignal 等）仍算同一轮：
// 10-25 哈迪斯 main+0x27aaa0 的联网监视线程 `for(;;){ SleepThread(0); SubmitNetworkRequest; IsNetworkRequestOnHold…; IsNetworkAvailable; }`
// 每圈夹 3 次服务调用、寄存器不变，原规则「夹任何系统调用就新一轮」使它从不退避，每局上亿次服务调用、烧满一核。
// 头 2 ms 照旧 sched_yield（线程间交接多在这个范围内）；之后每次让步睡「已等时长 / 8」，夹在 10–250 µs。
// 合 Switch 语义：游戏轮询的是内存 / 时钟 / 服务状态，睡只让它晚看到变化（≤ 250 µs），不会少看到唤醒；睡在系统调用里，虚拟核已在 svc_enter 放开，同核线程照常上核。
// 值：1 = 缺省（250 µs 封顶、2 ms 起）；<封顶 µs>[:<起点 µs>] 自定；0 / 不设 = 关
static int64_t now_mono_ns(void);
static int64_t g_yb_cap = -1, g_yb_start = 2000000;
static _Atomic uint64_t g_yb_yields, g_yb_sleeps, g_yb_sleep_ns;
static int yb_on(void) {
    if (g_yb_cap < 0) { const char *e = getenv("S2A_YIELD_BACKOFF"); g_yb_cap = 0;   // 10-24 缺省开（S2A_YIELD_BACKOFF=0 关）：虚拟核改按需后，让步自旋的能耗由它管
        if (!e || !*e) e = "1";
        if (strcmp(e, "0")) { g_yb_cap = 250000; if (strcmp(e, "1")) { g_yb_cap = atoll(e) * 1000; const char *c = strchr(e, ':'); if (c) g_yb_start = atoll(c + 1) * 1000; }
            LOG("让步退避：开（连续让步 %lld µs 后开始睡，每次最多 %lld µs）", (long long)(g_yb_start / 1000), (long long)(g_yb_cap / 1000)); } }
    return g_yb_cap > 0;
}
// 有进展即返回（S2A_YIELD_FAST，缺省开；照上游 thr_yield，MIT）：新一轮（x19–x28 变了或中间真阻塞过）的让步不进内核、直接回游戏。
//   Switch 同优先级轮转不靠这次 sched_yield：虚拟核在 svc_enter 放核、svc_exit 按 yielded 重取（core_claimed）；它只是给宿主调度器的提示。
//   10-26 真机塞尔达 120 秒让步 886 万次（约 7 万次 / 秒内核调用），上游同款 CPU 能耗降约 40%；真机同包 A/B 达标再翻缺省
//   10-29 改缺省开（S2A_YIELD_FAST=0 关）：参考实现 同手段已在塞尔达实测 CPU 108–121% → 76–83% 一核；真机塞尔达 12.3 M 次让步 / 76 秒全走 sched_yield
static int g_yf = -1; static _Atomic uint64_t g_yf_n;
static int yf_on(void) { if (g_yf < 0) { const char *e = getenv("S2A_YIELD_FAST"); g_yf = !(e && !strcmp(e, "0")); LOG("让步有进展即返回：%s", g_yf ? "开（缺省，S2A_YIELD_FAST=0 关）" : "关（S2A_YIELD_FAST=0）"); } return g_yf; }
// SleepThread 时长分布（收局打）：短于 SLEEP_YIELD_NS 的按让出走，其余按 [20 µs, 100 µs) / [100 µs, 1 ms) / [1 ms, 10 ms) / ≥ 10 ms 计
#define SLEEP_YIELD_NS 20000
#define SPIN_GAP_NS 20000   // 两次过闸 / 两次短睡之间游戏代码不超过这么久算同一轮等待（自旋闸、短睡轮询退避共用）
static _Atomic uint64_t g_sl_short, g_sl_hist[4], g_sl_bo, g_sl_bo_ns;
static void sleep_hist(int64_t ns) { atomic_fetch_add_explicit(&g_sl_hist[ns < 100000 ? 0 : ns < 1000000 ? 1 : ns < 10000000 ? 2 : 3], 1, memory_order_relaxed); }
static void yield_bo(thr_t *me, const uint64_t *regs) {
    if (!yb_on()) { sched_yield(); return; }
    uint64_t now = (uint64_t)now_mono_ns(), sig = 0xcbf29ce484222325ull;
    for (int i = 19; i <= 28; i++) sig = (sig ^ regs[i]) * 0x100000001b3ull;
    // 新一轮 = 中间真阻塞过（等事件 / 锁 / 条件），或寄存器变了且「换了调用点或距上次让步返回超过 SPIN_GAP_NS（20 µs）」——循环在干活。
    //   11-04 能耗线：卡比 探索发现主线程让一下看一眼、每圈 x19–x28 都变（计数 / 指针在走），旧判据每次都算新一轮：单局 3.33 亿次让步、退避 0 次，
    //   「有进展即返回」直接回游戏 = 用户态空转占着性能核（nx-81 1.23 W）。同一调用点、两次之间游戏代码 ≤ 20 µs 的仍是同一轮轮询（同短睡退避 10-29 的判轮）；
    //   寄存器不变的照旧同一轮（夹不阻塞的系统调用也算，10-25 哈迪斯）
    static int yr = -1; if (yr < 0) { const char *e = getenv("S2A_YIELD_ROUND"); yr = !(e && !strcmp(e, "0")); if (!yr) LOG("让步判轮：S2A_YIELD_ROUND=0，寄存器一变就算新一轮（11-04 前旧判据，A/B 用）"); }
    int fresh = me->yrun_blk != me->nblk || (me->yrun_sig != sig && (!yr || me->y_lr != regs[30] || now - me->y_ret > SPIN_GAP_NS));
    if (fresh) me->yrun_t0 = now;
    me->yrun_blk = me->nblk; me->yrun_sig = sig; me->y_lr = regs[30];
    atomic_fetch_add_explicit(&g_yb_yields, 1, memory_order_relaxed);
    uint64_t run = now - me->yrun_t0;
    if (fresh && yf_on()) { atomic_fetch_add_explicit(&g_yf_n, 1, memory_order_relaxed); me->y_ret = (uint64_t)now_mono_ns(); return; }
    if (run < (uint64_t)g_yb_start) { sched_yield(); me->y_ret = (uint64_t)now_mono_ns(); return; }
    uint64_t sl = run / 8; if (sl < 10000) sl = 10000; if (sl > (uint64_t)g_yb_cap) sl = (uint64_t)g_yb_cap;
    struct timespec ts = {0, (long)sl}; nanosleep(&ts, NULL);
    atomic_fetch_add_explicit(&g_yb_sleeps, 1, memory_order_relaxed); atomic_fetch_add_explicit(&g_yb_sleep_ns, sl, memory_order_relaxed);
    me->y_ret = (uint64_t)now_mono_ns();
}
// 短睡轮询退避（S2A_YIELD_BACKOFF 同一开关）：短于 1 ms 的 SleepThread 连成一轮（同让步：中间没真阻塞、x19–x28 不变，让步与短睡混着也算），
//   10-29 改判轮：丝之歌每圈睡 25 µs、调用点固定，但 x19–x28 每圈都变（Mac 诊断 40 万次里 39.8 万次因寄存器切开），按寄存器判永远成不了轮。
//   短睡的轮 = 同一调用点（返回地址）、两次之间没有别的系统调用、醒来到下次再睡之间游戏代码不超过 20 µs（同自旋闸 SPIN_GAP_NS）——「睡一下看一眼」。
//   头 2 ms 照原时长睡，之后每次睡 max(原时长, 已等 / 8)，上限见 sl_cap（缺省 250 µs）。10-29 丝之歌 Mac 局 120 秒 242 万次 SleepThread 全在 20–100 µs（真机约 3.8 万次唤醒 / 秒、
//   整机 5.13 W）：线程睡几十微秒查一次有没有活。合 Switch 语义：SleepThread 只保证至少睡这么久，多睡 ≤ 250 µs 只让它晚看到活
// 短睡退避每次最多睡多久（S2A_SLEEP_BACKOFF=<µs>，缺省 250，同让步退避）。轮询线程睡越久越省；帧关键线程靠出图线程的实时声明留在性能核（gfxmode.c 不再关 DM_RT），不靠缩短退避。10-30 测试设备丝之歌同包对照（各 1–2 局）：
//   250 µs 慢帧 2.54% / 2.61%、整机 1.72 / 1.76 W——线程看着太闲，被系统挪到能效核、性能核降到约 2 GHz，成串 33.3 ms 帧；
//   100 µs 慢帧 0.03%、4.03 W，性能核 4.04 GHz；50 µs 慢帧 0、4.69 W；旧宿主（每次精确睡 25 µs）5.13 W
static int64_t g_sl_cap = -1;
static int64_t sl_cap(void) {
    if (g_sl_cap < 0) { const char *e = getenv("S2A_SLEEP_BACKOFF"); g_sl_cap = e && *e ? atoll(e) * 1000 : 250000; if (g_sl_cap < 20000) g_sl_cap = 20000;
        LOG("短睡轮询退避：每次最多睡 %lld µs（S2A_SLEEP_BACKOFF 改）", (long long)(g_sl_cap / 1000)); }
    return g_sl_cap;
}
static int64_t sleep_bo(thr_t *me, const uint64_t *regs, int64_t ns) {
    if (ns >= 1000000) { me->nblk++; return ns; }   // ≥ 1 ms 的睡是真等，同真阻塞：切开让步的轮
#if defined(__APPLE__)   // 安全读栈靠 Mach vm_read_overwrite；安卓上不抽样（sl_up 保持 0，不伪造）
    if (!(me->nsl++ & 4095)) {   // 每 4096 次抽一次：沿 x29 取上两层返回地址（安全读，坏指针不崩）
#if defined(__APPLE__)
        uint64_t fp = regs[29], fr[2]; vm_size_t got = 0;
        for (int k = 0; k < 2 && fp && !(fp & 7); k++) {
            if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, 16, (vm_address_t)fr, &got) != KERN_SUCCESS || got != 16) break;
            me->sl_up[k] = fr[1] & 0x0000007fffffffffull; if (fr[0] <= fp) break; fp = fr[0];
        }
#else
        // 非 Apple 没有 mach 的 vm_read_overwrite：这一轮不抽样，sl_up 置 0（收局按“未取到”处理）。
        // main 的 pr1030 合入时漏了平台守卫，Android 构建在 vm_size_t/mach_task_self 处失败；这里先局部补上。
        me->sl_up[0] = me->sl_up[1] = 0;
#endif
    }
#else
    me->nsl++;
#endif
    if (!yb_on()) return ns;
    uint64_t now = (uint64_t)now_mono_ns(), seq = atomic_load_explicit(&me->nsvc_all, memory_order_relaxed);
    if (me->sl_lr != regs[30] || me->sl_seq + 1 != seq || now - me->sl_ret > SPIN_GAP_NS) me->sl_t0 = now;
    me->sl_lr = regs[30]; me->sl_seq = seq;
    uint64_t run = now - me->sl_t0;
    if (run < (uint64_t)g_yb_start) return ns;
    uint64_t sl = run / 8; if (sl > (uint64_t)sl_cap()) sl = (uint64_t)sl_cap();
    if (sl <= (uint64_t)ns) return ns;
    atomic_fetch_add_explicit(&g_sl_bo, 1, memory_order_relaxed); atomic_fetch_add_explicit(&g_sl_bo_ns, sl - (uint64_t)ns, memory_order_relaxed);
    me->nsl_bo++; me->sl_bo_ns += sl - (uint64_t)ns;
    return (int64_t)sl;
}
// ---------- 自旋闸（10-24 起缺省开，S2A_SPIN=0 关；包要经 S2A_REWRITE_SPIN 改写（出包缺省开）才有这类门。借鉴上游 dm_spin_wait，MIT）----------
// 改写器认出的「只读内存、自己离不开」的忙等循环，第二圈起每圈过这里（见 runtime/rewriter/rewriter.c 自旋等待）。Switch 上这种轮询只占自己那个核，
// 宿主上却吃掉干活线程的核。一轮等待 = 两次过闸之间没有系统调用、游戏代码不超过 20 µs；头 2 ms 照转（多是工人之间的短交接），之后每圈睡 run/8（10–250 µs）。
// 合 Switch 语义：循环只读内存，睡只让它晚看到别的线程的写（≤ 250 µs），不会少看到；开虚拟核时睡前放核、睡完按优先级重取（同核线程在这期间照常上核）。
// 确定性档不等（串行执行下睡只拖慢）。S2A_SPIN_POLL=<µs> 改头段时长
static int g_spin_on = -1; static uint64_t g_spin_poll = 2000000;
static _Atomic uint64_t g_spin_pass, g_spin_sleeps, g_spin_sleep_ns;
void s2a_spin_wait(uint64_t site) {
    (void)site;
    if (g_spin_on < 0) { const char *e = getenv("S2A_SPIN"), *pl = getenv("S2A_SPIN_POLL"); g_spin_on = !(e && !strcmp(e, "0"));   // 10-24 缺省开（S2A_SPIN=0 关）
        if (pl && *pl) g_spin_poll = strtoull(pl, NULL, 10) * 1000;
        LOG("自旋闸：%s（头 %llu µs 照转，之后每圈最多睡 250 µs）", g_spin_on ? "开（缺省，S2A_SPIN=0 关）" : "关（S2A_SPIN=0）", (unsigned long long)(g_spin_poll / 1000)); }
    atomic_fetch_add_explicit(&g_spin_pass, 1, memory_order_relaxed);
    thr_t *me = thread_current_or_null();
    if (!g_spin_on || !me) return;
#ifndef S2A_UNIT_TEST
    if (s2a_det_mode()) return;
#endif
    uint64_t now = (uint64_t)now_mono_ns();
    uint64_t sequence = atomic_load_explicit(&me->nsvc_all, memory_order_relaxed);
    if (me->srun_seq != sequence || now - me->spin_ret > SPIN_GAP_NS) me->srun_t0 = now;   // 新一轮：中间有系统调用，或游戏代码跑了不止一圈的工夫
    me->srun_seq = sequence;
    uint64_t run = now - me->srun_t0;
    if (run >= g_spin_poll) {
        uint64_t sl = run / 8; if (sl < 10000) sl = 10000; if (sl > 250000) sl = 250000;
        int held = cores_on() && atomic_load(&me->held_core);
        if (held) { atomic_store(&me->off_guest, 1); core_release(me); }
        struct timespec ts = {0, (long)sl}; nanosleep(&ts, NULL);
        if (held) { core_acquire(me); atomic_store(&me->off_guest, 0); }
        atomic_fetch_add_explicit(&g_spin_sleeps, 1, memory_order_relaxed); atomic_fetch_add_explicit(&g_spin_sleep_ns, sl, memory_order_relaxed);
    }
    me->spin_ret = (uint64_t)now_mono_ns();
}
// ---------- 读时钟闸（10-26 缺省关，S2A_TICK_WAIT=1 开）----------
// 10-26 改缺省关：暗黑3 加载阶段有按圈数推进、每圈读一次时钟的循环，闸把它当成按时间空等，每圈睡 250 µs：真机冷启 60 秒 0 帧（睡了 53.7 秒）、
//   Mac 首帧 6.7 → 16.9 秒。「连续读时钟」分不出「等时间」与「按圈数干活顺带计时」；重开前要有能区分两者的判据，并在真机冷启暗黑3 + 哈迪斯验过
// 游戏按时钟等下一帧：哈迪斯直连主循环（main+0x2496fc）每圈 GetSystemTick → 算 dt → update / render，时间没到两者立即返回，
//   整圈只是在读时钟（Mac 采样 15 秒：nx-81 89% 样本在 cntpct 门 / mach_absolute_time，7% 在 update / render），真机烧满一个能效核（0.97 核、约 0.37 W）。
//   Switch 上这只占游戏自己那个核；宿主上是白烧。读时钟不是系统调用，让步退避和自旋闸（只认只读内存的忙等）都管不到。
// 一轮等待 = 同一调用点（门位置 ^ 调用方返回地址：游戏都经 SDK 同一个 GetSystemTick 读，只看门位置分不出是哪个循环；
//   干活的线程给任务计时是起止两个调用点交替，不会被当成一轮）连续读、两次之间没有系统调用、游戏代码不超过 20 µs。
// 头 2 ms 照转，之后每次读前睡 run/8（10–250 µs），同自旋闸。合 Switch 语义：只让它晚 ≤ 250 µs 看到时间，帧预算 16.7 / 33.3 ms 内；
//   开虚拟核时睡前放核、睡完按优先级重取。确定性档不睡
static int g_tw_on = -1;
static _Atomic uint64_t g_tw_pass, g_tw_sleeps, g_tw_sleep_ns;
void s2a_tick_wait(uint64_t key) {
    if (g_tw_on < 0) { const char *e = getenv("S2A_TICK_WAIT"); g_tw_on = e && !strcmp(e, "1");
        LOG("读时钟闸：%s（同一调用点连续读时钟满 2 ms 后每次最多睡 250 µs）", g_tw_on ? "开（S2A_TICK_WAIT=1）" : "关（缺省，S2A_TICK_WAIT=1 开）"); }
    atomic_fetch_add_explicit(&g_tw_pass, 1, memory_order_relaxed);
    thr_t *me = thread_current_or_null();
    if (!g_tw_on || !me) return;
#ifndef S2A_UNIT_TEST
    if (s2a_det_mode()) return;
#endif
    uint64_t now = (uint64_t)now_mono_ns();
    if (me->trun_key != key || me->trun_seq != me->nsvc_all || now - me->tick_ret > SPIN_GAP_NS) me->trun_t0 = now;   // 新一轮：换了调用点、中间有系统调用，或游戏代码跑了不止一圈的工夫
    me->trun_key = key; me->trun_seq = me->nsvc_all;
    uint64_t run = now - me->trun_t0;
    if (run >= g_spin_poll) {
        uint64_t sl = run / 8; if (sl < 10000) sl = 10000; if (sl > 250000) sl = 250000;
        int held = cores_on() && atomic_load(&me->held_core);
        if (held) { atomic_store(&me->off_guest, 1); core_release(me); }
        struct timespec ts = {0, (long)sl}; nanosleep(&ts, NULL);
        if (held) { core_acquire(me); atomic_store(&me->off_guest, 0); }
        atomic_fetch_add_explicit(&g_tw_sleeps, 1, memory_order_relaxed); atomic_fetch_add_explicit(&g_tw_sleep_ns, sl, memory_order_relaxed);
    }
    me->tick_ret = (uint64_t)now_mono_ns();
}
#ifdef S2A_UNIT_TEST
void s2a_test_tick_wait(int on) { g_tw_on = on; g_spin_poll = 2000000; }
uint64_t s2a_test_tick_sleeps(void) { return atomic_load(&g_tw_sleeps); }
#endif
#ifdef S2A_UNIT_TEST
void s2a_test_spin(int on) { g_spin_on = on; g_spin_poll = 2000000; }
uint64_t s2a_test_spin_sleeps(void) { return atomic_load(&g_spin_sleeps); }
#endif
void s2a_yield_stat_log(const char *when) {
    if (g_spin_on >= 0 && atomic_load(&g_spin_pass))
        LOG("自旋闸 [%s]：过闸 %llu 次，睡 %llu 次、共 %.1f ms", when, (unsigned long long)atomic_load(&g_spin_pass), (unsigned long long)atomic_load(&g_spin_sleeps), atomic_load(&g_spin_sleep_ns) / 1e6);
    if (g_tw_on >= 0 && atomic_load(&g_tw_pass))
        LOG("读时钟闸 [%s]：读时钟 %llu 次，睡 %llu 次、共 %.1f ms", when, (unsigned long long)atomic_load(&g_tw_pass), (unsigned long long)atomic_load(&g_tw_sleeps), atomic_load(&g_tw_sleep_ns) / 1e6);
    LOG("SleepThread [%s]：短于 20 µs 按让出 %llu 次；20–100 µs %llu、0.1–1 ms %llu、1–10 ms %llu、≥10 ms %llu 次；短睡轮询退避拉长 %llu 次、共多睡 %.1f ms", when, (unsigned long long)atomic_load(&g_sl_short),
        (unsigned long long)atomic_load(&g_sl_hist[0]), (unsigned long long)atomic_load(&g_sl_hist[1]), (unsigned long long)atomic_load(&g_sl_hist[2]), (unsigned long long)atomic_load(&g_sl_hist[3]),
        (unsigned long long)atomic_load(&g_sl_bo), atomic_load(&g_sl_bo_ns) / 1e6);
    {   // 按线程：短睡最多的 6 条（线程号 / 优先级 / 是否主线程 / 宿主线程名、短睡次数、被拉长次数、多睡 ms）
        thr_t *top[6] = {0}; glock();
        for (thr_t *t = g_all; t; t = t->all) { if (!t->nsl) continue;
            for (int i = 0; i < 6; i++) if (!top[i] || t->nsl > top[i]->nsl) { for (int j = 5; j > i; j--) top[j] = top[j - 1]; top[i] = t; break; } }
        for (int i = 0; i < 6 && top[i]; i++) { char nm[64] = "", at[160] = "?";
#if defined(__APPLE__)   // bionic 只在 _GNU_SOURCE 下声明 pthread_getname_np；安卓上线程名留空
            if (top[i]->pt) pthread_getname_np(top[i]->pt, nm, sizeof nm);
#endif
            { uint64_t a = top[i]->sl_lr & 0x0000007fffffffffull; module_t *m = a ? module_of((void *)a) : NULL;   // 最近一次短睡的返回地址：定它在轮询什么
              if (m) snprintf(at, sizeof at, "%s+%#llx", m->name, (unsigned long long)(a - (uint64_t)m->base)); else if (a) snprintf(at, sizeof at, "%#llx", (unsigned long long)a);
              for (int k = 0; k < 2; k++) { uint64_t u = top[i]->sl_up[k]; module_t *um = u ? module_of((void *)u) : NULL; size_t o = strlen(at);
                  if (um) snprintf(at + o, sizeof at - o, " ← %s+%#llx", um->name, (unsigned long long)(u - (uint64_t)um->base)); } }
            LOG("SleepThread [%s] 按线程 #%d：线程 %llu「%s」优先级 %d%s，短睡 %llu 次、被拉长 %llu 次、多睡 %.1f ms；最近调用点 %s", when, i + 1, (unsigned long long)top[i]->id, nm[0] ? nm : "?", top[i]->prio,
                top[i]->is_main ? "（游戏主线程）" : "", (unsigned long long)top[i]->nsl, (unsigned long long)top[i]->nsl_bo, top[i]->sl_bo_ns / 1e6, at); }
        pthread_mutex_unlock(&G); }
    if (g_yb_cap <= 0) return;
    LOG("让步退避 [%s]：让步 %llu 次，其中退避睡 %llu 次、共 %.1f ms；有进展直接返回 %llu 次", when, (unsigned long long)atomic_load(&g_yb_yields), (unsigned long long)atomic_load(&g_yb_sleeps), atomic_load(&g_yb_sleep_ns) / 1e6, (unsigned long long)atomic_load(&g_yf_n));
}
#ifdef S2A_UNIT_TEST
void s2a_test_yield_bo(int on) { g_yb_cap = on ? 250000 : 0; g_yb_start = 2000000; }
void s2a_test_yield_fast(int on) { g_yf = on; }
uint64_t s2a_test_yield_fastret(void) { return atomic_load(&g_yf_n); }
uint64_t s2a_test_yield_sleeps(void) { return atomic_load(&g_yb_sleeps); }
uint64_t s2a_test_sleep_short(void) { return atomic_load(&g_sl_short); }
uint64_t s2a_test_sleep_bo(void) { return atomic_load(&g_sl_bo); }
#endif
#ifdef S2A_UNIT_TEST
// 单测钉钟（10-25 t_thread 偶发根修）：≥ 0 时单调钟取这个值、由单测按事件推进。让步退避 / 自旋闸 / 唤醒让先的判定都读单调钟，
//   用墙钟驱动时测试线程被宿主切走就会改变判定（高负载下偶发红）；钉住后期望次数可按调用序列手算
static _Atomic int64_t g_test_mono = -1;
void s2a_test_mono_set(int64_t ns) { atomic_store(&g_test_mono, ns); }
#endif
static int64_t now_mono_ns(void) {
#ifdef S2A_UNIT_TEST
    int64_t f = atomic_load(&g_test_mono); if (f >= 0) return f;
#endif
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ll + ts.tv_nsec; }

// ---------- 暂停与上下文（0x32 / 0x33）----------
// 目标在系统调用里 → 出入口挂起，上下文取门帧；在跑游戏代码 → Mach 挂起，只接受停在模块原始代码区（补丁区门块临时借用 x29 / x30），
// 停在宿主代码里（可能持 malloc 等宿主锁）也放开重试，2 秒上限
static int pc_in_guest_text(uint64_t pc) {
    module_t *m = module_of((void *)pc); if (!m) return 0;
    uint64_t off = pc - (uint64_t)m->base;
    if (m->patch2_len && s2a_in_patch(m, pc)) return 0;   // 双补丁岛的另一个岛（基址前 / 键页后）不算
    return m->patch_off >= 0 ? off < (uint64_t)m->patch_off : pc >= (uint64_t)m->base;
}
#if defined(__APPLE__)
// 停稳条件同安卓（审计 P1-49）：只有 paused_ack 置上才算停住、才回 0——目标自己进 pause_park；在系统调用里（出口必再认暂停，svc_leave_check）；
//   在游戏代码里被 Mach 挂住（平台挂起原语，安卓是 SIGURG 让目标自己停）。后两种由暂停方持 G 代发（pwait），恢复方收回。
//   原先在系统调用里 / 已退出 / Mach 挂起失败都直接回 0（暂停标志已是 1），Mach 挂住后的 pmach 在 G 外发布、恢复方或停全部线程可能看不到而不放开
static uint32_t thr_pause(thr_t *t) {
    glock(); int already = atomic_load(&t->pause); if (!already) atomic_store(&t->pause, 1); pthread_mutex_unlock(&G);
    if (already) return R_BAD_STATE;
    uint64_t t0 = now_ns();
    for (int k = 0;; k++) {
        glock();
        if (t->exited || !atomic_load(&t->pause)) {   // 已退出（清掉自己置的暂停）/ 暂停已被恢复或停全部线程收走：不算停住
            if (t->exited && atomic_load(&t->pause)) { atomic_store(&t->pause, 0); pthread_cond_broadcast(&g_pause_cv); }
            pthread_mutex_unlock(&G); return R_BAD_STATE;
        }
        if (atomic_load_explicit(&t->paused_ack, memory_order_acquire)) { pthread_mutex_unlock(&G); return 0; }
        if (atomic_load(&t->in_svc)) { t->pwait = 1; atomic_store_explicit(&t->paused_ack, 1, memory_order_release); pthread_mutex_unlock(&G); return 0; }
        pthread_mutex_unlock(&G);
        pthread_mutex_lock(&g_susp_mu);   // 与采样读 PC 互斥（suspend 计数要配平，见 thr_pc_lr）
        if (thread_suspend(t->mport) != KERN_SUCCESS) {   // 挂不住：回错并清掉刚置的暂停（目标这期间自己停进 pause_park 的照算停住）
            pthread_mutex_unlock(&g_susp_mu);
            glock(); uint32_t r = atomic_load(&t->paused_ack) ? 0 : R_BAD_STATE;
            if (r && atomic_load(&t->pause)) { atomic_store(&t->pause, 0); pthread_cond_broadcast(&g_pause_cv); }
            pthread_mutex_unlock(&G); return r;
        }
        arm_thread_state64_t st; arm_neon_state64_t ns; mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
        int guest = !atomic_load(&t->in_svc) && thread_get_state(t->mport, ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS && pc_in_guest_text(__darwin_arm_thread_state64_get_pc(st));
        if (guest) { mach_msg_type_number_t nn = ARM_NEON_STATE64_COUNT; thread_get_state(t->mport, ARM_NEON_STATE64, (thread_state_t)&ns, &nn); }
        pthread_mutex_unlock(&g_susp_mu);   // 先放 g_susp_mu 再拿 G（thr_pc_lr 是持 G 再拿 g_susp_mu）
        if (guest) {
#ifdef S2A_UNIT_TEST
            if (s2a_test_pause_suspended) s2a_test_pause_suspended(t);
#endif
            // 持 G 发布：恢复方 / 停全部线程都在 G 下取 pmach 放开线程，暂停还在才发布；已被收走就自己放开
            glock();
            if (atomic_load(&t->pause) && !t->exited) {
                t->pst = st; t->pns = ns; t->pmach = 1; t->pwait = 1; atomic_store_explicit(&t->paused_ack, 1, memory_order_release);
                pthread_mutex_unlock(&G);
                // 停在游戏代码里被挂起：自己放不了核，由暂停方代放（S2A_CORES；否则整核被游戏自己暂停的线程卡死——原生层线 10-20 指出）；恢复后下一次 svc_exit 重取
                core_release(t); return 0;
            }
            pthread_mutex_unlock(&G);
            pthread_mutex_lock(&g_susp_mu); thread_resume(t->mport); pthread_mutex_unlock(&g_susp_mu);
            return R_BAD_STATE;
        }
        pthread_mutex_lock(&g_susp_mu); thread_resume(t->mport); pthread_mutex_unlock(&g_susp_mu);
        if (now_ns() - t0 > 2000000000ull) {
            // 取证（D-QJTH）：目标这 2 秒一直在宿主代码里（不在游戏代码、不在系统调用）——记下最后一次挂住时的 pc / lr 落在哪个宿主函数，
            //   定它是在哪种宿主调用里长时间不回（图形 API 等 GPU？），再判该等它回游戏代码还是直接致命
            uint64_t pc = __darwin_arm_thread_state64_get_pc(st), lr = __darwin_arm_thread_state64_get_lr(st); Dl_info di = {0}, dl = {0};
            int okp = n == ARM_THREAD_STATE64_COUNT && dladdr((void *)pc, &di), okl = n == ARM_THREAD_STATE64_COUNT && dladdr((void *)lr, &dl);
            die("【红】SetThreadActivity：线程 %#x 2 秒内没停到游戏代码或系统调用里（重试 %d 次）；最后一次挂住时 pc %#llx（%s+%#llx）lr %#llx（%s+%#llx）", t->handle, k,
                (unsigned long long)pc, okp && di.dli_sname ? di.dli_sname : "?", (unsigned long long)(okp && di.dli_saddr ? pc - (uint64_t)di.dli_saddr : 0),
                (unsigned long long)lr, okl && dl.dli_sname ? dl.dli_sname : "?", (unsigned long long)(okl && dl.dli_saddr ? lr - (uint64_t)dl.dli_saddr : 0));
        }
        usleep(20);
    }
}
static void thr_unpin(thr_t *t) { glock(); --t->pins; if (!t->pins && t->exited && t->closed) thr_free(t); pthread_mutex_unlock(&G); }
// 诊断：挂起全部游戏线程（S2A_GUEST_PAUSE_AT，host_main.c；能耗减法实验「只跑我们的层」，帧率能耗线 10-20）。
// 渲染循环 / 服务层 / 声音在宿主线程上，照跑。逐条走 SetThreadActivity 同一套 thr_pause（停在游戏代码或系统调用出口）。返回挂起的条数
int s2a_guest_pause_all(void) {
    enum { MAXT = 512 }; thr_t *v[MAXT]; int n = 0, ok = 0;
    glock(); for (thr_t *t = g_all; t && n < MAXT; t = t->all) if (t->started && !t->exited && t->mport) { ++t->pins; v[n++] = t; } pthread_mutex_unlock(&G);
    for (int i = 0; i < n; i++) { if (thr_pause(v[i]) == 0) ok++; thr_unpin(v[i]); }
    return ok;
}
// 即时存档可行性：冻结不退出探针（快照开时才开：苹果端缺省开、S2A_SNAP_MODE=0 关，或恢复局 S2A_SNAP_RESTORE）。置全局冻结，轮询直到每条游戏线程都到安全点：
//   ① 自己停进 pause_park（系统调用出入口 / 回客体）；② 阻塞在内核等待里（in_svc 且 wkind≠0、未被唤醒）；③ 在游戏代码里被 Mach 挂住。
//   在系统调用里正在处理（in_svc、无等待）或在宿主函数里的算「未到」，继续等；超时不致命，记下未到的线程。返回到齐用时（微秒，-1 = 超时）
// ---------- 即时存档：冻结到「门里」安全点并抓线程现场 / 句柄表 ----------
// 安全点只认门里的四种：入口停（park_at 1，门帧是入参）、出口停（2，门帧是返回值）、内核等待中（未被唤醒）、SleepThread 睡着。
//   不 Mach 挂起游戏代码里的线程（现场不在门帧里），冻结期间它们跑到下一次系统调用入口自己停；宿主函数回客体处不停（s2a_guest_return 看 g_snap_frz）。
// 成功回 0 并保持冻结（g_frz、g_snap_frz 不清；调用方存完就退出进程）；超时回 -1 并放开
static uint64_t tsd_slot(thr_t *t, pthread_key_t k);
enum { SNAP_T_ENTRY = 1, SNAP_T_EXIT = 2, SNAP_T_WAIT = 3, SNAP_T_SLEEP = 4, SNAP_T_DEAD = 5, SNAP_T_UNSTARTED = 6, SNAP_T_RUN = 7 };   // RUN：停在游戏原代码里、Mach 挂住
static int snap_state_raw(thr_t *t);
// 门帧没有切栈槽 = 在宿主栈上进的门（如 NVN 命令内存回调里调游戏函数、游戏函数再进门）：帧下面是宿主返回地址，快照存不下，不算安全点，等它出来再冻
static int snap_state(thr_t *t) {   // 持 G
    int st = snap_state_raw(t);
    if (st >= SNAP_T_ENTRY && st <= SNAP_T_SLEEP && t->svc_regs && !((const uint64_t *)t->svc_regs)[816 / 8]) return 0;
    return st;
}
// 同步 IPC（0x21 走 TLS 消息）在 C# 服务里已阻塞 0.3 秒以上：回复还没写，门帧与 TLS 里的请求原样——当入口安全点（恢复后重发这次请求）。
//   短请求不算（照旧等它回门停在出口），免得判齐后它恰好完成、这次存档作废
static int snap_cs_blocked(thr_t *t) { return atomic_load(&t->in_svc) && t->cs_svc == 0x21 && t->tlr && t->svc_regs && now_ns() - t->cs_t0 > 300000000ull; }
static int snap_state_raw(thr_t *t) {   // 持 G
    if (t->exited) return SNAP_T_DEAD;
    if (!t->started && !t->is_main) return SNAP_T_UNSTARTED;
#if defined(__APPLE__)
    if (t->snap_run) return SNAP_T_RUN;
#endif
    if (atomic_load(&t->paused_ack) && (t->park_at == 1 || t->park_at == 2)) return t->park_at == 1 ? SNAP_T_ENTRY : SNAP_T_EXIT;
    int ins = atomic_load(&t->in_svc);
    if (ins && t->wkind && !t->woke) return SNAP_T_WAIT;
    if (ins && t->last_svc == 0x0B && !atomic_load(&t->paused_ack)) return SNAP_T_SLEEP;
    if (snap_cs_blocked(t)) return SNAP_T_ENTRY;
    return 0;
}
#if defined(__APPLE__)
// 放开冻结 / 恢复时 Mach 挂住的线程：虚拟核开着且它此刻不持核（冻结方代放过、或恢复局新建的）→ 先挂抢占信号，放开后执行任何游戏指令前进 tramp 取核
static void snap_run_resume(thr_t *t) {
    if (core_part(t) && !t->held_core && t->pt) { atomic_store(&t->preempting, 1); pthread_kill(t->pt, S2A_SIG_PREEMPT); }
    thread_resume(t->mport);
}
#endif
// 解冻（存档进程）：Mach 挂住的线程放开（停在原指令上，接着跑）；门里停着的线程放开冻结标志后各自继续。冻结放弃、存完接着玩都走这里
void s2a_snap_thaw(void) {
#if defined(__APPLE__)
    // 冻结时代放过核的（off_guest 标着）：放开前先挂抢占信号，回游戏代码执行任何指令前先进 tramp 按优先级取核（同 thr_unpause）
    glock(); for (thr_t *t = g_all; t; t = t->all) if (t->snap_run) { t->snap_run = 0; snap_run_resume(t); } pthread_mutex_unlock(&G);
#endif
    glock(); atomic_store(&g_frz, 0); atomic_store(&g_snap_frz, 0); pthread_cond_broadcast(&g_pause_cv); pthread_mutex_unlock(&G);
}
#if !defined(S2A_UNIT_TEST)
void s2a_where(uint64_t a, char *b, size_t n); int s2a_frames_of(uint64_t fp, uint64_t *lr, int max);   // crash.c
#endif
int s2a_snap_freeze(int timeout_ms, int *nthr, int *nst) {
    glock(); atomic_store(&g_snap_frz, 1); atomic_store(&g_frz, 1); pthread_mutex_unlock(&G);
    uint64_t t0 = now_ns(), dl = t0 + (uint64_t)timeout_ms * 1000000;
    for (;;) {
        int n = 0, left = 0; memset(nst, 0, sizeof(int) * 8);
        glock();
        for (thr_t *t = g_all; t; t = t->all) { n++; int st = snap_state(t); nst[st]++; if (!st) left++; }
        if (!left) {   // 判齐：C# 里阻塞的那些，此刻另存门帧与请求消息（之后它若完成、C# 会改写这两处）
            for (thr_t *t = g_all; t; t = t->all) { t->snap_cs = 0; if (atomic_load(&t->paused_ack) || t->wkind || !snap_cs_blocked(t)) continue;
                t->snap_cs_ret = atomic_load(&t->cs_ret); memcpy(t->snap_cs_frame, t->svc_regs, sizeof t->snap_cs_frame); memcpy(t->snap_cs_msg, t->tlr, sizeof t->snap_cs_msg);
                if (atomic_load(&t->cs_ret) == t->snap_cs_ret && snap_cs_blocked(t)) { t->snap_cs = 1; t->snap_cs_seq = t->cs_seq; LOG("即时存档：线程 id %llu 在服务请求里阻塞（会话 %#x），按入口记、恢复后重发", (unsigned long long)t->id, t->cs_h); } }
            *nthr = n; pthread_mutex_unlock(&G); return (int)((now_ns() - t0) / 1000); }   // 持 G 判定全员到齐；之后调用方再持 G 抓现场
#if defined(__APPLE__)
        // 0.5 秒还没到门的：不在系统调用里、停在游戏原代码上（忙等等着别的已冻线程）——Mach 挂住、取完整现场，当作安全点；停在宿主代码 / 补丁岛的放开再等
        thr_t *rel[64]; int nrel = 0;   // 本轮挂住、持着虚拟核的：解 G 后代放核（持 G 取 g_cmu 与「持 g_cmu 等 G」互等）
        if (now_ns() - t0 > 500000000ull) for (thr_t *t = g_all; t; t = t->all) {
            if (snap_state(t) || t->snap_run || !t->mport || atomic_load(&t->in_svc)) continue;
            if (thread_suspend(t->mport) != KERN_SUCCESS) continue;
            arm_thread_state64_t st; mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT; arm_neon_state64_t ns; mach_msg_type_number_t nc = ARM_NEON_STATE64_COUNT;
            if (!atomic_load(&t->in_svc) && thread_get_state(t->mport, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS && pc_in_guest_text(__darwin_arm_thread_state64_get_pc(st))
                && thread_get_state(t->mport, ARM_NEON_STATE64, (thread_state_t)&ns, &nc) == KERN_SUCCESS) {
                if (!t->snap_mc && !(t->snap_mc = calloc(1, sizeof *t->snap_mc))) { thread_resume(t->mport); continue; }
                struct snap_mach_ctx *mc = t->snap_mc;
                for (int r = 0; r < 29; r++) mc->regs[r] = st.__x[r];
                mc->regs[29] = __darwin_arm_thread_state64_get_fp(st); mc->regs[30] = __darwin_arm_thread_state64_get_lr(st);
                mc->regs[31] = __darwin_arm_thread_state64_get_sp(st); mc->regs[32] = __darwin_arm_thread_state64_get_pc(st); mc->regs[33] = st.__cpsr;
                memcpy(mc->neon, &ns.__v, sizeof mc->neon); mc->fpsr = ns.__fpsr; mc->fpcr = ns.__fpcr; t->snap_run = 1;
                if (t->held_core && nrel < 64) { atomic_store(&t->off_guest, 1); rel[nrel++] = t; }
                LOG("即时存档：线程 id %llu 停在游戏代码 %#llx（忙等不进门），Mach 挂住取现场", (unsigned long long)t->id, (unsigned long long)mc->regs[32]);
            } else thread_resume(t->mport);
        }
        pthread_mutex_unlock(&G);
        // 虚拟核开着（S2A_CORES=1 / 按需）：挂住的忙等线程手里的核由冻结方代放（同游戏 svc 暂停停在游戏代码里的线程，thr_pause）。
        //   不放的话同核另一条线程若正卡在出口取核（in_svc 还是 1、不算安全点）永远等不到，冻结到超时、这次存档失败
        for (int i = 0; i < nrel; i++) core_release(rel[i]);
        glock();
#endif
        pthread_mutex_unlock(&G);
        if (now_ns() > dl) {   // 没到齐：逐条说清哪条线程、停在哪、为什么不算安全点（定因用）
            glock(); int k = 0;
            for (thr_t *t = g_all; t; t = t->all) { if (snap_state(t)) continue; int raw = snap_state_raw(t);
                if (k++ < 8) LOG("即时存档诊断：线程 id %llu 没到安全点（原始停法 %d、在系统调用 %d、最近 svc %#x、等待种类 %d、门帧%s切栈槽）", (unsigned long long)t->id, raw, atomic_load(&t->in_svc), t->last_svc, t->wkind,
                                 t->svc_regs ? (((const uint64_t *)t->svc_regs)[816 / 8] ? "有" : "没有") : "（无）");
#if defined(__APPLE__) && !defined(S2A_UNIT_TEST)   // 单测单独链本文件、不链 crash.c
                // 停在宿主函数里的（门里调宿主、宿主那边等着）：挂一下只取 pc 与帧链（只读内存、不取锁），放开后再符号化打日志——
                //   挂着时打日志会和被挂线程手里的锁（malloc / dyld）互等。定因用：说清卡在哪个宿主函数
                if (k <= 8 && t->mport && thread_suspend(t->mport) == KERN_SUCCESS) {
                    arm_thread_state64_t st; mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT; uint64_t pc = 0, lr[12]; int nf = 0;
                    if (thread_get_state(t->mport, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
                        pc = (uint64_t)__darwin_arm_thread_state64_get_pc(st); nf = s2a_frames_of((uint64_t)__darwin_arm_thread_state64_get_fp(st), lr, 12); }
                    thread_resume(t->mport);
                    if (pc) { char w[160]; s2a_where(pc, w, sizeof w); LOG("即时存档诊断：线程 id %llu 现在 pc %s", (unsigned long long)t->id, w);
                        for (int f = 0; f < nf; f++) { s2a_where(lr[f], w, sizeof w); LOG("即时存档诊断：  #%d %s", f, w); } } }
#endif
            }
            pthread_mutex_unlock(&G);
            *nthr = n; s2a_snap_thaw(); return -1; }
        usleep(100);
    }
}
// 存档写完前核对：判齐时按入口记下的「C# 里阻塞」线程，期间有没有哪条的请求完成了（完成 = 回复已写进游戏内存 / 请求日志，快照前后不一致）。回完成了的条数
// 按入口记的阻塞请求在存档途中完成了：只有进了服务请求日志的才作数（它完成时可能建句柄、改内核账，而内核账正在写）。
//   它完成后才追加进请求日志（SnapLog.cs 按完成先后记），可能已写进刚存的日志之外，恢复时照常从入口重发；不进日志的（套接字等待 / 读取）完成了也不改服务状态，回复写进的 TLS 恢复时按判齐那一刻的请求消息写回、重发，输出缓冲在回复前对游戏无意义——照存
int s2a_snap_cs_moved(void) {
    int n = 0, quiet = 0; glock();
    for (thr_t *t = g_all; t; t = t->all) if (t->snap_cs && atomic_load(&t->cs_ret) != t->snap_cs_ret) { if (t->snap_cs_seq >= 0) n++; else quiet++; }
    pthread_mutex_unlock(&G);
    if (quiet) LOG("即时存档：%d 条不进请求日志的阻塞请求在存档途中等到了回复（不影响这份快照，恢复后照常重发）", quiet);
    return n;
}
static int sw(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n ? 0 : -1; }
// 线程记录（版本 1）。门帧 832 字节原样存：[0,800) 寄存器 / 状态、[800] 客体 sp（含借的 32 字节）；客体栈上借存的 x16/x17 随游戏内存走
typedef struct {
    uint32_t handle, state, last_svc, prio, base_prio, core, started, exited, closed, cancel, is_main, pinned, pin_core, wkind, wtype, wtag, yielded, _p;
    uint64_t id, entry, arg, stack_top, mask, pin_mask, waddr, wkey, wtmo_left, wseq, tlr, k18, ktls, ktlr, own_tls, own_obj;
    int32_t wval, _q;
    uint8_t frame[832];
    uint64_t rregs[34]; uint8_t rneon[32 * 16]; uint32_t rfpsr, rfpcr;   // RUN：Mach 现场（x0–x28、fp、lr、sp、pc、cpsr；向量寄存器；浮点状态）
    uint8_t csmsg[256]; uint32_t has_csmsg;   // 入口停在 C# 阻塞的 IPC 里：判齐那一刻的 TLS 请求消息（恢复时写回再重发）
} snap_thr_rec;
#include "snapfmt.h"
void s2a_snap_mark_old(void);   // snapshot.c：记「旧版快照接不上」（给玩家的提示分类）
// 线程记录的字段号（带版本格式，snapfmt.h）：标量各一号；门帧按语义存（通用寄存器 / 向量寄存器 / 标志 / 浮点状态 / 客体 sp / 有没有切栈槽），
//   恢复时由本版按自己的门帧格局（entry.S）摆回——门帧格局变了也接得上。版本 1
#define SNAP_THR_FIELDS(X) \
    X(1, handle) X(2, state) X(3, last_svc) X(4, prio) X(5, base_prio) X(6, core) X(7, started) X(8, exited) X(9, closed) X(10, cancel) X(11, is_main) \
    X(12, pinned) X(13, pin_core) X(14, wkind) X(15, wtype) X(16, wtag) X(17, yielded) X(20, id) X(21, entry) X(22, arg) X(23, stack_top) X(24, mask) \
    X(25, pin_mask) X(26, waddr) X(27, wkey) X(28, wtmo_left) X(29, wseq) X(30, tlr) X(31, k18) X(32, ktls) X(33, ktlr) X(34, own_tls) X(35, own_obj) X(36, wval)
enum { THR_F_XREGS = 50, THR_F_VREGS = 51, THR_F_NZCV = 52, THR_F_FPSR = 53, THR_F_FPCR = 54, THR_F_GSP = 55, THR_F_SWSLOT = 56, THR_F_GATE_ORIG = 57, THR_F_GATE_KIND = 58,
       THR_F_RUN_REGS = 60, THR_F_RUN_NEON = 61, THR_F_RUN_FPSR = 62, THR_F_RUN_FPCR = 63, THR_F_CS_MSG = 64 };
int s2a_gate_identity(uint64_t ret, uint64_t *orig, int *kind); uint64_t s2a_gate_by_identity(uint64_t orig, int kind);   // gate.c
// 单独链本文件的线程单测没有门表：弱缺省 = 门身份查不到；宿主 / iOS 壳链进 gate.c 的真定义
__attribute__((weak)) int s2a_gate_identity(uint64_t ret, uint64_t *orig, int *kind) { (void)ret; (void)orig; (void)kind; return -1; }
__attribute__((weak)) uint64_t s2a_gate_by_identity(uint64_t orig, int kind) { (void)orig; (void)kind; return 0; }
static int g_snap_gate_miss;   // 恢复：门身份在本版门表里找不到的线程数（> 0 = 改写器约定变了，C 档拒绝）
// 本版门帧格局（entry.S sw_gate）：x0–x30 在 0、q0–q31 在 256、nzcv 768、fpsr 776、fpcr 784、客体 sp 800、切栈槽 816
enum { GF_X = 0, GF_Q = 256, GF_NZCV = 768, GF_FPSR = 776, GF_FPCR = 784, GF_GSP = 800, GF_SWSLOT = 816 };
static void snap_thr_write(sfw *w, const snap_thr_rec *r) {
    sfw_rec(w);
#define WF(id, f) sfw_u64(w, id, (uint64_t)r->f);
    SNAP_THR_FIELDS(WF)
#undef WF
    const uint8_t *F = r->frame;
    sfw_blob(w, THR_F_XREGS, F + GF_X, 31 * 8); sfw_blob(w, THR_F_VREGS, F + GF_Q, 32 * 16);
    uint64_t v; memcpy(&v, F + GF_NZCV, 8); sfw_u64(w, THR_F_NZCV, v); memcpy(&v, F + GF_FPSR, 8); sfw_u64(w, THR_F_FPSR, v); memcpy(&v, F + GF_FPCR, 8); sfw_u64(w, THR_F_FPCR, v);
    memcpy(&v, F + GF_GSP, 8); sfw_u64(w, THR_F_GSP, v); memcpy(&v, F + GF_SWSLOT, 8); sfw_u64(w, THR_F_SWSLOT, v != 0);
    if (r->has_csmsg) sfw_blob(w, THR_F_CS_MSG, r->csmsg, sizeof r->csmsg);
    if (r->state == SNAP_T_RUN) { sfw_blob(w, THR_F_RUN_REGS, r->rregs, sizeof r->rregs); sfw_blob(w, THR_F_RUN_NEON, r->rneon, sizeof r->rneon); sfw_u64(w, THR_F_RUN_FPSR, r->rfpsr); sfw_u64(w, THR_F_RUN_FPCR, r->rfpcr); }
    if (r->state >= SNAP_T_ENTRY && r->state <= SNAP_T_SLEEP) {   // 门帧的返回点（x30）在补丁岛里：另记门身份，跨版本按身份换回新返回点
        uint64_t ret; memcpy(&ret, F + GF_X + 30 * 8, 8); uint64_t orig; int kind;
        if (!s2a_gate_identity(ret, &orig, &kind)) { sfw_u64(w, THR_F_GATE_ORIG, orig); sfw_u64(w, THR_F_GATE_KIND, (uint64_t)kind); } }
    sfw_end_rec(w);
}
static void snap_thr_read(const sfr *rd, snap_thr_rec *r) {
    memset(r, 0, sizeof *r);
#define RF(id, f) r->f = (typeof(r->f))sfr_u64(rd, id, 0);
    SNAP_THR_FIELDS(RF)
#undef RF
    uint8_t *F = r->frame;
    sfr_into(rd, THR_F_XREGS, F + GF_X, 31 * 8); sfr_into(rd, THR_F_VREGS, F + GF_Q, 32 * 16);
    uint64_t v = sfr_u64(rd, THR_F_NZCV, 0); memcpy(F + GF_NZCV, &v, 8); v = sfr_u64(rd, THR_F_FPSR, 0); memcpy(F + GF_FPSR, &v, 8); v = sfr_u64(rd, THR_F_FPCR, 0); memcpy(F + GF_FPCR, &v, 8);
    v = sfr_u64(rd, THR_F_GSP, 0); memcpy(F + GF_GSP, &v, 8); v = sfr_u64(rd, THR_F_SWSLOT, 0); memcpy(F + GF_SWSLOT, &v, 8);   // 非 0 = 有切栈槽，续跑时改成本进程的槽
    sfr_into(rd, THR_F_RUN_REGS, r->rregs, sizeof r->rregs); sfr_into(rd, THR_F_RUN_NEON, r->rneon, sizeof r->rneon);
    r->rfpsr = (uint32_t)sfr_u64(rd, THR_F_RUN_FPSR, 0); r->rfpcr = (uint32_t)sfr_u64(rd, THR_F_RUN_FPCR, 0);
    if (sfr_get(rd, THR_F_CS_MSG, NULL)) { sfr_into(rd, THR_F_CS_MSG, r->csmsg, sizeof r->csmsg); r->has_csmsg = 1; }
    if (sfr_get(rd, THR_F_GATE_ORIG, NULL)) {   // 门帧：按门身份在本版门表里取返回点（同一份镜像时与原值相同）
        uint64_t orig = sfr_u64(rd, THR_F_GATE_ORIG, 0); int kind = (int)sfr_u64(rd, THR_F_GATE_KIND, 0); uint64_t nr = s2a_gate_by_identity(orig, kind);
        if (!nr) { g_snap_gate_miss++; if (g_snap_gate_miss <= 4) LOG("【红】即时存档恢复：线程 id %llu 停在原地址 %#llx 的门（类型 %d），本版门表里没有这道门", (unsigned long long)r->id, (unsigned long long)orig, kind); }
        else memcpy(F + GF_X + 30 * 8, &nr, 8);
    }
}
int s2a_snap_kernel_save(FILE *f) {
    int e = 0; uint32_t mg = 0x324E524Bu;   // "KRN2"：段式带版本字段格式（snapfmt.h）
    glock();
    // 冻结判齐（s2a_snap_freeze）到这里取锁之间放过锁：宿主服务线程不冻，会给游戏事件发信号，把等待中的游戏线程唤醒——它此刻在走向出口、不在安全点
    //   （一款大游戏：等 svc 0x18 的线程在这段空窗被唤醒，记录写成停法 0、门帧全空，恢复时跳到地址 0 崩）。持锁重判，有没到的放锁等它停到出口再判；
    //   之后全程持锁写，唤醒（wake 持 G）进不来
    { uint64_t dl = now_ns() + 2000000000ull; int k = 0;
      for (;;) { int left = 0; for (thr_t *t = g_all; t; t = t->all) if (!snap_state(t)) left++;
        if (!left) break;
        if (now_ns() > dl) { pthread_mutex_unlock(&G); LOG("【红】即时存档：判齐后有 %d 条线程离开安全点、2 秒没回来，这次不存", left); return -1; }
        if (!k++) LOG("即时存档：判齐后有 %d 条线程被唤醒离开安全点（宿主服务发的信号），等它们停到出口", left);
        pthread_mutex_unlock(&G); usleep(100); glock(); } }
    e |= sw(f, &mg, 4);
    { sfw hw; sfw_sec(&hw, f, "kernel", 1); sfw_rec(&hw); sfw_u64(&hw, 1, g_gen); sfw_u64(&hw, 2, g_gen_async); sfw_u64(&hw, 3, g_next_id); sfw_u64(&hw, 4, g_wseq); sfw_end_rec(&hw); e |= sfw_end(&hw); }
    // 句柄表：非空格一条记录。obj（A 里的对象地址）只当身份用：同一事件被读写两端句柄共用时按它配对
    uint32_t nh = 0; for (int i = 1; i < MAX_H; i++) nh += g_h[i].kind != 0;
    { sfw hw; sfw_sec(&hw, f, "handles", 1);
      for (uint32_t i = 1; i < MAX_H; i++) if (g_h[i].kind) {
        sfw_rec(&hw); sfw_u64(&hw, 1, i); sfw_u64(&hw, 2, g_h[i].kind); sfw_u64(&hw, 3, g_h[i].gen); sfw_u64(&hw, 4, (uint64_t)(uintptr_t)g_h[i].obj); sfw_u64(&hw, 5, g_h[i].cookie);
        uint32_t k = g_h[i].kind & 15;
        if (k == S2A_OBJ_EVENT_R || k == S2A_OBJ_EVENT_W) { event_t *ev = g_h[i].obj; sfw_u64(&hw, 10, (uint64_t)ev->signaled); sfw_u64(&hw, 11, (uint64_t)ev->refs); sfw_u64(&hw, 12, ev->did); }
        else if (k == S2A_OBJ_TMEM) { tmem_t *tm = g_h[i].obj; sfw_u64(&hw, 20, tm->addr); sfw_u64(&hw, 21, tm->size); sfw_u64(&hw, 22, tm->perm); }
        else if (k == S2A_OBJ_SHMEM) { shmem_t *sm = g_h[i].obj; sfw_u64(&hw, 30, sm->size); sfw_u64(&hw, 31, (uint64_t)sm->refs); sfw_u64(&hw, 32, (uint64_t)sm->owned);
            if (sm->owned) sfw_blob(&hw, 33, sm->buf, (uint32_t)sm->size); }   // 游戏自建的共享内存：内容随快照；服务层建的（owned 0）内容由服务重生
        sfw_end_rec(&hw); }
      e |= sfw_end(&hw); }
    // 线程
    uint32_t nt = 0; for (thr_t *t = g_all; t; t = t->all) nt++;
    e |= sw(f, &nt, 4);
    sfw tw; sfw_sec(&tw, f, "threads", 1);
    int nslot0 = 0;
    uint64_t now = now_ns();
    int bad = 0;
    for (thr_t *t = g_all; t; t = t->all) {
        snap_thr_rec r; memset(&r, 0, sizeof r);
        r.handle = t->handle; r.state = (uint32_t)snap_state(t); r.last_svc = (uint32_t)t->last_svc; r.prio = (uint32_t)t->prio; r.base_prio = (uint32_t)t->base_prio;
        r.core = (uint32_t)t->core; r.started = (uint32_t)t->started; r.exited = (uint32_t)t->exited; r.closed = (uint32_t)t->closed; r.cancel = (uint32_t)t->cancel; r.is_main = (uint32_t)t->is_main;
        r.pinned = (uint32_t)t->pinned; r.pin_core = (uint32_t)t->pin_core; r.wkind = (uint32_t)t->wkind; r.wtype = (uint32_t)t->wtype; r.wtag = t->wtag; r.yielded = (uint32_t)t->yielded;
        r.id = t->id; r.entry = t->entry; r.arg = t->arg; r.stack_top = t->stack_top; r.mask = t->mask; r.pin_mask = t->pin_mask; r.waddr = t->waddr; r.wkey = t->wkey;
        r.wtmo_left = t->wtmo > 0 && t->wait_t0 ? (uint64_t)t->wtmo : (uint64_t)t->wtmo;   // 剩余超时：首版按原超时（恢复后多等一点不改语义）
        r.wseq = t->wseq; r.tlr = (uint64_t)(uintptr_t)t->tlr; r.wval = t->wval;
        if (t->tsd && !t->exited && r.state != SNAP_T_DEAD && r.state != SNAP_T_UNSTARTED) { /* 已退出的线程宿主栈已还，TSD 基址失效 */ r.k18 = tsd_slot(t, g_key18); r.ktls = tsd_slot(t, g_keytls); r.ktlr = tsd_slot(t, g_keytlr); uint64_t own = tsd_slot(t, g_own_key);
            if (own) { r.own_tls = ((uint64_t *)(uintptr_t)own)[0]; r.own_obj = ((uint64_t *)(uintptr_t)own)[1]; } }
        if (t->snap_cs && r.state == SNAP_T_ENTRY && atomic_load(&t->cs_ret) == t->snap_cs_ret) { memcpy(r.frame, t->snap_cs_frame, sizeof r.frame); memcpy(r.csmsg, t->snap_cs_msg, sizeof r.csmsg); r.has_csmsg = 1; }
        else if ((r.state >= SNAP_T_ENTRY && r.state <= SNAP_T_SLEEP) && t->svc_regs) memcpy(r.frame, t->svc_regs, sizeof r.frame);
        else if (r.state >= SNAP_T_ENTRY && r.state <= SNAP_T_SLEEP) bad++;
        if (!r.state) bad++;   // 不在安全点的线程绝不写成空现场（上面已判齐，这里只是兜住）
#if defined(__APPLE__)
        if (r.state == SNAP_T_RUN) { if (t->snap_mc) { memcpy(r.rregs, t->snap_mc->regs, sizeof r.rregs); memcpy(r.rneon, t->snap_mc->neon, sizeof r.rneon); r.rfpsr = t->snap_mc->fpsr; r.rfpcr = t->snap_mc->fpcr; } else bad++; }
#endif
        if (r.last_svc == 0x18 && r.state >= SNAP_T_ENTRY && r.state <= SNAP_T_SLEEP) { uint64_t *fr = (uint64_t *)r.frame;   // 诊断：0x18 的入参
            LOG("即时存档诊断：线程 id %llu 停法 %u svc 0x18 x0 %#llx x1 %#llx x2 %llu 首句柄 %#x", (unsigned long long)r.id, r.state, (unsigned long long)fr[0], (unsigned long long)fr[1], (unsigned long long)fr[2], fr[1] ? *(uint32_t *)(uintptr_t)fr[1] : 0); }
        if (r.state >= SNAP_T_ENTRY && r.state <= SNAP_T_SLEEP) { uint64_t sl; memcpy(&sl, r.frame + 816, 8); if (!sl) { nslot0++;
            if (nslot0 <= 6) LOG("即时存档诊断：线程 id %llu 停法 %u 门帧没有切栈槽（宿主栈上进门 / 本线程没槽），帧 %p、svc %#x", (unsigned long long)r.id, r.state, (void *)t->svc_regs, r.last_svc); } }
        snap_thr_write(&tw, &r);
    }
    if (nslot0) LOG("即时存档诊断：门里的线程有 %d 条门帧没有切栈槽", nslot0);
    e |= sfw_end(&tw);
    (void)now;
    pthread_mutex_unlock(&G);
    if (bad) LOG("【红】即时存档：%d 条线程在门里却没有门帧指针", bad);
    LOG("即时存档：内核账 句柄 %u 个、线程 %u 条、g_gen %u、g_next_id %llu", nh, nt, g_gen, (unsigned long long)g_next_id);
    return e || bad ? -1 : 0;
}
// ---------- 即时存档恢复（阶段 2）：句柄表 / 事件 / 传输内存 / 游戏自建共享内存 / 线程 ----------
static int sr(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n ? 0 : -1; }
static snap_hlog_t *g_rhlog; static int g_nrhlog;
static int snap_last_create_cs(uint32_t h);
static struct { uint32_t idx, kind; uint16_t gen; int signaled; uint64_t aobj; } *g_skip;
static struct { uint32_t idx, kind; uint16_t gen; void *obj; uint64_t cookie; int placed; } *g_defer; static int g_ndefer, g_capdefer;
static uint32_t g_temp[64]; static int g_ntemp;   // 重放临时造的游戏句柄（请求后摘掉）
static int defer_place(int j) {   // 持 G
    if (g_defer[j].placed) return 0;
    uint32_t idx = g_defer[j].idx;
    if (g_h[idx].kind) { LOG("【红】即时存档恢复：游戏句柄格 %u 要放时已被占（种类 %u 代 %u）", idx, g_h[idx].kind, g_h[idx].gen); return -1; }
    g_h[idx].kind = g_defer[j].kind; g_h[idx].gen = g_defer[j].gen; g_h[idx].obj = g_defer[j].obj; g_h[idx].cookie = g_defer[j].cookie; g_defer[j].placed = 1; return 0;
}
// 重放某条请求前：它要拷贝 / 移交的游戏句柄若还没放，先放
int s2a_snap_need_handle(uint32_t h) {
    glock(); int r = 1, i = h_idx(h);
    if (!i) { r = -1; for (int j = 0; j < g_ndefer; j++) if (((uint32_t)g_defer[j].gen << 15 | g_defer[j].idx) == h) { r = defer_place(j); break; } }
    pthread_mutex_unlock(&G);
    if (r < 0) {   // A 里后来关了：按日志里最后一次「建」临时造一个同号对象，这条请求之后摘掉
        snap_hlog_t *c = NULL; for (int i = g_nrhlog - 1; i >= 0 && !c; i--) if (!g_rhlog[i].op && g_rhlog[i].h == h) c = &g_rhlog[i];
        if (c && (c->kind & 15) == S2A_OBJ_TMEM) {
            tmem_t *tm = malloc(sizeof *tm); tm->addr = c->a; tm->size = c->b; tm->perm = c->c;
            glock(); uint32_t idx = h & 0x7fff; if (!g_h[idx].kind) { g_h[idx].kind = c->kind; g_h[idx].gen = (uint16_t)(h >> 15); g_h[idx].obj = tm; g_h[idx].cookie = 0; r = 0; if (g_ntemp < 64) g_temp[g_ntemp++] = h; } pthread_mutex_unlock(&G);
            LOG("%s即时存档恢复：重放要用的传输内存句柄 %#x（A 里后来关了）临时按日志建（%#llx +%#llx）", r ? "【红】" : "", h, (unsigned long long)tm->addr, (unsigned long long)tm->size);
        } else LOG("【红】即时存档恢复：重放要用的游戏句柄 %#x 不在快照最终表里、日志里也造不出（种类 %u）", h, c ? c->kind : 0);
    }
    return r;
}
// 重放结束后：放剩下的游戏句柄
int s2a_snap_defer_finish(void) { int bad = 0, n = 0; glock(); for (int j = 0; j < g_ndefer; j++) if (!g_defer[j].placed) { bad += defer_place(j) != 0; n++; } pthread_mutex_unlock(&G);
    LOG("%s即时存档恢复：游戏句柄放回 %d 个，失败 %d", bad ? "【红】" : "", n, bad); return bad ? -1 : 0; } static int g_nskip, g_capskip;   // 交给服务重放的句柄（重放后核对、补事件状态）
// 重放之后：服务层重建的句柄必须与 A 同格同代同种；事件按 A 的已触发位补上
int s2a_snap_kernel_post(void) {
    int bad = 0, ev = 0; glock();
    for (int i = 0; i < g_nskip; i++) {
        uint32_t idx = g_skip[i].idx;
        uint32_t k0 = g_skip[i].kind & 15;
        if (!g_h[idx].kind && (k0 == S2A_OBJ_EVENT_R || k0 == S2A_OBJ_EVENT_W)) {   // 服务层在请求外建的事件句柄（B 里没建）：找 A 里同一事件的别的句柄在 B 的对象，复制一个同号句柄
            event_t *ev = NULL;
            for (int j = 0; j < g_nskip && !ev; j++) if (j != i && g_skip[j].aobj == g_skip[i].aobj) { uint32_t jx = g_skip[j].idx; if (g_h[jx].kind && g_h[jx].gen == g_skip[j].gen) ev = g_h[jx].obj; }
            int fresh = !ev; if (fresh) { ev = calloc(1, sizeof *ev); ev->signaled = g_skip[i].signaled; }
            ev->refs++; g_h[idx].kind = g_skip[i].kind; g_h[idx].gen = g_skip[i].gen; g_h[idx].obj = ev; g_h[idx].cookie = 0;
            LOG("【黄】即时存档恢复：服务层请求外建的事件句柄格 %u %s", idx, fresh ? "在 B 里找不到同一事件，新建一个（服务不会触发它）" : "按同一事件的别的句柄补上");
        }
        if (g_h[idx].kind != g_skip[i].kind || g_h[idx].gen != g_skip[i].gen) {
            if (bad++ < 20) LOG("【红】即时存档恢复：重放后句柄格 %u 不对（应为 种类 %u 代 %u，实为 种类 %u 代 %u）", idx, g_skip[i].kind, g_skip[i].gen, g_h[idx].kind, g_h[idx].gen);
            continue;
        }
        uint32_t k = g_skip[i].kind & 15;
        if ((k == S2A_OBJ_EVENT_R || k == S2A_OBJ_EVENT_W) && g_h[idx].obj) { ((event_t *)g_h[idx].obj)->signaled = g_skip[i].signaled; ev++; }
    }
    // B 里多出来的句柄（A 最终表里没有）：A 的服务层在请求外建过、或关闭没对上
    int extra = 0;
    for (int i = 1; i < MAX_H; i++) if (g_h[i].kind) {
        int found = 0; for (int j = 0; j < g_nskip && !found; j++) found = g_skip[j].idx == (uint32_t)i;
        if (!found && snap_last_create_cs(((uint32_t)g_h[i].gen << 15) | (uint32_t)i) == 1) extra++;
    }
    g_h_used = 0; memset(g_kcnt, 0, sizeof g_kcnt); for (int i = 1; i < MAX_H; i++) if (g_h[i].kind) { g_h_used++; g_kcnt[g_h[i].kind & 15]++; }
    pthread_mutex_unlock(&G);
    LOG("%s即时存档恢复：服务层句柄核对 %d 个，不对 %d、事件状态补 %d、B 里多出 %d", bad ? "【红】" : "", g_nskip, bad, ev, extra);
    return bad ? -1 : 0;
}
static snap_thr_rec *g_snap_thr; static uint32_t g_snap_nthr;
static thr_t **g_snap_t;   // 与 g_snap_thr 一一对应的新对象
// skip(h, kind)：这个句柄由服务层在重放时按原值重建（返回 1 则跳过）；NULL = 全部按快照建
int s2a_snap_kernel_load(FILE *f, int (*skip)(uint32_t h, uint32_t kind)) {
    uint32_t mg = 0, nh = 0, nt = 0; uint16_t gen, gena; uint64_t nid, wseq;
    if (sr(f, &mg, 4) || mg != 0x324E524Bu) { LOG("【红】即时存档恢复：内核账文件头不对（%s）", mg == 0x314E524Bu ? "旧的整块格式，本版不读" : "不认识"); return -1; }
    { sfr hr; int rc = sfr_sec(&hr, f, "kernel", 1);
      if (rc || sfr_rec(&hr) != 1) { LOG("【红】即时存档恢复：内核账头段读不了%s", rc == -2 ? "（格式比本版新）" : ""); sfr_done(&hr); return -1; }
      gen = (uint16_t)sfr_u64(&hr, 1, 0); gena = (uint16_t)sfr_u64(&hr, 2, 0); nid = sfr_u64(&hr, 3, 0); wseq = sfr_u64(&hr, 4, 0);
      int t = sfr_rec(&hr); sfr_done(&hr); if (t != 0) { LOG("【红】即时存档恢复：内核账头段尾不对"); return -1; } }
    sfr hr; { int rc = sfr_sec(&hr, f, "handles", 1); if (rc) { LOG("【红】即时存档恢复：句柄表段读不了%s", rc == -2 ? "（格式比本版新）" : ""); return -1; } }
    nh = MAX_H;   // 逐条读到段尾为止（下面的循环在段尾退出）
    struct { uint64_t old; void *nw; } *evm = calloc(nh + 1, sizeof *evm); int nev = 0, e = 0, kept = 0, made = 0, skipped = 0, conflict = 0;
    struct { uint32_t idx; uint64_t old; } *thm = calloc(nh + 1, sizeof *thm); int nthm = 0;
    glock();
    uint32_t nread = 0;
    for (;;) {
        int rc = sfr_rec(&hr); if (rc == 0) break; if (rc < 0) { e = -1; break; }
        nread++;
        uint32_t idx = (uint32_t)sfr_u64(&hr, 1, 0), kind = (uint32_t)sfr_u64(&hr, 2, 0); uint16_t g = (uint16_t)sfr_u64(&hr, 3, 0); uint64_t obj = sfr_u64(&hr, 4, 0), cookie = sfr_u64(&hr, 5, 0);
        if (!idx || idx >= MAX_H) { e = -1; break; }
        uint32_t kk = kind & 15, h = ((uint32_t)g << 15) | idx;
        event_t evr = {0}; tmem_t tmr = {0}; uint64_t ssize = 0; int srefs = 0, sown = 0; uint8_t *scont = NULL;
        if (kk == S2A_OBJ_EVENT_R || kk == S2A_OBJ_EVENT_W) { evr.signaled = (int)sfr_u64(&hr, 10, 0); evr.refs = (int)sfr_u64(&hr, 11, 0); evr.did = (uint32_t)sfr_u64(&hr, 12, 0); }
        else if (kk == S2A_OBJ_TMEM) { tmr.addr = sfr_u64(&hr, 20, 0); tmr.size = sfr_u64(&hr, 21, 0); tmr.perm = (uint32_t)sfr_u64(&hr, 22, 0); }
        else if (kk == S2A_OBJ_SHMEM) { ssize = sfr_u64(&hr, 30, 0); srefs = (int)sfr_u64(&hr, 31, 0); sown = (int)sfr_u64(&hr, 32, 0);
            if (sown) { uint32_t cn = 0; const void *cp = sfr_get(&hr, 33, &cn); if (!cp || cn != ssize) { e = -1; break; }
                scont = mmap(NULL, (size_t)((ssize + 0x3fff) & ~0x3fffull), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); if (scont == MAP_FAILED) { e = -1; break; } memcpy(scont, cp, cn); } }
        if (skip && skip(h, kind)) { skipped++; if (scont) munmap(scont, (size_t)((ssize + 0x3fff) & ~0x3fffull));
            if (g_nskip == g_capskip) { g_capskip = g_capskip ? g_capskip * 2 : 256; g_skip = realloc(g_skip, sizeof *g_skip * (size_t)g_capskip); }
            g_skip[g_nskip++] = (typeof(*g_skip)){idx, kind, g, evr.signaled, obj}; continue; }
        if (g_h[idx].kind && !(snap_last_create_cs(h) == 0)) {   // B 起局时已占：同种同代 = 同一个（服务层初始化建的），留 B 的
            if (g_h[idx].kind == kind && g_h[idx].gen == g) { kept++; continue; }
            LOG("【红】即时存档恢复：句柄格 %u 冲突（快照 种类 %u 代 %u，当前 种类 %u 代 %u）", idx, kind, g, g_h[idx].kind, g_h[idx].gen); conflict++; continue;
        }
        void *nobj = NULL;
        if (kk == S2A_OBJ_EVENT_R || kk == S2A_OBJ_EVENT_W) {
            for (int i = 0; i < nev; i++) if (evm[i].old == obj) { nobj = evm[i].nw; break; }
            if (!nobj) { event_t *ev = calloc(1, sizeof *ev); *ev = evr; ev->trace_id = 0; nobj = ev; evm[nev].old = obj; evm[nev++].nw = ev; }
        } else if (kk == S2A_OBJ_TMEM) { tmem_t *tm = malloc(sizeof *tm); *tm = tmr; nobj = tm; }
        else if (kk == S2A_OBJ_SHMEM) {
            if (!sown) { LOG("【红】即时存档恢复：服务层的共享内存句柄 %#x 没被重放接管", h); conflict++; continue; }
            shmem_t *sm = calloc(1, sizeof *sm); sm->buf = scont; sm->size = ssize; sm->refs = srefs; sm->owned = 1; sm->next = g_shmem; g_shmem = sm; nobj = sm;
        } else if (kk == S2A_OBJ_THREAD) { thm[nthm].idx = idx; thm[nthm++].old = obj; nobj = NULL; }
        else { LOG("【红】即时存档恢复：句柄 %#x 种类 %u 没被重放接管、首版不会建", h, kind); conflict++; continue; }
        if (g_ndefer == g_capdefer) { g_capdefer = g_capdefer ? g_capdefer * 2 : 256; g_defer = realloc(g_defer, sizeof *g_defer * (size_t)g_capdefer); }
        g_defer[g_ndefer++] = (typeof(*g_defer)){idx, kind, g, nobj, cookie, 0}; made++;   // 先不放进表：服务重放期间 A 的同号格可能还被服务层的句柄占着
    }
    sfr_done(&hr); nh = nread;
    // 线程记录
    if (!e && sr(f, &nt, 4)) e = -1;
    if (!e) { g_snap_thr = calloc(nt ? nt : 1, sizeof *g_snap_thr); g_snap_t = calloc(nt ? nt : 1, sizeof *g_snap_t); g_snap_nthr = nt;
              sfr tr; int rc = sfr_sec(&tr, f, "threads", 1);
              if (rc == -2) { LOG("【红】即时存档恢复：线程记录格式版本 %u 比本版新（旧版不读新快照）", tr.ver); e = -1; }
              else if (rc) { LOG("【红】即时存档恢复：线程记录段读不了"); e = -1; }
              for (uint32_t i = 0; i < nt && !e; i++) { if (sfr_rec(&tr) != 1) { e = -1; break; } snap_thr_read(&tr, &g_snap_thr[i]); }
              if (!e && sfr_rec(&tr) != 0) e = -1;   // 段尾
              if (!e && g_snap_gate_miss) { LOG("【红】即时存档恢复：%d 条线程停的门在本版改写产物里找不到（改写器约定变了），这个快照是旧版存的，接不上，拒绝恢复；游戏自己的存档不受影响", g_snap_gate_miss); s2a_snap_mark_old(); e = -1; }
              sfr_done(&tr); }
    for (uint32_t i = 0; i < nt && !e; i++) {
        snap_thr_rec *r = &g_snap_thr[i]; thr_t *t = calloc(1, sizeof *t); pthread_cond_init(&t->cv, NULL);
        t->handle = r->handle; t->id = r->id; t->entry = r->entry; t->arg = r->arg; t->stack_top = r->stack_top; t->prio = (int)r->prio; t->base_prio = (int)r->base_prio;
        t->core = (int)r->core; t->mask = r->mask; t->started = (int)r->started; t->exited = (int)r->exited; t->closed = (int)r->closed; t->cancel = (int)r->cancel; t->is_main = (int)r->is_main;
        t->pinned = (int)r->pinned; t->pin_core = (int)r->pin_core; t->pin_mask = r->pin_mask; t->tlr = (void *)(uintptr_t)r->tlr;
        atomic_store(&t->in_svc, 1);
        g_snap_t[i] = t;
        // 句柄表里指向它的格（A 里的 thr_t 指针按句柄对上）
        uint32_t idx = r->handle & 0x7fff;
        for (int j = 0; j < g_ndefer; j++) if (g_defer[j].idx == idx && (g_defer[j].kind & 15) == S2A_OBJ_THREAD && !r->closed) g_defer[j].obj = t;
    }
    // g_all 按快照顺序（快照里是表头在前）
    for (int i = (int)nt - 1; i >= 0 && !e; i--) { g_snap_t[i]->all = g_all; g_all = g_snap_t[i]; }
    for (int j = 0; j < g_ndefer; j++) if ((g_defer[j].kind & 15) == S2A_OBJ_THREAD && !g_defer[j].obj) { LOG("【红】即时存档恢复：线程句柄格 %u 没对上线程记录", g_defer[j].idx); conflict++; }
    g_gen = gen; g_gen_async = gena; g_next_id = nid; g_wseq = wseq;
    g_h_used = 0; memset(g_kcnt, 0, sizeof g_kcnt); for (int i = 1; i < MAX_H; i++) if (g_h[i].kind) { g_h_used++; g_kcnt[g_h[i].kind & 15]++; }
    pthread_mutex_unlock(&G);
    free(evm); free(thm);
    LOG("%s即时存档恢复：内核账 句柄 %u 个（新建 %d、留 B 的 %d、交给服务重放 %d、冲突 %d）、事件对象 %d、线程 %u 条", e || conflict ? "【红】" : "", nh, made, kept, skipped, conflict, nev, nt);
    return e || conflict ? -1 : 0;
}
// ---------- 即时存档恢复（阶段 3 配合）：句柄日志驱动的重放 ----------
static uint32_t close_handle(uint32_t h);
static uint32_t *g_rforce; static int g_nrforce;
// 「最终仍活着的句柄」是不是服务层建的：按值找它最后一次「建」的那条日志
static int snap_last_create_cs(uint32_t h) { for (int i = g_nrhlog - 1; i >= 0; i--) if (g_rhlog[i].op == 0 && g_rhlog[i].h == h) return g_rhlog[i].cs; return -1; }
// 即时存档：进程内重建开新一局时（开着快照才调），句柄代数与句柄日志归全新进程的初值——新一局的日志从开局记起、重放出的句柄值与冷启动相同
//   （进程内「回到快照」：上一局用到第 300 代，重放建的句柄成了第 301 代起，与快照里的第 2 代起对不上 → 恢复失败）
void s2a_snap_kernel_fresh(void) { glock(); g_gen = 0; g_gen_async = 0; g_nhlog = 0; atomic_store(&g_snap_lastseq, -1); pthread_mutex_unlock(&G); }
// 恢复局接着记：快照里的句柄日志整份当开头（恢复完成、游戏线程放开之前调，见 snapshot.c snap_live_after_restore）
void s2a_snap_hlog_continue(void) {
    glock();
    if (g_nrhlog > g_caphlog) { g_caphlog = g_nrhlog + 4096; g_hlog = realloc(g_hlog, sizeof *g_hlog * (size_t)g_caphlog); }
    if (g_nrhlog) memcpy(g_hlog, g_rhlog, sizeof *g_hlog * (size_t)g_nrhlog);
    g_nhlog = g_nrhlog; int last = -1; for (int i = 0; i < g_nrhlog; i++) { if (g_rhlog[i].ctx > last) last = g_rhlog[i].ctx; if (g_rhlog[i].uid > g_huid) g_huid = g_rhlog[i].uid; }
    if (last >= 0) atomic_store(&g_snap_lastseq, last);
    pthread_mutex_unlock(&G);
}
int s2a_snap_hlog_load(FILE *f) {
    if (fread(&g_nrhlog, sizeof g_nrhlog, 1, f) != 1 || g_nrhlog < 0) return -1;
    g_rhlog = malloc(sizeof *g_rhlog * (size_t)(g_nrhlog ? g_nrhlog : 1));
    if (g_nrhlog && fread(g_rhlog, sizeof *g_rhlog, (size_t)g_nrhlog, f) != (size_t)g_nrhlog) return -1;
    g_rforce = malloc(sizeof *g_rforce * (size_t)(g_nrhlog ? g_nrhlog : 1));
    int ncs_req = 0, ncs_out = 0, nguest = 0, ncl = 0;
    for (int i = 0; i < g_nrhlog; i++) { snap_hlog_t *x = &g_rhlog[i]; if (x->op) ncl++; else if (!x->cs) nguest++; else if (x->ctx >= 0) ncs_req++; else ncs_out++; }
    LOG("即时存档恢复：句柄日志 %d 项（服务请求里建 %d、服务层请求外建 %d、游戏系统调用建 %d、关 %d）", g_nrhlog, ncs_req, ncs_out, nguest, ncl);
    return 0;
}
int s2a_snap_skip_cs(uint32_t h, uint32_t kind) { (void)kind; return snap_last_create_cs(h) == 1; }
// 重放第 seq 条之前：把 A 里这条请求建的句柄按顺序装成本线程的强制分配串
int s2a_snap_replay_begin(int seq) {
    g_nrforce = 0;
    for (int i = 0; i < g_nrhlog; i++) if (g_rhlog[i].op == 0 && g_rhlog[i].cs && g_rhlog[i].ctx == seq) g_rforce[g_nrforce++] = g_rhlog[i].h;
    s2a_snap_force(g_rforce, g_nrforce); return g_nrforce;
}
// 首条之前（seq = -1）与每条之后：按日志执行「此后、下一条之前」发生的关闭——只关服务层建的、在 B 里此刻还在的句柄
static int snap_apply_closes(int after) {
    int n = 0;
    for (int i = 0; i < g_nrhlog; i++) {
        snap_hlog_t *x = &g_rhlog[i]; if (!x->op || x->after != after) continue;
        // 服务自己关的（Dispose / 关会话时级联释放的对象）：重放引发它的那件事（请求、或游戏关会话）时服务会再关一次，这里替它关就成了关两次
        //   （一款大游戏：先按日志关掉 AudioDevice 的事件句柄，再关它的会话时 Dispose 又关一遍 → InvalidHandle，收局 Horizon 服务停止也报错）
        if (x->cs) continue;
        // 这条关闭之前、同值最后一次建是不是服务层建的
        int cs = -1; for (int j = i - 1; j >= 0; j--) if (g_rhlog[j].op == 0 && g_rhlog[j].h == x->h) { cs = g_rhlog[j].cs; break; }
        if (cs != 1) continue;
        glock(); int alive = h_idx(x->h) != 0; pthread_mutex_unlock(&G);
        if (alive) { close_handle(x->h); n++; }
    }
    return n;
}
int s2a_snap_replay_end(int seq) {
    if (g_ntemp) { glock(); for (int i = 0; i < g_ntemp; i++) { int ix = h_idx(g_temp[i]); if (ix) { free(g_h[ix].obj); memset(&g_h[ix], 0, sizeof g_h[ix]); } } g_ntemp = 0; pthread_mutex_unlock(&G); }
    int left = s2a_snap_force_left(); s2a_snap_force(NULL, 0);
    if (left) LOG("【红】即时存档恢复：重放第 %d 条后还有 %d 个句柄没按 A 的值建出来（这条请求在 B 里少建了句柄）", seq, left);
    snap_apply_closes(seq);
    return left ? -1 : 0;
}
int s2a_snap_replay_prelude(void) { return snap_apply_closes(-1); }
uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs);
void *sw_xstack_slot(void);
void sw_snap_resume(void *frame) __attribute__((noreturn));
// 线程单测不链 gate.c / entry.S：快照恢复线程在单测里永远不起（快照关），弱缺省只为链得上
__attribute__((weak)) uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs) { (void)ret_addr; (void)regs; return 0; }
__attribute__((weak)) void *sw_xstack_slot(void) { return 0; }
__attribute__((weak, noreturn)) void sw_snap_resume(void *frame) { (void)frame; abort(); }
static _Atomic int g_snap_settled;
// 恢复的游戏线程：绑宿主、按快照装 TSD / TLR，宿主栈上做一个门帧，按停法续跑。全程 g_frz 开着：续跑出来的线程都停回门里，等全体就位后统一放开
#if defined(__APPLE__)
// 弱缺省定义：真定义在 GL 出图层 graphics/nvn/gfx/gl/egl.m（线程本地的当前上下文按快照接回，同一个程序里强定义盖过它）；不链图形库的宿主构建与只链 thread.c 的单测用这份空实现
__attribute__((weak)) void gl_snap_thread_bind(uint64_t tid) { (void)tid; }
#endif
// 当前线程的客体线程号（不是游戏线程回 0）：GL 出图层按它记各线程的当前上下文（即时存档）
uint64_t s2a_snap_thread_self(void) { thr_t *t = thread_current_or_null(); return t ? t->id : 0; }
static void *snap_thread_main(void *arg) {
    snap_thr_rec *r = &g_snap_thr[(uintptr_t)arg]; thr_t *t = g_snap_t[(uintptr_t)arg];
#if defined(__APPLE__)
    set_qos(guest_qos(t->is_main, t->mask));
#endif
    if (pthread_setspecific(g_keythr, t)) die("恢复线程 TSD 失败");
    { char nm[24]; snprintf(nm, sizeof nm, "nx-%llu", (unsigned long long)t->id); pthread_setname_np(nm); }
    t->mport = pthread_mach_thread_np(pthread_self()); t->tsd = plat_tsd_base(); t->pt = pthread_self();
    s2a_crash_thread_init();
#if defined(__APPLE__)
    gl_snap_thread_bind(t->id);
#endif
    pthread_setspecific(g_keygate, (void *)sw_gate);
    pthread_setspecific(g_key18, (void *)(uintptr_t)r->k18);
    pthread_setspecific(g_keytls, (void *)(uintptr_t)r->ktls);
    pthread_setspecific(g_keytlr, (void *)(uintptr_t)r->ktlr);
    { void **blk = calloc(3, sizeof *blk); blk[0] = (void *)(uintptr_t)r->own_tls; blk[1] = (void *)(uintptr_t)r->own_obj; blk[2] = (void *)1; pthread_setspecific(g_own_key, blk); }
    uint64_t F[832 / 8] __attribute__((aligned(16)));
    uint64_t here = ((uint64_t)__builtin_frame_address(0) - 0x400) & ~0xfull;   // 门帧 F 在本帧里、比 here 高；切栈游标从 here 往下用
    sw_xstack_thread_init(here, t->stack_top);
    memcpy(F, r->frame, sizeof F);
    uint64_t *slot = sw_xstack_slot();
    if (F[816 / 8]) { F[808 / 8] = slot[0]; F[816 / 8] = (uint64_t)(uintptr_t)slot; slot[0] = (uint64_t)(uintptr_t)F; }
    t->svc_regs = F; t->last_svc = (int)r->last_svc;
    int reblock = r->state == SNAP_T_WAIT && (r->last_svc == 0x1C || (r->last_svc == 0x34 && r->wtype == 1));
    atomic_fetch_add(&g_snap_settled, 1);   // 已就位（下面的动作都会停回门里 / 挂进等待）
#if defined(__APPLE__)
    if (r->state == SNAP_T_RUN) {   // 停在游戏代码里的：TSD / 切栈槽已按本进程设好；在内核里等着，放开冻结时由 s2a_snap_unfreeze 写入快照现场、从原指令接着跑
        atomic_store(&t->in_svc, 0); t->snap_run = 1;
        for (;;) pause();
    }
#endif
    if (r->state == SNAP_T_EXIT) {
        svc_exit(t);                                  // 冻结中：先停在出口，放开后取核、清 in_svc
    } else if (reblock) {
        glock(); t->wkey = r->wkey; t->waddr = r->waddr; t->wtag = r->wtag; t->wtype = (int)r->wtype; t->wval = r->wval;
        int64_t res = block(t, (int)r->wkind, (int64_t)F[3]); pthread_mutex_unlock(&G);   // 0x1C / 0x34 的超时参数都在 x3（-1 = 无限等）；首版按原超时重新计时
        F[0] = (uint64_t)res; svc_exit(t);
    } else {
        // 入口停 / 睡眠 / 可重发的等待：门帧是入参，重新分派一次（冻结中会停在入口，放开后照常执行）
        if (r->last_svc == 0x18) LOG("即时存档恢复诊断：线程 id %llu 停法 %u 重发 svc 0x18 x0 %#llx x1 %#llx x2 %llu 首句柄 %#x", (unsigned long long)t->id, r->state, (unsigned long long)F[0], (unsigned long long)F[1], (unsigned long long)F[2], F[1] ? *(uint32_t *)(uintptr_t)F[1] : 0);
        if (r->has_csmsg && r->tlr) memcpy((void *)(uintptr_t)r->tlr, r->csmsg, sizeof r->csmsg);   // C# 里阻塞的 IPC：判齐那一刻的请求消息写回 TLS，再重发
        gate_dispatch(F[30], F);
    }
    sw_snap_resume(F);
}
// 起全部恢复的线程（冻结开着）；回 0 = 全部就位
int s2a_snap_threads_start(void) {
    glock(); atomic_store(&g_snap_frz, 1); atomic_store(&g_frz, 1); pthread_mutex_unlock(&G);
    // 等待中的按原等待先后起（同优先级先到先醒的次序靠它），其余在后
    uint32_t n = g_snap_nthr, *ord = calloc(n ? n : 1, sizeof *ord), k = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < n; i++) { int w = g_snap_thr[i].state == SNAP_T_WAIT; if ((pass == 0) == w) ord[k++] = i; }
        if (pass == 0) for (uint32_t a = 0; a + 1 < k; a++) for (uint32_t b = a + 1; b < k; b++) if (g_snap_thr[ord[b]].wseq < g_snap_thr[ord[a]].wseq) { uint32_t x = ord[a]; ord[a] = ord[b]; ord[b] = x; }
    }
    int started = 0;
    for (uint32_t j = 0; j < n; j++) {
        uint32_t i = ord[j]; snap_thr_rec *r = &g_snap_thr[i]; thr_t *t = g_snap_t[i];
        if (r->state == SNAP_T_DEAD) continue;
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8u << 20);
        atomic_fetch_add(&g_live_threads, 1);
        int rc = r->state == SNAP_T_UNSTARTED ? pthread_create(&t->pt, &a, thread_main, t) : pthread_create(&t->pt, &a, snap_thread_main, (void *)(uintptr_t)i);
        pthread_attr_destroy(&a);
        if (rc) { LOG("【红】即时存档恢复：起线程 id %llu 失败 rc=%d", (unsigned long long)t->id, rc); free(ord); return -1; }
        if (r->state != SNAP_T_UNSTARTED) started++;
        // 等它就位再起下一条（等待先后不乱）
        uint64_t dl = now_ns() + 2000000000ull;
        while (atomic_load(&g_snap_settled) < started && now_ns() < dl) usleep(50);
        if (r->state == SNAP_T_WAIT) { dl = now_ns() + 200000000ull; while (now_ns() < dl) { glock(); int in = t->wkind != 0 || atomic_load(&t->paused_ack); pthread_mutex_unlock(&G); if (in) break; usleep(50); } }
    }
    free(ord);
    LOG("即时存档恢复：起了 %d 条线程（另有已退出 / 未启动的按快照留着）", started);
    return 0;
}
void s2a_snap_unfreeze(void) {
#if defined(__APPLE__)
    // 停在游戏代码里的线程：挂住 → 中止它在内核里的等待 → 写入快照里的寄存器现场（PC 是游戏原代码地址，跨版本不变）→ 放行
    int nrun = 0, nbad = 0;
    for (uint32_t i = 0; i < g_snap_nthr; i++) { snap_thr_rec *r = &g_snap_thr[i]; thr_t *t = g_snap_t[i]; if (r->state != SNAP_T_RUN || !t || !t->mport) continue;
        if (!pc_in_guest_text(r->rregs[32])) { LOG("【红】即时存档恢复：线程 id %llu 的现场 PC %#llx 不在本局游戏代码里", (unsigned long long)t->id, (unsigned long long)r->rregs[32]); nbad++; continue; }
        if (thread_suspend(t->mport) != KERN_SUCCESS) { nbad++; continue; }
        thread_abort(t->mport);
        arm_thread_state64_t st; memset(&st, 0, sizeof st); for (int k = 0; k < 29; k++) st.__x[k] = r->rregs[k];
        __darwin_arm_thread_state64_set_fp(st, r->rregs[29]); __darwin_arm_thread_state64_set_lr_fptr(st, (void *)(uintptr_t)r->rregs[30]);
        __darwin_arm_thread_state64_set_sp(st, r->rregs[31]); __darwin_arm_thread_state64_set_pc_fptr(st, (void *)(uintptr_t)r->rregs[32]); st.__cpsr = (uint32_t)r->rregs[33];
        arm_neon_state64_t ns; memset(&ns, 0, sizeof ns); memcpy(&ns.__v, r->rneon, sizeof ns.__v); ns.__fpsr = r->rfpsr; ns.__fpcr = r->rfpcr;
        if (thread_set_state(t->mport, ARM_THREAD_STATE64, (thread_state_t)&st, ARM_THREAD_STATE64_COUNT) != KERN_SUCCESS
            || thread_set_state(t->mport, ARM_NEON_STATE64, (thread_state_t)&ns, ARM_NEON_STATE64_COUNT) != KERN_SUCCESS) { LOG("【红】即时存档恢复：线程 id %llu 写不进寄存器现场", (unsigned long long)t->id); nbad++; }
        if (core_part(t)) atomic_store(&t->off_guest, 1);   // 新进程里它还没持核：同解冻，先挂抢占信号再放开
        t->snap_run = 0; snap_run_resume(t); nrun++; }
    if (nrun || nbad) LOG("%s即时存档恢复：停在游戏代码里的线程 %d 条按快照现场接着跑（失败 %d）", nbad ? "【红】" : "", nrun, nbad);
#endif
    glock(); atomic_store(&g_frz, 0); atomic_store(&g_snap_frz, 0); pthread_cond_broadcast(&g_pause_cv); pthread_mutex_unlock(&G); LOG("即时存档恢复：放开冻结，游戏接着跑"); }
static uint32_t thr_unpause(thr_t *t) {
    glock();
    if (!atomic_load(&t->pause)) { pthread_mutex_unlock(&G); return R_BAD_STATE; }
    int mach = t->pmach; t->pmach = 0;
    if (t->pwait) { t->pwait = 0; atomic_store(&t->paused_ack, 0); }   // 代发的 ack 由恢复方收回（同安卓）；目标自己停进 pause_park 的由它出来时清
    // 在游戏代码里被暂停的、S2A_CORES 开：清暂停前先标「在宿主侧」（off_guest），恢复后进 tramp 取到核才清（preempt_tramp）。
    //   原先清暂停与挂信号 / 恢复线程之间（解锁后）它的标志看着像「在跑、不持核、不在等核」，调度计数记一次违规——实际它还挂着、没执行游戏指令
    //   （10-23 门禁负载下 t_thread ⑰ 偶发红：恢复方解锁后被调度走，窗口变宽）
    if (mach && cores_on()) atomic_store(&t->off_guest, 1);
    atomic_store(&t->pause, 0); pthread_cond_broadcast(&g_pause_cv); pthread_mutex_unlock(&G);
    // 在游戏代码里被暂停的（暂停方已代放核）：恢复前先给它挂一个抢占信号（S2A_CORES），恢复后执行任何游戏指令前先进 tramp 自己按优先级取核。
    // 不让恢复方代取核：恢复方在系统调用里可能持着目标要的核，代取会互等；挂信号后恢复方不阻塞。原先恢复后到下一次系统调用前不持核在跑（t_thread ⑰）
    if (mach) { if (core_part(t)) { atomic_store(&t->preempting, 1); pthread_kill(t->pt, S2A_SIG_PREEMPT); } thread_resume(t->mport); }
    return 0;
}
#else
static uint32_t thr_pause(thr_t *t) {
    glock(); int already = atomic_load(&t->pause);
    // An unpause may not yet have woken the target: its old ack and saved
    // context remain valid. Only the target changes the acknowledged kind.
    if (!already) atomic_store(&t->pause, 1);
    if (!already) { t->trace_pause=g_pd_current; t->trace_pause_conditions=0; }
    // 与 wait-safe 发布持同一把 G；唤醒并不替目标发布 ack。
    if (!already && android_wait_pause_on() && t->vm_wait_safe) pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&G);
    if (already) return R_BAD_STATE;
    uint64_t deadline = plat_now_ns() + 2000000000ull;
    if (wait_diag_on()) { glock(); pause_diag_locked(t, 10, 1, 0, deadline, 0, 0); pthread_mutex_unlock(&G); }
    unsigned sends = 0; int send_error = 0;
    for (;;) {
        if (android_wait_pause_on()) {
            // pins 只保护 thr；退出发布和最后一次发 pthread 信号必须同在 G 下。
            glock();
            uint32_t failure=t->exited || !atomic_load(&t->pause) ? R_BAD_STATE : 0;
            int ack=atomic_load_explicit(&t->paused_ack,memory_order_acquire);
            if (failure || ack) {
                if (wait_diag_on()) pause_diag_locked(t,12,1,failure,deadline,sends,send_error);
                pthread_mutex_unlock(&G);return failure;
            }
            int sent=pthread_kill(t->pt,SIGURG);++sends;if(sent)send_error=sent;
            // 目标可能已从原 CV 转入 VM futex，且屏蔽 SIGURG。
            // 只通知它重查 pause，不代 ack、不结束 epoch；重试覆盖 poll→wait 窗口。
            if (atomic_load(&t->vm_ack)) android_futex_wake(&android_vm_epoch);
            pthread_mutex_unlock(&G);
        } else {
        if (atomic_load_explicit(&t->paused_ack, memory_order_acquire)) {
            if (wait_diag_on()) { glock(); pause_diag_locked(t, 12, 1, 0, deadline, sends, send_error); pthread_mutex_unlock(&G); }
            return 0;
        }
        glock(); int exited = t->exited;
        // Blocked in an SVC wait (WaitSynchronization, condition / address waits): it sleeps in
        // android_condition_wait with G released and cannot leave the SVC without G, so it is
        // stopped at the gate frame now. Horizon pauses such a thread at once (Ryujinx
        // KThread.SetActivity); waiting for its own ack timed out after 2 s whenever nothing woke it
        // (Silksong's Unity GC suspending a blocked worker: 0xEA01, abort in nn::os::SuspendThread).
        // svc_exit parks it if it wakes while still paused.
        if (!exited && atomic_load(&t->in_svc) && t->vm_wait_safe && t->svc_regs) {
            t->pmach = 0; t->pwait = 1; atomic_store_explicit(&t->paused_ack, 1, memory_order_release);
            pthread_mutex_unlock(&G); return 0;
        }
        pthread_mutex_unlock(&G);
        if (exited) {
            if (wait_diag_on()) { glock(); pause_diag_locked(t, 12, 1, R_BAD_STATE, deadline, sends, send_error); pthread_mutex_unlock(&G); }
            return R_BAD_STATE;
        }
        int sent = pthread_kill(t->pt, SIGURG); ++sends; if (sent) send_error = sent;
        }
        if (plat_now_ns() >= deadline) {
            glock();
            if (wait_diag_on()) pause_diag_locked(t, 12, 1, R_TIMEOUT, deadline, sends, send_error);
            t->trace_pause=0; t->trace_pause_conditions=0;
            atomic_store(&t->pause, 0); pthread_cond_broadcast(&g_pause_cv); pthread_mutex_unlock(&G);
            return R_TIMEOUT;
        }
        usleep(100);
    }
}
static uint32_t thr_unpause(thr_t *t) {
    glock();
    if (!atomic_load(&t->pause)) { pthread_mutex_unlock(&G); return R_BAD_STATE; }
    // Acked on its behalf and still in its wait: the ack is ours to withdraw, or a later pause
    // would accept it after the thread has run on
    if (t->pwait) { t->pwait = 0; atomic_store(&t->paused_ack, 0); }
    t->trace_pause=0; t->trace_pause_conditions=0;
    atomic_store(&t->pause, 0); pthread_cond_broadcast(&g_pause_cv); pthread_mutex_unlock(&G);
    return 0;
}
int s2a_guest_pause_all(void) {
    // Optional diagnostic, using the same real pause protocol and lifetime pins.
    thr_t *list[ANDROID_VM_MAX_THREADS]; size_t count = 0; int paused = 0;
    glock();
    for (thr_t *t = g_all; t && count < ANDROID_VM_MAX_THREADS; t = t->all)
        if (t->started && !t->exited && t->pt != pthread_self()) { ++t->pins; list[count++] = t; }
    pthread_mutex_unlock(&G);
    for (size_t i = 0; i < count; ++i) paused += thr_pause(list[i]) == 0;
    glock();
    for (size_t i = 0; i < count; ++i) { thr_t *t = list[i]; --t->pins; if (!t->pins && t->exited && t->closed) thr_free(t); }
    pthread_mutex_unlock(&G); return paused;
}
#endif
static uint64_t tsd_slot(thr_t *t, pthread_key_t k) {
    uint64_t off;
    if (!t->tsd || plat_tsd_offset((uint32_t)k, &off)) return 0;
    return *(uint64_t *)(t->tsd + off);
}
// ThreadContext（0x320，Horizon 布局）：x0–x28、fp、lr、sp、pc、pstate、v0–v31、fpcr、fpsr、tpidr（= TLR）
static void thr_context3(thr_t *t, uint8_t *out) {
    uint64_t x[29], fp, lr, sp, pc; uint32_t psr, fpcr, fpsr; __uint128_t v[32];
    if (t->pmach) {
#if defined(__APPLE__)
        for (int i = 0; i < 29; i++) x[i] = t->pst.__x[i];
        fp = __darwin_arm_thread_state64_get_fp(t->pst); lr = __darwin_arm_thread_state64_get_lr(t->pst); sp = __darwin_arm_thread_state64_get_sp(t->pst); pc = __darwin_arm_thread_state64_get_pc(t->pst);
        psr = t->pst.__cpsr; memcpy(v, t->pns.__v, sizeof v); fpcr = t->pns.__fpcr; fpsr = t->pns.__fpsr;
#else
        const mcontext_t *mc = &t->paused_context.uc_mcontext;
        memcpy(x, mc->regs, sizeof x); fp = mc->regs[29]; lr = mc->regs[30]; sp = mc->sp; pc = mc->pc; psr = (uint32_t)mc->pstate;
        const struct fpsimd_context *fpstate = NULL;
        for (size_t offset = 0; offset + sizeof(struct _aarch64_ctx) <= sizeof mc->__reserved;) {
            const struct _aarch64_ctx *record = (const void *)(mc->__reserved + offset);
            if (!record->magic || record->size < sizeof *record || record->size > sizeof mc->__reserved - offset) break;
            if (record->magic == FPSIMD_MAGIC && record->size == sizeof(struct fpsimd_context)) { fpstate = (const void *)record; break; }
            offset += record->size;
        }
        if (!fpstate) die("Android paused context lacks FPSIMD");
        memcpy(v, fpstate->vregs, sizeof v); fpcr = fpstate->fpcr; fpsr = fpstate->fpsr;
#endif
    } else {   // 门帧（entry.S sw_gate）：x0..x29 @0、q0..q31 @256、nzcv @768、fpsr @776、fpcr @784、[800] = 游戏 sp G（含门借的 32 字节）；
               // x29 / x30 被门块借用（改写器在 bl sw_gate 前 stp x29, x30, [sp, #-16]!），原值在 G+32 / G+40；游戏原 sp = G+48
        const uint64_t *r = t->svc_regs; const uint8_t *rb = (const uint8_t *)r; const uint64_t *g = (const uint64_t *)(uintptr_t)r[100];
        for (int i = 0; i < 29; i++) x[i] = r[i];
        fp = g[4]; lr = g[5]; sp = (uint64_t)(uintptr_t)(g + 6); pc = t->svc_pc + 4;
        memcpy(v, rb + 256, sizeof v); psr = (uint32_t)r[96]; fpsr = (uint32_t)r[97]; fpcr = (uint32_t)r[98];
    }
    x[18] = tsd_slot(t, g_key18);
    memset(out, 0, 0x320);
    memcpy(out, x, sizeof x); memcpy(out + 0xE8, &fp, 8); memcpy(out + 0xF0, &lr, 8); memcpy(out + 0xF8, &sp, 8); memcpy(out + 0x100, &pc, 8);
    psr &= 0xFF0FFE20; memcpy(out + 0x108, &psr, 4);
    memcpy(out + 0x110, v, sizeof v); memcpy(out + 0x310, &fpcr, 4); memcpy(out + 0x314, &fpsr, 4);
    uint64_t tp = tsd_slot(t, g_keytlr); memcpy(out + 0x318, &tp, 8);
}

// ---------- CloseHandle ----------
void s2a_cs_handle_closed(uint32_t kind, uint64_t cookie);
static uint32_t close_handle(uint32_t h) {   // 不持 G 进来
    glock();
    int i = h_idx(h); uint32_t kind = i ? g_h[i].kind : 0; uint64_t cookie = i ? g_h[i].cookie : 0;
    if (!i) { pthread_mutex_unlock(&G); return R_BAD_HANDLE; }
    void *o = g_h[i].obj, *cm = NULL; hlog_add(1, h, kind, t_close_cs, NULL); h_free(i);
    switch (kind) {
    case S2A_OBJ_EVENT_R: case S2A_OBJ_EVENT_W: if (--((event_t *)o)->refs == 0) { if (((event_t *)o)->did) evmap_del(o); free(o); } break;
    case S2A_OBJ_TMEM: break;   // 放锁后还回源再释放：还回取 svc_mem 的锁，那边持锁时会来取 G（s2a_tmem_get）
    case S2A_OBJ_NATIVE_REF: if (--*(int *)o == 0) { if (cookie == 2) cm = o; else free(o); } break;   // 原生层内部对象（资源上限 / 代码内存，svc_mem.c）：首字段是引用计数；代码内存放锁后还回源再释放（同 TMEM）
    case S2A_OBJ_SHMEM: --((shmem_t *)o)->refs; break;   // 后备不释放：游戏可能还映射着，C# 还在写（量级 KB–MB，进程内常驻）
    case S2A_OBJ_THREAD: { thr_t *t = o; t->closed = 1; if (t->exited) thr_free(t); break; }
    default: break;
    }
    pthread_mutex_unlock(&G);
    if (kind == S2A_OBJ_TMEM) { tmem_t *tm = o; s2a_mem_tmem_close(tm->addr, tm->size); free(tm); }
    if (cm) s2a_mem_codemem_free(cm);
    if (kind == S2A_OBJ_SESSION || kind == S2A_OBJ_PORT || kind == S2A_OBJ_CS_OTHER) s2a_cs_handle_closed(kind, cookie);
    return 0;
}

// ---------- 系统调用分派 ----------
static int svc_thread_impl(thr_t *me, int num, uint64_t *regs);
// 诊断（S2A_DIAG_STAMP 设了才计、收局 / 崩溃时打；iOS 同样可用）：锁 / 条件变量的交接延迟——解锁 / 唤醒方 wake() 那一刻到等待方回到游戏代码。
//   含 pthread 条件变量唤醒、重新拿全局锁 G、（CORES 开时）取核。分 0x1A 锁、0x1C 条件变量两类，按量级计数并记最大
static _Atomic uint64_t g_lw_n[2], g_lw_b[2][7], g_lw_max[2];
static void lockwake_note(int k, uint64_t dt) {
    if (!lw_on()) return;
    uint64_t ns = thread_clock_ns(dt);
    int b = ns < 1000 ? 0 : ns < 10000 ? 1 : ns < 100000 ? 2 : ns < 1000000 ? 3 : ns < 10000000 ? 4 : ns < 100000000 ? 5 : 6;
    atomic_fetch_add(&g_lw_n[k], 1); atomic_fetch_add(&g_lw_b[k][b], 1);
    for (uint64_t o = atomic_load(&g_lw_max[k]); ns > o && !atomic_compare_exchange_weak(&g_lw_max[k], &o, ns);) {}
}
static void wt_report(void) {
    if (!wt_on()) return;
    static const char *bn[7] = {"<1µs", "<10µs", "<100µs", "<1ms", "<10ms", "<100ms", "≥100ms"};
    for (int k = 0; k < 2; k++) { char b[300]; size_t l = 0;
        for (int i = 0; i < 7; i++) l += (size_t)snprintf(b + l, sizeof b - l, "%s%s %llu", i ? "、" : "", bn[i], (unsigned long long)atomic_load(&g_wt_b[k][i]));
        LOG("唤醒追踪：%s %llu 次：%s", k ? "唤醒方「唤醒别人 → 自己回到游戏」" : "被唤醒方「唤醒 → 回到游戏」", (unsigned long long)atomic_load(&g_wt_n[k]), b); }
    LOG("唤醒追踪：超过 2 ms 的用户态段 被宿主停住 %llu 段共 %llu ms、在算 %llu 段共 %llu ms、宿主阻塞段为主 %llu 段共 %llu ms、等虚拟核为主 %llu 段共 %llu ms",
        (unsigned long long)atomic_load(&g_wt_st[0]), (unsigned long long)atomic_load(&g_wt_st_ms[0]), (unsigned long long)atomic_load(&g_wt_st[1]), (unsigned long long)atomic_load(&g_wt_st_ms[1]),
        (unsigned long long)atomic_load(&g_wt_st[2]), (unsigned long long)atomic_load(&g_wt_st_ms[2]), (unsigned long long)atomic_load(&g_wt_st[3]), (unsigned long long)atomic_load(&g_wt_st_ms[3]));
    for (int r = 0; r < 20; r++) {   // 宿主阻塞段（>0.1 ms）按调用点、线程取前 20：每轮挑剩下最大的一格
        int best = -1; uint64_t bn = 0;
        for (int i = 0; i < WT_BLK_N; i++) { uint64_t v = atomic_load(&g_wt_blk[i].ns); if (atomic_load(&g_wt_blk[i].key) && v > bn) { bn = v; best = i; } }
        if (best < 0) break;
        uint64_t k = atomic_load(&g_wt_blk[best].key), tid = k >> 48; char b[200]; hc_ret_str(k ^ (tid << 48), b, sizeof b);
        LOG("唤醒追踪：宿主阻塞段 #%d 线程 id %llu %s：%llu 段共 %.1f ms", r + 1, (unsigned long long)tid, b, (unsigned long long)atomic_load(&g_wt_blk[best].n), bn / 1e6);
        atomic_store(&g_wt_blk[best].ns, 0); }
}
void s2a_lockwake_report(void) {
    wt_report();
    if (!lw_on()) return;
    { static const char *bn[7] = {"<1µs", "<10µs", "<100µs", "<1ms", "<10ms", "<100ms", "≥100ms"}; char b[300]; size_t l = 0;
      for (int i = 0; i < 7; i++) l += (size_t)snprintf(b + l, sizeof b - l, "%s%s %llu", i ? "、" : "", bn[i], (unsigned long long)atomic_load(&g_gl_b[i]));
      LOG("诊断：全局锁 G 加锁 %llu 次，其中要等的 %llu 次：%s；最长等 %.1f µs", (unsigned long long)atomic_load(&g_gl_try), (unsigned long long)atomic_load(&g_gl_wait), b, atomic_load(&g_gl_max) / 1000.0); }
    static const char *bn[7] = {"<1µs", "<10µs", "<100µs", "<1ms", "<10ms", "<100ms", "≥100ms"};
    for (int k = 0; k < 2; k++) { char b[300]; size_t l = 0;
        for (int i = 0; i < 7; i++) l += (size_t)snprintf(b + l, sizeof b - l, "%s%s %llu", i ? "、" : "", bn[i], (unsigned long long)atomic_load(&g_lw_b[k][i]));
        LOG("诊断：%s交接延迟（唤醒 → 等待方回到游戏）%llu 次，%s；最大 %.1f µs", k ? "条件变量 0x1C " : "锁 0x1A ", (unsigned long long)atomic_load(&g_lw_n[k]), b, atomic_load(&g_lw_max[k]) / 1000.0); }
}
// 钉核 / 解钉（持 G；S2A_SCHED_HZ）：钉 = 理想核 / 掩码只留当前核、优先级抬升；解 = 还原钉前的理想核与掩码。每核同时只钉一条（KProcess.PinnedThreads）
static int g_pinned_core[4];
static void pin_set(thr_t *t, int on) {
    if (on) { int c = cores_on() && atomic_load(&t->held_core) > 0 ? atomic_load(&t->held_core) - 1 : (t->core >= 0 ? t->core : g_def_core);
        if (c < 0 || c > 3 || g_pinned_core[c]) return;
        t->pinned = 1; g_pinned_core[c] = 1; t->pin_core = t->core; t->pin_mask = t->mask; t->core = c; t->mask = 1ull << c;
        *(volatile uint16_t *)((uint8_t *)t->tlr + 0x102) = 1; }
    else { int c = t->core; if (c >= 0 && c < 4) g_pinned_core[c] = 0; t->pinned = 0; t->core = t->pin_core; t->mask = t->pin_mask; }
    prio_update(t);
}
// 同核高优先级唤醒让先：本次系统调用叫醒了掩码只含本核、优先级更高的线程 → 返回游戏前等它跑到下一次系统调用（或退出 / 重新等待），
//   上限 5 ms（同上游 dm_yield_to_core_peers，但只在真唤醒了这样的线程时等，不是每个系统调用后都等）。先放核：对方要的可能正是这个核
static void peer_yield(thr_t *me) {
    peer_wait_t peer = me->peer; me->peer.handle = 0;   // 仅消费值快照；每轮在 G 内重新验证句柄与线程身份
#ifndef S2A_UNIT_TEST
    if (s2a_det_mode()) return;   // 确定性档游戏线程串行：等不到对方，只白耗 5 ms
#endif
    atomic_fetch_add_explicit(&g_py_n, 1, memory_order_relaxed);
    core_release(me);
    uint64_t t0 = (uint64_t)now_mono_ns();
    for (;;) {
        glock(); thr_t *p = h_obj(peer.handle, S2A_OBJ_THREAD); int done = !p || p->id != peer.id || p->exited || p->wkind || atomic_load_explicit(&p->nsvc_all, memory_order_relaxed) != peer.nsvc; pthread_mutex_unlock(&G);
        if (done) return;
        if ((uint64_t)now_mono_ns() - t0 > 5000000) { atomic_fetch_add_explicit(&g_py_timeout, 1, memory_order_relaxed); return; }
        struct timespec ts = {0, 50000}; nanosleep(&ts, NULL);
    }
}
int svc_thread(int num, uint64_t *regs) {
#if defined(__APPLE__)
    if (__builtin_expect(g_wg_state > 0 && g_wg_mode == 3 && !t_wg_joined, 0)) { thr_t *w = thread_current_or_null(); if (w) { char nm[48]; snprintf(nm, sizeof nm, "游戏线程 %llu（优先级 %d）", (unsigned long long)w->id, w->prio); wg_join_self(nm); } }
#endif
    { thr_t *s = thread_current_or_null(); if (s) { atomic_fetch_add_explicit(&s->nsvc_all, 1, memory_order_relaxed);
        if (hz_pin() && !s->pinned && s->tlr && *(volatile uint16_t *)((uint8_t *)s->tlr + 0x100)) { glock(); pin_set(s, 1); pthread_mutex_unlock(&G); } } }
    switch (num) {
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x25:
    case 0x11: case 0x12: case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D:
    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x45: break;
    default: return 0;
    }
    thr_t *me = cur();
    // 先标「在系统调用里」再放核：反过来的话，放核到 svc_enter 之间这几条指令里线程不持核又不算在系统调用里，
    //   调度计数采成「不持核在跑」，别的线程恰好拿到这个核时同核计成 2（10-23 Mac 塞尔达 4 次 / 暗黑3 13 次，违规线程 last_svc 全是 0x0B / 0x1A / 0x1C 这类）
    svc_enter(me, regs, num);
    if (num == 0x0B || num == 0x18 || num == 0x1A || num == 0x1C || num == 0x34 || num == 0x0A) core_release(me);   // 会阻塞 / 让出 / 退出：放开核（svc_exit 返回游戏前重取）
    me->wake_t = 0;
    int diag = wait_diag_on(), source_svc = 0;
    int wait_trace = diag && (num == 0x18 || num == 0x1a || num == 0x1c || num == 0x34);
    if (wait_trace) {
        glock(); wait_diag_begin_locked(me, num, regs); pthread_mutex_unlock(&G);
        wait_diag_flush(1);
    }
    if (diag) { source_svc = g_wd_source_svc; g_wd_source_svc = num; }
    uint32_t *sv_addr = sync_svc_addr(num, regs), sv_k = me->sv_n % 8;
    if (sv_addr || num == 0x1D) {
        me->sv_ring[sv_k].num = (uint32_t)num; me->sv_ring[sv_k].lr = regs[30];
        for (int i = 0; i < 4; i++) me->sv_ring[sv_k].a[i] = regs[i];
        me->sv_ring[sv_k].pre = sv_addr ? __atomic_load_n(sv_addr, __ATOMIC_RELAXED) : 0;
    }
    int r = svc_thread_impl(me, num, regs);
    if (sv_addr || num == 0x1D) { me->sv_ring[sv_k].ret = (uint32_t)regs[0]; me->sv_ring[sv_k].post = sv_addr ? __atomic_load_n(sv_addr, __ATOMIC_RELAXED) : 0; me->sv_n++; }
    if (diag) g_wd_source_svc = source_svc;
    if (wait_trace) {
        glock(); wait_diag_end_locked(me, regs); pthread_mutex_unlock(&G);
        wait_diag_flush(1);
    }
    if (me->peer.handle) peer_yield(me);
    svc_exit(me);
    if (me->wake_t && (num == 0x1A || num == 0x1C)) lockwake_note(num == 0x1C, thread_clock() - me->wake_t);   // 诊断：唤醒 → 回到游戏的交接延迟
    return r;
}
void s2a_svc_frame(uint64_t pc) { thr_t *t = thread_current_or_null(); if (t) t->svc_pc = pc; }
static int svc_thread_impl(thr_t *me, int num, uint64_t *regs) {
    switch (num) {
    case 0x08: {  // CreateThread(→w1 句柄; x1 入口, x2 参数, x3 栈顶, w4 优先级, w5 核)
        // 校验与次序照 Ryujinx Syscall.CreateThread：核（-2 = 进程缺省核，必允许）→ 优先级 → 线程数上限
        { int core = (int)regs[5], prio = (int)regs[4];
          if (core != -2 && ((unsigned)core >= (unsigned)g_hw_cores || (g_cap_core && !(g_cap_core >> core & 1)))) { regs[0] = 0xE201; return 1; }   // InvalidCpuCore
          if ((unsigned)prio >= 64 || (g_cap_prio && !(g_cap_prio >> prio & 1))) { regs[0] = 0xE001; return 1; }                  // InvalidPriority
          if (lim_on()) { int n = 0; glock(); for (thr_t *o = g_all; o; o = o->all) n += !o->exited; pthread_mutex_unlock(&G);
              if (n >= LIM_THREADS) { static int said; if (!said++) LOG("线程数到 Switch 上限 %d，CreateThread 回 ResLimitExceeded（同 Switch；只打第一次）", LIM_THREADS); regs[0] = 0x10801; return 1; } } }
        if (hstk_reap(0)) { regs[0] = R_OUT_OF_RES; return 1; }
        thr_t *t = calloc(1, sizeof *t);
        pthread_cond_init(&t->cv, NULL);
        // 还没进游戏代码：算在系统调用里（调度计数不把「已启动、未到 thread_main 取核」的空窗记成不持核在跑），游戏入口线程进门 svc_exit 取核时清
        atomic_store(&t->in_svc, 1);
        t->entry = regs[1]; t->arg = regs[2]; t->stack_top = regs[3] & ~0xFull;
        t->prio = (int)regs[4]; t->core = (int)regs[5]; t->mask = t->core >= 0 ? 1ull << t->core : 7;
        if (hz_core() && t->core == -2) { t->core = g_def_core; t->mask = 1ull << g_def_core; }
        t->base_prio = t->prio;
        glock(); t->handle = h_new(S2A_OBJ_THREAD, t, 0); if (t->handle) { t->id = g_next_id++; t->all = g_all; g_all = t; } pthread_mutex_unlock(&G);
        if (!t->handle) { pthread_cond_destroy(&t->cv); free(t); regs[0] = R_OUT_OF_RES; return 1; }
        // 宿主栈 1 MB：门与 C# 跑在这上面（接口约定 ≥ 512 KB）。原 8 MB：Mac 暗黑3 100 秒 36 条游戏线程宿主栈单条最大只用到 48 KB，
        // 却合计占 288 MB 地址；iPhone 上窗口占满（VAMM）时窗口外只剩约 1.7 GB 空洞，C# 初始化后最大一块 380–473 MB，
        // 栈散落其间还切碎空洞（10-20 巫师3 测试设备2 365 MB 着色器离线库开不了）。主线程（host_main.c）仍 8 MB
        pthread_attr_t a; pthread_attr_init(&a);
        { uint8_t *st = s2a_tail_map(HSTK_SIZE + HSTK_GUARD, S2A_TAIL_STACK, PROT_READ | PROT_WRITE);   // 尾部放不下时退回低区匿名映射（同样自己还）
          if (st && !mprotect(st, HSTK_GUARD, PROT_NONE) && !pthread_attr_setstack(&a, st + HSTK_GUARD, HSTK_SIZE)) t->hstk = st;
          else { if (st) s2a_tail_unmap(st, HSTK_SIZE + HSTK_GUARD, S2A_TAIL_STACK); pthread_attr_setstacksize(&a, HSTK_SIZE); } }
        pthread_attr_setdetachstate(&a,
#if defined(__APPLE__)
            PTHREAD_CREATE_JOINABLE
#else
            t->hstk ? PTHREAD_CREATE_JOINABLE : PTHREAD_CREATE_DETACHED
#endif
        );
        {
#if defined(__APPLE__)
          atomic_store(&g_join_verified, 0);
          atomic_fetch_add(&g_live_threads, 1);
          int rc = pthread_create(&t->pt, &a, thread_main, t);
#else
          pthread_t created;
          int rc = pthread_create(&created, &a, thread_main, t);
          // child 的 thread_bind_host 和 VM census/watchdog 都持 G；pthread 不得在锁外写共享 pt。
          if (!rc) { glock(); t->pt = created; pthread_mutex_unlock(&G); }
#endif
          if (rc) { uint64_t ht, hl; s2a_low_holes(&ht, &hl);   // 宿主栈要窗口外的地址：iPhone 上窗口占满时只剩低区空洞（10-20 暗黑3 测试设备2 第 9 条线程即失败）
            die("pthread_create 失败 rc=%d（%s）：游戏线程 id %llu，低区空洞合计 %llu MB、最大一块 %llu MB", rc, strerror(rc), (unsigned long long)t->id, (unsigned long long)(ht >> 20), (unsigned long long)(hl >> 20)); } }
        pthread_attr_destroy(&a);
        regs[0] = 0; regs[1] = t->handle; return 1;
    }
    case 0x09: {  // StartThread(w0)：照 Ryujinx KThread.Start——已退出回 ThreadTerminating（0x7601），已启动（含当前线程伪句柄）回 InvalidState
        glock(); thr_t *t = h_obj((uint32_t)regs[0], S2A_OBJ_THREAD); uint64_t rc = t ? 0 : R_BAD_HANDLE;
        if (t && t->exited) rc = 0x7601;
        else if (t && t->started) rc = R_BAD_STATE;
        else if (t) { t->started = 1; pthread_cond_signal(&t->cv); }
        pthread_mutex_unlock(&G);
        regs[0] = rc; return 1;
    }
    case 0x0A: svc_exit(me); thread_exit(me); return 1;
    case 0x0B: {  // SleepThread(x0 ns)；0 / -1 / -2 = 各类让出
        int64_t ns = (int64_t)regs[0];
#ifndef S2A_UNIT_TEST
        if (s2a_det_mode()) { s2a_det_release(); if (ns > 0) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, NULL); } else sched_yield(); s2a_det_acquire(); return 1; }
#endif
        // 短于 20 µs 的睡按让出走（照 the reference port SleepThread，MIT）：游戏拿它轮询（银河 2 Hagi 用 SleepThread(100 ns)，main+0x517c），
        //   每次起一个精确定时器就是一次硬件唤醒。10-29 真机银河 2 整局 3100 万次（约 16 万次唤醒 / 秒）、丝之歌 540 万次（约 3.8 万次 / 秒），
        //   空转把整机拉到 4.7–5.1 W；当让出走则头 2 ms 照转、之后退避睡 10–250 µs。Switch 上 20 µs 内的睡本就和让出无别
        if (ns >= SLEEP_YIELD_NS) { sleep_hist(ns); int64_t sl = sleep_bo(me, regs, ns); if (sl > ns) sleep_thread_backoff(me, sl); else sleep_thread_ontime(me, sl); me->sl_ret = (uint64_t)now_mono_ns(); }
        else { if (ns > 0) atomic_fetch_add_explicit(&g_sl_short, 1, memory_order_relaxed); me->yielded = 1; yield_bo(me, regs); }
        return 1;
    }
    case 0x0C: { glock(); thr_t *t = h_obj((uint32_t)regs[1], S2A_OBJ_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)t->prio : 0; pthread_mutex_unlock(&G); return 1; }
    case 0x0D: {  // SetThreadPriority(w0 句柄, w1 优先级)：照 Horizon 先核优先级——0~63 且在 NPDM 允许的掩码里（同 0x08 的 g_cap_prio），否则 InvalidPriority，再找句柄
        { int prio = (int)regs[1]; if ((unsigned)prio >= 64 || (g_cap_prio && !(g_cap_prio >> prio & 1))) { regs[0] = 0xE001; return 1; } }
        glock(); thr_t *t = h_obj((uint32_t)regs[0], S2A_OBJ_THREAD);
        if (t) { t->base_prio = (int)regs[1]; if (hz_pi() || hz_pin()) prio_update(t); else t->prio = (int)regs[1]; }
        regs[0] = t ? 0 : R_BAD_HANDLE; pthread_mutex_unlock(&G); return 1; }
    case 0x0E: { glock(); thr_t *t = h_obj((uint32_t)regs[2], S2A_OBJ_THREAD);
                 regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)(uint32_t)t->core : 0; regs[2] = t ? t->mask : 0; pthread_mutex_unlock(&G); return 1; }
    case 0x0F: {  // SetThreadCoreMask(w0, w1 理想核, x2 掩码)：-1 = 用进程缺省、-2/-3 = 不改理想核；宿主只记账
        // 校验照 Ryujinx Syscall.SetThreadCoreMask（-2 的「改成进程缺省核 + 只许该核」语义属调度，留在批 D）：
        //   掩码超出 NPDM 允许核 → InvalidCpuCore；掩码 0 → InvalidCombination；理想核 ≥ 核数时只许 -1 / -3，否则 InvalidCpuCore；理想核不在掩码里 → InvalidCombination
        { int c = (int)regs[1]; uint64_t m = regs[2];
          if (c != -2) {
              if (g_cap_core && (g_cap_core | m) != g_cap_core) { regs[0] = 0xE201; return 1; }
              if (!m) { regs[0] = 0xE801; return 1; }
              if ((unsigned)c >= (unsigned)g_hw_cores) { if ((c | 2) != -1) { regs[0] = 0xE201; return 1; } }
              else if (!(m >> c & 1)) { regs[0] = 0xE801; return 1; }
          } }
        glock(); thr_t *t = h_obj((uint32_t)regs[0], S2A_OBJ_THREAD);
        if (t) { int c = (int)regs[1]; uint64_t m = regs[2];
            if (hz_core() && c == -2) { c = g_def_core; m = 1ull << g_def_core; }
            // 理想核照 Ryujinx KThread.SetCoreAndAffinityMask：-1（不指定）原样记、GetThreadCoreMask 读回 -1；-3 = 不改理想核，原理想核不在新掩码里 → InvalidCombination。
            //   原先 -1 记成 0：Blade II 先设（-1, 0x2），再把读回的（0, 0x2）原样设给新线程 → InvalidCombination、游戏起局即 Break
            int *pc = t->pinned ? &t->pin_core : &t->core;   // 钉核期间只改钉前存的那份，解钉时生效（Ryujinx 同）
            //   原理想核是 -1 时，Ryujinx 的 1UL << -1 与 Horizon（arm64 移位按 64 取模）都查第 63 位 → 实际总是 InvalidCombination，照抄
            if (c == -3 && !(m >> (*pc & 63) & 1)) { pthread_mutex_unlock(&G); regs[0] = 0xE801; return 1; }
            if (c >= 0 || c == -1) *pc = c;
            if (m) { if (t->pinned) t->pin_mask = m; else t->mask = m; } }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x10: {  // GetCurrentProcessorNumber：宿主不钉核，回理想核；理想核不在掩码里时回掩码最低核——
                  // 否则几条工作线程都自称 0 号核，游戏按核号分的无锁数据被同时改（旧库塞尔达实测链表成环）
        // 开虚拟核（S2A_CORES）时回此刻实际持有的核，同 Ryujinx KThread.CurrentCore：同一时刻两条线程不会报同一个核
        if (!atomic_exchange(&me->percore, 1) && cores_mode() == 1) core_acquire(me);   // 读过核号（各模式都记，同核诊断「丙」用）；按需模式第一次读起参与核互斥（先取核，回的就是持有的核）
        int c = me->core; if (me->mask && (c < 0 || !(me->mask >> c & 1))) c = __builtin_ctzll(me->mask);
        if (cores_on()) { int hc = atomic_load(&me->held_core); if (hc > 0) c = hc - 1; }
        regs[0] = (uint64_t)(uint32_t)c; return 1; }
    case 0x25: { glock(); thr_t *t = h_obj((uint32_t)regs[1], S2A_OBJ_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? t->id : 0; pthread_mutex_unlock(&G); return 1; }

    case 0x1A: {  // ArbitrateLock(w0 持有者句柄, x1 地址, w2 本线程标签)
        uint64_t addr = regs[1]; int64_t r = 0;
        if (addr & 3) { regs[0] = 0xCC01; return 1; }   // InvalidAddress：锁字要 4 字节对齐（Ryujinx IsAddressNotWordAligned）
        tag_check(me, (uint32_t)regs[2], "ArbitrateLock", addr);
        // 快路径（11-16 能耗线，借同赛道 MIT 移植的做法）：锁字已不是「持有者 | 等待位」就直接回 0、不拿全局锁 G——同 Horizon，游戏回用户态重抢。
        //   不拿锁读到旧值只等于一次本来就会发生的竞争（游戏照样重试）；宿主改锁字的 lock_handoff 都在 G 里，对得上时下面持 G 再核一遍。
        //   G 每秒上万次加锁、5.5% 要等（大金刚 Mac 局 S2A_DIAG_STAMP 读数）
        if (ld32(addr) != ((uint32_t)regs[0] | WAITERS_BIT)) { regs[0] = 0; return 1; }
        glock();
        if (ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) {
            if (!h_obj((uint32_t)regs[0], S2A_OBJ_THREAD)) r = R_BAD_HANDLE;   // 持有者句柄无效：照 Ryujinx ArbitrateLock 回 InvalidHandle（原先挂上去永远等不到交锁）
            else { me->waddr = addr; me->wtag = (uint32_t)regs[2]; if (hz_pi()) prio_raise(addr, me->prio); r = block(me, 1, -1); }
        }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1B: if (regs[0] & 3) { regs[0] = 0xCC01; return 1; } glock(); lock_handoff(regs[0]); pthread_mutex_unlock(&G); regs[0] = 0; return 1;   // ArbitrateUnlock：同上对齐
    case 0x1C: {  // WaitProcessWideKeyAtomic(x0 锁地址, x1 条件键, w2 标签, x3 超时)
        // 顺序要紧：先置条件键 = 1，再放互斥锁。SDK 的 Signal 在用户态先读条件键、为 0 就不发 0x1D；先放锁后置键，
        // 别的线程可在用户态抢锁、改完条件、读到键 0 就不唤醒，本线程随后挂上去永远等不到（旧库资源表读写锁卡死实例）
        uint64_t addr = regs[0], key = regs[1];
        if (addr & 3) { regs[0] = 0xCC01; return 1; }   // InvalidAddress：锁字要 4 字节对齐（Ryujinx）
        tag_check(me, (uint32_t)regs[2], "WaitProcessWideKeyAtomic", addr);
        glock();
        st32(key, 1);
        me->wkey = key; me->waddr = addr; me->wtag = (uint32_t)regs[2];
        lock_handoff(addr);
#ifdef S2A_UNIT_TEST
        { extern void (*s2a_test_cv_after_release)(void); if (s2a_test_cv_after_release) s2a_test_cv_after_release(); }   // t_thread：紧跟放锁之后、挂链之前
#endif
        int64_t r = block(me, 2, (int64_t)regs[3]);   // 超时：不重新拿锁（用户态自己拿）
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1D: {  // SignalProcessWideKey(x0 条件键, w1 个数，≤0 全部)
        uint64_t key = regs[0]; int cnt = (int)regs[1], n = 0;
        glock();
        wait_trace_bump_cv(key);   // 先记「这个键被发过信号」再挑人：没被挑中的等待者也能看出信号来过（S2A_WAIT_TRACE）
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) { wait_diag_cv_signal_locked(w, key); lock_acquire_for(w, w->waddr, w->wtag); }
        int left = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) left++;
        if (!left) st32(key, 0);
        cvl_note(key, me->id, n, left);
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }
    case 0x34: {  // WaitForAddress(x0 地址, w1 类型, x2 值, x3 超时)：0 小于则等、1 小于则减一再等、2 相等则等、3 64 位相等则等
        // 类型 3 WaitIfEqual64（HOS 19.0.0+ 内核，Mesosphère KAddressArbiter::WaitIfEqual64）：地址 8 字节对齐、x2 整个 64 位比较；
        //   Switch 2 SDK 的 nn::os::WaitForAddress(long const*, long)（libc++ std::atomic::wait 走它）只发这一种。Ryujinx / Eden-NX2 / Eden-NS2 都没有。
        //   11-05 时之笛开局 Break 2001-0120（InvalidEnumValue，栈 nn::os::WaitForAddress ← __libcpp_atomic_wait）
        uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2]; int64_t val64 = (int64_t)regs[2];
        if (addr & (type == 3 ? 7 : 3)) { regs[0] = 0xCC01; return 1; }   // InvalidAddress（Ryujinx Syscall.WaitForAddress；64 位按 8 字节）
        if ((unsigned)type > 3) { regs[0] = 0xF001; return 1; }        // InvalidEnumValue：原先未知类型按「小于则等」执行
        glock();
        int64_t r; uint32_t v = ld32(addr);
        int ok = type == 3 ? (int64_t)__atomic_load_n((uint64_t *)addr, __ATOMIC_SEQ_CST) == val64 : type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        me->wtype = type; me->wval = val; me->wtmo = (int64_t)regs[3];   // 现场导出：等的是哪种条件、比较值、超时
        if (!ok) r = R_BAD_STATE; else {
            // 换了地址才清「谁发过信号」：同地址超时重挂要保留——TimedWaitLightEvent 会周期性重挂，
            //   每次清零就会永远显示「从没来过」，正好把要找的那条抹掉
            if (me->waddr != addr) { me->sig_id = 0; me->sig_tick = 0; }
            me->waddr = addr; r = block(me, 3, (int64_t)regs[3]);
        }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x35: {  // SignalToAddress(x0 地址, w1 类型, w2 值, w3 个数)：0 唤醒、1 相等则加一并唤醒、2 相等则按等待数改值并唤醒
        uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2]; int cnt = (int)regs[3];
        if (addr & 3) { regs[0] = 0xCC01; return 1; }                  // InvalidAddress（Ryujinx Syscall.SignalToAddress）
        if ((unsigned)type > 2) { regs[0] = 0xF001; return 1; }        // InvalidEnumValue：原先未知类型按类型 1 执行并改内存
        glock();
        int nw = 0; uint64_t sig_now = thread_clock();
        for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) {
            nw++;
            // 缺省就记「谁发的、什么时候」：state 字里没有 owner，这是我们唯一能回答「该 signal 的那一路跑没跑」的地方。
            //   连没被挑中唤醒的等待者也记——它照样说明「信号来过、只是没轮到我」，与「从来没人来」处置完全不同
            w->sig_id = me->id; w->sig_tick = sig_now;
            if (wait_trace_on()) w->nsig++;
        }
        int64_t r = 0;
        if (type != 0) {
            int32_t nv = val + 1;
            // 类型 2（SignalAndModifyByWaitingCountIfEqual，7.0.0+ 语义，Atmosphère KAddressArbiter / Ryujinx 同）：没人等 +1；
            // 全唤醒（个数 ≤ 0）且有人等 −2；只唤醒部分时等待数 ≤ 个数 −1，否则不变。10-20 前「全唤醒」误写成 −1（SDK 轻事件按 −2 编码状态）
            if (type == 2) nv = nw == 0 ? val + 1 : cnt <= 0 ? val - 2 : nw <= cnt ? val - 1 : val;
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r) { int n = 0; for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
#if !defined(__APPLE__)
    case 0x32: {
        glock(); thr_t *t = h_obj((uint32_t)regs[0], S2A_OBJ_THREAD);
        uint32_t result = !t ? R_BAD_HANDLE : t == me ? R_BAD_THREAD : t->exited || !t->started ? R_BAD_STATE : 0;
        uint64_t request = 0;
        if (wait_diag_on()) {
            request = ++g_pd_request;
            pause_diag_svc_locked(me, t, request, 9, (uint32_t)regs[1], result, (uint32_t)regs[0]);
            if (result) pause_diag_svc_locked(me, t, request, 13, (uint32_t)regs[1], result, (uint32_t)regs[0]);
        }
        if (!result) ++t->pins;
        pthread_mutex_unlock(&G);
        if (!result) {
            g_pd_current=request;
            result = (uint32_t)regs[1] ? thr_pause(t) : thr_unpause(t);
            g_pd_current=0;
            glock();
            if (wait_diag_on()) pause_diag_svc_locked(me, t, request, 13, (uint32_t)regs[1], result, (uint32_t)regs[0]);
            --t->pins; if (!t->pins && t->exited && t->closed) thr_free(t); pthread_mutex_unlock(&G);
        }
        if (wait_diag_on()) wait_diag_flush(1);
        regs[0] = result; return 1;
    }
#else
    case 0x32: {  // SetThreadActivity(w0 线程, w1 0 运行 / 1 暂停)：语义按 Ryujinx KThread.SetActivity
        // 查到就钉住（同非苹果）：放开 G 之后暂停最长约 2 秒，期间目标退出、句柄被关也不能释放对象
        glock(); thr_t *t = h_obj((uint32_t)regs[0], S2A_OBJ_THREAD);
        uint32_t result = !t ? R_BAD_HANDLE : t == me ? R_BAD_THREAD : t->exited || !t->started ? R_BAD_STATE : 0;
        if (!result) ++t->pins;
        pthread_mutex_unlock(&G);
        if (result) { regs[0] = result; return 1; }
#ifdef S2A_UNIT_TEST
        if (s2a_test_after_lookup) s2a_test_after_lookup(t);
#endif
        result = (uint32_t)regs[1] ? thr_pause(t) : thr_unpause(t);
        thr_unpin(t); regs[0] = result; return 1;
    }
#endif
    case 0x33: {  // GetThreadContext3(x0 出参, w1 线程)：目标须已停稳（两平台同一条件：暂停中且 paused_ack，审计 P1-49；原先苹果只看暂停标志，暂停还在进行就读到过期门帧）
        // 持 G 把快照拷进栈缓冲：thr_unpause 持 G 清 pmach 再恢复线程，放锁后再读 pst / 门帧会和并发恢复拼成两截（审计 P2-15）
        uint8_t context[0x320];
        glock(); thr_t *t = h_obj((uint32_t)regs[1], S2A_OBJ_THREAD);
        uint32_t result = !t ? R_BAD_HANDLE : t == me ? R_BAD_THREAD
            : !atomic_load(&t->pause) || !atomic_load_explicit(&t->paused_ack, memory_order_acquire)
                || (!t->pmach && !t->svc_regs) ? R_BAD_STATE : 0;
        if (!result) thr_context3(t, context);
        pthread_mutex_unlock(&G);
        if (!result) {
            // Snapshot while pause ownership is stable, then release G before
            // readback can enter managed code. No borrowed thr_t survives it.
            void s2a_host_write_hook(uint64_t, uint64_t);
            s2a_host_write_hook(regs[0], sizeof context);
            memcpy((void *)(uintptr_t)regs[0], context, sizeof context);
        }
        regs[0] = result; return 1;
    }
    case 0x36: {  // SynchronizePreemptionState：S2A_SCHED_HZ 关时宿主从不钉核 ⇒ 清标志（TLR+0x102）即同语义；开时被钉的解钉（KThread.SynchronizePreemptionState）
        if (hz_pin()) { glock(); if (me->pinned) pin_set(me, 0); pthread_mutex_unlock(&G); }
        *(volatile uint16_t *)((uint8_t *)me->tlr + 0x102) = 0; regs[0] = 0; return 1; }
    case 0x45: {  // CreateEvent(→w1 写端, w2 读端)
        event_t *e = calloc(1, sizeof *e); e->refs = 2;
        glock(); uint32_t hw = h_new(S2A_OBJ_EVENT_W, e, 0), hr = hw ? h_new(S2A_OBJ_EVENT_R, e, 0) : 0;
        if (hw && !hr) h_free((int)(hw & 0x7fff));
        pthread_mutex_unlock(&G);
        if (!hr) { free(e); regs[0] = R_OUT_OF_RES; return 1; }
        regs[0] = 0; regs[1] = hw; regs[2] = hr; return 1;
    }
    case 0x11: case 0x12: case 0x17: {  // SignalEvent（写端）/ ClearEvent（任一端）/ ResetSignal（读端，未触发报错）
        uint32_t h = (uint32_t)regs[0]; int64_t r = 0;
        glock();
        event_t *e = num == 0x11 ? h_obj(h, S2A_OBJ_EVENT_W) : num == 0x17 ? h_obj(h, S2A_OBJ_EVENT_R)
                   : (h_obj(h, S2A_OBJ_EVENT_W) ? h_obj(h, S2A_OBJ_EVENT_W) : h_obj(h, S2A_OBJ_EVENT_R));
#ifndef S2A_UNIT_TEST
        if (e && !e->did && s2a_det_in_turn()) ev_expose(e);
#endif
        if (!e) r = (num == 0x17 && h_obj(h, S2A_OBJ_THREAD)) ? R_BAD_STATE : R_BAD_HANDLE;
        else if (num == 0x11) { e->signaled = 1; wait_diag_event_locked(e, 1); wake_sync_waiters(); }
        else if (num == 0x17 && !e->signaled) r = R_BAD_STATE;
        else { e->signaled = 0; wait_diag_event_locked(e, 0); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x18: {  // WaitSynchronization(→w1 下标; x1 句柄数组, w2 个数, x3 超时)
        const uint32_t *hs = (const uint32_t *)regs[1]; int n = (int)regs[2]; int64_t ns = (int64_t)regs[3];
        int64_t deadline = ns > 0 ? now_mono_ns() + ns : 0, r; int idx = -1;
        if (n < 0 || n > 64) { regs[0] = 0xEE01; return 1; }   // OutOfRange（Horizon 上限 64）
        glock(); me->wh = hs; me->wn = n;
        // 句柄只在进入时校验并换成对象（同 Horizon / Ryujinx：等的是对象，不是句柄）：事件多持一份引用，等待中途游戏关掉句柄（#1520 D-BJZ6：视频解码线程收尾时
        //   关了被等的事件句柄），对象照样活着、别处触发照样唤醒；原先每次醒来按句柄重查，查不到就回句柄无效（2001-0114），游戏随即中止。线程照旧按句柄查，
        //   中途关掉的当作不会再触发
        event_t *pin[64] = {0}; int bad = 0;
        for (int i = 0; i < n && !bad; i++) { if (hs[i] == S2A_H_CUR_THREAD) continue; int ix = h_idx(hs[i]); uint32_t k = ix ? g_h[ix].kind : 0;
            if (k == S2A_OBJ_EVENT_R) { pin[i] = g_h[ix].obj; pin[i]->refs++; } else if (k != S2A_OBJ_THREAD) bad = 1; }
        if (bad) { r = R_BAD_HANDLE;
            if (s2a_snap_restoring()) for (int i = 0; i < n; i++) { int b2 = 0; sig_obj(hs[i], &b2);   // 即时存档恢复局：说清是哪个句柄
                if (b2) LOG("【红】即时存档恢复局：WaitSynchronization 第 %d 个句柄 %#x 无效（格 %u：种类 %u 代 %u）", i, hs[i], hs[i] & 0x7fff, g_h[hs[i] & 0x7fff].kind, g_h[hs[i] & 0x7fff].gen); } }
        for (; !bad;) {
            if (me->cancel) { me->cancel = 0; r = R_CANCELLED; break; }
            for (int i = 0; i < n && idx < 0; i++) {
                if (pin[i]) {
#ifndef S2A_UNIT_TEST
                    if (!pin[i]->did && s2a_det_in_turn()) ev_expose(pin[i]);   // 确定性档：同 sig_obj
#endif
                    if (pin[i]->signaled) idx = i; }
                else { int gone = 0; if (sig_obj(hs[i], &gone)) idx = i; } }   // 线程（或当前线程伪句柄）；中途关掉的 gone，不当无效
            if (idx >= 0) { r = 0; break; }
            int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - now_mono_ns();
            if (ns > 0 && left <= 0) { r = R_TIMEOUT; break; }
            r = block(me, 4, left);
            // A normal event wake (-1) only asks us to check the objects
            // again. Session cancellation must reach svc_exit, outside G;
            // re-enqueueing it would keep this thread alive indefinitely.
            if (r == R_TIMEOUT || r == R_CANCELLED) break;
        }
        for (int i = 0; i < n; i++) if (pin[i] && --pin[i]->refs == 0) { if (pin[i]->did) evmap_del(pin[i]); free(pin[i]); }   // 还回进入时多持的那份（同 close_handle）
        me->wn = 0; pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; regs[1] = idx >= 0 ? (uint64_t)idx : 0; return 1;
    }
    case 0x19: {  // CancelSynchronization(w0 线程)
        glock(); thr_t *t = h_obj((uint32_t)regs[0], S2A_OBJ_THREAD);
        if (t) { t->cancel = 1; if (t->wkind == 4) wake(t, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x15: {  // CreateTransferMemory(→w1 句柄; x1 地址, x2 大小, w3 权限)：游戏把自己的一段内存交给服务（音频渲染器工作区等）
        // 校验与上限照 Ryujinx Syscall.CreateTransferMemory：地址 / 大小 4 KB 对齐、非零、不回绕，权限只许 无 / 读 / 读写；同时存在 ≤ 128 个
        { uint64_t a = regs[1], n = regs[2]; uint32_t pm = (uint32_t)regs[3];
          if (a & 0xfff) { regs[0] = 0xCC01; return 1; }                  // InvalidAddress
          if ((n & 0xfff) || !n) { regs[0] = 0xCA01; return 1; }          // InvalidSize
          if (a + n <= a) { regs[0] = 0xD401; return 1; }                 // InvalidMemState
          if (pm > 3 || pm == 2) { regs[0] = 0xD801; return 1; }          // InvalidPermission
          if (lim_on()) { int c = 0; glock(); for (int i = 1; i < MAX_H; i++) c += g_h[i].kind == S2A_OBJ_TMEM; pthread_mutex_unlock(&G);
              if (c >= LIM_TMEM) { regs[0] = 0x10801; return 1; } } }     // ResLimitExceeded
        // 分配失败回 OutOfResource（11-01 审计 P2-11）；源借出（权限账记成建立时的权限）照 Ryujinx KTransferMemory.Initialize 在发句柄前，发不出句柄即还回
        tmem_t *tm = malloc(sizeof *tm); if (!tm) { regs[0] = 0xCE01; return 1; }
        tm->addr = regs[1]; tm->size = regs[2]; tm->perm = (uint32_t)regs[3];
        uint32_t be = s2a_mem_tmem_borrow(tm->addr, tm->size, tm->perm); if (be) { free(tm); regs[0] = be; return 1; }
        glock(); uint32_t h = h_new(S2A_OBJ_TMEM, tm, 0); pthread_mutex_unlock(&G);
        if (!h) { s2a_mem_tmem_unborrow(tm->addr, tm->size); free(tm); regs[0] = R_OUT_OF_RES; return 1; }
        regs[0] = 0; regs[1] = h; return 1;
    }
    case 0x16: {   // CloseHandle；诊断 S2A_SVC_TRACE=1 时记游戏关了哪个句柄、什么种类（查服务与游戏对句柄归属的理解不一致）
        uint32_t h = (uint32_t)regs[0]; static int tr = -1; if (tr < 0) { const char *e = getenv("S2A_SVC_TRACE"); tr = e && *e == '1'; }
        uint32_t k = 0; if (tr) { glock(); int i = h_idx(h); k = i ? g_h[i].kind : 0; pthread_mutex_unlock(&G); }
        regs[0] = close_handle(h);
        if (tr) LOG("[关句柄] 游戏线程 %#x 关 %#x 种类 %u 结果 %#llx", me->handle, h, k, (unsigned long long)regs[0]);
        return 1; }
    }
    return 0;
}

// ---------- 给 C# 的入口参数（gate.c 调）----------
void s2a_svc_ctx_fill(s2a_svc_ctx *c, int num, uint64_t *regs) {
    thr_t *t = thread_current_or_null();
    memset(c, 0, sizeof *c); c->num = (uint32_t)num; c->regs = regs; c->thread = t ? t->handle : 0; c->tlr = t ? t->tlr : NULL;
    if (num == 0x21 || num == 0x22 || num == 0x23) {   // 会话句柄：0x21 在 w0，0x22 / 0x23 在 w2
        uint32_t h = (uint32_t)(num == 0x21 ? regs[0] : regs[2]);
        glock(); int i = h_idx(h); if (i) { c->obj_kind = g_h[i].kind; c->obj = g_h[i].cookie; } pthread_mutex_unlock(&G);
    }
    if (t) { t->cs_svc = num; t->cs_h = (uint32_t)(num == 0x21 ? regs[0] : regs[2]); t->cs_t0 = now_ns(); t->cs_seq = -1; }   // 序号由 C# 请求开始时设（s2a_snap_hctx）
    if (t) { t->svc_regs = regs; atomic_store(&t->in_svc, 1); }   // C# 里可能阻塞：算在系统调用里（暂停取门帧）
}
// C# 处理完一次系统调用、回到原生层的第一时间（gate.c，取核之前）：回复已写进游戏的消息区 / 寄存器帧。即时存档据此判断「判齐后这次请求有没有完成」
void s2a_svc_cs_returned(void) { thr_t *t = thread_current_or_null(); if (t) { t->cs_svc = 0; atomic_fetch_add(&t->cs_ret, 1); } }
void s2a_svc_ctx_done(void) { thr_t *t = thread_current_or_null(); if (t) {
#if !defined(__APPLE__)
    svc_exit(t);
#else
    s2a_thread_stop_point();
    atomic_store(&t->in_svc, 0);
    svc_leave_check(t);   // C# 系统调用出口也认暂停（审计 P1-49：原先这里不查，在 C# 里被暂停的线程回成功后照跑游戏代码）
#endif
} }

// ---------- §4 C# 用的 C 接口 ----------
uint64_t s2a_process_id(void) { return 0x51; }   // 应用进程号（Ryujinx 首个应用进程同取 0x51 起；GetProcessId 与 C# 两边同值）
// 原生层内部对象的句柄（svc_mem.c 的资源上限 / 代码内存）：种类号不进 s2a_native.h，C# 不发不认。obj 首字段是 int 引用计数，发句柄加一、关句柄减一，到 0 释放
uint32_t s2a_native_handle_new(uint32_t sub, void *obj) { glock(); ++*(int *)obj; uint32_t h = h_new(S2A_OBJ_NATIVE_REF, obj, sub); if (!h) --*(int *)obj; pthread_mutex_unlock(&G); return h; }
void *s2a_native_handle_obj(uint32_t h, uint32_t sub) { glock(); int i = h_idx(h); void *o = i && g_h[i].kind == S2A_OBJ_NATIVE_REF && g_h[i].cookie == sub ? g_h[i].obj : NULL; pthread_mutex_unlock(&G); return o; }
void s2a_handle_kind_count(uint32_t kind, int *cur, int *peak) { glock(); *cur = g_kcnt[kind & 15]; *peak = g_kpeak[kind & 15]; pthread_mutex_unlock(&G); }
uint32_t s2a_handle_new(uint32_t kind, uint64_t cookie) {
    if (kind != S2A_OBJ_SESSION && kind != S2A_OBJ_PORT && kind != S2A_OBJ_CS_OTHER) { LOG("【红】s2a_handle_new：种类 %u 不归 C# 发", kind); return 0; }
    glock(); uint32_t h = h_new_cs(kind, NULL, cookie); pthread_mutex_unlock(&G); return h;
}
int s2a_handle_get(uint32_t h, uint32_t *kind, uint64_t *cookie) {
    if (h == S2A_H_CUR_PROCESS) { *kind = S2A_OBJ_PROCESS; *cookie = s2a_process_id(); return 1; }   // 进程伪句柄：cookie = 进程号
    glock();
    if (h == S2A_H_CUR_THREAD) { thr_t *t = thread_current_or_null(); *kind = t ? S2A_OBJ_THREAD : 0; *cookie = t ? t->handle : 0; pthread_mutex_unlock(&G); return t != NULL; }
    // 原生种类的 cookie = 可直接用的原生对象号：事件 → 事件号（可传 s2a_event_signal / clear；要跨请求持有先 s2a_event_retain）、
    // 共享内存 → 宿主指针、传输内存 → 0（用 s2a_tmem_get）、线程 → 线程 id；C# 种类 → 发句柄时给的 cookie
    int i = h_idx(h);
    if (i) { *kind = g_h[i].kind;
        switch (g_h[i].kind) {
        case S2A_OBJ_THREAD: *cookie = ((thr_t *)g_h[i].obj)->id; break;
        case S2A_OBJ_EVENT_R: case S2A_OBJ_EVENT_W: *cookie = (uint64_t)(uintptr_t)g_h[i].obj; break;
        case S2A_OBJ_SHMEM: *cookie = (uint64_t)(uintptr_t)((shmem_t *)g_h[i].obj)->buf; break;
        case S2A_OBJ_TMEM: *cookie = 0; break;
        default: *cookie = g_h[i].cookie; } }
    pthread_mutex_unlock(&G); return i != 0;
}
// 1 = 关掉了，0 = 无此句柄（与 s2a_handle_get 同口径）。C# 关不存在的句柄多半是两边对句柄归属的理解不一致（已转给游戏 / 已被游戏关掉 / 从没建过），打一行红区分
int s2a_handle_close(uint32_t h) {
    if (trace_on()) LOG("[句柄] C# 关 %#x", h);
    int was = t_close_cs; t_close_cs = 1; uint32_t r = close_handle(h); t_close_cs = was;
    if (r == 0) return 1;
    glock(); unsigned i = h & 0x7fff, gen = h >> 15; int used = i && i < MAX_H, live = used && g_h[i].kind; unsigned cur = used ? g_h[i].gen : 0; pthread_mutex_unlock(&G);
    LOG("【红】s2a_handle_close：C# 关不存在的句柄 %#x（%s）", h, !h ? "句柄 0：从没建过" : !used ? "格子越界：不是原生句柄" : live ? "格子已被别的对象复用（这个句柄早已被关）" : cur == gen ? "已被关掉（游戏或服务先关了）" : "已被关掉、格子空着");
    return 0;
}
uint64_t s2a_event_create(uint32_t *h_read, uint32_t *h_write) {
    event_t *e = calloc(1, sizeof *e); e->refs = 1;   // C# 持有一份
    glock();
#ifndef S2A_UNIT_TEST
    if (s2a_det_in_turn()) ev_expose(e);   // 确定性档：游戏线程持锁时建的当场编号；异步建的等第一次对游戏可见再编
#endif
    uint32_t hr = h_new_cs(S2A_OBJ_EVENT_R, e, 0); if (hr) e->refs++;
    uint32_t hw = h_write && hr ? h_new_cs(S2A_OBJ_EVENT_W, e, 0) : 0; if (hw) e->refs++;
    pthread_mutex_unlock(&G);
    if (!hr || (h_write && !hw)) { LOG("【红】s2a_event_create：句柄表满"); if (hr) close_handle(hr); if (hw) close_handle(hw); free(e); return 0; }
    *h_read = hr; if (h_write) *h_write = hw;
    if (trace_on()) LOG("[句柄] C# 建事件 读 %#x 写 %#x 事件 %p", hr, hw, (void *)e);
    return (uint64_t)(uintptr_t)e;
}
#ifndef S2A_UNIT_TEST
int s2a_det_host_event(uint32_t id, int op);   // det.c：确定性档下宿主线程改事件由它接管（记录 / 重放）
#define DET_HOST(e, op) if (s2a_det_host_event((e)->did, op)) return
#else
#define DET_HOST(e, op) (void)0
#endif
void s2a_det_apply_host(uint32_t did, int op) {
    glock(); event_t *e = evmap_get(did);
    if (e) { e->signaled = op; wait_diag_event_locked(e, op); if (op) wake_sync_waiters(); }
    pthread_mutex_unlock(&G);
    if (!e) { static _Atomic int miss; if (atomic_fetch_add(&miss, 1) < 8) LOG("【红】确定性：宿主改事件 对象号 %#x 查不到（重放时异步建的事件编号与记录不同？）", did); }
}
void s2a_event_signal(uint64_t ev) { if (!ev) return; event_t *e = (event_t *)(uintptr_t)ev; DET_HOST(e, 1); glock(); e->signaled = 1; wait_diag_event_locked(e, 1); wake_sync_waiters(); pthread_mutex_unlock(&G); }
void s2a_event_clear(uint64_t ev) { if (!ev) return; event_t *e = (event_t *)(uintptr_t)ev; DET_HOST(e, 0); glock(); e->signaled = 0; wait_diag_event_locked(e, 0); pthread_mutex_unlock(&G); }
void s2a_event_retain(uint64_t ev) { if (!ev) return; glock(); ((event_t *)(uintptr_t)ev)->refs++; pthread_mutex_unlock(&G); }
void s2a_event_release(uint64_t ev) { if (!ev) return; glock(); event_t *e = (event_t *)(uintptr_t)ev; if (--e->refs == 0) {
#ifndef S2A_UNIT_TEST
    evmap_del(e);
#endif
    free(e); } pthread_mutex_unlock(&G); }
// 给读端再发一个句柄（复制句柄语义：服务每次应答都应给新句柄，游戏关掉它不影响服务手里的事件）
uint32_t s2a_event_dup_read(uint64_t ev) {
    if (!ev) return 0;
    glock(); event_t *e = (event_t *)(uintptr_t)ev; uint32_t h = h_new_cs(S2A_OBJ_EVENT_R, e, 0); if (h) e->refs++;
#ifndef S2A_UNIT_TEST
    if (h && !e->did && s2a_det_in_turn()) ev_expose(e);
#endif
    pthread_mutex_unlock(&G);
    if (trace_on()) LOG("[句柄] C# 复制读端 %#x 事件 %p", h, (void *)e);
    return h;
}
#include <sys/mman.h>
uint32_t s2a_shmem_create(uint64_t size, void **host) {
    if (!size) return 0;
    shmem_t *sm = calloc(1, sizeof *sm); sm->size = size; sm->refs = 1; sm->owned = 1;
    sm->buf = mmap(NULL, (size + 0x3FFF) & ~0x3FFFull, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, S2A_VMTAG_FD(S2A_VMTAG_SHMEM), 0);   // MAP_SHARED：vm_remap 别名到游戏地址后同一批物理页
    if (sm->buf == MAP_FAILED) die("共享内存后备 mmap %#llx", (unsigned long long)size);
    glock(); uint32_t h = h_new_cs(S2A_OBJ_SHMEM, sm, 0);
    if (h) { sm->next = g_shmem; g_shmem = sm; } pthread_mutex_unlock(&G);
    if (!h) { munmap(sm->buf, (size + 0x3FFF) & ~0x3FFFull); free(sm); return 0; }
    *host = sm->buf; return h;
}
// 后备由 C# 持有（如 Ryujinx KSharedMemory 的页）：原生层只发句柄，映射时别名过去；源地址按平台实际宿主页对齐。
uint32_t s2a_shmem_wrap(void *host, uint64_t size) {
#if defined(__ANDROID__) || defined(__OHOS__)
    // 与 plat_vm_alias 共用页大小；4 KB 设备上的合法源不必额外满足 Apple 的 16 KB 对齐。
    size_t page = plat_page_size();
    if (!page || !host || !size || (uintptr_t)host % page) { LOG("【红】s2a_shmem_wrap：后备 %p +%#llx 无效或未按宿主页 %#zx 对齐", host, (unsigned long long)size, page); return 0; }
    if (size % page) LOG("s2a_shmem_wrap：大小 %#llx 不是宿主页 %#zx 的整数倍，映射给游戏时末页多出的 %#llx 字节也会别名过去", (unsigned long long)size, page, (unsigned long long)(page - size % page));
#else
    if (!host || !size || ((uintptr_t)host & 0x3FFF)) { LOG("【红】s2a_shmem_wrap：后备 %p +%#llx 不是 16 KB 对齐（宿主页 16 KB，别名映射要求页内偏移相同）", host, (unsigned long long)size); return 0; }
    if (size & 0x3FFF) LOG("s2a_shmem_wrap：大小 %#llx 不是 16 KB 整数倍，映射给游戏时末页多出的 %#llx 字节也会别名过去", (unsigned long long)size, (unsigned long long)(0x4000 - (size & 0x3FFF)));
#endif
    shmem_t *sm = calloc(1, sizeof *sm); sm->size = size; sm->refs = 1; sm->buf = host;
    glock(); uint32_t h = h_new_cs(S2A_OBJ_SHMEM, sm, 0);
    if (h) { sm->next = g_shmem; g_shmem = sm; } pthread_mutex_unlock(&G);
    if (!h) free(sm);
    return h;
}
void *s2a_shmem_lookup(uint32_t h, uint64_t *size) {   // svc_mem.c：Map / UnmapSharedMemory 用
    glock(); shmem_t *sm = h_obj(h, S2A_OBJ_SHMEM); pthread_mutex_unlock(&G);
    if (!sm) return NULL; *size = sm->size; return sm->buf;
}
int s2a_tmem_get(uint32_t h, uint64_t *addr, uint64_t *size, uint32_t *perm) {
    glock(); tmem_t *tm = h_obj(h, S2A_OBJ_TMEM); if (tm) { *addr = tm->addr; *size = tm->size; *perm = tm->perm; } pthread_mutex_unlock(&G);
    return tm != NULL;
}
uint32_t s2a_thread_current(void) { thr_t *t = thread_current_or_null(); return t ? t->handle : 0; }

#if defined(__APPLE__)
int s2a_thread_restart_supported(void) {
    // Capability is queried before the first guest registers its thread keys.
    // Initialize the actual idempotent stop boundary before inspecting it.
    if (thread_keys_init()) return 0;
    if (!g_stop_sp || !g_stop_boundary) return 0;
    int supported = 1;
    glock();
    for (thr_t *t = g_all; t; t = t->all) if (!t->exited && sw_is_host_code(t->entry)) { supported = 0; break; }
    pthread_mutex_unlock(&G);
    pthread_mutex_lock(&g_reap_mu); supported = supported && !g_reap_failed; pthread_mutex_unlock(&g_reap_mu);
    return supported;
}
int s2a_thread_session_joined(void) {
    if (!atomic_load(&g_join_verified) || atomic_load(&g_live_threads)) return 0;
    pthread_mutex_lock(&g_reap_mu);
    int joined = !g_nreap && !g_reap_failed;
    pthread_mutex_unlock(&g_reap_mu);
    return joined;
}
static _Atomic int g_join_done, g_join_result;
static void *session_join(void *unused) {
    (void)unused; pthread_setname_np("s2a-线程回收");
    atomic_store(&g_join_result, hstk_reap(1));
    atomic_store(&g_join_done, 1);
    return NULL;
}
// Close SVC admission and drain managed calls first. Never kill a thread that
// may own host locks; timeout keeps every session resource owned and intact.
int s2a_thread_stop_all(int wait_ms) {
    // A service can fail before the first guest registers and initializes TLS.
    // Prepare the real keys/boundaries before checking or joining its roster.
    if (thread_keys_init()) return -1;
    // Recheck the roster after closing admission; the earlier request preflight
    // cannot rule out a host entry created while that request was being accepted.
    if (!s2a_thread_restart_supported()) return -1;
    atomic_store(&g_join_verified, 0);
    atomic_store(&g_session_stop, 1);
    uint64_t deadline = now_ns() + (uint64_t)wait_ms * 1000000;
    while (atomic_load(&g_live_threads)) {
        glock();
        for (thr_t *t = g_all; t; t = t->all) {
            if (t->exited) continue;
            if (sw_is_host_code(t->entry)) { pthread_mutex_unlock(&G); return -1; }
            if (t->wkind) wake(t, R_CANCELLED);
            pthread_cond_broadcast(&t->cv);
            atomic_store(&t->pause, 0);
            if (t->pmach) { t->pmach = 0; thread_resume(t->mport); }
            if (t->mport) pthread_kill(t->pt, S2A_SIG_PREEMPT);
        }
        pthread_cond_broadcast(&g_pause_cv);
        pthread_mutex_unlock(&G);
        pthread_mutex_lock(&g_cmu); pthread_cond_broadcast(&g_ccv); pthread_mutex_unlock(&g_cmu);
        if (now_ns() >= deadline) return -1;
        usleep(1000);
    }
    // Guest TLS destructors (including NativeAOT runtime detach) are part of
    // thread retirement. If join blocks, retain the session and report failure.
    atomic_store(&g_join_done, 0); atomic_store(&g_join_result, 0);
    pthread_t joiner;
    if (pthread_create(&joiner, NULL, session_join, NULL)) return -1;
    pthread_detach(joiner);
    while (!atomic_load(&g_join_done)) {
        if (now_ns() >= deadline) return -1;
        usleep(1000);
    }
    int result = atomic_load(&g_join_result);
    if (!result) atomic_store(&g_join_verified, 1);
    return result;
}

// Services and all guest threads have retired. Preserve handle generations
// so that an integer from the old game cannot immediately identify a new one.
int s2a_thread_session_reset(void) {
    if (!s2a_thread_session_joined() || hstk_reap(1)) return -1;
    for (int i = 1; i < MAX_H; i++) {
        glock(); uint32_t h = g_h[i].kind ? ((uint32_t)g_h[i].gen << 15) | (uint32_t)i : 0;
        pthread_mutex_unlock(&G);
        if (h) close_handle(h);
    }
    glock();
    // A host pause/context reader may still own a pin after guest join.
    // thr_free retains pinned objects; looping under G would block their unpin.
    for (thr_t *t = g_all; t; t = t->all) if (t->pins) {
        pthread_mutex_unlock(&G);
        LOG("【红】会话线程表仍有暂停 / 上下文读者，保留对象，禁止重建");
        return -1;
    }
    while (g_all) thr_free(g_all);
    while (g_shmem) {
        shmem_t *sm = g_shmem; g_shmem = sm->next;
        if (sm->owned) munmap(sm->buf, (sm->size + 0x3fff) & ~0x3fffull);
        free(sm);
    }
    g_wait = NULL; g_next_id = 0x51; g_wseq = 0;
    g_h_peak = 0; memset(g_kpeak, 0, sizeof g_kpeak);
    memset(g_evmap, 0, sizeof g_evmap); memset(g_cvl, 0, sizeof g_cvl);
    memset(g_pinned_core, 0, sizeof g_pinned_core);
    g_cap_core = g_cap_prio = 0; g_def_core = 0; g_main_prio = 44;
    pthread_mutex_unlock(&G);
    pthread_mutex_lock(&g_cmu);
    memset(g_cown, 0, sizeof g_cown); memset(g_cown_t, 0, sizeof g_cown_t);
    memset(g_cw, 0, sizeof g_cw); g_ncw = 0;
    pthread_mutex_unlock(&g_cmu);
    atomic_store(&g_session_stop, 0);
    return 0;
}
#endif
#if !defined(__APPLE__)
// Android does not maintain the live/reap proof needed for in-process rebuild.
// Callers first check s2a_session_rebuild_supported; never certify a join here.
int s2a_thread_session_joined(void) { return 0; }
// No stop/parking boundary off Apple: session.c calls this unconditionally from s2a_host_restart(),
// so game mode needs the symbol to link. 0 = the in-process restart request fails closed (PR #15).
int s2a_thread_restart_supported(void) { return 0; }
#endif
uint64_t s2a_thread_id(uint32_t h) { glock(); thr_t *t = h_obj(h, S2A_OBJ_THREAD); uint64_t id = t ? t->id : 0; pthread_mutex_unlock(&G); return id; }

// ---------- 给 svc_mem.c 的 ----------
int s2a_thread_alive_count(void) { int n = 0; glock(); for (thr_t *t = g_all; t; t = t->all) n += !t->exited; pthread_mutex_unlock(&G); return n; }
int s2a_thread_cur_core(void) { thr_t *t = thread_current_or_null(); if (!t) return 0; int c = t->core; if (t->mask && (c < 0 || !(t->mask >> c & 1))) c = __builtin_ctzll(t->mask); return c < 0 || c >= g_hw_cores ? 0 : c; }
uint64_t s2a_thread_cpu_ns(uint32_t h, int *ok) {
#if defined(__APPLE__)

    glock(); thr_t *t = h_obj(h, S2A_OBJ_THREAD); mach_port_t mp = t ? t->mport : 0; pthread_mutex_unlock(&G);
    *ok = t != NULL; if (!mp) return 0;
    thread_basic_info_data_t bi; mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(mp, THREAD_BASIC_INFO, (thread_info_t)&bi, &n) != KERN_SUCCESS) return 0;
    return ((uint64_t)bi.user_time.seconds + bi.system_time.seconds) * 1000000000ull + ((uint64_t)bi.user_time.microseconds + bi.system_time.microseconds) * 1000ull;
#else
    glock(); thr_t *t = h_obj(h, S2A_OBJ_THREAD); clockid_t clock; struct timespec value;
    int error = !t || t->exited ? ESRCH : pthread_getcpuclockid(t->pt, &clock);
    if (!error && clock_gettime(clock, &value)) error = errno;
    pthread_mutex_unlock(&G);
    *ok = !error;
    return error ? 0 : (uint64_t)value.tv_sec * 1000000000ull + value.tv_nsec;
#endif
}

void *s2a_thread_tlr(void) { thr_t *t = thread_current_or_null(); return t ? t->tlr : NULL; }

#ifdef S2A_UNIT_TEST
// t_thread ⑨：线程此刻持有的虚拟核（-1 = 未持有；句柄无效 -2）
int s2a_test_blocked(uint32_t h) { glock(); thr_t *t = h_obj(h, S2A_OBJ_THREAD); int b = t && t->wkind; pthread_mutex_unlock(&G); return b; }   // 该线程此刻在等待队列里（已阻塞）
int s2a_test_held_core(uint32_t h) { glock(); thr_t *t = h_obj(h, S2A_OBJ_THREAD); int c = t ? t->held_core - 1 : -2; pthread_mutex_unlock(&G); return c; }
int s2a_test_parked(uint32_t h) { glock(); thr_t *t = h_obj(h, S2A_OBJ_THREAD); int v = t && atomic_load(&t->pause) && atomic_load(&t->paused_ack) && !t->pwait; pthread_mutex_unlock(&G); return v; }   // 目标自己停进了 pause_park（ack 不是暂停方代发的）
// t_thread ㉗：把本线程置成「正在系统调用里」并取核 —— 模拟 svc 处理中卡住、回不到 svc_exit 的现场（宿主区合法持核形态之一）
void s2a_test_host_hold_svc(void) { thr_t *t = thread_current_or_null(); if (!t) return; atomic_store(&t->in_svc, 1); core_acquire(t); }
int s2a_test_unpaired(void) { return atomic_load(&g_unpaired_n); }   // 惰性取核：钩子里「DM_BLOCK 未配对」早报的次数
int s2a_test_core_lendable(uint32_t h) { glock(); thr_t *t = h_obj(h, S2A_OBJ_THREAD); int v = t ? (!t->held_core || atomic_load(&t->csoft) == 1) : -1; pthread_mutex_unlock(&G); return v; }   // 单测：不持核，或持着但宿主侧可借（等核者能直接取走）
void s2a_test_soft_counts(uint64_t *keeps, uint64_t *steals) { glock(); uint64_t k = atomic_load(&g_soft_keeps); for (thr_t *t = g_all; t; t = t->all) if (!t->exited) k += t->soft_keeps; pthread_mutex_unlock(&G); *keeps = k; *steals = atomic_load(&g_soft_steals); }   // 单测：宿主侧可借计数
int s2a_test_off_guest(uint32_t h) { glock(); thr_t *t = h_obj(h, S2A_OBJ_THREAD); int v = t ? atomic_load(&t->off_guest) : -1; pthread_mutex_unlock(&G); return v; }   // 惰性取核：block_end 之后应停在 1（回客体代码前不持核）
int s2a_test_waiters(int kind, uint64_t a) { int n = 0; glock(); for (thr_t *t = g_wait; t; t = t->next) if (t->wkind == kind && (kind == 2 ? t->wkey : t->waddr) == a) n++; pthread_mutex_unlock(&G); return n; }
// 第一个匹配的等待者句柄（0 = 没有）：单测按句柄查 0x34 的条件留档与逐线程被发信号次数
uint32_t s2a_test_wait_first(int kind, uint64_t a) {
    uint32_t h = 0; glock();
    for (thr_t *t = g_wait; t; t = t->next) if (t->wkind == kind && (kind == 2 ? t->wkey : t->waddr) == a) { h = t->handle; break; }
    pthread_mutex_unlock(&G); return h;
}
// t_thread ㉓：等待中的线程此刻的 0x34 条件（类型 / 比较值 / 超时）与等待期间被发信号次数
int s2a_test_wait_fields(uint32_t h, int *type, int32_t *val, int64_t *tmo, unsigned *nsig) {
    int ok = 0; glock(); thr_t *t = h_obj(h, S2A_OBJ_THREAD);
    if ((ok = t && t->wkind)) { if (type) *type = t->wtype; if (val) *val = t->wval; if (tmo) *tmo = t->wtmo; if (nsig) *nsig = t->nsig; }
    pthread_mutex_unlock(&G); return ok;
}
// t_thread ⑪：yield 同优先级轮转的决策点，确定性反证（不走并发、不受 sched_yield 影响）。
// 核 0 空、A 让出（yielded）、B 同优先级同核且在等核队列里登记更早（下标更小）时，core_claimed(0,A) 必返 1——
// 挡住让出者 A 重取核、把核让给登记更早的同优先级 B（Switch yield 轮转）；A 未让出时必返 0。
// 旧版 core_claimed 不看 yielded，两种都返 0 → 返回值 0（反证不过）。期望返回 1。
int s2a_test_yield_claims(void) {
    thr_t A = {0}, B = {0}; A.prio = B.prio = 44; A.mask = B.mask = 1;
    pthread_mutex_lock(&g_cmu);
    thr_t *s0 = g_cw[0], *s1 = g_cw[1]; int sn = g_ncw;   // 存现场
    g_cw[0] = &B; g_cw[1] = &A; g_ncw = 2;                // B 登记更早（下标 0）、A 其后（下标 1）
    A.yielded = 1; int yes = core_claimed(0, &A) ? 1 : 0; // 让出：新版 1、旧版 0
    A.yielded = 0; int no  = core_claimed(0, &A) ? 1 : 0; // 未让出：新旧都 0
    g_cw[0] = s0; g_cw[1] = s1; g_ncw = sn;               // 恢复现场
    pthread_mutex_unlock(&g_cmu);
    return yes | (no << 1);                               // 期望 1（yes=1、no=0）
}
#endif
// ---------- 现场导出（crash.c / 退出时）----------
void s2a_thread_dump(void) {
    static const char *const wk[] = {"在跑", "等锁", "等条件", "等地址", "等同步"};
    // 试拿最多 500 ms：G 只在系统调用里短暂持有，一次 trylock 不成多半是碰上别人正持锁（10-20 测试设备2 现场即此，全线程里无人等 G）；
    // 500 ms 仍拿不到才是真被占（崩溃发生在持锁时 / 持锁阻塞）
    int got = 0; for (int i = 0; i < 500 && !(got = !pthread_mutex_trylock(&G)); i++) usleep(1000);
    if (!got) { LOG("线程表：全局锁 500 ms 拿不到，跳过（崩溃发生在持锁时，或有线程持锁阻塞）"); return; }
    for (thr_t *t = g_all; t; t = t->all) {
        module_t *m = module_of((void *)t->last_lr);
#if defined(__APPLE__)
        qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0; if (!t->exited && t->pt) pthread_get_qos_class_np(t->pt, &qc, &rp);
#else
        int qc = SCHED_OTHER; struct sched_param parameter = {0};
        if (!t->exited && t->pt) pthread_getschedparam(t->pt, &qc, &parameter);
#endif
        // 核 = 理想核；掩码 / 持核（虚拟核开时此刻持有的核，-1 = 未持有）/ 等核：查同核互斥卡死用（10-20 八方2 开虚拟核卡死）
        LOG("  线程 %#x id %llu QoS %#x 优先级 %d 核 %d 掩码 %#llx 持核 %d %s%s%s 最近 svc %#x lr %s+%#llx 共 %llu 次 等待地址 %#llx", t->handle, (unsigned long long)t->id, qc, t->prio, t->core,
            (unsigned long long)t->mask, t->held_core - 1,
            t->exited ? "已退出" : !t->started ? "未启动" : wk[(t->wkind < 0 || t->wkind > 4) ? 0 : t->wkind], atomic_load(&t->wcore) ? "（等核）" : "", atomic_load(&t->pause) ? "（暂停）" : "",
            t->last_svc, m ? m->name : "?", (unsigned long long)(m ? t->last_lr - (uint64_t)m->base : t->last_lr), (unsigned long long)t->nsvc, (unsigned long long)t->waddr);
        if (!t->exited && t->started && t->wkind == 0 && t->sv_n) {   // 在跑：最近的同步类系统调用（旧 → 新），看空转时游戏给了什么、我们回了什么
            uint32_t n = t->sv_n < 8 ? t->sv_n : 8;
            for (uint32_t i = 0; i < n; i++) {
                const typeof(t->sv_ring[0]) *e = &t->sv_ring[(t->sv_n - n + i) % 8]; module_t *em = module_of((void *)e->lr);
                LOG("    同步调用 %u/%u：svc %#x 参数 %#llx %#llx %#llx %#llx，地址处值 调前 %#x 调后 %#x，返回 %#x，lr %s+%#llx", i + 1, n, e->num,
                    (unsigned long long)e->a[0], (unsigned long long)e->a[1], (unsigned long long)e->a[2], (unsigned long long)e->a[3], e->pre, e->post, e->ret,
                    em ? em->name : "?", (unsigned long long)(em ? e->lr - (uint64_t)em->base : e->lr));
            }
        }
#if defined(__APPLE__)
        // 客体回溯（10-25 哈迪斯卡面板：只有 svc 包装函数的 lr 看不出是谁在轮询）：沿 x29 帧链走 ≤ 8 层，打每层返回地址的模块 + 偏移。
        //   在系统调用里的线程：用进门时存的 x29（栈此刻不动）。在跑（游戏代码里、不在系统调用）的线程：短暂停住它的宿主线程读 pc / lr / fp 再放开
        //   （卡住时主线程多半在跑：看不到它在哪个循环里就定不了卡点）。帧读取一律经 vm_read_overwrite，读不到即停，不会因坏帧指针崩
        if (!t->exited && t->started && t->pt && !pthread_equal(t->pt, pthread_self())) {
            uint64_t pc = 0, lr = 0, fp = 0, xr[29]; int run = !atomic_load(&t->in_svc), got_x = 0;
            if (!run) fp = t->last_fp;
            else if (t->mport && thread_suspend(t->mport) == KERN_SUCCESS) {
                // 停住后先看宿主线程的运行状态：在内核里不可中断等待（缺页读写文件、驱动调用之类）时 thread_get_state 要等它走到可停的点，
                //   一直出不来就把看门线程也卡死（11-14 一台测试设备卡在第 17515 帧：线程表打到第 4 条就断，宿主栈和后两次现场都没出）。这种线程不取寄存器、标出来——
                //   「客体代码里却卡在内核」正是要找的卡点
                thread_basic_info_data_t bi; mach_msg_type_number_t bc = THREAD_BASIC_INFO_COUNT;
                int unint = thread_info(t->mport, THREAD_BASIC_INFO, (thread_info_t)&bi, &bc) == KERN_SUCCESS && bi.run_state == TH_STATE_UNINTERRUPTIBLE;
                arm_thread_state64_t st; mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
                if (unint) LOG("    【红】这条线程不在系统调用里，宿主线程却在内核里不可中断等待（缺页读写文件 / 驱动调用之类），不取寄存器（取了会把看门线程一起卡住）");
                else if (thread_get_state(t->mport, ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS) {
                    pc = arm_thread_state64_get_pc(st); lr = arm_thread_state64_get_lr(st); fp = arm_thread_state64_get_fp(st);
                    for (int i = 0; i < 29; i++) xr[i] = st.__x[i];
                    got_x = 1; }
                thread_resume(t->mport);
            }
            // 在跑线程的通用寄存器（恢复它之后再打，停着时不碰日志锁）：游戏在空转循环里的计数、下标、数据指针都在这里
            //   （11-14 停帧：166 号在解析 BYML，循环次数来自数据里的节点头，要看计数 / 下标 / 指针才判得了数据是不是坏的）
            if (got_x) for (int i = 0; i < 29; i += 4)
                LOG("    寄存器 x%-2d %016llx %016llx %016llx %016llx", i, (unsigned long long)xr[i], (unsigned long long)(i + 1 < 29 ? xr[i + 1] : 0),
                    (unsigned long long)(i + 2 < 29 ? xr[i + 2] : 0), (unsigned long long)(i + 3 < 29 ? xr[i + 3] : 0));
            char bt[2048], w[256]; int bn = 0;   // 每帧带导出名（s2a_guest_where，#806）：一帧至多约 200 字节
            uint64_t at[2] = {pc, lr};
            for (int k = 0; k < 2; k++) { module_t *rm = at[k] ? module_of((void *)at[k]) : NULL;   // 宿主代码里（门、桩）的 pc / lr 不在游戏模块内，略过
                if (rm && s2a_guest_where(at[k], w, sizeof w)) bn += snprintf(bt + bn, sizeof bt - bn, " %s%s", k ? "lr " : "pc ", w); }
            for (int d = 0; d < 8 && fp && !(fp & 0xF); d++) {
                uint64_t fr[2]; vm_size_t got = 0;
                if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof fr, (vm_address_t)fr, &got) != KERN_SUCCESS || got != sizeof fr) break;
                if (!s2a_guest_where(fr[1], w, sizeof w)) break;
                bn += snprintf(bt + bn, sizeof bt - bn, " ← %s", w);
                if (fr[0] <= fp || bn >= (int)sizeof bt - 256) break;
                fp = fr[0];
            }
            if (bn) LOG("    客体回溯%s%s", run ? "（在跑，采样）" : "", bt);
            // 采样到的 pc / lr 落在宿主代码（core_acquire 里等核、宿主层阻塞）时上面一行打不出任何帧——
            //   而「持核者在哪」正是判「等核是不是被持核者卡住」的唯一线索，所以原样把地址打出来
            else if (run && (pc || lr)) LOG("    客体回溯（在跑，采样）：pc %#llx lr %#llx 不在游戏模块内（宿主代码），fp %#llx", (unsigned long long)pc, (unsigned long long)lr, (unsigned long long)fp);
        }
#endif
        if (t->tlr && !t->exited) { uint64_t tt = *(uint64_t *)((uint8_t *)t->tlr + 0x1f8);   // SDK 线程对象里的句柄字段（锁标签来源）与线程句柄不一致时打出
            if (tt >= 0x1000 && *(uint32_t *)(tt + 0x1b0) != t->handle) LOG("    线程对象 %#llx 句柄字段(+0x1b0) %#x ≠ 线程句柄", (unsigned long long)tt, *(uint32_t *)(tt + 0x1b0)); }
        if (t->wkind == 1 && t->waddr) {   // 等锁：锁字当前值与持有者（锁字 = 持有者句柄 | 等待位），据此找持锁线程在干什么
            uint32_t v = ld32(t->waddr); thr_t *o = h_obj(v & ~WAITERS_BIT, S2A_OBJ_THREAD);
            LOG("    锁字 %#x → 持有者 %s%llu%s", v, o ? "id " : "句柄无效 ", o ? (unsigned long long)o->id : (unsigned long long)(v & ~WAITERS_BIT), o ? (o->wkind == 1 && o->waddr == t->waddr ? "（自己也在等这把锁）" : "") : "");
        } else if (!t->wkind && t->last_svc == 0x1A && t->waddr) {   // 刚从 ArbitrateLock 出来在跑（空转时多是这种）：也打锁字
            uint32_t v = ld32(t->waddr); thr_t *o = h_obj(v & ~WAITERS_BIT, S2A_OBJ_THREAD);
            LOG("    锁字 %#x → 持有者 %s%llu", v, o ? "id " : "句柄无效 ", o ? (unsigned long long)o->id : (unsigned long long)(v & ~WAITERS_BIT));
        } else if (t->wkind == 3 && t->waddr) {   // 等地址：把 0x34 的条件原样写出来。只看「地址处值」定不了这个线程该不该醒：
            // 值等于比较值可能是「进门时条件就成立」（正常，等的是 0x35），也可能是「挂上之后值被改过而没人发信号」（丢唤醒）
            static const char *const at[] = {"小于则等", "减一后小于则等", "相等则等", "64 位相等则等"};
            char tmo[24], sig[96]; int ty = t->wtype;
            if (t->wtmo < 0) snprintf(tmo, sizeof tmo, "无限"); else snprintf(tmo, sizeof tmo, "%lld ms", (long long)(t->wtmo / 1000000));
            // 有没有人来过、是谁、多久前：state 字里没有 owner（SDK 结构，改不了），只有 0x35 入口知道。
            //   这一行才是「丢唤醒」与「信号方那一路根本没跑」的分水岭——两种处置完全不同（10-30 botw 卡死线）
            if (t->sig_id) snprintf(sig, sizeof sig, "，最近一次被 id %llu 发信号在 %.0f ms 前", (unsigned long long)t->sig_id, mach_ms(thread_clock() - t->sig_tick));
            else snprintf(sig, sizeof sig, "，从没被任何线程对这个地址发过信号");
            if (wait_trace_on()) LOG("    地址处值 %#x；条件「%s」(类型 %d) 比较值 %d，超时 %s%s；等待期间此地址被发信号 %u 次",
                ld32(t->waddr), at[(unsigned)ty > 3 ? 0 : ty], ty, (int)t->wval, tmo, sig, t->nsig);
            else LOG("    地址处值 %#x；条件「%s」(类型 %d) 比较值 %d，超时 %s%s",
                ld32(t->waddr), at[(unsigned)ty > 3 ? 0 : ty], ty, (int)t->wval, tmo, sig);
        }
        else if (t->wkind == 2 && t->wkey) {   // 等条件：键当前值、已等多久、这个键最近一次被谁发过信号
            unsigned i = cvl_h(t->wkey); uint64_t now = thread_clock();
            // 锁字解出持有者：SDK 的条件变量协议是「持锁方决定要不要发信号」，锁在谁手里就是「谁该 signal」的第一嫌疑人
            uint32_t lv = ld32(t->waddr); thr_t *lo = lv ? h_obj(lv & ~WAITERS_BIT, S2A_OBJ_THREAD) : NULL;
            char mine[192];
            if (!lv) snprintf(mine, sizeof mine, "，锁 %#llx 锁字 0（空闲：谁改条件谁该发信号）", (unsigned long long)t->waddr);
            else snprintf(mine, sizeof mine, "，锁 %#llx 锁字 %#x（持有者 %s%llu%s）", (unsigned long long)t->waddr, lv,
                          lo ? "id " : "句柄无效 ", lo ? (unsigned long long)lo->id : (unsigned long long)(lv & ~WAITERS_BIT),
                          lo && lo->wkind == 1 && lo->waddr == t->waddr ? "，自己也在等这把锁" : "");
            if (wait_trace_on()) { size_t k = strlen(mine);   // 逐线程计数：键表撞掉时「从没被发过」是假象，这个不是
                snprintf(mine + k, sizeof mine - k, "；等待期间本键被发信号 %u 次", t->nsig); }
            if (g_cvl[i].key == t->wkey)
                LOG("    条件键 %#llx 当前值 %#x，已等 %.0f ms；此键共被发信号 %llu 次，最近一次线程 id %llu 在 %.0f ms 前（唤醒 %d 条、之后剩 %d 条）%s",
                    (unsigned long long)t->wkey, ld32(t->wkey), mach_ms(now - t->wait_t0), (unsigned long long)g_cvl[i].n, (unsigned long long)g_cvl[i].sig_id,
                    mach_ms(now - g_cvl[i].t), g_cvl[i].woken, g_cvl[i].left, mine);
            else LOG("    条件键 %#llx 当前值 %#x，已等 %.0f ms；此键从没被发过信号（或记录被别的键覆盖）%s",
                     (unsigned long long)t->wkey, ld32(t->wkey), mach_ms(now - t->wait_t0), mine);
            // 锁 / 键周围的原样内存（#19361b：上古卷轴加载线程在 WaitLightEvent 里等，键「从没被发过信号」、锁字 0——
            //   判不了是「事件已置位、唤醒丢了」还是「根本没人置位」：两者只差键后面那几个字节里的「已置位」标志）。
            //   从锁地址按 16 对齐往前 16 字节起打 48 字节，盖住游戏自己包在外面的锁字与 SDK 事件的锁 / 键 / 已置位
#if defined(__APPLE__)   // 安全读靠 Mach vm_read_overwrite（同上面抽样回溯）；安卓不打
            { uint64_t a0 = (t->waddr < t->wkey ? t->waddr : t->wkey) & ~0xFull; a0 -= 0x10; uint32_t m[12]; vm_size_t got = 0;
              if (vm_read_overwrite(mach_task_self(), (vm_address_t)a0, sizeof m, (vm_address_t)m, &got) == KERN_SUCCESS && got == sizeof m)
                  LOG("    键周围内存 %#llx：%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x", (unsigned long long)a0,
                      m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
              else LOG("    键周围内存 %#llx：读不了", (unsigned long long)a0); }
#endif
        }
        if (t->wkind && t->wkind != 2 && !t->exited) LOG("    已等 %.0f ms", mach_ms(thread_clock() - t->wait_t0));
        if (t->wkind == 4 && t->wn > 0) {   // 在等哪些句柄：种类 + cookie（C# 对象 / 事件号）——事件一直不来时据此找是哪个服务的
            static const char *const kn[] = {"无", "线程", "事件读", "事件写", "会话", "共享内存", "传输内存", "端口", "C#", "进程"};
            char b[512]; size_t k = 0;
            for (int j = 0; j < t->wn && j < 16 && k < sizeof b - 64; j++) { int i = h_idx(t->wh[j]); unsigned kd = i ? g_h[i].kind : 0;
                k += (size_t)snprintf(b + k, sizeof b - k, " %#x=%s%s", t->wh[j], kd < 10 ? kn[kd] : "?", i && kd == S2A_OBJ_EVENT_R && ((event_t *)g_h[i].obj)->signaled ? "(已触发)" : ""); }
            LOG("    等同步句柄 %d 个：%s", t->wn, b);
        }
    }
    pthread_mutex_unlock(&G);
}

#ifdef S2A_UNIT_TEST
// 仅供单测：在没有 flush 的情况下生产固定数量，检查环首尾。
int s2a_test_wait_ring_probe(uint64_t *first, uint64_t *last, size_t *pending, uint64_t *dropped) {
    glock(); thr_t *t=cur();
    for (int i=0; i<WD_CAP+3; ++i) wait_diag_add_locked(t, WD_BEGIN);
    *pending=g_wd_used; *dropped=g_wd_dropped;
    *first=g_wd[g_wd_read].seq; *last=g_wd[(g_wd_read+g_wd_used-1)%WD_CAP].seq;
    pthread_mutex_unlock(&G); return 1;
}
size_t s2a_test_wait_records(void) { glock(); size_t n=g_wd_used; pthread_mutex_unlock(&G); return n; }
uint64_t s2a_test_wait_dropped(void) { glock(); uint64_t n=g_wd_dropped; pthread_mutex_unlock(&G); return n; }
int s2a_test_wait_enabled(void) { return wait_diag_on(); }
int s2a_test_wait_source_svc(void) { return g_wd_test_source_svc; }
void s2a_wait_diag_report(void) { wait_diag_flush(2); }
#endif
// 即时存档：宿主子系统里存的「事件对象」与游戏句柄互查（存时把宿主事件指针换成句柄号，恢复后按句柄号取回本进程的事件对象并加一份引用）
uint32_t s2a_snap_event_handle_of(uint64_t ev) {
    glock(); uint32_t h = 0;
    for (int i = 1; i < MAX_H && !h; i++) if ((g_h[i].kind & 15) == S2A_OBJ_EVENT_R && g_h[i].obj == (void *)(uintptr_t)ev) h = ((uint32_t)g_h[i].gen << 15) | (uint32_t)i;
    pthread_mutex_unlock(&G); return h;
}
uint64_t s2a_snap_event_of_handle(uint32_t h) {
    glock(); int i = h_idx(h); event_t *e = i && ((g_h[i].kind & 15) == S2A_OBJ_EVENT_R || (g_h[i].kind & 15) == S2A_OBJ_EVENT_W) ? g_h[i].obj : NULL; if (e) e->refs++;
    pthread_mutex_unlock(&G); return (uint64_t)(uintptr_t)e;
}
