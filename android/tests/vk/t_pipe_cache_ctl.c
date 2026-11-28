// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// VK_EXT_pipeline_creation_cache_control 合同测试（Mesa lavapipe + VVL + SyncVal）。
// 验证：
//  1) 冷盘 VkPipelineCache：nvn_vk_pipe_demand_kick 用 FAIL_ON_PIPELINE_COMPILE_REQUIRED 零阻塞探测，
//     驱动回 VK_PIPELINE_COMPILE_REQUIRED（前台 0 编译），顺滑交后台线程编译并落盘 VkPipelineCache。
//  2) 重开新会话加载同一盘上 VkPipelineCache（内存管线表全空）：
//     - 启用 cache_control（mode=on）：nvn_vk_pipe_demand_kick 直接命中盘上缓存回 NVK_DEMAND_READY
//       （0 唤醒后台、0 跳笔、0 同步编译、像素逐字节一致）；同会话里另建一条未缓存管线仍回 PENDING 走后台。
//     - 关闭/隐藏扩展/隐藏特性/关管线缓存（mode=off/hide-ext/hide-feat/no-plc）：绝不传 FAIL_ON_COMPILE 标志，
//       退回原后台路径，像素逐字节一致，VVL + SyncVal 0 错。
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "nvn_backend.h"
#include "nvn_plat.h"
#include "nvn_vk_pipe_demand.h"
#include "rp_shader.h"
#include "vk_device.h"

extern void nvn_vk_pipe_cache_ctl_stats(uint64_t out[3]) __attribute__((weak));
extern uint64_t rp_sync_pipelines_created(void);
extern uint64_t nvn_vk_dbg_pipe_stat(int which);

// --wrap 钩子：观测真实驱动交互 & 构造扩展/特性隐藏负控。
// 说明：Mesa 26.0.8 lavapipe 的 VkPipelineCache 为占位实现（只落盘 32B 头，不缓存图形管线编译产物，
// 实测同会话同 SPIR-V 亦回 VK_PIPELINE_COMPILE_REQUIRED）。因此：
//  - 冷路径（Session 1 首次、Session 2 未缓存配方 pd_b）与所有负控臂完全保留真实 lavapipe 返回值；
//  - 仅当 Session 1 带非空 VkPipelineCache 成功编译并落盘、且 Session 2 真实加载到盘上缓存
//    （pipe_cache_loaded_bytes > 0）后，生产代码真实传了 FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT
//    探测已落盘配方时，先原样过真实 lavapipe + VVL 校验，再去掉探测位让 lavapipe 生成真实 VkPipeline
//    模拟真实 GPU 驱动（Adreno/Mali）的盘上缓存命中（口径同 t_backend_vk.c inspect_creation_feedback）。
extern VkResult __real_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *);
extern void __real_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
extern VkResult __real_vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
extern VkResult __real_vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *);

static int g_hide_ext, g_hide_feat, g_flag_off, g_no_plc, g_serial_mode;
static int g_combo, g_combo_dq;
static unsigned g_push_calls;
static PFN_vkCmdPushDescriptorSetKHR real_push;
static unsigned g_probe_calls, g_probe_hits, g_probe_misses, g_full_builds, g_dev_ext_enabled, g_dev_feat_enabled;
static uint32_t g_cached_mask_bits;
static int g_disk_cache_loaded;
static pthread_mutex_t g_gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gate_cv = PTHREAD_COND_INITIALIZER;
static uint32_t g_hold_mask_bit;
static int g_hold_active, g_hold_entered;

static VKAPI_ATTR void VKAPI_CALL record_push(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout,
    uint32_t set, uint32_t count, const VkWriteDescriptorSet *writes) {
    if (point == VK_PIPELINE_BIND_POINT_GRAPHICS)
        __atomic_fetch_add(&g_push_calls, 1, __ATOMIC_RELAXED);
    real_push(cb, point, layout, set, count, writes);
}

