// #999：图形阶段的存储图像（kind 3，含 imageBuffer）。真实结果解析（nvn_vk_program_parse_graphics）+ 生产 layout_ent + 生产 build，不另写模型：
//   * 句柄字 72..86（偶）→ NVN 图像槽 (字 − 72) / 2 放进 tex_slot（同计算阶段）；别的字 / 数组保持 -1，由 layout_ent 具名拒，不猜槽位；
//   * 布局项 NVN_RB_IMG：普通图像 IMAGE、imageBuffer IMAGE_TBUF，读写；
//   * build 把着色器里声明的目标 / 数值类 / SPIR-V Image Format 记进程序记录（回放按声明选视图；计算阶段 build_compute 同一口径），
//     交给后端的布局就是程序记录里的那份；
//   * 贴图（kind 2）的字 72..86 仍是组合槽 32..39，图像的改动不碰它。
// 着色器是手搭的最小 SPIR-V（只有装饰 / 类型 / 变量，同 image_decl_test.c）；后端着色器装载是桩：只记下收到的布局。
#include "../native/gfx_plat/nvk_shader.c"
#include "tbuf_spirv_fixtures.h"

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", what); fails++; } } while (0)

// ---- 后端桩：记下每个阶段收到的布局项 ----
static unsigned g_loads;
static unsigned g_got_nent[6];
static nvn_gpu_layout_ent g_got_ent[6][RP_PROG_ENT_MAX];
nvn_gpu_rc nvn_gpu_shader_load(const nvn_gpu_shader_src *s, nvn_gpu_shader *o, nvn_gpu_err *e) {
    (void)e;
    g_loads++;
    if ((unsigned)s->stage < 6 && s->layout) {
        g_got_nent[s->stage] = s->layout->nent;
        for (uint32_t i = 0; i < s->layout->nent && i < RP_PROG_ENT_MAX; i++) g_got_ent[s->stage][i] = s->layout->ent[i];
    }
    *o = (nvn_gpu_shader)(uintptr_t)(g_loads + 1);
    return (nvn_gpu_rc){NVN_GPU_OK};
}
void nvn_gpu_shader_release(nvn_gpu_shader s) { (void)s; }
// 平台桩：开关取缺省值；红行记下最后一条（失败时好定位）
static char g_last_red[256];
int nvn_plat_flag(const char *name, int dflt) { (void)name; return dflt; }
const char *nvn_plat_dir(int which) { (void)which; return NULL; }
void nvn_plat_safepoint(void) {}
void nvn_plat_log(int level, const char *cls, const char *text) {
    if (level == NVN_PLAT_LOG_RED && !strcmp(cls, "shader")) snprintf(g_last_red, sizeof g_last_red, "%s", text);
}
void nvn_gpu_caps_get(nvn_gpu_caps *out) { memset(out, 0, sizeof *out); }

// ---- 手搭 SPIR-V：每个存储图像一个变量（DescriptorSet 3 / Binding = 序号）----
struct img_decl { uint32_t sampled_type;   // 1 = uint、2 = float、3 = sint
                  uint32_t dim;            // 1 = 2D、5 = Buffer
                  uint32_t format; };      // SPIR-V Image Format 枚举
