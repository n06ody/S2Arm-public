// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define main feedback_fixture_main
#include "t_feedback.c"
#undef main
#include "nvn_unimpl.h"

enum { EXT_ONLY, KHR_ONLY, NO_EXTENSIONS, NO_FEATURE, MAX_2, KHR_ZERO_FIRST };
static int control;
static unsigned enabled_ext, enabled_khr, feature_enabled, zero_divisor;
static unsigned khr_props_seen, khr_nonzero_first;
static const uint32_t height = 8, width = 64, bytes = 64 * 8 * 4;
extern uint64_t nvn_vk_dbg_dq_recs(void);
extern uint64_t nvn_vk_dbg_pipe_stat(int which);

extern __typeof__(vkEnumerateDeviceExtensionProperties) __real_vkEnumerateDeviceExtensionProperties;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice p,
        const char *layer, uint32_t *count, VkExtensionProperties *props) {
    VkResult rc = __real_vkEnumerateDeviceExtensionProperties(p, layer, count, props);
    if (layer || !props || rc != VK_SUCCESS) return rc;
    const int khr = control == KHR_ONLY || control == KHR_ZERO_FIRST;
    uint32_t n = 0;
    for (uint32_t i = 0; i < *count; i++) {
        const char *name = props[i].extensionName;
        if (!strcmp(name, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) && (khr || control == NO_EXTENSIONS)) continue;
        if (!strcmp(name, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) && !khr) continue;
        props[n++] = props[i];
    }
    *count = n;
    return rc;
}

extern __typeof__(vkGetPhysicalDeviceFeatures2) __real_vkGetPhysicalDeviceFeatures2;
VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *f) {
    __real_vkGetPhysicalDeviceFeatures2(p, f);
    if (control != NO_FEATURE) return;
    for (VkBaseOutStructure *s = f->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT)
            ((VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *)s)->vertexAttributeInstanceRateDivisor = VK_FALSE;
}

extern __typeof__(vkGetPhysicalDeviceProperties2) __real_vkGetPhysicalDeviceProperties2;
VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceProperties2(VkPhysicalDevice p, VkPhysicalDeviceProperties2 *f) {
    __real_vkGetPhysicalDeviceProperties2(p, f);
    for (VkBaseOutStructure *s = f->pNext; s; s = s->pNext) {
        if (control == MAX_2 && s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT) {
            VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *v = (VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *)s;
            if (v->maxVertexAttribDivisor > 2) v->maxVertexAttribDivisor = 2;
        }
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR) {
            VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR *v = (VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR *)s;
            if (control == KHR_ZERO_FIRST) v->supportsNonZeroFirstInstance = VK_FALSE;
            khr_props_seen++;
            khr_nonzero_first = v->supportsNonZeroFirstInstance;
        }
    }
}

extern __typeof__(vkCreateDevice) __real_vkCreateDevice;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(VkPhysicalDevice p, const VkDeviceCreateInfo *ci,
        const VkAllocationCallbacks *a, VkDevice *d) {
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        enabled_ext |= !strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
        enabled_khr |= !strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    }
    for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT) {
            const VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *f = (const VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *)s;
            feature_enabled = f->vertexAttributeInstanceRateDivisor;
            zero_divisor = f->vertexAttributeInstanceRateZeroDivisor;
        }
    return __real_vkCreateDevice(p, ci, a, d);
}

static int allowed(unsigned divisor) {
    return divisor == 1 || (control != NO_EXTENSIONS && control != NO_FEATURE && (control != MAX_2 || divisor <= 2));
}

