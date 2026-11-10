// NVN 多颜色渲染目标（MRT）→ Vulkan 附件计划：纯逻辑，宿主回归与真机共用。
//
// 这里只做“槽位事实 → 附件计划 / 逐槽颜色混合状态”的确定性推导，不创建 Vulkan 对象、
// 不读设备能力（格式是否可作颜色附件由调用方按 physical device 能力再核）。
// NVN 的槽位是 0..7，SetRenderTargets 允许中间槽为空；空槽是合法空洞，不是缺事实。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVN_VK_MRT_MAX_COLOR 8

// 一个颜色槽的已解析事实（由 first_draw 从 rt 快照 + 纹理缓存填）。
// bound=0 时其余字段不参与计划；bound=1 时 format/尺寸/identity 必须真实。
struct nvn_vk_mrt_slot {
    uint8_t bound;
    VkFormat format;          // 附件视图格式（不是“纹理对象格式”的猜测值）
    uint32_t width, height;   // 该视图 mip 级别的实际尺寸
    uint64_t image_id;        // 底层图像身份（VkImage 或宿主夹具里的唯一值）
    uint32_t level, layer;    // 视图子资源：重复槽判定用（与 Metal rt_dup_slot 同口径）
    uint32_t write_mask;      // NVNchannelMaskState 该槽的 4 位写掩码（r=1 g=2 b=4 a=8）
};

// 计划：color_count 是 subpass 的 colorAttachmentCount（最高绑定槽 + 1，含空洞）；
// attachment_count 是 render pass 真正声明的附件数（只含参与的槽 + 可选深度）。
// color_att[i] 对空洞/被丢弃的重复槽是 VK_ATTACHMENT_UNUSED。
struct nvn_vk_mrt_plan {
    uint32_t color_count;
    uint32_t bound_mask;       // 参与本次绘制的槽（重复且写掩码为 0 的槽已剔除）
    uint32_t dropped_mask;     // 因“同 image+level+layer 重复”被丢弃的槽
    uint32_t attachment_count;
    uint32_t color_att[NVN_VK_MRT_MAX_COLOR];
    uint32_t depth_att;
    uint32_t depth_used;
    uint32_t width, height;
    uint32_t first_slot;       // 第一参与槽；无颜色时为 UINT32_MAX
};

// 返回 0 = 计划成立；-1 = 未支持/事实不一致，why 写明确原因。不会用默认格式/尺寸兜底。
int nvn_vk_mrt_plan_build(const struct nvn_vk_mrt_slot slots[NVN_VK_MRT_MAX_COLOR], uint32_t rt_count,
                          int has_depth, uint32_t depth_width, uint32_t depth_height,
                          struct nvn_vk_mrt_plan *plan, char *why, size_t why_size);

// 逐槽颜色混合状态输入：NVN 状态对象原文 + seen 位。
// blend_obj 按 NVNblendState.target 分槽（不是“最后一次绑定”）；blend_seen 的 bit i = 槽 i 绑过。
struct nvn_vk_mrt_blend_input {
    uint8_t color_obj[0x18];
    uint8_t chmask_obj[0x18];
    uint8_t blend_obj[NVN_VK_MRT_MAX_COLOR][0x18];
    uint8_t state_seen;   // bit0 blend / bit1 color / bit2 chmask（与 s2a_record_draw_state 一致）
    uint8_t blend_seen;   // bit i = 该槽的 blend 状态真实绑定过
};

// 生成一个槽的 VkPipelineColorBlendAttachmentState：
//   * 未参与槽：写掩码 0、混合关（附件是 VK_ATTACHMENT_UNUSED，不写任何通道）；
//   * 写掩码取 chmask 的 4*i 位；blendEnable 取 color.blend_en 的 bit i；
//   * 整数格式不混合（Vulkan 合同；NVN/Maxwell 同样忽略整数目标的混合状态）；
//   * ADD/ONE/ZERO 的数学等价配置关混合（不改变像素，省 dst 读+混合）。
// 片元未写输出的信息当前编译结果不提供，这里不猜写掩码。
void nvn_vk_mrt_color_blend(uint32_t slot, int slot_bound, VkFormat format,
                            const struct nvn_vk_mrt_blend_input *in,
                            VkPipelineColorBlendAttachmentState *out);

