// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real Vulkan 1.1 + Mesa lavapipe + VVL + SyncVal contract test for VK_KHR_shader_float16_int8.
#define _POSIX_C_SOURCE 200809L
#include "nvn_backend.h"
#include "vk_device.h"
#include "nvn_plat_vk.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_hide_ext;
static int g_hide_fp16;
static int g_hide_int8;
static int g_hw_fp16;
static int g_hw_int8;
static int g_dev_ext;
static int g_dev_fp16;
static int g_dev_int8;
static uint64_t g_sig;
static nvn_gpu_err g_err;

#define REQ(x) do { if (!(x)) { fprintf(stderr, "【红】f16i8 line %d: %s (why=%s)\n", __LINE__, #x, g_err.why); return 1; } } while (0)

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
extern __typeof__(vkGetPhysicalDeviceFeatures2) __real_vkGetPhysicalDeviceFeatures2;
extern __typeof__(vkCreateDevice) __real_vkCreateDevice;

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice p, const char *layer, uint32_t *count, VkExtensionProperties *props) {
    if (!g_hide_ext) return __real_vkEnumerateDeviceExtensionProperties(p, layer, count, props);
    uint32_t n = 0;
    VkResult r = __real_vkEnumerateDeviceExtensionProperties(p, layer, &n, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties *tmp = calloc(n ? n : 1, sizeof *tmp);
    if (!tmp) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = __real_vkEnumerateDeviceExtensionProperties(p, layer, &n, tmp);
    if (r != VK_SUCCESS) { free(tmp); return r; }
    uint32_t out_n = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(tmp[i].extensionName, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME)) continue;
        if (props && out_n < *count) props[out_n] = tmp[i];
        out_n++;
    }
    free(tmp);
    if (!props) { *count = out_n; return VK_SUCCESS; }
    int incomplete = out_n > *count;
    if (!incomplete) *count = out_n;
    return incomplete ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *f) {
    __real_vkGetPhysicalDeviceFeatures2(p, f);
    if (!f) return;
    for (VkBaseOutStructure *b = f->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR) {
            VkPhysicalDeviceShaderFloat16Int8FeaturesKHR *sf = (VkPhysicalDeviceShaderFloat16Int8FeaturesKHR *)b;
            g_hw_fp16 = (sf->shaderFloat16 == VK_TRUE);
            g_hw_int8 = (sf->shaderInt8 == VK_TRUE);
            if (g_hide_fp16) sf->shaderFloat16 = VK_FALSE;
            if (g_hide_int8) sf->shaderInt8 = VK_FALSE;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(
    VkPhysicalDevice p, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *d) {
    g_dev_ext = 0; g_dev_fp16 = 0; g_dev_int8 = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        if (!strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME)) g_dev_ext = 1;
    }
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR) {
            const VkPhysicalDeviceShaderFloat16Int8FeaturesKHR *sf = (const VkPhysicalDeviceShaderFloat16Int8FeaturesKHR *)b;
            if (sf->shaderFloat16 == VK_TRUE) g_dev_fp16 = 1;
            if (sf->shaderInt8 == VK_TRUE) g_dev_int8 = 1;
        }
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

static uint32_t f32_bits(float v) { uint32_t u = 0; memcpy(&u, &v, 4); return u; }