extern __typeof__(vkGetDeviceProcAddr) __real_vkGetDeviceProcAddr;
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice dev, const char *name) {
    PFN_vkVoidFunction proc = __real_vkGetDeviceProcAddr(dev, name);
    if (g_combo && proc && !strcmp(name, "vkCmdPushDescriptorSetKHR")) {
        real_push = (PFN_vkCmdPushDescriptorSetKHR)proc;
        return (PFN_vkVoidFunction)record_push;
    }
    return proc;
}

static uint32_t pipe_mask_bit(const VkGraphicsPipelineCreateInfo *info) {
    if (!info || !info->pColorBlendState || info->pColorBlendState->attachmentCount < 1 ||
        !info->pColorBlendState->pAttachments)
        return 0;
    const uint32_t m = info->pColorBlendState->pAttachments[0].colorWriteMask & 0xFu;
    return 1u << m;
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
        if (!strcmp(tmp[i].extensionName, VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME)) continue;
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
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT)
            ((VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT *)b)->pipelineCreationCacheControl = VK_FALSE;
    }
}

VkResult __wrap_vkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *out) {
    if (ci) {
        for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
            if (!strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME))
                g_dev_ext_enabled++;
        for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
            if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT &&
                ((const VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT *)b)->pipelineCreationCacheControl == VK_TRUE)
                g_dev_feat_enabled++;
        }
    }
    return __real_vkCreateDevice(phys, ci, alloc, out);
}

VkResult __wrap_vkCreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t count,
                                          const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *alloc, VkPipeline *pipes) {
    int is_probe = 0;
    for (uint32_t i = 0; i < count; i++)
        if (infos[i].flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT) is_probe = 1;
    if (!is_probe && count == 1 && g_hold_mask_bit && pipe_mask_bit(&infos[0]) == g_hold_mask_bit) {
        pthread_mutex_lock(&g_gate_mu);
        while (g_hold_active) {
            g_hold_entered = 1;
            pthread_cond_broadcast(&g_gate_cv);
            pthread_cond_wait(&g_gate_cv, &g_gate_mu);
        }
        pthread_mutex_unlock(&g_gate_mu);
    }
    VkResult r = __real_vkCreateGraphicsPipelines(dev, cache, count, infos, alloc, pipes);
    if (is_probe) {
        if (r == VK_PIPELINE_COMPILE_REQUIRED && cache != VK_NULL_HANDLE &&
            __atomic_load_n(&g_disk_cache_loaded, __ATOMIC_ACQUIRE) && count == 1) {
            const uint32_t bit = pipe_mask_bit(&infos[0]);
            if (bit && (__atomic_load_n(&g_cached_mask_bits, __ATOMIC_ACQUIRE) & bit)) {
                VkGraphicsPipelineCreateInfo ci = infos[0];
                ci.flags &= ~(VkPipelineCreateFlags)VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT;
                r = __real_vkCreateGraphicsPipelines(dev, cache, 1, &ci, alloc, pipes);
            }
        }
        g_probe_calls++;
        if (r == VK_SUCCESS) g_probe_hits++;
        else if (r == VK_PIPELINE_COMPILE_REQUIRED) g_probe_misses++;
    } else if (r == VK_SUCCESS) {
        g_full_builds++;
        if (cache != VK_NULL_HANDLE) {
            for (uint32_t i = 0; i < count; i++) {
                const uint32_t bit = pipe_mask_bit(&infos[i]);
                if (bit) __atomic_fetch_or(&g_cached_mask_bits, bit, __ATOMIC_RELEASE);
            }
        }
    }
    return r;
}

static void loaded_bytes_cb(void *u, const char *name, uint64_t v) {
    if (!strcmp(name, "pipe_cache_loaded_bytes")) *(uint64_t *)u = v;
}

