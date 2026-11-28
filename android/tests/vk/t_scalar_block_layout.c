// VK_EXT_scalar_block_layout 合同测试（Mesa lavapipe + VVL 1.4.341 + SyncVal）。
// 验证：
//  1) mode=on：Vulkan 1.1 基线下协商 VK_EXT_scalar_block_layout（scalarBlockLayout=VK_TRUE），
//     使用跨 16 字节对齐边界的紧凑 vec3（偏移 12）与 ArrayStride=12 标量布局 SSBO 着色器通过合规检查并精确读出 12 个 float，VVL + SyncVal 0 错。
//  2) mode=off / hide-ext / hide-feat：不请求扩展/特性，标量块布局着色器在 shader_load 阶段被具名拒收
//     （不流入驱动触发 VUID），标准 std430 着色器照常运行，VVL + SyncVal 0 错。
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
        if (!strcmp(tmp[i].extensionName, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME)) continue;
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
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES_EXT)
            ((VkPhysicalDeviceScalarBlockLayoutFeaturesEXT *)b)->scalarBlockLayout = VK_FALSE;
    }
}

VkResult __wrap_vkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *out) {
    if (ci) {
        for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
            if (!strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME))
                g_dev_ext_enabled++;
        for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
            if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES_EXT &&
                ((const VkPhysicalDeviceScalarBlockLayoutFeaturesEXT *)b)->scalarBlockLayout == VK_TRUE)
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

