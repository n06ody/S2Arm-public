// 存储前瞻索引（rp_sdx.c）宿主用例：真 rp_sdx / rp_state / rp_exec / rp_pass / rp_draw / rp_res / rp_buf / rp_present 等，
// 后端与平台件用 t_nvn_replay.c 的替身（同 nvk_pipe_async_draw_test 的套路），程序 peek 用本文件的桩。
// 用例：正例（整屏清除 → store DONTCARE）、各 why 反例（续接 ATT / 回读 / 拷贝 / 无事件 / 判不到 → STORE）、
//   索引作废（invalidate / 序号对不上 / nvk_sdx 关）。
// 运行：rp_sdx_test main | off（开关只读一次：两组值各一个进程）。壳脚本另跑负对照（S2A_SDX_NEG 改一处判据，期望红）。
#define main replay_fixture_main
#define nvn_plat_flag fixture_plat_flag
#include "../../graphics/nvn/tests/t_nvn_replay.c"
#undef nvn_plat_flag
#undef main

static int g_sdx_flag = 1;
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_sdx") || !strcmp(name, "nvk_fd")) return g_sdx_flag;   // off 进程同关两个优化
    return fixture_plat_flag(name, dflt);
}

// peek 桩：命中时给一份带「fs 读贴图槽 3」反射的程序；不命中（NULL）时前瞻按全槽读保守记。
static struct rp_prog g_peek_prog; static int g_peek_on;
const struct rp_prog *rp_shader_peek(uint64_t ser) { (void)ser; return g_peek_on ? &g_peek_prog : NULL; }

static void sdx_fresh(void) {   // 每用例：会话 + 通道观察归零；开 TBDR 能力位（sdx_on 的门槛，同真机 Mali/Adreno）
    pass_fresh();
    g_caps_extra = NVN_GPU_CAP_TILE_MEMORY;
    memset(&g_peek_prog, 0, sizeof g_peek_prog);
    g_peek_prog.pos_attr = -1; g_peek_prog.may_discard = 1; g_peek_prog.writes_smask = 1;
    g_peek_on = 0;
    g_snap_prog = NULL; g_snap_ok = 0;
}
static void q_bind_tex(stream *s, int stage, int first, int n, int reg) {   // reg = 贴图登记号（nvn_be_reg_tex 的 id）；池号随意
    p_bind_handles *b = put(s, OP_BIND_TEX, sizeof *b); b->stage = stage; b->first = first; b->n = n; b->h[0] = (uint64_t)reg | 3ull << 20;
}
static void q_bind_prog(stream *s, prog_t *p, uint64_t serial) {
    p_bind_program_id *id = put(s, OP_BIND_PROGRAM, sizeof *id); id->prog = p; id->serial = serial; id->stages = 0x3;
    g_snap_prog = p; g_snap_ok = 0x3; memset(g_snap_stage, 0xff, sizeof g_snap_stage); g_snap_stage[0] = 0; g_snap_stage[1] = 1;
}

// 正例：A 画一笔 → 换 B 画 → 回 A 整屏清 → 再画。A 的第一个通道结束后被整屏覆盖：store = DONTCARE。
static void t_dontcare_after_clear(void) {
    sdx_fresh();
    static tex_t ta, tb;
    ta = mk_tex(300001, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&ta);
    tb = mk_tex(300002, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&tb);
    stream s = {0};
    put_set_rt(&s, &ta, NULL); q_draw(&s); put_set_rt(&s, &tb, NULL); q_draw(&s); put_set_rt(&s, &ta, NULL);
    { p_clear_color *c = put(&s, OP_CLEAR_COLOR, sizeof *c); c->index = 0; c->mask = 0xf; }
    q_draw(&s); end(&s); submit1(&s);
    CHECK(g_npd == 3);
    CHECK(g_pd[0].color[0].store == NVN_GPU_STORE_DONTCARE);   // 第一个通道的内容随后被整屏清掉：冗余
    CHECK(g_pd[1].color[0].store == NVN_GPU_STORE_STORE);     // B 的通道：之后再无事件 → 存
    CHECK(g_pd[2].color[0].store == NVN_GPU_STORE_STORE);     // 清除后的绘制内容无人再覆盖 → 存
}

