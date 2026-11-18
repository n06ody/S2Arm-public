// Draw 快照 → 编译事实/内容身份/P1 备忘键。见 nvn_vk_compile_facts.h 的合同说明。
// 原事实/身份仍为纯计算；可选 W2 支路只持有有界元数据副本，不建 GPU 对象、不改编译输入。
#include "nvn_vk_compile_facts.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>

#include "direct/input_identity.h"
#include "direct/compile_export_w2.h"
#include "direct/sha256.h"
#include "direct/source_store.h"
#include "nvn_vk_format.h"
#include "nvn_vk_mrt.h"

// 设备级真实状态（hle_nvn 的 SetWindowOriginMode/SetDepthMode）。弱引用：没有共享前端时为空。
// 弱链接[androiddraw]：s2a_nvn_window_origin_mode（graphics/nvn 前端提供；宿主单测不链）
extern int s2a_nvn_window_origin_mode __attribute__((weak));
// 弱链接[androiddraw]：s2a_nvn_depth_mode（graphics/nvn 前端提供；宿主单测不链）
extern int s2a_nvn_depth_mode __attribute__((weak));
extern uint64_t s2a_vk_adapter_registry_generation(void) __attribute__((weak));
// 捕获期算定的完整源摘要；没有源快照模块时身份不可用（准确失败，不退回短 key）。
extern int s2a_source_identity(const uint8_t key[8], uint8_t sha[32], uint32_t *code_bytes,
                               uint32_t *control_bytes) __attribute__((weak));
extern uint64_t s2a_source_epoch(void) __attribute__((weak));

// 弱接缝保持纯 facts 宿主调用与未链接诊断模块的行为；缺依赖就没有诊断。
extern int s2a_compile_export_open(void) __attribute__((weak));
extern void s2a_compile_export_w2_set_provider(s2a_compile_export_w2_provider, void *) __attribute__((weak));
// 弱链接[androiddraw]：s2a_vk_adapter_w2_capture_registered（宿主单测 w2_observation_draw_test 编本文件不链 mtl_vk_adapter.c；macOS ld64 不接受可达的未定义弱引用，按 tools/weak_link_decl.py 登记放行）
// 弱链接[androiddraw]：s2a_compile_export_w2_set_provider（W2 宿主单测有的不链 compile_export_w2.c / nvn_vk_draw.c）
// 弱链接[androiddraw]：nvk_attr_pair_canon
extern int s2a_vk_adapter_w2_capture_registered(const int *, size_t,
    struct nvk_w2_raw_registration *, uint64_t *) __attribute__((weak));
extern int s2a_source_acquire(const uint8_t[8], struct s2a_source_entry_view *) __attribute__((weak));
extern void s2a_source_release(uint64_t) __attribute__((weak));

static void w2_collection_begin(const struct s2a_record_draw_state *, struct nvk_texture_set *);
static void w2_collection_observe(int id, unsigned stage, unsigned slot, int have, const struct nvk_tex_desc *);
static void w2_collection_end(struct nvk_texture_set *);
static int w2_identity_begin(void);
static void w2_identity_capture(const struct s2a_record_draw_state *, const struct s2a_direct_compile_context *,
    const struct s2a_input_identity *, const uint8_t *, size_t, uint64_t, int);

static uint64_t nvk_fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

void nvk_sha256_bytes(const uint8_t *buf, size_t len, uint8_t sha[32]) {
    s2a_sha256 c;
    s2a_sha256_init(&c);
    s2a_sha256_update(&c, buf, len);
    s2a_sha256_final(&c, sha);
}

uint64_t nvk_closure_hash(const char *s) {
    if (!s) return 0x9e3779b97f4a7c15ull;   // NULL 不是 ""
    uint64_t h = nvk_fnv1a((const uint8_t *)s, strlen(s));
    return h ? h : 1;                        // 0 留给 NULL
}

// 片元输出类型是否参与特化：默认 1（保持原语义，宿主测试不受影响）。
static int g_frag_out_spec = 1;
void nvk_compile_facts_set_frag_out_specialization(int on) { g_frag_out_spec = on ? 1 : 0; }
int nvk_compile_facts_frag_out_specialization(void) { return g_frag_out_spec; }

// 从 profile JSON 里读 needsFragmentOutputSpecialization。只做一次（按 session/generation 缓存），
// 不引 JSON 解析器：找键名 → 跳过冒号空白 → 看是不是 true。找不到就当“需要特化”（保守）。
static int profile_frag_out_spec(const char *json, size_t bytes) {
    static const char key[] = "\"needsFragmentOutputSpecialization\"";
    const size_t klen = sizeof key - 1;
    if (!json || bytes < klen) return 1;
    for (size_t i = 0; i + klen <= bytes; i++) {
        if (memcmp(json + i, key, klen)) continue;
        size_t j = i + klen;
        while (j < bytes && (json[j] == ' ' || json[j] == '\t' || json[j] == '\n' || json[j] == '\r')) j++;
        if (j >= bytes || json[j] != ':') continue;
        j++;
        while (j < bytes && (json[j] == ' ' || json[j] == '\t' || json[j] == '\n' || json[j] == '\r')) j++;
        if (j + 4 <= bytes && !memcmp(json + j, "true", 4)) return 1;
        if (j + 5 <= bytes && !memcmp(json + j, "false", 5)) return 0;
        return 1;
    }
    return 1;
}

void nvk_env_fill(const struct s2a_direct_compile_context *pc, struct nvk_env *out) {
    static struct { uint64_t session, generation; struct nvk_env env; int valid; } cache;
    memset(out, 0, sizeof *out);
    if (!pc) return;
    out->closure_hash = nvk_closure_hash(pc->translator_closure);
    if (!pc->profile_json || !pc->profile_bytes) return;
    if (cache.valid && cache.session == pc->session && cache.generation == pc->generation) {
        uint64_t closure = out->closure_hash;
        *out = cache.env;
        out->closure_hash = closure;
        return;
    }
    nvk_sha256_bytes((const uint8_t *)pc->profile_json, pc->profile_bytes, out->profile_sha);
    out->have_profile = 1;
    // 只解析、不落地开关：本文件也被宿主 CTest 链接，不能依赖 Android 的日志/属性 API。
    // 由绘制侧（nvk_program_for_draw）读环境变量、设置开关并打一行日志。
    out->frag_out_specialization = profile_frag_out_spec(pc->profile_json, pc->profile_bytes);
    cache.session = pc->session;
    cache.generation = pc->generation;
    cache.env = *out;
    cache.valid = 1;
}

// NVN 绘制图元 → 翻译器输入拓扑。QUADS/POLYGON 没有对应拓扑，按 Triangles 记（真实 quad→triangle
// 的 primitive-id 修正属于 halvePrimitiveId，本笔没有该事实来源，请求里不填）。
static const char *nvk_input_topology(uint32_t prim) {
    switch (prim) {
        case 0x0: return "Points";
        case 0x1: case 0x2: case 0x3: return "Lines";
        case 0xA: case 0xB: return "LinesAdjacency";
        case 0x4: case 0x5: case 0x6: case 0x7: case 0x8: case 0x9: case 0xE: return "Triangles";
        case 0xC: case 0xD: return "TrianglesAdjacency";
        default: return NULL;
    }
}

