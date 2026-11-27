// NVN → Vulkan 格式映射。NVN_FORMAT_* 取自 graphics/nvn/gfx/nvn/include/nvn.h。
// UI2F / I2F 是“整数存储 + 顶点取样时转 float”的变体，对应 Vulkan 的 USCALED/SSCALED。
// 32 位没有 scaled 等价物：nvn_vk_format 仍给整数存储格式（映射描述存储事实），
// 顶点路径一律走 CPU 解码（nvn_vattr_must_cpu_decode），不让 Float 输入拿到整数位模式。
#include "nvn_vk_format.h"
#include "nvn_fmt_info.h"
#include "include/nvn.h"
#include <stdio.h>
#include <string.h>

// #398：设备缺 D24 深度附件时的换格式（参照 Mac 后端「D24 换 D32」）。进程内同时只有一台 Vulkan 设备
// （vk_device.c 建设备时按格式能力设一次）；默认 0 = 照原样映射。
static uint32_t g_depth_subst;

uint32_t nvn_vk_depth_subst_pick(VkFormatFeatureFlags d24s8, VkFormatFeatureFlags d32s8,
                                 VkFormatFeatureFlags x8d24, VkFormatFeatureFlags d32) {
    const VkFormatFeatureFlags att = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    uint32_t m = 0;
    if (!(d24s8 & att) && (d32s8 & att)) m |= NVN_VK_DEPTH_SUBST_D24S8;
    if (!(x8d24 & att) && (d32 & att)) m |= NVN_VK_DEPTH_SUBST_X8D24;
    return m;
}
void nvn_vk_format_set_depth_subst(uint32_t mask) { g_depth_subst = mask; }
uint32_t nvn_vk_format_depth_subst(void) { return g_depth_subst; }
int nvn_vk_format_substituted(uint32_t f) {
    return (f == NVN_FORMAT_DEPTH24_STENCIL8 && (g_depth_subst & NVN_VK_DEPTH_SUBST_D24S8)) ||
           (f == NVN_FORMAT_DEPTH24 && (g_depth_subst & NVN_VK_DEPTH_SUBST_X8D24));
}

// 换成 D32 存储后的深度面字节换算，口径与原 D24 深度面拷贝一致（每纹素 4 字节，低 24 位是 UNORM 深度）。
void nvn_vk_depth24_to_f32(void *texels, size_t n) {
    uint8_t *p = texels;
    for (size_t i = 0; i < n; i++, p += 4) {
        uint32_t v; memcpy(&v, p, 4);
        float f = (float)((double)(v & 0xffffffu) / 16777215.0);
        memcpy(p, &f, 4);
    }
}
void nvn_vk_depth_f32_to_24(void *texels, size_t n) {
    uint8_t *p = texels;
    for (size_t i = 0; i < n; i++, p += 4) {
        float f; memcpy(&f, p, 4);
        double d = f != f ? 0.0 : f < 0.0f ? 0.0 : f > 1.0f ? 1.0 : (double)f;   // NaN 按 0
        uint32_t v = (uint32_t)(d * 16777215.0 + 0.5);
        memcpy(p, &v, 4);
    }
}

// #975：16 位打包格式（见 nvn_vk_format.h）。精确格式按 NVN 低位起的位段逐个对上 Vulkan 的位段：
//   RGBA4  r0-3 g4-7 b8-11 a12-15       = A4B4G4R4（VK_EXT_4444_formats）；退回 B4G4R4A4（必备）：r 落在它的 A、g 落在 R、b 落在 G、a 落在 B
//   RGB5A1 r0-4 g5-9 b10-14 a15 / RGB5  = A1B5G5R5（VK_KHR_maintenance5）；退回 A1R5G5B5（必备），r/b 对调
//   RGB565 r0-4 g5-10 b11-15            = B5G6R5；退回 R5G6B5（必备），r/b 对调
//   BGR565 b0-4 g5-10 r11-15            = R5G6B5（必备），无需退回
//   BGR5A1 b0-4 g5-9 r10-14 a15 / BGR5  = A1R5G5B5（必备），无需退回
//   A1BGR5 a0 b1-5 g6-10 r11-15         = R5G5B5A1；退回 B5G5R5A1，r/b 对调
// map 是退回格式下 NVN 分量 r,g,b,a 各落在 Vulkan 的第几个分量。RGB5 / BGR5 的 bit15 是填充位，
// a 读 1 由视图（nvn_fmt_alpha_reads_one）钉。
static const struct { uint32_t nvn; VkFormat exact, fallback; uint8_t map[4]; } k_p16[NVN_VK_P16_N] = {
    { NVN_FORMAT_RGBA4,  VK_FORMAT_A4B4G4R4_UNORM_PACK16_EXT, VK_FORMAT_B4G4R4A4_UNORM_PACK16, {3, 0, 1, 2} },
    { NVN_FORMAT_RGB5,   VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR, VK_FORMAT_A1R5G5B5_UNORM_PACK16, {2, 1, 0, 3} },
    { NVN_FORMAT_RGB5A1, VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR, VK_FORMAT_A1R5G5B5_UNORM_PACK16, {2, 1, 0, 3} },
    { NVN_FORMAT_RGB565, VK_FORMAT_B5G6R5_UNORM_PACK16,       VK_FORMAT_R5G6B5_UNORM_PACK16,   {2, 1, 0, 3} },
    { NVN_FORMAT_BGR565, VK_FORMAT_R5G6B5_UNORM_PACK16,       VK_FORMAT_UNDEFINED,             {0, 1, 2, 3} },
    { NVN_FORMAT_BGR5,   VK_FORMAT_A1R5G5B5_UNORM_PACK16,     VK_FORMAT_UNDEFINED,             {0, 1, 2, 3} },
    { NVN_FORMAT_BGR5A1, VK_FORMAT_A1R5G5B5_UNORM_PACK16,     VK_FORMAT_UNDEFINED,             {0, 1, 2, 3} },
    { NVN_FORMAT_A1BGR5, VK_FORMAT_R5G5B5A1_UNORM_PACK16,     VK_FORMAT_B5G5R5A1_UNORM_PACK16, {2, 1, 0, 3} },
};
static uint32_t g_p16_fallback;   // 第 i 位 = k_p16[i] 走退回格式；同 g_depth_subst，建设备时设一次

