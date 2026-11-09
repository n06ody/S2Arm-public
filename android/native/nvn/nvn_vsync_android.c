// 安卓节拍源：AChoreographer（#48 B1；契约 graphics/nvn/gfx/nvn/nvn_vsync.h）。
//
// Choreographer 的帧回调是「一次性」的：每拍都要重投下一拍。AChoreographer 绑定
// 调用线程的 ALooper、回调在持有该 looper 的线程上派发——所以节拍跑在一条专用
// looper 线程上（= nvn_vsync.h 说的「平台节拍线程 / 上屏线程」）：prepare、
// getInstance、投首拍，然后 ALooper_pollOnce 阻塞派发。
//
// NDK 符号全部经 dlsym 惰性探测（不直接绑缺失符号；符号名/签名对照
// android/choreographer.h、android/looper.h 真头——宿主测试经 -idirafter 本机 NDK
// sysroot 直接编真头，镜像与真头不一致的可能不存在）：
// API 33+ 优先 postVsyncCallback + FrameCallbackData 时间线——目标呈现时刻取
// ExpectedPresentationTimeNanos(preferredIndex)（对应 iOS
// CAMetalDisplayLinkUpdate.targetPresentationTimestamp；deadline 是「帧须此前就绪」
// 不是呈现时刻，VsyncId 是标识不是时刻，都不用）；API 29–32 退 postFrameCallback64
// （frameTimeNanos + 相邻拍实测周期估目标）；API 24–28 退 postFrameCallback。
// 拿不到可用节拍源返回 -1，核心回退 nvn_pace 计时。
//
// 直连消费侧（mtl_link_active / mtl_link_wait_tick → nvn_be_link_*）走
// s2a_android_link_* 助手：debug.s2a.vsync 门（默认关，等设备 A/B 再定默认）、
// vsyncs>0 按 60Hz 槽位等拍（floor(target_at*60) 绝对槽位前进 N；120Hz 屏上一拍
// ≈0.5 槽，等 1 槽≈16.7ms——与 iOS vperiod 语义对齐，不在平台侧造第二种口径）、
// 首次 wait_tick 懒启动。回调里只记账（原子计数 + 条件变量），不碰栅栏。
#include "../../../graphics/nvn/gfx/nvn/nvn_vsync.h"
#include "nvn_vsync_android.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifndef S2A_VSYNC_TEST
#include <sys/system_properties.h>
#endif

static uint64_t vs_now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
// cond 等待截止：Android/Linux 按仓库惯例把 cond 挂 CLOCK_MONOTONIC（见 wsi_lease.c，
// deadline 不受墙钟跳变影响）；macOS 宿主没有 pthread_condattr_setclock
//（backing_test.c 同款桩），退回 CLOCK_REALTIME。两边 cond 与截止时刻必须同钟——
// pthread_cond_timedwait 默认 REALTIME，用 MONOTONIC 算截止会让绝对时刻落在过去、
// 第一次等待就立刻超时。
#if defined(__APPLE__)
#  define VS_WAIT_CLOCK CLOCK_REALTIME
#else
#  define VS_WAIT_CLOCK CLOCK_MONOTONIC
#endif
static void vs_deadline_ts(uint64_t timeout_ns, struct timespec *ts) {
    clock_gettime(VS_WAIT_CLOCK, ts);
    ts->tv_sec += (time_t)(timeout_ns / 1000000000ull);
    ts->tv_nsec += (long)(timeout_ns % 1000000000ull);
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}
static void vs_why(char *why, size_t why_size, const char *msg) {
    if (why && why_size) snprintf(why, why_size, "%s", msg ? msg : "unavailable");
}

// ---------------- 平台符号表（dlsym / 测试替身） ----------------
static struct s2a_vs_ops g_ops;
static int g_ops_ready, g_ops_ok;
static char g_ops_why[160];

static int ops_resolve(char *why, size_t why_size) {
    if (g_ops_ready) { if (!g_ops_ok) vs_why(why, why_size, g_ops_why); return g_ops_ok; }
    g_ops_ready = 1;
#ifdef S2A_VSYNC_TEST
    // 替身符号表：名字→函数，表项名必须取自真头声明过的标识符（测试侧编译期钉死）。
    // NULL = 模拟「拿不到节拍源」，直接 fail 报 why。
    void *(*lookup)(const char *) = s2a_vsync_test_dlsym();
    void *h = lookup;
    if (!h) { snprintf(g_ops_why, sizeof g_ops_why, "test dlsym absent"); goto fail; }
#define SYM(dst, name) do { *(void **)&(dst) = lookup(name); } while (0)
#else
    void *h = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(g_ops_why, sizeof g_ops_why, "dlopen libandroid.so: %s", dlerror() ? dlerror() : "?"); goto fail; }
