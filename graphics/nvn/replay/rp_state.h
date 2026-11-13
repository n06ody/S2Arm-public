// 回放状态：命令流里的状态类命令只改这里，绘制时才生效（对齐 nvn_mtl.m 的 S / R 与 state_op）。
// 跨命令缓冲、跨提交都保留；只在首次回放和会话收尾时重置（nvn_mtl.m:14492、nvn_session.inc:141）。
#pragma once
#include "nvn_obj.h"
#include "nvn2_cmd.h"

#define RP_NTEX 256

struct rp_state {
    const prog_t *bst[5];      // 各图形阶段绑定的程序（NVN 阶段位 1 顶点 2 片元 4 几何 8 / 16 细分）
    uint64_t bser[5];          // 绑定当时的序号：之后对象被 Finalize 或复用时按序号取（见 rp_state.c stage_bind）
    const prog_t *cprog; uint64_t cser;   // 计算阶段单独一份绑定
    uint8_t skey[6][8]; uint8_t skey_ok;   // 各 NVN 阶段（0 顶点 … 5 计算）绑定当时的着色器键；位 st = 该阶段的键有效
    p_prim_restart restart;
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream;
    NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][RP_NTEX], samp[6][128], img[6][32];
    uint8_t tex_diag_written[6][128]; // 一次性探针：分离贴图槽曾收到绑定，零句柄也算
    uint8_t tex_diag_enabled;         // 会话启动时读取一次诊断开关
    uint8_t samp_bound[6][128]; // 显式绑定过，句柄 0 本身也是合法池项
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][4]; float dr[2]; int has_vp; int depth_clamp; uint8_t vp_sw[16][4];
    float blend_color[4];
    p_tf_bind tf[4]; uint64_t tf_off[4]; int tf_on, rast_discard; uint64_t tf_ctl;
    int tex_base, samp_base;   // 当前绑定的贴图池 / 采样器池在宿主表里的起点
    int tex_pool_n, samp_pool_n;   // 当前绑定的两个描述符池的容量（0 = 没绑过）
    float alpha_ref;
    int patch_size;
    uint8_t push[2][NVN2_PUSH_SLOTS][NVN2_PUSH_BYTES];
    uint64_t push_ver[2][NVN2_PUSH_SLOTS];
};
struct rp_target { p_scissor sc; int has_sc; };

extern struct rp_state rp_S;
extern struct rp_target rp_R;
// 脏位：只决定绘制时要不要重新取管线 / 深度模板状态 / 重发某阶段的绑定。值没变不标脏。
extern uint8_t rp_pso_dirty, rp_dss_dirty, rp_args_dirty[6];
extern uint64_t rp_tex_sep_gen[6];   // 各阶段分离贴图槽（rp_S.tex[阶段][128..]）的代号：有一格变了就加一；整份状态复位时也加

void rp_state_reset(void);                                    // NVN 默认值
int rp_state_op(uint32_t op, const void *a, uint32_t size);   // 状态类命令：1 = 已处理；0 = 不是状态类命令
void rp_dirty_all(void);                                      // 新的后端命令段开头：已发给后端的状态全部重发（逻辑状态不动）
