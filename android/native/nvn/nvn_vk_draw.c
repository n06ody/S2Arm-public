// 新后端的绘制入口（第一步）：把回放器交来的 prog_t 换成真实着色器模块。
//
// 这一片只做"程序 → VkShaderModule + 绑定表"的加载与缓存，管线/描述符/顶点输入还没接上，
// 所以绘制会**明确失败**并写出已经加载到哪一步（阶段数、绑定数、输入数），不画占位画面。
// 加载路径与引擎无关：直接读设备内编译产出的 <key>.program.json（S2A_SHADER_DIR）。
#include <stdarg.h>
#include <sys/system_properties.h>
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>

#include "nvn_vk_draw.h"
#include "nvn_vk_threadbuf.h"
#include "nvn_vk_compile_facts.h"
#include "nvn_vk_pipe_worker.h"   // W2.7：longframe 归因读管线创建计数

#include "direct/program_cache.h"
#include "direct/offline_library.h"
#include "nvn_vk_program.h"
#include "nvn_obj.h"
#include "vk_record.h"
#include "direct/diag.h"
#include "direct/compile_export.h"
#include "direct/compile_export_w2.h"
#include "direct/w2_observation.h"
#include "direct/compile_worker.h"
#include "direct/input_identity.h"
#include "direct/texture_facts.h"
#include "mtl_vk_adapter.h"
#include "direct/source_store.h"
#include "direct/compile_request.h"
#include "direct/sha256.h"
#include "nvn_vk_first_draw.h"
#include "nvn_vk_format.h"
#include "nvn_first_use.h"   // C4a：iOS pso_pend 语义共用件（graphics/nvn/gfx/nvn）

// SetShaders 期的源快照（direct/source_store.c）。编译请求必须从这里取 code/control，
// 不能用绑定后可能已被游戏复用的内存。非 Android 构建没有这个实现。
extern int s2a_source_get_key(const uint8_t key[8], struct s2a_source_entry_view *out) __attribute__((weak));
extern int s2a_source_acquire(const uint8_t key[8], struct s2a_source_entry_view *out) __attribute__((weak));
extern void s2a_source_release(uint64_t ref) __attribute__((weak));
extern int s2a_source_identity(const uint8_t key[8], uint8_t sha[32], uint32_t *code_bytes,
                               uint32_t *control_bytes) __attribute__((weak));
extern uint64_t s2a_source_epoch(void) __attribute__((weak));
extern uint64_t s2a_vk_adapter_registry_generation(void) __attribute__((weak));
extern uint64_t s2a_source_rejected(void) __attribute__((weak));
extern int s2a_vk_adapter_tex_facts(int id, struct s2a_vk_tex_facts *out, char *why, size_t why_size)
    __attribute__((weak));
// 弱链接[androiddraw]：s2a_vk_adapter_exec_pause（宿主单测编本文件不链 mtl_vk_adapter.c；macOS ld64 不接受可达的未定义弱引用，按 tools/weak_link_decl.py 登记放行）
// 弱链接[androiddraw]：s2a_vk_adapter_exec_resume
// 弱链接[androiddraw]：s2a_vk_adapter_tex_facts
extern int s2a_vk_adapter_exec_pause(void) __attribute__((weak));
extern void s2a_vk_adapter_exec_resume(void) __attribute__((weak));

// 设备内补编入口（shader_survey.c）：编译器晚于部分 SetShaders 绑定时，那里排队的 code/control
// 在这里补编落盘，再重试加载。没有该模块（非 Android 构建）时为空。
extern int dm_shader_survey_flush(void) __attribute__((weak));

#define NVK_PROGRAM_CACHE 8

static void draw_log(const char *fmt, ...);

// 第一笔真实绘制：把 Draw 快照 + SetShaders 源快照拼成 s2a-direct-compile-v1，经已绑定的 compile_fn
// 真正调用统一编译器，并把结果复制到后端自有内存。
//
// 结果缓存（有界、不可变、带引用）：
//   key = 阶段 key 组 + 会改变编译请求的全部快照事实（命中时另校验 session/generation）；
//   条目持有结果字节的独立副本，寿命与源快照无关（源快照被淘汰不影响已建条目）；
//   绘制期间 pin（users>0），淘汰只发生在 users==0 的条目上；容量按字节预算而不是固定槽数，
//   并统计命中/未命中/淘汰/被 pin 阻止/相同完整请求重编；预算内腾不出位置时明确失败，不静默丢。
#define NVK_PROGRAM_ENTRIES 128u   // 初始槽数（不是上限）

// 32 MiB 在 NPC/商人这种工作集大的区域不够：实测常驻 33.5 MB 就开始淘汰，而每次未命中都要
// 从磁盘重读约 31 KB 编译结果。48 MiB 留出余量，仍是明确上限（不是无限增长）。
#define NVK_PROGRAM_BUDGET (48u << 20)
#define NVK_REQ_HISTORY 256u

struct nvk_cached_program {
    char key[144];               // 仅用于日志/淘汰显示（阶段 key 组），不参与命中判断
    uint8_t input_sha[32];        // 规范输入的完整 SHA-256（内容身份）
    // 规范输入原文（命中时逐字节复核）。按实际长度堆分配：身份通常几百字节，固定 4096 数组会让
    // 每个槽固定吃 4 KB、把可缓存程序数压到几百。NULL = 这份身份副本没有建立（该条目永不命中，
    // 但也不影响本笔已经拿到的结果）。
    uint8_t *input;
    uint64_t input_hash;         // FNV-1a 64：命中的廉价预筛（原文复核才是判据）
    uint16_t input_len;
    uint8_t *bytes;
    size_t len;
    char request_id[64];
    uint8_t request_sha[32];     // 完整编译请求的 SHA-256：只用于统计“同一完整请求重编”
    uint8_t result_sha[32];      // 完整结果字节的 SHA-256：建立时算一次，逐笔绘制不再重算
    uint64_t session, generation, last_use;
    // 每次“填槽”新发一个代号。内容身份备忘（见下）用它来证明“这个序号拿到的还是当初
    // 那一条”：槽被淘汰/复用后代号必然变化，备忘不需要第二次判据也不会误认。
    // 顶点属性格式元组：只用于"格式对等价"的证据采集（same_result 时对比两份身份对应的元组），
    // 不参与命中判据（判据仍是规范输入逐字节相等）。
    uint32_t vattr_fmt[32];
    uint64_t serial;
    uint32_t users;              // >0 = 绘制正在使用，禁止淘汰
    uint8_t c1[2][0x100];        // 结果建立时读到的 c1 常量段副本（源快照淘汰后仍可绑定）
    uint8_t c1_ok;               // bit per NVN stage（0 vs / 1 fs）
    int from_disk;               // 1 = 结果取自跨局缓存，不是本局编出来的
    int failed;
    char why[192];
};

// 槽数按需增长：固定 128 槽时，世界不同区域的工作集会超过它（真机在商人/泰瑞尔处
// `misses=15136 evicts=15008`，每次未命中都从磁盘重新读 ~31 KB 程序结果），表现为 NPC 处卡顿。
// 上限 512 槽 + 32 MiB 字节预算；增长用 realloc，下标不变，所以备忘里存的 idx 仍然有效
// （备忘另有 serial 校验，即使下标指向别的条目也只会当成未命中）。
static struct nvk_cached_program *g_programs;
static unsigned g_program_slots;

// 槽数没有语义上限，只有内存上限：数组本身按 4 MiB 封顶（每槽 sizeof(struct nvk_cached_program)，
// 约 4.2 KB，其中 4 KB 是规范输入副本），结果字节另有 NVK_PROGRAM_BUDGET 的独立预算。
// 槽数跟着工作集走：128→256→512…，所以“再多一个程序就要淘汰”的隐藏阈值不存在。
static unsigned prog_slots_max(void) {
    unsigned m = (4u << 20) / (unsigned)sizeof(struct nvk_cached_program);
    if (m < NVK_PROGRAM_ENTRIES) m = NVK_PROGRAM_ENTRIES;
    return m;
}

static struct nvk_cached_program *nvk_program_grow(void) {
    unsigned n = g_program_slots ? g_program_slots * 2 : NVK_PROGRAM_ENTRIES;
    const unsigned cap = prog_slots_max();
    if (n > cap) n = cap;
    if (n <= g_program_slots) return NULL;
    struct nvk_cached_program *ng = (struct nvk_cached_program *)realloc(g_programs, (size_t)n * sizeof *ng);
    if (!ng) return NULL;   // 增长失败：按“缓存满”处理，淘汰旧条目，语义不变
    memset(ng + g_program_slots, 0, (size_t)(n - g_program_slots) * sizeof *ng);
    g_programs = ng;
    g_program_slots = n;
    return ng;
}
// P1 内容身份备忘：每线程一组“已验证输入 → 条目代号”。判据/键字段在
// nvn_vk_compile_facts.c（宿主回归跑同一份实现）。
// 放置按摘要做集合相联（每桶 NVK_IDMEMO_PROBE 路），不是先进先出覆盖：世界的真实变体数
// 远超原 128 槽，环形覆盖会把仍在用的条目挤掉（实测命中 49%），而且每次未命中都要重算
// 纹理事实 + 身份编码 + 缓存扫描。容量到 512（真实变体数在千级以内）后桶碰撞显著下降。
// 用堆分配（仅绘制线程惰性一次），避免把 ~1.2MB 静态 TLS 挂到每个线程。
// 判据仍是整键+状态表逐字节相等，改错只会重算、不会算错。
#define NVK_IDMEMO 512
#define NVK_IDMEMO_PROBE 8
static __thread struct nvk_id_memo *g_idmemo;
static __thread unsigned g_idmemo_next;
// 备忘槽是每线程 ~4.8 MB 的堆分配（512 × 9.6 KB），交给 nvn_vk_threadbuf：线程退出时由 pthread_key
// 析构归还，不依赖“只有渲染线程会走到这里”这个假设（会话重建会重建渲染线程，预热路径也可能各建一份）。
// 本次查找用的摘要：命中路径只用它定位桶，未命中路径用它把新条目放进同一个桶。
static __thread uint64_t g_idmemo_hash;
static struct nvk_id_memo *idmemo_slots(void) {
    if (!g_idmemo) {
        g_idmemo = (struct nvk_id_memo *)nvn_vk_threadbuf_acquire(NVK_IDMEMO * sizeof *g_idmemo);
        if (g_idmemo) memset(g_idmemo, 0, NVK_IDMEMO * sizeof *g_idmemo);   // 新线程首次：按空槽开始
    }
    return g_idmemo;   // 分配失败返回 NULL：调用方按“未命中”走完整判据（正确性不受影响）
}
// 未命中的放置：先找本桶空槽（serial==0）填补，满了再轮转覆盖。只在未命中路径调用。
static void idmemo_store(const struct nvk_id_key *k, const struct s2a_record_draw_state *st,
                         uint64_t serial, unsigned idx) {
    struct nvk_id_memo *memo = idmemo_slots();
    if (!memo) return;
    const unsigned base = (unsigned)(g_idmemo_hash & (NVK_IDMEMO - 1));
    unsigned slot = 0;
    for (unsigned k2 = 0; k2 < NVK_IDMEMO_PROBE; k2++) {
        unsigned s = (base + k2) & (NVK_IDMEMO - 1);
        if (memo[s].serial == 0) { slot = s; goto store; }
    }
    slot = (base + (g_idmemo_next++ % NVK_IDMEMO_PROBE)) & (NVK_IDMEMO - 1);
store:
    nvk_id_memo_store(&memo[slot], k, st, serial, idx);
}
static uint64_t g_program_clock;
static uint64_t g_entry_serial;
static size_t g_program_bytes;
// 诊断：不同“内容身份”却编译出**完全相同的结果字节**（result_sha 相同）的次数。这个数说明
// 身份键里有多少字段其实不影响编译产物——只有在同一次运行里数出来，才可以据此精简键；
// 精简后仍要能通过 `debug.s2a.id_verify` 的完整复核，不能只靠推断。
static _Atomic uint64_t g_program_same_result, g_program_same_result_checked;
// 顶点属性"格式对"证据（诊断 + 归一化门）：同一份程序产物下，实际观察到的 (fmt_a, fmt_b) 位置互换。
// 只有被观察到“换了格式对、产物仍逐字节相同”的对，才允许在开启归一化时把它们映射到同一个代表值。
#define NVK_ATTR_PAIRS 64u
struct nvk_attr_pair { uint32_t a, b, n; };
static struct nvk_attr_pair g_attr_pairs[NVK_ATTR_PAIRS];
static unsigned g_attr_pairs_n;
static _Atomic uint64_t g_attr_perm_ok, g_attr_perm_bad;

static void attr_pair_note(uint32_t a, uint32_t b) {
    if (a > b) { uint32_t t = a; a = b; b = t; }
    for (unsigned i = 0; i < g_attr_pairs_n; i++)
        if (g_attr_pairs[i].a == a && g_attr_pairs[i].b == b) { g_attr_pairs[i].n++; return; }
    if (g_attr_pairs_n < NVK_ATTR_PAIRS) {
        g_attr_pairs[g_attr_pairs_n].a = a; g_attr_pairs[g_attr_pairs_n].b = b;
        g_attr_pairs[g_attr_pairs_n].n = 1;
        g_attr_pairs_n++;
    }
}
// 归一化（默认关，debug.s2a.attrnorm=1 才生效）：把已证明等价的格式对映射到较小的代表值。
// 只有观察到 >=8 次“换了这一对、产物逐字节相同”的对才进表生效；任何一次无法归因（attr_perm_bad）
// 都应该先停下来看，而不是打开这个开关。
uint32_t nvk_attr_pair_canon(uint32_t f) {
    static int on = -1;
    if (on < 0) { char b[PROP_VALUE_MAX] = {0}; __system_property_get("debug.s2a.attrnorm", b); on = (b[0] && atoi(b)) ? 1 : 0; }
    if (!on) return f;
    for (unsigned i = 0; i < g_attr_pairs_n; i++) {
        if (g_attr_pairs[i].n < 8) continue;
        if (g_attr_pairs[i].a == f) return g_attr_pairs[i].a;
        if (g_attr_pairs[i].b == f) return g_attr_pairs[i].a;
    }
    return f;
}
static uint64_t g_program_lookups, g_program_hits, g_program_misses, g_program_evicts,
                g_program_pinned_blocked, g_program_recompiles, g_program_disk_hits,
                g_program_offline_hits, g_program_offline_misses, g_program_offline_rejects,
                g_program_compile_attempts, g_program_compile_success, g_program_compile_failures;