#define SYM(dst, name) do { *(void **)&(dst) = dlsym(h, name); } while (0)
#endif
    // 名字串 = NDK 真头声明名（android/choreographer.h / android/looper.h，逐字）。
    SYM(g_ops.ALooper_prepare, "ALooper_prepare");
    SYM(g_ops.ALooper_pollOnce, "ALooper_pollOnce");
    SYM(g_ops.ALooper_wake, "ALooper_wake");
    SYM(g_ops.AChoreographer_getInstance, "AChoreographer_getInstance");
    SYM(g_ops.AChoreographer_postVsyncCallback, "AChoreographer_postVsyncCallback");
    SYM(g_ops.AChoreographer_postFrameCallback64, "AChoreographer_postFrameCallback64");
    SYM(g_ops.AChoreographer_postFrameCallback, "AChoreographer_postFrameCallback");
    SYM(g_ops.AChoreographerFrameCallbackData_getFrameTimeNanos,
        "AChoreographerFrameCallbackData_getFrameTimeNanos");
    SYM(g_ops.AChoreographerFrameCallbackData_getFrameTimelinesLength,
        "AChoreographerFrameCallbackData_getFrameTimelinesLength");
    SYM(g_ops.AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex,
        "AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex");
    SYM(g_ops.AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos,
        "AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos");
#undef SYM
    // API 33 富时间线路径要四件套齐（postVsyncCallback + frameTime + 时间线条数/preferred +
    // expectedPresentation），不齐退回 postFrameCallback64 简单回调（拍照样到）。
    if (g_ops.AChoreographer_postVsyncCallback &&
        (!g_ops.AChoreographerFrameCallbackData_getFrameTimeNanos ||
         !g_ops.AChoreographerFrameCallbackData_getFrameTimelinesLength ||
         !g_ops.AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex ||
         !g_ops.AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos))
        g_ops.AChoreographer_postVsyncCallback = NULL;
    if (!g_ops.ALooper_prepare || !g_ops.ALooper_pollOnce || !g_ops.ALooper_wake ||
        !g_ops.AChoreographer_getInstance ||
        (!g_ops.AChoreographer_postVsyncCallback && !g_ops.AChoreographer_postFrameCallback64 &&
         !g_ops.AChoreographer_postFrameCallback)) {
        snprintf(g_ops_why, sizeof g_ops_why, "AChoreographer/ALooper NDK 符号缺失（API<24 或被裁剪）");
        goto fail;
    }
    g_ops_ok = 1;
    return 1;
fail:
    g_ops_ok = 0;
    vs_why(why, why_size, g_ops_why);
    return 0;
}

// ---------------- 节拍线程与回调 ----------------
// ALOOPER_PREPARE_ALLOW_NON_CALLBACKS 取自真头（android/looper.h）；节拍线程只用回调派发，带上此选项防御
// 个别实现把「无 fd 回调的 looper」判成不可用。

static pthread_t g_beat;
static int g_beat_up;                       // 节拍线程活着（需 join）
static ALooper *g_beat_looper;              // 节拍线程发布（stop 可能跑在发布之前，判空用）
static AChoreographer *g_choreo;
static _Atomic int g_stop, g_beat_state;    // state: 0=拉起中 1=有节拍源 2=拿不到节拍源已退出
static _Atomic uint64_t g_gen;              // 节拍源代次：回调线程内重开时旧线程按代次不符自退（g_stop 会被新 start 清零，挡不住它）
static _Thread_local uint64_t t_gen;
static _Atomic int g_running;
static nvn_vsync_fn g_fn;
static void *g_user;
static _Atomic uint64_t g_tick;
static int64_t g_last_vsync_ns;
static uint64_t g_period_ns = 16666666;     // 帧回调路径下实测的 vsync 周期（初值 60Hz）
static pthread_mutex_t g_state_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_state_cv;
static char g_start_why[160];
// link 等拍记账（由 deliver 统一更新，见下）：拍数见 g_tick，槽位见 g_slot
static pthread_mutex_t g_lk_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_lk_cv;
static int64_t g_slot;                                           // floor(最近一拍 target_at * 60)

