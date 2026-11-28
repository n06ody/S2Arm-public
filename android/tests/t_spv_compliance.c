// #1865 宿主回归：spv_compliance.h 声明扫描 + 能力/扩展需求表。
// 合成 SPIR-V（手排 32 位词，不依赖工具链）逐类负控；正样用真库件的声明集（vs/cs 翻译器样板块）。
// 反例基线：表外能力必须能被扫到（漏扫=静默放行）、OpExtension 名必须按字面比（前缀/大小写都过不了）、
//   PushConstant / Workgroup 字节按声明类型图实算（不许估）、畸形模块具名失败不许崩。
#include "../native/direct/spirv_compliance.h"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "assertion line %d: %s\n", __LINE__, #x); exit(2); } } while (0)
#define W32(op, len) ((((len) & 0xffffu) << 16) | ((op) & 0xffffu))
#define MAGIC 0x07230203u

static size_t put_str(uint32_t *w, const char *s) {   // SPIR-V 字面串：含终止符打包成词
    size_t n = strlen(s) + 1; memcpy(w, s, n); return (n + 3) / 4;
}
// 头五词 + 声明区。调用方从 words[5] 续写。
static uint32_t *spv_head(uint32_t *w, uint32_t version, uint32_t bound) {
    w[0] = MAGIC; w[1] = version; w[2] = 0; w[3] = bound; w[4] = 0;
    return w + 5;
}
static int has_cap(const struct s2a_spv_decl *d, uint32_t cap) {
    for (unsigned i = 0; i < d->n_caps; i++) if (d->caps[i] == cap) return 1;
    return 0;
}