// 反例（同目标续接）：SET_RT(A) DRAW SET_RT(A) DRAW ——续接不结束通道，但前瞻上第二笔绘制 rtseq>第一：必须判 STORE。
static void t_cont_att_stores(void) {
    sdx_fresh();
    static tex_t ta;
    ta = mk_tex(300101, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&ta);
    stream s = {0};
    put_set_rt(&s, &ta, NULL); q_draw(&s); put_set_rt(&s, &ta, NULL); q_draw(&s); end(&s); submit1(&s);
    CHECK(g_npd >= 1 && g_pd[0].color[0].store == NVN_GPU_STORE_STORE);   // ATT@rtseq>R0 可能就在本通道里（续接）→ 存
}

// 反例（回读）：A 当附件画 → 换 B 目标并绑 A 为片元贴图 → 程序反射证明 fs 读槽 3（=A）。前瞻命中时 A 的通道必须存。
static void t_read_stores(void) {
    sdx_fresh();
    static tex_t ta, tb;
    ta = mk_tex(300201, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&ta); nvn_be_reg_tex(11, &ta, NULL);
    tb = mk_tex(300202, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&tb);
    g_peek_on = 1;
    g_peek_prog.nent[1] = 1; g_peek_prog.ent[1][0].src_kind = NVN_RB_TEX; g_peek_prog.ent[1][0].src_reg = 3;   // fs 的贴图槽 3
    { static prog_t pr; memset(&pr, 0, sizeof pr); pr.magic = MAGIC('P', 'R', 'O', 'G'); pr.serial = 77; pr.nstage = 2;
      stream s = {0};
      q_bind_prog(&s, &pr, 77);
      put_set_rt(&s, &ta, NULL); q_draw(&s);
      put_set_rt(&s, &tb, NULL); q_bind_tex(&s, 1, 3, 1, 11); q_draw(&s);
      end(&s); submit1(&s); }
    CHECK(g_npd == 2);
    CHECK(g_pd[0].color[0].store == NVN_GPU_STORE_STORE);   // A 随后被 B 通道的片元读 → 存
}

// 反例（peek 不中）：同一形状但程序没编译出来——前瞻把绑着的槽全按「可能被读」记，A 的通道照样存。
static void t_read_stores_peekmiss(void) {
    sdx_fresh();
    static tex_t ta, tb;
    ta = mk_tex(300301, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&ta); nvn_be_reg_tex(13, &ta, NULL);
    tb = mk_tex(300302, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&tb);
    g_peek_on = 0;
    { static prog_t pr; memset(&pr, 0, sizeof pr); pr.magic = MAGIC('P', 'R', 'O', 'G'); pr.serial = 78; pr.nstage = 2;
      stream s = {0};
      q_bind_prog(&s, &pr, 78);
      put_set_rt(&s, &ta, NULL); q_draw(&s);
      put_set_rt(&s, &tb, NULL); q_bind_tex(&s, 1, 3, 1, 13); q_draw(&s);
      end(&s); submit1(&s); }
    CHECK(g_npd == 2 && g_pd[0].color[0].store == NVN_GPU_STORE_STORE);
}

// 反例（拷贝消费）：A 当附件画 → A→B 纹理拷贝（NEED 在索引里）→ A 的通道必须存。
static void t_copy_stores(void) {
    sdx_fresh();
    static tex_t ta, tb;
    ta = mk_tex(300401, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&ta);
    tb = mk_tex(300402, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&tb);
    stream s = {0};
    put_set_rt(&s, &ta, NULL); q_draw(&s);
    { p_copy_t2t *c = put(&s, OP_COPY_T2T, sizeof *c); c->src = &ta; c->dst = &tb; }
    end(&s); submit1(&s);
    CHECK(g_npd >= 1 && g_pd[0].color[0].store == NVN_GPU_STORE_STORE);
}

