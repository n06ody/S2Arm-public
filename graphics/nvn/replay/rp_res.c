#include "rp_res.h"
#include "rp_exec.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "nvn_hmap.h"
#include "nvn_dirty.h"
#include "nvn_fmt_info.h"
#include "nvn_deswizzle.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 一份后端贴图存储。可被多个贴图对象（序号）共用。
struct rp_tex {
    nvn_gpu_tex g;
    int users;                 // 指向它的序号个数
    uint8_t cpu;               // 内容来自游戏内存（按脏页重传）
    uint8_t gpu_written;       // 内容以 GPU 为准
    uint8_t uploaded;
    uint8_t need_init;         // 刚建的存储，初始内容还没定（init_content）
    uint8_t zero_pending;      // 初始内容该是全零、还没填：等第一个通道用清零载入做掉（rp_view_for_attach / rp_tex_take_zero）
    uint8_t in_alias;
    uint8_t check_dirty;       // 上次核对内容时判过脏、但没能传（没有段 / 上传失败）：下一次必须重查
    uint8_t _rsv[3];
    uint64_t seen;             // 共用表里的：最近一次被查到 / 新建时的提交号 + 1（alias_take_over 用）
    uint64_t wrote;            // 最近一次被 GPU 写时的提交号 + 1（0 = 没写过）
    uint64_t stamp;            // 最近一次上传取的脏页代号
    uint64_t up_sub;           // 最近一次上传所在的提交序号
    uint64_t check_epoch;      // 上次核对内容时的全局写代号（nvn_dirty_now）：等于当前值 = 那之后没有任何被跟踪的页被写过
    uint64_t check_seg;        // 上次核对所在的段号：换段（新一次提交）必重核一次
    uint64_t check_map;        // 上次核对时的映射代际（换底 / 重新登记后句柄与保护状态都要重验）
    const void *span;          // 跟踪区间句柄（nvn_dirty_span）：NULL = 这段内存没被写跟踪，只能每次保守查
    const uint8_t *mem; uint64_t size;
};
// 同存储共用表的键：内存池 + 偏移 + 描述（去掉只表示用途、不影响存储布局的标志）
typedef struct { uint64_t pool; int64_t off; nvn_texdesc d; } alias_key;
enum { ALIAS_USE_FLAGS = NVN_TEXTURE_FLAGS_IMAGE | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_ADAPTIVE_ZCULL | NVN_TEXTURE_FLAGS_DEPTH16_PREFER_FAST_CLEAR | NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL };
enum { SWZ_IDENTITY = 2 | 3 << 3 | 4 << 6 | 5 << 9 };   // tex_builder_t.swz 打包法下的 R G B A：等于没设
static void alias_key_of(alias_key *k, const tex_t *t) { memset(k, 0, sizeof *k); k->pool = (uint64_t)(uintptr_t)t->pool; k->off = t->off; k->d = t->d; k->d.flags &= ~(int)ALIAS_USE_FLAGS; }

typedef struct { tex_t t; view_t v; uint8_t ok, has_v; nvn_gpu_view view; struct rp_tex *of; uint64_t gen; } treg_t;
typedef struct { samp_par p; uint8_t ok; nvn_gpu_samp s; } sreg_t;

static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
static treg_t *g_treg; static int g_ntreg;
static uint64_t g_treg_gen;   // 登记表里「编译事实看得到的内容」每变一次加一（登记 / 注销 / 清空）；rp_tex_reg_gen 给着色器来源的快路径判缓存用
uint64_t rp_tex_reg_gen(void) { return __atomic_load_n(&g_treg_gen, __ATOMIC_ACQUIRE); }
static sreg_t *g_sreg; static int g_nsreg;
static uint32_t g_pin_depth;
struct pinned_samp { nvn_gpu_samp samp; struct pinned_samp *next; };
static struct pinned_samp *g_pinned_samps;
static int g_tpool_n;
static nvn_hmap g_by_serial = {.klen = 8};                  // 序号 → struct rp_tex *
static nvn_hmap g_alias = {.klen = sizeof(alias_key)};      // 键 → struct rp_tex *
static nvn_hmap g_cpu_ser = {.klen = 8};                    // 游戏用 WriteTexels 写过的序号（可能早于首次使用）
static nvn_hmap g_pool_gpu = {.klen = 8};                   // GPU 写过的内存池
static uint64_t g_ser_gen = 1;                              // 任何「序号 → 存储」映射变了就 +1：各贴图号缓存的视图据此重验
static int g_built; static uint64_t g_uploads, g_alias_taken, g_alias_dead;   // g_alias_taken：被新贴图占了存储而出表的共用存储数；g_alias_dead：其中还有对象指着的

// 声明类型的零贴图与默认视图：会话内共用，不参与视图环淘汰。
static struct { nvn_gpu_tex tex; nvn_gpu_view view; } g_zero[NVN_TEXTURE_TARGET_BUFFER + 1][4];
static int g_zero_cube_logged;

// ---------- 登记 ----------
static void treg_drop_view(treg_t *r) { if (r->view) { nvn_gpu_view_release(r->view); r->view = NULL; } r->of = NULL; }
void nvn_be_set_tex_pool_n(int n) { pthread_mutex_lock(&g_rmu); g_tpool_n = n; pthread_mutex_unlock(&g_rmu); }
void nvn_be_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id >= (1 << 20) || !t) return;
    pthread_mutex_lock(&g_rmu);
    if (id >= g_ntreg) { int n = id + 1024; treg_t *nr = realloc(g_treg, (size_t)n * sizeof *nr); if (!nr) { pthread_mutex_unlock(&g_rmu); return; }
        memset(nr + g_ntreg, 0, (size_t)(n - g_ntreg) * sizeof *nr); g_treg = nr; g_ntreg = n; }
    treg_t *r = &g_treg[id];
    int same = r->ok && r->t.serial == t->serial && r->has_v == (v != NULL) && (!v || !memcmp(&r->v, v, sizeof *v));
    if (!same) treg_drop_view(r);
    r->t = *t; r->has_v = v != NULL; if (v) r->v = *v; else memset(&r->v, 0, sizeof r->v); r->ok = 1;   // 只存快照：不建图、不建视图
    __atomic_fetch_add(&g_treg_gen, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_rmu);
}
void nvn_be_unreg_tex_range(int base, int n) {
    pthread_mutex_lock(&g_rmu);
    for (int i = base < 0 ? 0 : base; i < base + n && i < g_ntreg; i++) { treg_drop_view(&g_treg[i]); g_treg[i].ok = 0; }
    __atomic_fetch_add(&g_treg_gen, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_rmu);
}
void nvn_be_reg_samp(int id, const samp_par *p) {
    if (id < 0 || id >= (1 << 20) || !p) return;
    pthread_mutex_lock(&g_rmu);
    if (id >= g_nsreg) { int n = id + 256; sreg_t *nr = realloc(g_sreg, (size_t)n * sizeof *nr); if (!nr) { pthread_mutex_unlock(&g_rmu); return; }
        memset(nr + g_nsreg, 0, (size_t)(n - g_nsreg) * sizeof *nr); g_sreg = nr; g_nsreg = n; }
    sreg_t *r = &g_sreg[id];
    if (r->s) {
        if (g_pin_depth) {
            struct pinned_samp *pin = malloc(sizeof *pin);
            if (!pin) { nvn_unimpl("gpu", "sampler_pin_allocation", p); pthread_mutex_unlock(&g_rmu); return; }
            pin->samp = r->s; pin->next = g_pinned_samps; g_pinned_samps = pin;
        } else nvn_gpu_samp_release(r->s);
        r->s = NULL;
    }
    r->p = *p; r->ok = 1;
    __atomic_fetch_add(&g_treg_gen, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_rmu);
}

