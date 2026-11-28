// Write-protect dirty-page tracking for guest memory: pool pages of packaged textures (CPU writes, GPU reads) are set read-only after upload, and as soon as the game writes it faults -> mark dirty, unprotect;
// before use, check the write generation of the covered pages and re-upload if newer than at upload time. NVN semantics: a texture descriptor points to pool memory, and the GPU reads the current content every time
// (measured: the game writes streamed assets into texture storage that is still being sampled, without re-registering).
// Signal handler constraint: the page-fault path uses only atomic operations and mprotect, no allocation, no locks.
#include "nvn_dirty.h"
#include "../../host/dm.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif
#include <sched.h>

// Platform clock: Apple keeps mach_absolute_time (async-signal-safe, already used by the fault path);
// other hosts use CLOCK_MONOTONIC ns, so the dirty-page policy and its test build off-Mac too.
#if defined(__APPLE__)
static uint64_t dirty_tick(void) { return mach_absolute_time(); }
static void dirty_tick_rate(uint64_t *num, uint64_t *den) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb); *num = tb.numer; *den = tb.denom;
}
#else
static uint64_t dirty_tick(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void dirty_tick_rate(uint64_t *num, uint64_t *den) { *num = 1; *den = 1; }
#endif

// Order of page state and mprotect: the side changing protection first sets the state to BUSY and sets the final state only after mprotect has landed; other threads wait when they see BUSY.
//   set read-only: NONE -> BUSY -> mprotect(read-only) -> PROT; unprotect: PROT -> BUSY -> mprotect(writable) -> NONE.
// 整块解除时，块内本来就是 NONE（可写）的页也要先认领：NONE -> BUSY_WRITABLE。否则并发的“加保护”线程可能在本线程
//   mprotect(可写) 之后才落地 mprotect(只读) 并写 PROT，留下“状态 PROT、内存可写”的漏检（维护方审 11-04 第 7 条）。
//   mprotect 失败时按认领前的状态回退（BUSY -> PROT、BUSY_WRITABLE -> NONE），不发布虚假状态。
// Invariant: a page whose state is NONE is always writable, one whose state is PROT is always read-only. Setting read-only used to be "record PROT, then mprotect"; in between, another thread's unprotect-before-write
// (the movie decoder thread writing a frame into texture storage, a disk read into a guest buffer) could set the state back to NONE, then the mprotect landed => the page read-only with state NONE: a game thread writing it got only
// "re-execute" from the fault handler and spun forever; a host system call reading into it saw NONE in prep_write, skipped, and failed with EFAULT (reproduced by t_dirty_race).
// BUSY lasts at most one mprotect (plus a few atomic writes when setting read-only). The wait is capped at BUSY_WAIT_MS and reports red beyond it: in the fault handler it then returns "not our page" so the crash handler reports truthfully
enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2, PG_BUSY_WRITABLE = 3,
       PG_PROBE = 4, PG_PROBE_ARMING = 5, PG_PROBE_OPENING = 6 };
static int page_busy(uint8_t state) {
    return state == PG_BUSY || state == PG_BUSY_WRITABLE ||
           state == PG_PROBE_ARMING || state == PG_PROBE_OPENING;
}
static int probe_page(uint8_t state) {
    return state == PG_PROBE || state == PG_PROBE_ARMING || state == PG_PROBE_OPENING;
}
enum { BUSY_WAIT_MS = 2000 };
// Tracked ranges = the memory pools (registered by MemoryPoolInitialize). After a pool is destroyed its live=0 slot is reused; the signal handler scans without locks.
// The table grows in blocks of 1,024 slots, up to 262,144; a block is never moved or freed, so the signal handler reads it without locks. A pool past the end goes untracked: its textures count
// as dirty and are re-uploaded at every use. The table was a fixed 256 slots (Zelda has about 300 pools at the title), then a fixed 4,096, and Breath of the Wild reaches 4,006 pools in its first
// shrine. The scans only cover slots in use, so the capacity itself costs nothing
enum { BLK_SHIFT = 10, BLK = 1 << BLK_SHIFT, NBLK = 256, MAX_RANGE = BLK * NBLK };
// Ranges are rounded to host pages (16 KB) but guest memory pools are only 4 KB aligned: adjacent pools share their first / last pages, so one address can fall in several registered ranges.
// So: "is this texture tracked" looks for a range that holds its whole span; a fault / host pre-write unprotects and stamps every range covering that page; pool destruction identifies its range by the pool's exact start mem.
// (Previously the first range containing the address was always taken: two SMBW ASTC 6x5 textures whose pool's first page is shared with a small preceding pool were judged untracked and re-uploaded on every use, about 6 MB per frame)
// 脏页追加日志（#1501 A②）：本模块里不配置就不记（g_log_enabled 初值 0），须在任何池登记前 nvn_dirty_log_configure。
// 安卓 nvk_session 按开关 debug.s2a.nvk_buf_dirtylog 配置，缺省开、设 0 关；苹果侧不调 configure，日志不启用。
// 一页从只读变可写（缺页 / 宿主写前解除）就追加一条 [a, b)（宿主页对齐的地址），在 mprotect 之前发布——写入必然晚于这条事件。
// 两次加保护之间一页最多产生一次事件，所以「镜像上次同步后被写过的页」= 它的游标之后的事件，消费方按事件数付钱，不按范围页数。
// 缺页路径里只有原子操作：条目预分配（总量有上限 g_log_bytes），写者 fetch_add 取槽、先清 seq 再写区间、最后发布 seq = 下标 + 1。
// 读者按 seqlock 读（见 nvn_dirty_log_read）；环绕（游标落后超过容量）、跟踪不可靠、换底、与别的池重叠、池销毁时判不可用，调用方退回逐页核对。
typedef struct dirty_log {
    uint64_t ident; uint32_t cap; _Atomic int bad;
    _Atomic uint64_t head;
    struct dirty_log *next;
    struct { _Atomic uint64_t seq; _Atomic uintptr_t a, b; } ev[];
} dirty_log;
struct probe_epoch_page {
    _Atomic uint64_t serial;
    _Atomic(struct nvn_dirty_deferred_source *) deferred;
};
struct probe_epochs {
    struct probe_epochs *next;
    uintptr_t base, end;
    const _Atomic uint8_t *state;
    struct probe_epoch_page page[];
};
typedef struct { uintptr_t base, end, mem; _Atomic uint8_t *state; _Atomic uint64_t *gen; size_t cap; _Atomic int live;
                 _Atomic(dirty_log *) log;
                 _Atomic(struct probe_epochs *) probe_epochs;
                 _Atomic uint64_t last; } range_t;   // last: the largest write generation of any page in the range (CAS max next to every gen store): a range not written since a stamp answers without its page scan
static inline void last_max(range_t *r, uint64_t g) { uint64_t c = atomic_load(&r->last); while (c < g && !atomic_compare_exchange_weak(&r->last, &c, g)) {} }   // async-signal-safe: atomics only
static _Atomic(range_t *) g_blk[NBLK];
static inline range_t *rg(int i) { return &atomic_load_explicit(&g_blk[i >> BLK_SHIFT], memory_order_acquire)[i & (BLK - 1)]; }
static _Atomic int g_nr;
static uintptr_t g_pg, g_shift;
static _Atomic uint64_t g_now = 1;
static _Atomic uint64_t g_faults, g_preps;
static _Atomic uint64_t g_adds_total;
static _Atomic int g_live, g_live_peak;   // number of registered pools and peak (for the memory readout line)
void nvn_dirty_stat(int *live, int *peak, int *cap) { *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak); *cap = MAX_RANGE; }
static _Atomic int g_overflow_logged;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // Pool creation / destruction (guest threads, possibly concurrent) pick slots under a mutex; the page-fault handling path does not touch it
// Range-table writers hold g_slot_mu and bracket publication/reuse with odd /
// even epochs. Never reset this counter: other threads can retain negative
// queries across sessions. Page protection and write generations are unrelated.
// find_span's sorted index (below) trusts this epoch too, as a seqlock: a writer that changes base / end / live / the index without bracketing it lets a reader return a stale answer.
static _Atomic uint64_t g_table_epoch = 2;
// 写者的括号。开工：代际变奇数，并且这次写必须先于后面对表和索引的任何写被别的线程看到（所以补一道 release 栅栏，
// 光靠这次自增本身只约束它之前的访问）；收工：代际变偶数，release 保证表和索引的写都先于它。读者的配套读法在 find_span。
static inline void table_write_begin(void) { atomic_fetch_add_explicit(&g_table_epoch, 1, memory_order_relaxed); atomic_thread_fence(memory_order_release); }
static inline void table_write_end(void) { atomic_fetch_add_explicit(&g_table_epoch, 1, memory_order_release); }
typedef struct { uintptr_t addr; size_t len; uint64_t epoch; } prep_miss;
static _Thread_local prep_miss g_prep_miss[64];
// Old arrays can still be held by a fault reader during slot reuse. Keep them
// until the same drained reset that already frees the current range arrays.
typedef struct retired_arrays {
    _Atomic uint8_t *state;
    _Atomic uint64_t *gen;
    struct retired_arrays *next;
} retired_arrays;
static retired_arrays *g_retired_arrays;   // g_slot_mu; never read by the signal path
// 追加日志：即使槽位复用，缺页线程仍可能握着旧日志，和旧页数组一样留到所有读者退出的会话重置再放；保留量有上限，耗尽则后续池没有日志（消费方退回逐页）。
static dirty_log *g_all_logs;            // g_slot_mu
static size_t g_log_bytes;               // g_slot_mu
static uint64_t g_log_ident;             // g_slot_mu；每次登记一条新日志加一（槽位复用后旧游标对不上）
enum { LOG_BYTES_LIMIT = 64 * 1024 * 1024, LOG_CAP_MIN = 64, LOG_CAP_MAX = 16384 };

