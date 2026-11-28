// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real Vulkan 1.1 + Mesa lavapipe + VVL + SyncVal contract test for Vulkan 1.1 core VariablePointers features.
#define _POSIX_C_SOURCE 200809L
#include "nvn_backend.h"
#include "vk_device.h"
#include "nvn_plat_vk.h"
#include "spirv.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_hide_vp;
static int g_hide_vpsb;
static int g_hw_vpsb;
static int g_hw_vp;
static int g_dev_struct;
static int g_dev_vpsb;
static int g_dev_vp;
static uint64_t g_sig;
static nvn_gpu_err g_err;

#define REQ(x) do { if (!(x)) { fprintf(stderr, "【红】variable-pointers line %d: %s (why=%s)\n", __LINE__, #x, g_err.why); return 1; } } while (0)

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
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES) {
            VkPhysicalDeviceVariablePointersFeatures *vp = (VkPhysicalDeviceVariablePointersFeatures *)b;
            g_hw_vpsb = (vp->variablePointersStorageBuffer == VK_TRUE);
            g_hw_vp   = (vp->variablePointers == VK_TRUE);
            if (g_hide_vp)   vp->variablePointers = VK_FALSE;
            // hide-vpsb intentionally clears variablePointersStorageBuffer while leaving variablePointers
            // untouched to prove vk_device obeys VUID-VkPhysicalDeviceVariablePointersFeatures-variablePointers-01431.
            if (g_hide_vpsb) vp->variablePointersStorageBuffer = VK_FALSE;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(
    VkPhysicalDevice p, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *d) {
    g_dev_struct = 0; g_dev_vpsb = 0; g_dev_vp = 0;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES) {
            const VkPhysicalDeviceVariablePointersFeatures *vp = (const VkPhysicalDeviceVariablePointersFeatures *)b;
            g_dev_struct = 1;
            if (vp->variablePointersStorageBuffer == VK_TRUE) g_dev_vpsb = 1;
            if (vp->variablePointers == VK_TRUE) g_dev_vp = 1;
        }
    }
    return __real_vkCreateDevice(p, ci, a, d);
}

#define W(op, n) ((((uint32_t)(n)) << 16) | ((uint32_t)(op)))

