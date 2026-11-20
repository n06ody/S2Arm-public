// 内存 / 信息 / 计时 / 调试类系统调用（旧库 hle_svc.c 内存类 + hle_vamm.c 宿主映射部分，改后搬）。
// vawin.c + layout.c 持有宿主地址窗口；Apple 可用远端影子，Android 暂沿用窗口内布局。
// 4 KB 游戏页按 16 KB 分组记账；Android 映射原语使用实际宿主页，Apple 共享内存保留 16 KB 对齐。
// 随包表：addr_need.tsv（出包组装 pack/ship_ext.py 从产物根拷进 App，runtime/build.mk 按这行自动收清单）
#include <dlfcn.h>
#include "s2a_init_fail.h"
#include "native.h"
#include "as_type.h"
#include "s2a_docs.h"
#include <errno.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <time.h>
#if defined(__ANDROID__) && __has_include(<sys/system_properties.h>)
#include <sys/system_properties.h>
#endif
#ifndef __ANDROID__
#include <TargetConditionals.h>
#endif
#include <unistd.h>
#include "../../platform/plat.h"
#ifdef __ANDROID__
#include "../../android/native/runtime/tracking/runtime.h"
#include "../../android/native/runtime/tracking/mirror.h"
#include "../../android/native/runtime/vm/backing.h"
#include "../../android/native/runtime/vm/writers.h"
#include "../../graphics/track/s2a_track.h"
#include "../../graphics/track/track_platform.h"
#else
#include <mach/mach.h>
#include <mach/vm_map.h>
#endif

#define HPG 0x4000ull                 // 宿主页
// HPG groups the 4K guest bitmap; protection syscalls use the real host page.
static uint64_t memory_protection_page(void) {
#ifdef __ANDROID__
    uint64_t page = plat_page_size();
    return page >= 0x1000 && !(page & (page - 1)) ? page : 0;
#else
    return HPG;
#endif
}
#define APP_POOL S2A_APP_POOL         // 应用内存池 = Switch 3285 MB / Switch 2 9 GiB（native.h）；SetHeapSize 上限 = 池 − 映像
enum { ST_FREE = 0, ST_CODE = 3, ST_CODEDATA = 4, ST_HEAP = 5, ST_SHARED = 6, ST_STACK = 0x0B, ST_THREADLOCAL = 0x0C, ST_TMEM_ISO = 0x0D, ST_TMEM = 0x0E, ST_INACCESSIBLE = 0x10,
       ST_CODE_RO = 0x14, ST_CODE_RW = 0x15 };

static s2a_layout L;
static int g_npdm_core, g_npdm_prio;   // NPDM 头的缺省核 / 主线程优先级（S2A_SCHED_HZ 用，thread.c）
static int g_npdm_ounce;                // NPDM 是 Switch 2 布局（npdm_read 认，系统计数器按 31.25 MHz）
// 直连图形层按它走 NVN2（graphics/nvn/gfx/nvn/nvn2.c；那边有弱定义缺省 0，单测不链原生层）：装载时 npdm_read 已定，之后只读
int s2a_platform_ounce(void) { return g_npdm_ounce; }
static uint64_t g_program_id, g_core_mask, g_prio_mask;
static uint64_t g_svc_perm[3];   // NPDM ACI0 内核能力的系统调用许可位（0–0xBF）   // 后两项来自 NPDM ACI0 内核能力的 ThreadInfo 项（同 Ryujinx KProcessCapabilities）
static uint8_t *g_heap; static size_t g_heap_cur;
// MapPhysicalMemory 已映射的物理页（4 KB 子页计）。Switch / Ryujinx 里这部分和堆、代码共用应用内存池（KPageTableBase.MapPhysicalMemory
//   向进程资源上限 Reserve，超了回 ResLimitExceeded 0x10801）。原先不记账也不设限：10-23 异度神剑3 一直映射下去，
//   常驻在 Mac 上 1 秒冲到 32 GB、iPhone 上起来 5 秒即退。缺省开（10-23 维护者：照 Switch 语义实现对的就直接开——游戏本就受 Switch 上限约束；
//   记账口径同 Ryujinx，见 pool_fixed）；S2A_PHYS_LIMIT=0 应急关
static uint64_t g_phys;
static uint64_t g_mstack;   // 主线程栈（NPDM MainThreadStackSize 按 4 KB 取整），进应用池记账
static int phys_limit_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("S2A_PHYS_LIMIT"); v = !(e && *e == '0'); } return v; }
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   // 堆、映射表、子页掩码、TLR 池
// 锁序断言（#1217 原生层裁定第 1 条）：持 g_mu 时不得再取 NVN 的 g_vp_mu。
//   g_mu 是可重入 / 可临时放开的（backing_begin_locked 放锁等 writers 停稳），所以用线程局部深度记
//   「当前线程此刻是否持着它」，而不是 pthread_mutex_t 的属性。Debug 构建下 hle_nvn.c 的
//   vp_lock() 调 s2a_mem_lockorder_vp_enter()，这里深度非 0 直接 abort。
//   苹果没有 NVN 的 g_vp_mu / 这条断言，宏展开回原来的 pthread 调用，不引入线程局部状态。
#ifdef __ANDROID__
static _Thread_local unsigned g_mu_depth;
#define GMU_LOCK() do { pthread_mutex_lock(&g_mu); g_mu_depth++; } while (0)
#define GMU_UNLOCK() do { if (!g_mu_depth) abort(); g_mu_depth--; pthread_mutex_unlock(&g_mu); } while (0)
#define GMU_TRYLOCK() (pthread_mutex_trylock(&g_mu) == 0 ? (g_mu_depth++, 1) : 0)
int s2a_mem_g_mu_held(void) { return g_mu_depth != 0; }
// hle_nvn.c 的 vp_lock() 在取 g_vp_mu 之前调这里：持 g_mu 时取 NVN 锁会形成
// g_mu → g_vp_mu 的反向，和 NVN 的 g_vp_mu → g_mu 组成环。Debug 构建直接中止，
// 正式构建不付代价（这个断言只证明锁序，不是运行时兜底）。
void s2a_mem_lockorder_vp_enter(void) {
#if !defined(NDEBUG)
    if (g_mu_depth) { LOG("【红】锁序违约：线程持 g_mu 时取 g_vp_mu（#1217 锁序断言）"); abort(); }
#endif
}
static _Thread_local int g_phys_fast;   // 本次 0x2C 已按“只向全空宿主页增页”取得只串行 epoch
#else
// 苹果：保持 main 的原语，不引入深度计数与 underflow abort。
#define GMU_LOCK() pthread_mutex_lock(&g_mu)
#define GMU_UNLOCK() pthread_mutex_unlock(&g_mu)
#define GMU_TRYLOCK() (pthread_mutex_trylock(&g_mu) == 0)
#endif

#ifdef __ANDROID__
static uint64_t backing_limit_end(void);
static void vma_report_start(void);
// Q3 计数：0x01 增长 / 0x2C 复用撞上 NVN 长寿别名；deferred 计数：释放时因 ref 跳过的宿主页。
static _Atomic uint64_t g_backing_reuse_denied, g_backing_release_deferred;
// Q6 打洞计数与累计耗时（ns）：提交前打洞 / 释放打洞 / 最后一次 unref 补打。只在 Android 编译。
// 只统计真正调了 fallocate 的那几次（deferred 没打洞，不进计数）。
enum { BACKING_PUNCH_COMMIT, BACKING_PUNCH_RELEASE, BACKING_PUNCH_UNREF, BACKING_PUNCH_KINDS };
static _Atomic uint64_t g_backing_punch_count[BACKING_PUNCH_KINDS];
static _Atomic uint64_t g_backing_punch_ns[BACKING_PUNCH_KINDS];
static void backing_reuse_denied(const char *what, uint64_t a, uint64_t n, _Atomic uint64_t *count) {
    uint64_t k = atomic_fetch_add(count, 1) + 1;
    if (!(k & (k - 1)))   // 首次与 2^k 次
        LOG("【红】%s %#llx+%#llx 仍被别名引用，回 InvalidMemState（第 %llu 次）", what,
            (unsigned long long)a, (unsigned long long)n, (unsigned long long)k);
}
#endif
#ifdef __ANDROID__
static void vma_report_start(void);
#endif
const s2a_layout *s2a_layout_get(void) { return &L; }
// 窗口里一段第一次放开成可读写之前调（vawin.c s2a_guest_commit）：客体内存文件后备溢出模式按块定后备（预算内匿名、超出的块换文件）。
//   只登记匿名占位 → 读写的放开点；vm_remap 别名盖上去的（alias_map、MapMemory 对齐路径、MapSharedMemory）不调——那段的页是源的，不占这边的后备。
//   不进 native.h（出包抽取阶段的键）；安卓窗口另有实现（vawin_android.inc），不调
#ifndef __ANDROID__
void s2a_guest_commit(uint64_t a, uint64_t n);
#define GUEST_COMMIT(a, n) s2a_guest_commit((uint64_t)(a), (uint64_t)(n))
#else
#define GUEST_COMMIT(a, n) ((void)0)
#endif
uint64_t s2a_mem_heap_end(void) { return (uint64_t)g_heap + g_heap_cur; }
void s2a_mem_guest_split(uint64_t *heap_lo, uint64_t *heap_len, uint64_t *alias_lo, uint64_t *alias_len, uint64_t *phys) { *heap_lo = (uint64_t)g_heap; *heap_len = g_heap_cur; *alias_lo = L.alias_base; *alias_len = L.alias_size; *phys = g_phys; }
uint64_t s2a_program_id(void) { return g_program_id; }

// ---------- 钩子（显卡线登记，§4.5）----------
static s2a_track_hooks g_hooks;
void s2a_register_track_hooks(const s2a_track_hooks *h) { if (h) g_hooks = *h; }
static void prot_changed(uint64_t a, uint64_t n) { if (g_hooks.prot_changed) g_hooks.prot_changed(a, n); }
#ifndef __ANDROID__
static void backing_changed(uint64_t a, uint64_t n) { if (g_hooks.backing_changed) g_hooks.backing_changed(a, n); }
static int (*g_bk_begin)(uint64_t, uint64_t, uint64_t); static void (*g_bk_end)(uint64_t, uint64_t, int, int);
void s2a_register_track_backing(int (*begin)(uint64_t, uint64_t, uint64_t), void (*end)(uint64_t, uint64_t, int, int)) { g_bk_begin = begin; g_bk_end = end; }
// 苹果改表系统调用串行（同安卓 svc_mem() 外层 writer epoch 的这一半作用）：换页事务 begin 等排空期时放开 g_mu，放开期间别的改表调用不许插进来
static pthread_mutex_t g_vm_writer = PTHREAD_MUTEX_INITIALIZER;
#endif
static void host_write(uint64_t a, uint64_t n) { if (g_hooks.host_write) g_hooks.host_write(a, n); }
void s2a_host_write_hook(uint64_t a, uint64_t n) { host_write(a, n); }   // thread.c 等别处写游戏内存前调
#ifndef __ANDROID__   // 与本文件其余苹果段同口径（端点核对在 Mac 上加 -D__ANDROID__ 编，__APPLE__ 照样为真）
// 装载器（宿主扮演 rtld）写游戏内存里的一个 8 字节槽：nn::ro 装 NRO 后把起局没解析的导入补绑过去（loader.c s2a_nro_map）。
//   11-09 玩家 iPhone 17 Pro Max（01001BA027DD8000，Nmpl 引擎）：游戏 DT_INIT 后用 SetMemoryPermission 把 main 的 RELRO（导入表所在）改只读，
//   随后装 romfs/nro/NmplDLL.nro，补绑直接写只读页 → SIGBUS（同地址连崩 3 局）。真机 rtld 不受游戏设的页权限约束（这类导入走延迟绑定），宿主也不该受：
//   槽所在页可写就直接写；只读的数据页经跟踪器同一钩子（guest_protect，与缺页自愈串行）临时加写、写入、恢复原保护；代码页（可执行）一律不写。返回 0 成功、-1 没写
int s2a_loader_store64(uint64_t *where, uint64_t v) {
    uint64_t a = (uint64_t)(uintptr_t)where, page = memory_protection_page(); if (!page || (a & 7)) return -1;
    uint64_t lo = a & ~(page - 1);
    vm_address_t ra = (vm_address_t)lo; vm_size_t rs = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
    if (vm_region_64(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS || ra > lo) return -1;
    if (info.protection & VM_PROT_EXECUTE) return -1;
    host_write(a, 8);
    if (info.protection & VM_PROT_WRITE) { __atomic_store_n(where, v, __ATOMIC_RELEASE); return 0; }
    int cur = (info.protection & VM_PROT_READ) ? PROT_READ : PROT_NONE;
    int (*prot)(uint64_t, uint64_t, int) = g_hooks.guest_protect;
    if (prot ? prot(lo, page, cur | PROT_READ | PROT_WRITE) : mprotect((void *)(uintptr_t)lo, page, cur | PROT_READ | PROT_WRITE)) return -1;
    __atomic_store_n(where, v, __ATOMIC_RELEASE);
    int e = prot ? prot(lo, page, cur) : mprotect((void *)(uintptr_t)lo, page, cur);
    prot_changed(lo, page);
    return e ? -1 : 0;
}
#endif
#ifdef __ANDROID__
// 启动期间、尚未放行guest时登记同一个shared VM写入/权限通道，不建立第二张映射表。
int s2a_android_tracking_vm_prepare(void) {
    if ((g_hooks.prot_changed && g_hooks.prot_changed != s2a_track_prot_changed) ||
        (g_hooks.guest_protect && g_hooks.guest_protect != s2a_track_set_guest_protection) ||
        (g_hooks.host_write && g_hooks.host_write != s2a_track_write) || g_hooks.backing_changed) return EBUSY;
    s2a_track_hooks hooks = {s2a_track_prot_changed, s2a_track_write, NULL, s2a_track_set_guest_protection};
    s2a_register_track_hooks(&hooks);
    return 0;
}
int s2a_android_tracking_vm_ready(void) {
    return g_hooks.prot_changed == s2a_track_prot_changed && g_hooks.guest_protect == s2a_track_set_guest_protection &&
        g_hooks.host_write == s2a_track_write &&
        !g_hooks.backing_changed && s2a_android_guest_writers_ready() == 1;
}
#endif


// ---------- 映射表：MapMemory 的栈、共享内存（只登记游戏请求的范围，QueryMemory 据此回报；SDK 靠它找空位）----------
#define MAX_MAP 4096
static struct { uint64_t base, size, src; uint32_t state; uint32_t h; uint32_t perm; } g_map[MAX_MAP];   // perm：QueryMemory 回的权限（共享内存按映射时要的，其余读写）
// 并行标志位（g_map 结构不改）：MAPF_BACKING_REF = 这项映射的源在背板区、已 ref。
//   只有 Android 编译 / 读写它；苹果没有背板，数组与交换删除都不参与编译。
// map_del 的交换删除必须同步交换，否则别名解映射时会 unref 错源。
#ifdef __ANDROID__
enum { MAPF_BACKING_REF = 1 };
static uint8_t g_map_flags[MAX_MAP];
#endif
static int g_nmap;
static int map_find(uint64_t a, uint64_t n) { for (int i = 0; i < g_nmap; i++) if (a < g_map[i].base + g_map[i].size && g_map[i].base < a + n) return i + 1; return 0; }   // 持 g_mu；1 基
enum { MAP_ALIAS = 1 };   // 栈映射（ST_STACK）的 h：1 = 目标与源共享同一批物理页（0x04 对齐时），0 = 拷贝
// 局内峰值（游戏地址用量行报；10-21 缩窗口尾部的前提：堆 / 栈区 / 可随机区各要「峰值 × 2 ≤ 区大小」）：当前字节、峰值字节、用到的最高区内偏移
static uint64_t g_cur_st, g_cur_sh, g_pk_st, g_pk_sh, g_pk_st_end, g_pk_sh_end, g_pk_heap;
static uint64_t g_pk_heap_request, g_pk_alias_end;
// 学跨度只认「装得进窗口」的地址（#1143 续，11-15 诊断普查）：窗口放在 2^39 以下最大空洞的顶上，任何一局的窗口都不会超过这块空洞（vawin.c g_fit）。
//   游戏在别名区高处做的物理映射（窗口外、远端影子代管）离窗口基址超过空洞大小，哪一局都装不进窗口，算进跨度只会把记录撑到整个布局：
//   某机 49 GB 窗口那局别名高水位 66.6 GB，记下 73 GB，此后每局窗口不缩、宿主只留 3 GB，低区碰底崩（1312 / 2954 局学到的跨度 ≥ 空洞）。
//   g_span_lim = 空洞大小（vawin.c 占窗后经 s2a_mem_span_limit 给；0 = 不限，安卓 / 单测）；g_pk_alias_in = 别名区映射末端截到这个界内的最高偏移
static uint64_t g_span_lim, g_pk_alias_in;
void s2a_mem_span_limit(uint64_t lim) { g_span_lim = lim; }
static void alias_in_note(uint64_t a, uint64_t size) {
    uint64_t e = a + size, cap = g_span_lim ? L.win_base + g_span_lim : UINT64_MAX;
    if (e > cap) e = cap;
    if (e > a && e - L.alias_base > g_pk_alias_in) g_pk_alias_in = e - L.alias_base;
}
// 跨度 = 各分量末端里离窗口基址最远、且不超过界的那个（超界的整项不算：截到界上等于学成「整块空洞」，宿主照旧只剩 3 GB）。纯函数，t_svc_mem 测
uint64_t s2a_span_pick(uint64_t win_base, uint64_t lim, const uint64_t *ends, int n) {
    uint64_t best = 0;
    for (int i = 0; i < n; i++) { if (ends[i] <= win_base) continue; uint64_t o = ends[i] - win_base; if (lim && o > lim) continue; if (o > best) best = o; }
    return best;
}
static _Atomic uint64_t g_va_short;   // 映射落在宿主没占到的地址、回了内存不足的次数（win_has / va_short）
static void addr_need_report(void) {
    LOG("地址需求 v2：程序号 %016llx 堆请求 %llu 字节 堆成功 %llu 字节 别名高水位 %llu 字节",
        (unsigned long long)g_program_id, (unsigned long long)g_pk_heap_request,
        (unsigned long long)g_pk_heap, (unsigned long long)g_pk_alias_end);
}
static void map_peak(uint32_t st, uint64_t b, uint64_t n, int add) {
    uint64_t *cur = st == ST_STACK ? &g_cur_st : st == ST_SHARED ? &g_cur_sh : NULL; if (!cur) return;
    if (!add) { *cur -= n; return; }
    *cur += n; uint64_t *pk = st == ST_STACK ? &g_pk_st : &g_pk_sh, *pe = st == ST_STACK ? &g_pk_st_end : &g_pk_sh_end, rb = st == ST_STACK ? L.stack_base : L.aslr_base;
    if (*cur > *pk) *pk = *cur;
    if (b + n > rb && b + n - rb > *pe) *pe = b + n - rb;
}
#ifdef __ANDROID__
// 改表断言（#619 P1-1 加固）：第一次进 svc_mem() 之后（游戏线程已在跑）改映射表 / 堆，必须在本线程的 VM epoch 里——
//   MapMemory / UnmapMemory / SetHeapSize 拷贝与跟踪时放开 g_mu，正确性全靠 svc_mem() 外层 writers_begin 把所有改表系统调用串行化。
//   起局建表（s2a_mem_init）在任何系统调用之前，不判
int s2a_android_vm_writer_held(void);
// 单向置位、起局建表先于任何系统调用
static _Atomic int g_mem_svc_live;
static void vm_mutation_check(const char *what) { if (atomic_load_explicit(&g_mem_svc_live, memory_order_relaxed) && !s2a_android_vm_writer_held()) die("%s不在 VM epoch 内（新改表的系统调用没登记进 svc_mem() 的 backing 列表？）", what); }
#else
static void vm_mutation_check(const char *what) { (void)what; }
#endif
// SetMemoryPermission 的权限账（11-01 审计 P2-14）：游戏按 4 KB 改过的权限只记这一处，query / readable 按它盖过各来源的缺省权限（堆 / 物理映射读写、模块分段等）。
//   升序、互不重叠、相邻同权限合并；换页（堆伸缩、物理映射新增 / 解除、映射表增删）清掉那段——新给的页按来源缺省，同 Horizon 新页的权限。持 g_mu
//   属性账（g_attr，同一套区间表，值 = 属性位）：目前只记借出（ATTR_BORROWED，同 Horizon / Ryujinx MemoryAttribute.Borrowed = 1）——传输内存 / 代码内存建立、
//   MapMemory 映射时给源置位，还回 / 解除时清位；借作源的入口与缩堆要属性为 0，否则 InvalidMemState（Ryujinx 各入口 CheckRange 的 attributeExpected = None）
static struct perm_ov { uint64_t lo, hi; uint32_t perm; } *g_perm, *g_attr; static int g_nperm, g_nattr;
#define ATTR_BORROWED 1u
static void ov_set(struct perm_ov **pv, int *pn, uint64_t lo, uint64_t hi, int perm) {   // perm < 0：清掉 [lo, hi)
    if (hi <= lo) return;
    struct perm_ov *g = *pv; int gn = *pn;
    if (perm < 0) { int hit = 0; for (int i = 0; i < gn && !hit; i++) hit = g[i].lo < hi && lo < g[i].hi; if (!hit) return; }
    struct perm_ov *v = malloc((size_t)(gn + 3) * sizeof *v); if (!v) die("权限 / 属性账分配失败");
    int n = 0, put = perm < 0;
    for (int i = 0; i < gn; i++) {   // 各段留下 [lo, hi) 左右两边的残段，新段插在左右之间
        struct perm_ov e = g[i];
        if (e.lo < lo) v[n++] = (struct perm_ov){e.lo, e.hi < lo ? e.hi : lo, e.perm};
        if (e.hi > hi) { if (!put) { v[n++] = (struct perm_ov){lo, hi, (uint32_t)perm}; put = 1; } v[n++] = (struct perm_ov){e.lo > hi ? e.lo : hi, e.hi, e.perm}; }
    }
    if (!put) v[n++] = (struct perm_ov){lo, hi, (uint32_t)perm};
    int m = 0;
    for (int i = 0; i < n; i++) { if (m && v[m - 1].hi == v[i].lo && v[m - 1].perm == v[i].perm) v[m - 1].hi = v[i].hi; else v[m++] = v[i]; }
    free(g); *pv = v; *pn = m;
}
static void perm_set(uint64_t lo, uint64_t hi, int perm) { ov_set(&g_perm, &g_nperm, lo, hi, perm); }
static void attr_set(uint64_t lo, uint64_t hi, int attr) { ov_set(&g_attr, &g_nattr, lo, hi, attr); }   // attr < 0：清
static int attr_any(uint64_t lo, uint64_t hi) { for (int i = 0; i < g_nattr; i++) if (g_attr[i].lo < hi && lo < g_attr[i].hi) return 1; return 0; }
// addr 所在、缺省权限为 perm 的一段 [*b, *b + *s)：落在改过的段里回那段权限并截到段内，否则截到相邻改过的段之间（权限不变）
static uint32_t ov_clip(const struct perm_ov *g, int gn, uint64_t addr, uint64_t *b, uint64_t *s, uint32_t perm) {
    uint64_t lo = *b, hi = *b + *s;
    for (int i = 0; i < gn; i++) {
        const struct perm_ov *e = &g[i];
        if (e->hi <= addr) { if (e->hi > lo) lo = e->hi; continue; }
        if (e->lo <= addr) { if (e->lo > lo) lo = e->lo; if (e->hi < hi) hi = e->hi; perm = e->perm; }
        else if (e->lo < hi) hi = e->lo;
        break;
    }
    *b = lo; *s = hi - lo; return perm;
}
static uint32_t perm_clip(uint64_t addr, uint64_t *b, uint64_t *s, uint32_t perm) { return ov_clip(g_perm, g_nperm, addr, b, s, perm); }
// MapMemory 的栈（ST_STACK 带源）映射期间源是借出（Ryujinx KPageTableBase.Map 给源置 Borrowed、Unmap 清）；代码内存 / 传输内存的源在建立对象时借出，这里不管
static void map_add(uint64_t b, uint64_t n, uint64_t src, uint32_t st, uint32_t h) { vm_mutation_check("映射表新增"); perm_set(b, b + n, -1); if (g_nmap >= MAX_MAP) die("映射表满"); g_map[g_nmap] = (typeof(g_map[0])){b, n, src, st, h, 3};
#ifdef __ANDROID__
    g_map_flags[g_nmap] = 0;
#endif
    g_nmap++; map_peak(st, b, n, 1);
    if (st == ST_STACK && src) attr_set(src, src + n, ATTR_BORROWED); }
static void map_del(int i) { vm_mutation_check("映射表删除"); perm_set(g_map[i].base, g_map[i].base + g_map[i].size, -1); map_peak(g_map[i].state, g_map[i].base, g_map[i].size, 0);
    if (g_map[i].state == ST_STACK && g_map[i].src) attr_set(g_map[i].src, g_map[i].src + g_map[i].size, -1);
    g_map[i] = g_map[--g_nmap];
#ifdef __ANDROID__
    g_map_flags[i] = g_map_flags[g_nmap]; g_map_flags[g_nmap] = 0;
#endif
}
// 共享内存别名按整宿主页覆盖映射，页里没请求的尾部在映射表里是空闲的：另一块共享内存若落进同一宿主页会被 VM_FLAGS_OVERWRITE 冲掉。宿主页跨度另记一张表，重叠即拒。
//   每项对应映射表里一项共享内存映射，容量取映射表的 MAX_MAP（审计 #1534 P1-70：原先 64 是迁入时随手定的，Horizon / Ryujinx 没有块数上限，第 65 块直接 die）
static struct { uint64_t lo, hi; } g_span[MAX_MAP]; static int g_nspan;
static int span_busy(uint64_t lo, uint64_t hi) { for (int i = 0; i < g_nspan; i++) if (lo < g_span[i].hi && g_span[i].lo < hi) return 1; return 0; }

// ---------- 别名区 4 KB 子页掩码（MapPhysicalMemory）----------
static uint8_t *g_amask;   // 每宿主页一字节，低 4 位 = 哪几个 4 KB 子页已映射；按别名区大小懒建（calloc 大块走零页，用到才占内存）
#ifdef S2A_UNIT_TEST
static uint64_t g_amask_probes;
// 快照相关的弱缺省已由本文件后部的通用定义提供（合并 upstream 后统一，勿在此重复定义）
#define AMASK_PROBE() (++g_amask_probes)
#else
#define AMASK_PROBE() ((void)0)
#endif
static uint8_t *amask_at(uint64_t a) { AMASK_PROBE(); if (!g_amask) g_amask = calloc(L.alias_size / HPG, 1); return g_amask + (a - L.alias_base) / HPG; }

// ---------- TLR：窗口 TLS/IO 区里按 0x200 一格分配（§5）----------
static uint64_t g_tlr_next; static void *g_tlr_free;
static uint64_t g_user_exception_context, g_det_arena_top;
static uint64_t g_entropy[4]; static int g_entropy_ready;
void *s2a_tlr_alloc(void) {
    GMU_LOCK(); void *p = g_tlr_free;
    if (p) g_tlr_free = *(void **)p;
    else {
        if (!g_tlr_next) g_tlr_next = L.tlsio_base;
        if (g_tlr_next + 0x200 > L.tlsio_base + L.tlsio_size) die("TLS/IO 区放不下更多线程的 TLR");
        if (!(g_tlr_next & (HPG - 1))) GUEST_COMMIT(g_tlr_next, HPG);
        if (!(g_tlr_next & (HPG - 1)) && mprotect((void *)g_tlr_next, HPG, PROT_READ | PROT_WRITE)) die("TLR 页 mprotect errno=%d", errno);
        p = (void *)g_tlr_next; g_tlr_next += 0x200;
    }
    GMU_UNLOCK();
    memset(p, 0, 0x200); return p;
}
void s2a_tlr_free(void *p) { if (!p) return; GMU_LOCK(); *(void **)p = g_tlr_free; g_tlr_free = p; GMU_UNLOCK(); }

// ---------- 起局：读 NPDM、排布局、建主线程栈 ----------
static size_t image_bytes(void) { size_t n = 0; for (int i = 0; i < g_nmod; i++) n += g_mod[i].size; return (n + 0x1fffff) & ~(size_t)0x1fffff; }
// 应用池里堆以外的部分（照 Ryujinx KProcess.GetMemoryUsage）：代码页（各模块原始映像，不含我方补丁区，按 4 KB 取整）
// + 主线程栈 + SystemResourceSize（个人页表堆）+ 物理映射页。S2A_PHYS_LIMIT=0 应急关时回到旧口径（映像按 2 MB 取整）
static uint64_t code_bytes(void) {
    uint64_t n = 0;
    for (int i = 0; i < g_nmod; i++) { const module_t *m = &g_mod[i]; n += ((m->patch_off >= 0 ? (uint64_t)m->patch_off : m->image_size) + 0xfff) & ~0xfffull; }
    return n;
}
// ---------- 应用池按宿主余量扩 ----------
// 应用池（Switch 3285 MB / Switch 2 9 GiB）是在模拟主机的上限。游戏要的超过它、宿主却还有真实余量时，照 Switch 回 ResLimitExceeded 等于为一个
//   物理上不存在的限制让游戏自己中止（#1493 一款游戏开局要约 3340 MB；关掉记账就能玩，宿主撑得住）。改成：超过模拟的池时问宿主还剩多少
//   （Mac：空闲 + 可回收页；iPhone：os_proc_available_memory，即离系统给本 App 的上限还剩多少；安卓：系统可用内存），留 512 MB 不动，
//   给得了就把池扩到这次要用的总量；给不了才回 ResLimitExceeded 并说清是宿主余量不够。报给游戏的总量 / 资源上限跟着扩过的池走（总量 − 已用不为负）。
//   最多扩到模拟的池再加 1/8（Switch 3285 MB → 3695 MB；9 GiB → 10.1 GiB）：按计划要的略超池的游戏放得下（#1493 多要约 55 MB），
//   一直映射到被拒为止的游戏（一款 39 位大游戏每次 1 MB 往上探）到这里照样被拒、自己收手——不设上限时它在 Mac 上把池扩到了 61440 MB（地址窗口那么大），
//   iPhone 上同样一路探下去会被系统杀
#define POOL_HOST_RESERVE (512ull << 20)
static uint64_t g_pool_headroom_div = 8;
static uint64_t g_pool_grown;   // 扩过的池（0 = 没扩过）
static uint64_t pool_eff(void) { return g_pool_grown > APP_POOL ? g_pool_grown : APP_POOL; }
#ifdef S2A_UNIT_TEST
static uint64_t g_test_host_avail;
void s2a_mem_test_host_avail(uint64_t v) { g_test_host_avail = v; g_pool_grown = 0; }
void s2a_mem_test_headroom_div(uint64_t d) { g_pool_headroom_div = d ? d : 8; }
static uint64_t host_avail(void) { return g_test_host_avail; }
#elif defined(__ANDROID__)
static uint64_t host_avail(void) { long n = sysconf(_SC_AVPHYS_PAGES), pg = sysconf(_SC_PAGESIZE); return n > 0 && pg > 0 ? (uint64_t)n * (uint64_t)pg : 0; }
#else
uint64_t s2a_host_mem_avail(void);   // 苹果平台实现 runtime/src/hostmem_apple.c（iPhone：离系统给本 App 的上限还剩多少；Mac：空闲 + 可回收页）
static uint64_t host_avail(void) { return s2a_host_mem_avail(); }
#endif
// 这次之后要用到的总量 need 放得下就回 1（超过模拟的池、宿主给得了时扩池）
static int pool_fits(uint64_t need, uint64_t *avail_out) {
    if (avail_out) *avail_out = 0;
    if (need <= pool_eff()) return 1;
    if (need > APP_POOL + APP_POOL / g_pool_headroom_div) return 0;   // 超出模拟的池 + 1/8：不扩（见上）
    uint64_t extra = need - pool_eff(), av = host_avail(); if (avail_out) *avail_out = av;
    if (av <= POOL_HOST_RESERVE || extra > av - POOL_HOST_RESERVE) return 0;
    uint64_t was = pool_eff(); g_pool_grown = need;
    if (was == APP_POOL || (need >> 28) != (was >> 28))   // 第一次扩、之后每过 256 MB 打一行
        LOG("应用池按宿主余量扩：%llu MB → %llu MB（模拟的池 %llu MB；宿主余量 %llu MB，留 %llu MB 不动）", (unsigned long long)(was >> 20), (unsigned long long)(need >> 20),
            (unsigned long long)(APP_POOL >> 20), (unsigned long long)(av >> 20), (unsigned long long)(POOL_HOST_RESERVE >> 20));
    return 1;
}
// 物理映射的来往（超池那一刻打出来定因用：游戏按什么在规划、超在哪次调用）：次数 / 累计字节 + 最近 16 次（号、地址、大小、调用方）
static uint64_t g_pm_n[2], g_pm_bytes[2];
static struct { int num; uint64_t a, size, lr; } g_pm_ring[16]; static unsigned g_pm_next;
static void pm_note(int num, uint64_t a, uint64_t size, uint64_t lr) {
    int k = num == 0x2C ? 0 : 1; g_pm_n[k]++; g_pm_bytes[k] += size;
    g_pm_ring[g_pm_next % 16].num = num; g_pm_ring[g_pm_next % 16].a = a; g_pm_ring[g_pm_next % 16].size = size; g_pm_ring[g_pm_next % 16].lr = lr; g_pm_next++;
}
#ifdef S2A_UNIT_TEST   // 单测不链 crash.c：调用方只打地址
static void s2a_where(uint64_t a, char *b, size_t n) { snprintf(b, n, "%#llx", (unsigned long long)a); }
#else
void s2a_where(uint64_t a, char *b, size_t n);   // crash.c
#endif
static void pm_overrun_dump(uint64_t lr) {
    char w[160]; s2a_where(lr, w, sizeof w);
    LOG("物理映射超池现场：本次调用方 %s；此前映射 %llu 次共 %llu MB、解除 %llu 次共 %llu MB（报给游戏的总量 %llu MB、已用 %llu MB）",
        w, (unsigned long long)g_pm_n[0], (unsigned long long)(g_pm_bytes[0] >> 20), (unsigned long long)g_pm_n[1], (unsigned long long)(g_pm_bytes[1] >> 20),
        (unsigned long long)((pool_eff() - L.sys_resource_size) >> 20), (unsigned long long)((g_heap_cur + code_bytes() + g_mstack + g_phys) >> 20));
    for (unsigned i = g_pm_next > 16 ? g_pm_next - 16 : 0; i < g_pm_next; i++) {
        s2a_where(g_pm_ring[i % 16].lr, w, sizeof w);
        LOG("物理映射超池现场：  第 %u 次 %s %#llx +%llu KB 调用方 %s", i + 1, g_pm_ring[i % 16].num == 0x2C ? "映射" : "解除",
            (unsigned long long)g_pm_ring[i % 16].a, (unsigned long long)(g_pm_ring[i % 16].size >> 10), w);
    }
}
static uint64_t pool_fixed(void) { return phys_limit_on() ? code_bytes() + g_mstack + L.sys_resource_size + g_phys : image_bytes(); }
static uint64_t mask_minmax(uint32_t lo, uint32_t hi) { uint32_t r = hi - lo + 1; return r >= 64 ? ~0ull : ((1ull << r) - 1) << lo; }
// apply = 0（s2a_npdm_window_need 量窗口跨度）：不写本局专属的 NPDM 状态（优先级 / 缺省核 / 核与优先级掩码 / 系统调用许可）——
//   多款容器构造期要逐款读包内每一款的 NPDM（runtime/src/vawin.c），许可位是按位或累加的，读了别款就混进本局。本局的状态由起局时 layout_prepare / s2a_mem_init 按 apply = 1 读。
//   Switch 2 标志与应用池两种都设（标志是赋值、起局 layout_prepare 按本款重写；池 9 GiB 不回设，容器逐款算完由 vawin.c 放回）：
//   窗口跨度按池算堆区（layout.c S2A_HEAP_DEFAULT），单款构造期的客体内存文件后备判定也读它（vawin.c guest_file_attach）
// Switch 2 款的应用内存池：缺省 9 GiB（native.h S2A_APP_POOL）。诊断开关 S2A_APP_POOL_MB=<MiB> 覆盖（只给排查用，缺省不设 = 照旧）：
//   一款 Switch 2 游戏起局把堆开满到池上限（9156 / 9216 MB）、第 7 帧分配拿到空指针崩，要验「真机池更大、游戏按更大的池规划」这个猜测（参考实现 12 GB 档 9809 MiB）。
//   设了不是正数照旧缺省。纯读环境，t_svc_mem 测
uint64_t s2a_ounce_pool(void) {
    const char *e = getenv("S2A_APP_POOL_MB"); unsigned long long v = e && *e && *e != '-' ? strtoull(e, NULL, 10) : 0;   // strtoull：安卓 VM 夹具链接集合里有它（atoll / strtoll 没有，android/test.sh 端点核对）；负数当没设
    if (v > (1ull << 24)) v = 0;   // 大到不像 MiB 的（溢出 / 乱写）也当没设
    return v ? (uint64_t)v << 20 : 0x240000000ull;
}
static int npdm_read(const char *path, uint32_t *as_type, uint64_t *sysres, uint32_t *main_stack, uint64_t *pid, int apply) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    uint8_t meta[0x80]; size_t n = fread(meta, 1, sizeof meta, f);
    if (n < sizeof meta || memcmp(meta, "META", 4)) { fclose(f); return -1; }
    uint32_t sr, aci; memcpy(&sr, meta + 0x14, 4); memcpy(main_stack, meta + 0x1C, 4); memcpy(&aci, meta + 0x70, 4);
    *as_type = (meta[0x0C] >> 1) & 7; *sysres = sr;
    if (apply) { g_npdm_prio = meta[0x0E]; g_npdm_core = meta[0x0F]; }   // NPDM 头：+0x0E 主线程优先级、+0x0F 缺省核（Ryujinx Npdm.MainThreadPriority / DefaultCpuId）
    // ACI0：+0x10 程序号，+0x30 / +0x34 内核能力（相对 ACI0）。能力字低位连续 1 的个数定类型，3 个 = ThreadInfo：
    // 位 4–9 最高优先级数、10–15 最低优先级数、16–23 最小核、24–31 最大核
    // Switch 2（Ounce）的 ACID 用 RSA-3072 签名：签名 / 公钥各 0x180，魔数在 ACID +0x300（Switch 1 +0x200；switchbrew NPDM）。按它认 Switch 2 进程
    { uint32_t acid; uint8_t mg[4]; memcpy(&acid, meta + 0x78, 4);
      g_npdm_ounce = !fseek(f, (long)acid + 0x300, SEEK_SET) && fread(mg, 1, 4, f) == 4 && !memcmp(mg, "ACID", 4);
      if (g_npdm_ounce) s2a_app_pool_set(s2a_ounce_pool()); }   // Switch 2：应用池 9 GiB（native.h S2A_APP_POOL）；起局第一次读 NPDM（窗口量跨度）就设，布局之前
    uint8_t a[0x38]; *pid = 0;
    if (!fseek(f, aci, SEEK_SET) && fread(a, 1, sizeof a, f) == sizeof a && !memcmp(a, "ACI0", 4)) {
        uint32_t ko, ks; memcpy(pid, a + 0x10, 8); memcpy(&ko, a + 0x30, 4); memcpy(&ks, a + 0x34, 4);
        uint32_t caps[256]; size_t nc = ks / 4 > 256 ? 256 : ks / 4;
        if (apply && !fseek(f, aci + ko, SEEK_SET) && fread(caps, 4, nc, f) == nc)
            for (size_t i = 0; i < nc; i++) {
                uint32_t c = caps[i], lo = ((c + 1) & ~c) - 1;
                if (lo == 7) { g_prio_mask = mask_minmax((c >> 10) & 0x3f, (c >> 4) & 0x3f); g_core_mask = mask_minmax((c >> 16) & 0xff, (c >> 24) & 0xff); }
                // 低位 4 个 1 = EnableSystemCalls：位 5–28 是 24 个号的许可、29–31 是第几组（号 = 组 × 24 + 位）
                else if (lo == 0xf) { uint32_t g = c >> 29, m = (c >> 5) & 0xffffff;
                    for (int b = 0; b < 24; b++) if (m >> b & 1) { int s = (int)(g * 24 + b); g_svc_perm[s >> 6] |= 1ull << (s & 63); } }
            }
    }
    fclose(f); return 0;
}
static const char *npdm_dir(const char *npdm) {   // main.npdm 所在目录（模块目录）
    static char d[1100]; snprintf(d, sizeof d, "%s", npdm); char *sl = strrchr(d, '/'); if (sl) *sl = 0; else snprintf(d, sizeof d, "."); return d; }
