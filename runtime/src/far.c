// 远端：别名区伸出宿主窗口的那段（地址空间根修线 10-27，docs/方案_地址空间根修.md；乙布局 = s2a_far_set 被调到即开，甲布局没有远端）。
//   设备上这段游戏地址根本没有宿主地址（iOS ≤26 免费签名的大地址档上限 63 GiB、最大连续空洞 51.0 GiB），游戏却会解引用它（巫师3 碰到 +49 GiB）。
//   这里把远端页的内容放在宿主别处的「影子区」：游戏地址 → 影子地址按 2 MiB 一块查目录（按游戏地址递增的顺序分块，相邻游戏块多半落在相邻影子块）。
//   游戏地址本身保持没有映射（Mac 上是压窗口的围栏），一碰即缺页：缺页处理里按「影子 − 游戏」的差值代执行这一条访存（emul_ld / emul_st），
//   游戏线程、宿主线程都能走（影子页本身可读写，不要别名槽）。同时记下每个访存点（模块 + 偏移、次数、类别）——出包期静态改写的依据。
#include "native.h"
#include "s2a_emul_st.h"
#include <errno.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <mach/mach.h>
#include <pthread.h>
#include <TargetConditionals.h>

#define FPG 0x4000ull                 // 宿主页
#define CHUNK (2ull << 20)            // 影子块
#define NDIR (1u << (S2A_FAR_SPAN_BITS - 21))   // 改写器跳板判「是不是远端」只要一次右移 36 位
static uint64_t g_lo, g_hi;           // 远端 [lo, hi)
static uint64_t g_sh_lo, g_sh_hi, g_sh_used;   // 影子区 = 堆区 [lo, hi)：影子块从 hi 往下分（已分 g_sh_used），堆从 lo 往上长（layout.c 乙）
static uint64_t g_lin;   // 线性影子基址（0 = 分块）：影子 = g_lin + (游戏 − g_lo)，整个远端一次预留地址（不可访问、不占内存），用到才放开。#419
static uint64_t g_ov_used;   // 堆区装不下后从宿主任意空闲地址分的影子（目录存的是每块差值，影子块放哪都成立；不占堆区，不影响 s2a_far_floor）
__attribute__((weak)) uint64_t s2a_mem_heap_end(void) { return 0; }   // svc_mem.c；只链本文件的单测没有堆
__attribute__((weak)) int s2a_guest_map_fixed(uint64_t a, uint64_t len) { (void)a; (void)len; return 1; }   // vawin.c：线性影子块走文件后备（1 = 没开文件后备）
__attribute__((weak)) void s2a_guest_commit(uint64_t a, uint64_t len) { (void)a; (void)len; }   // vawin.c：堆区影子在窗口里，放开前按块定后备（文件后备溢出模式）；窗口外的不管
// 每 2 MiB 游戏块一项「影子 − 游戏」的差值：0 = 没分块（影子在窗口前段、远端在窗口之外，差值不可能为 0）。
//   改写器的翻译跳板（runtime/rewriter，S2A_FAR_TSV 列的点）与缺页代执行查同一张表：s2a_far_desc = {远端起点, 目录}
static _Atomic int64_t *g_dir;
struct { uint64_t lo; _Atomic int64_t *dir; } s2a_far_desc;
// C# 宿主侧翻译（GuestMemory，接口 v2.13）取同一张表：回 &s2a_far_desc（地址定值；dir 为空 = 远端没开）
const void *s2a_far_desc_get(void) { return &s2a_far_desc; }
uint64_t s2a_far_floor(void) { return g_dir ? (g_lin ? g_sh_hi : g_sh_hi - g_sh_used) : UINT64_MAX; }   // 线性影子不占堆区
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
int s2a_far_has(uint64_t a, uint64_t n) { return g_dir && a >= g_lo && a + n <= g_hi && a + n >= a; }
// 远端的游戏地址必须一碰就缺页：空洞全占成不可访问的保留（不带 MAP_FIXED、核对落点，不盖别人）；已有宿主可访问的映射 = 远端访问会静默读写宿主内存，
//   报红并回段数（地址上限之外的部分本来就不可访问，不用占）
static int fence_far(uint64_t lo, uint64_t hi, uint64_t *fenced) {
    int clash = 0; vm_address_t a = lo; *fenced = 0;
    while (a < hi) {
        vm_address_t r = a; vm_size_t sz = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
        int ok = vm_region_64(mach_task_self(), &r, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) == KERN_SUCCESS;
        uint64_t he = ok && r < hi ? r : hi;
        if (he > a) { void *p = mmap((void *)a, he - a, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (p == (void *)a) *fenced += he - a; else if (p != MAP_FAILED) munmap(p, he - a); }   // 地址上限之外 mmap 失败：不可访问，正好
        if (!ok || r >= hi) break;
        if (info.protection != VM_PROT_NONE) { clash++; LOG("【红】远端：%#lx +%#lx 是宿主可访问的映射（权限 %d），游戏碰这段远端地址会读写到宿主内存", (unsigned long)r, (unsigned long)sz, info.protection); }
        a = r + sz;
    }
    return clash;
}
static void far_tick_start(void);
void s2a_far_set(uint64_t lo, uint64_t hi, uint64_t sh_lo, uint64_t sh_hi) {
    if (hi <= lo || sh_hi <= sh_lo) return;
    if (hi - lo > S2A_FAR_SPAN) hi = lo + S2A_FAR_SPAN;
    _Atomic int64_t *dir = calloc(NDIR, sizeof *dir);
    if (!dir) { LOG("【红】远端：目录分配失败，远端照旧回内存不足"); return; }
    uint64_t fenced; int clash = fence_far(lo, hi, &fenced);
    // 与宿主可访问的映射重叠（11-01 审计 P1-12）：远端地址碰到那里会静默读写宿主内存，不开远端（目录留空）。伸出窗口的物理映射照旧回内存不足（svc_mem.c va_short）
    if (clash) { free(dir); LOG("【红】远端：远端 %#llx +%llu MB 与宿主可访问的映射重叠 %d 段，不开远端（伸出窗口的映射回内存不足）", (unsigned long long)lo, (unsigned long long)((hi - lo) >> 20), clash); return; }
    if (g_lin) { munmap((void *)(uintptr_t)g_lin, g_hi - g_lo); g_lin = 0; }   // 再次开远端（单测）：上一份线性预留按旧跨度退
    g_lo = lo; g_hi = hi; g_sh_lo = sh_lo; g_sh_hi = sh_hi & ~(CHUNK - 1); g_dir = dir; s2a_far_desc.lo = lo; s2a_far_desc.dir = dir;
    // #419：分块影子从堆区顶往下切，后一次映射请求拿到更低的地址——游戏地址相邻、分两次映射的两块在影子里倒过来不相邻，显存池正好跨这两次映射时
    //   零拷贝缓冲建不起来（nvn_mtl.m pool_host「far shadow is not contiguous」，Mac 约 21 分钟 abort）。Mac 地址空间够：整个远端一次预留线性影子，
    //   任何游戏相邻的地址影子都相邻，与分几次请求无关。iOS 地址紧（免费签名大地址档上限 63 GiB），照旧分块；S2A_FAR_LINEAR=0 强制分块（单测用）
#if TARGET_OS_OSX
    const char *le = getenv("S2A_FAR_LINEAR");
    if (!le || strcmp(le, "0")) {
        void *p = mmap(NULL, hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, S2A_VMTAG_FD(S2A_VMTAG_FAR), 0);
        if (p != MAP_FAILED && !((uint64_t)(uintptr_t)p & (CHUNK - 1))) g_lin = (uint64_t)(uintptr_t)p;
        else if (p != MAP_FAILED) {   // 内核给的不按 2 MiB 对齐：多占 2 MiB 再对齐（影子块与游戏块同对齐，块内偏移一致）
            munmap(p, hi - lo); p = mmap(NULL, hi - lo + CHUNK, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
            if (p != MAP_FAILED) { uint64_t b = ((uint64_t)(uintptr_t)p + CHUNK - 1) & ~(CHUNK - 1), e = (uint64_t)(uintptr_t)p + hi - lo + CHUNK;
                if (b > (uint64_t)(uintptr_t)p) munmap(p, b - (uint64_t)(uintptr_t)p);
                if (e > b + (hi - lo)) munmap((void *)(uintptr_t)(b + (hi - lo)), e - b - (hi - lo));
                g_lin = b; }
        }
        if (!g_lin) LOG("远端：线性影子预留 %llu MB 失败（errno=%d），照旧分块（跨两次映射的显存池可能建不起零拷贝缓冲）", (unsigned long long)((hi - lo) >> 20), errno);
    }
#endif
    LOG("远端：开，远端 %#llx +%llu MB 的内容放影子区（与堆共用 %#llx +%llu MB，影子从顶往下 2 MiB 一块），游戏一碰远端地址即缺页代执行并记点；远端空洞占住 %llu MB、与宿主映射重叠 %d 段",
        (unsigned long long)lo, (unsigned long long)((hi - lo) >> 20), (unsigned long long)sh_lo, (unsigned long long)((sh_hi - sh_lo) >> 20), (unsigned long long)(fenced >> 20), clash);
    far_tick_start();
}
// 游戏地址 → 影子地址；没分块回 NULL。[*gbeg, *gend) 回差值相同的连续游戏地址（本块向前后各并一块）：代执行允许的访存范围。
//   跨块且两块影子不相邻的访存（巫师3 sdk+0x58763c 的 16 字节 stp 正好跨块界）走 s2a_far_fault 里的两页并排槽
static uint8_t *g2h(uint64_t a, uint64_t *gbeg, uint64_t *gend) {
    if (a < g_lo || a >= g_hi) return NULL;
    uint64_t i = (a - g_lo) / CHUNK; int64_t d = atomic_load_explicit(&g_dir[i], memory_order_acquire);
    if (!d) return NULL;
    if (gbeg) *gbeg = g_lo + (i && atomic_load(&g_dir[i - 1]) == d ? i - 1 : i) * CHUNK;
    if (gend) { *gend = g_lo + (i + 1 < NDIR && atomic_load(&g_dir[i + 1]) == d ? i + 2 : i + 1) * CHUNK; if (*gend > g_hi) *gend = g_hi; }
    return (uint8_t *)(uintptr_t)(a + (uint64_t)d);
}
uint8_t *s2a_far_g2h(uint64_t a) { return g2h(a, NULL, NULL); }

// ---------- 即时存档：远端按游戏地址存 / 取 ----------
// 远端的内容在影子块里，影子块的宿主地址每个进程不同（堆区顶往下切、溢出时宿主任意地址、Mac 线性预留），目录存的是「影子 − 游戏」差值。
//   存：逐个已分块的游戏 2 MiB 块，逐页记「游戏地址、当前权限、内容（全零不存）」——只有游戏地址，不含任何宿主地址；
//   取：按游戏地址用本进程的分配器重新分影子（s2a_far_map）、写回内容、还原权限，目录随之重建。快照格式与宿主地址、分配方式无关，跨版本同样成立
#include "snapfmt.h"
// 窗口内堆区里的影子块范围（内存导出要跳过：那里的字节归远端管，按游戏地址另存）；没开远端 / 线性影子回 0
int s2a_far_shadow_in_window(uint64_t *lo, uint64_t *hi) { if (!g_dir || g_lin || !g_sh_used) return 0; *lo = g_sh_hi - g_sh_used; *hi = g_sh_hi; return 1; }
int s2a_snap_far_save(FILE *f) {
    sfw w; sfw_sec(&w, f, "far", 1);
    sfw_rec(&w); sfw_u64(&w, 1, g_dir ? g_lo : 0); sfw_u64(&w, 2, g_dir ? g_hi : 0); sfw_end_rec(&w);
    uint64_t np = 0, nz = 0; static uint8_t zero[FPG];
    for (uint64_t i = 0; g_dir && g_lo + i * CHUNK < g_hi; i++) {
        int64_t d = atomic_load(&g_dir[i]); if (!d) continue;
        for (uint64_t off = 0; off < CHUNK; off += FPG) {
            uint64_t ga = g_lo + i * CHUNK + off; if (ga >= g_hi) break;
            uint8_t *h = (uint8_t *)(uintptr_t)(ga + (uint64_t)d);
            vm_address_t ra = (vm_address_t)(uintptr_t)h; vm_size_t rs = 0; vm_region_basic_info_data_64_t bi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t ob;
            if (vm_region_64(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &ob) != KERN_SUCCESS || ra > (vm_address_t)(uintptr_t)h) continue;
            if (!(bi.protection & VM_PROT_READ)) continue;   // 没给过游戏的页（块内只放开了请求覆盖的那些）
            sfw_rec(&w); sfw_u64(&w, 50, 1); sfw_u64(&w, 1, ga); sfw_u64(&w, 2, (uint64_t)bi.protection);
            if (memcmp(h, zero, FPG)) sfw_blob(&w, 3, h, FPG); else nz++;
            sfw_end_rec(&w); np++;
        }
    }
    LOG("即时存档：远端 %s，已给游戏的页 %llu 个（全零 %llu）", g_dir ? "开" : "没开", (unsigned long long)np, (unsigned long long)nz);
    return sfw_end(&w);
}
int s2a_snap_far_load(FILE *f) {
    sfr r; int rc = sfr_sec(&r, f, "far", 1);
    if (rc || sfr_rec(&r) != 1) { LOG("【红】即时存档恢复：远端段读不了%s", rc == -2 ? "（格式比本版新）" : ""); sfr_done(&r); return -1; }
    uint64_t lo = sfr_u64(&r, 1, 0), hi = sfr_u64(&r, 2, 0), np = 0, bad = 0;
    if (lo && (!g_dir || lo != g_lo || hi != g_hi)) { LOG("【红】即时存档恢复：远端范围与快照时不同（快照 %#llx–%#llx，本局 %#llx–%#llx）", (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)g_lo, (unsigned long long)g_hi); sfr_done(&r); return -1; }
    for (;;) {
        int t = sfr_rec(&r); if (t == 0) break; if (t < 0) { bad++; break; }
        if (sfr_u64(&r, 50, 0) != 1) continue;
        uint64_t ga = sfr_u64(&r, 1, 0); uint32_t prot = (uint32_t)sfr_u64(&r, 2, VM_PROT_READ | VM_PROT_WRITE);
        if (ga < g_lo || ga + FPG > g_hi || s2a_far_map(ga, FPG)) { LOG("【红】即时存档恢复：远端页 %#llx 分不到影子", (unsigned long long)ga); bad++; break; }
        uint8_t *h = g2h(ga, NULL, NULL); if (!h) { bad++; break; }
        uint32_t n = 0; const void *c = sfr_get(&r, 3, &n);
        if (c && n == FPG) memcpy(h, c, FPG); else memset(h, 0, FPG);
        int pp = (prot & VM_PROT_READ ? PROT_READ : 0) | (prot & VM_PROT_WRITE ? PROT_WRITE : 0);
        if (pp != (PROT_READ | PROT_WRITE) && mprotect(h, FPG, pp)) { bad++; break; }
        np++;
    }
    sfr_done(&r);
    LOG("%s即时存档恢复：远端页 %llu 个按游戏地址放回（本进程重新分影子、重建目录）", bad ? "【红】" : "", (unsigned long long)np);
    return bad ? -1 : 0;
}
// 已分块里 [a, a+n) 的影子页：改保护前按宿主映射项（vm_region，项内保护一致）切段、记下各段原保护，放不开时逐段退回（P2-19 后续）。
//   影子页原保护只有两种来源——给过（读写）、没给 / 还回（不可访问），但一次请求里两种可以交错，只能逐段取。持 g_mu
typedef struct { uint64_t h, n; int prot; } far_run;
static int old_runs(uint64_t a, uint64_t n, far_run **out, int *cnt) {   // 0 成功；取不到原保护 / 分配失败回 -1（什么都没改）
    far_run *v = NULL; int m = 0, cap = 0;
    for (uint64_t p = a; p < a + n;) {
        uint8_t *h = g2h(p, NULL, NULL);
        if (!h) { p += FPG; continue; }   // 新块里的页另管
        vm_address_t r = (vm_address_t)h; vm_size_t sz = 0; vm_region_basic_info_data_64_t in; mach_msg_type_number_t ct = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t ob = MACH_PORT_NULL;
        kern_return_t kr = vm_region_64(mach_task_self(), &r, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&in, &ct, &ob);
        if (ob != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), ob);
        if (kr != KERN_SUCCESS || r > (vm_address_t)h) { free(v); return -1; }
        uint64_t ce = g_lo + ((p - g_lo) / CHUNK + 1) * CHUNK, len = r + sz - (uint64_t)(uintptr_t)h;   // 本块内影子连续：到映射项尾 / 块尾 / 请求尾取最近
        if (len > ce - p) len = ce - p;
        if (len > a + n - p) len = a + n - p;
        len = (len + FPG - 1) & ~(FPG - 1);
        if (m == cap) { far_run *w = realloc(v, (size_t)(cap = cap ? cap * 2 : 8) * sizeof *v); if (!w) { free(v); return -1; } v = w; }
        v[m++] = (far_run){(uint64_t)(uintptr_t)h, len, in.protection & (PROT_READ | PROT_WRITE | PROT_EXEC)};
        p += len;
    }
    *out = v; *cnt = m; return 0;
}
static void runs_undo(const far_run *v, int m) { for (int i = 0; i < m; i++) if (v[i].prot != (PROT_READ | PROT_WRITE)) mprotect((void *)(uintptr_t)v[i].h, v[i].n, v[i].prot); }
// 给页：[a, a+n) 整宿主页（一次映射请求整段传进来），其中没分的影子块作为一整段从影子区顶切下、按游戏地址升序排——
//   一次映射的显存池在影子里连续，直连路径才能整池建一个零拷贝缓冲（nvn_mtl.m nvn_be_pool_add）。不越过堆顶；影子页放成可读写（新页是零页）。
//   堆区放不下（影子用满或撞上堆顶）：这一整段改从宿主任意空闲地址占（同样连续；巫师3 无 IML 档影子要 ≥2.2 GB，堆区只有 2 GB）。
//   0 成功；宿主也占不到、或影子页放不开回 -1（调用方回内存不足）。
//   新块的影子页先放开、成功了才发布目录项（11-01 审计 P2-19：原先先发布再放锁 mprotect，失败直接回 -1，目录留着指向不可访问的影子、
//   影子余量也白占——游戏碰这段远端照样按目录代执行到不可访问的影子上）；失败时新块整段收回、目录不动，已分块里改过的页逐段退回原保护
int s2a_far_map(uint64_t a, uint64_t n) {
    pthread_mutex_lock(&g_mu);
    uint64_t i0 = (a - g_lo) / CHUNK, i1 = (a + n - 1 - g_lo) / CHUNK, k = 0;
    for (uint64_t i = i0; i <= i1; i++) if (!atomic_load(&g_dir[i])) k++;
    far_run *rv = NULL; int rm = 0;   // 已分块里的页：先记原保护，再逐段放开；中途放不开退回已改的段
    if (old_runs(a, n, &rv, &rm)) { pthread_mutex_unlock(&g_mu); LOG("【红】远端：取不到已分块影子页的原保护（游戏 %#llx +%#llx），不改", (unsigned long long)a, (unsigned long long)n); return -1; }
    for (int r = 0; r < rm; r++) {
        if (rv[r].prot != (PROT_READ | PROT_WRITE)) s2a_guest_commit(rv[r].h, rv[r].n);   // 还回过的影子页（窗口里的块多半已定，不加锁直接回）
        if (rv[r].prot == (PROT_READ | PROT_WRITE) || !mprotect((void *)(uintptr_t)rv[r].h, rv[r].n, PROT_READ | PROT_WRITE)) continue;
        int e = errno; runs_undo(rv, r); free(rv); pthread_mutex_unlock(&g_mu);
        LOG("【红】远端：已分块影子页放不开（游戏 %#llx +%#llx，errno=%d），已改的 %d 段退回原保护", (unsigned long long)a, (unsigned long long)n, e, r);
        return -1;
    }
    if (k && g_lin) {   // 线性影子：块位置由游戏地址定，不分配不记账；文件后备时先把块映射成文件（同分块时的规则），再放开请求覆盖的页
        for (uint64_t i = i0; i <= i1; i++) {
            if (atomic_load(&g_dir[i])) continue;
            uint64_t cb = g_lo + i * CHUNK, cs = g_lin + i * CHUNK, lo = a > cb ? a : cb, hi = a + n < cb + CHUNK ? a + n : cb + CHUNK;
            if (s2a_guest_map_fixed(cs, CHUNK) < 0 || mprotect((void *)(uintptr_t)(cs + (lo - cb)), hi - lo, PROT_READ | PROT_WRITE)) {
                int e = errno;
                for (uint64_t u = i0; u <= i; u++) if (!atomic_load(&g_dir[u])) mmap((void *)(uintptr_t)(g_lin + u * CHUNK), CHUNK, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);   // 盖回预留；没发布过、游戏碰不到
                runs_undo(rv, rm); free(rv); pthread_mutex_unlock(&g_mu);
                LOG("【红】远端：线性影子页放不开（游戏 %#llx +%#llx，errno=%d），目录不发布", (unsigned long long)a, (unsigned long long)n, e);
                return -1;
            }
        }
        for (uint64_t i = i0; i <= i1; i++)
            if (!atomic_load(&g_dir[i])) atomic_store_explicit(&g_dir[i], (int64_t)(g_lin - g_lo), memory_order_release);
    } else if (k) {
        int ov = 0;
        uint64_t at = g_sh_hi - g_sh_used - k * CHUNK, he = s2a_mem_heap_end();
        if (g_sh_used + k * CHUNK <= g_sh_hi - g_sh_lo && at >= he) g_sh_used += k * CHUNK;
        else {
            void *p = s2a_guest_map_any(k * CHUNK); if (p == MAP_FAILED) p = mmap(NULL, k * CHUNK, PROT_NONE, MAP_PRIVATE | MAP_ANON, S2A_VMTAG_FD(S2A_VMTAG_FAR), 0);   // 客体内存文件后备时影子块也走文件（vawin.c）
            if (p == MAP_FAILED) { int e0 = errno; runs_undo(rv, rm); free(rv); pthread_mutex_unlock(&g_mu); LOG("【红】远端：影子区与堆撞上且宿主地址也占不到（堆顶 %#llx，堆区影子 %llu MB、堆区外 %llu MB，本次要 %llu MB，errno=%d）", (unsigned long long)he, (unsigned long long)(g_sh_used >> 20), (unsigned long long)(g_ov_used >> 20), (unsigned long long)(k * CHUNK >> 20), e0); return -1; }
            if (!g_ov_used) LOG("远端：堆区影子余量用完（堆顶 %#llx，堆区影子 %llu MB），影子块改从宿主任意空闲地址分", (unsigned long long)he, (unsigned long long)(g_sh_used >> 20));
            at = (uint64_t)(uintptr_t)p; g_ov_used += k * CHUNK; ov = 1;
        }
        for (uint64_t i = i0, j = 0; i <= i1; i++) {   // 新块里请求覆盖的页（块内连续）
            if (atomic_load(&g_dir[i])) continue;
            uint64_t cb = g_lo + i * CHUNK, lo = a > cb ? a : cb, hi = a + n < cb + CHUNK ? a + n : cb + CHUNK;
            if (!ov) s2a_guest_commit(at + j * CHUNK + (lo - cb), hi - lo);   // 堆区影子在窗口里（宿主任意地址分的那种已按文件后备映射，见上）
            if (mprotect((void *)(uintptr_t)(at + j++ * CHUNK + (lo - cb)), hi - lo, PROT_READ | PROT_WRITE)) {
                int e = errno;
                if (ov) { munmap((void *)(uintptr_t)at, k * CHUNK); g_ov_used -= k * CHUNK; }
                else { mprotect((void *)(uintptr_t)at, k * CHUNK, PROT_NONE); g_sh_used -= k * CHUNK; }   // 没发布过、游戏碰不到：新页还没写过
                runs_undo(rv, rm); free(rv); pthread_mutex_unlock(&g_mu);
                LOG("【红】远端：影子页放不开（游戏 %#llx +%#llx，errno=%d），新分的 %llu MB 影子收回、目录不发布", (unsigned long long)a, (unsigned long long)n, e, (unsigned long long)(k * CHUNK >> 20));
                return -1;
            }
        }
        for (uint64_t i = i0, j = 0; i <= i1; i++)
            if (!atomic_load(&g_dir[i])) atomic_store_explicit(&g_dir[i], (int64_t)(at + j++ * CHUNK - (g_lo + i * CHUNK)), memory_order_release);
    }
    free(rv); pthread_mutex_unlock(&g_mu);
    return 0;
}
// 还页：影子页换成新的不可访问占位（物理页当场归还，再给出去是零页；同 svc_mem.c pages_release）
void s2a_far_release(uint64_t a, uint64_t n) {
    for (uint64_t p = a; p < a + n; p += FPG) { uint8_t *h = g2h(p, NULL, NULL); if (h && !s2a_guest_zero((uint64_t)(uintptr_t)h, FPG) && mmap(h, FPG, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) die("远端还页 errno=%d", errno); }
}

// 点表：开放寻址（键 = pc，0 空），信号处理里只做原子操作
#define NSLOT (1u << 16)
typedef struct { _Atomic uint64_t pc; _Atomic uint64_t n; _Atomic uint32_t cls; _Atomic uint32_t fail; } site_t;
static site_t g_site[NSLOT];
static _Atomic uint64_t g_faults, g_emul, g_skip, g_nsite, g_full, g_fail, g_cls_fail[10];
uint64_t s2a_far_faults_total(void) { return atomic_load(&g_faults); }
// 逐秒账（#442 / D-DX74：Mac 大款玩久了内存池落进远端后掉到个位数帧，「慢在缺页代执行」要有直接读数）：远端开着就每秒醒一次，
//   这一秒有缺页才打一行（游戏没碰远端时不出行，日志不涨）；和 [longframe] / 心跳同在一份日志里按时间对齐看。S2A_FAR_TICK=0 关
// 逐秒账已结算到的累计缺页数（每秒醒来记账后存）：单测读口，等它追上 s2a_far_faults_total 再判，不按墙钟对齐秒边界（11-19：原先静 1.2 秒 / 等 2.2 秒，
//   负载高时醒得晚、前面的缺页混进来或 5 次读跨两秒 → 行数不对 → 假红）。只读，行为不变
static _Atomic uint64_t g_tick_seen, g_tick_n;   // g_tick_n：醒了几次（每秒一次，有没有缺页都算）
uint64_t s2a_far_tick_seen(void); uint64_t s2a_far_faults_total(void); uint64_t s2a_far_tick_n(void);
uint64_t s2a_far_tick_seen(void) { return atomic_load(&g_tick_seen); }
uint64_t s2a_far_tick_n(void) { return atomic_load(&g_tick_n); }
static void *far_tick(void *arg) {
    (void)arg; pthread_setname_np("s2a-far-tick");
    uint64_t pf = 0, pe = 0, ps = 0, pfl = 0, pn = 0, sec = 0;
    for (;;) {
        sleep(1); sec++;
        uint64_t f = atomic_load(&g_faults), e = atomic_load(&g_emul), s = atomic_load(&g_skip), fl = atomic_load(&g_fail), n = atomic_load(&g_nsite);
        if (f != pf)
            LOG("远端每秒 第 %llu 秒：缺页 %llu、代执行 %llu、跳过 %llu、失败 %llu、新访存点 %llu（累计缺页 %llu、访存点 %llu）", (unsigned long long)sec,
                (unsigned long long)(f - pf), (unsigned long long)(e - pe), (unsigned long long)(s - ps), (unsigned long long)(fl - pfl), (unsigned long long)(n - pn), (unsigned long long)f, (unsigned long long)n);
        pf = f; pe = e; ps = s; pfl = fl; pn = n; atomic_store(&g_tick_seen, f); atomic_fetch_add(&g_tick_n, 1);
    }
    return NULL;
}
static void far_tick_start(void) {
    static atomic_flag once = ATOMIC_FLAG_INIT;
    const char *e = getenv("S2A_FAR_TICK");
    if ((e && !strcmp(e, "0")) || atomic_flag_test_and_set(&once)) return;
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, far_tick, NULL)) LOG("【黄】远端：逐秒账线程起不来（errno=%d），只在收局 / 崩溃时打总账", errno);
    pthread_attr_destroy(&a);
}
static site_t *site_get(uint64_t pc) {
    uint32_t i = (uint32_t)((pc >> 2) * 0x9E3779B97F4A7C15ull >> 48) & (NSLOT - 1);
    for (uint32_t k = 0; k < NSLOT; k++, i = (i + 1) & (NSLOT - 1)) {
        uint64_t cur = atomic_load(&g_site[i].pc);
        if (cur == pc) return &g_site[i];
        if (!cur) { uint64_t z = 0; if (atomic_compare_exchange_strong(&g_site[i].pc, &z, pc)) { atomic_fetch_add(&g_nsite, 1); return &g_site[i]; } if (z == pc) return &g_site[i]; }
    }
    atomic_fetch_add(&g_full, 1); return NULL;
}
// 缓存维护（DC CIVAC / CVAC / CVAU / IVAC / ZVA 以外）与预取：对远端地址没有可做的（苹果内存一致），跳过这条
static int skippable(uint32_t i) {
    if ((i & 0xFFF80000u) == 0xD5080000u && ((i >> 12) & 0xF) == 7 && ((i >> 16) & 7) == 3 && ((i >> 8) & 0xF) != 4) return 1;   // DC <op>, Xt（op1 = 3，CRn = 7；CRm = 4 是 ZVA，要真写零）
    if ((i & 0xFFC00000u) == 0xF9800000u || (i & 0xFFE00C00u) == 0xF8A00800u || (i & 0xFF000000u) == 0xD8000000u) return 1;   // PRFM 立即数 / 寄存器 / 字面量
    return 0;
}