int nvn_vk_p16_entry(int i, uint32_t *nvn_format, VkFormat *exact, VkFormat *fallback) {
    if (i < 0 || i >= NVN_VK_P16_N) return 0;
    if (nvn_format) *nvn_format = k_p16[i].nvn;
    if (exact) *exact = k_p16[i].exact;
    if (fallback) *fallback = k_p16[i].fallback;
    return 1;
}
uint32_t nvn_vk_p16_fallback_pick(const VkFormatFeatureFlags exact[NVN_VK_P16_N], const VkFormatFeatureFlags fallback[NVN_VK_P16_N]) {
    const VkFormatFeatureFlags s = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    uint32_t m = 0;
    for (int i = 0; i < NVN_VK_P16_N; i++)
        if (k_p16[i].fallback != VK_FORMAT_UNDEFINED && !(exact[i] & s) && (fallback[i] & s)) m |= 1u << i;
    return m;
}
void nvn_vk_format_set_p16_fallback(uint32_t mask) {   // 没有退回格式的项不收（位置 1 也照走精确格式）
    for (int i = 0; i < NVN_VK_P16_N; i++) if (k_p16[i].fallback == VK_FORMAT_UNDEFINED) mask &= ~(1u << i);
    g_p16_fallback = mask & ((1u << NVN_VK_P16_N) - 1u);
}
uint32_t nvn_vk_format_p16_fallback(void) { return g_p16_fallback; }
static int p16_index(uint32_t f) {
    for (int i = 0; i < NVN_VK_P16_N; i++) if (k_p16[i].nvn == f) return i;
    return -1;
}
int nvn_vk_format_chanmap(uint32_t f, uint8_t map[4]) {
    int i = p16_index(f);
    int remap = i >= 0 && ((g_p16_fallback >> i) & 1u);
    for (int c = 0; c < 4; c++) map[c] = remap ? k_p16[i].map[c] : (uint8_t)c;
    return remap;
}

VkFormat nvn_vk_tex_format(uint32_t f) {
    nvn_fmt_info info;
    if (!nvn_fmt_info_of(f, &info)) return VK_FORMAT_UNDEFINED;
    return info.expand ? nvn_vk_format(info.expand) : nvn_vk_format(f);
}

