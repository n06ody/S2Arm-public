// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real-Vulkan contract fixture for VK_KHR_synchronization2 (debug.s2a.nvk_sync2).
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nvn_gpu_err err;
static int g_dq_mode = 0;
static int g_opt_mode = 0;
static int g_sync2_flag = 1;
static int g_hide_ext = 0;
static unsigned g_failures = 0;
static uint64_t g_sig = 0;

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "sync2 fixture failed line %d: %s rc=%d why=%s\n", __LINE__, #x, err.rc.code, err.why); exit(2); } } while (0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))

// ---- Vulkan call observers via -Wl,--wrap ----
static _Atomic uint32_t g_ext_requested = 0;
static _Atomic uint32_t g_feat_chained = 0;
static _Atomic uint64_t g_b1_calls = 0;
static _Atomic uint64_t g_b2_calls = 0;
static _Atomic uint64_t g_b2_copy = 0;
static _Atomic uint64_t g_b2_clear = 0;
static _Atomic uint64_t g_b2_blit = 0;
static _Atomic uint64_t g_b2_resolve = 0;
static _Atomic uint64_t g_b2_fine_vi = 0;
static _Atomic uint64_t g_b2_none_src = 0;
static _Atomic uint64_t g_b2_sampled_read = 0;
static _Atomic uint64_t g_b2_storage_write = 0;
static _Atomic uint64_t g_b2_bad_none_access = 0;

static PFN_vkCmdPipelineBarrier g_real_b1 = NULL;
static PFN_vkCmdPipelineBarrier2KHR g_real_b2 = NULL;

static void obs_reset_barriers(void) {
    atomic_store(&g_b1_calls, 0);
    atomic_store(&g_b2_calls, 0);
    atomic_store(&g_b2_copy, 0);
    atomic_store(&g_b2_clear, 0);
    atomic_store(&g_b2_blit, 0);
    atomic_store(&g_b2_resolve, 0);
    atomic_store(&g_b2_fine_vi, 0);
    atomic_store(&g_b2_none_src, 0);
    atomic_store(&g_b2_sampled_read, 0);
    atomic_store(&g_b2_storage_write, 0);
    atomic_store(&g_b2_bad_none_access, 0);
}

static void obs_inspect_stage_access(VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                                     VkPipelineStageFlags2 ds, VkAccessFlags2 da, int is_img) {
    const VkPipelineStageFlags2 both = ss | ds;
    if (both & VK_PIPELINE_STAGE_2_COPY_BIT_KHR) atomic_fetch_add(&g_b2_copy, 1);
    if (both & VK_PIPELINE_STAGE_2_CLEAR_BIT_KHR) atomic_fetch_add(&g_b2_clear, 1);
    if (both & VK_PIPELINE_STAGE_2_BLIT_BIT_KHR) atomic_fetch_add(&g_b2_blit, 1);
    if (both & VK_PIPELINE_STAGE_2_RESOLVE_BIT_KHR) atomic_fetch_add(&g_b2_resolve, 1);
    if (both & (VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT_KHR | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT_KHR))
        atomic_fetch_add(&g_b2_fine_vi, 1);
    if (is_img && ss == VK_PIPELINE_STAGE_2_NONE_KHR && sa == 0)
        atomic_fetch_add(&g_b2_none_src, 1);
    if ((sa | da) & VK_ACCESS_2_SHADER_SAMPLED_READ_BIT_KHR)
        atomic_fetch_add(&g_b2_sampled_read, 1);
    if (sa & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT_KHR)
        atomic_fetch_add(&g_b2_storage_write, 1);
    if ((ss == VK_PIPELINE_STAGE_2_NONE_KHR && sa != 0) ||
        (ds == VK_PIPELINE_STAGE_2_NONE_KHR && da != 0))
        atomic_fetch_add(&g_b2_bad_none_access, 1);
}

static VKAPI_ATTR void VKAPI_CALL obs_b1(VkCommandBuffer cb, VkPipelineStageFlags ss, VkPipelineStageFlags ds,
                                         VkDependencyFlags dep, uint32_t nm, const VkMemoryBarrier *m,
                                         uint32_t nb, const VkBufferMemoryBarrier *b,
                                         uint32_t ni, const VkImageMemoryBarrier *im) {
    atomic_fetch_add(&g_b1_calls, 1);
    g_real_b1(cb, ss, ds, dep, nm, m, nb, b, ni, im);
}

