// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real Vulkan 1.1 + Mesa lavapipe + VVL + SyncVal contract test for VK_KHR_8bit_storage.
#define _POSIX_C_SOURCE 200809L
#include "nvn_backend.h"
#include "vk_device.h"
#include "nvn_plat_vk.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_hide_ext;
static int g_hide_ssbo;
static int g_hide_ubo;
static int g_hide_push;
static int g_hw_ssbo;
static int g_hw_ubo;
static int g_hw_push;
static int g_dev_ext;
static int g_dev_ssbo;
static int g_dev_ubo;
static int g_dev_push;
static uint64_t g_sig;
static nvn_gpu_err g_err;

#define REQ(x) do { if (!(x)) { fprintf(stderr, "【红】storage-8bit line %d: %s (why=%s)\n", __LINE__, #x, g_err.why); return 1; } } while (0)

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
        if (!strcmp(tmp[i].extensionName, VK_KHR_8BIT_STORAGE_EXTENSION_NAME)) continue;
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
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES_KHR) {
            VkPhysicalDevice8BitStorageFeaturesKHR *sf = (VkPhysicalDevice8BitStorageFeaturesKHR *)b;
            g_hw_ssbo = (sf->storageBuffer8BitAccess == VK_TRUE);
            g_hw_ubo = (sf->uniformAndStorageBuffer8BitAccess == VK_TRUE);
            g_hw_push = (sf->storagePushConstant8 == VK_TRUE);
            if (g_hide_ssbo) sf->storageBuffer8BitAccess = VK_FALSE;
            if (g_hide_ubo) sf->uniformAndStorageBuffer8BitAccess = VK_FALSE;
            if (g_hide_push) sf->storagePushConstant8 = VK_FALSE;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(
    VkPhysicalDevice p, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *d) {
    g_dev_ext = 0; g_dev_ssbo = 0; g_dev_ubo = 0; g_dev_push = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        if (!strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_8BIT_STORAGE_EXTENSION_NAME)) g_dev_ext = 1;
    }
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES_KHR) {
            const VkPhysicalDevice8BitStorageFeaturesKHR *sf = (const VkPhysicalDevice8BitStorageFeaturesKHR *)b;
            if (sf->storageBuffer8BitAccess == VK_TRUE) g_dev_ssbo = 1;
            if (sf->uniformAndStorageBuffer8BitAccess == VK_TRUE) g_dev_ubo = 1;
            if (sf->storagePushConstant8 == VK_TRUE) g_dev_push = 1;
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

static int run_std_case(const char *dir) {
    size_t bytes = 0;
    uint32_t *words = load_spv(dir, "s8_std.comp.spv", &bytes);
    if (!words) return 0;
    nvn_gpu_layout_ent ent = { .cls = NVN_GPU_RES_SSBO, .count = 1, .stages = 1u << NVN_GPU_SH_CS, .access = NVN_GPU_ACC_RW };
    nvn_gpu_layout layout = { .ent = &ent, .nent = 1 };
    nvn_gpu_shader_src src = { .stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV, .blob = words, .bytes = bytes, .entry = "main", .layout = &layout };
    nvn_gpu_shader sh = NULL;
    memset(&g_err, 0, sizeof g_err);
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh, &g_err);
    free(words);
    if (!nvn_gpu_ok(r) || !sh) return 0;
    nvn_gpu_stage_desc cs = { .sh = sh };
    nvn_gpu_pipe pipe = NULL;
    if (!nvn_gpu_ok(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &pipe, &g_err))) { nvn_gpu_shader_release(sh); return 0; }
    uint32_t init[8] = {0};
    nvn_gpu_buf buf = NULL;
    if (!nvn_gpu_ok(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_SHARED, init, &buf, &g_err))) { nvn_gpu_shader_release(sh); return 0; }
    nvn_gpu_cmd cmd = NULL;
    int ok = nvn_gpu_ok(nvn_gpu_cmd_begin(NULL, &cmd, &g_err));
    if (ok) {
        nvn_gpu_set_pipe(cmd, pipe);
        nvn_gpu_binding b = { .cls = NVN_GPU_RES_SSBO, .u.b = { .buf = buf, .len = sizeof init } };
        nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &b, 1);
        nvn_gpu_dispatch(cmd, pipe, 1, 1, 1, NULL, 0);
        ok = nvn_gpu_ok(nvn_gpu_cmd_commit(cmd, ++g_sig, done_cb, NULL, &g_err)) &&
             nvn_gpu_ok(nvn_gpu_fence_wait(g_sig, 5000000000ull, &g_err));
    }
    uint32_t got[8] = {0};
    if (ok) memcpy(got, nvn_gpu_buf_cpu(buf), sizeof got);
    nvn_gpu_buf_release(buf, NULL, NULL);
    nvn_gpu_shader_release(sh);
    for (uint32_t i = 0; ok && i < 8; i++) if (got[i] != (i + 1u) * 0x101u) ok = 0;
    return ok;
}

