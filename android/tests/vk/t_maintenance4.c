// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real Vulkan 1.1 + Mesa lavapipe + VVL + SyncVal contract test for VK_KHR_maintenance4:
//   1) Optional Vulkan 1.1 negotiation (ext + feature + maxBufferSize + dynamic vkGetDeviceProcAddr)
//   2) Real s2a_vk_buffer_memory_requirements / s2a_vk_image_memory_requirements cross-checked
//      against vkGetBufferMemoryRequirements / vkGetImageMemoryRequirements (both KHR fast path & 1.1 fallback)
//   3) Real NVN compute pipeline dispatch using SPIR-V 1.3 OpExecutionModeId LocalSizeId with GPU readback
//   4) Pre-driver LocalSizeId workgroup limit guard + off/hide-ext/hide-feat/withdraw negative controls
#define _POSIX_C_SOURCE 200809L
#include "nvn_backend.h"
#include "vk_device.h"
#include "nvn_plat_vk.h"
#include "../../../graphics/third_party/spirv_cross/spirv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_hide_ext;
static int g_hide_feat;
static int g_dev_ext;
static int g_dev_feat;
static int g_proc_buf_mr;
static int g_proc_img_mr;
static uint64_t g_sig;
static nvn_gpu_err g_err;

#define REQ(x) do { if (!(x)) { fprintf(stderr, "【红】maint4 line %d: %s (why=%s)\n", __LINE__, #x, g_err.why); return 1; } } while (0)

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
extern __typeof__(vkGetDeviceProcAddr) __real_vkGetDeviceProcAddr;

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
        if (!strcmp(tmp[i].extensionName, VK_KHR_MAINTENANCE_4_EXTENSION_NAME)) continue;
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
    if (!g_hide_feat || !f) return;
    for (VkBaseOutStructure *b = f->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES_KHR) {
            ((VkPhysicalDeviceMaintenance4FeaturesKHR *)b)->maintenance4 = VK_FALSE;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(
    VkPhysicalDevice p, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *d) {
    g_dev_ext = 0; g_dev_feat = 0; g_proc_buf_mr = 0; g_proc_img_mr = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        if (!strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_MAINTENANCE_4_EXTENSION_NAME)) g_dev_ext = 1;
    }
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES_KHR) {
            if (((const VkPhysicalDeviceMaintenance4FeaturesKHR *)b)->maintenance4 == VK_TRUE) g_dev_feat = 1;
        }
    }
    return __real_vkCreateDevice(p, ci, a, d);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice d, const char *name) {
    PFN_vkVoidFunction fn = __real_vkGetDeviceProcAddr(d, name);
    if (name && !strcmp(name, "vkGetDeviceBufferMemoryRequirementsKHR") && fn) g_proc_buf_mr = 1;
    if (name && !strcmp(name, "vkGetDeviceImageMemoryRequirementsKHR") && fn) g_proc_img_mr = 1;
    return fn;
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

// Convert OpExecutionMode %main LocalSize 4 1 1 -> OpExecutionModeId %main LocalSizeId %c4 %c1 %c1 (SPIR-V 1.3).
static int patch_to_local_size_id(uint32_t *w, size_t bytes) {
    size_t n = bytes / 4;
    if (n < 5 || w[0] != SpvMagicNumber) return -1;
    if (w[1] < 0x00010300u) w[1] = 0x00010300u;
    uint32_t u32_ty = 0, c4 = 0, c1 = 0;
    for (size_t i = 5; i < n;) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffffu;
        if (!len || len > n - i) return -1;
        if (op == SpvOpTypeInt && len == 4 && w[i + 2] == 32 && w[i + 3] == 0) u32_ty = w[i + 1];
        i += len;
    }
    if (!u32_ty) return -1;
    for (size_t i = 5; i < n;) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffffu;
        if (op == SpvOpConstant && len == 4 && w[i + 1] == u32_ty) {
            if (w[i + 3] == 4u && !c4) c4 = w[i + 2];
            if (w[i + 3] == 1u && !c1) c1 = w[i + 2];
        }
        i += len;
    }
    if (!c4 || !c1) return -1;
    int patched = 0;
    for (size_t i = 5; i < n;) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffffu;
        if (op == SpvOpExecutionMode && len == 6 && w[i + 2] == SpvExecutionModeLocalSize) {
            w[i] = (6u << 16) | SpvOpExecutionModeId;
            w[i + 2] = SpvExecutionModeLocalSizeId;
            w[i + 3] = c4;
            w[i + 4] = c1;
            w[i + 5] = c1;
            patched++;
        }
        i += len;
    }
    return patched == 1 ? 0 : -1;
}

