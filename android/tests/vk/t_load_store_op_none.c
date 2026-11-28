// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real Vulkan (lavapipe + VVL/SyncVal) pixel-readback & attachment load/store observer
// for VK_EXT_load_store_op_none (debug.s2a.nvk_load_store_op_none).
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nvn_gpu_err err;
static int dq_mode, opt_mode, ls_none_mode, hide_extension;
static unsigned failures, ncases;

#define REQUIRE(x) do { \
    if (!(x)) { \
        fprintf(stderr, "fixture failed line %d: %s rc=%d why=%s\n", __LINE__, #x, err.rc.code, err.why); \
        exit(2); \
    } \
} while (0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))

static pthread_mutex_t obs_mu = PTHREAD_MUTEX_INITIALIZER;
static int enabled_none;

struct obs_rp {
    VkRenderPass pass;
    uint32_t n, ncol;
    uint32_t cols[8], ds;
    VkAttachmentDescription a[9];
};
struct obs_begin_rec {
    VkRect2D area;
    uint32_t n, ncol, cols[8], ds;
    VkAttachmentDescription a[9];
};
enum { OBS_DEPTH_TEST = 1, OBS_DEPTH_WRITE = 2, OBS_STENCIL_TEST = 4, OBS_STENCIL_OP = 8 };
struct obs_pipe {
    VkPipeline pipe;
    uint32_t ncol, mask[8], dynamic_ds;
    VkPipelineDepthStencilStateCreateInfo ds;
};

static struct obs_rp obs_rps[512];
static uint32_t obs_nrp;
static struct obs_begin_rec obs_begins[64];
static uint32_t obs_nbegin;
static VkRenderPass obs_active_rp;
static struct obs_pipe obs_pipes[128];
static uint32_t obs_npipe, obs_bound_pipe;
static VkPipelineDepthStencilStateCreateInfo obs_dynamic_ds;
static uint32_t obs_stencil_wmask[2] = {0xff, 0xff};
static uint32_t obs_unsafe_write, obs_unsafe_read;

static void obs_reset(void) {
    pthread_mutex_lock(&obs_mu);
    obs_nrp = 0;
    obs_nbegin = 0;
    obs_active_rp = VK_NULL_HANDLE;
    obs_npipe = 0;
    obs_bound_pipe = 128;
    memset(&obs_dynamic_ds, 0, sizeof obs_dynamic_ds);
    obs_stencil_wmask[0] = obs_stencil_wmask[1] = 0xff;
    obs_unsafe_write = 0;
    obs_unsafe_read = 0;
    pthread_mutex_unlock(&obs_mu);
}

extern __typeof__(vkCreateDevice) __real_vkCreateDevice;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(
    VkPhysicalDevice p, const VkDeviceCreateInfo *ci,
    const VkAllocationCallbacks *a, VkDevice *d) {
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        if (!strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME))
            enabled_none = 1;
    }
    return __real_vkCreateDevice(p, ci, a, d);
}

extern __typeof__(vkEnumerateDeviceExtensionProperties) __real_vkEnumerateDeviceExtensionProperties;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice p, const char *layer, uint32_t *count, VkExtensionProperties *props) {
    if (!hide_extension || layer)
        return __real_vkEnumerateDeviceExtensionProperties(p, layer, count, props);
    uint32_t n = 0;
    VkResult r = __real_vkEnumerateDeviceExtensionProperties(p, layer, &n, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties *all = calloc(n ? n : 1, sizeof *all);
    REQUIRE(all);
    r = __real_vkEnumerateDeviceExtensionProperties(p, layer, &n, all);
    if (r != VK_SUCCESS) { free(all); return r; }
    uint32_t visible = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(all[i].extensionName, VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME))
            all[visible++] = all[i];
    }
    if (!props) { *count = visible; free(all); return VK_SUCCESS; }
    uint32_t written = *count < visible ? *count : visible;
    memcpy(props, all, written * sizeof *all);
    *count = written;
    free(all);
    return written < visible ? VK_INCOMPLETE : VK_SUCCESS;
}

extern __typeof__(vkCreateRenderPass) __real_vkCreateRenderPass;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateRenderPass(
    VkDevice d, const VkRenderPassCreateInfo *p,
    const VkAllocationCallbacks *a, VkRenderPass *out) {
    VkResult r = __real_vkCreateRenderPass(d, p, a, out);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&obs_mu);
        uint32_t k = 0;
        while (k < obs_nrp && obs_rps[k].pass != *out) k++;
        if (k == obs_nrp) { REQUIRE(obs_nrp < 512); obs_nrp++; }
        REQUIRE(p->attachmentCount <= 9 && p->subpassCount == 1 && p->pSubpasses[0].colorAttachmentCount <= 8);
        obs_rps[k].pass = *out;
        obs_rps[k].n = p->attachmentCount;
        obs_rps[k].ncol = p->pSubpasses[0].colorAttachmentCount;
        for (uint32_t i = 0; i < obs_rps[k].ncol; i++)
            obs_rps[k].cols[i] = p->pSubpasses[0].pColorAttachments[i].attachment;
        obs_rps[k].ds = p->pSubpasses[0].pDepthStencilAttachment
            ? p->pSubpasses[0].pDepthStencilAttachment->attachment : VK_ATTACHMENT_UNUSED;
        if (p->attachmentCount)
            memcpy(obs_rps[k].a, p->pAttachments, p->attachmentCount * sizeof *p->pAttachments);
        pthread_mutex_unlock(&obs_mu);
    }
    return r;
}

