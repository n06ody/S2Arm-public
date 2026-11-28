// 管线预热清单的宿主单测：链生产 nvk_vk_pipe_warm.c，假程序来源 + 假 nvn_gpu_pipe_get。
// 覆盖：逐字段往返（每个字段改一点都必须往返后不等）、同配方去重与 uses、散列撞但字节不同必须不命中、
// 上限与淘汰（含缺省上限 8192 与硬顶）、跨会话落盘/读回（顺序 = uses 降序）、坏文件整份拒绝、stats 转发。
//
// 跨会话的部分用「同一个二进制换参数再跑一次」实现（模块是进程单例，load 只读一次）：
//   父进程建临时目录、按顺序子进程跑各模式；缓存目录经环境变量交给子进程。
// 用法：nvn_vk_pipe_warm_test            （全部模式）
//       nvn_vk_pipe_warm_test <模式名>   （单跑一个模式，交给父进程用）
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "nvn_vk_pipe_warm.h"
#include "rp_shader.h"

// ---- 平台 / 来源 / 后端替身 ----
static struct rp_prog g_prog;          // 假程序：身份 + 三个阶段句柄
static int g_pipe_get_calls;
static nvn_gpu_pipe_desc g_last_desc;  // 假 nvn_gpu_pipe_get 收到的描述
static int g_pipe_fail;
static uint64_t g_fake_hits = 12345;

int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_warm_reactive")) return getenv("S2A_TEST_WARM_REACTIVE") != NULL;
    if (!strcmp(name, "nvk_compute_warm")) return getenv("S2A_COMPUTE_WARM") ? atoi(getenv("S2A_COMPUTE_WARM")) : dflt;
    if (!strcmp(name, "nvk_warm_max")) { const char *e = getenv("S2A_PW_MAX"); return e && *e ? atoi(e) : dflt; }
    if (!strcmp(name, "nvk_pw_diag")) { const char *e = getenv("S2A_PW_DIAG"); return e && *e ? atoi(e) : dflt; }   // #1372：诊断开关，缺省 0
    return dflt;
}
// 日志替身：把诊断行（pipe 类、带「预热清单诊断」）存下来给用例看，其余丢弃。
// 「不在清单锁里打日志」的探针（#1372 评审）：g_probe_lock 开着时，每条诊断行出来时另起一个线程去拿清单锁
// （nvk_pipe_warm_diag_stats 会进 g.mu），最多等 300 毫秒——拿不到，说明这条日志是持着清单锁打的。
// 探针线程用完即走（detached），完成标志放在静态数组里、每次探针一格：持锁打日志的变异体会让它晚于本函数返回才完成，
// 标志不能放在本函数的栈上。
#define NLOG 256
static char g_logs[NLOG][1024];
static int g_nlogs;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_probe_lock;               // 1 = 开探针
static int g_probe_n, g_probe_blocked; // 探针跑了几次 / 其中几次拿不到锁
static int g_probe_done[NLOG];
static void *probe_thread(void *p) {
    struct nvk_pipe_warm_diag t;
    nvk_pipe_warm_diag_stats(&t);
    __atomic_store_n((int *)p, 1, __ATOMIC_RELEASE);
    return NULL;
}
void nvn_plat_log(int level, const char *cls, const char *text) {
    (void)level;
    if (!(cls && text && !strcmp(cls, "pipe") && strstr(text, "预热清单诊断"))) return;
    pthread_mutex_lock(&g_log_mu);
    if (g_nlogs < NLOG) snprintf(g_logs[g_nlogs++], sizeof g_logs[0], "%s", text);
    pthread_mutex_unlock(&g_log_mu);
    if (g_probe_lock && g_probe_n < NLOG) {
        int *done = &g_probe_done[g_probe_n];
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &at, probe_thread, done) == 0) {
            g_probe_n++;
            for (int i = 0; i < 300 && !__atomic_load_n(done, __ATOMIC_ACQUIRE); i++) usleep(1000);
            if (!__atomic_load_n(done, __ATOMIC_ACQUIRE)) g_probe_blocked++;
        }
        pthread_attr_destroy(&at);
    }
}
const char *nvk_session_cache_dir(void) { return getenv("S2A_PW_DIR"); }
// 程序清单的随包种子（nvn_vk_prog_warm.c；本单测只链管线模块）：本局没用种子 = 管线也不读种子（种子用例在 nvn_vk_warm_seed_test.c）
int nvk_prog_warm_seed_dir(char *out, size_t n) { if (out && n) out[0] = 0; return 0; }
int nvk_prog_warm_contains(const uint8_t identity[32]) { (void)identity; return 0; }
void nvn_gpu_pipe_warm_scope(int on) { (void)on; }
uint64_t nvn_gpu_pipe_warm_hits(void) { return g_fake_hits; }
uint64_t nvn_gpu_pipe_warm_created(void) { return 0; }
// 假后端的管线标签（真后端把标签存在管线对象上，对象销毁时一起没；这里按指针查一张小表）
#define FAKE_NP_MAX 64
static struct { nvn_gpu_pipe p; uint64_t tag; } g_np_tag[FAKE_NP_MAX];
uint64_t nvn_gpu_pipe_warm_tag(nvn_gpu_pipe p) {
    for (int i = 0; i < FAKE_NP_MAX; i++) if (g_np_tag[i].p == p) return g_np_tag[i].tag;
    return 0;
}
void nvn_gpu_pipe_warm_tag_set(nvn_gpu_pipe p, uint64_t tag) {
    int free_i = -1;
    for (int i = 0; i < FAKE_NP_MAX; i++) {
        if (g_np_tag[i].p == p) { g_np_tag[i].tag = tag; return; }
        if (!g_np_tag[i].p && free_i < 0) free_i = i;
    }
    if (free_i >= 0) { g_np_tag[free_i].p = p; g_np_tag[free_i].tag = tag; }
}
static void fake_np_forget(nvn_gpu_pipe p) {   // 模拟「这条管线对象没了、同一个地址又建了一条新的」：标签清零
    for (int i = 0; i < FAKE_NP_MAX; i++) if (g_np_tag[i].p == p) { g_np_tag[i].p = NULL; g_np_tag[i].tag = 0; }
}

int rp_shader_prog_identity(const struct rp_prog *p, uint8_t out[32]) {
    if (!p) return 0;
    memcpy(out, p->identity, 32);
    return 1;
}
int rp_shader_prog_by_identity(const uint8_t identity[32], const struct rp_prog **out) {
    if (out) *out = NULL;
    if (memcmp(identity, g_prog.identity, 32) != 0) return 0;
    if (out) *out = &g_prog;
    return 1;
}
// 程序借用（rp_shader.h）：管线预热整段使用期间必须持借用，桩里直接转发到上面的假程序表。
static int g_borrow_n;
static int g_borrow_fail;   // 用例：模拟「会话已停 / 借用表满」的拒绝
int rp_shader_prog_borrow(const uint8_t identity[32], const struct rp_prog **out) {
    if (out) *out = NULL;
    if (g_borrow_fail) return -1;
    if (memcmp(identity, g_prog.identity, 32) != 0) return 0;
    if (out) *out = &g_prog;
    g_borrow_n++;
    return 1;
}
void rp_shader_prog_return(const uint8_t identity[32]) { (void)identity; if (g_borrow_n > 0) g_borrow_n--; }
int rp_shader_prog_borrowed(void) { return g_borrow_n; }
nvn_gpu_rc nvn_gpu_pipe_get(const nvn_gpu_pipe_desc *d, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) {
    (void)mode;
    (void)err;
    if (!d || !out) return (nvn_gpu_rc){NVN_GPU_E_ARG};
    g_pipe_get_calls++;
    g_last_desc = *d;
    if (g_pipe_fail) return (nvn_gpu_rc){NVN_GPU_E_COMPILE};
    *out = (nvn_gpu_pipe)(uintptr_t)0x9001;
    return (nvn_gpu_rc){NVN_GPU_OK};
}

static int g_compute_pipe_calls;
static nvn_gpu_shader g_compute_pipe_shader;
nvn_gpu_rc nvn_gpu_cpipe_get(const nvn_gpu_stage_desc *cs, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) {
    (void)mode; (void)err;
    if (!cs || !cs->sh || !out) return (nvn_gpu_rc){NVN_GPU_E_ARG};
    g_compute_pipe_calls++; g_compute_pipe_shader = cs->sh;
    *out = (nvn_gpu_pipe)(uintptr_t)0x9002;
    return (nvn_gpu_rc){NVN_GPU_OK};
}

// ---- 断言 ----
static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); g_fail = 1; } } while (0)

// ---- 基线描述：每个字段取一个可 +1 的小值；两个阶段有句柄（句柄不进记录） ----
static void base_desc(nvn_gpu_pipe_desc *d) {
    memset(d, 0, sizeof *d);
    d->st[0].sh = (nvn_gpu_shader)(uintptr_t)0x1001;
    d->st[1].sh = (nvn_gpu_shader)(uintptr_t)0x1002;
    d->cfmt[0] = 0x23; d->cfmt[1] = 0x25;
    d->dfmt = 0x2a; d->sfmt = 0; d->samples = 1;
    d->lmap[0] = 0; for (int i = 1; i < 8; i++) d->lmap[i] = 0xff;
    d->prim = 4; d->patch_size = 0; d->prim_restart = 0;
    for (int i = 0; i < 8; i++) {
        d->blend[i].target = 0; d->blend[i].src_rgb = 1; d->blend[i].dst_rgb = 2; d->blend[i].src_a = 3;
        d->blend[i].dst_a = 4; d->blend[i].eq_rgb = 1; d->blend[i].eq_a = 2;
        d->blend[i].adv_mode = 0; d->blend[i].adv_overlap = 0; d->blend[i].adv_premul = 0; d->blend[i].adv_normdst = 0;
    }
    d->color.blend_en = 1; d->color.logic_op = 1; d->color.alpha_func = 1;
    d->chmask.mask = 0x00001111u;
    d->ms.enable = 0; d->ms.samples = 1; d->ms.a2c = 0; d->ms.a2c_dither = 0; d->ms.raster_samples = 1;
    d->ms.cov_mod = 0; d->ms.cov2color_en = 0; d->ms.cov2color_out = 0; d->ms.sl_en = 0; d->ms.sl_grid = 0;
    for (int i = 0; i < 16; i++) { d->attr[i].fmt = 0; d->attr[i].offset = 0; d->attr[i].stream = 0; }
    d->attr[0].fmt = 0x17; d->attr[0].offset = 8;
    for (int i = 0; i < 16; i++) { d->stream[i].stride = 0; d->stream[i].divisor = 0; }
    d->stream[0].stride = 16;
    d->ds.dt = 1; d->ds.dw = 1; d->ds.df = 1; d->ds.st = 0;
    d->ds.sf_f = 1; d->ds.sf_b = 2; d->ds.fail_f = 1; d->ds.zfail_f = 2; d->ds.zpass_f = 3;
    d->ds.fail_b = 4; d->ds.zfail_b = 5; d->ds.zpass_b = 6;
    d->poly.cull = 1; d->poly.front = 0; d->poly.mode = 2; d->poly.offset_en = 1;
    d->rast_discard = 0; d->clip_neg1 = 0; d->origin_ll = 1; d->depth_clamp = 0;
}

static void ident_fill(uint8_t id[32], uint8_t seed) {
    for (int i = 0; i < 32; i++) id[i] = (uint8_t)(seed + i * 7u);
}
static void prog_setup(uint8_t seed) {
    memset(&g_prog, 0, sizeof g_prog);
    ident_fill(g_prog.identity, seed);
    g_prog.sh[0] = (nvn_gpu_shader)(uintptr_t)0x1001;
    g_prog.sh[1] = (nvn_gpu_shader)(uintptr_t)0x1002;
    g_prog.sh[2] = NULL;
}

// ---- 逐字段改动表：kind + 下标，mutate 把该字段改「一点点」 ----
enum {
    K_CFMT, K_LMAP, K_PRIM, K_PATCH, K_PRIM_RESTART, K_PROVOKING_LAST, K_DFMT, K_SFMT, K_SAMPLES,
    K_BLEND_TARGET, K_BLEND_SRC_RGB, K_BLEND_DST_RGB, K_BLEND_SRC_A, K_BLEND_DST_A, K_BLEND_EQ_RGB, K_BLEND_EQ_A,
    K_BLEND_ADV_MODE, K_BLEND_ADV_OVERLAP, K_BLEND_ADV_PREMUL, K_BLEND_ADV_NORMDST,
    K_COLOR_BLEND_EN, K_COLOR_LOGIC_OP, K_COLOR_ALPHA_FUNC, K_CHMASK, K_MS_A2C, K_MS_SL_GRID,
    K_ATTR_FMT, K_ATTR_OFFSET, K_ATTR_STREAM, K_STREAM_STRIDE, K_STREAM_DIVISOR_2, K_STREAM_DIVISOR_3,
    K_DS_DT, K_DS_DW, K_DS_DF, K_DS_ST, K_DS_SF_F, K_DS_SF_B, K_DS_FAIL_F, K_DS_ZFAIL_F, K_DS_ZPASS_F,
    K_DS_FAIL_B, K_DS_ZFAIL_B, K_DS_ZPASS_B,
    K_POLY_CULL, K_POLY_FRONT, K_POLY_MODE, K_POLY_OFFSET_EN, K_DISABLED_SAMPLES,
    K_RAST_DISCARD, K_CLIP_NEG1, K_ORIGIN_LL, K_DEPTH_CLAMP, K_SAMPLE_SHADING, K_DEPTH_BOUNDS
};

