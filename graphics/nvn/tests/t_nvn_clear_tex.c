// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 独立录制流夹具：真实回放核心，后端替身记录清除、通道和顺序。
#define nvn_gpu_view_create fixture_view_create
#define nvn_plat_flag fixture_flag
#define nvn_gpu_format_caps fixture_format_caps
#define nvn_gpu_timestamp fixture_timestamp
#define nvn_gpu_copy_tex fixture_copy_tex
#define nvn_gpu_pass_begin fixture_pass_begin
#define main replay_fixture_main
#include "t_nvn_replay.c"
#undef main
#undef nvn_gpu_view_create
#undef nvn_plat_flag
#undef nvn_gpu_format_caps
#undef nvn_gpu_timestamp
#undef nvn_gpu_copy_tex
#undef nvn_gpu_pass_begin

static struct { int n, npd, ends, is_int; float f[4]; uint32_t u[4]; } g_cv;
static int g_transfers_at_first_pass, g_pending_at_first_pass;
static nvn_gpu_tex g_mrt_alias;
static int g_clears_at_alias_view;
static view_t g_seen_view[16]; static int g_nseen_view;
void nvn_gpu_pass_begin(nvn_gpu_cmd c, const nvn_gpu_pass_desc *d) {
    if (!g_npd) {
        g_transfers_at_first_pass = g_cv.n;
        g_pending_at_first_pass = rp_tex_clear_active;
    }
    fixture_pass_begin(c, d);
}
nvn_gpu_rc nvn_gpu_view_create(nvn_gpu_tex t, const view_t *v, nvn_gpu_view *out, nvn_gpu_err *err) {
    if (v && g_nseen_view < 16) g_seen_view[g_nseen_view++] = *v;
    if (t == g_mrt_alias && g_clears_at_alias_view < 0) g_clears_at_alias_view = g_cv.n;
    return fixture_view_create(t, v, out, err);
}

