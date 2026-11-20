// 存储前瞻索引（iOS nvn_mtl.m 13684–13817 sdx 机器的移植）：提交开头把本次提交的命令流按回放顺序线性扫一遍，
// 每张贴图序号记一串事件 {seq, rtseq, 子资源, 类型}；通道 begin 时在该贴图的事件桶里二分「当前 rt 之后的首个事件」，
// 证明附件在本通道结束后内容不再被消费 → store 取 DONTCARE。判据保守：任何对不上号的都按 STORE。
//
// 与 iOS 的差异（Vulkan 契约的形状决定）：
//  · iOS 在通道 END 时决策（Metal 的 storeAction 在编码器收尾取）；Vulkan 的 storeOp 烘在渲染通道对象里，必须在
//    begin 时定。这里按「seq > 当前绘制、rtseq > 当前目标的 SET_RT 序号」扫第一条有效事件。等价性里有个坑：
//    同目标续通道（nvk_rt_cont）会让本通道带着新 rtseq 继续画——ATT@rtseq>R0 可能就在本通道里，所以只能判 STORE
//    （保守）；CLEAR 则任何 rtseq 下都安全：rp_pass 在整屏清除前必定先结束当前通道（rp_pass_clear_color 的
//    rp_pass_end），清除之后的绘制属于下一个通道、它的写回由它自己的 decide 负责。READ / NEED 同理必然在通道外。
//  · 不搬 xv / udc / hoist / nst / defer_pre 那几台 iOS 附属机器：判不到就 STORE。
//  · TEXU / IMGU / TBUFU（常量缓冲里的句柄）前瞻看不到内容：程序带这类绑定时按「全部候选贴图被读」记账。
//  · 条件渲染跳过的命令在索引里（扫描在提交开头做，运行时条件未定）：回放真跳过时整份索引作废，
//    本次提交余下通道全 STORE（rp_sdx_invalidate，exec 循环每条 op 处判）。
//  · 事件类型：READ 绘制 / 派发按程序反射读这张贴图；NEED 拷贝任一端 / 整图清除；ATT 绘制时它是当前附件；
//    CLEAR 整屏清除它是当前附件。首个有效事件决定：READ / NEED / ATT → 要存；CLEAR → 冗余；没有 → 要存。
#include "rp_exec.h"
#include "rp_state.h"
#include "rp_shader.h"
#include "rp_res.h"
#include "nvn_plat.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int nvn_cmd_call_resolve(uint64_t h, uint64_t *to);   // hle_nvn.c（0 照执行，2 跳同设备地址的新录制，<0 跳过）
int rp_prog_is(const prog_t *p, uint64_t serial);     // rp_state.c
void rp_bh_fresh(const p_bind_handles *b, int is_samp);   // rp_state.c：递延绑定把记录里的地址当场取回（原地改写，回放再取同一值）

typedef struct { uint32_t seq, rtseq; uint64_t ser; const uint8_t *op; uint16_t lv, sl; uint8_t kind, plane, anyls, why; } sdx_ev;
enum { SDX_READ = 1, SDX_NEED, SDX_ATT, SDX_CLEAR };
#define SDX_NONE 0xffffffffu
static sdx_ev *g_sdx, *g_sdx_tmp; static uint32_t g_sdx_n, g_sdx_cap; static int g_sdx_ok;
// 候选集（本次提交被当过附件的贴图）→ 桶：开址散列；事件发生时还不是附件的贴图与已结束的通道无关，不记。
#define SDX_HB 2048
static uint64_t g_sdx_hk[SDX_HB]; static uint16_t g_sdx_hb[SDX_HB]; static uint32_t g_sdx_nb, g_sdx_bcnt[SDX_HB + 1], g_sdx_bst[SDX_HB + 1];
static const uint8_t **g_sdx_op; static uint32_t g_sdx_nop, g_sdx_opcap;
static uint32_t g_exec_seq;                            // 回放已数到的命令序号（索引与回放同序，rp_sdx_seq 逐条对）
static uint64_t g_pp_slot[6][RP_NTEX + 32];            // 各阶段的贴图槽（0..RP_NTEX-1），随后 32 个图像槽
static const prog_t *g_pp_prog[6], *g_pp_cprog;        // 扫描时各阶段绑着的程序对象（绘制读谁反射看谁）
static const p_set_rt *g_pp_rt; static p_set_rt g_pp_rt_buf; static uint32_t g_pp_rtseq;
static int g_sdx_enabled = -1, g_fd_enabled = -1;      // -1 未取：各自开关 × 后端 TBDR 能力
static int tbdr(void) {   // 后端能力位：load / store 有真实瓦片内存代价（IMR / 未知 = 0，优化不开）
    nvn_gpu_caps cp; nvn_gpu_caps_get(&cp); return (cp.caps & NVN_GPU_CAP_TILE_MEMORY) != 0;
}