static void case_caps_table(void) {
    CHECK(s2a_spv_cap_req(SpvCapabilityInt16) == SPV_REQ_INT16);
    CHECK(s2a_spv_cap_req(SpvCapabilityInt64) == SPV_REQ_INT64);
    CHECK(s2a_spv_cap_req(SpvCapabilityInt64Atomics) == SPV_REQ_INT64);
    CHECK(s2a_spv_cap_req(SpvCapabilityFloat16) == SPV_REQ_FLOAT16);
    CHECK(s2a_spv_cap_req(SpvCapabilityFloat64) == SPV_REQ_FLOAT64);
    CHECK(s2a_spv_cap_req(SpvCapabilityInt8) == SPV_REQ_INT8);
    CHECK(s2a_spv_cap_req(SpvCapabilityGeometry) == SPV_REQ_GEOMETRY);
    CHECK(s2a_spv_cap_req(SpvCapabilityTessellation) == SPV_REQ_TESSELLATION);
    CHECK(s2a_spv_cap_req(SpvCapabilityClipDistance) == SPV_REQ_CLIP_DIST);
    CHECK(s2a_spv_cap_req(SpvCapabilityCullDistance) == SPV_REQ_CULL_DIST);
    CHECK(s2a_spv_cap_req(SpvCapabilityImageGatherExtended) == SPV_REQ_IMAGE_GATHER_EXT);
    CHECK(s2a_spv_cap_req(SpvCapabilityImageCubeArray) == SPV_REQ_CUBE_ARRAY);
    CHECK(s2a_spv_cap_req(SpvCapabilityMultiViewport) == SPV_REQ_MULTI_VIEWPORT);
    CHECK(s2a_spv_cap_req(SpvCapabilityStorageImageReadWithoutFormat) == SPV_REQ_STORAGE_READ_WOF);
    CHECK(s2a_spv_cap_req(SpvCapabilityStorageImageWriteWithoutFormat) == SPV_REQ_STORAGE_WRITE_WOF);
    CHECK(s2a_spv_cap_req(SpvCapabilityGroupNonUniformBallot) == SPV_REQ_API_1_1);
    CHECK(s2a_spv_cap_req(SpvCapabilityDrawParameters) == SPV_REQ_DRAW_PARAMETERS);
    CHECK(s2a_spv_cap_req(SpvCapabilityStorageBuffer16BitAccess) == SPV_REQ_16BIT_STORAGE_SSBO);
    CHECK(s2a_spv_cap_req(SpvCapabilityUniformAndStorageBuffer16BitAccess) == SPV_REQ_16BIT_STORAGE_UBO_SSBO);
    CHECK(s2a_spv_cap_req(SpvCapabilityStoragePushConstant16) == SPV_REQ_16BIT_STORAGE_PUSH);
    CHECK(s2a_spv_cap_req(SpvCapabilityStorageInputOutput16) == SPV_REQ_16BIT_STORAGE_IO);
    CHECK(s2a_spv_cap_req(SpvCapabilityVariablePointersStorageBuffer) == SPV_REQ_VARIABLE_POINTERS_SB);
    CHECK(s2a_spv_cap_req(SpvCapabilityVariablePointers) == SPV_REQ_VARIABLE_POINTERS);
    CHECK(s2a_spv_cap_req(SpvCapabilityMultiView) == SPV_REQ_MULTIVIEW);
    CHECK(s2a_spv_cap_req(SpvCapabilityDenormPreserve) == SPV_REQ_FLOAT_CONTROLS);
    CHECK(s2a_spv_cap_req(SpvCapabilityShader) == SPV_REQ_CORE);
    CHECK(s2a_spv_cap_req(SpvCapabilityImageQuery) == SPV_REQ_CORE);
    CHECK(s2a_spv_cap_req(SpvCapabilitySampledBuffer) == SPV_REQ_CORE);
    // 表外能力：GroupNonUniformPartitionedNV(5297) / 未知名号 → UNLISTED
    CHECK(s2a_spv_cap_req(5297u) == SPV_REQ_UNLISTED);
    CHECK(s2a_spv_cap_req(9999u) == SPV_REQ_UNLISTED);
    // req 名可回查（日志用），未知名不出指针（拒收行按数值打）
    CHECK(s2a_spv_req_name(SPV_REQ_INT16) && strstr(s2a_spv_req_name(SPV_REQ_INT16), "Int16"));
    CHECK(s2a_spv_req_name(SPV_REQ_8BIT_STORAGE_SSBO) && !strcmp(s2a_spv_req_name(SPV_REQ_8BIT_STORAGE_SSBO), "storageBuffer8BitAccess"));
    CHECK(s2a_spv_req_name(SPV_REQ_8BIT_STORAGE_UBO_SSBO) && !strcmp(s2a_spv_req_name(SPV_REQ_8BIT_STORAGE_UBO_SSBO), "uniformAndStorageBuffer8BitAccess"));
    CHECK(s2a_spv_req_name(SPV_REQ_8BIT_STORAGE_PUSH) && !strcmp(s2a_spv_req_name(SPV_REQ_8BIT_STORAGE_PUSH), "storagePushConstant8"));
    CHECK(s2a_spv_req_name(SPV_REQ_16BIT_STORAGE_SSBO) && !strcmp(s2a_spv_req_name(SPV_REQ_16BIT_STORAGE_SSBO), "storageBuffer16BitAccess"));
    CHECK(s2a_spv_req_name(SPV_REQ_16BIT_STORAGE_UBO_SSBO) && !strcmp(s2a_spv_req_name(SPV_REQ_16BIT_STORAGE_UBO_SSBO), "uniformAndStorageBuffer16BitAccess"));
    CHECK(s2a_spv_req_name(SPV_REQ_16BIT_STORAGE_PUSH) && !strcmp(s2a_spv_req_name(SPV_REQ_16BIT_STORAGE_PUSH), "storagePushConstant16"));
    CHECK(s2a_spv_req_name(SPV_REQ_16BIT_STORAGE_IO) && !strcmp(s2a_spv_req_name(SPV_REQ_16BIT_STORAGE_IO), "storageInputOutput16"));
    CHECK(s2a_spv_req_name(SPV_REQ_VARIABLE_POINTERS_SB) && !strcmp(s2a_spv_req_name(SPV_REQ_VARIABLE_POINTERS_SB), "variablePointersStorageBuffer"));
    CHECK(s2a_spv_req_name(SPV_REQ_VARIABLE_POINTERS) && !strcmp(s2a_spv_req_name(SPV_REQ_VARIABLE_POINTERS), "variablePointers"));
    // 扩展表
    CHECK(s2a_spv_ext_req("SPV_KHR_shader_draw_parameters") == SPV_REQ_DRAW_PARAMETERS);
    CHECK(s2a_spv_ext_req("SPV_KHR_16bit_storage") == SPV_REQ_16BIT_STORAGE);
    CHECK(s2a_spv_ext_req("SPV_KHR_8bit_storage") == SPV_REQ_8BIT_STORAGE);
    CHECK(s2a_spv_ext_req("SPV_KHR_variable_pointers") == SPV_REQ_VARIABLE_POINTERS_SB);
    CHECK(s2a_spv_ext_req("SPV_KHR_multiview") == SPV_REQ_MULTIVIEW);
    CHECK(s2a_spv_ext_req("SPV_EXT_descriptor_indexing") == SPV_REQ_DESCRIPTOR_INDEXING);
    CHECK(s2a_spv_ext_req("SPV_KHR_storage_buffer_storage_class") == SPV_REQ_SBC_CLASS);
    CHECK(s2a_spv_ext_req("SPV_KHR_device_group") == SPV_REQ_API_1_1);
    CHECK(s2a_spv_ext_req("SPV_GOOGLE_fancy") == SPV_REQ_UNLISTED);
    CHECK(s2a_spv_ext_req("SPV_KHR_16bit_storage_EXTRA") == SPV_REQ_UNLISTED);   // 前缀不吞
    printf("case_caps_table ok\n");
}