// NVN 顶点属性格式（NVN_FORMAT_*）→ 翻译器 AttributeType 名。
// NVN 的 format 只有 UI/I/SN/无符号归一化/浮点五族，无法表达 Maxwell 的 Sscaled/Uscaled：
// 因此 UI→Uint、I→Sint，其余（含 SN、unorm、float、packed）都是 Float（与 Ryujinx
// SpecializationStateUpdater 对 Snorm/Unorm 的换算一致）。NONE(0)=未绑定的初始状态，同样 Float。
static const char *nvk_attribute_type(uint32_t fmt) {
    switch (fmt) {
        // 32 位整数：R32UI/RG32UI（0xB/0x17）与 R32I/RG32I（0xC/0x18）此前漏掉，
        // 导致一槽未映射时整组 attributeTypes 被请求生成器省略。
        case 0x3: case 0xB: case 0x8: case 0xF: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
        case 0x27: case 0x2C: case 0x2F: case 0x3E:
            return "Uint";
        case 0x4: case 0xC: case 0x9: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
        case 0x28: case 0x2D: case 0x30: case 0x6A:
            return "Sint";
        // I2F/UI2F（0x51-0x68、0x6B/0x6C）：整数存储、顶点取样时转成 float（Metal vconv_i2f 同义），
        // 着色器输入是 Float；0x69 RGB10A2SN 是归一化 Float，0x9A BGRA8 unorm 也是 Float。
        // 0x37/0x38 与 0x95-0x9C BGR 家族（含 SRGB）同为归一化 Float。
        case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58:
        case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60:
        case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68:
        case 0x69: case 0x6B: case 0x6C: case 0x9A:
        case 0x37: case 0x38: case 0x95: case 0x96: case 0x97: case 0x98: case 0x99: case 0x9B: case 0x9C:
        case 0x0:
        case 0x1: case 0x2: case 0x5: case 0x6: case 0x7: case 0xA: case 0xD: case 0xE: case 0x11:
        case 0x12: case 0x13: case 0x16: case 0x19: case 0x1A: case 0x1D: case 0x1E: case 0x1F:
        case 0x22: case 0x25: case 0x26: case 0x29: case 0x2A:
        case 0x2B: case 0x2E: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3F: case 0x40:
            return "Float";
        default:
            return NULL;   // 认不出的格式：不猜类型，请求里省略该字段并记缺口
    }
}

// 颜色附件 VkFormat → AttributeType：整数附件按 Sint/Uint，其余（float/unorm/srgb）Float。
static const char *nvk_color_type(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_A2B10G10R10_SINT_PACK32: case VK_FORMAT_A2R10G10B10_SINT_PACK32:
            return "Sint";
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2R10G10B10_UINT_PACK32:
            return "Uint";
        case VK_FORMAT_UNDEFINED:
            return NULL;   // 附件已绑定但格式没解析出来：不填事实
        default:
            return "Float";
    }
}

// NVNcolorState.alphaFunc → AlphaTestOp 名（两个枚举同序：1 Never … 8 Always）。
static const char *nvk_alpha_test(uint32_t func) {
    static const char *const names[] = { NULL, "Never", "Less", "Equal", "LessOrEqual", "Greater", "NotEqual",
                                         "GreaterOrEqual", "Always" };
    return func >= 1 && func <= 8 ? names[func] : NULL;
}

// 把已绑定的真实状态对象填进编译事实。只读 state_seen 标记过的 kind；没绑定过就不写。
static void nvk_fill_bound_state(struct s2a_compile_graphics_src *gfx,
                                 const struct s2a_record_draw_state *state) {
    // 未绑定的状态对象：NVN 没有定义“未绑定时的值”，这里采用前端既有模型——
    // Metal 回放的状态结构从零值起步（nvn_mtl.m 的 S.ms.a2c / S.color / S.blend）。
    // 零值 color.alphaFunc 不是合法枚举 → alpha test 关闭（Always）。
    if (state->state_seen & (1u << 1)) {   // color
        uint32_t v;
        memcpy(&v, state->state_obj[1], sizeof v);
        uint32_t alpha_func = (v >> 12) & 0xfu;
        const char *op = nvk_alpha_test(alpha_func);
        if (op) { gfx->has_alpha_test = 1; gfx->alpha_test_compare = op; }
    } else {
        gfx->has_alpha_test = 1;
        gfx->alpha_test_compare = "Always";
    }
    if (state->state_seen & (1u << 3)) {   // multisample
        gfx->has_alpha_to_coverage = 1;
        gfx->alpha_to_coverage_enable = state->state_obj[3][2] ? 1 : 0;
        gfx->alpha_to_coverage_dither = state->state_obj[3][3] ? 1 : 0;
    } else {
        gfx->has_alpha_to_coverage = 1;    // 零值状态：a2c / dither 都关
        gfx->alpha_to_coverage_enable = 0;
        gfx->alpha_to_coverage_dither = 0;
    }
    if (state->state_seen & (1u << 0)) {   // blend
        blend_st b;
        memcpy(&b, state->state_obj[0], sizeof b);
        gfx->has_blend = 1;
        // NVN_BLEND_FUNC_SRC1_* = 0x10..0x13；任一因子用它就是双源混合。
        gfx->dual_source_blend = ((b.src_rgb >= 0x10 && b.src_rgb <= 0x13) ||
                                  (b.dst_rgb >= 0x10 && b.dst_rgb <= 0x13) ||
                                  (b.src_a >= 0x10 && b.src_a <= 0x13) ||
                                  (b.dst_a >= 0x10 && b.dst_a <= 0x13)) ? 1 : 0;
    } else {
        gfx->has_blend = 1;                // 零值混合状态没有 SRC1 因子
        gfx->dual_source_blend = 0;
    }
}

