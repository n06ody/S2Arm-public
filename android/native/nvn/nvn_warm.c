// 预热调度：见 nvn_warm.h 的合同。一个后台线程按「先程序、后管线」的顺序把清单喂完。
#include "nvn_warm.h"
#include "nvn_vk_prog_warm.h"
#include "nvn_vk_pipe_warm.h"
#include "nvn_plat.h"
#if defined(__ANDROID__) && defined(S2A_ANDROID_VK_BACKEND_NVK)
#include "../capacity_affinity.h"
#endif

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define WARM_RETRY 8            // 在途重试槽（程序等编译、管线等程序）
#define WARM_TICK_MS 5          // 没活可做时一轮的睡眠粒度
#define WARM_LOG_MS 5000        // 进度日志间隔
#define WARM_PROG_RETRY_MS 20000   // 程序：编译在途最多等这么久（iOS 的编译服务同样可能十秒级）
#define WARM_PIPE_RETRY_MS 5000    // 管线：等程序发布，正常只差毫秒级
#define WARM_SAVE_MS 30000          // 定期落盘：进程被强杀时最后一次 Stop 不会来，清单不能只靠收尾写
#define WARM_FIRST_SAVE_MS 10000    // 第一次定期落盘（本局前面的记录先落地，别都压在 30 秒之后）
#define WARM_IDLE_MS 250            // 清单做完之后线程留在原地等 Stop 的睡眠粒度（它还要负责定期落盘）
// Stop 的统一截止（nvn_warm.h 合同：交互调用方只等 ≤500 毫秒；会话硬界限 700 毫秒，见 nvk_session.h）。
// 三段各自有上限，加起来不越过统一截止：① 等线程走到退出点 ② 后台构建静默 ③ 等清单落盘完成。
#define WARM_STOP_BUDGET_MS 500
#define WARM_STOP_JOIN_MS 150
#define WARM_STOP_QUIESCE_MS 100

enum { W_IDLE = 0, W_RUNNING = 1, W_STOPPING = 2 };
enum { WHY_DONE = 0, WHY_STOP = 1, WHY_OFF = 2, WHY_NONE = 3 };


// 线程名：Linux / 安卓是 (线程, 名字)，苹果只能给自己起名（宿主单测在 macOS 上编这个文件）
static void warm_thread_name(const char *name) {
#ifdef __APPLE__
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_exit = PTHREAD_COND_INITIALIZER;   // 预热线程退出时广播（收尾用有界等待，不依赖 pthread_timedjoin_np：bionic 要 _GNU_SOURCE）
static pthread_t g_th;
static int g_state = W_IDLE;      // 受 g_mu
static int g_live;                // 线程还在跑（受 g_mu）
static int g_on = -1;
static int64_t g_t0;
static _Atomic uint64_t g_prog_built, g_prog_failed, g_pipe_built, g_pipe_failed;
static _Atomic int g_why = WHY_DONE;
static _Atomic int g_retired;     // Stop 之后不再起线程
// 停局路径的状态（语义见 nvk_warm_stop）：
//   g_wake   请停唤醒：预热线程的每一段睡眠都可被打断（留驻循环 250 毫秒的粒度会直接吃掉 Stop 预算）；
//   g_save_mu 落盘归属：周期落盘与停局落盘都经 warm_persist，同一把锁串行（同一 owner 的两次 save 不交错）；
//   g_saved  本次停局的清单已落盘。Stop 线程**只等这个标志**，自己绝不做文件 IO。
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_save_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_saved;
static int g_thread_joinable;     // g_th 可以 join（detach 之后为 0）；受 g_mu
static int g_stop_requested;      // 已发布过停局请求：落盘执行者只选一次；受 g_mu
static int g_stop_finished;       // 已判定「静默 + 落盘完成」：重复 Stop 直接返回 0；受 g_mu
static _Atomic unsigned g_detached;   // 预算内没退出 → detach 交还的次数（读数）
// 上一次落盘时的记录数：只有「当前这一任落盘者」读写（同一时刻只有一任）
static uint64_t g_last_saved_rec, g_last_saved_pipe;

struct prog_retry { struct nvk_prog_rec rec; int64_t first_ms; uint8_t used; };
struct pipe_retry { struct nvk_pipe_rec rec; int64_t first_ms; uint8_t used; };
static struct prog_retry g_pretry[WARM_RETRY];   // 只有预热线程碰
static struct pipe_retry g_qretry[WARM_RETRY];

static int64_t warm_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int warm_on(void) {
    if (g_on < 0) g_on = nvn_plat_flag("nvk_warm", 1);
    return g_on;
}

static int warm_stopping(void) {
    pthread_mutex_lock(&g_mu);
    int s = g_state == W_STOPPING;
    pthread_mutex_unlock(&g_mu);
    return s;
}

// 在 g_mu 上按毫秒限时等 g_exit（调用方持 g_mu）。返回 1 = 到点。
// 不依赖 pthread_timedjoin_np：bionic 要 _GNU_SOURCE，宿主与设备两边都要能编。
static int warm_wait_flag_locked(uint32_t ms) {
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += ms / 1000;
    dl.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(&g_exit, &g_mu, &dl) == ETIMEDOUT;
}

// 预热线程的睡眠一律走这里：Stop 一到就立刻醒（否则要等满一个睡眠粒度，那段延迟会直接吃掉
// Stop 的 500 毫秒统一截止）。返回 1 = 已请停，调用方应尽快走到退出点。线程：[预热线程]。
static int warm_sleep_or_stop(uint32_t ms) {
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += ms / 1000;
    dl.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g_mu);
    while (g_state != W_STOPPING)
        if (pthread_cond_timedwait(&g_wake, &g_mu, &dl) == ETIMEDOUT) break;
    int stop = g_state == W_STOPPING;
    pthread_mutex_unlock(&g_mu);
    return stop;
}