static void case_scan_positive(void) {
    // 真库件 vs 阶段的声明集（9 能力 + 无扩展 + 无 push）：零误拒的最小正样
    uint32_t w[128]; uint32_t *p = spv_head(w, 0x10300u, 64);
    const uint32_t caps[] = {SpvCapabilityShader, SpvCapabilityGroupNonUniformBallot,
        SpvCapabilityGroupNonUniformShuffle, SpvCapabilityGroupNonUniformVote,
        SpvCapabilityGroupNonUniformArithmetic, SpvCapabilityImageBuffer,
        SpvCapabilityImageGatherExtended, SpvCapabilityImageQuery,
        SpvCapabilitySampledBuffer, SpvCapabilityDrawParameters};
    for (unsigned i = 0; i < sizeof caps / sizeof caps[0]; i++) { p[0] = W32(SpvOpCapability, 2); p[1] = caps[i]; p += 2; }
    struct s2a_spv_decl d;
    CHECK(s2a_spv_scan(w, (p - w) * 4, &d) == NULL);
    CHECK(d.version == 0x10300u);
    CHECK(d.n_caps == 10 && !d.caps_truncated);
    CHECK(d.n_ext == 0 && !d.has_push_bytes && d.push_bytes == 0);
    for (unsigned i = 0; i < sizeof caps / sizeof caps[0]; i++) CHECK(has_cap(&d, caps[i]));
    // 全表逐项可判：每个声明都有 CORE 或映射需求（真库集不该有 UNLISTED）
    for (unsigned i = 0; i < d.n_caps; i++) CHECK(s2a_spv_cap_req(d.caps[i]) != SPV_REQ_UNLISTED);
    printf("case_scan_positive ok\n");
}

static void case_scan_ext_and_unlisted(void) {
    uint32_t w[128]; uint32_t *p = spv_head(w, 0x10000u, 64);
    p[0] = W32(SpvOpCapability, 2); p[1] = SpvCapabilityInt16; p += 2;
    p[0] = W32(SpvOpCapability, 2); p[1] = 5297u; p += 2;            // 表外能力
    size_t u = put_str(p + 1, "SPV_KHR_16bit_storage"); p[0] = W32(SpvOpExtension, 1 + u); p += 1 + u;
    u = put_str(p + 1, "SPV_GOOGLE_fancy_ext"); p[0] = W32(SpvOpExtension, 1 + u); p += 1 + u;   // 表外扩展
    struct s2a_spv_decl d;
    CHECK(s2a_spv_scan(w, (p - w) * 4, &d) == NULL);
    CHECK(d.n_caps == 2 && has_cap(&d, SpvCapabilityInt16) && has_cap(&d, 5297u));
    CHECK(d.n_ext == 2 && !strcmp(d.exts[0], "SPV_KHR_16bit_storage") && !strcmp(d.exts[1], "SPV_GOOGLE_fancy_ext"));
    printf("case_scan_ext_and_unlisted ok\n");
}