// 单个组合纹理槽 → 一条事实（graphics 与 compute 共用同一套规则与句柄坐标）：
// 句柄表条目：写过的槽是绑定值；未写的槽读出 0，即池局部纹理 id 0（上游
// ReadPackedId 对空条目同解）。已绑定句柄的 id 录制时已按池基址重基，
// 未绑槽的“池局部 0”换算成绝对 id 就是 tex_pool_base 本身。
static void nvk_texture_slot_fact(int bound, uint64_t handle, int32_t pool_base,
                                  uint32_t pool_n, uint8_t st, unsigned slot,
                                  const char *stage_name, nvk_tex_lookup_fn lookup,
                                  void *opaque, struct nvk_texture_set *set) {
    long id = bound ? (long)(handle & 0xFFFFFu) : (long)pool_base;
    struct nvk_tex_desc td;
    int have = lookup && id >= 0 && id <= INT32_MAX && lookup((int)id, &td, opaque) == 0;
    w2_collection_observe((int)id, st, slot, have, have ? &td : NULL);
    struct s2a_nvn_texture_fact nf;
    if (have) {
        char fwhy[96] = "";
        if (s2a_nvn_texture_fact(td.nvn_format, td.nvn_target, td.layers, &nf,
                                 fwhy, sizeof fwhy)) {
            // 注册过但表达不了：真实缺项，不出事实，留明细给诊断。
            int i = set->unmapped_total++;
            if (i < NVK_TEXTURE_UNMAPPED_MAX) {
                set->unmapped[i].stage = st;
                set->unmapped[i].slot = (uint8_t)slot;
                set->unmapped[i].id = (uint32_t)id;
                set->unmapped[i].nvn_format = td.nvn_format;
                set->unmapped[i].nvn_target = td.nvn_target;
                set->unmapped[i].layers = td.layers;
            }
            return;
        }
    } else {
        // 无注册描述符：与 Ryujinx TextureBindingsManager.GetTextureDescriptor 对齐。
        // IsValidId = id <= 池 MaximumId，即 0 <= (id - tex_pool_base) < tex_pool_n。
        int in_pool = pool_base >= 0 && id >= pool_base &&
                      (uint64_t)(id - pool_base) < pool_n;
        memset(&nf, 0, sizeof nf);
        snprintf(nf.format, sizeof nf.format, "Unknown");
        snprintf(nf.sampler_type, sizeof nf.sampler_type, "%s", in_pool ? "1d" : "2d");
        nf.is_buffer = 0;
        nf.normalized = in_pool ? 0 : 1;
        nf.array_length = 1;
    }
    int n = set->count;
    memcpy(set->fmt[n], nf.format, sizeof set->fmt[n]);
    memcpy(set->smp[n], nf.sampler_type, sizeof set->smp[n]);
    struct s2a_compile_texture_src *t = &set->items[n];
    t->stage = stage_name;
    t->handle = 8 + 2 * (int)slot;
    t->cbuf_slot = -1;
    t->sampler_type = set->smp[n];
    t->format = set->fmt[n];
    t->is_buffer = nf.is_buffer;
    t->normalized = nf.normalized;
    t->separate = 0;
    t->array_length = nf.array_length;
    set->count++;
}

void nvk_texture_facts_fill(const struct s2a_record_draw_state *state,
                            nvk_tex_lookup_fn lookup, void *opaque,
                            struct nvk_texture_set *set) {
    memset(set, 0, sizeof *set);
    w2_collection_begin(state, set);
    if (!state) return;
    for (unsigned st = 0, idx = 0; st < 6 && idx < 6 && set->count < NVK_TEXTURE_SET_MAX; st++) {
        if (!(state->stages & (1u << st))) continue;
        const char *name = st == 0 ? "vs" : "fs";
        for (unsigned slot = 0; slot < 32 && set->count < NVK_TEXTURE_SET_MAX; slot++) {
            nvk_texture_slot_fact(state->tex_present[st][slot] != 0,
                state->tex_handle[st][slot], state->tex_pool_base, state->tex_pool_n,
                (uint8_t)st, slot, name, lookup, opaque, set);
        }
        idx++;
    }
    w2_collection_end(set);
}

void nvk_compute_texture_facts_fill(const struct s2a_record_dispatch_state *state,
                                    nvk_tex_lookup_fn lookup, void *opaque,
                                    struct nvk_texture_set *set) {
    memset(set, 0, sizeof *set);
    if (!state) return;
    for (unsigned slot = 0; slot < 32 && set->count < NVK_TEXTURE_SET_MAX; slot++) {
        nvk_texture_slot_fact(state->tex_present[slot] != 0,
            state->tex_handle[slot], state->tex_pool_base, state->tex_pool_n,
            5, slot, "cs", lookup, opaque, set);
    }
}

// Storage bindings are indexed by NVN stage, not packed VS/FS shader order.
// Compute stage 5 is independent and cannot affect a graphics compile identity.
static int graphics_ssbo_bound(const struct s2a_record_draw_state *state) {
    for (unsigned stage = 0; stage < 5; stage++)
        if (state->ssbo_bound[stage]) return 1;
    return 0;
}

// graphicsState.hasUnalignedStorageBuffer 的真实来源：本笔已绑定的 graphics SSBO 里，
// 是否有地址不按设备 minStorageBufferOffsetAlignment 对齐（Ryujinx 同语义：只看起始
// 地址，不看 size）。没有任何 graphics SSBO 时该布尔确定为 false；有绑定但没有对齐
// 来源（alignment=0）时 *known=0，编译请求据此报缺口，不能伪造 false。
// Draw 快照里的 addr 是回放用的 CPU 地址（NVN 绑定入口已 nvn_ga2ca），与计算路径按
// 同一地址判断对齐，偏移在池内按字节保留。
static int graphics_unaligned_ssbo(const struct s2a_record_draw_state *state, uint32_t alignment,
                                   int *known) {
    *known = 1;
    if (!graphics_ssbo_bound(state)) return 0;
    if (!alignment) { *known = 0; return 0; }
    for (unsigned stage = 0; stage < 5; stage++) {
        uint64_t bound = state->ssbo_bound[stage];
        for (unsigned slot = 0; slot < 16; slot++) {
            if (!(bound & ((uint64_t)1 << slot))) continue;
            if (state->ssbo[stage][slot].addr % alignment) return 1;
        }
    }
    return 0;
}

