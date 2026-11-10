// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// OP_DISPATCH(op=27) 墙钟九段仪器（debug.s2a.compute_phase=1 开，默认关）。
// 目的：回答「每笔 ~13.7ms 的 OP_DISPATCH 墙钟里，CPU 实做与等待各占多少」——
// 分段之和与 opcode 墙钟（win_stats g_op_ns[27] 同口径，shim 挂点给真值）对账，
// 差额单列 unattr（未归属：pass_break 非等部分、状态填充、执行层出入与计时盲区）。
// 关时开销 = 各调用点一次原子读 + 分支，不取钟、不记账。
// 窗口 = 每 64 笔完成样本打一行 logcat：每段 avg-us / p95-us + wall + unattr。
// 单线程假设：与 win_stats / frame_log 相同——只跑在记录回放线程（exec 路径）。
// 段口径：
//   prep    = 纹理快照获取 + profile 解析 + REQUEST 构造（nvn_vk_compute_bridge.c）
//   lookup  = nvk_compute_lookup 全程（request_facts JSON 三遍 + 磁盘程序缓存读
//             + accept 封套解析含第一遍 SPIR-V 反射解析）
//   parse   = nvn_vk_compute_pipeline_build 内第二遍 SPIR-V 反射解析
//   alloc   = 无界并集窗 + 逐绑定 buffer 建/分配/映射 + guest memcpy 载入
//   desc    = 描述符池创建 + 集分配 + vkUpdateDescriptorSets
//   record  = vk_record 端 begin_command + vkCmd* 录制
//   submit  = 本笔的全部同步等待：pass_break(1) 排空（其内 vkQueueSubmit 返回耗时
//             与逐批 vkWaitForFences）+ submit() 全程（EndCommandBuffer/fence 建/
//             vkQueueSubmit 返回/vkWaitForFences 本笔完成）
//   wb      = finish() 里可写绑定 + 并集窗的 guest.write 回写 memcpy
//   dtor    = finish() 里 destroy() 全程（描述符池销毁 + 逐缓冲 unmap/destroy/free）
// 失败/保留口径：reject 早于计时点的那半段落入 unattr；finish(-2) 完成未知时
// wb/dtor = 0（资源保留，之后真正完成时不再回补该样本）。
#ifndef NVN_VK_COMPUTE_PHASE_H
#define NVN_VK_COMPUTE_PHASE_H
#include <stdint.h>
enum nvk_compute_phase_seg {
    NVK_PHASE_PREPARE = 0,
    NVK_PHASE_LOOKUP,
    NVK_PHASE_PARSE,
    NVK_PHASE_ALLOC_COPY,
    NVK_PHASE_DESCRIPTOR,
    NVK_PHASE_RECORD,
    NVK_PHASE_SUBMIT_WAIT,
    NVK_PHASE_WRITEBACK,
    NVK_PHASE_DESTROY,
    NVK_PHASE_SEGS
};
int nvk_compute_phase_on(void);
uint64_t nvk_compute_phase_now(void);
void nvk_compute_phase_begin(void);                 // 新一笔起点；在途旧向量先按内部窗宽结算
void nvk_compute_phase_add(int seg, uint64_t ns);   // 未起笔自动起（pass_break 缺席路径兜底）
void nvk_compute_phase_flush(void);                 // 按内部窗宽结算当前笔（无 shim 挂点的路径）
void nvk_compute_phase_wall(uint64_t op_wall_ns);   // opcode 墙钟结算（无 wall_begin/end 挂点的路径）
// shim 挂点成对用：wall 由模块自取 CLOCK_MONOTONIC 与九段同钟同单位，span 与挂点的
// opcode 墙钟同一区间。调用方不再喂计数值——历史上喂过 CNTVCT 原始 tick（差 ~52 倍）。
void nvk_compute_phase_wall_begin(void);
void nvk_compute_phase_wall_end(void);
uint64_t nvk_compute_phase_wall_total(void);        // wall 累计 ns（宿主同单位断言用）
// seg_sum = 每段累计 ns；seg_hits = 每段被计时的次数——「段是否走到」用 hits 判，
// 时长在粗粒度钟下可合法为 0（<1 tick），不能用 ns>0 断言覆盖。
void nvk_compute_phase_stats(uint64_t seg_sum[NVK_PHASE_SEGS], uint64_t seg_hits[NVK_PHASE_SEGS],
                             uint64_t *samples, uint64_t *lines);
#endif
