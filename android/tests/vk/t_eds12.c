// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// One shader pair and attachment layout, with independent raster/depth state changes.
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "nvn_vk_pipe_warm.h"

VKAPI_ATTR VkResult VKAPI_CALL __real_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical, const char *layer, uint32_t *count, VkExtensionProperties *properties);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical, const char *layer, uint32_t *count, VkExtensionProperties *properties) {
    if (layer || !getenv("S2A_EDS_NO_EXT"))
        return __real_vkEnumerateDeviceExtensionProperties(physical, layer, count, properties);
    uint32_t total = 0;
    VkResult result = __real_vkEnumerateDeviceExtensionProperties(physical, NULL, &total, NULL);
    if (result != VK_SUCCESS) return result;
    VkExtensionProperties *all = calloc(total ? total : 1, sizeof *all);
    if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    result = __real_vkEnumerateDeviceExtensionProperties(physical, NULL, &total, all);
    if (result == VK_SUCCESS) {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < total; i++)
            if (strcmp(all[i].extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME))
                all[kept++] = all[i];
        if (!properties) *count = kept;
        else {
            if (*count < kept) result = VK_INCOMPLETE;
            else *count = kept;
            memcpy(properties, all, *count * sizeof *all);
        }
    }
    free(all);
    return result;
}

VKAPI_ATTR void VKAPI_CALL __real_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *features) {
    __real_vkGetPhysicalDeviceFeatures2(physical, features);
    if (getenv("S2A_EDS_NO_FEAT"))
        for (VkBaseOutStructure *next = features->pNext; next; next = next->pNext)
            if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT)
                ((VkPhysicalDeviceExtendedDynamicState2FeaturesEXT *)next)->extendedDynamicState2 = VK_FALSE;
}

static PFN_vkCmdSetCullModeEXT real_cull;
static atomic_uint cull_commands;

static VKAPI_ATTR void VKAPI_CALL count_cull(VkCommandBuffer cmd, VkCullModeFlags mode) {
    atomic_fetch_add_explicit(&cull_commands, 1, memory_order_relaxed);
    real_cull(cmd, mode);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __real_vkGetDeviceProcAddr(VkDevice, const char *);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice device, const char *name) {
    if (getenv("S2A_EDS_NO_PROC") && !strcmp(name, "vkCmdSetPrimitiveRestartEnableEXT"))
        return NULL;
    PFN_vkVoidFunction real = __real_vkGetDeviceProcAddr(device, name);
    if (real && !strcmp(name, "vkCmdSetCullModeEXT")) {
        real_cull = (PFN_vkCmdSetCullModeEXT)real;
        return (PFN_vkVoidFunction)count_cull;
    }
    return real;
}

uint64_t rp_sync_pipelines_created(void);