void nvk_compile_facts_fill(struct s2a_compile_graphics_src *gfx,
                            const struct s2a_record_draw_state *state,
                            const struct s2a_direct_compile_context *pc, int *complete) {
    if (complete) *complete = 1;
    gfx->topology = nvk_input_topology(state->prim);
    gfx->rt_count = (int)state->rt_count;
    gfx->constant_buffer_use_overflow = state->ubo_use_overflow;
    nvk_fill_bound_state(gfx, state);
    if (&s2a_nvn_depth_mode) { gfx->has_depth_mode = 1; gfx->depth_mode = s2a_nvn_depth_mode == NVN_DEPTH_MODE_NEAR_IS_MINUS_W; }
    if (&s2a_nvn_window_origin_mode) {
        gfx->has_y_negate = 1;
        gfx->y_negate = s2a_nvn_window_origin_mode == 0;
    }
    gfx->has_origin = 1;
    gfx->origin_upper_left = 1;
    gfx->has_halve_primitive_id = 1;
    gfx->host_supports_quads = 0;
    gfx->has_geometry_shader = 0;
    gfx->halve_primitive_id = (gfx->topology != NULL && state->prim == 0x7u) ? 1 : 0;
    gfx->early_z_force = 0;
    gfx->viewport_transform_disable = 0;
    gfx->has_cb_draw_params = 1;
    gfx->cb_draw_params = pc->shader_draw_parameters;
    gfx->has_point_size = 1;
    gfx->program_point_size_enable = 1;
    gfx->point_size = 1.0f;
    gfx->has_alpha_test_reference = 1;
    gfx->alpha_test_reference = 0.0f;
    {
        int known = 0;
        int unaligned = graphics_unaligned_ssbo(state, pc->storage_buffer_offset_alignment, &known);
        if (known) { gfx->has_unaligned_ssbo = 1; gfx->unaligned_ssbo = unaligned; }
    }
    {
        // 顶点属性“格式对”归一化：只在绘制侧证明了某一对格式互换不改变产物（且显式开启）时才生效。
        // 宿主测试没有这个符号，弱引用为空 ⇒ 行为不变。
        extern uint32_t nvk_attr_pair_canon(uint32_t f) __attribute__((weak));
        for (int i = 0; i < 32; i++) {
            uint32_t f = state->vattr_fmt[i];
            if (nvk_attr_pair_canon) f = nvk_attr_pair_canon(f);
            gfx->attribute_types[i] = nvk_attribute_type(f);
        }
    }
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        if (i < (int)state->rt_count) {
            if (state->rt[i].key == 0) {
                // 合法未绑定槽：SetRenderTargets 给了槽位但没有纹理（本笔就是 depth-only pass）。
                // 这里不伪造附件：管线按“没有颜色附件”建（first_draw 读 rt[0].key==0）；
                // fragmentOutputTypes 只是 FS 输出声明类型，不存在的附件用中性 Float。
                gfx->fragment_output_types[i] = "Float";
                continue;
            }
            // 槽位有对象但对象/格式不可信（magic 不对 = 快照丢失，或格式没映射）：这是真实缺项。
            if (state->rt[i].obj_magic != 0x00584554u) { ok = 0; break; }
            VkFormat f = state->rt[i].format;
            // 纹理表未解析时用快照/视图的 NVN 格式重映射；仍不识别才判缺项。
            if (f == VK_FORMAT_UNDEFINED && state->rt[i].nvn_format)
                f = nvn_vk_format(state->rt[i].nvn_format);
            // 压缩底图经“块=像素”的无压缩视图渲染（bcrt 影子）：编译事实必须按视图格式定
            // fragmentOutputTypes，否则压缩格式在 nvk_color_type 里无类型 → 请求缺字段被拒。
            if (f != VK_FORMAT_UNDEFINED && nvn_vk_format_is_compressed(f)) {
                const uint32_t vn = state->rt[i].view_format ? state->rt[i].view_format : state->rt[i].nvn_format;
                const VkFormat vf = nvn_vk_format(vn);
                VkFormat shadow = VK_FORMAT_UNDEFINED;
                char bwhy[128] = "";
                if (vf != VK_FORMAT_UNDEFINED &&
                    nvn_vk_bcrt_view_format(f, vf, &shadow, bwhy, sizeof bwhy) == 0)
                    f = shadow;
            }
            const char *ct = f != VK_FORMAT_UNDEFINED ? nvk_color_type(f) : NULL;
            if (!ct) { ok = 0; break; }
            // 该 profile 不需要片元输出特化时，类型名不影响产物（profile_sha 已经把 caps 位带进身份），
            // 所以这里写中性常量：不同颜色格式不再裂成不同身份。判据不变，仍逐字节比较。
            gfx->fragment_output_types[i] = g_frag_out_spec ? ct : "Float";
        } else {
            gfx->fragment_output_types[i] = "Float";
        }
    }
    if (!ok) {
        for (int i = 0; i < 8; i++) gfx->fragment_output_types[i] = NULL;
        if (complete) *complete = 0;
    }
}

int nvk_input_identity(const struct s2a_record_draw_state *state,
                       const struct s2a_direct_compile_context *pc,
                       const struct nvk_env *env,
                       const struct s2a_compile_graphics_src *gfx,
                       const struct s2a_compile_texture_src *textures, int texture_count,
                       struct nvk_src_ids *ids,
                       uint8_t *buf, size_t *len, uint8_t sha[32]) {
    int w2_collected = w2_identity_begin();
    uint64_t w2_epoch = s2a_source_epoch ? s2a_source_epoch() : 0;
    struct s2a_input_identity in;
    memset(&in, 0, sizeof in);
    in.direct_abi = (int)pc->direct_abi;
    in.translator_closure = pc->translator_closure;
    if (!env || !env->have_profile) return -1;
    memcpy(in.profile_sha, env->profile_sha, 32);
    in.stage_mask = state->stages;
    in.requires_linked_varying_interface = s2a_direct_requires_linked_varying_interface(in.stage_mask);
    memcpy(in.keys, state->keys, sizeof in.keys);
    // 完整源摘要来自 SetShaders/Bind 捕获时算定的那份快照（不逐笔重哈希）。
    // 缺任何一条都准确失败：拿短 key 当内容证明就是允许误命中。
    unsigned n = 0;
    if (ids) { memset(ids, 0, sizeof *ids); ids->missing_stage = 0xff; }
    for (unsigned st = 0, idx = 0; st < 6 && idx < 6; st++) {
        if (!(state->stages & (1u << st))) continue;
        if (n >= 6 || !s2a_source_identity) {
            if (ids) { ids->missing_stage = (uint8_t)st; memcpy(ids->missing_key, state->keys[idx], 8); }
            return -2;
        }
        if (s2a_source_identity(state->keys[idx], in.source_sha[n], &in.source_code_bytes[n],
                                &in.source_control_bytes[n]) != 0) {
            if (ids) { ids->missing_stage = (uint8_t)st; memcpy(ids->missing_key, state->keys[idx], 8); }
            return -2;
        }
        in.source_present |= (uint8_t)(1u << n);
        n++;
        idx++;
    }
    if (!n) return -2;
    if (ids) {
        memset(ids, 0, sizeof *ids);
        ids->missing_stage = 0xff;
        ids->present = in.source_present;
        for (unsigned i = 0; i < n; i++) {
            memcpy(ids->sha[i], in.source_sha[i], 32);
            ids->code_bytes[i] = in.source_code_bytes[i];
            ids->control_bytes[i] = in.source_control_bytes[i];
        }
    }
    for (int i = 0; i < 6; i++) in.ubo_use[i] = state->ubo_use[i];
    in.texture_array_length = state->tex_pool_n;
    in.sampler_array_length = state->samp_pool_n;
    in.transform_feedback_enabled = state->tf_enabled;
    in.gfx = gfx;
    in.textures = textures;
    in.texture_count = texture_count;
    if (s2a_input_identity_encode(&in, buf, S2A_INPUT_IDENTITY_MAX, len)) return -1;
    // sha 可以为 NULL：逐笔命中的判据是“原文逐字节相等”，SHA 只在 miss 时（存条目/磁盘键）才需要。
    if (sha) nvk_sha256_bytes(buf, *len, sha);
    w2_identity_capture(state, pc, &in, buf, *len, w2_epoch, w2_collected);
    return 0;
}

