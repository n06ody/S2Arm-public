// 格式映射自测：已知样例 + 未知格式必须 UNDEFINED（不猜）。
#include <stdio.h>
#include <string.h>
#include "nvn_vk_format.h"
#include "include/nvn.h"

int main(void) {
    struct { uint32_t nvn; VkFormat vk; const char *name; } cases[] = {
        { NVN_FORMAT_RGBA8, VK_FORMAT_R8G8B8A8_UNORM, "RGBA8" },
        { NVN_FORMAT_RGBA8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, "RGBA8_SRGB" },
        { NVN_FORMAT_RGB565, VK_FORMAT_B5G6R5_UNORM_PACK16, "RGB565" },   // #975：NVN 低位起，r 在 0–4 位
        { NVN_FORMAT_DEPTH24_STENCIL8, VK_FORMAT_D24_UNORM_S8_UINT, "D24S8" },
        { NVN_FORMAT_RGBA16F, VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F" },
        { NVN_FORMAT_RGB_DXT1, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "DXT1" },
        { NVN_FORMAT_BPTC_UNORM, VK_FORMAT_BC7_UNORM_BLOCK, "BPTC" },
        // ASTC 全 14 种块尺寸 × UNORM / SRGB（0x79–0x94）。Wonder 首绘撞到 0x87（4x4 SRGB）。
        { NVN_FORMAT_RGBA_ASTC_4x4, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC4x4" },
        { NVN_FORMAT_RGBA_ASTC_5x4, VK_FORMAT_ASTC_5x4_UNORM_BLOCK, "ASTC5x4" },
        { NVN_FORMAT_RGBA_ASTC_5x5, VK_FORMAT_ASTC_5x5_UNORM_BLOCK, "ASTC5x5" },
        { NVN_FORMAT_RGBA_ASTC_6x5, VK_FORMAT_ASTC_6x5_UNORM_BLOCK, "ASTC6x5" },
        { NVN_FORMAT_RGBA_ASTC_6x6, VK_FORMAT_ASTC_6x6_UNORM_BLOCK, "ASTC6x6" },
        { NVN_FORMAT_RGBA_ASTC_8x5, VK_FORMAT_ASTC_8x5_UNORM_BLOCK, "ASTC8x5" },
        { NVN_FORMAT_RGBA_ASTC_8x6, VK_FORMAT_ASTC_8x6_UNORM_BLOCK, "ASTC8x6" },
        { NVN_FORMAT_RGBA_ASTC_8x8, VK_FORMAT_ASTC_8x8_UNORM_BLOCK, "ASTC8x8" },
        { NVN_FORMAT_RGBA_ASTC_10x5, VK_FORMAT_ASTC_10x5_UNORM_BLOCK, "ASTC10x5" },
        { NVN_FORMAT_RGBA_ASTC_10x6, VK_FORMAT_ASTC_10x6_UNORM_BLOCK, "ASTC10x6" },
        { NVN_FORMAT_RGBA_ASTC_10x8, VK_FORMAT_ASTC_10x8_UNORM_BLOCK, "ASTC10x8" },
        { NVN_FORMAT_RGBA_ASTC_10x10, VK_FORMAT_ASTC_10x10_UNORM_BLOCK, "ASTC10x10" },
        { NVN_FORMAT_RGBA_ASTC_12x10, VK_FORMAT_ASTC_12x10_UNORM_BLOCK, "ASTC12x10" },
        { NVN_FORMAT_RGBA_ASTC_12x12, VK_FORMAT_ASTC_12x12_UNORM_BLOCK, "ASTC12x12" },
        { NVN_FORMAT_RGBA_ASTC_4x4_SRGB, VK_FORMAT_ASTC_4x4_SRGB_BLOCK, "ASTC4x4_SRGB" },
        { NVN_FORMAT_RGBA_ASTC_5x4_SRGB, VK_FORMAT_ASTC_5x4_SRGB_BLOCK, "ASTC5x4_SRGB" },
        { NVN_FORMAT_RGBA_ASTC_5x5_SRGB, VK_FORMAT_ASTC_5x5_SRGB_BLOCK, "ASTC5x5_SRGB" },
        { NVN_FORMAT_RGBA_ASTC_6x5_SRGB, VK_FORMAT_ASTC_6x5_SRGB_BLOCK, "ASTC6x5_SRGB" },
        { NVN_FORMAT_RGBA_ASTC_6x6_SRGB, VK_FORMAT_ASTC_6x6_SRGB_BLOCK, "ASTC6x6_SRGB" },
        { NVN_FORMAT_RGBA_ASTC_8x5_SRGB, VK_FORMAT_ASTC_8x5_SRGB_BLOCK, "ASTC8x5_SRGB" },
        { NVN_FORMAT_RGBA_ASTC_8x6_SRGB, VK_FORMAT_ASTC_8x6_SRGB_BLOCK, "ASTC8x6_SRGB" },
        { NVN_FORMAT_RGBA_ASTC_8x8_SRGB, VK_FORMAT_ASTC_8x8_SRGB_BLOCK, "ASTC8x8_SRGB" },
        { NVN_FORMAT_RGBA_ASTC_10x5_SRGB, VK_FORMAT_ASTC_10x5_SRGB_BLOCK, "ASTC10x5_SRGB" },
        { NVN_FORMAT_RGBA_ASTC_10x6_SRGB, VK_FORMAT_ASTC_10x6_SRGB_BLOCK, "ASTC10x6_SRGB" },
        { NVN_FORMAT_RGBA_ASTC_10x8_SRGB, VK_FORMAT_ASTC_10x8_SRGB_BLOCK, "ASTC10x8_SRGB" },
        { NVN_FORMAT_RGBA_ASTC_10x10_SRGB, VK_FORMAT_ASTC_10x10_SRGB_BLOCK, "ASTC10x10_SRGB" },
        { NVN_FORMAT_RGBA_ASTC_12x10_SRGB, VK_FORMAT_ASTC_12x10_SRGB_BLOCK, "ASTC12x10_SRGB" },
        { NVN_FORMAT_RGBA_ASTC_12x12_SRGB, VK_FORMAT_ASTC_12x12_SRGB_BLOCK, "ASTC12x12_SRGB" },
        // 顶点属性常用的打包格式：10/10/10/2 的 SNORM/SINT（法线/切线），NVN 低位往上 r,g,b,a。
        { NVN_FORMAT_RGB10A2SN, VK_FORMAT_A2B10G10R10_SNORM_PACK32, "RGB10A2SN" },
        { NVN_FORMAT_RGB10A2I, VK_FORMAT_A2B10G10R10_SINT_PACK32, "RGB10A2I" },
        // X（未定义第四通道）族：Vulkan 没有 RGBX 位宽格式，落到同宽 RGBA（X 位不可依赖）。
        { NVN_FORMAT_RGBX8, VK_FORMAT_R8G8B8A8_UNORM, "RGBX8" },
        { NVN_FORMAT_RGBX8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, "RGBX8_SRGB" },
        { NVN_FORMAT_RGBX16F, VK_FORMAT_R16G16B16A16_SFLOAT, "RGBX16F" },
        { NVN_FORMAT_RGBX32F, VK_FORMAT_R32G32B32A32_SFLOAT, "RGBX32F" },
        { NVN_FORMAT_RGBX8UI, VK_FORMAT_R8G8B8A8_UINT, "RGBX8UI" },
        // BGR 族：NVN 低位往上 r,g,b,a，与 Vulkan 的 BGR 打包一一对应。
        { NVN_FORMAT_BGR565, VK_FORMAT_R5G6B5_UNORM_PACK16, "BGR565" },   // #975：b 在 0–4 位、r 在 11–15 位
        { NVN_FORMAT_BGRX8, VK_FORMAT_B8G8R8A8_UNORM, "BGRX8" },
        { NVN_FORMAT_BGRA8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, "BGRA8_SRGB" },
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        VkFormat got = nvn_vk_format(cases[i].nvn);
        printf("%-14s 0x%02x -> %d (want %d) %s\n", cases[i].name, cases[i].nvn, (int)got, (int)cases[i].vk, got == cases[i].vk ? "ok" : "FAIL");
        if (got != cases[i].vk) bad++;
    }
    // 压缩块表必须同步收 SRGB 项；真实上传走 nvn_fmt_bpb（带块宽高），不靠这张布尔表算尺寸。
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (cases[i].nvn < NVN_FORMAT_RGBA_ASTC_4x4 || cases[i].nvn > NVN_FORMAT_RGBA_ASTC_12x12_SRGB) continue;
        int got = nvn_vk_format_is_compressed(cases[i].vk);
        printf("compressed %-14s -> %d %s\n", cases[i].name, got, got == 1 ? "ok" : "FAIL");
        if (got != 1) bad++;
    }
    if (nvn_vk_format_is_compressed(VK_FORMAT_R8G8B8A8_UNORM) != 0) {
        printf("compressed RGBA8 -> 1 FAIL\n");
        bad++;
    }
    // 压缩块总字节必须按格式自己的块宽高算：非 4x4 ASTC 在修前一律按 4x4 会多算。
    struct { uint32_t nvn, w, h; unsigned long long want; const char *name; } blocks[] = {
        { NVN_FORMAT_RGBA_ASTC_4x4, 8, 8, 64, "ASTC4x4 8x8" },
        { NVN_FORMAT_RGBA_ASTC_5x5, 10, 10, 64, "ASTC5x5 10x10" },
        { NVN_FORMAT_RGBA_ASTC_5x5_SRGB, 10, 10, 64, "ASTC5x5_SRGB 10x10" },
        { NVN_FORMAT_RGBA_ASTC_8x8, 16, 16, 64, "ASTC8x8 16x16" },
        { NVN_FORMAT_RGBA_ASTC_8x8_SRGB, 16, 16, 64, "ASTC8x8_SRGB 16x16" },
        { NVN_FORMAT_RGBA_ASTC_12x12, 24, 24, 64, "ASTC12x12 24x24" },
        { NVN_FORMAT_RGBA_ASTC_12x12_SRGB, 24, 24, 64, "ASTC12x12_SRGB 24x24" },
        { NVN_FORMAT_RGBA_ASTC_5x5, 1, 1, 16, "ASTC5x5 1x1" },
        { NVN_FORMAT_RGBA_ASTC_8x8, 17, 9, 96, "ASTC8x8 17x9" },
        { NVN_FORMAT_BPTC_UNORM, 8, 8, 64, "BPTC 8x8" },
    };
    for (unsigned i = 0; i < sizeof blocks / sizeof blocks[0]; i++) {
        unsigned long long got = (unsigned long long)nvn_vk_format_compressed_image_bytes(
            blocks[i].nvn, blocks[i].w, blocks[i].h);
        printf("block-bytes %-18s %ux%u -> %llu (want %llu) %s\n", blocks[i].name,
               blocks[i].w, blocks[i].h, got, blocks[i].want, got == blocks[i].want ? "ok" : "FAIL");
        if (got != blocks[i].want) bad++;
    }
    if (nvn_vk_format_compressed_image_bytes(NVN_FORMAT_RGBA8, 8, 8) != 0) {
        printf("block-bytes RGBA8 (uncompressed) -> nonzero FAIL\n");
        bad++;
    }
    if (nvn_vk_format_compressed_image_bytes(0xDEADu, 8, 8) != 0) {
        printf("block-bytes unknown -> nonzero FAIL\n");
        bad++;
    }
    // #398：设备缺 D24 深度附件（三星 Xclipse 只有 D16S8 / D32S8）时换 D32；有 D24 时一律原样。
    {
        const VkFormatFeatureFlags att = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        struct { VkFormatFeatureFlags d24s8, d32s8, x8d24, d32; uint32_t want; const char *name; } pick[] = {
            { att, att, att, att, 0, "both D24 present" },
            { 0, att, 0, att, NVN_VK_DEPTH_SUBST_D24S8 | NVN_VK_DEPTH_SUBST_X8D24, "Xclipse: no D24" },
            { VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, att, att, att, NVN_VK_DEPTH_SUBST_D24S8, "D24S8 sample-only" },
            { 0, 0, 0, att, NVN_VK_DEPTH_SUBST_X8D24, "no D24S8 / D32S8: keep D24S8" },
        };
        for (unsigned i = 0; i < sizeof pick / sizeof pick[0]; i++) {
            uint32_t got = nvn_vk_depth_subst_pick(pick[i].d24s8, pick[i].d32s8, pick[i].x8d24, pick[i].d32);
            printf("depth-subst %-30s -> %u (want %u) %s\n", pick[i].name, got, pick[i].want, got == pick[i].want ? "ok" : "FAIL");
            if (got != pick[i].want) bad++;
        }
        nvn_vk_format_set_depth_subst(NVN_VK_DEPTH_SUBST_D24S8 | NVN_VK_DEPTH_SUBST_X8D24);
        VkFormat ds = nvn_vk_format(NVN_FORMAT_DEPTH24_STENCIL8), dd = nvn_vk_format(NVN_FORMAT_DEPTH24);
        VkFormat dt = nvn_vk_tex_format(NVN_FORMAT_DEPTH24_STENCIL8), d16 = nvn_vk_format(NVN_FORMAT_DEPTH16);
        int sub = nvn_vk_format_substituted(NVN_FORMAT_DEPTH24_STENCIL8) && nvn_vk_format_substituted(NVN_FORMAT_DEPTH24) &&
                  !nvn_vk_format_substituted(NVN_FORMAT_DEPTH32F_STENCIL8) && !nvn_vk_format_substituted(NVN_FORMAT_DEPTH16);
        int okm = ds == VK_FORMAT_D32_SFLOAT_S8_UINT && dd == VK_FORMAT_D32_SFLOAT && dt == VK_FORMAT_D32_SFLOAT_S8_UINT &&
                  d16 == VK_FORMAT_D16_UNORM && sub;
        printf("depth-subst on: D24S8 -> %d, D24 -> %d, tex D24S8 -> %d, D16 -> %d, substituted flags %d %s\n",
               (int)ds, (int)dd, (int)dt, (int)d16, sub, okm ? "ok" : "FAIL");
        if (!okm) bad++;
        nvn_vk_format_set_depth_subst(0);
        okm = nvn_vk_format(NVN_FORMAT_DEPTH24_STENCIL8) == VK_FORMAT_D24_UNORM_S8_UINT &&
              nvn_vk_format(NVN_FORMAT_DEPTH24) == VK_FORMAT_X8_D24_UNORM_PACK32 &&
              !nvn_vk_format_substituted(NVN_FORMAT_DEPTH24_STENCIL8);
        printf("depth-subst off: back to D24 %s\n", okm ? "ok" : "FAIL");
        if (!okm) bad++;
        // 深度面换算：D24（低 24 位 UNORM，高 8 位不参与）→ float → D24 往返不丢值；越界 / NaN 夹到 [0,1]。
        uint32_t t[5] = { 0x00000000u, 0x00ffffffu, 0xab800000u, 0x00123456u, 0x7f000001u };
        const uint32_t back[5] = { 0x00000000u, 0x00ffffffu, 0x00800000u, 0x00123456u, 0x00000001u };
        nvn_vk_depth24_to_f32(t, 5);
        float f[5]; memcpy(f, t, sizeof f);
        int okc = f[0] == 0.0f && f[1] == 1.0f && f[2] > 0.5f && f[2] < 0.5001f;
        nvn_vk_depth_f32_to_24(t, 5);
        for (int i = 0; i < 5; i++) okc = okc && t[i] == back[i];
        float edge[3] = { -0.5f, 2.0f, 0.0f }; edge[2] = edge[2] / edge[2];   // NaN
        nvn_vk_depth_f32_to_24(edge, 3);
        uint32_t eu[3]; memcpy(eu, edge, sizeof eu);
        okc = okc && eu[0] == 0 && eu[1] == 0x00ffffffu && eu[2] == 0;
        printf("depth24 <-> f32 round trip %s\n", okc ? "ok" : "FAIL");
        if (!okc) bad++;
    }
    VkFormat unk = nvn_vk_format(0xDEADu);
    printf("unknown 0xDEAD -> %d (want %d) %s\n", (int)unk, (int)VK_FORMAT_UNDEFINED, unk == VK_FORMAT_UNDEFINED ? "ok" : "FAIL");
    if (unk != VK_FORMAT_UNDEFINED) bad++;
    // 同格式视图不是覆盖：Vulkan 只要求不同格式时同兼容类。Wonder 在 BC4 纹理上设同格式+swizzle 视图曾被误拒。
    char why[192] = "";
    VkFormat out = VK_FORMAT_UNDEFINED;
    int ok = nvn_vk_format_override_ok(VK_FORMAT_BC4_UNORM_BLOCK, NVN_FORMAT_RGTC1_UNORM, &out, why, sizeof why);
    printf("same-format BC4 view -> rc %d out %d %s\n", ok, (int)out, (ok == 0 && out == VK_FORMAT_BC4_UNORM_BLOCK) ? "ok" : "FAIL");
    if (ok != 0 || out != VK_FORMAT_BC4_UNORM_BLOCK) bad++;
    why[0] = 0; out = VK_FORMAT_UNDEFINED;
    ok = nvn_vk_format_override_ok(VK_FORMAT_BC4_UNORM_BLOCK, NVN_FORMAT_RGTC2_UNORM, &out, why, sizeof why);
    printf("cross-format BC4/BC5 view -> rc %d (%s) %s\n", ok, why, ok != 0 ? "ok" : "FAIL");
    if (ok == 0) bad++;
    // 解码后的 BC 纹理取「同 NVN 格式」视图：图像是解码后格式（BC4 → R8_UNORM），视图必须
    // 沿用图像格式；拿 BC4（139）去和 R8_UNORM（9）比兼容类会误拒。同一修法覆盖全部 compute
    // 解码族（BC1–BC7 的 UNORM/SNORM/SRGB）；ASTC 在本仓库没有 compute 软解路径（原生支持），
    // 但“同 NVN 格式视图恒合法”对压缩格式同样成立，一并钉住。
    struct { uint32_t nvn; VkFormat base; const char *name; } same_fmt[] = {
        { NVN_FORMAT_RGB_DXT1_SRGB, VK_FORMAT_R8G8B8A8_SRGB, "BC1 SRGB" },
        { NVN_FORMAT_RGBA_DXT5_SRGB, VK_FORMAT_R8G8B8A8_SRGB, "BC3 SRGB" },
        { NVN_FORMAT_RGTC1_UNORM, VK_FORMAT_R8_UNORM, "BC4 UNORM" },
        { NVN_FORMAT_RGTC1_SNORM, VK_FORMAT_R8_SNORM, "BC4 SNORM" },
        { NVN_FORMAT_RGTC2_UNORM, VK_FORMAT_R8G8_UNORM, "BC5 UNORM" },
        { NVN_FORMAT_RGTC2_SNORM, VK_FORMAT_R8G8_SNORM, "BC5 SNORM" },
        { NVN_FORMAT_BPTC_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, "BC6H SFLOAT" },
        { NVN_FORMAT_BPTC_UFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, "BC6H UFLOAT" },
        { NVN_FORMAT_BPTC_UNORM, VK_FORMAT_R8G8B8A8_UNORM, "BC7 UNORM" },
        { NVN_FORMAT_BPTC_UNORM_SRGB, VK_FORMAT_R8G8B8A8_SRGB, "BC7 SRGB" },
        { NVN_FORMAT_RGBA_ASTC_4x4, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC4x4" },
        { NVN_FORMAT_RGBA_ASTC_8x8_SRGB, VK_FORMAT_ASTC_8x8_SRGB_BLOCK, "ASTC8x8 SRGB" },
    };
    for (unsigned i = 0; i < sizeof same_fmt / sizeof same_fmt[0]; i++) {
        why[0] = 0; out = VK_FORMAT_UNDEFINED;
        int rc = nvn_vk_format_texview_ok(same_fmt[i].base, same_fmt[i].nvn, same_fmt[i].nvn,
                                          &out, why, sizeof why);
        printf("decoded same-NVN %-12s -> rc %d out %d %s\n", same_fmt[i].name, rc, (int)out,
               (rc == 0 && out == same_fmt[i].base) ? "ok" : "FAIL");
        if (rc != 0 || out != same_fmt[i].base) bad++;
    }
    // 异格式仍拒：同族不同存储格式（如 BC4 SNORM）、跨族压缩格式、不同块尺寸的 ASTC。
    struct { uint32_t nvn; VkFormat base; uint32_t view; const char *name; } cross_fmt[] = {
        { NVN_FORMAT_RGTC1_UNORM, VK_FORMAT_R8_UNORM, NVN_FORMAT_RGTC1_SNORM, "BC4 UNORM vs 0x4a" },
        { NVN_FORMAT_RGTC1_UNORM, VK_FORMAT_R8_UNORM, NVN_FORMAT_RGTC2_UNORM, "BC4 UNORM vs 0x4b" },
        { NVN_FORMAT_RGTC2_UNORM, VK_FORMAT_R8G8_UNORM, NVN_FORMAT_RGTC1_UNORM, "BC5 UNORM vs 0x49" },
        { NVN_FORMAT_BPTC_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, NVN_FORMAT_BPTC_UNORM, "BC6H vs 0x4d" },
        { NVN_FORMAT_BPTC_UNORM, VK_FORMAT_R8G8B8A8_UNORM, NVN_FORMAT_RGTC2_UNORM, "BC7 vs 0x4b" },
        { NVN_FORMAT_BPTC_UNORM_SRGB, VK_FORMAT_R8G8B8A8_SRGB, NVN_FORMAT_RGBA16F, "BC7 SRGB vs RGBA16F" },
        { NVN_FORMAT_RGBA_ASTC_4x4, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, NVN_FORMAT_RGBA_ASTC_8x8, "ASTC4x4 vs 8x8" },
    };
    for (unsigned i = 0; i < sizeof cross_fmt / sizeof cross_fmt[0]; i++) {
        why[0] = 0; out = VK_FORMAT_UNDEFINED;
        int rc = nvn_vk_format_texview_ok(cross_fmt[i].base, cross_fmt[i].nvn, cross_fmt[i].view,
                                          &out, why, sizeof why);
        printf("decoded cross-NVN %-18s -> rc %d (%s) %s\n", cross_fmt[i].name, rc, why,
               rc != 0 ? "ok" : "FAIL");
        if (rc == 0) bad++;
    }
    // #975：16 位打包格式的贴图映射。期望值不用被测表：NVN 位段按「低位起」手写，Vulkan 各 PACK16 格式的
    // 分量位段按规范手写（首名分量在最高位），逐分量核「NVN 的 c 分量 = 所选 Vulkan 格式第 map[c] 个分量」位段相同。
    // 两种路由都核：mask 0（全精确格式）与 mask 全 1（有退回的都走退回 + 通道补偿）。
    {
        struct p16f { uint32_t nvn; int lo[4], bits[4]; const char *name; };   // bits 0 = 没有该分量
        static const struct p16f nvnf[] = {
            { NVN_FORMAT_RGBA4,  {0, 4, 8, 12},  {4, 4, 4, 4}, "RGBA4" },
            { NVN_FORMAT_RGB5,   {0, 5, 10, 0},  {5, 5, 5, 0}, "RGB5" },
            { NVN_FORMAT_RGB5A1, {0, 5, 10, 15}, {5, 5, 5, 1}, "RGB5A1" },
            { NVN_FORMAT_RGB565, {0, 5, 11, 0},  {5, 6, 5, 0}, "RGB565" },
            { NVN_FORMAT_BGR565, {11, 5, 0, 0},  {5, 6, 5, 0}, "BGR565" },
            { NVN_FORMAT_BGR5,   {10, 5, 0, 0},  {5, 5, 5, 0}, "BGR5" },
            { NVN_FORMAT_BGR5A1, {10, 5, 0, 15}, {5, 5, 5, 1}, "BGR5A1" },
            { NVN_FORMAT_A1BGR5, {11, 6, 1, 0},  {5, 5, 5, 1}, "A1BGR5" },
        };
        // Vulkan 分量 R,G,B,A 的位段（规范 Formats 章：分量按名字从最高位往下排）。
        struct vkf { VkFormat vk; int lo[4], bits[4]; };
        static const struct vkf vkt[] = {
            { VK_FORMAT_A4B4G4R4_UNORM_PACK16_EXT, {0, 4, 8, 12},  {4, 4, 4, 4} },
            { VK_FORMAT_B4G4R4A4_UNORM_PACK16,     {4, 8, 12, 0},  {4, 4, 4, 4} },
            { VK_FORMAT_R4G4B4A4_UNORM_PACK16,     {12, 8, 4, 0},  {4, 4, 4, 4} },
            { VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR, {0, 5, 10, 15}, {5, 5, 5, 1} },
            { VK_FORMAT_A1R5G5B5_UNORM_PACK16,     {10, 5, 0, 15}, {5, 5, 5, 1} },
            { VK_FORMAT_R5G5B5A1_UNORM_PACK16,     {11, 6, 1, 0},  {5, 5, 5, 1} },
            { VK_FORMAT_B5G5R5A1_UNORM_PACK16,     {1, 6, 11, 0},  {5, 5, 5, 1} },
            { VK_FORMAT_R5G6B5_UNORM_PACK16,       {11, 5, 0, 0},  {5, 6, 5, 0} },
            { VK_FORMAT_B5G6R5_UNORM_PACK16,       {0, 5, 11, 0},  {5, 6, 5, 0} },
        };
        int routes = 0;
        for (int pass = 0; pass < 2; pass++) {
            nvn_vk_format_set_p16_fallback(pass ? 0xffu : 0u);
            for (unsigned i = 0; i < sizeof nvnf / sizeof nvnf[0]; i++) {
                VkFormat vk = nvn_vk_format(nvnf[i].nvn);
                uint8_t map[4];
                int remap = nvn_vk_format_chanmap(nvnf[i].nvn, map);
                const struct vkf *t = NULL;
                for (unsigned k = 0; k < sizeof vkt / sizeof vkt[0]; k++) if (vkt[k].vk == vk) t = &vkt[k];
                int ok = t != NULL;
                for (int c = 0; ok && c < 4; c++) {
                    if (!nvnf[i].bits[c]) continue;   // NVN 没有的分量（565 的 a、RGB5 的 a）不核位段
                    if (map[c] > 3 || t->lo[map[c]] != nvnf[i].lo[c] || t->bits[map[c]] != nvnf[i].bits[c]) ok = 0;
                }
                printf("p16 %-7s %s -> vk %d remap %d map %u%u%u%u %s\n", nvnf[i].name, pass ? "fallback" : "exact   ",
                       (int)vk, remap, map[0], map[1], map[2], map[3], ok ? "ok" : "FAIL");
                if (!ok) bad++;
                routes += remap;
            }
        }
        nvn_vk_format_set_p16_fallback(0);
        // 有退回格式的 5 项（RGBA4 / RGB5 / RGB5A1 / RGB565 / A1BGR5）在 mask 全 1 时都必须走补偿；mask 0 时一项都不走。
        printf("p16 remap routes = %d (want 5) %s\n", routes, routes == 5 ? "ok" : "FAIL");
        if (routes != 5) bad++;
        // 路由判定：精确格式不能采样、退回格式能采样才退；两边都不能采样不退（照精确格式报能力，由调用方报红）。
        VkFormatFeatureFlags ex[NVN_VK_P16_N], fb[NVN_VK_P16_N];
        const VkFormatFeatureFlags S = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        for (int i = 0; i < NVN_VK_P16_N; i++) { ex[i] = S; fb[i] = S; }
        uint32_t m0 = nvn_vk_p16_fallback_pick(ex, fb);
        for (int i = 0; i < NVN_VK_P16_N; i++) ex[i] = 0;
        uint32_t m1 = nvn_vk_p16_fallback_pick(ex, fb);
        for (int i = 0; i < NVN_VK_P16_N; i++) fb[i] = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        uint32_t m2 = nvn_vk_p16_fallback_pick(ex, fb);
        uint32_t want1 = 0;
        for (int i = 0; i < NVN_VK_P16_N; i++) { VkFormat f; nvn_vk_p16_entry(i, NULL, NULL, &f); if (f != VK_FORMAT_UNDEFINED) want1 |= 1u << i; }
        int ok = m0 == 0 && m1 == want1 && want1 == 0x8fu && m2 == 0 && !nvn_vk_p16_entry(NVN_VK_P16_N, NULL, NULL, NULL);
        printf("p16 pick: exact-ok 0x%x, exact-missing 0x%x (want 0x8f), fallback-unsampled 0x%x %s\n", m0, m1, m2, ok ? "ok" : "FAIL");
        if (!ok) bad++;
    }
    // CPU 顶点解码（设备无顶点获取能力的格式 / 必须解码的格式 → float4）位级对拍。
    // sRGB 期望值 = 规范分段公式（(c+0.055)/1.055)^2.4 段；16 位打包格式按 NVN 低位起（#975：首名分量在最低位），
    // 每格式逐个分量把该位段置满（再加一个最低位单比特），期望值按位段手算。
    // src 是小端字节序；exp 是 NVN 顶点取数语义（缺失分量补 (0,0,0,1)，RGB5/BGR5 的 bit15 是填充位）。
    // 32 位 UI2F/I2F 期望 = C 整数→float 的 IEEE 就近舍入（2^24 以上向偶数取整），逐位核对。
    struct { uint32_t fmt; uint8_t src[16]; float exp[4]; int eb; const char *name; } dec[] = {
        { NVN_FORMAT_RGBA8_SRGB,  {0x00,0x80,0xFF,0x80}, {0.0f, 2.15860500e-01f, 1.0f, 128.0f*(1.0f/255.0f)}, 4, "RGBA8_SRGB" },
        { NVN_FORMAT_BGRA8_SRGB,  {0x80,0x00,0xFF,0x40}, {1.0f, 0.0f, 2.15860500e-01f, 64.0f*(1.0f/255.0f)}, 4, "BGRA8_SRGB" },
        { NVN_FORMAT_RGBX8_SRGB,  {0xFF,0x00,0x00,0x00}, {1.0f, 0.0f, 0.0f, 1.0f}, 4, "RGBX8_SRGB" },
        { NVN_FORMAT_BGRX8_SRGB,  {0x80,0x40,0xFF,0x00}, {1.0f, 5.12694584e-02f, 2.15860500e-01f, 1.0f}, 4, "BGRX8_SRGB" },
        // RGBA4：r0-3 g4-7 b8-11 a12-15
        { NVN_FORMAT_RGBA4,       {0x0F,0x00,0,0},       {1.0f, 0.0f, 0.0f, 0.0f}, 2, "RGBA4 r" },
        { NVN_FORMAT_RGBA4,       {0xF0,0x00,0,0},       {0.0f, 1.0f, 0.0f, 0.0f}, 2, "RGBA4 g" },
        { NVN_FORMAT_RGBA4,       {0x00,0x0F,0,0},       {0.0f, 0.0f, 1.0f, 0.0f}, 2, "RGBA4 b" },
        { NVN_FORMAT_RGBA4,       {0x00,0xF0,0,0},       {0.0f, 0.0f, 0.0f, 1.0f}, 2, "RGBA4 a" },
        { NVN_FORMAT_RGBA4,       {0x01,0x00,0,0},       {1.0f / 15.0f, 0.0f, 0.0f, 0.0f}, 2, "RGBA4 r lsb" },
        // RGB5：r0-4 g5-9 b10-14，bit15 填充（a 恒 1）
        { NVN_FORMAT_RGB5,        {0x1F,0x00,0,0},       {1.0f, 0.0f, 0.0f, 1.0f}, 2, "RGB5 r" },
        { NVN_FORMAT_RGB5,        {0xE0,0x03,0,0},       {0.0f, 1.0f, 0.0f, 1.0f}, 2, "RGB5 g" },
        { NVN_FORMAT_RGB5,        {0x00,0x7C,0,0},       {0.0f, 0.0f, 1.0f, 1.0f}, 2, "RGB5 b" },
        { NVN_FORMAT_RGB5,        {0x00,0x80,0,0},       {0.0f, 0.0f, 0.0f, 1.0f}, 2, "RGB5 pad" },
        // RGB5A1：r0-4 g5-9 b10-14 a15
        { NVN_FORMAT_RGB5A1,      {0x1F,0x00,0,0},       {1.0f, 0.0f, 0.0f, 0.0f}, 2, "RGB5A1 r" },
        { NVN_FORMAT_RGB5A1,      {0xE0,0x03,0,0},       {0.0f, 1.0f, 0.0f, 0.0f}, 2, "RGB5A1 g" },
        { NVN_FORMAT_RGB5A1,      {0x00,0x7C,0,0},       {0.0f, 0.0f, 1.0f, 0.0f}, 2, "RGB5A1 b" },
        { NVN_FORMAT_RGB5A1,      {0x00,0x80,0,0},       {0.0f, 0.0f, 0.0f, 1.0f}, 2, "RGB5A1 a" },
        { NVN_FORMAT_RGB5A1,      {0x20,0x00,0,0},       {0.0f, 1.0f / 31.0f, 0.0f, 0.0f}, 2, "RGB5A1 g lsb" },
        // RGB565：r0-4 g5-10 b11-15
        { NVN_FORMAT_RGB565,      {0x1F,0x00,0,0},       {1.0f, 0.0f, 0.0f, 1.0f}, 2, "RGB565 r" },
        { NVN_FORMAT_RGB565,      {0xE0,0x07,0,0},       {0.0f, 1.0f, 0.0f, 1.0f}, 2, "RGB565 g" },
        { NVN_FORMAT_RGB565,      {0x00,0xF8,0,0},       {0.0f, 0.0f, 1.0f, 1.0f}, 2, "RGB565 b" },
        { NVN_FORMAT_RGB565,      {0x20,0x00,0,0},       {0.0f, 1.0f / 63.0f, 0.0f, 1.0f}, 2, "RGB565 g lsb" },
        // BGR565：b0-4 g5-10 r11-15
        { NVN_FORMAT_BGR565,      {0x1F,0x00,0,0},       {0.0f, 0.0f, 1.0f, 1.0f}, 2, "BGR565 b" },
        { NVN_FORMAT_BGR565,      {0xE0,0x07,0,0},       {0.0f, 1.0f, 0.0f, 1.0f}, 2, "BGR565 g" },
        { NVN_FORMAT_BGR565,      {0x00,0xF8,0,0},       {1.0f, 0.0f, 0.0f, 1.0f}, 2, "BGR565 r" },
        { NVN_FORMAT_BGR565,      {0x00,0x08,0,0},       {1.0f / 31.0f, 0.0f, 0.0f, 1.0f}, 2, "BGR565 r lsb" },
        // BGR5：b0-4 g5-9 r10-14，bit15 填充
        { NVN_FORMAT_BGR5,        {0x1F,0x00,0,0},       {0.0f, 0.0f, 1.0f, 1.0f}, 2, "BGR5 b" },
        { NVN_FORMAT_BGR5,        {0xE0,0x03,0,0},       {0.0f, 1.0f, 0.0f, 1.0f}, 2, "BGR5 g" },
        { NVN_FORMAT_BGR5,        {0x00,0x7C,0,0},       {1.0f, 0.0f, 0.0f, 1.0f}, 2, "BGR5 r" },
        { NVN_FORMAT_BGR5,        {0x00,0x80,0,0},       {0.0f, 0.0f, 0.0f, 1.0f}, 2, "BGR5 pad" },
        // BGR5A1：b0-4 g5-9 r10-14 a15
        { NVN_FORMAT_BGR5A1,      {0x1F,0x00,0,0},       {0.0f, 0.0f, 1.0f, 0.0f}, 2, "BGR5A1 b" },
        { NVN_FORMAT_BGR5A1,      {0xE0,0x03,0,0},       {0.0f, 1.0f, 0.0f, 0.0f}, 2, "BGR5A1 g" },
        { NVN_FORMAT_BGR5A1,      {0x00,0x7C,0,0},       {1.0f, 0.0f, 0.0f, 0.0f}, 2, "BGR5A1 r" },
        { NVN_FORMAT_BGR5A1,      {0x00,0x80,0,0},       {0.0f, 0.0f, 0.0f, 1.0f}, 2, "BGR5A1 a" },
        // A1BGR5：a0 b1-5 g6-10 r11-15
        { NVN_FORMAT_A1BGR5,      {0x01,0x00,0,0},       {0.0f, 0.0f, 0.0f, 1.0f}, 2, "A1BGR5 a" },
        { NVN_FORMAT_A1BGR5,      {0x3E,0x00,0,0},       {0.0f, 0.0f, 1.0f, 0.0f}, 2, "A1BGR5 b" },
        { NVN_FORMAT_A1BGR5,      {0xC0,0x07,0,0},       {0.0f, 1.0f, 0.0f, 0.0f}, 2, "A1BGR5 g" },
        { NVN_FORMAT_A1BGR5,      {0x00,0xF8,0,0},       {1.0f, 0.0f, 0.0f, 0.0f}, 2, "A1BGR5 r" },
        { NVN_FORMAT_A1BGR5,      {0x02,0x00,0,0},       {0.0f, 0.0f, 1.0f / 31.0f, 0.0f}, 2, "A1BGR5 b lsb" },
        { NVN_FORMAT_RGB9E5F,     {0x00,0x01,0x01,0x79}, {0.5f, 0.25f, 0.125f, 1.0f}, 4, "RGB9E5F" },
        { NVN_FORMAT_RGB8,       {0,128,255}, {0.0f, 128.0f/255.0f, 1.0f, 1.0f}, 3, "RGB8" },
        { NVN_FORMAT_RGB8SN,     {0x80,0x01,0x7f}, {-1.0f, 1.0f/127.0f, 1.0f, 1.0f}, 3, "RGB8SN" },
        { NVN_FORMAT_RGB8_UI2F,  {0,128,255}, {0.0f, 128.0f, 255.0f, 1.0f}, 3, "RGB8_UI2F" },
        { NVN_FORMAT_RGB8_I2F,   {0x80,0x01,0x7f}, {-128.0f, 1.0f, 127.0f, 1.0f}, 3, "RGB8_I2F" },
        { NVN_FORMAT_RGB16F,     {1,0, 0,0xbc, 0,0x80}, {0x1p-24f, -1.0f, -0.0f, 1.0f}, 6, "RGB16F" },
        { NVN_FORMAT_RGB16,      {0,0, 1,0, 0xff,0xff}, {0.0f, 1.0f/65535.0f, 1.0f, 1.0f}, 6, "RGB16" },
        { NVN_FORMAT_RGB16SN,    {0,0x80, 1,0, 0xff,0x7f}, {-1.0f, 1.0f/32767.0f, 1.0f, 1.0f}, 6, "RGB16SN" },
        { NVN_FORMAT_RGB16_UI2F, {0,0x80, 1,0, 0xff,0xff}, {32768.0f, 1.0f, 65535.0f, 1.0f}, 6, "RGB16_UI2F" },
        { NVN_FORMAT_RGB16_I2F,  {0,0x80, 1,0, 0xff,0x7f}, {-32768.0f, 1.0f, 32767.0f, 1.0f}, 6, "RGB16_I2F" },
        { NVN_FORMAT_RGB32F,     {0,0,0,0xbf, 0,0,0x00,0x40, 0,0,0,0x80}, {-0.5f, 2.0f, -0.0f, 1.0f}, 12, "RGB32F" },
        // 32 位 UI2F/I2F：0 / 最大值 / 负数 / 2^24 以上精度边界，逐位对拍。
        { NVN_FORMAT_R32_UI2F,    {0,0,0,0},           {0.0f, 0.0f, 0.0f, 1.0f}, 4, "R32_UI2F 0" },
        { NVN_FORMAT_R32_UI2F,    {0xC8,0,0,0},        {200.0f, 0.0f, 0.0f, 1.0f}, 4, "R32_UI2F 200" },
        { NVN_FORMAT_R32_UI2F,    {0xFF,0xFF,0xFF,0xFF},{4294967296.0f, 0.0f, 0.0f, 1.0f}, 4, "R32_UI2F max" },
        { NVN_FORMAT_R32_UI2F,    {0x01,0x00,0x00,0x01},{16777216.0f, 0.0f, 0.0f, 1.0f}, 4, "R32_UI2F 2^24+1" },
        { NVN_FORMAT_R32_UI2F,    {0x03,0x00,0x00,0x01},{16777220.0f, 0.0f, 0.0f, 1.0f}, 4, "R32_UI2F 2^24+3" },
        { NVN_FORMAT_R32_I2F,     {0xFF,0xFF,0xFF,0xFF},{-1.0f, 0.0f, 0.0f, 1.0f}, 4, "R32_I2F -1" },
        { NVN_FORMAT_R32_I2F,     {0x00,0x00,0x00,0x80},{-2147483648.0f, 0.0f, 0.0f, 1.0f}, 4, "R32_I2F min" },
        { NVN_FORMAT_R32_I2F,     {0x01,0x00,0x00,0x01},{16777216.0f, 0.0f, 0.0f, 1.0f}, 4, "R32_I2F 2^24+1" },
        { NVN_FORMAT_RG32_UI2F,   {0x01,0,0,0, 0xFF,0xFF,0xFF,0xFF},
                                  {1.0f, 4294967296.0f, 0.0f, 1.0f}, 8, "RG32_UI2F" },
        { NVN_FORMAT_RG32_I2F,    {0xFE,0xFF,0xFF,0xFF, 0xE8,0x03,0,0},
                                  {-2.0f, 1000.0f, 0.0f, 1.0f}, 8, "RG32_I2F" },
        { NVN_FORMAT_RGB32_UI2F,  {0x01,0x00,0x00,0x01, 0x07,0,0,0, 0x08,0,0,0},
                                  {16777216.0f, 7.0f, 8.0f, 1.0f}, 12, "RGB32_UI2F" },
        { NVN_FORMAT_RGB32_I2F,   {0xFF,0xFF,0xFF,0xFE, 0x00,0x00,0x00,0x80, 0x05,0,0,0},
                                  {-16777216.0f, -2147483648.0f, 5.0f, 1.0f}, 12, "RGB32_I2F" },
        { NVN_FORMAT_RGBA32_UI2F, {0,0,0,0, 0x01,0,0,0, 0xFF,0xFF,0xFF,0xFF, 0x03,0x00,0x00,0x01},
                                  {0.0f, 1.0f, 4294967296.0f, 16777220.0f}, 16, "RGBA32_UI2F" },
        { NVN_FORMAT_RGBA32_I2F,  {0xFF,0xFF,0xFF,0xFF, 0xFE,0xFF,0xFF,0xFF, 0xFD,0xFF,0xFF,0xFF, 0xFC,0xFF,0xFF,0xFF},
                                  {-1.0f, -2.0f, -3.0f, -4.0f}, 16, "RGBA32_I2F" },
    };
    for (unsigned i = 0; i < sizeof dec / sizeof dec[0]; i++) {
        float got[4] = {-1, -1, -1, -1};
        int eb = nvn_vattr_cpu_decode(dec[i].fmt, dec[i].src, got);
        int ok = eb == dec[i].eb;
        for (int c = 0; ok && c < 4; c++) {
            uint32_t gb, xb;
            memcpy(&gb, &got[c], 4); memcpy(&xb, &dec[i].exp[c], 4);
            if (gb != xb) ok = 0;   // 位级对拍：不允许任何舍入偏差
        }
        printf("vattr-decode %-18s -> eb %d {%g,%g,%g,%g} %s\n", dec[i].name, eb,
               got[0], got[1], got[2], got[3], ok ? "ok" : "FAIL");
        if (!ok) bad++;
    }
    // must_cpu_decode：32 位 UI2F/I2F 8 项必须解码（无原生等价物），且每项都有解码实现；
    // 其余格式（含 8/16 位 UI2F/I2F——原生 USCALED/SSCALED 已换算）不得标成必须解码。
    {
        uint32_t must[] = { NVN_FORMAT_R32_UI2F, NVN_FORMAT_R32_I2F,
                            NVN_FORMAT_RG32_UI2F, NVN_FORMAT_RG32_I2F,
                            NVN_FORMAT_RGB32_UI2F, NVN_FORMAT_RGB32_I2F,
                            NVN_FORMAT_RGBA32_UI2F, NVN_FORMAT_RGBA32_I2F };
        uint32_t native[] = { NVN_FORMAT_R8_UI2F, NVN_FORMAT_R8_I2F,
                              NVN_FORMAT_RGBA16_UI2F, NVN_FORMAT_RGBA16_I2F,
                              NVN_FORMAT_RGB10A2_UI2F, NVN_FORMAT_RGB10A2_I2F,
                              NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA8_SRGB,
                              NVN_FORMAT_RGB565, NVN_FORMAT_BGRA8, NVN_FORMAT_RGB9E5F, 0xDEAD };
        for (unsigned i = 0; i < sizeof must / sizeof must[0]; i++) {
            float got[4];
            int eb = nvn_vattr_cpu_decode(must[i], (const uint8_t *)"\x01\x02\x03\x04\x05\x06\x07\x08"
                                          "\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10", got);
            int ok = nvn_vattr_must_cpu_decode(must[i]) == 1 && eb > 0;
            printf("must-decode 0x%02x -> flag %d eb %d %s\n", must[i],
                   nvn_vattr_must_cpu_decode(must[i]), eb, ok ? "ok" : "FAIL");
            if (!ok) bad++;
        }
        for (unsigned i = 0; i < sizeof native / sizeof native[0]; i++) {
            if (nvn_vattr_must_cpu_decode(native[i]) != 0) {
                printf("must-decode 0x%02x -> 1 FAIL\n", native[i]);
                bad++;
            }
        }
    }
    // 不解码的格式返回 0：调用方按“不支持”明确报红（不猜、不兜底）。
    {
        float got[4] = {9, 9, 9, 9};
        uint32_t no_dec[] = { NVN_FORMAT_RGBA8, NVN_FORMAT_BGRA8, NVN_FORMAT_RGB_DXT1, NVN_FORMAT_DEPTH16, 0xFF };
        for (unsigned i = 0; i < sizeof no_dec / sizeof no_dec[0]; i++) {
            int eb = nvn_vattr_cpu_decode(no_dec[i], (const uint8_t *)"\x01\x02\x03\x04", got);
            if (eb != 0) { printf("vattr-decode 0x%x -> eb %d FAIL\n", no_dec[i], eb); bad++; }
        }
    }
    printf("nvn-vk-format: %s (%d failures)\n", bad ? "FAIL" : "ok", bad);
    return bad ? 1 : 0;
}
