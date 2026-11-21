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
    CHECK(s2a_spv_cap_req(SpvCapabilityStorageBuffer16BitAccess) == SPV_REQ_16BIT_STORAGE);
    CHECK(s2a_spv_cap_req(SpvCapabilityDenormPreserve) == SPV_REQ_FLOAT_CONTROLS);
    CHECK(s2a_spv_cap_req(SpvCapabilityShader) == SPV_REQ_CORE);
    CHECK(s2a_spv_cap_req(SpvCapabilityImageQuery) == SPV_REQ_CORE);
    CHECK(s2a_spv_cap_req(SpvCapabilitySampledBuffer) == SPV_REQ_CORE);
    // 表外能力：GroupNonUniformPartitionedNV(5297) / 未知名号 → UNLISTED
    CHECK(s2a_spv_cap_req(5297u) == SPV_REQ_UNLISTED);
    CHECK(s2a_spv_cap_req(9999u) == SPV_REQ_UNLISTED);
    // req 名可回查（日志用），未知名不出指针（拒收行按数值打）
    CHECK(s2a_spv_req_name(SPV_REQ_INT16) && strstr(s2a_spv_req_name(SPV_REQ_INT16), "Int16"));
    // 扩展表
    CHECK(s2a_spv_ext_req("SPV_KHR_shader_draw_parameters") == SPV_REQ_DRAW_PARAMETERS);
    CHECK(s2a_spv_ext_req("SPV_KHR_16bit_storage") == SPV_REQ_16BIT_STORAGE);
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

int main(void) {
    case_caps_table();
    case_scan_positive();
    case_scan_ext_and_unlisted();
    case_push_bytes();
    case_workgroup_bytes();
    case_malformed();
    printf("t_spv_compliance: all cases pass\n");
    return 0;
}
