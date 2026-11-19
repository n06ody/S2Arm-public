// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 附件反馈快照在活动的遮挡查询里拆通道（#1658）：查询在新通道里换续段槽接着数，样本数连续；续段槽按块给；
// 一个查询要不到续段槽（保险丝）或有一段没读到时，只这一个查询按「未完成」读 0，不留部分和，别的查询和整段不受影响。
// 真实 Vulkan 驱动（lavapipe）+ validation 层。每笔反馈绘制是 4×4 的全屏三角形 = 16 个样本，样本数用整数断言。
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "nvn_unimpl.h"
#include "vk_device.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint32_t nvn_gpu_vk_test_occl_chunk(void);                // backend_vulkan.c：一块续段槽的槽数
uint32_t nvn_gpu_vk_test_occl_query_max(uint32_t n);      // 一个声明槽名下最多几个续段槽（0 = 缺省）；返回改之前的值
uint32_t nvn_gpu_vk_test_occl_skip_begin(uint32_t n);     // 每段第 N 个续段槽（从 1 数）不开，让它读回时没回来（0 = 不干预）；返回改之前的值
uint32_t nvn_gpu_vk_test_occl_declared_miss(uint32_t on); // 声明槽那一段读回时当作没回来（0 = 不干预；提交前开、完成回调后关）；返回改之前的值

static nvn_gpu_err err;
static int dq_mode, opt_mode;
static unsigned failures;
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "fixture failed line %d: %s rc=%d why=%s\n", __LINE__, #x, err.rc.code, err.why); exit(2); } } while (0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))
#define EXPECT(cond, ...) do { if (!(cond)) { failures++; printf("  EXPECT 失败 line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

const char *const *nvn_plat_vk_instance_exts(uint32_t *n) { *n = 0; return NULL; }
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) { (void)i; (void)w; *s = VK_NULL_HANDLE; return VK_ERROR_EXTENSION_NOT_PRESENT; }
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) { (void)w; (void)x; (void)y; return 0; }
void nvn_plat_safepoint(void) {}
int nvn_plat_flag(const char *n, int d) { if (!strcmp(n, "nvk_dq")) return dq_mode; if (!strcmp(n, "nvk_opt_layout")) return opt_mode; return d; }
void nvn_plat_log(int l, const char *c, const char *s) { (void)l; fprintf(stderr, "[%s] %s\n", c ? c : "", s ? s : ""); }

// 完成回调里把读数拷出来（d->occlusion 只在回调期间有效）
static struct { _Atomic int n; uint32_t status, nocc; uint64_t occ[8]; char why[200]; } g_done;
static void done(void *u, const nvn_gpu_done *d) {
    (void)u;
    g_done.status = d->status;
    g_done.nocc = d->nocclusion;
    memset(g_done.occ, 0, sizeof g_done.occ);
    if (d->occlusion) memcpy(g_done.occ, d->occlusion, (d->nocclusion < 8 ? d->nocclusion : 8) * sizeof g_done.occ[0]);
    snprintf(g_done.why, sizeof g_done.why, "%s", d->why ? d->why : "");
    atomic_store(&g_done.n, 1);
}