static int verify_memory_requirements(const struct s2a_vk_device *dev) {
    VkBufferCreateInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = 4096;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkMemoryRequirements got_b = {0}, ref_b = {0};
    if (s2a_vk_buffer_memory_requirements(dev, &bi, &got_b) != VK_SUCCESS) return 0;
    VkBuffer buf = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev->device, &bi, NULL, &buf) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(dev->device, buf, &ref_b);
    vkDestroyBuffer(dev->device, buf, NULL);
    if (got_b.size < 4096 || got_b.alignment == 0 || got_b.memoryTypeBits == 0 ||
        got_b.size != ref_b.size || got_b.alignment != ref_b.alignment ||
        got_b.memoryTypeBits != ref_b.memoryTypeBits) {
        fprintf(stderr, "【红】buffer memreq mismatch: got=(%llu,%llu,%#x) ref=(%llu,%llu,%#x)\n",
                (unsigned long long)got_b.size, (unsigned long long)got_b.alignment, got_b.memoryTypeBits,
                (unsigned long long)ref_b.size, (unsigned long long)ref_b.alignment, ref_b.memoryTypeBits);
        return 0;
    }

    VkImageCreateInfo ii = {0};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width = 64; ii.extent.height = 64; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements got_i = {0}, ref_i = {0};
    if (s2a_vk_image_memory_requirements(dev, &ii, &got_i) != VK_SUCCESS) return 0;
    VkImage img = VK_NULL_HANDLE;
    if (vkCreateImage(dev->device, &ii, NULL, &img) != VK_SUCCESS) return 0;
    vkGetImageMemoryRequirements(dev->device, img, &ref_i);
    vkDestroyImage(dev->device, img, NULL);
    if (got_i.size == 0 || got_i.alignment == 0 || got_i.memoryTypeBits == 0 ||
        got_i.size != ref_i.size || got_i.alignment != ref_i.alignment ||
        got_i.memoryTypeBits != ref_i.memoryTypeBits) {
        fprintf(stderr, "【红】image memreq mismatch: got=(%llu,%llu,%#x) ref=(%llu,%llu,%#x)\n",
                (unsigned long long)got_i.size, (unsigned long long)got_i.alignment, got_i.memoryTypeBits,
                (unsigned long long)ref_i.size, (unsigned long long)ref_i.alignment, ref_i.memoryTypeBits);
        return 0;
    }
    return 1;
}

