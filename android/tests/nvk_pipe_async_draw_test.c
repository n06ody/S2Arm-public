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
void nvk_pipe_demand_worker_enter(void) {}
nvn_gpu_rc nvn_gpu_pipe_get(const nvn_gpu_pipe_desc *d, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err);
void nvn_gpu_set_pipe(nvn_gpu_cmd c, nvn_gpu_pipe p);
void nvn_gpu_draw(nvn_gpu_cmd c, const nvn_gpu_draw_args *a);
int nvn_gpu_pipe_lookup(const nvn_gpu_pipe_desc *d, nvn_gpu_pipe *out);

// ---------- 开关：两组值（进程参数选） ----------
static int g_item_ms = 20, g_frame_ms = 100, g_unsafe_ms = 300;
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_pipe_async") || !strcmp(name, "nvk_pipe_workers")) return 1;
    if (!strcmp(name, "fu_item_ms")) return g_item_ms;
    if (!strcmp(name, "fu_frame_ms")) return g_frame_ms < 0 ? dflt : g_frame_ms;
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
static const nvn_gpu_pipe_desc *g_driver_recipe;   // 单后台线程夹具，受 g_bmu 保护
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
    if (bg) {
        int k = g_bg_entered++;
        g_driver_recipe = d;
        while (k < MAXP && g_gate[k] == GATE_HOLD) pthread_cond_wait(&g_bcv, &g_bmu);
        g_bg_builds++;
    } else {
        g_fg_builds++;
        pthread_cond_broadcast(&g_bcv);
        // 同真实 nvn_pipe_flight：同步调用遇同键在途要等其结果，不能建第二次。
        while (g_driver_recipe && same_desc(d, g_driver_recipe)) pthread_cond_wait(&g_bcv, &g_bmu);
    }
    struct fpipe *p = NULL;
    for (int i = 0; i < g_np && !p; i++) if (g_p[i].session == sess && same_desc(&g_p[i].d, d)) p = &g_p[i];
    if (!p && g_np < MAXP) { p = &g_p[g_np++]; p->d = *d; p->session = sess; }
    if (bg) { g_driver_recipe = NULL; pthread_cond_broadcast(&g_bcv); }
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
static const uint8_t g_prog_identity[32] = {0x5a, [31] = 0xa5};
static const struct rp_prog g_borrow_prog = {.sh = {&g_fsh[0], &g_fsh[1]}};
int rp_shader_prog_borrow(const uint8_t identity[32], const struct rp_prog **out) {
    if (memcmp(identity, g_prog_identity, sizeof g_prog_identity)) { *out = NULL; return 0; }
    __atomic_fetch_add(&g_borrows, 1, __ATOMIC_RELAXED); *out = &g_borrow_prog; return 1;
}
void rp_shader_prog_return(const uint8_t identity[32]) { (void)identity; __atomic_fetch_add(&g_returns, 1, __ATOMIC_RELAXED); }

