// 前端调用的其余入口（上沿 nvn_be_*，声明在 nvn_mtl.h / nvn_obj.h）。
// 有真实语义而新层还没接的入口，一律经 nvn_unimpl("be", 名字) 记账后返回「没有 / 没做」，不返回假成功；
// 只是 Metal 侧诊断读数的入口返回 0。接上一个就把对应的 nvn_unimpl 换成实现，并在单测里加用例。
#include "rp_exec.h"
#include "rp_shader.h"
#include "rp_state.h"
#include "rp_res.h"
#include "nvn_deswizzle.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "nvn_backend.h"
#include "nvn_mtl.h"
#include "nvn_hmap.h"

void rp_res_tex_gone(uint64_t serial);   // rp_res.c
void rp_res_reset(void);
void rp_buf_pool_gone(const void *mem);   // rp_buf.c
void rp_buf_reset(void);
void rp_shader_reset(void);            // 着色器来源（核心外）
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>   // mincore：池尾之后那段在不在映射里（nvn_be_pool_cover）
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define UN(name) nvn_unimpl("be", name, __builtin_return_address(0))

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

void mtl_init(void) {}

// ---------- 内存池登记：按地址判断「这段在不在某个池里」----------
enum { RP_NPOOL = 4096 };
static struct { const uint8_t *mem; uint64_t size; } g_pool[RP_NPOOL];
static uint64_t g_pool_maxend[RP_NPOOL];   // prefix max of pool end addresses (ranges may nest; prunes the walk-back)
static int g_npool;
// 池表按起点升序（插入时挪位）。查找走二分 + 「上次命中的池」快路：
// 原来三条查找都是「抢全局锁 + 线性扫 4096 项」，而每笔绘制的顶点流解析要调两次；真机（重叠档）读到
// 「找池取镜像」每帧 20.6–23.0 毫秒 / 5619 次调用（≈4 微秒一次），就是这把锁与这张线性表。
// 快路只在「池表代际没变」时生效：加池 / 删池都会推进代际，之后重新走锁内二分，不会读到销毁的池。
static volatile uint64_t g_pool_epoch = 1;
// 上次命中的池（无锁读）：用奇偶序号做 seqlock，读侧看到序号变化就放弃快路——
// 只靠「代际 + 两个字段」会被撕裂读（新起点配旧长度）误判成命中，进而把地址解析到错的池上。
static volatile uint64_t g_pool_lh_seq;
// [lo, lim): the interval this entry truly answers for. lim = the next pool start (UINT64_MAX for the last pool);
// lo = the greatest end among the pools the binary search passed over after the hit (none of them can cover an
// address at or past its end). Without them the fast path would answer addresses past a nested pool with the
// outer pool, or fall back to the locked search on every nested lookup.
static struct { uint64_t a, n, lo, lim, epoch; } g_pool_lh;
// 线程内备忘（#974 问 3，维护者答「做」）：上面「上次命中的池」全局只有一格，而一笔绘制要在顶点池 / 常量缓冲池 / 索引池之间来回，
// 那一格几乎总被别的池占着，多数查找仍然抢全局锁 + 二分（真机取样：rp_pool_find 占回放线程 2.6%，每帧约 4500 段）。
// 每个线程多记 16 个池，命中时不抢锁、不碰任何共享的缓存行。
// 键 = 地址区间 [a, lim)：池表按起点升序，二分的候选是「起点不超过地址的最后一个池」，区间内任何地址的候选都是这一个池；
// 命中后「装不装得下」用与锁内二分逐字相同的算术判，装不下就不算命中、照旧走锁内——答案与锁内二分逐位一致，不改任何返回值。
// 代际：与上面同一个 g_pool_epoch（加池 / 删池都会推进）；记录发生在锁内，读到的代际与表同时成立；代际一变整张作废。
typedef struct { uint64_t a, n, lo, lim, epoch; } pool_memo;   // 区间 [a, lim) 内的地址，二分候选都是起点 a、长 n 的这个池
enum { POOL_MEMO_N = 16 };   // 2 的幂
// 一个线程一份、整块放进同一个 _Thread_local 对象：每个 _Thread_local 变量在动态装载的 .so 里（安卓 dlopen 进来的游戏库）每次取址都要
// 调一次 TLS 描述符的解析函数（linker64 里，约几十条指令、还要查 DTV 代际）。原先 memo 数组 / put / mru / hits / lookups / tick 是六个独立变量，
// rp_pool_find 每次调用要解析五六次（真机取样 linker64 占回放线程 1.4%，样本全落在 rp_entry.c 取这几个变量的行上）；合成一个对象后每次调用只解析一次。
typedef struct {
    pool_memo memo[POOL_MEMO_N];
    unsigned put, mru, tick;
    unsigned tick_fast;
    uint64_t hits, lookups;
    uint64_t fast_hits;
} pool_tls;
static _Thread_local pool_tls g_pool_tls;
static int g_pool_fast_off = -1;   // nvn_plat_flag("nvk_pool_nofast", 0)：缺省开快路；每 4096 次查找重读一次（release 包里直接是缺省）
static int g_pool_memo_on = -1;   // nvn_plat_flag("nvk_pool_memo", 1)：缺省开；每 4096 次查找重读一次（release 包里直接是缺省）
void rp_pool_memo_stats(uint64_t *hits, uint64_t *lookups) { const pool_tls *t = &g_pool_tls; *hits = t->hits; *lookups = t->lookups; }   // 调用线程自己的累计
static int pool_find_memo(pool_tls *const t, uint64_t addr, uint64_t len, const uint8_t **mem, uint64_t *size) {
    int on = __atomic_load_n(&g_pool_memo_on, __ATOMIC_RELAXED);
    if (on < 0 || !(++t->tick & 4095)) { on = nvn_plat_flag("nvk_pool_memo", 1) != 0; __atomic_store_n(&g_pool_memo_on, on, __ATOMIC_RELAXED); }
    if (!on) return 0;
    t->lookups++;
    const uint64_t ep = __atomic_load_n(&g_pool_epoch, __ATOMIC_ACQUIRE);
    for (unsigned j = 0; j < POOL_MEMO_N; j++) {
        const unsigned i = (t->mru + j) & (POOL_MEMO_N - 1);   // 先看上次命中的那一项：相邻两次查找多半在同一个池
        const pool_memo *m = &t->memo[i];
        if (m->epoch != ep || addr < m->lo || addr >= m->lim) continue;
        if (addr - m->a > m->n || len > m->n - (addr - m->a)) continue;   // 区间内但装不下：池可能嵌套，看后面的记录，都不行再落回锁内
        t->mru = i; t->hits++;
        if (mem) *mem = (const uint8_t *)(uintptr_t)m->a;
        if (size) *size = m->n;
        return 1;
    }
    return 0;
}
static void pool_maxend_recompute(void) {   // caller holds g_mu; O(n) on add/del (pool count is in the hundreds, adds are rare)
    uint64_t m = 0;
    for (int i = 0; i < g_npool; i++) { uint64_t e = (uint64_t)(uintptr_t)g_pool[i].mem + g_pool[i].size; if (e > m) m = e; g_pool_maxend[i] = m; }
}
static int pool_find_locked(uint64_t addr, uint64_t len, const uint8_t **mem, uint64_t *size) {
    int lo = 0, hi = g_npool - 1, cand = -1;
    while (lo <= hi) { int mid = (lo + hi) / 2; if ((uint64_t)(uintptr_t)g_pool[mid].mem <= addr) { cand = mid; lo = mid + 1; } else hi = mid - 1; }
    if (cand < 0) return 0;
    // Ranges may nest: a pool's storage can lie inside another pool's range. Taking only the last start <= addr
    // would let the inner (smaller) pool shadow the outer one. Walk back to the greatest start that truly covers
    // [addr, addr+len); g_pool_maxend prunes entries that cannot reach addr at all.
    int hit = -1;
    for (int j = cand; j >= 0; j--) {
        uint64_t jb = (uint64_t)(uintptr_t)g_pool[j].mem, jsz = g_pool[j].size;
        if (addr - jb <= jsz && len <= jsz - (addr - jb)) { hit = j; break; }
        if (j == 0 || g_pool_maxend[j - 1] <= addr) break;
    }
    if (hit < 0) return 0;
    const int ci = hit;
    uint64_t b = (uint64_t)(uintptr_t)g_pool[ci].mem, sz = g_pool[ci].size;
    // Answer interval [lo, lim): lim is where the binary search stops pointing at ci; lo is the greatest end
    // among the pools ci+1..cand the search passed over (none of them can cover an address at/after it).
    uint64_t alo = b;
    for (int k = ci + 1; k <= cand; k++) {
        uint64_t ke = (uint64_t)(uintptr_t)g_pool[k].mem + g_pool[k].size;
        if (ke > alo) alo = ke;
    }
    uint64_t lim = cand + 1 < g_npool ? (uint64_t)(uintptr_t)g_pool[cand + 1].mem : UINT64_MAX;
    if (mem) *mem = g_pool[ci].mem;
    if (size) *size = sz;
    const uint64_t ep = __atomic_load_n(&g_pool_epoch, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_pool_lh_seq, 1, __ATOMIC_RELEASE);   // 奇数：正在写
    g_pool_lh.a = b; g_pool_lh.n = sz; g_pool_lh.lo = alo; g_pool_lh.lim = lim; g_pool_lh.epoch = ep;
    __atomic_add_fetch(&g_pool_lh_seq, 1, __ATOMIC_RELEASE);   // 偶数：写完
    if (__atomic_load_n(&g_pool_memo_on, __ATOMIC_RELAXED) > 0) {   // 备忘开着才记（关着时行为与改动前完全一致）
        pool_tls *const t = &g_pool_tls;
        t->memo[t->put++ & (POOL_MEMO_N - 1)] = (pool_memo){b, sz, alo, lim, ep};
    }
    return 1;
}
static int pool_find_fast(pool_tls *const t, uint64_t addr, uint64_t len, const uint8_t **mem, uint64_t *size) {   // 快路：不抢锁；代际一致才认
    int off = __atomic_load_n(&g_pool_fast_off, __ATOMIC_RELAXED);
    if (off < 0 || !(++t->tick_fast & 4095)) { off = nvn_plat_flag("nvk_pool_nofast", 0) != 0; __atomic_store_n(&g_pool_fast_off, off, __ATOMIC_RELAXED); }
    if (off) return 0;   // debug.s2a.nvk_pool_nofast=1 关快路（缺省开；玩家包恒缺省）
    const uint64_t ep = __atomic_load_n(&g_pool_epoch, __ATOMIC_ACQUIRE);
    const uint64_t s1 = __atomic_load_n(&g_pool_lh_seq, __ATOMIC_ACQUIRE);
    if (s1 & 1) return 0;                                   // 正在写：这一笔走锁内二分
    uint64_t a = g_pool_lh.a, n = g_pool_lh.n, lo = g_pool_lh.lo, lim = g_pool_lh.lim, cep = g_pool_lh.epoch;
    const uint64_t s2 = __atomic_load_n(&g_pool_lh_seq, __ATOMIC_ACQUIRE);
    if (s1 != s2 || cep != ep) return 0;
    if (!a) return 0;
    if (addr < lo || addr >= lim) return 0;   // outside the answer interval: the locked search decides
    if (addr - a > n || len > n - (addr - a)) return 0;
    if (mem) *mem = (const uint8_t *)(uintptr_t)a;
    if (size) *size = n;
    t->fast_hits++;
    return 1;
}
void nvn_be_pool_add(const void *mem, uint64_t size) {
    if (!mem || !size) return;
    pthread_mutex_lock(&g_mu);
    if (g_npool < RP_NPOOL) {
        uint64_t b = (uint64_t)(uintptr_t)mem;
        int at = g_npool;
        for (int i = 0; i < g_npool; i++) if ((uint64_t)(uintptr_t)g_pool[i].mem > b) { at = i; break; }
        if (at != g_npool) memmove(&g_pool[at + 1], &g_pool[at], (size_t)(g_npool - at) * sizeof g_pool[0]);
        g_pool[at] = (__typeof__(g_pool[0])){mem, size};
        g_npool++;
        pool_maxend_recompute();
        __atomic_add_fetch(&g_pool_epoch, 1, __ATOMIC_RELEASE);
    }
    else { pthread_mutex_unlock(&g_mu); nvn_unimpl("limit", "pool_table_full_4096", mem); return; }
    pthread_mutex_unlock(&g_mu);
}
// 池上伸出池尾的缓冲对象（D-6VG6，同 nvn_mtl.m nvn_be_pool_cover）：把这个池在本后端的登记扩到 [mem, mem + need)，越过池尾的访问落在池存储之后同一处内存（真机语义）。
//   本表注销只删第一个同起点的条目，所以原地加长那一条，不另登记。池尾之后那段须整段映射着（mincore 每页都认），否则不扩、记一笔（读写会 SIGSEGV）
void nvn_be_pool_cover(const void *mem, uint64_t size, uint64_t need) {
    if (!mem || need <= size) return;
    uintptr_t pg = (uintptr_t)getpagesize(), a = ((uintptr_t)mem + size) & ~(pg - 1), e = ((uintptr_t)mem + need + pg - 1) & ~(pg - 1);
    for (uintptr_t p = a; p < e; p += pg) { unsigned char v[1]; if (mincore((void *)p, pg, (void *)v)) { nvn_unimpl("limit", "pool_cover_tail_unmapped", mem); return; } }
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_npool; i++) if (g_pool[i].mem == mem) {
        if (g_pool[i].size < need) { g_pool[i].size = need; pool_maxend_recompute(); __atomic_add_fetch(&g_pool_epoch, 1, __ATOMIC_RELEASE); }
        break; }
    pthread_mutex_unlock(&g_mu);
}
void nvn_be_pool_del(const void *mem) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_npool; i++) if (g_pool[i].mem == mem) {
        memmove(&g_pool[i], &g_pool[i + 1], (size_t)(g_npool - i - 1) * sizeof g_pool[0]);
        g_npool--;
        pool_maxend_recompute();
        __atomic_add_fetch(&g_pool_epoch, 1, __ATOMIC_RELEASE);
        break; }
    pthread_mutex_unlock(&g_mu);
    rp_buf_pool_gone(mem);
}
int nvn_be_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    const uint8_t *mem = NULL; uint64_t size = 0;
    pool_tls *const t = &g_pool_tls;
    int hit = pool_find_memo(t, addr, len, &mem, &size) || pool_find_fast(t, addr, len, &mem, &size);
    if (!hit) {
        pthread_mutex_lock(&g_mu);
        hit = pool_find_locked(addr, len, &mem, &size);
        pthread_mutex_unlock(&g_mu);
    }
    if (hit) { if (buf) *buf = (void *)(uintptr_t)mem; if (off) *off = addr - (uint64_t)(uintptr_t)mem; }
    return hit;
}
int rp_pool_find(uint64_t addr, uint64_t len, const uint8_t **mem, uint64_t *size) {
    pool_tls *const t = &g_pool_tls;
    if (pool_find_memo(t, addr, len, mem, size) || pool_find_fast(t, addr, len, mem, size)) return 1;
    pthread_mutex_lock(&g_mu);
    int hit = pool_find_locked(addr, len, mem, size);
    pthread_mutex_unlock(&g_mu);
    return hit;
}
// NULL 取池表代际；非空要求整份镜像的后备恰由一个池登记覆盖，含嵌套、同起点池在内的别名都不作证明。
uint64_t rp_pool_mirror_epoch(const uint8_t *mem, uint64_t size) {
    if (!mem) return __atomic_load_n(&g_pool_epoch, __ATOMIC_ACQUIRE);
    const uint64_t base = (uintptr_t)mem;
    if (size > UINT64_MAX - base || pthread_mutex_trylock(&g_mu)) return 0;
    int lo = 0, hi = g_npool;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if ((uintptr_t)g_pool[mid].mem < base) lo = mid + 1;
        else hi = mid;
    }
    const int unique = lo < g_npool && g_pool[lo].mem == mem && g_pool[lo].size == size &&
                       (lo == 0 || g_pool_maxend[lo - 1] <= base) &&
                       (lo + 1 == g_npool || (uintptr_t)g_pool[lo + 1].mem >= base + size);
    const uint64_t epoch = unique ? __atomic_load_n(&g_pool_epoch, __ATOMIC_RELAXED) : 0;
    pthread_mutex_unlock(&g_mu);
    return epoch;
}
int rp_pool_count(void) { pthread_mutex_lock(&g_mu); int n = g_npool; pthread_mutex_unlock(&g_mu); return n; }
uint64_t rp_pool_fast_hits(void) { return g_pool_tls.fast_hits; }
void nvn_be_pool_addressed(const void *mem, uint64_t size) { (void)mem; (void)size; }   // 原始全局内存表：翻译器的裸地址访问接上时用