extern __typeof__(vkCreateGraphicsPipelines) __real_vkCreateGraphicsPipelines;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateGraphicsPipelines(
    VkDevice d, VkPipelineCache cache, uint32_t n,
    const VkGraphicsPipelineCreateInfo *p, const VkAllocationCallbacks *a, VkPipeline *out) {
    VkResult r = __real_vkCreateGraphicsPipelines(d, cache, n, p, a, out);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&obs_mu);
        for (uint32_t j = 0; j < n; j++) if (out[j]) {
            uint32_t i = 0;
            while (i < obs_npipe && obs_pipes[i].pipe != out[j]) i++;
            if (i == obs_npipe) { REQUIRE(obs_npipe < 128); obs_npipe++; }
            obs_pipes[i].pipe = out[j];
            obs_pipes[i].ncol = p[j].pColorBlendState ? p[j].pColorBlendState->attachmentCount : 0;
            REQUIRE(obs_pipes[i].ncol <= 8);
            for (uint32_t k = 0; k < obs_pipes[i].ncol; k++)
                obs_pipes[i].mask[k] = p[j].pColorBlendState->pAttachments[k].colorWriteMask;
            memset(&obs_pipes[i].ds, 0, sizeof obs_pipes[i].ds);
            if (p[j].pDepthStencilState) obs_pipes[i].ds = *p[j].pDepthStencilState;
            obs_pipes[i].dynamic_ds = 0;
            const VkPipelineDynamicStateCreateInfo *dyn = p[j].pDynamicState;
            if (dyn) {
                for (uint32_t k = 0; k < dyn->dynamicStateCount; k++) {
                    switch (dyn->pDynamicStates[k]) {
                    case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT: obs_pipes[i].dynamic_ds |= OBS_DEPTH_TEST; break;
                    case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT: obs_pipes[i].dynamic_ds |= OBS_DEPTH_WRITE; break;
                    case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT: obs_pipes[i].dynamic_ds |= OBS_STENCIL_TEST; break;
                    case VK_DYNAMIC_STATE_STENCIL_OP_EXT: obs_pipes[i].dynamic_ds |= OBS_STENCIL_OP; break;
                    default: break;
                    }
                }
            }
        }
        pthread_mutex_unlock(&obs_mu);
    }
    return r;
}

extern __typeof__(vkCmdBeginRenderPass) __real_vkCmdBeginRenderPass;
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdBeginRenderPass(
    VkCommandBuffer cb, const VkRenderPassBeginInfo *p, VkSubpassContents contents) {
    pthread_mutex_lock(&obs_mu);
    obs_active_rp = p->renderPass;
    uint32_t k = 0;
    while (k < obs_nrp && obs_rps[k].pass != p->renderPass) k++;
    REQUIRE(k < obs_nrp);
    if (obs_nbegin < 64) {
        struct obs_begin_rec *rec = &obs_begins[obs_nbegin++];
        rec->area = p->renderArea;
        rec->n = obs_rps[k].n;
        rec->ncol = obs_rps[k].ncol;
        memcpy(rec->cols, obs_rps[k].cols, sizeof rec->cols);
        rec->ds = obs_rps[k].ds;
        memcpy(rec->a, obs_rps[k].a, sizeof rec->a);
    }
    pthread_mutex_unlock(&obs_mu);
    __real_vkCmdBeginRenderPass(cb, p, contents);
}

extern __typeof__(vkCmdBindPipeline) __real_vkCmdBindPipeline;
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdBindPipeline(
    VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        pthread_mutex_lock(&obs_mu);
        uint32_t i = 0;
        while (i < obs_npipe && obs_pipes[i].pipe != p) i++;
        REQUIRE(i < obs_npipe);
        obs_bound_pipe = i;
        pthread_mutex_unlock(&obs_mu);
    }
    __real_vkCmdBindPipeline(cb, bp, p);
}

extern __typeof__(vkCmdSetStencilWriteMask) __real_vkCmdSetStencilWriteMask;
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdSetStencilWriteMask(
    VkCommandBuffer cb, VkStencilFaceFlags face, uint32_t mask) {
    pthread_mutex_lock(&obs_mu);
    if (face & VK_STENCIL_FACE_FRONT_BIT) obs_stencil_wmask[0] = mask;
    if (face & VK_STENCIL_FACE_BACK_BIT) obs_stencil_wmask[1] = mask;
    pthread_mutex_unlock(&obs_mu);
    __real_vkCmdSetStencilWriteMask(cb, face, mask);
}

// 用驱动收到的命令求有效深度模板状态。EDS 管线描述中的静态占位值不生效。
#define TRACK_DS_ENABLE(fn, field) \
    static PFN_##fn real_##fn; \
    static VKAPI_ATTR void VKAPI_CALL obs_##fn(VkCommandBuffer cb, VkBool32 on) { \
        pthread_mutex_lock(&obs_mu); \
        obs_dynamic_ds.field = on; \
        pthread_mutex_unlock(&obs_mu); \
        real_##fn(cb, on); \
    }
TRACK_DS_ENABLE(vkCmdSetDepthTestEnableEXT, depthTestEnable)
TRACK_DS_ENABLE(vkCmdSetDepthWriteEnableEXT, depthWriteEnable)
TRACK_DS_ENABLE(vkCmdSetStencilTestEnableEXT, stencilTestEnable)
#undef TRACK_DS_ENABLE

static PFN_vkCmdSetStencilOpEXT real_vkCmdSetStencilOpEXT;
static VKAPI_ATTR void VKAPI_CALL obs_vkCmdSetStencilOpEXT(VkCommandBuffer cb, VkStencilFaceFlags faces,
    VkStencilOp fail, VkStencilOp pass, VkStencilOp depth_fail, VkCompareOp compare) {
    const VkStencilOpState state = {.failOp = fail, .passOp = pass, .depthFailOp = depth_fail, .compareOp = compare};
    pthread_mutex_lock(&obs_mu);
    if (faces & VK_STENCIL_FACE_FRONT_BIT) obs_dynamic_ds.front = state;
    if (faces & VK_STENCIL_FACE_BACK_BIT) obs_dynamic_ds.back = state;
    pthread_mutex_unlock(&obs_mu);
    real_vkCmdSetStencilOpEXT(cb, faces, fail, pass, depth_fail, compare);
}

