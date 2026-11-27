// 绘制（对齐 nvn_mtl.m draw / draw_emit @12900 一带与 encode_args_ @9640 的职责）：
// 状态类命令早已落在 rp_S 里，这里在绘制时把它们变成后端的管线、动态状态、顶点流和绑定，再发绘制。
// 没改过的不重发：管线看 rp_pso_dirty，各阶段绑定看 rp_args_dirty[阶段]。
// 四边形图元（NVN 7）没有原生对应：在这里展开成三角形列表的 32 位索引（每个四边形 0,1,2 / 0,2,3，同 nvn_mtl.m fanquad_expand），按带索引的三角形画。
// 另外三种没有原生对应的图元（#710）：四边形带（8）当三角带画（顶点序不变；零头 1 个丢掉；带索引又开着重启时按段展开成三角形列表），
// 多边形（9）当三角扇画，线环（2）展开成线段列表的 32 位索引（每段末尾补一条回到段首的线）。
// 还没接：间接计数多绘、变换反馈、邻接图元与面片、图元重启以外的小状态、存储缓冲与图像绑定、空槽假贴图。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1   // process_vm_readv（逐笔日志经内核读游戏内存）；编译参数已定义的不重复
#endif
#include "rp_exec.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_shader.h"
#include "rp_access.h"
#include "nvn_unimpl.h"
#include "nvn_hmap.h"
#include "nvn_plat.h"
#include "nvn_refl.h"
#include "nvn_fmt_info.h"
#include "nvn_dirty.h"   // 写跟踪：暂存片的逐页写代号（nvk_slc_gen 用）
#include "nvn_first_use.h"   // iOS pso_pend 语义共用件：fresh/seen 历史 / 帧预算 / wait_decide
#include "nvn_pso_retry.h"   // iOS 暂时性建管线失败的退避节奏（图形管线失败表用）
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#else
#include <sys/uio.h>
#endif

// 弱链接登记（tools/weak_link_decl.py，场合 replay = 回放核心宿主单测）：后端 / 首用模块的可选件，缺实现时地址为空、走不到调用
// 弱链接[replay]：nvk_fu_frame_ms
// 弱链接[replay]：nvk_fu_item_ms
// 弱链接[replay]：nvk_fu_unsafe_ms
// 弱链接[replay]：nvk_pipe_warm_note_prog
// 弱链接[replay]：nvk_shader_cs_lib_note
// 弱链接[replay]：nvn_vk_pipe_demand_kick
// 弱链接[replay]：nvn_vk_pipe_demand_mark
// 弱链接[replay]：nvn_vk_pipe_demand_marks
// 弱链接[replay]：nvn_vk_pipe_demand_on
// 弱链接[replay]：rp_shader_lib_reject
// 弱链接[replay]：nvn_vk_pipe_demand_wait
// 弱链接[replay]：s2a_fu_budget_left_ms
// 弱链接[replay]：s2a_fu_budget_spend
// 弱链接[replay]：s2a_fu_rt_fresh_now
// 弱链接[replay]：s2a_fu_rt_mark_seen
// 弱链接[replay]：s2a_fu_rt_seen_steady
// 弱链接[replay]：s2a_fu_wait_decide

// 弱链接[replay]：rp_cold_submit_scope
// 弱链接[replay]：rp_pass_slot_planned
// 弱链接[replay]：nvn_gpu_pipe_lookup
// 弱链接[replay]：nvn_vk_pipe_demand_cold_kick
// 弱链接[replay]：nvn_vk_cold_guard_auto
// 弱链接[replay]：nvn_vk_cold_guard_draw
int rp_cold_submit_scope(nvn_gpu_cmd, nvn_gpu_cmd *) __attribute__((weak));
int rp_pass_slot_planned(int) __attribute__((weak));
int nvn_gpu_pipe_lookup(const nvn_gpu_pipe_desc *, nvn_gpu_pipe *) __attribute__((weak));
int nvn_vk_pipe_demand_cold_kick(const nvn_gpu_pipe_desc *, const uint8_t[32], const uint8_t[32],
                                nvn_gpu_pipe *) __attribute__((weak));
int nvn_vk_cold_guard_auto(nvn_gpu_cmd, nvn_gpu_cmd, nvn_gpu_pipe, nvn_gpu_pipe,
                           const nvn_gpu_draw_args *, const struct nvn_gpu_cold_domain *) __attribute__((weak));
int nvn_vk_cold_guard_draw(nvn_gpu_cmd, const nvn_gpu_draw_args *) __attribute__((weak));

// SupportBuffer 里现在要填的字段（翻译器产物的布局；其余保持 0）
enum { SB_RENDER_SCALE = 192, SB_S2A_FLAGS = 1392, SB_S2A_POINT = 1408, SB_RENDER_SCALE_Y = 1440, SB_RENDER_SCALE_COUNT = 73 };
// 驱动常量段 c0 的合成布局（着色器从它读存储缓冲描述符表、最大点径、base instance）
enum { DRV0_BYTES = 0xA00 };
static const int k_drv0_base[5] = {0x110, 0x510, 0x410, 0x210, 0x310};   // 各图形阶段的存储缓冲表起点

// 逐笔日志读游戏内存（贴图首 8 字节、常量缓冲前 20 个四元组）：这段内存可能整页无读权限（真机重场景里被采样的一批贴图在池标 0x2a1 =
// 虚拟 | 可压缩 | GPU 缓存 | CPU 不可访问 的池里，回放自己对这种池不读，cpu_content 见标志就返回 0），
// 直接解引用会让回放线程吃 SIGSEGV（设备读数：开逐笔日志后重场景第一帧就崩在这里，SEGV_ACCERR 读）。
// 经内核读：读不到只回 0（out 清零），不发信号。
static int dbg_peek(const void *p, void *out, size_t n) {
    memset(out, 0, n);
#if defined(__APPLE__)
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)(uintptr_t)p, (vm_size_t)n, (vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == n;
#else
    struct iovec l = {out, n}, r = {(void *)(uintptr_t)p, n};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
#endif
}

int rp_pool_find(uint64_t addr, uint64_t len, const uint8_t **mem, uint64_t *size);   // rp_entry.c
extern int s2a_nvn_rast_flip;   // hle_nvn.c：当前提交队列的正面光栅翻转位（建队列时按窗口原点定）
// 管线预热清单（安卓后端件 android/native/nvn/nvn_vk_pipe_warm.c）：真实创建成功后记一条配方，下一局
// 开局由预热线程按同一份配方提前把管线建好，真实 draw 直接命中后端缓存。声明成弱符号：iOS 与宿主单测
// 没有这份实现，缺了就当作「不记清单」（不拦执行、不返回假成功）。程序身份在那边从 rp_prog 取，程序
// 没有身份（失败项 / 还没算出来）就不记。np 是刚取到的后端管线：清单在它上面存一个标签，之后换回同一条
// 管线只加计数（不序列化、不散列、不查表）。
int nvk_pipe_warm_note_prog(const nvn_gpu_pipe_desc *d, const struct rp_prog *p, nvn_gpu_pipe np) __attribute__((weak));
const char *nvk_shader_cs_lib_note(const uint8_t key[8]) __attribute__((weak));   // 计算阶段键没走出包预编库的原因（诊断；苹果侧没有这个符号）

// 句柄放在游戏用户常量缓冲里的贴图（NVN_RB_TEXU；语义照 iOS nvn_mtl.m ubo_read / ubo_handle / ubo_packed / ubo_tex_id / ubo_samp_id / bind_sid）。
// 一个字：常量槽 reg 的字节 off 处，编码这一刻从游戏内存读（同 NVN_RB_CBUF 绑的就是这一刻的内容）；槽没绑、越界、不在登记过的池里读作 0（iOS：空槽）
// 用户常量缓冲的有效长度：NVN 声明为 64 KiB（MAX_UNIFORM_BUFFER_SIZE、Ryujinx ConstantBufferSize），
// 超出按 64 KiB 截短。准备阶段（预解析镜像）与发绑定两处必须用同一个值，否则镜像长度与绑定长度各算各的。
static inline uint64_t cbuf_len(const p_bind_buf *ub) {
    enum { CBUF_SHADER_BYTES = 0x10000 };
    return ub->size < CBUF_SHADER_BYTES ? ub->size : CBUF_SHADER_BYTES;
}

static uint32_t ubo_word(int st, unsigned reg, unsigned off) {
    if (st < 0 || st >= 6 || reg >= 16) return 0;
    const p_bind_buf *u = &rp_S.ubo[st][reg];
    if (!u->addr || (uint64_t)off + 4 > u->size || !rp_pool_find(u->addr + off, 4, NULL, NULL)) return 0;
    uint32_t h; memcpy(&h, (const void *)(uintptr_t)(u->addr + off), 4);
    return h;
}
// 打包句柄（Ryujinx ReadPackedId）：自己这个字（采样器号字左移 20）或上同一句柄另一半的字（pair_sid 时左移 20）
static uint32_t ubo_packed(int st, const struct rp_uword *w) {
    uint32_t h = ubo_word(st, w->reg, w->off);
    if (w->sid) h <<= 20;
    if (w->pair) { uint32_t o = ubo_word(st, w->pair_reg, w->pair_off); h |= w->pair_sid ? o << 20 : o; }
    return h;
}
// 这一项这一笔的纹理号 / 采样器号（宿主登记表里的号）。NULL = 取到；否则是跳过这笔的原因。
//   纹理号 = 打包句柄低 20 位 + 当前贴图池起点（ID 0 是池的第 0 项，同硬件）。
//   采样器：SAMPU = 打包句柄高 12 位 + 采样器池起点；SAMPI = 着色器里的常量号：samp_pool = 0 取原值（离线库的登记号），
//   samp_pool = 1 是立即数采样器池下标（#1594 片 3）、采样器号 = 贴图字高 12 位 | 下标（const_samp_id，同 Ryujinx ReadPackedId），登记号 = 采样器号 + 采样器池起点（同 SAMPU，号 0 取池第 0 项）；
//   SAMP = NVN 采样器槽的句柄高 12 位（没绑过照 NVN_RB_TEX 分离采样器口径跳过）。
//   iOS 把采样器号 0 与「没有采样器半边」绑成假采样器；这里没有假采样器，取采样器池第 0 项（= 号 0 在硬件上的意思）
// 类型 3 句柄（SeparateConstantSamplerHandle）的采样器号：Ryujinx TextureBindingsManager.ReadPackedId 对它 samplerHandle = 立即数、<<= 20、handle |= samplerHandle，
//   采样器号取 handle >> 20，即 贴图字高 12 位 | 立即数池下标。贴图字高 12 位为 0 时就是立即数；c2 分离贴图表的槽与游戏 UBO 里的字两种来源共用这一处。
//   imm 不再掩：合同内 ≤ 4095（解析与库校验都拒更大的），越界的值落到没登记的号（片 6 起绑默认采样器），不折回低位去绑别的采样器。
static inline int const_samp_id(uint32_t tex_word, unsigned imm) { return (int)(((tex_word >> 20) & NVN_GPU_SAMP2_ID_MASK) | imm); }
static const char *usrc_ids(int st, const struct rp_usrc *u, int *tid, int *sid) {
    *tid = (int)(ubo_packed(st, &u->tex) & 0xFFFFF) + rp_S.tex_base;
    if (u->samp_kind == NVN_RB_SAMPU) *sid = (int)(ubo_packed(st, &u->samp) >> 20) + rp_S.samp_base;
    else if (u->samp_kind == NVN_RB_SAMPI) *sid = u->samp_pool ? const_samp_id(ubo_packed(st, &u->tex), u->samp_reg) + rp_S.samp_base : u->samp_reg;
    else if (u->samp_kind == NVN_RB_SAMP) {
        if (u->samp_reg >= 128 || !rp_S.samp_bound[st][u->samp_reg]) return "separate_sampler_unbound";
        *sid = (int)(rp_S.samp[st][u->samp_reg] >> 20);
    } else *sid = rp_S.samp_base;
    return NULL;
}

// 布局项分离采样器来源 src_reg2 的形状（nvn_backend.h）：UINT16_MAX = 无；0..127 = NVN 采样器槽；
//   NVN_GPU_SAMP2_CONST | 12 位池下标 = 着色器里的立即数采样器池下标（#1594，标记位与下标位之外必须为 0）。别的值认不出，调用方具名跳过。
static int samp2_shape_ok(uint16_t r) {
    if (r == UINT16_MAX) return 1;
    if (r & NVN_GPU_SAMP2_CONST) return !(r & ~(NVN_GPU_SAMP2_CONST | NVN_GPU_SAMP2_ID_MASK));
    return r < 128;
}

static const struct rp_prog *g_prog; static nvn_gpu_pipe g_pipe; static nvn_gpu_cmd g_pipe_cmd;
static uint64_t g_draws, g_skips, g_dispatches; static uint32_t g_prim = ~0u; static int g_quads = -1, g_raster = -1;   // g_quads：这笔的索引展开方式（EXP_*）
static int g_ms_off;   // #1015：当前管线是「多重采样目标上关了多重采样」（游戏 ms.enable = 0、目标样本数 > 1）

// ---- 首用管线异步：后台建 + 可恢复的首次缺画（iOS pso_pend / first_skip_ok 的 Vulkan 新回放版）----
// 弱引用安卓 demand 层与三档上限读数：iOS / 鸿蒙 / 宿主单测没有这份实现 → rp_fu_enabled()=0，行为原样退回 SYNC。
// 状态值必须与 android/native/nvn/nvn_vk_pipe_demand.h 的 NVK_DEMAND_* 一致（那边是强定义，这里只做弱引用）。
enum { RP_FU_DEMAND_NONE = 0, RP_FU_DEMAND_PENDING = 1, RP_FU_DEMAND_READY = 2, RP_FU_DEMAND_FAILED = 3 };
int  nvn_vk_pipe_demand_kick(const nvn_gpu_pipe_desc *, const uint8_t[32], nvn_gpu_pipe *) __attribute__((weak));
int  nvn_vk_pipe_demand_wait(const nvn_gpu_pipe_desc *, const uint8_t[32], uint32_t, nvn_gpu_pipe *, char *, size_t) __attribute__((weak));
int  nvn_vk_pipe_demand_on(void) __attribute__((weak));
int  rp_shader_lib_reject(const struct rp_prog *p) __attribute__((weak));
void nvn_vk_pipe_demand_mark(const nvn_gpu_pipe_desc *, const uint8_t[32], int is_stuck) __attribute__((weak));
int  nvn_vk_pipe_demand_marks(const nvn_gpu_pipe_desc *, const uint8_t[32], int *late, int *stuck) __attribute__((weak));
uint32_t nvk_fu_item_ms(void) __attribute__((weak));
uint32_t nvk_fu_frame_ms(void) __attribute__((weak));
uint32_t nvk_fu_unsafe_ms(void) __attribute__((weak));
// s2a_fu_*（nvn_first_use.c）在只编回放核的孤立宿主门禁里可能没链接；weak 化后缺实现时 rp_fu_enabled() 也为假
// （demand_on 亦弱），根本走不到这些调用，门禁照常链接。生产 NVK 源里 nvn_first_use.c 是强定义，弱引用被强解析。
#pragma weak s2a_fu_rt_fresh_now
#pragma weak s2a_fu_rt_seen_steady
#pragma weak s2a_fu_budget_left_ms
#pragma weak s2a_fu_budget_spend
#pragma weak s2a_fu_wait_decide
#pragma weak s2a_fu_rt_mark_seen

// 弱链接[replay]：rp_occl_want
#pragma weak rp_occl_want
static int rp_fu_enabled(void) { return nvn_vk_pipe_demand_on && nvn_vk_pipe_demand_on(); }
static uint64_t rp_fu_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static struct s2a_fu_budget g_fu_budget;   // 本帧首用等待总账（iOS g_pp_bud_*）；只提交线程碰
static uint64_t g_fu_skip_n, g_fu_skip_budget, g_fu_skip_late, g_fu_skip_stuck;
static void rp_fu_skip_note(int kind) {
    uint64_t n = ++g_fu_skip_n;
    if (kind == S2A_FU_T_BUDGET) g_fu_skip_budget++;
    else if (kind == S2A_FU_T_STUCK) g_fu_skip_stuck++;
    else g_fu_skip_late++;
    if (n == 1 || !(n & (n - 1))) {
        char b[180];
        snprintf(b, sizeof b, "首用跳过累计 %llu 笔（budget %llu / late %llu / stuck %llu）",
                 (unsigned long long)n, (unsigned long long)g_fu_skip_budget,
                 (unsigned long long)g_fu_skip_late, (unsigned long long)g_fu_skip_stuck);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "draw", b);
    }
}
// 本笔是否写 storage（无反射 rw 位，保守口径）：transform feedback / 绑了 SSBO / 程序声明了存储图像 → 不可丢。
static int rp_fu_storage_write(void) {
    if (rp_S.tf_on) return 1;
    for (int st = 0; st < 6; st++)
        for (int i = 0; i < 16; i++)
            if (rp_S.ssbo[st][i].addr) return 1;   // 地址有效、长度 0 的槽也写真实内存（见 sb_len）
    if (g_prog)
        for (int st = 0; st < RP_PROG_STAGES; st++)
            for (uint32_t i = 0; i < g_prog->nent[st]; i++)
                if (g_prog->ent[st][i].src_kind == NVN_RB_IMG) return 1;
    return 0;
}
// 程序的着色器会不会写存储缓冲 / 存储图像（布局项的写访问位；同 iOS nvn_mtl.m swrite_note 的判据）。回放层按它记「通道里有着色器写」（rp_pass_note_draw），
// 游戏随后发顺序屏障时据此在下一笔绘制前拆通道（rp_pass.c rp_pass_barrier）。
static int prog_shader_writes(const struct rp_prog *pg) {
    if (!pg) return 0;
    for (int st = 0; st < RP_PROG_STAGES; st++)
        for (uint32_t i = 0; i < pg->nent[st]; i++) {
            const nvn_gpu_layout_ent *e = &pg->ent[st][i];
            if ((e->access & NVN_GPU_ACC_W) && (e->src_kind == NVN_RB_IMG || e->src_kind == NVN_RB_IMGU || e->src_kind == NVN_RB_IMGA || e->src_kind == NVN_RB_SBUFP || e->src_kind == NVN_RB_GMEM)) return 1;
        }
    return 0;
}
// iOS skip_safe：无 storage 写 + 每个颜色/深度目标本帧 fresh + 近两帧 steady。任一不满足就不许跳。
// 出库版程序（lib_sourced）在绑定核对里发现声明与实际绑定不符：身份标坏（rp_shader_lib_reject），按当前状态取现编版。
//   出包预编库按苹果端约定翻译（缺类型答案时一维取数译成二维行读，宿主侧按行绑缓冲贴图，nvn_mtl.m tbuf_as_2d），
//   声明不随这一刻绑的贴图变；现编按此刻的贴图事实翻译，声明与绑定一致。回退只做一次（lib_retry），现编版再不符照旧按原因处理。
//   返回新程序；NULL = 不是出库版 / 已回退过 / 取不到现编版（调用方照原路处理）。
static const struct rp_prog *lib_fallback_graphics(const struct rp_prog *p, int *lib_retry, uint32_t prim, const p_set_rt *rt) {
    if (!p->lib_sourced || *lib_retry || !rp_shader_lib_reject || !rp_shader_lib_reject(p)) return NULL;
    struct rp_shader_req q; memset(&q, 0, sizeof q); q.prim = prim; q.rt_count = rt->n;
    for (int k = 0; k < rt->n && k < 8; k++) if (rt->color[k]) q.rt_format[k] = (uint32_t)(rt->has_cview[k] && rt->cview[k].set_format ? rt->cview[k].format : rt->color[k]->d.format);
    const struct rp_prog *np = NULL;
    *lib_retry = 1;
    rp_view_pin_end();
    return !rp_shader_resolve(&q, &np) && np ? np : NULL;
}
static const struct rp_prog *lib_fallback_compute(const struct rp_prog *p, int *lib_retry) {
    if (!p->lib_sourced || *lib_retry || !rp_shader_lib_reject || !rp_shader_lib_reject(p)) return NULL;
    const struct rp_prog *np = NULL;
    *lib_retry = 1;
    rp_view_pin_end();
    return !rp_shader_resolve_compute(&np) && np && np != p ? np : NULL;
}
// 采样贴图取视图。出库版（还没回退过）带核对：声明目标与此刻绑的贴图桥接不了时返回 NULL、*unbridged = 1（不绑零贴图），
//   调用方换现编版重绑；其余同 rp_view_by_id_decl / rp_view_by_ubo_handle_decl。
static nvn_gpu_view sampled_view(const struct rp_prog *p, int lib_retry, int *unbridged, int ubo, int id, uint8_t dtt, uint8_t dcls, int elem, nvn_gpu_cmd c, tex_t *t_out) {
    const int probe = p->lib_sourced && !lib_retry && dtt;
    if (probe) rp_decl_probe_begin();
    nvn_gpu_view v = ubo ? rp_view_by_ubo_handle_decl(id, dtt, dcls, c, t_out) : rp_view_by_id_decl(id, dtt, dcls, elem, c, t_out);
    *unbridged = probe ? rp_decl_probe_end() : 0;
    return v;
}

static int rp_fu_skip_safe(const p_set_rt *rt) {
    if (!rt || rp_fu_storage_write()) return 0;
    const uint64_t frame = rp_frame();
    int targets = 0;
    for (int i = 0; i < rt->n && i < 8; i++) {
        const tex_t *t = rt->color[i];
        if (!t) continue;
        targets++;
        uint64_t sk = t->mem ? (uint64_t)(uintptr_t)t->mem : 0;
        if (!s2a_fu_rt_fresh_now(t->serial, frame)) return 0;
        if (!s2a_fu_rt_seen_steady(t->serial, sk, (uint64_t)t->d.format, frame)) return 0;
    }
    if (rt->depth) {
        targets++;
        uint64_t sk = rt->depth->mem ? (uint64_t)(uintptr_t)rt->depth->mem : 0;
        if (!s2a_fu_rt_fresh_now(rt->depth->serial, frame)) return 0;
        if (!s2a_fu_rt_seen_steady(rt->depth->serial, sk, (uint64_t)rt->depth->d.format, frame)) return 0;
    }
    return targets > 0;
}
// 还没有程序反射时也必须保护可能的写入和查询。逐槽检查实际绑定，不借上一笔程序的反射。
static int rp_live_compile_defer_frame(uint64_t *frame) {
    if (!rp_fu_enabled() || !rp_occl_want || rp_occl_want() ||
        !s2a_fu_rt_fresh_now || !s2a_fu_rt_seen_steady) return 0;
    for (unsigned st = 0; st < sizeof rp_S.img / sizeof rp_S.img[0]; st++)
        for (unsigned slot = 0; slot < sizeof rp_S.img[0] / sizeof rp_S.img[0][0]; slot++)
            if (rp_S.img[st][slot]) return 0;
    if (!rp_fu_skip_safe(rp_pass_target())) return 0;
    *frame = rp_frame();
    return 1;
}
static nvn_gpu_rc rp_fu_ok(void) { nvn_gpu_rc r; r.code = NVN_GPU_OK; return r; }
// ---------- 图形管线失败表 ----------
// 同一条建不出来的图形管线，原先每笔绘制都同步再建一次（真机一款 3D RPG 3 分钟 9.7 万次、7 帧）。口径对齐：
//   iOS nvn_mtl.m g_pso_fail（失败的管线记下、整局不再建）与 g_pso_retry + nvn_pso_retry.h（系统编译服务这类暂时性失败按 30·4^(n-1) 帧退避重试，
//   连续失败 PSO_RETRY_MAX 次才进失败表）；本文件计算管线的 g_cpipe_bad；Ryujinx PipelineState.CreateGraphicsPipelineCore 失败时
//   program.AddGraphicsPipeline(key, null)；Eden vk_pipeline_cache.cpp graphics_cache.try_emplace 先占位、建失败留空项不重建。
// 按后端返回码分三种（rp_pfail_kind）：
//   确定性：COMPILE（驱动编译 / vkCreateGraphicsPipelines 非内存类错误）、UNSUPPORTED（设备能力 / 本段未实现）、LAYOUT、ARG——同一份描述再建结果一样，记进失败表整局不再建；
//   暂时性：OOM（主存 / 显存 / 描述符池不够）——退避重试，等待期间这条管线的绘制跳过，连续 PSO_RETRY_MAX 次失败才进失败表；
//   不记：STOPPED（会话正在关闭）、DEVICE（设备丢失，终态，什么都建不了）及其他——每笔照旧交后端。
// 键是整份管线描述的 128 位摘要（rp_pfail_key）：后端建管线只看这份描述，描述相同 ⇒ 结果相同。会话重置（rp_draw_reset）清空。
// 分类：原因（返回码 + 后端原文里数字归一成 #）+ 粗签名（阶段、拓扑、颜色附件数、采样数、深度、变换反馈、顶点属性数）。
//   每类第一次失败打一行红（带签名与两个程序键），present 每 5 秒的汇总行按类列前 5（rp_pipe_fail_summary）。
enum { RP_PFAIL_NONE = 0, RP_PFAIL_FIXED = 1, RP_PFAIL_TRANSIENT = 2 };
static int rp_pfail_kind(int code) {
    switch (code) {
    case NVN_GPU_E_COMPILE: case NVN_GPU_E_UNSUPPORTED: case NVN_GPU_E_LAYOUT: case NVN_GPU_E_ARG: return RP_PFAIL_FIXED;
    case NVN_GPU_E_OOM: return RP_PFAIL_TRANSIENT;
    default: return RP_PFAIL_NONE;
    }
}
static const char *rp_gpu_code_name(int code) {
    switch (code) {
    case NVN_GPU_E_ARG: return "ARG"; case NVN_GPU_E_UNSUPPORTED: return "UNSUPPORTED"; case NVN_GPU_E_OOM: return "OOM";
    case NVN_GPU_E_DEVICE: return "DEVICE"; case NVN_GPU_E_COMPILE: return "COMPILE"; case NVN_GPU_E_LAYOUT: return "LAYOUT";
    case NVN_GPU_E_STOPPED: return "STOPPED"; default: return "OTHER";
    }
}
typedef struct { uint64_t a, b; } rp_pfail_k;
static inline uint64_t rp_pfail_mix(uint64_t h, uint64_t m, const void *p, size_t n) {
    const uint8_t *s = (const uint8_t *)p;
    for (; n >= 8; s += 8, n -= 8) { uint64_t w; memcpy(&w, s, 8); h = (h ^ w) * m; h ^= h >> 29; }
    if (n) { uint64_t w = 0; memcpy(&w, s, n); h = (h ^ w) * m; h ^= h >> 29; }
    return h;
}
// 着色器句柄 + 前 nspec 条特化常量 + 描述里阶段表之后的全部字节（调用方先把描述整个清零再填，填充位是 0）
static rp_pfail_k rp_pfail_key(const nvn_gpu_pipe_desc *d) {
    static const uint64_t m[2] = {0xff51afd7ed558ccdull, 0xc4ceb9fe1a85ec53ull}, s0[2] = {0x9e3779b97f4a7c15ull, 0x6a09e667f3bcc909ull};
    uint64_t h[2];
    for (int l = 0; l < 2; l++) {
        h[l] = s0[l];
        for (int i = 0; i < 5; i++) {
            const uintptr_t sh = (uintptr_t)d->st[i].sh; const uint32_t ns = d->st[i].nspec < NVN_GPU_SPEC_MAX ? d->st[i].nspec : NVN_GPU_SPEC_MAX;
            h[l] = rp_pfail_mix(h[l], m[l], &sh, sizeof sh); h[l] = rp_pfail_mix(h[l], m[l], &ns, sizeof ns);
            if (ns) h[l] = rp_pfail_mix(h[l], m[l], d->st[i].spec, (size_t)ns * sizeof d->st[i].spec[0]);
        }
        const size_t off = offsetof(nvn_gpu_pipe_desc, cfmt);
        h[l] = rp_pfail_mix(h[l], m[l], (const uint8_t *)d + off, sizeof *d - off);
        h[l] ^= h[l] >> 33; h[l] *= m[l ^ 1]; h[l] ^= h[l] >> 33;
    }
    rp_pfail_k k = {h[0], h[1]};
    return k;
}
// 粗签名
typedef struct { uint8_t stages, ncolor, depth, tf, nattr, _r[3]; uint32_t prim, samples; } rp_pfail_sig;
static rp_pfail_sig rp_pfail_sig_of(const nvn_gpu_pipe_desc *d) {
    rp_pfail_sig s; memset(&s, 0, sizeof s);
    for (int i = 0; i < 5; i++) if (d->st[i].sh) s.stages |= (uint8_t)(1u << i);
    for (int i = 0; i < 8; i++) if (d->cfmt[i]) s.ncolor++;
    for (int i = 0; i < 16; i++) if (d->attr[i].fmt) s.nattr++;
    s.depth = (uint8_t)(d->dfmt || d->sfmt); s.tf = (uint8_t)(rp_S.tf_on != 0); s.prim = d->prim; s.samples = d->samples;
    return s;
}
static void rp_pfail_sig_str(const rp_pfail_sig *s, char *b, size_t n) {
    static const char *const nm[5] = {"vs", "fs", "gs", "tcs", "tes"};
    char st[24] = ""; for (int i = 0; i < 5; i++) if (s->stages >> i & 1) { if (st[0]) strcat(st, "+"); strcat(st, nm[i]); }
    snprintf(b, n, "阶段 %s、拓扑 %u、颜色附件 %u、采样 %u、深度 %s、变换反馈 %s、顶点属性 %u",
             st[0] ? st : "-", (unsigned)s->prim, (unsigned)s->ncolor, (unsigned)s->samples, s->depth ? "有" : "无", s->tf ? "有" : "无", (unsigned)s->nattr);
}
// 原因：返回码名 + 后端原文，连续的数字（含 0x 十六进制）归一成 #，在 UTF-8 字边界上截断
static void rp_pfail_reason(int code, const char *why, char *b, size_t n) {
    size_t o = (size_t)snprintf(b, n, "%s:", rp_gpu_code_name(code));
    for (const char *p = why ? why : ""; *p && o + 2 < n;) {
        if (*p >= '0' && *p <= '9') {
            b[o++] = '#';
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
            while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) p++;
            continue;
        }
        size_t cl = (unsigned char)*p < 0x80 ? 1 : (unsigned char)*p >= 0xf0 ? 4 : (unsigned char)*p >= 0xe0 ? 3 : 2;
        if (o + cl + 1 > n) break;
        for (size_t i = 0; i < cl && p[i]; i++) b[o++] = p[i];
        p += cl;
    }
    b[o < n ? o : n - 1] = 0;
}
enum { RP_PFAIL_CLS_MAX = 32 };   // 类表上限；满了之后的新类并进最后一格「其它」
struct rp_pfail_cls { char reason[112]; rp_pfail_sig sig; uint64_t fails, cached; };
static struct rp_pfail_cls g_pfail_cls[RP_PFAIL_CLS_MAX + 1];
static unsigned g_pfail_ncls;
static uint64_t g_pfail_win;   // 上次汇总后有没有新的失败 / 挡下（没有就不打汇总行）
static nvn_hmap g_pipe_bad = {.klen = sizeof(rp_pfail_k)};     // 进了失败表的：值 = 类号 + 1
static nvn_hmap g_pipe_retry = {.klen = sizeof(rp_pfail_k)};   // 暂时性失败等重试的：值 = pso_retry_pack(重试帧, 连续失败次数)
static void rp_pfail_reset(void) {
    free(g_pipe_bad.keys); free(g_pipe_bad.vals); nvn_hm_init(&g_pipe_bad, sizeof(rp_pfail_k));
    free(g_pipe_retry.keys); free(g_pipe_retry.vals); nvn_hm_init(&g_pipe_retry, sizeof(rp_pfail_k));
    memset(g_pfail_cls, 0, sizeof g_pfail_cls); g_pfail_ncls = 0; g_pfail_win = 0;
}
// 查表：1 = 这条管线在失败表里或还在退避期，本笔跳过不交后端
static int rp_pfail_hit(const rp_pfail_k *k) {
    void *v = nvn_hm_get(&g_pipe_bad, k);
    if (v) { unsigned c = (unsigned)(uintptr_t)v - 1; if (c <= RP_PFAIL_CLS_MAX) g_pfail_cls[c].cached++; g_pfail_win++; return 1; }
    v = nvn_hm_get(&g_pipe_retry, k);
    if (v && pso_retry_frame((uint64_t)(uintptr_t)v) > rp_frame()) { g_pfail_win++; return 1; }
    return 0;
}
// 记一次后端失败。k 按本笔原本的描述算（点 / 线退回填充重试时 d 被改过，键仍记原样：下一笔来的还是原样）；d 只用来出粗签名
static void rp_pfail_note(rp_pfail_k k, const nvn_gpu_pipe_desc *d, int code, const char *why) {
    const int kind = rp_pfail_kind(code);
    if (kind == RP_PFAIL_NONE) return;
    char reason[sizeof g_pfail_cls[0].reason]; rp_pfail_reason(code, why, reason, sizeof reason);
    const rp_pfail_sig sig = rp_pfail_sig_of(d);
    unsigned c = 0;
    while (c < g_pfail_ncls && (strcmp(g_pfail_cls[c].reason, reason) || memcmp(&g_pfail_cls[c].sig, &sig, sizeof sig))) c++;
    if (c == g_pfail_ncls) {
        if (c < RP_PFAIL_CLS_MAX) { snprintf(g_pfail_cls[c].reason, sizeof g_pfail_cls[c].reason, "%s", reason); g_pfail_cls[c].sig = sig; g_pfail_ncls++; }
        else { c = RP_PFAIL_CLS_MAX; if (!g_pfail_cls[c].reason[0]) snprintf(g_pfail_cls[c].reason, sizeof g_pfail_cls[c].reason, "其它（类表满 %d 类后的新类）", RP_PFAIL_CLS_MAX); }
        if (c < RP_PFAIL_CLS_MAX || !g_pfail_cls[c].fails) {   // 每类第一次失败打一行红；总行数 ≤ RP_PFAIL_CLS_MAX + 1
            char sg[160], ks[2][17] = {"-", "-"}, b[640];
            rp_pfail_sig_str(&sig, sg, sizeof sg);
            for (unsigned st = 0; st < 2; st++) { uint8_t key[8]; if (rp_shader_stage_key(st, key)) for (int i = 0; i < 8; i++) snprintf(ks[st] + 2 * i, 3, "%02x", key[i]); }
            snprintf(b, sizeof b, "图形管线建失败 第 %u 类（%s）：原因 %s；%s；程序键 vs %s fs %s；后端原文：%s",
                     c + 1, kind == RP_PFAIL_FIXED ? "确定性，记进失败表不再建" : "暂时性，退避重试", reason, sg, ks[0], ks[1], why ? why : "");
            nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", b);
        }
    }
    g_pfail_cls[c].fails++; g_pfail_win++;
    if (kind == RP_PFAIL_FIXED) { nvn_hm_del(&g_pipe_retry, &k); nvn_hm_put(&g_pipe_bad, &k, (void *)(uintptr_t)(c + 1)); return; }
    void *v = nvn_hm_get(&g_pipe_retry, &k);
    const unsigned fails = (v ? pso_retry_fails((uint64_t)(uintptr_t)v) : 0) + 1;
    const uint64_t at = pso_retry_at(rp_frame(), fails);
    if (!at) { nvn_hm_del(&g_pipe_retry, &k); nvn_hm_put(&g_pipe_bad, &k, (void *)(uintptr_t)(c + 1)); return; }   // 连续 PSO_RETRY_MAX 次：按确定性处理
    nvn_hm_put(&g_pipe_retry, &k, (void *)(uintptr_t)pso_retry_pack(at, fails));
}
static void rp_pfail_ok(const rp_pfail_k *k) { if (g_pipe_retry.n) nvn_hm_del(&g_pipe_retry, k); }   // 暂时性失败后建成了：连续次数清零
// present 汇总：有新失败 / 新挡下时写一行（按「后端失败 + 挡下」排前 5 类），返回 1；没有返回 0
int rp_pipe_fail_summary(char *b, size_t n) {
    if (!g_pfail_win || !n) return 0;
    g_pfail_win = 0;
    unsigned idx[RP_PFAIL_CLS_MAX + 1], m = 0; uint64_t tf = 0, tc = 0;
    for (unsigned c = 0; c <= RP_PFAIL_CLS_MAX; c++) if (g_pfail_cls[c].fails || g_pfail_cls[c].cached) { idx[m++] = c; tf += g_pfail_cls[c].fails; tc += g_pfail_cls[c].cached; }
    for (unsigned i = 1; i < m; i++) for (unsigned j = i; j > 0; j--) {
        const struct rp_pfail_cls *x = &g_pfail_cls[idx[j]], *y = &g_pfail_cls[idx[j - 1]];
        if (x->fails + x->cached <= y->fails + y->cached) break;
        unsigned t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t;
    }
    size_t o = (size_t)snprintf(b, n, "图形管线失败（累计）：%u 类，后端建失败 %llu 次、失败表挡下 %llu 笔；前 5 类：", m, (unsigned long long)tf, (unsigned long long)tc);
    for (unsigned i = 0; i < m && i < 5 && o < n; i++) {
        const struct rp_pfail_cls *x = &g_pfail_cls[idx[i]]; char sg[160];
        rp_pfail_sig_str(&x->sig, sg, sizeof sg);
        o += (size_t)snprintf(b + o, n - o, "%s#%u %s [%s] 失败 %llu / 挡下 %llu", i ? "；" : "", idx[i] + 1, x->reason, idx[i] < RP_PFAIL_CLS_MAX ? sg : "-",
                              (unsigned long long)x->fails, (unsigned long long)x->cached);
    }
    return 1;
}
// 单测读数：第 c 类（0 起）的原因、后端失败次数与挡下笔数；c 越界返回 0
int rp_pipe_fail_class(unsigned c, const char **reason, uint64_t *fails, uint64_t *cached) {
    if (c > RP_PFAIL_CLS_MAX || (!g_pfail_cls[c].fails && !g_pfail_cls[c].cached)) return 0;
    if (reason) *reason = g_pfail_cls[c].reason;
    if (fails) *fails = g_pfail_cls[c].fails;
    if (cached) *cached = g_pfail_cls[c].cached;
    return 1;
}