// ---- find_span 的有序索引（#1133；取代 #974 的线程内备忘）----
// 线性扫槽位表是 O(池数)：世界场景里池有上千个，回放线程每个走慢路的缓冲解析、每次贴图内容检查都问一次「哪个范围装得下 [a, a+len)」
// （真机取样：线性扫 + 备忘共占回放线程约 4%；备忘只在表没变时有效，命中 68–74%，其余每次整表扫）。
// 索引 = 现存范围按 (起点, 槽号) 排序的数组，外加「隐含的区间树」：以数组下标区间 [lo, hi) 的中点为根递归成树，每个根记子树里最大的 end（sm）。
// 查询 [a, a+len)：从根往下，子树 sm < a+len 整棵剪掉；根的起点 > a 则根和右子树都不可能装得下，只走左边；否则根是候选，左子树起点必然 ≤ a，右子树继续看。
// 复杂度 O(log N + k)，k = 真正装得下的范围数（共用边界页时 ≤ 2）；范围互相嵌套（一个大池里套小池）也不退化。取所有装得下的范围里槽号最小的，所以与线性扫逐位相同。
// 写者（登记 / 注销 / 重置会话）在 g_slot_mu 内、代际为奇数的窗口里增删条目并重算 sm，是 O(N)（N = 现存池数；宿主 -O2 读数：已有 1000 / 3000 个池时一次增或删约 2 / 7 微秒，不含 mprotect）。
// 并发：索引的每个字段都是 relaxed 原子量（读者和写者同时碰它不构成数据竞争）。读者按 seqlock 读：先取偶数代际、读完索引、再复查代际没变才信；
// 奇数（有写者在改）或复查不一致就退回线性扫。块只增不减，直到重置会话才释放，所以读者读到半改状态的条目也不会踩到已释放的内存，只是答案会被复查丢掉。
// 索引不可用（现存范围超上限 / 分配失败 / 与表对不上）时置 g_span_off，之后 find_span 一律线性扫（答案不变，只是慢），重置会话恢复；这是真退路，不是成功兜底。
#ifndef NVN_DIRTY_SPAN_IDX_MAX
#define NVN_DIRTY_SPAN_IDX_MAX 32768   // 索引最多收多少条（= 现存池数）；超了每次登记都要 O(N) 重算，不值得，退回线性扫
#endif
enum { SPAN_BLK_SHIFT = 10, SPAN_BLK = 1 << SPAN_BLK_SHIFT, SPAN_IDX_MAX = NVN_DIRTY_SPAN_IDX_MAX,
       SPAN_NBLK = (SPAN_IDX_MAX + SPAN_BLK - 1) >> SPAN_BLK_SHIFT, SPAN_STACK = 48 };
#define SPAN_NONE 0xFFFFFFFFu
typedef struct { _Atomic uintptr_t base, end, sm; _Atomic uint32_t slot; } span_ent;   // 32 字节：一个节点一条缓存线的一半
#ifndef NVN_DIRTY_SPAN_CALLOC
#define NVN_DIRTY_SPAN_CALLOC() calloc(SPAN_BLK, sizeof(span_ent))   // 单测钩子：换成可以注入失败的分配
#endif
static _Atomic(span_ent *) g_span_blk[SPAN_NBLK];   // 块每块 1024 条，按需分配，只增不减
static _Atomic int g_span_n;                         // 索引条数（= 现存范围数）；写者在 g_slot_mu 内改
static _Atomic int g_span_off;                       // 1 = 索引不可用（见上）；重置会话清零
static inline span_ent *span_at(int i) { return &atomic_load_explicit(&g_span_blk[i >> SPAN_BLK_SHIFT], memory_order_relaxed)[i & (SPAN_BLK - 1)]; }
static inline uintptr_t span_base(const span_ent *e) { return atomic_load_explicit(&e->base, memory_order_relaxed); }
static inline uintptr_t span_end(const span_ent *e) { return atomic_load_explicit(&e->end, memory_order_relaxed); }
static inline uint32_t span_slot(const span_ent *e) { return atomic_load_explicit(&e->slot, memory_order_relaxed); }
static void span_off(const char *why) {
    if (!atomic_exchange(&g_span_off, 1)) LOG("【红】 dirty-page tracking: 取范围索引停用（%s），find_span 退回线性扫（答案不变，只是慢）；重置会话后恢复", why);
}
static int span_lower(uintptr_t base, uint32_t slot, int n) {   // 第一个键 >= (base, slot) 的下标
    int lo = 0, hi = n;
    while (lo < hi) {
        const int mid = (lo + hi) >> 1; const span_ent *e = span_at(mid); const uintptr_t eb = span_base(e);
        if (eb < base || (eb == base && span_slot(e) < slot)) lo = mid + 1; else hi = mid;
    }
    return lo;
}
static void span_copy(span_ent *to, const span_ent *from) {
    atomic_store_explicit(&to->base, span_base(from), memory_order_relaxed);
    atomic_store_explicit(&to->end, span_end(from), memory_order_relaxed);
    atomic_store_explicit(&to->slot, span_slot(from), memory_order_relaxed);
}
static uintptr_t span_sm_build(int lo, int hi) {   // 返回 [lo, hi) 里最大的 end，并把它记在这棵子树的根（中点）上
    if (lo >= hi) return 0;
    const int mid = (lo + hi) >> 1; span_ent *e = span_at(mid);
    uintptr_t m = span_end(e);
    const uintptr_t l = span_sm_build(lo, mid), r = span_sm_build(mid + 1, hi);
    if (l > m) m = l;
    if (r > m) m = r;
    atomic_store_explicit(&e->sm, m, memory_order_relaxed);
    return m;
}
// 以下三个只在 g_slot_mu 内、代际为奇数时调（写者的括号里）
static void span_insert(uintptr_t base, uintptr_t end, uint32_t slot) {
    if (atomic_load_explicit(&g_span_off, memory_order_relaxed)) return;
    const int n = atomic_load_explicit(&g_span_n, memory_order_relaxed);
    if (n >= SPAN_IDX_MAX) { span_off("现存范围超过索引上限"); return; }
    if (!atomic_load_explicit(&g_span_blk[n >> SPAN_BLK_SHIFT], memory_order_relaxed)) {   // 先发布块，再让 g_span_n 数上它
        span_ent *nb = NVN_DIRTY_SPAN_CALLOC();
        if (!nb) { span_off("内存分配失败"); return; }
        atomic_store_explicit(&g_span_blk[n >> SPAN_BLK_SHIFT], nb, memory_order_release);
    }
    const int p = span_lower(base, slot, n);
    for (int i = n; i > p; i--) span_copy(span_at(i), span_at(i - 1));
    span_ent *e = span_at(p);
    atomic_store_explicit(&e->base, base, memory_order_relaxed); atomic_store_explicit(&e->end, end, memory_order_relaxed); atomic_store_explicit(&e->slot, slot, memory_order_relaxed);
    atomic_store_explicit(&g_span_n, n + 1, memory_order_release);
    span_sm_build(0, n + 1);
}
static void span_remove(uintptr_t base, uint32_t slot) {
    if (atomic_load_explicit(&g_span_off, memory_order_relaxed)) return;
    const int n = atomic_load_explicit(&g_span_n, memory_order_relaxed), p = span_lower(base, slot, n);
    if (p >= n || span_base(span_at(p)) != base || span_slot(span_at(p)) != slot) { span_off("索引里找不到要注销的范围"); return; }
    for (int i = p; i < n - 1; i++) span_copy(span_at(i), span_at(i + 1));
    atomic_store_explicit(&g_span_n, n - 1, memory_order_release);
    span_sm_build(0, n - 1);
}
static void span_reset(void) {   // 重置会话：所有读者线程已收拢（见 nvn_dirty_reset_session），块可以放掉
    for (int b = 0; b < SPAN_NBLK; b++) free(atomic_exchange(&g_span_blk[b], NULL));
    atomic_store(&g_span_n, 0); atomic_store(&g_span_off, 0);
}
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;
// 保护分块（页数，相对所属映射起点对齐）。共享默认 0 = 逐页，与 main 行为一致：
// Apple 路径（iOS / Mac 也编本文件）没有 VMA 上限问题，不能替它付整块重传的代价（维护方审 11-04）。
// 安卓直连为什么分块：真机反例里一个 32 MiB AHB 初始只有 1 条 VMA，逐页 mprotect 后变成 65 条，
// 而且恢复读写、甚至整段恢复都不合并——碎片永久留下，世界跑几分钟就把 vm.max_map_count 吃满。
// 安卓在第一次纹理上传前由 fd_apply_chunk_kb() 设成 1 MiB（debug.s2a.chunk_kb 可调）。
static _Atomic uint32_t g_chunk_pages = 0;
// 跟踪不可靠：任何一次 mprotect 失败都会置位。置位后所有查询走保守路径（整张视为脏 /
// 代际返回 -1），不再只靠“旧代际比较”判断——否则失败后仍会认为内容没变而漏传。
static _Atomic int g_track_unreliable;
static int g_log_enabled;      // g_slot_mu；只能在没有活池时设置（追加日志），缺页路径不读取此变量。
// 映射代际：换底/重建后自增，供纹理缓存判定“旧句柄/旧保护状态已失效，必须重新验证并重新加保护”。
static _Atomic uint64_t g_mapping_gen = 1;
void nvn_dirty_set_chunk_pages(uint32_t pages) { atomic_store(&g_chunk_pages, pages); }
uint32_t nvn_dirty_chunk_pages(void) { return atomic_load(&g_chunk_pages); }
int nvn_dirty_log_configure(int enabled) {
    pthread_mutex_lock(&g_slot_mu);
    int ok = atomic_load(&g_live) == 0;
    if (enabled && (ATOMIC_LLONG_LOCK_FREE != 2 || ATOMIC_LONG_LOCK_FREE != 2 ||
                    ATOMIC_INT_LOCK_FREE != 2 || ATOMIC_POINTER_LOCK_FREE != 2)) ok = 0;
    if (ok) g_log_enabled = !!enabled;
    pthread_mutex_unlock(&g_slot_mu);
    return ok;
}
int nvn_dirty_tracking_unreliable(void) { return atomic_load(&g_track_unreliable); }   // This module's mprotect: call count, pages covered, time spent (clock ticks, safe in the page-fault path too)
static _Atomic uint64_t g_mp_fails;
static _Atomic int g_mp_fail_logged;
// 返回 mprotect 的结果：失败时必须由调用方决定“不推进状态、不声称已处理”。
// 之前忽略返回值，失败后照样把页面记成已解除保护并声称异常已处理，会导致写异常反复触发。
// 单测钩子（默认 NULL）：记录实际 mprotect 区间，或注入失败。生产路径不设钩子，零开销。
int (*nvn_dirty_mp_hook)(void *addr, size_t len, int prot);
static int mp(uintptr_t a, size_t len, int prot) {
    if (nvn_dirty_mp_hook && nvn_dirty_mp_hook((void *)a, len, prot) != 0) {
        atomic_fetch_add(&g_mp_fails, 1);
        return -1;   // 注入失败：按真失败处理
    }
    uint64_t t = dirty_tick();
    int rc = mprotect((void *)a, len, prot);
    atomic_fetch_add(&g_mp_ticks, dirty_tick() - t); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, len >> g_shift);
    if (rc != 0) {
        int e = errno;
        atomic_fetch_add(&g_mp_fails, 1);
        if (!atomic_exchange(&g_mp_fail_logged, 1))
            LOG("【红】 dirty-page tracking: mprotect(%p, %zu, prot=%d) 失败 errno=%d；页面状态不推进，也不声称异常已处理（VMA 压力下 mmap/mprotect 可能返回 ENOMEM）",
                (void *)a, len, prot, e);
    }
    return rc;
}

