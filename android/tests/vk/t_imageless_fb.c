// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real-Vulkan contract test for VK_KHR_imageless_framebuffer (A/B switch debug.s2a.nvk_imageless_fb).
// Verifies extension + feature gating, sharp reduction in vkCreateFramebuffer / vkDestroyFramebuffer
// across RT/DS view churn, DQ deep-copy of VkRenderPassAttachmentBeginInfo (dq=1), mip/layered/
// mutable-format/color-attachment-usage/pass_reopen cases, and zero-regression fallback when hidden.
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int dq_mode, opt_mode, ifb_mode, hide_extension;
static int enabled_ifb_ext, enabled_ifb_feat;
static unsigned obs_fb_create, obs_ifb_create, obs_fb_destroy, obs_rp_begin, obs_rp_begin_ab;
static unsigned obs_ifb_bad_info;
static unsigned failures, ncases;
static nvn_gpu_err err;

const char *const *nvn_plat_vk_instance_exts(uint32_t *n) { *n = 0; return NULL; }
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) {
    (void)i; (void)w; *s = VK_NULL_HANDLE; return VK_ERROR_EXTENSION_NOT_PRESENT;
}
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) { (void)w; (void)x; (void)y; return 0; }
void nvn_plat_safepoint(void) {}
void nvn_plat_log(int level, const char *tag, const char *msg) {
    (void)level; fprintf(stderr, "[%s] %s\n", tag ? tag : "", msg ? msg : "");
}
int nvn_plat_flag(const char *name, int def) {
    if (!strcmp(name, "nvk_dq")) return dq_mode;
    if (!strcmp(name, "nvk_opt_layout")) return opt_mode;
    if (!strcmp(name, "nvk_imageless_fb")) return ifb_mode;
    if (!strcmp(name, "nvk_pass_cache")) return 1;
    return def;
}

static void fail_line(int line, const char *expr, int code) {
    fprintf(stderr, "fixture failed line %d: %s rc=%d why=%s\n", line, expr, code, err.why);
    exit(2);
}
#define REQUIRE(x) do { if (!(x)) fail_line(__LINE__, #x, 0); } while (0)
#define GPU(x) do { nvn_gpu_rc r_ = (x); if (!nvn_gpu_ok(r_)) fail_line(__LINE__, #x, r_.code); } while (0)

extern __typeof__(vkEnumerateDeviceExtensionProperties) __real_vkEnumerateDeviceExtensionProperties;
extern __typeof__(vkCreateDevice) __real_vkCreateDevice;
extern __typeof__(vkCreateFramebuffer) __real_vkCreateFramebuffer;
extern __typeof__(vkDestroyFramebuffer) __real_vkDestroyFramebuffer;
extern __typeof__(vkCmdBeginRenderPass) __real_vkCmdBeginRenderPass;
extern __typeof__(vkGetDeviceProcAddr) __real_vkGetDeviceProcAddr;

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice phys, const char *layer, uint32_t *count, VkExtensionProperties *props) {
    if (!hide_extension || layer) return __real_vkEnumerateDeviceExtensionProperties(phys, layer, count, props);
    uint32_t total = 0;
    VkResult r = __real_vkEnumerateDeviceExtensionProperties(phys, NULL, &total, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties *all = calloc(total ? total : 1, sizeof *all);
    if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = __real_vkEnumerateDeviceExtensionProperties(phys, NULL, &total, all);
    if (r != VK_SUCCESS) { free(all); return r; }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < total; i++)
        if (strcmp(all[i].extensionName, VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME)) all[kept++] = all[i];
    if (!props) { *count = kept; free(all); return VK_SUCCESS; }
    const uint32_t n = *count < kept ? *count : kept;
    memcpy(props, all, n * sizeof *props);
    *count = n;
    free(all);
    return n < kept ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *dev) {
    enabled_ifb_ext = 0;
    enabled_ifb_feat = 0;
    if (ci) {
        for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
            if (!strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME))
                enabled_ifb_ext = 1;
        for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
            if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES_KHR) {
                const VkPhysicalDeviceImagelessFramebufferFeaturesKHR *f = (const void *)p;
                if (f->imagelessFramebuffer == VK_TRUE) enabled_ifb_feat = 1;
            }
        }
    }
    return __real_vkCreateDevice(phys, ci, alloc, dev);
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateFramebuffer(VkDevice dev, const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *alloc, VkFramebuffer *fb) {
    __atomic_fetch_add(&obs_fb_create, 1, __ATOMIC_RELAXED);
    if (ci && (ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT_KHR)) {
        __atomic_fetch_add(&obs_ifb_create, 1, __ATOMIC_RELAXED);
        int found_att = 0;
        for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
            if (p->sType == VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO_KHR) {
                const VkFramebufferAttachmentsCreateInfoKHR *a = (const void *)p;
                if (a->attachmentImageInfoCount == ci->attachmentCount) found_att = 1;
            }
        }
        if (ci->pAttachments != NULL || !found_att) __atomic_fetch_add(&obs_ifb_bad_info, 1, __ATOMIC_RELAXED);
    }
    return __real_vkCreateFramebuffer(dev, ci, alloc, fb);
}

