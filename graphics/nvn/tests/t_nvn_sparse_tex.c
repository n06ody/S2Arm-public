// 回放核心：虚拟池（稀疏池）里压缩贴图的内容来源（rp_res.c sparse_*）。与 t_nvn_replay.c 共用同一份替身（重命名它的 main 后 include）。
// 要证明什么：
//   ① 内容：虚拟池里、标志不是渲染目标 / 图像 / 线性 / 压缩提示的压缩贴图，上传的是游戏映射在那里的物理页内容（按页表读，没映射的页按 0），
//      而不是「没有内容来源」。期望值走独立路径：同样的字节放进普通池里的同形贴图，走原有的 CPU 贴图上传，两边上传的每一级每一层字节逐字相同。
//   ② 判脏：背后的物理页被写 / 页表映射变了（含「只映射、没写任何页」，写代号不动）→ 重传恰好一次；无关的写 / 无关的映射 → 不重传；
//      稳态检查走「映射日志 + 物理段写代号」，不逐页走页表（walks 只在重建缓存时有）。
//   ③ 范围：不碰未压缩 / 带 COMPRESSIBLE、IMAGE、DISPLAY 标志 / 普通池里的贴图；WriteTexels 写过的贴图照旧直接读；超过副本上限的报红、不传。
//   ④ 随包布局：真机的流式角色贴图是「标志只有 SPARSE、带随包布局（字节 03 04）、池标志 0x2a1、压缩格式」。这一类过去先被「随包布局 = CPU 内容」
//      接走，到上传又被「SPARSE 标志不是线性族」拒掉，一次都没传过（后端存储全零：ASTC 解出洋红、BC 解出黑）。现在先看虚拟池再看随包布局：
//      走页表读，逐级字节与「期望存储直接交公共反交织」相同（SPARSE 标志的贴图在普通池里没有参照路径，期望走这条独立路径），背后的页变了重传。
// 虚拟池的页表、映射日志、物理页写代号都是本文件里的替身（接口与 hle_nvn.c 的 nvn_sparse_* 同形）。贴图的 mem 指在预留范围之外 32 GiB 处
//   （同真机的稀疏贴图），任何直接读 t->mem 的实现在这里都会崩或读到别的内容。
// 负对照：环境变量 T_NVN_SPARSE_OFF=1 把开关 nvk_sparse_tex 关掉（走改动前的行为）；设了再跑，内容 / 判脏各例必须变红。
#define main replay_fixture_main
#define nvn_plat_flag fixture_plat_flag
#define nvn_dirty_since fixture_dirty_since
#define nvn_gpu_tex_upload fixture_gpu_tex_upload
#include "t_nvn_replay.c"
#undef nvn_gpu_tex_upload
#undef nvn_dirty_since
#undef nvn_plat_flag
#undef main
#include <stdio.h>
#include <sys/mman.h>

static int sp_off(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("T_NVN_SPARSE_OFF"); v = e && *e && *e != '0'; }
    return v;
}
int nvn_plat_flag(const char *name, int dflt) {
    if (sp_off() && !strcmp(name, "nvk_sparse_tex")) return 0;
    return fixture_plat_flag(name, dflt);
}

// ---------- 物理池 / 页表 / 映射日志 替身 ----------
enum { VP = 4096, VPAGES = 2048, PHYS_PAGES = 2048, VLOG = 16, SP_CHUNK = 1 << 20 };
static const uint64_t VOFF0 = 32ull << 30;   // 测试贴图在虚拟池里的窗口起点：池基址之后 32 GiB（真机稀疏贴图的位置，在预留范围之外）
static _Alignas(4096) uint8_t g_phys[(size_t)PHYS_PAGES * VP];   // 游戏用 CPU 写的内容
static uint64_t g_phys_gen[PHYS_PAGES];                           // 物理页写代号（替身写跟踪）
static uint8_t g_phys_armed[PHYS_PAGES];                          // 被 nvn_sparse_protect 加过保护的物理页
static struct { int ph; uint64_t gen; } g_vpt[VPAGES];            // 虚拟页 → 物理页号（-1 = 没映射）+ 映射它那次请求的代号
static uint64_t g_vgen, g_nvlog;
static struct { uint64_t off, len; } g_vlog[VLOG];                // 映射日志：偏移相对池基址
static pool_t g_vpool, g_vpool2;                                  // g_vpool：页表在上面的虚拟池；g_vpool2：虚拟池标志、但存储是真内存（WriteTexels 用例）
static uint8_t *g_vbase;
static struct { int copy_calls, prot_calls, walks, runs_calls, hit_calls, since_calls; uint64_t copy_bytes, prot_bytes; } g_spc;