VkFormat nvn_vk_format(uint32_t f) {
    nvn_fmt_info info;
    if (!nvn_fmt_info_of(f, &info)) return VK_FORMAT_UNDEFINED;   // 中立表没有的格式不映射
    switch (f) {
    case NVN_FORMAT_R8: return VK_FORMAT_R8_UNORM;
    case NVN_FORMAT_R8SN: return VK_FORMAT_R8_SNORM;
    case NVN_FORMAT_R8UI: return VK_FORMAT_R8_UINT;
    case NVN_FORMAT_R8I: return VK_FORMAT_R8_SINT;
    case NVN_FORMAT_R16F: return VK_FORMAT_R16_SFLOAT;
    case NVN_FORMAT_R16: return VK_FORMAT_R16_UNORM;
    case NVN_FORMAT_R16SN: return VK_FORMAT_R16_SNORM;
    case NVN_FORMAT_R16UI: return VK_FORMAT_R16_UINT;
    case NVN_FORMAT_R16I: return VK_FORMAT_R16_SINT;
    case NVN_FORMAT_R32F: return VK_FORMAT_R32_SFLOAT;
    case NVN_FORMAT_R32UI: return VK_FORMAT_R32_UINT;
    case NVN_FORMAT_R32I: return VK_FORMAT_R32_SINT;
    case NVN_FORMAT_RG8: return VK_FORMAT_R8G8_UNORM;
    case NVN_FORMAT_RG8SN: return VK_FORMAT_R8G8_SNORM;
    case NVN_FORMAT_RG8UI: return VK_FORMAT_R8G8_UINT;
    case NVN_FORMAT_RG8I: return VK_FORMAT_R8G8_SINT;
    case NVN_FORMAT_RG16F: return VK_FORMAT_R16G16_SFLOAT;
    case NVN_FORMAT_RG16: return VK_FORMAT_R16G16_UNORM;
    case NVN_FORMAT_RG16SN: return VK_FORMAT_R16G16_SNORM;
    case NVN_FORMAT_RG16UI: return VK_FORMAT_R16G16_UINT;
    case NVN_FORMAT_RG16I: return VK_FORMAT_R16G16_SINT;
    case NVN_FORMAT_RG32F: return VK_FORMAT_R32G32_SFLOAT;
    case NVN_FORMAT_RG32UI: return VK_FORMAT_R32G32_UINT;
    case NVN_FORMAT_RG32I: return VK_FORMAT_R32G32_SINT;
    case NVN_FORMAT_RGB8: return VK_FORMAT_R8G8B8_UNORM;
    case NVN_FORMAT_RGB8SN: return VK_FORMAT_R8G8B8_SNORM;
    case NVN_FORMAT_RGB8UI: return VK_FORMAT_R8G8B8_UINT;
    case NVN_FORMAT_RGB8I: return VK_FORMAT_R8G8B8_SINT;
    case NVN_FORMAT_RGB16F: return VK_FORMAT_R16G16B16_SFLOAT;
    case NVN_FORMAT_RGB16: return VK_FORMAT_R16G16B16_UNORM;
    case NVN_FORMAT_RGB16SN: return VK_FORMAT_R16G16B16_SNORM;
    case NVN_FORMAT_RGB16UI: return VK_FORMAT_R16G16B16_UINT;
    case NVN_FORMAT_RGB16I: return VK_FORMAT_R16G16B16_SINT;
    case NVN_FORMAT_RGB32F: return VK_FORMAT_R32G32B32_SFLOAT;
    case NVN_FORMAT_RGB32UI: return VK_FORMAT_R32G32B32_UINT;
    case NVN_FORMAT_RGB32I: return VK_FORMAT_R32G32B32_SINT;
    case NVN_FORMAT_RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case NVN_FORMAT_RGBA8SN: return VK_FORMAT_R8G8B8A8_SNORM;
    case NVN_FORMAT_RGBA8UI: return VK_FORMAT_R8G8B8A8_UINT;
    case NVN_FORMAT_RGBA8I: return VK_FORMAT_R8G8B8A8_SINT;
    case NVN_FORMAT_RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case NVN_FORMAT_RGBA16: return VK_FORMAT_R16G16B16A16_UNORM;
    case NVN_FORMAT_RGBA16SN: return VK_FORMAT_R16G16B16A16_SNORM;
    case NVN_FORMAT_RGBA16UI: return VK_FORMAT_R16G16B16A16_UINT;
    case NVN_FORMAT_RGBA16I: return VK_FORMAT_R16G16B16A16_SINT;
    case NVN_FORMAT_RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case NVN_FORMAT_RGBA32UI: return VK_FORMAT_R32G32B32A32_UINT;
    case NVN_FORMAT_RGBA32I: return VK_FORMAT_R32G32B32A32_SINT;
    case NVN_FORMAT_STENCIL8: return VK_FORMAT_S8_UINT;
    case NVN_FORMAT_DEPTH16: return VK_FORMAT_D16_UNORM;
    // #398：设备没有 D24 深度附件（三星 Xclipse 只有 D16S8 / D32S8）时换 D32，见 nvn_vk_format_set_depth_subst。
    case NVN_FORMAT_DEPTH24:
        return (g_depth_subst & NVN_VK_DEPTH_SUBST_X8D24) ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_X8_D24_UNORM_PACK32;
    case NVN_FORMAT_DEPTH32F: return VK_FORMAT_D32_SFLOAT;
    case NVN_FORMAT_DEPTH24_STENCIL8:
        return (g_depth_subst & NVN_VK_DEPTH_SUBST_D24S8) ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT;
    case NVN_FORMAT_DEPTH32F_STENCIL8: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case NVN_FORMAT_RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    // #975：16 位打包格式按位段对上（表与退回规则见 k_p16）。BGR5 / RGB5 没有无 alpha 的 Vulkan 形式，
    // 按多一位 alpha 的形式收，a 由视图钉成 1。
    case NVN_FORMAT_RGBA4: case NVN_FORMAT_RGB5: case NVN_FORMAT_RGB5A1: case NVN_FORMAT_RGB565:
    case NVN_FORMAT_BGR565: case NVN_FORMAT_BGR5: case NVN_FORMAT_BGR5A1: case NVN_FORMAT_A1BGR5: {
        int i = p16_index(f);
        return ((g_p16_fallback >> i) & 1u) ? k_p16[i].fallback : k_p16[i].exact;
    }
    case NVN_FORMAT_RGB10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case NVN_FORMAT_RGB10A2UI: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    // NVN 把 10/10/10/2 的 SNORM/SINT 也列进了枚举（顶点法线/切线常用）；位序按“从低位往上
    // r,g,b,a”的既有口径（与 RGB10A2 相同），对应 Vulkan 的 A2B10G10R10_*_PACK32。
    case NVN_FORMAT_RGB10A2SN: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case NVN_FORMAT_RGB10A2I: return VK_FORMAT_A2B10G10R10_SINT_PACK32;
    // “X”是未定义的第四通道：Vulkan 没有 8/16/32 位的 RGBX 格式，落到同宽的 RGBA（X 位读作
    // 有效通道但不保证内容；NVN 也不允许依赖 X）。这是唯一能表示同样存储布局的 Vulkan 格式。
    case NVN_FORMAT_RGBX8: return VK_FORMAT_R8G8B8A8_UNORM;
    case NVN_FORMAT_RGBX8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case NVN_FORMAT_RGBX8SN: return VK_FORMAT_R8G8B8A8_SNORM;
    case NVN_FORMAT_RGBX8UI: return VK_FORMAT_R8G8B8A8_UINT;
    case NVN_FORMAT_RGBX8I: return VK_FORMAT_R8G8B8A8_SINT;
    case NVN_FORMAT_RGBX16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case NVN_FORMAT_RGBX16: return VK_FORMAT_R16G16B16A16_UNORM;
    case NVN_FORMAT_RGBX16SN: return VK_FORMAT_R16G16B16A16_SNORM;
    case NVN_FORMAT_RGBX16UI: return VK_FORMAT_R16G16B16A16_UINT;
    case NVN_FORMAT_RGBX16I: return VK_FORMAT_R16G16B16A16_SINT;
    case NVN_FORMAT_RGBX32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case NVN_FORMAT_RGBX32UI: return VK_FORMAT_R32G32B32A32_UINT;
    case NVN_FORMAT_RGBX32I: return VK_FORMAT_R32G32B32A32_SINT;
    // BGR 族：NVN 按“低位往上 r,g,b,a”命名，与 Vulkan 的 BGR 打包顺序一一对应（16 位打包的 BGR 见上面 k_p16）。
    case NVN_FORMAT_BGRX8: return VK_FORMAT_B8G8R8A8_UNORM;
    case NVN_FORMAT_BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
    case NVN_FORMAT_BGRX8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case NVN_FORMAT_BGRA8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case NVN_FORMAT_R11G11B10F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case NVN_FORMAT_RGB9E5F: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case NVN_FORMAT_RGB_DXT1: case NVN_FORMAT_RGBA_DXT1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_DXT3: return VK_FORMAT_BC2_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_DXT5: return VK_FORMAT_BC3_UNORM_BLOCK;
    case NVN_FORMAT_RGB_DXT1_SRGB: case NVN_FORMAT_RGBA_DXT1_SRGB: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_DXT3_SRGB: return VK_FORMAT_BC2_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_DXT5_SRGB: return VK_FORMAT_BC3_SRGB_BLOCK;
    case NVN_FORMAT_RGTC1_UNORM: return VK_FORMAT_BC4_UNORM_BLOCK;
    case NVN_FORMAT_RGTC1_SNORM: return VK_FORMAT_BC4_SNORM_BLOCK;
    case NVN_FORMAT_RGTC2_UNORM: return VK_FORMAT_BC5_UNORM_BLOCK;
    case NVN_FORMAT_RGTC2_SNORM: return VK_FORMAT_BC5_SNORM_BLOCK;
    case NVN_FORMAT_BPTC_UNORM: return VK_FORMAT_BC7_UNORM_BLOCK;
    case NVN_FORMAT_BPTC_UNORM_SRGB: return VK_FORMAT_BC7_SRGB_BLOCK;
    case NVN_FORMAT_BPTC_SFLOAT: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case NVN_FORMAT_BPTC_UFLOAT: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    // ASTC 全 14 种块尺寸 × UNORM / SRGB（nvn.h 0x79–0x94），不按游戏挑子集。
    case NVN_FORMAT_RGBA_ASTC_4x4: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_5x4: return VK_FORMAT_ASTC_5x4_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_5x5: return VK_FORMAT_ASTC_5x5_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_6x5: return VK_FORMAT_ASTC_6x5_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_6x6: return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_8x5: return VK_FORMAT_ASTC_8x5_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_8x6: return VK_FORMAT_ASTC_8x6_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_8x8: return VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_10x5: return VK_FORMAT_ASTC_10x5_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_10x6: return VK_FORMAT_ASTC_10x6_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_10x8: return VK_FORMAT_ASTC_10x8_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_10x10: return VK_FORMAT_ASTC_10x10_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_12x10: return VK_FORMAT_ASTC_12x10_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_12x12: return VK_FORMAT_ASTC_12x12_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_4x4_SRGB: return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_5x4_SRGB: return VK_FORMAT_ASTC_5x4_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_5x5_SRGB: return VK_FORMAT_ASTC_5x5_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_6x5_SRGB: return VK_FORMAT_ASTC_6x5_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_6x6_SRGB: return VK_FORMAT_ASTC_6x6_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_8x5_SRGB: return VK_FORMAT_ASTC_8x5_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_8x6_SRGB: return VK_FORMAT_ASTC_8x6_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_8x8_SRGB: return VK_FORMAT_ASTC_8x8_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_10x5_SRGB: return VK_FORMAT_ASTC_10x5_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_10x6_SRGB: return VK_FORMAT_ASTC_10x6_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_10x8_SRGB: return VK_FORMAT_ASTC_10x8_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_10x10_SRGB: return VK_FORMAT_ASTC_10x10_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_12x10_SRGB: return VK_FORMAT_ASTC_12x10_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_12x12_SRGB: return VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
    // UI2F / I2F：整数存储、顶点取样时转换成 float（Metal vconv_i2f 同义，200 → 200.0）。
    // Vulkan 的 USCALED/SSCALED 正是这一换算，且编译事实（nvk_attribute_type）把这类输入
    // 声明为 Float，所以 8 / 16 位与 10:10:10:2 一律走 USCALED/SSCALED。32 位没有
    // USCALED/SSCALED 等价物：这里仍映射整数存储格式（存储事实如此），但顶点路径不再消费
    // 这个映射——nvn_vattr_must_cpu_decode 把这 8 项钉成必须 CPU 解码，整数→float 转换后
    // 经 R32G32B32A32_SFLOAT 合成 binding 进 shader。贴图路径由 expand 指到同宽 float 格式。
    case NVN_FORMAT_R8_UI2F: return VK_FORMAT_R8_USCALED;
    case NVN_FORMAT_R8_I2F: return VK_FORMAT_R8_SSCALED;
    case NVN_FORMAT_R16_UI2F: return VK_FORMAT_R16_USCALED;
    case NVN_FORMAT_R16_I2F: return VK_FORMAT_R16_SSCALED;
    case NVN_FORMAT_R32_UI2F: return VK_FORMAT_R32_UINT;
    case NVN_FORMAT_R32_I2F: return VK_FORMAT_R32_SINT;
    case NVN_FORMAT_RG8_UI2F: return VK_FORMAT_R8G8_USCALED;
    case NVN_FORMAT_RG8_I2F: return VK_FORMAT_R8G8_SSCALED;
    case NVN_FORMAT_RG16_UI2F: return VK_FORMAT_R16G16_USCALED;
    case NVN_FORMAT_RG16_I2F: return VK_FORMAT_R16G16_SSCALED;
    case NVN_FORMAT_RG32_UI2F: return VK_FORMAT_R32G32_UINT;
    case NVN_FORMAT_RG32_I2F: return VK_FORMAT_R32G32_SINT;
    case NVN_FORMAT_RGB8_UI2F: return VK_FORMAT_R8G8B8_USCALED;
    case NVN_FORMAT_RGB8_I2F: return VK_FORMAT_R8G8B8_SSCALED;
    case NVN_FORMAT_RGB16_UI2F: return VK_FORMAT_R16G16B16_USCALED;
    case NVN_FORMAT_RGB16_I2F: return VK_FORMAT_R16G16B16_SSCALED;
    case NVN_FORMAT_RGB32_UI2F: return VK_FORMAT_R32G32B32_UINT;
    case NVN_FORMAT_RGB32_I2F: return VK_FORMAT_R32G32B32_SINT;
    case NVN_FORMAT_RGBA8_UI2F: return VK_FORMAT_R8G8B8A8_USCALED;
    case NVN_FORMAT_RGBA8_I2F: return VK_FORMAT_R8G8B8A8_SSCALED;
    case NVN_FORMAT_RGBA16_UI2F: return VK_FORMAT_R16G16B16A16_USCALED;
    case NVN_FORMAT_RGBA16_I2F: return VK_FORMAT_R16G16B16A16_SSCALED;
    case NVN_FORMAT_RGBA32_UI2F: return VK_FORMAT_R32G32B32A32_UINT;
    case NVN_FORMAT_RGBA32_I2F: return VK_FORMAT_R32G32B32A32_SINT;
    case NVN_FORMAT_RGB10A2_UI2F: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
    case NVN_FORMAT_RGB10A2_I2F: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
    // RGBX：同位宽 RGBA（X 字节读作第 4 分量，Metal 经 swizzle 把 alpha 固定成 1；顶点属性在上游
    // nvn_vattr_fmt_canon 已归一成 RGB，这里的映射主要给纹理 / 视图路径兜底）。
    // BGR 家族（BGRA8 是 Switch 顶点颜色的常见格式）；16 位打包的 BGR5 / BGR5A1 / A1BGR5 见上面 k_p16。
    default: return VK_FORMAT_UNDEFINED;
    }
}