static int g_flag_clear_tex_fold, g_flag_clear_tex_fold_diag, g_clear_at_report, g_clear_at_copy;
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_clear_tex_fold")) return g_flag_clear_tex_fold;
    if (!strcmp(name, "nvk_clear_tex_fold_diag")) return g_flag_clear_tex_fold_diag;
    return fixture_flag(name, dflt);
}
uint32_t nvn_gpu_format_caps(uint32_t fmt) {
    uint32_t caps = fixture_format_caps(fmt);
    if (fmt == NVN_FORMAT_RGBA8 || fmt == NVN_FORMAT_RGBA32UI || fmt == NVN_FORMAT_RGBA32I)
        caps |= NVN_GPU_FMT_RENDER | NVN_GPU_FMT_CLEAR_FOLD;
    return caps;
}
void nvn_gpu_timestamp(nvn_gpu_cmd c, uint32_t slot, uint32_t type) {
    g_clear_at_report = g_cv.n;
    fixture_timestamp(c, slot, type);
}
void nvn_gpu_copy_tex(nvn_gpu_cmd c, nvn_gpu_tex s, uint32_t sm, uint32_t sl, const NVNcopyRegion *sr,
                      nvn_gpu_tex d, uint32_t dm, uint32_t dl, const NVNcopyRegion *dr) {
    g_clear_at_copy = g_cv.n;
    fixture_copy_tex(c, s, sm, sl, sr, d, dm, dl, dr);
}
int nvn_gpu_cmd_keep_clear_view(nvn_gpu_cmd c, nvn_gpu_view v) {
    return c && v;
}
void nvn_gpu_clear_view(nvn_gpu_cmd c, nvn_gpu_view v, const float rgba[4], const uint32_t u4[4], int is_int) {
    (void)c; CHECK(v != NULL); g_cv.n++; g_cv.npd = g_npd; g_cv.ends = g_pass_ends; g_cv.is_int = is_int;
    if (rgba) memcpy(g_cv.f, rgba, sizeof g_cv.f);
    if (u4) memcpy(g_cv.u, u4, sizeof g_cv.u);
}
void nvn_gpu_clear_kept_view(nvn_gpu_cmd c, nvn_gpu_view v, const float rgba[4], const uint32_t u4[4], int is_int) {
    nvn_gpu_clear_view(c, v, rgba, u4, is_int);
}
static p_clear_tex *clear_op(stream *s, const tex_t *t, int x, int y, int w, int h, int mask) {
    p_clear_tex *p = put(s, OP_CLEAR_TEX, sizeof *p); p->tex = t;
    p->r.xoffset = x; p->r.yoffset = y; p->r.width = w; p->r.height = h; p->r.depth = 1;
    p->mask = mask; p->value_type = CLEAR_TEX_FLOAT;
    p->c.f[0] = 0.25f; p->c.f[1] = 0.5f; p->c.f[2] = 0.75f; p->c.f[3] = 1.0f;
    return p;
}
static void clear_fresh(void) { pass_fresh(); memset(&g_cv, 0, sizeof g_cv); g_nseen_view = 0; }
static void test_whole_and_order(void) {
    clear_fresh(); tex_t t = mk_tex(201, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    stream s = {0}; put_set_rt(&s, &t, NULL);
    p_clear_color *c = put(&s, OP_CLEAR_COLOR, sizeof *c); c->index = 0; c->mask = 15; c->rgba[0] = 0.125f;
    clear_op(&s, &t, 0, 0, 8, 8, 15); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 1 && g_cv.npd == 1 && g_cv.ends == 1);
    CHECK(!g_cv.is_int && g_cv.f[0] == 0.25f && g_cv.f[2] == 0.75f && g_cv.f[3] == 1.0f);
    CHECK(g_pd[0].color[0].load == NVN_GPU_LOAD_CLEAR && g_pd[0].color[0].clear.f[0] == 0.125f);
    CHECK(nvn_unimpl_count("op", "OP_CLEAR_TEX") == 0);
}
static void test_partial(void) {
    clear_fresh(); tex_t t = mk_tex(202, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    stream s = {0}; clear_op(&s, &t, 2, 1, 3, 4, 5); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 0 && g_npd == 1 && g_pass_ends == 1 && g_ncr == 1);
    CHECK(g_pd[0].ncolor == 1 && g_pd[0].w == 8 && g_pd[0].h == 8 && g_pd[0].color[0].load == NVN_GPU_LOAD_LOAD && g_pd[0].color[0].store == NVN_GPU_STORE_STORE);
    CHECK(g_cr[0].d.x == 2 && g_cr[0].d.y == 1 && g_cr[0].d.w == 3 && g_cr[0].d.h == 4 && g_cr[0].d.color_mask == 5);
    CHECK(g_cr[0].d.value.f[0] == 0.25f && g_cr[0].d.value.f[2] == 0.75f && g_cr[0].npd == 1 && g_cr[0].ends == 0);
    CHECK(nvn_unimpl_count("op", "OP_CLEAR_TEX") == 0);
}
static void test_empty_and_bounds(void) {
    clear_fresh(); tex_t t = mk_tex(203, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    stream s = {0}; clear_op(&s, &t, 0, 0, 8, 8, 0); clear_op(&s, &t, 0, 0, 0, 8, 15);
    end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 0 && g_npd == 0 && g_ncr == 0 && nvn_unimpl_kinds() == 0);
    clear_fresh(); stream b = {0}; clear_op(&b, &t, 7, 0, 2, 8, 15); end(&b); submit1(&b); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 0 && g_npd == 0 && g_ncr == 0 && nvn_unimpl_kinds() > 0);
    CHECK(nvn_unimpl_count("op", "OP_CLEAR_TEX") == 0);
}
static void test_levels_layers_and_types(void) {
    clear_fresh(); tex_t t = mk_tex(204, 0, NVN_FORMAT_RGBA8, 8, 8, 2, 0);
    t.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; t.d.depth = 4; nvn_tex_layout(&t.d, &t.L);
    stream s = {0}; p_clear_tex *p = clear_op(&s, &t, 0, 0, 4, 4, 15);
    p->has_view = 1; p->view.set_levels = 1; p->view.base_level = 1; p->view.set_layers = 1; p->view.min_layer = 1;
    p->r.zoffset = 1; p->r.depth = 2; end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 2 && g_nseen_view == 2);
    CHECK(g_seen_view[0].base_level == 1 && g_seen_view[0].nlevels == 1 && g_seen_view[0].min_layer == 2 && g_seen_view[0].nlayers == 1);
    CHECK(g_seen_view[1].base_level == 1 && g_seen_view[1].min_layer == 3 && g_seen_view[1].nlayers == 1);
    CHECK(nvn_be_tex_gpu_owned(t.serial));
    clear_fresh(); tex_t u = mk_tex(205, 0, NVN_FORMAT_RGBA32UI, 4, 4, 1, 0);
    stream q = {0}; p = clear_op(&q, &u, 0, 0, 4, 4, 15); p->value_type = CLEAR_TEX_UINT;
    p->c.u[0] = 0xffffffffu; p->c.u[1] = 16777217u; p->c.u[2] = 3; p->c.u[3] = 4;
    end(&q); submit1(&q); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 1 && g_cv.is_int && g_cv.u[0] == 0xffffffffu && g_cv.u[1] == 16777217u);
    clear_fresh(); tex_t si = mk_tex(207, 0, NVN_FORMAT_RGBA32I, 4, 4, 1, 0);
    stream neg = {0}; p = clear_op(&neg, &si, 0, 0, 4, 4, 15); p->value_type = CLEAR_TEX_INT;
    p->c.i[0] = INT32_MIN; p->c.i[1] = -16777217; p->c.i[2] = INT32_MAX; p->c.i[3] = -1;
    end(&neg); submit1(&neg); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 1 && g_cv.is_int && g_cv.u[0] == 0x80000000u && g_cv.u[1] == (uint32_t)-16777217 && g_cv.u[2] == 0x7fffffffu && g_cv.u[3] == 0xffffffffu);
    clear_fresh(); tex_t cvt = mk_tex(208, 0, NVN_FORMAT_RGBA32UI, 4, 4, 1, 0);
    stream conv = {0}; p = clear_op(&conv, &cvt, 0, 0, 4, 4, 15); p->c.f[0] = -1; p->c.f[1] = 12.75f; p->c.f[2] = 1.0e30f;
    end(&conv); submit1(&conv); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 1 && g_cv.is_int && g_cv.u[0] == 0 && g_cv.u[1] == 12 && g_cv.u[2] == 0xffffffffu && g_cv.u[3] == 1);
    clear_fresh(); tex_t v = mk_tex(206, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    stream bad = {0}; p = clear_op(&bad, &v, 0, 0, 8, 8, 15); p->has_view = 1; p->view.set_format = 1; p->view.format = NVN_FORMAT_RGBA8UI;
    end(&bad); submit1(&bad); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 0 && g_npd == 0 && nvn_unimpl_count("tex", "clear_view_format") == 1 && !nvn_be_tex_gpu_owned(v.serial));
}
static void t_clear_tex_fold(void) {
    enum {
        UNORM, UINT, SINT, SUBRESOURCE, NO_COLOR_PARK,
        BARRIER, REPORT, COPY, SAMPLED, SAMPLED_ALIAS, ALIAS, SMALL_FB, SMALL_FB_COLD, WRONG_MIP, WRONG_LAYER,
        NON_RENDERABLE, TAIL, PARTIAL, REINTERPRET, OFF,
    };
    const int old_park = g_flag_clear_park, old_area = g_flag_pass_area;
    for (int kind = UNORM; kind <= OFF; kind++) {
        clear_fresh();
        g_resolve_rc = 0;
        g_flag_clear_tex_fold = kind != OFF;
        g_flag_clear_tex_fold_diag = kind != NO_COLOR_PARK;   // 功能开而诊断关的独立对照。
        g_flag_clear_park = kind == NO_COLOR_PARK ? 0 : old_park;
        g_flag_pass_area = 1;
        g_clear_at_report = g_clear_at_copy = 0;
        g_ts_calls = g_nct = 0;
        stream s = {0};
        q_env(&s);
        const uint32_t fmt = kind == UINT ? NVN_FORMAT_RGBA32UI : kind == SINT ? NVN_FORMAT_RGBA32I :
                             kind == NON_RENDERABLE ? NVN_FORMAT_RGBA8UI : NVN_FORMAT_RGBA8;
        const int sub = kind == SUBRESOURCE || kind == WRONG_MIP || kind == WRONG_LAYER;
        tex_t t = mk_tex(601, 0x20000, (int)fmt, 16, 16, sub ? 2 : 1, 0);
        if (sub) {
            t.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; t.d.depth = 2;
            nvn_tex_layout(&t.d, &t.L);
            CHECK(t.L.size > 0);
        }
        memset(t.mem, 0x22, t.L.size);
        const int other_size = kind == SMALL_FB || kind == SMALL_FB_COLD || kind == SAMPLED_ALIAS ? 8 : 16;
        const int64_t other_off = kind == ALIAS || kind == SAMPLED_ALIAS ? t.off : 0x38000;
        tex_t other = mk_tex(602, other_off, NVN_FORMAT_RGBA8, other_size, other_size, 1, 0);
        memset(other.mem, 0x33, other.L.size);
        if (kind == SAMPLED_ALIAS || kind == SMALL_FB) {
            // 先上传另一纹理，单独核对取样别名与较小帧缓冲的兑现边界。
            nvn_be_sub_begin();
            CHECK(rp_tex_resolve(&other, &g_fake_cmd) != NULL);
            nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
        }
        p_clear_tex *op = put(&s, OP_CLEAR_TEX, sizeof *op);
        op->tex = &t; op->mask = 15;
        const int width = kind == PARTIAL || sub ? 8 : 16;
        op->r = (NVNcopyRegion){0, 0, 0, width, sub ? 8 : 16, 1};
        if (sub) {
            op->has_view = 1;
            op->view.set_levels = op->view.set_layers = 1;
            op->view.base_level = op->view.nlevels = 1;
            op->view.min_layer = op->view.nlayers = 1;
        }
        if (kind == REINTERPRET) {
            op->has_view = 1; op->view.set_format = 1; op->view.format = NVN_FORMAT_RGBA8UI;
        }
        if (kind == UINT || kind == NON_RENDERABLE) {
            op->value_type = CLEAR_TEX_UINT;
            op->c.u[0] = UINT32_MAX; op->c.u[1] = 0x10000001u; op->c.u[2] = 0x80000001u; op->c.u[3] = 1;
        } else if (kind == SINT) {
            op->value_type = CLEAR_TEX_INT;
            op->c.i[0] = INT32_MIN; op->c.i[1] = -0x10000001; op->c.i[2] = INT32_MAX; op->c.i[3] = 1;
        } else {
            op->c.f[0] = 0.25f; op->c.f[1] = 0.75f; op->c.f[2] = 0.5f; op->c.f[3] = 1.0f;
        }
        uint64_t report[2] = {0, 0};
        if (kind == BARRIER) *(uint32_t *)put(&s, OP_BARRIER, 8) = NVN_BARRIER_ORDER_FRAGMENTS_BIT;
        if (kind == REPORT) { g_caps_extra = NVN_GPU_CAP_REPORT_TIMESTAMP; ts_report(&s, report); }
        if (kind == COPY) {
            p_copy_t2t *copy = put(&s, OP_COPY_T2T, sizeof *copy);
            copy->src = &t; copy->dst = &other; copy->sr = op->r; copy->dr = op->r;
        }
        if (kind == SAMPLED || kind == SAMPLED_ALIAS) {
            samp_par sampler = {0};
            nvn_be_reg_tex(7, kind == SAMPLED ? &t : &other, NULL); nvn_be_reg_samp(3, &sampler);
            g_fprog.nent[1] = 1;
            g_fprog.ent[1][0].cls = NVN_GPU_RES_TEX_SAMP;
            g_fprog.ent[1][0].src_kind = NVN_RB_TEX;
            g_fprog.ent[1][0].src_reg = 5;
            g_fprog.dtt[1][0] = NVN_TEXTURE_TARGET_2D + 1;
            p_bind_handles *bh = put(&s, OP_BIND_TEX, sizeof *bh);
            bh->stage = 1; bh->first = 5; bh->n = 1; bh->h[0] = 7 | 3 << 20;
        }
        if (kind != TAIL && kind != NON_RENDERABLE) {
            p_set_rt rt = {0};
            rt.n = 1; rt.color[0] = kind == ALIAS ? &other : &t;
            if (kind == SMALL_FB || kind == SMALL_FB_COLD) { rt.n = 2; rt.color[1] = &other; }
            if (sub) {
                rt.has_cview[0] = 1; rt.cview[0] = op->view;
                if (kind == WRONG_MIP) rt.cview[0].base_level = 0;
                if (kind == WRONG_LAYER) rt.cview[0].min_layer = 0;
            }
            set_rt_put(put(&s, OP_SET_RT, set_rt_size(&rt)), &rt);
            if (kind == UNORM) area_scissor(&s, 0, 0, 16, 16);
            q_draw(&s);
        }
        end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        printf("CLEAR_TEX_FOLD case=%d transfers=%d passes=%d first_load=%u\n",
               kind, g_cv.n, g_npd, g_npd ? g_pd[0].color[0].load : 0);
        if (kind <= NO_COLOR_PARK) {
            CHECK(g_cv.n == 0 && g_npd == 1 && g_pd[0].color[0].load == NVN_GPU_LOAD_CLEAR);
            CHECK(!memcmp(&g_pd[0].color[0].clear, &op->c, sizeof op->c));
            CHECK(g_pd[0].area_w == 0 && g_pd[0].area_h == 0);
        } else if (kind == PARTIAL) {
            CHECK(g_cv.n == 0 && g_npd == 2 && g_ncr == 1 && g_cr[0].d.w == 8);
            CHECK(g_pd[0].color[0].load == NVN_GPU_LOAD_LOAD && g_pd[1].color[0].load == NVN_GPU_LOAD_LOAD);
        } else if (kind == REINTERPRET) {
            CHECK(nvn_unimpl_count("tex", "clear_view_format") == 1 && g_cv.n == 0);
        } else {
            const int draws = kind != TAIL && kind != NON_RENDERABLE;
            CHECK(g_cv.n == 1 && g_npd == draws);
            const void *bits = g_cv.is_int ? (const void *)g_cv.u : (const void *)g_cv.f;
            CHECK(g_cv.is_int == (kind == NON_RENDERABLE) && !memcmp(bits, &op->c, sizeof op->c));
            if (draws) CHECK(g_cv.npd == 0 && g_pd[0].color[0].load == NVN_GPU_LOAD_LOAD);
            if (kind == REPORT) CHECK(g_clear_at_report == 1 && g_ts_calls == 1 && report[0] != 0);
            if (kind == COPY) CHECK(g_clear_at_copy == 1 && g_nct == 1);
            if (kind == SMALL_FB || kind == SMALL_FB_COLD) CHECK(g_pd[0].w == 8 && g_pd[0].h == 8 && g_pd[0].color[1].view);
        }
        uint64_t counts[RP_TF_N] = {0};
        CHECK(rp_tex_fold_stats_take(counts) == (kind != NO_COLOR_PARK));
        const enum rp_tex_fold_result expected[] = {
            [UNORM] = RP_TF_HIT, [UINT] = RP_TF_HIT, [SINT] = RP_TF_HIT, [SUBRESOURCE] = RP_TF_HIT,
            [BARRIER] = RP_TF_FALL_BARRIER, [REPORT] = RP_TF_FALL_BOUNDARY, [COPY] = RP_TF_FALL_TRANSFER,
            [SAMPLED] = RP_TF_FALL_READ, [SAMPLED_ALIAS] = RP_TF_FALL_ALIAS, [ALIAS] = RP_TF_FALL_ALIAS,
            [SMALL_FB] = RP_TF_FALL_TARGET, [SMALL_FB_COLD] = RP_TF_FALL_TRANSFER,
            [WRONG_MIP] = RP_TF_FALL_TARGET, [WRONG_LAYER] = RP_TF_FALL_TARGET,
            [NON_RENDERABLE] = RP_TF_FORMAT_VALUE, [TAIL] = RP_TF_FALL_END, [PARTIAL] = RP_TF_SHAPE,
            [OFF] = RP_TF_DISABLED,
        };
        for (int i = 0; i < RP_TF_N; i++) {
            const uint64_t want = kind != NO_COLOR_PARK && kind != REINTERPRET && i == (int)expected[kind];
            CHECK(counts[i] == want);
        }
        g_caps_extra = 0;
    }
    g_flag_clear_tex_fold = g_flag_clear_tex_fold_diag = 0;
    g_flag_clear_park = old_park;
    g_flag_pass_area = old_area;
    pass_fresh();
}

