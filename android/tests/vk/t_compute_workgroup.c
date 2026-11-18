// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real shader_load/cpipe/dispatch with lavapipe and validation. Only reduce the
// advertised limits, never raise them. Rejected shapes remain legal on the real
// device, so the unfixed backend can fail our assertion without invalid Vulkan.
#include "nvn_backend.h"
#include "vk_device.h"
#include "nvn_plat_vk.h"
#include "../../../graphics/third_party/spirv_cross/spirv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures, cases, modules, pipelines;
static uint64_t signal_value;
static nvn_gpu_err error;
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "fixture failure line %d: %s (%s)\n", __LINE__, #x, error.why); exit(2); } } while (0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))

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
static void done(void *u, const nvn_gpu_done *d) { (void)u; (void)d; }

// Declarations and pointer types come from NDK 29's Vulkan header.
extern __typeof__(vkCreateShaderModule) __real_vkCreateShaderModule;
extern __typeof__(vkCreateComputePipelines) __real_vkCreateComputePipelines;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *c,
                                                        const VkAllocationCallbacks *a, VkShaderModule *s) {
    modules++; return __real_vkCreateShaderModule(d, c, a, s);
}
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateComputePipelines(VkDevice d, VkPipelineCache cache, uint32_t n,
        const VkComputePipelineCreateInfo *c, const VkAllocationCallbacks *a, VkPipeline *p) {
    pipelines += n; return __real_vkCreateComputePipelines(d, cache, n, c, a, p);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateShaderModule), PFN_vkCreateShaderModule), "NDK module signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateComputePipelines), PFN_vkCreateComputePipelines), "NDK pipeline signature");

static uint32_t *read_shader(const char *dir, const char *name, size_t *bytes) {
    char path[1024]; REQUIRE(snprintf(path, sizeof path, "%s/%s.spv", dir, name) < (int)sizeof path);
    FILE *f = fopen(path, "rb"); REQUIRE(f); REQUIRE(!fseek(f, 0, SEEK_END));
    long n = ftell(f); REQUIRE(n > 20 && !(n & 3)); rewind(f);
    uint32_t *words = malloc((size_t)n); REQUIRE(words);
    REQUIRE(fread(words, 1, (size_t)n, f) == (size_t)n); fclose(f); *bytes = (size_t)n; return words;
}

// Change just the execution mode, or remove just the BuiltIn decoration. Keep
// the compiled shader's arithmetic/constant defaults and resource accesses.
static void variant(uint32_t *words, size_t *bytes, int mode) {
    unsigned local = 0, builtin = 0;
    for (size_t i = 5; i < *bytes / 4;) {
        uint32_t count = words[i] >> 16, op = words[i] & 65535u;
        REQUIRE(count && count <= *bytes / 4 - i);
        if (op == SpvOpExecutionMode && count == 6 && words[i + 2] == SpvExecutionModeLocalSize) {
            local++;
            if (mode == 1) words[i + 3] = words[i + 4] = words[i + 5] = 1;
            if (mode == 2) { words[i + 3] = 16; words[i + 4] = 16; words[i + 5] = 1; }
        }
        if (op == SpvOpDecorate && count == 4 && words[i + 2] == SpvDecorationBuiltIn &&
            words[i + 3] == SpvBuiltInWorkgroupSize) {
            builtin++;
            if (mode == 3) {
                memmove(words + i, words + i + count, *bytes - (i + count) * 4);
                *bytes -= count * 4; continue;
            }
        }
        i += count;
    }
    REQUIRE(local == 1 && builtin == 1);
}