// Assemble a valid SPIR-V 1.3 GLCompute shader exercising OpCapability VariablePointersStorageBuffer
// (OpSelect between two StorageBuffer pointers into the SAME buffer variable: even lanes read BufA[i], odd lanes read BufA[7 - i]).
static size_t build_vpsb_spv(uint32_t *out) {
    uint32_t *p = out;
    // Header: magic, SPIR-V 1.3, generator=0, bound=40, schema=0
    *p++ = SpvMagicNumber; *p++ = 0x00010300u; *p++ = 0; *p++ = 40; *p++ = 0;
    // Capabilities
    *p++ = W(SpvOpCapability, 2); *p++ = SpvCapabilityShader;
    *p++ = W(SpvOpCapability, 2); *p++ = SpvCapabilityVariablePointersStorageBuffer;
    // MemoryModel
    *p++ = W(SpvOpMemoryModel, 3); *p++ = SpvAddressingModelLogical; *p++ = SpvMemoryModelGLSL450;
    // EntryPoint GLCompute %main(2) "main" %gl_gid(10)
    *p++ = W(SpvOpEntryPoint, 6); *p++ = SpvExecutionModelGLCompute; *p++ = 2;
    *p++ = 0x6e69616du; *p++ = 0x00000000u; *p++ = 10;
    // ExecutionMode %main LocalSize 8 1 1
    *p++ = W(SpvOpExecutionMode, 6); *p++ = 2; *p++ = SpvExecutionModeLocalSize; *p++ = 8; *p++ = 1; *p++ = 1;
    // Decorations
    *p++ = W(SpvOpDecorate, 4); *p++ = 10; *p++ = SpvDecorationBuiltIn; *p++ = SpvBuiltInGlobalInvocationId;
    *p++ = W(SpvOpDecorate, 4); *p++ = 14; *p++ = SpvDecorationArrayStride; *p++ = 4;
    *p++ = W(SpvOpMemberDecorate, 5); *p++ = 15; *p++ = 0; *p++ = SpvDecorationOffset; *p++ = 0;
    *p++ = W(SpvOpDecorate, 3); *p++ = 15; *p++ = SpvDecorationBlock;
    *p++ = W(SpvOpDecorate, 4); *p++ = 18; *p++ = SpvDecorationDescriptorSet; *p++ = 0;
    *p++ = W(SpvOpDecorate, 4); *p++ = 18; *p++ = SpvDecorationBinding; *p++ = 0;
    *p++ = W(SpvOpDecorate, 4); *p++ = 19; *p++ = SpvDecorationDescriptorSet; *p++ = 0;
    *p++ = W(SpvOpDecorate, 4); *p++ = 19; *p++ = SpvDecorationBinding; *p++ = 1;
    // Types & Constants
    *p++ = W(SpvOpTypeVoid, 2); *p++ = 1;
    *p++ = W(SpvOpTypeFunction, 3); *p++ = 3; *p++ = 1;
    *p++ = W(SpvOpTypeInt, 4); *p++ = 4; *p++ = 32; *p++ = 0;
    *p++ = W(SpvOpTypeBool, 2); *p++ = 5;
    *p++ = W(SpvOpTypeVector, 4); *p++ = 6; *p++ = 4; *p++ = 3;
    *p++ = W(SpvOpTypePointer, 4); *p++ = 7; *p++ = SpvStorageClassInput; *p++ = 6;
    *p++ = W(SpvOpTypePointer, 4); *p++ = 8; *p++ = SpvStorageClassInput; *p++ = 4;
    *p++ = W(SpvOpConstant, 4); *p++ = 4; *p++ = 9; *p++ = 0;
    *p++ = W(SpvOpVariable, 4); *p++ = 7; *p++ = 10; *p++ = SpvStorageClassInput;
    *p++ = W(SpvOpConstant, 4); *p++ = 4; *p++ = 11; *p++ = 8;
    *p++ = W(SpvOpConstant, 4); *p++ = 4; *p++ = 12; *p++ = 1;
    *p++ = W(SpvOpConstant, 4); *p++ = 4; *p++ = 13; *p++ = 7;
    *p++ = W(SpvOpTypeArray, 4); *p++ = 14; *p++ = 4; *p++ = 11;
    *p++ = W(SpvOpTypeStruct, 3); *p++ = 15; *p++ = 14;
    *p++ = W(SpvOpTypePointer, 4); *p++ = 16; *p++ = SpvStorageClassStorageBuffer; *p++ = 15;
    *p++ = W(SpvOpTypePointer, 4); *p++ = 17; *p++ = SpvStorageClassStorageBuffer; *p++ = 4;
    *p++ = W(SpvOpVariable, 4); *p++ = 16; *p++ = 18; *p++ = SpvStorageClassStorageBuffer;
    *p++ = W(SpvOpVariable, 4); *p++ = 16; *p++ = 19; *p++ = SpvStorageClassStorageBuffer;
    // Function %main
    *p++ = W(SpvOpFunction, 5); *p++ = 1; *p++ = 2; *p++ = SpvFunctionControlMaskNone; *p++ = 3;
    *p++ = W(SpvOpLabel, 2); *p++ = 21;
    *p++ = W(SpvOpAccessChain, 5); *p++ = 8; *p++ = 22; *p++ = 10; *p++ = 9;
    *p++ = W(SpvOpLoad, 4); *p++ = 4; *p++ = 23; *p++ = 22;
    *p++ = W(SpvOpISub, 5); *p++ = 4; *p++ = 24; *p++ = 13; *p++ = 23;
    *p++ = W(SpvOpBitwiseAnd, 5); *p++ = 4; *p++ = 25; *p++ = 23; *p++ = 12;
    *p++ = W(SpvOpIEqual, 5); *p++ = 5; *p++ = 26; *p++ = 25; *p++ = 9;
    *p++ = W(SpvOpAccessChain, 6); *p++ = 17; *p++ = 27; *p++ = 18; *p++ = 9; *p++ = 23;
    *p++ = W(SpvOpAccessChain, 6); *p++ = 17; *p++ = 28; *p++ = 18; *p++ = 9; *p++ = 24;
    *p++ = W(SpvOpSelect, 6); *p++ = 17; *p++ = 29; *p++ = 26; *p++ = 27; *p++ = 28;
    *p++ = W(SpvOpLoad, 4); *p++ = 4; *p++ = 30; *p++ = 29;
    *p++ = W(SpvOpAccessChain, 6); *p++ = 17; *p++ = 31; *p++ = 19; *p++ = 9; *p++ = 23;
    *p++ = W(SpvOpStore, 3); *p++ = 31; *p++ = 30;
    *p++ = W(SpvOpReturn, 1);
    *p++ = W(SpvOpFunctionEnd, 1);
    return (size_t)(p - out) * 4u;
}

