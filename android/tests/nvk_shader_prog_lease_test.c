// 程序借用（graphics/nvn/replay/rp_shader.h 的借用合同）宿主单测：链生产 android/native/gfx_plat/nvk_shader.c。
//
// 这条合同防的是复核报的 P1：管线预热从来源缓存取走裸 rp_prog* 之后继续读它，会话收尾
// （rp_shader_reset）在「程序构建在途」归零后就释放程序记录 → heap-use-after-free。
// 夹具里程序走**生产预热发布路径**真发进 g_progs（结果字节 / 结果解析 / 后端着色器装载是替身），
// 借用 / 归还 / 静默 / 收尾 / 读数全部是生产实现。
//
// 覆盖：
//   ① 借到 → 重复借用计数 → 归还 → 静默成功 → 收尾释放阶段句柄，之后同身份借不到；
//   ② 借用未还 → 静默返回 1（P1 回归点）→ 收尾有界等 200 ms 后**拒绝释放**、记录保留、读数报红；
//      消费者在收尾之后仍能读 prog->sh[]（ASan 下这一步就是 UAF 判据）→ 归还 → 再收尾通过；
//   ③ 收尾被拒后表换成空表：下一局不许按身份命中上一代设备的旧句柄；
//   ④ 还没发布 → 0；失败项 → 0（不是可用程序）；停局 / NULL 参数 / 表满 → -1 并计数；
//   ⑤ 借用表满（256 格）→ 第 257 个如实 -1，不静默降级成裸指针；读数带峰值与拒绝数；
//   ⑥ 无主归还：计数并报一次红，借用表不动；
//   ⑦ 身份粒度诊断缺省关（#1328）：玩家包 nvn_plat_flag 恒返回缺省值，缺省必须是 0，且不打「身份粒度」行。
//
// 编译与运行（cc 直编，不需要 CMake / NDK 工具链；Vulkan 头用仓库第三方目录）：
//   cc -std=c11 -O1 -g -pthread -fsanitize=address -D_GNU_SOURCE
//      -Iandroid/tests/vkhost -Igraphics/nvn/third_party/vulkan-headers/include
//      -Iruntime/include -Iandroid/native -Iandroid/native/nvn -Iandroid/native/direct
//      -Iandroid/native/gfx_plat -Igraphics/nvn -Igraphics/nvn/replay
//      -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include
//      android/tests/nvk_shader_prog_lease_test.c android/native/gfx_plat/nvk_shader.c
//      -o build/nvk-shader-prog-lease-test -lm
//   （一行写全即可；上面按参数分组折行。）
//   build/nvk-shader-prog-lease-test
// 负对照：把 nvk_shader.c 里 rp_shader_reset 的 "if (bs.borrowed)" 换成 "if (0)"（收尾照旧释放）
//   或把 nvk_shader_warm_quiesce 的 "|| g_borrowed > 0" 去掉，本文件必须变红（见 ②③）。

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nvn_backend.h"
#include "rp_shader.h"
#include "nvn_plat.h"
#include "nvn_unimpl.h"
#include "nvn/nvn_vk_prog_warm.h"
#include "nvn/nvn_vk_program_result.h"
#include "nvk_session.h"   // nvk_shader_open / nvk_shader_stop / nvk_shader_inflight（生产声明）
#include "direct/compile_request.h"
#include "direct/offline_library.h"
#include "direct/program_cache.h"

static int g_checks, g_fail;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ---- 平台替身：日志按行留档（读数断言用），开关一律取缺省 ----
enum { LOG_LINES = 256, LOG_LEN = 256 };
static char g_log[LOG_LINES][LOG_LEN];
static int g_log_n, g_red_n;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
void nvn_plat_log(int level, const char *cls, const char *text) {
    (void)cls;
    pthread_mutex_lock(&g_log_mu);
    if (level == NVN_PLAT_LOG_RED) g_red_n++;
    snprintf(g_log[g_log_n % LOG_LINES], LOG_LEN, "%s", text ? text : "");
    g_log_n++;
    pthread_mutex_unlock(&g_log_mu);
}
int nvn_plat_flag(const char *name, int dflt) { (void)name; return dflt; }
const char *nvn_plat_dir(int which) { (void)which; return NULL; }
void nvn_plat_safepoint(void) {}
uint64_t nvn_unimpl(const char *cls, const char *name, const void *site) { (void)cls; (void)name; (void)site; return 0; }

