// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once
#include <stddef.h>
#include <stdint.h>

/* 结果 bindings 条数的合同上限——按 producer 合同推导，不是拍出来的常量：
 * DirectAllocator/DirectProfile 每阶段至多 18 uniform（NVN/Maxwell cbuf 槽）+
 * 16 storage（存储描述符表项）+ 64 texture（执行层组合纹理上限）+ 16 image；
 * DirectShaderCompiler 的图形程序固定 vs+fs+gs 三阶段，再加 1 条全程序共享的
 * support binding：1 + 3×(18+16+64+16) = 343。compute 程序仅 cs 一阶段
 * （1+18+16=35），同一上限同样覆盖。
 * 超过此数的结果已违反 s2a-direct-program-v1 合同或已损坏——照旧准确失败，
 * 绝不静默截断。设备端能否容纳另由 maxDescriptorSet* / maxPerStageDescriptor*
 * 校验把关：compute 侧在 nvn_vk_compute.c 建对象前显式比对设备限额（why =
 * "compute descriptors exceed device limits"）；图形侧当前没有消费点，靠
 * vkCreateDescriptorSetLayout 返回 VK_ERROR 兜底（准确报红但不可定位，显式比对待补）。 */
#define FD_STAGE_MAX_UNIFORMS 18
#define FD_STAGE_MAX_STORAGE 16
#define FD_STAGE_MAX_TEXTURES 64
#define FD_STAGE_MAX_IMAGES 16
/* Replay's RP_TEXARR_MAX; validate before the 16-bit backend layout count. */
#define FD_MAX_ARRAY_ELEMENTS 64
/* Parsed layout needs more descriptors than the replay layer can bind per array. */
#define NVK_PROGRAM_ARRAY_UNSUPPORTED (-2)
#define FD_GRAPHICS_STAGE_COUNT 3
#define FD_MAX_BINDINGS (1 + FD_GRAPHICS_STAGE_COUNT * \
    (FD_STAGE_MAX_UNIFORMS + FD_STAGE_MAX_STORAGE + FD_STAGE_MAX_TEXTURES + FD_STAGE_MAX_IMAGES))
#define FD_MAX_STAGES 6
#define SUPPORT_DEFAULT_BYTES 2608u
struct fd_stage {
    int present;
    uint8_t *spirv;
    size_t spirv_bytes;
    uint32_t attr_mask;
    uint32_t fragment_output_map;   // 片元阶段：每个颜色目标 4 位（RGBA）= 着色器写了哪些输出分量；读不到为全 1
    int binst_unread;               // 图形阶段：结果的 c0Reads 证明不读 c0 + 0x644（本笔 base instance）；0 = 读或未知
    int binst_unread_static;        // 不做动态下标定界的证明；缺字段 / null = 未知
    int binst_static_present;       // 新结果带静态读集（null 也算有字段）
};
static inline int nvn_vk_binst_unread(const struct fd_stage *st, int bound_on) {
    return bound_on ? st->binst_unread : (st->binst_static_present && st->binst_unread_static);
}
#define FD_C0_BASE_INSTANCE 0x644u   // 驱动常量段里图形阶段的 base instance 字（nvn_vk_mrt.h、回放 fill_graphics_drv 同位）

/* 句柄放在游戏用户常量缓冲里的一个 32 位字（v1 uboHandle / sampler.handle；iOS nvn_refl.h NVN_RB_TEXU / SAMPU / TBUFU 的 reg / off / sid / pair）：
 * 用户常量槽 slot 的字节 offset 处；sid = 这个字是采样器号（读出后左移 20）；pair = 同一句柄的另一半（pair_sid：那半是采样器号字，左移 20 后相或）。 */
struct fd_ubo_word { uint8_t slot, sid, pair, pair_slot, pair_sid; uint16_t offset, pair_offset; };
enum { FD_SAMP_NONE = 0, FD_SAMP_UBO, FD_SAMP_ID, FD_SAMP_SLOT };   // fd_binding.samp_src

