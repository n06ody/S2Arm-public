// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real Vulkan (lavapipe + VVL/SyncVal) pixel/buffer readback & call-count observer
// for VK_KHR_push_descriptor (debug.s2a.nvk_push_desc).
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nvn_gpu_err err;
static int dq_mode, opt_mode, push_mode, hide_extension, template_mode, dedup_mode;
static int memo_mode;
static unsigned failures, ncases;

#define REQUIRE(x) do { \
    if (!(x)) { \
        fprintf(stderr, "fixture failed line %d: %s rc=%d why=%s\n", __LINE__, #x, err.rc.code, err.why); \
        exit(2); \
    } \
} while (0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))

static pthread_mutex_t obs_mu = PTHREAD_MUTEX_INITIALIZER;
static int enabled_push_ext;
static PFN_vkCmdPushDescriptorSetKHR real_push_desc;
static PFN_vkUpdateDescriptorSetWithTemplate real_template_update;
static uint32_t obs_push_dsl, obs_push_calls, obs_push_writes, obs_push_sets;
static uint32_t obs_dynamic_offsets[3], obs_dynamic_binds;
static uint32_t obs_alloc_sets, obs_update_calls, obs_template_calls, obs_bind_calls;

static void obs_reset(void) {
    pthread_mutex_lock(&obs_mu);
    obs_push_dsl = 0;
    obs_push_calls = 0;
    obs_push_writes = 0;
    obs_push_sets = 0;
    obs_alloc_sets = 0;
    obs_update_calls = 0;
    obs_template_calls = 0;
    obs_bind_calls = 0;
    obs_dynamic_binds = 0;
    pthread_mutex_unlock(&obs_mu);
}

extern __typeof__(vkCreateDevice) __real_vkCreateDevice;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(
    VkPhysicalDevice p, const VkDeviceCreateInfo *ci,
    const VkAllocationCallbacks *a, VkDevice *d) {
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        if (!strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
            enabled_push_ext = 1;
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
    VkExtensionProperties *all = calloc(n ? n : 1, sizeof *all); REQUIRE(all);
    r = __real_vkEnumerateDeviceExtensionProperties(p, layer, &n, all);
    if (r != VK_SUCCESS) { free(all); return r; }
    uint32_t visible = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(all[i].extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
            all[visible++] = all[i];
    }
    if (!props) { *count = visible; free(all); return VK_SUCCESS; }
    uint32_t written = *count < visible ? *count : visible;
    memcpy(props, all, written * sizeof *all);
    *count = written;
    free(all);
    return written < visible ? VK_INCOMPLETE : VK_SUCCESS;
}

extern __typeof__(vkCreateDescriptorSetLayout) __real_vkCreateDescriptorSetLayout;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDescriptorSetLayout(
    VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci,
    const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    VkResult r = __real_vkCreateDescriptorSetLayout(d, ci, a, out);
    if (r == VK_SUCCESS && (ci->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR)) {
        pthread_mutex_lock(&obs_mu);
        obs_push_dsl++;
        pthread_mutex_unlock(&obs_mu);
    }
    return r;
}

extern __typeof__(vkAllocateDescriptorSets) __real_vkAllocateDescriptorSets;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkAllocateDescriptorSets(
    VkDevice d, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *sets) {
    VkResult r = __real_vkAllocateDescriptorSets(d, ai, sets);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&obs_mu);
        obs_alloc_sets += ai->descriptorSetCount;
        pthread_mutex_unlock(&obs_mu);
    }
    return r;
}

extern __typeof__(vkUpdateDescriptorSets) __real_vkUpdateDescriptorSets;
VKAPI_ATTR void VKAPI_CALL __wrap_vkUpdateDescriptorSets(
    VkDevice d, uint32_t nw, const VkWriteDescriptorSet *w,
    uint32_t nc, const VkCopyDescriptorSet *c) {
    pthread_mutex_lock(&obs_mu);
    obs_update_calls++;
    pthread_mutex_unlock(&obs_mu);
    __real_vkUpdateDescriptorSets(d, nw, w, nc, c);
}

extern __typeof__(vkCmdBindDescriptorSets) __real_vkCmdBindDescriptorSets;
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdBindDescriptorSets(
    VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l,
    uint32_t first, uint32_t n, const VkDescriptorSet *s, uint32_t nd, const uint32_t *offs) {
    pthread_mutex_lock(&obs_mu);
    obs_bind_calls++;
    if (nd == 1 && obs_dynamic_binds < 3)
        obs_dynamic_offsets[obs_dynamic_binds++] = offs[0];
    pthread_mutex_unlock(&obs_mu);
    __real_vkCmdBindDescriptorSets(cb, bp, l, first, n, s, nd, offs);
}

static VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPushDescriptorSetKHR(
    VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l,
    uint32_t set, uint32_t nw, const VkWriteDescriptorSet *w) {
    pthread_mutex_lock(&obs_mu);
    obs_push_calls++;
    obs_push_writes += nw;
    obs_push_sets |= 1u << set;
    pthread_mutex_unlock(&obs_mu);
    REQUIRE(real_push_desc != NULL);
    real_push_desc(cb, bp, l, set, nw, w);
}

static VKAPI_ATTR void VKAPI_CALL wrap_vkUpdateDescriptorSetWithTemplate(
    VkDevice d, VkDescriptorSet set, VkDescriptorUpdateTemplate tpl, const void *data) {
    pthread_mutex_lock(&obs_mu);
    obs_template_calls++;
    pthread_mutex_unlock(&obs_mu);
    real_template_update(d, set, tpl, data);
}

extern __typeof__(vkGetDeviceProcAddr) __real_vkGetDeviceProcAddr;
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice d, const char *name) {
    if (!strcmp(name, "vkCmdBindDescriptorSets")) return (PFN_vkVoidFunction)__wrap_vkCmdBindDescriptorSets;
    if (!strcmp(name, "vkCmdPushDescriptorSetKHR")) {
        PFN_vkVoidFunction fn = __real_vkGetDeviceProcAddr(d, name);
        if (!fn) return NULL;
        real_push_desc = (PFN_vkCmdPushDescriptorSetKHR)fn;
        return (PFN_vkVoidFunction)wrap_vkCmdPushDescriptorSetKHR;
    }
    if (!strcmp(name, "vkUpdateDescriptorSetWithTemplate")) {
        PFN_vkVoidFunction fn = __real_vkGetDeviceProcAddr(d, name);
        if (!fn) return NULL;
        real_template_update = (PFN_vkUpdateDescriptorSetWithTemplate)fn;
        return (PFN_vkVoidFunction)wrap_vkUpdateDescriptorSetWithTemplate;
    }
    return __real_vkGetDeviceProcAddr(d, name);
}

_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateDevice), PFN_vkCreateDevice), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkEnumerateDeviceExtensionProperties), PFN_vkEnumerateDeviceExtensionProperties), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateDescriptorSetLayout), PFN_vkCreateDescriptorSetLayout), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkAllocateDescriptorSets), PFN_vkAllocateDescriptorSets), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkUpdateDescriptorSets), PFN_vkUpdateDescriptorSets), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCmdBindDescriptorSets), PFN_vkCmdBindDescriptorSets), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkGetDeviceProcAddr), PFN_vkGetDeviceProcAddr), "sig");

const char *const *nvn_plat_vk_instance_exts(uint32_t *n) { *n = 0; return NULL; }
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) {
    (void)i; (void)w; *s = VK_NULL_HANDLE; return VK_ERROR_EXTENSION_NOT_PRESENT;
}
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) { (void)w; (void)x; (void)y; return 0; }
void nvn_plat_safepoint(void) {}
void nvn_plat_log(int l, const char *c, const char *s) { (void)l; fprintf(stderr, "[%s] %s\n", c ? c : "", s ? s : ""); }
int nvn_plat_flag(const char *n, int d) {
    if (!strcmp(n, "nvk_desc_memo")) return memo_mode;
    if (!strcmp(n, "nvk_dq")) return dq_mode;
    if (!strcmp(n, "nvk_opt_layout")) return opt_mode;
    if (!strcmp(n, "nvk_push_desc")) return push_mode;
    if (!strcmp(n, "nvk_desc_template")) return template_mode;
    if (!strcmp(n, "nvk_bind_dedup")) return dedup_mode;
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

extern uint64_t nvn_vk_dbg_pipe_stat(int which);

static nvn_gpu_pipe make_gfx_pipe(nvn_gpu_shader vs, nvn_gpu_shader fs, int use_alias) {
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3;
    pd.prim = 4;
    pd.samples = 1;
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.chmask.mask = 0xf;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));
    if (use_alias) {
        nvn_gpu_pipe owner = pipe;
        const uint64_t creates = nvn_vk_dbg_pipe_stat(5);
        // EDS 把正面朝向录到绘制状态。关闭裁面时翻转它不改像素。
        pd.poly.front = 1;
        GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));
        REQUIRE(pipe != owner && nvn_vk_dbg_pipe_stat(5) == creates);
    }
    return pipe;
}

