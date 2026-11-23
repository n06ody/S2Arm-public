// 管线预热清单的实现，合同见 nvn_vk_pipe_warm.h。
//
// 数据：固定槽位数组（上限 debug.s2a.nvk_warm_max，缺省 8192、也是硬顶）+ 每条一个散列桶链。散列只作预筛，
// 命中要 identity + 紧凑配方整份逐字节相等（旧层按 64 位散列直接命中撞过一次：同 hash 不同配方会
// 复用不兼容管线）。note 在提交线程（每次「管线状态变了」一笔，世界场景里几乎每笔绘制一次），所以：
// 同一条后端管线第二次起按管线标签只加计数（nvk_pipe_warm_note_prog：不序列化、不散列、不查表）；
// 标签对不上的才走完整路径——先算散列、拿最近 4 条 memo 挡掉重复，只有新配方才整份拷进清单。
#define _GNU_SOURCE 1
#include "direct/installed_seed.h"
#include "nvn_vk_pipe_warm.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "nvn_vk_warm_options.h"
#include "nvn_plat.h"     // nvn_plat_flag / nvn_plat_log
#include "rp_shader.h"    // struct rp_prog / rp_shader_prog_identity

// 会话缓存目录（android/native/gfx_plat/nvk_session.c 定义；会话没开返回 NULL）。声明成弱符号：
// 宿主单测不链会话层，没有它就按「目录还没出来」处理（load 返回 -1 让预热线程稍后重试）。
const char *nvk_session_cache_dir(void) __attribute__((weak));
// 后端协作（backend_vulkan.c）。宿主单测不链后端：缺了就跳过打标 / 计数，其余照做。
void nvn_gpu_pipe_warm_scope(int on) __attribute__((weak));
uint64_t nvn_gpu_pipe_warm_hits(void) __attribute__((weak));
uint64_t nvn_gpu_pipe_warm_created(void) __attribute__((weak));
uint64_t nvn_gpu_pipe_warm_tag(nvn_gpu_pipe p) __attribute__((weak));
void nvn_gpu_pipe_warm_tag_set(nvn_gpu_pipe p, uint64_t tag) __attribute__((weak));
// 程序清单（nvn_vk_prog_warm.c）：随包种子的状态与「这个身份在不在程序清单里」。宿主单测只链本模块时没有它们 = 不读管线种子。
int nvk_prog_warm_seed_dir(char *out, size_t n) __attribute__((weak));
int nvk_prog_warm_contains(const uint8_t identity[32]) __attribute__((weak));

// ---- 文件格式 ----
// 头 32 字节：magic(8) + 格式版本 + 描述版本（=紧凑配方字节数）+ 条目大小 + 条数 + FNV-1a 整份校验。
// 条目 388 字节：程序身份(32) + 紧凑配方(NVK_PW_DESC_BYTES) + uses(4)。任一项不符整份拒绝。
#define NVK_PW_FORMAT 1u
#define NVK_PW_ENTRY (32 + NVK_PW_DESC_BYTES + 4)
#define NVK_PW_HDR 32
#define NVK_PW_MAX_HARD 8192          // 内存上限：8192 × 400 B ≈ 3.2 MiB
#define NVK_PW_FILE_MAX (NVK_PW_HDR + (size_t)NVK_PW_MAX_HARD * NVK_PW_ENTRY)
#define NVK_PW_DEF_CAP 8192           // 缺省上限 = 硬顶：同一路线一局的管线配方撞满 8192（4096 时走动才用到的配方被挤掉，走进新区域现建管线）
_Static_assert(NVK_PW_DEF_CAP <= NVK_PW_MAX_HARD, "缺省上限不能超过硬顶");
#define NVK_PW_MEMO 4

static const char k_magic[8] = {'S', '2', 'A', 'P', 'W', '1', 0, 0};

// ---- 逐字段序列化的布局自守恒：改了 NVN 状态记录的字节宽或漏了字段，都要在这一行报错 ----
_Static_assert(sizeof(blend_st) == 8 && sizeof(ds_st) == 8 && sizeof(poly_st) == 4 && sizeof(va_st) == 4 &&
               sizeof(vs_st) == 8 && sizeof(color_st) == 4 && sizeof(chmask_st) == 4 && sizeof(ms_st) == 10,
               "NVN 状态记录字节宽变了：nvk_pw_desc_write / pw_desc_read 与 NVK_PW_DESC_BYTES 要同批改");
#define NVK_PW_DESC_SUM (1u + 8u * 4u + 12u + 8u + 8u + 1u + 8u * 8u + 4u + 4u + 10u + 16u * 4u + 16u * 8u + 8u + 4u + 4u)
_Static_assert(NVK_PW_DESC_SUM == NVK_PW_DESC_BYTES, "紧凑配方的字段长度与 NVK_PW_DESC_BYTES 不一致");

// ---- 状态。全部字段受 g.mu 保护；只有这一把锁 ----
static struct pw_state {
    pthread_mutex_t mu;
    char dir[480];
    char path[520];
    int inited;             // 槽位数组 / 桶已分配
    int loaded;             // load 已经成功走过一次
    uint32_t cap;
    uint32_t nbucket;       // cap 向上取 2 的幂
    uint32_t count;
    // 本局预热集：load 时冻结的一份值拷贝（程序身份 + 紧凑配方），take 只走它。运行期的 note / 淘汰 /
    // 热点重排（note_gen → order 重建）只写 g.recs / g.order，只影响下一局清单——否则 take 的游标会走在
    // 正在被重排的 order 上：同一配方取两次、另一个配方永远漏掉（外部复核 pipe_order_probe 的 A→A）。
    struct nvk_pipe_rec *plan;
    uint32_t plan_n, plan_i;
    struct nvk_pipe_rec *recs;
    uint32_t *uses;         // 与 recs 平行：使用次数（落盘用）
    uint32_t *seen;         // 最近使用序号（同 uses 的淘汰顺序）
    uint32_t *gen;          // 槽代号：每往槽里写一份新配方（入清单 / 淘汰重用 / 重载）就取一个新值，标签靠它认「还是当初那一份」
    uint32_t gen_clock;     // 槽代号发号器（从 1 起；0 不会发出去，所以全 0 的标签永远对不上）
    uint64_t *hash;         // 配方散列（桶定位）
    int32_t *next;          // 桶链
    int32_t *bucket;        // 桶头（-1 空）
    uint8_t *used;          // 槽位占用位
    uint8_t *fresh;         // 本轮碰过（新记 / 命中 / 标签刷新）= 1；载入时全清 0；满表淘汰先挑没碰过的（见 pw_victim_locked）
    int32_t *order;         // take / 落盘的顺序（uses 降序、同 uses 最近优先）
    uint64_t order_gen, note_gen;
    uint64_t clock;         // 单调使用序号
    uint64_t dirty;
    struct { uint64_t h; int32_t slot; } memo[NVK_PW_MEMO];
    struct nvk_pipe_warm_stats st;
    uint64_t save_ok, save_fail, save_skip;
    uint64_t save_gen, published_gen;   // 快照代际：旧快照不许覆盖新快照（保存串行下的兜底断言）
} g = { .mu = PTHREAD_MUTEX_INITIALIZER };
// 落盘串行：周期保存与停局保存不并发（否则两个 writer 抢同一个 .tmp / rename 目标，新快照会被旧快照覆盖；
// 外部复核 pipe_save_race_probe 复现：两条的文件被旧快照覆写后，新进程加载返回 0）。
static pthread_mutex_t g_save_mu = PTHREAD_MUTEX_INITIALIZER;

static void pw_log(int red, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void pw_log(int red, const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    nvn_plat_log(red == 2 ? NVN_PLAT_LOG_WARN : red ? NVN_PLAT_LOG_RED : NVN_PLAT_LOG_INFO, "pipe", b);   // red：0 普通 / 1 红 / 2 黄
}

static int pw_id_zero(const uint8_t id[32]) {
    for (int i = 0; i < 32; i++) if (id[i]) return 0;
    return 1;
}

// ---- 逐字段序列化（小端） ----
static void pw_put32(uint8_t *o, size_t *at, uint32_t v) {
    o[(*at)++] = (uint8_t)v; o[(*at)++] = (uint8_t)(v >> 8);
    o[(*at)++] = (uint8_t)(v >> 16); o[(*at)++] = (uint8_t)(v >> 24);
}
static void pw_put64(uint8_t *o, size_t *at, uint64_t v) {
    for (int i = 0; i < 8; i++) o[(*at)++] = (uint8_t)(v >> (8 * i));
}
static uint32_t pw_get32(const uint8_t *b, size_t *at) {
    uint32_t v = (uint32_t)b[*at] | (uint32_t)b[*at + 1] << 8 | (uint32_t)b[*at + 2] << 16 | (uint32_t)b[*at + 3] << 24;
    *at += 4;
    return v;
}
static uint64_t pw_get64(const uint8_t *b, size_t *at) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)b[*at + i] << (8 * i);
    *at += 8;
    return v;
}