static void warm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void warm_log(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "warm", b);
}

// 验收读数：真实 draw 有没有取到预建对象（draw_hits）、有没有「预热过却仍自备」（draw_miss）。
static void warm_progress(const char *tag) {
    struct nvk_prog_warm_stats ps;
    struct nvk_pipe_warm_stats qs;
    memset(&ps, 0, sizeof ps);
    memset(&qs, 0, sizeof qs);
    nvk_prog_warm_stats(&ps);
    nvk_pipe_warm_stats(&qs);
    warm_log("nvk warm %s: 程序 载入 %llu / 记录 %llu / 建 %llu / 失败 %llu / 本局 draw 命中 %llu 自备 %llu（剩 %llu）；"
             "管线 载入 %llu / 记录 %llu / 建 %llu / 失败 %llu / draw 命中 %llu / 标签命中 %llu（剩 %llu）；%lld 毫秒",
             tag,
             (unsigned long long)ps.loaded, (unsigned long long)ps.recorded, (unsigned long long)ps.built,
             (unsigned long long)ps.failed, (unsigned long long)ps.draw_hits, (unsigned long long)ps.draw_miss,
             (unsigned long long)ps.pending,
             (unsigned long long)qs.loaded, (unsigned long long)qs.recorded, (unsigned long long)qs.built,
             (unsigned long long)qs.failed, (unsigned long long)qs.draw_hits, (unsigned long long)qs.tag_hits, (unsigned long long)qs.pending,
             (long long)(g_t0 ? warm_now_ms() - g_t0 : 0));
}

// 唯一的落盘入口：周期落盘与停局落盘都走这里、同一把锁（同一 owner 的两次 save 不交错写同一个临时文件）。
// 线程：[预热线程] 或 [一次性 saver]，**绝不在 Stop 调用线程上**——写文件 / fsync / rename / 目录 fsync
// 都没有截止参数，放在交互线程上就是复核里 900 毫秒的 Stop（会话 700 毫秒硬界限会因此被顶穿）。
static void warm_persist(int force_log, const char *tag) {
    pthread_mutex_lock(&g_save_mu);
    nvk_prog_warm_save();
    nvk_pipe_warm_save();
    pthread_mutex_unlock(&g_save_mu);
    struct nvk_prog_warm_stats ps;
    struct nvk_pipe_warm_stats qs;
    memset(&ps, 0, sizeof ps);
    memset(&qs, 0, sizeof qs);
    nvk_prog_warm_stats(&ps);
    nvk_pipe_warm_stats(&qs);
    // 周期落盘内容没变就不打（真机上不刷屏）；停局落盘一律打一行（验收读数）。
    if (force_log || ps.recorded != g_last_saved_rec || qs.recorded != g_last_saved_pipe) {
        g_last_saved_rec = ps.recorded;
        g_last_saved_pipe = qs.recorded;
        warm_log("nvk warm %s落盘：程序记录 %llu 条、管线记录 %llu 条", tag,
                 (unsigned long long)ps.recorded, (unsigned long long)qs.recorded);
    }
}

