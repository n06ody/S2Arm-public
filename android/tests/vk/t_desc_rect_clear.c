// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define nvn_plat_flag backend_fixture_flag
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#undef nvn_plat_flag

enum { FULL, INSIDE, REOPEN };
static int keep_on, memo_on, none_on, push_on, area_mode;

int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_desc_keep_rect_clear")) return keep_on;
    if (!strcmp(name, "nvk_desc_memo")) return memo_on;
    if (!strcmp(name, "nvk_load_store_op_none")) return none_on;
    if (!strcmp(name, "nvk_push_desc")) return push_on;
    return backend_fixture_flag(name, dflt);
}

static atomic_uint graphics_binds, compute_binds, dynamic_offsets, update_calls, writes;
static atomic_uint graphics_pushes, compute_pushes;
static PFN_vkCmdBindDescriptorSets real_bind;
static PFN_vkCmdPushDescriptorSetKHR real_push;

static pthread_mutex_t pass_observer = PTHREAD_MUTEX_INITIALIZER;
static struct pass_ops {
    VkRenderPass pass;
    unsigned load, store;
} pass_ops[64];
static struct pass_begin {
    VkRect2D area;
    unsigned load, store;
} pass_begins[16];
static unsigned nops, nbegins;

extern __typeof__(vkCreateRenderPass) __real_vkCreateRenderPass;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateRenderPass(VkDevice dev, const VkRenderPassCreateInfo *ci,
    const VkAllocationCallbacks *alloc, VkRenderPass *out) {
    const VkResult rc = __real_vkCreateRenderPass(dev, ci, alloc, out);
    if (area_mode && rc == VK_SUCCESS) {
        pthread_mutex_lock(&pass_observer);
        unsigned slot = 0;
        while (slot < nops && pass_ops[slot].pass != *out) slot++;
        CHECK(slot < sizeof pass_ops / sizeof pass_ops[0], "desc-clear pass observer capacity");
        if (slot < sizeof pass_ops / sizeof pass_ops[0]) {
            struct pass_ops ops = {.pass = *out};
            for (uint32_t i = 0; i < ci->attachmentCount; i++) {
                ops.load += ci->pAttachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD;
                ops.store += ci->pAttachments[i].storeOp == VK_ATTACHMENT_STORE_OP_STORE;
            }
            pass_ops[slot] = ops;
            if (slot == nops) nops++;
        }
        pthread_mutex_unlock(&pass_observer);
    }
    return rc;
}

extern __typeof__(vkCmdBeginRenderPass) __real_vkCmdBeginRenderPass;
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi,
    VkSubpassContents contents) {
    if (area_mode) {
        pthread_mutex_lock(&pass_observer);
        unsigned slot = 0;
        while (slot < nops && pass_ops[slot].pass != bi->renderPass) slot++;
        CHECK(slot < nops && nbegins < sizeof pass_begins / sizeof pass_begins[0], "desc-clear unknown physical pass");
        if (slot < nops && nbegins < sizeof pass_begins / sizeof pass_begins[0])
            pass_begins[nbegins++] = (struct pass_begin){bi->renderArea, pass_ops[slot].load, pass_ops[slot].store};
        pthread_mutex_unlock(&pass_observer);
    }
    __real_vkCmdBeginRenderPass(cb, bi, contents);
}

static VKAPI_ATTR void VKAPI_CALL record_bind(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout,
    uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t ndyn, const uint32_t *offsets) {
    if (point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        atomic_fetch_add(&graphics_binds, 1);
        atomic_fetch_add(&dynamic_offsets, ndyn);
    } else if (point == VK_PIPELINE_BIND_POINT_COMPUTE) {
        atomic_fetch_add(&compute_binds, 1);
    }
    real_bind(cb, point, layout, first, count, sets, ndyn, offsets);
}

static VKAPI_ATTR void VKAPI_CALL record_push(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout,
    uint32_t set, uint32_t count, const VkWriteDescriptorSet *writes) {
    if (point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        atomic_fetch_add(&graphics_binds, 1);
        atomic_fetch_add(&graphics_pushes, 1);
    } else if (point == VK_PIPELINE_BIND_POINT_COMPUTE) {
        atomic_fetch_add(&compute_binds, 1);
        atomic_fetch_add(&compute_pushes, 1);
    }
    real_push(cb, point, layout, set, count, writes);
}

