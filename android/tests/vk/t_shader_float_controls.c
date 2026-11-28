// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real Vulkan 1.1 + Mesa lavapipe + VVL + SyncVal contract test for VK_KHR_shader_float_controls.
#define _POSIX_C_SOURCE 200809L
#include "nvn_backend.h"
#include "vk_device.h"
#include "nvn_plat_vk.h"
#include "../../../graphics/third_party/spirv_cross/spirv.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_hide_ext;
static int g_hide_mode;
static int g_dev_ext;
static uint32_t g_hw_f32_mask;
static uint64_t g_sig;
static nvn_gpu_err g_err;

#define REQ(x) do { if (!(x)) { fprintf(stderr, "【红】fctrl line %d: %s (why=%s)\n", __LINE__, #x, g_err.why); return 1; } } while (0)

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
extern __typeof__(vkGetPhysicalDeviceProperties2) __real_vkGetPhysicalDeviceProperties2;
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
        if (!strcmp(tmp[i].extensionName, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME)) continue;
        if (props && out_n < *count) props[out_n] = tmp[i];
        out_n++;
    }
    free(tmp);
    if (!props) { *count = out_n; return VK_SUCCESS; }
    int incomplete = out_n > *count;
    if (!incomplete) *count = out_n;
    return incomplete ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceProperties2(VkPhysicalDevice p, VkPhysicalDeviceProperties2 *props) {
    __real_vkGetPhysicalDeviceProperties2(p, props);
    if (!props) return;
    for (VkBaseOutStructure *b = props->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES_KHR) {
            VkPhysicalDeviceFloatControlsPropertiesKHR *fc = (VkPhysicalDeviceFloatControlsPropertiesKHR *)b;
            uint32_t m = 0;
            if (fc->shaderDenormPreserveFloat32) m |= 1u << 0;
            if (fc->shaderDenormFlushToZeroFloat32) m |= 1u << 1;
            if (fc->shaderSignedZeroInfNanPreserveFloat32) m |= 1u << 2;
            if (fc->shaderRoundingModeRTEFloat32) m |= 1u << 3;
            if (fc->shaderRoundingModeRTZFloat32) m |= 1u << 4;
            g_hw_f32_mask = m;
            if (g_hide_mode) {
                fc->shaderDenormPreserveFloat32 = VK_FALSE;
                fc->shaderDenormFlushToZeroFloat32 = VK_FALSE;
                fc->shaderSignedZeroInfNanPreserveFloat32 = VK_FALSE;
                fc->shaderRoundingModeRTEFloat32 = VK_FALSE;
                fc->shaderRoundingModeRTZFloat32 = VK_FALSE;
            }
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(
    VkPhysicalDevice p, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *d) {
    g_dev_ext = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        if (!strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME)) g_dev_ext = 1;
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

// Insert OpCapability <cap>, OpExtension "SPV_KHR_float_controls", and OpExecutionMode <entry> <mode> 32
// in valid SPIR-V 1.3 section order.
static uint32_t *inject_float_controls(const uint32_t *src, size_t src_bytes,
                                       uint32_t cap, uint32_t mode, size_t *out_bytes) {
    size_t n = src_bytes / 4;
    if (n < 5 || src[0] != SpvMagicNumber) return NULL;
    uint32_t entry_id = 0;
    size_t cap_end = 5, mode_pos = 0;
    for (size_t i = 5; i < n;) {
        uint32_t len = src[i] >> 16, op = src[i] & 0xffffu;
        if (!len || len > n - i) return NULL;
        if (op == SpvOpCapability) cap_end = i + len;
        if (op == SpvOpEntryPoint && len >= 4 && src[i + 1] == SpvExecutionModelGLCompute) entry_id = src[i + 2];
        if (op == SpvOpExecutionMode && len >= 3 && src[i + 1] == entry_id) mode_pos = i + len;
        i += len;
    }
    if (!entry_id || !mode_pos) return NULL;
    static const char ext_name[] = "SPV_KHR_float_controls";
    size_t ext_words = (sizeof ext_name + 3) / 4; // 23 bytes -> 6 words
    size_t add_words = 2 + (1 + ext_words) + 4;
    uint32_t *dst = calloc(n + add_words, 4);
    if (!dst) return NULL;
    size_t k = 0;
    memcpy(dst, src, cap_end * 4);
    if (dst[1] < 0x00010300u) dst[1] = 0x00010300u;
    k = cap_end;
    dst[k++] = (2u << 16) | SpvOpCapability;
    dst[k++] = cap;
    dst[k++] = ((uint32_t)(1 + ext_words) << 16) | SpvOpExtension;
    memcpy(&dst[k], ext_name, sizeof ext_name);
    k += ext_words;
    memcpy(&dst[k], src + cap_end, (mode_pos - cap_end) * 4);
    k += (mode_pos - cap_end);
    dst[k++] = (4u << 16) | SpvOpExecutionMode;
    dst[k++] = entry_id;
    dst[k++] = mode;
    dst[k++] = 32u;
    memcpy(&dst[k], src + mode_pos, (n - mode_pos) * 4);
    k += (n - mode_pos);
    *out_bytes = k * 4;
    return dst;
}

static uint32_t f32_bits(float v) { uint32_t u = 0; memcpy(&u, &v, 4); return u; }

static int run_shader_case(const uint32_t *words, size_t bytes, const char *label,
                           int expect_ok, const char *expect_why) {
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
    if (!expect_ok) {
        int ok = (r.code == NVN_GPU_E_UNSUPPORTED && sh == NULL && (!expect_why || strstr(g_err.why, expect_why) != NULL));
        if (!ok) {
            fprintf(stderr, "【红】%s expected reject(%s) got rc=%d why=%s\n", label, expect_why ? expect_why : "", r.code, g_err.why);
            if (sh) nvn_gpu_shader_release(sh);
        }
        return ok;
    }
    if (!nvn_gpu_ok(r) || !sh) {
        fprintf(stderr, "【红】%s expected accept got rc=%d why=%s\n", label, r.code, g_err.why);
        return 0;
    }
    nvn_gpu_stage_desc cs = {0}; cs.sh = sh;
    nvn_gpu_pipe pipe = NULL;
    if (!nvn_gpu_ok(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &pipe, &g_err))) {
        fprintf(stderr, "【红】%s cpipe_get failed: %s\n", label, g_err.why);
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
    nvn_gpu_binding b = {0};
    b.cls = NVN_GPU_RES_SSBO; b.u.b.buf = buf; b.u.b.len = sizeof init;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(cmd, pipe, 1, 1, 1, NULL, 0);
    int ok = nvn_gpu_ok(nvn_gpu_cmd_commit(cmd, ++g_sig, done_cb, NULL, &g_err)) &&
             nvn_gpu_ok(nvn_gpu_fence_wait(g_sig, 5000000000ull, &g_err));
    uint32_t got[4] = {0};
    if (ok) memcpy(got, nvn_gpu_buf_cpu(buf), sizeof got);
    nvn_gpu_buf_release(buf, NULL, NULL);
    nvn_gpu_shader_release(sh);
    uint32_t want[4] = { 0x80000000u, f32_bits(1.5f), f32_bits(3.0f), f32_bits(4.5f) };
    for (uint32_t i = 0; ok && i < 4; i++) {
        if (got[i] != want[i]) {
            fprintf(stderr, "【红】%s mismatch[%u]: got=%#x want=%#x\n", label, i, got[i], want[i]);
            ok = 0;
        }
    }
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <shader_dir> <on|off|hide-ext|hide-mode|withdraw>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1], *mode = argv[2];
    if (!strcmp(mode, "off") || !strcmp(mode, "withdraw")) setenv("debug.s2a.nvk_shader_float_controls", "0", 1);
    else setenv("debug.s2a.nvk_shader_float_controls", "1", 1);
    g_hide_ext = !strcmp(mode, "hide-ext");
    g_hide_mode = !strcmp(mode, "hide-mode");

    struct s2a_vk_device dev;
    char why[256] = "";
    REQ(!s2a_vk_device_create(&dev, why, sizeof why));
    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    od.app_label = "shader-float-controls-test";
    REQ(nvn_gpu_ok(nvn_gpu_open(&od, &g_err)));

    size_t base_bytes = 0, fc_bytes = 0;
    uint32_t *base_spv = load_spv(dir, "fctrl_base.comp.spv", &base_bytes);
    REQ(base_spv != NULL);
    // Prefer SignedZeroInfNanPreserve (bit 2), fall back to RoundingModeRTE (bit 3) or any supported float32 mode.
    uint32_t pick_cap = SpvCapabilitySignedZeroInfNanPreserve;
    uint32_t pick_mode = SpvExecutionModeSignedZeroInfNanPreserve;
    if (g_hw_f32_mask && !(g_hw_f32_mask & (1u << 2))) {
        if (g_hw_f32_mask & (1u << 3)) { pick_cap = SpvCapabilityRoundingModeRTE; pick_mode = SpvExecutionModeRoundingModeRTE; }
        else if (g_hw_f32_mask & (1u << 0)) { pick_cap = SpvCapabilityDenormPreserve; pick_mode = SpvExecutionModeDenormPreserve; }
        else if (g_hw_f32_mask & (1u << 1)) { pick_cap = SpvCapabilityDenormFlushToZero; pick_mode = SpvExecutionModeDenormFlushToZero; }
        else if (g_hw_f32_mask & (1u << 4)) { pick_cap = SpvCapabilityRoundingModeRTZ; pick_mode = SpvExecutionModeRoundingModeRTZ; }
    }
    uint32_t *fc_spv = inject_float_controls(base_spv, base_bytes, pick_cap, pick_mode, &fc_bytes);
    REQ(fc_spv != NULL);

    int expect_ext = (!strcmp(mode, "on") || !strcmp(mode, "hide-mode") || !strcmp(mode, "withdraw"));
    int expect_fc_ok = (!strcmp(mode, "on") || !strcmp(mode, "withdraw"));
    uint32_t pick_bit = 1u << ((pick_mode - SpvExecutionModeDenormPreserve) * 3u + 1u);
    int mask_ok = expect_fc_ok ? ((dev.float_controls_mask & pick_bit) != 0)
                               : (!expect_ext ? (dev.float_controls_mask == 0)
                                              : ((dev.float_controls_mask & pick_bit) == 0));
    int std_ok = run_shader_case(base_spv, base_bytes, "std", 1, NULL);
    int fc_ok = run_shader_case(fc_spv, fc_bytes, "fctrl", expect_fc_ok, expect_fc_ok ? NULL : "VK_KHR_shader_float_controls");
    printf("shader-float-controls mode=%s hw_f32=0x%x pick=%u dev_ext=%d en=%d mask=0x%04x mask_ok=%d std=%d fctrl=%d\n",
           mode, g_hw_f32_mask, pick_mode, g_dev_ext, dev.float_controls_enabled, dev.float_controls_mask,
           mask_ok, std_ok, fc_ok);

    int rc = 0;
    if (g_dev_ext != expect_ext || dev.float_controls_enabled != expect_ext || !mask_ok || !std_ok || !fc_ok) rc = 1;

    free(fc_spv);
    free(base_spv);
    REQ(nvn_gpu_ok(nvn_gpu_close(5000000000ull, &g_err)));
    REQ(!s2a_vk_device_destroy(&dev, 1));
    return rc;
}