static void t_clear_tex_fold_mrt(void) {
    enum { FOUR, FULL, RECLEAR, ALIAS };
    for (int kind = FOUR; kind <= ALIAS; kind++) {
        clear_fresh();
        g_resolve_rc = 0;
        g_flag_clear_tex_fold = g_flag_clear_tex_fold_diag = 1;
        const int count = kind == FULL ? 9 : 4;
        tex_t tex[9];
        // 排除首次上传这个既有兑现边界，本例只量同段的连续清除。
        nvn_be_sub_begin();
        for (int i = 0; i < count; i++) {
            tex[i] = mk_tex(700 + i, 0x4000 * i, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
            memset(tex[i].mem, 0x40 + i, tex[i].L.size);
            CHECK(rp_tex_resolve(&tex[i], &g_fake_cmd) != NULL);
        }
        tex_t alias = tex[0];
        if (kind == ALIAS) {
            alias.serial = 800;
            alias.d.levels = 2;   // 地址重叠，但宿主会建不同的图
            nvn_tex_layout(&alias.d, &alias.L);
            g_mrt_alias = rp_tex_resolve(&alias, &g_fake_cmd);
            CHECK(g_mrt_alias != NULL);
            g_clears_at_alias_view = -1;
        }
        nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
        stream s = {0}; q_env(&s);
        for (int i = 0; i < count; i++) {
            p_clear_tex *op = clear_op(&s, &tex[i], 0, 0, 16, 16, 15);
            op->c.f[0] = (float)(i + 1) / 16.0f;
        }
        if (kind == RECLEAR || kind == ALIAS) {
            p_clear_tex *op = clear_op(&s, kind == ALIAS ? &alias : &tex[0], 0, 0, 16, 16, 15);
            op->c.f[0] = 0.75f;
        }
        p_set_rt rt = {0};
        rt.n = kind == FULL ? 8 : 4;
        for (int i = 0; i < rt.n; i++) rt.color[i] = &tex[i + (kind == FULL)];
        if (kind == ALIAS) rt.color[0] = &alias;
        set_rt_put(put(&s, OP_SET_RT, set_rt_size(&rt)), &rt);
        q_draw(&s); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
        unsigned clears = 0;
        for (int i = 0; i < rt.n; i++) clears += g_pd[0].color[i].load == NVN_GPU_LOAD_CLEAR;
        printf("CLEAR_TEX_FOLD_MRT case=%d transfers=%d passes=%d clears=%u\n", kind, g_cv.n, g_npd, clears);
        CHECK(g_cv.n == (kind != FOUR) && g_npd == 1 && clears == (unsigned)rt.n);
        uint64_t counts[RP_TF_N];
        CHECK(rp_tex_fold_stats_take(counts));
        for (int i = 0; i < RP_TF_N; i++) {
            const uint64_t want = i == RP_TF_HIT ? (uint64_t)rt.n :
                (i == RP_TF_FALL_FULL && kind == FULL) || (i == RP_TF_FALL_ALIAS && (kind == RECLEAR || kind == ALIAS));
            CHECK(counts[i] == want);
        }
        CHECK(rp_tex_fold_stats_take(counts));
        for (int i = 0; i < RP_TF_N; i++) CHECK(counts[i] == 0);
        if (kind != FOUR) CHECK(g_cv.npd == 0 && g_cv.f[0] == 1.0f / 16.0f);
        if (kind == ALIAS) {
            CHECK(g_clears_at_alias_view == 1);
            g_mrt_alias = NULL;
        }
        for (int i = 0; i < rt.n; i++) {
            const float value = !i && (kind == RECLEAR || kind == ALIAS) ?
                                0.75f : (float)(i + 1 + (kind == FULL)) / 16.0f;
            CHECK(g_pd[0].color[i].clear.f[0] == value);
        }
    }
    g_flag_clear_tex_fold = g_flag_clear_tex_fold_diag = 0;
    clear_fresh();
}

static void t_clear_tex_fold_pass_area(void) {
    enum { LOCAL, FULL, AREA_OFF, FOLD_OFF };
    const int old_area = g_flag_pass_area;
    for (int kind = LOCAL; kind <= FOLD_OFF; kind++) {
        clear_fresh();
        g_resolve_rc = 0;
        g_flag_clear_tex_fold = kind != FOLD_OFF;
        g_flag_clear_tex_fold_diag = 1;
        g_flag_pass_area = kind != AREA_OFF;
        tex_t tex[5];
        nvn_be_sub_begin();
        for (int i = 0; i < 5; i++) {
            tex[i] = mk_tex(900 + i, 0x4000 * (i + 1), NVN_FORMAT_RGBA8, 16, 16, 1, 0);
            memset(tex[i].mem, 0x50 + i, tex[i].L.size);
            CHECK(rp_tex_resolve(&tex[i], &g_fake_cmd) != NULL);
        }
        nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
        stream s = {0}; q_env(&s);
        for (int i = 0; i < 5; i++) clear_op(&s, &tex[i], 0, 0, 16, 16, 15)->c.f[0] = (float)(i + 1) / 16.0f;
        const tex_t *colors[] = {&tex[0], &tex[1], &tex[2], &tex[3]};
        put_set_rt_n(&s, colors, 4, NULL);
        if (kind == FULL) area_scissor(&s, 0, 0, 16, 16);
        else area_scissor(&s, 2, 3, 4, 5);
        const int before = g_gdraws;
        q_draw(&s); q_draw(&s); end(&s);
        submit1(&s); fake_complete(NVN_GPU_DONE_OK);

        const int local = kind == LOCAL || kind == FOLD_OFF;
        const int transfer = local ? 5 : 1;
        const nvn_gpu_pass_desc *d = &g_pd[0];
        unsigned clears = 0;
        for (int i = 0; i < 4; i++) clears += d->color[i].load == NVN_GPU_LOAD_CLEAR;
        printf("CLEAR_TEX_FOLD_AREA case=%d area=%u,%u transfers=%d before_pass=%d pending=%d passes=%d clears=%u\n",
               kind, d->area_w, d->area_h, g_cv.n, g_transfers_at_first_pass, g_pending_at_first_pass, g_npd, clears);
        CHECK(g_npd == 1 && g_pass_ends == 1 && g_gdraws == before + 2);
        CHECK(d->ncolor == 4 && d->w == 16 && d->h == 16);
        CHECK(d->area_w == (local ? 4u : 0u) && d->area_h == (local ? 5u : 0u));
        CHECK(!local || (d->area_x == 2 && d->area_y == 3));
        CHECK(g_cv.n == transfer && g_transfers_at_first_pass == transfer && g_pending_at_first_pass == 0);
        CHECK(g_cv.f[0] == 5.0f / 16.0f);   // 不属于本遍的清除在第一遍之前兑现
        uint64_t counts[RP_TF_N] = {0};
        CHECK(rp_tex_fold_stats_take(counts));
        CHECK(counts[RP_TF_HIT] == (local ? 0u : 4u));
        CHECK(counts[RP_TF_FALL_AREA] == (kind == LOCAL ? 5u : 0u));
        CHECK(counts[RP_TF_FALL_TARGET] == (local ? 0u : 1u));
        CHECK(counts[RP_TF_DISABLED] == (kind == FOLD_OFF ? 5u : 0u));
        for (int i = 0; i < 4; i++) {
            CHECK(d->color[i].view && d->color[i].load == (local ? NVN_GPU_LOAD_LOAD : NVN_GPU_LOAD_CLEAR));
            if (!local) CHECK(d->color[i].clear.f[0] == (float)(i + 1) / 16.0f);
        }
    }
    g_flag_pass_area = old_area;
    g_flag_clear_tex_fold = g_flag_clear_tex_fold_diag = 0;
    clear_fresh();
}

int main(void) {
    t_clear_tex_fold_mrt();
    t_clear_tex_fold_pass_area();
    t_clear_tex_fold();
    test_whole_and_order(); test_partial(); test_empty_and_bounds(); test_levels_layers_and_types();
    pass_fresh();
    printf("%s t_nvn_clear_tex：%d 项断言，%d 项失败\n", g_fail ? "✗" : "✓", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
