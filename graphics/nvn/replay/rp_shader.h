// 回放核心 ↔ 着色器来源的接口。
// 核心只知道「给这一笔绘制的事实，要这组阶段的后端着色器和它的绑定布局」；翻译、离线库、跨局缓存、现场编译都在来源那一侧。
// 来源的实现不在核心里：现在在 android/native/gfx_plat/nvk_shader.c（它用的编译链零件搬进共用目录后，这个实现跟着搬，鸿蒙共用）。
#pragma once
#include "nvn_backend.h"

enum { RP_PROG_STAGES = 3, RP_PROG_ENT_MAX = 128 };   // 顶点、片元、几何
// 句柄放在游戏用户常量缓冲里的绑定（NVN_RB_TEXU / NVN_RB_TBUFU，语义照 iOS nvn_refl.h / nvn_mtl.m ubo_packed）的来源。
//   一个字：用户常量槽 reg 的字节 off 处的 32 位字；sid = 它是采样器号（读出后左移 20）；pair = 同一 Ryujinx 句柄的另一半
//   （pair_reg / pair_off 处的字，pair_sid 时左移 20），两半相或 = 打包句柄：低 20 位纹理号、高 12 位采样器号。
struct rp_uword { uint8_t reg, sid, pair, pair_sid, pair_reg; uint16_t off, pair_off; };
// 一项的完整来源：tex = 贴图句柄字；samp_kind = 组合采样的采样器来源（0 = 着色器没有采样器半边；NVN_RB_SAMPU = samp 处的字；
//   NVN_RB_SAMPI = 常量采样器号 samp_reg；NVN_RB_SAMP = NVN 采样器槽 samp_reg 绑的句柄）。布局项的 src_reg2 = 本表下标。
//   SAMPI 有两种口径，用 samp_pool 分开（不挤进 samp_reg 的某一位：samp_reg 的 16 位全是号）：
//     samp_pool = 0：宿主登记的采样器号原值（离线库 {"source":"id"}，iOS 口径），不加采样器池起点；
//     samp_pool = 1：着色器里的立即数采样器池下标（#1594 片 3，类型 3 句柄，硬件的 12 位采样器号），采样器号 = 贴图字高 12 位 | samp_reg
//                    （Ryujinx ReadPackedId；字高位为 0 时就是 samp_reg），登记号 = 采样器号 + 当前采样器池起点，
//                    与 NVN_RB_SAMPU 读出的句柄高 12 位、c2 常量采样器（布局项 src_reg2 的 NVN_GPU_SAMP2_CONST）同口径。
struct rp_usrc { struct rp_uword tex, samp; uint8_t samp_kind; uint16_t samp_reg; uint8_t samp_pool; };
enum { RP_PROG_USRC_MAX = 64 };
struct rp_prog {
    nvn_gpu_shader sh[RP_PROG_STAGES];                        // 0 顶点 1 片元 2 几何
    nvn_gpu_layout_ent ent[RP_PROG_STAGES][RP_PROG_ENT_MAX];  // 与交给后端装载的布局同一份：核心按 src_kind / src_reg 逐项绑定
    uint8_t dtt[RP_PROG_STAGES][RP_PROG_ENT_MAX];             // 声明目标 + 1；0 = 未知，沿用登记视图
    uint8_t dcls[RP_PROG_STAGES][RP_PROG_ENT_MAX];            // 0 float，1 uint，2 sint，3 depth 比较
    uint8_t difmt[RP_PROG_STAGES][RP_PROG_ENT_MAX];           // 存储图像声明的 SPIR-V Image Format（0 = Unknown；计算阶段与图形阶段的 imageBuffer 填，见 nvk_shader.c sampled_decl）
    uint8_t sampler_unused[RP_PROG_STAGES][RP_PROG_ENT_MAX];   // 已从 SPIR-V 证明该组合描述符不读取采样器
    uint32_t nent[RP_PROG_STAGES];
    uint32_t support_bytes;                                   // NVN_RB_SUPPORT 那一项的缓冲长度
    uint32_t attr_mask;                                       // 顶点着色器实际读的属性位
    uint32_t color_out_mask;                                  // 片元着色器写了哪些输出分量：每个颜色目标 4 位（RGBA）。没有片元阶段 / 不知道 = 全 1
    const uint8_t *c1[RP_PROG_STAGES]; uint32_t c1_len[RP_PROG_STAGES]; uint8_t c1_ok;   // NVN_RB_C1：着色器自带的常量段，整段（位 i = 阶段 i 有；长度至少 0x100，
                                                              //   着色器按 vec4 下标可读到整段末尾——八方旅人调色 LUT 读到 2304 字节；iOS nvn_mtl.m c1_store 同样整段，专用缓冲 64 KB）
                                                              //   存储至少有 c1_len 向上补齐到 16 字节那么长（补齐部分为 0），绑定按补齐后的长度
    uint32_t spill_bytes;                                     // 计算：共享内存溢出缓冲每个线程组的字节数（NVN_RB_SPILL 项）；0 = 没有
    uint8_t is_compute;                                       // 计算程序：只有 sh[0] / ent[0]，绑定取 NVN 阶段 5 的状态
    uint8_t binst_unread;                                     // 位 i = 阶段 i 已证不读 c0 + 0x644（编译器 c0Reads）：间接绘制不用把 GPU 侧的 base instance 拷进它的驱动段。0 = 读或未知（照旧拷）
    uint8_t z_raw;                                            // 顶点出口 z 原样（不随深度模式改写，出包预编库的程序）：near = -w 时管线设裁剪深度 [-1, 1]
    uint8_t lib_sourced;                                      // 来自出包预编库（libpack）。绑定核对发现声明与实际绑定不符时，rp_shader_lib_reject 据此走现编回退
    uint8_t lib_bad;                                          // 这份程序身份已按绑定核对拒收：解析路径（含事实复用 / 结果合并）绕过它，下次取现编版
    uint8_t layer_out;                                        // 顶点 / 几何阶段写渲染目标层号（BuiltIn Layer）：这笔按分层通道画，图元按层号落到附件对应层（iOS nvn_mtl.m layer_out → renderTargetArrayLength）
    struct rp_usrc usrc[RP_PROG_USRC_MAX]; uint32_t nusrc;    // NVN_RB_TEXU / TBUFU 项的句柄来源（布局项 src_reg2 指向这里）
    // 内容身份：S2AI1 v3 规范输入的 SHA-256，也就是来源缓存（g_progs）的键。管线配方与跨会话预热清单
    // 用它跨会话定位「同一份程序」；全 0 = 没有身份（失败项 / 还没算出来），这种程序不进任何清单。
    uint8_t identity[32];
};
// 可选的单笔诊断，由调用方清零，解析期间只由当前提交线程写。
struct rp_shader_hitch {
    // 互不重叠的 CPU 墙钟区间。命中/拒收覆盖已知结果，普通 miss 留在 resolve 总耗时中。
    // inflight 是等待同身份程序发布，worker 是等待实时编译队列，compile 是当前线程同步编译。
    uint64_t hit_ns, reject_ns, inflight_ns, compile_ns, worker_ns, build_ns;
    uint8_t first_identity[32], last_identity[32];
    unsigned candidates, hits, rejects;
};
struct rp_shader_req {
    uint32_t prim;              // NVNdrawPrimitive
    int rt_count;               // 当前颜色目标个数
    uint32_t rt_format[8];      // 各颜色目标的 NVN 格式（视图改了格式的取视图的）；0 = 空槽
    // 仅在提交线程发现现编结果尚未就绪时调用。可暂缓则返回 1 并写入预算所属帧。
    int (*compile_defer_frame)(uint64_t *frame);
    struct rp_shader_hitch *hitch;
};
// 当前绘制生效的图形阶段（bit = NVN 阶段 0..4）与各阶段的着色器键。核心按「程序实际有哪些阶段」
// 窄化后的结果（同 iOS bind_stages：VS/FS 不能被关，GS/TCS/TES 程序没有就解绑）；着色器来源只消费这份结果。
uint32_t rp_shader_graphics_stages(void);
int rp_shader_stage_key(unsigned st, uint8_t key[8]);   // 1 = 该阶段已绑定且有键；0 = 没绑定 / 键缺失
// 取当前回放状态（rp_S）下这笔绘制的程序。0 = 得到；1 = 还没好，跳过这笔；-1 = 取不到（已记账）。线程：[提交]。
int rp_shader_resolve(const struct rp_shader_req *q, const struct rp_prog **out);
// 取当前回放状态下计算阶段（NVN 阶段 5）绑着的程序。返回值同上。
int rp_shader_resolve_compute(const struct rp_prog **out);
struct rp_shader_live_compile_stats {
    uint64_t enqueued, wait_ns, pending, fallback, timeout;
    uint64_t compiled, compile_ns, compile_max_ns;   // 编译线程这段时间跑完的份数（成败都算）/ 合计耗时 / 最长一份（排队等候不算）
};
void rp_shader_live_compile_take(struct rp_shader_live_compile_stats *out);
// 程序的内容身份（见 struct rp_prog.identity）。1 = 有；0 = 没有（p 为空 / 失败项 / 计算旧路径没记）。
// 线程：[任意]（只读，不加锁）。生命周期：p 有效期间有效。
int rp_shader_prog_identity(const struct rp_prog *p, uint8_t out[32]);
// 按内容身份取已发布到来源缓存的程序：1 = 有；0 = 还没发布 / 这份身份不在本局。
// 给预热后台线程按管线配方找程序用（建管线要拿程序阶段的着色器句柄）。
// 线程：[后台]。生命周期：借用指针，到会话收尾（rp_shader_reset）前有效：取到之后不允许再等编译。
int rp_shader_prog_by_identity(const uint8_t identity[32], const struct rp_prog **out);

