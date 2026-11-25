// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 真实回放与池镜像账本；GPU 拷贝在完成前由夹具按记录的行距写入镜像。
#define S2A_T2B_ORIGIN_TEST 1
#define main replay_fixture_main
#define nvn_gpu_copy_tex_buf fixture_copy_tex_buf
#define nvn_gpu_buf_cpu fixture_buf_cpu
#include "t_nvn_replay.c"
#undef nvn_gpu_copy_tex_buf
#undef nvn_gpu_buf_cpu
#undef main

static volatile uint32_t *g_event_check; static int g_event_seen_during_land = -1;
uint8_t *nvn_gpu_buf_cpu(nvn_gpu_buf b) {
    if (g_event_check && g_event_seen_during_land < 0) g_event_seen_during_land = (int)*g_event_check;
    return fixture_buf_cpu(b);
}
static struct { uint64_t off, row, img; uint32_t level, layer; NVNcopyRegion r; } g_copy[8];
static int g_ncopy;
void nvn_gpu_copy_tex_buf(nvn_gpu_cmd c, nvn_gpu_tex src, uint32_t level, uint32_t layer,
                          const NVNcopyRegion *r, nvn_gpu_buf dst, uint64_t off, uint64_t row, uint64_t img) {
    fixture_copy_tex_buf(c, src, level, layer, r, dst, off, row, img);
    if (g_ncopy < 8 && r) { g_copy[g_ncopy].off = off; g_copy[g_ncopy].row = row; g_copy[g_ncopy].img = img;
        g_copy[g_ncopy].level = level; g_copy[g_ncopy].layer = layer; g_copy[g_ncopy].r = *r; }
    g_ncopy++;
}
static p_copy_b2t *put_t2b(stream *s, const tex_t *t, uint64_t dst) {
    p_copy_b2t *p = put(s, OP_COPY_T2B, sizeof *p); p->tex = t; p->src = dst;
    p->r = (NVNcopyRegion){0, 0, 0, 3, 2, 1}; return p;
}
static uint8_t mirror[sizeof g_pmem];
static void fresh(void) {
    pass_fresh(); memset(g_pmem, 0x11, sizeof g_pmem); memset(mirror, 0x55, sizeof mirror);
    g_buf_cpu_fake = mirror; g_ncopy = g_ntb = 0; nvn_be_pool_add(g_pmem, sizeof g_pmem);
}
static void finish(void) { nvn_be_pool_del(g_pmem); g_buf_cpu_fake = NULL; pass_fresh(); }
static void test_layered_complete(void) {
    origin_mode = 1; origin_n = 0; nvn_be_set_frame(57);
    fresh(); tex_t t = mk_tex(810, 0, NVN_FORMAT_RGBA8, 8, 8, 2, 0);
    t.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; t.d.depth = 4; nvn_tex_layout(&t.d, &t.L);
    stream s = {0}; p_copy_b2t *p = put_t2b(&s, &t, (uint64_t)(uintptr_t)g_pmem + 64);
    p->has_view = 1; p->view.set_levels = 1; p->view.base_level = 1; p->view.set_layers = 1; p->view.min_layer = 1;
    p->r.zoffset = 1; p->r.depth = 2; p->row_stride = 16; p->image_stride = 48;
    volatile uint32_t event = 0; uint8_t *ev = put(&s, OP_SIGNAL_EVENT, 16);
    volatile uint32_t *event_ptr = &event; uint32_t mode = 0, value = 17;
    memcpy(ev, &event_ptr, 8); memcpy(ev + 8, &mode, 4); memcpy(ev + 12, &value, 4);
    end(&s); submit1(&s);
    CHECK(g_ncopy == 2 && nvn_unimpl_count("op", "OP_COPY_T2B") == 0);
    CHECK(g_copy[0].level == 1 && g_copy[0].layer == 2 && g_copy[0].off == 64 && g_copy[0].row == 16 && g_copy[0].img == 48);
    CHECK(g_copy[1].level == 1 && g_copy[1].layer == 3 && g_copy[1].off == 112 && g_copy[1].row == 16 && g_copy[1].img == 48);
    CHECK(g_pmem[64] == 0x11 && g_pmem[112] == 0x11);  // 尚未收到 GPU 完成，不回写。
    g_pmem[76] = 0x42; g_pmem[100] = 0x43;   // 进行中 CPU 写的是行 / 片填充，完成时不能被旧镜像覆盖。
    g_pg_gen[0] = ++g_dirty_now; g_dirty_now++;
    nvn_be_sub_begin(); g_nbw = 0;
    CHECK(rp_buf_resolve(&g_fake_cmd, (uint64_t)(uintptr_t)g_pmem + 64, 96, NULL, NULL));
    // 后续使用点也要同步填充，不能被在途行误挡。上一次提交显卡还没做完，这次同步不直接写镜像、按序录成 GPU 上的拷贝
    // （主线「池镜像只在此前的提交都做完时直写」）：录下的片段要盖到两处填充（偏移 76、100），不能盖到在途的行（首字节 64、80、112、128）。
    { int c76 = 0, c100 = 0, hit_row = 0;
      for (int i = 0; i < g_nbw; i++) { uint64_t lo = g_bw[i].off, hi = lo + g_bw[i].len;
          if (lo <= 76 && 76 < hi) c76 = 1;
          if (lo <= 100 && 100 < hi) c100 = 1;
          if ((lo <= 64 && 64 < hi) || (lo <= 80 && 80 < hi) || (lo <= 112 && 112 < hi) || (lo <= 128 && 128 < hi)) hit_row = 1; }
      CHECK(g_nbw > 0 && c76 && c100 && !hit_row && mirror[76] != 0x42); }
    nvn_be_sub_commit();
    for (int z = 0; z < g_ncopy && z < 2; z++) for (int y = 0; y < 2; y++)
        memset(mirror + g_copy[z].off + y * g_copy[z].row, 0xa0 + z, 12);
    g_event_check = &event; g_event_seen_during_land = -1; fake_complete(NVN_GPU_DONE_OK); g_event_check = NULL;
    CHECK(g_event_seen_during_land == 0 && event == 17);   // 先落数据，再让等待事件的 CPU 看见完成。
    for (int i = 0; i < 96; i++) {
        int z = i / 48, q = i % 48; uint8_t want = q < 32 && q % 16 < 12 ? (uint8_t)(0xa0 + z) : 0x11;
        if (i == 12) want = 0x42;
        if (i == 36) want = 0x43;
        CHECK(g_pmem[64 + i] == want);   // 每行和每层尾部间隙保持原样。
    }
    CHECK(g_pmem[63] == 0x11 && g_pmem[160] == 0x11); finish();
    CHECK(origin_matches("raw frame=57", "bytes=12 op=t2b stage=-1 slot=-1") == 4);
    CHECK(origin_matches("window source=t2b", "raw=4/48 merged=4/48 copied=4/48") == 1);
    origin_mode = 0;
}
static void test_failed_segment(void) {
    fresh(); tex_t t = mk_tex(811, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    stream s = {0}; put_t2b(&s, &t, (uint64_t)(uintptr_t)g_pmem + 64); end(&s); submit1(&s);
    CHECK(g_ncopy == 1); memset(mirror + 64, 0xfe, 24); fake_complete(NVN_GPU_DONE_FAILED);
    CHECK(g_pmem[64] == 0x11 && g_pmem[87] == 0x11); finish();
}
static void test_reject(void) {
    fresh(); tex_t t = mk_tex(812, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    stream s = {0}; p_copy_b2t *p = put_t2b(&s, &t, (uint64_t)(uintptr_t)g_pmem + sizeof g_pmem - 4); p->row_stride = 16;
    end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ncopy == 0 && nvn_unimpl_count("buf", "t2b_destination_outside_pools") == 1);
    stream s2 = {0}; p = put_t2b(&s2, &t, (uint64_t)(uintptr_t)g_pmem + 64); p->row_stride = 8;
    end(&s2); submit1(&s2); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_ncopy == 0 && nvn_unimpl_count("tex", "t2b_bad_stride") == 1);
    CHECK(g_pmem[64] == 0x11); finish();
}
static void test_registration_rollback(void) {
    fresh(); uint64_t base = (uint64_t)(uintptr_t)g_pmem; nvn_be_sub_begin();
    CHECK(!rp_post_gpu_write_rows(base + sizeof g_pmem - 4, 4, 8, 8, 2, 1, (struct rp_write_source){.op = RP_W_T2B}));
    g_pmem[sizeof g_pmem - 4] = 0x44;
    CHECK(rp_buf_resolve(&g_fake_cmd, base + sizeof g_pmem - 4, 4, NULL, NULL));
    CHECK(mirror[sizeof g_pmem - 4] == 0x44);   // 第二行登记失败，第一行不能残留「GPU 在途」。
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); finish();
}
static void test_3d_and_bc(void) {
    fresh(); tex_t t = mk_tex(813, 0, NVN_FORMAT_RGBA8, 8, 8, 2, 0);
    t.d.target = NVN_TEXTURE_TARGET_3D; t.d.depth = 8; nvn_tex_layout(&t.d, &t.L);
    stream s = {0}; p_copy_b2t *p = put_t2b(&s, &t, (uint64_t)(uintptr_t)g_pmem + 64);
    p->has_view = 1; p->view.set_levels = 1; p->view.base_level = 1; p->view.set_layers = 1; p->view.min_layer = 3;
    p->r.zoffset = 1; p->r.depth = 2; end(&s); submit1(&s);
    CHECK(g_ncopy == 2 && g_copy[0].level == 1 && g_copy[0].layer == 1 && g_copy[1].layer == 2);
    CHECK(g_copy[0].row == 12 && g_copy[0].img == 24 && g_copy[1].off == 88);
    fake_complete(NVN_GPU_DONE_FAILED); finish();
    fresh(); tex_t bc = mk_tex(814, 0, NVN_FORMAT_RGBA_DXT1, 8, 8, 1, 0);
    stream q = {0}; p = put_t2b(&q, &bc, (uint64_t)(uintptr_t)g_pmem + 64);
    p->r = (NVNcopyRegion){0, 0, 0, 8, 8, 1}; end(&q); submit1(&q);
    CHECK(g_ncopy == 1 && g_copy[0].row == 16 && g_copy[0].img == 32 && nvn_unimpl_count("op", "OP_COPY_T2B") == 0);
    fake_complete(NVN_GPU_DONE_FAILED); finish();
}
static void test_failed_registration_no_copy(void) {
    fresh(); tex_t t = mk_tex(815, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    p_copy_b2t p = {0}; p.tex = &t; p.src = (uint64_t)(uintptr_t)g_pmem + 64; p.r = (NVNcopyRegion){0, 0, 0, 3, 2, 1};
    rp_op_copy_t2b(&g_fake_cmd, &p, &p);   // 没有活动段：写回登记失败，整笔不能编码。
    CHECK(g_ncopy == 0 && nvn_unimpl_count("buf", "t2b_write_registration") == 1);
    CHECK(g_pmem[64] == 0x11); finish();
}
int main(void) {
    test_layered_complete(); test_failed_segment(); test_reject(); test_registration_rollback(); test_3d_and_bc(); test_failed_registration_no_copy();
    printf("%s t_nvn_copy_t2b：%d 项断言，%d 项失败\n", g_fail ? "✗" : "✓", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