// 该 VkFormat 是否是深度/模板格式（决定 image usage / view aspect / 布局）。
int nvn_vk_format_is_compressed(VkFormat f) {
    switch (f) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK:
    case VK_FORMAT_ASTC_4x4_UNORM_BLOCK: case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
    case VK_FORMAT_ASTC_5x5_UNORM_BLOCK: case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
    case VK_FORMAT_ASTC_6x6_UNORM_BLOCK: case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
    case VK_FORMAT_ASTC_8x6_UNORM_BLOCK: case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
    case VK_FORMAT_ASTC_10x5_UNORM_BLOCK: case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
    case VK_FORMAT_ASTC_10x8_UNORM_BLOCK: case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
    case VK_FORMAT_ASTC_12x10_UNORM_BLOCK: case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
    case VK_FORMAT_ASTC_4x4_SRGB_BLOCK: case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
    case VK_FORMAT_ASTC_5x5_SRGB_BLOCK: case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
    case VK_FORMAT_ASTC_6x6_SRGB_BLOCK: case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
    case VK_FORMAT_ASTC_8x6_SRGB_BLOCK: case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
    case VK_FORMAT_ASTC_10x5_SRGB_BLOCK: case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
    case VK_FORMAT_ASTC_10x8_SRGB_BLOCK: case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
    case VK_FORMAT_ASTC_12x10_SRGB_BLOCK: case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
        return 1;
    default: return 0;
    }
}