// 停局落盘：由落盘者（预热线程退出前 / 一次性 saver）调用；完成后置标志并唤醒 Stop 的等待者。
// Stop 侧只等 g_saved，不做任何文件动作。
static void warm_persist_stop(void) {
    warm_persist(1, "停局");
    atomic_store(&g_saved, 1);
    pthread_mutex_lock(&g_mu);
    pthread_cond_broadcast(&g_exit);
    pthread_mutex_unlock(&g_mu);
}

static int pretry_push(const struct nvk_prog_rec *r) {
    for (int i = 0; i < WARM_RETRY; i++)
        if (!g_pretry[i].used) { g_pretry[i].rec = *r; g_pretry[i].used = 1; g_pretry[i].first_ms = warm_now_ms(); return 0; }
    return -1;
}
static int qretry_push(const struct nvk_pipe_rec *r) {
    for (int i = 0; i < WARM_RETRY; i++)
        if (!g_qretry[i].used) { g_qretry[i].rec = *r; g_qretry[i].used = 1; g_qretry[i].first_ms = warm_now_ms(); return 0; }
    return -1;
}

// 清单加载：本局 profile / 编译闭包可能还没冻结（provider activate 的顺序），最多重试约 10 秒。
// 之后仍拿不到按「本局没有清单」处理：拿不到上下文不是清单坏了，不拿它当失败重试到底。
static int warm_load(void) {
    for (int i = 0; i < 100; i++) {
        if (warm_stopping()) return -1;
        int np = nvk_prog_warm_load();
        int nq = nvk_pipe_warm_load();
        if (np >= 0 || nq >= 0) {
            warm_log("nvk warm 清单：程序 %d 条、管线 %d 条", np, nq);
            return np + nq;
        }
        (void)warm_sleep_or_stop(100);
    }
    return -2;
}

static void warm_leave(void) {
    pthread_mutex_lock(&g_mu);
    g_live = 0;
    g_state = W_IDLE;
    pthread_cond_broadcast(&g_exit);
    pthread_mutex_unlock(&g_mu);
}

// 没有预热线程替我们落盘时（开关关闭 / 线程起不来 / 本局从未 start）的一次性落盘者。
// detached：Stop 只按预算等 g_saved，到点就交还，不 join 它；它退出前一定把清单写完。
static void *warm_saver_main(void *unused) {
    (void)unused;
    warm_thread_name("nvk-warm-save");
#if defined(__ANDROID__) && defined(S2A_ANDROID_VK_BACKEND_NVK)
    s2a_capacity_enter(S2A_CAPACITY_BACKGROUND);
#endif
    warm_persist_stop();
    return NULL;
}

// 预热线程唯一的退出路径：停局。先落盘、置完成标志、再报告退出——清单落盘归本线程，
// Stop 线程只等标志（见 nvk_warm_stop）。「清单跑完」不清退（留驻做定期落盘，见 warm_main 的
// drained 分支），所以这里只由 Stop 触发。线程：[预热线程]。
static void warm_exit_stop(void) {
    atomic_store(&g_why, WHY_STOP);
    warm_progress("收工（Stop）");
    warm_persist_stop();
    warm_leave();
}