static int run_ssbo8_case(const char *dir, int expect_ok, const char *expect_why) {
    size_t bytes = 0;
    uint32_t *words = load_spv(dir, "s8_ssbo.comp.spv", &bytes);
    if (!words) return 0;
    nvn_gpu_layout_ent ent = { .cls = NVN_GPU_RES_SSBO, .count = 1, .stages = 1u << NVN_GPU_SH_CS, .access = NVN_GPU_ACC_RW };
    nvn_gpu_layout layout = { .ent = &ent, .nent = 1 };
    nvn_gpu_shader_src src = { .stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV, .blob = words, .bytes = bytes, .entry = "main", .layout = &layout };
    nvn_gpu_shader sh = NULL;
    memset(&g_err, 0, sizeof g_err);
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh, &g_err);
    free(words);
    if (!expect_ok) {
        int ok = (r.code == NVN_GPU_E_UNSUPPORTED && sh == NULL && (!expect_why || strstr(g_err.why, expect_why) != NULL));
        if (!ok) {
            fprintf(stderr, "【红】s8_ssbo expected reject(%s) got rc=%d why=%s\n", expect_why ? expect_why : "", r.code, g_err.why);
            if (sh) nvn_gpu_shader_release(sh);
        }
        return ok;
    }
    if (!nvn_gpu_ok(r) || !sh) {
        fprintf(stderr, "【红】s8_ssbo expected accept got rc=%d why=%s\n", r.code, g_err.why);
        return 0;
    }
    nvn_gpu_stage_desc cs = { .sh = sh };
    nvn_gpu_pipe pipe = NULL;
    if (!nvn_gpu_ok(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &pipe, &g_err))) {
        fprintf(stderr, "【红】s8_ssbo cpipe_get failed: %s\n", g_err.why);
        nvn_gpu_shader_release(sh); return 0;
    }
    uint8_t init[16] = { 10, 20, 30, 40, 240, 245, 250, 255, 0 };
    nvn_gpu_buf buf = NULL;
    if (!nvn_gpu_ok(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_SHARED, init, &buf, &g_err))) { nvn_gpu_shader_release(sh); return 0; }
    nvn_gpu_cmd cmd = NULL;
    int ok = nvn_gpu_ok(nvn_gpu_cmd_begin(NULL, &cmd, &g_err));
    if (ok) {
        nvn_gpu_set_pipe(cmd, pipe);
        nvn_gpu_binding b = { .cls = NVN_GPU_RES_SSBO, .u.b = { .buf = buf, .len = sizeof init } };
        nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &b, 1);
        nvn_gpu_dispatch(cmd, pipe, 1, 1, 1, NULL, 0);
        ok = nvn_gpu_ok(nvn_gpu_cmd_commit(cmd, ++g_sig, done_cb, NULL, &g_err)) &&
             nvn_gpu_ok(nvn_gpu_fence_wait(g_sig, 5000000000ull, &g_err));
    }
    uint8_t got[16] = {0};
    if (ok) memcpy(got, nvn_gpu_buf_cpu(buf), sizeof got);
    nvn_gpu_buf_release(buf, NULL, NULL);
    nvn_gpu_shader_release(sh);
    for (uint32_t i = 0; ok && i < 8; i++) {
        uint8_t want = (uint8_t)((init[i] + i * 3u + 7u) & 0xffu);
        if (got[i] != want) {
            fprintf(stderr, "【红】s8_ssbo mismatch[%u]: got=%u want=%u\n", i, got[i], want);
            ok = 0;
        }
    }
    return ok;
}

