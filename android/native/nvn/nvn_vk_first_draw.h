// 首笔真实 GPU 绘制（内存消费统一编译结果）：真实资源 + 真实 draw 参数 + GPU 完成等待。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "vk_device.h"
#include "vk_record.h"

#ifdef __cplusplus
extern "C" {
#endif
// 崩溃取证：最近关键 Vulkan 调用的环形记录（debug.s2a.callring=1 时记录，崩溃处理器落盘）。
void nvn_vk_callring_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void nvn_vk_callring_dump(int fd);
int nvn_vk_callring_enabled(void);
// 离线库/私有盘条目在采纳前的内容校验：用与真正消费相同的 parse_program 解析一遍，
// 返回 1 = 可解析（采纳才算成立），0 = 不可解析（why 为原因）。只读，不建对象、不改缓存。
int nvn_vk_first_draw_program_valid(const uint8_t *bytes, size_t length, char *why, size_t why_size);
// Inspect the same final vertex module before an unsupported device adopts a compiled result.
int nvn_vk_program_vertex_storage_safe(const uint8_t *bytes, size_t length, char *why, size_t why_size);
// draw_log 生产通道覆盖（s2arm.draw_log intent extra → NativeBridge.setDrawLog）：
// on >= 0 时逐笔/首 8 笔诊断按它开，不再读 debug.s2a.draw_log 属性（user 版设备
// untrusted_app 读不到 debug_prop）；on < 0 不动现有覆盖，继续按属性。
void s2a_fd_set_draw_log(int on);
// program: 统一编译结果原文（s2a-direct-program-v1，内存里）。成功返回 0 并写 why（实际绘制参数摘要）。
// 失败返回非 0 并写 why：缺哪一项资源/状态就报哪一项，不用占位资源。
// 提交本帧积压命令并等完成。返回 0 = 队列已完成（资源已释放，可继续）；
// 非 0 = 提交失败或等待超时：队列进入终态，命令缓冲与在途资源全部保留（不重试、不复用），
// 后续绘制/上屏/QueueFinish 都必须失败退出，由下一次启动重建会话。
int nvn_vk_first_draw_flush(void);
// 带预算的完成证明（guest fence 的 poll/有限超时）：
//   budget_ns==0 = 纯状态探测：不关 pass、不提交，只对已提交 fence 做 vkGetFenceStatus；
//                  有 open pass、未提交前缀或任一 fence 未 signal 都返回 1（没有完成证明）。
//   budget_ns>0  = 最多等这么久：必要时关闭 pass、提交未提交前缀，再按顺序等 fence。
// 返回 0 = 已提交部分全部完成；1 = 尚未完成（非失败，资源/fence/命令保留）；-1 = 终态失败。
// 不能把 1 当完成，也不能据此释放/复用资源。
int nvn_vk_first_draw_flush_ready(int64_t budget_ns);
// 已结束录制的命令缓冲入帧队列：三态明确区分所有权与失败。
// NOT_TAKEN: 调用方仍拥有 cmd；ACCEPTED: 队列拥有（可能还未提交）；
// FAILED_OWNED: 队列已接手后批次提交失败，队列持有 cmd，调用方必须报错且不可释放/重试。
enum s2a_fq_enqueue_result {
    S2A_FQ_ENQUEUE_NOT_TAKEN = -1,
    S2A_FQ_ENQUEUE_ACCEPTED = 0,
    S2A_FQ_ENQUEUE_FAILED_OWNED = 1,
};
int nvn_vk_first_draw_enqueue(struct s2a_vk_device *d, VkCommandBuffer cmd);
// pass 边界（记录回放在附件变化/非绘制 GPU 操作前调用）：关闭当前 open pass；sync != 0 时
// 还把已录制但尚未提交的前缀按队列顺序交出去并等完成，之后的 GPU 操作才有顺序保证。
// 返回 0 = 边界已落地（sync 时有完成证明）；非 0 = 没有完成证明（帧队列进入终态并保留在途资源），
// 调用方必须停止执行后续 GPU 操作并按真实错误返回，不能继续按“顺序已保证”消费。
int nvn_vk_first_draw_pass_break(int sync);
// 帧队列保留的首错原文（没有失败返回空串）：失败传播时带上它，不覆盖也不丢掉首错。
const char *nvn_vk_first_draw_first_error(void);
// 上屏 fence 等到后调用：解除本帧纹理 pin 与逐帧查找缓存（flush 本身不做）。
void nvn_vk_first_draw_present_done(void);
// QueueFinish：提交积压命令并等完成，不销毁会话/设备。0 = 已完成。
int nvn_vk_first_draw_finish(void);
// 真正销毁设备前调用：丢弃属于旧设备的 fence/描述符池/管线缓存/arena 记账。
void nvn_vk_first_draw_release_device(struct s2a_vk_device *d);
// program_sha：调用方在结果建立时算好的 SHA-256(program bytes)，可为 NULL（本函数自行计算）。
// 传已知 SHA 可避免逐笔重算完整结果（50 KB 级）；身份仍是完整内容，不是短键。
int nvn_vk_first_draw(struct s2a_vk_device *d, const struct s2a_record_draw_state *state,
                      const uint8_t *program, size_t program_bytes, const uint8_t program_sha[32],
                      const uint8_t program_input_sha[32],
                      uint64_t session, uint64_t generation, const char *request_id,
                      char *why, size_t why_size);
// 自上次调用以来进入绘制路径的 draw 数 / 其中 depth-only 数（窗口汇总用；读取即清零）。
// 这是“本窗多少笔”，不是 pass 关闭次数；每笔明细只在 debug.s2a.draw_log 打开时打印。
void nvn_vk_first_draw_take_counts(uint64_t *draws, uint64_t *depth_only);
// 逐帧 TSV 用的不清零累计（与 take_counts 窗口账互不影响；记站点同口径）。
void nvn_vk_first_draw_draw_pass_totals(uint64_t *draws, uint64_t *passes);
// 自上次调用以来 render pass 缓存的命中/新建次数，以及建 render pass / framebuffer 的累计耗时
// （读取即清零）。用于判断 pass 对象创建是否还是每帧成本；命中多不等于成本已经降下来。
void nvn_vk_first_draw_take_pass_stats(uint64_t *hits, uint64_t *miss, uint64_t *create_ns,
                                       uint64_t *fb_ns, uint64_t *fb_n);
// 绘制路径上真实 vkCreateGraphicsPipelines 的累计次数/墙钟（不含预热；预热单独记账）。
void nvn_vk_first_draw_pipe_create_stats(uint64_t *n, uint64_t *ns);
// 预热创建：次数/墙钟/跳过数（独立记账，不与绘制路径混）。
void nvn_vk_first_draw_preheat_stats(uint64_t *n, uint64_t *ns, uint64_t *skip);
void nvn_vk_first_draw_take_pass_open_stats(uint64_t *cget_ns, uint64_t *bcb_ns, uint64_t *rest_ns, uint64_t *npass);
// GPU 帧时长诊断（debug.s2a.gpu_time=1）：present blit 末尾写结束时间戳，渲染 fence 完成后读。
void nvn_vk_first_draw_gpu_end(VkCommandBuffer cmd);
void nvn_vk_first_draw_gpu_read(struct s2a_vk_device *d, const char *where);
// 延迟 present：对应 slot 的 render fence signal 后按帧奇偶读帧起止时间戳；0 = 未取到（不是 0 ms）。
int nvn_vk_first_draw_gpu_read_frame(struct s2a_vk_device *d, int parity, const char *where);
// 最近一次成功读到的 GPU 帧时间；0 = 还没有有效值。成功读出后才更新，不拿未知冒充 0。
int nvn_vk_first_draw_gpu_last(double *ms, int *passes, uint64_t *frame);
// debug.s2a.gpu_time 开关是否开（不含设备能力：能力要 device、首帧 arm 才定）。
int nvn_vk_first_draw_gpu_on(void);
// 延迟槽已拿到完成证明并回收的帧数（W6 资源退休账）。
uint64_t nvn_vk_first_draw_deferred_retired(void);
void nvn_vk_first_draw_gpu_cancel(void);
// 延迟 present（debug.s2a.present_defer=1）：present 只提交不等完成，下一帧/同步边界再收口。
int nvn_vk_first_draw_present_deferred(void);
int nvn_vk_first_draw_flush_defer(void);
// 开 pass / 单笔的统一窗口账：每项都带自己的累计 ns 与次数。
// 分母必须分开：passes（每次新开）、draws（每笔）、frame_id 差值（每帧）。禁止混用。
enum {
    S2A_OW_SEG_ENTRY_MODULES = 0,   // 入口 + 程序模块
    S2A_OW_SEG_PASS,                // pass 段（含新开的摊销）
    S2A_OW_SEG_PIPE,                // 管线查找
    S2A_OW_SEG_RES,                 // 支撑 + 资源绑定
    S2A_OW_SEG_VBUF,                // 顶点/索引缓冲
    S2A_OW_SEG_DESC_CMD,            // 描述符 + 录制命令
    S2A_OW_SEG_MAX
};
struct nvn_vk_open_window {
    uint64_t frame_id;              // 取数时的帧序号（调用方用差值得到本窗帧数）
    uint64_t passes;                // 本窗新开 pass 次数（= rest 子项的分母）
    uint64_t cget_ns, bcb_ns, rest_ns;
    uint64_t rest_gpuarm_ns, rest_query_ns, rest_precopy_ns, rest_barrier_ns, rest_beginrp_ns;
    uint64_t rest_sub_n, open_call_ns, open_call_n;
    uint64_t draws;                 // 本窗经过单笔统计的笔数（= seg_ns 的分母）
    uint64_t seg_ns[S2A_OW_SEG_MAX];
    uint64_t seg_total_ns;
    uint64_t res_ub_ns, res_ub_n, res_tex_ns, res_tex_n;
    uint64_t desc_alloc_ns, desc_alloc_n, desc_loop_ns, desc_loop_n;
    uint64_t desc_write_ns, desc_write_n, desc_pool_ns, desc_pool_n;
    // 描述符调用次数：本窗 vkAllocateDescriptorSets / vkUpdateDescriptorSets 各调了多少次、
    // 一共写了多少条描述符、帧内复用命中多少。验收「调用数减少」看这里，不看耗时估计。
    uint64_t desc_alloc_calls, desc_update_calls, desc_writes, desc_cache_hits, desc_cache_miss;
};
void nvn_vk_first_draw_take_open_window(struct nvn_vk_open_window *o);
int nvn_vk_first_draw_retire_all(void);
// 当前录制帧的序号（从 1 开始，每帧/每次完整 flush 递增）：时间戳待写项按它归属到帧，
// 退休只结算已拿到完成证明的帧。
uint64_t nvn_vk_first_draw_frame_id(void);
// OP_REPORT_COUNTER(TIMESTAMP_TOP)：把 reset + TOP_OF_PIPE 时间戳录进当前帧队列的命令缓冲，
// 不 submit、不等 fence；地址与槽位的写回由 nvn_vk_query.c 在帧完成点统一处理。
int nvn_vk_first_draw_timestamp_top(struct s2a_vk_device *d, VkQueryPool pool, uint32_t slot,
                                    char *why, size_t why_size);
void nvn_vk_first_draw_slot_blit(VkFence fence, VkCommandBuffer cmd);
#ifdef __cplusplus
}
#endif
