// NVN 纹理格式 → Vulkan 格式的通用映射（新 Vulkan 后端用；不写死游戏）。
// 未知格式返回 VK_FORMAT_UNDEFINED，由调用方明确报红，不猜。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif
// 深度 / 模板格式（含纯模板 S8_UINT）：决定用法（深度模板附件）与走深度模板路径。图像有哪些面看下面两个（#1581 O）。
int nvn_vk_format_is_depth(VkFormat f);
// 有深度面（S8_UINT 没有）。
int nvn_vk_format_has_depth(VkFormat f);
// 压缩块格式（BC/ASTC/ETC2）：视图格式覆盖不适用于它们。
int nvn_vk_format_is_compressed(VkFormat f);
// 压缩块格式的图像总字节数：按该格式自己的块宽高向上取整（ASTC 4x4…12x12、BC 4x4）。
// 未压缩格式或未知格式返回 0，由调用方按 bpp 处理或明确报红。
uint64_t nvn_vk_format_compressed_image_bytes(uint32_t nvn_format, uint32_t w, uint32_t h);
int nvn_vk_format_has_stencil(VkFormat f);
VkFormat nvn_vk_format(uint32_t nvn_format);
// #398 深度格式替换：设备的 D24_UNORM_S8_UINT / X8_D24_UNORM_PACK32 没有 DEPTH_STENCIL_ATTACHMENT 时，
// NVN DEPTH24_STENCIL8 / DEPTH24 映射成 D32_SFLOAT_S8_UINT / D32_SFLOAT（同 Mac 后端「D24 换 D32」：
// 作渲染目标是精度提升、行为等价；按字节上传 / 回读要换算深度面）。
enum { NVN_VK_DEPTH_SUBST_D24S8 = 1u << 0, NVN_VK_DEPTH_SUBST_X8D24 = 1u << 1 };
// 纯判定（可单测）：各参数是对应格式的 optimalTilingFeatures。原格式缺深度附件、替换格式有，才换。
uint32_t nvn_vk_depth_subst_pick(VkFormatFeatureFlags d24s8, VkFormatFeatureFlags d32s8,
                                 VkFormatFeatureFlags x8d24, VkFormatFeatureFlags d32);