// 独占 / 原子（emul_ld / emul_st 不做）：远端地址的独占访存不能放回游戏地址重执行，在影子上做。
//   读独占（LDXR / LDAXR / LDXP）照常读影子并记本线程的监视（地址、长度、读到的值）；写独占（STXR / STLXR / STXP）对影子做一次比较交换，
//   值没变就算成功（状态 0）——模拟器通行做法（值被改了又改回的 ABA 当成功，与真机独占监视的差别只在这一点）。LSE（CAS / LD<op> / SWP）与
//   LDAR / STLR 直接在影子上做同宽原子操作。监视表按 pthread_self 键、信号处理里只做原子操作
#define NMON 256
static struct { _Atomic uintptr_t tid; uint64_t addr, len; unsigned __int128 val; } g_mon[NMON];
static int mon_slot(uintptr_t tid) {
    unsigned i = (unsigned)((tid >> 4) * 0x9E3779B97F4A7C15ull >> 56);
    for (unsigned k = 0; k < NMON; k++, i = (i + 1) % NMON) { uintptr_t z = 0, cur = atomic_load(&g_mon[i].tid);
        if (cur == tid || (!cur && atomic_compare_exchange_strong(&g_mon[i].tid, &z, tid)) || z == tid) return (int)i; }
    return -1;
}
static uint64_t rx(_STRUCT_ARM_THREAD_STATE64 *ss, unsigned r) { return r == 31 ? 0 : r == 29 ? ss->__fp : r == 30 ? ss->__lr : ss->__x[r]; }
static void wx(_STRUCT_ARM_THREAD_STATE64 *ss, unsigned r, uint64_t v) { if (r == 31) return; if (r == 29) ss->__fp = v; else if (r == 30) ss->__lr = v; else ss->__x[r] = v; }
static uint64_t ld_n(const void *p, unsigned n) { switch (n) { case 1: return __atomic_load_n((const uint8_t *)p, __ATOMIC_SEQ_CST); case 2: return __atomic_load_n((const uint16_t *)p, __ATOMIC_SEQ_CST);
    case 4: return __atomic_load_n((const uint32_t *)p, __ATOMIC_SEQ_CST); default: return __atomic_load_n((const uint64_t *)p, __ATOMIC_SEQ_CST); } }