// ---------- 一帧：清目标（可选）→ 可选绑 SSBO / 开 TF → 若干笔绘制；返回这次提交的墙钟（毫秒） ----------
enum {
    F_CLEAR = 1, F_SSBO = 2, F_TF = 4, F_MESH = 8, F_SMALL = 16,
    F_TINY = 32, F_IMG = 64, F_DISCARD = 128, F_INDEXED = 256,
    F_D24 = 512, F_STENCIL_ONLY = 1024, F_DEPTH_ONLY = 2048,
};
static tex_t g_mesh_depth, g_img;
static uint16_t g_indices[6] = {0, 1, 2, 2, 3, 0};
static int prim_count(int prim) { return prim == 0 ? 1 : (prim == 1 || prim == 3) ? 2 : 3; }
static double run_frame(uint64_t frame, int flags, const int *prims, int n) {
    nvn_be_set_frame(frame);
    stream s = {0}; q_env(&s);
    if (flags & F_MESH) {
        const uint32_t w = flags & F_SMALL ? 320 : 640, h = flags & F_SMALL ? 240 : 360;
        g_qrt = mk_tex(91, 0, NVN_FORMAT_RGBA8, w, h, 1, 0);
        const int depth_fmt = flags & F_D24 ? NVN_FORMAT_DEPTH24_STENCIL8 : NVN_FORMAT_DEPTH32F;
        g_mesh_depth = mk_tex(92, 0x180000, depth_fmt, w, h, 1, 0);
        put_set_rt(&s, &g_qrt, &g_mesh_depth);
        p_viewports *vp = put(&s, OP_VIEWPORTS, sizeof *vp);
        vp->n = 1; vp->v[0][2] = (float)w; vp->v[0][3] = (float)h;
        ds_st ds = {.dt = 1, .dw = 1, .df = NVN_DEPTH_FUNC_LESS};
        if (flags & F_D24) {
            ds.st = 1;
            ds.sf_f = ds.sf_b = NVN_STENCIL_FUNC_ALWAYS;
            ds.zpass_f = ds.zpass_b = NVN_STENCIL_OP_REPLACE;
        }
        put_state(&s, 5, &ds, sizeof ds);
        if (flags & F_CLEAR) {
            p_clear_ds *dc = put(&s, OP_CLEAR_DS, sizeof *dc);
            dc->depth = 1.0f;
            dc->depth_mask = !(flags & F_STENCIL_ONLY);
            dc->stencil_mask = (flags & F_D24) && !(flags & F_DEPTH_ONLY) ? 0xff : 0;
        }
    }
    memcpy(g_fprog.identity, g_prog_identity, sizeof g_prog_identity);
    if (flags & F_CLEAR) { p_clear_color *cc = put(&s, OP_CLEAR_COLOR, sizeof *cc); cc->index = 0; cc->mask = 0xf; cc->rgba[3] = 1; }
    if (flags & F_SSBO) { p_bind_buf *b = put(&s, OP_BIND_SSBO, sizeof *b); b->stage = 1; b->index = 0; b->addr = 0x10000; b->size = 64; }
    if (flags & F_TF) { p_tf_state *t = put(&s, OP_TF_STATE, sizeof *t); t->what = 0; }
    if (flags & F_DISCARD) {
        p_rast_discard *r = put(&s, OP_RAST_DISCARD, sizeof *r);
        r->on = 1;
    }
    if (flags & F_IMG) {
        g_img = mk_tex(93, 0x300000, NVN_FORMAT_RGBA8, 8, 8, 1, NVN_TEXTURE_FLAGS_IMAGE);
        nvn_be_reg_tex(3, &g_img, NULL);
        nvn_be_set_tex_pool_n(8);
        g_fprog.nent[1] = 1;
        g_fprog.ent[1][0] = (nvn_gpu_layout_ent){
            .cls = NVN_GPU_RES_IMAGE, .src_kind = NVN_RB_IMG, .src_reg = 2,
            .access = NVN_GPU_ACC_RW, .count = 1, .src_reg2 = UINT16_MAX,
        };
        rp_S.img[1][2] = 3;  // 同 t_graphics_image 的真实回放绑定槽口径
    }
    if (flags & F_INDEXED) nvn_be_pool_add(g_indices, sizeof g_indices);
    for (int i = 0; i < n; i++) {
        const int prim = prims[i] & 15;
        if (flags & F_MESH) {
            poly_st poly = {.cull = (uint32_t)prims[i] >> 4, .mode = NVN_POLYGON_MODE_FILL};
            put_state(&s, 4, &poly, sizeof poly);
        }
        if (flags & F_INDEXED) {
            p_draw_idx *d = put(&s, OP_DRAW_IDX, sizeof *d);
            d->prim = (uint32_t)prim; d->count = 6; d->instances = 1; d->itype = 1;
            d->ibuf = (uint64_t)(uintptr_t)g_indices;
        } else {
            p_draw *d = put(&s, OP_DRAW, sizeof *d);
            d->prim = (uint32_t)prim; d->instances = 1;
            d->count = (flags & F_TINY) ? 6u : (flags & F_MESH) ? 24u : (uint32_t)prim_count(prim);
        }
    }
    end(&s);
    const uint64_t t0 = now_ns(); submit1(&s); const double ms = (double)(now_ns() - t0) / 1e6;
    fake_complete(NVN_GPU_DONE_OK);
    if (flags & F_INDEXED) nvn_be_pool_del(g_indices);
    return ms;
}
static const int TRI[] = {4};
// 后台建完第 k 次（从 0 数）为止：按事件等，不设墙钟上限（挂死交门禁分组超时）
static int wait_bg_built(int k) { for (;;) { pthread_mutex_lock(&g_bmu); int b = g_bg_builds; pthread_mutex_unlock(&g_bmu); if (b > k) return 1; sleep_ms(1); } }
// 放闸后等结果真发布进待取表（需求模块 published 涨过放闸前的值）再画下一帧：后台建好（g_bg_builds）在发布之前，
//   中间若下一帧先到，那笔管线还挂着「卡住」标记，同管线直接跳过（0 ms），用例 6 就红在「建好后照画」。
//   一台测试主机上 16 路并跑：只等建好 80 次红 16 次（全在 :212，现场 gdraws=0、第 2 帧 0 ms、built=1）；等发布 128 次全绿。
static uint64_t demand_published(void) { struct nvn_pipe_demand_stats st; nvn_vk_pipe_demand_stats(&st); return st.published; }
static int open_and_wait_published(int k) {
    const uint64_t p0 = demand_published();
    gate_open(k);
    if (!wait_bg_built(k)) return 0;
    while (demand_published() <= p0) sleep_ms(1);   // 按事件等发布，不设墙钟上限
    return 1;
}
// 等待超时次数（需求模块：一笔按上限等、到点没建好 = 1 次）。「第二笔不再等」「不按首用等」按这个计数判，不量墙钟
static uint64_t demand_timeouts(void) { struct nvn_pipe_demand_stats st; nvn_vk_pipe_demand_stats(&st); return st.timeouts; }
// 定时放闸：ms 毫秒后放第 k 次
struct rel { int k; unsigned ms; };
static void *rel_main(void *a) { struct rel *r = a; sleep_ms(r->ms); gate_open(r->k); return NULL; }
static pthread_t release_after(struct rel *r) { pthread_t t; pthread_create(&t, NULL, rel_main, r); return t; }
static void session_begin(void) {
    pass_fresh(); g_gdraws = 0; g_bad_draw = 0; g_last_set = NULL; gate_reset();
    pthread_mutex_lock(&g_bmu); g_session++; g_np = 0; pthread_mutex_unlock(&g_bmu);
    nvn_vk_pipe_demand_start();
    CHECK(nvn_vk_pipe_demand_worker_count() == 1);   // 前提：本局后台 worker 真起来了（没起来时被测的笔会走前台同步建，后面的断言全错位）
}
// 停局 + 放开所有闸 + 按事件等旧 worker 真退出：产品的 stop 只有界等 worker（卡在闸里的那条要等放闸后才退出），
//   不等它退出就进下一个用例，下一局 start 会因旧 worker 还在跑而不启动（负载高时 11-21 车道偶发红在用例 6）
static void session_end(void) {
    nvn_vk_pipe_demand_stop(); for (int k = 0; k < MAXP; k++) gate_open(k);
    while (!nvn_vk_pipe_demand_quiescent()) sleep_ms(1);
}
// 目标先画两帧（三角形，后台建、闸开着）：第 3 帧起这张目标「近两帧画过」
static void steady_target(void) {
    CHECK(run_frame(1, F_CLEAR | F_MESH, TRI, 1) >= 0 && g_gdraws == 1);
    CHECK(run_frame(2, F_CLEAR | F_MESH, TRI, 1) >= 0 && g_gdraws == 2);
}
static int g_case_fail;
#define CASE_BEGIN() int f0_ = g_fail
#define CASE_END(name) do { printf("%s %s\n", g_fail == f0_ ? "通" : "【红】", name); if (g_fail != f0_) g_case_fail++; } while (0)