// 建设备时设一次（vk_device.c）；影响之后所有 nvn_vk_format / nvn_vk_tex_format 的结果。
void nvn_vk_format_set_depth_subst(uint32_t mask);
uint32_t nvn_vk_format_depth_subst(void);
int nvn_vk_format_substituted(uint32_t nvn_format);   // 1 = 本设备上这个 NVN 格式被换了存储
// 深度面字节换算（n 个 4 字节纹素，原地）：D24 深度面（低 24 位 UNORM）↔ D32_SFLOAT 深度面。
void nvn_vk_depth24_to_f32(void *texels, size_t n);
void nvn_vk_depth_f32_to_24(void *texels, size_t n);
VkFormat nvn_vk_tex_format(uint32_t nvn_format);   // 贴图专用：需要上传展开时返回展开目标格式
// #975：NVN 16 位打包格式从最低位起命名（RGBA4 = r 在 0–3 位 … a 在 12–15 位；iOS nvn_fmt_mtl.c 真机实测、
// Ryujinx FormatTable.cs 同口径），Vulkan 的 PACK16 格式是首名分量在最高位。每个格式有一个位序精确对上的
// Vulkan 格式（exact）；exact 在本设备不能采样（要的扩展没开 / 缺 SAMPLED_IMAGE）时退到同尺寸的退回格式
// （fallback），由采样视图的通道映射把分量放回去。通道映射对渲染目标 / 存储图像 / 顶点取数 / blit 不起作用，
// 走退回格式的格式在这些用途上由调用方具名报红。
enum { NVN_VK_P16_N = 8 };
// 第 i 项（0..NVN_VK_P16_N-1）：NVN 格式、精确格式、退回格式（没有 = UNDEFINED）。越界返回 0。
int nvn_vk_p16_entry(int i, uint32_t *nvn_format, VkFormat *exact, VkFormat *fallback);
// 纯判定（可单测）：exact[i] / fallback[i] 是第 i 项两种格式的 optimalTilingFeatures（精确格式要的扩展没开时
// 调用方传 0）。精确格式不能采样、退回格式能采样时第 i 位置 1。
uint32_t nvn_vk_p16_fallback_pick(const VkFormatFeatureFlags exact[NVN_VK_P16_N], const VkFormatFeatureFlags fallback[NVN_VK_P16_N]);
// 建设备时设一次（vk_device.c）；默认 0 = 全走精确格式。
void nvn_vk_format_set_p16_fallback(uint32_t mask);
uint32_t nvn_vk_format_p16_fallback(void);
// 本设备上这个 NVN 格式的分量位置：map[c] = NVN 第 c 个分量（r,g,b,a）在映射出的 Vulkan 格式里是第几个
// 分量（0 = R … 3 = A）。返回 1 = 非恒等（走了退回格式，只能采样）；0 = 恒等（map 写 0,1,2,3）。
int nvn_vk_format_chanmap(uint32_t nvn_format, uint8_t map[4]);
// CPU 顶点解码：设备不支持顶点获取（bufferFeatures 无 VERTEX_BUFFER_BIT）的格式，
// 把一个顶点的属性字节解成 float4，经 R32G32B32A32_SFLOAT 顶点缓冲进 shader。
// 返回该格式的存储字节数；0 = 无解码路径（调用方按不支持报红）。out 恒写满 4 分量，
// 缺失分量按 NVN 顶点取数缺省补 (0,0,0,1)。
int nvn_vattr_cpu_decode(uint32_t nvn_format, const uint8_t *src, float out[4]);
// 与设备能力无关、必须走 CPU 解码的顶点格式：nvn_vk_format 映射出的整数存储格式
// 表达不了它的取数语义（32 位 UI2F/I2F，无 scaled 等价物）。返回 1 = 顶点路径必须解码；
// 这些格式在 nvn_vattr_cpu_decode 里都有实现，配套出现。
int nvn_vattr_must_cpu_decode(uint32_t nvn_format);
// NVNtextureView::SetFormat 的附件/采样覆盖合法性：只允许同兼容类的 8 位颜色格式互换。
// 输出 bpp/class；不合法返回 -1 并写 why（不回退 base 格式）。
int nvn_vk_format_override_class(VkFormat f, uint32_t *bpp, int *cls);
int nvn_vk_format_override_ok(VkFormat base, uint32_t nvn_format, VkFormat *out, char *why, size_t why_size);
// 纹理采样视图的格式选择：view_nvn_format == tex_nvn_format（视图就是纹理自身格式）时不是
// 格式覆盖，输出 base（解码后的图像/默认采样视图格式）；异格式才按兼容类表校验。
int nvn_vk_format_texview_ok(VkFormat base, uint32_t tex_nvn_format, uint32_t view_nvn_format,
                             VkFormat *out, char *why, size_t why_size);
// bc_decoded 贴图（compute 解码未压缩存储或 BC1->ASTC 4x4 转码存储）的异格式视图解析：
// - BC->BC：仅允许同解码算法的 UNORM<->SRGB 孪生（BC1 RGB/RGBA、BC2、BC3、BC7）及 RGBA_DXT1*->RGB_DXT1*；
// - BC->未压缩：仅限未压缩解码存储，且视图格式须为单平面非深度/非压缩/非展开且每纹素字节数与存储一致；
// - 其余（跨族 BC、BC4/5 UNORM<->SNORM、BC6H SFLOAT<->UFLOAT、ASTC 转码图的非 BC1 视图、异字节类等）明确返回 -1。
int nvn_vk_bc_decoded_view_ok(VkFormat storage_fmt, uint32_t tex_nvn_format, uint32_t view_nvn_format,
                              VkFormat *out, char *why, size_t why_size);
#ifdef __cplusplus
}
#endif