// W2.7 longframe 归因（CPU 半）：真正调用编译器的次数与墙钟；GPU 时间戳另接。
static uint64_t g_compile_count, g_compile_ns_total, g_compile_ns_max;
// W2.7 逐帧归因：present 侧按帧取累计值做差（见 mtl_vk_shim.c 的 longframe(本帧)）。
void nvk_program_long_totals(uint64_t *compiles, uint64_t *compile_ns,
                             uint64_t *pipes, uint64_t *pipe_ns) {
    if (compiles) *compiles = g_compile_count;
    if (compile_ns) *compile_ns = g_compile_ns_total;
    uint64_t attempts = 0, failures = 0, ns = 0, max = 0;
    nvk_pipeline_long_stats(&attempts, &failures, &ns, &max);
    (void)failures; (void)max;
    if (pipes) *pipes = attempts;
    if (pipe_ns) *pipe_ns = ns;
}
// 程序查找（含身份编码、纹理事实、缓存扫描）本身的耗时：它在 draw 内部计时之外，
// 属于 op=26 与 draw timing 之间那段没有归属的时间。
static _Atomic uint64_t g_pf_calls, g_pf_ns;
// 程序查找内部再拆三段：纹理事实 / 身份编码+摘要 / 缓存扫描。没有这三个数就只能猜。
static _Atomic uint64_t g_pf_tex_ns, g_pf_id_ns, g_pf_scan_ns;
// 纹理事实内部：注册表查询 vs 其余（格式表+名字拷贝）。两者都可能是主项，所以分开测。
static _Atomic uint64_t g_pf_reg_ns;
// 内容身份备忘（见 nvk_id_memo）：调用次数与命中次数，用来判断这条优化到底有没有吃上。
static _Atomic uint64_t g_pf_memo_calls, g_pf_memo_hits;
// 未命中的两种成因分开数：桶内根本没有同摘要的条目（键漂移/代际推进，放置策略再改也没用）
// vs 有同摘要但状态表不同（工作集真的比桶宽）。这两个数决定下一步改键还是改容量。
static _Atomic uint64_t g_pf_memo_miss_nokey, g_pf_memo_miss_diff;
// 备忘未命中、但完整身份在内存缓存里已经命中：这部分是“键漂移/代际推进”造成的可回收量，
// 不是真的新程序。设备上先看这个比例，再决定 P1 要不要继续加码（不承诺具体收益）。
static _Atomic uint64_t g_pf_memo_miss_cached;
// P1 自检（debug.s2a.id_verify=1）：备忘命中时再走一遍完整事实/身份，和条目里存的规范输入
// 逐字节比较。只用来在真实命中入口证明“捷径等价”；关闭时只多一个 static int 判断。
static _Atomic uint64_t g_pf_memo_verify_calls, g_pf_memo_verify_bad;
static int nvk_id_verify_enabled(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.id_verify", b);
        v = (b[0] && strcmp(b, "0")) ? 1 : 0;
    }
    return v;
}
// 身份不可用（缺完整源快照 / 编码越规格）导致的“本次绘制不做缓存查找”：与 miss 严格区分，
// 不能用 miss 统计代替（它会让“没查”看起来像“查了没中”）。
static _Atomic uint64_t g_id_unavailable;
// 纹理事实缓存的命中统计（见 nvk_tex_facts）。
static _Atomic uint64_t g_tf_calls, g_tf_hits;
// draw 外壳：builder 总时长 / 查找前 / 查找后。op=26 与 draw timing 之间那段一直靠相减猜，
// 这三个数把它拆开：外壳 = 前段 + 查找 + 后段。
static _Atomic uint64_t g_dbg_builder_ns, g_dbg_builder_n, g_dbg_pre_ns, g_dbg_tail_ns;
// 逐笔查找/外壳的分段耗时只在 debug.s2a.perf_timing=1 时采：正常游玩时这些打点曾是每笔
// 4 次（外壳）+ 每个纹理绑定 2 次 clock_gettime，世界帧 500 笔、每笔最多 32 个纹理，
// 是本路径最大的一笔诊断开销。关闭时返回 0，不读时钟。
static int g_pf_stats = -1;
static int pf_stats_on(void) {
    if (g_pf_stats < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.perf_timing", b);
        g_pf_stats = b[0] ? atoi(b) : 0;
    }
    return g_pf_stats;
}
static inline uint64_t pf_now(void) {
    if (!pf_stats_on()) return 0;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

// 后段残差（诊断，开关 debug.s2a.draw_tail，默认关）：c1 常量段回填与 nvk_program_release
// 在 draw timing 分项之外，原先只能靠相减猜。采样闸门取 first_draw 的同一帧闸门（perf_sample
// 时两者看同一批帧），调用方每笔最多 +2 次 clock；关闭时不取钟、不累加、不打印。
static _Atomic uint64_t g_dbg_c1_ns, g_dbg_c1_n, g_dbg_rel_ns, g_dbg_rel_n;
// c1 块的内部拆账（同一个 debug.s2a.draw_tail 开关）：3.69 us/笔 究竟花在“程序条目→绘制状态复制”、
// “回源快照 acquire”还是“回写程序条目”。没有这三个数就只能整块优化，容易改错地方。
//   fill  = 第一层（prog->c1 → ms->c1 的 2×0x100 复制）
//   acq   = 第二层每段 s2a_source_acquire + release（含 memset 视图、命中/未命中）
//   wb    = 回写 prog->c1（只在 st<2 且 hit 时发生）
static _Atomic uint64_t g_c1_fill_ns, g_c1_acq_ns, g_c1_acq_n, g_c1_acq_held, g_c1_wb_n;
static _Atomic uint64_t g_c1_need_n;   // 真正进入第二层（至少有一次 acquire 尝试）的笔数
// 弱链接[androiddraw]：nvn_vk_first_draw_tail_on（nvn_vk_first_draw.c 提供；宿主单测从 draw 抽 body，不链 first_draw.c）
extern int nvn_vk_first_draw_tail_on(void) __attribute__((weak));
static inline uint64_t nvk_tail_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
void nvk_draw_take_tail_stats(uint64_t *c1_ns, uint64_t *c1_n, uint64_t *rel_ns, uint64_t *rel_n) {
    if (c1_ns) *c1_ns = atomic_exchange(&g_dbg_c1_ns, 0);
    if (c1_n) *c1_n = atomic_exchange(&g_dbg_c1_n, 0);
    if (rel_ns) *rel_ns = atomic_exchange(&g_dbg_rel_ns, 0);
    if (rel_n) *rel_n = atomic_exchange(&g_dbg_rel_n, 0);
}

// 相同完整请求的历史（只存 SHA-256；不按 requestId 合并不同变体）。
static struct { uint8_t sha[32]; uint64_t count; } g_req_history[NVK_REQ_HISTORY];
static unsigned g_req_history_next;

static int nvk_req_seen(const uint8_t sha[32]) {
    for (unsigned i = 0; i < NVK_REQ_HISTORY; i++)
        if (g_req_history[i].count && !memcmp(g_req_history[i].sha, sha, 32)) return 1;
    return 0;
}

static void nvk_req_note(const uint8_t sha[32]) {
    for (unsigned i = 0; i < NVK_REQ_HISTORY; i++)
        if (g_req_history[i].count && !memcmp(g_req_history[i].sha, sha, 32)) { g_req_history[i].count++; return; }
    unsigned at = g_req_history_next++ % NVK_REQ_HISTORY;
    memcpy(g_req_history[at].sha, sha, 32);
    g_req_history[at].count = 1;
}

// F4.3：查找与编译共用的完整规范输入（direct/input_identity.c）。旧的 64 位签名漏了
// constantBufferUse（FS 0→1 时请求不同、签名相同 a63653ea25a3f33a），所以内容身份改为
// “规范编码 + 完整 SHA-256”，命中还要逐字节比较同一份编码；session/generation/requestId
// 只是传输身份，不参与内容键。
// 逐笔都要跑：snprintf 六个 stage 的 8 字节十六进制实测占程序查找里的固定开销，
// 换成查表直写。输出格式与原实现一致（阶段前缀 + 16 个十六进制字符）。
static char *nvk_hex8(char *o, const uint8_t b[8]) {
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) { *o++ = d[b[i] >> 4]; *o++ = d[b[i] & 0x0f]; }
    return o;
}
static void nvk_program_display_key(const struct s2a_record_draw_state *state, char out[144]) {
    char *o = out;
    for (unsigned st = 0, idx = 0; st < 6 && idx < 6; st++) {
        if (!(state->stages & (1u << st))) continue;
        *o++ = st == 0 ? 'v' : st == 1 ? 'f' : '?';
        o = nvk_hex8(o, state->keys[idx]);
        idx++;
    }
    if (o == out) { memcpy(out, "none", 5); return; }
    *o = 0;
}

static uint64_t nvk_fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static void nvk_program_drop(struct nvk_cached_program *e) {
    if (e->bytes) g_program_bytes -= e->len;
    free(e->bytes);
    free(e->input);
    memset(e, 0, sizeof *e);
}

// 为 need 字节腾位置并返回可写槽；被 pin 的条目永不释放。NULL = 预算内没有可淘汰条目。
static struct nvk_cached_program *nvk_program_reserve(size_t need) {
    if (need > NVK_PROGRAM_BUDGET) return NULL;
    int blocked = 0;
    while (g_program_bytes + need > NVK_PROGRAM_BUDGET) {
        struct nvk_cached_program *victim = NULL;
        for (unsigned i = 0; i < g_program_slots; i++) {
            struct nvk_cached_program *e = &g_programs[i];
            if (!e->key[0]) continue;
            if (e->users) { blocked = 1; continue; }
            if (!victim || e->last_use < victim->last_use) victim = e;
        }
        if (!victim) break;
        if (g_program_evicts < 8)
            draw_log("program cache: evict %s (%zu B) to stay within %u MiB", victim->key, victim->len,
                     (unsigned)(NVK_PROGRAM_BUDGET >> 20));
        nvk_program_drop(victim);
        g_program_evicts++;
    }
    if (g_program_bytes + need > NVK_PROGRAM_BUDGET) { if (blocked) g_program_pinned_blocked++; return NULL; }
    if (!g_programs) (void)nvk_program_grow();
    struct nvk_cached_program *empty = NULL, *victim = NULL;
    for (unsigned i = 0; i < g_program_slots; i++) {
        struct nvk_cached_program *e = &g_programs[i];
        if (!e->key[0]) { empty = e; break; }
        if (e->users) { blocked = 1; continue; }
        if (!victim || e->last_use < victim->last_use) victim = e;
    }
    if (empty) return empty;
    // 没有空槽：先尝试加槽（工作集比槽数宽时这才是正确的做法），加不动再淘汰。
    if (nvk_program_grow()) {
        for (unsigned i = 0; i < g_program_slots; i++)
            if (!g_programs[i].key[0]) return &g_programs[i];
    }
    if (!victim) { g_program_pinned_blocked++; return NULL; }
    g_program_evicts++;
    nvk_program_drop(victim);
    return victim;
}

// 源快照 pin/淘汰读数：程序缓存汇总之外单独打，保证短局（未到 8192 次查找）也能取到回执。
static void nvk_source_pin_log(const char *why) {
    struct s2a_source_pin_stats ps;
    uint32_t entries = 0, bytes = 0, dropped = 0;
    s2a_source_pin_stats(&ps);
    s2a_source_stats(&entries, &bytes, &dropped);
    draw_log("源快照 pin(%s)：pinned_entries=%u pins=%llu pin_calls=%llu pin_misses=%llu unpins=%llu "
             "evict_skipped=%llu keep_fail_bytes=%llu keep_fail_slots=%llu | entries=%u bytes=%u dropped=%llu",
             why, ps.pinned_entries, (unsigned long long)ps.pins, (unsigned long long)ps.pin_calls,
             (unsigned long long)ps.pin_misses, (unsigned long long)ps.unpins,
             (unsigned long long)ps.evict_skipped, (unsigned long long)ps.keep_fail_bytes,
             (unsigned long long)ps.keep_fail_slots, entries, bytes, (unsigned long long)dropped);
}