extern __typeof__(vkCmdDraw) __real_vkCmdDraw;
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdDraw(
    VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    pthread_mutex_lock(&obs_mu);
    uint32_t p = 0;
    while (p < obs_nrp && obs_rps[p].pass != obs_active_rp) p++;
    REQUIRE(p < obs_nrp && obs_bound_pipe < obs_npipe);
    const struct obs_pipe *pi = &obs_pipes[obs_bound_pipe];
    for (uint32_t i = 0; i < obs_rps[p].ncol; i++) {
        uint32_t a = obs_rps[p].cols[i];
        if (a == VK_ATTACHMENT_UNUSED) continue;
        if (i < pi->ncol && pi->mask[i]) {
            if (obs_rps[p].a[a].storeOp == VK_ATTACHMENT_STORE_OP_NONE_EXT) obs_unsafe_write++;
            if (obs_rps[p].a[a].loadOp == VK_ATTACHMENT_LOAD_OP_NONE_EXT) obs_unsafe_read++;
        }
    }
    if (obs_rps[p].ds != VK_ATTACHMENT_UNUSED) {
        const VkAttachmentDescription *da = &obs_rps[p].a[obs_rps[p].ds];
        const VkBool32 depth_test = pi->dynamic_ds & OBS_DEPTH_TEST ? obs_dynamic_ds.depthTestEnable : pi->ds.depthTestEnable;
        const VkBool32 depth_write = pi->dynamic_ds & OBS_DEPTH_WRITE ? obs_dynamic_ds.depthWriteEnable : pi->ds.depthWriteEnable;
        const VkBool32 stencil_test = pi->dynamic_ds & OBS_STENCIL_TEST ? obs_dynamic_ds.stencilTestEnable : pi->ds.stencilTestEnable;
        if (depth_test) {
            if (da->loadOp == VK_ATTACHMENT_LOAD_OP_NONE_EXT) obs_unsafe_read++;
            if (depth_write && da->storeOp == VK_ATTACHMENT_STORE_OP_NONE_EXT) obs_unsafe_write++;
        }
        if (stencil_test) {
            if (da->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_NONE_EXT) obs_unsafe_read++;
            const VkPipelineDepthStencilStateCreateInfo *ds = pi->dynamic_ds & OBS_STENCIL_OP ? &obs_dynamic_ds : &pi->ds;
            VkStencilOpState st[2] = {ds->front, ds->back};
            for (int f = 0; f < 2; f++) {
                if ((obs_stencil_wmask[f] & 0xffu) &&
                    (st[f].failOp != VK_STENCIL_OP_KEEP || st[f].passOp != VK_STENCIL_OP_KEEP ||
                     st[f].depthFailOp != VK_STENCIL_OP_KEEP)) {
                    if (da->stencilStoreOp == VK_ATTACHMENT_STORE_OP_NONE_EXT) obs_unsafe_write++;
                }
            }
        }
    }
    pthread_mutex_unlock(&obs_mu);
    __real_vkCmdDraw(cb, vc, ic, fv, fi);
}

extern __typeof__(vkGetDeviceProcAddr) __real_vkGetDeviceProcAddr;
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice d, const char *name) {
    if (!strcmp(name, "vkCmdBeginRenderPass")) return (PFN_vkVoidFunction)__wrap_vkCmdBeginRenderPass;
    if (!strcmp(name, "vkCmdBindPipeline")) return (PFN_vkVoidFunction)__wrap_vkCmdBindPipeline;
    if (!strcmp(name, "vkCmdSetStencilWriteMask")) return (PFN_vkVoidFunction)__wrap_vkCmdSetStencilWriteMask;
    if (!strcmp(name, "vkCmdDraw")) return (PFN_vkVoidFunction)__wrap_vkCmdDraw;
    PFN_vkVoidFunction real = __real_vkGetDeviceProcAddr(d, name);
#define OBSERVE_DS_PROC(fn) \
    if (!strcmp(name, #fn) && real) { real_##fn = (PFN_##fn)real; return (PFN_vkVoidFunction)obs_##fn; }
    OBSERVE_DS_PROC(vkCmdSetDepthTestEnableEXT)
    OBSERVE_DS_PROC(vkCmdSetDepthWriteEnableEXT)
    OBSERVE_DS_PROC(vkCmdSetStencilTestEnableEXT)
    OBSERVE_DS_PROC(vkCmdSetStencilOpEXT)
#undef OBSERVE_DS_PROC
    return real;
}

_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateDevice), PFN_vkCreateDevice), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkEnumerateDeviceExtensionProperties), PFN_vkEnumerateDeviceExtensionProperties), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateRenderPass), PFN_vkCreateRenderPass), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateGraphicsPipelines), PFN_vkCreateGraphicsPipelines), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCmdBeginRenderPass), PFN_vkCmdBeginRenderPass), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCmdBindPipeline), PFN_vkCmdBindPipeline), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCmdSetStencilWriteMask), PFN_vkCmdSetStencilWriteMask), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCmdDraw), PFN_vkCmdDraw), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkGetDeviceProcAddr), PFN_vkGetDeviceProcAddr), "sig");

const char *const *nvn_plat_vk_instance_exts(uint32_t *n) { *n = 0; return NULL; }
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) {
    (void)i; (void)w; *s = VK_NULL_HANDLE; return VK_ERROR_EXTENSION_NOT_PRESENT;
}
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) { (void)w; (void)x; (void)y; return 0; }
void nvn_plat_safepoint(void) {}
void nvn_plat_log(int l, const char *c, const char *s) { (void)l; fprintf(stderr, "[%s] %s\n", c ? c : "", s ? s : ""); }
int nvn_plat_flag(const char *n, int d) {
    if (!strcmp(n, "nvk_dq")) return dq_mode;
    if (!strcmp(n, "nvk_opt_layout")) return opt_mode;
    if (!strcmp(n, "nvk_load_store_op_none")) return ls_none_mode;
    if (!strcmp(n, "nvk_pass_cache")) return 1;
    return d;
}
static void done(void *u, const nvn_gpu_done *d) { (void)u; REQUIRE(d->status == NVN_GPU_DONE_OK); }