VkBlendFactor nvn_vk_mrt_blend_factor(uint32_t f);
VkBlendOp nvn_vk_mrt_blend_op(uint32_t e);

// 颜色附件的基本格式约束（不含设备能力）：非 UNDEFINED、非深度/模板、非压缩块格式。
int nvn_vk_mrt_format_is_color(VkFormat f);

// 图形阶段 NVN 驱动常量段 c0：shader 从 c0 读“存储缓冲描述符表”等驱动生成数据。
// 已测量的布局（与 Metal 路线同一份测量结论，这里是 Vulkan 侧唯一的合成口径决定）：
//   vs/fs/gs/tcs/tes 的存储缓冲表基址 = 0x110/0x510/0x410/0x210/0x310，每槽 16 字节
//   {uint64 addr, uint32 size, uint32 pad}（只用 NVN 已绑定的槽，未绑定槽保持 0）；
//   0x610 = 最大点径（float 2047.0f，GLSLC 用它钳制 shader 写的 gl_PointSize）；
//   0x644 = 本笔 draw 的 base instance（gl_InstanceID 不含它，绝对实例要加）。
// 其它位置保持 0：没有测量结论的驱动字段不猜。
#define NVN_VK_DRIVER_C0_BYTES 0xA00u
#define NVN_VK_DRIVER_C0_MAX_POINT 2047.0f
int nvn_vk_driver_c0_base(uint32_t stage_idx);   // 0=vs 1=fs 2=gs 3=tcs 4=tes；未知返回 -1
int nvn_vk_driver_c0_fill_graphics(uint8_t *dst, size_t dst_size, uint32_t stage_idx,
                                   const uint64_t ssbo_addr[16], const uint32_t ssbo_size[16],
                                   uint32_t base_instance, char *why, size_t why_size);

// NVN 允许 storage 绑定给 size=0（“整个缓冲/无边界”，与 Vulkan 的 VK_WHOLE_SIZE 同义）。
// guest 池的 GPU 地址窗口由前端维护（hle_nvn.c 的 gva 表，三元组 {GPU 起点, CPU 起点, 字节数}），
// 这里按“所在池的池尾 − 地址”求可证明的区间，并夹到调用方给的上限。
// 返回 0 = 不可证明（地址不在任何池窗口 / 前端不可用），调用方必须准确失败，不许猜。
uint64_t nvn_vk_guest_pool_extent(uint64_t guest_addr, uint64_t cap);
// 同上，但优先用“地址所在活 nvnBuffer 的精确长度”（*exact=1）；只有退回池尾近似时 *exact=0。
// 可写绑定必须用 exact=1 的区间才能整段回写，否则会覆盖相邻分配。
uint64_t nvn_vk_guest_buffer_extent(uint64_t guest_addr, uint64_t cap, int *exact);

// 反馈采样快照的源子资源计划：附件底图可以是多 mip / 多层 / 3D 的，但 SetRenderTargets 的
// cview 只选一个 mip + 单层切片；快照必须从该子资源拷，不能假设 level=0/layer=0。
// 3D 底图的“层号”是深度切片（与 nvn_vk_texview.h 的 attachment 口径一致）；快照仍是 2D，
// 拷贝时用 srcOffset.z 选切片（Vulkan 的 3D 子资源不能按 array layer 选）。
struct nvn_vk_feedback_src {
    uint32_t level, layer, layers;
    uint8_t is_3d;     // 1 = 底图是 3D，layer 字段无意义，用 slice
    uint32_t slice;
};
int nvn_vk_feedback_src_plan(uint32_t target, uint32_t depth, uint32_t levels, uint32_t layers,
                             uint32_t view_level, uint32_t view_layer, uint32_t view_layers,
                             uint32_t w, uint32_t h,
                             struct nvn_vk_feedback_src *out, char *why, size_t why_size);

// 单子资源 → 2D 快照的拷贝区域（纯构造，不调 Vulkan）。extent 取 pass 与快照的交集；
// 3D 源用 srcSubresource.layerCount=1 + srcOffset.z=slice，其余源用 baseArrayLayer。
int nvn_vk_feedback_copy_make(const struct nvn_vk_feedback_src *src, uint32_t pass_w, uint32_t pass_h,
                              uint32_t snap_w, uint32_t snap_h, VkImageCopy *out,
                              char *why, size_t why_size);