static uint32_t g_w[256]; static size_t g_n;
static void op(uint32_t opcode, int count, ...) {
    va_list ap; va_start(ap, count); g_w[g_n++] = (uint32_t)count << 16 | opcode;
    for (int i = 1; i < count; i++) g_w[g_n++] = va_arg(ap, uint32_t);
    va_end(ap);
}
static void module_build(const struct img_decl *d, int n) {
    const uint32_t bound = 4u + 3u * (uint32_t)n;
    g_w[0] = 0x07230203u; g_w[1] = 0x00010300u; g_w[2] = 0; g_w[3] = bound; g_w[4] = 0; g_n = 5;
    for (int i = 0; i < n; i++) { uint32_t var = 6u + 3u * (uint32_t)i; op(71, 4, var, 34u, 3u); op(71, 4, var, 33u, (uint32_t)i); }
    op(21, 4, 1u, 32u, 0u); op(22, 3, 2u, 32u); op(21, 4, 3u, 32u, 1u);
    for (int i = 0; i < n; i++) op(25, 9, 4u + 3u * (uint32_t)i, d[i].sampled_type, d[i].dim, 0u, 0u, 0u, 2u, d[i].format);
    for (int i = 0; i < n; i++) op(32, 4, 5u + 3u * (uint32_t)i, 0u, 4u + 3u * (uint32_t)i);
    for (int i = 0; i < n; i++) op(59, 4, 5u + 3u * (uint32_t)i, 6u + 3u * (uint32_t)i, 0u);
}
static void b64(const uint8_t *in, size_t n, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63]; out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < n ? t[v >> 6 & 63] : '='; out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = 0;
}

// 一份 vs + fs 的 v2 结果：fs 里有 n 个存储图像变量，bindings 由调用方给（对象列表的原文）
static int make_json(char *json, size_t cap, const struct img_decl *d, int n, const char *bindings) {
    static char fs[2048];
    module_build(d, n);
    b64((const uint8_t *)g_w, g_n * 4, fs);
    return snprintf(json, cap,
        "{\"schema\":\"s2a-direct-program-v2\",\"content\":{\"contract\":\"s2a-direct-program-content-v2\","
        "\"stages\":[{\"stage\":\"vs\",\"spirv\":\"%s\"},{\"stage\":\"fs\",\"spirv\":\"%s\"}],\"bindings\":[%s]}}",
        S2A_TEST_VERT_1, fs, bindings);
}
// 一个 fs 图像绑定（序号 b、句柄字 slot、可选的原文字段）
static const char *img_binding(char *out, size_t cap, int b, int slot, int count, const char *extra) {
    snprintf(out, cap, "{\"stage\":\"fs\",\"kind\":\"image\",\"set\":3,\"binding\":%d,\"count\":%d,\"slot\":%d%s%s}", b, count, slot, extra[0] ? "," : "", extra);
    return out;
}

// 真实解析 + 生产布局映射（不经 build）。返回 1 = 解析并映射成功；*tex = 解析器给的 tex_slot
static int map_img(int slot, int count, const char *extra, uint32_t dim, nvn_gpu_layout_ent *e, int *tex, char *why, size_t cap) {
    const struct img_decl d[1] = {{1, dim, 33}};
    char json[8192], bind[256];
    int n = make_json(json, sizeof json, d, 1, img_binding(bind, sizeof bind, 0, slot, count, extra));
    struct fd_program p; memset(&p, 0, sizeof p); memset(e, 0, sizeof *e); why[0] = 0; *tex = -99;
    if (n <= 0 || (size_t)n >= sizeof json) return 0;
    int rc = nvn_vk_program_parse_graphics((const uint8_t *)json, (size_t)n, &p, why, cap);
    int ok = 0;
    if (!rc && p.binding_count == 1) { *tex = p.bindings[0].tex_slot; ok = layout_ent(&p.bindings[0], e); }
    nvn_vk_program_free(&p);
    return ok;
}