// glslc's portable resource limits may cap Z at 64. The real test device must
// support 65, checked below. Raise only the controlled shader's literal/default
// from 64 to 65, then let real Vulkan validation check the resulting module.
static void axis_z65(uint32_t *words, size_t bytes) {
    unsigned modes = 0, constants = 0;
    for (size_t i = 5; i < bytes / 4;) {
        uint32_t count = words[i] >> 16, op = words[i] & 65535u;
        REQUIRE(count && count <= bytes / 4 - i);
        if (op == SpvOpExecutionMode && count == 6 && words[i + 2] == SpvExecutionModeLocalSize && words[i + 5] == 64) {
            words[i + 5] = 65; modes++;
        }
        if (op == SpvOpConstant && count == 4 && words[i + 3] == 64) { words[i + 3] = 65; constants++; }
        i += count;
    }
    REQUIRE(modes == 1 && constants == 1);
}

static void run_case(const char *name, const uint32_t *words, size_t bytes, const char *entry,
                     const char *reject, uint32_t expected_threads) {
    unsigned m0 = modules, p0 = pipelines, bad = 0; cases++;
    nvn_gpu_layout_ent ent = {0}; ent.cls = NVN_GPU_RES_SSBO; ent.count = 1;
    ent.stages = 1u << NVN_GPU_SH_CS; ent.access = NVN_GPU_ACC_RW;
    nvn_gpu_layout layout = {0}; layout.nent = 1; layout.ent = &ent;
    nvn_gpu_shader_src src = {0}; src.stage = NVN_GPU_SH_CS; src.kind = NVN_GPU_BLOB_SPIRV;
    src.blob = words; src.bytes = bytes; src.entry = entry; src.layout = expected_threads ? &layout : NULL;
    nvn_gpu_shader sh = NULL; memset(&error, 0, sizeof error);
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh, &error);
    if (reject) {
        bad = r.code != NVN_GPU_E_UNSUPPORTED || sh != NULL || modules != m0 || pipelines != p0 || !strstr(error.why, reject);
        printf("workgroup case=%s expected=reject:%s actual_rc=%d modules=%u pipelines=%u bad=%u why=%s\n",
               name, reject, r.code, modules - m0, pipelines - p0, bad, error.why);
    } else if (!nvn_gpu_ok(r) || !sh) {
        bad = 1; printf("workgroup case=%s expected=accept actual_rc=%d bad=1 why=%s\n", name, r.code, error.why);
    } else {
        nvn_gpu_stage_desc cs = {0}; cs.sh = sh; nvn_gpu_pipe pipe = NULL;
        GPU(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &pipe, &error));
        nvn_gpu_cmd cmd = NULL; GPU(nvn_gpu_cmd_begin(NULL, &cmd, &error));
        nvn_gpu_set_pipe(cmd, pipe);
        uint32_t zero = 0, result = 0; nvn_gpu_buf buffer = NULL;
        if (expected_threads) {
            GPU(nvn_gpu_buf_create(4, NVN_GPU_MEM_SHARED, &zero, &buffer, &error));
            nvn_gpu_binding binding = {0}; binding.cls = NVN_GPU_RES_SSBO;
            binding.u.b.buf = buffer; binding.u.b.len = 4;
            nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &binding, 1);
        }
        nvn_gpu_dispatch(cmd, pipe, 2, 1, 1, NULL, 0);
        GPU(nvn_gpu_cmd_commit(cmd, ++signal_value, done, NULL, &error));
        GPU(nvn_gpu_fence_wait(signal_value, 5000000000ull, &error));
        if (buffer) { memcpy(&result, nvn_gpu_buf_cpu(buffer), 4); nvn_gpu_buf_release(buffer, NULL, NULL); }
        bad = result != expected_threads * 2 || modules != m0 + 1 || pipelines != p0 + 1;
        printf("workgroup case=%s expected_threads=%u actual_threads=%u modules=%u pipelines=%u bad=%u\n",
               name, expected_threads * 2, result, modules - m0, pipelines - p0, bad);
    }
    if (sh) nvn_gpu_shader_release(sh);
    failures += bad;
}