// 出包静态提示（addr_need.tsv 的「别名区GB」= tools/addr_need.py static_alias_gb：main 里 AllocateAddressRegion 的常量尺寸，按代码特征认、不跑游戏）：
//   > 0 = 开局在别名区要一整块巨大地址区（GiB，含 SDK 元数据），布局别名优先、按它定别名区承诺（layout.c）；表里「别名区上限 不定」时回负数（不收窗口）。
//   没有文件 / 没有这一行 = 0；程序号对不上报红、按 0
static int alias_first_hint(const char *npdm, uint64_t pid) {
    char p[1100]; snprintf(p, sizeof p, "%s", npdm); char *sl = strrchr(p, '/'); snprintf(sl ? sl + 1 : p, sizeof p - (size_t)(sl ? sl + 1 - p : 0), "addr_need.tsv");
    FILE *f = fopen(p, "r"); if (!f) return 0;
    char line[1024]; unsigned long long alias = 0, tp = 0; int has_pid = 0, unbounded = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "别名区GB\t", strlen("别名区GB\t"))) alias = strtoull(line + strlen("别名区GB\t"), NULL, 10);
        else if (!strncmp(line, "别名区上限\t不定", strlen("别名区上限\t不定"))) unbounded = 1;
        else if (!strncmp(line, "上限不定原因\t", strlen("上限不定原因\t"))) { line[strcspn(line, "\n")] = 0; LOG("布局提示：别名区上限不定，不收窗口（照旧 64 GiB + 远端）——%s", line + strlen("上限不定原因\t")); }
        else if (!strncmp(line, "程序号\t", strlen("程序号\t"))) { tp = strtoull(line + strlen("程序号\t"), NULL, 16); has_pid = 1; }
    }
    fclose(f);
    if (has_pid && tp != pid) { LOG("【红】%s 的程序号 %016llx 与 NPDM %016llx 不符，不用它的布局提示", p, tp, (unsigned long long)pid); return 0; }
    if (alias) LOG("布局提示：出包静态认出开局在别名区要大块地址区（%llu GB），别名优先、堆区压小", alias);
    int gb = alias > 63 ? 63 : (int)alias;   // 39 位别名区最多 64 GiB（addr_need.py ALIAS_MAX_GB）
    return unbounded ? -gb : gb;             // 负数 = 上限不定（layout.c 不收窗口）
}
__attribute__((weak)) void s2a_init_failed(int code) { (void)code; }   // 弱定义：宿主里 host_main.c 的强定义顶替（走初启失败出口、不返回）；只链本文件的单测用这份空的，返回后照旧 die
// 布局只算一次：模块装载（loader.c 要模块段起点）早于 s2a_mem_init，两处都经这里。只看 NPDM 与宿主占到的窗口（layout.c）
static int g_lay_ok; static uint64_t g_mod_base, g_shadow_base, g_lay_need;
// 报给游戏的别名区大小（GetInfo AliasSize，#1441）：0 = 照 L.alias_size。内部布局与映射检查仍按 L.alias_size，只有告诉游戏的值可能收小（layout.c s2a_alias_report_size）
static uint64_t g_alias_report, g_lay_pid;
static int alias_full_path(char *out, size_t n, uint64_t pid) {
    const char *ov = getenv("S2A_ALIAS_FULL"); if (ov && *ov) { snprintf(out, n, "%s", ov); return 1; }   // 单测 / 诊断指定
    return pid ? s2a_docs_path(out, n, "wire/%016llX/alias_full", (unsigned long long)pid) : 0;
}
// 这款真要过的普通堆（字节，十进制一行）：SetHeapSize 超出堆区时记（只增不减），起局布局前读回给 layout.c s2a_layout_heap_floor_set（D-NGH9 / #947）
static int heap_need_path(char *out, size_t n, uint64_t pid) {
    const char *ov = getenv("S2A_HEAP_NEED"); if (ov && *ov) { snprintf(out, n, "%s", ov); return 1; }   // 单测 / 诊断指定
    return pid ? s2a_docs_path(out, n, "wire/%016llX/heap_need", (unsigned long long)pid) : 0;
}
static uint64_t heap_need_read(uint64_t pid) {
    char p[1024]; unsigned long long v = 0; FILE *f = heap_need_path(p, sizeof p, pid) ? fopen(p, "r") : NULL;
    if (f) { if (fscanf(f, "%llu", &v) != 1) v = 0; fclose(f); }
    return v;
}
static int heap_need_note(uint64_t pid, uint64_t size) {   // → 1 = 记下了（比原记录大才写）
    if (size <= heap_need_read(pid)) return 1;
    char p[1024]; FILE *f = heap_need_path(p, sizeof p, pid) ? fopen(p, "w") : NULL;
    return f && fprintf(f, "%llu\n", (unsigned long long)size) > 0 && !fclose(f);
}
static _Atomic int g_heap_noted;   // 这一局 SetHeapSize 超出堆区、已记下要的堆
static _Atomic uint64_t g_heap_noted_size;   // 这一局超出堆区时要过的最大普通堆（字节）
static uint64_t g_lay_heap_floor;            // 这一局布局用的「上一局真要过的普通堆」（heap_need，layout_prepare 读回）
// 堆不够、需求已记下（#947）：自动发起进程内重开一局，新局重算布局时堆区按记下的给（layout.c g_heap_floor），玩家看到多一次加载、不用再点开始。
//   只在记下的比本局布局已按的大时重开（按它给过仍不够 = 不是堆区给小，不再重开，防循环）；不支持进程内重建（安卓）/ 被拒时回 0，
//   调用方照旧报「需要更多内存、已记下、再点一次开始」（S2A_INIT_FAIL_E_HEAP_LEARNED）
int s2a_mem_heap_learned_restart(void) {
    uint64_t need = atomic_load(&g_heap_noted_size);
    if (!atomic_load(&g_heap_noted) || need <= g_lay_heap_floor) return 0;
    int (*restart)(void (*)(int, const char *, void *), void *) = (int (*)(void (*)(int, const char *, void *), void *))dlsym(RTLD_DEFAULT, "s2a_host_restart");
    int rc = restart ? restart(NULL, NULL) : -2;
    if (rc) { LOG("普通堆不够、已记下 %llu MB：进程内重开没受理（%d），改提示再点一次开始", (unsigned long long)(need >> 20), rc); return 0; }
    LOG("普通堆不够、已记下 %llu MB（本局布局按 %llu MB）：自动进程内重开一局，新局堆区按记下的给（#947）", (unsigned long long)(need >> 20), (unsigned long long)(g_lay_heap_floor >> 20));
    return 1;
}
static int alias_full_seen(uint64_t pid) { char p[1024]; FILE *f = alias_full_path(p, sizeof p, pid) ? fopen(p, "r") : NULL; if (f) fclose(f); return f != NULL; }   // 只用 fopen / fclose：安卓 VM 端点集合（check_endpoints.py）
// 收小后游戏照旧映射到收掉的那段（它没按报的大小来，比如写死了 64 GiB）：这一局照常服务（那段在 L.alias_size 内，伸出窗口的走远端，同收小前），
//   报一次红并记下本款，下次起局恢复原值——宁慢不写错
static void alias_report_overrun(uint64_t a, uint64_t size) {
    static _Atomic int said; if (atomic_exchange(&said, 1)) return;
    char p[1024]; int ok = 0;
    if (alias_full_path(p, sizeof p, g_lay_pid)) { FILE *f = fopen(p, "w"); if (f) ok = !fclose(f); }   // 空文件即标记；越界地址在下面这行红里
    LOG("【红】别名区收小后游戏仍映射到收掉的那段（%#llx +%#llx，报给游戏的别名区到 %#llx）：这局照常走远端，%s", (unsigned long long)a, (unsigned long long)size,
        (unsigned long long)(L.alias_base + g_alias_report), ok ? "已记下本款，下次起局别名区恢复原值" : "记录写不进，下次起局仍会收小");
}
static void layout_prepare(const char *npdm) {
    if (g_lay_ok) return;
    uint32_t t, ms; uint64_t sr, pid, wb, ws;
    if (npdm_read(npdm, &t, &sr, &ms, &pid, 1)) die("读不到 NPDM：%s", npdm);
    s2a_window_get(&wb, &ws);
    uint64_t ws_hole = ws;   // 宿主占到的空洞（下面可能按学到的跨度再收）：判「放不下标准布局」用它（#1441）
    char detail[256] = {0};
    int platform_result = plat_layout_override(npdm, t, sr, pid, wb, ws, &L, &g_lay_need, detail, sizeof detail);
    if (!platform_result) {
        if (detail[0]) LOG("地址需求：%s", detail);
        g_alias_report = L.alias_size;
        s2a_window_trim(L.win_size); g_lay_ok = 1; return;
    }
    if (platform_result != ENOTSUP) die("【红】平台地址布局失败：%s", detail);
    uint64_t mod = s2a_module_span(npdm_dir(npdm));
    g_lay_need = s2a_layout_need(t, sr, mod);
    // 多款容器选定游戏后按这一款自己学到的跨度收窗口（#55248c）：构造期还没选游戏，窗口按包内各款学到跨度的最大值给（vawin.c），
    //   一款跨度大的（或没记录、按标准布局算的）会把别款的窗口撑大、宿主被饿——11-16 马车世界三台：本款只碰到 13.6 GB，窗口却按别款 40 GB 给，
    //   宿主只剩 11 GB，虚拟池预留减半到 16 MB 也占不到、Metal 拿到空数据崩。布局按收窄后的大小算，多出的尾部由下面 s2a_window_trim 还给宿主；
    //   只在窗口本来就放不下标准布局（按需档）时收：放得下的款照旧给满标准布局，不因学到的跨度小而改成按需。
    //   收窄后放不下走原有自愈（-2 → 删记录、放宽重算）。单款包构造期已按本款跨度给，这里至多再收 1 GiB 取整的零头
    { uint64_t (*span_seen)(uint64_t) = (uint64_t (*)(uint64_t))dlsym(RTLD_DEFAULT, "s2a_window_span_seen");   // vawin.c；安卓 / 单测没有就不收
      uint64_t (*for_span)(uint64_t) = (uint64_t (*)(uint64_t))dlsym(RTLD_DEFAULT, "s2a_window_for_span");
      uint64_t cap = span_seen && for_span ? for_span(span_seen(pid)) : 0;
      // 即时存档恢复：窗口要与快照时一样（vawin.c 构造期已按快照清单占），学到的跨度不再收（两局之间学到的不同，收了就「窗口不符」）
      { int s2a_snap_restoring(void); if (cap && s2a_snap_restoring()) { LOG("即时存档恢复：不按本款学到的跨度收窗口（窗口保持快照时的 %llu MB）", (unsigned long long)(ws >> 20)); cap = 0; } }
      if (ws < g_lay_need && cap && cap < ws) { LOG("地址窗口：按本款（%016llx）学到的跨度收到 %llu MB（原 %llu MB），其余还给宿主", (unsigned long long)pid, (unsigned long long)(cap >> 20), (unsigned long long)(ws >> 20)); ws = cap; } }
    int alias_gb = alias_first_hint(npdm, pid);
    { void s2a_layout_heap_floor_set(uint64_t); uint64_t hn = heap_need_read(pid); s2a_layout_heap_floor_set(hn); g_lay_heap_floor = hn;   // 上一局真要过的普通堆（见 heap_need_path）
      if (hn) LOG("地址布局：本款上一局要过 %llu MB 普通堆（wire/%016llX/heap_need），堆区至少给到这么多", (unsigned long long)(hn >> 20), (unsigned long long)pid); }
    int rc = s2a_layout_compute(t, sr, mod, alias_gb, wb, ws, &L, &g_mod_base, &g_shadow_base);
    if (rc == -1) die("【红】NPDM 地址空间类型 %u（%s）暂不支持（布局只支持类型 1 = 36 位、3 = 39 位）", t, s2a_as_type_bits(t));
    if (rc == -2) {   // 窗口按学到的跨度收窄了（#783）：当场放宽回缺省给法、重算一次（vawin.c；安卓 / 单测没有这个就不调）
        int (*widen)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "s2a_window_span_widen");
        if (widen && widen()) { s2a_window_get(&wb, &ws); rc = s2a_layout_compute(t, sr, mod, alias_gb, wb, ws, &L, &g_mod_base, &g_shadow_base); }
    }
    if (rc == -2) {   // #446：设备地址空间 / App 内存上限不够（iPad mini 6：2^39 以下只有 6 GB）。原先 die → abort，玩家只看到加载页后闪退；改走初启失败出口，iOS 停在提示页说清原因
        LOG("【红】窗口 %#llx +%llu MB 连模块段（%llu MB）、栈、堆前段都放不下（SystemResourceSize %#llx，标准布局要 %llu MB）：这台设备的地址空间 / 内存不够跑这款", (unsigned long long)wb, (unsigned long long)(ws >> 20),
            (unsigned long long)(mod >> 20), (unsigned long long)sr, (unsigned long long)(g_lay_need >> 20));
        s2a_init_failed(S2A_INIT_FAIL_E_DEVICE_ADDRESS);   // 宿主里不返回
        die("窗口放不下（没链宿主的失败出口）");
    }
    if (L.win_size < g_lay_need && L.alias_base + L.alias_size <= L.win_base + L.win_size)   // 甲，或别名优先的乙放得下静态大区 + 余量（别名区收到窗口末，layout.c）
        LOG("地址按需：标准布局要 %llu MB，宿主占到 %llu MB——别名区 %llu MB 整段在窗口内（静态大区 %d GB），不伸出、不开远端",
            (unsigned long long)(g_lay_need >> 20), (unsigned long long)(L.win_size >> 20), (unsigned long long)(L.alias_size >> 20), alias_gb < 0 ? -alias_gb : alias_gb);
    else if (L.win_size < g_lay_need)
        LOG("地址按需：标准布局要 %llu MB，宿主占到 %llu MB——小区在前、别名区伸出窗口 %llu MB（只是承诺）；游戏映射到宿主没占到的地址回内存不足并报红",
            (unsigned long long)(g_lay_need >> 20), (unsigned long long)(L.win_size >> 20), (unsigned long long)((L.alias_base + L.alias_size - L.win_base - L.win_size) >> 20));
    g_lay_pid = pid;
    { int full = alias_full_seen(pid);
      g_alias_report = s2a_alias_report_size(L.alias_base, L.alias_size, L.win_base + L.win_size, ws_hole, g_lay_need, alias_gb, full);
      if (g_alias_report < L.alias_size)
          LOG("别名区收小到 %llu GB 报给游戏（原 %llu GB）：空洞 %llu MB 放不下标准布局 %llu MB、别名区伸出窗口，收到窗口末让游戏的内存落在窗口里、不走远端缺页代执行（#1441）",
              (unsigned long long)(g_alias_report >> 30), (unsigned long long)(L.alias_size >> 30), (unsigned long long)(ws_hole >> 20), (unsigned long long)(g_lay_need >> 20));
      else if (full && s2a_alias_report_size(L.alias_base, L.alias_size, L.win_base + L.win_size, ws_hole, g_lay_need, alias_gb, 0) < L.alias_size)
          LOG("【黄】别名区不收小：本款上次收小后仍映射到收掉的那段（wire/%016llX/alias_full），这局照旧报 %llu GB、伸出段走远端", (unsigned long long)pid, (unsigned long long)(L.alias_size >> 30)); }
    s2a_window_trim(L.win_size);   // 布局之外的窗口尾部交给宿主（vawin.c）
    if (g_shadow_base) s2a_far_set(L.win_base + L.win_size, L.alias_base + L.alias_size, L.heap_base, L.heap_base + L.heap_size);   // 乙：别名区伸出窗口，伸出段走远端影子（far.c）
    g_lay_ok = 1;
}
uint64_t s2a_mem_mod_base(const char *npdm) { layout_prepare(npdm); return g_mod_base; }
// 36 位老 SDK 兜底点（traps.c，#1503）：老 SDK 写死的两段 = 新 SDK 问 GetInfo 拿到的可随机区 / 栈区，这里给同一组值（同下面 GetInfo 0x0C–0x0F）
int s2a_mem_as_region(int stack, uint64_t *lo, uint64_t *hi) {
    if (!g_lay_ok) return -1;
    *lo = stack ? L.stack_base : L.aslr_base; *hi = *lo + (stack ? L.stack_size : L.aslr_size);
    return 0;
}
// 本局已碰到的地址跨度（#783）：同「地址按需」行的「本局所需跨度」口径（碰到的最高地址 − 窗口基址），但只读局内峰值、不扫别名区位图，心跳每 10 秒调也便宜：
//   别名区高水位、堆峰值、栈区 / 可随机区用到的最高偏移、模块末（在窗口内的）。vawin.c 记进 wire/<程序号>/span_peak，下一局窗口放不下标准布局时按它给窗口。布局没定 = 0
uint64_t s2a_mem_span_now(void) {
    if (!g_lay_ok) return 0;
    uint64_t e[5]; int n = 0, mm = 0; uint64_t mtop = 0;
    if (g_pk_alias_in) e[n++] = L.alias_base + g_pk_alias_in;
    if (g_pk_heap) e[n++] = (uint64_t)g_heap + g_pk_heap;
    if (g_pk_st_end) e[n++] = L.stack_base + g_pk_st_end;
    if (g_pk_sh_end) e[n++] = L.aslr_base + g_pk_sh_end;
    for (int i = 0; i < g_nmod; i++) { uint64_t m = (uint64_t)g_mod[i].base + g_mod[i].size; if (m <= L.win_base + L.win_size && m > mtop) { mtop = m; mm = 1; } }   // 模块：窗口内的最高末端
    if (mm) e[n++] = mtop;
    return s2a_span_pick(L.win_base, g_span_lim, e, n);
}
// 本款标准布局的窗口大小（vawin.c 构造函数占窗口用，早于 s2a_mem_init）：读不到 0
uint64_t s2a_npdm_window_need(const char *npdm) {
    uint32_t t, ms; uint64_t sr, pid;
    if (npdm_read(npdm, &t, &sr, &ms, &pid, 0)) return 0;
    uint64_t mod = s2a_module_span(npdm_dir(npdm));
    return mod ? s2a_layout_need(t, sr, mod) : 0;   // 读不到模块 = 不知道要多大
}
// 本款应用池（vawin.c 多款容器「装游戏前按本款再判」文件后备时按名字取，#964）：那一刻已选定游戏、模块目录已是这一款，但布局还没按本款排、
//   s2a_app_pool 仍是容器缺省 3285 MiB——一款 Switch 2 大款（0400486024DEC000） 会被判成「池 ≤ 剩余」照旧匿名、首局被杀。按本款 NPDM 认 Switch 2 → 9 GiB，否则缺省；读不到 0
uint64_t s2a_npdm_app_pool(const char *npdm) {
    uint32_t t, ms; uint64_t sr, pid;
    if (npdm_read(npdm, &t, &sr, &ms, &pid, 0)) return 0;
    return g_npdm_ounce ? s2a_ounce_pool() : 0xCD500000ull;
}
// 本款程序号（vawin.c 构造函数占窗口时按名字取）：同 s2a_npdm_window_need 的不写本局状态读法（apply = 0）；读不到 / 没有 ACI0 为 0。
//   审计 P1-139：构造期客体内存文件后备要按程序号找本款实测峰值 / 「上一局前台死掉」标记，那时图形模式还没落定，不能经 s2a_npdm_program_id → s2a_gfx_mode
uint64_t s2a_npdm_pid(const char *npdm) {
    uint32_t t, ms; uint64_t sr, pid;
    return npdm_read(npdm, &t, &sr, &ms, &pid, 0) ? 0 : pid;
}
// 装模块前认出本款是不是 Switch 2：1 = 是、0 = 不是、−1 = 读不到 NPDM。装载器重定位时 s2a_direct_lookup 逐个导入问直连图形层（nvn2.c nvn2_on = s2a_platform_ounce），
//   Switch 2 的 SDK 接管要靠这个标志。苹果侧 load_modules 经 s2a_mem_mod_base → layout_prepare 在重定位前读过 NPDM；安卓 APK 装载（load_modules_from_source）
//   不经那条路，标志要等装完模块的 s2a_mem_init 才有，重定位时恒为 0——runtime.c 在装模块前调一次。
//   同 s2a_npdm_pid 的 apply = 0 读法：npdm_read 里 Switch 2 标志与应用池两样都设（见上），其余 NPDM 状态（优先级 / 核 / 系统调用许可）留给 s2a_mem_init。
int s2a_npdm_ounce(const char *npdm) {
    uint32_t t, ms; uint64_t sr, pid;
    return npdm_read(npdm, &t, &sr, &ms, &pid, 0) ? -1 : g_npdm_ounce;
}
void s2a_thread_set_caps(uint64_t core_mask, uint64_t prio_mask);   // thread.c：CreateThread 按 NPDM 核 / 优先级掩码校验
void s2a_thread_set_npdm(int def_core, int main_prio);              // thread.c：NPDM 缺省核 / 主线程优先级（S2A_SCHED_HZ 用）
// 返回主线程栈顶。主线程栈放栈区最高处、登记成 Stack（SDK 对自己的 sp 做 QueryMemory 求主线程栈区间）
uint64_t s2a_mem_init(const char *npdm) {
    uint32_t t, ms; uint64_t sr; uint64_t wb, ws;
    if (npdm_read(npdm, &t, &sr, &ms, &g_program_id, 1)) die("读不到 NPDM：%s", npdm);
    if (!g_program_id) die("NPDM 里没有 ACI0 程序号：%s", npdm);
    if (!g_core_mask || !g_prio_mask) die("NPDM 内核能力里没有 ThreadInfo 项（核 / 优先级掩码）：%s", npdm);
    layout_prepare(npdm); wb = L.win_base; ws = L.win_size;
#ifdef __ANDROID__
    // 背板必须先于任何 guest 页提交：open 自检失败直接红退，不退回共享匿名映射（owner r3 Q1-Q5 裁定）。
    { int be = plat_backing_open(backing_limit_end()); if (be) die("背板 memfd 不可用 errno=%d（不回退匿名映射）", be); }
    vma_report_start();
    // 镜像生产开关缺省关（s2arm.mirror=off）：入口第一行判一次标志即返回，不建窗、不分配（tracking/platform.c）。
    s2a_android_mirror_init((uintptr_t)wb, (size_t)ws);
    s2a_android_mirror_report("开局");
#endif
    { uint64_t ht, hl; s2a_low_holes(&ht, &hl);   // 维护者 10-19：起局打窗口与低区余量（Metal 大缓冲进窗口余量、≤256 MB 小缓冲进低区）
      LOG("地址余量：窗口 %#llx 起布局用 %llu MB（标准布局要 %llu MB）；低区空洞合计 %llu MB、最大一块 %llu MB",
          (unsigned long long)wb, (unsigned long long)(ws >> 20), (unsigned long long)(g_lay_need >> 20), (unsigned long long)(ht >> 20), (unsigned long long)(hl >> 20)); }
    g_heap = (uint8_t *)L.heap_base;
    LOG("NPDM：核掩码 %#llx 优先级掩码 %#llx", (unsigned long long)g_core_mask, (unsigned long long)g_prio_mask);
    s2a_thread_set_caps(g_core_mask, g_prio_mask);
    { int s2a_svc_check_perm(const uint64_t perm[3], char *out, size_t cap); char b[600]; int np = 0;
      for (int i = 0; i < 3; i++) np += __builtin_popcountll(g_svc_perm[i]);
      int s2a_svc_ryu_missing(int); char kb[64]; size_t kk = 0; kb[0] = 0;
      for (int i = 0; i < 0xC0; i++) if ((g_svc_perm[i >> 6] >> (i & 63) & 1) && s2a_svc_ryu_missing(i) && kk < sizeof kb) kk += (size_t)snprintf(kb + kk, sizeof kb - kk, " %#x", i);
      int nu = s2a_svc_check_perm(g_svc_perm, b, sizeof b);
      if (nu) LOG("【红】NPDM 许可了 %d 个系统调用，其中 %d 个我们没实现（游戏可能会调）：%s", np, nu, b);
      else LOG("NPDM 许可 %d 个系统调用，除真值（Ryujinx）也没有的%s 外全部已实现", np, kk ? kb : " 无"); }
    if (g_npdm_ounce) s2a_thread_set_hw_cores(8);   // Switch 2：8 核（游戏用 NPDM 掩码里的 0–5），核号 4 / 5 合法
    s2a_thread_set_npdm(g_npdm_core, g_npdm_prio);
    // 系统计数器频率随 SDK 平台：Switch 2 SDK 的 nn::os::GetSystemTickFrequency 是常量 31.25 MHz（0x1dcd650），自己读 cntpct 按它换算；
    //   宿主仍按 19.2 MHz 给，游戏量出的时长短 1.63 倍、帧率控制器把交换间隔抬到 2–3（the reference port docs/reference/frame-rate.md，MIT）
    if (g_npdm_ounce) { void s2a_guest_tick_hz_set(uint64_t); s2a_guest_tick_hz_set(31250000); LOG("NPDM 是 Switch 2 布局：系统计数器按 31.25 MHz"); }
    LOG("布局：程序号 %016llx 类型 %u SystemResourceSize %#llx｜别名 %#llx+%#llx 堆 %#llx+%#llx 栈 %#llx+%#llx TLS/IO %#llx+%#llx 可随机 %#llx+%#llx",
        (unsigned long long)g_program_id, t, (unsigned long long)sr, (unsigned long long)L.alias_base, (unsigned long long)L.alias_size, (unsigned long long)L.heap_base, (unsigned long long)L.heap_size, (unsigned long long)L.stack_base, (unsigned long long)L.stack_size, (unsigned long long)L.tlsio_base, (unsigned long long)L.tlsio_size, (unsigned long long)L.aslr_base, (unsigned long long)L.aslr_size);
#if !defined(__ANDROID__)
    { void (*carve)(uint64_t, uint64_t) = (void (*)(uint64_t, uint64_t))dlsym(RTLD_DEFAULT, "s2a_guest_file_carve"); if (carve) carve(L.alias_base, L.alias_size); }   // 按名字取：只链本文件的单测没有 vawin.c   // 文件后备时物理映射区（GPU 每帧写的内存池）保持匿名（vawin.c，#964 a）
#endif
    if (!ms) die("NPDM MainThreadStackSize 为 0");
    uint64_t len = ((uint64_t)ms + HPG - 1) & ~(HPG - 1), base = L.stack_base + L.stack_size - len;
    GUEST_COMMIT(base, len);
    if (mprotect((void *)base, len, PROT_READ | PROT_WRITE)) die("主线程栈 mprotect errno=%d", errno);
    map_add(base, ms, 0, ST_STACK, 0);
    g_mstack = ((uint64_t)ms + 0xfff) & ~0xfffull;
    LOG("主线程栈 %#llx +%#x", (unsigned long long)base, ms);
    return base + ms;
}