static void nvk_program_stats(void) {
    if ((g_program_lookups & 8191u)) return;
    // 上面这行的 program cache 计数是整局累计（不做窗口化：淘汰/命中总量才是缓存容量的判据）。
    draw_log("program cache: lookups=%llu hits=%llu misses=%llu evicts=%llu pinned_blocked=%llu "
             "identical_recompiles=%llu slots=%u same_result=%llu/%llu resident=%zu B（累计）",
             (unsigned long long)g_program_lookups, (unsigned long long)g_program_hits,
             (unsigned long long)g_program_misses, (unsigned long long)g_program_evicts,
             (unsigned long long)g_program_pinned_blocked, (unsigned long long)g_program_recompiles,
             g_program_slots,
             (unsigned long long)atomic_load(&g_program_same_result),
             (unsigned long long)atomic_load(&g_program_same_result_checked), g_program_bytes);
    nvk_source_pin_log("summary");
    // 窗口口径（报告 §6.2）：查找各段与「draw 外壳」在同一个打印点（每 8192 次查找）一起读取并
    // 清零，所以同一批输出里的「外壳 = 前段 + 查找 + 后段」是同一窗口的样本，不再把整局累计的
    // 查找均值（含开机加载期的磁盘采纳/SHA/首次编译）和窗口化的前段/后段拼在一行。
    // 计时项只在 debug.s2a.perf_timing=1 时有样本，关闭时全为 0。切换点这一笔的瓶颈：本函数在
    // nvk_program_for_draw 内部被调用，此时这一笔的纹理/身份/扫描分项已经入账，而总耗时由外层
    // nvk_program_for_draw_timed 在返回后才累加——所以总量与分项在窗口边界错开一笔（1/8192）。
    // “其余”用饱和减法，避免这个错位把无符号差值下溢成巨大的假读数。
    const uint64_t pf_calls = atomic_exchange(&g_pf_calls, 0);
    const uint64_t pf_ns = atomic_exchange(&g_pf_ns, 0);
    const uint64_t pf_tex_ns = atomic_exchange(&g_pf_tex_ns, 0);
    const uint64_t pf_id_ns = atomic_exchange(&g_pf_id_ns, 0);
    const uint64_t pf_scan_ns = atomic_exchange(&g_pf_scan_ns, 0);
    const uint64_t pf_reg_ns = atomic_exchange(&g_pf_reg_ns, 0);
    const uint64_t tf_calls = atomic_exchange(&g_tf_calls, 0);
    const uint64_t tf_hits = atomic_exchange(&g_tf_hits, 0);
    const uint64_t memo_calls = atomic_exchange(&g_pf_memo_calls, 0);
    const uint64_t memo_hits = atomic_exchange(&g_pf_memo_hits, 0);
    const uint64_t builder_n = atomic_exchange(&g_dbg_builder_n, 0);
    const uint64_t builder_ns = atomic_exchange(&g_dbg_builder_ns, 0);
    const uint64_t pre_ns = atomic_exchange(&g_dbg_pre_ns, 0);
    const uint64_t tail_ns = atomic_exchange(&g_dbg_tail_ns, 0);
    {
        uint64_t ok = atomic_load(&g_attr_perm_ok), bad = atomic_load(&g_attr_perm_bad);
        if (ok || bad) {
            atomic_store(&g_attr_perm_ok, 0); atomic_store(&g_attr_perm_bad, 0);
            draw_log("attr 格式对：置换等价实证 %llu 次，无法归因 %llu 次，已知对 %u 个"
                     "（归一化 debug.s2a.attrnorm=1，默认关）",
                     (unsigned long long)ok, (unsigned long long)bad, g_attr_pairs_n);
            for (unsigned i = 0; i < g_attr_pairs_n && i < 8; i++)
                draw_log("  attr 格式对 #%u: fmt %u <-> %u  n=%u", i, g_attr_pairs[i].a,
                         g_attr_pairs[i].b, g_attr_pairs[i].n);
        }
        uint64_t c = pf_calls, ns = pf_ns;
        // 饱和减法：窗口切换点总量比分项少 1 笔，直接减可能下溢（见上面的注释）。
        const uint64_t pf_sub_ns = pf_tex_ns + pf_id_ns + pf_scan_ns;
        const uint64_t pf_other_ns = ns > pf_sub_ns ? ns - pf_sub_ns : 0;
        draw_log("程序查找耗时(本窗, 每 8192 次查找清零; debug.s2a.perf_timing=1 才有样本)："
                 "calls=%llu avg=%llu us = 纹理事实 %llu + 身份编码 %llu + 缓存扫描 %llu + 其余 %llu（均 us/笔）",
                 (unsigned long long)c, (unsigned long long)(c ? ns / c / 1000 : 0),
                 (unsigned long long)(c ? pf_tex_ns / c / 1000 : 0),
                 (unsigned long long)(c ? pf_id_ns / c / 1000 : 0),
                 (unsigned long long)(c ? pf_scan_ns / c / 1000 : 0),
                 (unsigned long long)(c ? pf_other_ns / c / 1000 : 0));
        {
            uint64_t c2 = c;
            draw_log("  纹理事实内部(同窗)：注册表查询 %llu us + 其余 %llu us（均 us/笔）",
                     (unsigned long long)(c2 ? pf_reg_ns / c2 / 1000 : 0),
                     (unsigned long long)(c2 ? (pf_tex_ns - pf_reg_ns) / c2 / 1000 : 0));
        }
        {
            draw_log("  纹理事实缓存(本窗)：命中 %llu / %llu（%llu%%）",
                     (unsigned long long)tf_hits, (unsigned long long)tf_calls,
                     (unsigned long long)(tf_calls ? tf_hits * 100 / tf_calls : 0));
        }
        {
            uint64_t vc = atomic_load(&g_pf_memo_verify_calls), vb = atomic_load(&g_pf_memo_verify_bad);
            if (vc)
                draw_log("  P1 自检(debug.s2a.id_verify)：%llu 次命中重算，%llu 次身份不同",
                         (unsigned long long)vc, (unsigned long long)vb);
            draw_log("  内容身份备忘(本窗)：命中 %llu / %llu（%llu%%），命中即跳过纹理事实+身份编码+缓存扫描；"
                     "本窗未命中 无同摘要=%llu 同摘要不同状态=%llu",
                     (unsigned long long)memo_hits, (unsigned long long)memo_calls,
                     (unsigned long long)(memo_calls ? memo_hits * 100 / memo_calls : 0),
                     (unsigned long long)atomic_load(&g_pf_memo_miss_nokey),
                     (unsigned long long)atomic_load(&g_pf_memo_miss_diff));
            atomic_store(&g_pf_memo_miss_nokey, 0); atomic_store(&g_pf_memo_miss_diff, 0);
        }
        {
            if (builder_n) {
                uint64_t lookup = pf_calls ? pf_ns / pf_calls : 0;
                draw_log("  draw 外壳(本窗, 与上面的查找同批；debug.s2a.perf_timing=1)："
                         "总 %llu us = 前段 %llu + 查找 %llu + 后段 %llu（均 us/笔，n=%llu）",
                         (unsigned long long)(builder_ns / builder_n / 1000),
                         (unsigned long long)(pre_ns / builder_n / 1000),
                         (unsigned long long)(lookup / 1000),
                         (unsigned long long)(tail_ns / builder_n / 1000),
                         (unsigned long long)builder_n);
            }
        }
        // c1 块内部拆账（同一个 debug.s2a.draw_tail 开关；与「外壳」同窗口读取即清零）。
        // 只回答“后段里 c1 这一块花在哪”，不改变任何分支。
        {
            const uint64_t fill_ns = atomic_exchange(&g_c1_fill_ns, 0);
            const uint64_t acq_ns = atomic_exchange(&g_c1_acq_ns, 0);
            const uint64_t acq_n = atomic_exchange(&g_c1_acq_n, 0);
            const uint64_t acq_held = atomic_exchange(&g_c1_acq_held, 0);
            const uint64_t wb_n = atomic_exchange(&g_c1_wb_n, 0);
            const uint64_t need_n = atomic_exchange(&g_c1_need_n, 0);
            if (need_n || fill_ns || acq_n) {
                draw_log("  c1 块(本窗, debug.s2a.draw_tail=1)：进入补取 %llu/%llu 笔；"
                         "prog→ms 复制 总 %.1f us；acquire %.2f us/次 ×%llu（held %llu，回写程序条目 %llu）",
                         (unsigned long long)need_n, (unsigned long long)builder_n,
                         (double)fill_ns / 1e3, acq_n ? (double)acq_ns / (double)acq_n / 1e3 : 0.0,
                         (unsigned long long)acq_n, (unsigned long long)acq_held, (unsigned long long)wb_n);
            }
        }
    }
    draw_log("源快照：拒绝 %llu（累计；短 key 与完整摘要不符） 内容代际 %llu（累计） 身份不可用 %llu（本窗）",
             (unsigned long long)(s2a_source_rejected ? s2a_source_rejected() : 0),
             (unsigned long long)(s2a_source_epoch ? s2a_source_epoch() : 0),
             (unsigned long long)atomic_load(&g_id_unavailable));
    atomic_store(&g_id_unavailable, 0);
    // 来源分开计数：disk adopt 不算 compile，offline 命中不算 miss；未接的来源不写 0。
    draw_log("program 来源：memory_hit=%llu offline_adopt=%llu private_adopt=%llu "
             "offline_miss=%llu offline_reject=%llu compile_attempt=%llu compile_success=%llu "
             "compile_failure=%llu",
             (unsigned long long)g_program_hits,
             (unsigned long long)g_program_offline_hits,
             (unsigned long long)g_program_disk_hits,
             (unsigned long long)g_program_offline_misses,
             (unsigned long long)g_program_offline_rejects,
             (unsigned long long)g_program_compile_attempts,
             (unsigned long long)g_program_compile_success,
             (unsigned long long)g_program_compile_failures);
    {
        uint64_t pa = 0, pf = 0, pns = 0, pmax = 0;
        nvk_pipeline_long_stats(&pa, &pf, &pns, &pmax);
        // 只报 CPU 侧真账：编译墙钟与真实 vkCreateGraphicsPipelines 的次数/耗时。
        // GPU 时间戳还没接（需要同 queue 的有效 timestamp 与 valid bits），这里不拿 CPU 等待冒充。
        draw_log("longframe 归因：compile=%llu ms=%llu max_ms=%llu | pipeline_create=%llu "
                 "failure=%llu ms=%llu max_ms=%llu | GPU 时间戳未接",
                 (unsigned long long)g_compile_count,
                 (unsigned long long)(g_compile_ns_total / 1000000ull),
                 (unsigned long long)(g_compile_ns_max / 1000000ull),
                 (unsigned long long)pa, (unsigned long long)pf,
                 (unsigned long long)(pns / 1000000ull),
                 (unsigned long long)(pmax / 1000000ull));
    }
    {
        struct s2a_compile_worker_stats cw;
        s2a_compile_worker_stats(&cw);
        // 容量只有 1：dedup 多说明合并起作用，busy_timeout 多说明排队超时（背压打满）。
        draw_log("补编 worker：runs=%llu compiles=%llu dedup=%llu busy_timeout=%llu "
                 "failure=%llu late_discarded=%llu",
                 (unsigned long long)cw.runs, (unsigned long long)cw.compiles,
                 (unsigned long long)cw.dedup, (unsigned long long)cw.busy_timeouts,
                 (unsigned long long)cw.failures, (unsigned long long)cw.late_discarded);
    }
    draw_log("program cache 跨局：disk_hits=%llu disk_stores=%llu disk_failures=%llu "
             "offline_hits=%llu offline_misses=%llu offline_rejects=%llu",
             (unsigned long long)g_program_disk_hits,
             (unsigned long long)s2a_program_cache_stores(),
             (unsigned long long)s2a_program_cache_failures(),
             (unsigned long long)g_program_offline_hits,
             (unsigned long long)g_program_offline_misses,
             (unsigned long long)g_program_offline_rejects);
}

static void nvk_program_release(struct nvk_cached_program *e) { if (e && e->users) e->users--; }

// Draw 快照 → graphicsState 事实（推导在 nvn_vk_compile_facts.c，宿主回归跑同一份）。
// 附件缺项只在这里记一次事实行：区分“该槽没绑纹理”“记录对象丢失”“格式没映射”，
// 并带上命令序号，便于与 SetRenderTargets 对账；不补默认格式。
static void nvk_log_rt_facts(const struct s2a_record_draw_state *state) {
    char facts[640];
    int o = 0;
    uint32_t mask = 0;
    for (unsigned i = 0; i < state->rt_count && i < 8; i++)
        if (state->rt[i].key) mask |= 1u << i;
    o += snprintf(facts + o, sizeof facts - (size_t)o,
                  "seq=%llu rt_seq=%llu n=%u nonnull=0x%x",
                  (unsigned long long)state->cmd_seq, (unsigned long long)state->rt_seq,
                  state->rt_count, mask);
    for (unsigned i = 0; i < state->rt_count && i < 8 && o < (int)sizeof facts - 8; i++)
        o += snprintf(facts + o, sizeof facts - (size_t)o,
                      " | [%u] null=%d magic=0x%x serial=%llu nvn=0x%x view_set=%d view_fmt=0x%x"
                      " vk=%d mapped=%d %ux%u",
                      i, state->rt[i].key == 0, state->rt[i].obj_magic,
                      (unsigned long long)state->rt[i].obj_serial, state->rt[i].nvn_format,
                      state->rt[i].view_format != 0, state->rt[i].view_format,
                      (int)state->rt[i].format, (int)nvn_vk_format(state->rt[i].nvn_format),
                      state->rt[i].width, state->rt[i].height);
    draw_log("rt facts (fragmentOutputTypes incomplete): %s", facts);
    draw_log("rt depth: present=%d magic=0x%x nvn=0x%x mapped=%d serial=%llu",
             state->has_depth, state->depth_magic, state->depth_nvn_format,
             (int)nvn_vk_format(state->depth_nvn_format), (unsigned long long)state->depth_serial);
}

static void nvk_fill_gfx(const struct s2a_record_draw_state *state, const struct s2a_direct_compile_context *pc,
                         struct s2a_compile_graphics_src *gfx, int log_facts) {
    int complete = 1;
    nvk_compile_facts_fill(gfx, state, pc, &complete);
    if (!complete && log_facts) nvk_log_rt_facts(state);
}

// F4.2B：纹理事实只需收集一次：身份（F4.3）与请求体必须用同一份（nvk_texture_set 见
// nvn_vk_compile_facts.h；每个在场阶段的 32 个组合槽都出一条事实，槽位语义对齐上游）。
// id → 纹理事实的线程内缓存。事实只随注册表里的 tex_t 快照变化，而 reg_tex/reg_samp/tex_release
// 都会推进注册表代际，所以代际相同的重复查询不必再拿一次注册表锁、也不必再走一遍哈希探查。
// 这里只做“同一代际内同一 id 的答案不变”这一条，不改变任何事实的算法。
#define NVK_TFACTS_CACHE 256
struct nvk_tfacts_slot { uint64_t reggen; int id; struct s2a_vk_tex_facts f; };
static __thread struct nvk_tfacts_slot g_tfacts[NVK_TFACTS_CACHE];

static int nvk_tex_facts(int id, struct s2a_vk_tex_facts *out) {
    if (!s2a_vk_adapter_tex_facts) return -1;
    atomic_fetch_add(&g_tf_calls, 1);
    uint64_t g = s2a_vk_adapter_registry_generation ? s2a_vk_adapter_registry_generation() : 0;
    struct nvk_tfacts_slot *e = &g_tfacts[(unsigned)id % NVK_TFACTS_CACHE];
    if (g && e->reggen == g && e->id == id) {
        *out = e->f;
        atomic_fetch_add(&g_tf_hits, 1);
        return 0;
    }
    char why[96] = "";
    if (s2a_vk_adapter_tex_facts(id, out, why, sizeof why)) return -1;
    e->reggen = g;
    e->id = id;
    e->f = *out;
    return 0;
}

// nvk_texture_facts_fill 的 lookup 注入：走带代际缓存的注册表查询，事实子集拷给纯计算层。
static int nvk_tex_lookup(int id, struct nvk_tex_desc *out, void *opaque) {
    (void)opaque;
    struct s2a_vk_tex_facts tf;
    uint64_t r0 = pf_now();
    int rc = nvk_tex_facts(id, &tf);
    atomic_fetch_add_explicit(&g_pf_reg_ns, pf_now() - r0, memory_order_relaxed);
    if (rc) return -1;
    out->nvn_format = tf.nvn_format;
    out->nvn_target = tf.nvn_target;
    out->layers = tf.layers;
    return 0;
}

