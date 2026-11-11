// AHB 池后备接入（窗口化 + 安全点迁移）。
//
// 设计依据（方案 §2/§3/§4）：
//   - 只在既有 VM 安全点迁移：绘制录制期间绝不改映射；迁移发生在帧边界（渲染线程不在 pass 内），
//     并用正式合同 s2a_track_backing_begin/end 让写入者排空并挡住新的。
//   - 只迁移实际被绘制用到且足够热的窗口（4 MiB 对齐、落在同一个 NVN 池内、单映射），
//     避免把几十 MB 的大池整体换成 AHB（本机实测：整池换到 576 MB 立刻把进程拖死）。
//   - 每个窗口都经资源账本（vk_resid）登记：身份 + 映射代际 + 内容版本 + 在途引用；失败即回滚。
//   - 任何前提不满足（对齐/跨池/跨映射/超预算/无能力）就放弃，绘制层继续走原来的上传路径。
#include "nvn_vk_poolahb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

#include "direct/vk_device.h"
#include "direct/vk_membuf.h"
#include "direct/vk_resid.h"
#include "mtl_vk_adapter.h"
#include "nvn_dirty.h"
#include "poolahb_cover.h"
#include "poolahb_policy.h"
#include "poolahb_alias.h"
#include "runtime/tracking/runtime.h"   // s2a_track_backing_begin/end（两阶段改表协议）

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#include <android/log.h>
#define PA_LOG(...) __android_log_print(ANDROID_LOG_INFO, "S2Arm", __VA_ARGS__)
#else
// 宿主单测/宿主构建：日志照打（stderr）。用空语句的话，只有日志用到的局部变量会被
// -Werror=unused-but-set-variable 判红，宿主根本编不过。
#define PA_LOG(...) do { fputs("[poolahb] ", stderr); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

// 记录器在独立宿主夹具中可缺席；macOS 链接参数从此登记生成。
// 弱链接[poolahb_test]：s2a_record_dispatch_fq_enabled
extern int s2a_record_dispatch_fq_enabled(void) __attribute__((weak));

#define PA_WINDOW (1ull << 20)          // 单个窗口 1 MiB：VB/IB 段通常只有几十~几百 KB，窗口越小利用率越高
// AHB 总预算：实测整池累计 576 MB 会 ART abort（进程直接没了），所以不能只写死一个常数。
// 取 min(320 MiB, 设备内存/16)：小内存机型自动降档，8 GB 以上才用满 320 MiB。
#define PA_TOTAL_CAP_MAX (320ull << 20)
static uint64_t pa_total_cap(void) {
    static uint64_t cap;
    if (cap) return cap;
    cap = PA_TOTAL_CAP_MAX;
#if defined(__ANDROID__)
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            unsigned long long kb = 0;
            if (sscanf(line, "MemTotal: %llu kB", &kb) == 1 && kb) {
                uint64_t by_ram = (uint64_t)kb * 1024ull / 16ull;
                if (by_ram < cap) cap = by_ram;
                break;
            }
        }
        fclose(f);
    }
#endif
    if (cap < (64ull << 20)) cap = (64ull << 20);   // 至少够一个池，否则干脆别迁
    return cap;
}
#define PA_MAX 160                      // 最多同时存在多少个已迁移窗口（账本上限 256）
#define PA_HOT_HITS 4u                  // 一帧里被引用这么多次才值得迁
#define PA_PAGE 4096ull
#define PA_POOL_MAX (64ull << 20)   // 单池上限：本机池普遍 64 MiB；整池换 8 个以上会拖死进程，靠预算限制

struct pa_live {           // 已迁移窗口
    uint64_t addr, len, backing_id;
    int retired;           // 池已删除：等完成证明后才能销毁
    VkBuffer buf;
    struct s2a_gpu_buf gb;
    VkBufferUsageFlags usage;   // 后备创建时的 usage（compute 直绑按它判 UNIFORM/STORAGE 可绑）
    int live;
};

// 按池累计引用字节：每帧 12.6 MB 拷贝分散在 8.4k 笔小额引用上（VB 平均 838B、IB 157B、UBO 142B），
// 逐段迁移覆盖率上不去；按“池”整体迁移才能一次覆盖大量引用（iOS 就是整池零拷贝）。
struct pa_pool_stat { uint64_t base, len, bytes; unsigned refs; int used; int done; int failed; };
#define PA_POOLS 64
static struct pa_pool_stat g_pa_pool[PA_POOLS];
// 待迁移槽数（used && !done && !failed）。owner 窗口先用它**无锁**判断「到底有没有活要干」：
// 加载期 0x2C 增页很密，每次都为不存在的迁移去停世界会把整条快路径抹掉（复核必改第 2 条）。
// 只在持 PA_GUARD 的地方写，读侧不加锁。
static int g_pa_pending_pools;
static void pa_pool_note_new(void) { __atomic_fetch_add(&g_pa_pending_pools, 1, __ATOMIC_RELAXED); }
static void pa_pool_note_settled(struct pa_pool_stat *ps, int failed) {
    if (ps->done || ps->failed) return;   // 已结算过：不重复扣
    ps->done = failed ? 0 : 1;
    ps->failed = failed;
    __atomic_fetch_sub(&g_pa_pending_pools, 1, __ATOMIC_RELAXED);
}
// 池被删除：已迁的（done）也要改标失败——之后的新命中一律不命中，不能再走零拷贝。
// 与 pa_pool_note_settled 的区别是它允许 done → failed 的迁移。
static void pa_pool_note_removed(struct pa_pool_stat *ps) {
    if (ps->failed) return;
    if (!ps->done) __atomic_fetch_sub(&g_pa_pending_pools, 1, __ATOMIC_RELAXED);
    ps->done = 0;
    ps->failed = 1;
}
static unsigned pa_pending_pools(void) { return (unsigned)__atomic_load_n(&g_pa_pending_pools, __ATOMIC_RELAXED); }


// 迁移前的旧内容备份：回滚要能恢复字节（否则只能“调用了一次回滚”却仍画错）。
// 只在事务窗口内存在，成功发布或回滚完成后立即释放。
struct pa_backup { uint64_t addr, len; void *data; int used; };
static struct pa_backup g_pa_backup[4];

