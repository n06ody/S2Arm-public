#include "rp_res.h"
#include "rp_exec.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "nvn_hmap.h"
#include "nvn_dirty.h"
#include "nvn_fmt_info.h"
#include "nvn_deswizzle.h"
#include <pthread.h>
#include <stdatomic.h>
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
    uint8_t log_ok;            // 在所属池的脏页追加日志上有游标（log_span / log_id / log_cur）：核对只读游标之后的事件、与本贴图范围求交
    uint8_t unarmed;           // 最近一次上传时有页加保护没落地（被并发的解保护挡住、此刻仍可写）：之后的写可能不缺页、不进日志也不推代号，这次上传不算同步
    uint8_t _rsv[1];
    uint64_t uid;              // 创建序号（只增、不回收）：按存储取视图的表（g_vf）用它认存储——存储释放后新分配可能落在同一地址，不能拿指针当身份
    uint64_t seen;             // 共用表里的：最近一次被查到 / 新建时的提交号 + 1（alias_take_over 用）
    uint64_t wrote;            // 最近一次被 GPU 写时的提交号 + 1（0 = 没写过）
    uint64_t stamp;            // 最近一次上传取的脏页代号
    uint64_t up_sub;           // 最近一次上传所在的提交序号
    uint64_t check_epoch;      // 上次核对内容时的全局写代号（nvn_dirty_now）：等于当前值 = 那之后没有任何被跟踪的页被写过
    uint64_t check_seg;        // 上次核对所在的段号：换段（新一次提交）必重核一次
    uint64_t check_map;        // 上次核对时的映射代际（换底 / 重新登记后句柄与保护状态都要重验）
    const void *span;          // 跟踪区间句柄（nvn_dirty_span）：NULL = 这段内存没被写跟踪，只能每次保守查
    const void *log_span; uint64_t log_id, log_cur;   // 脏页日志游标（tex_log_arm 起；log_ok 为 0 时不用）
    const uint8_t *mem; uint64_t size;
};
// 同存储共用表的键：内存池 + 偏移 + 描述（去掉只表示用途、不影响存储布局的标志）
typedef struct { uint64_t pool; int64_t off; nvn_texdesc d; } alias_key;
enum { ALIAS_USE_FLAGS = NVN_TEXTURE_FLAGS_IMAGE | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_ADAPTIVE_ZCULL | NVN_TEXTURE_FLAGS_DEPTH16_PREFER_FAST_CLEAR | NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL };
enum { SWZ_IDENTITY = 2 | 3 << 3 | 4 << 6 | 5 << 9 };   // tex_builder_t.swz 打包法下的 R G B A：等于没设
static void alias_key_of(alias_key *k, const tex_t *t) { memset(k, 0, sizeof *k); k->pool = (uint64_t)(uintptr_t)t->pool; k->off = t->off; k->d = t->d; k->d.flags &= ~(int)ALIAS_USE_FLAGS; }

// Reserve the retirement node when creating a registered view, not when it is
// invalidated: pinning an already borrowed handle must never require allocation.
struct registered_view { nvn_gpu_view view; struct registered_view *next; };
typedef struct { tex_t t; view_t v; uint8_t ok, has_v; nvn_gpu_view view; struct registered_view *owned; struct rp_tex *of; uint64_t gen; } treg_t;
typedef struct { samp_par p; uint8_t ok; nvn_gpu_samp s, sn; } sreg_t;   // sn：同参数的最近点采样器（#1072，配不能线性过滤的格式时用），用到才建
static nvn_gpu_samp g_query_sampler;

static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
static treg_t *g_treg; static int g_ntreg;
static uint64_t g_treg_gen;   // 登记表里「编译事实看得到的内容」每变一次加一（登记 / 注销 / 清空）；rp_tex_reg_gen 给着色器来源的快路径判缓存用
uint64_t rp_tex_reg_gen(void) { return __atomic_load_n(&g_treg_gen, __ATOMIC_ACQUIRE); }
static sreg_t *g_sreg; static int g_nsreg;
static uint32_t g_pin_depth;
static struct registered_view *g_pinned_registered_views;
struct pinned_samp { nvn_gpu_samp samp; struct pinned_samp *next; };
static struct pinned_samp *g_pinned_samps;
static int g_tpool_n;
static nvn_hmap g_by_serial = {.klen = 8};                  // 序号 → struct rp_tex *
static nvn_hmap g_alias = {.klen = sizeof(alias_key)};      // 键 → struct rp_tex *
static nvn_hmap g_cpu_ser = {.klen = 8};                    // 游戏用 WriteTexels 写过的序号（可能早于首次使用）
static nvn_hmap g_pool_gpu = {.klen = 8};                   // GPU 写过的内存池
static uint64_t g_ser_gen = 1;                              // 任何「序号 → 存储」映射变了就 +1：各贴图号缓存的视图据此重验
static uint64_t g_tex_uid;                                  // rp_tex 创建序号（只增、会话重置也不清）
static int g_built; static uint64_t g_uploads, g_alias_taken, g_alias_dead;   // g_alias_taken：被新贴图占了存储而出表的共用存储数；g_alias_dead：其中还有对象指着的

// 声明类型的零贴图与默认视图：会话内共用，不参与视图环淘汰。
static struct { nvn_gpu_tex tex; nvn_gpu_view view; } g_zero[NVN_TEXTURE_TARGET_BUFFER + 1][4];
static int g_zero_cube_logged;

