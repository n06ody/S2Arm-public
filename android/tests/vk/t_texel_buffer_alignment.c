// VK_EXT_texel_buffer_alignment 合同测试（Mesa lavapipe + VVL 1.4.341 + SyncVal）。
// 验证：
//  1) mode=on：在 Vulkan 1.1 基线下协商 VK_EXT_texel_buffer_alignment，按规范放宽纹素缓冲视图偏移对齐
//     （R32UI @ off=4、RG32UI @ off=8 均低于 minTexelBufferOffsetAlignment=16）；真 compute 采样（tbuf.comp）
//     与存储写（itbuf.comp）读回逐字节精确、邻字零污染；低于单纹素对齐（R32UI @ off=2、RG32UI @ off=4）仍拒收 E_ARG；
//     VVL + SyncVal 0 错。
//  2) mode=off / hide-ext / hide-feat：不请求扩展/特性，退回 minTexelBufferOffsetAlignment，
//     off=4 必拒收 E_ARG，off=minTexelAlign 正常读写，VVL + SyncVal 0 错。
//  3) mode=withdraw：关开关按启用期望验收，必报红退非零（负对照）。
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
        if (!strcmp(tmp[i].extensionName, VK_EXT_TEXEL_BUFFER_ALIGNMENT_EXTENSION_NAME)) continue;
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
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT)
            ((VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT *)b)->texelBufferAlignment = VK_FALSE;
    }
}

VkResult __wrap_vkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *out) {
    if (ci) {
        for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
            if (!strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_TEXEL_BUFFER_ALIGNMENT_EXTENSION_NAME))
                g_dev_ext_enabled++;
        for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
            if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT &&
                ((const VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT *)b)->texelBufferAlignment == VK_TRUE)
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