static uint64_t vp_base(void) { return (uint64_t)(uintptr_t)g_vpool.mem + VOFF0; }
static int vp_page(const void *pool, uint64_t a) {   // 这个地址是窗口里的第几页；不在窗口 -1
    if (pool != &g_vpool) return -1;
    return a < vp_base() || a >= vp_base() + (uint64_t)VPAGES * VP ? -1 : (int)((a - vp_base()) / VP);
}
int nvn_sparse_pool(const void *pool) { return pool == &g_vpool || pool == &g_vpool2; }
uint64_t nvn_sparse_gen(void) { return g_vgen; }
uint64_t nvn_sparse_copy(const void *pool, void *dst, uint64_t a, uint64_t n) {
    g_spc.copy_calls++; g_spc.copy_bytes += n;
    uint64_t miss = 0;
    for (uint64_t p = 0; p < n;) {
        const uint64_t at = a + p; uint64_t k = VP - (at & (VP - 1)); if (k > n - p) k = n - p;
        const int vp = vp_page(pool, at);
        if (vp >= 0 && g_vpt[vp].ph >= 0) memcpy((uint8_t *)dst + p, g_phys + (size_t)g_vpt[vp].ph * VP + (at & (VP - 1)), (size_t)k);
        else { memset((uint8_t *)dst + p, 0, (size_t)k); miss += k; }
        p += k;
    }
    return miss;
}
void nvn_sparse_protect(const void *pool, uint64_t a, uint64_t n) {
    g_spc.prot_calls++; g_spc.prot_bytes += n;
    for (uint64_t p = 0; p < n; p += VP) { const int vp = vp_page(pool, a + p); if (vp >= 0 && g_vpt[vp].ph >= 0) g_phys_armed[g_vpt[vp].ph] = 1; }
}
int nvn_sparse_since(const void *pool, uint64_t a, uint64_t n, uint64_t gen, uint64_t stamp) {   // 同 hle_nvn.c vpt_walk：先看页的映射代号（取消映射也算），再看背后的物理页写代号
    g_spc.walks++;
    if (!nvn_sparse_pool(pool)) return 1;
    for (uint64_t p = 0; p < n; p += VP) {
        const int vp = vp_page(pool, a + p); if (vp < 0) continue;
        if (g_vpt[vp].gen > gen) return 1;
        if (g_vpt[vp].ph >= 0 && g_phys_gen[g_vpt[vp].ph] > stamp) return 1;
    }
    return 0;
}
uint64_t nvn_sparse_log_seq(const void *pool) { (void)pool; return g_nvlog; }
int nvn_sparse_log_hit(const void *pool, uint64_t *seq, uint64_t a, uint64_t n) {
    (void)pool; g_spc.hit_calls++;
    if (g_nvlog - *seq > VLOG) return 1;   // 日志转过一圈：说不清，当有
    const uint64_t lo = a - (uint64_t)(uintptr_t)g_vpool.mem;
    for (uint64_t k = *seq; k < g_nvlog; k++) { const uint64_t eo = g_vlog[k % VLOG].off, el = g_vlog[k % VLOG].len; if (eo < lo + n && lo < eo + el) return 1; }
    *seq = g_nvlog; return 0;
}
int nvn_sparse_runs(const void *pool, uint64_t a, uint64_t n, uint64_t (*out)[2], int max) {
    g_spc.runs_calls++;
    int k = 0; uint64_t rs = 0, re = 0;
    for (uint64_t at = a & ~(uint64_t)(VP - 1); at < a + n; at += VP) {
        const int vp = vp_page(pool, at); if (vp < 0 || g_vpt[vp].ph < 0) continue;
        const uint64_t ph = (uint64_t)(uintptr_t)(g_phys + (size_t)g_vpt[vp].ph * VP);
        if (rs && ph == re) { re += VP; continue; }
        if (rs) { if (k == max) return -1; out[k][0] = rs; out[k][1] = re - rs; k++; }
        rs = ph; re = rs + VP;
    }
    if (rs) { if (k == max) return -1; out[k][0] = rs; out[k][1] = re - rs; k++; }
    return k;
}
// 写跟踪替身：物理池的页按页记写代号；其余内存照旧用替身的全局答法
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) {
    const uintptr_t a = (uintptr_t)addr;
    if (len && a >= (uintptr_t)g_phys && a + len <= (uintptr_t)g_phys + sizeof g_phys) {
        g_spc.since_calls++;
        for (size_t k = (a - (uintptr_t)g_phys) / VP, e = (a + len - 1 - (uintptr_t)g_phys) / VP; k <= e; k++) if (g_phys_gen[k] > stamp) return 1;
        return 0;
    }
    return fixture_dirty_since(addr, len, stamp);
}
// 上传捕获：每次上传的整块字节取散列（FNV-1a），记级 / 层 / 字节数 / 有没有非零字节；仍转给替身记账
static struct { uint32_t level, layer; uint64_t bytes, hash; int nz; } g_cap[96]; static int g_ncap;
nvn_gpu_rc nvn_gpu_tex_upload(nvn_gpu_cmd c, nvn_gpu_tex t, uint32_t level, uint32_t layer, const NVNcopyRegion *r, const void *src, uint64_t row_bytes, uint64_t image_bytes) {
    if (g_ncap < 96) {
        uint64_t h = 1469598103934665603ull; int nz = 0; const uint8_t *p = src;
        for (uint64_t i = 0; i < image_bytes; i++) { h = (h ^ p[i]) * 1099511628211ull; nz |= p[i] != 0; }
        g_cap[g_ncap].level = level; g_cap[g_ncap].layer = layer; g_cap[g_ncap].bytes = image_bytes; g_cap[g_ncap].hash = h; g_cap[g_ncap].nz = nz; g_ncap++;
    }
    return fixture_gpu_tex_upload(c, t, level, layer, r, src, row_bytes, image_bytes);
}