// Assemble a valid SPIR-V 1.3 GLCompute shader exercising OpCapability VariablePointers
// (OpSelect on Workgroup pointers: stores buf_in[i] ^ 0x12340000 into wg[i], barrier,
// selects &wg[i] for even lanes and &wg[7 - i] for odd lanes, writes to buf_out[i]).
static size_t build_vp_full_spv(uint32_t *out) {
    uint32_t *p = out;
    *p++ = SpvMagicNumber; *p++ = 0x00010300u; *p++ = 0; *p++ = 48; *p++ = 0;
    // Capabilities
    *p++ = W(SpvOpCapability, 2); *p++ = SpvCapabilityShader;
    *p++ = W(SpvOpCapability, 2); *p++ = SpvCapabilityVariablePointers;
    // MemoryModel
    *p++ = W(SpvOpMemoryModel, 3); *p++ = SpvAddressingModelLogical; *p++ = SpvMemoryModelGLSL450;
    // EntryPoint
    *p++ = W(SpvOpEntryPoint, 6); *p++ = SpvExecutionModelGLCompute; *p++ = 2;
    *p++ = 0x6e69616du; *p++ = 0x00000000u; *p++ = 10;
    // ExecutionMode
    *p++ = W(SpvOpExecutionMode, 6); *p++ = 2; *p++ = SpvExecutionModeLocalSize; *p++ = 8; *p++ = 1; *p++ = 1;
    // Decorations
    *p++ = W(SpvOpDecorate, 4); *p++ = 10; *p++ = SpvDecorationBuiltIn; *p++ = SpvBuiltInGlobalInvocationId;
    *p++ = W(SpvOpDecorate, 4); *p++ = 14; *p++ = SpvDecorationArrayStride; *p++ = 4;
    *p++ = W(SpvOpMemberDecorate, 5); *p++ = 15; *p++ = 0; *p++ = SpvDecorationOffset; *p++ = 0;
    *p++ = W(SpvOpDecorate, 3); *p++ = 15; *p++ = SpvDecorationBlock;
    *p++ = W(SpvOpDecorate, 4); *p++ = 18; *p++ = SpvDecorationDescriptorSet; *p++ = 0;
    *p++ = W(SpvOpDecorate, 4); *p++ = 18; *p++ = SpvDecorationBinding; *p++ = 0;
    *p++ = W(SpvOpDecorate, 4); *p++ = 19; *p++ = SpvDecorationDescriptorSet; *p++ = 0;
    *p++ = W(SpvOpDecorate, 4); *p++ = 19; *p++ = SpvDecorationBinding; *p++ = 1;
    // Types & Constants
    *p++ = W(SpvOpTypeVoid, 2); *p++ = 1;
    *p++ = W(SpvOpTypeFunction, 3); *p++ = 3; *p++ = 1;
    *p++ = W(SpvOpTypeInt, 4); *p++ = 4; *p++ = 32; *p++ = 0;
    *p++ = W(SpvOpTypeBool, 2); *p++ = 5;
    *p++ = W(SpvOpTypeVector, 4); *p++ = 6; *p++ = 4; *p++ = 3;
    *p++ = W(SpvOpTypePointer, 4); *p++ = 7; *p++ = SpvStorageClassInput; *p++ = 6;
    *p++ = W(SpvOpTypePointer, 4); *p++ = 8; *p++ = SpvStorageClassInput; *p++ = 4;
    *p++ = W(SpvOpConstant, 4); *p++ = 4; *p++ = 9; *p++ = 0;
    *p++ = W(SpvOpVariable, 4); *p++ = 7; *p++ = 10; *p++ = SpvStorageClassInput;
    *p++ = W(SpvOpConstant, 4); *p++ = 4; *p++ = 11; *p++ = 8;
    *p++ = W(SpvOpConstant, 4); *p++ = 4; *p++ = 12; *p++ = 1;
    *p++ = W(SpvOpConstant, 4); *p++ = 4; *p++ = 13; *p++ = 7;
    *p++ = W(SpvOpTypeArray, 4); *p++ = 14; *p++ = 4; *p++ = 11;   // SSBO array (ArrayStride 4)
    *p++ = W(SpvOpTypeStruct, 3); *p++ = 15; *p++ = 14;
    *p++ = W(SpvOpTypePointer, 4); *p++ = 16; *p++ = SpvStorageClassStorageBuffer; *p++ = 15;
    *p++ = W(SpvOpTypePointer, 4); *p++ = 17; *p++ = SpvStorageClassStorageBuffer; *p++ = 4;
    *p++ = W(SpvOpVariable, 4); *p++ = 16; *p++ = 18; *p++ = SpvStorageClassStorageBuffer;
    *p++ = W(SpvOpVariable, 4); *p++ = 16; *p++ = 19; *p++ = SpvStorageClassStorageBuffer;
    // Workgroup array (separate undecorated OpTypeArray %u32 8)
    *p++ = W(SpvOpTypeArray, 4); *p++ = 20; *p++ = 4; *p++ = 11;
    *p++ = W(SpvOpTypePointer, 4); *p++ = 21; *p++ = SpvStorageClassWorkgroup; *p++ = 20;
    *p++ = W(SpvOpTypePointer, 4); *p++ = 22; *p++ = SpvStorageClassWorkgroup; *p++ = 4;
    *p++ = W(SpvOpVariable, 4); *p++ = 21; *p++ = 23; *p++ = SpvStorageClassWorkgroup;
    *p++ = W(SpvOpConstant, 4); *p++ = 4; *p++ = 24; *p++ = 0x12340000u;
    *p++ = W(SpvOpConstant, 4); *p++ = 4; *p++ = 25; *p++ = SpvScopeWorkgroup;
    *p++ = W(SpvOpConstant, 4); *p++ = 4; *p++ = 26; *p++ = SpvMemorySemanticsAcquireReleaseMask | SpvMemorySemanticsWorkgroupMemoryMask;
    // Function %main
    *p++ = W(SpvOpFunction, 5); *p++ = 1; *p++ = 2; *p++ = SpvFunctionControlMaskNone; *p++ = 3;
    *p++ = W(SpvOpLabel, 2); *p++ = 27;
    *p++ = W(SpvOpAccessChain, 5); *p++ = 8; *p++ = 28; *p++ = 10; *p++ = 9;
    *p++ = W(SpvOpLoad, 4); *p++ = 4; *p++ = 29; *p++ = 28;                         // i
    *p++ = W(SpvOpAccessChain, 6); *p++ = 17; *p++ = 30; *p++ = 18; *p++ = 9; *p++ = 29;
    *p++ = W(SpvOpLoad, 4); *p++ = 4; *p++ = 31; *p++ = 30;                         // in[i]
    *p++ = W(SpvOpBitwiseXor, 5); *p++ = 4; *p++ = 32; *p++ = 31; *p++ = 24;        // in[i] ^ 0x12340000
    *p++ = W(SpvOpAccessChain, 5); *p++ = 22; *p++ = 33; *p++ = 23; *p++ = 29;      // &wg[i]
    *p++ = W(SpvOpStore, 3); *p++ = 33; *p++ = 32;
    *p++ = W(SpvOpControlBarrier, 4); *p++ = 25; *p++ = 25; *p++ = 26;
    *p++ = W(SpvOpISub, 5); *p++ = 4; *p++ = 34; *p++ = 13; *p++ = 29;              // 7 - i
    *p++ = W(SpvOpAccessChain, 5); *p++ = 22; *p++ = 35; *p++ = 23; *p++ = 34;      // &wg[7 - i]
    *p++ = W(SpvOpBitwiseAnd, 5); *p++ = 4; *p++ = 36; *p++ = 29; *p++ = 12;
    *p++ = W(SpvOpIEqual, 5); *p++ = 5; *p++ = 37; *p++ = 36; *p++ = 9;
    *p++ = W(SpvOpSelect, 6); *p++ = 22; *p++ = 38; *p++ = 37; *p++ = 33; *p++ = 35;
    *p++ = W(SpvOpLoad, 4); *p++ = 4; *p++ = 39; *p++ = 38;
    *p++ = W(SpvOpAccessChain, 6); *p++ = 17; *p++ = 40; *p++ = 19; *p++ = 9; *p++ = 29;
    *p++ = W(SpvOpStore, 3); *p++ = 40; *p++ = 39;
    *p++ = W(SpvOpReturn, 1);
    *p++ = W(SpvOpFunctionEnd, 1);
    return (size_t)(p - out) * 4u;
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
    uint32_t *words = load_spv(dir, "vp_std.comp.spv", &bytes);
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
    for (uint32_t i = 0; ok && i < 8; i++) if (got[i] != (i + 1u) * 0x2001u) ok = 0;
    return ok;
}

