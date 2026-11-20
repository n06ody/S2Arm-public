// 绘制（对齐 nvn_mtl.m draw / draw_emit @12900 一带与 encode_args_ @9640 的职责）：
// 状态类命令早已落在 rp_S 里，这里在绘制时把它们变成后端的管线、动态状态、顶点流和绑定，再发绘制。
// 没改过的不重发：管线看 rp_pso_dirty，各阶段绑定看 rp_args_dirty[阶段]。
// 四边形图元（NVN 7）没有原生对应：在这里展开成三角形列表的 32 位索引（每个四边形 0,1,2 / 0,2,3，同 nvn_mtl.m fanquad_expand），按带索引的三角形画。
// 另外三种没有原生对应的图元（#710）：四边形带（8）当三角带画（顶点序不变；零头 1 个丢掉；带索引又开着重启时按段展开成三角形列表），
// 多边形（9）当三角扇画，线环（2）展开成线段列表的 32 位索引（每段末尾补一条回到段首的线）。
// 还没接：间接计数多绘、变换反馈、邻接图元与面片、图元重启以外的小状态、存储缓冲与图像绑定、空槽假贴图。
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// 弱链接登记（tools/weak_link_decl.py，场合 replay = 回放核心宿主单测）：后端 / 首用模块的可选件，缺实现时地址为空、走不到调用
// 弱链接[replay]：nvk_fu_frame_ms
// 弱链接[replay]：nvk_fu_item_ms
// 弱链接[replay]：nvk_fu_unsafe_ms
// 弱链接[replay]：nvk_pipe_warm_note_prog
// 弱链接[replay]：nvn_vk_pipe_demand_kick
// 弱链接[replay]：nvn_vk_pipe_demand_mark
// 弱链接[replay]：nvn_vk_pipe_demand_marks
// 弱链接[replay]：nvn_vk_pipe_demand_on
// 弱链接[replay]：nvn_vk_pipe_demand_wait
// 弱链接[replay]：s2a_fu_budget_left_ms
// 弱链接[replay]：s2a_fu_budget_spend
// 弱链接[replay]：s2a_fu_rt_fresh_now
// 弱链接[replay]：s2a_fu_rt_mark_seen
// 弱链接[replay]：s2a_fu_rt_seen_steady
// 弱链接[replay]：s2a_fu_wait_decide

// SupportBuffer 里现在要填的字段（翻译器产物的布局；其余保持 0）
enum { SB_RENDER_SCALE = 192, SB_S2A_FLAGS = 1392, SB_S2A_POINT = 1408, SB_RENDER_SCALE_Y = 1440, SB_RENDER_SCALE_COUNT = 73 };
// 驱动常量段 c0 的合成布局（着色器从它读存储缓冲描述符表、最大点径、base instance）
enum { DRV0_BYTES = 0xA00 };
static const int k_drv0_base[5] = {0x110, 0x510, 0x410, 0x210, 0x310};   // 各图形阶段的存储缓冲表起点

int rp_pool_find(uint64_t addr, uint64_t len, const uint8_t **mem, uint64_t *size);   // rp_entry.c
extern int s2a_nvn_rast_flip;   // hle_nvn.c：当前提交队列的正面光栅翻转位（建队列时按窗口原点定）
// 管线预热清单（安卓后端件 android/native/nvn/nvn_vk_pipe_warm.c）：真实创建成功后记一条配方，下一局
// 开局由预热线程按同一份配方提前把管线建好，真实 draw 直接命中后端缓存。声明成弱符号：iOS 与宿主单测
// 没有这份实现，缺了就当作「不记清单」（不拦执行、不返回假成功）。程序身份在那边从 rp_prog 取，程序
// 没有身份（失败项 / 还没算出来）就不记。np 是刚取到的后端管线：清单在它上面存一个标签，之后换回同一条
// 管线只加计数（不序列化、不散列、不查表）。
int nvk_pipe_warm_note_prog(const nvn_gpu_pipe_desc *d, const struct rp_prog *p, nvn_gpu_pipe np) __attribute__((weak));

// 句柄放在游戏用户常量缓冲里的贴图（NVN_RB_TEXU；语义照 iOS nvn_mtl.m ubo_read / ubo_handle / ubo_packed / ubo_tex_id / ubo_samp_id / bind_sid）。
// 一个字：常量槽 reg 的字节 off 处，编码这一刻从游戏内存读（同 NVN_RB_CBUF 绑的就是这一刻的内容）；槽没绑、越界、不在登记过的池里读作 0（iOS：空槽）
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
static uint64_t g_draws, g_skips, g_dispatches; static uint32_t g_prim = ~0u; static int g_quads = -1, g_face = -1;   // g_quads：这笔的索引展开方式（EXP_*）
static int g_ms_off;   // #1015：当前管线是「多重采样目标上关了多重采样」（游戏 ms.enable = 0、目标样本数 > 1）