// 平台桩：控制 debug.s2a.* 开关
int nvn_plat_flag(const char *name, int dflt) {
    if (!name) return dflt;
    if (!strcmp(name, "nvk_pipe_cache_ctl")) return g_flag_off ? 0 : dflt;
    if (!strcmp(name, "nvk_pipe_cache")) return g_no_plc ? 0 : dflt;
    if (!strcmp(name, "nvk_pipe_serial")) return g_serial_mode ? 1 : 0;
    if (!strcmp(name, "nvk_pipe_async")) return 1;
    if (g_combo && !strcmp(name, "nvk_pipe_limit")) return 1;
    if (g_combo && !strcmp(name, "nvk_push_desc")) return 1;
    if (g_combo && !strcmp(name, "nvk_dq")) return g_combo_dq;
    return dflt;
}
void nvn_plat_log(int level, const char *cls, const char *text) {
    (void)level; (void)cls; (void)text;
}

static struct rp_prog g_prog_a, g_prog_b, g_prog_c;
static uint8_t g_ident_a[32], g_ident_b[32], g_ident_c[32];

int rp_shader_prog_borrow(const uint8_t identity[32], const struct rp_prog **out) {
    if (!identity || !out) return -1;
    if (!memcmp(identity, g_ident_a, 32)) { *out = &g_prog_a; return 1; }
    if (!memcmp(identity, g_ident_b, 32)) { *out = &g_prog_b; return 1; }
    if (!memcmp(identity, g_ident_c, 32)) { *out = &g_prog_c; return 1; }
    return 0;
}
void rp_shader_prog_return(const uint8_t identity[32]) { (void)identity; }

struct serial_kick_arg {
    const nvn_gpu_pipe_desc *desc;
    const uint8_t *ident;
    int rc;
    nvn_gpu_pipe pipe;
    int done;
    pthread_mutex_t mu;
    pthread_cond_t cv;
};
static void *serial_kick_main(void *opaque) {
    struct serial_kick_arg *a = opaque;
    a->rc = nvn_vk_pipe_demand_kick(a->desc, a->ident, &a->pipe);
    pthread_mutex_lock(&a->mu);
    a->done = 1;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
    return NULL;
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

static int draw_and_readback(nvn_gpu_pipe pipe, uint64_t signal, uint8_t rgba_out[BYTES]) {
    nvn_gpu_err err = {0};
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
    nvn_gpu_buf ubo = NULL;
    const float green[] = {0, 1, 0, 1};
    if (g_combo) GPU(nvn_gpu_buf_create(sizeof green, NVN_GPU_MEM_SHARED, green, &ubo, &err));
    const unsigned pushes = __atomic_load_n(&g_push_calls, __ATOMIC_RELAXED);

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_pass_desc pass = {.ncolor = 1, .color = {{.view = view, .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE}}, .w = W, .h = H, .layers = 1};
    pass.color[0].clear.f[3] = 1.0f;
    nvn_gpu_pass_begin(cmd, &pass);
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_dyn dyn = {.nvp = 1, .vp = {{0, 0, (float)W, (float)H, 0.0f, 1.0f}}, .has_sc = 1, .sc = {0, 0, W, H}, .origin_ll = 1};
    nvn_gpu_set_dyn(cmd, &dyn);
    if (g_combo) {
        const nvn_gpu_binding binding = {.ent = 0, .cls = NVN_GPU_RES_UBO, .u.b = {.buf = ubo, .len = sizeof green}};
        nvn_gpu_bind(cmd, NVN_GPU_SH_FS, &binding, 1);
    }
    nvn_gpu_draw_args draw = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1};
    nvn_gpu_draw(cmd, &draw);
    nvn_gpu_pass_end(cmd);
    NVNcopyRegion reg = {0, 0, 0, W, H, 1};
    nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, &reg, rb, 0, W * 4, BYTES);
    GPU(nvn_gpu_cmd_commit(cmd, signal, NULL, NULL, &err));
    GPU(nvn_gpu_fence_wait(signal, 5000000000ull, &err));
    memcpy(rgba_out, nvn_gpu_buf_cpu(rb), BYTES);
    if (g_combo) {
        REQUIRE(__atomic_load_n(&g_push_calls, __ATOMIC_RELAXED) == pushes + 1);
        nvn_gpu_buf_release(ubo, NULL, NULL);
    }

    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(view);
    nvn_gpu_tex_release(tex);
    return 0;
}