// 首用取管线：kick→PENDING 时按 iOS pso_pend_take 的判定限时等/跳；可安全丢才跳，否则按 unsafe 上限等一次后标 stuck。
// 返回 rc；*skip 非 0 = 本笔要跳过（此时 *out 为 NULL）。NONE/FAILED/ABSENT 都回退现场 SYNC 建。
static nvn_gpu_rc rp_draw_pipe_async(const nvn_gpu_pipe_desc *pd, const p_set_rt *rt, nvn_gpu_pipe *out, nvn_gpu_err *err, int *skip) {
    *skip = 0; *out = NULL;
    uint8_t ident[32];
    int have_id = 0;
    if (g_prog) for (int i = 0; i < 32; i++) if (g_prog->identity[i]) { have_id = 1; break; }
    if (!nvn_vk_pipe_demand_kick || !have_id)
        return nvn_gpu_pipe_get(pd, NVN_GPU_PIPE_SYNC, out, err);   // 无身份 / 无 demand 实现：回退 SYNC
    memcpy(ident, g_prog->identity, 32);
    nvn_gpu_pipe cached = NULL;
    int k = nvn_vk_pipe_demand_kick(pd, ident, &cached);
    if (k == RP_FU_DEMAND_READY) { *out = cached; return rp_fu_ok(); }
    if (k != RP_FU_DEMAND_PENDING)   // NONE / FAILED：回退现场建（拿真实结果/错误，与关闭开关同义）
        return nvn_gpu_pipe_get(pd, NVN_GPU_PIPE_SYNC, out, err);
    const uint64_t frame = rp_frame();
    const int may_skip = rp_fu_skip_safe(rt);
    int late = 0, stuck = 0;
    if (nvn_vk_pipe_demand_marks) nvn_vk_pipe_demand_marks(pd, ident, &late, &stuck);
    uint32_t item_ms = nvk_fu_item_ms ? nvk_fu_item_ms() : 2000;
    uint32_t frame_ms = nvk_fu_frame_ms ? nvk_fu_frame_ms() : 100;
    uint32_t unsafe_ms = nvk_fu_unsafe_ms ? nvk_fu_unsafe_ms() : 10000;
    int64_t left = s2a_fu_budget_left_ms(&g_fu_budget, frame, frame_ms);
    struct s2a_fu_verdict v = s2a_fu_wait_decide(0, late, stuck, may_skip, item_ms, frame_ms, left, unsafe_ms);
    if (v.action == S2A_FU_SKIP) { *skip = v.timeout; return rp_fu_ok(); }
    // wait_ms == 0 = 不设限（nvn_first_use.h；fu_unsafe_ms = 0）：按 250 ms 分片一直等到有结果，分片超时不当终局。
    const uint32_t slice = v.wait_ms ? v.wait_ms : 250;
    int w;
    do {
        uint64_t t0 = rp_fu_now();
        w = nvn_vk_pipe_demand_wait(pd, ident, slice, out, err ? err->why : NULL, err ? sizeof err->why : 0);
        uint64_t waited = rp_fu_now() - t0;
        if (may_skip) s2a_fu_budget_spend(&g_fu_budget, frame, waited);
    } while (w == 1 && !v.wait_ms);
    if (w == 0) return rp_fu_ok();
    if (w == 2 || w == 3)   // 后台失败 / 待定项消失：回退现场建（拿真实结果/错误，不丢笔，也不按未实现报红）
        return nvn_gpu_pipe_get(pd, NVN_GPU_PIPE_SYNC, out, err);
    // w == 1：本片超时。只有 late/stuck 打项标（iOS：BUDGET 不打，预算裁短的超时下一帧还能再等）。
    if (v.timeout == S2A_FU_T_LATE || v.timeout == S2A_FU_T_STUCK) {
        if (nvn_vk_pipe_demand_mark) nvn_vk_pipe_demand_mark(pd, ident, v.timeout == S2A_FU_T_STUCK);
    }
    *skip = v.timeout ? v.timeout : S2A_FU_T_LATE;
    return rp_fu_ok();
}


// 四边形 → 三角形索引。src NULL = 不带索引（顶点自己，0 起；first 由调用方放进 base vertex）；否则是 isz 字节一个的索引。
// rv >= 0：图元重启值，每遇到一个就另起一串四边形，前一串不足一个四边形的零头丢掉。返回写出的索引个数（d 至少要 count / 4 * 6 个）
static uint32_t quad_expand(const uint8_t *src, uint32_t isz, uint32_t count, int64_t rv, uint32_t *d) {
#define IX(i) (!src ? (uint32_t)(i) : isz == 1 ? src[i] : isz == 2 ? (uint32_t)(src[(size_t)(i) * 2] | src[(size_t)(i) * 2 + 1] << 8) : \
               ((uint32_t)src[(size_t)(i) * 4] | (uint32_t)src[(size_t)(i) * 4 + 1] << 8 | (uint32_t)src[(size_t)(i) * 4 + 2] << 16 | (uint32_t)src[(size_t)(i) * 4 + 3] << 24))
    uint32_t o = 0;
    for (uint32_t s = 0; s < count;) {
        uint32_t e = s; if (src && rv >= 0) { while (e < count && IX(e) != (uint32_t)rv) e++; } else e = count;
        for (uint32_t q = s; q + 4 <= e; q += 4) {
            d[o++] = IX(q); d[o++] = IX(q + 1); d[o++] = IX(q + 2);
            d[o++] = IX(q); d[o++] = IX(q + 2); d[o++] = IX(q + 3);
        }
        s = e + 1;
    }
#undef IX
    return o;
}

// 索引展开方式：0 = 不展开；其余见下面三个展开函数
enum { EXP_NONE = 0, EXP_QUADS = 7, EXP_LOOP = 2, EXP_QSTRIP = 8, EXP_GS_TSTRIP = 5 };
#define IXR(i) (!src ? (uint32_t)(i) : isz == 1 ? src[i] : isz == 2 ? (uint32_t)(src[(size_t)(i) * 2] | src[(size_t)(i) * 2 + 1] << 8) : \
                ((uint32_t)src[(size_t)(i) * 4] | (uint32_t)src[(size_t)(i) * 4 + 1] << 8 | (uint32_t)src[(size_t)(i) * 4 + 2] << 16 | (uint32_t)src[(size_t)(i) * 4 + 3] << 24))
// 线环 → 线段列表。每段（重启值之间）n 个点：n ≥ 2 时出 n 条线（i,i+1）与收口的（n-1,0）；不足 2 个点的段什么都不画。
// 用线段列表而不是「线带 + 补首点」，是为了不依赖后端的线带重启：各段之间不用插重启值。每条线的首点就是线带里那条线的首点，激发顶点不变。
// d 至少要 count * 2 个
static uint32_t loop_expand(const uint8_t *src, uint32_t isz, uint32_t count, int64_t rv, uint32_t *d) {
    uint32_t o = 0;
    for (uint32_t s = 0; s < count;) {
        uint32_t e = s; if (src && rv >= 0) { while (e < count && IXR(e) != (uint32_t)rv) e++; } else e = count;
        if (e - s >= 2) {
            for (uint32_t i = s; i + 1 < e; i++) { d[o++] = IXR(i); d[o++] = IXR(i + 1); }
            d[o++] = IXR(e - 1); d[o++] = IXR(s);
        }
        s = e + 1;
    }
    return o;
}
// 四边形带 → 三角形列表，只在带索引又开着重启时用（其余情况直接当三角带画）。每段按三角带的绕序：
// 第 k 个四边形（段内第 2k..2k+3 个点）出 (2k,2k+1,2k+2) 与 (2k+1,2k+3,2k+2)：就是 Vulkan 三角带第 2k、2k+1 个三角形的顶点序
// （规范：第 i 个三角形是 {i, i+(1+i%2), i+(2-i%2)}），绕序与激发顶点都和直接当三角带画一致；段内零头丢掉。
// d 至少要 count * 3 个
static uint32_t qstrip_expand(const uint8_t *src, uint32_t isz, uint32_t count, int64_t rv, uint32_t *d) {
    uint32_t o = 0;
    for (uint32_t s = 0; s < count;) {
        uint32_t e = s; while (e < count && IXR(e) != (uint32_t)rv) e++;
        for (uint32_t q = s; q + 4 <= e; q += 2) {
            d[o++] = IXR(q); d[o++] = IXR(q + 1); d[o++] = IXR(q + 2);
            d[o++] = IXR(q + 1); d[o++] = IXR(q + 3); d[o++] = IXR(q + 2);
        }
        s = e + 1;
    }
    return o;
}
// #977：重启值按索引宽度截断后不是全 1 时，Vulkan 的重启值固定是全 1、对不上。改写成 32 位索引：等于重启值的 → 0xFFFFFFFF（照样重启），
// 其余原样（原来的全 1 在 32 位里不再是重启值，当普通顶点画）。返回写出的个数（= count）。
static uint32_t restart_rewrite(const uint8_t *src, uint32_t isz, uint32_t count, uint32_t rv, uint32_t *d) {
    for (uint32_t i = 0; i < count; i++) { uint32_t v = IXR(i); d[i] = v == rv ? 0xFFFFFFFFu : v; }
    return count;
}
#undef IXR
// 几何着色器输入的三角带（#696，只接不带索引、非间接的）→ 三角形列表，按 GL 的顶点序（GL 4.6 §10.1.6：A、B 两个存储顶点轮换，
// 之后每个顶点与 A、B 组成 (A, B, 当前)）：第 i 个三角形偶 i 为 (i, i+1, i+2)、奇 i 为 (i+1, i, i+2)。GS 的 gl_in[0..2] 就是这三个
// （§11.3.1）。Vulkan 原生三角带的奇三角形是 (i, i+2, i+1)：绕序相同、gl_in[0] 不同，所以 GS 不能直接吃原生三角带。
// 展开后第 i 个列表三角形就是 GL 的第 i 个三角形，gl_PrimitiveIDIn 一致。顶点号从 0 起，首顶点由调用方放进 base vertex。
// 与 iOS 共用头 nvn_geometry.h 的 nvn_geo_assemble 同一条公式（k = i+(i&1), i+1-(i&1), i+2）。d 至少要 (count - 2) * 3 个；count < 3 返回 0
static uint32_t gs_tstrip_expand(uint32_t count, uint32_t *d) {
    uint32_t o = 0;
    for (uint32_t i = 0; i + 2 < count; i++) { d[o++] = i + (i & 1); d[o++] = i + 1 - (i & 1); d[o++] = i + 2; }
    return o;
}
// 带索引的同一展开：顶点号取游戏的索引（编码这一刻读出），开着重启时每段各自从「第 0 个三角形」起算（GL：重启后像新的一次绘制）。
//   d 至少要 (count - 2) * 3 个（重启只会更少）；首顶点由调用方放进 base vertex（索引原值）。
static uint32_t gs_tstrip_expand_ix(const uint8_t *src, uint32_t isz, uint32_t count, int64_t rv, uint32_t *d) {
#define IXG(i) (isz == 1 ? src[i] : isz == 2 ? (uint32_t)(src[(size_t)(i) * 2] | src[(size_t)(i) * 2 + 1] << 8) : \
                ((uint32_t)src[(size_t)(i) * 4] | (uint32_t)src[(size_t)(i) * 4 + 1] << 8 | (uint32_t)src[(size_t)(i) * 4 + 2] << 16 | (uint32_t)src[(size_t)(i) * 4 + 3] << 24))
    uint32_t o = 0;
    for (uint32_t s = 0; s < count;) {
        uint32_t e = s; if (rv >= 0) { while (e < count && IXG(e) != (uint32_t)rv) e++; } else e = count;
        for (uint32_t i = 0; s + i + 2 < e; i++) { const uint32_t k = s + i; d[o++] = IXG(k + (i & 1)); d[o++] = IXG(k + 1 - (i & 1)); d[o++] = IXG(k + 2); }
        s = e + 1;
    }
#undef IXG
    return o;
}

// 贴图数组（着色器按运行时下标取句柄）的各项：每个阶段至多一个数组，至多 RP_TEXARR_MAX 项。
// 数组第 k 项 = 驱动句柄表的第 k 个 8 字节项；贴图槽 s 的句柄在第 4 + s 项，前 4 项与没绑的槽读作池里的第 0 项（同单个贴图槽没绑时的口径）
enum { RP_TEXARR_MAX = 64, RP_HANDLE_TABLE_FIRST_SLOT = 4 };
static nvn_gpu_view g_av[RP_PROG_STAGES][RP_TEXARR_MAX]; static nvn_gpu_samp g_as[RP_PROG_STAGES][RP_TEXARR_MAX];

// 后端能力表在设备生命周期里是不变的，但取一次要拿后端全局锁并整份拷贝：真机上常量缓冲暂存一帧约 3700 次，
// 每次取一遍能力表就是 3700 次加锁（和提交完成线程抢同一把锁）。这里取一次就记住，会话重开（rp_draw_reset）再取。
static nvn_gpu_caps g_caps_c; static int g_caps_ok;
static const nvn_gpu_caps *caps_cached(void) { if (!g_caps_ok) { nvn_gpu_caps_get(&g_caps_c); g_caps_ok = 1; } return &g_caps_c; }
// 存储缓冲的宿主绑定窗口，同 Ryujinx BufferManager.Set*StorageBuffer 与 GlobalToStorage 的 AlignDown(基址, 宿主对齐)：
// 基址不对齐时编译请求带 hasUnalignedStorageBuffer，着色器按「地址 − (基址 & −对齐)」取数，所以窗口从 基址 & −对齐 起、长度补上余数，
// 描述符偏移也就满足 minStorageBufferOffsetAlignment。只对 NVN 存储缓冲槽（SBUFP）这么做：编译事实只看这张表；
// UBOP（计算阶段用户 UBO 槽当存储缓冲）的着色器按基址本身取数，起点不动。
// 长度再夹到 maxStorageBufferRange（着色器用 OpArrayLength 取实际绑定长度判越界，窗口外按越界处理）。返回 1 = 被夹短。
// 存储缓冲槽的有效长度。硬件语义：着色器访问存储缓冲编成 LDG / STG，按 64 位客体地址裸访存，不拿槽里的长度做越界检查；
//   游戏自己判界时读的是常量缓冲里的长度（原样翻译照样生效）。所以「地址有效、长度 0」的槽访问的是真实内存：
//   长度取这段地址所在池剩下的部分（之后按存储缓冲范围上限夹短）。Ryujinx 把它判成没映射，是建宿主缓冲要一个范围的实现限制，不是硬件行为。
//   地址 0、或地址不在任何池里：没有可绑的真实内存，返回 0（调用方绑空缓冲照画，见 sb_unmapped）。只对 NVN 存储缓冲槽（SBUFP）这样补；统一缓冲来源（UBOP）长度 0 照旧算没绑。
static uint64_t sb_len(const p_bind_buf *sb, int sbufp) {
    if (!sb->addr) return 0;
    if (sb->size || !sbufp) return sb->size;
    const uint8_t *pmem = NULL; uint64_t psz = 0;
    if (!rp_pool_find(sb->addr, 1, &pmem, &psz)) return 0;
    return psz - (sb->addr - (uint64_t)(uintptr_t)pmem);
}
static int sb_window(const p_bind_buf *sb, int align_down, uint64_t *wa, uint64_t *wl) {
    const nvn_gpu_caps *cp = caps_cached();
    uint64_t a = sb->addr, l = sb_len(sb, align_down);   // align_down 各调用处都等于「是 SBUFP」
    if (align_down && cp->min_ssbo_align > 1) { const uint64_t r = a & (uint64_t)(cp->min_ssbo_align - 1); a -= r; l += r; }
    int clamped = cp->max_ssbo_range && l > cp->max_ssbo_range;
    if (clamped) l = cp->max_ssbo_range;
    // 游戏绑的长度伸出所在池的池尾（实测：4096 字节的池里偏移 2560 处绑 1872 字节）：真机上着色器只读得到池里有的那一段，
    // 这里把窗口夹到池尾照画（返回值第 2 位），不整笔丢。起点不在任何池里的仍然由解析那一步拒绝。
    const uint8_t *pm = NULL; uint64_t ps = 0;
    //   只对 NVN 存储缓冲槽（SBUFP，align_down 各调用处都等于它）夹：计算阶段把统一缓冲当存储读（UBOP）伸出池尾的照旧拒（storage_buffer_unresolved）。
    if (align_down && l && !rp_pool_find(a, l, NULL, NULL) && rp_pool_find(a, 1, &pm, &ps)) {
        const uint64_t rem = ps - (a - (uint64_t)(uintptr_t)pm);
        if (rem && rem < l) {
            static unsigned said;
            if (said < 4) { said++; char d[160]; snprintf(d, sizeof d, "存储缓冲伸出池尾，夹到池尾：addr=%#llx 绑 %llu，池里只有 %llu；池尾之后%s别的池", (unsigned long long)a, (unsigned long long)l, (unsigned long long)rem, rp_pool_find(a + rem, 1, NULL, NULL) ? "紧挨着" : "没有");
                nvn_plat_log(NVN_PLAT_LOG_INFO, "draw", d); }
            l = rem; clamped |= 2;
        }
    }
    *wa = a; *wl = l; return clamped;
}
// Bounded per-session first-cause detail; bytes do not split one cause into repeated messages.
enum { ARENA_REASON_MAX = 8 };
static struct { int rc; char why[sizeof ((nvn_gpu_err *)0)->why]; } g_arena_reasons[ARENA_REASON_MAX];
static unsigned g_arena_reason_n;
static int g_arena_reason_full;
static void arena_fail_detail(nvn_gpu_rc rc, const nvn_gpu_err *err, uint64_t bytes) {
    const char *why = err->why[0] ? err->why : nvn_gpu_ok(rc) ? "CPU mapping unavailable" : "missing backend detail";
    for (unsigned i = 0; i < g_arena_reason_n; i++)
        if (g_arena_reasons[i].rc == rc.code && !strcmp(g_arena_reasons[i].why, why)) return;
    if (g_arena_reason_n == ARENA_REASON_MAX) {
        if (!g_arena_reason_full) {
            g_arena_reason_full = 1;
            nvn_plat_log(NVN_PLAT_LOG_RED, "draw", "arena_alloc_failed：后续不同原因详情省略（最多8）");
        }
        return;
    }
    g_arena_reasons[g_arena_reason_n].rc = rc.code;
    snprintf(g_arena_reasons[g_arena_reason_n++].why, sizeof g_arena_reasons[0].why, "%s", why);
    char line[256];
    snprintf(line, sizeof line, "arena_alloc_failed rc=%d bytes=%llu why=%.160s", rc.code,
             (unsigned long long)bytes, why);
    nvn_plat_log(NVN_PLAT_LOG_RED, "draw", line);
}

static uint64_t dt_now(void);   // 计时器（定义在文件后部）
static int g_btm;   // 暂存分段计时开关（跟 nvk_bind_tm 一起，见 rp_bind_tm 那边）
static uint64_t g_bt[8];   // 分段计时累加（正式定义在后面，这里先给暂存路径用）
static int alloc_copy(nvn_gpu_cmd c, uint64_t n, const void *src, nvn_gpu_slice *sl) {
    const uint32_t align = caps_cached()->min_ubo_align ? caps_cached()->min_ubo_align : 256;
    nvn_gpu_err err = {0};
    const uint64_t a0 = g_btm ? dt_now() : 0;
    nvn_gpu_rc rc = nvn_gpu_arena_alloc(c, n, align, sl, &err);
    const uint64_t a1 = g_btm ? dt_now() : 0;
    if (!nvn_gpu_ok(rc) || !sl->cpu) { arena_fail_detail(rc, &err, n); return 0; }
    if (src) memcpy(sl->cpu, src, (size_t)n); else memset(sl->cpu, 0, (size_t)n);
    if (a0) { const uint64_t a2 = dt_now(); g_bt[6] += a1 - a0; g_bt[7] += a2 - a1; }
    return 1;
}

// 常量缓冲暂存的拷贝交给后端异步填（照 iOS nvn_rec.h 的常量缓冲拷贝线程；安卓上是两级回放的录制线程按序做，见 nvn_gpu_arena_fill）。
// 一款大型开放世界游戏（01007EF00011E000）的神庙场景每帧约 1.1 万次、44 MB，原先全在回放线程上 memcpy。正确性靠三点：
//   ① GPU 只在段提交之后读上传片，后端保证提交 / 放弃之前填完；
//   ② 回放线程自己改游戏内存（OP_UPDATE_UB / OP_COPY_B2B / OP_CLEAR_BUF）之前调 rp_ub_fill_fence：来源与它相交、还没填完的先让它填完；
//   ③ 一次提交里游戏不改它的缓冲（GPU 在提交之后才读，见段内切片表那段说明）；提交返回之前段已提交，①已经等完。
// 完成回调里的「落地」（GPU 写回游戏内存）和原先编码时就拷一样是另一条线程上的并发写，挪后不改变这一点。
// 只挪大块：小块走队列一条记录的开销比拷贝本身还大。开关 nvk_ub_fill（缺省开；0 = 照旧在回放线程上拷）。
// ② 怎么「让它填完」（开关 nvk_ub_fill_steal，缺省开）：
//   开：相交的每一笔都调 nvn_gpu_arena_fill_take——还没开始就由回放线程自己拷（代拷），录制线程正拷就只等这一笔。不相交的不管。
//       一台骁龙 8 至尊版手机上（洞内走动，每帧）改内存前要等约 260 次，原先每次都等录制线程把这一笔前面排着的绘制记录全录完，
//       游戏线程 15.1% 耗在等待；代拷只付这一笔自己的 memcpy。代拷只保证这一笔，所以要对每一笔相交的都做，不能只管最新那笔。
//   关（0）：原来的做法，等最新一条相交的票号（nvn_gpu_arena_fill_wait 保证更旧的随之做完）。
enum { UBF_MIN = 256, UBF_N = 256 };
static struct { uint64_t lo, hi, t; } g_ubf[UBF_N];   // 交出去的填充：来源区间与票号，按交出顺序环形存放（票号递增）；代拷 / 等过的那条把区间清空（hi = lo）
static uint32_t g_ubf_w; static uint64_t g_ubf_done;   // 交出去的条数；已知填完的最大票号（水位：≤ 它的都填完了）
static int ubf_on(void) { static int v = -1; static unsigned k; if (v < 0 || !(++k & 255)) v = nvn_plat_flag("nvk_ub_fill", 1) != 0; return v; }
static int g_ubf_steal = -1; static unsigned g_ubf_steal_k;   // 开关 nvk_ub_fill_steal 的缓存（每 256 次重读；会话重开清掉）
static int ubf_steal_on(void) { if (g_ubf_steal < 0 || !(++g_ubf_steal_k & 255)) g_ubf_steal = nvn_plat_flag("nvk_ub_fill_steal", 1) != 0; return g_ubf_steal; }
static void ubf_take(uint32_t i) {   // 代拷第 i 条（只管它自己），记数
    const int r = nvn_gpu_arena_fill_take(g_ubf[i].t);
    if (r == 1) { rp_perf_add(RP_PERF_UBO_FILL_STEAL_N, 1); rp_perf_add(RP_PERF_UBO_FILL_STEAL_B, g_ubf[i].hi - g_ubf[i].lo); }
    else if (r == 2) rp_perf_add(RP_PERF_UBO_FILL_BUSY_N, 1);
    g_ubf[i].hi = g_ubf[i].lo;   // 已拷完：之后的写不再与它相交
}
void rp_ub_fill_fence(uint64_t addr, uint64_t len) {
    if (!g_ubf_w || !len) return;
    g_ubf_done = nvn_gpu_arena_fill_wait(0);
    const uint64_t hi = addr + len;
    const int steal = ubf_steal_on();
    int hit = 0;
    for (uint32_t k = 0; k < UBF_N && k < g_ubf_w; k++) {   // 从新往旧：票号递增，遇到水位以下的，更旧的也都填完了
        const uint32_t i = (g_ubf_w - 1 - k) % UBF_N;
        if (g_ubf[i].t <= g_ubf_done) break;
        if (!(g_ubf[i].lo < hi && addr < g_ubf[i].hi)) continue;
        if (!steal) { g_ubf_done = nvn_gpu_arena_fill_wait(g_ubf[i].t); rp_perf_add(RP_PERF_UBO_FILL_WAIT_N, 1); return; }   // 等最新一条相交的：更旧的随之填完
        ubf_take(i); hit = 1;   // 代拷：每一笔相交的都要拷完（代拷不带着更旧的）
    }
    if (hit) rp_perf_add(RP_PERF_UBO_FILL_WAIT_N, 1);   // 与关着时同口径：碰上相交、没填完的改内存次数
}
static int alloc_fill(nvn_gpu_cmd c, uint64_t n, const void *src, nvn_gpu_slice *sl) {
    if (n < UBF_MIN || !ubf_on()) return alloc_copy(c, n, src, sl);
    const uint32_t align = caps_cached()->min_ubo_align ? caps_cached()->min_ubo_align : 256;
    nvn_gpu_err err = {0};
    const uint64_t a0 = g_btm ? dt_now() : 0;
    nvn_gpu_rc rc = nvn_gpu_arena_alloc(c, n, align, sl, &err);
    const uint64_t a1 = g_btm ? dt_now() : 0;
    if (!nvn_gpu_ok(rc) || !sl->cpu) { arena_fail_detail(rc, &err, n); return 0; }
    const uint64_t t = nvn_gpu_arena_fill(c, sl, src, n);
    if (t) {
        const uint32_t i = g_ubf_w++ % UBF_N;
        if (g_ubf[i].t > g_ubf_done) {   // 表满：挤掉的那条必须已经填完，否则相交的写就漏等了
            if (ubf_steal_on()) ubf_take(i); else g_ubf_done = nvn_gpu_arena_fill_wait(g_ubf[i].t);
        }
        g_ubf[i].lo = (uint64_t)(uintptr_t)src; g_ubf[i].hi = g_ubf[i].lo + n; g_ubf[i].t = t;
        rp_perf_add(RP_PERF_UBO_FILL_N, 1);
    }
    if (a0) { const uint64_t a2 = dt_now(); g_bt[6] += a1 - a0; g_bt[7] += a2 - a1; }
    return 1;
}

