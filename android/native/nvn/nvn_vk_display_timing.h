// B2：VK_GOOGLE_display_timing 上屏时间计量（只做直连 present_image 路径）。
// 设备支持该扩展时给 present 挂 presentID，vkGetPastPresentationTimingGOOGLE 回报每帧
// 实际上屏时刻（主机单调域 ns）。用途：nvn_be_presented_time 出真值（fstep 上屏判定）、
// 周期统计行给 QA 量真实帧率。不支持的设备明确报 unavailable，不拿 CPU present 返回
// 时刻冒充（定稿 A6）。desiredPresentTime 恒 0 = 最早可排：本件只做计量，均匀排队
// （与 #193 nvn_vsync 节拍源配合）另起一任务。
// 开关：debug.s2a.display_timing 默认开计量；"0" 关闭（不挂 presentID、不轮询）。
// presentID → 帧号不假设连续：挂链时刻由调用方给本帧 frame（g_present_n+1 口径），
// 记录 pid→frame 表；clear/draw_present 等诊断 present 不挂 id，不占序号。
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// 绑定设备：supported = vk_device 探测到且已启用 VK_GOOGLE_display_timing。
// 取到函数表才 active；取不到报"unsupported"。重复调用先 reset。
void nvn_vk_dt_attach(VkDevice device, int supported,
                      PFN_vkGetPastPresentationTimingGOOGLE past_fn,
                      PFN_vkGetRefreshCycleDurationGOOGLE refresh_fn);
void nvn_vk_dt_detach(void);

// 0=不可用 1=计量中。状态串给"明确报不可用"面：disabled/unsupported/active。
int nvn_vk_dt_active(void);
const char *nvn_vk_dt_state(void);

// 呈现前调用：frame>0 且 active 时把 present 时刻信息链进 pi（ti/t 由调用方栈上持有，
// 仅本次 vkQueuePresentKHR 有效）。返回 1 = 已挂 id；0 = 未挂（pi 原样）。
// ti->pNext 链接在既有 pi->pNext 前面，不丢 present fence 链。
int nvn_vk_dt_chain(VkPresentInfoKHR *pi, VkPresentTimesInfoGOOGLE *ti,
                    VkPresentTimeGOOGLE *t, uint64_t frame);

// 机会式排干已到时刻（每帧一次、不阻塞）。device/swapchain 为调用方当前句柄。
void nvn_vk_dt_poll(VkDevice device, VkSwapchainKHR sc);

// 第 n 帧（与 nvn_be_presented_time 同口径）实际上屏秒；未知/未到/丢帧 = 0。
double nvn_vk_dt_shown(uint64_t n);

// 周期读数（QA 真实帧率）：近窗上屏间隔 p50/p95/max、refresh、未回报在途数。out 可为 NULL。
void nvn_vk_dt_stats(uint64_t *tagged, uint64_t *received, uint64_t *pending,
                     double *gap_p50_ms, double *gap_p95_ms, double *gap_max_ms, double *refresh_ns);

#ifdef S2A_DT_TEST
// 宿主替身：测试直接注入函数表与门值，不经 vkGetDeviceProcAddr/系统属性。
void nvn_vk_dt_test_reset(void);
void nvn_vk_dt_test_gate(int on);   // -1=读真实属性（默认）；0/1=固定
#endif

#ifdef __cplusplus
}
#endif