static nvn_gpu_pipe_desc make_desc(nvn_gpu_shader vs, nvn_gpu_shader fs, uint32_t chmask) {
    nvn_gpu_pipe_desc d = {0};
    d.st[NVN_GPU_SH_VS].sh = vs;
    d.st[NVN_GPU_SH_FS].sh = fs;
    d.prim = NVN_DRAW_PRIMITIVE_TRIANGLES;
    d.cfmt[0] = NVN_FORMAT_RGBA8;
    d.samples = 1;
    d.origin_ll = 1;
    d.poly.mode = NVN_POLYGON_MODE_FILL;
    d.color.logic_op = 3; // COPY
    d.chmask.mask = chmask;
    return d;
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
    // argv: <shader_dir> <cache_dir> <mode: on|off|hide-ext|hide-feat|no-plc|withdraw|combo|combo-dq>
    if (argc != 4) {
        fprintf(stderr, "usage: %s <shader_dir> <cache_dir> <mode>\n", argv[0]);
        return 2;
    }
    const char *sdir = argv[1];
    const char *cdir = argv[2];
    const char *mode = argv[3];
    const int is_withdraw = !strcmp(mode, "withdraw");
    g_combo_dq = !strcmp(mode, "combo-dq");
    g_combo = g_combo_dq || !strcmp(mode, "combo");
    const int is_serial_busy = !strcmp(mode, "serial-busy");
    if (!strcmp(mode, "off") || is_withdraw) {
        g_flag_off = 1;
        setenv("debug.s2a.nvk_pipe_cache_ctl", "0", 1);
    } else {
        unsetenv("debug.s2a.nvk_pipe_cache_ctl");
        if (!strcmp(mode, "hide-ext")) g_hide_ext = 1;
        else if (!strcmp(mode, "hide-feat")) g_hide_feat = 1;
        else if (!strcmp(mode, "no-plc")) g_no_plc = 1;
        else if (is_serial_busy) g_serial_mode = 1;
        else if (!g_combo && strcmp(mode, "on") != 0) return 2;
    }

    const int expect_probe_active = !strcmp(mode, "on") || g_combo || is_serial_busy;
    const int expect_ctl = expect_probe_active || is_withdraw;

    char vp[512], fp[512];
    snprintf(vp, sizeof vp, "%s/tri3.vert.spv", sdir);
    snprintf(fp, sizeof fp, "%s/%s", sdir, g_combo ? "ubo.frag.spv" : "solid_green.frag.spv");
    void *vspv = NULL, *fspv = NULL;
    size_t vlen = 0, flen = 0;
    REQUIRE(load_spv(vp, &vspv, &vlen) && load_spv(fp, &fspv, &flen));
    memset(g_ident_a, 0xA1, 32);
    memset(g_ident_b, 0xB2, 32);
    memset(g_ident_c, 0xC4, 32);

    // ---- Session 1: 冷盘 VkPipelineCache，首用经 demand 后台编译并落盘 ----
    struct s2a_vk_device dev1 = {0};
    char why[256] = "";
    REQUIRE(s2a_vk_device_create(&dev1, why, sizeof why) == 0);
    nvn_gpu_open_desc od = {0};
    od.host_device = &dev1;
    od.cache_dir = cdir;
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&od, &err));
    nvn_vk_pipe_demand_start();

    static const nvn_gpu_layout empty_layout = {0};
    static const nvn_gpu_layout_ent color = {
        .set = 0, .binding = 0, .cls = NVN_GPU_RES_UBO, .count = 1, .access = NVN_GPU_ACC_R, .stages = 1u << NVN_GPU_SH_FS,
    };
    static const nvn_gpu_layout color_layout = {.ent = &color, .nent = 1};
    nvn_gpu_shader vs1 = NULL, fs1 = NULL;
    nvn_gpu_shader_src vsd = {.stage = NVN_GPU_SH_VS, .kind = NVN_GPU_BLOB_SPIRV, .blob = vspv, .bytes = vlen, .entry = "main", .layout = &empty_layout};
    nvn_gpu_shader_src fsd = {.stage = NVN_GPU_SH_FS, .kind = NVN_GPU_BLOB_SPIRV, .blob = fspv, .bytes = flen,
                             .entry = "main", .layout = g_combo ? &color_layout : &empty_layout};
    GPU(nvn_gpu_shader_load(&vsd, &vs1, &err));
    GPU(nvn_gpu_shader_load(&fsd, &fs1, &err));
    memset(&g_prog_a, 0, sizeof g_prog_a);
    g_prog_a.sh[NVN_GPU_SH_VS] = vs1;
    g_prog_a.sh[NVN_GPU_SH_FS] = fs1;

    nvn_gpu_pipe_desc pd_a1 = make_desc(vs1, fs1, 0xFu);
    nvn_gpu_pipe p1 = NULL;
    int k1 = nvn_vk_pipe_demand_kick(&pd_a1, g_ident_a, &p1);
    REQUIRE(k1 == NVK_DEMAND_PENDING && !p1);
    char wwhy[128] = "";
    REQUIRE(nvn_vk_pipe_demand_wait(&pd_a1, g_ident_a, 10000, &p1, wwhy, sizeof wwhy) == 0 && p1 != NULL);
    uint8_t img1[BYTES];
    REQUIRE(draw_and_readback(p1, 1, img1) == 0);
    REQUIRE(check_solid(img1, 0, 255, 0, 255) == 0);

    const unsigned s1_probes = g_probe_calls, s1_misses = g_probe_misses, s1_hits = g_probe_hits;
    nvn_vk_pipe_demand_stop();
    nvn_gpu_shader_release(vs1);
    nvn_gpu_shader_release(fs1);
    GPU(nvn_gpu_close(5000000000ull, &err));
    s2a_vk_device_destroy(&dev1, 1);

    // ---- Session 2: 重开新会话加载同一盘上 VkPipelineCache（内存表全空） ----
    g_probe_calls = g_probe_hits = g_probe_misses = g_full_builds = 0;
    struct s2a_vk_device dev2 = {0};
    REQUIRE(s2a_vk_device_create(&dev2, why, sizeof why) == 0);
    if (g_combo) REQUIRE(dev2.eds1_enabled && dev2.push_descriptor_enabled);
    od.host_device = &dev2;
    GPU(nvn_gpu_open(&od, &err));
    uint64_t s2_loaded = 0;
    nvn_gpu_stats(loaded_bytes_cb, &s2_loaded, 0);
    if (g_no_plc) REQUIRE(s2_loaded == 0);
    else REQUIRE(s2_loaded > 0);
    __atomic_store_n(&g_disk_cache_loaded, s2_loaded > 0 ? 1 : 0, __ATOMIC_RELEASE);
    nvn_vk_pipe_demand_start();

    nvn_gpu_shader vs2 = NULL, fs2 = NULL;
    GPU(nvn_gpu_shader_load(&vsd, &vs2, &err));
    GPU(nvn_gpu_shader_load(&fsd, &fs2, &err));
    memset(&g_prog_a, 0, sizeof g_prog_a);
    g_prog_a.sh[NVN_GPU_SH_VS] = vs2;
    g_prog_a.sh[NVN_GPU_SH_FS] = fs2;
    g_prog_b = g_prog_a;
    g_prog_c = g_prog_a;

    struct nvn_pipe_demand_stats st0 = {0};
    nvn_vk_pipe_demand_stats(&st0);
    const uint64_t sync_before = rp_sync_pipelines_created();

    nvn_gpu_pipe_desc pd_a2 = make_desc(vs2, fs2, 0xFu);
    nvn_gpu_pipe look = NULL;
    REQUIRE(nvn_gpu_pipe_lookup(&pd_a2, &look) == 0 && !look); // 内存表确认全空

    nvn_gpu_pipe p2 = NULL;
    int k2 = nvn_vk_pipe_demand_kick(&pd_a2, g_ident_a, &p2);
    struct nvn_pipe_demand_stats st1 = {0};
    nvn_vk_pipe_demand_stats(&st1);
    const uint64_t sync_after_a = rp_sync_pipelines_created();

    if (k2 == NVK_DEMAND_PENDING) {
        REQUIRE(nvn_vk_pipe_demand_wait(&pd_a2, g_ident_a, 10000, &p2, wwhy, sizeof wwhy) == 0 && p2 != NULL);
    }
    REQUIRE(p2 != NULL);
    uint8_t img2[BYTES];
    REQUIRE(draw_and_readback(p2, 1, img2) == 0);
    REQUIRE(memcmp(img1, img2, BYTES) == 0);
    nvn_gpu_pipe_desc alias_desc = pd_a2;
    nvn_gpu_pipe alias = NULL;
    if (g_combo) {
        alias_desc.poly.front ^= 1;
        const uint64_t creates = nvn_vk_dbg_pipe_stat(5);
        REQUIRE(nvn_gpu_pipe_lookup(&alias_desc, &alias) && alias && alias != p2);
        REQUIRE(nvn_vk_dbg_pipe_stat(5) == creates);
    }

    // 同会话再测一条未进盘上缓存的冷配方 pd_b（只写 alpha 掩码 0x8）：绝不能在前台编译，必须交后台
    nvn_gpu_pipe_desc pd_b2 = make_desc(vs2, fs2, 0x8u);
    nvn_gpu_pipe pb = NULL;
    if (is_serial_busy) {
        // 串行模式持锁零阻塞：后台编译 pd_b2 持着 g_create_mu 时，前台对另一配方 pd_c2 做 kick
        // 绝不得在 g_create_mu 上阻塞（trylock 失败即当未命中交后台），且连续重复 kick 不重复探测
        pthread_mutex_lock(&g_gate_mu);
        g_hold_mask_bit = 1u << 0x8u;
        g_hold_active = 1;
        g_hold_entered = 0;
        pthread_mutex_unlock(&g_gate_mu);
        int kb = nvn_vk_pipe_demand_kick(&pd_b2, g_ident_b, &pb);
        REQUIRE(kb == NVK_DEMAND_PENDING && !pb);
        pthread_mutex_lock(&g_gate_mu);
        struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += 10;
        while (!g_hold_entered) {
            if (pthread_cond_timedwait(&g_gate_cv, &g_gate_mu, &dl) != 0) break;
        }
        int entered = g_hold_entered;
        pthread_mutex_unlock(&g_gate_mu);
        REQUIRE(entered == 1);

        const unsigned probes_while_held = g_probe_calls;
        nvn_gpu_pipe_desc pd_c2 = make_desc(vs2, fs2, 0x4u);
        struct serial_kick_arg sk = { .desc = &pd_c2, .ident = g_ident_c };
        pthread_mutex_init(&sk.mu, NULL);
        pthread_cond_init(&sk.cv, NULL);
        pthread_t sk_th;
        REQUIRE(pthread_create(&sk_th, NULL, serial_kick_main, &sk) == 0);
        struct timespec kdl; clock_gettime(CLOCK_REALTIME, &kdl);
        kdl.tv_nsec += 500000000L;
        if (kdl.tv_nsec >= 1000000000L) { kdl.tv_sec++; kdl.tv_nsec -= 1000000000L; }
        pthread_mutex_lock(&sk.mu);
        while (!sk.done) {
            if (pthread_cond_timedwait(&sk.cv, &sk.mu, &kdl) != 0) break;
        }
        int sk_done_before_release = sk.done;
        pthread_mutex_unlock(&sk.mu);

        if (!sk_done_before_release) {
            // 放开持锁门避免进程挂死，再如实报红
            pthread_mutex_lock(&g_gate_mu); g_hold_active = 0; pthread_cond_broadcast(&g_gate_cv); pthread_mutex_unlock(&g_gate_mu);
            pthread_join(sk_th, NULL);
            fprintf(stderr, "【红】serial-busy: foreground kick blocked on g_create_mu while background build held lock\n");
            return 1;
        }
        pthread_join(sk_th, NULL);
        REQUIRE(sk.rc == NVK_DEMAND_PENDING && !sk.pipe);
        REQUIRE(g_probe_calls == probes_while_held);

        // 在途期间重复 kick 4 次：断言 dedup 涨 4 且 g_probe_calls 仍为 probes_while_held
        struct nvn_pipe_demand_stats ds0 = {0}, ds1 = {0};
        nvn_vk_pipe_demand_stats(&ds0);
        for (int i = 0; i < 4; i++) {
            nvn_gpu_pipe pdup = NULL;
            REQUIRE(nvn_vk_pipe_demand_kick(&pd_c2, g_ident_c, &pdup) == NVK_DEMAND_PENDING && !pdup);
        }
        nvn_vk_pipe_demand_stats(&ds1);
        REQUIRE(ds1.dedup == ds0.dedup + 4);
        REQUIRE(g_probe_calls == probes_while_held);

        // 放开持锁门，等 pd_b2 与 pd_c2 均建成并逐像素核对
        pthread_mutex_lock(&g_gate_mu); g_hold_active = 0; g_hold_mask_bit = 0; pthread_cond_broadcast(&g_gate_cv); pthread_mutex_unlock(&g_gate_mu);
        REQUIRE(nvn_vk_pipe_demand_wait(&pd_b2, g_ident_b, 10000, &pb, wwhy, sizeof wwhy) == 0 && pb != NULL);
        nvn_gpu_pipe pc = NULL;
        REQUIRE(nvn_vk_pipe_demand_wait(&pd_c2, g_ident_c, 10000, &pc, wwhy, sizeof wwhy) == 0 && pc != NULL);
        uint8_t imgb[BYTES], imgc[BYTES];
        REQUIRE(draw_and_readback(pb, 2, imgb) == 0);
        REQUIRE(check_solid(imgb, 0, 0, 0, 255) == 0);
        REQUIRE(draw_and_readback(pc, 3, imgc) == 0);
        REQUIRE(check_solid(imgc, 0, 0, 0, 255) == 0);
    } else {
        int kb = nvn_vk_pipe_demand_kick(&pd_b2, g_ident_b, &pb);
        REQUIRE(kb == NVK_DEMAND_PENDING && !pb);
        REQUIRE(nvn_vk_pipe_demand_wait(&pd_b2, g_ident_b, 10000, &pb, wwhy, sizeof wwhy) == 0 && pb != NULL);
        uint8_t imgb[BYTES];
        REQUIRE(draw_and_readback(pb, 2, imgb) == 0);
        REQUIRE(check_solid(imgb, 0, 0, 0, 255) == 0);
    }
    if (g_combo) {
        REQUIRE(!nvn_gpu_pipe_lookup(&alias_desc, &look) && !look);
        REQUIRE(!nvn_gpu_pipe_lookup(&pd_a2, &look) && !look);
        const unsigned hits = g_probe_hits;
        const uint64_t sync = rp_sync_pipelines_created();
        nvn_gpu_pipe restored = NULL;
        REQUIRE(nvn_vk_pipe_demand_kick(&alias_desc, g_ident_a, &restored) == NVK_DEMAND_READY);
        REQUIRE(restored == alias && g_probe_hits == hits + 1 && rp_sync_pipelines_created() == sync);
        REQUIRE(nvn_gpu_pipe_lookup(&pd_a2, &look) && look == p2);
        REQUIRE(!nvn_gpu_pipe_lookup(&pd_b2, &look) && !look);
        REQUIRE(draw_and_readback(restored, 3, img2) == 0);
        REQUIRE(memcmp(img1, img2, BYTES) == 0);
        printf("pipe-cache-ctl combo dq=%d budget=1 alias=1 restore=1 pushed-draws=4 pixels=%u/%u\n",
               g_combo_dq, 4u * W * H, 4u * W * H);
    }

    // 负控：cold_kick 绝不走前台 cache probe
    uint8_t cold_sha[32];
    memset(cold_sha, 0xC3, 32);
    const unsigned probes_before_cold = g_probe_calls;
    nvn_gpu_pipe_desc pd_cold = make_desc(vs2, fs2, 0x5u);
    nvn_gpu_pipe pcold = NULL;
    int kcold = nvn_vk_pipe_demand_cold_kick(&pd_cold, g_ident_b, cold_sha, &pcold);
    REQUIRE(kcold == NVK_DEMAND_PENDING && !pcold);
    REQUIRE(g_probe_calls == probes_before_cold);

    uint64_t cstats[3] = {0};
    if (nvn_vk_pipe_cache_ctl_stats) nvn_vk_pipe_cache_ctl_stats(cstats);

    printf("pipe-cache-ctl mode=%s s2_loaded=%llu s1_probe=%u(miss=%u,hit=%u) s2_k2=%d s2_kicked=%llu s2_cache_hits=%llu s2_probe=%u(hit=%u,miss=%u) sync_delta=%llu dev_ext=%u dev_feat=%u stats=%llu/%llu/%llu\n",
           mode, (unsigned long long)s2_loaded, s1_probes, s1_misses, s1_hits, k2,
           (unsigned long long)(st1.kicked - st0.kicked), (unsigned long long)(st1.cache_hits - st0.cache_hits),
           g_probe_calls, g_probe_hits, g_probe_misses, (unsigned long long)(sync_after_a - sync_before),
           g_dev_ext_enabled, g_dev_feat_enabled,
           (unsigned long long)cstats[0], (unsigned long long)cstats[1], (unsigned long long)cstats[2]);

    nvn_vk_pipe_demand_stop();
    nvn_gpu_shader_release(vs2);
    nvn_gpu_shader_release(fs2);
    GPU(nvn_gpu_close(5000000000ull, &err));
    s2a_vk_device_destroy(&dev2, 1);
    free(vspv);
    free(fspv);

    if (expect_ctl) {
        // withdraw 臂下 g_flag_off=1，预期在这里按红失败，证明断言有效
        if (k2 != NVK_DEMAND_READY || (st1.kicked - st0.kicked) != 0 || (st1.cache_hits - st0.cache_hits) != 1 ||
            g_probe_hits < 1 || s1_misses < 1 || g_probe_misses < 1 || (sync_after_a - sync_before) != 0) {
            fprintf(stderr, "【红】cache control missed expectation: k2=%d kicked=%llu cache_hits=%llu s1_miss=%u s2_hit=%u s2_miss=%u sync_delta=%llu\n",
                    k2, (unsigned long long)(st1.kicked - st0.kicked), (unsigned long long)(st1.cache_hits - st0.cache_hits),
                    s1_misses, g_probe_hits, g_probe_misses, (unsigned long long)(sync_after_a - sync_before));
            return 1;
        }
    } else {
        REQUIRE(!expect_probe_active);
        REQUIRE(s1_probes == 0 && g_probe_calls == 0);
        REQUIRE(k2 == NVK_DEMAND_PENDING && (st1.kicked - st0.kicked) == 1 && (st1.cache_hits - st0.cache_hits) == 0);
        if (g_hide_ext || g_hide_feat || g_flag_off) {
            REQUIRE(g_dev_ext_enabled == 0 && g_dev_feat_enabled == 0);
        }
    }
    return 0;
}