// 按脏位少发绑定（对照 compat/tree/src/Ryujinx.Graphics.Vulkan/DescriptorSetUpdater.cs：只有变过的那一组才重建）。
// 后端按「缓冲 + 偏移 + 长度」判一组描述符变没变，所以核心要做到：内容没变的块，给后端的就是同一片暂存。两层：
//   ① 段内切片表：同一段里同一个游戏地址区间的常量缓冲、同一个程序的自带常量段，只在暂存区落一份。
//      一次提交里游戏缓冲的内容是定的（真机上 GPU 在提交之后才读，游戏不能在这期间改），例外只有回放时在 CPU 上
//      改缓冲的三个命令，它们改 rp_cpu_write_gen，表按它失效。
//   ② 上一笔的绑定表：这个阶段的绑定状态没变（rp_args_dirty 没立）、程序和 base instance 也没变，非贴图项直接沿用上一笔的。
extern uint64_t rp_cpu_write_gen;   // rp_buf.c
enum { SLC_N = 64 };
static struct {
    nvn_gpu_cmd c; uint64_t seg, gen, k0, k1; nvn_gpu_slice sl;
    // 写入记号（开关 nvk_slc_gen，缺省开）：这份片拷好之后，来源那一段再没被这三个改缓冲的命令写过 → 内容没变，可以直接用回这一片。
    // 全局写代号一被任何一处写入推进，原先这张表就整体作废；真机上常量缓冲一帧暂存 3700 次、共 9.2 MB，
    // 其中大部分（每帧固定的那几块）内容其实没变，靠逐区间判定把它们认回来。
    uint64_t wmark;
} g_slc[SLC_N];
// stable=1：内容与游戏内存无关的片（程序自带的常量段 C1）：同一段内一直有效，不看全局写代号。
// 不看写代号是关键：回放里每改一次游戏缓冲内容全局写代号就加一，原先 C1 片也跟着作废（一帧约 2200 次、
// 每笔每阶段各一份 256 字节）——内容根本不会变，白分配白拷贝。
static int slc_get_x(nvn_gpu_cmd c, uint64_t k0, uint64_t k1, const void *src, nvn_gpu_slice *sl, int stable) {
    uint64_t h = k0 * 0x9E3779B97F4A7C15ull ^ k1; uint32_t i = (uint32_t)(h >> 40) & (SLC_N - 1);
    uint64_t sg = rp_seg_serial();
    static int genv = -1; static unsigned gk;
    if (genv < 0 || !(++gk & 255)) genv = nvn_plat_flag("nvk_slc_gen", 1) != 0;
    const int same = g_slc[i].c == c && g_slc[i].seg == sg && g_slc[i].k0 == k0 && g_slc[i].k1 == k1;
    if (same && (stable ? g_slc[i].gen == UINT64_MAX : g_slc[i].gen == rp_cpu_write_gen)) { *sl = g_slc[i].sl; rp_perf_add(RP_PERF_BIND_SLICE_HIT, 1); return 1; }
    // 写代号被别处推进了不一定真变：按「这一段在拷好之后有没有被改缓冲的命令写过」再判一次
    if (same && !stable && genv && g_slc[i].wmark && src && !rp_write_touched(g_slc[i].wmark, (uint64_t)(uintptr_t)src, (uint32_t)k1)) {
        g_slc[i].gen = rp_cpu_write_gen;   // 内容确认没变：把这份片重新挂到当前代号上
        *sl = g_slc[i].sl; rp_perf_add(RP_PERF_BIND_SLICE_HIT, 1); return 1;
    }
    if (!(stable ? alloc_copy(c, k1 & 0xffffffffu, src, sl) : alloc_fill(c, k1 & 0xffffffffu, src, sl))) return 0;   // 游戏内存里的常量缓冲交后端异步填；程序自带常量段照旧就地拷
    rp_perf_add(RP_PERF_UBO_STAGED_B, k1 & 0xffffffffu);   // 读数：去重之后真落进暂存区的字节（「常量缓冲暂存 … KB」是去重前的上限）
    g_slc[i].c = c; g_slc[i].seg = sg; g_slc[i].gen = stable ? UINT64_MAX : rp_cpu_write_gen; g_slc[i].k0 = k0; g_slc[i].k1 = k1; g_slc[i].sl = *sl;
    g_slc[i].wmark = (!stable && src) ? rp_write_mark() : 0;
    return 1;
}
static int slc_get(nvn_gpu_cmd c, uint64_t k0, uint64_t k1, const void *src, nvn_gpu_slice *sl) { return slc_get_x(c, k0, k1, src, sl, 0); }
static struct { nvn_gpu_cmd c; uint64_t seg, gen, reg_gen; const struct rp_prog *pg; uint32_t base_instance, n; int ok; uint16_t bof[RP_PROG_ENT_MAX]; nvn_gpu_binding b[RP_PROG_ENT_MAX]; } g_lb[RP_PROG_STAGES];

// B3：驱动段暂存的按内容缓存。DRV0（2.5 KB）/ DRV2（0.29 KB）的整段内容只由下面这些输入决定，
// 同一段里内容相同就复用同一片 slice，省掉每笔每阶段的 memset + 拷贝。段号（rp_seg_serial）进键：段内暂存随段回收，跨段不能复用。
// 键仍按内容整份存下来逐字节比（碰撞不会错内容）；上面那层 64 位摘要只是先筛一遍，省掉线性扫多格时的大块比较。
typedef struct { nvn_gpu_cmd c; uint64_t seg; uint32_t st, base_instance; uint64_t sb[16][2], ub[16][2]; uint32_t dim[3]; uint8_t has_ub; } drv0_key;
// 驱动句柄表 c2 的全部来源（同 iOS nvn_mtl.m DRV2 的四张表）：组合槽 0..40、图像槽 0..7、分离贴图槽 0..127、分离采样器槽 0..127
typedef struct { nvn_gpu_cmd c; uint64_t seg; uint32_t st; uint64_t h[41], img[8], sep[128], samp[128]; } drv2_key;
// 表按 64 格两路探测；键里再放一个按字段算的 64 位摘要，先比摘要再比整键。
// 真机上每帧 2292 次 DRV0 查询，原先线性扫 8 格 × 552 字节整键比较 ≈ 4.4 KB/次（一帧 10 MB 比较）。
enum { DRV_CACHE_N = 64, DRV_CACHE_WAYS = 2 };
static uint64_t drv_mix(uint64_t h, uint64_t v) { h ^= v; h *= 0x100000001B3ull; return h ^ (h >> 29); }
static uint64_t drv0_hash(const drv0_key *k) {   // 按字段算（不整块读键）：~80 次乘加
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)k->c;
    h = drv_mix(h, k->seg); h = drv_mix(h, (uint64_t)k->st << 32 | k->base_instance); h = drv_mix(h, k->has_ub);
    for (int r = 0; r < 16; r++) { h = drv_mix(h, k->sb[r][0]); h = drv_mix(h, k->sb[r][1]); }
    for (int r = 0; r < 16; r++) { h = drv_mix(h, k->ub[r][0]); h = drv_mix(h, k->ub[r][1]); }
    for (int r = 0; r < 3; r++) h = drv_mix(h, k->dim[r]);
    return h;
}
static uint64_t drv2_hash(const drv2_key *k) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)k->c;
    h = drv_mix(h, k->seg); h = drv_mix(h, k->st);
    for (int r = 0; r < 41; r++) h = drv_mix(h, k->h[r]);
    for (int r = 0; r < 8; r++) h = drv_mix(h, k->img[r]);
    for (int r = 0; r < 128; r++) { h = drv_mix(h, k->sep[r]); h = drv_mix(h, k->samp[r]); }
    return h;
}
static struct { drv0_key k; uint64_t h; uint8_t ok; nvn_gpu_slice sl; } g_drv0c[DRV_CACHE_N]; static int g_drv0c_next;
static struct { drv2_key k; uint64_t h; uint8_t ok; nvn_gpu_slice sl; } g_drv2c[DRV_CACHE_N]; static int g_drv2c_next;
static int drv0_lookup(const drv0_key *k, nvn_gpu_slice *sl) {
    uint64_t h = drv0_hash(k); uint32_t base = (uint32_t)(h % (DRV_CACHE_N / DRV_CACHE_WAYS)) * DRV_CACHE_WAYS;
    for (int w = 0; w < DRV_CACHE_WAYS; w++) {
        const uint32_t i = base + (uint32_t)w;
        if (g_drv0c[i].ok && g_drv0c[i].h == h && !memcmp(&g_drv0c[i].k, k, sizeof *k)) { *sl = g_drv0c[i].sl; return 1; }
    }
    return 0;
}
static void drv0_store(const drv0_key *k, const nvn_gpu_slice *sl) {
    uint64_t h = drv0_hash(k); uint32_t base = (uint32_t)(h % (DRV_CACHE_N / DRV_CACHE_WAYS)) * DRV_CACHE_WAYS;
    for (int w = 0; w < DRV_CACHE_WAYS; w++) if (!g_drv0c[base + w].ok || !memcmp(&g_drv0c[base + w].k, k, sizeof *k)) {
        g_drv0c[base + w].k = *k; g_drv0c[base + w].h = h; g_drv0c[base + w].ok = 1; g_drv0c[base + w].sl = *sl; return; }
    uint32_t i = (uint32_t)(g_drv0c_next++ % DRV_CACHE_N);   // 这一组都占了：轮转替换
    g_drv0c[i].k = *k; g_drv0c[i].h = h; g_drv0c[i].ok = 1; g_drv0c[i].sl = *sl;
}
static int drv2_lookup(const drv2_key *k, nvn_gpu_slice *sl) {
    uint64_t h = drv2_hash(k); uint32_t base = (uint32_t)(h % (DRV_CACHE_N / DRV_CACHE_WAYS)) * DRV_CACHE_WAYS;
    for (int w = 0; w < DRV_CACHE_WAYS; w++) {
        const uint32_t i = base + (uint32_t)w;
        if (g_drv2c[i].ok && g_drv2c[i].h == h && !memcmp(&g_drv2c[i].k, k, sizeof *k)) { *sl = g_drv2c[i].sl; return 1; }
    }
    return 0;
}
static void drv2_store(const drv2_key *k, const nvn_gpu_slice *sl) {
    uint64_t h = drv2_hash(k); uint32_t base = (uint32_t)(h % (DRV_CACHE_N / DRV_CACHE_WAYS)) * DRV_CACHE_WAYS;
    for (int w = 0; w < DRV_CACHE_WAYS; w++) if (!g_drv2c[base + w].ok || !memcmp(&g_drv2c[base + w].k, k, sizeof *k)) {
        g_drv2c[base + w].k = *k; g_drv2c[base + w].h = h; g_drv2c[base + w].ok = 1; g_drv2c[base + w].sl = *sl; return; }
    uint32_t i = (uint32_t)(g_drv2c_next++ % DRV_CACHE_N);
    g_drv2c[i].k = *k; g_drv2c[i].h = h; g_drv2c[i].ok = 1; g_drv2c[i].sl = *sl;
}
static int g_bind_keep[RP_PROG_STAGES], g_pipe_set;   // 这一笔：阶段的绑定状态没变（rp_draw 在换管线之前取）；这一笔重设过管线
// 读数（随 nvk_gputime）：整组重建分两块——[0] 含暂存与构造的总时间、[1] 其中交给后端 nvn_gpu_bind 的时间、[2] 重建次数
static uint64_t dt_now(void);   // 计时器（定义在文件后部）
static uint64_t g_bt[8];   // [0] 总 [1] 交后端 [2] 次数 [3] 驱动段 DRV0/DRV2 [4] 常量缓冲 [5] 其它 [6] 暂存申请 [7] 内容拷贝
void rp_bind_tm_take(uint64_t *tot, uint64_t *backend, uint64_t *n) { *tot = g_bt[0]; *backend = g_bt[1]; *n = g_bt[2]; g_bt[0] = g_bt[1] = g_bt[2] = 0; }
void rp_bind_tm_take2(uint64_t *drv, uint64_t *buf, uint64_t *rest) { *drv = g_bt[3]; *buf = g_bt[4]; *rest = g_bt[5]; g_bt[3] = g_bt[4] = g_bt[5] = 0; }
void rp_bind_tm_take3(uint64_t *alloc, uint64_t *copy) { *alloc = g_bt[6]; *copy = g_bt[7]; g_bt[6] = g_bt[7] = 0; }   // 暂存申请 / 内容拷贝（见 alloc_copy）


// 调试读回用的绘制历史（见文件后部 dump_chain）
enum { HIST_N = 4096, HIST_TEX = 12, DUMP_MAX = 48 };
static struct { uint64_t target, tex[HIST_TEX]; } g_hist[HIST_N]; static uint32_t g_hist_n;
static void hist_note(uint64_t target, const uint64_t *tex, int ntex) {
    uint32_t i = g_hist_n++ % HIST_N; g_hist[i].target = target; memset(g_hist[i].tex, 0, sizeof g_hist[i].tex);
    for (int k = 0; k < ntex && k < HIST_TEX; k++) g_hist[i].tex[k] = tex[k];
}
static int g_log_left;   // 逐笔日志还剩几行（调试开关 nvk_drawlog：上屏每 600 帧给下一帧开一次，见 rp_draw_log_arm）
static int skip(const char *why, const void *rec) {
    nvn_unimpl("draw", why, rec); g_skips++;
    if (g_log_left) { g_log_left--; char b[96]; snprintf(b, sizeof b, "跳过一笔：%s", why); nvn_plat_log(NVN_PLAT_LOG_INFO, "draw", b); }
    return 0;
}
void rp_draw_log_arm(int lines) { g_log_left = lines; }
static const char *image_resolve(nvn_gpu_cmd c, const struct rp_prog *pg, int pi, uint32_t i, int stage, nvn_gpu_view *out, tex_t *snapout);
static const char *image_tbuf_resolve(nvn_gpu_cmd c, const struct rp_prog *pg, int pi, uint32_t i, int stage, nvn_gpu_view *out);
// 图形阶段存储图像：这一笔解析出的登记快照（调用方持有；登记表会扩容换地址），画成后按写访问登记 GPU 写
static tex_t g_gimg_snap[RP_PROG_STAGES][RP_PROG_ENT_MAX]; static const tex_t *g_gimg[RP_PROG_STAGES][RP_PROG_ENT_MAX];

// 图形高槽与驱动图像表重叠：与 DRV2 的写入顺序、iOS tex_slot_h 同一来源。
static uint64_t graphics_tex_handle(int st, unsigned reg) {
    if (st < RP_PROG_STAGES && reg >= 32 && reg < 40 && rp_S.img[st][reg - 32])
        return rp_S.img[st][reg - 32];
    return rp_S.tex[st][reg];
}

// ---------- 缓冲贴图（NVN target BUFFER）→ 后端缓冲视图 ----------
// 两种用法同一套：采样纹素缓冲（TBUF，NVN_RB_TBUF，着色器 texelFetch）与存储纹素缓冲（IMAGE_TBUF，图像槽里的 imageBuffer，计算读写）。
//   对照：compat Ryujinx TextureBindingsManager 遇 Target.TextureBuffer 不论采样还是图像都走 SetBufferTextureStorage（isImage 只决定描述符种类）；
//   Eden vk_compute_pipeline.cpp 把 texture_buffer / image_buffer 两组都交给 BindComputeTextureBuffer，图像那组的格式取着色器显式声明的格式；
//   iOS nvn_mtl.m 建贴图时 target 10 直接在池缓冲上建 texture buffer 视图（零拷贝），图像槽照普通图像绑那张视图。
// 偏移不满足 minTexelBufferOffsetAlignment：Vulkan 不许原地建视图（VUID-VkBufferViewCreateInfo-offset-02749，Metal 那边会静默读错位）。
//   选对齐副本（compat Ryujinx BufferHolder.CreateView / ArmAlignedTexelWriteback 同一做法，也是 iOS tba_copy 的思路）：开遍前把这段字节在 GPU 上
//   拷进本段暂存区里按对齐起点分的一块，视图建在副本上；存储纹素缓冲写过的副本在派发之后拷回原处（texel_wb_flush）。
//   不选 iOS 的前导元素（tbl_view：视图从向下取整的边界开、着色器取数下标加 lead）：那要改写着色器的取数与尺寸查询，安卓直连编译器没有这道改写。
//   偏移连元素字节都不对齐的不做副本（iOS 同样给空），交给后端照实拒、具名记账。
enum { TV_TOO_MANY, TV_UNALIGNED_GFX_WRITE, TV_UNALIGNED_PARTIAL, TV_SHADOW_FULL, TV_SHADOW_ALLOC, TV_VIEW_FAILED, TV_N };
static const char *const k_tvw[3][TV_N] = {
    {"tbuf_too_many_elements", "tbuf_unaligned_graphics_write", "tbuf_unaligned_partial_word", "tbuf_shadow_table_full", "tbuf_shadow_alloc_failed", "tbuf_view_create_failed"},
    {"compute_image_tbuf_too_many_elements", "compute_image_tbuf_unaligned_graphics_write", "compute_image_tbuf_unaligned_partial_word", "compute_image_tbuf_shadow_table_full", "compute_image_tbuf_shadow_alloc_failed", "compute_image_tbuf_view_create_failed"},
    {"graphics_image_tbuf_too_many_elements", "graphics_image_tbuf_unaligned_graphics_write", "graphics_image_tbuf_unaligned_partial_word", "graphics_image_tbuf_shadow_table_full", "graphics_image_tbuf_shadow_alloc_failed", "graphics_image_tbuf_view_create_failed"}};
// 本笔派发里写过的对齐副本：派发之后拷回原处（texel_wb_flush）。只在一笔派发 / 绘制之内有效
enum { TWB_MAX = 16 };
static struct { nvn_gpu_buf src, sh; uint64_t src_off, sh_off, bytes; } g_twb[TWB_MAX]; static int g_ntwb;
static uint64_t g_tv_shadows, g_tv_view_fails;
// buf / off：池镜像里这段字节；elem：元素字节数；kind：0 TBUF、1 计算 imageBuffer、2 图形 imageBuffer；write：着色器会写。成功返回 NULL、*out 为临时视图
static const char *texel_bind(nvn_gpu_cmd c, nvn_gpu_buf buf, uint64_t off, uint64_t bytes, uint32_t elem, uint32_t fmt, int kind, int write, nvn_gpu_view *out) {
    const char *const *w = k_tvw[kind];
    const nvn_gpu_caps *cp = caps_cached();
    *out = NULL;
    if (cp->max_texel_elems && bytes / elem > cp->max_texel_elems) return w[TV_TOO_MANY];
    const uint64_t al = cp->min_texel_align;
    nvn_gpu_buf vb = buf; uint64_t vo = off; int shadow = 0;
    if (al > 1 && (off % al) && !(off % elem)) {
        if (write && kind == 2) return w[TV_UNALIGNED_GFX_WRITE];   // 绘制在通道里，写完的副本拷不回去：照实记账，不原地硬建
        if (bytes & 3) return w[TV_UNALIGNED_PARTIAL];              // 缓冲拷贝按 4 字节；多拷会把相邻字节一起写回
        if (write && g_ntwb == TWB_MAX) return w[TV_SHADOW_FULL];
        nvn_gpu_slice sl; nvn_gpu_err aerr = {0};
        rp_pass_flush(c);   // 拷贝在通道外
        nvn_gpu_rc arc = nvn_gpu_arena_alloc(c, bytes, (uint32_t)al, &sl, &aerr);
        if (!nvn_gpu_ok(arc)) { arena_fail_detail(arc, &aerr, bytes); return w[TV_SHADOW_ALLOC]; }
        nvn_gpu_copy_buf(c, buf, off, sl.buf, sl.off, bytes);
        vb = sl.buf; vo = sl.off; shadow = 1;
        if (!g_tv_shadows++) { char m[320]; snprintf(m, sizeof m, "缓冲贴图在不对齐的池偏移（镜像偏移 %llu，对齐 %llu）：%s，%llu 字节拷进对齐副本再建视图（首例）",
            (unsigned long long)off, (unsigned long long)al, kind ? "存储纹素缓冲（写完拷回）" : "采样纹素缓冲", (unsigned long long)bytes); nvn_plat_log(NVN_PLAT_LOG_INFO, "draw", m); }
    }
    nvn_gpu_err err = {0}; nvn_gpu_rc rc = nvn_gpu_view_create_buf(vb, vo, bytes, fmt, out, &err);
    if (!nvn_gpu_ok(rc) || !*out) {
        *out = NULL;
        if (g_tv_view_fails++ < 4) { char m[640]; snprintf(m, sizeof m, "%s：格式 %#x 偏移 %llu 字节 %llu（纹素偏移对齐 %llu、元素上限 %u）码 %d：%s",
            w[TV_VIEW_FAILED], fmt, (unsigned long long)vo, (unsigned long long)bytes, (unsigned long long)al, cp->max_texel_elems, rc.code, err.why); nvn_plat_log(NVN_PLAT_LOG_RED, "draw", m); }
        return w[TV_VIEW_FAILED];
    }
    if (shadow && write) { g_twb[g_ntwb].src = buf; g_twb[g_ntwb].src_off = off; g_twb[g_ntwb].sh = vb; g_twb[g_ntwb].sh_off = vo; g_twb[g_ntwb].bytes = bytes; g_ntwb++; }
    return NULL;
}
// 派发之后：写过的对齐副本拷回池镜像原处，再一道屏障让拷回的字节对后面所有阶段可见（缓冲拷贝之前后端自带全屏障，派发的写对拷贝可见）
static void texel_wb_flush(nvn_gpu_cmd c) {
    for (int k = 0; k < g_ntwb; k++) nvn_gpu_copy_buf(c, g_twb[k].sh, g_twb[k].sh_off, g_twb[k].src, g_twb[k].src_off, g_twb[k].bytes);
    if (g_ntwb) nvn_gpu_barrier(c, NVN_BARRIER_INVALIDATE_SHADER_BIT);
    g_ntwb = 0;
}
// 这一项的视图是本笔临时建的缓冲视图（编码完由调用方释放）
static int texel_tmp(const nvn_gpu_layout_ent *e) { return e->src_kind == NVN_RB_TBUF || (e->src_kind == NVN_RB_IMG && e->cls == NVN_GPU_RES_IMAGE_TBUF); }

// Resolve the exact backing bytes before the pass. The view is temporary: the
// backend retains it at bind, and the caller releases it after encoding.
static int tbuf_prepare(nvn_gpu_cmd c, const nvn_gpu_layout_ent *e, int st,
                        uint8_t dtt, uint8_t dcls, nvn_gpu_view *out, const void *rec) {
    *out = NULL;
    if (e->cls != NVN_GPU_RES_TBUF || e->count != 1 || e->src_reg >= RP_NTEX ||
        e->src_reg2 != UINT16_MAX || e->access != NVN_GPU_ACC_R)
        return skip("tbuf_binding_shape", rec);
    uint64_t h = graphics_tex_handle(st, e->src_reg);
    int id = h ? (int)(h & 0xfffff) : rp_S.tex_base;
    tex_t t; view_t v; int hv = 0;
    if (!rp_tex_snap_by_id(id, &t, &v, &hv)) return skip("tbuf_texture_unresolved", rec);
    if (t.d.target != NVN_TEXTURE_TARGET_BUFFER || (hv && v.set_target && v.target != NVN_TEXTURE_TARGET_BUFFER) ||
        !t.mem || !t.pool || !t.d.width || t.d.levels > 1 || t.d.samples > 1 ||
        (hv && v.set_levels && (v.base_level || v.nlevels > 1)) ||
        (hv && v.set_layers && (v.min_layer || v.nlayers > 1))) return skip("tbuf_texture_shape", rec);
    for (int i = 0; i < 4; i++) {
        unsigned sw = hv && v.set_swizzle ? v.swz[i] : t.set_swz ? (t.swz >> (3 * i) & 7) : (unsigned)(2 + i);
        if (sw != (unsigned)(2 + i)) return skip("tbuf_swizzle_unsupported", rec);
    }
    uint32_t fmt = hv && v.set_format ? v.format : (uint32_t)t.d.format;
    nvn_fmt_info base, view;
    if (!nvn_fmt_info_of((uint32_t)t.d.format, &base) || !nvn_fmt_info_of(fmt, &view) ||
        !base.bytes || base.bw != 1 || base.bh != 1 || base.depth || base.stencil ||
        view.bw != 1 || view.bh != 1 || view.bytes != base.bytes || view.depth || view.stencil)
        return skip("tbuf_format_unsupported", rec);
    unsigned cls = view.kind == NVN_FMT_UINT ? 1u : view.kind == NVN_FMT_SINT ? 2u : 0u;
    if (dtt != NVN_TEXTURE_TARGET_BUFFER + 1 || dcls != cls) return skip("tbuf_format_mismatch", rec);
    uint64_t bytes = (uint64_t)t.d.width * base.bytes;
    nvn_gpu_buf buf = NULL; uint64_t off = 0;
    if (!rp_buf_resolve(c, (uint64_t)(uintptr_t)t.mem, bytes, &buf, &off)) return skip("tbuf_buffer_unresolved", rec);
    const char *why = texel_bind(c, buf, off, bytes, base.bytes, fmt, 0, 0, out);
    return why ? skip(why, rec) : 1;
}

// 一个阶段的全部绑定。0 = 有取不到的资源（已记账），这笔不画
// pi：程序里的阶段下标（0 顶点 / 计算，1 片元）；st：NVN 阶段号（回放状态按它取：0 顶点、1 片元、5 计算）
static nvn_gpu_buf g_disp_ind; static uint64_t g_disp_ind_off;
// 本笔绘制在开通道之前解析好的图形阶段存储缓冲（[阶段][布局项]）；只在 rp_draw 里填、在同一笔的发绑定里用
static nvn_gpu_buf g_sb_buf[RP_PROG_STAGES][RP_PROG_ENT_MAX]; static uint64_t g_sb_off[RP_PROG_STAGES][RP_PROG_ENT_MAX];
static uint8_t g_sb_ok[RP_PROG_STAGES][RP_PROG_ENT_MAX];
// 常量缓冲的 GPU 待写回镜像：同存储缓冲，开通道之前解析好，发绑定只消费结果（见 rp_draw 的准备阶段）。
static nvn_gpu_buf g_ub_buf[RP_PROG_STAGES][RP_PROG_ENT_MAX]; static uint64_t g_ub_off[RP_PROG_STAGES][RP_PROG_ENT_MAX];
static uint8_t g_ub_ok[RP_PROG_STAGES][RP_PROG_ENT_MAX];
// 存储缓冲槽「没有可绑的真实内存」：地址 0，或地址不在任何池里（sb_len 返回 0）。绑 64 KiB 空缓冲、这一笔照画（不是整笔跳过）。
//   「地址有效、长度 0」不在此列：按硬件语义绑到真实内存（见 sb_len）。只管 NVN 存储缓冲槽（SBUFP）；
//   计算阶段把统一缓冲当存储读（UBOP）的没绑仍具名跳过。
static int sb_unmapped(const p_bind_buf *sb) { return !sb_len(sb, 1); }
// Per-draw GPU-populated driver slices. Never retained by the binding cache.
static nvn_gpu_slice g_cold_drv;   // 只在本笔：归约预录和片元绑定共用的 CPU DRV0
static nvn_gpu_slice g_ind_drv[RP_PROG_STAGES];
static uint8_t g_ind_drv_ok[RP_PROG_STAGES];
static void fill_graphics_drv(nvn_gpu_slice *sl, int st, uint32_t base_instance) {
    for (int r = 0; r < 16; r++) { const p_bind_buf *sb = &rp_S.ssbo[st][r]; if (!sb->addr) continue;
        uint64_t a = sb->addr; uint32_t sz = (uint32_t)sb->size;
        memcpy(sl->cpu + k_drv0_base[st] + 16 * r, &a, 8);
        memcpy(sl->cpu + k_drv0_base[st] + 16 * r + 8, &sz, 4);
    }
    float max_point = 2047.0f;
    memcpy(sl->cpu + 0x610, &max_point, 4); memcpy(sl->cpu + 0x644, &base_instance, 4);
}

