// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real Vulkan 1.1 + Mesa lavapipe + VVL + SyncVal contract test for VK_EXT_global_priority:
//   - default (unset = 0): no VK_EXT_global_priority, no queue pNext
//   - on (=1): chains VkDeviceQueueGlobalPriorityCreateInfoEXT(HIGH_EXT); accepts grant or clean NOT_PERMITTED downgrade
//   - not-permitted (=1 + injected VK_ERROR_NOT_PERMITTED_EXT): verifies 2-step auto-downgrade retry without ext/pNext
//   - hide-ext (=1 + ext stripped): no ext, no pNext, single vkCreateDevice call
//   - withdraw (=0 + expect HIGH): fails non-zero with 0 VVL errors
#define _POSIX_C_SOURCE 200809L
#include "nvn_backend.h"
#include "vk_device.h"
#include "nvn_plat_vk.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_hide_ext;
static int g_emulate_ext;
static int g_real_has_ext;
static int g_inject_not_permitted;
static int g_create_calls;
static int g_first_ext, g_last_ext;
static int g_first_qprio, g_last_qprio;
static uint64_t g_sig;
static nvn_gpu_err g_err;

#define REQ(x) do { if (!(x)) { fprintf(stderr, "【红】gprio line %d: %s (why=%s)\n", __LINE__, #x, g_err.why); return 1; } } while (0)

const char *const *nvn_plat_vk_instance_exts(uint32_t *n) { *n = 0; return NULL; }
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) {
    (void)i; (void)w; *s = VK_NULL_HANDLE; return VK_ERROR_EXTENSION_NOT_PRESENT;
}
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) { (void)w; (void)x; (void)y; return 0; }
void nvn_plat_safepoint(void) {}
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_pipe_cache")) return 0;
    return dflt;
}
void nvn_plat_log(int level, const char *cls, const char *text) {
    (void)level; fprintf(stderr, "[%s] %s\n", cls ? cls : "", text ? text : "");
}
static void done_cb(void *u, const nvn_gpu_done *d) { (void)u; (void)d; }

extern __typeof__(vkEnumerateDeviceExtensionProperties) __real_vkEnumerateDeviceExtensionProperties;
extern __typeof__(vkCreateDevice) __real_vkCreateDevice;

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice p, const char *layer, uint32_t *count, VkExtensionProperties *props) {
    uint32_t n = 0;
    VkResult r = __real_vkEnumerateDeviceExtensionProperties(p, layer, &n, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties *tmp = calloc(n ? n : 1, sizeof *tmp);
    if (!tmp) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = __real_vkEnumerateDeviceExtensionProperties(p, layer, &n, tmp);
    if (r != VK_SUCCESS) { free(tmp); return r; }
    int found = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(tmp[i].extensionName, VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME)) found = 1;
    }
    g_real_has_ext = found;
    uint32_t out_n = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (g_hide_ext && !strcmp(tmp[i].extensionName, VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME)) continue;
        if (props && out_n < *count) props[out_n] = tmp[i];
        out_n++;
    }
    if (g_emulate_ext && !g_hide_ext && !found) {
        if (props && out_n < *count) {
            memset(&props[out_n], 0, sizeof props[out_n]);
            snprintf(props[out_n].extensionName, sizeof props[out_n].extensionName, "%s", VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME);
            props[out_n].specVersion = 2;
        }
        out_n++;
    }
    free(tmp);
    if (!props) { *count = out_n; return VK_SUCCESS; }
    int incomplete = out_n > *count;
    if (!incomplete) *count = out_n;
    return incomplete ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(
    VkPhysicalDevice p, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *d) {
    int has_ext = 0, qprio = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        if (!strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME)) has_ext = 1;
    }
    for (uint32_t q = 0; q < ci->queueCreateInfoCount; q++) {
        for (const VkBaseInStructure *b = ci->pQueueCreateInfos[q].pNext; b; b = b->pNext) {
            if (b->sType == VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT) {
                qprio = (int)((const VkDeviceQueueGlobalPriorityCreateInfoEXT *)b)->globalPriority;
            }
        }
    }
    if (g_create_calls == 0) { g_first_ext = has_ext; g_first_qprio = qprio; }
    g_last_ext = has_ext; g_last_qprio = qprio;
    g_create_calls++;
    if (g_inject_not_permitted && qprio == (int)VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT) {
        return VK_ERROR_NOT_PERMITTED_EXT;
    }
    return __real_vkCreateDevice(p, ci, a, d);
}

static uint32_t *load_spv(const char *dir, const char *name, size_t *bytes) {
    char path[1024];
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n < 20 || (n & 3)) { fclose(f); return NULL; }
    uint32_t *w = malloc((size_t)n);
    if (!w || fread(w, 1, (size_t)n, f) != (size_t)n) { free(w); fclose(f); return NULL; }
    fclose(f);
    *bytes = (size_t)n;
    return w;
}

