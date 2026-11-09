// 前端 mtl_* → 新 Vulkan 后端的适配层（第一片）：全局会话的 init/shutdown/ready。
// 后续把 mtl_pool_* / mtl_tex_* / mtl_present / mtl_exec 接到同一会话。
#pragma once
#include <stddef.h>
#include "nvn_vk_session.h"
#include "vk_record.h"
#include "direct/texture_facts.h"
#include <android/native_window.h>

void s2a_vk_adapter_mark_written(VkImage image);
#ifdef __cplusplus
extern "C" {
#endif
int s2a_vk_adapter_init(char *why, size_t why_size);
// 只有 provider 的显式新会话（activate）可以清除 Stop 终态；迟到入口不能调用。
void s2a_vk_adapter_new_session(void);
// 关闭会话。返回 0 = 帧队列完成且最后一帧呈现退休都已有证明；-1 = 有一步没证明完成
// （资源保留、根因已写日志）。调用方不能把 -1 当成“正常收局”（不清 attempt、不 ACK 失败）。
int s2a_vk_adapter_shutdown(void);
int s2a_vk_adapter_finish(void);
// 提交当前帧队列并等完成，不销毁会话（guest fence 的完成证明入口）。0 = 已完成；
// 非 0 = 提交失败或等待超时：队列进入终态、在途资源保留，调用方必须按真实失败处理。
int s2a_vk_adapter_flush(void);
// 带预算：0 = 已完成；1 = 已提交但未完成（guest 超时/轮询，非设备错误）；
// 2 = 预算内没拿到 owner 锁（同样没有完成证明）；-1 = 终态失败。
int s2a_vk_adapter_flush_budget(int64_t budget_ns);
int s2a_vk_adapter_ready(void);
// 窗口汇总：等 g_mu / 等执行包让出的总时间与次数（读取即清零）。
void s2a_vk_adapter_exec_lock_stats(uint64_t *wait_ns, uint64_t *calls);
struct nvn_vk_session *s2a_vk_adapter_session(void);
// 前端 mtl_pool_add/del 的记账：地址区间用于后面把 tex_t/prog 指针解析到池。
int s2a_vk_adapter_pool_add(const void *mem, uint64_t size);
// 查地址区间所在的池范围（供内存层惰性换页用）。
int s2a_vk_adapter_pool_find(uint64_t addr, uint64_t len, uint64_t *base, uint64_t *size);
// guest 写了池内存：让账本推进内容版本（零拷贝/版本化决策都依赖它）。
void s2a_vk_adapter_pool_written(const void *mem, uint64_t off, uint64_t size);
void s2a_vk_adapter_pool_del(const void *mem);
int s2a_vk_adapter_pool_contains(uint64_t addr, uint64_t len);
// GPU 地址 → (池基址, 偏移)；地址不完整落在某个池里返回 0。mtl_addr 用。
int s2a_vk_adapter_pool_lookup(uint64_t addr, uint64_t len, void **base, uint64_t *off);
// 前端纹理对象 → 会话纹理缓存（mtl_tex_init 的适配入口）。
struct nvn_vk_tex *s2a_vk_adapter_tex(const tex_t *t, char *why, size_t why_size);
// 丢掉/释放该 tex_t 的会话纹理（mtl_tex_release）。
void s2a_vk_adapter_tex_release(const tex_t *t);
// CPU 改了纹理内容（mtl_tex_cpu_written）：只标脏，下一次解析按内容哈希复查后重传，不销毁图像。
// 无范围 hint，不构成内容归属证明。
void s2a_vk_adapter_tex_cpu_written(const tex_t *t);
// WriteTexels 的精确区域（内容归属权威）：level / layer0（3D 用 z0/nz 作深度切片）/ 区域。
void s2a_vk_adapter_tex_written_rect(const tex_t *t, unsigned level, unsigned layer0,
                                     int x, int y, int w, int h, int z0, int nz);
void s2a_vk_adapter_tex_gpu_written_subres(const tex_t *t, unsigned level, unsigned layer);
// 真实 gpu_owned：该 serial 的纹理当前内容是否归 GPU（前端据此决定是否通知 CPU 写）。
int s2a_vk_adapter_tex_gpu_owned(uint64_t serial);
// 窗口/present 的适配入口（mtl_present 用）：纹理缓存解析 + swapchain present。
int s2a_vk_adapter_present_tex(const tex_t *t, const int crop[4], char *why, size_t why_size);
// 窗口/present 的适配入口（mtl_present 用）。
int s2a_vk_adapter_attach_window(ANativeWindow *window, uint64_t generation, char *why, size_t why_size);
// 窗口撤销/换代：销毁交换链（有界观察最后一帧 present fence）。返回 0 = 可以归还租约；
// -1 = 呈现退休没有证明：调用方必须保留 swapchain 与 WSI 租约，不得 ACK/released。
int s2a_vk_adapter_detach_window(void);
// B3：buffer_geometry 开启且交换链已按游戏输出尺寸建好时返回 1 并给出输出尺寸。
int s2a_vk_adapter_buffer_geometry(uint32_t *w, uint32_t *h);
// 没有 surface 的 present：把本帧命令提交掉，并且只在拿到 GPU 完成证明后释放纹理 pin
// （等价 present 的收尾），不上屏、不销毁会话。返回 0 = 本帧已完成；-1 = 没有完成证明，
// pin/队列资源保留，调用方按真实失败处理（后台几帧后 pin list 满正是漏了这一步）。
int s2a_vk_adapter_skip_present(void);
int s2a_vk_adapter_present_clear(float r, float g, float b, float a, char *why, size_t why_size);
int s2a_vk_adapter_draw_present(const char *blob_path, char *why, size_t why_size);
// 纹理/采样器注册（mtl_reg_tex / mtl_reg_samp 的适配入口）与按 NVN 池 id 解析。
void s2a_vk_adapter_reg_tex(int id, const tex_t *t, const view_t *v);
void s2a_vk_adapter_reg_samp(int id, const samp_par *p);
int s2a_vk_adapter_samp_by_id(int id, samp_par *out);
// 纹理通道 swizzle：0 = 未设置（恒等）；set_swz=0 时 swz 无意义。
int s2a_vk_adapter_tex_swz_by_id(int id, uint32_t *swz, int *set);
// 注册时保存的 NVNtextureView（RegisterTexture 的第二参数）：返回 1 = 有视图并写出内容，0 = 该纹理没有视图，
// -1 = id 未注册。采样侧还没完整消费它；诊断要用真实视图字段（格式/mip/层/swizzle）定位，不按猜测绑定。
int s2a_vk_adapter_tex_view_by_id(int id, view_t *out);
// 按注册视图取（或建）精确采样视图（mip/层/格式/target/通道映射）；换视图不重建图像。
VkImageView s2a_vk_adapter_tex_view(struct nvn_vk_tex *t, const view_t *v, char *why, size_t why_size);
// 附件视图：mip/层子集与同兼容类格式覆盖；返回实际附件 VkFormat。深度/模板覆盖明确拒绝。
VkImageView s2a_vk_adapter_attachment_view(struct nvn_vk_tex *t, const view_t *v, VkFormat *format_out,
                                           char *why, size_t why_size);
int s2a_vk_adapter_tex_by_id(int id, struct nvn_vk_tex **out, char *why, size_t why_size);
// F4.2B：只读注册表里的真实纹理描述（不建视图/不上传），供编译请求输出 program.textures[] 事实。
// s2a_vk_tex_facts 定义在 direct/texture_facts.h：graphics 与 compute 的请求消费同一份事实结构。
int s2a_vk_adapter_tex_facts(int id, struct s2a_vk_tex_facts *out, char *why, size_t why_size);
// 原 facts 采集前批量复制注册元数据；不初始化表、不读 GPU/guest、不推导 view 默认值。
struct nvk_w2_raw_registration;
int s2a_vk_adapter_w2_capture_registered(const int *ids, size_t count,
    struct nvk_w2_raw_registration *out, uint64_t *generation);
// F4.4：draw 等待需求编译时挂起当前执行包并交还/重新取得执行锁；只在当前执行包线程内有效。
// pause 返回 0 = 已挂起；-1 = 不是执行包线程/已挂起/Stop 已发布（等待不得当成已挂起）。
int s2a_vk_adapter_exec_pause(void);
// 恢复只能发生一次：同一执行包线程、同一挂起令牌、同一代际；Stop/换代后拒绝并保留原因。
void s2a_vk_adapter_exec_resume(void);
// owner poll 的无锁查询：当前是否有执行包挂起等待需求编译（1/0）。
int s2a_vk_adapter_exec_suspended(void);
int s2a_vk_adapter_stop_requested(void);
// 执行包 owner 统计：进入/等待、挂起/恢复、拒绝、Stop 时是否挂起、挂起总时长、挂起窗口内
// present 次数。读取不改变状态；用于 Stop 收尾日志与真机对账。
struct s2a_vk_owner_stats {
    uint64_t enters, wait_enters, suspends, resumes;
    uint64_t refused_foreign, refused_after_stop, stop_during_suspend, leaves_while_suspended;
    uint64_t suspend_total_ns;
    uint64_t presented_during_suspend;
};
void s2a_vk_adapter_owner_stats(struct s2a_vk_owner_stats *out);
// owner poll 里的窗口挂接：0 = 已挂上；2 = 锁忙/未就绪（本轮跳过，下一轮重试，不是失败）；
// -1 = 真实失败（why 写明）。预算只覆盖等 owner 锁，不保证驱动调用返回时间。
int s2a_vk_adapter_attach_window_budget(ANativeWindow *window, uint64_t generation, int64_t budget_ns,
                                        char *why, size_t why_size);
// 注册表代际：任何 reg_tex/reg_samp/tex_release 都会递增。绘制侧按它失效 id→对象缓存，
// 避免每笔 draw 都拿注册表锁重新解析同一批纹理/采样器。
uint64_t s2a_vk_adapter_registry_generation(void);
// NVN ReadTexels（纹理读回）：按内容归属选 guest 内存或 GPU 图像两条路径，串行在同一个执行包上。
// 返回 0 = 已读回；-1 = 失败且 why 带可定位上下文（serial/尺寸/格式/level/layer/区域）；调用方必须
// 按真实失败处理（red），不能把未读回当成功。
int s2a_vk_adapter_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r,
                               uint8_t *dst, size_t row, size_t img, char *why, size_t why_size);
// 单条 OP_* 记录的执行入口（mtl_exec 用）。
int s2a_vk_adapter_exec(const uint8_t *packet, size_t len,
                        const struct s2a_record_memory *memory, struct s2a_record_result *result);
// 拍平期 guest 直读（延迟参数 / 失效快照在命令数据所有权转移点取一次）；0 = 成功，非 0 = 不可读。
int s2a_vk_adapter_guest_read(uint64_t address, void *dst, size_t bytes);
// guest 地址即宿主地址（S2Arm 的 HLE 池直映射）：记录的地址可直接 memcpy。
// mtl_exec 用它执行前端记录，不需要池映射。
int s2a_vk_adapter_exec_guest(const uint8_t *packet, size_t len, struct s2a_record_result *result);
#ifdef __cplusplus
}
#endif
