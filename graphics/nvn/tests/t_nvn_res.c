// 回放核心的资源池与绑定构造单测：与 t_nvn_replay.c 共用同一份替身（重命名它的 main 后 include）。
// 覆盖两块改动：
//   ① 贴图内容检查按「全局写代号 + 段号 + 映射代际」只查一次（rp_res.c）；
//   ② 绑定构造省重（rp_draw.c）：B1 整组沿用上一笔、B3 驱动段按内容命中、B2 换管线按布局立脏。
// 期望值都由用例自己写下的输入直接得出，并经 rp_perf_take / 替身计数核对；带负对照。
#define main replay_fixture_main
#define rp_shader_resolve fixture_shader_resolve
#define nvn_plat_flag fixture_plat_flag
#define nvn_dirty_since fixture_dirty_since
#include "t_nvn_replay.c"
#undef nvn_dirty_since
#undef nvn_plat_flag
#undef rp_shader_resolve
#undef main
#include <stdio.h>

// 负对照开关：环境变量 T_NVN_RES_FLAGS_OFF=1 把三个省重开关都关掉（走改动前的旧行为）。
// 正常跑（不设变量）：三个开关缺省开，全绿；设了变量再跑：贴图纪元 / 整组沿用 / 驱动段命中三处必须变红。
static int res_flags_off(void) {
    static int v = -1; static unsigned k;
    if (v < 0 || !(++k & 255)) { const char *e = getenv("T_NVN_RES_FLAGS_OFF"); v = e && *e && *e != '0'; }
    return v;
}
int nvn_plat_flag(const char *name, int dflt) {
    if (res_flags_off() && (!strcmp(name, "nvk_bind_fast") || !strcmp(name, "nvk_drv_memo") || !strcmp(name, "nvk_tex_epoch"))) return 0;
    return fixture_plat_flag(name, dflt);
}

// 逐页比对的独立账：替身的 nvn_dirty_since 答案不变，这里另记被调次数和它要比的页数（按替身的 4096 字节页），用来核 RP_PERF_TEX_EPOCH_MISS / TEX_CHECK_PAGES。
static uint64_t g_since_calls, g_since_pages;
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) {
    g_since_calls++;
    if (len) { uintptr_t a = (uintptr_t)addr; g_since_pages += (a + len - 1) / PG - a / PG + 1; }
    return fixture_dirty_since(addr, len, stamp);
}

// 程序切换：替身只会给 g_fprog；B2 要两个程序对象，这里在用例指定时换成另一个。
static struct rp_prog *g_alt_prog;
int rp_shader_resolve(const struct rp_shader_req *q, const struct rp_prog **out) {
    if (g_alt_prog) { *out = g_alt_prog; return 0; }
    return fixture_shader_resolve(q, out);
}
static void res_prog_min(void) {   // 最小程序：没有绑定项；用例再按需加布局项
    memset(&g_fprog, 0, sizeof g_fprog);
    for (int st = 0; st < RP_PROG_STAGES; st++) for (int i = 0; i < RP_PROG_ENT_MAX; i++) g_fprog.ent[st][i].src_reg2 = UINT16_MAX;
    g_fprog.sh[0] = &g_fsh[0]; g_fprog.sh[1] = &g_fsh[1]; g_fprog.support_bytes = 2608; g_fprog.color_out_mask = 0xffffffffu;
}

