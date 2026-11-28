// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real Vulkan 1.1 + Mesa lavapipe + VVL + SyncVal contract test for Vulkan 1.1 core 16-bit storage features.
#define _POSIX_C_SOURCE 200809L
#include "nvn_backend.h"
#include "vk_device.h"
#include "nvn_plat_vk.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_hide_ssbo;
static int g_hide_ubo;
static int g_hide_push;
static int g_hide_io;
static int g_hw_ssbo;
static int g_hw_ubo;
static int g_hw_push;
static int g_hw_io;
static int g_dev_struct;
static int g_dev_ssbo;
static int g_dev_ubo;
static int g_dev_push;
static int g_dev_io;
static uint64_t g_sig;
static nvn_gpu_err g_err;

#define REQ(x) do { if (!(x)) { fprintf(stderr, "【红】storage-16bit line %d: %s (why=%s)\n", __LINE__, #x, g_err.why); return 1; } } while (0)

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

extern __typeof__(vkGetPhysicalDeviceFeatures2) __real_vkGetPhysicalDeviceFeatures2;
extern __typeof__(vkCreateDevice) __real_vkCreateDevice;

VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *f) {
    __real_vkGetPhysicalDeviceFeatures2(p, f);
    if (!f) return;
    for (VkBaseOutStructure *b = f->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES) {
            VkPhysicalDevice16BitStorageFeatures *sf = (VkPhysicalDevice16BitStorageFeatures *)b;
            g_hw_ssbo = (sf->storageBuffer16BitAccess == VK_TRUE);
            g_hw_ubo  = (sf->uniformAndStorageBuffer16BitAccess == VK_TRUE);
            g_hw_push = (sf->storagePushConstant16 == VK_TRUE);
            g_hw_io   = (sf->storageInputOutput16 == VK_TRUE);
            if (g_hide_ssbo) sf->storageBuffer16BitAccess = VK_FALSE;
            if (g_hide_ubo)  sf->uniformAndStorageBuffer16BitAccess = VK_FALSE;
            if (g_hide_push) sf->storagePushConstant16 = VK_FALSE;
            if (g_hide_io)   sf->storageInputOutput16 = VK_FALSE;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(
    VkPhysicalDevice p, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *d) {
    g_dev_struct = 0; g_dev_ssbo = 0; g_dev_ubo = 0; g_dev_push = 0; g_dev_io = 0;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES) {
            const VkPhysicalDevice16BitStorageFeatures *sf = (const VkPhysicalDevice16BitStorageFeatures *)b;
            g_dev_struct = 1;
            if (sf->storageBuffer16BitAccess == VK_TRUE) g_dev_ssbo = 1;
            if (sf->uniformAndStorageBuffer16BitAccess == VK_TRUE) g_dev_ubo = 1;
            if (sf->storagePushConstant16 == VK_TRUE) g_dev_push = 1;
            if (sf->storageInputOutput16 == VK_TRUE) g_dev_io = 1;
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
    uint32_t *words = load_spv(dir, "s16_std.comp.spv", &bytes);
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
    for (uint32_t i = 0; ok && i < 8; i++) if (got[i] != (i + 1u) * 0x1001u) ok = 0;
    return ok;
}

static int run_ssbo16_case(const char *dir, int expect_ok, const char *expect_why) {
    size_t bytes = 0;
    uint32_t *words = load_spv(dir, "s16_ssbo.comp.spv", &bytes);
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
            fprintf(stderr, "【红】s16_ssbo expected reject(%s) got rc=%d why=%s\n", expect_why ? expect_why : "", r.code, g_err.why);
            if (sh) nvn_gpu_shader_release(sh);
        }
        return ok;
    }
    if (!nvn_gpu_ok(r) || !sh) {
        fprintf(stderr, "【红】s16_ssbo expected accept got rc=%d why=%s\n", r.code, g_err.why);
        return 0;
    }
    nvn_gpu_stage_desc cs = { .sh = sh };
    nvn_gpu_pipe pipe = NULL;
    if (!nvn_gpu_ok(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &pipe, &g_err))) {
        fprintf(stderr, "【红】s16_ssbo cpipe_get failed: %s\n", g_err.why);
        nvn_gpu_shader_release(sh); return 0;
    }
    uint16_t init[8] = { 100, 200, 300, 400, 65500, 65510, 65520, 65535 };
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
    uint16_t got[8] = {0};
    if (ok) memcpy(got, nvn_gpu_buf_cpu(buf), sizeof got);
    nvn_gpu_buf_release(buf, NULL, NULL);
    nvn_gpu_shader_release(sh);
    for (uint32_t i = 0; ok && i < 8; i++) {
        uint16_t want = (uint16_t)((init[i] + i * 5u + 11u) & 0xffffu);
        if (got[i] != want) {
            fprintf(stderr, "【红】s16_ssbo mismatch[%u]: got=%u want=%u\n", i, got[i], want);
            ok = 0;
        }
    }
    return ok;
}