int nvn_vk_format_is_depth(VkFormat f) {
    switch (f) {
        case VK_FORMAT_S8_UINT: case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return 1;
        default: return 0;
    }
}
int nvn_vk_format_has_depth(VkFormat f) {
    return nvn_vk_format_is_depth(f) && f != VK_FORMAT_S8_UINT;
}
uint64_t nvn_vk_format_compressed_image_bytes(uint32_t nvn_format, uint32_t w, uint32_t h) {
    nvn_fmt_info info;
    if (!nvn_fmt_info_of(nvn_format, &info) || !info.bytes) return 0;
    if (info.bw <= 1 && info.bh <= 1) return 0;   // 未压缩：块宽高 1x1
    return ((uint64_t)(w + info.bw - 1) / info.bw) *
           ((uint64_t)(h + info.bh - 1) / info.bh) * info.bytes;
}

int nvn_vk_format_has_stencil(VkFormat f) {
    switch (f) {
        case VK_FORMAT_S8_UINT: case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return 1;
        default: return 0;
    }
}

// 视图格式覆盖的合法性：只允许 Vulkan 同兼容类的 8 位颜色格式互换（UNORM/SRGB 同类）。
// 其它格式一律明确拒绝——不拿不合法的视图创建去赌驱动行为。
int nvn_vk_format_override_class(VkFormat f, uint32_t *bpp, int *cls) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: *bpp = 4; *cls = 1; return 1;
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SRGB: *bpp = 1; *cls = 2; return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SRGB: *bpp = 2; *cls = 3; return 1;
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32: *bpp = 4; *cls = 4; return 1;
    default: return 0;
    }
}
int nvn_vk_format_override_ok(VkFormat base, uint32_t nvn_format, VkFormat *out,
                              char *why, size_t why_size) {
    VkFormat vf = nvn_vk_format(nvn_format);
    if (vf == VK_FORMAT_UNDEFINED) {
        if (why && why_size) snprintf(why, why_size, "texture view format 0x%x is not mapped", nvn_format);
        return -1;
    }
    // 视图格式与图像格式相同不是覆盖：Vulkan 只要求不同格式时同兼容类（且才需要 MUTABLE_FORMAT）。
    // 同格式视图（如 BC4 纹理上设同格式 + 自身 swizzle 的采样视图）恒合法，不进类表。
    if (vf != base) {
        uint32_t ab = 0, bb = 0; int ac = 0, bc = 0;
        if (!nvn_vk_format_override_class(vf, &ab, &ac) || !nvn_vk_format_override_class(base, &bb, &bc) ||
            ab != bb || ac != bc) {
            if (why && why_size) snprintf(why, why_size,
                "texture view format override 0x%x (%d) is not in the same compatibility class as the image format %d",
                nvn_format, (int)vf, (int)base);
            return -1;
        }
    }
    if (out) *out = vf;
    return 0;
}

// 纹理视图的格式选择：视图 set_format 等于纹理自身的 NVN 格式时不是格式覆盖。
// compute 解码的 BC 纹理图像是解码后格式（如 BC4 → R8_UNORM），视图必须沿用 base
// （图像/默认采样视图格式）；拿 BC4 去和 R8_UNORM 比兼容类会误拒。
// 只有异格式才进兼容类表，异格式不兼容仍准确拒绝。
int nvn_vk_format_texview_ok(VkFormat base, uint32_t tex_nvn_format, uint32_t view_nvn_format,
                             VkFormat *out, char *why, size_t why_size) {
    if (view_nvn_format == tex_nvn_format) {
        if (out) *out = base;
        return 0;
    }
    return nvn_vk_format_override_ok(base, view_nvn_format, out, why, why_size);
}