// 索引作废路径（不调回放，直接调索引接口）：序号对不上 / invalidate / 没开始回放，一律判不到 → STORE 方向。
static void t_invalidate_paths(void) {
    sdx_fresh();
    static tex_t ta;
    ta = mk_tex(300501, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&ta);
    stream s = {0};
    put_set_rt(&s, &ta, NULL); q_draw(&s); put_set_rt(&s, &ta, NULL);
    { p_clear_color *c = put(&s, OP_CLEAR_COLOR, sizeof *c); c->index = 0; c->mask = 0xf; }
    end(&s);
    uint64_t h = (uint64_t)(uintptr_t)s.b;
    nvn_be_sub_index(&h, 1);
    CHECK(rp_sdx_decide(ta.serial, 0, 0, 0, 0) == 6 << 2);   // 还没开始回放（g_exec_seq=0）：判不到
    rp_sdx_seq((const uint8_t *)s.b);                        // 对上第一条（SET_RT）
    rp_sdx_seq((const uint8_t *)s.b + 0x80);                 // 错指针：索引作废
    CHECK(rp_sdx_decide(ta.serial, 0, 0, 0, 0) == 6 << 2);
    nvn_be_sub_index(&h, 1);
    rp_sdx_invalidate();                                     // 条件渲染真跳过：作废
    CHECK(rp_sdx_decide(ta.serial, 0, 0, 0, 0) == 6 << 2);
    rp_sdx_reset();
}

// 条件渲染真跳过（真机全零嫌疑之一）：先丢一笔（invalidate 在这笔处发），恢复后走正例形状
// （A 画 → 回 A 整屏清 → 再画）。索引已死：本来能判 DONTCARE 的通道只能 STORE。
static void t_rcond_invalidate(void) {
    sdx_fresh(); g_rcond = 1;
    static tex_t ta;
    ta = mk_tex(300801, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&ta);
    stream s = {0};
    { p_render_cond *rc = put(&s, OP_RENDER_COND, sizeof *rc); rc->mode = 3; }
    q_draw(&s);   // 被丢：索引里这条序号有事件、回放没有 → 作废
    { p_render_cond *rc = put(&s, OP_RENDER_COND, sizeof *rc); rc->mode = 2; }   // 恢复
    put_set_rt(&s, &ta, NULL); q_draw(&s); put_set_rt(&s, &ta, NULL);
    { p_clear_color *c = put(&s, OP_CLEAR_COLOR, sizeof *c); c->index = 0; c->mask = 0xf; }
    q_draw(&s); end(&s); submit1(&s);
    g_rcond = 0;
    CHECK(g_npd >= 1 && g_pd[0].color[0].store == NVN_GPU_STORE_STORE);
}

// nvk_sdx 关：索引根本不建，通道全 STORE。只在 off 进程跑（开关进程内只读一次）。
static void t_disabled(void) {
    sdx_fresh();
    static tex_t ta;
    ta = mk_tex(300601, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&ta);
    stream s = {0};
    put_set_rt(&s, &ta, NULL); q_draw(&s); put_set_rt(&s, &ta, NULL);
    { p_clear_color *c = put(&s, OP_CLEAR_COLOR, sizeof *c); c->index = 0; c->mask = 0xf; }
    q_draw(&s); end(&s); submit1(&s);
    CHECK(g_npd >= 1 && g_pd[0].color[0].store == NVN_GPU_STORE_STORE);
}

// ================= 首笔全画证明（rp_draw.c fd_*）=================
// 顶点数据放本文件的静态池：nvn_gva_pool_of 桩把它报成一个池（回放核心里是弱符号，宿主没链 hle_nvn.c）。
static float g_pool[4 * 4];
int nvn_gva_pool_of(uint64_t ca, uint64_t *cpu_base, uint64_t *bytes) {
    const uint64_t b = (uint64_t)(uintptr_t)g_pool;
    if (ca >= b && ca < b + sizeof g_pool) { *cpu_base = b; *bytes = sizeof g_pool; return 1; }
    return 0;
}

