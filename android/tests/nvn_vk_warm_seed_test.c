// 首局预热随包种子的宿主回归：链生产 nvn_vk_prog_warm.c + nvn_vk_pipe_warm.c（两个进程单例，每个用例 fork 一个子进程跑，模块状态干净）。
// 覆盖（合同见 nvn_vk_prog_warm.h / nvn_vk_pipe_warm.h「随包种子」）：
//   seed_used     本机清单为空 → 程序清单读 <revision>/shaders/direct/warm-vk/<闭包>-<profile>/ 的种子、管线清单读同目录种子并预建；
//                 种子里程序身份不在程序清单里的管线条目不收；建出的管线描述与手写字节逐字段对上；落盘写本机路径、种子文件不动
//   local_first   本机有程序清单与管线清单 → 两边都用本机的，种子不读
//   local_prog    本机只有程序清单（没有管线清单）→ 管线也不读种子（种子只跟着程序种子走）
//   bad_seed      程序种子校验和坏 → 打黄一行、本局不用种子、管线不读种子；坏条目（阶段掩码非法）逐条跳过不崩
//   bad_pipe      程序种子好、管线种子校验和坏 → 程序照用、管线打黄一行不用
//   undecided     程序清单还没载入时管线 load 返回 -1（稍后重试），程序载入后再 load 收下种子
//   other_profile 种子目录名对得上但文件头是另一台设备 profile 记的 → 打黄不用
//   seed 查无精确件 → 区分其他 profile、其他闭包、精确目录缺件和无随包种子，始终不读其他种子
// 另有 --emit / --installed / --installed-nowindow 三个模式，由 nvk_warm_modules_test.sh 先跑真实安装器再读：--installed-nowindow 读安装后修订里 nowindow/ 的补件（替换管线清单 + 程序结果走受控读取），
//   库外 / 环 / 绝对链接、桶号不符与目录别名由脚本换进去，期望被拒。
// 期望值不来自被测实现：种子文件按格式手写（程序 60 + n*600 + 8：88 字节条目头 + 两段 0x100 字节 c1、管线 32 + n*388），种子目录名由测试自己算 SHA-256，
// 管线紧凑配方的字段偏移按 nvn_vk_pipe_warm.c pw_desc_write 的顺序手算（smask 0、cfmt 1..32、dfmt 33、sfmt 37、samples 41、
// lmap 45..52、prim 53、patch 57、restart 61、blend 62..125、color 126、chmask 130、ms 134..143、attr 144..207、stream 208..335、
// ds 336、poly 344、rast_discard 348、clip_neg1 349、origin_ll 350、depth_clamp 351）。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nvn_vk_prog_warm.h"
#include "nvn_vk_pipe_warm.h"
#include "nvn_plat.h"
#include "rp_shader.h"
#include "direct/compile_request.h"
#include "direct/program_cache.h"
#include "direct/sha256.h"

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ---- 平台 / 来源 / 后端替身 ----
static char g_cache[512];
static const char *g_profile = "profile-A", *g_closure = "closure-A";
static const int32_t g_abi = 7;
static char g_warn[4096];   // 本用例所有黄行（拼起来）
static int g_warnings, g_no_local_info;
const char *nvn_plat_dir(int which) { return which == NVN_PLAT_DIR_CACHE && g_cache[0] ? g_cache : NULL; }
const char *nvk_session_cache_dir(void) { return g_cache[0] ? g_cache : NULL; }
static int g_nowindow = 1;   // nvk_warm_nowindow：缺省开（同产品缺省）；--installed 旧用例关掉，只测程序清单旁的种子叶子
int nvn_plat_flag(const char *name, int dflt) { return !strcmp(name, "nvk_warm_nowindow") ? g_nowindow : dflt; }
void nvn_plat_log(int level, const char *cls, const char *text) {
    (void)cls;
    if (level == NVN_PLAT_LOG_WARN) {
        g_warnings++;
        if (strlen(g_warn) + strlen(text) + 2 < sizeof g_warn) { strcat(g_warn, text); strcat(g_warn, "\n"); }
    }
    if (level == NVN_PLAT_LOG_INFO && strstr(text, "程序清单：目录里还没有这一版翻译器的清单"))
        g_no_local_info++;
}
int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *out) {
    memset(out, 0, sizeof *out);
    out->session = 1; out->generation = 1; out->profile_json = g_profile; out->profile_bytes = strlen(g_profile);
    out->direct_abi = (uint32_t)g_abi; out->translator_closure = g_closure;
    return 0;
}
void nvn_gpu_caps_get(nvn_gpu_caps *out) { if (out) *out = (nvn_gpu_caps){0}; }
void nvn_gpu_pipe_warm_scope(int on) { (void)on; }
uint64_t nvn_gpu_pipe_warm_hits(void) { return 0; }
uint64_t nvn_gpu_pipe_warm_created(void) { return 0; }
uint64_t nvn_gpu_pipe_warm_tag(nvn_gpu_pipe p) { (void)p; return 0; }
void nvn_gpu_pipe_warm_tag_set(nvn_gpu_pipe p, uint64_t tag) { (void)p; (void)tag; }
// 假程序表：P1 / P2 是已发布的图形程序（顶点 + 片元）
static struct rp_prog g_progs[2];
static int g_borrow_n;
int rp_shader_prog_identity(const struct rp_prog *p, uint8_t out[32]) { if (!p) return 0; memcpy(out, p->identity, 32); return 1; }
int rp_shader_prog_by_identity(const uint8_t identity[32], const struct rp_prog **out) {
    for (int i = 0; i < 2; i++) if (!memcmp(identity, g_progs[i].identity, 32)) { if (out) *out = &g_progs[i]; return 1; }
    if (out) *out = NULL;
    return 0;
}
int rp_shader_prog_borrow(const uint8_t identity[32], const struct rp_prog **out) {
    int r = rp_shader_prog_by_identity(identity, out);
    if (r) g_borrow_n++;
    return r;
}
void rp_shader_prog_return(const uint8_t identity[32]) { (void)identity; if (g_borrow_n > 0) g_borrow_n--; }
int rp_shader_prog_borrowed(void) { return g_borrow_n; }
static int g_pipe_get_calls;
static nvn_gpu_pipe_desc g_last_desc;
nvn_gpu_rc nvn_gpu_pipe_get(const nvn_gpu_pipe_desc *d, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) {
    (void)mode; (void)err;
    g_pipe_get_calls++; g_last_desc = *d;
    *out = (nvn_gpu_pipe)(uintptr_t)0x9001;
    return (nvn_gpu_rc){NVN_GPU_OK};
}
nvn_gpu_rc nvn_gpu_cpipe_get(const nvn_gpu_stage_desc *cs, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) {
    (void)cs; (void)mode; (void)err; *out = (nvn_gpu_pipe)(uintptr_t)0x9002;
    return (nvn_gpu_rc){NVN_GPU_OK};
}

