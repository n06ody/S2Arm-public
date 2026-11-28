// VK_EXT_primitive_topology_list_restart 合同测试（Mesa 26.0.8 lavapipe + VVL 1.4.341 + SyncVal）。
// 验证（全程真实驱动与真实像素读回，零伪造）：
//  1) mode=on：Vulkan 1.1 运行时协商 VK_EXT_primitive_topology_list_restart + primitiveTopologyListRestart 特性位；
//     - TRIANGLE_LIST + prim_restart=1：uint8 索引 {0, 0xFF, 0, 1, 2, 0xFF, 3, 4, 5}（256 顶点缓冲零越界，
//       哨兵 255 为红色诱饵）正确在 0xFF 处重启图元 → 左下角纯绿 (00 FF 00 FF)、中上黑 (00 00 00 FF)、
//       右下角纯蓝 (00 00 FF FF)、整图 0 红像素；
//     - 同图元下 uint16 (0xFFFF) 与 uint32 (0xFFFFFFFF) 重启哨兵对 6 顶点缓冲零越界且像素逐字节一致；
//     - LINE_LIST + prim_restart=1 + uint16 (0xFFFF) 正常提交绘制，VVL + SyncVal 0 错；
//     - 列表图元 prim_restart=1 与 prim_restart=0 建出两条不同管线（ia_list_restart_pipes >= 2，l_on != l_off）。
//  2) mode=off / hide-ext / hide-feat：零扩展/零特性链静默回退旧行为（eff_restart 在列表图元归 0，
//     l_on == l_off，ia_list_restart_pipes == 0，不设 primitiveRestartEnable），VVL + SyncVal 0 错。
//  3) mode=withdraw（负对照）：开关关闭却按 on 期望验收，必须报红退出非零。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn_backend.h"
#include "nvn_plat.h"
#include "vk_device.h"

extern VkResult __real_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *);
extern void __real_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
extern VkResult __real_vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
extern VkResult __real_vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *);
extern uint64_t nvn_vk_dbg_pipe_stat(int which);

static int g_hide_ext, g_hide_feat, g_flag_off;
static unsigned g_dev_ext_enabled, g_dev_feat_enabled, g_ia_list_restart_pipes;

static int is_list_topo(VkPrimitiveTopology t) {
    return t == VK_PRIMITIVE_TOPOLOGY_POINT_LIST || t == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
           t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || t == VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY ||
           t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
}

VkResult __wrap_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice phys, const char *layer, uint32_t *count, VkExtensionProperties *props) {
    if (!g_hide_ext || layer) return __real_vkEnumerateDeviceExtensionProperties(phys, layer, count, props);
    uint32_t total = 0;
    VkResult r = __real_vkEnumerateDeviceExtensionProperties(phys, NULL, &total, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties *tmp = total ? calloc(total, sizeof *tmp) : NULL;
    if (total && !tmp) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = __real_vkEnumerateDeviceExtensionProperties(phys, NULL, &total, tmp);
    if (r != VK_SUCCESS) { free(tmp); return r; }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < total; i++) {
        if (!strcmp(tmp[i].extensionName, VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME)) continue;
        tmp[kept++] = tmp[i];
    }
    if (!props) { *count = kept; free(tmp); return VK_SUCCESS; }
    uint32_t n = *count < kept ? *count : kept;
    memcpy(props, tmp, n * sizeof *tmp);
    *count = n;
    free(tmp);
    return n < kept ? VK_INCOMPLETE : VK_SUCCESS;
}

void __wrap_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice phys, VkPhysicalDeviceFeatures2 *features) {
    __real_vkGetPhysicalDeviceFeatures2(phys, features);
    if (!g_hide_feat || !features) return;
    for (VkBaseOutStructure *b = features->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT)
            ((VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT *)b)->primitiveTopologyListRestart = VK_FALSE;
    }
}

VkResult __wrap_vkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *out) {
    if (ci) {
        for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
            if (!strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME))
                g_dev_ext_enabled++;
        for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
            if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT &&
                ((const VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT *)b)->primitiveTopologyListRestart == VK_TRUE)
                g_dev_feat_enabled++;
        }
    }
    return __real_vkCreateDevice(phys, ci, alloc, out);
}