// 直录和两级录制都从设备入口表取 vkCmdBindDescriptorSets。
extern __typeof__(vkGetDeviceProcAddr) __real_vkGetDeviceProcAddr;
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice dev, const char *name) {
    PFN_vkVoidFunction fn = __real_vkGetDeviceProcAddr(dev, name);
    if (fn && !strcmp(name, "vkCmdBeginRenderPass")) return (PFN_vkVoidFunction)__wrap_vkCmdBeginRenderPass;
    if (fn && !strcmp(name, "vkCmdPushDescriptorSetKHR")) {
        real_push = (PFN_vkCmdPushDescriptorSetKHR)fn;
        return (PFN_vkVoidFunction)record_push;
    }
    if (!fn || strcmp(name, "vkCmdBindDescriptorSets")) return fn;
    real_bind = (PFN_vkCmdBindDescriptorSets)fn;
    return (PFN_vkVoidFunction)record_bind;
}

extern __typeof__(vkUpdateDescriptorSets) __real_vkUpdateDescriptorSets;
VKAPI_ATTR void VKAPI_CALL __wrap_vkUpdateDescriptorSets(VkDevice dev, uint32_t nw, const VkWriteDescriptorSet *wr,
    uint32_t nc, const VkCopyDescriptorSet *copies) {
    atomic_fetch_add(&update_calls, 1);
    atomic_fetch_add(&writes, nw);
    __real_vkUpdateDescriptorSets(dev, nw, wr, nc, copies);
}

enum { W = 32, H = 32, TARGETS = 4, COMPUTE_WORDS = 16 };

static void begin_pass(nvn_gpu_cmd c, nvn_gpu_view view, nvn_gpu_view depth, nvn_gpu_pipe p, nvn_gpu_buf vb,
                       unsigned area_width) {
    nvn_gpu_pass_desc pass = {.w = W, .h = H, .layers = 1, .ncolor = 1,
                              .area_w = area_width, .area_h = area_width ? H : 0};
    pass.color[0].view = view;
    pass.color[0].load = area_width ? NVN_GPU_LOAD_LOAD : NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[3] = 1;
    if (depth) {
        pass.depth.view = depth;
        pass.depth.load = NVN_GPU_LOAD_LOAD;
        pass.depth.store = NVN_GPU_STORE_STORE;
    }
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, p);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
}

static void draw_range(nvn_gpu_cmd cmd, unsigned x, unsigned w) {
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    dyn.vp[0][2] = W; dyn.vp[0][3] = H;
    dyn.sc[0] = (int)x; dyn.sc[2] = w; dyn.sc[3] = H;
    nvn_gpu_set_dyn(cmd, &dyn);
    const nvn_gpu_draw_args draw = {.prim = 4, .count = 3, .instances = 1};
    nvn_gpu_draw(cmd, &draw);
}

static int retire(nvn_gpu_cmd cmd, uint64_t signal) {
    nvn_gpu_err err = {0};
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(cmd, signal, on_done, NULL, &err);
    CHECK(OK(rc), "desc-clear commit: %s", err.why);
    if (!OK(rc)) return 0;
    rc = nvn_gpu_fence_wait(signal, 5000000000ull, &err);
    CHECK(OK(rc), "desc-clear wait: %s", err.why);
    return OK(rc);
}

static void check_calls(unsigned expected_graphics, unsigned expected_updates, unsigned expected_writes, unsigned segment) {
    unsigned gfx = atomic_load(&graphics_binds), cs = atomic_load(&compute_binds);
    unsigned dyn = atomic_load(&dynamic_offsets), upd = atomic_load(&update_calls), entries = atomic_load(&writes);
    const unsigned gfx_push = atomic_load(&graphics_pushes), cs_push = atomic_load(&compute_pushes);
    printf("desc-clear calls segment=%u graphics=%u compute=%u sets-updated=%u writes=%u dynamic=%u graphics-push=%u compute-push=%u\n",
           segment, gfx, cs, upd, entries, dyn, gfx_push, cs_push);
    CHECK(gfx == expected_graphics && upd == expected_updates && entries == expected_writes,
          "desc-clear call count segment=%u wanted graphics=%u updates=%u writes=%u",
          segment, expected_graphics, expected_updates, expected_writes);
    CHECK(cs == 1 && dyn == (memo_on ? gfx : 0) &&
          gfx_push == (push_on && !memo_on ? gfx : 0) && cs_push == (unsigned)push_on,
          "desc-clear push or dynamic bind count segment=%u", segment);
}