// ---------- QueryMemory ----------
// 模块映像按真机分段：text = Code R-X，rodata = Code R，data+bss = CodeData RW；改写补丁区报 Free（SDK 的模块枚举不该把它当模块）
static int module_region(uint64_t addr, uint64_t *base, uint64_t *size, uint32_t *state, uint32_t *perm) {
    for (int i = 0; i < g_nmod; i++) {
        const module_t *m = &g_mod[i]; uint64_t b = (uint64_t)m->base;
        uint64_t img_end = m->patch_off >= 0 ? (uint64_t)m->patch_off : m->image_size;
        uint64_t plo = b + (uint64_t)m->patch_off, phi = m->patch_off >= 0 ? b + m->size : plo + m->patch_len;
        if (addr >= plo && addr < phi) { *base = plo; *size = phi - plo; *state = ST_FREE; *perm = 0; return 1; }
        if (m->patch2_len && addr >= b + (uint64_t)m->patch2_off && addr < b + (uint64_t)m->patch2_off + m->patch2_len) {   // 双补丁岛的另一个岛同样报 Free
            *base = b + (uint64_t)m->patch2_off; *size = m->patch2_len; *state = ST_FREE; *perm = 0; return 1; }
        if (addr < b || addr >= b + img_end) continue;
        uint64_t o = addr - b;
        if (o < m->ro_off) { *base = b; *size = m->ro_off; *state = ST_CODE; *perm = 5; }
        else if (o < m->data_off) { *base = b + m->ro_off; *size = m->data_off - m->ro_off; *state = ST_CODE; *perm = 1; }
        else { *base = b + m->data_off; *size = img_end - m->data_off; *state = ST_CODEDATA; *perm = 3; }
        return 1;
    }
    return 0;
}
// 不属于任何已知映射的地址：回整段空闲（到相邻已知映射为界，不回 4 KB 一块——SDK 逐块走地址空间时旧库实测 20 亿次 QueryMemory）。
// 窗口内：以堆、映射表、别名区已映射子页、模块为界；窗口外：模块之间是空闲，2^39 以上不可访问
#ifdef __ANDROID__
// 背板 memfd 大小：pgoff 等于 guest 地址，必须覆盖整段可提交的 guest 地址。
//   free_span 的 AS_END 算式保持 main 原样（计划 §2.5.10），这里不复用 / 不改它。
static uint64_t backing_limit_end(void) {
    uint64_t alias_end = L.alias_base + L.alias_size;
    return alias_end > (1ull << 39) ? alias_end : 1ull << 39;
}
// VMA 采样器（#1217 §2.5.9）：缺省关；主机 S2A_VMA_REPORT=1，设备 debug.s2a.vma=1。
// 只读 /proc/self/maps 与 max_map_count，用来和 #1205 的 20 s 21,578 行基线同图对照。
static int vma_report_enabled(void) {
    static int value = -1;
    if (value < 0) {
        const char *e = getenv("S2A_VMA_REPORT");
        value = e && *e == '1';
#if defined(__ANDROID__) && __has_include(<sys/system_properties.h>)
        if (!value) { char b[PROP_VALUE_MAX] = {0}; value = __system_property_get("debug.s2a.vma", b) == 1 && b[0] == '1'; }
#endif
    }
    return value;
}
#ifdef S2A_UNIT_TEST
static uint64_t g_vma_test_last[6];   // total/guest/memfd/mirror/far/host，主机用例读取
void s2a_android_vma_test_last(uint64_t out[6]) { memcpy(out, g_vma_test_last, sizeof g_vma_test_last); }
#endif
void s2a_android_vma_report(const char *when) {
    if (!vma_report_enabled()) return;
    // 解析 /proc/self/maps 绝不持 g_mu（正文承诺；s2a_guest_va_report 已在放锁后才调本函数）：
    // 同一线程已持 g_mu 时直接中止，避免「解析 maps 持锁」的回归悄悄回来。
    if (s2a_mem_g_mu_held()) die("VMA 采样在持 g_mu 时解析 maps（调用方违约）");
    // 只把需要读的 VM 状态在锁内抄一份；/proc/self/maps 可能有 2 万行，解析时绝不持 g_mu
    //（否则会扰动它自己要测的 VMA / 事务时序）。
    if (!GMU_TRYLOCK()) return;   // 别的线程正在改表：本次采样跳过
    uint64_t win_lo = L.win_base, win_hi = L.win_base + L.win_size;
    uint64_t alias_lo = L.alias_base, alias_hi = L.alias_base + L.alias_size;
    uintptr_t mb = 0; size_t ms = 0;
    s2a_android_mirror_region(&mb, &ms);
    GMU_UNLOCK();
    // far 只数 guest 自己的映射越出 guest 窗口：背板 memfd（memfd:s2a-guest）或别名区内的映射。
    // 窗口上方的宿主映射（anon / kgsl / dalvik 等）单列 host，不再算进 far（8E 冒烟：宿主映射 ~8k 行）。
    uint64_t total = 0, guest = 0, memfd = 0, mirror = 0, far = 0, host = 0;
    int maps_ok = 0;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char *line = NULL; size_t cap = 0; maps_ok = 1;
        while (getline(&line, &cap, maps) >= 0) {
            unsigned long lo, hi;
            if (sscanf(line, "%lx-%lx", &lo, &hi) != 2) { maps_ok = 0; break; }
            int is_memfd = strstr(line, "memfd:s2a-guest") != NULL;
            int in_alias = alias_hi > alias_lo && lo < alias_hi && hi > alias_lo;
            total++;
            if (lo < win_hi && hi > win_lo) guest++;
            if (is_memfd) memfd++;
            if (mb && lo < mb + ms && hi > mb) mirror++;
            if (hi > win_hi) {
                if (is_memfd || in_alias) far++;
                else host++;
            }
        }
        free(line); fclose(maps);
    }
#ifdef S2A_UNIT_TEST
    g_vma_test_last[0] = total; g_vma_test_last[1] = guest; g_vma_test_last[2] = memfd;
    g_vma_test_last[3] = mirror; g_vma_test_last[4] = far; g_vma_test_last[5] = host;
#endif
    // app 内 SELinux 拒绝读 /proc/sys/vm/max_map_count：如实打「不可读」，不兜底造值。
    unsigned long maximum = 0; int max_ok = 0;
    FILE *limit = fopen("/proc/sys/vm/max_map_count", "r");
    if (limit) { max_ok = fscanf(limit, "%lu", &maximum) == 1; fclose(limit); }
    if (!maps_ok) { LOG("[vma] [%s] 不可读 /proc/self/maps errno=%d", when, errno); return; }
    if (max_ok) LOG("[vma] [%s] total=%llu guest=%llu memfd=%llu mirror=%llu far=%llu host=%llu max=%lu", when,
                    (unsigned long long)total, (unsigned long long)guest, (unsigned long long)memfd,
                    (unsigned long long)mirror, (unsigned long long)far, (unsigned long long)host, maximum);
    else LOG("[vma] [%s] total=%llu guest=%llu memfd=%llu mirror=%llu far=%llu host=%llu max=不可读", when,
             (unsigned long long)total, (unsigned long long)guest, (unsigned long long)memfd,
             (unsigned long long)mirror, (unsigned long long)far, (unsigned long long)host);
}
static void *vma_report_thread(void *arg) {
    (void)arg;
    for (;;) {
        struct timespec pause = {10, 0};
        nanosleep(&pause, NULL);
        s2a_android_vma_report("周期");   // 内部只在抄状态时短暂拿 g_mu，解析 maps 不持锁
    }
    return NULL;
}
static _Atomic int g_vma_thread_started;
static void vma_report_start(void) {
    if (!vma_report_enabled()) return;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_vma_thread_started, &expected, 1)) return;
    pthread_t thread;
    if (pthread_create(&thread, NULL, vma_report_thread, NULL)) { atomic_store(&g_vma_thread_started, 0); LOG("[vma] 采样线程起不来"); return; }
    pthread_detach(thread);
}
#endif
static void free_span(uint64_t addr, uint64_t *base, uint64_t *size, uint32_t *state) {
    // 地址空间上界：本款类型的位宽（36 位款 2^36，39 位款 2^39，as_type.h）；别名区承诺末端越过它时（只在 Mac 上 macOS 27 模拟 iOS 26 时出现，真机别名区末远低于它）按别名区末
    //   36 位款窗口放在 2^36 以上那一块时（#1419，as_type.h s2a_as_type_end）按那一块的末端
    const uint64_t AS_TOP = L.as_type == 1 && L.win_base >= 1ull << 36 && s2a_as_type_end(1, L.win_base, L.win_size) ? s2a_as_type_end(1, L.win_base, L.win_size)
                          : s2a_as_type_top(L.as_type) ? s2a_as_type_top(L.as_type) : 1ull << 39;
    const uint64_t AS_END = L.alias_base + L.alias_size > AS_TOP ? L.alias_base + L.alias_size : AS_TOP;
    if (addr >= AS_END) { *base = AS_END; *size = 0 - AS_END; *state = ST_INACCESSIBLE; return; }
    uint64_t lo = 0, hi = AS_END;
#define EDGE(b_, e_) do { uint64_t B = (b_), E = (e_); if (B == E) break; if (E <= addr && E > lo) lo = E; if (B > addr && B < hi) hi = B; } while (0)
    for (int i = 0; i < g_nmod; i++) { const module_t *m = &g_mod[i]; uint64_t b = (uint64_t)m->base; EDGE(b, b + (m->patch_off >= 0 ? (uint64_t)m->patch_off : m->image_size)); }
    EDGE((uint64_t)g_heap, (uint64_t)g_heap + g_heap_cur);
    for (int i = 0; i < g_nmap; i++) EDGE(g_map[i].base, g_map[i].base + g_map[i].size);
    // 窗口起止是真边界（原先写成零长度的 EDGE，被「B == E 跳过」吞掉：窗口里的空闲段一路并到窗口下方的宿主内存，
    // 八方2 的 SDK 地址区分配据此探到窗口外，10-20 M3 摸底）。窗口外除模块外都是宿主自己的内存：回不可访问，游戏不该在那里分配
    // 别名区伸出宿主窗口时（乙布局，layout.c），伸出去的那段是地址承诺、照 Horizon 报空闲：SDK 的 AllocateAddressRegion 靠 QueryMemory 找整段空闲
    //   （八方2 要 62 GB 一段），截在窗口末尾它就找不到。游戏真往那里映射时 svc 回内存不足（win_has）
    uint64_t we = L.win_base + L.win_size;
    if (L.alias_base < we && L.alias_base + L.alias_size > we) we = L.alias_base + L.alias_size;
    if (addr < L.win_base) { if (L.win_base < hi) hi = L.win_base; }
    else if (addr >= we) { if (we > lo) lo = we; }
    else { if (L.win_base > lo) lo = L.win_base; if (we < hi) hi = we; }
#undef EDGE
    *base = lo; *size = hi - lo; *state = (addr >= L.win_base && addr < we) ? ST_FREE : ST_INACCESSIBLE;
}
static void query(uint64_t addr, s2a_meminfo *o) {   // 持 g_mu
    uint64_t base, size; uint32_t state, perm = 0; int k;
    memset(o, 0, sizeof *o);
    if (addr >= (uint64_t)g_heap && addr < (uint64_t)g_heap + g_heap_cur) { base = (uint64_t)g_heap; size = g_heap_cur; state = ST_HEAP; perm = 3; }
    else if ((k = map_find(addr, 1))) { base = g_map[k - 1].base; size = g_map[k - 1].size; state = g_map[k - 1].state; perm = g_map[k - 1].perm; }
    else if (module_region(addr, &base, &size, &state, &perm)) { }
    // TLS/IO 区里已分配的 TLR 页（4 KB 一页、8 格）：照 Horizon / Ryujinx 报 ThreadLocal、读写（原先报空闲，SDK 可能往里挑址）
    else if (g_tlr_next && addr >= L.tlsio_base && addr < ((g_tlr_next + 0xfff) & ~0xfffull)) { base = L.tlsio_base; size = ((g_tlr_next + 0xfff) & ~0xfffull) - L.tlsio_base; state = ST_THREADLOCAL; perm = 3; }
    else if (addr >= L.alias_base && addr < L.alias_base + L.alias_size && g_amask) {   // 别名区：按 4 KB 子页并段（已映射 = Heap RW，同 Ryujinx MapPhysicalMemory）
        uint64_t p = addr & ~0xfffull; int on = *amask_at(p) >> ((p & (HPG - 1)) >> 12) & 1;
        uint64_t lo = p, hi = p + 0x1000;
        // 并段不设距离上限（原先各向最多 1 GB：SDK 的 nn::os::AllocateAddressRegion 靠 QueryMemory 找整段空闲，
        // 八方2 要 62 GB 一段，只看到 ≤2 GB 的空闲段 → ResultOutOfVirtualAddressSpace 退出，10-20 M3 摸底）。整页（16 KB 一字节）相同就整页跳
        const uint8_t full = (uint8_t)((1u << (HPG >> 12)) - 1), want = on ? full : 0;
        while (lo > L.alias_base) {
            if (!(lo & (HPG - 1)) && lo - HPG >= L.alias_base && *amask_at(lo - HPG) == want) { lo -= HPG; continue; }
            if ((*amask_at(lo - 0x1000) >> (((lo - 0x1000) & (HPG - 1)) >> 12) & 1) != on) break;
            lo -= 0x1000;
        }
        while (hi < L.alias_base + L.alias_size) {
            if (!(hi & (HPG - 1)) && hi + HPG <= L.alias_base + L.alias_size && *amask_at(hi) == want) { hi += HPG; continue; }
            if ((*amask_at(hi) >> ((hi & (HPG - 1)) >> 12) & 1) != on) break;
            hi += 0x1000;
        }
        if (on) { base = lo; size = hi - lo; state = ST_HEAP; perm = 3; }
        else { free_span(addr, &base, &size, &state); if (base < lo) base = lo; if (base + size > hi) size = hi - base; }
    }
    else free_span(addr, &base, &size, &state);
    uint32_t attr = 0;
    if (state != ST_FREE && state != ST_INACCESSIBLE) { if (g_nperm) perm = perm_clip(addr, &base, &size, perm); if (g_nattr) attr = ov_clip(g_attr, g_nattr, addr, &base, &size, 0); }
    o->base = base; o->size = size; o->state = state; o->attr = attr; o->perm = perm;
}
int s2a_mem_query(uint64_t addr, s2a_meminfo *out) { GMU_LOCK(); query(addr, out); GMU_UNLOCK(); return 0; }

// 诊断只记进程内首个失败；状态来自已有账本，不代表宿主内核页表。
typedef struct {
    uint64_t dst, src, size, result, pc, stack_base, stack_size;
    uintptr_t thread;
    int host_errno, snapshot, guest_result;
    const char *reason, *sample_time;
    s2a_meminfo src_page, dst_page;
} map_failure;
static atomic_flag g_map_failure_seen = ATOMIC_FLAG_INIT;
static int map_failure_claim(void) {
    const char *e = getenv("S2A_MAP_DIAG");
    return e && !strcmp(e, "1") && !atomic_flag_test_and_set(&g_map_failure_seen);
}
// 调用方持 g_mu。只采样地址所在的单个 4 KB 页，不扫描整段别名区。
static void map_failure_page(uint64_t addr, s2a_meminfo *o) {
    memset(o, 0, sizeof *o);
    o->base = addr & ~0xfffull; o->size = 0x1000;
    int k = addr == UINT64_MAX ? 0 : map_find(addr, 1);
    if (addr >= (uint64_t)g_heap && addr - (uint64_t)g_heap < g_heap_cur) {
        o->state = ST_HEAP; o->perm = 3;
    } else if (k) {
        o->base = g_map[k - 1].base; o->size = g_map[k - 1].size;
        o->state = g_map[k - 1].state; o->perm = g_map[k - 1].perm;
    } else if (module_region(addr, &o->base, &o->size, &o->state, &o->perm)) {
    } else if (g_tlr_next && addr >= L.tlsio_base && addr < ((g_tlr_next + 0xfff) & ~0xfffull)) {
        o->state = ST_THREADLOCAL; o->perm = 3;
    } else if (addr >= L.alias_base && addr - L.alias_base < L.alias_size && g_amask) {
        int on = (*amask_at(addr) >> ((addr & (HPG - 1)) >> 12)) & 1;
        o->state = on ? ST_HEAP : ST_FREE; o->perm = on ? 3 : 0;
    } else {
        uint64_t b, s; free_span(addr, &b, &s, &o->state);
    }
}
static void map_failure_snapshot(map_failure *f, const char *when) {
    f->snapshot = 1; f->sample_time = when; f->stack_base = L.stack_base; f->stack_size = L.stack_size;
    map_failure_page(f->src, &f->src_page); map_failure_page(f->dst, &f->dst_page);
}
static void map_failure_log(const map_failure *f) {
    char result[32];
    if (f->guest_result) snprintf(result, sizeof result, "%#llx", (unsigned long long)f->result);
    else snprintf(result, sizeof result, "none");
    LOG("MapMemory首失败：原因=%s result=%s errno=%d pc=%#llx thread=%#llx dst=%#llx src=%#llx size=%#llx 账本可用=%d 采样=%s 栈=%#llx+%#llx 源账本=%#llx+%#llx/state=%#x/perm=%#x 目标账本=%#llx+%#llx/state=%#x/perm=%#x",
        f->reason, result, f->host_errno, (unsigned long long)f->pc,
        (unsigned long long)f->thread, (unsigned long long)f->dst, (unsigned long long)f->src,
        (unsigned long long)f->size, f->snapshot, f->sample_time ? f->sample_time : "unavailable", (unsigned long long)f->stack_base, (unsigned long long)f->stack_size,
        (unsigned long long)f->src_page.base, (unsigned long long)f->src_page.size, f->src_page.state, f->src_page.perm,
        (unsigned long long)f->dst_page.base, (unsigned long long)f->dst_page.size, f->dst_page.state, f->dst_page.perm);
}
static map_failure map_failure_new(uint64_t dst, uint64_t src, uint64_t size, uint64_t result, uint64_t pc, int host_errno, const char *reason) {
    return (map_failure){.dst=dst, .src=src, .size=size, .result=result, .pc=pc,
                         .thread=(uintptr_t)pthread_self(), .host_errno=host_errno, .guest_result=(result != 0), .reason=reason};
}
#ifdef S2A_UNIT_TEST
void s2a_mem_test_map_diag_reset(void) { atomic_flag_clear(&g_map_failure_seen); }
void s2a_mem_test_stack(uint64_t base, uint64_t size) { L.stack_base = base; L.stack_size = size; }
#endif

// [addr, addr + size) 是否全已映射（与「逐段 QueryMemory、遇权限 0 即否」同判）：只看区间涉及的堆 / 映射表 / 模块 / 别名子页，不并段。
// 10-21 帧率能耗线采样：C# 的 IsRangeMapped 每次托管写都调（暗黑3 计数结果回调每帧约 18 次），原先经 s2a_mem_query 在别名区把相邻子页一路并成整段，
// S2A.GPU 线程上 20 秒 622 ms。步数 = 区间涉及的已映射子页数（整宿主页全映射一步跳 16 KB）
static uint64_t g_mapped_steps;   // mapped 走过的步数（持 g_mu 加）：单测比步数判「一步到位」，不比耗时（11-19：比耗时 20 倍在负载高时失真）
static int mapped(uint64_t a, uint64_t end) {   // 持 g_mu
    while (a < end) {
        g_mapped_steps++;
        uint64_t base, size; uint32_t state, perm; int k;
        if (a >= (uint64_t)g_heap && a < (uint64_t)g_heap + g_heap_cur) { a = (uint64_t)g_heap + g_heap_cur; continue; }
        if ((k = map_find(a, 1))) { a = g_map[k - 1].base + g_map[k - 1].size; continue; }
        if (module_region(a, &base, &size, &state, &perm)) { if (!perm || !size) return 0; a = base + size; continue; }
        if (a >= L.alias_base && a < L.alias_base + L.alias_size && g_amask) {
            const uint8_t full = (uint8_t)((1u << (HPG >> 12)) - 1); uint8_t m = *amask_at(a);
            if (m == full) { a = (a & ~(HPG - 1)) + HPG; continue; }
            if (!(m >> ((a & (HPG - 1)) >> 12) & 1)) return 0;
            a = (a & ~0xfffull) + 0x1000; continue;
        }
        return 0;
    }
    return 1;
}
uint64_t s2a_mem_mapped_steps(void);
uint64_t s2a_mem_mapped_steps(void) { GMU_LOCK(); uint64_t v = g_mapped_steps; GMU_UNLOCK(); return v; }
int s2a_mem_mapped(uint64_t addr, uint64_t size) {
    if (addr + size < addr) return 0;
    GMU_LOCK(); int r = mapped(addr, addr + size); GMU_UNLOCK(); return r;
}
// IPC 缓冲描述符的地址只有 39 位（HIPC：低 32 位 + 4 位 + 3 位）。macOS 上地址窗口是 0x70_0000_0000 那块 64 GiB 空洞、顶到 2^39，
//   放不下标准布局的款别名区伸出窗口、落在 2^39 之上（远端，far.c）；游戏把那里的缓冲传给服务，第 39 位在 SDK 打包描述符时就丢了
//   （10-29 普查瓦力欧制造：读盘缓冲在伸出段，IStorage.Read 读写到错地址 → Break）。服务层解码描述符后调这里补回第 39 位：
//   只在「不是 0、本身不是游戏地址、补上第 39 位正好是游戏已映射的地址」时补——macOS 上窗口以下是宿主自己的内存，不会是游戏地址，没有歧义；
//   iOS 的窗口在低位、2^39 之上没有游戏地址，原样返回
static int in_as(uint64_t a, uint64_t n);
//   36 位款窗口放在 2^36 以上那一块时（#1419，as_type.h s2a_as_type_end）：老 SDK 打包描述符只写地址 32–35 位（sdk HipcMessageBufferAccessor::SetSend），
//   游戏地址的 36 位以上全丢。同一判据按窗口所在块补回：不是 0、本身不是游戏地址、补上块基址正好是游戏已映射的地址才补（块以下是宿主自己的内存，没有歧义）
__attribute__((visibility("default"), used)) uint64_t s2a_ipc_addr39(uint64_t a) {
    const uint64_t B36 = 1ull << 36, B39 = 1ull << 39;
    if (!a || in_as(a, 1)) return a;
    if (L.as_type == 1 && L.win_base >= B36 && a < B36) { uint64_t hi = L.win_base & ~(B36 - 1); return s2a_mem_mapped(a | hi, 1) ? a | hi : a; }
    if (a >= B39) return a;
    return s2a_mem_mapped(a | B39, 1) ? a | B39 : a;
}
// The copy provider needs a permission predicate, not QueryMemory's complete
// neighbouring extent. Keep query() unchanged for guest SDK address allocation.
static int readable(uint64_t a, uint64_t end) {   // 持g_mu，优先级与query一致
    while (a < end) {
        uint64_t base = 0, size = 0; uint32_t state, perm = 0; int k;
        if (a >= (uint64_t)g_heap && a < (uint64_t)g_heap + g_heap_cur) {
            base = (uint64_t)g_heap; size = g_heap_cur; perm = 3;
        } else if ((k = map_find(a, 1))) {
            base = g_map[k - 1].base; size = g_map[k - 1].size; perm = g_map[k - 1].perm;
        } else if (module_region(a, &base, &size, &state, &perm)) {
        } else if (g_tlr_next && a >= L.tlsio_base && a < ((g_tlr_next + 0xfff) & ~0xfffull)) {
            base = L.tlsio_base; size = ((g_tlr_next + 0xfff) & ~0xfffull) - base; perm = 3;
        } else if (a >= L.alias_base && a < L.alias_base + L.alias_size && g_amask) {
            uint8_t bits = *amask_at(a);
            const uint8_t full = (uint8_t)((1u << (HPG >> 12)) - 1);
            if (!(bits >> ((a & (HPG - 1)) >> 12) & 1)) return 0;
            base = bits == full ? a & ~(HPG - 1) : a & ~0xfffull; size = bits == full ? HPG : 0x1000; perm = 3;
        } else return 0;
        if (g_nperm) perm = perm_clip(a, &base, &size, perm);
        if (!(perm & 1) || !size || base > a || size > UINT64_MAX - base || base + size <= a) return 0;
        a = base + size;
    }
    return 1;
}
int s2a_mem_readable(uint64_t addr, uint64_t size) {
    if (size > UINT64_MAX - addr) return 0;
    GMU_LOCK();
    int result = readable(addr, addr + size);
    GMU_UNLOCK();
    return result;
}
// CreateTransferMemory 借出源（11-01 审计 P2-11，thread.c 0x15 / 关句柄调）：同 Ryujinx BorrowTransferMemory——[a, a+n) 须在地址空间内、逐段已映射且读写，
//   否则回 InvalidMemState；借出后权限账记成建立时的权限（QueryMemory 看到借出的源，同真机）。宿主保护不改：服务按源地址读写这段（Dispatcher S2AInitializeForeign），
//   宿主页 16 KB 也会连带邻居。还回（句柄关闭）清掉这段改过的权限，回来源缺省（借出前已核过整段读写，同 Ryujinx UnborrowTransferMemory 回读写）
//   借出同时置借出属性：已借出的段（不论权限）不能再借（Ryujinx SetAttributesAndChangePermission 要属性 None）。代码内存借出照 Ryujinx BorrowCodeMemory 权限不变（perm < 0）
static uint32_t borrow_locked(uint64_t a, uint64_t n, int perm) {   // 持 g_mu
    for (uint64_t p = a; p < a + n;) {
        s2a_meminfo mi; query(p, &mi);
        if (mi.state == ST_FREE || mi.state == ST_INACCESSIBLE || mi.perm != 3 || mi.attr || mi.base + mi.size <= p) return 0xD401;
        p = mi.base + mi.size;
    }
    attr_set(a, a + n, ATTR_BORROWED); if (perm >= 0) perm_set(a, a + n, perm);
    return 0;
}
static void unborrow_locked(uint64_t a, uint64_t n) { attr_set(a, a + n, -1); perm_set(a, a + n, -1); }   // 同 Ryujinx Unborrow*：清借出、回读写（来源缺省）
uint32_t s2a_mem_tmem_borrow(uint64_t a, uint64_t n, uint32_t perm) {
    if (!in_as(a, n)) return 0xD401;
    GMU_LOCK(); uint32_t e = borrow_locked(a, n, (int)perm); GMU_UNLOCK();
    return e;
}
void s2a_mem_tmem_unborrow(uint64_t a, uint64_t n) { GMU_LOCK(); unborrow_locked(a, n); GMU_UNLOCK(); }
// 传输内存最后一个句柄关掉（thread.c close_handle，放 G 之后调：这里取 g_mu）：照 Ryujinx KTransferMemory.Destroy，还映射着（0x51 没解除）就不还回、源保持借出，
//   没映射才还回。原先无条件还回：映射期间关句柄后源属性清零，0x2D / 缩堆能把别名还在用的源页换掉
void s2a_mem_tmem_close(uint64_t a, uint64_t n) {
    GMU_LOCK();
    int mapped = 0;
    for (int i = 0; i < g_nmap; i++) if ((g_map[i].state == ST_TMEM || g_map[i].state == ST_TMEM_ISO) && g_map[i].src == a) mapped = 1;
    if (!mapped) unborrow_locked(a, n);
    GMU_UNLOCK();
    if (mapped) LOG("【黄】传输内存 %#llx+%#llx 映射期间关句柄：源保持借出（Ryujinx KTransferMemory.Destroy 同）", (unsigned long long)a, (unsigned long long)n);   // dbcb99865 新路径：真局读数期望 0 次
}
#ifdef S2A_UNIT_TEST
int s2a_mem_test_alias_hint(const char *npdm, uint64_t pid) { return alias_first_hint(npdm, pid); }   // 读 npdm 同目录 addr_need.tsv
void s2a_mem_test_map_perm(int index, uint32_t perm) { g_map[index].perm = perm; }
void s2a_mem_test_tlr(uint64_t base, uint64_t next) { L.tlsio_base = base; g_tlr_next = next; }
void s2a_mem_test_astype(uint32_t t) { L.as_type = t; }
void s2a_mem_test_heap_floor(uint64_t v) { g_lay_heap_floor = v; }   // #947：本局布局按过的「上一局真要过的普通堆」
void s2a_mem_test_reset_probes(void) { g_amask_probes = 0; }
uint64_t s2a_mem_test_probes(void) { return g_amask_probes; }
#ifdef __ANDROID__
// 锁序断言的负控：持 g_mu 再取 g_vp_mu，Debug 构建必须 abort。
void s2a_mem_test_lockorder_abort(void) { GMU_LOCK(); s2a_mem_lockorder_vp_enter(); GMU_UNLOCK(); }
#endif
#ifndef __ANDROID__
static void punch_reset_locked(void);   // #258 懒清零状态复位（定义在 pages_release 前）
#endif
// 单测：不经 NPDM / 窗口，直接摆出别名区、堆、映射表与子页位
void s2a_mem_test_alias_report(uint64_t v) { g_alias_report = v; }   // 单测：设报给游戏的别名区大小（#1441）
void s2a_mem_test_setup(uint64_t alias_base, uint64_t alias_size, uint64_t win_base, uint64_t win_size, void *heap, uint64_t heap_cur) {
    memset(&L, 0, sizeof L); L.alias_base = alias_base; L.alias_size = alias_size; L.win_base = win_base; L.win_size = win_size; g_alias_report = 0;
#ifdef __ANDROID__
    s2a_android_mirror_init((uintptr_t)win_base, (size_t)win_size);
#endif
    g_heap = heap; g_heap_cur = heap_cur; free(g_amask); g_amask = NULL;
#ifndef __ANDROID__
    punch_reset_locked();
#endif
    g_nmap = 0; g_nspan = 0; g_nmod = 0; g_phys = 0; g_tlr_next = 0; free(g_perm); g_perm = NULL; g_nperm = 0; free(g_attr); g_attr = NULL; g_nattr = 0;
    g_cur_st = g_cur_sh = g_pk_st = g_pk_sh = g_pk_st_end = g_pk_sh_end = g_pk_alias_end = g_pk_alias_in = 0;
    g_pk_heap_request = g_pk_heap = heap_cur;
#ifdef __ANDROID__
    // 夹具 setup 等价于一次新会话：先清空 refs / 整文件打洞，再按本布局的地址空间上界 open（只增不缩）。
    if (plat_backing_limit()) { int re = plat_backing_reset(); if (re) die("背板 reset 失败 errno=%d", re); }
    { int be = plat_backing_open(backing_limit_end()); if (be) die("背板 memfd 不可用 errno=%d", be); }
#endif
}
// 单测：在 test_setup 之后改成 VAMM 布局（SystemResourceSize 非 0）并给出堆区
void s2a_mem_test_vamm(uint64_t sysres, uint64_t heap_base, uint64_t heap_size) { L.sys_resource_size = sysres; L.heap_base = heap_base; L.heap_size = heap_size; g_heap = (uint8_t *)heap_base; }
void s2a_mem_test_subpage(uint64_t a, int on) { uint8_t *m = amask_at(a), bit = (uint8_t)(1u << ((a & (HPG - 1)) >> 12)); *m = on ? (*m | bit) : (*m & ~bit); }
void s2a_mem_test_map(uint64_t b, uint64_t n) { map_add(b, n, 0, ST_STACK, 0); }
void s2a_mem_test_sysres(uint64_t v, uint64_t heap_region) { L.sys_resource_size = v; L.heap_size = heap_region; }
uint64_t s2a_mem_test_phys(void) { return g_phys; }
void s2a_mem_test_svc_perm(int num, int on) { if (on) g_svc_perm[num >> 6] |= 1ull << (num & 63); else g_svc_perm[num >> 6] &= ~(1ull << (num & 63)); }
uint64_t s2a_mem_test_alias_in(void) { return g_pk_alias_in; }
void s2a_mem_test_peaks(uint64_t *heap_request, uint64_t *alias_end) { *heap_request = g_pk_heap_request; *alias_end = g_pk_alias_end; }
#endif