static uint64_t g_tb_num, g_tb_den;   // obtained when the first pool is registered (the fault path only multiplies and divides)
// This session has handed on_fault / prep_write to the native layer. Per session, not per process: the in-process restart clears every direct-host callback
//   slot (runtime/src/direct_host.c s2a_direct_host_session_reset); registering once per process left the second session's protected pages with no fault owner
//   (11-02 BotW restart on an iPhone: SIGBUS on the first guest write to a pool page, main+0xd87c80)
static _Atomic int g_hooks_on;
static _Atomic int g_busy_red;
static _Atomic(nvn_dirty_read_probe_redirect_fn) g_probe_redirect;
static _Atomic(int (*)(void *)) g_probe_guest_pc;
static _Atomic uint64_t g_probe_host_fault_read, g_probe_host_fault_write;
static _Atomic uint64_t g_probe_armed, g_probe_read, g_probe_write, g_probe_host_read, g_probe_host_write;
static _Atomic uint64_t g_probe_censored;
static struct nvn_dirty_probe_access *g_probe_access;   // g_slot_mu
static unsigned g_probe_vm_scopes;                     // g_slot_mu
static struct probe_epochs *g_probe_epoch_tables;      // g_slot_mu; free only after signal users retire
static size_t g_probe_epoch_bytes;
static uint64_t g_probe_next_epoch;                    // monotonic across sessions
static _Thread_local struct { uintptr_t page; uint64_t epoch; } g_probe_retry;
enum { PROBE_EPOCH_BYTES_LIMIT = 32 * 1024 * 1024 };
// Guest pointers may be exported before an NVN pool exists. Keep their
// address intervals through pool reuse. Only drained session reset proves
// that every outstanding managed view and native borrower has retired.
enum { PROBE_EXPORT_CAP = 8192 };
struct probe_export { uintptr_t lo, hi; };
static struct probe_export g_probe_export[PROBE_EXPORT_CAP];  // sorted, disjoint; g_slot_mu
static size_t g_probe_export_count;
static _Atomic int g_probe_export_table_full;
static _Atomic uint64_t g_probe_exports, g_probe_export_refused;
static _Atomic uint64_t g_defer_published, g_defer_materialized, g_defer_replaced;
static _Atomic uint64_t g_defer_by_reason[NVN_DEFER_MATERIALIZE_COUNT];
static _Atomic uint64_t g_probe_fail, g_probe_mp_calls, g_probe_mp_ticks;

void nvn_dirty_read_probe_set_redirect(nvn_dirty_read_probe_redirect_fn redirect) {
    atomic_store_explicit(&g_probe_redirect, redirect, memory_order_release);
}
void nvn_dirty_read_probe_set_guest_pc(int (*is_guest_pc)(void *)) {
    atomic_store_explicit(&g_probe_guest_pc, is_guest_pc, memory_order_release);
}
void nvn_dirty_read_probe_stats(struct nvn_dirty_read_probe_stats *out) {
    uint64_t num = g_tb_num, den = g_tb_den;
    if (!den) dirty_tick_rate(&num, &den);
    *out = (struct nvn_dirty_read_probe_stats){
        .armed = atomic_load(&g_probe_armed), .read = atomic_load(&g_probe_read),
        .write = atomic_load(&g_probe_write), .host_read = atomic_load(&g_probe_host_read),
        .host_write = atomic_load(&g_probe_host_write), .censored = atomic_load(&g_probe_censored),
        .failed = atomic_load(&g_probe_fail), .mp_calls = atomic_load(&g_probe_mp_calls),
        .mp_ns = atomic_load(&g_probe_mp_ticks) * num / den,
        .host_fault_read = atomic_load(&g_probe_host_fault_read),
        .host_fault_write = atomic_load(&g_probe_host_fault_write),
        .export_calls = atomic_load(&g_probe_exports),
        .export_refused_pages = atomic_load(&g_probe_export_refused),
        .export_table_full = atomic_load(&g_probe_export_table_full),
        .defer_published = atomic_load(&g_defer_published),
        .defer_materialized = atomic_load(&g_defer_materialized),
        .defer_replaced = atomic_load(&g_defer_replaced),
    };
    for (unsigned i = 0; i < NVN_DEFER_MATERIALIZE_COUNT; i++)
        out->defer_by_reason[i] = atomic_load_explicit(&g_defer_by_reason[i], memory_order_relaxed);
}
static int probe_mp(uintptr_t addr, size_t len, int prot) {
    const uint64_t t = dirty_tick();
    const int result = mp(addr, len, prot);
    atomic_fetch_add(&g_probe_mp_ticks, dirty_tick() - t);
    atomic_fetch_add(&g_probe_mp_calls, 1);
    if (result) atomic_fetch_add(&g_probe_fail, 1);
    return result;
}
// All guest fault handlers and graphics callbacks have been joined before this
// entry. Old protections belong to the native window that is remapped next.
void nvn_dirty_reset_session(void) {
    pthread_mutex_lock(&g_slot_mu);
    if (atomic_load(&g_defer_published) != atomic_load(&g_defer_materialized) + atomic_load(&g_defer_replaced))
        die("dirty writeback: session reset with retained producer pages");
    table_write_begin();
    span_reset();
    for (int b = 0; b < NBLK; b++) {
        range_t *r = atomic_exchange(&g_blk[b], NULL);
        if (!r) continue;
        for (int i = 0; i < BLK; i++) { free(r[i].state); free(r[i].gen); }
        free(r);
    }
    while (g_retired_arrays) {
        retired_arrays *r = g_retired_arrays; g_retired_arrays = r->next;
        free(r->state); free(r->gen); free(r);
    }
    while (g_all_logs) { dirty_log *l = g_all_logs; g_all_logs = l->next; free(l); }
    while (g_probe_epoch_tables) {
        struct probe_epochs *epochs = g_probe_epoch_tables;
        g_probe_epoch_tables = epochs->next;
        free(epochs);
    }
    g_probe_epoch_bytes = 0;
    g_log_bytes = 0; g_log_enabled = 0;
    atomic_store(&g_nr, 0); atomic_store(&g_live, 0); atomic_store(&g_live_peak, 0);
    atomic_store(&g_now, 1); atomic_store(&g_overflow_logged, 0); atomic_store(&g_busy_red, 0); atomic_store(&g_hooks_on, 0);
    nvn_dirty_read_probe_set_redirect(NULL);
    nvn_dirty_read_probe_set_guest_pc(NULL);
    atomic_store(&g_probe_host_fault_read, 0); atomic_store(&g_probe_host_fault_write, 0);
    atomic_store(&g_probe_armed, 0); atomic_store(&g_probe_read, 0); atomic_store(&g_probe_write, 0);
    if (g_probe_access || g_probe_vm_scopes) die("dirty read probe: session reset while an access scope is outstanding");
    atomic_store(&g_probe_host_read, 0); atomic_store(&g_probe_host_write, 0);
    atomic_store(&g_probe_censored, 0); atomic_store(&g_probe_fail, 0);
    atomic_store(&g_probe_mp_calls, 0); atomic_store(&g_probe_mp_ticks, 0);
    g_probe_export_count = 0;
    atomic_store(&g_probe_export_table_full, 0);
    atomic_store(&g_probe_exports, 0); atomic_store(&g_probe_export_refused, 0);
    atomic_store(&g_defer_published, 0); atomic_store(&g_defer_materialized, 0); atomic_store(&g_defer_replaced, 0);
    for (unsigned i = 0; i < NVN_DEFER_MATERIALIZE_COUNT; i++) atomic_store(&g_defer_by_reason[i], 0);
    table_write_end();
    pthread_mutex_unlock(&g_slot_mu);
}
// Wait for this page to leave BUSY. Returns 1 = it has left; 0 = waited past the limit (reported red once). Usable in a signal handler: only atomic reads, dirty_tick and sched_yield
static int busy_wait(range_t *r, uintptr_t p) {
    uint64_t t0 = 0;
    for (unsigned n = 1; page_busy(atomic_load(&r->state[p])); n++) {
        if (n & 0x3ff) continue;
        uint64_t now = dirty_tick(); if (!t0) t0 = now;
        uint64_t num = g_tb_den ? g_tb_num : 1, den = g_tb_den ? g_tb_den : 1;
        if ((now - t0) * num / den > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
            if (!atomic_exchange(&g_busy_red, 1)) LOG("【红】 dirty-page tracking: page %p stuck 'busy' for over %d ms (the thread changing its protection has not finished), no longer waiting", (void *)(r->base + (p << g_shift)), BUSY_WAIT_MS);
            return 0; }
        sched_yield();
    }
    return 1;
}
static int live_at(int i) { return atomic_load_explicit(&rg(i)->live, memory_order_acquire); }
// The registered range that holds all of [a, a+len) (used for a texture's tracking / protection / dirty check)
static range_t *find_span_scan(uintptr_t a, size_t len) {
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) { range_t *r = rg(i); if (live_at(i) && a >= r->base && a + len <= r->end) return r; }
    return NULL;
}
// ---- find_span：查有序索引，不行就退回线性扫（索引的结构与并发说明见前面 span_ent 一节）----
// 不进信号路径：on_fault 自己扫，不走 find_span。
// 读者的计数是线程自己的（回放线程打印读数时读到的是它自己的），不用原子量。
static _Thread_local uint64_t g_span_idx_hits, g_span_scans, g_span_nodes;
void nvn_dirty_span_stats(uint64_t *idx_hits, uint64_t *scans, uint64_t *nodes) { *idx_hits = g_span_idx_hits; *scans = g_span_scans; *nodes = g_span_nodes; }
// 单测钩子（默认空，零开销）：单测编译时换成「在索引读完、复查代际之前插进一次写者操作」，确定性地走到复查不一致的那条路。
#ifndef NVN_DIRTY_SPAN_AFTER_QUERY
#define NVN_DIRTY_SPAN_AFTER_QUERY() ((void)0)
#endif
// 装得下 [a, endq) 的现存范围里槽号最小的；没有则 SPAN_NONE。读到半改状态的索引时返回值没有意义（调用方复查代际后丢掉），但不会越界。
static uint32_t span_query(uintptr_t a, uintptr_t endq, unsigned *nodes) {
    const int n = atomic_load_explicit(&g_span_n, memory_order_acquire);
    int slo[SPAN_STACK], shi[SPAN_STACK], sp = 0;
    uint32_t best = SPAN_NONE; unsigned visited = 0;
    if (n > 0) { slo[0] = 0; shi[0] = n; sp = 1; }
    while (sp > 0) {
        int lo = slo[--sp], hi = shi[sp];
        while (lo < hi) {
            const int mid = (lo + hi) >> 1; const span_ent *e = span_at(mid);
            visited++;
            if (atomic_load_explicit(&e->sm, memory_order_relaxed) < endq) break;   // 这棵子树里没有谁的 end 够得着：整棵剪掉
            if (span_base(e) > a) { hi = mid; continue; }   // 根和右子树的起点都在 a 之后：只看左边
            if (span_end(e) >= endq) { const uint32_t s = span_slot(e); if (s < best) best = s; }   // 根装得下
            if (mid + 1 < hi) { slo[sp] = mid + 1; shi[sp++] = hi; }   // 右子树留着以后看（里面的起点要逐个比）
            hi = mid;   // 左子树的起点都 ≤ 根的起点 ≤ a，继续往左
        }
    }
    *nodes = visited;
    return best;
}
static range_t *find_span(uintptr_t a, size_t len) {
    if (len && a + len >= a) {   // 零长度（nvn_dirty_tracked 会带 0 进来）和地址回绕：线性扫的比较自有语义，交给它，不在索引里重新发明
        const uint64_t epoch = atomic_load_explicit(&g_table_epoch, memory_order_acquire);
        if (!(epoch & 1) && !atomic_load_explicit(&g_span_off, memory_order_relaxed)) {   // 奇数 = 有写者正在改表（含索引）：不信索引
            unsigned nodes = 0;
            const uint32_t slot = span_query(a, a + len, &nodes);
            NVN_DIRTY_SPAN_AFTER_QUERY();
            atomic_thread_fence(memory_order_acquire);   // 读索引的那些 load 不能挪到下面这次代际复查之后
            if (atomic_load_explicit(&g_table_epoch, memory_order_relaxed) == epoch) {   // 整个查询期间表没变：答案（含「没有」）与线性扫在这张表上的逐位相同
                g_span_idx_hits++; g_span_nodes += nodes;
                return slot == SPAN_NONE ? NULL : rg((int)slot);
            }
        }
    }
    g_span_scans++;
    return find_span_scan(a, len);
}
// 分块边界：以所属映射（r->base）为基准，块内页号 c*CP..c*CP+CP-1，末块按映射长度截断。
// 加保护、缺页解保护、宿主写前解除、范围销毁四条路径都必须用它，否则仍会制造页级碎片。
static uintptr_t chunk_first_page(range_t *r, uintptr_t p) {
    (void)r;
    uint32_t cp = atomic_load(&g_chunk_pages);
    return cp ? (p / cp * cp) : p;
}
static uintptr_t chunk_last_page(range_t *r, uintptr_t p) {
    uint32_t cp = atomic_load(&g_chunk_pages);
    if (!cp) return p;
    uintptr_t npg = (r->end - r->base) >> g_shift;
    if (!npg) return p;
    uintptr_t first = p / cp * cp;
    uintptr_t last = first + cp - 1;
    return last < npg - 1 ? last : npg - 1;
}
// 追加一条事件（缺页路径可调：只有原子操作）。先把槽的 seq 清 0（读者见 0 = 正在写），写区间，最后发布 seq = 下标 + 1。
static void log_append(dirty_log *lg, uintptr_t a, uintptr_t b) {
    const uint64_t i = atomic_fetch_add_explicit(&lg->head, 1, memory_order_acq_rel);
    __typeof__(&lg->ev[0]) e = &lg->ev[i % lg->cap];
    atomic_store_explicit(&e->seq, 0, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&e->a, a, memory_order_relaxed);
    atomic_store_explicit(&e->b, b, memory_order_relaxed);
    atomic_store_explicit(&e->seq, i + 1, memory_order_release);
}
// Operates after signal return, or in a normal host preflight. All owners
// of this page still use the same range state to serialize mprotect.
enum { PROBE_READ, PROBE_WRITE, PROBE_HOST_READ, PROBE_HOST_WRITE,
       PROBE_HOST_FAULT_READ, PROBE_HOST_FAULT_WRITE, PROBE_CENSOR };