// ---------- 贴图：初始化只登记序号，建图在首次使用（nvn_mtl.m:2597 / :4113）----------
static nvn_hmap g_tex_live; static int g_tex_live_inited;
static _Atomic uint64_t g_tex_init_n, g_tex_rel_n;
void nvn_be_tex_init(const tex_t *t) {
    if (!t->serial) return;
    atomic_fetch_add(&g_tex_init_n, 1);
    pthread_mutex_lock(&g_mu);
    if (!g_tex_live_inited) { nvn_hm_init(&g_tex_live, 8); g_tex_live_inited = 1; }
    nvn_hm_put(&g_tex_live, &t->serial, (void *)1);
    pthread_mutex_unlock(&g_mu);
}
void nvn_be_tex_release(tex_t *t) {
    if (!t || !t->serial) return;
    atomic_fetch_add(&g_tex_rel_n, 1);
    pthread_mutex_lock(&g_mu);
    if (g_tex_live_inited) nvn_hm_del(&g_tex_live, &t->serial);
    pthread_mutex_unlock(&g_mu);
    rp_res_tex_gone(t->serial);
}
int rp_tex_live_n(void) { pthread_mutex_lock(&g_mu); int n = g_tex_live_inited ? (int)g_tex_live.n : 0; pthread_mutex_unlock(&g_mu); return n; }
void nvn_be_tex_counters(uint64_t *init, uint64_t *rel) { if (init) *init = atomic_load(&g_tex_init_n); if (rel) *rel = atomic_load(&g_tex_rel_n); }
void nvn_be_tex_tick(uint64_t frame) { (void)frame; }
int nvn_be_tex_from_gl(void *t, uint32_t gl_name) { (void)t; (void)gl_name; UN("tex_from_gl"); return 0; }
void nvn_be_xv_window_tex(uint64_t serial) { (void)serial; }   // 跨提交有效性账本随通道规划接

