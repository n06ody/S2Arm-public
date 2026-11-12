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

struct gpu_write { uint64_t off, len, id; };   // GPU 写进镜像、还没回写到游戏内存的一段
struct shadow { const uint8_t *mem; uint64_t size; nvn_gpu_buf buf; uint64_t *synced, *sub, *checked; uint64_t npages; uintptr_t page0; struct gpu_write *gw; int ngw, cgw;
    uint64_t seen_map; uint8_t seen_ok; };   // checked[i]：第 i 页上次逐页核对时的全局写代号。代号没动 = 那之后游戏没写过任何被跟踪的页，这一页不用再核   // synced[i]：第 i 页上次同步取的脏页代号，0 = 从没同步
static pthread_mutex_t g_bmu = PTHREAD_MUTEX_INITIALIZER;
static nvn_hmap g_shadow = {.klen = 8};   // 池起点 → struct shadow *
static uint64_t g_sync_bytes, g_sync_runs;

int rp_pool_find(uint64_t addr, uint64_t len, const uint8_t **mem, uint64_t *size);   // rp_entry.c

static struct shadow *shadow_get(const uint8_t *mem, uint64_t size) {
    uint64_t k = (uint64_t)(uintptr_t)mem;
    struct shadow *s = nvn_hm_get(&g_shadow, &k);
    if (s) return s;
    nvn_gpu_buf b = NULL; nvn_gpu_err err = {0};
    if (!nvn_gpu_ok(nvn_gpu_buf_create(size, NVN_GPU_MEM_SHARED, NULL, &b, &err))) { nvn_unimpl("gpu", "buf_create_pool_mirror", mem); return NULL; }
    size_t ps = nvn_dirty_page_size();
    s = calloc(1, sizeof *s);
    if (!s) { nvn_gpu_buf_release(b, NULL, NULL); nvn_unimpl("gpu", "pool_mirror_alloc_failed", mem); return NULL; }
    s->mem = mem; s->size = size; s->buf = b;
    s->page0 = (uintptr_t)mem & ~(uintptr_t)(ps - 1);
    s->npages = ((uintptr_t)mem + size - s->page0 + ps - 1) / ps;
    s->synced = calloc(s->npages ? s->npages : 1, sizeof *s->synced);
    s->sub = calloc(s->npages ? s->npages : 1, sizeof *s->sub);   // 各页上次同步所在的提交序号
    s->checked = calloc(s->npages ? s->npages : 1, sizeof *s->checked);
    if (!s->synced || !s->sub || !s->checked) {   // 页表没分出来：不把半张表放进镜像表（之后每次使用都会读它），这一笔按取不到缓冲处理
        free(s->synced); free(s->sub); free(s->checked); free(s); nvn_gpu_buf_release(b, NULL, NULL);
        nvn_unimpl("gpu", "pool_mirror_alloc_failed", mem); return NULL; }
    nvn_hm_put(&g_shadow, &k, s);
    return s;
}

// 把游戏内存的 [a, b)（池内偏移）传进镜像，绕开 GPU 写过、还没回写落地的那些段：
// 那些字节镜像里的才是新的，游戏内存里是旧的（同步按页，一页里别的字节脏了不能连它一起盖回去）。
static void sync_write(nvn_gpu_cmd xc, uint8_t *direct, struct shadow *s, uint64_t a, uint64_t b) {   // direct 非空：直接写进镜像的 CPU 映射（见 rp_buf_resolve）
    while (a < b) {
        uint64_t stop = b, skip_to = 0;   // 找起点落在 [a, b) 里最靠前的一段；a 本身在某段里就先跳过去
        int inside = 0;
        for (int i = 0; i < s->ngw; i++) { uint64_t g0 = s->gw[i].off, g1 = g0 + s->gw[i].len;
            if (g0 <= a && a < g1) { inside = 1; if (g1 > skip_to) skip_to = g1; }
            else if (g0 > a && g0 < stop) stop = g0; }
        if (inside) { a = skip_to; continue; }
        if (direct) memcpy(direct + a, s->mem + a, (size_t)(stop - a));
        else nvn_gpu_buf_write(xc, s->buf, a, s->mem + a, stop - a);
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
        if (s->ngw < s->cgw) { id = ++next_id; s->gw[s->ngw++] = (struct gpu_write){addr - (uint64_t)(uintptr_t)mem, len, id}; *pool_key = (uint64_t)(uintptr_t)mem; }
    }
    pthread_mutex_unlock(&g_bmu);
    return id;
}
void rp_buf_gpu_write_land(uint64_t pool_key, uint64_t id, int ok) {   // 线程：[完成回调]
    pthread_mutex_lock(&g_bmu);
    struct shadow *s = nvn_hm_get(&g_shadow, &pool_key);
    if (s) for (int i = 0; i < s->ngw; i++) if (s->gw[i].id == id) {
        const uint8_t *src = ok ? nvn_gpu_buf_cpu(s->buf) : NULL;
        if (src) memcpy((uint8_t *)(uintptr_t)(s->mem + s->gw[i].off), src + s->gw[i].off, (size_t)s->gw[i].len);   // 游戏内存是真值：GPU 的结果回到它那里
        s->gw[i] = s->gw[--s->ngw];
        break; }
    pthread_mutex_unlock(&g_bmu);
}