#ifdef __ANDROID__
// FPS checked store（#968 owner 裁定）：plat_mem_write32 的唯一入口，fps.c 换页线程逐帧调用。
//   锁序固定 VM g_mu → tracker 互斥 → drain 门，后两级只 trylock：任一拿不到按相反顺序放掉已得的、回 -EAGAIN，
//   实现内不自旋，调用方见 -EAGAIN 本次放弃、下一帧再试（fps.c 逐帧循环天然重试）。
//   只占这一页：4 字节经整宿主页共享别名写，绝不改原映射保护、不占第二页；成功按 unprotect 非等待段同一口径记 CPU 脏。
//   错误码：-EFAULT 无活映射 / 跨映射 / 溢出 / 未对齐；-EACCES 游戏视角不可写（含 0x02 外部只读，按内核现态核）；
//   -EOPNOTSUPP 私有页建不出共享别名；-EAGAIN 锁竞争 / 页被占 / 子页 GPU 改过未落地 / 精确快照在途 / 排空期 / 替页在途。
//   替页检测双保险：writer epoch 非零（含进入 backing_begin 之前的 SVC 窗口）+ backing 在途标志（不含 epoch 的替页也挡）。
int s2a_mem_write32_checked(uint64_t addr, uint32_t value) {
    if ((addr & 3) || addr > UINT64_MAX - 4) return -EFAULT;
    size_t page = plat_page_size();
    if (!page || (page & (page - 1))) return -EFAULT;
    GMU_LOCK();
    if (s2a_android_guest_writers_epoch() || s2a_track_backing_busy()) { GMU_UNLOCK(); return -EAGAIN; }
    s2a_meminfo mi; query(addr, &mi);
    if (mi.state == ST_FREE || mi.state == ST_INACCESSIBLE || addr + 4 > mi.base + mi.size) { GMU_UNLOCK(); return -EFAULT; }
    if (!(mi.perm & 2)) { GMU_UNLOCK(); return -EACCES; }
    uint8_t prior;
    int e = s2a_track_store32_claim(addr, &prior);
    if (e) { GMU_UNLOCK(); return e; }
    uint64_t base = addr & ~(uint64_t)(page - 1);
    void *alias = NULL;
    e = -(int)s2a_track_platform_alias(base, page, PROT_READ | PROT_WRITE, &alias);
    int wrote = 0;
    if (!e) {
        *(volatile uint32_t *)((uint8_t *)alias + (addr - base)) = value;
        wrote = 1;
        int ue = s2a_track_platform_unalias(alias, page);
        if (ue) LOG("checked 写已落地但解别名失败 errno=%d（写不回滚，unalias 只记日志）", ue);
    }
    s2a_track_store32_release(addr, prior, wrote);
    GMU_UNLOCK();
    return e;
}
#endif

// 游戏地址用量（诊断，C# 初始化后 / 收局 / 崩溃各打一次）：窗口里游戏真映射了多少、落在哪——别名区按 1 GB 一档列已映射字节，
// 另报最低 / 最高已映射地址、堆当前大小、映射表（栈 / 共享内存）合计与最高地址。只读本文件的账，不扫页表
void s2a_guest_va_report(const char *when) {
    // 崩溃处理里也调：拿不到锁就不等（只读计数，读到半新不旧也只是诊断）
    // GMU_TRYLOCK 成功返回 1（与旧 pthread_mutex_trylock 的 0 成功相反）；苹果宏也统一成 1。
    int lk = GMU_TRYLOCK();
    uint64_t amap = 0, alo = 0, ahi = 0, bk[80] = {0};   // 乙布局别名区可到窗口基址 +67 GiB 以上
    if (g_amask && L.alias_size) for (uint64_t i = 0, n = L.alias_size / HPG; i < n; i++) if (g_amask[i]) {
        uint64_t a = L.alias_base + i * HPG, b = (uint64_t)__builtin_popcount(g_amask[i] & 0xf) << 12;
        amap += b; if (!alo) alo = a; ahi = a + HPG; uint64_t g = (a - L.win_base) >> 30; if (g < 80) bk[g] += b; }
    uint64_t msz = 0, mhi = 0; for (int i = 0; i < g_nmap; i++) { msz += g_map[i].size; if (g_map[i].base + g_map[i].size > mhi) mhi = g_map[i].base + g_map[i].size; }
    char b[1200]; int o = 0; b[0] = 0;
    for (int g = 0; g < 80 && o < (int)sizeof b - 40; g++) if (bk[g]) o += snprintf(b + o, sizeof b - (size_t)o, " +%dG:%lluM", g, (unsigned long long)(bk[g] >> 20));
    LOG("游戏地址用量 [%s]：别名区 %#llx+%llu MB 已映射 %llu MB（最低 %#llx 最高 %#llx）；堆 %llu MB；映射表 %d 段 %llu MB 最高 %#llx｜别名区按窗口内 GB 档：%s",
        when, (unsigned long long)L.alias_base, (unsigned long long)(L.alias_size >> 20), (unsigned long long)(amap >> 20), (unsigned long long)alo, (unsigned long long)ahi,
        (unsigned long long)(g_heap_cur >> 20), g_nmap, (unsigned long long)(msz >> 20), (unsigned long long)mhi, o ? b : " 无");
    LOG("游戏地址峰值 [%s]：堆 %llu KB（堆区 %llu MB）；栈区映射 %llu KB、最高到区内 +%llu KB（栈区 %llu MB）；共享内存 %llu KB、最高到区内 +%llu KB（可随机区 %llu MB）",
        when, (unsigned long long)(g_pk_heap >> 10), (unsigned long long)(L.heap_size >> 20), (unsigned long long)(g_pk_st >> 10), (unsigned long long)(g_pk_st_end >> 10),
        (unsigned long long)(L.stack_size >> 20), (unsigned long long)(g_pk_sh >> 10), (unsigned long long)(g_pk_sh_end >> 10), (unsigned long long)(L.aslr_size >> 20));
    {   // 本局所需地址跨度（改前 → 改后表的「改后」）：别名区最高已映射、堆顶、映射表最高、模块末，取最高减窗口基址
        uint64_t top = ahi > mhi ? ahi : mhi; if ((uint64_t)g_heap + g_heap_cur > top) top = (uint64_t)g_heap + g_heap_cur;
        for (int i = 0; i < g_nmod; i++) { uint64_t e = (uint64_t)g_mod[i].base + g_mod[i].size; if (e > top && e <= L.win_base + L.win_size) top = e; }
        LOG("地址按需 [%s]：宿主占到 %#llx +%llu MB（标准布局要 %llu MB），本局所需跨度 %llu MB（碰到的最高地址 − 窗口基址），越界映射 %llu 次", when,
            (unsigned long long)L.win_base, (unsigned long long)(L.win_size >> 20), (unsigned long long)(g_lay_need >> 20),
            (unsigned long long)(top > L.win_base ? (top - L.win_base) >> 20 : 0), (unsigned long long)atomic_load(&g_va_short)); }
    addr_need_report();
#ifdef __ANDROID__
    LOG("背板 [%s]：limit=%llu deferred=%llu reuse_denied=%llu punch_commit=%llu/%llu ns punch_release=%llu/%llu ns punch_unref=%llu/%llu ns", when,
        (unsigned long long)plat_backing_limit(), (unsigned long long)atomic_load(&g_backing_release_deferred),
        (unsigned long long)atomic_load(&g_backing_reuse_denied),
        (unsigned long long)atomic_load(&g_backing_punch_count[BACKING_PUNCH_COMMIT]),
        (unsigned long long)atomic_load(&g_backing_punch_ns[BACKING_PUNCH_COMMIT]),
        (unsigned long long)atomic_load(&g_backing_punch_count[BACKING_PUNCH_RELEASE]),
        (unsigned long long)atomic_load(&g_backing_punch_ns[BACKING_PUNCH_RELEASE]),
        (unsigned long long)atomic_load(&g_backing_punch_count[BACKING_PUNCH_UNREF]),
        (unsigned long long)atomic_load(&g_backing_punch_ns[BACKING_PUNCH_UNREF]));
#endif
    if (lk) GMU_UNLOCK();
#ifdef __ANDROID__
    // VMA 采样（/proc/self/maps）必须放掉 g_mu 后再做：正文承诺解析 maps 不持锁，采样器入口也断言这一点。
    // 崩溃信号上下文不能调 fopen/getline（同 mirror_report 的约定）：只跳过 VMA 采样。
    if (strcmp(when, "崩溃时")) s2a_android_vma_report(when);
#endif
    s2a_far_report(when);
#ifdef __ANDROID__
    // 崩溃信号上下文不可调用 fopen/getline 等非常规信号安全 API。
    if (lk && strcmp(when, "崩溃时")) s2a_android_mirror_report(when);
#endif
}

// ---------- 报红 / 外部 ----------
// 已接上的号、按语义回了失败码（堆区不够、句柄不是伪句柄、信息类型不认识）：打普通日志（每号首次与第 2^k 次），不进报红通道——
//   gate.c s2a_svc_red 只留给分派里真没接的号（退出汇总「未实现系统调用」按它计；10-26 五款的「未实现 0x1」其实是堆区不够）
static void svc_fail_log(int num, uint64_t rc, const char *why) {
    static _Atomic uint64_t n[0x100]; uint64_t k = atomic_fetch_add(&n[num & 0xff], 1) + 1;
    if (!(k & (k - 1))) LOG("系统调用 %#x 回 %#llx：%s（第 %llu 次）", num, (unsigned long long)rc, why, (unsigned long long)k);
}
void *s2a_shmem_lookup(uint32_t h, uint64_t *size);                               // thread.c
int s2a_thread_alive_count(void); int s2a_thread_cur_core(void); uint64_t s2a_thread_cpu_ns(uint32_t h, int *ok);
void s2a_host_shutdown(const char *why);                                            // lifecycle.c
static void session_guest_end(const char *why) {
    void (*park)(const char *) = (void (*)(const char *))dlsym(RTLD_DEFAULT, "s2a_session_park_guest_end");
    if (park) park(why);
}
void s2a_svc_frame(uint64_t pc);                                                    // thread.c

enum { I_CoreMask = 0, I_PriorityMask = 1, I_AliasAddr = 2, I_AliasSize = 3, I_HeapAddr = 4, I_HeapSize = 5, I_TotalMem = 6, I_UsedMem = 7,
       I_DebuggerAttached = 8, I_ResourceLimit = 9, I_IdleTick = 10, I_RandomEntropy = 11, I_AslrAddr = 12, I_AslrSize = 13, I_StackAddr = 14, I_StackSize = 15,
       I_SysResTotal = 16, I_SysResUsed = 17, I_ProgramId = 18, I_UserExcAddr = 20, I_TotalNonSys = 21, I_UsedNonSys = 22, I_IsApplication = 23,
       I_FreeThreadCount = 24, I_ThreadTickCount = 25, I_IsSvcPermitted = 26, I_AliasExtraSize = 28,
       I_RemoteAddr = 29, I_RemoteSize = 30, I_RemoteUsage = 31, I_RemoteUsagePeak = 32, I_PageSize = 33,   // Switch 2（switchbrew SVC [S2]）
       I_ThreadTickCountOld = 0xF0000002 };   // 25 是 4.0.0 起的正式号，0xF0000002 是旧号