// 开关 nvk_tex_epoch（缺省开）：贴图内容检查按全局写代号只做一次。照后端的每 256 次重读形态（nvn_plat_flag 在正式包里只返回缺省值）。
static int tex_epoch_on(void) {
    static int v = -1; static unsigned k;
    if (v < 0 || !(++k & 255)) v = nvn_plat_flag("nvk_tex_epoch", 1) != 0;
    return v;
}

// ---------- 内容归属 ----------
void nvn_be_pool_gpu_written(const void *pool) { uint64_t k = (uint64_t)(uintptr_t)pool; pthread_mutex_lock(&g_rmu); nvn_hm_put(&g_pool_gpu, &k, (void *)1); pthread_mutex_unlock(&g_rmu); }
void nvn_be_tex_cpu_written(const tex_t *t) {
    if (!t || !t->serial) return;
    pthread_mutex_lock(&g_rmu);
    nvn_hm_put(&g_cpu_ser, &t->serial, (void *)1);
    struct rp_tex *x = nvn_hm_get(&g_by_serial, &t->serial);
    if (x) { x->cpu = 1; x->gpu_written = 0; x->uploaded = 0; x->check_epoch = 0; x->check_seg = 0; x->check_dirty = 1; }   // 游戏刚用 CPU 写了它：下次使用整张重传（纪元清零 = 强制重查）
    pthread_mutex_unlock(&g_rmu);
}
// 调试取证：这个序号的存储现在的归属（位 0 当 CPU 贴图、位 1 传过、位 2 GPU 写过、位 3 有存储），没建过 = 0
int rp_tex_debug_state(uint64_t serial) {
    pthread_mutex_lock(&g_rmu);
    struct rp_tex *x = nvn_hm_get(&g_by_serial, &serial); int r = x ? 8 | (x->cpu ? 1 : 0) | (x->uploaded ? 2 : 0) | (x->gpu_written ? 4 : 0) : 0;
    pthread_mutex_unlock(&g_rmu);
    return r;
}
int nvn_be_tex_gpu_owned(uint64_t serial) {
    pthread_mutex_lock(&g_rmu);
    struct rp_tex *x = nvn_hm_get(&g_by_serial, &serial); int r = x && x->gpu_written;
    pthread_mutex_unlock(&g_rmu);
    return r;
}
// 只读查询：这个序号被前端标过「CPU 写过」（nvn_be_tex_cpu_written 记下的）；同步读回（rp_entry.c）判内容在不在游戏内存里用，同 nvn_mtl.m 的 tex_cpu_written
int rp_tex_cpu_written(uint64_t serial) {
    pthread_mutex_lock(&g_rmu);
    int r = nvn_hm_get(&g_cpu_ser, &serial) != NULL;
    pthread_mutex_unlock(&g_rmu);
    return r;
}
// iOS 按页记 GPU 写的时刻（gpw_*，nvn_mtl.m:3560 一带）；这里先按整张贴图判，页级账随读回路径一起接
int nvn_be_tex_gpu_owned_since(uint64_t serial, uint64_t stamp) { (void)stamp; return nvn_be_tex_gpu_owned(serial); }
void rp_tex_mark_gpu_written(const tex_t *t) {
    if (!t || !t->serial) return;
    pthread_mutex_lock(&g_rmu);
    struct rp_tex *x = nvn_hm_get(&g_by_serial, &t->serial);
    if (x) { x->gpu_written = 1; x->cpu = 0; x->wrote = rp_sub_serial() + 1; }
    pthread_mutex_unlock(&g_rmu);
}

static int fmt_compressed(int f) { nvn_fmt_info i; return nvn_fmt_info_of((uint32_t)f, &i) && (i.bw > 1 || i.bh > 1); }
static int fmt_depth(int f) { nvn_fmt_info i; return nvn_fmt_info_of((uint32_t)f, &i) && (i.depth || i.stencil); }
static uint64_t res_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
// 有没有非零字节。按 8 字节一比：新建的渲染目标每张都要扫一遍自己的存储（常是几兆全 0），逐字节扫在真机上是每帧毫秒级。
static int mem_any(const uint8_t *p, uint64_t n) {
    uint64_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t v; memcpy(&v, p + i, 8); if (v) return 1; }
    for (; i < n; i++) if (p[i]) return 1;
    return 0;
}

// LINEAR 贴图（CPU 写入的字体 / UI 图集；iOS 同名语义见 nvn_mtl.m tex_linear @3756 / tex_linear_needs_upload @3777）。
// 返回 0 = 不是线性族；1 = 第一批准入（走 CPU 内容路径）；-1 = 线性族但第一批不收（*why 是未接名，调用方记账）。
// 准入只看排布 / 可访问性 / 真实来源，不看“现在有没有非零字节”——字体引擎可以建图之后才填字形。
static int linear_class(const tex_t *t, const char **why) {
    *why = NULL;
    if (!(t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_SPARSE))) return 0;
    if (!(t->d.flags & NVN_TEXTURE_FLAGS_LINEAR)) { *why = "linear_render_target_unsupported"; return -1; }   // 只有 LINEAR_RENDER_TARGET：内容通常归 GPU
    if (t->d.flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET) { *why = "linear_render_target_and_linear_unsupported"; return -1; }
    if (t->d.flags & NVN_TEXTURE_FLAGS_SPARSE) { *why = "linear_sparse_unsupported"; return -1; }
    if (!t->mem || !t->pool) { *why = "linear_no_cpu_memory"; return -1; }
    if (t->has_layout) { *why = "linear_packaged_unsupported"; return -1; }
    if (t->d.target != NVN_TEXTURE_TARGET_2D) { *why = "linear_target_not_2d"; return -1; }
    if (t->d.levels > 1) { *why = "linear_mips_unsupported"; return -1; }   // 公共布局只给单级的行距
    if (t->L.layers > 1) { *why = "linear_layers_unsupported"; return -1; }
    if (t->d.samples > 1) { *why = "linear_multisample_unsupported"; return -1; }
    { int bw = 1, bh = 1; if (!nvn_fmt_bpb(t->d.format, &bw, &bh) || bw != 1 || bh != 1) { *why = "linear_compressed_format_unsupported"; return -1; } }
    if (t->pool->flags & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL)) { *why = "linear_pool_not_cpu_accessible"; return -1; }
    if (!t->d.width || !t->d.height) { *why = "linear_empty_shape"; return -1; }
    if ((uint64_t)t->off + t->L.size > (uint64_t)t->pool->size) { *why = "linear_range_outside_pool"; return -1; }
    return 1;
}

// 首次使用时判「存储里是 CPU 写的数据」（nvn_mtl.m tex_cpu_content @3900）。持 g_rmu 调。
static int cpu_content(const tex_t *t) {
    if (!t->mem || !t->pool || t->d.samples > 1 || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return 0;
    if (t->has_layout || nvn_hm_get(&g_cpu_ser, &t->serial)) return 1;   // 随包贴图 / WriteTexels 写过
    uint64_t pk = (uint64_t)(uintptr_t)t->pool;
    int unc = !fmt_compressed(t->d.format) && t->d.target != NVN_TEXTURE_TARGET_3D;
    if (unc && (fmt_depth(t->d.format) || (t->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_VIDEO_DECODE)) || nvn_hm_get(&g_pool_gpu, &pk))) return 0;
    if (t->pool->flags & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL)) return 0;
    { const char *why = NULL; int lc = linear_class(t, &why);
        if (lc > 0) return 1;                 // 线性准入：初始全零也进入持续观察（首用之后 CPU 再写也会被脏页跟踪接住）
        if (lc < 0) {
            if (!strcmp(why, "linear_render_target_unsupported")) return 0;   // 只有 LINEAR_RENDER_TARGET：内容归 GPU（可能由 copy_b2t 写入），不按 CPU 内容传、不报红
            nvn_unimpl("tex", why, t); return 0; } }   // 线性族里第一批不收的：准确报红，不猜内容
    if ((t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE) || ((t->d.flags & NVN_TEXTURE_FLAGS_IMAGE) && !fmt_compressed(t->d.format))) return 0;
    return mem_any(t->mem, t->L.size);
}