static void *warm_main(void *unused) {
    (void)unused;
    {   // 让出游戏线程：预热只是把关键路径上本来就要做的活提前，不跟游戏抢核（iOS 后台预热同样降 QoS）
        setpriority(PRIO_PROCESS, 0, 5);
        warm_thread_name("nvk-warm");
    }
#if defined(__ANDROID__) && defined(S2A_ANDROID_VK_BACKEND_NVK)
    s2a_capacity_enter(S2A_CAPACITY_BACKGROUND);
#endif
    g_t0 = warm_now_ms();
    int loaded = warm_load();
    if (loaded == -1) {   // 载入重试期间就收到 Stop：照常落盘退出（清单落盘归本线程）
        warm_log("nvk warm：载入清单时收到 Stop，清单没载入");
        warm_exit_stop();
        return NULL;
    }
    if (loaded < 0) {
        // 拿不到清单不是清单坏了，也不清退：本局真实 draw 还在记录新身份，定期落盘要靠这条线程，
        // Stop 时的清单落盘也要靠它（没有线程替它，那一步就只能回到 Stop 调用线程上做）。
        atomic_store(&g_why, WHY_NONE);
        warm_log("nvk warm：本局没有可用清单（%d），不预热，只做定期落盘", loaded);
    }
    struct nvk_prog_rec pr;
    struct nvk_pipe_rec qr;
    // 本局预热集在开局就冻结：只做「上一局落盘、这一局载入」的那批。本局新记录的身份会继续进清单
    // （下一局才用），但不再扩预热集——否则游戏每帧都在产生新身份，预热线程会跟着游戏无限跑，
    // 跟它抢核（真机第二次启动：预热到 105 秒还在跑，帧率 22.6 → 18.0）。
    int prog_budget = loaded < 0 ? 0 : nvk_prog_warm_pending();
    int pipe_budget = loaded < 0 ? 0 : nvk_pipe_warm_pending();
    int prog_done = loaded < 0 ? 1 : 0, idle_rounds = 0, drained = 0;
    int64_t last_log = warm_now_ms(), next_save = g_t0 + WARM_FIRST_SAVE_MS;
    for (;;) {
        if (warm_stopping()) { warm_exit_stop(); return NULL; }
        int did = 0;
        // 清单做完了不是「线程退休」：本局还在不断记录新的身份/配方，定期落盘要靠这条线程
        // （bench / 玩家直接杀进程时不会有 Stop，只有这里的周期性 save 能把记录留下来）。
        if (drained) goto idle;
        // 第一遍：程序清单按记录顺序做（热点在前由清单自己保证）。
        if (!prog_done) {
            if (prog_budget <= 0) prog_done = 1;   // 本局预热集做完（本局新记录不再扩集）
            else if (nvk_prog_warm_take(&pr)) {
                prog_budget--;
                int rc = nvk_prog_warm_build(&pr);
                did = 1;
                if (rc == 0) atomic_fetch_add(&g_prog_built, 1);
                else if (rc == 1) { if (pretry_push(&pr)) atomic_fetch_add(&g_prog_failed, 1); }
                else atomic_fetch_add(&g_prog_failed, 1);
            } else {
                prog_done = 1;
            }
        }
        // 第二遍：等编译在途的程序（同身份，编译服务里正在做）。
        if (!did && prog_done) {
            for (int i = 0; i < WARM_RETRY; i++) {
                if (!g_pretry[i].used) continue;
                int rc = nvk_prog_warm_build(&g_pretry[i].rec);
                if (rc == 0) { g_pretry[i].used = 0; atomic_fetch_add(&g_prog_built, 1); }
                else if (rc < 0) { g_pretry[i].used = 0; atomic_fetch_add(&g_prog_failed, 1); }
                else if (warm_now_ms() - g_pretry[i].first_ms > WARM_PROG_RETRY_MS) { g_pretry[i].used = 0; atomic_fetch_add(&g_prog_failed, 1); }
                did = 1;
                break;
            }
        }
        // 第三遍：管线（要程序先发布；程序清单全部走完之后才开始，顺序由这份循环保证）。
        if (!did && prog_done) {
            if (pipe_budget > 0 && nvk_pipe_warm_take(&qr)) {
                pipe_budget--;
                int rc = nvk_pipe_warm_build(&qr);
                did = 1;
                if (rc == 0) atomic_fetch_add(&g_pipe_built, 1);
                else if (rc == 1) { if (qretry_push(&qr)) atomic_fetch_add(&g_pipe_failed, 1); }
                else atomic_fetch_add(&g_pipe_failed, 1);
            }
        }
        if (!did && prog_done) {
            for (int i = 0; i < WARM_RETRY; i++) {
                if (!g_qretry[i].used) continue;
                int rc = nvk_pipe_warm_build(&g_qretry[i].rec);
                if (rc == 0) { g_qretry[i].used = 0; atomic_fetch_add(&g_pipe_built, 1); }
                else if (rc < 0) { g_qretry[i].used = 0; atomic_fetch_add(&g_pipe_failed, 1); }
                else if (warm_now_ms() - g_qretry[i].first_ms > WARM_PIPE_RETRY_MS) { g_qretry[i].used = 0; atomic_fetch_add(&g_pipe_failed, 1); }
                did = 1;
                break;
            }
        }
        if (!did) {
            int more = !prog_done || prog_budget > 0 || pipe_budget > 0;
            int retry_left = 0;
            for (int i = 0; i < WARM_RETRY; i++) retry_left += g_pretry[i].used + g_qretry[i].used;
            if (!more && !retry_left) { drained = 1; atomic_store(&g_why, WHY_DONE); warm_progress("清单完成"); }
            else if (!more && retry_left && ++idle_rounds > 400) { drained = 1; atomic_store(&g_why, WHY_DONE); warm_progress("清单完成（在途重试到期）"); }
            if (more) idle_rounds = 0;
            if (!drained) (void)warm_sleep_or_stop(WARM_TICK_MS);
        } else {
            idle_rounds = 0;
        }
        {
            int64_t now = warm_now_ms();
            if (!drained && now - last_log >= WARM_LOG_MS) { last_log = now; warm_progress("进度"); }
            // 定期落盘：玩家直接杀进程时没有 Stop，本局记录靠这一步落盘（文件有界，原子写；
            // 内容没变时两个 owner 自己会跳过写盘）。
            if (now >= next_save) {
                next_save = now + WARM_SAVE_MS;
                warm_persist(0, "定期");
            }
        }
        continue;
idle:
        if (!did) {
            (void)warm_sleep_or_stop(WARM_IDLE_MS);
            int64_t now = warm_now_ms();
            if (now >= next_save) {
                next_save = now + WARM_SAVE_MS;
                warm_persist(0, "定期");
            }
        }
    }   // for(;;)：只从这里的上面的 Stop 分支退出（warm_exit_stop），不会走到循环外
}