VKAPI_ATTR void VKAPI_CALL __wrap_vkDestroyFramebuffer(VkDevice dev, VkFramebuffer fb, const VkAllocationCallbacks *alloc) {
    if (fb != VK_NULL_HANDLE) __atomic_fetch_add(&obs_fb_destroy, 1, __ATOMIC_RELAXED);
    __real_vkDestroyFramebuffer(dev, fb, alloc);
}

VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents contents) {
    __atomic_fetch_add(&obs_rp_begin, 1, __ATOMIC_RELAXED);
    if (bi) {
        for (const VkBaseInStructure *p = bi->pNext; p; p = p->pNext) {
            if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO_KHR) {
                const VkRenderPassAttachmentBeginInfoKHR *ab = (const void *)p;
                if (ab->attachmentCount > 0 && ab->pAttachments != NULL)
                    __atomic_fetch_add(&obs_rp_begin_ab, 1, __ATOMIC_RELAXED);
            }
        }
    }
    __real_vkCmdBeginRenderPass(cb, bi, contents);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice d, const char *name) {
    if (!strcmp(name, "vkCmdBeginRenderPass")) return (PFN_vkVoidFunction)__wrap_vkCmdBeginRenderPass;
    if (!strcmp(name, "vkDestroyFramebuffer")) return (PFN_vkVoidFunction)__wrap_vkDestroyFramebuffer;
    return __real_vkGetDeviceProcAddr(d, name);
}

_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkEnumerateDeviceExtensionProperties), PFN_vkEnumerateDeviceExtensionProperties), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateDevice), PFN_vkCreateDevice), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateFramebuffer), PFN_vkCreateFramebuffer), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkDestroyFramebuffer), PFN_vkDestroyFramebuffer), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCmdBeginRenderPass), PFN_vkCmdBeginRenderPass), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkGetDeviceProcAddr), PFN_vkGetDeviceProcAddr), "sig");

static void obs_reset(void) {
    obs_fb_create = obs_ifb_create = obs_fb_destroy = obs_rp_begin = obs_rp_begin_ab = obs_ifb_bad_info = 0;
}

static void done(void *u, const nvn_gpu_done *d) {
    (void)u;
    REQUIRE(d->status == NVN_GPU_DONE_OK);
}

static nvn_gpu_shader load_sh(const char *dir, const char *name, nvn_gpu_stage st, const nvn_gpu_layout *lay) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s.spv", dir, name);
    FILE *f = fopen(p, "rb"); REQUIRE(f);
    REQUIRE(!fseek(f, 0, SEEK_END));
    long n = ftell(f); REQUIRE(n > 0);
    rewind(f);
    void *b = malloc((size_t)n); REQUIRE(b);
    REQUIRE(fread(b, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    nvn_gpu_shader_src sd = {0};
    sd.stage = st; sd.kind = NVN_GPU_BLOB_SPIRV; sd.entry = "main";
    sd.blob = b; sd.bytes = (size_t)n; sd.layout = lay;
    nvn_gpu_shader s = NULL;
    GPU(nvn_gpu_shader_load(&sd, &s, &err));
    free(b);
    return s;
}

static nvn_gpu_pipe make_pipe_ex(nvn_gpu_shader vs, nvn_gpu_shader fs, uint32_t ncol, const uint32_t *cfmt, uint32_t dfmt, int depth_test) {
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3;
    pd.chmask.mask = 0xffff;
    pd.prim = 4;
    pd.samples = 1;
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    for (uint32_t i = 0; i < ncol && i < 8; i++) pd.cfmt[i] = cfmt[i];
    pd.dfmt = dfmt;
    if (dfmt == NVN_FORMAT_DEPTH24_STENCIL8) pd.sfmt = dfmt;
    if (depth_test) {
        pd.ds.dt = 1;
        pd.ds.dw = 1;
        pd.ds.df = 2;   // VK_COMPARE_OP_LESS
    }
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));
    return pipe;
}

static nvn_gpu_dyn make_dyn(uint32_t w, uint32_t h) {
    nvn_gpu_dyn dyn = {0};
    dyn.nvp = 1; dyn.vp[0][2] = (float)w; dyn.vp[0][3] = (float)h; dyn.vp[0][5] = 1.0f;
    dyn.has_sc = 1; dyn.sc[2] = w; dyn.sc[3] = h;
    return dyn;
}