// ---------- 上传：块线性存储 → 逐级逐层线性 → 后端 ----------
// track：1 = CPU 贴图（先保护、记代号，之后脏了重传）；0 = 一次性的初始内容，不跟踪写
static int upload_all(const tex_t *t, struct rp_tex *x, nvn_gpu_cmd c, int track) {
    rp_perf_add(RP_PERF_TEX_UP_N, 1); rp_perf_add(RP_PERF_TEX_UP_KB, t->L.size / 1024);
    { const char *why = NULL; int lc = linear_class(t, &why);   // 线性：走下面 nvn_deswizzle 的逐行拷贝（源行距由公共布局解释）
        if (lc < 0) { nvn_unimpl("tex", why, t); return -1; }
        if (lc > 0 && (!t->pool || (uint64_t)t->off + t->L.size > (uint64_t)t->pool->size)) { nvn_unimpl("tex", "linear_range_outside_pool", t); return -1; }
        // 非线性的布局也要落在这个池子里：真机上游戏给的布局与池边界不一致时，照原样 deswizzle 会读到池外（宿主用例里 128x64x64
        // 的三维贴图就把 1 MiB 的池读到段错误）。宁可报未接，也不越界读。
        if (t->pool && (uint64_t)t->off + t->L.size > (uint64_t)t->pool->size) { nvn_unimpl("tex", "tex_range_outside_pool", t); return -1; } }
    int bw = 1, bh = 1; int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) { nvn_unimpl("fmt", "upload_unknown_format", t); return -1; }
    uint64_t st = 0;
    if (track) { nvn_dirty_protect(t->mem, t->L.size); st = nvn_dirty_now(); x->span = nvn_dirty_span(t->mem, t->L.size); }   // 先保护、取代号、再读：取代号之后的写下次查得到
    uint32_t levels = t->d.levels ? t->d.levels : 1, layers = t->L.layers ? t->L.layers : 1;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    uint8_t *tmp = NULL; size_t cap = 0; int rc = 0;
    for (uint32_t l = 0; l < levels && !rc; l++) {
        uint32_t w = t->d.width >> l, h = t->d.height >> l, dp = is3d ? t->d.depth >> l : 1;
        if (!w) w = 1;
        if (!h) h = 1;
        if (!dp) dp = 1;
        size_t pitch = (size_t)((w + (uint32_t)bw - 1) / (uint32_t)bw) * (size_t)bpb, rows = (h + (uint32_t)bh - 1) / (uint32_t)bh, img = pitch * rows * dp;
        if (img > cap) { uint8_t *n = realloc(tmp, img); if (!n) { rc = -1; break; } tmp = n; cap = img; }
        for (uint32_t y = 0; y < layers; y++) {
            if (nvn_deswizzle(&t->d, l, y, t->mem, tmp, pitch)) { nvn_unimpl("tex", "deswizzle_failed", t); rc = -1; break; }
            NVNcopyRegion r = {0, 0, 0, (int)w, (int)h, (int)dp};
            nvn_gpu_tex_upload(c, x->g, l, y, &r, tmp, pitch, img);
        }
    }
    free(tmp);
    if (!rc && track) { x->stamp = st; x->uploaded = 1; g_uploads++; }
    return rc;
}

// ---------- 新建存储的初始内容（nvn_mtl.m tex_init_fill @3670、tex_zero_fill @3345 / @4297） ----------
// Switch 上贴图就是内存池里那段内存：建贴图之前池里放着什么，贴图一开始就是什么。游戏把文件读进 CPU 不能访问的池再在上面建贴图，
// 之后不会再有任何通知——不在建图时取一次，这类贴图永远是空的。取不了 / 不该取的，新图清零（宿主的新图内容没有定义）。
// 与 iOS 的差：没有按页的 GPU 写账（gpw_stale：别的贴图的 GPU 写盖过、客体内存里已经过时的那些字节不取），这里整段照取；
// 判全 0 时不跳过游戏没碰过的页（iOS mem_any_touched 靠 mincore；安卓上换出的页同样报不在内存，不能当 0）。
static int fmt_nonfinite(const tex_t *t) {   // 浮点格式的存储里有 Inf / NaN（iOS fill_nonfinite）
    const uint8_t *p = t->mem; uint64_t n;
    switch (t->d.format) {
    case NVN_FORMAT_R11G11B10F: n = t->L.size / 4; for (uint64_t i = 0; i < n; i++) { uint32_t v; memcpy(&v, p + i * 4, 4); if (((v >> 6) & 31) == 31 || ((v >> 17) & 31) == 31 || ((v >> 27) & 31) == 31) return 1; } return 0;
    case NVN_FORMAT_R16F: case NVN_FORMAT_RG16F: case NVN_FORMAT_RGBA16F: n = t->L.size / 2; for (uint64_t i = 0; i < n; i++) { uint16_t v; memcpy(&v, p + i * 2, 2); if (((v >> 10) & 31) == 31) return 1; } return 0;
    case NVN_FORMAT_R32F: case NVN_FORMAT_RG32F: case NVN_FORMAT_RGBA32F: n = t->L.size / 4; for (uint64_t i = 0; i < n; i++) { uint32_t v; memcpy(&v, p + i * 4, 4); if (((v >> 23) & 255) == 255) return 1; } return 0;
    default: return 0;
    }
}
static int init_from_storage(const tex_t *t) {
    if (!t->mem || !t->pool || (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) || t->has_layout) return 0;
    if (t->d.samples > 1 || t->d.target == NVN_TEXTURE_TARGET_BUFFER || fmt_depth(t->d.format)) return 0;
    int no_cpu = (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) != 0;
    if (no_cpu && (t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE)) return 0;   // CPU 不能访问的池里带压缩提示的贴图：那段内存对 CPU 可能没有意义
    if (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_SPARSE)) return 0;   // 线性 / 稀疏的上传还没接（upload_all 记账）
    if (!mem_any(t->mem, t->L.size)) return 0;
    if (no_cpu && fmt_nonfinite(t)) return 0;   // 游戏写不了这种池：里面的 Inf / NaN 是别人留下的，不是数据
    return 1;
}
static uint64_t g_new_sizes[64]; static int g_new_nsizes;   // 诊断读数：这一窗口里新建贴图的宽高（只在提交线程记，呈现线程取时可能少看一条，不要紧）
static void new_size_note(uint32_t w, uint32_t h) { uint64_t k = (uint64_t)w << 32 | h; for (int i = 0; i < g_new_nsizes; i++) if (g_new_sizes[i] == k) return; if (g_new_nsizes < 64) g_new_sizes[g_new_nsizes++] = k; }
int rp_res_new_sizes_take(void) { int n = g_new_nsizes; g_new_nsizes = 0; return n; }
static _Thread_local int g_attach_intent;   // 正在给颜色附件取视图（rp_view_for_attach）
// 填零能不能交给第一个通道的清零载入：zero_fill 会填的格式里，只有一级一层的二维图（一个附件视图盖住整张图）
static int zero_deferrable(const tex_t *t) {
    int bw = 1, bh = 1; int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb || bw != 1 || bh != 1 || fmt_depth(t->d.format) || t->d.samples > 1) return 0;
    return t->d.target == NVN_TEXTURE_TARGET_2D && t->d.levels <= 1 && t->L.layers <= 1;
}
static void zero_fill(const tex_t *t, struct rp_tex *x, nvn_gpu_cmd c) {
    int bw = 1, bh = 1; int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb || bw != 1 || bh != 1 || fmt_depth(t->d.format) || t->d.samples > 1 || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return;   // 压缩 / 深度模板 / 多重采样的不清（同 iOS）
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    uint32_t w0 = t->d.width ? (uint32_t)t->d.width : 1, h0 = t->d.height ? (uint32_t)t->d.height : 1, d0 = is3d && t->d.depth ? (uint32_t)t->d.depth : 1;
    size_t need = (size_t)w0 * (size_t)bpb * h0 * d0;   // 第 0 级一层最大
    if (need > (64u << 20)) return;
    uint8_t *z = calloc(1, need); if (!z) return;
    uint32_t levels = t->d.levels ? t->d.levels : 1, layers = t->L.layers ? t->L.layers : 1;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t w = w0 >> l, h = h0 >> l, dp = d0 >> l;
        if (!w) w = 1;
        if (!h) h = 1;
        if (!dp) dp = 1;
        size_t pitch = (size_t)w * (size_t)bpb;
        NVNcopyRegion r = {0, 0, 0, (int)w, (int)h, (int)dp};
        for (uint32_t y = 0; y < layers; y++) nvn_gpu_tex_upload(c, x->g, l, y, &r, z, pitch, pitch * h * dp);
    }
    free(z);
}