// sRGB → 线性：规范分段公式在 8 位输入上逐值预计算（与固定功能顶点获取同一数学定义；
// 本文件刻意不进 libm，powf/ldexpf 会让宿主纯函数单测多一条链接依赖）。
static const float vattr_srgb_lut[256] = {
    0.00000000e+00f, 3.03526984e-04f, 6.07053967e-04f, 9.10580951e-04f, 1.21410793e-03f, 1.51763492e-03f,
    1.82116190e-03f, 2.12468888e-03f, 2.42821587e-03f, 2.73174285e-03f, 3.03526984e-03f, 3.34653576e-03f,
    3.67650732e-03f, 4.02471702e-03f, 4.39144204e-03f, 4.77695348e-03f, 5.18151670e-03f, 5.60539162e-03f,
    6.04883302e-03f, 6.51209079e-03f, 6.99541019e-03f, 7.49903204e-03f, 8.02319299e-03f, 8.56812562e-03f,
    9.13405870e-03f, 9.72121732e-03f, 1.03298230e-02f, 1.09600940e-02f, 1.16122452e-02f, 1.22864884e-02f,
    1.29830323e-02f, 1.37020830e-02f, 1.44438436e-02f, 1.52085144e-02f, 1.59962934e-02f, 1.68073758e-02f,
    1.76419545e-02f, 1.85002201e-02f, 1.93823610e-02f, 2.02885631e-02f, 2.12190104e-02f, 2.21738848e-02f,
    2.31533662e-02f, 2.41576324e-02f, 2.51868596e-02f, 2.62412219e-02f, 2.73208916e-02f, 2.84260395e-02f,
    2.95568344e-02f, 3.07134437e-02f, 3.18960331e-02f, 3.31047666e-02f, 3.43398068e-02f, 3.56013149e-02f,
    3.68894504e-02f, 3.82043716e-02f, 3.95462353e-02f, 4.09151969e-02f, 4.23114106e-02f, 4.37350293e-02f,
    4.51862044e-02f, 4.66650863e-02f, 4.81718242e-02f, 4.97065660e-02f, 5.12694584e-02f, 5.28606470e-02f,
    5.44802764e-02f, 5.61284900e-02f, 5.78054302e-02f, 5.95112382e-02f, 6.12460542e-02f, 6.30100177e-02f,
    6.48032667e-02f, 6.66259386e-02f, 6.84781698e-02f, 7.03600957e-02f, 7.22718507e-02f, 7.42135684e-02f,
    7.61853815e-02f, 7.81874218e-02f, 8.02198203e-02f, 8.22827071e-02f, 8.43762115e-02f, 8.65004620e-02f,
    8.86555863e-02f, 9.08417112e-02f, 9.30589628e-02f, 9.53074666e-02f, 9.75873471e-02f, 9.98987282e-02f,
    1.02241733e-01f, 1.04616484e-01f, 1.07023103e-01f, 1.09461711e-01f, 1.11932428e-01f, 1.14435374e-01f,
    1.16970668e-01f, 1.19538428e-01f, 1.22138772e-01f, 1.24771818e-01f, 1.27437680e-01f, 1.30136477e-01f,
    1.32868322e-01f, 1.35633330e-01f, 1.38431615e-01f, 1.41263291e-01f, 1.44128471e-01f, 1.47027266e-01f,
    1.49959790e-01f, 1.52926152e-01f, 1.55926464e-01f, 1.58960835e-01f, 1.62029376e-01f, 1.65132195e-01f,
    1.68269400e-01f, 1.71441101e-01f, 1.74647404e-01f, 1.77888416e-01f, 1.81164244e-01f, 1.84474995e-01f,
    1.87820772e-01f, 1.91201683e-01f, 1.94617830e-01f, 1.98069320e-01f, 2.01556254e-01f, 2.05078736e-01f,
    2.08636870e-01f, 2.12230757e-01f, 2.15860500e-01f, 2.19526200e-01f, 2.23227957e-01f, 2.26965874e-01f,
    2.30740049e-01f, 2.34550582e-01f, 2.38397574e-01f, 2.42281122e-01f, 2.46201327e-01f, 2.50158285e-01f,
    2.54152094e-01f, 2.58182853e-01f, 2.62250658e-01f, 2.66355605e-01f, 2.70497791e-01f, 2.74677312e-01f,
    2.78894263e-01f, 2.83148740e-01f, 2.87440838e-01f, 2.91770650e-01f, 2.96138271e-01f, 3.00543794e-01f,
    3.04987314e-01f, 3.09468923e-01f, 3.13988713e-01f, 3.18546778e-01f, 3.23143209e-01f, 3.27778098e-01f,
    3.32451536e-01f, 3.37163615e-01f, 3.41914425e-01f, 3.46704056e-01f, 3.51532600e-01f, 3.56400144e-01f,
    3.61306780e-01f, 3.66252596e-01f, 3.71237680e-01f, 3.76262123e-01f, 3.81326011e-01f, 3.86429434e-01f,
    3.91572478e-01f, 3.96755231e-01f, 4.01977780e-01f, 4.07240212e-01f, 4.12542613e-01f, 4.17885071e-01f,
    4.23267670e-01f, 4.28690497e-01f, 4.34153636e-01f, 4.39657174e-01f, 4.45201195e-01f, 4.50785783e-01f,
    4.56411023e-01f, 4.62077000e-01f, 4.67783796e-01f, 4.73531496e-01f, 4.79320183e-01f, 4.85149940e-01f,
    4.91020850e-01f, 4.96932995e-01f, 5.02886458e-01f, 5.08881321e-01f, 5.14917665e-01f, 5.20995573e-01f,
    5.27115126e-01f, 5.33276404e-01f, 5.39479489e-01f, 5.45724461e-01f, 5.52011402e-01f, 5.58340390e-01f,
    5.64711506e-01f, 5.71124829e-01f, 5.77580440e-01f, 5.84078418e-01f, 5.90618841e-01f, 5.97201788e-01f,
    6.03827339e-01f, 6.10495571e-01f, 6.17206562e-01f, 6.23960392e-01f, 6.30757136e-01f, 6.37596874e-01f,
    6.44479682e-01f, 6.51405637e-01f, 6.58374817e-01f, 6.65387298e-01f, 6.72443157e-01f, 6.79542470e-01f,
    6.86685312e-01f, 6.93871761e-01f, 7.01101892e-01f, 7.08375780e-01f, 7.15693501e-01f, 7.23055129e-01f,
    7.30460740e-01f, 7.37910409e-01f, 7.45404210e-01f, 7.52942217e-01f, 7.60524505e-01f, 7.68151147e-01f,
    7.75822218e-01f, 7.83537792e-01f, 7.91297940e-01f, 7.99102738e-01f, 8.06952258e-01f, 8.14846572e-01f,
    8.22785754e-01f, 8.30769877e-01f, 8.38799012e-01f, 8.46873232e-01f, 8.54992608e-01f, 8.63157213e-01f,
    8.71367119e-01f, 8.79622397e-01f, 8.87923118e-01f, 8.96269353e-01f, 9.04661174e-01f, 9.13098652e-01f,
    9.21581856e-01f, 9.30110858e-01f, 9.38685728e-01f, 9.47306537e-01f, 9.55973353e-01f, 9.64686248e-01f,
    9.73445290e-01f, 9.82250550e-01f, 9.91102097e-01f, 1.00000000e+00f,
};