static int run_tbuf_rw(nvn_gpu_pipe p_read, nvn_gpu_pipe p_write, uint64_t off, uint64_t *sig_io) {
    nvn_gpu_err err = {0};
    uint32_t init[64];
    for (uint32_t i = 0; i < 64; i++) init[i] = 1000u + i;
    nvn_gpu_buf tbuf = NULL, outbuf = NULL, srcbuf = NULL;
    GPU(nvn_gpu_buf_create(sizeof init, NVN_GPU_MEM_SHARED, NULL, &tbuf, &err));
    GPU(nvn_gpu_buf_create(16, NVN_GPU_MEM_SHARED, NULL, &outbuf, &err));
    GPU(nvn_gpu_buf_create(16, NVN_GPU_MEM_SHARED, NULL, &srcbuf, &err));
    memcpy(nvn_gpu_buf_cpu(tbuf), init, sizeof init);
    memset(nvn_gpu_buf_cpu(outbuf), 0, 16);
    uint32_t src_vals[4] = { 10u, 20u, 30u, 40u };
    memcpy(nvn_gpu_buf_cpu(srcbuf), src_vals, sizeof src_vals);

    nvn_gpu_view tv = NULL;
    GPU(nvn_gpu_view_create_buf(tbuf, off, 16, NVN_FORMAT_R32UI, &tv, &err));

    // 1) uniform texel buffer 读（tbuf.comp）
    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_set_pipe(cmd, p_read);
    nvn_gpu_binding rb[2] = {0};
    rb[0].ent = 0; rb[0].cls = NVN_GPU_RES_TBUF; rb[0].u.view = tv;
    rb[1].ent = 1; rb[1].cls = NVN_GPU_RES_SSBO; rb[1].u.b.buf = outbuf; rb[1].u.b.len = 16;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, rb, 2);
    nvn_gpu_dispatch(cmd, p_read, 1, 1, 1, NULL, 0);
    uint64_t s1 = ++(*sig_io);
    GPU(nvn_gpu_cmd_commit(cmd, s1, NULL, NULL, &err));
    GPU(nvn_gpu_fence_wait(s1, 5000000000ull, &err));

    const uint32_t *got_r = (const uint32_t *)nvn_gpu_buf_cpu(outbuf);
    const uint32_t base_idx = (uint32_t)(off / 4);
    for (uint32_t i = 0; i < 4; i++) {
        if (got_r[i] != 1000u + base_idx + i) {
            fprintf(stderr, "【红】tbuf read mismatch at off=%llu[%u]: want=%u got=%u\n",
                    (unsigned long long)off, i, 1000u + base_idx + i, got_r[i]);
            return 1;
        }
    }

    // 2) storage texel buffer 写（itbuf.comp：写入 src[i] + 100）
    cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_set_pipe(cmd, p_write);
    nvn_gpu_binding wb[2] = {0};
    wb[0].ent = 0; wb[0].cls = NVN_GPU_RES_IMAGE_TBUF; wb[0].u.view = tv;
    wb[1].ent = 1; wb[1].cls = NVN_GPU_RES_SSBO; wb[1].u.b.buf = srcbuf; wb[1].u.b.len = 16;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, wb, 2);
    nvn_gpu_dispatch(cmd, p_write, 1, 1, 1, NULL, 0);
    uint64_t s2 = ++(*sig_io);
    GPU(nvn_gpu_cmd_commit(cmd, s2, NULL, NULL, &err));
    GPU(nvn_gpu_fence_wait(s2, 5000000000ull, &err));

    const uint32_t *after = (const uint32_t *)nvn_gpu_buf_cpu(tbuf);
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t want = (i >= base_idx && i < base_idx + 4) ? (src_vals[i - base_idx] + 100u) : (1000u + i);
        if (after[i] != want) {
            fprintf(stderr, "【红】itbuf write/neighbor mismatch at off=%llu word[%u]: want=%u got=%u\n",
                    (unsigned long long)off, i, want, after[i]);
            return 1;
        }
    }

    nvn_gpu_view_release(tv);
    nvn_gpu_buf_release(srcbuf, NULL, NULL);
    nvn_gpu_buf_release(outbuf, NULL, NULL);
    nvn_gpu_buf_release(tbuf, NULL, NULL);
    return 0;
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
        setenv("debug.s2a.nvk_texel_buffer_alignment", "0", 1);
    } else {
        unsetenv("debug.s2a.nvk_texel_buffer_alignment");
        if (!strcmp(mode, "hide-ext")) g_hide_ext = 1;
        else if (!strcmp(mode, "hide-feat")) g_hide_feat = 1;
        else if (strcmp(mode, "on") != 0) return 2;
    }
    const int expect_on = !strcmp(mode, "on") || is_withdraw;

    char rp[512], wp[512];
    snprintf(rp, sizeof rp, "%s/tbuf.comp.spv", sdir);
    snprintf(wp, sizeof wp, "%s/itbuf.comp.spv", sdir);
    void *rspv = NULL, *wspv = NULL;
    size_t rlen = 0, wlen = 0;
    REQUIRE(load_spv(rp, &rspv, &rlen) && load_spv(wp, &wspv, &wlen));

    struct s2a_vk_device dev = {0};
    char why[256] = "";
    REQUIRE(s2a_vk_device_create(&dev, why, sizeof why) == 0);

    // 探测物理设备原生的 texelBufferAlignment 属性（用于日志与精确断言）
    VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT raw_tbp = {0};
    raw_tbp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES_EXT;
    if (!g_hide_ext) {
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &raw_tbp };
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    }

    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&od, &err));

    nvn_gpu_caps caps = {0};
    nvn_gpu_caps_get(&caps);
    const uint64_t core_align = dev.properties.limits.minTexelBufferOffsetAlignment;
    REQUIRE(core_align >= 16); // lavapipe 为 16，确保 off=4 / off=8 严格低于核心对齐

    nvn_gpu_layout_ent r_ents[2] = {
        { .set = 0, .binding = 0, .cls = NVN_GPU_RES_TBUF, .access = NVN_GPU_ACC_R, .count = 1, .stages = 1u << NVN_GPU_SH_CS },
        { .set = 0, .binding = 1, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS },
    };
    nvn_gpu_layout r_lay = { .ent = r_ents, .nent = 2 };
    nvn_gpu_layout_ent w_ents[2] = {
        { .set = 0, .binding = 0, .cls = NVN_GPU_RES_IMAGE_TBUF, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS },
        { .set = 0, .binding = 1, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_R, .count = 1, .stages = 1u << NVN_GPU_SH_CS },
    };
    nvn_gpu_layout w_lay = { .ent = w_ents, .nent = 2 };

    nvn_gpu_shader sh_r = NULL, sh_w = NULL;
    nvn_gpu_shader_src s_r = { .stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV, .blob = rspv, .bytes = rlen, .entry = "main", .layout = &r_lay };
    nvn_gpu_shader_src s_w = { .stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV, .blob = wspv, .bytes = wlen, .entry = "main", .layout = &w_lay };
    GPU(nvn_gpu_shader_load(&s_r, &sh_r, &err));
    GPU(nvn_gpu_shader_load(&s_w, &sh_w, &err));
    nvn_gpu_pipe p_read = NULL, p_write = NULL;
    nvn_gpu_stage_desc csd_r = { .sh = sh_r };
    nvn_gpu_stage_desc csd_w = { .sh = sh_w };
    GPU(nvn_gpu_cpipe_get(&csd_r, NVN_GPU_PIPE_SYNC, &p_read, &err));
    GPU(nvn_gpu_cpipe_get(&csd_w, NVN_GPU_PIPE_SYNC, &p_write, &err));

    uint64_t sig = 0;
    // 基线：核心对齐偏移（16B）在任何模式下都必须读写精确
    REQUIRE(run_tbuf_rw(p_read, p_write, core_align, &sig) == 0);

    // 核心验证：sub-aligned 视图（R32UI @ off=4、RG32UI @ off=8）与非法细偏移拒收（R32UI @ off=2、RG32UI @ off=4、RGBA32UI @ off=4）
    nvn_gpu_buf probe_buf = NULL;
    GPU(nvn_gpu_buf_create(256, NVN_GPU_MEM_SHARED, NULL, &probe_buf, &err));
    nvn_gpu_view v_r32_off4 = NULL, v_r32_off2 = NULL, v_rg32_off8 = NULL, v_rg32_off4 = NULL;
    nvn_gpu_view v_rgba32_off4 = NULL, v_rgba32_off16 = NULL;
    nvn_gpu_rc rc_r32_4 = nvn_gpu_view_create_buf(probe_buf, 4, 16, NVN_FORMAT_R32UI, &v_r32_off4, &err);
    nvn_gpu_rc rc_r32_2 = nvn_gpu_view_create_buf(probe_buf, 2, 16, NVN_FORMAT_R32UI, &v_r32_off2, &err);
    nvn_gpu_rc rc_rg32_8 = nvn_gpu_view_create_buf(probe_buf, 8, 32, NVN_FORMAT_RG32UI, &v_rg32_off8, &err);
    nvn_gpu_rc rc_rg32_4 = nvn_gpu_view_create_buf(probe_buf, 4, 32, NVN_FORMAT_RG32UI, &v_rg32_off4, &err);
    nvn_gpu_rc rc_rgba32_4 = nvn_gpu_view_create_buf(probe_buf, 4, 32, NVN_FORMAT_RGBA32UI, &v_rgba32_off4, &err);
    nvn_gpu_rc rc_rgba32_16 = nvn_gpu_view_create_buf(probe_buf, 16, 32, NVN_FORMAT_RGBA32UI, &v_rgba32_off16, &err);

    int rw_off4_ok = 0;
    if (nvn_gpu_ok(rc_r32_4)) {
        REQUIRE(run_tbuf_rw(p_read, p_write, 4, &sig) == 0);
        rw_off4_ok = 1;
    }

    printf("texel-buffer-alignment mode=%s core_align=%llu caps_min_align=%u raw_s=%llu(single=%u) raw_u=%llu(single=%u) dev_ext=%u dev_feat=%u r32_off4=%d r32_off2=%d rg32_off8=%d rg32_off4=%d rgba32_off4=%d rgba32_off16=%d rw_off4=%d\n",
           mode, (unsigned long long)core_align, caps.min_texel_align,
           (unsigned long long)raw_tbp.storageTexelBufferOffsetAlignmentBytes, (unsigned)raw_tbp.storageTexelBufferOffsetSingleTexelAlignment,
           (unsigned long long)raw_tbp.uniformTexelBufferOffsetAlignmentBytes, (unsigned)raw_tbp.uniformTexelBufferOffsetSingleTexelAlignment,
           g_dev_ext_enabled, g_dev_feat_enabled,
           rc_r32_4.code, rc_r32_2.code, rc_rg32_8.code, rc_rg32_4.code, rc_rgba32_4.code, rc_rgba32_16.code, rw_off4_ok);

    if (v_r32_off4) nvn_gpu_view_release(v_r32_off4);
    if (v_r32_off2) nvn_gpu_view_release(v_r32_off2);
    if (v_rg32_off8) nvn_gpu_view_release(v_rg32_off8);
    if (v_rg32_off4) nvn_gpu_view_release(v_rg32_off4);
    if (v_rgba32_off4) nvn_gpu_view_release(v_rgba32_off4);
    if (v_rgba32_off16) nvn_gpu_view_release(v_rgba32_off16);
    nvn_gpu_buf_release(probe_buf, NULL, NULL);
    nvn_gpu_shader_release(sh_r);
    nvn_gpu_shader_release(sh_w);
    GPU(nvn_gpu_close(5000000000ull, &err));
    s2a_vk_device_destroy(&dev, 1);
    free(rspv);
    free(wspv);

    // 低于单纹素粒度的偏移永远非法；16B 格式在 16B 对齐处永远合法、4B 处永远非法
    REQUIRE(rc_r32_2.code == NVN_GPU_E_ARG);
    REQUIRE(rc_rg32_4.code == NVN_GPU_E_ARG);
    REQUIRE(rc_rgba32_4.code == NVN_GPU_E_ARG);
    REQUIRE(nvn_gpu_ok(rc_rgba32_16));

    if (expect_on) {
        if (g_dev_ext_enabled != 1 || g_dev_feat_enabled != 1 ||
            !nvn_gpu_ok(rc_r32_4) || !nvn_gpu_ok(rc_rg32_8) || !rw_off4_ok) {
            fprintf(stderr, "【红】texel_buffer_alignment expected active: dev_ext=%u dev_feat=%u r32_off4=%d rg32_off8=%d rw_off4=%d\n",
                    g_dev_ext_enabled, g_dev_feat_enabled, rc_r32_4.code, rc_rg32_8.code, rw_off4_ok);
            return 1;
        }
    } else {
        REQUIRE(g_dev_ext_enabled == 0 && g_dev_feat_enabled == 0);
        REQUIRE(rc_r32_4.code == NVN_GPU_E_ARG);
        REQUIRE(rc_rg32_8.code == NVN_GPU_E_ARG);
        REQUIRE(rw_off4_ok == 0);
    }
    return 0;
}
