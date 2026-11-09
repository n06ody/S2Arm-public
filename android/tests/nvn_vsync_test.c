// nvn_vsync_android.c 宿主测试：替身 ALooper/AChoreographer（真实线程派发模型，
// post 登记一拍、fire 在节拍线程上派发回调——与平台的一次性回调语义一致）。
//
// 替身与生产共用 <android/choreographer.h>/<android/looper.h> 真头：CMake 用
// -idirafter 把本机 NDK sysroot/usr/include 挂上（不 vendored 进仓库；缺 NDK 时
// android_nvn_vsync 显式登记为跳过）。替身符号表的名字来自真头声明过的标识符
// （kNdk 用 #fn 字符串化 + &fn 编译期钉死），实现侧 dlsym 找错名字串 → 替身表
// 查不到 → 回退路径或 why。FrameCallbackData 是不透明结构体，夹具按自己的字段
// 布局补全后塞给回调。
//
// 覆盖：节拍到达、vsyncs>0 的 60Hz 槽位等拍（60Hz/120Hz 屏两档）、丢拍超时、
// 后台/停局停节拍、重启恢复、门（debug.s2a.vsync 默认关）、不可用回退、
// API 33→29→24 回退路径、preferredIndex 语义（非末条）、expected≠deadline 负控面。
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../../graphics/nvn/gfx/nvn/nvn_vsync.h"
#include "../native/nvn/nvn_vsync_android.h"