int rp_buf_resolve(nvn_gpu_cmd c, uint64_t addr, uint64_t len, nvn_gpu_buf *buf, uint64_t *off) {
    (void)c;   // 同步录进哪一段由 rp_xfer_cmd 定
    const uint8_t *mem; uint64_t size;
    if (!len || !rp_pool_find(addr, len, &mem, &size)) { nvn_unimpl("buf", "address_outside_pools", (const void *)(uintptr_t)addr); return 0; }
    pthread_mutex_lock(&g_bmu);
    struct shadow *s = shadow_get(mem, size);
    if (!s) { pthread_mutex_unlock(&g_bmu); return 0; }
    size_t ps = nvn_dirty_page_size();
    uint64_t p = ((uintptr_t)addr - s->page0) / ps, q = ((uintptr_t)addr + len - 1 - s->page0) / ps, n = q - p + 1;
    uint64_t now0 = nvn_dirty_now();
    uint64_t map0 = nvn_dirty_mapping_generation();   // 跟踪的区间有增减 / 换底：代际变，快路径作废
    if (s->seen_ok && s->seen_map == map0 && !nvn_dirty_tracking_unreliable()) {
        // 快路径：要用的每一页都在「全局写代号 = 现在这个值」时核对过（核对过 = 当时是干净的或当场同步了），之后没有任何写。
        // 必须按页记：只记「镜像上次核对时的代号」不够——上次核对的是别的页，这几页可能在那之前就脏了（11-11 真机：镜头一动几何全碎）
        int all = 1; for (uint64_t i = p; i <= q; i++) if (!s->synced[i] || s->checked[i] != now0) { all = 0; break; }
        if (all) { if (buf) *buf = s->buf; if (off) *off = addr - (uint64_t)(uintptr_t)mem; pthread_mutex_unlock(&g_bmu); return 1; }
    }
    uint64_t stack[256], *gens = n <= 256 ? stack : malloc((size_t)n * sizeof *gens);
    long got = gens ? nvn_dirty_page_gens(nvn_dirty_span((const void *)(uintptr_t)addr, (size_t)len), (const void *)(uintptr_t)addr, (size_t)len, gens, (size_t)n) : -1;
    int tracked = got == (long)n;   // 没被写跟踪的池：每次使用都整段同步
    for (uint64_t i = p; i <= q;) {
        if (s->synced[i] && tracked && gens[i - p] <= s->synced[i]) { i++; continue; }
        uint64_t j = i; while (j + 1 <= q && !(s->synced[j + 1] && tracked && gens[j + 1 - p] <= s->synced[j + 1])) j++;
        uintptr_t a = s->page0 + (uintptr_t)(i * ps), b = s->page0 + (uintptr_t)((j + 1) * ps);   // 这一串脏页，裁到池的范围
        if (a < (uintptr_t)mem) a = (uintptr_t)mem;
        if (b > (uintptr_t)mem + size) b = (uintptr_t)mem + size;
        nvn_dirty_protect((const void *)a, b - a); uint64_t st = nvn_dirty_now(); if (!st) st = 1;   // 先保护、取代号、再读
        // 这一串里只要有一页本次提交已同步过，整串就按「第二次同步」走主段
        uint64_t last = 0; for (uint64_t k = i; k <= j; k++) if (s->sub[k] > last) last = s->sub[k];
        // 本次提交里第一次同步这一串：镜像是 CPU 也能写的共享缓冲，直接写进去，不录成 GPU 上的拷贝。
        // 等价于真机（和 iOS 的零拷贝包内存）的语义——缓冲就是游戏内存，GPU 执行到哪一笔读到的就是当时的内容；
        // 游戏在提交前把这次提交要用的内容写好，这一条本来就成立（原先录进前置段，也是先于本次提交的全部通道）。
        // 真机读数（3D 那款场景，同一局里来回切）：每帧约 27 MB、100 次带全局屏障的 GPU 拷贝改成直写，帧率 11.0 → 12.9。
        // 同一次提交里已经同步过又变了的，必须排在已录的绘制之后：照旧结束通道、录进主段。镜像 CPU 读不到的后端也照旧。
        // 还要此前的提交都已经做完：显卡还在执行上一次提交时，镜像里的旧内容它可能还没读，直接写会让上一帧画到这一帧的数据。
        // 游戏每帧等显卡做完（Finish / 等同步）时这个条件总成立；不等的游戏退回按序拷。
        uint8_t *m = last != rp_sub_serial() && rp_prev_subs_done() ? nvn_gpu_buf_cpu(s->buf) : NULL;
        if (m) { sync_write(NULL, m, s, a - (uintptr_t)mem, b - (uintptr_t)mem); last = rp_sub_serial(); }
        else { nvn_gpu_cmd xc = rp_xfer_cmd(&last); if (xc) sync_write(xc, NULL, s, a - (uintptr_t)mem, b - (uintptr_t)mem); }
        for (uint64_t k = i; k <= j; k++) { s->synced[k] = st; s->sub[k] = last; }
        g_sync_bytes += b - a; g_sync_runs++; rp_perf_add(RP_PERF_BUF_SYNC_KB, (b - a) / 1024);
        i = j + 1;
    }
    if (gens != stack) free(gens);
    s->seen_ok = (uint8_t)tracked; s->seen_map = map0;
    for (uint64_t i = p; i <= q; i++) s->checked[i] = now0;   // now0 取在读各页代号之前：这之后才写的页，代号已经不等于 now0   // 没被写跟踪的池不走快路径（每次使用都整段同步）
    if (buf) *buf = s->buf;
    if (off) *off = addr - (uint64_t)(uintptr_t)mem;
    pthread_mutex_unlock(&g_bmu);
    return 1;
}

