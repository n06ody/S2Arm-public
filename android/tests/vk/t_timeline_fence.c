// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real-Vulkan contract test for VK_KHR_timeline_semaphore submission completion tracking
// (A/B switch debug.s2a.nvk_timeline_fence, default 0).
// Verifies runtime extension/feature/entry-point gating, zero per-submit VkFence alloc/wait/destroy,
// O(1) batch retirement via vkGetSemaphoreCounterValueKHR, out-of-order & non-blocking fence_wait,
// terminal fence-fail propagation, close/reopen lifecycle, and --wrap extension-hiding fallback.
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void nvn_gpu_vk_test_fail_fence(uint64_t signal);
extern void nvn_gpu_vk_test_delay_segment(uint64_t signal, uint32_t ms);

static int dq_mode, opt_mode, tl_mode, hide_extension;
static int enabled_tl_ext, enabled_tl_feat;
static PFN_vkGetSemaphoreCounterValueKHR real_tl_get;
static PFN_vkWaitSemaphoresKHR real_tl_wait;

static pthread_mutex_t obs_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned obs_tl_sem_create, obs_fence_create, obs_fence_destroy, obs_fence_wait;
static unsigned obs_tl_submit, obs_tl_submit_bad, obs_tl_get, obs_tl_wait;
static uint64_t obs_tl_last_sig;
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
    if (!strcmp(name, "nvk_timeline_fence")) return tl_mode;
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
extern __typeof__(vkCreateSemaphore) __real_vkCreateSemaphore;
extern __typeof__(vkCreateFence) __real_vkCreateFence;
extern __typeof__(vkDestroyFence) __real_vkDestroyFence;
extern __typeof__(vkWaitForFences) __real_vkWaitForFences;
extern __typeof__(vkQueueSubmit) __real_vkQueueSubmit;
extern __typeof__(vkGetDeviceProcAddr) __real_vkGetDeviceProcAddr;

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice phys, const char *layer, uint32_t *count, VkExtensionProperties *props) {
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
        if (strcmp(all[i].extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) all[kept++] = all[i];
    if (!props) { *count = kept; free(all); return VK_SUCCESS; }
    const uint32_t n = *count < kept ? *count : kept;
    memcpy(props, all, n * sizeof *props);
    *count = n;
    free(all);
    return n < kept ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(
    VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *dev) {
    enabled_tl_ext = 0;
    enabled_tl_feat = 0;
    if (ci) {
        for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
            if (!strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME))
                enabled_tl_ext = 1;
        for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
            if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR) {
                const VkPhysicalDeviceTimelineSemaphoreFeaturesKHR *f = (const void *)p;
                if (f->timelineSemaphore == VK_TRUE) enabled_tl_feat = 1;
            }
        }
    }
    return __real_vkCreateDevice(phys, ci, alloc, dev);
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateSemaphore(
    VkDevice d, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *alloc, VkSemaphore *out) {
    VkResult r = __real_vkCreateSemaphore(d, ci, alloc, out);
    if (r == VK_SUCCESS && ci) {
        for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
            if (p->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR) {
                const VkSemaphoreTypeCreateInfoKHR *t = (const void *)p;
                if (t->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE_KHR) {
                    pthread_mutex_lock(&obs_mu);
                    obs_tl_sem_create++;
                    pthread_mutex_unlock(&obs_mu);
                }
            }
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateFence(
    VkDevice d, const VkFenceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkFence *out) {
    VkResult r = __real_vkCreateFence(d, ci, alloc, out);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&obs_mu);
        obs_fence_create++;
        pthread_mutex_unlock(&obs_mu);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL __wrap_vkDestroyFence(VkDevice d, VkFence f, const VkAllocationCallbacks *alloc) {
    if (f != VK_NULL_HANDLE) {
        pthread_mutex_lock(&obs_mu);
        obs_fence_destroy++;
        pthread_mutex_unlock(&obs_mu);
    }
    __real_vkDestroyFence(d, f, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkWaitForFences(
    VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t t) {
    pthread_mutex_lock(&obs_mu);
    obs_fence_wait++;
    pthread_mutex_unlock(&obs_mu);
    return __real_vkWaitForFences(d, n, f, all, t);
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkQueueSubmit(
    VkQueue q, uint32_t count, const VkSubmitInfo *si, VkFence fence) {
    if (si && count > 0) {
        for (uint32_t i = 0; i < count; i++) {
            for (const VkBaseInStructure *p = si[i].pNext; p; p = p->pNext) {
                if (p->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR) {
                    const VkTimelineSemaphoreSubmitInfoKHR *t = (const void *)p;
                    pthread_mutex_lock(&obs_mu);
                    obs_tl_submit++;
                    if (fence != VK_NULL_HANDLE || t->signalSemaphoreValueCount != si[i].signalSemaphoreCount ||
                        t->signalSemaphoreValueCount < 1 || !t->pSignalSemaphoreValues) {
                        obs_tl_submit_bad++;
                    } else {
                        uint64_t v = t->pSignalSemaphoreValues[t->signalSemaphoreValueCount - 1];
                        if (v <= obs_tl_last_sig) obs_tl_submit_bad++;
                        obs_tl_last_sig = v;
                    }
                    pthread_mutex_unlock(&obs_mu);
                }
            }
        }
    }
    return __real_vkQueueSubmit(q, count, si, fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetSemaphoreCounterValueKHR(VkDevice d, VkSemaphore s, uint64_t *val) {
    pthread_mutex_lock(&obs_mu);
    obs_tl_get++;
    pthread_mutex_unlock(&obs_mu);
    REQUIRE(real_tl_get != NULL);
    return real_tl_get(d, s, val);
}

static VKAPI_ATTR VkResult VKAPI_CALL wrap_vkWaitSemaphoresKHR(VkDevice d, const VkSemaphoreWaitInfoKHR *wi, uint64_t t) {
    pthread_mutex_lock(&obs_mu);
    obs_tl_wait++;
    pthread_mutex_unlock(&obs_mu);
    REQUIRE(real_tl_wait != NULL);
    return real_tl_wait(d, wi, t);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice d, const char *name) {
    if (!strcmp(name, "vkGetSemaphoreCounterValueKHR")) {
        PFN_vkVoidFunction fn = __real_vkGetDeviceProcAddr(d, name);
        if (!fn) return NULL;
        real_tl_get = (PFN_vkGetSemaphoreCounterValueKHR)fn;
        return (PFN_vkVoidFunction)wrap_vkGetSemaphoreCounterValueKHR;
    }
    if (!strcmp(name, "vkWaitSemaphoresKHR")) {
        PFN_vkVoidFunction fn = __real_vkGetDeviceProcAddr(d, name);
        if (!fn) return NULL;
        real_tl_wait = (PFN_vkWaitSemaphoresKHR)fn;
        return (PFN_vkVoidFunction)wrap_vkWaitSemaphoresKHR;
    }
    return __real_vkGetDeviceProcAddr(d, name);
}

_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkEnumerateDeviceExtensionProperties), PFN_vkEnumerateDeviceExtensionProperties), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateDevice), PFN_vkCreateDevice), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateSemaphore), PFN_vkCreateSemaphore), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateFence), PFN_vkCreateFence), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkDestroyFence), PFN_vkDestroyFence), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkWaitForFences), PFN_vkWaitForFences), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkQueueSubmit), PFN_vkQueueSubmit), "sig");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkGetDeviceProcAddr), PFN_vkGetDeviceProcAddr), "sig");