VkResult __wrap_vkCreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t count,
                                          const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *alloc, VkPipeline *pipes) {
    if (infos) {
        for (uint32_t i = 0; i < count; i++) {
            const VkPipelineInputAssemblyStateCreateInfo *ia = infos[i].pInputAssemblyState;
            if (ia && ia->primitiveRestartEnable == VK_TRUE && is_list_topo(ia->topology))
                __atomic_fetch_add(&g_ia_list_restart_pipes, 1, __ATOMIC_RELAXED);
        }
    }
    return __real_vkCreateGraphicsPipelines(dev, cache, count, infos, alloc, pipes);
}

int nvn_plat_flag(const char *name, int dflt) {
    if (!name) return dflt;
    if (!strcmp(name, "nvk_list_restart")) return g_flag_off ? 0 : dflt;
    if (!strcmp(name, "nvk_dq")) {
        const char *dq = getenv("S2A_LIST_RESTART_DQ");
        if (dq) return !strcmp(dq, "1");
    }
    return dflt;
}
void nvn_plat_log(int level, const char *cls, const char *text) {
    (void)level; (void)cls; (void)text;
}

#define REQUIRE(cond) do { \
    if (!(cond)) { fprintf(stderr, "【红】fixture failed line %d: %s\n", __LINE__, #cond); return 1; } \
} while (0)
#define GPU(call) do { \
    nvn_gpu_rc rc_ = (call); \
    if (!nvn_gpu_ok(rc_)) { fprintf(stderr, "【红】gpu failed line %d: rc=%d why=%s\n", __LINE__, rc_.code, err.why); return 1; } \
} while (0)

static int load_spv(const char *path, void **buf, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return 0; }
    *buf = malloc((size_t)sz);
    if (!*buf || fread(*buf, 1, (size_t)sz, f) != (size_t)sz) { free(*buf); fclose(f); return 0; }
    fclose(f);
    *len = (size_t)sz;
    return 1;
}

enum { W = 16, H = 16, BYTES = W * H * 4 };

struct vert { float pos[4], col[4]; };

static nvn_gpu_pipe_desc make_pipe_desc(nvn_gpu_shader vs, nvn_gpu_shader fs, uint32_t prim, uint8_t restart) {
    nvn_gpu_pipe_desc d = {0};
    d.st[NVN_GPU_SH_VS].sh = vs;
    d.st[NVN_GPU_SH_FS].sh = fs;
    d.prim = prim;
    d.prim_restart = restart;
    d.cfmt[0] = NVN_FORMAT_RGBA8;
    d.samples = 1;
    d.origin_ll = 1;
    d.poly.mode = NVN_POLYGON_MODE_FILL;
    d.color.logic_op = 3; // COPY
    d.chmask.mask = 0xFu;
    d.attr[0].fmt = NVN_FORMAT_RGBA32F; d.attr[0].offset = 0;  d.attr[0].stream = 0;
    d.attr[1].fmt = NVN_FORMAT_RGBA32F; d.attr[1].offset = 16; d.attr[1].stream = 0;
    d.stream[0].stride = sizeof(struct vert);
    return d;
}