static void mutate(nvn_gpu_pipe_desc *d, int kind, int idx) {
    switch (kind) {
    case K_CFMT: d->cfmt[idx] += 1; break;
    case K_LMAP: d->lmap[idx] = d->lmap[idx] == 0xff ? 0 : (uint8_t)(d->lmap[idx] + 1); break;
    case K_PRIM: d->prim += 1; break;
    case K_PATCH: d->patch_size += 1; break;
    case K_PRIM_RESTART: d->prim_restart ^= 1; break;
    case K_PROVOKING_LAST: d->provoking_last ^= 1; break;
    case K_DFMT: d->dfmt += 1; break;
    case K_SFMT: d->sfmt += 1; break;
    case K_SAMPLES: d->samples += 1; break;
    case K_BLEND_TARGET: d->blend[idx].target += 1; break;
    case K_BLEND_SRC_RGB: d->blend[idx].src_rgb += 1; break;
    case K_BLEND_DST_RGB: d->blend[idx].dst_rgb += 1; break;
    case K_BLEND_SRC_A: d->blend[idx].src_a += 1; break;
    case K_BLEND_DST_A: d->blend[idx].dst_a += 1; break;
    case K_BLEND_EQ_RGB: d->blend[idx].eq_rgb += 1; break;
    case K_BLEND_EQ_A: d->blend[idx].eq_a += 1; break;
    case K_BLEND_ADV_MODE: d->blend[idx].adv_mode += 1; break;
    case K_BLEND_ADV_OVERLAP: d->blend[idx].adv_overlap += 1; break;
    case K_BLEND_ADV_PREMUL: d->blend[idx].adv_premul ^= 1; break;
    case K_BLEND_ADV_NORMDST: d->blend[idx].adv_normdst ^= 1; break;
    case K_COLOR_BLEND_EN: d->color.blend_en += 1; break;
    case K_COLOR_LOGIC_OP: d->color.logic_op += 1; break;
    case K_COLOR_ALPHA_FUNC: d->color.alpha_func += 1; break;
    case K_CHMASK: d->chmask.mask ^= 1u; break;
    case K_MS_A2C: d->ms.a2c ^= 1; break;
    case K_MS_SL_GRID: d->ms.sl_grid ^= 1; break;
    case K_ATTR_FMT: d->attr[idx].fmt += 1; break;
    case K_ATTR_OFFSET: d->attr[idx].offset += 1; break;
    case K_ATTR_STREAM: d->attr[idx].stream += 1; break;
    case K_STREAM_STRIDE: d->stream[idx].stride += 1; break;
    case K_STREAM_DIVISOR_2: d->stream[idx].divisor = 2; break;
    case K_STREAM_DIVISOR_3: d->stream[idx].divisor = 3; break;
    case K_DS_DT: d->ds.dt ^= 1; break;
    case K_DS_DW: d->ds.dw ^= 1; break;
    case K_DS_DF: d->ds.df += 1; break;
    case K_DS_ST: d->ds.st ^= 1; break;
    case K_DS_SF_F: d->ds.sf_f += 1; break;
    case K_DS_SF_B: d->ds.sf_b += 1; break;
    case K_DS_FAIL_F: d->ds.fail_f += 1; break;
    case K_DS_ZFAIL_F: d->ds.zfail_f += 1; break;
    case K_DS_ZPASS_F: d->ds.zpass_f += 1; break;
    case K_DS_FAIL_B: d->ds.fail_b += 1; break;
    case K_DS_ZFAIL_B: d->ds.zfail_b += 1; break;
    case K_DS_ZPASS_B: d->ds.zpass_b += 1; break;
    case K_POLY_CULL: d->poly.cull += 1; break;
    case K_POLY_FRONT: d->poly.front ^= 1; break;
    case K_POLY_MODE: d->poly.mode += 1; break;
    case K_POLY_OFFSET_EN: d->poly.offset_en += 1; break;
    case K_DISABLED_SAMPLES: d->disabled_samples = 0x8105; break;
    case K_RAST_DISCARD: d->rast_discard ^= 1; break;
    case K_CLIP_NEG1: d->clip_neg1 ^= 1; break;
    case K_ORIGIN_LL: d->origin_ll ^= 1; break;
    case K_SAMPLE_SHADING: d->sample_shading = NVN_SAMPLE_MODE_FORCE_ON; break;
    case K_DEPTH_BOUNDS: d->depth_bounds_enable ^= 1; break;
    case K_DEPTH_CLAMP: d->depth_clamp ^= 1; break;
    default: break;
    }
}

struct mut { const char *name; int kind, idx; };
static const struct mut k_muts[] = {
    {"cfmt0", K_CFMT, 0}, {"cfmt1", K_CFMT, 1}, {"cfmt2", K_CFMT, 2}, {"cfmt3", K_CFMT, 3},
    {"cfmt4", K_CFMT, 4}, {"cfmt5", K_CFMT, 5}, {"cfmt6", K_CFMT, 6}, {"cfmt7", K_CFMT, 7},
    {"lmap0", K_LMAP, 0}, {"lmap1", K_LMAP, 1}, {"lmap7", K_LMAP, 7},
    {"prim", K_PRIM, 0}, {"patch_size", K_PATCH, 0}, {"prim_restart", K_PRIM_RESTART, 0},
    {"provoking_last", K_PROVOKING_LAST, 0},
    {"dfmt", K_DFMT, 0}, {"sfmt", K_SFMT, 0}, {"samples", K_SAMPLES, 0},
    {"color.blend_en", K_COLOR_BLEND_EN, 0}, {"color.logic_op", K_COLOR_LOGIC_OP, 0},
    {"color.alpha_func", K_COLOR_ALPHA_FUNC, 0}, {"chmask", K_CHMASK, 0},
    {"ms.a2c", K_MS_A2C, 0}, {"ms.sl_grid", K_MS_SL_GRID, 0},
    {"ds.dt", K_DS_DT, 0}, {"ds.dw", K_DS_DW, 0}, {"ds.df", K_DS_DF, 0}, {"ds.st", K_DS_ST, 0},
    {"ds.sf_f", K_DS_SF_F, 0}, {"ds.sf_b", K_DS_SF_B, 0}, {"ds.fail_f", K_DS_FAIL_F, 0},
    {"ds.zfail_f", K_DS_ZFAIL_F, 0}, {"ds.zpass_f", K_DS_ZPASS_F, 0}, {"ds.fail_b", K_DS_FAIL_B, 0},
    {"ds.zfail_b", K_DS_ZFAIL_B, 0}, {"ds.zpass_b", K_DS_ZPASS_B, 0},
    {"poly.cull", K_POLY_CULL, 0}, {"poly.front", K_POLY_FRONT, 0}, {"poly.mode", K_POLY_MODE, 0},
    {"poly.offset_en", K_POLY_OFFSET_EN, 0},
    {"disabled_samples", K_DISABLED_SAMPLES, 0},
    {"rast_discard", K_RAST_DISCARD, 0}, {"clip_neg1", K_CLIP_NEG1, 0},
    {"origin_ll", K_ORIGIN_LL, 0}, {"depth_clamp", K_DEPTH_CLAMP, 0}, {"sample_shading", K_SAMPLE_SHADING, 0},
    {"depth_bounds_enable", K_DEPTH_BOUNDS, 0},
    // 数组：每个元素都扫（blend 的 11 个字段扫第 0、第 7 个元素；attr / stream 扫全部 16 个）
    {"blend0.target", K_BLEND_TARGET, 0}, {"blend0.src_rgb", K_BLEND_SRC_RGB, 0},
    {"blend0.dst_rgb", K_BLEND_DST_RGB, 0}, {"blend0.src_a", K_BLEND_SRC_A, 0},
    {"blend0.dst_a", K_BLEND_DST_A, 0}, {"blend0.eq_rgb", K_BLEND_EQ_RGB, 0}, {"blend0.eq_a", K_BLEND_EQ_A, 0},
    {"blend0.adv_mode", K_BLEND_ADV_MODE, 0}, {"blend0.adv_overlap", K_BLEND_ADV_OVERLAP, 0},
    {"blend0.adv_premul", K_BLEND_ADV_PREMUL, 0}, {"blend0.adv_normdst", K_BLEND_ADV_NORMDST, 0},
    {"blend7.target", K_BLEND_TARGET, 7}, {"blend7.src_rgb", K_BLEND_SRC_RGB, 7},
    {"blend7.dst_a", K_BLEND_DST_A, 7}, {"blend7.adv_mode", K_BLEND_ADV_MODE, 7},
    {"attr0.fmt", K_ATTR_FMT, 0}, {"attr0.offset", K_ATTR_OFFSET, 0}, {"attr0.stream", K_ATTR_STREAM, 0},
    {"attr15.fmt", K_ATTR_FMT, 15}, {"attr15.stream", K_ATTR_STREAM, 15},
    {"stream0.stride", K_STREAM_STRIDE, 0}, {"stream0.divisor2", K_STREAM_DIVISOR_2, 0},
    {"stream0.divisor3", K_STREAM_DIVISOR_3, 0},
    {"stream15.stride", K_STREAM_STRIDE, 15}, {"stream15.divisor2", K_STREAM_DIVISOR_2, 15},
};
#define NMUT ((int)(sizeof k_muts / sizeof k_muts[0]))
#define NFIELDS (NMUT + 1)

// 把反序列化得到的描述里的句柄清掉，好与期望逐字节比（记录里 sh 本来就不存）
static void strip_sh(nvn_gpu_pipe_desc *d) {
    for (int st = 0; st < 5; st++) d->st[st].sh = NULL;
}

// ---- 模式：逐字段往返 ----
static int mode_fields(void) {
    uint8_t id[32];
    ident_fill(id, 0x11);
    prog_setup(0x11);
    nvn_gpu_pipe_desc base;
    base_desc(&base);
    nvk_pipe_warm_note(&base, id);
    static nvn_gpu_pipe_desc exp[NFIELDS];
    exp[0] = base;
    strip_sh(&exp[0]);
    for (int i = 0; i < NMUT; i++) {
        exp[i + 1] = base;
        mutate(&exp[i + 1], k_muts[i].kind, k_muts[i].idx);
        strip_sh(&exp[i + 1]);
        nvn_gpu_pipe_desc send = base;
        mutate(&send, k_muts[i].kind, k_muts[i].idx);
        nvk_pipe_warm_note(&send, id);
    }
    // 本局预热集只来自载入：先落盘再载入，take 才是「这一局要预热的那批」（运行期 note 只影响下一局清单）。
    nvk_pipe_warm_save();
    CHECK(nvk_pipe_warm_load() == NFIELDS, "fields: save+load must bring back %d recipes", NFIELDS);
    int pend = nvk_pipe_warm_pending();
    CHECK(pend == NFIELDS, "expected %d pending recipes, got %d", NFIELDS, pend);

    int seen[NFIELDS];
    memset(seen, 0, sizeof seen);
    int ntake = 0;
    struct nvk_pipe_rec rec;
    while (nvk_pipe_warm_take(&rec)) {
        int rc = nvk_pipe_warm_build(&rec);
        CHECK(rc == 0, "build of a recorded recipe must succeed, rc=%d", rc);
        nvn_gpu_pipe_desc got = g_last_desc;
        strip_sh(&got);
        int matched = -1;
        for (int i = 0; i < NFIELDS && matched < 0; i++)
            if (memcmp(&exp[i], &got, sizeof got) == 0) matched = i;
        CHECK(matched >= 0, "recipe #%d did not round-trip to any expected desc (bytes differ)", ntake);
        if (matched >= 0) seen[matched]++;
        ntake++;
    }
    CHECK(ntake == NFIELDS, "took %d recipes, expected %d", ntake, NFIELDS);
    for (int i = 0; i < NFIELDS; i++) {
        if (seen[i] != 1) {
            CHECK(0, "expected desc %s matched %d times (want exactly 1)", i == 0 ? "base" : k_muts[i - 1].name, seen[i]);
        }
    }
    return g_fail;
}

// ---- 模式：同配方去重 / uses / 跳过条件 ----
static int mode_dedup(void) {
    uint8_t id[32], zero[32];
    ident_fill(id, 0x22);
    memset(zero, 0, sizeof zero);
    prog_setup(0x22);
    nvn_gpu_pipe_desc pd;
    base_desc(&pd);
    for (int i = 0; i < 3; i++) nvk_pipe_warm_note(&pd, id);
    nvk_pipe_warm_note(&pd, zero);            // 没有身份：不记
    nvn_gpu_pipe_desc spec = pd;
    spec.st[0].nspec = 1;                     // 特化常量：不记
    nvk_pipe_warm_note(&spec, id);
    struct nvk_pipe_warm_stats st;
    nvk_pipe_warm_stats(&st);
    CHECK(st.recorded == 1, "three notes of one recipe must record 1 entry, got %llu", (unsigned long long)st.recorded);
    CHECK(st.rejected >= 1, "a recipe with nspec != 0 must be rejected, got %llu", (unsigned long long)st.rejected);
    nvk_pipe_warm_save();
    CHECK(nvk_pipe_warm_load() == 1, "dedup: save+load must bring back the one recipe");
    CHECK(nvk_pipe_warm_pending() == 1, "pending must be 1, got %d", nvk_pipe_warm_pending());
    struct nvk_pipe_rec rec;
    CHECK(nvk_pipe_warm_take(&rec) == 1, "take must return the only entry");
    CHECK(rec.uses == 3, "uses must count every real bind (3), got %u", rec.uses);
    // 再 note 一次：命中去重、uses 继续加
    nvk_pipe_warm_note(&pd, id);
    nvk_pipe_warm_stats(&st);
    CHECK(st.recorded == 1, "repeat note must not add an entry, got %llu", (unsigned long long)st.recorded);
    CHECK(st.draw_hits == g_fake_hits, "draw_hits must forward the backend counter, got %llu", (unsigned long long)st.draw_hits);
    return g_fail;
}