// ---------- 用例辅助 ----------
static void sp_fresh(void) {
    res_fresh();
    memset(g_phys_gen, 0, sizeof g_phys_gen); memset(g_phys_armed, 0, sizeof g_phys_armed);
    for (int i = 0; i < VPAGES; i++) { g_vpt[i].ph = -1; g_vpt[i].gen = 0; }
    for (int p = 0; p < PHYS_PAGES; p++) for (int j = 0; j < VP; j++) g_phys[(size_t)p * VP + j] = (uint8_t)(p * 31 + j * 7 + 1);   // 每页内容不同、基本非零
    g_vgen = 0; g_nvlog = 0; memset(&g_spc, 0, sizeof g_spc); g_ncap = 0; g_nup = 0;
    g_dirty_now = 100; g_dirty_written = 0; g_protects = 0; nvn_unimpl_reset();
    if (!g_vbase) { g_vbase = mmap(NULL, 1 << 20, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0); CHECK(g_vbase != MAP_FAILED); }
    memset(&g_vpool, 0, sizeof g_vpool); g_vpool.mem = g_vbase; g_vpool.size = 64ull << 30;
    g_vpool.flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL | NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED | NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE;   // 真机的 0x2a1
    memset(&g_vpool2, 0, sizeof g_vpool2); g_vpool2.mem = g_tmem; g_vpool2.size = sizeof g_tmem; g_vpool2.flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
}
// 映射：窗口里 [vpage, vpage + n) 逐页指到 ph[i]（ph = NULL：按打乱的物理页 (页号 * 37 + 11) % PHYS_PAGES，相邻页不连号；负数 = 取消映射）。一次请求 = 一个代号 + 一条日志
static void vmap(int vpage, int n, const int *ph) {
    g_vgen++;
    g_vlog[g_nvlog % VLOG].off = VOFF0 + (uint64_t)vpage * VP; g_vlog[g_nvlog % VLOG].len = (uint64_t)n * VP; g_nvlog++;
    for (int i = 0; i < n; i++) { const int p = ph ? ph[i] : (int)(((long)(vpage + i) * 37 + 11) % PHYS_PAGES); g_vpt[vpage + i].ph = p < 0 ? -1 : p; g_vpt[vpage + i].gen = g_vgen; }
}
static int phys_of(int vpage) { return g_vpt[vpage].ph; }
static void phys_write(int page, int off, uint8_t v, int n) { memset(g_phys + (size_t)page * VP + off, v, (size_t)n); g_phys_gen[page] = ++g_dirty_now; }
// 一个没被 [v0, v0 + n) 这几页用到的物理页
static int free_phys(int v0, int n) { for (int p = PHYS_PAGES - 1; p >= 0; p--) { int used = 0; for (int i = 0; i < n; i++) used |= g_vpt[v0 + i].ph == p; if (!used) return p; } return -1; }
// 虚拟池里的一张贴图：窗口起点 + 字节偏移（4096 对齐）
static tex_t sp_tex(uint64_t serial, uint64_t voff, int fmt, uint32_t w, uint32_t h, uint32_t levels, int flags) {
    tex_t t = mk_tex(serial, 0, fmt, w, h, levels, flags);
    t.pool = &g_vpool; t.off = (int64_t)(VOFF0 + voff); t.mem = (uint8_t *)((uintptr_t)g_vbase + VOFF0 + voff);
    return t;
}
static int sp_pages(const tex_t *t) { return (int)((t->L.size + VP - 1) / VP); }
static int sp_v0(const tex_t *t) { return (int)(((uint64_t)t->off - VOFF0) / VP); }
static uint8_t g_want[(size_t)PHYS_PAGES * VP];
// 期望的存储字节：虚拟页逐页取背后的物理页，没映射的按 0（不经被测实现）
static void compose(const tex_t *t) {
    const int p0 = sp_v0(t), n = sp_pages(t);
    memset(g_want, 0, (size_t)n * VP);
    for (int i = 0; i < n; i++) if (g_vpt[p0 + i].ph >= 0) memcpy(g_want + (size_t)i * VP, g_phys + (size_t)g_vpt[p0 + i].ph * VP, VP);
}
static int cap_same(int a0, int a1, int b0, int b1) {   // 两段上传捕获逐项相同（级 / 层 / 字节数 / 散列），且不为空
    if (a1 - a0 != b1 - b0 || a1 == a0) return 0;
    for (int i = 0; i < a1 - a0; i++) {
        if (g_cap[a0 + i].level != g_cap[b0 + i].level || g_cap[a0 + i].layer != g_cap[b0 + i].layer || g_cap[a0 + i].bytes != g_cap[b0 + i].bytes || g_cap[a0 + i].hash != g_cap[b0 + i].hash) return 0;
    }
    return 1;
}
static int cap_nz(int a, int b) { for (int i = a; i < b; i++) if (g_cap[i].nz) return 1; return 0; }
// 上传次数：扣掉参照贴图自己的那几次（rp_res_uploads 是全局累计）
static uint64_t g_ref_ups;
static uint64_t ups(void) { return rp_res_uploads() - g_ref_ups; }
// 参照上传：期望字节（g_want）放进普通池里的同形同标志贴图，走原有的 CPU 贴图上传；返回它的上传捕获 [*a, *b)
static void ref_upload(const tex_t *like, int *a, int *b) {
    static uint64_t serial = 9900;
    tex_t r = mk_tex(++serial, 0x100000, like->d.format, like->d.width, like->d.height, like->d.levels, like->d.flags);
    CHECK(r.L.size == like->L.size);
    memcpy(r.mem, g_want, (size_t)r.L.size);
    const uint64_t u = rp_res_uploads();
    *a = g_ncap; CHECK(rp_tex_resolve(&r, &g_fake_cmd) != NULL); *b = g_ncap;
    g_ref_ups += rp_res_uploads() - u;
    nvn_be_tex_release(&r);
}
// 上传了的内容是否等于「此刻页表 / 物理页背后的内容」：重算期望、走参照路径比
static int content_ok(const tex_t *t, int c0, int c1) {
    int r0, r1; compose(t); ref_upload(t, &r0, &r1);
    return r1 > r0 && cap_same(c0, c1, r0, r1);
}
static void seg_next(void) { nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); nvn_be_sub_begin(); }   // 换一次提交（新的一段）
static tex_t sp_astc(uint64_t serial, uint64_t voff, int flags) { return sp_tex(serial, voff, NVN_FORMAT_RGBA_ASTC_6x6, 256, 256, 5, flags); }   // 真机里最多的一类：ASTC 6x6、多级