// ---- 夹具：手写文件 ----
static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void w64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t fnv(const uint8_t *b, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; } return h; }
static void sha(const char *s, uint8_t out[32]) { s2a_sha256 h; s2a_sha256_init(&h); s2a_sha256_update(&h, s, strlen(s)); s2a_sha256_final(&h, out); }
static void hex8(const uint8_t d[32], char out[17]) { for (int i = 0; i < 8; i++) snprintf(out + 2 * i, 3, "%02x", d[i]); }
static void ident(int n, uint8_t id[32]) { for (int i = 0; i < 32; i++) id[i] = (uint8_t)(n * 31 + i + 1); }
static int write_file(const char *path, const uint8_t *b, size_t n) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    int ok = fwrite(b, 1, n, f) == n; fclose(f); return ok ? 0 : -1;
}
static long file_size(const char *path) { struct stat st; return stat(path, &st) ? -1 : (long)st.st_size; }
// 程序清单文件：ids[i] 的身份 + masks[i]（3 = 顶点 + 片元；5 是非法掩码，用来造坏条目），uses = 10 - i
static void write_prog(const char *path, const int *ids, const uint32_t *masks, int n, const char *profile, int corrupt) {
    size_t len = 60 + (size_t)n * 600 + 8;   // 条目 = 88 字节头 + 顶点 / 片元两段 c1 各 0x100
    uint8_t *b = calloc(1, len);
    uint8_t ps[32], cs[32]; sha(profile, ps); sha(g_closure, cs);
    w32(b, 0x31575053u); w32(b + 4, 2); w32(b + 8, 88); w32(b + 12, (uint32_t)n);
    memcpy(b + 16, ps, 32); w32(b + 48, (uint32_t)g_abi); memcpy(b + 52, cs, 8);
    for (int i = 0; i < n; i++) {
        uint8_t *e = b + 60 + (size_t)i * 600;
        ident(ids[i], e); w32(e + 32, masks[i]);
        for (int k = 0; k < 24; k++) e[36 + k] = (uint8_t)(ids[i] + k);
        e[60] = 3; e[61] = 2;
        w32(e + 64, 0x100); w32(e + 68, 0x100); w32(e + 72, 0);
        w32(e + 76, (uint32_t)(10 - i)); w64(e + 80, (uint64_t)(n - i));
        memset(e + 88, ids[i], 0x200);
    }
    w64(b + len - 8, fnv(b, len - 8) ^ (corrupt ? 1u : 0u));
    CHECK(write_file(path, b, len) == 0, "write %s", path);
    free(b);
}
// 一条手写紧凑配方：每条只填几样，值随 tag 变（tag 1 / 2 / 9 三条互不相同）
static void mkdesc(uint8_t d[352], int tag) {
    memset(d, 0, 352);
    d[0] = 0x3;                                       // smask：顶点 + 片元
    w32(d + 1, 0x25u + (uint32_t)tag);                // cfmt[0]
    w32(d + 33, 0x3eu);                               // dfmt
    w32(d + 41, 4);                                   // samples
    d[45] = 0; d[46] = 1;                             // lmap[0..1]
    w32(d + 53, 5u + (uint32_t)tag);                  // prim
    d[61] = 1;                                        // prim_restart
    w64(d + 62, 1u | (uint64_t)0x10 << 3 | (uint64_t)0x11 << 10);   // blend[0]：target 1、src_rgb 0x10、dst_rgb 0x11
    w32(d + 130, 0xFu);                               // chmask
    w32(d + 144, 0x2Au | 12u << 8 | 1u << 27);        // attr[0]：fmt 0x2A、offset 12、stream 1
    w32(d + 208, 24); w32(d + 212, 0);                // stream[0]：stride 24
    w32(d + 344, 2u | 1u << 2);                       // poly：cull 2、front 1
    d[350] = 1;                                       // origin_ll
    d[351] = (uint8_t)(tag & 1);                      // depth_clamp
}
static void write_pipe(const char *path, const int *ids, int n, int corrupt) {
    size_t len = 32 + (size_t)n * 388;
    uint8_t *b = calloc(1, len);
    memcpy(b, "S2APW1\0\0", 8); w32(b + 8, 1); w32(b + 12, 352); w32(b + 16, 388); w32(b + 20, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        uint8_t *e = b + 32 + (size_t)i * 388;
        ident(ids[i], e); mkdesc(e + 32, ids[i]); w32(e + 384, (uint32_t)(5 - i));
    }
    w64(b + 24, fnv(b + 32, len - 32) ^ (corrupt ? 1u : 0u));
    CHECK(write_file(path, b, len) == 0, "write %s", path);
    free(b);
}

