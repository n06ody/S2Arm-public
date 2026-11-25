// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real replay, with observations at the Vulkan-facing boundary. The real GPU
// selector and MRT behavior are exercised separately by t_cold_guard.c.
#define main nvn_replay_all_tests
#include "../../../graphics/nvn/tests/t_nvn_replay.c"
#undef main

static struct nvn_gpu_pipe_s variant_pipe;
static unsigned cold_asks, cold_scans, cold_draws;
static int variant_ready, proof_ready;
static struct nvn_gpu_cold_domain candidate;

int nvn_gpu_pipe_lookup(const nvn_gpu_pipe_desc *desc, nvn_gpu_pipe *out) {
    if (!g_pipe_gets) return 0;
    CHECK(desc->cfmt[0] == g_last_pdesc.cfmt[0] && desc->prim == g_last_pdesc.prim);
    *out = &g_fpipe;
    return 1;
}
int nvn_vk_pipe_demand_cold_kick(const nvn_gpu_pipe_desc *desc, const uint8_t identity[32],
                                const uint8_t variant_sha[32], nvn_gpu_pipe *out) {
    CHECK(desc->st[NVN_GPU_SH_FS].sh == &g_variant_fs && identity[0] == 2 && variant_sha[0] == 3);
    cold_asks++;
    *out = variant_ready ? &variant_pipe : NULL;
    return variant_ready ? 2 : 1;
}
int nvn_vk_cold_guard_auto(nvn_gpu_cmd cmd, nvn_gpu_cmd pre, nvn_gpu_pipe a, nvn_gpu_pipe b,
                           const nvn_gpu_draw_args *draw, const struct nvn_gpu_cold_domain *d) {
    (void)pre;
    CHECK(cmd == &g_fake_cmd && !g_pass_open && a == &g_fpipe && b == &variant_pipe);
    CHECK(draw->count == 3 && draw->instances == 1 && !draw->ind && !draw->ibuf);
    CHECK(d->buf && d->binding.off == 2 * PG && d->binding.bytes == 256);
    CHECK(d->prefix.buf == &g_arena_buf && d->prefix.binding.bytes == 0xA00);
    uint64_t original_addr;
    memcpy(&original_addr, g_arena + d->prefix.binding.off + d->prefix.byte, sizeof original_addr);
    CHECK(original_addr == (uint64_t)(uintptr_t)(g_pmem + 2 * PG));
    candidate = *d;
    cold_scans++;
    return proof_ready;
}
int nvn_vk_cold_guard_draw(nvn_gpu_cmd c, const nvn_gpu_draw_args *draw) {
    CHECK(g_pass_open && g_nfb[NVN_GPU_SH_FS] == 2);
    const nvn_gpu_binding *fs = g_fb[NVN_GPU_SH_FS];
    CHECK(fs[0].u.b.buf == candidate.buf && fs[0].u.b.off == candidate.binding.off);
    CHECK(fs[1].u.b.buf == candidate.prefix.buf && fs[1].u.b.off == candidate.prefix.binding.off);
    cold_draws++;
    nvn_gpu_draw(c, draw);
    return 1;
}

static void cold_round(int proof, int ready, int capture, int occlusion) {
    pass_fresh();
    g_gdraws = g_pipe_gets = g_resolve_rc = 0;
    g_arena_used = 0;
    cold_asks = cold_scans = cold_draws = 0;
    variant_ready = ready;
    proof_ready = 1;
    memset(g_pmem, 0, sizeof g_pmem);
    nvn_be_pool_add(g_pmem, sizeof g_pmem);
    static const struct rp_cold_proof recipe = {
        .fragment = &g_variant_fs, .variant_sha = {3}, .fragment_ent = 0, .first_word = 3,
        .stride_words = 5, .count = 8, .prefix_ent = 1, .prefix_byte = 0x510, .prefix_mask = 15,
    };
    stream init = {0};
    q_env(&init);
    g_fprog.identity[0] = 2;
    g_fprog.cold = proof ? &recipe : NULL;
    g_fprog.nent[NVN_GPU_SH_FS] = 2;
    g_fprog.ent[NVN_GPU_SH_FS][0] = (nvn_gpu_layout_ent){
        .cls = NVN_GPU_RES_SSBO, .src_kind = NVN_RB_SBUFP, .count = 1,
        .access = NVN_GPU_ACC_R, .src_reg2 = UINT16_MAX,
    };
    g_fprog.ent[NVN_GPU_SH_FS][1] = (nvn_gpu_layout_ent){
        .cls = NVN_GPU_RES_UBO, .src_kind = NVN_RB_DRV0, .count = 1,
        .access = NVN_GPU_ACC_R, .src_reg2 = UINT16_MAX, .binding = 1,
    };
    p_bind_buf *sb = put(&init, OP_BIND_SSBO, sizeof *sb);
    sb->stage = NVN_GPU_SH_FS; sb->addr = (uint64_t)(uintptr_t)(g_pmem + 2 * PG); sb->size = 256;
    q_draw(&init); end(&init); submit1(&init); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == 1 && cold_draws == 0);
    stream sub = {0};
    if (capture) {
        p_render_cond *cond = put(&sub, OP_RENDER_COND, sizeof *cond);
        cond->mode = capture == 2 ? 3 : 0;
        cond->addr = (uint64_t)(uintptr_t)g_pmem;
    }
    if (occlusion) q_counter(&sub, OP_RESET_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED, NULL);
    q_draw(&sub); q_draw(&sub); end(&sub); submit1(&sub); fake_complete(NVN_GPU_DONE_OK);
    const int eligible = proof && !capture && !occlusion;
    CHECK(cold_asks == (unsigned)eligible);
    CHECK(cold_scans == (unsigned)(eligible && ready) && cold_draws == cold_scans);
    CHECK(g_gdraws == (capture == 2 ? 1 : 3));
    CHECK(g_bind_outside == 0 && g_draw_outside == 0);
}

int main(void) {
    cold_round(1, 1, 0, 0);
    cold_round(1, 0, 0, 0);
    cold_round(0, 1, 0, 0);
    cold_round(1, 1, 1, 0);
    cold_round(1, 1, 2, 0);
    cold_round(1, 1, 0, 1);
    pass_fresh();
    printf("cold replay: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