static uint32_t usage_of(const tex_t *t, int cpu) {
    uint32_t caps = nvn_gpu_format_caps((uint32_t)t->d.format), u = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_VIEW_FMT;
    // Vulkan 的用途事后补不了，Metal 那边是用到时再升级（tex_upgrade）：能当附件的格式一律带上附件用途
    if ((caps & (NVN_GPU_FMT_RENDER | NVN_GPU_FMT_DEPTH)) && !fmt_compressed(t->d.format)) u |= NVN_GPU_TEX_RENDER;
    if ((t->d.flags & NVN_TEXTURE_FLAGS_IMAGE) && (caps & NVN_GPU_FMT_STORAGE)) u |= NVN_GPU_TEX_STORAGE;
    if (cpu) u |= NVN_GPU_TEX_CPU_UPLOAD;
    return u;
}

static void tex_unref(struct rp_tex *x) {
    if (--x->users > 0 || x->in_alias) return;   // 共用表里的存储由表持有：池销毁时才放（nvn_be_alias_drop_pool）
    nvn_gpu_tex_release(x->g); g_built--; free(x);
}

// 新的共用存储占了同一段池内存时，放掉被它盖住的旧条目。照 iOS nvn_mtl.m 的 alias_take_over，两条规则：
//   ① 没有贴图对象指着（users == 0）：同池、存储区间重叠、描述不同、闲置满 ALIAS_IDLE 次提交 → 放。在 NVN 上那段内存现在归新贴图，
//      旧的那张只是没人再能叫到的一份拷贝。闲置窗口保住「每帧 Finalize 再按同描述重建」的瞬态目标（靠同描述再查到同一份）。
//   ② 还有对象指着：它自己最近 ALIAS_IDLE 次提交里没被 GPU 写过，并且在它最后一次被用 / 被写之后，另一张与它存储重叠的贴图被 GPU 写了
//      → 也放（Switch 上几张贴图建在同一段内存就共用那些字节，别的贴图写过去，它的内容就没了）。对象下次再用时按新存储重建，
//      初始内容照新建存储的规矩取。
// 不搬的后果（真机，一款带动态分辨率的 3D 游戏）：十来个渲染目标在同一批偏移上每帧换一个宽高（1617x910、1604x902、1607x904…），
// 每个宽高是一个并存的贴图对象、表里一个新条目，只进不出——标题画面一分钟后端贴图 297 → 2299 张、显存 8.7 GB，手机内存吃光、游戏被杀。
// iOS 的同类事故：一款游戏的阴影图几乎每帧换尺寸，同一偏移 429 张深度贴图。持 g_rmu。
enum { ALIAS_IDLE = 16 };
static void vf_drop_of(const struct rp_tex *x);   // 按目标取视图的表里指着这份存储的项（定义在那张表后面）
static int alias_overwritten(const alias_key *a, const struct rp_tex *self, uint64_t since) {   // 与 a 存储重叠的别的条目里，有没有在 since 之后被 GPU 写过的
    const int64_t lo = a->off, hi = a->off + (int64_t)self->size;
    for (uint32_t i = 0; i < g_alias.cap; i++) {
        const struct rp_tex *y = g_alias.vals[i];
        if (!y || y == self) continue;
        alias_key k; memcpy(&k, g_alias.keys + (size_t)i * g_alias.klen, sizeof k);
        if (k.pool != a->pool || k.off >= hi || k.off + (int64_t)y->size <= lo) continue;
        if (y->wrote > since) return 1;
    }
    return 0;
}
static void alias_take_over(const alias_key *nk, uint64_t nsize) {
    if (!nsize) return;
    const int64_t lo = nk->off, hi = nk->off + (int64_t)nsize;
    const uint64_t now = rp_sub_serial() + 1;
    for (int again = 1; again;) {
        again = 0;
        for (uint32_t i = 0; i < g_alias.cap; i++) {
            struct rp_tex *x = g_alias.vals[i];
            if (!x) continue;
            alias_key k; memcpy(&k, g_alias.keys + (size_t)i * g_alias.klen, sizeof k);
            if (k.pool != nk->pool || !memcmp(&k, nk, sizeof k) || k.off >= hi || k.off + (int64_t)x->size <= lo) continue;
            if (x->seen && now - x->seen < ALIAS_IDLE) continue;
            if (x->users > 0) {   // ②
                const uint64_t last = x->seen > x->wrote ? x->seen : x->wrote;
                if ((x->wrote && now - x->wrote < ALIAS_IDLE) || !alias_overwritten(&k, x, last)) continue;
                for (int more = 1; more;) {   // 指着它的序号都出表：对象下次用到时重建
                    more = 0;
                    for (uint32_t q = 0; q < g_by_serial.cap; q++) if (g_by_serial.vals[q] == x) {
                        uint64_t sk; memcpy(&sk, g_by_serial.keys + (size_t)q * g_by_serial.klen, sizeof sk);
                        nvn_hm_del(&g_by_serial, &sk); nvn_hm_del(&g_cpu_ser, &sk); more = 1; break; }
                }
                x->users = 0; g_alias_dead++;
            }
            nvn_hm_del(&g_alias, &k);   // 删除会挪动别的格：从头再扫
            vf_drop_of(x);
            for (int r = 0; r < g_ntreg; r++) if (g_treg[r].of == x) treg_drop_view(&g_treg[r]);
            nvn_gpu_tex_release(x->g); g_built--; free(x); g_ser_gen++; g_alias_taken++;
            again = 1; break;
        }
    }
}