static void fd_state(stream *s, int bad_vb) {   // 全画证明要求的整套状态：全屏视口、写掩码满、FILL、不测试、属性 0 = RGBA32F@流 0
    p_bind_state *by;
    by = put(s, OP_BIND_STATE, sizeof *by); by->kind = 1; { color_st cs = { .blend_en = 0, .logic_op = NVN_LOGIC_OP_COPY, .alpha_func = NVN_ALPHA_FUNC_ALWAYS }; memcpy(by->bytes, &cs, sizeof cs); }
    by = put(s, OP_BIND_STATE, sizeof *by); by->kind = 2; { chmask_st cm = { .mask = 0xf }; memcpy(by->bytes, &cm, sizeof cm); }
    by = put(s, OP_BIND_STATE, sizeof *by); by->kind = 4; { poly_st po = { .cull = 0, .front = NVN_FRONT_FACE_CCW, .mode = NVN_POLYGON_MODE_FILL }; memcpy(by->bytes, &po, sizeof po); }
    by = put(s, OP_BIND_STATE, sizeof *by); by->kind = 5; { ds_st dz = {0}; memcpy(by->bytes, &dz, sizeof dz); }
    p_bind_vattr *va = put(s, OP_BIND_VATTR, sizeof *va); va->n = 1; { va_st at = { .fmt = NVN_FORMAT_RGBA32F, .offset = 0, .stream = 0 }; memcpy(&va->st[0], &at, 4); }
    p_bind_vstream *vs = put(s, OP_BIND_VSTREAM, sizeof *vs); vs->n = 1; vs->st[0].stride = 16;
    p_bind_vbuf_single *vb = put(s, OP_BIND_VBUF, sizeof *vb); vb->size = sizeof g_pool; vb->addr = bad_vb ? 0x1000 : (uint64_t)(uintptr_t)g_pool - 0x1000;   // 记录是 GPU 地址，替身换算 +0x1000
    p_viewports *vp = put(s, OP_VIEWPORTS, sizeof *vp); vp->n = 1; vp->v[0][2] = 64; vp->v[0][3] = 64; vp->v[0][5] = 1;
}
static void fd_fresh(const float verts[3][4]) {   // 程序事实全开（pos_attr=0、w=1、无 discard / smask）；顶点摆进池
    sdx_fresh();
    memset(&g_fprog, 0, sizeof g_fprog);
    for (int st = 0; st < RP_PROG_STAGES; ++st) for (int i = 0; i < RP_PROG_ENT_MAX; ++i) g_fprog.ent[st][i].src_reg2 = UINT16_MAX;
    g_fprog.sh[0] = &g_fsh[0]; g_fprog.sh[1] = &g_fsh[1]; g_fprog.color_out_mask = 0xffffffffu;
    g_fprog.pos_src = 1; g_fprog.pos_attr = 0; g_fprog.pos_w1 = 1; g_fprog.may_discard = 0; g_fprog.writes_smask = 0;
    memcpy(g_pool, verts, sizeof(float[3][4]));
}
// ta 画一遍（待清零标记被吃掉）→ 换 tb → 回 ta 再画：第三个通道（g_pd[2]）是「载入非零旧内容」的那次，全画证明生效就 DONTCARE。
static void fd_run3(int bad_vb) {
    static tex_t ta, tb;
    ta = mk_tex(300701, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&ta);
    tb = mk_tex(300702, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&tb);
    stream s = {0};
    fd_state(&s, bad_vb);
    put_set_rt(&s, &ta, NULL); q_draw(&s); put_set_rt(&s, &tb, NULL); q_draw(&s); put_set_rt(&s, &ta, NULL); q_draw(&s);
    end(&s); submit1(&s);
}
static const float FD_FULL[3][4] = {{-1,-1,.5f,1},{3,-1,.5f,1},{-1,3,.5f,1}};   // 盖满 NDC 方块的三角形
static const float FD_SMALL[3][4] = {{-.1f,-.1f,.5f,1},{.1f,-.1f,.5f,1},{-.1f,.1f,.5f,1}};