static int log_has(const char *needle) {
    int hit = 0;
    pthread_mutex_lock(&g_log_mu);
    for (int i = 0; i < g_log_n && i < LOG_LINES; i++) if (strstr(g_log[i], needle)) { hit = 1; break; }
    pthread_mutex_unlock(&g_log_mu);
    return hit;
}
static const char *log_select(const char *needle) {
    static char out[LOG_LEN];
    pthread_mutex_lock(&g_log_mu);
    out[0] = 0;
    for (int i = 0; i < g_log_n && i < LOG_LINES; i++)
        if (strstr(g_log[i], needle)) { snprintf(out, sizeof out, "%.*s", (int)(LOG_LEN - 1), g_log[i]); break; }
    pthread_mutex_unlock(&g_log_mu);
    return out;
}
static void log_reset(void) { pthread_mutex_lock(&g_log_mu); g_log_n = 0; g_red_n = 0; pthread_mutex_unlock(&g_log_mu); }
static void log_counters(int *lines, int *red) { pthread_mutex_lock(&g_log_mu); *lines = g_log_n; *red = g_red_n; pthread_mutex_unlock(&g_log_mu); }

static uint64_t mono_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull; }

// ---- 预热发布路径上的边界替身 ----
// 夹具约定：每份「程序」有 g_fake_nst 个阶段；结果字节内容不作数（解析桩按约定出阶段）。
static unsigned g_fake_nst = 2;
static int g_parse_fail;
static int g_load_calls, g_release_calls;
static int g_fake_c1;   // 1 = 假编译结果里带一条 NVN_RB_C1 绑定（stage 0 的 cbuf 槽 1）：复现「光照着色器要 c1」