static uint64_t heap_region(void) { return L.heap_size; }
// ---------- 资源上限对象（系统调用批 B，照 Ryujinx KResourceLimit / ProcessLoaderHelper）----------
// 句柄种类：原生层内部引用计数对象（thread.c s2a_native_handle_new），子号 1 = 资源上限、2 = 代码内存。资源：0 内存、1 线程、2 事件、3 传输内存、4 会话
// 本进程的上限对象：内存 = 应用池、线程 608、事件 700、传输内存 128、会话 894；当前值 / 峰值按实际记账（内存 = 堆 + 堆外固定部分，其余按句柄种类计数）
uint32_t s2a_native_handle_new(uint32_t sub, void *obj); void *s2a_native_handle_obj(uint32_t h, uint32_t sub); void s2a_handle_kind_count(uint32_t kind, int *cur, int *peak);
typedef struct { int refs; int proc; int64_t lim[5], cur[5], peak[5]; } reslimit_t;
static reslimit_t g_rl = {1, 1, {0, 608, 700, 128, 894}, {0}, {0}};
static uint64_t g_mem_peak;
static uint64_t pool_fixed(void);
static uint64_t mem_used(void) { return g_heap_cur + pool_fixed(); }
static void mem_peak_note(void) { uint64_t u = mem_used(); if (u > g_mem_peak) g_mem_peak = u; }
static int rl_value(uint32_t h, uint32_t which, int what, int64_t *v) {   // what：0 上限、1 当前、2 峰值；回 0 或错误码
    if (which >= 5) return 0xF001;   // InvalidEnumValue
    reslimit_t *r = s2a_native_handle_obj(h, 1); if (!r) return 0xE401;
    if (!r->proc) { *v = what == 0 ? r->lim[which] : what == 1 ? r->cur[which] : r->peak[which]; return 0; }
    if (which == 0) { *v = what == 0 ? (int64_t)pool_eff() : what == 1 ? (int64_t)mem_used() : (int64_t)(g_mem_peak > mem_used() ? g_mem_peak : mem_used()); return 0; }
    if (what == 0) { *v = r->lim[which]; return 0; }
    static const uint32_t kind[5] = {0, S2A_OBJ_THREAD, S2A_OBJ_EVENT_R, S2A_OBJ_TMEM, S2A_OBJ_SESSION};
    int c, pk; s2a_handle_kind_count(kind[which], &c, &pk); *v = what == 1 ? c : pk; return 0;
}
// 游戏地址空间（Ryujinx MemoryManager.InsideAddrSpace）：布局窗口，或某个模块映像（含补丁区；模块可在窗口外）
static int in_as(uint64_t a, uint64_t n) {
    if (a >= L.win_base && a + n <= L.win_base + L.win_size) return 1;
    if (a >= L.alias_base && a + n <= L.alias_base + L.alias_size && a + n >= a) return 1;   // 别名区整段是地址空间（伸出宿主窗口的那段是承诺，layout.c 乙）
    for (int i = 0; i < g_nmod; i++) { const module_t *m = &g_mod[i]; uint64_t b = (uint64_t)m->base;
        uint64_t lo = m->patch_off < 0 ? b + (uint64_t)m->patch_off : b, hi = b + (m->patch_off < 0 ? m->image_size : m->size);
        if (a >= lo && a + n <= hi) return 1;
        if (m->patch2_len && a >= b + (uint64_t)m->patch2_off && a + n <= b + (uint64_t)m->patch2_off + m->patch2_len) return 1; }
    return 0;
}
// 宿主占到的窗口（布局可伸出窗口，伸出去的只是承诺，layout.c）：游戏要映射到窗口外 = 宿主给不出这段地址，回 OutOfMemory（0xD001，
//   Switch 上内存不够时的同一结果）并报红（首次与第 2^k 次），不 mprotect 到宿主自己的内存上
static int win_has(uint64_t a, uint64_t n) { return a >= L.win_base && n <= L.win_size && a - L.win_base <= L.win_size - n; }
// 最近一次地址越界的请求（崩溃现场归因用：游戏不理会失败照样访问那段，崩出 SIGBUS 时点名是它，crash.c fault_region_note）
static _Atomic uint64_t g_vs_a, g_vs_n; static _Atomic int g_vs_num;
int s2a_va_short_last(uint64_t fa, int *num, uint64_t *a, uint64_t *n, uint64_t *count) {
    uint64_t a0 = atomic_load(&g_vs_a), n0 = atomic_load(&g_vs_n);
    if (!n0 || fa < a0 || fa - a0 >= n0) return 0;
    *num = atomic_load(&g_vs_num); *a = a0; *n = n0; *count = atomic_load(&g_va_short); return 1;
}
// 最近 32 次 UnmapPhysicalMemory（崩溃现场归因用，诊断#ff9a04：游戏读别名区里当前未映射的一页崩了，那段是游戏自己早先解除的（悬空指针），
//   还是从没解除过、是我方弄丢的，原先判不了）。只在成功解除后记；崩溃处理里无锁读，读到半新不旧也只是诊断
#define UNMAP_RING 32
static struct { _Atomic uint64_t a, n, ms; } g_unmap_ring[UNMAP_RING];
static _Atomic uint64_t g_unmap_seq;
static uint64_t mono_ms(void) { struct timespec t; return clock_gettime(CLOCK_MONOTONIC, &t) ? 0 : (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static void unmap_ring_note(uint64_t a, uint64_t n) {
    uint64_t k = atomic_fetch_add(&g_unmap_seq, 1) % UNMAP_RING;
    atomic_store(&g_unmap_ring[k].n, 0); atomic_store(&g_unmap_ring[k].a, a); atomic_store(&g_unmap_ring[k].ms, mono_ms()); atomic_store(&g_unmap_ring[k].n, n);
}
// fa 落在最近 32 次里哪一次解除的段：命中返回 1（取最近的一次，*back = 倒数第几次、*ago_ms = 多久前），*total = 本局解除总次数
int s2a_phys_unmap_hit(uint64_t fa, uint64_t *a, uint64_t *n, uint64_t *back, uint64_t *ago_ms, uint64_t *total) {
    uint64_t seq = atomic_load(&g_unmap_seq), now = mono_ms(); *total = seq;
    for (uint64_t i = 1; i <= UNMAP_RING && i <= seq; i++) {
        uint64_t k = (seq - i) % UNMAP_RING, a0 = atomic_load(&g_unmap_ring[k].a), n0 = atomic_load(&g_unmap_ring[k].n);
        if (n0 && fa >= a0 && fa - a0 < n0) { uint64_t t = atomic_load(&g_unmap_ring[k].ms); *a = a0; *n = n0; *back = i; *ago_ms = now > t ? now - t : 0; return 1; }
    }
    return 0;
}
static uint64_t va_short(int num, uint64_t a, uint64_t n, const char *region) {
    uint64_t k = atomic_fetch_add(&g_va_short, 1) + 1;
    atomic_store(&g_vs_num, num); atomic_store(&g_vs_a, a); atomic_store(&g_vs_n, n);
    if (!(k & (k - 1))) LOG("【红】地址越界：svc %#x 要在%s映射 %#llx +%#llx，宿主只占到 [%#llx, %#llx)（第 %llu 次）；回内存不足（游戏真碰的地址跨度超过本机给的地址）",
        num, region, (unsigned long long)a, (unsigned long long)n, (unsigned long long)L.win_base, (unsigned long long)(L.win_base + L.win_size), (unsigned long long)k);
    return 0xD001;
}
// 只链本文件的单测没有 far.c：弱定义（探针关）；宿主链了 far.c 用它的
__attribute__((weak)) void s2a_far_set(uint64_t lo, uint64_t hi, uint64_t sh_lo, uint64_t sh_hi) { (void)lo; (void)hi; (void)sh_lo; (void)sh_hi; }
__attribute__((weak)) uint64_t s2a_far_floor(void) { return UINT64_MAX; }
__attribute__((weak)) int s2a_far_has(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }
__attribute__((weak)) int s2a_far_map(uint64_t a, uint64_t n) { (void)a; (void)n; return -1; }
__attribute__((weak)) void s2a_far_report(const char *when) { (void)when; }
__attribute__((weak)) uint8_t *s2a_far_g2h(uint64_t a) { (void)a; return NULL; }
__attribute__((weak)) void s2a_far_release(uint64_t a, uint64_t n) { (void)a; (void)n; }
// 远端（far.c）：4 KB 子页清零——远端的内容在影子区，经 s2a_far_g2h 写
#ifndef __ANDROID__
static void zero_sub(uint64_t p, uint64_t we) {
    memset(p < we ? (uint8_t *)p : s2a_far_g2h(p), 0, 0x1000);
}
#endif
// 地址 / 大小 / 回绕三项公共校验（Ryujinx Syscall 各内存调用开头同一套）：地址 4 KB 对齐 → 大小 4 KB 对齐且非零 → 不回绕
// 把 [src, src+n) 的同一批物理页共享映射到 dst（游戏地址 = 宿主地址），三者按平台宿主页对齐并按 prot 设权限。成功 0，否则 Horizon 错误码
static uint64_t alias_map(uint64_t dst, uint64_t src, uint64_t n, int prot) {
#ifdef __ANDROID__
    size_t page = plat_page_size();
    if (!page || (dst | src | n) & (page - 1)) return 0xD401;
    int e = plat_vm_alias((void *)dst, (const void *)src, n, (uint32_t)prot);
    if (e == EIO) die("别名映射失败且无法恢复地址保留");
    return e ? 0xD401 : 0;
#else
    if ((dst | src | n) & (HPG - 1)) return 0xD401;
    vm_address_t a = (vm_address_t)dst; vm_prot_t cur, max;
    if (vm_remap(mach_task_self(), &a, (vm_size_t)n, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)src, FALSE, &cur, &max, VM_INHERIT_SHARE) != KERN_SUCCESS || a != (vm_address_t)dst) return 0xD401;
    if (prot != (VM_PROT_READ | VM_PROT_WRITE) && vm_protect(mach_task_self(), a, (vm_size_t)n, FALSE, prot) != KERN_SUCCESS) return 0xD401;
    return 0;
#endif
}
#ifndef __ANDROID__
// 还回游戏不再用的整宿主页（缩堆、解除物理映射）：换成新的匿名占位（PROT_NONE），物理页当场归还，日后再给出去一定是零页——
//   Switch / Ryujinx 新给的堆和物理映射都是填 0 的页（KPageTableBase SetHeapSize / MapPhysicalMemory 带 shouldFillPages、填充值 0）。
//   原先用 madvise(MADV_FREE) + mprotect(PROT_NONE)：那是懒回收，内核还没收走的页在 mprotect 放出时带着旧内容回到游戏手里。
//   调用方解锁后要报 backing_changed（渲染器包着游戏内存的零拷贝缓冲要重建）
// #258 文件后备下的还页不再在锁内打洞（懒清零）：时之笛 S2（Mac 16 GB，文件后备自动开）每约 30 秒 UnmapPhysicalMemory 约 250 MB，逐 16 KB 打洞 + 重映射
//   每页 60–70 µs、全程持 g_mu 约 1 s，音频渲染（s2a_mem_mapped）等锁、整帧停住。改法：锁内只把这段 mprotect 成不可访问（微秒级）并记成「待清零」，
//   不打洞、不重映射；页被游戏重新放出来时就地处理——0x2C / SetHeapSize 增长 / MapMemory 拷贝路径先 mprotect 读写再 memset 清零（punch_zero_locked），
//   MapMemory 对齐路径 vm_remap 盖掉原映射、只撤销标记（punch_forget_locked）。零页语义不变；释放的页留在文件里当脏页，内存紧时由内核按文件后备原设计换出
//   （真机 6 分钟释放 1.19 GB，物理脚印不涨）。两版「尽快打洞」（攒段一次 F_PUNCHHOLE；后台线程逐块打洞）都在真机上让 Metal 报 GPU 挂死：
//   游戏渲染期间对后备文件打洞，Metal 命令提交就在内核里和 F_PUNCHHOLE 互卡；原版不挂只因为那 1 s 卡顿让 GPU 空闲。细节见 docs/坑记录.md。
//   S2A_PUNCH_DEFER=0 回原路径（逐页同步打洞 + 重映射，负对照用）。
static uint8_t *g_punch_pend;   // 窗口内每宿主页一字节：1 待清零（释放后还没放出来）；g_mu 保护
static uint64_t g_punch_pending_n;   // 待清零宿主页数
static uint64_t g_punch_inline_n, g_punch_forget_n;   // 读数：放出来时就地清零的页数、对齐 MapMemory 盖掉而撤销标记的页数
static uint64_t g_unmap_n, g_unmap_ns, g_unmap_max_ns;   // 读数：≥1 MB 的 UnmapPhysicalMemory 持锁次数 / 总时长 / 最长一次（S2A_PUNCH_DEFER=0 与缺省同一份代码量，PR 修前→修后用）
static int punch_defer_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("S2A_PUNCH_DEFER"); v = !(e && *e == '0'); } return v; }
static uint8_t *punch_at(uint64_t hp) { return g_punch_pend + (hp - L.win_base) / HPG; }
// 还页走懒清零（持 g_mu）：[a, a + n) 全在文件后备窗口里才接，改成不可访问并记待清零；接了返回 1
static int punch_defer_locked(uint64_t a, uint64_t n) {
    if (!punch_defer_on() || !L.win_size || !s2a_guest_in_file(a, n) || a < L.win_base || a + n > L.win_base + L.win_size || (a | n) & (HPG - 1)) return 0;
    // 待清零表先分配、分不出就不走懒清零（返回 0：调用方走 s2a_guest_zero 立即清零）。诊断#0ed154（8 GB iPad、低区耗尽、errno 12 ×22）：
    //   原先 mprotect 后才 calloc、不判空，calloc 回空后按页号写进空指针（读 0xfebb），宿主在还页路径上崩
    if (!g_punch_pend && !(g_punch_pend = calloc(L.win_size / HPG, 1))) {
        static int once; if (!once++) LOG("【红】懒清零待清零表 %llu KB 分配失败（errno=%d），改走立即清零", (unsigned long long)(L.win_size / HPG >> 10), errno);
        return 0; }
    if (mprotect((void *)a, n, PROT_NONE)) die("懒清零 mprotect errno=%d", errno);
    for (uint64_t p = a; p < a + n; p += HPG) { uint8_t *b = punch_at(p); if (!*b) { *b = 1; g_punch_pending_n++; } }
    return 1;
}
// 游戏把 [lo, hi) 放出来（持 g_mu，调用方已把这段 mprotect 成读写）：其中待清零的宿主页内容是旧的，就地清零并撤销标记
static void punch_zero_locked(uint64_t lo, uint64_t hi) {
    if (!g_punch_pend || !L.win_size) return;
    uint64_t we = L.win_base + L.win_size; if (lo < L.win_base) lo = L.win_base; if (hi > we) hi = we;
    for (uint64_t p = lo; p < hi; p += HPG) { uint8_t *b = punch_at(p); if (!*b) continue; *b = 0; g_punch_pending_n--; memset((void *)p, 0, HPG); g_punch_inline_n++; }
}
// [lo, hi) 的原映射被整体盖掉（MapMemory 对齐路径 vm_remap 共享源页）：旧内容不可见了，只撤销标记、不碰内存（那是源的页）
static void punch_forget_locked(uint64_t lo, uint64_t hi) {
    if (!g_punch_pend || !L.win_size) return;
    uint64_t we = L.win_base + L.win_size; if (lo < L.win_base) lo = L.win_base; if (hi > we) hi = we;
    for (uint64_t p = lo; p < hi; p += HPG) { uint8_t *b = punch_at(p); if (!*b) continue; *b = 0; g_punch_pending_n--; g_punch_forget_n++; }
}
// 即时存档（snapshot.c dump_mem）：这一宿主页是游戏已释放、还没清零的（懒清零待清零）——语义上是零页，内容是作废的旧数据，不该存进快照。
//   存档时游戏线程已冻结、不会再有还页 / 放出，这里不取 g_mu
int s2a_snap_page_released(uint64_t hp) {
    return g_punch_pend && L.win_size && hp >= L.win_base && hp < L.win_base + L.win_size && *punch_at(hp & ~(uint64_t)(HPG - 1));
}
// 收局时（lifecycle.c，_exit 前）打一行读数：取消 / SIGTERM 路径不走会话重置，否则读数永远看不到
void s2a_punch_summary(void) {
    if (g_unmap_n) LOG("#258 解除物理映射读数：≥1 MB 的解除 %llu 次、持锁共 %llu ms、最长一次 %llu ms", (unsigned long long)g_unmap_n, (unsigned long long)(g_unmap_ns / 1000000), (unsigned long long)(g_unmap_max_ns / 1000000));
    if (g_punch_pend) LOG("#258 懒清零读数（%s）：放出来时就地清零 %llu 页、对齐 MapMemory 盖掉 %llu 页、待清零 %llu 页", punch_defer_on() ? "开" : "关", (unsigned long long)g_punch_inline_n, (unsigned long long)g_punch_forget_n, (unsigned long long)g_punch_pending_n);
}
static void punch_reset_locked(void) {
    if (g_punch_pend) s2a_punch_summary();
    free(g_punch_pend); g_punch_pend = NULL; g_punch_pending_n = 0;
    g_punch_inline_n = g_punch_forget_n = 0; g_unmap_n = g_unmap_ns = g_unmap_max_ns = 0;
}
#ifdef S2A_UNIT_TEST
int s2a_mem_test_alias_eacces;   // 单测：对齐 MapMemory 的共享映射按「最大权限不含写」处理（真机 iPad 上源在私有文件映射里的情形，Mac 上造不出来）
uint64_t s2a_mem_test_punch_pending(void) { return g_punch_pending_n; }
void s2a_mem_test_punch_stats(uint64_t *inline_zero, uint64_t *forget) { *inline_zero = g_punch_inline_n; *forget = g_punch_forget_n; }
#endif
static void pages_release(uint64_t a, uint64_t n) {
    if (punch_defer_locked(a, n)) return;   // #258：文件后备窗口里的页懒清零（上面）
    if (s2a_guest_zero(a, n)) return;   // 客体内存文件后备（vawin.c）：打洞 + 原偏移重映射，再给出去同样是零
    if (mmap((void *)a, n, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) die("还页 mmap errno=%d", errno);
}
#endif
static uint64_t alias_unmap(uint64_t dst, uint64_t n, uint64_t source, int prot) {   // 还回窗口的不可访问保留（我方窗口内，不碰别人）
#if defined(__ANDROID__)
    {
        // 别名解除：同步清掉受控别名登记（否则一次别名会把这个范围永久判成“不可迁移”）。
        // 只在安卓直连编；苹果宿主不链这个后端，弱引用在 Mach-O 上没有对齐放行会变成未定义符号。
        extern void s2a_alias_drop(uint64_t) __attribute__((weak));
        if (s2a_alias_drop) s2a_alias_drop(dst);
    }
#endif
#ifndef __ANDROID__
    if (s2a_guest_zero(dst, n)) return 0;   // 客体内存文件后备（vawin.c）：盖回文件自己那段（打洞 = 零）
#endif
    if (mmap((void *)dst, n, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) {
#ifdef __ANDROID__
        // 先完整恢复旧 alias 与 guest 请求的权限，再报告失败；不能把 tracker 临时权限当 guest 权限。
        if (plat_vm_alias((void *)dst, (const void *)source, n, (uint32_t)prot)) die("解别名失败且旧映射回滚失败");
        return 0xD401;
#else
        die("解别名映射 mmap errno=%d", errno);
#endif
    }
#ifndef __ANDROID__
    (void)source; (void)prot;
#endif
    return 0;
}
#ifdef __ANDROID__
static uint32_t backing_refs_checked(uint64_t a, uint64_t n, const char *what) {
    uint32_t refs = plat_backing_refs(a, n);
    if (refs == UINT32_MAX) die("背板引用查询失败 %s %#llx+%#llx", what, (unsigned long long)a, (unsigned long long)n);
    return refs;
}
static uint64_t backing_punch_now_ns(void) {
    struct timespec ts;
    return clock_gettime(CLOCK_MONOTONIC, &ts) ? 0 : (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
// Q6：只对真正打洞的调用计数 / 计时；deferred 只做了一次引用查询，不是打洞。
static int backing_punch_timed(int kind, uint64_t a, size_t n, int *deferred) {
    uint64_t start = backing_punch_now_ns();
    int error = plat_backing_punch(a, n, deferred);
    uint64_t elapsed = backing_punch_now_ns() - start;
    if (!error && !*deferred) {
        atomic_fetch_add(&g_backing_punch_count[kind], 1);
        atomic_fetch_add(&g_backing_punch_ns[kind], elapsed);
    }
    return error;
}
#ifdef S2A_UNIT_TEST
// 主机用例断言计数随操作增加；顺序：提交次数/耗时、释放次数/耗时、unref 补打次数/耗时。
void s2a_mem_test_backing_punch_stats(uint64_t out[BACKING_PUNCH_KINDS * 2]) {
    for (int i = 0; i < BACKING_PUNCH_KINDS; i++) {
        out[i * 2] = atomic_load(&g_backing_punch_count[i]);
        out[i * 2 + 1] = atomic_load(&g_backing_punch_ns[i]);
    }
}
// 释放时因 ref 跳过的宿主页（svc 侧计数，不是 backing.c 的竞态 deferred）。
uint64_t s2a_mem_test_release_deferred(void) { return atomic_load(&g_backing_release_deferred); }
#endif
// 已死页判定：堆区以当前堆顶为界；别名区按 4 KB 子页掩码（同一宿主页的所有子页都死了才能打洞）。
static int backing_page_dead_locked(uint64_t p) {
    if (g_heap && L.heap_size && p >= (uint64_t)g_heap && p - (uint64_t)g_heap < L.heap_size)
        return p - (uint64_t)g_heap >= g_heap_cur;
    if (L.alias_size && p >= L.alias_base && p - L.alias_base < L.alias_size) {
        if (!g_amask) return 1;
        return !(*amask_at(p) & (uint8_t)(1u << ((p & (HPG - 1)) >> 12)));
    }
    return 0;
}
// 释放后打洞：只对整宿主页都已死、且没有别名引用的连续段调用 fallocate。
// 有引用的页必须断开跳过——它还可能被 NVN 长寿别名读到，打洞会把活内容清零；
// 推迟的页等最后一次 unref（s2a_mem_backing_alias_unref）再打。
static void backing_release_locked(uint64_t a, uint64_t n, int punch_kind) {
    if (!n || !plat_backing_limit()) return;
    size_t page = plat_page_size();
    if (!page || ((a | n) & (page - 1))) die("背板释放范围未按宿主页对齐 %#llx+%#llx", (unsigned long long)a, (unsigned long long)n);
    uint64_t run_lo = 0, run_len = 0;
    for (uint64_t p = a; p < a + n; p += page) {
        int dead = 1;
        for (uint64_t sub = p; sub < p + page; sub += 0x1000)
            if (!backing_page_dead_locked(sub)) { dead = 0; break; }
        int referenced = dead && backing_refs_checked(p, page, "释放") != 0;
        if (referenced) atomic_fetch_add(&g_backing_release_deferred, 1);   // 因 ref 跳过的宿主页
        if (dead && !referenced) {
            if (!run_len) run_lo = p;
            run_len += page;
            continue;
        }
        if (run_len) {
            int deferred = 0;
            int error = backing_punch_timed(punch_kind, run_lo, (size_t)run_len, &deferred);
            if (error) die("背板释放打洞失败 errno=%d（%#llx+%#llx）", error, (unsigned long long)run_lo, (unsigned long long)run_len);
            if (deferred) atomic_fetch_add(&g_backing_release_deferred, run_len / page);
            run_len = 0;
        }
    }
    if (run_len) {
        int deferred = 0;
        int error = backing_punch_timed(punch_kind, run_lo, (size_t)run_len, &deferred);
        if (error) die("背板释放打洞失败 errno=%d（%#llx+%#llx）", error, (unsigned long long)run_lo, (unsigned long long)run_len);
        if (deferred) atomic_fetch_add(&g_backing_release_deferred, run_len / page);
    }
}
// 「源在背板区」按首宿主页是否落在堆区 / 别名区判断（不能只看 limit：limit ≥ 2^39，
//   0x13 的宿主 shmem 地址也会落在里面）。命中就整段 ref；NVN 长寿别名只在这里核活。
// 逐宿主页核活：首页活、后面有死页时不能 ref（那会让 0x2C / 堆增长永久回 0xD401）。
// 回 1 = 已 ref；0 = 源不在背板区（0x13 宿主 shmem / 栈区私有页）；-EFAULT = 有源页已死；其它 -errno。
static int backing_alias_ref_locked(uint64_t src, uint64_t n) {
    size_t page = plat_page_size();
    if (!page || (page & (page - 1)) || !n || ((src | n) & (page - 1))) return -EINVAL;
    uint64_t first = src;
    int in_heap = g_heap && L.heap_size && first >= (uint64_t)g_heap && first - (uint64_t)g_heap < L.heap_size;
    int in_alias = L.alias_size && first >= L.alias_base && first - L.alias_base < L.alias_size;
    if (!in_heap && !in_alias) return 0;
    uint64_t end = in_heap ? (uint64_t)g_heap + L.heap_size : L.alias_base + L.alias_size;
    if (n > end - first) return -EINVAL;
    for (uint64_t p = first; p < first + n; p += page)
        if (backing_page_dead_locked(p)) return -EFAULT;
    int error = plat_backing_ref(src, (size_t)n);
    return error ? -error : 1;
}
static void backing_alias_unref_locked(uint64_t src, uint64_t n) {
    int error = plat_backing_unref(src, (size_t)n);
    if (error) die("背板引用下溢 / 区间不匹配 errno=%d（%#llx+%#llx）", error, (unsigned long long)src, (unsigned long long)n);
    backing_release_locked(src, n, BACKING_PUNCH_UNREF);
}
// 提交前打洞：新页必须是零页，不能依赖之前所有的打洞都完成、也不能依赖没有残留别名回填过。
// 调用方已在事务里（backing_begin_locked 之后）。返回 0 = 已打洞；EAGAIN = 放锁窗口里
// 有 NVN 线程 ref 进来（不能 die，调用方回 0xD401 并回滚本段事务）；其它 = fallocate 失败，调用方 die。
#ifdef S2A_UNIT_TEST
// 负控 / 竞态夹具：在「复用检查之后、提交前打洞之前」注入一个 ref，模拟 NVN 在
// backing_begin_locked 放锁窗口里进来。生产路径不设这个钩子。
void (*s2a_mem_test_prepare_hook)(uint64_t a, uint64_t n);
#endif
static int backing_prepare_locked(uint64_t a, uint64_t n) {
#ifdef S2A_UNIT_TEST
    if (s2a_mem_test_prepare_hook) s2a_mem_test_prepare_hook(a, n);
#endif
    int deferred = 0;
    int error = backing_punch_timed(BACKING_PUNCH_COMMIT, a, (size_t)n, &deferred);
    if (error) return error;
    return deferred ? EAGAIN : 0;
}
// #1281 NVN 稀疏虚拟池的长寿别名入口（hle_nvn.c 的 Android 块调用）。
// 回 1 = 已 ref；0 = 源不在背板区（池存储是宿主内存）；-EFAULT = 源页已死；其它负数 = -errno。
int s2a_mem_backing_alias_ref(uint64_t src, uint64_t n) {
    GMU_LOCK();
    int result = backing_alias_ref_locked(src, n);
    GMU_UNLOCK();
    return result;
}
// 解引用后再打洞：源页若已死（缩堆 / 0x2D 之后才解别名），这里是它最后一次被引用。
void s2a_mem_backing_alias_unref(uint64_t src, uint64_t n) {
    GMU_LOCK();
    backing_alias_unref_locked(src, n);
    GMU_UNLOCK();
}
#endif

// 调用者已经取得 thread owner 的真实 writers 安全点；GPU回调不得持有 VM 查询所需的锁。
static int backing_begin_locked(uint64_t dst, uint64_t n, uint64_t source) {
#ifdef __ANDROID__
    // g_mu 可解锁；外层 svc_mem 的独占 writer epoch 保持至整个内存 SVC 返回。
    GMU_UNLOCK();
    {
        // 后端（图形层）可在这里消费自己的“热窗口迁移请求”：此刻 guest writers 已被外层
        // 独占 writer epoch 停稳，属于合同要求的真实安全点；后端必须做完自己的 begin/end 事务再返回。
        // 非 0 = 后端没能把窗口升级成“全体停稳”（epoch 已撤销）：按合同这次映射事务必须终止，
        // 不能再调 s2a_track_backing_begin（那等于在没停稳的状态下改映射）。
        extern int s2a_android_mem_owner_window(void) __attribute__((weak));
        int we = s2a_android_mem_owner_window ? s2a_android_mem_owner_window() : 0;
        if (we) {
            GMU_LOCK();
            return we;   // 调用方（svc_mem 的改表分支）统一回 0xD401，映射未改
        }
    }
    int e = s2a_track_backing_begin(dst, n, source);
    GMU_LOCK();
    if (!e) s2a_android_mirror_begin(dst, (size_t)n);   // 开关关时入口第一行判标志即返回，不翻世代号
    return e;
#else
    // 苹果：按宿主页取整（写跟踪按 16 KB 页记账）；等在途排空期时放开 g_mu（排空期里 C# 经 IsRangeMapped 进 s2a_mem_mapped），
    //   放开期间别的改表调用由 svc_mem() 外层 g_vm_writer 挡住（审计 #806 第 5 条续：原先苹果这里是空函数，先改保护 / 换页、后报 prot_changed，窗口里排空按旧记账读到刚设成不可访问的页）
    if (!g_bk_begin || !n) return 0;
    uint64_t lo = dst & ~(HPG - 1), hi = (dst + n + HPG - 1) & ~(HPG - 1);
    GMU_UNLOCK();
    int e = g_bk_begin(lo, hi - lo, source);
    GMU_LOCK();
    return e;
#endif
}
static void backing_end_locked(uint64_t dst, uint64_t n, int mapped, int committed) {
#ifdef __ANDROID__
    // 镜像开：先把 tracker 记账清到新世代（lastp=0xfe / act=0），再让 mirror_end 翻回偶数发布新页（#1184 D1）。
    // 顺序反过来时，ESTALE 重试的写者会在新偶数世代拿到别名，却读到旧 lastp/act 通过前提复核，
    // 把旧 GPU 数据写进刚换入的页。reset 仍在本事务临界区内，写者计数已保证没有在途写者。
    // 镜像关（缺省）：不调 reset_before_publish、不碰 mirror_end，回到 main 的「放锁 → backing_end」原时序；
    // 关时没有世代 / ESTALE 重试，s2a_track_backing_end 里的 !backing.reset 分支会照旧清一次。
    if (s2a_android_mirror_enabled()) {
        s2a_track_backing_reset_before_publish(dst, n);
        s2a_android_mirror_end((uintptr_t)dst, (size_t)n, mapped, committed);
    }
    GMU_UNLOCK();
    s2a_track_backing_end(dst, n, mapped, committed);
    GMU_LOCK();
#else
    if (!g_bk_end || !n) return;
    uint64_t lo = dst & ~(HPG - 1), hi = (dst + n + HPG - 1) & ~(HPG - 1);
    g_bk_end(lo, hi - lo, mapped, committed);   // 只拿写跟踪锁、不等排空：持 g_mu 调（锁序 svc_mem → 写跟踪，同 host_write）
#endif
}
// 映射目标区：在地址空间内、不在堆区 / 别名区（Ryujinx IsInvalidRegion / InsideHeapRegion / InsideAliasRegion → InvalidMemRange），且目前空闲
static uint64_t map_target_ok(uint64_t a, uint64_t n) {
    if (!in_as(a, n) || (a < L.heap_base + L.heap_size && L.heap_base < a + n) || (a < L.alias_base + L.alias_size && L.alias_base < a + n)) return 0xDC01;
    if (map_find(a, n) || span_busy(a, a + n)) return 0xD401;
    return 0;
}
#ifdef __ANDROID__
// 只在已持writers安全点、完成GPU before且目标属自有窗口时使用。
// 新 guest 页一律映射到单一 memfd（pgoff = VA）：同一文件的相邻 pgoff 内核可以合并 VMA，
// 逐页提交 1 万页仍是一段；PROT_NONE 占位也用同一 memfd，才能和两侧 RW 段合并。
// 失败不回退匿名映射：回退会让「已换页」的语义分叉，直接中止。
static void replace_pages_locked(uint64_t address, uint64_t size, int mapped) {
    uint32_t prot = mapped ? (PLAT_PROT_READ | PLAT_PROT_WRITE) : 0;
    int error = plat_backing_map((void *)address, size, prot, address);
    if (error) die("Android guest 页背板映射失败 errno=%d（%#llx+%#llx，不回退匿名映射）",
                   error, (unsigned long long)address, (unsigned long long)size);
}
static int physical_changes(uint64_t address, int mapping) {
    uint8_t bit = (uint8_t)(1u << ((address & (HPG - 1)) >> 12));
    return (!!(*amask_at(address) & bit)) != mapping;
}
// 持 g_mu：这次 0x2C 是否只向“实际操作宿主页整页都还没有活子页”的页面增加新零页。
// 只满足这个条件才能免全体停稳：没有旧内容要回写，也不替换任何邻居；0x2D 永不进入。
static int physical_growth_empty_locked(uint64_t a, uint64_t size) {
    if (!size || ((a | size) & 0xfff) || a + size < a) return 0;
    if (a < L.alias_base || a + size > L.alias_base + L.alias_size) return 0;
    size_t page = plat_page_size();
    if (!page || (page & (page - 1)) || page < 0x1000) return 0;
    int changed = 0;
    for (uint64_t cursor = a, end = a + size; cursor < end;) {
        if (!physical_changes(cursor, 1)) { cursor += 0x1000; continue; }
        uint64_t first = cursor;
        do { cursor += 0x1000; } while (cursor < end && physical_changes(cursor, 1));
        if (first % page || (cursor - first) % page) return 0;
        for (uint64_t host = first; host < cursor; host += page)
            for (uint64_t p = host; p < host + page; p += 0x1000) {
                uint8_t bit = (uint8_t)(1u << ((p & (HPG - 1)) >> 12));
                if (*amask_at(p) & bit) return 0;
            }
        changed = 1;
    }
    return changed;
}
// 只读预检：先决定走只串行还是全体停稳；映射表锁内的复核会拦住预检后的变化并升级。
static int physical_growth_empty(uint64_t a, uint64_t size) {
    GMU_LOCK();
    int eligible = physical_growth_empty_locked(a, size);
    GMU_UNLOCK();
    return eligible;
}
#endif
// 代码内存对象（CreateCodeMemory，句柄子号 2）：游戏交出一段自己的内存给 JIT 用，ControlCodeMemory 再把它映成可写（Map）/ 给自己只读或可执行（MapToOwner）
typedef struct { int refs; uint64_t addr, size, map_rw, map_ro; } codemem_t;
// 代码内存对象最后一个句柄关掉（thread.c close_handle，放 G 之后调：这里取 g_mu）：照 Ryujinx KCodeMemory.Destroy，两处映射都已解除才还回源，否则源保持借出
void s2a_mem_codemem_free(void *o) {
    codemem_t *c = o;
    GMU_LOCK(); if (!c->map_rw && !c->map_ro) unborrow_locked(c->addr, c->size); GMU_UNLOCK();   // 映射标记由 0x4C 持 g_mu 改
    free(c);
}
static uint64_t chk_ars(uint64_t a, uint64_t n) { return a & 0xfff ? 0xCC01 : (n & 0xfff) || !n ? 0xCA01 : a + n <= a ? 0xD401 : 0; }
// 0x2C 全空宿主页快路升级全停稳到期后的重试上限：每轮各自有 2 s 预算，映射未改，可安全重来；
// 上限到了回 0xD401（InvalidMemState），不再让一次宿主协调超时 abort 掉整局。
#define S2A_PHYS_RETRY_MAX 3
static _Thread_local int g_phys_retry;

static int svc_mem_impl(int num, uint64_t *regs, uint64_t orig) {
    s2a_svc_frame(orig);
    switch (num) {
    case 0x01: {  // SetHeapSize(→x1 堆基址; x1 大小)
        uint64_t size = regs[1];
        // 出参先置 0，失败的每条路都带着它返回（同 Ryujinx Syscall.SetHeapSize / KPageTableBase.SetHeapSize 开头 address = 0）：原先失败时 x1 原样留着请求的大小，
        //   不看结果码的游戏拿它当堆基址往下写（D-ASK2 iPad mini 6：设堆 0xD001 后照样初始化堆，写进宿主只读代码段 SIGBUS）
        regs[1] = 0;
        if (size & 0x1fffff) { regs[0] = 0xCA01; return 1; }   // InvalidSize：Horizon 要 2 MB 对齐
        GMU_LOCK();
        uint64_t fixed = pool_fixed();
        if (size > UINT64_MAX - fixed || !pool_fits(fixed + size, NULL)) { GMU_UNLOCK(); regs[0] = 0x10801; return 1; }   // 先防加法绕回
        if (size > g_pk_heap_request) { g_pk_heap_request = size; addr_need_report(); }
        if (size > heap_region()) {
            GMU_UNLOCK();
            // 出包时堆按应用池上限给、别名优先的款按静态识别让堆——到这里说明那两条都没覆盖到（堆大小是运行时算的，D-NGH9）：记下本款要的堆，下一局布局至少给到这么多
            int noted = heap_need_note(g_lay_pid, size);
            if (noted) { atomic_store(&g_heap_noted, 1); if (size > atomic_load(&g_heap_noted_size)) atomic_store(&g_heap_noted_size, size); }   // 游戏随后 Break（拿到 0xD001 中止）时按「需要更多内存、已记下」收（见 Break）
            char b[320]; snprintf(b, sizeof b, "普通堆需要 %llu 字节，堆区只有 %llu 字节（出包缺省 / 静态识别没覆盖到这款）：%s",
                (unsigned long long)size, (unsigned long long)heap_region(), noted ? "已记下本款要的堆，下次起局堆区按它给" : "记录写不进，下次起局仍按缺省");
            svc_fail_log(num, 0xD001, b); regs[0] = 0xD001; return 1;
        }
        if (size > g_heap_cur && map_find((uint64_t)g_heap + g_heap_cur, size - g_heap_cur)) { GMU_UNLOCK(); regs[0] = 0xD401; return 1; }
        // 缩堆：还掉的尾段有借出的页（MapMemory / 代码内存 / 传输内存的源）回 InvalidMemState——Ryujinx KPageTableBase.SetHeapSize 缩堆要属性 None。
        //   原先照还：之后 UnmapMemory 往已还掉的页拷回，宿主 SIGSEGV
        if (size < g_heap_cur && attr_any((uint64_t)g_heap + size, (uint64_t)g_heap + g_heap_cur)) { GMU_UNLOCK(); regs[0] = 0xD401; return 1; }
        // 堆从窗口里长出窗口末尾（乙布局堆区伸出窗口时才会发生）：宿主没有这段地址
        if (size > g_heap_cur && (uint64_t)g_heap + size > s2a_far_floor()) { GMU_UNLOCK(); regs[0] = va_short(num, (uint64_t)g_heap + g_heap_cur, size - g_heap_cur, "堆区（与远端影子撞上）"); return 1; }
        if (size > g_heap_cur && (uint64_t)g_heap >= L.win_base && (uint64_t)g_heap + size > L.win_base + L.win_size) {
            // 小内存 iPad：堆请求超出本机窗口（不是超出堆区，所以走不到上面的 heap_need_note）。记下本款要的堆，
            //   下一局让 vawin 按它给窗口；这一局照上游口径停局报「本机地址空间装不下」，不再让不看结果码的游戏往下写宿主内存。
            int noted = heap_need_note(g_lay_pid, size);
            if (noted) atomic_store(&g_heap_noted, 1);
            GMU_UNLOCK(); regs[0] = va_short(num, (uint64_t)g_heap + g_heap_cur, size - g_heap_cur, "堆区");
            if (noted) LOG("普通堆需要 %llu 字节，本机窗口只有 %llu MB：已记下本款要的堆，下一局按它给窗口",
                (unsigned long long)size, (unsigned long long)(L.win_size >> 20));
            // 窗口是本机给得出的最大一块（按需档才会让堆区伸出窗口）：这么大的堆物理上放不下。停在这里报真实原因：真机上堆从不失败，不看结果码的游戏
            //   拿到 0xD001 照样往下写，写进地址更低处的宿主映像 SIGBUS（#01b36b D-SSBZ iPad14,1 动森：窗口 3072 MB，要 3198 MB 的堆，写到宿主代码段；
            //   同族 #1eefc4 小地址设备上的一款 Switch 2 游戏、D-ASK2）。看结果码的游戏原本也是拿到它就中止，结果一样，原因说清了
            LOG("【红】本机地址空间装不下：游戏要 %llu MB 的堆，堆区起点到地址窗口末端只有 %llu MB（窗口 %llu MB 已是本机给得出的最大一块）——物理上放不下，停局报原因",
                (unsigned long long)(size >> 20), (unsigned long long)((L.win_base + L.win_size - (uint64_t)g_heap) >> 20), (unsigned long long)(L.win_size >> 20));
            s2a_init_failed(S2A_INIT_FAIL_E_DEVICE_ADDRESS);   // 宿主里不返回；只链本文件的单测回到这里，照旧回 0xD001
            return 1;
        }
        uint64_t lo = (uint64_t)g_heap + (size < g_heap_cur ? size : g_heap_cur), n = size > g_heap_cur ? size - g_heap_cur : g_heap_cur - size;
#ifdef __ANDROID__
        int growing = size > g_heap_cur;
        size_t page = plat_page_size();
        // 复用保护：NVN 长寿别名还引用着这段（增长段）时，同一个 VA 不能再拿到新页。
        // 这是 pgoff=VA 的固有限制；回 InvalidMemState 并打红字，等别名解开后再试。
        if (n && growing && backing_refs_checked(lo, n, "堆增长")) {
            GMU_UNLOCK();
            backing_reuse_denied("SetHeapSize 增长", lo, n, &g_backing_reuse_denied);
            regs[0] = 0xD401; return 1;
        }
        if (n && (!page || lo % page || n % page || backing_begin_locked(lo, n, 0))) {
            GMU_UNLOCK(); regs[0] = 0xD401; return 1;
        }
        if (n) {
            if (growing) {
                int pe = backing_prepare_locked(lo, n);
                if (pe == EAGAIN) {   // 放锁窗口里有 NVN ref 进来：回滚本段事务，回 0xD401
                    backing_end_locked(lo, n, 0, 0);
                    GMU_UNLOCK();
                    backing_reuse_denied("SetHeapSize 增长（放锁窗口）", lo, n, &g_backing_reuse_denied);
                    regs[0] = 0xD401; return 1;
                }
                if (pe) die("SetHeapSize 提交前打洞失败 errno=%d（%#llx+%#llx）", pe, (unsigned long long)lo, (unsigned long long)n);
            }
            replace_pages_locked(lo, n, growing);
        }
#else
        int shrink = size < g_heap_cur;
        if (backing_begin_locked(lo, n, 0)) { GMU_UNLOCK(); regs[0] = 0xD401; return 1; }
        if (size > g_heap_cur) GUEST_COMMIT(g_heap + g_heap_cur, size - g_heap_cur);
        if (size > g_heap_cur && mprotect(g_heap + g_heap_cur, size - g_heap_cur, PROT_READ | PROT_WRITE)) die("SetHeapSize mprotect errno=%d", errno);
        if (size > g_heap_cur) punch_zero_locked((uint64_t)g_heap + g_heap_cur, (uint64_t)g_heap + size);   // #258：增长段里待清零的旧页就地清零
        if (shrink) pages_release((uint64_t)g_heap + size, g_heap_cur - size);
#endif
        vm_mutation_check("堆大小改动"); g_heap_cur = size; if (size > g_pk_heap) g_pk_heap = size;
        perm_set(lo, lo + n, -1);
        mem_peak_note();
#ifdef __ANDROID__
        // 缩掉的尾段已死：仍在事务里（backing_end 之前）按宿主页打洞还内存。
        if (n && !growing) backing_release_locked(lo, n, BACKING_PUNCH_RELEASE);
        if (n) backing_end_locked(lo, n, growing, 1);
#else
        backing_end_locked(lo, n, !shrink, 1);   // 换页事务收尾：重算这段记账（原先放锁后才 prot_changed）
#endif
        GMU_UNLOCK();
#ifndef __ANDROID__
        if (shrink) backing_changed(lo, n);
#endif
        regs[0] = 0; regs[1] = (uint64_t)g_heap; return 1;
    }
    case 0x02: {  // SetMemoryPermission(x0 地址, x1 大小, w2 权限)：游戏页 4 KB、宿主页 16 KB。收紧（如 DT_INIT 后把 relro 改只读）只改完整落在区间内的宿主页，
                  // 放宽按外扩到整页改——宿主页粒度下离请求最近、且不会让游戏要访问的页不可访问
        uint64_t a = regs[0], n = regs[1]; uint32_t p = (uint32_t)regs[2];
        // 校验照 Ryujinx Syscall.SetMemoryPermission：地址 / 大小 / 回绕 → 权限只许 无 / 读 / 读写 → 在游戏地址空间内（原先窗口外宿主内存也照改）
        { uint64_t e = chk_ars(a, n); if (!e && p != 0 && (p | 2) != 3) e = 0xD801; if (!e && !in_as(a, n)) e = 0xD401; if (e) { regs[0] = e; return 1; } }
        int pr = (p & 1 ? PROT_READ : 0) | (p & 2 ? PROT_WRITE : 0), widen = (p & 2) != 0;
        uint64_t page = memory_protection_page();
        if (!page) { regs[0] = 0xD401; return 1; }
        uint64_t lo = widen ? a & ~(page - 1) : (a + page - 1) & ~(page - 1), hi = widen ? (a + n + page - 1) & ~(page - 1) : (a + n) & ~(page - 1);
        // #815 / #523（0100FA501AF90000，补丁 @rugal7699）：游戏把模块映像（main / subsdk / sdk / NRO）里的一段收紧成只读后，游戏 SDK 自己的 nn::ro 装 NRO 时
        //   还会往这段（main+0x579508）写 → SIGBUS。模块映像里的收紧只记游戏可见权限（下面 perm_set 照记，QueryMemory 照回只读），
        //   不改宿主保护；S2A_MOD_RELRO_ENFORCE=1 恢复旧行为。放宽（widen）照旧。
        {
            static int enforce = -1;
            if (enforce < 0) { const char *ev = getenv("S2A_MOD_RELRO_ENFORCE"); enforce = ev && *ev == '1'; }
            module_t *mm = n ? module_of((const void *)(uintptr_t)a) : NULL;
            int in_mod = !widen && mm && module_of((const void *)(uintptr_t)(a + n - 1)) == mm;
            uint64_t mb = mm ? (uint64_t)(uintptr_t)mm->base : 0;
            static _Atomic int said;   // 只记模块映像内的收紧（游戏可能频繁改别处权限），每局至多 16 行
            if (in_mod && atomic_fetch_add(&said, 1) < 16)
                LOG("SetMemoryPermission %s+%#llx 大小 %#llx 权限 %u → 宿主 [%#llx, %#llx)%s",
                    mm->name, (unsigned long long)(a - mb), (unsigned long long)n, p,
                    (unsigned long long)(lo - mb), (unsigned long long)(hi > lo ? hi - mb : lo - mb),
                    !enforce ? "（模块映像内收紧：只记游戏权限，宿主保持可写）" : "（S2A_MOD_RELRO_ENFORCE=1：照旧设宿主只读）");
            if (in_mod && !enforce) hi = lo;   // 跳过下面的宿主 mprotect / prot_changed
        }
        // 远端（别名区伸出宿主窗口的那段）只是承诺：页的保护由远端探针 / 翻译层管，这里不改宿主保护
        uint64_t we = L.win_base + L.win_size; if (hi > we && lo < we && s2a_far_has(we, hi - we)) hi = we; else if (lo >= we && s2a_far_has(lo, hi - lo)) hi = lo;
        if (hi > lo) {
            // 跟踪器在同一个每页临界区里先占页再 mprotect（与缺页自愈串行）；没有挂这个钩子（单测夹具）时退回直接 mprotect。
            //   苹果也走（10-28 审计 #806 第 1 条：原先苹果先 mprotect 再 prot_changed，中间的写缺页被自愈改回可写）
            if (pr) GUEST_COMMIT(lo, hi - lo);   // 放宽到一段从没放开过的地址（多半已放开、块已定，不加锁直接回）
            int ge = g_hooks.guest_protect ? g_hooks.guest_protect(lo, hi - lo, pr) : (mprotect((void *)lo, hi - lo, pr) ? errno : 0);
            if (ge) { regs[0] = 0xD401; return 1; }
            prot_changed(lo, hi - lo);   // 通知（生产就是跟踪器；重复收尾幂等）
        }
        // 游戏看到的权限按请求的 4 KB 范围记（宿主页取整只影响宿主保护）；改表串行由 svc_mem() 外层 writer 保证（审计 P1-10）
        GMU_LOCK(); perm_set(a, a + n, (int)p); GMU_UNLOCK();
        regs[0] = 0; return 1;
    }
    case 0x03: {   // SetMemoryAttribute(x0 地址, x1 大小, w2 掩码, w3 值)：只有无缓存 / 锁权限两种属性，苹果内存一致，校验过即回成功（属性不记）
        // 校验照 Ryujinx Syscall.SetMemoryAttribute：地址 / 大小 → 掩码 | 值 = 掩码、只含 Uncached(8) / PermissionLocked(0x10) → 锁权限不许清 → 在地址空间内
        uint64_t a = regs[0], n = regs[1]; uint32_t mk = (uint32_t)regs[2], vl = (uint32_t)regs[3], at = mk | vl;
        uint64_t e = a & 0xfff ? 0xCC01 : (n & 0xfff) || !n ? 0xCA01 : 0;
        if (!e && (at != mk || (at | 0x18) != 0x18)) e = 0xE801;            // InvalidCombination
        if (!e && (mk & 0x10) != (vl & 0x10)) e = 0xE801;
        if (!e && !in_as(a, n)) e = 0xD401;
        regs[0] = e; return 1;
    }
    case 0x80: {   // Switch 2（Pokopia 0400A3A01EE4C000）新号：sdk 唯一调用者是 nn::gpu::sdk::CreateGpuAccessibleMemory / nn::gs 原生内存的建 / 拆 / 查，
        //   建时传 (地址, 大小, 0x20, 0x20)、拆时传 (地址, 大小, 0x20, 0)，与 0x03 SetMemoryAttribute 的「地址, 大小, 掩码, 值」同形，按「GPU 可访问」属性位 0x20 理解
        //   （语义是按调用点推断的，不是规格）。苹果统一内存 CPU / GPU 本就一致：校验同 0x03（多认 0x20 位）、回成功、属性不记
        uint64_t a = regs[0], n = regs[1]; uint32_t mk = (uint32_t)regs[2], vl = (uint32_t)regs[3], at = mk | vl;
        uint64_t e = a & 0xfff ? 0xCC01 : (n & 0xfff) || !n ? 0xCA01 : 0;
        if (!e && (at != mk || (at | 0x38) != 0x38)) e = 0xE801;
        if (!e && (mk & 0x10) != (vl & 0x10)) e = 0xE801;
        if (!e && !in_as(a, n)) e = 0xD401;
        { static _Atomic int said; if (!atomic_exchange(&said, 1)) LOG("系统调用 0x80（Switch 2，按调用点推断为设内存属性 0x20 = GPU 可访问）：掩码 %#x 值 %#x 回 %#llx", mk, vl, (unsigned long long)e); }
        regs[0] = e; return 1;
    }
    case 0x04: {  // MapMemory(x0 目标, x1 源, x2 大小)：SDK 建线程时把堆上的栈搬进栈区。宿主页对齐时共享映射，否则拷贝（映射期间源区不被访问）
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        // 校验照 Ryujinx Syscall.MapMemory：源 / 目标对齐、大小、回绕 → 源在地址空间内 → 目标在栈区内（不在堆区 / 别名区）→ 目标已占回 InvalidMemState
        const char *why = NULL;
        uint64_t e = 0;
        if ((src | dst) & 0xfff) { e = 0xCC01; why = "alignment"; }
        else if ((size & 0xfff) || !size) { e = 0xCA01; why = "size"; }
        else if (src + size <= src || dst + size <= dst) { e = 0xD401; why = "overflow"; }
        else if (!in_as(src, size)) { e = 0xD401; why = "source-window"; }
        if (e) {
            if (map_failure_claim()) {
                map_failure f = map_failure_new(dst, src, size, e, orig, 0, why);
                GMU_LOCK(); map_failure_snapshot(&f, "post-validation"); GMU_UNLOCK();
                map_failure_log(&f);
            }
            regs[0] = e; return 1;
        }
        GMU_LOCK();
        if (dst < L.stack_base || dst + size > L.stack_base + L.stack_size || map_find(dst, size)) {
            int range = dst < L.stack_base || dst + size > L.stack_base + L.stack_size;
            e = range ? 0xDC01 : 0xD401;
            int report = map_failure_claim();
            map_failure f = map_failure_new(dst, src, size, e, orig, 0, range ? "stack-range" : "destination-overlap");
            if (report) map_failure_snapshot(&f, "failure");
            GMU_UNLOCK();
            if (report) map_failure_log(&f);
            regs[0] = e; return 1;
        }
        // 源要整段已映射且可读（真机要源是可映射状态、读写：审计 P1-11），否则下面的 memcpy 碰到未提交 / 不可读的页直接 SIGSEGV；
        //   源不能是借出的（传输内存 / 代码内存 / 另一次 MapMemory 的源，Ryujinx Map 核源属性 None）
        if (!readable(src, src + size) || attr_any(src, src + size)) {
            int report = map_failure_claim();
            map_failure f = map_failure_new(dst, src, size, 0xD401, orig, 0, "source-state");
            if (report) map_failure_snapshot(&f, "failure");
            GMU_UNLOCK();
            if (report) map_failure_log(&f);
            regs[0] = 0xD401; return 1;
        }
        uint64_t page = memory_protection_page();
        if (!page) {
            int report = map_failure_claim();
            map_failure f = map_failure_new(dst, src, size, 0xD401, orig, 0, "host-page-size");
            if (report) map_failure_snapshot(&f, "failure");
            GMU_UNLOCK();
            if (report) map_failure_log(&f);
            regs[0] = 0xD401; return 1;
        }
        uint64_t lo = dst & ~(page - 1), hi = (dst + size + page - 1) & ~(page - 1);
        if (!win_has(lo, hi - lo)) {
            int report = map_failure_claim();
            map_failure f = map_failure_new(dst, src, size, 0xD001, orig, 0, "host-window");
            if (report) map_failure_snapshot(&f, "failure");
            GMU_UNLOCK();
            if (report) map_failure_log(&f);
            regs[0] = va_short(num, dst, size, "栈区"); return 1;
        }
#ifndef __ANDROID__
        // 源 / 目标 / 大小都按宿主页对齐：目标共享映射源的同一批物理页（同 0x13 的 alias_map），映射期间两边是同一份，解除时不拷回、目标换回不可访问。
        //   源页不改不可访问：宿主页 16 KB 会连带堆里的邻居，真机上游戏本就不碰借出的源。不对齐（SDK 栈只保证 4 KB）照旧拷贝
        if (!((dst | src | size) & (HPG - 1))) {
            if (backing_begin_locked(dst, size, src)) { GMU_UNLOCK(); regs[0] = 0xD401; return 1; }
            uint64_t ae = alias_map(dst, src, size, VM_PROT_READ | VM_PROT_WRITE);
            int mpe = ae ? 0 : mprotect((void *)dst, size, PROT_READ | PROT_WRITE) ? errno : 0;   // 源被设过只读时 vm_remap 随源保护，栈要读写
#ifdef S2A_UNIT_TEST
            if (!ae && !mpe && s2a_mem_test_alias_eacces) mpe = EACCES;
#endif
            if (mpe == EACCES) {
                // 共享映射的最大权限没有「写」（#4c853c：iPad 上游戏把线程栈放在主程序数据段、堆用 0 字节，数据段是从签名镜像文件私有映射来的，
                //   vm_remap 共享过去最大权限只剩读，mprotect 读写 EACCES，原先 die 闪退；Mac 上同样的映射最大权限照旧可写）。
                //   撤回这次共享映射，改走下面的拷贝路径（不对齐时本来就走它；解除时把内容搬回源）
                alias_unmap(dst, size, src, PROT_READ | PROT_WRITE);
                backing_end_locked(dst, size, 0, 1);
                static _Atomic int said; if (!atomic_exchange(&said, 1)) LOG("MapMemory：%#llx 共享映射的最大权限不含写（源 %#llx 在只能私有映射的区域），改用拷贝", (unsigned long long)dst, (unsigned long long)src);
            } else {
                if (mpe) die("MapMemory 共享映射 mprotect errno=%d", mpe);
                if (ae) { backing_end_locked(dst, size, 0, 0); GMU_UNLOCK(); regs[0] = ae; return 1; }
                punch_forget_locked(dst, dst + size);   // #258：目标原映射被盖掉，这段若是上次 UnmapMemory 释放的待清零页，只撤销标记
                map_add(dst, size, src, ST_STACK, MAP_ALIAS);
                backing_end_locked(dst, size, 1, 1);
                GMU_UNLOCK();
                regs[0] = 0; return 1;
            }
        }
        // 拷贝前同步源侧（审计 P1-48，同安卓同一协议）：GPU 改过的子页先回写到 CPU、跟踪中的页先解开，再 memcpy，否则拷进栈的是回写前的旧字节。
        //   换页事务 begin 不算：track.c 的 s2a_track_backing_begin 只等排空期结束、挡新排空期，不看 source、不回写不解开。
        //   不持 g_mu（回写回调可能经 IsRangeMapped 查映射）、放在 begin 之前（回写可能要等排空期，事务挂着时新排空期进不来）；别的改表调用由外层 g_vm_writer 挡住。
        //   安卓同一步在下面 mprotect 之后（源在堆、目标在栈区，两者不相交，先后无关）
        GMU_UNLOCK();
        host_write(src, size);
        GMU_LOCK();
        if (backing_begin_locked(lo, hi - lo, src)) { GMU_UNLOCK(); regs[0] = 0xD401; return 1; }
#endif
        GUEST_COMMIT(lo, hi - lo);   // 拷贝路径：目标是栈区的匿名占位
        if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) {
            int saved_errno = errno;
            if (map_failure_claim()) {
                map_failure f = map_failure_new(dst, src, size, 0, orig, saved_errno, "host-mprotect-fatal");
                map_failure_snapshot(&f, "failure");
                map_failure_log(&f);
            }
            die("MapMemory mprotect errno=%d", saved_errno);
        }
#ifndef __ANDROID__
        punch_zero_locked(lo, hi);   // #258：目标里上次 UnmapMemory 释放的待清零页内容是旧的，先清零再拷（memcpy 只盖 [dst, dst+size)）
#endif
#ifdef __ANDROID__
        // 实际writers安全点保护映射表；同步GPU回写/复制时不持g_mu，回调可查询VM。
        // g_mu 解锁期间外层仍持独占 writer epoch，其他替页或栈搬移事务不得进入。
        // 复用现有跟踪写预备协议保守地同步并解开源页，不能读untracked旧GPU内容。
        GMU_UNLOCK();
        host_write(src, size);
        memcpy((void *)dst, (void *)src, size);
        GMU_LOCK();
#else
        memcpy((void *)dst, (void *)src, size);
#endif
        map_add(dst, size, src, ST_STACK, 0);
#ifndef __ANDROID__
        backing_end_locked(lo, hi - lo, 1, 1);
#endif
        GMU_UNLOCK();
#ifdef __ANDROID__
        prot_changed(lo, hi - lo);
#endif
        regs[0] = 0; return 1;
    }
    case 0x05: {  // UnmapMemory(x0 目标, x1 源, x2 大小)：共享映射直接解除；拷贝的把内容搬回源区，归还完全落在区间内的整宿主页
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        GMU_LOCK();
        int k = map_find(dst, size);
        if (!k || g_map[k - 1].base != dst || g_map[k - 1].size != size || g_map[k - 1].src != src) { GMU_UNLOCK(); regs[0] = 0xD401; return 1; }
        uint64_t page = memory_protection_page();
        if (!page) { GMU_UNLOCK(); regs[0] = 0xD401; return 1; }
#ifndef __ANDROID__
        if (g_map[k - 1].h == MAP_ALIAS) {   // 共享映射（0x04 对齐时）：内容本就在源里，目标换回不可访问的保留（解除后访问目标应失败）
            if (backing_begin_locked(dst, size, 0)) { GMU_UNLOCK(); regs[0] = 0xD401; return 1; }
            alias_unmap(dst, size, src, PROT_READ | PROT_WRITE);
            map_del(k - 1);
            backing_end_locked(dst, size, 0, 1);
            GMU_UNLOCK();
            regs[0] = 0; return 1;
        }
#endif
        // 两侧都可能被GPU改过；先回写，再复制。不能持VM锁等待touch→MemQuery。
        // g_mu 解锁期间外层仍持独占 writer epoch（苹果：g_vm_writer），其他替页或栈搬移事务不得进入。
        // 两平台同一段（审计 P1-48）：原先苹果只回写源、不回写栈（目标），且持 g_mu 调回写
        GMU_UNLOCK();
        host_write(dst, size);
        host_write(src, size);
        memcpy((void *)src, (void *)dst, size);
        GMU_LOCK();
        uint64_t lo = (dst + page - 1) & ~(page - 1), hi = (dst + size) & ~(page - 1);
#ifndef __ANDROID__
        // 归还整宿主页（换成不可访问占位）也是换页：事务包住，收尾重算（原先苹果这里不报，跟踪里还是旧快照 / 旧哈希）
        if (hi > lo && backing_begin_locked(lo, hi - lo, 0)) { GMU_UNLOCK(); regs[0] = 0xD401; return 1; }
#endif
#ifdef __ANDROID__
        // On 4K hosts revoke the exact guest page; 16K hosts retain the existing policy.
        if (page == 0x1000 && hi > lo && mprotect((void *)lo, hi - lo, PROT_NONE)) die("UnmapMemory mprotect errno=%d", errno);
        // 栈区仍是私有匿名（#1217 Q8：本卡不迁栈），MADV_FREE 在这里是有效的懒回收；
        // 返回值原先不检查，失败时会被当成回收成功。只报红并继续（语义上映射已解除）。
        if (hi > lo && madvise((void *)lo, hi - lo, MADV_FREE))
            LOG("【红】UnmapMemory madvise(MADV_FREE) errno=%d（%#llx+%#llx）", errno,
                (unsigned long long)lo, (unsigned long long)(hi - lo));
#else
        // 完全落在区间内的整宿主页走已有的还页（换成不可访问的匿名占位，审计 P2-9）：原先只 madvise(MADV_FREE)，页仍映射读写，
        //   游戏再读「已解除」的栈读到旧内容、不缺页。盖不满一个宿主页的边缘不动（16 KB 宿主页连着邻居）
        if (hi > lo) pages_release(lo, hi - lo);
#endif
        map_del(k - 1);
#ifndef __ANDROID__
        if (hi > lo) backing_end_locked(lo, hi - lo, 0, 1);
#endif
        GMU_UNLOCK();
#ifdef __ANDROID__
        if (page == 0x1000 && hi > lo) prot_changed(lo, hi - lo);
#else
        if (hi > lo) backing_changed(lo, hi - lo);   // 换了页：渲染器包着这段的零拷贝缓冲作废重建（pages_release 约定）
#endif
        regs[0] = 0; return 1;
    }
    case 0x06: {  // QueryMemory(x0 MemoryInfo*, x1 PageInfo*, x2 地址) → x1 = PageInfo
        s2a_meminfo mi; s2a_mem_query(regs[2], &mi);
        // 36 位款查 Horizon 36 位的固定代码 / 栈区 [0x8000000, 0x80000000)（Ryujinx AddressSpace64BitDeprecated CodeRegionStart / stackAndTlsIo）：
        //   老 SDK 不问 GetInfo、把这段写死（11-06 Minecraft: Story Mode nnosInitialize：线程栈分配器 [0x8000000, +0x80000000)），而游戏地址 = 宿主地址，
        //   Apple arm64 主程序的 __PAGEZERO 必须盖满 4 GB（改小内核直接杀进程），这段宿主给不出——照实报不可访问，打一次红说清
        if (L.as_type == 1 && regs[2] >= 0x8000000ull && regs[2] < 0x100000000ull && regs[2] < L.win_base) {
            static _Atomic int said; if (!atomic_exchange(&said, 1))
                LOG("【红】36 位款查询 Horizon 固定代码 / 栈区地址 %#llx（[0x8000000, 0x80000000)，老 SDK 写死不问 GetInfo）：宿主 4 GB 以下是 __PAGEZERO，游戏地址 = 宿主地址给不出，报不可访问；"
                    "出包改写器没认出写死这段的代码形状（runtime/rewriter sdk_as36_ranges：认出时换成本局可随机区 / 栈区，#1503）", (unsigned long long)regs[2]); }
        if (regs[0]) { host_write(regs[0], 0x28); uint8_t *o = (uint8_t *)regs[0]; memset(o, 0, 0x28); memcpy(o, &mi.base, 8); memcpy(o + 8, &mi.size, 8); memcpy(o + 0x10, &mi.state, 4); memcpy(o + 0x14, &mi.attr, 4); memcpy(o + 0x18, &mi.perm, 4); }
        regs[0] = 0; regs[1] = 0; return 1;
    }
    case 0x13: case 0x14: {  // Map / UnmapSharedMemory(w0 句柄, x1 地址, x2 大小[, w3 权限])：后备别名到游戏地址（同一批物理页）
        uint64_t n = 0; uint8_t *buf = s2a_shmem_lookup((uint32_t)regs[0], &n); uint64_t a = regs[1];
        // 校验照 Ryujinx Syscall.Map / UnmapSharedMemory：地址 / 大小 / 回绕 →（映射）权限只许 读 / 读写 → 句柄 → 大小 = 共享内存大小（KSharedMemory 回 InvalidSize）
        { uint64_t e = chk_ars(a, regs[2]); if (!e && num == 0x13 && ((uint32_t)regs[3] | 2) != 3) e = 0xD801; if (!e && !buf) e = 0xE401; if (!e && regs[2] != n) e = 0xCA01;
          if (e) { regs[0] = e; return 1; } }
#ifdef __ANDROID__
        // HPG 仍是别名区位图/TLR 的16KB分组；Linux映射原语使用实际宿主页。
        size_t page = plat_page_size();
        if (!page) { regs[0] = 0xD401; return 1; }
        uint64_t span = (n + page - 1) & ~((uint64_t)page - 1);
#else
        uint64_t span = (n + HPG - 1) & ~(HPG - 1);
#endif
        if (num == 0x13) {
#ifdef __ANDROID__
            if (a & (page - 1)) { regs[0] = 0xD401; return 1; }
#else
            // 只接受 16 KB 对齐的游戏地址：宿主页 16 KB，不对齐就得重定位宿主缓冲（旧库做法，要服务配锁）。回 0xD401 让 SDK 换地址——
            // 两代 SDK 都把 0xD401 当可重试、最多试 64 次（旧库读四款 sdk 二进制核过），连败概率 (3/4)^64 ≈ 1e-8
            if (a & (HPG - 1)) { regs[0] = 0xD401; return 1; }
#endif
            GMU_LOCK();
            // 区域照 Ryujinx CanContain(SharedMemory)：整个堆区 / 别名区都不许放（回 InvalidMemRange 0xDC01，SDK 同 0xD401 一样换地址重试，
            //   sdk SharedMemoryImplByHorizon::Map 把两者都转成可重试码）。原先只挡「当前已用的堆」：不用 SetHeapSize 的款共享内存落进堆区，
            //   远端影子从堆区顶往下切到同一宿主页后被还页盖成不可访问（11-03 火炬之光3 读时间共享内存 SIGBUS）
            if ((a < L.heap_base + L.heap_size && L.heap_base < a + span) || (a < L.alias_base + L.alias_size && L.alias_base < a + span)) { GMU_UNLOCK(); regs[0] = 0xDC01; return 1; }
            int bad = map_find(a, span) || span_busy(a, a + span) || a < L.win_base || a + span > L.win_base + L.win_size;
            if (bad) { GMU_UNLOCK(); regs[0] = 0xD401; return 1; }
            // 映射表 / 跨度表满：在改页之前回 OutOfResource（Ryujinx MapPages 内存块表分配不出同码），游戏收到「这次映射失败」，不 die
            if (g_nmap >= MAX_MAP || g_nspan >= MAX_MAP) { GMU_UNLOCK(); LOG("【红】MapSharedMemory：映射表满（%d 项），回 OutOfResource", g_nmap); regs[0] = 0xCE01; return 1; }
#ifdef __ANDROID__
            int p = ((regs[3] & 1) ? PROT_READ : 0) | ((regs[3] & 2) ? PROT_WRITE : 0);
            if (backing_begin_locked(a, span, (uint64_t)buf)) { GMU_UNLOCK(); regs[0] = 0xD401; return 1; }
            int s2a_det_shmem_map(uint64_t ga, const void *host, uint64_t n, uint64_t span, int prot);
            uint64_t error = 0;
            if (!s2a_det_shmem_map(a, buf, n, span, p)) error = alias_map(a, (uint64_t)buf, span, p);
            if (error) { backing_end_locked(a, span, 0, 0); GMU_UNLOCK(); regs[0] = error; return 1; }
#else
            vm_prot_t p = ((regs[3] & 1) ? VM_PROT_READ : 0) | ((regs[3] & 2) ? VM_PROT_WRITE : 0);
            if (backing_begin_locked(a, span, (uint64_t)buf)) { GMU_UNLOCK(); regs[0] = 0xD401; return 1; }
            int s2a_det_shmem_map(uint64_t ga, const void *host, uint64_t n, uint64_t span, int prot);
            if (!s2a_det_shmem_map(a, buf, n, span, p)) {   // 确定性记录 / 重放档：游戏看影子副本（det.c）
            vm_address_t dst = (vm_address_t)a; vm_prot_t cur, max;
            kern_return_t kr = vm_remap(mach_task_self(), &dst, (vm_size_t)span, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)buf, FALSE, &cur, &max, VM_INHERIT_SHARE);
            if (kr != KERN_SUCCESS || dst != (vm_address_t)a) die("MapSharedMemory：vm_remap 失败 kr=%d", kr);
            // 游戏侧按请求的权限映射（w3：1 读 / 2 写）：hid 等只读共享内存游戏误写当场在那条指令上报错，不静默写坏宿主正写的缓冲
            if (p != (VM_PROT_READ | VM_PROT_WRITE) && vm_protect(mach_task_self(), dst, (vm_size_t)span, FALSE, p) != KERN_SUCCESS) die("MapSharedMemory：vm_protect 权限 %#llx 失败", regs[3]);
            }
#endif
            map_add(a, n, 0, ST_SHARED, (uint32_t)regs[0]); g_map[g_nmap - 1].perm = (uint32_t)regs[3] & 3;   // 只读映射回只读（Ryujinx 按映射权限）
            g_span[g_nspan].lo = a; g_span[g_nspan++].hi = a + span;
            backing_end_locked(a, span, 1, 1);
            GMU_UNLOCK();
        } else {
            GMU_LOCK();
            int k = map_find(a, n);
            if (!k || g_map[k - 1].base != a || g_map[k - 1].h != (uint32_t)regs[0]) { GMU_UNLOCK(); regs[0] = 0xD401; return 1; }
            if (backing_begin_locked(a, span, 0)) { GMU_UNLOCK(); regs[0] = 0xD401; return 1; }
            uint64_t error = alias_unmap(a, span, (uint64_t)buf, (int)g_map[k - 1].perm);
            if (error) { backing_end_locked(a, span, 1, 0); GMU_UNLOCK(); regs[0] = error; return 1; }
            map_del(k - 1);
            for (int i = 0; i < g_nspan; i++) if (g_span[i].lo == a) { g_span[i] = g_span[--g_nspan]; break; }
            backing_end_locked(a, span, 0, 1);
            GMU_UNLOCK();
        }
#ifdef __ANDROID__
        prot_changed(a, span);
#else
        backing_changed(a, span);   // 零拷贝缓冲作废重建（只登记）；记账重算已在换页事务 end 里做
#endif
        regs[0] = 0; return 1;
    }
    case 0x2C: case 0x2D: {  // Map / UnmapPhysicalMemory(x0 地址, x1 大小)：只许在别名区内、4 KB 对齐；宿主按 16 KB 页取整，子页掩码记账
        uint64_t a = regs[0], size = regs[1];
        if (!L.sys_resource_size) { regs[0] = 0xD401; return 1; }   // 没有 SystemResourceSize 不能用（Ryujinx：PersonalMmHeap 为 0 即拒）
        if ((a | size) & 0xfff || !size) { regs[0] = 0xCC01; return 1; }   // InvalidAddress
        if (a + size < a || a < L.alias_base || a + size > L.alias_base + L.alias_size) { regs[0] = 0xDC01; return 1; }   // InvalidMemoryRegion
        if (num == 0x2C && g_alias_report && g_alias_report < L.alias_size && a + size > L.alias_base + g_alias_report) alias_report_overrun(a, size);   // #1441
        uint64_t lo = a & ~(HPG - 1), hi = (a + size + HPG - 1) & ~(HPG - 1);
#ifdef __ANDROID__
        int mapping = num == 0x2C;
#endif
#ifndef __ANDROID__
        uint64_t rel_lo = UINT64_MAX, rel_hi = 0;
        uint64_t we = L.win_base + L.win_size, wl = hi < we ? hi : lo > we ? lo : we;
        if (num == 0x2C && hi > wl && !s2a_far_has(wl, hi - wl)) { regs[0] = va_short(num, a, size, "别名区"); return 1; }
        struct timespec t_unmap_ts; uint64_t t_unmap0 = 0;
        if (num == 0x2D && hi - lo >= (1u << 20) && wl > lo && !clock_gettime(CLOCK_MONOTONIC, &t_unmap_ts)) t_unmap0 = (uint64_t)t_unmap_ts.tv_sec * 1000000000ull + (uint64_t)t_unmap_ts.tv_nsec;
#endif
        GMU_LOCK();
        if (num == 0x2C) {
            if (phys_limit_on()) {   // 先数这次真正新增的子页（已映射的不重复记账，同 Ryujinx「remainingSize = size − mappedSize」），超池即拒、什么都不改
                uint64_t add = 0;
                for (uint64_t p = a; p < a + size; p += 0x1000) if (!(*amask_at(p) & (uint8_t)(1u << ((p & (HPG - 1)) >> 12)))) add += 0x1000;
                uint64_t av = 0;
                if (!pool_fits(g_heap_cur + pool_fixed() + add, &av)) {
                    static int dumped; if (!dumped++) pm_overrun_dump(regs[30]);
                    static int said; if (!said++) LOG("物理映射到上限：堆 %llu MB + 代码 %llu MB + 主线程栈 %llu KB + SystemResourceSize %llu MB + 已物理映射 %llu MB，本次再要 %llu KB > 池 %llu MB（最多扩到模拟的池 + 1/8；宿主余量 %llu MB、留 %llu MB），回 ResLimitExceeded（只打第一次）",
                        (unsigned long long)(g_heap_cur >> 20), (unsigned long long)(code_bytes() >> 20), (unsigned long long)(g_mstack >> 10), (unsigned long long)(L.sys_resource_size >> 20), (unsigned long long)(g_phys >> 20), (unsigned long long)(add >> 10), (unsigned long long)(pool_eff() >> 20),
                        (unsigned long long)(av >> 20), (unsigned long long)(POOL_HOST_RESERVE >> 20));
                    GMU_UNLOCK(); regs[0] = 0x10801; return 1;
                }
            }
        }
        pm_note(num, a, size, regs[30]);   // 只记真要做的（超池拒掉的不记，它在超池现场里单独打）
        // 解除的段里有借出中的源（传输内存 / 代码内存 / MapMemory 源）→ InvalidMemState，什么都不改（同 Ryujinx UnmapPhysicalMemory：堆块属性须为 None）
        if (num == 0x2D && attr_any(a, a + size)) {
            GMU_UNLOCK();
            LOG("【红】UnmapPhysicalMemory %#llx+%#llx 段里有借出中的源（传输内存 / 代码内存 / MapMemory），回 InvalidMemState（Ryujinx 同）", (unsigned long long)a, (unsigned long long)size);   // dbcb99865 新返回路径：真局读数期望 0 次
            regs[0] = 0xD401; return 1;
        }
#ifdef __ANDROID__
        // 预检后可能有另一条改表先占了这个宿主页/邻居。串行仍在本线程手里，映射尚未动：
        // 撤销只串行 epoch，按原来的全体停顿路径重做；升级失败终止，绝不继续改表。
        if (mapping && g_phys_fast && !physical_growth_empty_locked(a, size)) {
            GMU_UNLOCK();
            int error = s2a_android_guest_writers_quiesce();
            if (error) {
                if (error == ECANCELED && s2a_svc_stopping()) s2a_android_guest_park_forever();
                // 升级到期时映射一处都没改（quiesce 失败已撤销 serial/epoch），这是可重试的宿主协调
                // 问题，不是客体地址错误：回 2 让 svc_mem 重新准入再做一轮（第 2 轮起直接走全体停稳），
                // 超过上限才回 InvalidMemState。以前这里 die()，一次 2 s 到期就把整局 abort 掉。
                if (g_phys_retry < S2A_PHYS_RETRY_MAX) return 2;
                LOG("【红】MapPhysicalMemory 停世界升级 %d 轮都到期 errno=%d，回 InvalidMemState（映射未修改，不 abort）",
                    S2A_PHYS_RETRY_MAX + 1, error);
                regs[0] = 0xD401; return 1;
            }
            GMU_LOCK();
            g_phys_fast = 0;
        }
#endif
#ifndef __ANDROID__
        // 换页事务只包窗口内那段 [lo, wl)：远端页不进写跟踪（影子页由 far.c 管）
        if (wl > lo && backing_begin_locked(lo, wl - lo, 0)) { GMU_UNLOCK(); regs[0] = 0xD401; return 1; }
        if (num == 0x2C) {
            // 先给窗口外要影子页、再放开窗口内（审计 #806 第 2 条：原先先 mprotect 窗口内，远端给页失败直接返回，窗口内留下可读写、子页掩码却是未映射）
            if (hi > wl) {   // 远端一次请求整段给页（影子按请求连续，far.c）；已映射子页的影子页本就可读写，再放一次不改内容
                uint64_t f0 = wl; while (f0 < hi && *amask_at(f0)) f0 += HPG;
                uint64_t f1 = hi; while (f1 > f0 && *amask_at(f1 - HPG)) f1 -= HPG;
                if (f1 > f0 && s2a_far_map(f0, f1 - f0)) { if (wl > lo) backing_end_locked(lo, wl - lo, 0, 0); GMU_UNLOCK(); regs[0] = va_short(num, a, size, "别名区（影子区与堆撞上）"); return 1; }
            }
            if (wl > lo) GUEST_COMMIT(lo, wl - lo);   // 只窗口内那段；远端影子由 far.c 管
            if (wl > lo && mprotect((void *)lo, wl - lo, PROT_READ | PROT_WRITE)) die("MapPhysicalMemory mprotect errno=%d", errno);
            if (wl > lo) punch_zero_locked(lo, wl);   // #258：整宿主页原先全空、释放后待清零的，内容是旧的，先清零（下面按「原先全空 = 零」不再清）
            uint64_t run = 0;   // 新映射的连续子页起点：权限账按段清（已映射的子页保留游戏改过的权限）
            uint8_t was = 0;   // 本次调用之前这个宿主页的子页掩码：循环里会给 *m 置位，判「原先全空」必须看进来时的值
            for (uint64_t p = a; p < a + size; p += 0x1000) {   // 新映射的子页清零（真机新页是零页）；整宿主页原先全空的是解除时换上的新占位（pages_release），mprotect 放出来本来就是零
                uint8_t *m = amask_at(p), bit = (uint8_t)(1u << ((p & (HPG - 1)) >> 12));
                // 11-03 火纹：原先判 *m，同一宿主页前面的子页刚置过位，第 2–4 个子页都 memset，映射多少提交多少（起局一次映射约 9 GB → 足迹尖峰 9.3 GB，iPhone 6 GB 上限被杀）
                if (p == a || !(p & (HPG - 1))) was = *m;
                if (!(*m & bit)) { if (was) zero_sub(p, we); *m |= bit; g_phys += 0x1000; if (g_phys > g_mem_peak) mem_peak_note(); if (!run) run = p; }
                else if (run) { perm_set(run, p, -1); run = 0; }
            }
            if (run) perm_set(run, a + size, -1);
            if (a + size - L.alias_base > g_pk_alias_end) g_pk_alias_end = a + size - L.alias_base;
            alias_in_note(a, size);
        } else {
            for (uint64_t p = a; p < a + size; p += 0x1000) {
                uint8_t *m = amask_at(p), bit = (uint8_t)(1u << ((p & (HPG - 1)) >> 12));
                if (!(*m & bit)) continue;
                *m &= (uint8_t)~bit; g_phys -= 0x1000;
                // 宿主页还有别的子页：不清零——再映射时上面的映射路径先放开整页再清新子页；解除时清会撞上游戏设的只读页
                //   （10-31 Minecraft：SetMemoryPermission 只读后合法解除，memset SIGBUS；真机解除只查状态与属性、不查权限）
                if (!*m) { uint64_t hp = p & ~(HPG - 1); if (hp >= we) { s2a_far_release(hp, HPG); continue; } pages_release(hp, HPG); if (hp < rel_lo) rel_lo = hp; if (hp + HPG > rel_hi) rel_hi = hp + HPG; }
            }
            perm_set(a, a + size, -1);
        }
        if (wl > lo) backing_end_locked(lo, wl - lo, num == 0x2C, 1);
#endif
#ifdef __ANDROID__
        // 先校验所有将改变的连续段，未覆盖完整实际宿主页的请求零修改拒绝。
        // 原HPG位图仍记录4K guest子页；4K宿主不再把邻居一并改保护/清零。
        size_t page = plat_page_size();
        for (uint64_t cursor = a, end = a + size; cursor < end;) {
            if (!physical_changes(cursor, mapping)) { cursor += 0x1000; continue; }
            uint64_t first = cursor;
            do { cursor += 0x1000; } while (cursor < end && physical_changes(cursor, mapping));
            if (!page || first % page || (cursor - first) % page) {
                GMU_UNLOCK(); regs[0] = 0xD401; return 1;
            }
            // 复用保护（任何映射之前）：这段页仍被 NVN 长寿别名引用时不能拿到新页。
            if (mapping && backing_refs_checked(first, cursor - first, "物理映射复用")) {
                GMU_UNLOCK();
                backing_reuse_denied("MapPhysicalMemory 复用", first, cursor - first, &g_backing_reuse_denied);
                regs[0] = 0xD401; return 1;
            }
        }
        int committed = 0;
        for (uint64_t cursor = a, end = a + size; cursor < end;) {
            if (!physical_changes(cursor, mapping)) { cursor += 0x1000; continue; }
            uint64_t first = cursor;
            do { cursor += 0x1000; } while (cursor < end && physical_changes(cursor, mapping));
            uint64_t length = cursor - first;
            if (backing_begin_locked(first, length, 0)) {
                if (committed) die("Android物理页部分提交后无法启动下一事务，拒绝继续");
                GMU_UNLOCK(); regs[0] = 0xD401; return 1;
            }
            if (mapping) {
                int pe = backing_prepare_locked(first, length);
                if (pe == EAGAIN) {   // 放锁窗口里 NVN ref 进来：回滚本段，回 0xD401
                    backing_end_locked(first, length, 0, 0);
                    if (committed) die("Android物理页部分提交后遇到放锁窗口 ref，拒绝继续");
                    GMU_UNLOCK();
                    backing_reuse_denied("MapPhysicalMemory 复用（放锁窗口）", first, length, &g_backing_reuse_denied);
                    regs[0] = 0xD401; return 1;
                }
                if (pe) die("MapPhysicalMemory 提交前打洞失败 errno=%d（%#llx+%#llx）", pe, (unsigned long long)first, (unsigned long long)length);
            }
            replace_pages_locked(first, length, mapping);
            for (uint64_t address = first; address < cursor; address += 0x1000) {
                uint8_t *mask = amask_at(address), bit = (uint8_t)(1u << ((address & (HPG - 1)) >> 12));
                if (mapping) { *mask |= bit; g_phys += 0x1000; }
                else { *mask &= (uint8_t)~bit; g_phys -= 0x1000; }
            }
            perm_set(first, cursor, -1);
            mem_peak_note();
            // 解除的页已死（掩码已清）：仍在本段事务里打洞还内存，有别名引用的页由 refs 挡住。
            if (!mapping) backing_release_locked(first, length, BACKING_PUNCH_RELEASE);
            backing_end_locked(first, length, mapping, 1);
            committed = 1;
        }
        (void)lo; (void)hi;
        if (mapping && a + size - L.alias_base > g_pk_alias_end) g_pk_alias_end = a + size - L.alias_base;
        if (mapping) alias_in_note(a, size);
#endif
#ifndef __ANDROID__
        if (t_unmap0 && !clock_gettime(CLOCK_MONOTONIC, &t_unmap_ts)) { uint64_t dt = (uint64_t)t_unmap_ts.tv_sec * 1000000000ull + (uint64_t)t_unmap_ts.tv_nsec - t_unmap0; g_unmap_n++; g_unmap_ns += dt; if (dt > g_unmap_max_ns) g_unmap_max_ns = dt; static int said; if (dt >= 200000000 && said < 20) { said++; LOG("#258 解除物理映射 %#llx +%llu MB 持锁 %llu ms", (unsigned long long)a, (unsigned long long)(size >> 20), (unsigned long long)(dt / 1000000)); } }
#endif
        GMU_UNLOCK();
#ifndef __ANDROID__
        if (rel_hi > we) rel_hi = we;
        if (rel_hi > rel_lo) backing_changed(rel_lo, rel_hi - rel_lo);
#endif
        if (num == 0x2D) unmap_ring_note(a, size);
        regs[0] = 0; return 1;
    }
    case 0x07:
        // 堆不够后游戏自己退出（#947 HOT WHEELS：拿到 0xD001 打「Failed to Set MemoryHeap Size」后 ExitProcess，不走 Break）：同 Break，
        //   能进程内重开就重开（新局按记下的堆给），不能就按「需要更多内存、已记下、再点一次开始」收，不当成正常退出
        if (atomic_load(&g_heap_noted) && !s2a_mem_heap_learned_restart()) { LOG("游戏在堆不够后退出：本款要的堆已记下，下一局按它给"); s2a_init_failed(S2A_INIT_FAIL_E_HEAP_LEARNED); }
        session_guest_end("游戏调 ExitProcess"); s2a_host_shutdown("游戏调 ExitProcess"); for (;;) pause();
    case 0x1E: { uint64_t s2a_det_tick(uint64_t); regs[0] = s2a_det_tick(guest_tick()); return 1; }   // GetSystemTick：x0 = 计数（无 Result）
    case 0x24: {  // GetProcessId(→x1; w1 句柄)：只认本进程 / 本线程伪句柄
        uint32_t h = (uint32_t)regs[1]; uint64_t s2a_process_id(void);
        if (h != S2A_H_CUR_PROCESS && h != S2A_H_CUR_THREAD) { svc_fail_log(num, 0xE401, "句柄不是本进程 / 本线程伪句柄"); regs[0] = 0xE401; return 1; }
        regs[0] = 0; regs[1] = s2a_process_id(); return 1;
    }
    case 0x26: {  // Break(x0 原因, x1 指针, x2 大小)：SDK 致命中止
        module_t *m = module_of((void *)orig);
        // 原因最高位 = 仅通知（Horizon BreakReason NotificationOnly；例 0x80000003 = ro 装动态模块前的通知），不是错误：照 Ryujinx 继续，
        //   只记普通行、不打红、不回溯（10-30 普查 DISGAEA 6 / 大航海Ⅳ / 忍者龟：打成【红】Break 后被当成「第一个失败点」，真正的失败点被盖住）
        if (regs[0] & 0x80000000ull) {
            static _Atomic unsigned said;
            if (atomic_fetch_add(&said, 1) < 8) LOG("Break 通知（仅通知、不是错误，继续）原因 %#llx 位于 %s+%#llx", (unsigned long long)regs[0], m ? m->name : "?", (unsigned long long)(m ? orig - (uint64_t)m->base : orig));
            regs[0] = 0; return 1;
        }
        { char at[256]; if (!s2a_guest_where(orig, at, sizeof at)) snprintf(at, sizeof at, "?+%#llx", (unsigned long long)orig);   // 带导出名（#806）
          LOG("【红】Break 原因 %#llx 载荷 %#llx+%#llx 位于 %s", (unsigned long long)regs[0], (unsigned long long)regs[1], (unsigned long long)regs[2], at); }
        // SDK 的「结果不成功就中止」（原因 0、载荷 4 字节）载荷就是那个失败的 Result：读出来按 2xxx-yyyy 打，一眼看出是哪个服务回了什么
        //   （10-29 普查 Servonauts / Fit Boxing 3 同在 sdk+0x162868 中止，日志只有载荷地址、没有值，定不了是哪个服务）。载荷在游戏栈上，只读窗口内对齐的
        if (regs[2] == 4 && !(regs[1] & 3) && win_has(regs[1], 4)) {
            uint32_t r = *(const volatile uint32_t *)(uintptr_t)regs[1];
            LOG("【红】Break 载荷结果码 %#x（%04u-%04u：模块 %u、描述 %u）", r, 2000 + (r & 0x1FF), (r >> 9) & 0x1FFF, r & 0x1FF, (r >> 9) & 0x1FFF);
        }
        s2a_backtrace(regs[29]);
        // 这一局普通堆超出堆区、已记下要的堆（D-NGH9）：游戏拿到 0xD001 自己中止。告诉玩家「需要更多内存、已记下、再点一次开始」，不按崩溃说
        //   （iOS 人手局停提示页，Mac / 自动化局失败退出，Mac 启动器按同一句话报，pack/jobs/build.py）。没记下（写不进）照旧走下面
        if (atomic_load(&g_heap_noted) && !s2a_mem_heap_learned_restart()) { LOG("游戏在堆不够后中止：本款要的堆已记下，下一局按它给"); s2a_init_failed(S2A_INIT_FAIL_E_HEAP_LEARNED); }
        session_guest_end("游戏 Break");
#if TARGET_OS_IPHONE
        // 资源包缺 / 不全导致的 Break（10-26 测试设备2：缺 ctl/control.nacp，App 退出像闪退）：不退出，停住游戏、弹缺资源包说明页（pack_apply.c）
        { int s2a_pack_help_on_break(void); void s2a_guest_freeze(int); if (s2a_pack_help_on_break()) { s2a_guest_freeze(1); for (;;) pause(); } }
#endif
        s2a_host_shutdown("游戏 Break");
        for (;;) pause();
    }
    case 0x27: { static int dbg = -1; if (dbg < 0) { const char *e = getenv("S2A_GUEST_LOG"); dbg = e && *e == '1'; }   // OutputDebugString(x0 串, x1 长)
        if (dbg) LOG("[游戏] %.*s", (int)regs[1], (const char *)regs[0]);
        regs[0] = 0; return 1; }
    case 0x30: case 0x31: case 0x37: {   // GetResourceLimitLimit / Current / PeakValue(→x1; w1 句柄, w2 资源)
        int64_t v = 0; int e = rl_value((uint32_t)regs[1], (uint32_t)regs[2], num == 0x30 ? 0 : num == 0x31 ? 1 : 2, &v);
        regs[0] = (uint64_t)e; regs[1] = e ? 0 : (uint64_t)v; return 1; }
    case 0x7d: {   // CreateResourceLimit(→w1 句柄)：新对象，上限 / 当前 / 峰值全 0，不挂到本进程
        reslimit_t *r = calloc(1, sizeof *r); uint32_t h = s2a_native_handle_new(1, r);
        if (!h) { free(r); regs[0] = 0xD201; regs[1] = 0; return 1; }
        regs[0] = 0; regs[1] = h; return 1; }
    case 0x7e: {   // SetResourceLimitLimitValue(w0 句柄, w1 资源, x2 值)：当前值 > 新上限回 InvalidState，成功时峰值重置为当前值（Ryujinx KResourceLimit.SetLimitValue）
        uint32_t which = (uint32_t)regs[1]; int64_t lim = (int64_t)regs[2];
        if (which >= 5) { regs[0] = 0xF001; return 1; }
        reslimit_t *r = s2a_native_handle_obj((uint32_t)regs[0], 1); if (!r) { regs[0] = 0xE401; return 1; }
        int64_t cur = 0; if (r->proc) rl_value((uint32_t)regs[0], which, 1, &cur); else cur = r->cur[which];
        if (cur > lim) { regs[0] = 0xFA01; return 1; }
        r->lim[which] = lim; if (!r->proc) r->peak[which] = r->cur[which];
        regs[0] = 0; return 1; }
    case 0x4b: {   // CreateCodeMemory(→w1 句柄; x1 地址, x2 大小)：校验照 Ryujinx（对齐、大小、回绕、地址空间）
        uint64_t a = regs[1], n = regs[2], e = chk_ars(a, n); if (!e && !in_as(a, n)) e = 0xD401;
        if (e) { regs[0] = e; regs[1] = 0; return 1; }
        // 源借出照 Ryujinx KCodeMemory.Initialize → BorrowCodeMemory（已映射、读写、属性 None，否则 InvalidMemState；置借出、权限不变），关句柄还回（s2a_mem_codemem_free）
        codemem_t *c = calloc(1, sizeof *c); if (!c) { regs[0] = 0xCE01; regs[1] = 0; return 1; }
        c->addr = a; c->size = n;
        GMU_LOCK(); e = borrow_locked(a, n, -1); GMU_UNLOCK();
        if (e) { free(c); regs[0] = e; regs[1] = 0; return 1; }
        uint32_t h = s2a_native_handle_new(2, c); if (!h) { GMU_LOCK(); unborrow_locked(a, n); GMU_UNLOCK(); free(c); regs[0] = 0xD201; regs[1] = 0; return 1; }
        regs[0] = 0; regs[1] = h; return 1; }
    case 0x4c: {   // ControlCodeMemory(w0 句柄, w1 操作, x2 地址, x3 大小, w4 权限)：0 Map（读写）/ 1 MapToOwner（读 / 读执行）/ 2 Unmap / 3 UnmapFromOwner（权限须 0）
        // 照 Ryujinx Syscall.ControlCodeMemory（游戏进程 AllowCodeMemoryForJit = true）。苹果上游戏运行时生成的代码不能按可执行映射（iOS 不许、也没经改写器），
        //   MapToOwner 要读执行时按只读映射并报红：执行到时显式出错，不静默
        codemem_t *c = s2a_native_handle_obj((uint32_t)regs[0], 2); uint32_t op = (uint32_t)regs[1], pm = (uint32_t)regs[4]; uint64_t a = regs[2], n = regs[3], e = 0;
        if (!c) { regs[0] = 0xE401; return 1; }
        if (op > 3) { regs[0] = 0xF001; return 1; }
        if (n != c->size) { regs[0] = 0xCA01; return 1; }
        if (op == 0 && pm != 3) e = 0xD801; else if (op == 1 && pm != 1 && pm != 5) e = 0xD801; else if (op >= 2 && pm != 0) e = 0xD801;
        if (e) { regs[0] = e; return 1; }
        GMU_LOCK();
        if (op <= 1) {
            // 代码内存的源已经由 0x4B 的借出校验保证是活页；这里再按背板区整段 ref，
            // 防止映射存活期间源页被释放后打洞清掉。ref 与 e 分开，失败撤销不掩盖主错误。
#ifdef __ANDROID__
            int refd = 0;
#endif
            int transaction = 0;
            if ((op == 0 ? c->map_rw : c->map_ro)) e = 0xFA01;   // InvalidState：已映射
            if (!e) e = map_target_ok(a, n);
#ifdef __ANDROID__
            // 16K 宿主上源只按 4K 对齐：旧 alias_map 在 mremap 前回 0xD401，这里提前保持同一语义，
            // 不能因为 ref 的 EINVAL 走到 die。
            if (!e) { size_t pg = plat_page_size(); if (!pg || ((c->addr | n) & (pg - 1))) e = 0xD401; }
            if (!e) {
                int r = backing_alias_ref_locked(c->addr, n);
                if (r < 0) die("代码内存背板引用失败 errno=%d", -r);
                refd = r;
            }
#endif
            if (!e) { transaction = !backing_begin_locked(a, n, c->addr); if (!transaction) e = 0xD401; }
            if (!e) e = alias_map(a, c->addr, n, op == 0 ? (PROT_READ | PROT_WRITE) : PROT_READ);
            if (!e) { map_add(a, n, c->addr, op == 0 ? ST_CODE_RW : ST_CODE_RO, (uint32_t)regs[0]); g_map[g_nmap - 1].perm = op == 0 ? 3 : pm;
#ifdef __ANDROID__
                g_map_flags[g_nmap - 1] = refd ? MAPF_BACKING_REF : 0;
#endif
                if (op == 0) c->map_rw = a; else c->map_ro = a; }
            if (transaction) backing_end_locked(a, n, !e, !e);
#ifdef __ANDROID__
            if (e && refd) backing_alias_unref_locked(c->addr, n);
#endif
            if (!e && op == 1 && pm == 5) { static int said; if (!said++) LOG("【红】ControlCodeMemory 映成读执行：苹果上游戏运行时生成的代码不能执行，按只读映射（执行到会出错）"); }
        } else {
            uint64_t want = op == 2 ? c->map_rw : c->map_ro; int k = map_find(a, n);
#ifdef __ANDROID__
            uint64_t src = 0; int refd = 0;
#endif
            if (!want || want != a || !k || g_map[k - 1].base != a) e = 0xD401;
            else if (backing_begin_locked(a, n, 0)) e = 0xD401;
            else {
#ifdef __ANDROID__
                src = g_map[k - 1].src; refd = g_map_flags[k - 1] == MAPF_BACKING_REF;
#endif
                e = alias_unmap(a, n, c->addr, op == 2 ? PROT_READ | PROT_WRITE : PROT_READ);
                if (!e) { map_del(k - 1); if (op == 2) c->map_rw = 0; else c->map_ro = 0; }
                backing_end_locked(a, n, !!e, !e);
            }
#ifdef __ANDROID__
            if (!e && refd) backing_alias_unref_locked(src, n);
#endif
        }
        GMU_UNLOCK();
#ifdef __ANDROID__
        if (!e) prot_changed(a, n);   // 苹果：记账重算在换页事务 end 里（backing_end_locked）
#endif
        regs[0] = e; return 1; }
    case 0x51: case 0x52: {   // Map / UnmapTransferMemory(w0 句柄, x1 地址, x2 大小[, w3 权限])：校验照 Ryujinx；大小须等于传输内存大小、映射权限须等于建立时的
        uint32_t h = (uint32_t)regs[0]; uint64_t a = regs[1], n = regs[2], e = chk_ars(a, n), ta = 0, tn = 0; uint32_t tp = 0, pm = (uint32_t)regs[3];
        if (!e && num == 0x51 && (pm > 3 || pm == 2)) e = 0xD801;
        if (!e && !s2a_tmem_get(h, &ta, &tn, &tp)) e = 0xE401;
        if (e) { regs[0] = e; return 1; }
        GMU_LOCK();
        if (num == 0x51) {
            // 传输内存的源由 0x15 借出校验保证是活页（对齐 Ryujinx 的 KTransferMemory.Destroy 后，
            // 映射期间关句柄也不还回）；0x51 的源同样做 ref，防止源页被释放后打洞清掉。
#ifdef __ANDROID__
            int refd = 0;
#endif
            int transaction = 0;
            if (((n + 0xfff) & ~0xfffull) != ((tn + 0xfff) & ~0xfffull)) e = 0xCA01;
            else if (pm != tp) e = 0xFA01;
            if (!e) e = map_target_ok(a, n);
#ifdef __ANDROID__
            if (!e) { size_t pg = plat_page_size(); if (!pg || ((ta | n) & (pg - 1))) e = 0xD401; }
            if (!e) {
                int r = backing_alias_ref_locked(ta, n);
                if (r < 0) die("传输内存背板引用失败 errno=%d", -r);
                refd = r;
            }
#endif
            if (!e) { transaction = !backing_begin_locked(a, n, ta); if (!transaction) e = 0xD401; }
            if (!e) e = alias_map(a, ta, n, PROT_READ | PROT_WRITE);   // Ryujinx 一律按读写映射（KTransferMemory.MapIntoProcess）
            if (!e) { map_add(a, n, ta, tp ? ST_TMEM : ST_TMEM_ISO, h);
#ifdef __ANDROID__
                g_map_flags[g_nmap - 1] = refd ? MAPF_BACKING_REF : 0;
#endif
            }
            if (transaction) backing_end_locked(a, n, !e, !e);
#ifdef __ANDROID__
            if (e && refd) backing_alias_unref_locked(ta, n);
#endif
        } else {
            int k = map_find(a, n);
#ifdef __ANDROID__
            uint64_t src = 0; int refd = 0;
#endif
            if (!k || g_map[k - 1].base != a || g_map[k - 1].h != h || (g_map[k - 1].state != ST_TMEM && g_map[k - 1].state != ST_TMEM_ISO)) e = 0xD401;
            else if (g_map[k - 1].size != n) e = 0xCA01;
            else if (backing_begin_locked(a, n, 0)) e = 0xD401;
            else {
#ifdef __ANDROID__
                src = g_map[k - 1].src; refd = g_map_flags[k - 1] == MAPF_BACKING_REF;
#endif
                e = alias_unmap(a, n, ta, PROT_READ | PROT_WRITE);
                if (!e) map_del(k - 1);
                backing_end_locked(a, n, !!e, !e);
            }
#ifdef __ANDROID__
            if (!e && refd) backing_alias_unref_locked(src, n);
#endif
        }
        GMU_UNLOCK();
#ifdef __ANDROID__
        if (!e) prot_changed(a, n);   // 苹果：记账重算在换页事务 end 里（backing_end_locked）
#endif
        regs[0] = e; return 1; }
    case 0x5f: regs[0] = 0; return 1;   // FlushProcessDataCache(w0 进程, x1 地址, x2 大小)：Ryujinx 直接回成功（苹果 CPU 与显卡缓存一致）
    case 0x6f: {   // GetSystemInfo(→x1; w1 类型, w2 句柄, x3 子号)：照 Ryujinx——类型 > 2 InvalidEnumValue、句柄须 0；
                   //   类型 0 / 1 = 内存区（子号 0 应用 / 1 小程序 / 2 系统 / 3 系统不安全）的总量 / 空闲，类型 2 = 特权进程号范围（子号 0 / 1）
        uint32_t id = (uint32_t)regs[1]; int64_t sub = (int64_t)regs[3]; uint64_t v = 0;
        if (id > 2) { regs[0] = 0xF001; regs[1] = 0; return 1; }
        if ((uint32_t)regs[2]) { regs[0] = 0xE401; regs[1] = 0; return 1; }
        if (id < 2) {
            if ((uint64_t)sub > 3) { regs[0] = 0xE801; regs[1] = 0; return 1; }
            // 4 GB 机型的内存区（Ryujinx KSystemControl / KernelInit）：应用 3285 MiB、小程序 507 MiB、系统服务约 251 MiB（上游注释量级，取决于内核保留区，近似）、
            //   nv 服务 (0x2238 + 0x710 + 0x80) × 4 KB。只有应用区按实际记账算空闲；其余三区游戏碰不到，空闲按总量报
            static const uint64_t total[4] = {3285ull << 20, 507ull << 20, 251ull << 20, (0x2238ull + 0x710 + 0x80) << 12};
            v = id == 0 ? total[sub] : sub == 0 ? (pool_eff() > mem_used() ? pool_eff() - mem_used() : 0) : total[sub];
        } else {
            if ((uint64_t)sub > 1) { regs[0] = 0xE801; regs[1] = 0; return 1; }
            v = sub == 0 ? 1 : 8;   // 特权进程号范围：Ryujinx KernelContext.PrivilegedProcessLowestId / HighestId
        }
        regs[0] = 0; regs[1] = v; return 1; }
    case 0x2A: case 0x2B: regs[0] = 0; return 1;   // Flush(Entire)DataCache：苹果芯片 CPU 与显卡缓存一致，无可做
    case 0x29: {  // GetInfo(→x1; w1 类型, w2 句柄, x3 子号)
        uint64_t id = regs[1], v = 0; uint32_t hd = (uint32_t)regs[2]; int64_t sub = (int64_t)regs[3];
        // 参数校验照 Ryujinx Syscall.GetInfo：进程类各项子号须 0（InvalidCombination）、句柄须是本进程（伪句柄 0xFFFF8001，InvalidHandle）；
        //   调试器 / 资源上限 / 随机熵句柄须 0；随机熵子号 0–3
        switch (id) {
        case I_CoreMask: case I_PriorityMask: case I_AliasAddr: case I_AliasSize: case I_HeapAddr: case I_HeapSize: case I_TotalMem: case I_UsedMem:
        case I_AslrAddr: case I_AslrSize: case I_StackAddr: case I_StackSize: case I_SysResTotal: case I_SysResUsed: case I_ProgramId: case I_UserExcAddr:
        case I_TotalNonSys: case I_UsedNonSys: case I_IsApplication: case I_FreeThreadCount: case I_AliasExtraSize:
        case I_RemoteAddr: case I_RemoteSize: case I_RemoteUsage: case I_RemoteUsagePeak: case I_PageSize:
            if (sub != 0) { regs[0] = 0xE801; regs[1] = 0; return 1; }
            if (hd != 0xFFFF8001u) { regs[0] = 0xE401; regs[1] = 0; return 1; }
            break;
        case I_DebuggerAttached: case I_ResourceLimit:
            if (hd) { regs[0] = 0xE401; regs[1] = 0; return 1; }
            if (sub != 0) { regs[0] = 0xE801; regs[1] = 0; return 1; }
            break;
        case I_RandomEntropy:
            if (hd) { regs[0] = 0xE401; regs[1] = 0; return 1; }
            if ((uint64_t)sub > 3) { regs[0] = 0xE801; regs[1] = 0; return 1; }
            break;
        case I_IsSvcPermitted:   // 照 Ryujinx：句柄须 0；子号只许问 0x36 SynchronizePreemptionState
            if (hd) { regs[0] = 0xE401; regs[1] = 0; return 1; }
            if (sub != 0x36) { regs[0] = 0xE801; regs[1] = 0; return 1; }
            break;
        default: break;
        }
        switch (id) {
        case I_CoreMask: v = g_core_mask; break;
        case I_PriorityMask: v = g_prio_mask; break;
        case I_AliasAddr: v = L.alias_base; break;  case I_AliasSize: v = g_alias_report ? g_alias_report : L.alias_size; break;
        case I_HeapAddr: v = L.heap_base; break;    case I_HeapSize: v = L.heap_size; break;
        case I_AslrAddr: v = L.aslr_base; break;    case I_AslrSize: v = L.aslr_size; break;
        case I_StackAddr: v = L.stack_base; break;  case I_StackSize: v = L.stack_size; break;
        // 总量 / 已用量照 Ryujinx GetMemoryCapacity / GetMemoryUsage（NonSys 变体减 SystemResourceSize），与 SetHeapSize / 物理映射的核对同口径；
        // 游戏按「总量 − 已用」设堆。S2A_PHYS_LIMIT=0 应急关回旧口径（已用 = 堆 + 映像 ≥ 64 MB）
        case I_TotalMem: v = pool_eff(); break;
        case I_TotalNonSys: v = pool_eff() - (phys_limit_on() ? L.sys_resource_size : 0); break;
        case I_UsedMem: case I_UsedNonSys:
            if (phys_limit_on()) { v = g_heap_cur + pool_fixed() - (id == I_UsedNonSys ? L.sys_resource_size : 0); break; }
            { size_t ib = image_bytes(); v = g_heap_cur + (ib > 0x4000000 ? ib : 0x4000000); break; }
        case I_SysResTotal: v = L.sys_resource_size; break;
        // 系统资源装的是本进程的页表（Ryujinx 按内存块个数记）。宿主没有这份页表，按已映射字节（堆 + 代码 + 主线程栈 + 物理映射，同 UsedNonSys 口径）
        //   估末级页表：每 2 MB 一张 4 KB 表（稠密下界），不超过总量；没有系统资源回 0（Ryujinx：PersonalMmHeapPagesCount 为 0 回 0）。原先恒 0
        case I_SysResUsed: if (L.sys_resource_size) { uint64_t pt = ((g_heap_cur + code_bytes() + g_mstack + g_phys + (2ull << 20) - 1) >> 21) << 12; v = pt < L.sys_resource_size ? pt : L.sys_resource_size; } break;
        case I_DebuggerAttached: v = 0; break;
        case I_ResourceLimit: {   // 本进程的资源上限对象：每次发一个新句柄（同 Ryujinx GenerateHandle）；句柄表满回 HandleTableFull
            uint32_t h = s2a_native_handle_new(1, &g_rl); if (!h) { regs[0] = 0xD201; regs[1] = 0; return 1; } v = h; break; }
        case I_RandomEntropy: {   // 每进程固定 4 个值（Ryujinx KProcess.RandomEntropy[4]，建进程时取一次）；原先每次调用都不同
            if (!g_entropy_ready) { for (int i = 0; i < 4; i++) g_entropy[i] = s2a_entropy64(); g_entropy_ready = 1; }
            v = g_entropy[sub]; break; }
        case I_ProgramId: v = g_program_id; break;
        case I_IsApplication: v = 1; break;
        case I_UserExcAddr: {   // 进程的用户异常上下文：同 Horizon / Ryujinx 在 TLS 区给进程分一个 0x200 槽（KProcess.UserExceptionContextAddress）。
                                // 原先回 0 且报成功：塞尔达首帧前往 0+0x1C0 写，崩在 sdk+0x1437cc（10-20 M3 摸底）
            if (!g_user_exception_context) g_user_exception_context = (uint64_t)s2a_tlr_alloc();
            v = g_user_exception_context; break; }
        case I_FreeThreadCount: { int n = 608 - s2a_thread_alive_count(); v = n > 0 ? (uint64_t)n : 0; break; }   // Ryujinx：应用线程上限 608（ProcessLoaderHelper）− 已占，同 thread.c LIM_THREADS
        case I_AliasExtraSize: v = 0; break;
        // Switch 2 远端区（Pokopia nnosInitialize 起局就问 29，回错就 Break）：SDK 拿 [29, 29+30) 建 RemoteRegionManager、31 / 32 报用量；唯一用户是
        //   GPU 驱动起局（nv::InitializeGraphics → nn::gs BootWithRemoteMemory）。宿主不给远端区：全回 0（同 the reference port hle_svc.c，MIT）
        case I_RemoteAddr: case I_RemoteSize: case I_RemoteUsage: case I_RemoteUsagePeak: v = 0; break;
        // 内核页大小：nn::ro 拿它对齐 RELRO（值 − 1 当掩码，0 会 abort）。Horizon 页 4 KB，各模块段按它对齐
        case I_PageSize: v = 0x1000; break;
        case I_IdleTick: {   // 句柄须 0；子号 -1 或当前核。宿主取对应 CPU 累计空闲（1/100 秒）换系统计数器（19.2 / 31.25 MHz）
            int core = s2a_thread_cur_core(); int64_t sub = (int64_t)regs[3];
            if ((uint32_t)regs[2]) { regs[0] = 0xE401; regs[1] = 0; return 1; }
            if (sub != -1 && sub != core) { regs[0] = 0xE801; regs[1] = 0; return 1; }
#ifdef __ANDROID__
            svc_fail_log(num, 0xF001, "GetInfo IdleTick 的 Android 逐核计时未接通");
            regs[0] = 0xF001; regs[1] = 0; return 1;
#else
            natural_t nc = 0; processor_info_array_t info; mach_msg_type_number_t cnt; uint64_t idle = 0;
            if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &nc, &info, &cnt) == KERN_SUCCESS) {
                if (core < (int)nc) idle = ((processor_cpu_load_info_t)info)[core].cpu_ticks[CPU_STATE_IDLE];
                vm_deallocate(mach_task_self(), (vm_address_t)info, cnt * sizeof(integer_t)); }
            v = idle * (s2a_guest_tick_hz() / 100); break;
#endif
        }
        case I_IsSvcPermitted: v = g_svc_perm[0x36 >> 6] >> (0x36 & 63) & 1; break;   // NPDM 许可位
        case I_ThreadTickCount: case I_ThreadTickCountOld: {   // 子号 -1 = 线程累计运行时间；其他核 → 0；子号 = 当前核：真机回「距上次切换」，
            //   宿主不记切换时刻（虚拟核没开时根本没有切换点），回累计——游戏拿它算本段耗时会偏大，不会为 0 或倒退
            int64_t sub = (int64_t)regs[3]; int ok = 0;
            if (sub < -1 || sub >= s2a_thread_hw_cores()) { regs[0] = 0xE801; regs[1] = 0; return 1; }
            uint64_t ns = s2a_thread_cpu_ns((uint32_t)regs[2], &ok);
            if (!ok) { regs[0] = 0xE401; regs[1] = 0; return 1; }
            v = (sub != -1 && sub != s2a_thread_cur_core()) ? 0 : s2a_ns_to_tick(ns); break; }
        default: { char b[64]; snprintf(b, sizeof b, "GetInfo 信息类型 %#llx 未做", (unsigned long long)id); svc_fail_log(num, 0xF001, b); }
            regs[0] = 0xF001; regs[1] = 0; return 1;   // InvalidEnumValue
        }
        {   // 每种类型第一次被问时记一行回了什么（诊断：一款 Switch 2 游戏把堆开到池满后第 7 帧分配拿到空，要对照它看到的内存数；GetInfo 每局百余次，只记首次）
            static _Atomic uint64_t seen; if (id < 64 && !(atomic_fetch_or(&seen, 1ull << id) & (1ull << id)))
                LOG("GetInfo 首次：类型 %llu 子号 %lld → %#llx（%llu MB）", (unsigned long long)id, (long long)sub, (unsigned long long)v, (unsigned long long)(v >> 20)); }
        regs[0] = 0; regs[1] = v; return 1;
    }
    }
    return 0;
}

