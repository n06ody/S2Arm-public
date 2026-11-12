// 着色器来源（graphics/nvn/replay/rp_shader.h 的实现）：回放状态 → 编译事实 → 内容身份 →
//   包内离线库 / 跨局缓存 / 现场编译（C# 翻译器）→ 解析成每阶段 SPIR-V + 绑定表 → 后端着色器。
// 事实与身份的口径沿用保留零件 nvn/nvn_vk_compile_facts.c（它的入参是旧回放器的绘制快照，这里直接从 rp_S 填同一组事实）。
// 现场编译现在在提交线程上同步做（首次遇到一组着色器会卡这一帧）；后台编 + 首用等待策略随后接。
#include "rp_shader.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_exec.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "nvn_refl.h"
#include "nvn_fmt_info.h"
#include "nvn_hmap.h"
#include "direct/compile_request.h"
#include "direct/input_identity.h"
#include "direct/source_store.h"
#include "direct/texture_facts.h"
#include "direct/offline_library.h"
#include "direct/program_cache.h"
#include "direct/sha256.h"
#include "nvn/nvn_vk_program_result.h"
#include "nvn/nvn_vk_compute_request.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct entry { struct rp_prog prog; int failed; };
static nvn_hmap g_progs = {.klen = 32};   // 内容身份（SHA-256）→ struct entry *
static struct { uint64_t session, generation; uint8_t sha[32]; int ok; } g_profile;
static uint64_t g_n_offline, g_n_cache, g_n_compiled, g_n_failed;
static int g_texpool_logged, g_texpool_last_over_limit;

static void sha256(const void *p, size_t n, uint8_t out[32]) { s2a_sha256 c; s2a_sha256_init(&c); s2a_sha256_update(&c, p, n); s2a_sha256_final(&c, out); }
static void red1(const char *name, const char *why) { if (nvn_unimpl("shader", name, NULL) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "shader", why); }

// 编译器 DirectProgram 的 program.textureArrayLength 合同：1..65536。
// 不修改池值；零/负数是没有有效绑定，超限的真实值必须留在错误里。
static int shader_texture_pool_ready(void) {
    const int n = rp_S.tex_pool_n;
    if (n > 0 && !g_texpool_logged) {
        g_texpool_logged = 1;
        if (nvn_plat_flag("nvk_texpool_log", 0)) {
            char line[160];
            snprintf(line, sizeof line, "texture_pool_capacity=%d sampler_pool_capacity=%d", n, rp_S.samp_pool_n);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", line);
        }
    }
    if (n <= 0) {
        char why[96]; snprintf(why, sizeof why, "贴图池未绑定或容量无效：%d", n);
        red1("texture_pool_missing", why);
        return 0;
    }
    if (n > 65536) {
        uint64_t count = nvn_unimpl("shader", "texture_pool_over_limit", NULL);
        if (count == 1 || g_texpool_last_over_limit != n) {
            char why[128]; snprintf(why, sizeof why, "贴图池容量 %d 超过编译器上限 65536", n);
            nvn_plat_log(NVN_PLAT_LOG_RED, "shader", why);
            g_texpool_last_over_limit = n;
        }
        return 0;
    }
    return 1;
}

// ---- 事实换算（同 nvn_vk_compile_facts.c）----
static const char *topology(uint32_t prim) {
    switch (prim) {
        case 0x0: return "Points";
        case 0x1: case 0x2: case 0x3: return "Lines";
        case 0xA: case 0xB: return "LinesAdjacency";
        case 0x4: case 0x5: case 0x6: case 0x7: case 0x8: case 0x9: case 0xE: return "Triangles";
        case 0xC: case 0xD: return "TrianglesAdjacency";
        default: return NULL;
    }
}
// 顶点属性 / 颜色附件的数值类：整数格式按 Uint / Sint，其余（归一化、浮点、打包、未绑定）都是 Float。
// 带「整数存储、取样时转 float」的顶点格式（I2F / UI2F，0x51–0x68、0x6B、0x6C）是 Float。
static const char *num_type(uint32_t fmt, int vertex) {
    if (!fmt) return "Float";
    if (vertex && ((fmt >= 0x51 && fmt <= 0x68) || fmt == 0x6B || fmt == 0x6C)) return "Float";
    nvn_fmt_info fi;
    if (!nvn_fmt_info_of(fmt, &fi)) return NULL;
    return fi.kind == NVN_FMT_UINT ? "Uint" : fi.kind == NVN_FMT_SINT ? "Sint" : "Float";
}
static const char *alpha_op(uint32_t func) {
    static const char *const names[] = {NULL, "Never", "Less", "Equal", "LessOrEqual", "Greater", "NotEqual", "GreaterOrEqual", "Always"};
    return func >= 1 && func <= 8 ? names[func] : "Always";
}
static int src1(unsigned f) { return f >= 0x10 && f <= 0x13; }

struct facts {
    struct s2a_compile_graphics_src gfx;
    struct s2a_compile_texture_src tex[64]; int ntex;
    char tfmt[64][S2A_TEX_FORMAT_MAX], tsmp[64][S2A_TEX_SAMPLER_MAX];
    uint32_t ubo_use[6];
};