int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *c) {
    memset(c, 0, sizeof *c);
    c->session = 7; c->generation = 9; c->direct_abi = 1;
    c->profile_json = "{}"; c->profile_bytes = 2;
    c->profile_hash = "sha256-0000";
    return 0;
}
static const uint8_t g_payload[] = "S2A-PROGRAM-FIXTURE";
int s2a_program_cache_load(const char *dir, const uint8_t identity[32], uint8_t **bytes, size_t *len,
                           uint8_t result_sha[32], char *why, size_t why_size) {
    (void)dir; (void)identity; (void)why; (void)why_size;
    if (!bytes || !len) return -1;
    if (result_sha) memset(result_sha, 0, 32);
    *bytes = (uint8_t *)malloc(sizeof g_payload);
    if (!*bytes) return -1;
    memcpy(*bytes, g_payload, sizeof g_payload);
    *len = sizeof g_payload;
    return 0;
}
// 结果合并（#1355）读头：这个夹具没有缓存目录，永远「没有可合并的」；结果合并本身由 nvk_shader_alias_test 覆盖。
int s2a_program_cache_probe(const char *dir, const uint8_t identity[32], uint8_t result_sha[32], uint64_t *len,
                            char *why, size_t why_size) {
    (void)dir; (void)identity; (void)why; (void)why_size;
    if (result_sha) memset(result_sha, 0, 32);
    if (len) *len = 0;
    return 1;   // 没有文件（返回值口径见 program_cache.h）
}
int s2a_offline_library_fetch(const char *revision_root, const struct s2a_offline_request *request,
                              struct s2a_offline_program *out, char *why, size_t why_size) {
    (void)revision_root; (void)request; (void)why; (void)why_size;
    memset(out, 0, sizeof *out);
    return S2A_OFFLINE_MISS;
}
int nvn_vk_program_parse_graphics_stages(const uint8_t *bytes, size_t length, struct fd_program *out,
                                         int allow_vs_only, char *why, size_t why_size) {
    (void)bytes; (void)length; (void)allow_vs_only; (void)why; (void)why_size;
    memset(out, 0, sizeof *out);
    if (g_parse_fail) return -1;
    static uint8_t spirv[4] = {3, 2, 1, 0};   // 假 SPIR-V：装载替身不看内容
    if (g_fake_c1) {   // 一条 stage 0 的 C1（cbuf 槽 1）绑定：layout_ent 会把它映射成 NVN_RB_C1
        out->binding_count = 1;
        struct fd_binding *b = &out->bindings[0];
        memset(b, 0, sizeof *b);
        b->kind = 0; b->slot = 1; b->stage_idx = 0; b->set = 0; b->binding = 0;
        snprintf(b->stage, sizeof b->stage, "vs");
    }
    for (unsigned st = 0; st < g_fake_nst && st < FD_MAX_STAGES; st++) {
        out->stage[st].present = 1;
        out->stage[st].spirv = spirv;
        out->stage[st].spirv_bytes = sizeof spirv;
        out->stage[st].fragment_output_map = 0xffffffffu;
    }
    return 0;
}
void nvn_vk_program_free(struct fd_program *program) { (void)program; }
int nvn_vk_program_parse_compute(const uint8_t *bytes, size_t length, struct nvn_vk_compute_program *out,
                                 char *why, size_t why_size) {
    (void)bytes; (void)length; (void)out; (void)why; (void)why_size;
    return -1;
}
void nvn_vk_program_free_compute(struct nvn_vk_compute_program *program) { (void)program; }
nvn_gpu_rc nvn_gpu_shader_load(const nvn_gpu_shader_src *s, nvn_gpu_shader *out, nvn_gpu_err *err) {
    (void)s; (void)err;
    static unsigned seq;
    g_load_calls++;
    *out = (nvn_gpu_shader)(uintptr_t)(0x1000u + ++seq);
    return (nvn_gpu_rc){NVN_GPU_OK};
}
void nvn_gpu_shader_release(nvn_gpu_shader s) { (void)s; g_release_calls++; }
void nvn_gpu_caps_get(nvn_gpu_caps *out) { memset(out, 0, sizeof *out); }   // 建程序时问深度裁剪能力（nvk_shader.c depth_clip_cap）：本用例只管借用，报「没有」
void nvk_prog_warm_reset(void) {}
void nvk_prog_warm_stat_build(int rc) { (void)rc; }
void nvk_prog_warm_stat_draw_hit(void) {}
void nvk_prog_warm_stat_draw_miss(void) {}
void nvk_prog_warm_note(const struct nvk_prog_rec *rec) { (void)rec; }
int nvk_prog_warm_contains(const uint8_t identity[32]) { (void)identity; return 0; }

// ---- 夹具 ----
static void ident_fill(uint8_t id[32], unsigned seed) {
    for (int i = 0; i < 32; i++) id[i] = (uint8_t)(seed * 31u + (unsigned)i * 7u + 1u);
    // 前 4 字节写完整种子：任意两个不同种子的身份一定不同（表满用例要 257 个互不相同的身份）。
    id[0] = (uint8_t)seed; id[1] = (uint8_t)(seed >> 8); id[2] = (uint8_t)(seed >> 16); id[3] = (uint8_t)(seed >> 24);
}
// 走生产预热发布路径把一份程序真发进 g_progs。返回 nvk_prog_warm_build 的返回值（0 = 已发布）。
static int publish(unsigned seed, unsigned nst) {
    struct nvk_prog_rec rec;
    memset(&rec, 0, sizeof rec);
    ident_fill(rec.identity, seed);
    rec.stage_mask = nst >= 3 ? 7u : (nst == 2 ? 3u : 1u);
    g_fake_nst = nst;
    return nvk_prog_warm_build(&rec);
}