static int dispatch_check(const uint32_t *words, size_t bytes, int expect_ok, const char *expect_why) {
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
    if (!expect_ok) {
        if (r.code != NVN_GPU_E_UNSUPPORTED || sh != NULL || (expect_why && !strstr(g_err.why, expect_why))) {
            fprintf(stderr, "【红】expected reject(%s) got rc=%d why=%s\n", expect_why ? expect_why : "", r.code, g_err.why);
            if (sh) nvn_gpu_shader_release(sh);
            return 1;
        }
        return 0;
    }
    if (!nvn_gpu_ok(r) || !sh) {
        fprintf(stderr, "【红】expected shader_load accept got rc=%d why=%s\n", r.code, g_err.why);
        return 1;
    }
    nvn_gpu_stage_desc cs = {0}; cs.sh = sh;
    nvn_gpu_pipe pipe = NULL;
    REQ(nvn_gpu_ok(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &pipe, &g_err)));
    uint32_t init[8] = {0};
    nvn_gpu_buf buf = NULL;
    REQ(nvn_gpu_ok(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_SHARED, init, &buf, &g_err)));
    nvn_gpu_cmd cmd = NULL;
    REQ(nvn_gpu_ok(nvn_gpu_cmd_begin(NULL, &cmd, &g_err)));
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_binding b = {0};
    b.cls = NVN_GPU_RES_SSBO;
    b.u.b.buf = buf;
    b.u.b.len = sizeof init;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(cmd, pipe, 2, 1, 1, NULL, 0);
    REQ(nvn_gpu_ok(nvn_gpu_cmd_commit(cmd, ++g_sig, done_cb, NULL, &g_err)));
    REQ(nvn_gpu_ok(nvn_gpu_fence_wait(g_sig, 5000000000ull, &g_err)));
    uint32_t got[8] = {0};
    memcpy(got, nvn_gpu_buf_cpu(buf), sizeof got);
    nvn_gpu_buf_release(buf, NULL, NULL);
    nvn_gpu_shader_release(sh);
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t want = (i + 1u) * 4u + 1u;
        if (got[i] != want) {
            fprintf(stderr, "【红】SSBO mismatch at %u: got=%u want=%u\n", i, got[i], want);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <shader_dir> <on|off|hide-ext|hide-feat|withdraw>\n", argv[0]); return 2; }
    const char *dir = argv[1], *mode = argv[2];
    int expect_on = (!strcmp(mode, "on") || !strcmp(mode, "withdraw"));
    if (!strcmp(mode, "off") || !strcmp(mode, "withdraw")) setenv("debug.s2a.nvk_maintenance4", "0", 1);
    else setenv("debug.s2a.nvk_maintenance4", "1", 1);
    g_hide_ext = !strcmp(mode, "hide-ext");
    g_hide_feat = !strcmp(mode, "hide-feat");

    struct s2a_vk_device dev;
    char why[256] = "";
    REQ(!s2a_vk_device_create(&dev, why, sizeof why));
    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    od.app_label = "maintenance4-test";
    REQ(nvn_gpu_ok(nvn_gpu_open(&od, &g_err)));

    size_t bytes = 0;
    uint32_t *std_spv = load_spv(dir, "maint4_localsizeid.comp.spv", &bytes);
    REQ(std_spv != NULL);
    uint32_t *id_spv = malloc(bytes);
    REQ(id_spv != NULL);
    memcpy(id_spv, std_spv, bytes);
    REQ(patch_to_local_size_id(id_spv, bytes) == 0);

    int memreq_ok = verify_memory_requirements(&dev);
    int std_ok = (dispatch_check(std_spv, bytes, 1, NULL) == 0);
    int id_ok = (dispatch_check(id_spv, bytes, expect_on, expect_on ? NULL : "maintenance4") == 0);
    int limit_guard_ok = 1;
    if (expect_on && dev.maintenance4_enabled) {
        uint32_t saved_x = dev.properties.limits.maxComputeWorkGroupSize[0];
        dev.properties.limits.maxComputeWorkGroupSize[0] = 2;
        limit_guard_ok = (dispatch_check(id_spv, bytes, 0, "maxComputeWorkGroupSize") == 0);
        dev.properties.limits.maxComputeWorkGroupSize[0] = saved_x;
    }
    int max_buf_ok = expect_on ? (dev.max_buffer_size > 0) : (dev.max_buffer_size == 0);
    int proc_ok = (g_proc_buf_mr == expect_on) && (g_proc_img_mr == expect_on) &&
                  ((dev.get_device_buffer_memory_requirements != NULL) == expect_on) &&
                  ((dev.get_device_image_memory_requirements != NULL) == expect_on);
    printf("maintenance4 mode=%s dev_ext=%d dev_feat=%d enabled=%d max_buf=%llu proc=%d/%d memreq=%d std=%d id=%d lim_guard=%d\n",
           mode, g_dev_ext, g_dev_feat, dev.maintenance4_enabled,
           (unsigned long long)dev.max_buffer_size, g_proc_buf_mr, g_proc_img_mr,
           memreq_ok, std_ok, id_ok, limit_guard_ok);

    int rc = 0;
    if (g_dev_ext != expect_on || g_dev_feat != expect_on || dev.maintenance4_enabled != expect_on ||
        !max_buf_ok || !proc_ok || !memreq_ok || !std_ok || !id_ok || !limit_guard_ok) {
        rc = 1;
    }

    free(id_spv);
    free(std_spv);
    REQ(nvn_gpu_ok(nvn_gpu_close(5000000000ull, &g_err)));
    REQ(!s2a_vk_device_destroy(&dev, 1));
    return rc;
}
