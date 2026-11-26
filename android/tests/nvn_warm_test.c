// nvn_warm.c（预热调度）的宿主回归：用可控的假 owner 证明时序合同，不碰 GPU、不碰真清单。
// 覆盖：
//   1) 顺序：程序清单全部走完（含在途重试）之后才开始管线清单；
//   2) 在途重试：build 返回 1 的条目会被重试，最终成功计入 built，不丢；
//   3) 计数：失败条目计入 failed，不阻塞其它条目；
//   4) Stop（快落盘）：返回 0、任务线程退出、两个 owner 的清单都落盘、后台静默被调用、耗时远小于统一截止；
//   5) Stop（慢落盘 3 秒）：**返回 1、记「未完成」、耗时远小于这次落盘**——落盘归预热线程（或一次性 saver），
//      Stop 调用线程绝不做文件 IO；到点后后台继续把清单写完（旧文件在此之前保留）；
//   6) 没有预热线程时（开关关闭 / 线程起不来 / 本局从未 start）由一次性 saver 落盘，Stop 同样有界；
//   7) 开关：debug.s2a.nvk_warm=0 时不起线程。
// 各场景各跑一个进程：平台开关在进程内不变（nvn_plat.h 契约），且 Stop 之后本模块不再起线程（g_retired）。
//   NVK_WARM_OFF=1                     → 场景 7（+ 6 的快落盘）
//   NVK_WARM_SLOW_SAVE_US=<微秒>       → 场景 5
//   NVK_WARM_OFF=1 NVK_WARM_SLOW_SAVE_US=<微秒> → 场景 6（没有预热线程 + 慢落盘）
// 合同参考 graphics/nvn/gfx/nvn/nvn_mtl.m 的 warm_start（后台清单预热、不挡首帧）与 nvn_warm.h 的 Stop 合同。
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../android/native/nvn/nvn_warm.h"
#include "../../android/native/nvn/nvn_vk_prog_warm.h"
#include "../../android/native/nvn/nvn_vk_pipe_warm.h"

// ---- 假 owner（被 nvn_warm.c 调用的两个模块）----
// debug.s2a.nvk_warm 的假替身。平台开关在进程内不变（nvn_plat.h 契约），所以「关」与「开」用两次进程跑：
// 本程序第二次带 NVK_WARM_OFF=1 起，断言起不来线程。
static int g_flag = 1;
static int g_prefetch_flag;
static atomic_int g_prefetch_count, g_prefetch_release;
static uint8_t g_prefetch_order[4];
static int g_prog_total = 6, g_pipe_total = 4;
static int g_prog_taken, g_pipe_taken;
static atomic_int g_order_violation;    // 管线先于程序被取走 = 1
static atomic_int g_retry_prog = 2;     // 前 2 次调用返回 1（在途），之后成功
static atomic_int g_prog_built, g_prog_failed, g_pipe_built, g_pipe_failed;
static atomic_int g_saved, g_quiesced;

// 落盘慢桩：NVK_WARM_SLOW_SAVE_US 设置程序清单的假 IO 时长（复核用 900 毫秒：远大于 Stop 的 500 毫秒截止）。
static atomic_int g_save_delay_us;
// 慢落盘场景的假 IO 时长：取 3 秒，远大于 Stop 的 500 毫秒截止。判「Stop 有没有等落盘」看 Stop 的决定（返回 1、记「未完成」）
//   与耗时是否远小于这次落盘，不卡墙钟毫秒：门禁机负载高时调度抖动不是我方输出（11-18 一次 964 毫秒误红，平时 500–510）。
#define SLOW_SAVE_US 3000000
// Stop 的读数行与红行：落盘归属改到后台之后，「Stop 有没有超时」只能从这一行看出来，所以断言它。
static char g_stop_line[512];
static atomic_int g_red_seen;
static atomic_int g_creation_feedback_reported;

