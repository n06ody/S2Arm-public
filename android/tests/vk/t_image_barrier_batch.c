// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define nvn_plat_flag backend_fixture_flag
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#undef nvn_plat_flag

static int batch_on, layout_on;

int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_image_barrier_batch")) return batch_on;
    if (!strcmp(name, "nvk_opt_layout")) return layout_on;
    return backend_fixture_flag(name, dflt);
}

enum { W = 32, H = 32, TARGETS = 5, NEW_IMAGES = 10 };

#define DS_STAGES (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | \
                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
#define SAMPLE_STAGES (VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)

static struct observation {
    const char *label;
    VkImageLayout from, to;
    VkPipelineStageFlags src, dst;
    VkAccessFlags read, write;
    unsigned calls, images, bad;
} observed[] = {
    {"init", VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
     0, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0, 0, 0},
    {"color-enter", VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
     VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, 0, 0},
    {"depth-enter", VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, DS_STAGES,
     VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0, 0, 0},
    {"color-sample", VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, SAMPLE_STAGES,
     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 0, 0},
    {"depth-sample", VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
     DS_STAGES, SAMPLE_STAGES,
     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 0, 0},
};
static pthread_mutex_t observed_mu = PTHREAD_MUTEX_INITIALIZER;

static PFN_vkCmdPipelineBarrier real_pipeline_barrier;

static VKAPI_ATTR void VKAPI_CALL probe_pipeline_barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t nm, const VkMemoryBarrier *mem, uint32_t nb, const VkBufferMemoryBarrier *buffers,
    uint32_t ni, const VkImageMemoryBarrier *images) {
    pthread_mutex_lock(&observed_mu);
    for (size_t j = 0; j < sizeof observed / sizeof observed[0]; j++) {
        struct observation *o = &observed[j];
        unsigned matching = 0;
        for (uint32_t i = 0; i < ni; i++) {
            const VkImageMemoryBarrier *b = &images[i];
            if (b->oldLayout != o->from || b->newLayout != o->to) continue;
            matching++;
            o->bad += b->srcAccessMask != o->read || b->dstAccessMask != o->write ||
                b->srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED || b->dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED;
        }
        if (matching) {
            o->calls++;
            o->images += matching;
            o->bad += src != o->src || dst != o->dst || flags || nm || nb || matching != ni;
        }
    }
    pthread_mutex_unlock(&observed_mu);
    real_pipeline_barrier(cb, src, dst, flags, nm, mem, nb, buffers, ni, images);
}

// 两种录制方式都从 Vulkan 设备入口表取指针，须在入口表上观测并继续调用校验层。
extern __typeof__(vkGetDeviceProcAddr) __real_vkGetDeviceProcAddr;
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice device, const char *name) {
    PFN_vkVoidFunction proc = __real_vkGetDeviceProcAddr(device, name);
    if (!proc || strcmp(name, "vkCmdPipelineBarrier")) return proc;
    real_pipeline_barrier = (PFN_vkCmdPipelineBarrier)proc;
    return (PFN_vkVoidFunction)probe_pipeline_barrier;
}

static void check_barriers(int expect_batch) {
    const unsigned images[] = {NEW_IMAGES, 4, 1, 3, 1};
    const unsigned batched[] = {2, 2, 1, 1, 1};
    for (size_t i = 0; i < sizeof observed / sizeof observed[0]; i++) {
        const struct observation *o = &observed[i];
        const int used = !i || layout_on;
        const unsigned calls = used ? (expect_batch ? batched[i] : images[i]) : 0;
        printf("batch %s calls=%u images=%u flags-bad=%u\n", o->label, o->calls, o->images, o->bad);
        CHECK(o->images == (used ? images[i] : 0) && !o->bad, "batch barrier scope %s", o->label);
        CHECK(o->calls == calls, "batch call count %s expected=%u got=%u", o->label, calls, o->calls);
    }
}