static void fill_facts(const struct rp_shader_req *q, const struct s2a_direct_compile_context *pc, struct facts *f, int nst) {
    memset(f, 0, sizeof *f);
    struct s2a_compile_graphics_src *g = &f->gfx;
    g->topology = topology(q->prim);
    g->rt_count = q->rt_count;
    g->has_alpha_test = 1; g->alpha_test_compare = alpha_op(rp_S.color.alpha_func);
    g->has_alpha_to_coverage = 1; g->alpha_to_coverage_enable = rp_S.ms.a2c ? 1 : 0; g->alpha_to_coverage_dither = rp_S.ms.a2c_dither ? 1 : 0;
    g->has_blend = 1;
    for (int i = 0; i < 8; i++) if (src1(rp_S.blend[i].src_rgb) || src1(rp_S.blend[i].dst_rgb) || src1(rp_S.blend[i].src_a) || src1(rp_S.blend[i].dst_a)) g->dual_source_blend = 1;
    g->has_depth_mode = 1; g->depth_mode = !rp_depth_zero();
    g->has_y_negate = 1; g->y_negate = rp_origin_lower_left();
    g->has_origin = 1; g->origin_upper_left = 1;
    g->has_halve_primitive_id = 1; g->halve_primitive_id = g->topology && q->prim == 0x7u;
    g->has_cb_draw_params = 1; g->cb_draw_params = pc->shader_draw_parameters;
    g->has_point_size = 1; g->program_point_size_enable = 1; g->point_size = 1.0f;
    // 参考值是绘制时的状态（OP_ALPHA_REF）。测试没开（函数是 ALWAYS）时固定给 0：不让同一个程序因为一个用不到的数拆成多份
    g->has_alpha_test_reference = 1; g->alpha_test_reference = rp_S.color.alpha_func >= 1 && rp_S.color.alpha_func <= 7 ? rp_S.alpha_ref : 0.0f;
    int any_ssbo = 0, unaligned = 0;
    for (int st = 0; st < 5; st++) for (int i = 0; i < 16; i++) if (rp_S.ssbo[st][i].addr) { any_ssbo = 1; if (pc->storage_buffer_offset_alignment && rp_S.ssbo[st][i].addr % pc->storage_buffer_offset_alignment) unaligned = 1; }
    if (!any_ssbo || pc->storage_buffer_offset_alignment) { g->has_unaligned_ssbo = 1; g->unaligned_ssbo = unaligned; }
    for (int i = 0; i < 32; i++) g->attribute_types[i] = num_type(i < rp_S.nattr && i < 16 ? rp_S.attr[i].fmt : 0, 1);
    for (int i = 0; i < 8; i++) g->fragment_output_types[i] = i < q->rt_count && q->rt_format[i] ? num_type(q->rt_format[i], 0) : "Float";
    for (int st = 0; st < 6; st++) { uint32_t m = 0; for (int i = 0; i < 16; i++) if (rp_S.ubo[st][i].size) m |= 1u << i; f->ubo_use[st] = m << 3; }   // 用户常量缓冲从 c3 起
    // 组合贴图槽事实：顶点、片元各 32 槽。槽里是「贴图号 | 采样器号 << 20」；没绑过的槽读作池里的第 0 项。
    for (int st = 0; st < nst && f->ntex < 64; st++) for (int slot = 0; slot < 32 && f->ntex < 64; slot++) {
        uint64_t h = rp_S.tex[st][slot]; long id = h ? (long)(h & 0xFFFFF) : (long)rp_S.tex_base;
        tex_t t; view_t tv; int has_tv = 0; struct s2a_nvn_texture_fact nf; char why[96];
        if (rp_tex_snap_by_id((int)id, &t, &tv, &has_tv)) {
            // 格式、目标都取绑定视图的（视图自己设了就用它的）：声明的采样数值型要和绑上去的视图一致，坐标归一与否只看有效目标
            if (s2a_nvn_texture_fact((uint32_t)(has_tv && tv.set_format ? tv.format : t.d.format), (uint32_t)(has_tv && tv.set_target ? tv.target : t.d.target), t.d.depth, &nf, why, sizeof why)) continue;   // 表达不了的格式 / 目标：不出事实，编译器按缺项准确失败
        } else {
            int in_pool = id >= rp_S.tex_base && (uint64_t)(id - rp_S.tex_base) < (uint64_t)rp_S.tex_pool_n;
            memset(&nf, 0, sizeof nf); snprintf(nf.format, sizeof nf.format, "Unknown");
            snprintf(nf.sampler_type, sizeof nf.sampler_type, "%s", in_pool ? "1d" : "2d"); nf.normalized = !in_pool; nf.array_length = 1;
        }
        int n = f->ntex++;
        memcpy(f->tfmt[n], nf.format, sizeof f->tfmt[n]); memcpy(f->tsmp[n], nf.sampler_type, sizeof f->tsmp[n]);
        f->tex[n] = (struct s2a_compile_texture_src){st ? "fs" : "vs", (uint32_t)(8 + 2 * slot), -1, f->tsmp[n], f->tfmt[n], nf.is_buffer, nf.normalized, 0, nf.array_length};
    }
}