// push 变量：scalar → array[8] → ptr → var。float_words 3 = 经典形，4 = SPV 1.6 带编码操作数形。
static size_t emit_push_mod(uint32_t *w, uint32_t float_words) {
    uint32_t *p = spv_head(w, 0x10000u, 16);
    p[0] = W32(SpvOpTypeFloat, float_words); p[1] = 1; p[2] = 32; p += float_words;
    p[0] = W32(SpvOpTypeInt, 4); p[1] = 2; p[2] = 32; p[3] = 0; p += 4;
    p[0] = W32(SpvOpConstant, 4); p[1] = 2; p[2] = 3; p[3] = 8; p += 4;
    p[0] = W32(SpvOpTypeArray, 4); p[1] = 4; p[2] = 1; p[3] = 3; p += 4;
    p[0] = W32(SpvOpTypePointer, 4); p[1] = 5; p[2] = SpvStorageClassPushConstant; p[3] = 4; p += 4;
    p[0] = W32(SpvOpVariable, 4); p[1] = 5; p[2] = 6; p[3] = SpvStorageClassPushConstant; p += 4;
    return (size_t)(p - w) * 4;
}
static void case_push_bytes(void) {
    uint32_t w[256]; struct s2a_spv_decl d;
    // float[8] push = 32 字节：3 词（经典）与 4 词（SPV 1.6 编码操作数）都合法
    CHECK(s2a_spv_scan(w, emit_push_mod(w, 3), &d) == NULL);
    CHECK(d.has_push_bytes && d.push_bytes == 32);
    CHECK(s2a_spv_scan(w, emit_push_mod(w, 4), &d) == NULL);
    CHECK(d.has_push_bytes && d.push_bytes == 32);
    // 5 词 OpTypeFloat 仍是畸形（编码操作数至多 1 个）
    CHECK(s2a_spv_scan(w, emit_push_mod(w, 5), &d) != NULL);
    printf("case_push_bytes ok\n");
}

static void case_workgroup_bytes(void) {
    // 12588501 病灶形：Workgroup uint[11264] = 45056 字节，从 SPV 直读、不靠元数据。
    uint32_t w[256]; uint32_t *p = spv_head(w, 0x10300u, 16);
    p[0] = W32(SpvOpTypeInt, 4); p[1] = 2; p[2] = 32; p[3] = 0; p += 4;
    p[0] = W32(SpvOpConstant, 4); p[1] = 2; p[2] = 3; p[3] = 11264; p += 4;
    p[0] = W32(SpvOpTypeArray, 4); p[1] = 4; p[2] = 2; p[3] = 3; p += 4;
    p[0] = W32(SpvOpTypePointer, 4); p[1] = 5; p[2] = SpvStorageClassWorkgroup; p[3] = 4; p += 4;
    p[0] = W32(SpvOpVariable, 4); p[1] = 5; p[2] = 6; p[3] = SpvStorageClassWorkgroup; p += 4;
    uint64_t total = 0;
    CHECK(s2a_spv_workgroup_bytes(w, (p - w) * 4, &total) == NULL);
    CHECK(total == 45056);
    struct s2a_spv_decl d;
    CHECK(s2a_spv_scan(w, (p - w) * 4, &d) == NULL);
    CHECK(d.has_workgroup_init == 0);
    // 带 OpConstantNull 初始化器的 Workgroup OpVariable（5 词）→ 必须扫出 has_workgroup_init=1
    p = spv_head(w, 0x10300u, 16);
    p[0] = W32(SpvOpTypeInt, 4); p[1] = 2; p[2] = 32; p[3] = 0; p += 4;
    p[0] = W32(SpvOpConstant, 4); p[1] = 2; p[2] = 3; p[3] = 64; p += 4;
    p[0] = W32(SpvOpTypeArray, 4); p[1] = 4; p[2] = 2; p[3] = 3; p += 4;
    p[0] = W32(SpvOpTypePointer, 4); p[1] = 5; p[2] = SpvStorageClassWorkgroup; p[3] = 4; p += 4;
    p[0] = W32(SpvOpConstantNull, 3); p[1] = 4; p[2] = 7; p += 3;
    p[0] = W32(SpvOpVariable, 5); p[1] = 5; p[2] = 6; p[3] = SpvStorageClassWorkgroup; p[4] = 7; p += 5;
    CHECK(s2a_spv_scan(w, (p - w) * 4, &d) == NULL);
    CHECK(d.has_workgroup_init == 1);
    printf("case_workgroup_bytes ok\n");
}