struct fd_binding {
    int set, binding, kind;   // kind: 0 uniform/support, 1 storage, 2 texture, 3 image, 4 计算共享内存溢出缓冲（spill）
    int slot;
    uint16_t count;           // 描述符数组长度（不是数组为 1）。kind==2 且 count>1 = 从句柄表第 slot 项起的贴图数组
    char stage[16];
    // 下面这些只由 program 内容决定，加载时算一次；逐笔 draw 不再做 strcmp / 除法。
    int8_t stage_idx;         // 0=vs 1=fs 2=gs 3=tcs 4=tes 5=cs，-1 = 其它/未知
    int16_t tex_slot;         // 组合槽 0..31 或分离贴图槽 128..255；-1 = 未解析。kind==3（存储图像，图形与计算阶段同一形态）：NVN 图像槽 0..7 = (句柄字 − 72) / 2，
                              //   图形阶段只在 count == 1 且字为 72..86 偶数时给，其余保持 -1（layout_ent 具名拒，不猜槽位）
    int16_t samp_slot;        // -1 = 组合句柄；0..127 = 分离采样器来源槽
    uint8_t texel_buffer;     // kind==2：真实 SPIR-V 证明该 texture 是只读 Buffer（不是普通采样图像）；kind==3：存储图像是 imageBuffer（Dim=Buffer，存储纹素缓冲）
    // kind==2 且 tex_ubo = 1：贴图句柄不在槽里，在用户常量缓冲 tex_u 处（_b.json tex_ubo / tbuf_ubo）。此时 tex_slot = -1。
    //   samp_src = 组合采样的采样器从哪来：FD_SAMP_UBO = 常量缓冲 samp_u 处的字；FD_SAMP_ID = 常量采样器号 samp_val；
    //   FD_SAMP_SLOT = NVN 采样器槽 samp_val；FD_SAMP_NONE = 着色器没有采样器半边
    // kind==2 且 tex_ubo = 0、tex_slot >= 128、samp_src = FD_SAMP_ID（#1594，v2 的 constSampler）：贴图字在 c2 分离贴图表，
    //   采样器池下标 samp_val（0..4095）是着色器里的立即数；samp_slot 仍为 -1。回放的采样器号 = 贴图字高 12 位 | 池下标（Ryujinx ReadPackedId，字高位为 0 时就是池下标）。
    //   计算阶段（#1594 片 7）也接这一种形态（samp_pool = 1）；片 10 起计算阶段也接游戏 UBO 来源的类型 3 句柄（下一条的形态：tex_ubo = 1，同图形）。
    // kind==2 且 pool != 0：整池数组（追不到槽的句柄回退，编译结果 "pool"）。1 = 贴图池（只是贴图半边）、2 = 采样器池（只是采样器半边），
    //   count = 池长（2..65534，不受 FD_MAX_ARRAY_ELEMENTS 限制），slot / tex_slot / samp_slot 都没有。宿主映射到 NVN_RB_TEXP / NVN_RB_SAMPP。
    //   图形阶段与计算阶段（stage "cs"，stage_idx 5）同一份合同
    uint8_t pool;
    // kind==2 且 tex_ubo = 1、samp_src = FD_SAMP_ID、samp_pool = 1（#1594 片 3，v2 的 constSampler 配平铺的 uboSlot / uboOffset）：贴图字在游戏 UBO 的 tex_u 处，
    //   采样器池下标 samp_val（0..4095）同上是着色器里的立即数，回放按「（贴图字高 12 位 | 池下标）+ 当前采样器池起点」取采样器。
    //   samp_pool = 0 的 FD_SAMP_ID（离线库 {"source":"id"}）是宿主登记的采样器号原值（iOS 口径），不加池起点——两种口径不混。
    uint8_t tex_ubo, samp_src;
    uint16_t samp_val;
    uint8_t samp_pool;
    struct fd_ubo_word tex_u, samp_u;
    int16_t ubo_slot;         // kind==0 的 NVN 用户槽（cbuf - 3，clamp 到 0）
    uint8_t is_support;       // kind==0 且 stage=="program"（SupportBuffer）
    // kind==1 的地址来源（编译期从 ShaderProgramInfo.SBuffers 解析）：
    // 0 = 没有映射；1 = NVN storage 槽；2 = 用户 UBO 槽内的 SbDescriptor；
    // 3 = 间接 UBO 指针（只有地址、没有 extent，运行时必须报红）。
    int32_t storage_cb_slot, storage_cb_offset;
    int8_t storage_kind, storage_slot;
    uint8_t storage_writable;
};

struct fd_program {
    struct fd_stage stage[FD_MAX_STAGES];
    struct fd_binding bindings[FD_MAX_BINDINGS];
    int binding_count;
    uint32_t support_bytes;
};

