// C3：NVN 共享命令记录 → native Vulkan 回放的最小执行器。
// 记录布局来自 graphics/nvn/gfx/nvn/nvn_obj.h（与 hle_nvn.c 写入、nvn_mtl.m 读取同一份）。
// 首版实现控制流、CPU 可见的 buffer 记录与整 mip/全通道 CLEAR_TEX；其余 opcode 明确 UNSUPPORTED。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "vk_device.h"
#include "nvn_obj.h"   // view_t：附件 mip/层/格式覆盖的真实记录布局

#ifdef __cplusplus
extern "C" {
#endif

struct s2a_replay_texture {
    const void *key;          // 共享记录里的 const tex_t*，只作身份，不在此层解引用
    VkImage image;
    VkFormat format;
    uint32_t width, height, depth;
    uint64_t serial;          // 前端 tex_t.serial；0 = 仅按 key 匹配（合成测试）
    uint32_t levels, layers;  // 图像的真实 mip/层数（0 = 未知，调用方按视图范围谨慎使用）
};

struct s2a_replay_draw {
    const void *program_key;   // 共享记录里的 const prog_t*
    const void *rt_key;        // 当前第一个颜色目标的 const tex_t*
    VkPipeline pipeline;
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkExtent2D extent;
    VkPipelineLayout layout;
    VkDescriptorSet descriptor;
    VkBuffer vertex_buffer;
    VkDeviceSize vertex_offset;
};

struct s2a_replay_dispatch {
    const void *program_key;   // 共享记录里的 const prog_t*
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSet descriptor;
};

struct s2a_record_memory {
    void *opaque;
    int (*read)(void *opaque, uint64_t address, void *dst, size_t bytes);
    int (*write)(void *opaque, uint64_t address, const void *src, size_t bytes);
};

enum s2a_record_code {
    S2A_RECORD_OK = 0,
    S2A_RECORD_ARGUMENT = -1,
    S2A_RECORD_INVALID = -2,
    S2A_RECORD_UNSUPPORTED = -3,
    S2A_RECORD_GPU = -4,
    S2A_RECORD_MEMORY = -5,
    S2A_RECORD_DEPTH = -6,
    S2A_RECORD_LIMIT = -7,
};

struct s2a_record_result {
    int code;
    uint32_t opcode;
    uint64_t executed;
    char why[160];
};

// begin..end 必须覆盖同一条命令流的所有 CALL/JUMP 目标。任何越界/未对齐/未知记录都失败。
// 单包执行：把一条 op_hdr 记录补一个 OP_END 后交给回放，供适配层 mtl_exec 使用。
int s2a_record_exec_one(struct s2a_vk_device *device, const uint8_t *packet, size_t len,
                        const struct s2a_record_memory *memory, struct s2a_record_result *result);

// 静态纹理表之外的动态解析（适配层用）：记录里的 const tex_t* 不在表内时回调一次；
// 解析成功返回 1 并写 *out（key 必须回填成调用方给的 key）。线程局部：只对当前线程的回放生效。
typedef int (*s2a_record_tex_resolve)(void *opaque, const void *key, struct s2a_replay_texture *out);
void s2a_record_set_tex_resolver(s2a_record_tex_resolve resolve, void *opaque);

// 绘制接线：注册后，OP_DRAW / OP_DRAW_IDX 把已绑定的 prog 交给后端自己建管线并绘制。
// 返回 0 = 已画；非 0 = 失败码（result 里写明原因）。未注册时仍走静态 draw 表（测试路径）。
// 绘制快照：只放“此刻真实成立”的状态（绑定、附件、顶点流、视口、绘制参数）。
// 不是默认值，也不是从 prog 里猜的；后端据此形成编译请求/管线配方。
struct s2a_record_draw_state {
    const void *program;            // 绑定时的 prog_t*（可能已被游戏复用，只作身份参考）
    uint8_t keys[6][8];             // 绑定时的 stage key 快照（数组顺序）
    uint32_t stages;                // 绑定时的 NVN stage 掩码
    int indexed;
    uint32_t count, first, instances, base_vertex, base_instance;   // count<0 表示间接/未知
    // base_instance 是 NVN instance_first：Vulkan 的 firstInstance 必须用它，不能固定 0
    // （固定 0 会让实例化绘制读到错误的 per-instance 属性/instance id）。
    uint64_t index_address;                          // indexed 时索引缓冲地址（0 = 不知）
    uint32_t index_type;
    uint32_t rt_count;
    // 附件事实的来源与有效性（首错定位用，不参与默认值填充）：key==0 表示 SetRenderTargets
    // 该槽没有给纹理；obj_magic != 'TEX0' 表示记录下来的对象在回放时仍无效（快照丢失），
    // 此时 nvn_format/尺寸都不可信，不能当“合法未绑定槽”消费。
    struct { uint64_t key; VkFormat format; uint32_t nvn_format; uint32_t width, height; int has_view;
             uint32_t obj_magic; uint64_t obj_serial; uint64_t storage; uint32_t view_format; view_t view; } rt[8];
    uint64_t cmd_seq, rt_seq;        // 本笔 draw 的命令序号 / 当前颜色目标那次 SetRenderTargets 的序号
    int32_t vp_first, vp_n, sc_x, sc_y, sc_w, sc_h;
    uint8_t vp_seen, sc_seen;   // 0 = 该状态从未被该命令流设置（与显式设置成空区分开）
    float vp_x, vp_y, vp_w, vp_h, vp_near, vp_far;   // 视口 0 的真实值（NVN first..n 的第一项）
    int32_t tex_stage, tex_first, tex_n;
    int32_t vb_first, vb_n;
    uint64_t ubo_addr, ubo_size; int32_t ubo_stage, ubo_index;
    struct { uint64_t addr, size; } ssbo[6][16];              // NVN stages 0..5 (not packed shader indices)
    uint64_t ssbo_bound[6];                                 // addr != 0; unbind clears the slot bit
    // 编译请求需要的真实 GPU 事实（都是 NVN 命令流里实际发生过的，不是默认值）。
    uint32_t prim;                  // NVNdrawPrimitive
    uint32_t tex_pool_n, samp_pool_n;   // 已绑定描述符池的容量；0 = 没有池/未知
    int32_t tex_pool_base;              // 已绑定纹理池的全局描述符编号基址；-1 = 未知/没绑（句柄 id 已是绝对编号）
    int tf_enabled;                 // 变换反馈是否处于 Begin 状态
    uint32_t ubo_use[6];            // 各 NVN 阶段常量缓冲号掩码（翻译器 cbuf 位图，完整 32 位；>=29 号槽见 ubo_use_overflow）
    int ubo_use_overflow;           // 曾绑定过 >=29 号 cbuf：32 位掩码装不下，作为事实报出
    uint32_t vattr_fmt[32];         // 顶点属性格式（NVN 格式号；未绑定的槽位保持 NVN 初始状态 0）
    // 最近一次绑定的 NVN 状态对象原文（kind 0 blend / 1 color / 2 chmask / 3 ms / 4 poly / 5 ds）；
    // state_seen 的 bit 表示该 kind 至少被真实绑定过一次，未绑定≠已证认为默认值。
    uint8_t state_obj[6][0x18];
    uint8_t state_seen;
    // 按 NVNblendState.target（bit 0..2）分槽的混合状态原文：MRT 的每个颜色槽可以绑不同
    // 混合状态，state_obj[0] 只保留“最后一次绑定”，不足以逐槽建管线。blend_seen 的 bit i
    // 表示该槽至少真实绑定过一次；未绑定的槽不是默认值，由消费方按 NVN 默认语义处理。
    uint8_t blend_obj[8][0x18];
    uint8_t blend_seen;
    // 执行真实绘制需要的逐槽位真实状态（不是摘要）。
    struct { uint32_t format, offset, stream; } vattr[32];   // OP_BIND_VATTR 原样
    uint32_t vstream_stride[16], vstream_divisor[16];        // OP_BIND_VSTREAM
    uint64_t vbuf_addr[16], vbuf_size[16];                   // OP_BIND_VBUF（NVNbufferRange）
    struct { uint64_t addr, size; } ubo[6][16];              // 每 stage 每槽位最近一次绑定（size=0 未绑）
    uint64_t tex_handle[6][32];                              // 组合纹理句柄（纹理 id | 采样器 id << 20）
    uint8_t tex_present[6][32];
    uint64_t samp_handle[6][32];                             // 独立采样器句柄
    uint8_t samp_present[6][32];
    uint8_t c1[6][0x100];                                    // 运行时 c1（真实范围可能更长，超出部分截断）
    uint8_t c1_ok[6];
    uint64_t last_ub_addr[6][16];      // 该槽最近一次 UpdateUniformBuffer 的目标地址（0 = 没见过）
    uint64_t last_ub_bytes[6][16];
    uint32_t last_ub_count[6][16];
    uint64_t depth_key;                                      // 深度/模板目标（SetRenderTarget 的真实 depth 纹理快照）
    uint64_t depth_storage;                                  // 深度目标的存储身份（tex_t.mem；首用 steady 判定的第二键）
    uint8_t has_depth;
    uint8_t has_dview;                                       // SetRenderTargets 给了深度视图（mip/层/格式/面）
    view_t dview;                                            // 深度视图原样（附件消费用；未给时保持 0）
    uint32_t depth_magic, depth_nvn_format;                  // 深度快照对象 magic / NVN 格式（诊断）
    uint64_t depth_serial;
    float depth_clear;
    float stencil_clear;
    // ClearDepthStencil 按命令位置立即执行，所以到 draw 时没有待处理清除：两个掩码恒为 0
    // （附件 loadOp=LOAD）。保留字段是为了让消费者按“深度/模板两个独立平面”消费，而不是合并成一个标志。
    uint8_t depth_clear_mask;
    uint8_t stencil_clear_mask;
    uint8_t depth_cleared;
};

typedef int (*s2a_record_draw_fn)(struct s2a_vk_device *device, const struct s2a_record_draw_state *state,
                                  struct s2a_record_result *result, void *opaque);
void s2a_record_set_draw_builder(s2a_record_draw_fn draw, void *opaque);
// Compute 快照独立于 draw；keys 为 packed CS key + 5 个 inactive 零槽，
// ubo_use 按 NVN stage 编号（只有 [5] 活跃）。program 只作参考，serial 来自绑定记录。
// 未观察到的 pool/local memory 不等于零；消费方必须按 seen 标志拒绝缺失事实。
struct s2a_record_dispatch_state {
    const void *program;
    uint64_t program_serial, cmd_seq;
    uint8_t keys[6][8];
    uint32_t stages, groups[3];
    uint32_t tex_pool_n, samp_pool_n;
    int32_t tex_pool_base;              // 已绑定纹理池的全局描述符编号基址；-1 = 未知/没绑
    uint8_t tex_pool_seen, samp_pool_seen;
    uint32_t ubo_use[6];
    int ubo_use_overflow;
    uint32_t ubo_seen, ssbo_seen; // bit per slot: an explicit bind/unbind was observed
    struct { uint64_t addr, size; } ubo[16], ssbo[16];
    uint64_t tex_handle[32], samp_handle[32], image_handle[32];
    uint8_t tex_present[32], samp_present[32], image_present[32];
    uint32_t local_memory_bytes;
    uint8_t local_memory_seen;
};
#define S2A_RECORD_DISPATCH_MAX_SETS 8u
// Callback gets 0 = proven completed after COMPUTE_SHADER/SHADER_WRITE -> HOST/HOST_READ,
// -1 = never submitted, -2 = completion unknown. Non-coherent memory still needs invalidation.
// -2 MUST retain every referenced object; never recycle descriptors on this notification.
// Record retains owner/finish across -2 and calls finish(0) exactly once after that
// fence reports VK_SUCCESS and its command buffer is freed. Memory callback/opaque
// lifetimes must extend through this delayed finish. Unknown completion is not success.
/* Diagnostic facts never confer execution success. Unknown identity is explicit. */
enum s2a_compute_source { S2A_COMPUTE_UNKNOWN, S2A_COMPUTE_OFFLINE,
    S2A_COMPUTE_PRIVATE, S2A_COMPUTE_LIVE, S2A_COMPUTE_MISS, S2A_COMPUTE_SOURCE_COUNT };
struct s2a_compute_diagnostic {
    uint64_t session, generation;
    uint8_t identity[32], identity_valid;
    enum s2a_compute_source source;
};
enum s2a_compute_outcome { S2A_COMPUTE_BUILD_FAILED, S2A_COMPUTE_UNSUBMITTED,
    S2A_COMPUTE_COMPLETED, S2A_COMPUTE_COMPLETION_UNKNOWN, S2A_COMPUTE_COPYBACK_FAILED };
#define S2A_COMPUTE_REASON_SLOTS 16
struct s2a_record_compute_stats {
    uint64_t session, generation, dispatches, sources[S2A_COMPUTE_SOURCE_COUNT];
    uint64_t submitted, completed, unknown_completion, unsubmitted, build_failed, copyback_failed;
    uint64_t trace_lines, reason_overflow, stale_events;
    /* Historical field name; contains fixed reason codes only, never raw why. */
    struct { char why[192]; uint64_t count; } reasons[S2A_COMPUTE_REASON_SLOTS];
    int active, trace;
};
/* Call begin/end only at the actual session barriers, never per submit/reset.
 * Trace is an explicit contract: a platform caller must first prove debug permission. */
int s2a_record_compute_session_begin(uint64_t session, uint64_t generation);
int s2a_record_compute_trace(int enabled);
void s2a_record_compute_snapshot(struct s2a_record_compute_stats *out);
int s2a_record_compute_session_end(void);
void s2a_record_compute_note(const struct s2a_compute_diagnostic *diagnostic,
    enum s2a_compute_outcome outcome, const char *why);
struct s2a_record_dispatch_output {
    struct s2a_compute_diagnostic diagnostic;
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSet sets[S2A_RECORD_DISPATCH_MAX_SETS];
    uint32_t set_count;
    // 反射出来的采样纹理绑定数；>0 时派发前必须把此前所有 GPU 写发布给 CS 采样读
    // （图形路径的 pass/上传屏障 dstStage 不含 COMPUTE_SHADER，覆盖不到这里）。
    uint32_t sampled_images;
    void *owner;
    int (*finish)(struct s2a_vk_device *device, void *owner, int completion);
};
typedef int (*s2a_record_dispatch_fn)(struct s2a_vk_device *device,
    const struct s2a_record_dispatch_state *state, const struct s2a_record_memory *memory,
    struct s2a_record_dispatch_output *output, struct s2a_record_result *result, void *opaque);
void s2a_record_set_dispatch_builder(s2a_record_dispatch_fn dispatch, void *opaque);

// 复位跨包保留的 GPU 绑定状态（一次提交开始时调用）。
void s2a_record_reset_state(void);

// GPU 对象与一次性提交的记账（诊断/验收用，不改变执行语义）。创建/销毁逐类计数，正常完成后
// 两者相等；保留账本里是“已提交但没有完成证明”的对象（Vulkan 禁止释放/复用）。
struct s2a_record_gpu_stats {
    uint64_t cmd_created, cmd_freed;
    uint64_t fence_created, fence_freed;
    uint64_t buffer_created, buffer_freed;
    uint64_t memory_created, memory_freed;
    uint64_t image_created, image_freed;
    uint64_t submit_completed;     // 已提交且拿到完成证明
    uint64_t submit_unsubmitted;   // 没有提交成功（命令从未执行）
    uint64_t submit_incomplete;    // 已提交但完成未知（对象已转入保留账本）
    uint64_t retained_entries;     // 账本当前条目数
    uint64_t retained_overflow;    // 账本已满、仅保留未记账的次数
};
void s2a_record_gpu_stats(struct s2a_record_gpu_stats *out);
// 设备销毁前结算保留账本：对每条做有界完成检查（预算 timeout_ns 总量）。拿到完成的条目显式销毁
// （精确回收命令缓冲/辅助资源/fence）；没拿到的保留并计数，随设备销毁（不伪造完成）。
// 返回显式回收的条目数；*left_out = 仍然完成未知的条目数。
int s2a_record_release_retained(struct s2a_vk_device *d, uint64_t timeout_ns, uint32_t *left_out);
// 首个 GPU 错误原文（不覆盖）；没有错误返回空串。
const char *s2a_record_first_gpu_error(void);
// 诊断/测试注入：1 = 真实提交后按“完成未知”处理（跳过等待，对象进保留账本）；
// 2 = 假装提交失败（不调用 vkQueueSubmit）。只用于验收失败/退休路径，默认 0 = 正常语义。
void s2a_record_set_submit_fault(int mode);

int s2a_record_replay(struct s2a_vk_device *device, const uint8_t *begin, const uint8_t *end,
                      const struct s2a_record_memory *memory,
                      const struct s2a_replay_texture *textures, size_t texture_count,
                      const struct s2a_replay_draw *draws, size_t draw_count,
                      const struct s2a_replay_dispatch *dispatches, size_t dispatch_count,
                      struct s2a_record_result *result);

#ifdef __cplusplus
}
#endif