nvn_gpu_tex rp_tex_resolve(const tex_t *t, nvn_gpu_cmd c) {
    if (!t || !t->serial) return NULL;
    uint64_t r0_ = res_now();
    pthread_mutex_lock(&g_rmu);
    uint64_t r1_ = res_now(); rp_perf_add(RP_PERF_RES_LOCK, r1_ - r0_);
    struct rp_tex *x = nvn_hm_get(&g_by_serial, &t->serial);
    if (!x) {
        rp_perf_add(RP_PERF_RES_NEW, 1); new_size_note(t->d.width, t->d.height);
        int cpu = cpu_content(t);
        alias_key k; alias_key_of(&k, t);
        if (!cpu && t->pool) { x = nvn_hm_get(&g_alias, &k); if (x) x->seen = rp_sub_serial() + 1; }   // 同存储同描述的另一个对象已经建过：共用
        if (!x) {
            nvn_gpu_tex_desc d = {0}; d.d = t->d; d.usage = usage_of(t, cpu); d.swz = t->set_swz && t->swz != SWZ_IDENTITY ? t->swz : 0; d.stencil_view = t->stencil;
            nvn_gpu_tex g = NULL; nvn_gpu_err err = {0};
            uint64_t mk0_ = res_now(); nvn_gpu_rc mkr_ = nvn_gpu_tex_create(&d, &g, &err); rp_perf_add(RP_PERF_RES_MAKE, res_now() - mk0_);
            if (!nvn_gpu_ok(mkr_)) {
                pthread_mutex_unlock(&g_rmu);
                char name[48]; snprintf(name, sizeof name, "tex_create_fmt_%#x_target_%d", (unsigned)t->d.format, t->d.target); if (nvn_unimpl("gpu", name, t) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", err.why);   // 首次：带上后端给的原因
                return NULL;
            }
            x = calloc(1, sizeof *x); x->g = g; x->cpu = (uint8_t)cpu; x->mem = t->mem; x->size = t->L.size; x->need_init = 1; g_built++;
            if (!cpu && t->pool) { alias_take_over(&k, t->L.size); nvn_hm_put(&g_alias, &k, x); x->in_alias = 1; x->seen = rp_sub_serial() + 1; }
        }
        x->users++;
        nvn_hm_put(&g_by_serial, &t->serial, x); g_ser_gen++;
    }
    if (x->need_init && c) {   // 刚建的存储：CPU 贴图走下面的上传；其余的取一次存储里现有的内容，取不了的清零
        x->need_init = 0;
        if (!x->cpu && !x->gpu_written) {
            nvn_gpu_cmd xc = rp_xfer_cmd(&x->up_sub);
            if (xc) {   // 三段分别计时：扫存储定来源 / 整张上传 / 整张填零
                uint64_t i0_ = res_now(); int from = init_from_storage(t); uint64_t i1_ = res_now(); rp_perf_add(RP_PERF_RES_SCAN, i1_ - i0_);
                int zero = !from;
                if (from) { zero = upload_all(t, x, xc, 0); uint64_t i2_ = res_now(); rp_perf_add(RP_PERF_RES_UP, i2_ - i1_); rp_perf_add(RP_PERF_RES_UPN, 1); i1_ = i2_; }
                if (zero && g_attach_intent && zero_deferrable(t)) { x->zero_pending = 1; rp_perf_add(RP_PERF_RES_ZDEFER, 1); }
                else if (zero) { zero_fill(t, x, xc); rp_perf_add(RP_PERF_RES_ZERO, res_now() - i1_); rp_perf_add(RP_PERF_RES_ZERON, 1); }
            }
        }
    }
    if (x->zero_pending && !g_attach_intent && c) {   // 待清零没被通道取走（开通道失败），现在有别的用途要它：照旧填零
        nvn_gpu_cmd xc = rp_xfer_cmd(&x->up_sub);
        if (xc) { x->zero_pending = 0; if (!x->gpu_written) { zero_fill(t, x, xc); rp_perf_add(RP_PERF_RES_ZERON, 1); } }
    }
    if (x->cpu && !x->gpu_written && c && t->mem) {
        // 内容核对按「全局写代号」只做一次（同 rp_buf.c 的页核对快路）：代号没动 = 上次核对之后游戏没写过任何被跟踪的页。
        // 段号进键：换段（新一次提交）必重核一次——「一次提交里游戏不能改 GPU 正在读的贴图」只对一次提交成立。
        // 映射代际进键：换底 / 重新登记后旧句柄与保护状态都不作数（同 rp_buf 的 seen_map）。
        const uint64_t now0 = nvn_dirty_now(), map0 = nvn_dirty_mapping_generation(), seg0 = rp_seg_serial();
        if (tex_epoch_on() && x->uploaded && x->span && !x->check_dirty && x->check_epoch == now0 && x->check_seg == seg0 && x->check_map == map0
            && !nvn_dirty_tracking_unreliable()) {
            rp_perf_add(RP_PERF_TEX_EPOCH, 1);
        } else {
            x->span = nvn_dirty_span(t->mem, t->L.size);   // 换底 / 重新登记后重取句柄（NULL = 不再被跟踪，照旧每次都查）
            if (!x->uploaded || x->check_dirty || nvn_dirty_since(t->mem, t->L.size, x->stamp)) {
                nvn_gpu_cmd xc = rp_xfer_cmd(&x->up_sub);   // 上传是传输：录进前置段（本次提交里第二次上传才回主段）
                if (xc && upload_all(t, x, xc, 1) == 0) { x->check_epoch = now0; x->check_seg = seg0; x->check_map = map0; x->check_dirty = 0; }
                else x->check_dirty = 1;
            } else { x->check_epoch = now0; x->check_seg = seg0; x->check_map = map0; x->check_dirty = 0; }
        }
    }
    nvn_gpu_tex g = x->g;
    rp_perf_add(RP_PERF_RES_INIT, res_now() - r1_);   // 持锁做的全部事（查表 / 新建 / 初始化 / 上传）
    pthread_mutex_unlock(&g_rmu);
    return g;
}

// ---------- 按（贴图对象，视图记录）取视图：渲染目标、拷贝端点这些不经贴图号的使用点 ----------
// 表是 128 组 x 8 路：按（贴图序号，视图记录）散列到一组，只在这一组的 8 格里找和换。
// 原先是 64 格的环、逐格比：一帧一百多个通道的附件加拷贝端点远超 64 种，环不停轮转，真机一款 3D 游戏每帧新建 228 个后端视图
// （对象读数），后端按视图记的帧缓冲也跟着每帧重建。
enum { VF_WAYS = 8, VF_N = 128 * VF_WAYS };
static uint32_t vf_base(uint64_t serial, const view_t *v) {
    uint64_t h = serial * 0x9E3779B97F4A7C15ull;
    if (v) { const uint8_t *p = (const uint8_t *)v; for (size_t i = 0; i < sizeof *v; i++) h = (h ^ p[i]) * 0x100000001B3ull; }
    return (uint32_t)((h >> 20) % (VF_N / VF_WAYS)) * VF_WAYS;
}
static struct { uint64_t serial; view_t v; uint8_t has_v; nvn_gpu_view view; struct rp_tex *of; } g_vf[VF_N];
static int g_vf_next;
static uint32_t g_vf_seen[VF_N];       // 本轮取用记号：等于 g_pin_token 的项已交给调用方，轮转不能放
static uint32_t g_pin_token;
// 窗口内取用数超过环容量时的溢出视图（生产上界：8 颜色 + 深度 + 影子 ≤ 12；这里只是防御）。
// 不进环、由窗口结束时统一放：后端在 pass_begin 里已 cmd_ref，窗口之后不再需要核心持有。
static struct { nvn_gpu_view view; } g_pin_tmp[4096];
static int g_npin_tmp;
void rp_view_pin_begin(void) { pthread_mutex_lock(&g_rmu); if (g_pin_depth++ == 0) g_pin_token++; pthread_mutex_unlock(&g_rmu); }
void rp_view_pin_end(void) {
    pthread_mutex_lock(&g_rmu);
    if (g_pin_depth > 0 && --g_pin_depth == 0) {
        g_pin_token++;
        for (int i = 0; i < g_npin_tmp; i++) if (g_pin_tmp[i].view) nvn_gpu_view_release(g_pin_tmp[i].view);
        g_npin_tmp = 0;
        while (g_pinned_samps) {
            struct pinned_samp *pin = g_pinned_samps; g_pinned_samps = pin->next;
            nvn_gpu_samp_release(pin->samp); free(pin);
        }
    }
    pthread_mutex_unlock(&g_rmu);
}
static void vf_drop_of(const struct rp_tex *x) { for (int i = 0; i < VF_N; i++) if (g_vf[i].view && g_vf[i].of == x) { nvn_gpu_view_release(g_vf[i].view); memset(&g_vf[i], 0, sizeof g_vf[i]); } }   // 持 g_rmu
static void vf_drop_serial(uint64_t serial) {   // 持 g_rmu
    for (int i = 0; i < VF_N; i++) if (g_vf[i].view && (!serial || g_vf[i].serial == serial)) { nvn_gpu_view_release(g_vf[i].view); memset(&g_vf[i], 0, sizeof g_vf[i]); }
}
nvn_gpu_view rp_view_for_attach(const tex_t *t, const view_t *v, nvn_gpu_cmd c) { g_attach_intent = 1; nvn_gpu_view o = rp_view_for(t, v, c); g_attach_intent = 0; return o; }
int rp_tex_take_zero(const tex_t *t) {
    if (!t || !t->serial) return 0;
    pthread_mutex_lock(&g_rmu);
    struct rp_tex *x = nvn_hm_get(&g_by_serial, &t->serial);
    int r = x && x->zero_pending;
    if (r) x->zero_pending = 0;
    pthread_mutex_unlock(&g_rmu);
    return r;
}
nvn_gpu_view rp_view_for(const tex_t *t, const view_t *v, nvn_gpu_cmd c) {
    nvn_gpu_tex g = rp_tex_resolve(t, c);
    if (!g) return NULL;
    pthread_mutex_lock(&g_rmu);
    struct rp_tex *x = nvn_hm_get(&g_by_serial, &t->serial);
    nvn_gpu_view out = NULL; int slot = -1, hit = -1;
    const uint32_t base = vf_base(t->serial, v);
    for (int i = (int)base; i < (int)base + VF_WAYS && x; i++) if (g_vf[i].view && g_vf[i].serial == t->serial && g_vf[i].has_v == (v != NULL) && (!v || !memcmp(&g_vf[i].v, v, sizeof *v))) {
        hit = i;
        if (g_vf[i].of == x) out = g_vf[i].view; else slot = i;   // 序号换了存储：这一项作废重建
        break; }
    if (hit >= 0 && g_pin_depth > 0) g_vf_seen[hit] = g_pin_token;   // 本轮已交给调用方：轮转不许放
    if (!out && x) {
        // 没找到同项：挑一个本轮没钉住的槽（钉住 = 已交给调用方、还没交给后端）
        int ring = 1;
        if (slot < 0) {
            uint32_t start = (uint32_t)g_vf_next++; int pick = -1;
            for (uint32_t k = 0; k < VF_WAYS && pick < 0; k++) if (!g_vf[base + k].view) pick = (int)(base + k);   // 先用空格
            for (uint32_t k = 0; k < VF_WAYS && pick < 0; k++) { uint32_t i = base + (start + k) % VF_WAYS; if (g_pin_depth <= 0 || g_vf_seen[i] != g_pin_token) pick = (int)i; }
            if (pick >= 0) slot = pick;
            else ring = 0;   // 这一组 8 格都在本轮钉住：走溢出表，不动表
        }
        nvn_gpu_err err = {0};
        if (!ring) {
            nvn_gpu_view tv = NULL;
            if (nvn_gpu_ok(nvn_gpu_view_create(x->g, v, &tv, &err)) && g_npin_tmp < (int)(sizeof g_pin_tmp / sizeof g_pin_tmp[0])) { g_pin_tmp[g_npin_tmp++].view = tv; out = tv; }
            else { if (tv) nvn_gpu_view_release(tv); if (nvn_unimpl("gpu", "view_pin_overflow", t) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", err.why); }
        } else {
        if (g_vf[slot].view) nvn_gpu_view_release(g_vf[slot].view);
        memset(&g_vf[slot], 0, sizeof g_vf[slot]);
        rp_perf_add(RP_PERF_RES_VIEWMAKE, 1);
        if (nvn_gpu_ok(nvn_gpu_view_create(x->g, v, &g_vf[slot].view, &err))) { g_vf[slot].serial = t->serial; g_vf[slot].has_v = v != NULL; if (v) g_vf[slot].v = *v; g_vf[slot].of = x; out = g_vf[slot].view; if (g_pin_depth > 0) g_vf_seen[slot] = g_pin_token; }
        else { g_vf[slot].view = NULL; if (nvn_unimpl("gpu", "view_create", t) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", err.why); }
        }
    }
    pthread_mutex_unlock(&g_rmu);
    return out;
}

nvn_gpu_view rp_present_view(const tex_t *t) {
    nvn_gpu_tex g = rp_tex_resolve(t, NULL);
    if (!g) return NULL;
    nvn_gpu_view v = NULL; nvn_gpu_err err = {0};
    if (!nvn_gpu_ok(nvn_gpu_view_create(g, NULL, &v, &err))) {
        if (nvn_unimpl("gpu", "view_create", t) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", err.why);
        return NULL;
    }
    return v;
}

// 贴图对象 Finalize（rp_entry.c nvn_be_tex_release 调）：序号不再指向这份存储
void rp_res_tex_gone(uint64_t serial) {
    pthread_mutex_lock(&g_rmu);
    vf_drop_serial(serial);
    struct rp_tex *x = nvn_hm_del(&g_by_serial, &serial);
    nvn_hm_del(&g_cpu_ser, &serial);
    if (x) { tex_unref(x); g_ser_gen++; }
    pthread_mutex_unlock(&g_rmu);
}

// 内存池 Finalize：建在这个池上的共用存储全部出表（nvn_mtl.m nvn_be_alias_drop_pool @2408）
void nvn_be_alias_drop_pool(const void *pool) {
    uint64_t pk = (uint64_t)(uintptr_t)pool;
    pthread_mutex_lock(&g_rmu);
    nvn_hm_del(&g_pool_gpu, &pk);
    for (int again = 1; again;) { again = 0;
        for (uint32_t i = 0; i < g_alias.cap; i++) { if (!g_alias.vals[i]) continue;
            alias_key k; memcpy(&k, g_alias.keys + (size_t)i * sizeof k, sizeof k); if (k.pool != pk) continue;
            struct rp_tex *x = nvn_hm_del(&g_alias, &k); x->in_alias = 0;
            if (x->users <= 0) { nvn_gpu_tex_release(x->g); g_built--; free(x); }
            again = 1; break; } }   // 删除会挪表项：从头再扫
    g_ser_gen++;
    pthread_mutex_unlock(&g_rmu);
}

nvn_gpu_view rp_view_by_id(int id, nvn_gpu_cmd c, const tex_t **t_out) {
    pthread_mutex_lock(&g_rmu);
    if (id < 0 || id >= g_ntreg || !g_treg[id].ok || (g_tpool_n && id >= g_tpool_n)) { pthread_mutex_unlock(&g_rmu); return NULL; }
    tex_t t = g_treg[id].t; view_t v = g_treg[id].v; int has_v = g_treg[id].has_v;
    pthread_mutex_unlock(&g_rmu);
    nvn_gpu_tex g = rp_tex_resolve(&t, c);   // 每次都过一遍：CPU 内容脏了要在这一段里重传
    if (!g) return NULL;
    pthread_mutex_lock(&g_rmu);
    nvn_gpu_view out = NULL;
    if (id < g_ntreg && g_treg[id].ok && g_treg[id].t.serial == t.serial) {
        treg_t *r = &g_treg[id];
        struct rp_tex *x = nvn_hm_get(&g_by_serial, &t.serial);
        if (r->view && (r->gen != g_ser_gen) && r->of != x) treg_drop_view(r);   // 序号换了存储：旧视图作废
        if (!r->view && x) {
            nvn_gpu_err err = {0};
            if (nvn_gpu_ok(nvn_gpu_view_create(x->g, has_v ? &v : NULL, &r->view, &err))) r->of = x;
            else { r->view = NULL; nvn_unimpl("gpu", "view_create", r); }
        }
        r->gen = g_ser_gen; out = r->view;
        if (t_out) *t_out = &r->t;
    }
    pthread_mutex_unlock(&g_rmu);
    return out;
}

// 后端建图内容未定义，首次取用在前置传输段逐层上传零。
static nvn_gpu_view decl_zero(uint8_t dtt, uint8_t dcls, nvn_gpu_cmd c) {
    if (!dtt || dtt > NVN_TEXTURE_TARGET_BUFFER + 1 || dcls > 3) return NULL;
    int want = dtt - 1;
    if (g_zero[want][dcls].view) return g_zero[want][dcls].view;
    if (!c) return NULL;
    uint64_t sub = 0;
    nvn_gpu_cmd xc = rp_xfer_cmd(&sub);
    if (!xc) return NULL;
    nvn_gpu_tex_desc d = {0};
    static const int formats[4] = {NVN_FORMAT_RGBA8, NVN_FORMAT_R32UI, NVN_FORMAT_R32I, NVN_FORMAT_DEPTH32F};
    d.d.target = want; d.d.format = formats[dcls];
    d.d.width = d.d.height = d.d.levels = 1;
    d.d.depth = want == NVN_TEXTURE_TARGET_CUBEMAP || want == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY ? 6 : 1;
    d.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD;
    nvn_gpu_tex tex = NULL; nvn_gpu_view view = NULL; nvn_gpu_err err = {0};
    if (!nvn_gpu_ok(nvn_gpu_tex_create(&d, &tex, &err))) {
        if (nvn_unimpl("tex", "decl_zero_create", NULL) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "tex", err.why);
        return NULL;
    }
    const uint32_t zero = 0;
    NVNcopyRegion r = {0, 0, 0, 1, 1, 1};
    for (uint32_t layer = 0; layer < d.d.depth; layer++) nvn_gpu_tex_upload(xc, tex, 0, layer, &r, &zero, 4, 4);
    // 视图带显式目标：6 层的立方体数组存储默认会被后端按面数配成立方体，声明是立方体数组时类型就又对不上了
    view_t zv; memset(&zv, 0, sizeof zv);
    zv.set_target = 1; zv.target = (uint8_t)want; zv.set_layers = 1; zv.nlayers = (uint16_t)d.d.depth;
    if (!nvn_gpu_ok(nvn_gpu_view_create(tex, &zv, &view, &err))) {
        nvn_gpu_tex_release(tex);
        if (nvn_unimpl("tex", "decl_zero_view", NULL) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "tex", err.why);
        return NULL;
    }
    g_zero[want][dcls].tex = tex; g_zero[want][dcls].view = view;
    return view;
}

static nvn_gpu_view view_by_id_decl(int id, uint8_t dtt, uint8_t dcls, nvn_gpu_cmd c, const tex_t **t_out);
nvn_gpu_view rp_view_by_id_decl(int id, uint8_t dtt, uint8_t dcls, int elem, nvn_gpu_cmd c, const tex_t **t_out) {
    if (!dtt) return rp_view_by_id(id, c, t_out);
    nvn_gpu_view out = view_by_id_decl(id, dtt, dcls, c, t_out);
    // 贴图数组里取不到的元素：描述符数组每项都要有效，用声明类型的零贴图顶上（类型与声明一致，不再拿别的元素凑）
    if (!out && elem) out = decl_zero(dtt, dcls, c);
    return out;
}
static nvn_gpu_view view_by_id_decl(int id, uint8_t dtt, uint8_t dcls, nvn_gpu_cmd c, const tex_t **t_out) {
    if (t_out) *t_out = NULL;
    pthread_mutex_lock(&g_rmu);
    if (id < 0 || id >= g_ntreg || !g_treg[id].ok || (g_tpool_n && id >= g_tpool_n)) {
        pthread_mutex_unlock(&g_rmu);
        return NULL;
    }
    tex_t t = g_treg[id].t; view_t v = g_treg[id].v; int has_v = g_treg[id].has_v;
    if (t_out) *t_out = &g_treg[id].t;
    pthread_mutex_unlock(&g_rmu);
    int want = dtt - 1, have = has_v && v.set_target ? v.target : t.d.target;
    if (want == have) return rp_view_by_id(id, c, t_out);
    uint32_t layers = t.d.target == NVN_TEXTURE_TARGET_CUBEMAP ? 6 :
        t.d.target == NVN_TEXTURE_TARGET_2D_ARRAY || t.d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY ? t.d.depth : 1;
    if (has_v && v.set_layers) layers = v.nlayers ? v.nlayers : (layers > v.min_layer ? layers - v.min_layer : 1);   // 0 = 从起点取到底（同 iOS）
    uint32_t n = 0;
    int layered = have == NVN_TEXTURE_TARGET_2D || have == NVN_TEXTURE_TARGET_2D_ARRAY ||
        have == NVN_TEXTURE_TARGET_CUBEMAP || have == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    if (layered) {
        if (want == NVN_TEXTURE_TARGET_2D) n = 1;
        else if (want == NVN_TEXTURE_TARGET_2D_ARRAY) n = layers;
        else if (want == NVN_TEXTURE_TARGET_CUBEMAP && layers >= 6) n = 6;
        else if (want == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && layers >= 6) n = layers / 6 * 6;
    }
    if ((want == NVN_TEXTURE_TARGET_2D_MULTISAMPLE && have == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY) ||
        (want == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY && have == NVN_TEXTURE_TARGET_2D_MULTISAMPLE)) n = 1;
    int cube = want == NVN_TEXTURE_TARGET_CUBEMAP || want == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    if (!n && cube && (have == NVN_TEXTURE_TARGET_2D || (have == NVN_TEXTURE_TARGET_2D_ARRAY && layers < 6))) {
        if (!g_zero_cube_logged) {
            g_zero_cube_logged = 1;
            nvn_plat_log(NVN_PLAT_LOG_INFO, "tex", "声明为立方体的二维占位贴图绑定零贴图");
        }
        return decl_zero(dtt, dcls, c);
    }
    if (!n || n > UINT16_MAX) {
        // 不同维度的组合（如 3D 贴图被声明成二维数组读，#936）：Vulkan 不许这种视图，原样绑下去是未定义行为（Maleoon 上整片黑斑）。
        // 同 iOS 层（nvn_mtl.m tex_fit 之后）与 Ryujinx（Texture.GetTargetTexture 对非同维目标返回 null、槽位读作未绑定）：绑声明类型的零贴图。
        // 记账照旧；零贴图建不出时才退回原视图（描述符必须有效）
        if (nvn_unimpl("tex", "decl_type_mismatch", NULL) == 1) {
            char m[160];
            snprintf(m, sizeof m, "贴图类型不符：着色器声明目标 %d，贴图是 %d（%ux%ux%u）：按 iOS / Ryujinx 绑声明类型的零贴图", want, have,
                     (unsigned)t.d.width, (unsigned)t.d.height, (unsigned)t.d.depth);
            nvn_plat_log(NVN_PLAT_LOG_WARN, "tex", m);
        }
        nvn_gpu_view z = decl_zero(dtt, dcls, c);
        return z ? z : rp_view_by_id(id, c, t_out);
    }
    if (!has_v) memset(&v, 0, sizeof v);
    v.set_target = 1; v.target = (uint8_t)want;
    v.set_layers = 1; v.nlayers = (uint16_t)n;
    // 非立方体存储（≥6 层的二维数组）当立方体读：同 iOS tex_fit 直接建立方体视图。后端给方形、≥6 层、单采样的二维数组建图时
    // 带立方体兼容（P0-6），这种照交下去；别的（非方形等）Vulkan 建不了立方体兼容的图，必被后端拒绝：记账（不静默）并绑声明类型的零贴图
    int cube_compat_array = t.d.target == NVN_TEXTURE_TARGET_2D_ARRAY && t.d.width == t.d.height && t.d.samples <= 1 && t.d.depth >= 6;
    if (cube && t.d.target != NVN_TEXTURE_TARGET_CUBEMAP && t.d.target != NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && !cube_compat_array) {
        nvn_unimpl("tex", "cube_view_of_2d_array", NULL);
        return decl_zero(dtt, dcls, c);
    }
    return rp_view_for(&t, &v, c);
}

// 一组贴图槽的「编译事实」摘要：每槽取绑定贴图的有效格式、有效目标、深度（没登记的槽取「在不在池范围里」）。
// 着色器来源拿它判断「这些槽对编译来说跟上次一样」，一次加锁看完一组，不必逐槽取快照。
uint64_t rp_tex_fact_sig(const uint64_t *handles, int n, int tex_base, int tex_pool_n) {
    uint64_t hsh = 1469598103934665603ull;
    #define SIG_MIX(v) do { hsh ^= (uint64_t)(v); hsh *= 1099511628211ull; } while (0)
    pthread_mutex_lock(&g_rmu);
    for (int i = 0; i < n; i++) {
        long id = handles[i] ? (long)(handles[i] & 0xFFFFF) : (long)tex_base;
        if (id >= 0 && id < g_ntreg && g_treg[id].ok) { const treg_t *r = &g_treg[id];
            SIG_MIX(1); SIG_MIX(r->has_v && r->v.set_format ? r->v.format : r->t.d.format); SIG_MIX(r->has_v && r->v.set_target ? r->v.target : r->t.d.target); SIG_MIX(r->t.d.depth);
        } else { SIG_MIX(2); SIG_MIX(id >= tex_base && (uint64_t)(id - tex_base) < (uint64_t)tex_pool_n); }
    }
    pthread_mutex_unlock(&g_rmu);
    #undef SIG_MIX
    return hsh;
}

// 贴图号 → 登记时的贴图描述快照（着色器编译事实用；不建图）。1 = 有
int rp_tex_snap_by_id(int id, tex_t *t, view_t *v, int *has_v) {
    int ok = 0;
    pthread_mutex_lock(&g_rmu);
    if (id >= 0 && id < g_ntreg && g_treg[id].ok) { if (t) *t = g_treg[id].t; if (v) *v = g_treg[id].v; if (has_v) *has_v = g_treg[id].has_v; ok = 1; }
    pthread_mutex_unlock(&g_rmu);
    return ok;
}

nvn_gpu_samp rp_samp_by_id(int id) {
    pthread_mutex_lock(&g_rmu);
    nvn_gpu_samp s = NULL;
    if (id >= 0 && id < g_nsreg && g_sreg[id].ok) {
        if (!g_sreg[id].s) { nvn_gpu_err err = {0}; if (!nvn_gpu_ok(nvn_gpu_samp_create(&g_sreg[id].p, &g_sreg[id].s, &err))) { g_sreg[id].s = NULL; nvn_unimpl("gpu", "samp_create", &g_sreg[id]); } }
        s = g_sreg[id].s;
    }
    pthread_mutex_unlock(&g_rmu);
    return s;
}

void rp_res_reset(void) {
    pthread_mutex_lock(&g_rmu);
    for (int i = 0; i < g_ntreg; i++) treg_drop_view(&g_treg[i]);
    vf_drop_serial(0);
    for (int target = 0; target <= NVN_TEXTURE_TARGET_BUFFER; target++) for (int cls = 0; cls < 4; cls++) {
        if (g_zero[target][cls].view) nvn_gpu_view_release(g_zero[target][cls].view);
        if (g_zero[target][cls].tex) nvn_gpu_tex_release(g_zero[target][cls].tex);
    }
    memset(g_zero, 0, sizeof g_zero); g_zero_cube_logged = 0;
    for (int i = 0; i < g_nsreg; i++) if (g_sreg[i].s) nvn_gpu_samp_release(g_sreg[i].s);
    free(g_treg); g_treg = NULL; g_ntreg = 0; __atomic_fetch_add(&g_treg_gen, 1, __ATOMIC_RELEASE); free(g_sreg); g_sreg = NULL; g_nsreg = 0; g_tpool_n = 0;
    // 一份存储可能同时挂在序号表（多次）和共用表里：先收集去重再放
    struct rp_tex **all = NULL; int n = 0, cap = 0;
    nvn_hmap *tabs[2] = {&g_by_serial, &g_alias};
    for (int k = 0; k < 2; k++) for (uint32_t i = 0; i < tabs[k]->cap; i++) { struct rp_tex *x = tabs[k]->vals[i]; if (!x) continue;
        int dup = 0; for (int j = 0; j < n; j++) if (all[j] == x) { dup = 1; break; }
        if (dup) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64; all = realloc(all, (size_t)cap * sizeof *all); }
        all[n++] = x; }
    for (int j = 0; j < n; j++) { nvn_gpu_tex_release(all[j]->g); free(all[j]); }
    free(all);
    nvn_hmap *clr[4] = {&g_by_serial, &g_alias, &g_cpu_ser, &g_pool_gpu};
    for (int k = 0; k < 4; k++) { uint32_t kl = clr[k]->klen; free(clr[k]->keys); free(clr[k]->vals); nvn_hm_init(clr[k], kl); }
    g_built = 0; g_ser_gen++;
    pthread_mutex_unlock(&g_rmu);
}

int rp_res_tex_built(void) { pthread_mutex_lock(&g_rmu); int n = g_built; pthread_mutex_unlock(&g_rmu); return n; }
int rp_res_tex_serials(void) { pthread_mutex_lock(&g_rmu); int n = (int)g_by_serial.n; pthread_mutex_unlock(&g_rmu); return n; }
uint64_t rp_res_uploads(void) { pthread_mutex_lock(&g_rmu); uint64_t n = g_uploads; pthread_mutex_unlock(&g_rmu); return n; }

// 调试：序号 → 后端贴图（没建过 = NULL）。读回落盘用（rp_debug_dump_chain）
nvn_gpu_tex rp_tex_backend_by_serial(uint64_t serial) {
    pthread_mutex_lock(&g_rmu);
    struct rp_tex *x = nvn_hm_get(&g_by_serial, &serial); nvn_gpu_tex g = x ? x->g : NULL;
    pthread_mutex_unlock(&g_rmu);
    return g;
}
