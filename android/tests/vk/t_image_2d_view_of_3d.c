// VK_EXT_image_2d_view_of_3d 合同测试（Mesa 26.0.8 lavapipe + VVL 1.4.341 + SyncVal）。
// 验证（全程真实驱动与真实像素/纹素读回，零伪造）：
//  1) mode=on：Vulkan 1.1 运行时协商 VK_EXT_image_2d_view_of_3d + image2DViewOf3D / sampler2DViewOf3D；
//     - 3D 纹理创建带 VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT（img_2d3d >= 2）；
//     - 2D 采样视图（slice z=2，蓝 00 00 FF FF）经 sampler2D 实绘读回 16/16 像素精确匹配；
//     - 2D 存储视图（slice z=1）经 image2D compute imageStore 写入精确字节序列，读回 slice z=1
//       逐字节匹配且相邻 slice z=0（红）/ z=2（蓝）零污染；
//     - 2D 附件视图（slice z=3）作为颜色附件清屏洋红 FF 00 FF FF 读回精确匹配；
//     - 纯 SAMPLED（无 RENDER）3D 纹理建 2D 采样视图实绘 slice z=1 精确匹配。
//  2) mode=off / hide-ext / hide-feat：不挂扩展/特性或运行时关闭开关时 img_2d3d == 0；
//     - 3D 纹理 2D 附件视图仍经 2D_ARRAY_COMPATIBLE_BIT 正常工作且 VVL/SyncVal 0 错（9000 等无扩展设备无回归）；
//     - 纯 SAMPLED 3D 纹理建 2D 视图准确返回 NVN_GPU_E_UNSUPPORTED；
//     - 带 RENDER 的 3D 纹理 2D 视图尝试绑定为采样纹理或存储图像时准确报 NVN_GPU_E_UNSUPPORTED，VVL/SyncVal 0 错。
//  3) mode=withdraw（负对照）：开关关闭却按 on 期望验收，必须报红退出非零且 VVL/SyncVal 0 错。
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
extern VkResult __real_vkCreateImage(VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *);

static int g_hide_ext, g_hide_feat, g_flag_off;
static unsigned g_dev_ext_enabled, g_dev_img_feat_enabled, g_dev_samp_feat_enabled;
static unsigned g_img_3d_2dview_flag;

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
        if (!strcmp(tmp[i].extensionName, VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME)) continue;
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
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT) {
            VkPhysicalDeviceImage2DViewOf3DFeaturesEXT *f = (VkPhysicalDeviceImage2DViewOf3DFeaturesEXT *)b;
            f->image2DViewOf3D = VK_FALSE;
            f->sampler2DViewOf3D = VK_FALSE;
        }
    }
}

VkResult __wrap_vkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *out) {
    if (ci) {
        for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
            if (!strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME))
                g_dev_ext_enabled++;
        for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
            if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT) {
                const VkPhysicalDeviceImage2DViewOf3DFeaturesEXT *f = (const VkPhysicalDeviceImage2DViewOf3DFeaturesEXT *)b;
                if (f->image2DViewOf3D == VK_TRUE) g_dev_img_feat_enabled++;
                if (f->sampler2DViewOf3D == VK_TRUE) g_dev_samp_feat_enabled++;
            }
        }
    }
    return __real_vkCreateDevice(phys, ci, alloc, out);
}

VkResult __wrap_vkCreateImage(VkDevice dev, const VkImageCreateInfo *ci, const VkAllocationCallbacks *alloc, VkImage *out) {
    if (ci && ci->imageType == VK_IMAGE_TYPE_3D && (ci->flags & VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT))
        __atomic_fetch_add(&g_img_3d_2dview_flag, 1, __ATOMIC_RELAXED);
    return __real_vkCreateImage(dev, ci, alloc, out);
}