// ---------- 间接绘制 base instance 的通道级批量拷（开关 debug.s2a.nvk_binst_hoist：0 逐笔拷；1 批量、每段一次拷贝各带全屏障；2 批量、一次多区域拷贝 + 精确屏障（缺省）；会话打开时读定） ----------
// 间接绘制的 firstInstance 在 GPU 侧的间接记录里，读 c0 + 0x644 的阶段要先把它拷进这一笔自己的驱动段切片。Vulkan 渲染通道里不能拷贝，
//   逐笔拷就是每笔先收通道（8E：每帧收通道 381.7 个里 250 个是它，切出的 0–1 笔通道 190.8 个）。
// 这里改成开通道之前前瞻：从这一笔的记录往后扫命令流，到第一条可能写缓冲 / 结束通道的命令为止，把途经的单条间接绘制各拷一份进各自的切片
//   （每笔每阶段一个独立切片，不共用），之后这一遍里的这些笔直接用拷好的切片，不再为拷贝收通道。CPU 侧的驱动段内容照旧在这一笔发绑定前填
//   （0x644 那个字由排在前面的 GPU 拷贝覆盖：CPU 写在提交之前，拷贝在提交之后执行）。
// 正确性边界（任一不成立就照旧逐笔拷）：
//   ① 前瞻只越过纯状态命令、绘制、换目标和跳转；派发、传输、改缓冲（CPU / GPU）、屏障、清除、计数器、事件、调用子列表、列表尾都停。
//      游戏让一笔图形写对后面的间接读可见只能靠屏障，所以停在屏障就排除了通道内的图形生产者；派发 / 传输本来就先结束通道，也停。
//   ② 前移之后主段里录了写镜像的传输（rp_gpu_wr_gen 变）：间接记录可能已被改，整表作废。
//   ③ 段（rp_seg_serial）或命令缓冲换了：切片是那一段的暂存，作废。
//   ④ 按命令流顺序消费：这一笔必须是表里当前位置之后的某一项，记录里的间接地址、绑定时解析出的镜像位置都要和表里一致，阶段要被表里的掩码盖住。
// 阶段掩码：前瞻时不知道后面每笔的程序，按「这一会话里要拷过的阶段」的并集拷（g_hz_mask）；后面某笔要的阶段不在掩码里就照旧逐笔拷。
int rp_pass_is_open(void) __attribute__((weak));   // 只链 rp_draw.c 的隔离用例可以不给：拿不到通道状态就只在要拷的那一笔前移
const void *rp_draw_stream;
uint64_t rp_gpu_wr_gen;
enum { HZ_MAX = 128, HZ_SCAN = 4096 };
struct hz_ent { const void *rec; uint64_t ind; nvn_gpu_buf buf; uint64_t off; uint8_t indexed, mask, used, carried; nvn_gpu_slice sl[RP_PROG_STAGES]; };
struct hz_tab { struct hz_ent e[HZ_MAX]; int n, cur; nvn_gpu_cmd c; uint64_t seg, wgen; };
static struct hz_tab g_hzt[2]; static int g_hzi;   // 当前表 g_hzt[g_hzi]；重建写进另一张再换
static int g_hz_on = -1; static uint8_t g_hz_mask;
static int hz_mode(int v) { return v <= 0 ? 0 : v == 1 ? 1 : 2; }   // 0 逐笔拷；1 批量、每段一次 nvn_gpu_copy_buf（各带全屏障）；2 批量、一次多区域拷贝 + 只到读它的阶段的屏障（缺省）
static int hz_on(void) { if (g_hz_on < 0) g_hz_on = hz_mode(nvn_plat_flag("nvk_binst_hoist", 2)); return g_hz_on; }
static int hz_live(const struct hz_tab *t, nvn_gpu_cmd c) { return t->n && t->c == c && t->seg == rp_seg_serial() && t->wgen == rp_gpu_wr_gen; }
// 前瞻能越过的命令：只改回放状态、不碰缓冲内容、自己不结束通道（换目标可能结束通道，但不写缓冲：越过它只多拷几笔，见 ④）
static int hz_pass_op(uint32_t op) {
    switch (op) {
        case OP_SET_RT: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR:
        case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_STENCIL:
        case OP_POLY_OFFSET: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_BLEND_COLOR: case OP_BIND_IMG: case OP_DEPTH_CLAMP:
        case OP_VP_SWIZZLE: case OP_BIND_TEX1: case OP_PRIM_RESTART: case OP_SET_PATCH_SIZE: case OP_ALPHA_REF: case OP_RAST_DISCARD:
        case OP_PUSH_CONST: case OP_PUSH_CONST_PTR: return 1;
        default: return 0;
    }
}
// 从 rec（本笔记录头）往后前瞻，建新表：已在旧表里、还有效的项沿用（不再拷），其余解析间接记录、分切片、录拷贝，最后一道屏障。调用方保证通道已收。
static void hz_build(nvn_gpu_cmd c, const void *rec, uint8_t mask) {
    struct hz_tab *o = &g_hzt[g_hzi], *t = &g_hzt[g_hzi ^ 1];
    t->n = 0; t->cur = 0;
    const uint8_t *p = rec;
    for (int k = 0; k < HZ_SCAN && t->n < HZ_MAX; k++) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7)) break;
        if (h.op == OP_JUMP) { const uint8_t *to; memcpy(&to, p + 8, 8); if (!to) break; p = to; continue; }
        if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) {
            const int ix = h.op == OP_DRAW_IDX; uint64_t ind, mc, tfc = 0;
            if (ix) { p_draw_idx tt; const p_draw_idx *d = draw_idx_rec(p + 8, h.size, &tt); ind = d->ind; mc = d->mdi_cnt; }
            else { p_draw tt; const p_draw *d = draw_rec(p + 8, h.size, &tt); ind = d->ind; mc = d->mdi_cnt; tfc = d->tfc; }
            if (ind && !mc && !tfc) {
                struct hz_ent *e = &t->e[t->n++]; memset(e, 0, sizeof *e); e->rec = p; e->ind = ind; e->indexed = (uint8_t)ix;
            }
        } else if (!hz_pass_op(h.op)) break;
        p += h.size;
    }
    // 先把全部间接记录解析完（脏页同步可能录进主段），再判旧表还能不能沿用、再录拷贝：拷贝之后不能再有同步改这些记录
    static uint64_t ra[HZ_MAX], rl[HZ_MAX], ro[HZ_MAX]; static nvn_gpu_buf rb[HZ_MAX]; static unsigned char rok[HZ_MAX];
    for (int i = 0; i < t->n; i++) { ra[i] = t->e[i].ind; rl[i] = t->e[i].indexed ? 20 : 16; }
    if (t->n) rp_buf_resolve_multi(t->n, ra, rl, rb, ro, rok);   // 一次抢锁解析一批（同顶点流）
    int n = 0;
    for (int i = 0; i < t->n; i++) {
        if (!rok[i] || (ro[i] & 3)) continue;   // 解析不了：这一笔到时自己按原路记未接
        t->e[n] = t->e[i]; t->e[n].buf = rb[i]; t->e[n].off = ro[i]; n++;
    }
    t->n = n;
    const int live = hz_live(o, c);
    int j = live ? o->cur : o->n;
    for (int i = 0; i < t->n; i++) {
        struct hz_ent *e = &t->e[i];
        for (int q = j; q < o->n; q++) {
            const struct hz_ent *f = &o->e[q];
            if (f->rec != e->rec) continue;
            if (f->ind == e->ind && f->indexed == e->indexed && f->buf == e->buf && f->off == e->off && (f->mask & mask) == mask) {
                memcpy(e->sl, f->sl, sizeof e->sl); e->mask = f->mask; e->carried = 1; o->e[q].carried = 1;
            }
            j = q + 1; break;
        }
    }
    uint64_t waste = 0;
    for (int q = 0; q < o->n; q++) if (!o->e[q].used && !o->e[q].carried) waste++;
    if (waste) rp_perf_add(RP_PERF_BINST_WASTE, waste);
    static nvn_gpu_buf_copy cv[HZ_MAX * RP_PROG_STAGES]; uint32_t ncv = 0;
    for (int i = 0; i < t->n; i++) {
        struct hz_ent *e = &t->e[i];
        if (e->carried) { e->carried = 0; continue; }
        int ok = 1;
        for (int st = 0; st < RP_PROG_STAGES && ok; st++) if (mask >> st & 1) {
            if (!alloc_copy(c, DRV0_BYTES, NULL, &e->sl[st])) ok = 0;
        }
        if (!ok) { t->n = i; break; }   // 暂存区满：表截在这里，后面的照旧逐笔
        e->mask = mask;
        for (int st = 0; st < RP_PROG_STAGES; st++) if (mask >> st & 1) {
            cv[ncv++] = (nvn_gpu_buf_copy){e->buf, e->off + (e->indexed ? 16 : 12), e->sl[st].buf, e->sl[st].off + 0x644, 4};
        }
    }
    if (ncv && g_hz_on == 2) nvn_gpu_copy_bufs_uniform(c, cv, ncv, mask);   // 一次多区域拷贝，屏障只到读驱动段的这几个阶段（不发全屏障）
    else if (ncv) {   // 开关 1：每段一次拷贝（后端各带一次全屏障），最后一道同逐笔拷的屏障
        for (uint32_t k = 0; k < ncv; k++) nvn_gpu_copy_buf(c, cv[k].src, cv[k].soff, cv[k].dst, cv[k].doff, cv[k].bytes);
        nvn_gpu_barrier(c, NVN_BARRIER_ORDER_INDIRECT_DATA_BIT | NVN_BARRIER_INVALIDATE_SHADER_BIT);   // 同逐笔拷：传输写对驱动段常量读可见
    }
    for (int q = 0; q < o->n; q++) o->e[q].carried = 0;
    o->n = 0;
    t->c = c; t->seg = rp_seg_serial(); t->wgen = rp_gpu_wr_gen;
    g_hzi ^= 1;
    rp_perf_add(RP_PERF_BINST_SCAN, 1);
}
// 表里有这一笔、仍有效、阶段被盖住：返回它（并把消费位置挪到它后面），否则 NULL
static struct hz_ent *hz_take(nvn_gpu_cmd c, const void *rec, uint64_t ind, int indexed, nvn_gpu_buf buf, uint64_t off, uint8_t need) {
    struct hz_tab *t = &g_hzt[g_hzi];
    if (!hz_live(t, c)) return NULL;
    for (int i = t->cur; i < t->n; i++) {
        struct hz_ent *e = &t->e[i];
        if (e->rec != rec) continue;
        if (e->ind != ind || e->indexed != (uint8_t)indexed || e->buf != buf || e->off != off || (e->mask & need) != need) return NULL;
        t->cur = i + 1; e->used = 1;
        return e;
    }
    return NULL;
}
// 计算写的可见性与 CPU 回写账分开：完成回调落地不等于发布给下一笔 GPU 图形读取。
static struct rp_compute_access g_compute_access;
static nvn_gpu_access_span g_dispatch_access[RP_PROG_ENT_MAX];
static size_t g_ndispatch_access;
static int g_compute_access_on = -1;
static int compute_access_on(void) {
    if (g_compute_access_on < 0) g_compute_access_on = nvn_plat_flag("nvk_cgr", 1) != 0;
    return g_compute_access_on;
}
static int compute_read_buf(nvn_gpu_buf b, uint64_t off, uint64_t bytes) {
    nvn_gpu_access_span span;
    if (!b || !bytes) return 0;
    if (!nvn_gpu_buf_access_span(b, off, bytes, &span)) return -1;
    return rp_access_read(&g_compute_access, &span);
}
static int compute_read_view(nvn_gpu_view v) {
    nvn_gpu_access_span span;
    if (!v) return 0;   // 缺资源交给原来的绑定校验具名拒绝
    if (!nvn_gpu_view_access_span(v, &span)) return -1;
    return rp_access_read(&g_compute_access, &span);
}
// 弱链接[replay]：nvn_gpu_shader_write_end
// 弱链接[replay]：rp_post_gpu_write_prefix
// Optional backend reflection for the final compute module. Zero means no proven endpoint.
uint64_t nvn_gpu_shader_write_end(nvn_gpu_shader, unsigned, unsigned, unsigned, uint32_t) __attribute__((weak));
int rp_post_gpu_write_prefix(uint64_t, uint64_t, uint64_t) __attribute__((weak));
// 首个会话直接派发；退出时只清缓存，以便下个会话使用自己的属性值。
static int g_land_bound = -1;
static int land_bound_on(void) {
    if (g_land_bound < 0) g_land_bound = nvn_plat_flag("nvk_land_bound", 0) != 0;
    return g_land_bound;
}
static uint32_t g_disp_dim[3];   // 这次派发的组数（计算阶段的驱动段 c0 里要有）

// 两个省重开关（缺省开；正式包里 nvn_plat_flag 只返回缺省值）：
//   nvk_bind_fast：同一段里同程序 / 同 base_instance / 参数没变时整组沿用上一笔的绑定，不重建；
//   nvk_drv_memo：驱动段（DRV0 / DRV2）暂存按内容命中，不重复落 2.5 KB / 0.29 KB。
// 每 256 次重读一次，照后端 nvk_desc_memo 的读法。
static int flag_read(const char *name) {
    static struct { const char *n; int v; } f[4]; static int nf, k;
    if (!(++k & 255)) { for (int i = 0; i < nf; i++) f[i].v = -1; }
    for (int i = 0; i < nf; i++) if (!strcmp(f[i].n, name)) { if (f[i].v < 0) f[i].v = nvn_plat_flag(name, 1) != 0; return f[i].v; }
    if (nf < 4) { f[nf].n = name; f[nf].v = nvn_plat_flag(name, 1) != 0; return f[nf++].v; }
    return nvn_plat_flag(name, 1) != 0;
}

// B1 快路的前置检查：这个阶段的布局里有没有「每笔都必须在本笔重做」的项。
//   · 可写存储缓冲：每笔要 rp_post_gpu_write 登记 GPU 写回，跳过就不回写了；
//   · 间接寻址存储缓冲（UBOA）：每笔要读 UBO 里的指针、解析目标范围，内容可能刚变；
//   · 取不到的资源：完整构造会按原来的名字记账并跳过这一笔，快路不能把这一笔画出去。
static int fast_items_ok(const struct rp_prog *pg, int pi, int st, const nvn_gpu_view *tv, const nvn_gpu_samp *ts) {
    for (uint32_t i = 0; i < pg->nent[pi]; i++) {
        const nvn_gpu_layout_ent *e = &pg->ent[pi][i];
        switch (e->src_kind) {
        case NVN_RB_SBUFP: case NVN_RB_UBOP:
            if (e->access & NVN_GPU_ACC_W) return 0;
            if (i >= RP_PROG_ENT_MAX || !g_sb_ok[st][i]) return 0;
            break;
        case NVN_RB_UBOA: return 0;
        case NVN_RB_TBUF: return 0;  // 临时缓冲视图每笔重新建并绑定
        case NVN_RB_TEXP: case NVN_RB_SAMPP: return 0;   // 池快照不在 b[] 里，每笔在普通绑定之后重发 pool_bind
        case NVN_RB_SPILL: return 0;   // 每次派发一片新的溢出缓冲
        case NVN_RB_TEX: case NVN_RB_TEXU: {
            // 这一笔刚解析出来的视图 / 采样器必须与上一组里的逐项相同：视图被轮转重建过就交给完整构造重发。
            const uint32_t cnt = e->count > 1 ? e->count : 1;
            if (e->count > RP_TEXARR_MAX || i >= RP_PROG_ENT_MAX) return 0;
            const uint32_t at = g_lb[pi].bof[i];
            if ((uint64_t)at + cnt > g_lb[pi].n || at >= RP_PROG_ENT_MAX) return 0;
            const nvn_gpu_binding *pb = &g_lb[pi].b[at];
            if (e->count > 1) { for (uint32_t k = 0; k < cnt; k++) if (!g_av[pi][k] || !g_as[pi][k] || pb[k].u.ts.view != g_av[pi][k] || pb[k].u.ts.samp != g_as[pi][k]) return 0; }
            else if (!tv[i] || !ts[i] || pb->u.ts.view != tv[i] || pb->u.ts.samp != ts[i]) return 0;
            break; }
        case NVN_RB_IMG: {
            if (e->cls == NVN_GPU_RES_IMAGE_TBUF) return 0;   // 缓冲视图每笔重新建（且要登记写回），同 TBUF
            if (!tv[i] || i >= RP_PROG_ENT_MAX) return 0;
            const uint32_t at = g_lb[pi].bof[i];
            if ((uint64_t)at + 1 > g_lb[pi].n || at >= RP_PROG_ENT_MAX || g_lb[pi].b[at].u.view != tv[i]) return 0;
            break; }
        default: break;
        }
    }
    return 1;
}

// B2：两个程序在某个图形阶段的绑定布局是否等同（后端按各阶段布局的 set / binding / 类别 / 数量合并管线布局）。
// 等同 = 换管线不用把这个阶段的绑定重立脏；程序本身变了由 g_lb[pi].pg 让 keep 失效，照样完整重建。
static int prog_layout_same(const struct rp_prog *a, const struct rp_prog *b, int st) {
    if (!a || !b || a->nent[st] != b->nent[st]) return 0;
    for (uint32_t i = 0; i < a->nent[st]; i++) {
        const nvn_gpu_layout_ent *x = &a->ent[st][i], *y = &b->ent[st][i];
        if (x->set != y->set || x->binding != y->binding || x->cls != y->cls || x->count != y->count) return 0;
    }
    return 1;
}
// 整池数组：开通道前取好的池快照（rp_pool_table），bind_stage_at 在普通绑定之后逐项 nvn_gpu_pool_bind。
//   行 = 图形阶段 0..RP_PROG_STAGES-1，最后一行（RP_PROG_STAGES）是计算阶段（硬件阶段 5，见 pool_row）
static nvn_gpu_pool_table g_ptd[RP_PROG_STAGES + 1][RP_PROG_ENT_MAX];
static int pool_row(int st) { return st == 5 ? RP_PROG_STAGES : st >= 0 && st < RP_PROG_STAGES ? st : -1; }
// 取一项整池数组（NVN_RB_TEXP / SAMPP）的池快照，存进 g_ptd[row][i]；绘制与计算派发共用，行为一致。
//   count = 池长 + 1（表尾 dummy），池长须与编译时一致；返回 NULL = 取到了，否则是具名跳过的原因
static const char *pool_table_resolve(nvn_gpu_cmd c, const struct rp_prog *pg, int pi, uint32_t i, int row) {
    const nvn_gpu_layout_ent *e = &pg->ent[pi][i];
    const int sp = e->src_kind == NVN_RB_SAMPP;
    const uint32_t pn = e->count ? (uint32_t)e->count - 1 : 0;
    if (row < 0 || i >= RP_PROG_ENT_MAX) return "pool_table_unresolved";
    if (pn != (uint32_t)(sp ? rp_S.samp_pool_n : rp_S.tex_pool_n)) return "pool_length_mismatch";
    const char *why = NULL;
    g_ptd[row][i] = rp_pool_table(c, sp, sp ? rp_S.samp_base : rp_S.tex_base, pn, pg->dtt[pi][i], pg->dcls[pi][i], &why);
    return g_ptd[row][i] ? NULL : why ? why : "pool_table_unresolved";
}
static int bind_stage_at(nvn_gpu_cmd c, const struct rp_prog *pg, int pi, int st, uint32_t base_instance, const nvn_gpu_view *tv, const nvn_gpu_samp *ts, const void *rec);
static nvn_gpu_slice g_spill; static uint64_t g_spill_len;   // 本次派发的共享内存溢出缓冲（rp_dispatch 分配，绑定时用；0 = 没有）
static int g_spill_on = -1, g_spill_said;   // 平台开关 nvk_spill（缺省开；关时 spill 程序的派发报名跳过，真机 A/B 用）；会话内前 64 次 spill 派发各记一行
static int bind_stage(nvn_gpu_cmd c, const struct rp_prog *pg, int st, uint32_t base_instance, const nvn_gpu_view *tv, const nvn_gpu_samp *ts, const void *rec) { return bind_stage_at(c, pg, st, st, base_instance, tv, ts, rec); }
static int bind_stage_at(nvn_gpu_cmd c, const struct rp_prog *pg, int pi, int st, uint32_t base_instance, const nvn_gpu_view *tv, const nvn_gpu_samp *ts, const void *rec) {
    nvn_gpu_binding b[RP_PROG_ENT_MAX]; uint32_t n = 0;
    int has_tbuf = 0, npool = 0;
    // 沿用上一笔：只限图形两个阶段；存储缓冲（要同步镜像、登记 GPU 写）和贴图（视图会轮转）每笔照取
    int keep = 0;
    {   // 逐条判「整组沿用」能不能成立，并记下失败原因（读数：真机上这一条几乎从不成立，要先看清是被哪一项否掉的）
        int why = -1;
        if (st < RP_PROG_STAGES && !g_ind_drv_ok[st] && !(st == NVN_GPU_SH_FS && g_cold_drv.buf) && g_bind_keep[pi] && g_lb[pi].ok) {   // #941：本笔用 GPU 写出的 DRV0 的阶段不能沿用
            if (g_lb[pi].c != c) why = RP_PERF_KEEP_F_CMD;
            else if (g_lb[pi].seg != rp_seg_serial()) why = RP_PERF_KEEP_F_SEG;
            else if (g_lb[pi].gen != rp_cpu_write_gen) why = RP_PERF_KEEP_F_GEN;
            else if (g_lb[pi].reg_gen != rp_tex_reg_gen()) why = RP_PERF_KEEP_F_REG;
            else if (g_lb[pi].pg != pg) why = RP_PERF_KEEP_F_PG;
            else if (g_lb[pi].base_instance != base_instance) why = RP_PERF_KEEP_F_BI;
            else keep = 1;
        }
        if (keep) rp_perf_add(RP_PERF_KEEP_OK, 1); else if (why >= 0) rp_perf_add(why, 1);
    }
    // B1 快路：上一笔交给后端的这一组就是这一笔要的，不重建 b[]（省掉每笔每阶段的驱动段暂存拷贝与整轮贴图视图取用）。
    // 不能沿用的：可写存储缓冲（每笔要登记 GPU 写回）、间接寻址存储缓冲（每笔要读指针重新解析）、取不到的资源（交给完整构造去记账跳过）。
    // 贴图视图由 fast_items_ok 逐项与本笔刚解析的结果比对：视图被轮转重建过就不走快路。
    // 换管线不再挡沿用：同一程序 ⇒ 同一套绑定布局（后面换管线的分支只对「布局真的变了」的阶段立脏），
    // 描述符组在 Vulkan 里也不随管线对象失效，只有布局不兼容才必须重发。真机上一帧 383 次换管线、程序却没变，
    // 全被这条挡掉了（读数见「分时（绑定）」行）。
    if (keep && flag_read("nvk_bind_fast")) {
        if (fast_items_ok(pg, pi, st, tv, ts)) { if (g_pipe_set) rp_perf_add(RP_PERF_KEEP_F_PIPESET, 1); rp_perf_add(RP_PERF_BIND_FAST, 1); return 1; }
        rp_perf_add(RP_PERF_KEEP_F_ITEMS, 1);
    }
    static int btm = -1; static unsigned btk; if (btm < 0 || !(++btk & 255)) btm = nvn_plat_flag("nvk_bind_tm", 0) != 0;
    g_btm = btm;
    const uint64_t btm0 = btm ? dt_now() : 0;
    if (st < RP_PROG_STAGES) g_lb[pi].ok = 0;
    for (uint32_t i = 0; i < pg->nent[pi]; i++) {
        const nvn_gpu_layout_ent *e = &pg->ent[pi][i];
        if (st < RP_PROG_STAGES) g_lb[pi].bof[i] = (uint16_t)n;   // 上一组的布局项 → b[] 起始下标（B1 快路逐项比对贴图视图用）
        if (e->src_kind == NVN_RB_TEXP || e->src_kind == NVN_RB_SAMPP) {   // 池快照不占 b[]：普通绑定发完再 pool_bind（见函数末尾）
            const int pr = pool_row(st);
            if (pr < 0 || i >= RP_PROG_ENT_MAX || !g_ptd[pr][i]) return skip("pool_table_unresolved", rec);
            npool++; continue;
        }
        nvn_gpu_binding *o = &b[n]; memset(o, 0, sizeof *o); o->ent = (uint16_t)i; o->cls = e->cls;
        nvn_gpu_slice sl;
        if (keep && e->src_kind != NVN_RB_SBUFP && e->src_kind != NVN_RB_TEX && e->src_kind != NVN_RB_TEXU && e->src_kind != NVN_RB_IMG && e->src_kind != NVN_RB_TBUF) { *o = g_lb[pi].b[n]; n++; continue; }
        switch (e->src_kind) {
            case NVN_RB_SBUFP: case NVN_RB_UBOP: {   // 存储缓冲：取地址、绑池镜像里的那一段；可写的登记一次 GPU 写（完成后回写游戏内存）。两种来源只差在哪张表给地址：
                // SBUFP 按 NVN 存储缓冲槽（rp_S.ssbo）；UBOP 按计算阶段的用户 UBO 槽（rp_S.ubo；反射映射 NVK_STORAGE_UBO_PTR），段的范围就是这个 UBO 绑定的地址与大小
                const p_bind_buf *sb = e->src_reg < 16 ? (e->src_kind == NVN_RB_UBOP ? &rp_S.ubo[st][e->src_reg] : &rp_S.ssbo[st][e->src_reg]) : NULL;
                if (sb && e->src_kind == NVN_RB_SBUFP && sb_unmapped(sb)) {   // 没有可绑真实内存的存储缓冲槽：绑 64 KiB 空缓冲、照画（见 sb_unmapped）
                    o->u.b.buf = rp_buf_for_null_storage();
                    if (!o->u.b.buf) return skip("storage_buffer_empty_failed", rec);
                    o->u.b.off = 0; o->u.b.len = 0x10000; break;
                }
                const int sbufp = e->src_kind == NVN_RB_SBUFP;
                if (!sb || !sb_len(sb, sbufp)) return skip("storage_buffer_unbound", rec);
                const uint64_t blen = sb_len(sb, sbufp);   // 「地址有效、长度 0」的槽是池剩余长度（见 sb_len）
                uint64_t wa, wl; const int clamped = sb_window(sb, sbufp, &wa, &wl);
                nvn_gpu_buf gb; uint64_t go;
                if (st < RP_PROG_STAGES && i < RP_PROG_ENT_MAX && g_sb_ok[st][i]) { gb = g_sb_buf[st][i]; go = g_sb_off[st][i]; }   // 开通道前已按同一窗口同步（见 rp_draw 开头）
                else if (!rp_buf_resolve(c, wa, wl, &gb, &go)) return skip("storage_buffer_unresolved", rec);
                // 镜像偏移必须对齐；UBOP 着色器按原基址取数，不能通过移动窗口补救。
                const uint32_t al = caps_cached()->min_ssbo_align;
                if (al > 1 && (go & (al - 1))) return skip("storage_buffer_offset_unaligned", rec);
                if (clamped & 1) nvn_unimpl("draw", "storage_buffer_window_clamped", rec);   // 超出 maxStorageBufferRange 的部分着色器访问不到：照画，具名记账
                if (clamped & 2) nvn_unimpl("draw", "storage_buffer_past_pool_end_clamped", rec);   // 伸出池尾的部分没有存储：照画，具名记账
                // 回写只登记游戏绑定的那一段（窗口前补的余数字节不是这个缓冲的，回写会盖掉相邻内容）
                if (e->access & NVN_GPU_ACC_W) { const uint64_t reach = wa + wl - sb->addr;   // 窗口里属于这个缓冲的长度（余数 < 对齐 ≤ 256，上限 ≥ 128 MiB，不会为负）
                    const uint64_t bytes = reach < blen ? reach : blen;
                    uint64_t copy_bytes = bytes;
                    if (st == 5 && sbufp && !g_disp_ind && g_disp_dim[0] &&
                        g_disp_dim[1] && g_disp_dim[2] && land_bound_on() && nvn_gpu_shader_write_end && rp_post_gpu_write_prefix) {
                        uint64_t end = nvn_gpu_shader_write_end(pg->sh[pi], i, e->set, e->binding, g_disp_dim[0]);
                        uint64_t padding = sb->addr - wa;
                        if (end > padding && end - padding < bytes) copy_bytes = end - padding;
                    }
                    if (copy_bytes < bytes) rp_post_gpu_write_prefix(sb->addr, bytes, copy_bytes);
                    else rp_post_gpu_write(sb->addr, bytes); }
                o->u.b.buf = gb; o->u.b.off = go; o->u.b.len = wl; break; }
            case NVN_RB_UBOA: {   // 计算存储缓冲来自「用户 UBO 槽 r 内 src_off 处的 8 字节 guest 裸指针」（map 3，NVK_STORAGE_UBO_ADDRESS）：
                // 指针字节必须当前：先按写的来源解析那 8 字节（池镜像里 GPU 写过的部分不会被旧 guest 字节盖掉），
                // 再从镜像的 CPU 映射读数；目标长度按所在池的余量，并夹到 SSBO 描述符范围上限。
                const p_bind_buf *ub = e->src_reg < 16 ? &rp_S.ubo[st][e->src_reg] : NULL;
                if (!ub || !ub->addr || !ub->size) return skip("uniform_buffer_unbound", rec);
                if ((uint64_t)e->src_off + 8 > ub->size) return skip("uniform_buffer_indirect_offset_outside", rec);
                if (e->access & NVN_GPU_ACC_W) return skip("compute_storage_indirect_writable_unknown", rec);   // 长度未知的可写绑定会把回写扩到相邻分配：准确拒绝
                uint64_t paddr = ub->addr + e->src_off;
                nvn_gpu_buf gb; uint64_t go;
                if (!rp_buf_resolve(c, paddr, 8, &gb, &go)) return skip("uniform_buffer_unresolved", rec);
                uint8_t *cpu = nvn_gpu_buf_cpu(gb);
                if (!cpu) return skip("uniform_buffer_not_host_readable", rec);
                uint64_t target = 0; memcpy(&target, cpu + go, 8);
                target = nvn_ga2ca(target);
                if (!target) return skip("compute_storage_indirect_null", rec);
                const uint8_t *pmem = NULL; uint64_t psz = 0;
                if (!rp_pool_find(target, 1, &pmem, &psz)) return skip("storage_buffer_outside_pools", rec);
                uint64_t extent = psz - (target - (uint64_t)(uintptr_t)pmem);
                { nvn_gpu_caps cp; nvn_gpu_caps_get(&cp);   // 本函数作用域里没有外层 caps：这里自己取一次
                  if (cp.max_ssbo_range && extent > cp.max_ssbo_range) extent = cp.max_ssbo_range; }
                if (!extent) return skip("storage_buffer_indirect_empty", rec);
                if (!rp_buf_resolve(c, target, extent, &gb, &go)) return skip("storage_buffer_unresolved", rec);
                const uint32_t al = caps_cached()->min_ssbo_align;
                if (al > 1 && (go & (al - 1))) return skip("storage_buffer_offset_unaligned", rec);
                o->u.b.buf = gb; o->u.b.off = go; o->u.b.len = extent; break; }
            case NVN_RB_SUPPORT: {
                uint32_t flags = (rp_depth_zero() ? 0u : 1u) | 2u;   // 位 0：深度 −1..1；位 1：base vertex / instance 取内建（后端基线要求 shaderDrawParameters）
                // 这块内容只取决于长度与这两位：同一次提交的同一段里填过一份就接着用（每笔每阶段新建一份 2.6 KB 是回放里的固定开销）
                static struct { nvn_gpu_cmd c; uint64_t sub; uint32_t bytes, flags; nvn_gpu_slice sl; } sup[4]; static int sup_next;
                { int hit = -1; uint64_t sub = rp_seg_serial();
                  for (int k = 0; k < 4; k++) if (sup[k].c == c && sup[k].sub == sub && sup[k].bytes == pg->support_bytes && sup[k].flags == flags) hit = k;
                  if (hit >= 0) { o->u.b.buf = sup[hit].sl.buf; o->u.b.off = sup[hit].sl.off; o->u.b.len = pg->support_bytes; break; } }
                if (!alloc_copy(c, pg->support_bytes, NULL, &sl)) return skip("arena_alloc_failed", rec);
                if (SB_S2A_FLAGS + 4 <= pg->support_bytes) memcpy(sl.cpu + SB_S2A_FLAGS, &flags, 4);
                float one = 1.0f;
                if (SB_S2A_POINT + 4 <= pg->support_bytes) memcpy(sl.cpu + SB_S2A_POINT, &one, 4);
                for (uint32_t k = 0; k < SB_RENDER_SCALE_COUNT; k++) {
                    if (SB_RENDER_SCALE + k * 16 + 4 <= pg->support_bytes) memcpy(sl.cpu + SB_RENDER_SCALE + k * 16, &one, 4);
                    if (SB_RENDER_SCALE_Y + k * 16 + 4 <= pg->support_bytes) memcpy(sl.cpu + SB_RENDER_SCALE_Y + k * 16, &one, 4);
                }
                sup[sup_next].c = c; sup[sup_next].sub = rp_seg_serial(); sup[sup_next].bytes = pg->support_bytes; sup[sup_next].flags = flags; sup[sup_next].sl = sl; sup_next = (sup_next + 1) & 3;
                o->u.b.buf = sl.buf; o->u.b.off = sl.off; o->u.b.len = pg->support_bytes; break; }
            case NVN_RB_DRV0: {
                if (st == NVN_GPU_SH_FS && g_cold_drv.buf) {
                    o->u.b.buf = g_cold_drv.buf;
                    o->u.b.off = g_cold_drv.off;
                    o->u.b.len = DRV0_BYTES;
                    break;
                }
                if (st < RP_PROG_STAGES && g_ind_drv_ok[st]) {   // #941：间接绘制的 DRV0 由 GPU 写出，直接用那一片，不走内容缓存
                    o->u.b.buf = g_ind_drv[st].buf; o->u.b.off = g_ind_drv[st].off; o->u.b.len = DRV0_BYTES; break;
                }
                const uint64_t drv0_t0 = btm ? dt_now() : 0;
                drv0_key k; memset(&k, 0, sizeof k);
                k.c = c; k.seg = rp_seg_serial(); k.st = (uint32_t)st; k.base_instance = base_instance;
                for (int r = 0; r < 16; r++) { k.sb[r][0] = rp_S.ssbo[st][r].addr; k.sb[r][1] = rp_S.ssbo[st][r].size; }
                int cacheable = 1;
                if (st == 5) {   // 计算阶段的驱动段（nvn_mtl.m @9897）：0x200 派发组数，0x210 + 16r 常量缓冲表，0x310 + 16r 存储缓冲表（各项：地址 64 位、长度 32 位）
                    if (g_disp_ind) cacheable = 0;   // 间接派发：0x200 是从缓冲里拷过来的，不能复用旧内容
                    else { k.has_ub = 1;
                        for (int r = 0; r < 16; r++) { k.ub[r][0] = rp_S.ubo[5][r].addr; k.ub[r][1] = rp_S.ubo[5][r].size; }
                        memcpy(k.dim, g_disp_dim, sizeof k.dim); }
                }
                if (cacheable && flag_read("nvk_drv_memo") && drv0_lookup(&k, &sl)) {   // 同一段里内容相同：复用上次那片
                    rp_perf_add(RP_PERF_BIND_SLICE_HIT, 1);
                    o->u.b.buf = sl.buf; o->u.b.off = sl.off; o->u.b.len = DRV0_BYTES; break;
                }
                if (!alloc_copy(c, DRV0_BYTES, NULL, &sl)) return skip("arena_alloc_failed", rec);
                if (st == 5) {
                    memcpy(sl.cpu + 0x200, g_disp_dim, 12);
                    if (g_disp_ind) nvn_gpu_copy_buf(c, g_disp_ind, g_disp_ind_off, sl.buf, sl.off + 0x200, 12);
                    for (int r = 0; r < 16; r++) { uint64_t a = rp_S.ubo[5][r].addr; uint32_t sz = (uint32_t)rp_S.ubo[5][r].size; memcpy(sl.cpu + 0x210 + 16 * r, &a, 8); memcpy(sl.cpu + 0x210 + 16 * r + 8, &sz, 4); }
                    for (int r = 0; r < 16; r++) { uint64_t a = rp_S.ssbo[5][r].addr; uint32_t sz = (uint32_t)rp_S.ssbo[5][r].size; memcpy(sl.cpu + 0x310 + 16 * r, &a, 8); memcpy(sl.cpu + 0x310 + 16 * r + 8, &sz, 4); }
                } else {
                    for (int r = 0; r < 16; r++) { const p_bind_buf *sb = &rp_S.ssbo[st][r]; if (!sb->addr) continue;
                        uint64_t a = sb->addr; uint32_t sz = (uint32_t)sb->size; memcpy(sl.cpu + k_drv0_base[st] + 16 * r, &a, 8); memcpy(sl.cpu + k_drv0_base[st] + 16 * r + 8, &sz, 4); }
                    float max_point = 2047.0f; memcpy(sl.cpu + 0x610, &max_point, 4); memcpy(sl.cpu + 0x644, &base_instance, 4);
                }
                if (cacheable && flag_read("nvk_drv_memo")) drv0_store(&k, &sl);
                o->u.b.buf = sl.buf; o->u.b.off = sl.off; o->u.b.len = DRV0_BYTES; if (btm) g_bt[3] += dt_now() - drv0_t0; break; }
            case NVN_RB_DRV2: {   // 着色器把驱动句柄表 c2 当常量缓冲读：按完整驱动表填（同 iOS nvn_mtl.m DRV2，C2_TEX / C2_IMG / C2_SEPTEX / C2_SAMP）
                //   每项 8 字节、句柄在低 4 字节：组合槽 r 在 0x20 + 8r（r ≥ 32 与图像表同址，只在绑了该组合槽、且同址的图像槽没绑时写，同 iOS「Hades II」那条）；
                //   图像槽 r 在 0x120 + 8r；分离贴图槽 r 在 0x168 + 8r；分离采样器槽 r 在 0x568 + 8r（采样器号 << 20）。整段 0xA00，其余为 0
                enum { DRV2_BYTES = 0xA00, C2_TEX = 0x20, C2_IMG = 0x120, C2_SEPTEX = 0x168, C2_SAMP = 0x568 };
                drv2_key k; memset(&k, 0, sizeof k); k.c = c; k.seg = rp_seg_serial(); k.st = (uint32_t)st;
                for (int r = 0; r < 41; r++) k.h[r] = rp_S.tex[st][r];
                for (int r = 0; r < 8; r++) k.img[r] = rp_S.img[st][r];
                for (int r = 0; r < 128; r++) { k.sep[r] = rp_S.tex[st][128 + r]; k.samp[r] = rp_S.samp[st][r]; }
                if (flag_read("nvk_drv_memo") && drv2_lookup(&k, &sl)) {
                    rp_perf_add(RP_PERF_BIND_SLICE_HIT, 1);
                    o->u.b.buf = sl.buf; o->u.b.off = sl.off; o->u.b.len = DRV2_BYTES; break;
                }
                if (!alloc_copy(c, DRV2_BYTES, NULL, &sl)) return skip("arena_alloc_failed", rec);
                for (int r = 0; r < 8; r++) { uint32_t v = (uint32_t)k.img[r]; memcpy(sl.cpu + C2_IMG + 8 * r, &v, 4); }
                for (int r = 0; r < 41; r++) {
                    if (r >= 32 && (!k.h[r] || (r - 32 < 8 && k.img[r - 32]))) continue;
                    uint32_t v = (uint32_t)k.h[r]; memcpy(sl.cpu + C2_TEX + 8 * r, &v, 4);
                }
                for (int r = 0; r < 128; r++) { uint32_t v = (uint32_t)k.sep[r]; memcpy(sl.cpu + C2_SEPTEX + 8 * r, &v, 4); }
                for (int r = 0; r < 128; r++) { uint32_t v = (uint32_t)k.samp[r]; memcpy(sl.cpu + C2_SAMP + 8 * r, &v, 4); }
                if (flag_read("nvk_drv_memo")) drv2_store(&k, &sl);
                o->u.b.buf = sl.buf; o->u.b.off = sl.off; o->u.b.len = DRV2_BYTES; break; }
            case NVN_RB_C1: {
                if (!(pg->c1_ok >> pi & 1)) return skip("c1_segment_not_captured", rec);
                rp_perf_add(RP_PERF_UBO_C1_N, 1);
                // 程序自带常量段整段绑（内容恒定，段内一直有效）。只绑前 0x100 字节时，着色器读后段全得 0（八方旅人调色 LUT 读到第 2304 字节 → 场景整幅黑）。
                //   长度按 16 字节补齐（std140 vec4 数组）；补齐部分是暂存区里的 0，着色器读到的与「越界读 0」一致。
                const uint32_t c1n = (pg->c1_len[pi] + 15u) & ~15u;
                { const nvn_gpu_caps *cp = caps_cached(); if (cp->max_ubo_range && c1n > cp->max_ubo_range) return skip("c1_segment_over_ubo_range", rec); }
                if (!slc_get_x(c, (uint64_t)(uintptr_t)pg ^ (uint64_t)(pi + 1) << 60, c1n, pg->c1[pi], &sl, 1)) return skip("arena_alloc_failed", rec);
                o->u.b.buf = sl.buf; o->u.b.off = sl.off; o->u.b.len = c1n; break; }
            case NVN_RB_CBUF: {
                const uint64_t cbuf_t0 = btm ? dt_now() : 0;
                const p_bind_buf *ub = &rp_S.ubo[st][e->src_reg];
                if (!ub->addr || !ub->size) return skip("uniform_buffer_unbound", rec);
                // 用户常量缓冲声明为 64 KiB（MAX_UNIFORM_BUFFER_SIZE、Ryujinx ConstantBufferSize）。
                const uint64_t len = cbuf_len(ub);   // 与准备阶段同一个有效长度
                const nvn_gpu_caps *cp = caps_cached();
                // 设备范围不能作为裁剪上限，否则着色器仍可能访问被截掉的字节。
                // 在交给后端前具名跳过此笔，避免后端的范围错误使整段作废。
                if (cp->max_ubo_range && len > cp->max_ubo_range) return skip("uniform_buffer_exceeds_device_range", rec);
                if (!rp_pool_find(ub->addr, len, NULL, NULL)) return skip("uniform_buffer_outside_pools", rec);
                rp_perf_add(RP_PERF_UBO_N, 1); rp_perf_add(RP_PERF_UBO_KB, len >> 10);
                if (rp_buf_gpu_pending(ub->addr, len)) {   // 尚未落地的 GPU 写（#1022）：游戏内存是旧内容，直接绑镜像
                    const uint64_t al = cp->min_ubo_align ? cp->min_ubo_align : 256;
                    nvn_gpu_buf mb; uint64_t mo;
                    const int have = st < RP_PROG_STAGES && i < RP_PROG_ENT_MAX && g_ub_ok[st][i];
                    int mok = 0;
                    if (have) { mb = g_ub_buf[st][i]; mo = g_ub_off[st][i]; mok = 1; }   // 图形：开通道前已解析，这里不再解析
                    else if (st >= RP_PROG_STAGES) {                                   // 计算：本来就在通道外，现场解析安全
                        mok = rp_buf_resolve(c, ub->addr, len, &mb, &mo);
                    } else {
                        // 图形阶段而准备阶段没给出镜像：**绝不在这里解析**——同一次提交里的第二次同步要录成传输
                        // （rp_xfer_cmd），那个函数结束当前通道，绑定与绘制就落在通道外（真机：整屏黑、每帧 2 笔）。
                        // 照旧走下面的暂存路径（内容是游戏内存里的旧值）并记账；不具名跳过是因为这一笔本身是合法绘制，
                        // 缺一段常量比缺整笔轻，也与相邻的「镜像偏移不对齐」那条保持同一处置。
                        nvn_unimpl("draw", "uniform_gpu_pending_unprepared", rec);
                    }
                    if (mok && !(mo & (al - 1))) {
                        rp_perf_add(RP_PERF_UBO_MIRROR_N, 1);
                        o->u.b.buf = mb; o->u.b.off = mo; o->u.b.len = len; if (btm) g_bt[4] += dt_now() - cbuf_t0; break;
                    }
                    nvn_unimpl("draw", "uniform_gpu_pending_unaligned", rec);   // 镜像无法绑定：保留原有暂存路径并记账
                }
                // 暂存编码时看到的内容，按序的 OP_UPDATE_UB 之后会取新值。
                if (!slc_get(c, ub->addr, len, (const void *)(uintptr_t)ub->addr, &sl)) return skip("arena_alloc_failed", rec);
                o->u.b.buf = sl.buf; o->u.b.off = sl.off; o->u.b.len = len; if (btm) g_bt[4] += dt_now() - cbuf_t0; break; }
            case NVN_RB_SPILL:   // 共享内存溢出缓冲：rp_dispatch 按这次的组数分好的那一片
                if (st != 5 || !g_spill_len) return skip("compute_spill_unallocated", rec);
                o->u.b.buf = g_spill.buf; o->u.b.off = g_spill.off; o->u.b.len = g_spill_len; break;
            case NVN_RB_IMG: {
                if ((e->cls != NVN_GPU_RES_IMAGE && e->cls != NVN_GPU_RES_IMAGE_TBUF) || e->count > 1) return skip(st == 5 ? "compute_image_shape" : "graphics_image_shape", rec);
                if (!tv[i]) return skip(st == 5 ? "compute_image_unresolved" : "graphics_image_unresolved", rec);
                if (e->cls == NVN_GPU_RES_IMAGE_TBUF) has_tbuf = 1;   // 本笔临时建的缓冲视图：这一组不留作下一笔沿用
                o->u.view = tv[i]; break; }
            case NVN_RB_TBUF: {
                if (e->cls != NVN_GPU_RES_TBUF || !tv[i]) return skip("tbuf_view_unresolved", rec);
                has_tbuf = 1; o->u.view = tv[i]; break; }
            case NVN_RB_TEX: case NVN_RB_TEXU: {
                if (e->count > 1) {   // 贴图数组：逐项绑（视图与采样器已在开通道前取好，放在 g_av / g_as）
                    if (n + e->count > RP_PROG_ENT_MAX) return skip("too_many_bindings", rec);
                    for (uint32_t k = 0; k < e->count; k++) {
                        if (!g_av[pi][k]) return skip("texture_slot_unresolved", rec);
                        if (!g_as[pi][k]) return skip("sampler_slot_unresolved", rec);
                        nvn_gpu_binding *ok = &b[n + k]; memset(ok, 0, sizeof *ok); ok->ent = (uint16_t)i; ok->cls = e->cls; ok->elem = k;
                        ok->u.ts.view = g_av[pi][k]; ok->u.ts.samp = g_as[pi][k];
                    }
                    n += e->count - 1;   // 循环末尾再加 1
                    break;
                }
                if (!tv[i]) return skip("texture_slot_unresolved", rec);
                if (!ts[i]) return skip("sampler_slot_unresolved", rec);
                o->u.ts.view = tv[i]; o->u.ts.samp = ts[i]; break; }
            default: return skip("binding_source_unknown", rec);
        }
        n++;
    }
    if (st == 5 && compute_access_on()) {
        for (uint32_t i = 0; i < n; i++) {
            if (!(pg->ent[pi][b[i].ent].access & NVN_GPU_ACC_W)) continue;
            nvn_gpu_access_span span; int ok = 0;
            if (b[i].cls == NVN_GPU_RES_SSBO) ok = nvn_gpu_buf_access_span(b[i].u.b.buf, b[i].u.b.off, b[i].u.b.len, &span);
            else if (b[i].cls == NVN_GPU_RES_IMAGE || b[i].cls == NVN_GPU_RES_IMAGE_TBUF) ok = nvn_gpu_view_access_span(b[i].u.view, &span);
            if (!ok || g_ndispatch_access == RP_PROG_ENT_MAX) return skip("compute_write_span_unresolved", rec);
            g_dispatch_access[g_ndispatch_access++] = span;
        }
        // 编码派发前保证记账空间；分配失败必须拒绝，不能执行一笔此后无法追踪的写。
        if (!rp_access_reserve(&g_compute_access, g_ndispatch_access)) return skip("compute_write_tracking_alloc_failed", rec);
    }
    if (st < RP_PROG_STAGES) {
        // 和上一笔逐项相同、管线也没重设：后端手里的就是这一组，不用再发
        const int same = keep && !has_tbuf && !npool && g_lb[pi].n == n && !memcmp(g_lb[pi].b, b, (size_t)n * sizeof *b);
        g_lb[pi].c = c; g_lb[pi].seg = rp_seg_serial(); g_lb[pi].gen = rp_cpu_write_gen; g_lb[pi].reg_gen = rp_tex_reg_gen(); g_lb[pi].pg = pg; g_lb[pi].base_instance = base_instance; g_lb[pi].n = n; g_lb[pi].ok = !has_tbuf && !g_ind_drv_ok[st];   // #941：用 GPU 写出 DRV0 的这一组不留作下一笔沿用；纹素缓冲视图是本笔临时建的，也不留
        if (same) { rp_perf_add(RP_PERF_BIND_SKIP, 1); return 1; }
        memcpy(g_lb[pi].b, b, (size_t)n * sizeof *b);
    }
    const nvn_gpu_stage gst = st == 5 ? NVN_GPU_SH_CS : (nvn_gpu_stage)st;   // 普通绑定与池表绑定同一阶段
    { const uint64_t bb = btm ? dt_now() : 0;
      nvn_gpu_bind(c, gst, b, n);
      if (btm) { const uint64_t be = dt_now(); g_bt[1] += be - bb; g_bt[2]++; g_bt[0] += be - btm0; } }
    if (npool) {   // 整池数组：后端按布局项把整张池表绑上（快照由后端引用到命令段完成）
        for (uint32_t i = 0; i < pg->nent[pi]; i++) {
            const nvn_gpu_layout_ent *e = &pg->ent[pi][i];
            if (e->src_kind != NVN_RB_TEXP && e->src_kind != NVN_RB_SAMPP) continue;
            nvn_gpu_err err = {0};
            if (!nvn_gpu_ok(nvn_gpu_pool_bind(c, gst, i, g_ptd[pool_row(st)][i], &err))) {
                if (nvn_unimpl("draw", "pool_bind_failed", rec) == 1 && err.why[0]) nvn_plat_log(NVN_PLAT_LOG_RED, "draw", err.why);
                return 0;
            }
        }
        if (st < RP_PROG_STAGES) g_lb[pi].ok = 0;   // 有池表的这组不留作下一笔整组沿用
    }
    return 1;
}

