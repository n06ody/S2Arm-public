// VK_EXT_host_query_reset 合同测试（Mesa 26.0.8 lavapipe + VVL 1.4.341 + SyncVal，DQ=0/1）。
// 验证（全程真实驱动与真实查询结果，零伪造）：
//  1) mode=on：Vulkan 1.1 运行时协商 VK_EXT_host_query_reset + hostQueryReset 特性 +
//     vkGetDeviceProcAddr("vkResetQueryPoolEXT")（零静态 1.2 符号）。
//     - 通道内调 nvn_gpu_timestamp：首次建 report_ts 当场主机重置，提交段跳过独立前置缓冲 init_cb
//       与 ALL_COMMANDS→ALL_COMMANDS 全管线屏障（无新图转布局时 vkQueueSubmit 仅 1 条 CB）；
//     - 连续两段遮挡查询（覆盖新建池与 seg_take_q 回收复用的池）+ nvk_gputime 时间戳池均走主机重置：
//       vkCmdResetQueryPool == 0，vkResetQueryPoolEXT >= 6，遮挡样本数精确等于 256 / 0，时间戳单调有效，
//       RGBA 读回逐字节一致，VVL + SyncVal 0 错。
//  2) mode=off / hide-ext / hide-feat / hide-proc：干净退回 vkCmdResetQueryPool + init_cb 路径，
//     vkResetQueryPoolEXT == 0，遮挡样本数、时间戳与像素完全一致，VVL + SyncVal 0 错。
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
extern PFN_vkVoidFunction __real_vkGetDeviceProcAddr(VkDevice, const char *);
extern void __real_vkCmdResetQueryPool(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t);
extern VkResult __real_vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo *, VkFence);

static int g_hide_ext, g_hide_feat, g_hide_proc, g_flag_off, g_dq_on;
static unsigned g_dev_ext_enabled, g_dev_feat_enabled;
static unsigned g_host_resets, g_cmd_resets;
static unsigned g_submits_1cb, g_submits_2cb, g_observe_submits;
static PFN_vkResetQueryPoolEXT g_real_reset_query_pool;
static PFN_vkCmdResetQueryPool g_real_cmd_reset_query_pool;

static VKAPI_ATTR void VKAPI_CALL spy_vkResetQueryPoolEXT(VkDevice dev, VkQueryPool pool, uint32_t first, uint32_t count) {
    __atomic_fetch_add(&g_host_resets, 1, __ATOMIC_RELAXED);
    g_real_reset_query_pool(dev, pool, first, count);
}

static VKAPI_ATTR void VKAPI_CALL spy_vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t count) {
    __atomic_fetch_add(&g_cmd_resets, 1, __ATOMIC_RELAXED);
    g_real_cmd_reset_query_pool(cb, pool, first, count);
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
        if (!strcmp(tmp[i].extensionName, VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME)) continue;
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
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES_EXT)
            ((VkPhysicalDeviceHostQueryResetFeaturesEXT *)b)->hostQueryReset = VK_FALSE;
    }
}

VkResult __wrap_vkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *out) {
    if (ci) {
        for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
            if (!strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME))
                g_dev_ext_enabled++;
        for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
            if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES_EXT &&
                ((const VkPhysicalDeviceHostQueryResetFeaturesEXT *)b)->hostQueryReset == VK_TRUE)
                g_dev_feat_enabled++;
        }
    }
    return __real_vkCreateDevice(phys, ci, alloc, out);
}

PFN_vkVoidFunction __wrap_vkGetDeviceProcAddr(VkDevice dev, const char *name) {
    if (name && !strcmp(name, "vkResetQueryPoolEXT")) {
        if (g_hide_proc) return NULL;
        PFN_vkVoidFunction fn = __real_vkGetDeviceProcAddr(dev, name);
        if (!fn) return NULL;
        g_real_reset_query_pool = (PFN_vkResetQueryPoolEXT)fn;
        return (PFN_vkVoidFunction)spy_vkResetQueryPoolEXT;
    }
    if (name && !strcmp(name, "vkCmdResetQueryPool")) {
        PFN_vkVoidFunction fn = __real_vkGetDeviceProcAddr(dev, name);
        if (!fn) return NULL;
        g_real_cmd_reset_query_pool = (PFN_vkCmdResetQueryPool)fn;
        return (PFN_vkVoidFunction)spy_vkCmdResetQueryPool;
    }
    return __real_vkGetDeviceProcAddr(dev, name);
}

