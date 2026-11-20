// 结果合并（#1355）宿主单测：链生产 android/native/gfx_plat/nvk_shader.c 与**真** android/native/direct/program_cache.c
// （真缓存文件、真文件头、真结果摘要），只把编译服务 / 解析 / 后端着色器装载换成替身。
//
// 合同：同一份编译结果（缓存文件头里的结果摘要相同、负载长度相同）的多个输入身份，共用一份程序记录
// （一份阶段句柄，管线缓存按句柄比，所以管线也合并）；合并只读 88 字节头，不读负载、不建后端着色器。
//
// 覆盖（argv[1]：缺省 = 显式打开；"default" = 不设开关、取生产代码里的缺省（须为打开）；"off" = 显式关闭，即 debug.s2a.nvk_alias=0 的退路）：
//   ① 打开：结果相同的第二个身份不再装载（装载次数不变），借到同一个程序记录，记录里的身份是先发布的那个；
//      结果不同的身份照常装载；已发布的身份不重复读头；收尾每份记录只释放一次（别名不重复释放）。
//   ② 打开：收尾之后结果表也清空——下一局相同结果的身份必须重新装载，不许合并到上一局已释放的记录（ASan 判 UAF）。
//   ③ 打开：头里的摘要与常驻结果相同、但负载长度不同 → 不合并（头没有校验字段，多比一个长度）；正式读取也拒绝它。
//   ④ 打开：预热已静默 → 结果合并也不得登记（与预热发布同一条静默约束）。
//   ⑤ 打开：失败项不参与合并——同结果的另一个身份照常建出。
//   ⑥ 打开：别名身份被借用时收尾拒绝释放；读数的「保留」按记录数而不是按身份键数。
//   ⑦ 关闭（退路）：同样的输入逐身份各装载一份、各自释放，与合并之前一致。
//   ⑧ 打开：比对键里「由身份推出来、不是由负载读出来」的两项——阶段数、每阶段 c1（及其有无 / 所在阶段）——
//      逐项「只改这一项 → 不合并；改回去 → 合并」。结果摘要、负载长度两项见 ① ③。
//
// 编译与运行见 android/tests/nvk_shader_alias_test.py（含负对照：撤掉各处防线必须红）。

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nvn_backend.h"
#include "rp_shader.h"
#include "nvn_plat.h"
#include "nvn_unimpl.h"
#include "nvn/nvn_vk_prog_warm.h"
#include "nvn/nvn_vk_program_result.h"
#include "nvk_session.h"
#include "direct/compile_request.h"
#include "direct/offline_library.h"
#include "direct/program_cache.h"

static int g_checks, g_fail;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ---- 平台替身：日志按行留档（读数断言用）；开关只有 nvk_alias 由命令行定，其余取缺省 ----
enum { LOG_LINES = 256, LOG_LEN = 256 };
static char g_log[LOG_LINES][LOG_LEN];
static int g_log_n;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
void nvn_plat_log(int level, const char *cls, const char *text) {
    (void)level; (void)cls;
    pthread_mutex_lock(&g_log_mu);
    snprintf(g_log[g_log_n % LOG_LINES], LOG_LEN, "%s", text ? text : "");
    g_log_n++;
    pthread_mutex_unlock(&g_log_mu);
}
static int g_alias_flag = -1;   // -1 = 不设（取生产代码里的缺省）；0 / 1 = 调试属性显式给定
int nvn_plat_flag(const char *name, int dflt) { return (name && !strcmp(name, "nvk_alias") && g_alias_flag >= 0) ? g_alias_flag : dflt; }
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
static void log_reset(void) { pthread_mutex_lock(&g_log_mu); g_log_n = 0; pthread_mutex_unlock(&g_log_mu); }

// ---- 预热发布路径上的边界替身（结果字节来自**真**缓存文件；解析 / 后端装载是替身）----
static unsigned g_fake_nst = 2;
static int g_parse_fail;
static int g_load_calls, g_release_calls;