// 一条管线描述 → 紧凑字节。st[].sh / nspec / spec / _rsv_prim 不进记录：sh 由程序身份还原，
// nspec 恒 0（note 处已挡），spec 是它的空参数区，_rsv_prim 是保留位。
static void pw_desc_write(const nvn_gpu_pipe_desc *d, uint8_t *o) {
    size_t at = 0;
    uint8_t smask = 0;
    for (int st = 0; st < 5; st++) if (d->st[st].sh) smask |= (uint8_t)(1u << st);
    o[at++] = smask;
    for (int i = 0; i < 8; i++) pw_put32(o, &at, d->cfmt[i]);
    pw_put32(o, &at, d->dfmt);
    pw_put32(o, &at, d->sfmt);
    pw_put32(o, &at, d->samples);
    for (int i = 0; i < 8; i++) o[at++] = d->lmap[i];
    pw_put32(o, &at, d->prim);
    pw_put32(o, &at, d->patch_size);
    o[at++] = d->prim_restart;
    for (int i = 0; i < 8; i++) {
        const blend_st *b = &d->blend[i];
        pw_put64(o, &at, (uint64_t)b->target | (uint64_t)b->src_rgb << 3 | (uint64_t)b->dst_rgb << 10 |
                         (uint64_t)b->src_a << 17 | (uint64_t)b->dst_a << 24 | (uint64_t)b->eq_rgb << 31 |
                         (uint64_t)b->eq_a << 34 | (uint64_t)b->adv_mode << 37 | (uint64_t)b->adv_overlap << 43 |
                         (uint64_t)b->adv_premul << 45 | (uint64_t)b->adv_normdst << 46);
    }
    pw_put32(o, &at, (uint32_t)d->color.blend_en | (uint32_t)d->color.logic_op << 8 | (uint32_t)d->color.alpha_func << 12);
    pw_put32(o, &at, d->chmask.mask);
    { const uint8_t *m = &d->ms.enable; for (int i = 0; i < 10; i++) o[at++] = m[i]; }
    for (int i = 0; i < 16; i++)
        pw_put32(o, &at, (uint32_t)d->attr[i].fmt | (uint32_t)d->attr[i].offset << 8 | (uint32_t)d->attr[i].stream << 27);
    for (int i = 0; i < 16; i++) { pw_put32(o, &at, d->stream[i].stride); pw_put32(o, &at, d->stream[i].divisor); }
    { const ds_st *s = &d->ds;
      pw_put64(o, &at, (uint64_t)s->dt | (uint64_t)s->dw << 1 | (uint64_t)s->df << 2 | (uint64_t)s->st << 6 |
                       (uint64_t)s->sf_f << 7 | (uint64_t)s->sf_b << 11 | (uint64_t)s->fail_f << 15 |
                       (uint64_t)s->zfail_f << 19 | (uint64_t)s->zpass_f << 23 | (uint64_t)s->fail_b << 27 |
                       (uint64_t)s->zfail_b << 31 | (uint64_t)s->zpass_b << 35); }
    pw_put32(o, &at, (uint32_t)d->poly.cull | (uint32_t)d->poly.front << 2 | (uint32_t)d->poly.mode << 3 |
                     (uint32_t)d->poly.offset_en << 5);
    o[at++] = d->rast_discard;
    o[at++] = d->clip_neg1;
    o[at++] = d->origin_ll;
    o[at++] = d->depth_clamp;
}

// 紧凑字节 → 一条 memset 过的管线描述（st[].sh 留空，由 nc 侧用程序身份填）。0 = 字节数不符。
static int pw_desc_read(const uint8_t *b, uint32_t bytes, nvn_gpu_pipe_desc *d) {
    if (bytes != NVK_PW_DESC_BYTES) return 0;
    memset(d, 0, sizeof *d);
    size_t at = 0;
    uint8_t smask = b[at++];
    if (smask & ~0x1fu) return 0;   // 只认 5 个图形阶段
    d->st[0].nspec = 0;             // 记录里不存：恒 0（留一行让人看清这里为什么不读）
    for (int i = 0; i < 8; i++) d->cfmt[i] = pw_get32(b, &at);
    d->dfmt = pw_get32(b, &at);
    d->sfmt = pw_get32(b, &at);
    d->samples = pw_get32(b, &at);
    for (int i = 0; i < 8; i++) d->lmap[i] = b[at++];
    d->prim = pw_get32(b, &at);
    d->patch_size = pw_get32(b, &at);
    d->prim_restart = b[at++];
    for (int i = 0; i < 8; i++) {
        uint64_t v = pw_get64(b, &at);
        blend_st *o = &d->blend[i];
        o->target = v & 0x7u;
        o->src_rgb = (v >> 3) & 0x7fu;
        o->dst_rgb = (v >> 10) & 0x7fu;
        o->src_a = (v >> 17) & 0x7fu;
        o->dst_a = (v >> 24) & 0x7fu;
        o->eq_rgb = (v >> 31) & 0x7u;
        o->eq_a = (v >> 34) & 0x7u;
        o->adv_mode = (v >> 37) & 0x3fu;
        o->adv_overlap = (v >> 43) & 0x3u;
        o->adv_premul = (v >> 45) & 0x1u;
        o->adv_normdst = (v >> 46) & 0x1u;
    }
    { uint32_t v = pw_get32(b, &at);
      d->color.blend_en = v & 0xffu; d->color.logic_op = (v >> 8) & 0xfu; d->color.alpha_func = (v >> 12) & 0xfu; }
    d->chmask.mask = pw_get32(b, &at);
    { uint8_t *m = &d->ms.enable; for (int i = 0; i < 10; i++) m[i] = b[at++]; }
    for (int i = 0; i < 16; i++) {
        uint32_t v = pw_get32(b, &at);
        d->attr[i].fmt = v & 0xffu;
        d->attr[i].offset = (v >> 8) & 0x7ffffu;
        d->attr[i].stream = (v >> 27) & 0x1fu;
    }
    for (int i = 0; i < 16; i++) { d->stream[i].stride = pw_get32(b, &at); d->stream[i].divisor = pw_get32(b, &at); }
    { uint64_t v = pw_get64(b, &at);
      ds_st *o = &d->ds;
      o->dt = v & 0x1u; o->dw = (v >> 1) & 0x1u; o->df = (v >> 2) & 0xfu;
      o->st = (v >> 6) & 0x1u; o->sf_f = (v >> 7) & 0xfu; o->sf_b = (v >> 11) & 0xfu;
      o->fail_f = (v >> 15) & 0xfu; o->zfail_f = (v >> 19) & 0xfu;
      o->zpass_f = (v >> 23) & 0xfu; o->fail_b = (v >> 27) & 0xfu;
      o->zfail_b = (v >> 31) & 0xfu; o->zpass_b = (v >> 35) & 0xfu; }
    { uint32_t v = pw_get32(b, &at);
      d->poly.cull = v & 0x3u; d->poly.front = (v >> 2) & 0x1u; d->poly.mode = (v >> 3) & 0x3u;
      d->poly.offset_en = (v >> 5) & 0x7u; }
    d->rast_discard = b[at++];
    d->clip_neg1 = b[at++];
    d->origin_ll = b[at++];
    d->depth_clamp = b[at++];
    return 1;
}

// ---- 散列 / 判等 ----
// 只需快筛：命中一律再看 identity + 整份字节（pw_same）。
static uint64_t pw_hash(const uint8_t *desc, const uint8_t ident[32]) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < NVK_PW_DESC_BYTES; i++) h = (h ^ desc[i]) * 1099511628211ull;
    for (int i = 0; i < 32; i++) h = (h ^ ident[i]) * 1099511628211ull;
    return h;
}

static int pw_same(const struct nvk_pipe_rec *r, const uint8_t *desc, const uint8_t ident[32]) {
    return r->bytes == NVK_PW_DESC_BYTES && memcmp(r->desc, desc, NVK_PW_DESC_BYTES) == 0 &&
           memcmp(r->prog_identity, ident, 32) == 0;
}

// ---- 槽位与桶 ----
static uint32_t pw_cap_from_flag(void) {
    int v = nvn_plat_flag("nvk_warm_max", NVK_PW_DEF_CAP);
    if (v < 1) v = 1;
    if (v > NVK_PW_MAX_HARD) v = NVK_PW_MAX_HARD;
    return (uint32_t)v;
}

static uint32_t pw_bucket_of(uint64_t h, uint32_t nbucket) { return (uint32_t)(h & (nbucket - 1)); }