enum nvk_pipe_feedback_mode nvn_vk_pipe_creation_stats(struct nvk_pipe_creation_stats out[NVK_PIPE_CREATION_BUCKETS]) {
    memset(out, 0, sizeof(*out) * NVK_PIPE_CREATION_BUCKETS);
    out[NVK_PIPE_GFX_WARM] = (struct nvk_pipe_creation_stats){.created = 2, .valid = 1, .no_feedback = 1,
                                                          .app_hit = 1, .driver_ns = 100, .driver_p95_le_ns = 103,
                                                          .wall_ns = 200};
    return NVK_PIPE_FEEDBACK_ON;
}

int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_warm")) return g_flag;
    if (!strcmp(name, "nvk_shader_prefetch")) return g_prefetch_flag;
    return dflt;
}
int nvk_prog_prefetch_build(const uint8_t (*keys)[8], int n, uint64_t session, uint64_t generation) {
    assert(g_prefetch_flag && session == 4 && generation == 6 && n == 1);
    const int index = atomic_load(&g_prefetch_count);
    assert(index < 4);
    g_prefetch_order[index] = keys[0][0];
    atomic_store(&g_prefetch_count, index + 1);
    if (!index) {
        for (int i = 0; i < 2000 && !atomic_load(&g_prefetch_release); i++) usleep(1000);
        assert(atomic_load(&g_prefetch_release));
    }
    return 1;
}
void nvn_plat_log(int level, const char *cls, const char *text) {
    (void)cls;
    if (level == 2) atomic_store(&g_red_seen, 1);   // NVN_PLAT_LOG_RED = 2（graphics/nvn/replay/nvn_plat.h）
    if (text && strstr(text, "nvk warm 收尾:") &&
        strstr(text, "PSO feedback=on") && strstr(text, "gfx_warm=2/1/1/1/100/103/200") &&
        strstr(text, "compute_draw=0/0/0/0/0/0/0"))
        atomic_store(&g_creation_feedback_reported, 1);
    if (text && strstr(text, "停局：耗时")) snprintf(g_stop_line, sizeof g_stop_line, "%s", text);
}
static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int nvk_prog_warm_load(void) { return g_prog_total; }
int nvk_prog_warm_pending(void) { return g_prog_total - g_prog_taken; }
int nvk_prog_warm_take(struct nvk_prog_rec *out) {
    if (g_prog_taken >= g_prog_total) return 0;
    memset(out, 0, sizeof *out);
    out->identity[0] = (uint8_t)g_prog_taken++;
    out->stage_mask = 3;
    return 1;
}
static atomic_int g_prog_attempts[64];
int nvk_prog_warm_build(const struct nvk_prog_rec *rec) {
    int id = rec->identity[0];
    int n = atomic_fetch_add(&g_prog_attempts[id], 1);
    if (id == 3) return -1;                                  // 一条固定失败
    if (n < atomic_load(&g_retry_prog)) return 1;            // 前几条在途：应被重试
    atomic_fetch_add(&g_prog_built, 1);
    return 0;
}
void nvk_prog_warm_note(const struct nvk_prog_rec *rec) { (void)rec; }
void nvk_prog_warm_save(void) {
    int d = atomic_load(&g_save_delay_us);
    if (d > 0) usleep((useconds_t)d);
    atomic_fetch_add(&g_saved, 1);
}
int nvk_shader_warm_quiesce(uint32_t budget_ms) { (void)budget_ms; atomic_fetch_add(&g_quiesced, 1); return 0; }
void nvk_prog_warm_stats(struct nvk_prog_warm_stats *o) {
    if (!o) return;
    memset(o, 0, sizeof *o);
    o->loaded = (uint64_t)g_prog_total;
    o->built = (uint64_t)atomic_load(&g_prog_built);
    o->failed = (uint64_t)atomic_load(&g_prog_failed);
    o->pending = (uint64_t)(g_prog_total - g_prog_taken);
}