static char g_root[512], g_seed[700], g_seed_prog[800], g_seed_pipe[800], g_local_prog[800], g_local_pipe[800];
static void setup(const char *base, const char *name) {
    snprintf(g_root, sizeof g_root, "%s/%s/rev", base, name);
    snprintf(g_cache, sizeof g_cache, "%s/%s/cache", base, name);
    char d[800];
    snprintf(d, sizeof d, "%s/%s", base, name); mkdir(d, 0700);
    mkdir(g_root, 0700); mkdir(g_cache, 0700);
    snprintf(d, sizeof d, "%s/shaders", g_root); mkdir(d, 0700);
    snprintf(d, sizeof d, "%s/shaders/direct", g_root); mkdir(d, 0700);
    snprintf(d, sizeof d, "%s/shaders/direct/warm-vk", g_root); mkdir(d, 0700);
    uint8_t cs[32], ps[32]; sha(g_closure, cs); sha(g_profile, ps);
    char c16[17], p16[17]; hex8(cs, c16); hex8(ps, p16);
    snprintf(g_seed, sizeof g_seed, "%s/shaders/direct/warm-vk/%s-%s", g_root, c16, p16); mkdir(g_seed, 0700);
    snprintf(g_seed_prog, sizeof g_seed_prog, "%s/programs-v1-%s.bin", g_seed, c16);
    snprintf(g_seed_pipe, sizeof g_seed_pipe, "%s/pipes-v1-352.bin", g_seed);
    snprintf(g_local_prog, sizeof g_local_prog, "%s/programs-v1-%s.bin", g_cache, c16);
    snprintf(g_local_pipe, sizeof g_local_pipe, "%s/pipes-v1-352.bin", g_cache);
    setenv("S2A_SHADER_REVISION_ROOT", g_root, 1);
    ident(1, g_progs[0].identity); g_progs[0].sh[0] = (nvn_gpu_shader)(uintptr_t)0x101; g_progs[0].sh[1] = (nvn_gpu_shader)(uintptr_t)0x102;
    ident(2, g_progs[1].identity); g_progs[1].sh[0] = (nvn_gpu_shader)(uintptr_t)0x201; g_progs[1].sh[1] = (nvn_gpu_shader)(uintptr_t)0x202;
}
static const int k_seed_prog[] = {1, 2}; static const uint32_t k_masks[] = {3, 3, 5};
static const int k_seed_pipe[] = {1, 2, 9};   // 9：程序不在程序种子里
static const int k_nowindow_ident = 7;
static const uint8_t k_nowindow_result[] = "nowindow program result bytes: opaque to the cache, checked by digest only";