static void nvk_fill_textures(const struct s2a_record_draw_state *state, struct nvk_texture_set *set) {
    nvk_texture_facts_fill(state, nvk_tex_lookup, NULL, set);
    if (set->unmapped_total) {
        // 注册过但目标/格式映射不了的槽位不出事实（准确失败路径）。缺事实诊断必须带原始输入：
        // 只写“未映射”无法区分是真实未知格式、缺 target 还是注册代际已过期。
        static int noted;
        for (int i = 0; i < set->unmapped_total && i < NVK_TEXTURE_UNMAPPED_MAX && noted < 8; i++, noted++) {
            // 原因串用同一条生产映射重算（带内部备忘，代价等于一次查表）。
            struct s2a_nvn_texture_fact nf;
            char fwhy[96] = "";
            (void)s2a_nvn_texture_fact(set->unmapped[i].nvn_format, set->unmapped[i].nvn_target,
                                       set->unmapped[i].layers, &nf, fwhy, sizeof fwhy);
            draw_log("compile textures: stage=%u slot=%u id=%u 格式/目标未映射（%s）"
                     " 原始事实 nvn_fmt=0x%x target=0x%x layers=%u reggen=%llu",
                     set->unmapped[i].stage, set->unmapped[i].slot, set->unmapped[i].id, fwhy,
                     set->unmapped[i].nvn_format, set->unmapped[i].nvn_target,
                     set->unmapped[i].layers,
                     (unsigned long long)(s2a_vk_adapter_registry_generation
                                              ? s2a_vk_adapter_registry_generation() : 0));
        }
        if (set->unmapped_total > NVK_TEXTURE_UNMAPPED_MAX && noted < 8)
            draw_log("compile textures: 未映射槽位共 %d 条（只打前 %d 条）",
                     set->unmapped_total, NVK_TEXTURE_UNMAPPED_MAX);
    }
    if (set->count) {
        static int noted;
        if (noted++ < 4)
            draw_log("compile textures: 收集 %d 条纹理事实（handle=8+2*slot, cbufSlot=-1，"
                     "注册不到描述符的槽位按上游既定默认补齐）", set->count);
    }
}

// F4.4：需求编译交给单槽 worker；等待期间交还执行锁，让 present/Stop 继续。
static int nvk_worker_pause(void *ctx) { (void)ctx; return s2a_vk_adapter_exec_pause ? s2a_vk_adapter_exec_pause() : -1; }
static void nvk_worker_resume(void *ctx) { (void)ctx; if (s2a_vk_adapter_exec_resume) s2a_vk_adapter_exec_resume(); }

static int nvk_worker_compile(uint64_t session, uint64_t generation, const char *json, size_t len,
                              const uint8_t sha[32], uint8_t **bytes, size_t *bytes_len,
                              char *why, size_t why_size) {
    // Production defines both in mtl_vk_adapter.c; optional links must pass a pair or neither.
    int has_pause_pair = s2a_vk_adapter_exec_pause && s2a_vk_adapter_exec_resume;
    return s2a_compile_worker_run(session, generation, json, len, sha, 10000,
                                  has_pause_pair ? nvk_worker_pause : NULL,
                                  has_pause_pair ? nvk_worker_resume : NULL,
                                  NULL, bytes, bytes_len, why, why_size);
}

/* ---------------- C4a/C3a 首用等待（debug.s2a.fu_wait，默认 0 = 旧同步路径） ----------------
   开关开：编译需求非阻塞投进 worker（submit_async + 短队列 + 完成环），本笔绘制按
   iOS pso_pend 语义决定等多久——可安全跳过的绘制只在帧预算内等，超预算/超单项上限
   跳过本笔并单独入账（不记入 unsupported，也不写失败缓存条目：下一笔绘制同一
   请求会再查询在途结果）。不可安全跳过的绘制按 unsafe 上限等，过限记 stuck 终局。

   帧与渲染目标证据：
   - frame id 取 first_draw 的当前录制帧序号（nvn_vk_first_draw_frame_id）；
   - rt_seen 在真实绘制成功时记（跳过的绘制不算画过）；
   - rt_fresh 由记录层在 ClearColor/ClearDepthStencil 执行点弱调用 nvn_vk_fu_rt_fresh；
   - fresh 表逐帧自清，seen 证据只留最近两帧（见 nvn_first_use.c）。 */

extern uint64_t nvn_vk_first_draw_frame_id(void) __attribute__((weak));

/* 弱链接[androiddraw]：s2a_compile_worker_submit_async（宿主单测桩化 worker 时不链
   compile_worker.c；缺实现 = 队列不可用，回落旧同步通道，与关闭异步同义） */
/* 弱链接[androiddraw]：s2a_compile_worker_poll */
/* 弱链接[androiddraw]：s2a_compile_worker_wait_async */
int s2a_compile_worker_submit_async(uint64_t, uint64_t, const char *, size_t,
                                    const uint8_t[32], char *, size_t) __attribute__((weak));
int s2a_compile_worker_poll(const uint8_t[32], uint8_t **, size_t *, char *, size_t) __attribute__((weak));
int s2a_compile_worker_wait_async(const uint8_t[32], uint32_t, uint8_t **, size_t *,
                                  char *, size_t) __attribute__((weak));

static int nvk_fu_on(void) {
    static int v = -1;
    if (v < 0) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.s2a.fu_wait", b);
        v = b[0] && atoi(b);
    }
    return v;
}
/* 三档预算与 iOS 默认值同（DM_NVN_PSO_*）；0 = 该项不设限（iOS 语义）。 */
static uint32_t nvk_fu_prop_ms(const char *name, uint32_t dflt, uint32_t *cache) {
    if (!*cache) {
        char b[PROP_VALUE_MAX] = {0};
        __system_property_get(name, b);
        *cache = b[0] ? (uint32_t)strtoul(b, NULL, 10) : dflt;
        if (!*cache) *cache = UINT32_MAX;   /* 0 也代表“显式不设限”：用哨兵分开“未读” */
    }
    return *cache == UINT32_MAX ? 0 : *cache;
}
static uint32_t nvk_fu_item_ms(void)   { static uint32_t v; return nvk_fu_prop_ms("debug.s2a.fu_item_ms", 2000, &v); }
static uint32_t nvk_fu_frame_ms(void)  { static uint32_t v; return nvk_fu_prop_ms("debug.s2a.fu_frame_ms", 100, &v); }
static uint32_t nvk_fu_unsafe_ms(void) { static uint32_t v; return nvk_fu_prop_ms("debug.s2a.fu_unsafe_ms", 10000, &v); }
static uint64_t nvk_fu_frame(void) { return nvn_vk_first_draw_frame_id ? nvn_vk_first_draw_frame_id() : 0; }

/* 首用等待的计时器：必须是真实单调钟。pf_now 受 draw_pf 统计开关门控（关时返 0），
   借它记账会让帧预算在统计关时永远不耗。 */
static uint64_t nvk_fu_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static struct s2a_fu_budget g_fu_budget;

/* 在途请求的 late/stuck 终局标记：iOS 写在 pending 项上，跨笔/跨帧都挡。
   request_sha 是完整请求身份；64 项直线表，挤最久未触的（满证据按保守走）。 */
#define NVK_FU_PEND 64
static struct { uint8_t sha[32]; uint8_t late, stuck; uint64_t last; } g_fu_pend[NVK_FU_PEND];
static uint64_t g_fu_pend_seq;

static int nvk_fu_pend_marks(const uint8_t sha[32], int *late, int *stuck) {
    for (unsigned i = 0; i < NVK_FU_PEND; i++)
        if (!memcmp(g_fu_pend[i].sha, sha, 32)) { *late = g_fu_pend[i].late; *stuck = g_fu_pend[i].stuck; return 1; }
    *late = *stuck = 0;
    return 0;
}
static void nvk_fu_pend_mark(const uint8_t sha[32], int timeout) {
    if (timeout != S2A_FU_T_LATE && timeout != S2A_FU_T_STUCK) return;   // BUDGET 不打项标（iOS 同义）
    unsigned slot = 0;
    uint64_t oldest = UINT64_MAX;
    for (unsigned i = 0; i < NVK_FU_PEND; i++) {
        if (!memcmp(g_fu_pend[i].sha, sha, 32)) { slot = i; break; }
        if (!g_fu_pend[i].last || g_fu_pend[i].last < oldest) { oldest = g_fu_pend[i].last; slot = i; }
    }
    memcpy(g_fu_pend[slot].sha, sha, 32);
    if (timeout == S2A_FU_T_LATE) g_fu_pend[slot].late = 1; else g_fu_pend[slot].stuck = 1;
    g_fu_pend[slot].last = ++g_fu_pend_seq;
}

/* 跳过计数与 unsupported 分账：跳过是有意的、可恢复的（下一笔可能已就绪）；
   stuck 是放弃一个已经证明等不出来的请求——两者都如实分类，不静默。 */
static _Atomic uint64_t g_fu_submit, g_fu_ready_n, g_fu_wait_n, g_fu_wait_ns;
static _Atomic uint64_t g_fu_skip_budget, g_fu_skip_late, g_fu_skip_stuck, g_fu_fallback;

static void nvk_fu_stats_log(void) {
    static _Atomic uint64_t n;
    uint64_t c = atomic_fetch_add(&n, 1) + 1;
    if (c != 1 && c % 256) return;
    draw_log("first-use: submit=%llu ready=%llu waits=%llu wait=%.1fms "
             "skip{budget=%llu late=%llu stuck=%llu} sync_fallback=%llu",
             (unsigned long long)atomic_load(&g_fu_submit),
             (unsigned long long)atomic_load(&g_fu_ready_n),
             (unsigned long long)atomic_load(&g_fu_wait_n),
             (double)atomic_load(&g_fu_wait_ns) / 1e6,
             (unsigned long long)atomic_load(&g_fu_skip_budget),
             (unsigned long long)atomic_load(&g_fu_skip_late),
             (unsigned long long)atomic_load(&g_fu_skip_stuck),
             (unsigned long long)atomic_load(&g_fu_fallback));
}

/* ClearColor/ClearDepthStencil 执行点（vk_record.c 弱调用）：目标内容本帧重新起过。 */
void nvn_vk_fu_rt_fresh(uint64_t serial) {
    if (nvk_fu_on()) s2a_fu_rt_mark_fresh(serial, nvk_fu_frame());
}

/* 真实绘制成功后调用：目标本帧又被画过（iOS rt_seen_put；跳过的绘制不记）。 */
static void nvk_fu_rt_drawn(const struct s2a_record_draw_state *state, uint64_t frame) {
    if (!nvk_fu_on()) return;
    for (uint32_t i = 0; i < state->rt_count && i < 8; i++)
        s2a_fu_rt_mark_seen(state->rt[i].obj_serial, state->rt[i].storage,
                            state->rt[i].nvn_format, frame);
    if (state->has_depth)
        s2a_fu_rt_mark_seen(state->depth_serial, state->depth_storage,
                            state->depth_nvn_format, frame);
}

/* iOS skip_safe：无 storage 写 + 每个目标本帧 fresh + 近 2 帧都画过。
   安卓侧的 storage 写证据：draw 快照里真实绑定的 SSBO（ssbo_bound）与 transform
   feedback。图形管线 storage image 绑定不进 draw 快照（保守口径，见交付限制）；
   片元输出写目标本身不是 storage 写——fresh/steady 两项覆盖它能不能安全丢。 */
static int nvk_fu_skip_safe(const struct s2a_record_draw_state *state, uint64_t frame) {
    if (state->tf_enabled) return 0;
    for (unsigned st = 0; st < 6; st++)
        if (state->ssbo_bound[st]) return 0;
    int targets = 0;
    for (uint32_t i = 0; i < state->rt_count && i < 8; i++) {
        targets++;
        if (!s2a_fu_rt_fresh_now(state->rt[i].obj_serial, frame)) return 0;
        if (!s2a_fu_rt_seen_steady(state->rt[i].obj_serial, state->rt[i].storage,
                                  (uint64_t)state->rt[i].nvn_format, frame)) return 0;
    }
    if (state->has_depth) {
        targets++;
        if (!s2a_fu_rt_fresh_now(state->depth_serial, frame)) return 0;
        if (!s2a_fu_rt_seen_steady(state->depth_serial, state->depth_storage,
                                  (uint64_t)state->depth_nvn_format, frame)) return 0;
    }
    return targets > 0;
}

static const char *nvk_fu_class(int t) {
    return t == S2A_FU_T_LATE ? "late" : t == S2A_FU_T_BUDGET ? "budget" : t == S2A_FU_T_STUCK ? "stuck" : "?";
}

/* 首用路径的编译等待：提交进 worker 在途，然后按判定环等/跳。
   返回 0 = 结果就绪（bytes）或已按类跳过（*skipped = S2A_FU_T_*）；
   非 0 = 真实失败（why 带原因，调用方照旧写失败条目）。
   等片期间的实际耗时计入本帧预算（iOS pso_budget_spend 同义）。 */
static int nvk_fu_compile(const struct s2a_record_draw_state *state,
                          uint64_t session, uint64_t generation,
                          const char *json, size_t len, const uint8_t sha[32],
                          uint8_t **bytes, size_t *bytes_len,
                          char *why, size_t why_size, int *skipped) {
    *skipped = 0;
    atomic_fetch_add(&g_fu_submit, 1);
    /* 弱引用场合缺异步实现 = 队列不可用：直接回落同步通道，与开关关闭同义。 */
    int src = (s2a_compile_worker_submit_async && s2a_compile_worker_poll && s2a_compile_worker_wait_async)
        ? s2a_compile_worker_submit_async(session, generation, json, len, sha, why, why_size) : 1;
    if (src == 1) {
        /* 短队列满：回落同步通道（等价旧路径），不算在途首用。 */
        atomic_fetch_add(&g_fu_fallback, 1);
        static _Atomic int nf;
        if (atomic_fetch_add(&nf, 1) < 8)
            draw_log("first-use: async queue full, falling back to synchronous compile");
        return nvk_worker_compile(session, generation, json, len, sha, bytes, bytes_len, why, why_size);
    }
    if (src < 0) return -1;   // worker 未起/参数问题：why 已写明
    const uint64_t frame = nvk_fu_frame();
    const uint32_t item_ms = nvk_fu_item_ms(), frame_ms = nvk_fu_frame_ms(), unsafe_ms = nvk_fu_unsafe_ms();
    const int may_skip = nvk_fu_skip_safe(state, frame);
    int late = 0, stuck = 0;
    nvk_fu_pend_marks(sha, &late, &stuck);
    for (;;) {
        uint8_t *rb = NULL;
        size_t rlen = 0;
        int prc = s2a_compile_worker_poll(sha, &rb, &rlen, why, why_size);
        if (prc == 0) {
            atomic_fetch_add(&g_fu_ready_n, 1);
            *bytes = rb; *bytes_len = rlen;
            return 0;
        }
        if (prc == 2) return -1;                    // 编译失败：why 已带编译器原因
        if (prc == 3) {                             // stop 清场/环挤掉：结果按丢失定性
            snprintf(why, why_size, "compile worker: first-use result lost (stopped or evicted)");
            return -1;
        }
        /* 在途：取一次判定（iOS pso_pend_take 同义）。 */
        int64_t left = s2a_fu_budget_left_ms(&g_fu_budget, frame, frame_ms);
        struct s2a_fu_verdict v = s2a_fu_wait_decide(0, late, stuck, may_skip,
                                                   item_ms, frame_ms, left, unsafe_ms);
        if (v.action == S2A_FU_SKIP) {
            nvk_fu_pend_mark(sha, v.timeout);
            *skipped = v.timeout;
            return 0;
        }
        /* v.wait_ms == 0 = iOS 语义上的无界等待：用分片轮询表达（切片不耗帧预算上限，
           实际等待仍全额记账）。 */
        const uint32_t slice = v.wait_ms ? v.wait_ms : 250;
        const uint64_t t0 = nvk_fu_now();
        int wrc = s2a_compile_worker_wait_async(sha, slice, &rb, &rlen, why, why_size);
        const uint64_t waited = nvk_fu_now() - t0;
        if (may_skip) s2a_fu_budget_spend(&g_fu_budget, frame, waited);
        atomic_fetch_add(&g_fu_wait_n, 1);
        atomic_fetch_add(&g_fu_wait_ns, waited);
        if (wrc == 0) {
            atomic_fetch_add(&g_fu_ready_n, 1);
            *bytes = rb; *bytes_len = rlen;
            return 0;
        }
        if (wrc == 2) return -1;
        if (wrc == 3) {
            snprintf(why, why_size, "compile worker: first-use result lost while waiting");
            return -1;
        }
        /* wrc 1 = 本片超时仍无结果：按判定表给的终局定性，打项标并跳过本笔。 */
        nvk_fu_pend_mark(sha, v.timeout);
        *skipped = v.timeout;
        return 0;
    }
}