// 分配槽位数组（幂等）。失败返回 -1：note 只计数，不影响绘制。
static int pw_init_locked(void) {
    if (g.inited) return 0;
    g.cap = pw_cap_from_flag();
    uint32_t nb = 1;
    while (nb < g.cap) nb <<= 1;
    g.nbucket = nb;
    g.recs = calloc(g.cap, sizeof *g.recs);
    g.uses = calloc(g.cap, sizeof *g.uses);
    g.seen = calloc(g.cap, sizeof *g.seen);
    g.gen = calloc(g.cap, sizeof *g.gen);
    g.hash = calloc(g.cap, sizeof *g.hash);
    g.next = calloc(g.cap, sizeof *g.next);
    g.used = calloc(g.cap, 1);
    g.fresh = calloc(g.cap, 1);
    g.order = calloc(g.cap, sizeof *g.order);
    g.bucket = malloc((size_t)nb * sizeof *g.bucket);
    if (!g.recs || !g.uses || !g.seen || !g.gen || !g.hash || !g.next || !g.used || !g.fresh || !g.order || !g.bucket) {
        free(g.recs); free(g.uses); free(g.seen); free(g.gen); free(g.hash); free(g.next); free(g.used); free(g.fresh); free(g.order); free(g.bucket);
        g.recs = NULL; g.uses = NULL; g.seen = NULL; g.gen = NULL; g.hash = NULL; g.next = NULL; g.used = NULL; g.fresh = NULL; g.order = NULL; g.bucket = NULL;
        return -1;
    }
    for (uint32_t i = 0; i < nb; i++) g.bucket[i] = -1;
    for (uint32_t i = 0; i < g.cap; i++) g.next[i] = -1;
    for (int i = 0; i < NVK_PW_MEMO; i++) g.memo[i].slot = -1;
    g.inited = 1;
    return 0;
}

static void pw_unlink_locked(uint32_t slot) {
    uint32_t b = pw_bucket_of(g.hash[slot], g.nbucket);
    int32_t cur = g.bucket[b], prev = -1;
    while (cur >= 0) {
        if ((uint32_t)cur == slot) {
            if (prev < 0) g.bucket[b] = g.next[slot];
            else g.next[prev] = g.next[slot];
            g.next[slot] = -1;
            return;
        }
        prev = cur;
        cur = g.next[cur];
    }
}

static void pw_memo_drop_locked(int32_t slot) {
    for (int i = 0; i < NVK_PW_MEMO; i++) if (g.memo[i].slot == slot) { g.memo[i].slot = -1; g.memo[i].h = 0; }
}

static void pw_memo_touch_locked(uint64_t h, int32_t slot) {
    int at = -1;
    for (int i = 0; i < NVK_PW_MEMO; i++) if (g.memo[i].slot == slot) { at = i; break; }
    if (at > 0) {
        uint64_t mh = g.memo[at].h; int32_t ms = g.memo[at].slot;
        for (int k = at; k > 0; k--) g.memo[k] = g.memo[k - 1];
        g.memo[0].h = mh; g.memo[0].slot = ms;
        return;
    }
    if (at == 0) { g.memo[0].h = h; g.memo[0].slot = slot; return; }
    for (int k = NVK_PW_MEMO - 1; k > 0; k--) g.memo[k] = g.memo[k - 1];
    g.memo[0].h = h; g.memo[0].slot = slot;
}

// ---- 诊断（debug.s2a.nvk_pw_diag，缺省 0；#1372）----
// 清单满了以后每新记一条都要整表扫一遍挑淘汰目标。站着不动的场景里这一项每秒都在发生，原因有两种，修法不同：
//   (a) 颠簸：被淘汰的配方过一会儿又要用、再插回来；  (b) 配方里混进了逐帧变化的量：同一条管线每帧都被当成新配方。
// 这里只加计数和日志来分辨它们，**不改清单的任何判定、顺序和落盘字节**（用例：诊断关（开关不设 = 缺省）/ 开两遍同一串 note，
// 落盘文件逐字节相同）。玩家包的 nvn_plat_flag 恒取缺省 0 → dg.on == 0，下面每个入口一进来就返回：不分配、不读时钟、不打日志。
// 诊断开着时不污染读数：持 g.mu 期间只做计数、淘汰散列集合的查 / 插，以及（每窗口每类至多一次）一份清单快照的内存拷贝；
// 整表比对和所有日志都在 g.mu 之外做（用例：每条诊断行出来时另起线程去拿 g.mu，拿不到就红）。
// 状态受 g.mu 保护（与清单同一把锁，不另立锁），snap_busy 例外（原子读写）。
#define PW_DG_WINDOW_NS 5000000000ull   // 窗口：5 秒
#define PW_DG_EV_BITS 17                // 淘汰散列集合：2^17 项 × 8 字节 = 1 MiB，只在诊断开时分配
#define PW_DG_SAMPLE_MAX 24             // 两类样本各最多输出这么多条（每个窗口每类 1 条 → 约 2 分钟）
#define PW_DG_DIFF_SHOW 6               // 每条样本最多点名的差异字节数

static struct pw_dg_state {
    int on;                             // -1 = 还没读开关；0 = 关；1 = 开
    uint64_t t0;                        // 本窗口起点（CLOCK_MONOTONIC，纳秒）
    struct nvk_pipe_warm_diag c, c0;    // 累计 / 窗口起点时的累计（窗口 = 两者之差）
    uint64_t *ev;                       // 被淘汰过的配方散列（开放寻址，0 = 空；装满 3/4 清空重来）
    uint32_t ev_n;
    int win_s_fresh, win_s_reins;       // 本窗口两类样本各抽过没有
    struct nvk_pipe_rec *snap;          // 抽样用的清单快照：g.cap 条（缺省 ≈ 3.2 MiB），只在诊断开时分配；抢到名额时持锁拷一份，锁外比对
    uint32_t snap_n;                    // 快照里的条数
    int snap_busy;                      // 1 = 快照正在被锁外比对（原子读写）；忙时不抢新名额，免得它在比对中途被覆盖
} dg = { .on = -1 };

// 持锁时备好、解锁后输出的东西：每次 note 一份，放在调用者栈上；诊断关着时不初始化、不读。
struct pw_dg_out {
    char line[704];                     // 要打的汇总行（开机行 / 窗口行）；空串 = 没有
    int sample;                         // 1 = 要在锁外抽样，下面几项有效
    int reins;                          // 样本类别：1 = 被淘汰过又插回来；0 = 此前没见过
    uint64_t nth;                       // 本类第几条样本
    uint64_t snap_us;                   // 持锁拷快照用了多少微秒
    uint8_t desc[NVK_PW_DESC_BYTES];    // 这条新记配方
    uint8_t ident[32];
};

static uint64_t pw_dg_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t pw_dg_home(uint64_t key) { return (uint32_t)(key ^ (key >> 32)) & ((1u << PW_DG_EV_BITS) - 1u); }

static int pw_dg_ev_has(uint64_t h) {
    if (!dg.ev) return 0;
    const uint32_t mask = (1u << PW_DG_EV_BITS) - 1u;
    const uint64_t key = h ? h : 1;   // 0 留给「空」
    for (uint32_t i = pw_dg_home(key);; i = (i + 1) & mask) {   // 装载率 ≤ 3/4：一定遇得到空槽
        if (!dg.ev[i]) return 0;
        if (dg.ev[i] == key) return 1;
    }
}

static void pw_dg_ev_add(uint64_t h) {
    if (!dg.ev) return;
    const uint32_t mask = (1u << PW_DG_EV_BITS) - 1u;
    const uint64_t key = h ? h : 1;
    if (dg.ev_n >= (mask + 1u) / 4u * 3u) {   // 装满 3/4：清空重来；清空之后的「插回」会漏数，计 ev_clears
        memset(dg.ev, 0, ((size_t)mask + 1u) * sizeof *dg.ev);
        dg.ev_n = 0;
        dg.c.ev_clears++;
    }
    for (uint32_t i = pw_dg_home(key);; i = (i + 1) & mask) {
        if (!dg.ev[i]) { dg.ev[i] = key; dg.ev_n++; return; }
        if (dg.ev[i] == key) return;
    }
}

// 紧凑配方字节偏移 → 字段名（布局 = pw_desc_write 的写入顺序）。
_Static_assert(NVK_PW_DESC_BYTES == 352, "诊断的字段名表按 352 字节布局写：pw_desc_write 改了布局，要同批改 pw_dg_field");
static void pw_dg_field(uint32_t off, char *out, size_t n) {
    static const char *const ms_f[10] = {"enable", "samples", "a2c", "a2c_dither", "raster_samples",
                                         "cov_mod", "cov2color_en", "cov2color_out", "sl_en", "sl_grid"};
    if (off == 0) snprintf(out, n, "stage_mask");
    else if (off < 33) snprintf(out, n, "cfmt[%u]", (unsigned)((off - 1) / 4));
    else if (off < 37) snprintf(out, n, "dfmt");
    else if (off < 41) snprintf(out, n, "sfmt");
    else if (off < 45) snprintf(out, n, "samples");
    else if (off < 53) snprintf(out, n, "lmap[%u]", (unsigned)(off - 45));
    else if (off < 57) snprintf(out, n, "prim");
    else if (off < 61) snprintf(out, n, "patch_size");
    else if (off < 62) snprintf(out, n, "prim_restart");
    else if (off < 126) snprintf(out, n, "blend[%u]", (unsigned)((off - 62) / 8));
    else if (off < 130) snprintf(out, n, "color");
    else if (off < 134) snprintf(out, n, "chmask");
    else if (off < 144) snprintf(out, n, "ms.%s", ms_f[off - 134]);
    else if (off < 208) snprintf(out, n, "attr[%u]", (unsigned)((off - 144) / 4));
    else if (off < 336) snprintf(out, n, "stream[%u].%s", (unsigned)((off - 208) / 8), ((off - 208) % 8) < 4 ? "stride" : "divisor");
    else if (off < 344) snprintf(out, n, "ds");
    else if (off < 348) snprintf(out, n, "poly");
    else if (off == 348) snprintf(out, n, "rast_discard");
    else if (off == 349) snprintf(out, n, "clip_neg1");
    else if (off == 350) snprintf(out, n, "origin_ll");
    else if (off == 351) snprintf(out, n, "depth_clamp");
    else snprintf(out, n, "?");
}