// 编译期求值表（pos_src=3 的证据形态）：vid k 的出口位置 vec4 位模式
static void fd_vtab(const float verts[][4], int n) {
    memset(g_fprog.pos_vtab, 0, sizeof g_fprog.pos_vtab);
    for (int k = 0; k < n; k++) for (int c = 0; c < 4; c++) { float f = verts[k][c]; memcpy(&g_fprog.pos_vtab[k * 4 + c], &f, 4); }
}
static void t_fd_vid_positive(void) {   // 序号生成：位置全在表里——顶点缓冲故意指到池外也证出（vidgen 不读顶点数据）
    fd_fresh(FD_FULL);
    g_fprog.pos_src = 3; g_fprog.pos_attr = -1; fd_vtab(FD_FULL, 3);
    fd_run3(1);
    CHECK(g_npd >= 3 && g_pd[2].color[0].load == NVN_GPU_LOAD_DONTCARE);
}
static void t_fd_neg_vid_small(void) {   // 表在但摆出来盖不满：证不出，载（表错不等于无脑跳载入）
    fd_fresh(FD_FULL);
    g_fprog.pos_src = 3; g_fprog.pos_attr = -1; fd_vtab(FD_SMALL, 3);
    fd_run3(0);
    CHECK(g_npd >= 3 && g_pd[2].color[0].load == NVN_GPU_LOAD_LOAD);
}
static void t_fd_neg_src_const(void) {   // 编译期常量位置（src=2）：全顶点同点退化，永不盖满
    fd_fresh(FD_FULL);
    g_fprog.pos_src = 2; g_fprog.pos_attr = -1;
    fd_run3(0);
    CHECK(g_npd >= 3 && g_pd[2].color[0].load == NVN_GPU_LOAD_LOAD);
}
static void t_fd_neg_src_unknown(void) {   // src=0（complex）：来源证不出，载
    fd_fresh(FD_FULL);
    g_fprog.pos_src = 0; g_fprog.pos_attr = -1;
    fd_run3(0);
    CHECK(g_npd >= 3 && g_pd[2].color[0].load == NVN_GPU_LOAD_LOAD);
}

static void t_fd_positive(void) {
    fd_fresh(FD_FULL); fd_run3(0);
    CHECK(g_npd >= 3 && g_pd[2].color[0].load == NVN_GPU_LOAD_DONTCARE);
}
static void t_fd_neg_small(void) {   // 不盖满：证不出，载
    fd_fresh(FD_SMALL); fd_run3(0);
    CHECK(g_npd >= 3 && g_pd[2].color[0].load == NVN_GPU_LOAD_LOAD);
}
static void t_fd_neg_nofacts(void) {   // 编译器没给事实：pos_attr=-1 是缺省，载
    fd_fresh(FD_FULL); g_fprog.pos_attr = -1; fd_run3(0);
    CHECK(g_npd >= 3 && g_pd[2].color[0].load == NVN_GPU_LOAD_LOAD);
}
static void t_fd_neg_discard(void) {   // 片元能丢：写不满本槽，载
    fd_fresh(FD_FULL); g_fprog.may_discard = 1; fd_run3(0);
    CHECK(g_npd >= 3 && g_pd[2].color[0].load == NVN_GPU_LOAD_LOAD);
}
static void t_fd_neg_blend(void) {   // 本槽开混合：读旧像素，载
    fd_fresh(FD_FULL);
    static tex_t ta, tb;
    ta = mk_tex(300711, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&ta);
    tb = mk_tex(300713, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&tb);
    stream s = {0}; fd_state(&s, 0);
    p_bind_state *by = put(&s, OP_BIND_STATE, sizeof *by); by->kind = 1; { color_st cs = { .blend_en = 1, .logic_op = NVN_LOGIC_OP_COPY, .alpha_func = NVN_ALPHA_FUNC_ALWAYS }; memcpy(by->bytes, &cs, sizeof cs); }
    put_set_rt(&s, &ta, NULL); q_draw(&s); put_set_rt(&s, &tb, NULL); q_draw(&s); put_set_rt(&s, &ta, NULL); q_draw(&s);
    end(&s); submit1(&s);
    CHECK(g_npd >= 3 && g_pd[2].color[0].load == NVN_GPU_LOAD_LOAD);
}
static void t_fd_neg_outofpool(void) {   // 顶点缓冲不在池里：CPU 读不到真内容，载
    fd_fresh(FD_FULL); fd_run3(1);
    CHECK(g_npd >= 3 && g_pd[2].color[0].load == NVN_GPU_LOAD_LOAD);
}
static void t_fd_neg_scissor(void) {   // 裁剪框缩过：画不满整张，载
    fd_fresh(FD_FULL);
    static tex_t ta, tb;
    ta = mk_tex(300712, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&ta);
    tb = mk_tex(300714, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&tb);
    stream s = {0}; fd_state(&s, 0);
    p_scissor *sc = put(&s, OP_SCISSOR, sizeof *sc); sc->w = 32; sc->h = 64;
    put_set_rt(&s, &ta, NULL); q_draw(&s); put_set_rt(&s, &tb, NULL); q_draw(&s); put_set_rt(&s, &ta, NULL); q_draw(&s);
    end(&s); submit1(&s);
    CHECK(g_npd >= 3 && g_pd[2].color[0].load == NVN_GPU_LOAD_LOAD);
}