static void conds_init(void) {
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
#if !defined(__APPLE__)
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);   // VS_WAIT_CLOCK 同侧：cond 挂单调钟
#endif
    pthread_cond_init(&g_state_cv, &ca); pthread_cond_init(&g_lk_cv, &ca);
    pthread_condattr_destroy(&ca);
}
static pthread_once_t g_conds_once = PTHREAD_ONCE_INIT;

static void vs_cb_vsync(const AChoreographerFrameCallbackData *data, void *user);
static void vs_cb_frame64(int64_t frame_ns, void *user);
static void vs_cb_frame(long frame_ns, void *user);

static void beat_post_next(void) {          // 只在节拍线程上调用
    if (g_ops.AChoreographer_postVsyncCallback)
        g_ops.AChoreographer_postVsyncCallback(g_choreo, vs_cb_vsync, NULL);
    else if (g_ops.AChoreographer_postFrameCallback64)
        g_ops.AChoreographer_postFrameCallback64(g_choreo, vs_cb_frame64, NULL);
    else
        g_ops.AChoreographer_postFrameCallback(g_choreo, vs_cb_frame, NULL);
}

// 记账（计拍、60Hz 槽位、实测周期），调用户 fn，仍在跑就重投下一拍。回调里不许
// 等栅栏——这里只做原子/常数时间操作。槽位记账在派发层统一做：节拍源不论由谁
// （link 助手还是别的契约消费者）开起来，link 的等拍计数都照走。
static void deliver(double target_s, int64_t vsync_ns) {
    if (vsync_ns > 0) {
        int64_t d = vsync_ns - g_last_vsync_ns;
        if (g_last_vsync_ns && d > 2000000 && d < 100000000) g_period_ns = (uint64_t)d;
        g_last_vsync_ns = vsync_ns;
    }
    uint64_t n = atomic_fetch_add_explicit(&g_tick, 1, memory_order_relaxed) + 1;
    pthread_mutex_lock(&g_lk_mu);
    int64_t s = (int64_t)(target_s * 60.0);
    if (s > g_slot) g_slot = s;
    pthread_cond_broadcast(&g_lk_cv);
    pthread_mutex_unlock(&g_lk_mu);
    if (g_fn) g_fn(g_user, target_s, n, NULL);
    if (!atomic_load_explicit(&g_stop, memory_order_acquire) &&
        t_gen == atomic_load(&g_gen)) beat_post_next();          // 代次不符 = 本线程已被新节拍源顶掉
}

// API 33 富时间线回调。NDK 语义：getFrameTimeNanos = 本帧渲染起点（拍起点）；
// 目标呈现时刻取平台默认调度的那条时间线 = ExpectedPresentationTimeNanos(preferredIndex)。
// deadline 是「帧要在此之前就绪才赶得上上屏」——不是呈现时刻；VsyncId 是标识符不是时刻，都不用。
static void vs_cb_vsync(const AChoreographerFrameCallbackData *data, void *user) {
    (void)user;
    int64_t target = 0, vsync = 0;
    if (g_ops.AChoreographerFrameCallbackData_getFrameTimeNanos)
        vsync = g_ops.AChoreographerFrameCallbackData_getFrameTimeNanos(data);
    if (g_ops.AChoreographerFrameCallbackData_getFrameTimelinesLength &&
        g_ops.AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex &&
        g_ops.AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos) {
        size_t len = g_ops.AChoreographerFrameCallbackData_getFrameTimelinesLength(data);
        size_t pref = g_ops.AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(data);
        if (pref < len)
            target = g_ops.AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(data, pref);
    }
    if (target <= 0) target = (vsync > 0 ? vsync : (int64_t)vs_now_ns()) + (int64_t)g_period_ns;
    deliver((double)target / 1e9, vsync);
}
static void vs_cb_frame64(int64_t frame_ns, void *user) {
    (void)user;
    deliver(((double)frame_ns + (double)g_period_ns) / 1e9, frame_ns);   // frameTimeNanos 是拍起点，目标呈现≈+1 周期
}
static void vs_cb_frame(long frame_ns, void *user) { vs_cb_frame64(frame_ns, user); }