// ⑦ 预热守卫：清单记录缺 c1、但程序带 C1 绑定 → 必须拒绝发布（否则 rp_draw 会把这类绘制全跳过：火把光消失）
static int publish_c1(unsigned seed, unsigned nst, int c1_ok) {
    struct nvk_prog_rec rec;
    memset(&rec, 0, sizeof rec);
    ident_fill(rec.identity, seed);
    rec.stage_mask = nst >= 3 ? 7u : (nst == 2 ? 3u : 1u);
    rec.c1_ok = (uint8_t)c1_ok;
    if (c1_ok) for (int st = 0; st < 3; st++) if (c1_ok & (1 << st)) memset(rec.c1[st], (uint8_t)(seed + st), NVK_PROG_REC_C1_BYTES);
    g_fake_nst = nst;
    return nvk_prog_warm_build(&rec);
}
static void case_c1_guard(void) {
    rp_shader_reset();
    g_load_calls = g_release_calls = 0;
    g_fake_c1 = 1;
    // (a) 清单缺 c1：必须拒绝（返回 -1），且这个身份不许出现在 g_progs 里
    CHECK(publish_c1(10, 2, 0) == -1, "清单缺 c1 且程序带 C1 绑定：预热必须拒绝发布（返回 -1）");
    const struct rp_prog *p = NULL;
    uint8_t id[32]; ident_fill(id, 10);
    CHECK(rp_shader_prog_borrow(id, &p) == 0 && p == NULL, "被拒绝的程序不得发布（借不到）");
    // (b) 清单带 c1：正常发布，且程序对象拿到 c1
    CHECK(publish_c1(11, 2, 3) == 0, "清单带 c1：预热应发布成功");
    ident_fill(id, 11);
    CHECK(rp_shader_prog_borrow(id, &p) == 1 && p != NULL, "带 c1 的程序已发布");
    CHECK((p->c1_ok & 3u) == 3u && p->c1[0][0] == 11 && p->c1[1][0] == 12, "程序对象带回 c1（c1_ok=%#x）", (unsigned)p->c1_ok);
    rp_shader_prog_return(id);
    // (c) 程序不带 C1 绑定、清单也没 c1：这是合法情况，必须照常发布（别把正常程序也拒了）
    g_fake_c1 = 0;
    CHECK(publish_c1(12, 2, 0) == 0, "程序不带 C1 绑定：清单无 c1 也应正常发布");
    g_fake_c1 = 0;
}

// ① 借到 → 归还 → 收尾释放
static void case_borrow_return_reset(void) {
    rp_shader_reset();
    log_reset();
    g_load_calls = g_release_calls = 0;
    CHECK(publish(1, 2) == 0, "夹具程序应已发布");
    uint8_t id[32]; ident_fill(id, 1);
    const struct rp_prog *p = NULL, *p2 = NULL;
    CHECK(rp_shader_prog_borrow(id, &p) == 1 && p != NULL, "已发布身份应借到");
    CHECK(p != NULL && p->sh[0] != NULL && p->sh[1] != NULL && p->sh[2] == NULL, "借到的程序带两个阶段句柄");
    CHECK(rp_shader_prog_borrowed() == 1, "借到后借用数 = 1（实测 %d）", rp_shader_prog_borrowed());
    CHECK(rp_shader_prog_borrow(id, &p2) == 1 && p2 == p, "同身份可重复借用、指针相同");
    CHECK(rp_shader_prog_borrowed() == 2, "重复借用后借用数 = 2（实测 %d）", rp_shader_prog_borrowed());
    rp_shader_prog_return(id);
    CHECK(rp_shader_prog_borrowed() == 1, "归还一次后借用数 = 1（实测 %d）", rp_shader_prog_borrowed());
    rp_shader_prog_return(id);
    CHECK(rp_shader_prog_borrowed() == 0, "归还完借用数 = 0（实测 %d）", rp_shader_prog_borrowed());
    CHECK(nvk_shader_warm_quiesce(50) == 0, "借用归零后静默成功");
    int rel0 = g_release_calls;
    rp_shader_reset();   // 收尾：释放该程序的两份阶段句柄
    CHECK(g_release_calls - rel0 == 2, "收尾释放阶段句柄（期望 2，实测 %d）", g_release_calls - rel0);
    CHECK(rp_shader_prog_borrowed() == 0, "收尾后借用数仍为 0");
    CHECK(log_has("借用 0（峰值 2）"), "收尾读数记下本局借用峰值 2（实测：%s）", log_select("程序借用读数"));
    p = NULL;
    CHECK(rp_shader_prog_borrow(id, &p) == 0 && p == NULL, "收尾后同身份借不到（记录已释放）");
}

