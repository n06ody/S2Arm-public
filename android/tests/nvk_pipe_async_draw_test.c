// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 新回放层「首用管线异步」的绘制级宿主用例：真 rp_draw.c / rp_pass.c（取管线、skip_safe、fresh / seen 标记）+ 真 nvn_vk_pipe_demand.c /
// nvn_pipe_pend.c（后台建管线、待定表、项标）+ 真 nvn_first_use.c（判定表、帧预算），后端与平台件用 t_nvn_replay.c 的替身。
// 建管线换成可控替身：按调用顺序给每次后台建一个闸（放行 / 延时放行 / 一直挡），建出的管线对象各不相同、记下自己的描述，
// 设管线时能核「这一笔拿到的是不是为它建的那条」。
// 期望值只从合同来（docs/设计_管线首用异步与跳过.md §2–3、nvn_first_use.h 的判定表、iOS pso_pend 语义），不读被测实现的输出：
//   可跳 = 不写 storage、不开 TF、每个目标本帧整张清过且近两帧画过；可跳的笔在帧预算内等、等不到就跳；
//   不可跳的笔按 unsafe 上限等，0 = 不设限；late 只挡可跳的笔，stuck 挡一切；预算按单调钟扣、每帧重置；
//   停局后在途的结果不得发布进下一局。
// 运行：nvk_pipe_async_draw_test main | unbounded（两组开关值各一个进程：开关值在进程内只读一次）。
#define main replay_fixture_main
#define nvn_gpu_pipe_get fixture_pipe_get
#define nvn_gpu_set_pipe fixture_set_pipe
#define nvn_plat_flag fixture_plat_flag
#define nvn_gpu_draw fixture_draw
#include "../../graphics/nvn/tests/t_nvn_replay.c"
#undef nvn_gpu_draw
#undef nvn_plat_flag
#undef nvn_gpu_set_pipe
#undef nvn_gpu_pipe_get
#undef main

#include <pthread.h>
#include <time.h>
#include "nvn_vk_pipe_demand.h"
#include "nvn_first_use.h"

int nvn_plat_flag(const char *name, int dflt);
nvn_gpu_rc nvn_gpu_pipe_get(const nvn_gpu_pipe_desc *d, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err);
void nvn_gpu_set_pipe(nvn_gpu_cmd c, nvn_gpu_pipe p);
void nvn_gpu_draw(nvn_gpu_cmd c, const nvn_gpu_draw_args *a);
int nvn_gpu_pipe_lookup(const nvn_gpu_pipe_desc *d, nvn_gpu_pipe *out);

// ---------- 开关：两组值（进程参数选） ----------
static int g_item_ms = 20, g_frame_ms = 100, g_unsafe_ms = 300;
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_pipe_async")) return 1;
    if (!strcmp(name, "fu_item_ms")) return g_item_ms;
    if (!strcmp(name, "fu_frame_ms")) return g_frame_ms;
    if (!strcmp(name, "fu_unsafe_ms")) return g_unsafe_ms;
    return fixture_plat_flag(name, dflt);
}

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void sleep_ms(unsigned ms) { struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L}; nanosleep(&t, NULL); }