// 采样绑定替换判定：附件被同一张图采样时，能不能/该不该把绑定换成快照视图。
//   NONE       采样视图不含附件那个子资源：不是本子资源的反馈，保持原绑定（合法）。
//   SUBSTITUTE 采样视图正好是附件子资源、形状是单层 2D：换单层 2D 快照。
//   ISO        采样视图覆盖多个子资源（默认整幅视图落在多级/多层/3D 底图上）：单层 2D 快照
//              代表不了整幅视图，换“底图同形”的同构快照（nvn_vk_feedback_iso）的等形视图。
//   LEAVE      反馈存在但两条快照路都表达不了（视图建不出、格式/通道重写等）：保持原绑定并记原因。
struct nvn_vk_feedback_bind {
    uint8_t fb_is_3d;              // 附件底图是 3D（fb_layer 是深度切片）
    uint32_t fb_level, fb_layer;   // 附件子资源
    uint32_t tex_levels, tex_layers, tex_depth;
    uint8_t view_override;         // 采样绑定带注册 NVNtextureView
    uint8_t view_ok;               // 采样视图能按 Vulkan 约束建出来（plan.ok）
    uint8_t override_ok;           // 无格式/通道/深度模板重写（修掉这些才能直接复用快照默认视图）
    uint32_t view_type;            // 采样视图的 VkImageViewType
    uint32_t view_base_level, view_nlevels, view_base_layer, view_nlayers;
};
enum { NVN_VK_FB_BIND_NONE = 0, NVN_VK_FB_BIND_SUBSTITUTE = 1, NVN_VK_FB_BIND_LEAVE = 2,
       NVN_VK_FB_BIND_ISO = 3 };
int nvn_vk_feedback_bind_plan(const struct nvn_vk_feedback_bind *in, char *why, size_t why_size);
// 被采样视图是否覆盖多个子资源（与 bind_plan 的 ISO 分支同族判定，但不看附件覆盖关系——
// 快照创建时附件的 mip/层还没定）。负控 debug.s2a.no_fb_iso=1 用它做粒度：多子资源视图
// 连快照都不建、不预拷、不加屏障，纯直通活绑定——注意这比修复前路径更严（旧路径仍
// 建单层快照+预拷+屏障、只是不绑定），是验证「多层视图活绑定 UB」机制的最强负控；
// 单子资源视图照旧单层快照（修复前那部分本来就是正确路径）。
int nvn_vk_feedback_view_multi(const struct nvn_vk_feedback_bind *in);

// 同构反馈快照计划：快照照被采样底图的形状建（同 level×layer/depth、同格式），开遍前把
// 每个子资源的开遍前内容逐份拷进快照，绑定视图与采样视图同形——着色器读到的就是
// “本笔开始前的附件内容”，与苹果路线 fb_prepare ④ 的整视图快照同口径（Metal 的①视图收窄
// 在 Vulkan 不可移植：同图任一子资源是附件时采样仍属未定义，只能靠换一张图）。
struct nvn_vk_feedback_iso {
    uint8_t is_3d;               // 1 = 底图 3D：快照是 3D 图像，拷贝按 level 整深度走
    uint32_t w, h, depth;        // level 0 尺寸
    uint32_t levels, layers;     // 子资源覆盖（3D 时 layers 恒 1，切片数 = depth）
    uint32_t n_copies;           // 拷贝份数：levels ×（is_3d ? 1 : layers）
    uint64_t est_bytes;          // 全部子资源字节和（含 bpp），预算/上限判定用
};
// 计划：输出快照形状与拷贝份数；bytes_per_texel 由调用方按 VkFormat 查表给（本层不藏格式表）。
// cap_bytes 是单张快照的拷贝成本上限（超了拒绝，与苹果 ④ 的 64MB 上限同思路）。返回 0 成立。
int nvn_vk_feedback_iso_plan(uint32_t is_3d, uint32_t w, uint32_t h, uint32_t depth,
                             uint32_t levels, uint32_t layers, uint32_t bytes_per_texel,
                             uint64_t cap_bytes,
                             struct nvn_vk_feedback_iso *out, char *why, size_t why_size);