// ② 借用未还 → 静默 / 收尾都拒绝 → 消费者仍可读 → 归还 → 再收尾通过
struct consumer_ctx {
    const uint8_t *identity;
    _Atomic int rc, ready, go, sh0, sh1, sh2;
};
static void *consumer_main(void *arg) {
    struct consumer_ctx *c = (struct consumer_ctx *)arg;
    const struct rp_prog *p = NULL;
    int rc = rp_shader_prog_borrow(c->identity, &p);
    atomic_store(&c->rc, rc);
    atomic_store(&c->ready, 1);
    while (!atomic_load(&c->go)) usleep(1000);
    if (p) {   // 收尾被拒之后仍然合法：借用期间对象不得释放（ASan 在这里判 UAF）
        atomic_store(&c->sh0, p->sh[0] != NULL);
        atomic_store(&c->sh1, p->sh[1] != NULL);
        atomic_store(&c->sh2, p->sh[2] != NULL);
    }
    if (rc == 1) rp_shader_prog_return(c->identity);
    return NULL;
}
static void case_reset_refused_while_borrowed(void) {
    rp_shader_reset();
    log_reset();
    g_load_calls = g_release_calls = 0;
    CHECK(publish(2, 2) == 0, "夹具程序应已发布");
    uint8_t id[32]; ident_fill(id, 2);
    struct consumer_ctx c;
    memset(&c, 0, sizeof c);
    c.identity = id;
    pthread_t th;
    CHECK(pthread_create(&th, NULL, consumer_main, &c) == 0, "消费者线程起得来");
    while (!atomic_load(&c.ready)) usleep(500);
    CHECK(atomic_load(&c.rc) == 1, "消费者借到程序");
    // 静默判据必须含借用：借用还在时不能报「已静」——这就是 P1 的回归点。
    uint64_t t0 = mono_ms();
    int q = nvk_shader_warm_quiesce(80);
    uint64_t waited = mono_ms() - t0;
    CHECK(q == 1, "借用未还时静默必须返回 1（实测 %d）", q);
    CHECK(waited >= 70 && waited <= 3000, "静默等待有界（期望约 80ms，实测 %llu ms）", (unsigned long long)waited);
    // 收尾：有界等借用 200 ms，没归零就不释放。
    int rel0 = g_release_calls;
    t0 = mono_ms();
    rp_shader_reset();
    waited = mono_ms() - t0;
    CHECK(waited >= 180 && waited <= 3000, "收尾借用等待有界（期望 ≥180ms，实测 %llu ms）", (unsigned long long)waited);
    CHECK(g_release_calls - rel0 == 0, "借用未还时收尾不释放程序记录（实测释放 %d）", g_release_calls - rel0);
    CHECK(rp_shader_prog_borrowed() == 1, "收尾被拒后借用数仍为 1（实测 %d）", rp_shader_prog_borrowed());
    CHECK(log_has("借用 1（峰值 1）"), "收尾读数报红并记下「借用 1（峰值 1）」（实测：%s）", log_select("程序借用读数"));
    CHECK(log_has("收尾保留"), "收尾读数记下保留的程序记录（泄漏到进程结束）");
    { int lines = 0, red = 0; log_counters(&lines, &red); CHECK(red >= 1, "收尾拒绝必须有红行（实测红行 %d）", red); }
    // 被拒的收尾把表换成空表：下一局不许按身份命中上一代设备留下的旧句柄。
    const struct rp_prog *old = (const struct rp_prog *)(uintptr_t)1;
    CHECK(rp_shader_prog_by_identity(id, &old) == 0 && old == NULL, "拒绝收尾后旧身份不再可查（下一局不命中旧句柄）");
    // 消费者继续读：生产实现若在借用未还时释放了记录，这里就是 heap-use-after-free。
    atomic_store(&c.go, 1);
    pthread_join(th, NULL);
    CHECK(atomic_load(&c.sh0) == 1 && atomic_load(&c.sh1) == 1 && atomic_load(&c.sh2) == 0,
          "收尾之后借用期间读到的阶段句柄不变（读 sh=%d/%d/%d）",
          atomic_load(&c.sh0), atomic_load(&c.sh1), atomic_load(&c.sh2));
    CHECK(rp_shader_prog_borrowed() == 0, "归还后借用数 = 0");
    // 第二次收尾：借用已归零 → 正常通过；旧记录已是进程级泄漏，不再释放。
    rel0 = g_release_calls;
    rp_shader_reset();
    CHECK(g_release_calls - rel0 == 0, "第二次收尾不重复释放（实测 %d）", g_release_calls - rel0);
    CHECK(rp_shader_prog_borrowed() == 0, "第二次收尾后借用数 = 0");
    CHECK(log_has("收尾保留 1 份"), "读数记住「收尾保留 1 份」（实测：%s）", log_select("程序借用读数"));
}