// ---------- 建管线替身：后端缓存 + 按调用序号的闸 ----------
enum { MAXP = 64, GATE_OPEN = 0, GATE_HOLD = 1 };
struct fpipe { struct nvn_gpu_pipe_s s; nvn_gpu_pipe_desc d; int session; };
static struct fpipe g_p[MAXP]; static int g_np;
static pthread_mutex_t g_bmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_bcv = PTHREAD_COND_INITIALIZER;
static int g_gate[MAXP];          // 第 k 次后台建（从 0 数）的闸；缺省放行
static int g_bg_builds, g_fg_builds, g_bg_entered;
static int g_session = 1;         // 当前「后端会话」号：建出的管线带上它，设管线时核不能用上一局的
static pthread_t g_main;
static void gate_reset(void) { pthread_mutex_lock(&g_bmu); memset(g_gate, 0, sizeof g_gate); g_bg_builds = g_fg_builds = g_bg_entered = 0; pthread_mutex_unlock(&g_bmu); }
static void gate_hold(int k) { pthread_mutex_lock(&g_bmu); g_gate[k] = GATE_HOLD; pthread_mutex_unlock(&g_bmu); }
static void gate_open(int k) { pthread_mutex_lock(&g_bmu); g_gate[k] = GATE_OPEN; pthread_cond_broadcast(&g_bcv); pthread_mutex_unlock(&g_bmu); }
static int bg_entered(void) { pthread_mutex_lock(&g_bmu); int n = g_bg_entered; pthread_mutex_unlock(&g_bmu); return n; }
static int same_desc(const nvn_gpu_pipe_desc *a, const nvn_gpu_pipe_desc *b) { return !memcmp(a, b, sizeof *a); }
nvn_gpu_rc nvn_gpu_pipe_get(const nvn_gpu_pipe_desc *d, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) {
    (void)mode; if (err) memset(err, 0, sizeof *err);
    const int bg = !pthread_equal(pthread_self(), g_main);
    pthread_mutex_lock(&g_bmu);
    const int sess = g_session;   // 开建时的会话：卡在闸里跨过停局的那条仍属于上一局（真后端里它用的是上一局的着色器与设备对象）
    if (bg) { int k = g_bg_entered++; while (k < MAXP && g_gate[k] == GATE_HOLD) pthread_cond_wait(&g_bcv, &g_bmu); g_bg_builds++; }
    else g_fg_builds++;
    struct fpipe *p = NULL;
    for (int i = 0; i < g_np && !p; i++) if (g_p[i].session == sess && same_desc(&g_p[i].d, d)) p = &g_p[i];
    if (!p && g_np < MAXP) { p = &g_p[g_np++]; p->d = *d; p->session = sess; }
    pthread_mutex_unlock(&g_bmu);
    *out = p ? &p->s : NULL;
    return (nvn_gpu_rc){p ? NVN_GPU_OK : NVN_GPU_E_OOM};
}
int nvn_gpu_pipe_lookup(const nvn_gpu_pipe_desc *d, nvn_gpu_pipe *out) {
    pthread_mutex_lock(&g_bmu);
    struct fpipe *p = NULL;
    for (int i = 0; i < g_np && !p; i++) if (g_p[i].session == g_session && same_desc(&g_p[i].d, d)) p = &g_p[i];
    pthread_mutex_unlock(&g_bmu);
    if (out) *out = p ? &p->s : NULL;
    return p != NULL;
}
static const struct fpipe *g_last_set; static int g_bad_draw;   // 画下去的这一笔用的管线不是为它的图元建的 / 是上一局的
void nvn_gpu_set_pipe(nvn_gpu_cmd c, nvn_gpu_pipe p) { g_last_set = (const struct fpipe *)p; fixture_set_pipe(c, p); }
void nvn_gpu_draw(nvn_gpu_cmd c, const nvn_gpu_draw_args *a) {
    if (!g_last_set || g_last_set->session != g_session || g_last_set->d.prim != a->prim) g_bad_draw++;
    fixture_draw(c, a);
}

// ---------- 程序借用替身：只认当前程序的身份 ----------
static int g_borrows, g_returns;
int rp_shader_prog_borrow(const uint8_t identity[32], const struct rp_prog **out) {
    if (memcmp(identity, g_fprog.identity, 32)) { *out = NULL; return 0; }
    __atomic_fetch_add(&g_borrows, 1, __ATOMIC_RELAXED); *out = &g_fprog; return 1;
}
void rp_shader_prog_return(const uint8_t identity[32]) { (void)identity; __atomic_fetch_add(&g_returns, 1, __ATOMIC_RELAXED); }