static nvn_gpu_dyn make_dyn(void) {
    nvn_gpu_dyn dyn = {0};
    dyn.nvp = 1; dyn.vp[0][2] = dyn.vp[0][3] = 4.0f; dyn.vp[0][5] = 1.0f;
    dyn.has_sc = 1; dyn.sc[2] = dyn.sc[3] = 4;
    return dyn;
}

static unsigned check_rgba(const unsigned char *px, unsigned char r, unsigned char g, unsigned char b) {
    unsigned bad = 0;
    for (unsigned i = 0; i < 16; i++) {
        const unsigned char *p = px + i * 4;
        if (abs((int)p[0] - (int)r) > 1 || abs((int)p[1] - (int)g) > 1 ||
            abs((int)p[2] - (int)b) > 1 || p[3] != 255) bad++;
    }
    return bad;
}

static void fill_tex(nvn_gpu_cmd cmd, nvn_gpu_view v, float r, float g, float b) {
    nvn_gpu_pass_desc p = {0}; p.w = p.h = 4; p.layers = 1; p.ncolor = 1;
    p.color[0].view = v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[0] = r; p.color[0].clear.f[1] = g; p.color[0].clear.f[2] = b; p.color[0].clear.f[3] = 1.0f;
    nvn_gpu_pass_begin(cmd, &p); nvn_gpu_pass_end(cmd);
}

struct pd_case { const char *name; int id; };
static const struct pd_case k_cases[] = {
    {"gfx-ubo-offset-push", 0},
    {"gfx-ubo-dynamic-offset-reuse", 8},
    {"gfx-multi-set-push-plus-tex", 1},
    {"gfx-tex-in-push-set", 2},
    {"gfx-texel-buf-in-push-set", 3},
    {"compute-ssbo-ubo-push", 4},
    {"gfx-pool-plus-push-set", 5},
    {"gfx-over-limit-fallback", 6},
    {"gfx-eds-alias-push-set1", 7},
};