static void case_malformed(void) {
    uint32_t w[64]; uint32_t *p = spv_head(w, 0x10300u, 64);
    struct s2a_spv_decl d;
    w[0] = 0; CHECK(s2a_spv_scan(w, sizeof w, &d) != NULL);                       // 坏魔数
    w[0] = MAGIC; CHECK(s2a_spv_scan(w, 19, &d) != NULL);                        // 短头
    p[0] = W32(SpvOpCapability, 0);                                              // 零长指令
    CHECK(s2a_spv_scan(w, 24, &d) != NULL);
    p[0] = W32(SpvOpExtension, 3); memcpy(p + 1, "ABCDEFGH", 8);              // 8 字节无终止串名
    CHECK(s2a_spv_scan(w, 32, &d) != NULL);
    printf("case_malformed ok\n");
}

static void case_scalar_block_layout(void) {
    uint32_t w[256]; struct s2a_spv_decl d;
    // 1) 标准 std430 / relaxed_block_layout（float@0 + vec3@4 不跨 16B，vec3[] ArrayStride=16）→ 0
    uint32_t *p = spv_head(w, 0x10300u, 16);
    p[0] = W32(SpvOpDecorate, 4); p[1] = 5; p[2] = SpvDecorationArrayStride; p[3] = 16; p += 4;
    p[0] = W32(SpvOpMemberDecorate, 5); p[1] = 6; p[2] = 0; p[3] = SpvDecorationOffset; p[4] = 0; p += 5;
    p[0] = W32(SpvOpMemberDecorate, 5); p[1] = 6; p[2] = 1; p[3] = SpvDecorationOffset; p[4] = 4; p += 5;
    p[0] = W32(SpvOpTypeFloat, 3); p[1] = 1; p[2] = 32; p += 3;
    p[0] = W32(SpvOpTypeVector, 4); p[1] = 2; p[2] = 1; p[3] = 3; p += 4;
    p[0] = W32(SpvOpTypeInt, 4); p[1] = 3; p[2] = 32; p[3] = 0; p += 4;
    p[0] = W32(SpvOpConstant, 4); p[1] = 3; p[2] = 4; p[3] = 2; p += 4;
    p[0] = W32(SpvOpTypeArray, 4); p[1] = 5; p[2] = 2; p[3] = 4; p += 4;
    p[0] = W32(SpvOpTypeStruct, 4); p[1] = 6; p[2] = 1; p[3] = 2; p += 4;
    CHECK(s2a_spv_scan(w, (p - w) * 4, &d) == NULL && d.has_scalar_block_layout == 0);
    // 2) vec3 偏移 12（跨 16B 对齐边界）→ 1
    p = spv_head(w, 0x10300u, 16);
    p[0] = W32(SpvOpMemberDecorate, 5); p[1] = 6; p[2] = 0; p[3] = SpvDecorationOffset; p[4] = 0; p += 5;
    p[0] = W32(SpvOpMemberDecorate, 5); p[1] = 6; p[2] = 1; p[3] = SpvDecorationOffset; p[4] = 12; uint32_t *offset = &p[4]; p += 5;
    p[0] = W32(SpvOpTypeFloat, 3); p[1] = 1; p[2] = 32; uint32_t *width = &p[2]; p += 3;
    p[0] = W32(SpvOpTypeVector, 4); p[1] = 2; p[2] = 1; p[3] = 3; uint32_t *components = &p[3]; p += 4;
    p[0] = W32(SpvOpTypeStruct, 4); p[1] = 6; p[2] = 2; p[3] = 2; p += 4;
    CHECK(s2a_spv_scan(w, (p - w) * 4, &d) == NULL && d.has_scalar_block_layout == 1);
    // relaxed 布局按 16B 边界判跨越。vec2@4 合法，vec2@12 跨界。
    *offset = 4;
    *components = 2;
    CHECK(s2a_spv_scan(w, (p - w) * 4, &d) == NULL && d.has_scalar_block_layout == 0);
    *offset = 12;
    CHECK(s2a_spv_scan(w, (p - w) * 4, &d) == NULL && d.has_scalar_block_layout == 1);
    // 超过 16B 的向量只要求从 16B 边界开始。
    *width = 64;
    *components = 3;
    *offset = 16;
    CHECK(s2a_spv_scan(w, (p - w) * 4, &d) == NULL && d.has_scalar_block_layout == 0);
    *offset = 8;
    CHECK(s2a_spv_scan(w, (p - w) * 4, &d) == NULL && d.has_scalar_block_layout == 1);
    // 3) vec3 数组 ArrayStride=12 → 1
    p = spv_head(w, 0x10300u, 16);
    p[0] = W32(SpvOpDecorate, 4); p[1] = 5; p[2] = SpvDecorationArrayStride; p[3] = 12; p += 4;
    p[0] = W32(SpvOpTypeFloat, 3); p[1] = 1; p[2] = 32; p += 3;
    p[0] = W32(SpvOpTypeVector, 4); p[1] = 2; p[2] = 1; p[3] = 3; p += 4;
    p[0] = W32(SpvOpTypeInt, 4); p[1] = 3; p[2] = 32; p[3] = 0; p += 4;
    p[0] = W32(SpvOpConstant, 4); p[1] = 3; p[2] = 4; p[3] = 2; p += 4;
    p[0] = W32(SpvOpTypeArray, 4); p[1] = 5; p[2] = 2; p[3] = 4; p += 4;
    CHECK(s2a_spv_scan(w, (p - w) * 4, &d) == NULL && d.has_scalar_block_layout == 1);
    printf("case_scalar_block_layout ok\n");
}