static struct pa_backup *pa_backup_slot(uint64_t addr) {
    for (int i = 0; i < 4; i++) if (g_pa_backup[i].used && g_pa_backup[i].addr == addr) return &g_pa_backup[i];
    return NULL;
}
static struct pa_backup *pa_backup_new(uint64_t addr, uint64_t len) {
    for (int i = 0; i < 4; i++) {
        if (g_pa_backup[i].used) continue;
        void *m = malloc((size_t)len);
        if (!m) return NULL;
        g_pa_backup[i].addr = addr; g_pa_backup[i].len = len; g_pa_backup[i].data = m; g_pa_backup[i].used = 1;
        return &g_pa_backup[i];
    }
    return NULL;
}
static void pa_backup_free(struct pa_backup *b) {
    if (!b) return;
    free(b->data);
    memset(b, 0, sizeof *b);
}

static struct s2a_vk_device *g_pa_dev;
// 已迁移窗口：[0, g_pa_live_n) 按 addr 升序、无洞。删除时压缩，插入时从末尾左移——
// 以前用“最低空槽 + 只左移”的插入，回收低槽后新窗口地址更大时数组不再有序，二分查找会漏命中。
static struct pa_live g_pa_live[PA_MAX];
static int g_pa_live_n;
// 迁移事务里的新条目：成功后按 addr 插入有序数组，失败由 release 回收（不在数组里，不参与查找）。
static struct pa_live g_pa_pending;
static int g_pa_pending_used;
// 回滚也失败（旧映射无法恢复）时置位：合同要求终止，不能继续画/继续迁移。
static int g_pa_fatal;
// g_pa_live / g_pa_pool / 统计被三处并发访问：绘制线程（lookup、每帧几百次）、guest 线程（establish /
// note_write / pool_removed）、VM owner 窗口（service 迁移）。停 guest writers 不覆盖绘制线程，
// 所以数组插入与二分查找必须自己加锁（复核：另一线程 draw 二分查 g_pa_live 时这边正在插入）。
// 顺序固定 pa → resid（lookup/service 会在持锁期间调 resid），resid 从不回调本层。
static pthread_mutex_t g_pa_mu = PTHREAD_MUTEX_INITIALIZER;
static void pa_mu_unlock(pthread_mutex_t **m) { if (m && *m) pthread_mutex_unlock(*m); }
#define PA_GUARD() \
    __attribute__((cleanup(pa_mu_unlock))) pthread_mutex_t *pa_mu = &g_pa_mu; \
    pthread_mutex_lock(pa_mu); \
    (void)pa_mu
static struct s2a_poolahb_stats g_pa_stats;
static int g_pa_on = -1;
static uint64_t g_pa_seq;            // 当前提交序号（单调递增，绝不回绕成两帧同号；初值 0 表示还没有提交）
static uint64_t g_pa_seq_next = 1;   // 下一个提交序号
static uint64_t g_pa_next_backing = 1;
static uint64_t g_pa_bytes;
static int g_pa_release_pending;
// 迁移的一次性成本：单窗口耗时 + 本次进程内累计（供“整池换底要多久”的读数，只加日志不改行为）。
static uint64_t pa_now_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
static uint64_t g_pa_migrate_us;
static unsigned g_pa_migrate_windows;
// 迁移失败原因（诊断）：覆盖不足到底是“尺寸/跨映射/AHB/事务”哪一类。
static unsigned g_fail_big, g_fail_multi, g_fail_ahb, g_fail_tx, g_fail_alias;
static int g_pa_cand_map[PA_POOLS];   // 候选压缩后的下标 → g_pa_pool 下标

int s2a_poolahb_enabled(void) {
    if (g_pa_on < 0) {
#if defined(__ANDROID__)
        // 默认开（直连侧的通用能力适配，所有游戏共用）；显式 debug.s2a.pool_ahb=0 可关。
        // 设备不支持 AHB 时 s2a_poolahb_init 会按能力关掉，绘制层继续走正确的上传路径。
        char b[PROP_VALUE_MAX] = {0};
        if (__system_property_get("debug.s2a.pool_ahb", b) > 0) g_pa_on = (b[0] == '0') ? 0 : 1;
        else g_pa_on = 1;
#else
        g_pa_on = 0;
#endif
    }
    return g_pa_on;
}

// 这段地址是否被**连续**的可读写映射完整覆盖（可以是几段相邻映射）。
// 64 MiB 的池在 /proc/self/maps 里通常是多段相邻 rw 映射，要求“单条覆盖”会把大多数池误拒
// （实测 5 个热池全被判 failed，覆盖上不去）。
// 注意：这只能证明“地址连续可写”，不能证明没有别的地址指向同一后备；别名安全靠的是调用点——
// 迁移只在 svc_mem 的 owner 窗口里做（外层已持独占 writer epoch、别名记账由原生层负责）。
// 映射表来源：生产（安卓 / Linux）就是 procfs。宿主单测在没有 /proc 的平台（macOS 门禁机）上给强符号，
//   按真实虚拟内存区域生成同格式的表（见 tests/vk_poolahb_core_test.c）；生产二进制里没人覆盖它
__attribute__((weak)) const char *s2a_poolahb_maps_path(void) { return "/proc/self/maps"; }
// 诊断：把与 [base,base+len) 相交的映射段按顺序打出来（为什么判定“不连续”）。
static void pa_dump_maps(uint64_t base, uint64_t len) {
    FILE *f = fopen(s2a_poolahb_maps_path(), "r");
    if (!f) return;
    char line[512];
    int shown = 0;
    while (fgets(line, sizeof line, f) && shown < 10) {
        unsigned long lo = 0, hi = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
        if ((uint64_t)hi <= base || (uint64_t)lo >= base + len) continue;
        { char *nl = strchr(line, '\n'); if (nl) *nl = 0; PA_LOG("poolahb maps: %s", line); }
        shown++;
    }
    fclose(f);
}