static unsigned check_rgba(const unsigned char *px, unsigned r, unsigned g, unsigned b, unsigned tol) {
    unsigned bad = 0;
    for (unsigned i = 0; i < 64; i += 4) {
        if ((unsigned)abs((int)px[i] - (int)r) > tol ||
            (unsigned)abs((int)px[i + 1] - (int)g) > tol ||
            (unsigned)abs((int)px[i + 2] - (int)b) > tol) bad++;
    }
    return bad;
}

struct tc { const char *name; int id; };
static const struct tc k_cases[] = {
    {"rt-swap-churn-reuse", 0},
    {"mrt-color-plus-depth-churn", 1},
    {"mip-level-attachment", 2},
    {"mutable-view-fmt-and-color-att-usage", 3},
    {"layered-2d-array-attachment", 4},
    {"pass-reopen-partial-area-imageless", 5},
};

static void run_case(struct s2a_vk_device *device, const char *dir, const struct tc *tc) {
    ncases++;
    nvn_gpu_open_desc od = {0};
    od.host_device = device;
    od.app_label = tc->name;
    GPU(nvn_gpu_open(&od, &err));
    obs_reset();

    const int active = device->imageless_framebuffer_enabled && ifb_mode;
    nvn_gpu_shader vs = load_sh(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs_pc = NULL, fs_mrt = NULL, vs_z = NULL;
    nvn_gpu_layout_ent pc_ent = {0};
    (void)pc_ent;
    nvn_gpu_layout pc_lay = {.push_bytes = 16};
    fs_pc = load_sh(dir, "solid_pc.frag", NVN_GPU_SH_FS, &pc_lay);

    unsigned op_bad = 0, px_bad = 0;
    unsigned fb_destroy_mid = 0;

    if (tc->id == 0) {
        // 9 distinct 4x4 RGBA8 RTs: draw 6, destroy 3 (forcing view-churn), draw 3 brand-new RTs.
        // Imageless FB must create ONLY 1 FBO total and destroy 0 during view churn!
        uint32_t fmt = NVN_FORMAT_RGBA8;
        nvn_gpu_pipe pipe = make_pipe_ex(vs, fs_pc, 1, &fmt, 0, 0);
        nvn_gpu_tex_desc td = {0};
        td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8;
        td.d.width = td.d.height = 4; td.d.depth = 1; td.d.levels = 1;
        td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;

        nvn_gpu_buf rb = NULL;
        unsigned char z[9 * 64]; memset(z, 0, sizeof z);
        GPU(nvn_gpu_buf_create(sizeof z, NVN_GPU_MEM_SHARED, z, &rb, &err));

        nvn_gpu_tex rts[9] = {0};
        nvn_gpu_view vrts[9] = {0};
        for (int i = 0; i < 6; i++) {
            GPU(nvn_gpu_tex_create(&td, &rts[i], &err));
            GPU(nvn_gpu_view_create(rts[i], NULL, &vrts[i], &err));
        }

        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_dyn dyn = make_dyn(4, 4);
        nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
        for (int i = 0; i < 6; i++) {
            nvn_gpu_pass_desc p = {0}; p.w = p.h = 4; p.layers = 1; p.ncolor = 1;
            p.color[0].view = vrts[i]; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
            nvn_gpu_pass_begin(cmd, &p);
            nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn);
            float col[4] = {(float)(i + 1) / 10.0f, 1.0f - (float)(i + 1) / 10.0f, 0.25f, 1.0f};
            nvn_gpu_push(cmd, NVN_GPU_SH_FS, 0, col, sizeof col);
            nvn_gpu_draw(cmd, &da);
            nvn_gpu_pass_end(cmd);
            nvn_gpu_copy_tex_buf(cmd, rts[i], 0, 0, NULL, rb, (uint64_t)i * 64, 16, 64);
        }
        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));

        // Churn: destroy first 3 views + textures before creating 3 brand-new RTs!
        for (int i = 0; i < 3; i++) {
            nvn_gpu_view_release(vrts[i]); vrts[i] = NULL;
            nvn_gpu_tex_release(rts[i]); rts[i] = NULL;
        }
        // 段完成值先于对象退引用发布。等同一 worker 完成下一空段再读取销毁计数。
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        GPU(nvn_gpu_cmd_commit(cmd, 2, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(2, 5000000000ull, &err));
        fb_destroy_mid = obs_fb_destroy;

        for (int i = 6; i < 9; i++) {
            GPU(nvn_gpu_tex_create(&td, &rts[i], &err));
            GPU(nvn_gpu_view_create(rts[i], NULL, &vrts[i], &err));
        }
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        for (int i = 6; i < 9; i++) {
            nvn_gpu_pass_desc p = {0}; p.w = p.h = 4; p.layers = 1; p.ncolor = 1;
            p.color[0].view = vrts[i]; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
            nvn_gpu_pass_begin(cmd, &p);
            nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn);
            float col[4] = {(float)(i + 1) / 10.0f, 1.0f - (float)(i + 1) / 10.0f, 0.5f, 1.0f};
            nvn_gpu_push(cmd, NVN_GPU_SH_FS, 0, col, sizeof col);
            nvn_gpu_draw(cmd, &da);
            nvn_gpu_pass_end(cmd);
            nvn_gpu_copy_tex_buf(cmd, rts[i], 0, 0, NULL, rb, (uint64_t)i * 64, 16, 64);
        }
        GPU(nvn_gpu_cmd_commit(cmd, 3, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(3, 5000000000ull, &err));

        const unsigned char *px = nvn_gpu_buf_cpu(rb);
        for (int i = 0; i < 9; i++) {
            unsigned r = (unsigned)lround(((double)(i + 1) / 10.0) * 255.0);
            unsigned g = (unsigned)lround((1.0 - (double)(i + 1) / 10.0) * 255.0);
            unsigned b = i < 6 ? 64 : 128;
            px_bad += check_rgba(px + (size_t)i * 64, r, g, b, 2);
        }
        if (active) {
            if (obs_ifb_create != 1 || obs_fb_create != 1 || fb_destroy_mid != 0 || obs_rp_begin_ab != 9 || obs_ifb_bad_info != 0) op_bad++;
        } else {
            if (obs_ifb_create != 0 || obs_rp_begin_ab != 0 || obs_fb_create != 9 || fb_destroy_mid != 3) op_bad++;
        }
        for (int i = 3; i < 9; i++) {
            if (vrts[i]) nvn_gpu_view_release(vrts[i]);
            if (rts[i]) nvn_gpu_tex_release(rts[i]);
        }
        nvn_gpu_buf_release(rb, NULL, NULL);
    } else if (tc->id == 1) {
        // MRT (2 color attachments) + Depth24Stencil8 across 4 distinct (rt0, rt1, ds) triples.
        nvn_gpu_layout vs_lay = {.push_off = 0, .push_bytes = 16};
        nvn_gpu_layout mrt_lay = {.push_off = 16, .push_bytes = 16};
        vs_z = load_sh(dir, "fullscreen_z.vert", NVN_GPU_SH_VS, &vs_lay);
        fs_mrt = load_sh(dir, "mrt_pc.frag", NVN_GPU_SH_FS, &mrt_lay);
        uint32_t cfmt[2] = {NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA8};
        nvn_gpu_pipe pipe = make_pipe_ex(vs_z, fs_mrt, 2, cfmt, NVN_FORMAT_DEPTH24_STENCIL8, 1);

        nvn_gpu_tex_desc ctd = {0};
        ctd.d.target = NVN_TEXTURE_TARGET_2D; ctd.d.format = NVN_FORMAT_RGBA8;
        ctd.d.width = ctd.d.height = 4; ctd.d.depth = 1; ctd.d.levels = 1;
        ctd.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
        nvn_gpu_tex_desc dtd = ctd;
        dtd.d.format = NVN_FORMAT_DEPTH24_STENCIL8;

        nvn_gpu_tex c0[4], c1[4], ds[4];
        nvn_gpu_view vc0[4], vc1[4], vds[4];
        for (int i = 0; i < 4; i++) {
            GPU(nvn_gpu_tex_create(&ctd, &c0[i], &err)); GPU(nvn_gpu_view_create(c0[i], NULL, &vc0[i], &err));
            GPU(nvn_gpu_tex_create(&ctd, &c1[i], &err)); GPU(nvn_gpu_view_create(c1[i], NULL, &vc1[i], &err));
            GPU(nvn_gpu_tex_create(&dtd, &ds[i], &err)); GPU(nvn_gpu_view_create(ds[i], NULL, &vds[i], &err));
        }
        unsigned char z[8 * 64]; memset(z, 0, sizeof z);
        nvn_gpu_buf rb = NULL;
        GPU(nvn_gpu_buf_create(sizeof z, NVN_GPU_MEM_SHARED, z, &rb, &err));

        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_dyn dyn = make_dyn(4, 4);
        nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
        for (int i = 0; i < 4; i++) {
            nvn_gpu_pass_desc p = {0}; p.w = p.h = 4; p.layers = 1; p.ncolor = 2;
            p.color[0].view = vc0[i]; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
            p.color[1].view = vc1[i]; p.color[1].load = NVN_GPU_LOAD_CLEAR; p.color[1].store = NVN_GPU_STORE_STORE;
            p.depth.view = vds[i]; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE;
            p.stencil.view = vds[i]; p.stencil.load = NVN_GPU_LOAD_CLEAR; p.stencil.store = NVN_GPU_STORE_STORE;
            p.clear_depth = 1.0f;
            nvn_gpu_pass_begin(cmd, &p);
            nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn);
            // First draw near (z = 0.25): RT0 = (0.25*(i+1), 0.75, 0.25), RT1 = (0.25, 0.25*(i+1), 0.75)
            float near_z[4] = {0.25f, 0.0f, 0.0f, 0.0f};
            float near_c[4] = {0.25f * (float)(i + 1), 0.75f, 0.25f, 1.0f};
            nvn_gpu_push(cmd, NVN_GPU_SH_VS, 0, near_z, sizeof near_z);
            nvn_gpu_push(cmd, NVN_GPU_SH_FS, 16, near_c, sizeof near_c);
            nvn_gpu_draw(cmd, &da);
            // Second draw far (z = 0.75): must be rejected by depth test!
            float far_z[4] = {0.75f, 0.0f, 0.0f, 0.0f};
            float far_c[4] = {1.0f, 0.0f, 0.0f, 1.0f};
            nvn_gpu_push(cmd, NVN_GPU_SH_VS, 0, far_z, sizeof far_z);
            nvn_gpu_push(cmd, NVN_GPU_SH_FS, 16, far_c, sizeof far_c);
            nvn_gpu_draw(cmd, &da);
            nvn_gpu_pass_end(cmd);
            nvn_gpu_copy_tex_buf(cmd, c0[i], 0, 0, NULL, rb, (uint64_t)(i * 2) * 64, 16, 64);
            nvn_gpu_copy_tex_buf(cmd, c1[i], 0, 0, NULL, rb, (uint64_t)(i * 2 + 1) * 64, 16, 64);
        }
        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));

        const unsigned char *px = nvn_gpu_buf_cpu(rb);
        for (int i = 0; i < 4; i++) {
            unsigned v = (unsigned)lround(0.25 * (double)(i + 1) * 255.0);
            px_bad += check_rgba(px + (size_t)(i * 2) * 64, v, 191, 64, 2);
            px_bad += check_rgba(px + (size_t)(i * 2 + 1) * 64, 64, v, 191, 2);
        }
        if (active) {
            if (obs_ifb_create != 1 || obs_fb_create != 1 || obs_rp_begin_ab != 4 || obs_ifb_bad_info != 0) op_bad++;
        } else {
            if (obs_ifb_create != 0 || obs_fb_create != 4) op_bad++;
        }
        for (int i = 0; i < 4; i++) {
            nvn_gpu_view_release(vc0[i]); nvn_gpu_tex_release(c0[i]);
            nvn_gpu_view_release(vc1[i]); nvn_gpu_tex_release(c1[i]);
            nvn_gpu_view_release(vds[i]); nvn_gpu_tex_release(ds[i]);
        }
        nvn_gpu_buf_release(rb, NULL, NULL);
    } else if (tc->id == 2) {
        // Mip-level > 0 attachment: 8x8 base (levels=2), render to mip 1 (4x4) on 2 textures -> 1 imageless FB!
        uint32_t fmt = NVN_FORMAT_RGBA8;
        nvn_gpu_pipe pipe = make_pipe_ex(vs, fs_pc, 1, &fmt, 0, 0);
        nvn_gpu_tex_desc td = {0};
        td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8;
        td.d.width = td.d.height = 8; td.d.depth = 1; td.d.levels = 2;
        td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;

        nvn_gpu_tex t[2]; nvn_gpu_view vm[2];
        view_t vd = {0}; vd.set_levels = 1; vd.base_level = 1; vd.nlevels = 1;
        for (int i = 0; i < 2; i++) {
            GPU(nvn_gpu_tex_create(&td, &t[i], &err));
            GPU(nvn_gpu_view_create(t[i], &vd, &vm[i], &err));
        }
        unsigned char z[2 * 64]; memset(z, 0, sizeof z);
        nvn_gpu_buf rb = NULL;
        GPU(nvn_gpu_buf_create(sizeof z, NVN_GPU_MEM_SHARED, z, &rb, &err));

        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_dyn dyn = make_dyn(4, 4);
        nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
        for (int i = 0; i < 2; i++) {
            nvn_gpu_pass_desc p = {0}; p.w = p.h = 4; p.layers = 1; p.ncolor = 1;
            p.color[0].view = vm[i]; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
            nvn_gpu_pass_begin(cmd, &p);
            nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn);
            float col[4] = {i ? 0.25f : 0.75f, 0.5f, i ? 1.0f : 0.25f, 1.0f};
            nvn_gpu_push(cmd, NVN_GPU_SH_FS, 0, col, sizeof col);
            nvn_gpu_draw(cmd, &da);
            nvn_gpu_pass_end(cmd);
            nvn_gpu_copy_tex_buf(cmd, t[i], 1, 0, NULL, rb, (uint64_t)i * 64, 16, 64);
        }
        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));

        const unsigned char *px = nvn_gpu_buf_cpu(rb);
        px_bad += check_rgba(px, 191, 128, 64, 2);
        px_bad += check_rgba(px + 64, 64, 128, 255, 2);
        if (active) {
            if (obs_ifb_create != 1 || obs_fb_create != 1 || obs_rp_begin_ab != 2 || obs_ifb_bad_info != 0) op_bad++;
        } else {
            if (obs_ifb_create != 0 || obs_fb_create != 2) op_bad++;
        }
        for (int i = 0; i < 2; i++) { nvn_gpu_view_release(vm[i]); nvn_gpu_tex_release(t[i]); }
        nvn_gpu_buf_release(rb, NULL, NULL);
    } else if (tc->id == 3) {
        // Mutable-format (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT + VkImageFormatListCreateInfo)
        // + nvn_gpu_view_create_color_attachment (VkImageViewUsageCreateInfo.usage = COLOR_ATTACHMENT_BIT).
        // 2 textures x 2 formats (RGBA8 UNORM & RGBA8_SRGB) = 4 passes -> 2 imageless FBOs (vs 4 when off)!
        uint32_t fmt_unorm = NVN_FORMAT_RGBA8, fmt_srgb = NVN_FORMAT_RGBA8_SRGB;
        nvn_gpu_pipe pipe_u = make_pipe_ex(vs, fs_pc, 1, &fmt_unorm, 0, 0);
        nvn_gpu_pipe pipe_s = make_pipe_ex(vs, fs_pc, 1, &fmt_srgb, 0, 0);

        nvn_gpu_tex_desc td = {0};
        td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8;
        td.d.width = td.d.height = 4; td.d.depth = 1; td.d.levels = 1;
        td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_VIEW_FMT;

        nvn_gpu_tex t[2]; nvn_gpu_view vu[2], vsrgb[2];
        view_t sv = {0}; sv.set_format = 1; sv.format = NVN_FORMAT_RGBA8_SRGB;
        for (int i = 0; i < 2; i++) {
            GPU(nvn_gpu_tex_create(&td, &t[i], &err));
            GPU(nvn_gpu_view_create(t[i], NULL, &vu[i], &err));
            GPU(nvn_gpu_view_create_color_attachment(t[i], &sv, &vsrgb[i], &err));
        }
        unsigned char z[4 * 64]; memset(z, 0, sizeof z);
        nvn_gpu_buf rb = NULL;
        GPU(nvn_gpu_buf_create(sizeof z, NVN_GPU_MEM_SHARED, z, &rb, &err));

        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_dyn dyn = make_dyn(4, 4);
        nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
        float lin[4] = {0.25f, 0.5f, 0.0f, 1.0f};
        for (int i = 0; i < 2; i++) {
            nvn_gpu_pass_desc p = {0}; p.w = p.h = 4; p.layers = 1; p.ncolor = 1;
            p.color[0].view = vu[i]; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
            nvn_gpu_pass_begin(cmd, &p);
            nvn_gpu_set_pipe(cmd, pipe_u); nvn_gpu_set_dyn(cmd, &dyn);
            nvn_gpu_push(cmd, NVN_GPU_SH_FS, 0, lin, sizeof lin);
            nvn_gpu_draw(cmd, &da);
            nvn_gpu_pass_end(cmd);
            nvn_gpu_copy_tex_buf(cmd, t[i], 0, 0, NULL, rb, (uint64_t)(i * 2) * 64, 16, 64);

            p.color[0].view = vsrgb[i];
            nvn_gpu_pass_begin(cmd, &p);
            nvn_gpu_set_pipe(cmd, pipe_s); nvn_gpu_set_dyn(cmd, &dyn);
            nvn_gpu_push(cmd, NVN_GPU_SH_FS, 0, lin, sizeof lin);
            nvn_gpu_draw(cmd, &da);
            nvn_gpu_pass_end(cmd);
            nvn_gpu_copy_tex_buf(cmd, t[i], 0, 0, NULL, rb, (uint64_t)(i * 2 + 1) * 64, 16, 64);
        }
        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));

        const unsigned char *px = nvn_gpu_buf_cpu(rb);
        for (int i = 0; i < 2; i++) {
            // Linear 0.25 -> 64 UNORM, ~137 sRGB; 0.5 -> 128 UNORM, ~188 sRGB
            px_bad += check_rgba(px + (size_t)(i * 2) * 64, 64, 128, 0, 2);
            px_bad += check_rgba(px + (size_t)(i * 2 + 1) * 64, 137, 188, 0, 3);
        }
        if (active) {
            if (obs_ifb_create != 2 || obs_fb_create != 2 || obs_rp_begin_ab != 4 || obs_ifb_bad_info != 0) op_bad++;
        } else {
            if (obs_ifb_create != 0 || obs_fb_create != 4) op_bad++;
        }
        for (int i = 0; i < 2; i++) {
            nvn_gpu_view_release(vu[i]); nvn_gpu_view_release(vsrgb[i]); nvn_gpu_tex_release(t[i]);
        }
        nvn_gpu_buf_release(rb, NULL, NULL);
    } else if (tc->id == 4) {
        // Layered 2D_ARRAY attachment (layers=2, exercises view_attachment att_n & layerCount=2).
        uint32_t fmt = NVN_FORMAT_RGBA8;
        nvn_gpu_pipe pipe = make_pipe_ex(vs, fs_pc, 1, &fmt, 0, 0);
        nvn_gpu_tex_desc td = {0};
        td.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; td.d.format = NVN_FORMAT_RGBA8;
        td.d.width = td.d.height = 4; td.d.depth = 2; td.d.levels = 1;
        td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;

        nvn_gpu_tex t[2]; nvn_gpu_view v[2];
        for (int i = 0; i < 2; i++) {
            GPU(nvn_gpu_tex_create(&td, &t[i], &err));
            GPU(nvn_gpu_view_create(t[i], NULL, &v[i], &err));
        }
        unsigned char z[4 * 64]; memset(z, 0, sizeof z);
        nvn_gpu_buf rb = NULL;
        GPU(nvn_gpu_buf_create(sizeof z, NVN_GPU_MEM_SHARED, z, &rb, &err));

        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        nvn_gpu_dyn dyn = make_dyn(4, 4);
        nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
        for (int i = 0; i < 2; i++) {
            nvn_gpu_pass_desc p = {0}; p.w = p.h = 4; p.layers = 2; p.ncolor = 1;
            p.color[0].view = v[i]; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
            p.color[0].clear.f[0] = 0.0f; p.color[0].clear.f[1] = 0.25f; p.color[0].clear.f[2] = 0.75f; p.color[0].clear.f[3] = 1.0f;
            nvn_gpu_pass_begin(cmd, &p);
            nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn);
            float col[4] = {1.0f, 0.5f, 0.0f, 1.0f};
            nvn_gpu_push(cmd, NVN_GPU_SH_FS, 0, col, sizeof col);
            nvn_gpu_draw(cmd, &da);
            nvn_gpu_pass_end(cmd);
            // Layer 0 was drawn (255, 128, 0); Layer 1 kept clear color (0, 64, 191)!
            nvn_gpu_copy_tex_buf(cmd, t[i], 0, 0, NULL, rb, (uint64_t)(i * 2) * 64, 16, 64);
            nvn_gpu_copy_tex_buf(cmd, t[i], 0, 1, NULL, rb, (uint64_t)(i * 2 + 1) * 64, 16, 64);
        }
        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));

        const unsigned char *px = nvn_gpu_buf_cpu(rb);
        for (int i = 0; i < 2; i++) {
            px_bad += check_rgba(px + (size_t)(i * 2) * 64, 255, 128, 0, 2);
            px_bad += check_rgba(px + (size_t)(i * 2 + 1) * 64, 0, 64, 191, 2);
        }
        if (active) {
            if (obs_ifb_create != 1 || obs_fb_create != 1 || obs_rp_begin_ab != 2 || obs_ifb_bad_info != 0) op_bad++;
        } else {
            if (obs_ifb_create != 0 || obs_fb_create != 2) op_bad++;
        }
        for (int i = 0; i < 2; i++) { nvn_gpu_view_release(v[i]); nvn_gpu_tex_release(t[i]); }
        nvn_gpu_buf_release(rb, NULL, NULL);
    } else if (tc->id == 5) {
        // pass_reopen path: start with LOAD + partial render area (2x2), then draw with full 4x4 scissor ->
        // triggers pass_reopen mid-pass, which must ALSO use imageless FB + AttachmentBeginInfo cleanly!
        uint32_t fmt = NVN_FORMAT_RGBA8;
        nvn_gpu_pipe pipe = make_pipe_ex(vs, fs_pc, 1, &fmt, 0, 0);
        nvn_gpu_tex_desc td = {0};
        td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8;
        td.d.width = td.d.height = 4; td.d.depth = 1; td.d.levels = 1;
        td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;

        nvn_gpu_tex t = NULL; nvn_gpu_view v = NULL;
        GPU(nvn_gpu_tex_create(&td, &t, &err));
        GPU(nvn_gpu_view_create(t, NULL, &v, &err));
        unsigned char z[64]; memset(z, 0, sizeof z);
        nvn_gpu_buf rb = NULL;
        GPU(nvn_gpu_buf_create(sizeof z, NVN_GPU_MEM_SHARED, z, &rb, &err));

        nvn_gpu_cmd cmd = NULL;
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
        // Clear first
        nvn_gpu_pass_desc pc = {0}; pc.w = pc.h = 4; pc.layers = 1; pc.ncolor = 1;
        pc.color[0].view = v; pc.color[0].load = NVN_GPU_LOAD_CLEAR; pc.color[0].store = NVN_GPU_STORE_STORE;
        nvn_gpu_pass_begin(cmd, &pc);
        nvn_gpu_pass_end(cmd);

        // Partial area LOAD pass -> draw outside area forces pass_reopen!
        nvn_gpu_pass_desc p = {0}; p.w = p.h = 4; p.layers = 1; p.ncolor = 1;
        p.area_x = 0; p.area_y = 0; p.area_w = 2; p.area_h = 2;
        p.color[0].view = v; p.color[0].load = NVN_GPU_LOAD_LOAD; p.color[0].store = NVN_GPU_STORE_STORE;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_dyn dyn = make_dyn(4, 4);
        nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn);
        float col[4] = {0.25f, 1.0f, 0.5f, 1.0f};
        nvn_gpu_push(cmd, NVN_GPU_SH_FS, 0, col, sizeof col);
        nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_draw(cmd, &da);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, t, 0, 0, NULL, rb, 0, 16, 64);

        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));

        px_bad += check_rgba(nvn_gpu_buf_cpu(rb), 64, 255, 128, 2);
        if (active) {
            if (obs_rp_begin < 3 || obs_ifb_create == 0 || obs_ifb_create != obs_fb_create || obs_rp_begin_ab != obs_rp_begin || obs_ifb_bad_info != 0) op_bad++;
        } else {
            if (obs_rp_begin < 3 || obs_ifb_create != 0 || obs_rp_begin_ab != 0) op_bad++;
        }
        nvn_gpu_view_release(v); nvn_gpu_tex_release(t); nvn_gpu_buf_release(rb, NULL, NULL);
    }

    const unsigned bad = op_bad || px_bad;
    printf("imageless-fb case=%s active=%d fb=%u ifb=%u fbd_mid=%u rpb=%u rpb_ab=%u bad_info=%u op_bad=%u px_bad=%u bad=%u\n",
           tc->name, active, obs_fb_create, obs_ifb_create, fb_destroy_mid,
           obs_rp_begin, obs_rp_begin_ab, obs_ifb_bad_info, op_bad, px_bad, bad);
    failures += !!bad;

    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs_pc);
    if (fs_mrt) nvn_gpu_shader_release(fs_mrt);
    if (vs_z) nvn_gpu_shader_release(vs_z);
    GPU(nvn_gpu_close(5000000000ull, &err));
}

