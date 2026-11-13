// F4.3 宿主回归：查找/编译共用的完整规范输入必须覆盖请求生产者的全部内容字段。
// 反例基线：旧生产签名（nvk_request_signature）漏了 constantBufferUse，FS 0→1 时请求不同、
// 签名相同；这里逐字段改一次，编码必须不同（否则就是又一个误命中入口）。
#include <stdio.h>
#include <string.h>

#include "direct/input_identity.h"
#include "direct/sha256.h"

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static void hex(const uint8_t *b, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
    out[2 * n] = 0;
}

int main(void) {
    struct s2a_compile_graphics_src g;
    memset(&g, 0, sizeof g);
    g.rt_count = 2;
    g.topology = "Triangles";
    g.has_tess_mode = 1;
    g.tess_mode = 1;
    g.fragment_output_types[0] = "Float";
    g.fragment_output_types[1] = "Float";
    g.attribute_types[0] = "Float";
    g.attribute_types[1] = "Float";
    g.attribute_types[2] = "Float";
    g.has_alpha_to_coverage = 1;
    g.alpha_to_coverage_enable = 1;
    g.alpha_to_coverage_dither = 1;
    g.has_alpha_test = 1;
    g.alpha_test_compare = "Always";
    g.has_blend = 1;
    g.dual_source_blend = 0;
    g.has_depth_mode = 1;
    g.depth_mode = 1;
    g.has_y_negate = 1;
    g.y_negate = 0;
    g.has_origin = 1;
    g.origin_upper_left = 1;
    g.has_halve_primitive_id = 1;
    g.halve_primitive_id = 0;
    g.has_cb_draw_params = 1;
    g.cb_draw_params = 1;
    g.has_point_size = 1;
    g.program_point_size_enable = 0;
    g.point_size = 1.0f;
    g.has_alpha_test_reference = 1;
    g.alpha_test_reference = 0.5f;
    g.has_unaligned_ssbo = 1;
    g.unaligned_ssbo = 0;
    g.has_geometry_shader = 1;
    g.host_supports_quads = 1;
    g.early_z_force = 0;
    g.viewport_transform_disable = 0;

    struct s2a_input_identity in;
    memset(&in, 0, sizeof in);
    in.direct_abi = 5;
    in.translator_closure = "closure-5";
    for (int i = 0; i < 32; i++) in.profile_sha[i] = (uint8_t)(i * 3 + 1);
    in.stage_mask = 0x3;
    for (int s = 0; s < 2; s++) for (int b = 0; b < 8; b++) in.keys[s][b] = (uint8_t)(0x10 * s + b);
    // F4.3 修订：短 key 只是完整摘要的前 8 字节，身份必须带完整源摘要与真实长度。
    // 反例基线：只改 code、强制同短 key 时，旧编码完全相同（canonical_equal=1）。
    in.source_present = 0x3;
    for (int s = 0; s < 2; s++) {
        for (int b = 0; b < 32; b++) in.source_sha[s][b] = (uint8_t)(0x40 * s + b * 7 + 1);
        in.source_code_bytes[s] = 1024u + (unsigned)s * 512u;
        in.source_control_bytes[s] = 0x880;
    }
    in.ubo_use[0] = 0;      // 反例字段：旧签名漏了它
    in.ubo_use[1] = 0x1f;
    in.texture_array_length = 10256;
    in.sampler_array_length = 1256;
    in.transform_feedback_enabled = 0;
    in.gfx = &g;
    struct s2a_compile_texture_src texts[2];
    memset(texts, 0, sizeof texts);
    texts[0] = (struct s2a_compile_texture_src){ "fs", 8, -1, "2d", "R8G8B8A8Unorm", 0, 1, 0, 1 };
    texts[1] = (struct s2a_compile_texture_src){ "fs", 10, -1, "2d-array", "R16G16B16A16Float", 0, 1, 0, 4 };
    in.textures = texts;
    in.texture_count = 2;

    static uint8_t base[2048], other[2048];
    size_t n = 0, n2 = 0;
    CHECK(s2a_input_identity_encode(&in, base, sizeof base, &n) == 0, "encode base input");
    CHECK(n > 0 && n < sizeof base, "base encoding is bounded");
    CHECK(s2a_input_identity_encode(&in, other, sizeof other, &n2) == 0 && n2 == n && !memcmp(base, other, n),
          "identical inputs encode identically");

    // 已知反例：只有 constantBufferUse 从 0 变 1。
    {
        struct s2a_input_identity m = in;
        m.ubo_use[0] = 1;
        CHECK(s2a_input_identity_encode(&m, other, sizeof other, &n2) == 0 && (n2 != n || memcmp(base, other, n)),
              "constantBufferUse is part of the content identity (old signature collision)");
    }

#define MUTATE(code, what) do {                                                     \
        struct s2a_input_identity m = in;                                           \
        struct s2a_compile_graphics_src mg = g;                                     \
        m.gfx = &mg;                                                                \
        code;                                                                       \
        size_t mn = 0;                                                              \
        int rc = s2a_input_identity_encode(&m, other, sizeof other, &mn);           \
        CHECK(rc == 0 && (mn != n || memcmp(base, other, n)), what);                 \
    } while (0)

    MUTATE(m.ubo_use[5] = 3, "high constantBufferUse slot is covered");
    MUTATE(m.keys[1][3] ^= 0x40, "stage key is covered");
    // 核心反例：同短 key、不同内容。完整摘要/长度任一变化都必须改变身份。
    MUTATE(m.source_sha[0][31] ^= 0x01, "full source digest is covered (same short key collision)");
    MUTATE(m.source_sha[1][0] ^= 0x80, "second stage source digest is covered");
    MUTATE(m.source_code_bytes[0] += 1, "real code length is covered (same concat digest, different split)");
    MUTATE(m.source_control_bytes[1] -= 0x10, "real control length is covered");
    MUTATE(m.stage_mask = 0x1; m.source_present = 0x1, "stage mask is covered (consistent source proof)");
    MUTATE(m.profile_sha[5] ^= 0xff, "profile identity is covered");
    MUTATE(m.translator_closure = "closure-6", "translator closure is covered");
    MUTATE(m.direct_abi = 6, "direct ABI is covered");
    MUTATE(m.texture_array_length = 1, "texture pool capacity is covered");
    MUTATE(m.sampler_array_length = 1, "sampler pool capacity is covered");
    MUTATE(m.transform_feedback_enabled = 1, "transform feedback is covered");
    MUTATE(mg.rt_count = 3, "color target count is covered");
    MUTATE(mg.topology = "Lines", "topology is covered");
    MUTATE(mg.topology = NULL, "absent topology differs from a name");
    MUTATE(mg.has_tess_mode = 0, "tess mode presence is covered");
    MUTATE(mg.tess_mode = 0, "tess mode value is covered");
    MUTATE(mg.fragment_output_types[2] = "Uint", "fragment output format is covered");
    MUTATE(mg.fragment_output_types[1] = NULL, "unbound fragment output differs from a format");
    MUTATE(mg.attribute_types[3] = "Sint", "vertex attribute format is covered");
    MUTATE(mg.attribute_types[31] = "Float", "high vertex attribute slot is covered");
    MUTATE(mg.constant_buffer_use_overflow = 1, "constantBufferUse overflow flag is covered");
    MUTATE(mg.has_alpha_to_coverage = 0, "alpha-to-coverage presence is covered");
    MUTATE(mg.alpha_to_coverage_enable = 0, "alpha-to-coverage enable is covered");
    MUTATE(mg.alpha_to_coverage_dither = 0, "alpha-to-coverage dither is covered");
    MUTATE(mg.has_alpha_test = 0, "alpha test presence is covered");
    MUTATE(mg.alpha_test_compare = "Never", "alpha test compare is covered");
    MUTATE(mg.has_blend = 0, "blend presence is covered");
    MUTATE(mg.dual_source_blend = 1, "dual-source blend is covered");
    MUTATE(mg.has_depth_mode = 0, "depth mode presence is covered");
    MUTATE(mg.depth_mode = 0, "depth mode value is covered");
    MUTATE(mg.has_y_negate = 0, "y negate presence is covered");
    MUTATE(mg.y_negate = 1, "y negate value is covered");
    MUTATE(mg.has_origin = 0, "origin presence is covered");
    MUTATE(mg.origin_upper_left = 0, "origin value is covered");
    MUTATE(mg.has_halve_primitive_id = 0, "halve primitive id presence is covered");
    MUTATE(mg.halve_primitive_id = 1, "halve primitive id value is covered");
    MUTATE(mg.has_cb_draw_params = 0, "cb draw params presence is covered");
    MUTATE(mg.cb_draw_params = 0, "cb draw params value is covered");
    MUTATE(mg.has_point_size = 0, "point size presence is covered");
    MUTATE(mg.program_point_size_enable = 1, "program point size is covered");
    MUTATE(mg.point_size = 2.0f, "point size value is covered (bit pattern)");
    MUTATE(mg.has_alpha_test_reference = 0, "alpha test reference presence is covered");
    MUTATE(mg.alpha_test_reference = 0.25f, "alpha test reference value is covered");
    MUTATE(mg.has_unaligned_ssbo = 0, "unaligned SSBO presence is covered");
    MUTATE(mg.unaligned_ssbo = 1, "unaligned SSBO value is covered");
    MUTATE(mg.has_geometry_shader = 0, "geometry shader presence is covered");
    MUTATE(mg.host_supports_quads = 0, "quad support is covered");
    MUTATE(mg.early_z_force = 1, "early-Z policy is covered");
    MUTATE(mg.viewport_transform_disable = 1, "viewport transform policy is covered");
