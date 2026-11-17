// 内存池在后端的镜像缓冲，与缓冲类命令。
// iOS 上内存池就是零拷贝的 MTLBuffer（nvn_mtl.m pool_add @556），GPU 直接读游戏内存。Vulkan 没有通用的等价物，这里的做法是：
//   · 游戏内存是唯一真值；每个被当缓冲用到的池在后端有一份等大的镜像，按页同步；
//   · 同步点 = 每次使用点（绘制 / 派发 / 拷贝取这段地址时）：自上次同步后被写过的页，在当前段的 GPU 顺序里重传；
//   · 所以按 GPU 顺序生效的 CPU 数据写（OP_UPDATE_UB / OP_COPY_B2B / OP_CLEAR_BUF）只需要改游戏内存：
//     排在它前面的使用点已经带着旧内容同步过，排在它后面的使用点会看到页脏了再同步新内容。
// GPU 往池里写（贴图拷回缓冲、计算着色器写存储缓冲、变换反馈）要反方向回写游戏内存，随那几条命令一起接。
// 平台有零拷贝后备（安卓 AHB）时镜像与同步都可以省掉，那是平台接口「内存池后备」一组，接它不改这里的对外形状。
#include "rp_exec.h"
#include "rp_res.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "nvn_hmap.h"
#include "nvn_fmt_info.h"
#include "nvn_dirty.h"
#include "nvn_mtl.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct gpu_write { uint64_t off, len, id; uint8_t mi; };   // GPU 写进镜像、还没回写到游戏内存的一段；mi = 写进了哪一份镜像
struct mirror_block { uint64_t version, epoch, covered; };
// 一份镜像。synced[i]：第 i 页上次同步取的脏页代号，0 = 从没同步；sub[i]：那次同步所在的提交序号；
// checked[i]：第 i 页上次逐页核对时的全局写代号（代号没动 = 那之后游戏没写过任何被跟踪的页，这一页不用再核）。
// use_sub / use_fence：最近一次用到这份镜像的提交（序号、栅栏值）；prev_fence：再往前一次用到它的提交的栅栏值——
// 它做完了，显卡就不再读这份镜像里的旧内容，这次提交第一次同步的页可以直接写。
struct mirror { nvn_gpu_buf buf; uint64_t *synced, *sub, *checked; struct mirror_block *blocks; uint64_t seen_map; uint8_t seen_ok; uint64_t use_sub, use_fence, prev_fence; };
// 每个池最多两份镜像。平时只有第 0 份；游戏的 Finish 被放行、不等上一帧做完时（rp_exec.c nvn_be_finish_publish），每帧换一份用：
// 上一帧的活显卡还在做，读的是另一份。两份各记各的页代号，换回来时自己把这两帧里变过的页补上。
// landing：正在锁外把 GPU 写回的内容拷进游戏内存（拷贝是 13 MB/帧量级，压在全局锁里会把提交线程挡住 11–21 毫秒/帧）。
// 拷贝期间不允许对同一份池做「直接写镜像」（会与拷贝读到的内容打架）；dead：池在拷贝期间被销毁，等拷贝收尾再释放。
// landing/nlr：有多段「显卡写回」正在锁外拷进游戏内存。拷贝期间**只**禁止对这些区间做「直接写镜像」——
// 以前按整份池禁止，走路时剔除数据每帧都在落地，于是几乎每次解析都退回「录成 GPU 拷贝 + 全屏障」，
// 真机表现为走路一顿一顿（池镜像同步 4.2 MB/帧、其中直接写只剩 0.2 MB，见坑记录续 33）。
struct land_rng { uint64_t off, len; };
struct shadow { const uint8_t *mem; uint64_t size; struct mirror m[2]; uint64_t npages, nblocks; uintptr_t page0; struct gpu_write *gw; int ngw, cgw; int landing, dead;
                int blocks_on;
                int failed;   // 第一份镜像建不出来：记住，之后这个池直接按未接返回，不再每笔重建（池释放时随表项删掉，重建池会重试）
                struct land_rng lr[16]; int nlr; };
static int g_mirror_cur;   // 现在用第几份；只在提交锁里改（Finish 与提交互斥）
static pthread_mutex_t g_bmu = PTHREAD_MUTEX_INITIALIZER;
static nvn_hmap g_shadow = {.klen = 8};   // 池起点 → struct shadow *
static uint64_t g_sync_bytes, g_sync_runs;
static nvn_gpu_buf g_null_storage;

int rp_pool_find(uint64_t addr, uint64_t len, const uint8_t **mem, uint64_t *size);   // rp_entry.c

static void mirror_free(struct mirror *m) { if (m->buf) nvn_gpu_buf_release(m->buf, NULL, NULL); free(m->synced); free(m->sub); free(m->checked); free(m->blocks); memset(m, 0, sizeof *m); }
static void shadow_free(struct shadow *s) { mirror_free(&s->m[0]); mirror_free(&s->m[1]); free(s->gw); free(s); }
// 镜像映射代号：新建镜像、换镜像都会让「同一段地址解析出的 (缓冲, 偏移)」变掉。回放核心的解析结果缓存按它作废。