static size_t emit_ubo_mod(uint32_t *w, uint32_t block_dec, uint32_t stride) {
    uint32_t *p = spv_head(w, 0x10300u, 16);
    p[0] = W32(SpvOpDecorate, 3); p[1] = 5; p[2] = block_dec; p += 3;
    p[0] = W32(SpvOpDecorate, 4); p[1] = 4; p[2] = SpvDecorationArrayStride; p[3] = stride; p += 4;
    p[0] = W32(SpvOpTypeFloat, 3); p[1] = 1; p[2] = 32; p += 3;
    p[0] = W32(SpvOpTypeInt, 4); p[1] = 2; p[2] = 32; p[3] = 0; p += 4;
    p[0] = W32(SpvOpConstant, 4); p[1] = 2; p[2] = 3; p[3] = 8; p += 4;
    p[0] = W32(SpvOpTypeArray, 4); p[1] = 4; p[2] = 1; p[3] = 3; p += 4;
    p[0] = W32(SpvOpTypeStruct, 3); p[1] = 5; p[2] = 4; p += 3;
    p[0] = W32(SpvOpTypePointer, 4); p[1] = 6; p[2] = SpvStorageClassUniform; p[3] = 5; p += 4;
    p[0] = W32(SpvOpVariable, 4); p[1] = 6; p[2] = 7; p[3] = SpvStorageClassUniform; p += 4;
    return (size_t)(p - w) * 4;
}
static void case_ubo_std430_layout(void) {
    uint32_t w[256]; struct s2a_spv_decl d;
    // 1) 标准 std140 UBO（Block + ArrayStride=16）→ 0
    CHECK(s2a_spv_scan(w, emit_ubo_mod(w, SpvDecorationBlock, 16), &d) == NULL && d.has_ubo_std430_layout == 0);
    // 2) 旧式 SSBO（BufferBlock + ArrayStride=4）→ 0（不误报 SSBO）
    CHECK(s2a_spv_scan(w, emit_ubo_mod(w, SpvDecorationBufferBlock, 4), &d) == NULL && d.has_ubo_std430_layout == 0);
    // 3) std430 紧凑 UBO（Block + ArrayStride=4）→ 1
    CHECK(s2a_spv_scan(w, emit_ubo_mod(w, SpvDecorationBlock, 4), &d) == NULL && d.has_ubo_std430_layout == 1);
    printf("case_ubo_std430_layout ok\n");
}