static void *beat_main(void *arg) {
    (void)arg;
    t_gen = atomic_load(&g_gen);
    ALooper *looper = g_ops.ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    AChoreographer *choreo = looper ? g_ops.AChoreographer_getInstance() : NULL;
    if (!choreo) {
        snprintf(g_start_why, sizeof g_start_why, "%s", looper ? "AChoreographer_getInstance 返回 NULL" : "ALooper_prepare 失败");
        pthread_mutex_lock(&g_state_mu); g_beat_state = 2; pthread_cond_broadcast(&g_state_cv); pthread_mutex_unlock(&g_state_mu);
        return NULL;
    }
    g_beat_looper = looper; g_choreo = choreo;
    pthread_mutex_lock(&g_state_mu); g_beat_state = 1; pthread_cond_broadcast(&g_state_cv); pthread_mutex_unlock(&g_state_mu);
    beat_post_next();
    while (!atomic_load_explicit(&g_stop, memory_order_acquire) &&
           t_gen == atomic_load(&g_gen))                          // 代次不符（回调内被新 start 顶掉）同样自退
        g_ops.ALooper_pollOnce(-1, NULL, NULL, NULL);            // 阻塞到下一次派发（回调在本线程执行）
    return NULL;
}

// 起停共用一把生命周期锁：start 的「停旧→建线程→握手→置 running」与 stop 的
// 「置 stop→wake→join」互斥，杜绝并发 start/stop 交错出双 join 或半启动状态。
static pthread_mutex_t g_life_mu = PTHREAD_MUTEX_INITIALIZER;

static void beat_stop_locked(void) {                             // g_life_mu 持有中；节拍线程上不许调
    if (!g_beat_up) return;
    atomic_store_explicit(&g_running, 0, memory_order_release);
    atomic_store_explicit(&g_stop, 1, memory_order_release);
    if (g_beat_looper && g_ops.ALooper_wake) g_ops.ALooper_wake(g_beat_looper);
    pthread_join(g_beat, NULL);                                  // 节拍线程退出路径不碰 g_life_mu
    g_beat_up = 0; g_beat_looper = NULL; g_choreo = NULL; g_fn = NULL; g_user = NULL;
}

int nvn_vsync_start(uint32_t hz, nvn_vsync_fn fn, void *user, char *why, size_t why_size) {
    (void)hz;   // 期望频率只是提示：Choreographer 无变速 API，核心按 target_at 自己数拍
    if (!fn) { vs_why(why, why_size, "fn 为空"); return -1; }
    if (!ops_resolve(why, why_size)) return -1;
    pthread_once(&g_conds_once, conds_init);
    pthread_mutex_lock(&g_life_mu);
    if (g_beat_up && pthread_equal(pthread_self(), g_beat)) {    // 回调线程内重开（契约允许 [上屏] 调 start）：
        atomic_fetch_add(&g_gen, 1);                             // 旧线程 join 不了自己——顶代次让它回调返回后自退、
        atomic_store_explicit(&g_running, 0, memory_order_release);       // 脱管不再 join；新线程照旧建
        pthread_detach(g_beat);
        g_beat_up = 0; g_beat_looper = NULL; g_choreo = NULL;
    } else {
        beat_stop_locked();                                      // 重复调用先停旧的（含 fn 内违规 stop 的残留线程）
    }
    g_fn = fn; g_user = user; g_beat_looper = NULL; g_choreo = NULL;
    atomic_store_explicit(&g_tick, 0, memory_order_relaxed); g_last_vsync_ns = 0; g_period_ns = 16666666;
    pthread_mutex_lock(&g_lk_mu); g_slot = 0; pthread_mutex_unlock(&g_lk_mu);   // 槽位记账重启归零：wait_tick 的「等 N 槽」从新局调用点起算（旧局槽位冻结在停局时刻，沿用会提前放行）
    atomic_store_explicit(&g_stop, 0, memory_order_release); atomic_store_explicit(&g_beat_state, 0, memory_order_release);
    int rc = -1;
    if (pthread_create(&g_beat, NULL, beat_main, NULL)) { vs_why(why, why_size, "节拍线程创建失败"); goto out; }
    g_beat_up = 1;
    pthread_mutex_lock(&g_state_mu);
    struct timespec ts;
    vs_deadline_ts(500000000ull, &ts);                           // 节拍源握手 ≤500ms
    while (g_beat_state == 0)
        if (pthread_cond_timedwait(&g_state_cv, &g_state_mu, &ts) == ETIMEDOUT) break;
    int st = g_beat_state;
    pthread_mutex_unlock(&g_state_mu);
    if (st != 1) {                                               // 拿不到节拍源：收掉线程后报 -1
        beat_stop_locked();
        vs_why(why, why_size, st == 2 ? g_start_why : "节拍源握手超时");
        goto out;
    }
    atomic_store_explicit(&g_running, 1, memory_order_release);
    rc = 0;
out:
    pthread_mutex_unlock(&g_life_mu);
    return rc;
}

