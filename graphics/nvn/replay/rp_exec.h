// 回放核心内部接口（核心各文件之间用；前端只认 nvn_mtl.h / nvn_obj.h 里的 nvn_be_*）。
#pragma once
#include <stdint.h>
#include "nvn_obj.h"
#include "nvn_backend.h"

const char *rp_op_name(uint32_t op);   // 录制命令名（"OP_DRAW" …）；范围外返回 "OP_?"
int rp_pool_count(void);                // 已登记的内存池个数
int rp_tex_live_n(void);                // 活贴图对象个数（只是登记数，不等于已建图的个数）
int rp_depth_zero(void);                // NVN 深度模式：1 = 近平面为 0
int rp_origin_lower_left(void);         // NVN 窗口原点：1 = 左下
uint64_t rp_frame(void);                // 前端给的当前帧号

// ---------- 内存池镜像（rp_buf.c）----------
// 取 [addr, addr+len) 在后端的缓冲与偏移；自上次同步后写过的页在 c 这一段的 GPU 顺序里重传。0 = 地址不在任何池里（已记账）。线程：[提交]。
int rp_buf_resolve(nvn_gpu_cmd c, uint64_t addr, uint64_t len, nvn_gpu_buf *buf, uint64_t *off);
void rp_buf_stats(uint64_t *bytes, uint64_t *runs);   // 累计同步字节数 / 段数
int rp_buf_mirrors(void);                             // 现有镜像个数
// 缓冲类命令（rec = 命令记录地址，只进未接日志）
void rp_op_update_ub(const void *a, const void *rec);
void rp_op_copy_b2b(const void *a, const void *rec);
void rp_op_clear_buf(const p_clear_buf *cb, const void *rec);
void rp_op_copy_b2t(nvn_gpu_cmd c, const p_copy_b2t *o, const void *rec);
void rp_op_copy_t2t(nvn_gpu_cmd c, const p_copy_t2t *o, const void *rec);
void rp_op_clear_tex(nvn_gpu_cmd c, const p_clear_tex *o, const void *rec);

// 会话四件套（前端按名字调，上沿头文件里没有声明）
void nvn_be_begin_session(void);
int nvn_be_request_stop(void);
int nvn_be_drain_session(void);
int nvn_be_shutdown_session(void);   // 放掉全部后端对象、状态回到 NVN 默认值

// ---------- 渲染目标与通道（rp_pass.c）----------
void rp_pass_set_rt(nvn_gpu_cmd c, const p_set_rt *r);
void rp_pass_clear_color(nvn_gpu_cmd c, const p_clear_color *cc, const void *rec);
void rp_pass_clear_ds(nvn_gpu_cmd c, const p_clear_ds *cd, const void *rec);
void rp_pass_barrier(uint32_t bits);
int rp_pass_begin(nvn_gpu_cmd c);     // 绘制前调：1 = 通道开着；0 = 开不了（已记账），跳过这笔
void rp_pass_end(nvn_gpu_cmd c);
void rp_pass_flush(nvn_gpu_cmd c);    // 提交前 / 通道外操作前：落地待折清除并结束通道
void rp_pass_reset(void);
int rp_pass_is_open(void);
const p_set_rt *rp_pass_target(void); // 当前目标（没设过 NULL）

// ---------- 上屏（rp_present.c）----------
void rp_present_set_surface(nvn_gpu_surface s, int display_hz);   // 平台件：窗口挂上（s 非空）/ 摘下（NULL）
void rp_present_stats(uint64_t *shown, uint64_t *dropped);
void rp_present_reset(void);