// 锁外抽样：在快照里找与这条新配方最近的一条（程序身份相同优先，身份不同记 1000 分；再比配方字节差，差得最少的），
// 打出身份是否相同、差几个字节、差在哪些字段（偏移、旧字节→新字节），以及这次抽样的耗时。整表比对（≤ 8192 × 352 字节）约 1–3 毫秒。
// 快照是抢到名额那一刻（持锁）拷的，snap_busy 放回之前别的线程不会去改它；这里不碰 g.*，所以比对多久都不挡回放线程。
static void pw_dg_sample_run(const struct pw_dg_out *o) {
    const uint64_t t0 = pw_dg_now();
    int32_t best = -1;
    uint32_t best_score = 0xffffffffu;
    for (uint32_t i = 0; i < dg.snap_n; i++) {
        const struct nvk_pipe_rec *r = &dg.snap[i];
        if (r->bytes != NVK_PW_DESC_BYTES) continue;
        uint32_t score = memcmp(r->prog_identity, o->ident, 32) == 0 ? 0u : 1000u;
        for (uint32_t j = 0; j < NVK_PW_DESC_BYTES && score < best_score; j++)
            if (r->desc[j] != o->desc[j]) score++;
        if (score < best_score) { best_score = score; best = (int32_t)i; }
    }
    if (best >= 0) {
        const struct nvk_pipe_rec *r = &dg.snap[best];
        char det[360];
        size_t dn = 0;
        uint32_t ndiff = 0;
        det[0] = 0;
        for (uint32_t j = 0; j < NVK_PW_DESC_BYTES; j++) {
            if (r->desc[j] == o->desc[j]) continue;
            if (ndiff < PW_DG_DIFF_SHOW && dn < sizeof det) {
                char nm[40];
                pw_dg_field(j, nm, sizeof nm);
                int w = snprintf(det + dn, sizeof det - dn, "%s%s@%u %02x→%02x", ndiff ? "、" : "", nm, (unsigned)j,
                                 (unsigned)r->desc[j], (unsigned)o->desc[j]);
                if (w > 0) dn += (size_t)w;
            }
            ndiff++;
        }
        char line[640];
        snprintf(line, sizeof line,
                 "预热清单诊断 样本（%s，第 %llu 条）：最近条目程序身份%s，配方差 %u 字节%s%s；快照拷贝 %llu 微秒（持锁）、比对 %llu 微秒（锁外）",
                 o->reins ? "被淘汰过又插回来" : "此前没见过", (unsigned long long)o->nth,
                 memcmp(r->prog_identity, o->ident, 32) == 0 ? "相同" : "不同", (unsigned)ndiff, ndiff ? "：" : "", det,
                 (unsigned long long)o->snap_us, (unsigned long long)((pw_dg_now() - t0) / 1000u));
        nvn_plat_log(NVN_PLAT_LOG_INFO, "pipe", line);   // 不走 pw_log：它的 512 字节缓冲放不下这一行
    }
    __atomic_store_n(&dg.snap_busy, 0, __ATOMIC_RELEASE);   // 比对完了：快照缓冲放回，下一个窗口的名额可以再用
}

// 一个窗口一行（持锁只格式化进 o->line，解锁后再打）：窗口内的 note 次数（memo / 链 / 新记）、新记拆成「此前没见过」与
// 「被淘汰过又插回」、满后淘汰、淘汰扫描次数与总耗时、淘汰集合清空次数（本窗口 / 累计：清空之后的「插回」会漏数，
// 读到「此前没见过」偏高时要靠它分清是真新还是集合被清过）、当前条数 / 容量；后面带累计值。然后开新窗口。
static void pw_dg_window_locked(struct pw_dg_out *o, uint64_t now) {
    const struct nvk_pipe_warm_diag *a = &dg.c, *b = &dg.c0;
    snprintf(o->line, sizeof o->line,
             "预热清单诊断 %.1f 秒：note %llu 次（memo %llu / 链 %llu / 新记 %llu）；新记里 此前没见过 %llu、被淘汰过又插回 %llu；"
             "满后淘汰 %llu 次；淘汰扫描 %llu 次共 %llu 微秒；淘汰集合清空 %llu 次（累计 %llu）；条数 %u / 容量 %u；"
             "累计 新记 %llu（没见过 %llu、插回 %llu）、淘汰 %llu、扫描 %llu 次共 %llu 微秒；淘汰新一轮累计 %llu 次",
             (double)(now - dg.t0) / 1e9,
             (unsigned long long)(a->calls - b->calls), (unsigned long long)(a->memo_hits - b->memo_hits),
             (unsigned long long)(a->chain_hits - b->chain_hits), (unsigned long long)(a->news - b->news),
             (unsigned long long)(a->fresh - b->fresh), (unsigned long long)(a->reins - b->reins),
             (unsigned long long)(a->evicts - b->evicts), (unsigned long long)(a->scans - b->scans),
             (unsigned long long)((a->scan_ns - b->scan_ns) / 1000u),
             (unsigned long long)(a->ev_clears - b->ev_clears), (unsigned long long)a->ev_clears,
             (unsigned)g.count, (unsigned)g.cap,
             (unsigned long long)a->news, (unsigned long long)a->fresh, (unsigned long long)a->reins,
             (unsigned long long)a->evicts, (unsigned long long)a->scans, (unsigned long long)(a->scan_ns / 1000u),
             (unsigned long long)g.st.epochs);
    dg.c0 = dg.c;
    dg.t0 = now;
    dg.win_s_fresh = dg.win_s_reins = 0;
}

// 第一次进 note 时读开关；开着就分配淘汰集合与清单快照缓冲、记窗口起点，并把开机行备进 o->line。返回是否开着。
// 开着时每次调用都把 o 初始化（空行、不抽样）；关着时不碰 o。
static int pw_dg_on_locked(struct pw_dg_out *o) {
    if (dg.on < 0) {
        dg.on = nvn_plat_flag("nvk_pw_diag", 0) != 0;
        if (!dg.on) return 0;
        dg.ev = calloc((size_t)1 << PW_DG_EV_BITS, sizeof *dg.ev);
        dg.snap = calloc(g.cap, sizeof *dg.snap);
        if (dg.snap) memset(dg.snap, 0, (size_t)g.cap * sizeof *dg.snap);   // 开机时一次性把页占住：别等到第一次抽样拷贝时才在锁里缺页
        dg.t0 = pw_dg_now();
        snprintf(o->line, sizeof o->line,
                 "预热清单诊断：开（debug.s2a.nvk_pw_diag=1）；淘汰散列集合%s、清单快照缓冲%s；每 5 秒一行，样本每类最多 %d 条",
                 dg.ev ? "已分配" : "分配失败（「被淘汰过又插回」不计）", dg.snap ? "已分配" : "分配失败（不抽样）", PW_DG_SAMPLE_MAX);
        o->sample = 0;
        return 1;
    }
    if (dg.on > 0) { o->line[0] = 0; o->sample = 0; }
    return dg.on;
}

// note 每次进锁之后（诊断开着）：计数并看窗口到没到（到了就把窗口行备进 o->line；开机行占着时这次不出窗口行）。
static void pw_dg_call_locked(struct pw_dg_out *o) {
    dg.c.calls++;
    const uint64_t now = pw_dg_now();
    if (now - dg.t0 >= PW_DG_WINDOW_NS && !o->line[0]) pw_dg_window_locked(o, now);
}

// 新记一条之前（note 里，持锁）：归到「被淘汰过又插回」或「此前没见过」，并按窗口名额决定抽不抽样。
// 抽样 = 持锁只做内存拷贝：这条新配方 + 一份清单快照（看的是插入前的清单）；整表比对与日志在解锁后（pw_dg_out_run）做。
// 清单为空没有可比的 / 快照缓冲还在被上一次抽样占着 → 不抽、不占名额。
static void pw_dg_new_locked(struct pw_dg_out *o, const uint8_t *desc, const uint8_t ident[32], uint64_t h) {
    const int reins = pw_dg_ev_has(h);
    dg.c.news++;
    if (reins) dg.c.reins++; else dg.c.fresh++;
    int *won = reins ? &dg.win_s_reins : &dg.win_s_fresh;
    uint64_t *total = reins ? &dg.c.samples_reins : &dg.c.samples_fresh;
    if (*won || *total >= PW_DG_SAMPLE_MAX || !dg.snap || g.count == 0 || __atomic_load_n(&dg.snap_busy, __ATOMIC_ACQUIRE)) return;
    const uint64_t t0 = pw_dg_now();
    memcpy(dg.snap, g.recs, (size_t)g.count * sizeof *dg.snap);   // 槽位按顺序填，已用的就是 [0, count)
    dg.snap_n = g.count;
    o->snap_us = (pw_dg_now() - t0) / 1000u;
    memcpy(o->desc, desc, NVK_PW_DESC_BYTES);
    memcpy(o->ident, ident, 32);
    o->reins = reins;
    o->sample = 1;
    *won = 1;
    o->nth = ++*total;
    __atomic_store_n(&dg.snap_busy, 1, __ATOMIC_RELAXED);   // 持锁时置位；pw_dg_sample_run 比对完放回
}

