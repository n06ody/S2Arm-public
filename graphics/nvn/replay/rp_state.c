// 状态类命令的落地（对齐 nvn_mtl.m state_op @10374、state_reset @7222、bind_stages @7177、bb_fresh / bh_fresh @6478）。
#include "rp_state.h"
#include "rp_shader.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include <string.h>

struct rp_state rp_S;
uint64_t rp_tex_sep_gen[6];
struct rp_target rp_R;
uint8_t rp_pso_dirty, rp_dss_dirty, rp_args_dirty[6];
static uint64_t g_push_seq;

int nvn_dpool_base(const void *pool, int tex);   // hle_nvn.c：描述符池的编号在宿主表里的起点

static void args_dirty_all(void) { memset(rp_args_dirty, 1, sizeof rp_args_dirty); }
void rp_dirty_all(void) { rp_pso_dirty = rp_dss_dirty = 1; args_dirty_all(); }

// 着色器来源消费的「当前有效图形阶段」与各阶段的键：OP_BIND_PROGRAM 按阶段窄化后的结果。
uint32_t rp_shader_graphics_stages(void) {
    uint32_t m = 0;
    for (int st = 0; st < 5; st++) if (rp_S.bser[st]) m |= 1u << st;
    return m;
}
int rp_shader_stage_key(unsigned st, uint8_t key[8]) {
    if (st > 4 || !rp_S.bser[st] || !(rp_S.skey_ok >> st & 1)) return 0;
    if (key) memcpy(key, rp_S.skey[st], 8);
    return 1;
}

void rp_state_reset(void) {
    memset(&rp_S, 0, sizeof rp_S); memset(&rp_R, 0, sizeof rp_R); for (int i = 0; i < 6; i++) rp_tex_sep_gen[i]++;
    rp_S.tex_diag_enabled = !!nvn_plat_flag("nvk_fact_probe", 0);
    for (int i = 0; i < 8; i++) rp_S.blend[i] = (blend_st){.target = (unsigned)i, .src_rgb = NVN_BLEND_FUNC_ONE, .dst_rgb = NVN_BLEND_FUNC_ZERO, .src_a = NVN_BLEND_FUNC_ONE, .dst_a = NVN_BLEND_FUNC_ZERO, .eq_rgb = NVN_BLEND_EQUATION_ADD, .eq_a = NVN_BLEND_EQUATION_ADD};
    rp_S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; rp_S.chmask.mask = 0xffffffffu;
    rp_S.ms.enable = 1;
    rp_S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    rp_S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    rp_S.patch_size = 3;
    rp_S.st_vmask[0] = rp_S.st_vmask[1] = rp_S.st_wmask[0] = rp_S.st_wmask[1] = 0xff; rp_S.dr[1] = 1;
    rp_dirty_all();
}

// 延后绑定：记录里存的是游戏内存地址，回放到这条时才读。
static const p_bind_buf *bb_fresh(const void *a, p_bind_buf *t) {
    const p_bind_buf *b = a; if (!(b->stage & BIND_BUF_DEFER)) return b;
    const NVNbufferRange *r = (const NVNbufferRange *)(uintptr_t)b->addr;
    *t = (p_bind_buf){b->stage & ~BIND_BUF_DEFER, b->index, r ? nvn_ga2ca(r->address) : 0, r ? r->size : 0}; return t;
}
static void bh_fresh(const p_bind_handles *b, int is_samp) {
    if (b->defer && b->src && b->n > 0 && b->n <= 32) {
        p_bind_pool_bases bases; memcpy(&bases, b->h + b->n, sizeof bases);
        uint64_t *h = (uint64_t *)b->h;
        memcpy(h, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n);
        for (int i = 0; i < b->n; i++) {
            uint64_t tid = h[i] & 0xfffff, sid = h[i] >> 20;
            h[i] = (tid + (uint64_t)bases.tbase) | ((sid || is_samp) ? sid + (uint64_t)bases.sbase : 0) << 20;
        }
    }
}

static int prog_is(const prog_t *p, uint64_t serial) { return p && p->magic == MAGIC('P', 'R', 'O', 'G') && p->serial == serial; }