void __wrap_vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t count) {
    __atomic_fetch_add(&g_cmd_resets, 1, __ATOMIC_RELAXED);
    __real_vkCmdResetQueryPool(cb, pool, first, count);
}

VkResult __wrap_vkQueueSubmit(VkQueue q, uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
    if (__atomic_load_n(&g_observe_submits, __ATOMIC_ACQUIRE) && submits) {
        for (uint32_t i = 0; i < count; i++) {
            if (submits[i].commandBufferCount == 1) __atomic_fetch_add(&g_submits_1cb, 1, __ATOMIC_RELAXED);
            else if (submits[i].commandBufferCount >= 2) __atomic_fetch_add(&g_submits_2cb, 1, __ATOMIC_RELAXED);
        }
    }
    return __real_vkQueueSubmit(q, count, submits, fence);
}

int nvn_plat_flag(const char *name, int dflt) {
    if (!name) return dflt;
    if (!strcmp(name, "nvk_host_query_reset")) return g_flag_off ? 0 : dflt;
    if (!strcmp(name, "nvk_gputime")) return 1;
    if (!strcmp(name, "nvk_dq")) return g_dq_on;
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

struct done_rec {
    uint32_t status, nts, nocc;
    uint64_t occ[4];
    nvn_gpu_timestamp_result ts[4];
};

static void on_done(void *user, const nvn_gpu_done *d) {
    struct done_rec *r = (struct done_rec *)user;
    r->status = d->status;
    r->nocc = d->nocclusion < 4 ? d->nocclusion : 4;
    for (uint32_t i = 0; i < r->nocc; i++) r->occ[i] = d->occlusion[i];
    r->nts = d->ntimestamps < 4 ? d->ntimestamps : 4;
    for (uint32_t i = 0; i < r->nts; i++) r->ts[i] = d->timestamps[i];
}

struct hqr_stats {
    uint64_t host_resets, cmd_resets, init_cb_saved;
    int seen;
};

static void stats_cb(void *u, const char *name, uint64_t v) {
    struct hqr_stats *s = (struct hqr_stats *)u;
    if (!strcmp(name, "query_host_resets")) { s->host_resets = v; s->seen++; }
    else if (!strcmp(name, "query_cmd_resets")) { s->cmd_resets = v; s->seen++; }
    else if (!strcmp(name, "query_init_cb_saved")) { s->init_cb_saved = v; s->seen++; }
}

static int run_query_seg(nvn_gpu_tex tex, nvn_gpu_view view, nvn_gpu_buf rb, nvn_gpu_pipe pipe,
                         uint64_t signal, int has_ts_cap, struct done_rec *out_rec, uint8_t rgba[BYTES]) {
    nvn_gpu_err err = {0};
    nvn_gpu_cmd_desc cd = {.occlusion_slots = 4};
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(&cd, &cmd, &err));
    nvn_gpu_pass_desc pass = {
        .ncolor = 1,
        .color = {{.view = view, .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE}},
        .w = W, .h = H, .layers = 1, .occlusion = 1,
    };
    pass.color[0].clear.f[3] = 1.0f;
    nvn_gpu_pass_begin(cmd, &pass);
    // 通道内首用时间戳：没开 host_query_reset 时必须建独立 init_cb + ALL_COMMANDS 屏障；开启后主机重置零 init_cb
    if (has_ts_cap) nvn_gpu_timestamp(cmd, 0, NVN_COUNTER_TYPE_TIMESTAMP_TOP);
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_dyn dyn = {.nvp = 1, .vp = {{0, 0, (float)W, (float)H, 0.0f, 1.0f}}, .has_sc = 1, .sc = {0, 0, W, H}, .origin_ll = 1};
    nvn_gpu_set_dyn(cmd, &dyn);
    nvn_gpu_draw_args draw = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1};
    // 槽 0：全屏绿绘制 → 精确 16*16=256 样本
    nvn_gpu_occlusion(cmd, 0);
    nvn_gpu_draw(cmd, &draw);
    // 槽 1：空绘制（无 draw）→ 精确 0 样本
    nvn_gpu_occlusion(cmd, 1);
    nvn_gpu_occlusion(cmd, -1);
    if (has_ts_cap) nvn_gpu_timestamp(cmd, 1, NVN_COUNTER_TYPE_TIMESTAMP);
    nvn_gpu_pass_end(cmd);
    NVNcopyRegion reg = {0, 0, 0, W, H, 1};
    nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, &reg, rb, 0, W * 4, BYTES);
    memset(out_rec, 0, sizeof *out_rec);
    GPU(nvn_gpu_cmd_commit(cmd, signal, on_done, out_rec, &err));
    GPU(nvn_gpu_fence_wait(signal, 5000000000ull, &err));
    memcpy(rgba, nvn_gpu_buf_cpu(rb), BYTES);
    return 0;
}