static VKAPI_ATTR void VKAPI_CALL obs_b2(VkCommandBuffer cb, const VkDependencyInfoKHR *di) {
    atomic_fetch_add(&g_b2_calls, 1);
    if (di) {
        for (uint32_t i = 0; i < di->memoryBarrierCount; i++) {
            const VkMemoryBarrier2KHR *b = &di->pMemoryBarriers[i];
            obs_inspect_stage_access(b->srcStageMask, b->srcAccessMask, b->dstStageMask, b->dstAccessMask, 0);
        }
        for (uint32_t i = 0; i < di->bufferMemoryBarrierCount; i++) {
            const VkBufferMemoryBarrier2KHR *b = &di->pBufferMemoryBarriers[i];
            obs_inspect_stage_access(b->srcStageMask, b->srcAccessMask, b->dstStageMask, b->dstAccessMask, 0);
        }
        for (uint32_t i = 0; i < di->imageMemoryBarrierCount; i++) {
            const VkImageMemoryBarrier2KHR *b = &di->pImageMemoryBarriers[i];
            obs_inspect_stage_access(b->srcStageMask, b->srcAccessMask, b->dstStageMask, b->dstAccessMask, 1);
        }
    }
    g_real_b2(cb, di);
}

VkResult __real_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *);
VkResult __wrap_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice p, const char *layer, uint32_t *cnt, VkExtensionProperties *props) {
    if (!g_hide_ext) return __real_vkEnumerateDeviceExtensionProperties(p, layer, cnt, props);
    uint32_t full = 0;
    VkResult r = __real_vkEnumerateDeviceExtensionProperties(p, layer, &full, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties *tmp = calloc(full ? full : 1, sizeof *tmp);
    REQUIRE(tmp);
    r = __real_vkEnumerateDeviceExtensionProperties(p, layer, &full, tmp);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) { free(tmp); return r; }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < full; i++) {
        if (!strcmp(tmp[i].extensionName, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) continue;
        if (props && kept < *cnt) props[kept] = tmp[i];
        kept++;
    }
    free(tmp);
    if (!props) { *cnt = kept; return VK_SUCCESS; }
    if (*cnt < kept) return VK_INCOMPLETE;
    *cnt = kept;
    return VK_SUCCESS;
}

VkResult __real_vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
VkResult __wrap_vkCreateDevice(VkPhysicalDevice p, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        if (!strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME))
            atomic_store(&g_ext_requested, 1);
    for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR) {
            const VkPhysicalDeviceSynchronization2FeaturesKHR *f = (const void *)s;
            if (f->synchronization2 == VK_TRUE) atomic_store(&g_feat_chained, 1);
        }
    }
    return __real_vkCreateDevice(p, ci, a, out);
}

PFN_vkVoidFunction __real_vkGetDeviceProcAddr(VkDevice, const char *);
PFN_vkVoidFunction __wrap_vkGetDeviceProcAddr(VkDevice dev, const char *name) {
    PFN_vkVoidFunction fn = __real_vkGetDeviceProcAddr(dev, name);
    if (!fn || !name) return fn;
    if (!strcmp(name, "vkCmdPipelineBarrier")) {
        g_real_b1 = (PFN_vkCmdPipelineBarrier)fn;
        return (PFN_vkVoidFunction)obs_b1;
    }
    if (!strcmp(name, "vkCmdPipelineBarrier2KHR")) {
        g_real_b2 = (PFN_vkCmdPipelineBarrier2KHR)fn;
        return (PFN_vkVoidFunction)obs_b2;
    }
    return fn;
}

void __real_vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
                                 uint32_t, const VkMemoryBarrier *, uint32_t, const VkBufferMemoryBarrier *,
                                 uint32_t, const VkImageMemoryBarrier *);
void __wrap_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags ss, VkPipelineStageFlags ds,
                                 VkDependencyFlags dep, uint32_t nm, const VkMemoryBarrier *m,
                                 uint32_t nb, const VkBufferMemoryBarrier *b,
                                 uint32_t ni, const VkImageMemoryBarrier *im) {
    atomic_fetch_add(&g_b1_calls, 1);
    __real_vkCmdPipelineBarrier(cb, ss, ds, dep, nm, m, nb, b, ni, im);
}