// ---------- ① 内容：按页表读，没映射的页按 0 ----------
static void t_sp_content(void) {
    sp_fresh(); nvn_be_sub_begin();
    tex_t a = sp_astc(9101, 0x40000, 0);   // 压缩、多级、标志 0、虚拟池；映射到打乱的物理页
    const int np = sp_pages(&a), v0 = sp_v0(&a);
    CHECK(np >= 8 && np < 64);
    vmap(v0, np, NULL);
    compose(&a);
    int r0, r1; ref_upload(&a, &r0, &r1);
    CHECK(r1 - r0 == 5 && cap_nz(r0, r1));                                    // 参照有 5 级上传、确实有内容（不是全 0 比全 0）
    const int c0 = g_ncap; const uint64_t u0 = ups(); const int pr0 = g_protects;
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL);
    CHECK(ups() == u0 + 1);                                         // 首用：整张传一次
    CHECK(cap_same(c0, g_ncap, r0, r1));                                       // 逐级逐层字节与参照相同
    CHECK(g_spc.copy_calls == 1 && g_spc.copy_bytes == a.L.size);              // 按页表读了整段存储
    CHECK(g_spc.prot_calls == 1 && g_spc.prot_bytes == a.L.size && g_protects == pr0);   // 加保护走页表那条（背后的物理页），没去保护 t->mem 这个不可读的地址
    { int armed = 1; for (int i = 0; i < np; i++) armed &= g_phys_armed[phys_of(v0 + i)]; CHECK(armed); }
    // 没映射的页读 0：第 1、4 页没映射 → 期望里这两页是 0，其余照旧
    tex_t b = sp_astc(9103, 0x80000, 0);
    const int bv0 = sp_v0(&b);
    vmap(bv0, np, NULL); vmap(bv0 + 1, 1, (int[]){-1}); vmap(bv0 + 4, 1, (int[]){-1});
    compose(&b);
    { int hole = 1; for (int j = 0; j < VP; j++) hole &= g_want[VP + j] == 0 && g_want[4 * VP + j] == 0; CHECK(hole); CHECK(g_want[0] != 0 || g_want[1] != 0); }   // 期望自检：空洞是 0，邻页不是
    const int d0 = g_ncap; CHECK(rp_tex_resolve(&b, &g_fake_cmd) != NULL);
    { const int d1 = g_ncap; int q0, q1; ref_upload(&b, &q0, &q1); CHECK(q1 > q0 && cap_same(d0, d1, q0, q1)); }
    // 一页都没映射：上传全 0（有内容来源、内容是 0；不是「没有来源」）
    tex_t z = sp_astc(9105, 0xC0000, 0);
    const int e0 = g_ncap; const uint64_t uz = ups();
    CHECK(rp_tex_resolve(&z, &g_fake_cmd) != NULL && ups() == uz + 1 && g_ncap - e0 == 5 && !cap_nz(e0, g_ncap));
    nvn_be_tex_release(&a); nvn_be_tex_release(&b); nvn_be_tex_release(&z);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// ---------- ② 判脏：稳态检查 ----------