// 覆盖判定在 poolahb_cover.c（宿主可测）：不要求每段都 rw，只要连续匿名映射完整覆盖。
// 返回 0 = 可迁移；1 = 有受控别名；2 = 地址覆盖不足。
static int pa_migratable(uint64_t base, uint64_t len) {
    if (s2a_alias_conflict(base, len)) return 1;
    if (!s2a_poolahb_cover_check_path(s2a_poolahb_maps_path(), base, len)) return 2;
    return 0;
}

// 按 addr 升序插入（数量很小，插入排序足够）。只在迁移成功后调用一次。
static void pa_live_insert(const struct pa_live *e) {
    if (g_pa_live_n >= PA_MAX || !e) return;
    int pos = g_pa_live_n;
    while (pos > 0 && g_pa_live[pos - 1].addr > e->addr) {
        g_pa_live[pos] = g_pa_live[pos - 1];
        pos--;
    }
    g_pa_live[pos] = *e;
    g_pa_live_n++;
}

// 删除第 idx 条并压缩，保持 [0, g_pa_live_n) 无洞、按 addr 升序（二分查找的前提）。
static void pa_live_remove(int idx) {
    if (idx < 0 || idx >= g_pa_live_n) return;
    for (int i = idx; i < g_pa_live_n - 1; i++) g_pa_live[i] = g_pa_live[i + 1];
    g_pa_live_n--;
    memset(&g_pa_live[g_pa_live_n], 0, sizeof g_pa_live[0]);
}

// 热路径查找：二分。这里**不**查账本——账本是迁移/回滚/退休时的权威，
// 每笔 draw 都扫 256 项会把省下的拷贝又花回去（实测 exec 12.2 → 12.8）。
static struct pa_live *pa_live_find(uint64_t addr, uint64_t len) {
    int lo = 0, hi = g_pa_live_n - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_pa_live[mid].addr <= addr) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return NULL;
    struct pa_live *e = &g_pa_live[best];
    if (e->retired) return NULL;
    if (addr - e->addr > e->len || len > e->len - (addr - e->addr)) return NULL;
    return e;
}

static struct pa_live *pa_by_backing(uint64_t id) {
    if (g_pa_pending_used && g_pa_pending.backing_id == id) return &g_pa_pending;
    for (int i = 0; i < g_pa_live_n; i++)
        if (g_pa_live[i].backing_id == id) return &g_pa_live[i];
    return NULL;
}

static int pa_track_begin(void *ctx, uint64_t addr, uint64_t size, uint64_t source) {
    (void)ctx;
    return s2a_track_backing_begin(addr, size, source);
}
static void pa_track_end(void *ctx, uint64_t addr, uint64_t size, int mapped, int committed) {
    (void)ctx;
    s2a_track_backing_end(addr, size, mapped, committed);
}
// ---- 迁移前 VMA 预算 ---------------------------------------------------------------
// 真机实测：整块 AHB 映射被细粒度页保护切碎后不合并（32 MiB：1 条 → 65 条），世界里 VMA 会
// 一路涨到 vm.max_map_count。分块保护把碎片上界压到“块数”，但仍要在**迁移前**按最坏情况预留，
// 并给其他模块留余量；超预算就拒绝新增迁移（保留原后备），而不是等 mmap 失败再崩。
static uint64_t g_pa_vma_est;          // 估算的当前 VMA 数（0 = 还没测过）
static _Atomic int g_pa_vma_refused;

static uint64_t pa_vma_budget(void) {
    static uint64_t v;
    if (v) return v;
    v = 48000ull;   // 65530 之下给其他模块留 17k
#if defined(__ANDROID__)
    {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.vma_budget", b);
        if (b[0] && atoi(b) > 0) v = (uint64_t)atoi(b);
    }
#endif
    return v;
}
static uint64_t pa_vma_measure(void) {
    FILE *m = fopen("/proc/self/maps", "r");
    if (!m) return 0;
    char buf[256];
    uint64_t n = 0;
    while (fgets(buf, sizeof buf, m)) n++;
    fclose(m);
    return n;
}
// 一个迁移窗口最坏会新增多少条 VMA：分块数（每块可能被单独切成一条）+ 别名数量。
static uint64_t pa_vma_worst(uint64_t len, uint64_t aliases) {
    const uint64_t pg = (uint64_t)sysconf(_SC_PAGESIZE);
    // 必须与脏页保护实际使用的分块同口径（debug.s2a.chunk_kb 可调），否则预算与真实碎片不符。
    // 共享默认 0 = 逐页（Apple 路径），按 1 页算；安卓直连在第一次上传前已设 1 MiB。
    // 不用 weak 声明：Mach-O 上块作用域 weak 不生效、链接会报 Undefined symbols（维护方 Mac 实跑）。
    uint32_t cp = nvn_dirty_chunk_pages();
    if (!cp) cp = 1u;
    const uint64_t chunk = (pg ? pg : 4096) * cp;
    const uint64_t chunks = (len + chunk - 1) / chunk;
    return chunks + (aliases ? aliases : 1) + 2;
}
static int pa_vma_budget_ok(uint64_t len) {
    const uint64_t worst = pa_vma_worst(len, PA_MAX);
    const uint64_t limit = pa_vma_budget();
    if (!g_pa_vma_est) g_pa_vma_est = pa_vma_measure();
    if (g_pa_vma_est + worst <= limit) { g_pa_vma_est += worst; return 1; }
    // 估算逼近上限：重测一次真实值再判（迁移不频繁，这一次读 maps 值得）
    const uint64_t real = pa_vma_measure();
    if (real) g_pa_vma_est = real;
    if (g_pa_vma_est + worst <= limit) { g_pa_vma_est += worst; return 1; }
    if (!atomic_exchange(&g_pa_vma_refused, 1))
        PA_LOG("【红】 poolahb: VMA 预算不足，拒绝新增迁移（当前≈%llu + 最坏 %llu > 上限 %llu）；保留原后备",
               (unsigned long long)g_pa_vma_est, (unsigned long long)worst, (unsigned long long)limit);
    return 0;
}