static int mirror_make(struct shadow *s, int i) {
    struct mirror *m = &s->m[i];
    nvn_gpu_buf b = NULL; nvn_gpu_err err = {0};
    if (!nvn_gpu_ok(nvn_gpu_buf_create(s->size, NVN_GPU_MEM_SHARED, NULL, &b, &err))) {
        // 完整原因：池大小、申请字节、单次分配上限、SSBO 范围上限、错误码、后端原文。按池大小去重（同一池每笔都会走到这里）。
        nvn_gpu_caps cp; nvn_gpu_caps_get(&cp);
        static uint64_t said_size[8]; static int nsaid;
        int dup = 0; for (int k = 0; k < nsaid; k++) if (said_size[k] == s->size) dup = 1;
        if (!dup && nsaid < 8) { said_size[nsaid++] = s->size;
            char t[200]; snprintf(t, sizeof t, "池镜像（第 %d 份）建不出来：池 %p+%llu 字节，申请 %llu，分配上限 %llu，SSBO 范围上限 %llu，错误 %d：%s",
                                  i, (const void *)s->mem, (unsigned long long)s->size, (unsigned long long)s->size,
                                  (unsigned long long)cp.max_buf_bytes, (unsigned long long)cp.max_ssbo_range, (int)err.rc.code, err.why);
            nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", t); }
        return 0; }
    m->buf = b;
    rp_perf_add(RP_PERF_MIRROR_N, 1);
    m->synced = calloc(s->npages ? s->npages : 1, sizeof *m->synced);
    m->sub = calloc(s->npages ? s->npages : 1, sizeof *m->sub);
    m->checked = calloc(s->npages ? s->npages : 1, sizeof *m->checked);
    if (!m->synced || !m->sub || !m->checked) { mirror_free(m); return 0; }   // 页表没分出来：不留半份镜像（之后每次使用都会读它）
    if (s->blocks_on && s->nblocks <= SIZE_MAX / sizeof *m->blocks)
        m->blocks = calloc(s->nblocks ? (size_t)s->nblocks : 1, sizeof *m->blocks); // 失败时沿用逐页路径
    return 1;
}
static struct shadow *shadow_get(const uint8_t *mem, uint64_t size) {
    uint64_t k = (uint64_t)(uintptr_t)mem;
    struct shadow *s = nvn_hm_get(&g_shadow, &k);
    if (s) {
        if (s->failed) { nvn_unimpl("gpu", "buf_create_pool_mirror", mem); return NULL; }
        return s;
    }
    size_t ps = nvn_dirty_page_size();
    s = calloc(1, sizeof *s);
    if (!s) { nvn_unimpl("gpu", "pool_mirror_alloc_failed", mem); return NULL; }
    s->mem = mem; s->size = size;
    s->page0 = (uintptr_t)mem & ~(uintptr_t)(ps - 1);
    s->npages = ((uintptr_t)mem + size - s->page0 + ps - 1) / ps;
    s->blocks_on = nvn_plat_flag("nvk_buf_blocks", 0) != 0;
    s->nblocks = ((s->page0 / ps & 63u) + s->npages + 63u) / 64u;
    if (!mirror_make(s, 0)) { s->failed = 1; nvn_hm_put(&g_shadow, &k, s); nvn_unimpl("gpu", "buf_create_pool_mirror", mem); return NULL; }
    nvn_hm_put(&g_shadow, &k, s);
    return s;
}
// [off, off + len) 这一段该用哪一份镜像：显卡写过、还没回写落地的段只在写它的那一份里有；其余用当前这一份。
// 顺带记下「这次提交用到了它」。第二份建不出来就退回第一份（显卡还在用它时按序拷，结果照样对）。
static struct mirror *mirror_pick(struct shadow *s, uint64_t off, uint64_t len, int *mi) {
    int i = g_mirror_cur;
    for (int k = 0; k < s->ngw; k++) if (s->gw[k].off < off + len && off < s->gw[k].off + s->gw[k].len) { i = s->gw[k].mi; break; }
    if (!s->m[i].buf && !mirror_make(s, i)) i = 0;
    struct mirror *m = &s->m[i]; const uint64_t sub = rp_sub_serial();
    if (m->use_sub != sub) { m->prev_fence = m->use_fence; m->use_sub = sub; m->use_fence = rp_sub_fence(); }
    if (mi) *mi = i;
    return m;
}

// 这一段受不受写跟踪：不受跟踪的池每次使用都要整段同步，解析结果不能缓存（真机上的硬约束，见 rp_buf_resolve 注释）
void rp_buf_frame_flip(void) { pthread_mutex_lock(&g_bmu); g_mirror_cur ^= 1; pthread_mutex_unlock(&g_bmu); }

// 把游戏内存的 [a, b)（池内偏移）传进镜像，绕开 GPU 写过、还没回写落地的那些段：
// 那些字节镜像里的才是新的，游戏内存里是旧的（同步按页，一页里别的字节脏了不能连它一起盖回去）。
static void sync_write(nvn_gpu_cmd xc, uint8_t *direct, struct shadow *s, struct mirror *m, uint64_t a, uint64_t b) {   // direct 非空：直接写进镜像的 CPU 映射（见 rp_buf_resolve）
    while (a < b) {
        uint64_t stop = b, skip_to = 0;   // 找起点落在 [a, b) 里最靠前的一段；a 本身在某段里就先跳过去
        int inside = 0;
        for (int i = 0; i < s->ngw; i++) { uint64_t g0 = s->gw[i].off, g1 = g0 + s->gw[i].len;
            if (g0 <= a && a < g1) { inside = 1; if (g1 > skip_to) skip_to = g1; }
            else if (g0 > a && g0 < stop) stop = g0; }
        if (inside) { a = skip_to; continue; }
        if (direct) memcpy(direct + a, s->mem + a, (size_t)(stop - a));
        else nvn_gpu_buf_write(xc, m->buf, a, s->mem + a, stop - a);
        a = stop;
    }
}

// GPU 往池里写（计算着色器写存储缓冲）。提交线程登记这一段，返回登记号；段完成时 rp_buf_gpu_write_land 把结果拷回游戏内存、撤掉登记。
// pool_key：这段所在池的起点（落地时凭它找镜像：池已经销毁就什么都不做）。0 = 地址不在登记过的池里。
uint64_t rp_buf_gpu_write_note(uint64_t addr, uint64_t len, uint64_t *pool_key) {
    static uint64_t next_id;
    const uint8_t *mem; uint64_t size;
    if (!len || !rp_pool_find(addr, len, &mem, &size)) return 0;
    pthread_mutex_lock(&g_bmu);
    struct shadow *s = shadow_get(mem, size); uint64_t id = 0;
    if (s) {
        if (s->ngw == s->cgw) { int nc = s->cgw ? s->cgw * 2 : 8; struct gpu_write *n = realloc(s->gw, (size_t)nc * sizeof *n); if (n) { s->gw = n; s->cgw = nc; } }
        if (s->ngw < s->cgw) { int mi = 0; mirror_pick(s, addr - (uint64_t)(uintptr_t)mem, len, &mi);   // 和取这段缓冲时（rp_buf_resolve）挑的是同一份
            id = ++next_id; s->gw[s->ngw++] = (struct gpu_write){addr - (uint64_t)(uintptr_t)mem, len, id, (uint8_t)mi}; *pool_key = (uint64_t)(uintptr_t)mem; }
    }
    pthread_mutex_unlock(&g_bmu);
    return id;
}
void rp_buf_gpu_write_land(uint64_t pool_key, uint64_t id, int ok) {   // 线程：[完成回调]
    // 锁内只做「摘登记 + 记一笔正在拷」，13 MB 的拷贝放到锁外做：这一段原来持锁拷贝，
    // 与提交线程的缓冲解析抢同一把锁，重叠档实测「锁等待」11.1–21.2 毫秒/帧（见坑记录续 25）。
    const uint8_t *src = NULL, *mem = NULL; uint64_t off = 0, len = 0; struct shadow *s = NULL;
    pthread_mutex_lock(&g_bmu);
    s = nvn_hm_get(&g_shadow, &pool_key);
    if (s) for (int i = 0; i < s->ngw; i++) if (s->gw[i].id == id) {
        src = ok ? nvn_gpu_buf_cpu(s->m[s->gw[i].mi].buf) : NULL;
        mem = s->mem; off = s->gw[i].off; len = s->gw[i].len;
        rp_perf_add(RP_PERF_LAND_N, 1); rp_perf_add(RP_PERF_LAND_KB, len / 1024);
        s->gw[i] = s->gw[--s->ngw];
        s->landing++;
        if (s->nlr < 16) s->lr[s->nlr++] = (struct land_rng){off, len};   // 锁外拷贝期间要避开的区间
        break; }
    pthread_mutex_unlock(&g_bmu);
    if (!s) return;
    if (src) memcpy((uint8_t *)(uintptr_t)(mem + off), src + off, (size_t)len);   // 游戏内存是真值：GPU 的结果回到它那里
    pthread_mutex_lock(&g_bmu);
    for (int i = s->nlr - 1; i >= 0; i--) if (s->lr[i].off == off && s->lr[i].len == len) { s->lr[i] = s->lr[--s->nlr]; break; }
    s->landing--;
    if (s->landing <= 0 && s->dead) shadow_free(s);   // 拷贝期间池被销毁：现在收尾
    pthread_mutex_unlock(&g_bmu);
}

// 锁内版：调用方已持有 g_bmu，mem/size 是调用方查好的池。一笔绘制常要解析 3~4 段（顶点流 + 存储缓冲），
// 逐段各抢一次全局锁时「找池取镜像」在重叠档读到 13.7–16.6 毫秒/帧（5627 次、每次约 2.4 微秒）——
// 大头是这把锁与完成线程（落地 GPU 写）互抢，所以另给批量入口，一笔绘制只抢一次。
static int g_res_tm = -1; static unsigned g_res_tmk;
static uint64_t res_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static int resolve_part_locked(uint64_t addr, uint64_t len, const uint8_t *mem, uint64_t size,
                               struct shadow *s, struct mirror *m, int count_resolve) {
    if (g_res_tm < 0 || !(++g_res_tmk & 255)) g_res_tm = nvn_plat_flag("nvk_res_tm", 0) != 0;
    const uint64_t q0 = g_res_tm ? res_now() : 0;
    if (count_resolve) rp_perf_add(RP_PERF_RES_N, 1);
    if (!s) s = shadow_get(mem, size);
    if (!s) return 0;
    if (!m) m = mirror_pick(s, addr - (uint64_t)(uintptr_t)mem, len, NULL);
    size_t ps = nvn_dirty_page_size();
    uint64_t p = ((uintptr_t)addr - s->page0) / ps, q = ((uintptr_t)addr + len - 1 - s->page0) / ps, n = q - p + 1;
    uint64_t now0 = nvn_dirty_now();
    uint64_t map0 = nvn_dirty_mapping_generation();   // 跟踪的区间有增减 / 换底：代际变，快路径作废
    if (m->seen_ok && m->seen_map == map0 && !nvn_dirty_tracking_unreliable()) {
        // 快路径：要用的每一页都在「全局写代号 = 现在这个值」时核对过（核对过 = 当时是干净的或当场同步了），之后没有任何写。
        // 必须按页记：只记「镜像上次核对时的代号」不够——上次核对的是别的页，这几页可能在那之前就脏了（11-11 真机：镜头一动几何全碎）
        int all = 1; for (uint64_t i = p; i <= q; i++) if (!m->synced[i] || m->checked[i] != now0) { all = 0; break; }
        if (all) { rp_perf_add(RP_PERF_BUF_FAST_N, 1); if (q0) rp_perf_add(RP_PERF_RES_FIND_NS, res_now() - q0); return 1; }
    }
    uint64_t stack[256], *gens = n <= 256 ? stack : malloc((size_t)n * sizeof *gens);
    long got = gens ? nvn_dirty_page_gens(nvn_dirty_span((const void *)(uintptr_t)addr, (size_t)len), (const void *)(uintptr_t)addr, (size_t)len, gens, (size_t)n) : -1;
    int tracked = got == (long)n;   // 没被写跟踪的池：每次使用都整段同步
    if (tracked) { rp_perf_add(RP_PERF_BUF_SLOW_N, 1); rp_perf_add(RP_PERF_BUF_PAGES_N, n); } else rp_perf_add(RP_PERF_BUF_NOSPAN_N, 1);
    for (uint64_t i = p; i <= q;) {
        if (m->synced[i] && tracked && gens[i - p] <= m->synced[i]) { i++; continue; }
        uint64_t j = i; while (j + 1 <= q && !(m->synced[j + 1] && tracked && gens[j + 1 - p] <= m->synced[j + 1])) j++;
        uintptr_t a = s->page0 + (uintptr_t)(i * ps), b = s->page0 + (uintptr_t)((j + 1) * ps);   // 这一串脏页，裁到池的范围
        if (a < (uintptr_t)mem) a = (uintptr_t)mem;
        if (b > (uintptr_t)mem + size) b = (uintptr_t)mem + size;
        nvn_dirty_protect((const void *)a, b - a); uint64_t st = nvn_dirty_now(); if (!st) st = 1;   // 先保护、取代号、再读
        uint64_t last = 0; for (uint64_t k = i; k <= j; k++) if (m->sub[k] > last) last = m->sub[k];
        // 本次提交里第一次同步这一串：镜像是 CPU 也能写的共享缓冲，直接写进去，不录成 GPU 上的拷贝（理由与真机读数见坑记录）。
        // 正在拷回的段数多于登记数（表满 16 段没记上）→ 说不清哪段在拷，按原口径整池退回录拷贝
        int landing_hit = s->landing > s->nlr;   // 这一段里有没有正在锁外拷回的区间与 [a,b) 相交：有就只能退回录拷贝
        if (!landing_hit && s->nlr) { const uint64_t ra = a - (uintptr_t)mem, rb = b - (uintptr_t)mem;
            for (int k = 0; k < s->nlr; k++) if (s->lr[k].off < rb && ra < s->lr[k].off + s->lr[k].len) { landing_hit = 1; break; } }
        uint8_t *d = last != rp_sub_serial() && rp_fence_done(m->prev_fence) && !landing_hit ? nvn_gpu_buf_cpu(m->buf) : NULL;
        if (d) { sync_write(NULL, d, s, m, a - (uintptr_t)mem, b - (uintptr_t)mem); last = rp_sub_serial(); rp_perf_add(RP_PERF_BUF_SYNC_DIRECT_KB, (b - a) / 1024); }
        else { nvn_gpu_cmd xc = rp_xfer_cmd(&last); if (xc) sync_write(xc, NULL, s, m, a - (uintptr_t)mem, b - (uintptr_t)mem); }
        for (uint64_t k = i; k <= j; k++) { m->synced[k] = st; m->sub[k] = last; }
        g_sync_bytes += b - a; g_sync_runs++; rp_perf_add(RP_PERF_BUF_SYNC_KB, (b - a) / 1024);
        i = j + 1;
    }
    if (gens != stack) free(gens);
    m->seen_ok = (uint8_t)tracked; m->seen_map = map0;
    for (uint64_t i = p; i <= q; i++) m->checked[i] = now0;   // now0 取在读各页代号之前：这之后才写的页，代号已经不等于 now0
    if (q0) rp_perf_add(RP_PERF_RES_LA_NS, res_now() - q0);
    return 1;
}
static int resolve_locked(uint64_t addr, uint64_t len, const uint8_t *mem, uint64_t size) {
    struct shadow *s = shadow_get(mem, size);
    if (!s) return 0;
    // 整段先选一次镜像：未落地的 GPU 写可能只覆盖其中一个摘要块。
    struct mirror *m = mirror_pick(s, addr - (uint64_t)(uintptr_t)mem, len, NULL);
    size_t ps = nvn_dirty_page_size();
    uint64_t p = ((uintptr_t)addr - s->page0) / ps, q = ((uintptr_t)addr + len - 1 - s->page0) / ps;
    if (!m->blocks || q - p + 1 <= 64)
        return resolve_part_locked(addr, len, mem, size, s, m, 1);
    const void *span = nvn_dirty_span((const void *)(uintptr_t)addr, (size_t)len);
    if (!span) return resolve_part_locked(addr, len, mem, size, s, m, 1);
    rp_perf_add(RP_PERF_RES_N, 1);
    const uint64_t base_page = s->page0 / ps;
    int all_fast = 1; uint64_t skipped = 0;
    for (uint64_t i = p; i <= q;) {
        uint64_t first_bit = (base_page + i) & 63u;
        uint64_t j = i + (63u - first_bit); if (j > q) j = q;
        uint64_t idx = (base_page + i) / 64u - base_page / 64u;
        uintptr_t a = s->page0 + (uintptr_t)(i * ps), b = s->page0 + (uintptr_t)((j + 1) * ps);
        if (a < (uintptr_t)mem) a = (uintptr_t)mem;
        if (b > (uintptr_t)mem + size) b = (uintptr_t)mem + size;
        uint64_t version, epoch;
        if (idx >= s->nblocks || !nvn_dirty_block_snapshot(span, (const void *)a, b - a, &version, &epoch))
            return resolve_part_locked(addr, len, mem, size, s, m, 0);
        struct mirror_block *block = &m->blocks[idx];
        uint64_t count = j - i + 1;
        uint64_t mask = count == 64 ? UINT64_MAX : ((UINT64_C(1) << count) - 1) << first_bit;
        if (block->epoch != epoch) {
            // 增删/换底可能令旧的逐页 synced 也失去意义；仅清本次用到的页，保留 sub 顺序。
            for (uint64_t k = i; k <= j; k++) m->synced[k] = 0;
            block->epoch = epoch; block->version = version; block->covered = 0;
        } else if (block->version != version) { block->version = version; block->covered = 0; }
        if ((block->covered & mask) != mask) {
            all_fast = 0;
            if (!resolve_part_locked((uint64_t)a, b - a, mem, size, s, m, 0)) return 0;
            uint64_t v1, e1;
            if (m->seen_ok && nvn_dirty_block_snapshot(span, (const void *)a, b - a, &v1, &e1) &&
                v1 == version && e1 == epoch) block->covered |= mask;
            else block->covered = 0;
        } else skipped++;
        i = j + 1;
    }
    if (all_fast) rp_perf_add(RP_PERF_BUF_FAST_N, 1);
    if (skipped) rp_perf_add(RP_PERF_BUF_BLOCK_SKIP_N, skipped);
    return 1;
}
int rp_buf_resolve(nvn_gpu_cmd c, uint64_t addr, uint64_t len, nvn_gpu_buf *buf, uint64_t *off) {
    (void)c;
    const uint8_t *mem; uint64_t size;
    if (!len || !rp_pool_find(addr, len, &mem, &size)) { nvn_unimpl("buf", "address_outside_pools", (const void *)(uintptr_t)addr); return 0; }
    pthread_mutex_lock(&g_bmu);
    int r = resolve_locked(addr, len, mem, size);
    if (r) {
        struct shadow *s = shadow_get(mem, size);
        if (s) {
            struct mirror *m = mirror_pick(s, addr - (uint64_t)(uintptr_t)mem, len, NULL);
            if (buf) *buf = m->buf;
            if (off) *off = addr - (uint64_t)(uintptr_t)mem;
        } else r = 0;
    }
    pthread_mutex_unlock(&g_bmu);
    return r;
}
// 批量：一次抢锁解析多段（顶点流 / 存储缓冲），成功写 1、失败写 0。
static void resolve_multi_chunk(int n, const uint64_t *addr, const uint64_t *len, nvn_gpu_buf *buf, uint64_t *off, unsigned char *ok) {
    const uint8_t *mem[16]; uint64_t size[16];
    for (int i = 0; i < n; i++) { mem[i] = NULL; size[i] = 0; ok[i] = 0; if (len[i]) rp_pool_find(addr[i], len[i], &mem[i], &size[i]); }
    const uint64_t w0 = (g_res_tm && n) ? res_now() : 0;
    pthread_mutex_lock(&g_bmu);
    if (w0) rp_perf_add(RP_PERF_RES_LW_NS, res_now() - w0);
    for (int i = 0; i < n; i++) {
        if (!mem[i]) continue;
        struct shadow *s = shadow_get(mem[i], size[i]);
        if (!s || !resolve_locked(addr[i], len[i], mem[i], size[i])) continue;
        struct mirror *m = mirror_pick(s, addr[i] - (uint64_t)(uintptr_t)mem[i], len[i], NULL);
        buf[i] = m->buf; off[i] = addr[i] - (uint64_t)(uintptr_t)mem[i]; ok[i] = 1;
    }
    pthread_mutex_unlock(&g_bmu);
}
int rp_buf_resolve_multi(int n, const uint64_t *addr, const uint64_t *len, nvn_gpu_buf *buf, uint64_t *off, unsigned char *ok) {
    // Storage bindings can exceed the 16 vertex streams. Resolve every item;
    // the fixed scratch arrays bound each lock acquisition, not the request.
    while (n > 0) {
        int count = n < 16 ? n : 16;
        resolve_multi_chunk(count, addr, len, buf, off, ok);
        n -= count; addr += count; len += count; buf += count; off += count; ok += count;
    }
    return 1;
}

void rp_buf_pool_gone(const void *mem) {
    uint64_t k = (uint64_t)(uintptr_t)mem;
    pthread_mutex_lock(&g_bmu);
    struct shadow *s = nvn_hm_del(&g_shadow, &k);
    if (s) { if (s->landing > 0) s->dead = 1; else shadow_free(s); }   // 有拷贝在锁外进行：由收尾那边释放（见 rp_buf_gpu_write_land）
    pthread_mutex_unlock(&g_bmu);
}
nvn_gpu_buf rp_buf_for_null_storage(void) {
    pthread_mutex_lock(&g_bmu);
    if (!g_null_storage) {
        // 对齐 Ryujinx DescriptorSetUpdater：不支持 nullDescriptor 时，未使用的缓冲绑定用 64 KiB 物理对象。
        // 只初始化一次；着色器写入后不承诺内容仍为零，也不把写入落到任何 guest 地址。
        void *zero = calloc(1, 0x10000);
        nvn_gpu_err err = {0};
        if (!zero || !nvn_gpu_ok(nvn_gpu_buf_create(0x10000, NVN_GPU_MEM_SHARED, zero, &g_null_storage, &err))) {
            g_null_storage = NULL;
            if (nvn_unimpl("gpu", "null_storage_create", NULL) == 1 && err.why[0]) nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", err.why);
        }
        free(zero);
    }
    nvn_gpu_buf result = g_null_storage;
    pthread_mutex_unlock(&g_bmu); return result;
}

void rp_buf_reset(void) {
    pthread_mutex_lock(&g_bmu);
    for (uint32_t i = 0; i < g_shadow.cap; i++) { struct shadow *s = g_shadow.vals[i]; if (s) shadow_free(s); }
    if (g_null_storage) nvn_gpu_buf_release(g_null_storage, NULL, NULL);
    g_null_storage = NULL;
    free(g_shadow.keys); free(g_shadow.vals); nvn_hm_init(&g_shadow, 8); g_mirror_cur = 0;
    pthread_mutex_unlock(&g_bmu);
}
void rp_buf_stats(uint64_t *bytes, uint64_t *runs) { pthread_mutex_lock(&g_bmu); if (bytes) *bytes = g_sync_bytes; if (runs) *runs = g_sync_runs; pthread_mutex_unlock(&g_bmu); }
int rp_buf_mirrors(void) { pthread_mutex_lock(&g_bmu); int n = (int)g_shadow.n; pthread_mutex_unlock(&g_bmu); return n; }

// ---------- 缓冲类命令：只改游戏内存（见文件头）----------
static int in_pool(uint64_t a, uint64_t n) { return n && rp_pool_find(a, n, NULL, NULL); }
// 回放时在 CPU 上改游戏缓冲内容的三个命令：改一次代号加一（rp_draw 的段内常量缓冲切片按它失效）
uint64_t rp_cpu_write_gen;
// 写入区间日志（开关 nvk_slc_gen 用）：只记这三个命令改写过的 [a, b)，给「暂存片还能不能用」逐区间判定。
// 用全局写代号判会让所有区间一起作废；按区间判的话，每帧固定的那几块常量缓冲（内容没变）就能一直用同一片暂存。
enum { WL_N = 128 };
static struct wl_ent { uint64_t seq, a, b; } g_wl[WL_N];
static uint64_t g_wl_seq, g_wl_put;
uint64_t rp_write_mark(void) { return g_wl_seq; }
void rp_write_note(uint64_t a, uint64_t n) { g_wl[g_wl_put].seq = ++g_wl_seq; g_wl[g_wl_put].a = a; g_wl[g_wl_put].b = a + n; g_wl_put = (g_wl_put + 1) % WL_N; }
// mark 之后有没有人写过和 [a, a+n) 重叠的区间。只顺着记号扫「mark 之后的那几条」——
// 一帧要问 8000 次左右，扫满 128 条就是一百万次比较（实测这样做的缓存反而更慢）。
// 日志绕一圈（mark 太旧、记录已被覆盖）时保守当作写过。
int rp_write_touched(uint64_t mark, uint64_t a, uint64_t n) {
    const uint64_t seq = __atomic_load_n(&g_wl_seq, __ATOMIC_RELAXED);
    if (mark == seq) return 0;
    if (seq - mark > WL_N) return 1;
    const uint64_t b = a + n;
    for (uint64_t s = mark + 1; s <= seq; s++) {   // 第 s 次写入放在下标 (s-1)%WL_N（rp_write_note 的写法）
        const struct wl_ent *w = &g_wl[(s - 1) % WL_N];
        if (w->seq == s && w->a < b && a < w->b) return 1;
    }
    return 0;
}
void rp_op_update_ub(const void *a, const void *rec) {
    uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8);
    if (!n) return;
    if (!in_pool(d, n)) { nvn_unimpl("buf", "update_outside_pools", rec); return; }
    memcpy((void *)(uintptr_t)d, (const uint8_t *)a + 16, (size_t)n); rp_cpu_write_gen++; rp_write_note(d, n);
}
void rp_op_copy_b2b(const void *a, const void *rec) {
    uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, (const uint8_t *)a + 8, 8); memcpy(&n, (const uint8_t *)a + 16, 8);
    if (!n) return;
    if (!in_pool(s, n) || !in_pool(d, n)) { nvn_unimpl("buf", "copy_outside_pools", rec); return; }
    memmove((void *)(uintptr_t)d, (const void *)(uintptr_t)s, (size_t)n); rp_cpu_write_gen++; rp_write_note(d, n);
}
void rp_op_clear_buf(const p_clear_buf *cb, const void *rec) {   // 32 位样式填充（不是按字节）
    if (!cb->size) return;
    if (!in_pool(cb->addr, cb->size)) { nvn_unimpl("buf", "clear_outside_pools", rec); return; }
    uint8_t *p = (uint8_t *)(uintptr_t)cb->addr; uint64_t n = cb->size, i = 0; rp_cpu_write_gen++; rp_write_note(cb->addr, cb->size);
    for (; i + 4 <= n; i += 4) memcpy(p + i, &cb->value, 4);
    if (i < n) memcpy(p + i, &cb->value, (size_t)(n - i));
}