#undef MUTATE

    // 纹理事实（F4.2B）也是内容身份的一部分：任一字段变化都必须改变身份。
#define MUTATE_TEX(code, what) do {                                                 \
        struct s2a_input_identity m = in;                                           \
        struct s2a_compile_graphics_src mg = g;                                     \
        struct s2a_compile_texture_src mt[2] = { texts[0], texts[1] };              \
        m.gfx = &mg; m.textures = mt;                                               \
        code;                                                                       \
        size_t mn = 0;                                                              \
        int rc = s2a_input_identity_encode(&m, other, sizeof other, &mn);           \
        CHECK(rc == 0 && (mn != n || memcmp(base, other, n)), what);                 \
    } while (0)
    MUTATE_TEX(mt[0].format = "R16G16B16A16Float", "texture format is covered");
    MUTATE_TEX(mt[0].sampler_type = "cube", "texture sampler type is covered");
    MUTATE_TEX(mt[0].handle = 12, "texture handle is covered");
    MUTATE_TEX(mt[0].cbuf_slot = 3, "texture cbuf slot is covered");
    MUTATE_TEX(mt[0].is_buffer = 1, "texture isBuffer is covered");
    MUTATE_TEX(mt[0].normalized = 0, "texture normalized is covered");
    MUTATE_TEX(mt[0].separate = 1, "texture separate is covered");
    MUTATE_TEX(mt[1].array_length = 8, "texture array length is covered");
    MUTATE_TEX(mt[1].stage = "vs", "texture stage is covered");
    MUTATE_TEX(m.texture_count = 1, "texture count is covered");