static unsigned check_pixels(nvn_gpu_buf buf, unsigned target) {
    static const uint8_t rgba[][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {0, 0, 0, 255}};
    const uint8_t *data = buf_cpu(buf);
    unsigned good = 0;
    for (unsigned y = 0; data && y < H; y++) for (unsigned x = 0; x < W; x++) {
        unsigned color = 0;
        if (!target) color = (x / 8 == 1) ? 2 : (x / 8 == 3) ? 3 : 0;
        else if (target == 1) color = x >= W / 2;
        else if (target == 2) color = 1;
        good += !memcmp(data + (y * W + x) * 4, rgba[color], 4);
    }
    CHECK(good == W * H, "desc-clear pixels target=%u correct=%u/%u", target, good, W * H);
    return good;
}

static void check_area(nvn_gpu_buf rb) {
    const float *pixels = (const float *)buf_cpu(rb);
    unsigned good = 0, outside = 0;
    for (unsigned y = 0; pixels && y < H; y++) for (unsigned x = 0; x < W; x++) {
        const int cleared = x >= 8 && x < 16;
        const int equal = pixels[y * W + x] == (cleared ? 0.5f : 0.75f);
        good += equal;
        outside += equal && !cleared;
    }
    CHECK(good == W * H, "desc-clear area depth pixels=%u/%u", good, W * H);
    pthread_mutex_lock(&pass_observer);
    const unsigned reopen = area_mode == REOPEN;
    const unsigned width = reopen ? 8 : 24;
    const struct pass_begin first = pass_begins[1], second = pass_begins[2];
    CHECK(nbegins == 4 + reopen, "desc-clear area physical passes=%u expected=%u", nbegins, 4 + reopen);
    CHECK(!first.area.offset.x && !first.area.offset.y && first.area.extent.width == width && first.area.extent.height == H &&
          first.load == 2 && first.store == 2, "desc-clear first area size=%ux%u LOAD/STORE=%u/%u",
          first.area.extent.width, first.area.extent.height, first.load, first.store);
    if (reopen) CHECK(!second.area.offset.x && !second.area.offset.y && second.area.extent.width == W && second.area.extent.height == H &&
                      second.load == 2 && second.store == 2, "desc-clear full-area reopen LOAD/STORE=%u/%u", second.load, second.store);
    printf("desc-clear area width=%u physical-passes=%u depth=%u/%u unmodified-depth=%u/%u\n",
           first.area.extent.width, nbegins, good, W * H, outside, (W - 8) * H);
    pthread_mutex_unlock(&pass_observer);
}