// 计算管线使用同一清单的去重、跨会话排序及借用合同；重建时必须走计算入口。
static int mode_compute(void) {
    setenv("S2A_COMPUTE_WARM", "1", 1);
    prog_setup(0x31); g_prog.is_compute = 1; g_prog.sh[1] = NULL;
    nvn_gpu_pipe_desc d; memset(&d, 0, sizeof d); d.st[0].sh = g_prog.sh[0];
    CHECK(nvk_pipe_warm_note_prog(&d, &g_prog, NULL), "compute first note");
    CHECK(nvk_pipe_warm_note_prog(&d, &g_prog, NULL), "compute second note");
    nvk_pipe_warm_save();
    CHECK(nvk_pipe_warm_load() == 1, "compute dedup and persist one recipe");
    struct nvk_pipe_rec rec; memset(&rec, 0, sizeof rec);
    CHECK(nvk_pipe_warm_take(&rec) == 1 && rec.uses == 2, "compute take preserves uses");
    CHECK(nvk_pipe_warm_build(&rec) == 0, "compute prewarm succeeds");
    CHECK(g_compute_pipe_calls == 1 && g_compute_pipe_shader == g_prog.sh[0] && g_pipe_get_calls == 0,
          "compute recipe must build a compute pipeline with its borrowed CS");
    g_prog.is_compute = 0;
    CHECK(nvk_pipe_warm_build(&rec) == -1 && g_compute_pipe_calls == 1, "compute recipe rejects a graphics program");
    g_prog.is_compute = 1;
    rec.desc[1] = 1;
    CHECK(nvk_pipe_warm_build(&rec) == -1 && g_compute_pipe_calls == 1, "compute recipe rejects malformed bytes");
    CHECK(g_borrow_n == 0, "compute borrow returned");
    return g_fail;
}

static int mode_compute_off(void) {
    setenv("S2A_COMPUTE_WARM", "0", 1);
    CHECK(nvk_pipe_warm_load() == 1, "compute off keeps existing manifest");
    CHECK(nvk_pipe_warm_pending() == 0, "compute off must not schedule saved compute pipe");
    return g_fail;
}

// ---- 模式：散列相同、配方不同必须留两条 ----
// 64 位散列构造不出真碰撞，用模块的用例钩子把散列固定成常数：这时「散列相同的两条不同配方」
// 就是每个配方都同散列。判据若只比散列，B 会被并进 A（pending 1）；判据是整份字节，则两条都在。
static int mode_collide(void) {
    uint8_t id[32];
    ident_fill(id, 0x33);
    prog_setup(0x33);
    nvn_gpu_pipe_desc da, db;
    base_desc(&da);
    db = da;
    db.depth_clamp ^= 1;   // 只改一个语义字节：配方不同
    nvk_pipe_warm_test_hash_const(1);
    struct nvk_pipe_rec ra, rb;
    nvk_pipe_warm_rec_of(&da, id, &ra);
    nvk_pipe_warm_rec_of(&db, id, &rb);
    CHECK(nvk_pipe_warm_hash(&ra) == nvk_pipe_warm_hash(&rb), "hash-const hook must make both hashes equal");
    CHECK(memcmp(ra.desc, rb.desc, NVK_PW_DESC_BYTES) != 0, "the two recipes must differ in bytes");
    nvk_pipe_warm_note(&da, id);
    nvk_pipe_warm_note(&db, id);
    nvk_pipe_warm_save();
    CHECK(nvk_pipe_warm_load() == 2, "collide: save+load must bring back both recipes");
    CHECK(nvk_pipe_warm_pending() == 2,
          "same hash + different bytes must NOT dedup (pending %d, want 2)", nvk_pipe_warm_pending());
    nvk_pipe_warm_note(&da, id);   // 同散列同字节：必须去重、只加 uses
    struct nvk_pipe_warm_stats st;
    nvk_pipe_warm_stats(&st);
    CHECK(st.recorded == 2, "identical recipe must dedup by bytes (recorded %llu)", (unsigned long long)st.recorded);
    nvk_pipe_warm_test_hash_const(0);
    return g_fail;
}

// ---- 模式：上限与淘汰 ----
static int mode_cap(void) {
    uint8_t id[32];
    ident_fill(id, 0x44);
    prog_setup(0x44);
    nvn_gpu_pipe_desc hot, others[5];
    base_desc(&hot);
    hot.prim = 5;                              // 让 hot 与 others 都不同
    for (int i = 0; i < 5; i++) {
        base_desc(&others[i]);
        others[i].prim = 6 + (uint32_t)i;
        for (int k = 0; k <= i + 1; k++) nvk_pipe_warm_note(&others[i], id);   // uses = i+2
    }
    for (int k = 0; k < 20; k++) nvk_pipe_warm_note(&hot, id);                 // uses = 20，必须活下来
    nvk_pipe_warm_save();
    CHECK(nvk_pipe_warm_load() == 4, "cap: save+load must bring back exactly the capped set");
    int pend = nvk_pipe_warm_pending();
    CHECK(pend == 4, "cap=4 must keep 4 entries after 6 distinct recipes, got %d", pend);
    int found_hot = 0, found_cold = 0;
    struct nvk_pipe_rec rec;
    while (nvk_pipe_warm_take(&rec)) {
        CHECK(nvk_pipe_warm_build(&rec) == 0, "build in cap mode");
        strip_sh(&g_last_desc);
        nvn_gpu_pipe_desc h2 = hot;
        strip_sh(&h2);
        if (memcmp(&h2, &g_last_desc, sizeof h2) == 0) found_hot = 1;
        nvn_gpu_pipe_desc c2 = others[0];
        strip_sh(&c2);
        if (memcmp(&c2, &g_last_desc, sizeof c2) == 0) found_cold = 1;
    }
    CHECK(found_hot == 1, "the hottest recipe (uses 20) must survive the cap");
    CHECK(found_cold == 0, "the coldest recipe (uses 2, oldest) must be evicted");
    return g_fail;
}

// ---- 模式：缺省上限（#1325）----
// 没设 nvk_warm_max 时清单留 8192 条（= 硬顶）；设成比硬顶还大的数（hardcap 模式，S2A_PW_MAX=100000）也只留 8192。
// 期望手算：热 100 条各记 3 次（uses 3），冷 8900 条各记 1 次（uses 1）。前 8192 条进表后表满（100 热 + 冷序号 100..8191），
// 之后每进一条冷的就挤掉冷里最老的一条：8192..8999 共 808 条进来，挤掉冷序号 100..907；
// 留下 100 热 + 冷序号 908..8999（8092 条）= 8192 条。序号藏在第 2 条流的 stride 里（每个序号一份不同的配方）。
// 变异体：缺省回到 4096 → 读回 4096 条，红。
static int mode_defcap_impl(void) {
    enum { HOT = 100, TOTAL = 9000, KEEP = 8192, BASE = 1000 };
    uint8_t id[32];
    ident_fill(id, 0x66);
    prog_setup(0x66);
    nvn_gpu_pipe_desc d;
    base_desc(&d);
    for (uint32_t i = 0; i < TOTAL; i++) {
        d.stream[1].stride = BASE + i;
        for (int k = 0; k < (i < HOT ? 3 : 1); k++) nvk_pipe_warm_note(&d, id);
    }
    nvk_pipe_warm_save();
    CHECK(nvk_pipe_warm_load() == KEEP, "default cap: save+load must bring back exactly %d recipes", KEEP);
    CHECK(nvk_pipe_warm_pending() == KEEP, "default cap: %d recipes pending", KEEP);
    static unsigned char seen[TOTAL];
    memset(seen, 0, sizeof seen);
    int dup = 0, bad = 0, taken = 0, hot_first = 1;
    struct nvk_pipe_rec rec;
    while (nvk_pipe_warm_take(&rec)) {
        if (nvk_pipe_warm_build(&rec) != 0) { bad++; continue; }
        uint32_t idx = g_last_desc.stream[1].stride - BASE;
        if (idx >= TOTAL) { bad++; continue; }
        if (seen[idx]) dup++;
        seen[idx] = 1;
        if (taken < HOT && idx >= HOT) hot_first = 0;   // uses 降序：前 100 条必须是热的
        taken++;
    }
    CHECK(bad == 0 && dup == 0 && taken == KEEP, "default cap: %d distinct recipes handed out (bad %d, dup %d, taken %d)", KEEP, bad, dup, taken);
    int hot_ok = 1, evicted_ok = 1, kept_ok = 1;
    for (uint32_t i = 0; i < HOT; i++) if (!seen[i]) hot_ok = 0;
    for (uint32_t i = HOT; i <= 907; i++) if (seen[i]) evicted_ok = 0;
    for (uint32_t i = 908; i < TOTAL; i++) if (!seen[i]) kept_ok = 0;
    CHECK(hot_ok, "default cap: all 100 hot recipes (uses 3) survive");
    CHECK(evicted_ok, "default cap: the 808 oldest cold recipes (100..907) are the ones evicted");
    CHECK(kept_ok, "default cap: the newest 8092 cold recipes (908..8999) are kept");
    CHECK(hot_first, "default cap: hot recipes are handed out first (uses desc)");
    return g_fail;
}
static int mode_defcap(void) { unsetenv("S2A_PW_MAX"); return mode_defcap_impl(); }
static int mode_hardcap(void) { setenv("S2A_PW_MAX", "100000", 1); return mode_defcap_impl(); }

// ---- 模式：落盘 / 读回 ----
static int file_path(char *out, size_t n) {
    const char *dir = getenv("S2A_PW_DIR");
    if (!dir || !*dir) return -1;
    snprintf(out, n, "%s/pipes-v1-%d.bin", dir, NVK_PW_DESC_BYTES);
    return 0;
}