// 绘制时的动态状态（对照 nvn_mtl.m @12740–12776）。返回 0 = 裁剪矩形与目标没有交集：这笔什么都画不出，不发（iOS 同样跳过，不算未接）
static int fill_dyn(nvn_gpu_dyn *d, uint32_t tw, uint32_t th, const void *rec) {
    memset(d, 0, sizeof *d);
    d->nvp = 1;
    if (rp_S.has_vp) { memcpy(d->vp[0], rp_S.vp[0], 16); } else { d->vp[0][2] = (float)tw; d->vp[0][3] = (float)th; }
    d->vp[0][4] = rp_S.dr[0]; d->vp[0][5] = rp_S.dr[1];
    // 视口重排：只认「y 取反」（等于视口上下翻，正面判定在它之后做，绕序状态不用动）；其余重排照没重排画，记一笔
    { uint32_t sw; memcpy(&sw, rp_S.vp_sw[0], 4);
      if (sw == 0x100) { d->vp[0][1] += d->vp[0][3]; d->vp[0][3] = -d->vp[0][3]; }
      else if (sw) nvn_unimpl("gpu", "viewport_swizzle", rec); }
    // 裁剪矩形夹到目标范围内（原生接口不收负的起点；宽高是游戏给的任意数）
    if (rp_R.has_sc) {
        p_scissor sc;
        if (!rp_clip_scissor(&rp_R.sc, tw, th, &sc)) return 0;
        d->has_sc = 1; d->sc[0] = sc.x; d->sc[1] = sc.y; d->sc[2] = sc.w; d->sc[3] = sc.h;
    }
    d->origin_ll = (uint8_t)rp_origin_lower_left(); d->depth_clamp = (uint8_t)(rp_S.depth_clamp != 0);
    d->po = rp_S.po;
    memcpy(d->st_vmask, rp_S.st_vmask, sizeof d->st_vmask); memcpy(d->st_wmask, rp_S.st_wmask, sizeof d->st_wmask); memcpy(d->st_ref, rp_S.st_ref, sizeof d->st_ref);
    memcpy(d->blend_color, rp_S.blend_color, sizeof d->blend_color);
    d->ds = rp_S.ds; d->poly = rp_S.poly;
    return 1;
}

// #658：小组由回放器以已解码记录单遍积累，禁止前瞻读取命令流。
static uint32_t g_decoded_count_n; static uint64_t g_decoded_count_stride;
void rp_draw_count_batch(nvn_gpu_cmd c, const p_draw *dr, const p_draw_idx *di,
                         const void *rec, uint32_t n, uint64_t stride) {
    g_decoded_count_n = n; g_decoded_count_stride = stride;
    rp_draw(c, dr, di, rec);
    g_decoded_count_n = 0; g_decoded_count_stride = 0;
}
// OP_MULTI_DRAW（超过 1,024 条的一次调用，前端整段录一条）：组大小与步长由记录直接给，不往后数
static uint32_t g_mdi_force_n; static uint64_t g_mdi_force_stride;
void rp_multi_draw(nvn_gpu_cmd c, const p_multi_draw *m, const void *rec) {
    if (!c || !m || m->max <= 0 || m->stride <= 0) { skip("multi_draw_invalid", rec); return; }
    g_mdi_force_n = (uint32_t)m->max; g_mdi_force_stride = (uint64_t)m->stride;
    if (m->itype >= 0) {
        p_draw_idx d; memset(&d, 0, sizeof d);
        d.prim = m->prim; d.itype = m->itype; d.instances = 1; d.ibuf = m->ibuf; d.ind = m->ind; d.mdi_cnt = m->cnt; d.mdi_i = 0;
        rp_draw(c, NULL, &d, rec);
    } else {
        p_draw d; memset(&d, 0, sizeof d);
        d.prim = m->prim; d.instances = 1; d.ind = m->ind; d.mdi_cnt = m->cnt; d.mdi_i = 0;
        rp_draw(c, &d, NULL, rec);
    }
    g_mdi_force_n = 0;
}
// OP_DRAW / OP_DRAW_IDX 共用。di 非空 = 带索引
// 分时：一笔绘制里各步各花多久（累计到 rp_perf，上屏那边每 5 秒报一次）
static uint64_t dt_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
// 分时抽样（#1103）：原先每笔取 10 次时钟 + 9 次原子加（世界稳态每帧约 1430 笔 → 1.4 万次取时钟），占回放线程约 3%，
// 而它只服务每 5 秒一行的「分时」读数。现在每 rp_tm_stride() 帧只完整计 1 帧：采样帧里逐步计时照旧，其余帧一次时钟都不取；
// 「分时」行里这些项按采样帧数求每帧值（rp_present.c）。抽样状态放在本文件：vk 隔离用例只链接 rp_draw.c / rp_state.c，不带 rp_exec.c。
int rp_tm_on;                  // 1 = 当前这一帧要逐步计时（上屏所在线程在帧边界翻，其它线程只读）
static unsigned g_tm_stride;   // 抽样间隔（帧）；0 = 还没读开关
static unsigned g_tm_n;        // 离上一个采样帧过了几帧（上屏所在线程独占）
void rp_tm_reset(void) {       // 会话重开：重读开关。nvk_tm_full=1 压过 nvk_tm_stride
    const int full = nvn_plat_flag("nvk_tm_full", 0) != 0, v = nvn_plat_flag("nvk_tm_stride", 32);
    const unsigned s = full || v < 1 ? 1u : v > 1024 ? 1024u : (unsigned)v;
    g_tm_stride = s; g_tm_n = 0;
    __atomic_store_n(&rp_tm_on, s == 1, __ATOMIC_RELAXED);   // 每帧都计：从头就开着；否则第一个采样帧在第 s 帧
}
unsigned rp_tm_stride(void) { if (!g_tm_stride) rp_tm_reset(); return g_tm_stride; }
void rp_tm_frame_end(void) {   // 每次上屏调一次：刚结束的是采样帧就记一帧，再定下一帧要不要计时
    const int was = rp_tm();   // 先取：第一次调用时下面的 rp_tm_stride() 会读开关并改写它
    if (was) rp_perf_add(RP_PERF_TM_FRAMES, 1);
    const unsigned s = rp_tm_stride();
    g_tm_n = g_tm_n + 1 >= s ? 0 : g_tm_n + 1;
    __atomic_store_n(&rp_tm_on, g_tm_n == 0, __ATOMIC_RELAXED);
}
#define DT(step) do { if (tm_) { uint64_t n_ = dt_now(); rp_perf_add(step, n_ - dt_); dt_ = n_; } } while (0)
static nvn_gpu_pipe_desc replay_pipe_desc(const p_set_rt *rt, uint32_t prim, int quads, int zattr,
                                          int zstream, int clip_neg1, uint32_t color_write_mask,
                                          int preview, const void *rec) {
    nvn_gpu_pipe_desc pd; memset(&pd, 0, sizeof pd);
    for (int st = 0; st < RP_PROG_STAGES; st++) pd.st[st].sh = g_prog->sh[st];
    for (int i = 0; i < rt->n && i < 8; i++) { pd.cfmt[i] = rt->color[i] && (preview ? rp_pass_slot_planned(i) : rp_pass_slot_attached(i)) ? (uint32_t)(rt->has_cview[i] && rt->cview[i].set_format ? rt->cview[i].format : rt->color[i]->d.format) : 0; pd.lmap[i] = (uint8_t)i; }
    if (rt->depth) { nvn_fmt_info fi; uint32_t f = (uint32_t)rt->depth->d.format; int ok = nvn_fmt_info_of(f, &fi); if (!ok || fi.depth) pd.dfmt = f; if (ok && fi.stencil) pd.sfmt = f; }
    const tex_t *c0 = NULL; for (int i = 0; i < rt->n && i < 8 && !c0; i++) c0 = rt->color[i];   // 第一个有图的颜色槽
    pd.samples = c0 ? (uint32_t)c0->d.samples : rt->depth ? (uint32_t)rt->depth->d.samples : 0;
    pd.prim = prim; pd.patch_size = (uint32_t)rp_S.patch_size; pd.prim_restart = (uint8_t)(rp_S.restart.on != 0 && !quads);   // 要展开的图元，重启在展开时已经处理掉
    memcpy(pd.blend, rp_S.blend, sizeof pd.blend); pd.color = rp_S.color; pd.chmask = rp_S.chmask; pd.ms = rp_S.ms;
    // 归一化（对照 nvn_mtl.m pso_key_norm / pso_desc，docs/对照_iOS回放层语义.md 第 1 段）：
    //   · 片元着色器没写的输出分量不落帧缓冲：写掩码与着色器的输出映射相与（硬件只写着色器输出表里的；宿主上没写的输出是未定义值）；
    //   · 一个分量都不写的目标、没有图的槽：不混合；高级混合位只为查询保真，不进管线。
    pd.chmask.mask = color_write_mask;
    for (int i = 0; i < 8; i++) {
        if (!pd.cfmt[i] || !(pd.chmask.mask >> (4 * i) & 0xf)) { pd.chmask.mask &= ~(0xfu << (4 * i)); pd.color.blend_en &= (uint8_t)~(1u << i); memset(&pd.blend[i], 0, sizeof pd.blend[i]); continue; }
        pd.blend[i].adv_mode = 0; pd.blend[i].adv_overlap = 0; pd.blend[i].adv_premul = 0; pd.blend[i].adv_normdst = 0;
        // #1072：目标格式不能混合（G710 的 R32F / RGBA32F / RGBA4 / RGB5A1 等）：开着混合建管线是未定义行为。关混合、按格式具名记账。
        // 整数格式与开了逻辑运算时后端本来就不混合，不管。
        if ((pd.color.blend_en >> i & 1) && pd.color.logic_op == NVN_LOGIC_OP_COPY) {
            nvn_fmt_info cfi; int is_int = nvn_fmt_info_of(pd.cfmt[i], &cfi) && (cfi.kind == NVN_FMT_UINT || cfi.kind == NVN_FMT_SINT);
            if (!is_int && !(rp_fmt_caps(pd.cfmt[i]) & NVN_GPU_FMT_BLEND)) {
                if (!preview) {
                    char nm[40]; snprintf(nm, sizeof nm, "blend_unsupported_fmt_0x%x", (unsigned)pd.cfmt[i]); nvn_unimpl("draw", nm, rec);
                }
                pd.color.blend_en &= (uint8_t)~(1u << i); memset(&pd.blend[i], 0, sizeof pd.blend[i]);
            }
        }
    }
    // 只放着色器真读的属性（attr_mask）和它们用到的流（同旧首遇路径 nvn_vk_first_draw.c 按 attr_mask 取 location）。
    // 游戏设了格式、着色器不读的属性原先也烘进管线：同一程序因这些属性的格式 / 偏移 / 流步长不同就多出管线变体（换管线、首遇编译都跟着涨；
    // 参考 BOTWRecompAndroid #483「管线键不再烘入全部顶点属性」，在用管线降 35%），不读的属性格式在本设备没映射时还会让整条管线建失败。
    uint32_t pstreams = 0;
    for (int i = 0; i < rp_S.nattr && i < 16; i++) if (g_prog->attr_mask >> i & 1) { pd.attr[i] = rp_S.attr[i]; if (pd.attr[i].fmt && pd.attr[i].stream < 16) pstreams |= 1u << pd.attr[i].stream; }
    for (int i = 0; i < rp_S.nstream && i < 16; i++) if (pstreams >> i & 1) pd.stream[i] = rp_S.stream[i];
    if (zattr) {
        for (int i = 0; i < 16; i++) if (zattr >> i & 1) { pd.attr[i].fmt = NVN_FORMAT_RGBA32F; pd.attr[i].offset = 0; pd.attr[i].stream = (uint32_t)zstream; }
        pd.stream[zstream].stride = 0; pd.stream[zstream].divisor = 0;
    }
    pd.ds = rp_S.ds; pd.poly = rp_S.poly; pd.rast_discard = (uint8_t)(rp_S.rast_discard != 0);
    // Ryujinx StateUpdater.UpdateFrontFace 与 EnumConversion 合成后，Vulkan 帧缓冲绕序 = API 值 XOR 队列 flip。
    // nvn_mtl 的同一表达式也是帧缓冲绕序，不能直接当本接口的 NDC 绕序。
    // 后端 C80 会按当前窗口原点转换 NDC → 帧缓冲；这里先消去这次转换，保留建队列时固定的 flip。
    pd.origin_ll = (uint8_t)(rp_origin_lower_left() != 0);
    pd.poly.front = (((rp_S.poly.front == NVN_FRONT_FACE_CCW) != (s2a_nvn_rast_flip != 0)) != (pd.origin_ll != 0)) ? NVN_FRONT_FACE_CCW : NVN_FRONT_FACE_CW;
    pd.depth_clamp = (uint8_t)(rp_S.depth_clamp != 0);
    pd.clip_neg1 = (uint8_t)(g_prog->z_raw && clip_neg1);   // 程序没做 near = -w 的深度改写：交给管线（只在 CAP_DEPTH_CLIP_CTL 时会出这种程序）
    return pd;
}

static int cold_prepare(nvn_gpu_cmd c, const nvn_gpu_pipe_desc *recipe, const nvn_gpu_draw_args *a) {
    nvn_gpu_cmd pre = NULL;
    if (!rp_cold_submit_scope(c, &pre)) return 0;
    const struct rp_cold_proof *proof = g_prog->cold;
    const uint32_t fs = NVN_GPU_SH_FS, ent = proof->fragment_ent;
    if (ent >= g_prog->nent[fs] || ent >= RP_PROG_ENT_MAX || !g_sb_ok[fs][ent]) return 0;
    const nvn_gpu_layout_ent *slot = &g_prog->ent[fs][ent];
    if (slot->src_kind != NVN_RB_SBUFP || slot->src_reg >= 16 || slot->count != 1 ||
        slot->cls != NVN_GPU_RES_SSBO || slot->access != NVN_GPU_ACC_R) return 0;

    nvn_gpu_pipe original = NULL, variant = NULL;
    if (!nvn_gpu_pipe_lookup(recipe, &original)) return 0;
    nvn_gpu_pipe_desc changed = *recipe;
    changed.st[fs].sh = proof->fragment;
    if (nvn_vk_pipe_demand_cold_kick(&changed, g_prog->identity, proof->variant_sha, &variant) != RP_FU_DEMAND_READY) return 0;

    const p_bind_buf *sb = &rp_S.ssbo[fs][slot->src_reg];
    uint64_t wa, wl;
    sb_window(sb, 1, &wa, &wl);
    struct nvn_gpu_cold_domain scan = {
        .buf = g_sb_buf[fs][ent], .fragment_ent = ent, .first_word = proof->first_word,
        .stride_words = proof->stride_words, .count = proof->count,
    };
    if (!nvn_gpu_buf_access_span(scan.buf, g_sb_off[fs][ent], wl, &scan.binding)) return 0;
    nvn_gpu_slice driver = {0};
    if (proof->prefix_mask) {
        // The same slice is consumed by the ordinary FS binding path below.
        if (!alloc_copy(c, DRV0_BYTES, NULL, &driver)) return 0;
        fill_graphics_drv(&driver, fs, 0);
        scan.prefix = (struct nvn_gpu_cold_prefix){
            .buf = driver.buf, .fragment_ent = proof->prefix_ent, .byte = proof->prefix_byte,
            .mask = proof->prefix_mask,
        };
        if (!nvn_gpu_buf_access_span(driver.buf, driver.off, DRV0_BYTES, &scan.prefix.binding)) return 0;
    }
    if (!nvn_vk_cold_guard_auto(c, pre, original, variant, a, &scan)) return 0;
    g_cold_drv = driver;
    return 1;
}