#ifdef __cplusplus
extern "C" {
#endif
int nvn_vk_program_graphics_stage_index(const char *name);
int nvn_vk_program_parse_graphics(const uint8_t *bytes, size_t length, struct fd_program *out, char *why, size_t why_size);
// 同上，allow_vs_only = 1 时也接受只有顶点阶段的程序（只写深度的绘制；新回放层用）。
int nvn_vk_program_parse_graphics_stages(const uint8_t *bytes, size_t length, struct fd_program *out, int allow_vs_only, char *why, size_t why_size);
/* Live provider adapters also admit the negotiated binary frame. */
int nvn_vk_program_parse_graphics_live(const uint8_t *bytes, size_t length, struct fd_program *out, int allow_vs_only, char *why, size_t why_size);
void nvn_vk_program_free(struct fd_program *program);
/* 两段着色器接口对不对得上（出包预编库按阶段分别取、拼成一份程序，拼时不过链接）：
 *   上一段（顶点，或几何）带 Location 的 Output 位置集合，必须包含片元带 Location 的 Input 位置集合。
 *   返回 1 = 对得上，或认不全不下结论（有不带 Location 的用户接口变量、位置 ≥ 64、数组 / 矩阵 / 结构体接口、读不了 SPIR-V）；
 *   0 = 片元确定读了上一段没写的位置，*missing 回填这些位置（位 i = Location i）。驱动按规范拒建这种管线（Adreno：input v_locN not declared）。 */
int nvn_vk_spirv_link_ok(const uint8_t *producer, size_t producer_bytes, const uint8_t *fragment, size_t fragment_bytes, uint64_t *missing);
#ifdef __cplusplus
}
#endif

/* Compute keeps its own stage layout; graphics stage indices remain unchanged. */
enum nvn_vk_storage_mapping { NVK_STORAGE_UNKNOWN, NVK_STORAGE_NVN, NVK_STORAGE_UBO_PTR, NVK_STORAGE_UBO_ADDRESS };
struct nvn_vk_compute_storage {
    uint32_t cb_slot, cb_offset; /* source constant buffer and byte offset */
    int writable; /* proven BufferUsageFlags.Write */
    enum nvn_vk_storage_mapping mapping;
    uint32_t index, offset; /* proven NVN slot / UBO slot and optional byte offset */
};
#define NVK_COMPUTE_C0_MAX_READS 256
#define NVK_PROGRAM_RESULT_MAX_BYTES (64u * 1024u * 1024u)   // 编译结果文档上限：同跨局程序缓存单条（program_cache.c PCACHE_MAX_PAYLOAD）
#define NVK_COMPUTE_C0_BYTES 0x410u
struct nvn_vk_c0_read { uint32_t offset, size; };
struct nvn_vk_support_scale { uint32_t offset, stride, count, element_bytes; };
struct nvn_vk_compute_program {
    struct nvn_vk_c0_read support_reads[NVK_COMPUTE_C0_MAX_READS];
    uint32_t support_read_count;
    // 编译器静态分析给不出 supportReads（JSON null）时为 1：运行时只能按“读的都是恒等值字段”
    // 填满整个 SupportBuffer（见 nvn_vk_compute.c 的说明），不能当成“没有读”。
    int support_reads_unknown;
    struct nvn_vk_support_scale support_scales[2];
    struct nvn_vk_c0_read c0_reads[NVK_COMPUTE_C0_MAX_READS];
    uint32_t c0_read_count;
    // 编译器给不出 c0Reads（JSON null）时为 1：按完整驱动布局填，而不是当成“没有读”。
    int c0_reads_unknown;
    int has_unaligned_storage_buffer; /* mandatory boolean in compute content */
    struct fd_stage stage;
    struct nvn_vk_compute_storage storage[FD_MAX_BINDINGS];
    struct fd_binding bindings[FD_MAX_BINDINGS];
    int binding_count;
    uint32_t support_bytes;
    uint32_t spill_bytes;   // 共享内存溢出缓冲每个线程组的字节数（结果里 kind = spill 的绑定）；0 = 没有
};
#ifdef __cplusplus
extern "C" {
#endif
int nvn_vk_program_parse_compute(const uint8_t *bytes, size_t length,
    struct nvn_vk_compute_program *out, char *why, size_t why_size);
int nvn_vk_program_parse_compute_live(const uint8_t *bytes, size_t length,
    struct nvn_vk_compute_program *out, char *why, size_t why_size);
void nvn_vk_program_free_compute(struct nvn_vk_compute_program *program);
#ifdef __cplusplus
}
#endif