static void run_workload(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    const nvn_gpu_layout_ent ent = {.cls = NVN_GPU_RES_TEX_SAMP, .access = NVN_GPU_ACC_R,
                                     .count = 1, .stages = 1u << NVN_GPU_SH_FS};
    const nvn_gpu_layout layout = {.ent = &ent, .nent = 1};
    if (!load_shader_pair(dir, "tri3.vert", "feedback_combined.frag", NULL, &layout, &vs, &fs, &vm, &fm)) {
        CHECK(0, "batch sample shaders");
        return;
    }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err err = {0};
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err)), "batch sample pipe: %s", err.why);
    nvn_gpu_samp samp = NULL;
    samp_par sp = {0};
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &err)), "batch sampler: %s", err.why);
    const float verts[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    nvn_gpu_tex tex[NEW_IMAGES];
    nvn_gpu_view view[TARGETS];
    nvn_gpu_buf readback[TARGETS];
    for (int i = 0; i < NEW_IMAGES; i++) {
        tex[i] = make_tex(W, H, i == 3 ? NVN_FORMAT_DEPTH32F : NVN_FORMAT_RGBA8,
                          NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
        if (i < TARGETS) view[i] = make_view(tex[i]);
    }

    nvn_gpu_pass_desc pass = {.w = W, .h = H, .layers = 1, .ncolor = 3};
    for (int i = 0; i < 3; i++) {
        pass.color[i].view = view[i];
        pass.color[i].load = NVN_GPU_LOAD_CLEAR;
        pass.color[i].store = NVN_GPU_STORE_STORE;
        pass.color[i].clear.f[(i + 1) % 3] = pass.color[i].clear.f[3] = 1;
    }
    pass.depth.view = view[3];
    pass.depth.load = NVN_GPU_LOAD_CLEAR;
    pass.depth.store = NVN_GPU_STORE_STORE;
    pass.clear_depth = 0.5f;
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_pass_end(c);

    memset(&pass, 0, sizeof pass);
    pass.w = W; pass.h = H; pass.layers = 1; pass.ncolor = 1;
    pass.color[0].view = view[4];
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    dyn.vp[0][2] = W; dyn.vp[0][3] = H; dyn.sc[2] = W; dyn.sc[3] = H;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_binding b = {.cls = NVN_GPU_RES_TEX_SAMP, .u.ts = {.view = view[0], .samp = samp}};
    nvn_gpu_bind(c, NVN_GPU_SH_FS, &b, 1);
    nvn_gpu_draw_args draw = {.prim = 4, .count = 3, .instances = 1};
    nvn_gpu_draw(c, &draw);
    nvn_gpu_pass_end(c);
    for (int i = 0; i < TARGETS; i++) {
        readback[i] = make_buf(W * H * 4, NVN_GPU_MEM_SHARED, NULL);
        nvn_gpu_copy_tex_buf(c, tex[i], 0, 0, NULL, readback[i], 0, W * 4, W * H * 4);
    }
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &err);
    CHECK(OK(r), "batch commit: %s", err.why);
    if (OK(r)) {
        r = nvn_gpu_fence_wait(1, 5000000000ull, &err);
        CHECK(OK(r), "batch retire: %s", err.why);
    }
    static const uint8_t expected[TARGETS][4] = {
        {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 0, 0, 255}, {0}, {64, 255, 0, 255},
    };
    unsigned good = 0;
    for (int i = 0; i < TARGETS; i++) {
        const void *data = buf_cpu(readback[i]);
        for (int p = 0; OK(r) && data && p < W * H; p++)
            good += i == 3 ? ((const float *)data)[p] == 0.5f :
                    !memcmp((const uint8_t *)data + p * 4, expected[i], 4);
        nvn_gpu_buf_release(readback[i], NULL, NULL);
        nvn_gpu_view_release(view[i]);
    }
    printf("batch pixels=%u/%d\n", good, TARGETS * W * H);
    CHECK(good == TARGETS * W * H, "batch pixel mismatch");
    for (int i = 0; i < NEW_IMAGES; i++) nvn_gpu_tex_release(tex[i]);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_samp_release(samp);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
    free(vm);
    free(fm);
}

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    batch_on = atoi(argv[2]);
    layout_on = atoi(argv[3]);
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) {
        fprintf(stderr, "device: %s\n", why);
        return 2;
    }
    g_desc = (nvn_gpu_open_desc){.host_device = &g_dev, .app_label = "image-barrier-batch"};
    if (!open_backend()) return 2;
    run_workload(argv[1]);
    check_barriers(atoi(argv[4]));
    extern uint64_t nvn_vk_dbg_dq_recs(void);
    const uint64_t records = nvn_vk_dbg_dq_recs();
    CHECK(!!records == !!getenv("S2A_TEST_NVK_DQ"), "batch DQ mode records=%llu", (unsigned long long)records);
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "close: %s", err.why);
    CHECK(!s2a_vk_device_destroy(&g_dev, 1), "destroy device");
    printf("batch Vulkan failures=%d\n", g_fail);
    return g_fail ? 1 : 0;
}
