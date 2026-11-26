// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define nvn_plat_flag backend_fixture_flag
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#undef nvn_plat_flag

static int g_write_run = -1;  // -1 exercises the real backend default.
static _Atomic int g_watch;
static unsigned g_full, g_ncopy, g_full_at_copy[64];

int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_buf_write_bar_diag")) return 1;
    if (!strcmp(name, "nvk_buf_write_bar_coalesce") && g_write_run >= 0) return g_write_run;
    return backend_fixture_flag(name, dflt);
}

static PFN_vkCmdPipelineBarrier g_real_barrier;
static PFN_vkCmdCopyBuffer g_real_copy;

static VKAPI_ATTR void VKAPI_CALL capture_barrier(
    VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags flags,
    uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b,
    uint32_t ni, const VkImageMemoryBarrier *i) {
    if (atomic_load_explicit(&g_watch, memory_order_relaxed) &&
        src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && dst == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
        flags == 0 && nm == 1 && !nb && !ni && m[0].srcAccessMask == VK_ACCESS_MEMORY_WRITE_BIT &&
        m[0].dstAccessMask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)) g_full++;
    g_real_barrier(cb, src, dst, flags, nm, m, nb, b, ni, i);
}

static VKAPI_ATTR void VKAPI_CALL capture_copy(
    VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy *r) {
    if (atomic_load_explicit(&g_watch, memory_order_relaxed) &&
        g_ncopy < sizeof g_full_at_copy / sizeof *g_full_at_copy) g_full_at_copy[g_ncopy++] = g_full;
    g_real_copy(cb, src, dst, n, r);
}

// DQ resolves these commands through the per-device dispatch table in both modes.
extern __typeof__(vkGetDeviceProcAddr) __real_vkGetDeviceProcAddr;
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice device, const char *name) {
    PFN_vkVoidFunction entry = __real_vkGetDeviceProcAddr(device, name);
    if (!entry) return NULL;
    if (!strcmp(name, "vkCmdPipelineBarrier")) {
        g_real_barrier = (PFN_vkCmdPipelineBarrier)entry;
        return (PFN_vkVoidFunction)capture_barrier;
    }
    if (!strcmp(name, "vkCmdCopyBuffer")) {
        g_real_copy = (PFN_vkCmdCopyBuffer)entry;
        return (PFN_vkVoidFunction)capture_copy;
    }
    return entry;
}

static uint64_t g_signal;
static void trace_begin(void) {
    g_full = g_ncopy = 0;
    memset(g_full_at_copy, 0, sizeof g_full_at_copy);
    atomic_store_explicit(&g_watch, 1, memory_order_relaxed);
}

static void trace_retire(nvn_gpu_cmd c) {
    nvn_gpu_err err = {0};
    ++g_signal;
    CHECK(OK(nvn_gpu_cmd_commit(c, g_signal, on_done, NULL, &err)), "write command commit: %s", err.why);
    CHECK(OK(nvn_gpu_fence_wait(g_signal, 5000000000ull, &err)), "write command retire: %s", err.why);
    atomic_store_explicit(&g_watch, 0, memory_order_relaxed);
}