static void case_seed_used(void) {
    write_prog(g_seed_prog, k_seed_prog, k_masks, 2, g_profile, 0);
    write_pipe(g_seed_pipe, k_seed_pipe, 3, 0);
    long seed_prog_len = file_size(g_seed_prog), seed_pipe_len = file_size(g_seed_pipe);
    CHECK(nvk_prog_warm_load() == 2, "程序清单从种子收下 2 条");
    char sd[800]; int s = nvk_prog_warm_seed_dir(sd, sizeof sd);
    CHECK(s == 1 && !strcmp(sd, g_seed), "种子目录 = 手算的 <闭包>-<profile>（得 %d %s）", s, sd);
    CHECK(nvk_pipe_warm_load() == 2, "管线清单从种子收下 2 条（身份 9 的程序不在程序清单里，不收）");
    CHECK(!g_warnings, "matching installed seeds emit no warnings (%s)", g_warn);
    struct nvk_pipe_warm_stats qs; nvk_pipe_warm_stats(&qs);
    CHECK(qs.rejected == 1, "不收的 1 条计入 rejected（得 %llu）", (unsigned long long)qs.rejected);
    struct nvk_pipe_rec r; int n = 0, saw1 = 0;
    while (nvk_pipe_warm_take(&r)) {
        n++;
        CHECK(nvk_pipe_warm_build(&r) == 0, "种子管线预建成功");
        uint8_t i1[32]; ident(1, i1);
        if (!memcmp(r.prog_identity, i1, 32)) {
            saw1 = 1;
            const nvn_gpu_pipe_desc *d = &g_last_desc;
            CHECK(d->st[0].sh == g_progs[0].sh[0] && d->st[1].sh == g_progs[0].sh[1] && !d->st[2].sh, "阶段句柄取自程序 P1");
            CHECK(d->cfmt[0] == 0x26 && d->cfmt[1] == 0, "cfmt[0] = 0x25 + 1（得 %#x）", d->cfmt[0]);
            CHECK(d->dfmt == 0x3e && d->sfmt == 0 && d->samples == 4, "dfmt 0x3e / sfmt 0 / samples 4");
            CHECK(d->lmap[0] == 0 && d->lmap[1] == 1, "lmap 0 1");
            CHECK(d->prim == 6 && d->patch_size == 0 && d->prim_restart == 1, "prim 6 / patch 0 / restart 1（得 %u）", d->prim);
            CHECK(d->blend[0].target == 1 && d->blend[0].src_rgb == 0x10 && d->blend[0].dst_rgb == 0x11 && d->blend[0].src_a == 0, "blend[0]");
            CHECK(d->chmask.mask == 0xF, "chmask 0xF");
            CHECK(d->attr[0].fmt == 0x2A && d->attr[0].offset == 12 && d->attr[0].stream == 1, "attr[0]");
            CHECK(d->stream[0].stride == 24 && d->stream[0].divisor == 0, "stream[0]");
            CHECK(d->poly.cull == 2 && d->poly.front == 1 && d->poly.mode == 0, "poly");
            CHECK(d->rast_discard == 0 && d->clip_neg1 == 0 && d->origin_ll == 1 && d->depth_clamp == 1, "末尾四个字节");
        }
    }
    CHECK(n == 2 && saw1 && g_pipe_get_calls == 2, "预热集 2 条、都建了（取 %d、建 %d）", n, g_pipe_get_calls);
    // 本局记一条新程序 → 落盘写本机路径；种子文件不动
    static uint8_t c1b[0x100]; struct nvk_prog_rec pr; memset(&pr, 0, sizeof pr); ident(3, pr.identity); pr.stage_mask = 3; pr.c1_ok = 3; pr.uses = 1;
    pr.c1[0] = pr.c1[1] = c1b; pr.c1_len[0] = pr.c1_len[1] = 0x100;
    nvk_prog_warm_note(&pr);
    nvk_prog_warm_save();
    CHECK(file_size(g_local_prog) == 60 + 3 * 600 + 8, "落盘写本机路径：种子 2 条 + 本局 1 条（得 %ld 字节）", file_size(g_local_prog));
    CHECK(file_size(g_seed_prog) == seed_prog_len && file_size(g_seed_pipe) == seed_pipe_len, "种子文件不动");
}
static void case_local_first(void) {
    write_prog(g_seed_prog, k_seed_prog, k_masks, 2, g_profile, 0);
    write_pipe(g_seed_pipe, k_seed_pipe, 3, 0);
    static const int lp[] = {2}; write_prog(g_local_prog, lp, k_masks, 1, g_profile, 0);
    write_pipe(g_local_pipe, lp, 1, 0);
    CHECK(nvk_prog_warm_load() == 1, "有本机程序清单：用本机的 1 条");
    CHECK(nvk_prog_warm_seed_dir(NULL, 0) == 0, "种子状态 = 没用");
    CHECK(nvk_pipe_warm_load() == 1, "有本机管线清单：用本机的 1 条");
}
static void case_local_prog(void) {
    write_prog(g_seed_prog, k_seed_prog, k_masks, 2, g_profile, 0);
    write_pipe(g_seed_pipe, k_seed_pipe, 3, 0);
    static const int lp[] = {2}; write_prog(g_local_prog, lp, k_masks, 1, g_profile, 0);
    CHECK(nvk_prog_warm_load() == 1, "本机程序清单 1 条");
    CHECK(nvk_pipe_warm_load() == 0, "本机没有管线清单、程序没用种子：管线也不读种子（0 条）");
}
static void case_bad_seed(void) {
    write_prog(g_seed_prog, k_seed_prog, k_masks, 2, g_profile, 1);   // 校验和坏
    write_pipe(g_seed_pipe, k_seed_pipe, 3, 0);
    CHECK(nvk_prog_warm_load() == 0, "程序种子校验和坏：本局 0 条");
    CHECK(strstr(g_warn, "程序清单随包种子不用") != NULL, "打一行黄（%s）", g_warn);
    CHECK(nvk_prog_warm_seed_dir(NULL, 0) == 0, "种子状态 = 没用");
    CHECK(nvk_pipe_warm_load() == 0, "管线不读种子");
    // 坏条目：三条里第三条阶段掩码 5 非法 → 逐条跳过，前两条照收（同进程换一份好种子再 reset 载入）
    static const int ids3[] = {1, 2, 4};
    write_prog(g_seed_prog, ids3, k_masks, 3, g_profile, 0);
    nvk_prog_warm_reset();
    CHECK(nvk_prog_warm_load() == 2, "坏条目跳过：3 条收 2 条");
    struct nvk_prog_warm_stats ps; nvk_prog_warm_stats(&ps);
    CHECK(ps.rejected == 1 && nvk_prog_warm_seed_dir(NULL, 0) == 1, "坏条目计 1、种子照用");
}
static void case_bad_pipe(void) {
    write_prog(g_seed_prog, k_seed_prog, k_masks, 2, g_profile, 0);
    write_pipe(g_seed_pipe, k_seed_pipe, 3, 1);   // 校验和坏
    CHECK(nvk_prog_warm_load() == 2, "程序种子照用");
    CHECK(nvk_pipe_warm_load() == 0, "管线种子坏：0 条");
    CHECK(strstr(g_warn, "管线预热清单随包种子不用") != NULL, "打一行黄（%s）", g_warn);
}
static void case_undecided(void) {
    write_prog(g_seed_prog, k_seed_prog, k_masks, 2, g_profile, 0);
    write_pipe(g_seed_pipe, k_seed_pipe, 3, 0);
    CHECK(nvk_pipe_warm_load() == -1, "程序清单还没载入：管线 load 返回 -1 稍后重试");
    CHECK(nvk_prog_warm_load() == 2, "程序种子 2 条");
    CHECK(nvk_pipe_warm_load() == 2, "重试时收下管线种子 2 条");
}
static void case_other_profile(void) {
    write_prog(g_seed_prog, k_seed_prog, k_masks, 2, "profile-X", 0);   // 目录名对得上，文件头是别的设备记的
    write_pipe(g_seed_pipe, k_seed_pipe, 3, 0);
    CHECK(nvk_prog_warm_load() == 0, "别的 profile 记的种子不用");
    CHECK(strstr(g_warn, "随包种子不用") && strstr(g_warn, "profile"), "黄行写明 profile 不同（%s）", g_warn);
    CHECK(nvk_pipe_warm_load() == 0, "管线也不读种子");
}

