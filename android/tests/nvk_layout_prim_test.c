// #1581 M：着色器来源（nvk_shader.c）的两处事实，用生产函数直接验：
//   ① 句柄表贴图数组的布局项（layout_ent）：数组第 k 项 = c2 第 slot + 2k 字（同 iOS NVN_RB_TEXA：c2 字节 off + 8k），
//      布局项的项数必须等于数组长度——项数少了，描述符布局的 descriptorCount 小于着色器声明，越界的项读到的是未定义的描述符。
//   ② halvePrimitiveId（fill_facts）：同 Ryujinx GpuChannelGraphicsState——不支持四边形的宿主、无几何着色器、图元是 Quads / QuadStrip。
// 期望值都直接写常量。
#include "../native/gfx_plat/nvk_shader.c"

// 贴图登记表替身：本测试不登记贴图（组合槽事实按「没登记」出），与生产语义一致：号没登记 = 0
int rp_tex_snap_by_id(int id, tex_t *t, view_t *v, int *has_v) { (void)id; (void)t; (void)v; (void)has_v; return 0; }
// 其余平台 / 核心件的替身（同 nvk_shader_pool_test.c）：缺省状态、没有能力位、没有登记池、日志丢弃
void nvn_plat_log(int level, const char *cls, const char *text) { (void)level; (void)cls; (void)text; }
int rp_depth_zero(void) { return 0; }
int rp_origin_lower_left(void) { return 0; }
void nvn_gpu_caps_get(nvn_gpu_caps *out) { memset(out, 0, sizeof *out); }
int rp_pool_find(uint64_t addr, uint64_t len, const uint8_t **mem, uint64_t *size) { (void)addr; (void)len; (void)mem; (void)size; return 0; }

static int g_bad;
#define EXPECT(c, ...) do { if (!(c)) { g_bad++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static struct fd_binding tex_binding(int slot, int count) {
    struct fd_binding b; memset(&b, 0, sizeof b);
    b.kind = 2; b.slot = slot; b.count = (uint16_t)count; b.samp_slot = -1;
    b.tex_slot = slot >= 8 && slot < 72 && !(slot & 1) ? (int16_t)((slot - 8) / 2) : -1;   // 结果解析（nvn_vk_program_result.c）的组合槽换算
    snprintf(b.stage, sizeof b.stage, "fs");
    return b;
}

static void arrays(void) {
    // 句柄表下标 = c2 字 / 2；组合槽 s 在句柄表第 4 + s 项（rp_draw.c RP_HANDLE_TABLE_FIRST_SLOT）
    static const struct { int slot, count, ok; uint16_t src_reg, n; } k[] = {
        {0, 8, 1, 0, 8},      // 从 c2 第 0 字起的整表数组（原有）
        {8, 4, 1, 4, 4},      // 从组合槽 0 起的 sampler2D tex[4]
        {20, 3, 1, 10, 3},    // 从组合槽 6 起
        {66, 4, 1, 33, 4},    // 组合槽 29..32：越出组合表的项同整表数组的口径（回放核心按项读作池里第 0 项）
        {9, 2, 0, 0, 0},      // 奇数字不是句柄起点
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        struct fd_binding b = tex_binding(k[i].slot, k[i].count);
        nvn_gpu_layout_ent e; memset(&e, 0xA5, sizeof e);
        int ok = layout_ent(&b, &e);
        EXPECT(ok == k[i].ok, "array at c2 word %d x%d: layout_ent=%d want %d", k[i].slot, k[i].count, ok, k[i].ok);
        if (ok && k[i].ok)
            EXPECT(e.cls == NVN_GPU_RES_TEX_SAMP && e.src_kind == NVN_RB_TEX && e.count == k[i].n && e.src_reg == k[i].src_reg && e.src_reg2 == UINT16_MAX,
                   "array at c2 word %d x%d: cls %u kind %u count %u src_reg %u sampler %u (want count %u src_reg %u)",
                   k[i].slot, k[i].count, e.cls, e.src_kind, e.count, e.src_reg, e.src_reg2, k[i].n, k[i].src_reg);
    }
    // 单个组合槽不受影响：槽 3（c2 第 14 字），项数 1
    struct fd_binding b = tex_binding(14, 1); nvn_gpu_layout_ent e;
    EXPECT(layout_ent(&b, &e) && e.count == 1 && e.src_reg == 3 && e.src_kind == NVN_RB_TEX, "single combined slot 3: count %u src_reg %u", e.count, e.src_reg);
}

static void halve(void) {
    static const struct { uint32_t prim; int nst, want; } k[] = {
        {7, 2, 1}, {8, 2, 1},           // 四边形 / 四边形带
        {4, 2, 0}, {5, 2, 0}, {6, 2, 0}, {9, 2, 0}, {0, 2, 0}, {2, 2, 0},
        {7, 3, 0},                      // 有几何着色器：不减半（Ryujinx !hasGeometryShader）
    };
    struct s2a_direct_compile_context pc; memset(&pc, 0, sizeof pc);
    static struct facts f;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        rp_state_reset();
        struct rp_shader_req q; memset(&q, 0, sizeof q); q.prim = k[i].prim; q.rt_count = 1; q.rt_format[0] = NVN_FORMAT_RGBA8;
        fill_facts(&q, &pc, &f, k[i].nst);
        EXPECT(f.gfx.has_halve_primitive_id && f.gfx.halve_primitive_id == k[i].want, "prim %u stages %d: halvePrimitiveId %d want %d",
               k[i].prim, k[i].nst, f.gfx.halve_primitive_id, k[i].want);
    }
}

int main(void) {
    arrays();
    halve();
    if (g_bad) { fprintf(stderr, "nvk_layout_prim_test: %d failures\n", g_bad); return 1; }
    puts("PASS: handle-table arrays keep their length at any c2 start; halvePrimitiveId for quads / quad strips without GS");
    return 0;
}