// 确定性诊断（det.c 分块哈希）：游戏可写区清单——堆已提交部分、MapMemory / 栈映射；回个数
int s2a_det_mem_regions(uint64_t *b, uint64_t *n, int max) {
    int k = 0; GMU_LOCK();
    if (g_heap_cur && k < max) { b[k] = (uint64_t)g_heap; n[k++] = g_heap_cur; }
    for (int i = 0; i < g_nmap && k < max; i++) if (g_map[i].state == ST_STACK || g_map[i].state == ST_HEAP) { b[k] = g_map[i].base; n[k++] = g_map[i].size; }
    GMU_UNLOCK();
    return k;
}
// 确定性档：游戏线程的 TLS 块 / 线程对象从 TLS/IO 区顶端往下按序分配（宿主 calloc 的地址每局随机，游戏把它们存进自己的数据结构、
// 按指针排序 / 哈希就会走岔——10-20 重放检查点快照实测：堆里 1728 字节差异全是这些指针）。线程创建串行 ⇒ 地址每局相同；不回收
void *s2a_det_arena_alloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    GMU_LOCK();
    if (!g_det_arena_top) g_det_arena_top = L.tlsio_base + L.tlsio_size;
    uint64_t lo = g_det_arena_top - n;
    if (lo < g_tlr_next + HPG) die("确定性：TLS/IO 区放不下 TLS 块");
    uint64_t pg = lo & ~(HPG - 1);
    GUEST_COMMIT(pg, g_det_arena_top - pg);
    if (mprotect((void *)pg, g_det_arena_top - pg, PROT_READ | PROT_WRITE)) die("确定性：TLS 块页 mprotect errno=%d", errno);
    g_det_arena_top = lo; GMU_UNLOCK();
    memset((void *)lo, 0, n); return (void *)lo;
}
// TLR 区（每线程 0x200：IPC 消息缓冲在 +0；+0x1F8 是宿主分配的 TLS 块指针，每局不同，哈希时跳过）
void s2a_det_tlr_region(uint64_t *b, uint64_t *n) { GMU_LOCK(); *b = L.tlsio_base; *n = g_tlr_next > L.tlsio_base ? g_tlr_next - L.tlsio_base : 0; GMU_UNLOCK(); }