// 同一段没有写 / 映射：纪元快路；换段、游戏每段都在写别处：只查背后几段物理页（不走页表）；背后的页被写：重传一次，缓存重建。
static void t_sp_steady(void) {
    sp_fresh(); nvn_be_sub_begin();
    tex_t a = sp_astc(9111, 0x40000, 0);
    const int np = sp_pages(&a), v0 = sp_v0(&a);
    vmap(v0, np, NULL);
    const uint64_t u0 = ups();
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && ups() == u0 + 1);
    for (int i = 0; i < 3; i++) CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL);   // 同一段：没有任何写 / 映射
    CHECK(ups() == u0 + 1 && g_spc.walks == 0 && g_spc.since_calls == 0 && g_spc.hit_calls == 0 && g_spc.copy_calls == 1);
    seg_next();
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL);   // 换段：核对一次，页表整段走一次、记下背后的物理段
    CHECK(ups() == u0 + 1 && g_spc.walks == 1 && g_spc.runs_calls == 1);
    const int un = free_phys(v0, np);
    for (int s = 0; s < 5; s++) {   // 每段游戏都在写别处的页（全局写代号每段都动）：只查那 np 段物理页，不再走页表
        phys_write(un, 0, (uint8_t)(0x11 + s), 16); seg_next();
        CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL);
    }
    CHECK(ups() == u0 + 1 && g_spc.walks == 1 && g_spc.runs_calls == 1 && g_spc.hit_calls == 5 && g_spc.since_calls == 5 * np && g_spc.copy_calls == 1);
    // 背后的一页被写（同一段里）：重传一次，内容是新的
    const int k = 3, cc = g_ncap;
    phys_write(phys_of(v0 + k), 5, 0xAB, 50);
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL);
    CHECK(ups() == u0 + 2 && g_spc.copy_calls == 2 && content_ok(&a, cc, g_ncap));
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && ups() == u0 + 2);   // 再用：同一段、没有新写：不再传
    seg_next();
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && ups() == u0 + 2 && g_spc.walks == 2 && g_spc.runs_calls == 2);   // 重传后第一次核对：整段走一次、按新代号重建缓存
    // 再写背后另一页（换段之后）：再重传一次
    phys_write(phys_of(v0 + 7), 0, 0xCD, 4); seg_next();
    const int cc2 = g_ncap;
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && ups() == u0 + 3 && content_ok(&a, cc2, g_ncap));
    nvn_be_tex_release(&a);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// ---------- ② 判脏：页表映射变化 ----------