static int run_ubo16_case(const char *dir, int expect_ok, const char *expect_why) {
    size_t bytes = 0;
    uint32_t *words = load_spv(dir, "s16_ubo.comp.spv", &bytes);
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
            fprintf(stderr, "【红】s16_ubo expected reject(%s) got rc=%d why=%s\n", expect_why ? expect_why : "", r.code, g_err.why);
            if (sh) nvn_gpu_shader_release(sh);
        }
        return ok;
    }
    if (!nvn_gpu_ok(r) || !sh) {
        fprintf(stderr, "【红】s16_ubo expected accept got rc=%d why=%s\n", r.code, g_err.why);
        return 0;
    }
    nvn_gpu_stage_desc cs = { .sh = sh };
    nvn_gpu_pipe pipe = NULL;
    if (!nvn_gpu_ok(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &pipe, &g_err))) {
        fprintf(stderr, "【红】s16_ubo cpipe_get failed: %s\n", g_err.why);
        nvn_gpu_shader_release(sh); return 0;
    }
    uint16_t ubo_init[8] = { 0x1101, 0x2202, 0x3303, 0x4404, 0x8808, 0x9909, 0xaa0a, 0xf00f };
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
        uint32_t want = (uint32_t)ubo_init[i] ^ 0x5a5au;
        if (got[i] != want) {
            fprintf(stderr, "【红】s16_ubo mismatch[%u]: got=%#x want=%#x\n", i, got[i], want);
            ok = 0;
        }
    }
    return ok;
}

static int run_push16_case(const char *dir, int expect_ok, const char *expect_why) {
    size_t bytes = 0;
    uint32_t *words = load_spv(dir, "s16_push.comp.spv", &bytes);
    if (!words) return 0;
    nvn_gpu_layout_ent ent = { .cls = NVN_GPU_RES_SSBO, .count = 1, .stages = 1u << NVN_GPU_SH_CS, .access = NVN_GPU_ACC_RW };
    nvn_gpu_layout layout = { .ent = &ent, .nent = 1, .push_off = 0, .push_bytes = 16 };
    nvn_gpu_shader_src src = { .stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV, .blob = words, .bytes = bytes, .entry = "main", .layout = &layout };
    nvn_gpu_shader sh = NULL;
    memset(&g_err, 0, sizeof g_err);
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh, &g_err);
    free(words);
    if (!expect_ok) {
        int ok = (r.code == NVN_GPU_E_UNSUPPORTED && sh == NULL && (!expect_why || strstr(g_err.why, expect_why) != NULL));
        if (!ok) {
            fprintf(stderr, "【红】s16_push expected reject(%s) got rc=%d why=%s\n", expect_why ? expect_why : "", r.code, g_err.why);
            if (sh) nvn_gpu_shader_release(sh);
        }
        return ok;
    }
    if (!nvn_gpu_ok(r) || !sh) {
        fprintf(stderr, "【红】s16_push expected accept got rc=%d why=%s\n", r.code, g_err.why);
        return 0;
    }
    nvn_gpu_stage_desc cs = { .sh = sh };
    nvn_gpu_pipe pipe = NULL;
    if (!nvn_gpu_ok(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &pipe, &g_err))) {
        fprintf(stderr, "【红】s16_push cpipe_get failed: %s\n", g_err.why);
        nvn_gpu_shader_release(sh); return 0;
    }
    uint16_t pc[8] = { 5, 17, 42, 99, 1280, 2000, 40000, 65000 };
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
        uint32_t want = (uint32_t)pc[i] + 0x10000u;
        if (got[i] != want) {
            fprintf(stderr, "【红】s16_push mismatch[%u]: got=%#x want=%#x\n", i, got[i], want);
            ok = 0;
        }
    }
    return ok;
}