static int run_shader_case(const char *dir, const char *name, int expect_ok, const char *expect_why, const uint32_t want[8]) {
    size_t bytes = 0;
    uint32_t *words = load_spv(dir, name, &bytes);
    if (!words) { fprintf(stderr, "【红】cannot load %s\n", name); return 0; }
    nvn_gpu_layout_ent ent = {0};
    ent.cls = NVN_GPU_RES_SSBO;
    ent.count = 1;
    ent.stages = 1u << NVN_GPU_SH_CS;
    ent.access = NVN_GPU_ACC_RW;
    nvn_gpu_layout layout = {0};
    layout.nent = 1;
    layout.ent = &ent;
    nvn_gpu_shader_src src = {0};
    src.stage = NVN_GPU_SH_CS;
    src.kind = NVN_GPU_BLOB_SPIRV;
    src.blob = words;
    src.bytes = bytes;
    src.entry = "main";
    src.layout = &layout;
    nvn_gpu_shader sh = NULL;
    memset(&g_err, 0, sizeof g_err);
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh, &g_err);
    free(words);
    if (!expect_ok) {
        int ok = (r.code == NVN_GPU_E_UNSUPPORTED && sh == NULL && (!expect_why || strstr(g_err.why, expect_why) != NULL));
        if (!ok) {
            fprintf(stderr, "【红】%s expected reject(%s) got rc=%d why=%s\n", name, expect_why ? expect_why : "", r.code, g_err.why);
            if (sh) nvn_gpu_shader_release(sh);
        }
        return ok;
    }
    if (!nvn_gpu_ok(r) || !sh) {
        fprintf(stderr, "【红】%s expected accept got rc=%d why=%s\n", name, r.code, g_err.why);
        return 0;
    }
    nvn_gpu_stage_desc cs = {0}; cs.sh = sh;
    nvn_gpu_pipe pipe = NULL;
    if (!nvn_gpu_ok(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &pipe, &g_err))) {
        fprintf(stderr, "【红】%s cpipe_get failed: %s\n", name, g_err.why);
        nvn_gpu_shader_release(sh);
        return 0;
    }
    uint32_t init[8] = {0};
    nvn_gpu_buf buf = NULL;
    if (!nvn_gpu_ok(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_SHARED, init, &buf, &g_err))) {
        nvn_gpu_shader_release(sh); return 0;
    }
    nvn_gpu_cmd cmd = NULL;
    if (!nvn_gpu_ok(nvn_gpu_cmd_begin(NULL, &cmd, &g_err))) {
        nvn_gpu_buf_release(buf, NULL, NULL); nvn_gpu_shader_release(sh); return 0;
    }
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_binding b = {0};
    b.cls = NVN_GPU_RES_SSBO; b.u.b.buf = buf; b.u.b.len = sizeof init;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(cmd, pipe, 1, 1, 1, NULL, 0);
    int ok = nvn_gpu_ok(nvn_gpu_cmd_commit(cmd, ++g_sig, done_cb, NULL, &g_err)) &&
             nvn_gpu_ok(nvn_gpu_fence_wait(g_sig, 5000000000ull, &g_err));
    uint32_t got[8] = {0};
    if (ok) memcpy(got, nvn_gpu_buf_cpu(buf), sizeof got);
    nvn_gpu_buf_release(buf, NULL, NULL);
    nvn_gpu_shader_release(sh);
    for (uint32_t i = 0; ok && i < 8; i++) {
        if (got[i] != want[i]) {
            fprintf(stderr, "【红】%s mismatch[%u]: got=%#x want=%#x\n", name, i, got[i], want[i]);
            ok = 0;
        }
    }
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <shader_dir> <on|off|hide-ext|hide-fp16|hide-int8|withdraw>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1], *mode = argv[2];
    if (!strcmp(mode, "off") || !strcmp(mode, "withdraw")) setenv("debug.s2a.nvk_shader_float16_int8", "0", 1);
    else setenv("debug.s2a.nvk_shader_float16_int8", "1", 1);
    g_hide_ext = !strcmp(mode, "hide-ext");
    g_hide_fp16 = !strcmp(mode, "hide-fp16");
    g_hide_int8 = !strcmp(mode, "hide-int8");

    struct s2a_vk_device dev;
    char why[256] = "";
    REQ(!s2a_vk_device_create(&dev, why, sizeof why));
    if (!strcmp(mode, "on") || !strcmp(mode, "withdraw")) {
        // Real test device (e.g. lavapipe) must support at least one of shaderFloat16 / shaderInt8.
        REQ(g_hw_fp16 || g_hw_int8);
    }
    int expect_fp16 = (!strcmp(mode, "on") || !strcmp(mode, "hide-int8") || !strcmp(mode, "withdraw")) ? g_hw_fp16 : 0;
    int expect_int8 = (!strcmp(mode, "on") || !strcmp(mode, "hide-fp16") || !strcmp(mode, "withdraw")) ? g_hw_int8 : 0;
    int expect_ext = (expect_fp16 || expect_int8);
    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    od.app_label = "shader-float16-int8-test";
    REQ(nvn_gpu_ok(nvn_gpu_open(&od, &g_err)));

    uint32_t want_std[8], want_fp16[8], want_int8[8];
    for (uint32_t i = 0; i < 8; i++) {
        want_std[i] = (i + 1u) * 0x101u;
        want_fp16[i] = f32_bits((float)(i + 1u) * 1.5f + 0.25f);
        uint8_t u = (uint8_t)(250u + i * 4u);
        int8_t s = (int8_t)(-10 + (int)(i * 3u));
        want_int8[i] = (uint32_t)u | (((uint32_t)((int)s + 128) & 0xffu) << 8);
    }

    int std_ok = run_shader_case(dir, "f16i8_std.comp.spv", 1, NULL, want_std);
    int fp16_ok = run_shader_case(dir, "f16i8_fp16.comp.spv", expect_fp16, expect_fp16 ? NULL : "shaderFloat16", want_fp16);
    int int8_ok = run_shader_case(dir, "f16i8_int8.comp.spv", expect_int8, expect_int8 ? NULL : "shaderInt8", want_int8);
    printf("shader-float16-int8 mode=%s hw=%d/%d dev_ext=%d dev_fp16=%d dev_int8=%d en_fp16=%d en_int8=%d std=%d fp16=%d int8=%d\n",
           mode, g_hw_fp16, g_hw_int8, g_dev_ext, g_dev_fp16, g_dev_int8, dev.shader_float16_enabled, dev.shader_int8_enabled,
           std_ok, fp16_ok, int8_ok);

    int rc = 0;
    if (g_dev_ext != expect_ext || g_dev_fp16 != expect_fp16 || g_dev_int8 != expect_int8 ||
        dev.shader_float16_enabled != expect_fp16 || dev.shader_int8_enabled != expect_int8 ||
        !std_ok || !fp16_ok || !int8_ok) {
        rc = 1;
    }

    REQ(nvn_gpu_ok(nvn_gpu_close(5000000000ull, &g_err)));
    REQ(!s2a_vk_device_destroy(&dev, 1));
    return rc;
}