#define INST(op, n) (((uint32_t)(n) << 16) | (op))
// Two valid entry points, with sizes attached to their own function IDs. The
// non-selected entry must neither reject the small entry nor hide the large one.
static uint32_t two_entries[] = {
    0, 0x00010000, 0, 7, 0,
    INST(SpvOpCapability, 2), SpvCapabilityShader,
    INST(SpvOpMemoryModel, 3), SpvAddressingModelLogical, SpvMemoryModelGLSL450,
    INST(SpvOpEntryPoint, 5), SpvExecutionModelGLCompute, 3, 0x6c616d73, 0x6c,
    INST(SpvOpEntryPoint, 5), SpvExecutionModelGLCompute, 4, 0x6772616c, 0x65,
    INST(SpvOpExecutionMode, 6), 3, SpvExecutionModeLocalSize, 16, 8, 1,
    INST(SpvOpExecutionMode, 6), 4, SpvExecutionModeLocalSize, 16, 16, 1,
    INST(SpvOpTypeVoid, 2), 1, INST(SpvOpTypeFunction, 3), 2, 1,
    INST(SpvOpFunction, 5), 1, 3, SpvFunctionControlMaskNone, 2,
    INST(SpvOpLabel, 2), 5, INST(SpvOpReturn, 1), INST(SpvOpFunctionEnd, 1),
    INST(SpvOpFunction, 5), 1, 4, SpvFunctionControlMaskNone, 2,
    INST(SpvOpLabel, 2), 6, INST(SpvOpReturn, 1), INST(SpvOpFunctionEnd, 1),
};