static int pa_prepare(void *ctx, uint64_t len, uint64_t *out_backing) {
    (void)ctx;
    if (g_pa_pending_used || g_pa_live_n >= PA_MAX) return -1;
    if (!pa_vma_budget_ok(len)) return -1;   // 预算不足：不迁移，调用方保留原后备
    char why[160] = "";
    memset(&g_pa_pending, 0, sizeof g_pa_pending);
    // 默认关时保持原有创建掩码；开门后只试一次带 STORAGE 的后备，失败再按旧掩码创建。
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (s2a_record_dispatch_fq_enabled && s2a_record_dispatch_fq_enabled())
        usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (s2a_gpu_buf_create(g_pa_dev, len, usage, S2A_GPU_MEM_AHB, &g_pa_pending.gb,
                           why, sizeof why) != 0) {
        if (!(usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
            PA_LOG("poolahb: AHB create/import failed: %s", why);
            g_fail_ahb++;
            return -1;
        }
        PA_LOG("poolahb: AHB create/import with storage usage failed, retrying legacy mask: %s", why);
        why[0] = 0;
        usage &= ~VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (s2a_gpu_buf_create(g_pa_dev, len, usage, S2A_GPU_MEM_AHB, &g_pa_pending.gb,
                               why, sizeof why) != 0) {
            PA_LOG("poolahb: AHB create/import failed: %s", why);
            g_fail_ahb++;
            return -1;
        }
    }
    g_pa_pending.usage = usage;
    g_pa_pending.backing_id = g_pa_next_backing++;
    g_pa_pending.buf = g_pa_pending.gb.buf;
    g_pa_pending.live = 1;
    g_pa_pending_used = 1;
    *out_backing = g_pa_pending.backing_id;
    return 0;
}
static int pa_copy(void *ctx, uint64_t addr, uint64_t backing, uint64_t len) {
    (void)ctx;
    struct pa_live *e = pa_by_backing(backing);
    if (!e || !e->gb.cpu) return -1;
    memcpy(e->gb.cpu, (const void *)(uintptr_t)addr, (size_t)len);   // 旧内容搬进新后备
    return 0;
}
static int pa_no_remap(void) {
    static int v = -1;
    if (v < 0) {
#if defined(__ANDROID__)
        char b[PROP_VALUE_MAX] = {0};
        v = (__system_property_get("debug.s2a.pool_no_remap", b) > 0 && b[0] == '1') ? 1 : 0;
#else
        v = 0;
#endif
    }
    return v;
}
static int pa_remap(void *ctx, uint64_t addr, uint64_t len, uint64_t backing) {
    (void)ctx;
    // 诊断：只做 AHB 分配+导入，不替换 guest 映射——用来把“AHB 分配/导入的开销”与“换页本身”分开。
    if (pa_no_remap()) return 0;
    struct pa_live *e = pa_by_backing(backing);
    if (!e) return -1;
    // 换底前把旧内容整段备份（回滚要恢复字节；MAP_FIXED 之后原匿名页就没了）。
    struct pa_backup *bk = pa_backup_new(addr, len);
    if (!bk) { PA_LOG("poolahb: backup alloc failed for %#llx+%#llx", (unsigned long long)addr, (unsigned long long)len); return -1; }
    memcpy(bk->data, (const void *)(uintptr_t)addr, (size_t)len);
    void *mapped = NULL;
    char why[160] = "";
    if (s2a_gpu_buf_map_at(&e->gb, (void *)(uintptr_t)addr, (size_t)len, &mapped, why, sizeof why) != 0) {
        PA_LOG("poolahb: alias map failed: %s", why);
        return -1;
    }
    return 0;
}
static int pa_no_dirty(void) {
    static int v = -1;
    if (v < 0) {
#if defined(__ANDROID__)
        char b[PROP_VALUE_MAX] = {0};
        v = (__system_property_get("debug.s2a.pool_no_dirty", b) > 0 && b[0] == '1') ? 1 : 0;
#else
        v = 0;
#endif
    }
    return v;
}
static int pa_rebuild(void *ctx, uint64_t addr, uint64_t len) {
    (void)ctx;
    // 换页丢掉页记账/保护：重新登记，让脏页跟踪与后续纹理消费者继续工作。
    // 诊断开关：debug.s2a.pool_no_dirty=1 跳过这一步，用来隔离“重新登记脏页跟踪”本身的代价。
    if (pa_no_dirty()) return 0;
    // 换底必须“替换”旧跟踪状态：新映射实际可读写，若沿用旧条目的“已保护”，写入不会触发异常，
    // 代际不变 → 纹理缓存误判内容没变（花屏/噪点）。replace 会重置状态、发布新映射代际并按分块重新加保护。
    if (!nvn_dirty_replace_range((const void *)(uintptr_t)addr, (size_t)len))
        nvn_dirty_add_range((const void *)(uintptr_t)addr, (size_t)len);
    return 0;
}
static int pa_publish(void *ctx, uint64_t addr, uint64_t len) {
    (void)ctx; (void)len;
    pa_backup_free(pa_backup_slot(addr));   // 提交成功：备份不再需要
    return 0;
}
static int pa_rollback(void *ctx, uint64_t addr, uint64_t len) {
    (void)ctx;
    struct pa_backup *bk = pa_backup_slot(addr);
    if (!bk || bk->len != len) return -1;   // 没有可恢复的备份：不能假装回滚成功
    // 先把**原 guest 背板映射**恢复回来，再写回迁移前字节。
    // main 11-02 起 guest RAM 是单一 memfd:s2a-guest（按 guest 地址当 pgoff 映射）；退回匿名页会让这段
    // 地址脱离背板——后续 VM 的打洞/重映射看到的是 memfd 旧内容，guest 之后写进匿名页的字节会丢。
    // 没有 backing.map 的构建（宿主微例 / 旧平台）才退回匿名页 + 恢复字节。
    extern int plat_backing_map(void *addr, size_t size, uint32_t plat_prot, uint64_t pgoff)
        __attribute__((weak));
    int restored = 0;
    if (plat_backing_map) {
        // PLAT_PROT_READ | PLAT_PROT_WRITE == 3（platform/plat.h），pgoff 与 guest 地址同值。
        if (plat_backing_map((void *)(uintptr_t)addr, (size_t)len, 3u, addr) == 0) {
            restored = 1;
        } else {
            PA_LOG("poolahb: rollback: restoring the guest backing mapping failed at %#llx",
                   (unsigned long long)addr);
        }
    }
    if (!restored) {
        void *m = mmap((void *)(uintptr_t)addr, (size_t)len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (m == MAP_FAILED || m != (void *)(uintptr_t)addr) {
            PA_LOG("poolahb: rollback remap failed at %#llx (errno=%d)", (unsigned long long)addr, errno);
            return -1;
        }
    }
    // 两条路径都写回迁移前字节（背板恢复时内容本就相同，这一步只保证“回滚后字节一定等于迁移前”）。
    memcpy((void *)(uintptr_t)addr, bk->data, (size_t)len);
    // 重建页记账/保护：共享 add_range 不再隐式转向 replace，这里显式按「先替换、没有条目再登记」写。
    if (!nvn_dirty_replace_range((const void *)(uintptr_t)addr, (size_t)len))
        nvn_dirty_add_range((const void *)(uintptr_t)addr, (size_t)len);
    pa_backup_free(bk);
    return 0;
}
static void pa_release(void *ctx, uint64_t backing) {
    (void)ctx;
    if (g_pa_pending_used && g_pa_pending.backing_id == backing) {
        s2a_gpu_buf_destroy(g_pa_dev, &g_pa_pending.gb);
        memset(&g_pa_pending, 0, sizeof g_pa_pending);
        g_pa_pending_used = 0;
        return;
    }
    for (int i = 0; i < g_pa_live_n; i++) {
        if (g_pa_live[i].backing_id != backing) continue;
        s2a_gpu_buf_destroy(g_pa_dev, &g_pa_live[i].gb);
        pa_live_remove(i);
        return;
    }
}

static const struct s2a_res_ops g_pa_ops = {
    NULL, pa_track_begin, pa_track_end, pa_prepare, pa_copy, pa_remap, pa_rebuild, pa_publish,
    pa_rollback, pa_release,
};

static int pa_migrate_window(uint64_t addr, uint64_t len);
static void pa_reap_pending(void);

int s2a_poolahb_init(struct s2a_vk_device *dev) {
    PA_GUARD();
    g_pa_dev = dev;
    memset(g_pa_live, 0, sizeof g_pa_live);
    g_pa_live_n = 0;
    memset(&g_pa_pending, 0, sizeof g_pa_pending);
    g_pa_pending_used = 0;
    memset(g_pa_pool, 0, sizeof g_pa_pool);
    __atomic_store_n(&g_pa_pending_pools, 0, __ATOMIC_RELAXED);
    g_pa_release_pending = 0;
    g_fail_big = g_fail_multi = g_fail_ahb = g_fail_tx = g_fail_alias = 0;
    g_pa_fatal = 0;
    memset(&g_pa_stats, 0, sizeof g_pa_stats);
    g_pa_bytes = 0;
    g_pa_migrate_us = 0;
    g_pa_migrate_windows = 0;
    if (!s2a_poolahb_enabled()) return -1;
    const struct s2a_gpu_mem_caps *caps = s2a_gpu_mem_caps_get();
    static struct s2a_gpu_mem_caps local;
    if (!caps) {
        char wy[160] = "";
        if (s2a_gpu_mem_probe(dev, &local, wy, sizeof wy) != 0) return -1;
        caps = s2a_gpu_mem_caps_get();
    }
    if (!caps || !caps->ahb || !dev->ahb_import_enabled) {
        PA_LOG("poolahb: disabled (ahb=%d import_enabled=%d)", caps ? caps->ahb : 0, dev->ahb_import_enabled);
        g_pa_on = 0;
        return -1;
    }
    PA_LOG("poolahb: enabled (pool max=%llu MiB, total cap=%llu MiB(按设备内存自适应), migrate in VM owner window)",
           (unsigned long long)(PA_POOL_MAX >> 20), (unsigned long long)(pa_total_cap() >> 20));
    return 0;
}

// 池建立点（guest 线程、NVN 调用内，与 svc_mem 改表事务同一上下文）：符合前提就整池换 AHB。
// 只换小池（≤ PA_POOL_MAX）：整池换几十 MB 会瞬间吃掉 GPU 内存（本机实测把进程拖死）；
// 渲染线程（帧边界）不发起任何改表事务——那会和 present 互相等，实测 present 卡到 600 ms。
// 池建立点（guest 线程、NVN 调用内，不在 present/exec 路径上）：把**已积累够热的窗口**迁走。
// 不迁“刚注册的池”本身：池注册顺序与用途无关，实测按注册顺序整池换 95 MB 后绘制命中=0，
// 白占 GPU 内存。真正要迁的是绘制层实际引用最多的那些窗口。
int s2a_poolahb_establish(uint64_t addr, uint64_t len) {
    PA_GUARD();
    if (!s2a_poolahb_enabled() || !g_pa_dev || !addr || !len) return -1;
    if (addr % PA_PAGE || len % PA_PAGE) { g_pa_stats.refused++; return -1; }
    // 只登记“这里是一段受管内存”的事实；迁移必须由 VM/thread owner 在证明 writers 已停稳后发起，
    // 池建立点只是“存在候选”，不是安全点。
    return 0;
}

static int pa_migrate_window(uint64_t addr, uint64_t len) {
    if (g_pa_bytes + len > pa_total_cap()) { g_pa_stats.refused++; return -1; }
    uint64_t t0 = pa_now_us();
    uint64_t gen = 0;
    int rc = s2a_res_register(addr, len, addr, 0, &gen);
    if (rc != S2A_RES_OK) { g_pa_stats.refused++; PA_LOG("poolahb: register %#llx+%#llx rc=%d", (unsigned long long)addr, (unsigned long long)len, rc); return -1; }
    rc = s2a_res_migrate(&g_pa_ops, addr, len, 0, &gen);
    if (rc != S2A_RES_OK) {
        s2a_res_retire(addr, len);
        g_pa_stats.refused++;
        if (rc == S2A_RES_EFATAL) {
            // 合同：连回滚都失败（旧映射恢复不了）时必须终止，不能继续画。AHB 后备仍映射在 guest 地址上，
            // 不能销毁（销毁会留下悬空映射），这里只把它从 pending 槽摘掉并整体停止迁移；
            // 进程应在下一笔录制失败时报红退出。
            g_pa_fatal = 1;
            if (g_pa_pending_used) { memset(&g_pa_pending, 0, sizeof g_pa_pending); g_pa_pending_used = 0; }
            for (int i = 0; i < PA_POOLS; i++) if (g_pa_pool[i].used) pa_pool_note_settled(&g_pa_pool[i], 1);
            PA_LOG("poolahb: FATAL: rollback could not restore the guest mapping at %#llx; migration stopped (terminate required)",
                   (unsigned long long)addr);
            return -1;
        }
        PA_LOG("poolahb: migrate %#llx+%#llx refused rc=%d", (unsigned long long)addr, (unsigned long long)len, rc);
        return -1;
    }
    // 迁移成功：回填地址并按 addr 插入有序数组（之前漏了回填，Finalize 永远匹配不到）。
    if (g_pa_pending_used) {
        g_pa_pending.addr = addr;
        g_pa_pending.len = len;
        g_pa_pending.retired = 0;
        pa_live_insert(&g_pa_pending);
        memset(&g_pa_pending, 0, sizeof g_pa_pending);
        g_pa_pending_used = 0;
    }
    g_pa_bytes += len;
    g_pa_stats.established++;
    g_pa_stats.bytes = g_pa_bytes;
    uint64_t took_us = pa_now_us() - t0;
    g_pa_migrate_us += took_us;
    g_pa_migrate_windows++;
    PA_LOG("poolahb: window %#llx+%#llx AHB-backed (gen=%llu total=%llu MB, took=%llu.%03llu ms, migrate_total=%u windows/%llu.%03llu ms)",
           (unsigned long long)addr, (unsigned long long)len, (unsigned long long)gen,
           (unsigned long long)(g_pa_bytes >> 20),
           (unsigned long long)(took_us / 1000), (unsigned long long)(took_us % 1000),
           g_pa_migrate_windows,
           (unsigned long long)(g_pa_migrate_us / 1000), (unsigned long long)(g_pa_migrate_us % 1000));
    return 0;
}

static int pa_lookup(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off,
                     VkBufferUsageFlags *usage) {
    PA_GUARD();
    if (!s2a_poolahb_enabled() || !buf || !off || !len) return -1;
    struct pa_live *e = pa_live_find(addr, len);
    if (e) {
        // 在途保活（对象级、幂等）：失败就不能用零拷贝结果，否则后备可能在 GPU 还在读时被释放。
        if (s2a_res_ref_commit_backing(g_pa_seq, e->backing_id) != S2A_RES_OK) {
            g_pa_stats.miss_ref_full++;
            g_pa_stats.misses++;
            return -1;
        }
        *buf = e->buf;
        *off = addr - e->addr;
        if (usage) *usage = e->usage;
        g_pa_stats.lookups++;
        g_pa_stats.hit_bytes += len;
        return 0;
    }
    g_pa_stats.misses++;
    // miss 原因（互斥、总和 == misses）：地址不在池 → 池未迁 / 池迁失败 / 范围未登记 / 内部表满。
    uint64_t pool_base = 0, pool_size = 0;
    if (s2a_vk_adapter_pool_find(addr, len, &pool_base, &pool_size) != 0) {
        g_pa_stats.miss_no_pool++;
        return -1;
    }
    struct pa_pool_stat *ps = NULL;
    for (int i = 0; i < PA_POOLS; i++)
        if (g_pa_pool[i].used && g_pa_pool[i].base == pool_base) { ps = &g_pa_pool[i]; break; }
    if (!ps) {
        for (int i = 0; i < PA_POOLS; i++) if (!g_pa_pool[i].used) {
            g_pa_pool[i].used = 1; g_pa_pool[i].base = pool_base; g_pa_pool[i].len = pool_size;
            pa_pool_note_new();
            ps = &g_pa_pool[i]; break;
        }
    }
    if (!ps) { g_pa_stats.miss_table_full++; return -1; }
    if (ps->failed) { g_pa_stats.miss_pool_failed++; return -1; }
    if (ps->done) { g_pa_stats.miss_range_unregistered++; return -1; }
    ps->bytes += len;
    ps->refs++;
    g_pa_stats.miss_cold++;
    return -1;
}

int s2a_poolahb_lookup(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off) {
    return pa_lookup(addr, len, buf, off, NULL);
}
int s2a_poolahb_lookup_usage(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off,
                             VkBufferUsageFlags *usage) {
    if (usage) *usage = 0;
    // 诊断禁用 remap 时 guest 和 AHB 不共享字节，不能用作 compute 直绑。
    if (pa_no_remap()) return -1;
    return pa_lookup(addr, len, buf, off, usage);
}

// 迁移服务：只在**本帧 fence 已全部等到**的完成点调用（GPU 已空闲、我们没有任何在途提交）。
// 在提交后立刻调用会和 present/驱动互相等待（实测 present 553 ms），所以服务点必须放在完成点。
// owner 提供的安全证明：只有 VM/thread owner 在“guest writers 已停稳、且受影响 GPU 使用已排空”之后
// 才能置位。没有证明就拒绝迁移——这是合同要求，不是可选项（runtime.h: begin 前须已停 writers）。
static int g_pa_owner_ready;
void s2a_poolahb_owner_note_writers_stopped(int stopped) { g_pa_owner_ready = stopped ? 1 : 0; }

// svc_mem 在“已停稳 guest writers、持独占 writer epoch”的窗口里调用本函数：
// 这是合同要求的真实安全点，热窗口迁移请求在这里消费（每次只迁一个，风险可控）。
// 返回 0 = 没事 / 已迁移；非 0 = 本次窗口的 writer epoch 已被撤销（升级失败），
// 调用方**必须终止这次映射事务**（不能在没有 writer epoch 的情况下改映射）。
int s2a_android_mem_owner_window(void) {
    if (!s2a_poolahb_enabled() || !g_pa_dev) return 0;
    if (g_pa_fatal) return 0;                        // 回滚失败后整体停止迁移（不改映射也不算协调失败）
    // 没有待迁移窗口就直接返回：绝不为了“可能有的活”去停世界。svc 0x2C 增页在加载期很密，
    // 每次都 quiesce 会把原本的快路径整个抹掉（复核必改第 2 条）。
    if (!pa_pending_pools()) return 0;
    // 只有“独占 writer epoch（其他 writer 已停稳）”的窗口才是真安全点。svc 0x2C 全空页增页的快路径
    // 只取**串行** epoch（fault.h:69 写明不停其他 writer），在这里换池后备会把其他线程同时写进池子的
    // 内容丢掉（memcpy→MAP_FIXED 之间的写），draw 线程也会在我们插入 g_pa_live 时二分到错位。
    extern int s2a_android_guest_writers_serial_only(void) __attribute__((weak));
    if (s2a_android_guest_writers_serial_only && s2a_android_guest_writers_serial_only()) {
        // 串行 epoch（0x2C 全空宿主页增页的快路径）不是安全点：别的 writer 还在写。
        // 旧做法是把它升级成全体停稳来腾出安全点，但实测补编压力下 2 s 停世界会到期，
        // 而升级失败会撤销 epoch，把一次普通增页变成“事务中途被撤销”（真机：
        // writer协调到期 原因=stop-world-upgrade → migration refused → 二次收尾 SIGABRT）。
        // 迁移是可选优化：安全点未就绪就**延期**，留到下一个真正的全体停稳窗口再迁，
        // 绝不为可选迁移把普通增页拖进停世界。
        g_pa_owner_ready = 0;   // 不留旧授权：下一次必须重新给出“writers 已停稳”的证明
        static int deferred_serial;
        if (!deferred_serial++) {
            PA_LOG("poolahb: migration deferred (serial-only writer epoch is not a safe point; deferring to the next full stop-world window)");
        }
        return 0;
    }
    s2a_poolahb_owner_note_writers_stopped(1);
    (void)s2a_poolahb_service();
    g_pa_owner_ready = 0;   // 一次性授权：proof 只对本次窗口有效

    // 迁移只迁一个窗口就够（其余留到下一个内存 SVC 窗口）
    return 0;
}

int s2a_poolahb_service(void) {
    PA_GUARD();
    if (!s2a_poolahb_enabled() || !g_pa_dev) return -1;
    if (g_pa_fatal) return -1;   // 回滚失败后不再迁移（见 pa_migrate_window 的 EIO 分支）
    if (!g_pa_owner_ready) {
        static int warned;
        if (!warned) { warned = 1; PA_LOG("poolahb: migration refused (no VM owner proof: writers not stopped)"); }
        return -1;
    }
    static unsigned calls;
    if ((calls++ % 120u) == 0) {
        unsigned np = 0, pending = 0, done = 0, failed = 0;
        uint64_t hot = 0;
        for (int i = 0; i < PA_POOLS; i++) {
            if (!g_pa_pool[i].used) continue;
            np++;
            if (g_pa_pool[i].done) done++;
            else if (g_pa_pool[i].failed) failed++;
            else { pending++; if (g_pa_pool[i].bytes > hot) hot = g_pa_pool[i].bytes; }
        }
        PA_LOG("poolahb service: calls=%u pools=%u(已迁=%u 待定=%u 失败=%u) 失败原因[超大=%u 覆盖不足=%u 别名=%u AHB=%u 事务=%u] 最热待定=%llu KB 已用=%llu MB",
               calls, np, done, pending, failed, g_fail_big, g_fail_multi, g_fail_alias, g_fail_ahb, g_fail_tx,
               (unsigned long long)(hot >> 10), (unsigned long long)(g_pa_bytes >> 20));
    }
    int moved = 0;
    // 衰减：只要近期热（否则装载期累计的字节会永远把冷池顶到前面，实测迁移 3 个池只有 6% 命中）。
    for (int i = 0; i < PA_POOLS; i++) {
        if (!g_pa_pool[i].used || g_pa_pool[i].done) continue;
        g_pa_pool[i].bytes /= 2;
        g_pa_pool[i].refs /= 2;
    }
    // 选“省字节/AHB 字节”最高的池整池迁移（策略在 poolahb_policy.c，宿主有反例）。
    struct s2a_pa_candidate cand[PA_POOLS];
    int cn = 0;
    for (int i = 0; i < PA_POOLS; i++) {
        if (!g_pa_pool[i].used) continue;
        cand[cn].base = g_pa_pool[i].base;
        cand[cn].len = g_pa_pool[i].len;
        cand[cn].bytes = g_pa_pool[i].bytes;
        cand[cn].done = g_pa_pool[i].done;
        cand[cn].failed = g_pa_pool[i].failed;
        // 记录回写到 g_pa_pool 的下标映射
        g_pa_cand_map[cn] = i;
        cn++;
    }
    const uint64_t cap = pa_total_cap();
    int pick = s2a_pa_select(cand, cn, 64ull << 10, PA_POOL_MAX, cap > g_pa_bytes ? cap - g_pa_bytes : 0);
    if (pick >= 0) {
        int pi = g_pa_cand_map[pick];
        struct pa_pool_stat *ps = &g_pa_pool[pi];
        int why = pa_migratable(ps->base, ps->len);
        if (why) {
            pa_pool_note_settled(ps, 1);
            if (why == 1) { g_fail_alias++; PA_LOG("poolahb: pool %#llx has a controlled alias, refuse migration", (unsigned long long)ps->base); }
            else g_fail_multi++;
            PA_LOG("poolahb: pool %#llx+%llu MB not contiguously rw-mapped", (unsigned long long)ps->base, (unsigned long long)(ps->len >> 20));
            pa_dump_maps(ps->base, ps->len);
            g_pa_owner_ready = 0;   // 一次性授权：早退也要消费掉，下次必须重新给“writers 已停稳”的证明
            return -1;
        }
        if (pa_migrate_window(ps->base, ps->len) != 0) {
            pa_pool_note_settled(ps, 1); g_fail_tx++;
            PA_LOG("poolahb: pool %#llx migrate failed (transaction)", (unsigned long long)ps->base);
            g_pa_owner_ready = 0;
            return -1;
        }
        PA_LOG("poolahb: pool %#llx hot (%.0f KB/窗, %u refs, %llu MB) migrated whole",
               (unsigned long long)ps->base, (double)ps->bytes / 1024.0, ps->refs,
               (unsigned long long)(ps->len >> 20));
        pa_pool_note_settled(ps, 0);
        ps->bytes = 0;
        moved = 1;
    }
    g_pa_owner_ready = 0;   // 一次性授权：owner 每次要迁移都必须重新给出“writers 已停稳”的证明
    return moved;
}

// 提交编号 = 帧槽奇偶 + 1：录制时登记引用，只有该槽的 fence 真正等到后才退休。
// 开始一个新提交：返回单调递增的序号，后续 lookup 的保活都记在这个序号下。
// 不能用“帧槽奇偶+1”：延迟上屏时 N-1 与 N+1 落在同一槽，两个不同提交会同号，
// complete 一次就把另一帧仍在 GPU 上读的引用一起清掉（VUID-vkDestroyBuffer-buffer-00922）。
uint64_t s2a_poolahb_begin_seq(void) {
    PA_GUARD();
    if (!s2a_poolahb_enabled()) return 0;
    g_pa_seq = g_pa_seq_next++;
    return g_pa_seq;
}

// 该提交的完成证明已拿到：只退休这个序号下的引用。
void s2a_poolahb_complete_seq(uint64_t seq) {
    PA_GUARD();
    if (!s2a_poolahb_enabled() || !seq) return;
    (void)s2a_res_complete(seq);
    pa_reap_pending();
}

// 该槽已完成（fence 等到）：只有这时才允许退休它的引用。


// 兼容旧名：不再在这里做“完成证明”（延迟 present 分支此时 fence 还没等到）。
void s2a_poolahb_frame_boundary(uint64_t new_seq) {
    if (!s2a_poolahb_enabled()) return;
    (void)new_seq;
    // 这里不发起迁移：改表事务（s2a_track_backing_begin）会与 present 互相等待，实测把帧卡到 600 ms。
    // 迁移只在池建立点（guest 线程）做；帧边界只负责“上一帧提交已完成”的证明。
}

// guest 写过这段池内存：推进内容版本（零拷贝是否启用都要记，供后续版本化/校验使用）。
int s2a_poolahb_note_write(uint64_t addr, uint64_t len) {
    if (!s2a_poolahb_enabled() || !g_pa_dev || !len) return -1;
    return s2a_res_note_write(addr, len);
}

// 池被 Finalize：账本立即停止新命中；在途引用等 fence 完成后再释放后备（fence 证明由 complete_parity 给）。
void s2a_poolahb_pool_removed(uint64_t addr, uint64_t len) {
    PA_GUARD();
    if (!s2a_poolahb_enabled() || !g_pa_dev || !len) return;
    for (int i = 0; i < g_pa_live_n; i++) {
        struct pa_live *e = &g_pa_live[i];
        if (e->retired) continue;
        if (e->addr != addr || e->len != len) continue;
        (void)s2a_res_retire(addr, len);      // 新命中立即停
        e->retired = 1;                       // 标记该条目待回收
        g_pa_release_pending++;                // 释放推迟到没有在途引用时（见 pa_reap_pending）
    }
    // 池统计条目一并标失败，避免继续追这个池
    for (int i = 0; i < PA_POOLS; i++)
        if (g_pa_pool[i].used && g_pa_pool[i].base == addr) pa_pool_note_removed(&g_pa_pool[i]);
}

// 有完成证明时才真正释放已删除池的后备。
static void pa_reap_pending(void) {
    if (!g_pa_release_pending) return;
    for (int i = 0; i < g_pa_live_n; ) {
        struct pa_live *e = &g_pa_live[i];
        if (!e->retired) { i++; continue; }   // 只回收“池已删除”的；仍在用的绝不销毁
        int busy = 0;
        uint64_t id = e->backing_id;
        if (s2a_res_backing_busy(id, &busy) != S2A_RES_OK || busy) { i++; continue; }
        s2a_gpu_buf_destroy(g_pa_dev, &e->gb);
        pa_live_remove(i);                    // 压缩数组，不留洞（否则二分查找失效）
        if (g_pa_release_pending) g_pa_release_pending--;
    }
}

void s2a_poolahb_shutdown(void) {
    PA_GUARD();
    for (int i = 0; i < g_pa_live_n; i++)
        if (g_pa_dev) s2a_gpu_buf_destroy(g_pa_dev, &g_pa_live[i].gb);
    if (g_pa_dev && g_pa_pending_used) s2a_gpu_buf_destroy(g_pa_dev, &g_pa_pending.gb);
    memset(g_pa_live, 0, sizeof g_pa_live);
    g_pa_live_n = 0;
    memset(&g_pa_pending, 0, sizeof g_pa_pending);
    g_pa_pending_used = 0;
    // 会话重建后同 base 重新注册的池不能继承上一局的 done/failed，否则该池永远不再迁移。
    memset(g_pa_pool, 0, sizeof g_pa_pool);
    __atomic_store_n(&g_pa_pending_pools, 0, __ATOMIC_RELAXED);
    g_pa_release_pending = 0;
    g_fail_big = g_fail_multi = g_fail_ahb = g_fail_tx = g_fail_alias = 0;
    g_pa_fatal = 0;
    s2a_res_reset();
    g_pa_dev = NULL;
    g_pa_bytes = 0;
    g_pa_migrate_us = 0;
    g_pa_migrate_windows = 0;
    memset(&g_pa_stats, 0, sizeof g_pa_stats);
    // 已经替换过的 guest 映射不会恢复；要回到原路径必须新进程（方案 §7）。
}

void s2a_poolahb_stats_get(struct s2a_poolahb_stats *out) {
    PA_GUARD();
    if (out) *out = g_pa_stats;
}