// 程序对象在绑定之后可能被游戏 Finalize 或复用（nvn_mtl.m:7150 一带的两次崩溃）：之后只认绑定当时的序号 bser，
// bst 只在对象仍是它时非空。bser 为 0 = 该阶段没有绑定。
// iOS 还按「这个程序实际有哪些阶段的着色器」收窄顶点 / 片元位（nvn_mtl.m:7186）；那要等着色器解析接上（M1），
// 在此之前阶段位照单全收。
static void stage_sweep(void) {
    for (int st = 0; st < 5; st++) if (rp_S.bst[st] && !prog_is(rp_S.bst[st], rp_S.bser[st])) rp_S.bst[st] = NULL;
    if (rp_S.cprog && !prog_is(rp_S.cprog, rp_S.cser)) rp_S.cprog = NULL;
}

int rp_state_op(uint32_t op, const void *a, uint32_t size) {
    switch (op) {
        case OP_SCISSOR: rp_R.sc = *(const p_scissor *)a; rp_R.has_sc = 1; return 1;
        case OP_PUSH_CONST: case OP_PUSH_CONST_PTR: { const p_push_const *pc = a; int c = pc->compute ? 1 : 0;
            const void *src = op == OP_PUSH_CONST ? (const void *)(pc + 1) : (const void *)(uintptr_t)((const p_push_const_ptr *)a)->data;
            if (pc->offset + pc->size > NVN2_PUSH_BYTES || !src) return 1;
            memcpy(rp_S.push[c][pc->slot & (NVN2_PUSH_SLOTS - 1)] + pc->offset, src, pc->size); rp_S.push_ver[c][pc->slot & (NVN2_PUSH_SLOTS - 1)] = ++g_push_seq;
            if (c) rp_args_dirty[5] = 1; else memset(rp_args_dirty, 1, 5);
            return 1; }
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: { int tex = op == OP_SET_TEX_POOL, b = nvn_dpool_base(((const p_set_pool *)a)->pool, tex);
            if (b != (tex ? rp_S.tex_base : rp_S.samp_base)) { if (tex) rp_S.tex_base = b; else rp_S.samp_base = b; args_dirty_all(); }
            { const dpool_t *dp = ((const p_set_pool *)a)->pool; int n = dp ? dp->n : 0;
              if (tex) { if (rp_S.tex_pool_n != n) { rp_S.tex_pool_n = n; rp_pso_dirty = 1; } } else if (rp_S.samp_pool_n != n) { rp_S.samp_pool_n = n; rp_pso_dirty = 1; } }   // 池容量是着色器编译事实
            return 1; }
        case OP_BIND_PROGRAM: { p_bind_program t; const p_bind_program *bp = bind_program_rec(a, size, &t);
            const prog_t *p = prog_is(bp->prog, bp->snap.serial) ? bp->prog : NULL; uint64_t ser = bp->prog ? bp->snap.serial : 0;
            // 程序实际有哪些阶段的着色器：快照的 stage_of[]（同旧 vk_record.c 的取法），nstage 是着色器项数。
            // 取不到（快照缺 / 有认不出的阶段）时 known=0，沿用旧规则（本次掩码照单全收），留给绘制时按名字报。
            uint32_t prog_st = 0; int prog_known = 0;
            if (bp->snap.nstage > 0) {
                uint32_t m = 0; int ok = 1;
                for (uint32_t i = 0; i < bp->snap.nstage && i < 6u; i++) {
                    unsigned st = bp->stage_of[i];
                    if (st > 5) { ok = 0; break; }
                    m |= 1u << st;
                }
                if (ok) { prog_st = m; prog_known = 1; }
            }
            // 各阶段的着色器键：记录里的 keys 按程序的着色器数组序，stage_of 给出每一项是哪个 NVN 阶段
            for (int i = 0; i < 6; i++) { unsigned st = bp->stage_of[i]; if (st > 5 || !(bp->stages >> st & 1)) continue;
                if (bp->kok >> i & 1) { memcpy(rp_S.skey[st], bp->keys[i], 8); rp_S.skey_ok |= (uint8_t)(1u << st); } else rp_S.skey_ok &= (uint8_t)~(1u << st); }
            for (int st = 0; st < 6; st++) if ((bp->stages >> st & 1) && !ser) rp_S.skey_ok &= (uint8_t)~(1u << st);   // 解绑
            if (bp->stages & 0x20) { if (rp_S.cser != ser) args_dirty_all(); rp_S.cprog = p; rp_S.cser = ser; }
            if (bp->stages & 0x1f) {
                int ch = 0;
                for (int st = 0; st < 5; st++) {
                    if (!(bp->stages >> st & 1)) continue;   // 本次掩码没覆盖的阶段：保留原绑定（NVN 按阶段绑定）
                    if (prog_known && !(prog_st >> st & 1)) {
                        if (st < 2) continue;   // 顶点 / 片元不能被关：程序没有就保留原绑定（同 iOS bind_stages，太鼓达人）
                        if (rp_S.bser[st]) ch = 1;   // 几何 / 细分：程序没有就解绑旧程序（同 iOS，异度 3）
                        rp_S.bst[st] = NULL; rp_S.bser[st] = 0; rp_S.skey_ok &= (uint8_t)~(1u << st);
                        continue;
                    }
                    if (rp_S.bser[st] != ser) ch = 1;
                    rp_S.bst[st] = p; rp_S.bser[st] = ser;
                }
                if (ch) { rp_pso_dirty = 1; args_dirty_all(); }
            }
            stage_sweep();
            return 1; }
        case OP_BIND_STATE: {
            const p_bind_state *b = a; void *dst = NULL; size_t n = 0; blend_st bs;
            switch (b->kind) {
                case 0: memcpy(&bs, b->bytes, sizeof bs); dst = &rp_S.blend[bs.target]; n = sizeof bs; break;
                case 1: dst = &rp_S.color; n = sizeof rp_S.color; break;
                case 2: dst = &rp_S.chmask; n = sizeof rp_S.chmask; break;
                case 3: dst = &rp_S.ms; n = sizeof rp_S.ms; break;
                case 4: dst = &rp_S.poly; n = sizeof rp_S.poly; break;
                case 5: dst = &rp_S.ds; n = sizeof rp_S.ds; break;
            }
            if (dst && memcmp(dst, b->bytes, n)) { memcpy(dst, b->bytes, n); if (b->kind == 5) rp_dss_dirty = 1; else rp_pso_dirty = 1; }
            return 1; }
        case OP_BIND_VATTR: {
            const p_bind_vattr *b = a; va_st at[16]; if (b->n < 0 || b->n > 16) return 1;
            memcpy(at, b->st, 4 * (size_t)b->n); for (int i = 0; i < b->n; i++) at[i].fmt = nvn_vattr_fmt_canon(at[i].fmt);
            if (rp_S.nattr != b->n || memcmp(rp_S.attr, at, 4 * (size_t)b->n)) { memcpy(rp_S.attr, at, 4 * (size_t)b->n); rp_S.nattr = b->n; rp_pso_dirty = 1; }
            return 1; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; if (b->n < 0 || b->n > 16) return 1;
            if (rp_S.nstream != b->n || memcmp(rp_S.stream, b->st, 8 * (size_t)b->n)) { memcpy(rp_S.stream, b->st, 8 * (size_t)b->n); rp_S.nstream = b->n; rp_pso_dirty = 1; }
            return 1; }
        case OP_BIND_VBUF: { p_bind_vbuf t; const p_bind_vbuf *b = bind_vbuf_rec(a, size, &t);
            if (b->first >= 0 && b->n >= 0 && b->first + b->n <= 16) memcpy(&rp_S.vbuf[b->first], b->r, sizeof *b->r * (size_t)b->n);
            return 1; }
        case OP_BIND_UBO: case OP_BIND_SSBO: { p_bind_buf bt; const p_bind_buf *b = bb_fresh(a, &bt);
            if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) {
                p_bind_buf *d = op == OP_BIND_UBO ? &rp_S.ubo[b->stage][b->index] : &rp_S.ssbo[b->stage][b->index];
                if (memcmp(d, b, sizeof *b)) { *d = *b; rp_args_dirty[b->stage] = 1; }
            }
            return 1; }
        case OP_BIND_TEX: case OP_BIND_TEX1: { p_bind_handles t1; const p_bind_handles *b = bind_rec(op, a, &t1);
            if (b->n < 0 || b->n > 32) { nvn_unimpl("rec", "bind_tex_count_overwritten", a); return 1; }
            bh_fresh(b, op == OP_BIND_SAMP);
            if (rp_S.tex_diag_enabled && b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= RP_NTEX)
                for (int i = 0; i < b->n; i++) if (b->first + i >= 128)
                    rp_S.tex_diag_written[b->stage][b->first + i - 128] = 1;
            if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= RP_NTEX && memcmp(&rp_S.tex[b->stage][b->first], b->h, 8 * (size_t)b->n)) {
                memcpy(&rp_S.tex[b->stage][b->first], b->h, 8 * (size_t)b->n); rp_args_dirty[b->stage] = 1;
                if (b->first + b->n > 128) rp_tex_sep_gen[b->stage]++; }   // 分离贴图槽（128 起）变了
            return 1; }
        case OP_BIND_SAMP: { const p_bind_handles *b = a; if (b->n < 0 || b->n > 32) return 1; bh_fresh(b, 1);
            if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) {
                for (int i = 0; i < b->n; ++i) {
                    int slot = b->first + i;
                    if (!rp_S.samp_bound[b->stage][slot] || rp_S.samp[b->stage][slot] != b->h[i]) rp_args_dirty[b->stage] = 1;
                    rp_S.samp_bound[b->stage][slot] = 1; rp_S.samp[b->stage][slot] = b->h[i];
                }
            }
            return 1; }
        case OP_BIND_IMG: { const p_bind_handles *b = a; if (b->n < 0 || b->n > 32) return 1; bh_fresh(b, op == OP_BIND_SAMP);
            if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) {
                if (memcmp(&rp_S.img[b->stage][b->first], b->h, 8 * (size_t)b->n)) { memcpy(&rp_S.img[b->stage][b->first], b->h, 8 * (size_t)b->n); rp_args_dirty[b->stage] = 1; }
            } else nvn_unimpl("rec", "bind_img_out_of_range", a);
            return 1; }
        case OP_STENCIL: { const p_stencil *b = a;
            if (b->which != 2) rp_dss_dirty = 1;
            for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) rp_S.st_vmask[f] = (uint32_t)b->value_mask; else if (b->which == 1) rp_S.st_wmask[f] = (uint32_t)b->mask; else rp_S.st_ref[f] = (uint32_t)b->ref; }
            return 1; }
        case OP_POLY_OFFSET: rp_S.po = *(const p_poly_offset *)a; return 1;
        case OP_VIEWPORTS: { const p_viewports *b = a; if (b->first < 0 || b->n < 0 || b->first + b->n > 16) return 1;
            for (int i = 0; i < b->n; i++) { memcpy(rp_S.vp[b->first + i], b->v[i], 16); }
            rp_S.has_vp = 1; return 1; }
        case OP_DEPTH_RANGE: { const p_depth_range *b = a; rp_S.dr[0] = b->n; rp_S.dr[1] = b->f; return 1; }
        case OP_DEPTH_CLAMP: { int on = ((const p_depth_clamp *)a)->on; if ((on != 0) != (rp_S.depth_clamp != 0)) rp_pso_dirty = 1;   // 原生接口里深度夹取是管线状态
            rp_S.depth_clamp = on; return 1; }
        case OP_VP_SWIZZLE: { const p_vp_swizzle *b = a; if (b->first < 0 || b->n < 0 || b->first + b->n > 16) return 1;
            memcpy(rp_S.vp_sw[b->first], b->sw, 4 * (size_t)b->n); return 1; }
        case OP_BLEND_COLOR: memcpy(rp_S.blend_color, ((const p_blend_color *)a)->rgba, 16); return 1;
        case OP_TF_BIND: { const p_tf_bind *b = a; if (b->index >= 0 && b->index < 4) rp_S.tf[b->index] = *b; return 1; }
        case OP_TF_STATE: { const p_tf_state *b = a;
            if (b->what == 0) { rp_S.tf_on = 1; rp_S.tf_ctl = b->ctl; memset(rp_S.tf_off, 0, sizeof rp_S.tf_off); } else rp_S.tf_on = b->what == 3;
            return 1; }
        case OP_PRIM_RESTART: rp_S.restart = *(const p_prim_restart *)a; return 1;
        case OP_ALPHA_REF: { float r = ((const p_alpha_ref *)a)->ref;   // 透明测试开着时参考值是着色器编译事实：变了要重新取程序
            if (r != rp_S.alpha_ref) { rp_S.alpha_ref = r; if (rp_S.color.alpha_func >= 1 && rp_S.color.alpha_func <= 7) rp_pso_dirty = 1; }
            return 1; }
        case OP_SET_PATCH_SIZE: { int sz = ((const p_set_patch_size *)a)->size; if (sz > 0 && sz <= 32) rp_S.patch_size = sz; return 1; }
        case OP_RAST_DISCARD: rp_S.rast_discard = ((const p_rast_discard *)a)->on; return 1;
        default: return 0;
    }
}