static void case_local_size_id(void) {
    // 合成 SPIR-V 1.3 计算模块：OpExecutionModeId %fn LocalSizeId %c4 %c2 %c1
    uint32_t w[128]; uint32_t *p = spv_head(w, 0x10300u, 16);
    p[0] = W32(SpvOpCapability, 2); p[1] = SpvCapabilityShader; p += 2;
    p[0] = W32(SpvOpMemoryModel, 3); p[1] = SpvAddressingModelLogical; p[2] = SpvMemoryModelGLSL450; p += 3;
    p[0] = W32(SpvOpEntryPoint, 5); p[1] = SpvExecutionModelGLCompute; p[2] = 3;
    size_t nm = put_str(p + 3, "main"); p[0] = W32(SpvOpEntryPoint, (uint32_t)(3 + nm)); p += 3 + nm;
    uint32_t *exec_mode = p;
    p[0] = W32(SpvOpExecutionModeId, 6); p[1] = 3; p[2] = SpvExecutionModeLocalSizeId; p[3] = 7; p[4] = 8; p[5] = 9; p += 6;
    p[0] = W32(SpvOpTypeVoid, 2); p[1] = 1; p += 2;
    p[0] = W32(SpvOpTypeFunction, 3); p[1] = 2; p[2] = 1; p += 3;
    p[0] = W32(SpvOpTypeInt, 4); p[1] = 6; p[2] = 32; p[3] = 0; p += 4;
    p[0] = W32(SpvOpConstant, 4); p[1] = 6; p[2] = 7; p[3] = 4; p += 4;
    p[0] = W32(SpvOpSpecConstant, 4); p[1] = 6; p[2] = 8; p[3] = 2; p += 4;
    p[0] = W32(SpvOpConstant, 4); p[1] = 6; p[2] = 9; p[3] = 1; p += 4;
    p[0] = W32(SpvOpFunction, 5); p[1] = 1; p[2] = 3; p[3] = SpvFunctionControlMaskNone; p[4] = 2; p += 5;
    p[0] = W32(SpvOpLabel, 2); p[1] = 4; p += 2;
    p[0] = W32(SpvOpReturn, 1); p += 1;
    p[0] = W32(SpvOpFunctionEnd, 1); p += 1;
    size_t nb = (size_t)(p - w) * 4;
    struct s2a_spv_decl d;
    CHECK(s2a_spv_scan(w, nb, &d) == NULL && d.has_local_size_id == 1);
    CHECK(s2a_spv_req_name(SPV_REQ_MAINTENANCE4) && !strcmp(s2a_spv_req_name(SPV_REQ_MAINTENANCE4), "VK_KHR_maintenance4"));
    uint32_t sz[3] = {0};
    // maintenance4 关闭时拒收并具名 maintenance4
    const char *why_off = s2a_spirv_workgroup_size_ex(w, nb, "main", 0, sz);
    CHECK(why_off && strstr(why_off, "maintenance4"));
    // maintenance4 开启时准确解析常量 / 特化常量默认值 {4, 2, 1}
    CHECK(s2a_spirv_workgroup_size_ex(w, nb, "main", 1, sz) == NULL && sz[0] == 4 && sz[1] == 2 && sz[2] == 1);
    // 非法 ID（越界）在 maintenance4 开启时仍拒收
    exec_mode[3] = 99;
    const char *why_bad = s2a_spirv_workgroup_size_ex(w, nb, "main", 1, sz);
    CHECK(why_bad && strstr(why_bad, "LocalSizeId"));
    printf("case_local_size_id ok\n");
}

static void case_float_controls(void) {
    uint32_t w[128]; struct s2a_spv_decl d;
    uint32_t *p = spv_head(w, 0x10300u, 16);
    p[0] = W32(SpvOpCapability, 2); p[1] = SpvCapabilityShader; p += 2;
    p[0] = W32(SpvOpCapability, 2); p[1] = SpvCapabilitySignedZeroInfNanPreserve; p += 2;
    size_t u = put_str(p + 1, "SPV_KHR_float_controls"); p[0] = W32(SpvOpExtension, (uint32_t)(1 + u)); p += 1 + u;
    p[0] = W32(SpvOpExecutionMode, 4); p[1] = 3; p[2] = SpvExecutionModeSignedZeroInfNanPreserve; p[3] = 32; p += 4;
    p[0] = W32(SpvOpExecutionMode, 4); p[1] = 3; p[2] = SpvExecutionModeRoundingModeRTE; p[3] = 16; p += 4;
    p[0] = W32(SpvOpExecutionMode, 4); p[1] = 3; p[2] = SpvExecutionModeDenormFlushToZero; p[3] = 64; p += 4;
    CHECK(s2a_spv_scan(w, (size_t)(p - w) * 4, &d) == NULL);
    CHECK(d.float_controls_modes == ((1u << 7) | (1u << 9) | (1u << 5)));
    // 非法位宽（24 位）具名拒收
    p[-1] = 24;
    CHECK(s2a_spv_scan(w, (size_t)(p - w) * 4, &d) != NULL);
    printf("case_float_controls ok\n");
}

int main(void) {
    case_caps_table();
    case_scan_positive();
    case_scan_ext_and_unlisted();
    case_push_bytes();
    case_workgroup_bytes();
    case_scalar_block_layout();
    case_ubo_std430_layout();
    case_local_size_id();
    case_float_controls();
    case_malformed();
    printf("t_spv_compliance: all cases pass\n");
    return 0;
}