static int mode_save(void) {
    uint8_t id[32];
    ident_fill(id, 0x55);
    prog_setup(0x55);
    nvn_gpu_pipe_desc d[3];
    for (int i = 0; i < 3; i++) {
        base_desc(&d[i]);
        d[i].prim = 1 + (uint32_t)i;
        for (int k = 0; k <= i * 2; k++) nvk_pipe_warm_note(&d[i], id);   // uses = 1 / 3 / 5
    }
    nvk_pipe_warm_save();
    char path[600];
    if (file_path(path, sizeof path)) return 2;
    FILE *f = fopen(path, "rb");
    if (!f) { CHECK(0, "save must create the manifest file"); return g_fail; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    CHECK(sz == 32 + 3 * (32 + NVK_PW_DESC_BYTES + 4), "file size %ld for 3 entries", sz);
    return g_fail;
}

static int mode_load(void) {
    int n = nvk_pipe_warm_load();
    CHECK(n == 3, "load must read 3 entries, got %d", n);
    prog_setup(0x55);
    uint32_t want_uses[3] = {5, 3, 1};
    for (int i = 0; i < 3; i++) {
        struct nvk_pipe_rec rec;
        CHECK(nvk_pipe_warm_take(&rec) == 1, "take #%d after load", i);
        CHECK(rec.uses == want_uses[i], "entry #%d uses %u, want %u (hot first)", i, rec.uses, want_uses[i]);
        CHECK(nvk_pipe_warm_build(&rec) == 0, "build #%d after load", i);
        CHECK(g_last_desc.prim == 3 - (uint32_t)i, "entry #%d prim %u", i, g_last_desc.prim);
    }
    struct nvk_pipe_rec extra;
    CHECK(nvk_pipe_warm_take(&extra) == 0, "take must end after 3");
    return g_fail;
}

// ---- 模式：坏文件整份拒绝 ----
static int mode_badfile(const char *variant) {
    char path[600];
    if (file_path(path, sizeof path)) return 2;
    FILE *f = fopen(path, "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { if (buf) free(buf); fclose(f); return 2; }
    fclose(f);
    long keep = sz;
    const char *v = variant;
    if (!strcmp(v, "magic")) buf[0] ^= 0xff;
    else if (!strcmp(v, "fmt")) buf[8] ^= 0xff;
    else if (!strcmp(v, "descver")) buf[12] ^= 0xff;
    else if (!strcmp(v, "entry")) buf[16] ^= 0xff;
    else if (!strcmp(v, "count")) buf[20] ^= 0xff;
    else if (!strcmp(v, "sum")) buf[24] ^= 0xff;
    else if (!strcmp(v, "trunc")) keep = sz > 20 ? 20 : 1;
    else if (!strcmp(v, "payload")) buf[sz - 1] ^= 0xff;
    else return 2;
    f = fopen(path, "wb");
    if (!f) { free(buf); return 2; }
    fwrite(buf, 1, (size_t)keep, f);
    fclose(f);
    free(buf);
    int n = nvk_pipe_warm_load();
    CHECK(n == 0, "corrupt manifest (%s) must be rejected wholesale, load returned %d", v, n);
    struct nvk_pipe_warm_stats st;
    nvk_pipe_warm_stats(&st);
    CHECK(st.rejected >= 1, "corrupt manifest (%s) must count rejected entries", v);
    CHECK(nvk_pipe_warm_pending() == 0, "corrupt manifest must yield no work");
    return g_fail;
}

// ---- 父进程：子进程跑各模式 ----
// ---- 模式：程序借用（外部复核 P1「管线预热缺少程序寿命保护」的消费侧）----
// 建管线必须持借用、且无论成功失败都要归还；借用被拒（会话已停 / 借用表满）时如实失败，不假装建过。
static int mode_lease(void) {
    uint8_t id[32];
    ident_fill(id, 0x77);
    prog_setup(0x77);
    nvn_gpu_pipe_desc pd;
    base_desc(&pd);
    nvk_pipe_warm_note(&pd, id);
    nvk_pipe_warm_save();
    CHECK(nvk_pipe_warm_load() == 1, "lease: one recipe must load");
    struct nvk_pipe_rec rec;
    CHECK(nvk_pipe_warm_take(&rec) == 1, "lease: take the recipe");
    struct nvk_pipe_warm_stats st0, st1;
    nvk_pipe_warm_stats(&st0);
    g_borrow_fail = 1;
    int rc = nvk_pipe_warm_build(&rec);
    g_borrow_fail = 0;
    nvk_pipe_warm_stats(&st1);
    CHECK(rc == -1, "lease: a refused borrow must fail the build (-1), got %d", rc);
    CHECK(st1.failed == st0.failed + 1, "lease: refused borrow counts as failed, not retried");
    CHECK(rp_shader_prog_borrowed() == 0, "lease: no borrow leaked on the failure path");
    rc = nvk_pipe_warm_build(&rec);
    CHECK(rc == 0, "lease: build with a borrow succeeds, got %d", rc);
    CHECK(rp_shader_prog_borrowed() == 0, "lease: the borrow is returned after the build (no leak)");
    return g_fail;
}

// ---- 模式：并发保存（外部复核 P2「管线清单并发保存会损坏文件」的等价夹具）----
// 4 个线程各自 note + save，然后在同一进程里重新载入：保存必须被串行化，最后发布的文件必须完整可读，
// 且取出的是那条唯一集合（不出现重复、不出现半截条目）。负对照：去掉保存互斥 + 让写盘变慢 → 载入返回 0。
static void *save_thread(void *arg) {
    long k = (long)(intptr_t)arg;
    uint8_t id[32];
    ident_fill(id, (int)(0x70 + k));
    prog_setup((unsigned)(0x70 + k));
    for (int i = 0; i < 40; i++) {
        nvn_gpu_pipe_desc d;
        base_desc(&d);
        d.stream[0].stride = (uint32_t)(100 + i * 7 + k * 1000);
        nvk_pipe_warm_note(&d, id);
        nvk_pipe_warm_save();
    }
    return NULL;
}
static int mode_save_race(void) {
    enum { NTH = 4 };
    pthread_t th[NTH];
    for (long i = 0; i < NTH; i++) pthread_create(&th[i], NULL, save_thread, (void *)(intptr_t)i);
    for (int i = 0; i < NTH; i++) pthread_join(th[i], NULL);
    int n = nvk_pipe_warm_load();
    CHECK(n == 160, "save race: a fresh load of the last published snapshot must return all 160 recipes, got %d", n);
    int seen = 0, dup = 0;
    struct nvk_pipe_rec got, seenr[160];
    while (nvk_pipe_warm_take(&got)) {
        for (int i = 0; i < seen; i++) if (!memcmp(seenr[i].desc, got.desc, NVK_PW_DESC_BYTES) && !memcmp(seenr[i].prog_identity, got.prog_identity, 32)) dup = 1;
        if (seen < 160) seenr[seen] = got;
        seen++;
    }
    CHECK(seen == 160 && !dup, "save race: the published set must be exactly 160 unique recipes (seen=%d dup=%d)", seen, dup);
    return g_fail;
}

// ---- 模式：本局预热集冻结（外部复核 P1「冻结了数量，身份集合仍会变化」的 repro）----
// 载入之后 take 的顺序与集合必须就是文件里那份；运行期的 note、热点重排（order 重建）、淘汰、周期 save
// 都不许影响它——否则同一配方被取两次、另一个配方永远漏掉（复核复现的 A→A）。
static int mode_frozen(void) {
    setenv("S2A_TEST_WARM_REACTIVE", "1", 1);
    uint8_t id[32];
    ident_fill(id, 0x66);
    prog_setup(0x66);
    nvn_gpu_pipe_desc a, b, c, d;
    base_desc(&a); b = a; b.prim = 7; c = a; c.prim = 8; d = a; d.prim = 9;
    for (int i = 0; i < 10; i++) nvk_pipe_warm_note(&a, id);   // uses=10，最热
    for (int i = 0; i < 5; i++) nvk_pipe_warm_note(&b, id);    // uses=5
    nvk_pipe_warm_note(&c, id);                                // uses=1
    nvk_pipe_warm_save();
    CHECK(nvk_pipe_warm_load() == 3, "frozen: three recipes load");
    struct nvk_pipe_rec got, ar, br, cr;
    nvk_pipe_warm_rec_of(&a, id, &ar); nvk_pipe_warm_rec_of(&b, id, &br); nvk_pipe_warm_rec_of(&c, id, &cr);
    uint8_t other[32];
    memcpy(other, id, sizeof other);
    other[31] ^= 1;
    CHECK(!nvk_pipe_warm_take_program(other, &got) && nvk_pipe_warm_pending() == 3,
          "frozen: same bucket with different full identity cannot take a recipe");
    CHECK(nvk_pipe_warm_take_program(id, &got) && !memcmp(got.desc, ar.desc, NVK_PW_DESC_BYTES) &&
          nvk_pipe_warm_pending() == 2, "frozen: published program takes its hottest recipe exactly once");
    // 运行期搅动：B 变最热、插入新配方 D、立刻落盘（会重排 order）
    for (int i = 0; i < 20; i++) nvk_pipe_warm_note(&b, id);
    nvk_pipe_warm_note(&d, id);
    nvk_pipe_warm_save();
    CHECK(nvk_pipe_warm_take(&got) == 1 && !memcmp(got.desc, br.desc, NVK_PW_DESC_BYTES),
          "frozen: second take must still be B from the loaded plan (not A again, not D)");
    {   // 集合唯一性：剩下必须恰好是 C 一条，且全程没有重复
        int ntake = 0, dup = 0;
        struct nvk_pipe_rec seen[4];
        int ns = 0;
        while (nvk_pipe_warm_take(&got)) {
            for (int i = 0; i < ns; i++) if (!memcmp(seen[i].desc, got.desc, NVK_PW_DESC_BYTES)) dup = 1;
            if (ns < 4) seen[ns++] = got;
            ntake++;
        }
        CHECK(ntake == 1 && !dup && !memcmp(seen[0].desc, cr.desc, NVK_PW_DESC_BYTES),
              "frozen: exactly C left, no duplicates (ntake=%d dup=%d)", ntake, dup);
    }
    return g_fail;
}

// ---- 模式：管线标签路径与完整路径出同一份清单 ----
// 同一串「换管线」序列：一遍走完整路径（nvk_pipe_warm_note，改前的行为），一遍走带管线标签的 nvk_pipe_warm_note_prog；
// 两遍各在自己的进程里跑（模块是进程单例）、各自落盘，父进程逐字节比两份清单文件（条数、各条 uses、顺序都在文件里）。
// 带标签的一遍另外核对：标签命中次数恰是模型算出来的次数——说明快路径真走了，而不是每次都退回完整路径。
// variant 0 = 容量够（不淘汰）；variant 1 = 上限 6 条、20 种配方：槽不断被淘汰重用，旧标签只能靠槽代号作废。
enum { TAG_STEPS = 6000, TAG_NP_BASE = 0x7000 };
static nvn_gpu_pipe tag_np(int r) { return (nvn_gpu_pipe)(uintptr_t)(TAG_NP_BASE + 16 * r); }
static void tag_recipe(nvn_gpu_pipe_desc *d, int r) {   // 配方 r：prim / stride / 颜色格式各不同
    base_desc(d);
    d->prim = 1u + (uint32_t)(r % 7);
    d->stream[0].stride = 16u + 4u * (uint32_t)(r / 7);
    d->cfmt[0] = 0x23u + (uint32_t)(r % 3);
}

// variant 2 = 上限 3 条的手排序列：淘汰先看 uses、同 uses 看最近序号，所以标签快路径刷新最近序号这件事
// 只有「uses 相同、最近序号不同」的局面下才看得出来。0/1/2 先各入一次，再按 2、1、0 的顺序各命中一次
// （三条 uses 都是 2，最近序号 2 最旧）；这时进来第四份配方 3，被淘汰的必须是 2。快路径若不刷新最近序号，
// 被淘汰的会变成 0，清单文件就和完整路径的不一样。
static void tag_note(int use_tag, int r) {
    nvn_gpu_pipe_desc d;
    tag_recipe(&d, r);
    prog_setup((uint8_t)(0x30 + (r & 1)));
    if (use_tag) CHECK(nvk_pipe_warm_note_prog(&d, &g_prog, tag_np(r)) == 1, "tag: note_prog must accept recipe %d", r);
    else nvk_pipe_warm_note(&d, g_prog.identity);
}
static int mode_tag_recency(int use_tag) {
    static const int seq[] = {0, 1, 2, 2, 1, 0, 3, 3, 2, 2, 0, 1, 1, 3, 0};
    for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) tag_note(use_tag, seq[i]);
    nvk_pipe_warm_save();
    struct nvk_pipe_warm_stats st;
    nvk_pipe_warm_stats(&st);
    if (use_tag) CHECK(st.tag_hits >= 3, "tag: the hand-written sequence must use the tag path (tag_hits=%llu)", (unsigned long long)st.tag_hits);
    else CHECK(st.tag_hits == 0, "tag: the plain path must never use a tag, tag_hits=%llu", (unsigned long long)st.tag_hits);
    return g_fail;
}

static int mode_tag(int use_tag, int variant) {
    if (variant == 2) return mode_tag_recency(use_tag);
    const int nrec = variant ? 20 : 24;
    uint32_t x = 20260116u;
    int has_tag[24] = {0};          // 模型：这条 np 现在有没有有效标签（只在不淘汰的 variant 0 里用）
    uint64_t model_hits = 0;
    int seen_recipe[24] = {0}, distinct = 0;
    for (int step = 0; step < TAG_STEPS; step++) {
        x = x * 1103515245u + 12345u;
        const uint32_t k = (x >> 16) & 0xffu;
        const int r = k < 128 ? (int)(k % 4u) : (int)(k % (uint32_t)nrec);   // 4 个热配方 + 其余偶尔换到
        if (step % 311 == 310) {                                           // 偶尔一条 np 换新（标签清零）
            const int c = (step / 311) % nrec;
            fake_np_forget(tag_np(c));
            has_tag[c] = 0;
        }
        nvn_gpu_pipe_desc d;
        tag_recipe(&d, r);
        prog_setup((uint8_t)(0x30 + (r & 1)));                              // 奇偶配方用两个不同的程序身份
        if (!seen_recipe[r]) { seen_recipe[r] = 1; distinct++; }
        if (use_tag) {
            CHECK(nvk_pipe_warm_note_prog(&d, &g_prog, tag_np(r)) == 1, "tag: note_prog must accept the recipe");
            if (variant == 0) { if (has_tag[r]) model_hits++; has_tag[r] = 1; }
        } else {
            nvk_pipe_warm_note(&d, g_prog.identity);
        }
    }
    nvk_pipe_warm_save();
    struct nvk_pipe_warm_stats st;
    nvk_pipe_warm_stats(&st);
    if (use_tag) {
        if (variant == 0) {
            CHECK(st.tag_hits == model_hits, "tag: tag_hits %llu, the model says %llu", (unsigned long long)st.tag_hits, (unsigned long long)model_hits);
            CHECK(st.recorded == (uint64_t)distinct, "tag: recorded %llu, distinct recipes %d", (unsigned long long)st.recorded, distinct);
        } else {
            CHECK(st.tag_hits > 0 && st.tag_hits < TAG_STEPS, "tag: some but not all steps hit the tag under eviction (hits=%llu)", (unsigned long long)st.tag_hits);
        }
    } else {
        CHECK(st.tag_hits == 0, "tag: the plain path must never use a tag, tag_hits=%llu", (unsigned long long)st.tag_hits);
    }
    return g_fail;
}

static int read_all(const char *path, unsigned char **out, long *n) {
    *out = NULL; *n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)(sz > 0 ? sz : 1));
    if (!b || (sz > 0 && fread(b, 1, (size_t)sz, f) != (size_t)sz)) { free(b); fclose(f); return -1; }
    fclose(f);
    *out = b; *n = sz;
    return 0;
}

// ---- 模式：重载之后旧标签必须作废 ----
// 开局的第一次 load 会把槽里的内容整份换成文件里的那份（同一进程里先 note、后 load）：
//   槽 0（文件里有）：槽号原样对得上，只有槽代号能认出「这已经不是当初那一份了」；
//   槽 1（文件里没有）：载入后槽空着，槽代号没动，只有「槽还占着」这一条能拦住旧标签——
//     放过去的话这份配方就从清单里丢了（旧路径会把它重新记进来）。
// 落盘的 uses 也要对：配方 0 = 3（载入）+ 1（完整路径）+ 1（标签）= 5；配方 1 = 1（重新记入）+ 1（标签）= 2。
static int mode_tag_reload(void) {
    nvn_gpu_pipe_desc a, b;
    tag_recipe(&a, 0);
    tag_recipe(&b, 1);
    prog_setup(0x30);
    const nvn_gpu_pipe np0 = tag_np(0), np1 = tag_np(1);
    struct nvk_pipe_warm_stats st;
    for (int i = 0; i < 3; i++) CHECK(nvk_pipe_warm_note_prog(&a, &g_prog, np0) == 1, "reload: note a %d", i);
    nvk_pipe_warm_save();                                                    // 文件里只有配方 0（uses 3）
    for (int i = 0; i < 2; i++) CHECK(nvk_pipe_warm_note_prog(&b, &g_prog, np1) == 1, "reload: note b %d", i);
    nvk_pipe_warm_stats(&st);
    CHECK(st.recorded == 2 && st.tag_hits == 3, "reload: 2 records and 3 tag hits before the load (recorded=%llu hits=%llu)", (unsigned long long)st.recorded, (unsigned long long)st.tag_hits);
    CHECK(nvk_pipe_warm_load() == 1, "reload: the one recipe in the file is loaded back");
    const uint64_t old_tag0 = nvn_gpu_pipe_warm_tag(np0), old_tag1 = nvn_gpu_pipe_warm_tag(np1);
    CHECK(nvk_pipe_warm_note_prog(&a, &g_prog, np0) == 1, "reload: note a after the load");
    nvk_pipe_warm_stats(&st);
    CHECK(st.tag_hits == 3, "reload: a tag whose slot was refilled must not hit (tag_hits=%llu, want 3)", (unsigned long long)st.tag_hits);
    CHECK(nvn_gpu_pipe_warm_tag(np0) != old_tag0, "reload: the full path must write a new tag back (a)");
    CHECK(nvk_pipe_warm_note_prog(&a, &g_prog, np0) == 1, "reload: note a again");
    nvk_pipe_warm_stats(&st);
    CHECK(st.tag_hits == 4, "reload: the rewritten tag hits (tag_hits=%llu, want 4)", (unsigned long long)st.tag_hits);
    CHECK(nvk_pipe_warm_note_prog(&b, &g_prog, np1) == 1, "reload: note b after the load");
    nvk_pipe_warm_stats(&st);
    CHECK(st.tag_hits == 4 && st.recorded == 3, "reload: a tag whose slot is empty must not hit, the recipe is recorded again (hits=%llu recorded=%llu, want 4/3)", (unsigned long long)st.tag_hits, (unsigned long long)st.recorded);
    CHECK(nvn_gpu_pipe_warm_tag(np1) != old_tag1, "reload: the full path must write a new tag back (b)");
    CHECK(nvk_pipe_warm_note_prog(&b, &g_prog, np1) == 1, "reload: note b again");
    nvk_pipe_warm_stats(&st);
    CHECK(st.tag_hits == 5, "reload: the rewritten tag of b hits (tag_hits=%llu, want 5)", (unsigned long long)st.tag_hits);
    nvk_pipe_warm_save();
    char path[600];
    if (file_path(path, sizeof path)) return 2;
    unsigned char *buf; long n;
    const long rec = 32 + NVK_PW_DESC_BYTES + 4;
    if (read_all(path, &buf, &n) != 0 || n != 32 + 2 * rec) { CHECK(0, "reload: manifest file missing or wrong size (%ld)", n); free(buf); return g_fail ? g_fail : 1; }
    for (int k = 0; k < 2; k++) {   // 按 uses 降序：配方 0（5）在前，配方 1（2）在后
        const unsigned char *u = buf + 32 + k * rec + 32 + NVK_PW_DESC_BYTES;
        const uint32_t uses = (uint32_t)u[0] | (uint32_t)u[1] << 8 | (uint32_t)u[2] << 16 | (uint32_t)u[3] << 24;
        CHECK(uses == (k == 0 ? 5u : 2u), "reload: persisted uses of record %d is %u, want %u", k, uses, k == 0 ? 5u : 2u);
    }
    free(buf);
    return g_fail;
}