size_t nvn_dirty_land_defer_remaining(const struct nvn_dirty_deferred_source *source) {
    return __atomic_load_n(&source->remaining, __ATOMIC_ACQUIRE);
}

static void deferred_page_put(struct nvn_dirty_deferred_source *source) {
    __atomic_fetch_sub(&source->remaining, 1, __ATOMIC_RELEASE);
}

// PG_PROBE_OPENING owns the page. Copy through the private alias while the
// guest mapping is still PROT_NONE. The last page can release the source
// slot immediately after the copy, with no allocation or fence wait here.
static int deferred_materialize(range_t *r, uintptr_t p, enum nvn_defer_materialize reason) {
    struct probe_epochs *epochs = atomic_load(&r->probe_epochs);
    if (!epochs) return 0;
    struct nvn_dirty_deferred_source *source = atomic_exchange(&epochs->page[p].deferred, NULL);
    if (!source) return 0;
    const size_t offset = r->base + (p << g_shift) - (uintptr_t)source->destination;
    memcpy((uint8_t *)source->alias + offset, (const uint8_t *)source->completed_bytes + offset, g_pg);
    atomic_fetch_add_explicit(&g_defer_by_reason[reason], 1, memory_order_relaxed);
    atomic_fetch_add(&g_defer_materialized, 1);
    deferred_page_put(source);
    return 1;
}

static int probe_open_page(range_t *r, uintptr_t p, int access, enum nvn_defer_materialize reason) {
    for (;;) {
        uint8_t state = atomic_load(&r->state[p]);
        if (!probe_page(state)) return 1;
        if (state == PG_PROBE) {
            if (atomic_compare_exchange_strong(&r->state[p], &state, PG_PROBE_OPENING)) break;
        } else if (!busy_wait(r, p)) return 0;
    }
    const uintptr_t a = r->base + (p << g_shift);
    // Handing a deferred page back RW ends the mirror's ownership. A later
    // CPU store through that mapping need not trap, so publish the dirty event
    // before enabling access even when the operation opening the page is a read.
    const int released_deferred = deferred_materialize(r, p, reason);
    if (released_deferred || access == PROBE_WRITE || access == PROBE_HOST_WRITE || access == PROBE_HOST_FAULT_WRITE) {
        dirty_log *log = atomic_load_explicit(&r->log, memory_order_acquire);
        if (log) log_append(log, a, a + g_pg);
        const uint64_t gen = atomic_fetch_add(&g_now, 1) + 1;
        atomic_store(&r->gen[p], gen);
        last_max(r, gen);
    }
    if (probe_mp(a, g_pg, PROT_READ | PROT_WRITE)) {
        atomic_store(&r->state[p], PG_PROBE);
        return 0;
    }
    if (access == PROBE_READ) atomic_fetch_add(&g_probe_read, 1);
    else if (access == PROBE_WRITE) atomic_fetch_add(&g_probe_write, 1);
    else if (access == PROBE_HOST_READ) atomic_fetch_add(&g_probe_host_read, 1);
    else if (access == PROBE_HOST_WRITE) atomic_fetch_add(&g_probe_host_write, 1);
    else if (access == PROBE_HOST_FAULT_READ) atomic_fetch_add(&g_probe_host_fault_read, 1);
    else if (access == PROBE_HOST_FAULT_WRITE) atomic_fetch_add(&g_probe_host_fault_write, 1);
    else atomic_fetch_add(&g_probe_censored, 1);
    atomic_store(&r->state[p], PG_NONE);
    return 1;
}

// Caller holds g_slot_mu: only the registered mapping owner can change state.
// Other CPUs may simultaneously go through the atomic fault callback.
static void probe_restore_span(uintptr_t lo, uintptr_t hi, int access, enum nvn_defer_materialize reason) {
    const int count = atomic_load(&g_nr);
    for (int i = 0; i < count; i++) {
        range_t *r = rg(i);
        if (!live_at(i) || hi <= r->base || lo >= r->end) continue;
        const uintptr_t first = ((lo > r->base ? lo : r->base) - r->base) >> g_shift;
        const uintptr_t last = ((hi < r->end ? hi : r->end) - 1 - r->base) >> g_shift;
        for (uintptr_t p = first; p <= last; p++)
            if (!probe_open_page(r, p, access, reason))
                die("dirty read probe: failed to restore %p", (void *)(r->base + (p << g_shift)));
    }
}

int nvn_dirty_read_probe_vm_begin(void) {
    if (!atomic_load_explicit(&g_probe_redirect, memory_order_acquire)) return 0;
    pthread_mutex_lock(&g_slot_mu);
    g_probe_vm_scopes++;
    const uint64_t armed = atomic_load(&g_probe_armed);
    const uint64_t retired = atomic_load(&g_probe_read) + atomic_load(&g_probe_write) +
        atomic_load(&g_probe_host_read) + atomic_load(&g_probe_host_write) + atomic_load(&g_probe_censored) +
        atomic_load(&g_probe_host_fault_read) + atomic_load(&g_probe_host_fault_write);
    if (armed > retired) probe_restore_span(0, UINTPTR_MAX, PROBE_CENSOR, NVN_DEFER_VM);
    pthread_mutex_unlock(&g_slot_mu);
    return 1;
}

void nvn_dirty_read_probe_vm_end(void) {
    pthread_mutex_lock(&g_slot_mu);
    if (!g_probe_vm_scopes) die("dirty read probe: VM scope end without begin");
    g_probe_vm_scopes--;
    pthread_mutex_unlock(&g_slot_mu);
}

void nvn_dirty_read_probe_access_begin(struct nvn_dirty_probe_access *access, const void *addr,
                                       size_t len, int is_write) {
    *access = (struct nvn_dirty_probe_access){0};
    const uintptr_t lo = (uintptr_t)addr;
    if (!len || len > UINTPTR_MAX - lo ||
        !atomic_load_explicit(&g_probe_redirect, memory_order_acquire)) return;
    pthread_mutex_lock(&g_slot_mu);
    *access = (struct nvn_dirty_probe_access){g_probe_access, lo, lo + len};
    g_probe_access = access;
    probe_restore_span(lo, lo + len, is_write ? PROBE_HOST_WRITE : PROBE_HOST_READ,
                       is_write ? NVN_DEFER_HOST_WRITE : NVN_DEFER_HOST_READ);
    pthread_mutex_unlock(&g_slot_mu);
}

void nvn_dirty_read_probe_access_end(struct nvn_dirty_probe_access *access) {
    if (!access->hi) return;
    pthread_mutex_lock(&g_slot_mu);
    struct nvn_dirty_probe_access **pending = &g_probe_access;
    while (*pending && *pending != access) pending = &(*pending)->next;
    if (!*pending) die("dirty read probe: unknown host access scope");
    *pending = access->next;
    access->hi = 0;
    pthread_mutex_unlock(&g_slot_mu);
}

// First export whose end is >= lo. Adjoining exports can share one
// interval; arm checks below use strict intersection.
static size_t probe_export_lower(uintptr_t lo) {
    size_t a = 0, b = g_probe_export_count;
    while (a < b) {
        const size_t mid = a + (b - a) / 2;
        if (g_probe_export[mid].hi < lo) a = mid + 1;
        else b = mid;
    }
    return a;
}

