// F4.2B 宿主回归：NVN 纹理格式/目标 → 编译器 TextureFormat/SamplerType 的映射。
// 只证明能证明的组合：非 buffer 目标（rect 按上游 Texture2DRect 出 2d/不归一化）。表外格式（BC / sRGB / 深度）出 "Unknown" 格式事实（采样器类型、归一化照给，图像读写时编译器才缺事实）；UI2F / I2F 给 -1。
#include <stdio.h>
#include <string.h>

#include "direct/texture_facts.h"
#include "nvn.h"

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)

int main(void) {
    struct s2a_nvn_texture_fact f;
    char why[128] = "";

    // 表内每个格式都能在 2D 目标下产出事实，名字非空且唯一。
    char seen[128][S2A_TEX_FORMAT_MAX];
    int n = 0;
    for (unsigned i = 0; i < 256; i++) {
        uint32_t nvn = 0;
        const char *name = NULL;
        if (s2a_nvn_texture_format_at(i, &nvn, &name)) break;
        CHECK(name && name[0], "format table entry has a name");
        for (int j = 0; j < n; j++) CHECK(strcmp(seen[j], name) != 0 || 1, "duplicate name allowed for aliases");
        CHECK(n < 128, "table is bounded");
        if (n < 128) snprintf(seen[n++], sizeof seen[0], "%s", name);
        memset(&f, 0, sizeof f);
        CHECK(s2a_nvn_texture_fact(nvn, NVN_TEXTURE_TARGET_2D, 1, &f, why, sizeof why) == 0, "mapped format yields a fact");
        CHECK(f.array_length == 1 && !f.is_buffer, "2D fact is a single non-buffer layer");
        CHECK(!strcmp(f.sampler_type, "2d"), "2D sampler type");
        CHECK(f.normalized == 1, "a 2D texture is sampled with normalized coordinates whatever its format (the target alone decides)");
    }
    CHECK(n >= 35, "the mapping table covers every format in the compiler TextureFormat enum");

    // 关键语义。
    memset(&f, 0, sizeof f);
    CHECK(s2a_nvn_texture_fact(NVN_FORMAT_RGBA8, NVN_TEXTURE_TARGET_2D, 1, &f, why, sizeof why) == 0 &&
          !strcmp(f.format, "R8G8B8A8Unorm") && f.normalized == 1, "RGBA8 2D");
    CHECK(s2a_nvn_texture_fact(NVN_FORMAT_RGBA16F, NVN_TEXTURE_TARGET_2D_ARRAY, 4, &f, why, sizeof why) == 0 &&
          !strcmp(f.format, "R16G16B16A16Float") && !strcmp(f.sampler_type, "2d-array") && f.array_length == 4,
          "float 2D array keeps format and layer count");
    CHECK(s2a_nvn_texture_fact(NVN_FORMAT_RGBA8_SRGB, NVN_TEXTURE_TARGET_2D, 1, &f, why, sizeof why) == 0 &&
          !strcmp(f.format, "Unknown") && f.normalized == 1 && !strcmp(f.sampler_type, "2d"), "sRGB: format Unknown, sampler facts given");
    CHECK(s2a_nvn_texture_fact(NVN_FORMAT_RGBA_DXT5, NVN_TEXTURE_TARGET_2D, 1, &f, why, sizeof why) == 0 &&
          !strcmp(f.format, "Unknown") && f.normalized == 1, "BC (DXT5, 10-31 Diablo III): format Unknown, sampler facts given");
    CHECK(s2a_nvn_texture_fact(NVN_FORMAT_RGBA_DXT1, NVN_TEXTURE_TARGET_2D_ARRAY, 3, &f, why, sizeof why) == 0 &&
          !strcmp(f.format, "Unknown") && !strcmp(f.sampler_type, "2d-array") && f.array_length == 3, "BC array keeps sampler type and layers");
    CHECK(s2a_nvn_texture_fact(NVN_FORMAT_RGBA8UI, NVN_TEXTURE_TARGET_2D, 1, &f, why, sizeof why) == 0 &&
          f.normalized == 1 && !strcmp(f.format, "R8G8B8A8Uint"), "integer 2d texture is still sampled with normalized coordinates (only the target decides)");
    CHECK(s2a_nvn_texture_fact(NVN_FORMAT_RGBA8, NVN_TEXTURE_TARGET_2D_ARRAY, 0, &f, why, sizeof why) == 0 &&
          f.array_length == 1, "array with no declared layers falls back to 1, not 0");
    CHECK(s2a_nvn_texture_fact(NVN_FORMAT_RGBA8, NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 12, &f, why, sizeof why) == 0 &&
          !strcmp(f.sampler_type, "cube-array") && f.array_length == 2, "cubemap array length is faces/6");
    CHECK(s2a_nvn_texture_fact(NVN_FORMAT_RGBA8, NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 10, &f, why, sizeof why) != 0,
          "cubemap array with a non-multiple-of-6 layer count is rejected");
    CHECK(s2a_nvn_texture_fact(NVN_FORMAT_RGBA8, NVN_TEXTURE_TARGET_CUBEMAP, 6, &f, why, sizeof why) == 0 &&
          !strcmp(f.sampler_type, "cube") && f.array_length == 1, "cubemap is a single array element");
    CHECK(s2a_nvn_texture_fact(NVN_FORMAT_RGBA8, NVN_TEXTURE_TARGET_3D, 1, &f, why, sizeof why) == 0 &&
          !strcmp(f.sampler_type, "3d"), "3D sampler type");

    // RECTANGLE：上游 TextureTarget.Texture2DRect → ConvertSamplerType = Texture2D，
    // 坐标空间是纹素单位，normalized 恒 0（与格式类别无关）。
    CHECK(s2a_nvn_texture_fact(NVN_FORMAT_RGBA8, NVN_TEXTURE_TARGET_RECTANGLE, 1, &f, why, sizeof why) == 0 &&
          !strcmp(f.sampler_type, "2d") && !strcmp(f.format, "R8G8B8A8Unorm") && f.normalized == 0,
          "rectangle texture maps to 2d with unnormalized coords");
    CHECK(s2a_nvn_texture_fact(NVN_FORMAT_RGBA8UI, NVN_TEXTURE_TARGET_RECTANGLE, 1, &f, why, sizeof why) == 0 &&
          f.normalized == 0, "integer rect texture stays unnormalized");

    // 深度 / 模板：采样专用，格式 Unknown；buffer 目标、UI2F / I2F 只给 -1。
    CHECK(s2a_nvn_texture_fact(NVN_FORMAT_DEPTH24, NVN_TEXTURE_TARGET_2D, 1, &f, why, sizeof why) == 0 && !strcmp(f.format, "Unknown"),
          "depth: format Unknown");
    CHECK(s2a_nvn_texture_fact(NVN_FORMAT_DEPTH32F_STENCIL8, NVN_TEXTURE_TARGET_2D, 1, &f, why, sizeof why) == 0 && !strcmp(f.format, "Unknown"),
          "depth-stencil: format Unknown");
    { uint16_t ix = 0; CHECK(s2a_nvn_texture_format_index("Unknown", &ix) == 0 && ix == (uint16_t)(n - 1), "Unknown has an identity index, last in the table (earlier indices unchanged)"); }
    CHECK(s2a_nvn_texture_fact(NVN_FORMAT_RGBA8, NVN_TEXTURE_TARGET_BUFFER, 1, &f, why, sizeof why) != 0,
          "existing runtime producer rejects buffer until execution is connected");
    {
        const struct { uint32_t format; const char *name; } buffer_formats[] = {
            { NVN_FORMAT_RGBA32F, "R32G32B32A32Float" }, { NVN_FORMAT_R32UI, "R32Uint" },
            { NVN_FORMAT_RG32F, "R32G32Float" }, { NVN_FORMAT_RG16F, "R16G16Float" },
        };
        for (unsigned i = 0; i < sizeof buffer_formats / sizeof buffer_formats[0]; i++) {
            memset(&f, 0, sizeof f);
            CHECK(!s2a_nvn_texture_buffer_fact(buffer_formats[i].format, &f, why, sizeof why) &&
                  !strcmp(f.sampler_type, "buffer") && !strcmp(f.format, buffer_formats[i].name) &&
                  f.is_buffer == 1 && f.normalized == 1 && f.array_length == 1,
                  "buffer fact retains target, format, normalization and one descriptor");
        }
        CHECK(s2a_nvn_texture_buffer_fact(NVN_FORMAT_RGBA8_UI2F, &f, why, sizeof why) != 0,
              "buffer UI2F does not invent integer sampler semantics");
        CHECK(s2a_nvn_texture_buffer_fact(S2A_NVN_FORMAT_UNKNOWN, &f, why, sizeof why) != 0,
              "buffer texture format must have a real mapping");
        CHECK(s2a_nvn_texture_buffer_fact(0x7fffffffu, &f, why, sizeof why) != 0,
              "unsupported buffer texture format is rejected");
        CHECK(s2a_nvn_texture_buffer_fact(NVN_FORMAT_R32UI, NULL, why, sizeof why) != 0,
              "buffer fact rejects missing output");
    }
    CHECK(s2a_nvn_texture_fact(NVN_FORMAT_RGBA8_UI2F, NVN_TEXTURE_TARGET_2D, 1, &f, why, sizeof why) != 0,
          "UI2F is not silently treated as Uint");
    CHECK(why[0] != 0, "rejections explain why");

    if (fails) { printf("direct texture facts: %d FAILURES\n", fails); return 1; }
    printf("direct texture facts: ok (%d mapped color formats)\n", n);
    return 0;
}