void rp_draw(nvn_gpu_cmd c, const p_draw *dr, const p_draw_idx *di, const void *rec) {
    if (!c) return;
    g_cold_drv.buf = NULL;
    memset(g_ind_drv_ok, 0, sizeof g_ind_drv_ok);
    const int tm_ = rp_tm();   // 采样帧才逐步计时：非采样帧不取时钟、不做原子加
    uint64_t dt_ = tm_ ? dt_now() : 0;
    uint32_t prim = (uint32_t)(di ? di->prim : dr->prim);
    const uint64_t ind = di ? di->ind : dr->ind;
    const uint64_t mcnt = di ? di->mdi_cnt : dr->mdi_cnt;
    uint32_t mdi_n = 0; uint64_t mdi_stride = 0;
    if (mcnt) {
        const int32_t mi = di ? di->mdi_i : dr->mdi_i;
        if (mi != 0) { skip("indirect_count_group_broken", rec); return; }
        if (g_mdi_force_n) { mdi_n = g_mdi_force_n; mdi_stride = g_mdi_force_stride; }
        else if (g_decoded_count_n) { mdi_n = g_decoded_count_n; mdi_stride = g_decoded_count_stride; }
        else { skip("indirect_count_group_unavailable", rec); return; }
        if (mdi_n == 1) mdi_stride = di ? 20 : 16;
        if (!ind || mdi_stride < (uint64_t)(di ? 20 : 16) || (mdi_stride & 3) || mdi_stride > 0xFFFFFFFFu) { skip("indirect_count_group_unavailable", rec); return; }
        nvn_gpu_caps caps; nvn_gpu_caps_get(&caps);   // 设备没开 drawIndirectCount 就不交（交了后端整段判失败），照旧报名跳过
        if (!(caps.caps & NVN_GPU_CAP_INDIRECT_COUNT)) { skip(g_decoded_count_n ? "indirect_count_unsupported" : "indirect_count_no_device_support", rec); return; }
    }
    if (ind && (prim == 2 || prim == 7 || prim == 8 || prim == 9 || (di && di->itype == 0))) {
        skip("indirect_topology_or_index_conversion", rec); return;
    }
    if (dr && dr->tfc) { skip("transform_feedback_count", rec); return; }
    if (rp_S.tf_on) { skip("transform_feedback_active", rec); return; }
    // 0..9 是普通图元，10..13 是四种邻接图元（直接映射 Vulkan 的 *_WITH_ADJACENCY，不展开）；14 曲面细分补丁另说
    if (!(prim <= 13)) { char nm[24]; snprintf(nm, sizeof nm, "prim_%u", prim); skip(nm, rec); return; }
    if (prim >= 10) {   // Vulkan：设备没开 geometryShader 特性时不许用邻接拓扑（VUID-VkPipelineInputAssemblyStateCreateInfo-topology-00429）
        nvn_gpu_caps caps; nvn_gpu_caps_get(&caps);
        if (!(caps.caps & NVN_GPU_CAP_GEOMETRY)) { skip("adjacency_without_geometry_feature", rec); return; }
    }
    // 带 GS 的拓扑（同 Ryujinx 的 Vulkan 后端：原生能收的直接交给 GS，收不了的换成三角形列表）：
    //   三角带按 GL 序展开（GS 的 gl_in 顺序与 Vulkan 原生三角带的奇三角形不同，#696）：不带索引 gs_tstrip_expand、带索引 gs_tstrip_expand_ix（读游戏索引、分段重启）；
    //   间接的三角带：索引 / 参数在 GPU 上，CPU 展开不了，按原生三角带画（Ryujinx 同此，奇三角形 gl_in[0] 与 GL 不同、绕序相同）；
    //   线带 / 三角扇 / 多边形（= 扇）/ 邻接图元：原生交给 GS；线环 / 四边形 / 四边形带：沿用下面的展开（GS 看到展开后的线 / 三角形，同 Ryujinx）。
    const int gs = (rp_shader_graphics_stages() & 4u) != 0;
    uint32_t count = (uint32_t)(di ? di->count : dr->count);
    if (!count && !ind) return;
    if (ind) count = 0;   // GPU parameters, not stale direct fields, own the draw count.
    // 没有原生对应的图元：换成能画的图元号，要展开索引的记下展开方式（quads），索引在取资源那一步展开
    int restart = di && rp_S.restart.on;
    int quads = EXP_NONE;
    if (prim == 7) quads = EXP_QUADS;
    else if (prim == 2) quads = EXP_LOOP;
    else if (prim == 5 && gs && !ind) quads = EXP_GS_TSTRIP;   // 带 GS 的三角带按 GL 序展开
    else if (prim == 8) {           // 四边形带 = 三角带；零头 1 个点在三角带里会多画一个三角形，丢掉
        if (restart) quads = EXP_QSTRIP;
        else { count &= ~1u; if (count < 4) return; }
    }
    prim = rp_draw_shader_prim(prim, gs, restart, ind != 0);
    const p_set_rt *rt = rp_pass_target();
    if (!rt) { skip("draw_without_target", rec); return; }
    // 程序：管线状态或任一阶段的绑定变过才重新取（贴图槽里换了不同类型的贴图会换变体）
    // #977：带索引的条带（线带 / 三角带 / 扇，含多边形）开着重启、重启值按索引宽度截断后不是全 1：直接绘制改写索引，间接绘制具名不画。
    uint32_t rst_rv = 0; int rst_rewrite = 0;
    if (restart && !quads && (prim == 3 || prim == 5 || prim == 6) && di->itype >= 0 && di->itype <= 2) {
        const uint32_t ones = di->itype == 0 ? 0xFFu : di->itype == 1 ? 0xFFFFu : 0xFFFFFFFFu;
        rst_rv = (uint32_t)rp_S.restart.index & ones;
        rst_rewrite = rst_rv != ones;
    }
    if (rst_rewrite && ind) { skip("indirect_restart_index_not_all_ones", rec); return; }
    if (prim != g_prim || quads != g_quads) { g_prim = prim; g_quads = quads; rp_pso_dirty = 1; }
    // 窗口原点、光栅翻转和近裁模式都在状态块之外。近裁模式还会改变着色器变体，须在程序复用前检查。
    const int clip_neg1 = !rp_depth_zero();
    const int raster = (rp_origin_lower_left() != 0) | ((s2a_nvn_rast_flip != 0) << 1) | (clip_neg1 << 2);
    if (raster != g_raster) { g_raster = raster; rp_pso_dirty = 1; }
    int args_dirty = rp_args_dirty[0] || rp_args_dirty[1] || rp_args_dirty[2];
    int lib_retry = 0;   // 出库版绑定核对失败只回退现编一次：现编版再失败照旧具名跳过
    if (!g_prog || rp_pso_dirty || args_dirty) {
        struct rp_shader_req q; memset(&q, 0, sizeof q); q.prim = prim; q.rt_count = rt->n;
        for (int i = 0; i < rt->n && i < 8; i++) if (rt->color[i]) q.rt_format[i] = (uint32_t)(rt->has_cview[i] && rt->cview[i].set_format ? rt->cview[i].format : rt->color[i]->d.format);
        q.compile_defer_frame = rp_live_compile_defer_frame;
        const struct rp_prog *np = NULL;
        int sr = rp_shader_resolve(&q, &np);
        if (sr) { skip(sr > 0 ? "program_not_ready" : "program_unresolved", rec); g_prog = NULL; return; }
        if (np != g_prog) { g_prog = np; rp_pso_dirty = 1; }
    }
draw_rebind:   // 出库回退落地：g_prog 已换现编版（pso_dirty 已置，下面按新程序重取资源与管线）
    // 仅拒绝绑定 DRV0 且未证明不读 base instance 的已解码 count 组；单条间接/#941 和 OP_MULTI_DRAW 保持原路径。
    // 组大小 1（max_draws = 1）只有一条命令，没有「别的命令的 firstInstance 被第一条顶替」的问题，语义与单条间接绘制相同：
    //   不跳，走下面单条间接的那次 4 字节拷贝（firstInstance 取这一条记录的）。组大小 ≥ 2 照旧具名跳（#1787 第二处）。
    if (g_decoded_count_n > 1) {
        for (int st = 0; st < RP_PROG_STAGES; st++)
            for (uint32_t i = 0; i < g_prog->nent[st]; i++)
                if (g_prog->ent[st][i].src_kind == NVN_RB_DRV0 && !(g_prog->binst_unread >> st & 1)) {
                    skip("indirect_count_driver_base_instance", rec); return;
                }
    }
    DT(RP_PERF_D_PROG);
    // 先把这笔要用的资源都取好：池镜像同步、贴图上传是传输，不能出现在通道里，必须在开通道之前做完
    nvn_gpu_buf vb_buf[16]; uint64_t vb_off[16]; uint32_t streams = 0;
    // #977：着色器读了但游戏没设格式的属性（属性表外 / 格式 0）不取它的流；由核心补一条 RGBA32F、步长 0、内容全 0 的属性
    // （编译端把这种输入按 Float 声明；同 iOS ZERO_ATTR、Ryujinx IsZero → 零缓冲），绑到一条没被有格式属性占用的流上。
    uint32_t zattr = 0, zused = 0; int zstream = -1;
    for (int i = 0; i < 16; i++) {
        const int has = i < rp_S.nattr && rp_S.attr[i].fmt;
        if (has) zused |= 1u << rp_S.attr[i].stream;   // 有格式的属性都会在后端占一条绑定（不论着色器读不读）
        if (g_prog->attr_mask >> i & 1) { if (has) streams |= 1u << rp_S.attr[i].stream; else zattr |= 1u << i; }
    }
    if (zattr) {
        for (int s = 0; s < 16 && zstream < 0; s++) if (!(zused >> s & 1)) zstream = s;
        if (zstream < 0) { skip("zero_attr_no_free_stream", rec); return; }
    }
    {   // 一次抢锁解析全部顶点流（逐段各抢一次时这把锁和完成线程互抢，重叠档能吃到 13.7–16.6 毫秒/帧）
        uint64_t vaddr[16], vlen[16], voff[16]; unsigned char vok[16]; int nv = 0;
        for (int i = 0; i < 16; i++) if (streams >> i & 1) {
            uint64_t va = nvn_ga2ca(rp_S.vbuf[i].address), vn = rp_S.vbuf[i].size;   // 记录里的顶点缓冲地址是 GPU 地址
            if (!va || !vn) { skip("vertex_stream_unbound", rec); return; }
            vaddr[nv] = va; vlen[nv] = vn; nv++;
        }
        if (nv) {
            nvn_gpu_buf vbuf[16];
            rp_buf_resolve_multi(nv, vaddr, vlen, vbuf, voff, vok);
            int k = 0;
            for (int i = 0; i < 16; i++) if (streams >> i & 1) {
                if (!vok[k]) { skip("vertex_buffer_unresolved", rec); return; }
                vb_buf[i] = vbuf[k]; vb_off[i] = voff[k]; k++;
            }
        }
    }
    // 存储缓冲同样要在开通道之前同步：同一次提交里第二次同步这块池页时要录成传输、结束通道（rp_xfer_cmd），
    // 放到发绑定时才做，通道会在设了管线之后被结束，这一笔的绑定与绘制落在通道外，后端置粘滞、整段作废
    // （#697 Mali 真机：「bind 在通道外（非计算管线）」）。这里先同步一遍，发绑定时同一段已同步过，走快路径、不再结束通道。
    // 解析失败不在这里报：发绑定时同样的解析会按原来的名字记未接。
    // 结果留给发绑定直接用（g_sb_pre），发绑定时不再解析：即使这块页在开通道之后又被别的游戏线程写脏，也不会在通道里再同步一次。
    memset(g_sb_ok, 0, sizeof g_sb_ok);
    {   // 一次抢锁把这一笔的存储缓冲全解析掉（同顶点流：逐段各抢一次锁会和完成线程互抢）
        enum { SB_BATCH = 64 };
        uint64_t ba[SB_BATCH], bl[SB_BATCH], bo[SB_BATCH]; nvn_gpu_buf bb[SB_BATCH]; unsigned char bok[SB_BATCH];
        int bs[SB_BATCH], bi[SB_BATCH], nb = 0;
        for (int st = 0; st < RP_PROG_STAGES; st++) for (uint32_t i = 0; i < g_prog->nent[st] && i < RP_PROG_ENT_MAX; i++) {
            const nvn_gpu_layout_ent *e = &g_prog->ent[st][i];
            if (e->src_kind != NVN_RB_SBUFP && e->src_kind != NVN_RB_UBOP) continue;
            const p_bind_buf *sb = e->src_reg < 16 ? (e->src_kind == NVN_RB_UBOP ? &rp_S.ubo[st][e->src_reg] : &rp_S.ssbo[st][e->src_reg]) : NULL;
            if (!sb || !sb_len(sb, e->src_kind == NVN_RB_SBUFP)) continue;
            uint64_t wa, wl; (void)sb_window(sb, e->src_kind == NVN_RB_SBUFP, &wa, &wl);   // 与发绑定时同一个窗口（被夹短的记账在发绑定那边）
            if (nb < SB_BATCH) { bs[nb] = st; bi[nb] = (int)i; ba[nb] = wa; bl[nb] = wl; nb++; }
            else if (rp_buf_resolve(c, wa, wl, &g_sb_buf[st][i], &g_sb_off[st][i])) g_sb_ok[st][i] = 1;   // 超出一批的照旧逐段解析
        }
        if (nb) {
            rp_buf_resolve_multi(nb, ba, bl, bb, bo, bok);
            for (int k = 0; k < nb; k++) if (bok[k]) { g_sb_buf[bs[k]][bi[k]] = bb[k]; g_sb_off[bs[k]][bi[k]] = bo[k]; g_sb_ok[bs[k]][bi[k]] = 1; }
        }
    }
    // 常量缓冲（NVN_RB_CBUF）的 GPU 待写回镜像同理，而且这条比存储缓冲更要命：
    // 在发绑定阶段才发现「有 GPU 待写回」再解析，会同步池页；同一次提交里第二次同步要录成传输（rp_xfer_cmd），
    // 那个函数会结束当前通道——通道一结束，这一笔后面的绑定与绘制就落在通道外，后端置首错
    // 「bind 在通道外（非计算管线）」→ 整段提交失败 → 时间线失败终态（真机表现：起局整屏黑，每帧 2 笔重复帧）。
    // 这里的解析发生在开通道之前，绑定阶段只消费结果（不解析、不传输、不切通道）。
    memset(g_ub_ok, 0, sizeof g_ub_ok);
    for (int st = 0; st < RP_PROG_STAGES; st++) for (uint32_t i = 0; i < g_prog->nent[st] && i < RP_PROG_ENT_MAX; i++) {
        const nvn_gpu_layout_ent *e = &g_prog->ent[st][i];
        if (e->src_kind != NVN_RB_CBUF || e->src_reg >= 16) continue;
        const p_bind_buf *ub = &rp_S.ubo[st][e->src_reg];
        if (!ub->addr || !ub->size) continue;
        const uint64_t len = cbuf_len(ub);   // 与发绑定同一口径（#2125：声明 64 KiB，超出按它截短）
        const nvn_gpu_caps *cp = caps_cached();
        if (cp->max_ubo_range && len > cp->max_ubo_range) continue;   // 这一笔在 bind_stage_at 开头会被具名跳过，不用预解析
        if (!rp_buf_gpu_pending(ub->addr, len)) continue;   // 没有 GPU 待写回：照旧暂存，不需要镜像
        nvn_gpu_buf mb; uint64_t mo;
        if (rp_buf_resolve(c, ub->addr, len, &mb, &mo)) { g_ub_buf[st][i] = mb; g_ub_off[st][i] = mo; g_ub_ok[st][i] = 1; }
    }
    nvn_gpu_draw_args a; memset(&a, 0, sizeof a);
    uint64_t indirect_bytes = 0, index_bytes = 0;
    if (di && (di->itype < 0 || di->itype > 2)) { skip("index_type_invalid", rec); return; }
    if (ind) {
        const uint64_t isize = di ? 20 : 16, ispan = mdi_n ? (uint64_t)(mdi_n - 1) * mdi_stride + isize : isize;
        if (!rp_buf_resolve(c, ind, ispan, &a.ind, &a.ind_off) || (a.ind_off & 3)) {
            skip("indirect_buffer_unresolved", rec); return;
        }
        indirect_bytes = ispan;
        a.max_draws = 1; a.stride = (uint32_t)isize;
        if (mdi_n) {   // 整组：GPU 读计数，画 min(计数, mdi_n) 条
            if (!rp_buf_resolve(c, mcnt, 4, &a.cnt, &a.cnt_off) || (a.cnt_off & 3)) { skip("indirect_count_unresolved", rec); return; }
            a.max_draws = mdi_n; a.stride = (uint32_t)mdi_stride;
        }
        if (di) {
            const uint8_t *mem = NULL; uint64_t bytes = 0;
            const uint64_t isz = di->itype == 1 ? 2 : 4;
            // The GPU owns firstIndex/indexCount. Resolve the registered binding tail,
            // not a CPU-derived count, without reading index or argument bytes.
            if (!rp_pool_find(di->ibuf, isz, &mem, &bytes)) { skip("index_buffer_unresolved", rec); return; }
            uint64_t off = di->ibuf - (uint64_t)(uintptr_t)mem;
            if (off > bytes || !rp_buf_resolve(c, di->ibuf, bytes - off, &a.ibuf, &a.ioff) || (a.ioff & (isz - 1))) {
                skip("index_buffer_unresolved", rec); return;
            }
            a.itype = (uint32_t)di->itype; index_bytes = bytes - off;
        }
    } else if (quads) {
        uint32_t isz = di ? (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4) : 0;
        if (quads == EXP_QUADS && count / 4 == 0) return;   // 不足一个四边形：什么都不画
        if (quads == EXP_GS_TSTRIP && count < 3) return;    // 不足一个三角形：什么都不画
        // 带索引：游戏的索引在登记过的池里，编码这一刻读出来展开（同常量缓冲：就是此刻的内容）
        if (di && !rp_pool_find(di->ibuf, (uint64_t)count * isz, NULL, NULL)) { skip("index_buffer_unresolved", rec); return; }
        uint64_t cap = quads == EXP_QUADS ? (uint64_t)(count / 4) * 6 : quads == EXP_LOOP ? (uint64_t)count * 2 :
                       quads == EXP_GS_TSTRIP ? (uint64_t)(count - 2) * 3 : (uint64_t)count * 3;   // 展开后最多几个索引
        nvn_gpu_slice sl; nvn_gpu_err err = {0};
        nvn_gpu_rc rc = nvn_gpu_arena_alloc(c, cap * 4, 4, &sl, &err);
        if (!nvn_gpu_ok(rc) || !sl.cpu) { arena_fail_detail(rc, &err, cap * 4); skip("arena_alloc_failed", rec); return; }
        const uint8_t *src = di ? (const uint8_t *)(uintptr_t)di->ibuf : NULL; int64_t rv = restart ? (int64_t)rp_S.restart.index : -1; uint32_t *dst = (uint32_t *)(void *)sl.cpu;
        uint32_t n = quads == EXP_QUADS ? quad_expand(src, isz, count, rv, dst) : quads == EXP_LOOP ? loop_expand(src, isz, count, rv, dst) :
                     quads == EXP_GS_TSTRIP ? (src ? gs_tstrip_expand_ix(src, isz, count, rv, dst) : gs_tstrip_expand(count, dst)) : qstrip_expand(src, isz, count, rv, dst);
        if (!n) return;
        a.ibuf = sl.buf; a.ioff = sl.off; a.itype = 2; a.base_vertex = di ? di->base_vertex : dr->first; count = n;
    } else if (rst_rewrite) {
        uint32_t isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4;
        if (!rp_pool_find(di->ibuf, (uint64_t)count * isz, NULL, NULL)) { skip("index_buffer_unresolved", rec); return; }
        nvn_gpu_slice sl; nvn_gpu_err err = {0};
        nvn_gpu_rc rc = nvn_gpu_arena_alloc(c, (uint64_t)count * 4, 4, &sl, &err);
        if (!nvn_gpu_ok(rc) || !sl.cpu) { arena_fail_detail(rc, &err, (uint64_t)count * 4); skip("arena_alloc_failed", rec); return; }
        count = restart_rewrite((const uint8_t *)(uintptr_t)di->ibuf, isz, count, rst_rv, (uint32_t *)(void *)sl.cpu);
        a.ibuf = sl.buf; a.ioff = sl.off; a.itype = 2; a.base_vertex = di->base_vertex;
    } else if (di) {
        uint32_t isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4;
        if (!rp_buf_resolve(c, di->ibuf, (uint64_t)count * isz, &a.ibuf, &a.ioff)) { skip("index_buffer_unresolved", rec); return; }
        a.itype = (uint32_t)di->itype; a.base_vertex = di->base_vertex; index_bytes = (uint64_t)count * isz;
    } else a.first = (uint32_t)dr->first;
    if (ind || g_hz_mask) {
        // 要拷的阶段：用驱动段、编译器没证明不读 c0 + 0x644
        uint8_t need = 0;
        if (ind) for (int st = 0; st < RP_PROG_STAGES; st++) for (uint32_t i = 0; i < g_prog->nent[st]; i++)
            if (g_prog->ent[st][i].src_kind == NVN_RB_DRV0 && !(g_prog->binst_unread >> st & 1)) need |= (uint8_t)(1u << st);
        const int hz = hz_on() && rec && rec == rp_draw_stream && !mdi_n;
        if (hz) {
            g_hz_mask |= need;
            // 通道还没开（这一笔会开它）：开之前前瞻一遍，本通道里后面的间接绘制一起拷
            int built = 0;
            if (rp_pass_is_open && !rp_pass_is_open() && g_hz_mask) { hz_build(c, rec, g_hz_mask); built = 1; }
            if (need) {
                struct hz_ent *e = hz_take(c, rec, ind, di != NULL, a.ind, a.ind_off, need);
                if (!e && !built) {   // 表里没有（前瞻停在前面 / 作废了 / 阶段不够）：收通道，从这一笔重新前瞻；刚建过还没有就走下面的逐笔拷
                    if (!rp_pass_is_open || rp_pass_is_open()) rp_perf_add(RP_PERF_BINST_CUT, 1);
                    rp_pass_flush(c); hz_build(c, rec, g_hz_mask);
                    e = hz_take(c, rec, ind, di != NULL, a.ind, a.ind_off, need);
                    if (e) rp_perf_add(RP_PERF_BINST_ONE, 1);
                } else if (e) rp_perf_add(RP_PERF_BINST_HZ, 1);
                if (e) for (int st = 0; st < RP_PROG_STAGES; st++) if (need >> st & 1) {
                    g_ind_drv[st] = e->sl[st]; fill_graphics_drv(&g_ind_drv[st], st, 0);
                    g_ind_drv_ok[st] = 1; g_lb[st].ok = 0;
                }
            }
        }
    }
    if (ind) {
        int copied = 0;
        for (int st = 0; st < RP_PROG_STAGES; st++) for (uint32_t i = 0; i < g_prog->nent[st]; i++) {
            if (g_prog->ent[st][i].src_kind != NVN_RB_DRV0 || g_ind_drv_ok[st]) continue;
            // 编译器证明这一阶段不读 c0 + 0x644（cbdp 真时顶点阶段对它的读已换成内建 BaseInstance，间接绘制的 firstInstance 直接生效）：
            //   不拷、不为拷贝收通道，驱动段走普通内容缓存（base_instance 那个字没人读）。未知 / 读的照旧拷。
            if (g_prog->binst_unread >> st & 1) continue;
            if (!alloc_copy(c, DRV0_BYTES, NULL, &g_ind_drv[st])) { skip("arena_alloc_failed", rec); return; }
            fill_graphics_drv(&g_ind_drv[st], st, 0);
            // Stay in the main command stream after any GPU producer; a pre-submit
            // transfer command would move this read before that producer.
            if (!copied) { rp_perf_add(RP_PERF_BINST_ONE, 1); if (!rp_pass_is_open || rp_pass_is_open()) rp_perf_add(RP_PERF_BINST_CUT, 1); rp_pass_flush(c); }
            nvn_gpu_copy_buf(c, a.ind, a.ind_off + (di ? 16 : 12),
                             g_ind_drv[st].buf, g_ind_drv[st].off + 0x644, 4);
            g_ind_drv_ok[st] = 1; g_lb[st].ok = 0; copied = 1;
        }
        if (copied) nvn_gpu_barrier(c, NVN_BARRIER_ORDER_INDIRECT_DATA_BIT | NVN_BARRIER_INVALIDATE_SHADER_BIT);   // transfer writes visible to driver uniform reads
    }
    static nvn_gpu_view tv[RP_PROG_STAGES][RP_PROG_ENT_MAX]; static nvn_gpu_samp ts[RP_PROG_STAGES][RP_PROG_ENT_MAX];
    memset(tv, 0, sizeof tv); memset(ts, 0, sizeof ts); memset(g_gimg, 0, sizeof g_gimg);
    char tl[520]; size_t tn = 0; tl[0] = 0;   // 逐笔日志用：这笔采样的贴图
    int narr[RP_PROG_STAGES] = {0};
    DT(RP_PERF_D_PREP);
    rp_view_pin_begin();
    for (int st = 0; st < RP_PROG_STAGES; st++) for (uint32_t i = 0; i < g_prog->nent[st]; i++) {
        const nvn_gpu_layout_ent *e = &g_prog->ent[st][i];
        if (e->src_kind == NVN_RB_TBUF) {
            if (!tbuf_prepare(c, e, st, g_prog->dtt[st][i], g_prog->dcls[st][i], &tv[st][i], rec)) goto draw_done;
            continue;
        }
        if (e->src_kind == NVN_RB_TEXU) {   // 句柄在用户常量缓冲里：读句柄，取视图 / 采样器走与 NVN_RB_TEX 同一组函数
            if (e->count != 1 || e->src_reg2 >= g_prog->nusrc) { skip("uniform_handle_shape", rec); goto draw_done; }
            int tid = 0, sid = 0; const char *bad = usrc_ids(st, &g_prog->usrc[e->src_reg2], &tid, &sid);
            if (bad) { skip(bad, rec); goto draw_done; }
            int unb = 0;
            tv[st][i] = sampled_view(g_prog, lib_retry, &unb, 1, tid, g_prog->dtt[st][i], g_prog->dcls[st][i], 0, c, NULL);
            if (unb) {
                const struct rp_prog *np = lib_fallback_graphics(g_prog, &lib_retry, prim, rt);
                if (np) { g_prog = np; rp_pso_dirty = 1; goto draw_rebind; }
                if (unb == RP_DECL_NUMERIC_MISMATCH) { skip("sampled_numeric_fallback_failed", rec); goto draw_done; }
                tv[st][i] = rp_view_by_ubo_handle_decl(tid, g_prog->dtt[st][i], g_prog->dcls[st][i], c, NULL);
            }   // 没登记的号绑零贴图（Ryujinx null 贴图），不跳整笔
            // 立即数采样器池下标（#1594 片 3）：号没登记 = Ryujinx 的空采样器，绑默认采样器、整笔照画（片 6，不拿别的已登记采样器顶替）；
            //   登记了但后端建不出的仍具名跳过。samp_pool = 0（离线库登记号）口径不变
            const int const_samp = g_prog->usrc[e->src_reg2].samp_pool;
            ts[st][i] = const_samp ? rp_samp_for_const_handle(sid, tid, rp_S.samp_base, rp_S.samp_pool_n) : rp_samp_for_tex(sid, tid);
            if (!ts[st][i] && const_samp) { skip("const_sampler_unresolved", rec); goto draw_done; }
            if (g_log_left && tn < sizeof tl - 40) tn += (size_t)snprintf(tl + tn, sizeof tl - tn, " u%d.%u+%#x=#%d/s%d", st, (unsigned)e->src_reg, (unsigned)e->src_off, tid, sid);
            continue;
        }
        if (e->src_kind == NVN_RB_TEXP || e->src_kind == NVN_RB_SAMPP) {   // 整池数组：池快照（取法见 pool_table_resolve，计算派发共用）
            const char *why = pool_table_resolve(c, g_prog, st, i, pool_row(st));
            if (why) { skip(why, rec); goto draw_done; }
            continue;
        }
        if (e->src_kind == NVN_RB_IMG && e->cls == NVN_GPU_RES_IMAGE_TBUF) {   // 图形阶段 imageBuffer：缓冲视图（见 image_tbuf_resolve）
            const char *why = image_tbuf_resolve(c, g_prog, st, i, st, &tv[st][i]);
            if (why) { skip(why, rec); goto draw_done; }
            continue;
        }
        if (e->src_kind == NVN_RB_IMG) {   // 图形阶段存储图像：与计算派发同一解析（image_resolve），句柄取这一阶段 BindImage 的槽
            const char *why = image_resolve(c, g_prog, st, i, st, &tv[st][i], &g_gimg_snap[st][i]);
            if (why) {
                const struct rp_prog *np = lib_fallback_graphics(g_prog, &lib_retry, prim, rt);   // 出库版声明与实绑不符：身份标坏换现编版，同一笔重绑
                if (np) { g_prog = np; rp_pso_dirty = 1; goto draw_rebind; }
                skip(why, rec); goto draw_done;
            }
            g_gimg[st][i] = &g_gimg_snap[st][i];
            continue;
        }
        if (e->src_kind != NVN_RB_TEX) continue;
        if (e->src_reg >= RP_NTEX || (e->src_reg2 != UINT16_MAX && e->count != 1) || !samp2_shape_ok(e->src_reg2)) {
            skip("separate_texture_shape", rec); goto draw_done;
        }
        if (e->count > 1) {
            if (e->count > RP_TEXARR_MAX || narr[st]++) { skip("texture_array_unsupported_shape", rec); goto draw_done; }
            for (uint32_t k = 0; k < e->count; k++) {
                int slot = (int)(e->src_reg + k) - RP_HANDLE_TABLE_FIRST_SLOT;
                uint64_t hk = slot >= 0 && slot < 32 ? rp_S.tex[st][slot] : 0;
                int unb = 0;
                g_av[st][k] = sampled_view(g_prog, lib_retry, &unb, 0, hk ? (int)(hk & 0xFFFFF) : rp_S.tex_base, g_prog->dtt[st][i], g_prog->dcls[st][i], 1, c, NULL);
                if (unb) {
                    const struct rp_prog *np = lib_fallback_graphics(g_prog, &lib_retry, prim, rt);
                    if (np) { g_prog = np; rp_pso_dirty = 1; goto draw_rebind; }
                    g_av[st][k] = rp_view_by_id_decl(hk ? (int)(hk & 0xFFFFF) : rp_S.tex_base, g_prog->dtt[st][i], g_prog->dcls[st][i], 1, c, NULL);
                }
                g_as[st][k] = rp_samp_for_tex(hk ? (int)(hk >> 20) : rp_S.samp_base, hk ? (int)(hk & 0xFFFFF) : rp_S.tex_base);
                if (hk && !g_as[st][k]) { skip("sampler_slot_unresolved", rec); goto draw_done; }
            }
            // 已知声明的空元素直接取同类型零贴图；未知声明沿用首个有效视图。采样器仍共用首个有效项。
            { nvn_gpu_view fv = NULL; nvn_gpu_samp fs = NULL;
              for (uint32_t k = 0; k < e->count && !(fv && fs); k++) { if (!fv) fv = g_av[st][k]; if (!fs) fs = g_as[st][k]; }
              for (uint32_t k = 0; k < e->count; k++) { if (!g_av[st][k] && !g_prog->dtt[st][i]) g_av[st][k] = fv; if (!g_as[st][k]) g_as[st][k] = fs; } }
            continue;
        }
        uint64_t h = graphics_tex_handle(st, e->src_reg);
        tex_t tsnap, *tt = g_log_left ? &tsnap : NULL;   // 登记快照只给逐笔日志用：日志关时不拷
        { int unb = 0;
          tv[st][i] = sampled_view(g_prog, lib_retry, &unb, 0, h ? (int)(h & 0xFFFFF) : rp_S.tex_base, g_prog->dtt[st][i], g_prog->dcls[st][i], 0, c, tt);
          if (unb) {
              const struct rp_prog *np = lib_fallback_graphics(g_prog, &lib_retry, prim, rt);
              if (np) { g_prog = np; rp_pso_dirty = 1; goto draw_rebind; }
              if (unb == RP_DECL_NUMERIC_MISMATCH) { skip("sampled_numeric_fallback_failed", rec); goto draw_done; }
              tv[st][i] = rp_view_by_id_decl(h ? (int)(h & 0xFFFFF) : rp_S.tex_base, g_prog->dtt[st][i], g_prog->dcls[st][i], 0, c, tt);
          } }
        if (tt && tn < sizeof tl - 56) {
            int id = h ? (int)(h & 0xFFFFF) : rp_S.tex_base;
            if (tt->serial) {
                uint32_t pk[2] = {0, 0}; char pks[24];
                if (!tt->mem || tt->L.size < 8 || dbg_peek(tt->mem, pk, sizeof pk)) snprintf(pks, sizeof pks, "%08x%08x", (unsigned)pk[0], (unsigned)pk[1]);
                else snprintf(pks, sizeof pks, "读不到");   // 无读权限单独标出，别当成全 0
                tn += (size_t)snprintf(tl + tn, sizeof tl - tn, " t%d.%u=#%llu(%#x %dx%d 态%x 存%s 标%#x 池%#x 包%d)", st, (unsigned)e->src_reg, (unsigned long long)tt->serial, (unsigned)tt->d.format, tt->d.width, tt->d.height, (unsigned)rp_tex_debug_state(tt->serial),
                                       pks, (unsigned)tt->d.flags, tt->pool ? (unsigned)tt->pool->flags : 0xffffu, (int)tt->has_layout);
            } else tn += (size_t)snprintf(tl + tn, sizeof tl - tn, " t%d.%u=#%d(无)", st, (unsigned)e->src_reg, id);
        }
        if (e->src_reg2 != UINT16_MAX && (e->src_reg2 & NVN_GPU_SAMP2_CONST)) {
            // 类型 3 句柄（#1594）：采样器号 = 贴图字高 12 位 | 着色器里的立即数池下标（const_samp_id，同 Ryujinx ReadPackedId），
            //   硬件按当前绑定的采样器池取——登记号 = 采样器号 + 池起点（号 0 取池第 0 项）。不经 NVN 采样器槽。
            //   号没登记 = Ryujinx 的空采样器：绑默认采样器、整笔照画（片 6），不拿别的已登记采样器顶替；登记了但后端建不出的仍具名跳过。
            ts[st][i] = rp_samp_for_const_handle(const_samp_id((uint32_t)h, e->src_reg2 & NVN_GPU_SAMP2_ID_MASK) + rp_S.samp_base, h ? (int)(h & 0xFFFFF) : rp_S.tex_base,
                                                 rp_S.samp_base, rp_S.samp_pool_n);
            if (!ts[st][i]) { skip("const_sampler_unresolved", rec); goto draw_done; }
        } else if (e->src_reg2 != UINT16_MAX) {
            if (!rp_S.samp_bound[st][e->src_reg2]) { skip("separate_sampler_unbound", rec); goto draw_done; }
            uint64_t sh = rp_S.samp[st][e->src_reg2];
            ts[st][i] = rp_samp_for_tex((int)(sh >> 20), h ? (int)(h & 0xFFFFF) : rp_S.tex_base);
            if (!ts[st][i]) { skip("separate_sampler_unresolved", rec); goto draw_done; }
        } else ts[st][i] = rp_samp_for_tex(h ? (int)(h >> 20) : rp_S.samp_base, h ? (int)(h & 0xFFFFF) : rp_S.tex_base);
    }
    DT(RP_PERF_D_TEX);
    // 按这一笔真正取到的视图和池快照落清除，避免再按槽号推断采样来源。挂起表为空时不收集。
    if (rp_pass_parked()) {
        // 每阶段至多有一组定长数组，每个其余布局项至多一个视图或池快照。
        static rp_pass_read rd[RP_PROG_STAGES * (RP_PROG_ENT_MAX + RP_TEXARR_MAX)];
        int nr = 0;
        for (int st = 0; st < RP_PROG_STAGES; st++)
            for (uint32_t i = 0; i < g_prog->nent[st]; i++) {
                const nvn_gpu_layout_ent *e = &g_prog->ent[st][i];
                if (e->src_kind == NVN_RB_TEXP) {
                    rd[nr++] = (rp_pass_read){.pool = g_ptd[st][i]};
                    continue;
                }
                if (e->src_kind == NVN_RB_TEX && e->count > 1) {
                    for (uint32_t k = 0; k < e->count; k++) {
                        nvn_gpu_tex tex = nvn_gpu_view_texture(g_av[st][k]);
                        if (tex) rd[nr++] = (rp_pass_read){.tex = tex};
                    }
                } else if (tv[st][i]) {
                    nvn_gpu_tex tex = nvn_gpu_view_texture(tv[st][i]);
                    if (tex) rd[nr++] = (rp_pass_read){.tex = tex};
                }
            }
        rp_pass_land_reads(c, rd, nr);
    }
    if (g_compute_access.n) {
        int need = 0;
        // 只碰本笔真正读取的绑定：不按派发次数、通道次数或某个着色器键加屏障。
#define ACCESS_READ(value) do { int ar_ = (value); if (ar_ < 0) { skip("graphics_read_span_unresolved", rec); goto draw_done; } need |= ar_; } while (0)
        ACCESS_READ(compute_read_buf(a.ind, a.ind_off, indirect_bytes));
        ACCESS_READ(compute_read_buf(a.cnt, a.cnt_off, a.cnt ? 4 : 0));
        ACCESS_READ(compute_read_buf(a.ibuf, a.ioff, index_bytes));
        for (int i = 0; i < 16 && !need; i++) if (streams >> i & 1) ACCESS_READ(compute_read_buf(vb_buf[i], vb_off[i], rp_S.vbuf[i].size));
        for (int st = 0; st < RP_PROG_STAGES && !need; st++) for (uint32_t i = 0; i < g_prog->nent[st] && !need; i++) {
            const nvn_gpu_layout_ent *e = &g_prog->ent[st][i];
            if (!(e->access & NVN_GPU_ACC_R)) continue;
            if (e->src_kind == NVN_RB_SBUFP || e->src_kind == NVN_RB_UBOP) {
                const p_bind_buf *sb = e->src_reg < 16 ? (e->src_kind == NVN_RB_UBOP ? &rp_S.ubo[st][e->src_reg] : &rp_S.ssbo[st][e->src_reg]) : NULL;
                if (g_sb_ok[st][i] && sb) { uint64_t wa, wl; (void)sb_window(sb, e->src_kind == NVN_RB_SBUFP, &wa, &wl); ACCESS_READ(compute_read_buf(g_sb_buf[st][i], g_sb_off[st][i], wl)); }
                else if (sb && sb_unmapped(sb) && e->src_kind == NVN_RB_SBUFP) ACCESS_READ(compute_read_buf(rp_buf_for_null_storage(), 0, 0x10000));
            } else if (e->src_kind == NVN_RB_TEX || e->src_kind == NVN_RB_TEXU || texel_tmp(e)) {
                if (e->count > 1) { for (uint32_t k = 0; k < e->count && !need; k++) ACCESS_READ(compute_read_view(g_av[st][k])); }
                else ACCESS_READ(compute_read_view(tv[st][i]));
            }
        }
#undef ACCESS_READ
        if (need) {
            rp_pass_flush(c);
            uint32_t game_barrier = rp_pass_take_barrier();
            if (game_barrier) nvn_gpu_barrier(c, game_barrier);
            else nvn_gpu_compute_to_graphics(c);
            g_compute_access.n = 0;   // 同一发布覆盖此前全部计算写；重复读不会再加屏障
        }
    }
    uint32_t color_write_mask = rp_S.chmask.mask & g_prog->color_out_mask;
    { static int fm = -1; if (fm < 0) fm = nvn_plat_flag("nvk_fullmask", 0); if (fm) color_write_mask = 0xffffffffu; }   // 既有诊断：管线与附件选择必须用同一掩码
    rp_pass_want_layered(c, g_prog->layer_out);   // 写层号的程序按分层通道画（八方旅人 32x32x32 调色 LUT：每个实例写一层）
    rp_pass_prepare_draw(c, color_write_mask);   // 有效写掩码与像素区域共同决定这一笔的通道
    int cold_active = 0;
    if (g_prog->cold && dr && !di && !ind && !quads && !a.ibuf && count &&
        !dr->base_instance && (dr->instances == 0 || dr->instances == 1) &&
        nvn_gpu_pipe_lookup && nvn_vk_pipe_demand_cold_kick && nvn_vk_cold_guard_auto &&
        nvn_vk_cold_guard_draw && rp_cold_submit_scope && rp_pass_slot_planned &&
        rp_pass_is_open && !rp_pass_is_open()) {
        nvn_gpu_draw_args direct = a;
        direct.prim = prim; direct.count = count; direct.instances = 1;
        const nvn_gpu_pipe_desc recipe = replay_pipe_desc(rt, prim, quads, zattr, zstream, clip_neg1,
                                                          color_write_mask, 1, rec);
        cold_active = cold_prepare(c, &recipe, &direct);
    }
    if (!rp_pass_begin(c)) { skip("pass_begin_failed", rec); goto draw_done; }
    DT(RP_PERF_D_PASS);   // 开通道单独记；打印时与取管线合成原来那一项，另出一行细分
    for (int st = 0; st < RP_PROG_STAGES; st++) g_bind_keep[st] = !rp_args_dirty[st];
    g_pipe_set = 0;   // 换管线会把两个阶段都立脏，绑定状态变没变要在那之前取
    if (rp_pso_dirty || !g_pipe || g_pipe_cmd != c) {
        // 配方有没有可能变：只有状态真的变了（或本 cmd 还没取过管线）才值得记预热清单。换 cmd 时
        // g_pipe 会重取一次，但配方与上一笔相同——每次都记的话真机每帧 130 次配方序列化 + 散列，
        // 「开通道与取管线」会多 1 毫秒左右（旧包基线 4.35、预热包 5.3–6.05 就是这笔）。
        const int recipe_new = rp_pso_dirty || !g_pipe;
        nvn_gpu_pipe_desc pd = replay_pipe_desc(rt, prim, quads, zattr, zstream, clip_neg1,
                                                color_write_mask, 0, rec);
        g_ms_off = pd.samples > 1 && !pd.ms.enable;
        nvn_gpu_pipe np = NULL; nvn_gpu_err err = {0};
        const uint64_t pg0_ = tm_ ? dt_now() : 0;
        // 失败表（见 rp_pfail_kind）：表空时不算摘要；在表里 / 还在退避期的不交后端，跳过这笔
        const int pf_have = g_pipe_bad.n || g_pipe_retry.n;
        rp_pfail_k pfk = {0, 0}; if (pf_have) pfk = rp_pfail_key(&pd);
        if (pf_have && rp_pfail_hit(&pfk)) { nvn_unimpl("gpu", "pipe_get_cached_fail", rec); g_skips++; g_pipe = NULL; goto draw_done; }
        const poly_st pf_poly0 = pd.poly;   // 点 / 线退回填充重试会改描述；失败表的键按本笔原样算
        int fu_skip = 0;
        nvn_gpu_rc r = rp_fu_enabled() ? rp_draw_pipe_async(&pd, rt, &np, &err, &fu_skip)
                                       : nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &np, &err);
        if (!fu_skip && r.code == NVN_GPU_E_UNSUPPORTED && rp_S.poly.mode != NVN_POLYGON_MODE_FILL) {   // 设备没有点 / 线填充模式：记一笔，照旧按填充画（不让这笔绘制消失）
            nvn_unimpl("gpu", "polygon_mode_not_fill", rec);
            pd.poly.mode = NVN_POLYGON_MODE_FILL; memset(&err, 0, sizeof err);
            r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &np, &err);
        }
        if (fu_skip) {   // 首次缺管线、本笔可安全丢：跳过（后台建，下几笔/下帧再画）
            rp_fu_skip_note(fu_skip); g_skips++; g_pipe = NULL; goto draw_done;
        }
        if (!nvn_gpu_ok(r)) {
            char logic_name[32];
            const char *name = "pipe_get";
            if (r.code == NVN_GPU_E_UNSUPPORTED && rp_S.color.logic_op != NVN_LOGIC_OP_COPY) {
                snprintf(logic_name, sizeof logic_name, "logic_op_%u", (unsigned)rp_S.color.logic_op);
                name = logic_name;
            }
            if (r.code == NVN_GPU_E_UNSUPPORTED && name != logic_name) {   // #977：实例除数大于 1，设备没有除数扩展 / 超上限
                for (int i = 0; i < 16; i++)
                    if (pd.attr[i].fmt && pd.attr[i].stream < 16 && pd.stream[pd.attr[i].stream].divisor > 1) { name = "vertex_divisor_gt1"; break; }
            }
            const uint64_t un = nvn_unimpl("gpu", name, rec);
            if (rp_pfail_kind(r.code) != RP_PFAIL_NONE) {   // 记表 + 每类第一次打红行（带原因、签名、程序键）
                if (!pf_have) { const poly_st cur = pd.poly; pd.poly = pf_poly0; pfk = rp_pfail_key(&pd); pd.poly = cur; }
                rp_pfail_note(pfk, &pd, r.code, err.why);
            }
            else if (un == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", err.why);   // 不记表的（会话关闭 / 设备丢失）照旧只报首行
            g_skips++; g_pipe = NULL; goto draw_done;
        }
        if (pf_have) rp_pfail_ok(&pfk);
        g_pipe = np; g_pipe_cmd = c;
        if (recipe_new && nvk_pipe_warm_note_prog) nvk_pipe_warm_note_prog(&pd, g_prog, np);   // 下一局按这份配方提前建（验收看后端 warm 命中）；np 带清单标签，已记过的管线只加计数
        nvn_gpu_set_pipe(c, g_pipe); g_pipe_set = 1;
        if (tm_) rp_perf_add(RP_PERF_D_PGET, dt_now() - pg0_);   // 后端取管线 + 设管线（含在取管线那一项里）
        // 换管线：只有绑定布局（set / binding / 类别 / 数量）变了的阶段才立脏重发；布局没变时后端按管线布局自己判描述符组归属。
        for (int st = 0; st < RP_PROG_STAGES; st++) if (!prog_layout_same(g_lb[st].ok ? g_lb[st].pg : NULL, g_prog, st)) rp_args_dirty[st] = 1;
    }
    // #1015：多重采样目标上游戏关了多重采样。Vulkan 没有「目标多重采样、光栅按单样本」的开关（iOS 写满 sample_mask 的做法在 Vulkan 里
    // 等于没写：片元输出的样本掩码与光栅覆盖按位与）；照多重采样画，但具名记账，不静默。
    if (g_ms_off) nvn_unimpl("draw", "multisample_off_on_ms_target", rec);
    uint32_t tw = 1, th = 1;
    { int fi = -1; for (int i = 0; i < rt->n && i < 8 && fi < 0; i++) if (rt->color[i]) fi = i;   // 缺省视口的尺寸：第一个有图的颜色槽，没有就取深度目标
      if (fi >= 0) rp_pass_slot_size(fi, &tw, &th);   // 块压缩基底配一块当一个像素的视图时是块数（#709）
      else if (rt->depth) { uint32_t l = rt->has_dview && rt->dview.set_levels ? rt->dview.base_level : 0; tw = rt->depth->d.width >> l; th = rt->depth->d.height >> l; }
      rp_pass_noatt_size(&tw, &th);   // 无附件通道（颜色槽全空 / 全丢）：帧缓冲按开通道时取的视口尺寸，缺省视口与裁剪夹取都照它
      if (!tw) tw = 1;
      if (!th) th = 1; }
    DT(RP_PERF_D_PIPE);
    nvn_gpu_dyn dyn; if (!fill_dyn(&dyn, tw, th, rec)) goto draw_done;
    nvn_gpu_set_dyn(c, &dyn);
    for (int i = 0; i < 16; i++) if (streams >> i & 1) nvn_gpu_set_vbuf(c, (uint32_t)i, vb_buf[i], vb_off[i]);
    if (zattr) {   // 零属性的流：每笔从暂存区取 16 字节全 0
        nvn_gpu_slice zs;
        if (!alloc_copy(c, 16, NULL, &zs)) { skip("arena_alloc_failed", rec); goto draw_done; }
        nvn_gpu_set_vbuf(c, (uint32_t)zstream, zs.buf, zs.off);
    }
    uint32_t base_instance = ind ? 0 : (uint32_t)(di ? di->base_instance : dr->base_instance);
    DT(RP_PERF_D_DYN);
    // 每个阶段都要发，程序里没有的阶段发零条：后端按阶段留着上一笔的绑定，不清掉的话「上一笔有片元、这一笔只有顶点」
    // 会带着旧的片元绑定去配没有片元布局的管线，后端报布局错、整段作废
    for (int st = 0; st < RP_PROG_STAGES; st++) if (!bind_stage(c, g_prog, st, base_instance, tv[st], ts[st], rec)) goto draw_done;
    DT(RP_PERF_D_BIND);   // 首版每笔都整组重发（support / c0 每笔在暂存区里新建）；按脏位省重发是后面的事
    a.prim = prim; a.count = count; a.instances = (uint32_t)(di ? di->instances : dr->instances); if (!a.instances) a.instances = 1; a.base_instance = base_instance;
    rp_occl_draw(c);
    if (!cold_active || !nvn_vk_cold_guard_draw(c, &a)) nvn_gpu_draw(c, &a);
    for (int st = 0; st < RP_PROG_STAGES; st++) for (uint32_t i = 0; i < g_prog->nent[st]; i++)   // 图形阶段写了的存储图像：内容归 GPU（CPU 读前要回读）
        if (g_gimg[st][i] && (g_prog->ent[st][i].access & NVN_GPU_ACC_W)) rp_tex_mark_gpu_written(g_gimg[st][i]);
    // 首用跳过判据的 seen 标记：本笔真画成了，每个颜色/深度目标记「近两帧画过」（跳过的笔不走到这里，故不记）。
    if (rp_fu_enabled()) {
        uint64_t ff = rp_frame();
        for (int i = 0; i < rt->n && i < 8; i++) { const tex_t *t = rt->color[i]; if (!t) continue;
            s2a_fu_rt_mark_seen(t->serial, t->mem ? (uint64_t)(uintptr_t)t->mem : 0, (uint64_t)t->d.format, ff); }
        if (rt->depth) { const tex_t *t = rt->depth;
            s2a_fu_rt_mark_seen(t->serial, t->mem ? (uint64_t)(uintptr_t)t->mem : 0, (uint64_t)t->d.format, ff); }
    }
    rp_pass_note_draw((rp_S.ds.dt || rp_S.ds.dw || rp_S.ds.st ? RP_NOTE_DS : 0) | (prog_shader_writes(g_prog) ? RP_NOTE_SHADER_WRITE : 0));
    DT(RP_PERF_D_DRAW);
    if (g_log_left) {
        g_log_left--;
        char b[1100]; const blend_st *bl = &rp_S.blend[0];
        snprintf(b, sizeof b, "第 %llu 笔：图元 %u 数 %u 实例 %u%s 目标 #%llu %ux%u 格式 %#x 视图格式 %#x 颜色槽 %d 程序 %02x%02x/%02x%02x 混合 开%#x 源%u 目%u 掩码 %#x 视口 %.0f,%.0f %.0fx%.0f 裁剪 %d(%d,%d %dx%d) 深度测 %d 模板 %d 剔除 %u 正面 %u%s",
                 (unsigned long long)g_draws, prim, count, a.instances, di ? " 带索引" : "", (unsigned long long)(rt->n && rt->color[0] ? rt->color[0]->serial : rt->depth ? rt->depth->serial : 0), tw, th, (unsigned)(rt->n && rt->color[0] ? rt->color[0]->d.format : 0), (unsigned)(rt->n && rt->color[0] && rt->has_cview[0] && rt->cview[0].set_format ? rt->cview[0].format : 0), rt->n,
                 rp_S.skey[0][0], rp_S.skey[0][1], rp_S.skey[1][0], rp_S.skey[1][1], (unsigned)rp_S.color.blend_en, (unsigned)bl->src_rgb, (unsigned)bl->dst_rgb, (unsigned)rp_S.chmask.mask,
                 dyn.vp[0][0], dyn.vp[0][1], dyn.vp[0][2], dyn.vp[0][3], dyn.has_sc, dyn.sc[0], dyn.sc[1], dyn.sc[2], dyn.sc[3], (int)rp_S.ds.dt, (int)rp_S.ds.st, (unsigned)rp_S.poly.cull, (unsigned)rp_S.poly.front, tl);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "draw", b);
        if (rt->n > 1) { int o = snprintf(b, sizeof b, "  其余颜色目标："); for (int k = 1; k < rt->n; k++) o += snprintf(b + o, sizeof b - (size_t)o, " [%d]=#%llu(%#x)", k, (unsigned long long)(rt->color[k] ? rt->color[k]->serial : 0), (unsigned)(rt->color[k] ? rt->color[k]->d.format : 0)); nvn_plat_log(NVN_PLAT_LOG_INFO, "draw", b); }
        // 片元阶段第 0 个常量缓冲的前 20 个四元组与着色器自带常量段的前 2 个（调试取证：对照着色器里读的位置看内容对不对）
        { const p_bind_buf *ub = &rp_S.ubo[1][0];
          float uf[80];
          if (ub->addr && ub->size >= 320 && rp_pool_find(ub->addr, ub->size, NULL, NULL)) {
            if (!dbg_peek((const void *)(uintptr_t)ub->addr, uf, sizeof uf)) { snprintf(b, sizeof b, "  片元常量 0（长 %llu）：读不到", (unsigned long long)ub->size); nvn_plat_log(NVN_PLAT_LOG_INFO, "draw", b); }
            else for (int q = 0; q < 20; q += 5) { int o = snprintf(b, sizeof b, "  片元常量 0（长 %llu）第 %d 项起：", (unsigned long long)ub->size, q);
                for (int k = q; k < q + 5; k++) o += snprintf(b + o, sizeof b - (size_t)o, "[%.3g %.3g %.3g %.3g]", (double)uf[k * 4], (double)uf[k * 4 + 1], (double)uf[k * 4 + 2], (double)uf[k * 4 + 3]);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "draw", b); } }
          if (g_prog->c1_ok >> 1 & 1) { float f[8]; memcpy(f, g_prog->c1[1], sizeof f);   // c1 至少 0x100 字节（c1_ok 的判据）
            snprintf(b, sizeof b, "  片元自带常量：[%.3g %.3g %.3g %.3g][%.3g %.3g %.3g %.3g]", (double)f[0], (double)f[1], (double)f[2], (double)f[3], (double)f[4], (double)f[5], (double)f[6], (double)f[7]);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "draw", b); } }
    }
    { uint64_t ts4[HIST_TEX]; int nt = 0;
      for (int st = 0; st < RP_PROG_STAGES; st++) for (uint32_t i = 0; i < g_prog->nent[st] && nt < HIST_TEX; i++) { const nvn_gpu_layout_ent *e = &g_prog->ent[st][i];
          if (e->src_kind != NVN_RB_TEX || e->count > 1) continue;
          uint64_t h = graphics_tex_handle(st, e->src_reg); tex_t snap; view_t sv; int hv;
          if (rp_tex_snap_by_id(h ? (int)(h & 0xFFFFF) : rp_S.tex_base, &snap, &sv, &hv)) ts4[nt++] = snap.serial; }
      int noted = 0;
      for (int k = 0; k < rt->n; k++) if (rt->color[k]) { hist_note(rt->color[k]->serial, ts4, nt); noted = 1; }   // 每个颜色目标各记一条：多目标的绘制，第二个以后的目标也要能往上追
      if (!noted) hist_note(rt->depth ? rt->depth->serial : 0, ts4, nt); }
    rp_pso_dirty = rp_dss_dirty = 0; memset(rp_args_dirty, 0, sizeof rp_args_dirty);
    g_draws++;
    DT(RP_PERF_D_TAIL);