// ---------- ① 贴图内容纪元（rp_res.c）----------
// 同一段里连续用同一张 CPU 贴图：第二次只比全局写代号，不扫脏页（RP_PERF_TEX_EPOCH +1），上传次数不变。
// 换段（新一次提交）必重核一次；游戏写过（写代号推进）必须重传；WriteTexels 强制作废纪元。
// 负对照：不是被写跟踪的内存不走快路，照旧每次保守查。
static void t_res_tex_epoch(void) {
    res_fresh();
    nvn_be_sub_begin();
    memset(g_pmem, 0, sizeof g_pmem);
    memset(g_pmem, 0x11, 8 * 8 * 4);
    tex_t t = mk_tex(900, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    t.mem = g_pmem;                                    // 放在被写跟踪的测试内存里（nvn_dirty_span 认得它）
    uint64_t u0 = rp_res_uploads();
    uint64_t p[RP_PERF_N];
    rp_perf_take(p);                                   // 清计数
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() == u0 + 1);   // 首用：整张传一次
    rp_perf_take(p);
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() == u0 + 1);   // 同段第二次：不传
    rp_perf_take(p);
    CHECK(p[RP_PERF_TEX_EPOCH] == 1);                                               // 而且根本没扫脏页（纪元命中）
    // 换一次提交（段号变）：内容没脏，重核一次但不重传；同一段里下一次又走纪元快路
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); nvn_be_sub_begin();
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() == u0 + 1);
    rp_perf_take(p);
    CHECK(p[RP_PERF_TEX_EPOCH] == 0);                   // 换段必重核（负对照：没有跳过）
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL);
    rp_perf_take(p);
    CHECK(p[RP_PERF_TEX_EPOCH] == 1);
    // 游戏写过：写代号推进，必须重传一次；之后同段再次使用回到纪元快路
    g_dirty_written = g_dirty_now + 1; g_dirty_now += 2;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() == u0 + 2);
    rp_perf_take(p);
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() == u0 + 2);
    rp_perf_take(p);
    CHECK(p[RP_PERF_TEX_EPOCH] == 1);
    // WriteTexels（nvn_be_tex_cpu_written）：纪元清零 → 强制重传
    nvn_be_tex_cpu_written(&t);
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() == u0 + 3);
    // 负对照：内存不在写跟踪范围（g_tmem）→ 没有纪元可依，照旧每次保守查，一次都不跳过
    tex_t u = mk_tex(901, 0x4000, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    memset(u.mem, 0x22, u.L.size);
    CHECK(rp_tex_resolve(&u, &g_fake_cmd) != NULL);
    uint64_t u1 = rp_res_uploads();
    rp_perf_take(p);
    for (int i = 0; i < 4; i++) CHECK(rp_tex_resolve(&u, &g_fake_cmd) != NULL);
    CHECK(p[RP_PERF_TEX_EPOCH] == 0);
    CHECK(rp_res_uploads() == u1);                     // 没脏：也不重传
    nvn_be_tex_release(&t); nvn_be_tex_release(&u);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// ---------- ①b 无关写让贴图纪元失效（复现 9000 的「取贴图视图」持锁段）----------
// 同一段里 N 张已传过、内容没变的 CPU 贴图；每笔绘制前游戏写一次别处（常量缓冲等，只推进全局写代号，这些贴图的页都没写），
// 每笔绘制把 N 张各取两次。理想：贴图没被写，一次都不用逐页比。现状：纪元键是全局写代号，每笔之后第一次取都没命中、整张逐页比，
// 第二次取才命中。两个计数必须和独立账（g_since_calls / g_since_pages）逐项一致；上传次数不变（内容没变，不该重传）。
static void t_res_tex_epoch_unrelated(void) {
    enum { N = 8, D = 32, W = 128, H = 128 };          // 128x128 RGBA8 = 64 KB = 16 页（替身 4096 字节一页）
    res_fresh();
    g_block_mem = (uint8_t *)(((uintptr_t)g_block_arena + BLOCK_TEST_ALIGN - 1) & ~(uintptr_t)(BLOCK_TEST_ALIGN - 1));
    nvn_be_sub_begin();
    tex_t t[N];
    for (int i = 0; i < N; i++) {
        t[i] = mk_tex(950 + (uint64_t)i, 0, NVN_FORMAT_RGBA8, W, H, 1, 0);
        t[i].mem = g_block_mem + (size_t)i * t[i].L.size;   // 放在被写跟踪的块内存里，各占一段
        memset(t[i].mem, 0x30 + i, t[i].L.size);
        CHECK(rp_tex_resolve(&t[i], &g_fake_cmd) != NULL);   // 首用：整张传
    }
    const uint64_t pages_per = t[0].L.size / PG;
    const uint64_t u0 = rp_res_uploads();
    uint64_t p[RP_PERF_N]; rp_perf_take(p);
    g_since_calls = g_since_pages = 0;
    for (int d = 0; d < D; d++) {
        g_dirty_now++;                                  // 无关写：只推进全局写代号（g_dirty_written 不动 = 这些贴图的页没被写）
        for (int r = 0; r < 2; r++) for (int i = 0; i < N; i++) CHECK(rp_tex_resolve(&t[i], &g_fake_cmd) != NULL);
    }
    rp_perf_take(p);
    CHECK(rp_res_uploads() == u0);                      // 内容没变：不重传
    CHECK(p[RP_PERF_TEX_EPOCH] == (uint64_t)N * D);     // 每笔里第二次取命中
    CHECK(p[RP_PERF_TEX_EPOCH_MISS] == g_since_calls && g_since_calls == (uint64_t)N * D);   // 每笔里第一次取没命中，且每次都调了逐页比对
    CHECK(p[RP_PERF_TEX_CHECK_PAGES] == g_since_pages && g_since_pages == (uint64_t)N * D * pages_per);
    printf("tex-epoch unrelated-write repro: textures=%d draws=%d resolves=%d epoch_skip=%llu epoch_miss=%llu pages_compared=%llu uploads=%llu (ideal: miss 0, pages 0)\n",
           N, D, 2 * N * D, (unsigned long long)p[RP_PERF_TEX_EPOCH], (unsigned long long)p[RP_PERF_TEX_EPOCH_MISS],
           (unsigned long long)p[RP_PERF_TEX_CHECK_PAGES], (unsigned long long)(rp_res_uploads() - u0));
    // 对照：没有无关写时同样 2N 次取，一次都不比（纪元键本身是对的，失效只来自全局写代号推进）
    g_since_calls = 0;
    for (int r = 0; r < 2; r++) for (int i = 0; i < N; i++) CHECK(rp_tex_resolve(&t[i], &g_fake_cmd) != NULL);
    rp_perf_take(p);
    CHECK(p[RP_PERF_TEX_EPOCH_MISS] == 0 && p[RP_PERF_TEX_CHECK_PAGES] == 0 && g_since_calls == 0 && p[RP_PERF_TEX_EPOCH] == 2 * N);
    for (int i = 0; i < N; i++) nvn_be_tex_release(&t[i]);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    g_block_mem = NULL;
}

// ---------- ② B1 整组沿用 + B3 驱动段按内容命中（rp_draw.c）----------
// 连续四笔同一程序、同一 base_instance：
//   ① 首次：三个阶段各发一次绑定（完整构造）；
//   ② 参数没变：三个阶段都直接沿用（BIND_FAST +3，一次 bind 都不发）；
//   ③ 换常量缓冲：只有顶点阶段重建；驱动段内容（存储缓冲表 / base_instance / 贴图句柄表）没变 → 命中上次的暂存（SLICE_HIT +2）；
//   ④ 换贴图：只有片元阶段重建（DRAW 阶段快路 +2）；贴图句柄表变了 → 驱动段必须重做（SLICE_HIT +0）。
static void t_res_bind_fast(void) {
    pass_fresh(); res_fresh();
    g_resolve_rc = g_cresolve_rc = 0; g_gdraws = 0;
    memset(g_pg_gen, 0, sizeof g_pg_gen); g_pg_tracked = 1; g_dirty_now = 100; g_dirty_written = 0;
    memset(g_pmem, 0, sizeof g_pmem); g_arena_used = 0;
    uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    nvn_be_pool_add(g_pmem, sizeof g_pmem);
    res_prog_min();
    g_fprog.nent[0] = 4;
    g_fprog.ent[0][0].cls = NVN_GPU_RES_UBO; g_fprog.ent[0][0].src_kind = NVN_RB_SUPPORT;
    g_fprog.ent[0][1].cls = NVN_GPU_RES_UBO; g_fprog.ent[0][1].src_kind = NVN_RB_CBUF; g_fprog.ent[0][1].src_reg = 2;
    g_fprog.ent[0][2].cls = NVN_GPU_RES_UBO; g_fprog.ent[0][2].src_kind = NVN_RB_DRV0;
    g_fprog.ent[0][3].cls = NVN_GPU_RES_UBO; g_fprog.ent[0][3].src_kind = NVN_RB_DRV2;   // 换常量缓冲时驱动段内容没变：两片都该命中
    g_fprog.nent[1] = 2;
    g_fprog.ent[1][0].cls = NVN_GPU_RES_TEX_SAMP; g_fprog.ent[1][0].src_kind = NVN_RB_TEX; g_fprog.ent[1][0].src_reg = 5;
    g_fprog.dtt[1][0] = NVN_TEXTURE_TARGET_2D + 1;
    g_fprog.ent[1][1].cls = NVN_GPU_RES_UBO; g_fprog.ent[1][1].src_kind = NVN_RB_DRV2;
    tex_t rtt = mk_tex(910, 0, NVN_FORMAT_RGBA8, 64, 36, 1, 0);
    tex_t tx = mk_tex(911, 0x4000, NVN_FORMAT_RGBA8, 8, 8, 1, 0), ty = mk_tex(912, 0x5000, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    samp_par sp; memset(&sp, 0, sizeof sp);
    nvn_be_reg_tex(7, &tx, NULL); nvn_be_reg_tex(8, &ty, NULL); nvn_be_reg_samp(3, &sp);
    memset(g_pmem + PG + 128, 0x11, 64); memset(g_pmem + PG + 512, 0x22, 64);
    stream s = {0};
    put_set_rt(&s, &rtt, NULL);
    p_bind_vattr *va = put(&s, OP_BIND_VATTR, sizeof *va); va->n = 1; va_st a0 = {.fmt = 0x2A, .offset = 0, .stream = 1}; memcpy(&va->st[0], &a0, 4);
    p_bind_vstream *vs = put(&s, OP_BIND_VSTREAM, sizeof *vs); vs->n = 2; vs->st[1].stride = 16;
    p_bind_vbuf_single *vb = put(&s, OP_BIND_VBUF, sizeof *vb); vb->first = 1; vb->size = 64; vb->addr = base + 2 * PG + 32 - 0x1000;
    p_bind_handles *bh = put(&s, OP_BIND_TEX, sizeof *bh); bh->stage = 1; bh->first = 5; bh->n = 1; bh->h[0] = 7 | 3 << 20;
    p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 0; ub->index = 2; ub->addr = base + PG + 128; ub->size = 64;
    p_draw *dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3; dn->instances = 1;   // ① 首笔
    size_t o1 = s.n;
    dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3; dn->instances = 1;           // ② 什么都没变
    size_t o2 = s.n;
    ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 0; ub->index = 2; ub->addr = base + PG + 512; ub->size = 64;
    dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3; dn->instances = 1;           // ③ 换常量缓冲
    size_t o3 = s.n;
    bh = put(&s, OP_BIND_TEX, sizeof *bh); bh->stage = 1; bh->first = 5; bh->n = 1; bh->h[0] = 8 | 3 << 20;
    dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3; dn->instances = 1;           // ④ 换贴图
    end(&s);
    memset(g_bind_calls, 0, sizeof g_bind_calls);
    nvn_be_sub_begin();
    const op_hdr eh = {OP_END, 8}; uint8_t sv[8]; uint64_t pf[RP_PERF_N];
    memcpy(sv, s.b + o1, 8); memcpy(s.b + o1, &eh, 8);
    nvn_be_exec(s.b);                                                 // ①
    CHECK(g_gdraws == 1 && g_bind_calls[0] == 1 && g_bind_calls[1] == 1 && g_bind_calls[2] == 1);
    rp_perf_take(pf);
    memcpy(s.b + o1, sv, 8);
    memcpy(sv, s.b + o2, 8); memcpy(s.b + o2, &eh, 8);
    nvn_be_exec(s.b + o1);                                            // ②
    CHECK(g_gdraws == 2 && g_bind_calls[0] == 1 && g_bind_calls[1] == 1 && g_bind_calls[2] == 1);   // 一次 bind 都没发
    rp_perf_take(pf); CHECK(pf[RP_PERF_BIND_FAST] == 3);              // 三个阶段都整组沿用
    memcpy(s.b + o2, sv, 8);
    memcpy(sv, s.b + o3, 8); memcpy(s.b + o3, &eh, 8);
    rp_perf_take(pf);
    nvn_be_exec(s.b + o2);                                            // ③
    CHECK(g_gdraws == 3 && g_bind_calls[0] == 2 && g_bind_calls[1] == 1 && g_bind_calls[2] == 1);   // 只有顶点阶段重建
    rp_perf_take(pf);
    CHECK(pf[RP_PERF_BIND_FAST] == 2);                          // 没变的两阶段整组沿用
    CHECK(pf[RP_PERF_BIND_SLICE_HIT] == 2);                     // DRV0 / DRV2 内容没变：命中上次那片
    memcpy(s.b + o3, sv, 8);
    rp_perf_take(pf);
    nvn_be_exec(s.b + o3);                                            // ④
    CHECK(g_gdraws == 4 && g_bind_calls[0] == 2 && g_bind_calls[1] == 2 && g_bind_calls[2] == 1);   // 只有片元阶段重建
    rp_perf_take(pf);
    CHECK(pf[RP_PERF_BIND_FAST] == 2);                          // 没变的两阶段整组沿用
    CHECK(pf[RP_PERF_BIND_SLICE_HIT] == 0);                     // 贴图句柄表变了：驱动段必须重做
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_tex_release(&rtt); nvn_be_tex_release(&tx); nvn_be_tex_release(&ty); nvn_be_pool_del(g_pmem);
    pass_fresh();
}

// ---------- ③ B2 换管线按布局立脏（rp_draw.c）----------
// 用的是「换管线之后这一笔被裁剪掉」的路径：绘制没发出，rp_args_dirty 留在原地，正好核对换管线时立了什么脏。
//   ① 基线（程序 A）；
//   ② 换成程序对象 B（布局与 A 相同）：布局等同 → 一个阶段的绑定都不立脏；
//   ③ 把 B 的顶点布局改出差异：只有顶点阶段立脏，片元 / 几何不动。
static void t_res_pipe_layout(void) {
    static struct rp_prog prog2;
    pass_fresh(); res_fresh();
    g_resolve_rc = 0; g_alt_prog = NULL; g_gdraws = 0;
    res_prog_min();
    prog2 = g_fprog;
    prog2.sh[0] = &g_fsh[2];   // 另一个程序对象（着色器不同、布局相同）
    tex_t rtt = mk_tex(920, 0, NVN_FORMAT_RGBA8, 64, 36, 1, 0);
    stream s = {0};
    put_set_rt(&s, &rtt, NULL);
    p_draw *dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3; dn->instances = 1;
    size_t o1 = s.n;
    p_scissor *sc = put(&s, OP_SCISSOR, sizeof *sc); sc->x = 1000; sc->y = 0; sc->w = 10; sc->h = 10;   // 整个在目标外：这一笔画不出东西
    dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3; dn->instances = 1;
    size_t o2 = s.n;
    dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3; dn->instances = 1;
    end(&s);
    nvn_be_sub_begin();
    const op_hdr eh = {OP_END, 8}; uint8_t sv[8];
    memcpy(sv, s.b + o1, 8); memcpy(s.b + o1, &eh, 8);
    nvn_be_exec(s.b);                                 // ① 基线：画成功，脏位清零
    CHECK(g_gdraws == 1 && rp_args_dirty[0] == 0 && rp_args_dirty[1] == 0);
    int d0 = g_gdraws;
    memcpy(s.b + o1, sv, 8);
    memcpy(sv, s.b + o2, 8); memcpy(s.b + o2, &eh, 8);
    g_alt_prog = &prog2;
    rp_pso_dirty = 1;                                 // 强制换一次管线（布局判定的用例）
    nvn_be_exec(s.b + o1);                            // ② 换程序对象、布局等同
    CHECK(g_gdraws == d0 && rp_args_dirty[0] == 0 && rp_args_dirty[1] == 0 && rp_args_dirty[2] == 0);
    memcpy(s.b + o2, sv, 8);
    prog2.nent[0] = 1; prog2.ent[0][0].cls = NVN_GPU_RES_UBO; prog2.ent[0][0].src_kind = NVN_RB_CBUF; prog2.ent[0][0].src_reg = 2;
    rp_pso_dirty = 1;
    nvn_be_exec(s.b + o2);                            // ③ 布局变了：只有顶点阶段立脏
    CHECK(g_gdraws == d0 && rp_args_dirty[0] == 1 && rp_args_dirty[1] == 0 && rp_args_dirty[2] == 0);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    g_alt_prog = NULL; nvn_be_tex_release(&rtt);
    pass_fresh();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    t_res_tex_epoch();
    t_res_tex_epoch_unrelated();
    t_res_bind_fast();
    t_res_pipe_layout();
    printf("t_nvn_res：%d 项检查，%d 项红\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