static int draw_indexed(nvn_gpu_tex tex, nvn_gpu_view view, nvn_gpu_buf rb, nvn_gpu_pipe pipe,
                        nvn_gpu_buf vb, nvn_gpu_buf ib, uint32_t prim, uint32_t itype, uint32_t count,
                        uint64_t signal, uint8_t rgba_out[BYTES]) {
    nvn_gpu_err err = {0};
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_pass_desc pass = {
        .ncolor = 1,
        .color = {{.view = view, .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE}},
        .w = W, .h = H, .layers = 1,
    };
    pass.color[0].clear.f[3] = 1.0f;
    nvn_gpu_pass_begin(cmd, &pass);
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_dyn dyn = {.nvp = 1, .vp = {{0, 0, (float)W, (float)H, 0.0f, 1.0f}}, .has_sc = 1, .sc = {0, 0, W, H}, .origin_ll = 1};
    nvn_gpu_set_dyn(cmd, &dyn);
    nvn_gpu_set_vbuf(cmd, 0, vb, 0);
    nvn_gpu_draw_args draw = {.prim = prim, .count = count, .instances = 1, .ibuf = ib, .itype = itype};
    nvn_gpu_draw(cmd, &draw);
    nvn_gpu_pass_end(cmd);
    NVNcopyRegion reg = {0, 0, 0, W, H, 1};
    nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, &reg, rb, 0, W * 4, BYTES);
    GPU(nvn_gpu_cmd_commit(cmd, signal, NULL, NULL, &err));
    GPU(nvn_gpu_fence_wait(signal, 5000000000ull, &err));
    memcpy(rgba_out, nvn_gpu_buf_cpu(rb), BYTES);
    return 0;
}

