// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// VK_EXT_image_robustness 合同测试（Mesa lavapipe + VVL 1.4.341 + SyncVal）。
// 验证：
//  1) mode=on：在 Vulkan 1.1 基线下协商 VK_EXT_image_robustness（robustImageAccess=VK_TRUE），
//     实测越界 texelFetch（正/负坐标）与越界 imageLoad（正/负坐标）返回规范允许的确定性零值 (0,0,0,0/1)，
//     越界 imageStore 不污染界内纹素，界内读写逐字节精确，VVL + SyncVal 0 错。
//  2) mode=off / hide-ext / hide-feat：不请求扩展/特性，界内读写照常精确，VVL + SyncVal 0 错。
//  3) mode=withdraw：关开关按启用期望验收，必报红退非零（负对照）。
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn_backend.h"
#include "nvn_plat.h"
#include "vk_device.h"

int nvn_plat_flag(const char *name, int dflt) { (void)name; return dflt; }
void nvn_plat_log(int level, const char *cls, const char *text) { (void)level; (void)cls; (void)text; }

extern VkResult __real_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *);
extern void __real_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
extern VkResult __real_vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);

static int g_hide_ext, g_hide_feat;
static unsigned g_dev_ext_enabled, g_dev_feat_enabled;

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
        if (!strcmp(tmp[i].extensionName, VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME)) continue;
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
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT)
            ((VkPhysicalDeviceImageRobustnessFeaturesEXT *)b)->robustImageAccess = VK_FALSE;
    }
}

VkResult __wrap_vkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *out) {
    if (ci) {
        for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
            if (!strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME))
                g_dev_ext_enabled++;
        for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
            if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT &&
                ((const VkPhysicalDeviceImageRobustnessFeaturesEXT *)b)->robustImageAccess == VK_TRUE)
                g_dev_feat_enabled++;
        }
    }
    return __real_vkCreateDevice(phys, ci, alloc, out);
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