static uint64_t t_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static void t_sleep_ms(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
static int64_t slot_of(int64_t ns) { return ns * 60 / 1000000000; }   // 与实现同口径：floor(target*60)

static int t_got, t_fail;
#define T(cond) do { if (!(cond)) { printf("  FAIL %s:%d %s\n", __func__, __LINE__, #cond); t_fail++; } else t_got++; } while (0)

// ---------------- 替身平台 ----------------
// 不透明类型在夹具里补全：帧起始时刻 + 时间线数组（preferred/expected/deadline 各自独立，
// 以便断言「取 preferredIndex 的 expected，不是末条、不是 deadline」）。
struct AChoreographerFrameCallbackData {
    int64_t frame_ns;                          // getFrameTimeNanos：帧/拍起始时刻
    size_t len, pref;
    int64_t expected[4], deadline[4];
};
_Static_assert(sizeof(struct AChoreographerFrameCallbackData) > 0, "fixture completes the opaque type");

static struct {
    pthread_mutex_t mu; pthread_cond_t cv;
    AChoreographer_vsyncCallback cb_vs;        // API 33+ 登记的一次性回调
    AChoreographer_frameCallback64 cb64;       // API 29+
    AChoreographer_frameCallback cb;           // API 24+
    void *cb_user;
    int pending, woke;
    int64_t frame_ns_fallback;                 // cb64/cb 派发用的 frameTimeNanos
    struct AChoreographerFrameCallbackData data;
    const char *hide[4]; int hide_n;           // dlsym 要查不到的名字（回退路径测试）
} fk;

static ALooper *fk_looper(int opts) { (void)opts; return (ALooper *)0x1; }
static int fk_poll(int ms, int *fd, int *ev, void **data) {
    (void)ms; (void)fd; (void)ev; (void)data;
    pthread_mutex_lock(&fk.mu);
    while (!fk.pending && !fk.woke) pthread_cond_wait(&fk.cv, &fk.mu);
    if (fk.pending) {
        fk.pending = 0;
        AChoreographer_vsyncCallback v = fk.cb_vs;
        AChoreographer_frameCallback64 f64 = fk.cb64;
        AChoreographer_frameCallback f = fk.cb;
        void *u = fk.cb_user;
        pthread_mutex_unlock(&fk.mu);
        if (v) v(&fk.data, u);
        else if (f64) f64(fk.frame_ns_fallback, u);
        else if (f) f((long)fk.frame_ns_fallback, u);
        return 1;
    }
    fk.woke = 0;
    pthread_mutex_unlock(&fk.mu);
    return 0;
}
static void fk_wake(ALooper *looper) { (void)looper; pthread_mutex_lock(&fk.mu); fk.woke = 1; pthread_cond_broadcast(&fk.cv); pthread_mutex_unlock(&fk.mu); }
static AChoreographer *fk_choreo(void) { return (AChoreographer *)0x2; }
static void fk_post_vs(AChoreographer *c, AChoreographer_vsyncCallback cb, void *u) { (void)c; pthread_mutex_lock(&fk.mu); fk.cb_vs = cb; fk.cb64 = NULL; fk.cb = NULL; fk.cb_user = u; pthread_mutex_unlock(&fk.mu); }
static void fk_post64(AChoreographer *c, AChoreographer_frameCallback64 cb, void *u) { (void)c; pthread_mutex_lock(&fk.mu); fk.cb64 = cb; fk.cb_vs = NULL; fk.cb = NULL; fk.cb_user = u; pthread_mutex_unlock(&fk.mu); }
static void fk_post(AChoreographer *c, AChoreographer_frameCallback cb, void *u) { (void)c; pthread_mutex_lock(&fk.mu); fk.cb = cb; fk.cb_vs = NULL; fk.cb64 = NULL; fk.cb_user = u; pthread_mutex_unlock(&fk.mu); }
static int64_t fk_frame_time(const AChoreographerFrameCallbackData *d) { return d->frame_ns; }
static size_t fk_tl_len(const AChoreographerFrameCallbackData *d) { return d->len; }
static size_t fk_tl_pref(const AChoreographerFrameCallbackData *d) { return d->pref; }
static int64_t fk_tl_expected(const AChoreographerFrameCallbackData *d, size_t i) { return i < d->len ? d->expected[i] : INT64_MAX; }
static int64_t fk_tl_deadline(const AChoreographerFrameCallbackData *d, size_t i) { return i < d->len ? d->deadline[i] : INT64_MAX; }
static AVsyncId fk_tl_vsync_id(const AChoreographerFrameCallbackData *d, size_t i) { (void)d; return (AVsyncId)i; }

// NDK 符号表：名字 = 头文件声明过的标识符字符串化；&fn 引用保证该标识符真在头里声明过
// （夹具若自造名字，DECLARED 行直接编译红——杜绝「替身与实现一起写错」）。
#define REG(fn, impl) { #fn, (void *)(impl) }
#define DECLARED(fn) _Static_assert(sizeof(&(fn)) == sizeof(void *), "fixture name not in NDK header: " #fn)
DECLARED(ALooper_prepare); DECLARED(ALooper_pollOnce); DECLARED(ALooper_wake);
DECLARED(AChoreographer_getInstance); DECLARED(AChoreographer_postVsyncCallback);
DECLARED(AChoreographer_postFrameCallback64);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
DECLARED(AChoreographer_postFrameCallback);      // 真头 __DEPRECATED_IN(29)：符号存在即可用，钉名不判新旧
#pragma clang diagnostic pop
DECLARED(AChoreographerFrameCallbackData_getFrameTimeNanos);
DECLARED(AChoreographerFrameCallbackData_getFrameTimelinesLength);
DECLARED(AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex);
DECLARED(AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos);
DECLARED(AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos);
DECLARED(AChoreographerFrameCallbackData_getFrameTimelineVsyncId);
static const struct { const char *name; void *fn; } kNdk[] = {
    REG(ALooper_prepare, fk_looper),
    REG(ALooper_pollOnce, fk_poll),
    REG(ALooper_wake, fk_wake),
    REG(AChoreographer_getInstance, fk_choreo),
    REG(AChoreographer_postVsyncCallback, fk_post_vs),
    REG(AChoreographer_postFrameCallback64, fk_post64),
    REG(AChoreographer_postFrameCallback, fk_post),
    REG(AChoreographerFrameCallbackData_getFrameTimeNanos, fk_frame_time),
    REG(AChoreographerFrameCallbackData_getFrameTimelinesLength, fk_tl_len),
    REG(AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex, fk_tl_pref),
    REG(AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos, fk_tl_expected),
    REG(AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos, fk_tl_deadline),
    REG(AChoreographerFrameCallbackData_getFrameTimelineVsyncId, fk_tl_vsync_id),
};
static void *fk_dlsym(const char *name) {
    for (int i = 0; i < fk.hide_n; i++) if (fk.hide[i] && !strcmp(name, fk.hide[i])) return NULL;
    for (size_t i = 0; i < sizeof kNdk / sizeof kNdk[0]; i++)
        if (!strcmp(name, kNdk[i].name)) return kNdk[i].fn;
    return NULL;
}

// 节拍派发：目标呈现时刻 = target_ns 进 expected；deadline 恒取 target-4ms（≠expected——
// 实现若误用 deadline 当呈现时刻立刻红）；帧起始取 target-8ms。
static _Atomic int64_t g_last_target;
static void fire_at(int64_t target_ns) {
    atomic_store_explicit(&g_last_target, target_ns, memory_order_relaxed);
    pthread_mutex_lock(&fk.mu);
    fk.data.frame_ns = target_ns - 8000000;
    fk.data.len = 1; fk.data.pref = 0;
    fk.data.expected[0] = target_ns;
    fk.data.deadline[0] = target_ns - 4000000;
    fk.frame_ns_fallback = fk.data.frame_ns;
    fk.pending = 1;
    pthread_cond_broadcast(&fk.cv);
    pthread_mutex_unlock(&fk.mu);
}
static void fire_slots(int n) { int64_t t = atomic_load_explicit(&g_last_target, memory_order_relaxed); fire_at((slot_of(t) + n) * 1000000000 / 60 + 4000000); }   // 落 n 槽后 +4ms
static void fire_frac(int64_t ns) { fire_at(atomic_load_explicit(&g_last_target, memory_order_relaxed) + ns); }                                     // 拍内偏移（<1 槽）

// 计划 fire：一条线程按 6ms 间隔执行步骤——{n,0} 跳 n 槽；{0,ns} 拍内偏移。
// wait_tick 是「从调用点再等 N 槽」，拍必须落在等待进行中，所以不能先 fire 再等。
struct planstep { int slots; int64_t frac; };
static void *fire_plan(void *arg) {
    const struct planstep *p = arg;
    for (; p->slots || p->frac; p++) { t_sleep_ms(6); if (p->slots) fire_slots(p->slots); else fire_frac(p->frac); }
    return NULL;
}
#define PLAN(...) do { static const struct planstep p[] = { __VA_ARGS__, {0, 0} }; \
    pthread_t th; pthread_create(&th, NULL, fire_plan, (void *)p); pthread_detach(th); } while (0)

