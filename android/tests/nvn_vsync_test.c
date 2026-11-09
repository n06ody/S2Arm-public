// nvn_vsync_android.c 宿主测试：替身 ALooper/AChoreographer（真实线程派发模型，
// post 登记一拍、fire 在节拍线程上派发回调——与平台的一次性回调语义一致）。
// 覆盖：节拍到达、vsyncs>0 的 60Hz 槽位等拍（60Hz/120Hz 屏两档）、丢拍超时、
// 后台/停局停节拍、重启恢复、门（debug.s2a.vsync 默认关）、不可用回退。
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

// ---------------- 替身平台：post 登记一拍，poll_once 阻塞到 fire/wake 后在节拍线程上派发 ----------------
static struct {
    pthread_mutex_t mu; pthread_cond_t cv;
    s2a_vs_vsync_cb cb_vs; s2a_vs_frame64_cb cb64; s2a_vs_frame_cb cb;
    void *cb_user;
    int pending, woke;
    int64_t vsync_ns, deadline_ns;      // 替身 FrameCallbackData：拍起点 / 目标呈现时刻
} fk;

static void *fk_looper(int flags) { (void)flags; return (void *)0x1; }
static int fk_poll(int ms, int *fd, int *ev, void **data) {
    (void)ms; (void)fd; (void)ev; (void)data;
    pthread_mutex_lock(&fk.mu);
    while (!fk.pending && !fk.woke) pthread_cond_wait(&fk.cv, &fk.mu);
    if (fk.pending) {
        fk.pending = 0;
        s2a_vs_vsync_cb v = fk.cb_vs; s2a_vs_frame64_cb f64 = fk.cb64; s2a_vs_frame_cb f = fk.cb; void *u = fk.cb_user;
        pthread_mutex_unlock(&fk.mu);
        if (v) v(&fk, u); else if (f64) f64(fk.vsync_ns, u); else if (f) f((long)fk.vsync_ns, u);
        return 1;
    }
    fk.woke = 0;
    pthread_mutex_unlock(&fk.mu);
    return 0;
}
static void fk_wake(void *looper) { (void)looper; pthread_mutex_lock(&fk.mu); fk.woke = 1; pthread_cond_broadcast(&fk.cv); pthread_mutex_unlock(&fk.mu); }
static void *fk_choreo(void) { return (void *)0x2; }
static void fk_post_vs(void *c, s2a_vs_vsync_cb cb, void *u) { (void)c; pthread_mutex_lock(&fk.mu); fk.cb_vs = cb; fk.cb64 = NULL; fk.cb = NULL; fk.cb_user = u; pthread_mutex_unlock(&fk.mu); }
static void fk_post64(void *c, s2a_vs_frame64_cb cb, void *u) { (void)c; pthread_mutex_lock(&fk.mu); fk.cb64 = cb; fk.cb_vs = NULL; fk.cb = NULL; fk.cb_user = u; pthread_mutex_unlock(&fk.mu); }
static void fk_post(void *c, s2a_vs_frame_cb cb, void *u) { (void)c; pthread_mutex_lock(&fk.mu); fk.cb = cb; fk.cb_vs = NULL; fk.cb64 = NULL; fk.cb_user = u; pthread_mutex_unlock(&fk.mu); }
static int64_t fk_frame_time(const void *d) { (void)d; return fk.vsync_ns; }
static size_t fk_tl_len(const void *d) { (void)d; return 1; }
static int64_t fk_tl_vsync(const void *d, size_t i) { (void)d; (void)i; return fk.vsync_ns; }
static int64_t fk_tl_deadline(const void *d, size_t i) { (void)d; (void)i; return fk.deadline_ns; }