// ---------- 一帧：清目标（可选）→ 可选绑 SSBO / 开 TF → 若干笔绘制；返回这次提交的墙钟（毫秒） ----------
enum { F_CLEAR = 1, F_SSBO = 2, F_TF = 4 };
static int prim_count(int prim) { return prim == 0 ? 1 : (prim == 1 || prim == 3) ? 2 : 3; }
static double run_frame(uint64_t frame, int flags, const int *prims, int n) {
    nvn_be_set_frame(frame);
    stream s = {0}; q_env(&s);
    memset(g_fprog.identity, 0, sizeof g_fprog.identity); g_fprog.identity[0] = 0x5a; g_fprog.identity[31] = 0xa5;   // 有内容身份才走异步
    if (flags & F_CLEAR) { p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[3] = 1; }
    if (flags & F_SSBO) { p_bind_buf *b = put(&s, OP_BIND_SSBO, sizeof *b); b->stage = 1; b->index = 0; b->addr = 0x10000; b->size = 64; }
    if (flags & F_TF) { p_tf_state *t = put(&s, OP_TF_STATE, sizeof *t); t->what = 0; }
    for (int i = 0; i < n; i++) { p_draw *d = put(&s, OP_DRAW, sizeof *d); d->prim = (uint32_t)prims[i]; d->count = (uint32_t)prim_count(prims[i]); d->instances = 1; }
    end(&s);
    const uint64_t t0 = now_ns(); submit1(&s); const double ms = (double)(now_ns() - t0) / 1e6;
    fake_complete(NVN_GPU_DONE_OK);
    return ms;
}
static const int TRI[] = {4};
// 后台建完第 k 次（从 0 数）为止：最多等 2 秒
static int wait_bg_built(int k) { for (int i = 0; i < 2000; i++) { pthread_mutex_lock(&g_bmu); int b = g_bg_builds; pthread_mutex_unlock(&g_bmu); if (b > k) return 1; sleep_ms(1); } return 0; }
// 定时放闸：ms 毫秒后放第 k 次
struct rel { int k; unsigned ms; };
static void *rel_main(void *a) { struct rel *r = a; sleep_ms(r->ms); gate_open(r->k); return NULL; }
static pthread_t release_after(struct rel *r) { pthread_t t; pthread_create(&t, NULL, rel_main, r); return t; }
static void session_begin(void) {
    pass_fresh(); g_gdraws = 0; g_bad_draw = 0; g_last_set = NULL; gate_reset();
    pthread_mutex_lock(&g_bmu); g_session++; pthread_mutex_unlock(&g_bmu);
    nvn_vk_pipe_demand_start();
}
static void session_end(void) { nvn_vk_pipe_demand_stop(); for (int k = 0; k < MAXP; k++) gate_open(k); }
// 目标先画两帧（三角形，后台建、闸开着）：第 3 帧起这张目标「近两帧画过」
static void steady_target(void) {
    CHECK(run_frame(1, F_CLEAR, TRI, 1) >= 0 && g_gdraws == 1);
    CHECK(run_frame(2, F_CLEAR, TRI, 1) >= 0 && g_gdraws == 2);
}
static int g_case_fail;
#define CASE_BEGIN() int f0_ = g_fail
#define CASE_END(name) do { printf("%s %s\n", g_fail == f0_ ? "通" : "【红】", name); if (g_fail != f0_) g_case_fail++; } while (0)