static void t_sp_changes(void) {
    sp_fresh(); nvn_be_sub_begin();
    tex_t a = sp_astc(9121, 0x40000, 0);
    const int np = sp_pages(&a), v0 = sp_v0(&a);
    vmap(v0, np / 2, NULL);   // 起初只映射前一半，后一半没映射（读 0）
    const uint64_t u0 = ups(); int cc = g_ncap;
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && ups() == u0 + 1 && content_ok(&a, cc, g_ncap));
    // (a) 同一段里补映射后一半：只有映射代号动，没有写任何页（物理页写代号、全局写代号都不动）→ 必须重传（纪元键里要有映射代号）
    cc = g_ncap; vmap(v0 + np / 2, np - np / 2, NULL);
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && ups() == u0 + 2 && content_ok(&a, cc, g_ncap));
    // (b) 把第 2 页重映射到另一个物理页（内容不同）
    cc = g_ncap; vmap(v0 + 2, 1, (int[]){free_phys(v0, np)});
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && ups() == u0 + 3 && content_ok(&a, cc, g_ncap));
    // (c) 取消第 5 页的映射 → 读 0
    cc = g_ncap; vmap(v0 + 5, 1, (int[]){-1});
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && ups() == u0 + 4 && content_ok(&a, cc, g_ncap));
    seg_next();
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && ups() == u0 + 4);   // 之后没变：不再传（这一次整段走、重建缓存）
    // (d) 窗口里别处的映射（贴图范围之外）：不重传、不走页表（缓存是热的，日志里没有重叠请求）
    const int w0 = g_spc.walks;
    vmap(1000, 8, NULL); seg_next();
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && ups() == u0 + 4 && g_spc.walks == w0);
    // (e) 无关映射请求多到日志转一圈：说不清，整段走一次，仍是干净的，不重传
    for (int i = 0; i < 20; i++) vmap(1100 + i, 1, NULL);
    seg_next();
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && ups() == u0 + 4 && g_spc.walks == w0 + 1);
    // (f) 映射请求和本贴图重叠（再映射同一批页）：重传（页的映射代号变大了），内容仍与期望一致
    cc = g_ncap; vmap(v0, 3, NULL); seg_next();
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && ups() == u0 + 5 && content_ok(&a, cc, g_ncap));
    nvn_be_tex_release(&a);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// ---------- ③ 范围 ----------