draw_done:
    g_cold_drv.buf = NULL;
    for (int st = 0; st < RP_PROG_STAGES; st++) for (uint32_t i = 0; i < g_prog->nent[st]; i++)
        if (texel_tmp(&g_prog->ent[st][i]) && tv[st][i]) nvn_gpu_view_release(tv[st][i]);
    rp_view_pin_end();
}

// ---------- 调试：把「上屏那张图」和它上游几层的贴图读回落盘（开关 nvk_dump；rp_present 每 600 帧调一次） ----------
// 每笔绘制记下目标序号和它采样的头几张贴图序号；从上屏的那张图往回走：画到它的那些笔采样了谁，再往上，至多 3 层、12 张。
// 每张读第 0 级第 0 层的原始字节，写到缓存目录（没有就写诊断目录）的 nvkdump_<帧>_<层>_<序号>_<宽>x<高>_<格式>.raw。
struct dump_job { nvn_gpu_buf buf; uint64_t bytes; char path[300]; };
static void dump_done(void *user, const nvn_gpu_done *d) {
    struct dump_job *j = user;
    if (d->status != NVN_GPU_DONE_FAILED) { FILE *f = fopen(j->path, "wb"); if (f) { fwrite(nvn_gpu_buf_cpu(j->buf), 1, (size_t)j->bytes, f); fclose(f); nvn_plat_log(NVN_PLAT_LOG_INFO, "dump", j->path); } }
    nvn_gpu_buf_release(j->buf, NULL, NULL); free(j);
}
// 上屏线程只登记「要读回」；真正的读回在提交线程上、一次提交收尾时做（时间线的值只有提交线程能发）
static uint64_t g_dump_serial, g_dump_frame;
void rp_debug_dump_request(uint64_t present_serial, uint64_t frame) { __atomic_store_n(&g_dump_frame, frame, __ATOMIC_RELEASE); __atomic_store_n(&g_dump_serial, present_serial, __ATOMIC_RELEASE); }
static void dump_chain(uint64_t present_serial, uint64_t frame, uint64_t *fence);
void rp_debug_dump_poll(uint64_t *fence) {
    uint64_t s = __atomic_exchange_n(&g_dump_serial, 0, __ATOMIC_ACQ_REL);
    if (s) dump_chain(s, __atomic_load_n(&g_dump_frame, __ATOMIC_ACQUIRE), fence);
}
static void dump_chain(uint64_t present_serial, uint64_t frame, uint64_t *fence) {
    // 先用缓存目录：安卓上它在应用私有区，调试包可以用 run-as 取出来；诊断目录在外部存储的应用目录里，新系统上 adb 读不到
    const char *dir = nvn_plat_dir(NVN_PLAT_DIR_CACHE); if (!dir) dir = nvn_plat_dir(NVN_PLAT_DIR_DIAG);
    if (!dir) { nvn_plat_log(NVN_PLAT_LOG_WARN, "dump", "没有可写目录，不落盘"); return; }
    uint64_t list[DUMP_MAX]; int depth[DUMP_MAX], n = 0; list[n] = present_serial; depth[n++] = 0;
    // 点名要读的贴图序号（调试开关 nvk_dump_s0 … s5）：不靠往上追，直接读；层号记 9
    for (int k = 0; k < 6; k++) { char nm[16]; snprintf(nm, sizeof nm, "nvk_dump_s%d", k); int sv = nvn_plat_flag(nm, 0); if (sv > 0) { list[n] = (uint64_t)sv; depth[n++] = 9; } }
    uint32_t have = g_hist_n < HIST_N ? g_hist_n : HIST_N;
    for (int i = 0; i < n && n < DUMP_MAX; i++) {
        if (depth[i] >= 6) continue;
        for (uint32_t h = 0; h < have && n < DUMP_MAX; h++) { if (g_hist[h].target != list[i]) continue;
            for (int k = 0; k < HIST_TEX && n < DUMP_MAX; k++) { uint64_t s = g_hist[h].tex[k]; if (!s) continue;
                int seen = 0; for (int q = 0; q < n; q++) if (list[q] == s) seen = 1;
                if (!seen) { list[n] = s; depth[n++] = depth[i] + 1; } } }
    }
    for (int i = 0; i < n; i++) {
        nvn_gpu_tex g = rp_tex_backend_by_serial(list[i]); if (!g) continue;
        nvn_gpu_tex_info ti; nvn_gpu_tex_info_get(g, &ti);
        int bw = 1, bh = 1, bpb = nvn_fmt_bpb((int)ti.nvn_format, &bw, &bh);
        if (!bpb || bw != 1 || bh != 1 || !ti.w || !ti.h || ti.samples > 1 || ti.depth > 1) continue;   // 只读回不压缩的二维贴图：读回失败的段会让时间线进失败终态
        uint64_t row = (uint64_t)((ti.w + (uint32_t)bw - 1) / (uint32_t)bw) * (uint64_t)bpb, bytes = row * ((ti.h + (uint32_t)bh - 1) / (uint32_t)bh);
        struct dump_job *j = calloc(1, sizeof *j); nvn_gpu_err err = {0}; nvn_gpu_cmd c = NULL; nvn_gpu_cmd_desc cd = {0, "nvk dump"};
        if (!j || !nvn_gpu_ok(nvn_gpu_buf_create(bytes, NVN_GPU_MEM_SHARED, NULL, &j->buf, &err)) || !nvn_gpu_ok(nvn_gpu_cmd_begin(&cd, &c, &err))) { if (j && j->buf) nvn_gpu_buf_release(j->buf, NULL, NULL); free(j); continue; }
        j->bytes = bytes; snprintf(j->path, sizeof j->path, "%s/nvkdump_%llu_%d_%llu_%ux%u_%#x.raw", dir, (unsigned long long)frame, depth[i], (unsigned long long)list[i], ti.w, ti.h, ti.nvn_format);
        nvn_gpu_copy_tex_buf(c, g, 0, 0, NULL, j->buf, 0, row, bytes);
        *fence += 1;
        nvn_gpu_rc cr = nvn_gpu_cmd_commit(c, *fence, dump_done, j, &err);
        if (!nvn_gpu_ok(cr)) { char m[260]; snprintf(m, sizeof m, "读回 #%llu（%ux%u 格式 %#x）提交失败 %d：%s", (unsigned long long)list[i], ti.w, ti.h, ti.nvn_format, cr.code, err.why); nvn_plat_log(NVN_PLAT_LOG_RED, "dump", m); }
    }
}