static int near_f(float a, float b) { return fabsf(a - b) <= 0.01f; }
static int is_robust_oob_zero(const float v[4]) {
    // Vulkan VK_EXT_image_robustness 规范：RGBA 格式越界读返回 (0, 0, 0, 0) 或 (0, 0, 0, 1)
    return near_f(v[0], 0.0f) && near_f(v[1], 0.0f) && near_f(v[2], 0.0f) &&
           (near_f(v[3], 0.0f) || near_f(v[3], 1.0f));
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <shader_dir> <on|off|hide-ext|hide-feat|withdraw>\n", argv[0]);
        return 2;
    }
    const char *sdir = argv[1];
    const char *mode = argv[2];
    const int is_withdraw = !strcmp(mode, "withdraw");
    if (!strcmp(mode, "off") || is_withdraw) {
        setenv("debug.s2a.nvk_image_robustness", "0", 1);
    } else {
        unsetenv("debug.s2a.nvk_image_robustness");
        if (!strcmp(mode, "hide-ext")) g_hide_ext = 1;
        else if (!strcmp(mode, "hide-feat")) g_hide_feat = 1;
        else if (strcmp(mode, "on") != 0) return 2;
    }
    const int expect_on = !strcmp(mode, "on") || is_withdraw;

    char sp[512];
    snprintf(sp, sizeof sp, "%s/image_robust_oob.comp.spv", sdir);
    void *spv = NULL;
    size_t spv_len = 0;
    REQUIRE(load_spv(sp, &spv, &spv_len));

    struct s2a_vk_device dev = {0};
    char why[256] = "";
    REQUIRE(s2a_vk_device_create(&dev, why, sizeof why) == 0);
    // 只在 vkCreateDevice 实际请求扩展与特性时执行越界路径。回退档验证定义良好的界内读写。
    const int probe_oob = g_dev_ext_enabled == 1 && g_dev_feat_enabled == 1;

    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&od, &err));

    // 两张 4x4 RGBA8 纹理：tex（采样）与 img（存储读写）
    nvn_gpu_tex_desc td = {0};
    td.d.width = 4; td.d.height = 4; td.d.depth = 1; td.d.levels = 1;
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8;
    td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD;
    nvn_gpu_tex tex = NULL, img = NULL;
    GPU(nvn_gpu_tex_create(&td, &tex, &err));
    td.usage = NVN_GPU_TEX_STORAGE | NVN_GPU_TEX_CPU_UPLOAD;
    GPU(nvn_gpu_tex_create(&td, &img, &err));

    uint8_t tex_px[4 * 4 * 4], img_px[4 * 4 * 4];
    for (int i = 0; i < 16; i++) {
        tex_px[i * 4 + 0] = 64;  tex_px[i * 4 + 1] = 128; tex_px[i * 4 + 2] = 192; tex_px[i * 4 + 3] = 255;
        img_px[i * 4 + 0] = 128; img_px[i * 4 + 1] = 64;  img_px[i * 4 + 2] = 255; img_px[i * 4 + 3] = 192;
    }

    nvn_gpu_view v_tex = NULL, v_img = NULL;
    GPU(nvn_gpu_view_create(tex, NULL, &v_tex, &err));
    GPU(nvn_gpu_view_create(img, NULL, &v_img, &err));
    samp_par spar = {0};
    nvn_gpu_samp samp = NULL;
    GPU(nvn_gpu_samp_create(&spar, &samp, &err));

    nvn_gpu_buf out_buf = NULL, rb_img = NULL;
    GPU(nvn_gpu_buf_create(6 * 16, NVN_GPU_MEM_SHARED, NULL, &out_buf, &err));
    GPU(nvn_gpu_buf_create(64, NVN_GPU_MEM_SHARED, NULL, &rb_img, &err));
    memset(nvn_gpu_buf_cpu(out_buf), 0x7F, 6 * 16);
    memset(nvn_gpu_buf_cpu(rb_img), 0, 64);

    nvn_gpu_layout_ent ents[3] = {
        { .set = 0, .binding = 0, .cls = NVN_GPU_RES_TEX_SAMP, .access = NVN_GPU_ACC_R, .count = 1, .stages = 1u << NVN_GPU_SH_CS },
        { .set = 0, .binding = 1, .cls = NVN_GPU_RES_IMAGE, .access = NVN_GPU_ACC_RW, .count = 1, .stages = 1u << NVN_GPU_SH_CS },
        { .set = 0, .binding = 2, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS },
    };
    nvn_gpu_layout lay = { .ent = ents, .nent = 3 };
    nvn_gpu_shader sh = NULL;
    nvn_gpu_shader_src ssrc = { .stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV, .blob = spv, .bytes = spv_len, .entry = "main", .layout = &lay };
    GPU(nvn_gpu_shader_load(&ssrc, &sh, &err));
    nvn_gpu_stage_desc csd = { .sh = sh, .nspec = 1 };
    csd.spec[0] = (nvn_gpu_spec){ .id = 0, .type = NVN_GPU_SPEC_UINT, .bits = (uint32_t)probe_oob };
    nvn_gpu_pipe cp = NULL;
    GPU(nvn_gpu_cpipe_get(&csd, NVN_GPU_PIPE_SYNC, &cp, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    NVNcopyRegion reg = {0, 0, 0, 4, 4, 1};
    nvn_gpu_tex_upload(cmd, tex, 0, 0, &reg, tex_px, 16, 64);
    nvn_gpu_tex_upload(cmd, img, 0, 0, &reg, img_px, 16, 64);
    nvn_gpu_set_pipe(cmd, cp);
    nvn_gpu_binding binds[3] = {0};
    binds[0].ent = 0; binds[0].cls = NVN_GPU_RES_TEX_SAMP; binds[0].u.ts.view = v_tex; binds[0].u.ts.samp = samp;
    binds[1].ent = 1; binds[1].cls = NVN_GPU_RES_IMAGE; binds[1].u.view = v_img;
    binds[2].ent = 2; binds[2].cls = NVN_GPU_RES_SSBO; binds[2].u.b.buf = out_buf; binds[2].u.b.len = 6 * 16;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, binds, 3);
    nvn_gpu_dispatch(cmd, cp, 1, 1, 1, NULL, 0);
    nvn_gpu_copy_tex_buf(cmd, img, 0, 0, &reg, rb_img, 0, 16, 64);
    GPU(nvn_gpu_cmd_commit(cmd, 1, NULL, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));

    const float (*got)[4] = (const float (*)[4])nvn_gpu_buf_cpu(out_buf);
    const int in_tex_ok = near_f(got[0][0], 64.0f / 255.0f) && near_f(got[0][1], 128.0f / 255.0f) &&
                          near_f(got[0][2], 192.0f / 255.0f) && near_f(got[0][3], 1.0f);
    const int in_img_ok = near_f(got[3][0], 128.0f / 255.0f) && near_f(got[3][1], 64.0f / 255.0f) &&
                          near_f(got[3][2], 1.0f) && near_f(got[3][3], 192.0f / 255.0f);
    const int oob_ok = probe_oob && is_robust_oob_zero(got[1]) && is_robust_oob_zero(got[2]) &&
                       is_robust_oob_zero(got[4]) && is_robust_oob_zero(got[5]);

    const uint8_t *img_after = (const uint8_t *)nvn_gpu_buf_cpu(rb_img);
    int store_ok = (img_after[0] == 0 && img_after[1] == 255 && img_after[2] == 0 && img_after[3] == 255);
    for (int i = 1; i < 16; i++) {
        if (memcmp(img_after + i * 4, img_px + i * 4, 4) != 0) store_ok = 0;
    }

    printf("image-robustness mode=%s dev_ext=%u dev_feat=%u in_tex=%d in_img=%d oob_tested=%d oob_zero=%d store_safe=%d\n",
           mode, g_dev_ext_enabled, g_dev_feat_enabled, in_tex_ok, in_img_ok, probe_oob, oob_ok, store_ok);

    nvn_gpu_buf_release(rb_img, NULL, NULL);
    nvn_gpu_buf_release(out_buf, NULL, NULL);
    nvn_gpu_samp_release(samp);
    nvn_gpu_view_release(v_img);
    nvn_gpu_view_release(v_tex);
    nvn_gpu_tex_release(img);
    nvn_gpu_tex_release(tex);
    nvn_gpu_shader_release(sh);
    GPU(nvn_gpu_close(5000000000ull, &err));
    s2a_vk_device_destroy(&dev, 1);
    free(spv);

    REQUIRE(in_tex_ok && in_img_ok && store_ok);
    if (expect_on) {
        if (g_dev_ext_enabled != 1 || g_dev_feat_enabled != 1 || !oob_ok) {
            fprintf(stderr, "【红】image_robustness expected active: dev_ext=%u dev_feat=%u oob_zero=%d\n",
                    g_dev_ext_enabled, g_dev_feat_enabled, oob_ok);
            return 1;
        }
    } else {
        REQUIRE(g_dev_ext_enabled == 0 && g_dev_feat_enabled == 0);
    }
    return 0;
}
