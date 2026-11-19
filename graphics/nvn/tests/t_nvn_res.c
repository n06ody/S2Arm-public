// 回放核心的资源池与绑定构造单测：与 t_nvn_replay.c 共用同一份替身（重命名它的 main 后 include）。
// 覆盖两块改动：
//   ① 贴图内容检查按「全局写代号 + 段号 + 映射代际」只查一次（rp_res.c）；
//   ② 绑定构造省重（rp_draw.c）：B1 整组沿用上一笔、B3 驱动段按内容命中、B2 换管线按布局立脏。
// 期望值都由用例自己写下的输入直接得出，并经 rp_perf_take / 替身计数核对；带负对照。
#define main replay_fixture_main
#define rp_shader_resolve fixture_shader_resolve
#define nvn_plat_flag fixture_plat_flag
#include "t_nvn_replay.c"
#undef nvn_plat_flag
#undef rp_shader_resolve
#undef main
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

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

// ---------- ④ 贴图存储伸出内存池（rp_res.c tex_window / tex_src）----------
// 真机（丝之歌等多款 Unity 游戏，苹果端红行「storage = pool + offset 0xffffffffffffffff」）：窗口那张 1920x1080 贴图的存储偏移是 -1，
//   存储第一个字节落在池前一页，那页游戏没映射。用例照搬这个形态：池前一页 PROT_NONE，池内偏移 i 的字节 = pool_byte(i)（从不为 0）。
//   改前：判内容扫全段 / 上传反交织都读池前那一字节，段错误。改后：池外字节按 0（同参照实现「读没映射的页得 0」），池内照取；写跟踪只罩池内那段。
// 期望值不经被测实现：存储偏移 s 的字节 = s 在池外 ? 0 : pool_byte(s - 1)；第 0 行第 x 字节在 GOB 里的偏移按公开 GOB 公式（y = 0）= (x%64/32)·256 + (x%32/16)·32 + x%16。
static uint8_t pool_byte(uint64_t i) { return (uint8_t)(i % 251 + 1); }
static uint8_t store_byte(uint64_t s) { return s == 0 ? 0 : pool_byte(s - 1); }   // 偏移 -1：存储偏移 0 在池外
static uint64_t gob_x(uint32_t x) { return (uint64_t)(x % 64 / 32) * 256 + (x % 32 / 16) * 32 + x % 16; }
static void t_res_tex_before_pool(void) {
    nvn_be_sub_begin(); res_fresh();
    const size_t pg = (size_t)getpagesize();
    uint8_t *m = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(m != MAP_FAILED);
    if (m == MAP_FAILED) return;
    CHECK(mprotect(m, pg, PROT_NONE) == 0);   // 池前一页：游戏没映射
    pool_t pool; memset(&pool, 0, sizeof pool); pool.mem = m + pg; pool.size = 2 * pg; pool.flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
    for (size_t i = 0; i < pool.size; i++) pool.mem[i] = pool_byte(i);
    // A：游戏用 CPU 写过（同真机 ser=3 被判 CPU 写过）→ 整张上传
    tex_t a = mk_tex(7300, 0, NVN_FORMAT_RGBA8, 16, 8, 1, 0); a.pool = &pool; a.off = -1; a.mem = pool.mem - 1;
    CHECK(a.L.size >= 512 && a.L.size <= pool.size);
    nvn_be_tex_cpu_written(&a);
    int n0 = g_nup;
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && g_nup == n0 + 1);
    CHECK(g_up[n0].pitch == 64 && g_up[n0].w == 16 && g_up[n0].h == 8);
    { int ok = 1; for (uint32_t x = 0; x < 64; x++) ok &= g_up[n0].first[x] == store_byte(gob_x(x));
      CHECK(ok); CHECK(g_up[n0].first[0] == 0 && g_up[n0].first[1] == pool_byte(0) && g_up[n0].first[16] == pool_byte(31) && g_up[n0].first[32] == pool_byte(255)); }
    CHECK(g_prot_addr == pool.mem && g_prot_len == a.L.size - 1);   // 写跟踪罩的是池内那段：池外字节恒为 0，不用跟踪
    nvn_be_tex_release(&a);
    // B：没标过 CPU 写：首用判「存储里有没有 CPU 数据」要扫存储，同样只扫池内那段（池内非 0 → 当 CPU 贴图上传）
    res_fresh();
    tex_t b = mk_tex(7301, 0, NVN_FORMAT_RGBA8, 16, 8, 1, 0); b.pool = &pool; b.off = -1; b.mem = pool.mem - 1;
    n0 = g_nup;
    CHECK(rp_tex_resolve(&b, &g_fake_cmd) != NULL && g_nup == n0 + 1 && g_up[n0].first[0] == 0 && g_up[n0].first[1] == pool_byte(0));
    nvn_be_tex_release(&b);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    munmap(m, 3 * pg);
}