// OP_COPY_B2T：缓冲里是线性行（行距 / 片距可由游戏设），直接从游戏内存按当前段的 GPU 顺序传进贴图（nvn_mtl.m copy_b2t @13762）。
// 线性 / 线性渲染目标贴图走同一条路：宿主贴图不按游戏的内存布局建，拷贝只看区域、行距、片距（iOS copy_b2t 同样不分）
void rp_op_copy_b2t(nvn_gpu_cmd c, const p_copy_b2t *o, const void *rec) {
    const tex_t *t = o->tex; if (!t) return;
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb || o->r.width <= 0 || o->r.height <= 0) { nvn_unimpl("tex", "b2t_bad_format_or_region", rec); return; }
    uint32_t level = o->has_view && o->view.set_levels ? o->view.base_level : 0, layer0 = o->has_view && o->view.set_layers ? o->view.min_layer : 0;
    uint64_t row = o->row_stride ? o->row_stride : (uint64_t)(((uint32_t)o->r.width + (uint32_t)bw - 1) / (uint32_t)bw) * (uint64_t)bpb;
    uint64_t rows = ((uint32_t)o->r.height + (uint32_t)bh - 1) / (uint32_t)bh, img = o->image_stride ? o->image_stride : row * rows;
    uint32_t nz = o->r.depth > 0 ? (uint32_t)o->r.depth : 1;
    if (!in_pool(o->src, img * nz)) { nvn_unimpl("buf", "b2t_source_outside_pools", rec); return; }
    nvn_gpu_tex g = rp_tex_resolve(t, c);
    if (!g || !c) return;
    if (t->d.target == NVN_TEXTURE_TARGET_3D && nvn_plat_flag("nvk_drawlog", 0)) {   // 调试开关下：三维贴图的每次上传打一行
        char b[200]; snprintf(b, sizeof b, "三维贴图上传 #%llu 格式 %#x 尺寸 %dx%dx%d 级 %u：区域 %d,%d,%d %dx%dx%d 行距 %llu 片距 %llu（记录里 行 %llu 片 %llu）块 %dx%d 每块 %d 字节",
                             (unsigned long long)t->serial, (unsigned)t->d.format, t->d.width, t->d.height, t->d.depth, level, o->r.xoffset, o->r.yoffset, o->r.zoffset, o->r.width, o->r.height, o->r.depth,
                             (unsigned long long)row, (unsigned long long)img, (unsigned long long)o->row_stride, (unsigned long long)o->image_stride, bw, bh, bpb);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "tex", b);
    }
    for (uint32_t z = 0; z < nz; z++) {
        NVNcopyRegion r = o->r; r.depth = 1;
        // 第几层 / 第几片都经 layer 交给后端（三维贴图的 layer 就是深度方向的片号；视图的起始层只对分层贴图有意义）
        uint32_t layer = (uint32_t)o->r.zoffset + z + (t->d.target == NVN_TEXTURE_TARGET_3D ? 0 : layer0);
        r.zoffset = 0;
        nvn_gpu_tex_upload(c, g, level, layer, &r, (const void *)(uintptr_t)(o->src + z * img), row, img);
    }
    rp_tex_mark_gpu_written(t);   // 这张贴图的内容从此不再从它自己的存储重传
}