// ---------- 同步读贴图（ReadTexels） ----------
// 对齐 iOS（nvn_mtl.m 的 nvn_be_read_texels）：
// ① 游戏内存里就是内容（随包 / WriteTexels 或刷新写过 / 脏页追踪发现被 CPU 写过）：就地反交织，按调用方给的区域 / 行距 / 片距逐行拷进 dst；
//    不碰 GPU、不建宿主贴图，任何线程都能调。
// ② 内容归 GPU（渲染 / 拷贝 / 计算写过）：rt_gpu 从宿主贴图读回（片 1：二维 / 二维数组 / 立方体 / 三维，NVN_GPU_FMT_READBACK 的格式）。
// 线性渲染目标复用这两条路径：客体按线性行距解包，宿主按实际渲染存储读回。
int nvn_ft_written(const tex_t *t) __attribute__((weak));   // hle_nvn.c：脏页追踪（档案开关在它里面判），命中时它自己记 nvn_be_tex_cpu_written；没有它的构建当「没写过」
static int tex_content_in_guest_memory(const tex_t *t) {
    if (!t->mem) return 0;
    // 已被 GPU 写过：游戏内存里的字节比 GPU 写的旧，读出来是错的，留给读回路径（iOS 没有这条）。排在 nvn_ft_written 之前：它命中会把归属翻回 CPU
    if (nvn_be_tex_gpu_owned(t->serial)) return 0;
    return t->has_layout || rp_tex_cpu_written(t->serial) || (nvn_ft_written && nvn_ft_written(t));
}
// 弱链接[replay]：nvn_hle_submit_lock
// 弱链接[replay]：nvn_hle_submit_unlock
#include "rp_read_texels.inc"   // ② 内容归 GPU 的读回（rt_gpu）
void nvn_be_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    if (!t || !r || !dst) { nvn_unimpl("be", "read_texels_bad_args", __builtin_return_address(0)); return; }
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) { nvn_unimpl("fmt", "read_texels_unknown_format", t); return; }
    if ((t->d.flags & NVN_TEXTURE_FLAGS_SPARSE) || t->d.samples > 1 || t->d.target == NVN_TEXTURE_TARGET_BUFFER) { UN("read_texels"); return; }
    if (t->d.flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET) {
        if (!rp_linear_rt_layout(t)) {
            nvn_unimpl("tex", "linear_render_target_shape", t); return;
        }
    } else if (t->d.flags & NVN_TEXTURE_FLAGS_LINEAR) { UN("read_texels"); return; }
    // 区域按块折算（x / y 按块宽高除，宽高向上取整到块）。级、层、片、区域、行距、片距任一不成立都拒绝：不读存储之外的字节，
    // 也不写互相覆盖的输出（iOS 不查）。这些检查都是纯的，排在可能改归属的 nvn_ft_written 之前
    uint32_t levels = t->d.levels ? t->d.levels : 1, layers = t->L.layers ? t->L.layers : 1;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    int one_d = t->d.target == NVN_TEXTURE_TARGET_1D || t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY;   // 一维只有一行；一维数组的 height 是层数（nvn_tex.c shape）
    if (level >= levels || level >= 32 || r->xoffset < 0 || r->yoffset < 0 || r->zoffset < 0 || r->width <= 0 || r->height <= 0 || r->depth < 0) { nvn_unimpl("be", "read_texels_bad_region", t); return; }
    uint32_t lw = t->d.width >> level, lh = one_d ? 1 : t->d.height >> level, ld = is3d ? t->d.depth >> level : 1;
    if (!lw) lw = 1;
    if (!lh) lh = 1;
    if (!ld) ld = 1;
    // 下面的几何换算都用 64 位无符号算：宽高是调用方给的 int / uint32，加块宽减一不能回绕
    uint64_t lp = ((uint64_t)lw + (uint64_t)bw - 1) / (uint64_t)bw * (uint64_t)bpb, lr = ((uint64_t)lh + (uint64_t)bh - 1) / (uint64_t)bh;   // 这一级一行块的字节数 / 块行数
    uint64_t x0 = (uint64_t)(r->xoffset / bw) * (uint64_t)bpb, y0 = (uint64_t)(r->yoffset / bh);
    uint64_t wb = ((uint64_t)r->width + (uint64_t)bw - 1) / (uint64_t)bw * (uint64_t)bpb, hb = ((uint64_t)r->height + (uint64_t)bh - 1) / (uint64_t)bh;
    uint64_t nz = r->depth ? (uint64_t)r->depth : 1, zoff = (uint64_t)r->zoffset;
    if (x0 + wb > lp || y0 + hb > lr || (is3d ? zoff + nz > ld : (uint64_t)layer0 + zoff + nz > layers)) { nvn_unimpl("be", "read_texels_bad_region", t); return; }
    size_t ext = 0, ext_all = 0;   // 一片 / 全部输出占的字节范围：行距 / 片距得容得下一行 / 一片，且这个范围本身不回绕
    if (row < wb || __builtin_mul_overflow(hb - 1, row, &ext) || __builtin_add_overflow(ext, wb, &ext) ||
        (nz > 1 && (img < ext || __builtin_mul_overflow((size_t)(nz - 1), img, &ext_all) || __builtin_add_overflow(ext_all, ext, &ext_all)))) { nvn_unimpl("be", "read_texels_bad_stride", t); return; }
    if (!tex_content_in_guest_memory(t)) {
        if (nvn_be_tex_gpu_owned(t->serial)) rt_gpu(t, level, is3d ? (uint32_t)zoff : (uint32_t)(layer0 + zoff), (uint32_t)nz, r, wb, hb, bw, bh, lw, lh, dst, row, img);
        else UN("read_texels");   // 游戏内存里没有内容、GPU 也没写过：内容未定义，不猜
        return;
    }
    uint64_t need = 0;   // 一级（三维：整个体积）线性化后的字节数
    if (__builtin_mul_overflow((uint64_t)lp, (uint64_t)lr, &need) || (is3d && __builtin_mul_overflow(need, (uint64_t)ld, &need)) || need > SIZE_MAX) need = 0;
    uint8_t *lin = need ? malloc((size_t)need) : NULL;
    if (!lin) { nvn_unimpl("limit", "read_texels_alloc", t); return; }
    uint8_t *part = NULL; const uint8_t *store = rp_tex_src(t, &part);   // 存储伸出内存池时池外按 0，不越界读（rp_res.c）
    if (!store) { nvn_unimpl("limit", "read_texels_alloc", t); free(lin); return; }
    for (uint64_t z = 0; z < nz; z++) {
        if (!is3d || z == 0) {   // 二维 / 数组：一层一解；三维：一次解整级体积，各片都从这块里取
            if (nvn_deswizzle(&t->d, level, is3d ? 0 : (unsigned)(layer0 + zoff + z), store, lin, (size_t)lp)) { nvn_unimpl("tex", "deswizzle_failed", t); free(lin); free(part); return; }
        }
        const uint8_t *src = lin + (is3d ? (size_t)((zoff + z) * lp * lr) : 0);
        for (uint64_t y = 0; y < hb; y++) memcpy(dst + (size_t)z * img + (size_t)y * row, src + (y0 + y) * lp + x0, (size_t)wb);
    }
    free(lin); free(part);
}