// 解锁之后：把持锁时备好的行打出来，抽样的在这里做整表比对。
static void pw_dg_out_run(const struct pw_dg_out *o) {
    if (o->line[0]) nvn_plat_log(NVN_PLAT_LOG_INFO, "pipe", o->line);   // 不走 pw_log：它的 512 字节缓冲放不下窗口行
    if (o->sample) pw_dg_sample_run(o);
}

// 淘汰目标：表没满 = 第一个空槽。表满了：
//   · 先在「本轮没碰过」的条目（g.fresh == 0）里挑：uses 最小，同 uses 取最久没用的；
//   · 没有这样的条目（本轮碰过的配方把表占满）= 开新一轮：全部清成没碰过，再在全表里按同一规则挑。
// 为什么不是只看 uses：满表时载入进来的旧条目 uses 都 ≥ 2，而新记的配方 uses 才 1，只看 uses 的话每条新配方都只能挤掉
// 上一条新配方——同一批配方记了又被挤、挤了又记（真机 99.9% 的新记是「被淘汰过又插回」），标签永远立不住，
// 清单也学不到本局的配方。先挑没碰过的，新配方在旧条目挤光之前不会被下一条新配方挤掉；旧条目里仍是 uses 最小的先走，
// 热点旧配方留到最后。
static uint32_t pw_victim_locked(void) {
    uint32_t cold = UINT32_MAX, hot = UINT32_MAX;   // 没碰过的最小条目 / 碰过的最小条目
    for (uint32_t i = 0; i < g.cap; i++) {
        if (!g.used[i]) return i;
        uint32_t *best = g.fresh[i] ? &hot : &cold;
        if (*best == UINT32_MAX || g.uses[i] < g.uses[*best] || (g.uses[i] == g.uses[*best] && g.seen[i] < g.seen[*best])) *best = i;
    }
    if (cold != UINT32_MAX) return cold;
    memset(g.fresh, 0, g.cap);   // 全表都碰过了：新一轮，下一次起重新分「碰过 / 没碰过」
    g.st.epochs++;
    return hot;
}

// 槽代号：槽里每换一份配方就取一个新值（0 不发）。标签 = 代号 << 32 | (槽号 + 1)，所以 0 永远不是有效标签。
static uint32_t pw_next_gen_locked(void) {
    if (++g.gen_clock == 0) g.gen_clock = 1;   // 回绕后仍不发 0（回绕需要 40 亿次写槽，只为守住「0 = 没设过」这条不变式）
    return g.gen_clock;
}
static uint64_t pw_tag_of_locked(uint32_t slot) { return ((uint64_t)g.gen[slot] << 32) | (uint64_t)(slot + 1u); }

static uint32_t pw_insert_locked(const uint8_t *desc, const uint8_t ident[32], uint64_t h, uint32_t uses) {
    const uint64_t dg_t0 = dg.on > 0 ? pw_dg_now() : 0;
    uint32_t slot = pw_victim_locked();
    if (dg.on > 0) { dg.c.scans++; dg.c.scan_ns += pw_dg_now() - dg_t0; }   // 诊断：只计数，不影响上面的选择
    if (g.used[slot]) {
        if (dg.on > 0) { dg.c.evicts++; pw_dg_ev_add(g.hash[slot]); }      // 诊断：记下被淘汰配方的散列（覆盖之前读）
        __atomic_fetch_add(&g.st.rejected, 1, __ATOMIC_RELAXED);   // 上限满了淘汰最冷的：不拒绝新配方，代价记在这里
        pw_unlink_locked(slot);
        pw_memo_drop_locked((int32_t)slot);
    } else {
        g.count++;
    }
    struct nvk_pipe_rec *r = &g.recs[slot];
    memcpy(r->desc, desc, NVK_PW_DESC_BYTES);
    memcpy(r->prog_identity, ident, 32);
    r->bytes = NVK_PW_DESC_BYTES;
    r->uses = uses;
    g.uses[slot] = uses;
    g.seen[slot] = (uint32_t)++g.clock;
    g.gen[slot] = pw_next_gen_locked();
    g.hash[slot] = h;
    g.used[slot] = 1;
    g.fresh[slot] = 1;   // 新记进来的算本轮碰过：旧条目挤光之前不会被下一条新配方挤掉
    uint32_t b = pw_bucket_of(h, g.nbucket);
    g.next[slot] = g.bucket[b];
    g.bucket[b] = (int32_t)slot;
    g.note_gen++;
    g.dirty = 1;
    pw_memo_touch_locked(h, (int32_t)slot);
    return slot;
}

static void pw_touch_locked(uint32_t slot) {
    g.uses[slot]++;
    g.seen[slot] = (uint32_t)++g.clock;
    g.fresh[slot] = 1;   // 命中（含 memo / 桶链 / 标签快路径）也算本轮碰过
    g.recs[slot].uses = g.uses[slot];
    g.dirty = 1;
}

// ---- 用例 / 诊断入口 ----
void nvk_pipe_warm_rec_of(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32], struct nvk_pipe_rec *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!d || !prog_identity) return;
    for (int st = 0; st < 5; st++) if (d->st[st].nspec) return;   // 特化常量不进记录：bytes 留 0
    pw_desc_write(d, out->desc);
    memcpy(out->prog_identity, prog_identity, 32);
    out->bytes = NVK_PW_DESC_BYTES;
}

static int g_hash_const;   // 用例钩子：见 nvn_vk_pipe_warm.h（读写一律走 __atomic_*）

// 槽里存的散列：note 和 load 都从这里取，钩子开着时两边给同一个常数（否则载入的条目和之后 note 的散列对不上）。
static uint64_t pw_slot_hash(const uint8_t *desc, const uint8_t ident[32]) {
    return __atomic_load_n(&g_hash_const, __ATOMIC_ACQUIRE) ? 0x5a5a5a5a5a5a5a5aull : pw_hash(desc, ident);
}

uint64_t nvk_pipe_warm_hash(const struct nvk_pipe_rec *rec) {
    if (!rec || rec->bytes != NVK_PW_DESC_BYTES) return 0;
    if (__atomic_load_n(&g_hash_const, __ATOMIC_ACQUIRE)) return 0x5a5a5a5a5a5a5a5aull;
    return pw_hash(rec->desc, rec->prog_identity);
}

void nvk_pipe_warm_test_hash_const(int on) { __atomic_store_n(&g_hash_const, on ? 1 : 0, __ATOMIC_RELEASE); }

// ---- note（提交线程；第一次见到一条管线才走完整路径，之后按管线标签只加计数，见 nvk_pipe_warm_note_prog） ----
// 完整路径。*tag_out（可为 NULL）= 这份配方在清单里的标签；没收下（没身份 / nspec 非 0 / 分配失败）时给 0。
static void pw_note(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32], uint64_t *tag_out, int compute) {
    if (tag_out) *tag_out = 0;
    if (!d || !prog_identity) return;
    if (pw_id_zero(prog_identity)) return;   // 没有身份的程序不进清单（失败项 / 还没算出来）
    for (int st = 0; st < 5; st++)
        if (d->st[st].nspec) {   // 特化常量不进记录：那类配方本层不产生，宁可不预热也不建错
            __atomic_fetch_add(&g.st.rejected, 1, __ATOMIC_RELAXED);
            return;
        }
    uint8_t desc[NVK_PW_DESC_BYTES];
    if (compute) { memset(desc, 0, sizeof desc); desc[0] = 0x80; }
    else pw_desc_write(d, desc);
    uint64_t h = pw_slot_hash(desc, prog_identity);
    pthread_mutex_lock(&g.mu);
    if (pw_init_locked() != 0) { pthread_mutex_unlock(&g.mu); return; }
    struct pw_dg_out dgo;                         // 诊断：持锁时备好、解锁后输出的东西
    const int dgon = pw_dg_on_locked(&dgo);       // 诊断开关（缺省 0）：关着时 dgo 不初始化，下面所有 dg 钩子都不进
    if (dgon) pw_dg_call_locked(&dgo);
    for (int i = 0; i < NVK_PW_MEMO; i++) {   // 最近 4 条：同散列且整份相等就只 uses ++，不再拷配方
        int32_t slot = g.memo[i].slot;
        if (slot >= 0 && g.memo[i].h == h && pw_same(&g.recs[slot], desc, prog_identity)) {
            if (dgon) dg.c.memo_hits++;
            pw_touch_locked((uint32_t)slot);
            pw_memo_touch_locked(h, slot);
            if (tag_out) *tag_out = pw_tag_of_locked((uint32_t)slot);
            pthread_mutex_unlock(&g.mu);
            if (dgon) pw_dg_out_run(&dgo);
            return;
        }
    }
    {   // 桶链（memo 没挡住时：隔了 4 条以上又回到同一份配方，或第一次见）
        uint32_t b = pw_bucket_of(h, g.nbucket);
        for (int32_t slot = g.bucket[b]; slot >= 0; slot = g.next[slot])
            if (g.hash[slot] == h && pw_same(&g.recs[slot], desc, prog_identity)) {
                if (dgon) dg.c.chain_hits++;
                pw_touch_locked((uint32_t)slot);
                pw_memo_touch_locked(h, (int32_t)slot);
                if (tag_out) *tag_out = pw_tag_of_locked((uint32_t)slot);
                pthread_mutex_unlock(&g.mu);
                if (dgon) pw_dg_out_run(&dgo);
                return;
            }
    }
    __atomic_fetch_add(&g.st.recorded, 1, __ATOMIC_RELAXED);
    if (dgon) pw_dg_new_locked(&dgo, desc, prog_identity, h);   // 先归类 / 抽样（拷下插入前的清单），再插
    const uint32_t slot = pw_insert_locked(desc, prog_identity, h, 1);
    if (tag_out) *tag_out = pw_tag_of_locked(slot);
    pthread_mutex_unlock(&g.mu);
    if (dgon) pw_dg_out_run(&dgo);   // 解锁之后：打汇总行、抽样的做整表比对
}

