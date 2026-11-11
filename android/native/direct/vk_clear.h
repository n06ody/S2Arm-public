// 目标区域清除：NVN ClearColor / ClearDepthStencil 的 Vulkan 实现（scissor 区域 + 深度/模板分平面
// + 8 位模板位掩码）。只用于“GPU 立即清除”，不改变绘制管线；每次清除同步提交并等完成。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "vk_device.h"

#ifdef __cplusplus
extern "C" {
#endif

// 清除目标：一个精确子资源（base_level/base_layer 的单 mip/单层）。多维目标由调用方逐层调用。
typedef struct s2a_clear_target {
    VkImage image;
    VkFormat format;
    VkImageAspectFlags aspect;   // COLOR / DEPTH / (DEPTH|STENCIL)
    uint32_t base_level, base_layer;
    uint32_t width, height;
} s2a_clear_target;

// 清除的提交结果（与一次性提交的三类收尾一致）：调用方必须分开消费。
enum {
    S2A_VK_CLEAR_OK = 0,          // 已清除并拿到完成证明
    S2A_VK_CLEAR_NOOP = 1,        // 区域为空（scissor 与目标不相交），无需清除
    S2A_VK_CLEAR_UNSUBMITTED = -1,// 没有提交成功（含参数/对象创建失败）：命令从未执行，对象已回收
    S2A_VK_CLEAR_UNCOMPLETED = -2,// 已提交但没有完成证明：命令缓冲/fence/framebuffer/view 全部保留
    S2A_VK_CLEAR_FQ_FAILED = -3,  // 帧队列接手命令后提交失败：队列持有，不能释放或回退
};
// C1：帧队列化清屏。命中的清除只录进一段命令缓冲并交给帧队列（由 nvn_vk_first_draw 提供），
// 不 submit、不等 fence——原来每笔清除都 vkQueueSubmit + vkWaitForFences(5s)，等的其实是
// 前面整段绘制，尖峰帧 8 笔合计 19–45 ms。只有“能整幅表达”的清除走这条路：
// 颜色必须 color_mask==0xF，深度必须 depth_mask!=0 且（有模板时）stencil_bit_mask==0xFF。
// 返回 S2A_VK_CLEAR_QUEUED = 已交给帧队列（调用方不得再走同步路径）；
//     S2A_VK_CLEAR_UNHANDLED = 这条路不适用（调用方必须走原同步路径）；
//     S2A_VK_CLEAR_NOOP/其它 = 同下面同步版本的语义。
enum { S2A_VK_CLEAR_QUEUED = 2, S2A_VK_CLEAR_UNHANDLED = 3 };
int s2a_vk_clear_color_queued(struct s2a_vk_device *d, const s2a_clear_target *t,
                              const VkClearColorValue *value, uint32_t color_mask,
                              const VkRect2D *scissor, char *why, size_t why_size);
int s2a_vk_clear_ds_queued(struct s2a_vk_device *d, const s2a_clear_target *t,
                           float depth, uint32_t stencil, int depth_mask, uint32_t stencil_bit_mask,
                           const VkRect2D *scissor, char *why, size_t why_size);

// scissor 是 NVN 当前裁剪矩形（设备像素，已包含 viewport 位移）；NULL 或覆盖整幅时表示整目标。
// 返回上面的枚举值；失败时写 why。UNCOMPLETED 仍可能已经写进目标（GPU 可能执行了命令），
// 上层不能把它当作“没有写过”，也不能当成功。
// color_mask 是 NVN 的 4 位颜色写掩码（bit0=R, bit1=G, bit2=B, bit3=A）；0 = 空操作，
// 0xF = 整幅清除，其余走受 VkColorWriteMask 约束的全屏三角形（不污染后续游戏状态）。
int s2a_vk_clear_color(struct s2a_vk_device *d, const s2a_clear_target *t,
                       const VkClearColorValue *value, uint32_t color_mask, const VkRect2D *scissor,
                       char *why, size_t why_size);
// depth_mask != 0 清深度；stencil_bit_mask 是 8 位平面掩码（0xFF = 整平面，0x01 = 只清 bit0）。
int s2a_vk_clear_depth_stencil(struct s2a_vk_device *d, const s2a_clear_target *t,
                               float depth, uint32_t stencil, int depth_mask, uint32_t stencil_bit_mask,
                               const VkRect2D *scissor, char *why, size_t why_size);
// 最近一次清除失败的 vk 层细节（首错保留），供上层日志使用。
const char *s2a_vk_clear_first_error(void);
// 对象/提交记账（诊断与验收用）：正常完成后创建/销毁逐类相等；UNCOMPLETED 的条目在账本里。
struct s2a_vk_clear_stats {
    uint64_t commands_created, commands_freed;
    uint64_t fences_created, fences_freed;
    uint64_t framebuffers_created, framebuffers_freed;
    uint64_t views_created, views_freed;
    uint64_t submitted, completed, unsubmitted, uncompleted;
    uint32_t retained;            // 当前保留条目数（fence/命令缓冲/fb/view）
    uint64_t retained_overflow;   // 账本满、仅保留未记账的次数
};
void s2a_vk_clear_stats(struct s2a_vk_clear_stats *out);
// 设备销毁前结算保留账本：有界等待 fence，拿到完成的条目显式销毁，其余保留并计数（随设备销毁）。
int s2a_vk_clear_release_retained(struct s2a_vk_device *d, uint64_t timeout_ns, uint32_t *left_out);
// 诊断/测试注入：1 = 真实提交后按“完成未知”处理；2 = 假装提交失败。默认 0 = 正常语义。
void s2a_vk_clear_set_submit_fault(int mode);

#ifdef __cplusplus
}
#endif