static void run_case(struct s2a_vk_device *device, const char *dir, const struct pd_case *tc) {
    memo_mode = tc->id == 8 || tc->id == 1 || tc->id == 7;
    obs_reset();
    ncases++;
    const int active = push_mode && !hide_extension;

    nvn_gpu_open_desc od = {0}; od.host_device = device; od.app_label = "push-descriptor";
    GPU(nvn_gpu_open(&od, &err));

    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 4; td.d.depth = td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;

    nvn_gpu_tex rt = NULL; nvn_gpu_view vrt = NULL;
    GPU(nvn_gpu_tex_create(&td, &rt, &err)); GPU(nvn_gpu_view_create(rt, NULL, &vrt, &err));

    unsigned char rb_init[256]; memset(rb_init, 0xa5, sizeof rb_init);
    nvn_gpu_buf rb = NULL;
    GPU(nvn_gpu_buf_create(sizeof rb_init, NVN_GPU_MEM_SHARED, rb_init, &rb, &err));

    // UBO buffer with 3 distinct 256-byte-aligned slots (vec4 colors)
    float ubo_data[3 * 64]; memset(ubo_data, 0, sizeof ubo_data);
    ubo_data[0] = 1.0f; ubo_data[1] = 0.25f; ubo_data[2] = 0.0f; ubo_data[3] = 1.0f;
    ubo_data[64] = 0.5f; ubo_data[65] = 1.0f; ubo_data[66] = 0.25f; ubo_data[67] = 1.0f;
    ubo_data[128] = 0.25f; ubo_data[129] = 0.5f; ubo_data[130] = 1.0f; ubo_data[131] = 1.0f;
    nvn_gpu_buf ubo = NULL;
    GPU(nvn_gpu_buf_create(sizeof ubo_data, NVN_GPU_MEM_SHARED, ubo_data, &ubo, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));

    nvn_gpu_shader vs = load_sh(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = NULL, cs = NULL;
    nvn_gpu_tex t0 = NULL, t1 = NULL;
    nvn_gpu_view vt0 = NULL, vt1 = NULL, tbuf_v = NULL;
    nvn_gpu_samp samp = NULL;
    nvn_gpu_buf ssbo = NULL, tbuf = NULL;
    nvn_gpu_pool_table pool = NULL;

    unsigned op_bad = 0, px_bad = 0;

    if (tc->id == 0 || tc->id == 8) {
        // gfx-ubo-offset-push: single set 0 = UBO, 3 draws with 3 different offsets
        nvn_gpu_layout_ent ent = {0};
        ent.set = 0; ent.binding = 0; ent.cls = NVN_GPU_RES_UBO; ent.count = 1;
        ent.access = NVN_GPU_ACC_R; ent.stages = 1u << NVN_GPU_SH_FS;
        nvn_gpu_layout lay = {.ent = &ent, .nent = 1};
        fs = load_sh(dir, "ubo_only.frag", NVN_GPU_SH_FS, &lay);
        nvn_gpu_pipe pipe = make_gfx_pipe(vs, fs, 0);
        nvn_gpu_dyn dyn = make_dyn();
        nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;

        for (unsigned k = 0; k < 3; k++) {
            nvn_gpu_pass_desc p = {0}; p.w = p.h = 4; p.layers = 1; p.ncolor = 1;
            p.color[0].view = vrt; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
            nvn_gpu_pass_begin(cmd, &p);
            nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn);
            nvn_gpu_binding b = {0}; b.ent = 0; b.cls = NVN_GPU_RES_UBO;
            b.u.b.buf = ubo; b.u.b.off = (uint64_t)k * 256; b.u.b.len = 16;
            nvn_gpu_bind(cmd, NVN_GPU_SH_FS, &b, 1);
            nvn_gpu_draw(cmd, &da);
            nvn_gpu_pass_end(cmd);
            nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, (uint64_t)k * 64, 16, 64);
        }
        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        if (tc->id == 8) {
            if (obs_push_dsl || obs_push_calls || obs_dynamic_binds != 3 ||
                obs_dynamic_offsets[0] != 0 || obs_dynamic_offsets[1] != 256 ||
                obs_dynamic_offsets[2] != 512 || obs_update_calls + obs_template_calls != 1)
                op_bad++;
        } else if (active) {
            if (obs_push_dsl < 1 || obs_push_calls != 3 || obs_alloc_sets != 0 || obs_update_calls != 0 || obs_bind_calls != 0) op_bad++;
        } else {
            if (obs_push_dsl != 0 || obs_push_calls != 0) op_bad++;
        }
        const unsigned char *px = nvn_gpu_buf_cpu(rb);
        px_bad += check_rgba(px, 255, 64, 0);
        px_bad += check_rgba(px + 64, 128, 255, 64);
        px_bad += check_rgba(px + 128, 64, 128, 255);
    } else if (tc->id == 1 || tc->id == 7) {
        const int alias = tc->id == 7;
        const unsigned ubo_set = alias ? 1 : 0;
        nvn_gpu_layout_ent ent[3] = {{0}};
        ent[0].set = ubo_set; ent[0].binding = 0; ent[0].cls = NVN_GPU_RES_UBO;
        ent[1].set = 1 - ubo_set; ent[1].binding = 0; ent[1].cls = NVN_GPU_RES_TEX;
        ent[2].set = 1 - ubo_set; ent[2].binding = 1; ent[2].cls = NVN_GPU_RES_SAMP;
        for (int i = 0; i < 3; i++) { ent[i].count = 1; ent[i].access = NVN_GPU_ACC_R; ent[i].stages = 1u << NVN_GPU_SH_FS; }
        nvn_gpu_layout lay = {.ent = ent, .nent = 3};
        fs = load_sh(dir, alias ? "ubo_set1_tex_set0.frag" : "ubo_set0_tex_set1.frag", NVN_GPU_SH_FS, &lay);
        nvn_gpu_pipe pipe = make_gfx_pipe(vs, fs, alias);
        GPU(nvn_gpu_tex_create(&td, &t0, &err)); GPU(nvn_gpu_view_create(t0, NULL, &vt0, &err));
        GPU(nvn_gpu_tex_create(&td, &t1, &err)); GPU(nvn_gpu_view_create(t1, NULL, &vt1, &err));
        samp_par sp = {0}; GPU(nvn_gpu_samp_create(&sp, &samp, &err));
        fill_tex(cmd, vt0, 1.0f, 0.5f, 1.0f);
        fill_tex(cmd, vt1, 0.5f, 1.0f, 0.5f);

        nvn_gpu_dyn dyn = make_dyn();
        nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_pass_desc p = {0}; p.w = p.h = 4; p.layers = 1; p.ncolor = 1;
        p.color[0].view = vrt; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;

        // Draw 1: UBO[0] (1, 0.25, 0) * t0 (1, 0.5, 1) = (1, 0.125, 0) -> (255, 32, 0)
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn);
        nvn_gpu_binding b[3] = {{0}};
        b[0].ent = 0; b[0].cls = NVN_GPU_RES_UBO; b[0].u.b.buf = ubo; b[0].u.b.off = 0; b[0].u.b.len = 16;
        b[1].ent = 1; b[1].cls = NVN_GPU_RES_TEX; b[1].u.view = vt0;
        b[2].ent = 2; b[2].cls = NVN_GPU_RES_SAMP; b[2].u.samp = samp;
        nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 3);
        nvn_gpu_draw(cmd, &da);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);

        // Draw 2: change ONLY UBO to UBO[1] (0.5, 1, 0.25) * t0 (1, 0.5, 1) = (0.5, 0.5, 0.25) -> (128, 128, 64)
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn);
        b[0].u.b.off = 256;
        nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 3);
        nvn_gpu_draw(cmd, &da);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 64, 16, 64);

        // Draw 3: change ONLY tex to t1 (0.5, 1, 0.5) * UBO[1] (0.5, 1, 0.25) = (0.25, 1, 0.125) -> (64, 255, 32)
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn);
        b[1].u.view = vt1;
        nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 3);
        nvn_gpu_draw(cmd, &da);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 128, 16, 64);

        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        if (active) {
            // 纹理组在第 1、3 笔推送。UBO 组只写一次，第二笔只变绑定偏移。
            if (obs_push_dsl != 1 || obs_push_sets != (1u << (1 - ubo_set)) || obs_push_calls != 2 ||
                obs_update_calls != (template_mode ? 0u : 1u) || obs_template_calls != (template_mode ? 1u : 0u) ||
                obs_dynamic_binds != 2 || obs_dynamic_offsets[0] != 0 || obs_dynamic_offsets[1] != 256)
                op_bad++;
        } else {
            if (obs_push_dsl != 0 || obs_push_calls != 0) op_bad++;
        }
        const unsigned char *px = nvn_gpu_buf_cpu(rb);
        px_bad += check_rgba(px, 255, 32, 0);
        px_bad += check_rgba(px + 64, 128, 128, 64);
        px_bad += check_rgba(px + 128, 64, 255, 32);
    } else if (tc->id == 2) {
        // gfx-tex-in-push-set: single set 0 has UBO + TEX + SAMP (both buffer & image infos in push!)
        nvn_gpu_layout_ent ent[3] = {{0}};
        ent[0].set = 0; ent[0].binding = 0; ent[0].cls = NVN_GPU_RES_UBO;
        ent[1].set = 0; ent[1].binding = 1; ent[1].cls = NVN_GPU_RES_TEX;
        ent[2].set = 0; ent[2].binding = 2; ent[2].cls = NVN_GPU_RES_SAMP;
        for (int i = 0; i < 3; i++) { ent[i].count = 1; ent[i].access = NVN_GPU_ACC_R; ent[i].stages = 1u << NVN_GPU_SH_FS; }
        nvn_gpu_layout lay = {.ent = ent, .nent = 3};
        fs = load_sh(dir, "ubo_tex_set0.frag", NVN_GPU_SH_FS, &lay);
        nvn_gpu_pipe pipe = make_gfx_pipe(vs, fs, 0);
        GPU(nvn_gpu_tex_create(&td, &t0, &err)); GPU(nvn_gpu_view_create(t0, NULL, &vt0, &err));
        samp_par sp = {0}; GPU(nvn_gpu_samp_create(&sp, &samp, &err));
        fill_tex(cmd, vt0, 0.5f, 1.0f, 0.5f);

        nvn_gpu_pass_desc p = {0}; p.w = p.h = 4; p.layers = 1; p.ncolor = 1;
        p.color[0].view = vrt; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_dyn dyn = make_dyn();
        nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn);
        nvn_gpu_binding b[3] = {{0}};
        b[0].ent = 0; b[0].cls = NVN_GPU_RES_UBO; b[0].u.b.buf = ubo; b[0].u.b.off = 0; b[0].u.b.len = 16;
        b[1].ent = 1; b[1].cls = NVN_GPU_RES_TEX; b[1].u.view = vt0;
        b[2].ent = 2; b[2].cls = NVN_GPU_RES_SAMP; b[2].u.samp = samp;
        nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 3);
        nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_draw(cmd, &da);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);

        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        if (active) {
            if (obs_push_dsl < 1 || obs_push_calls != 1 || obs_push_writes != 3 || obs_alloc_sets != 0 || obs_update_calls != 0) op_bad++;
        } else {
            if (obs_push_calls != 0) op_bad++;
        }
        px_bad += check_rgba(nvn_gpu_buf_cpu(rb), 128, 64, 0);
    } else if (tc->id == 3) {
        // gfx-texel-buf-in-push-set: set 0 = TBUF (pTexelBufferView deep-copy in DQ + push)
        nvn_gpu_layout_ent ent = {0};
        ent.set = 0; ent.binding = 0; ent.cls = NVN_GPU_RES_TBUF; ent.count = 1;
        ent.access = NVN_GPU_ACC_R; ent.stages = 1u << NVN_GPU_SH_FS;
        nvn_gpu_layout lay = {.ent = &ent, .nent = 1};
        fs = load_sh(dir, "tbuf_set0.frag", NVN_GPU_SH_FS, &lay);
        nvn_gpu_pipe pipe = make_gfx_pipe(vs, fs, 0);
        unsigned char tdata[16] = {64, 192, 128, 255, 64, 192, 128, 255, 64, 192, 128, 255, 64, 192, 128, 255};
        GPU(nvn_gpu_buf_create(sizeof tdata, NVN_GPU_MEM_SHARED, tdata, &tbuf, &err));
        GPU(nvn_gpu_view_create_buf(tbuf, 0, sizeof tdata, NVN_FORMAT_RGBA8, &tbuf_v, &err));

        nvn_gpu_pass_desc p = {0}; p.w = p.h = 4; p.layers = 1; p.ncolor = 1;
        p.color[0].view = vrt; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_dyn dyn = make_dyn();
        nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn);
        nvn_gpu_binding b = {0}; b.ent = 0; b.cls = NVN_GPU_RES_TBUF; b.u.view = tbuf_v;
        nvn_gpu_bind(cmd, NVN_GPU_SH_FS, &b, 1);
        nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_draw(cmd, &da);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);

        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        if (active) {
            if (obs_push_dsl < 1 || obs_push_calls != 1 || obs_alloc_sets != 0 || obs_update_calls != 0) op_bad++;
        } else {
            if (obs_push_calls != 0) op_bad++;
        }
        px_bad += check_rgba(nvn_gpu_buf_cpu(rb), 64, 192, 128);
    } else if (tc->id == 4) {
        // compute-ssbo-ubo-push: CS with set 0 = UBO + SSBO, 2 dispatches with different UBO offsets
        nvn_gpu_layout_ent ent[2] = {{0}};
        ent[0].set = 0; ent[0].binding = 0; ent[0].cls = NVN_GPU_RES_UBO; ent[0].access = NVN_GPU_ACC_R;
        ent[1].set = 0; ent[1].binding = 1; ent[1].cls = NVN_GPU_RES_SSBO; ent[1].access = NVN_GPU_ACC_RW;
        for (int i = 0; i < 2; i++) { ent[i].count = 1; ent[i].stages = 1u << NVN_GPU_SH_CS; }
        nvn_gpu_layout lay = {.ent = ent, .nent = 2};
        cs = load_sh(dir, "push_add.comp", NVN_GPU_SH_CS, &lay);
        nvn_gpu_stage_desc csd = {.sh = cs};
        nvn_gpu_pipe cpipe = NULL;
        GPU(nvn_gpu_cpipe_get(&csd, NVN_GPU_PIPE_SYNC, &cpipe, &err));
        float zbuf[8] = {0};
        GPU(nvn_gpu_buf_create(sizeof zbuf, NVN_GPU_MEM_SHARED, zbuf, &ssbo, &err));

        nvn_gpu_set_pipe(cmd, cpipe);
        for (unsigned k = 0; k < 2; k++) {
            nvn_gpu_binding b[2] = {{0}};
            b[0].ent = 0; b[0].cls = NVN_GPU_RES_UBO; b[0].u.b.buf = ubo; b[0].u.b.off = (uint64_t)k * 256; b[0].u.b.len = 16;
            b[1].ent = 1; b[1].cls = NVN_GPU_RES_SSBO; b[1].u.b.buf = ssbo; b[1].u.b.off = (uint64_t)k * 16; b[1].u.b.len = 16;
            nvn_gpu_bind(cmd, NVN_GPU_SH_CS, b, 2);
            nvn_gpu_dispatch(cmd, cpipe, 1, 1, 1, NULL, 0);
        }
        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        if (active) {
            if (obs_push_dsl < 1 || obs_push_calls != 2 || obs_alloc_sets != 0 || obs_update_calls != 0 || obs_bind_calls != 0) op_bad++;
        } else {
            if (obs_push_calls != 0) op_bad++;
        }
        const float *out = (const float *)nvn_gpu_buf_cpu(ssbo);
        if (out[0] != 1.0f || out[1] != 0.25f || out[4] != 0.5f || out[5] != 1.0f) px_bad++;
    } else if (tc->id == 5) {
        // gfx-pool-plus-push-set: set 0 = UBO (push), set 1 = NVN_RB_TEXP pool (regular) + sampler
        nvn_gpu_layout_ent ent[3] = {{0}};
        ent[0].set = 0; ent[0].binding = 0; ent[0].cls = NVN_GPU_RES_UBO; ent[0].count = 1;
        ent[1].set = 1; ent[1].binding = 0; ent[1].cls = NVN_GPU_RES_TEX; ent[1].count = 2; ent[1].src_kind = NVN_RB_TEXP;
        ent[2].set = 1; ent[2].binding = 1; ent[2].cls = NVN_GPU_RES_SAMP; ent[2].count = 1;
        for (int i = 0; i < 3; i++) { ent[i].access = NVN_GPU_ACC_R; ent[i].stages = 1u << NVN_GPU_SH_FS; }
        nvn_gpu_layout lay = {.ent = ent, .nent = 3};
        fs = load_sh(dir, "ubo_set0_pool_set1.frag", NVN_GPU_SH_FS, &lay);
        nvn_gpu_pipe pipe = make_gfx_pipe(vs, fs, 0);
        GPU(nvn_gpu_tex_create(&td, &t0, &err)); GPU(nvn_gpu_view_create(t0, NULL, &vt0, &err));
        samp_par sp = {0}; GPU(nvn_gpu_samp_create(&sp, &samp, &err));
        fill_tex(cmd, vt0, 0.5f, 1.0f, 0.5f);
        nvn_gpu_pool_item item = {.state = NVN_GPU_POOL_READY}; item.resource.view = vt0;
        nvn_gpu_pool_desc pdsc = {.kind = NVN_GPU_POOL_IMAGE, .count = 1, .items = &item}; pdsc.dummy.view = vt0;
        GPU(nvn_gpu_pool_create(&pdsc, &pool, &err));

        nvn_gpu_pass_desc p = {0}; p.w = p.h = 4; p.layers = 1; p.ncolor = 1;
        p.color[0].view = vrt; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_dyn dyn = make_dyn();
        nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn);
        nvn_gpu_binding b[2] = {{0}};
        b[0].ent = 0; b[0].cls = NVN_GPU_RES_UBO; b[0].u.b.buf = ubo; b[0].u.b.off = 0; b[0].u.b.len = 16;
        b[1].ent = 2; b[1].cls = NVN_GPU_RES_SAMP; b[1].u.samp = samp;
        nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
        GPU(nvn_gpu_pool_bind(cmd, NVN_GPU_SH_FS, 1, pool, &err));
        nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_draw(cmd, &da);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);

        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        if (active) {
            if (obs_push_dsl != 1 || obs_push_calls != 1) op_bad++;
        } else {
            if (obs_push_calls != 0) op_bad++;
        }
        px_bad += check_rgba(nvn_gpu_buf_cpu(rb), 128, 64, 0);
    } else if (tc->id == 6) {
        // gfx-over-limit-fallback: set 0 has 33 textures (> maxPushDescriptors=32) -> must NOT use push DSL!
        nvn_gpu_layout_ent ent[2] = {{0}};
        ent[0].set = 0; ent[0].binding = 0; ent[0].cls = NVN_GPU_RES_TEX; ent[0].count = 33;
        ent[1].set = 0; ent[1].binding = 1; ent[1].cls = NVN_GPU_RES_SAMP; ent[1].count = 1;
        for (int i = 0; i < 2; i++) { ent[i].access = NVN_GPU_ACC_R; ent[i].stages = 1u << NVN_GPU_SH_FS; }
        nvn_gpu_layout lay = {.ent = ent, .nent = 2};
        fs = load_sh(dir, "over_limit.frag", NVN_GPU_SH_FS, &lay);
        nvn_gpu_pipe pipe = make_gfx_pipe(vs, fs, 0);
        GPU(nvn_gpu_tex_create(&td, &t0, &err)); GPU(nvn_gpu_view_create(t0, NULL, &vt0, &err));
        samp_par sp = {0}; GPU(nvn_gpu_samp_create(&sp, &samp, &err));
        fill_tex(cmd, vt0, 0.25f, 0.75f, 1.0f);

        nvn_gpu_pass_desc p = {0}; p.w = p.h = 4; p.layers = 1; p.ncolor = 1;
        p.color[0].view = vrt; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_dyn dyn = make_dyn();
        nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn);
        nvn_gpu_binding b[34]; memset(b, 0, sizeof b);
        for (uint32_t i = 0; i < 33; i++) { b[i].ent = 0; b[i].elem = i; b[i].cls = NVN_GPU_RES_TEX; b[i].u.view = vt0; }
        b[33].ent = 1; b[33].cls = NVN_GPU_RES_SAMP; b[33].u.samp = samp;
        nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 34);
        nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_draw(cmd, &da);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);

        GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        // Over limit (34 > 32): MUST fall back cleanly to regular descriptor set!
        if (device->max_push_descriptors < 34 && (obs_push_dsl != 0 || obs_push_calls != 0)) op_bad++;
        px_bad += check_rgba(nvn_gpu_buf_cpu(rb), 64, 191, 255);
    }

    if (active && (tc->id == 0 || tc->id == 2 || tc->id == 3 || tc->id == 4 || tc->id == 5) &&
        obs_template_calls) op_bad++;
    const unsigned bad = op_bad || px_bad;
    printf("push-desc case=%s active=%d pdsl=%u push=%u pw=%u alloc=%u upd=%u tpl=%u bind=%u op_bad=%u px_bad=%u bad=%u\n",
           tc->name, active, obs_push_dsl, obs_push_calls, obs_push_writes,
           obs_alloc_sets, obs_update_calls, obs_template_calls, obs_bind_calls, op_bad, px_bad, bad);
    failures += !!bad;

    if (pool) nvn_gpu_pool_release(pool);
    if (samp) nvn_gpu_samp_release(samp);
    if (vt0) nvn_gpu_view_release(vt0);
    if (t0) nvn_gpu_tex_release(t0);
    if (vt1) nvn_gpu_view_release(vt1);
    if (t1) nvn_gpu_tex_release(t1);
    if (tbuf_v) nvn_gpu_view_release(tbuf_v);
    if (tbuf) nvn_gpu_buf_release(tbuf, NULL, NULL);
    if (ssbo) nvn_gpu_buf_release(ssbo, NULL, NULL);
    nvn_gpu_view_release(vrt); nvn_gpu_tex_release(rt);
    nvn_gpu_buf_release(ubo, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs);
    if (fs) nvn_gpu_shader_release(fs);
    if (cs) nvn_gpu_shader_release(cs);
    GPU(nvn_gpu_close(5000000000ull, &err));
}