static void check_contiguous(int expect) {
    uint8_t zeros[64] = {0}, want_a[64] = {0}, want_b[64] = {0};
    nvn_gpu_buf a = make_buf(sizeof zeros, NVN_GPU_MEM_SHARED, zeros);
    nvn_gpu_buf b = make_buf(sizeof zeros, NVN_GPU_MEM_SHARED, zeros);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_buf dst[] = {a, a, b, a, a};
    const size_t off[] = {0, 16, 0, 8, 48};
    trace_begin();
    for (unsigned i = 0; i < sizeof dst / sizeof *dst; i++) {
        uint8_t bytes[16];
        memset(bytes, (int)i + 1, sizeof bytes);
        nvn_gpu_buf_write(c, dst[i], off[i], bytes, sizeof bytes);
        memcpy((dst[i] == a ? want_a : want_b) + off[i], bytes, sizeof bytes);
        if (!i) nvn_gpu_buf_write(c, a, off[i], NULL, 0);
    }
    trace_retire(c);
    const unsigned folded[] = {1, 1, 1, 2, 2};
    CHECK(g_ncopy == sizeof dst / sizeof *dst, "write run copies=%u", g_ncopy);
    for (unsigned i = 0; i < sizeof dst / sizeof *dst; i++)
        CHECK(g_full_at_copy[i] == (expect ? folded[i] : i + 1),
              "write run before copy %u barriers=%u expect=%u", i, g_full_at_copy[i], expect ? folded[i] : i + 1);
    const uint8_t *actual_a = buf_cpu(a), *actual_b = buf_cpu(b);
    CHECK(actual_a && !memcmp(actual_a, want_a, sizeof want_a), "write run bytes A");
    CHECK(actual_b && !memcmp(actual_b, want_b, sizeof want_b), "write run bytes B");
    printf("write run copies=%u barriers=%u,%u,%u,%u,%u\n", g_ncopy, g_full_at_copy[0],
           g_full_at_copy[1], g_full_at_copy[2], g_full_at_copy[3], g_full_at_copy[4]);
    nvn_gpu_buf_release(a, NULL, NULL);
    nvn_gpu_buf_release(b, NULL, NULL);
}

static void check_capacity_unknown(int expect) {
    uint32_t zeros[64] = {0}, want[64] = {0};
    nvn_gpu_buf dst = make_buf(sizeof zeros, NVN_GPU_MEM_SHARED, zeros);
    nvn_gpu_buf read = make_buf(sizeof(uint32_t), NVN_GPU_MEM_SHARED, zeros);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_slice source;
    nvn_gpu_err err = {0};
    if (!OK(nvn_gpu_arena_alloc(c, sizeof(uint32_t), 4, &source, &err))) {
        CHECK(0, "staging target allocation: %s", err.why);
        nvn_gpu_cmd_abort(c);
        return;
    }
    memset(source.cpu, 0, sizeof(uint32_t));
    trace_begin();
    // 16 non-overlapping words fill the run. The 17th requires a new full barrier.
    for (unsigned i = 0; i < 17; i++) {
        want[i] = i + 1;
        nvn_gpu_buf_write(c, dst, i * sizeof(uint32_t), &want[i], sizeof(uint32_t));
    }
    const uint32_t staging_value = 0x22334455;
    nvn_gpu_buf_write(c, source.buf, source.off, &staging_value, sizeof staging_value);
    want[24] = 65;
    nvn_gpu_buf_write(c, dst, 24 * sizeof(uint32_t), &want[24], sizeof(uint32_t));
    nvn_gpu_copy_buf(c, source.buf, source.off, read, 0, sizeof(uint32_t));
    trace_retire(c);
    CHECK(g_ncopy == 20, "capacity copy count %u", g_ncopy);
    for (unsigned i = 0; i < 20; i++) {
        const unsigned expected = expect ? (i < 16 ? 1 : i - 14) : i + 1;
        CHECK(g_full_at_copy[i] == expected, "capacity/unknown before copy %u: %u expect=%u",
              i, g_full_at_copy[i], expected);
    }
    CHECK(!memcmp(buf_cpu(dst), want, sizeof want), "capacity bytes");
    CHECK(!memcmp(buf_cpu(read), &staging_value, sizeof staging_value), "staging target readback");
    nvn_gpu_buf_release(dst, NULL, NULL);
    nvn_gpu_buf_release(read, NULL, NULL);
}