// ---------- 程序（描述符池登记、贴图内容归属在 rp_res.c）----------
void nvn_be_prog_release(prog_t *p) { (void)p; }               // 还没有按程序的管线缓存：没有东西要释放
void nvn_be_shader_prefetch(const uint8_t key[8]) { (void)key; }   // 预取是提速手段，不影响正确性；着色器链接上后再接
void nvn_be_pair_prefetch(const uint8_t (*keys)[8], int n) { (void)keys; (void)n; }   // 同上：提速手段
// 1 = 超过 1,024 条的间接计数多绘整段录一条 OP_MULTI_DRAW（rp_multi_draw 一次交后端）；设备没开 drawIndirectCount 时仍逐条录（rp_draw 报名跳过）
int nvn_be_multi_draw_range(void) { nvn_gpu_caps caps; nvn_gpu_caps_get(&caps); return (caps.caps & NVN_GPU_CAP_INDIRECT_COUNT) != 0; }
int nvn_be_buf_extent_consumer(void) { return 0; }             // 新回放按绑定范围和池镜像解析，未查询前端的活缓冲表

// ---------- 全局状态 ----------
static int g_depth_zero, g_origin_ll = 1; static uint64_t g_frame;
void nvn_be_set_depth_zero(int on) { g_depth_zero = on; }
void nvn_be_set_origin_lower_left(int on) { g_origin_ll = on; }
// 上屏翻转按被上屏窗口建时的原点（hle_nvn.c win_t.present_ll，同 nvn_mtl.m g_present_ll）；-1 = 没有窗口说过，跟当前原点
static int g_present_ll = -1;
void nvn_be_set_present_lower_left(int on) { g_present_ll = on; }
int rp_present_lower_left(void) { return g_present_ll >= 0 ? g_present_ll : g_origin_ll; }
void nvn_be_set_frame(uint64_t f) { g_frame = f; }
int rp_depth_zero(void) { return g_depth_zero; }
int rp_origin_lower_left(void) { return g_origin_ll; }
uint64_t rp_frame(void) { return g_frame; }