static void t_sp_scope(void) {
    sp_fresh(); nvn_be_sub_begin();
    // 不走页表的：未压缩；带 COMPRESSIBLE / IMAGE / DISPLAY 标志的压缩贴图（它们要么是渲染目标、要么内容归 GPU）
    struct { int fmt, flags; } ex[] = {
        {NVN_FORMAT_RGBA8, 0}, {NVN_FORMAT_RGBA_ASTC_6x6, NVN_TEXTURE_FLAGS_COMPRESSIBLE}, {NVN_FORMAT_RGBA_ASTC_6x6, NVN_TEXTURE_FLAGS_IMAGE},
        {NVN_FORMAT_RGBA_ASTC_6x6, NVN_TEXTURE_FLAGS_DISPLAY}, {NVN_FORMAT_RGBA_DXT1, NVN_TEXTURE_FLAGS_IMAGE | NVN_TEXTURE_FLAGS_COMPRESSIBLE},
    };
    for (int i = 0; i < (int)(sizeof ex / sizeof ex[0]); i++) {
        tex_t e = sp_tex(9130 + (uint64_t)i, 0x40000 + (uint64_t)i * 0x20000, ex[i].fmt, 64, 64, 1, ex[i].flags);
        vmap(sp_v0(&e), sp_pages(&e), NULL);
        const uint64_t u = ups();
        CHECK(rp_tex_resolve(&e, &g_fake_cmd) != NULL);
        CHECK(g_spc.copy_calls == 0 && g_spc.prot_calls == 0 && ups() == u);   // 一页表都没读、没加保护、没传 CPU 内容：照旧
        nvn_be_tex_release(&e);
    }
    // 普通池里的压缩贴图：照旧走 CPU 贴图（内存里的字节），不走页表
    { tex_t n = mk_tex(9140, 0x10000, NVN_FORMAT_RGBA_ASTC_6x6, 64, 64, 1, 0); memset(n.mem, 0x6B, n.L.size);
      const uint64_t u = ups(); g_nup = 0;
      CHECK(rp_tex_resolve(&n, &g_fake_cmd) != NULL && ups() == u + 1 && g_spc.copy_calls == 0 && g_spc.prot_calls == 0 && g_nup == 1 && g_up[0].first[0] == 0x6B);
      nvn_be_tex_release(&n); }
    // SPARSE 标志的压缩贴图：照样走页表（与 Apple 端一致：稀疏标志不排除）。普通池里没有它的参照路径（线性族的判定把 SPARSE 标志一并拒掉），
    //   这里不比参照：核「走了页表、传了非零内容、背后的页一变内容跟着变」
    { tex_t s = sp_astc(9141, 0x100000, NVN_TEXTURE_FLAGS_SPARSE); vmap(sp_v0(&s), sp_pages(&s), NULL);
      const uint64_t u = ups(); const int c0 = g_ncap;
      CHECK(rp_tex_resolve(&s, &g_fake_cmd) != NULL && ups() == u + 1 && g_spc.copy_calls == 1 && g_spc.copy_bytes == s.L.size);
      const int c1 = g_ncap; CHECK(c1 - c0 == 5 && cap_nz(c0, c1));
      phys_write(phys_of(sp_v0(&s)), 0, 0x77, 64);   // 背后第一页变了 → 重传，至少一级的字节跟着变
      CHECK(rp_tex_resolve(&s, &g_fake_cmd) != NULL && ups() == u + 2 && g_ncap - c1 == 5);
      { int diff = 0; for (int i = 0; i < 5; i++) diff |= g_cap[c0 + i].hash != g_cap[c1 + i].hash; CHECK(diff); }
      nvn_be_tex_release(&s); }
    // 超过副本上限（整张读成一份放不下）：不传、不读页表，准确报红；贴图本身照旧能用
    { tex_t big = sp_tex(9142, 0, NVN_FORMAT_RGBA_ASTC_4x4, 16384, 16384, 2, 0);
      CHECK(big.L.size > (256u << 20));
      const uint64_t u = ups(); const int cc = g_spc.copy_calls;
      CHECK(rp_tex_resolve(&big, &g_fake_cmd) != NULL && ups() == u && g_spc.copy_calls == cc);
      CHECK(nvn_unimpl_count("limit", "tex_sparse_pool_too_large") == 1);
      nvn_be_tex_release(&big); }
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// ---------- 分块读：一次读不超过 1 MiB（不在前端的页表锁里一口气拷几十兆） ----------
static void t_sp_big(void) {
    sp_fresh(); nvn_be_sub_begin();
    tex_t a = sp_tex(9151, 0x100000, NVN_FORMAT_RGBA_ASTC_4x4, 2048, 1024, 1, 0);   // 2 MiB 左右，跨块边界
    const int np = sp_pages(&a), v0 = sp_v0(&a);
    CHECK(a.L.size > 2 * (uint64_t)SP_CHUNK - 4096 && np < PHYS_PAGES);
    vmap(v0, np, NULL);
    const int c0 = g_ncap; const uint64_t u0 = ups();
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && ups() == u0 + 1);
    CHECK(g_spc.copy_calls == (int)((a.L.size + SP_CHUNK - 1) / SP_CHUNK) && g_spc.copy_bytes == a.L.size && g_spc.copy_calls >= 2);
    CHECK(content_ok(&a, c0, g_ncap));
    nvn_be_tex_release(&a);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// ---------- WriteTexels 写过的贴图：字节写在游戏看得到的地址上，照旧直接读，不走页表 ----------
static void t_sp_direct(void) {
    sp_fresh(); nvn_be_sub_begin();
    tex_t t = mk_tex(9161, 0x1000, NVN_FORMAT_RGBA_ASTC_6x6, 64, 64, 1, 0);
    t.pool = &g_vpool2; memset(t.mem, 0x5A, t.L.size);   // 虚拟池标志的池、存储是真内存
    nvn_be_tex_cpu_written(&t);
    const uint64_t u0 = ups(); const int p0 = g_protects;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL);
    CHECK(ups() == u0 + 1 && g_spc.copy_calls == 0 && g_spc.prot_calls == 0 && g_protects == p0 + 1);   // 直接读、用原来的写跟踪
    CHECK(g_nup == 1 && g_up[0].first[0] == 0x5A);                                                              // 传的是真内存里的字节
    nvn_be_tex_release(&t);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// ---------- ④ 随包布局的虚拟池贴图 ----------
// 真机的随包布局字节 03 04（块高 8 GOB、行距对齐 16 GOB）：d.pkg_layout = (3 | 4 << 8) + 1，贴图对象带 has_layout
static void pkg(tex_t *t) { t->has_layout = 1; t->d.pkg_layout = (3 | 4 << 8) + 1; nvn_tex_layout(&t->d, &t->L); }
// 期望的上传字节（SPARSE 标志的贴图在普通池里没有参照路径）：期望存储 g_want 直接交公共反交织，逐级取散列，与上传捕获 [c0, c1) 比（二维单层：一级一次上传）
static int deswz_same(const tex_t *t, int c0, int c1) {
    const uint32_t levels = t->d.levels ? t->d.levels : 1;
    int bw = 1, bh = 1; const int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (c1 - c0 != (int)levels || bpb <= 0) return 0;
    compose(t);
    static uint8_t tmp[1 << 20];
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t w = t->d.width >> l, h = t->d.height >> l; if (!w) w = 1; if (!h) h = 1;
        const size_t pitch = (size_t)((w + (uint32_t)bw - 1) / (uint32_t)bw) * (size_t)bpb, rows = (h + (uint32_t)bh - 1) / (uint32_t)bh, img = pitch * rows;
        if (img > sizeof tmp || nvn_deswizzle(&t->d, l, 0, g_want, tmp, pitch)) return 0;
        uint64_t hs = 1469598103934665603ull; for (size_t i = 0; i < img; i++) hs = (hs ^ tmp[i]) * 1099511628211ull;
        if (g_cap[c0 + l].level != l || g_cap[c0 + l].layer != 0 || g_cap[c0 + l].bytes != img || g_cap[c0 + l].hash != hs) return 0;
    }
    return 1;
}
static void t_sp_packaged(void) {
    sp_fresh(); nvn_be_sub_begin();
    // (a) 真机的组合：随包布局 + 只有 SPARSE 标志 + 虚拟池（0x2a1）+ 压缩（ASTC 6x6、多级）
    tex_t a = sp_astc(9171, 0x40000, NVN_TEXTURE_FLAGS_SPARSE); pkg(&a);
    const int np = sp_pages(&a), v0 = sp_v0(&a);
    CHECK(a.has_layout && np >= 8 && np < PHYS_PAGES / 2);
    vmap(v0, np, NULL);
    const uint64_t u0 = ups(); const int c0 = g_ncap;
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL);
    CHECK(ups() == u0 + 1);                                                        // 首用整张传一次（过去 0 次）
    CHECK(g_spc.copy_calls == 1 && g_spc.copy_bytes == a.L.size);                  // 按页表读了整段存储
    CHECK(g_ncap - c0 == 5 && cap_nz(c0, g_ncap) && deswz_same(&a, c0, g_ncap));   // 5 级上传，字节 = 期望存储交公共反交织
    CHECK(nvn_unimpl_count("tex", "linear_render_target_unsupported") == 0);       // 不再被「SPARSE 不是线性族」拒
    // (b) 背后的一页被写 → 重传一次，内容是新的
    const int c1 = g_ncap; phys_write(phys_of(v0 + 3), 5, 0xAB, 50);
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && ups() == u0 + 2 && g_ncap - c1 == 5 && deswz_same(&a, c1, g_ncap));
    // (c) 只有映射变了（没写任何页）→ 再重传一次，内容跟着变
    const int c2 = g_ncap; vmap(v0 + 2, 1, (int[]){free_phys(v0, np)});
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && ups() == u0 + 3 && g_ncap - c2 == 5 && deswz_same(&a, c2, g_ncap));
    // (d) 之后没变：不再传（稳态）
    seg_next();
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && ups() == u0 + 3);
    nvn_be_tex_release(&a);
    // (e) 随包 + SPARSE + 虚拟池的未压缩布局仍不走页表。读端必须收到明确的准备失败。
    { tex_t u = sp_tex(9172, 0x100000, NVN_FORMAT_RGBA8, 64, 64, 1, NVN_TEXTURE_FLAGS_SPARSE); pkg(&u);
      vmap(sp_v0(&u), sp_pages(&u), NULL);
      const int cc = g_spc.copy_calls, pc = g_spc.prot_calls;
      const uint64_t rejected = nvn_unimpl_count("tex", "linear_sparse_unsupported");
      CHECK(rp_tex_resolve(&u, &g_fake_cmd) == NULL && g_spc.copy_calls == cc && g_spc.prot_calls == pc &&
            nvn_unimpl_count("tex", "linear_sparse_unsupported") == rejected + 1);
      nvn_be_tex_release(&u); }
    // (f) 随包布局 + 压缩 + 普通池（不是虚拟池）：照旧直接读池里的字节（flags 0），不走页表
    { tex_t n = mk_tex(9173, 0x10000, NVN_FORMAT_RGBA_ASTC_6x6, 64, 64, 1, 0); pkg(&n); memset(n.mem, 0x6C, n.L.size);
      const uint64_t u = ups(); g_nup = 0; const int cc = g_spc.copy_calls;
      CHECK(rp_tex_resolve(&n, &g_fake_cmd) != NULL && ups() == u + 1 && g_spc.copy_calls == cc && g_nup == 1 && g_up[0].first[0] == 0x6C);
      nvn_be_tex_release(&n); }
    // (g) 随包布局 + WriteTexels 写过 + 虚拟池标志的池（存储是真内存）：先看 WriteTexels → 照旧直接读，不走页表
    { tex_t w = mk_tex(9174, 0x2000, NVN_FORMAT_RGBA_ASTC_6x6, 64, 64, 1, 0); pkg(&w);
      w.pool = &g_vpool2; memset(w.mem, 0x5B, w.L.size); nvn_be_tex_cpu_written(&w);
      const uint64_t u = ups(); g_nup = 0; const int cc = g_spc.copy_calls;
      CHECK(rp_tex_resolve(&w, &g_fake_cmd) != NULL && ups() == u + 1 && g_spc.copy_calls == cc && g_nup == 1 && g_up[0].first[0] == 0x5B);
      nvn_be_tex_release(&w); }
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

int main(void) {
    t_sp_content();
    t_sp_steady();
    t_sp_changes();
    t_sp_scope();
    t_sp_big();
    t_sp_direct();
    t_sp_packaged();
    printf("t_nvn_sparse_tex：%d 项检查，%d 项红\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