#undef MUTATE_TEX

    {
        struct s2a_input_identity m = in;
        m.textures = NULL;
        size_t mn = 0;
        CHECK(s2a_input_identity_encode(&m, other, sizeof other, &mn) != 0, "texture facts cannot be silently dropped");
    }

    // 缺完整源证明（少一个阶段 / 多置位 / 长度为 0 / 没有阶段）一律编码失败：
    // 不能拿短 key 冒充“内容相同”，也不能让缺项悄悄退化成零摘要。
    {
        struct s2a_input_identity m = in;
        size_t mn = 0;
        m.source_present = 0x1;
        CHECK(s2a_input_identity_encode(&m, other, sizeof other, &mn) != 0,
              "missing source proof for a bound stage is rejected");
        m = in; m.source_present = 0x7;
        CHECK(s2a_input_identity_encode(&m, other, sizeof other, &mn) != 0,
              "extra source bits are rejected");
        m = in; m.source_code_bytes[1] = 0;
        CHECK(s2a_input_identity_encode(&m, other, sizeof other, &mn) != 0,
              "zero code length is rejected");
        m = in; m.source_control_bytes[0] = 0;
        CHECK(s2a_input_identity_encode(&m, other, sizeof other, &mn) != 0,
              "zero control length is rejected");
        m = in; m.stage_mask = 0;
        CHECK(s2a_input_identity_encode(&m, other, sizeof other, &mn) != 0,
              "empty stage mask is rejected");
        m = in; m.stage_mask = 0x40;
        CHECK(s2a_input_identity_encode(&m, other, sizeof other, &mn) != 0,
              "stage mask beyond the six NVN stages is rejected");
        // 编码版本字节：v2 → v3 的修改必须改变身份（旧磁盘条目/旧内存身份不能互认）。
        static const uint8_t v3_magic[] = { 'S', '2', 'A', 'I', '1', 3 };
        CHECK(n >= sizeof v3_magic && !memcmp(base, v3_magic, sizeof v3_magic),
              "identity carries the S2AI1 version 3 marker");
    }

    // constantBufferUse 加宽（compute 的用户 uniform 槽 0..15 = NVN cbuf 3..18）：
    // 旧 5 位窗口（cbuf 0..4）的输入逐字节仍是 v3；只有出现高位 (cbuf 5..18) 的 compute 输入才用 v4。
    // 图形路径不参与加宽：同一图形输入即使掩码带高位也保持 v3（图形填表仍只产生旧窗口内的位）。
    {
        static const uint8_t v3_magic[] = { 'S', '2', 'A', 'I', '1', 3 };
        static const uint8_t v4_magic[] = { 'S', '2', 'A', 'I', '1', 4 };
        struct s2a_input_identity c;
        memset(&c, 0, sizeof c);
        c.direct_abi = 5;
        c.translator_closure = "closure-5";
        for (int i = 0; i < 32; i++) c.profile_sha[i] = (uint8_t)(i * 3 + 1);
        c.stage_mask = 0x20;   // compute
        for (int b = 0; b < 8; b++) c.keys[0][b] = (uint8_t)(b + 1);
        c.source_present = 0x1;
        for (int b = 0; b < 32; b++) c.source_sha[0][b] = (uint8_t)(b * 5 + 3);
        c.source_code_bytes[0] = 2048;
        c.source_control_bytes[0] = 0x760;
        c.gfx = &g;
        c.transform_feedback_enabled = 0;
        c.compute_has_unaligned_ssbo = 1;   // compute 身份必须显式给对齐事实
        c.compute_unaligned_ssbo = 0;
        c.ubo_use[5] = 0x18;   // cbuf 3,4（旧窗口内）
        size_t cn = 0;
        uint8_t cbase[4096], cwide[4096], cwide2[4096];
        CHECK(s2a_input_identity_encode(&c, cbase, sizeof cbase, &cn) == 0, "compute identity encodes");
        CHECK(cn >= sizeof v3_magic && !memcmp(cbase, v3_magic, sizeof v3_magic),
              "compute identity inside the old 5-bit window stays v3");
        size_t n2 = 0, n3 = 0;
        c.ubo_use[5] = 0x38;   // cbuf 3,4,5：出现旧窗口外的位
        CHECK(s2a_input_identity_encode(&c, cwide, sizeof cwide, &n2) == 0, "wide compute identity encodes");
        CHECK(n2 >= sizeof v4_magic && !memcmp(cwide, v4_magic, sizeof v4_magic),
              "wide cbuf mask uses the v4 identity marker");
        CHECK(!(n2 == cn && !memcmp(cbase, cwide, cn)), "wide mask cannot share the old identity bytes");
        CHECK(s2a_input_identity_encode(&c, cwide2, sizeof cwide2, &n3) == 0 && n3 == n2 &&
              !memcmp(cwide, cwide2, n2), "same wide input encodes byte-identically twice");
        // 图形填表也扩到完整掩码（P0-3）：图形出现高位同样必须落 v4，否则旧缓存里截断掩码的
        // 程序会和新程序混判；窗口内（cbuf 0..4）仍逐字节 v3。
        struct s2a_input_identity gfx = in;
        gfx.ubo_use[0] = 0x1f;
        size_t gn = 0;
        CHECK(s2a_input_identity_encode(&gfx, cwide, sizeof cwide, &gn) == 0 &&
              gn >= sizeof v3_magic && !memcmp(cwide, v3_magic, sizeof v3_magic),
              "graphics identity inside the old 5-bit window stays v3");
        gfx.ubo_use[0] = 0x38;   // 图形高位（cbuf 3,4,5）
        size_t gwn = 0;
        uint8_t gwbuf[4096];
        CHECK(s2a_input_identity_encode(&gfx, gwbuf, sizeof gwbuf, &gwn) == 0 &&
              gwn >= sizeof v4_magic && !memcmp(gwbuf, v4_magic, sizeof v4_magic),
              "graphics identity version-bumps to v4 when a high cbuf bit is set");
        CHECK(!(gwn == gn && !memcmp(cwide, gwbuf, gn)), "graphics high cbuf mask cannot share the v3 identity");
    }

    // F4.3/A2：名字不在已映射表里时必须明确失败。以前用 wstr 编名字，未知名字照样能编码，
    // 于是“表里没有的格式”和“表里有的格式”会得到不同身份——但两份请求里该条根本不存在，
    // 差异没有意义；更危险的是溢出后退回 key-only 阶段 key 比较，会让不同内容同一身份。
    {
        struct s2a_input_identity m = in;
        struct s2a_compile_texture_src mt[2] = { texts[0], texts[1] };
        m.textures = mt;
        mt[0].format = "NotACompilerFormat";
        CHECK(s2a_input_identity_encode(&m, other, sizeof other, &n2) != 0,
              "unknown texture format is rejected, not encoded");
        mt[0].format = texts[0].format;
        mt[0].sampler_type = "not-a-target";
        CHECK(s2a_input_identity_encode(&m, other, sizeof other, &n2) != 0,
              "unknown sampler type is rejected, not encoded");
        mt[0].sampler_type = texts[0].sampler_type;
        mt[0].stage = NULL;
        CHECK(s2a_input_identity_encode(&m, other, sizeof other, &n2) != 0,
              "missing texture stage differs from vs and is rejected");
    }

    // A2 容量：最大数量的纹理事实 + 满额 attribute/fragment 名字必须仍能编码。
    // 这是 S2A_INPUT_IDENTITY_MAX 存在的理由；编不下就必须失败，不能靠降级掩盖。
    {
        static struct s2a_compile_texture_src many[S2A_COMPILE_TEXTURE_MAX];
        static struct s2a_compile_graphics_src bg;
        struct s2a_input_identity m = in;
        bg = g;
        for (int i = 0; i < 32; i++) bg.attribute_types[i] = "R16G16B16A16Unorm";   // 18 字节
        for (int i = 0; i < 8; i++) bg.fragment_output_types[i] = "R16G16B16A16Unorm";
        m.gfx = &bg;
        for (int i = 0; i < S2A_COMPILE_TEXTURE_MAX; i++) {
            many[i] = texts[i & 1];
            many[i].stage = (i & 1) ? "fs" : "vs";
            many[i].handle = (uint32_t)(8 + 2 * (i % 32));
            many[i].sampler_type = "2d-ms-array";
            many[i].format = "R16G16B16A16Unorm";
            many[i].array_length = 2048;
        }
        m.textures = many;
        m.texture_count = S2A_COMPILE_TEXTURE_MAX;
        uint8_t big[S2A_INPUT_IDENTITY_MAX];
        size_t bn = 0;
        CHECK(s2a_input_identity_encode(&m, big, sizeof big, &bn) == 0,
              "maximum texture facts + full attribute names fit the identity cap");
        CHECK(bn < S2A_INPUT_IDENTITY_MAX, "worst-case identity leaves headroom");
        if (bn >= S2A_INPUT_IDENTITY_MAX)
            printf("  identity worst case = %zu B (cap %d)\n", bn, S2A_INPUT_IDENTITY_MAX);
    }

    // 容量不足：准确失败，不截断成“看似相同”的输入。
    CHECK(s2a_input_identity_encode(&in, other, 64, &n2) != 0, "small buffer is rejected");
    CHECK(s2a_input_identity_encode(NULL, other, sizeof other, &n2) != 0, "missing input is rejected");

    // 与查找热路径相同的摘要：完整 32 字节 SHA-256（不是 64 位短签名）。
    uint8_t digest[32];
    s2a_sha256 ctx;
    s2a_sha256_init(&ctx);
    s2a_sha256_update(&ctx, base, n);
    s2a_sha256_final(&ctx, digest);
    char dh[65];
    hex(digest, 32, dh);
    CHECK(strlen(dh) == 64, "content identity is a full SHA-256");
    if (!fails) printf("direct input identity: ok (%zu B encoded, ubo_use covered, digest=%s)\n", n, dh);
    else printf("direct input identity: %d FAILURES\n", fails);
    return fails ? 1 : 0;
}