static int run_ubo8_case(const char *dir, int expect_ok, const char *expect_why) {
    size_t bytes = 0;
    uint32_t *words = load_spv(dir, "s8_ubo.comp.spv", &bytes);
    if (!words) return 0;
    nvn_gpu_layout_ent ents[2] = {
        { .set = 0, .binding = 0, .cls = NVN_GPU_RES_UBO, .count = 1, .stages = 1u << NVN_GPU_SH_CS, .access = NVN_GPU_ACC_R },
        { .set = 0, .binding = 1, .cls = NVN_GPU_RES_SSBO, .count = 1, .stages = 1u << NVN_GPU_SH_CS, .access = NVN_GPU_ACC_RW },
    };
    nvn_gpu_layout layout = { .ent = ents, .nent = 2 };
    nvn_gpu_shader_src src = { .stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV, .blob = words, .bytes = bytes, .entry = "main", .layout = &layout };
    nvn_gpu_shader sh = NULL;
    memset(&g_err, 0, sizeof g_err);
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh, &g_err);
    free(words);
    if (!expect_ok) {
        int ok = (r.code == NVN_GPU_E_UNSUPPORTED && sh == NULL && (!expect_why || strstr(g_err.why, expect_why) != NULL));
        if (!ok) {
            fprintf(stderr, "【红】s8_ubo expected reject(%s) got rc=%d why=%s\n", expect_why ? expect_why : "", r.code, g_err.why);
            if (sh) nvn_gpu_shader_release(sh);
        }
        return ok;
    }
    if (!nvn_gpu_ok(r) || !sh) {
        fprintf(stderr, "【红】s8_ubo expected accept got rc=%d why=%s\n", r.code, g_err.why);
        return 0;
    }
    nvn_gpu_stage_desc cs = { .sh = sh };
    nvn_gpu_pipe pipe = NULL;
    if (!nvn_gpu_ok(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &pipe, &g_err))) {
        fprintf(stderr, "【红】s8_ubo cpipe_get failed: %s\n", g_err.why);
        nvn_gpu_shader_release(sh); return 0;
    }
    uint8_t ubo_init[16] = { 0x11, 0x22, 0x33, 0x44, 0x88, 0x99, 0xaa, 0xf0, 0 };
    uint32_t out_init[8] = {0};
    nvn_gpu_buf ubo = NULL, outb = NULL;
    if (!nvn_gpu_ok(nvn_gpu_buf_create(sizeof ubo_init, NVN_GPU_MEM_SHARED, ubo_init, &ubo, &g_err)) ||
        !nvn_gpu_ok(nvn_gpu_buf_create(sizeof out_init, NVN_GPU_MEM_SHARED, out_init, &outb, &g_err))) {
        if (ubo) nvn_gpu_buf_release(ubo, NULL, NULL);
        if (outb) nvn_gpu_buf_release(outb, NULL, NULL);
        nvn_gpu_shader_release(sh); return 0;
    }
    nvn_gpu_cmd cmd = NULL;
    int ok = nvn_gpu_ok(nvn_gpu_cmd_begin(NULL, &cmd, &g_err));
    if (ok) {
        nvn_gpu_set_pipe(cmd, pipe);
        nvn_gpu_binding binds[2] = {
            { .ent = 0, .cls = NVN_GPU_RES_UBO, .u.b = { .buf = ubo, .len = sizeof ubo_init } },
            { .ent = 1, .cls = NVN_GPU_RES_SSBO, .u.b = { .buf = outb, .len = sizeof out_init } },
        };
        nvn_gpu_bind(cmd, NVN_GPU_SH_CS, binds, 2);
        nvn_gpu_dispatch(cmd, pipe, 1, 1, 1, NULL, 0);
        ok = nvn_gpu_ok(nvn_gpu_cmd_commit(cmd, ++g_sig, done_cb, NULL, &g_err)) &&
             nvn_gpu_ok(nvn_gpu_fence_wait(g_sig, 5000000000ull, &g_err));
    }
    uint32_t got[8] = {0};
    if (ok) memcpy(got, nvn_gpu_buf_cpu(outb), sizeof got);
    nvn_gpu_buf_release(ubo, NULL, NULL);
    nvn_gpu_buf_release(outb, NULL, NULL);
    nvn_gpu_shader_release(sh);
    for (uint32_t i = 0; ok && i < 8; i++) {
        uint32_t want = (uint32_t)ubo_init[i] ^ 0x5au;
        if (got[i] != want) {
            fprintf(stderr, "【红】s8_ubo mismatch[%u]: got=%#x want=%#x\n", i, got[i], want);
            ok = 0;
        }
    }
    return ok;
}