static int sdx_on(void) {
    if (g_sdx_enabled < 0) g_sdx_enabled = nvn_plat_flag("nvk_sdx", 1) && tbdr();
    return g_sdx_enabled;
}
int rp_sdx_on(void) { return sdx_on(); }
// 首笔全画跳载入（rp_draw.c fd_*）同走 TBDR 能力位，独立开关 nvk_fd 便于设备 A/B 分离读数。
int rp_fd_on(void) {
    if (g_fd_enabled < 0) g_fd_enabled = nvn_plat_flag("nvk_fd", 1) && tbdr();
    return g_fd_enabled;
}
// 真机诊断（r2/r3 全零定位）：索引成败、对账、decide 六码的逐条件计数；rp_present 每帧长行带走并清零。
// st: 0 门关 1 建 2 坏记录 3 桶满 4 深度/守卫 5 分配 6 序坏 7 作废 8..13 decide 存{读,再挂,拷,清,否,无} 14 decide 跳
static uint64_t g_sdx_st[16];
static void sdx_abort(int k) { g_sdx_st[k]++; g_sdx_ok = 0; }
int rp_sdx_diag(char *b, size_t n) {   // → 已写字节数；全零不写
    uint64_t t = 0; for (int i = 0; i < 16; i++) t |= g_sdx_st[i]; if (!t) return 0;
    int o = snprintf(b, n, "前瞻因：关%llu 建%llu 坏%llu 桶%llu 深%llu 分%llu 序%llu 作%llu | 存：读%llu 挂%llu 拷%llu 清%llu 否%llu 无%llu | 跳%llu",
        (unsigned long long)g_sdx_st[0], (unsigned long long)g_sdx_st[1], (unsigned long long)g_sdx_st[2], (unsigned long long)g_sdx_st[3],
        (unsigned long long)g_sdx_st[4], (unsigned long long)g_sdx_st[5], (unsigned long long)g_sdx_st[6], (unsigned long long)g_sdx_st[7],
        (unsigned long long)g_sdx_st[8], (unsigned long long)g_sdx_st[9], (unsigned long long)g_sdx_st[10], (unsigned long long)g_sdx_st[11],
        (unsigned long long)g_sdx_st[12], (unsigned long long)g_sdx_st[13], (unsigned long long)g_sdx_st[14]);
    memset(g_sdx_st, 0, sizeof g_sdx_st); return o;
}
static uint64_t h_ser(uint64_t h) {   // 槽句柄 → 贴图序号（0 = 登记里没有 / 解不出）
    tex_t t; return rp_tex_snap_by_id((int)(h & 0xfffff), &t, NULL, NULL) ? t.serial : 0;
}
static int sdx_bucket(uint64_t ser, int add) {
    if (!ser) return -1;
    for (uint32_t i = (uint32_t)(ser * 0x9E3779B97F4A7C15ull >> 53), n = 0; n < SDX_HB; i = (i + 1) & (SDX_HB - 1), n++) {
        if (g_sdx_hk[i] == ser) return g_sdx_hb[i];
        if (!g_sdx_hk[i]) { if (!add) return -1; if (g_sdx_nb >= SDX_HB / 2) { sdx_abort(3); return -1; } g_sdx_hk[i] = ser; g_sdx_hb[i] = (uint16_t)g_sdx_nb; return (int)g_sdx_nb++; }
    }
    return -1;
}
static void sdx_ev_add(sdx_ev e) {
    if (sdx_bucket(e.ser, 0) < 0) return;
    if (g_sdx_n == g_sdx_cap) {
        uint32_t c = g_sdx_cap ? g_sdx_cap * 2 : 4096;
        sdx_ev *a = realloc(g_sdx, (size_t)c * sizeof *g_sdx), *b = realloc(g_sdx_tmp, (size_t)c * sizeof *g_sdx_tmp);
        if (!a || !b) { free(a); free(b); sdx_abort(5); return; }
        g_sdx = a; g_sdx_tmp = b; g_sdx_cap = c;
    }
    g_sdx[g_sdx_n++] = e;
}
static void sdx_lvsl(const view_t *v, int hv, uint16_t *lv, uint16_t *sl) { *lv = (uint16_t)(hv && v->set_levels ? v->base_level : 0); *sl = (uint16_t)(hv && v->set_layers ? v->min_layer : 0); }
static void sdx_att_all(const p_set_rt *r, uint32_t seq, int why) {   // 当前目标的全部附件各记一条 ATT
    for (int k = 0; k < r->n && k < 8; k++) if (r->color[k]) { uint16_t lv, sl; sdx_lvsl(&r->cview[k], r->has_cview[k], &lv, &sl); sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->color[k]->serial, NULL, lv, sl, SDX_ATT, 0, 0, (uint8_t)why}); }
    if (r->depth) { uint16_t lv, sl; sdx_lvsl(&r->dview, r->has_dview, &lv, &sl); sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->depth->serial, NULL, lv, sl, SDX_ATT, 8, !r->has_dview, (uint8_t)why}); }
}
// 本笔绘制 / 派发按程序反射读到的贴图槽各记一条 READ；程序还没编译出来（peek 不中）按「绑着的槽都可能被读」保守记；
// 程序带 TEXU / IMGU / TBUFU（句柄在常量缓冲里，前瞻看不到内容）按「全部候选贴图都可能被读」保守记。
static void sdx_reads(const struct rp_prog *pg, uint32_t seq, int is_compute) {
    if (!pg) {
        for (int st = 0; st < 6; st++) for (int k = 0; k < RP_NTEX + 32; k++) if (g_pp_slot[st][k]) sdx_ev_add((sdx_ev){seq, 0, g_pp_slot[st][k], NULL, 0, 0, SDX_READ, 0, 0, 1});
        return;
    }
    int bindless = 0;
    for (int st = 0; st < RP_PROG_STAGES && !bindless; st++)
        for (uint32_t i = 0; i < pg->nent[st]; i++) {
            uint8_t k = pg->ent[st][i].src_kind;
            if (k == NVN_RB_TEXU || k == NVN_RB_IMGU || k == NVN_RB_TBUFU) { bindless = 1; break; }
        }
    if (bindless) {   // 常量缓冲里的句柄前瞻看不到：候选集里每张都可能被它读
        for (uint32_t b = 0; b < g_sdx_nb; b++) if (g_sdx_hk[b]) sdx_ev_add((sdx_ev){seq, 0, g_sdx_hk[b], NULL, 0, 0, SDX_READ, 0, 0, 1});
        return;
    }
    int nst = is_compute ? 1 : RP_PROG_STAGES, base = is_compute ? 5 : 0;   // 计算程序的 ent 装在 0 号段、槽位取 NVN 阶段 5
    for (int st = 0; st < nst; st++) {
        int nvst = base + st;
        for (uint32_t i = 0; i < pg->nent[st]; i++) {
            const nvn_gpu_layout_ent *e = &pg->ent[st][i];
            int k = e->src_kind == NVN_RB_IMG ? RP_NTEX + e->src_reg : (e->src_kind == NVN_RB_TEX || e->src_kind == NVN_RB_TBUF) ? e->src_reg : -1;
            if (k < 0 || k >= RP_NTEX + 32) continue;
            uint64_t ser = g_pp_slot[nvst][k];
            if (ser) sdx_ev_add((sdx_ev){seq, 0, ser, NULL, 0, 0, SDX_READ, 0, 0, 1});
        }
    }
}
const struct rp_prog *rp_shader_peek(uint64_t ser);   // nvk_shader.c：程序序号 → 已编译的 rp_prog（没编出 = NULL，不触发编译）