// ---------- 绘制（rp_draw.c）----------
void rp_draw(nvn_gpu_cmd c, const p_draw *dr, const p_draw_idx *di, const void *rec);   // di 非空 = 带索引
void rp_draw_stats(uint64_t *draws, uint64_t *skips);
// 分时读数：自上次取走以来的累计（纳秒 / 次数），取走即清零
enum { RP_PERF_SUBMIT_NS, RP_PERF_COMMIT_NS, RP_PERF_SUBS, RP_PERF_WAIT_NS, RP_PERF_WAIT_N, RP_PERF_DONE_NS, RP_PERF_DONE_N, RP_PERF_PASSES, RP_PERF_PRESENT_NS,
       RP_PERF_D_PROG, RP_PERF_D_PREP, RP_PERF_D_TEX, RP_PERF_D_PIPE, RP_PERF_D_DYN, RP_PERF_D_BIND, RP_PERF_D_DRAW, RP_PERF_D_TAIL,
       RP_PERF_P_DS, RP_PERF_P_DS_IDLE, RP_PERF_P_LOADC, RP_PERF_P_KPIX, RP_PERF_P_ONE, RP_PERF_P_CUT_XFER, RP_PERF_P_CUT_CLEAR, RP_PERF_P_CUT_RT, RP_PERF_P_CUT_OP, RP_PERF_TEX_UP_N, RP_PERF_TEX_UP_KB, RP_PERF_BUF_SYNC_KB, RP_PERF_N };   // TEX_UP_*：贴图整张上传的次数与千字节；BUF_SYNC_KB：池镜像同步的千字节   // P_*：通道的构成（见 rp_pass.c rp_pass_end）   // D_*：一笔绘制里各步（见 rp_draw.c）
void rp_perf_add(int i, uint64_t v);
extern int rp_split_draws;       // 中途交段：每段的笔数（见 rp_exec.c）
uint64_t rp_seg_serial(void);   // 第几个后端段：段里的暂存内容（arena 切片）只在同一段里能复用
uint64_t rp_sub_serial(void);   // 第几次提交（一次提交内可复用的暂存内容按它判）
void rp_perf_take(uint64_t out[RP_PERF_N]);
void rp_draw_reset(void);
void rp_draw_log_arm(int lines);   // 接下来的 lines 笔绘制（含被跳过的）各出一行日志；0 = 关

// 资源同步用的段（见 rp_exec.c）：通常是先于主段提交的前置段；同一资源本次提交里第二次同步时结束当前通道、回到主段。
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last_sub);
// 遮挡计数（rp_exec.c）：通道与绘制那边的接入点
int rp_occl_want(void);                 // 开通道时：要不要声明遮挡计数
void rp_occl_pass_opened(int declared); void rp_occl_pass_closed(void);
void rp_occl_pause(nvn_gpu_cmd c);      // 通道里按矩形清之前
void rp_occl_draw(nvn_gpu_cmd c);       // 每笔绘制之前
void rp_dispatch(nvn_gpu_cmd c, const p_dispatch *dp, const void *rec);   // 计算派发（rp_draw.c）
void rp_post_gpu_write(uint64_t addr, uint64_t len);                      // 这一段里 GPU 写了游戏内存的这一段：完成后回写（rp_exec.c）
uint64_t rp_buf_gpu_write_note(uint64_t addr, uint64_t len, uint64_t *pool_key); void rp_buf_gpu_write_land(uint64_t pool_key, uint64_t id, int ok);
int rp_rc_skip(void);                   // 条件渲染判成不画的那一段里
void rp_pass_note_draw(int uses_ds);   // 一笔绘制进了当前通道（uses_ds：这笔开着深度测试 / 深度写 / 模板测试）
int rp_pass_slot_attached(int i);   // 当前目标的颜色槽 i 真的挂了附件（有图，且不是重复挂的同一张）：管线描述按它给格式
void rp_pass_slot_size(int i, uint32_t *w, uint32_t *h);   // 颜色槽 i 的视图尺寸（像素；块压缩基底配一块当一个像素的视图时按块数）
// 调试读回（rp_draw.c）：上屏线程登记，提交线程在一次提交收尾时执行；fence 是提交线程的时间线值，每读一张 +1
void rp_debug_dump_request(uint64_t present_serial, uint64_t frame);
void rp_debug_dump_poll(uint64_t *fence);