static nvn_gpu_shader load_sh(const char *dir, const char *name, nvn_gpu_stage stage, const nvn_gpu_layout *layout) {
    char path[1024];
    REQUIRE(snprintf(path, sizeof path, "%s/%s.spv", dir, name) < (int)sizeof path);
    FILE *f = fopen(path, "rb"); REQUIRE(f);
    REQUIRE(!fseek(f, 0, SEEK_END));
    long n = ftell(f); REQUIRE(n > 0);
    rewind(f);
    void *data = malloc((size_t)n); REQUIRE(data);
    REQUIRE(fread(data, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    nvn_gpu_shader_src s = {0};
    s.stage = stage; s.kind = NVN_GPU_BLOB_SPIRV; s.entry = "main";
    s.blob = data; s.bytes = (size_t)n; s.layout = layout;
    nvn_gpu_shader h = NULL;
    GPU(nvn_gpu_shader_load(&s, &h, &err));
    free(data);
    return h;
}

static nvn_gpu_dyn make_dyn(uint32_t st_wmask, uint32_t st_ref) {
    nvn_gpu_dyn dyn = {0};
    dyn.nvp = 1; dyn.vp[0][2] = dyn.vp[0][3] = 4.0f; dyn.vp[0][5] = 1.0f;
    dyn.has_sc = 1; dyn.sc[2] = dyn.sc[3] = 4;
    dyn.st_vmask[0] = dyn.st_vmask[1] = 0xff;
    dyn.st_wmask[0] = dyn.st_wmask[1] = st_wmask;
    dyn.st_ref[0] = dyn.st_ref[1] = st_ref;
    return dyn;
}

static nvn_gpu_pipe make_pipe(
    nvn_gpu_shader vs, nvn_gpu_shader fs, int mrt2, uint32_t chmask,
    uint32_t ds_fmt, int dt, int dw, uint32_t df,
    int st, uint32_t sf, uint32_t sfail, uint32_t szfail, uint32_t szpass) {
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3; // COPY
    pd.prim = 4; // TRIANGLES
    pd.samples = 1;
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    if (mrt2) pd.cfmt[1] = NVN_FORMAT_RGBA8;
    pd.chmask.mask = chmask;
    if (ds_fmt) {
        pd.dfmt = ds_fmt;
        pd.ds.dt = (uint8_t)dt;
        pd.ds.dw = (uint8_t)dw;
        pd.ds.df = (uint8_t)df;
        if (ds_fmt == NVN_FORMAT_DEPTH32F_STENCIL8) {
            pd.sfmt = ds_fmt;
            pd.ds.st = (uint8_t)st;
            pd.ds.sf_f = pd.ds.sf_b = (uint8_t)sf;
            pd.ds.fail_f = pd.ds.fail_b = (uint8_t)sfail;
            pd.ds.zfail_f = pd.ds.zfail_b = (uint8_t)szfail;
            pd.ds.zpass_f = pd.ds.zpass_b = (uint8_t)szpass;
        }
    }
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));
    return pipe;
}

static void draw_z(nvn_gpu_cmd cmd, float z, float r, float g, float b, float a) {
    float vz = z;
    float fc[4] = {r, g, b, a};
    nvn_gpu_push(cmd, NVN_GPU_SH_VS, 0, &vz, sizeof vz);
    nvn_gpu_push(cmd, NVN_GPU_SH_FS, 16, fc, sizeof fc);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(cmd, &da);
}

static unsigned check_split_px(const unsigned char *px, unsigned char lr, unsigned char lg,
                               unsigned char rr, unsigned char rg) {
    unsigned bad = 0;
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            const unsigned char *p = px + (y * 4 + x) * 4;
            unsigned char er = x < 2 ? lr : rr;
            unsigned char eg = x < 2 ? lg : rg;
            if (p[0] != er || p[1] != eg || p[2] != 0 || p[3] != 255) bad++;
        }
    }
    return bad;
}

static unsigned check_solid_px(const unsigned char *px, unsigned char er, unsigned char eg, unsigned char eb) {
    unsigned bad = 0;
    for (unsigned i = 0; i < 16; i++) {
        const unsigned char *p = px + i * 4;
        if (p[0] != er || p[1] != eg || p[2] != eb || p[3] != 255) bad++;
    }
    return bad;
}

struct ls_case {
    const char *name;
    int id;
};

static const struct ls_case k_cases[] = {
    {"depth-readonly", 0},
    {"depth-idle", 1},
    {"depth-stencil-split-aspect", 2},
    {"stencil-readonly-keep", 3},
    {"stencil-write-not-none", 4},
    {"depth-clear-not-none", 5},
    {"promote-readonly-to-write", 6},
    {"promote-idle-to-read", 7},
    {"color-zero-mask-and-promote", 8},
    {"clear-only-pass-idle-depth", 9},
};