// views 是调用方按 key 取得、并在整个编译期间 pin 住的快照（与 keys 同序）。
// 不再按 key 二次查找：那会把“编的这份”和“身份证明的那份”分开，源一换就编译到别的内容。
static _Atomic uint64_t g_vertex_feature_compile_rejects;
static void nvk_program_compile_inner(struct s2a_vk_device *device, struct nvk_cached_program *entry, const struct s2a_record_draw_state *state,
                                      const char *dir, const struct s2a_direct_compile_context *pc,
                                      const struct nvk_texture_set *tex,
                                      const struct s2a_source_entry_view *views, int *skipped) {
    (void)dir;   // 编译只读源快照/内存结果；诊断写 S2A_DIRECT_DIAG_DIR（F4.1），不写 shader 目录
    if (!views) {
        entry->failed = 1;
        snprintf(entry->why, sizeof entry->why, "没有源快照模块");
        return;
    }
    struct s2a_compile_request_input in;
    memset(&in, 0, sizeof in);

    in.session = pc->session;
    in.generation = pc->generation;
    in.profile_json = pc->profile_json;
    in.profile_bytes = pc->profile_bytes;
    in.direct_abi = (int)pc->direct_abi;
    in.translator_closure = pc->translator_closure;
    in.texture_array_length = state->tex_pool_n;
    in.sampler_array_length = state->samp_pool_n;
    in.transform_feedback_enabled = state->tf_enabled;
    nvk_fill_gfx(state, pc, &in.gfx, 1);
    char ident[64];
    int idoff = 0;
    for (unsigned st = 0, idx = 0; st < 6 && idx < 6; st++) {
        if (!(state->stages & (1u << st))) continue;
        const char *name = st == 0 ? "vs" : st == 1 ? "fs" : NULL;
        char khex[17];
        for (int b = 0; b < 8; b++) snprintf(khex + 2 * b, 3, "%02x", state->keys[idx][b]);
        if (!name) {
            entry->failed = 1;
            snprintf(entry->why, sizeof entry->why, "NVN stage %u 不在 vs/fs 首版契约（stages=0x%x key=%s）",
                     st, state->stages, khex);
            return;
        }
        const struct s2a_source_entry_view *pv = &views[idx];
        struct s2a_source_entry_view v = *pv;
        if (!v.complete || !v.code || !v.control) {
            entry->failed = 1;
            snprintf(entry->why, sizeof entry->why, "stage %s 源快照缺失（key=%s）", name, khex);
            return;
        }
        struct s2a_compile_stage_src *dst = &in.stages[in.stage_count++];
        dst->stage = name;
        const uint8_t *code = v.code;
        uint32_t code_bytes = v.code_bytes;
        if (code_bytes > 0x30 && code[0] == 0x78 && code[1] == 0x56 && code[2] == 0x34 && code[3] == 0x12) {
            code += 0x30;
            code_bytes -= 0x30;
        }
        dst->code = code; dst->code_bytes = code_bytes;
        dst->control = v.control; dst->control_bytes = v.control_bytes;
        dst->c1 = v.c1; dst->c1_bytes = v.c1_bytes;   // 真实捕获的常量段；未捕获时为空
        if (v.c1 && v.c1_bytes >= 0x100) {   // 结果条目自留一份：源快照淘汰后仍能绑 c1
            memcpy(entry->c1[st], v.c1, 0x100);
            entry->c1_ok |= (uint8_t)(1u << st);
        }
        dst->constant_buffer_use = state->ubo_use[st];
        if (!in.gfx.has_tess_mode && v.control_bytes >= 0x748) {
            memcpy(&in.gfx.tess_mode, v.control + 0x744, 4);
            in.gfx.has_tess_mode = 1;
        }
        idoff += snprintf(ident + idoff, sizeof ident - (size_t)idoff, "%s", khex);
        idx++;
    }
    if (in.stage_count != 2) {
        entry->failed = 1;
        snprintf(entry->why, sizeof entry->why, "stages=0x%x 不是完整 vs+fs（%d 个阶段）", state->stages, in.stage_count);
        return;
    }
    if (tex && tex->count) {
        memcpy(in.textures, tex->items, sizeof in.textures);
        in.texture_count = tex->count;
    }
    in.request_id = ident;
    char *json = NULL;
    size_t json_len = 0;
    char gaps[2048] = {0};
    if (s2a_compile_request_build(&in, &json, &json_len, gaps, sizeof gaps)) {
        entry->failed = 1;
        snprintf(entry->why, sizeof entry->why, "请求生成失败（输入不完整）");
        return;
    }
    {
        s2a_sha256 ctx;
        s2a_sha256_init(&ctx);
        s2a_sha256_update(&ctx, (const uint8_t *)json, json_len);
        s2a_sha256_final(&ctx, entry->request_sha);
    }
    draw_log("compile request: %zu B requestId=%s stages=0x%x topology=%s texPool=%u sampPool=%u tf=%d",
             json_len, ident, state->stages, in.gfx.topology ? in.gfx.topology : "?",
             state->tex_pool_n, state->samp_pool_n, state->tf_enabled);
    if (gaps[0]) draw_log("compile request 来源缺口：%s", gaps);
    const int export_open = s2a_compile_export_open();
    {
        char dump[PROP_VALUE_MAX] = {0};
        if (export_open && s2a_compile_export_external_open())
            __system_property_get("debug.s2a.dump_fail", dump);
        if (dump[0] == '/') {
            // 文件名带完整内容身份（input_sha = 规范输入 SHA-256），离线库的 capture 用同一
            // 64 hex 作 identity；两处 dump 成对，不靠 stage 短 key，也不会互相覆盖。
            char idhex[65];
            for (int b = 0; b < 32; b++) snprintf(idhex + 2 * b, 3, "%02x", entry->input_sha[b]);
            char path[1200];
            snprintf(path, sizeof path, "%s/%s-%s.compile-request.json", dump, ident, idhex);
            FILE *f = fopen(path, "wb");
            if (f) { size_t w = fwrite(json, 1, json_len, f); fclose(f); draw_log("compile request: dumped %s (%zu/%zu B)", path, w, json_len); }
        }
    }
    // F4.1：编译请求诊断写本局私有目录，不写安装 revision。目录不可用时只丢诊断，
    // 不影响随后正确的内存编译结果；绝不回落到 shader/revision 目录。
    if (export_open) {
        // 文件名必须带内容身份：同一 requestId 只是阶段 key 组，同一 key 组会有多个内容变体
        // （纹理事实/附件格式不同），只按 requestId 命名会让后来的覆盖前面的，事后无法 diff。
        const char *diag = getenv("S2A_DIRECT_DIAG_DIR");
        // 与 dump_fail / importer 一致：完整规范输入身份，不是 REQUEST 正文摘要。
        char idhex[65];
        for (int b = 0; b < 32; b++) snprintf(idhex + 2 * b, 3, "%02x", entry->input_sha[b]);
        char dname[160];
        int nname = snprintf(dname, sizeof dname, "%s-%s.compile-request.json", ident, idhex);
        int name_ok = nname >= 0 && (size_t)nname < sizeof dname;
        char dwhy[160] = "";
        if (!name_ok) snprintf(dwhy, sizeof dwhy, "诊断文件名过长或格式化失败");
        if (diag && diag[0] && name_ok && s2a_direct_diag_write(diag, dname, json, json_len, dwhy, sizeof dwhy) == 0) {
            draw_log("compile request: 已保存诊断 %s/%s", diag, dname);
        } else {
            static int noted;
            if (!noted++)
                draw_log("compile request: 诊断未保存（%s）；安装 revision 保持只读，编译结果不受影响",
                         diag && diag[0] ? dwhy : "没有本局私有诊断目录");
        }
    }
    if (export_open && s2a_compile_export_request(getenv("S2A_DIRECT_DIAG_DIR"),
            pc->session, pc->generation, pc->direct_abi, 1, pc->translator_closure,
            pc->profile_hash, ident, entry->input_sha, json, json_len))
        __android_log_print(ANDROID_LOG_ERROR, "S2Arm", "【红】唯一请求集发布失败");
    struct s2a_compile_outcome outcome;
    memset(&outcome, 0, sizeof outcome);
    // 计数与墙钟都只包住"真正调用编译器"这一段：源快照缺失/身份不符在更早返回，
    // 那些不是编译器调用，不能算进 compile_attempt（正式验收的 0→1→0 要按真实边界读）。
    uint64_t compile_t0 = pf_now();
    g_program_compile_attempts++;
    int ran;
    if (s2a_compile_worker_active()) {
        uint8_t *wbytes = NULL;
        size_t wlen = 0;
        char wwhy[512] = "";
        if (nvk_fu_on()) {
            /* C4a：在途提交 + 首用判定。*skipped>0 = 本笔安全跳过（entry 保持未失败、不入缓存）。 */
            ran = nvk_fu_compile(state, in.session, in.generation, json, json_len, entry->request_sha,
                                 &wbytes, &wlen, wwhy, sizeof wwhy, skipped);
        } else {
            ran = nvk_worker_compile(in.session, in.generation, json, json_len, entry->request_sha,
                                     &wbytes, &wlen, wwhy, sizeof wwhy);
        }
        if (*skipped) { free(json); free(wbytes); return; }   // 跳过的绘制：结果/失败都不落地
        if (!ran) {
            outcome.bytes = wbytes;
            outcome.bytes_len = wlen;
            snprintf(outcome.summary, sizeof outcome.summary, "worker compile ok（%zu B）", wlen);
        } else {
            snprintf(outcome.summary, sizeof outcome.summary, "%s", wwhy);
        }
    } else {
        ran = s2a_compile_request_execute(in.session, in.generation, json, json_len, &outcome);
    }
    {
        uint64_t cost = pf_now() - compile_t0;
        g_compile_count++;
        g_compile_ns_total += cost;
        if (cost > g_compile_ns_max) g_compile_ns_max = cost;
    }
    draw_log("compile outcome: %s", outcome.summary);
    free(json);
    if (ran || !outcome.bytes || !outcome.bytes_len) {
        if (export_open) {
            struct s2a_w2_query_receipt receipt;
            struct s2a_compile_export_w2_key key = { .session = in.session, .generation = in.generation };
            memcpy(key.input_sha, entry->input_sha, sizeof key.input_sha);
            int w2_status = s2a_compile_export_w2_diagnose(&key, &receipt);
            if (w2_status == S2A_COMPILE_EXPORT_W2_UNWIRED)
                __android_log_print(ANDROID_LOG_ERROR, "S2Arm", "【红】W2 单坐标诊断未接线：缺少冻结来源与原始坐标");
            else if (w2_status < 0)
                __android_log_print(ANDROID_LOG_ERROR, "S2Arm", "【红】W2 单坐标诊断失败：%d", w2_status);
        }
        entry->failed = 1;
        snprintf(entry->why, sizeof entry->why, "%s", outcome.summary);
        s2a_compile_outcome_free(&outcome);
        return;
    }
    if (!device->core_features.vertexPipelineStoresAndAtomics) {
        char storage_why[192] = "";
        if (nvn_vk_program_vertex_storage_safe(outcome.bytes, outcome.bytes_len, storage_why, sizeof storage_why)) {
            uint64_t count = atomic_fetch_add(&g_vertex_feature_compile_rejects, 1) + 1;
            __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                "【红】compile program %s missing vertexPipelineStoresAndAtomics: %s (rejected #%llu)",
                ident, storage_why, (unsigned long long)count);
            entry->failed = 1;
            snprintf(entry->why, sizeof entry->why, "program %.64s: %.120s", ident, storage_why);
            s2a_compile_outcome_free(&outcome);
            return;
        }
    }
    entry->bytes = outcome.bytes;
    entry->len = outcome.bytes_len;
    outcome.bytes = NULL;
    {
        // 内容身份在结果建立时确定：逐笔 draw 只传这 32 字节，避免每笔重算 50 KB。
        s2a_sha256 rctx; s2a_sha256_init(&rctx);
        s2a_sha256_update(&rctx, entry->bytes, entry->len);
        s2a_sha256_final(&rctx, entry->result_sha);
    }
    s2a_compile_outcome_free(&outcome);
    snprintf(entry->request_id, sizeof entry->request_id, "%s", ident);
    entry->session = in.session;
    entry->generation = in.generation;
    char dump[PROP_VALUE_MAX] = {0};
    if (export_open && s2a_compile_export_external_open())
        __system_property_get("debug.s2a.dump_fail", dump);
    if (dump[0] == '/') {
        // 与 compile-request 同名规则：<ident>-<64hex 内容身份>.program.json，capture 可直接配对。
        char idhex[65];
        for (int b = 0; b < 32; b++) snprintf(idhex + 2 * b, 3, "%02x", entry->input_sha[b]);
        char path[1200];
        snprintf(path, sizeof path, "%s/%s-%s.program.json", dump, ident, idhex);
        FILE *f = fopen(path, "wb");
        if (f) { size_t w = fwrite(entry->bytes, 1, entry->len, f); fclose(f);
                 draw_log("compile result: dumped %s (%zu/%zu B)", path, w, entry->len); }
    }
}