static void sdx_walk(const uint8_t *p, int depth) {
    if (depth > 64) { sdx_abort(4); return; }
    uint32_t guard = 0;
    for (; guard < 4000000 && p && g_sdx_ok; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        if (h.size < 8 || (h.size & 7) || h.op >= OP_NVN2_END_ || h.op == OP_BEGIN) { sdx_abort(2); return; }   // 坏记录：索引作废，余下通道全存
        if (g_sdx_nop == g_sdx_opcap) {
            uint32_t c = g_sdx_opcap ? g_sdx_opcap * 2 : 16384;
            const uint8_t **t = realloc(g_sdx_op, (size_t)c * sizeof *g_sdx_op);
            if (!t) { sdx_abort(5); return; }
            g_sdx_op = t; g_sdx_opcap = c;
        }
        uint32_t seq = g_sdx_nop; g_sdx_op[g_sdx_nop++] = p;
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8);
                uint64_t to; int ck = nvn_cmd_call_resolve(t, &to); if (ck == 2) { t = to; ck = 0; }   // 与回放同一判定（#768338）
                if (ck < 0) break;
                sdx_walk((const uint8_t *)(uintptr_t)t, depth + 1); break; }
            case OP_SET_RT: { const p_set_rt *r = set_rt_rec(a, h.size, &g_pp_rt_buf); g_pp_rt = r; g_pp_rtseq = seq;
                for (int k = 0; k < r->n && k < 8; k++) if (r->color[k]) sdx_bucket(r->color[k]->serial, 1);
                if (r->depth) sdx_bucket(r->depth->serial, 1);
                break; }
            case OP_CLEAR_COLOR: { const p_clear_color *c = a; const p_set_rt *r = g_pp_rt;
                if (!r || (c->mask & 0xf) != 0xf || c->index < 0 || c->index >= r->n || c->index >= 8 || !r->color[c->index]) break;
                uint16_t lv, sl; sdx_lvsl(&r->cview[c->index], r->has_cview[c->index], &lv, &sl); int last = -1;   // 同一子资源挂在多个槽：记最后那个
                for (int k = 0; k < r->n && k < 8; k++) { uint16_t l2, s2; if (!r->color[k] || r->color[k]->serial != r->color[c->index]->serial) continue; sdx_lvsl(&r->cview[k], r->has_cview[k], &l2, &s2); if (l2 == lv && s2 == sl) last = k; }
                if (last == c->index) sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->color[c->index]->serial, p, lv, sl, SDX_CLEAR, 0, 0, 0});
                break; }
            case OP_CLEAR_DS: { const p_clear_ds *c = a; const p_set_rt *r = g_pp_rt; if (!r || !r->depth) break;
                uint16_t lv, sl; sdx_lvsl(&r->dview, r->has_dview, &lv, &sl);
                if (c->depth_mask) sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->depth->serial, p, lv, sl, SDX_CLEAR, 1, !r->has_dview, 0});
                if ((c->stencil_mask & 0xff) == 0xff) sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->depth->serial, p, lv, sl, SDX_CLEAR, 2, !r->has_dview, 0});
                break; }
            case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_IMG: { p_bind_handles t1; const p_bind_handles *b = bind_rec(h.op, a, &t1); rp_bh_fresh(b, 0);
                int base = h.op == OP_BIND_IMG ? RP_NTEX : 0, lim = h.op == OP_BIND_IMG ? 32 : RP_NTEX;
                if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) g_pp_slot[b->stage][base + b->first + k] = h_ser(b->h[k]);
                break; }
            case OP_BIND_PROGRAM: { p_bind_program t; const p_bind_program *bp = bind_program_rec(a, h.size, &t);
                const prog_t *pg = rp_prog_is(bp->prog, bp->snap.serial) ? bp->prog : NULL;
                if (bp->stages & 0x20) g_pp_cprog = pg;
                for (int st = 0; st < 5; st++) if (bp->stages >> st & 1) g_pp_prog[st] = pg;
                break; }
            case OP_DRAW: case OP_DRAW_IDX: case OP_MULTI_DRAW: case OP_DISPATCH: {
                const struct rp_prog *pg = NULL;
                if (h.op == OP_DISPATCH) {
                    pg = g_pp_cprog ? rp_shader_peek(g_pp_cprog->serial) : NULL;
                } else {
                    // 逐阶段 peek：任一在绑阶段的程序没编译出来，那个阶段读什么判不到 → 全槽读（同 iOS 程序未载的保守分支）。
                    // 细分阶段（3 / 4）的反射 rp_prog 不载：绑了就当读不到，一样全槽读。
                    int miss = 0;
                    for (int st = 0; st < 5; st++) if (g_pp_prog[st]) {
                        const struct rp_prog *q = rp_shader_peek(g_pp_prog[st]->serial);
                        if (!q) { miss = 1; break; }
                        if (!pg) pg = q;   // 任一阶段的程序都能给出整组 ent（编译时全部在绑阶段一起进）
                    }
                    if (miss) pg = NULL;
                }
                if (!pg) { for (int st = 0; st < 6; st++) for (int k = 0; k < RP_NTEX + 32; k++) if (g_pp_slot[st][k]) sdx_ev_add((sdx_ev){seq, 0, g_pp_slot[st][k], NULL, 0, 0, SDX_READ, 0, 0, 1}); }
                else sdx_reads(pg, seq, h.op == OP_DISPATCH);
                if (h.op != OP_DISPATCH && g_pp_rt) sdx_att_all(g_pp_rt, seq, 2);
                break; }
            case OP_DRAW_TEX: { const p_draw_tex *t = a;
                uint64_t ser = h_ser(t->handle); if (ser) sdx_ev_add((sdx_ev){seq, 0, ser, NULL, 0, 0, SDX_READ, 0, 0, 1});
                if (g_pp_rt) sdx_att_all(g_pp_rt, seq, 0);
                break; }
            case OP_COPY_T2T: { const p_copy_t2t *c = a; if (c->src) sdx_ev_add((sdx_ev){seq, 0, c->src->serial, p, 0, 0, SDX_NEED, 0, 0, 3}); if (c->dst) sdx_ev_add((sdx_ev){seq, 0, c->dst->serial, p, 0, 0, SDX_NEED, 0, 0, 3}); break; }
            case OP_COPY_T2B: case OP_COPY_B2T: { const p_copy_b2t *c = a; if (c->tex) sdx_ev_add((sdx_ev){seq, 0, c->tex->serial, p, 0, 0, SDX_NEED, 0, 0, 3}); break; }
            case OP_CLEAR_TEX: { const p_clear_tex *c = a; if (c->tex) sdx_ev_add((sdx_ev){seq, 0, c->tex->serial, p, 0, 0, SDX_NEED, 0, 0, 4}); break; }
            default: break;
        }
        p += h.size;
    }
    if (guard >= 4000000) sdx_abort(4);
}
void nvn_be_sub_index(const uint64_t *h, int n) {   // 提交开头（hle_nvn.c QueueSubmitCommands）：建本次提交的存储前瞻索引
    g_sdx_n = g_sdx_nop = 0; g_exec_seq = 0; g_sdx_ok = sdx_on();
    rp_pass_rtseq_reset();   // 序号按提交重数：上次留下的旧 rtseq 会把这次的事件全吞掉
    if (!g_sdx_ok) { g_sdx_st[0]++; return; }
    memset(g_sdx_hk, 0, sizeof g_sdx_hk); g_sdx_nb = 0;
    const p_set_rt *live = rp_pass_target();
    if (live) { for (int k = 0; k < live->n && k < 8; k++) if (live->color[k]) sdx_bucket(live->color[k]->serial, 1); if (live->depth) sdx_bucket(live->depth->serial, 1); }   // 提交开头还挂着的目标
    memset(g_pp_slot, 0, sizeof g_pp_slot); memset(g_pp_prog, 0, sizeof g_pp_prog); g_pp_cprog = NULL; g_pp_rt = NULL; g_pp_rtseq = SDX_NONE;
    for (int st = 0; st < 6; st++) {
        for (int k = 0; k < RP_NTEX; k++) g_pp_slot[st][k] = rp_S.tex[st][k] ? h_ser(rp_S.tex[st][k]) : 0;
        for (int k = 0; k < 32; k++) g_pp_slot[st][RP_NTEX + k] = rp_S.img[st][k] ? h_ser(rp_S.img[st][k]) : 0;
        g_pp_prog[st] = st < 5 ? rp_S.bst[st] : NULL;
    }
    g_pp_cprog = rp_S.cprog;
    for (int i = 0; i < n && g_sdx_ok; i++) sdx_walk((const uint8_t *)(uintptr_t)h[i], 0);
    if (!g_sdx_ok) return;
    // 计数排序进桶：事件按 seq 追加，同 seq 里 READ 在 ATT 前（同一笔先判读后判附件），稳定散列后每桶自然有序。
    memset(g_sdx_bcnt, 0, (g_sdx_nb + 1) * sizeof *g_sdx_bcnt);
    for (uint32_t i = 0; i < g_sdx_n; i++) g_sdx_bcnt[sdx_bucket(g_sdx[i].ser, 0)]++;
    for (uint32_t b = 0, acc = 0; b <= g_sdx_nb; b++) { g_sdx_bst[b] = acc; acc += b < g_sdx_nb ? g_sdx_bcnt[b] : 0; }
    for (uint32_t b = 0; b < g_sdx_nb; b++) g_sdx_bcnt[b] = g_sdx_bst[b];
    for (uint32_t i = 0; i < g_sdx_n; i++) g_sdx_tmp[g_sdx_bcnt[sdx_bucket(g_sdx[i].ser, 0)]++] = g_sdx[i];
    sdx_ev *t = g_sdx; g_sdx = g_sdx_tmp; g_sdx_tmp = t;
    g_sdx_st[1]++;
}
void nvn_be_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }   // 前瞻游标（fz_cursor）这台机器这里不搬：索引已盖整次提交

