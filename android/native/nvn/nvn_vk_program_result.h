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
};

struct fd_binding {
    int set, binding, kind;   // kind: 0 uniform/support, 1 storage, 2 texture, 3 image
    int slot;
    uint16_t count;           // 描述符数组长度（不是数组为 1）。kind==2 且 count>1 = 从句柄表第 slot 项起的贴图数组
    char stage[16];
    // 下面这些只由 program 内容决定，加载时算一次；逐笔 draw 不再做 strcmp / 除法。
    int8_t stage_idx;         // 0=vs 1=fs 2=gs 3=tcs 4=tes 5=cs，-1 = 其它/未知
    int16_t tex_slot;         // 组合槽 0..31 或分离贴图槽 128..255；-1 = 未解析
    int16_t samp_slot;        // -1 = 组合句柄；0..127 = 分离采样器来源槽
    uint8_t texel_buffer;     // 真实 SPIR-V 证明该 texture 是只读 Buffer；不是普通采样图像
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
void nvn_vk_program_free(struct fd_program *program);
int nvn_vk_program_vertex_storage_safe(const uint8_t *bytes, size_t length, char *why, size_t why_size);
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
};
#ifdef __cplusplus
extern "C" {
#endif
int nvn_vk_program_parse_compute(const uint8_t *bytes, size_t length,
    struct nvn_vk_compute_program *out, char *why, size_t why_size);
void nvn_vk_program_free_compute(struct nvn_vk_compute_program *program);
#ifdef __cplusplus
}
#endif