// 第 n 份拷贝的 VkImageCopy（纯构造）：dst 与 src 同形，(level, layer) 一一映射；
// 3D 一个 level 一份拷贝，深度进 extent.depth（Vulkan 的 3D 子资源 layerCount 恒 1）。
// n 按 (level * layers + layer) 展开；3D 时 n 就是 level。
int nvn_vk_feedback_iso_copy(const struct nvn_vk_feedback_iso *iso, uint32_t n,
                             VkImageCopy *out, char *why, size_t why_size);

// BC/块压缩纹理作为渲染目标（经“块=像素”的无压缩 UI 视图）：
//   0100F2C0115B6000 把一张 32x32x32 的 RGTC1(BC4) 3D 纹理每笔画一个深度切片，附件视图用 RGBA16UI 之类
//   无压缩格式覆盖——一个视图像素 = 一个 8 字节压缩块（GPU 上直接编码 BC4）。Vulkan 既不能把
//   压缩格式当颜色附件，也不能建压缩↔无压缩视图，所以渲染到等尺寸影子（bw×bh 像素），pass
//   前后用 buffer 往返拷块：压缩格式没有 COLOR_ATTACHMENT 特性、不能建无压缩视图，而
//   image→image 拷贝要求格式兼容（压缩与无压缩不兼容），所以 buffer 往返是 Vulkan 的唯一通路。
//   只有“视图每像素字节数 == 底图每块字节数”的覆盖才算块编码；其它压缩附件明确拒绝。
struct nvn_vk_bcrt_plan {
    uint32_t block_bytes;   // 每个压缩块的字节数（BC1/BC4 = 8，BC3/BC5 = 16）
    uint32_t side;          // 块边长（BC 固定 4）
    uint32_t width, height; // 被画的 mip/平面尺寸（纹理元素）
    uint32_t bw, bh;        // 影子尺寸 = ceil(width/side) × ceil(height/side)
    uint32_t level;         // 子资源：mip
    uint32_t layer;         // 子资源：层（3D 时是深度切片）
    uint8_t is_3d;
};

// 判定：底图是 BC 压缩格式、视图格式是无压缩颜色格式、且视图每像素字节数 == 块字节数。
// 返回 0 表示可走影子；why 给出准确原因（格式/尺寸不匹配、视图缺失、非 UI/整型等）。
int nvn_vk_bcrt_plan_make(VkFormat base_vk_format, VkFormat view_vk_format, uint32_t base_width,
                          uint32_t base_height, uint32_t level, uint32_t layer, uint8_t is_3d,
                          struct nvn_vk_bcrt_plan *out, char *why, size_t why_size);

// 仅做“格式层”判定：底图是 BC 压缩、视图是无压缩颜色格式、且视图每像素字节数 == 块字节数。
// 编译事实（fragmentOutputTypes）与附件计划共用这一条，保证“能编译”和“能挂附件”是同一个判据。
// 返回 0 表示可做块编码，*out 给视图 VkFormat；否则写 why。
int nvn_vk_bcrt_view_format(VkFormat base_vk_format, VkFormat view_vk_format, VkFormat *out,
                            char *why, size_t why_size);

// 往返拷贝区域（纯构造，不调 Vulkan）：底图（压缩，整 mip/平面）↔ buffer ↔ 影子（bw×bh 像素）。
// 三个 copy 都是紧打包（bufferRowLength=0）：压缩块行字节数 = bw*block_bytes = 影子行字节数。
void nvn_vk_bcrt_img_to_buf(const struct nvn_vk_bcrt_plan *p, VkBufferImageCopy *out);
void nvn_vk_bcrt_buf_to_shadow(const struct nvn_vk_bcrt_plan *p, VkBufferImageCopy *out);
void nvn_vk_bcrt_shadow_to_buf(const struct nvn_vk_bcrt_plan *p, VkBufferImageCopy *out);
void nvn_vk_bcrt_buf_to_img(const struct nvn_vk_bcrt_plan *p, VkBufferImageCopy *out);

#ifdef __cplusplus
}
#endif
