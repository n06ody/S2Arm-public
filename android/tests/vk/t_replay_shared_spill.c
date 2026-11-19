// SPDX-License-Identifier: Apache-2.0
// Real replay + Vulkan backend: compare unspilled and transformed compute,
// including replay's per-dispatch allocation and indirect argument snapshot.
#define main replay_dispatch_barrier_fixture_main
#define rp_view_for dispatch_fixture_view_for
#define rp_storage_srgb_alias dispatch_fixture_storage_srgb_alias
#include "t_replay_dispatch_barrier.c"
#undef rp_storage_srgb_alias
#undef rp_view_for
#undef main

// The fixture binds only storage buffers. Abort if an image path is reached.
nvn_gpu_view rp_view_for(const tex_t *t, const view_t *v, nvn_gpu_cmd c) {
    (void)t; (void)v; (void)c; abort();
}
uint32_t rp_storage_srgb_alias(uint32_t f) { (void)f; abort(); }

enum { SPILL_GROUPS = 12, SPILL_WORDS = SPILL_GROUPS * 9 };
static void run_spill(const char *dir, int spilled, int indirect, uint64_t serial,
                      uint32_t result[SPILL_WORDS]) {
    memset(&prog, 0, sizeof prog); prog.is_compute = 1; prog.nent[0] = spilled ? 2 : 1;
    nvn_gpu_layout_ent *e = &prog.ent[0][0];
    e->cls = NVN_GPU_RES_SSBO; e->src_kind = NVN_RB_SBUFP; e->src_reg = 0;
    e->src_reg2 = UINT16_MAX; e->access = NVN_GPU_ACC_RW; e->count = 1;
    e->stages = 1u << NVN_GPU_SH_CS;
    if (spilled) {
        e = &prog.ent[0][1]; e->cls = NVN_GPU_RES_SSBO; e->src_kind = NVN_RB_SPILL;
        e->binding = 1; e->src_reg2 = UINT16_MAX; e->access = NVN_GPU_ACC_RW;
        e->count = 1; e->stages = 1u << NVN_GPU_SH_CS; prog.spill_bytes = 10240;
    }
    nvn_gpu_layout lay = {0}; lay.ent = prog.ent[0]; lay.nent = prog.nent[0];
    prog.sh[0] = shader(dir, spilled ? "spill" : "reference", &lay);
    memset(guest, 0xa5, sizeof guest);
    const uint32_t args[3] = {2, 3, 2}; memcpy(guest + 1024, args, sizeof args);
    rp_state_reset(); rp_draw_reset(); nvn_unimpl_reset();
    rp_S.ssbo[5][0].addr = (uint64_t)(uintptr_t)guest;
    rp_S.ssbo[5][0].size = SPILL_WORDS * sizeof(uint32_t);
    GPU(nvn_gpu_cmd_begin(NULL, &active, &err));
    nvn_gpu_buf mirror = NULL; uint64_t off = 0;
    REQUIRE(rp_buf_resolve(active, (uint64_t)(uintptr_t)guest, SPILL_WORDS * 4, &mirror, &off));
    p_dispatch dp = {0};
    if (indirect) dp.ind = (uint64_t)(uintptr_t)(guest + 1024);
    else { dp.x = 2; dp.y = 3; dp.z = 2; }
    rp_dispatch(active, &dp, NULL); REQUIRE(nvn_unimpl_kinds() == 0);
    GPU(nvn_gpu_cmd_commit(active, serial, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(serial, 5000000000ull, &err));
    memcpy(result, (const uint8_t *)nvn_gpu_buf_cpu(mirror) + off, SPILL_WORDS * 4);
    for (int k = 0; k < nw; k++) rp_buf_gpu_write_land(wkey[k], wid[k], 0);
    nw = 0; rp_draw_reset(); rp_buf_reset(); nvn_gpu_shader_release(prog.sh[0]);
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    struct s2a_vk_device d; char why[256];
    if (s2a_vk_device_create(&d, why, sizeof why)) { fprintf(stderr, "%s\n", why); return 2; }
    nvn_gpu_open_desc od = {0}; od.host_device = &d; od.app_label = "replay-shared-spill";
    GPU(nvn_gpu_open(&od, &err));
    uint32_t ref[SPILL_WORDS], actual[SPILL_WORDS]; unsigned bad = 0; uint64_t serial = 0;
    for (int indirect = 0; indirect < 2; indirect++) {
        run_spill(argv[1], 0, indirect, ++serial, ref);
        run_spill(argv[1], 1, indirect, ++serial, actual);
        for (unsigned g = 0; g < SPILL_GROUPS; g++) for (unsigned i = 0; i < 9; i++) {
            uint32_t expected = i == 8 ? 100 : (g*100+i) ^ (g*200+(i+1)%8) ^ (g*300+7-i) ^ 36u ^ 64u;
            if (ref[g*9+i] != expected || actual[g*9+i] != ref[g*9+i]) {
                if (bad++ < 5) fprintf(stderr, "mismatch indirect=%d group=%u i=%u expected=%u ref=%u actual=%u\n", indirect, g, i, expected, ref[g*9+i], actual[g*9+i]);
            }
        }
    }
    printf("replay_spill groups=%d direct_indirect=2 byte_comparisons=%zu mismatches=%u\n", SPILL_GROUPS, 2*sizeof ref, bad);
    GPU(nvn_gpu_close(5000000000ull, &err)); REQUIRE(!s2a_vk_device_destroy(&d, 1));
    return bad ? 1 : 0;
}