void nvk_pipe_warm_note(const nvn_gpu_pipe_desc *d, const uint8_t prog_identity[32]) { pw_note(d, prog_identity, NULL, 0); }

// 标签快路径：标签里的槽还占着、槽代号没变（= 还是当初记下的那份配方），并且槽里那份配方的程序身份就是这次
// 传进来的程序，才只 uses++ / 刷新最近序号——与完整路径命中已有条目时做的事相同，只是不序列化配方、不散列、
// 不查备忘和桶链。程序身份这一项是前提的保险：今天一局内同一条管线对象（np）只对应一份程序，将来如果有人
// 让着色器句柄按内容合并、同一条 np 带着另一份程序身份回来，标签就不算命中，走完整路径按新身份入清单。
static int pw_touch_tag(uint64_t tag, const uint8_t ident[32]) {
    const uint32_t slot1 = (uint32_t)tag, gen = (uint32_t)(tag >> 32);
    int ok = 0;
    pthread_mutex_lock(&g.mu);
    if (g.inited && slot1 >= 1 && slot1 <= g.cap && g.used[slot1 - 1] && g.gen[slot1 - 1] == gen &&
        memcmp(g.recs[slot1 - 1].prog_identity, ident, 32) == 0) {
        pw_touch_locked(slot1 - 1);
        g.st.tag_hits++;
        ok = 1;
    }
    pthread_mutex_unlock(&g.mu);
    return ok;
}

int nvk_pipe_warm_note_prog(const nvn_gpu_pipe_desc *d, const struct rp_prog *p, nvn_gpu_pipe np) {
    uint8_t ident[32];
    if (!d || !p || (p->is_compute && !nvn_plat_flag("nvk_compute_warm", 0))) return 0;
    if (!rp_shader_prog_identity(p, ident)) return 0;   // 只读 32 字节，不加锁
    if (pw_id_zero(ident)) return 0;
    if (np && nvn_gpu_pipe_warm_tag) {
        const uint64_t t = nvn_gpu_pipe_warm_tag(np);
        if (t && pw_touch_tag(t, ident)) return 1;
    }
    uint64_t tag = 0;
    pw_note(d, ident, &tag, p->is_compute != 0);
    if (np && tag && nvn_gpu_pipe_warm_tag_set) nvn_gpu_pipe_warm_tag_set(np, tag);
    return 1;
}

// ---- take / pending（预热线程） ----
static int pw_order_cmp(const void *pa, const void *pb) {
    int32_t a = *(const int32_t *)pa, b = *(const int32_t *)pb;
    if (g.uses[a] != g.uses[b]) return g.uses[a] < g.uses[b] ? 1 : -1;   // uses 降序：热点先建
    if (g.seen[a] != g.seen[b]) return g.seen[a] < g.seen[b] ? 1 : -1;   // 同 uses：最近用过的在前
    return a < b ? -1 : 1;
}

static void pw_build_order_locked(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g.cap; i++) if (g.used[i]) g.order[n++] = (int32_t)i;
    qsort(g.order, n, sizeof *g.order, pw_order_cmp);
    g.order_gen = g.note_gen;
}

int nvk_pipe_warm_take(struct nvk_pipe_rec *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&g.mu);
    if (g.plan_i >= g.plan_n) { pthread_mutex_unlock(&g.mu); return 0; }
    *out = g.plan[g.plan_i++];   // 开局冻结快照，不可变：与 note / 淘汰 / 重排完全解耦
    pthread_mutex_unlock(&g.mu);
    return 1;
}

int nvk_pipe_warm_pending(void) {
    pthread_mutex_lock(&g.mu);
    int n = g.plan_i < g.plan_n ? (int)(g.plan_n - g.plan_i) : 0;
    pthread_mutex_unlock(&g.mu);
    return n;
}

// ---- build（预热线程） ----
// 管线预热（预热线程）：从程序身份借到程序，然后按记录里的紧凑配方建管线。
// 寿命合同（外部复核 P1）：这里不是「查一下马上用」——整段 build（读 prog->sh[]、建管线、装缓存）都可能与
// 会话收尾并发，所以必须先 rp_shader_prog_borrow 拿到正式借用；借用未归还前 rp_shader_reset 不会释放程序
// 记录、预热静默判据也不会判静。裸指针（rp_shader_prog_by_identity）只能用于「借用表可用」的快速判断。
int nvk_pipe_warm_build(const struct nvk_pipe_rec *rec) {
    if (!rec || rec->bytes != NVK_PW_DESC_BYTES) return -1;
    const struct rp_prog *prog = NULL;
    int br = rp_shader_prog_borrow(rec->prog_identity, &prog);
    if (br == 0) {
        __atomic_fetch_add(&g.st.retried, 1, __ATOMIC_RELAXED);
        return 1;   // 程序还没发布：预热线程隔几毫秒再试（不是失败）
    }
    if (br < 0 || !prog) {
        // 会话已停 / 借用表满：本局这条不建（如实计数，不假装建过）。
        __atomic_fetch_add(&g.st.failed, 1, __ATOMIC_RELAXED);
        pw_log(1, "管线预热：程序借用失败（停局 / 借用表满），整条跳过");
        return -1;
    }
    int rc = -1;
    const int compute = rec->desc[0] == 0x80;
    if (compute) {
        uint8_t extra = 0; for (size_t i = 1; i < NVK_PW_DESC_BYTES; i++) extra |= rec->desc[i];
        if (extra || !prog->is_compute || !prog->sh[0]) {
            __atomic_fetch_add(&g.st.failed, 1, __ATOMIC_RELAXED);
            pw_log(1, "管线预热：计算配方与程序不匹配，整条跳过");
            goto out;
        }
        nvn_gpu_stage_desc cs = {0}; cs.sh = prog->sh[0];
        nvn_gpu_pipe pipe = NULL; nvn_gpu_err err = {0};
        if (nvn_gpu_pipe_warm_scope) nvn_gpu_pipe_warm_scope(1);
        nvn_gpu_rc r = nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &pipe, &err);
        if (nvn_gpu_pipe_warm_scope) nvn_gpu_pipe_warm_scope(0);
        if (!nvn_gpu_ok(r) || !pipe) {
            __atomic_fetch_add(&g.st.failed, 1, __ATOMIC_RELAXED);
            pw_log(1, "计算管线预热失败（%d）：%s", (int)r.code, err.why);
            goto out;
        }
        __atomic_fetch_add(&g.st.built, 1, __ATOMIC_RELAXED);
        rc = 0;
        goto out;
    }
    if (prog->is_compute) {
        __atomic_fetch_add(&g.st.failed, 1, __ATOMIC_RELAXED);
        pw_log(1, "管线预热：图形配方对应计算程序，整条跳过");
        goto out;
    }
    nvn_gpu_pipe_desc pd;
    if (!pw_desc_read(rec->desc, rec->bytes, &pd)) {
        __atomic_fetch_add(&g.st.failed, 1, __ATOMIC_RELAXED);
        pw_log(1, "管线预热：记录长度 %u 与描述版本 %u 不符，整条跳过", (unsigned)rec->bytes, (unsigned)NVK_PW_DESC_BYTES);
        goto out;
    }
    // 记录里记着哪些阶段（st[].sh 的存在位），必须和程序实际的阶段一致：对不上就是旧布局 / 别的层的
    // 记录，建出来的就不是这条配方——宁可不建，也不建一条「差不多」的。
    uint8_t want = rec->desc[0];
    for (int st = 0; st < 3; st++) {
        int have = prog->sh[st] != NULL;
        if (((want >> st) & 1) != have) {
            __atomic_fetch_add(&g.st.failed, 1, __ATOMIC_RELAXED);
            pw_log(1, "管线预热：记录的阶段位 %#x 与程序的阶段对不上（阶段 %d），整条跳过", (unsigned)want, st);
            goto out;
        }
    }
    if ((want & ~0x7u) != 0) {   // GS 以外的阶段（TCS / TES）本层不产生：有就是坏记录
        __atomic_fetch_add(&g.st.failed, 1, __ATOMIC_RELAXED);
        goto out;
    }
    for (int st = 0; st < 3; st++) pd.st[st].sh = prog->sh[st];
    if (nvn_gpu_pipe_warm_scope) nvn_gpu_pipe_warm_scope(1);
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err err = {0};
    nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err);
    if (nvn_gpu_pipe_warm_scope) nvn_gpu_pipe_warm_scope(0);
    if (!nvn_gpu_ok(r)) {
        __atomic_fetch_add(&g.st.failed, 1, __ATOMIC_RELAXED);
        pw_log(1, "管线预热：nvn_gpu_pipe_get 失败（%d）：%s", (int)r.code, err.why);
        goto out;
    }
    __atomic_fetch_add(&g.st.built, 1, __ATOMIC_RELAXED);
    rc = 0;