void nvn_dirty_read_probe_pin_export(const void *addr, size_t len, int is_write) {
    const uintptr_t lo = (uintptr_t)addr;
    if (!len || len > UINTPTR_MAX - lo ||
        !atomic_load_explicit(&g_probe_redirect, memory_order_acquire)) return;
    const uintptr_t hi = lo + len;
    pthread_mutex_lock(&g_slot_mu);
    atomic_fetch_add(&g_probe_exports, 1);
    if (!atomic_load(&g_probe_export_table_full)) {
        uintptr_t begin = lo, end = hi;
        const size_t a = probe_export_lower(lo);
        size_t b = a;
        while (b < g_probe_export_count && g_probe_export[b].lo <= end) {
            if (g_probe_export[b].lo < begin) begin = g_probe_export[b].lo;
            if (g_probe_export[b].hi > end) end = g_probe_export[b].hi;
            b++;
        }
        if (a == b && g_probe_export_count == PROBE_EXPORT_CAP) {
            atomic_store(&g_probe_export_table_full, 1);   // refuse further arms; still restore on every export
        } else {
            memmove(g_probe_export + a + 1, g_probe_export + b,
                    (g_probe_export_count - b) * sizeof g_probe_export[0]);
            g_probe_export[a] = (struct probe_export){begin, end};
            g_probe_export_count = g_probe_export_count - (b - a) + 1;
        }
    }
    probe_restore_span(lo, hi, is_write ? PROBE_HOST_WRITE : PROBE_HOST_READ, NVN_DEFER_EXPORT);
    pthread_mutex_unlock(&g_slot_mu);
}

static void resume_probe(void *argument) {
    const uintptr_t packed = (uintptr_t)argument;
    const uintptr_t page = packed & ~(g_pg - 1);
    const int write = packed & 1;
    const int access = packed & 2 ? PROBE_HOST_FAULT_READ + write : PROBE_READ + write;
    const enum nvn_defer_materialize reason = packed & 2 ? NVN_DEFER_HOST_FAULT_READ + write : NVN_DEFER_GUEST_READ + write;
    const int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) {
        range_t *r = rg(i);
        if (!live_at(i) || page < r->base || page >= r->end) continue;
        if (!probe_open_page(r, (page - r->base) >> g_shift, access, reason))
            die("dirty read probe: failed to restore %p", (void *)page);
    }
}

static size_t probe_arm(const void *addr, size_t len, struct nvn_dirty_deferred_source *source) {
    if (source) source->reject = NVN_DEFER_NO_PROVIDER;
    if (!atomic_load_explicit(&g_probe_redirect, memory_order_acquire) || !g_pg) return 0;
    uintptr_t first = (uintptr_t)addr;
    if (!len || len > UINTPTR_MAX - first || first > UINTPTR_MAX - (g_pg - 1)) return 0;
    const uintptr_t last = (first + len) & ~(g_pg - 1);
    first = (first + g_pg - 1) & ~(g_pg - 1);
    if (last <= first) return 0;

    pthread_mutex_lock(&g_slot_mu);
    if (g_probe_vm_scopes) {
        if (source) source->reject = NVN_DEFER_VM_BUSY;
        pthread_mutex_unlock(&g_slot_mu);
        return 0;
    }
    size_t export = probe_export_lower(first);
    if (export < g_probe_export_count && g_probe_export[export].hi == first) export++;
    if (atomic_load(&g_probe_export_table_full) ||
        (export < g_probe_export_count && g_probe_export[export].lo < last)) {
        if (source) source->reject = NVN_DEFER_PINNED;
        atomic_fetch_add(&g_probe_export_refused, (last - first) >> g_shift);
        pthread_mutex_unlock(&g_slot_mu);
        return 0;
    }
    for (const struct nvn_dirty_probe_access *a = g_probe_access; a; a = a->next)
        if (a->lo < last && first < a->hi) {
            if (source) source->reject = NVN_DEFER_HOST_ACTIVE;
            pthread_mutex_unlock(&g_slot_mu);
            return 0;
        }
    // Refuse any shared host page, including adjacent sub-page pools.
    range_t *owner = NULL;
    const int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) {
        range_t *r = rg(i);
        if (!live_at(i) || last <= r->base || first >= r->end) continue;
        if (owner || first < r->base || last > r->end) {
            owner = NULL;
            break;
        }
        owner = r;
    }
    size_t armed = 0;
    if (source) source->reject = NVN_DEFER_NO_OWNER;
    if (owner) {
        struct probe_epochs *epochs = atomic_load(&owner->probe_epochs);
        if (!epochs) {
            const size_t pages = (owner->end - owner->base) >> g_shift;
            const size_t available = PROBE_EPOCH_BYTES_LIMIT - g_probe_epoch_bytes;
            if (available >= sizeof *epochs && pages <= (available - sizeof *epochs) / sizeof epochs->page[0]) {
                const size_t bytes = sizeof *epochs + pages * sizeof epochs->page[0];
                epochs = calloc(1, bytes);
                if (epochs) {
                    epochs->next = g_probe_epoch_tables;
                    epochs->base = owner->base;
                    epochs->end = owner->end;
                    epochs->state = owner->state;
                    g_probe_epoch_tables = epochs;
                    g_probe_epoch_bytes += bytes;
                    atomic_store(&owner->probe_epochs, epochs);
                }
            }
            if (!epochs) {
                if (source) source->reject = NVN_DEFER_EPOCH_TABLE;
                pthread_mutex_unlock(&g_slot_mu);
                return 0;
            }
        }
        const uintptr_t begin = (first - owner->base) >> g_shift;
        const uintptr_t end = (last - owner->base) >> g_shift;
        const int replace = source && source->replace_verified && atomic_load(&owner->state[begin]) == PG_PROBE;
        const uint8_t prior = replace ? PG_PROBE : PG_NONE;
        if (source) source->reject = NVN_DEFER_PAGE_STATE;
        uintptr_t page = begin;
        for (; page < end; page++) {
            uint8_t state = prior;
            if (!atomic_compare_exchange_strong(&owner->state[page], &state, PG_PROBE_ARMING)) break;
            if (replace) {
                const struct nvn_dirty_deferred_source *old = atomic_load(&epochs->page[page].deferred);
                if (!old || old->version >= source->version) {
                    atomic_store(&owner->state[page], prior);
                    break;
                }
            }
        }
        int next = prior;
        if (page == end) {
            if (source) source->reject = NVN_DEFER_PROTECT_FAILED;
            if (replace || !probe_mp(first, last - first, PROT_NONE)) {
                armed = end - begin;
                const uint64_t epoch = ++g_probe_next_epoch;
                if (source) {
                    source->page_slots = &epochs->page[begin];
                    __atomic_store_n(&source->remaining, armed, __ATOMIC_RELEASE);
                    atomic_fetch_add(&g_defer_published, armed);
                }
                for (uintptr_t p = begin; p < end; p++) {
                    atomic_store(&epochs->page[p].serial, epoch);
                    if (source) {
                        struct nvn_dirty_deferred_source *old = atomic_exchange(&epochs->page[p].deferred, source);
                        if (old) deferred_page_put(old);
                    }
                }
                if (replace) atomic_fetch_add(&g_defer_replaced, armed);
                else atomic_fetch_add(&g_probe_armed, armed);
                next = PG_PROBE;
            } else if (probe_mp(first, last - first, PROT_READ | PROT_WRITE)) {
                die("dirty read probe: failed to roll back %p +%zu", (void *)first, last - first);
            }
        }
        for (uintptr_t p = begin; p < page; p++) atomic_store(&owner->state[p], next);
    }
    pthread_mutex_unlock(&g_slot_mu);
    return armed;
}

size_t nvn_dirty_read_probe_arm(const void *addr, size_t len) {
    return probe_arm(addr, len, NULL);
}

size_t nvn_dirty_land_defer_install(struct nvn_dirty_deferred_source *source, int completed) {
    if (source) source->reject = NVN_DEFER_BAD_SOURCE;
    if (!completed || !source || !source->destination || !source->alias || !source->completed_bytes ||
        !source->length || !source->version || !g_pg || nvn_dirty_land_defer_remaining(source)) return 0;
    const uintptr_t destination = (uintptr_t)source->destination, alias = (uintptr_t)source->alias;
    const uintptr_t bytes = (uintptr_t)source->completed_bytes;
    if (((destination | alias | source->length) & (g_pg - 1)) ||
        source->length > UINTPTR_MAX - destination || source->length > UINTPTR_MAX - alias ||
        source->length > UINTPTR_MAX - bytes ||
        (destination < alias + source->length && alias < destination + source->length) ||
        (bytes < alias + source->length && alias < bytes + source->length) ||
        (bytes < destination + source->length && destination < bytes + source->length)) return 0;
    return probe_arm(source->destination, source->length, source);
}

int nvn_dirty_land_defer_owns_page(const struct nvn_dirty_deferred_source *source, uintptr_t address) {
    const uintptr_t base = (uintptr_t)source->destination;
    if (!source->page_slots || address < base || address - base >= source->length) return 0;
    const __typeof__(((struct probe_epochs *)0)->page[0]) *slots = source->page_slots;
    return atomic_load(&slots[(address - base) >> g_shift].deferred) == source;
}