static void obs_reset_fences(void) {
    pthread_mutex_lock(&obs_mu);
    obs_fence_create = obs_fence_destroy = obs_fence_wait = 0;
    pthread_mutex_unlock(&obs_mu);
}

static void obs_reset(void) {
    pthread_mutex_lock(&obs_mu);
    obs_tl_sem_create = obs_fence_create = obs_fence_destroy = obs_fence_wait = 0;
    obs_tl_submit = obs_tl_submit_bad = obs_tl_get = obs_tl_wait = 0;
    obs_tl_last_sig = 0;
    pthread_mutex_unlock(&obs_mu);
}

struct done_log {
    pthread_mutex_t mu;
    uint64_t sigs[32];
    int statuses[32];
    unsigned n;
};

static void record_done(void *u, const nvn_gpu_done *d) {
    struct done_log *dl = u;
    pthread_mutex_lock(&dl->mu);
    if (dl->n < 32) {
        dl->sigs[dl->n] = d->signal;
        dl->statuses[dl->n] = d->status;
        dl->n++;
    }
    pthread_mutex_unlock(&dl->mu);
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

static nvn_gpu_pipe make_pipe(nvn_gpu_shader vs, nvn_gpu_shader fs) {
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3;
    pd.chmask.mask = 0xf;
    pd.prim = 4;
    pd.samples = 1;
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));
    return pipe;
}