void nvn_vsync_stop(void) {
    pthread_mutex_lock(&g_life_mu);
    if (!g_beat_up) { pthread_mutex_unlock(&g_life_mu); return; } // 没开始时无操作
    if (pthread_equal(pthread_self(), g_beat)) {                 // 契约禁止在 fn 里调 stop：
        fprintf(stderr, "s2a: nvn_vsync_stop 不允许在回调里调用（节拍线程不能 join 自己）\n");
        atomic_store_explicit(&g_running, 0, memory_order_release);
        atomic_store_explicit(&g_stop, 1, memory_order_release);          // beat 看到 g_stop 自退，等下一个外部 stop 收尾
        pthread_mutex_unlock(&g_life_mu);
        return;
    }
    beat_stop_locked();                                          // 返回时保证 fn 不再被调用
    pthread_mutex_unlock(&g_life_mu);
}

// ---------------- 直连消费侧：link 助手（debug.s2a.vsync 门 + 槽位等拍） ----------------
// 等拍用的拍数/槽位由节拍派发层（deliver）统一记账，见上。
static _Atomic int g_link_broken;                                // 节拍源起过且失败：本局别再试，直接回退

static int link_enabled(void) {
#ifdef S2A_VSYNC_TEST
    return s2a_vsync_test_enabled();
#else
    static _Atomic int v = -1; static _Atomic uint64_t at;
    uint64_t now = vs_now_ns();
    if (v >= 0 && now - at < 1000000000ull) return v;            // 属性 1Hz 轮询（同 adpf/dguard 惯例）
    char b[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.s2a.vsync", b);
    int r = b[0] == '1';                                         // 默认关：只认显式 "1"
    v = r; at = now;
    return r;
#endif
}

// 懒启动时的用户回调：计拍/槽位记账已在节拍派发层统一做，这里不重复记账。
static void link_tick(void *user, double target_at, uint64_t tick_n, void *frame) {
    (void)user; (void)target_at; (void)tick_n; (void)frame;
}

int s2a_android_link_active(void) {
    if (!link_enabled()) {
        // 门关掉时收节拍省电——只收 link 助手自己开起来的源；别的契约消费者（nvn_vsync_start
        // 直达）开的源不受 link 门管辖，不动它。
        if (atomic_load(&g_running) && g_fn == link_tick) nvn_vsync_stop();
        return 0;
    }
    return !atomic_load(&g_link_broken);
}

int s2a_android_link_wait_tick(int vsyncs, uint64_t timeout_ns) {
    if (!s2a_android_link_active()) return 0;
    pthread_once(&g_conds_once, conds_init);
    if (!atomic_load(&g_running)) {
        char why[96] = "";
        if (nvn_vsync_start(0, link_tick, NULL, why, sizeof why)) {
            atomic_store(&g_link_broken, 1);
            fprintf(stderr, "s2a: vsync 节拍源不可用（%s），回退内置 60Hz 计时\n", why);
            return 0;
        }
    }
    struct timespec ts;
    vs_deadline_ts(timeout_ns, &ts);
    pthread_mutex_lock(&g_lk_mu);
    int ok = 0;
    if (vsyncs > 0) {                                            // 数 60Hz 槽位前进（60Hz 屏 1 拍≈1 槽，120Hz 屏 1 拍≈0.5 槽）
        int64_t want = (g_slot ? g_slot : (int64_t)(vs_now_ns() * 60 / 1000000000)) + vsyncs;
        while (!(ok = g_slot >= want)) if (pthread_cond_timedwait(&g_lk_cv, &g_lk_mu, &ts) == ETIMEDOUT) break;
    } else {                                                     // 等一拍实际显示拍
        uint64_t want = atomic_load(&g_tick) + 1;
        while (!(ok = atomic_load(&g_tick) >= want)) if (pthread_cond_timedwait(&g_lk_cv, &g_lk_mu, &ts) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&g_lk_mu);
    return ok;
}

void s2a_android_link_stop(void) { nvn_vsync_stop(); }