// ③ 静默判据：借用未还 → 1，归还 → 0；发布但没借用的程序不挡静默
static void case_quiesce_counts_borrow(void) {
    rp_shader_reset();
    log_reset();
    CHECK(publish(4, 2) == 0, "夹具程序应已发布");
    CHECK(nvk_shader_warm_quiesce(20) == 0, "没有借用时静默成功（发布过但不影响）");
    uint8_t id[32]; ident_fill(id, 4);
    const struct rp_prog *p = NULL;
    CHECK(rp_shader_prog_borrow(id, &p) == 1 && p != NULL, "借到");
    CHECK(nvk_shader_warm_quiesce(20) == 1, "借用未还时静默返回 1");
    CHECK(rp_shader_prog_borrowed() == 1, "静默失败不改变借用数");
    rp_shader_prog_return(id);
    CHECK(nvk_shader_warm_quiesce(20) == 0, "归还后静默返回 0");
    rp_shader_reset();
}

// ④ 边界：没发布 / 失败项 / 停局 / NULL 参数 / 无主归还
static void case_edges(void) {
    rp_shader_reset();
    log_reset();
    uint8_t unknown[32]; ident_fill(unknown, 999);
    const struct rp_prog *p = (const struct rp_prog *)(uintptr_t)1;
    CHECK(rp_shader_prog_borrow(unknown, &p) == 0 && p == NULL, "没发布过的身份返回 0（不是错误）");
    CHECK(rp_shader_prog_borrow(NULL, &p) == -1, "identity 为 NULL 返回 -1");
    CHECK(rp_shader_prog_borrow(unknown, NULL) == -1, "out 为 NULL 返回 -1");
    // 失败项不是可用程序：口径同 rp_shader_prog_by_identity（借不到）
    uint8_t failed_id[32]; ident_fill(failed_id, 77);
    struct nvk_prog_rec rec;
    memset(&rec, 0, sizeof rec);
    memcpy(rec.identity, failed_id, 32);
    rec.stage_mask = 3;
    g_fake_nst = 2; g_parse_fail = 1;
    CHECK(nvk_prog_warm_build(&rec) == -1, "解析失败的预热项发布为失败项");
    g_parse_fail = 0;
    p = (const struct rp_prog *)(uintptr_t)1;
    CHECK(rp_shader_prog_borrow(failed_id, &p) == 0 && p == NULL, "失败项借不到（返回 0）");
    // 停局：拒绝借用并计数
    CHECK(publish(3, 2) == 0, "夹具程序应已发布");
    uint8_t id[32]; ident_fill(id, 3);
    nvk_shader_stop();
    p = (const struct rp_prog *)(uintptr_t)1;
    CHECK(rp_shader_prog_borrow(id, &p) == -1 && p == NULL, "停局后借不到（-1）");
    CHECK(rp_shader_prog_borrowed() == 0, "停局拒绝不算借用");
    CHECK(nvk_shader_open() == 0, "重新开会话");
    // 无主归还：计数 + 报一次红，表不动
    rp_shader_prog_return(unknown);
    CHECK(rp_shader_prog_borrowed() == 0, "无主归还不动借用数");
    rp_shader_reset();
    CHECK(log_has("拒绝 3"), "读数记下 3 次拒绝（实测：%s）", log_select("程序借用读数"));
    CHECK(log_has("无主归还 1"), "读数记下 1 次无主归还（实测：%s）", log_select("程序借用读数"));
}