// ---- 模式：同一条 np 换了程序身份，必须走完整路径 ----
// 标签快路径的前提是「一局内 np 与（配方 + 程序身份）一一对应」，今天靠后端缓存键里的着色器句柄指针保证。
// 将来如果有人让句柄按内容合并，同一条 np 就可能带着另一份程序身份回来：这时标签不能算命中，
// 必须按新身份走完整路径入清单；否则 uses 会记到别的程序头上，落盘的预热清单就错了。
static int mode_tag_prog(void) {
    nvn_gpu_pipe_desc a;
    tag_recipe(&a, 0);
    const nvn_gpu_pipe np = tag_np(0);
    struct nvk_pipe_warm_stats st;
    prog_setup(0x30);                                    // 程序 A
    CHECK(nvk_pipe_warm_note_prog(&a, &g_prog, np) == 1, "prog: A first");
    CHECK(nvk_pipe_warm_note_prog(&a, &g_prog, np) == 1, "prog: A again");
    nvk_pipe_warm_stats(&st);
    CHECK(st.recorded == 1 && st.tag_hits == 1, "prog: A = 1 record + 1 tag hit (recorded=%llu hits=%llu)", (unsigned long long)st.recorded, (unsigned long long)st.tag_hits);
    const uint64_t tag_a = nvn_gpu_pipe_warm_tag(np);
    prog_setup(0x31);                                    // 同一条 np，程序 B（身份不同）
    CHECK(nvk_pipe_warm_note_prog(&a, &g_prog, np) == 1, "prog: B on the same np");
    nvk_pipe_warm_stats(&st);
    CHECK(st.tag_hits == 1, "prog: the tag of A must not hit for B (tag_hits=%llu, want 1)", (unsigned long long)st.tag_hits);
    CHECK(st.recorded == 2, "prog: B must be recorded through the full path (recorded=%llu, want 2)", (unsigned long long)st.recorded);
    CHECK(nvn_gpu_pipe_warm_tag(np) != tag_a, "prog: the full path must write B's tag back");
    CHECK(nvk_pipe_warm_note_prog(&a, &g_prog, np) == 1, "prog: B again");
    nvk_pipe_warm_stats(&st);
    CHECK(st.tag_hits == 2, "prog: B's tag hits (tag_hits=%llu, want 2)", (unsigned long long)st.tag_hits);
    prog_setup(0x30);                                    // 回到 A：B 的标签同样不能算 A 的命中
    CHECK(nvk_pipe_warm_note_prog(&a, &g_prog, np) == 1, "prog: back to A");
    nvk_pipe_warm_stats(&st);
    CHECK(st.tag_hits == 2 && st.recorded == 2, "prog: back to A goes the full path and merges into A's entry (hits=%llu recorded=%llu, want 2/2)", (unsigned long long)st.tag_hits, (unsigned long long)st.recorded);
    nvk_pipe_warm_save();
    char path[600];
    if (file_path(path, sizeof path)) return 2;
    unsigned char *buf; long n;
    if (read_all(path, &buf, &n) != 0 || n != 32 + 2 * (32 + NVK_PW_DESC_BYTES + 4)) { CHECK(0, "prog: manifest file missing or wrong size (%ld)", n); free(buf); return g_fail ? g_fail : 1; }
    // 清单按 uses 降序：A = 1（完整）+ 1（标签）+ 1（回头走完整路径）= 3，B = 1 + 1 = 2
    const long rec = 32 + NVK_PW_DESC_BYTES + 4;
    for (int k = 0; k < 2; k++) {
        const unsigned char *r = buf + 32 + k * rec;
        const unsigned char *u = r + 32 + NVK_PW_DESC_BYTES;
        const uint32_t uses = (uint32_t)u[0] | (uint32_t)u[1] << 8 | (uint32_t)u[2] << 16 | (uint32_t)u[3] << 24;
        CHECK(r[0] == (k == 0 ? 0x30 : 0x31) && uses == (k == 0 ? 3u : 2u),
              "prog: record %d is program %02x with uses %u, want %02x / %u", k, r[0], uses, k == 0 ? 0x30 : 0x31, k == 0 ? 3u : 2u);
    }
    free(buf);
    return g_fail;
}

// ---- 模式：两条不同的 np 带同一份（配方 + 程序身份） ----
// 同一份程序重新装载一次（新句柄）就是新的 np；对清单它们是同一条记录。第二条 np 第一次走完整路径、并进
// 已有条目（紧挨着的走备忘；隔了几份别的配方的走桶链），拿到同一个槽的标签，之后各走各的标签。
// 这两条完整路径都必须把标签写回：漏写的话那条 np 永远退回完整路径，快路径就白做了。
static int mode_tag_twin(void) {
    nvn_gpu_pipe_desc a;
    tag_recipe(&a, 0);
    prog_setup(0x30);
    struct nvk_pipe_warm_stats st;
    const nvn_gpu_pipe x = tag_np(0), y = tag_np(8), z = tag_np(9);
    CHECK(nvk_pipe_warm_note_prog(&a, &g_prog, x) == 1, "twin: x first");
    CHECK(nvk_pipe_warm_note_prog(&a, &g_prog, x) == 1, "twin: x again");
    CHECK(nvk_pipe_warm_note_prog(&a, &g_prog, y) == 1, "twin: y first (memo)");
    nvk_pipe_warm_stats(&st);
    CHECK(st.recorded == 1 && st.tag_hits == 1, "twin: y's first note merges into the entry through the full path (recorded=%llu hits=%llu, want 1/1)", (unsigned long long)st.recorded, (unsigned long long)st.tag_hits);
    CHECK(nvn_gpu_pipe_warm_tag(y) == nvn_gpu_pipe_warm_tag(x), "twin: both np's carry the tag of the same slot");
    CHECK(nvk_pipe_warm_note_prog(&a, &g_prog, y) == 1, "twin: y again");
    CHECK(nvk_pipe_warm_note_prog(&a, &g_prog, x) == 1, "twin: x again 2");
    nvk_pipe_warm_stats(&st);
    CHECK(st.tag_hits == 3, "twin: y and x both hit their tags (tag_hits=%llu, want 3)", (unsigned long long)st.tag_hits);
    for (int r = 1; r <= 4; r++) {   // 4 份别的配方把 a 挤出最近 4 条备忘
        nvn_gpu_pipe_desc o;
        tag_recipe(&o, r);
        CHECK(nvk_pipe_warm_note_prog(&o, &g_prog, tag_np(r)) == 1, "twin: other recipe %d", r);
    }
    CHECK(nvk_pipe_warm_note_prog(&a, &g_prog, z) == 1, "twin: z first (bucket chain)");
    nvk_pipe_warm_stats(&st);
    CHECK(st.recorded == 5 && st.tag_hits == 3, "twin: z's first note merges through the bucket chain (recorded=%llu hits=%llu, want 5/3)", (unsigned long long)st.recorded, (unsigned long long)st.tag_hits);
    CHECK(nvn_gpu_pipe_warm_tag(z) == nvn_gpu_pipe_warm_tag(x), "twin: z carries the tag of the same slot");
    CHECK(nvk_pipe_warm_note_prog(&a, &g_prog, z) == 1, "twin: z again");
    nvk_pipe_warm_stats(&st);
    CHECK(st.tag_hits == 4, "twin: z hits its tag (tag_hits=%llu, want 4)", (unsigned long long)st.tag_hits);
    nvk_pipe_warm_save();
    char path[600];
    if (file_path(path, sizeof path)) return 2;
    unsigned char *buf; long n;
    const long rec = 32 + NVK_PW_DESC_BYTES + 4;
    if (read_all(path, &buf, &n) != 0 || n != 32 + 5 * rec) { CHECK(0, "twin: manifest file missing or wrong size (%ld)", n); free(buf); return g_fail ? g_fail : 1; }
    const unsigned char *u = buf + 32 + 32 + NVK_PW_DESC_BYTES;   // 第一条 = uses 最大的 a：x、x、y、y、x、z、z = 7
    const uint32_t uses = (uint32_t)u[0] | (uint32_t)u[1] << 8 | (uint32_t)u[2] << 16 | (uint32_t)u[3] << 24;
    CHECK(uses == 7, "twin: persisted uses of the shared entry is %u, want 7", uses);
    free(buf);
    return g_fail;
}

// ---- 模式：清单颠簸 / 逐帧变化量诊断（#1372）----
// 计数与日志只读、不改清单。期望全部按规则手算，不拿被测实现推。开关缺省 0（玩家包恒如此）；
// 用例在模式内置环境变量（模块第一次 note 才读开关 / 上限）。窗口行靠 nvk_pipe_warm_diag_flush 立刻出，不等 5 秒。
static int log_count(const char *needle) {
    int n = 0;
    for (int i = 0; i < g_nlogs; i++) if (strstr(g_logs[i], needle)) n++;
    return n;
}
static int log_has(const char *needle) { return log_count(needle) > 0; }
#define CHECK_LOG(needle) CHECK(log_has(needle), "diag log must contain: %s", needle)
#define U64(x) ((unsigned long long)(x))

// 序列 S（清单上限 4；E1..E4 的 prim = 6..9，A / B / C 的 prim = 20 / 21 / 22，其余字节全同、程序身份同）：
//   E1..E4 各记 3 次填满（uses 都是 3，E1 最久）；然后 A、B、C 轮流记 3 轮；再记一次还在表里的 E2；最后把第 1 轮被挤掉的 E1 记回来。
// 手算（淘汰规则：先挑本轮没碰过的；全表都碰过了就开新一轮）：
//   E1..E4 填满，全是本轮碰过的。A 进来：没有「没碰过」的 → 开新一轮（epochs 1，全部清成没碰过），按 uses / 最久挑 E1，A 记为碰过。
//   B 进来：没碰过的 = {E2, E3, E4}，挤掉 E2；C 进来：挤掉 E3。表 = {A, B, C, E4}（A / B / C 碰过，E4 没碰过）。
//   第 2、3 轮：A / B / C 全在表里，只是命中（memo 里正好是 {C, B, A, E4}，6 次全是 memo 命中），uses 3 / 3 / 3。
//   E2 在第 1 轮被挤掉过：记回来是新记，没碰过的只剩 E4 → 挤掉 E4（E2 算「被淘汰过又插回来」）；
//   E1 同样被挤掉过：记回来是新记，表里 A / B / C / E2 全是碰过的 → 开新一轮（epochs 2），挤掉 uses 最小的 E2（uses 1），E1 算「插回」——
//   这一条专防「只记新配方被挤掉时的散列、不记被挤掉那条自己的散列」：E1 是填表时进的，不是靠挤掉别人进来的。
//   note 共 12 + 9 + 1 + 1 = 23 次；memo 命中 8 + 6 = 14、桶链 0；新记 4 + 3 + 1 + 1 = 9；表满后淘汰 3 + 1 + 1 = 5；
//   此前没见过 4 + 3 = 7（E1..E4、第 1 轮的 A / B / C）；被淘汰过又插回 2（E2、E1）；淘汰扫描 9 次（每次新记一遍，含填表时找空位）。
//   最后表 = {A 3, B 3, C 3, E1 1}。
// 反证（改坏项「去掉先挑没碰过的」= 整表只看 uses、同 uses 取最久没用的，也就是满表淘汰以前的老规则）：
//   第 1 轮 A 挤 E1、B 挤 A、C 挤 B，表 = {C, E2, E3, E4}；第 2、3 轮 A 挤 C、B 挤 A、C 挤 B，每次都是「被淘汰过又插回来」；
//   E2 还在表里（memo 里只剩 C 的槽，走桶链）；E1 记回来挤掉 C，算「插回」。读数 memo 8 / 桶链 1 / 新记 14 / 淘汰 10 / 插回 7 / 扫描 14：
//   这条序列真的颠簸，下面的断言（memo 14 / 新记 9 / 淘汰 5 / 插回 2 / 新一轮 2）红在这一条上。
static void seq_mixed(const uint8_t id[32]) {
    nvn_gpu_pipe_desc e[4], r[3];
    for (int i = 0; i < 4; i++) { base_desc(&e[i]); e[i].prim = 6 + (uint32_t)i; }
    for (int i = 0; i < 3; i++) { base_desc(&r[i]); r[i].prim = 20 + (uint32_t)i; }
    for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++) nvk_pipe_warm_note(&e[i], id);
    for (int round = 0; round < 3; round++) for (int i = 0; i < 3; i++) nvk_pipe_warm_note(&r[i], id);
    nvk_pipe_warm_note(&e[1], id);
    nvk_pipe_warm_note(&e[0], id);
}