static nvn_gpu_dyn make_dyn(void) {
    nvn_gpu_dyn dyn = {0};
    dyn.nvp = 1; dyn.vp[0][2] = dyn.vp[0][3] = 4.0f; dyn.vp[0][5] = 1.0f;
    dyn.has_sc = 1; dyn.sc[2] = dyn.sc[3] = 4;
    return dyn;
}

static unsigned check_rgba(const unsigned char *px, unsigned r, unsigned g, unsigned b) {
    unsigned bad = 0;
    for (unsigned i = 0; i < 64; i += 4) {
        if (abs((int)px[i] - (int)r) > 2 ||
            abs((int)px[i + 1] - (int)g) > 2 ||
            abs((int)px[i + 2] - (int)b) > 2) bad++;
    }
    return bad;
}

static void submit_draw(nvn_gpu_pipe pipe, nvn_gpu_tex rt, nvn_gpu_view vrt, nvn_gpu_buf rb,
                        uint64_t rb_off, float r, float g, float b, uint64_t signal, struct done_log *dl) {
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_pass_desc p = {0}; p.w = p.h = 4; p.layers = 1; p.ncolor = 1;
    p.color[0].view = vrt; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_dyn dyn = make_dyn();
    nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn);
    float col[4] = {r, g, b, 1.0f};
    nvn_gpu_push(cmd, NVN_GPU_SH_FS, 0, col, sizeof col);
    nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(cmd, &da);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, rb_off, 16, 64);
    GPU(nvn_gpu_cmd_commit(cmd, signal, record_done, dl, &err));
}

struct tc { const char *name; int id; };
static const struct tc k_cases[] = {
    {"burst-16-batch-completion", 0},
    {"out-of-order-and-nonblocking-wait", 1},
    {"delayed-worker-batch-retire", 2},
    {"fence-fail-terminal-propagation", 3},
    {"close-reopen-lifecycle", 4},
};