int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *c) {
    memset(c, 0, sizeof *c);
    c->session = 7; c->generation = 9; c->direct_abi = 1;
    c->profile_json = "{}"; c->profile_bytes = 2;
    c->profile_hash = "sha256-0000";
    return 0;
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
void nvn_gpu_caps_get(nvn_gpu_caps *out) { memset(out, 0, sizeof *out); }
void nvk_prog_warm_reset(void) {}
void nvk_prog_warm_stat_build(int rc) { (void)rc; }
void nvk_prog_warm_stat_draw_hit(void) {}
void nvk_prog_warm_stat_draw_miss(void) {}
void nvk_prog_warm_note(const struct nvk_prog_rec *rec) { (void)rec; }
int nvk_prog_warm_contains(const uint8_t identity[32]) { (void)identity; return 0; }

// ---- 夹具：真缓存文件 ----
static char g_dir[128];

static void ident_fill(uint8_t id[32], unsigned seed) {
    for (int i = 0; i < 32; i++) id[i] = (uint8_t)(seed * 31u + (unsigned)i * 7u + 1u);
    id[0] = (uint8_t)seed; id[1] = (uint8_t)(seed >> 8); id[2] = (uint8_t)(seed >> 16); id[3] = (uint8_t)(seed >> 24);
}
static void path_of(unsigned ident_seed, char *out, size_t n) {
    uint8_t id[32]; ident_fill(id, ident_seed);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", id[i]);
    snprintf(out, n, "%s/%s.program", g_dir, hex);
}
// 把「结果种子 res_seed、长度 len」的结果存成 ident 这个身份的缓存文件：
// 同种子同长度 → 负载逐字节相同 → 头里的结果摘要相同；种子不同 → 摘要不同。
static void put_result(unsigned ident_seed, unsigned res_seed, size_t len) {
    uint8_t id[32]; ident_fill(id, ident_seed);
    uint8_t *buf = (uint8_t *)malloc(len);
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(res_seed * 131u + (unsigned)i * 7u + 3u);
    char why[128] = "";
    int rc = s2a_program_cache_store(g_dir, id, buf, len, why, sizeof why);
    CHECK(rc == 0, "夹具缓存文件写得进（身份种子 %u，结果种子 %u）：%s", ident_seed, res_seed, why);
    free(buf);
}
static int read_head_sha(unsigned ident_seed, uint8_t sha[32]) {
    char p[256]; path_of(ident_seed, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    uint8_t head[88];
    size_t r = fread(head, 1, sizeof head, f);
    fclose(f);
    if (r != sizeof head) return -1;
    memcpy(sha, head + 48, 32);
    return 0;
}
static int forge_head_sha(unsigned ident_seed, const uint8_t sha[32]) {
    char p[256]; path_of(ident_seed, p, sizeof p);
    FILE *f = fopen(p, "r+b");
    if (!f) return -1;
    int ok = fseek(f, 48, SEEK_SET) == 0 && fwrite(sha, 1, 32, f) == 32;
    fclose(f);
    return ok ? 0 : -1;
}
// 走生产预热发布路径把一份程序发进 g_progs。返回 nvk_prog_warm_build 的返回值（0 = 已发布 / 已登记）。
static int publish(unsigned seed, unsigned nst) {
    struct nvk_prog_rec rec;
    memset(&rec, 0, sizeof rec);
    ident_fill(rec.identity, seed);
    rec.stage_mask = nst >= 3 ? 7u : (nst == 2 ? 3u : 1u);
    g_fake_nst = nst;
    return nvk_prog_warm_build(&rec);
}
// 同 publish，另带清单里的 c1（c1_ok 的位 st = 阶段 st 有 c1，内容全填 fill[st]）。
static int publish_c1x(unsigned seed, unsigned nst, unsigned c1_ok, const uint8_t fill[3]) {
    struct nvk_prog_rec rec;
    memset(&rec, 0, sizeof rec);
    ident_fill(rec.identity, seed);
    rec.stage_mask = nst >= 3 ? 7u : (nst == 2 ? 3u : 1u);
    rec.c1_ok = (uint8_t)c1_ok;
    for (unsigned st = 0; st < nst && st < 3; st++) if (c1_ok & (1u << st)) memset(rec.c1[st], fill[st], NVK_PROG_REC_C1_BYTES);
    g_fake_nst = nst;
    return nvk_prog_warm_build(&rec);
}
static unsigned g_held[128];   // 借了还没还的身份种子（用例结束统一归还，免得收尾被借用挡住）
static int g_held_n;
static const struct rp_prog *borrow(unsigned seed) {
    uint8_t id[32]; ident_fill(id, seed);
    const struct rp_prog *p = NULL;
    if (rp_shader_prog_borrow(id, &p) != 1) return NULL;
    if (g_held_n < 128) g_held[g_held_n++] = seed;
    return p;
}
static void give_back(unsigned seed) {
    for (int i = 0; i < g_held_n; i++) if (g_held[i] == seed) { g_held[i] = g_held[--g_held_n]; break; }
    uint8_t id[32]; ident_fill(id, seed);
    rp_shader_prog_return(id);
}
static void give_back_all(void) { while (g_held_n > 0) give_back(g_held[g_held_n - 1]); }
static int resident(unsigned seed) {
    uint8_t id[32]; ident_fill(id, seed);
    const struct rp_prog *p = NULL;
    return rp_shader_prog_by_identity(id, &p) == 1 && p != NULL;
}

// ① 结果相同的身份共用一份程序记录
static void case_basic_alias(void) {
    rp_shader_reset();
    log_reset();
    g_load_calls = g_release_calls = 0;
    put_result(1, 11, 200); put_result(2, 11, 200); put_result(3, 12, 200);
    CHECK(publish(1, 2) == 0, "第一个身份应发布");
    CHECK(g_load_calls == 2, "第一个身份装载两个阶段（实测 %d）", g_load_calls);
    const int l1 = g_load_calls;
    const uint64_t h1 = s2a_program_cache_hits(), m1 = s2a_program_cache_misses(), f1 = s2a_program_cache_failures();
    CHECK(publish(2, 2) == 0, "结果相同的第二个身份应已登记");
    CHECK(g_load_calls == l1, "合并：第二个身份不再装载（装载次数应不变，多出 %d）", g_load_calls - l1);
    CHECK(s2a_program_cache_hits() == h1 && s2a_program_cache_misses() == m1 && s2a_program_cache_failures() == f1,
          "合并不做正式读取：只读 88 字节头，不读负载（正式读取的命中 / 未命中 / 失败计数都不动）");
    const struct rp_prog *p1 = borrow(1), *p2 = borrow(2);
    CHECK(p1 && p2 && p1 == p2, "两个身份借到同一个程序记录（%p / %p）", (const void *)p1, (const void *)p2);
    uint8_t id1[32]; ident_fill(id1, 1);
    CHECK(p1 && !memcmp(p1->identity, id1, 32), "共用的程序记录以先发布的身份为规范身份");
    CHECK(p1 && p1->sh[0] && p1->sh[1] && !p1->sh[2], "程序记录带两个阶段句柄");
    CHECK(rp_shader_prog_borrowed() == 2, "两次借用都记账（实测 %d）", rp_shader_prog_borrowed());
    give_back(1); give_back(2);
    CHECK(rp_shader_prog_borrowed() == 0, "归还后借用数 = 0");
    // 结果不同：照常装载
    const int l2 = g_load_calls;
    CHECK(publish(3, 2) == 0, "结果不同的身份应发布");
    CHECK(g_load_calls - l2 == 2, "结果不同的身份照常装载两个阶段（实测 %d）", g_load_calls - l2);
    const struct rp_prog *p3 = borrow(3);
    CHECK(p3 && p3 != p1, "结果不同的身份有自己的程序记录");
    give_back(3);
    // 已发布的身份再来一次：命中已发布，不装载
    const int l3 = g_load_calls;
    CHECK(publish(1, 2) == 0 && publish(2, 2) == 0, "已发布 / 已登记的身份再次发布仍成功");
    CHECK(g_load_calls == l3, "重复发布不装载（多出 %d）", g_load_calls - l3);
    // 收尾：每份记录只释放一次（2 份记录 × 2 个阶段 = 4，不是 3 个身份 × 2 = 6）
    const int rel0 = g_release_calls;
    rp_shader_reset();
    CHECK(g_release_calls - rel0 == 4, "收尾每份记录只释放一次：期望 4 个阶段句柄，实测 %d", g_release_calls - rel0);
    CHECK(!resident(1) && !resident(2) && !resident(3), "收尾后三个身份都查不到");
}

// ② 收尾后结果表清空：下一局不许合并到上一局已释放的记录
static void case_no_stale_after_reset(void) {
    rp_shader_reset();
    g_load_calls = g_release_calls = 0;
    put_result(1, 11, 200); put_result(2, 11, 200);
    CHECK(publish(1, 2) == 0 && g_load_calls == 2, "上一局：第一个身份装载");
    rp_shader_reset();
    CHECK(g_release_calls == 2, "上一局收尾释放它的两个句柄（实测 %d）", g_release_calls);
    const int l0 = g_load_calls;
    CHECK(publish(2, 2) == 0, "下一局：同结果的另一个身份应发布");
    CHECK(g_load_calls - l0 == 2, "下一局必须重新装载（结果表已随收尾清空；合并到旧记录 = 悬垂）：实测装载 %d", g_load_calls - l0);
    const struct rp_prog *p2 = borrow(2);
    CHECK(p2 && p2->sh[0] && p2->sh[1], "下一局借到的程序记录阶段句柄有效");
    const int l1 = g_load_calls;
    CHECK(publish(1, 2) == 0 && g_load_calls == l1, "下一局里先前那个身份可以合并到新记录（不再装载）");
    const struct rp_prog *p1 = borrow(1);
    CHECK(p1 && p1 == p2, "合并到的是下一局的新记录");
    give_back(1); give_back(2);
    rp_shader_reset();
}

// ③ 摘要相同、长度不同：不合并
static void case_length_guard(void) {
    rp_shader_reset();
    g_load_calls = g_release_calls = 0;
    put_result(5, 21, 200);
    put_result(6, 22, 201);
    uint8_t sha5[32];
    CHECK(read_head_sha(5, sha5) == 0, "读得到规范文件的头");
    CHECK(forge_head_sha(6, sha5) == 0, "把另一个文件头里的结果摘要改成与规范文件相同（长度仍是 201）");
    CHECK(publish(5, 2) == 0, "规范身份应发布");
    const int l0 = g_load_calls;
    const int rc = publish(6, 2);
    CHECK(rc == -1, "摘要相同但长度不同：不得合并；正式读取也因负载摘要不符拒绝它，预热应报失败（实测 %d）", rc);
    CHECK(g_load_calls == l0, "没有为它装载任何着色器（多出 %d）", g_load_calls - l0);
    CHECK(!resident(6), "这个身份不得出现在程序表里");
    rp_shader_reset();
}

// ④ 预热已静默：结果合并也不得登记
static void case_quiesced(void) {
    rp_shader_reset();
    g_load_calls = g_release_calls = 0;
    put_result(7, 31, 200); put_result(8, 31, 200);
    CHECK(publish(7, 2) == 0, "规范身份应发布");
    CHECK(nvk_shader_warm_quiesce(10) == 0, "无在途、无借用：静默成功");
    const int l0 = g_load_calls;
    const int rc = publish(8, 2);
    CHECK(rc == -1, "预热已静默：即使结果已常驻也不得登记（实测 %d）", rc);
    CHECK(g_load_calls == l0 && !resident(8), "静默后没有装载、没有登记");
    rp_shader_reset();   // 收尾清静默位
    CHECK(publish(7, 2) == 0 && publish(8, 2) == 0, "下一局重新允许发布与合并");
    rp_shader_reset();
}

// ⑤ 失败项不参与合并
static void case_failed_not_aliased(void) {
    rp_shader_reset();
    g_load_calls = g_release_calls = 0;
    put_result(9, 41, 200); put_result(10, 41, 200);
    g_parse_fail = 1;
    const int rc9 = publish(9, 2);
    g_parse_fail = 0;
    CHECK(rc9 == -1, "解析失败的身份发布为失败项（实测 %d）", rc9);
    CHECK(!resident(9), "失败项借不到");
    const int l0 = g_load_calls;
    CHECK(publish(10, 2) == 0, "同结果的另一个身份照常建出（失败项不参与合并）");
    CHECK(g_load_calls - l0 == 2, "它自己装载两个阶段（实测 %d）", g_load_calls - l0);
    const struct rp_prog *p = borrow(10);
    CHECK(p && p->sh[0] && p->sh[1], "借到的是可用程序");
    give_back(10);
    rp_shader_reset();
}

// ⑥ 别名身份被借用时收尾拒绝释放；「保留」按记录数
static void case_borrowed_alias_retention(void) {
    rp_shader_reset();
    log_reset();
    g_load_calls = g_release_calls = 0;
    put_result(11, 51, 200); put_result(12, 51, 200);
    CHECK(publish(11, 2) == 0 && publish(12, 2) == 0, "规范身份与别名身份都应已发布");
    const struct rp_prog *p = borrow(12);   // 借的是别名键
    CHECK(p != NULL && p->sh[0] && p->sh[1], "借到别名身份的程序记录");
    const int rel0 = g_release_calls;
    rp_shader_reset();   // 借用未还：拒绝释放（有界等 200 ms）
    CHECK(g_release_calls == rel0, "借用未还时收尾不释放（实测释放 %d）", g_release_calls - rel0);
    CHECK(log_has("收尾保留 1 份"), "读数的「保留」按记录数算（2 个身份键共 1 份记录）（实测：%s）", log_select("程序借用读数"));
    CHECK(p != NULL && p->sh[0] != NULL && p->sh[1] != NULL, "收尾被拒之后程序记录仍可读（ASan 下读已释放内存会判 UAF）");
    give_back(12);
    CHECK(rp_shader_prog_borrowed() == 0, "归还后借用数 = 0");
}

// ⑧ 比对键里由身份推出来的两项（阶段数 / c1）：只改这一项 → 不合并；改回去 → 合并
static void case_key_nst(void) {
    rp_shader_reset();
    g_load_calls = g_release_calls = 0;
    for (unsigned i = 13; i <= 17; i++) put_result(i, 61, 200);
    CHECK(publish(13, 2) == 0 && g_load_calls == 2, "规范身份（2 个阶段）装载两个句柄");
    int l0 = g_load_calls;
    CHECK(publish(14, 3) == 0, "同结果、3 个阶段的身份应发布");
    CHECK(g_load_calls - l0 == 3, "只有阶段数不同（3）：不合并，自己装载 3 个阶段（实测 %d）", g_load_calls - l0);
    l0 = g_load_calls;
    CHECK(publish(15, 1) == 0, "同结果、1 个阶段的身份应发布");
    CHECK(g_load_calls - l0 == 1, "只有阶段数不同（1）：不合并，自己装载 1 个阶段（实测 %d）", g_load_calls - l0);
    const struct rp_prog *p13 = borrow(13), *p14 = borrow(14), *p15 = borrow(15);
    CHECK(p13 && p14 && p15 && p13 != p14 && p13 != p15 && p14 != p15, "阶段数不同的三个身份各有自己的程序记录");
    CHECK(p14 && p14->sh[0] && p14->sh[1] && p14->sh[2], "3 阶段程序带三个句柄");
    CHECK(p15 && p15->sh[0] && !p15->sh[1], "1 阶段程序只带一个句柄");
    l0 = g_load_calls;
    CHECK(publish(16, 2) == 0 && publish(17, 3) == 0, "阶段数相同的身份应登记");
    CHECK(g_load_calls == l0, "阶段数相同的各自合并到各自的规范程序（不再装载，多出 %d）", g_load_calls - l0);
    CHECK(borrow(16) == p13 && borrow(17) == p14, "合并到阶段数相同的那份");
    give_back_all();
    rp_shader_reset();
}
static void case_key_c1(void) {
    rp_shader_reset();
    g_load_calls = g_release_calls = 0;
    for (unsigned i = 20; i <= 29; i++) put_result(i, 71, 200);
    const uint8_t f[3] = {0x11, 0x22, 0x33};
    CHECK(publish_c1x(20, 2, 3, f) == 0 && g_load_calls == 2, "规范身份（阶段 0 / 1 都有 c1）");
    const struct rp_prog *p20 = borrow(20);
    CHECK(p20 && p20->c1_ok == 3 && p20->c1[0][0] == 0x11 && p20->c1[1][0] == 0x22, "程序记录带两段 c1");
    int l0 = g_load_calls;
    CHECK(publish_c1x(21, 2, 3, f) == 0 && g_load_calls == l0 && borrow(21) == p20, "c1 逐字节相同：合并");
    // 只改一个阶段的 c1 内容
    const uint8_t g[3] = {0x11, 0x23, 0x33};
    l0 = g_load_calls;
    CHECK(publish_c1x(22, 2, 3, g) == 0 && g_load_calls - l0 == 2, "只有阶段 1 的 c1 内容不同：不合并（实测装载 %d）", g_load_calls - l0);
    const struct rp_prog *p22 = borrow(22);
    CHECK(p22 && p22 != p20 && p22->c1[1][0] == 0x23, "不合并的身份用自己的 c1");
    // 只改「有没有」：阶段 1 没有 c1
    l0 = g_load_calls;
    CHECK(publish_c1x(23, 2, 1, f) == 0 && g_load_calls - l0 == 2, "只有阶段 1 的 c1「有无」不同：不合并（实测装载 %d）", g_load_calls - l0);
    const struct rp_prog *p23 = borrow(23);
    CHECK(p23 && p23 != p20 && p23->c1_ok == 1, "不合并的身份没有阶段 1 的 c1");
    // 两个阶段都没有 c1 的身份之间照常合并（c1 缺席不是不合并的理由）
    l0 = g_load_calls;
    const uint8_t none[3] = {0, 0, 0};
    CHECK(publish_c1x(26, 2, 0, none) == 0 && g_load_calls - l0 == 2, "与规范身份 c1 有无不同：不合并，自己装载");
    l0 = g_load_calls;
    CHECK(publish_c1x(27, 2, 0, none) == 0 && g_load_calls == l0, "同样没有 c1 的两个身份：合并");
    CHECK(borrow(27) == borrow(26), "…并到同一份");
    // 「有 c1 的是哪个阶段」：阶段 0 有 vs 阶段 1 有同样的内容——字节流相同，只有「有无位」把它们分开
    l0 = g_load_calls;
    const uint8_t only0[3] = {0x44, 0, 0}, only1[3] = {0, 0x44, 0};
    CHECK(publish_c1x(24, 2, 1, only0) == 0 && g_load_calls - l0 == 2, "只在阶段 0 有 c1 的身份：与上面各项都不同，自己装载");
    l0 = g_load_calls;
    CHECK(publish_c1x(25, 2, 2, only1) == 0 && g_load_calls - l0 == 2, "同样的内容只是挪到阶段 1：不合并（实测装载 %d）", g_load_calls - l0);
    give_back_all();
    rp_shader_reset();
}

// ⑦ 关闭（退路 debug.s2a.nvk_alias=0）：逐身份各装载、各释放
// ⑨ 读头的两种失败分开数：这个身份没有缓存文件 → 「没有缓存文件」+1；有文件但头坏（magic 不对）→ 「缓存文件坏」+1，并打一行带原因的黄。
//    换翻译器闭包后的第一局，读头失败几乎全是前一种（offline-shader-miss 调查），原先两种混在「读头失败」里。
extern void nvk_shader_test_alias_probe(uint64_t out[3]);
static void case_probe_split(void) {
    rp_shader_reset();
    uint64_t a[3], b[3];
    nvk_shader_test_alias_probe(a);
    (void)publish(41, 2);   // 没有缓存文件
    nvk_shader_test_alias_probe(b);
    CHECK(b[0] - a[0] == 1 && b[1] - a[1] == 1 && b[2] == a[2], "没有文件：读头 +%llu、没有缓存文件 +%llu、缓存文件坏 +%llu（期望 1 1 0）",
          (unsigned long long)(b[0] - a[0]), (unsigned long long)(b[1] - a[1]), (unsigned long long)(b[2] - a[2]));
    put_result(42, 51, 200);
    { char pth[256]; path_of(42, pth, sizeof pth); FILE *f = fopen(pth, "r+b"); CHECK(f != NULL, "打开夹具缓存文件"); if (f) { fputc('X', f); fclose(f); } }
    log_reset();
    nvk_shader_test_alias_probe(a);
    (void)publish(42, 2);   // 文件头坏
    nvk_shader_test_alias_probe(b);
    CHECK(b[0] - a[0] == 1 && b[1] == a[1] && b[2] - a[2] == 1, "头坏：读头 +%llu、没有缓存文件 +%llu、缓存文件坏 +%llu（期望 1 0 1）",
          (unsigned long long)(b[0] - a[0]), (unsigned long long)(b[1] - a[1]), (unsigned long long)(b[2] - a[2]));
    CHECK(log_has("缓存文件头不对") && log_has("magic"), "头坏打一行带原因（magic）");
    rp_shader_reset();
}

static void case_off(void) {
    rp_shader_reset();
    g_load_calls = g_release_calls = 0;
    put_result(1, 11, 200); put_result(2, 11, 200);
    CHECK(publish(1, 2) == 0 && publish(2, 2) == 0, "两个身份都应发布");
    CHECK(g_load_calls == 4, "开关关闭：每个身份各装载两个阶段，共 4（实测 %d）", g_load_calls);
    const struct rp_prog *p1 = borrow(1), *p2 = borrow(2);
    CHECK(p1 && p2 && p1 != p2, "开关关闭：两个身份各有自己的程序记录");
    give_back(1); give_back(2);
    const int rel0 = g_release_calls;
    rp_shader_reset();
    CHECK(g_release_calls - rel0 == 4, "开关关闭：收尾释放 4 个句柄（实测 %d）", g_release_calls - rel0);
}

static void cleanup_dir(void) {
    DIR *d = opendir(g_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char p[512]; snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name);
            unlink(p);
        }
        closedir(d);
    }
    rmdir(g_dir);
}