// 9 位尾数 × 2^(e-24)：2^k 用指数位拼出（k ∈ [-24,7] 恒在正规格内），不借 libm。
static float vattr_e5m9(int mant, int e) {
    uint32_t bits = (uint32_t)(127 + e - 24) << 23;
    float scale;
    memcpy(&scale, &bits, sizeof scale);
    return (float)mant * scale;
}

// 32 位 UI2F/I2F 的逐分量转换：小端 u32 / i32 → float。C 的整数→float 是 IEEE 就近
// 舍入（>2^24 的值向偶数取整），与 GPU scaled 取数 / Metal vconv_i2f 同一结果。
static void vattr_i2f32(const uint8_t *src, float *out, int n, int is_signed) {
    for (int i = 0; i < n; i++) {
        uint32_t u;
        memcpy(&u, src + 4 * i, 4);
        out[i] = is_signed ? (float)(int32_t)u : (float)u;
    }
}

static float vattr_f16(uint16_t value) {
    const uint32_t exp = (value >> 10) & 31u, frac = value & 1023u;
    if (!exp) {
        const float magnitude = (float)frac * 0x1p-24f;
        return value & 0x8000u ? -magnitude : magnitude;
    }
    const uint32_t bits = ((uint32_t)(value & 0x8000u) << 16) | ((exp == 31u ? 255u : exp + 112u) << 23) | (frac << 13);
    float out;
    memcpy(&out, &bits, sizeof out);
    return out;
}

// 顶点取数语义无原生 VkFormat 等价物、必须走 CPU 解码的 NVN 格式（不取决于设备能力）：
// 32 位 UI2F/I2F——Vulkan 无 32 位 USCALED/SSCALED；按 *_UINT/*_SINT 绑定会让声明为
// Float 的着色器输入拿到整数位模式（静默读错值）。返回 1 = 顶点路径必须解码。
int nvn_vattr_must_cpu_decode(uint32_t nvn_format) {
    switch (nvn_format) {
    case NVN_FORMAT_R32_UI2F: case NVN_FORMAT_R32_I2F:
    case NVN_FORMAT_RG32_UI2F: case NVN_FORMAT_RG32_I2F:
    case NVN_FORMAT_RGB32_UI2F: case NVN_FORMAT_RGB32_I2F:
    case NVN_FORMAT_RGBA32_UI2F: case NVN_FORMAT_RGBA32_I2F:
        return 1;
    default: return 0;
    }
}