// (a) 颠簸：小清单里三条配方轮流进出。读数按上面的「手算」；去掉「先挑没碰过的」这一步，同一条序列真的颠簸（见「反证」），这里的断言就红。
static int mode_diag_thrash(void) {
    setenv("S2A_PW_MAX", "4", 1);
    setenv("S2A_PW_DIAG", "1", 1);
    g_probe_lock = 1;
    uint8_t id[32];
    ident_fill(id, 0x81);
    prog_setup(0x81);
    seq_mixed(id);
    const unsigned long long w_memo = 14, w_chain = 0, w_news = 9, w_evict = 5, w_reins = 2, w_epochs = 2;
    struct nvk_pipe_warm_diag d;
    nvk_pipe_warm_diag_stats(&d);
    CHECK(d.on == 1, "diag must be on when nvk_pw_diag=1");
    CHECK(d.calls == 23 && d.memo_hits == w_memo && d.chain_hits == w_chain,
          "note/memo/chain want 23/%llu/%llu, got %llu/%llu/%llu", w_memo, w_chain, U64(d.calls), U64(d.memo_hits), U64(d.chain_hits));
    CHECK(d.news == w_news && d.evicts == w_evict, "new/evict want %llu/%llu, got %llu/%llu", w_news, w_evict, U64(d.news), U64(d.evicts));
    CHECK(d.fresh == 7 && d.reins == w_reins, "never-seen/reinserted want 7/%llu, got %llu/%llu", w_reins, U64(d.fresh), U64(d.reins));
    CHECK(d.scans == w_news, "victim scans want %llu, got %llu", w_news, U64(d.scans));
    CHECK(d.ev_clears == 0, "no eviction-set clear expected, got %llu", U64(d.ev_clears));
    CHECK(d.samples_fresh == 1 && d.samples_reins == 1, "one sample per class per window, got %llu/%llu",
          U64(d.samples_fresh), U64(d.samples_reins));
    struct nvk_pipe_warm_stats st;
    nvk_pipe_warm_stats(&st);
    CHECK(st.recorded == d.news, "diag news must equal stats.recorded (%llu vs %llu)", U64(d.news), U64(st.recorded));
    CHECK(st.epochs == w_epochs, "epochs want %llu, got %llu", w_epochs, U64(st.epochs));
    CHECK(g_nlogs == 3, "before the flush: the switch-on line and the two sample lines only (no window line yet), got %d", g_nlogs);
    nvk_pipe_warm_diag_flush();
    char want[256];
    snprintf(want, sizeof want, "秒：note 23 次（memo %llu / 链 %llu / 新记 %llu）", w_memo, w_chain, w_news);
    CHECK_LOG(want);
    snprintf(want, sizeof want, "新记里 此前没见过 7、被淘汰过又插回 %llu", w_reins);
    CHECK_LOG(want);
    snprintf(want, sizeof want, "满后淘汰 %llu 次", w_evict);
    CHECK_LOG(want);
    snprintf(want, sizeof want, "淘汰扫描 %llu 次", w_news);
    CHECK_LOG(want);
    CHECK_LOG("淘汰集合清空 0 次（累计 0）");
    CHECK_LOG("条数 4 / 容量 4");
    // 样本：第一条「此前没见过」是 E2（清单里只有 E1，只差 prim 的低字节 06→07）；
    // 第一条「被淘汰过又插回」是 E2 记回来那次（表 = {A, B, C, E4}，都只差 prim，取最前的 A：14→07）；
    // 去掉「先挑没碰过的」时是第 2 轮的 A（表 = {C, E2, E3, E4}，取最前的 C：16→14）。
    CHECK_LOG("样本（此前没见过，第 1 条）：最近条目程序身份相同，配方差 1 字节：prim@53 06→07");
    CHECK_LOG("样本（被淘汰过又插回来，第 1 条）：最近条目程序身份相同，配方差 1 字节：prim@53 14→07");
    // 样本行带抽样耗时：持锁只拷快照，整表比对在锁外
    CHECK_LOG("快照拷贝 ");
    CHECK_LOG(" 微秒（持锁）、比对 ");
    // 锁外：开机行、两条样本、窗口行——每一条出来时清单锁都得是空的
    CHECK(g_probe_n == g_nlogs && g_probe_n == 4, "lock probe must run once per diag line (probe %d, lines %d, want 4)", g_probe_n, g_nlogs);
    CHECK(g_probe_blocked == 0, "%d diag line(s) were logged while holding the list lock", g_probe_blocked);
    return g_fail;
}

// (b) 逐帧变化量：同一条管线每帧只有第 3 条流的 stride 在变。四个窗口各一条样本（用 flush 切窗口）：
//   窗口 1：A、B（B 与 A 只差 prim：04→07）；
//   窗口 2：V0 = A 的配方但 stride[2] = 100，程序身份是另一个（id2）→ 最近条目是 A，身份不同（记 1000 分），差 stride 低字节 00→64；
//   窗口 3：同一份 V0，程序身份与 A 相同（id1）→ 最近条目必须是 A（身份相同优先；若只比字节，会选字节全同的 id2 那条，差 0 字节），差 00→64；
//   窗口 4：39 帧，每帧 A、B（memo 命中）+ 一条 stride = 100 + k 的新配方 → 样本是第 1 帧的 V1（stride 101）：最近 A（A 在 V0 之前，同分取前），差 00→65。
// 手算：note 2 + 1 + 1 + 39 × 3 = 121 次；memo 命中 39 × 2 = 78；链 0；新记 2 + 1 + 1 + 39 = 43，全是此前没见过；没有淘汰（上限 8192）；
// 扫描 43 次；样本此前没见过 4 条、插回 0 条。
static int mode_diag_vary(void) {
    setenv("S2A_PW_DIAG", "1", 1);
    unsetenv("S2A_PW_MAX");
    g_probe_lock = 1;
    uint8_t id1[32], id2[32];
    ident_fill(id1, 0x91);
    ident_fill(id2, 0x92);
    prog_setup(0x91);
    nvn_gpu_pipe_desc a, b, v;
    base_desc(&a);
    b = a; b.prim = 7;
    nvk_pipe_warm_note(&a, id1);
    nvk_pipe_warm_note(&b, id1);
    nvk_pipe_warm_diag_flush();
    v = a; v.stream[2].stride = 100;
    nvk_pipe_warm_note(&v, id2);
    nvk_pipe_warm_diag_flush();
    nvk_pipe_warm_note(&v, id1);
    nvk_pipe_warm_diag_flush();
    for (int k = 1; k < 40; k++) {
        nvk_pipe_warm_note(&a, id1);
        nvk_pipe_warm_note(&b, id1);
        v.stream[2].stride = (uint32_t)(100 + k);
        nvk_pipe_warm_note(&v, id1);
    }
    struct nvk_pipe_warm_diag d;
    nvk_pipe_warm_diag_stats(&d);
    CHECK(d.calls == 121 && d.memo_hits == 78 && d.chain_hits == 0,
          "note/memo/chain want 121/78/0, got %llu/%llu/%llu", U64(d.calls), U64(d.memo_hits), U64(d.chain_hits));
    CHECK(d.news == 43 && d.fresh == 43 && d.reins == 0 && d.evicts == 0,
          "new/fresh/reins/evict want 43/43/0/0, got %llu/%llu/%llu/%llu", U64(d.news), U64(d.fresh), U64(d.reins), U64(d.evicts));
    CHECK(d.scans == 43, "victim scans want 43, got %llu", U64(d.scans));
    CHECK(d.samples_fresh == 4 && d.samples_reins == 0, "samples want 4/0, got %llu/%llu", U64(d.samples_fresh), U64(d.samples_reins));
    CHECK_LOG("样本（此前没见过，第 1 条）：最近条目程序身份相同，配方差 1 字节：prim@53 04→07");
    CHECK_LOG("样本（此前没见过，第 2 条）：最近条目程序身份不同，配方差 1 字节：stream[2].stride@224 00→64");
    CHECK_LOG("样本（此前没见过，第 3 条）：最近条目程序身份相同，配方差 1 字节：stream[2].stride@224 00→64");
    CHECK_LOG("样本（此前没见过，第 4 条）：最近条目程序身份相同，配方差 1 字节：stream[2].stride@224 00→65");
    // 开机行 1 + 窗口行 3（三次 flush）+ 样本行 4 = 8 条，每条出来时清单锁都得是空的
    CHECK(g_probe_n == g_nlogs && g_probe_n == 8, "lock probe must run once per diag line (probe %d, lines %d, want 8)", g_probe_n, g_nlogs);
    CHECK(g_probe_blocked == 0, "%d diag line(s) were logged while holding the list lock", g_probe_blocked);
    return g_fail;
}

// 负对照 1：没有颠簸、没有淘汰——50 条不同的配方各记 2 次，上限 8192。「被淘汰过又插回」「淘汰」必须是 0。
static int mode_diag_calm(void) {
    setenv("S2A_PW_DIAG", "1", 1);
    unsetenv("S2A_PW_MAX");
    uint8_t id[32];
    ident_fill(id, 0xa1);
    prog_setup(0xa1);
    nvn_gpu_pipe_desc d;
    base_desc(&d);
    for (int i = 0; i < 50; i++) {
        d.stream[1].stride = (uint32_t)(500 + i);
        nvk_pipe_warm_note(&d, id);
        nvk_pipe_warm_note(&d, id);
    }
    struct nvk_pipe_warm_diag s;
    nvk_pipe_warm_diag_stats(&s);
    CHECK(s.calls == 100 && s.memo_hits == 50 && s.chain_hits == 0, "note/memo/chain want 100/50/0, got %llu/%llu/%llu",
          U64(s.calls), U64(s.memo_hits), U64(s.chain_hits));
    CHECK(s.news == 50 && s.fresh == 50 && s.reins == 0 && s.evicts == 0, "new/fresh/reins/evict want 50/50/0/0, got %llu/%llu/%llu/%llu",
          U64(s.news), U64(s.fresh), U64(s.reins), U64(s.evicts));
    return g_fail;
}

// 负对照 2：有淘汰、没有颠簸——4 条的清单里依次进 10 条各不相同的配方，没有任何一条回来过。
// 「淘汰」= 10 − 4 = 6，「被淘汰过又插回」必须是 0（只数淘汰的实现会把它记成 6）。
static int mode_diag_evict(void) {
    setenv("S2A_PW_MAX", "4", 1);
    setenv("S2A_PW_DIAG", "1", 1);
    uint8_t id[32];
    ident_fill(id, 0xb1);
    prog_setup(0xb1);
    nvn_gpu_pipe_desc d;
    base_desc(&d);
    for (int i = 0; i < 10; i++) {
        d.stream[1].stride = (uint32_t)(700 + i);
        nvk_pipe_warm_note(&d, id);
    }
    struct nvk_pipe_warm_diag s;
    nvk_pipe_warm_diag_stats(&s);
    CHECK(s.calls == 10 && s.news == 10 && s.evicts == 6, "note/new/evict want 10/10/6, got %llu/%llu/%llu", U64(s.calls), U64(s.news), U64(s.evicts));
    CHECK(s.fresh == 10 && s.reins == 0, "never-seen/reinserted want 10/0, got %llu/%llu", U64(s.fresh), U64(s.reins));
    CHECK(s.scans == 10, "victim scans want 10, got %llu", U64(s.scans));
    return g_fail;
}

// 淘汰集合清空（#1372 评审：读到「此前没见过」偏高时，要分得清是真新、还是集合被清过）：上限 4，10 万条各不相同的配方依次进（各 1 次）。
// 手算：第 k 次淘汰（k = 1..99,996）挤掉第 k−1 条配方（从 0 数；条 0..3 填表，条 4 挤掉条 0）。淘汰集合 2^17 项，装到 3/4 = 98,304 项时清空：
//   第 98,305 次淘汰（挤掉条 98,304）之前集合里已有 98,304 项 → 清空一次，条 98,304 起重新记；99,996 次淘汰里只清空这一次。
//   再记条 0（清空前被挤掉的，记录已丢）→ 只能算「此前没见过」（漏数，窗口行的「清空次数」就是用来标这种情形的）；
//   再记条 99,990（清空之后才被挤掉的，记录还在）→ 算「被淘汰过又插回来」。
// 合计：note 100,002；新记 100,002；淘汰 99,998；此前没见过 100,001；插回 1；清空 1 次。
static int mode_diag_clear(void) {
    setenv("S2A_PW_MAX", "4", 1);
    setenv("S2A_PW_DIAG", "1", 1);
    uint8_t id[32];
    ident_fill(id, 0xc1);
    prog_setup(0xc1);
    nvn_gpu_pipe_desc d;
    base_desc(&d);
    for (uint32_t i = 0; i < 100000; i++) {
        d.stream[1].stride = 1000 + i;
        nvk_pipe_warm_note(&d, id);
    }
    struct nvk_pipe_warm_diag s;
    nvk_pipe_warm_diag_stats(&s);
    CHECK(s.news == 100000 && s.evicts == 99996 && s.fresh == 100000 && s.reins == 0,
          "after 100000 distinct recipes: new/evict/fresh/reins want 100000/99996/100000/0, got %llu/%llu/%llu/%llu",
          U64(s.news), U64(s.evicts), U64(s.fresh), U64(s.reins));
    CHECK(s.ev_clears == 1, "the eviction set must have been cleared exactly once, got %llu", U64(s.ev_clears));
    d.stream[1].stride = 1000;   // 条 0：清空前被挤掉的
    nvk_pipe_warm_note(&d, id);
    nvk_pipe_warm_diag_stats(&s);
    CHECK(s.fresh == 100001 && s.reins == 0,
          "recipe evicted before the clear must count as never-seen (record lost): fresh/reins want 100001/0, got %llu/%llu",
          U64(s.fresh), U64(s.reins));
    d.stream[1].stride = 1000 + 99990;   // 条 99,990：清空后才被挤掉的
    nvk_pipe_warm_note(&d, id);
    nvk_pipe_warm_diag_stats(&s);
    CHECK(s.calls == 100002 && s.news == 100002 && s.evicts == 99998 && s.fresh == 100001 && s.reins == 1 && s.ev_clears == 1,
          "calls/new/evict/fresh/reins/clears want 100002/100002/99998/100001/1/1, got %llu/%llu/%llu/%llu/%llu/%llu",
          U64(s.calls), U64(s.news), U64(s.evicts), U64(s.fresh), U64(s.reins), U64(s.ev_clears));
    nvk_pipe_warm_diag_flush();
    CHECK_LOG("（累计 1）；条数 4 / 容量 4");   // 不管 5 秒窗口在中途滚没滚，最后一行的累计清空次数都是 1
    return g_fail;
}