// F4.2：编译期间按 key 持有源引用，淘汰/清空不能释放正在读的 code/control。修复 reserve 只
// 引用到 dest 的指针，释放发生在请求 JSON 建好之后。
// 磁盘缓存命中：把已校验的结果字节按编译成功的同一套字段落地。
// request_id/session/generation 是传输身份，缓存里不存，这里留空并注明来源，
// 不能让复用路径看起来像“本局重新编过”。
// c1 是编译时从源快照抓的，不在结果字节里。磁盘命中跳过编译，必须自己补这一份，
// 否则复用路径绑定不到常量段（c1_ok 为 0，draw 处会跳过 c1 绑定）。
static void nvk_program_capture_c1(struct nvk_cached_program *entry,
                                   const struct s2a_record_draw_state *state) {
    if (!s2a_source_acquire || !state) return;
    for (unsigned st = 0, idx = 0; st < 6 && idx < 6; st++) {
        if (!(state->stages & (1u << st))) continue;
        struct s2a_source_entry_view v;
        memset(&v, 0, sizeof v);
        // 借用的视图在锁外可能已被淘汰：这里用 acquire/release 的同一代际，
        // 复制发生在引用有效期间。
        if (s2a_source_acquire(state->keys[idx], &v)) {
            if (v.c1 && v.c1_bytes >= 0x100) {
                memcpy(entry->c1[st], v.c1, 0x100);
                entry->c1_ok |= (uint8_t)(1u << st);
            }
            if (s2a_source_release) s2a_source_release(v.ref);
        }
        idx++;
    }
}

static void nvk_program_adopt_cached(struct nvk_cached_program *entry, uint64_t session,
                                     uint64_t generation, const uint8_t identity[32],
                                     uint8_t *bytes, size_t len, const uint8_t result_sha[32]) {
    entry->bytes = bytes;
    entry->len = len;
    memcpy(entry->result_sha, result_sha, 32);
    entry->session = session;
    entry->generation = generation;
    entry->from_disk = 1;
    memcpy(entry->input_sha, identity, 32);
    snprintf(entry->request_id, sizeof entry->request_id, "%s", "disk-cache");
    snprintf(entry->why, sizeof entry->why, "%s", "");
}

static void nvk_program_compile(struct s2a_vk_device *device, struct nvk_cached_program *entry, const struct s2a_record_draw_state *state,
                                const char *dir, const struct s2a_direct_compile_context *pc,
                                const struct nvk_texture_set *tex, const struct nvk_src_ids *ids, int *skipped) {
    struct s2a_source_entry_view views[6];
    uint64_t refs[6];
    int npins = 0;
    const char *fail = NULL;
    memset(views, 0, sizeof views);
    memset(refs, 0, sizeof refs);
    for (unsigned st = 0, idx = 0; st < 6 && idx < 6; st++) {
        if (!(state->stages & (1u << st))) continue;
        if (!s2a_source_acquire || !s2a_source_acquire(state->keys[idx], &views[idx]) || !views[idx].ref) {
            fail = "源快照缺失";
            break;
        }
        // 编译必须用身份里证明过的那一份内容：短 key 相同不等于内容相同。
        if (!ids || !(ids->present & (1u << idx)) || memcmp(views[idx].content_sha, ids->sha[idx], 32) ||
            views[idx].code_bytes != ids->code_bytes[idx] ||
            views[idx].control_bytes != ids->control_bytes[idx]) {
            s2a_source_release(views[idx].ref);
            views[idx].ref = 0;
            fail = "源快照内容与身份不符";
            break;
        }
        refs[npins++] = views[idx].ref;
        idx++;
    }
    if (fail) {
        entry->failed = 1;
        snprintf(entry->why, sizeof entry->why, "%s（stages=0x%x）", fail, state->stages);
    } else {
        nvk_program_compile_inner(device, entry, state, dir, pc, tex, views, skipped);
    }
    for (int i = 0; i < npins; i++)
        if (s2a_source_release) s2a_source_release(refs[i]);
}

// 生产命中入口的自检：用同一份生产事实/编码重算规范输入，与备忘指向条目保存的原文比较。
// 返回 0 = 等价；非 0 = 备忘命中了不同输入（这正是反例要抓的）。不修改缓存、不参与正常判据。
static __attribute__((noinline)) int nvk_id_verify_hit(const struct s2a_record_draw_state *state,
                                                      const struct s2a_direct_compile_context *pc,
                                                      const struct nvk_env *env,
                                                      const struct nvk_cached_program *hit) {
    struct s2a_compile_graphics_src vg;
    memset(&vg, 0, sizeof vg);
    nvk_fill_gfx(state, pc, &vg, 0);
    struct nvk_texture_set vtex;
    memset(&vtex, 0, sizeof vtex);
    if (state->stages) nvk_fill_textures(state, &vtex);
    uint8_t vbuf[S2A_INPUT_IDENTITY_MAX];   // 4 KB 栈（noinline 独立帧）；不用 static，避免多线程共用
    size_t vlen = 0;
    int rc = nvk_input_identity(state, pc, env, &vg, vtex.items, vtex.count, NULL, vbuf, &vlen, NULL);
    if (rc != 0) return rc;   // -1 越规格 / -2 缺源：自检也不能证明等价
    if (!hit->input || vlen != hit->input_len || memcmp(vbuf, hit->input, vlen)) return -3;
    return 0;
}

static struct nvk_cached_program *nvk_program_for_draw(struct s2a_vk_device *device, const struct s2a_record_draw_state *state, const char *dir, int *skipped);