void nvk_warm_start(void) {
    if (!warm_on()) { atomic_store(&g_why, WHY_OFF); return; }
    pthread_mutex_lock(&g_mu);
    if (g_state != W_IDLE || g_live || atomic_load(&g_retired)) { pthread_mutex_unlock(&g_mu); return; }
    memset(g_pretry, 0, sizeof g_pretry);
    memset(g_qretry, 0, sizeof g_qretry);
    atomic_store(&g_why, WHY_DONE);
    g_state = W_RUNNING;
    g_live = 1;
    int rc = pthread_create(&g_th, NULL, warm_main, NULL);
    if (rc) {
        g_live = 0;
        g_state = W_IDLE;
        pthread_mutex_unlock(&g_mu);
        nvn_plat_log(NVN_PLAT_LOG_RED, "warm", "nvk warm：预热线程起不来（本局不预热，首次使用照旧同步准备）");
        return;
    }
    g_thread_joinable = 1;   // Stop 成功路径要 join；超时路径改成 detach（见 nvk_warm_stop）
    pthread_mutex_unlock(&g_mu);
    warm_log("nvk warm：预热线程已起（debug.s2a.nvk_warm=1；设 0 关）");
}

// 停局。**有界且绝不做文件 IO**（nvn_warm.h 合同）：交互调用方只等「静默 + 清单落盘完成」，
// 统一截止 WARM_STOP_BUDGET_MS；落盘归预热线程（它退出前做）或一次性 saver，本线程只等标志。
//
// 生命周期/线程：由会话 Stop 的调用线程（交互路径）调用，可能被重复调用（幂等）：
//   0 = 已静默且清单落盘完成（重复调用直接 0）；1 = 预算内没完成（后台继续收尾，旧文件保留）。
// 到期还没退的线程一律 detach 交还：不留僵尸、不无界 join；迟到结果由各 owner 的停局判定丢弃。
int nvk_warm_stop(void) {
    const int64_t t0 = warm_now_ms();
    const int64_t deadline = t0 + WARM_STOP_BUDGET_MS;
    pthread_mutex_lock(&g_mu);
    if (g_stop_finished) { pthread_mutex_unlock(&g_mu); return 0; }   // 幂等：已经收干净
    const int first = !g_stop_requested;
    g_stop_requested = 1;
    if (g_state == W_RUNNING) g_state = W_STOPPING;
    const int live = g_live;
    const pthread_t th = g_th;
    if (first) atomic_store(&g_saved, 0);   // 本次停局的落盘完成标志只置一次
    pthread_cond_broadcast(&g_wake);        // 唤醒留驻循环（250 毫秒的睡眠粒度会吃掉预算）
    // 没有任何预热线程可替我们落盘（开关关闭 / 线程起不来 / 本局从未 start）：起一次性 saver。
    const int spawn_saver = first && !live;
    pthread_mutex_unlock(&g_mu);

    int saver_ok = 1;
    if (spawn_saver) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t st;
        int cr = pthread_create(&st, &attr, warm_saver_main, NULL);
        pthread_attr_destroy(&attr);
        if (cr) {
            saver_ok = 0;
            // 预算优先：落盘者起不来也不在本线程做文件 IO（那就是复核的 900 毫秒），如实报红、不假装落盘。
            nvn_plat_log(NVN_PLAT_LOG_RED, "warm", "nvk warm：停局落盘线程起不来（本次 Stop 不落盘，旧文件保留）");
        }
    }
    // ① 等预热线程走到退出点（清单落盘在它的退出路径里）：最多 WARM_STOP_JOIN_MS。
    int exited = !live;
    if (live) {
        const int64_t lim = t0 + WARM_STOP_JOIN_MS;
        pthread_mutex_lock(&g_mu);
        while (g_live) {
            int64_t left = lim - warm_now_ms();
            if (left <= 0) break;
            if (warm_wait_flag_locked((uint32_t)left)) break;
        }
        exited = !g_live;
        pthread_mutex_unlock(&g_mu);
    }
    // ② 后台构建在途收拢（有界）：线程已退出时只剩别的后台路径，通常立刻返回。
    const int quiet = nvk_shader_warm_quiesce(WARM_STOP_QUIESCE_MS) == 0;
    // ③ 等清单落盘完成：只等标志、不碰文件；慢盘在这里被统一截止裁掉（旧文件保留、后台继续写）。
    pthread_mutex_lock(&g_mu);
    while (!atomic_load(&g_saved)) {
        int64_t left = deadline - warm_now_ms();
        if (left <= 0) break;
        if (warm_wait_flag_locked((uint32_t)left)) break;
    }
    const int saved = atomic_load(&g_saved);
    pthread_mutex_unlock(&g_mu);
    const int ok = saved && quiet && saver_ok;
    // ④ 线程处置：退出了就 join，没退出就 detach 交还（两种都不留僵尸、都不无界等）。
    pthread_mutex_lock(&g_mu);
    int join_now = 0, detach_now = 0;
    if (g_thread_joinable) {
        g_thread_joinable = 0;
        if (g_live) detach_now = 1; else join_now = 1;
    }
    if (ok) g_stop_finished = 1;
    pthread_mutex_unlock(&g_mu);
    if (detach_now) {
        (void)pthread_detach(th);                   // 交还：线程退出时自己回收资源，不等它（它退出前仍会落盘）
        atomic_fetch_add(&g_detached, 1);
    } else if (join_now) {
        (void)pthread_join(th, NULL);               // g_live==0：线程已走到退出路径末尾，join 不阻塞
    }
    atomic_store(&g_retired, 1);   // Stop 之后不再起线程（与改前一致：本局不再预热）
    warm_log("nvk warm 停局：耗时 %lld 毫秒、线程退出 %s、清单落盘 %s、后台静默 %s、detach 累计 %u",
             (long long)(warm_now_ms() - t0), exited ? "是" : "否", saved ? "完成" : "未完成",
             quiet ? "完成" : "未完成", atomic_load(&g_detached));
    if (!ok)
        nvn_plat_log(NVN_PLAT_LOG_RED, "warm",
                     "【红】nvk warm 停局：没在 500 毫秒统一截止内完成（清单没落盘、旧文件保留、后台线程继续收尾；"
                     "这一段占用会话 700 毫秒硬界限）");
    warm_progress("收尾");
    return ok ? 0 : 1;
}


int nvk_warm_active(void) {
    pthread_mutex_lock(&g_mu);
    int a = g_live;
    pthread_mutex_unlock(&g_mu);
    return a;
}

void nvk_warm_totals(struct nvk_warm_totals *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->prog_built = atomic_load(&g_prog_built);
    out->prog_failed = atomic_load(&g_prog_failed);
    out->pipe_built = atomic_load(&g_pipe_built);
    out->pipe_failed = atomic_load(&g_pipe_failed);
    out->ms = g_t0 ? (uint64_t)(warm_now_ms() - g_t0) : 0;
    out->why = atomic_load(&g_why);
}