int nvk_pipe_warm_load(void) { return g_pipe_total; }
int nvk_pipe_warm_pending(void) { return g_pipe_total - g_pipe_taken; }
int nvk_pipe_warm_take(struct nvk_pipe_rec *out) {
    if (g_pipe_taken >= g_pipe_total) return 0;
    if (g_prog_taken < g_prog_total) atomic_store(&g_order_violation, 1);   // 程序没走完就取管线 = 违约
    memset(out, 0, sizeof *out);
    out->prog_identity[0] = (uint8_t)g_pipe_taken++;
    return 1;
}
int nvk_pipe_warm_build(const struct nvk_pipe_rec *rec) {
    if (rec->prog_identity[0] == 1) return -1;               // 一条固定失败
    atomic_fetch_add(&g_pipe_built, 1);
    return 0;
}
void nvk_pipe_warm_note(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32]) { (void)d; (void)prog_identity; }
void nvk_pipe_warm_save(void) { atomic_fetch_add(&g_saved, 1); }
void nvk_pipe_warm_stats(struct nvk_pipe_warm_stats *o) {
    if (!o) return;
    memset(o, 0, sizeof *o);
    o->loaded = (uint64_t)g_pipe_total;
    o->built = (uint64_t)atomic_load(&g_pipe_built);
    o->pending = (uint64_t)(g_pipe_total - g_pipe_taken);
}

// ---- 用例 ----
static void wait_idle(int ms) {
    for (int i = 0; i < ms; i++) { if (!nvk_warm_active()) return; usleep(1000); }
}

// 关掉开关：不起线程（进程内开关不变，只在本进程第一次 start 时读，所以单独一个进程跑）。
// 没有预热线程时清单落盘归一次性 saver：快落盘必须返回 0（预算内写完）。
static void t_disable(int delay_us) {
    atomic_store(&g_save_delay_us, delay_us);
    nvk_warm_start();
    assert(!nvk_warm_active());
    int64_t t0 = now_ms();
    int rc = nvk_warm_stop();
    int64_t dt = now_ms() - t0;
    printf("无预热线程（快落盘）：Stop 耗时 %lld 毫秒、返回 %d\n", (long long)dt, rc);
    assert(rc == 0);        // saver 在预算内把清单写完
    assert(dt <= 500);
    assert(atomic_load(&g_saved) >= 1);
    assert(strstr(g_stop_line, "清单落盘 完成") != NULL);
    assert(atomic_load(&g_creation_feedback_reported));
    printf("t_disable ok（debug.s2a.nvk_warm=0 时不起线程，落盘归一次性 saver）\n");
}

// 没有预热线程 + 慢落盘：Stop 起一次性 saver，自己仍不做文件 IO（复核的 900 毫秒不进交互路径）。
static void t_stop_without_thread_slow(void) {
    atomic_store(&g_save_delay_us, SLOW_SAVE_US);
    nvk_warm_start();
    assert(!nvk_warm_active());
    int64_t t0 = now_ms();
    int rc = nvk_warm_stop();
    int64_t dt = now_ms() - t0;
    printf("无预热线程 + 慢落盘：Stop 耗时 %lld 毫秒、返回 %d\n", (long long)dt, rc);
    assert(rc == 1);        // 预算内没写完
    assert(dt < SLOW_SAVE_US / 1000);   // 没等这次落盘（等了就是整段 3 秒，见负对照）
    assert(atomic_load(&g_saved) == 0);
    assert(atomic_load(&g_red_seen) == 1);
    assert(strstr(g_stop_line, "清单落盘 未完成") != NULL);
    int waited = 0;
    while (waited < 6000 && atomic_load(&g_saved) == 0) { usleep(1000); waited++; }
    assert(atomic_load(&g_saved) >= 1);   // saver 在后台把它写完了（旧文件在此之前保留）
    printf("t_stop_without_thread_slow ok（saver 在 %d 毫秒内写完清单）\n", waited);
}