// 程序借用：后台消费者（管线预热、预取）从 rp_shader_prog_by_identity 拿到 rp_prog* 之后，整段使用期间
// 必须持有借用。收尾（rp_shader_reset）与预热静默判据都等借用归零才允许释放程序记录：代际判断只能决定
// 「迟到结果还能不能安装」，不能证明对象仍存活。
//   1 = 借到（*out 有效，跨线程只读）；0 = 这个身份还没发布；-1 = 会话已停 / 参数问题。
int rp_shader_prog_borrow(const uint8_t identity[32], const struct rp_prog **out);
void rp_shader_prog_return(const uint8_t identity[32]);
int  rp_shader_prog_borrowed(void);   // 当前未归还的借用数（静默 / 收尾判据）
// 绑定核对（声明 vs 实际绑定）在绘制 / 派发时失败：这份程序若是出库版（lib_sourced），把它的身份标记
// 为「不再取出包库、不再按已发布项 / 别名命中」——调用方再调一次 resolve 即得现编版（同一笔换程序重新绑定）。
//   1 = 已标记，值得重取；0 = 不是出库版（没有可退的替代，照旧具名跳过）。线程：[提交]。
int rp_shader_lib_reject(const struct rp_prog *p);
void rp_shader_reset(void);     // 会话收尾：放掉全部着色器
// 游戏常量缓冲里的贴图句柄指向没登记的号、按默认描述符给了编译事实的次数（信息计数，不是未接；会话收尾清零）。#1594 片 4
uint64_t rp_shader_ubo_default_fact_count(void);