static int check_solid(const uint8_t rgba[BYTES], uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (unsigned i = 0; i < W * H; i++) {
        const uint8_t *p = rgba + i * 4;
        if (p[0] != r || p[1] != g || p[2] != b || p[3] != a) {
            fprintf(stderr, "【红】pixel mismatch at %u: want=%02x%02x%02x%02x got=%02x%02x%02x%02x\n",
                    i, r, g, b, a, p[0], p[1], p[2], p[3]);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    // argv: <shader_dir> <mode: on|off|hide-ext|hide-feat|hide-proc|withdraw> <dq: 0|1>
    if (argc != 4) {
        fprintf(stderr, "usage: %s <shader_dir> <mode> <dq>\n", argv[0]);
        return 2;
    }
    const char *sdir = argv[1];
    const char *mode = argv[2];
    g_dq_on = atoi(argv[3]) != 0;
    const int is_withdraw = !strcmp(mode, "withdraw");
    if (!strcmp(mode, "off") || is_withdraw) {
        g_flag_off = 1;
        setenv("debug.s2a.nvk_host_query_reset", "0", 1);
    } else {
        unsetenv("debug.s2a.nvk_host_query_reset");
        if (!strcmp(mode, "hide-ext")) g_hide_ext = 1;
        else if (!strcmp(mode, "hide-feat")) g_hide_feat = 1;
        else if (!strcmp(mode, "hide-proc")) g_hide_proc = 1;
        else if (strcmp(mode, "on") != 0) return 2;
    }
    const int expect_on = !strcmp(mode, "on") || is_withdraw;

    char vp[512], fp[512];
    snprintf(vp, sizeof vp, "%s/fullscreen.vert.spv", sdir);
    snprintf(fp, sizeof fp, "%s/solid_green.frag.spv", sdir);
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

    nvn_gpu_caps caps = {0};
    nvn_gpu_caps_get(&caps);
    const int has_ts_cap = (caps.caps & NVN_GPU_CAP_REPORT_TIMESTAMP) != 0;

    static const nvn_gpu_layout empty_layout = {0};
    nvn_gpu_shader vs = NULL, fs = NULL;
    nvn_gpu_shader_src vsd = {.stage = NVN_GPU_SH_VS, .kind = NVN_GPU_BLOB_SPIRV, .blob = vspv, .bytes = vlen, .entry = "main", .layout = &empty_layout};
    nvn_gpu_shader_src fsd = {.stage = NVN_GPU_SH_FS, .kind = NVN_GPU_BLOB_SPIRV, .blob = fspv, .bytes = flen, .entry = "main", .layout = &empty_layout};
    GPU(nvn_gpu_shader_load(&vsd, &vs, &err));
    GPU(nvn_gpu_shader_load(&fsd, &fs, &err));

    nvn_gpu_pipe_desc pd = {0};
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.prim = NVN_DRAW_PRIMITIVE_TRIANGLES;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.samples = 1;
    pd.origin_ll = 1;
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3; // COPY
    pd.chmask.mask = 0xFu;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));

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

    // 段 1：预热图像首次转布局（g_newimg=1 会走 init_cb）
    struct done_rec r1 = {0}, r2 = {0}, r3 = {0};
    uint8_t img1[BYTES], img2[BYTES], img3[BYTES];
    REQUIRE(run_query_seg(tex, view, rb, pipe, 1, has_ts_cap, &r1, img1) == 0);
    REQUIRE(r1.status == NVN_GPU_DONE_OK && r1.nocc >= 2 && r1.occ[0] == W * H && r1.occ[1] == 0);
    REQUIRE(check_solid(img1, 0, 255, 0, 255) == 0);

    // 段 2 & 段 3：图像已完成首次转布局（n_newimg=0），且段 2/3 复用 seg_take_q 回收的遮挡查询池。
    // 观测这两段提交的 CB 数量（host_query_reset 开启时 report_ts 不再逼出 init_cb + ALL_COMMANDS 屏障）
    __atomic_store_n(&g_observe_submits, 1, __ATOMIC_RELEASE);
    REQUIRE(run_query_seg(tex, view, rb, pipe, 2, has_ts_cap, &r2, img2) == 0);
    REQUIRE(run_query_seg(tex, view, rb, pipe, 3, has_ts_cap, &r3, img3) == 0);
    __atomic_store_n(&g_observe_submits, 0, __ATOMIC_RELEASE);

    REQUIRE(r2.status == NVN_GPU_DONE_OK && r2.nocc >= 2 && r2.occ[0] == W * H && r2.occ[1] == 0);
    REQUIRE(r3.status == NVN_GPU_DONE_OK && r3.nocc >= 2 && r3.occ[0] == W * H && r3.occ[1] == 0);
    REQUIRE(memcmp(img1, img2, BYTES) == 0 && memcmp(img1, img3, BYTES) == 0);
    if (has_ts_cap) {
        REQUIRE(r2.nts >= 2 && r2.ts[0].valid && r2.ts[1].valid && r2.ts[1].host_ns >= r2.ts[0].host_ns);
        REQUIRE(r3.nts >= 2 && r3.ts[0].valid && r3.ts[1].valid && r3.ts[1].host_ns >= r3.ts[0].host_ns);
    }

    struct hqr_stats st = {0};
    nvn_gpu_stats(stats_cb, &st, 0);

    const unsigned host_resets = __atomic_load_n(&g_host_resets, __ATOMIC_RELAXED);
    const unsigned cmd_resets = __atomic_load_n(&g_cmd_resets, __ATOMIC_RELAXED);
    const unsigned sub1 = __atomic_load_n(&g_submits_1cb, __ATOMIC_RELAXED);
    const unsigned sub2 = __atomic_load_n(&g_submits_2cb, __ATOMIC_RELAXED);

    printf("host-query-reset mode=%s dq=%d ts_cap=%d host_resets=%u cmd_resets=%u s23_1cb=%u s23_2cb=%u dev_ext=%u dev_feat=%u stats=%llu/%llu/%llu\n",
           mode, g_dq_on, has_ts_cap, host_resets, cmd_resets, sub1, sub2,
           g_dev_ext_enabled, g_dev_feat_enabled,
           (unsigned long long)st.host_resets, (unsigned long long)st.cmd_resets, (unsigned long long)st.init_cb_saved);

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
        const unsigned min_host = has_ts_cap ? 9u : 6u; // 3 段 × (qpool + tsq [+ report_ts])
        if (g_dev_ext_enabled != 1 || g_dev_feat_enabled != 1 || host_resets < min_host || cmd_resets != 0 ||
            sub1 != 2 || sub2 != 0 || st.init_cb_saved != (has_ts_cap ? sub1 : 0) ||
            st.host_resets < min_host || st.cmd_resets != 0) {
            fprintf(stderr, "【红】host_query_reset missed expectation: ext=%u feat=%u host=%u cmd=%u 1cb=%u 2cb=%u saved=%llu\n",
                    g_dev_ext_enabled, g_dev_feat_enabled, host_resets, cmd_resets, sub1, sub2, (unsigned long long)st.init_cb_saved);
            return 1;
        }
    } else {
        REQUIRE(host_resets == 0);
        REQUIRE(cmd_resets >= (has_ts_cap ? 9u : 6u));
        REQUIRE(st.init_cb_saved == 0);
        if (has_ts_cap) REQUIRE(sub2 == 2 && sub1 == 0);
        if (g_hide_ext || g_hide_feat || g_flag_off) {
            REQUIRE(g_dev_ext_enabled == 0 && g_dev_feat_enabled == 0);
        }
    }
    return 0;
}