void nvk_id_key_fill(struct nvk_id_key *k, const struct s2a_record_draw_state *st,
                     const struct s2a_direct_compile_context *pc, const struct nvk_env *env) {
    memset(k, 0, sizeof *k);
    k->session = pc->session;
    k->generation = pc->generation;
    k->reggen = s2a_vk_adapter_registry_generation ? s2a_vk_adapter_registry_generation() : 0;
    // 源内容代际：keep 成功/淘汰/清空都推进。同 key 的新内容出现后，旧备忘不能再命中——
    // 短 key 本身不是“内容没变”的证明。
    k->src_epoch = s2a_source_epoch ? s2a_source_epoch() : 0;
    k->closure_hash = env ? env->closure_hash : 0;
    k->prim = st->prim;
    k->rt_count = st->rt_count;
    k->stages = st->stages;
    k->tex_pool_n = st->tex_pool_n;
    k->tex_pool_base = st->tex_pool_base;
    k->samp_pool_n = st->samp_pool_n;
    k->tf_enabled = st->tf_enabled ? 1u : 0u;
    k->ubo_overflow = st->ubo_use_overflow ? 1u : 0u;
    k->state_seen = st->state_seen;
    k->direct_abi = (uint32_t)pc->direct_abi;
    k->cb_draw_params = (uint32_t)(pc->shader_draw_parameters ? 1 : 0);
    // 与 fill_gfx 使用同一份当前 graphics 阶段逐槽绑定事实。
    k->ssbo_bound = graphics_ssbo_bound(st) ? 1u : 0u;
    {
        int known = 0;
        int unaligned = graphics_unaligned_ssbo(st, pc->storage_buffer_offset_alignment, &known);
        k->ssbo_unaligned = unaligned ? 1u : 0u;
        k->ssbo_unaligned_known = known ? 1u : 0u;
    }
    k->depth_mode = &s2a_nvn_depth_mode ? s2a_nvn_depth_mode : 0;
    k->origin_mode = &s2a_nvn_window_origin_mode ? s2a_nvn_window_origin_mode : 0;
    // 顶点属性格式直接进键：attributeTypes 是它的函数，旧键只比了已算好的名字，
    // 改格式而名字相同（如两种 NVN 浮点格式）就会误命中。
    memcpy(k->vattr_fmt, st->vattr_fmt, sizeof k->vattr_fmt);
    // 颜色附件同理：fragmentOutputTypes 由槽位是否绑定、对象是否可信、VkFormat/NVN 格式决定。
    for (unsigned i = 0; i < 8; i++) {
        if (i >= st->rt_count) break;
        k->rt[i].key = st->rt[i].key;
        k->rt[i].format = (uint32_t)st->rt[i].format;
        k->rt[i].nvn_format = st->rt[i].nvn_format;
        k->rt[i].obj_magic = st->rt[i].obj_magic;
    }
}

int nvk_id_memo_match(const struct nvk_id_memo *m, const struct nvk_id_key *k,
                      const struct s2a_record_draw_state *st) {
    if (!m->serial) return 0;
    if (memcmp(&m->k, k, sizeof *k)) return 0;
    if (memcmp(m->keys, st->keys, sizeof m->keys)) return 0;
    if (memcmp(m->ubo_use, st->ubo_use, sizeof m->ubo_use)) return 0;
    if (memcmp(m->state_obj, st->state_obj, sizeof m->state_obj)) return 0;
    if (memcmp(m->tex_present, st->tex_present, sizeof m->tex_present)) return 0;
    if (memcmp(m->tex_handle, st->tex_handle, sizeof m->tex_handle)) return 0;
    return 1;
}