void rp_sdx_seq(const uint8_t *p) {   // 回放每条命令开头打一次：序号对不上索引就作废，余下通道全存
    if (!g_sdx_ok) return;
    if (g_exec_seq >= g_sdx_nop || g_sdx_op[g_exec_seq] != p) {
        static unsigned said; if (said++ < 8) nvn_plat_log(NVN_PLAT_LOG_INFO, "sdx", "存储前瞻索引与回放顺序对不上：索引作废，本次提交余下通道全存");
        g_sdx_st[6]++; g_sdx_ok = 0; return;
    }
    g_exec_seq++;
}
void rp_sdx_invalidate(void) { if (g_sdx_ok) { g_sdx_st[7]++; g_sdx_ok = 0; } }   // 条件渲染真跳过了命令：索引里有它们、回放里没有，作废（rp_exec 每条 op 处判）
uint32_t rp_sdx_cur(void) { return g_exec_seq ? g_exec_seq - 1 : SDX_NONE; }   // 正在回放的那条的序号（rp_pass_set_rt 记目标的 rtseq 用）
void rp_sdx_reset(void) { g_sdx_ok = 0; g_sdx_n = g_sdx_nop = g_exec_seq = 0; }

// 通道 begin 时的单附件判定：ser/lv/sl 是附件子资源，plane 0 颜色 / 1 深度 / 2 模板，rt0seq 是当前目标的 SET_RT 序号。
// 返回 iOS 的 st 码：2 = 冗余（可 DONTCARE）；其他 = why<<2 要存（1 被读 2 重新挂上再画 3 拷贝端 4 整图清除 5 判不到 6 索引不可用）。
static int sdx_decide_(uint64_t ser, int lv, int sl, int plane, uint32_t rt0seq) {
    if (!g_sdx_ok || !g_exec_seq) return 6 << 2;
    const uint32_t P = g_exec_seq;   // 正在回放的绘制之后的第一条
    int b = sdx_bucket(ser, 0); if (b < 0) return 5 << 2;   // 没进候选集：本次提交与它无关的事件一条没有 → 判不到，存
    uint32_t lo = g_sdx_bst[b], end = g_sdx_bst[b + 1], hi = end;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (g_sdx[m].seq < P) lo = m + 1; else hi = m; }   // 首个 seq >= P 的事件
    for (uint32_t j = lo; j < end; j++) {
        const sdx_ev *e = &g_sdx[j];
        if (e->kind == SDX_READ || e->kind == SDX_NEED) return e->why << 2;
        if (e->rtseq == SDX_NONE || e->rtseq <= rt0seq) continue;   // 本通道自己的事件（同一份目标设置）：不算
        int hit = plane ? (e->kind == SDX_ATT ? e->plane == 8 : e->plane == plane) && (e->anyls || (e->lv == lv && e->sl == sl))
                        : e->plane == 0 && e->lv == lv && e->sl == sl;
        if (!hit) continue;
        return e->kind == SDX_CLEAR ? 2 : 2 << 2;
    }
    return 5 << 2;
}
int rp_sdx_decide(uint64_t ser, int lv, int sl, int plane, uint32_t rt0seq) {
    int rc = sdx_decide_(ser, lv, sl, plane, rt0seq);
    if (rc == 2) g_sdx_st[14]++; else { int w = rc >> 2; if (w >= 1 && w <= 6) g_sdx_st[7 + w]++; }
    return rc;
}