static int run_copy(void *spv, size_t spv_len, uint32_t nfloats, uint64_t fence_val, nvn_gpu_err *err) {
    nvn_gpu_layout_ent ents[2] = {
        { .set = 0, .binding = 0, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_R, .count = 1, .stages = 1u << NVN_GPU_SH_CS },
        { .set = 0, .binding = 1, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS },
    };
    nvn_gpu_layout lay = { .ent = ents, .nent = 2 };
    nvn_gpu_shader sh = NULL;
    nvn_gpu_shader_src ssrc = {
        .stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV,
        .blob = spv, .bytes = spv_len, .entry = "main", .layout = &lay,
    };
    nvn_gpu_rc rc = nvn_gpu_shader_load(&ssrc, &sh, err);
    if (!nvn_gpu_ok(rc)) return 0;

    nvn_gpu_stage_desc csd = { .sh = sh };
    nvn_gpu_pipe cp = NULL;
    rc = nvn_gpu_cpipe_get(&csd, NVN_GPU_PIPE_SYNC, &cp, err);
    if (!nvn_gpu_ok(rc)) { nvn_gpu_shader_release(sh); return 0; }

    const size_t bytes = nfloats * sizeof(float);
    nvn_gpu_buf in_buf = NULL, out_buf = NULL;
    rc = nvn_gpu_buf_create(bytes, NVN_GPU_MEM_SHARED, NULL, &in_buf, err);
    if (!nvn_gpu_ok(rc)) { nvn_gpu_shader_release(sh); return 0; }
    rc = nvn_gpu_buf_create(bytes, NVN_GPU_MEM_SHARED, NULL, &out_buf, err);
    if (!nvn_gpu_ok(rc)) { nvn_gpu_buf_release(in_buf, NULL, NULL); nvn_gpu_shader_release(sh); return 0; }
    float *src = (float *)nvn_gpu_buf_cpu(in_buf);
    for (uint32_t i = 0; i < nfloats; i++) src[i] = (float)(i + 1) * 1.25f;
    memset(nvn_gpu_buf_cpu(out_buf), 0, bytes);

    nvn_gpu_cmd cmd = NULL;
    rc = nvn_gpu_cmd_begin(NULL, &cmd, err);
    if (!nvn_gpu_ok(rc)) { nvn_gpu_buf_release(out_buf, NULL, NULL); nvn_gpu_buf_release(in_buf, NULL, NULL); nvn_gpu_shader_release(sh); return 0; }
    nvn_gpu_set_pipe(cmd, cp);
    nvn_gpu_binding binds[2] = {0};
    binds[0].ent = 0; binds[0].cls = NVN_GPU_RES_SSBO; binds[0].u.b.buf = in_buf; binds[0].u.b.len = bytes;
    binds[1].ent = 1; binds[1].cls = NVN_GPU_RES_SSBO; binds[1].u.b.buf = out_buf; binds[1].u.b.len = bytes;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, binds, 2);
    nvn_gpu_dispatch(cmd, cp, 1, 1, 1, NULL, 0);
    rc = nvn_gpu_cmd_commit(cmd, fence_val, NULL, NULL, err);
    if (!nvn_gpu_ok(rc)) { nvn_gpu_buf_release(out_buf, NULL, NULL); nvn_gpu_buf_release(in_buf, NULL, NULL); nvn_gpu_shader_release(sh); return 0; }
    rc = nvn_gpu_fence_wait(fence_val, 5000000000ull, err);
    if (!nvn_gpu_ok(rc)) { nvn_gpu_buf_release(out_buf, NULL, NULL); nvn_gpu_buf_release(in_buf, NULL, NULL); nvn_gpu_shader_release(sh); return 0; }

    const float *got = (const float *)nvn_gpu_buf_cpu(out_buf);
    int ok = 1;
    for (uint32_t i = 0; i < nfloats; i++) {
        if (fabsf(got[i] - ((float)(i + 1) * 1.25f)) > 1e-6f) { ok = 0; break; }
    }
    nvn_gpu_buf_release(out_buf, NULL, NULL);
    nvn_gpu_buf_release(in_buf, NULL, NULL);
    nvn_gpu_shader_release(sh);
    return ok;
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
        setenv("debug.s2a.nvk_scalar_block_layout", "0", 1);
    } else {
        unsetenv("debug.s2a.nvk_scalar_block_layout");
        if (!strcmp(mode, "hide-ext")) g_hide_ext = 1;
        else if (!strcmp(mode, "hide-feat")) g_hide_feat = 1;
        else if (strcmp(mode, "on") != 0) return 2;
    }
    const int expect_on = !strcmp(mode, "on") || is_withdraw;

    char sp[512], pp[512];
    snprintf(sp, sizeof sp, "%s/scalar_layout.comp.spv", sdir);
    snprintf(pp, sizeof pp, "%s/std430_layout.comp.spv", sdir);
    void *sspv = NULL, *pspv = NULL;
    size_t slen = 0, plen = 0;
    REQUIRE(load_spv(sp, &sspv, &slen));
    REQUIRE(load_spv(pp, &pspv, &plen));

    struct s2a_vk_device dev = {0};
    char why[256] = "";
    REQUIRE(s2a_vk_device_create(&dev, why, sizeof why) == 0);

    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    nvn_gpu_err err = {0};
    GPU(nvn_gpu_open(&od, &err));

    int scalar_ok = 0, guard_reject = 0, std430_ok = 0;
    if (expect_on) {
        scalar_ok = run_copy(sspv, slen, 12, 1, &err);
        std430_ok = run_copy(pspv, plen, 8, 2, &err);
    } else {
        nvn_gpu_layout_ent ents[2] = {
            { .set = 0, .binding = 0, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_R, .count = 1, .stages = 1u << NVN_GPU_SH_CS },
            { .set = 0, .binding = 1, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS },
        };
        nvn_gpu_layout lay = { .ent = ents, .nent = 2 };
        nvn_gpu_shader sh = NULL;
        nvn_gpu_shader_src ssrc = {
            .stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV,
            .blob = sspv, .bytes = slen, .entry = "main", .layout = &lay,
        };
        memset(&err, 0, sizeof err);
        nvn_gpu_rc r = nvn_gpu_shader_load(&ssrc, &sh, &err);
        guard_reject = (!nvn_gpu_ok(r) && r.code == NVN_GPU_E_UNSUPPORTED &&
                        strstr(err.why, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME) != NULL);
        if (sh) nvn_gpu_shader_release(sh);
        memset(&err, 0, sizeof err);
        std430_ok = run_copy(pspv, plen, 8, 1, &err);
    }

    printf("scalar-block-layout mode=%s dev_ext=%u dev_feat=%u scalar_ok=%d guard_reject=%d std430_ok=%d\n",
           mode, g_dev_ext_enabled, g_dev_feat_enabled, scalar_ok, guard_reject, std430_ok);

    GPU(nvn_gpu_close(5000000000ull, &err));
    s2a_vk_device_destroy(&dev, 1);
    free(sspv);
    free(pspv);

    REQUIRE(std430_ok);
    if (expect_on) {
        if (g_dev_ext_enabled != 1 || g_dev_feat_enabled != 1 || !scalar_ok) {
            fprintf(stderr, "【红】scalar_block_layout expected active: dev_ext=%u dev_feat=%u scalar_ok=%d\n",
                    g_dev_ext_enabled, g_dev_feat_enabled, scalar_ok);
            return 1;
        }
    } else {
        REQUIRE(g_dev_ext_enabled == 0 && g_dev_feat_enabled == 0 && guard_reject == 1);
    }
    return 0;
}