static nvn_gpu_shader shader(const char *dir, const char *name, nvn_gpu_stage stage, const nvn_gpu_layout *layout) {
    char path[1024];
    REQUIRE(snprintf(path, sizeof path, "%s/%s.spv", dir, name) < (int)sizeof path);
    FILE *f = fopen(path, "rb"); REQUIRE(f);
    REQUIRE(!fseek(f, 0, SEEK_END));
    long n = ftell(f); REQUIRE(n > 0);
    rewind(f);
    void *data = malloc((size_t)n); REQUIRE(data);
    REQUIRE(fread(data, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    nvn_gpu_shader_src s = {0};
    s.stage = stage; s.kind = NVN_GPU_BLOB_SPIRV; s.entry = "main"; s.blob = data; s.bytes = (size_t)n; s.layout = layout;
    nvn_gpu_shader h = NULL;
    GPU(nvn_gpu_shader_load(&s, &h, &err));
    free(data);
    return h;
}

// 一个用例：declared 个声明槽；先在没有活动查询时画 plain 笔反馈绘制，再依次用 nslot 个槽，第 s 个槽里画 draws[s] 笔反馈绘制。
// 每一笔都采样自己正在写的附件，所以每一笔前后端都要拆一次通道（反馈快照）。
// query_max：这个用例里一个声明槽名下最多几个续段槽（0 = 缺省）；skip_begin：每段第 N 个续段槽不开（0 = 不干预）；declared_miss：声明槽那一段读回时当作没回来。
// zero_slot：期望读 0 的槽（续不下去 / 有一段没读到的那个），-1 = 没有；want_lost / want_miss：nvn_unimpl 两项计数这个用例里应增加几次。
struct occ_case { const char *name; unsigned declared, plain, nslot, draws[4], query_max, skip_begin; int zero_slot; unsigned want_lost, want_miss, declared_miss; };

static unsigned expected_red(unsigned draws) {   // 每笔：读快照 + 0.25 红，UNORM8 往返；清屏色 0.25 = 64
    unsigned v = 64;
    for (unsigned i = 0; i < draws; i++) { float f = (float)v / 255.0f + 0.25f; if (f > 1.0f) f = 1.0f; v = (unsigned)(f * 255.0f + 0.5f); }
    return v;
}

static void run_case(struct s2a_vk_device *device, const char *dir, const struct occ_case *k) {
    nvn_gpu_open_desc od = {0};
    od.host_device = device; od.app_label = "feedback-occlusion";
    GPU(nvn_gpu_open(&od, &err));
    nvn_gpu_layout_ent ent[2] = {{0}};
    ent[0].cls = NVN_GPU_RES_TEX; ent[1].cls = NVN_GPU_RES_SAMP; ent[1].binding = 1;
    for (unsigned i = 0; i < 2; i++) { ent[i].access = NVN_GPU_ACC_R; ent[i].count = 1; ent[i].stages = 1u << NVN_GPU_SH_FS; }
    nvn_gpu_layout layout = {0};
    layout.ent = ent; layout.nent = 2;
    nvn_gpu_shader vs = shader(dir, "fullscreen.vert", NVN_GPU_SH_VS, NULL), fs = shader(dir, "feedback.frag", NVN_GPU_SH_FS, &layout);
    nvn_gpu_pipe_desc pd = {0};
    pd.poly.mode = NVN_POLYGON_MODE_FILL; pd.color.logic_op = 3; pd.prim = 4; pd.samples = 1; pd.chmask.mask = 15;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs; pd.cfmt[0] = NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));
    nvn_gpu_tex images[2] = {0}; nvn_gpu_view views[2] = {0}, sample = NULL;
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = 4; td.d.depth = 1; td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    view_t av = {0};
    av.set_layers = 1; av.nlayers = 1; av.set_target = 1; av.target = NVN_TEXTURE_TARGET_2D;
    for (unsigned i = 0; i < 2; i++) { GPU(nvn_gpu_tex_create(&td, &images[i], &err)); GPU(nvn_gpu_view_create(images[i], &av, &views[i], &err)); }
    view_t v = {0};
    v.set_levels = 1; v.nlevels = 1; v.base_level = 0;
    GPU(nvn_gpu_view_create(images[0], &v, &sample, &err));   // 采样的就是正在写的那张：每笔都是反馈
    samp_par sp = {0};
    nvn_gpu_samp sampler = NULL;
    GPU(nvn_gpu_samp_create(&sp, &sampler, &err));
    unsigned char sentinel[80];
    memset(sentinel, 0xa5, sizeof sentinel);
    nvn_gpu_buf rb = NULL;
    GPU(nvn_gpu_buf_create(sizeof sentinel, NVN_GPU_MEM_SHARED, sentinel, &rb, &err));

    nvn_gpu_cmd_desc cd = {0};
    cd.occlusion_slots = k->declared; cd.label = k->name;
    nvn_gpu_cmd cmd = NULL;
    (void)nvn_gpu_vk_test_occl_query_max(k->query_max);   // 录这一段的时候生效；提交完立刻恢复缺省
    (void)nvn_gpu_vk_test_occl_skip_begin(k->skip_begin);
    (void)nvn_gpu_vk_test_occl_declared_miss(k->declared_miss);   // 读回在完成回调之前，回调之后才恢复
    const uint64_t lost0 = nvn_unimpl_count("gpu", "occlusion_continuation_lost"), miss0 = nvn_unimpl_count("gpu", "occlusion_continuation_part_missing");
    GPU(nvn_gpu_cmd_begin(&cd, &cmd, &err));
    for (unsigned i = 0; i < 2; i++) {   // 两张都清成 0.25 红（通道不声明遮挡）
        nvn_gpu_pass_desc p = {0};
        p.ncolor = 1; p.color[0].view = views[i]; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
        p.color[0].clear.f[0] = .25f; p.color[0].clear.f[3] = 1; p.w = p.h = 4; p.layers = 1;
        nvn_gpu_pass_begin(cmd, &p); nvn_gpu_pass_end(cmd);
    }
    nvn_gpu_pass_desc p = {0};   // 主通道声明遮挡计数
    p.ncolor = 1; p.color[0].view = views[0]; p.color[0].load = NVN_GPU_LOAD_LOAD; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = p.h = 4; p.layers = 1; p.occlusion = 1;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_dyn dyn = {0};
    dyn.nvp = 1; dyn.vp[0][2] = dyn.vp[0][3] = 4; dyn.vp[0][5] = 1; dyn.has_sc = 1; dyn.sc[2] = dyn.sc[3] = 4;
    nvn_gpu_set_dyn(cmd, &dyn);
    nvn_gpu_binding b[2] = {{0}};
    b[0].cls = NVN_GPU_RES_TEX; b[0].u.view = sample; b[1].ent = 1; b[1].cls = NVN_GPU_RES_SAMP; b[1].u.samp = sampler;
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, b, 2);
    nvn_gpu_draw_args a = {0};
    a.prim = 4; a.count = 3; a.instances = 1;
    unsigned total = k->plain;
    for (unsigned i = 0; i < k->plain; i++) nvn_gpu_draw(cmd, &a);
    for (unsigned s = 0; s < k->nslot; s++) {
        nvn_gpu_occlusion(cmd, (int32_t)s);
        for (unsigned i = 0; i < k->draws[s]; i++) nvn_gpu_draw(cmd, &a);
        nvn_gpu_occlusion(cmd, -1);
        total += k->draws[s];
    }
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, images[0], 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_samp_release(sampler); nvn_gpu_view_release(sample);   // 提交前放掉所有来源的所有权：命令引用与私有快照必须活到完成
    for (unsigned i = 0; i < 2; i++) { nvn_gpu_view_release(views[i]); nvn_gpu_tex_release(images[i]); }

    atomic_store(&g_done.n, 0);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err);
    (void)nvn_gpu_vk_test_occl_query_max(0);
    (void)nvn_gpu_vk_test_occl_skip_begin(0);
    unsigned bad = 0;
    // 整段必须成功：续不下去 / 没读到的只是那一个查询按未完成处理，不是整段失败
    GPU(r);
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    for (int i = 0; i < 5000 && !atomic_load(&g_done.n); i++) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
    (void)nvn_gpu_vk_test_occl_declared_miss(0);
    if (!atomic_load(&g_done.n) || g_done.status != NVN_GPU_DONE_OK) { bad++; EXPECT(0, "回调应为 OK，实为 n=%d status=%u why=%s", atomic_load(&g_done.n), g_done.status, g_done.why); }
    if (nvn_gpu_fence_failed() != 0) { bad++; EXPECT(0, "fence_failed 应为 0，实为 %llu", (unsigned long long)nvn_gpu_fence_failed()); }
    char got[96] = "";
    if (k->nslot) {
        if (g_done.nocc != k->declared) { bad++; EXPECT(0, "读数槽数应为声明的 %u，实为 %u", k->declared, g_done.nocc); }
        for (unsigned s = 0; s < k->nslot && s < 8; s++) {
            const uint64_t want = (int)s == k->zero_slot ? 0 : 16ull * k->draws[s];
            size_t o = strlen(got); snprintf(got + o, sizeof got - o, "%s%llu→%llu", s ? " " : "", (unsigned long long)want, (unsigned long long)g_done.occ[s]);
            if (g_done.occ[s] != want) { bad++; EXPECT(0, "槽 %u 样本数应为 %llu，实为 %llu", s, (unsigned long long)want, (unsigned long long)g_done.occ[s]); }
        }
        for (unsigned s = k->nslot; s < k->declared && s < 8; s++) if (g_done.occ[s]) { bad++; EXPECT(0, "没用过的槽 %u 应为 0，实为 %llu", s, (unsigned long long)g_done.occ[s]); }
    }
    const unsigned lost = (unsigned)(nvn_unimpl_count("gpu", "occlusion_continuation_lost") - lost0), miss = (unsigned)(nvn_unimpl_count("gpu", "occlusion_continuation_part_missing") - miss0);
    if (lost != k->want_lost) { bad++; EXPECT(0, "「续不下去」计数应增加 %u，实为 %u", k->want_lost, lost); }
    if (miss != k->want_miss) { bad++; EXPECT(0, "「有一段没读到」计数应增加 %u，实为 %u", k->want_miss, miss); }
    const unsigned char *bytes = nvn_gpu_buf_cpu(rb);
    const unsigned red = expected_red(total);
    unsigned pixels_bad = 0;
    for (unsigned i = 0; i < 16; i++) if (bytes[i * 4] != red || bytes[i * 4 + 1] != 0 || bytes[i * 4 + 2] != 0 || bytes[i * 4 + 3] != 255) pixels_bad++;
    for (unsigned i = 64; i < 80; i++) if (bytes[i] != 0xa5) pixels_bad++;
    if (pixels_bad) { bad++; EXPECT(0, "像素不符 %u 处（期望红 %u）", pixels_bad, red); }
    printf("feedback-occlusion case=%s dq=%d opt=%d draws=%u samples(期望→真结果)=%s lost=%u miss=%u red=%u pixel-bad=%u bad=%u\n", k->name, dq_mode, opt_mode, total, got[0] ? got : "-", lost, miss, red, pixels_bad, bad);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    GPU(nvn_gpu_close(5000000000ull, &err));
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    dq_mode = atoi(argv[2]); opt_mode = atoi(argv[3]);
    REQUIRE((dq_mode == 0 || dq_mode == 1) && (opt_mode == 0 || opt_mode == 1));
    struct s2a_vk_device device;
    char why[256];
    if (s2a_vk_device_create(&device, why, sizeof why)) { fprintf(stderr, "%s\n", why); return 2; }
    const unsigned K = nvn_gpu_vk_test_occl_chunk();   // 一块续段槽的槽数；下面的笔数都按它算，不把常量抄进用例
    REQUIRE(K >= 4 && K <= 4096);
    // 每笔反馈绘制 16 个样本；一笔一次拆通道 = 一个续段槽
    const struct occ_case cases[] = {
        {"一个槽三笔（之前先有一笔查询外的反馈）", 1, 1, 1, {3}, 0, 0, -1, 0, 0, 0},
        {"三个槽（2 / 1 / 3 笔，声明 4 槽）", 4, 0, 3, {2, 1, 3}, 0, 0, -1, 0, 0, 0},
        {"查询外的反馈不占续段槽", 1, 2, 0, {0}, 0, 0, -1, 0, 0, 0},
        {"刚好一块（K 笔）", 1, 0, 1, {K}, 0, 0, -1, 0, 0, 0},
        {"跨块（K + 1 笔：第二块只用 1 个）", 1, 0, 1, {K + 1}, 0, 0, -1, 0, 0, 0},
        {"三个槽的续段跨块边界（归属对得上）", 3, 0, 3, {K / 2 + 10, K / 2 + 10, K / 2}, 0, 0, -1, 0, 0, 0},
        {"保险丝刚好用满（一个查询最多 300 个续段槽，画 300 笔）", 1, 0, 1, {300}, 300, 0, -1, 0, 0, 0},
        {"保险丝多一笔：只这个查询读 0，下一个查询照常，整段不失败", 2, 0, 2, {301, 3}, 300, 0, 0, 1, 0, 0},
        {"一个续段没回来：这个查询读 0 不留部分和，下一个查询照常", 2, 0, 2, {5, 2}, 0, 3, 0, 0, 1, 0},
        {"声明槽那一段没回来：这个查询读 0，名下的续段不再并进来", 1, 0, 1, {3}, 0, 0, 0, 0, 1, 1},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) run_case(&device, argv[1], &cases[i]);
    REQUIRE(!s2a_vk_device_destroy(&device, 1));
    printf("feedback-occlusion cases=%u failures=%u chunk=%u\n", (unsigned)(sizeof cases / sizeof cases[0]), failures, K);
    return failures ? 1 : 0;
}