int nvn_dirty_land_defer_flush_range(struct nvn_dirty_deferred_source *source, uintptr_t addr, size_t len,
                                    enum nvn_defer_materialize reason) {
    if (!nvn_dirty_land_defer_remaining(source)) return 1;
    const uintptr_t begin = (uintptr_t)source->destination;
    const uintptr_t end = begin + source->length;
    if (!len || len > UINTPTR_MAX - addr) return 0;
    const uintptr_t lo = addr > begin ? addr : begin;
    const uintptr_t hi = addr + len < end ? addr + len : end;
    if (hi <= lo) return 1;
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = find_span_scan(begin, source->length);
    struct probe_epochs *epochs = r ? atomic_load(&r->probe_epochs) : NULL;
    int ok = 0;
    if (epochs) {
        const uintptr_t last = (hi - 1 - r->base) >> g_shift;
        ok = 1;
        for (uintptr_t p = (lo - r->base) >> g_shift; p <= last; p++) {
            // A fault may have taken the pointer before finishing its alias
            // copy. Wait only for this CPU-owned page transition.
            if (!busy_wait(r, p) || (atomic_load(&epochs->page[p].deferred) == source &&
                                     !probe_open_page(r, p, PROBE_CENSOR, reason))) {
                ok = 0;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_slot_mu);
    return ok || !nvn_dirty_land_defer_remaining(source);
}

int nvn_dirty_land_defer_flush(struct nvn_dirty_deferred_source *source, enum nvn_defer_materialize reason) {
    return nvn_dirty_land_defer_flush_range(source, (uintptr_t)source->destination, source->length, reason) &&
           !nvn_dirty_land_defer_remaining(source);
}

// 整段解除 first..last（含）：成功则整段记脏（代际推进）——共享块的另一半也必须看到写入，
// 否则只更新发起方会让同块内另一张纹理读到旧内容（维护者指出的“两张纹理共块”反例）。
// 先逐页认领（PROT→BUSY、NONE→BUSY_WRITABLE），挡住并发的加保护，再整段 mprotect：
// 不能 mprotect 完再无条件写状态——另一线程刚把某页 NONE→BUSY→mprotect(READ)，本线程再写 NONE
// 就留下“状态可写、内存只读”的漏检（维护方审 11-04 第 7 条）。
enum unprotect_result { UNPROTECT_FAILED, UNPROTECT_OK, UNPROTECT_PROBE };
static enum unprotect_result unprotect_span_pages(range_t *r, uintptr_t first, uintptr_t last,
                                                  uintptr_t write_first, uintptr_t write_last,
                                                  enum nvn_defer_materialize reason) {
    int probe_claimed = 0;
    for (uintptr_t k = first; k <= last; ) {
        uint8_t st = atomic_load(&r->state[k]);
        if (st == PG_PROT) { if (!atomic_compare_exchange_strong(&r->state[k], &st, PG_BUSY)) continue; }
        else if (st == PG_NONE) { if (!atomic_compare_exchange_strong(&r->state[k], &st, PG_BUSY_WRITABLE)) continue; }
        else if (st == PG_PROBE && k >= write_first && k <= write_last) {
            if (!atomic_compare_exchange_strong(&r->state[k], &st, PG_PROBE_OPENING)) continue;
            probe_claimed = 1;
        }
        else {
            if (st == PG_PROBE || !busy_wait(r, k)) {   // 别人正在改且等超时：把已认领的页按认领前状态回退，不能留下永久 BUSY
                for (uintptr_t j = first; j < k; j++) {
                    uint8_t js = atomic_load(&r->state[j]);
                    if (js == PG_BUSY) atomic_store(&r->state[j], PG_PROT);
                    else if (js == PG_BUSY_WRITABLE) atomic_store(&r->state[j], PG_NONE);
                    else if (js == PG_PROBE_OPENING) atomic_store(&r->state[j], PG_PROBE);
                }
                return st == PG_PROBE ? UNPROTECT_PROBE : UNPROTECT_FAILED;
            }
            continue;   // 等完要回头重判并认领这一页：直接往下走它就没被认领，别的线程可再认领它，后面的整段 mprotect / 发布 / 回退会盖掉别人的认领
        }
        k++;
    }
    uintptr_t begin = r->base + (first << g_shift), end = r->base + ((last + 1) << g_shift);
    {   // 追加日志：在页变可写之前发布（mprotect 失败时这条是多余的，只会让消费方多同步一次；失败本身会置「跟踪不可靠」）
        dirty_log *lg = atomic_load_explicit(&r->log, memory_order_acquire);
        if (lg) log_append(lg, begin, end);
    }
    if (probe_claimed)
        for (uintptr_t k = first; k <= last; k++)
            if (atomic_load(&r->state[k]) == PG_PROBE_OPENING) deferred_materialize(r, k, reason);
    const int rc = probe_claimed ? probe_mp(begin, end - begin, PROT_READ | PROT_WRITE) :
                                   mp(begin, end - begin, PROT_READ | PROT_WRITE);
    if (rc) {
        // 失败：不推进代际，按认领前状态回退，并标记跟踪不可靠（跨 VMA 的 mprotect 可能部分成功，不能假定整段未变）
        for (uintptr_t k = first; k <= last; k++) {
            uint8_t st = atomic_load(&r->state[k]);
            if (st == PG_BUSY) atomic_store(&r->state[k], PG_PROT);
            else if (st == PG_BUSY_WRITABLE) atomic_store(&r->state[k], PG_NONE);
            else if (st == PG_PROBE_OPENING) {
                // The enclosing syscall may have restored only part of the run.
                // Resolve the owned probe page before allowing a dereference.
                const uintptr_t address = r->base + (k << g_shift);
                if (probe_mp(address, g_pg, PROT_READ | PROT_WRITE))
                    die("dirty read probe: failed to restore %p", (void *)address);
                atomic_fetch_add(&g_probe_censored, 1);
                atomic_store(&r->state[k], PG_NONE);
            }
        }
        atomic_store(&g_track_unreliable, 1);
        return UNPROTECT_FAILED;
    }
    const uint64_t g = atomic_fetch_add(&g_now, 1) + 1;
    for (uintptr_t k = first; k <= last; k++) {
        if (atomic_load(&r->state[k]) == PG_PROBE_OPENING) atomic_fetch_add(&g_probe_censored, 1);
        atomic_store(&r->gen[k], g);
        atomic_store(&r->state[k], PG_NONE);
    }
    last_max(r, g);
    return UNPROTECT_OK;
}
// 解除 p 所在的整块。块边界以所属映射为基准（chunk_first/last_page），不越界保护相邻内存。
static enum unprotect_result unprotect_chunk(range_t *r, uintptr_t p) {
    const uintptr_t first = chunk_first_page(r, p), last = chunk_last_page(r, p);
    if (!atomic_load(&g_probe_redirect)) return unprotect_span_pages(r, first, last, 1, 0, NVN_DEFER_GUEST_WRITE);
    for (;;) {
        if (probe_page(atomic_load(&r->state[p]))) return UNPROTECT_PROBE;
        uintptr_t lo = p, hi = p;
        while (lo > first && !probe_page(atomic_load(&r->state[lo - 1]))) lo--;
        while (hi < last && !probe_page(atomic_load(&r->state[hi + 1]))) hi++;
        // A neighbor can arm after the scan. The CAS pass rolls back its
        // claims without changing protection, then we take the new boundary.
        const enum unprotect_result result = unprotect_span_pages(r, lo, hi, 1, 0, NVN_DEFER_GUEST_WRITE);
        if (result != UNPROTECT_PROBE) return result;
    }
}
// Unprotect one page of a range: if protected, unprotect and stamp it; if another thread is changing its protection, wait for it to finish and look again -- if the other side was setting read-only, the page is PROT after the wait and still has to be unprotected
// (an unprotect-before-write never reaches the fault handler, so it must be done here). Returns 1 = this page is managed by this module; 0 = it is not, or the wait on BUSY timed out
static int release_page(range_t *r, uintptr_t p, _Atomic uint64_t *count) {
    for (;;) {
        uint8_t st = atomic_load(&r->state[p]);
        if (st == PG_NONE) return atomic_load(&r->gen[p]) != 0;   // already writable (another thread just unprotected it) -> re-execute; pages never protected are not ours
        if (st == PG_PROBE) return 1;   // An arm raced the fault lookup. Retry through the probe redirect.
        if (st == PG_PROT) {
            const enum unprotect_result result = unprotect_chunk(r, p);
            if (result == UNPROTECT_FAILED) return 0;
            if (result == UNPROTECT_PROBE) return 1;
            if (count) atomic_fetch_add(count, 1);
            return 1;
        }
        if (!busy_wait(r, p)) return 0;   // another thread is changing its protection: wait, then look again
    }
}
static int on_fault(void *addr, int is_write, void *uc) {
    uintptr_t a = (uintptr_t)addr;
    nvn_dirty_read_probe_redirect_fn redirect = atomic_load_explicit(&g_probe_redirect, memory_order_acquire);
    if (redirect) {
        const int n = atomic_load(&g_nr);
        for (int i = 0; i < n; i++) {
            range_t *r = rg(i);
            if (!live_at(i)) continue;
            const struct probe_epochs *epochs = atomic_load(&r->probe_epochs);
            if (!epochs || a < epochs->base || a >= epochs->end) continue;
            const uintptr_t p = (a - epochs->base) >> g_shift;
            const uintptr_t base = a & ~(g_pg - 1);
            if (probe_page(atomic_load(&epochs->state[p]))) {
                int (*guest_pc)(void *) = atomic_load_explicit(&g_probe_guest_pc, memory_order_acquire);
                const int host = guest_pc && !guest_pc(uc);
                const uintptr_t packed = base | (uintptr_t)(host * 2 + (is_write != 0));
                return redirect(uc, resume_probe, (void *)packed) == 1 ? 1 : -1;
            }
            // The kernel can deliver a fault after another thread already
            // restored this page. Permit one retry for this exact arm epoch;
            // a genuine access violation must still reach the normal handler.
            const uint64_t epoch = atomic_load(&epochs->page[p].serial);
            if (epoch && (g_probe_retry.page != base || g_probe_retry.epoch != epoch)) {
                g_probe_retry.page = base;
                g_probe_retry.epoch = epoch;
                return 1;
            }
        }
    }
    if (!is_write) return 0;
    int ours = 0, n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++)   // Every range sharing the page must be released: releasing only one leaves the other range's state at "protected", and later writes no longer fault and are missed
        { range_t *r = rg(i); if (live_at(i) && a >= r->base && a < r->end) ours |= release_page(r, (a - r->base) >> g_shift, &g_faults); }
    return ours;
}
// Unprotect pages p..q of a range with one mprotect per run of protected pages (a host write over a whole texture, e.g. WriteTexels, used to make one system call per 16 KB page:
// Link's Awakening, about 275 a frame). Pages another thread is changing are handled one by one after the run so far is finished, so no BUSY page is held while waiting
// 分块模式下先把 [p,q] 扩到整块边界：块内只要有一段要解除，就整块解除并整块记脏。
// 逐块处理：块内没有只读页就跳过（保持 main 的“不碰可写页”，避免无谓 mprotect / 记脏）；
// 扫描与解除之间若有线程新加保护，写入会再次缺页，由缺页路径兜住。
static void release_host_span(range_t *r, uintptr_t first, uintptr_t last,
                              uintptr_t p, uintptr_t q, _Atomic uint64_t *count,
                              enum nvn_defer_materialize reason) {
    if (!atomic_load(&g_probe_redirect)) {
        if (unprotect_span_pages(r, first, last, p, q, reason) == UNPROTECT_OK && count)
            atomic_fetch_add(count, last - first + 1);
        return;
    }
    for (uintptr_t k = first; k <= last;) {
        if ((k < p || k > q) && probe_page(atomic_load(&r->state[k]))) {
            k++;
            continue;
        }
        uintptr_t hi = k;
        while (hi < last) {
            const uintptr_t next = hi + 1;
            if ((next < p || next > q) && probe_page(atomic_load(&r->state[next]))) break;
            hi = next;
        }
        const enum unprotect_result result = unprotect_span_pages(r, k, hi, p, q, reason);
        if (result == UNPROTECT_PROBE) continue;   // A new neighbor guard shortened this run.
        if (result == UNPROTECT_OK && count) atomic_fetch_add(count, hi - k + 1);
        k = hi + 1;
    }
}
static void release_pages(range_t *r, uintptr_t p, uintptr_t q, _Atomic uint64_t *count,
                          enum nvn_defer_materialize reason) {
    uintptr_t fp = chunk_first_page(r, p), lq = chunk_last_page(r, q);
    // 把连续的「含只读页的块」攒成一段，只调一次 unprotect_span_pages：
    // 分块 = 0（苹果路径）时退化成 main 的「连续只读页一段」，整张 WriteTexels 仍是一次 mprotect；
    // 不能逐块各调一次，否则 16 KiB 页的宿主会从 1 次变 64 次（维护方 Mac 实跑）。
    uintptr_t run_first = 0; int open = 0;
    for (uintptr_t i = fp; i <= lq; ) {
        uintptr_t last = chunk_last_page(r, i);
        if (last > lq) last = lq;
        if (last < i) break;
        int any_prot = 0;
        for (uintptr_t k = i; k <= last; k++) if (atomic_load(&r->state[k]) != PG_NONE) { any_prot = 1; break; }
        if (any_prot) {
            if (!open) { run_first = i; open = 1; }
        } else if (open) {
            release_host_span(r, run_first, i - 1, p, q, count, reason);
            open = 0;
        }
        i = last + 1;
    }
    if (open) release_host_span(r, run_first, lq, p, q, count, reason);
}
static void prep_write_reason(void *addr, size_t len, enum nvn_defer_materialize reason) {
    if (!len) return;
    uintptr_t a = (uintptr_t)addr, e = a + len;
    uint64_t epoch = atomic_load(&g_table_epoch);
    prep_miss *miss = &g_prep_miss[((a >> 2) ^ (a >> 11) ^ len) & 63];
    if (!(epoch & 1) && miss->epoch == epoch && miss->addr == a && miss->len == len &&
        atomic_load(&g_table_epoch) == epoch) return;
    int n = atomic_load(&g_nr), overlap = 0;
    for (int i = 0; i < n; i++) {
        range_t *r = rg(i);
        if (!live_at(i) || e <= r->base || a >= r->end) continue;
        overlap = 1;
        uintptr_t lo = a > r->base ? a : r->base, hi = e < r->end ? e : r->end;
        release_pages(r, (lo - r->base) >> g_shift, (hi - 1 - r->base) >> g_shift, &g_preps, reason);
    }
    // Only a complete, stable no-overlap scan can be remembered. In particular,
    // an overlapping but already-writable page is not a negative query.
    if (!overlap && e >= a && !(epoch & 1) && atomic_load(&g_table_epoch) == epoch)
        *miss = (prep_miss){a, len, epoch};
}
static void prep_write(void *addr, size_t len) {
    prep_write_reason(addr, len, NVN_DEFER_PREP_WRITE);
}

// AHB 换底：同一地址的新映射实际是“可读写”，旧条目的「已保护」状态不能继承。
// 否则 find_span 先命中旧条目 → 以为已保护而跳过保护 → 游戏写入不触发异常 → 代际不变
// → 纹理缓存误判“内容没变”，表现为花屏/噪点（维护者反例：换底后写入 0 次异常、判脏=否）。
// 这里在同一把表锁内：原地重置状态与代际（旧句柄继续指向同一条 range，看到的是新状态）、
// 发布新的映射代际、并按分块重新加只读保护。
int nvn_dirty_replace_range(const void *mem, size_t size) {
    if (!g_pg) { g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg); }
    pthread_mutex_lock(&g_slot_mu);
    table_write_begin();
    range_t *r = NULL;
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n && !r; i++) if (live_at(i) && rg(i)->mem == (uintptr_t)mem) r = rg(i);
    if (!r) { table_write_end(); pthread_mutex_unlock(&g_slot_mu); return 0; }
    // 换底长度/对齐与登记时不同：旧的页状态数组按旧 end 分配，不能按旧 end 重加只读（会保护到新映射之外）。
    // 报红并走「删除 + 重新登记」，由 add_range 按新长度重新分配页状态（维护方审 11-04 第 8 条）。
    const uintptr_t nbase = (uintptr_t)mem & ~(g_pg - 1);
    const uintptr_t nend = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    if (nbase != r->base || nend != r->end) {
        LOG("【红】 dirty-page tracking: replace_range %p size=%zu 与登记区间 [%p,%p) 不同，删除并重新登记（不按旧 end 加保护）",
            mem, size, (void *)r->base, (void *)r->end);
        table_write_end();
        pthread_mutex_unlock(&g_slot_mu);
        nvn_dirty_remove_range(mem);
        nvn_dirty_add_range(mem, size);
        atomic_fetch_add(&g_mapping_gen, 1);
        return 1;
    }
    uintptr_t np = (r->end - r->base) >> g_shift;
    // 换底后内容可能整体变了：把整段标记为“在新代际上写过”，这样任何换底前取的 stamp
    // 比较都会得到“脏”，缓存必须重新上传。归零会让人误判成“从没写过”（测试抓到的正是这点）。
    const uint64_t newg = atomic_fetch_add(&g_now, 1) + 1;
    { dirty_log *lg = atomic_load_explicit(&r->log, memory_order_acquire); if (lg) atomic_store(&lg->bad, 1); }   // 换底不走解保护，日志里没有这次「整段变了」：作废
    for (uintptr_t k = 0; k < np; k++) { atomic_store(&r->state[k], PG_NONE); atomic_store(&r->gen[k], newg); }
    last_max(r, newg);
    atomic_fetch_add(&g_mapping_gen, 1);
    for (uintptr_t k = 0; k < np; ) {
        uintptr_t first = chunk_first_page(r, k), last = chunk_last_page(r, k);
        if (last >= np) last = np - 1;
        if (first >= np) break;
        if (mp(r->base + (first << g_shift), (last - first + 1) << g_shift, PROT_READ) == 0) {
            for (uintptr_t j = first; j <= last; j++) atomic_store(&r->state[j], PG_PROT);
        } else {
            atomic_store(&g_track_unreliable, 1);
            break;
        }
        k = last + 1;
    }
    table_write_end();
    pthread_mutex_unlock(&g_slot_mu);
    return 1;
}
uint64_t nvn_dirty_mapping_generation(void) { return atomic_load(&g_mapping_gen); }