static struct nvk_cached_program *nvk_program_for_draw_timed(struct s2a_vk_device *device, const struct s2a_record_draw_state *state, const char *dir, int *skipped) {
    if (!pf_stats_on()) return nvk_program_for_draw(device, state, dir, skipped);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    struct nvk_cached_program *r = nvk_program_for_draw(device, state, dir, skipped);
    clock_gettime(CLOCK_MONOTONIC, &b);
    uint64_t d = (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull + (uint64_t)(b.tv_nsec - a.tv_nsec);
    atomic_fetch_add_explicit(&g_pf_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_pf_ns, d, memory_order_relaxed);
    return r;
}

static struct nvk_cached_program *nvk_program_for_draw(struct s2a_vk_device *device, const struct s2a_record_draw_state *state, const char *dir, int *skipped) {
    struct s2a_direct_compile_context pc;
    memset(&pc, 0, sizeof pc);
    // engine 后端/诊断只编 WSI 探针时没有 provider；没有编译器就不产生 compile context，
    // 不要用返回成功的空 provider 顶链接。
    extern int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *out) __attribute__((weak));
    int have_pc = s2a_direct_provider_compile_context && s2a_direct_provider_compile_context(&pc) == 0;
    // 备忘查在一切重算之前：命中就直接交回同一条目，纹理事实/身份编码/缓存扫描都不做。
    struct nvk_env env;
    memset(&env, 0, sizeof env);
    int have_env = 0;
    if (have_pc) {
        nvk_env_fill(&pc, &env);
        have_env = env.have_profile;
        if (have_env) {
            // 环境里的 caps 决定片元输出类型是否参与特化；开关与日志留在绘制侧，
            // 让编译事实模块保持纯 C（宿主 CTest 要能链）。
            {
                char b[PROP_VALUE_MAX] = {0};
                __system_property_get("debug.s2a.fragout_keep", b);
                if (b[0] && atoi(b)) env.frag_out_specialization = 1;
            }
            nvk_compile_facts_set_frag_out_specialization(env.frag_out_specialization);
            static int noted;
            if (!noted++)
                draw_log("compile facts: needsFragmentOutputSpecialization=%d → 身份%s按片元输出类型区分",
                         env.frag_out_specialization, env.frag_out_specialization ? "" : "不");
        }
    }
    struct nvk_id_key idkey;
    int have_key = 0;
    if (have_env) {
        nvk_id_key_fill(&idkey, state, &pc, &env);
        have_key = 1;
        atomic_fetch_add_explicit(&g_pf_memo_calls, 1, memory_order_relaxed);
        g_idmemo_hash = nvk_id_memo_hash(&idkey, state);
        const unsigned memo_base = (unsigned)(g_idmemo_hash & (NVK_IDMEMO - 1));
        struct nvk_id_memo *memo = idmemo_slots();
        int memo_same_hash = 0;
        for (unsigned k = 0; memo && k < NVK_IDMEMO_PROBE; k++) {
            struct nvk_id_memo *m = &memo[(memo_base + k) & (NVK_IDMEMO - 1)];
            if (!m->serial || m->hash != g_idmemo_hash) continue;
            memo_same_hash = 1;
            if (!nvk_id_memo_match(m, &idkey, state)) continue;
            struct nvk_cached_program *hit = &g_programs[m->idx];
            // 槽被淘汰（代号清零）或被复用（新代号）时当作没命中，重新走完整判据。
            if (!hit->key[0] || hit->serial != m->serial) continue;
            if (nvk_id_verify_enabled()) {
                int vrc = nvk_id_verify_hit(state, &pc, &env, hit);
                atomic_fetch_add_explicit(&g_pf_memo_verify_calls, 1, memory_order_relaxed);
                if (vrc) {
                    atomic_fetch_add_explicit(&g_pf_memo_verify_bad, 1, memory_order_relaxed);
                    static unsigned noted;
                    if (noted++ < 4)
                        draw_log("P1 自检失败：备忘命中但完整身份不同（rc=%d key=%s），本次改用完整判据",
                                 vrc, hit->key);
                    continue;
                }
            }
            hit->last_use = ++g_program_clock;
            hit->users++;
            g_program_hits++;
            g_program_lookups++;
            atomic_fetch_add_explicit(&g_pf_memo_hits, 1, memory_order_relaxed);
            nvk_program_stats();
            s2a_w2_shader_lookup(&pc, hit->input_sha, S2A_W2_MEMO, !hit->failed && hit->bytes);
            return hit;
        }
        if (memo_same_hash) atomic_fetch_add_explicit(&g_pf_memo_miss_diff, 1, memory_order_relaxed);
        else atomic_fetch_add_explicit(&g_pf_memo_miss_nokey, 1, memory_order_relaxed);
    }
    struct s2a_compile_graphics_src gfx;
    memset(&gfx, 0, sizeof gfx);
    nvk_fill_gfx(state, &pc, &gfx, 0);
    char key[144];
    nvk_program_display_key(state, key);
    struct nvk_texture_set tex;
    memset(&tex, 0, sizeof tex);
    uint64_t t_tex0 = pf_now();
    if (state->stages) nvk_fill_textures(state, &tex);
    atomic_fetch_add_explicit(&g_pf_tex_ns, pf_now() - t_tex0, memory_order_relaxed);
    uint8_t inbuf[S2A_INPUT_IDENTITY_MAX];
    size_t inlen = 0;
    uint8_t insha[32] = {0};   // 只在 miss 时算
    struct nvk_src_ids srcids;
    memset(&srcids, 0, sizeof srcids);
    uint64_t t_id0 = pf_now();
    int idrc = have_env ? nvk_input_identity(state, &pc, &env, &gfx, tex.items, tex.count, &srcids,
                                             inbuf, &inlen, NULL)
                        : -1;
    int have_id = idrc == 0;
    atomic_fetch_add_explicit(&g_pf_id_ns, pf_now() - t_id0, memory_order_relaxed);
    const uint64_t fnh = have_id ? nvk_fnv1a(inbuf, inlen) : 0;
    // 没有内容身份就绝不做缓存查找：短 key 只是阶段 key 组，相同 key 可以有不同代码/事实/profile，
    // 按它命中就是“不同内容、同一身份”的误命中。这里明确失败，不退回 key-only 兜底。
    // 合法实现里 encode 不会失败（纹理事实的名字只来自已映射表），失败即输入越规格。
    if (!have_id) {
        atomic_fetch_add(&g_id_unavailable, 1);
        static unsigned noted_src, noted_enc;
        if (idrc == -2 && noted_src++ < 4) {
            char mk[17] = "-";
            const char *stn = "?";
            if (srcids.missing_stage != 0xff) {
                nvk_hex8(mk, srcids.missing_key);
                if (srcids.missing_stage == 0) stn = "VS";
                else if (srcids.missing_stage == 1) stn = "FS";
                else { char other[8]; snprintf(other, sizeof other, "stage%u", srcids.missing_stage); stn = other; }
            }
            draw_log("program cache: 缺完整源快照，本次绘制不做缓存查找（key=%s stages=0x%x 源拒绝=%llu；"
                     "缺 stage=%s key=%s）",
                     key, state->stages, (unsigned long long)(s2a_source_rejected ? s2a_source_rejected() : 0),
                     stn, mk);
            {
                struct s2a_source_pin_stats ps;
                uint32_t se = 0, sb = 0, sd = 0;
                s2a_source_pin_stats(&ps);
                s2a_source_stats(&se, &sb, &sd);
                draw_log("源快照 pin（缺源时）：pinned_entries=%u pins=%llu pin_calls=%llu pin_misses=%llu "
                         "unpins=%llu evict_skipped=%llu | entries=%u bytes=%u dropped=%llu",
                         ps.pinned_entries, (unsigned long long)ps.pins, (unsigned long long)ps.pin_calls,
                         (unsigned long long)ps.pin_misses, (unsigned long long)ps.unpins,
                         (unsigned long long)ps.evict_skipped, se, sb, (unsigned long long)sd);
            }
        } else if (idrc != -2 && noted_enc++ < 4) {
            draw_log("program cache: 内容身份编码失败，本次绘制不做缓存查找（key=%s stages=0x%x tex=%d）",
                     key, state->stages, tex.count);
        }
        return NULL;   // 不记 lookup：这次没有做缓存查找，计数要能区分“查了没中”和“没查”
    }
    g_program_lookups++;
    uint64_t t_scan0 = pf_now();
    for (unsigned i = 0; i < g_program_slots; i++) {
        struct nvk_cached_program *e = &g_programs[i];
        if (!e->key[0]) continue;
        if (have_pc && (e->session != pc.session || e->generation != pc.generation)) continue;
        // 命中 = 完整内容身份一致，且逐字节复核规范输入（不能只看短 key/哈希）。
        // 预筛用 64 位哈希 + 长度，判据仍然是原文逐字节相等（比只比 SHA 更强）。
        int same = (e->input && e->input_len == inlen && e->input_hash == fnh &&
                    !memcmp(e->input, inbuf, inlen));
        if (!same) continue;
        e->last_use = ++g_program_clock;
        e->users++;
        g_program_hits++;
        if (have_key) atomic_fetch_add_explicit(&g_pf_memo_miss_cached, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_pf_scan_ns, pf_now() - t_scan0, memory_order_relaxed);
        if (have_key) idmemo_store(&idkey, state, e->serial, (unsigned)(e - g_programs));
        nvk_program_stats();
        s2a_w2_shader_lookup(&pc, e->input_sha, S2A_W2_MEMORY, !e->failed && e->bytes);
        return e;
    }
    atomic_fetch_add_explicit(&g_pf_scan_ns, pf_now() - t_scan0, memory_order_relaxed);
    g_program_misses++;
    nvk_sha256_bytes(inbuf, inlen, insha);   // 只有 miss 需要内容身份（存条目 / 磁盘缓存键）
    struct nvk_cached_program tmp;
    enum s2a_w2_source observation_source = S2A_W2_UNKNOWN;
    memset(&tmp, 0, sizeof tmp);
    snprintf(tmp.key, sizeof tmp.key, "%s", key);
    // inlen 由 S2A_INPUT_IDENTITY_MAX(4096) 界定，uint16_t 不会截断；截断会让复核读不到尾部。
    if (inlen > 0xFFFFu) return NULL;
    memcpy(tmp.input_sha, insha, 32);
    tmp.input_hash = nvk_fnv1a(inbuf, inlen);
    tmp.input_len = (uint16_t)inlen;
    if (!have_pc) {
        tmp.failed = 1;
        snprintf(tmp.why, sizeof tmp.why, "provider context 不存在（只有 direct nvn_vk 会话才有）");
    } else {
        const char *cdir = getenv("S2A_DIRECT_CACHE_DIR");
        // 诊断：debug.s2a.no_disk_cache=1 跳过私有磁盘缓存，强制本局重新编译；
        // 离线库 capture 需要成对的 request/result，热缓存会让它们根本不产生。
        int no_disk = 0;
        {
            static int v = -1;
            if (v < 0) {
                char b[PROP_VALUE_MAX] = {0};
                __system_property_get("debug.s2a.no_disk_cache", b);
                v = (b[0] && atoi(b) != 0);
            }
            no_disk = v;
        }
        uint8_t *cbytes = NULL;
        size_t clen = 0;
        uint8_t csha[32];
        char cwhy[192] = "";
        // 来源顺序固定：内存命中（上面已返回）→ 包内只读离线库 → 私有缓存 → 现编。
        // 包内库是出包产物（随 revision 可复现），私有缓存只是本机副产物；反过来查会让
        // 首装命中包内库这件事被本机旧缓存遮住，也使 W2.8 的"首装零现编"不可判定。
        int offline_used = 0;
        // 离线库与私有缓存是两条独立的只读来源：no_disk_cache 只跳过私有缓存（capture 仍要
        // 产生 request/result），要连离线库一起跳过用 no_offline。
        int no_offline = 0;
        {
            static int v = -1;
            if (v < 0) {
                char b[PROP_VALUE_MAX] = {0};
                __system_property_get("debug.s2a.no_offline", b);
                v = (b[0] && atoi(b) != 0);
            }
            no_offline = v;
        }
        // 诊断：debug.s2a.offline_root=<绝对目录> 把只读库根换到外部目录，用于在
        // 不产生新 revision 的前提下真机验证“装库后命中/漏项补编/次局命中”。
        // prop_root 必须与 rev_root 同作用域：放到内层块会让 rev_root 悬垂，
        // 后续局部（owhy 等）复用栈时把库根覆盖成错误文本。
        char prop_root[PROP_VALUE_MAX] = {0};
        const char *rev_root = getenv("S2A_SHADER_REVISION_ROOT");
        if (__system_property_get("debug.s2a.offline_root", prop_root) > 0 && prop_root[0] == '/')
            rev_root = prop_root;
        if (!no_offline && rev_root && rev_root[0] == '/' && pc.profile_hash && pc.profile_hash[0]) {
            struct s2a_offline_request oreq;
            memset(&oreq, 0, sizeof oreq);
            memcpy(oreq.identity, insha, 32);
            oreq.direct_abi = pc.direct_abi;
            oreq.profile_hash = pc.profile_hash;
            oreq.profile_declared = pc.offline_profile_available;
            struct s2a_offline_program oprog;
            memset(&oprog, 0, sizeof oprog);
            char owhy[192] = "";
            int orc = s2a_offline_library_fetch(rev_root, &oreq, &oprog, owhy, sizeof owhy);
            // 容器摘要只证明字节没坏，不证明内容可消费：采纳前先过同一套解析。
            // 不可解析按"离线库拒绝"处理（一次计数、落到私有盘/现编），不能先算成功再在消费阶段炸。
            int valid = 0;
            if (orc == S2A_OFFLINE_HIT) {
                char vwhy[192] = "";
                valid = nvn_vk_first_draw_program_valid(oprog.bytes, oprog.len, vwhy, sizeof vwhy);
                if (!valid) {
                    free(oprog.bytes);
                    oprog.bytes = NULL;
                    oprog.len = 0;
                    snprintf(owhy, sizeof owhy, "离线条目不可解析：%s", vwhy);
                }
            }
            if (orc == S2A_OFFLINE_HIT && valid) {
                nvk_program_adopt_cached(&tmp, pc.session, pc.generation, insha,
                                         oprog.bytes, oprog.len, oprog.result_sha);
                tmp.from_disk = 2;   // 2 = 只读离线库命中（不写回私有缓存）
                snprintf(tmp.request_id, sizeof tmp.request_id, "%s", "offline-lib");
                nvk_program_capture_c1(&tmp, state);
                g_program_offline_hits++;
                offline_used = 1;
                observation_source = S2A_W2_OFFLINE;
                draw_log("program cache: offline hit %zu B identity=%02x%02x%02x%02x profile=%s",
                         oprog.len, insha[0], insha[1], insha[2], insha[3], pc.profile_hash);
            } else if (orc == S2A_OFFLINE_REJECT || (orc == S2A_OFFLINE_HIT && !valid)) {
                g_program_offline_rejects++;
                static _Atomic int nr;
                int n = atomic_fetch_add(&nr, 1);
                if (n < 4)
                    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                                        "first draw: offline library: rejected entry "
                                        "identity=%02x%02x%02x%02x root=%s (%s); falling back to compile",
                                        insha[0], insha[1], insha[2], insha[3], rev_root, owhy);
                else
                    draw_log("offline library: rejected entry for identity=%02x%02x%02x%02x (%s); "
                             "falling back to compile", insha[0], insha[1], insha[2], insha[3], owhy);
            } else {
                g_program_offline_misses++;
                static _Atomic int nm;
                if (atomic_fetch_add(&nm, 1) < 4)
                    draw_log("offline library: miss root=%s abi=%u profile=%s identity=%02x%02x%02x%02x (%s)",
                             rev_root, pc.direct_abi, pc.profile_hash,
                             insha[0], insha[1], insha[2], insha[3], owhy);
            }
        }
        if (!offline_used && !no_disk && cdir && *cdir &&
            s2a_program_cache_load(cdir, insha, &cbytes, &clen, csha, cwhy, sizeof cwhy) == 0) {
            nvk_program_adopt_cached(&tmp, pc.session, pc.generation, insha, cbytes, clen, csha);
            nvk_program_capture_c1(&tmp, state);
            g_program_disk_hits++;
            observation_source = S2A_W2_PRIVATE;
            draw_log("program cache: disk hit %zu B key=%s identity=%02x%02x%02x%02x",
                     clen, key, insha[0], insha[1], insha[2], insha[3]);
        } else if (!offline_used) {
            observation_source = S2A_W2_LIVE;
            nvk_program_compile(device, &tmp, state, dir, &pc, &tex, &srcids, skipped);
            if (skipped && *skipped) {
                /* 首用安全跳过：条目不落缓存（没有失败，也没有结果），本次绘制由调用方分流。 */
                free(tmp.bytes);
                return NULL;
            }
            if (tmp.failed) g_program_compile_failures++;
            else g_program_compile_success++;
            // 方案 §5.5.2 第 5 条：当前绘制不等磁盘。写失败只记缓存问题，不影响已可用的内存结果。
            if (cdir && *cdir && !tmp.failed && tmp.bytes && tmp.len)
                s2a_program_cache_store_async(cdir, insha, tmp.bytes, tmp.len);
        }
    }
    s2a_w2_shader_lookup(&pc, tmp.input_sha, observation_source, !tmp.failed && tmp.bytes);
    struct nvk_cached_program *slot = nvk_program_reserve(tmp.len);
    if (!slot) {
        draw_log("program cache: no room for %s (%zu B, resident %zu/%u B); not dropping silently",
                 tmp.key, tmp.len, g_program_bytes, (unsigned)(NVK_PROGRAM_BUDGET >> 20));
        free(tmp.bytes);
        return NULL;
    }
    *slot = tmp;
    if (slot->bytes) g_program_bytes += slot->len;
    {
        // 只在这条唯一成功出口上分配：tmp 的其它早退路径都不持有堆内存。
        slot->input = (uint8_t *)malloc(inlen ? inlen : 1u);
        if (slot->input) {
            memcpy(slot->input, inbuf, inlen);
        } else {
            // 分配失败：条目不建立身份副本 ⇒ 永不命中（本笔仍可用已拿到的结果），不能静默短拷。
            slot->input_len = 0; slot->input_hash = 0;
            static unsigned noted;
            if (noted++ < 4) draw_log("program cache: 身份副本分配失败，该条目不会被后续命中（%zu B）", inlen);
        }
    }
    if (!slot->failed && slot->bytes) {
        // 只有在“身份不同”这条路径上才会走到这里，所以同一 result_sha 出现就是“键多了一个无用维度”。
        atomic_fetch_add_explicit(&g_program_same_result_checked, 1, memory_order_relaxed);
        for (unsigned i = 0; i < g_program_slots; i++) {
            struct nvk_cached_program *e = &g_programs[i];
            if (e == slot || !e->key[0] || e->failed || !e->bytes || e->len != slot->len) continue;
            if (memcmp(e->result_sha, slot->result_sha, 32)) continue;
            {
                // 两份身份对应同一份产物：如果顶点属性元组只是顺序不同（多重集相同），
                // 那每对位置互换就是一个“格式对等价”的实证；不是置换却产物相同，则单独计数报出来
                // （意味着还有别的维度在起作用，不能据此归一化）。
                int diff = 0;
                uint32_t ca[32], cb[32];
                memcpy(ca, e->vattr_fmt, sizeof ca); memcpy(cb, slot->vattr_fmt, sizeof cb);
                for (int q = 0; q < 32; q++) if (ca[q] != cb[q]) diff = 1;
                if (diff) {
                    // 排序后比较多重集
                    for (int x = 0; x < 32; x++)
                        for (int y = x + 1; y < 32; y++) {
                            if (ca[x] > ca[y]) { uint32_t t = ca[x]; ca[x] = ca[y]; ca[y] = t; }
                            if (cb[x] > cb[y]) { uint32_t t = cb[x]; cb[x] = cb[y]; cb[y] = t; }
                        }
                    if (!memcmp(ca, cb, sizeof ca)) {
                        atomic_fetch_add_explicit(&g_attr_perm_ok, 1, memory_order_relaxed);
                        for (int q = 0; q < 32; q++)
                            if (e->vattr_fmt[q] != slot->vattr_fmt[q])
                                attr_pair_note(e->vattr_fmt[q], slot->vattr_fmt[q]);
                    } else {
                        atomic_fetch_add_explicit(&g_attr_perm_bad, 1, memory_order_relaxed);
                    }
                }
            }
            atomic_fetch_add_explicit(&g_program_same_result, 1, memory_order_relaxed);
            {
                // 归因：两串规范输入的长度、首个不同字节、不同区间的段数（有界打印）。
                // 长度不等 ⇒ 差异落在变长字段（字符串/纹理条数）；等长少量不同 ⇒ 定长字段。
                // 每窗口最多 4 条：早期只打“前 8 条”，等某个维度修掉后剩下的维度就再也看不见了。
                static _Atomic int noted;
                if (atomic_fetch_add(&noted, 1) % 64 < 4 && e->input && slot->input) {
                    size_t la = e->input_len, lb = slot->input_len, n = la < lb ? la : lb;
                    size_t first = n;
                    for (size_t k = 0; k < n; k++) if (e->input[k] != slot->input[k]) { first = k; break; }
                    int runs = 0;
                    for (size_t k = 0; k < n; k++) {
                        int d = e->input[k] != slot->input[k];
                        if (d && (k == 0 || e->input[k - 1] == slot->input[k - 1])) runs++;
                    }
                    char ta[48] = {0}, tb[48] = {0};
                    for (size_t k = 0; k < 40 && first + k < la && k < sizeof ta - 1; k++) {
                        uint8_t c = e->input[first + k];
                        ta[k] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
                    }
                    for (size_t k = 0; k < 40 && first + k < lb && k < sizeof tb - 1; k++) {
                        uint8_t c = slot->input[first + k];
                        tb[k] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
                    }
                    draw_log("program cache: same result, different identity: len %zu vs %zu first_diff=%zu runs=%d "
                             "A[%zu]=\"%s\" B[%zu]=\"%s\"",
                             la, lb, first, runs, first, ta, first, tb);
                }
            }
            break;
        }
    }
    slot->last_use = ++g_program_clock;
    slot->users = 1;
    memcpy(slot->vattr_fmt, state->vattr_fmt, sizeof slot->vattr_fmt);
    slot->serial = ++g_entry_serial;   // 新代号：备忘里存的是旧代号的条目一律作废
    if (have_key) idmemo_store(&idkey, state, slot->serial, (unsigned)(slot - g_programs));
    if (slot->bytes && !slot->failed && !slot->from_disk) {
        // 只对“本局真编出来”的条目判重编：disk 采纳的条目没有编译请求，request_sha 保持全 0，
        // 拿它去比对历史会让每个 disk 命中都被记成一次重编（历史上就是这样把 180 个采纳记成 180 次重编）。
        if (nvk_req_seen(slot->request_sha)) {
            g_program_recompiles++;
            if (g_program_recompiles <= 16)
                draw_log("program cache: identical full request recompiled (#%llu) key=%s request=%s",
                         (unsigned long long)g_program_recompiles, slot->key, slot->request_id);
        }
        nvk_req_note(slot->request_sha);
    }
    nvk_program_stats();
    return slot;
}

static void draw_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, "S2Arm", fmt, ap);
    va_end(ap);
}

struct cache_entry {
    char cache_key[6 * 17 + 1];
    char identity[65];
    int valid;
    struct nvn_vk_program program;
};

static struct cache_entry g_cache[NVK_PROGRAM_CACHE];
static uint32_t g_loaded, g_hits, g_failed;