// ⑤ 借用表满：第 257 个如实失败并计数，不静默降级成裸指针
static void case_borrow_table_full(void) {
    enum { N = 257 };
    static uint8_t ids[N][32];
    rp_shader_reset();
    log_reset();
    for (unsigned i = 0; i < N; i++) {
        struct nvk_prog_rec rec;
        memset(&rec, 0, sizeof rec);
        ident_fill(ids[i], 1000 + i);
        memcpy(rec.identity, ids[i], 32);
        rec.stage_mask = 1;
        g_fake_nst = 1;
        CHECK(nvk_prog_warm_build(&rec) == 0, "第 %u 份夹具程序应已发布", i);
    }
    for (unsigned i = 0; i < 256; i++) {
        const struct rp_prog *p = NULL;
        CHECK(rp_shader_prog_borrow(ids[i], &p) == 1 && p != NULL, "第 %u 次借用应成功", i);
    }
    CHECK(rp_shader_prog_borrowed() == 256, "借用表满时借用数 = 256（实测 %d）", rp_shader_prog_borrowed());
    const struct rp_prog *p = (const struct rp_prog *)(uintptr_t)1;
    CHECK(rp_shader_prog_borrow(ids[256], &p) == -1 && p == NULL, "表满后第 257 个借用如实失败（-1）");
    CHECK(rp_shader_prog_borrowed() == 256, "表满拒绝不改变借用数（实测 %d）", rp_shader_prog_borrowed());
    for (unsigned i = 0; i < 256; i++) rp_shader_prog_return(ids[i]);
    CHECK(rp_shader_prog_borrowed() == 0, "全部归还后借用数 = 0");
    CHECK(nvk_shader_warm_quiesce(50) == 0, "借用归零后静默成功");
    rp_shader_reset();
    CHECK(log_has("拒绝 1"), "读数记下 1 次拒绝（表满）（实测：%s）", log_select("程序借用读数"));
    CHECK(log_has("（峰值 256）"), "读数记下借用峰值 256（实测：%s）", log_select("程序借用读数"));
}

// ⑦ 身份粒度诊断缺省关（#1328）：玩家包里 nvn_plat_flag 恒返回缺省值，缺省必须是 0——
//    缺省 1 就是玩家包每笔绘制多算一遍 input_sig（分组键）、慢路径多记账、每 5 秒多打一行「身份粒度」。
//    本文件的 nvn_plat_flag 替身同样「开关一律取缺省」，这里读到的就是玩家包的状态。
//    负对照：把 nvk_shader.c 里 idg_on 的缺省 0 改回 1，下面三条都必须变红。
int nvk_shader_ident_diag_test(int force_log);   // nvk_shader.c 的用例钩子
static void case_ident_diag_default(void) {
    log_reset();
    CHECK(nvk_shader_ident_diag_test(0) == 0, "身份粒度诊断缺省应关着（玩家包 nvn_plat_flag 恒返回缺省值）");
    CHECK(nvk_shader_ident_diag_test(1) == 0, "把「上次打日志」拨到很早再调一次，诊断仍应关着");
    CHECK(!log_has("身份粒度"), "缺省不应出现「身份粒度」日志行（实测：%s）", log_select("身份粒度"));
}

int main(void) {
    printf("nvk_shader 程序借用宿主单测（链生产 nvk_shader.c；ASan 下读收尾后的程序记录）\n");
    // 预热发布路径只在有跨局缓存目录时去取结果字节；离线库走 S2A_OFFLINE_MISS 替身。
    setenv("S2A_DIRECT_CACHE_DIR", "lease-fixture-not-used", 1);
    unsetenv("S2A_SHADER_REVISION_ROOT");
    CHECK(nvk_shader_open() == 0, "会话可开");
    case_c1_guard();
    case_borrow_return_reset();
    case_reset_refused_while_borrowed();
    case_quiesce_counts_borrow();
    case_edges();
    case_borrow_table_full();
    case_ident_diag_default();
    printf("%s：%d 项断言，%d 失败；阶段装载 %d 次、释放 %d 次\n",
           g_fail ? "FAIL" : "PASS", g_checks, g_fail, g_load_calls, g_release_calls);
    return g_fail ? 1 : 0;
}