// 全部读写者退出后调用；窗口所有者整体替换客体页，保留地址归属。
void s2a_mem_session_reset(void) {
#ifdef __ANDROID__
    // 0x4C/0x51 的映射在会话复位时可能还活着：先按并行标志把它们的背板 ref 还掉（映射本身随地址空间一起消失）。
    // NVN vpool 的记录不在这里，必须先于本函数拆完；剩余 refs 让 plat_backing_reset 回 EBUSY 并 die。
    GMU_LOCK();
    for (int i = 0; i < g_nmap; i++) {
        if (g_map_flags[i] != MAPF_BACKING_REF) continue;
        int error = plat_backing_unref(g_map[i].src, (size_t)g_map[i].size);
        g_map_flags[i] = 0;
        if (error) { GMU_UNLOCK(); die("会话复位时 0x4C/0x51 背板引用下溢 errno=%d", error); }
    }
    GMU_UNLOCK();
    { int be = plat_backing_reset(); if (be) die("会话复位时背板引用未清空 errno=%d（NVN vpool 拆除顺序违约 / 0x4C-0x51 ref 未清）", be); }
#endif
    GMU_LOCK();
#ifndef __ANDROID__
    punch_reset_locked();
#endif
    free(g_amask); g_amask = NULL;
    L = (s2a_layout){0}; g_npdm_core = g_npdm_prio = 0;
    g_program_id = g_core_mask = g_prio_mask = 0;
    memset(g_svc_perm, 0, sizeof g_svc_perm);
    g_heap = NULL; g_heap_cur = 0; g_phys = g_mstack = 0; g_pool_grown = 0;
    g_hooks = (s2a_track_hooks){0};
    memset(g_map, 0, sizeof g_map); g_nmap = 0;
    free(g_perm); g_perm = NULL; g_nperm = 0; free(g_attr); g_attr = NULL; g_nattr = 0;
    memset(g_span, 0, sizeof g_span); g_nspan = 0;
    g_cur_st = g_cur_sh = g_pk_st = g_pk_sh = 0;
    g_pk_st_end = g_pk_sh_end = g_pk_heap = g_pk_heap_request = g_pk_alias_end = g_pk_alias_in = 0;
    g_tlr_next = 0; g_tlr_free = NULL;
    g_user_exception_context = g_det_arena_top = 0;
    memset(g_entropy, 0, sizeof g_entropy); g_entropy_ready = 0;
    g_lay_ok = 0; g_mod_base = g_shadow_base = g_lay_need = 0;
    atomic_store(&g_heap_noted, 0); atomic_store(&g_heap_noted_size, 0); g_lay_heap_floor = 0;   // 进程内重开：上一局的「堆不够已记下」不带进新局
    atomic_store(&g_va_short, 0); atomic_flag_clear(&g_map_failure_seen);
    g_rl = (reslimit_t){1, 1, {0, 608, 700, 128, 894}, {0}, {0}};
    g_mem_peak = 0;
    GMU_UNLOCK();
}