// 节拍派发（目标呈现时刻 = target_ns；vsync 起点取 target-8ms——拍内位置对槽位无影响）。
// g_last_target 是合成的“显示时钟”，与墙钟无关，全靠测试排步推进，天然确定性。
static _Atomic int64_t g_last_target;
static void fire_at(int64_t target_ns) {
    atomic_store_explicit(&g_last_target, target_ns, memory_order_relaxed);
    pthread_mutex_lock(&fk.mu);
    fk.vsync_ns = target_ns - 8000000; fk.deadline_ns = target_ns; fk.pending = 1;
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
static struct { _Atomic uint64_t n; _Atomic int64_t last_n; _Atomic int calls; double last_at; } usr;
static void usr_fn(void *u, double target_at, uint64_t tick_n, void *frame) {
    (void)u; (void)frame;
    atomic_fetch_add(&usr.calls, 1); usr.last_n = (int64_t)tick_n; usr.last_at = target_at;
    atomic_store(&usr.n, tick_n);
}
static void wait_usr_n(uint64_t want, int ms) { for (int i = 0; i < ms && atomic_load(&usr.n) < want; i++) t_sleep_ms(1); }

static int g_enabled = 1, g_use_fake = 1;
int s2a_vsync_test_enabled(void) { return g_enabled; }
const struct s2a_vs_ops *s2a_vsync_test_ops(void) {
    if (!g_use_fake) return NULL;
    static const struct s2a_vs_ops o = {
        .looper_prepare = fk_looper, .looper_poll_once = fk_poll, .looper_wake = fk_wake,
        .choreo_get = fk_choreo, .post_vsync64 = fk_post_vs, .post_frame64 = fk_post64, .post_frame = fk_post,
        .frame_time_ns = fk_frame_time, .tl_len = fk_tl_len, .tl_vsync_ns = fk_tl_vsync, .tl_deadline_ns = fk_tl_deadline,
    };
    return &o;
}

int main(void) {
    pthread_mutex_init(&fk.mu, NULL); pthread_cond_init(&fk.cv, NULL);
    atomic_store(&g_last_target, (int64_t)t_now() + 1000000000);
    char why[96] = "";

    // 0. 不可用回退：ops 探测结果进程内黏住，必须在首次解析前的干净子进程里测
    pid_t p = fork();
    if (p == 0) {
        g_use_fake = 0;                                            // 替身缺席 → 走真实 dlsym（宿主机上没有 libandroid.so）
        char w[96] = "";
        _exit(nvn_vsync_start(0, usr_fn, NULL, w, sizeof w) == -1 && w[0] ? 0 : 1);
    }
    int cst = 0; waitpid(p, &cst, 0);
    T(WIFEXITED(cst) && WEXITSTATUS(cst) == 0);

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

    // 5. 契约层接管：nvn_vsync_start 换用户回调——tick_n 从 1 起、target_at=时间线
    //    deadline（目标呈现时刻）；link 的槽位/拍数记账不受节拍源持有者影响
    T(nvn_vsync_start(0, usr_fn, NULL, why, sizeof why) == 0);
    fire_slots(1); wait_usr_n(1, 300);
    T(usr.calls == 1 && usr.last_n == 1);
    T(usr.last_at == (double)atomic_load(&g_last_target) / 1e9);
    PLAN({1, 0});
    T(s2a_android_link_wait_tick(1, 300000000ull) == 1);
    wait_usr_n(2, 300); T(usr.calls == 2);

    // 6. 后台/停局：stop 返回后回调不再派发；wait_tick 懒重启恢复
    nvn_vsync_stop();
    int calls = usr.calls;
    fire_at(atomic_load(&g_last_target) + 20000000); t_sleep_ms(20);
    T(usr.calls == calls);                                           // 停后不再调 fn
    PLAN({1, 0});
    T(s2a_android_link_wait_tick(0, 500000000ull) == 1);            // 懒重启：拍又来了
    t_sleep_ms(30);

    // 7. 重复 start 先停旧的：tick_n 重新从 1 起
    usr.n = 0; usr.calls = 0;
    T(nvn_vsync_start(0, usr_fn, NULL, why, sizeof why) == 0);
    fire_slots(1); wait_usr_n(1, 300);
    T(usr.last_n == 1);
    nvn_vsync_stop();

    printf("nvn_vsync android: %s (%d checks, %d fail)\n", t_fail ? "FAILED" : "passed", t_got + t_fail, t_fail);
    return t_fail ? 1 : 0;
}