// 并发：4 个线程各记 20000 次，配方在 2048 条里轮转，上限 64，诊断开（抽样的快照 / 比对 / 日志都在锁外，这里跑真并发）。
// 不依赖调度的守恒式：每次 note 恰好是 memo 命中 / 链命中 / 新记 三者之一；新记 = 此前没见过 + 插回；
// 新记先填满 64 个空位、其余每条淘汰一条（淘汰 = 新记 − 64）；recorded 与 news 同口径；
// 样本每类 ≤ 24，且样本行数 = 样本计数（抢到名额的样本，解锁后一定打出一行）。
struct mt_arg { int t; const uint8_t *id; };
static void *mt_worker(void *p) {
    const struct mt_arg *a = p;
    nvn_gpu_pipe_desc d;
    base_desc(&d);
    for (int k = 0; k < 20000; k++) {
        d.stream[1].stride = 3000 + (uint32_t)((a->t * 7919 + k * 13) % 2048);
        nvk_pipe_warm_note(&d, a->id);
    }
    return NULL;
}
static int mode_diag_mt(void) {
    setenv("S2A_PW_MAX", "64", 1);
    setenv("S2A_PW_DIAG", "1", 1);
    uint8_t id[32];
    ident_fill(id, 0xd1);
    prog_setup(0xd1);
    pthread_t th[4];
    struct mt_arg arg[4];
    for (int t = 0; t < 4; t++) {
        arg[t].t = t;
        arg[t].id = id;
        CHECK(pthread_create(&th[t], NULL, mt_worker, &arg[t]) == 0, "create worker thread %d", t);
    }
    for (int t = 0; t < 4; t++) pthread_join(th[t], NULL);
    struct nvk_pipe_warm_diag d;
    nvk_pipe_warm_diag_stats(&d);
    struct nvk_pipe_warm_stats st;
    nvk_pipe_warm_stats(&st);
    CHECK(d.calls == 80000, "calls want 80000, got %llu", U64(d.calls));
    CHECK(d.memo_hits + d.chain_hits + d.news == d.calls, "every note is a memo hit, a chain hit or a new record: %llu+%llu+%llu vs %llu",
          U64(d.memo_hits), U64(d.chain_hits), U64(d.news), U64(d.calls));
    CHECK(d.fresh + d.reins == d.news, "new = never-seen + reinserted: %llu+%llu vs %llu", U64(d.fresh), U64(d.reins), U64(d.news));
    CHECK(d.news >= 64 && d.evicts == d.news - 64, "evictions = new − 64: new %llu evict %llu", U64(d.news), U64(d.evicts));
    CHECK(st.recorded == d.news, "recorded must equal diag news: %llu vs %llu", U64(st.recorded), U64(d.news));
    CHECK(d.scans == d.news, "one victim scan per new record: %llu vs %llu", U64(d.scans), U64(d.news));
    CHECK(d.samples_fresh >= 1 && d.samples_fresh <= 24 && d.samples_reins <= 24, "samples per class within 1..24 / 0..24, got %llu/%llu",
          U64(d.samples_fresh), U64(d.samples_reins));
    CHECK(log_count("样本（") == (int)(d.samples_fresh + d.samples_reins), "every claimed sample must print one line: %d lines vs %llu counted",
          log_count("样本（"), U64(d.samples_fresh + d.samples_reins));
    return g_fail;
}

// 「不改行为」：同一串 note（序列 S）在诊断开 / 关两个进程里各跑一遍并落盘，父进程比两份文件逐字节相同（见 diag_equiv）。
// 子进程里：关着时诊断读数全 0、一行诊断日志都不出；开着时读数按上面的手算。
static int mode_diag_save(void) {
    setenv("S2A_PW_MAX", "4", 1);
    const char *e = getenv("S2A_PW_DIAG");
    const int want_on = e && *e && atoi(e) != 0;
    const unsigned long long w_news = 9, w_reins = 2, w_evict = 5;   // 读数按 seq_mixed 的手算
    uint8_t id[32];
    ident_fill(id, 0x81);
    prog_setup(0x81);
    seq_mixed(id);
    struct nvk_pipe_warm_diag d;
    nvk_pipe_warm_diag_stats(&d);
    if (want_on) {
        CHECK(d.on == 1 && d.news == w_news && d.reins == w_reins && d.evicts == w_evict, "diag on: on/new/reins/evict want 1/%llu/%llu/%llu, got %d/%llu/%llu/%llu",
              w_news, w_reins, w_evict, d.on, U64(d.news), U64(d.reins), U64(d.evicts));
    } else {
        CHECK(d.on == 0 && d.calls == 0 && d.memo_hits == 0 && d.chain_hits == 0 && d.news == 0 && d.evicts == 0 && d.fresh == 0 &&
              d.reins == 0 && d.scans == 0 && d.scan_ns == 0 && d.samples_fresh == 0 && d.samples_reins == 0,
              "diag off (default): every counter must stay 0");
        nvk_pipe_warm_diag_flush();
        CHECK(g_nlogs == 0, "diag off: no diag log line at all, got %d", g_nlogs);
    }
    struct nvk_pipe_warm_stats st;
    nvk_pipe_warm_stats(&st);
    CHECK(st.recorded == w_news, "recorded must be %llu with the diag on or off, got %llu", w_news, U64(st.recorded));
    nvk_pipe_warm_save();
    return g_fail;
}

// ---- 模式：满表淘汰规则（先挑本轮没碰过的旧条目；合同见 nvk_pipe_warm_note）----
// 起因（真机读数）：清单载入就是满的（8192 条，旧条目 uses 都 ≥ 2，其中 uses == 1 的只有 1 条），本局的配方是新记的、uses 才 1，
// 只按 uses 挑牺牲者就让每条新配方只能挤掉上一条新配方：一局里 65 万次「新记」有 99.9% 是被淘汰过又插回，清单学不到本局的配方。
// 反证不靠产品里的旧规则开关（产品里只有一条规则）：android/tools/nvk_warm_modules_test.sh 里有一个改坏项「去掉先挑没碰过的」，
// 把被测实现改回旧规则，下面的断言必须红在「新配方被互相挤掉」上，说明用例不是空转。
// 配方 k = 第 2 条流的 stride 取 AG_BASE + k（其余字节全同、程序身份同）；落盘文件里按整份配方字节认 k，不碰被测实现的内部。
enum { AG_BASE = 5000, AG_NK = 128 };
static void ag_desc(nvn_gpu_pipe_desc *d, uint32_t k) { base_desc(d); d->stream[1].stride = AG_BASE + k; }
static void ag_note(const uint8_t id[32], uint32_t k, int times) {
    nvn_gpu_pipe_desc d;
    ag_desc(&d, k);
    for (int i = 0; i < times; i++) nvk_pipe_warm_note(&d, id);
}
// 读回落盘文件：uses_of[k] = 配方 k 的 uses（文件里没有 = 0）。返回条数；读不了 / 有认不出的条目 = -1。
static int ag_file_uses(const uint8_t id[32], uint32_t *uses_of) {
    memset(uses_of, 0, AG_NK * sizeof *uses_of);
    char path[600];
    if (file_path(path, sizeof path)) return -1;
    unsigned char *b = NULL;
    long n = 0;
    if (read_all(path, &b, &n) || n < 32) { free(b); return -1; }
    const uint32_t cnt = (uint32_t)b[20] | (uint32_t)b[21] << 8 | (uint32_t)b[22] << 16 | (uint32_t)b[23] << 24;
    const size_t ent = 32 + NVK_PW_DESC_BYTES + 4;
    if ((size_t)n != 32 + cnt * ent) { free(b); return -1; }
    static struct nvk_pipe_rec want[AG_NK];
    for (uint32_t k = 0; k < AG_NK; k++) { nvn_gpu_pipe_desc d; ag_desc(&d, k); nvk_pipe_warm_rec_of(&d, id, &want[k]); }
    int bad = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        const unsigned char *e = b + 32 + i * ent;
        int found = -1;
        for (uint32_t k = 0; k < AG_NK && found < 0; k++)
            if (!memcmp(e, id, 32) && !memcmp(e + 32, want[k].desc, NVK_PW_DESC_BYTES)) found = (int)k;
        if (found < 0) { bad++; continue; }
        const unsigned char *u = e + 32 + NVK_PW_DESC_BYTES;
        uses_of[found] = (uint32_t)u[0] | (uint32_t)u[1] << 8 | (uint32_t)u[2] << 16 | (uint32_t)u[3] << 24;
    }
    free(b);
    return bad ? -1 : (int)cnt;
}
static void ag_setup(const char *cap, uint8_t seed, uint8_t id[32]) {
    setenv("S2A_PW_MAX", cap, 1);
    ident_fill(id, seed);
    prog_setup(seed);
}

// 上一局记下 16 条（配方 k 记 k + 2 次 → uses 2..17，正好填满），本局载入后只用 5 条新配方轮流（40 轮 = 200 次 note）。
// 5 条新配方各只记一次（recorded +5），挤掉的是旧条目里 uses 最小的 5 条（配方 0..4），其余 11 条旧条目原样留着，
// 没有「新一轮」（epochs 0），每条新配方 40 次 note = uses 40。
// 去掉「先挑没碰过的」（改坏项 no-fresh-preference）：新配方 uses 才 1，只能挤掉上一条新配方——200 次 note 全是新记（recorded +200），
// 旧条目只丢了配方 0，清单里最后只剩最后一条新配方（uses 1）；下面「新配方被互相挤掉」的断言红。
static int mode_aging_thrash(void) {
    enum { CAP = 16, WS = 5, ROUNDS = 40, NEW0 = 100 };
    uint8_t id[32];
    ag_setup("16", 0x71, id);
    for (uint32_t k = 0; k < CAP; k++) ag_note(id, k, (int)k + 2);
    nvk_pipe_warm_save();
    CHECK(nvk_pipe_warm_load() == CAP, "aging thrash: the previous session's %d recipes must load back", CAP);
    struct nvk_pipe_warm_stats s0, s1;
    nvk_pipe_warm_stats(&s0);
    for (int r = 0; r < ROUNDS; r++) for (uint32_t j = 0; j < WS; j++) ag_note(id, NEW0 + j, 1);
    nvk_pipe_warm_stats(&s1);
    nvk_pipe_warm_save();
    uint32_t u[AG_NK];
    CHECK(ag_file_uses(id, u) == CAP, "aging thrash: the saved manifest must hold %d recipes", CAP);
    const uint64_t rec_d = s1.recorded - s0.recorded;
    CHECK(rec_d == WS, "aging thrash: each of the %d new recipes is recorded once, got %llu new records", WS, U64(rec_d));
    CHECK(s1.epochs == 0, "aging thrash: old entries never ran out, epochs want 0, got %llu", U64(s1.epochs));
    CHECK(s1.rejected - s0.rejected == WS, "aging thrash: %d evictions (one per new recipe), got %llu", WS, U64(s1.rejected - s0.rejected));
    for (uint32_t k = 0; k < WS; k++) CHECK(u[k] == 0, "aging thrash: old recipe %u (uses %u, among the %d coldest) must be evicted, still has uses %u", k, k + 2, WS, u[k]);
    for (uint32_t k = WS; k < CAP; k++) CHECK(u[k] == k + 2, "aging thrash: old recipe %u must stay with uses %u, got %u", k, k + 2, u[k]);
    for (uint32_t j = 0; j < WS; j++) CHECK(u[NEW0 + j] == ROUNDS, "aging thrash: new recipe %u must be kept with uses %d, got %u", NEW0 + j, ROUNDS, u[NEW0 + j]);
    return g_fail;
}

// 载入进来的旧条目本局被命中一次就算碰过：新配方来时先挤别的旧条目，哪怕被命中的这条 uses 仍是最小。
// 旧文件：配方 0..3 的 uses = 1 / 3 / 4 / 5。本局命中配方 0（uses 2），再来一条新配方 100：
//   挤掉配方 1（没碰过的里 uses 最小）；去掉「先挑没碰过的」时会挤掉刚被命中的配方 0（全表 uses 最小）。
static int mode_aging_touch(void) {
    uint8_t id[32];
    ag_setup("4", 0x72, id);
    static const int old_uses[4] = {1, 3, 4, 5};
    for (uint32_t k = 0; k < 4; k++) ag_note(id, k, old_uses[k]);
    nvk_pipe_warm_save();
    CHECK(nvk_pipe_warm_load() == 4, "aging touch: 4 old recipes must load back");
    ag_note(id, 0, 1);
    ag_note(id, 100, 1);
    nvk_pipe_warm_save();
    uint32_t u[AG_NK];
    CHECK(ag_file_uses(id, u) == 4, "aging touch: the saved manifest must hold 4 recipes");
    CHECK(u[0] == 2, "aging touch: the recipe hit this session (uses 2) must stay, got uses %u", u[0]);
    CHECK(u[1] == 0, "aging touch: the coldest untouched old recipe (uses 3) must be evicted, got uses %u", u[1]);
    CHECK(u[2] == 4 && u[3] == 5 && u[100] == 1, "aging touch: recipes 2 / 3 / new keep uses 4 / 5 / 1, got %u / %u / %u", u[2], u[3], u[100]);
    return g_fail;
}