// 慢落盘（900 毫秒）+ 有预热线程：落盘归预热线程的退出路径，Stop 只等标志 → 有界返回 1。
// 这条用例就是复核 stop_budget_probe 的等价断言：修前 Stop 在调用线程上同步 save，实测 900–1050 毫秒。
static void t_stop_budget_slow(void) {
    atomic_store(&g_save_delay_us, SLOW_SAVE_US);
    nvk_warm_start();
    for (int i = 0; i < 2000 && !nvk_warm_active(); i++) usleep(1000);
    assert(nvk_warm_active());
    usleep(20000);   // 让载入/预热先跑起来，Stop 落在它工作期间
    int64_t t0 = now_ms();
    int rc = nvk_warm_stop();
    int64_t dt = now_ms() - t0;
    printf("慢落盘：Stop 耗时 %lld 毫秒、返回 %d\n", (long long)dt, rc);
    assert(rc == 1);        // 预算内没完成
    assert(dt < SLOW_SAVE_US / 1000);   // 没等这次落盘（等了就是整段 3 秒，见负对照）
    assert(atomic_load(&g_saved) == 0);   // 到点时 3 秒的写盘还没结束
    assert(atomic_load(&g_red_seen) == 1);
    assert(strstr(g_stop_line, "清单落盘 未完成") != NULL);
    // 到期 detach 交还，后台继续收尾：预热线程退出前把清单写完，之后 active 归 0。
    int waited = 0;
    while (waited < 6000 && (atomic_load(&g_saved) == 0 || nvk_warm_active())) { usleep(1000); waited++; }
    assert(atomic_load(&g_saved) >= 1);
    assert(!nvk_warm_active());
    printf("t_stop_budget_slow ok（后台在 %d 毫秒内写完清单并退出，旧文件在此之前保留）\n", waited);
}

// 本局预热集在开局冻结：预热进行中游戏继续产生新身份（g_*_total 增长）不得扩集，否则预热线程
// 会跟着游戏无限跑、跟它抢核（真机：第二次启动预热到 105 秒还在跑，帧率 22.6 → 18.0）。
static void t_order_and_counts(void) {
    g_flag = 1;
    g_prog_taken = g_pipe_taken = 0;
    const int prog0 = g_prog_total, pipe0 = g_pipe_total;
    atomic_store(&g_retry_prog, 2);
    atomic_store(&g_prog_built, 0); atomic_store(&g_prog_failed, 0);
    atomic_store(&g_pipe_built, 0); atomic_store(&g_pipe_failed, 0);
    memset((void *)g_prog_attempts, 0, sizeof g_prog_attempts);
    nvk_warm_start();
    // 预热线程开工后，模拟游戏持续产生新身份：预热集不得因此扩大。
    usleep(2000);
    g_prog_total = prog0 + 50;
    g_pipe_total = pipe0 + 50;
    for (int i = 0; i < 3000; i++) { if (g_prog_taken >= prog0 && g_pipe_taken >= pipe0) break; usleep(1000); }
    wait_idle(2000);
    assert(atomic_load(&g_order_violation) == 0);          // 程序先于管线
    g_prog_total = prog0;   // 断言用开局冻结的那批（之后追加的 50 条不该被取走）
    g_pipe_total = pipe0;
    assert(g_prog_taken == prog0);
    assert(g_pipe_taken == pipe0);
    assert(atomic_load(&g_prog_built) == g_prog_total - 1);  // 一条固定失败
    assert(atomic_load(&g_pipe_built) == g_pipe_total - 1);
    for (int i = 0; i < g_prog_total; i++)
        if (i != 3) assert(atomic_load(&g_prog_attempts[i]) >= 3);   // 在途重试确实发生过
    struct nvk_warm_totals tt;
    nvk_warm_totals(&tt);
    assert(tt.prog_built == (uint64_t)(g_prog_total - 1));
    assert(tt.pipe_built == (uint64_t)(g_pipe_total - 1));
    // 清单跑完不等于线程退休：它还要负责周期性把本局新记录落盘（玩家/bench 直接杀进程时没有 Stop）。
    // 修前这里会在清单排空后立刻退出（active=0），记录只能等 Stop——真机上就丢过整份清单。
    {
        int waited = 0;
        while (waited < 3000 && !nvk_warm_active()) { usleep(1000); waited++; }
        if (!nvk_warm_active()) { while (waited < 3000) { usleep(1000); waited++; } }
        assert(nvk_warm_active() == 1);   // 清单完成后线程仍在（等 Stop + 定期落盘）
    }
    // Stop 有界：清单早已跑完，stop 只等线程退出 + 静默 + 等落盘完成（落盘在预热线程上做），必须很快返回。
    int64_t t0 = now_ms();
    int rc = nvk_warm_stop();
    int64_t dt = now_ms() - t0;
    assert(rc == 0);                             // 已静默且清单落盘完成
    assert(dt <= 500);                           // 统一截止内（快落盘：正常个位数毫秒）
    assert(!nvk_warm_active());
    assert(atomic_load(&g_saved) >= 1);          // 两个 owner 的清单都落盘
    assert(atomic_load(&g_quiesced) >= 1);
    assert(atomic_load(&g_red_seen) == 0);       // 快路径不打红
    assert(strstr(g_stop_line, "清单落盘 完成") != NULL);
    assert(atomic_load(&g_creation_feedback_reported));
    assert(strstr(g_stop_line, "detach 累计") != NULL);   // 读数行里有 detach 计数
    // 幂等：收干净之后再调一次必须立刻返回 0（会话 request_stop 与 destroy 会各调一次）。
    t0 = now_ms();
    rc = nvk_warm_stop();
    dt = now_ms() - t0;
    assert(rc == 0);
    assert(dt <= 20);
    printf("t_order_and_counts ok (prog %llu pipe %llu why %d)\n",
           (unsigned long long)tt.prog_built, (unsigned long long)tt.pipe_built, tt.why);
}

