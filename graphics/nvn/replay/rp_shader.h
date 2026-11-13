// 回放核心 ↔ 着色器来源的接口。
// 核心只知道「给这一笔绘制的事实，要这组阶段的后端着色器和它的绑定布局」；翻译、离线库、跨局缓存、现场编译都在来源那一侧。
// 来源的实现不在核心里：现在在 android/native/gfx_plat/nvk_shader.c（它用的编译链零件搬进共用目录后，这个实现跟着搬，鸿蒙共用）。
#pragma once
#include "nvn_backend.h"

enum { RP_PROG_STAGES = 3, RP_PROG_ENT_MAX = 128 };   // 顶点、片元、几何
struct rp_prog {
    nvn_gpu_shader sh[RP_PROG_STAGES];                        // 0 顶点 1 片元 2 几何
    nvn_gpu_layout_ent ent[RP_PROG_STAGES][RP_PROG_ENT_MAX];  // 与交给后端装载的布局同一份：核心按 src_kind / src_reg 逐项绑定
    uint8_t dtt[RP_PROG_STAGES][RP_PROG_ENT_MAX];             // 声明目标 + 1；0 = 未知，沿用登记视图
    uint8_t dcls[RP_PROG_STAGES][RP_PROG_ENT_MAX];            // 0 float，1 uint，2 sint，3 depth 比较
    uint32_t nent[RP_PROG_STAGES];
    uint32_t support_bytes;                                   // NVN_RB_SUPPORT 那一项的缓冲长度
    uint32_t attr_mask;                                       // 顶点着色器实际读的属性位
    uint32_t color_out_mask;                                  // 片元着色器写了哪些输出分量：每个颜色目标 4 位（RGBA）。没有片元阶段 / 不知道 = 全 1
    uint8_t c1[RP_PROG_STAGES][0x100]; uint8_t c1_ok;         // NVN_RB_C1：着色器自带的常量段（位 i = 阶段 i 有）
    uint8_t is_compute;                                       // 计算程序：只有 sh[0] / ent[0]，绑定取 NVN 阶段 5 的状态
};
struct rp_shader_req {
    uint32_t prim;              // NVNdrawPrimitive
    int rt_count;               // 当前颜色目标个数
    uint32_t rt_format[8];      // 各颜色目标的 NVN 格式（视图改了格式的取视图的）；0 = 空槽
};
// 当前绘制生效的图形阶段（bit = NVN 阶段 0..4）与各阶段的着色器键。核心按「程序实际有哪些阶段」
// 窄化后的结果（同 iOS bind_stages：VS/FS 不能被关，GS/TCS/TES 程序没有就解绑）；着色器来源只消费这份结果。
uint32_t rp_shader_graphics_stages(void);
int rp_shader_stage_key(unsigned st, uint8_t key[8]);   // 1 = 该阶段已绑定且有键；0 = 没绑定 / 键缺失
// 取当前回放状态（rp_S）下这笔绘制的程序。0 = 得到；1 = 还没好，跳过这笔；-1 = 取不到（已记账）。线程：[提交]。
int rp_shader_resolve(const struct rp_shader_req *q, const struct rp_prog **out);
// 取当前回放状态下计算阶段（NVN 阶段 5）绑着的程序。返回值同上。
int rp_shader_resolve_compute(const struct rp_prog **out);
void rp_shader_reset(void);     // 会话收尾：放掉全部着色器