// fd_binding → 后端布局项（核心按 src_kind / src_reg 绑定）。0 = 这一项首版不支持
static int layout_ent(const struct fd_binding *b, nvn_gpu_layout_ent *e) {
    memset(e, 0, sizeof *e);
    e->set = (uint8_t)b->set; e->binding = (uint16_t)b->binding; e->count = 1; e->access = NVN_GPU_ACC_R;
    if (b->kind == 0) {
        e->cls = NVN_GPU_RES_UBO;
        if (b->is_support) e->src_kind = NVN_RB_SUPPORT;
        else if (b->slot == 0) e->src_kind = NVN_RB_DRV0;
        else if (b->slot == 1) e->src_kind = NVN_RB_C1;
        else if (b->slot == 2) e->src_kind = NVN_RB_DRV2;   // 驱动的贴图句柄表（着色器自己从表里读句柄时才会出现）
        else if (b->slot >= 3 && b->slot - 3 < 16) { e->src_kind = NVN_RB_CBUF; e->src_reg = (uint16_t)(b->slot - 3); }
        else return 0;
        return 1;
    }
    // 贴图数组：着色器按运行时下标取句柄，数组第 k 项 = 驱动句柄表的第 (src_reg + k) 个 8 字节项（核心按项逐个绑）
    if (b->kind == 2 && b->count > 1 && b->slot == 0) { e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEX; e->src_reg = 0; e->count = b->count; return 1; }
    if (b->kind == 2 && b->tex_slot >= 0) { e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEX; e->src_reg = (uint16_t)b->tex_slot; return 1; }
    return 0;   // 存储缓冲、图像：随对应命令接
}

// 驱动的贴图句柄表：贴图槽 s 的句柄在第 8 + 2s 个 32 位字，即第 4 + s 个 8 字节项；32 个槽 → 36 项
enum { HANDLE_TABLE_ENTRIES = 4 + 32 };