static void queue_prefetch(uint8_t key, int dedup) {
    const uint8_t keys[1][8] = {{key}};
    struct nvk_warm_totals before, after;
    nvk_warm_totals(&before);
    for (int i = 0; i < 200; i++) {
        nvk_warm_prefetch_queue(keys, 1, 4, 6);
        nvk_warm_totals(&after);
        if (dedup ? after.prefetch_dedup > before.prefetch_dedup
                  : after.prefetch_queued > before.prefetch_queued) return;
        usleep(1000);
    }
    assert(!"prefetch queue did not accept work");
}

static void t_prefetch_lifo(void) {
    g_flag = 0;
    g_prefetch_flag = 1;
    nvk_warm_start();
    queue_prefetch(10, 0);
    for (int i = 0; i < 1000 && !atomic_load(&g_prefetch_count); i++) usleep(1000);
    assert(atomic_load(&g_prefetch_count) == 1);
    queue_prefetch(20, 0);
    queue_prefetch(30, 0);
    queue_prefetch(40, 0);
    queue_prefetch(20, 1);   // New registration promotes the same queued work.
    atomic_store(&g_prefetch_release, 1);
    for (int i = 0; i < 1000 && atomic_load(&g_prefetch_count) < 4; i++) usleep(1000);
    assert(atomic_load(&g_prefetch_count) == 4);
    assert(!nvk_warm_stop());
    assert(!memcmp(g_prefetch_order, (const uint8_t[]){10, 20, 40, 30}, sizeof g_prefetch_order));
    struct nvk_warm_totals totals;
    nvk_warm_totals(&totals);
    assert(totals.prefetch_ready == 4 && totals.prefetch_dedup == 1);
    assert(!totals.prog_built && !totals.pipe_built);
    printf("t_prefetch_lifo ok (order 10,20,40,30; dedup 1)\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   // 断言跳 abort 时不丢读数行（负对照要看到真实的 Stop 耗时）
    if (getenv("NVK_WARM_PREFETCH_TEST")) {
        t_prefetch_lifo();
        return 0;
    }
    const char *off = getenv("NVK_WARM_OFF");
    const char *slow = getenv("NVK_WARM_SLOW_SAVE_US");
    const int delay_us = slow && slow[0] ? atoi(slow) : 0;
    g_flag = off && off[0] == '1' ? 0 : 1;
    if (!g_flag) {
        // 没有预热线程：快落盘走 saver 断言返回 0；慢落盘断言有界返回 1（前台绝不写盘）。
        if (delay_us > 0) t_stop_without_thread_slow(); else t_disable(0);
        printf("nvk warm scheduler test: ok（关）\n");
        return 0;
    }
    if (delay_us > 0) {
        t_stop_budget_slow();
        printf("nvk warm scheduler test: ok（慢落盘 3 秒）\n");
        return 0;
    }
    t_order_and_counts();
    printf("nvk warm scheduler test: ok\n");
    return 0;
}