static int run_io16_case(const char *dir, int expect_ok, const char *expect_why) {
    size_t vbytes = 0, fbytes = 0;
    uint32_t *vwords = load_spv(dir, "s16_io.vert.spv", &vbytes);
    uint32_t *fwords = load_spv(dir, "s16_io.frag.spv", &fbytes);
    if (!vwords || !fwords) { free(vwords); free(fwords); return 0; }
    static const nvn_gpu_layout empty = {0};
    nvn_gpu_shader_src vs_src = { .stage = NVN_GPU_SH_VS, .kind = NVN_GPU_BLOB_SPIRV, .blob = vwords, .bytes = vbytes, .entry = "main", .layout = &empty };
    nvn_gpu_shader_src fs_src = { .stage = NVN_GPU_SH_FS, .kind = NVN_GPU_BLOB_SPIRV, .blob = fwords, .bytes = fbytes, .entry = "main", .layout = &empty };
    nvn_gpu_shader vs = NULL, fs = NULL;
    memset(&g_err, 0, sizeof g_err);
    nvn_gpu_rc rv = nvn_gpu_shader_load(&vs_src, &vs, &g_err);
    nvn_gpu_err ev = g_err;
    memset(&g_err, 0, sizeof g_err);
    nvn_gpu_rc rf = nvn_gpu_shader_load(&fs_src, &fs, &g_err);
    free(vwords);
    free(fwords);
    if (!expect_ok) {
        int ok = (rv.code == NVN_GPU_E_UNSUPPORTED && vs == NULL && (!expect_why || strstr(ev.why, expect_why) != NULL)) &&
                 (rf.code == NVN_GPU_E_UNSUPPORTED && fs == NULL && (!expect_why || strstr(g_err.why, expect_why) != NULL));
        if (!ok) {
            fprintf(stderr, "【红】s16_io expected reject(%s) got rv=%d(%s) rf=%d(%s)\n",
                    expect_why ? expect_why : "", rv.code, ev.why, rf.code, g_err.why);
            if (vs) nvn_gpu_shader_release(vs);
            if (fs) nvn_gpu_shader_release(fs);
        }
        return ok;
    }
    if (!nvn_gpu_ok(rv) || !vs || !nvn_gpu_ok(rf) || !fs) {
        fprintf(stderr, "【红】s16_io expected accept got rv=%d(%s) rf=%d(%s)\n", rv.code, ev.why, rf.code, g_err.why);
        if (vs) nvn_gpu_shader_release(vs);
        if (fs) nvn_gpu_shader_release(fs);
        return 0;
    }
    enum { W = 4, H = 4, BYTES = W * H * 4 };
    nvn_gpu_pipe_desc pd = {0};
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.prim = NVN_DRAW_PRIMITIVE_TRIANGLES;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.samples = 1;
    pd.origin_ll = 1;
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3;
    pd.chmask.mask = 0xFu;
    nvn_gpu_pipe pipe = NULL;
    int ok = nvn_gpu_ok(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &g_err));
    nvn_gpu_tex tex = NULL;
    nvn_gpu_view view = NULL;
    nvn_gpu_buf rb = NULL;
    if (ok) {
        nvn_gpu_tex_desc td = {0};
        td.d.width = W; td.d.height = H; td.d.depth = 1; td.d.levels = 1;
        td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8;
        td.usage = NVN_GPU_TEX_RENDER;
        ok = nvn_gpu_ok(nvn_gpu_tex_create(&td, &tex, &g_err)) &&
             nvn_gpu_ok(nvn_gpu_view_create(tex, NULL, &view, &g_err)) &&
             nvn_gpu_ok(nvn_gpu_buf_create(BYTES, NVN_GPU_MEM_SHARED, NULL, &rb, &g_err));
    }
    if (ok) {
        nvn_gpu_cmd cmd = NULL;
        ok = nvn_gpu_ok(nvn_gpu_cmd_begin(NULL, &cmd, &g_err));
        if (ok) {
            nvn_gpu_pass_desc pass = { .ncolor = 1, .color = {{ .view = view, .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE }}, .w = W, .h = H, .layers = 1 };
            nvn_gpu_pass_begin(cmd, &pass);
            nvn_gpu_set_pipe(cmd, pipe);
            nvn_gpu_dyn dyn = { .nvp = 1, .vp = {{0, 0, (float)W, (float)H, 0.0f, 1.0f}}, .has_sc = 1, .sc = {0, 0, W, H}, .origin_ll = 1 };
            nvn_gpu_set_dyn(cmd, &dyn);
            nvn_gpu_draw_args draw = { .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1 };
            nvn_gpu_draw(cmd, &draw);
            nvn_gpu_pass_end(cmd);
            NVNcopyRegion reg = {0, 0, 0, W, H, 1};
            nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, &reg, rb, 0, W * 4, BYTES);
            ok = nvn_gpu_ok(nvn_gpu_cmd_commit(cmd, ++g_sig, done_cb, NULL, &g_err)) &&
                 nvn_gpu_ok(nvn_gpu_fence_wait(g_sig, 5000000000ull, &g_err));
        }
    }
    if (ok) {
        const uint8_t *px = nvn_gpu_buf_cpu(rb);
        for (unsigned i = 0; i < W * H; i++) {
            const uint8_t *p = px + i * 4;
            if (p[0] != 100 || p[1] != 200 || p[2] != 150 || p[3] != 255) {
                fprintf(stderr, "【红】s16_io pixel[%u] got=%u,%u,%u,%u want=100,200,150,255\n", i, p[0], p[1], p[2], p[3]);
                ok = 0; break;
            }
        }
    }
    if (rb) nvn_gpu_buf_release(rb, NULL, NULL);
    if (view) nvn_gpu_view_release(view);
    if (tex) nvn_gpu_tex_release(tex);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <shader_dir> <on|off|hide-ssbo|hide-ubo|hide-push|hide-io|withdraw>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1], *mode = argv[2];
    if (!strcmp(mode, "off") || !strcmp(mode, "withdraw")) setenv("debug.s2a.nvk_16bit_storage", "0", 1);
    else setenv("debug.s2a.nvk_16bit_storage", "1", 1);
    g_hide_ssbo = !strcmp(mode, "hide-ssbo");
    g_hide_ubo  = !strcmp(mode, "hide-ubo");
    g_hide_push = !strcmp(mode, "hide-push");
    g_hide_io   = !strcmp(mode, "hide-io");

    struct s2a_vk_device dev;
    char why[256] = "";
    REQ(!s2a_vk_device_create(&dev, why, sizeof why));
    if (!strcmp(mode, "on") || !strcmp(mode, "withdraw")) {
        REQ(g_hw_ssbo || g_hw_ubo || g_hw_push || g_hw_io);
    }
    const int active = !strcmp(mode, "on") || !strcmp(mode, "withdraw") || g_hide_ssbo || g_hide_ubo || g_hide_push || g_hide_io;
    const int expect_ssbo   = (active && !g_hide_ssbo) ? g_hw_ssbo : 0;
    const int expect_ubo    = (active && !g_hide_ubo)  ? g_hw_ubo  : 0;
    const int expect_push   = (active && !g_hide_push) ? g_hw_push : 0;
    const int expect_io     = (active && !g_hide_io)   ? g_hw_io   : 0;
    const int expect_struct = (expect_ssbo || expect_ubo || expect_push || expect_io);

    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    od.app_label = "storage-16bit-test";
    REQ(nvn_gpu_ok(nvn_gpu_open(&od, &g_err)));

    int std_ok  = run_std_case(dir);
    int ssbo_ok = run_ssbo16_case(dir, expect_ssbo, expect_ssbo ? NULL : "storageBuffer16BitAccess");
    int ubo_ok  = run_ubo16_case(dir, expect_ubo, expect_ubo ? NULL : "uniformAndStorageBuffer16BitAccess");
    int push_ok = run_push16_case(dir, expect_push, expect_push ? NULL : "storagePushConstant16");
    int io_ok   = run_io16_case(dir, expect_io, expect_io ? NULL : "storageInputOutput16");
    printf("storage-16bit mode=%s hw=%d/%d/%d/%d dev_s=%d dev_bits=%d/%d/%d/%d en_bits=%d/%d/%d/%d std=%d ssbo=%d ubo=%d push=%d io=%d\n",
           mode, g_hw_ssbo, g_hw_ubo, g_hw_push, g_hw_io, g_dev_struct, g_dev_ssbo, g_dev_ubo, g_dev_push, g_dev_io,
           dev.storage_16bit_ssbo_enabled, dev.storage_16bit_ubo_ssbo_enabled, dev.storage_16bit_push_enabled, dev.storage_16bit_io_enabled,
           std_ok, ssbo_ok, ubo_ok, push_ok, io_ok);

    int rc = 0;
    if (g_dev_struct != expect_struct || g_dev_ssbo != expect_ssbo || g_dev_ubo != expect_ubo ||
        g_dev_push != expect_push || g_dev_io != expect_io ||
        dev.storage_16bit_ssbo_enabled != expect_ssbo || dev.storage_16bit_ubo_ssbo_enabled != expect_ubo ||
        dev.storage_16bit_push_enabled != expect_push || dev.storage_16bit_io_enabled != expect_io ||
        !std_ok || !ssbo_ok || !ubo_ok || !push_ok || !io_ok) {
        rc = 1;
    }

    REQ(nvn_gpu_ok(nvn_gpu_close(5000000000ull, &g_err)));
    REQ(!s2a_vk_device_destroy(&dev, 1));
    return rc;
}