static void write_other_seed(const char *closure, const char *profile, char key[34]) {
    uint8_t cs[32], ps[32];
    char c16[17], p16[17], dir[700], program[800], pipe[800];
    sha(closure, cs); sha(profile, ps); hex8(cs, c16); hex8(ps, p16);
    snprintf(key, 34, "%s-%s", c16, p16);
    snprintf(dir, sizeof dir, "%s/shaders/direct/warm-vk/%s", g_root, key);
    CHECK(mkdir(dir, 0700) == 0, "create a different seed directory");
    snprintf(program, sizeof program, "%s/programs-v1-%s.bin", dir, c16);
    snprintf(pipe, sizeof pipe, "%s/pipes-v1-352.bin", dir);
    const char *expected_closure = g_closure;
    g_closure = closure;
    write_prog(program, k_seed_prog, k_masks, 2, profile, 0);
    g_closure = expected_closure;
    write_pipe(pipe, k_seed_pipe, 3, 0);
}

static void check_seed_not_loaded(void) {
    CHECK(nvk_prog_warm_load() == 0, "no program seed adopted for the live key");
    CHECK(nvk_prog_warm_seed_dir(NULL, 0) == 0, "no seed directory published for the live key");
    CHECK(nvk_pipe_warm_load() == 0, "no pipeline seed adopted for the live key");
}