static int nvk_draw_builder_impl(struct s2a_vk_device *device, const struct s2a_record_draw_state *state,
                     struct s2a_record_result *result, void *opaque) {
    const uint64_t dbg_t0 = pf_now();
    // 后段残差开关跟随 first_draw 的同一帧闸门（debug.s2a.draw_tail + 抽样）；关闭时只多一次
    // 弱符号调用 + 判断，不取钟、不累加。
    const int tail_on = (nvn_vk_first_draw_tail_on && nvn_vk_first_draw_tail_on()) ? 1 : 0;
    const prog_t *p = state ? (const prog_t *)state->program : NULL;
    static const uint8_t empty_keys[6][8];
    const uint8_t (*keys)[8] = state ? state->keys : empty_keys;
    uint32_t stages = state ? state->stages : 0;
    int indexed = state ? state->indexed : 0;
    const char *dir = (const char *)opaque;
    if (!device || !p || !state) {
        result->code = S2A_RECORD_ARGUMENT;
        snprintf(result->why, sizeof result->why, "draw: no device/program");
        return S2A_RECORD_ARGUMENT;
    }
    // keys 是 prog_t.sd[] 的数组顺序，stages 是 NVN stage 掩码（bit1 = FS）；两者由加载器按
    // “第 i 个置位 ↔ keys[i]”配对，并由 program 结果里的 stage 字符串校验。这里不做二次推测。
    if (!stages) {
        result->code = S2A_RECORD_UNSUPPORTED;
        snprintf(result->why, sizeof result->why,
                 "draw: NVN BindProgram stages mask is 0 (serial %llu nstage=%d key0=%02x%02x%02x%02x%02x%02x%02x%02x)",
                 (unsigned long long)p->serial, (int)p->nstage,
                 keys[0][0], keys[0][1], keys[0][2], keys[0][3], keys[0][4], keys[0][5], keys[0][6], keys[0][7]);
        g_failed++;
        return S2A_RECORD_UNSUPPORTED;
    }
    static int logged;
    if (!logged) {
        logged = 1;
        char keyhex[17], keyhex1[17] = "-";
        for (int b = 0; b < 8; b++) snprintf(keyhex + 2 * b, 3, "%02x", keys[0][b]);
        if (stages & 0x2) for (int b = 0; b < 8; b++) snprintf(keyhex1 + 2 * b, 3, "%02x", keys[1][b]);
        if (s2a_source_get_key) {
            for (unsigned st = 0, idx = 0; st < 6 && idx < 6; st++) {
                if (!(stages & (1u << st))) continue;
                char sk[17];
                for (int b = 0; b < 8; b++) snprintf(sk + 2 * b, 3, "%02x", keys[idx][b]);
                struct s2a_source_entry_view v;
                memset(&v, 0, sizeof v);
                if (s2a_source_get_key(keys[idx], &v))
                    draw_log("draw 源快照 stage=%u key=%s code=%u ctl=%u complete=%d", st, sk,
                             v.code_bytes, v.control_bytes, v.complete);
                else
                    draw_log("draw 源快照 stage=%u key=%s 缺（已淘汰或未记录真值）", st, sk);
                idx++;
            }
        }
        draw_log("draw 快照：key0=%s key1=%s stages=0x%x indexed=%d count=%u first=%u instances=%u rt=%u(rt0 fmt=%d %ux%u) vp=%d "
             "scissor=%d,%d,%d,%d tex=%d@%d ubo=%d:%llx+%llu vb=%d",
             keyhex, keyhex1, stages, indexed, state->count, state->first, state->instances, state->rt_count,
             state->rt_count ? (int)state->rt[0].format : 0, state->rt_count ? state->rt[0].width : 0,
             state->rt_count ? state->rt[0].height : 0, state->vp_n, state->sc_x, state->sc_y, state->sc_w, state->sc_h,
             state->tex_n, state->tex_first, state->ubo_index, (unsigned long long)state->ubo_addr,
             (unsigned long long)state->ubo_size, state->vb_n);
    }
    if (!dir || !dir[0]) {
        result->code = S2A_RECORD_ARGUMENT;
        snprintf(result->why, sizeof result->why, "draw: shader directory is empty (S2A_SHADER_DIR)");
        g_failed++;
        return S2A_RECORD_ARGUMENT;
    }
    // cache_key 只被下面那条（当前走不到的）旧加载路径消费：不再逐笔拼，放到用它的地方算。
    const uint64_t dbg_t1 = pf_now();
    atomic_fetch_add_explicit(&g_dbg_pre_ns, dbg_t1 - dbg_t0, memory_order_relaxed);
    int fu_skipped = 0;
    struct nvk_cached_program *prog = nvk_program_for_draw_timed(device, state, dir, &fu_skipped);
    const uint64_t dbg_t2 = pf_now();
    if (!prog && fu_skipped) {
        /* C4a 安全跳过：本笔不画，但记录继续（返回 0 = 本 op 已处理，不是失败）。
           与 UNSUPPORTED 分账：跳过类别如实入 first-use 计数与日志，不静默兜底。 */
        if (fu_skipped == S2A_FU_T_BUDGET) atomic_fetch_add(&g_fu_skip_budget, 1);
        else if (fu_skipped == S2A_FU_T_LATE) atomic_fetch_add(&g_fu_skip_late, 1);
        else if (fu_skipped == S2A_FU_T_STUCK) atomic_fetch_add(&g_fu_skip_stuck, 1);
        static _Atomic int nf;
        if (atomic_fetch_add(&nf, 1) < 16 || (atomic_load(&nf) & 0xFF) == 0)
            draw_log("first-use draw skipped (%s): program still compiling in flight; "
                     "draw not recorded as unsupported", nvk_fu_class(fu_skipped));
        nvk_fu_stats_log();
        result->code = S2A_RECORD_OK;
        snprintf(result->why, sizeof result->why, "first-use skip (%s)", nvk_fu_class(fu_skipped));
        return S2A_RECORD_OK;
    }
    if (!prog) {
        result->code = S2A_RECORD_LIMIT;
        snprintf(result->why, sizeof result->why, "draw: program cache unavailable");
        g_failed++;
        return S2A_RECORD_LIMIT;
    }
    // c1 常量段（同 key 的 control−0x100，cbuf 1 运行时绑定用）。key 是内容身份，同一 key 的 c1
    // 必然相同，所以命中路径直接用结果条目建立时抓到的那一份，不再逐笔去源快照取两次。
    // 只有条目没有的阶段（建立时快照已淘汰）才回源快照补，行为与原来一致。
    if (!prog->failed) {
        struct s2a_record_draw_state *ms = (struct s2a_record_draw_state *)state;
        const uint64_t c1_fill0 = (tail_on && dbg_t2) ? nvk_tail_now() : 0;
        for (unsigned st = 0; st < 2; st++)
            if ((prog->c1_ok & (1u << st)) && !ms->c1_ok[st]) {
                memcpy(ms->c1[st], prog->c1[st], 0x100);
                ms->c1_ok[st] = 1;
            }
        if (c1_fill0) atomic_fetch_add_explicit(&g_c1_fill_ns, nvk_tail_now() - c1_fill0, memory_order_relaxed);
        int need = 0;
        for (unsigned st = 0, idx = 0; st < 6 && idx < 6; st++) {
            if (!(stages & (1u << st))) continue;
            if (!ms->c1_ok[st] && s2a_source_acquire) {
                struct s2a_source_entry_view v;
                memset(&v, 0, sizeof v);
                const uint64_t acq0 = tail_on ? nvk_tail_now() : 0;
                if (!need) need = 1;
                int held = s2a_source_acquire(state->keys[idx], &v);
                if (held && v.c1 && v.c1_bytes >= 0x100) {
                    memcpy(ms->c1[st], v.c1, 0x100);
                    ms->c1_ok[st] = 1;
                    // 回填到程序条目：同一内容身份的 c1 必然相同，下一次绘制不再回源快照。
                    // （磁盘采纳/预建的程序条目建立时没有状态，不写回就会每笔都走 acquire。）
                    if (st < 2) {   // 程序条目的 c1 只有 VS/FS 两段；NVN stage 索引可到 5，越界会踩坏相邻内存
                        memcpy(prog->c1[st], v.c1, 0x100);
                        prog->c1_ok |= (uint8_t)(1u << st);
                        if (acq0) atomic_fetch_add_explicit(&g_c1_wb_n, 1, memory_order_relaxed);
                    }
                }
                // 只释放这次 acquire 的那一代（同 key 重新登记会得到新令牌）。
                if (held && s2a_source_release) s2a_source_release(v.ref);
                if (acq0) {
                    atomic_fetch_add_explicit(&g_c1_acq_ns, nvk_tail_now() - acq0, memory_order_relaxed);
                    atomic_fetch_add_explicit(&g_c1_acq_n, 1, memory_order_relaxed);
                    if (held) atomic_fetch_add_explicit(&g_c1_acq_held, 1, memory_order_relaxed);
                }
            }
            idx++;
        }
        if (need && tail_on) atomic_fetch_add_explicit(&g_c1_need_n, 1, memory_order_relaxed);
    }
    if (prog->failed) {
        snprintf(result->why, sizeof result->why, "draw: program not available: %s", prog->why);
        nvk_program_release(prog);   // 失败条目也可能被淘汰，先交还引用再返回
        result->code = S2A_RECORD_UNSUPPORTED;
        g_failed++;
        return S2A_RECORD_UNSUPPORTED;
    }
    // c1 回填结束（draw_tail，默认关）：起点复用 dbg_t2，只补一个终点。
    if (tail_on && dbg_t2) {
        uint64_t c1_end = nvk_tail_now();
        atomic_fetch_add_explicit(&g_dbg_c1_ns, c1_end - dbg_t2, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_dbg_c1_n, 1, memory_order_relaxed);
    }
    {
        char why[256] = "";
        int rc = nvn_vk_first_draw(device, state, prog->bytes, prog->len, prog->result_sha, prog->input_sha,
                                   prog->session, prog->generation, prog->request_id,
                                   why, sizeof why);
        const uint64_t dbg_rel0 = tail_on ? nvk_tail_now() : 0;   // release 起点（draw_tail）
        nvk_program_release(prog);   // 结果字节在 pin 期间不会被淘汰
        if (rc) {
            result->code = S2A_RECORD_UNSUPPORTED;
            snprintf(result->why, sizeof result->why, "draw: 真实绘制未完成：%s", why);
            g_failed++;
            return S2A_RECORD_UNSUPPORTED;
        }
        {
            const uint64_t dbg_t3 = pf_now();
            if (dbg_rel0 && dbg_t3) {
                atomic_fetch_add_explicit(&g_dbg_rel_ns, dbg_t3 - dbg_rel0, memory_order_relaxed);
                atomic_fetch_add_explicit(&g_dbg_rel_n, 1, memory_order_relaxed);
            }
            atomic_fetch_add_explicit(&g_dbg_tail_ns, dbg_t3 - dbg_t2, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_dbg_builder_ns, dbg_t3 - dbg_t0, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_dbg_builder_n, 1, memory_order_relaxed);
        }
        result->code = S2A_RECORD_OK;
        snprintf(result->why, sizeof result->why, "%s", why);
        nvk_fu_rt_drawn(state, nvk_fu_frame());   // C4a：目标本帧又画过（iOS rt_seen_put）
        g_loaded++;
        if (g_loaded == 1) nvk_source_pin_log("draw");
        if (why[0]) draw_log("draw: %s", why);   // 正常模式不逐笔打空摘要
        return S2A_RECORD_OK;
    }
    if (dm_shader_survey_flush) (void)dm_shader_survey_flush();
    char cache_key[6 * 17 + 1] = {0};
    {
        int off = 0;
        for (unsigned stage = 0, idx = 0; stage < 6 && idx < 6; stage++) {
            if (!(stages & (1u << stage))) continue;
            off += snprintf(cache_key + off, sizeof cache_key - (size_t)off, "%02x%02x%02x%02x%02x%02x%02x%02x",
                            keys[idx][0], keys[idx][1], keys[idx][2], keys[idx][3],
                            keys[idx][4], keys[idx][5], keys[idx][6], keys[idx][7]);
            idx++;
        }
    }
    struct nvn_vk_program *loaded = NULL;
    for (int i = 0; i < NVK_PROGRAM_CACHE; i++)
        if (g_cache[i].valid && !strcmp(g_cache[i].cache_key, cache_key)) { loaded = &g_cache[i].program; g_hits++; break; }
    if (!loaded) {
        for (int i = 0; i < NVK_PROGRAM_CACHE; i++) {
            if (g_cache[i].valid) continue;
            char why[192] = "";
            if (nvn_vk_program_load(device, dir, keys, stages, &g_cache[i].program, why, sizeof why)) {
                result->code = S2A_RECORD_UNSUPPORTED;
                snprintf(result->why, sizeof result->why, "draw: program result load failed: %s", why);
                g_failed++;
                return S2A_RECORD_UNSUPPORTED;
            }
            g_cache[i].valid = 1;
            snprintf(g_cache[i].cache_key, sizeof g_cache[i].cache_key, "%s", cache_key);
            snprintf(g_cache[i].identity, sizeof g_cache[i].identity, "%s", g_cache[i].program.identity);
            loaded = &g_cache[i].program;
            g_loaded++;
            break;
        }
        if (!loaded) {   // 缓存满：先明确失败，不静默丢
            result->code = S2A_RECORD_LIMIT;
            snprintf(result->why, sizeof result->why, "draw: shader program cache full (%d)", NVK_PROGRAM_CACHE);
            g_failed++;
            return S2A_RECORD_LIMIT;
        }
    }
    // 到这里已经证明：新后端能按 prog_t 的阶段 key 读到统一编译结果（s2a-direct-program-v1），
    // 按 NVN 阶段校验后用其 SPIR-V 建出 VkShaderModule，并拿到真实 set/binding 反射。
    char summary[160];
    nvn_vk_program_summary(loaded, summary, sizeof summary);
    result->code = S2A_RECORD_UNSUPPORTED;
    snprintf(result->why, sizeof result->why,
             "draw: program loaded (%s) but pipeline/descriptor binding is not wired yet", summary);
    g_failed++;
    (void)indexed;
    (void)g_loaded; (void)g_hits; (void)g_failed;
    return S2A_RECORD_UNSUPPORTED;
}

// Keep all production return paths and output bytes inside the unchanged body.
int nvk_draw_builder(struct s2a_vk_device *device, const struct s2a_record_draw_state *state,
                     struct s2a_record_result *result, void *opaque) {
    struct s2a_w2_shader observation = {0};
    s2a_w2_shader_begin(&observation, S2A_W2_GRAPHICS);
    int rc = nvk_draw_builder_impl(device, state, result, opaque);
    s2a_w2_shader_end(&observation, rc == S2A_RECORD_OK);
    return rc;
}

void nvk_draw_stats(uint32_t *loaded, uint32_t *hits, uint32_t *failed) {
    if (loaded) *loaded = g_loaded;
    if (hits) *hits = g_hits;
    if (failed) *failed = g_failed;
}