int main(void) {
    nvn_gpu_layout_ent e;
    char why[192];
    int tex;

    // 1) 解析 + 布局：偶字 72..86 → 图像槽 0..7（NVN_RB_IMG、IMAGE、读写）
    for (int k = 0; k < 8; k++) {
        char what[320];
        int ok = map_img(72 + 2 * k, 1, "", 1, &e, &tex, why, sizeof why);
        snprintf(what, sizeof what, "word %d maps to image slot %d (tex_slot=%d why=%s)", 72 + 2 * k, k, tex, why);
        CHECK(ok && tex == k && e.cls == NVN_GPU_RES_IMAGE && e.src_kind == NVN_RB_IMG && e.src_reg == k && e.count == 1 &&
              e.access == (NVN_GPU_ACC_R | NVN_GPU_ACC_W), what);
    }
    // imageBuffer：着色器里的图像是 Buffer 维度（来源字段同写 buffer-image）
    {
        int ok = map_img(76, 1, "\"source\":\"buffer-image\"", 5, &e, &tex, why, sizeof why);
        CHECK(ok && tex == 2 && e.cls == NVN_GPU_RES_IMAGE_TBUF && e.src_kind == NVN_RB_IMG && e.src_reg == 2, "buffer image maps to IMAGE_TBUF on image slot 2");
    }
    // 2) 表达不了的不猜槽位：tex_slot 保持 -1，layout_ent 具名拒（程序整份失败，不是静默绑到别的槽）
    static const struct { int slot, count; const char *what; } bad[] = {
        {73, 1, "odd word 73"}, {71, 1, "word 71 below the image table"}, {88, 1, "word 88 above the image table"},
        {90, 1, "word 90 (separate texture table)"}, {8, 1, "word 8 (combined texture slots)"}, {0, 1, "word 0"}, {72, 2, "image array at word 72"}};
    for (unsigned k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        char what[320];
        int ok = map_img(bad[k].slot, bad[k].count, "", 1, &e, &tex, why, sizeof why);
        snprintf(what, sizeof what, "%s: tex_slot stays -1 and layout_ent refuses (tex_slot=%d ok=%d)", bad[k].what, tex, ok);
        CHECK(!ok && tex == -1, what);
    }
    // 没有 slot 字段（反射没给槽位）：不猜
    {
        static const struct img_decl d[1] = {{1, 1, 33}};
        char json[8192];
        int n = make_json(json, sizeof json, d, 1, "{\"stage\":\"fs\",\"kind\":\"image\",\"set\":3,\"binding\":0,\"count\":1}");
        struct fd_program p; memset(&p, 0, sizeof p);
        int rc = nvn_vk_program_parse_graphics((const uint8_t *)json, (size_t)n, &p, why, sizeof why);
        CHECK(!rc && p.binding_count == 1 && p.bindings[0].tex_slot == -1 && !layout_ent(&p.bindings[0], &e), "an image without a reflected word is refused, not guessed");
        nvn_vk_program_free(&p);
    }
    // 3) 对照：贴图（kind 2）的字 72..86 仍是组合槽 32..39，不受图像改动影响
    {
        char json[4096];
        int n = snprintf(json, sizeof json,
            "{\"schema\":\"s2a-direct-program-v2\",\"content\":{\"contract\":\"s2a-direct-program-content-v2\","
            "\"stages\":[{\"stage\":\"vs\",\"spirv\":\"%s\"},{\"stage\":\"fs\",\"spirv\":\"%s\"}],"
            "\"bindings\":[{\"stage\":\"fs\",\"kind\":\"texture\",\"set\":2,\"binding\":0,\"count\":1,\"slot\":72}]}}", S2A_TEST_VERT_1, S2A_TEST_FRAG_1);
        struct fd_program p; memset(&p, 0, sizeof p);
        int rc = nvn_vk_program_parse_graphics((const uint8_t *)json, (size_t)n, &p, why, sizeof why);
        CHECK(!rc && p.binding_count == 1 && p.bindings[0].tex_slot == 32 && layout_ent(&p.bindings[0], &e) && e.src_kind == NVN_RB_TEX && e.src_reg == 32,
              "a texture at word 72 keeps combined slot 32");
        nvn_vk_program_free(&p);
    }

    // 4) 生产 build：四个图像（uint R32ui / float 无格式 / sint Rg32i / imageBuffer R32ui）→ 布局项 + 声明
    {
        static const struct img_decl d[4] = {{1, 1, 33}, {2, 1, 0}, {3, 1, 25}, {1, 5, 33}};
        char json[8192], bind[1024] = "", one[256];
        static const int words[4] = {72, 74, 86, 76};
        for (int b = 0; b < 4; b++) {
            img_binding(one, sizeof one, b, words[b], 1, b == 3 ? "\"source\":\"buffer-image\"" : "");
            if (b) strcat(bind, ",");
            strcat(bind, one);
        }
        int n = make_json(json, sizeof json, d, 4, bind);
        struct s2a_source_entry_view views[3]; memset(views, 0, sizeof views);
        uint8_t identity[32]; memset(identity, 7, sizeof identity);
        g_loads = 0; memset(g_got_nent, 0, sizeof g_got_nent);
        struct entry *en = build((const uint8_t *)json, (size_t)n, views, 2, identity, 0, NULL);
        CHECK(en && !en->failed, "a fragment stage with four storage images builds");
        if (en && !en->failed) {
            CHECK(en->prog.nent[1] == 4 && g_got_nent[1] == 4, "four layout entries reach the program record and the backend");
            static const struct { uint8_t cls; uint16_t reg; uint8_t dtt, dcls, difmt; } want[4] = {
                {NVN_GPU_RES_IMAGE, 0, NVN_TEXTURE_TARGET_2D + 1, 1, 33},
                {NVN_GPU_RES_IMAGE, 1, NVN_TEXTURE_TARGET_2D + 1, 0, 0},
                {NVN_GPU_RES_IMAGE, 7, NVN_TEXTURE_TARGET_2D + 1, 2, 25},
                {NVN_GPU_RES_IMAGE_TBUF, 2, NVN_TEXTURE_TARGET_BUFFER + 1, 1, 33}};
            for (int k = 0; k < 4 && k < (int)en->prog.nent[1]; k++) {
                char what[320];
                const nvn_gpu_layout_ent *le = &en->prog.ent[1][k];
                snprintf(what, sizeof what, "image %d: cls %d reg %d target %d class %d format %d (want cls %d reg %d target %d class %d format %d)", k,
                         (int)le->cls, (int)le->src_reg, (int)en->prog.dtt[1][k], (int)en->prog.dcls[1][k], (int)en->prog.difmt[1][k],
                         (int)want[k].cls, (int)want[k].reg, (int)want[k].dtt, (int)want[k].dcls, (int)want[k].difmt);
                CHECK(le->src_kind == NVN_RB_IMG && le->cls == want[k].cls && le->src_reg == want[k].reg && en->prog.dtt[1][k] == want[k].dtt &&
                      en->prog.dcls[1][k] == want[k].dcls && en->prog.difmt[1][k] == want[k].difmt, what);
                CHECK(g_got_nent[1] > (unsigned)k && g_got_ent[1][k].src_kind == NVN_RB_IMG && g_got_ent[1][k].src_reg == want[k].reg, "the backend got the same layout entry");
            }
        }
    }

    // 5) 生产 build：表达不了的图像整份程序失败（不静默绑槽）、不留后端着色器
    static const struct { int slot, count; const char *what; } bb[] = {{73, 1, "odd word"}, {88, 1, "word above the table"}, {72, 2, "image array"}};
    for (unsigned k = 0; k < sizeof bb / sizeof bb[0]; k++) {
        static const struct img_decl d[1] = {{1, 1, 33}};
        char json[8192], bind[256];
        int n = make_json(json, sizeof json, d, 1, img_binding(bind, sizeof bind, 0, bb[k].slot, bb[k].count, ""));
        struct s2a_source_entry_view views[3]; memset(views, 0, sizeof views);
        uint8_t identity[32]; memset(identity, 9, sizeof identity);
        struct entry *en = build((const uint8_t *)json, (size_t)n, views, 2, identity, 0, NULL);
        char what[320];
        snprintf(what, sizeof what, "build refuses a program with an unmappable image (%s)", bb[k].what);
        CHECK(en && en->failed && !en->prog.sh[0] && !en->prog.sh[1], what);
    }

    if (fails) { fprintf(stderr, "graphics image layout: %d FAILED\n", fails); return 1; }
    puts("PASS: graphics image layout (real parse + production layout_ent + production build)");
    return 0;
}