static void case_seed_profile_mismatch(void) {
    char sibling[34], other_closure[34];
    CHECK(rmdir(g_seed) == 0, "remove exact cohort");
    write_other_seed("closure-B", "profile-B", other_closure);
    write_other_seed(g_closure, "profile-B", sibling);
    check_seed_not_loaded();
    CHECK(nvk_prog_warm_load() == 0, "same session repeated load still adopts no seed");
    const char *live_key = strrchr(g_seed, '/') + 1;
    CHECK(g_warnings == 1 && strstr(g_warn, "warm_seed_profile_mismatch") &&
          strstr(g_warn, live_key) && strstr(g_warn, sibling),
          "missing exact seed emits one profile mismatch with live and sibling keys (%s)", g_warn);
}

static void case_seed_closure_mismatch(void) {
    char sibling[34];
    CHECK(rmdir(g_seed) == 0, "remove exact cohort");
    write_other_seed("closure-B", g_profile, sibling);
    check_seed_not_loaded();
    CHECK(g_warnings == 1 && strstr(g_warn, "warm_seed_closure_mismatch") && strstr(g_warn, sibling),
          "only another closure emits one closure mismatch (%s)", g_warn);
}

static void case_seed_program_missing(void) {
    char sibling[34];
    write_other_seed(g_closure, "profile-B", sibling);
    write_pipe(g_seed_pipe, k_seed_pipe, 3, 0);
    check_seed_not_loaded();
    CHECK(g_warnings == 1 && strstr(g_warn, "warm_seed_program_missing"),
          "exact cohort without the program file emits one missing-program warning (%s)", g_warn);
}