int nvn_plat_flag(const char *name, int dflt) {
    if (!name) return dflt;
    if (!strcmp(name, "nvk_image_2d_view_of_3d")) return g_flag_off ? 0 : dflt;
    if (!strcmp(name, "nvk_hot_hash")) return getenv("S2A_TEST_2D3D_HOT_HASH") != NULL;
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

enum { W = 4, H = 4, D = 4, SLICE_BYTES = W * H * 4 };

struct vert_uv { float pos[2], uv[2]; };

static view_t make_2d_slice_view(uint32_t slice) {
    view_t v = {0};
    v.set_target = 1;
    v.target = NVN_TEXTURE_TARGET_2D;
    v.set_levels = 1;
    v.base_level = 0;
    v.nlevels = 1;
    v.set_layers = 1;
    v.min_layer = slice;
    v.nlayers = 1;
    return v;
}

static int upload_3d_slices(nvn_gpu_tex t3d, nvn_gpu_buf up, uint64_t *sig, const uint8_t colors[D][4]) {
    nvn_gpu_err err = {0};
    uint8_t *p = nvn_gpu_buf_cpu(up);
    REQUIRE(p != NULL);
    for (uint32_t z = 0; z < D; z++) {
        uint8_t *dst = p + (size_t)z * SLICE_BYTES;
        for (uint32_t i = 0; i < W * H; i++) memcpy(dst + i * 4, colors[z], 4);
    }
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    for (uint32_t z = 0; z < D; z++)
        nvn_gpu_copy_buf_tex(cmd, up, (uint64_t)z * SLICE_BYTES, W * 4, SLICE_BYTES, t3d, 0, z, NULL);
    uint64_t s = ++(*sig);
    GPU(nvn_gpu_cmd_commit(cmd, s, NULL, NULL, &err));
    GPU(nvn_gpu_fence_wait(s, 5000000000ull, &err));
    return 0;
}

static int readback_slice(nvn_gpu_tex tex, uint32_t layer, nvn_gpu_buf rb, uint64_t *sig, uint8_t out[SLICE_BYTES]) {
    nvn_gpu_err err = {0};
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_copy_tex_buf(cmd, tex, 0, layer, NULL, rb, 0, W * 4, SLICE_BYTES);
    uint64_t s = ++(*sig);
    GPU(nvn_gpu_cmd_commit(cmd, s, NULL, NULL, &err));
    GPU(nvn_gpu_fence_wait(s, 5000000000ull, &err));
    const uint8_t *p = nvn_gpu_buf_cpu(rb);
    REQUIRE(p != NULL);
    memcpy(out, p, SLICE_BYTES);
    return 0;
}

static int check_solid(const uint8_t px[SLICE_BYTES], const uint8_t want[4]) {
    for (uint32_t i = 0; i < W * H; i++)
        if (memcmp(px + i * 4, want, 4) != 0) return 0;
    return 1;
}

static nvn_gpu_rc draw_sample_view(nvn_gpu_view rt_view, nvn_gpu_pipe gpipe, nvn_gpu_buf vb,
                                   nvn_gpu_view sample_view, nvn_gpu_samp samp, uint64_t *sig, nvn_gpu_err *err) {
    nvn_gpu_cmd cmd = NULL;
    nvn_gpu_rc rc_ = nvn_gpu_cmd_begin(NULL, &cmd, err);
    if (!nvn_gpu_ok(rc_)) return rc_;
    nvn_gpu_pass_desc pass = {0};
    pass.ncolor = 1;
    pass.color[0].view = rt_view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.w = W; pass.h = H; pass.layers = 1;
    nvn_gpu_pass_begin(cmd, &pass);
    nvn_gpu_set_pipe(cmd, gpipe);
    nvn_gpu_dyn dyn = {.nvp = 1, .vp = {{0, 0, (float)W, (float)H, 0.0f, 1.0f}},
                       .has_sc = 1, .sc = {0, 0, W, H}, .origin_ll = 1};
    nvn_gpu_set_dyn(cmd, &dyn);
    nvn_gpu_set_vbuf(cmd, 0, vb, 0);
    nvn_gpu_binding b = {0};
    b.ent = 0; b.cls = NVN_GPU_RES_TEX_SAMP; b.u.ts.view = sample_view; b.u.ts.samp = samp;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, &b, 1);
    nvn_gpu_draw_args da = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1};
    nvn_gpu_draw(cmd, &da);
    nvn_gpu_pass_end(cmd);
    uint64_t s = ++(*sig);
    rc_ = nvn_gpu_cmd_commit(cmd, s, NULL, NULL, err);
    if (nvn_gpu_ok(rc_)) rc_ = nvn_gpu_fence_wait(s, 5000000000ull, err);
    return rc_;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <shader-dir> <on|off|hide-ext|hide-feat|withdraw>\n", argv[0]); return 2; }
    const char *shdir = argv[1];
    const char *mode = argv[2];
    const int is_on = !strcmp(mode, "on");
    const int is_off = !strcmp(mode, "off");
    const int is_hide_ext = !strcmp(mode, "hide-ext");
    const int is_hide_feat = !strcmp(mode, "hide-feat");
    const int is_withdraw = !strcmp(mode, "withdraw");
    if (!is_on && !is_off && !is_hide_ext && !is_hide_feat && !is_withdraw) {
        fprintf(stderr, "unknown mode: %s\n", mode);
        return 2;
    }
    g_flag_off = is_off || is_withdraw;
    g_hide_ext = is_hide_ext;
    g_hide_feat = is_hide_feat;
    if (g_flag_off) setenv("debug.s2a.nvk_image_2d_view_of_3d", "0", 1);
    else unsetenv("debug.s2a.nvk_image_2d_view_of_3d");

    char vs_path[512], fs_path[512], cs_path[512];
    snprintf(vs_path, sizeof vs_path, "%s/tri.vert.spv", shdir);
    snprintf(fs_path, sizeof fs_path, "%s/tri_combined.frag.spv", shdir);
    snprintf(cs_path, sizeof cs_path, "%s/img_rgba8.comp.spv", shdir);
    void *vs_spv = NULL, *fs_spv = NULL, *cs_spv = NULL;
    size_t vs_len = 0, fs_len = 0, cs_len = 0;
    REQUIRE(load_spv(vs_path, &vs_spv, &vs_len));
    REQUIRE(load_spv(fs_path, &fs_spv, &fs_len));
    REQUIRE(load_spv(cs_path, &cs_spv, &cs_len));

    struct s2a_vk_device dev = {0};
    char why[256] = "";
    if (s2a_vk_device_create(&dev, why, sizeof why) != 0) {
        fprintf(stderr, "【红】vk_device_create failed: %s\n", why);
        return 1;
    }
    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&od, &err));

    static const nvn_gpu_layout empty_layout = {0};
    nvn_gpu_layout_ent fs_ent = {.set = 0, .cls = NVN_GPU_RES_TEX_SAMP, .access = NVN_GPU_ACC_R,
                                 .binding = 0, .count = 1, .stages = 1u << NVN_GPU_SH_FS};
    nvn_gpu_layout fs_layout = {.ent = &fs_ent, .nent = 1};
    nvn_gpu_layout_ent cs_ent = {.set = 0, .cls = NVN_GPU_RES_IMAGE, .access = NVN_GPU_ACC_W,
                                 .binding = 0, .count = 1, .stages = 1u << NVN_GPU_SH_CS};
    nvn_gpu_layout cs_layout = {.ent = &cs_ent, .nent = 1};

    nvn_gpu_shader vs = NULL, fs = NULL, cs = NULL;
    {
        nvn_gpu_shader_src sd = {.stage = NVN_GPU_SH_VS, .kind = NVN_GPU_BLOB_SPIRV,
                                 .blob = vs_spv, .bytes = vs_len, .entry = "main", .layout = &empty_layout};
        GPU(nvn_gpu_shader_load(&sd, &vs, &err));
    }
    {
        nvn_gpu_shader_src sd = {.stage = NVN_GPU_SH_FS, .kind = NVN_GPU_BLOB_SPIRV,
                                 .blob = fs_spv, .bytes = fs_len, .entry = "main", .layout = &fs_layout};
        GPU(nvn_gpu_shader_load(&sd, &fs, &err));
    }
    {
        nvn_gpu_shader_src sd = {.stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV,
                                 .blob = cs_spv, .bytes = cs_len, .entry = "main", .layout = &cs_layout};
        GPU(nvn_gpu_shader_load(&sd, &cs, &err));
    }
    free(vs_spv); free(fs_spv); free(cs_spv);

    nvn_gpu_pipe gpipe = NULL, cpipe = NULL;
    {
        nvn_gpu_pipe_desc pd = {0};
        pd.st[NVN_GPU_SH_VS].sh = vs;
        pd.st[NVN_GPU_SH_FS].sh = fs;
        pd.prim = NVN_DRAW_PRIMITIVE_TRIANGLES;
        pd.cfmt[0] = NVN_FORMAT_RGBA8;
        pd.samples = 1;
        pd.origin_ll = 1;
        pd.poly.mode = NVN_POLYGON_MODE_FILL;
        pd.color.logic_op = 3;
        pd.chmask.mask = 0xFu;
        pd.attr[0].fmt = NVN_FORMAT_RG32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
        pd.attr[1].fmt = NVN_FORMAT_RG32F; pd.attr[1].offset = 8; pd.attr[1].stream = 0;
        pd.stream[0].stride = sizeof(struct vert_uv);
        GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &gpipe, &err));
    }
    {
        nvn_gpu_stage_desc csd = {.sh = cs};
        GPU(nvn_gpu_cpipe_get(&csd, NVN_GPU_PIPE_SYNC, &cpipe, &err));
    }

    struct vert_uv tri[3] = {
        {{-1.0f, -1.0f}, {0.0f, 0.0f}},
        {{ 3.0f, -1.0f}, {2.0f, 0.0f}},
        {{-1.0f,  3.0f}, {0.0f, 2.0f}},
    };
    nvn_gpu_buf vb = NULL, up = NULL, rb = NULL;
    GPU(nvn_gpu_buf_create(sizeof tri, NVN_GPU_MEM_SHARED, tri, &vb, &err));
    GPU(nvn_gpu_buf_create(D * SLICE_BYTES, NVN_GPU_MEM_SHARED, NULL, &up, &err));
    GPU(nvn_gpu_buf_create(SLICE_BYTES, NVN_GPU_MEM_SHARED, NULL, &rb, &err));

    samp_par sp = {0};
    nvn_gpu_samp samp = NULL;
    GPU(nvn_gpu_samp_create(&sp, &samp, &err));

    nvn_gpu_tex rt = NULL;
    nvn_gpu_view rt_view = NULL;
    {
        nvn_gpu_tex_desc td = {0};
        td.d.target = NVN_TEXTURE_TARGET_2D;
        td.d.format = NVN_FORMAT_RGBA8;
        td.d.width = W; td.d.height = H; td.d.depth = 1; td.d.levels = 1; td.d.samples = 1;
        td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
        GPU(nvn_gpu_tex_create(&td, &rt, &err));
        GPU(nvn_gpu_view_create(rt, NULL, &rt_view, &err));
    }

    // 3D texture #1: RENDER | SAMPLED | STORAGE (depth=4)
    nvn_gpu_tex t3d = NULL;
    {
        nvn_gpu_tex_desc td = {0};
        td.d.target = NVN_TEXTURE_TARGET_3D;
        td.d.format = NVN_FORMAT_RGBA8;
        td.d.width = W; td.d.height = H; td.d.depth = D; td.d.levels = 1; td.d.samples = 1;
        td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_STORAGE;
        GPU(nvn_gpu_tex_create(&td, &t3d, &err));
    }
    // 3D texture #2: SAMPLED-only (no RENDER)
    nvn_gpu_tex t3d_samp_only = NULL;
    {
        nvn_gpu_tex_desc td = {0};
        td.d.target = NVN_TEXTURE_TARGET_3D;
        td.d.format = NVN_FORMAT_RGBA8;
        td.d.width = W; td.d.height = H; td.d.depth = D; td.d.levels = 1; td.d.samples = 1;
        td.usage = NVN_GPU_TEX_SAMPLED;
        GPU(nvn_gpu_tex_create(&td, &t3d_samp_only, &err));
    }

    static const uint8_t k_colors[D][4] = {
        {0xFF, 0x00, 0x00, 0xFF}, // z=0 red
        {0x00, 0xFF, 0x00, 0xFF}, // z=1 green
        {0x00, 0x00, 0xFF, 0xFF}, // z=2 blue
        {0xFF, 0xFF, 0x00, 0xFF}, // z=3 yellow
    };
    static const uint8_t k_magenta[4] = {0xFF, 0x00, 0xFF, 0xFF};
    uint64_t sig = 0;
    REQUIRE(upload_3d_slices(t3d, up, &sig, k_colors) == 0);
    REQUIRE(upload_3d_slices(t3d_samp_only, up, &sig, k_colors) == 0);

    // 2D attachment view of 3D slice z=3: must work in ALL modes (2D_ARRAY_COMPATIBLE_BIT)
    view_t v3 = make_2d_slice_view(3);
    nvn_gpu_view view_z3 = NULL;
    GPU(nvn_gpu_view_create(t3d, &v3, &view_z3, &err));
    {
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_pass_desc pass = {0};
        pass.ncolor = 1;
        pass.color[0].view = view_z3;
        pass.color[0].load = NVN_GPU_LOAD_CLEAR;
        pass.color[0].store = NVN_GPU_STORE_STORE;
        pass.color[0].clear.f[0] = 1.0f; pass.color[0].clear.f[1] = 0.0f;
        pass.color[0].clear.f[2] = 1.0f; pass.color[0].clear.f[3] = 1.0f;
        pass.w = W; pass.h = H; pass.layers = 1;
        nvn_gpu_pass_begin(cmd, &pass);
        nvn_gpu_pass_end(cmd);
        uint64_t s = ++sig;
        GPU(nvn_gpu_cmd_commit(cmd, s, NULL, NULL, &err));
        GPU(nvn_gpu_fence_wait(s, 5000000000ull, &err));
    }
    uint8_t px[SLICE_BYTES];
    REQUIRE(readback_slice(t3d, 3, rb, &sig, px) == 0);
    const int att_ok = check_solid(px, k_magenta);

    int samp_ok = 0, samp_only_ok = 0, store_ok = 0, neighbor_ok = 0;
    int samp_only_rejected = 0, bind_samp_rejected = 0, bind_store_rejected = 0;

    view_t v2 = make_2d_slice_view(2);
    view_t v1 = make_2d_slice_view(1);
    nvn_gpu_view view_z2 = NULL, view_z1 = NULL, view_so_z1 = NULL;
    GPU(nvn_gpu_view_create(t3d, &v2, &view_z2, &err));
    GPU(nvn_gpu_view_create(t3d, &v1, &view_z1, &err));

    nvn_gpu_rc so_rc = nvn_gpu_view_create(t3d_samp_only, &v1, &view_so_z1, &err);
    if (!nvn_gpu_ok(so_rc)) samp_only_rejected = (so_rc.code == NVN_GPU_E_UNSUPPORTED);

    if (is_on) {
        // Test A: sample 2D view of 3D slice z=2 (blue)
        GPU(draw_sample_view(rt_view, gpipe, vb, view_z2, samp, &sig, &err));
        REQUIRE(readback_slice(rt, 0, rb, &sig, px) == 0);
        samp_ok = check_solid(px, k_colors[2]);

        // Test B: compute imageStore into 2D view of 3D slice z=1 + check z=0 & z=2 untouched
        {
            nvn_gpu_cmd cmd = NULL;
            GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
            nvn_gpu_set_pipe(cmd, cpipe);
            nvn_gpu_binding b = {0};
            b.ent = 0; b.cls = NVN_GPU_RES_IMAGE; b.u.view = view_z1;
            nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &b, 1);
            nvn_gpu_dispatch(cmd, cpipe, 1, 1, 1, NULL, 0);
            uint64_t s = ++sig;
            GPU(nvn_gpu_cmd_commit(cmd, s, NULL, NULL, &err));
            GPU(nvn_gpu_fence_wait(s, 5000000000ull, &err));
            REQUIRE(readback_slice(t3d, 1, rb, &sig, px) == 0);
            store_ok = 1;
            for (uint32_t k = 0; k < W * H; k++) {
                uint8_t want[4] = {(uint8_t)(k * 16u), 128u, (uint8_t)(255u - k), 255u};
                if (memcmp(px + k * 4, want, 4) != 0) store_ok = 0;
            }
            uint8_t px0[SLICE_BYTES], px2[SLICE_BYTES];
            REQUIRE(readback_slice(t3d, 0, rb, &sig, px0) == 0);
            REQUIRE(readback_slice(t3d, 2, rb, &sig, px2) == 0);
            neighbor_ok = check_solid(px0, k_colors[0]) && check_solid(px2, k_colors[2]);
        }
        // Test D: sampled-only 3D texture 2D view of slice z=1 (green)
        if (view_so_z1) {
            GPU(draw_sample_view(rt_view, gpipe, vb, view_so_z1, samp, &sig, &err));
            REQUIRE(readback_slice(rt, 0, rb, &sig, px) == 0);
            samp_only_ok = check_solid(px, k_colors[1]);
        }
    } else {
        // Fallback/disabled arms: binding 2D view of 3D as sampled or storage must cleanly fail UNSUPPORTED (0 VVL!)
        {
            nvn_gpu_rc crc = draw_sample_view(rt_view, gpipe, vb, view_z2, samp, &sig, &err);
            bind_samp_rejected = (crc.code == NVN_GPU_E_UNSUPPORTED);
        }
    }

    if (view_so_z1) nvn_gpu_view_release(view_so_z1);
    nvn_gpu_view_release(view_z1);
    nvn_gpu_view_release(view_z2);
    nvn_gpu_view_release(view_z3);
    nvn_gpu_view_release(rt_view);
    nvn_gpu_tex_release(t3d_samp_only);
    nvn_gpu_tex_release(t3d);
    nvn_gpu_tex_release(rt);
    nvn_gpu_samp_release(samp);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_buf_release(up, NULL, NULL);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_shader_release(cs);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
    GPU(nvn_gpu_close(5000000000ull, &err));

    if (!is_on) {
        // Reopen a fresh session (first rejected commit moved session 1 timeline to failed terminal state)
        // and verify storage image binding of 2D view of 3D also cleanly fails NVN_GPU_E_UNSUPPORTED (0 VVL!)
        GPU(nvn_gpu_open(&od, &err));
        void *cs2_spv = NULL;
        size_t cs2_len = 0;
        REQUIRE(load_spv(cs_path, &cs2_spv, &cs2_len));
        nvn_gpu_shader cs2 = NULL;
        nvn_gpu_shader_src sd = {.stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV,
                                 .blob = cs2_spv, .bytes = cs2_len, .entry = "main", .layout = &cs_layout};
        GPU(nvn_gpu_shader_load(&sd, &cs2, &err));
        free(cs2_spv);
        nvn_gpu_pipe cpipe2 = NULL;
        nvn_gpu_stage_desc csd2 = {.sh = cs2};
        GPU(nvn_gpu_cpipe_get(&csd2, NVN_GPU_PIPE_SYNC, &cpipe2, &err));
        nvn_gpu_tex t3d2 = NULL;
        nvn_gpu_tex_desc td = {0};
        td.d.target = NVN_TEXTURE_TARGET_3D;
        td.d.format = NVN_FORMAT_RGBA8;
        td.d.width = W; td.d.height = H; td.d.depth = D; td.d.levels = 1; td.d.samples = 1;
        td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_STORAGE;
        GPU(nvn_gpu_tex_create(&td, &t3d2, &err));
        nvn_gpu_view vz1 = NULL;
        GPU(nvn_gpu_view_create(t3d2, &v1, &vz1, &err));
        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_set_pipe(cmd, cpipe2);
        nvn_gpu_binding b = {0};
        b.ent = 0; b.cls = NVN_GPU_RES_IMAGE; b.u.view = vz1;
        nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &b, 1);
        nvn_gpu_dispatch(cmd, cpipe2, 1, 1, 1, NULL, 0);
        nvn_gpu_rc crc = nvn_gpu_cmd_commit(cmd, 1, NULL, NULL, &err);
        bind_store_rejected = (crc.code == NVN_GPU_E_UNSUPPORTED);
        nvn_gpu_view_release(vz1);
        nvn_gpu_tex_release(t3d2);
        nvn_gpu_shader_release(cs2);
        GPU(nvn_gpu_close(5000000000ull, &err));
    }

    const unsigned img_2d3d = __atomic_load_n(&g_img_3d_2dview_flag, __ATOMIC_RELAXED);
    printf("image-2d-view-of-3d mode=%s dev_ext=%u dev_img=%u dev_samp=%u img_2d3d=%u att=%d samp=%d samp_only=%d store=%d neighbor=%d rej_so=%d rej_samp=%d rej_store=%d\n",
           mode, g_dev_ext_enabled, g_dev_img_feat_enabled, g_dev_samp_feat_enabled, img_2d3d,
           att_ok, samp_ok, samp_only_ok, store_ok, neighbor_ok,
           samp_only_rejected, bind_samp_rejected, bind_store_rejected);

    s2a_vk_device_destroy(&dev, 1);

    if (is_on || is_withdraw) {
        REQUIRE(g_dev_ext_enabled == 1);
        REQUIRE(g_dev_img_feat_enabled == 1);
        REQUIRE(g_dev_samp_feat_enabled == 1);
        REQUIRE(img_2d3d >= 2);
        REQUIRE(att_ok == 1);
        REQUIRE(samp_ok == 1);
        REQUIRE(samp_only_ok == 1);
        REQUIRE(store_ok == 1);
        REQUIRE(neighbor_ok == 1);
    } else if (is_off) {
        REQUIRE(g_dev_ext_enabled == 1);
        REQUIRE(g_dev_img_feat_enabled == 1);
        REQUIRE(g_dev_samp_feat_enabled == 1);
        REQUIRE(img_2d3d == 0);
        REQUIRE(att_ok == 1);
        REQUIRE(samp_only_rejected == 1);
        REQUIRE(bind_samp_rejected == 1);
        REQUIRE(bind_store_rejected == 1);
    } else {
        REQUIRE(g_dev_ext_enabled == 0);
        REQUIRE(g_dev_img_feat_enabled == 0);
        REQUIRE(g_dev_samp_feat_enabled == 0);
        REQUIRE(img_2d3d == 0);
        REQUIRE(att_ok == 1);
        REQUIRE(samp_only_rejected == 1);
        REQUIRE(bind_samp_rejected == 1);
        REQUIRE(bind_store_rejected == 1);
    }
    return 0;
}