static int run_vpsb_case(int expect_ok, const char *expect_why) {
    uint32_t words[256];
    size_t bytes = build_vpsb_spv(words);
    nvn_gpu_layout_ent ents[2] = {
        { .set = 0, .binding = 0, .cls = NVN_GPU_RES_SSBO, .count = 1, .stages = 1u << NVN_GPU_SH_CS, .access = NVN_GPU_ACC_R },
        { .set = 0, .binding = 1, .cls = NVN_GPU_RES_SSBO, .count = 1, .stages = 1u << NVN_GPU_SH_CS, .access = NVN_GPU_ACC_RW },
    };
    nvn_gpu_layout layout = { .ent = ents, .nent = 2 };
    nvn_gpu_shader_src src = { .stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV, .blob = words, .bytes = bytes, .entry = "main", .layout = &layout };
    nvn_gpu_shader sh = NULL;
    memset(&g_err, 0, sizeof g_err);
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh, &g_err);
    if (!expect_ok) {
        int ok = (r.code == NVN_GPU_E_UNSUPPORTED && sh == NULL && (!expect_why || strstr(g_err.why, expect_why) != NULL));
        if (!ok) {
            fprintf(stderr, "【红】vpsb expected reject(%s) got rc=%d why=%s\n", expect_why ? expect_why : "", r.code, g_err.why);
            if (sh) nvn_gpu_shader_release(sh);
        }
        return ok;
    }
    if (!nvn_gpu_ok(r) || !sh) {
        fprintf(stderr, "【红】vpsb expected accept got rc=%d why=%s\n", r.code, g_err.why);
        return 0;
    }
    nvn_gpu_stage_desc cs = { .sh = sh };
    nvn_gpu_pipe pipe = NULL;
    if (!nvn_gpu_ok(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &pipe, &g_err))) {
        fprintf(stderr, "【红】vpsb cpipe_get failed: %s\n", g_err.why);
        nvn_gpu_shader_release(sh); return 0;
    }
    uint32_t a[8] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7 };
    uint32_t o[8] = {0};
    nvn_gpu_buf ba = NULL, bo = NULL;
    int ok = nvn_gpu_ok(nvn_gpu_buf_create(sizeof a, NVN_GPU_MEM_SHARED, a, &ba, &g_err)) &&
             nvn_gpu_ok(nvn_gpu_buf_create(sizeof o, NVN_GPU_MEM_SHARED, o, &bo, &g_err));
    if (ok) {
        nvn_gpu_cmd cmd = NULL;
        ok = nvn_gpu_ok(nvn_gpu_cmd_begin(NULL, &cmd, &g_err));
        if (ok) {
            nvn_gpu_set_pipe(cmd, pipe);
            nvn_gpu_binding binds[2] = {
                { .ent = 0, .cls = NVN_GPU_RES_SSBO, .u.b = { .buf = ba, .len = sizeof a } },
                { .ent = 1, .cls = NVN_GPU_RES_SSBO, .u.b = { .buf = bo, .len = sizeof o } },
            };
            nvn_gpu_bind(cmd, NVN_GPU_SH_CS, binds, 2);
            nvn_gpu_dispatch(cmd, pipe, 1, 1, 1, NULL, 0);
            ok = nvn_gpu_ok(nvn_gpu_cmd_commit(cmd, ++g_sig, done_cb, NULL, &g_err)) &&
                 nvn_gpu_ok(nvn_gpu_fence_wait(g_sig, 5000000000ull, &g_err));
        }
    }
    uint32_t got[8] = {0};
    if (ok) memcpy(got, nvn_gpu_buf_cpu(bo), sizeof got);
    if (ba) nvn_gpu_buf_release(ba, NULL, NULL);
    if (bo) nvn_gpu_buf_release(bo, NULL, NULL);
    nvn_gpu_shader_release(sh);
    for (uint32_t i = 0; ok && i < 8; i++) {
        uint32_t want = (i & 1u) ? a[7u - i] : a[i];
        if (got[i] != want) {
            fprintf(stderr, "【红】vpsb mismatch[%u]: got=%#x want=%#x\n", i, got[i], want);
            ok = 0;
        }
    }
    return ok;
}