int main(int argc, char **argv) {
    REQUIRE(argc == 4);
    control = atoi(argv[2]);
    dq_mode = atoi(argv[3]);
    observed_expect_samples = 0;
    struct s2a_vk_device dev;
    char why[256] = {0};
    REQUIRE(!s2a_vk_device_create(&dev, why, sizeof why));
    nvn_gpu_open_desc od = {.host_device = &dev, .app_label = "vertex-attribute-divisor"};
    GPU(nvn_gpu_open(&od, &err));
    nvn_gpu_shader vs = shader(argv[1], "divisor_stripes.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = shader(argv[1], "quad.frag", NVN_GPU_SH_FS, NULL);
    nvn_gpu_pipe_desc pd = {.prim = 5, .samples = 1};
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = NVN_LOGIC_OP_COPY;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.chmask.mask = 15;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F;
    pd.attr[0].stream = 1;
    pd.stream[1].stride = 16;

    nvn_gpu_pipe pipes[4] = {0};
    const uint64_t pso_before = nvn_vk_dbg_pipe_stat(5);
    unsigned pipe_bad = 0, rejected = 0;
    for (unsigned i = 1; i <= 3; i++) {
        pd.stream[1].divisor = i;
        nvn_gpu_rc rc = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipes[i], &err);
        if (allowed(i)) {
            if (!nvn_gpu_ok(rc) || !pipes[i]) {
                printf("divisor=%u pipeline unexpectedly rejected rc=%d why=%s\n", i, rc.code, err.why);
                pipe_bad++;
            } else {
                nvn_gpu_pipe found = NULL;
                GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &found, &err));
                pipe_bad += found != pipes[i] || (i > 1 && found == pipes[i - 1]);
            }
        } else {
            char message[32];
            snprintf(message, sizeof message, "实例除数 %u", i);
            const int named = rc.code == NVN_GPU_E_UNSUPPORTED && !pipes[i] && strstr(err.why, message) &&
                (control != MAX_2 || (strstr(err.why, "超上限") && strstr(err.why, "上限 2")));
            if (named) rejected++;
            else pipe_bad++;
        }
    }
    const uint64_t psos = nvn_vk_dbg_pipe_stat(5) - pso_before;
    REQUIRE(psos == (dev.vertex_input_dynamic_enabled ? 1u : 3u - rejected));
    printf("vertex-divisor VIDS=%d canonical-psos=%llu\n", dev.vertex_input_dynamic_enabled, (unsigned long long)psos);

    const float palette[10][4] = {{1,0,0,1},{0,1,0,1},{0,0,1,1},{1,0,0,1},{0,1,0,1},
                                  {0,0,1,1},{1,0,0,1},{0,1,0,1},{0,0,1,1},{1,0,0,1}};
    nvn_gpu_tex_desc td = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
                                .width = width, .height = height, .depth = 1, .levels = 1},
                           .usage = NVN_GPU_TEX_RENDER};
    nvn_gpu_tex t = NULL;
    nvn_gpu_view v = NULL;
    nvn_gpu_buf colors = NULL, rb = NULL;
    GPU(nvn_gpu_tex_create(&td, &t, &err));
    GPU(nvn_gpu_view_create(t, NULL, &v, &err));
    GPU(nvn_gpu_buf_create(sizeof palette, NVN_GPU_MEM_SHARED, palette, &colors, &err));
    GPU(nvn_gpu_buf_create(6 * bytes, NVN_GPU_MEM_SHARED, NULL, &rb, &err));

    nvn_gpu_cmd cmd = NULL;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_pass_desc p = {.w = width, .h = height, .layers = 1, .ncolor = 1};
    p.color[0].view = v;
    p.color[0].load = NVN_GPU_LOAD_CLEAR;
    p.color[0].clear.f[3] = 1;
    nvn_gpu_dyn dyn = {.nvp = 1, .has_sc = 1};
    dyn.vp[0][2] = (float)width;
    dyn.vp[0][3] = (float)height;
    dyn.vp[0][5] = 1;
    dyn.sc[2] = (int32_t)width;
    dyn.sc[3] = (int32_t)height;
    for (unsigned i = 0; i < 6; i++) {
        const unsigned divisor = 1 + i / 2, base = i & 1;
        if (!pipes[divisor]) continue;
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_set_pipe(cmd, pipes[divisor]);
        nvn_gpu_set_dyn(cmd, &dyn);
        nvn_gpu_set_vbuf(cmd, 1, colors, 0);
        const nvn_gpu_draw_args a = {.prim = 5, .count = 4, .instances = 8, .base_instance = base};
        nvn_gpu_draw(cmd, &a);
        nvn_gpu_pass_end(cmd);
        nvn_gpu_copy_tex_buf(cmd, t, 0, 0, NULL, rb, i * bytes, width * 4, bytes);
    }
    nvn_gpu_buf indirect = NULL;
    if (control == KHR_ZERO_FIRST && pipes[2]) {
        const VkDrawIndirectCommand args = {.vertexCount = 4, .instanceCount = 8, .firstInstance = 1};
        GPU(nvn_gpu_buf_create(sizeof args, NVN_GPU_MEM_SHARED, &args, &indirect, &err));
        nvn_gpu_pass_begin(cmd, &p);
        nvn_gpu_set_pipe(cmd, pipes[2]);
        nvn_gpu_set_dyn(cmd, &dyn);
        nvn_gpu_set_vbuf(cmd, 1, colors, 0);
        const nvn_gpu_draw_args a = {.prim = 5, .ind = indirect};
        nvn_gpu_draw(cmd, &a);
        nvn_gpu_pass_end(cmd);
    }
    GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    const uint8_t *pixels = nvn_gpu_buf_cpu(rb);
    REQUIRE(pixels);

    const int want_khr = control == KHR_ONLY || control == KHR_ZERO_FIRST;
    const int restricted = want_khr && !khr_nonzero_first;
    unsigned bad = 0, checked = 0, want_draws = 0;
    for (unsigned i = 0; i < 6; i++) {
        const unsigned divisor = 1 + i / 2, base = i & 1;
        if (!allowed(divisor)) continue;
        checked += width * height;
        if (!pipes[divisor]) { bad += width * height; continue; }
        const int blocked = restricted && base && divisor > 1;
        want_draws += !blocked;
        for (unsigned j = 0; j < width * height; j++) {
            const unsigned column = j % width / 8;
            const unsigned attribute = base + (((column + 8 - base) % 8) / divisor);
            const unsigned channel = attribute % 3;
            const uint8_t *pixel = pixels + i * bytes + 4 * j;
            bad += pixel[0] != (!blocked && channel == 0 ? 255 : 0) ||
                   pixel[1] != (!blocked && channel == 1 ? 255 : 0) ||
                   pixel[2] != (!blocked && channel == 2 ? 255 : 0) || pixel[3] != 255;
        }
    }
    const int want_ext = control == EXT_ONLY || control == MAX_2;
    const unsigned skips = (unsigned)nvn_unimpl_count("draw", "vertex_divisor_nonzero_first_instance");
    const unsigned indirect_skips = (unsigned)nvn_unimpl_count("draw", "vertex_divisor_indirect_first_instance_unknown");
    const uint64_t dq_recs = nvn_vk_dbg_dq_recs();
    const int ok = !pipe_bad && !bad && observed_draws == want_draws && enabled_ext == (unsigned)want_ext &&
        enabled_khr == (unsigned)want_khr && feature_enabled == (unsigned)(want_khr || want_ext) &&
        !zero_divisor && khr_props_seen == (unsigned)want_khr && (control != KHR_ZERO_FIRST || !khr_nonzero_first) &&
        skips == (restricted ? 2u : 0u) &&
        indirect_skips == (unsigned)(control == KHR_ZERO_FIRST) && !!dq_recs == !!dq_mode;
    printf("vertex-divisor control=%d DQ=%d dq-recs=%llu pixels=%u/%u draws=%u/%u pipe-errors=%u rejected=%u skips=%u indirect=%u ext=%u khr=%u khr-nonzero-first=%u %s\n",
           control, dq_mode, (unsigned long long)dq_recs, checked - bad, checked, observed_draws, want_draws, pipe_bad, rejected, skips, indirect_skips,
           enabled_ext, enabled_khr, khr_nonzero_first, ok ? "PASS" : "FAIL");

    nvn_gpu_buf_release(rb, NULL, NULL);
    if (indirect) nvn_gpu_buf_release(indirect, NULL, NULL);
    nvn_gpu_buf_release(colors, NULL, NULL);
    nvn_gpu_view_release(v);
    nvn_gpu_tex_release(t);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
    GPU(nvn_gpu_close(5000000000ull, &err));
    REQUIRE(!s2a_vk_device_destroy(&dev, 1));
    return ok ? 0 : 1;
}