int main(int argc, char **argv) {
    const int off = argc > 1 && !strcmp(argv[1], "off");
    const int dflt = argc > 1 && !strcmp(argv[1], "default");
    g_alias_flag = dflt ? -1 : (off ? 0 : 1);
    printf("nvk_shader 结果合并宿主单测（开关 %s；链生产 nvk_shader.c + 真 program_cache.c）\n", off ? "显式关闭" : (dflt ? "取生产缺省" : "显式打开"));
    snprintf(g_dir, sizeof g_dir, "/tmp/s2a-alias-XXXXXX");
    if (!mkdtemp(g_dir)) { fprintf(stderr, "FAIL: mkdtemp\n"); return 1; }
    setenv("S2A_DIRECT_CACHE_DIR", g_dir, 1);
    unsetenv("S2A_SHADER_REVISION_ROOT");
    CHECK(nvk_shader_open() == 0, "会话可开");
    if (off) {
        case_off();
    } else {
        case_basic_alias();
        case_no_stale_after_reset();
        case_length_guard();
        case_quiesced();
        case_failed_not_aliased();
        case_key_nst();
        case_key_c1();
        case_probe_split();
        case_borrowed_alias_retention();   // 留到最后：它按设计泄漏一份记录到进程结束
    }
    cleanup_dir();
    printf("%s：%d 项断言，%d 失败；阶段装载 %d 次、释放 %d 次\n",
           g_fail ? "FAIL" : "PASS", g_checks, g_fail, g_load_calls, g_release_calls);
    return g_fail ? 1 : 0;
}
