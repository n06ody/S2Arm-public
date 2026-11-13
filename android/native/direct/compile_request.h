// 真实 Draw 快照 → s2a-direct-compile-v1 请求 → 已绑定 compile_fn → 结果复制回后端自有内存。
// 这里只做三件事：按契约字段生成严格 JSON、把请求交给统一编译器、把 emit 结果复制到 native 内存。
// 没有任何默认答案：每个字段要么来自真实快照（source_store / Draw 快照 / provider profile），
// 要么不写进请求并在 gaps 里写明“需要哪个来源”。
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// provider 在 Prepare 时合成、在 Activate 后可供 Draw 读取的请求身份半（profile 原文 + session/generation）。
struct s2a_direct_compile_context {
    uint64_t session;
    uint64_t generation;
    const char *profile_json;
    size_t profile_bytes;
    uint32_t direct_abi;
    const char *translator_closure;
    const char *profile_hash;       // 本局 effective profile（"sha256-"+64hex），离线库目录用
    int offline_profile_available;  // 描述符明确列出本局 profile，才允许读取离线库
    int shader_draw_parameters;     // 设备是否真的把 base vertex/instance 写进 cbuf 0
    // 设备实测的 VkPhysicalDeviceLimits::minStorageBufferOffsetAlignment（0 = 没有来源）。
    // graphicsState.hasUnalignedStorageBuffer 要按绑定地址对该值的对齐推出；
    // 0 时不能猜 false，调用方必须把该字段记为缺口。
    uint32_t storage_buffer_offset_alignment;
};
int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *out);

struct s2a_compile_stage_src {
    const char *stage;              // "vs" / "fs"（NVN 阶段序号 0 / 1）
    const uint8_t *code;
    uint32_t code_bytes;
    const uint8_t *control;
    uint32_t control_bytes;
    const uint8_t *c1;              // NULL/0 = 本笔没有 C1 来源（BRX 着色器会在编译期明确报缺失）
    uint32_t c1_bytes;
    uint32_t constant_buffer_use;   // 该阶段真实绑定过的 guest cbuf 掩码
};

struct s2a_compile_graphics_src {
    const char *topology;           // 已由 NVN 绘制图元转换（NULL = 没有来源）
    uint32_t tess_mode;             // 控制段 +0x744 的真实值
    int has_tess_mode;
    // 颜色附件真实格式（VkFormat 对应 AttributeType 名）：0 = 该目标未绑附件
    const char *fragment_output_types[8];   // 8 个颜色目标；未绑定目标用 NVN 初始格式（NONE→Float）
    const char *attribute_types[32];        // NVN 顶点属性格式 → AttributeType 名
    int rt_count;
    int constant_buffer_use_overflow;   // 曾绑定 >=29 号 cbuf：32 位掩码装不下（观测事实，不再当缺口）
    // 以下每组都有 has_*：只有真实状态存在时才写进请求；没有来源的组只记缺口。
    int has_alpha_to_coverage, alpha_to_coverage_enable, alpha_to_coverage_dither;
    int has_alpha_test;
    const char *alpha_test_compare;     // Never/Less/Equal/LessOrEqual/Greater/NotEqual/GreaterOrEqual/Always
    int has_blend, dual_source_blend;
    int has_depth_mode, depth_mode;     // 1 = near −w（消费端 GpuGraphicsState.DepthMode：true = MinusOneToOne）
    int has_y_negate, y_negate;
    int has_origin, origin_upper_left;
    int has_halve_primitive_id, halve_primitive_id;
    int has_cb_draw_params, cb_draw_params;
    int has_point_size, program_point_size_enable;
    float point_size;
    int has_alpha_test_reference;
    float alpha_test_reference;
    int has_unaligned_ssbo, unaligned_ssbo;
    int has_geometry_shader;
    int host_supports_quads;
    // 由 NVN API 面确定（不是默认值）：NVN 没有强制 early-Z 的入口，也没有视口变换开关。
    int early_z_force;
    int viewport_transform_disable;
};

// F4.2B：program.textures[] 的一条真实事实（来自 Draw 快照的真实绑定 + view/描述）。
// 只有（stage, handle, cbufSlot）完全对得上编译器查询时才写入；cbufSlot = -1 表示组合纹理。
struct s2a_compile_texture_src {
    const char *stage;          // "vs" / "fs"
    uint32_t handle;            // cbuf 表里的 HandleIndex
    int cbuf_slot;              // -1 = 组合纹理；2 = c2 的分离纹理来源
    const char *sampler_type;   // "2d" / "2d-array" / "cube" ...
    const char *format;         // 编译器 TextureFormat 名
    int is_buffer;
    int normalized;
    int separate;               // 事实来自分离纹理槽，而非组合槽
    uint32_t array_length;
};