static void case_no_seed_candidates(void) {
    char warm_root[700], candidate[800], path[900], alias_target[700], alias_key[34];
    CHECK(rmdir(g_seed) == 0, "remove exact cohort");
    snprintf(warm_root, sizeof warm_root, "%s/shaders/direct/warm-vk", g_root);
    snprintf(candidate, sizeof candidate, "%s/1111111111111111-2222222222222222", warm_root);
    CHECK(mkdir(candidate, 0700) == 0, "create an empty unrelated directory");
    snprintf(path, sizeof path, "%s/pipes-v1-352.bin", candidate);
    write_pipe(path, k_seed_pipe, 3, 0);
    write_other_seed("closure-B", "profile-B", alias_key);
    snprintf(candidate, sizeof candidate, "%s/%s", warm_root, alias_key);
    snprintf(alias_target, sizeof alias_target, "%s/aliased-cohort", g_cache);
    CHECK(rename(candidate, alias_target) == 0, "move valid seeds out of the package");
    CHECK(symlink(alias_target, candidate) == 0, "create a cohort directory alias with usable seed bytes");
    check_seed_not_loaded();
    CHECK(!g_warnings && g_no_local_info == 1, "an empty or aliased cohort never triggers a mismatch warning (%s)", g_warn);
}

static void case_no_seed_root(void) {
    char warm_root[700];
    CHECK(rmdir(g_seed) == 0, "remove exact cohort");
    snprintf(warm_root, sizeof warm_root, "%s/shaders/direct/warm-vk", g_root);
    CHECK(rmdir(warm_root) == 0, "remove the warm-vk root");
    check_seed_not_loaded();
    CHECK(!g_warnings && g_no_local_info == 1, "a package without seeds keeps the normal info path (%s)", g_warn);
}

