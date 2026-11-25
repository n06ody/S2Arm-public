// 首用管线的后台构建（安卓新回放层）。总方案见 docs/设计_管线首用异步与跳过.md。
//
// 分工：
//   提交线程  nvn_vk_pipe_demand_kick(desc, 程序身份)：缓存命中→READY；否则登记 COMPILING 并入有界队列→PENDING。
//   后台线程  rp_shader_prog_borrow(身份) 拿正式借用 → 用程序阶段句柄填 desc.st[].sh → nvn_gpu_pipe_get(SYNC)
//             （装进后端缓存）→ 归还借用 → 发布 READY/FAILED。程序寿命由 P1-1 借用合同保护，不裸指针跨线程。
//   提交线程  nvn_vk_pipe_demand_wait(desc, 身份, wait_ms)：命中/等到 READY→对象；超时→PENDING；FAILED 带原文。
//
// 关键：待定表的键 = 管线描述（阶段着色器句柄置空）＋ 程序内容身份。**必须带身份**——后端按着色器句柄区分
//   管线（backend same_pipe_desc/pipe_hash 都比 st[].sh），只按状态/格式作键会把「状态相同、着色器不同」的两套
//   配方合并成一条，第二套拿到第一套的对象（错管线上屏）。所有操作（state/wait/mark/marks）都要带同一身份。
//
// 开关：debug.s2a.nvk_pipe_async（缺省 1；release 包属性门关着→读不到→缺省开；调试包设 0 退回同步建）。三档上限沿用 iOS DM_NVN_PSO_*：
//    debug.s2a.fu_item_ms（2000）/ fu_frame_ms（100）/ fu_unsafe_ms（10000）。
// 生命周期：会话 open 后 start；Stop/close 前 stop（有界等待；超时后保留 joinable worker，直到退出后回收）。
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "nvn_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { NVK_DEMAND_NONE = 0, NVK_DEMAND_PENDING = 1, NVK_DEMAND_READY = 2, NVK_DEMAND_FAILED = 3 };

void nvk_pipe_demand_worker_enter(void);   // 标记本线程后续创建为后台首用，和预热分开
void nvn_vk_pipe_demand_start(void);
void nvn_vk_pipe_demand_stop(void);
/* Private session guard: returns false conservatively when lifecycle locks are busy; only reaps a worker already marked done, never waits for an active worker. */
int  nvn_vk_pipe_demand_quiescent(void);
#if defined(S2A_NVN_PIPE_DEMAND_TEST)
int  nvn_vk_pipe_demand_worker_count(void);
uint64_t nvn_vk_pipe_demand_test_start_lock_denials(void);
uint64_t nvn_vk_pipe_demand_test_stop_busy_returns(void);   // stop 因另一个 stop 在等而直接返回的次数
uint64_t nvn_vk_pipe_demand_test_stop_timeouts(void);       // stop 有界等待到点、worker 未退的次数
void nvn_vk_pipe_demand_test_set_on(int on);
int  nvn_vk_pipe_demand_test_lifecycle_busy(void);
int  nvn_vk_pipe_demand_test_lock_lifecycle(void);
void nvn_vk_pipe_demand_test_unlock_lifecycle(void);
#endif
int  nvn_vk_pipe_demand_on(void);

int  nvn_vk_pipe_demand_kick(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32], nvn_gpu_pipe *cached);
// Optional cold fragment variant. Results share the worker and lifetime
// budget, but have a separate demand identity and never wait on the caller.
int  nvn_vk_pipe_demand_cold_kick(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32],
                                 const uint8_t variant_sha[32], nvn_gpu_pipe *cached);
int  nvn_vk_pipe_demand_state(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32], nvn_gpu_pipe *cached);
int  nvn_vk_pipe_demand_wait(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32], uint32_t wait_ms, nvn_gpu_pipe *out, char *why, size_t why_cap);
void nvn_vk_pipe_demand_mark(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32], int is_stuck);
int  nvn_vk_pipe_demand_marks(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32], int *late, int *stuck);

uint32_t nvk_fu_item_ms(void);
uint32_t nvk_fu_frame_ms(void);
uint32_t nvk_fu_unsafe_ms(void);

struct nvn_pipe_demand_stats { uint64_t kicked, dedup, built, failed, timeouts, full, published; };   // published：结果已发布进待取表、前台看得见（built 在发布之前计）
void nvn_vk_pipe_demand_stats(struct nvn_pipe_demand_stats *out);

#ifdef __cplusplus
}
#endif