// 远端别名区判定（宿主填 runtime/src/far.c s2a_far_has，graphics/nvn/host_adapt.m；没填 = 没有远端，照常登记）。函数指针而不是弱引用：只链本文件的单测照常链得上
// 弱链接[nvntest]：s2a_far_has（host_adapt.m 填上面的指针时取它；NVN 单测链 libnvn 不链原生层，按这行放行未定义）
int (*nvn_dirty_far_fn)(uint64_t a, uint64_t n);
static _Atomic int g_far_skip_logged;
void nvn_dirty_add_range(const void *mem, size_t size) {
    // 远端别名区里的内存池不登记（11-14 D-GCMY：窗口 2048 MB、内存池全落在远端）。远端的游戏地址在宿主上没有映射（iPhone）或是护栏页（Mac），
    //   内容在影子里：对游戏地址加只读保护，iPhone 上回 ENOMEM，一次失败就把全局「跟踪不可靠」置位，此后所有打包纹理一律当脏重传；
    //   Mac 上反而会成功，把护栏页改成可读，游戏读到护栏里的 0 而不是影子。远端的写还走缺页代执行、改写器直达影子、SDK 内存函数快路径，
    //   缺页跟踪本来也看不全。所以不登记：这些池的纹理查不到跟踪条目，按「可能写过」在用到时重传（只影响远端池，不连累别的）
    int (*far)(uint64_t, uint64_t) = nvn_dirty_far_fn;
    if (far && size && (far((uint64_t)(uintptr_t)mem, 1) || far((uint64_t)(uintptr_t)mem + size - 1, 1))) {
        if (!atomic_exchange(&g_far_skip_logged, 1)) LOG("【黄】 dirty-page tracking: memory pool %p +%#zx is in the far alias region (no host mapping): not write-protected; its packaged textures are re-uploaded at use", mem, size);
        return;
    }
    static int inited;
    if (!inited) { inited = 1; g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg); dirty_tick_rate(&g_tb_num, &g_tb_den); }
    if (!atomic_exchange(&g_hooks_on, 1)) { dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write); }   // before any page of this session is protected
    atomic_fetch_add(&g_adds_total, 1);
    pthread_mutex_lock(&g_slot_mu);
    table_write_begin();
    int n = atomic_load(&g_nr);
    int slot = -1;
    for (int i = 0; i < n; i++) if (!live_at(i)) { slot = i; break; }
    if (slot < 0) {
        if (n >= MAX_RANGE) {
            int expected = 0;
            if (atomic_compare_exchange_strong(&g_overflow_logged, &expected, 1)) LOG("【红】 dirty-page tracking ranges are full (%d), pool %p not tracked", MAX_RANGE, mem);
            table_write_end();
            pthread_mutex_unlock(&g_slot_mu); return;
        }
        slot = n;
        if (!atomic_load_explicit(&g_blk[n >> BLK_SHIFT], memory_order_acquire)) {   // published before g_nr counts the slot (the scans read g_nr first)
            range_t *b = calloc(BLK, sizeof *b); if (!b) die("dirty-page tracking range table allocation failed (block %d)", n >> BLK_SHIFT);
            atomic_store_explicit(&g_blk[n >> BLK_SHIFT], b, memory_order_release);
        }
    }
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = rg(slot);
    uintptr_t base = (uintptr_t)mem & ~(g_pg - 1), end = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    size_t np = (end - base) >> g_shift;
    // This newly registered range could share a physical host page with
    // an observed range. End the exclusive observation before publishing it.
    if (end > base && atomic_load(&g_probe_redirect)) probe_restore_span(base, end, PROBE_CENSOR, NVN_DEFER_RANGE_CHANGE);
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen;
    if (r->cap < np) {
        size_t cap = np;
        if (r->cap) {
            if (r->cap > SIZE_MAX / 2) die("dirty-page capacity overflow (%zu pages)", np);
            if (cap < r->cap * 2) cap = r->cap * 2;
        }
        if (cap > SIZE_MAX / sizeof *gen) die("dirty-page table size overflow (%zu pages)", cap);
        state = calloc(cap, sizeof *state); gen = calloc(cap, sizeof *gen);
        if (!state || !gen) die("dirty-page table allocation failed (%zu pages)", cap);
        if (r->cap) {
            retired_arrays *old = malloc(sizeof *old);
            if (!old) die("dirty-page retirement record allocation failed");
            *old = (retired_arrays){r->state, r->gen, g_retired_arrays};
            g_retired_arrays = old;
        }
        r->cap = cap;
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen);
    }
    dirty_log *lg = NULL;
    if (g_log_enabled && end > base) {
        size_t cap = np / 16; if (cap < LOG_CAP_MIN) cap = LOG_CAP_MIN; if (cap > LOG_CAP_MAX) cap = LOG_CAP_MAX;
        const size_t bytes = sizeof *lg + cap * sizeof lg->ev[0];
        if (bytes <= LOG_BYTES_LIMIT - g_log_bytes) lg = calloc(1, bytes);
        if (lg) {
            lg->ident = ++g_log_ident; lg->cap = (uint32_t)cap; atomic_init(&lg->bad, 0); atomic_init(&lg->head, 0);
            lg->next = g_all_logs; g_all_logs = lg; g_log_bytes += bytes;
            // 相邻 / 嵌套的池共用宿主页：缺页回调逐个范围解除，加保护只按找到的那一个范围记状态，日志证明不了这类页的事件完整，双方都作废
            for (int i = 0; i < n; i++) if (i != slot && live_at(i)) {
                range_t *other = rg(i);
                if (base < other->end && other->base < end) {
                    dirty_log *ol = atomic_load_explicit(&other->log, memory_order_acquire);
                    if (ol) atomic_store(&ol->bad, 1);
                    atomic_store(&lg->bad, 1);
                }
            }
        } else LOG("【黄】 dirty-page tracking: 追加日志不可用（保留量用尽），这个池保守使用逐页核对");
    } else if (g_log_enabled) {   // 日志开着时，一个没有日志的新池若与已有日志的池重叠，对方同样证明不了
        for (int i = 0; i < n; i++) if (i != slot && live_at(i)) {
            range_t *other = rg(i);
            dirty_log *ol = atomic_load_explicit(&other->log, memory_order_acquire);
            if (ol && base < other->end && other->base < end) atomic_store(&ol->bad, 1);
        }
    }
    r->base = base; r->end = end; r->mem = (uintptr_t)mem; r->state = state; r->gen = gen; atomic_store(&r->last, 0);   // every gen is 0 here (calloc / memset above)
    atomic_store_explicit(&r->log, lg, memory_order_release);
    atomic_store(&r->probe_epochs, NULL);
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    span_insert(base, end, (uint32_t)slot);
    table_write_end();
    pthread_mutex_unlock(&g_slot_mu);
}
// Pool destruction: first restore still-protected pages to writable (the pool memory may be reused for something else once returned to the game), then mark invalid
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    table_write_begin();
    range_t *r = NULL; int n = atomic_load(&g_nr), slot = -1;
    for (int i = 0; i < n && !r; i++) if (live_at(i) && rg(i)->mem == (uintptr_t)mem) { r = rg(i); slot = i; }   // Match by the pool's exact start: an adjacent pool sharing a page may have the same rounded base
    if (r) {
        prep_write_reason((void *)r->base, r->end - r->base, NVN_DEFER_RANGE_DROP);
        atomic_store_explicit(&r->live, 0, memory_order_release);
        atomic_fetch_sub(&g_live, 1);
        span_remove(r->base, (uint32_t)slot);
    }
    table_write_end();
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    return find_span((uintptr_t)addr, len) != NULL;
}
// The tracked range holding [addr, addr+len), as a handle a caller keeps (packaged textures: the scan over every range ran twice per texture lookup). NULL = untracked
const void *nvn_dirty_span(const void *addr, size_t len) { return len ? find_span((uintptr_t)addr, len) : NULL; }
// Set read-only: mark the pages BUSY first and mark them PROT only after mprotect has landed (for the order and the invariant see the state notes at the top of the file)
void nvn_dirty_protect(const void *addr, size_t len) {
    range_t *r = len ? find_span((uintptr_t)addr, len) : NULL;
    if (!r) return;
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    // 按整块加保护：块边界以所属映射为基准，连续块合并成一次 mprotect。
    // 逐页/跨块保护都会在映射里制造新的 VMA，而实测恢复后不合并（32 MiB AHB：1 条 → 65 条）。
    uintptr_t fp = chunk_first_page(r, p), lq = chunk_last_page(r, q);
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = fp; i <= lq + 1; i++) {
        uint8_t st = PG_NONE;
        if (i >= p && i <= q && probe_page(atomic_load(&r->state[i])) &&
            !probe_open_page(r, i, PROBE_CENSOR, NVN_DEFER_REPROTECT))
            die("dirty read probe: failed to restore %p", (void *)(r->base + (i << g_shift)));
        int took = i <= lq && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took && !open) { run = i; open = 1; }
        if (!took && open) {
            // 只有整段成功才记 PG_PROT；失败则记未跟踪并置“跟踪不可靠”，让查询走保守路径。
            if (mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ) == 0) {
                for (uintptr_t k = run; k < i; k++) atomic_store(&r->state[k], PG_PROT);
            } else {
                for (uintptr_t k = run; k < i; k++) atomic_store(&r->state[k], PG_NONE);
                atomic_store(&g_track_unreliable, 1);
            }
            open = 0;
        }
    }
}
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
int nvn_dirty_since_span(const void *span, const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    range_t *r = (range_t *)span; uintptr_t a0 = (uintptr_t)addr;   // the kept handle still holds the span (a slot reused by another pool over the same memory is as good): no scan
    if (!r || !atomic_load_explicit(&r->live, memory_order_acquire) || a0 < r->base || a0 + len > r->end) r = find_span(a0, len);
    if (!r) return 1;   // When untracked, conservatively treat as dirty: better to upload more than to show stale content
    if (atomic_load(&g_track_unreliable)) return 1;   // 有保护失败过：一律视为脏，不靠旧代际比较
    if (atomic_load(&r->last) <= stamp) return 0;   // nothing in the whole range written after the stamp (马车世界缺口 ①：稀疏贴图每次提交按物理段查，段多的池不再逐页扫)
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    for (uintptr_t i = p; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) return 1;   // The generation of the last write before upload is <= the stamp; any later write must be > the stamp
    return 0;
}
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) { return nvn_dirty_since_span(NULL, addr, len, stamp); }
// The write generation of each page of [addr, addr+len) (gens[k] = page k counted from the one holding addr; 0 = not written since tracking began); returns the page count, -1 = untracked / over n
long nvn_dirty_page_gens(const void *span, const void *addr, size_t len, uint64_t *gens, size_t n) {
    if (!len) return 0;
    range_t *r = (range_t *)span; uintptr_t a = (uintptr_t)addr;
    if (!r || !atomic_load_explicit(&r->live, memory_order_acquire) || a < r->base || a + len > r->end) r = find_span(a, len);
    if (!r) return -1;
    if (atomic_load(&g_track_unreliable)) return -1;   // 保守：调用方按“整张需要上传”处理
    uintptr_t p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    if (q - p + 1 > n) return -1;
    for (uintptr_t i = p; i <= q; i++) gens[i - p] = atomic_load(&r->gen[i]);
    return (long)(q - p + 1);
}
size_t nvn_dirty_page_size(void) { return g_pg ? (size_t)g_pg : (size_t)getpagesize(); }
// Which pages of [addr, addr+len) were written after stamp, page by page as nvn_dirty_since: bit k of bits = the k-th host page counted from the one holding addr.
// Returns how many pages the span covers; -1 = untracked (as nvn_dirty_since: count it all as written) or more pages than nbits
long nvn_dirty_written_pages(const void *span, const void *addr, size_t len, uint64_t stamp, uint64_t *bits, size_t nbits) {
    if (!len) return 0;
    range_t *r = (range_t *)span; uintptr_t a = (uintptr_t)addr;
    if (!r || !atomic_load_explicit(&r->live, memory_order_acquire) || a < r->base || a + len > r->end) r = find_span(a, len);
    if (!r) return -1;
    // 跟踪不可靠（曾发生 mprotect 失败）：位图不再可信，返回“未知”，调用方必须整张上传。
    if (atomic_load(&g_track_unreliable)) return -1;
    uintptr_t p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift; size_t n = q - p + 1;
    if (n > nbits) return -1;
    memset(bits, 0, (n + 63) / 64 * sizeof *bits);
    for (uintptr_t i = p; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) bits[(i - p) >> 6] |= 1ull << ((i - p) & 63);
    return (long)n;
}
// ---- 追加日志的读面（调用方：回放核心 rp_buf.c 的池镜像，持自己的锁；不进信号路径）----
static dirty_log *log_usable(const void *span) {
    range_t *r = (range_t *)span;
    if (!r || !atomic_load_explicit(&r->live, memory_order_acquire) || atomic_load(&g_track_unreliable)) return NULL;
    dirty_log *lg = atomic_load_explicit(&r->log, memory_order_acquire);
    return lg && !atomic_load(&lg->bad) ? lg : NULL;
}
int nvn_dirty_log_open(const void *span, uint64_t *ident, uint64_t *cursor) {
    dirty_log *lg = log_usable(span);
    if (!lg || !ident || !cursor) return 0;
    *ident = lg->ident; *cursor = atomic_load_explicit(&lg->head, memory_order_acquire);
    return 1;
}
int nvn_dirty_log_read(const void *span, uint64_t ident, uint64_t *cursor, void (*fn)(void *user, uintptr_t a, uintptr_t b), void *user) {
    dirty_log *lg = log_usable(span);
    if (!lg || lg->ident != ident || !cursor) return 0;
    const uint64_t head = atomic_load_explicit(&lg->head, memory_order_acquire), from = *cursor;
    if (head < from || head - from > lg->cap) return 0;   // 环绕：游标之后有事件已被覆盖
    uint64_t next = head;   // 读完后游标停在哪：第一条还没发布完的事件（它的页还没变可写，写入不可能发生，下次重读）
    for (uint64_t i = from; i < head; i++) {
        __typeof__(&lg->ev[0]) e = &lg->ev[i % lg->cap];
        const uint64_t s1 = atomic_load_explicit(&e->seq, memory_order_acquire);
        const uintptr_t a = atomic_load_explicit(&e->a, memory_order_relaxed), b = atomic_load_explicit(&e->b, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        const uint64_t s2 = atomic_load_explicit(&e->seq, memory_order_relaxed);
        if (s1 == i + 1 && s2 == i + 1) { fn(user, a, b); continue; }
        if (s1 > i + 1 || s2 > i + 1 || atomic_load_explicit(&lg->head, memory_order_acquire) - i > lg->cap) return 0;   // 已被下一圈覆盖
        if (next == head) next = i;   // 写者取了槽还没发布：后面已发布的照读（重复读无害），游标不越过它
    }
    if (atomic_load(&lg->bad) || atomic_load(&g_track_unreliable)) return 0;   // 读的过程中作废了：这一轮读到的不算
    *cursor = next;
    return 1;
}
int nvn_dirty_page_armed(const void *span, const void *addr) {
    range_t *r = (range_t *)span; uintptr_t a = (uintptr_t)addr;
    if (!r || !atomic_load_explicit(&r->live, memory_order_acquire) || a < r->base || a >= r->end || atomic_load(&g_track_unreliable)) return 0;
    return atomic_load(&r->state[(a - r->base) >> g_shift]) == PG_PROT;
}
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total) {
    int n = atomic_load(&g_nr), active = 0;
    for (int i = 0; i < n; i++) active += live_at(i) != 0;
    *live = active; *used_slots = n; *adds_total = atomic_load(&g_adds_total);
}
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) { *faults = atomic_load(&g_faults); *preps = atomic_load(&g_preps); }
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) {
    uint64_t num = g_tb_num, den = g_tb_den;
    if (!den) dirty_tick_rate(&num, &den);
    *calls = atomic_load(&g_mp_calls); *pages = atomic_load(&g_mp_pages); *ns = atomic_load(&g_mp_ticks) * num / den;
}