// Three graphics stages, each with 32 combined and 128 separate texture slots.
#define S2A_COMPILE_TEXTURE_MAX 480

struct s2a_compile_request_input {
    uint64_t session;
    uint64_t generation;
    const char *request_id;
    const char *profile_json;       // provider 合成的 enabled profile，原样放进请求
    size_t profile_bytes;
    int direct_abi;
    const char *translator_closure;
    struct s2a_compile_stage_src stages[3];
    int stage_count;
    struct s2a_compile_graphics_src gfx;
    struct s2a_compile_texture_src textures[S2A_COMPILE_TEXTURE_MAX];   // 真实绑定的纹理事实；0 条 = 没有可用来源
    int texture_count;
    uint32_t texture_array_length;  // 真实纹理描述符池容量；0 = 未知
    uint32_t sampler_array_length;  // 真实采样器描述符池容量；0 = 未知
    int transform_feedback_enabled;
    // 驱动的贴图句柄表（常量缓冲 2）有多少个 8 字节项；0 = 不给（请求里不出 textureBufferLengths）。
    // 着色器按运行时下标取句柄时，翻译器把它变成长度为这个数的贴图数组（BindlessToArray）。
    uint32_t handle_table_entries;
};

struct s2a_compile_outcome {
    int32_t rc;                     // 0 = 编译器成功且 emit 已复制；负码 = s2a_direct_result
    uint8_t *bytes;                 // malloc 的后端自有内存；调用方负责 free
    size_t bytes_len;
    char summary[512];              // 结果身份/内容摘要（失败时写编译器的返回事实）
};

// 生成请求 JSON。成功返回 0 并把 malloc 的 JSON 写进 *json。字段缺真实来源时**不写**该字段，
// 并把“字段 ← 需要的来源”逐项追加到 gaps（可为 NULL）；请求仍交给编译器，由它给出权威的第一失败点。
int s2a_compile_request_build(const struct s2a_compile_request_input *in,
                              char **json, size_t *json_len, char *gaps, size_t gaps_size);

// 经已绑定的 s2a_direct_compile_fn 同步执行；emit 结果立即复制到 malloc 内存（不写文件）。
// 返回 0 表示结果已复制；非 0 表示没拿到结果（此时 outcome->rc 是 compile_fn 的返回码）。
int s2a_compile_request_execute(uint64_t session, uint64_t generation, const char *json, size_t json_len,
                                struct s2a_compile_outcome *out);

void s2a_compile_outcome_free(struct s2a_compile_outcome *out);

/* Internal diagnostic ownership only; not part of s2a_direct's managed ABI.
 * build -> original allocation take -> execute is the supported graphics path.
 * A byte-identical copy is not an original allocation. Missing hooks/parser,
 * capacity, ambiguity and cancellation only disable diagnostics. */
struct s2a_compile_diag_attempt;
struct s2a_compile_diag_attempt *s2a_compile_diag_take(
    uint64_t session, uint64_t generation, const char *json, size_t len);
void s2a_compile_diag_retain(struct s2a_compile_diag_attempt *attempt);
/* Lexical worker callback scope, including NULL attempts. No content lookup. */
void s2a_compile_diag_enter(struct s2a_compile_diag_attempt *attempt,
    uint64_t session, uint64_t generation, const char *job_json, size_t len);
void s2a_compile_diag_leave(void);
/* finish/release may take producer locks and must run outside the worker lock.
 * abandon only revokes atomically, including the capsule, and never blocks.
 * returned transfers the current invocation's receipt to caller TLS; NULL
 * clears it. take always clears an older returned receipt before validation. */
void s2a_compile_diag_finish(struct s2a_compile_diag_attempt *attempt, int accepted);
void s2a_compile_diag_abandon(struct s2a_compile_diag_attempt *attempt, int reason);
void s2a_compile_diag_returned(struct s2a_compile_diag_attempt *attempt);
void s2a_compile_diag_release(struct s2a_compile_diag_attempt *attempt);

#ifdef __cplusplus
}
#endif