// ---- 诊断归因（r6）：判据8「自拆」子桶 + 0–1 笔通道「单笔」判据桶分布 ----
static int fd_diag_grab(char *db, size_t n, unsigned long long sub[4], unsigned long long ty[16]) {   // rp_fd_diag 一行里解析两段计数；先调一次清累计
    int dl = rp_fd_diag(db, n); if (dl <= 0) return 0;
    char *p = strstr(db, "自拆：真");
    if (!p || sscanf(p, "自拆：真%llu 解%llu 数%llu 空%llu", &sub[0], &sub[1], &sub[2], &sub[3]) != 4) return 0;
    p = strstr(db, "单笔：证");
    if (!p || sscanf(p, "单笔：证%llu 关%llu 程%llu 弃%llu 位%llu 丢%llu 掩%llu 采%llu 数%llu 型%llu 自%llu 形%llu 逻%llu 视%llu 顶%llu 零%llu",
            &ty[0], &ty[1], &ty[2], &ty[3], &ty[4], &ty[14], &ty[15], &ty[5], &ty[6], &ty[7], &ty[8], &ty[9], &ty[10], &ty[11], &ty[12], &ty[13]) != 16) return 0;
    return 1;
}
static void t_fd_diag_tiny(void) {   // fd_run3 的三个通道都是单笔：开通道笔全落「位」桶，要原样回记
    char db[1024]; unsigned long long sub[4], ty[16];
    rp_fd_diag(db, sizeof db);
    fd_fresh(FD_FULL); g_fprog.pos_src = 0; g_fprog.pos_attr = -1; fd_run3(0);
    CHECK(fd_diag_grab(db, sizeof db, sub, ty) && ty[4] == 3 && !(sub[0] | sub[1] | sub[2] | sub[3]));
}
static void t_fd_diag_self_serial(void) {   // serial 真命中：fs 槽 3 绑的就是当前目标 → 自拆「真」、单笔「自」，载
    char db[1024]; unsigned long long sub[4], ty[16];
    rp_fd_diag(db, sizeof db);
    fd_fresh(FD_FULL);
    static tex_t ta;
    ta = mk_tex(300721, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&ta); nvn_be_reg_tex(21, &ta, NULL);
    samp_par sp; memset(&sp, 0, sizeof sp); nvn_be_reg_samp(3, &sp);   // 绑定走得到底：采这张贴图的采样器登记上
    g_fprog.nent[1] = 1; g_fprog.ent[1][0].src_kind = NVN_RB_TEX; g_fprog.ent[1][0].src_reg = 3; g_fprog.ent[1][0].count = 1;
    stream s = {0}; fd_state(&s, 0);
    put_set_rt(&s, &ta, NULL); q_bind_tex(&s, 1, 3, 1, 21); q_draw(&s);
    end(&s); submit1(&s);
    CHECK(g_npd >= 1 && g_pd[0].color[0].load == NVN_GPU_LOAD_LOAD);
    CHECK(fd_diag_grab(db, sizeof db, sub, ty) && sub[0] == 1 && sub[1] + sub[2] + sub[3] == 0 && ty[8] == 1);
}
static void t_fd_diag_self_unres(void) {   // IMGU 句柄在常量缓冲：判定点解不到 → 自拆「解」，载（夹具里 TEXU 在 fd 判定前的 usrc 解析就跳了，IMGU 同子桶且能走到判定）
    char db[1024]; unsigned long long sub[4], ty[16];
    rp_fd_diag(db, sizeof db);
    fd_fresh(FD_FULL);
    static tex_t ta;
    ta = mk_tex(300723, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&ta); nvn_be_reg_tex(23, &ta, NULL);
    g_fprog.nent[1] = 1; g_fprog.ent[1][0].src_kind = NVN_RB_IMGU; g_fprog.ent[1][0].count = 1;
    stream s = {0}; fd_state(&s, 0);
    put_set_rt(&s, &ta, NULL); q_draw(&s);
    end(&s); submit1(&s);
    CHECK(g_npd >= 1 && g_pd[0].color[0].load != NVN_GPU_LOAD_DONTCARE);   // 新贴图带待清零标记 → 首通道 load=CLEAR 也算「fd 没证出」
    CHECK(fd_diag_grab(db, sizeof db, sub, ty) && sub[1] == 1 && ty[8] == 1);
}
static void t_fd_diag_self_array(void) {   // 句柄表数组（count>1）：判定点不逐项解 → 自拆「数」，载（槽位绑真贴图只为让笔走完判定点之后的绑定）
    char db[1024]; unsigned long long sub[4], ty[16];
    rp_fd_diag(db, sizeof db);
    fd_fresh(FD_FULL);
    static tex_t ta, tc;
    ta = mk_tex(300725, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&ta); nvn_be_reg_tex(25, &ta, NULL);
    tc = mk_tex(300727, 0, NVN_FORMAT_RGBA8, 64, 64, 1, 0); nvn_be_tex_init(&tc); nvn_be_reg_tex(24, &tc, NULL);
    samp_par sp; memset(&sp, 0, sizeof sp); nvn_be_reg_samp(3, &sp);
    g_fprog.nent[1] = 1; g_fprog.ent[1][0].src_kind = NVN_RB_TEX; g_fprog.ent[1][0].src_reg = 4; g_fprog.ent[1][0].count = 2;   // 数组槽 0/1
    stream s = {0}; fd_state(&s, 0);
    put_set_rt(&s, &ta, NULL); q_bind_tex(&s, 1, 0, 1, 24); q_bind_tex(&s, 1, 1, 1, 24); q_draw(&s);
    end(&s); submit1(&s);
    CHECK(g_npd >= 1 && g_pd[0].color[0].load != NVN_GPU_LOAD_DONTCARE);   // 新贴图带待清零标记 → 首通道 load=CLEAR 也算「fd 没证出」
    CHECK(fd_diag_grab(db, sizeof db, sub, ty) && sub[2] == 1 && ty[8] == 1);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "off")) {
        g_sdx_flag = 0;
        t_disabled();
        printf("rp_sdx(off): %d checks, %d failures\n", g_checks, g_fail);
        return g_fail != 0;
    }
    if (argc > 1 && !strcmp(argv[1], "capsoff")) {   // 真机 r2 形态：后端不报 TBDR 位（vendorID 置位修复前的 Mali）→ 同 off
        g_caps_remove = NVN_GPU_CAP_TILE_MEMORY;
        t_disabled();
        printf("rp_sdx(capsoff): %d checks, %d failures\n", g_checks, g_fail);
        return g_fail != 0;
    }
    t_dontcare_after_clear();
    t_cont_att_stores();
    t_read_stores();
    t_read_stores_peekmiss();
    t_copy_stores();
    t_invalidate_paths();
    t_rcond_invalidate();
    t_fd_positive();
    t_fd_vid_positive();
    t_fd_neg_vid_small();
    t_fd_neg_src_const();
    t_fd_neg_src_unknown();
    t_fd_neg_small();
    t_fd_neg_nofacts();
    t_fd_neg_discard();
    t_fd_neg_blend();
    t_fd_neg_outofpool();
    t_fd_neg_scissor();
    t_fd_diag_tiny();
    t_fd_diag_self_serial();
    t_fd_diag_self_unres();
    t_fd_diag_self_array();
    printf("rp_sdx: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