static int px_eq(const uint8_t img[BYTES], int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint8_t *p = img + ((y * W + x) * 4);
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

static unsigned count_red(const uint8_t img[BYTES]) {
    unsigned n = 0;
    for (unsigned i = 0; i < W * H; i++) {
        const uint8_t *p = img + i * 4;
        if (p[0] > 200 && p[1] < 50 && p[2] < 50) n++;
    }
    return n;
}

int main(int argc, char **argv) {
    // argv: <shader_dir> <mode: on|off|hide-ext|hide-feat|withdraw>
    if (argc != 3) {
        fprintf(stderr, "usage: %s <shader_dir> <mode>\n", argv[0]);
        return 2;
    }
    const char *sdir = argv[1];
    const char *mode = argv[2];
    const int is_withdraw = !strcmp(mode, "withdraw");
    if (!strcmp(mode, "off") || is_withdraw) {
        g_flag_off = 1;
        setenv("debug.s2a.nvk_list_restart", "0", 1);
    } else {
        unsetenv("debug.s2a.nvk_list_restart");
        if (!strcmp(mode, "hide-ext")) g_hide_ext = 1;
        else if (!strcmp(mode, "hide-feat")) g_hide_feat = 1;
        else if (strcmp(mode, "on") != 0) return 2;
    }
    const int expect_on = !strcmp(mode, "on") || is_withdraw;

    char vp[512], fp[512];
    snprintf(vp, sizeof vp, "%s/quad.vert.spv", sdir);
    snprintf(fp, sizeof fp, "%s/quad.frag.spv", sdir);
    void *vspv = NULL, *fspv = NULL;
    size_t vlen = 0, flen = 0;
    REQUIRE(load_spv(vp, &vspv, &vlen) && load_spv(fp, &fspv, &flen));

    struct s2a_vk_device dev = {0};
    char why[256] = "";
    REQUIRE(s2a_vk_device_create(&dev, why, sizeof why) == 0);
    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&od, &err));

    static const nvn_gpu_layout empty_layout = {0};
    nvn_gpu_shader vs = NULL, fs = NULL;
    nvn_gpu_shader_src vsd = {.stage = NVN_GPU_SH_VS, .kind = NVN_GPU_BLOB_SPIRV, .blob = vspv, .bytes = vlen, .entry = "main", .layout = &empty_layout};
    nvn_gpu_shader_src fsd = {.stage = NVN_GPU_SH_FS, .kind = NVN_GPU_BLOB_SPIRV, .blob = fspv, .bytes = flen, .entry = "main", .layout = &empty_layout};
    GPU(nvn_gpu_shader_load(&vsd, &vs, &err));
    GPU(nvn_gpu_shader_load(&fsd, &fs, &err));

    // 256 顶点缓冲：v[0..2] 左下绿三角，v[3..5] 右下蓝三角，v[255] 红色哨兵诱饵（零越界）
    struct vert verts[256];
    memset(verts, 0, sizeof verts);
    verts[0] = (struct vert){{-1.0f, -1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}};
    verts[1] = (struct vert){{ 0.0f, -1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}};
    verts[2] = (struct vert){{-1.0f,  1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}};
    verts[3] = (struct vert){{ 0.0f, -1.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}};
    verts[4] = (struct vert){{ 1.0f, -1.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}};
    verts[5] = (struct vert){{ 1.0f,  1.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}};
    verts[255] = (struct vert){{ 0.0f,  1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}};

    nvn_gpu_buf vb256 = NULL, vb6 = NULL, ib8 = NULL, ib16 = NULL, ib32 = NULL, ib_line = NULL;
    GPU(nvn_gpu_buf_create(sizeof verts, NVN_GPU_MEM_SHARED, verts, &vb256, &err));
    GPU(nvn_gpu_buf_create(6 * sizeof(struct vert), NVN_GPU_MEM_SHARED, verts, &vb6, &err));
    static const uint8_t idx8[9] = {0, 0xFFu, 0, 1, 2, 0xFFu, 3, 4, 5};
    static const uint16_t idx16[9] = {0, 0xFFFFu, 0, 1, 2, 0xFFFFu, 3, 4, 5};
    static const uint32_t idx32[9] = {0, 0xFFFFFFFFu, 0, 1, 2, 0xFFFFFFFFu, 3, 4, 5};
    static const uint16_t idx_line[4] = {0, 0xFFFFu, 0, 1};
    GPU(nvn_gpu_buf_create(sizeof idx8, NVN_GPU_MEM_SHARED, idx8, &ib8, &err));
    GPU(nvn_gpu_buf_create(sizeof idx16, NVN_GPU_MEM_SHARED, idx16, &ib16, &err));
    GPU(nvn_gpu_buf_create(sizeof idx32, NVN_GPU_MEM_SHARED, idx32, &ib32, &err));
    GPU(nvn_gpu_buf_create(sizeof idx_line, NVN_GPU_MEM_SHARED, idx_line, &ib_line, &err));

    nvn_gpu_tex_desc td = {0};
    td.d.width = W; td.d.height = H; td.d.depth = 1; td.d.levels = 1;
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8;
    td.usage = NVN_GPU_TEX_RENDER;
    nvn_gpu_tex tex = NULL;
    GPU(nvn_gpu_tex_create(&td, &tex, &err));
    nvn_gpu_view view = NULL;
    GPU(nvn_gpu_view_create(tex, NULL, &view, &err));
    nvn_gpu_buf rb = NULL;
    GPU(nvn_gpu_buf_create(BYTES, NVN_GPU_MEM_SHARED, NULL, &rb, &err));

    nvn_gpu_pipe_desc pd_on = make_pipe_desc(vs, fs, NVN_DRAW_PRIMITIVE_TRIANGLES, 1);
    nvn_gpu_pipe_desc pd_off = make_pipe_desc(vs, fs, NVN_DRAW_PRIMITIVE_TRIANGLES, 0);
    nvn_gpu_pipe l_on = NULL, l_off = NULL;
    const uint64_t pso_before = nvn_vk_dbg_pipe_stat(5);
    GPU(nvn_gpu_pipe_get(&pd_on, NVN_GPU_PIPE_SYNC, &l_on, &err));
    GPU(nvn_gpu_pipe_get(&pd_off, NVN_GPU_PIPE_SYNC, &l_off, &err));
    const int distinct = (l_on != l_off);
    const uint64_t list_psos = nvn_vk_dbg_pipe_stat(5) - pso_before;
    const int dynamic_restart = dev.eds2_enabled;
    REQUIRE(list_psos == (uint64_t)(1 + (distinct && !dynamic_restart)));

    uint8_t img8[BYTES], img16[BYTES], img32[BYTES], img_line[BYTES];
    uint64_t sig = 1;
    REQUIRE(draw_indexed(tex, view, rb, l_on, vb256, ib8, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 9, sig++, img8) == 0);

    const int left_green = px_eq(img8, 2, 2, 0, 255, 0, 255);
    const int right_blue = px_eq(img8, 13, 2, 0, 0, 255, 255);
    const int top_black = px_eq(img8, 6, 13, 0, 0, 0, 255);
    const unsigned red_px = count_red(img8);
    int match16 = 0, match32 = 0, line_ok = 0;
    unsigned restart_off_red = red_px;

    if (!strcmp(mode, "on")) {
        // 仅在真正启用 list_restart 时跑 u16 (0xFFFF) / u32 (0xFFFFFFFF) 对 6 顶点缓冲与 LINE_LIST，
        // 若列表重启生效，哨兵绝不会越界解引用且像素与 u8 逐字节一致。
        if (left_green && right_blue && top_black && red_px == 0 && g_ia_list_restart_pipes >= 1) {
            REQUIRE(draw_indexed(tex, view, rb, l_on, vb6, ib16, NVN_DRAW_PRIMITIVE_TRIANGLES, 1, 9, sig++, img16) == 0);
            REQUIRE(draw_indexed(tex, view, rb, l_on, vb6, ib32, NVN_DRAW_PRIMITIVE_TRIANGLES, 2, 9, sig++, img32) == 0);
            match16 = (memcmp(img8, img16, BYTES) == 0);
            match32 = (memcmp(img8, img32, BYTES) == 0);
            nvn_gpu_pipe_desc pd_line = make_pipe_desc(vs, fs, NVN_DRAW_PRIMITIVE_LINES, 1);
            nvn_gpu_pipe p_line = NULL;
            GPU(nvn_gpu_pipe_get(&pd_line, NVN_GPU_PIPE_SYNC, &p_line, &err));
            REQUIRE(draw_indexed(tex, view, rb, p_line, vb6, ib_line, NVN_DRAW_PRIMITIVE_LINES, 1, 4, sig++, img_line) == 0);
            line_ok = 1;
            uint8_t img_off[BYTES];
            REQUIRE(draw_indexed(tex, view, rb, l_off, vb256, ib8, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 9, sig++, img_off) == 0);
            restart_off_red = count_red(img_off);
        }
    }

    const unsigned ia_pipes = __atomic_load_n(&g_ia_list_restart_pipes, __ATOMIC_RELAXED);
    printf("list-restart mode=%s dev_ext=%u dev_feat=%u ia_pipes=%u distinct=%d left_green=%d right_blue=%d top_black=%d red_px=%u u16=%d u32=%d line=%d\n",
           mode, g_dev_ext_enabled, g_dev_feat_enabled, ia_pipes, distinct,
           left_green, right_blue, top_black, red_px, match16, match32, line_ok);
    printf("list-restart EDS1=%d EDS2=%d DQ=%d list-psos=%llu restart-off-red=%u\n", dev.eds1_enabled,
           dynamic_restart, nvn_plat_flag("nvk_dq", 0), (unsigned long long)list_psos, restart_off_red);

    nvn_gpu_buf_release(vb256, NULL, NULL);
    nvn_gpu_buf_release(vb6, NULL, NULL);
    nvn_gpu_buf_release(ib8, NULL, NULL);
    nvn_gpu_buf_release(ib16, NULL, NULL);
    nvn_gpu_buf_release(ib32, NULL, NULL);
    nvn_gpu_buf_release(ib_line, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(view);
    nvn_gpu_tex_release(tex);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
    GPU(nvn_gpu_close(5000000000ull, &err));
    s2a_vk_device_destroy(&dev, 1);
    free(vspv);
    free(fspv);

    if (expect_on) {
        if (g_dev_ext_enabled != 1 || g_dev_feat_enabled != 1 || ia_pipes < 2 || !distinct ||
            !left_green || !right_blue || !top_black || red_px != 0 || !restart_off_red || !match16 || !match32 || !line_ok) {
            fprintf(stderr, "【红】list_restart missed expectation: ext=%u feat=%u ia=%u distinct=%d green=%d blue=%d black=%d red=%u u16=%d u32=%d line=%d\n",
                    g_dev_ext_enabled, g_dev_feat_enabled, ia_pipes, distinct,
                    left_green, right_blue, top_black, red_px, match16, match32, line_ok);
            return 1;
        }
    } else {
        REQUIRE(g_dev_ext_enabled == 0 && g_dev_feat_enabled == 0);
        REQUIRE(ia_pipes == 0 && !distinct);
        REQUIRE(!left_green && right_blue && red_px > 0);
    }
    return 0;
}