static void check_ordered_boundary(int is_readback) {
    uint32_t zeros[8] = {0};
    nvn_gpu_buf buf = make_buf(sizeof zeros, NVN_GPU_MEM_SHARED, zeros);
    nvn_gpu_buf read = make_buf(sizeof zeros, NVN_GPU_MEM_SHARED, zeros);
    const uint32_t data[] = {22, 33};
    nvn_gpu_cmd c = begin_cmd();
    trace_begin();
    nvn_gpu_buf_write(c, buf, 0, &data[0], 4);
    if (is_readback) nvn_gpu_copy_buf(c, buf, 0, read, 0, 4);
    else nvn_gpu_barrier(c, NVN_BARRIER_ORDER_FRAGMENTS_BIT);
    nvn_gpu_buf_write(c, buf, 4, &data[1], 4);
    trace_retire(c);
    CHECK(g_ncopy == 2u + is_readback && g_full_at_copy[0] == 1 && g_full_at_copy[g_ncopy - 1] == 3,
          "%s boundary: copies=%u, barriers=%u,%u,%u", is_readback ? "readback" : "explicit barrier",
          g_ncopy, g_full_at_copy[0], g_full_at_copy[1], g_full_at_copy[2]);
    CHECK(!memcmp(buf_cpu(buf), data, sizeof data), "ordered boundary writeback bytes");
    if (is_readback) CHECK(!memcmp(buf_cpu(read), data, 4), "readback before next write");
    nvn_gpu_buf_release(buf, NULL, NULL);
    nvn_gpu_buf_release(read, NULL, NULL);
}

static void check_dispatch(const char *dir, int expect) {
    char path[512];
    snprintf(path, sizeof path, "%s/comp.comp.spv", dir);
    size_t len = 0;
    void *blob = read_file(path, &len);
    if (!blob) { CHECK(0, "compute shader fixture"); return; }
    const nvn_gpu_layout_ent entry = {.cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .count = 1,
                                      .stages = 1u << NVN_GPU_SH_CS};
    const nvn_gpu_layout layout = {.ent = &entry, .nent = 1};
    const nvn_gpu_shader_src src = {.stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV, .entry = "main",
                                   .blob = blob, .bytes = len, .layout = &layout};
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_shader_load(&src, &sh, &err)), "compute shader: %s", err.why);
    const nvn_gpu_stage_desc stage = {.sh = sh};
    CHECK(OK(nvn_gpu_cpipe_get(&stage, NVN_GPU_PIPE_SYNC, &pipe, &err)), "compute pipe: %s", err.why);
    if (!pipe) { if (sh) nvn_gpu_shader_release(sh); free(blob); return; }
    uint32_t zeros[64] = {0}, launch[] = {1, 1, 1, 29};
    nvn_gpu_buf result = make_buf(sizeof zeros, NVN_GPU_MEM_SHARED, zeros);
    nvn_gpu_buf indirect = make_buf(sizeof launch, NVN_GPU_MEM_SHARED, zeros);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_binding binding = {.cls = NVN_GPU_RES_SSBO, .u.b = {.buf = result, .len = sizeof zeros}};
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_bind(c, NVN_GPU_SH_CS, &binding, 1);
    trace_begin();
    nvn_gpu_buf_write(c, indirect, 0, launch, 4);
    nvn_gpu_buf_write(c, indirect, 4, launch + 1, 8);
    nvn_gpu_dispatch(c, pipe, 0, 0, 0, indirect, 0);
    nvn_gpu_buf_write(c, indirect, 12, launch + 3, 4);
    trace_retire(c);
    CHECK(g_ncopy == 3 && g_full_at_copy[0] == 1 && g_full_at_copy[1] == (expect ? 1u : 2u) &&
          g_full_at_copy[2] == (expect ? 3u : 4u),
          "indirect dispatch boundary: copies=%u, barriers=%u,%u,%u",
          g_ncopy, g_full_at_copy[0], g_full_at_copy[1], g_full_at_copy[2]);
    uint32_t expected[64] = {0};
    for (unsigned i = 0; i < 16; i++) expected[i] = i + 1;
    const uint8_t *actual = buf_cpu(result);
    CHECK(actual && !memcmp(actual, expected, sizeof expected), "indirect arguments dispatch and result bytes");
    CHECK(!memcmp(buf_cpu(indirect), launch, sizeof launch), "indirect buffer bytes");
    nvn_gpu_buf_release(result, NULL, NULL);
    nvn_gpu_buf_release(indirect, NULL, NULL);
    nvn_gpu_shader_release(sh);
    free(blob);
}