// ---------- 登记 ----------
static void treg_drop_view(treg_t *r) {
    if (r->owned) {
        if (g_pin_depth) { r->owned->next = g_pinned_registered_views; g_pinned_registered_views = r->owned; }
        else { nvn_gpu_view_release(r->owned->view); free(r->owned); }
        r->owned = NULL;
    }
    r->view = NULL; r->of = NULL;
}
void nvn_be_set_tex_pool_n(int n) { pthread_mutex_lock(&g_rmu); g_tpool_n = n; pthread_mutex_unlock(&g_rmu); }
void nvn_be_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id >= (1 << 20) || !t) return;
    pthread_mutex_lock(&g_rmu);
    if (id >= g_ntreg) { int n = id + 1024;
#ifdef S2A_TEST_TREG_SCRUB
        // 宿主单测：扩容一定换地址，旧表写满 0xA5 后留到下一次扩容才放（读旧表读到标记值而不是崩溃），让「还拿着旧表地址」可确定地测出来
        extern int s2a_test_treg_grows; static treg_t *retired;
        treg_t *nr = malloc((size_t)n * sizeof *nr); if (!nr) { pthread_mutex_unlock(&g_rmu); return; }
        if (g_ntreg) memcpy(nr, g_treg, (size_t)g_ntreg * sizeof *nr);
        if (g_treg) memset(g_treg, 0xA5, (size_t)g_ntreg * sizeof *nr);
        free(retired); retired = g_treg; s2a_test_treg_grows++;
#else
        treg_t *nr = realloc(g_treg, (size_t)n * sizeof *nr); if (!nr) { pthread_mutex_unlock(&g_rmu); return; }
#endif
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
    nvn_gpu_samp *old_s[2] = {&r->s, &r->sn};
    for (int k = 0; k < 2; k++) if (*old_s[k]) {
        if (g_pin_depth) {
            struct pinned_samp *pin = malloc(sizeof *pin);
            if (!pin) { nvn_unimpl("gpu", "sampler_pin_allocation", p); pthread_mutex_unlock(&g_rmu); return; }
            pin->samp = *old_s[k]; pin->next = g_pinned_samps; g_pinned_samps = pin;
        } else nvn_gpu_samp_release(*old_s[k]);
        *old_s[k] = NULL;
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
// 脏页追加日志游标（开关 nvk_buf_dirtylog，安卓缺省开、同 rp_buf）：在加保护、取代号之前起（先起游标、后读代号），
// 之后这张贴图的页再变可写必然在游标之后留一条事件。只在上传时读开关；开关关 / 起不了日志时 log_ok = 0，核对照旧整张逐页比。
static void tex_log_arm(struct rp_tex *x, const tex_t *t) {
    x->log_ok = 0;
    if (!nvn_plat_flag("nvk_buf_dirtylog", 1)) return;
    const void *sp = nvn_dirty_span(t->mem, t->L.size);
    if (sp && nvn_dirty_log_open(sp, &x->log_id, &x->log_cur)) { x->log_span = sp; x->log_ok = 1; }
}
// 加保护是否落地：贴图覆盖的每一页此刻都只读（nvn_dirty_page_armed）。被并发的解保护挡住的页（对方先记了事件、后推代号），
//   之后的写不缺页：既不进游标之后的日志，代号也可能不晚于这次取的代号——日志路、逐页路都看不见。同 rp_buf 的「页重新只读才清旧位」。
//   没有跟踪句柄（不被写跟踪）不查：那种贴图每次都按逐页比保守判脏。
static int tex_pages_armed(const struct rp_tex *x, const tex_t *t) {
    if (!x->span || !t->L.size) return 1;
    const size_t pg = nvn_dirty_page_size();
    const uintptr_t a = (uintptr_t)t->mem, e = a + t->L.size;
    for (uintptr_t q = a; q < e; q = (q & ~(uintptr_t)(pg - 1)) + pg) if (!nvn_dirty_page_armed(x->span, (const void *)q)) return 0;
    return 1;
}
struct tex_log_q { uintptr_t a, b; uint64_t ev; int hit; };
static void tex_log_event(void *u, uintptr_t a, uintptr_t b) { struct tex_log_q *q = u; q->ev++; if (a < q->b && b > q->a) q->hit = 1; }
// 内容自上次上传后是否被写过：有日志游标先读日志（按事件数付钱）；日志不可用 / 环绕 / 换底 / 句柄变了 → 计一次退回，
//   重起游标后照旧整张逐页比代号（逐页比的结论对新游标同样成立：取游标之后的写都会进日志）。返回 1 = 脏。
static int tex_content_dirty(struct rp_tex *x, const tex_t *t) {
    const uintptr_t a = (uintptr_t)t->mem;
    if (x->log_ok) {
        struct tex_log_q q = { a, a + t->L.size, 0, 0 };
        if (x->log_span == x->span && nvn_dirty_log_read(x->log_span, x->log_id, &x->log_cur, tex_log_event, &q)) {
            rp_perf_add(RP_PERF_TEX_LOG_N, 1); rp_perf_add(RP_PERF_TEX_LOG_EV, q.ev);
            return q.hit;
        }
        rp_perf_add(RP_PERF_TEX_LOG_FALLBACK, 1);
        tex_log_arm(x, t);
    }
    const size_t pg = nvn_dirty_page_size();
    rp_perf_add(RP_PERF_TEX_CHECK_PAGES, t->L.size ? ((a + t->L.size - 1) / pg - a / pg + 1) : 0);   // nvn_dirty_since 要逐页比的页数（上限）
    return nvn_dirty_since(t->mem, t->L.size, x->stamp);
}

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
    uint64_t st = 0; int armed = 1;
    if (track) { tex_log_arm(x, t); nvn_dirty_protect(t->mem, t->L.size); st = nvn_dirty_now(); x->span = nvn_dirty_span(t->mem, t->L.size); armed = tex_pages_armed(x, t); }   // 先保护、取代号、再读：取代号之后的写下次查得到
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
    if (!rc && track) { x->stamp = st; x->uploaded = 1; x->unarmed = !armed; if (!armed) rp_perf_add(RP_PERF_TEX_UNARMED, 1); g_uploads++; }   // 加保护没落地：下一次必重传（rp_tex_resolve）
    return rc;
}

// 线性单级二维 CPU 贴图只重传被写过的行。真机：一款游戏每帧用 CPU 改一张 4096x8192 线性 R8 贴图的一小块，
//   按「有一页脏就整张重传」每帧传 32 MB、取贴图视图持锁约 25 ms（帧时间一半）。线性排布里一页对应连续若干行，
//   按脏页位图（nvn_dirty_written_pages）把连续的脏页换成行区间逐段上传。
//   顺序同 upload_all：先重新保护、取新代号，再按旧代号取位图、读内容——取位图之后的写，下次照样查得到。
//   返回 0 = 已按行传完；-1 = 不适用（没传过 / 不是线性单级二维 / 不被跟踪 / 脏行过半），调用方整张传；1 = 失败。
static int upload_linear_dirty(const tex_t *t, struct rp_tex *x, nvn_gpu_cmd c) {
    const char *why = NULL;
    if (!x->uploaded || !x->span || t->d.levels > 1 || t->L.layers > 1 || t->d.target != NVN_TEXTURE_TARGET_2D || linear_class(t, &why) <= 0) return -1;
    int bw = 1, bh = 1; int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (bpb <= 0 || bw != 1 || bh != 1) return -1;
    const uint32_t w = t->d.width, rows = t->d.height;
    const size_t row_bytes = (size_t)w * (size_t)bpb, len = t->L.size;
    if (!rows || !len) return -1;
    const uint64_t sp = len / rows;   // 源行距：同 nvn_deswizzle 的 move_linear（L.size / 行数）
    const size_t P = nvn_dirty_page_size();
    if (sp < row_bytes || !P) return -1;
    const uintptr_t a = (uintptr_t)t->mem, first = a & ~(uintptr_t)(P - 1);
    const size_t npg = (size_t)(((a + len - 1) & ~(uintptr_t)(P - 1)) - first) / P + 1;
    uint64_t *bits = calloc((npg + 63) / 64, sizeof *bits);
    if (!bits) return -1;
    tex_log_arm(x, t);   // 同 upload_all：先起游标再加保护；游标之前的写由下面按旧代号取的位图接住
    nvn_dirty_protect(t->mem, len);
    const uint64_t st = nvn_dirty_now();
    const int armed = tex_pages_armed(x, t);   // 读内容之前查：之后才重新只读的页不能证明中间那次写已读到
    long n = nvn_dirty_written_pages(x->span, t->mem, len, x->stamp, bits, npg);
    if (n < 0 || (size_t)n != npg) { free(bits); return -1; }
    // 脏页 → 行区间（合并相邻）；脏行过半就整张传，省得逐段的固定开销反而更贵
    uint32_t nrun = 0, dirty_rows = 0; uint32_t (*run)[2] = NULL; size_t cap = 0;
    for (size_t k = 0; k < npg; ) {
        if (!(bits[k >> 6] >> (k & 63) & 1)) { k++; continue; }
        size_t k1 = k; while (k1 + 1 < npg && (bits[(k1 + 1) >> 6] >> ((k1 + 1) & 63) & 1)) k1++;
        uintptr_t lo = first + k * P, hi = first + (k1 + 1) * P;
        if (lo < a) lo = a;
        if (hi > a + len) hi = a + len;
        uint32_t r0 = (uint32_t)((lo - a) / sp), r1 = (uint32_t)(((hi - a) + sp - 1) / sp);
        if (r1 > rows) r1 = rows;
        if (nrun && run[nrun - 1][1] >= r0) { if (r1 > run[nrun - 1][1]) { dirty_rows += r1 - run[nrun - 1][1]; run[nrun - 1][1] = r1; } }
        else if (r1 > r0) {
            if (nrun == cap) { size_t nc = cap ? cap * 2 : 16; void *nr = realloc(run, nc * sizeof *run); if (!nr) { free(run); free(bits); return 1; } run = nr; cap = nc; }
            run[nrun][0] = r0; run[nrun][1] = r1; nrun++; dirty_rows += r1 - r0;
        }
        k = k1 + 1;
    }
    free(bits);
    if ((uint64_t)dirty_rows * 2 > rows) { free(run); return -1; }
    uint8_t *tmp = NULL; size_t tcap = 0; int rc = 0;
    for (uint32_t i = 0; i < nrun; i++) {
        const uint32_t r0 = run[i][0], nr = run[i][1] - r0;
        const size_t img = row_bytes * nr;
        if (img > tcap) { uint8_t *nt = realloc(tmp, img); if (!nt) { rc = 1; break; } tmp = nt; tcap = img; }
        for (uint32_t y = 0; y < nr; y++) memcpy(tmp + (size_t)y * row_bytes, (const uint8_t *)t->mem + (uint64_t)(r0 + y) * sp, row_bytes);
        NVNcopyRegion r = {0, (int)r0, 0, (int)w, (int)nr, 1};
        nvn_gpu_tex_upload(c, x->g, 0, 0, &r, tmp, row_bytes, img);
        rp_perf_add(RP_PERF_TEX_UP_ROWS, nr);
    }
    free(tmp); free(run);
    if (rc) return 1;
    rp_perf_add(RP_PERF_TEX_UP_PART, 1);
    x->stamp = st; x->unarmed = !armed; if (!armed) rp_perf_add(RP_PERF_TEX_UNARMED, 1); g_uploads++;
    return 0;
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

uint32_t rp_storage_srgb_alias(uint32_t f) {
    nvn_gpu_caps cp; nvn_gpu_caps_get(&cp);   // 不缓存：单测换替身后端能力时要跟着变；只在建图和派发图像槽时走，代价一次结构体拷贝
    if (!(cp.caps & NVN_GPU_CAP_STORAGE_SRGB_ALIAS) || (nvn_gpu_format_caps(f) & NVN_GPU_FMT_STORAGE)) return 0;
    uint32_t a = f == NVN_FORMAT_RGBA8_SRGB ? NVN_FORMAT_RGBA8 : f == NVN_FORMAT_RGBX8_SRGB ? NVN_FORMAT_RGBX8
               : f == NVN_FORMAT_BGRA8_SRGB ? NVN_FORMAT_BGRA8 : f == NVN_FORMAT_BGRX8_SRGB ? NVN_FORMAT_BGRX8 : 0;
    return a && (nvn_gpu_format_caps(a) & NVN_GPU_FMT_STORAGE) ? a : 0;
}

static uint32_t usage_of(const tex_t *t, int cpu) {
    uint32_t caps = nvn_gpu_format_caps((uint32_t)t->d.format), u = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_VIEW_FMT;
    // Vulkan 的用途事后补不了，Metal 那边是用到时再升级（tex_upgrade）：能当附件的格式一律带上附件用途
    if ((caps & (NVN_GPU_FMT_RENDER | NVN_GPU_FMT_DEPTH)) && !fmt_compressed(t->d.format)) u |= NVN_GPU_TEX_RENDER;
    // 带图像标志的 sRGB 贴图：本身不能当存储，但后端能按同位宽 UNORM 视图写（rp_storage_srgb_alias），建图时就得带存储用途（事后补不了）
    if ((t->d.flags & NVN_TEXTURE_FLAGS_IMAGE) && ((caps & NVN_GPU_FMT_STORAGE) || rp_storage_srgb_alias((uint32_t)t->d.format))) u |= NVN_GPU_TEX_STORAGE;
    if (cpu) u |= NVN_GPU_TEX_CPU_UPLOAD;
    return u;
}

static void vf_drop_of(const struct rp_tex *x);   // 按目标取视图的表里指着这份存储的项（定义在那张表后面）
static void tex_unref(struct rp_tex *x) {
    if (--x->users > 0 || x->in_alias) return;   // 共用表里的存储由表持有：池销毁时才放（nvn_be_alias_drop_pool）
    vf_drop_of(x);   // 视图表按存储认视图：存储放掉之前，它名下的视图一并放（序号销毁时不再放，见 g_vf 一节）
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
    // 分时抽样（#1103）：原先每次解析都取 3 次时钟（世界稳态每帧至少 2148 次），现在只在采样帧里计时（见 rp_exec.h 的 rp_tm）。
    const int tm_ = rp_tm();
    uint64_t r0_ = tm_ ? res_now() : 0;
    pthread_mutex_lock(&g_rmu);
    uint64_t r1_ = 0;
    if (tm_) { r1_ = res_now(); rp_perf_add(RP_PERF_RES_LOCK, r1_ - r0_); }
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
            x = calloc(1, sizeof *x); x->g = g; x->cpu = (uint8_t)cpu; x->mem = t->mem; x->size = t->L.size; x->need_init = 1; x->uid = ++g_tex_uid; g_built++;
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
            if (!x->uploaded || x->check_dirty || tex_content_dirty(x, t)) {
                nvn_gpu_cmd xc = rp_xfer_cmd(&x->up_sub);   // 上传是传输：录进前置段（本次提交里第二次上传才回主段）
                int pr = xc ? upload_linear_dirty(t, x, xc) : 1;   // 线性单级二维：只传脏行（不适用返回 -1 再整张传）
                if (xc && (pr == 0 || (pr < 0 && upload_all(t, x, xc, 1) == 0))) { x->check_epoch = now0; x->check_seg = seg0; x->check_map = map0; x->check_dirty = x->unarmed; }   // 加保护没落地：不算同步，下一次必重查重传
                else x->check_dirty = 1;
            } else { x->check_epoch = now0; x->check_seg = seg0; x->check_map = map0; x->check_dirty = 0; }
        }
    }
    nvn_gpu_tex g = x->g;
    if (tm_) rp_perf_add(RP_PERF_RES_INIT, res_now() - r1_);   // 持锁做的全部事（查表 / 新建 / 初始化 / 上传）
    pthread_mutex_unlock(&g_rmu);
    return g;
}