out:
    rp_shader_prog_return(rec->prog_identity);   // 借用必须在整段使用（含建管线）之后才还
    return rc;
}

// ---- 盘上文件：<缓存目录>/pipes-v<格式>-<描述版本>.bin ----
static void pw_w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t pw_r32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void pw_w64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t pw_r64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

// 目录 / 文件名（幂等）。返回 1 = 有目录可用；0 = 目录还没出来（会话还没开 / 平台没给缓存目录）。
static int pw_ensure_dir_locked(void) {
    if (!g.dir[0]) {
        const char *dir = nvk_session_cache_dir ? nvk_session_cache_dir() : NULL;
        if (!dir || !*dir) return 0;
        snprintf(g.dir, sizeof g.dir, "%s", dir);
    }
    snprintf(g.path, sizeof g.path, "%s/pipes-v%u-%u.bin", g.dir, (unsigned)NVK_PW_FORMAT, (unsigned)NVK_PW_DESC_BYTES);
    return 1;
}

// 原子写：临时文件 + 内容 fsync + rename + 目录 fsync。掉电后不会留半个文件。
static int pw_write_atomic(const char *path, const uint8_t *buf, size_t bytes) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    {   // 目录由自己建：缓存目录原先是「现场编」第一次存结果时顺手建的，程序全从出包预编库取时（现场编 0 次）它不存在，清单就一直写不进去
        char d[600]; snprintf(d, sizeof d, "%s", path);
        char *sl = strrchr(d, '/');
        if (sl && sl != d) { *sl = 0; if (mkdir(d, 0700) != 0 && errno == ENOENT) { char *up = strrchr(d, '/'); if (up && up != d) { *up = 0; (void)mkdir(d, 0700); *up = '/'; } (void)mkdir(d, 0700); } }
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < bytes) {
        ssize_t w = write(fd, buf + off, bytes - off);
        if (w <= 0) { close(fd); unlink(tmp); return -1; }
        off += (size_t)w;
    }
    if (fsync(fd) != 0) { close(fd); unlink(tmp); return -1; }
    if (close(fd) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    {   // 目录项也要落盘：rename 之后文件才算真的在那儿
        char dir[600];
        snprintf(dir, sizeof dir, "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            int dfd = open(dir, O_RDONLY | O_CLOEXEC);
            if (dfd >= 0) { fsync(dfd); close(dfd); }
        }
    }
    return 0;
}

// 整份读（有界）。*enoent = 1 表示目录里还没有旧文件（不是错误）。
static int pw_read_file(const char *path, int installed, uint8_t **out, size_t *bytes, int *enoent) {
    *out = NULL; *bytes = 0; *enoent = 0;
    int fd = installed ? s2a_installed_seed_open(path) : open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { if (errno == ENOENT) { *enoent = 1; return 0; } return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0 || (uint64_t)st.st_size > (uint64_t)NVK_PW_FILE_MAX) { close(fd); return -1; }
    size_t n = (size_t)st.st_size;
    uint8_t *b = malloc(n ? n : 1);
    if (!b) { close(fd); return -1; }
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, b + got, n - got);
        if (r <= 0) { free(b); close(fd); return -1; }
        got += (size_t)r;
    }
    close(fd);
    *out = b; *bytes = n;
    return 0;
}

int nvk_pipe_warm_load(void) {
    pthread_mutex_lock(&g.mu);
    if (g.loaded) { int n = (int)g.count; pthread_mutex_unlock(&g.mu); return n; }
    if (pw_init_locked() != 0) { pthread_mutex_unlock(&g.mu); return -1; }
    if (!pw_ensure_dir_locked()) { pthread_mutex_unlock(&g.mu); return -1; }   // 目录还没出来：预热线程稍后重试
    char path[sizeof g.path];
    snprintf(path, sizeof path, "%s", g.path);
    uint32_t cap = g.cap;
    pthread_mutex_unlock(&g.mu);

    uint8_t *buf = NULL;
    size_t bytes = 0;
    int capture = nvk_warm_capture_enabled();
    int enoent = capture;
    if (!capture && pw_read_file(path, 0, &buf, &bytes, &enoent) != 0) {
        pw_log(1, "管线预热清单读失败：%s", path);
        return -1;   // 读不动：让预热线程稍后重试，不把「读失败」当「清单是空的」
    }
    // 首局（本机没有管线清单）：程序清单用了随包种子时，读同目录的管线种子（nvn_vk_pipe_warm.h「随包种子」）。
    // 管线种子没有自己的有效性头：只认和已收下的程序种子同一目录（目录名 = 闭包 + profile），条目再逐条核程序身份在程序清单里。
    int from_seed = 0;
    char seed_path[sizeof g.path + 64];
    if (!capture && enoent) {
        char sd[sizeof g.path];
        int s = nvk_prog_warm_seed_dir ? nvk_prog_warm_seed_dir(sd, sizeof sd) : 0;
        if (s < 0) return -1;   // 程序清单本局还没定（它先载入）：稍后重试，不把「还不知道」当「没有种子」
        if (s == 1) {
            // 普通翻译和窗口库的管线状态可以不同（如 clip_neg1）。关窗补件带完整替换清单。
            const int alternate = nvk_warm_nowindow_enabled();
            const char *subdir = alternate ? "/nowindow" : "";
            snprintf(seed_path, sizeof seed_path, "%s%s/pipes-v%u-%u.bin", sd, subdir,
                     (unsigned)NVK_PW_FORMAT, (unsigned)NVK_PW_DESC_BYTES);
            int se = 0;
            int rc = pw_read_file(seed_path, 1, &buf, &bytes, &se);
            if (!rc && se && alternate) {
                snprintf(seed_path, sizeof seed_path, "%s/pipes-v%u-%u.bin", sd,
                         (unsigned)NVK_PW_FORMAT, (unsigned)NVK_PW_DESC_BYTES);
                rc = pw_read_file(seed_path, 1, &buf, &bytes, &se);
            }
            if (rc) pw_log(2, "【黄】管线预热清单随包种子读不动：%s，本局不用种子", seed_path);
            else if (!se) { from_seed = 1; enoent = 0; }
        }
    }
    if (enoent) {
        pthread_mutex_lock(&g.mu); g.loaded = 1; pthread_mutex_unlock(&g.mu);
        pw_log(0, "管线预热清单：目录里还没有旧文件（%s），本局只记不预热", path);
        return 0;
    }
    // 头 + 整份校验：magic / 格式版本 / 描述版本 / 条目大小 / 条数 / 文件长度 / FNV，任一不符整份拒绝。
    uint32_t cnt = 0;
    const char *why = NULL;
    if (bytes < NVK_PW_HDR || memcmp(buf, k_magic, 8) != 0) why = "magic 不符";
    else if ((cnt = pw_r32(buf + 20)) > cap) why = "条数超过本局上限";
    else if (pw_r32(buf + 8) != NVK_PW_FORMAT) why = "格式版本不符";
    else if (pw_r32(buf + 12) != NVK_PW_DESC_BYTES) why = "描述版本不符（配方布局变了）";
    else if (pw_r32(buf + 16) != NVK_PW_ENTRY) why = "条目大小不符";
    else if (bytes != NVK_PW_HDR + (size_t)cnt * NVK_PW_ENTRY) why = "文件长度与条数不符";
    else {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = NVK_PW_HDR; i < bytes; i++) h = (h ^ buf[i]) * 1099511628211ull;
        if (h != pw_r64(buf + 24)) why = "整份校验和不符";
    }
    if (why) {
        pthread_mutex_lock(&g.mu);
        g.loaded = 1;
        __atomic_fetch_add(&g.st.rejected, cnt ? cnt : 1, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&g.mu);
        if (from_seed) pw_log(2, "【黄】管线预热清单随包种子不用（%s）：%s；本局按没有清单走，首遇照常建", why, seed_path);   // 锦上添花：不算本机清单坏
        else pw_log(1, "管线预热清单整份拒绝（%s）：%s，本局不预热", why, path);
        free(buf);
        return 0;
    }
    pthread_mutex_lock(&g.mu);
    // 同一进程里第二次 load（测试 / 重新开局）要先清掉上一份的槽位与桶：哈希链是按槽位下标串的，
    // 留着旧链会让新表指到上一份的条目上。
    memset(g.used, 0, g.cap);
    memset(g.fresh, 0, g.cap);   // 载入进来的都是旧条目：一次都还没被本轮碰过
    // 最近 4 条备忘里的槽号也随旧表作废：留着的话，载入前记过的配方在已经空出来的槽上「命中」备忘，
    // 只给空槽加 uses、不再记进清单（载入文件里没有它）。
    for (int i = 0; i < NVK_PW_MEMO; i++) { g.memo[i].slot = -1; g.memo[i].h = 0; }
    for (uint32_t i = 0; i < g.nbucket; i++) g.bucket[i] = -1;
    for (uint32_t i = 0; i < g.cap; i++) g.next[i] = -1;
    if (dg.on > 0 && dg.ev) { memset(dg.ev, 0, ((size_t)1 << PW_DG_EV_BITS) * sizeof *dg.ev); dg.ev_n = 0; }   // 诊断：换了一份清单，旧的淘汰记录作废
    // 随包种子逐条核：程序身份要在程序清单里（= 随包程序种子收下的那批，或本局已记的）。不在的那条预热时永远等不到程序发布，
    // 只会占着重试槽白等 5 秒——宁可少建，不收。本机清单照旧整份收（它的程序本来就是本机记的）。
    uint32_t kept = 0, dropped = 0, file_cnt = cnt;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *e = buf + NVK_PW_HDR + (size_t)i * NVK_PW_ENTRY;
        if (from_seed && !(nvk_prog_warm_contains && nvk_prog_warm_contains(e))) { dropped++; continue; }
        uint32_t k = kept++;
        struct nvk_pipe_rec *r = &g.recs[k];
        memcpy(r->prog_identity, e, 32);
        memcpy(r->desc, e + 32, NVK_PW_DESC_BYTES);
        r->bytes = NVK_PW_DESC_BYTES;
        r->uses = pw_r32(e + 32 + NVK_PW_DESC_BYTES);
        g.uses[k] = r->uses;
        g.seen[k] = cnt - i;   // 文件按热点降序写：前面 = 更近
        g.hash[k] = pw_slot_hash(r->desc, r->prog_identity);
        g.gen[k] = pw_next_gen_locked();   // 槽里换了一份配方：这之前发出去的标签全部作废
        g.used[k] = 1;
        uint32_t b = pw_bucket_of(g.hash[k], g.nbucket);
        g.next[k] = g.bucket[b];
        g.bucket[b] = (int32_t)k;
    }
    if (dropped) __atomic_fetch_add(&g.st.rejected, dropped, __ATOMIC_RELAXED);
    cnt = kept;
    g.count = cnt;
    // 冻结本局预热集：文件顺序（uses 降序）= 热点优先的一份值拷贝，之后的 note / 淘汰 / 重排都影响不到它。
    free(g.plan); g.plan = NULL; g.plan_n = g.plan_i = 0;
    if (cnt) {
        g.plan = (struct nvk_pipe_rec *)malloc((size_t)cnt * sizeof *g.plan);
        if (g.plan) { int cs = nvn_plat_flag("nvk_compute_warm", 0); for (uint32_t i = 0; i < cnt; i++) if (cs || g.recs[i].desc[0] != 0x80) g.plan[g.plan_n++] = g.recs[i]; }
        else pw_log(1, "【红】管线预热清单：本局预热集快照分配失败（本局不预热，清单照常记录）");
    }
    g.clock = file_cnt;   // 最近序号接着文件里最大的那个往后发（种子滤掉几条时 seen 仍按文件位置算）
    g.loaded = 1;
    g.dirty = 0;
    g.note_gen = 1;
    g.order_gen = 0;
    g.st.loaded = cnt;
    pthread_mutex_unlock(&g.mu);
    free(buf);
    if (from_seed) pw_log(0, "管线预热清单（随包种子，本机还没有清单）：%u 条（程序不在程序清单里的 %u 条不收；%s）", (unsigned)cnt, (unsigned)dropped, seed_path);
    else pw_log(0, "管线预热清单：%u 条（%s，%zu 字节）", (unsigned)cnt, path, bytes);
    return (int)cnt;
}