int main(int argc, char **argv) {
    if (argc != 6) return 2;
    dq_mode = atoi(argv[2]);
    opt_mode = atoi(argv[3]);
    ifb_mode = atoi(argv[4]);
    hide_extension = atoi(argv[5]);

    struct s2a_vk_device device;
    char why[256] = {0};
    REQUIRE(!s2a_vk_device_create(&device, why, sizeof why));
    REQUIRE(enabled_ifb_ext == !hide_extension);
    REQUIRE(enabled_ifb_feat == !hide_extension);
    REQUIRE(device.imageless_framebuffer_enabled == !hide_extension);

    uint32_t n = 0;
    REQUIRE(__real_vkEnumerateDeviceExtensionProperties(device.physical, NULL, &n, NULL) == VK_SUCCESS);
    VkExtensionProperties *ext = calloc(n ? n : 1, sizeof *ext); REQUIRE(ext);
    REQUIRE(__real_vkEnumerateDeviceExtensionProperties(device.physical, NULL, &n, ext) == VK_SUCCESS);
    int advertised = 0;
    for (uint32_t i = 0; i < n; i++)
        advertised |= !strcmp(ext[i].extensionName, VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME);
    free(ext);
    REQUIRE(advertised);

    for (unsigned i = 0; i < sizeof k_cases / sizeof *k_cases; i++)
        run_case(&device, argv[1], &k_cases[i]);

    const int dev_ifb = device.imageless_framebuffer_enabled;
    REQUIRE(!s2a_vk_device_destroy(&device, 1));
    printf("imageless-fb cases=%u failures=%u dq=%d opt=%d ifb=%d hidden=%d ext-enabled=%d feat-enabled=%d dev-enabled=%d\n",
           ncases, failures, dq_mode, opt_mode, ifb_mode, hide_extension, enabled_ifb_ext, enabled_ifb_feat, dev_ifb);
    return failures ? 1 : 0;
}