// ---------- 事件 / 跨队列：GPU 侧等事件未接（OP_SIGNAL_EVENT+WAIT），所以没有停住的队列 ----------
void nvn_be_event_written(volatile uint32_t *val) { (void)val; }
void nvn_be_event_gpu_recorded(volatile uint32_t *val) { (void)val; }
void nvn_be_xq_flush(void *q, const char *why) { (void)q; (void)why; }
int nvn_be_xq_fence(void *q, void *s) { (void)q; (void)s; return 0; }
int nvn_be_xq_owed(const void *s, void **q) { (void)s; (void)q; return 0; }
int nvn_be_xq_owed_wait(const void *s, uint64_t ns) { (void)s; (void)ns; return 1; }
void nvn_be_xq_wait_queue(void *q, void *owner) { (void)q; (void)owner; }

// ---------- 等完成 ----------
void nvn_be_finish(void) { uint64_t f = nvn_be_last_fence(); if (f) nvn_be_wait(f, 5000000000ull); }
void nvn_be_drain(void) { nvn_be_finish(); }

// ---------- 显示节拍：未接（上屏本身在 rp_present.c）----------
int nvn_be_link_active(void) { return 0; }
int nvn_be_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
void nvn_be_ub_present(double iv_ms) { (void)iv_ms; }

