// BC1–BC7 compute 解码（Granite MIT 着色器，源码 graphics/nvn/third_party/granite/decode/）。
// 用在 bc_native_mask 里没有原生 SAMPLED_IMAGE 支持的 NVN BC 格式：把压缩块经一次性
// 提交（staging→输入图→dispatch→dst 存储视图，等 fence）解成未压缩图像。
// 契约与 nvn_vk_texcache.c upload() 相同：返回 0 = 完成；-1 = 没有提交成功（临时资源已回收）；
// -2 = 已提交但没有完成证明（临时资源转入模块内保留账本，图像由调用方保留）。
// 解码失败（建模块/管线/dispatch/写原因）在 why 里写明 BC 族名（s3tc/rgtc/bc6/bc7）。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

struct s2a_vk_device;
struct nvn_vk_bcdec;

// 建四条族的全部管线变体；任何一族失败返回非零，why 写明族名（整局不进直连的上层凭此报红）。
int nvn_vk_bcdec_create(struct s2a_vk_device *d, struct nvn_vk_bcdec **out, char *why, size_t why_size);
void nvn_vk_bcdec_destroy(struct s2a_vk_device *d, struct nvn_vk_bcdec *bc);
// 等所有未证明完成的解码并回收内部资源；0 = 全部完成，-1 = 未证明完成（对象与目标图继续由调用方保活）。
// destroy 内部也会尝试等待；未完成时保留对象，调用方可稍后再调 wait / destroy。
int nvn_vk_bcdec_wait(struct s2a_vk_device *d, struct nvn_vk_bcdec *bc, uint64_t timeout_ns);

// nvn 格式 → 解码路由。返回 1 时给出：dst 存储视图格式、采样（逻辑）格式、物理建图格式、
// BC 族名、每块字节数。sRGB 格式物理图是 UNORM（移动端 sRGB+STORAGE 普遍不支持），SRGB 只放视图。
int nvn_vk_bcdec_route(uint32_t nvn_format, VkFormat *storage_view_fmt, VkFormat *decoded_fmt,
                       VkFormat *image_fmt, const char **family, int *block_bytes);

// 把 blocks（bw×bh 块，bb 字节/块，紧凑排布）解码进 dst 的 mip/layer（w×h 目标像素）。
// dst 必须带 STORAGE 与 MUTABLE_FORMAT usage；调用后 dst 该子资源处于 GENERAL、含解码结果。
// 这是一次性提交 + 同步等完成（最长 5 s），只给旧层 texcache 和模块单测用；新层后端的 BC 上传（CPU 字节与缓冲两条）都走下面的段内解码，
// 不单独提交——单独提交排在还没完成的段后面，等待会跟着它们一起等。
int nvn_vk_bcdec_decode(struct nvn_vk_bcdec *bc, struct s2a_vk_device *d,
                        VkImage dst, uint32_t nvn_format, uint32_t w, uint32_t h,
                        uint32_t mip, uint32_t layer, const void *blocks, size_t bytes,
                        char *why, size_t why_size);

// BC 贴图的段内解码（#697 缓冲 → BC 起用，CPU 字节 → BC 也走这条）：解码排进调用方的命令段，和段里前后的命令按 GPU 顺序生效，
// 不单独提交、不等 fence。这里只建资源（输入图、视图、描述符集，挑好管线），**不录命令**：
// 命令由调用方用自己的 vkCmd* 录（后端开两级回放时 vkCmd* 走包装进录制线程，模块里直接调会和段里其它命令乱序）。
// 输入图 8 字节块用 RG32_UINT、16 字节块用 RGBA32_UINT（一个纹素一块；S3TC / RGTC 的 8 字节块着色器只读 .xy）。
// 调用方要录的：in 从 UNDEFINED 转 TRANSFER_DST → 源（16 字节对齐的紧凑块）CopyBufferToImage 到 in（bw×bh）→ in 转 GENERAL
// → dst 子资源 UNDEFINED 转 GENERAL → 绑 pipe / ds（布局 pl）→ 推 {w, h} 8 字节 → Dispatch((w+7)/8, (h+7)/8, 1)。
// 资源要等段执行完才能 nvn_vk_bcdec_rec_free；返回非 0 时 rec 已清空。
struct nvn_vk_bcdec_rec { VkImage in; VkDeviceMemory in_mem; VkImageView in_view, out_view; VkDescriptorPool pool;
                          VkDescriptorSet ds; VkPipeline pipe; VkPipelineLayout pl; uint32_t bw, bh; };
int nvn_vk_bcdec_prepare(struct nvn_vk_bcdec *bc, struct s2a_vk_device *d, VkImage dst, uint32_t nvn_format,
                         uint32_t w, uint32_t h, uint32_t mip, uint32_t layer, struct nvn_vk_bcdec_rec *rec, char *why, size_t why_size);
void nvn_vk_bcdec_rec_free(struct s2a_vk_device *d, struct nvn_vk_bcdec_rec *rec);