static int run_push8_case(const char *dir, int expect_ok, const char *expect_why) {
    size_t bytes = 0;
    uint32_t *words = load_spv(dir, "s8_push.comp.spv", &bytes);
    if (!words) return 0;
    nvn_gpu_layout_ent ent = { .cls = NVN_GPU_RES_SSBO, .count = 1, .stages = 1u << NVN_GPU_SH_CS, .access = NVN_GPU_ACC_RW };
    nvn_gpu_layout layout = { .ent = &ent, .nent = 1, .push_off = 0, .push_bytes = 8 };
    nvn_gpu_shader_src src = { .stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV, .blob = words, .bytes = bytes, .entry = "main", .layout = &layout };
    nvn_gpu_shader sh = NULL;
    memset(&g_err, 0, sizeof g_err);
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh, &g_err);
    free(words);
    if (!expect_ok) {
        int ok = (r.code == NVN_GPU_E_UNSUPPORTED && sh == NULL && (!expect_why || strstr(g_err.why, expect_why) != NULL));
        if (!ok) {
            fprintf(stderr, "【红】s8_push expected reject(%s) got rc=%d why=%s\n", expect_why ? expect_why : "", r.code, g_err.why);
            if (sh) nvn_gpu_shader_release(sh);
        }
        return ok;
    }
    if (!nvn_gpu_ok(r) || !sh) {
        fprintf(stderr, "【红】s8_push expected accept got rc=%d why=%s\n", r.code, g_err.why);
        return 0;
    }
    nvn_gpu_stage_desc cs = { .sh = sh };
    nvn_gpu_pipe pipe = NULL;
    if (!nvn_gpu_ok(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &pipe, &g_err))) {
        fprintf(stderr, "【红】s8_push cpipe_get failed: %s\n", g_err.why);
        nvn_gpu_shader_release(sh); return 0;
    }
    uint8_t pc[8] = { 5, 17, 42, 99, 128, 200, 231, 254 };
    uint32_t out_init[8] = {0};
    nvn_gpu_buf outb = NULL;
    if (!nvn_gpu_ok(nvn_gpu_buf_create(sizeof out_init, NVN_GPU_MEM_SHARED, out_init, &outb, &g_err))) { nvn_gpu_shader_release(sh); return 0; }
    nvn_gpu_cmd cmd = NULL;
    int ok = nvn_gpu_ok(nvn_gpu_cmd_begin(NULL, &cmd, &g_err));
    if (ok) {
        nvn_gpu_set_pipe(cmd, pipe);
        nvn_gpu_binding b = { .cls = NVN_GPU_RES_SSBO, .u.b = { .buf = outb, .len = sizeof out_init } };
        nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &b, 1);
        nvn_gpu_push(cmd, NVN_GPU_SH_CS, 0, pc, sizeof pc);
        nvn_gpu_dispatch(cmd, pipe, 1, 1, 1, NULL, 0);
        ok = nvn_gpu_ok(nvn_gpu_cmd_commit(cmd, ++g_sig, done_cb, NULL, &g_err)) &&
             nvn_gpu_ok(nvn_gpu_fence_wait(g_sig, 5000000000ull, &g_err));
    }
    uint32_t got[8] = {0};
    if (ok) memcpy(got, nvn_gpu_buf_cpu(outb), sizeof got);
    nvn_gpu_buf_release(outb, NULL, NULL);
    nvn_gpu_shader_release(sh);
    for (uint32_t i = 0; ok && i < 8; i++) {
        uint32_t want = (uint32_t)pc[i] + 0x100u;
        if (got[i] != want) {
            fprintf(stderr, "【红】s8_push mismatch[%u]: got=%#x want=%#x\n", i, got[i], want);
            ok = 0;
        }
    }
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <shader_dir> <on|off|hide-ext|hide-ssbo|hide-ubo|hide-push|withdraw>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1], *mode = argv[2];
    if (!strcmp(mode, "off") || !strcmp(mode, "withdraw")) setenv("debug.s2a.nvk_8bit_storage", "0", 1);
    else setenv("debug.s2a.nvk_8bit_storage", "1", 1);
    g_hide_ext = !strcmp(mode, "hide-ext");
    g_hide_ssbo = !strcmp(mode, "hide-ssbo");
    g_hide_ubo = !strcmp(mode, "hide-ubo");
    g_hide_push = !strcmp(mode, "hide-push");

    struct s2a_vk_device dev;
    char why[256] = "";
    REQ(!s2a_vk_device_create(&dev, why, sizeof why));
    if (!strcmp(mode, "on") || !strcmp(mode, "withdraw")) {
        // Real test device (e.g. lavapipe) must support at least storageBuffer8BitAccess.
        REQ(g_hw_ssbo || g_hw_ubo || g_hw_push);
    }
    const int active = !strcmp(mode, "on") || !strcmp(mode, "withdraw") || g_hide_ssbo || g_hide_ubo || g_hide_push;
    const int expect_ssbo = (active && !g_hide_ssbo) ? g_hw_ssbo : 0;
    const int expect_ubo  = (active && !g_hide_ubo)  ? g_hw_ubo  : 0;
    const int expect_push = (active && !g_hide_push) ? g_hw_push : 0;
    const int expect_ext  = (expect_ssbo || expect_ubo || expect_push);

    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    od.app_label = "storage-8bit-test";
    REQ(nvn_gpu_ok(nvn_gpu_open(&od, &g_err)));

    int std_ok  = run_std_case(dir);
    int ssbo_ok = run_ssbo8_case(dir, expect_ssbo, expect_ssbo ? NULL : "storageBuffer8BitAccess");
    int ubo_ok  = run_ubo8_case(dir, expect_ubo, expect_ubo ? NULL : "uniformAndStorageBuffer8BitAccess");
    int push_ok = run_push8_case(dir, expect_push, expect_push ? NULL : "storagePushConstant8");
    printf("storage-8bit mode=%s hw=%d/%d/%d dev_ext=%d dev_bits=%d/%d/%d en_bits=%d/%d/%d std=%d ssbo=%d ubo=%d push=%d\n",
           mode, g_hw_ssbo, g_hw_ubo, g_hw_push, g_dev_ext, g_dev_ssbo, g_dev_ubo, g_dev_push,
           dev.storage_8bit_ssbo_enabled, dev.storage_8bit_ubo_ssbo_enabled, dev.storage_8bit_push_enabled,
           std_ok, ssbo_ok, ubo_ok, push_ok);

    int rc = 0;
    if (g_dev_ext != expect_ext || g_dev_ssbo != expect_ssbo || g_dev_ubo != expect_ubo || g_dev_push != expect_push ||
        dev.storage_8bit_ssbo_enabled != expect_ssbo || dev.storage_8bit_ubo_ssbo_enabled != expect_ubo ||
        dev.storage_8bit_push_enabled != expect_push || !std_ok || !ssbo_ok || !ubo_ok || !push_ok) {
        rc = 1;
    }

    REQ(nvn_gpu_ok(nvn_gpu_close(5000000000ull, &g_err)));
    REQ(!s2a_vk_device_destroy(&dev, 1));
    return rc;
}