static void st_n(void *p, unsigned n, uint64_t v) { switch (n) { case 1: __atomic_store_n((uint8_t *)p, (uint8_t)v, __ATOMIC_SEQ_CST); break; case 2: __atomic_store_n((uint16_t *)p, (uint16_t)v, __ATOMIC_SEQ_CST); break;
    case 4: __atomic_store_n((uint32_t *)p, (uint32_t)v, __ATOMIC_SEQ_CST); break; default: __atomic_store_n((uint64_t *)p, v, __ATOMIC_SEQ_CST); } }
static int cas_n(void *p, unsigned n, uint64_t *exp, uint64_t des) {   // 比较交换；失败时 *exp = 现值
    switch (n) {
    case 1: { uint8_t e = (uint8_t)*exp; int ok = __atomic_compare_exchange_n((uint8_t *)p, &e, (uint8_t)des, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *exp = e; return ok; }
    case 2: { uint16_t e = (uint16_t)*exp; int ok = __atomic_compare_exchange_n((uint16_t *)p, &e, (uint16_t)des, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *exp = e; return ok; }
    case 4: { uint32_t e = (uint32_t)*exp; int ok = __atomic_compare_exchange_n((uint32_t *)p, &e, (uint32_t)des, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *exp = e; return ok; }
    default: return __atomic_compare_exchange_n((uint64_t *)p, exp, des, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
}
static int64_t sx(uint64_t v, unsigned n) { return n == 8 ? (int64_t)v : (int64_t)(v << (64 - 8 * n)) >> (64 - 8 * n); }
// 1 = 已做（寄存器已更新，不含 pc）；0 = 不是这几类
static int far_atomic(_STRUCT_ARM_THREAD_STATE64 *ss, uint32_t i, uint64_t delta) {
    unsigned size = i >> 30, n = 1u << size, Rs = (i >> 16) & 31, Rt2 = (i >> 10) & 31, Rn = (i >> 5) & 31, Rt = i & 31;
    if (Rn == 31) return 0;   // 以 sp 为基址的访存不会落在远端
    uint64_t ea = rx(ss, Rn); uint8_t *h = (uint8_t *)(uintptr_t)(ea + delta);
    if (((i >> 24) & 0x3F) == 0x08) {   // 独占 / 载入获取 / 存储释放 / CAS
        unsigned o2 = (i >> 23) & 1, L = (i >> 22) & 1, o1 = (i >> 21) & 1;
        if (!o2 && !o1) {   // LDXR / LDAXR / STXR / STLXR
            int k = mon_slot((uintptr_t)pthread_self()); if (k < 0) return 0;
            if (L) { uint64_t v = ld_n(h, n); g_mon[k].addr = ea; g_mon[k].len = n; g_mon[k].val = v; wx(ss, Rt, v); return 1; }
            uint64_t e = (uint64_t)g_mon[k].val; int ok = g_mon[k].len == n && g_mon[k].addr == ea && cas_n(h, n, &e, rx(ss, Rt));
            g_mon[k].len = 0; wx(ss, Rs, ok ? 0 : 1); return 1;
        }
        if (!o2 && o1 && size >= 2) {   // LDXP / LDAXP / STXP / STLXP（每个寄存器 4 / 8 字节）；第二个寄存器可以是 xzr（写 0）。与 CASP 的区分靠位 31（CASP 恒 0、独占对恒 1），不靠 Rt2
            int k = mon_slot((uintptr_t)pthread_self()); unsigned e1 = size == 3 ? 8 : 4; if (k < 0) return 0;
            if (L) { unsigned __int128 v = size == 3 ? __atomic_load_n((unsigned __int128 *)h, __ATOMIC_SEQ_CST) : __atomic_load_n((uint64_t *)h, __ATOMIC_SEQ_CST);
                g_mon[k].addr = ea; g_mon[k].len = 2 * e1; g_mon[k].val = v;
                wx(ss, Rt, e1 == 8 ? (uint64_t)v : (uint32_t)v); wx(ss, Rt2, e1 == 8 ? (uint64_t)(v >> 64) : (uint32_t)((uint64_t)v >> 32)); return 1; }
            int ok = g_mon[k].len == 2 * e1 && g_mon[k].addr == ea;
            if (ok && e1 == 8) { unsigned __int128 e = g_mon[k].val, d = (unsigned __int128)rx(ss, Rt) | (unsigned __int128)rx(ss, Rt2) << 64;
                ok = __atomic_compare_exchange_n((unsigned __int128 *)h, &e, d, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
            else if (ok) { uint64_t e = (uint64_t)g_mon[k].val; ok = cas_n(h, 8, &e, (uint32_t)rx(ss, Rt) | (uint64_t)(uint32_t)rx(ss, Rt2) << 32); }
            g_mon[k].len = 0; wx(ss, Rs, ok ? 0 : 1); return 1;
        }
        if (o2 && !o1 && Rs == 31) { if (L) wx(ss, Rt, ld_n(h, n)); else st_n(h, n, rx(ss, Rt)); return 1; }   // LDAR / STLR（含 LO 变体）
        if (o2 && o1 && Rt2 == 31) { uint64_t e = rx(ss, Rs); cas_n(h, n, &e, rx(ss, Rt)); wx(ss, Rs, e); return 1; }   // CAS 族：Rs ← 旧值
        return 0;
    }
    if (((i >> 27) & 7) == 7 && !((i >> 26) & 1) && !((i >> 24) & 3) && ((i >> 21) & 1) && !((i >> 10) & 3)) {   // LSE：LD<op> / SWP
        unsigned o3 = (i >> 15) & 1, opc = (i >> 12) & 7; uint64_t v = rx(ss, Rs), old = ld_n(h, n), nw;
        if (o3 && opc) return 0;   // LDAPR 等不在这里
        for (;;) {
            switch (o3 ? 8 : opc) {
            case 0: nw = old + v; break; case 1: nw = old & ~v; break; case 2: nw = old ^ v; break; case 3: nw = old | v; break;
            case 4: nw = sx(old, n) > sx(v, n) ? old : v; break; case 5: nw = sx(old, n) < sx(v, n) ? old : v; break;
            case 6: { uint64_t m = n == 8 ? ~0ull : (1ull << 8 * n) - 1; nw = (old & m) > (v & m) ? old : v; break; }
            case 7: { uint64_t m = n == 8 ? ~0ull : (1ull << 8 * n) - 1; nw = (old & m) < (v & m) ? old : v; break; }
            default: nw = v;
            }
            uint64_t e = old; if (cas_n(h, n, &e, nw)) break; old = e;
        }
        wx(ss, Rt, n == 8 ? old : old & ((1ull << 8 * n) - 1)); return 1;
    }
    return 0;
}

// 跨页且两页影子不相邻（不同影子块）：把两页影子并排映射进本线程的 32 KB 槽（共享同一批物理页），在槽上代执行。
//   槽按 pthread_self 键、第一次用时建（信号处理里只调 mmap / vm_remap 两个系统调用）；用完不拆，下次覆盖
#define NXS 256
static struct { _Atomic uintptr_t tid; uint8_t *p; } g_xs[NXS];
static uint8_t *pair_slot(const uint8_t *p1, const uint8_t *p2) {
    uintptr_t tid = (uintptr_t)pthread_self(); unsigned i = (unsigned)((tid >> 4) * 0x9E3779B97F4A7C15ull >> 56), k;
    for (k = 0; k < NXS; k++, i = (i + 1) % NXS) { uintptr_t z = 0, cur = atomic_load(&g_xs[i].tid);
        if (cur == tid || (!cur && atomic_compare_exchange_strong(&g_xs[i].tid, &z, tid)) || z == tid) break; }
    if (k == NXS) return NULL;
    if (!g_xs[i].p) { void *m = mmap(NULL, 2 * FPG, PROT_NONE, MAP_PRIVATE | MAP_ANON, S2A_VMTAG_FD(S2A_VMTAG_FAR), 0); if (m == MAP_FAILED) return NULL; g_xs[i].p = m; }
    for (int j = 0; j < 2; j++) {
        vm_address_t a = (vm_address_t)(g_xs[i].p + j * FPG); vm_prot_t cur, max;
        if (vm_remap(mach_task_self(), &a, FPG, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)(j ? p2 : p1), FALSE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS
            || a != (vm_address_t)(g_xs[i].p + j * FPG) || vm_protect(mach_task_self(), a, FPG, FALSE, VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS) return NULL;
    }
    return g_xs[i].p;
}

// 缺页：1 = 已代执行（寄存器与 pc 已写回）；0 = 不是远端或代执行不了，调用方走老路（最终崩溃报告）
int s2a_far_fault(void *ctx, uint64_t fa, int w) {
    if (!s2a_far_has(fa, 1)) return 0;
    ucontext_t *uc = ctx; _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss; _STRUCT_ARM_NEON_STATE64 *ns = &uc->uc_mcontext->__ns;
    uint64_t pc = ss->__pc, lo, hi; uint32_t insn = *(const uint32_t *)(uintptr_t)pc; int cls = s2a_emul_class(insn);
    atomic_fetch_add(&g_faults, 1);
    site_t *s = site_get(pc);
    if (s) { atomic_fetch_add(&s->n, 1); atomic_store(&s->cls, (uint32_t)cls); }
    if (skippable(insn)) { atomic_fetch_add(&g_skip, 1); ss->__pc += 4; return 1; }
    uint8_t *h = g2h(fa, &lo, &hi);
    if (h && (insn & 0xFFFFFFE0u) == 0xD50B7420u) { memset((void *)((uintptr_t)h & ~63ull), 0, 64); ss->__pc += 4; atomic_fetch_add(&g_emul, 1); return 1; }   // DC ZVA：清 64 字节（苹果芯片 DCZID = 64 B）
    if (h) {
        s2a_regs r;
        for (int k = 0; k < 29; k++) r.x[k] = ss->__x[k];
        r.x[29] = ss->__fp; r.x[30] = ss->__lr; r.sp = ss->__sp;
        for (int k = 0; k < 32; k++) memcpy(r.v[k], &ns->__v[k], 16);
        uint64_t delta = (uint64_t)(uintptr_t)h - fa;   // 同一影子块内「影子 − 游戏」相同
        uint64_t ea = 0, len = 0;
        int ok = w ? s2a_emul_store_regs(insn, &r, delta, lo, hi, NULL, &ea, &len) : s2a_emul_load_regs(insn, &r, delta, lo, hi, NULL, &ea, &len);
        if (!ok && len && len <= FPG && (ea & ~(FPG - 1)) != ((ea + len - 1) & ~(FPG - 1))) {   // 跨页：两页影子并排进槽再试
            uint64_t pg = ea & ~(FPG - 1); uint8_t *p1 = g2h(pg, NULL, NULL), *p2 = g2h(pg + FPG, NULL, NULL);
            // 前一页是窗口的最后一页（远端紧接窗口末尾）：窗口里游戏地址就是宿主地址，拿它本身和远端首页的影子并排。
            //   11-14 D-52P7：LDR X8,[X10,X11] 读窗口末尾前 4 字节起的 8 字节，后半落进远端首页，原先这一页取不到影子 → 代执行不了 → SIGSEGV（玩 87 分钟后）
            if (!p1 && p2 && pg + FPG == g_lo) p1 = (uint8_t *)(uintptr_t)pg;
            uint8_t *sl = p1 && p2 ? pair_slot(p1, p2) : NULL;
            if (sl) { delta = (uint64_t)(uintptr_t)sl - pg; lo = pg; hi = pg + 2 * FPG;
                ok = w ? s2a_emul_store_regs(insn, &r, delta, lo, hi, NULL, NULL, NULL) : s2a_emul_load_regs(insn, &r, delta, lo, hi, NULL, NULL, NULL); }
        }
        if (!ok && far_atomic(ss, insn, delta)) { ss->__pc += 4; atomic_fetch_add(&g_emul, 1); return 1; }
        if (ok) {
            for (int k = 0; k < 29; k++) ss->__x[k] = r.x[k];
            ss->__fp = r.x[29]; ss->__lr = r.x[30]; ss->__sp = r.sp;
            if (!w) for (int k = 0; k < 32; k++) memcpy(&ns->__v[k], r.v[k], 16);
            ss->__pc += 4; atomic_fetch_add(&g_emul, 1); return 1;
        }
    }
    // 代执行不了（没分块 = 游戏碰了没映射的远端地址；独占 / 原子 / 跨影子块 / 不认识）：记账、走老路（崩溃报告带 pc）
    if (s) atomic_fetch_add(&s->fail, 1);
    atomic_fetch_add(&g_cls_fail[cls >= 0 && cls < 9 ? cls : 9], 1); atomic_fetch_add(&g_fail, 1);
    LOG("【红】远端：%s 代执行不了 pc %p 指令 %#010x 类别 %d 地址 %#llx%s", w ? "写" : "读", (void *)pc, insn, cls, (unsigned long long)fa, h ? "" : "（没映射）");
    return 0;
}

// ---------- SDK 内存函数族（巫师3 iOS 26 布局实测：memcpy 48.8% + memset 22.5% + FlushDataCache 11.1% 的远端触发）----------
// main / sdk 都经 PLT 调这几个（JUMP_SLOT 各一条）：装载器把表项改指这里（s2a_far_bind，只改数据、不动代码页）。
//   两端都不碰远端 = 宿主 libc（FlushDataCache 走原函数），只多一次区间比较；碰到远端按 2 MiB 块切段、换成影子地址再做
static uint64_t g_flush_orig;
__attribute__((weak)) void *sw_xstack_wrap(const char *name, void *fn) { (void)name; return fn; }   // xstack.c；只链本文件的单测直接调
static int touches(uint64_t a, uint64_t n) { return n && a < g_hi && a + n > g_lo; }
// 从 a 起最长 n 字节的一段：返回段长，*h = 可直接访问的地址（近地址原样；远端换影子，没映射照原地址——碰到即缺页走兜底 / 报红）
static uint64_t piece(uint64_t a, uint64_t n, uint8_t **h) {
    if (a < g_lo || a >= g_hi) { *h = (uint8_t *)(uintptr_t)a; return a < g_lo && a + n > g_lo ? g_lo - a : n; }
    uint64_t ce = (a & ~(CHUNK - 1)) + CHUNK, k = ce - a < n ? ce - a : n; uint8_t *x = g2h(a, NULL, NULL);
    *h = x ? x : (uint8_t *)(uintptr_t)a; return k;
}
static void *far_memcpy(void *d, const void *s, size_t n) {
    if (!touches((uint64_t)d, n) && !touches((uint64_t)s, n)) return memcpy(d, s, n);
    for (uint64_t pd = (uint64_t)d, ps = (uint64_t)s, left = n; left;) {
        uint8_t *hd, *hs; uint64_t a = piece(pd, left, &hd), b = piece(ps, left, &hs), k = a < b ? a : b;
        memcpy(hd, hs, k); pd += k; ps += k; left -= k;
    }
    return d;
}
static void *far_memset(void *d, int c, size_t n) {
    if (!touches((uint64_t)d, n)) return memset(d, c, n);
    for (uint64_t pd = (uint64_t)d, left = n; left;) { uint8_t *hd; uint64_t k = piece(pd, left, &hd); memset(hd, c, k); pd += k; left -= k; }
    return d;
}
static void *far_memmove(void *d, const void *s, size_t n) {
    if (!touches((uint64_t)d, n) && !touches((uint64_t)s, n)) return memmove(d, s, n);
    uint64_t a = (uint64_t)d, b = (uint64_t)s;
    if (a + n <= b || b + n <= a) return far_memcpy(d, s, n);   // 不重叠
    void *t = malloc(n); if (!t) die("远端 memmove：%zu 字节暂存分配失败", n);   // 重叠且碰到远端（罕见）：经暂存区，先整段读出再写回
    far_memcpy(t, s, n); far_memcpy(d, t, n); free(t); return d;
}
static void far_flush(const void *p, size_t n) {
    void (*orig)(const void *, size_t) = (void (*)(const void *, size_t))(uintptr_t)g_flush_orig;
    if (!touches((uint64_t)p, n)) { orig(p, n); return; }
    for (uint64_t pa = (uint64_t)p, left = n; left;) { uint8_t *h; uint64_t k = piece(pa, left, &h); orig(h, k); pa += k; left -= k; }
}
// 装载器重定位时问：这个导入要不要换成远端感知版（没有远端 = 原样）。v = 原本解析到的地址
uint64_t s2a_far_bind(const char *name, uint64_t v) {
    if (!g_dir || !v) return v;
    void *f = !strcmp(name, "memcpy") ? (void *)far_memcpy : !strcmp(name, "memset") ? (void *)far_memset : !strcmp(name, "memmove") ? (void *)far_memmove
            : !strcmp(name, "_ZN2nn2os14FlushDataCacheEPKvm") ? (void *)far_flush : NULL;
    if (!f) return v;
    if (f == (void *)far_flush) { if (g_flush_orig && g_flush_orig != v) LOG("【红】远端：FlushDataCache 两处解析到不同地址 %#llx / %#llx", (unsigned long long)g_flush_orig, (unsigned long long)v); g_flush_orig = v; }
    return (uint64_t)sw_xstack_wrap(name, f);
}

// 收局 / 崩溃：一行总账 + 点表文件（S2A_FAR_OUT，缺省 <局目录>/far_sites.tsv；列：模块 偏移 次数 类别 代执行失败）
void s2a_far_report(const char *when) {
    if (!g_dir) return;
    uint64_t ng = 0, nh = 0, dg = 0, dh = 0;
    const char *out = getenv("S2A_FAR_OUT"); char path[1100];
    if (!out) { const char *d = getenv("S2A_DOCS"); snprintf(path, sizeof path, "%s/far_sites.tsv", d ? d : "."); out = path; }
    FILE *f = fopen(out, "w");
    if (f) fprintf(f, "# 模块\t偏移\t次数\t类别\t代执行失败\n");
    for (uint32_t i = 0; i < NSLOT; i++) {
        uint64_t pc = atomic_load(&g_site[i].pc); if (!pc) continue;
        uint64_t n = atomic_load(&g_site[i].n); module_t *m = module_of((void *)pc);
        if (m) { ng++; dg += n; } else { nh++; dh += n; }
        if (f) fprintf(f, "%s\t%#llx\t%llu\t%u\t%u\n", m ? m->name : "宿主", (unsigned long long)(m ? pc - (uint64_t)m->base : pc), (unsigned long long)n, atomic_load(&g_site[i].cls), atomic_load(&g_site[i].fail));
    }
    if (f) fclose(f);
    char cf[256]; int k = 0; cf[0] = 0;
    for (int c = 0; c < 10; c++) { uint64_t v = atomic_load(&g_cls_fail[c]); if (v && k < (int)sizeof cf - 24) k += snprintf(cf + k, sizeof cf - (size_t)k, " 类%d×%llu", c, (unsigned long long)v); }
    LOG("远端 [%s]：影子区用 %llu MB（堆区外 %llu MB）；缺页 %llu 次、代执行 %llu 次、跳过（缓存维护 / 预取）%llu 次、失败 %llu 次%s；游戏代码访存点 %llu 个（触发 %llu 次）、宿主代码点 %llu 个（触发 %llu 次）；点表满 %llu；点表 %s",
        when, (unsigned long long)((g_sh_used + g_ov_used) >> 20), (unsigned long long)(g_ov_used >> 20), (unsigned long long)atomic_load(&g_faults), (unsigned long long)atomic_load(&g_emul), (unsigned long long)atomic_load(&g_skip),
        (unsigned long long)atomic_load(&g_fail), k ? cf : "", (unsigned long long)ng, (unsigned long long)dg, (unsigned long long)nh, (unsigned long long)dh, (unsigned long long)atomic_load(&g_full), f ? out : "（写不出）");
}

// 所有 guest/fault、服务与 renderer 读者已退出。窗口内影子由 window reset
// 回收；窗口外溢出块在此逐块解除映射。远端 PROT_NONE 围栏保留。
int s2a_far_session_reset(void) {
    for (unsigned i = 0; i < NXS; i++) {
        if (g_xs[i].p && munmap(g_xs[i].p, 2 * FPG)) {
            LOG("【红】远端跨页槽释放失败 errno=%d", errno); return -1;
        }
        g_xs[i].p = NULL; atomic_store(&g_xs[i].tid, 0);
    }
    if (g_lin) { if (munmap((void *)(uintptr_t)g_lin, g_hi - g_lo)) { LOG("【红】远端线性影子释放失败 errno=%d", errno); return -1; } g_lin = 0; }
    else if (g_dir) for (uint64_t i = 0; i < NDIR; i++) {
        int64_t delta = atomic_load(&g_dir[i]);
        if (!delta) continue;
        uint64_t shadow = g_lo + i * CHUNK + (uint64_t)delta;
        if (shadow >= g_sh_lo && shadow < g_sh_hi) continue;
        if (munmap((void *)(uintptr_t)shadow, CHUNK)) {
            LOG("【红】远端窗口外影子块释放失败 errno=%d", errno); return -1;
        }
        atomic_store(&g_dir[i], 0); g_ov_used -= CHUNK;
    }
    s2a_far_desc.dir = NULL; s2a_far_desc.lo = 0;
    free(g_dir); g_dir = NULL;
    g_lo = g_hi = g_sh_lo = g_sh_hi = g_sh_used = g_ov_used = g_flush_orig = 0;
    memset(g_mon, 0, sizeof g_mon); memset(g_site, 0, sizeof g_site);
    atomic_store(&g_faults, 0); atomic_store(&g_emul, 0); atomic_store(&g_skip, 0);
    atomic_store(&g_nsite, 0); atomic_store(&g_full, 0); atomic_store(&g_fail, 0);
    for (unsigned i = 0; i < 10; i++) atomic_store(&g_cls_fail[i], 0);
    return 0;
}