// ---------- 按（贴图对象，视图记录）取视图：渲染目标、拷贝端点这些不经贴图号的使用点 ----------
// 表是 128 组 x 8 路：按（键，视图记录）散列到一组，只在这一组的 8 格里找和换。
// 原先是 64 格的环、逐格比：一帧一百多个通道的附件加拷贝端点远超 64 种，环不停轮转，真机一款 3D 游戏每帧新建 228 个后端视图
// （对象读数），后端按视图记的帧缓冲也跟着每帧重建。
// 键（缺省按存储，开关 nvk_vf_key=0 退回按序号）：视图只由（后端贴图，视图记录）决定，与序号无关。游戏每帧把瞬态渲染目标 Finalize 再按同描述重建，
// 每次都是新序号，但共用表（g_alias）查到的是同一份存储；按序号认视图，这些目标的视图每帧作废重建，按视图记的帧缓冲也跟着每帧重建（真机每帧约 28 个新序号、
// 新建视图约 32 个、新建帧缓冲约 38 个）。按存储认：同一份存储换了序号，视图与帧缓冲照用；存储放掉时（tex_unref / alias_take_over / 池销毁）它名下的项一并放。
// 存储用创建序号（uid，只增不回收）认，不用指针：存储释放后新分配可能落在同一地址。
enum { VF_WAYS = 8, VF_N = 128 * VF_WAYS };
static uint32_t vf_base(uint64_t key, const view_t *v) {
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    if (v) { const uint8_t *p = (const uint8_t *)v; for (size_t i = 0; i < sizeof *v; i++) h = (h ^ p[i]) * 0x100000001B3ull; }
    return (uint32_t)((h >> 20) % (VF_N / VF_WAYS)) * VF_WAYS;
}
static int vf_key_storage(void) { static int on = -1; static unsigned k; if (on < 0 || !(++k & 255)) on = nvn_plat_flag("nvk_vf_key", 1) != 0; return on; }   // 调试开关，缺省 1 = 按存储；0 = 按序号（A/B 与负对照）
// kmode：这一项是按哪种键记的（开关中途改了也不会把两种键的项混着认）；key：按存储时是 of_uid，按序号时是 serial
static struct { uint64_t serial, key, of_uid; view_t v; uint8_t has_v, kmode; nvn_gpu_view view; struct registered_view *owned; struct rp_tex *of; } g_vf[VF_N];
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
        while (g_pinned_registered_views) {
            struct registered_view *pin = g_pinned_registered_views;
            g_pinned_registered_views = pin->next;
            nvn_gpu_view_release(pin->view); free(pin);
        }
        while (g_pinned_samps) {
            struct pinned_samp *pin = g_pinned_samps; g_pinned_samps = pin->next;
            nvn_gpu_samp_release(pin->samp); free(pin);
        }
    }
    pthread_mutex_unlock(&g_rmu);
}
static void vf_drop(int i) {   // Hold g_rmu; invalidation must preserve handed-out views too.
    if (g_vf[i].view) {
        if (g_pin_depth && g_vf_seen[i] == g_pin_token) {
            g_vf[i].owned->next = g_pinned_registered_views;
            g_pinned_registered_views = g_vf[i].owned;
        } else { nvn_gpu_view_release(g_vf[i].view); free(g_vf[i].owned); }
    }
    memset(&g_vf[i], 0, sizeof g_vf[i]);
}
static void vf_drop_of(const struct rp_tex *x) { for (int i = 0; i < VF_N; i++) if (g_vf[i].view && g_vf[i].of_uid == x->uid) vf_drop(i); }   // 持 g_rmu；按存储的 uid 认，两种键的项都放
static void vf_drop_serial(uint64_t serial) {   // 持 g_rmu。serial = 0：全放。否则只放按序号记的项——按存储记的项跟着存储走，序号销毁不放（存储还在共用表里，下一个同描述的新序号接着用）
    for (int i = 0; i < VF_N; i++) if (g_vf[i].view && (!serial || (!g_vf[i].kmode && g_vf[i].serial == serial))) vf_drop(i);
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
    const uint8_t km = (uint8_t)vf_key_storage();
    const uint64_t key = km ? (x ? x->uid : 0) : t->serial;
    const uint32_t base = vf_base(key, v);
    for (int i = (int)base; i < (int)base + VF_WAYS && x; i++) if (g_vf[i].view && g_vf[i].kmode == km && g_vf[i].key == key && g_vf[i].has_v == (v != NULL) && (!v || !memcmp(&g_vf[i].v, v, sizeof *v))) {
        hit = i;
        if (g_vf[i].of_uid == x->uid) out = g_vf[i].view; else slot = i;   // 按序号记的项：序号换了存储，这一项作废重建（按存储记的项键里就是 uid，必同）
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
        vf_drop(slot);
        struct registered_view *owned = calloc(1, sizeof *owned);
        if (!owned) {
            nvn_unimpl("gpu", "attachment_view_allocation", t);
            pthread_mutex_unlock(&g_rmu); return NULL;
        }
        rp_perf_add(RP_PERF_RES_VIEWMAKE, 1);
        if (nvn_gpu_ok(nvn_gpu_view_create(x->g, v, &g_vf[slot].view, &err))) { owned->view = g_vf[slot].view; g_vf[slot].owned = owned; g_vf[slot].serial = t->serial; g_vf[slot].key = key; g_vf[slot].kmode = km; g_vf[slot].has_v = v != NULL; if (v) g_vf[slot].v = *v; g_vf[slot].of = x; g_vf[slot].of_uid = x->uid; out = g_vf[slot].view; if (g_pin_depth > 0) g_vf_seen[slot] = g_pin_token; }
        else { free(owned); g_vf[slot].view = NULL; if (nvn_unimpl("gpu", "view_create", t) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", err.why); }
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
            if (x->users <= 0) { vf_drop_of(x); nvn_gpu_tex_release(x->g); g_built--; free(x); }
            again = 1; break; } }   // 删除会挪表项：从头再扫
    g_ser_gen++;
    pthread_mutex_unlock(&g_rmu);
}

nvn_gpu_view rp_view_by_id(int id, nvn_gpu_cmd c, tex_t *t_out) {
    if (t_out) memset(t_out, 0, sizeof *t_out);
    pthread_mutex_lock(&g_rmu);
    if (id < 0 || id >= g_ntreg || !g_treg[id].ok || (g_tpool_n && id >= g_tpool_n)) { pthread_mutex_unlock(&g_rmu); return NULL; }
    tex_t t = g_treg[id].t; view_t v = g_treg[id].v; int has_v = g_treg[id].has_v;
    if (t_out) *t_out = t;   // 锁内拷给调用方：登记表会被客体线程扩容换地址，不能把表内地址交出去
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
            struct registered_view *owned = calloc(1, sizeof *owned);
            if (!owned) nvn_unimpl("gpu", "registered_view_allocation", r);
            else if (nvn_gpu_ok(nvn_gpu_view_create(x->g, has_v ? &v : NULL, &owned->view, &err))) {
                r->owned = owned; r->view = owned->view; r->of = x;
            } else { free(owned); nvn_unimpl("gpu", "view_create", r); }
        }
        r->gen = g_ser_gen; out = r->view;
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

static nvn_gpu_view view_by_id_decl(int id, uint8_t dtt, uint8_t dcls, nvn_gpu_cmd c, tex_t *t_out);
static _Atomic uint64_t g_empty_slot_zero;   // 空槽绑零贴图的次数（#1147；信息计数，不是未接）
uint64_t rp_empty_slot_zero_count(void) { return atomic_load(&g_empty_slot_zero); }
nvn_gpu_view rp_view_by_id_decl(int id, uint8_t dtt, uint8_t dcls, int elem, nvn_gpu_cmd c, tex_t *t_out) {
    if (!dtt) return rp_view_by_id(id, c, t_out);
    if (elem) {
        tex_t t; view_t v; int has_v;
        if (rp_tex_snap_by_id(id, &t, &v, &has_v)) {
            nvn_fmt_info fi;
            uint32_t fmt = has_v && v.set_format ? v.format : (uint32_t)t.d.format;
            if (nvn_fmt_info_of(fmt, &fi)) {
                int stencil = fi.stencil && (!fi.depth || (has_v && v.set_dsmode && v.dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL));
                int cls = stencil || fi.kind == NVN_FMT_UINT ? 1 : fi.kind == NVN_FMT_SINT ? 2 : 0;
                if (cls && cls != dcls) {
                    if (t_out) memset(t_out, 0, sizeof *t_out);   // 调用方持有的快照：没取到清零
                    nvn_unimpl("tex", "sampled_array_numeric_mismatch", NULL);
                    return NULL;   // 不能用零元素掩盖已登记的整数视图。
                }
            }
        }
    }
    nvn_gpu_view out = view_by_id_decl(id, dtt, dcls, c, t_out);
    // 贴图数组里取不到的元素：描述符数组每项都要有效，用声明类型的零贴图顶上（类型与声明一致，不再拿别的元素凑）
    if (!out && elem) out = decl_zero(dtt, dcls, c);
    // 空槽（#1147）：着色器声明了这个组合槽、游戏这一笔没绑贴图（句柄 0 → 取池里 tex_base + 0；0 号是驱动保留区、TexturePoolInitialize 不写），
    //   或池容量内一个从没登记过的号：同 Ryujinx（无效描述符按未绑读 0）与 iOS（空槽读 0），绑声明类型的零贴图、不跳整笔，记一条信息计数。
    //   池容量不知道（没登记池）或号越过池容量、号为负，仍取不到 → 调用方照旧报 texture_slot_unresolved 跳笔
    if (!out && !elem && id >= 0) {
        pthread_mutex_lock(&g_rmu);
        const int empty = g_tpool_n && id < g_tpool_n && (id >= g_ntreg || !g_treg[id].ok);
        pthread_mutex_unlock(&g_rmu);
        if (empty && (out = decl_zero(dtt, dcls, c)) != NULL && atomic_fetch_add(&g_empty_slot_zero, 1) == 0)
            nvn_plat_log(NVN_PLAT_LOG_INFO, "tex", "空贴图槽（句柄 0 或池内未登记的号）：按 Ryujinx / iOS 绑声明类型的零贴图，不跳整笔");
    }
    return out;
}
static nvn_gpu_view view_by_id_decl(int id, uint8_t dtt, uint8_t dcls, nvn_gpu_cmd c, tex_t *t_out) {
    if (t_out) memset(t_out, 0, sizeof *t_out);
    pthread_mutex_lock(&g_rmu);
    if (id < 0 || id >= g_ntreg || !g_treg[id].ok || (g_tpool_n && id >= g_tpool_n)) {
        pthread_mutex_unlock(&g_rmu);
        return NULL;
    }
    tex_t t = g_treg[id].t; view_t v = g_treg[id].v; int has_v = g_treg[id].has_v;
    if (t_out) *t_out = t;
    pthread_mutex_unlock(&g_rmu);
    int want = dtt - 1, have = has_v && v.set_target ? v.target : t.d.target;
    int planned = rp_decl_view_target(&t, &v, has_v, dtt);
    if (!planned) return rp_view_by_id(id, c, t_out);
    if (planned < 0) {
        uint32_t layers = t.d.target == NVN_TEXTURE_TARGET_CUBEMAP ? 6 :
            t.d.target == NVN_TEXTURE_TARGET_2D_ARRAY || t.d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY ? t.d.depth : 1;
        if (has_v && v.set_layers) layers = v.nlayers ? v.nlayers : (layers > v.min_layer ? layers - v.min_layer : 1);
        int cube = want == NVN_TEXTURE_TARGET_CUBEMAP || want == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
        if (cube && (have == NVN_TEXTURE_TARGET_2D || (have == NVN_TEXTURE_TARGET_2D_ARRAY && layers < 6))) {
            if (!g_zero_cube_logged) {
                g_zero_cube_logged = 1;
                nvn_plat_log(NVN_PLAT_LOG_INFO, "tex", "声明为立方体的二维占位贴图绑定零贴图");
            }
            return decl_zero(dtt, dcls, c);
        }
        if (cube && have == NVN_TEXTURE_TARGET_2D_ARRAY && layers >= 6) {
            nvn_unimpl("tex", "cube_view_of_2d_array", NULL);
            return decl_zero(dtt, dcls, c);
        }
        if (nvn_unimpl("tex", "decl_type_mismatch", NULL) == 1) {
            char m[160];
            snprintf(m, sizeof m, "贴图类型不符：着色器声明目标 %d，贴图是 %d（%ux%ux%u）：按 iOS / Ryujinx 绑声明类型的零贴图", want, have,
                     (unsigned)t.d.width, (unsigned)t.d.height, (unsigned)t.d.depth);
            nvn_plat_log(NVN_PLAT_LOG_WARN, "tex", m);
        }
        nvn_gpu_view z = decl_zero(dtt, dcls, c);
        return z ? z : rp_view_by_id(id, c, t_out);
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

// 后端建采样器失败的 why 以「sampler_」打头的 ASCII 名字开始时（#976），按这个名字记未接，普查能分出是哪一项；
// 否则记笼统的 samp_create。两种都在首次把后端原文打进红行。
static void samp_red(const nvn_gpu_err *err, const void *site) {
    char name[40]; size_t n = 0;
    if (!strncmp(err->why, "sampler_", 8))
        while (n + 1 < sizeof name && ((err->why[n] >= 'a' && err->why[n] <= 'z') || (err->why[n] >= '0' && err->why[n] <= '9') || err->why[n] == '_')) { name[n] = err->why[n]; n++; }
    name[n] = 0;
    if (nvn_unimpl("gpu", n ? name : "samp_create", site) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", err->why[0] ? err->why : "samp_create（后端没写原因）");
}

static int samp_wrap_no_vk(uint32_t w) { return w == NVN_WRAP_MODE_MIRROR_CLAMP || w == NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER; }

// 两种采样器共享环绕近似与失败报红，不能在最近点变体失败后交回原线性采样器。
static nvn_gpu_samp samp_create(const samp_par *p, const void *site) {
    nvn_gpu_samp out = NULL; nvn_gpu_err err = {0};
    nvn_gpu_rc rc = nvn_gpu_samp_create(p, &out, &err);
    if (rc.code == NVN_GPU_E_UNSUPPORTED && (samp_wrap_no_vk(p->ws) || samp_wrap_no_vk(p->wt) || samp_wrap_no_vk(p->wr))) {
        // #976：Vulkan 不提供这两种环绕，近似成 MIRROR_CLAMP_TO_EDGE 并记账。
        nvn_unimpl("gpu", "samp_wrap_mirror_clamp_approx", site);
        samp_par q = *p;
        if (samp_wrap_no_vk(q.ws)) q.ws = NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE;
        if (samp_wrap_no_vk(q.wt)) q.wt = NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE;
        if (samp_wrap_no_vk(q.wr)) q.wr = NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE;
        memset(&err, 0, sizeof err);
        rc = nvn_gpu_samp_create(&q, &out, &err);
    }
    if (!nvn_gpu_ok(rc)) { out = NULL; samp_red(&err, site); }
    return out;
}

nvn_gpu_samp rp_samp_by_id(int id) {
    pthread_mutex_lock(&g_rmu);
    nvn_gpu_samp s = NULL;
    if (id >= 0 && id < g_nsreg && g_sreg[id].ok) {
        if (!g_sreg[id].s) g_sreg[id].s = samp_create(&g_sreg[id].p, &g_sreg[id]);
        s = g_sreg[id].s;
    }
    pthread_mutex_unlock(&g_rmu);
    return s;
}

// #1072：格式能力（FMT_FILTER / FMT_BLEND 等）按格式记住。后端每问一次都要查一遍格式特性，采样器配视图是每笔每个贴图都要判的。
// 高位 1 = 已查过；会话重开（rp_res_reset）清掉。
static uint32_t g_fmt_caps[512];
uint32_t rp_fmt_caps(uint32_t f) {
    if (f >= sizeof g_fmt_caps / sizeof g_fmt_caps[0]) return nvn_gpu_format_caps(f);
    uint32_t c = __atomic_load_n(&g_fmt_caps[f], __ATOMIC_ACQUIRE);
    if (!(c & 0x80000000u)) { c = nvn_gpu_format_caps(f) | 0x80000000u; __atomic_store_n(&g_fmt_caps[f], c, __ATOMIC_RELEASE); }
    return c & 0x7fffffffu;
}
static int samp_linear(const samp_par *p) {
    return p->magf == NVN_MAG_FILTER_LINEAR || p->minf == NVN_MIN_FILTER_LINEAR || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST ||
           p->minf == NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR || p->aniso > 1.0f;
}
// 采样器配上这张贴图：贴图的有效格式不能线性过滤（G710 的 RGBA16 UNORM、Adreno 830 的 D32S8 等）而采样器要线性过滤 / 级间插值 / 各向异性时，
// 照常绑是未定义行为。改用同参数的最近点采样器（级间也取最近、关各向异性），按格式具名记 tex / filter_unsupported_fmt_0x..。整数格式本来就走最近点，不管。
nvn_gpu_samp rp_samp_for_tex(int sid, int tid) {
    nvn_gpu_samp s = rp_samp_by_id(sid);
    if (!s) return s;
    uint32_t fmt = 0; samp_par p; int have = 0;
    pthread_mutex_lock(&g_rmu);
    if (tid >= 0 && tid < g_ntreg && g_treg[tid].ok) fmt = (uint32_t)(g_treg[tid].has_v && g_treg[tid].v.set_format ? g_treg[tid].v.format : g_treg[tid].t.d.format);
    if (sid >= 0 && sid < g_nsreg && g_sreg[sid].ok) { p = g_sreg[sid].p; have = 1; }
    pthread_mutex_unlock(&g_rmu);
    if (!fmt || !have || !samp_linear(&p)) return s;
    nvn_fmt_info fi; if (nvn_fmt_info_of(fmt, &fi) && (fi.kind == NVN_FMT_UINT || fi.kind == NVN_FMT_SINT)) return s;
    if (rp_fmt_caps(fmt) & NVN_GPU_FMT_FILTER) return s;
    { char nm[40]; snprintf(nm, sizeof nm, "filter_unsupported_fmt_0x%x", (unsigned)fmt); nvn_unimpl("tex", nm, NULL); }
    nvn_gpu_samp out = NULL;
    pthread_mutex_lock(&g_rmu);
    if (sid < g_nsreg && g_sreg[sid].ok) {
        sreg_t *r = &g_sreg[sid];
        if (!r->sn) {
            samp_par q = r->p;
            q.magf = NVN_MAG_FILTER_NEAREST; q.minf = NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST; q.aniso = 1.0f;
            r->sn = samp_create(&q, r);
        }
        if (r->sn) out = r->sn;
    }
    pthread_mutex_unlock(&g_rmu);
    return out;
}

nvn_gpu_samp rp_sampler_for_query(void) {
    pthread_mutex_lock(&g_rmu);
    if (!g_query_sampler) {
        // 查询不会读取这些值；给后端一个有效的物理对象，不登记为任何 NVN 池槽。
        // 取法对齐 Ryujinx DescriptorSetUpdater 的空 sampler 表示。
        samp_par p = {0}; p.minf = NVN_MIN_FILTER_NEAREST; p.magf = NVN_MAG_FILTER_NEAREST;
        p.ws = p.wt = p.wr = NVN_WRAP_MODE_REPEAT; p.cmp_mode = NVN_COMPARE_MODE_NONE;
        p.cmp_func = NVN_COMPARE_FUNC_ALWAYS; p.aniso = 1.f;
        nvn_gpu_err err = {0};
        if (!nvn_gpu_ok(nvn_gpu_samp_create(&p, &g_query_sampler, &err))) {
            g_query_sampler = NULL;
            if (nvn_unimpl("gpu", "query_sampler_create", NULL) == 1 && err.why[0]) nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", err.why);
        }
    }
    nvn_gpu_samp result = g_query_sampler;
    pthread_mutex_unlock(&g_rmu); return result;
}

// 整池数组（布局来源 NVN_RB_TEXP / NVN_RB_SAMPP，编译器给追不到槽的句柄的回退）：按池编号建一张不可变的池快照（nvn_backend.h nvn_gpu_pool_*）。
//   键 = (贴图 / 采样器, 池起点, 池长, 声明目标, 数值类)；登记表纪元或序号纪元变了才重建（快照不可原地改，换表 = 新建 + 放旧，
//   已绑的旧表由后端引用到命令段完成）。贴图项一律走 rp_view_by_id_decl：池内没登记的号 / 类型不符给声明类型的零贴图（#1147，同 Ryujinx），
//   取不到（已登记但建不出）记 UNAVAILABLE —— 后端按合同拒绑，这一笔具名跳过，不拿零贴图冒充。采样器项取不到记 EMPTY（表尾 dummy）。
//   线程：[提交]（开通道前，和贴图数组取视图同一处）。
enum { POOL_CACHE_N = 8 };
static struct { int used, samp, base; uint32_t n; uint8_t dtt, dcls; uint64_t treg_gen, ser_gen; nvn_gpu_pool_table t; } g_pcache[POOL_CACHE_N];
static uint32_t g_pcache_next; static uint64_t g_pool_builds, g_pool_items;
nvn_gpu_pool_table rp_pool_table(nvn_gpu_cmd c, int samp, int base, uint32_t n, uint8_t dtt, uint8_t dcls, const char **why) {
    *why = NULL;
    if (!n || n >= 0xffff) { *why = "pool_length_unsupported"; return NULL; }
    const uint64_t tg = rp_tex_reg_gen(); uint64_t sg; pthread_mutex_lock(&g_rmu); sg = g_ser_gen; pthread_mutex_unlock(&g_rmu);
    int slot = -1;
    for (int k = 0; k < POOL_CACHE_N; k++) {
        if (!g_pcache[k].used || g_pcache[k].samp != samp || g_pcache[k].base != base || g_pcache[k].n != n ||
            (!samp && (g_pcache[k].dtt != dtt || g_pcache[k].dcls != dcls))) continue;
        if (g_pcache[k].treg_gen == tg && g_pcache[k].ser_gen == sg) return g_pcache[k].t;
        slot = k; break;   // 同键旧表：重建后替换
    }
    nvn_gpu_pool_item *items = calloc(n, sizeof *items);
    if (!items) { *why = "pool_table_alloc_failed"; return NULL; }
    nvn_gpu_pool_desc d; memset(&d, 0, sizeof d);
    d.kind = samp ? NVN_GPU_POOL_SAMPLER : NVN_GPU_POOL_IMAGE; d.count = n; d.items = items;
    if (samp) {
        d.dummy.samp = rp_sampler_for_query();
        if (!d.dummy.samp) { free(items); *why = "pool_dummy_sampler_failed"; return NULL; }
        for (uint32_t i = 0; i < n; i++) { nvn_gpu_samp sp = rp_samp_by_id(base + (int)i);
            if (sp) { items[i].state = NVN_GPU_POOL_READY; items[i].resource.samp = sp; } else items[i].state = NVN_GPU_POOL_EMPTY; }
    } else {
        d.dummy.view = decl_zero(dtt, dcls, c);
        if (!d.dummy.view) { free(items); *why = "pool_dummy_texture_failed"; return NULL; }
        for (uint32_t i = 0; i < n; i++) { nvn_gpu_view v = rp_view_by_id_decl(base + (int)i, dtt, dcls, 0, c, NULL);
            if (v) { items[i].state = NVN_GPU_POOL_READY; items[i].resource.view = v; } else items[i].state = NVN_GPU_POOL_UNAVAILABLE; }
    }
    nvn_gpu_pool_table t = NULL; nvn_gpu_err err = {0};
    nvn_gpu_rc rc = nvn_gpu_pool_create(&d, &t, &err);
    free(items);
    if (!nvn_gpu_ok(rc)) {
        *why = rc.code == NVN_GPU_E_UNSUPPORTED ? "pool_table_unsupported" : "pool_table_create_failed";
        if (nvn_unimpl("tex", *why, NULL) == 1 && err.why[0]) nvn_plat_log(NVN_PLAT_LOG_RED, "tex", err.why);
        return NULL;
    }
    // 序号纪元取建完之后的：本次建表自己取视图会建存储、让序号纪元前进（不是别人改了映射）；登记纪元用建表前读的，建表期间有新登记就下一笔再建
    pthread_mutex_lock(&g_rmu); sg = g_ser_gen; pthread_mutex_unlock(&g_rmu);
    g_pool_builds++; g_pool_items += n;
    if (slot < 0) { for (int k = 0; k < POOL_CACHE_N && slot < 0; k++) if (!g_pcache[k].used) slot = k;
                    if (slot < 0) slot = (int)(g_pcache_next++ % POOL_CACHE_N); }
    if (g_pcache[slot].used) nvn_gpu_pool_release(g_pcache[slot].t);
    g_pcache[slot].used = 1; g_pcache[slot].samp = samp; g_pcache[slot].base = base; g_pcache[slot].n = n;
    g_pcache[slot].dtt = dtt; g_pcache[slot].dcls = dcls; g_pcache[slot].treg_gen = tg; g_pcache[slot].ser_gen = sg; g_pcache[slot].t = t;
    if (!(g_pool_builds & (g_pool_builds - 1))) {   // 到 2 的幂打一行：重建频率与项数（流式加载时若每帧重建，这里看得出）
        char m[160]; snprintf(m, sizeof m, "整池数组快照：第 %llu 次建表，累计 %llu 项（本次 %s 池 %u 项，起点 %d）",
                              (unsigned long long)g_pool_builds, (unsigned long long)g_pool_items, samp ? "采样器" : "贴图", n, base);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "tex", m);
    }
    return t;
}
static void pool_cache_reset(void) {
    for (int k = 0; k < POOL_CACHE_N; k++) if (g_pcache[k].used) nvn_gpu_pool_release(g_pcache[k].t);
    memset(g_pcache, 0, sizeof g_pcache);
}

void rp_res_reset(void) {
    pool_cache_reset();   // 池快照先放（引用着零贴图 / 视图 / 采样器）
    pthread_mutex_lock(&g_rmu);
    for (int i = 0; i < g_ntreg; i++) treg_drop_view(&g_treg[i]);
    vf_drop_serial(0);
    for (int target = 0; target <= NVN_TEXTURE_TARGET_BUFFER; target++) for (int cls = 0; cls < 4; cls++) {
        if (g_zero[target][cls].view) nvn_gpu_view_release(g_zero[target][cls].view);
        if (g_zero[target][cls].tex) nvn_gpu_tex_release(g_zero[target][cls].tex);
    }
    memset(g_zero, 0, sizeof g_zero); g_zero_cube_logged = 0;
    for (int i = 0; i < g_nsreg; i++) { if (g_sreg[i].s) nvn_gpu_samp_release(g_sreg[i].s); if (g_sreg[i].sn) nvn_gpu_samp_release(g_sreg[i].sn); }
    memset(g_fmt_caps, 0, sizeof g_fmt_caps);
    if (g_query_sampler) nvn_gpu_samp_release(g_query_sampler);
    g_query_sampler = NULL;
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