static int run_vp_full_case(int expect_ok, const char *expect_why) {
    uint32_t words[320];
    size_t bytes = build_vp_full_spv(words);
    nvn_gpu_layout_ent ents[2] = {
        { .set = 0, .binding = 0, .cls = NVN_GPU_RES_SSBO, .count = 1, .stages = 1u << NVN_GPU_SH_CS, .access = NVN_GPU_ACC_R },
        { .set = 0, .binding = 1, .cls = NVN_GPU_RES_SSBO, .count = 1, .stages = 1u << NVN_GPU_SH_CS, .access = NVN_GPU_ACC_RW },
    };
    nvn_gpu_layout layout = { .ent = ents, .nent = 2 };
    nvn_gpu_shader_src src = { .stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV, .blob = words, .bytes = bytes, .entry = "main", .layout = &layout };
    nvn_gpu_shader sh = NULL;
    memset(&g_err, 0, sizeof g_err);
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh, &g_err);
    if (!expect_ok) {
        int ok = (r.code == NVN_GPU_E_UNSUPPORTED && sh == NULL && (!expect_why || strstr(g_err.why, expect_why) != NULL));
        if (!ok) {
            fprintf(stderr, "【红】vp_full expected reject(%s) got rc=%d why=%s\n", expect_why ? expect_why : "", r.code, g_err.why);
            if (sh) nvn_gpu_shader_release(sh);
        }
        return ok;
    }
    if (!nvn_gpu_ok(r) || !sh) {
        fprintf(stderr, "【红】vp_full expected accept got rc=%d why=%s\n", r.code, g_err.why);
        return 0;
    }
    nvn_gpu_stage_desc cs = { .sh = sh };
    nvn_gpu_pipe pipe = NULL;
    if (!nvn_gpu_ok(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &pipe, &g_err))) {
        fprintf(stderr, "【红】vp_full cpipe_get failed: %s\n", g_err.why);
        nvn_gpu_shader_release(sh); return 0;
    }
    uint32_t in[8] = { 0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87 };
    uint32_t o[8] = {0};
    nvn_gpu_buf bi = NULL, bo = NULL;
    int ok = nvn_gpu_ok(nvn_gpu_buf_create(sizeof in, NVN_GPU_MEM_SHARED, in, &bi, &g_err)) &&
             nvn_gpu_ok(nvn_gpu_buf_create(sizeof o, NVN_GPU_MEM_SHARED, o, &bo, &g_err));
    if (ok) {
        nvn_gpu_cmd cmd = NULL;
        ok = nvn_gpu_ok(nvn_gpu_cmd_begin(NULL, &cmd, &g_err));
        if (ok) {
            nvn_gpu_set_pipe(cmd, pipe);
            nvn_gpu_binding binds[2] = {
                { .ent = 0, .cls = NVN_GPU_RES_SSBO, .u.b = { .buf = bi, .len = sizeof in } },
                { .ent = 1, .cls = NVN_GPU_RES_SSBO, .u.b = { .buf = bo, .len = sizeof o } },
            };
            nvn_gpu_bind(cmd, NVN_GPU_SH_CS, binds, 2);
            nvn_gpu_dispatch(cmd, pipe, 1, 1, 1, NULL, 0);
            ok = nvn_gpu_ok(nvn_gpu_cmd_commit(cmd, ++g_sig, done_cb, NULL, &g_err)) &&
                 nvn_gpu_ok(nvn_gpu_fence_wait(g_sig, 5000000000ull, &g_err));
        }
    }
    uint32_t got[8] = {0};
    if (ok) memcpy(got, nvn_gpu_buf_cpu(bo), sizeof got);
    if (bi) nvn_gpu_buf_release(bi, NULL, NULL);
    if (bo) nvn_gpu_buf_release(bo, NULL, NULL);
    nvn_gpu_shader_release(sh);
    for (uint32_t i = 0; ok && i < 8; i++) {
        uint32_t src_idx = (i & 1u) ? (7u - i) : i;
        uint32_t want = in[src_idx] ^ 0x12340000u;
        if (got[i] != want) {
            fprintf(stderr, "【红】vp_full mismatch[%u]: got=%#x want=%#x\n", i, got[i], want);
            ok = 0;
        }
    }
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <shader_dir> <on|off|hide-vp|hide-vpsb|withdraw>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1], *mode = argv[2];
    if (!strcmp(mode, "off") || !strcmp(mode, "withdraw")) setenv("debug.s2a.nvk_variable_pointers", "0", 1);
    else setenv("debug.s2a.nvk_variable_pointers", "1", 1);
    g_hide_vp   = !strcmp(mode, "hide-vp");
    g_hide_vpsb = !strcmp(mode, "hide-vpsb");

    struct s2a_vk_device dev;
    char why[256] = "";
    REQ(!s2a_vk_device_create(&dev, why, sizeof why));
    if (!strcmp(mode, "on") || !strcmp(mode, "withdraw")) {
        REQ(g_hw_vpsb);
    }
    // VUID-VkPhysicalDeviceVariablePointersFeatures-variablePointers-01431:
    // variablePointers cannot be enabled without variablePointersStorageBuffer.
    const int active = !strcmp(mode, "on") || !strcmp(mode, "withdraw") || g_hide_vp || g_hide_vpsb;
    const int expect_vpsb   = (active && !g_hide_vpsb) ? g_hw_vpsb : 0;
    const int expect_vp     = (expect_vpsb && !g_hide_vp) ? g_hw_vp : 0;
    const int expect_struct = expect_vpsb;

    nvn_gpu_open_desc od = {0};
    od.host_device = &dev;
    od.app_label = "variable-pointers-test";
    REQ(nvn_gpu_ok(nvn_gpu_open(&od, &g_err)));

    int std_ok  = run_std_case(dir);
    int vpsb_ok = run_vpsb_case(expect_vpsb, expect_vpsb ? NULL : "variablePointersStorageBuffer");
    int vp_ok   = run_vp_full_case(expect_vp, expect_vp ? NULL : "variablePointers");
    printf("variable-pointers mode=%s hw=%d/%d dev_s=%d dev_bits=%d/%d en_bits=%d/%d std=%d vpsb=%d vp=%d\n",
           mode, g_hw_vpsb, g_hw_vp, g_dev_struct, g_dev_vpsb, g_dev_vp,
           dev.variable_pointers_storage_buffer_enabled, dev.variable_pointers_enabled, std_ok, vpsb_ok, vp_ok);

    int rc = 0;
    if (g_dev_struct != expect_struct || g_dev_vpsb != expect_vpsb || g_dev_vp != expect_vp ||
        dev.variable_pointers_storage_buffer_enabled != expect_vpsb || dev.variable_pointers_enabled != expect_vp ||
        !std_ok || !vpsb_ok || !vp_ok) {
        rc = 1;
    }

    REQ(nvn_gpu_ok(nvn_gpu_close(5000000000ull, &g_err)));
    REQ(!s2a_vk_device_destroy(&dev, 1));
    return rc;
}