static void run_case(struct s2a_vk_device *device, const char *dir, const struct ls_case *tc) {
    obs_reset();
    ncases++;
    const int active = ls_none_mode && !hide_extension;
    const VkAttachmentLoadOp exp_load_none = active ? VK_ATTACHMENT_LOAD_OP_NONE_EXT : VK_ATTACHMENT_LOAD_OP_LOAD;
    const VkAttachmentStoreOp exp_store_none = active ? VK_ATTACHMENT_STORE_OP_NONE_EXT : VK_ATTACHMENT_STORE_OP_STORE;

    nvn_gpu_open_desc od = {0}; od.host_device = device; od.app_label = "load-store-op-none";
    GPU(nvn_gpu_open(&od, &err));

    nvn_gpu_layout vs_lay = {0}; vs_lay.push_off = 0; vs_lay.push_bytes = 4;
    nvn_gpu_layout fs_lay = {0}; fs_lay.push_off = 16; fs_lay.push_bytes = 16;
    nvn_gpu_shader vs = load_sh(dir, "pos_z.vert", NVN_GPU_SH_VS, &vs_lay);
    nvn_gpu_shader fs_solid = load_sh(dir, "solid_pc.frag", NVN_GPU_SH_FS, &fs_lay);
    nvn_gpu_shader fs_split_d = load_sh(dir, "split_depth.frag", NVN_GPU_SH_FS, NULL);
    nvn_gpu_shader fs_mrt2 = load_sh(dir, "solid_mrt2.frag", NVN_GPU_SH_FS, &fs_lay);

    const int use_s8 = (tc->id == 2 || tc->id == 3 || tc->id == 4);
    const int use_mrt2 = (tc->id == 8);
    const uint32_t ds_fmt = use_s8 ? NVN_FORMAT_DEPTH32F_STENCIL8 : NVN_FORMAT_DEPTH32F;

    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.width = td.d.height = 4; td.d.depth = td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;

    nvn_gpu_tex c0 = NULL, c1 = NULL, ds = NULL;
    nvn_gpu_view vc0 = NULL, vc1 = NULL, vds = NULL;
    td.d.format = NVN_FORMAT_RGBA8;
    GPU(nvn_gpu_tex_create(&td, &c0, &err)); GPU(nvn_gpu_view_create(c0, NULL, &vc0, &err));
    if (use_mrt2) {
        GPU(nvn_gpu_tex_create(&td, &c1, &err)); GPU(nvn_gpu_view_create(c1, NULL, &vc1, &err));
    }
    td.d.format = ds_fmt;
    GPU(nvn_gpu_tex_create(&td, &ds, &err)); GPU(nvn_gpu_view_create(ds, NULL, &vds, &err));

    unsigned char sentinel[192]; memset(sentinel, 0xa5, sizeof sentinel);
    nvn_gpu_buf rb = NULL;
    GPU(nvn_gpu_buf_create(sizeof sentinel, NVN_GPU_MEM_SHARED, sentinel, &rb, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));

    nvn_gpu_pipe p_init_d = make_pipe(vs, fs_split_d, 0, 0xf, ds_fmt, 1, 1, 8, 0, 8, 1, 1, 1);
    nvn_gpu_pipe p_ro_less = make_pipe(vs, fs_solid, 0, 0xf, ds_fmt, 1, 0, 2, 0, 8, 1, 1, 1);
    nvn_gpu_pipe p_rw_always = make_pipe(vs, fs_solid, 0, 0xf, ds_fmt, 1, 1, 8, 0, 8, 1, 1, 1);
    nvn_gpu_pipe p_idle_d = make_pipe(vs, fs_solid, 0, 0xf, ds_fmt, 0, 0, 8, 0, 8, 1, 1, 1);

    nvn_gpu_pass_desc p = {0};
    p.w = p.h = 4; p.layers = 1;
    p.ncolor = use_mrt2 ? 2 : 1;
    p.color[0].view = vc0; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1.0f;
    if (use_mrt2) {
        p.color[1].view = vc1; p.color[1].load = NVN_GPU_LOAD_CLEAR; p.color[1].store = NVN_GPU_STORE_STORE;
        p.color[1].clear.f[0] = 0.75f; p.color[1].clear.f[3] = 1.0f;
    }
    p.depth.view = vds; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE;
    p.clear_depth = 1.0f;
    if (use_s8) {
        p.stencil.view = vds; p.stencil.load = NVN_GPU_LOAD_CLEAR; p.stencil.store = NVN_GPU_STORE_STORE;
        p.clear_stencil = (tc->id == 2) ? 17 : (tc->id == 3) ? 42 : 10;
    }

    // Pass 0: initialize depth (left=0.25, right=0.75) and stencil (if S8) and color1 (if MRT2)
    nvn_gpu_pass_begin(cmd, &p);
    if (!use_mrt2) {
        nvn_gpu_dyn dyn = make_dyn(0, 0);
        nvn_gpu_set_pipe(cmd, p_init_d);
        nvn_gpu_set_dyn(cmd, &dyn);
        float vz = 0.5f; nvn_gpu_push(cmd, NVN_GPU_SH_VS, 0, &vz, sizeof vz);
        nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_draw(cmd, &da);
    }
    nvn_gpu_pass_end(cmd);

    unsigned op_bad = 0, px_bad = 0;

    if (tc->id == 0) {
        // depth-readonly: Pass 1 does 2 read-only depth-tested draws (z=0.5, LESS -> right half passes)
        p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].clear.f[0] = 0.0f; p.color[0].clear.f[1] = 0.0f;
        p.depth.load = NVN_GPU_LOAD_LOAD;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_dyn dyn = make_dyn(0, 0);
        nvn_gpu_set_pipe(cmd, p_ro_less); nvn_gpu_set_dyn(cmd, &dyn);
        draw_z(cmd, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f);
        draw_z(cmd, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, c0, 0, 0, NULL, rb, 0, 16, 64);

        // Pass 2: verify depth preserved after a read-only pass (read-only again)
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_set_pipe(cmd, p_ro_less); nvn_gpu_set_dyn(cmd, &dyn);
        draw_z(cmd, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, c0, 0, 0, NULL, rb, 64, 16, 64);

        // Pass 3: immediate depth CLEAR/write after a read-only pass (SyncVal WAR check)
        p.depth.load = NVN_GPU_LOAD_CLEAR; p.clear_depth = 0.1f;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_set_pipe(cmd, p_rw_always); nvn_gpu_set_dyn(cmd, &dyn);
        draw_z(cmd, 0.1f, 0.0f, 0.0f, 1.0f, 1.0f);
        nvn_gpu_pass_end(cmd);

        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        REQUIRE(obs_nbegin == 4);
        // Pass 1 & Pass 2 must have depth load=LOAD, store=STORE; color0 store=STORE
        for (unsigned k = 1; k <= 2; k++) {
            uint32_t ds_i = obs_begins[k].ds; REQUIRE(ds_i != VK_ATTACHMENT_UNUSED);
            if (obs_begins[k].a[ds_i].loadOp != VK_ATTACHMENT_LOAD_OP_LOAD) op_bad++;
            if (obs_begins[k].a[ds_i].storeOp != VK_ATTACHMENT_STORE_OP_STORE) op_bad++;
            if (obs_begins[k].a[0].storeOp != VK_ATTACHMENT_STORE_OP_STORE) op_bad++;
        }
        const unsigned char *px = nvn_gpu_buf_cpu(rb);
        px_bad += check_split_px(px, 0, 0, 0, 255);
        px_bad += check_split_px(px + 64, 0, 0, 255, 0);
    } else if (tc->id == 1) {
        // depth-idle: Pass 1 binds depth LOAD/STORE but draws with dt=0, dw=0 -> do not infer load/store from this draw
        p.depth.load = NVN_GPU_LOAD_LOAD;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_dyn dyn = make_dyn(0, 0);
        nvn_gpu_set_pipe(cmd, p_idle_d); nvn_gpu_set_dyn(cmd, &dyn);
        draw_z(cmd, 0.9f, 0.0f, 1.0f, 0.0f, 1.0f);
        nvn_gpu_pass_end(cmd);

        // Pass 2: depth test z=0.5 LESS must still see Pass 0's left=0.25 / right=0.75!
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_set_pipe(cmd, p_ro_less); nvn_gpu_set_dyn(cmd, &dyn);
        draw_z(cmd, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, c0, 0, 0, NULL, rb, 0, 16, 64);

        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        REQUIRE(obs_nbegin == 3);
        uint32_t ds_i = obs_begins[1].ds; REQUIRE(ds_i != VK_ATTACHMENT_UNUSED);
        if (obs_begins[1].a[ds_i].loadOp != VK_ATTACHMENT_LOAD_OP_LOAD) op_bad++;
        if (obs_begins[1].a[ds_i].storeOp != VK_ATTACHMENT_STORE_OP_STORE) op_bad++;
        px_bad += check_split_px(nvn_gpu_buf_cpu(rb), 0, 0, 255, 0);
    } else if (tc->id == 2) {
        // depth-stencil-split-aspect: Pass 1 writes depth (dt=1, dw=1), stencil off (st=0)
        p.depth.load = NVN_GPU_LOAD_LOAD;
        p.stencil.load = NVN_GPU_LOAD_LOAD;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_dyn dyn = make_dyn(0, 0);
        nvn_gpu_set_pipe(cmd, p_rw_always); nvn_gpu_set_dyn(cmd, &dyn);
        draw_z(cmd, 0.9f, 0.0f, 1.0f, 0.0f, 1.0f);
        nvn_gpu_pass_end(cmd);

        // Pass 2: test stencil == 17 (wmask=0, dt=0)
        nvn_gpu_pipe p_st_eq = make_pipe(vs, fs_solid, 0, 0xf, ds_fmt, 0, 0, 8, 1, 3, 1, 1, 1);
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_dyn dyn_st = make_dyn(0, 17);
        nvn_gpu_set_pipe(cmd, p_st_eq); nvn_gpu_set_dyn(cmd, &dyn_st);
        draw_z(cmd, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, c0, 0, 0, NULL, rb, 0, 16, 64);

        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        REQUIRE(obs_nbegin == 3);
        uint32_t ds1 = obs_begins[1].ds, ds2 = obs_begins[2].ds;
        if (obs_begins[1].a[ds1].storeOp != VK_ATTACHMENT_STORE_OP_STORE) op_bad++;
        if (obs_begins[1].a[ds1].stencilLoadOp != VK_ATTACHMENT_LOAD_OP_LOAD) op_bad++;
        if (obs_begins[1].a[ds1].stencilStoreOp != VK_ATTACHMENT_STORE_OP_STORE) op_bad++;
        if (obs_begins[2].a[ds2].loadOp != VK_ATTACHMENT_LOAD_OP_LOAD) op_bad++;
        if (obs_begins[2].a[ds2].storeOp != VK_ATTACHMENT_STORE_OP_STORE) op_bad++;
        if (obs_begins[2].a[ds2].stencilLoadOp != VK_ATTACHMENT_LOAD_OP_LOAD) op_bad++;
        if (obs_begins[2].a[ds2].stencilStoreOp != VK_ATTACHMENT_STORE_OP_STORE) op_bad++;
        px_bad += check_solid_px(nvn_gpu_buf_cpu(rb), 255, 0, 0);
    } else if (tc->id == 3) {
        // stencil-readonly-keep: st=1, wmask=0xff, all ops=KEEP, dt=1, dw=0
        nvn_gpu_pipe p_keep = make_pipe(vs, fs_solid, 0, 0xf, ds_fmt, 1, 0, 2, 1, 3, 1, 1, 1);
        p.depth.load = NVN_GPU_LOAD_LOAD; p.stencil.load = NVN_GPU_LOAD_LOAD;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_dyn dyn = make_dyn(0xff, 42);
        nvn_gpu_set_pipe(cmd, p_keep); nvn_gpu_set_dyn(cmd, &dyn);
        draw_z(cmd, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, c0, 0, 0, NULL, rb, 0, 16, 64);

        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        REQUIRE(obs_nbegin == 2);
        uint32_t ds1 = obs_begins[1].ds;
        if (obs_begins[1].a[ds1].loadOp != VK_ATTACHMENT_LOAD_OP_LOAD) op_bad++;
        if (obs_begins[1].a[ds1].storeOp != VK_ATTACHMENT_STORE_OP_STORE) op_bad++;
        if (obs_begins[1].a[ds1].stencilLoadOp != VK_ATTACHMENT_LOAD_OP_LOAD) op_bad++;
        if (obs_begins[1].a[ds1].stencilStoreOp != VK_ATTACHMENT_STORE_OP_STORE) op_bad++;
        px_bad += check_split_px(nvn_gpu_buf_cpu(rb), 0, 0, 255, 0);
    } else if (tc->id == 4) {
        // stencil-write-not-none: st=1, wmask=0xff, zpass=REPLACE(ref=99), dt=0 -> stencil STORE
        nvn_gpu_pipe p_st_wr = make_pipe(vs, fs_solid, 0, 0xf, ds_fmt, 0, 0, 8, 1, 8, 1, 1, 3);
        nvn_gpu_pipe p_st_eq = make_pipe(vs, fs_solid, 0, 0xf, ds_fmt, 0, 0, 8, 1, 3, 1, 1, 1);
        p.depth.load = NVN_GPU_LOAD_LOAD; p.stencil.load = NVN_GPU_LOAD_LOAD;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_dyn dyn_wr = make_dyn(0xff, 99);
        nvn_gpu_set_pipe(cmd, p_st_wr); nvn_gpu_set_dyn(cmd, &dyn_wr);
        draw_z(cmd, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f);
        nvn_gpu_pass_end(cmd);

        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_dyn dyn_rd = make_dyn(0, 99);
        nvn_gpu_set_pipe(cmd, p_st_eq); nvn_gpu_set_dyn(cmd, &dyn_rd);
        draw_z(cmd, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, c0, 0, 0, NULL, rb, 0, 16, 64);

        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        REQUIRE(obs_nbegin == 3);
        uint32_t ds1 = obs_begins[1].ds;
        if (obs_begins[1].a[ds1].loadOp != VK_ATTACHMENT_LOAD_OP_LOAD) op_bad++;
        if (obs_begins[1].a[ds1].storeOp != VK_ATTACHMENT_STORE_OP_STORE) op_bad++;
        if (obs_begins[1].a[ds1].stencilStoreOp != VK_ATTACHMENT_STORE_OP_STORE) op_bad++;
        px_bad += check_solid_px(nvn_gpu_buf_cpu(rb), 255, 0, 0);
    } else if (tc->id == 5) {
        // depth-clear-not-none: depth.load = CLEAR (0.8f), draw has dt=1, dw=0 -> depth store MUST stay STORE!
        p.depth.load = NVN_GPU_LOAD_CLEAR; p.clear_depth = 0.8f;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_dyn dyn = make_dyn(0, 0);
        nvn_gpu_set_pipe(cmd, p_ro_less); nvn_gpu_set_dyn(cmd, &dyn);
        draw_z(cmd, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f);
        nvn_gpu_pass_end(cmd);

        // Pass 2: test depth against 0.5 LESS -> ALL 16 pixels must pass (since clear=0.8 was stored!)
        p.depth.load = NVN_GPU_LOAD_LOAD;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_set_pipe(cmd, p_ro_less); nvn_gpu_set_dyn(cmd, &dyn);
        draw_z(cmd, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, c0, 0, 0, NULL, rb, 0, 16, 64);

        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        REQUIRE(obs_nbegin == 3);
        uint32_t ds1 = obs_begins[1].ds;
        if (obs_begins[1].a[ds1].loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR) op_bad++;
        if (obs_begins[1].a[ds1].storeOp != VK_ATTACHMENT_STORE_OP_STORE) op_bad++;
        px_bad += check_solid_px(nvn_gpu_buf_cpu(rb), 255, 0, 0);
    } else if (tc->id == 6) {
        // promote-readonly-to-write: Draw 1 read-only (dw=0) -> Draw 2 writes depth=0.1 (dw=1) in SAME pass!
        p.depth.load = NVN_GPU_LOAD_LOAD;
        p.color[0].load = NVN_GPU_LOAD_LOAD;
        p.area_w = 2; p.area_h = 4;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_dyn dyn = make_dyn(0, 0);
        dyn.sc[2] = 2;
        nvn_gpu_set_pipe(cmd, p_ro_less); nvn_gpu_set_dyn(cmd, &dyn);
        draw_z(cmd, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f);
        nvn_gpu_set_pipe(cmd, p_rw_always);
        draw_z(cmd, 0.1f, 0.0f, 0.0f, 1.0f, 1.0f);
        nvn_gpu_pass_end(cmd);

        // The first pass changes only the left half. A depth test must see the new depth there and the original on the right.
        p.color[0].load = NVN_GPU_LOAD_CLEAR;
        p.area_w = p.area_h = 0;
        dyn.sc[2] = 4;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_set_pipe(cmd, p_ro_less); nvn_gpu_set_dyn(cmd, &dyn);
        draw_z(cmd, 0.2f, 1.0f, 0.0f, 0.0f, 1.0f);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, c0, 0, 0, NULL, rb, 0, 16, 64);

        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        if (obs_nbegin != 3) op_bad++;
        if (obs_begins[1].a[obs_begins[1].ds].storeOp != VK_ATTACHMENT_STORE_OP_STORE) op_bad++;
        if (obs_begins[1].area.extent.width != 2 || obs_begins[1].area.extent.height != 4) op_bad++;
        px_bad += check_split_px(nvn_gpu_buf_cpu(rb), 0, 0, 255, 0);
    } else if (tc->id == 7) {
        // promote-idle-to-read: Draw 1 idle (dt=0, dw=0) -> Draw 2 reads depth (dt=1, dw=0, LESS z=0.5) in SAME pass!
        p.depth.load = NVN_GPU_LOAD_LOAD;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_dyn dyn = make_dyn(0, 0);
        nvn_gpu_set_pipe(cmd, p_idle_d); nvn_gpu_set_dyn(cmd, &dyn);
        draw_z(cmd, 0.9f, 0.0f, 1.0f, 0.0f, 1.0f);
        nvn_gpu_set_pipe(cmd, p_ro_less);
        draw_z(cmd, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, c0, 0, 0, NULL, rb, 0, 16, 64);

        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        if (obs_nbegin != 2u) op_bad++;
        uint32_t ds1 = obs_begins[1].ds;
        if (obs_begins[1].a[ds1].loadOp != VK_ATTACHMENT_LOAD_OP_LOAD) op_bad++;
        if (obs_begins[1].a[ds1].storeOp != VK_ATTACHMENT_STORE_OP_STORE) op_bad++;
        // Left half fails depth test (keeps green from Draw 1); right half passes (gets red from Draw 2)!
        px_bad += check_split_px(nvn_gpu_buf_cpu(rb), 0, 255, 255, 0);
    } else if (tc->id == 8) {
        // color-zero-mask-and-promote: Pass 1 writes color0 only (color1 chmask=0);
        // Pass 2 starts with color1 chmask=0 then clear_rect on color1 -> promotes color1 to STORE!
        nvn_gpu_pipe p_c0_only = make_pipe(vs, fs_mrt2, 1, 0x0f, ds_fmt, 0, 0, 8, 0, 8, 1, 1, 1);
        p.color[0].load = NVN_GPU_LOAD_LOAD;
        p.color[1].load = NVN_GPU_LOAD_LOAD;
        p.depth.load = NVN_GPU_LOAD_LOAD;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_dyn dyn = make_dyn(0, 0);
        nvn_gpu_set_pipe(cmd, p_c0_only); nvn_gpu_set_dyn(cmd, &dyn);
        draw_z(cmd, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, c1, 0, 0, NULL, rb, 0, 16, 64);

        // Pass 2: Draw 1 leaves color1 untouched, then clear_rect clears color1 to green without a restart.
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_set_pipe(cmd, p_c0_only); nvn_gpu_set_dyn(cmd, &dyn);
        draw_z(cmd, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f);
        nvn_gpu_clear cl = {0}; cl.color = 1; cl.color_mask = 0xf; cl.w = cl.h = 4;
        cl.value.f[1] = 1.0f; cl.value.f[3] = 1.0f;
        nvn_gpu_clear_rect(cmd, &cl);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, c1, 0, 0, NULL, rb, 64, 16, 64);

        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        if (obs_nbegin != 3u) op_bad++;
        for (unsigned k = 1; k <= 2; k++) {
            if (obs_begins[k].a[1].loadOp != VK_ATTACHMENT_LOAD_OP_LOAD) op_bad++;
            if (obs_begins[k].a[1].storeOp != VK_ATTACHMENT_STORE_OP_STORE) op_bad++;
        }
        const unsigned char *px = nvn_gpu_buf_cpu(rb);
        px_bad += check_solid_px(px, 191, 0, 0);      // 0.75 * 255 = 191.25 -> 191
        px_bad += check_solid_px(px + 64, 0, 255, 0); // cleared by clear_rect
    } else if (tc->id == 9) {
        // clear-only-pass-idle-depth: Pass 1 clears color0 to (0.5, 0, 0, 1), depth LOAD, 0 draws!
        p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].clear.f[0] = 0.5f;
        p.depth.load = NVN_GPU_LOAD_LOAD;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, c0, 0, 0, NULL, rb, 0, 16, 64);

        // Pass 2: verify depth 0.25/0.75 preserved!
        p.color[0].clear.f[0] = 0.0f;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_dyn dyn = make_dyn(0, 0);
        nvn_gpu_set_pipe(cmd, p_ro_less); nvn_gpu_set_dyn(cmd, &dyn);
        draw_z(cmd, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, c0, 0, 0, NULL, rb, 64, 16, 64);

        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        REQUIRE(obs_nbegin == 3);
        uint32_t ds1 = obs_begins[1].ds;
        if (obs_begins[1].a[0].loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR) op_bad++;
        if (obs_begins[1].a[0].storeOp != VK_ATTACHMENT_STORE_OP_STORE) op_bad++;
        if (obs_begins[1].a[ds1].loadOp != exp_load_none) op_bad++;
        if (obs_begins[1].a[ds1].storeOp != exp_store_none) op_bad++;
        const unsigned char *px = nvn_gpu_buf_cpu(rb);
        px_bad += check_solid_px(px, 128, 0, 0);
        px_bad += check_split_px(px + 64, 0, 0, 255, 0);
    }

    const unsigned bad = op_bad || px_bad || obs_unsafe_write || obs_unsafe_read;
    printf("ls-none case=%s active=%d begins=%u op_bad=%u px_bad=%u unsafe_w=%u unsafe_r=%u bad=%u\n",
           tc->name, active, obs_nbegin, op_bad, px_bad, obs_unsafe_write, obs_unsafe_read, bad);
    failures += !!bad;

    nvn_gpu_view_release(vc0); nvn_gpu_tex_release(c0);
    if (vc1) nvn_gpu_view_release(vc1);
    if (c1) nvn_gpu_tex_release(c1);
    nvn_gpu_view_release(vds); nvn_gpu_tex_release(ds);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs_solid);
    nvn_gpu_shader_release(fs_split_d); nvn_gpu_shader_release(fs_mrt2);
    GPU(nvn_gpu_close(5000000000ull, &err));
}