static int run_dispatch(const char *dir) {
    size_t bytes = 0;
    uint32_t *words = load_spv(dir, "gprio_check.comp.spv", &bytes);
    if (!words) return 0;
    nvn_gpu_layout_ent ent = {0};
    ent.cls = NVN_GPU_RES_SSBO; ent.count = 1;
    ent.stages = 1u << NVN_GPU_SH_CS; ent.access = NVN_GPU_ACC_RW;
    nvn_gpu_layout layout = {0}; layout.nent = 1; layout.ent = &ent;
    nvn_gpu_shader_src src = {0};
    src.stage = NVN_GPU_SH_CS; src.kind = NVN_GPU_BLOB_SPIRV;
    src.blob = words; src.bytes = bytes; src.entry = "main"; src.layout = &layout;
    nvn_gpu_shader sh = NULL;
    memset(&g_err, 0, sizeof g_err);
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh, &g_err);
    free(words);
    if (!nvn_gpu_ok(r) || !sh) return 0;
    nvn_gpu_stage_desc cs = {0}; cs.sh = sh;
    nvn_gpu_pipe pipe = NULL;
    if (!nvn_gpu_ok(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &pipe, &g_err))) {
        nvn_gpu_shader_release(sh); return 0;
    }
    uint32_t init[4] = {0};
    nvn_gpu_buf buf = NULL;
    if (!nvn_gpu_ok(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_SHARED, init, &buf, &g_err))) {
        nvn_gpu_shader_release(sh); return 0;
    }
    nvn_gpu_cmd cmd = NULL;
    if (!nvn_gpu_ok(nvn_gpu_cmd_begin(NULL, &cmd, &g_err))) {
        nvn_gpu_buf_release(buf, NULL, NULL); nvn_gpu_shader_release(sh); return 0;
    }
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_binding b = {0}; b.cls = NVN_GPU_RES_SSBO; b.u.b.buf = buf; b.u.b.len = sizeof init;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(cmd, pipe, 1, 1, 1, NULL, 0);
    int ok = nvn_gpu_ok(nvn_gpu_cmd_commit(cmd, ++g_sig, done_cb, NULL, &g_err)) &&
             nvn_gpu_ok(nvn_gpu_fence_wait(g_sig, 5000000000ull, &g_err));
    uint32_t got[4] = {0};
    if (ok) memcpy(got, nvn_gpu_buf_cpu(buf), sizeof got);
    nvn_gpu_buf_release(buf, NULL, NULL);
    nvn_gpu_shader_release(sh);
    for (uint32_t i = 0; ok && i < 4; i++) {
        if (got[i] != (i + 1u) * 0x135u) ok = 0;
    }
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <shader_dir> <default|on|not-permitted|hide-ext|withdraw>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1], *mode = argv[2];
    if (!strcmp(mode, "default")) unsetenv("debug.s2a.nvk_global_priority");
    else if (!strcmp(mode, "withdraw")) setenv("debug.s2a.nvk_global_priority", "0", 1);
    else setenv("debug.s2a.nvk_global_priority", "1", 1);
    g_hide_ext = !strcmp(mode, "hide-ext");
    g_inject_not_permitted = !strcmp(mode, "not-permitted");
    g_emulate_ext = (!strcmp(mode, "not-permitted") || !strcmp(mode, "withdraw"));

    struct s2a_vk_device dev;
    char why[256] = "";
    REQ(!s2a_vk_device_create(&dev, why, sizeof why));
    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    od.app_label = "global-priority-test";
    REQ(nvn_gpu_ok(nvn_gpu_open(&od, &g_err)));
    int disp_ok = run_dispatch(dir);

    int expect_try_high = (!strcmp(mode, "not-permitted") || !strcmp(mode, "withdraw") ||
                           (!strcmp(mode, "on") && g_real_has_ext));
    printf("global-priority mode=%s real_ext=%d calls=%d first_ext=%d first_qprio=%d last_ext=%d last_qprio=%d has=%d en=%d not_perm=%d prio=%d disp=%d\n",
           mode, g_real_has_ext, g_create_calls, g_first_ext, g_first_qprio, g_last_ext, g_last_qprio,
           dev.has_global_priority, dev.global_priority_enabled, dev.global_priority_not_permitted,
           (int)dev.queue_global_priority, disp_ok);

    int rc = 0;
    if (!disp_ok) rc = 1;
    if (expect_try_high) {
        if (!dev.has_global_priority || !g_first_ext || g_first_qprio != (int)VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT) rc = 1;
        if (!strcmp(mode, "not-permitted")) {
            if (g_create_calls != 2 || g_last_ext != 0 || g_last_qprio != 0 ||
                dev.global_priority_enabled != 0 || dev.global_priority_not_permitted != 1 ||
                dev.queue_global_priority != VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_EXT) rc = 1;
        } else if (!strcmp(mode, "on")) {
            int granted_ok = (g_create_calls == 1 && dev.global_priority_enabled == 1 &&
                              dev.global_priority_not_permitted == 0 &&
                              dev.queue_global_priority == VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT);
            int downgraded_ok = (g_create_calls == 2 && g_last_ext == 0 && g_last_qprio == 0 &&
                                 dev.global_priority_enabled == 0 && dev.global_priority_not_permitted == 1 &&
                                 dev.queue_global_priority == VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_EXT);
            if (!granted_ok && !downgraded_ok) rc = 1;
        }
    } else {
        int expect_has = (!strcmp(mode, "default") || !strcmp(mode, "on")) ? g_real_has_ext : 0;
        if (g_create_calls != 1 || g_first_ext != 0 || g_first_qprio != 0 ||
            dev.has_global_priority != expect_has || dev.global_priority_enabled != 0 ||
            dev.global_priority_not_permitted != 0 ||
            dev.queue_global_priority != VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_EXT) rc = 1;
    }

    REQ(nvn_gpu_ok(nvn_gpu_close(5000000000ull, &g_err)));
    REQ(!s2a_vk_device_destroy(&dev, 1));
    return rc;
}