// ---------- 计算派发（nvn_mtl.m dispatch @13556） ----------
// 取计算程序 → 落地待折清除并结束通道（派发在通道外，且可能读写那些目标）→ 绑定 NVN 阶段 5 的资源 → 派发。
// 范围：直接与池镜像参数的原生间接派发；常量缓冲、驱动段、按槽绑定的存储缓冲、单张组合贴图与图像。贴图数组：记账跳过。
static nvn_hmap g_cpipes = {.klen = 8};   // 计算程序 → 计算管线
static nvn_hmap g_cpipe_bad = {.klen = 16};   // 建管线失败过的（计算程序, 着色器）：失败是确定的，不再每次派发同步重建（读档后每帧 26 次 × ~27 毫秒）
// 计算存储图像的视图格式：按着色器的声明取，不按贴图自己的格式（对齐 Ryujinx TextureBindingsManager 按绑定的格式取贴图 / 视图，iOS nvn_mtl.m img_fit）。
//   声明 Unknown：数值类（dcls：0 浮点，1 uint，2 sint）和贴图相同就用贴图的格式；不同就取同字节数、目标数值类的格式
//     （RG32F 被按位读成 uint → RG32UI，原始位不变）。Vulkan 要求视图数值类与 Sampled Type 一致（VUID-07753）。
//   声明具体格式（宿主不支持无格式读时 SULD.B 声明 R32ui 一类）：和贴图格式相同照用；字节数相同取声明的格式；
//     字节数不同 Vulkan 建不出与声明一致的视图，报名跳过，不按贴图格式硬绑（那是未定义行为）。
//   声明没解析出来（dtt = 0）：照旧用贴图格式。
static const uint8_t k_spv_img_nvn[40] = {   // SPIR-V Image Format 枚举 → NVN 格式
    [1] = 0x2E, [2] = 0x29, [3] = 0x0A, [4] = 0x25, [5] = 0x26, [6] = 0x16, [7] = 0x11, [8] = 0x3F, [9] = 0x05, [10] = 0x2A,
    [11] = 0x3D, [12] = 0x12, [13] = 0x0D, [14] = 0x06, [15] = 0x01, [16] = 0x2B, [17] = 0x13, [18] = 0x0E, [19] = 0x07, [20] = 0x02,
    [21] = 0x30, [22] = 0x2D, [23] = 0x28, [24] = 0x0C, [25] = 0x18, [26] = 0x15, [27] = 0x10, [28] = 0x09, [29] = 0x04, [30] = 0x2F,
    [31] = 0x2C, [32] = 0x27, [33] = 0x0B, [34] = 0x3E, [35] = 0x17, [36] = 0x14, [37] = 0x0F, [38] = 0x08, [39] = 0x03 };
static const char *image_view_format(uint32_t have, uint8_t dtt, uint8_t dcls, uint8_t difmt, uint32_t *want) {
    *want = have;
    if (!dtt) return NULL;
    nvn_fmt_info fi;
    if (!nvn_fmt_info_of(have, &fi) || fi.bw != 1 || fi.bh != 1 || fi.depth || fi.stencil) return NULL;   // 压缩 / 深度模板：存储能力检查照旧拦
    if (difmt) {
        uint32_t f = difmt < sizeof k_spv_img_nvn ? k_spv_img_nvn[difmt] : 0; nvn_fmt_info di;
        if (!f || !nvn_fmt_info_of(f, &di)) return "compute_image_decl_format_unmapped";
        if (f == have) return NULL;
        if (di.bytes != fi.bytes) return "compute_image_decl_format_size_mismatch";
        *want = f; return NULL;
    }
    int cls = fi.kind == NVN_FMT_UINT ? 1 : fi.kind == NVN_FMT_SINT ? 2 : 0;
    if (dcls > 2 || dcls == cls) return NULL;
    static const uint8_t ui[17] = {[1] = 0x03, [2] = 0x08, [4] = 0x0B, [8] = 0x17, [16] = 0x2F}, si[17] = {[1] = 0x04, [2] = 0x09, [4] = 0x0C, [8] = 0x18, [16] = 0x30},
                         fl[17] = {[2] = 0x05, [4] = 0x0A, [8] = 0x16, [16] = 0x2E};
    uint32_t f = fi.bytes <= 16 ? (dcls == 1 ? ui : dcls == 2 ? si : fl)[fi.bytes] : 0;
    if (!f) return "compute_image_class_no_view";
    *want = f; return NULL;
}
// 存储图像（NVN_RB_IMG）解析：计算派发与图形绘制同一口径（声明选视图格式、可存储、单级、无重排、声明目标）。
//   stage = NVN 阶段号（取 rp_S.img[stage] 的句柄），pi / i = 程序里的阶段下标与项。成功返回 NULL，*out 为视图、*snapout 为登记快照；
//   失败返回具名原因：计算 / 图形各一组名字，计数分得开。
static const char *const g_imgw[2][6] = {
    {"compute_image_shape", "compute_image_unresolved", "compute_image_storage_unsupported", "compute_image_view_unsupported", "compute_image_decl_type_mismatch", "compute_image_decl_view_failed"},
    {"graphics_image_shape", "graphics_image_unresolved", "graphics_image_storage_unsupported", "graphics_image_view_unsupported", "graphics_image_decl_type_mismatch", "graphics_image_decl_view_failed"}};
static const char *image_why_graphics(const char *why) {   // image_view_format 的原因名换成图形阶段那组
    static const char *const m[][2] = {{"compute_image_decl_format_unmapped", "graphics_image_decl_format_unmapped"},
        {"compute_image_decl_format_size_mismatch", "graphics_image_decl_format_size_mismatch"}, {"compute_image_class_no_view", "graphics_image_class_no_view"}};
    for (unsigned k = 0; k < sizeof m / sizeof m[0]; k++) if (!strcmp(why, m[k][0])) return m[k][1];
    return why;
}
static const char *image_resolve(nvn_gpu_cmd c, const struct rp_prog *pg, int pi, uint32_t i, int stage, nvn_gpu_view *out, tex_t *snapout) {
    const nvn_gpu_layout_ent *e = &pg->ent[pi][i]; const int gfx = stage != 5;
#define IMGW(k) g_imgw[gfx][k]
    *out = NULL;
    if (e->cls != NVN_GPU_RES_IMAGE || e->count > 1 || e->src_reg >= 32) return IMGW(0);
    uint64_t h = rp_S.img[stage][e->src_reg];
    tex_t snap; view_t view; int has_view;
    if (!rp_tex_snap_by_id((int)(h & 0xFFFFF), &snap, &view, &has_view)) return IMGW(1);
    uint32_t format = has_view && view.set_format ? (uint32_t)view.format : (uint32_t)snap.d.format, vfmt;
    { const char *why = image_view_format(format, pg->dtt[pi][i], pg->dcls[pi][i], pg->difmt[pi][i], &vfmt); if (why) return gfx ? image_why_graphics(why) : why; }
    { uint32_t a = rp_storage_srgb_alias(vfmt); if (a) vfmt = a; }   // sRGB 贴图按同位宽 UNORM 视图写（后端能力位见 rp_storage_srgb_alias）
    if (!(snap.d.flags & NVN_TEXTURE_FLAGS_IMAGE) || !(nvn_gpu_format_caps(vfmt) & NVN_GPU_FMT_STORAGE)) {
        // 两种成因分不开就没法修：前 4 次把格式与两个条件打出来（贴图没带图像标志 / 后端这个格式不能当存储图像）。
        //   可存储按实际要建的视图格式（按着色器声明选的 vfmt）判，所以贴图格式与视图格式都打
        static int said;
        if (said < 4) { said++; char b[240]; snprintf(b, sizeof b, "%s图像不可写：贴图 %d 格式 %#x（视图改过格式 %d）、按声明的视图格式 %#x、带图像标志 %d、后端可存储 %d",
            gfx ? "图形阶段" : "计算", (int)(h & 0xFFFFF), format, has_view && view.set_format, vfmt, !!(snap.d.flags & NVN_TEXTURE_FLAGS_IMAGE), !!(nvn_gpu_format_caps(vfmt) & NVN_GPU_FMT_STORAGE));
            nvn_plat_log(NVN_PLAT_LOG_RED, "draw", b); }
        return IMGW(2);
    }
    uint32_t levels = has_view && view.set_levels ? (view.nlevels ? view.nlevels : 1) : (snap.d.levels ? snap.d.levels : 1);
    int swizzled = 0;
    for (int ch = 0; ch < 4; ch++) {
        uint32_t sw = has_view && view.set_swizzle ? view.swz[ch] : snap.set_swz && snap.swz ? (snap.swz >> (3 * ch) & 7) : (uint32_t)(2 + ch);
        if (sw <= 5 && sw != (uint32_t)(2 + ch)) swizzled = 1;
    }
    if (levels != 1 || swizzled) return IMGW(3);
    view_t fv; if (has_view) fv = view; else memset(&fv, 0, sizeof fv);
    int target_change = rp_decl_view_target(&snap, &fv, has_view, pg->dtt[pi][i]);
    if (target_change < 0) return IMGW(4);
    *out = rp_view_by_id((int)(h & 0xFFFFF), c, snapout);
    if (!*out || !snapout->serial) return IMGW(1);
    if (target_change || vfmt != format) {   // 目标和格式在同一份存储视图上组合。
        if (vfmt != format) { fv.set_format = 1; fv.format = (uint16_t)vfmt; }
        *out = rp_view_for(snapout, &fv, c);
        if (!*out) return IMGW(5);
    }
    return NULL;
#undef IMGW
}
// 图像槽里的缓冲贴图（imageBuffer，布局类别 IMAGE_TBUF）：按槽句柄取登记的 BUFFER 贴图，按声明选视图格式（同 image_resolve 的 image_view_format：
//   声明了格式就用声明的，Eden / Ryujinx 同样取着色器的显式格式），在池镜像上建存储纹素缓冲视图（texel_bind，不对齐时走对齐副本）。
//   着色器会写的这一段登记 GPU 写（段完成后回写游戏内存，同可写存储缓冲）。成功返回 NULL，*out 为本笔临时视图（调用方释放）
static const char *image_tbuf_resolve(nvn_gpu_cmd c, const struct rp_prog *pg, int pi, uint32_t i, int stage, nvn_gpu_view *out) {
    static const char *const nm[2][6] = {
        {"compute_image_tbuf_shape", "compute_image_tbuf_unresolved", "compute_image_tbuf_texture_shape", "compute_image_tbuf_format_unsupported", "compute_image_tbuf_buffer_unresolved", "compute_image_tbuf_decl_type_mismatch"},
        {"graphics_image_tbuf_shape", "graphics_image_tbuf_unresolved", "graphics_image_tbuf_texture_shape", "graphics_image_tbuf_format_unsupported", "graphics_image_tbuf_buffer_unresolved", "graphics_image_tbuf_decl_type_mismatch"}};
    const nvn_gpu_layout_ent *e = &pg->ent[pi][i]; const int gfx = stage != 5;
    *out = NULL;
    if (e->cls != NVN_GPU_RES_IMAGE_TBUF || e->count != 1 || e->src_reg >= 8) return nm[gfx][0];
    if (pg->dtt[pi][i] != NVN_TEXTURE_TARGET_BUFFER + 1) return nm[gfx][5];
    const uint64_t h = rp_S.img[stage][e->src_reg];
    tex_t t; view_t v; int hv = 0;
    if (!rp_tex_snap_by_id((int)(h & 0xFFFFF), &t, &v, &hv)) return nm[gfx][1];
    // 槽里不是缓冲贴图：Ryujinx 只绑同维度的（Texture.GetTargetTexture），iOS 给声明类型的空图像；这里具名跳过，不拿别的类型硬绑
    if (t.d.target != NVN_TEXTURE_TARGET_BUFFER || (hv && v.set_target && v.target != NVN_TEXTURE_TARGET_BUFFER) ||
        !t.mem || !t.pool || !t.d.width || t.d.levels > 1 || t.d.samples > 1) return nm[gfx][2];
    const uint32_t format = hv && v.set_format ? (uint32_t)v.format : (uint32_t)t.d.format;
    uint32_t vfmt;
    { const char *why = image_view_format(format, pg->dtt[pi][i], pg->dcls[pi][i], pg->difmt[pi][i], &vfmt); if (why) return gfx ? image_why_graphics(why) : why; }
    nvn_fmt_info base, view;
    if (!nvn_fmt_info_of((uint32_t)t.d.format, &base) || !nvn_fmt_info_of(vfmt, &view) || !base.bytes || base.bw != 1 || base.bh != 1 || base.depth || base.stencil ||
        view.bytes != base.bytes || view.bw != 1 || view.bh != 1 || view.depth || view.stencil) return nm[gfx][3];
    const uint64_t bytes = (uint64_t)base.bytes * t.d.width;   // 与 tbuf_prepare 那行写法不同：run.sh --tbuf-neg-elements 按那一行做负控，锚点要唯一
    nvn_gpu_buf buf = NULL; uint64_t off = 0;
    if (!rp_buf_resolve(c, (uint64_t)(uintptr_t)t.mem, bytes, &buf, &off)) return nm[gfx][4];
    const int write = (e->access & NVN_GPU_ACC_W) != 0;
    const char *why = texel_bind(c, buf, off, bytes, base.bytes, vfmt, gfx ? 2 : 1, write, out);
    if (why) return why;
    if (write) rp_post_gpu_write((uint64_t)(uintptr_t)t.mem, bytes);
    return NULL;
}
void rp_dispatch(nvn_gpu_cmd c, const p_dispatch *dp, const void *rec) {
    if (!c) return;
    rp_perf_add(RP_PERF_DISP_N, 1);   // 派发次数（其余命令里最贵的一项，看它到底多少次）
    struct timespec tsa; clock_gettime(CLOCK_MONOTONIC, &tsa); uint64_t t_disp0 = (uint64_t)tsa.tv_sec * 1000000000ull + (uint64_t)tsa.tv_nsec;
    if (!dp->ind && (dp->x <= 0 || dp->y <= 0 || dp->z <= 0)) return;   // 0 组：空操作
    const struct rp_prog *pg = NULL;
    { struct timespec tsh; clock_gettime(CLOCK_MONOTONIC, &tsh); uint64_t sh0 = (uint64_t)tsh.tv_sec * 1000000000ull + (uint64_t)tsh.tv_nsec;
      int srx = rp_shader_resolve_compute(&pg);
      clock_gettime(CLOCK_MONOTONIC, &tsh); rp_perf_add(RP_PERF_DISP_SHADER_NS, (uint64_t)tsh.tv_sec * 1000000000ull + (uint64_t)tsh.tv_nsec - sh0);
      if (srx) { skip(srx > 0 ? "compute_program_not_ready" : "compute_program_unresolved", rec); return; } }
    static nvn_gpu_view tv[RP_PROG_ENT_MAX]; static nvn_gpu_samp ts[RP_PROG_ENT_MAX];
    static tex_t img_snap[RP_PROG_ENT_MAX];   // 存储图像的登记快照（调用方持有；登记表会扩容换地址）
    const tex_t *images[RP_PROG_ENT_MAX];
    int lib_retry = 0;   // 出库版绑定核对失败只回退现编一次：现编版再失败照旧具名跳过
cs_rebind:
    g_ntwb = 0;
    memset(tv, 0, sizeof tv); memset(ts, 0, sizeof ts); memset(images, 0, sizeof images);
    struct timespec tr0; clock_gettime(CLOCK_MONOTONIC, &tr0); uint64_t r0 = (uint64_t)tr0.tv_sec * 1000000000ull + (uint64_t)tr0.tv_nsec;
    rp_perf_add(RP_PERF_DISP_ENTS, pg->nent[0]);
    rp_view_pin_begin();
    for (uint32_t i = 0; i < pg->nent[0]; i++) { const nvn_gpu_layout_ent *e = &pg->ent[0][i];
        if (e->src_kind == NVN_RB_TBUF) {
            rp_pass_flush(c);
            if (!tbuf_prepare(c, e, 5, pg->dtt[0][i], pg->dcls[0][i], &tv[i], rec)) goto done;
            continue;
        }
        if (e->src_kind == NVN_RB_IMG && e->cls == NVN_GPU_RES_IMAGE_TBUF) {   // 计算 imageBuffer：缓冲视图（见 image_tbuf_resolve）；写过的对齐副本派发后拷回
            const char *why = image_tbuf_resolve(c, pg, 0, i, 5, &tv[i]);
            if (why) { skip(why, rec); goto done; }
            continue;
        }
        if (e->src_kind == NVN_RB_IMG) {
            const char *why = image_resolve(c, pg, 0, i, 5, &tv[i], &img_snap[i]);
            if (why) {
                const struct rp_prog *np = lib_fallback_compute(pg, &lib_retry);   // 出库版声明与实绑不符：身份标坏换现编版，同一笔重绑
                if (np) { pg = np; goto cs_rebind; }
                skip(why, rec); goto done;
            }
            images[i] = &img_snap[i];
            continue;
        }
        if (e->src_kind == NVN_RB_TEXP || e->src_kind == NVN_RB_SAMPP) {   // 整池数组（#1594 片 8）：与绘制同一个取池快照函数，表挂在 g_ptd 的计算阶段那一行
            const char *why = pool_table_resolve(c, pg, 0, i, pool_row(5));
            if (why) { skip(why, rec); goto done; }
            continue;
        }
        if (e->src_kind == NVN_RB_TEXU) {   // 贴图字在游戏用户常量缓冲里的类型 3 句柄（#1594 片 10）：读法、采样器号、没登记 / 建不出的处理与图形阶段同一份
            if (e->count != 1 || e->src_reg2 >= pg->nusrc) { skip("uniform_handle_shape", rec); goto done; }
            int tid = 0, sid = 0; const char *bad = usrc_ids(5, &pg->usrc[e->src_reg2], &tid, &sid);   // 常量缓冲取计算阶段（5）绑的那一份
            if (bad) { skip(bad, rec); goto done; }
            tv[i] = rp_view_by_ubo_handle_decl(tid, pg->dtt[0][i], pg->dcls[0][i], c, NULL);   // 没登记的号绑零贴图（Ryujinx null 贴图），不跳整次派发
            const int const_samp = pg->usrc[e->src_reg2].samp_pool;
            ts[i] = const_samp ? rp_samp_for_const_handle(sid, tid, rp_S.samp_base, rp_S.samp_pool_n) : rp_samp_for_tex(sid, tid);
            if (!ts[i] && const_samp) { skip("const_sampler_unresolved", rec); goto done; }   // 号没登记 = 默认采样器（片 6）；登记了但后端建不出才具名跳过
            continue;
        }
        if (e->src_kind != NVN_RB_TEX) continue;
        // 类型 3 句柄（#1594 片 7）：布局项 src_reg2 带常量采样器标记 + 池下标，形状校验与图形阶段同一份（samp2_shape_ok）；数组 / 认不出的形状仍具名跳过
        if (e->count > 1 || e->src_reg >= RP_NTEX || (e->src_reg2 != UINT16_MAX && (e->count != 1 || !samp2_shape_ok(e->src_reg2)))) { skip("compute_texture_array", rec); goto done; }
        const int cs_const = e->src_reg2 != UINT16_MAX && (e->src_reg2 & NVN_GPU_SAMP2_CONST);
        // 贴图槽 32..39 = c2 图像句柄表第 0..7 项被当贴图采样：取图像槽绑着的句柄（同 iOS nvn_mtl.m tex_slot_h）；没绑同组合槽，读作池第 0 项
        uint64_t h = e->src_reg >= 32 && e->src_reg < 40 ? rp_S.img[5][e->src_reg - 32] : rp_S.tex[5][e->src_reg];
        { int unb = 0;
          tv[i] = sampled_view(pg, lib_retry, &unb, 0, h ? (int)(h & 0xFFFFF) : rp_S.tex_base, pg->dtt[0][i], pg->dcls[0][i], 0, c, NULL);
          if (unb) {
              const struct rp_prog *np = lib_fallback_compute(pg, &lib_retry);
              if (np) { pg = np; goto cs_rebind; }
              if (unb == RP_DECL_NUMERIC_MISMATCH) { skip("sampled_numeric_fallback_failed", rec); goto done; }
              tv[i] = rp_view_by_id_decl(h ? (int)(h & 0xFFFFF) : rp_S.tex_base, pg->dtt[0][i], pg->dcls[0][i], 0, c, NULL);
          } }
        if (cs_const) {
            // 采样器号 = 贴图字高 12 位 | 着色器里的立即数池下标（const_samp_id，同 Ryujinx ReadPackedId），登记号 = 采样器号 + 池起点；
            //   号没登记 = 空采样器：绑默认采样器照派发（同图形阶段片 6）；登记了但后端建不出的具名跳过。不经 NVN 采样器槽。
            ts[i] = rp_samp_for_const_handle(const_samp_id((uint32_t)h, e->src_reg2 & NVN_GPU_SAMP2_ID_MASK) + rp_S.samp_base, h ? (int)(h & 0xFFFFF) : rp_S.tex_base,
                                             rp_S.samp_base, rp_S.samp_pool_n);
            if (!ts[i]) { skip("const_sampler_unresolved", rec); goto done; }
        } else if (e->src_reg2 != UINT16_MAX) {
            if (!rp_S.samp_bound[5][e->src_reg2]) { skip("separate_sampler_unbound", rec); goto done; }
            ts[i] = rp_samp_for_tex((int)(rp_S.samp[5][e->src_reg2] >> 20), h ? (int)(h & 0xFFFFF) : rp_S.tex_base);
            if (!ts[i]) { skip("separate_sampler_unresolved", rec); goto done; }
        } else {
            ts[i] = rp_samp_for_tex(h ? (int)(h >> 20) : rp_S.samp_base, h ? (int)(h & 0xFFFFF) : rp_S.tex_base);
            if (tv[i] && !ts[i] && pg->sampler_unused[0][i]) ts[i] = rp_sampler_for_query();
        }
    }
    { struct timespec tr1; clock_gettime(CLOCK_MONOTONIC, &tr1); rp_perf_add(RP_PERF_DISP_RESOLVE_NS, (uint64_t)tr1.tv_sec * 1000000000ull + (uint64_t)tr1.tv_nsec - r0); }
    { struct timespec tsb; clock_gettime(CLOCK_MONOTONIC, &tsb); uint64_t f0 = (uint64_t)tsb.tv_sec * 1000000000ull + (uint64_t)tsb.tv_nsec;
      rp_pass_flush(c);
      clock_gettime(CLOCK_MONOTONIC, &tsb); rp_perf_add(RP_PERF_DISP_FLUSH_NS, (uint64_t)tsb.tv_sec * 1000000000ull + (uint64_t)tsb.tv_nsec - f0); }
    g_disp_ind = NULL; g_disp_ind_off = 0;
    { struct timespec tin; clock_gettime(CLOCK_MONOTONIC, &tin); uint64_t in0 = (uint64_t)tin.tv_sec * 1000000000ull + (uint64_t)tin.tv_nsec;
      int bad = dp->ind && ((dp->ind & 3) || !rp_buf_resolve(c, dp->ind, 12, &g_disp_ind, &g_disp_ind_off));
      clock_gettime(CLOCK_MONOTONIC, &tin); rp_perf_add(RP_PERF_DISP_IND_NS, (uint64_t)tin.tv_sec * 1000000000ull + (uint64_t)tin.tv_nsec - in0);
      if (bad) { skip("dispatch_indirect_unresolved", rec); goto done; } }
    // 共享内存溢出缓冲（同 iOS nvn_mtl.m dispatch 的 cs->spill）：每个线程组一片，按这次真正派发的组数分，派发后不留。
    //   间接派发的组数在记录里：只在这几个字没有待落地的 GPU 写时按游戏内存里的实值分；GPU 写出的读不到，报名跳过，不猜上限。
    g_spill_len = 0;
    if (pg->spill_bytes) {
        if (g_spill_on < 0) g_spill_on = nvn_plat_flag("nvk_spill", 1) != 0;
        if (!g_spill_on) { skip("compute_spill_disabled", rec); goto done; }
        uint32_t dims[3];
        if (dp->ind) {
            if (rp_buf_gpu_write_pending(dp->ind, 12) != 0) { skip("compute_spill_indirect_gpu_written", rec); goto done; }
            memcpy(dims, (const void *)(uintptr_t)dp->ind, sizeof dims);
            // 同一份快照既决定分配大小，也交给 GPU 派发。原镜像可能有尚未执行的
            // 同步拷贝，不能从它的映射地址推断 GPU 最终读到的值。
            nvn_gpu_slice args;
            if (!alloc_copy(c, sizeof dims, dims, &args)) { skip("compute_spill_indirect_snapshot_failed", rec); goto done; }
            g_disp_ind = args.buf; g_disp_ind_off = args.off;
        } else { dims[0] = (uint32_t)dp->x; dims[1] = (uint32_t)dp->y; dims[2] = (uint32_t)dp->z; }
        uint64_t groups = 1;
        for (int i = 0; i < 3; i++) {
            if (!dims[i]) { groups = 0; break; }
            if (groups > UINT64_MAX / dims[i]) { skip("compute_spill_size_overflow", rec); goto done; }
            groups *= dims[i];
        }
        if (!groups) groups = 1;   // 0 组的间接派发什么都不跑，绑定仍要合法
        if (groups > UINT64_MAX / pg->spill_bytes) { skip("compute_spill_size_overflow", rec); goto done; }
        const uint64_t bytes = groups * pg->spill_bytes;
        const nvn_gpu_caps *cap = caps_cached();
        if (!cap->max_ssbo_range) { skip("compute_spill_range_unknown", rec); goto done; }
        if (bytes > (512ull << 20) || bytes > cap->max_ssbo_range) { skip("compute_spill_too_large", rec); goto done; }
        nvn_gpu_err err = {0};
        nvn_gpu_rc arc = nvn_gpu_arena_alloc(c, bytes, cap->min_ssbo_align ? cap->min_ssbo_align : 256, &g_spill, &err);
        if (!nvn_gpu_ok(arc)) { arena_fail_detail(arc, &err, bytes); skip("compute_spill_alloc_failed", rec); goto done; }
        g_spill_len = bytes;
        if (g_spill_said < 64) {
            g_spill_said++; char m[280]; uint64_t key = 0; for (int b = 0; b < 8; b++) key = key << 8 | rp_S.skey[5][b];
            const char *why = nvk_shader_cs_lib_note ? nvk_shader_cs_lib_note(rp_S.skey[5]) : NULL;
            snprintf(m, sizeof m, "spill 派发 #%d：程序键 %016llx，%s %llu 组 × %u 字节 = %llu 字节；%s", g_spill_said, (unsigned long long)key,
                     dp->ind ? "间接" : "直接", (unsigned long long)groups, pg->spill_bytes, (unsigned long long)bytes,
                     pg->lib_sourced ? "出库版" : why ? why : "没有出库记录");
            nvn_plat_log(NVN_PLAT_LOG_INFO, "replay", m);
        }
    }
    uint64_t key = (uint64_t)(uintptr_t)pg; nvn_gpu_pipe cp = nvn_hm_get(&g_cpipes, &key);
    if (!cp) {
        uint64_t bkey[2] = {(uint64_t)(uintptr_t)pg, (uint64_t)(uintptr_t)pg->sh[0]};
        if (nvn_hm_get(&g_cpipe_bad, bkey)) { nvn_unimpl("gpu", "compute_pipeline", rec); g_skips++; goto done; }
        nvn_gpu_stage_desc cs; memset(&cs, 0, sizeof cs); cs.sh = pg->sh[0]; nvn_gpu_err err = {0};
        nvn_gpu_rc cr = nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &err);
        if (!nvn_gpu_ok(cr) || !cp) {
            if (nvn_unimpl("gpu", "compute_pipeline", rec) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", err.why);
            if (cr.code != NVN_GPU_E_STOPPED) {   // 会话关闭中不算确定失败
                nvn_hm_put(&g_cpipe_bad, bkey, (void *)(uintptr_t)1);
                static unsigned said; if (said < 16) { said++; char b[320]; snprintf(b, sizeof b, "计算管线建失败，记下不再重建：程序 %p 着色器 %p 码 %d：%s（第 %u 个失败的程序）", (void *)pg, (void *)pg->sh[0], cr.code, err.why, said); nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", b); }
            }
            g_skips++; goto done;
        }
        nvn_hm_put(&g_cpipes, &key, cp);
    }
    struct timespec tsd; clock_gettime(CLOCK_MONOTONIC, &tsd); uint64_t b0 = (uint64_t)tsd.tv_sec * 1000000000ull + (uint64_t)tsd.tv_nsec;
    nvn_gpu_set_pipe(c, cp);
    g_pipe = NULL; g_pipe_cmd = NULL; rp_dirty_all();   // 后端的当前管线换成了计算管线：下一笔绘制全部重发
    g_disp_dim[0] = dp->ind ? 0 : (uint32_t)dp->x; g_disp_dim[1] = dp->ind ? 0 : (uint32_t)dp->y; g_disp_dim[2] = dp->ind ? 0 : (uint32_t)dp->z;
    g_ndispatch_access = 0;
    if (!bind_stage_at(c, pg, 0, 5, 0, tv, ts, rec)) goto done;
    for (uint32_t i = 0; i < pg->nent[0]; i++) if (images[i] && (pg->ent[0][i].access & NVN_GPU_ACC_W)) rp_tex_mark_gpu_written(images[i]);
    // 游戏屏障（#978）：派发在通道外，等不到下一个通道开头，这里取走待发位交给后端；间接参数的那道合在同一次里
    uint32_t bar = rp_pass_take_barrier() | (g_disp_ind ? NVN_BARRIER_ORDER_INDIRECT_DATA_BIT | NVN_BARRIER_INVALIDATE_SHADER_BIT : 0);
    if (bar) { nvn_gpu_barrier(c, bar); g_compute_access.n = 0; }
    nvn_gpu_dispatch(c, cp, g_disp_dim[0], g_disp_dim[1], g_disp_dim[2], g_disp_ind, g_disp_ind_off);
    texel_wb_flush(c);   // imageBuffer 的对齐副本：派发写完拷回池镜像
    for (size_t i = 0; i < g_ndispatch_access; i++) rp_access_write(&g_compute_access, &g_dispatch_access[i]);
    if (nvk_pipe_warm_note_prog && nvn_plat_flag("nvk_compute_warm", 0)) {
        static const nvn_gpu_pipe_desc compute_recipe = {0};
        nvk_pipe_warm_note_prog(&compute_recipe, pg, cp);
    }
    { struct timespec tse; clock_gettime(CLOCK_MONOTONIC, &tse); rp_perf_add(RP_PERF_DISP_BIND_NS, (uint64_t)tse.tv_sec * 1000000000ull + (uint64_t)tse.tv_nsec - b0); }
    g_dispatches++;
done:
    g_ntwb = 0;   // 没派发成（跳过）：副本没被写过，不拷回
    for (uint32_t i = 0; i < pg->nent[0]; i++)
        if (texel_tmp(&pg->ent[0][i]) && tv[i]) nvn_gpu_view_release(tv[i]);
    rp_view_pin_end();
    { struct timespec tsc; clock_gettime(CLOCK_MONOTONIC, &tsc); rp_perf_add(RP_PERF_DISP_TOT_NS, (uint64_t)tsc.tv_sec * 1000000000ull + (uint64_t)tsc.tv_nsec - t_disp0); }
}

void rp_draw_stats(uint64_t *draws, uint64_t *skips) { if (draws) *draws = g_draws; if (skips) *skips = g_skips; }
void rp_draw_reset(void) { g_land_bound = -1; rp_access_reset(&g_compute_access); g_compute_access_on = -1; g_ndispatch_access = 0; g_caps_ok = 0; g_ubf_w = 0; memset(g_ubf, 0, sizeof g_ubf); g_ubf_steal = -1; g_arena_reason_n = 0; g_arena_reason_full = 0;   // 会话重开：能力表重新取
    g_spill_on = -1; g_spill_said = 0;
    rp_tm_reset();   // 分时抽样的开关也重新读（#1103）
    memset(g_ind_drv_ok, 0, sizeof g_ind_drv_ok); memset(g_ind_drv, 0, sizeof g_ind_drv); memset(g_lb, 0, sizeof g_lb);   // #941
    memset(g_hzt, 0, sizeof g_hzt); g_hzi = 0; g_hz_mask = 0; g_hz_on = hz_mode(nvn_plat_flag("nvk_binst_hoist", 2));   // 批量拷开关会话打开时读定
    free(g_cpipes.keys); free(g_cpipes.vals); nvn_hm_init(&g_cpipes, 8); free(g_cpipe_bad.keys); free(g_cpipe_bad.vals); nvn_hm_init(&g_cpipe_bad, 16); rp_pfail_reset(); g_prog = NULL; g_pipe = NULL; g_pipe_cmd = NULL; g_draws = g_skips = 0; g_prim = ~0u; g_quads = -1; g_raster = -1; g_ms_off = 0; }