// 用户回调侧（契约层验证）：记拍数、tick_n、target_at
static struct { _Atomic uint64_t n; _Atomic int64_t last_n; _Atomic int calls; _Atomic double last_at; } usr;
static void usr_fn(void *u, double target_at, uint64_t tick_n, void *frame) {
    (void)u; (void)frame;
    atomic_fetch_add(&usr.calls, 1); atomic_store(&usr.last_n, (int64_t)tick_n);
    atomic_store(&usr.last_at, target_at);
    atomic_store(&usr.n, tick_n);
}
static void wait_usr_n(uint64_t want, int ms) { for (int i = 0; i < ms && atomic_load(&usr.n) < want; i++) t_sleep_ms(1); }

static int g_enabled = 1, g_use_fake = 1;
int s2a_vsync_test_enabled(void) { return g_enabled; }
void *(*s2a_vsync_test_dlsym(void))(const char *) { return g_use_fake ? fk_dlsym : NULL; }

int main(void) {
    pthread_mutex_init(&fk.mu, NULL); pthread_cond_init(&fk.cv, NULL);
    atomic_store(&g_last_target, (int64_t)t_now() + 1000000000);
    char why[96] = "";

    // 0a. 不可用回退：ops 探测结果进程内黏住，必须在首次解析前的干净子进程里测
    pid_t p = fork();
    if (p == 0) {
        g_use_fake = 0;                                            // 替身缺席 → 走真实 dlsym（宿主机上没有 libandroid.so）
        char w[96] = "";
        _exit(nvn_vsync_start(0, usr_fn, NULL, w, sizeof w) == -1 && w[0] ? 0 : 1);
    }
    int cst = 0; waitpid(p, &cst, 0);
    T(WIFEXITED(cst) && WEXITSTATUS(cst) == 0);

    // 0b. 回退路径（各自在干净子进程里跑，ops 解析不黏）：藏掉 API33 → postFrameCallback64 派上拍；
    //     再藏掉 postFrameCallback64 → postFrameCallback 派上拍。
    static const char *fb[][2] = {
        { "AChoreographer_postVsyncCallback", NULL },                       // → API 29 路径
        { "AChoreographer_postVsyncCallback", "AChoreographer_postFrameCallback64" }, // → API 24 路径
    };
    for (int k = 0; k < 2; k++) {
        pid_t q = fork();
        if (q == 0) {
            fk.hide_n = 0;
            for (size_t i = 0; i < sizeof fb[k] / sizeof fb[k][0] && fb[k][i]; i++) fk.hide[fk.hide_n++] = fb[k][i];
            _Atomic uint64_t got = 0;
            if (nvn_vsync_start(0, usr_fn, NULL, why, sizeof why) != 0) _exit(2);
            fire_at((int64_t)t_now() + 100000000);
            for (int i = 0; i < 300 && !atomic_load(&usr.n); i++) t_sleep_ms(1);
            got = atomic_load(&usr.n);
            _exit(got >= 1 ? 0 : 3);
        }
        int st = 0; waitpid(q, &st, 0);
        if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) printf("  fallback child k=%d raw st=%#x\n", k, st);
        T(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    // 1. 门默认关：active=0，wait_tick 立刻回退
    g_enabled = 0;
    T(s2a_android_link_active() == 0);
    T(s2a_android_link_wait_tick(0, 20000000ull) == 0);
    g_enabled = 1;

    // 2. 懒启动 + 节拍到达：wait_tick 拉节拍源，等一拍实际显示拍
    T(s2a_android_link_active() == 1);
    PLAN({1, 0});                                                    // 等待中来一拍
    T(s2a_android_link_wait_tick(0, 500000000ull) == 1);
    t_sleep_ms(30);

    // 3. 60Hz 槽位：wait_tick(N) 从调用点等 N 个槽前进
    T(s2a_android_link_wait_tick(1, 50000000ull) == 0);              // 无拍：超时回退
    PLAN({1, 0});
    T(s2a_android_link_wait_tick(1, 300000000ull) == 1);             // 1 拍 → 槽位+1 → 放行
    PLAN({1, 0});                                                  // 只来 1 拍
    T(s2a_android_link_wait_tick(2, 200000000ull) == 0);            // 要 2 槽只前进 1 → 超时
    PLAN({1, 0}, {1, 0});                                          // 来 2 拍
    T(s2a_android_link_wait_tick(2, 300000000ull) == 1);            // 槽位+2 → 放行

    // 4. 120Hz 屏口径：拍间隔减半，等 1 槽≈跨一个槽边界（约 2 拍）
    PLAN({0, 8333333}, {0, 8333333});                                // 两拍共 +16.67ms ≈ +1 槽
    T(s2a_android_link_wait_tick(1, 300000000ull) == 1);
    PLAN({0, 8333333});                                              // 只来半槽一拍
    T(s2a_android_link_wait_tick(1, 150000000ull) == 0);            // 槽位不前进 → 超时

    // 5. 契约层接管：nvn_vsync_start 换用户回调——tick_n 从 1 起、target_at=
    //    FrameCallbackData expected(preferredIndex)；link 的槽位/拍数记账不受影响
    T(nvn_vsync_start(0, usr_fn, NULL, why, sizeof why) == 0);
    fire_slots(1); wait_usr_n(1, 300);
    T(usr.calls == 1 && usr.last_n == 1);
    T(usr.last_at == (double)atomic_load(&g_last_target) / 1e9);     // expected[0]，不是 deadline（target-4ms）
    PLAN({1, 0});
    T(s2a_android_link_wait_tick(1, 300000000ull) == 1);
    wait_usr_n(2, 300); T(usr.calls == 2);

    // 5b. preferredIndex 语义：三条时间线 pref=1（非末条），expected/deadline 全不同——
    //     target_at 必须取 expected[pref]，不是末条 expected、不是任何 deadline。
    {
        int64_t base = atomic_load(&g_last_target) + 100000000;
        pthread_mutex_lock(&fk.mu);
        fk.data.frame_ns = base - 8000000;
        fk.data.len = 3; fk.data.pref = 1;
        fk.data.expected[0] = base - 16000000; fk.data.expected[1] = base; fk.data.expected[2] = base + 8000000;
        fk.data.deadline[0] = base - 20000000; fk.data.deadline[1] = base - 4000000; fk.data.deadline[2] = base + 4000000;
        fk.frame_ns_fallback = fk.data.frame_ns;
        fk.pending = 1; pthread_cond_broadcast(&fk.cv);
        pthread_mutex_unlock(&fk.mu);
        atomic_store(&g_last_target, base);
        wait_usr_n(3, 300);
        T(usr.calls == 3);
        T(usr.last_at == (double)base / 1e9);                        // expected[pref]
        T(usr.last_at != (double)(base + 8000000) / 1e9);            // ≠末条 expected
        T(usr.last_at != (double)(base - 4000000) / 1e9);            // ≠deadline[pref]
    }

    // 6. 后台/停局：stop 返回后回调不再派发；wait_tick 懒重启恢复
    nvn_vsync_stop();
    int calls = usr.calls;
    fire_at(atomic_load(&g_last_target) + 20000000); t_sleep_ms(20);
    T(usr.calls == calls);                                           // 停后不再调 fn
    PLAN({1, 0});
    T(s2a_android_link_wait_tick(0, 500000000ull) == 1);            // 懒重启：拍又来了
    t_sleep_ms(30);

    // 7. 重启槽位记账：停局冻结 g_slot（只升不降）。重启不清零的话，wait_tick(N) 拿旧局
    //    槽位当基点——真实时间走过后「等 N 槽」被首拍提前放行。夹具拍链带 +1s 提前，
    //    睡 >1s 让调用点槽位反超旧槽位，坏版才暴露。
    nvn_vsync_stop();
    t_sleep_ms(1100);                                          // 调用点槽位 ≈ 停局槽位 +66
    atomic_store(&g_last_target, (int64_t)t_now());            // 新局拍按真实时刻来
    PLAN({1, 0});
    T(s2a_android_link_wait_tick(2, 300000000ull) == 0);       // 修好版：首拍槽位 < 调用点+2 → 仍等满；坏版：首拍即放行
    PLAN({1, 0}, {1, 0});
    T(s2a_android_link_wait_tick(2, 300000000ull) == 1);       // 从该点再进 2 槽 → 放行（两版同绿——正控）

    // 8. 重复 start 先停旧的：tick_n 重新从 1 起
    usr.n = 0; usr.calls = 0;
    T(nvn_vsync_start(0, usr_fn, NULL, why, sizeof why) == 0);
    fire_slots(1); wait_usr_n(1, 300);
    T(usr.last_n == 1);
    nvn_vsync_stop();

    printf("nvn_vsync android: %s (%d checks, %d fail)\n", t_fail ? "FAILED" : "passed", t_got + t_fail, t_fail);
    return t_fail ? 1 : 0;
}