// 备忘的廉价摘要：只用于线性扫描的预筛；真正的命中判据仍是整键+全部状态表逐字节比较。
// 8 槽环在 100+ 程序变体时命中率只有 34%，32 槽环 + 先比摘要可把扫描成本压到常数级。
static uint64_t memo_fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    size_t w = n / 8;
    // 用 memcpy 取 8 字节：state/keys 都不保证 8 字节对齐，直接解引用 uint64_t* 是 UB。
    for (size_t i = 0; i < w; i++) {
        uint64_t v;
        memcpy(&v, b + i * 8, sizeof v);
        h = (h ^ v) * 1099511628211ull;
    }
    for (size_t i = w * 8; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
uint64_t nvk_id_memo_hash(const struct nvk_id_key *k, const struct s2a_record_draw_state *st) {
    uint64_t h = 1469598103934665603ull;
    h = memo_fnv(h, k, sizeof *k);
    h = memo_fnv(h, st->keys, sizeof st->keys);
    h = memo_fnv(h, st->ubo_use, sizeof st->ubo_use);
    for (int s = 0; s < 6; s++) {
        if (!(st->stages & (1u << s))) continue;
        h ^= st->tex_handle[s][0] ^ (st->tex_handle[s][1] << 7) ^ st->tex_handle[s][2];
        h ^= (uint64_t)st->tex_present[s][0] << 11;
        h *= 1099511628211ull;
    }
    h ^= (uint64_t)st->state_seen << 3;
    return h;
}

void nvk_id_memo_store(struct nvk_id_memo *m, const struct nvk_id_key *k,
                       const struct s2a_record_draw_state *st, uint64_t serial, unsigned idx) {
    m->hash = nvk_id_memo_hash(k, st);
    m->k = *k;
    memcpy(m->keys, st->keys, sizeof m->keys);
    memcpy(m->ubo_use, st->ubo_use, sizeof m->ubo_use);
    memcpy(m->state_obj, st->state_obj, sizeof m->state_obj);
    memcpy(m->tex_present, st->tex_present, sizeof m->tex_present);
    memcpy(m->tex_handle, st->tex_handle, sizeof m->tex_handle);
    m->serial = serial;
    m->idx = idx;
}

// ---- W2 冻结注册元数据 producer（graphics 的独立只读支路） ----
// 每线程最多一个采集、一个未领取 capsule、一个本次返回回执；全进程最多 64 个
// capsule。没有按内容键找“最近一次失败”。来源只在 pin 期间访问；两次短 pin 之间
// 保留 ref/epoch/地址数值作为相等证明，不持有可解引用的借用指针，更不永久 pin 源。
#define W2_CAPSULE_LIMIT 64u
#define W2_STRINGS_MAX 8192u
#define W2_ERROR_MAX 8192u
struct w2_raw_entry {
    int id;
    unsigned stage, slot;
    struct nvk_w2_raw_registration raw;
};
struct nvk_w2_capsule {
    _Atomic unsigned refs;
    _Atomic int revoked, published;
    uint64_t epoch;
    struct s2a_input_identity identity;
    struct s2a_compile_graphics_src gfx;
    struct s2a_compile_texture_src textures[NVK_TEXTURE_SET_MAX];
    char strings[W2_STRINGS_MAX];
    size_t strings_used;
    struct { uintptr_t code, control, c1; uint64_t ref; uint32_t c1_bytes; } sources[2];
    struct w2_raw_entry raw[NVK_TEXTURE_SET_MAX];
    size_t raw_count;
    uint8_t canonical[S2A_INPUT_IDENTITY_MAX], sha[32];
    size_t canonical_bytes;
    uint8_t error[W2_ERROR_MAX];
    size_t error_bytes;
    struct s2a_w2_query_input input;
};
struct w2_thread {
    struct nvk_w2_capsule *pending, *returned;
    int consumed;
    struct {
        int active, valid;
        uint32_t stages;
        uint64_t generation, epoch;
        const struct s2a_compile_texture_src *allocation;
        struct nvk_texture_set facts;
        struct w2_raw_entry raw[NVK_TEXTURE_SET_MAX];
        size_t count;
    } collection;
};
static pthread_once_t w2_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t w2_key;
static _Atomic int w2_key_valid;
static _Atomic unsigned w2_capsules;
static _Atomic uint64_t w2_epoch = 1, w2_serial = 1;
static _Atomic int w2_stopped;

void nvk_w2_capsule_release(struct nvk_w2_capsule *cap) {
    if (cap && atomic_fetch_sub(&cap->refs, 1) == 1) {
        memset(cap, 0, sizeof *cap);
        free(cap);
        atomic_fetch_sub(&w2_capsules, 1);
    }
}
static void w2_thread_destroy(void *p) {
    struct w2_thread *t = p;
    if (!t) return;
    nvk_w2_capsule_release(t->pending);
    nvk_w2_capsule_release(t->returned);
    free(t);
}
static void w2_make_key(void) { w2_key_valid = pthread_key_create(&w2_key, w2_thread_destroy) == 0; }
static struct w2_thread *w2_thread_get(int create) {
    // provider 只读已创建的 TLS，不在 provider_lock 下做 once/分配/释放。
    if (create) pthread_once(&w2_key_once, w2_make_key);
    if (!w2_key_valid) return NULL;
    struct w2_thread *t = pthread_getspecific(w2_key);
    if (!t && create) {
        t = calloc(1, sizeof *t);
        if (t && pthread_setspecific(w2_key, t)) { free(t); t = NULL; }
    }
    return t;
}
static int w2_identity_begin(void) {
    struct w2_thread *t = w2_thread_get(0);
    if (!t) return 0;
    nvk_w2_capsule_release(t->pending); t->pending = NULL;
    int collected = t->collection.valid;
    t->collection.valid = 0; // even an early identity error consumes this collection
    return collected;
}
static int w2_open(void) {
    return !atomic_load(&w2_stopped) && s2a_compile_export_open && s2a_compile_export_open();
}
static int w2_alive(const struct nvk_w2_capsule *cap) {
    return cap && !atomic_load(&cap->revoked) && !atomic_load(&w2_stopped) &&
        cap->epoch == atomic_load(&w2_epoch);
}
void nvk_w2_capsule_abandon(struct nvk_w2_capsule *cap, int reason) {
    (void)reason;
    if (cap) atomic_store(&cap->revoked, 1);
}
void nvk_w2_diagnostic_cancel(void) {
    atomic_store(&w2_stopped, 1);
    atomic_fetch_add(&w2_epoch, 1);
}
void nvk_w2_diagnostic_resume(void) {
    atomic_fetch_add(&w2_epoch, 1);
    atomic_store(&w2_stopped, 0);
}
void nvk_w2_capsule_returned(struct nvk_w2_capsule *cap) {
    struct w2_thread *t = w2_thread_get(cap != NULL);
    if (!t) return;
    struct nvk_w2_capsule *old = t->returned;
    t->returned = NULL;
    t->consumed = 0;
    if (w2_alive(cap) && atomic_load(&cap->published) == 1) {
        atomic_fetch_add(&cap->refs, 1);
        t->returned = cap;
    }
    nvk_w2_capsule_release(old);
}
static int w2_string(struct nvk_w2_capsule *cap, const char *s, const char **out) {
    if (!s) { *out = NULL; return 0; }
    size_t n = 0, available = sizeof cap->strings - cap->strings_used;
    while (n < available && s[n]) n++;
    if (n == available) return -1;
    char *d = cap->strings + cap->strings_used;
    memcpy(d, s, n + 1); cap->strings_used += n + 1; *out = d;
    return 0;
}
static int w2_clone_identity(struct nvk_w2_capsule *cap, const struct s2a_input_identity *in) {
    cap->identity = *in; cap->gfx = *in->gfx;
    cap->identity.gfx = &cap->gfx; cap->identity.textures = cap->textures;
    if (w2_string(cap, in->translator_closure, &cap->identity.translator_closure) ||
        w2_string(cap, in->gfx->topology, &cap->gfx.topology) ||
        w2_string(cap, in->gfx->alpha_test_compare, &cap->gfx.alpha_test_compare)) return -1;
    for (int i = 0; i < 8; i++)
        if (w2_string(cap, in->gfx->fragment_output_types[i], &cap->gfx.fragment_output_types[i])) return -1;
    for (int i = 0; i < 32; i++)
        if (w2_string(cap, in->gfx->attribute_types[i], &cap->gfx.attribute_types[i])) return -1;
    for (int i = 0; i < in->texture_count; i++) {
        cap->textures[i] = in->textures[i];
        if (w2_string(cap, in->textures[i].stage, &cap->textures[i].stage) ||
            w2_string(cap, in->textures[i].format, &cap->textures[i].format) ||
            w2_string(cap, in->textures[i].sampler_type, &cap->textures[i].sampler_type)) return -1;
    }
    return 0;
}
static int w2_same_string(const char *a, const char *b) {
    return a && b ? !strcmp(a, b) : a == b;
}
static int w2_same_facts(const struct nvk_texture_set *set,
                         const struct s2a_compile_texture_src *items, int count) {
    if (count != set->count || count < 0 || count > NVK_TEXTURE_SET_MAX || (count && !items)) return 0;
    for (int i = 0; i < count; i++) {
        const struct s2a_compile_texture_src *a = &set->items[i], *b = &items[i];
        if (!w2_same_string(a->stage, b->stage) || !w2_same_string(a->format, b->format) ||
            !w2_same_string(a->sampler_type, b->sampler_type) || a->handle != b->handle ||
            a->cbuf_slot != b->cbuf_slot || a->is_buffer != b->is_buffer || a->normalized != b->normalized ||
            a->separate != b->separate || a->array_length != b->array_length) return 0;
    }
    return 1;
}
static int w2_slot_id(const struct s2a_record_draw_state *state, unsigned stage, unsigned slot) {
    return state->tex_present[stage][slot] ? (int)(state->tex_handle[stage][slot] & 0xfffffu) : state->tex_pool_base;
}
static void w2_collection_begin(const struct s2a_record_draw_state *state, struct nvk_texture_set *set) {
    struct w2_thread *t = w2_thread_get(w2_open());
    if (!t) return;
    nvk_w2_capsule_release(t->pending); t->pending = NULL;
    memset(&t->collection, 0, sizeof t->collection);
    if (!w2_open() || !state || !state->stages || (state->stages & ~3u) ||
        !s2a_vk_adapter_w2_capture_registered || !s2a_vk_adapter_registry_generation) return;
    t->collection.epoch = atomic_load(&w2_epoch);
    int ids[NVK_TEXTURE_SET_MAX]; struct nvk_w2_raw_registration raws[NVK_TEXTURE_SET_MAX];
    for (unsigned stage = 0; stage < 2; stage++) if (state->stages & (1u << stage)) {
        for (unsigned slot = 0; slot < 32; slot++) {
            size_t i = t->collection.count++;
            ids[i] = w2_slot_id(state, stage, slot);
            t->collection.raw[i].id = ids[i];
            t->collection.raw[i].stage = stage; t->collection.raw[i].slot = slot;
        }
    }
    if (s2a_vk_adapter_w2_capture_registered(ids, t->collection.count, raws, &t->collection.generation)) return;
    for (size_t i = 0; i < t->collection.count; i++) t->collection.raw[i].raw = raws[i];
    t->collection.stages = state->stages;
    t->collection.allocation = set->items;
    t->collection.active = t->collection.valid = 1;
}
static void w2_collection_observe(int id, unsigned stage, unsigned slot, int have, const struct nvk_tex_desc *td) {
    struct w2_thread *t = w2_thread_get(0);
    if (!t || !t->collection.active || !t->collection.valid) return;
    size_t n;
    for (n = 0; n < t->collection.count; n++) {
        const struct w2_raw_entry *r = &t->collection.raw[n];
        if (r->stage != stage || r->slot != slot) continue;
        if (r->id != id || !!r->raw.present != !!have || (have && (!td ||
            r->raw.format != td->nvn_format || r->raw.target != td->nvn_target || r->raw.layers != td->layers)))
            t->collection.valid = 0;
        break;
    }
    if (n == t->collection.count) t->collection.valid = 0;
}
static void w2_collection_end(struct nvk_texture_set *set) {
    struct w2_thread *t = w2_thread_get(0);
    if (!t || !t->collection.active) return;
    t->collection.active = 0;
    if (atomic_load(&w2_stopped) || t->collection.epoch != atomic_load(&w2_epoch) ||
        !s2a_vk_adapter_registry_generation ||
        t->collection.generation != s2a_vk_adapter_registry_generation()) t->collection.valid = 0;
    if (!t->collection.valid) return;
    t->collection.facts = *set;
    for (int i = 0; i < set->count; i++) {
        t->collection.facts.items[i].format = t->collection.facts.fmt[i];
        t->collection.facts.items[i].sampler_type = t->collection.facts.smp[i];
        // stage is one of the production static literals; it never borrows state storage.
    }
}
static int w2_source_matches(const struct s2a_source_entry_view *v,
                             const struct s2a_input_identity *in, unsigned i) {
    return v->complete && v->ref && v->code && v->control &&
        v->code_bytes == in->source_code_bytes[i] && v->control_bytes == in->source_control_bytes[i] &&
        !memcmp(v->content_sha, in->source_sha[i], 32);
}
static void w2_identity_capture(const struct s2a_record_draw_state *state,
    const struct s2a_direct_compile_context *pc, const struct s2a_input_identity *in,
    const uint8_t *canonical, size_t canonical_bytes, uint64_t source_epoch, int collected) {
    struct w2_thread *t = w2_thread_get(0);
    if (!t) return;
    nvk_w2_capsule_release(t->pending); t->pending = NULL;
    if (!collected || t->collection.epoch != atomic_load(&w2_epoch) || !w2_open() || !pc->session || !pc->generation || !source_epoch ||
        !s2a_source_epoch || !s2a_source_acquire || !s2a_source_release ||
        source_epoch != s2a_source_epoch() || canonical_bytes > S2A_INPUT_IDENTITY_MAX ||
        in->stage_mask != t->collection.stages || in->textures != t->collection.allocation ||
        !w2_same_facts(&t->collection.facts, in->textures, in->texture_count)) return;
    for (size_t i = 0; i < t->collection.count; i++) {
        const struct w2_raw_entry *r = &t->collection.raw[i];
        if (w2_slot_id(state, r->stage, r->slot) != r->id) return;
    }
    if (atomic_fetch_add(&w2_capsules, 1) >= W2_CAPSULE_LIMIT) { atomic_fetch_sub(&w2_capsules, 1); return; }
    struct nvk_w2_capsule *cap = calloc(1, sizeof *cap);
    if (!cap) { atomic_fetch_sub(&w2_capsules, 1); return; }
    atomic_init(&cap->refs, 1); atomic_init(&cap->revoked, 0); atomic_init(&cap->published, 0);
    cap->epoch = t->collection.epoch;
    cap->raw_count = t->collection.count;
    memcpy(cap->raw, t->collection.raw, cap->raw_count * sizeof cap->raw[0]);
    cap->canonical_bytes = canonical_bytes; memcpy(cap->canonical, canonical, canonical_bytes);
    nvk_sha256_bytes(canonical, canonical_bytes, cap->sha);
    if (w2_clone_identity(cap, in)) goto reject;
    uint8_t check[S2A_INPUT_IDENTITY_MAX]; size_t check_bytes = 0;
    if (s2a_input_identity_encode(&cap->identity, check, sizeof check, &check_bytes) ||
        check_bytes != canonical_bytes || memcmp(check, canonical, check_bytes)) goto reject;
    for (unsigned i = 0; i < 2 && (in->source_present & (1u << i)); i++) {
        struct s2a_source_entry_view view;
        if (s2a_source_acquire(in->keys[i], &view) != 1) goto reject;
        int match = w2_source_matches(&view, in, i);
        if (match) {
            cap->sources[i].code = (uintptr_t)view.code;
            cap->sources[i].control = (uintptr_t)view.control;
            cap->sources[i].c1 = (uintptr_t)view.c1;
            cap->sources[i].ref = view.ref; cap->sources[i].c1_bytes = view.c1_bytes;
        }
        s2a_source_release(view.ref); // no offline/cache-hit path retains source pins
        if (!match) goto reject;
    }
    if (source_epoch != s2a_source_epoch() || !w2_alive(cap)) goto reject;
    cap->input.enabled = 1; cap->input.identity = &cap->identity;
    cap->input.original.canonical = cap->canonical; cap->input.original.canonical_bytes = canonical_bytes;
    struct s2a_w2_query_stamp *stamp = &cap->input.original.stamp;
    stamp->session = pc->session; stamp->generation = pc->generation;
    stamp->snapshot_id = atomic_fetch_add(&w2_serial, 1); stamp->snapshot_version = 1;
    stamp->source_epoch = source_epoch; stamp->registry_generation = t->collection.generation;
    if (!stamp->snapshot_id || !stamp->registry_generation) goto reject;
    cap->input.current = *stamp;
    t->pending = cap;
    return;
reject:
    nvk_w2_capsule_release(cap);
}
int nvk_w2_capsule_claim(const struct s2a_compile_request_input *in, struct nvk_w2_capsule **out) {
    if (!out) return -1;
    *out = NULL;
    struct w2_thread *t = w2_thread_get(0);
    if (!t) return -1;
    struct nvk_w2_capsule *cap = t->pending; t->pending = NULL;
    if (!cap) return -1;
    if (!in || !w2_open() || !w2_alive(cap) || !s2a_source_epoch ||
        cap->input.original.stamp.source_epoch != s2a_source_epoch() ||
        in->session != cap->input.original.stamp.session || in->generation != cap->input.original.stamp.generation ||
        in->stage_count < 1 || in->stage_count > 2 || !in->profile_json || !in->profile_bytes) goto reject;
    // Linked policy is request-local identity, not a diagnostic normalization.
    // Reject before deriving correspondence; keep the frozen capsule untouched.
    if ((in->requires_linked_varying_interface != 0 && in->requires_linked_varying_interface != 1) ||
        in->requires_linked_varying_interface != cap->identity.requires_linked_varying_interface) goto reject;
    struct s2a_input_identity check = cap->identity;
    // A request may add tess after lookup identity creation. Keep both inputs
    // untouched: derive a separate expected request from the original owned
    // identity and the very same pinned source, then compare every encoded field.
    struct s2a_compile_graphics_src expected_gfx = cap->gfx;
    int derived_tess = 0;
    check.direct_abi = in->direct_abi; check.translator_closure = in->translator_closure;
    nvk_sha256_bytes((const uint8_t *)in->profile_json, in->profile_bytes, check.profile_sha);
    check.gfx = &in->gfx; check.textures = in->textures; check.texture_count = in->texture_count;
    check.texture_array_length = in->texture_array_length; check.sampler_array_length = in->sampler_array_length;
    check.transform_feedback_enabled = in->transform_feedback_enabled;
    uint32_t mask = 0;
    for (int i = 0; i < in->stage_count; i++) {
        const struct s2a_compile_stage_src *s = &in->stages[i];
        unsigned stage;
        if (s->stage && !strcmp(s->stage, "vs")) stage = 0;
        else if (s->stage && !strcmp(s->stage, "fs")) stage = 1;
        else goto reject;
        if ((mask & (1u << stage)) || (i && stage == 0)) goto reject;
        mask |= 1u << stage; check.ubo_use[stage] = s->constant_buffer_use;
        struct s2a_source_entry_view view;
        if (s2a_source_acquire(check.keys[i], &view) != 1) goto reject;
        int match = w2_source_matches(&view, &cap->identity, (unsigned)i) && view.ref == cap->sources[i].ref &&
            (uintptr_t)view.code == cap->sources[i].code && (uintptr_t)view.control == cap->sources[i].control &&
            (uintptr_t)view.c1 == cap->sources[i].c1 && view.c1_bytes == cap->sources[i].c1_bytes &&
            s->control == view.control && s->control_bytes == view.control_bytes &&
            s->c1 == view.c1 && s->c1_bytes == view.c1_bytes;
        if (match) {
            int original = s->code == view.code && s->code_bytes == view.code_bytes;
            int stripped = view.code_bytes > 0x30 && view.code[0] == 0x78 && view.code[1] == 0x56 &&
                view.code[2] == 0x34 && view.code[3] == 0x12 && s->code == view.code + 0x30 &&
                s->code_bytes == view.code_bytes - 0x30;
            match = original || stripped;
        }
        if (match && !expected_gfx.has_tess_mode && view.control_bytes >= 0x748) {
            // Match draw's first eligible source, including a real zero value.
            // This read occurs while that exact original source is pinned.
            memcpy(&expected_gfx.tess_mode, view.control + 0x744, 4);
            expected_gfx.has_tess_mode = 1;
            derived_tess = 1;
        }
        s2a_source_release(view.ref);
        if (!match) goto reject;
    }
    if (mask != cap->identity.stage_mask) goto reject;
    uint8_t bytes[S2A_INPUT_IDENTITY_MAX]; size_t n = 0;
    if (s2a_input_identity_encode(&check, bytes, sizeof bytes, &n)) goto reject;
    if (n != cap->canonical_bytes || memcmp(bytes, cap->canonical, n)) {
        // Not identity equality: only this explicitly source-derived relation
        // is permitted. Never clear request tess or normalize the lookup key.
        if (!derived_tess || in->gfx.has_tess_mode != 1) goto reject;
        struct s2a_input_identity expected = cap->identity;
        expected.gfx = &expected_gfx;
        uint8_t expected_bytes[S2A_INPUT_IDENTITY_MAX]; size_t expected_n = 0;
        if (s2a_input_identity_encode(&expected, expected_bytes, sizeof expected_bytes, &expected_n) ||
            n != expected_n || memcmp(bytes, expected_bytes, n)) goto reject;
    }
    if (cap->input.original.stamp.source_epoch != s2a_source_epoch() || !w2_alive(cap)) goto reject;
    *out = cap;
    return 0;
reject:
    nvk_w2_capsule_release(cap);
    return -1;
}
int nvk_w2_capsule_publish(struct nvk_w2_capsule *cap, const struct s2a_w2_query *query,
                          const uint8_t *error, size_t error_bytes) {
    if (!w2_alive(cap) || !query || !error || !error_bytes || error_bytes > sizeof cap->error ||
        atomic_load(&cap->published)) return -1;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&cap->published, &expected, -1)) return -1;
    cap->input.query = *query; memcpy(cap->error, error, error_bytes); cap->error_bytes = error_bytes;
    if (!w2_alive(cap)) return -1;
    atomic_store(&cap->published, 1);
    return w2_alive(cap) ? 0 : -1;
}
static int w2_resolve(void *snapshot, const struct s2a_w2_query *q, struct s2a_w2_query_source *source) {
    const struct nvk_w2_capsule *cap = snapshot;
    if (!w2_alive(cap) || !q || !source || q->coordinate.stage > 1 || q->coordinate.handle < 8 ||
        q->coordinate.handle > 70 || (q->coordinate.handle - 8) % 2) return -1;
    unsigned slot = (q->coordinate.handle - 8) / 2;
    for (size_t i = 0; i < cap->raw_count; i++) {
        const struct w2_raw_entry *r = &cap->raw[i];
        if (r->stage != q->coordinate.stage || r->slot != slot) continue;
        if (!r->raw.present || !r->raw.layers || (r->raw.view_present &&
            (!r->raw.view_complete || !r->raw.view_layers))) return -1;
        memset(source, 0, sizeof *source);
        source->binding = cap->input.original; source->requested = *q;
        source->resolved = q->coordinate; source->resolved.cbuf_slot = -1;
        source->source_id = i + 1; // immutable capsule entry id, not a pool incarnation/guest address
        source->descriptor = (struct s2a_w2_query_raw_texture){S2A_W2_RAW_COMPLETE,
            r->raw.format, r->raw.target, r->raw.layers};
        source->view_present = r->raw.view_present;
        if (r->raw.view_present) source->view = (struct s2a_w2_query_raw_texture){S2A_W2_RAW_COMPLETE,
            r->raw.view_format, r->raw.view_target, r->raw.view_layers};
        // A Stop overlapping an already-entered callback may linearize after
        // this read; owned metadata remains safe. Calls entering after Stop fail.
        return w2_alive(cap) ? 0 : -1;
    }
    return -1;
}
static int w2_provider(void *ctx, const struct s2a_compile_export_w2_key *key,
    const struct s2a_w2_query_input **input, s2a_w2_query_resolver *resolver, void **snapshot) {
    (void)ctx;
    struct w2_thread *t = w2_thread_get(0);
    if (!t || t->consumed) return -1;
    t->consumed = 1; // even a wrong key consumes this return, never fall back to another attempt
    struct nvk_w2_capsule *cap = t->returned;
    if (!w2_alive(cap) || atomic_load(&cap->published) != 1 || !key ||
        key->session != cap->input.original.stamp.session || key->generation != cap->input.original.stamp.generation ||
        memcmp(key->input_sha, cap->sha, 32)) return -1;
    *input = &cap->input; *resolver = w2_resolve; *snapshot = cap;
    return 0;
}
void nvk_w2_diagnostic_init(void) {
    pthread_once(&w2_key_once, w2_make_key);
    if (w2_key_valid && s2a_compile_export_w2_set_provider) s2a_compile_export_w2_set_provider(w2_provider, NULL);
}