// ---- 首用管线异步：后台建 + 可恢复的首次缺画（iOS pso_pend / first_skip_ok 的 Vulkan 新回放版）----
// 弱引用安卓 demand 层与三档上限读数：iOS / 鸿蒙 / 宿主单测没有这份实现 → rp_fu_enabled()=0，行为原样退回 SYNC。
// 状态值必须与 android/native/nvn/nvn_vk_pipe_demand.h 的 NVK_DEMAND_* 一致（那边是强定义，这里只做弱引用）。
enum { RP_FU_DEMAND_NONE = 0, RP_FU_DEMAND_PENDING = 1, RP_FU_DEMAND_READY = 2, RP_FU_DEMAND_FAILED = 3 };
int  nvn_vk_pipe_demand_kick(const nvn_gpu_pipe_desc *, const uint8_t[32], nvn_gpu_pipe *) __attribute__((weak));
int  nvn_vk_pipe_demand_wait(const nvn_gpu_pipe_desc *, const uint8_t[32], uint32_t, nvn_gpu_pipe *, char *, size_t) __attribute__((weak));
int  nvn_vk_pipe_demand_on(void) __attribute__((weak));
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
            if (rp_S.ssbo[st][i].size) return 1;
    if (g_prog)
        for (int st = 0; st < RP_PROG_STAGES; st++)
            for (uint32_t i = 0; i < g_prog->nent[st]; i++)
                if (g_prog->ent[st][i].src_kind == NVN_RB_IMG) return 1;
    return 0;
}
// iOS skip_safe：无 storage 写 + 每个颜色/深度目标本帧 fresh + 近两帧 steady。任一不满足就不许跳。
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
static nvn_gpu_rc rp_fu_ok(void) { nvn_gpu_rc r; r.code = NVN_GPU_OK; return r; }
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
static int sb_window(const p_bind_buf *sb, int align_down, uint64_t *wa, uint64_t *wl) {
    const nvn_gpu_caps *cp = caps_cached();
    uint64_t a = sb->addr, l = sb->size;
    if (align_down && cp->min_ssbo_align > 1) { const uint64_t r = a & (uint64_t)(cp->min_ssbo_align - 1); a -= r; l += r; }
    const int clamped = cp->max_ssbo_range && l > cp->max_ssbo_range;
    if (clamped) l = cp->max_ssbo_range;
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
//   ② 回放线程自己改游戏内存（OP_UPDATE_UB / OP_COPY_B2B / OP_CLEAR_BUF）之前调 rp_ub_fill_fence：来源与它相交、还没填完的先等；
//   ③ 一次提交里游戏不改它的缓冲（GPU 在提交之后才读，见段内切片表那段说明）；提交返回之前段已提交，①已经等完。
// 完成回调里的「落地」（GPU 写回游戏内存）和原先编码时就拷一样是另一条线程上的并发写，挪后不改变这一点。
// 只挪大块：小块走队列一条记录的开销比拷贝本身还大。开关 nvk_ub_fill（缺省开；0 = 照旧在回放线程上拷）。
enum { UBF_MIN = 256, UBF_N = 256 };
static struct { uint64_t lo, hi, t; } g_ubf[UBF_N];   // 交出去的填充：来源区间与票号，按交出顺序环形存放（票号递增）
static uint32_t g_ubf_w; static uint64_t g_ubf_done;   // 交出去的条数；已知填完的最大票号
static int ubf_on(void) { static int v = -1; static unsigned k; if (v < 0 || !(++k & 255)) v = nvn_plat_flag("nvk_ub_fill", 1) != 0; return v; }
void rp_ub_fill_fence(uint64_t addr, uint64_t len) {
    if (!g_ubf_w || !len) return;
    g_ubf_done = nvn_gpu_arena_fill_wait(0);
    const uint64_t hi = addr + len;
    for (uint32_t k = 0; k < UBF_N && k < g_ubf_w; k++) {   // 从新往旧：票号递增，遇到已填完的，更旧的也都填完了
        const uint32_t i = (g_ubf_w - 1 - k) % UBF_N;
        if (g_ubf[i].t <= g_ubf_done) break;
        if (g_ubf[i].lo < hi && addr < g_ubf[i].hi) { g_ubf_done = nvn_gpu_arena_fill_wait(g_ubf[i].t); rp_perf_add(RP_PERF_UBO_FILL_WAIT_N, 1); return; }   // 等最新一条相交的：更旧的随之填完
    }
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
        if (g_ubf[i].t > g_ubf_done) g_ubf_done = nvn_gpu_arena_fill_wait(g_ubf[i].t);   // 表满：挤掉的那条必须已经填完，否则相交的写就漏等了
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
// 图形阶段存储图像：这一笔解析出的登记快照（调用方持有；登记表会扩容换地址），画成后按写访问登记 GPU 写
static tex_t g_gimg_snap[RP_PROG_STAGES][RP_PROG_ENT_MAX]; static const tex_t *g_gimg[RP_PROG_STAGES][RP_PROG_ENT_MAX];

// 图形高槽与驱动图像表重叠：与 DRV2 的写入顺序、iOS tex_slot_h 同一来源。
static uint64_t graphics_tex_handle(int st, unsigned reg) {
    if (st < RP_PROG_STAGES && reg >= 32 && reg < 40 && rp_S.img[st][reg - 32])
        return rp_S.img[st][reg - 32];
    return rp_S.tex[st][reg];
}

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
    nvn_gpu_err err = {0}; nvn_gpu_rc rc = nvn_gpu_view_create_buf(buf, off, bytes, fmt, out, &err);
    if (!nvn_gpu_ok(rc) || !*out) return skip("tbuf_view_create_failed", rec);
    return 1;
}

// 一个阶段的全部绑定。0 = 有取不到的资源（已记账），这笔不画
// pi：程序里的阶段下标（0 顶点 / 计算，1 片元）；st：NVN 阶段号（回放状态按它取：0 顶点、1 片元、5 计算）
static nvn_gpu_buf g_disp_ind; static uint64_t g_disp_ind_off;
// 本笔绘制在开通道之前解析好的图形阶段存储缓冲（[阶段][布局项]）；只在 rp_draw 里填、在同一笔的发绑定里用
static nvn_gpu_buf g_sb_buf[RP_PROG_STAGES][RP_PROG_ENT_MAX]; static uint64_t g_sb_off[RP_PROG_STAGES][RP_PROG_ENT_MAX];
static uint8_t g_sb_ok[RP_PROG_STAGES][RP_PROG_ENT_MAX];
// Per-draw GPU-populated driver slices. Never retained by the binding cache.
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
// 整池数组：开通道前取好的池快照（rp_pool_table），bind_stage_at 在普通绑定之后逐项 nvn_gpu_pool_bind
static nvn_gpu_pool_table g_ptd[RP_PROG_STAGES][RP_PROG_ENT_MAX];
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
        if (st < RP_PROG_STAGES && !g_ind_drv_ok[st] && g_bind_keep[pi] && g_lb[pi].ok) {   // #941：本笔用 GPU 写出的 DRV0 的阶段不能沿用
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
            if (st >= RP_PROG_STAGES || i >= RP_PROG_ENT_MAX || !g_ptd[st][i]) return skip("pool_table_unresolved", rec);
            npool++; continue;
        }
        nvn_gpu_binding *o = &b[n]; memset(o, 0, sizeof *o); o->ent = (uint16_t)i; o->cls = e->cls;
        nvn_gpu_slice sl;
        if (keep && e->src_kind != NVN_RB_SBUFP && e->src_kind != NVN_RB_TEX && e->src_kind != NVN_RB_TEXU && e->src_kind != NVN_RB_IMG && e->src_kind != NVN_RB_TBUF) { *o = g_lb[pi].b[n]; n++; continue; }
        switch (e->src_kind) {
            case NVN_RB_SBUFP: case NVN_RB_UBOP: {   // 存储缓冲：取地址、绑池镜像里的那一段；可写的登记一次 GPU 写（完成后回写游戏内存）。两种来源只差在哪张表给地址：
                // SBUFP 按 NVN 存储缓冲槽（rp_S.ssbo）；UBOP 按计算阶段的用户 UBO 槽（rp_S.ubo；反射映射 NVK_STORAGE_UBO_PTR），段的范围就是这个 UBO 绑定的地址与大小
                const p_bind_buf *sb = e->src_reg < 16 ? (e->src_kind == NVN_RB_UBOP ? &rp_S.ubo[st][e->src_reg] : &rp_S.ssbo[st][e->src_reg]) : NULL;
                if (sb && e->src_kind == NVN_RB_SBUFP && !sb->addr && !sb->size) {
                    o->u.b.buf = rp_buf_for_null_storage();
                    if (!o->u.b.buf) return skip("storage_buffer_empty_failed", rec);
                    o->u.b.off = 0; o->u.b.len = 0x10000; break;
                }
                if (!sb || !sb->addr || !sb->size) return skip("storage_buffer_unbound", rec);
                const int sbufp = e->src_kind == NVN_RB_SBUFP;
                uint64_t wa, wl; const int clamped = sb_window(sb, sbufp, &wa, &wl);
                nvn_gpu_buf gb; uint64_t go;
                if (st < RP_PROG_STAGES && i < RP_PROG_ENT_MAX && g_sb_ok[st][i]) { gb = g_sb_buf[st][i]; go = g_sb_off[st][i]; }   // 开通道前已按同一窗口同步（见 rp_draw 开头）
                else if (!rp_buf_resolve(c, wa, wl, &gb, &go)) return skip("storage_buffer_unresolved", rec);
                // 镜像偏移必须对齐；UBOP 着色器按原基址取数，不能通过移动窗口补救。
                const uint32_t al = caps_cached()->min_ssbo_align;
                if (al > 1 && (go & (al - 1))) return skip("storage_buffer_offset_unaligned", rec);
                if (clamped) nvn_unimpl("draw", "storage_buffer_window_clamped", rec);   // 超出 maxStorageBufferRange 的部分着色器访问不到：照画，具名记账
                // 回写只登记游戏绑定的那一段（窗口前补的余数字节不是这个缓冲的，回写会盖掉相邻内容）
                if (e->access & NVN_GPU_ACC_W) { const uint64_t reach = wa + wl - sb->addr;   // 窗口里属于这个缓冲的长度（余数 < 对齐 ≤ 256，上限 ≥ 128 MiB，不会为负）
                    rp_post_gpu_write(sb->addr, reach < sb->size ? reach : sb->size); }
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
                if (!slc_get_x(c, (uint64_t)(uintptr_t)pg ^ (uint64_t)(pi + 1) << 60, 0x100, pg->c1[pi], &sl, 1)) return skip("arena_alloc_failed", rec);   // 程序自带常量段：内容恒定，段内一直有效
                o->u.b.buf = sl.buf; o->u.b.off = sl.off; o->u.b.len = 0x100; break; }
            case NVN_RB_CBUF: {
                const uint64_t cbuf_t0 = btm ? dt_now() : 0;
                const p_bind_buf *ub = &rp_S.ubo[st][e->src_reg];
                if (!ub->addr || !ub->size) return skip("uniform_buffer_unbound", rec);
                // 常量缓冲一律拷进本段的暂存区再绑：就是编码这一刻的内容（按序的 OP_UPDATE_UB 之后再用自然是新值），
                // 不经池镜像，也就没有通道内的传输。地址必须在登记过的池里。
                if (!rp_pool_find(ub->addr, ub->size, NULL, NULL)) return skip("uniform_buffer_outside_pools", rec);
                if (ub->size > 0xffffffffull) return skip("uniform_buffer_too_large", rec);
                rp_perf_add(RP_PERF_UBO_N, 1); rp_perf_add(RP_PERF_UBO_KB, ub->size >> 10);
                if (rp_buf_gpu_pending(ub->addr, ub->size)) {   // 这段有还没落地的 GPU 写（计算写、GPU 上做的拷贝 / 清除，#1022）：游戏内存里是旧内容，直接绑镜像
                    const nvn_gpu_caps *cp = caps_cached(); const uint64_t al = cp->min_ubo_align ? cp->min_ubo_align : 256;
                    nvn_gpu_buf mb; uint64_t mo;
                    if ((!cp->max_ubo_range || ub->size <= cp->max_ubo_range) && rp_buf_resolve(c, ub->addr, ub->size, &mb, &mo) && !(mo & (al - 1))) {
                        rp_perf_add(RP_PERF_UBO_MIRROR_N, 1);
                        o->u.b.buf = mb; o->u.b.off = mo; o->u.b.len = ub->size; if (btm) g_bt[4] += dt_now() - cbuf_t0; break;
                    }
                    nvn_unimpl("draw", "uniform_gpu_pending_unaligned", rec);   // 镜像偏移不满足常量缓冲对齐：照旧暂存游戏内存（旧内容），记账
                }
                if (!slc_get(c, ub->addr, ub->size, (const void *)(uintptr_t)ub->addr, &sl)) return skip("arena_alloc_failed", rec);
                o->u.b.buf = sl.buf; o->u.b.off = sl.off; o->u.b.len = ub->size; if (btm) g_bt[4] += dt_now() - cbuf_t0; break; }
            case NVN_RB_SPILL:   // 共享内存溢出缓冲：rp_dispatch 按这次的组数分好的那一片
                if (st != 5 || !g_spill_len) return skip("compute_spill_unallocated", rec);
                o->u.b.buf = g_spill.buf; o->u.b.off = g_spill.off; o->u.b.len = g_spill_len; break;
            case NVN_RB_IMG: {
                if (e->cls != NVN_GPU_RES_IMAGE || e->count > 1) return skip(st == 5 ? "compute_image_shape" : "graphics_image_shape", rec);
                if (!tv[i]) return skip(st == 5 ? "compute_image_unresolved" : "graphics_image_unresolved", rec);
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
            if (!nvn_gpu_ok(nvn_gpu_pool_bind(c, gst, i, g_ptd[st][i], &err))) {
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
        int64_t x0 = rp_R.sc.x < 0 ? 0 : rp_R.sc.x, y0 = rp_R.sc.y < 0 ? 0 : rp_R.sc.y, x1 = (int64_t)rp_R.sc.x + rp_R.sc.w, y1 = (int64_t)rp_R.sc.y + rp_R.sc.h;
        if (x1 > (int64_t)tw) x1 = tw;
        if (y1 > (int64_t)th) y1 = th;
        if (x1 <= x0 || y1 <= y0) return 0;
        d->has_sc = 1; d->sc[0] = (int32_t)x0; d->sc[1] = (int32_t)y0; d->sc[2] = (int32_t)(x1 - x0); d->sc[3] = (int32_t)(y1 - y0);
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
void rp_draw(nvn_gpu_cmd c, const p_draw *dr, const p_draw_idx *di, const void *rec) {
    if (!c) return;
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
    if (prim == 7) { quads = EXP_QUADS; prim = 4; }
    else if (prim == 2) { quads = EXP_LOOP; prim = 1; }
    else if (prim == 5 && gs && !ind) { quads = EXP_GS_TSTRIP; prim = 4; }   // 带 GS 的三角带按 GL 序展开（带索引的读游戏索引）；间接的照原生画
    else if (prim == 9) prim = 6;   // 多边形 = 三角扇（首点为扇心，绕序同扇）
    else if (prim == 8) {           // 四边形带 = 三角带；零头 1 个点在三角带里会多画一个三角形，丢掉
        if (restart) { quads = EXP_QSTRIP; prim = 4; }
        else { count &= ~1u; if (count < 4) return; prim = 5; }
    }
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
    { int face = (rp_origin_lower_left() != 0) | (s2a_nvn_rast_flip != 0) << 1;   // 窗口原点与光栅翻转位不在状态块里，但进管线描述（正面绕序）：变了就是换管线
      if (face != g_face) { g_face = face; rp_pso_dirty = 1; } }   // 图元在绘制记录里，不在状态块里：换了图元就是换管线
    int args_dirty = rp_args_dirty[0] || rp_args_dirty[1] || rp_args_dirty[2];
    if (!g_prog || rp_pso_dirty || args_dirty) {
        struct rp_shader_req q; memset(&q, 0, sizeof q); q.prim = prim; q.rt_count = rt->n;
        for (int i = 0; i < rt->n && i < 8; i++) if (rt->color[i]) q.rt_format[i] = (uint32_t)(rt->has_cview[i] && rt->cview[i].set_format ? rt->cview[i].format : rt->color[i]->d.format);
        const struct rp_prog *np = NULL;
        int sr = rp_shader_resolve(&q, &np);
        if (sr) { skip(sr > 0 ? "program_not_ready" : "program_unresolved", rec); g_prog = NULL; return; }
        if (np != g_prog) { g_prog = np; rp_pso_dirty = 1; }
    }
    // 仅拒绝绑定 DRV0 且未证明不读 base instance 的已解码 count 组；单条间接/#941 和 OP_MULTI_DRAW 保持原路径。
    if (g_decoded_count_n) {
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
            if (!sb || !sb->addr || !sb->size) continue;
            uint64_t wa, wl; (void)sb_window(sb, e->src_kind == NVN_RB_SBUFP, &wa, &wl);   // 与发绑定时同一个窗口（被夹短的记账在发绑定那边）
            if (nb < SB_BATCH) { bs[nb] = st; bi[nb] = (int)i; ba[nb] = wa; bl[nb] = wl; nb++; }
            else if (rp_buf_resolve(c, wa, wl, &g_sb_buf[st][i], &g_sb_off[st][i])) g_sb_ok[st][i] = 1;   // 超出一批的照旧逐段解析
        }
        if (nb) {
            rp_buf_resolve_multi(nb, ba, bl, bb, bo, bok);
            for (int k = 0; k < nb; k++) if (bok[k]) { g_sb_buf[bs[k]][bi[k]] = bb[k]; g_sb_off[bs[k]][bi[k]] = bo[k]; g_sb_ok[bs[k]][bi[k]] = 1; }
        }
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
            tv[st][i] = rp_view_by_ubo_handle_decl(tid, g_prog->dtt[st][i], g_prog->dcls[st][i], c, NULL);   // 没登记的号绑零贴图（Ryujinx null 贴图），不跳整笔
            // 立即数采样器池下标（#1594 片 3）：号没登记 = Ryujinx 的空采样器，绑默认采样器、整笔照画（片 6，不拿别的已登记采样器顶替）；
            //   登记了但后端建不出的仍具名跳过。samp_pool = 0（离线库登记号）口径不变
            const int const_samp = g_prog->usrc[e->src_reg2].samp_pool;
            ts[st][i] = const_samp ? rp_samp_for_const_handle(sid, tid, rp_S.samp_base, rp_S.samp_pool_n) : rp_samp_for_tex(sid, tid);
            if (!ts[st][i] && const_samp) { skip("const_sampler_unresolved", rec); goto draw_done; }
            if (g_log_left && tn < sizeof tl - 40) tn += (size_t)snprintf(tl + tn, sizeof tl - tn, " u%d.%u+%#x=#%d/s%d", st, (unsigned)e->src_reg, (unsigned)e->src_off, tid, sid);
            continue;
        }
        if (e->src_kind == NVN_RB_TEXP || e->src_kind == NVN_RB_SAMPP) {   // 整池数组：池快照（count = 池长 + 1 表尾 dummy；池长须与编译时一致）
            const int sp = e->src_kind == NVN_RB_SAMPP;
            const uint32_t pn = e->count ? (uint32_t)e->count - 1 : 0;
            if (pn != (uint32_t)(sp ? rp_S.samp_pool_n : rp_S.tex_pool_n)) { skip("pool_length_mismatch", rec); goto draw_done; }
            const char *why = NULL;
            g_ptd[st][i] = rp_pool_table(c, sp, sp ? rp_S.samp_base : rp_S.tex_base, pn, g_prog->dtt[st][i], g_prog->dcls[st][i], &why);
            if (!g_ptd[st][i]) { skip(why ? why : "pool_table_unresolved", rec); goto draw_done; }
            continue;
        }
        if (e->src_kind == NVN_RB_IMG) {   // 图形阶段存储图像：与计算派发同一解析（image_resolve），句柄取这一阶段 BindImage 的槽
            const char *why = image_resolve(c, g_prog, st, i, st, &tv[st][i], &g_gimg_snap[st][i]);
            if (why) { skip(why, rec); goto draw_done; }
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
                g_av[st][k] = rp_view_by_id_decl(hk ? (int)(hk & 0xFFFFF) : rp_S.tex_base, g_prog->dtt[st][i], g_prog->dcls[st][i], 1, c, NULL);
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
        tv[st][i] = rp_view_by_id_decl(h ? (int)(h & 0xFFFFF) : rp_S.tex_base, g_prog->dtt[st][i], g_prog->dcls[st][i], 0, c, tt);
        if (tt && tn < sizeof tl - 56) {
            int id = h ? (int)(h & 0xFFFFF) : rp_S.tex_base;
            if (tt->serial) tn += (size_t)snprintf(tl + tn, sizeof tl - tn, " t%d.%u=#%llu(%#x %dx%d 态%x 存%08x%08x 标%#x 池%#x 包%d)", st, (unsigned)e->src_reg, (unsigned long long)tt->serial, (unsigned)tt->d.format, tt->d.width, tt->d.height, (unsigned)rp_tex_debug_state(tt->serial),
                                         tt->mem && tt->L.size >= 8 ? (unsigned)((const uint32_t *)(const void *)tt->mem)[0] : 0u, tt->mem && tt->L.size >= 8 ? (unsigned)((const uint32_t *)(const void *)tt->mem)[1] : 0u, (unsigned)tt->d.flags, tt->pool ? (unsigned)tt->pool->flags : 0xffffu, (int)tt->has_layout);
            else tn += (size_t)snprintf(tl + tn, sizeof tl - tn, " t%d.%u=#%d(无)", st, (unsigned)e->src_reg, id);
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
    // 这一笔要采样的贴图里，有还挂着待折清除的：先让它落地（rp_pass_land_reads），否则采到的是没清过的旧内容。
    // 稳态下挂起表是空的（rp_pass_parked() == 0），这条近路不收集、不加锁；只有这一笔真遇到挂起项才走一次。
    if (rp_pass_parked()) {
        static tex_t rdsnap[2 * RP_TEXARR_MAX]; static const tex_t *rd[2 * RP_TEXARR_MAX];
        int nr = 0;
        for (int st = 0; st < RP_PROG_STAGES; st++)
            for (uint32_t i = 0; i < g_prog->nent[st] && nr < (int)(sizeof rd / sizeof rd[0]); i++) {
                const nvn_gpu_layout_ent *e = &g_prog->ent[st][i];
                if (e->src_kind == NVN_RB_TEXU) {   // 句柄在用户常量缓冲里：同取视图时的读法
                    int tid = 0, sid = 0;
                    if (e->src_reg2 < g_prog->nusrc && !usrc_ids(st, &g_prog->usrc[e->src_reg2], &tid, &sid) && rp_tex_snap_by_id(tid, &rdsnap[nr], NULL, NULL)) { rd[nr] = &rdsnap[nr]; nr++; }
                    continue;
                }
                if (e->src_kind == NVN_RB_IMG) { if (g_gimg[st][i]) { rd[nr] = g_gimg[st][i]; nr++; } continue; }   // 存储图像同样要读到清过的内容
                if (e->src_kind != NVN_RB_TEX) continue;
                uint32_t cnt = e->count > 1 ? e->count : 1;
                for (uint32_t k = 0; k < cnt && nr < (int)(sizeof rd / sizeof rd[0]); k++) {
                    int slot = (int)e->src_reg + (e->count > 1 ? (int)k - RP_HANDLE_TABLE_FIRST_SLOT : 0);
                    if (slot < 0 || slot >= (e->count > 1 ? 32 : 40)) continue;
                    uint64_t h = graphics_tex_handle(st, (unsigned)slot);
                    if (!rp_tex_snap_by_id(h ? (int)(h & 0xFFFFF) : rp_S.tex_base, &rdsnap[nr], NULL, NULL)) continue;
                    rd[nr] = &rdsnap[nr]; nr++;
                }
            }
        if (nr) rp_pass_land_reads(c, rd, nr);
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
                else if (sb && !sb->addr && !sb->size && e->src_kind == NVN_RB_SBUFP) ACCESS_READ(compute_read_buf(rp_buf_for_null_storage(), 0, 0x10000));
            } else if (e->src_kind == NVN_RB_TEX || e->src_kind == NVN_RB_TEXU || e->src_kind == NVN_RB_TBUF) {
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
    rp_pass_prepare_draw(c, color_write_mask);   // 有效写掩码与像素区域共同决定这一笔的通道
    if (!rp_pass_begin(c)) { skip("pass_begin_failed", rec); goto draw_done; }
    DT(RP_PERF_D_PASS);   // 开通道单独记；打印时与取管线合成原来那一项，另出一行细分
    for (int st = 0; st < RP_PROG_STAGES; st++) g_bind_keep[st] = !rp_args_dirty[st];
    g_pipe_set = 0;   // 换管线会把两个阶段都立脏，绑定状态变没变要在那之前取
    if (rp_pso_dirty || !g_pipe || g_pipe_cmd != c) {
        // 配方有没有可能变：只有状态真的变了（或本 cmd 还没取过管线）才值得记预热清单。换 cmd 时
        // g_pipe 会重取一次，但配方与上一笔相同——每次都记的话真机每帧 130 次配方序列化 + 散列，
        // 「开通道与取管线」会多 1 毫秒左右（旧包基线 4.35、预热包 5.3–6.05 就是这笔）。
        const int recipe_new = rp_pso_dirty || !g_pipe;
        nvn_gpu_pipe_desc pd; memset(&pd, 0, sizeof pd);
        for (int st = 0; st < RP_PROG_STAGES; st++) pd.st[st].sh = g_prog->sh[st];
        for (int i = 0; i < rt->n && i < 8; i++) { pd.cfmt[i] = rt->color[i] && rp_pass_slot_attached(i) ? (uint32_t)(rt->has_cview[i] && rt->cview[i].set_format ? rt->cview[i].format : rt->color[i]->d.format) : 0; pd.lmap[i] = (uint8_t)i; }
        if (rt->depth) { nvn_fmt_info fi; uint32_t f = (uint32_t)rt->depth->d.format; int ok = nvn_fmt_info_of(f, &fi); if (!ok || fi.depth) pd.dfmt = f; if (ok && fi.stencil) pd.sfmt = f; }
        const tex_t *c0 = NULL; for (int i = 0; i < rt->n && i < 8 && !c0; i++) c0 = rt->color[i];   // 第一个有图的颜色槽
        pd.samples = c0 ? (uint32_t)c0->d.samples : rt->depth ? (uint32_t)rt->depth->d.samples : 0;
        pd.prim = prim; pd.patch_size = (uint32_t)rp_S.patch_size; pd.prim_restart = (uint8_t)(rp_S.restart.on != 0 && !quads);   // 要展开的图元，重启在展开时已经处理掉
        memcpy(pd.blend, rp_S.blend, sizeof pd.blend); pd.color = rp_S.color; pd.chmask = rp_S.chmask; pd.ms = rp_S.ms;
        g_ms_off = pd.samples > 1 && !pd.ms.enable;
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
                    char nm[40]; snprintf(nm, sizeof nm, "blend_unsupported_fmt_0x%x", (unsigned)pd.cfmt[i]); nvn_unimpl("draw", nm, rec);
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
        pd.clip_neg1 = (uint8_t)(g_prog->z_raw && !rp_depth_zero());   // 程序没做 near = -w 的深度改写：交给管线（只在 CAP_DEPTH_CLIP_CTL 时会出这种程序）
        nvn_gpu_pipe np = NULL; nvn_gpu_err err = {0};
        const uint64_t pg0_ = tm_ ? dt_now() : 0;
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
            if (nvn_unimpl("gpu", name, rec) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", err.why);
            g_skips++; g_pipe = NULL; goto draw_done;
        }
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
    nvn_gpu_draw(c, &a);
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
    rp_pass_note_draw(rp_S.ds.dt || rp_S.ds.dw || rp_S.ds.st);
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
          if (ub->addr && ub->size >= 320 && rp_pool_find(ub->addr, ub->size, NULL, NULL)) { const float *f = (const float *)(uintptr_t)ub->addr;
            for (int q = 0; q < 20; q += 5) { int o = snprintf(b, sizeof b, "  片元常量 0（长 %llu）第 %d 项起：", (unsigned long long)ub->size, q);
                for (int k = q; k < q + 5; k++) o += snprintf(b + o, sizeof b - (size_t)o, "[%.3g %.3g %.3g %.3g]", (double)f[k * 4], (double)f[k * 4 + 1], (double)f[k * 4 + 2], (double)f[k * 4 + 3]);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "draw", b); } }
          if (g_prog->c1_ok >> 1 & 1) { float f[8]; memcpy(f, g_prog->c1[1], sizeof f);
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
    for (int st = 0; st < RP_PROG_STAGES; st++) for (uint32_t i = 0; i < g_prog->nent[st]; i++)
        if (g_prog->ent[st][i].src_kind == NVN_RB_TBUF && tv[st][i]) nvn_gpu_view_release(tv[st][i]);
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
    memset(tv, 0, sizeof tv); memset(ts, 0, sizeof ts);
    static tex_t img_snap[RP_PROG_ENT_MAX];   // 存储图像的登记快照（调用方持有；登记表会扩容换地址）
    const tex_t *images[RP_PROG_ENT_MAX] = {0};
    struct timespec tr0; clock_gettime(CLOCK_MONOTONIC, &tr0); uint64_t r0 = (uint64_t)tr0.tv_sec * 1000000000ull + (uint64_t)tr0.tv_nsec;
    rp_perf_add(RP_PERF_DISP_ENTS, pg->nent[0]);
    rp_view_pin_begin();
    for (uint32_t i = 0; i < pg->nent[0]; i++) { const nvn_gpu_layout_ent *e = &pg->ent[0][i];
        if (e->src_kind == NVN_RB_TBUF) {
            rp_pass_flush(c);
            if (!tbuf_prepare(c, e, 5, pg->dtt[0][i], pg->dcls[0][i], &tv[i], rec)) goto done;
            continue;
        }
        if (e->src_kind == NVN_RB_IMG) {
            const char *why = image_resolve(c, pg, 0, i, 5, &tv[i], &img_snap[i]);
            if (why) { skip(why, rec); goto done; }
            images[i] = &img_snap[i];
            continue;
        }
        if (e->src_kind != NVN_RB_TEX) continue;
        if (e->count > 1 || e->src_reg >= RP_NTEX || (e->src_reg2 != UINT16_MAX && (e->count != 1 || e->src_reg2 >= 128))) { skip("compute_texture_array", rec); goto done; }
        // 贴图槽 32..39 = c2 图像句柄表第 0..7 项被当贴图采样：取图像槽绑着的句柄（同 iOS nvn_mtl.m tex_slot_h）；没绑同组合槽，读作池第 0 项
        uint64_t h = e->src_reg >= 32 && e->src_reg < 40 ? rp_S.img[5][e->src_reg - 32] : rp_S.tex[5][e->src_reg];
        tv[i] = rp_view_by_id_decl(h ? (int)(h & 0xFFFFF) : rp_S.tex_base, pg->dtt[0][i], pg->dcls[0][i], 0, c, NULL);
        if (e->src_reg2 != UINT16_MAX) {
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
            g_spill_said++; char m[200]; uint64_t key = 0; for (int b = 0; b < 8; b++) key = key << 8 | rp_S.skey[5][b];
            snprintf(m, sizeof m, "spill 派发 #%d：程序键 %016llx，%s %llu 组 × %u 字节 = %llu 字节", g_spill_said, (unsigned long long)key,
                     dp->ind ? "间接" : "直接", (unsigned long long)groups, pg->spill_bytes, (unsigned long long)bytes);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "replay", m);
        }
    }
    uint64_t key = (uint64_t)(uintptr_t)pg; nvn_gpu_pipe cp = nvn_hm_get(&g_cpipes, &key);
    if (!cp) {
        nvn_gpu_stage_desc cs; memset(&cs, 0, sizeof cs); cs.sh = pg->sh[0]; nvn_gpu_err err = {0};
        if (!nvn_gpu_ok(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &err)) || !cp) { if (nvn_unimpl("gpu", "compute_pipeline", rec) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", err.why); g_skips++; goto done; }
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
    for (size_t i = 0; i < g_ndispatch_access; i++) rp_access_write(&g_compute_access, &g_dispatch_access[i]);
    if (nvk_pipe_warm_note_prog && nvn_plat_flag("nvk_compute_warm", 0)) {
        static const nvn_gpu_pipe_desc compute_recipe = {0};
        nvk_pipe_warm_note_prog(&compute_recipe, pg, cp);
    }
    { struct timespec tse; clock_gettime(CLOCK_MONOTONIC, &tse); rp_perf_add(RP_PERF_DISP_BIND_NS, (uint64_t)tse.tv_sec * 1000000000ull + (uint64_t)tse.tv_nsec - b0); }
    g_dispatches++;
done:
    for (uint32_t i = 0; i < pg->nent[0]; i++)
        if (pg->ent[0][i].src_kind == NVN_RB_TBUF && tv[i]) nvn_gpu_view_release(tv[i]);
    rp_view_pin_end();
    { struct timespec tsc; clock_gettime(CLOCK_MONOTONIC, &tsc); rp_perf_add(RP_PERF_DISP_TOT_NS, (uint64_t)tsc.tv_sec * 1000000000ull + (uint64_t)tsc.tv_nsec - t_disp0); }
}

void rp_draw_stats(uint64_t *draws, uint64_t *skips) { if (draws) *draws = g_draws; if (skips) *skips = g_skips; }
void rp_draw_reset(void) { rp_access_reset(&g_compute_access); g_compute_access_on = -1; g_ndispatch_access = 0; g_caps_ok = 0; g_ubf_w = 0; memset(g_ubf, 0, sizeof g_ubf); g_arena_reason_n = 0; g_arena_reason_full = 0;   // 会话重开：能力表重新取
    g_spill_on = -1; g_spill_said = 0;
    rp_tm_reset();   // 分时抽样的开关也重新读（#1103）
    memset(g_ind_drv_ok, 0, sizeof g_ind_drv_ok); memset(g_ind_drv, 0, sizeof g_ind_drv); memset(g_lb, 0, sizeof g_lb);   // #941
    memset(g_hzt, 0, sizeof g_hzt); g_hzi = 0; g_hz_mask = 0; g_hz_on = hz_mode(nvn_plat_flag("nvk_binst_hoist", 2));   // 批量拷开关会话打开时读定
    free(g_cpipes.keys); free(g_cpipes.vals); nvn_hm_init(&g_cpipes, 8); g_prog = NULL; g_pipe = NULL; g_pipe_cmd = NULL; g_draws = g_skips = 0; g_prim = ~0u; g_quads = -1; g_face = -1; g_ms_off = 0; }