// 替页与栈搬移SVC进入安全点；普通查询与时钟不承担停全局线程的成本。
int svc_mem(int num, uint64_t *regs, uint64_t orig) {
#ifdef __ANDROID__
    // 0x02 SetMemoryPermission 也改表（宿主保护 + 权限账，审计 P1-10）：要与换页串行，但不替页，只取串行 epoch、不停全体
    int backing = num == 0x01 || num == 0x02 || num == 0x2C || num == 0x2D || num == 0x04 || num == 0x05 || num == 0x13 || num == 0x14 || num == 0x4c || num == 0x51 || num == 0x52;
    atomic_store_explicit(&g_mem_svc_live, 1, memory_order_relaxed);   // 游戏线程已在跑：此后改表要在 VM epoch 里（vm_mutation_check）
    int fast = num == 0x2C && physical_growth_empty(regs[0], regs[1]);
    g_phys_retry = 0;
    int handled = 0;
    for (;;) {
    // 停世界/名册协调超时（errno=110 ETIMEDOUT）是负载问题：真机 11-06 23:14 在 frame 4.7 s 的
    // 编译/上传高峰里，guest 线程还在游戏代码里没响应停车信号，2 s 预算到期 → 这里 die → abort
    // （会话日志 `FATAL Android memory SVC 0x2d宿主writer协调失败 errno=110` → SIGABRT）。映射表
    // 还没改，重来是安全的：每次 begin 自带预算，重试前让其它 guest 线程再跑一会儿。
    int writer_error = 0;
    if (backing) {
        if (fast || num == 0x02) {
            writer_error = s2a_android_guest_writers_begin_serial();
        } else {
            for (int attempt = 0; attempt < 4; ++attempt) {
                writer_error = s2a_android_guest_writers_begin();
                if (!writer_error || writer_error != ETIMEDOUT) break;
                if (attempt < 3) {
                    struct timespec pause = {0, (long)(50u + 50u * (unsigned)attempt) * 1000000L};
                    nanosleep(&pause, NULL);
                    LOG("【黄】writer 协调超时 errno=110，映射未改：重试第 %d/4 次", attempt + 2);
                }
            }
        }
    }
    if (writer_error) {
        if (writer_error == ECANCELED && s2a_svc_stopping()) s2a_android_guest_park_forever();
        // 宿主停车/锁协议失败不是客体地址错误；不能伪造D401让游戏继续使用未完成映射。
        if (num == 0x04 && map_failure_claim()) {
            map_failure f = map_failure_new(regs[0], regs[1], regs[2], 0, orig, writer_error, "writer-admission-fatal");
            map_failure_log(&f);  // writer 准入失败时不尝试获取 g_mu。
        }
        die("Android memory SVC %#x宿主writer协调失败 errno=%d（尚未修改映射）", num, writer_error);
    }
    if (backing && s2a_svc_stopping()) {
        s2a_android_guest_writers_end();
        s2a_android_guest_park_forever();
    }
    g_phys_fast = fast && !writer_error;
    handled = svc_mem_impl(num, regs, orig);
    g_phys_fast = 0;
    if (backing) {
        if (s2a_android_guest_writers_revoked()) {
            // 调用栈里已经把本线程的事务撤销了（升级停世界到期）：epoch 不在，正常收尾会撞
            // “end 没有同线程 begin”。这次映射按未完成处理，重新准入后再做一遍。
            handled = 2;
        } else {
            s2a_android_guest_writers_end();
        }
    }
    if (handled != 2) break;
    // 0x2C 全空宿主页的快路在落锁后条件变了、升级全停稳又到期：映射未改，重来一轮。
    // 第二轮起 fast=0，直接取全体停稳 epoch（预算重新计一份），不再依赖只串行通道。
    LOG("writer 停世界升级到期，映射未修改：重取 epoch 重试（第 %d/%d 轮）", g_phys_retry + 1, S2A_PHYS_RETRY_MAX);
    fast = 0;
    ++g_phys_retry;
    }
    return handled;
#else
    int backing = num == 0x01 || num == 0x02 || num == 0x2C || num == 0x2D || num == 0x04 || num == 0x05 || num == 0x13 || num == 0x14 || num == 0x4c || num == 0x51 || num == 0x52;   // 同安卓那张表
    if (backing) pthread_mutex_lock(&g_vm_writer);
    int handled = svc_mem_impl(num, regs, orig);
    if (backing) pthread_mutex_unlock(&g_vm_writer);
    return handled;
#endif
}

#if !defined(__ANDROID__)   // 即时存档只在苹果平台（安卓不编 snapshot.c，开关恒关：runtime/src/snapsw.c）
// ---------- 即时存档：内存账导出 ----------
// 冻结后调（全部游戏线程已停在安全点）。把 svc_mem 的全部账写成一块：布局 L、堆、物理映射掩码（只写非零字节）、映射表、跨度表、权限 / 属性账、TLR / 确定性 arena。
//   内容（页数据）由 snapshot.c 按窗口逐段另存；这里只给「重建映射要知道的结构」。恢复（阶段 2）按同一顺序读回
#define SNAP_SVCMEM_MAGIC 0x324D5653u   // "SVM2"：段式带版本字段格式（snapfmt.h）
static int snap_w(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n ? 0 : -1; }
#include "snapfmt.h"
void s2a_snap_mark_old(void);   // snapshot.c：记「旧版快照接不上」（给玩家的提示分类）
// 单独链本文件的单测（t_svc_mem）没有快照引擎：弱缺省 = 快照关、不记；宿主 / iOS 壳链进 snapshot.c / snapsw.c 的真定义
__attribute__((weak)) void s2a_snap_mark_old(void) {}
__attribute__((weak)) int s2a_snap_restoring(void) { return 0; }
// 内存账（带版本字段格式，snapfmt.h 段 "svcmem" 版本 1）：头记录 = 布局各字段 + 堆 / TLR / 熵；其余每条记录字段 50 标种类：1 映射 2 跨度 3 权限 4 属性 5 物理映射掩码页
#define SVCMEM_LAYOUT(X) X(1, win_base) X(2, win_size) X(3, alias_base) X(4, alias_size) X(5, heap_base) X(6, heap_size) X(7, stack_base) X(8, stack_size) \
    X(9, tlsio_base) X(10, tlsio_size) X(11, aslr_base) X(12, aslr_size) X(13, sys_resource_size) X(14, as_type)
int s2a_snap_svcmem_save(FILE *f) {
    int e = 0; uint32_t mg = SNAP_SVCMEM_MAGIC; GMU_LOCK();
    e |= snap_w(f, &mg, 4);
    sfw w; sfw_sec(&w, f, "svcmem", 1);
    uint64_t heap = (uint64_t)(uintptr_t)g_heap, hc = g_heap_cur, tf = (uint64_t)(uintptr_t)g_tlr_free;
    sfw_rec(&w);
#define WL(id, fld) sfw_u64(&w, id, (uint64_t)L.fld);
    SVCMEM_LAYOUT(WL)
#undef WL
    sfw_u64(&w, 20, heap); sfw_u64(&w, 21, hc); sfw_u64(&w, 22, g_phys); sfw_u64(&w, 23, g_mstack);
    sfw_u64(&w, 30, g_tlr_next); sfw_u64(&w, 31, tf); sfw_u64(&w, 32, g_user_exception_context); sfw_u64(&w, 33, g_det_arena_top);
    sfw_blob(&w, 40, g_entropy, sizeof g_entropy); sfw_u64(&w, 41, (uint64_t)g_entropy_ready);
    sfw_end_rec(&w);
    for (int i = 0; i < g_nmap; i++) { sfw_rec(&w); sfw_u64(&w, 50, 1); sfw_u64(&w, 1, g_map[i].base); sfw_u64(&w, 2, g_map[i].size); sfw_u64(&w, 3, g_map[i].src); sfw_u64(&w, 4, g_map[i].state); sfw_u64(&w, 5, g_map[i].h); sfw_u64(&w, 6, g_map[i].perm); sfw_end_rec(&w); }
    for (int i = 0; i < g_nspan; i++) { sfw_rec(&w); sfw_u64(&w, 50, 2); sfw_u64(&w, 1, g_span[i].lo); sfw_u64(&w, 2, g_span[i].hi); sfw_end_rec(&w); }
    for (int i = 0; i < g_nperm; i++) { sfw_rec(&w); sfw_u64(&w, 50, 3); sfw_u64(&w, 1, g_perm[i].lo); sfw_u64(&w, 2, g_perm[i].hi); sfw_u64(&w, 3, g_perm[i].perm); sfw_end_rec(&w); }
    for (int i = 0; i < g_nattr; i++) { sfw_rec(&w); sfw_u64(&w, 50, 4); sfw_u64(&w, 1, g_attr[i].lo); sfw_u64(&w, 2, g_attr[i].hi); sfw_u64(&w, 3, g_attr[i].perm); sfw_end_rec(&w); }
    uint64_t na = g_amask ? L.alias_size / HPG : 0, nz = 0;
    for (uint64_t i = 0; i < na; i++) if (g_amask[i]) { nz++; sfw_rec(&w); sfw_u64(&w, 50, 5); sfw_u64(&w, 1, i); sfw_u64(&w, 2, g_amask[i]); sfw_end_rec(&w); }
    e |= sfw_end(&w);
    GMU_UNLOCK();
    LOG("即时存档：内存账 堆 %llu MB、物理映射 %llu MB（掩码非零 %llu 页）、映射表 %d 项、跨度 %d、权限账 %d / 属性账 %d、TLR 到 %#llx、arena 顶 %#llx",
        (unsigned long long)(hc >> 20), (unsigned long long)(g_phys >> 20), (unsigned long long)nz, g_nmap, g_nspan, g_nperm, g_nattr, (unsigned long long)g_tlr_next, (unsigned long long)g_det_arena_top);
    return e ? -1 : 0;
}
// 映射表里「共享内存」类区段（内容由服务层重新生成，不存页数据）：snapshot.c 存页时跳过。回个数
int s2a_snap_shared_ranges(uint64_t *b, uint64_t *n, int max) {
    int k = 0; GMU_LOCK();
    for (int i = 0; i < g_nmap && k < max; i++) if (g_map[i].state == ST_SHARED) { b[k] = g_map[i].base; n[k++] = g_map[i].size; }
    GMU_UNLOCK(); return k;
}
// 恢复（阶段 2）：在 B 里按 A 的账重建映射。前提：B 正常开局到装完模块（布局已定、主线程栈已映射），游戏线程未起、C# 服务已起。
//   做法：整表换成 A 的账，再把宿主侧的保护 / 别名按账做出来；页内容由 snapshot.c 随后写回。共享内存（服务层的 buffer）在服务重放后另调 s2a_snap_svcmem_shared。
//   失败回 -1（调用方当场退出，不起游戏线程）
static int snap_r(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n ? 0 : -1; }
static int snap_rw(uint64_t lo, uint64_t hi) {   // [lo, hi) 按宿主页外扩成读写
    lo &= ~(HPG - 1); hi = (hi + HPG - 1) & ~(HPG - 1);
    return hi > lo && mprotect((void *)(uintptr_t)lo, (size_t)(hi - lo), PROT_READ | PROT_WRITE) ? -1 : 0;
}
int s2a_snap_svcmem_load(FILE *f) {
    uint32_t mg = 0; uint64_t heap, hc, phys, mstack, nz = 0, tf, tn, uec, dat;
    if (snap_r(f, &mg, 4) || mg != SNAP_SVCMEM_MAGIC) { LOG("【红】即时存档恢复：内存账文件头不对（旧的整块格式本版不读）"); return -1; }
    sfr r; int rc = sfr_sec(&r, f, "svcmem", 1);
    if (rc || sfr_rec(&r) != 1) { LOG("【红】即时存档恢复：内存账段读不了%s", rc == -2 ? "（格式比本版新）" : ""); sfr_done(&r); return -1; }
    {   // 游戏看得见的布局：逐字段与本局相同才接得上（不同 = 布局变了，C 档）
        int bad = 0;
#define CL(id, fld) if (sfr_u64(&r, id, ~0ull) != (uint64_t)L.fld) { LOG("【红】即时存档恢复：内存布局 %s 与快照不同（快照 %#llx，当前 %#llx）", #fld, (unsigned long long)sfr_u64(&r, id, 0), (unsigned long long)L.fld); bad++; }
        SVCMEM_LAYOUT(CL)
#undef CL
        if (bad) { LOG("【红】即时存档恢复：这个快照是旧版存的，内存布局接不上，拒绝恢复；游戏自己的存档不受影响"); s2a_snap_mark_old(); sfr_done(&r); return -1; }
    }
    heap = sfr_u64(&r, 20, 0); hc = sfr_u64(&r, 21, 0); phys = sfr_u64(&r, 22, 0); mstack = sfr_u64(&r, 23, 0);
    tn = sfr_u64(&r, 30, 0); tf = sfr_u64(&r, 31, 0); uec = sfr_u64(&r, 32, 0); dat = sfr_u64(&r, 33, 0);
    uint8_t ent[sizeof g_entropy]; int ent_ok = sfr_into(&r, 40, ent, sizeof ent); int ent_ready = (int)sfr_u64(&r, 41, 0);
    if (heap != (uint64_t)(uintptr_t)g_heap && g_heap) { LOG("【红】即时存档恢复：堆基址不同（快照 %#llx，当前 %p）", (unsigned long long)heap, (void *)g_heap); sfr_done(&r); return -1; }
    GMU_LOCK();
    int e = 0;
    g_heap = (uint8_t *)(uintptr_t)heap; g_heap_cur = hc; g_phys = phys; g_mstack = mstack; g_pool_grown = 0; if (mem_used() > APP_POOL) g_pool_grown = mem_used();   // 快照时扩过的池：恢复后照已用量接上
    if (hc) e |= snap_rw(heap, heap + hc);
    // 映射表、跨度、权限 / 属性账、物理映射掩码：整表换成快照的
    g_nmap = 0; g_nspan = 0; free(g_perm); g_perm = NULL; g_nperm = 0; free(g_attr); g_attr = NULL; g_nattr = 0; int cperm = 0, cattr = 0, far_pages = 0;
    for (;;) {
        int t = sfr_rec(&r); if (t == 0) break; if (t < 0) { e = -1; break; }
        switch ((int)sfr_u64(&r, 50, 0)) {
        case 1: if (g_nmap >= MAX_MAP) { e = -1; break; }
            g_map[g_nmap].base = sfr_u64(&r, 1, 0); g_map[g_nmap].size = sfr_u64(&r, 2, 0); g_map[g_nmap].src = sfr_u64(&r, 3, 0);
            g_map[g_nmap].state = (uint32_t)sfr_u64(&r, 4, 0); g_map[g_nmap].h = (uint32_t)sfr_u64(&r, 5, 0); g_map[g_nmap].perm = (uint32_t)sfr_u64(&r, 6, 3); g_nmap++; break;
        case 2: if (g_nspan >= MAX_MAP) { e = -1; break; } g_span[g_nspan].lo = sfr_u64(&r, 1, 0); g_span[g_nspan].hi = sfr_u64(&r, 2, 0); g_nspan++; break;
        case 3: case 4: { int k = (int)sfr_u64(&r, 50, 0) == 3; struct perm_ov **pp = k ? &g_perm : &g_attr; int *np = k ? &g_nperm : &g_nattr, *cp = k ? &cperm : &cattr;
            if (*np == *cp) { *cp = *cp ? *cp * 2 : 64; *pp = realloc(*pp, sizeof **pp * (size_t)*cp); }
            (*pp)[*np].lo = sfr_u64(&r, 1, 0); (*pp)[*np].hi = sfr_u64(&r, 2, 0); (*pp)[*np].perm = (uint32_t)sfr_u64(&r, 3, 0); (*np)++; break; }
        case 5: { uint64_t ix = sfr_u64(&r, 1, 0); uint8_t v = (uint8_t)sfr_u64(&r, 2, 0); nz++;
            *amask_at(L.alias_base + ix * HPG) = v;
            uint64_t a = L.alias_base + ix * HPG, wb, ws; s2a_window_get(&wb, &ws);
            if (a + HPG > wb + ws) { far_pages++; break; }
            e |= snap_rw(a, a + HPG); break; }
        default: break;   // 新版加的种类：本版不认，跳过
        }
        if (e) break;
    }
    sfr_done(&r);
    if (far_pages) LOG("即时存档恢复：物理映射有 %d 页在远端（窗口外），由远端按游戏地址恢复（far.bin）", far_pages);
    // 映射表里各段的宿主侧做出来（共享内存除外，等服务重放）
    int nalias = 0, ncopy = 0, nshared = 0;
    for (int i = 0; i < g_nmap && !e; i++) {
        uint32_t st = g_map[i].state;
        if (st == ST_SHARED) { nshared++; continue; }
        if (st == ST_STACK && g_map[i].src && g_map[i].h == MAP_ALIAS) { if (alias_map(g_map[i].base, g_map[i].src, g_map[i].size, VM_PROT_READ | VM_PROT_WRITE)) { LOG("【红】即时存档恢复：栈别名 %#llx 重建失败", (unsigned long long)g_map[i].base); e = -1; } nalias++; continue; }
        if (st == ST_STACK) { e |= snap_rw(g_map[i].base, g_map[i].base + g_map[i].size); ncopy++; continue; }
        if (st == ST_TMEM || st == ST_TMEM_ISO) { if (alias_map(g_map[i].base, g_map[i].src, g_map[i].size, VM_PROT_READ | VM_PROT_WRITE)) e = -1; nalias++; continue; }
        LOG("【红】即时存档恢复：映射表第 %d 项状态 %#x（代码内存等）首版不支持，拒绝恢复", i, st); e = -1;
    }
    // TLR 与确定性 arena（线程 TLS 块 / 线程对象）
    g_tlr_next = tn; g_tlr_free = (void *)(uintptr_t)tf; g_user_exception_context = uec; g_det_arena_top = dat;
    if (tn > L.tlsio_base) e |= snap_rw(L.tlsio_base, tn);
    if (dat && dat < L.tlsio_base + L.tlsio_size) e |= snap_rw(dat, L.tlsio_base + L.tlsio_size);
    if (ent_ok) { memcpy(g_entropy, ent, sizeof g_entropy); g_entropy_ready = ent_ready; }
    GMU_UNLOCK();
    LOG("%s即时存档恢复：内存账 堆 %llu MB、物理映射页 %llu、映射表 %d 项（别名 %d、拷贝栈 %d、共享内存待接 %d）", e ? "【红】" : "", (unsigned long long)(hc >> 20), (unsigned long long)nz, g_nmap, nalias, ncopy, nshared);
    return e ? -1 : 0;
}
// 页内容写回后：按 A 的权限账收紧保护（只收紧整宿主页、同 0x02 的规则：模块映像里的不改宿主保护）
void s2a_snap_svcmem_perms(void) {
    GMU_LOCK(); int n = 0;
    for (int i = 0; i < g_nperm; i++) {
        uint64_t lo = (g_perm[i].lo + HPG - 1) & ~(HPG - 1), hi = g_perm[i].hi & ~(HPG - 1); if (hi <= lo) continue;
        uint32_t p = g_perm[i].perm; int prot = (p & 1 ? PROT_READ : 0) | (p & 2 ? PROT_WRITE : 0);
        if (p & 4) continue;   // 可执行的段（模块代码）不动
        if (!mprotect((void *)(uintptr_t)lo, (size_t)(hi - lo), prot)) n++;
    }
    GMU_UNLOCK(); LOG("即时存档恢复：权限账收紧 %d 段", n);
}
// 服务重放后：共享内存映射按句柄找到 B 里的后备 buffer，在 A 的地址重新别名映射
void *s2a_shmem_lookup(uint32_t h, uint64_t *size);
int s2a_snap_svcmem_shared(void) {
    GMU_LOCK(); int e = 0, n = 0;
    for (int i = 0; i < g_nmap; i++) if (g_map[i].state == ST_SHARED) {
        uint64_t sz = 0; uint8_t *buf = s2a_shmem_lookup(g_map[i].h, &sz);
        if (!buf) { LOG("【红】即时存档恢复：共享内存句柄 %#x 在 B 里没有对象", g_map[i].h); e = -1; continue; }
        uint64_t span = (g_map[i].size + HPG - 1) & ~(HPG - 1);
        vm_address_t a = (vm_address_t)g_map[i].base; vm_prot_t cur, max;
        if (vm_remap(mach_task_self(), &a, (vm_size_t)span, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)buf, FALSE, &cur, &max, VM_INHERIT_SHARE) != KERN_SUCCESS) { e = -1; continue; }
        int prot = (g_map[i].perm & 1 ? VM_PROT_READ : 0) | (g_map[i].perm & 2 ? VM_PROT_WRITE : 0);
        vm_protect(mach_task_self(), a, (vm_size_t)span, FALSE, prot); n++;
    }
    GMU_UNLOCK(); LOG("%s即时存档恢复：共享内存重接 %d 段", e ? "【红】" : "", n); return e;
}
// 即时存档恢复：当前堆（已提交部分）的起点与大小（重放暂存区借堆顶用，先备份后还原）
void s2a_snap_heap(uint64_t *base, uint64_t *cur) { GMU_LOCK(); *base = (uint64_t)(uintptr_t)g_heap; *cur = g_heap_cur; GMU_UNLOCK(); }
// 即时存档恢复：服务重放暂存区。在别名区最顶端临时登记一段（打开读写），服务按游戏地址读写它认作已映射；重放完撤掉、改回不可访问
//   （与游戏内存完全分开：之后若有服务异步往这里写，会当场崩在这个地址上，而不是悄悄写坏游戏数据）
uint64_t s2a_snap_scratch_map(uint64_t n) {
    // 取别名区顶端，但不能伸出宿主窗口：开远端时（iPhone 大地址档）别名区顶落在远端，那段游戏地址在服务层的内存追踪里无效，
    //   重放请求的应答一写进消息后面的接收区就报无效内存区（一款大游戏：AudioDevice#10、DeliveryCacheProgressService#1 等）
    uint64_t wb, ws; s2a_window_get(&wb, &ws); uint64_t top = L.alias_base + L.alias_size; if (top > wb + ws) top = wb + ws;
    uint64_t a = top - n; GMU_LOCK();
    for (uint64_t p = a; p < a + n; p += HPG) if (*amask_at(p)) { GMU_UNLOCK(); return 0; }
    if (map_find(a, n) || mprotect((void *)(uintptr_t)a, (size_t)n, PROT_READ | PROT_WRITE)) { GMU_UNLOCK(); return 0; }
    map_add(a, n, 0, ST_STACK, 0); GMU_UNLOCK(); return a;
}
void s2a_snap_scratch_unmap(uint64_t a, uint64_t n) {
    GMU_LOCK(); int i = map_find(a, n); if (i) map_del(i - 1);
    memset((void *)(uintptr_t)a, 0, (size_t)n); mprotect((void *)(uintptr_t)a, (size_t)n, PROT_NONE); GMU_UNLOCK();
}
#endif