int main(int argc, char **argv) {
    if (argc != 6) return 2;
    dq_mode = atoi(argv[2]);
    opt_mode = atoi(argv[3]);
    ls_none_mode = atoi(argv[4]);
    hide_extension = atoi(argv[5]);

    struct s2a_vk_device device;
    char why[256] = {0};
    REQUIRE(!s2a_vk_device_create(&device, why, sizeof why));
    REQUIRE(enabled_none == !hide_extension);
    REQUIRE(device.load_store_op_none_enabled == !hide_extension);

    uint32_t n = 0;
    REQUIRE(__real_vkEnumerateDeviceExtensionProperties(device.physical, NULL, &n, NULL) == VK_SUCCESS);
    VkExtensionProperties *ext = calloc(n ? n : 1, sizeof *ext); REQUIRE(ext);
    REQUIRE(__real_vkEnumerateDeviceExtensionProperties(device.physical, NULL, &n, ext) == VK_SUCCESS);
    int advertised = 0;
    for (uint32_t i = 0; i < n; i++)
        advertised |= !strcmp(ext[i].extensionName, VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME);
    free(ext);
    REQUIRE(advertised);

    for (unsigned i = 0; i < sizeof k_cases / sizeof *k_cases; i++)
        run_case(&device, argv[1], &k_cases[i]);

    REQUIRE(!s2a_vk_device_destroy(&device, 1));
    printf("load-store-op-none cases=%u failures=%u dq=%d opt=%d ls_none=%d hidden=%d ext-enabled=%d\n",
           ncases, failures, dq_mode, opt_mode, ls_none_mode, hide_extension, enabled_none);
    return failures ? 1 : 0;
}