// GPU 后端替身在入口内复制原始字节，再由 CPU 参考解排；两路均逐字对照手算 GOB。
// 池首和池尾各放一页 PROT_NONE，子进程让越界访问变成有名断言，而非中止整套。
static void t_res_tex_window_gpu_child(int tail) {
    int fail0 = g_fail; nvn_be_sub_begin(); res_fresh();
    const size_t pg = (size_t)getpagesize();
    uint8_t *m = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(m != MAP_FAILED); if (m == MAP_FAILED) _exit(2);
    CHECK(mprotect(m, pg, PROT_NONE) == 0 && mprotect(m + 2 * pg, pg, PROT_NONE) == 0);
    pool_t pool = {0}; pool.mem = m + pg; pool.size = pg; pool.flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
    for (size_t i = 0; i < pg; i++) pool.mem[i] = pool_byte(i);
    tex_t a = mk_tex(7310 + (unsigned)tail * 2, 0, NVN_FORMAT_RGBA8, 16, 8, 1, 0);
    a.pool = &pool; a.off = tail ? (int64_t)pg - 256 : -1; a.mem = pool.mem + a.off; a.has_layout = 1;
    CHECK(a.L.size == 512); g_window_capture = 1; g_caps_extra &= ~NVN_GPU_CAP_GPU_DESWIZZLE;
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && g_nup == 1);
    uint8_t cpu[512], expected[512]; memcpy(cpu, g_window_linear, sizeof cpu);
    for (unsigned y = 0; y < 8; y++) for (unsigned x = 0; x < 64; x++) {
        uint64_t s = (x / 32) * 256 + (y / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16;
        expected[y * 64 + x] = tail ? (s < 256 ? pool_byte(pg - 256 + s) : 0) : store_byte(s);
    }
    CHECK(!memcmp(cpu, expected, sizeof cpu)); nvn_be_tex_release(&a);
    res_fresh(); a.serial++; g_block_uploads = 0; g_caps_extra |= NVN_GPU_CAP_GPU_DESWIZZLE;
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && g_block_uploads == 1 && g_nup == 0);
    uint8_t decoded[512]; CHECK(!nvn_deswizzle(&a.d, 0, 0, g_window_raw, decoded, 64));
    CHECK(!memcmp(decoded, expected, sizeof decoded)); CHECK(!memcmp(decoded, cpu, sizeof decoded));
    CHECK(g_prot_addr == pool.mem + (tail ? pg - 256 : 0) && g_prot_len == (tail ? 256 : 511));
    nvn_be_tex_release(&a); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); munmap(m, 3 * pg);
    printf("GPU_WINDOW %s: CPU/GPU-contract bytes=512 failures=%d\n", tail ? "tail" : "before", g_fail - fail0); fflush(stdout);
    _exit(g_fail != fail0);
}
static void t_res_tex_window_gpu(void) {
    for (int tail = 0; tail < 2; tail++) {
        pid_t child = fork(); CHECK(child >= 0); if (child < 0) continue;
        if (!child) t_res_tex_window_gpu_child(tail);
        int status = 0; CHECK(waitpid(child, &status, 0) == child);
        if (WIFSIGNALED(status)) printf("GPU_WINDOW %s source access signal=%d\n", tail ? "tail" : "before", WTERMSIG(status));
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--gpu-window")) { t_res_tex_window_gpu(); return g_fail ? 1 : 0; }
    t_res_tex_epoch();
    t_res_bind_fast();
    t_res_pipe_layout();
    t_res_tex_before_pool();
    t_res_tex_window_gpu();
    printf("t_nvn_res：%d 项检查，%d 项红\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
