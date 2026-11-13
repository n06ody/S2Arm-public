// 安卓直连会话的装配点：把 prepare 建好的 Vulkan 设备交给 nvn_gpu_* 后端，管窗口挂接，会话收尾时出「未接」汇总表。
// 只被 provider_nvk.c 调用。回放核心与后端不认识本头。
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct s2a_vk_device;
// One budget for compile admission/retirement and device retirement, strictly below
// the process watchdog. A caller may already have used part of that watchdog.
#define NVK_SHUTDOWN_BUDGET_NS 700000000ull
int nvk_session_prepare(void);
void nvk_session_request_stop(void);
int nvk_session_wait_compiles(void);
uint64_t nvk_session_stop_deadline(void);
void nvk_shader_stop(void);
unsigned nvk_shader_inflight(void);
int nvk_shader_open(void);
// 打开：后端接管 dev（不另建、不拥有其寿命）。cache_dir 可为 NULL。0 = 成功；失败 why 写原因。
int nvk_session_open(struct s2a_vk_device *dev, const char *cache_dir, char *why, size_t why_size);
int nvk_session_ready(void);
// 关闭：等在途提交完成并关后端。0 = 已关且完成有证明；非 0 = 没有证明（调用方保留设备与窗口租约，不归还）。
int nvk_session_close(void);
// 窗口（ANativeWindow*）挂上 / 摘下。0 = 成功；摘下返回非 0 = 最后一帧没有完成证明，调用方保留窗口租约。
int nvk_session_attach_window(void *window, uint64_t generation, char *why, size_t why_size);
int nvk_session_detach_window(void);
void nvk_session_window_size(int32_t *w, int32_t *h);   // 挂窗口时记下的发布尺寸；没窗口 0

#ifdef __cplusplus
}
#endif