static void eds12(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) {
        CHECK(0, "EDS shader pair");
        return;
    }
    nvn_gpu_pipe_desc base;
    fill_gpipe_desc(&base, vs, fs, NVN_FORMAT_DEPTH24_STENCIL8);
    base.sfmt = NVN_FORMAT_DEPTH24_STENCIL8;
    base.poly.front = NVN_FRONT_FACE_CCW;
    nvn_gpu_caps caps;
    nvn_gpu_caps_get(&caps);
    const int dynamic = !getenv("S2A_EDS_STATIC");
    base.extended_dynamic = (caps.caps & NVN_GPU_CAP_EXTENDED_DYN) != 0;
    CHECK(base.extended_dynamic == dynamic, "EDS capability expected %d, got %d", dynamic, base.extended_dynamic);
    enum { BASE, CULL, FRONT, DEPTH_NEVER, DEPTH_LESS, DEPTH_WRITE, STENCIL, STENCIL_OP,
           BIAS, STRIP, RESTART, DISCARD, CASES };
    const char *name[CASES] = {"base", "cull", "front", "depth-never", "depth-less", "depth-write", "stencil-test", "stencil-op",
        "bias-enable", "triangle-strip", "primitive-restart", "rasterizer-discard"};
    nvn_gpu_pipe_desc pd[CASES];
    nvn_gpu_pipe pipes[CASES] = {0};
    for (unsigned i = 0; i < CASES; i++) pd[i] = base;
    pd[CULL].poly.cull = NVN_FACE_FRONT_AND_BACK;
    pd[FRONT].poly.front = NVN_FRONT_FACE_CW;
    pd[FRONT].poly.cull = NVN_FACE_BACK;
    pd[DEPTH_NEVER].ds.dt = 1;
    pd[DEPTH_NEVER].ds.df = NVN_COMPARE_FUNC_NEVER;
    pd[DEPTH_LESS].ds.dt = 1;
    pd[DEPTH_LESS].ds.df = NVN_COMPARE_FUNC_LESS;
    pd[DEPTH_WRITE].ds = pd[DEPTH_LESS].ds;
    pd[DEPTH_WRITE].ds.dw = 1;
    pd[STENCIL].ds.st = 1;
    pd[STENCIL].ds.sf_f = pd[STENCIL].ds.sf_b = NVN_COMPARE_FUNC_EQUAL;
    pd[STENCIL_OP].ds.st = 1;
    pd[STENCIL_OP].ds.sf_f = pd[STENCIL_OP].ds.sf_b = NVN_COMPARE_FUNC_ALWAYS;
    pd[STENCIL_OP].ds.zpass_f = pd[STENCIL_OP].ds.zpass_b = NVN_STENCIL_OP_REPLACE;
    pd[BIAS].poly.offset_en = 4;
    pd[BIAS].ds = pd[DEPTH_LESS].ds;
    pd[STRIP].prim = pd[RESTART].prim = NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP;
    pd[RESTART].prim_restart = 1;
    pd[DISCARD].rast_discard = 1;
    const uint8_t program[32] = {1};
    struct nvk_pipe_rec raw, variant;
    nvk_pipe_warm_rec_of(&base, program, &raw);
    CHECK(raw.bytes == 352, "EDS warm record retains its wire length");
    for (unsigned i = 1; i < CASES; i++) {
        nvk_pipe_warm_rec_of(&pd[i], program, &variant);
        CHECK((memcmp(raw.desc, variant.desc, raw.bytes) == 0) == dynamic,
              "%s warm recipe equivalence expected %d", name[i], dynamic);
    }
    const uint64_t before = rp_sync_pipelines_created();
    nvn_gpu_err err = {0};
    for (unsigned i = 0; i < CASES; i++) {
        CHECK(OK(nvn_gpu_pipe_get(&pd[i], NVN_GPU_PIPE_SYNC, &pipes[i], &err)), "%s pipe: %s", name[i], err.why);
        if (!pipes[i]) return;
    }
    const uint64_t created = rp_sync_pipelines_created() - before;
    const unsigned expected = dynamic ? 1 : CASES;
    printf("EDS triangles created=%llu expected=%u variants=%u\n", (unsigned long long)created, expected, CASES);
    CHECK(created == expected, "EDS creations expected %u, got %llu", expected, (unsigned long long)created);
    nvn_gpu_pipe line = NULL;
    nvn_gpu_pipe_desc other = base;
    other.prim = NVN_DRAW_PRIMITIVE_LINES;
    CHECK(OK(nvn_gpu_pipe_get(&other, NVN_GPU_PIPE_SYNC, &line, &err)) && line && line != pipes[0],
          "line and triangle must keep separate pipes: %s", err.why);

    enum { LIST_DEGENERATE = CASES, STRIP_FULL, RESTART16_OFF, RESTART16_ON, RESTART8_OFF, RESTART8_ON,
           DEPTH_KEEP, DEPTH_REPLACE, STENCIL_KEEP, STENCIL_REPLACE, TOTAL };
    other.prim = NVN_DRAW_PRIMITIVE_POINTS;
    CHECK(!nvn_vk_dbg_pipe_same(&other, &base), "point and triangle must have distinct keys");
    other.prim = NVN_DRAW_PRIMITIVE_PATCHES;
    CHECK(!nvn_vk_dbg_pipe_same(&other, &base), "patch and triangle must have distinct keys");

    const uint32_t bytes = 64 * 64 * 4;
    nvn_gpu_tex color = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_tex ds = make_tex(64, 64, NVN_FORMAT_DEPTH24_STENCIL8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view cv = make_view(color), dv = make_view(ds);
    const float strip[4][4] = {{-1, -1, 0, 1}, {-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
    nvn_gpu_buf vb = full_vb(), strip_vb = make_buf(sizeof strip, NVN_GPU_MEM_SHARED, strip);
    float (*vertices)[4] = calloc(UINT16_MAX + 1u, sizeof *vertices);
    if (!vertices) { CHECK(0, "EDS restart test vertex allocation"); return; }
    memcpy(vertices, &strip[1], 3 * sizeof *vertices);
    memcpy(vertices[UINT8_MAX], strip[0], sizeof *vertices);
    memcpy(vertices[UINT16_MAX], strip[0], sizeof *vertices);
    nvn_gpu_buf indexed_vb = make_buf((UINT16_MAX + 1u) * sizeof *vertices, NVN_GPU_MEM_SHARED, vertices);
    free(vertices);
    const uint16_t indices16[] = {0, UINT16_MAX, 1, 2};
    const uint8_t indices8[] = {0, UINT8_MAX, 1, 2};
    nvn_gpu_buf ib16 = make_buf(sizeof indices16, NVN_GPU_MEM_SHARED, indices16);
    nvn_gpu_buf ib8 = make_buf(sizeof indices8, NVN_GPU_MEM_SHARED, indices8);
    nvn_gpu_buf rb = make_buf(TOTAL * bytes, NVN_GPU_MEM_SHARED, NULL);
    enum { WRITE_DEPTH_KEEP, WRITE_DEPTH_REPLACE, WRITE_STENCIL_KEEP, WRITE_STENCIL_REPLACE, PREPARES };
    nvn_gpu_pipe_desc prepare_desc[PREPARES] = {pd[DEPTH_LESS], pd[DEPTH_WRITE], pd[STENCIL_OP], pd[STENCIL_OP]};
    prepare_desc[WRITE_STENCIL_KEEP].ds.zpass_f = prepare_desc[WRITE_STENCIL_KEEP].ds.zpass_b = NVN_STENCIL_OP_KEEP;
    nvn_gpu_pipe prepare[PREPARES] = {0};
    for (unsigned i = 0; i < PREPARES; i++) {
        prepare_desc[i].chmask.mask = 0;
        CHECK(OK(nvn_gpu_pipe_get(&prepare_desc[i], NVN_GPU_PIPE_SYNC, &prepare[i], &err)) && prepare[i],
              "EDS attachment preparation pipe %u: %s", i, err.why);
    }
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc pass = {.w=64, .h=64, .layers=1, .ncolor=1, .clear_depth=0.5f};
    pass.color[0] = (nvn_gpu_att){.view=cv, .load=NVN_GPU_LOAD_CLEAR, .store=NVN_GPU_STORE_STORE, .clear.f={0,0,0,1}};
    pass.depth = pass.stencil = (nvn_gpu_att){.view=dv, .load=NVN_GPU_LOAD_CLEAR, .store=NVN_GPU_STORE_STORE};
    for (unsigned i = 0; i < TOTAL; i++) {
        nvn_gpu_pass_begin(c, &pass);
        unsigned state = i < CASES ? i : BASE;
        if (i == STRIP_FULL || i == RESTART16_OFF || i == RESTART8_OFF) state = STRIP;
        if (i == RESTART16_ON || i == RESTART8_ON) state = RESTART;
        if (i == DEPTH_KEEP || i == DEPTH_REPLACE) state = DEPTH_LESS;
        if (i == STENCIL_KEEP || i == STENCIL_REPLACE) state = STENCIL;
        nvn_gpu_set_pipe(c, pipes[state]);
        nvn_gpu_dyn dyn;
        full_dyn(&dyn);
        dyn.ds = pd[state].ds;
        dyn.poly = pd[state].poly;
        dyn.prim_restart = pd[state].prim_restart;
        dyn.rast_discard = pd[state].rast_discard;
        dyn.st_vmask[0] = dyn.st_vmask[1] = 0xff;
        dyn.st_wmask[0] = dyn.st_wmask[1] = 0xff;
        dyn.st_ref[0] = dyn.st_ref[1] = 1;
        nvn_gpu_draw_args draw = {.prim = pd[state].prim, .count = 3, .instances = 1};
        if (i == BIAS) {
            dyn.vp[0][4] = dyn.vp[0][5] = 0.25f;
            dyn.po.units = 1000000000.0f;
        }
        if (i >= DEPTH_KEEP) {
            const unsigned prep = i - DEPTH_KEEP;
            nvn_gpu_dyn write = dyn;
            write.ds = prepare_desc[prep].ds;
            write.vp[0][4] = write.vp[0][5] = 0.25f;
            nvn_gpu_set_pipe(c, prepare[prep]);
            nvn_gpu_set_dyn(c, &write);
            nvn_gpu_set_vbuf(c, 0, vb, 0);
            nvn_gpu_draw(c, &draw);
            nvn_gpu_set_pipe(c, pipes[state]);
            dyn.vp[0][4] = dyn.vp[0][5] = 0.375f;
        }
        nvn_gpu_buf geom = vb;
        if (i >= LIST_DEGENERATE && i <= RESTART8_ON) {
            geom = strip_vb;
            draw.count = 4;
            if (i >= RESTART16_OFF) {
                geom = indexed_vb;
                draw.ibuf = i < RESTART8_OFF ? ib16 : ib8;
                draw.itype = i < RESTART8_OFF ? 1 : 0;
            }
        }
        const int force_u8_expansion = i == RESTART8_OFF || i == RESTART8_ON;
        if (force_u8_expansion) setenv("S2A_VK_FORCE_INDEX_U8_EXPAND", "1", 1);
        nvn_gpu_set_dyn(c, &dyn);
        nvn_gpu_set_vbuf(c, 0, geom, 0);
        nvn_gpu_draw(c, &draw);
        if (force_u8_expansion) unsetenv("S2A_VK_FORCE_INDEX_U8_EXPAND");
        if (i == BASE) nvn_gpu_draw(c, &draw);   // 同状态第二笔不应重发 EDS
        if (i == DISCARD) {
            nvn_gpu_set_pipe(c, pipes[BASE]);
            dyn.rast_discard = 0;
            dyn.has_sc = 1;
            dyn.sc[1] = 32;
            dyn.sc[2] = 64;
            dyn.sc[3] = 32;
            nvn_gpu_set_dyn(c, &dyn);
            nvn_gpu_draw(c, &draw);
        }
        nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c, color, 0, 0, NULL, rb, bytes * i, 64 * 4, bytes);
    }
    if (OK(nvn_gpu_cmd_commit(c, 1, on_done, NULL, &err)) && OK(nvn_gpu_fence_wait(1, 5000000000ull, &err))) {
        const uint8_t *pixel = buf_cpu(rb);
        unsigned exact = 0;
        for (unsigned i = 0; i < CASES; i++) {
            const int before_check = g_fail;
            const int black = i == CULL || i == FRONT || i == DEPTH_NEVER || i == STENCIL || i == BIAS;
            if (i == DISCARD) {
                expect_rect(pixel + bytes * i, 0, 0, 64, 32, 0, 0, 0, name[i]);
                expect_rect(pixel + bytes * i, 0, 32, 64, 32, 0, 255, 0, "rasterizer restored");
            } else expect_rect(pixel + bytes * i, 0, 0, 64, 64, 0, black ? 0 : 255, 0, name[i]);
            exact += before_check == g_fail;
        }
        const char *const probe[] = {"list ignores incomplete triangle", "strip rasterizes second triangle",
            "u16 restart off", "u16 restart on", "u8 widened restart off", "u8 widened restart on",
            "depth write off", "depth write on", "stencil keep", "stencil replace"};
        const unsigned char green[] = {0, 255, 255, 0, 255, 0, 255, 0, 0, 255};
        for (unsigned i = CASES; i < TOTAL; i++) {
            const int before_check = g_fail;
            expect_rect(pixel + bytes * i, 0, 0, 64, 64, 0, green[i - CASES], 0, probe[i - CASES]);
            exact += before_check == g_fail;
        }
        const unsigned expected_cull = dynamic ? TOTAL + 1 : 0;   // 每通道一次，取消丢弃额外恢复一次
        const unsigned actual_cull = atomic_load_explicit(&cull_commands, memory_order_relaxed);
        CHECK(actual_cull == expected_cull, "EDS cull commands expected %u, got %u", expected_cull, actual_cull);
        printf("EDS pixel cases=%u/%u pixels_per_case=4096 cull_commands=%u expected_cull=%u\n",
               exact, TOTAL, actual_cull, expected_cull);
    } else CHECK(0, "EDS submit/readback: %s", err.why);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_buf_release(strip_vb, NULL, NULL);
    nvn_gpu_buf_release(indexed_vb, NULL, NULL);
    nvn_gpu_buf_release(ib16, NULL, NULL);
    nvn_gpu_buf_release(ib8, NULL, NULL);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_view_release(cv);
    nvn_gpu_view_release(dv);
    nvn_gpu_tex_release(color);
    nvn_gpu_tex_release(ds);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
    free(vm);
    free(fm);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char why[192] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) {
        fprintf(stderr, "EDS device creation: %s\n", why);
        return 2;
    }
    printf("device: %s, api %u.%u\n", g_dev.properties.deviceName,
           VK_API_VERSION_MAJOR(g_dev.properties.apiVersion), VK_API_VERSION_MINOR(g_dev.properties.apiVersion));
    g_desc.host_device = &g_dev;
    g_desc.app_label = "eds12";
    if (open_backend()) {
        eds12(argv[1]);
        nvn_gpu_err err = {0};
        CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "EDS close: %s", err.why);
    } else CHECK(0, "EDS backend open");
    CHECK(!s2a_vk_device_destroy(&g_dev, 1), "EDS device destroy");
    return g_fail ? 1 : 0;
}