static void run_case(struct s2a_vk_device *device, const char *dir, const struct tc *tc) {
    ncases++;
    obs_reset();
    nvn_gpu_open_desc od = {0};
    od.host_device = device;
    od.app_label = tc->name;
    GPU(nvn_gpu_open(&od, &err));
    // Exclude one-time report_timestamp_open() calibration fence created inside nvn_gpu_open().
    obs_reset_fences();

    const int active = device->timeline_semaphore_enabled && device->timeline_get_counter && device->timeline_wait && tl_mode;
    nvn_gpu_shader vs = load_sh(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_layout pc_lay = {.push_bytes = 16};
    nvn_gpu_shader fs = load_sh(dir, "solid_pc.frag", NVN_GPU_SH_FS, &pc_lay);
    nvn_gpu_pipe pipe = make_pipe(vs, fs);

    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 4; td.d.depth = 1; td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex rt = NULL; nvn_gpu_view vrt = NULL;
    GPU(nvn_gpu_tex_create(&td, &rt, &err));
    GPU(nvn_gpu_view_create(rt, NULL, &vrt, &err));

    unsigned char z[16 * 64]; memset(z, 0, sizeof z);
    nvn_gpu_buf rb = NULL;
    GPU(nvn_gpu_buf_create(sizeof z, NVN_GPU_MEM_SHARED, z, &rb, &err));

    struct done_log dl = {0};
    pthread_mutex_init(&dl.mu, NULL);
    unsigned op_bad = 0, px_bad = 0;

    if (tc->id == 0) {
        // Burst 16 submits back-to-back, then wait on 16.
        for (unsigned i = 0; i < 16; i++) {
            float r = (float)(i + 1) / 16.0f, g = 1.0f - r, b = 0.25f;
            submit_draw(pipe, rt, vrt, rb, (uint64_t)i * 64, r, g, b, (uint64_t)i + 1, &dl);
        }
        GPU(nvn_gpu_fence_wait(16, 5000000000ull, &err));
        REQUIRE(nvn_gpu_fence_done() == 16);
        REQUIRE(nvn_gpu_fence_failed() == 0);
        if (dl.n != 16) op_bad++;
        for (unsigned i = 0; i < 16 && i < dl.n; i++)
            if (dl.sigs[i] != (uint64_t)i + 1 || dl.statuses[i] != NVN_GPU_DONE_OK) op_bad++;
        const unsigned char *px = nvn_gpu_buf_cpu(rb);
        for (unsigned i = 0; i < 16; i++) {
            double rf = (double)(i + 1) / 16.0;
            px_bad += check_rgba(px + (size_t)i * 64, (unsigned)lround(rf * 255.0), (unsigned)lround((1.0 - rf) * 255.0), 64);
        }
        if (active) {
            if (obs_tl_sem_create != 1 || obs_fence_create != 0 || obs_fence_wait != 0 ||
                obs_tl_submit != 16 || obs_tl_submit_bad != 0 || obs_tl_get < 1) op_bad++;
        } else {
            if (obs_tl_sem_create != 0 || obs_tl_submit != 0 || obs_fence_create != 16 || obs_fence_wait < 16) op_bad++;
        }
    } else if (tc->id == 1) {
        // Non-consecutive signals (10..80), non-blocking poll (timeout=0), wait middle (40), wait end (80), re-wait past (10,30,50).
        for (unsigned i = 0; i < 8; i++) {
            uint64_t sig = (uint64_t)(i + 1) * 10;
            float r = (float)(i + 1) / 8.0f, g = 0.5f, b = 1.0f - r;
            submit_draw(pipe, rt, vrt, rb, (uint64_t)i * 64, r, g, b, sig, &dl);
        }
        nvn_gpu_rc poll_rc = nvn_gpu_fence_wait(80, 0, &err);
        if (!nvn_gpu_ok(poll_rc) && poll_rc.code != NVN_GPU_E_TIMEOUT) op_bad++;
        GPU(nvn_gpu_fence_wait(40, 5000000000ull, &err));
        if (nvn_gpu_fence_done() < 40) op_bad++;
        GPU(nvn_gpu_fence_wait(80, 5000000000ull, &err));
        if (nvn_gpu_fence_done() != 80) op_bad++;
        // Out-of-order re-waits on already-completed values must succeed immediately with timeout=0!
        GPU(nvn_gpu_fence_wait(10, 0, &err));
        GPU(nvn_gpu_fence_wait(30, 0, &err));
        GPU(nvn_gpu_fence_wait(50, 0, &err));
        if (dl.n != 8) op_bad++;
        const unsigned char *px = nvn_gpu_buf_cpu(rb);
        for (unsigned i = 0; i < 8; i++) {
            double rf = (double)(i + 1) / 8.0;
            px_bad += check_rgba(px + (size_t)i * 64, (unsigned)lround(rf * 255.0), 128, (unsigned)lround((1.0 - rf) * 255.0));
        }
        if (active) {
            if (obs_tl_sem_create != 1 || obs_fence_create != 0 || obs_fence_wait != 0 || obs_tl_submit != 8 || obs_tl_submit_bad != 0) op_bad++;
        } else {
            if (obs_tl_sem_create != 0 || obs_tl_submit != 0 || obs_fence_create != 8) op_bad++;
        }
    } else if (tc->id == 2) {
        // Deterministic O(1) batch retirement proof: delay worker on segment 1 by 40ms while submitting 10 renders.
        // GPU completes all 10 during the delay -> segment 1 observes counter=10 -> segments 2..10 retire with ZERO Vulkan calls!
        nvn_gpu_vk_test_delay_segment(1, 40);
        for (unsigned i = 0; i < 10; i++) {
            float r = 0.25f, g = (float)(i + 1) / 10.0f, b = 0.75f;
            submit_draw(pipe, rt, vrt, rb, (uint64_t)i * 64, r, g, b, (uint64_t)i + 1, &dl);
        }
        GPU(nvn_gpu_fence_wait(10, 5000000000ull, &err));
        const unsigned char *px = nvn_gpu_buf_cpu(rb);
        for (unsigned i = 0; i < 10; i++) {
            double gf = (double)(i + 1) / 10.0;
            px_bad += check_rgba(px + (size_t)i * 64, 64, (unsigned)lround(gf * 255.0), 191);
        }
        if (active) {
            // Across 10 submits: 0 fences, and O(1) batch poll/wait must be strictly fewer than 10 (typically 1-2)!
            if (obs_fence_create != 0 || obs_fence_wait != 0 || obs_tl_submit != 10 ||
                (obs_tl_get + obs_tl_wait) >= 5) op_bad++;
        } else {
            if (obs_tl_submit != 0 || obs_fence_create != 10 || obs_fence_wait < 10) op_bad++;
        }
    } else if (tc->id == 3) {
        // Terminal failure propagation: fail signal 2 after real GPU completion.
        nvn_gpu_vk_test_fail_fence(2);
        for (unsigned i = 0; i < 4; i++) {
            float r = i == 0 ? 1.0f : 0.0f, g = i == 0 ? 0.5f : 1.0f, b = 0.25f;
            nvn_gpu_cmd cmd = NULL;
            GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
            nvn_gpu_pass_desc p = {0}; p.w = p.h = 4; p.layers = 1; p.ncolor = 1;
            p.color[0].view = vrt; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
            nvn_gpu_pass_begin(cmd, &p);
            nvn_gpu_dyn dyn = make_dyn();
            nvn_gpu_set_pipe(cmd, pipe); nvn_gpu_set_dyn(cmd, &dyn);
            float col[4] = {r, g, b, 1.0f};
            nvn_gpu_push(cmd, NVN_GPU_SH_FS, 0, col, sizeof col);
            nvn_gpu_draw_args da = {0}; da.prim = 4; da.count = 3; da.instances = 1;
            nvn_gpu_draw(cmd, &da);
            nvn_gpu_pass_end(cmd);
            nvn_gpu_copy_tex_buf(cmd, rt, 0, 0, NULL, rb, (uint64_t)i * 64, 16, 64);
            // Commits 3..4 may already observe g.failed != 0 and return E_FAILED directly (still invoking done(FAILED)).
            nvn_gpu_rc cr = nvn_gpu_cmd_commit(cmd, (uint64_t)i + 1, record_done, &dl, &err);
            if (i < 2 && !nvn_gpu_ok(cr)) op_bad++;
        }
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        nvn_gpu_rc r2 = nvn_gpu_fence_wait(2, 5000000000ull, &err);
        nvn_gpu_rc r4 = nvn_gpu_fence_wait(4, 5000000000ull, &err);
        if (r2.code != NVN_GPU_E_FAILED || r4.code != NVN_GPU_E_FAILED || nvn_gpu_fence_failed() != 2) op_bad++;
        px_bad += check_rgba(nvn_gpu_buf_cpu(rb), 255, 128, 64);
        if (active) {
            if (obs_fence_create != 0 || obs_fence_wait != 0 || obs_tl_submit < 2) op_bad++;
        } else {
            if (obs_tl_submit != 0 || obs_fence_create < 2) op_bad++;
        }
    } else if (tc->id == 4) {
        // Close with in-flight timeline-tracked submits (no prior fence_wait!), then verify clean reopen.
        for (unsigned i = 0; i < 3; i++)
            submit_draw(pipe, rt, vrt, rb, (uint64_t)i * 64, 0.5f, 0.25f, 1.0f, (uint64_t)i + 1, &dl);
        nvn_gpu_view_release(vrt); nvn_gpu_tex_release(rt);
        nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
        nvn_gpu_buf_release(rb, NULL, NULL);
        GPU(nvn_gpu_close(5000000000ull, &err));
        if (dl.n != 3) op_bad++;
        // Reopen a fresh session and run 1 submit to verify tl_seq resets cleanly to 1!
        const unsigned fc_before_reopen = obs_fence_create;
        const unsigned fd_before_reopen = obs_fence_destroy;
        const unsigned fw_before_reopen = obs_fence_wait;
        GPU(nvn_gpu_open(&od, &err));
        pthread_mutex_lock(&obs_mu);
        obs_fence_create = fc_before_reopen;
        obs_fence_destroy = fd_before_reopen;
        obs_fence_wait = fw_before_reopen;
        pthread_mutex_unlock(&obs_mu);
        vs = load_sh(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL);
        fs = load_sh(dir, "solid_pc.frag", NVN_GPU_SH_FS, &pc_lay);
        pipe = make_pipe(vs, fs);
        GPU(nvn_gpu_tex_create(&td, &rt, &err));
        GPU(nvn_gpu_view_create(rt, NULL, &vrt, &err));
        GPU(nvn_gpu_buf_create(64, NVN_GPU_MEM_SHARED, z, &rb, &err));
        obs_tl_last_sig = 0;
        submit_draw(pipe, rt, vrt, rb, 0, 0.25f, 1.0f, 0.5f, 1, &dl);
        GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
        px_bad += check_rgba(nvn_gpu_buf_cpu(rb), 64, 255, 128);
        if (active) {
            if (obs_tl_sem_create != 2 || obs_fence_create != 0 || obs_fence_wait != 0 || obs_tl_submit != 4) op_bad++;
        } else {
            if (obs_tl_sem_create != 0 || obs_tl_submit != 0 || obs_fence_create != 4) op_bad++;
        }
    }

    const unsigned bad = op_bad || px_bad;
    printf("timeline-fence case=%s active=%d tl_sem=%u fc=%u fd=%u fw=%u tl_sub=%u tl_get=%u tl_wait=%u bad_sub=%u op_bad=%u px_bad=%u bad=%u\n",
           tc->name, active, obs_tl_sem_create, obs_fence_create, obs_fence_destroy, obs_fence_wait,
           obs_tl_submit, obs_tl_get, obs_tl_wait, obs_tl_submit_bad, op_bad, px_bad, bad);
    failures += !!bad;

    nvn_gpu_view_release(vrt); nvn_gpu_tex_release(rt);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    GPU(nvn_gpu_close(5000000000ull, &err));
    pthread_mutex_destroy(&dl.mu);
}

int main(int argc, char **argv) {
    if (argc != 6) return 2;
    dq_mode = atoi(argv[2]);
    opt_mode = atoi(argv[3]);
    tl_mode = atoi(argv[4]);
    hide_extension = atoi(argv[5]);

    struct s2a_vk_device device;
    char why[256] = {0};
    REQUIRE(!s2a_vk_device_create(&device, why, sizeof why));
    REQUIRE(enabled_tl_ext == !hide_extension);
    REQUIRE(enabled_tl_feat == !hide_extension);
    REQUIRE(device.timeline_semaphore_enabled == !hide_extension);
    const int have_fns = device.timeline_get_counter != NULL && device.timeline_wait != NULL;
    REQUIRE(have_fns == !hide_extension);

    uint32_t n = 0;
    REQUIRE(__real_vkEnumerateDeviceExtensionProperties(device.physical, NULL, &n, NULL) == VK_SUCCESS);
    VkExtensionProperties *ext = calloc(n ? n : 1, sizeof *ext); REQUIRE(ext);
    REQUIRE(__real_vkEnumerateDeviceExtensionProperties(device.physical, NULL, &n, ext) == VK_SUCCESS);
    int advertised = 0;
    for (uint32_t i = 0; i < n; i++)
        advertised |= !strcmp(ext[i].extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    free(ext);
    REQUIRE(advertised);

    for (unsigned i = 0; i < sizeof k_cases / sizeof *k_cases; i++)
        run_case(&device, argv[1], &k_cases[i]);

    const int dev_tl = device.timeline_semaphore_enabled;
    REQUIRE(!s2a_vk_device_destroy(&device, 1));
    printf("timeline-fence cases=%u failures=%u dq=%d opt=%d tl=%d hidden=%d ext-enabled=%d feat-enabled=%d dev-enabled=%d fns=%d\n",
           ncases, failures, dq_mode, opt_mode, tl_mode, hide_extension, enabled_tl_ext, enabled_tl_feat, dev_tl, have_fns);
    return failures ? 1 : 0;
}