static void run_clear_workload(const char *dir, int expect_keep) {
    const uint32_t fs_mask = 1u << NVN_GPU_SH_FS;
    const nvn_gpu_layout_ent ents[] = {
        {.cls = NVN_GPU_RES_TEX, .access = NVN_GPU_ACC_R, .binding = 0, .count = 1, .stages = fs_mask},
        {.cls = NVN_GPU_RES_SAMP, .access = NVN_GPU_ACC_R, .binding = 1, .count = 1, .stages = fs_mask},
        {.cls = NVN_GPU_RES_UBO, .access = NVN_GPU_ACC_R, .binding = 2, .count = 1, .stages = fs_mask},
    };
    const nvn_gpu_layout fs_layout = {.ent = ents, .nent = 3};
    nvn_gpu_shader vs = NULL, fs = NULL, cs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "desc_rect_clear.frag", NULL, &fs_layout, &vs, &fs, &vm, &fm)) {
        CHECK(0, "desc-clear shaders");
        return;
    }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, area_mode || none_on ? NVN_FORMAT_DEPTH32F : 0);
    if (area_mode) { pd.ds.dt = 1; pd.ds.df = NVN_DEPTH_FUNC_LESS; }
    nvn_gpu_err err = {0};
    nvn_gpu_pipe gp = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &gp, &err)), "desc-clear graphics pipe: %s", err.why);
    const nvn_gpu_layout_ent cs_ent = {.cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS};
    const nvn_gpu_layout cs_layout = {.ent = &cs_ent, .nent = 1};
    nvn_gpu_pipe cp = mk_cpipe(dir, "comp.comp", &cs_layout, &cs);
    CHECK(cp != NULL, "desc-clear compute pipe");
    const float verts[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    const float scale[2][4] = {{1, 0, 1, 1}, {0, 1, 1, 1}};
    size_t stride = g_dev.properties.limits.minUniformBufferOffsetAlignment;
    if (stride < sizeof scale[0]) stride = sizeof scale[0];
    uint8_t *uniforms = calloc(2, stride);
    if (!uniforms) { CHECK(0, "desc-clear UBO fixture allocation"); return; }
    memcpy(uniforms, scale[0], sizeof scale[0]);
    memcpy(uniforms + stride, scale[1], sizeof scale[1]);
    nvn_gpu_buf ubo = make_buf(2 * stride, NVN_GPU_MEM_SHARED, uniforms);
    free(uniforms);
    nvn_gpu_buf out_cs = make_buf(COMPUTE_WORDS * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_tex input = make_tex(1, 1, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view input_view = make_view(input);
    nvn_gpu_samp sampler = NULL;
    samp_par sp = {0};
    CHECK(OK(nvn_gpu_samp_create(&sp, &sampler, &err)), "desc-clear sampler: %s", err.why);
    nvn_gpu_binding bind[] = {
        {.ent = 0, .cls = NVN_GPU_RES_TEX, .u.view = input_view},
        {.ent = 1, .cls = NVN_GPU_RES_SAMP, .u.samp = sampler},
        {.ent = 2, .cls = NVN_GPU_RES_UBO, .u.b = {.buf = ubo, .len = sizeof scale[0]}},
    };
    const nvn_gpu_binding cb = {.cls = NVN_GPU_RES_SSBO, .u.b = {.buf = out_cs, .len = COMPUTE_WORDS * 4}};
    nvn_gpu_tex tex[TARGETS];
    nvn_gpu_view views[TARGETS];
    nvn_gpu_buf rb[TARGETS];
    for (unsigned i = 0; i < TARGETS; i++) {
        tex[i] = make_tex(W, H, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
        views[i] = make_view(tex[i]);
        rb[i] = make_buf(W * H * 4, NVN_GPU_MEM_SHARED, NULL);
    }

    nvn_gpu_tex tex_depth = NULL;
    nvn_gpu_view depth = NULL;
    const uint8_t yellow[4] = {255, 255, 0, 255};
    nvn_gpu_cmd cmd = begin_cmd();
    nvn_gpu_tex_upload(cmd, input, 0, 0, NULL, yellow, 4, sizeof yellow);
    nvn_gpu_buf rb_depth = NULL;
    if (area_mode || none_on) {
        tex_depth = make_tex(W, H, NVN_FORMAT_DEPTH32F, NVN_GPU_TEX_RENDER);
        depth = make_view(tex_depth);
        nvn_gpu_pass_desc init = {.w = W, .h = H, .layers = 1, .clear_depth = 0.75f};
        init.depth.view = depth;
        init.depth.load = NVN_GPU_LOAD_CLEAR;
        init.depth.store = NVN_GPU_STORE_STORE;
        if (area_mode) {
            init.ncolor = 1;
            init.color[0] = (nvn_gpu_att){.view = views[0], .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE};
            init.color[0].clear.f[3] = 1;
            rb_depth = make_buf(W * H * sizeof(float), NVN_GPU_MEM_SHARED, NULL);
        }
        nvn_gpu_pass_begin(cmd, &init);
        nvn_gpu_pass_end(cmd);
    }
    begin_pass(cmd, views[0], depth, gp, vb, area_mode == INSIDE ? 24 : area_mode == REOPEN ? 8 : 0);
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, bind, 3);
    draw_range(cmd, 0, 8);
    nvn_gpu_clear blue = {.color_mask = 0xf, .x = 8, .w = 8, .h = H};
    blue.value.f[2] = blue.value.f[3] = 1;
    blue.depth = depth != NULL;
    blue.depth_value = 0.5f;
    nvn_gpu_clear_rect(cmd, &blue);
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, bind, 3);
    draw_range(cmd, 16, 8);
    nvn_gpu_pass_end(cmd);

    // 普通过遍应沿用绑定；只改变 UBO 的动态偏移时确实重绑。
    begin_pass(cmd, views[1], depth, gp, vb, 0);
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, bind, 3);
    draw_range(cmd, 0, W);
    bind[2].u.b.off = stride;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, bind, 3);
    draw_range(cmd, W / 2, W / 2);
    nvn_gpu_pass_end(cmd);

    nvn_gpu_set_pipe(cmd, cp);
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &cb, 1);
    nvn_gpu_dispatch(cmd, cp, 1, 1, 1, NULL, 0);
    begin_pass(cmd, views[2], depth, gp, vb, 0);
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, bind, 3);
    draw_range(cmd, 0, W);
    nvn_gpu_pass_end(cmd);
    for (unsigned i = 0; i < TARGETS - 1; i++)
        nvn_gpu_copy_tex_buf(cmd, tex[i], 0, 0, NULL, rb[i], 0, W * 4, W * H * 4);
    if (rb_depth) nvn_gpu_copy_tex_buf(cmd, tex_depth, 0, 0, NULL, rb_depth, 0, W * sizeof(float), W * H * sizeof(float));
    if (retire(cmd, 1)) {
        const uint32_t *data = (const uint32_t *)buf_cpu(out_cs);
        for (unsigned i = 0; data && i < COMPUTE_WORDS; i++)
            CHECK(data[i] == i + 1, "desc-clear CS word=%u value=%u", i, data[i]);
        CHECK(data, "desc-clear CS output missing");
        if (rb_depth) check_area(rb_depth);
    }
    // 图形三项在同一组，有 memo 时保留动态 UBO，整组按段只写一次。
    // 计算的 SSBO 组仍可推送。关 memo 后，同组图形描述符也能推送。
    const int graphics_push = push_on && !memo_on;
    const unsigned expected_gfx = 3 + !expect_keep;
    const unsigned expected_updates = graphics_push ? 0 : memo_on ? 1 + !push_on : 4 + !expect_keep;
    const unsigned expected_writes = graphics_push ? 0 : memo_on ? 3 + !push_on : 10 + 3 * !expect_keep;
    check_calls(expected_gfx, expected_updates, expected_writes, 1);

    cmd = begin_cmd();
    begin_pass(cmd, views[3], depth, gp, vb, 0);
    bind[2].u.b.off = 0;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, bind, 3);
    draw_range(cmd, 0, W);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, tex[3], 0, 0, NULL, rb[3], 0, W * 4, W * H * 4);
    retire(cmd, 2);
    check_calls(expected_gfx + 1, expected_updates + !graphics_push, expected_writes + (graphics_push ? 0 : 3), 2);
    unsigned good = 0;
    for (unsigned i = 0; i < TARGETS; i++) {
        good += check_pixels(rb[i], i);
        nvn_gpu_buf_release(rb[i], NULL, NULL);
        nvn_gpu_view_release(views[i]);
        nvn_gpu_tex_release(tex[i]);
    }
    printf("desc-clear pixels=%u/%d\n", good, TARGETS * W * H);
    if (depth) {
        if (rb_depth) nvn_gpu_buf_release(rb_depth, NULL, NULL);
        nvn_gpu_view_release(depth);
        nvn_gpu_tex_release(tex_depth);
    }
    nvn_gpu_samp_release(sampler);
    nvn_gpu_view_release(input_view);
    nvn_gpu_tex_release(input);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(ubo, NULL, NULL);
    nvn_gpu_buf_release(out_cs, NULL, NULL);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(cs);
    free(vm);
    free(fm);
}

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    keep_on = atoi(argv[2]);
    memo_on = atoi(argv[3]);
    none_on = getenv("S2A_TEST_DESC_CLEAR_NONE") != NULL;
    const char *area = getenv("S2A_TEST_DESC_CLEAR_AREA");
    area_mode = !area ? FULL : !strcmp(area, "inside") ? INSIDE : REOPEN;
    push_on = getenv("S2A_TEST_DESC_CLEAR_PUSH") != NULL;
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) {
        fprintf(stderr, "device: %s\n", why);
        return 2;
    }
    CHECK(!none_on || g_dev.load_store_op_none_enabled, "desc-clear NONE requires load/store NONE");
    CHECK(!push_on || g_dev.push_descriptor_enabled, "desc-clear push requires push descriptors");
    g_desc = (nvn_gpu_open_desc){.host_device = &g_dev, .app_label = "desc-rect-clear"};
    if (!open_backend()) return 2;
    run_clear_workload(argv[1], atoi(argv[4]));
    extern uint64_t nvn_vk_dbg_dq_recs(void);
    const uint64_t records = nvn_vk_dbg_dq_recs();
    CHECK(!!records == !!getenv("S2A_TEST_NVK_DQ"), "desc-clear DQ mode records=%llu", (unsigned long long)records);
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "close: %s", err.why);
    CHECK(!s2a_vk_device_destroy(&g_dev, 1), "destroy device");
    printf("desc-clear Vulkan failures=%d\n", g_fail);
    return g_fail ? 1 : 0;
}