void rp_buf_pool_gone(const void *mem) {
    uint64_t k = (uint64_t)(uintptr_t)mem;
    pthread_mutex_lock(&g_bmu);
    struct shadow *s = nvn_hm_del(&g_shadow, &k);
    if (s) { nvn_gpu_buf_release(s->buf, NULL, NULL); free(s->synced); free(s->sub); free(s->checked); free(s); }
    pthread_mutex_unlock(&g_bmu);
}
void rp_buf_reset(void) {
    pthread_mutex_lock(&g_bmu);
    for (uint32_t i = 0; i < g_shadow.cap; i++) { struct shadow *s = g_shadow.vals[i]; if (s) { nvn_gpu_buf_release(s->buf, NULL, NULL); free(s->synced); free(s->sub); free(s->checked); free(s); } }
    free(g_shadow.keys); free(g_shadow.vals); nvn_hm_init(&g_shadow, 8);
    pthread_mutex_unlock(&g_bmu);
}
void rp_buf_stats(uint64_t *bytes, uint64_t *runs) { pthread_mutex_lock(&g_bmu); if (bytes) *bytes = g_sync_bytes; if (runs) *runs = g_sync_runs; pthread_mutex_unlock(&g_bmu); }
int rp_buf_mirrors(void) { pthread_mutex_lock(&g_bmu); int n = (int)g_shadow.n; pthread_mutex_unlock(&g_bmu); return n; }