static void check_draw(const char *dir, int expect) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vs_bytes = NULL, *fs_bytes = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vs_bytes, &fs_bytes)) {
        CHECK(0, "draw shader fixture");
        return;
    }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err)), "draw pipe: %s", err.why);
    if (!pipe) return;
    const float verts[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
    nvn_gpu_buf vb = make_buf(sizeof verts + 16, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_tex tex = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view view = make_view(tex);
    nvn_gpu_buf read = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd();
    trace_begin();
    nvn_gpu_buf_write(c, vb, 0, verts, sizeof verts[0]);
    nvn_gpu_buf_write(c, vb, sizeof verts[0], verts + 1, 2 * sizeof verts[0]);
    nvn_gpu_pass_desc pass = {.w = 64, .h = 64, .layers = 1, .ncolor = 1};
    pass.color[0].view = view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    const nvn_gpu_draw_args draw = {.prim = 4, .count = 3, .instances = 1};
    nvn_gpu_draw(c, &draw);
    nvn_gpu_pass_end(c);
    nvn_gpu_buf_write(c, vb, sizeof verts, verts, 16);
    nvn_gpu_copy_tex_buf(c, tex, 0, 0, NULL, read, 0, 64 * 4, 64 * 64 * 4);
    trace_retire(c);
    CHECK(g_ncopy == 3 && g_full_at_copy[0] == 1 && g_full_at_copy[1] == (expect ? 1u : 2u) &&
          g_full_at_copy[2] == (expect ? 2u : 3u), "draw boundary: copies=%u, barriers=%u,%u,%u",
          g_ncopy, g_full_at_copy[0], g_full_at_copy[1], g_full_at_copy[2]);
    const uint8_t *pixels = buf_cpu(read);
    const uint8_t green[4] = {0, 255, 0, 255};
    unsigned good = 0;
    for (unsigned i = 0; pixels && i < 64 * 64; i++) good += !memcmp(pixels + i * 4, green, 4);
    CHECK(good == 64 * 64, "draw after coalesced vertex writes: %u green pixels", good);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(read, NULL, NULL);
    nvn_gpu_view_release(view);
    nvn_gpu_tex_release(tex);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
    free(vs_bytes);
    free(fs_bytes);
}

struct write_counts { uint64_t skipped, overlap, capacity, unknown, intervening, segment; };
static void take_write_stats(void *user, const char *name, uint64_t value) {
    struct write_counts *s = user;
    if (!strcmp(name, "buf_write_bar_skipped")) s->skipped = value;
    else if (!strcmp(name, "buf_write_bar_overlap")) s->overlap = value;
    else if (!strcmp(name, "buf_write_bar_capacity")) s->capacity = value;
    else if (!strcmp(name, "buf_write_bar_unknown")) s->unknown = value;
    else if (!strcmp(name, "buf_write_bar_intervening")) s->intervening = value;
    else if (!strcmp(name, "buf_write_bar_segment")) s->segment = value;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    g_write_run = atoi(argv[1]);
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    g_desc = (nvn_gpu_open_desc){.host_device = &g_dev, .app_label = "buf-write-bar-coalesce"};
    if (!open_backend()) { s2a_vk_device_destroy(&g_dev, 1); return 2; }
    const int expect = atoi(argv[2]);
    check_contiguous(expect);
    check_capacity_unknown(expect);
    check_ordered_boundary(0);
    check_ordered_boundary(1);
    check_dispatch(argv[3], expect);
    check_draw(argv[3], expect);
    struct write_counts counts = {0};
    nvn_gpu_stats(take_write_stats, &counts, 0);
    CHECK(counts.skipped == (expect ? 20u : 0u) && counts.overlap == (unsigned)expect &&
          counts.capacity == (unsigned)expect && counts.unknown == (unsigned)expect &&
          counts.intervening == (expect ? 6u : 0u) && counts.segment == (expect ? 4u : 0u),
          "write diagnostics: skip=%llu overlap=%llu capacity=%llu unknown=%llu intervening=%llu segment=%llu",
          (unsigned long long)counts.skipped, (unsigned long long)counts.overlap,
          (unsigned long long)counts.capacity, (unsigned long long)counts.unknown,
          (unsigned long long)counts.intervening, (unsigned long long)counts.segment);
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "write run close: %s", err.why);
    CHECK(!s2a_vk_device_destroy(&g_dev, 1), "destroy device");
    printf("write run failures=%d\n", g_fail);
    return g_fail != 0;
}