void nvk_pipe_warm_save(void) {
    char path[sizeof g.path];
    uint8_t *buf = NULL;
    size_t bytes = 0;
    uint32_t cnt = 0;
    uint64_t gen = 0;
    // 保存串行（外部复核 P2）：周期保存与停局保存可能来自不同调用点，两份快照共用同一个 .tmp 与 rename
    // 目标时，后发的旧快照会覆写先发的新快照。整段（取快照 → 写临时文件 → rename → 目录 fsync）只允许一个
    // writer；快照代际只用于「不许旧快照覆盖新快照」的兜底断言。
    pthread_mutex_lock(&g_save_mu);
    pthread_mutex_lock(&g.mu);
    if (!g.inited || !g.dirty || !pw_ensure_dir_locked() || g.count == 0) {
        g.save_skip++;   // 内容没变 / 没有目录 / 一条都没有：不写盘
        pthread_mutex_unlock(&g.mu);
        pthread_mutex_unlock(&g_save_mu);
        return;
    }
    gen = ++g.save_gen;
    if (g.order_gen != g.note_gen) pw_build_order_locked();
    cnt = g.count;
    bytes = NVK_PW_HDR + (size_t)cnt * NVK_PW_ENTRY;
    buf = malloc(bytes);
    if (!buf) {
        g.save_fail++;
        g.save_gen--;   // 这份快照不存在了：代际不推进
        pthread_mutex_unlock(&g.mu);
        pthread_mutex_unlock(&g_save_mu);
        return;
    }
    memset(buf, 0, NVK_PW_HDR);
    memcpy(buf, k_magic, 8);
    pw_w32(buf + 8, NVK_PW_FORMAT);
    pw_w32(buf + 12, NVK_PW_DESC_BYTES);
    pw_w32(buf + 16, NVK_PW_ENTRY);
    pw_w32(buf + 20, cnt);
    {   // 落盘顺序 = uses 降序、同 uses 最近优先（与 take 的顺序同一份）
        size_t o = NVK_PW_HDR;
        for (uint32_t k = 0; k < cnt; k++) {
            const struct nvk_pipe_rec *r = &g.recs[g.order[k]];
            memcpy(buf + o, r->prog_identity, 32); o += 32;
            memcpy(buf + o, r->desc, NVK_PW_DESC_BYTES); o += NVK_PW_DESC_BYTES;
            pw_w32(buf + o, g.uses[g.order[k]]); o += 4;
        }
        uint64_t h = 1469598103934665603ull;
        for (size_t i = NVK_PW_HDR; i < bytes; i++) h = (h ^ buf[i]) * 1099511628211ull;
        pw_w64(buf + 24, h);
    }
    memcpy(path, g.path, sizeof path);
    g.dirty = 0;
    pthread_mutex_unlock(&g.mu);

    int rc = pw_write_atomic(path, buf, bytes);
    free(buf);
    pthread_mutex_lock(&g.mu);
    if (rc == 0) {
        if (gen > g.published_gen) g.published_gen = gen;          // 已发布的是最新快照
        else pw_log(1, "【红】管线预热清单：旧快照代际 %llu 覆盖了新快照 %llu", (unsigned long long)gen, (unsigned long long)g.published_gen);
        g.save_ok++;
    } else { g.save_fail++; g.dirty = 1; }   // 写失败：留着脏位，下一次 save 再试（失败只计数）
    pthread_mutex_unlock(&g.mu);
    pthread_mutex_unlock(&g_save_mu);
    if (rc != 0) pw_log(1, "管线预热清单写盘失败（%u 条，%zu 字节）：%s", (unsigned)cnt, bytes, path);
}

void nvk_pipe_warm_stats(struct nvk_pipe_warm_stats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&g.mu);
    *out = g.st;
    out->pending = g.plan_i < g.plan_n ? g.plan_n - g.plan_i : 0;
    out->rejected = g.st.rejected + g.save_fail;
    pthread_mutex_unlock(&g.mu);
    out->draw_hits = nvn_gpu_pipe_warm_hits ? nvn_gpu_pipe_warm_hits() : 0;   // 转发后端：真实 draw 命中预建对象
}

// 诊断读数（用例 / 调试用）：开关关着（或还没有 note 读过开关）时全 0。
void nvk_pipe_warm_diag_stats(struct nvk_pipe_warm_diag *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&g.mu);
    if (dg.on > 0) { *out = dg.c; out->on = 1; }
    pthread_mutex_unlock(&g.mu);
}

// 立刻输出当前窗口一行并开新窗口（用例用；诊断关着时什么也不做）。行在锁外打，与 note 同一做法。
void nvk_pipe_warm_diag_flush(void) {
    struct pw_dg_out o;
    o.line[0] = 0;
    o.sample = 0;
    pthread_mutex_lock(&g.mu);
    if (dg.on > 0) pw_dg_window_locked(&o, pw_dg_now());
    pthread_mutex_unlock(&g.mu);
    pw_dg_out_run(&o);
}