// 本轮碰过的配方把表占满以后，工作集换了一批：开新一轮，换进来的新配方不再互相挤。
// 上限 8；配方 0..7 本局各记 20 - 2k 次（uses 20 / 18 / ... / 6，记的顺序 = 0 先 7 后，所以 uses 最小的 7 反而最近），全是本轮碰过的。
//   然后新配方 100..102 轮流 30 轮（90 次 note）。
//   100 进来时没有「没碰过」的 → 新一轮（epochs 1），挤 uses 最小的 7；101 挤 6、102 挤 5（按 uses，不按最近）；
//   之后 90 次里只有这 3 次是新记。表 = {0..4, 100..102}，新配方各 30 次 note = uses 30。
//   再命中 4（uses 12 → 13，仍是旧条目里最小，但碰过了），来新配方 110：挤的是没碰过的里 uses 最小的 3（14），不是 4。
//   去掉「先挑没碰过的」时：100 挤 7；101 挤上一条 100（uses 1）；之后每次 note 都新记（recorded +90），表里 0..6 不动、最后一条新配方 uses 1。
static int mode_aging_epoch(void) {
    enum { CAP = 8, ROUNDS = 30, NEW0 = 100 };
    uint8_t id[32];
    ag_setup("8", 0x73, id);
    for (uint32_t k = 0; k < CAP; k++) ag_note(id, k, 20 - 2 * (int)k);
    struct nvk_pipe_warm_stats s0, s1;
    nvk_pipe_warm_stats(&s0);
    for (int r = 0; r < ROUNDS; r++) for (uint32_t j = 0; j < 3; j++) ag_note(id, NEW0 + j, 1);
    nvk_pipe_warm_stats(&s1);
    const uint64_t rec_d = s1.recorded - s0.recorded;
    CHECK(rec_d == 3, "aging epoch: the 3 new recipes are recorded once each, got %llu new records", U64(rec_d));
    CHECK(s1.epochs == 1, "aging epoch: the first new recipe finds every entry touched → one new round, got epochs %llu", U64(s1.epochs));
    ag_note(id, 4, 1);          // 旧条目里 uses 最小的一条（12）被命中：13，本轮碰过
    ag_note(id, 110, 1);
    nvk_pipe_warm_save();
    uint32_t u[AG_NK];
    CHECK(ag_file_uses(id, u) == CAP, "aging epoch: the saved manifest must hold %d recipes", CAP);
    static const uint32_t keep[] = {20, 18, 16, 0, 13};   // 配方 0 / 1 / 2 / 3 / 4（3 被挤掉了）
    for (uint32_t k = 0; k < 5; k++) CHECK(u[k] == keep[k], "aging epoch: old recipe %u want uses %u, got %u", k, keep[k], u[k]);
    for (uint32_t k = 5; k < 8; k++) CHECK(u[k] == 0, "aging epoch: old recipe %u (uses %d) must be evicted by uses order, got uses %u", k, 20 - 2 * (int)k, u[k]);
    for (uint32_t j = 0; j < 3; j++) CHECK(u[NEW0 + j] == ROUNDS, "aging epoch: new recipe %u want uses %d, got %u", NEW0 + j, ROUNDS, u[NEW0 + j]);
    CHECK(u[110] == 1, "aging epoch: the last new recipe is kept with uses 1, got %u", u[110]);
    return g_fail;
}

static const char *k_modes[] = {"compute", "compute_off", "fields", "dedup", "collide", "cap", "defcap", "hardcap", "frozen", "lease", "save", "load", "save_race",
                                "tag_reload", "tag_prog", "tag_twin",
                                "aging_thrash", "aging_touch", "aging_epoch",
                                "diag_thrash", "diag_vary", "diag_calm", "diag_evict", "diag_clear", "diag_mt"};
static const char *k_bad[] = {"magic", "fmt", "descver", "entry", "count", "sum", "trunc", "payload"};

static int run_child(const char *self, const char *a, const char *b) {
    pid_t pid = fork();
    if (pid == 0) {
        if (b) execl(self, self, a, b, (char *)NULL);
        else execl(self, self, a, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// 诊断「不改行为」：序列 S 在诊断关（缺省）/ 开两个进程里各跑一遍并落盘，两份清单文件必须逐字节相同
// （同一串 note → 同样的槽位、同样的 uses / 淘汰顺序 → 同样的落盘字节）。返回 0 = 通过。
static int diag_equiv(const char *self, const char *base) {
    char dir[2][600], file[2][700];
    // 「关」= 开关属性根本不设（玩家包永远是这个状态，走代码里的缺省值）：这样把缺省值写反（缺省 1）的实现，关这一遍会被开关带着开起来而变红。
    const char *flag[2] = {NULL, "1"};
    int fail = 0;
    for (int i = 0; i < 2; i++) {
        snprintf(dir[i], sizeof dir[i], "%s/diag-%s", base, i ? "on" : "off");
        mkdir(dir[i], 0700);
        snprintf(file[i], sizeof file[i], "%.500s/pipes-v1-%d.bin", dir[i], NVK_PW_DESC_BYTES);
        unlink(file[i]);
        setenv("S2A_PW_DIR", dir[i], 1);
        if (flag[i]) setenv("S2A_PW_DIAG", flag[i], 1); else unsetenv("S2A_PW_DIAG");
        int rc = run_child(self, "diag_save", NULL);
        if (rc != 0) { fprintf(stderr, "FAIL diag_save child (diag=%s) rc=%d\n", flag[i] ? flag[i] : "unset(default)", rc); fail = 1; }
    }
    unsetenv("S2A_PW_DIAG");
    setenv("S2A_PW_DIR", base, 1);
    unsigned char *a = NULL, *b = NULL;
    long na = 0, nb = 0;
    if (read_all(file[0], &a, &na) || read_all(file[1], &b, &nb)) {
        fprintf(stderr, "FAIL diag equivalence: a saved manifest is missing\n");
        fail = 1;
    } else if (na != nb || memcmp(a, b, (size_t)na) != 0) {
        fprintf(stderr, "FAIL diag equivalence: saved manifests differ (diag off %ld bytes vs diag on %ld bytes)\n", na, nb);
        fail = 1;
    } else if (na != 32 + 4 * (32 + NVK_PW_DESC_BYTES + 4)) {
        fprintf(stderr, "FAIL diag equivalence: manifest must hold the 4 surviving recipes, got %ld bytes\n", na);
        fail = 1;
    }
    free(a);
    free(b);
    for (int i = 0; i < 2; i++) { unlink(file[i]); rmdir(dir[i]); }
    return fail;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "badfile")) return mode_badfile(argv[2]) ? 1 : 0;
    if (argc >= 3 && (!strcmp(argv[1], "tag_old") || !strcmp(argv[1], "tag_new"))) return mode_tag(!strcmp(argv[1], "tag_new"), atoi(argv[2])) ? 1 : 0;
    if (argc >= 2) {
        const char *m = argv[1];
        int (*fn)(void) = !strcmp(m, "compute") ? mode_compute : !strcmp(m, "compute_off") ? mode_compute_off : !strcmp(m, "fields") ? mode_fields : !strcmp(m, "dedup") ? mode_dedup :
                          !strcmp(m, "collide") ? mode_collide : !strcmp(m, "cap") ? mode_cap :
                          !strcmp(m, "defcap") ? mode_defcap : !strcmp(m, "hardcap") ? mode_hardcap :
                          !strcmp(m, "frozen") ? mode_frozen : !strcmp(m, "lease") ? mode_lease :
                          !strcmp(m, "save_race") ? mode_save_race : !strcmp(m, "tag_reload") ? mode_tag_reload :
                          !strcmp(m, "tag_prog") ? mode_tag_prog : !strcmp(m, "tag_twin") ? mode_tag_twin :
                          !strcmp(m, "aging_thrash") ? mode_aging_thrash : !strcmp(m, "aging_touch") ? mode_aging_touch :
                          !strcmp(m, "aging_epoch") ? mode_aging_epoch :
                          !strcmp(m, "diag_thrash") ? mode_diag_thrash : !strcmp(m, "diag_vary") ? mode_diag_vary :
                          !strcmp(m, "diag_calm") ? mode_diag_calm : !strcmp(m, "diag_evict") ? mode_diag_evict :
                          !strcmp(m, "diag_clear") ? mode_diag_clear : !strcmp(m, "diag_mt") ? mode_diag_mt :
                          !strcmp(m, "diag_save") ? mode_diag_save :
                          !strcmp(m, "save") ? mode_save : !strcmp(m, "load") ? mode_load : NULL;
        if (!fn) { fprintf(stderr, "unknown mode %s\n", m); return 2; }
        return fn() ? 1 : 0;
    }
    const char *dir = getenv("S2A_PW_DIR");
    char tmp[512];
    if (!dir || !*dir) {
        snprintf(tmp, sizeof tmp, "/tmp/s2a-pw-XXXXXX");
        if (!mkdtemp(tmp)) { perror("mkdtemp"); return 2; }
        setenv("S2A_PW_DIR", tmp, 1);
    }
    int fail = 0;
    for (unsigned i = 0; i < sizeof k_modes / sizeof k_modes[0]; i++) {
        if (!strcmp(k_modes[i], "cap")) setenv("S2A_PW_MAX", "4", 1);   // 上限模式：清单只留 4 条
        int rc = run_child(argv[0], k_modes[i], NULL);
        if (!strcmp(k_modes[i], "cap")) unsetenv("S2A_PW_MAX");
        printf("%-8s %s\n", k_modes[i], rc == 0 ? "ok" : "FAIL");
        if (rc != 0) fail = 1;
    }
    for (unsigned i = 0; i < sizeof k_bad / sizeof k_bad[0]; i++) {
        int rc = run_child(argv[0], "badfile", k_bad[i]);
        printf("bad/%-8s %s\n", k_bad[i], rc == 0 ? "ok" : "FAIL");
        if (rc != 0) fail = 1;
    }
    // 管线标签路径与完整路径：同一串换管线序列各跑一遍（各用自己的目录），清单文件必须逐字节相同
    char base[512];
    snprintf(base, sizeof base, "%s", getenv("S2A_PW_DIR"));   // 上面没给目录时是父进程自己建的临时目录（dir 还是空的）
    static const char *k_tag_name[] = {"keep", "evict", "recency"};
    for (int variant = 0; variant < 3; variant++) {
        char da[600], db[600], fa[700], fb[700], vs[8];
        snprintf(da, sizeof da, "%s/tag%d-plain", base, variant);
        snprintf(db, sizeof db, "%s/tag%d-tagged", base, variant);
        snprintf(vs, sizeof vs, "%d", variant);
        (void)mkdir(da, 0700); (void)mkdir(db, 0700);
        if (variant) setenv("S2A_PW_MAX", variant == 1 ? "6" : "3", 1);
        setenv("S2A_PW_DIR", da, 1);
        int r1 = run_child(argv[0], "tag_old", vs);
        setenv("S2A_PW_DIR", db, 1);
        int r2 = run_child(argv[0], "tag_new", vs);
        unsetenv("S2A_PW_MAX");
        setenv("S2A_PW_DIR", base, 1);
        snprintf(fa, sizeof fa, "%s/pipes-v1-%d.bin", da, NVK_PW_DESC_BYTES);
        snprintf(fb, sizeof fb, "%s/pipes-v1-%d.bin", db, NVK_PW_DESC_BYTES);
        unsigned char *ba = NULL, *bb = NULL; long na = 0, nb = 0;
        int same = read_all(fa, &ba, &na) == 0 && read_all(fb, &bb, &nb) == 0 && na > 32 && na == nb && memcmp(ba, bb, (size_t)na) == 0;
        printf("tag/%-7s 完整路径=%d 带标签=%d 清单 %ld 字节 %s\n", k_tag_name[variant], r1, r2, na, same ? "逐字节相同" : "不同");
        if (r1 != 0 || r2 != 0 || !same) fail = 1;
        free(ba); free(bb);
    }
    {   // 诊断开 / 关两遍同一串 note，落盘逐字节相同（诊断不改行为）；放在坏文件段之后：它们共用的清单文件已经用完
        // 传上面拷下来的 base，不要传 getenv 返回的指针：diag_equiv 里 setenv 会改写 S2A_PW_DIR，macOS 的 setenv 会释放这个变量
        // 之前由 setenv 分配的旧值（走到这里时它已被上面的 setenv 重设过，不管父进程给没给目录），指针会悬空；glibc 不释放，所以 Linux 上看不出来。
        int rc = diag_equiv(argv[0], base);
        printf("%-8s %s\n", "diag_eq", rc == 0 ? "ok" : "FAIL");
        if (rc != 0) fail = 1;
    }
    printf("%s\n", fail ? "FAIL" : "PASS 主机管道：逐字段往返 / 去重 uses / 散列碰撞不去重 / 上限淘汰（含缺省 8192 与硬顶）/ 跨会话清单 / 坏文件整份拒绝 / 本局预热集冻结 / 程序借用不泄漏 / 并发保存不损坏 / 管线标签路径与完整路径清单逐字节相同（含淘汰、最近序号、重载、换程序身份）/ 颠簸与逐帧变化量诊断计数（含负对照、淘汰集合清空、并发守恒、日志与比对在清单锁外、缺省关且开关不改落盘）");
    return fail;
}