// 1) 可跳的笔（目标本帧清过、近两帧画过、不写 storage）：后台还没建好 → 等到单项上限就跳，不画；同一管线再来一笔：late 项标，直接跳不再等。
//    闸放开、建好之后：下一帧画出来，用的是后台为它建的那条（提交线程没自己建）。
static void c_steady_skip(void) {
    CASE_BEGIN(); session_begin(); steady_target();
    const int k = bg_entered(); gate_hold(k);
    const int pts2[] = {0, 0};
    const double ms = run_frame(3, F_CLEAR, pts2, 2);
    CHECK(g_gdraws == 2);                                         // 两笔都跳了
    CHECK(ms >= g_item_ms - 2 && ms < 2.0 * g_item_ms);           // 第一笔等满单项上限，第二笔不再等
    gate_open(k); CHECK(wait_bg_built(k));
    const int fg0 = g_fg_builds;
    const int pts1[] = {0};
    run_frame(4, F_CLEAR, pts1, 1);
    CHECK(g_gdraws == 3 && g_bad_draw == 0 && g_fg_builds == fg0);
    session_end(); CASE_END("可跳的笔：等满单项上限后跳过、late 后同管线直接跳、建好后下一帧照画");
}
// 2) 目标第一帧（没有近两帧画过的证据）：不可跳，一直等到后台建好才画（等的时长 ≥ 放闸时刻）。
static void c_first_frame_waits(void) {
    CASE_BEGIN(); session_begin();
    const int k = bg_entered(); gate_hold(k);
    struct rel r = {k, 60}; pthread_t t = release_after(&r);
    const double ms = run_frame(1, F_CLEAR, TRI, 1);
    pthread_join(t, NULL);
    CHECK(g_gdraws == 1 && g_bad_draw == 0);
    CHECK(ms >= 55);
    session_end(); CASE_END("首帧目标：不跳，等到建好再画");
}
// 3) 绑了存储缓冲（可能写 storage）：目标虽然满足清过 + 近两帧画过，也不可跳，等到建好才画。
static void c_ssbo_waits(void) {
    CASE_BEGIN(); session_begin(); steady_target();
    const int k = bg_entered(); gate_hold(k);
    struct rel r = {k, 60}; pthread_t t = release_after(&r);
    const int pts1[] = {0};
    const double ms = run_frame(3, F_CLEAR | F_SSBO, pts1, 1);
    pthread_join(t, NULL);
    CHECK(g_gdraws == 3 && g_bad_draw == 0);
    CHECK(ms >= 55);
    session_end(); CASE_END("绑了 SSBO：不跳，等到建好再画");
}
// 4) 开着变换反馈：不能算成「首用跳过」。新层整笔按未接 transform_feedback_active 具名拒画（在取管线之前），不进后台建、不计首用跳过。
static void c_tf_not_fu_skip(void) {
    CASE_BEGIN(); session_begin(); steady_target();
    const int k = bg_entered(); gate_hold(k);
    const uint64_t tf0 = nvn_unimpl_count("draw", "transform_feedback_active");
    const int pts1[] = {0};
    const double ms = run_frame(3, F_CLEAR | F_TF, pts1, 1);
    CHECK(g_gdraws == 2 && nvn_unimpl_count("draw", "transform_feedback_active") == tf0 + 1);
    CHECK(bg_entered() == k && ms < g_item_ms - 2);              // 没交后台建，也没按首用等
    { stream e = {0}; p_tf_state *tt = put(&e, OP_TF_STATE, sizeof *tt); tt->what = 1; end(&e); submit1(&e); fake_complete(NVN_GPU_DONE_OK); }
    session_end(); CASE_END("开着 TF：按未接具名拒画，不算首用跳过、不交后台建");
}
// 5) 帧预算按单调钟扣：同一帧里 5 条不同的新管线都可跳；单项上限 item，预算 frame。
//    期望：这一帧等待合计不超过帧预算（加一个时钟粒度的余量），5 笔都跳；下一帧预算重置，没打项标（预算裁短的）那条又能等满单项上限。
static void c_frame_budget(void) {
    CASE_BEGIN(); session_begin(); steady_target();
    { const int b = bg_entered(); for (int k = b; k < b + 8; k++) gate_hold(k); }
    const int five[] = {0, 1, 3, 5, 6};
    const double ms = run_frame(3, F_CLEAR, five, 5);
    CHECK(g_gdraws == 2);
    CHECK(ms >= g_frame_ms - 3 && ms < g_frame_ms + 15);
    const int last[] = {6};
    const double ms2 = run_frame(4, F_CLEAR, last, 1);           // 第 5 条在上一帧是「预算用完」跳的，不打 late：新一帧照常等满单项上限
    CHECK(g_gdraws == 2 && ms2 >= g_item_ms - 2 && ms2 < g_item_ms + 15);
    session_end(); CASE_END("帧预算：单调钟扣、用完即跳、下一帧重置");
}
// 6) 不可跳的笔按 unsafe 上限等，等满就跳并打 stuck；同管线再来一笔（同样不可跳）立即跳；放闸建好后下一帧画出来。
static void c_stuck_cap(void) {
    CASE_BEGIN(); session_begin();
    const int k = bg_entered(); gate_hold(k);
    const int tri2[] = {4, 4};
    const double ms = run_frame(1, F_CLEAR, tri2, 2);
    CHECK(g_gdraws == 0);
    CHECK(ms >= g_unsafe_ms - 3 && ms < g_unsafe_ms + 40);       // 第一笔等满上限，第二笔不再等
    gate_open(k); CHECK(wait_bg_built(k));
    run_frame(2, F_CLEAR, TRI, 1);
    CHECK(g_gdraws == 1 && g_bad_draw == 0);
    session_end(); CASE_END("不可跳的笔：等满 unsafe 上限后跳过、stuck 后同管线直接跳、建好后照画");
}
// 7) 停局时在途的那条不能发布进下一局：上一局后台建卡住（停局有界等不到、线程脱离），下一局同一配方再要。
//    期望：下一局的这一笔等到本局自己建的那条再画（管线属于本局、图元对），不被上一局迟到的结果唤醒。
static void c_stop_discards(void) {
    CASE_BEGIN(); session_begin();
    const int k0 = bg_entered(); gate_hold(k0); gate_hold(k0 + 1);
    run_frame(1, F_CLEAR, TRI, 1);                                // 不可跳：等满 unsafe 后跳，后台那条卡在闸里
    CHECK(g_gdraws == 0);
    nvn_vk_pipe_demand_stop();                                    // 有界：等不到就脱离线程
    pass_fresh(); g_gdraws = 0; g_bad_draw = 0; g_last_set = NULL;
    pthread_mutex_lock(&g_bmu); g_session++; pthread_mutex_unlock(&g_bmu);
    nvn_vk_pipe_demand_start();
    struct rel ro = {k0, 50}, rn = {k0 + 1, 150};
    pthread_t to = release_after(&ro), tn = release_after(&rn);
    const double ms = run_frame(1, F_CLEAR, TRI, 1);
    pthread_join(to, NULL); pthread_join(tn, NULL);
    CHECK(g_gdraws == 1 && g_bad_draw == 0);                     // 画的是本局为它建的那条
    CHECK(ms >= 145);                                             // 没被上一局 50 ms 时迟到的结果唤醒
    session_end(); CASE_END("停局：上一局在途的结果不发布进下一局");
}
// 8)（unbounded 组：fu_unsafe_ms = 0 = 不设限）不可跳的笔一直等到建好再画，不按某个分片超时跳过。
static void c_unbounded(void) {
    CASE_BEGIN(); session_begin();
    const int k = bg_entered(); gate_hold(k);
    struct rel r = {k, 400}; pthread_t t = release_after(&r);
    const double ms = run_frame(1, F_CLEAR, TRI, 1);
    pthread_join(t, NULL);
    CHECK(g_gdraws == 1 && g_bad_draw == 0);
    CHECK(ms >= 395);
    session_end(); CASE_END("unsafe 上限 0（不设限）：一直等到建好再画");
}

int main(int argc, char **argv) {
    g_main = pthread_self();
    const char *mode = argc > 1 ? argv[1] : "main";
    if (!strcmp(mode, "unbounded")) { g_item_ms = 20; g_frame_ms = 100; g_unsafe_ms = 0; c_unbounded(); }
    else {
        g_item_ms = 30; g_frame_ms = 100; g_unsafe_ms = 300;
        c_steady_skip(); c_first_frame_waits(); c_ssbo_waits(); c_tf_not_fu_skip(); c_frame_budget(); c_stuck_cap(); c_stop_discards();
    }
    CHECK(g_borrows == g_returns);
    printf("nvk_pipe_async_draw（%s）：%d 项检查，%d 项红，%d 个用例红\n", mode, g_checks, g_fail, g_case_fail);
    return g_fail ? 1 : 0;
}