int main(int argc, char **argv) {
    if (argc < 6 || argc > 8) return 2;
    dq_mode = atoi(argv[2]);
    opt_mode = atoi(argv[3]);
    push_mode = atoi(argv[4]);
    hide_extension = atoi(argv[5]);
    if (argc >= 7) template_mode = atoi(argv[6]);
    if (argc == 8) dedup_mode = atoi(argv[7]);

    struct s2a_vk_device device;
    char why[256] = {0};
    REQUIRE(!s2a_vk_device_create(&device, why, sizeof why));
    REQUIRE(enabled_push_ext == !hide_extension);
    REQUIRE(device.push_descriptor_enabled == !hide_extension);
    if (template_mode) REQUIRE(device.desc_update_template_enabled);
    REQUIRE(device.eds1_enabled);
    if (!hide_extension) REQUIRE(device.max_push_descriptors >= 32);
    else REQUIRE(device.max_push_descriptors == 0);

    uint32_t n = 0;
    REQUIRE(__real_vkEnumerateDeviceExtensionProperties(device.physical, NULL, &n, NULL) == VK_SUCCESS);
    VkExtensionProperties *ext = calloc(n ? n : 1, sizeof *ext); REQUIRE(ext);
    REQUIRE(__real_vkEnumerateDeviceExtensionProperties(device.physical, NULL, &n, ext) == VK_SUCCESS);
    int advertised = 0;
    for (uint32_t i = 0; i < n; i++)
        advertised |= !strcmp(ext[i].extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    free(ext);
    REQUIRE(advertised);

    for (unsigned i = 0; i < sizeof k_cases / sizeof *k_cases; i++) {
        // Pool images require GENERAL layout when nvk_opt_layout=1 (matches t_feedback.c).
        if (k_cases[i].id == 5 && opt_mode) continue;
        run_case(&device, argv[1], &k_cases[i]);
    }

    const uint32_t max_push = device.max_push_descriptors;
    REQUIRE(!s2a_vk_device_destroy(&device, 1));
    printf("push-descriptor cases=%u failures=%u dq=%d opt=%d push=%d hidden=%d template=%d bind_dedup=%d ext-enabled=%d max=%u\n",
           ncases, failures, dq_mode, opt_mode, push_mode, hide_extension, template_mode, dedup_mode, enabled_push_ext, max_push);
    return failures ? 1 : 0;
}