int nvn_vattr_cpu_decode(uint32_t nvn_format, const uint8_t *src, float out[4]) {
    uint16_t v;
    uint32_t p;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (nvn_format) {
    // 三分量顶点格式的设备支持逐格式查。缺原生格式时只解这三个分量，W 仍是默认的 1。
    case NVN_FORMAT_RGB8:
    case NVN_FORMAT_RGB8SN:
    case NVN_FORMAT_RGB8_UI2F:
    case NVN_FORMAT_RGB8_I2F:
        for (int i = 0; i < 3; i++) {
            const int8_t s = (int8_t)src[i];
            switch (nvn_format) {
            case NVN_FORMAT_RGB8: out[i] = (float)src[i] / 255.0f; break;
            case NVN_FORMAT_RGB8SN: out[i] = s == INT8_MIN ? -1.0f : (float)s / 127.0f; break;
            case NVN_FORMAT_RGB8_UI2F: out[i] = (float)src[i]; break;
            default: out[i] = (float)s; break;
            }
        }
        return 3;
    case NVN_FORMAT_RGB16F:
    case NVN_FORMAT_RGB16:
    case NVN_FORMAT_RGB16SN:
    case NVN_FORMAT_RGB16_UI2F:
    case NVN_FORMAT_RGB16_I2F:
        for (int i = 0; i < 3; i++) {
            memcpy(&v, src + 2 * i, sizeof v);
            const int16_t s = (int16_t)v;
            switch (nvn_format) {
            case NVN_FORMAT_RGB16F: out[i] = vattr_f16(v); break;
            case NVN_FORMAT_RGB16: out[i] = (float)v / 65535.0f; break;
            case NVN_FORMAT_RGB16SN: out[i] = s == INT16_MIN ? -1.0f : (float)s / 32767.0f; break;
            case NVN_FORMAT_RGB16_UI2F: out[i] = (float)v; break;
            default: out[i] = (float)s; break;
            }
        }
        return 6;
    case NVN_FORMAT_RGB32F:
        memcpy(out, src, 3 * sizeof out[0]);
        return 12;
    // 8 位 sRGB：RGB 分量过 EOTF，alpha 恒为线性 unorm；RGBX/BGRX 的 X 是填充位取 1.0。
    case NVN_FORMAT_RGBX8_SRGB:
        out[0] = vattr_srgb_lut[src[0]]; out[1] = vattr_srgb_lut[src[1]]; out[2] = vattr_srgb_lut[src[2]];
        return 4;
    case NVN_FORMAT_RGBA8_SRGB:
        out[0] = vattr_srgb_lut[src[0]]; out[1] = vattr_srgb_lut[src[1]]; out[2] = vattr_srgb_lut[src[2]];
        out[3] = src[3] * (1.0f / 255.0f);
        return 4;
    case NVN_FORMAT_BGRX8_SRGB:
        out[0] = vattr_srgb_lut[src[2]]; out[1] = vattr_srgb_lut[src[1]]; out[2] = vattr_srgb_lut[src[0]];
        return 4;
    case NVN_FORMAT_BGRA8_SRGB:
        out[0] = vattr_srgb_lut[src[2]]; out[1] = vattr_srgb_lut[src[1]]; out[2] = vattr_srgb_lut[src[0]];
        out[3] = src[3] * (1.0f / 255.0f);
        return 4;
    // 16 位打包（#975）：NVN 从最低位起命名——首名分量在最低位（与贴图映射 k_p16 同一口径）。
    // RGB5/BGR5 的 bit15 是填充位不是 alpha（NVN 无该分量），alpha 取缺省 1.0。
    case NVN_FORMAT_RGBA4:
        memcpy(&v, src, 2);
        out[0] = (v & 0xf) / 15.0f; out[1] = ((v >> 4) & 0xf) / 15.0f;
        out[2] = ((v >> 8) & 0xf) / 15.0f; out[3] = ((v >> 12) & 0xf) / 15.0f;
        return 2;
    case NVN_FORMAT_RGB5:
        memcpy(&v, src, 2);
        out[0] = (v & 0x1f) / 31.0f; out[1] = ((v >> 5) & 0x1f) / 31.0f;
        out[2] = ((v >> 10) & 0x1f) / 31.0f;
        return 2;
    case NVN_FORMAT_RGB5A1:
        memcpy(&v, src, 2);
        out[0] = (v & 0x1f) / 31.0f; out[1] = ((v >> 5) & 0x1f) / 31.0f;
        out[2] = ((v >> 10) & 0x1f) / 31.0f; out[3] = (float)((v >> 15) & 1);
        return 2;
    case NVN_FORMAT_RGB565:
        memcpy(&v, src, 2);
        out[0] = (v & 0x1f) / 31.0f; out[1] = ((v >> 5) & 0x3f) / 63.0f;
        out[2] = ((v >> 11) & 0x1f) / 31.0f;
        return 2;
    case NVN_FORMAT_BGR565:
        memcpy(&v, src, 2);
        out[2] = (v & 0x1f) / 31.0f; out[1] = ((v >> 5) & 0x3f) / 63.0f;
        out[0] = ((v >> 11) & 0x1f) / 31.0f;
        return 2;
    case NVN_FORMAT_BGR5:
        memcpy(&v, src, 2);
        out[2] = (v & 0x1f) / 31.0f; out[1] = ((v >> 5) & 0x1f) / 31.0f;
        out[0] = ((v >> 10) & 0x1f) / 31.0f;
        return 2;
    case NVN_FORMAT_BGR5A1:
        memcpy(&v, src, 2);
        out[2] = (v & 0x1f) / 31.0f; out[1] = ((v >> 5) & 0x1f) / 31.0f;
        out[0] = ((v >> 10) & 0x1f) / 31.0f; out[3] = (float)((v >> 15) & 1);
        return 2;
    case NVN_FORMAT_A1BGR5:
        memcpy(&v, src, 2);
        out[3] = (float)(v & 1);
        out[2] = ((v >> 1) & 0x1f) / 31.0f; out[1] = ((v >> 6) & 0x1f) / 31.0f;
        out[0] = ((v >> 11) & 0x1f) / 31.0f;
        return 2;
    // 32 位 UI2F/I2F：无 Vulkan scaled 等价物，顶点路径一律 CPU 解码（must_cpu_decode
    // 保证走得到这里）；缺失分量沿用缺省 (0,0,0,1)。
    case NVN_FORMAT_R32_UI2F:    vattr_i2f32(src, out, 1, 0); return 4;
    case NVN_FORMAT_R32_I2F:     vattr_i2f32(src, out, 1, 1); return 4;
    case NVN_FORMAT_RG32_UI2F:   vattr_i2f32(src, out, 2, 0); return 8;
    case NVN_FORMAT_RG32_I2F:    vattr_i2f32(src, out, 2, 1); return 8;
    case NVN_FORMAT_RGB32_UI2F:  vattr_i2f32(src, out, 3, 0); return 12;
    case NVN_FORMAT_RGB32_I2F:   vattr_i2f32(src, out, 3, 1); return 12;
    case NVN_FORMAT_RGBA32_UI2F: vattr_i2f32(src, out, 4, 0); return 16;
    case NVN_FORMAT_RGBA32_I2F:  vattr_i2f32(src, out, 4, 1); return 16;
    // RGB9_E5 共享指数尾数：E 在 31-27，B/G/R 各占 9 位（值 = mant × 2^(E-24)）。
    case NVN_FORMAT_RGB9E5F:
        memcpy(&p, src, 4);
        {
            int e = (int)(p >> 27);
            out[0] = vattr_e5m9((int)(p & 0x1ffu), e);
            out[1] = vattr_e5m9((int)((p >> 9) & 0x1ffu), e);
            out[2] = vattr_e5m9((int)((p >> 18) & 0x1ffu), e);
        }
        return 4;
    default:
        return 0;
    }
}