// ---------- 会话 ----------
void nvn_be_begin_session(void) {}
int nvn_be_request_stop(void) { return 0; }
int nvn_be_drain_session(void) { nvn_be_finish(); return 0; }
void rp_exec_reset(void);
int nvn_be_shutdown_session(void) { rp_pool_stats_end(); rp_exec_reset(); rp_draw_reset(); rp_shader_reset(); rp_pass_reset(); rp_res_reset(); rp_buf_reset(); rp_state_reset(); return 0; }   // 上屏面由平台件摘（rp_present_set_surface）

// 长帧行在呈现线程作差，两个来源都按进程累计，读取不清零。
void nvn_be_create_stats(uint64_t *lib, uint64_t *pso) {
    if (lib) *lib = rp_shader_compiled_programs();
    if (pso) *pso = rp_sync_pipelines_created();
}

// ---------- Metal 侧诊断读数：新层没有对应账本，报 0 ----------
double nvn_be_gpu_ms(uint64_t f) { (void)f; return 0; }
int nvn_be_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; if (t0) *t0 = 0; if (t1) *t1 = 0; return 0; }
void nvn_be_stats(uint64_t *passes, uint64_t *draws) { if (passes) *passes = 0; if (draws) *draws = 0; }
void nvn_be_args_stats(uint64_t *enc, uint64_t *reuse) { if (enc) *enc = 0; if (reuse) *reuse = 0; }
void nvn_be_use_stats(uint64_t *call, uint64_t *skip) { if (call) *call = 0; if (skip) *skip = 0; }
uint64_t nvn_be_ds_omit(void) { return 0; }
uint64_t nvn_be_drawable_ns_take(void) { return 0; }
void nvn_be_bw(uint64_t *load, uint64_t *store, uint64_t *full) { if (load) *load = 0; if (store) *store = 0; if (full) *full = 0; }
uint64_t nvn_be_udc_saved_bytes(void) { return 0; }
uint64_t nvn_be_upload_ns_take(void) { return 0; }
uint64_t nvn_be_upload_gpu_ns_take(void) { return 0; }
void nvn_be_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip) { if (resolve) *resolve = 0; if (reuse) *reuse = 0; if (set) *set = 0; if (skip) *skip = 0; }
void mtl_res_counts(uint32_t tick, const char *why) { (void)tick; (void)why; }
void mtl_res_snapshot(mtl_res_count *out) { if (out) memset(out, 0, sizeof *out); }
void bf_want_note(const char *key) { (void)key; }
uint64_t nvn_be_upload_bytes(void) { return 0; }
void nvn_be_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { if (lib_ns) *lib_ns = 0; if (pso_ns) *pso_ns = 0; }
// 前端每 300 帧交来一次常驻内存读数（hle_nvn.c dump_frame；宿主构建里编进这一段的才会调到）：打一行，别的不做
void nvn_be_mem_stat(unsigned long long frame, unsigned long long rss_mb) {
    char b[96]; snprintf(b, sizeof b, "第 %llu 帧：常驻内存 %llu MB", frame, rss_mb); nvn_plat_log(NVN_PLAT_LOG_INFO, "mem", b);
}

// ---------- 帧率目标、帧录放 ----------
int dm_fps_setting(void) __attribute__((weak));   // 宿主：玩家的帧率设置；宿主没给这个入口时按 60
int nvn_be_fps_target(void) { return dm_fps_setting && dm_fps_setting() == 30 ? 30 : 60; }
// 帧录放（把一帧的命令流存盘再回放）只在 Metal 后端有（nvn_frame_rec.inc）：这里恒关
int nvn_frec_on(void) { return 0; }
void nvn_frec_submit(const uint64_t *h, int n) { (void)h; (void)n; }
void nvn_frec_present(const tex_t *t, double period, const int crop[4]) { (void)t; (void)period; (void)crop; }