static void guard_edges(const char *dir) {
    run_case("entry_missing", two_entries, sizeof two_entries, "absent", "entry not found", 0);
    run_case("entry_too_long", two_entries, sizeof two_entries,
             "entry_name_exceeding_the_backend_storage_capacity", "entry name exceeds", 0);
    uint32_t words[sizeof two_entries / 4];
    for (int mode = 0; mode < 4; mode++) {
        memcpy(words, two_entries, sizeof words);
        for (size_t i = 5; i < sizeof words / 4; i += words[i] >> 16) {
            if ((words[i] & 65535u) != SpvOpExecutionMode || words[i + 1] != 3) continue;
            if (mode == 0) words[i + 3] = 0;
            if (mode == 1) words[i + 3] = UINT32_MAX;
            if (mode == 2) words[i] = 0; // A zero-length instruction must not loop or reach the driver.
            if (mode == 3) {
                words[i] = INST(SpvOpExecutionModeId, 6); words[i + 2] = SpvExecutionModeLocalSizeId;
            }
            break;
        }
        const char *names[] = {"zero", "large_dimension", "malformed_length", "local_size_id"};
        const char *reasons[] = {"LocalSize[0]=0", "maxComputeWorkGroupSize", "malformed instruction length", "maintenance4"};
        run_case(names[mode], words, sizeof words, "small", reasons[mode], 0);
    }
    // A legal default expression (IAdd 1 1) instead of a scalar SpecConstant.
    // The backend does not evaluate arbitrary specialization expressions; it
    // must reject this explicitly rather than using the overridden LocalSize.
    size_t bytes; uint32_t *expr = read_shader(dir, "spec_boundary", &bytes);
    uint32_t *grown = realloc(expr, bytes + 8); REQUIRE(grown); expr = grown;
    size_t def = 0, one_pos = 0; uint32_t type = 0, id = 0, one = 0;
    for (size_t i = 5; i < bytes / 4; i += expr[i] >> 16) {
        if ((expr[i] & 65535u) == SpvOpSpecConstant && expr[i + 3] == 16) {
            def = i; type = expr[i + 1]; id = expr[i + 2]; break;
        }
    }
    REQUIRE(def);
    for (size_t i = 5; i < bytes / 4; i += expr[i] >> 16)
        if ((expr[i] & 65535u) == SpvOpConstant && expr[i + 1] == type && expr[i + 3] == 1) { one = expr[i + 2]; one_pos = i; }
    REQUIRE(one && one_pos < def);
    memmove(expr + def + 6, expr + def + 4, bytes - (def + 4) * 4); bytes += 8;
    expr[def] = INST(SpvOpSpecConstantOp, 6); expr[def + 3] = SpvOpIAdd; expr[def + 4] = expr[def + 5] = one;
    unsigned removed = 0;
    for (size_t i = 5; i < bytes / 4;) {
        uint32_t len = expr[i] >> 16;
        if ((expr[i] & 65535u) == SpvOpDecorate && len == 4 && expr[i + 1] == id && expr[i + 2] == SpvDecorationSpecId) {
            memmove(expr + i, expr + i + len, bytes - (i + len) * 4); bytes -= len * 4; removed++; continue;
        }
        i += len;
    }
    REQUIRE(removed == 1);
    run_case("constant_expression", expr, bytes, "main", "unsupported WorkgroupSize component expression", 128);
    free(expr);
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2 || (argc == 3 && !strcmp(argv[2], "--guard-edges")));
    struct s2a_vk_device device; char why[256];
    REQUIRE(!s2a_vk_device_create(&device, why, sizeof why));
    VkPhysicalDeviceLimits *l = &device.properties.limits;
    printf("device=%s real_limits=%u,%u,%u invocations=%u\n", device.properties.deviceName,
           l->maxComputeWorkGroupSize[0], l->maxComputeWorkGroupSize[1], l->maxComputeWorkGroupSize[2], l->maxComputeWorkGroupInvocations);
    REQUIRE(l->maxComputeWorkGroupInvocations >= 256 && l->maxComputeWorkGroupSize[0] >= 129 &&
            l->maxComputeWorkGroupSize[1] >= 129 && l->maxComputeWorkGroupSize[2] >= 65);
    l->maxComputeWorkGroupInvocations = 128;
    l->maxComputeWorkGroupSize[0] = l->maxComputeWorkGroupSize[1] = 128; l->maxComputeWorkGroupSize[2] = 64;
    nvn_gpu_open_desc desc = {0}; desc.host_device = &device; desc.app_label = "compute-workgroup-test";
    GPU(nvn_gpu_open(&desc, &error));
    const struct { const char *name, *reject; uint32_t threads; } tests[] = {
        {"one", NULL, 1}, {"boundary", NULL, 128}, {"boundary3d", NULL, 128},
        {"product", "maxComputeWorkGroupInvocations", 256}, {"product2", "maxComputeWorkGroupInvocations", 256},
        {"axis_x", "maxComputeWorkGroupSize", 129}, {"axis_y", "maxComputeWorkGroupSize", 129},
        {"axis_z", "maxComputeWorkGroupSize", 65},
        {"spec_boundary", NULL, 128}, {"spec_product", "maxComputeWorkGroupInvocations", 256},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        size_t bytes; uint32_t *words = read_shader(argv[1], tests[i].name, &bytes);
        if (!strcmp(tests[i].name, "axis_z")) axis_z65(words, bytes);
        run_case(tests[i].name, words, bytes, "main", tests[i].reject, tests[i].threads); free(words);
    }
    for (int mode = 1; mode <= 3; mode++) {
        size_t bytes; uint32_t *words = read_shader(argv[1], mode == 2 ? "boundary" : "product", &bytes);
        variant(words, &bytes, mode);
        run_case(mode == 1 ? "builtin_overrides_small" : mode == 2 ? "builtin_overrides_large" : "literal_only",
                 words, bytes, "main", mode == 2 ? NULL : "maxComputeWorkGroupInvocations", mode == 2 ? 128 : 256);
        free(words);
    }
    two_entries[0] = SpvMagicNumber;
    run_case("entry_small", two_entries, sizeof two_entries, "small", NULL, 0);
    run_case("entry_large", two_entries, sizeof two_entries, "large", "maxComputeWorkGroupInvocations", 0);
    if (argc == 3) guard_edges(argv[1]);
    GPU(nvn_gpu_close(5000000000ull, &error)); REQUIRE(!s2a_vk_device_destroy(&device, 1));
    printf("workgroup cases=%u failures=%u\n", cases, failures);
    return failures ? 1 : 0;
}