// ---- Platform stubs ----
const char *const *nvn_plat_vk_instance_exts(uint32_t *n) { *n = 0; return NULL; }
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) { *s = VK_NULL_HANDLE; return VK_ERROR_EXTENSION_NOT_PRESENT; }
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) { return 0; }
void nvn_plat_safepoint(void) {}
int nvn_plat_flag(const char *n, int d) {
    if (!strcmp(n, "nvk_dq")) return g_dq_mode;
    if (!strcmp(n, "nvk_opt_layout")) return g_opt_mode;
    if (!strcmp(n, "nvk_sync2")) return g_sync2_flag;
    return d;
}
void nvn_plat_log(int l, const char *c, const char *s) {
    fprintf(stderr, "[%s] %s\n", c ? c : "", s ? s : "");
}

static void on_done(void *u, const nvn_gpu_done *d) { REQUIRE(d->status == NVN_GPU_DONE_OK); }

static void submit_and_wait(nvn_gpu_cmd cmd) {
    uint64_t s = ++g_sig;
    GPU(nvn_gpu_cmd_commit(cmd, s, on_done, NULL, &err));
    GPU(nvn_gpu_fence_wait(s, 10000000000ull, &err));
}

static nvn_gpu_shader load_shader(const char *dir, const char *name, nvn_gpu_stage stage, const nvn_gpu_layout *layout) {
    char path[1024];
    REQUIRE(snprintf(path, sizeof path, "%s/%s.spv", dir, name) < (int)sizeof path);
    FILE *f = fopen(path, "rb");
    REQUIRE(f);
    REQUIRE(!fseek(f, 0, SEEK_END));
    long n = ftell(f);
    REQUIRE(n > 0);
    rewind(f);
    void *data = malloc((size_t)n);
    REQUIRE(data);
    REQUIRE(fread(data, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    nvn_gpu_shader_src src = {0};
    src.stage = stage;
    src.kind = NVN_GPU_BLOB_SPIRV;
    src.entry = "main";
    src.blob = data;
    src.bytes = (size_t)n;
    src.layout = layout;
    nvn_gpu_shader sh = NULL;
    GPU(nvn_gpu_shader_load(&src, &sh, &err));
    free(data);
    return sh;
}

static int near_u8(uint8_t a, uint8_t b, int tol) {
    int d = (int)a - (int)b;
    return d >= -tol && d <= tol;
}

static void check_rgba(const char *tag, const uint8_t *px, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int tol) {
    int ok = near_u8(px[0], r, tol) && near_u8(px[1], g, tol) && near_u8(px[2], b, tol) && near_u8(px[3], a, tol);
    if (!ok) {
        fprintf(stderr, "FAIL %s: got=(%u,%u,%u,%u) want=(%u,%u,%u,%u)+-%d\n",
                tag, px[0], px[1], px[2], px[3], r, g, b, a, tol);
        g_failures++;
    }
}

static void set_full_dyn(nvn_gpu_cmd cmd, uint32_t w, uint32_t h) {
    nvn_gpu_dyn dyn = {0};
    dyn.nvp = 1;
    dyn.vp[0][2] = (float)w;
    dyn.vp[0][3] = (float)h;
    dyn.vp[0][5] = 1.0f;
    dyn.has_sc = 1;
    dyn.sc[2] = w;
    dyn.sc[3] = h;
    nvn_gpu_set_dyn(cmd, &dyn);
}

// Case 1: init UNDEFINED->GENERAL (NONE_KHR) + clear_view (CLEAR_BIT_KHR) + copy_tex (COPY_BIT_KHR) + copy_tex_buf (COPY_BIT_KHR)
static void case_clear_copy_tex(void) {
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 4;
    td.d.depth = 1;
    td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex ta = NULL, tb = NULL;
    nvn_gpu_view va = NULL;
    GPU(nvn_gpu_tex_create(&td, &ta, &err));
    GPU(nvn_gpu_tex_create(&td, &tb, &err));
    GPU(nvn_gpu_view_create(ta, NULL, &va, &err));
    nvn_gpu_buf rb = NULL;
    uint8_t zero[64] = {0};
    GPU(nvn_gpu_buf_create(sizeof zero, NVN_GPU_MEM_SHARED, zero, &rb, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    const float col[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    nvn_gpu_clear_view(cmd, va, col, NULL, 0);
    nvn_gpu_copy_tex(cmd, ta, 0, 0, NULL, tb, 0, 0, NULL);
    nvn_gpu_copy_tex_buf(cmd, tb, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    REQUIRE(px);
    check_rgba("case1-clear-copy-tex", px, 64, 128, 191, 255, 2);

    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(va);
    nvn_gpu_tex_release(tb);
    nvn_gpu_tex_release(ta);
}

// Case 2: copy_buf_tex (COPY_BIT_KHR) + blit 8x8 -> 4x4 (BLIT_BIT_KHR) + readback
static void case_upload_blit(void) {
    nvn_gpu_tex_desc tds = {0}, tdd = {0};
    tds.d.target = tdd.d.target = NVN_TEXTURE_TARGET_2D;
    tds.d.format = tdd.d.format = NVN_FORMAT_RGBA8;
    tds.d.width = tds.d.height = 8;
    tdd.d.width = tdd.d.height = 4;
    tds.d.depth = tdd.d.depth = 1;
    tds.d.levels = tdd.d.levels = 1;
    tds.usage = tdd.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex ts = NULL, td = NULL;
    nvn_gpu_view vs = NULL, vd = NULL;
    GPU(nvn_gpu_tex_create(&tds, &ts, &err));
    GPU(nvn_gpu_tex_create(&tdd, &td, &err));
    GPU(nvn_gpu_view_create(ts, NULL, &vs, &err));
    GPU(nvn_gpu_view_create(td, NULL, &vd, &err));

    uint8_t src_px[8 * 8 * 4];
    for (int i = 0; i < 64; i++) {
        src_px[i * 4 + 0] = 200;
        src_px[i * 4 + 1] = 100;
        src_px[i * 4 + 2] = 50;
        src_px[i * 4 + 3] = 255;
    }
    nvn_gpu_buf sb = NULL, rb = NULL;
    uint8_t zero[64] = {0};
    GPU(nvn_gpu_buf_create(sizeof src_px, NVN_GPU_MEM_SHARED, src_px, &sb, &err));
    GPU(nvn_gpu_buf_create(sizeof zero, NVN_GPU_MEM_SHARED, zero, &rb, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_copy_buf_tex(cmd, sb, 0, 8 * 4, sizeof src_px, ts, 0, 0, NULL);
    NVNcopyRegion sr = {0, 0, 0, 8, 8, 1};
    NVNcopyRegion dr = {0, 0, 0, 4, 4, 1};
    nvn_gpu_blit(cmd, vs, &sr, vd, &dr, 1);
    nvn_gpu_copy_tex_buf(cmd, td, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    REQUIRE(px);
    check_rgba("case2-upload-blit", px, 200, 100, 50, 255, 2);

    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_buf_release(sb, NULL, NULL);
    nvn_gpu_view_release(vd);
    nvn_gpu_view_release(vs);
    nvn_gpu_tex_release(td);
    nvn_gpu_tex_release(ts);
}

// Case 3: MSAA 4x render -> nvn_gpu_blit resolve (RESOLVE_BIT_KHR) -> readback
static void case_msaa_resolve(const char *dir) {
    nvn_gpu_shader vs = load_shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = load_shader(dir, "solid_green.frag", NVN_GPU_SH_FS, NULL);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3;
    pd.prim = 4;
    pd.samples = 4;
    pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));

    nvn_gpu_tex_desc tdm = {0}, tdr = {0};
    tdm.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE;
    tdr.d.target = NVN_TEXTURE_TARGET_2D;
    tdm.d.format = tdr.d.format = NVN_FORMAT_RGBA8;
    tdm.d.width = tdm.d.height = tdr.d.width = tdr.d.height = 4;
    tdm.d.depth = tdr.d.depth = 1;
    tdm.d.levels = tdr.d.levels = 1;
    tdm.d.samples = 4;
    tdr.d.samples = 1;
    tdm.usage = NVN_GPU_TEX_RENDER;
    tdr.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex tm = NULL, tr = NULL;
    nvn_gpu_view vm = NULL, vr = NULL;
    GPU(nvn_gpu_tex_create(&tdm, &tm, &err));
    GPU(nvn_gpu_tex_create(&tdr, &tr, &err));
    GPU(nvn_gpu_view_create(tm, NULL, &vm, &err));
    GPU(nvn_gpu_view_create(tr, NULL, &vr, &err));
    nvn_gpu_buf rb = NULL;
    uint8_t zero[64] = {0};
    GPU(nvn_gpu_buf_create(sizeof zero, NVN_GPU_MEM_SHARED, zero, &rb, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_pass_desc pass = {0};
    pass.ncolor = 1;
    pass.color[0].view = vm;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.w = pass.h = 4;
    pass.layers = 1;
    nvn_gpu_pass_begin(cmd, &pass);
    nvn_gpu_set_pipe(cmd, pipe);
    set_full_dyn(cmd, 4, 4);
    nvn_gpu_draw_args da = {0};
    da.prim = 4;
    da.count = 3;
    da.instances = 1;
    nvn_gpu_draw(cmd, &da);
    nvn_gpu_pass_end(cmd);

    NVNcopyRegion reg = {0, 0, 0, 4, 4, 1};
    nvn_gpu_blit(cmd, vm, &reg, vr, &reg, 0);
    nvn_gpu_copy_tex_buf(cmd, tr, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    REQUIRE(px);
    check_rgba("case3-msaa-resolve", px, 0, 255, 0, 255, 1);

    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(vr);
    nvn_gpu_view_release(vm);
    nvn_gpu_tex_release(tr);
    nvn_gpu_tex_release(tm);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
}

// Case 4: copy_bufs_uniform (COPY_BIT_KHR -> FS UNIFORM_READ_BIT_KHR) + draw + readback
static void case_copy_bufs_uniform(const char *dir) {
    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].set = 0; ent[0].binding = 0; ent[0].cls = NVN_GPU_RES_UBO; ent[0].access = NVN_GPU_ACC_R; ent[0].count = 1; ent[0].stages = 1u << NVN_GPU_SH_FS;
    ent[1].set = 1; ent[1].binding = 3; ent[1].cls = NVN_GPU_RES_UBO; ent[1].access = NVN_GPU_ACC_R; ent[1].count = 1; ent[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout lay = { .ent = ent, .nent = 2 };
    nvn_gpu_shader vs = load_shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = load_shader(dir, "ubo_dyn.frag", NVN_GPU_SH_FS, &lay);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3;
    pd.prim = 4;
    pd.samples = 1;
    pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));

    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 4;
    td.d.depth = 1;
    td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex rt = NULL;
    nvn_gpu_view rtv = NULL;
    GPU(nvn_gpu_tex_create(&td, &rt, &err));
    GPU(nvn_gpu_view_create(rt, NULL, &rtv, &err));

    uint8_t zero[512] = {0};
    nvn_gpu_buf src_u = NULL, dst_u = NULL, rb = NULL;
    GPU(nvn_gpu_buf_create(512, NVN_GPU_MEM_SHARED, zero, &src_u, &err));
    GPU(nvn_gpu_buf_create(512, NVN_GPU_MEM_SHARED, zero, &dst_u, &err));
    GPU(nvn_gpu_buf_create(64, NVN_GPU_MEM_SHARED, zero, &rb, &err));

    const float ua[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    const float ub[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_buf_write(cmd, src_u, 0, ua, sizeof ua);
    nvn_gpu_buf_write(cmd, src_u, 256, ub, sizeof ub);
    nvn_gpu_buf_copy cps[2] = {
        { src_u, 0, dst_u, 0, sizeof ua },
        { src_u, 256, dst_u, 256, sizeof ub },
    };
    nvn_gpu_copy_bufs_uniform(cmd, cps, 2, 1u << NVN_GPU_SH_FS);

    nvn_gpu_pass_desc pass = {0};
    pass.ncolor = 1;
    pass.color[0].view = rtv;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.w = pass.h = 4;
    pass.layers = 1;
    nvn_gpu_pass_begin(cmd, &pass);
    nvn_gpu_set_pipe(cmd, pipe);
    set_full_dyn(cmd, 4, 4);
    nvn_gpu_binding binds[2] = {{0}};
    binds[0].ent = 0; binds[0].cls = NVN_GPU_RES_UBO; binds[0].u.b.buf = dst_u; binds[0].u.b.off = 0; binds[0].u.b.len = 16;
    binds[1].ent = 1; binds[1].cls = NVN_GPU_RES_UBO; binds[1].u.b.buf = dst_u; binds[1].u.b.off = 256; binds[1].u.b.len = 16;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, binds, 2);
    nvn_gpu_draw_args da = {0};
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(cmd, &da);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    REQUIRE(px);
    check_rgba("case4-copy-bufs-uniform", px, 255, 255, 0, 255, 1);

    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_buf_release(dst_u, NULL, NULL);
    nvn_gpu_buf_release(src_u, NULL, NULL);
    nvn_gpu_view_release(rtv);
    nvn_gpu_tex_release(rt);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
}

// Case 5: compute dispatch -> nvn_gpu_compute_to_graphics (INDEX_INPUT + VERTEX_ATTRIBUTE_INPUT + SHADER_STORAGE_WRITE) -> draw + readback
static void case_compute_to_graphics(const char *dir) {
    nvn_gpu_layout_ent cent = { .set = 0, .binding = 0, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS };
    nvn_gpu_layout clay = { .ent = &cent, .nent = 1 };
    nvn_gpu_shader cs = load_shader(dir, "compute_read_payload.comp", NVN_GPU_SH_CS, &clay);
    nvn_gpu_stage_desc csd = { .sh = cs };
    nvn_gpu_pipe cpipe = NULL;
    GPU(nvn_gpu_cpipe_get(&csd, NVN_GPU_PIPE_SYNC, &cpipe, &err));

    nvn_gpu_layout_ent fent = { .set = 0, .binding = 0, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_R, .count = 1, .stages = 1u << NVN_GPU_SH_FS };
    nvn_gpu_layout flay = { .ent = &fent, .nent = 1 };
    nvn_gpu_shader vs = load_shader(dir, "compute_read_vertex.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = load_shader(dir, "compute_read_buffer.frag", NVN_GPU_SH_FS, &flay);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3;
    pd.prim = 4;
    pd.samples = 1;
    pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F;
    pd.attr[0].offset = 0;
    pd.attr[0].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe gpipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &gpipe, &err));

    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 4;
    td.d.depth = 1;
    td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex rt = NULL;
    nvn_gpu_view rtv = NULL;
    GPU(nvn_gpu_tex_create(&td, &rt, &err));
    GPU(nvn_gpu_view_create(rt, NULL, &rtv, &err));

    uint8_t zero[64] = {0};
    nvn_gpu_buf ssbo = NULL, rb = NULL;
    GPU(nvn_gpu_buf_create(64, NVN_GPU_MEM_SHARED, zero, &ssbo, &err));
    GPU(nvn_gpu_buf_create(64, NVN_GPU_MEM_SHARED, zero, &rb, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_set_pipe(cmd, cpipe);
    nvn_gpu_binding cb = {0};
    cb.ent = 0; cb.cls = NVN_GPU_RES_SSBO; cb.u.b.buf = ssbo; cb.u.b.off = 0; cb.u.b.len = 64;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &cb, 1);
    nvn_gpu_dispatch(cmd, cpipe, 1, 1, 1, NULL, 0);
    nvn_gpu_compute_to_graphics(cmd);

    nvn_gpu_pass_desc pass = {0};
    pass.ncolor = 1;
    pass.color[0].view = rtv;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.w = pass.h = 4;
    pass.layers = 1;
    nvn_gpu_pass_begin(cmd, &pass);
    nvn_gpu_set_pipe(cmd, gpipe);
    set_full_dyn(cmd, 4, 4);
    nvn_gpu_set_vbuf(cmd, 0, ssbo, 0);
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, &cb, 1);
    nvn_gpu_draw_args da = {0};
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(cmd, &da);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    REQUIRE(px);
    check_rgba("case5-compute-to-graphics", px, 0, 255, 0, 255, 1);

    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_buf_release(ssbo, NULL, NULL);
    nvn_gpu_view_release(rtv);
    nvn_gpu_tex_release(rt);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(cs);
}

// Case 6: pass 1 writes color -> pass 2 samples it (exercises SHADER_SAMPLED_READ_BIT_KHR when opt=1)
static void case_render_to_sample(const char *dir) {
    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].cls = NVN_GPU_RES_TEX; ent[0].binding = 0; ent[0].access = NVN_GPU_ACC_R; ent[0].count = 1; ent[0].stages = 1u << NVN_GPU_SH_FS;
    ent[1].cls = NVN_GPU_RES_SAMP; ent[1].binding = 1; ent[1].access = NVN_GPU_ACC_R; ent[1].count = 1; ent[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout lay = { .ent = ent, .nent = 2 };
    nvn_gpu_shader vs = load_shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = load_shader(dir, "feedback.frag", NVN_GPU_SH_FS, &lay);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3;
    pd.prim = 4;
    pd.samples = 1;
    pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));

    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 4;
    td.d.depth = 1;
    td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex t1 = NULL, t2 = NULL;
    nvn_gpu_view v1 = NULL, v2 = NULL;
    GPU(nvn_gpu_tex_create(&td, &t1, &err));
    GPU(nvn_gpu_tex_create(&td, &t2, &err));
    GPU(nvn_gpu_view_create(t1, NULL, &v1, &err));
    GPU(nvn_gpu_view_create(t2, NULL, &v2, &err));
    samp_par sp = {0};
    nvn_gpu_samp samp = NULL;
    GPU(nvn_gpu_samp_create(&sp, &samp, &err));
    nvn_gpu_buf rb = NULL;
    uint8_t zero[64] = {0};
    GPU(nvn_gpu_buf_create(sizeof zero, NVN_GPU_MEM_SHARED, zero, &rb, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    // Pass 1: clear t1 to (0.25, 0.5, 0.0, 1.0)
    nvn_gpu_pass_desc p1 = {0};
    p1.ncolor = 1;
    p1.color[0].view = v1;
    p1.color[0].load = NVN_GPU_LOAD_CLEAR;
    p1.color[0].store = NVN_GPU_STORE_STORE;
    p1.color[0].clear.f[0] = 0.25f;
    p1.color[0].clear.f[1] = 0.5f;
    p1.color[0].clear.f[3] = 1.0f;
    p1.w = p1.h = 4;
    p1.layers = 1;
    nvn_gpu_pass_begin(cmd, &p1);
    nvn_gpu_pass_end(cmd);

    // Pass 2: sample t1 via feedback.frag (adds +0.25 to R -> 0.50, 0.50, 0.0, 1.0)
    nvn_gpu_pass_desc p2 = {0};
    p2.ncolor = 1;
    p2.color[0].view = v2;
    p2.color[0].load = NVN_GPU_LOAD_CLEAR;
    p2.color[0].store = NVN_GPU_STORE_STORE;
    p2.w = p2.h = 4;
    p2.layers = 1;
    nvn_gpu_pass_begin(cmd, &p2);
    nvn_gpu_set_pipe(cmd, pipe);
    set_full_dyn(cmd, 4, 4);
    nvn_gpu_binding b[2] = {{0}};
    b[0].ent = 0; b[0].cls = NVN_GPU_RES_TEX; b[0].u.view = v1;
    b[1].ent = 1; b[1].cls = NVN_GPU_RES_SAMP; b[1].u.samp = samp;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw_args da = {0};
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(cmd, &da);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, t2, 0, 0, NULL, rb, 0, 16, 64);
    submit_and_wait(cmd);

    const uint8_t *px = nvn_gpu_buf_cpu(rb);
    REQUIRE(px);
    check_rgba("case6-render-to-sample", px, 128, 128, 0, 255, 2);

    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_samp_release(samp);
    nvn_gpu_view_release(v2);
    nvn_gpu_view_release(v1);
    nvn_gpu_tex_release(t2);
    nvn_gpu_tex_release(t1);
    nvn_gpu_shader_release(fs);
    nvn_gpu_shader_release(vs);
}

int main(int argc, char **argv) {
    REQUIRE(argc >= 6);
    const char *dir = argv[1];
    g_dq_mode = atoi(argv[2]);
    g_opt_mode = atoi(argv[3]);
    g_sync2_flag = atoi(argv[4]);
    g_hide_ext = atoi(argv[5]);

    struct s2a_vk_device dev = {0};
    char why[192] = "";
    if (s2a_vk_device_create(&dev, why, sizeof why) != 0) {
        fprintf(stderr, "s2a_vk_device_create failed: %s\n", why);
        return 2;
    }

    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    od.app_label = "sync2-contract";
    GPU(nvn_gpu_open(&od, &err));
    obs_reset_barriers();

    case_clear_copy_tex();
    case_upload_blit();
    case_msaa_resolve(dir);
    case_copy_bufs_uniform(dir);
    case_compute_to_graphics(dir);
    case_render_to_sample(dir);

    GPU(nvn_gpu_close(5000000000ull, &err));

    const uint32_t ext_req = atomic_load(&g_ext_requested);
    const uint32_t feat_ch = atomic_load(&g_feat_chained);
    const uint64_t b1 = atomic_load(&g_b1_calls);
    const uint64_t b2 = atomic_load(&g_b2_calls);
    const uint64_t b2_copy = atomic_load(&g_b2_copy);
    const uint64_t b2_clear = atomic_load(&g_b2_clear);
    const uint64_t b2_blit = atomic_load(&g_b2_blit);
    const uint64_t b2_resolve = atomic_load(&g_b2_resolve);
    const uint64_t b2_vi = atomic_load(&g_b2_fine_vi);
    const uint64_t b2_none = atomic_load(&g_b2_none_src);
    const uint64_t b2_samp = atomic_load(&g_b2_sampled_read);
    const uint64_t b2_sw = atomic_load(&g_b2_storage_write);
    const uint64_t b2_bad = atomic_load(&g_b2_bad_none_access);

    const int expect_on = (g_sync2_flag != 0) && (g_hide_ext == 0);
    if (g_hide_ext) {
        if (dev.synchronization2_enabled != 0 || dev.cmd_pipeline_barrier2 != NULL || ext_req != 0 || feat_ch != 0) {
            fprintf(stderr, "FAIL hide=1 handshake: enabled=%d fn=%p ext_req=%u feat_ch=%u\n",
                    dev.synchronization2_enabled, (void *)(uintptr_t)dev.cmd_pipeline_barrier2, ext_req, feat_ch);
            g_failures++;
        }
    } else {
        if (dev.synchronization2_enabled != 1 || dev.cmd_pipeline_barrier2 == NULL || ext_req != 1 || feat_ch != 1) {
            fprintf(stderr, "FAIL hide=0 handshake: enabled=%d fn=%p ext_req=%u feat_ch=%u\n",
                    dev.synchronization2_enabled, (void *)(uintptr_t)dev.cmd_pipeline_barrier2, ext_req, feat_ch);
            g_failures++;
        }
    }

    if (expect_on) {
        if (b1 != 0 || b2 == 0 || b2_copy == 0 || b2_clear == 0 || b2_blit == 0 ||
            b2_resolve == 0 || b2_vi == 0 || b2_none == 0 || b2_sw == 0 || b2_bad != 0) {
            fprintf(stderr, "FAIL sync2 active counts: b1=%llu b2=%llu copy=%llu clear=%llu blit=%llu resolve=%llu vi=%llu none=%llu sw=%llu bad=%llu\n",
                    (unsigned long long)b1, (unsigned long long)b2, (unsigned long long)b2_copy,
                    (unsigned long long)b2_clear, (unsigned long long)b2_blit, (unsigned long long)b2_resolve,
                    (unsigned long long)b2_vi, (unsigned long long)b2_none, (unsigned long long)b2_sw,
                    (unsigned long long)b2_bad);
            g_failures++;
        }
        if (g_opt_mode && b2_samp == 0) {
            fprintf(stderr, "FAIL sync2 opt=1 expected SHADER_SAMPLED_READ_BIT_KHR > 0\n");
            g_failures++;
        }
    } else {
        if (b2 != 0 || b1 == 0) {
            fprintf(stderr, "FAIL sync2 fallback counts: b1=%llu b2=%llu\n",
                    (unsigned long long)b1, (unsigned long long)b2);
            g_failures++;
        }
    }

    printf("sync2 dq=%d opt=%d flag=%d hide=%d enabled=%d ext_req=%u feat_ch=%u b1=%llu b2=%llu "
           "copy=%llu clear=%llu blit=%llu resolve=%llu vi=%llu none=%llu samp=%llu sw=%llu bad=%llu cases=6 failures=%u\n",
           g_dq_mode, g_opt_mode, g_sync2_flag, g_hide_ext, dev.synchronization2_enabled, ext_req, feat_ch,
           (unsigned long long)b1, (unsigned long long)b2, (unsigned long long)b2_copy,
           (unsigned long long)b2_clear, (unsigned long long)b2_blit, (unsigned long long)b2_resolve,
           (unsigned long long)b2_vi, (unsigned long long)b2_none, (unsigned long long)b2_samp,
           (unsigned long long)b2_sw, (unsigned long long)b2_bad, g_failures);

    REQUIRE(s2a_vk_device_destroy(&dev, 0) == 0);
    return g_failures ? 1 : 0;
}