// 1) 可跳的笔（目标本帧清过、近两帧画过、不写 storage）：后台还没建好 → 等到单项上限就跳，不画；同一管线再来一笔：late 项标，直接跳不再等。
//    闸放开、建好之后：下一帧画出来，用的是后台为它建的那条（提交线程没自己建）。
static void c_steady_skip(void) {
    CASE_BEGIN(); session_begin(); steady_target();
    const int k = bg_entered(); gate_hold(k);
    const int strip2[] = {5, 5};
    const uint64_t to0 = demand_timeouts();
    const double ms = run_frame(3, F_CLEAR | F_MESH, strip2, 2);
    CHECK(g_gdraws == 2);                                         // 两笔都跳了
    CHECK(ms >= g_item_ms - 2);                                   // 第一笔等满单项上限（下界：负载只会更长）
    CHECK(demand_timeouts() == to0 + 1);                          // 第二笔 late、不再等：只有第一笔那一次超时
    CHECK(open_and_wait_published(k));
    const int fg0 = g_fg_builds;
    const int strip[] = {5};
    run_frame(4, F_CLEAR | F_MESH, strip, 1);
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
    const double ms = run_frame(3, F_CLEAR | F_MESH | F_SSBO, pts1, 1);
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
    const uint64_t to0 = demand_timeouts();
    run_frame(3, F_CLEAR | F_TF, pts1, 1);
    CHECK(g_gdraws == 2 && nvn_unimpl_count("draw", "transform_feedback_active") == tf0 + 1);
    CHECK(bg_entered() == k && demand_timeouts() == to0);        // 没交后台建，也没按首用等（一次等待超时都没有）
    { stream e = {0}; p_tf_state *tt = put(&e, OP_TF_STATE, sizeof *tt); tt->what = 1; end(&e); submit1(&e); fake_complete(NVN_GPU_DONE_OK); }
    session_end(); CASE_END("开着 TF：按未接具名拒画，不算首用跳过、不交后台建");
}
// 5) 帧预算按单调钟扣：同一帧里 5 条不同的新管线都可跳；单项上限 item，预算 frame。
//    期望：这一帧等待合计不超过帧预算（加一个时钟粒度的余量），5 笔都跳；下一帧预算重置，没打项标（预算裁短的）那条又能等满单项上限。
static void c_frame_budget(void) {
    CASE_BEGIN(); session_begin(); steady_target();
    { const int b = bg_entered(); for (int k = b; k < b + 8; k++) gate_hold(k); }
    const int five[] = {5, 6, 5 | (1 << 4), 6 | (1 << 4), 5 | (2 << 4)};
    const uint64_t to0 = demand_timeouts();
    const double ms = run_frame(3, F_CLEAR | F_MESH, five, 5);
    CHECK(g_gdraws == 2);
    CHECK(ms >= g_frame_ms - 3);                                  // 5 笔各可等 item、合计超过预算：预算一定用满（下界）
    // 预算 frame = 100、单项 item = 30：至多前 4 笔真等（30 + 30 + 30 + 剩下的 10），第 5 笔没预算不等；预算失效时 5 笔都等 = 5 次。
    //   负载高时单笔等得更久、预算更早用完，超时只会更少，所以判「≤ 4」
    CHECK(demand_timeouts() - to0 <= 4);
    const int last[] = {5 | (2 << 4)};
    const uint64_t to1 = demand_timeouts();
    const double ms2 = run_frame(4, F_CLEAR | F_MESH, last, 1);           // 第 5 条在上一帧是「预算用完」跳的，不打 late：新一帧照常等满单项上限
    CHECK(g_gdraws == 2 && ms2 >= g_item_ms - 2 && demand_timeouts() == to1 + 1);
    session_end(); CASE_END("帧预算：单调钟扣、用完即跳、下一帧重置");
}
// 6) 不可跳的笔耗完后台等待后转同步，同键仍由原后台构建完成。
struct cap_release { int gate, done, followed; };
static void *release_on_sync(void *opaque) {
    struct cap_release *r = opaque;
    pthread_mutex_lock(&g_bmu);
    while (!g_fg_builds && !r->done) pthread_cond_wait(&g_bcv, &g_bmu);
    r->followed = g_fg_builds && g_driver_recipe;
    pthread_mutex_unlock(&g_bmu);
    gate_open(r->gate);
    return NULL;
}
static void c_unsafe_sync(void) {
    CASE_BEGIN(); session_begin();
    const int k = bg_entered(); gate_hold(k);
    struct cap_release r = {.gate = k};
    pthread_t t;
    if (pthread_create(&t, NULL, release_on_sync, &r) != 0) {
        CHECK(0); session_end(); CASE_END("创建同步等待夹具"); return;
    }
    const int tri2[] = {4, 4};
    const uint64_t to0 = demand_timeouts();
    run_frame(1, F_CLEAR, tri2, 2);
    pthread_mutex_lock(&g_bmu); r.done = 1; pthread_cond_broadcast(&g_bcv); pthread_mutex_unlock(&g_bmu);
    pthread_join(t, NULL);
    CHECK(demand_timeouts() == to0 + 1 && r.followed);
    CHECK(g_gdraws == 2 && g_bad_draw == 0 && bg_entered() == k + 1);
    CHECK(g_bg_builds == 1 && g_fg_builds == 1);
    session_end(); CASE_END("不可跳的笔：耗完 unsafe 等待，跟随同键构建并提交两笔");
}
// 7) 停局时在途构建超出有界等待：旧 worker 保持归属，不能启动下一局 worker；旧线程退出后才允许重启。
static void c_stop_discards(void) {
    CASE_BEGIN(); session_begin(); steady_target();
    const int k0 = bg_entered(); gate_hold(k0);
    const int strip[] = {5};
    run_frame(3, F_CLEAR | F_MESH, strip, 1);
    CHECK(g_gdraws == 2);
    const uint64_t published = demand_published();
    nvn_vk_pipe_demand_stop();
    CHECK(!nvn_vk_pipe_demand_quiescent());                        // 超时仍保留旧的可 join worker
    CHECK(nvn_vk_pipe_demand_worker_count() == 1);
    nvn_vk_pipe_demand_start();
    CHECK(nvn_vk_pipe_demand_worker_count() == 1);                  // 不创建重叠 worker
    CHECK(bg_entered() == k0 + 1);
    gate_open(k0);
    while (!nvn_vk_pipe_demand_quiescent()) sleep_ms(1);            // 按事件等旧 worker 退出，不设墙钟上限
    CHECK(nvn_vk_pipe_demand_worker_count() == 0);
    CHECK(demand_published() == published);
    pass_fresh(); g_gdraws = 0; g_bad_draw = 0; g_last_set = NULL;
    pthread_mutex_lock(&g_bmu); g_session++; pthread_mutex_unlock(&g_bmu);
    nvn_vk_pipe_demand_start();
    run_frame(1, F_CLEAR | F_MESH, strip, 1);
    CHECK(g_gdraws == 1 && g_bad_draw == 0);
    CHECK(bg_entered() == k0 + 2);                                 // 旧 worker 退出后才构建新一代
    session_end(); CASE_END("停局：旧 worker 未退出前不重启，回收后下一局再建");
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

static void c_default_budget(int fmt_flags) {
    CASE_BEGIN(); session_begin();
    const int flags = F_CLEAR | F_MESH | fmt_flags;
    run_frame(1, flags, TRI, 1);
    run_frame(2, flags, TRI, 1);
    CHECK(g_gdraws == 2 && g_bad_draw == 0);
    const int k = bg_entered();
    gate_hold(k); gate_hold(k + 1);
    const int pending[] = {5, 6};
    const uint64_t to0 = demand_timeouts();
    const double waited = run_frame(3, flags, pending, 2);
    const uint64_t timeouts = demand_timeouts() - to0;
    printf("default mesh budget (%s): %u ms, timeouts=%llu, submit=%.1f ms\n",
           fmt_flags & F_D24 ? "D24S8" : "D32F", nvk_fu_frame_ms(), (unsigned long long)timeouts, waited);
    CHECK(nvk_fu_frame_ms() == 8);
    CHECK(timeouts == 1 && g_gdraws == 2);
    session_end();
    CASE_END("常规大深度网格：缺省最多等一次帧预算，其他待编笔立即交后台");
}

static void c_protected_draws(void) {
    static const struct { const char *name; int flags; int clear; } cases[] = {
        {"小附件", F_SMALL, F_CLEAR},
        {"6 顶点", F_TINY, F_CLEAR},
        {"6 索引", F_INDEXED, F_CLEAR},
        {"SSBO", F_SSBO, F_CLEAR},
        {"imageStore", F_IMG, F_CLEAR},
        {"关闭光栅化", F_DISCARD, F_CLEAR},
        {"持久附件", 0, 0},
        {"只清模板：深度仍持久", F_D24 | F_STENCIL_ONLY, F_CLEAR},
        {"只清深度：模板仍持久", F_D24 | F_DEPTH_ONLY, F_CLEAR},
    };
    const int alternate[] = {5};
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        CASE_BEGIN(); session_begin();
        const int warm = F_CLEAR | F_MESH | (cases[i].flags & (F_SMALL | F_D24));
        run_frame(1, warm, TRI, 1);
        run_frame(2, warm, TRI, 1);
        CHECK(g_gdraws == 2);
        const int k = bg_entered();
        const uint64_t timeouts = demand_timeouts();
        gate_hold(k);
        struct rel r = {k, 60};
        pthread_t t = release_after(&r);
        run_frame(3, F_MESH | cases[i].clear | cases[i].flags, alternate, 1);
        pthread_join(t, NULL);
        CHECK(g_gdraws == 3 && !g_bad_draw && demand_timeouts() == timeouts);
        if (cases[i].flags & F_IMG) CHECK(rp_tex_debug_state(g_img.serial) & 4);
        session_end(); CASE_END(cases[i].name);
    }
}

int main(int argc, char **argv) {
    g_main = pthread_self();
    const char *mode = argc > 1 ? argv[1] : "main";
    if (!strcmp(mode, "default")) {
        g_item_ms = 40; g_frame_ms = -1;
        c_default_budget(0);
        c_default_budget(F_D24);
    }
    else if (!strcmp(mode, "protected")) { g_item_ms = 20; g_frame_ms = 8; c_protected_draws(); }
    else if (!strcmp(mode, "unbounded")) { g_item_ms = 20; g_frame_ms = 100; g_unsafe_ms = 0; c_unbounded(); }
    else {
        g_item_ms = 30; g_frame_ms = 100; g_unsafe_ms = 300;
        c_steady_skip(); c_first_frame_waits(); c_ssbo_waits(); c_tf_not_fu_skip(); c_frame_budget(); c_unsafe_sync(); c_stop_discards();
    }
    CHECK(g_borrows == g_returns);
    printf("nvk_pipe_async_draw（%s）：%d 项检查，%d 项红，%d 个用例红\n", mode, g_checks, g_fail, g_case_fail);
    return g_fail ? 1 : 0;
}