static struct entry *build(const uint8_t *bytes, size_t len, const struct s2a_source_entry_view views[2], int nst) {
    struct entry *e = calloc(1, sizeof *e);
    struct fd_program fp; char why[192] = "";
    memset(&fp, 0, sizeof fp);
    if (nvn_vk_program_parse_graphics_stages(bytes, len, &fp, nst == 1, why, sizeof why)) { red1("program_parse_failed", why); e->failed = 1; return e; }
    e->prog.support_bytes = fp.support_bytes; e->prog.attr_mask = fp.stage[0].attr_mask;
    e->prog.color_out_mask = nst == 2 && fp.stage[1].present ? fp.stage[1].fragment_output_map : 0xffffffffu;
    { static int said; if (nst == 2 && said < 40 && nvn_plat_flag("nvk_drawlog", 0)) { said++; char m[96]; snprintf(m, sizeof m, "片元输出映射 %#x（片元代码 %zu 字节）", (unsigned)e->prog.color_out_mask, fp.stage[1].spirv_bytes); nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", m); } }
    for (int st = 0; st < nst && !e->failed; st++) {
        if (!fp.stage[st].present) { red1("stage_missing_in_result", st ? "fs" : "vs"); e->failed = 1; break; }
        for (int i = 0; i < fp.binding_count && !e->failed; i++) {
            const struct fd_binding *b = &fp.bindings[i];
            if (!(b->is_support || b->stage_idx == st)) continue;
            if (e->prog.nent[st] == RP_PROG_ENT_MAX) { red1("too_many_bindings", ""); e->failed = 1; break; }
            nvn_gpu_layout_ent *le = &e->prog.ent[st][e->prog.nent[st]];
            if (!layout_ent(b, le)) { char nm[40]; snprintf(nm, sizeof nm, "binding_kind_%d_slot_%d", b->kind, b->slot); red1(nm, b->stage); e->failed = 1; break; }
            le->stages = b->is_support ? (nst == 2 ? 3u : 1u) : 1u << st;
            e->prog.nent[st]++;
        }
        if (e->failed) break;
        nvn_gpu_layout L = {0}; L.ent = e->prog.ent[st]; L.nent = e->prog.nent[st]; L.support_bytes = fp.support_bytes; L.id_src = "s2a-direct-program-v1";
        nvn_gpu_shader_src src = {0}; src.stage = st ? NVN_GPU_SH_FS : NVN_GPU_SH_VS; src.kind = NVN_GPU_BLOB_SPIRV;
        src.blob = fp.stage[st].spirv; src.bytes = fp.stage[st].spirv_bytes; src.entry = "main"; src.layout = &L;
        nvn_gpu_err err = {0};
        if (!nvn_gpu_ok(nvn_gpu_shader_load(&src, &e->prog.sh[st], &err))) { red1("shader_load_failed", err.why); e->failed = 1; }
        if (views[st].c1 && views[st].c1_bytes >= 0x100) { memcpy(e->prog.c1[st], views[st].c1, 0x100); e->prog.c1_ok |= (uint8_t)(1u << st); }
    }
    nvn_vk_program_free(&fp);
    if (e->failed) for (int st = 0; st < 2; st++) if (e->prog.sh[st]) { nvn_gpu_shader_release(e->prog.sh[st]); e->prog.sh[st] = NULL; }
    return e;
}

// 快路径：输入跟上一次取程序时一样，就还是那个程序。下面的摘要盖住 fill_facts 与身份用到的全部回放状态
// （游戏每笔绘制前都会重绑贴图 / 缓冲，绑的东西对编译来说通常没变；不走快路径时每笔要重填事实、编码身份、算一次 SHA-256）。
// 调试开关 nvk_resolve_check=1：命中后仍走一遍慢路径核对，不一致报红。
// 一张小的直映射表而不是只记上一次：相邻两笔常在两三种材质之间来回换，只记一项时每次都落空。
enum { LAST_N = 1024 };
static struct last_ent { int ok; uint64_t key; uint64_t session, generation; const struct rp_prog *prog; } g_lasts[LAST_N];
// 阶段键一律经核心导出接口取（rp_shader_stage_key）：没绑定 / 键缺失给全 0，不读可能过期的核心内部分量。
static const uint8_t *stage_key(unsigned st) {
    static uint8_t k[2][8];
    unsigned i = st ? 1u : 0u;
    if (!rp_shader_stage_key(st, k[i])) memset(k[i], 0, 8);
    return k[i];
}
static uint64_t input_sig(const struct rp_shader_req *q, const struct s2a_direct_compile_context *pc, uint32_t stages) {
    uint64_t h = 1469598103934665603ull;
    #define MIX(v) do { h ^= (uint64_t)(v); h *= 1099511628211ull; } while (0)
    MIX(stages); MIX(q->prim); MIX(q->rt_count); for (int i = 0; i < 8; i++) MIX(q->rt_format[i]);
    for (int st = 0; st < 2; st++) { uint64_t k; memcpy(&k, stage_key((unsigned)st), 8); MIX(k); }
    MIX(rp_S.color.alpha_func); { uint32_t ar; float r = rp_S.color.alpha_func >= 1 && rp_S.color.alpha_func <= 7 ? rp_S.alpha_ref : 0.0f; memcpy(&ar, &r, 4); MIX(ar); }
    MIX(rp_S.ms.a2c ? 1 : 0); MIX(rp_S.ms.a2c_dither ? 1 : 0);
    { int dual = 0; for (int i = 0; i < 8; i++) if (src1(rp_S.blend[i].src_rgb) || src1(rp_S.blend[i].dst_rgb) || src1(rp_S.blend[i].src_a) || src1(rp_S.blend[i].dst_a)) dual = 1; MIX(dual); }
    MIX(rp_depth_zero()); MIX(rp_origin_lower_left()); MIX(pc->shader_draw_parameters);
    { int any = 0, un = 0; for (int st = 0; st < 5; st++) for (int i = 0; i < 16; i++) if (rp_S.ssbo[st][i].addr) { any = 1; if (pc->storage_buffer_offset_alignment && rp_S.ssbo[st][i].addr % pc->storage_buffer_offset_alignment) un = 1; } MIX(any); MIX(un); MIX(pc->storage_buffer_offset_alignment); }
    MIX(rp_S.nattr); for (int i = 0; i < 16; i++) MIX(i < rp_S.nattr ? rp_S.attr[i].fmt : 0);
    for (int st = 0; st < 6; st++) { uint32_t m = 0; for (int i = 0; i < 16; i++) if (rp_S.ubo[st][i].size) m |= 1u << i; MIX(m); }
    MIX(rp_S.tex_pool_n); MIX(rp_S.samp_pool_n); MIX(rp_S.tf_on); MIX(rp_S.tex_base);
    MIX(rp_tex_fact_sig(rp_S.tex[0], 32, rp_S.tex_base, rp_S.tex_pool_n)); MIX(rp_tex_fact_sig(rp_S.tex[1], 32, rp_S.tex_base, rp_S.tex_pool_n));
    #undef MIX
    return h;
}
static int resolve_slow(const struct rp_shader_req *q, const struct rp_prog **out, const struct s2a_direct_compile_context *pcp, uint32_t stages, int nst);

int rp_shader_resolve(const struct rp_shader_req *q, const struct rp_prog **out) {
    *out = NULL;
    uint32_t stages = rp_shader_graphics_stages();
    // 认两种：顶点 + 片元；只有顶点（只写深度的绘制：深度预通道、阴影图）。几何 / 细分阶段还没接
    if (stages != 3u && stages != 1u) { char nm[32]; snprintf(nm, sizeof nm, "stages_%#x_not_vs_fs", stages); nvn_unimpl("shader", nm, NULL); return -1; }
    const int nst = stages == 3u ? 2 : 1;
    for (uint32_t st2 = 0; st2 < 5; st2++) if ((stages >> st2 & 1) && !rp_shader_stage_key(st2, NULL)) { nvn_unimpl("shader", "stage_key_missing", NULL); return -1; }
    if (!shader_texture_pool_ready()) return -1;
    struct s2a_direct_compile_context pc; memset(&pc, 0, sizeof pc);
    if (s2a_direct_provider_compile_context(&pc) || !pc.profile_json || !pc.profile_bytes) { nvn_unimpl("shader", "no_compile_context", NULL); return -1; }
    uint64_t key = input_sig(q, &pc, stages);
    struct last_ent *le = &g_lasts[(key ^ key >> 32) & (LAST_N - 1)];
    static int check = -1; if (check < 0) check = nvn_plat_flag("nvk_resolve_check", 0);
    if (le->ok && le->key == key && le->session == pc.session && le->generation == pc.generation) {
        *out = le->prog;
        if (check) { const struct rp_prog *chk = NULL; int rc = resolve_slow(q, &chk, &pc, stages, nst); if (rc || chk != le->prog) red1("resolve_fast_path_mismatch", "快路径给的程序与慢路径不一致"); }
        return 0;
    }
    int rc = resolve_slow(q, out, &pc, stages, nst);
    le->ok = rc == 0; le->key = key; le->session = pc.session; le->generation = pc.generation; le->prog = *out;
    return rc;
}

static int resolve_slow(const struct rp_shader_req *q, const struct rp_prog **out, const struct s2a_direct_compile_context *pcp, uint32_t stages, int nst) {
    struct s2a_direct_compile_context pc = *pcp;
    *out = NULL;
    if (!g_profile.ok || g_profile.session != pc.session || g_profile.generation != pc.generation) {
        sha256(pc.profile_json, pc.profile_bytes, g_profile.sha); g_profile.session = pc.session; g_profile.generation = pc.generation; g_profile.ok = 1; }
    static struct facts f;   // 提交线程独占；结构大，不放栈上
    fill_facts(q, &pc, &f, nst);
    struct s2a_input_identity id; memset(&id, 0, sizeof id);
    id.direct_abi = (int)pc.direct_abi; id.translator_closure = pc.translator_closure; memcpy(id.profile_sha, g_profile.sha, 32);
    id.stage_mask = stages;
    for (int st = 0; st < nst; st++) {
        memcpy(id.keys[st], stage_key((unsigned)st), 8);
        if (s2a_source_identity(stage_key((unsigned)st), id.source_sha[st], &id.source_code_bytes[st], &id.source_control_bytes[st])) { nvn_unimpl("shader", "source_snapshot_missing", NULL); return -1; }
        id.source_present |= (uint8_t)(1u << st);
    }
    memcpy(id.ubo_use, f.ubo_use, sizeof id.ubo_use);
    id.texture_array_length = (uint32_t)rp_S.tex_pool_n; id.sampler_array_length = (uint32_t)rp_S.samp_pool_n; id.transform_feedback_enabled = rp_S.tf_on;
    id.gfx = &f.gfx; id.textures = f.tex; id.texture_count = f.ntex;
    static uint8_t idbuf[S2A_INPUT_IDENTITY_MAX]; size_t idlen = 0; uint8_t sha[32];
    if (s2a_input_identity_encode(&id, idbuf, sizeof idbuf, &idlen)) { nvn_unimpl("shader", "identity_encode_failed", NULL); return -1; }
    sha256(idbuf, idlen, sha);
    struct entry *e = nvn_hm_get(&g_progs, sha);
    if (!e) {
        struct s2a_source_entry_view views[2]; memset(views, 0, sizeof views);
        for (int st = 0; st < nst; st++) if (!s2a_source_get_key(stage_key((unsigned)st), &views[st]) || !views[st].complete)   /* get_key：1 = 命中 */ { nvn_unimpl("shader", "source_snapshot_incomplete", NULL); return -1; }
        uint8_t *bytes = NULL; size_t len = 0; char why[256] = "";
        const char *root = getenv("S2A_SHADER_REVISION_ROOT"), *cdir = getenv("S2A_DIRECT_CACHE_DIR");
        if (root && pc.profile_hash && pc.profile_hash[0] && !nvn_plat_flag("no_offline", 0)) {
            struct s2a_offline_request oq = {0}; memcpy(oq.identity, sha, 32); oq.direct_abi = pc.direct_abi; oq.profile_hash = pc.profile_hash; oq.profile_declared = pc.offline_profile_available;
            struct s2a_offline_program op = {0};
            if (s2a_offline_library_fetch(root, &oq, &op, why, sizeof why) == S2A_OFFLINE_HIT) { bytes = op.bytes; len = op.len; g_n_offline++; }
        }
        if (!bytes && cdir && !nvn_plat_flag("no_disk_cache", 0) && s2a_program_cache_load(cdir, sha, &bytes, &len, NULL, why, sizeof why) == 0) g_n_cache++;
        if (!bytes) {
            struct s2a_compile_request_input in; memset(&in, 0, sizeof in);
            in.session = pc.session; in.generation = pc.generation; in.profile_json = pc.profile_json; in.profile_bytes = pc.profile_bytes;
            in.direct_abi = (int)pc.direct_abi; in.translator_closure = pc.translator_closure;
            in.texture_array_length = (uint32_t)rp_S.tex_pool_n; in.sampler_array_length = (uint32_t)rp_S.samp_pool_n; in.transform_feedback_enabled = rp_S.tf_on;
            in.gfx = f.gfx; memcpy(in.textures, f.tex, sizeof in.textures); in.texture_count = f.ntex;
            in.handle_table_entries = HANDLE_TABLE_ENTRIES;
            char ident[40]; int o = 0;
            for (int st = 0; st < nst; st++) {
                struct s2a_compile_stage_src *d = &in.stages[in.stage_count++];
                const uint8_t *code = views[st].code; uint32_t nb = views[st].code_bytes;
                if (nb > 0x30 && code[0] == 0x78 && code[1] == 0x56 && code[2] == 0x34 && code[3] == 0x12) { code += 0x30; nb -= 0x30; }   // 带 0x30 字节头的代码段
                d->stage = st ? "fs" : "vs"; d->code = code; d->code_bytes = nb; d->control = views[st].control; d->control_bytes = views[st].control_bytes;
                d->c1 = views[st].c1; d->c1_bytes = views[st].c1_bytes; d->constant_buffer_use = f.ubo_use[st];
                if (!in.gfx.has_tess_mode && views[st].control_bytes >= 0x748) { memcpy(&in.gfx.tess_mode, views[st].control + 0x744, 4); in.gfx.has_tess_mode = 1; }
                for (int b = 0; b < 8; b++) o += snprintf(ident + o, sizeof ident - (size_t)o, "%02x", stage_key((unsigned)st)[b]);
            }
            in.request_id = ident;
            char *json = NULL; size_t jl = 0; char gaps[1024] = "";
            if (s2a_compile_request_build(&in, &json, &jl, gaps, sizeof gaps)) { red1("request_build_failed", gaps); return -1; }
            struct s2a_compile_outcome oc; memset(&oc, 0, sizeof oc);
            int ran = s2a_compile_request_execute(in.session, in.generation, json, jl, &oc);
            free(json);
            if (ran || !oc.bytes || !oc.bytes_len) {
                g_n_failed++; red1("compile_failed", oc.summary);
                s2a_compile_outcome_free(&oc);
                e = calloc(1, sizeof *e); e->failed = 1; nvn_hm_put(&g_progs, sha, e);   // 同一份输入不再反复编
                return -1;
            }
            bytes = oc.bytes; len = oc.bytes_len; oc.bytes = NULL; s2a_compile_outcome_free(&oc); g_n_compiled++;
            if (cdir && !nvn_plat_flag("no_disk_cache", 0)) s2a_program_cache_store_async(cdir, sha, bytes, len);
        }
        e = build(bytes, len, views, nst);
        free(bytes);
        nvn_hm_put(&g_progs, sha, e);
        if (!e->failed && ((g_n_offline + g_n_cache + g_n_compiled) == 1 || (g_n_offline + g_n_cache + g_n_compiled) % 50 == 0)) {
            char b[160]; snprintf(b, sizeof b, "程序累计：离线库 %llu、跨局缓存 %llu、现场编 %llu、失败 %llu", (unsigned long long)g_n_offline, (unsigned long long)g_n_cache, (unsigned long long)g_n_compiled, (unsigned long long)g_n_failed);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", b);
        }
    }
    if (e->failed) return -1;
    *out = &e->prog;
    return 0;
}

// ---------- 计算程序 ----------
// 输入的构造与身份用旧层留下的零件（nvn_vk_compute_request.c：只做输入，不碰设备），结果解析用 nvn_vk_program_parse_compute。
// 首版只认：常量缓冲（含驱动段 c0、着色器自带段 c1）、存储缓冲（地址在 NVN 槽表，或在计算阶段的用户 UBO 槽表）、组合贴图。
// 图像、地址藏在某个 UBO 内容里的存储缓冲（NVK_STORAGE_UBO_ADDRESS）：记账、取不到。
static struct entry *build_compute(const uint8_t *bytes, size_t len, const struct s2a_source_entry_view *src) {
    struct entry *e = calloc(1, sizeof *e); e->prog.is_compute = 1;
    static struct nvn_vk_compute_program cp; char why[192] = "";
    memset(&cp, 0, sizeof cp);
    if (nvn_vk_program_parse_compute(bytes, len, &cp, why, sizeof why)) { red1("compute_program_parse_failed", why); e->failed = 1; return e; }
    e->prog.support_bytes = cp.support_bytes;
    for (int i = 0; i < cp.binding_count && !e->failed; i++) {
        const struct fd_binding *b = &cp.bindings[i];
        if (e->prog.nent[0] == RP_PROG_ENT_MAX) { red1("too_many_bindings", "cs"); e->failed = 1; break; }
        nvn_gpu_layout_ent *le = &e->prog.ent[0][e->prog.nent[0]]; int ok = 0;
        if (b->kind == 1) {
            const struct nvn_vk_compute_storage *sg = &cp.storage[i];
            // 两种来源绑出来的都是「池镜像里的一段」，只差在哪张表给地址：NVK_STORAGE_NVN 取 NVN 存储缓冲槽（NVN_RB_SBUFP）、
            // NVK_STORAGE_UBO_PTR 取计算阶段的用户 UBO 槽（NVN_RB_UBOP，驱动段 c0 的 0x210 表）。access 一律取反射的 writable（翻译器见到写 / 原子操作才置位，
            // 没置位的缓冲在 SPIR-V 里带 NonWritable）；核心据此决定登记不登记 GPU 写。NVK_STORAGE_UBO_ADDRESS 要先读 UBO 内容才拿得到地址，这里不接。
            if ((sg->mapping == NVK_STORAGE_NVN || sg->mapping == NVK_STORAGE_UBO_PTR) && sg->index < 16 && !sg->offset) {
                memset(le, 0, sizeof *le); le->set = (uint8_t)b->set; le->binding = (uint16_t)b->binding; le->count = 1;
                le->cls = NVN_GPU_RES_SSBO; le->access = sg->writable ? NVN_GPU_ACC_RW : NVN_GPU_ACC_R;
                le->src_kind = sg->mapping == NVK_STORAGE_UBO_PTR ? NVN_RB_UBOP : NVN_RB_SBUFP; le->src_reg = (uint16_t)sg->index; ok = 1;
            }
        } else ok = layout_ent(b, le);
        if (!ok) { char nm[56]; snprintf(nm, sizeof nm, "compute_binding_kind_%d_slot_%d_map_%d", b->kind, b->slot, b->kind == 1 ? (int)cp.storage[i].mapping : 0); red1(nm, "cs"); e->failed = 1; break; }
        le->stages = 1u << NVN_GPU_SH_CS;
        e->prog.nent[0]++;
    }
    if (!e->failed && !cp.stage.present) { red1("stage_missing_in_result", "cs"); e->failed = 1; }
    if (!e->failed) {
        nvn_gpu_layout L = {0}; L.ent = e->prog.ent[0]; L.nent = e->prog.nent[0]; L.support_bytes = cp.support_bytes; L.id_src = "s2a-direct-program-v1";
        nvn_gpu_shader_src ss = {0}; ss.stage = NVN_GPU_SH_CS; ss.kind = NVN_GPU_BLOB_SPIRV; ss.blob = cp.stage.spirv; ss.bytes = cp.stage.spirv_bytes; ss.entry = "main"; ss.layout = &L;
        nvn_gpu_err err = {0};
        if (!nvn_gpu_ok(nvn_gpu_shader_load(&ss, &e->prog.sh[0], &err))) { red1("compute_shader_load_failed", err.why); e->failed = 1; }
        if (src->c1 && src->c1_bytes >= 0x100) { memcpy(e->prog.c1[0], src->c1, 0x100); e->prog.c1_ok |= 1; }
    }
    nvn_vk_program_free_compute(&cp);
    return e;
}

int rp_shader_resolve_compute(const struct rp_prog **out) {
    *out = NULL;
    if (!(rp_S.skey_ok >> 5 & 1)) { nvn_unimpl("shader", "compute_stage_key_missing", NULL); return -1; }
    if (!shader_texture_pool_ready()) return -1;
    struct s2a_direct_compile_context pc; memset(&pc, 0, sizeof pc);
    if (s2a_direct_provider_compile_context(&pc) || !pc.profile_json || !pc.profile_bytes) { nvn_unimpl("shader", "no_compile_context", NULL); return -1; }
    static struct s2a_record_dispatch_state st; memset(&st, 0, sizeof st);
    memcpy(st.keys[0], rp_S.skey[5], 8); st.stages = 32;
    { uint32_t m = 0; for (int i = 0; i < 16; i++) if (rp_S.ubo[5][i].size) m |= 1u << i; st.ubo_use[5] = m << 3; }   // 用户常量缓冲从 c3 起
    st.tex_pool_n = (uint32_t)rp_S.tex_pool_n; st.samp_pool_n = (uint32_t)rp_S.samp_pool_n; st.tex_pool_seen = rp_S.tex_pool_n > 0; st.samp_pool_seen = rp_S.samp_pool_n > 0; st.tex_pool_base = rp_S.tex_base;
    // 贴图事实：计算阶段的 32 个组合槽，取法同图形（没绑过的槽读作池里的第 0 项）
    static struct s2a_compile_texture_src tex[32]; static char tfmt[32][S2A_TEX_FORMAT_MAX], tsmp[32][S2A_TEX_SAMPLER_MAX]; int ntex = 0;
    for (int slot = 0; slot < 32; slot++) {
        uint64_t h = rp_S.tex[5][slot]; long id = h ? (long)(h & 0xFFFFF) : (long)rp_S.tex_base;
        tex_t t; view_t tv; int has_tv = 0; struct s2a_nvn_texture_fact nf; char why[96];
        if (rp_tex_snap_by_id((int)id, &t, &tv, &has_tv)) {
            if (s2a_nvn_texture_fact((uint32_t)(has_tv && tv.set_format ? tv.format : t.d.format), (uint32_t)(has_tv && tv.set_target ? tv.target : t.d.target), t.d.depth, &nf, why, sizeof why)) continue;
        } else {
            int in_pool = id >= rp_S.tex_base && (uint64_t)(id - rp_S.tex_base) < (uint64_t)rp_S.tex_pool_n;
            memset(&nf, 0, sizeof nf); snprintf(nf.format, sizeof nf.format, "Unknown");
            snprintf(nf.sampler_type, sizeof nf.sampler_type, "%s", in_pool ? "1d" : "2d"); nf.normalized = !in_pool; nf.array_length = 1;
        }
        memcpy(tfmt[ntex], nf.format, sizeof tfmt[ntex]); memcpy(tsmp[ntex], nf.sampler_type, sizeof tsmp[ntex]);
        tex[ntex] = (struct s2a_compile_texture_src){"cs", (uint32_t)(8 + 2 * slot), -1, tsmp[ntex], tfmt[ntex], nf.is_buffer, nf.normalized, 0, nf.array_length};
        ntex++;
    }
    struct nvk_compute_texture_facts tf = {tex, ntex, 1};
    int unaligned = 0;
    for (int i = 0; i < 16; i++) if (rp_S.ssbo[5][i].addr && pc.storage_buffer_offset_alignment && rp_S.ssbo[5][i].addr % pc.storage_buffer_offset_alignment) unaligned = 1;
    char ident[24]; { int o = 0; for (int b = 0; b < 8; b++) o += snprintf(ident + o, sizeof ident - (size_t)o, "%02x", rp_S.skey[5][b]); }
    static struct nvk_compute_request rq; memset(&rq, 0, sizeof rq); char why[256] = "";
    if (nvk_compute_request_prepare(&st, &pc, &tf, 1, unaligned, ident, &rq, why, sizeof why)) { red1("compute_request_failed", why); return -1; }
    struct entry *e = nvn_hm_get(&g_progs, rq.identity);
    if (!e) {
        uint8_t *bytes = NULL; size_t len = 0;
        const char *root = getenv("S2A_SHADER_REVISION_ROOT"), *cdir = getenv("S2A_DIRECT_CACHE_DIR");
        if (root && pc.profile_hash && pc.profile_hash[0] && !nvn_plat_flag("no_offline", 0)) {
            struct s2a_offline_request oq = {0}; memcpy(oq.identity, rq.identity, 32); oq.direct_abi = pc.direct_abi; oq.profile_hash = pc.profile_hash; oq.profile_declared = pc.offline_profile_available;
            struct s2a_offline_program op = {0};
            if (s2a_offline_library_fetch(root, &oq, &op, why, sizeof why) == S2A_OFFLINE_HIT) { bytes = op.bytes; len = op.len; g_n_offline++; }
        }
        if (!bytes && cdir && !nvn_plat_flag("no_disk_cache", 0) && s2a_program_cache_load(cdir, rq.identity, &bytes, &len, NULL, why, sizeof why) == 0) g_n_cache++;
        if (!bytes) {
            struct s2a_compile_outcome oc; memset(&oc, 0, sizeof oc);
            int ran = s2a_compile_request_execute(rq.session, rq.generation, rq.json, rq.json_bytes, &oc);
            if (ran || !oc.bytes || !oc.bytes_len) {
                g_n_failed++; red1("compute_compile_failed", oc.summary);
                s2a_compile_outcome_free(&oc);
                e = calloc(1, sizeof *e); e->failed = 1; nvn_hm_put(&g_progs, rq.identity, e);
                nvk_compute_request_release(&rq);
                return -1;
            }
            bytes = oc.bytes; len = oc.bytes_len; oc.bytes = NULL; s2a_compile_outcome_free(&oc); g_n_compiled++;
            if (cdir && !nvn_plat_flag("no_disk_cache", 0)) s2a_program_cache_store_async(cdir, rq.identity, bytes, len);
        }
        e = build_compute(bytes, len, &rq.source);
        free(bytes);
        nvn_hm_put(&g_progs, rq.identity, e);
    }
    nvk_compute_request_release(&rq);
    if (e->failed) return -1;
    *out = &e->prog;
    return 0;
}

void rp_shader_reset(void) {
    for (uint32_t i = 0; i < g_progs.cap; i++) { struct entry *e = g_progs.vals[i]; if (!e) continue;
        for (int st = 0; st < 2; st++) if (e->prog.sh[st]) nvn_gpu_shader_release(e->prog.sh[st]);
        free(e); }
    free(g_progs.keys); free(g_progs.vals); nvn_hm_init(&g_progs, 32);
    g_profile.ok = 0; memset(g_lasts, 0, sizeof g_lasts);
    g_texpool_logged = g_texpool_last_over_limit = 0;
}