// 贴图 → 贴图的拷贝（nvn_mtl.m copy_t2t_ @14049）。源 / 目标各按自己视图的级与起始层；同尺寸、同格式、不镜像的逐层直接拷（三维的一次拷若干片）；
// 降采样记录（DM_COPY_RESOLVE）在两边采样数、格式、尺寸都相同时就是整张拷贝。缩放、换格式、镜像、多重采样解析：iOS 是画出来 / 解析出来的，这里还没接，分开记账。
void rp_op_copy_t2t(nvn_gpu_cmd c, const p_copy_t2t *o, const void *rec) {
    const tex_t *s = o->src, *d = o->dst; if (!s || !d || !c) return;
    uint32_t sl = o->has_sv && o->sv.set_levels ? o->sv.base_level : 0, dl = o->has_dv && o->dv.set_levels ? o->dv.base_level : 0;
    uint32_t sz0 = o->has_sv && o->sv.set_layers ? o->sv.min_layer : 0, dz0 = o->has_dv && o->dv.set_layers ? o->dv.min_layer : 0;
    int sfmt = o->has_sv && o->sv.set_format ? o->sv.format : s->d.format, dfmt = o->has_dv && o->dv.set_format ? o->dv.format : d->d.format;
    int s3 = s->d.target == NVN_TEXTURE_TARGET_3D, d3 = d->d.target == NVN_TEXTURE_TARGET_3D;
    NVNcopyRegion sr = o->sr, dr = o->dr;
    if (nvn_plat_flag("nvk_drawlog", 0)) { static int said; if (said < 8) { said++; char b[260];
        snprintf(b, sizeof b, "贴图拷贝 #%llu(%#x %dx%d 采样 %d 级 %u 层 %u) %d,%d %dx%dx%d → #%llu(%#x %dx%d 采样 %d 级 %u 层 %u) %d,%d %dx%dx%d 标志 %#x", (unsigned long long)s->serial, (unsigned)sfmt, s->d.width, s->d.height, s->d.samples, sl, sz0,
                 sr.xoffset, sr.yoffset, sr.width, sr.height, sr.depth, (unsigned long long)d->serial, (unsigned)dfmt, d->d.width, d->d.height, d->d.samples, dl, dz0, dr.xoffset, dr.yoffset, dr.width, dr.height, dr.depth, (unsigned)o->flags);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "copy", b); } }
    if (o->flags & DM_COPY_RESOLVE) {
        int ss = s->d.samples > 1 ? s->d.samples : 1, ds = d->d.samples > 1 ? d->d.samples : 1;
        if (ss > 1 && ds == 1 && sfmt == dfmt && sfmt == s->d.format && dfmt == d->d.format &&
            s->d.width == d->d.width && s->d.height == d->d.height && !s3 && !d3 && sl == 0 && dl == 0) {
            nvn_fmt_info f;
            if (!nvn_fmt_info_of((uint32_t)sfmt, &f) || f.kind == NVN_FMT_UINT || f.kind == NVN_FMT_SINT || f.depth || f.stencil) {
                nvn_unimpl("tex", "t2t_multisample_resolve", rec); return;
            }
            view_t vs = {0}, vd = {0};
            vs.set_levels = vd.set_levels = 1; vs.nlevels = vd.nlevels = 1;
            vs.set_layers = vd.set_layers = 1; vs.min_layer = (int)sz0; vd.min_layer = (int)dz0; vs.nlayers = vd.nlayers = 1;
            rp_view_pin_begin();   // 连取两个视图：取第二个时不能把第一个轮转放掉
            nvn_gpu_view gv = rp_view_for(s, &vs, c), hv = rp_view_for(d, &vd, c);
            if (!gv || !hv) { rp_view_pin_end(); return; }
            NVNcopyRegion r = {0, 0, 0, s->d.width, s->d.height, 1};
            nvn_gpu_blit(c, gv, &r, hv, &r, 0);
            rp_view_pin_end();
            rp_tex_mark_gpu_written(d);
            return;
        }
        if (ss != ds || sfmt != dfmt || s->d.width != d->d.width || s->d.height != d->d.height) { nvn_unimpl("tex", ss > 1 && ds == 1 ? "t2t_multisample_resolve" : "t2t_downsample_case", rec); return; }
        uint32_t w = (uint32_t)s->d.width >> sl, h = (uint32_t)s->d.height >> sl; if (!w) w = 1; if (!h) h = 1;
        sr = (NVNcopyRegion){0, 0, 0, (int)w, (int)h, 1}; dr = sr;
    } else {
        int nz = sr.depth > 0 ? sr.depth : 1, dnz = dr.depth > 0 ? dr.depth : 1;
        int mirror = o->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT);
        if ((o->flags & NVN_COPY_FLAGS_MIRROR_Z_BIT) || (mirror && s->serial == d->serial)) { nvn_unimpl("tex", "t2t_mirror", rec); return; }
        if (mirror || sr.width != dr.width || sr.height != dr.height || sfmt != dfmt) {
            // 缩放 / 换格式：二维、单层、单采样。颜色走原生 blit；同格式 D24S8/D32FS8 最近点缩放由后端能力位声明。
            nvn_fmt_info fa, fb; int ia = !nvn_fmt_info_of((uint32_t)sfmt, &fa) || fa.kind == NVN_FMT_UINT || fa.kind == NVN_FMT_SINT || fa.depth || fa.stencil;
            int ib = !nvn_fmt_info_of((uint32_t)dfmt, &fb) || fb.kind == NVN_FMT_UINT || fb.kind == NVN_FMT_SINT || fb.depth || fb.stencil;
            int depth_pair = sfmt == dfmt && sfmt == s->d.format && dfmt == d->d.format &&
                (sfmt == NVN_FORMAT_DEPTH24_STENCIL8 || sfmt == NVN_FORMAT_DEPTH32F_STENCIL8) && !mirror && !(o->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT);
            if (s3 || d3 || nz != 1 || dnz != 1 || s->d.samples > 1 || d->d.samples > 1 || ((ia || ib) && !depth_pair)) { nvn_unimpl("tex", mirror ? "t2t_mirror" : sfmt != dfmt ? "t2t_format_change" : "t2t_scaled", rec); return; }
            nvn_gpu_caps caps; nvn_gpu_caps_get(&caps);
            if (depth_pair ? !(caps.caps & NVN_GPU_CAP_BLIT_DEPTH_STENCIL) : !(caps.caps & NVN_GPU_CAP_BLIT_SCALED)) {
                nvn_unimpl("tex", mirror ? "t2t_mirror" : depth_pair ? "t2t_scaled" : "t2t_scaled_no_native_blit", rec); return;
            }
            // NVN 区域仍为正尺寸；先防止反向端点的有符号加法溢出。
            if (mirror && (sr.width <= 0 || sr.height <= 0 || dr.width <= 0 || dr.height <= 0 ||
                sr.xoffset < 0 || sr.yoffset < 0 || (int64_t)sr.xoffset + sr.width > INT32_MAX ||
                (int64_t)sr.yoffset + sr.height > INT32_MAX)) { nvn_unimpl("tex", "t2t_mirror", rec); return; }
            // 视图只取级与层：拷贝按存储的格式做
            view_t vs, vd; memset(&vs, 0, sizeof vs); memset(&vd, 0, sizeof vd);
            vs.set_levels = 1; vs.base_level = (int)sl; vs.nlevels = 1; vs.set_layers = 1; vs.min_layer = (int)(sz0 + (uint32_t)sr.zoffset); vs.nlayers = 1;
            vd.set_levels = 1; vd.base_level = (int)dl; vd.nlevels = 1; vd.set_layers = 1; vd.min_layer = (int)(dz0 + (uint32_t)dr.zoffset); vd.nlayers = 1;
            // 一次取源 / 目标两个视图再 blit：同一段取用窗口，第二个不许把第一个轮转淘汰（#699）。
            // 两笔连续取用本身最多推进一次轮转，但要和 pass 内多附件守同一条不变量。
            rp_view_pin_begin();
            nvn_gpu_view gv = rp_view_for(s, &vs, c), hv = rp_view_for(d, &vd, c);
            if (!gv || !hv) { rp_view_pin_end(); return; }
            NVNcopyRegion a = sr, b = dr; a.zoffset = b.zoffset = 0; a.depth = b.depth = 1;
            // Ryujinx ExtentsToOffset3D 同样保留两端次序：镜像沿源矩形反向行走。
            if (mirror & NVN_COPY_FLAGS_MIRROR_X_BIT) { a.xoffset += a.width; a.width = -a.width; }
            if (mirror & NVN_COPY_FLAGS_MIRROR_Y_BIT) { a.yoffset += a.height; a.height = -a.height; }
            nvn_gpu_blit(c, gv, &a, hv, &b, (o->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) != 0);
            rp_view_pin_end();
            rp_tex_mark_gpu_written(d);
            return;
        }
        if (nz != dnz) { nvn_unimpl("tex", "t2t_scaled", rec); return; }
        if (s3 != d3) { nvn_unimpl("tex", "t2t_3d_mismatch", rec); return; }
    }
    if (sr.width <= 0 || sr.height <= 0) return;
    nvn_gpu_tex gs = rp_tex_resolve(s, c), gd = rp_tex_resolve(d, c);
    if (!gs || !gd) return;
    int nz = sr.depth > 0 ? sr.depth : 1;
    if (s3) nvn_gpu_copy_tex(c, gs, sl, 0, &sr, gd, dl, 0, &dr);   // 三维：片在区域的深度方向上，一次拷完
    else for (int z = 0; z < nz; z++) {
        NVNcopyRegion a = sr, b = dr; a.zoffset = b.zoffset = 0; a.depth = b.depth = 1;
        nvn_gpu_copy_tex(c, gs, sl, sz0 + (uint32_t)sr.zoffset + (uint32_t)z, &a, gd, dl, dz0 + (uint32_t)dr.zoffset + (uint32_t)z, &b);
    }
    rp_tex_mark_gpu_written(d);   // 目标的内容从此以 GPU 为准
}