static const struct { const char *name; void (*fn)(void); } k_cases[] = {
    {"seed_used", case_seed_used}, {"local_first", case_local_first}, {"local_prog", case_local_prog},
    {"bad_seed", case_bad_seed}, {"bad_pipe", case_bad_pipe}, {"undecided", case_undecided}, {"other_profile", case_other_profile},
    {"seed_profile_mismatch", case_seed_profile_mismatch}, {"seed_closure_mismatch", case_seed_closure_mismatch},
    {"seed_program_missing", case_seed_program_missing}, {"no_seed_candidates", case_no_seed_candidates}, {"no_seed_root", case_no_seed_root},
};

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--emit")) {
        setup(argv[2], "input");
        write_prog(g_seed_prog, k_seed_prog, k_masks, 2, g_profile, 0);
        write_pipe(g_seed_pipe, k_seed_pipe, 3, 0);
        // 非窗口补件（nowindow/）：一份普通程序结果（程序缓存格式，头 / 摘要合法）+ 一份只有 1 条的替换管线清单（与上一级的 2 条区分开）
        char nw[800]; snprintf(nw, sizeof nw, "%s/nowindow", g_seed); mkdir(nw, 0700);
        uint8_t id[32]; ident(k_nowindow_ident, id);
        char why[128] = "";
        CHECK(s2a_program_cache_store(nw, id, k_nowindow_result, sizeof k_nowindow_result, why, sizeof why) == 0, "store nowindow result (%s)", why);
        char np[900]; snprintf(np, sizeof np, "%s/pipes-v1-352.bin", nw);
        static const int one[] = {2};
        write_pipe(np, one, 1, 0);
        return failures != 0;
    }
    // --installed-nowindow <revision> <cache> <程序条数> <管线条数> <补件程序结果 0|1>：补件开着读，管线读 nowindow/ 的替换清单，程序结果走受控读取
    if (argc == 7 && !strcmp(argv[1], "--installed-nowindow")) {
        snprintf(g_cache, sizeof g_cache, "%s", argv[3]);
        setenv("S2A_SHADER_REVISION_ROOT", argv[2], 1);
        int progs = atoi(argv[4]), pipes = atoi(argv[5]), result = atoi(argv[6]);
        CHECK(nvk_prog_warm_load() == progs, "installed program seed: expected %d rows (%s)", progs, g_warn);
        CHECK(nvk_pipe_warm_load() == pipes, "installed nowindow pipe recipes: expected %d rows (%s)", pipes, g_warn);
        char sd[600], dir[700];
        CHECK(nvk_prog_warm_seed_dir(sd, sizeof sd) == 1, "program seed in use");
        snprintf(dir, sizeof dir, "%s/nowindow", sd);
        uint8_t id[32], sha_out[32], *bytes = NULL; size_t len = 0; char why[160] = "";
        ident(k_nowindow_ident, id);
        int rc = s2a_program_cache_load_installed(dir, id, &bytes, &len, sha_out, why, sizeof why);
        CHECK((rc == 0) == (result != 0), "installed nowindow program result: expected %s, got rc=%d (%s)", result ? "loaded" : "rejected", rc, why);
        if (!rc) CHECK(len == sizeof k_nowindow_result && !memcmp(bytes, k_nowindow_result, len), "installed nowindow program result bytes");
        free(bytes);
        return failures != 0;
    }
    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "--installed")) {
        g_nowindow = 0;
        snprintf(g_cache, sizeof g_cache, "%s", argv[3]);
        setenv("S2A_SHADER_REVISION_ROOT", argv[2], 1);
        int expected = atoi(argv[4]);
        CHECK(nvk_prog_warm_load() == expected, "installed program seed: expected %d rows (%s)", expected, g_warn);
        int pipes = argc == 6 ? atoi(argv[5]) : expected;
        CHECK(nvk_pipe_warm_load() == pipes, "installed pipe seed: expected %d rows (%s)", pipes, g_warn);
        return failures != 0;
    }
    const char *tmpdir = getenv("TMPDIR");
    char tmpl[400];
    if (snprintf(tmpl, sizeof tmpl, "%s/nvk-warm-seed-XXXXXX", tmpdir && *tmpdir ? tmpdir : "/tmp") >= (int)sizeof tmpl)
        return 1;
    char *base = mkdtemp(tmpl);
    if (!base) { fprintf(stderr, "FAIL: temp dir\n"); return 1; }
    int bad = 0, n = (int)(sizeof k_cases / sizeof k_cases[0]);
    for (int i = 0; i < n; i++) {
        fflush(stdout); fflush(stderr);
        pid_t pid = fork();
        if (pid == 0) {
            setup(base, k_cases[i].name);
            k_cases[i].fn();
            fprintf(stderr, "%s %s：%d 项检查，%d 项失败\n", failures ? "FAIL" : "ok", k_cases[i].name, checks, failures);
            _exit(failures ? 1 : 0);
        }
        int st = 0; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) { bad++; fprintf(stderr, "FAIL: 用例 %s 没过（状态 %d）\n", k_cases[i].name, st); }
    }
    pid_t cleanup = fork();
    if (cleanup == 0) { execlp("rm", "rm", "-rf", "--", base, (char *)NULL); _exit(1); }
    int cleanup_status = 0;
    if (cleanup < 0 || waitpid(cleanup, &cleanup_status, 0) != cleanup || !WIFEXITED(cleanup_status) || WEXITSTATUS(cleanup_status))
        return 1;
    if (bad) { fprintf(stderr, "FAIL: 随包种子 %d/%d 个用例没过\n", bad, n); return 1; }
    printf("PASS: 随包种子 %d 个用例全过\n", n);
    return 0;
}