// ---------- 缓冲类命令：只改游戏内存（见文件头）----------
static int in_pool(uint64_t a, uint64_t n) { return n && rp_pool_find(a, n, NULL, NULL); }
// 回放时在 CPU 上改游戏缓冲内容的三个命令：改一次代号加一（rp_draw 的段内常量缓冲切片按它失效）
uint64_t rp_cpu_write_gen;
void rp_op_update_ub(const void *a, const void *rec) {
    uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8);
    if (!n) return;
    if (!in_pool(d, n)) { nvn_unimpl("buf", "update_outside_pools", rec); return; }
    memcpy((void *)(uintptr_t)d, (const uint8_t *)a + 16, (size_t)n); rp_cpu_write_gen++;
}
void rp_op_copy_b2b(const void *a, const void *rec) {
    uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, (const uint8_t *)a + 8, 8); memcpy(&n, (const uint8_t *)a + 16, 8);
    if (!n) return;
    if (!in_pool(s, n) || !in_pool(d, n)) { nvn_unimpl("buf", "copy_outside_pools", rec); return; }
    memmove((void *)(uintptr_t)d, (const void *)(uintptr_t)s, (size_t)n); rp_cpu_write_gen++;
}
void rp_op_clear_buf(const p_clear_buf *cb, const void *rec) {   // 32 位样式填充（不是按字节）
    if (!cb->size) return;
    if (!in_pool(cb->addr, cb->size)) { nvn_unimpl("buf", "clear_outside_pools", rec); return; }
    uint8_t *p = (uint8_t *)(uintptr_t)cb->addr; uint64_t n = cb->size, i = 0; rp_cpu_write_gen++;
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
static void copydiag_attempt(const p_copy_t2t *o, const tex_t *s, const tex_t *d, int sfmt, int dfmt,
                             uint32_t sl, uint32_t sy, const NVNcopyRegion *sr,
                             uint32_t dl, uint32_t dy, const NVNcopyRegion *dr) {
    if (!nvn_plat_flag("nvk_copydiag", 0)) return;
    char line[1024];
    snprintf(line, sizeof line,
             "attempt flags=%#x src_serial=%llu src_nvn=%#x src_view_nvn=%#x src_mip=%u src_layer=%u src_region=%d,%d,%d,%d,%d,%d "
             "dst_serial=%llu dst_nvn=%#x dst_view_nvn=%#x dst_mip=%u dst_layer=%u dst_region=%d,%d,%d,%d,%d,%d "
             "original_src=%d,%d,%d,%d,%d,%d original_dst=%d,%d,%d,%d,%d,%d",
             (unsigned)o->flags, (unsigned long long)s->serial, (unsigned)s->d.format, (unsigned)sfmt, sl, sy,
             sr->xoffset, sr->yoffset, sr->zoffset, sr->width, sr->height, sr->depth,
             (unsigned long long)d->serial, (unsigned)d->d.format, (unsigned)dfmt, dl, dy,
             dr->xoffset, dr->yoffset, dr->zoffset, dr->width, dr->height, dr->depth,
             o->sr.xoffset, o->sr.yoffset, o->sr.zoffset, o->sr.width, o->sr.height, o->sr.depth,
             o->dr.xoffset, o->dr.yoffset, o->dr.zoffset, o->dr.width, o->dr.height, o->dr.depth);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "copydiag", line);
}

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
        if (o->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT | NVN_COPY_FLAGS_MIRROR_Z_BIT)) { nvn_unimpl("tex", "t2t_mirror", rec); return; }
        if (sr.width != dr.width || sr.height != dr.height || sfmt != dfmt) {
            // 缩放 / 换格式（iOS 是画出来的，copy_t2t_draw；这边后端有原生的缩放拷贝）：只做二维、单层、单采样、两边都不是整数 / 深度模板格式的
            nvn_fmt_info fa, fb; int ia = !nvn_fmt_info_of((uint32_t)sfmt, &fa) || fa.kind == NVN_FMT_UINT || fa.kind == NVN_FMT_SINT || fa.depth || fa.stencil;
            int ib = !nvn_fmt_info_of((uint32_t)dfmt, &fb) || fb.kind == NVN_FMT_UINT || fb.kind == NVN_FMT_SINT || fb.depth || fb.stencil;
            if (s3 || d3 || nz != 1 || dnz != 1 || s->d.samples > 1 || d->d.samples > 1 || ia || ib) { nvn_unimpl("tex", sfmt != dfmt ? "t2t_format_change" : "t2t_scaled", rec); return; }
            nvn_gpu_caps caps; nvn_gpu_caps_get(&caps);
            if (!(caps.caps & NVN_GPU_CAP_BLIT_SCALED)) { nvn_unimpl("tex", "t2t_scaled_no_native_blit", rec); return; }
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
    if (s3) {
        copydiag_attempt(o, s, d, sfmt, dfmt, sl, 0, &sr, dl, 0, &dr);
        nvn_gpu_copy_tex(c, gs, sl, 0, &sr, gd, dl, 0, &dr);   // 三维：片在区域的深度方向上，一次拷完
    }
    else for (int z = 0; z < nz; z++) {
        NVNcopyRegion a = sr, b = dr; a.zoffset = b.zoffset = 0; a.depth = b.depth = 1;
        copydiag_attempt(o, s, d, sfmt, dfmt, sl, sz0 + (uint32_t)sr.zoffset + (uint32_t)z,
                         &a, dl, dz0 + (uint32_t)dr.zoffset + (uint32_t)z, &b);
        nvn_gpu_copy_tex(c, gs, sl, sz0 + (uint32_t)sr.zoffset + (uint32_t)z, &a, gd, dl, dz0 + (uint32_t)dr.zoffset + (uint32_t)z, &b);
    }
    rp_tex_mark_gpu_written(d);   // 目标的内容从此以 GPU 为准
}
