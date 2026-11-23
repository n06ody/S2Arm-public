// 着色器来源（graphics/nvn/replay/rp_shader.h 的实现）：回放状态 → 编译事实 → 内容身份 →
//   包内离线库 / 跨局缓存 / 现场编译（C# 翻译器）→ 解析成每阶段 SPIR-V + 绑定表 → 后端着色器。
// 事实与身份的口径沿用保留零件 nvn/nvn_vk_compile_facts.c（它的入参是旧回放器的绘制快照，这里直接从 rp_S 填同一组事实）。
// 三段式：提交线程只捕获「不可变输入快照」（读 rp_S 填事实 / 算内容身份 / pin 住源引用）；工作函数只吃快照
// （离线库 → 跨局缓存 → 现场编译 → parse + 建后端着色器），同一身份在途只有一份；最后在锁里校验会话未停 /
// 代际未变才发布到 g_progs。预热带（nvn/nvn_warm.c）按跨会话清单在后台重建热点程序，真实 draw 直接命中；
// 缓存未命中时由提交线程生成请求；启用需求编译后，worker 只执行不可变请求并返回字节。
// 线程：后台线程不读 rp_S、不编码命令、不提交；nvn_gpu_shader_load 的契约明确允许 [后台]（预取）。
#include "rp_shader.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_exec.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "nvn_refl.h"
#include "nvn_fmt_info.h"
#include "nvn_hmap.h"
#include "direct/compile_request.h"
#include "direct/compile_worker.h"
#include "direct/input_identity.h"
#include "direct/source_store.h"
#include "direct/texture_facts.h"
#include "spirv_sampler_use.h"
#include "spirv_layer_out.h"
#include "direct/offline_library.h"
#include "direct/libpack_vk.h"
#include "direct/s2a_direct.h"
#include "../../../graphics/nvn/gfx/nvn/dmsh_chain.h"   // b-<DMSH_CHAIN>-vk：出包预编库目录名（同 nvn_vk_draw.c）
#include "direct/program_cache.h"
#include "direct/sha256.h"
#include "direct/spirv_storage.h"   // s2a_spirv_declares_cap（#1340 Int16 按程序拒收）
#include "direct/jv.h"
#include "s2a_direct_host.h"
#include "nvn/nvn_vk_program_result.h"
#include "nvn/nvn_vk_compute_request.h"
#include "nvn/nvn_vk_prog_warm.h"   // 跨会话程序清单（note / stats；build 与 quiesce 在本文件实现）
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef S2A_ANDROID_DEBUG_ENV
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// One atomic linearizes session stop with shader admission. The low bits
// retain every active resolver until it has finished using the session, not
// merely until the managed compiler returns.
#define NVK_SHADER_STOP_BIT (1u << 31)
static _Atomic unsigned g_shader_admission;
static int g_live_compile;   // 0 = 对照档，1 = 后台，-1 = 启动失败（具名退回原路径）；会话准备时定
static uint32_t g_live_frame_ms, g_live_unsafe_ms;
static uint64_t now_ns(void);
static uint64_t g_live_budget_frame, g_live_spent_ns;   // 提交 owner 独占
static _Atomic uint64_t g_live_enqueued, g_live_wait_ns, g_live_pending, g_live_fallback, g_live_timeout;
// 本帧前几个现编身份（诊断属性 nvk_hitch_log 开时才写；rp_shader_live_compile_take 取走并清空）
static char g_live_ids[72]; static _Atomic unsigned g_live_ids_n; static pthread_mutex_t g_live_ids_mu = PTHREAD_MUTEX_INITIALIZER;
static int live_worker(void *, uint64_t, uint64_t, const char *, size_t, uint8_t **, size_t *, char *, size_t);
void nvk_shader_stop(void) {
    atomic_fetch_or_explicit(&g_shader_admission, NVK_SHADER_STOP_BIT, memory_order_acq_rel);
    // 先唤醒等待者，不在 Stop 入口 join。执行 managed 的 worker 自己持准入，现有停局限时等它。
    if (g_live_compile == 1) (void)s2a_compile_worker_stop_bounded(0, NULL, 0);
}
unsigned nvk_shader_inflight(void) { return atomic_load_explicit(&g_shader_admission, memory_order_acquire) & ~NVK_SHADER_STOP_BIT; }
// 会话级在途 / 静默 / 代际清场（定义在下面的状态变量之后：open 在文件前面，用前向声明）。
static void shader_session_state_reset(void);
int nvk_shader_open(void) {
    unsigned expected = NVK_SHADER_STOP_BIT;
    if (atomic_compare_exchange_strong_explicit(&g_shader_admission, &expected, 0, memory_order_acq_rel, memory_order_acquire)) {
        shader_session_state_reset();   // 在途 / 静默位 / 代际清场
    } else if (expected != 0) return -1;
    g_live_compile = 0;
    g_live_budget_frame = 0;
    g_live_spent_ns = 0;
    struct rp_shader_live_compile_stats discarded;
    rp_shader_live_compile_take(&discarded);
    if (nvn_plat_flag("nvk_live_compile_async", 1)) {
        const int frame_ms = nvn_plat_flag("fu_frame_ms", 100);
        const int unsafe_ms = nvn_plat_flag("fu_unsafe_ms", 10000);
        g_live_frame_ms = frame_ms > 0 ? (uint32_t)frame_ms : 0;
        g_live_unsafe_ms = unsafe_ms > 0 ? (uint32_t)unsafe_ms : 0;
        char why[160] = "";
        const int configured = nvn_plat_flag("nvk_live_compile_slots", 0);
        unsigned slots = configured > 0 ? (unsigned)configured : s2a_compile_worker_recommended_slots();
        if (slots > S2A_CW_SLOTS_MAX) slots = S2A_CW_SLOTS_MAX;
        g_live_compile = s2a_compile_worker_start_pool(live_worker, NULL, slots, why, sizeof why) ? -1 : 1;
        if (why[0]) nvn_plat_log(g_live_compile < 0 ? NVN_PLAT_LOG_RED : NVN_PLAT_LOG_WARN, "shader", why);
    }
    return 0;
}
static int shader_enter(void) {
    unsigned s = atomic_load_explicit(&g_shader_admission, memory_order_acquire);
    for (;;) {
        if ((s & NVK_SHADER_STOP_BIT) || (s & ~NVK_SHADER_STOP_BIT) == NVK_SHADER_STOP_BIT - 1) return -1;
        if (atomic_compare_exchange_weak_explicit(&g_shader_admission, &s, s + 1, memory_order_acq_rel, memory_order_acquire)) return 0;
    }
}
static void shader_leave(void) { atomic_fetch_sub_explicit(&g_shader_admission, 1, memory_order_release); }
// The successful compare/exchange is the compile-admission linearization point.
// Stop after that point sees the resolver in flight and retains its resources.
static int shader_compile_admit(void) {
    unsigned s = atomic_load_explicit(&g_shader_admission, memory_order_acquire);
    for (;;) {
        if (s & NVK_SHADER_STOP_BIT) return 0;
        if (atomic_compare_exchange_weak_explicit(&g_shader_admission, &s, s, memory_order_acq_rel, memory_order_acquire)) return 1;
    }
}

struct entry { struct rp_prog prog; int failed; int warm; int warm_hit; uint8_t fact_ok; uint32_t fact_mask[4]; uint8_t res_ok; uint8_t res_key[32]; uint8_t seen; };   // res_key：这份程序的结果合并比对键（结果摘要 / 负载长度 / 阶段数 / c1 的 SHA-256，见 alias_key_hash；结果合并 #1355）；seen：rp_shader_reset 按「同一 entry 只释放一次」去重时的临时标记   // fact_*：结果字节里读出的「编译器实际读了哪些绑定事实」（nvk_shader_fact_cache.h），work_run 里算好，发布后由提交线程入表   // warm：预热带发布；warm_hit：已被真实 draw 采用过一次（验收计数口径）
static nvn_hmap g_progs = {.klen = 32};   // 内容身份（SHA-256）→ struct entry *。结果合并后，同一份结果的多个身份指向同一个 entry（拥有者只有一个，见 rp_shader_reset）
// 结果合并（#1355）：比对键（见 alias_key_hash：缓存文件头里的结果摘要 + 负载长度 + 阶段数 + c1）→ struct entry *（只借用；g_progs_mu 下读写；整表随 rp_shader_reset 清）。
// 同一份编译结果的不同身份共用一个 entry（一份着色器句柄，管线缓存按句柄比，所以管线也合并）。缺省打开；debug.s2a.nvk_alias=0 关闭（只有调试包读，出包版恒走缺省）。
static nvn_hmap g_res_tab = {.klen = 32};
static _Atomic uint64_t g_n_alias_fg, g_n_alias_bg, g_n_alias_probe, g_n_alias_nores, g_n_alias_nofile, g_n_alias_badfile;   // 合并（前台 / 预热带）/ 读头次数 / 头好但没有比对键一致的常驻程序 / 没有缓存文件 / 缓存文件坏
static int alias_on(void) { static int v = -1; if (v < 0) v = nvn_plat_flag("nvk_alias", 1) ? 1 : 0; return v; }
// g_progs 与「同身份在途」表共用一把锁：nvn_hm 不是线程安全的，而预热带（[后台]）与提交线程都会查找/发布。
// 锁只覆盖表结构与 entry 标志；parse、建后端着色器、编译都在锁外（否则预热带持锁建对象会挡住回放线程）。
static pthread_mutex_t g_progs_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_progs_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_progs_epoch;   // rp_shader_reset / 下一次 open 推进：换代前开始的后台结果一律不得发布
static int g_warm_quiesced;      // 预热收拢后（本局）后台不得再发布：收尾路径不再依赖线程是否真的退出了
// 同身份在途：谁先认领谁做，另一个等结果（提交线程超 2 秒打一行红但继续等，契约同 iOS pso_pend_take）。
enum { INFLIGHT_MAX = 128 };
struct inflight_ent { uint8_t identity[32]; uint8_t used, bg; };
static struct inflight_ent g_inflight[INFLIGHT_MAX];
static unsigned g_inflight_n, g_inflight_bg;   // bg：预热带认领、尚未结束的构建数（quiesce 等它）

// ---------- 程序借用（rp_shader.h 的借用合同）----------
// 谁用：后台消费者（管线预热，以后预取）拿到 rp_prog* 之后，整段使用期间必须持有借用。
// 谁等：预热静默（nvk_shader_warm_quiesce）与收尾（rp_shader_reset）。代际 / 停局位只能决定
//       「迟到的结果要不要安装」，不能证明对象还活着——所以释放 entry 之前必须等借用归零。
// 表：身份 → 未归还次数，定长 256 格线性查（借用次数与「同时在建管线的条数」同量级）。满了如实
// 返回 -1 并计数，不静默降级成裸指针（那正是这条合同要防的东西）。
// 锁：只在 g_progs_mu 下读写——与「发布 / 查找 / 静默 / 收尾」同一把锁，借用与 reset 因此有唯一先后。
enum { BORROW_SLOTS = 256 };
struct borrow_ent { uint8_t identity[32]; unsigned n; };   // n == 0 = 空槽（identity 内容不作数）
static struct borrow_ent g_borrow[BORROW_SLOTS];
static unsigned g_borrowed;         // 当前未归还数（= 所有槽 n 之和）
static unsigned g_borrow_peak;      // 上次成功收尾以来的峰值（读数）
static uint64_t g_borrow_rejected;  // 借不到的次数：停局 / 参数错 / 借用表满（读数）
static uint64_t g_borrow_unmatched; // 没有对应借用的归还（调用方 bug，读数）
static uint64_t g_retained_progs;   // 收尾时借用未归零、改成泄漏到进程结束的程序记录总数（读数）
static int g_borrow_full_said, g_borrow_unmatched_said;   // 这两类异常只报一次红（计数照常累加）
// 借用读数快照（锁内取；借用表与计数器只在 g_progs_mu 下改）。
struct borrow_snap { unsigned borrowed, peak; uint64_t rejected, unmatched, retained; };
static void borrow_snap_take(struct borrow_snap *s) {
    pthread_mutex_lock(&g_progs_mu);
    s->borrowed = g_borrowed; s->peak = g_borrow_peak;
    s->rejected = g_borrow_rejected; s->unmatched = g_borrow_unmatched; s->retained = g_retained_progs;
    pthread_mutex_unlock(&g_progs_mu);
}
// 一行读数：「借用 N（峰值 M）/ 拒绝 k」。正常收尾用 INFO（借用为 0 也打一行，证明判据真的看过借用），
// 收尾被拒绝时用 RED——未静默状态留在读数里，不只看一次性告警。
static void borrow_log(int level, const char *phase, const struct borrow_snap *s) {
    char b[256];
    size_t off = (size_t)snprintf(b, sizeof b, "程序借用读数 phase=%s 借用 %u（峰值 %u）/ 拒绝 %llu",
                                  phase, s->borrowed, s->peak, (unsigned long long)s->rejected);
    if (off < sizeof b && s->unmatched)
        off += (size_t)snprintf(b + off, sizeof b - off, " 无主归还 %llu", (unsigned long long)s->unmatched);
    if (off < sizeof b && s->retained)
        off += (size_t)snprintf(b + off, sizeof b - off, " 收尾保留 %llu 份（泄漏到进程结束）", (unsigned long long)s->retained);
    nvn_plat_log(level, "shader", b);
}
static struct { uint64_t session, generation; uint8_t sha[32]; int ok; } g_profile;
// 后台也会读/加：跨局缓存 / 离线库命中数（日志与统计用）。
static _Atomic uint64_t g_n_offline, g_n_cache, g_n_compiled, g_n_failed;
// 会话级在途 / 静默 / 代际清场（nvk_shader_open 用它；定义放在状态变量之后）。
static void shader_session_state_reset(void) {
    pthread_mutex_lock(&g_progs_mu);
    g_progs_epoch++;
    g_warm_quiesced = 0;
    memset(g_inflight, 0, sizeof g_inflight);
    g_inflight_n = g_inflight_bg = 0;
    pthread_mutex_unlock(&g_progs_mu);
}
static int g_texpool_logged, g_texpool_last_over_limit, g_texpool_diag;
static uint64_t g_pool_programs, g_pool_attempts, g_pool_compiled, g_pool_rejected, g_pool_unsupported;
static uint64_t g_pool_request_rejected, g_pool_request_facts, g_pool_request_other;
static struct { char name[80]; uint64_t count; } g_pool_reasons[64];
static unsigned g_pool_reason_count;
// 编译被拒时的上下文日志（#999，只加日志）的行数预算：每个会话两类各最多 REJECT_LINES_MAX 行（拒收只在编译时偶发，不在热路径上；预算只防失控）。
// 超出预算的只计数不打行；g_*_total 是两类各自被拒的总条数（含没打出来的），截断时在拒绝原因读数里补一行「已截断，共 N 条」（pool_reject_lines）。
enum { REJECT_LINES_MAX = 128 };
static unsigned g_reject_lines, g_img_reject_lines, g_reject_total, g_img_reject_total;

static void sha256(const void *p, size_t n, uint8_t out[32]) { s2a_sha256 c; s2a_sha256_init(&c); s2a_sha256_update(&c, p, n); s2a_sha256_final(&c, out); }
static void red1(const char *name, const char *why) { if (nvn_unimpl("shader", name, NULL) == 1) nvn_plat_log(NVN_PLAT_LOG_RED, "shader", why); }
// 后端拒装着色器时记哪一项（#1340）：设备没 shaderInt16、模块又声明了 Int16 的，具名 int16_not_supported（只拒这个程序，不整局停）；其余照旧。
static void red_load(nvn_gpu_rc rc, const nvn_gpu_shader_src *src, const char *fallback, const char *why) {
    red1(rc.code == NVN_GPU_E_UNSUPPORTED && s2a_spirv_declares_cap(src->blob, src->bytes, 22) == 1 ? "int16_not_supported" : fallback, why);
}
// 不需要 _GNU_SOURCE 的 strdup：快照要活过后台线程，profile 原文 / closure / profileHash 必须各存一份。
static char *dup_str(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

// ---- 来源缓存（g_progs）与同身份在途 ----
static int shader_stopped(void) { return (atomic_load_explicit(&g_shader_admission, memory_order_acquire) & NVK_SHADER_STOP_BIT) != 0; }
static uint64_t mono_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull; }
// 释放一个 entry（含它持有的后端着色器）。调用方之后不得再持有该指针。
static void entry_free(struct entry *e) {
    if (!e) return;
    for (int st = 0; st < RP_PROG_STAGES; st++) if (e->prog.sh[st]) { nvn_gpu_shader_release(e->prog.sh[st]); e->prog.sh[st] = NULL; }
    for (int st = 0; st < RP_PROG_STAGES; st++) free((void *)e->prog.c1[st]);
    free(e);
}
// 程序对象拷一份阶段 st 的 c1 常量段（整段；不足 0x100 的不算捕获，同 source_store / 预热清单的判据）。
// 只截前 0x100 字节时，读到后段的着色器全读 0（八方旅人调色 LUT 读到第 2304 字节 → LUT 全 0 → 场景整幅黑）。
static void prog_c1_take(struct rp_prog *p, int st, const uint8_t *c1, uint32_t bytes) {
    if (!c1 || bytes < 0x100 || p->c1[st]) return;
    uint8_t *x = calloc(1, ((size_t)bytes + 15u) & ~(size_t)15u);   // 存储补齐到 16 字节：回放核心按 16 字节补齐整段绑定（rp_draw NVN_RB_C1），补齐部分是 0
    if (!x) return;
    memcpy(x, c1, bytes); p->c1[st] = x; p->c1_len[st] = bytes; p->c1_ok |= (uint8_t)(1u << st);
    if (bytes > 0x100) {   // 读数：超过 0x100 的 c1（修前后段读 0 的那类）本局出现了几份，前 4 份各打一行
        static _Atomic uint32_t said; uint32_t k = atomic_fetch_add(&said, 1);
        if (k < 4) { char m[96]; snprintf(m, sizeof m, "c1 常量段整段 %u 字节（阶段 %d，本局第 %u 份超过 256 字节）", (unsigned)bytes, st, (unsigned)k + 1); nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", m); }
    }
}
// 预热记录带上阶段 st 的 c1 整段（指针只在 nvk_prog_warm_note 调用期间读，清单自己存一份只读副本）。
//   不足 0x100（没捕获）不记，同 build 的判据。返回 1 = 记上。
static int rec_c1_put(struct nvk_prog_rec *rec, int st, const uint8_t *c1, uint32_t bytes) {
    if (!c1 || bytes < NVK_PROG_REC_C1_MIN || bytes > NVK_PROG_REC_C1_MAX) return 0;
    rec->c1[st] = c1; rec->c1_len[st] = bytes; rec->c1_ok |= (uint8_t)(1u << st);
    return 1;
}
static struct entry *progs_get_locked(const uint8_t identity[32]) { return (struct entry *)nvn_hm_get(&g_progs, identity); }
// 锁内取已发布项；预建对象第一次被真实 draw 采用时置 warm_hit 并回报。
// 验收口径：draw_hits = 有多少份「预建程序」被真实 draw 采用过，**每份只计一次**（不是每笔绘制——
// 一份程序被画几千笔会把读数放大成百万级，失去意义）。*failed/*adopted 只在对应指针非空时写。
static struct entry *progs_get_adopted_locked(const uint8_t identity[32], int *failed, int *adopted) {
    struct entry *e = progs_get_locked(identity);
    if (failed) *failed = e && e->failed;
    if (adopted) *adopted = 0;
    if (e && !e->failed && e->warm && !e->warm_hit) { e->warm_hit = 1; if (adopted) *adopted = 1; }
    return e;
}
static struct inflight_ent *inflight_find_locked(const uint8_t identity[32]) {
    for (unsigned i = 0; i < INFLIGHT_MAX; i++) if (g_inflight[i].used && !memcmp(g_inflight[i].identity, identity, 32)) return &g_inflight[i];
    return NULL;
}
// 认领：1 = 认领成功（由本线程做）；0 = 已在途；-1 = 表满 / 停局 / 预热已静默。
static int inflight_claim_locked(const uint8_t identity[32], int bg) {
    if (inflight_find_locked(identity)) return 0;
    if (shader_stopped() || (bg && g_warm_quiesced)) return -1;
    for (unsigned i = 0; i < INFLIGHT_MAX; i++) {
        if (g_inflight[i].used) continue;
        memset(&g_inflight[i], 0, sizeof g_inflight[i]);
        memcpy(g_inflight[i].identity, identity, 32);
        g_inflight[i].used = 1; g_inflight[i].bg = (uint8_t)(bg ? 1 : 0);
        g_inflight_n++; if (bg) g_inflight_bg++;
        return 1;
    }
    return -1;
}
static void inflight_done_locked(const uint8_t identity[32]) {
    struct inflight_ent *f = inflight_find_locked(identity);
    if (!f) return;
    if (f->bg && g_inflight_bg) g_inflight_bg--;
    if (g_inflight_n) g_inflight_n--;
    f->used = 0;
    pthread_cond_broadcast(&g_progs_cv);
}
static void inflight_abort(const uint8_t identity[32]) {
    pthread_mutex_lock(&g_progs_mu);
    inflight_done_locked(identity);
    pthread_mutex_unlock(&g_progs_mu);
}
// 绑定核对拒收（lib_bad）的出库程序被现编版顶替后，旧 entry 从身份表摘除但对象不立刻释放：
// 预热后台可能还借用它（rp_prog* 在借用方手里）。登记到这里，收尾按借用判据统一回收；表满宁可泄漏不 UAF。
static struct entry *g_orphan[128]; static unsigned g_n_orphan;
static void orphan_note(struct entry *e) {   // 调用方持 g_progs_mu
    if (g_n_orphan < sizeof g_orphan / sizeof g_orphan[0]) g_orphan[g_n_orphan++] = e;
}
// 发布。返回 1 = 插入（entry 归 g_progs）；0 = 同身份已存在（*existing 给出，调用方释放自己的 entry）；
// -1 = 丢弃（停局 / 代际变了 / 预热已静默；调用方释放自己的 entry）。首次结果为准：失败项与成功项互不覆盖
// （与原有 g_progs 语义一致：同一份输入只编一次，失败也不反复编）。lib_bad 旧项例外：绑定核对拒收的出库版
// 被现编版顶替（对象转入孤儿表，借用方手里那份仍有效）。
static int progs_publish(const uint8_t identity[32], struct entry *e, int from_warm, uint64_t epoch, struct entry **existing, int *failed) {
    if (existing) *existing = NULL;
    if (failed) *failed = 0;
    pthread_mutex_lock(&g_progs_mu);
    struct entry *have = progs_get_locked(identity);
    int rc;
    if (have && !have->prog.lib_bad) { if (existing) *existing = have; if (failed) *failed = have->failed; inflight_done_locked(identity); rc = 0; }
    else if (shader_stopped() || epoch != g_progs_epoch || (from_warm && g_warm_quiesced)) {
        // 发布被拒（停局 / 换代 / 预热已静默）：认领必须在这里结束——否则在途表里留一条永不完成的记录，
        // 之后同身份的 draw 会在 await_inflight 上一直等到停局，预热静默检查也会一直报「有在途」。
        inflight_done_locked(identity);
        rc = -1;
    } else {
        e->warm = from_warm;
        if (failed) *failed = e->failed;
        struct entry *old = (struct entry *)nvn_hm_put(&g_progs, identity, e);
        if (old && old != e) orphan_note(old);   // lib_bad 旧项被顶替：转孤儿表，借用方持有的指针仍有效
        if (alias_on() && e->res_ok && !e->failed && !nvn_hm_get(&g_res_tab, e->res_key)) (void)nvn_hm_put(&g_res_tab, e->res_key, e);   // 结果合并：比对键相同的以后由它代表
        inflight_done_locked(identity);
        rc = 1;
    }
    pthread_mutex_unlock(&g_progs_mu);
    return rc;
}
// 结果合并的比对键（#1355）。除「结果摘要 + 负载长度」外，挂在程序对象上、**由身份推出来而不是由负载字节读出来**的东西都在这里比对，不一致就不合并：
//   · nst：阶段数（由身份的阶段掩码来；build 用它校验结果阶段、也用它定支持绑定的阶段掩码）；
//   · c1：每个阶段的 c1 常量段 + 「有没有」（来自请求方的源快照；c1 不在身份里——身份的源摘要只覆盖 control‖code——所以不能靠「身份相同」推出它相同）。
// 其余字段（着色器句柄、绑定表 / 反射、贴图声明、支持缓冲长度、属性 / 片元输出掩码、c0 读集、base instance 判定）全由负载字节唯一决定，
// z_raw 只由设备能力决定（本局内是常数）；出包预编库 / 离线库来的程序不带结果摘要，永远不进结果表。
struct alias_key { int nst; uint8_t c1_ok; const uint8_t *c1[RP_PROG_STAGES]; uint32_t c1_len[RP_PROG_STAGES]; };   // c1[st] / c1_len[st] 仅当 c1_ok 的位 st 为 1 时有效（整段）
// 比对键 = SHA-256(结果摘要 ‖ 负载长度 ‖ 阶段数 ‖ c1 有无位 ‖ 各有 c1 阶段的（c1 长度 ‖ c1 整段）)。键里每一项都改一下 → 键就变 → 不合并（见 nvk_shader_alias_test.c 的逐项负对照）。
// 有无位单独编进去：否则「只有阶段 0 有这段 c1」与「只有阶段 1 有同样的 c1」字节流相同；长度编进去：否则前段相同、长短不同的两段 c1 拼出同一串。
static void alias_key_hash(const uint8_t res[32], uint64_t rlen, const struct alias_key *k, uint8_t out[32]) {
    s2a_sha256 h; s2a_sha256_init(&h);
    uint8_t head[32 + 8 + 2]; size_t n = 0;
    memcpy(head, res, 32); n += 32;
    for (int i = 0; i < 8; i++) head[n++] = (uint8_t)(rlen >> (8 * i));
    head[n++] = (uint8_t)k->nst;
    const uint8_t mask = (uint8_t)((1u << k->nst) - 1u);
    head[n++] = (uint8_t)(k->c1_ok & mask);
    s2a_sha256_update(&h, head, n);
    for (int st = 0; st < k->nst; st++)
        if (k->c1_ok & (1u << st)) {
            const uint8_t len4[4] = {(uint8_t)k->c1_len[st], (uint8_t)(k->c1_len[st] >> 8), (uint8_t)(k->c1_len[st] >> 16), (uint8_t)(k->c1_len[st] >> 24)};
            s2a_sha256_update(&h, len4, 4); s2a_sha256_update(&h, k->c1[st], k->c1_len[st]);
        }
    s2a_sha256_final(&h, out);
}
// 建好的程序挂上自己的比对键（结果摘要来自缓存文件头已校验的值 / 现编结果现算；阶段数与 c1 取自程序对象本身，与 build 用的是同一份）。
static void entry_set_res(struct entry *e, const uint8_t rsha[32], uint64_t rlen, int nst) {
    struct alias_key k; memset(&k, 0, sizeof k);
    k.nst = nst; k.c1_ok = e->prog.c1_ok;
    for (int st = 0; st < nst && st < RP_PROG_STAGES; st++) { k.c1[st] = e->prog.c1[st]; k.c1_len[st] = e->prog.c1_len[st]; }
    alias_key_hash(rsha, rlen, &k, e->res_key);
    e->res_ok = 1;
}
// 结果合并（#1355）：读这个身份缓存文件的 88 字节头；头里的结果摘要对得上某份已常驻的程序，就把这个身份也登记成指向同一个 entry
// （g_progs 里同一个 entry 多个键；拥有者只有以规范身份登记的那一项，见 rp_shader_reset）。不读负载、不建后端着色器。
// 对得上 = 比对键（结果摘要、负载长度、阶段数、c1）一致。头本身没有校验字段：摘要由文件自己声明，正式读取才重算比对；负载长度也进键，把偶发位翻转挡在外面。
// bg：1 = 预热带（受静默 / 换代约束）；0 = 前台。返回 entry（非 NULL = 这个身份已登记，可直接用）；失败项不合并；读头 / 没常驻 / 同身份在途 = NULL（走原来的路）。
// *adopted：同 progs_get_adopted_locked（预热带建的那份第一次被前台采用时为 1）。
static struct entry *alias_try(const uint8_t identity[32], int bg, const struct alias_key *key, int *adopted) {
    if (adopted) *adopted = 0;
    const char *cdir = getenv("S2A_DIRECT_CACHE_DIR");
    if (!cdir || !*cdir || nvn_plat_flag("no_disk_cache", 0)) return NULL;
    pthread_mutex_lock(&g_progs_mu);
    const uint64_t epoch = g_progs_epoch;
    const int busy = progs_get_locked(identity) != NULL || inflight_find_locked(identity) != NULL;   // 已发布 / 同身份已有人在做：不读头（调用方照旧命中它或等它）
    pthread_mutex_unlock(&g_progs_mu);
    if (busy) return NULL;
    uint8_t res[32]; uint64_t rlen = 0; char why[96];
    atomic_fetch_add_explicit(&g_n_alias_probe, 1, memory_order_relaxed);
    const int prc = s2a_program_cache_probe(cdir, identity, res, &rlen, why, sizeof why);
    if (prc > 0) { atomic_fetch_add_explicit(&g_n_alias_nofile, 1, memory_order_relaxed); return NULL; }   // 这个身份没有缓存文件（换翻译器闭包后的第一局几乎全是这种）
    if (prc < 0) {   // 有文件但头不对（版本 / 内部身份 / 长度）：前几条把原因打出来
        const uint64_t nb = atomic_fetch_add_explicit(&g_n_alias_badfile, 1, memory_order_relaxed) + 1;
        if (nb <= 4) { char b[200]; snprintf(b, sizeof b, "结果合并：缓存文件头不对（第 %llu 个，身份 %02x%02x%02x%02x…）：%s", (unsigned long long)nb,
            identity[0], identity[1], identity[2], identity[3], why); nvn_plat_log(NVN_PLAT_LOG_WARN, "shader", b); }
        return NULL;
    }
    uint8_t rk[32];
    alias_key_hash(res, rlen, key, rk);
    struct entry *ret = NULL;
    pthread_mutex_lock(&g_progs_mu);
    struct entry *r = (struct entry *)nvn_hm_get(&g_res_tab, rk);
    if (shader_stopped() || epoch != g_progs_epoch || (bg && g_warm_quiesced)) { /* 停局 / 换代 / 静默：不登记 */ }
    else if (!r || r->failed) atomic_fetch_add_explicit(&g_n_alias_nores, 1, memory_order_relaxed);
    else if (!progs_get_locked(identity)) {   // 竞态里别人先发布了这个身份就不动它（调用方下一步照旧命中它）
        (void)nvn_hm_put(&g_progs, identity, r);
        atomic_fetch_add_explicit(bg ? &g_n_alias_bg : &g_n_alias_fg, 1, memory_order_relaxed);
        ret = progs_get_adopted_locked(identity, NULL, adopted);   // 与正常命中同一口径
    }
    pthread_mutex_unlock(&g_progs_mu);
    return ret;
}
// [提交线程] 同一身份已被别人（预热带 / 另一笔）认领：等它发布，自己不重复做。
// 返回 1 = 已发布（*out 有效，可能是失败项）；0 = 在途已消失且没发布（本线程接手）；-1 = 停局。
// 等待超过 2 秒打一行红（不是失败：继续等到出结果或停局，iOS pso_pend_take 的 late 同义）。
static int await_inflight(const uint8_t identity[32], struct entry **out) {
    *out = NULL;
    uint64_t t0 = mono_ms();
    int warned = 0;
    pthread_mutex_lock(&g_progs_mu);
    for (;;) {
        int adopted = 0;
        struct entry *e = progs_get_adopted_locked(identity, NULL, &adopted);
        if (e) {
            pthread_mutex_unlock(&g_progs_mu);
            if (adopted) nvk_prog_warm_stat_draw_hit();   // 等到的正是预建对象：这份程序第一次被真实 draw 采用
            *out = e;
            return 1;
        }
        if (!inflight_find_locked(identity)) { pthread_mutex_unlock(&g_progs_mu); return 0; }
        if (shader_stopped()) { pthread_mutex_unlock(&g_progs_mu); return -1; }
        if (!warned && mono_ms() - t0 >= 2000) {
            warned = 1;
            pthread_mutex_unlock(&g_progs_mu);
            nvn_plat_log(NVN_PLAT_LOG_RED, "shader", "【红】同一程序的预热在途超过 2 秒，本笔绘制继续等（不重复准备、不跳过绘制）");
            pthread_mutex_lock(&g_progs_mu);
            continue;
        }
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += 20L * 1000000L;
        while (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        (void)pthread_cond_timedwait(&g_progs_cv, &g_progs_mu, &dl);
    }
}
// 拒绝原因（兜底直连编译器 ResolvePoolSlots 拒绝的按原因计数）在玩家包里也数、也打：拒绝只在编译时偶发，不在每帧热路径上；
// 全库普查按款汇总这几行（定「贴图句柄从常量缓冲取」这类还剩多少）。整份程序统计与借用读数仍只在诊断开关（nvk_texpool_log）下打。
static uint64_t g_pool_log_ms; static int g_pool_log_dirty;   // 上次打拒绝原因的时刻；之后又有新拒绝还没打
static void pool_reject_lines(void) {
    char line[320];
    for (unsigned i = 0; i < g_pool_reason_count; i++) {
        snprintf(line, sizeof line, "texture_pool_compile_reject reason=%.79s count=%llu", g_pool_reasons[i].name,
                 (unsigned long long)g_pool_reasons[i].count);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", line);
    }
    if (g_pool_request_facts) {
        snprintf(line, sizeof line, "texture_pool_request_reject reason=descriptor_pool_facts count=%llu", (unsigned long long)g_pool_request_facts);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", line);
    }
    if (g_pool_request_other) {
        snprintf(line, sizeof line, "texture_pool_request_reject reason=other count=%llu", (unsigned long long)g_pool_request_other);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", line);
    }
    if (g_reject_total > g_reject_lines || g_img_reject_total > g_img_reject_lines) {   // 被拒上下文日志（reject_context）超出行数预算：读日志的人要知道少了多少
        snprintf(line, sizeof line, "texture_pool_reject_lines_truncated 已截断，共 程序行 %u 条（只打了前 %u 条）、image 行 %u 条（只打了前 %u 条）；具名计数不受截断影响",
                 g_reject_total, g_reject_lines, g_img_reject_total, g_img_reject_lines);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", line);
    }
    g_pool_log_ms = mono_ms(); g_pool_log_dirty = 0;
}
static void pool_stats(const char *phase) {
    if (!g_texpool_diag) { if (g_pool_reason_count || g_pool_request_facts || g_pool_request_other) pool_reject_lines(); return; }
    char line[320];
    snprintf(line, sizeof line, "texture_pool_program_stats phase=%s programs=%llu compile_attempts=%llu compile_successes=%llu compile_rejected=%llu array_unsupported=%llu request_rejected=%llu",
        phase, (unsigned long long)g_pool_programs, (unsigned long long)g_pool_attempts,
        (unsigned long long)g_pool_compiled, (unsigned long long)g_pool_rejected,
        (unsigned long long)g_pool_unsupported, (unsigned long long)g_pool_request_rejected);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", line);
    pool_reject_lines();
    // 借用读数跟着同一份诊断走：管线预热在用程序时，「借用」非 0 就是收尾必须等的那部分。
    { struct borrow_snap bs; borrow_snap_take(&bs); borrow_log(NVN_PLAT_LOG_INFO, phase, &bs); }
}
/* The normal Android process exit calls this after services/GPU stop. It does
 * not pass through rp_shader_reset, which belongs to session retirement. */
static void pool_final_summary(int normal) {
    if (normal && (g_pool_programs || g_pool_request_rejected || g_pool_reason_count)) pool_stats("final");
}
// 新拒绝：第一次立刻打，之后最多每 5 秒打一次（普查局是强停收局，等不到 final）；没打的由 pool_log_flush 在后续取程序时补
static void pool_reject_tick(void) {
    if (!g_pool_log_ms || mono_ms() - g_pool_log_ms >= 5000) pool_stats("progress"); else g_pool_log_dirty = 1;
}
static void pool_log_flush(void) {   // 取程序路径上调，每 1024 次才看一次时钟
    static unsigned k;
    if (g_pool_log_dirty && !(++k & 1023) && mono_ms() - g_pool_log_ms >= 5000) pool_stats("progress");
}
struct work_snap;
static void reject_context(const struct work_snap *s, const char *reason, const char *message);
/* The producer's failure summary embeds its s2a-direct-error-v1 JSON. Missing,
 * truncated or malformed errors stay explicitly unclassified.
 * snap 非空时（图形阶段现场编译被拒）另打一行程序键与图像槽的上下文，见 reject_context。 */
static void compiler_rejected(const char *summary, const struct work_snap *snap) {
    char code[64] = "", message[256] = "", schema[48] = "", reason[80] = "unclassified";
    const char *p = strstr(summary, " 原因=");
    if (p) {
        p += strlen(" 原因=");
        struct s2a_jv doc = {p, strlen(p)}, v;
        if (s2a_jv_is_object(doc) && !s2a_jv_obj_find(doc, "schema", &v) && !s2a_jv_str(v, schema, sizeof schema) &&
            !strcmp(schema, "s2a-direct-error-v1") && !s2a_jv_obj_find(doc, "code", &v) && !s2a_jv_str(v, code, sizeof code) &&
            !s2a_jv_obj_find(doc, "message", &v) && !s2a_jv_str(v, message, sizeof message)) {
            size_t n = strspn(code, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
            if (n && n == strlen(code)) snprintf(reason, sizeof reason, "%s", code);
            if (!strcmp(code, "UNSUPPORTED_PROGRAM") && strstr(message, "texture handle from cbuf slot "))
                snprintf(reason, sizeof reason, "cbuf_handle_unsupported");
            else if (!strcmp(code, "UNSUPPORTED_PROGRAM") && strstr(message, "separate sampler slot is not implemented"))
                snprintf(reason, sizeof reason, "separate_sampler_unsupported");
            else if (!strcmp(code, "MISSING_FACT")) {
                snprintf(reason, sizeof reason, "MISSING_FACT.unclassified");
                size_t method = strspn(message, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_");
                if (method && method < 64 && ((message[0] >= 'A' && message[0] <= 'Z') ||
                    (message[0] >= 'a' && message[0] <= 'z')) &&
                    !strncmp(message + method, " has no source (", strlen(" has no source (")))
                    snprintf(reason, sizeof reason, "MISSING_FACT.%.*s", (int)method, message);
                else if (strstr(message, ": control segment is shorter than 0x758"))
                    snprintf(reason, sizeof reason, "MISSING_FACT.control_segment");
                else if (!strncmp(message, "control section for stage ", strlen("control section for stage ")))
                    snprintf(reason, sizeof reason, "MISSING_FACT.control_section");
                else if (!strncmp(message, "request.program.textures[", strlen("request.program.textures[")) &&
                         strstr(message, "].format is not a TextureFormat name"))
                    snprintf(reason, sizeof reason, "MISSING_FACT.texture_format");
            }
        }
    }
    unsigned i = 0;
    for (; i < g_pool_reason_count; i++) if (!strcmp(g_pool_reasons[i].name, reason)) break;
    if (i == 63 && i == g_pool_reason_count) snprintf(reason, sizeof reason, "other");
    if (i == g_pool_reason_count) {
        if (i == 64) i = 63;
        else { snprintf(g_pool_reasons[i].name, sizeof g_pool_reasons[i].name, "%s", reason); g_pool_reason_count++; }
    }
    g_pool_reasons[i].count++; g_pool_rejected++;
    if (snap) reject_context(snap, reason, message);
    pool_reject_tick();
}
static void array_unsupported(const char *why) {
    nvn_unimpl("shader", "texture_pool_array_unsupported", NULL);
    nvn_plat_log(NVN_PLAT_LOG_RED, "shader", why);
    if (g_texpool_diag) { g_pool_unsupported++; pool_stats("progress"); }
}

// 编译器 DirectProgram 的 program.textureArrayLength 合同：1..1048576。
// 不修改池值；零/负数是没有有效绑定，超限的真实值必须留在错误里。
static int shader_texture_pool_ready(void) {
    const int n = rp_S.tex_pool_n;
    if (n > 0 && !g_texpool_logged) {
        g_texpool_logged = 1;
        g_texpool_diag = nvn_plat_flag("nvk_texpool_log", 0);
        if (g_texpool_diag) {
            s2a_direct_summary_register(pool_final_summary);
            char line[160];
            snprintf(line, sizeof line, "texture_pool_capacity=%d sampler_pool_capacity=%d", n, rp_S.samp_pool_n);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", line);
        }
    }
    if (n <= 0) {
        char why[96]; snprintf(why, sizeof why, "贴图池未绑定或容量无效：%d", n);
        red1("texture_pool_missing", why);
        return 0;
    }
    if (n > 1048576) {
        uint64_t count = nvn_unimpl("shader", "texture_pool_over_limit", NULL);
        if (count == 1 || g_texpool_last_over_limit != n) {
            char why[128]; snprintf(why, sizeof why, "贴图池容量 %d 超过编译器上限 1048576", n);
            nvn_plat_log(NVN_PLAT_LOG_RED, "shader", why);
            g_texpool_last_over_limit = n;
        }
        return 0;
    }
    return 1;
}

// ---- 事实换算（同 nvn_vk_compile_facts.c）----
static const char *topology(uint32_t prim) {
    switch (prim) {
        case 0x0: return "Points";
        case 0x1: case 0x2: case 0x3: return "Lines";
        case 0xA: case 0xB: return "LinesAdjacency";
        case 0x4: case 0x5: case 0x6: case 0x7: case 0x8: case 0x9: case 0xE: return "Triangles";
        case 0xC: case 0xD: return "TrianglesAdjacency";
        default: return NULL;
    }
}
// 顶点属性 / 颜色附件的数值类：整数格式按 Uint / Sint，其余（归一化、浮点、打包、未绑定）都是 Float。
// 带「整数存储、取样时转 float」的顶点格式（I2F / UI2F，0x51–0x68、0x6B、0x6C）是 Float。
static const char *num_type(uint32_t fmt, int vertex) {
    if (!fmt) return "Float";
    if (vertex && ((fmt >= 0x51 && fmt <= 0x68) || fmt == 0x6B || fmt == 0x6C)) return "Float";
    nvn_fmt_info fi;
    if (!nvn_fmt_info_of(fmt, &fi)) return NULL;
    return fi.kind == NVN_FMT_UINT ? "Uint" : fi.kind == NVN_FMT_SINT ? "Sint" : "Float";
}
static const char *alpha_op(uint32_t func) {
    static const char *const names[] = {NULL, "Never", "Less", "Equal", "LessOrEqual", "Greater", "NotEqual", "GreaterOrEqual", "Always"};
    return func >= 1 && func <= 8 ? names[func] : "Always";
}
static int src1(unsigned f) { return f >= 0x10 && f <= 0x13; }

// The request carries BUFFER facts so the compiler can distinguish them.
static int request_texture_fact(uint32_t fmt, uint32_t target, uint32_t layers,
                                struct s2a_nvn_texture_fact *out, char *why, size_t why_size) {
    return target == NVN_TEXTURE_TARGET_BUFFER ? s2a_nvn_texture_buffer_fact(fmt, out, why, why_size) :
        s2a_nvn_texture_fact(fmt, target, layers, out, why, why_size);
}

// 图像高槽（c2 字 72 + 2×槽）没出编译事实的原因（#999；只记录，不改行为）。fill_facts 的高槽循环对下面三种情形静默跳过；
// 编译器随后若按「image 句柄没有同字直接事实」拒绝这份程序，拒收时按这份请求当时记下的原因说清是哪一种（reject_context）。
enum { IMGF_GIVEN = 0, IMGF_UNBOUND, IMGF_NO_TEXTURE, IMGF_NO_FACT };
struct img_miss { uint8_t why; int32_t id; uint32_t handle, fmt, target, layers; };
struct facts {
    struct s2a_compile_graphics_src gfx;
    struct s2a_compile_texture_src tex[S2A_COMPILE_TEXTURE_MAX]; int ntex;
    char tfmt[S2A_COMPILE_TEXTURE_MAX][S2A_TEX_FORMAT_MAX], tsmp[S2A_COMPILE_TEXTURE_MAX][S2A_TEX_SAMPLER_MAX];
    uint32_t ubo_use[6];
    struct img_miss img[3][8];   // [阶段][高槽]：why = IMGF_*（0 = 事实给了）
};

// c2 字 72..86（组合槽 32..39）与图像表重叠，来源同 iOS tex_slot_h。
static void graphics_high_handles(int st, uint64_t handles[8]) {
    for (int slot = 0; slot < 8; slot++)
        handles[slot] = rp_S.img[st][slot] ? rp_S.img[st][slot] : rp_S.tex[st][32 + slot];
}

// ---- 句柄在游戏用户常量缓冲里的贴图事实（#999 第 4 片；编译器侧契约见 DirectGpuAccessor.NeedTexture）----
// 编译器对 c3..c18 里的句柄字只认精确事实 (阶段, 字下标, cbufSlot)，缺了按这个坐标报 MISSING_FACT。宿主事先不知道一份着色器
// 读哪些常量缓冲字，所以按编译器报的坐标学：记在「阶段 + 阶段键」名下（ubo_need_learn），之后这份着色器每次建请求都读那个字
// （同回放 rp_draw.c ubo_word：这一刻游戏内存里的值），低 20 位 + 池起点 = 贴图号，按登记快照给事实。读不到 / 没登记就不给，
// 编译器照实报缺、这笔照旧取不到——不猜类型。表只有提交线程读写（fill_facts / input_sig / work_run 现编段都在提交线程），rp_shader_reset 清。
int rp_pool_find(uint64_t addr, uint64_t len, const uint8_t **mem, uint64_t *size);   // rp_entry.c
static const uint8_t *stage_key(unsigned st);
enum { UBO_NEED_SLOTS = 256, UBO_NEED_PROBE = 16, UBO_NEED_COORDS = 8 };
struct ubo_need { uint8_t key[8], st, used, n; struct { uint16_t word; uint8_t cbuf; } c[UBO_NEED_COORDS]; };
static struct ubo_need g_ubo_need[UBO_NEED_SLOTS];
static unsigned g_ubo_need_n;     // 有坐标的项数：0 时 input_sig / fill_facts 一步不多走（绝大多数游戏）
static int g_ubo_learned;         // 本次现编学到了新坐标（resolve_slow 据此当场重编一次）
static struct ubo_need *ubo_need_at(int st, const uint8_t key[8], int add) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 8; i++) { h ^= key[i]; h *= 1099511628211ull; }
    h ^= (uint64_t)st; h *= 1099511628211ull;
    for (unsigned k = 0; k < UBO_NEED_PROBE; k++) {
        struct ubo_need *e = &g_ubo_need[(h + k) & (UBO_NEED_SLOTS - 1)];
        if (e->used && e->st == st && !memcmp(e->key, key, 8)) return e;
        if (!e->used) {
            if (!add) return NULL;
            memset(e, 0, sizeof *e); memcpy(e->key, key, 8); e->st = (uint8_t)st; e->used = 1; g_ubo_need_n++;
            return e;
        }
    }
    return NULL;
}
static struct ubo_need *ubo_need_find(int st) {
    if (!g_ubo_need_n) return NULL;
    struct ubo_need *e = ubo_need_at(st, stage_key((unsigned)st), 0);
    return e && e->n ? e : NULL;
}
// 现编失败的摘要里带编译器错误 JSON；只认 MISSING_FACT 且坐标是用户常量缓冲（cbufSlot 3..18）的那种。1 = 学到新坐标
// 阶段号 = NVN 阶段（0 顶点 / 1 片元 / 2 几何 / 5 计算）；keys[st] 是该阶段的键，nst 是 keys 的项数，st_mask 是调用方这份请求里有的阶段
// （图形传 0x7 = 顶点 / 片元 / 几何，计算传 1 << 5）：编译器报的阶段不在里面的不学，免得把别的阶段的坐标记到不相干的键名下。
static int ubo_need_learn(const char *summary, const uint8_t (*keys)[8], int nst, unsigned st_mask) {
    if (!summary || !strstr(summary, "\"code\":\"MISSING_FACT\"")) return 0;
    const char *m = strstr(summary, " has no source (stage=");
    if (!m) return 0;
    m += strlen(" has no source (stage=");
    int st = !strncmp(m, "vs,", 3) ? 0 : !strncmp(m, "fs,", 3) ? 1 : !strncmp(m, "gs,", 3) ? 2 : !strncmp(m, "cs,", 3) ? 5 : -1;
    unsigned word = 0; int cbuf = 0; char close = 0;
    if (st < 0 || st >= nst || !(st_mask >> st & 1u) || sscanf(m + 3, "handle=%u,cbufSlot=%d%c", &word, &cbuf, &close) != 3 || close != ')') return 0;
    if (cbuf < 3 || cbuf > 18 || word >= 0x10000 / 4) return 0;
    struct ubo_need *e = ubo_need_at(st, keys[st], 1);
    if (!e) { red1("ubo_handle_need_table_full", "着色器的常量缓冲句柄坐标表满"); return 0; }
    for (unsigned i = 0; i < e->n; i++) if (e->c[i].word == word && e->c[i].cbuf == cbuf) return 0;
    if (e->n == UBO_NEED_COORDS) { red1("ubo_handle_need_coords_full", "一份着色器读的常量缓冲句柄超过 8 个"); return 0; }
    e->c[e->n].word = (uint16_t)word; e->c[e->n].cbuf = (uint8_t)cbuf; e->n++;
    return 1;
}
static const char *ubo_st_name(int st) { return st == 0 ? "vs" : st == 1 ? "fs" : st == 5 ? "cs" : "gs"; }   // 用户常量缓冲句柄表里的阶段号（NVN 阶段）→ 编译器的阶段名
// 常量缓冲 cbuf（c3 起 = 用户槽 cbuf − 3）字下标 word 处的句柄 → 贴图号。-1 = 槽没绑 / 越界 / 不在登记过的池里
static long ubo_tex_id(int st, unsigned cbuf, unsigned word) {
    const unsigned reg = cbuf - 3, off = word * 4;
    if (reg >= 16) return -1;
    const p_bind_buf *u = &rp_S.ubo[st][reg];
    if (!u->addr || (uint64_t)off + 4 > u->size || !rp_pool_find(u->addr + off, 4, NULL, NULL)) {
        char b[200];
        snprintf(b, sizeof b, "常量缓冲句柄读不到：%s c%u 第 %u 字，槽地址 0x%llx 大小 %llu（%s）", ubo_st_name(st), cbuf, word,
                 (unsigned long long)u->addr, (unsigned long long)u->size, !u->addr ? "槽没绑" : (uint64_t)off + 4 > u->size ? "越界" : "不在登记过的池里");
        red1("ubo_handle_unreadable", b);
        return -1;
    }
    uint32_t h; memcpy(&h, (const void *)(uintptr_t)(u->addr + off), 4);
    return (long)(h & 0xFFFFF) + rp_S.tex_base;
}
static int request_texture_fact(uint32_t fmt, uint32_t target, uint32_t layers, struct s2a_nvn_texture_fact *out, char *why, size_t why_size);
// 常量缓冲句柄指向没登记的贴图号、按默认描述符给了事实的次数（信息计数，不是未接；换局清零）。每局首次打一行，带贴图号和池容量。
static _Atomic uint64_t g_ubo_default_fact;
uint64_t rp_shader_ubo_default_fact_count(void) { return atomic_load(&g_ubo_default_fact); }
static void ubo_default_fact_note(int st, unsigned cbuf, unsigned word, long id) {
    if (atomic_fetch_add(&g_ubo_default_fact, 1) != 0) return;
    char b[240];
    snprintf(b, sizeof b, "常量缓冲句柄指向没登记的贴图：%s c%u 第 %u 字 → 贴图号 %ld（贴图池基址 %d、容量 %d）：同 Ryujinx，按默认描述符给事实，整笔照常编译",
             ubo_st_name(st), cbuf, word, id, (int)rp_S.tex_base, (int)rp_S.tex_pool_n);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", b);
}
// 把阶段 st 学到的每个坐标补成编译事实，接在 tex[n0..) 后面（tfmt / tsmp 是各项事实的字符串存放处），返回新的项数；最多填到 cap 项。
// 图形（struct facts）和计算（resolve_compute_slow 的局部数组）共用这一份：口径只此一处，两边不会各自走样。
static int ubo_facts_into(int st, struct s2a_compile_texture_src *tex, char (*tfmt)[S2A_TEX_FORMAT_MAX], char (*tsmp)[S2A_TEX_SAMPLER_MAX], int n0, int cap) {
    const struct ubo_need *e = ubo_need_find(st);
    int ntex = n0;
    for (unsigned i = 0; e && i < e->n && ntex < cap; i++) {
        long id = ubo_tex_id(st, e->c[i].cbuf, e->c[i].word);
        tex_t t; view_t tv; int has_tv = 0; struct s2a_nvn_texture_fact nf; char why[96];
        if (id < 0) continue;   // ubo_tex_id 已具名报过
        if (!rp_tex_snap_by_id((int)id, &t, &tv, &has_tv)) {
            // 号没登记（越过池容量 / 池里没登记；0xFFFFF 常是游戏的「这一路不用贴图」哨兵）：Ryujinx 取到默认描述符、整笔照画。
            //   事实同组合槽（默认事实只此一份）。不是红行：计数 + 每局首次一行，号越界在读数里仍看得见
            s2a_nvn_default_texture_fact(id, rp_S.tex_base, (uint32_t)rp_S.tex_pool_n, &nf);
            ubo_default_fact_note(st, e->c[i].cbuf, e->c[i].word, id);
        } else if (request_texture_fact((uint32_t)(has_tv && tv.set_format ? tv.format : t.d.format), (uint32_t)(has_tv && tv.set_target ? tv.target : t.d.target), t.d.depth, &nf, why, sizeof why)) {
            char b[240]; snprintf(b, sizeof b, "常量缓冲句柄的贴图出不了编译事实：贴图号 %ld 格式 0x%x 目标 %u：%s", id, (unsigned)(has_tv && tv.set_format ? tv.format : t.d.format), (unsigned)(has_tv && tv.set_target ? tv.target : t.d.target), why);
            red1("ubo_handle_no_fact", b); continue;
        }
        int n = ntex++;
        memcpy(tfmt[n], nf.format, sizeof tfmt[n]); memcpy(tsmp[n], nf.sampler_type, sizeof tsmp[n]);
        tex[n] = (struct s2a_compile_texture_src){ubo_st_name(st), e->c[i].word, e->c[i].cbuf, tsmp[n], tfmt[n], nf.is_buffer, nf.normalized, 0, nf.array_length};
    }
    return ntex;
}
static void ubo_facts_add(struct facts *f, int st) { f->ntex = ubo_facts_into(st, f->tex, f->tfmt, f->tsmp, f->ntex, S2A_COMPILE_TEXTURE_MAX); }
// 快路摘要用：这份着色器学过的每个坐标此刻指向的贴图的编译事实（格式 / 目标 / 深度；读不到记「无」）
static uint64_t ubo_fact_sig(int st) {
    const struct ubo_need *e = ubo_need_find(st);
    if (!e) return 0;
    uint64_t h = 1469598103934665603ull;
    #define UMIX(v) do { h ^= (uint64_t)(v); h *= 1099511628211ull; } while (0)
    for (unsigned i = 0; i < e->n; i++) {
        long id = ubo_tex_id(st, e->c[i].cbuf, e->c[i].word);
        tex_t t; view_t tv; int has_tv = 0;
        if (id >= 0 && rp_tex_snap_by_id((int)id, &t, &tv, &has_tv)) {
            UMIX(1); UMIX(has_tv && tv.set_format ? tv.format : t.d.format); UMIX(has_tv && tv.set_target ? tv.target : t.d.target); UMIX(t.d.depth);
        } else UMIX(id < 0 ? 2 : 3 + !s2a_nvn_default_texture_fact(id, rp_S.tex_base, (uint32_t)rp_S.tex_pool_n, NULL));   // 读不到 / 没登记（池内、池外两种默认事实）各是一个值
    }
    #undef UMIX
    return h;
}

static int depth_clip_cap(void);
static void fill_facts(const struct rp_shader_req *q, const struct s2a_direct_compile_context *pc, struct facts *f, int nst) {
    memset(f, 0, sizeof *f);
    struct s2a_compile_graphics_src *g = &f->gfx;
    g->topology = topology(q->prim);
    g->rt_count = q->rt_count;
    g->has_alpha_test = 1; g->alpha_test_compare = alpha_op(rp_S.color.alpha_func);
    g->has_alpha_to_coverage = 1; g->alpha_to_coverage_enable = rp_S.ms.a2c ? 1 : 0; g->alpha_to_coverage_dither = rp_S.ms.a2c_dither ? 1 : 0;
    g->has_blend = 1;
    for (int i = 0; i < 8; i++) if (src1(rp_S.blend[i].src_rgb) || src1(rp_S.blend[i].dst_rgb) || src1(rp_S.blend[i].src_a) || src1(rp_S.blend[i].dst_a)) g->dual_source_blend = 1;
    // 设备能把管线裁剪深度设成 [-1, 1]（CAP_DEPTH_CLIP_CTL）时现编也不改写 z，深度一律交给管线（rp_draw clip_neg1）：
    //   出包预编库的程序不改写 z，两条路必须同一种算法，否则同一几何的深度不逐位相等，先画深度、再按「深度相等」画的引擎
    //   在预编 / 现编程序混用时整块画不上（11-15 王国之泪用预编库整屏发暗，二分下去「两段同时开才坏」）
    g->has_depth_mode = 1; g->depth_mode = !rp_depth_zero() && depth_clip_cap() != 1;
    g->has_y_negate = 1; g->y_negate = rp_origin_lower_left();
    g->has_origin = 1; g->origin_upper_left = 1;
    g->has_halve_primitive_id = 1; g->halve_primitive_id = g->topology && q->prim == 0x7u;
    g->has_cb_draw_params = 1; g->cb_draw_params = pc->shader_draw_parameters;
    g->has_point_size = 1; g->program_point_size_enable = 1; g->point_size = 1.0f;
    // 参考值是绘制时的状态（OP_ALPHA_REF）。测试没开（函数是 ALWAYS）时固定给 0：不让同一个程序因为一个用不到的数拆成多份
    g->has_alpha_test_reference = 1; g->alpha_test_reference = rp_S.color.alpha_func >= 1 && rp_S.color.alpha_func <= 7 ? rp_S.alpha_ref : 0.0f;
    int any_ssbo = 0, unaligned = 0;
    for (int st = 0; st < 5; st++) for (int i = 0; i < 16; i++) if (rp_S.ssbo[st][i].addr) { any_ssbo = 1; if (pc->storage_buffer_offset_alignment && rp_S.ssbo[st][i].addr % pc->storage_buffer_offset_alignment) unaligned = 1; }
    if (!any_ssbo || pc->storage_buffer_offset_alignment) { g->has_unaligned_ssbo = 1; g->unaligned_ssbo = unaligned; }
    for (int i = 0; i < 32; i++) g->attribute_types[i] = num_type(i < rp_S.nattr && i < 16 ? rp_S.attr[i].fmt : 0, 1);
    for (int i = 0; i < 8; i++) g->fragment_output_types[i] = i < q->rt_count && q->rt_format[i] ? num_type(q->rt_format[i], 0) : "Float";
    for (int st = 0; st < nst; st++) { uint32_t m = 0; for (int i = 0; i < 16; i++) if (rp_S.ubo[st][i].size) m |= 1u << i; f->ubo_use[st] = m << 3; }   // 用户常量缓冲从 c3 起
    // 组合贴图槽事实：顶点、片元各 32 槽。槽里是「贴图号 | 采样器号 << 20」；没绑过的槽读作池里的第 0 项。
    for (int st = 0; st < nst; st++) for (int slot = 0; slot < 32; slot++) {
        uint64_t h = rp_S.tex[st][slot]; long id = h ? (long)(h & 0xFFFFF) : (long)rp_S.tex_base;
        tex_t t; view_t tv; int has_tv = 0; struct s2a_nvn_texture_fact nf; char why[96];
        if (rp_tex_snap_by_id((int)id, &t, &tv, &has_tv)) {
            // 格式、目标都取绑定视图的（视图自己设了就用它的）：声明的采样数值型要和绑上去的视图一致，坐标归一与否只看有效目标
            if (request_texture_fact((uint32_t)(has_tv && tv.set_format ? tv.format : t.d.format), (uint32_t)(has_tv && tv.set_target ? tv.target : t.d.target), t.d.depth, &nf, why, sizeof why)) continue;   // 表达不了的格式 / 目标：不出事实，编译器按缺项准确失败
        } else s2a_nvn_default_texture_fact(id, rp_S.tex_base, (uint32_t)rp_S.tex_pool_n, &nf);   // 没登记：Ryujinx GetTextureDescriptor 的默认描述符（默认事实只此一份，texture_facts.c）
        int n = f->ntex++;
        memcpy(f->tfmt[n], nf.format, sizeof f->tfmt[n]); memcpy(f->tsmp[n], nf.sampler_type, sizeof f->tsmp[n]);
        f->tex[n] = (struct s2a_compile_texture_src){(const char *const[]){"vs", "fs", "gs"}[st], (uint32_t)(8 + 2 * slot), -1, f->tsmp[n], f->tfmt[n], nf.is_buffer, nf.normalized, 0, nf.array_length};
    }
    // 高槽只给实际登记且能表达的描述符出事实；不沿用低槽的 Unknown 占位。
    for (int st = 0; st < nst; st++) {
        uint64_t handles[8]; graphics_high_handles(st, handles);
        for (int slot = 0; slot < 8; slot++) {
            long id = handles[slot] ? (long)(handles[slot] & 0xFFFFF) : (long)rp_S.tex_base;
            tex_t t; view_t tv; int has_tv = 0; struct s2a_nvn_texture_fact nf; char why[96];
            struct img_miss *miss = &f->img[st][slot];
            miss->why = IMGF_GIVEN; miss->id = (int32_t)id; miss->handle = (uint32_t)handles[slot];
            if (!rp_tex_snap_by_id((int)id, &t, &tv, &has_tv)) { miss->why = handles[slot] ? IMGF_NO_TEXTURE : IMGF_UNBOUND; continue; }
            miss->fmt = (uint32_t)(has_tv && tv.set_format ? tv.format : t.d.format);
            miss->target = (uint32_t)(has_tv && tv.set_target ? tv.target : t.d.target); miss->layers = (uint32_t)t.d.depth;
            if (request_texture_fact(miss->fmt, miss->target, miss->layers, &nf, why, sizeof why)) { miss->why = IMGF_NO_FACT; continue; }
            int n = f->ntex++;
            memcpy(f->tfmt[n], nf.format, sizeof f->tfmt[n]); memcpy(f->tsmp[n], nf.sampler_type, sizeof f->tsmp[n]);
            f->tex[n] = (struct s2a_compile_texture_src){st == 0 ? "vs" : st == 1 ? "fs" : "gs",
                (uint32_t)(72 + 2 * slot), -1, f->tsmp[n], f->tfmt[n], nf.is_buffer, nf.normalized, 0, nf.array_length};
        }
    }
    // Separate texture table c2: 0x168 + 8*slot, mapped to replay slot 128+slot.
    // Only registered descriptors are facts. A zero handle names pool entry zero,
    // just like the existing combined-table producer; no descriptor means no fact.
    for (int st = 0; st < nst; st++) for (int slot = 0; slot < 128; slot++) {
        uint64_t h = rp_S.tex[st][128 + slot];
        long id = h ? (long)(h & 0xFFFFF) : (long)rp_S.tex_base;
        tex_t t; view_t tv; int has_tv = 0; struct s2a_nvn_texture_fact nf; char why[96];
        if (!rp_tex_snap_by_id((int)id, &t, &tv, &has_tv)) continue;
        if (request_texture_fact((uint32_t)(has_tv && tv.set_format ? tv.format : t.d.format),
                (uint32_t)(has_tv && tv.set_target ? tv.target : t.d.target), t.d.depth, &nf, why, sizeof why)) continue;
        int n = f->ntex++;
        memcpy(f->tfmt[n], nf.format, sizeof f->tfmt[n]); memcpy(f->tsmp[n], nf.sampler_type, sizeof f->tsmp[n]);
        f->tex[n] = (struct s2a_compile_texture_src){(const char *const[]){"vs", "fs", "gs"}[st], (uint32_t)(90 + 2 * slot), 2, f->tsmp[n], f->tfmt[n], nf.is_buffer, nf.normalized, 1, nf.array_length};
    }
    for (int st = 0; st < nst; st++) ubo_facts_add(f, st);   // 句柄在用户常量缓冲里的贴图（见 ubo_need_learn）
}

#include "nvk_shader_fact_cache.h"

// fd_binding → 后端布局项（核心按 src_kind / src_reg 绑定）。0 = 这一项首版不支持
static int layout_ent(const struct fd_binding *b, nvn_gpu_layout_ent *e) {
    memset(e, 0, sizeof *e);
    e->src_reg2 = UINT16_MAX;
    e->set = (uint8_t)b->set; e->binding = (uint16_t)b->binding; e->count = 1; e->access = NVN_GPU_ACC_R;
    if (b->kind == 0) {
        e->cls = NVN_GPU_RES_UBO;
        if (b->is_support) e->src_kind = NVN_RB_SUPPORT;
        else if (b->slot == 0) e->src_kind = NVN_RB_DRV0;
        else if (b->slot == 1) e->src_kind = NVN_RB_C1;
        else if (b->slot == 2) e->src_kind = NVN_RB_DRV2;   // 驱动的贴图句柄表（着色器自己从表里读句柄时才会出现）
        else if (b->slot >= 3 && b->slot - 3 < 16) { e->src_kind = NVN_RB_CBUF; e->src_reg = (uint16_t)(b->slot - 3); }
        else return 0;
        return 1;
    }
    // 句柄在游戏用户常量缓冲里（iOS NVN_RB_TEXU / TBUFU）：src_reg / src_off = 句柄字所在的常量槽与字节偏移；
    //   另一半与采样器来源放进程序的 usrc 表（usrc_put，src_reg2 = 表下标），核心逐笔从常量缓冲读句柄
    if (b->kind == 2 && b->tex_ubo && b->count == 1) {
        e->cls = b->texel_buffer ? NVN_GPU_RES_TBUF : NVN_GPU_RES_TEX_SAMP;
        e->src_kind = b->texel_buffer ? NVN_RB_TBUFU : NVN_RB_TEXU;
        e->src_reg = b->tex_u.slot; e->src_off = b->tex_u.offset;
        return 1;
    }
    // 贴图数组：着色器按运行时下标取句柄，数组第 k 项 = 驱动句柄表的第 (src_reg + k) 个 8 字节项（核心按项逐个绑）
    if (b->kind == 2 && b->texel_buffer && b->count == 1 && b->tex_slot >= 0) {
        e->cls = NVN_GPU_RES_TBUF; e->src_kind = NVN_RB_TBUF; e->src_reg = (uint16_t)b->tex_slot; return 1;
    }
    // 整池数组（解析器 pool = 1 贴图池 / 2 采样器池）：布局来源 NVN_RB_TEXP / NVN_RB_SAMPP，整表一个绑定，count = 池长 + 1（表尾 dummy，
    //   nvn_backend.h nvn_gpu_pool_*）；资源半边分开（贴图池只是贴图、采样器池只是采样器），回放按池建快照绑（rp_draw.c）
    if (b->kind == 2 && b->pool) {
        if (b->count >= 0xffff) return 0;
        e->cls = b->pool == 1 ? NVN_GPU_RES_TEX : NVN_GPU_RES_SAMP;
        e->src_kind = b->pool == 1 ? NVN_RB_TEXP : NVN_RB_SAMPP;
        e->src_reg = 0; e->count = (uint16_t)(b->count + 1);
        return 1;
    }
    if (b->kind == 2 && !b->texel_buffer && b->count > 1 && b->slot == 0) { e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEX; e->src_reg = 0; e->count = b->count; return 1; }
    if (b->kind == 2 && !b->texel_buffer && b->tex_slot >= 0) {
        e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEX; e->src_reg = (uint16_t)b->tex_slot;
        // 分离采样器来源：c2 采样器表的槽（samp_slot）/ 着色器里的立即数采样器池下标（#1594，解析器把它放在 samp_src = FD_SAMP_ID，
        // tex_ubo = 0 只有这一种）/ 没有（采样器取贴图字高位）。常量采样器编成标记位 + 池下标，旧回放读到会具名跳过。
        if (b->samp_slot >= 0) e->src_reg2 = (uint16_t)b->samp_slot;
        else if (b->samp_src == FD_SAMP_ID) {
            if (b->samp_val > NVN_GPU_SAMP2_ID_MASK) return 0;   // 解析器已限 0..4095；越界不截断成别的下标，当作不支持的项
            e->src_reg2 = (uint16_t)(NVN_GPU_SAMP2_CONST | b->samp_val);
        }
        return 1;
    }
    // 存储缓冲：只认「NVN 存储槽」这一种来源（反射里的 nvnSlot）。地址来自 UBO 内 SbDescriptor / c1 段的要逐笔读
    // 常量缓冲内容，首版还没接，准确报红而不是猜槽位。
    if (b->kind == 1 && b->storage_kind == 1 && b->storage_slot >= 0 && b->storage_slot < 16) {
        e->cls = NVN_GPU_RES_SSBO;
        e->src_kind = NVN_RB_SBUFP;
        e->src_reg = (uint16_t)b->storage_slot;
        if (b->storage_writable) e->access = NVN_GPU_ACC_R | NVN_GPU_ACC_W;
        return 1;
    }
    // 共享内存溢出缓冲（计算）：没有客体来源，回放按派发组数 × 程序的 spill_bytes 从暂存区分配（rp_draw.c rp_dispatch）
    if (b->kind == 4 && b->count == 1) { e->cls = NVN_GPU_RES_SSBO; e->src_kind = NVN_RB_SPILL; e->access = NVN_GPU_ACC_R | NVN_GPU_ACC_W; return 1; }
    // 图像：单个存储图像（解析器给出 NVN 图像槽 0..7，放在 tex_slot），计算与图形阶段同一形态；回放按槽取这一阶段 BindImage 的句柄
    //   建存储视图（rp_draw.c image_resolve：计算派发与绘制共用）。imageBuffer（Dim=Buffer）是存储纹素缓冲：同一个图像槽，
    //   类别 IMAGE_TBUF（描述符 STORAGE_TEXEL_BUFFER），回放按槽里的缓冲贴图建缓冲视图（rp_draw.c texel_view，同 Ryujinx 按 isImage 绑 TextureBuffer）
    if (b->kind == 3 && b->count == 1 && b->tex_slot >= 0 && b->tex_slot < 8) {
        e->cls = b->texel_buffer ? NVN_GPU_RES_IMAGE_TBUF : NVN_GPU_RES_IMAGE; e->src_kind = NVN_RB_IMG; e->src_reg = (uint16_t)b->tex_slot;
        e->access = NVN_GPU_ACC_R | NVN_GPU_ACC_W;
        return 1;
    }
    return 0;   // 图像数组、存储缓冲的其它地址来源：随对应命令接
}

// TEXU / TBUFU 项的句柄来源进程序的 usrc 表（rp_shader.h rp_usrc），布局项 src_reg2 = 下标。0 = 表满
static struct rp_uword uword_of(const struct fd_ubo_word *w) {
    return (struct rp_uword){ .reg = w->slot, .sid = w->sid, .pair = w->pair, .pair_sid = w->pair_sid, .pair_reg = w->pair_slot, .off = w->offset, .pair_off = w->pair_offset };
}
static int usrc_put(struct rp_prog *pg, const struct fd_binding *b, nvn_gpu_layout_ent *e) {
    if (pg->nusrc >= RP_PROG_USRC_MAX) return 0;
    struct rp_usrc *u = &pg->usrc[pg->nusrc];
    memset(u, 0, sizeof *u);
    u->tex = uword_of(&b->tex_u);
    if (b->samp_src == FD_SAMP_UBO) { u->samp_kind = NVN_RB_SAMPU; u->samp = uword_of(&b->samp_u); }
    else if (b->samp_src == FD_SAMP_ID) { u->samp_kind = NVN_RB_SAMPI; u->samp_reg = b->samp_val; u->samp_pool = b->samp_pool; }   // samp_pool：立即数采样器池下标（#1594 片 3）
    else if (b->samp_src == FD_SAMP_SLOT) { u->samp_kind = NVN_RB_SAMP; u->samp_reg = b->samp_val; }
    e->src_reg2 = (uint16_t)pg->nusrc++;
    return 1;
}

// 组合采样贴图与存储图像的声明；无法识别时保留 0，不根据绑定对象猜类型。
//   ifmt（可为 NULL）：存储图像 OpTypeImage 的 Image Format 操作数（SPIR-V 枚举，0 = Unknown），回放按它与数值类选视图格式
static void sampled_decl(const void *blob, size_t bytes, uint32_t set, uint32_t binding, uint8_t *dtt, uint8_t *dcls, uint8_t *ifmt) {
    const uint32_t *w = blob;
    if (!w || bytes < 20 || bytes % 4 || w[0] != 0x07230203u || !w[3] || w[3] > 0x100000u) return;
    uint32_t bound = w[3]; size_t words = bytes / 4;
    struct decl_id { size_t off; uint32_t set, binding; };
    struct decl_id *ids = calloc(bound, sizeof *ids);
    if (!ids) return;
    for (uint32_t id = 0; id < bound; id++) ids[id].set = ids[id].binding = UINT32_MAX;
    for (size_t pos = 5; pos < words;) {
        uint32_t count = w[pos] >> 16, op = w[pos] & 0xffffu;
        if (!count || count > words - pos) goto done;
        const uint32_t *q = w + pos;
        uint32_t id = 0;
        switch (op) {
        case 21: if (count >= 4) id = q[1]; break;   // OpTypeInt
        case 22: if (count >= 3) id = q[1]; break;   // OpTypeFloat
        case 25: if (count >= 9) id = q[1]; break;   // OpTypeImage
        case 27: if (count >= 3) id = q[1]; break;   // OpTypeSampledImage
        case 28: if (count >= 4) id = q[1]; break;   // OpTypeArray
        case 29: if (count >= 3) id = q[1]; break;   // OpTypeRuntimeArray
        case 32: if (count >= 4) id = q[1]; break;   // OpTypePointer
        case 59: if (count >= 4) id = q[2]; break;   // OpVariable
        case 71:
            if (count >= 4 && q[1] < bound) {
                if (q[2] == 34) ids[q[1]].set = q[3];
                if (q[2] == 33) ids[q[1]].binding = q[3];
            }
            break;
        default: break;
        }
        if (id && id < bound) ids[id].off = pos;
        pos += count;
    }
    for (uint32_t var = 1; var < bound; var++) {
        if (ids[var].set != set || ids[var].binding != binding || !ids[var].off) continue;
        const uint32_t *q = w + ids[var].off;
        if ((q[0] & 0xffffu) != 59 || q[3] != 0) continue;
        uint32_t id = q[1];
        if (id >= bound || !ids[id].off) continue;
        q = w + ids[id].off;
        if ((q[0] & 0xffffu) != 32) continue;
        id = q[3];
        // 限制步数，损坏的类型环不会卡住装载。
        for (uint32_t step = 0; step < bound && id < bound && ids[id].off; step++) {
            q = w + ids[id].off;
            uint32_t op = q[0] & 0xffffu;
            if (op == 28 || op == 29) { id = q[2]; continue; }
            if (op == 27) {
                id = q[2];
                if (id >= bound || !ids[id].off) break;
                q = w + ids[id].off;
            } else if (op != 25) break;
            if ((q[0] & 0xffffu) != 25) break;
            uint32_t type = q[2], dim = q[3], depth = q[4], arrayed = q[5], ms = q[6];
            if (arrayed > 1 || ms > 1 || depth > 2 || type >= bound || !ids[type].off) break;
            int target = -1;
            if (dim == 0 && !ms) target = arrayed ? NVN_TEXTURE_TARGET_1D_ARRAY : NVN_TEXTURE_TARGET_1D;
            else if (dim == 1) target = ms ?
                (arrayed ? NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY : NVN_TEXTURE_TARGET_2D_MULTISAMPLE) :
                (arrayed ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D);
            else if (dim == 2 && !arrayed && !ms) target = NVN_TEXTURE_TARGET_3D;
            else if (dim == 3 && !ms) target = arrayed ? NVN_TEXTURE_TARGET_CUBEMAP_ARRAY : NVN_TEXTURE_TARGET_CUBEMAP;
            else if (dim == 5 && !arrayed && !ms) target = NVN_TEXTURE_TARGET_BUFFER;
            if (target < 0) break;
            const uint32_t *sample = w + ids[type].off;
            uint32_t op_type = sample[0] & 0xffffu;
            if (op_type != 22 && (op_type != 21 || sample[3] > 1)) break;
            *dtt = (uint8_t)(target + 1);
            *dcls = depth == 1 ? 3 : op_type == 21 ? (sample[3] ? 2 : 1) : 0;
            if (ifmt && q[8] < 256) *ifmt = (uint8_t)q[8];
            goto done;
        }
    }
done:
    free(ids);
}

// 驱动的贴图句柄表：贴图槽 s 的句柄在第 8 + 2s 个 32 位字，即第 4 + s 个 8 字节项；32 个槽 → 36 项
enum { HANDLE_TABLE_ENTRIES = 4 + 32 };

// 解析结果字节 + 建后端着色器与绑定布局（提交线程与预热带都会调；nvn_gpu_shader_load 契约允许 [后台]）。
// identity 是这份程序的内容身份：写进 rp_prog.identity（管线配方 / 清单用它跨会话定位同一份程序）。
// 具名拒收行带 8 字节程序键（同 rp_S.skey[5]，spill 派发 / 现编日志里的那个键）：
//   同一份代码的多个身份、不同代码的同类拒绝，才能在日志里逐个对上（身份是 32 字节哈希，键才是游戏代码的名字）
static void with_program_key(char *out, size_t cap, const char *why, const uint8_t key[8]) {
    int n = snprintf(out, cap, "%s", why);
    if (key && n > 0 && (size_t)n < cap)
        snprintf(out + n, cap - (size_t)n, "（程序键 %02x%02x%02x%02x%02x%02x%02x%02x）",
                 key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7]);
}
#ifdef S2A_ANDROID_DEBUG_ENV
// 预热与提交线程走同一拒收点。按文件加锁，让追加与 1 MiB 上限在并发和跨局时仍成立。
static void record_link_refusal(const struct s2a_source_entry_view views[3], int nst, uint64_t miss) {
    if (!nvn_plat_flag("nvk_link_refused_file", 0)) return;
    const char *root = nvn_plat_dir(NVN_PLAT_DIR_CACHE);
    char path[1200], row[128], keys[3][17] = {"", "", "-"};
    int fd = -1;
    const char *warning = "预编接口拒绝清单写入失败，本次采集可能不完整";
    if (!root || !*root || nst < 2 || nst > 3) goto failed;
    int len = snprintf(path, sizeof path, "%s/libpack-link-refused-v1.tsv", root);
    if (len < 0 || (size_t)len >= sizeof path) goto failed;
    for (int st = 0; st < nst; st++) {
        if (!views[st].key) goto failed;
        for (int i = 0; i < 8; i++)
            snprintf(&keys[st][2 * i], 3, "%02x", views[st].key[i]);
    }
    len = snprintf(row, sizeof row, "%s\t%s\t0x%016llx\t%s\t%s\n",
                   keys[0], keys[1], (unsigned long long)miss, DMSH_CHAIN, keys[2]);
    if (len <= 0 || (size_t)len >= sizeof row) goto failed;
    fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) goto failed;
    while (flock(fd, LOCK_EX) < 0) {
        if (errno != EINTR) goto failed;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) goto failed;
    if (st.st_size > 1024 * 1024 - len) {
        warning = "预编接口拒绝清单达到 1 MiB 上限，后续拒绝未收录";
        goto failed;
    }
    ssize_t wrote;
    do { wrote = write(fd, row, (size_t)len); } while (wrote < 0 && errno == EINTR);
    if (wrote != len) {
        if (wrote > 0) (void)ftruncate(fd, st.st_size);
        goto failed;
    }
    close(fd);
    return;
failed:
    if (fd >= 0) close(fd);
    static _Atomic int warned;
    if (!atomic_exchange(&warned, 1)) nvn_plat_log(NVN_PLAT_LOG_WARN, "shader", warning);
}
#endif

// lib = 这份结果字节来自出包预编库（shader_src.precompiled=1）：装载闸对表外声明按拒收，
//   调用方照旧 mark_bad 退回现编。离线库 / 跨局缓存 / 现编结果传 0。
static _Atomic uint64_t g_lp_link_refused;   // 出包预编程序因两段接口对不上退回现编的份数（本局）
union lp_prepared {
    struct fd_program graphics;
    struct nvn_vk_compute_program compute;
};

static struct entry *build(const uint8_t *bytes, size_t len, const struct s2a_source_entry_view views[3], int nst,
                           const uint8_t identity[32], int lib, union lp_prepared *ready) {
    struct entry *e = calloc(1, sizeof *e);
    if (!e) {
        if (ready) { nvn_vk_program_free(&ready->graphics); free(ready); }
        return NULL;
    }
    memcpy(e->prog.identity, identity, 32);
    struct fd_program fp; char why[192] = "";
    int parsed = 0;
    if (ready) { fp = ready->graphics; free(ready); }
    else {
        memset(&fp, 0, sizeof fp);
        parsed = nvn_vk_program_parse_graphics_stages(bytes, len, &fp, nst == 1, why, sizeof why);
    }
    if (parsed) {
        if (parsed == NVK_PROGRAM_ARRAY_UNSUPPORTED) array_unsupported(why); else red1("program_parse_failed", why);
        nvn_vk_program_free(&fp); e->failed = 1; return e;
    }
    for (int st = 0; st < FD_MAX_STAGES; st++) {
        if (fp.stage[st].present != (st < nst)) {
            red1("result_stage_mismatch", "compiler result stages do not match request");
            nvn_vk_program_free(&fp); e->failed = 1; return e;
        }
    }
    e->prog.support_bytes = fp.support_bytes; e->prog.attr_mask = fp.stage[0].attr_mask;
    // 两个读集在同一个结果内，开关只改变消费侧，不改变请求 / 缓存键。旧结果没有静态字段时关档保守未知。
    int c0_bound = nvn_plat_flag("nvk_c0_bound", 1) != 0;
    for (int st = 0; st < nst && st < RP_PROG_STAGES; st++)
        if (nvn_vk_binst_unread(&fp.stage[st], c0_bound)) e->prog.binst_unread |= (uint8_t)(1u << st);
    { static _Atomic int c0_said;
      if (nvn_plat_flag("nvk_c0_bound_diag", 0) && atomic_fetch_add(&c0_said, 1) < 40) {
          char m[160]; snprintf(m, sizeof m, "c0 定界=%d 静态读集存在=%d/%d 动态不读=%d/%d 静态不读=%d/%d 采用掩码=%u",
              c0_bound, fp.stage[0].binst_static_present, fp.stage[1].binst_static_present,
              fp.stage[0].binst_unread, fp.stage[1].binst_unread,
              fp.stage[0].binst_unread_static, fp.stage[1].binst_unread_static, e->prog.binst_unread);
          nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", m);
      } }
    e->prog.z_raw = depth_clip_cap() == 1;   // 同 fill_facts：有管线裁剪深度时所有图形程序都不改写 z（预编库的程序另在取用处置 1）
    e->prog.color_out_mask = nst >= 2 && fp.stage[1].present ? fp.stage[1].fragment_output_map : 0xffffffffu;
    // 顶点 / 几何阶段写层号：回放核心按分层通道开（八方旅人的 32x32x32 调色 LUT 每个实例写一层，单层通道只剩第 0 层 → 整幅画面黑）
    for (int st = 0; st < nst && st < FD_MAX_STAGES; st += 2)
        if (fp.stage[st].present && s2a_spirv_writes_layer((const uint32_t *)fp.stage[st].spirv, fp.stage[st].spirv_bytes)) e->prog.layer_out = 1;
    // 出包预编库按阶段分别取、拼成一份，拼时不过链接（现编走 linked 请求、缺的输出由编译器补）：片元读了上一段没写的
    //   位置，驱动按规范拒建管线（Adreno：input v_locN not declared in output from previous stage）。这份不用库，调用方
    //   libpack_mark_bad 后走现编——同「状态 / 声明对得上才用预编库」。
    if (lib && nst >= 2) {
        const struct fd_stage *pre = nst >= 3 && fp.stage[2].present ? &fp.stage[2] : &fp.stage[0];
        uint64_t miss = 0;
        if (!nvn_vk_spirv_link_ok(pre->spirv, pre->spirv_bytes, fp.stage[1].spirv, fp.stage[1].spirv_bytes, &miss)) {
#ifdef S2A_ANDROID_DEBUG_ENV
            record_link_refusal(views, nst, miss);
#endif
            uint64_t k = atomic_fetch_add(&g_lp_link_refused, 1);
            if (k < 4) { char m[200]; snprintf(m, sizeof m, "出包预编程序两段接口对不上：片元读的位置 %#llx 上一段没写（身份 %02x%02x%02x%02x…），这份退回现编",
                (unsigned long long)miss, identity[0], identity[1], identity[2], identity[3]); nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", m); }
            nvn_vk_program_free(&fp); e->failed = 1; return e;
        }
    }
    { static _Atomic int said; if (nst >= 2 && nvn_plat_flag("nvk_drawlog", 0) && atomic_fetch_add(&said, 1) < 40) { char m[96]; snprintf(m, sizeof m, "片元输出映射 %#x（片元代码 %zu 字节）", (unsigned)e->prog.color_out_mask, fp.stage[1].spirv_bytes); nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", m); } }
    for (int st = 0; st < nst && !e->failed; st++) {
        if (!fp.stage[st].present) { red1("stage_missing_in_result", st ? "fs" : "vs"); e->failed = 1; break; }
        for (int i = 0; i < fp.binding_count && !e->failed; i++) {
            const struct fd_binding *b = &fp.bindings[i];
            if (!(b->is_support || b->stage_idx == st)) continue;
            if (e->prog.nent[st] == RP_PROG_ENT_MAX) { red1("too_many_bindings", ""); e->failed = 1; break; }
            nvn_gpu_layout_ent *le = &e->prog.ent[st][e->prog.nent[st]];
            if (!layout_ent(b, le)) {
                char nm[40], wy[96]; const uint8_t *k = views[st].key ? views[st].key : identity;
                snprintf(nm, sizeof nm, "binding_kind_%d_slot_%d", b->kind, b->slot);
                with_program_key(wy, sizeof wy, b->stage[0] ? b->stage : "?", k);
                red1(nm, wy); e->failed = 1; break;
            }
            if ((le->src_kind == NVN_RB_TEXU || le->src_kind == NVN_RB_TBUFU) && !usrc_put(&e->prog, b, le)) { red1("too_many_uniform_handles", b->stage); e->failed = 1; break; }
            if (le->cls == NVN_GPU_RES_TBUF && le->src_kind != NVN_RB_TBUF) { red1("texture_buffer_replay_unsupported", b->stage); e->failed = 1; break; }
            le->stages = b->is_support ? (1u << nst) - 1u : 1u << st;
            if (b->kind == 2) sampled_decl(fp.stage[st].spirv, fp.stage[st].spirv_bytes, (uint32_t)b->set, (uint32_t)b->binding,
                &e->prog.dtt[st][e->prog.nent[st]], &e->prog.dcls[st][e->prog.nent[st]], NULL);
            // 图形阶段的存储图像（含 imageBuffer）：回放按着色器声明的目标 / 数值类 / 格式选视图（image_resolve / image_tbuf_resolve，同计算阶段 build_compute）；
            //   声明没解析出来（目标记录为 0）时回放照旧用贴图自己的格式
            if (le->src_kind == NVN_RB_IMG) sampled_decl(fp.stage[st].spirv, fp.stage[st].spirv_bytes, (uint32_t)b->set, (uint32_t)b->binding,
                &e->prog.dtt[st][e->prog.nent[st]], &e->prog.dcls[st][e->prog.nent[st]], &e->prog.difmt[st][e->prog.nent[st]]);
            if (le->cls == NVN_GPU_RES_TBUF && e->prog.dtt[st][e->prog.nent[st]] != NVN_TEXTURE_TARGET_BUFFER + 1) {
                red1("tbuf_shader_type_unproven", b->stage); e->failed = 1; break;
            }
            if (le->cls == NVN_GPU_RES_IMAGE_TBUF && e->prog.dtt[st][e->prog.nent[st]] != NVN_TEXTURE_TARGET_BUFFER + 1) {
                red1("image_tbuf_shader_type_unproven", b->stage); e->failed = 1; break;
            }
            e->prog.nent[st]++;
        }
        if (e->failed) break;
        nvn_gpu_layout L = {0}; L.ent = e->prog.ent[st]; L.nent = e->prog.nent[st]; L.support_bytes = fp.support_bytes; L.id_src = "s2a-direct-program-v1";
        nvn_gpu_shader_src src = {0}; src.stage = (nvn_gpu_stage)st; src.kind = NVN_GPU_BLOB_SPIRV;
        src.blob = fp.stage[st].spirv; src.bytes = fp.stage[st].spirv_bytes; src.entry = "main"; src.layout = &L;
        src.precompiled = (uint8_t)lib;
        if (views[st].key) memcpy(src.key, views[st].key, 8);
        nvn_gpu_err err = {0};
        nvn_gpu_rc lr = nvn_gpu_shader_load(&src, &e->prog.sh[st], &err);
        if (!nvn_gpu_ok(lr)) { red_load(lr, &src, "shader_load_failed", err.why); e->failed = 1; }
        prog_c1_take(&e->prog, st, views[st].c1, views[st].c1_bytes);
    }
    nvn_vk_program_free(&fp);
    if (e->failed) for (int st = 0; st < RP_PROG_STAGES; st++) if (e->prog.sh[st]) { nvn_gpu_shader_release(e->prog.sh[st]); e->prog.sh[st] = NULL; }
    return e;
}

// ---- 解析的不可变输入快照（三段式的第一段）----
// 提交线程只做「读 rp_S 填事实 / 算内容身份 / pin 住源引用」，之后工作函数只吃快照：
// 预热带（[后台]）与提交线程跑的是同一份 work_run，后台因此永远不读 rp_S、不编码命令、不提交。
// 出包预编库按一组固定的特化状态翻译（translator/dmsh_xlate：深度不改写 = near 为 0、片元坐标不做 Y 取反、透明测试 Always、
//   不开 alpha 转覆盖、无双源混合；Metal 那边按入口补深度改写，这里没有）。状态不同时取到的 SPIR-V 与现场编的语义不一致：
//   11-14 王国之泪深度为 near=-w，用了预编库的图形程序画面整片暗 / 缺。只在状态相符时用（状态都进了程序身份，身份即状态）。
//   深度例外：设备能把管线裁剪深度设成 [-1, 1]（CAP_DEPTH_CLIP_CTL）时 near=-w 也能用，程序标 z_raw，rp_draw 置 clip_neg1
static int depth_clip_cap(void) {   // 1 = 有；能力还没取到时按没有（0），取到后定死
    static _Atomic int v = -1; int c = atomic_load(&v);
    if (c < 0) { nvn_gpu_caps cp; nvn_gpu_caps_get(&cp); c = (cp.caps & NVN_GPU_CAP_DEPTH_CLIP_CTL) != 0; if (!cp.caps) return 0; atomic_store(&v, c); }
    return c;
}
enum libpack_graphics_state { LIBPACK_OTHER_STATE, LIBPACK_ALIGNED, LIBPACK_SSBO_WINDOW };
static enum libpack_graphics_state libpack_state(const struct s2a_compile_graphics_src *g) {
    if ((g->depth_mode && !depth_clip_cap()) || g->y_negate || g->alpha_to_coverage_enable ||
        (g->alpha_test_compare && strcmp(g->alpha_test_compare, "Always")) || g->dual_source_blend)
        return LIBPACK_OTHER_STATE;
    return g->has_unaligned_ssbo && g->unaligned_ssbo ? LIBPACK_SSBO_WINDOW : LIBPACK_ALIGNED;
}
static void libpack_note_state(struct nvk_prog_rec *rec, const struct s2a_compile_graphics_src *g,
                               const struct rp_prog *prog) {
    enum libpack_graphics_state state = libpack_state(g);
    rec->lp_ssbo_window = state == LIBPACK_SSBO_WINDOW && prog->lib_sourced;
    rec->lp_ok = state == LIBPACK_ALIGNED || rec->lp_ssbo_window ? 2 : 0;
}
// 状态闸的五个来源（位）：libpack_state 不是 OTHER_STATE 就是「一位都没有」；现编原因日志按位报出是哪几个挡的。
enum { LPG_DEPTH = 1, LPG_Y_NEGATE = 2, LPG_ALPHA_TEST = 4, LPG_A2C = 8, LPG_DUAL_SRC = 16 };
static unsigned libpack_state_gate_bits(const struct s2a_compile_graphics_src *g) {
    unsigned bits = 0;
    if (g->depth_mode && !depth_clip_cap()) bits |= LPG_DEPTH;
    if (g->y_negate) bits |= LPG_Y_NEGATE;
    if (g->alpha_test_compare && strcmp(g->alpha_test_compare, "Always")) bits |= LPG_ALPHA_TEST;
    if (g->alpha_to_coverage_enable) bits |= LPG_A2C;
    if (g->dual_source_blend) bits |= LPG_DUAL_SRC;
    // 非对齐存储缓冲不再是状态闸：带窗口合同的库照用，旧库在取库处具名拒（原因进库层的 why），所以这里没有 LPG_UNALIGNED_SSBO 这一位
    return bits;
}
// 可选的直连诊断在调试包显式启用时记录每次现场编译。
__attribute__((weak)) int s2a_direct_diag_enabled(void);
__attribute__((weak)) uint64_t s2a_direct_diag_compile_begin(const uint8_t *, unsigned, int, int);
__attribute__((weak)) void s2a_direct_diag_compile_end(uint64_t, int);
// 配对接口表诊断（开关 nvk_link_map_diag，缺省关）：新编出的配对 VS+FS 每对打一行，两侧各自实际发射的
// 「客体逻辑 location → 宿主 Location」（编译器在结果信封外层 linkMap 里带出，不进 content / 缓存身份），
// 带程序键（两段 8 字节阶段键）与输入身份前 8 字节；SPIR-V 没有变量名时用它直接核两侧对应关系。只打前 LINK_MAP_DIAG_MAX 对。
// 档位：1 全打；2 只打任一侧 ≥2 个 varying 或两侧对不上的；3 只打两侧对不上的（标题 / 菜单那些单 varying 程序不再占满名额）。
#define LINK_MAP_DIAG_MAX 40
static _Atomic int g_link_map_said;
static int link_map_diag_want(void) {
    return nvn_plat_flag("nvk_link_map_diag", 0) && atomic_load_explicit(&g_link_map_said, memory_order_relaxed) < LINK_MAP_DIAG_MAX;
}
// 一侧 [[逻辑, 宿主], …] → "0>0,3>1"；格式不对写 "?"（诊断只读，不影响建程序）。
static void link_map_side(struct s2a_jv map, const char *side, char *out, size_t cap) {
    struct s2a_jv arr; int n; size_t o = 0; out[0] = 0;
    if (s2a_jv_obj_find(map, side, &arr) || (n = s2a_jv_arr_count(arr)) < 0) { snprintf(out, cap, "?"); return; }
    if (!n) { snprintf(out, cap, "-"); return; }
    for (int i = 0; i < n && o < cap; i++) {
        struct s2a_jv pair, a, b; uint32_t lg, ph;
        if (s2a_jv_arr_item(arr, i, &pair) || s2a_jv_arr_count(pair) != 2 || s2a_jv_arr_item(pair, 0, &a) || s2a_jv_arr_item(pair, 1, &b) ||
            s2a_jv_u32(a, &lg) || s2a_jv_u32(b, &ph)) { snprintf(out, cap, "?"); return; }
        int w = snprintf(out + o, cap - o, "%s%u>%u", i ? "," : "", lg, ph);
        if (w < 0) return;
        o += (size_t)w;
    }
}
// 「0>0,3>1」逐项拆开：FS 的每一项（逻辑>宿主）都必须在 VS 里原样出现，否则算对不上；「?」一律算对不上（看不懂就打出来）。
static int link_map_count(const char *t) { if (!t[0] || !strcmp(t, "-")) return 0; int n = 1; for (; *t; t++) n += *t == ','; return n; }
static int link_map_mismatch(const char *vs, const char *fs) {
    if (!strcmp(vs, "?") || !strcmp(fs, "?")) return 1;
    if (!strcmp(fs, "-")) return 0;
    const char *p = fs;
    while (*p) {
        const char *q = strchr(p, ','); size_t n = q ? (size_t)(q - p) : strlen(p);
        int found = 0;
        for (const char *v = vs; *v && !found; ) {
            const char *w = strchr(v, ','); size_t m = w ? (size_t)(w - v) : strlen(v);
            if (m == n && !strncmp(v, p, n)) found = 1;
            v = w ? w + 1 : v + m;
        }
        if (!found) return 1;
        p = q ? q + 1 : p + n;
    }
    return 0;
}
static int link_map_interesting(const char *vs, const char *fs, int level) {
    if (link_map_mismatch(vs, fs)) return 1;
    return level == 2 && (link_map_count(vs) >= 2 || link_map_count(fs) >= 2);
}
static void link_map_note(const uint8_t *bytes, size_t len, const char *keys_hex, const uint8_t identity[32]) {
    struct s2a_jv top = {(const char *)bytes, len}, map;
    char vs[256], fs[256], m[sizeof vs + sizeof fs + 192];   // 固定文字 + 键 + 身份不到 192 字节，两侧表各自最长 255
    if (s2a_jv_obj_find(top, "linkMap", &map)) { snprintf(vs, sizeof vs, "?"); snprintf(fs, sizeof fs, "?"); }
    else { link_map_side(map, "vs", vs, sizeof vs); link_map_side(map, "fs", fs, sizeof fs); }
    {   // 档位（nvk_link_map_diag）：1 全打；2 只打「任一侧 ≥2 个 varying 或两侧对不上」；3 只打「两侧对不上」。不合档的不占名额
        const int level = nvn_plat_flag("nvk_link_map_diag", 0);
        if (level >= 2 && !link_map_interesting(vs, fs, level)) return;
    }
    if (atomic_fetch_add_explicit(&g_link_map_said, 1, memory_order_relaxed) >= LINK_MAP_DIAG_MAX) return;
    snprintf(m, sizeof m, "配对接口表 程序键=vs:%.16s fs:%.16s 身份=%02x%02x%02x%02x%02x%02x%02x%02x VS输出(逻辑>宿主)=%s FS输入(逻辑>宿主)=%s",
             keys_hex, keys_hex + 16, identity[0], identity[1], identity[2], identity[3], identity[4], identity[5], identity[6], identity[7], vs, fs);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", m);
}
static uint64_t diag_begin(const uint8_t *id, unsigned stages, int submit) {
    return s2a_direct_diag_enabled && s2a_direct_diag_compile_begin && s2a_direct_diag_compile_end && s2a_direct_diag_enabled()
        ? s2a_direct_diag_compile_begin(id, stages, submit, 0) : 0;
}
static void diag_end(uint64_t token, int success) {
    if (token && s2a_direct_diag_compile_end) s2a_direct_diag_compile_end(token, success);
}
struct work_snap {
    uint64_t epoch;
    uint32_t stages;
    int nst;
    uint32_t tex_pool_n, samp_pool_n;
    int tf_on;
    struct facts f;                          // 编译事实（gfx + textures 的字符串存储都在这里）
    uint8_t identity[32];
    uint8_t keys[3][8];
    struct s2a_direct_compile_context pc;    // profile_json / closure / profile_hash 指向本结构内的私有副本
    struct s2a_source_entry_view views[3];   // acquire 的 pin；build() 从这里逐字节拷 c1
    int acquired;
    enum libpack_graphics_state lp_state;     // 快照时按事实算好：工作线程不碰 rp_S
    char *profile_json, *closure, *profile_hash;
};
static void snap_free(struct work_snap *s) {
    if (!s) return;
    if (s->acquired) for (int st = 0; st < s->nst; st++) s2a_source_release(s->views[st].ref);
    free(s->profile_json); free(s->closure); free(s->profile_hash);
    free(s);
}
// [提交线程] 捕获。NULL = 源快照缺失 / 内存不足（调用方准确失败，不猜、不退化成短 key）。
static struct work_snap *snap_capture(const struct facts *f, const struct s2a_input_identity *id, const uint8_t sha[32],
                                      const struct s2a_direct_compile_context *pc, uint32_t stages, int nst) {
    struct work_snap *s = (struct work_snap *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->f = *f;
    for (int i = 0; i < s->f.ntex; i++) {   // 指针修正：事实里的字符串原本指向提交线程的 static 存储
        s->f.tex[i].sampler_type = s->f.tsmp[i];
        s->f.tex[i].format = s->f.tfmt[i];
    }
    s->stages = stages; s->nst = nst; s->lp_state = libpack_state(&s->f.gfx);
    memcpy(s->identity, sha, 32);
    for (int st = 0; st < nst; st++) memcpy(s->keys[st], id->keys[st], 8);
    s->tex_pool_n = id->texture_array_length;
    s->samp_pool_n = id->sampler_array_length;
    s->tf_on = id->transform_feedback_enabled;
    s->pc = *pc;
    s->profile_json = (char *)malloc(pc->profile_bytes + 1);
    s->closure = dup_str(pc->translator_closure);
    s->profile_hash = dup_str(pc->profile_hash);
    if (!s->profile_json || !s->closure || (pc->profile_hash && !s->profile_hash)) { snap_free(s); return NULL; }
    memcpy(s->profile_json, pc->profile_json, pc->profile_bytes);
    s->profile_json[pc->profile_bytes] = 0;
    s->pc.profile_json = s->profile_json;
    s->pc.translator_closure = s->closure;
    s->pc.profile_hash = s->profile_hash;
    s->acquired = 1;   // 先置位：中途失败时 snap_free 会释放已经 pin 住的阶段
    for (int st = 0; st < nst; st++) {
        if (!s2a_source_acquire(s->keys[st], &s->views[st]) || !s->views[st].complete) {
            snap_free(s);
            return NULL;
        }
    }
    return s;
}
// ---- 出包预编库（LibPack，shaders/direct/b-<DMSH_CHAIN>-vk，出包 direct 段编好随包装进来；同 iOS 按每阶段 8 字节键取）----
// 提交线程与预热带都会来取：容器句柄与「取到但 build 不过」表都在 g_lp_mu 下（取一次是 mmap 读一条，锁粒度够用）。
// 取到的 v1 程序走同一个 build；build 不过（绑定映射 / 解析不认）记进不可用表、退回现编——不把失败记成这份输入的终态。
// 跨局缓存排在它前面：那是本机按现场事实编的，比出包推导的准。开关 debug.s2a.no_libpack=1 关掉（对照用）
static pthread_mutex_t g_lp_mu = PTHREAD_MUTEX_INITIALIZER;
// 本线程最近一次 libpack_fetch 过闸之后的结局（只作诊断，图形现编原因日志用；进函数清零）：库不在 / 记坏表 / 没有这一键 / 被拒收（带原因文本）/ 取到
enum { LPL_UNSET, LPL_CONTAINER_MISSING, LPL_CONTAINER_BAD, LPL_BAD_TABLE, LPL_MISS, LPL_REJECT, LPL_HIT };
static _Thread_local struct { int code; char why[192]; } g_lp_last;
static nvn_hmap g_lp_bad; static int g_lp_bad_init;
static _Atomic uint64_t g_n_libpack, g_n_libpack_bad, g_lp_unaligned_refused;
// 计算程序「为什么没走出包预编库」的备忘（只作诊断，不影响取程序）：按计算阶段键记最近 64 个，回放层的 spill 派发行与调查用。
//   命中库的键撤掉旧备忘；备忘满了按轮转覆盖。查表的是回放线程、写表的是提交线程 / 预热线程，一把小锁。
enum { CSN_NONE, CSN_UNALIGNED, CSN_SHARED, CSN_MISS, CSN_REJECT };
#define CS_NOTE_N 64
static pthread_mutex_t g_cs_note_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { uint8_t key[8]; uint8_t why; } g_cs_note[CS_NOTE_N];
static unsigned g_cs_note_next;
static void cs_note_set(const uint8_t key[8], int why) {
    pthread_mutex_lock(&g_cs_note_mu);
    int at = -1;
    for (int i = 0; i < CS_NOTE_N; i++) if (g_cs_note[i].why && !memcmp(g_cs_note[i].key, key, 8)) { at = i; break; }
    if (!why) { if (at >= 0) g_cs_note[at].why = 0; }
    else {
        if (at < 0) at = (int)(g_cs_note_next++ % CS_NOTE_N);
        memcpy(g_cs_note[at].key, key, 8); g_cs_note[at].why = (uint8_t)why;
    }
    pthread_mutex_unlock(&g_cs_note_mu);
}
// 回放层（rp_draw.c，弱链接）用：这个计算阶段键最近一次没走库的原因；没有记录 / 已命中库返回 NULL
const char *nvk_shader_cs_lib_note(const uint8_t key[8]);
const char *nvk_shader_cs_lib_note(const uint8_t key[8]) {
    static const char *const text[] = {NULL, "库闸：存储缓冲非对齐（只有对齐变体）", "库闸：共享内存超设备上限（出库拒，只能现编改写 spill）", "库里没有这一键", "库件被拒收"};
    int why = 0;
    pthread_mutex_lock(&g_cs_note_mu);
    for (int i = 0; i < CS_NOTE_N; i++) if (g_cs_note[i].why && !memcmp(g_cs_note[i].key, key, 8)) { why = g_cs_note[i].why; break; }
    pthread_mutex_unlock(&g_cs_note_mu);
    return text[why];
}
// 库状态（「程序累计」行的「库」列）：没查过 / 已打开 / 不在 / 坏。目录不在原先静默返回，读数只剩「命中 0 / 没有 0」，
// 分不出「资源包没带这一层」和「库里没有这个键」（offline-shader-miss 调查：测试包全是前者）。
enum { LP_ST_UNSEEN, LP_ST_OPEN, LP_ST_MISSING, LP_ST_BAD };
static _Atomic int g_lp_state;
static const char *const k_lp_state[] = {"没查过", "已打开", "不在", "坏"};
static struct s2a_libpack *libpack_for_locked(const char *root) {
    static struct s2a_libpack *lp; static char lp_root[1024], said_root[1024]; static int lp_red;
    if (!lp || strcmp(lp_root, root)) {
        char dir[1200]; snprintf(dir, sizeof dir, "%s/shaders/direct/b-%s-vk", root, DMSH_CHAIN);
        struct s2a_libpack *np = NULL; int orc = s2a_libpack_open(&np, dir);
        if (orc == 1) { s2a_libpack_close(lp); lp = np; snprintf(lp_root, sizeof lp_root, "%s", root); atomic_store(&g_lp_state, LP_ST_OPEN);
            char b[1300]; snprintf(b, sizeof b, "出包预编库已打开：%s", dir); nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", b); }
        else {
            s2a_libpack_close(lp); lp = NULL; lp_root[0] = 0; atomic_store(&g_lp_state, orc < 0 ? LP_ST_BAD : LP_ST_MISSING);
            if (orc < 0) { if (!lp_red++) red1("libpack_container_rejected", dir); }
            else if (strcmp(said_root, root)) {   // 不在：每个资源根只说一次（之后每次取程序都还会再试开，不重复打）
                snprintf(said_root, sizeof said_root, "%s", root);
                char b[1400]; snprintf(b, sizeof b, "【黄】出包预编库不在：%s（资源包没带这一层：本局出包预编一份都取不到，首遇的程序只能靠跨局缓存或现场编）", dir);
                nvn_plat_log(NVN_PLAT_LOG_WARN, "shader", b);
            }
        }
    }
    return lp;
}
// 取一份（图形：stages 是只有 vs 0x1、vs+fs 0x3 或 vs+fs+gs 0x7，keys 按 NVN 位序紧排 vs / fs / gs；计算：ckey 非空）。1 = 取到（*bytes 归调用方 free）
//   topology：本笔绘制的输入图元名（fill_facts 的 topology()，快照里已算好——工作线程不碰 rp_S）；几何程序按它挑离线库里同图元那份，
//   没有来源（NULL）几何程序 miss。本局 profile 的 geometryShader 没开时 libpack 照旧 miss（与现编同一字段）
// Private diagnostic APK. Log intervals on the same boot clock as v5 present records.
struct stair_probe { uint64_t start, fetched; const char *format, *role; };
static _Atomic uint64_t stair_sequence;
static uint64_t stair_us(void) {
    struct timespec t;
#ifdef CLOCK_BOOTTIME
    clock_gettime(CLOCK_BOOTTIME, &t);
#else
    clock_gettime(CLOCK_MONOTONIC, &t);
#endif
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}
static struct stair_probe stair_begin(int attempted, const char *role) {
    struct stair_probe p = {0, 0, "none", role};
    if (attempted && nvn_plat_flag("nvk_libpack_timing", 0)) p.start = stair_us();
    return p;
}
static void stair_end(struct stair_probe *p, const char *role, const uint8_t *id, unsigned stages, int ok) {
    if (!p->start) return;
    uint64_t end = stair_us();
    uint64_t seq = atomic_fetch_add(&stair_sequence, 1) + 1;
    char line[256];
    snprintf(line, sizeof line, "S2SB_EVT seq=%llu role=%s format=%s ok=%d st=%u id=%02x%02x%02x%02x%02x%02x%02x%02x start=%llu fetch=%llu end=%llu",
             (unsigned long long)seq, role, p->format, ok, stages, id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
             (unsigned long long)p->start, (unsigned long long)(p->fetched ? p->fetched : end), (unsigned long long)end);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", line);
}
static int fetch_binary_program(struct s2a_libpack *lp, const uint8_t keys[6][8], unsigned stages,
                                const struct s2a_libpack_gfx *gfx, uint32_t shared, int abi,
                                union lp_prepared **ready, char *why, size_t cap) {
    if (!lp || !s2a_libpack_is_binary(lp)) {
        snprintf(why, cap, "missing S2SB container");
        return S2A_LIBPACK_MISS;
    }
    if (!nvn_plat_flag("nvk_binary_libpack", 1)) {
        snprintf(why, cap, "binary stage library disabled");
        return S2A_LIBPACK_MISS;
    }
    if (abi != S2A_DIRECT_ABI || !(*ready = malloc(sizeof **ready))) {
        snprintf(why, cap, "%s", abi != S2A_DIRECT_ABI ? "binary stage session ABI mismatch" : "binary stage allocation failed");
        return S2A_LIBPACK_REJECT;
    }
    int rc = s2a_libpack_binary_program(lp, keys, stages, gfx, shared,
        stages == 32 ? NULL : &(*ready)->graphics, stages == 32 ? &(*ready)->compute : NULL, why, cap);
    if (rc) { free(*ready); *ready = NULL; }
    return rc;
}

#ifdef S2A_ANDROID_DEBUG_ENV
// Caller holds g_lp_mu. Opening the companion is outside both per-program intervals.
static struct s2a_libpack *pair_binary_for_locked(const char *root) {
    static struct s2a_libpack *binary;
    static char previous[1024];
    if (binary && !strcmp(previous, root)) return binary;
    s2a_libpack_close(binary);
    binary = NULL;
    char dir[1200];
    if (snprintf(dir, sizeof dir, "%s/shaders/direct/qa-s2sb/b-%s-vk", root, DMSH_CHAIN) >= (int)sizeof dir ||
        strlen(root) >= sizeof previous) return NULL;
    if (s2a_libpack_open(&binary, dir) != 1) return NULL;
    snprintf(previous, sizeof previous, "%s", root);
    return binary;
}

static int fetch_pair(struct s2a_libpack *json, struct s2a_libpack *binary, const uint8_t keys[6][8],
                      unsigned stages, const struct s2a_libpack_gfx *gfx, uint32_t shared,
                      const struct s2a_direct_compile_context *pc, const uint8_t identity[32], const char *role,
                      union lp_prepared **ready, char *why, size_t cap) {
    static uint64_t sequence;
    uint64_t seq = ++sequence;
    int json_first = seq & 1;
    uint64_t t[3];
    int json_rc = S2A_LIBPACK_MISS, bin_rc = S2A_LIBPACK_MISS;
    uint8_t *bytes = NULL;
    size_t len = 0;
    char shadow_why[192] = "";
    union lp_prepared *shadow = NULL;
    t[0] = stair_us();
    for (unsigned i = 0; i < 2; i++) {
        if ((i == 0) == json_first) {
            if (json && !s2a_libpack_is_binary(json))
                json_rc = s2a_libpack_shadow_json(json, keys, stages, gfx, pc->direct_abi, pc->profile_hash,
                    pc->translator_closure, shared, &bytes, &len, shadow_why, sizeof shadow_why);
            if (json_rc == S2A_LIBPACK_HIT) {
                shadow = calloc(1, sizeof *shadow);
                if (!shadow) json_rc = S2A_LIBPACK_REJECT;
                else if (stages == 32)
                    json_rc = nvn_vk_program_parse_compute(bytes, len, &shadow->compute, shadow_why, sizeof shadow_why);
                else
                    json_rc = nvn_vk_program_parse_graphics_stages(bytes, len, &shadow->graphics, stages == 1, shadow_why, sizeof shadow_why);
            }
        } else {
            bin_rc = fetch_binary_program(binary, keys, stages, gfx, shared, pc->direct_abi, ready, why, cap);
        }
        t[i + 1] = stair_us();
    }
    if (shadow) {
        if (stages == 32) nvn_vk_program_free_compute(&shadow->compute);
        else nvn_vk_program_free(&shadow->graphics);
        free(shadow);
    }
    free(bytes);
    char line[384], id_hex[65];
    for (unsigned i = 0; i < 32; i++) snprintf(id_hex + 2*i, 3, "%02x", identity[i]);
    snprintf(line, sizeof line, "S2A_LIBPAIR seq=%llu role=%s st=%u id=%s first=%s same_source=%d json_rc=%d s2sb_rc=%d json_us=%llu s2sb_us=%llu start=%llu end=%llu",
        (unsigned long long)seq, role, stages, id_hex, json_first ? "json" : "s2sb", s2a_libpack_same_producer(json, binary), json_rc, bin_rc,
        (unsigned long long)(json_first ? t[1]-t[0] : t[2]-t[1]), (unsigned long long)(json_first ? t[2]-t[1] : t[1]-t[0]),
        (unsigned long long)t[0], (unsigned long long)t[2]);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", line);
    return bin_rc;
}
#endif

static int libpack_fetch(const uint8_t identity[32], const uint8_t (*gkeys)[8], uint32_t stages, const uint8_t *ckey, int state_ok,
                         const char *topology, const struct s2a_direct_compile_context *pc, uint8_t **bytes, size_t *len, union lp_prepared **ready, struct stair_probe *probe) {
    const char *root = getenv("S2A_SHADER_REVISION_ROOT");
    // 非对齐存储缓冲允许查库（图形 / 计算同口径），逐段核对窗口合同：只有带合同的库才放行，旧库（对齐变体）在库层具名 MISS 退现编。
    //   对齐变体跑在非对齐缓冲上会让窗口外越界读写 → 设备丢局（9000 日包 DeviceLost 元凶 = cs 50c91227a1082f2c 库版此机制），
    //   所以不能把旧库当对齐库用。debug.s2a.nvk_libpack_ssbo_window=0 回到旧门：图形不查库，计算按「对齐闸」拒查并记账。
    if (state_ok == LIBPACK_SSBO_WINDOW && !nvn_plat_flag("nvk_libpack_ssbo_window", 1)) { if (!ckey) return 0; state_ok = LIBPACK_OTHER_STATE; }
    g_lp_last.code = LPL_UNSET; g_lp_last.why[0] = 0;
    if (!root || !state_ok || (!ckey && stages != 0x1u && stages != 0x3u && stages != 0x7u) || nvn_plat_flag("no_libpack", 0)) {
        // 计算侧对齐闸的拒查有界具名（程序键 + 原因）：库根在、ckey 非空、state_ok 假 = 非对齐 sbuf 拒查，
        //   真机直接看到闸生效。每次取程序都可能撞上：一行一个事件只打前 8 条防日志洪泛，之后只累计。
        if (root && ckey && !state_ok) {
            uint64_t n = atomic_fetch_add_explicit(&g_lp_unaligned_refused, 1, memory_order_relaxed);
            cs_note_set(ckey, CSN_UNALIGNED);
            char kh[17]; for (int i = 0; i < 8; i++) snprintf(kh + 2 * i, 3, "%02x", ckey[i]);
            if (n < 8) {
                char b[256]; snprintf(b, sizeof b,
                    "libpack_cs_unaligned_refused 键 %s：计算取库因存储缓冲非对齐被对齐闸拒——库件只有对齐变体，退回现编（身份 %02x%02x%02x%02x…）",
                    kh, identity[0], identity[1], identity[2], identity[3]);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", b);
            } else if (n == 8)
                nvn_plat_log(NVN_PLAT_LOG_INFO, "shader",
                    "libpack_cs_unaligned_refused 行已截断（前 8 条逐条，之后每 64 条一行带最近键与身份的汇总）");
            else if (!((n + 1) & 63)) {   // 第 64、128… 次：一行汇总（累计次数 + 最近一次的键与身份），长局里仍能看到这个闸在咬哪些程序
                char b[256]; snprintf(b, sizeof b,
                    "libpack_cs_unaligned_refused 累计 %llu 次（最近一次：键 %s，身份 %02x%02x%02x%02x…）",
                    (unsigned long long)(n + 1), kh, identity[0], identity[1], identity[2], identity[3]);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", b);
            }
        }
        return 0;
    }
    char lw[192] = ""; int lrc = S2A_LIBPACK_MISS;
    pthread_mutex_lock(&g_lp_mu);
    if (!g_lp_bad_init) { nvn_hm_init(&g_lp_bad, 32); g_lp_bad_init = 1; }
    int blocked = nvn_hm_get(&g_lp_bad, identity) != NULL;
    struct s2a_libpack *lp = blocked ? NULL : libpack_for_locked(root);
    int pair = 0;
#ifdef S2A_ANDROID_DEBUG_ENV
    pair = !blocked && nvn_plat_flag("nvk_libpack_pair_timing", 0);
#endif
    if (lp || pair) {
        nvn_gpu_caps caps = {0};
        nvn_gpu_caps_get(&caps);
        int window_used = 0;   // 本次取库有没有阶段真用了窗口掩码（nvk_libpack_win_part 二分诊断用）
        const struct s2a_libpack_gfx g = {
            .topology = topology, .profile_json = pc->profile_json, .profile_bytes = pc->profile_bytes,
            .storage_alignment = caps.min_ssbo_align, .require_ssbo_window = state_ok == LIBPACK_SSBO_WINDOW,
            .window_used = &window_used,
            .window_tex_classes = (uint32_t)nvn_plat_flag("nvk_libpack_win_tex", 0),
        };
        struct nvk_compute_profile_facts pf = {0};
        uint8_t keys[6][8] = {{0}};
        if (ckey) memcpy(keys[0], ckey, 8);
        else memcpy(keys, gkeys, (stages == 0x7u ? 3u : stages == 0x3u ? 2u : 1u) * sizeof keys[0]);

        if (probe->start) probe->format = s2a_libpack_is_binary(lp) ? "s2sb" : "json";
        char profile_why[160] = "";
        if (ckey && nvk_compute_profile_parse(pc, &pf, profile_why, sizeof profile_why)) {
            snprintf(lw, sizeof lw, "compute profile: %s", profile_why[0] ? profile_why : "invalid");
            lrc = S2A_LIBPACK_REJECT;
#ifdef S2A_ANDROID_DEBUG_ENV
        } else if (pair) {
            probe->format = "s2sb";
            lrc = fetch_pair(lp, pair_binary_for_locked(root), keys, ckey ? 32u : stages, &g, pf.max_shared,
                             pc, identity, probe->role, ready, lw, sizeof lw);
#endif
        } else if (s2a_libpack_is_binary(lp)) {
            lrc = fetch_binary_program(lp, keys, ckey ? 32u : stages, &g, pf.max_shared,
                                       pc->direct_abi, ready, lw, sizeof lw);
        } else if (ckey) {
            lrc = s2a_libpack_compute_v1(lp, ckey, (uint32_t)pc->direct_abi, pc->profile_hash,
                                         pc->translator_closure, pf.max_shared, &g, bytes, len, lw, sizeof lw);
        } else {
            lrc = s2a_libpack_program_v1(lp, keys, stages, &g, (uint32_t)pc->direct_abi, pc->profile_hash,
                                         pc->translator_closure, bytes, len, lw, sizeof lw);
        }
        // 窗口状态程序的二分诊断（缺省 0 = 关，行为不变）：1 = 只让用了窗口掩码的程序走库（没用到的退回现编）；2 = 只让没用到窗口掩码的走库。
        //   9000 / 9400（Mali）发白、8E（Adreno）不发：窗口变体命中里 71% 的程序两个阶段都不带掩码，先分清嫌疑在哪一组。
        const int part = !ckey && g.require_ssbo_window && lrc == S2A_LIBPACK_HIT ? nvn_plat_flag("nvk_libpack_win_part", 0) : 0;
        if ((part == 1 && !window_used) || (part == 2 && window_used)) {
            if (*bytes) { free(*bytes); *bytes = NULL; *len = 0; }
            if (ready && *ready) { nvn_vk_program_free(&(*ready)->graphics); free(*ready); *ready = NULL; }
            snprintf(lw, sizeof lw, "window part gate %d: %s", part, window_used ? "program uses the window mask" : "program does not use the window mask");
            lrc = S2A_LIBPACK_MISS;
        }
    }
    if (!lp) g_lp_last.code = nvn_hm_get(&g_lp_bad, identity) ? LPL_BAD_TABLE : atomic_load(&g_lp_state) == LP_ST_BAD ? LPL_CONTAINER_BAD : LPL_CONTAINER_MISSING;
    else { g_lp_last.code = lrc == S2A_LIBPACK_HIT ? LPL_HIT : lrc == S2A_LIBPACK_MISS ? LPL_MISS : LPL_REJECT; snprintf(g_lp_last.why, sizeof g_lp_last.why, "%s", lw); }
    pthread_mutex_unlock(&g_lp_mu);
    if (!lp && !pair) return 0;
    if (ckey) cs_note_set(ckey, lrc == S2A_LIBPACK_HIT ? CSN_NONE : strstr(lw, "exceeds device limit") ? CSN_SHARED :
                                lrc == S2A_LIBPACK_MISS ? CSN_MISS : CSN_REJECT);
    if (lrc != S2A_LIBPACK_HIT) {   // 没找到 / 容器里这一键坏：前几条把原因打出来（键 = 每阶段 8 字节控制键）
        static _Atomic int nm; if (atomic_fetch_add(&nm, 1) < 6) {
            const uint8_t *k = ckey ? ckey : gkeys[0];
            char b[400]; snprintf(b, sizeof b, "出包预编库没取到%s（%s，键 %02x%02x%02x%02x…）：%s", ckey ? "计算程序" : "",
                lrc == S2A_LIBPACK_MISS ? "没有" : "坏", k[0], k[1], k[2], k[3], lw); nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", b); }
        return 0;
    }
    return 1;
}
// 取到但 build 不过：记住，下次直接走现编（不再取一遍、拼一遍）
static void libpack_mark_bad(const uint8_t identity[32]) {
    uint64_t n = atomic_fetch_add(&g_n_libpack_bad, 1) + 1;
    pthread_mutex_lock(&g_lp_mu);
    if (!g_lp_bad_init) { nvn_hm_init(&g_lp_bad, 32); g_lp_bad_init = 1; }
    if (!nvn_hm_get(&g_lp_bad, identity)) nvn_hm_put(&g_lp_bad, identity, (void *)1);
    pthread_mutex_unlock(&g_lp_mu);
    if (n <= 4) { char b[200]; snprintf(b, sizeof b, "出包预编程序不可用、退回现编（第 %llu 个，身份 %02x%02x%02x%02x…）", (unsigned long long)n,
        identity[0], identity[1], identity[2], identity[3]); nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", b); }
}
static void libpack_note_hit(void) { atomic_fetch_add(&g_n_libpack, 1); }
// 存储缓冲窗口变体的库命中逐条记一行（nvk_hitch_log 开时、前 512 条）：键（逐阶段 8 字节）+ 阶段掩码 + 身份前 4 字节 + 来源（提交 / 预热）。
//   9000 B 局发白（石阶 / 扶栏精确 ×2、8E 不发）时累计只给「命中 400」，没有逐键：对不上是哪几支材质程序走了窗口变体。
static void libpack_window_note(const uint8_t identity[32], const uint8_t (*keys)[8], int nst, uint32_t stages, const char *who) {
    static _Atomic unsigned said;
    if (!nvn_plat_flag("nvk_hitch_log", 0) || atomic_fetch_add_explicit(&said, 1, memory_order_relaxed) >= 512) return;
    char ks[64] = ""; size_t o = 0;
    for (int st = 0; st < nst && st < 3; st++) {
        for (int b = 0; b < 8 && o + 3 < sizeof ks; b++) o += (size_t)snprintf(ks + o, sizeof ks - o, "%02x", keys[st][b]);
        if (st + 1 < nst && st + 1 < 3 && o + 1 < sizeof ks) ks[o++] = '/';
    }
    char b[200]; snprintf(b, sizeof b, "库命中（存储缓冲窗口变体）：键 %s 阶段 0x%x 身份 %02x%02x%02x%02x…（%s）", ks, (unsigned)stages,
                          identity[0], identity[1], identity[2], identity[3], who);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", b);
}
// 程序累计一行：出包预编 / 离线库 / 跨局缓存 / 现场编 合计第 1 个和每 50 个打一次（每发布一份新程序合计正好加 1）
static void prog_tally_text(char *b, size_t bn) {
    int bo = snprintf(b, bn, "程序累计：出包预编（库%s）命中 %llu / 没有 %llu / 坏 %llu / 不可用 %llu（删 64 位能力声明 %llu）、离线库 %llu、跨局缓存 %llu、现场编 %llu、失败 %llu",
        k_lp_state[atomic_load(&g_lp_state)], (unsigned long long)atomic_load(&g_n_libpack), (unsigned long long)s2a_libpack_misses(), (unsigned long long)s2a_libpack_rejects(),
        (unsigned long long)atomic_load(&g_n_libpack_bad), (unsigned long long)s2a_libpack_caps_stripped(), (unsigned long long)atomic_load(&g_n_offline), (unsigned long long)atomic_load(&g_n_cache),
        (unsigned long long)atomic_load(&g_n_compiled), (unsigned long long)atomic_load(&g_n_failed));
    if (alias_on() && bo > 0 && (size_t)bo < bn) {   // 结果合并读数（开着才出）
        uint32_t resn = 0;
        pthread_mutex_lock(&g_progs_mu); resn = g_res_tab.n; pthread_mutex_unlock(&g_progs_mu);
        snprintf(b + bo, bn - (size_t)bo, "、结果合并 前台 %llu / 预热 %llu（读头 %llu、没有可合并的常驻程序 %llu、没有缓存文件 %llu、缓存文件坏 %llu；常驻比对键 %u 个）",
            (unsigned long long)atomic_load(&g_n_alias_fg), (unsigned long long)atomic_load(&g_n_alias_bg), (unsigned long long)atomic_load(&g_n_alias_probe),
            (unsigned long long)atomic_load(&g_n_alias_nores), (unsigned long long)atomic_load(&g_n_alias_nofile), (unsigned long long)atomic_load(&g_n_alias_badfile), (unsigned)resn);
    }
}
static void prog_tally_log(void) {
    uint64_t n = atomic_load(&g_n_libpack) + atomic_load(&g_n_offline) + atomic_load(&g_n_cache) + atomic_load(&g_n_compiled) + atomic_load(&g_n_alias_fg);
    if (n != 1 && (!n || n % 50)) return;
    char b[640]; prog_tally_text(b, sizeof b);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", b);
}
// 测试读数：当下的「程序累计」行（不受每 50 个打一次的节拍限制）；结果合并读头的三个计数（读头 / 没有缓存文件 / 缓存文件坏）
void nvk_shader_test_tally_line(char *out, size_t n) { if (out && n) prog_tally_text(out, n); }
void nvk_shader_test_alias_probe(uint64_t out[3]) {
    out[0] = atomic_load(&g_n_alias_probe); out[1] = atomic_load(&g_n_alias_nofile); out[2] = atomic_load(&g_n_alias_badfile);
}

// ---- 编译被拒时的上下文日志（#999；只加日志、不改任何行为）----
// 1) 每次被拒（图形阶段现场编译）打一行「拒绝原因 + 程序键」，会话内最多 REJECT_LINES_MAX 行：TRANSLATION_DIAGNOSTIC 这类拒收消息里没有程序键，
//    靠这一行对到具体程序。程序键 = 各阶段键的十六进制按阶段顺序连起来，与发给编译器的 requestId 是同一个字符串。
// 2) 拒收是「image 句柄没有同字直接事实」（消息里带 [kind=image word=N …]）时，各记一个具名计数并打一行 image_fact_reject。
//    消息里的常量缓冲槽号（「texture handle from cbuf slot N is not implemented」的 N）是 2（c2 驱动句柄表）时，
//    按这份请求当时记下的原因分类：
//    image_fact_unbound         图像槽没绑句柄（池第 0 项也没登记快照）
//    image_fact_no_texture      槽里有句柄，但这个贴图号没登记快照
//    image_fact_no_fact         登记了，但格式 / 目标出不了编译事实
//    image_fact_given           事实给了、编译器仍然拒（说明「缺这个事实」的读法不对，要换方向查）
//    image_fact_word_unexpected 字不在 72..86 的偶数里，或消息里的阶段不在这份程序里
//    槽号不是 2 时，word 是那一槽自己的字偏移，与 c2 图像句柄表的字 72 + 2×槽不是同一个坐标系，不拿它去查上面的记录，单独一类：
//    image_fact_cbuf_other      其它常量缓冲槽的 image 句柄；行里 cbuf=<槽号>、slot=-1
// 所有 image_fact_reject 行都带 cbuf=<槽号>。计数的含义是「被拒的编译次数」：同一份程序身份被拒后按身份缓存（fe->failed），只记第一次的原因，
// 不是逐绘制的分布。
static void reject_context(const struct work_snap *s, const char *reason, const char *message) {
    char key[3 * 16 + 1] = "", line[400];
    int o = 0;
    for (int st = 0; st < s->nst && st < 3; st++)
        for (int b = 0; b < 8; b++) o += snprintf(key + o, sizeof key - (size_t)o, "%02x", s->keys[st][b]);
    g_reject_total++;
    if (g_reject_lines < REJECT_LINES_MAX) {
        g_reject_lines++;
        snprintf(line, sizeof line, "texture_pool_reject_program reason=%.79s stages=%d key=%s", reason, s->nst, key);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", line);
    }
    const char *img = strstr(message, "[kind=image ");
    if (!img || strcmp(reason, "cbuf_handle_unsupported")) return;
    const int st = !strncmp(message, "vs:", 3) ? 0 : !strncmp(message, "fs:", 3) ? 1 : !strncmp(message, "gs:", 3) ? 2 : -1;   // 消息以阶段名开头
    // 常量缓冲槽号：只有 2 才是 c2 图像句柄表；槽号带高 16 位的打包值照原样记，不是 2 就不是 c2（没解出数字得 0，也不是 c2）。
    static const char slot_text[] = "texture handle from cbuf slot ";
    const char *cs = strstr(message, slot_text);
    const unsigned long cbuf = cs ? strtoul(cs + sizeof slot_text - 1, NULL, 10) : 0;
    const int c2 = cbuf == 2;
    const char *w = strstr(img, " word=");
    const unsigned long word = w ? strtoul(w + 6, NULL, 10) : 0;
    const int slot = c2 && w && word >= 72 && word <= 86 && !(word & 1) ? (int)((word - 72) / 2) : -1;   // 字 72 + 2×槽（仅 c2）
    const struct img_miss *m = st >= 0 && st < s->nst && slot >= 0 ? &s->f.img[st][slot] : NULL;
    const char *name = !c2 ? "image_fact_cbuf_other" : !m ? "image_fact_word_unexpected" : m->why == IMGF_UNBOUND ? "image_fact_unbound" :
                       m->why == IMGF_NO_TEXTURE ? "image_fact_no_texture" : m->why == IMGF_NO_FACT ? "image_fact_no_fact" : "image_fact_given";
    nvn_unimpl("shader", name, NULL);
    g_img_reject_total++;
    if (g_img_reject_lines >= REJECT_LINES_MAX) return;
    g_img_reject_lines++;
    int n = snprintf(line, sizeof line, "image_fact_reject reason=%s stage=%s cbuf=%lu slot=%d word=%lu key=%s", name + strlen("image_fact_"),
                     st == 0 ? "vs" : st == 1 ? "fs" : st == 2 ? "gs" : "?", cbuf, slot, word, key);
    if (m && n > 0 && (size_t)n < sizeof line) {
        n += snprintf(line + n, sizeof line - (size_t)n, " handle=0x%x texture=%d", (unsigned)m->handle, (int)m->id);
        if ((m->why == IMGF_NO_FACT || m->why == IMGF_GIVEN) && (size_t)n < sizeof line)
            snprintf(line + n, sizeof line - (size_t)n, " format=0x%x target=%u layers=%u", (unsigned)m->fmt, (unsigned)m->target, (unsigned)m->layers);
    }
    nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", line);
}

// 仅 worker 执行 managed 翻译。生命周期准入在 worker 本线程持有，与提交者是否还等结果无关。
static _Atomic uint64_t stair_exec_count;
static int stair_execute(uint64_t session, uint64_t generation, const char *json, size_t len, struct s2a_compile_outcome *out) {
    uint64_t start = nvn_plat_flag("nvk_libpack_timing", 0) ? stair_us() : 0;
    int rc = s2a_compile_request_execute(session, generation, json, len, out);
    if (start) {
        uint64_t end = stair_us();
        char line[128];
        snprintf(line, sizeof line, "S2SB_LIVE seq=%llu start=%llu end=%llu rc=%d",
                 (unsigned long long)(atomic_fetch_add(&stair_exec_count, 1) + 1),
                 (unsigned long long)start, (unsigned long long)end, rc);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", line);
    }
    return rc;
}
static int live_worker(void *ctx, uint64_t session, uint64_t generation, const char *json, size_t len,
                       uint8_t **bytes, size_t *bytes_len, char *why, size_t why_size) {
    (void)ctx;
    if (shader_enter()) return -1;
    struct s2a_compile_outcome oc = {0};
    int ran = shader_compile_admit() ? stair_execute(session, generation, json, len, &oc) : -1;
    const int ok = !ran && oc.bytes && oc.bytes_len;
    if (ok) {
        *bytes = oc.bytes;
        *bytes_len = oc.bytes_len;
        oc.bytes = NULL;
    } else snprintf(why, why_size, "%s", oc.summary[0] ? oc.summary : "compile stopped or produced no result");
    s2a_compile_outcome_free(&oc);
    shader_leave();
    return ok ? 0 : -1;
}

enum live_status { LIVE_OK, LIVE_REJECTED, LIVE_PENDING, LIVE_ABORT };
void rp_shader_live_compile_take(struct rp_shader_live_compile_stats *s) {
    s->enqueued = atomic_exchange_explicit(&g_live_enqueued, 0, memory_order_relaxed);
    s->wait_ns = atomic_exchange_explicit(&g_live_wait_ns, 0, memory_order_relaxed);
    s->pending = atomic_exchange_explicit(&g_live_pending, 0, memory_order_relaxed);
    s->fallback = atomic_exchange_explicit(&g_live_fallback, 0, memory_order_relaxed);
    s->timeout = atomic_exchange_explicit(&g_live_timeout, 0, memory_order_relaxed);
    s->ids[0] = 0;   // 本帧前几个现编身份（诊断属性开时才有）
    if (atomic_load_explicit(&g_live_ids_n, memory_order_relaxed)) {
        pthread_mutex_lock(&g_live_ids_mu);
        snprintf(s->ids, sizeof s->ids, "%s", g_live_ids); g_live_ids[0] = 0; g_live_ids_n = 0;
        pthread_mutex_unlock(&g_live_ids_mu);
    }
}

// Worker 的键按实际请求字节（包括 c1），而非 g_progs 的较粗身份。没就绪/队满/停局均不缓存成编译器拒收。
static enum live_status live_compile(uint64_t session, uint64_t generation, const char *json, size_t len,
                                     const uint8_t identity[32], unsigned stages, const struct rp_shader_req *q,
                                     struct s2a_compile_outcome *oc) {
    if (!shader_compile_admit()) {
        nvn_unimpl("shader", "compile_stopped", NULL);
        return LIVE_ABORT;
    }
    if (g_live_compile != 1) {
        if (g_texpool_diag) g_pool_attempts++;
        if (g_live_compile < 0) atomic_fetch_add_explicit(&g_live_fallback, 1, memory_order_relaxed);
        const uint64_t trace = diag_begin(identity, stages, 1);
        const int ran = stair_execute(session, generation, json, len, oc);
        const int ok = !ran && oc->bytes && oc->bytes_len;
        diag_end(trace, ok);
        return ok ? LIVE_OK : LIVE_REJECTED;
    }
    uint8_t key[32];
    s2a_sha256 hash;
    s2a_sha256_init(&hash);
    s2a_sha256_update(&hash, &session, sizeof session);
    s2a_sha256_update(&hash, &generation, sizeof generation);
    s2a_sha256_update(&hash, json, len);
    s2a_sha256_final(&hash, key);
    int w = s2a_compile_worker_poll(key, &oc->bytes, &oc->bytes_len, oc->summary, sizeof oc->summary);
    if (shader_stopped()) return LIVE_ABORT;
    if (w == 0 && oc->bytes && oc->bytes_len) return LIVE_OK;
    if (w == 2) return LIVE_REJECTED;
    if (w == 1) s2a_compile_worker_promote(key);
    uint64_t frame = 0;
    const int may_defer = q && q->compile_defer_frame && q->compile_defer_frame(&frame);
    const uint64_t start = now_ns();
    uint64_t budget = (uint64_t)g_live_unsafe_ms * 1000000;
    if (may_defer) {
        if (frame != g_live_budget_frame) {
            g_live_budget_frame = frame;
            g_live_spent_ns = 0;
        }
        budget = (uint64_t)g_live_frame_ms * 1000000;
        budget = g_live_spent_ns < budget ? budget - g_live_spent_ns : 0;
    }
    while (w == 3 && !shader_stopped()) {
        int sr = s2a_compile_worker_submit_async(session, generation, json, len, key, oc->summary, sizeof oc->summary);
        if (sr == 0) {
            atomic_fetch_add_explicit(&g_live_enqueued, 1, memory_order_relaxed);
            if (g_texpool_diag) g_pool_attempts++;
        }
        if (sr == 0 || sr == 2) {
            w = s2a_compile_worker_poll(key, &oc->bytes, &oc->bytes_len, oc->summary, sizeof oc->summary);
            break;
        }
        if (sr < 0) { w = -1; break; }
        const uint64_t elapsed = now_ns() - start;
        const uint32_t left = elapsed < budget ? (uint32_t)((budget - elapsed) / 1000000) : 0;
        if (!left) break;
        if (s2a_compile_worker_pump(left) < 0) break;
    }
    const uint64_t elapsed = now_ns() - start;
    const uint32_t wait_ms = elapsed < budget ? (uint32_t)((budget - elapsed) / 1000000) : 0;
    if (w == 1 && wait_ms)
        w = s2a_compile_worker_wait_async(key, wait_ms, &oc->bytes, &oc->bytes_len, oc->summary, sizeof oc->summary);
    const uint64_t spent = now_ns() - start;
    atomic_fetch_add_explicit(&g_live_wait_ns, spent, memory_order_relaxed);
    if (may_defer) g_live_spent_ns += spent;
    if (shader_stopped()) return LIVE_ABORT;
    if (w == 0 && oc->bytes && oc->bytes_len) return LIVE_OK;
    if (w == 2) return LIVE_REJECTED;
    if (may_defer && (w == 1 || w == 3)) {
        atomic_fetch_add_explicit(&g_live_pending, 1, memory_order_relaxed);
        return LIVE_PENDING;
    }
    atomic_fetch_add_explicit(&g_live_timeout, 1, memory_order_relaxed);
    red1(w == 1 ? "live_compile_timeout" : "live_compile_unavailable", "后台着色器在等待时限内没有可用结果");
    return LIVE_ABORT;
}

// ---- 图形现编原因日志（只加日志，默认行为不变；诊断属性 nvk_hitch_log 开时才记 / 打）----
// 图形取库的闸全是静默返回，跨局缓存的未命中原因也没带到日志：8E 石阶每局 1 帧 77–82 ms 是一支图形着色器现编，查不出为什么没走库。
//   每支落到现编的图形程序打一行：程序键（逐阶段 8 字节）+ 阶段掩码 + 身份前 4 字节 + 三处没命中的原因；现编帧行再带前几个身份。
//   原因码：库闸（无资源根 / no_libpack / 状态闸的哪几位 / 阶段掩码）、库没有这一键、库件被拒收、库件取到但 build 不过（已记坏）、
//   记坏表命中；跨局缓存：没查（关了 / 无目录）、无文件、读头失败（文件短 / 头不全 / magic / 版本）、坏（其余：身份不符 / 长度 / 摘要）。
static int lp_gate_text(char *out, size_t n, const char *root, int state_ok, const uint8_t *ckey, uint32_t stages, unsigned state_bits) {
    size_t o = 0; out[0] = 0;
    #define LPG_ADD(...) do { if (o < n) o += (size_t)snprintf(out + o, n - o, __VA_ARGS__); } while (0)
    if (!root) LPG_ADD("%s无资源根", o ? "、" : "");
    if (!state_ok) {
        LPG_ADD("%s状态闸[", o ? "、" : "");
        static const struct { unsigned bit; const char *name; } nm[] = {{LPG_DEPTH, "深度裁剪"}, {LPG_Y_NEGATE, "y取反"}, {LPG_ALPHA_TEST, "alpha测试"}, {LPG_A2C, "alpha转覆盖"}, {LPG_DUAL_SRC, "双源混合"}};
        int first = 1;
        for (unsigned i = 0; i < sizeof nm / sizeof nm[0]; i++) if (state_bits & nm[i].bit) { LPG_ADD("%s%s", first ? "" : "+", nm[i].name); first = 0; }
        LPG_ADD("]");
    }
    if (!ckey && stages != 0x3u && stages != 0x7u) LPG_ADD("%s阶段掩码0x%x", o ? "、" : "", (unsigned)stages);
    if (nvn_plat_flag("no_libpack", 0)) LPG_ADD("%sno_libpack", o ? "、" : "");
    #undef LPG_ADD
    return o != 0;
}
static const char *cache_why_class(const char *why) {
    if (!strncmp(why, "未命中", strlen("未命中"))) return "无文件";
    if (!strncmp(why, "文件短于头部", strlen("文件短于头部")) || !strncmp(why, "头部读不全", strlen("头部读不全")) ||
        !strncmp(why, "magic", 5) || !strncmp(why, "格式版本不符", strlen("格式版本不符"))) return "读头失败";
    return "坏";
}
static struct { _Atomic unsigned said; uint8_t recent[16][4]; unsigned recent_n; } g_live_reason;   // 提交线程独占（work_run 的现编分支）
static void live_reason_note(const struct work_snap *s, const char *root, int state_ok, int offline_rc, const char *cache_state, int build_failed) {
    if (!nvn_plat_flag("nvk_hitch_log", 0)) return;
    {   // 现编帧行带前几个身份（每帧最多 4 个，take 时清空）
        pthread_mutex_lock(&g_live_ids_mu);
        unsigned n = g_live_ids_n;
        if (n < 4) { size_t o = strlen(g_live_ids); snprintf(g_live_ids + o, sizeof g_live_ids - o, "%s%02x%02x%02x%02x", n ? "," : "", s->identity[0], s->identity[1], s->identity[2], s->identity[3]); g_live_ids_n = n + 1; }
        pthread_mutex_unlock(&g_live_ids_mu);
    }
    int seen = 0;   // 同一身份再进现编（暂缓后重试）只打一次
    for (unsigned i = 0; i < 16 && i < g_live_reason.recent_n; i++) if (!memcmp(g_live_reason.recent[i], s->identity, 4)) { seen = 1; break; }
    if (seen) return;
    memcpy(g_live_reason.recent[g_live_reason.recent_n % 16], s->identity, 4); g_live_reason.recent_n++;
    unsigned k = atomic_fetch_add_explicit(&g_live_reason.said, 1, memory_order_relaxed);
    if (k >= 128 && (k % 16)) return;   // 有界：前 128 条逐条，之后每 16 条一行
    char keys[64] = ""; size_t o = 0;
    for (int st = 0; st < s->nst && st < 3; st++) { for (int b = 0; b < 8 && o + 3 < sizeof keys; b++) o += (size_t)snprintf(keys + o, sizeof keys - o, "%02x", s->keys[st][b]); if (st + 1 < s->nst && o + 1 < sizeof keys) keys[o++] = '/'; }
    char lib[256], gate[160];
    if (lp_gate_text(gate, sizeof gate, root, state_ok, NULL, s->stages, libpack_state_gate_bits(&s->f.gfx))) snprintf(lib, sizeof lib, "库闸：%s", gate);
    else if (build_failed) snprintf(lib, sizeof lib, "库件取到但 build 失败（已记坏、退回现编）");
    else switch (g_lp_last.code) {
        case LPL_CONTAINER_MISSING: snprintf(lib, sizeof lib, "库不在"); break;
        case LPL_CONTAINER_BAD: snprintf(lib, sizeof lib, "库容器坏"); break;
        case LPL_BAD_TABLE: snprintf(lib, sizeof lib, "记坏表命中（此前 build 失败）"); break;
        case LPL_MISS: if (strstr(g_lp_last.why, "window part gate")) snprintf(lib, sizeof lib, "窗口二分闸挡下：%s", g_lp_last.why); else snprintf(lib, sizeof lib, "库里没有这一键"); break;
        case LPL_REJECT: snprintf(lib, sizeof lib, "库件被拒收：%s", g_lp_last.why); break;
        default: snprintf(lib, sizeof lib, "库没查"); break;
    }
    char b[480]; snprintf(b, sizeof b, "图形现编原因：键 %s 阶段 0x%x 身份 %02x%02x%02x%02x…：离线库 %s；跨局缓存 %s；出包库 %s", keys, (unsigned)s->stages,
        s->identity[0], s->identity[1], s->identity[2], s->identity[3], offline_rc == -9 ? "没查" : offline_rc == S2A_OFFLINE_MISS ? "未命中" : offline_rc == S2A_OFFLINE_REJECT ? "被拒收" : "无效", cache_state, lib);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", b);
}
// 测试用（宿主）：同一份原因文本函数
void nvk_shader_test_gate_text(char *out, size_t n, int has_root, unsigned state_bits, uint32_t stages, int ckey_present) {
    static const uint8_t k[8] = {0}; if (out && n) lp_gate_text(out, n, has_root ? "/r" : NULL, state_bits == 0, ckey_present ? k : NULL, stages, state_bits);
}
const char *nvk_shader_test_cache_class(const char *why) { return cache_why_class(why ? why : ""); }

// [提交] 只吃快照：离线库 → 跨局缓存 → 现场编译 → parse + 建后端着色器。
// 仅真实编译器拒收生成 failed entry；预算用尽和 Stop 不作永久失败缓存。
static struct entry *work_run(const struct work_snap *s, const struct rp_shader_req *q, int *pending) {
    uint8_t *bytes = NULL; size_t len = 0; char why[256] = "";
    int offline_rc = -9, build_failed = 0; const char *cache_state = "没查";   // 现编原因日志（只读，不改取程序顺序）
    uint8_t rsha[32]; int have_res = 0;   // 结果摘要（跨局缓存读到的已校验摘要 / 现编结果现算的）：挂在 entry 上给结果合并用，离线库 / 预编库来的不挂
    const char *root = getenv("S2A_SHADER_REVISION_ROOT"), *cdir = getenv("S2A_DIRECT_CACHE_DIR");
    if (root && s->pc.profile_hash && s->pc.profile_hash[0] && !nvn_plat_flag("no_offline", 0)) {
        struct s2a_offline_request oq = {0};
        memcpy(oq.identity, s->identity, 32);
        oq.direct_abi = (uint32_t)s->pc.direct_abi;
        oq.profile_hash = s->pc.profile_hash;
        oq.profile_declared = s->pc.offline_profile_available;
        struct s2a_offline_program op = {0};
        offline_rc = s2a_offline_library_fetch(root, &oq, &op, why, sizeof why);
        if (offline_rc == S2A_OFFLINE_HIT) {
            bytes = op.bytes; len = op.len;
            atomic_fetch_add_explicit(&g_n_offline, 1, memory_order_relaxed);
        }
    }
    if (!bytes && cdir && !nvn_plat_flag("no_disk_cache", 0)) {
        char cwhy[160] = "";
        if (s2a_program_cache_load(cdir, s->identity, &bytes, &len, rsha, cwhy, sizeof cwhy) == 0) {
            atomic_fetch_add_explicit(&g_n_cache, 1, memory_order_relaxed);
            have_res = 1;
        } else cache_state = cache_why_class(cwhy);
    } else if (!bytes) cache_state = "没查（关了或无目录）";
    union lp_prepared *ready = NULL;
    struct stair_probe probe = stair_begin(!bytes, "fg");
    if (!bytes && libpack_fetch(s->identity, (const uint8_t (*)[8])s->keys, s->stages, NULL, s->lp_state, s->f.gfx.topology, &s->pc, &bytes, &len, &ready, &probe)) {
        if (probe.start) probe.fetched = stair_us();
        struct entry *le = build(bytes, len, s->views, s->nst, s->identity, 1, ready);
        if (le) { le->prog.z_raw = 1; le->prog.lib_sourced = 1; }
        free(bytes); bytes = NULL; len = 0;
        stair_end(&probe, "fg", s->identity, s->stages, le && !le->failed);
        if (le && !le->failed) { libpack_note_hit(); if (s->lp_state == LIBPACK_SSBO_WINDOW) libpack_window_note(s->identity, (const uint8_t (*)[8])s->keys, s->nst, s->stages, "提交"); return le; }
        if (le) entry_free(le);
        libpack_mark_bad(s->identity);
        build_failed = 1;
    } else stair_end(&probe, "fg", s->identity, s->stages, 0);
    if (!bytes) {
        live_reason_note(s, root, s->lp_state != LIBPACK_OTHER_STATE, offline_rc, cache_state, build_failed);
        // 现场编译只读取已捕获的请求，回到原 owner 处理事实学习和结果发布。
        static struct s2a_compile_request_input in;   // 提交线程独占
        memset(&in, 0, sizeof in);
        in.session = s->pc.session; in.generation = s->pc.generation;
        in.requires_linked_varying_interface = s2a_direct_requires_linked_varying_interface(s->stages);
        in.link_map_diag = in.requires_linked_varying_interface && link_map_diag_want();
        in.profile_json = s->pc.profile_json; in.profile_bytes = s->pc.profile_bytes;
        in.direct_abi = (int)s->pc.direct_abi; in.translator_closure = s->pc.translator_closure;
        in.texture_array_length = s->tex_pool_n; in.sampler_array_length = s->samp_pool_n;
        in.transform_feedback_enabled = s->tf_on;
        in.gfx = s->f.gfx;
        memcpy(in.textures, s->f.tex, sizeof in.textures);
        in.texture_count = s->f.ntex;
        in.handle_table_entries = HANDLE_TABLE_ENTRIES;
        char ident[49]; int o = 0;
        for (int st = 0; st < s->nst; st++) {
            struct s2a_compile_stage_src *d = &in.stages[in.stage_count++];
            const uint8_t *code = s->views[st].code; uint32_t nb = s->views[st].code_bytes;
            if (nb > 0x30 && code[0] == 0x78 && code[1] == 0x56 && code[2] == 0x34 && code[3] == 0x12) { code += 0x30; nb -= 0x30; }   // 带 0x30 字节头的代码段
            d->stage = (const char *const[]){"vs", "fs", "gs"}[st];
            d->code = code; d->code_bytes = nb;
            d->control = s->views[st].control; d->control_bytes = s->views[st].control_bytes;
            d->c1 = s->views[st].c1; d->c1_bytes = s->views[st].c1_bytes;
            d->constant_buffer_use = s->f.ubo_use[st];
            if (!in.gfx.has_tess_mode && s->views[st].control_bytes >= 0x748) { memcpy(&in.gfx.tess_mode, s->views[st].control + 0x744, 4); in.gfx.has_tess_mode = 1; }
            for (int b = 0; b < 8; b++) o += snprintf(ident + o, sizeof ident - (size_t)o, "%02x", s->keys[st][b]);
        }
        in.request_id = ident;
        char *json = NULL; size_t jl = 0; char gaps[1024] = "";
        if (s2a_compile_request_build(&in, &json, &jl, gaps, sizeof gaps)) { red1("request_build_failed", gaps); return NULL; }
        struct s2a_compile_outcome oc; memset(&oc, 0, sizeof oc);
        enum live_status status = live_compile(in.session, in.generation, json, jl, s->identity, s->stages, q, &oc);
        free(json);
        if (status == LIVE_PENDING || status == LIVE_ABORT) {
            *pending = status == LIVE_PENDING;
            s2a_compile_outcome_free(&oc);
            return NULL;
        }
        if (status == LIVE_REJECTED) {
            atomic_fetch_add_explicit(&g_n_failed, 1, memory_order_relaxed);
            // 编译器报缺的是用户常量缓冲里的句柄事实、且学到了新坐标：resolve_slow 会带上它当场重编，这次拒绝是学习的一步，
            // 不记成「未接 compile_failed」（不然每份着色器第一次见这类句柄都多几条假红）。学不到 / 重编到上限仍失败的，下一次照常报红。
            const int learned = ubo_need_learn(oc.summary, (const uint8_t (*)[8])s->keys, s->nst, 0x7u);
            if (learned) g_ubo_learned = 1;
            else red1("compile_failed", oc.summary);
            compiler_rejected(oc.summary, learned ? NULL : s);   // 学坐标的那一步马上重编，不打上下文；其余被拒打程序键与图像槽原因（只加日志）
            s2a_compile_outcome_free(&oc);
            struct entry *fe = (struct entry *)calloc(1, sizeof *fe);
            if (fe) { fe->failed = 1; memcpy(fe->prog.identity, s->identity, 32); }
            return fe;
        }
        bytes = oc.bytes; len = oc.bytes_len; oc.bytes = NULL;
        s2a_compile_outcome_free(&oc);
        if (in.link_map_diag) link_map_note(bytes, len, ident, s->identity);
        atomic_fetch_add_explicit(&g_n_compiled, 1, memory_order_relaxed);
        if (g_texpool_diag) { g_pool_compiled++; if (g_pool_attempts == 1 || !(g_pool_attempts % 50)) pool_stats("progress"); }
        if (cdir && !nvn_plat_flag("no_disk_cache", 0)) s2a_program_cache_store_async(cdir, s->identity, bytes, len);
        if (alias_on()) { sha256(bytes, len, rsha); have_res = 1; }   // 与缓存文件头里写的是同一个量（整份负载的 SHA-256）
    }
    if (!bytes) return NULL;
    struct entry *e = build(bytes, len, s->views, s->nst, s->identity, 0, NULL);
    if (e && !e->failed && have_res && alias_on()) entry_set_res(e, rsha, (uint64_t)len, s->nst);
    // 事实复用（#1187 nvk_shader_fact_cache.h）：结果字节在这里释放，掩码先算好挂在 entry 上（纯函数，后台线程也能算）
    if (e && !e->failed) e->fact_ok = (uint8_t)fact_cache_mask(bytes, len, &e->prog, e->fact_mask);
    free(bytes);
    return e;
}
// [后台] 预热带专用：只走离线库 / 跨局缓存（按内容身份读结果字节），不现场编译。
// 预热失败诊断：只打前几条 + 之后每 500 条一条（真机上不刷屏，但足以定位「为什么一条都建不出来」）。
// 失败原因写清是「上下文不可用 / 离线库 miss + 缓存 miss（带 why）/ parse 失败（red1 已有）」。
static void warm_diag(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void warm_diag(const char *fmt, ...) {
    static _Atomic unsigned n;
    unsigned k = atomic_fetch_add(&n, 1);
    if (k >= 8 && (k % 500) != 0) return;
    char b[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "warm", b);
}
static struct entry *build_compute(const uint8_t *, size_t, const struct s2a_source_entry_view *, const uint8_t[32], const uint8_t[8], int lib, union lp_prepared *ready);

static struct entry *warm_work_run(const struct nvk_prog_rec *rec, int nst) {
    uint8_t *bytes = NULL; size_t len = 0; char why[256] = "";
    struct s2a_direct_compile_context pc;
    memset(&pc, 0, sizeof pc);
    if (s2a_direct_provider_compile_context(&pc)) {
        warm_diag("程序预热：编译上下文不可用（provider 还没发布本局会话），本局预热整体不可用");
        return NULL;
    }
    const char *root = getenv("S2A_SHADER_REVISION_ROOT"), *cdir = getenv("S2A_DIRECT_CACHE_DIR");
    if (root && pc.profile_hash && pc.profile_hash[0] && !nvn_plat_flag("no_offline", 0)) {
        struct s2a_offline_request oq = {0};
        memcpy(oq.identity, rec->identity, 32);
        oq.direct_abi = pc.direct_abi;
        oq.profile_hash = pc.profile_hash;
        oq.profile_declared = pc.offline_profile_available;
        struct s2a_offline_program op = {0};
        if (s2a_offline_library_fetch(root, &oq, &op, why, sizeof why) == S2A_OFFLINE_HIT) {
            bytes = op.bytes; len = op.len;
            atomic_fetch_add_explicit(&g_n_offline, 1, memory_order_relaxed);
        }
    }
    const int had_bytes = bytes != NULL;
    uint8_t rsha[32]; int have_res = 0;   // 同 work_run：跨局缓存读到的已校验结果摘要，挂在 entry 上给结果合并用
    if (!bytes && cdir && !nvn_plat_flag("no_disk_cache", 0) &&
        s2a_program_cache_load(cdir, rec->identity, &bytes, &len, rsha, why, sizeof why) == 0) {
        atomic_fetch_add_explicit(&g_n_cache, 1, memory_order_relaxed);
        have_res = 1;
    }
    // 出包预编库：清单记着每阶段键，预热带直接按键取（新装的游戏第一局没有跨局缓存，靠的就是这一份）
    const int compute = rec->stage_mask == 32u;
    struct s2a_source_entry_view views[3];
    memset(views, 0, sizeof views);
    for (int st = 0; st < nst && st < 3; st++) {
        views[st].key = rec->keys[st];   // 拒收 / 合规核对行要靠它打程序键
        if (rec->c1_ok & (1u << st)) { views[st].c1 = rec->c1[st]; views[st].c1_bytes = rec->c1_len[st]; }
    }
    // 预热清单不记绘制图元：几何程序这里取不到（topology 传 NULL → miss），等提交线程按本笔图元取
    int lp_state = rec->lp_ok == 2 ? (rec->lp_ssbo_window ? LIBPACK_SSBO_WINDOW : LIBPACK_ALIGNED) : LIBPACK_OTHER_STATE;
    union lp_prepared *ready = NULL;
    struct stair_probe probe = stair_begin(!bytes, "warm");
    if (!bytes && libpack_fetch(rec->identity, compute ? NULL : (const uint8_t (*)[8])rec->keys,
                                compute ? 0 : rec->stage_mask, compute ? rec->keys[0] : NULL, lp_state, NULL, &pc, &bytes, &len, &ready, &probe)) {
        if (probe.start) probe.fetched = stair_us();
        struct entry *le = compute ? build_compute(bytes, len, &views[0], rec->identity, rec->keys[0], 1, ready) : build(bytes, len, views, nst, rec->identity, 1, ready);
        if (le) { if (!compute) le->prog.z_raw = 1; le->prog.lib_sourced = 1; }
        free(bytes); bytes = NULL; len = 0;
        stair_end(&probe, "warm", rec->identity, rec->stage_mask, le && !le->failed);
        if (le && !le->failed) { libpack_note_hit(); if (!compute && lp_state == LIBPACK_SSBO_WINDOW) libpack_window_note(rec->identity, (const uint8_t (*)[8])rec->keys, nst, rec->stage_mask, "预热"); return le; }
        if (le) entry_free(le);
        libpack_mark_bad(rec->identity);
    } else stair_end(&probe, "warm", rec->identity, rec->stage_mask, 0);
    if (!bytes) {
        warm_diag("程序预热：结果字节取不到（身份 %02x%02x%02x%02x…，离线库 %s，跨局缓存 %s「%s」，缓存目录 %s）",
                  rec->identity[0], rec->identity[1], rec->identity[2], rec->identity[3],
                  had_bytes ? "命中" : "未命中",
                  cdir ? "查了" : "没有目录",
                  why[0] ? why : "无",
                  cdir ? cdir : "(null)");
        return NULL;
    }
    // c1 随清单落盘（开局 source_store 是空的）：views 在上面按清单填好、传给 build()，逐位与真实解析一致。
    struct entry *e = compute ? build_compute(bytes, len, &views[0], rec->identity, rec->keys[0], 0, NULL) : build(bytes, len, views, nst, rec->identity, 0, NULL);
    if (!compute && e && !e->failed && have_res && alias_on()) entry_set_res(e, rsha, (uint64_t)len, nst);
    // 结果合并命中时前台要把事实复用（#1187）登记到这份程序上，登记要用「编译器实际读了哪些绑定事实」的掩码：
    // 前台自己建的（work_run）本来就算好了，预热带建的也要算（纯函数，后台线程能算）。关着开关时保持原样不算。
    if (!compute && e && !e->failed && alias_on()) e->fact_ok = (uint8_t)fact_cache_mask(bytes, len, &e->prog, e->fact_mask);
    free(bytes);
    return e;
}
static int stage_nst(uint32_t mask) { return mask == 32u ? 1 : mask == 7u ? 3 : mask == 3u ? 2 : mask == 1u ? 1 : 0; }

// 快路径：输入跟上一次取程序时一样，就还是那个程序。下面的摘要盖住 fill_facts 与身份用到的全部回放状态
// （游戏每笔绘制前都会重绑贴图 / 缓冲，绑的东西对编译来说通常没变；不走快路径时每笔要重填事实、编码身份、算一次 SHA-256）。
// 调试开关 nvk_resolve_check=1：命中后仍走一遍慢路径核对，不一致报红。
// 表是「4096 个桶 × 4 路」而不是只记上一次：相邻两笔常在两三种材质之间来回换，只记一项时每次都落空；同桶 4 路让
// 几种材质共存，桶内轮转淘汰（桶内热点不超过 4 个时就与 LRU 等价）。取不到程序的那次结果不占位：它本来也不会命中，
// 只会把有用的项挤掉（同一份输入再进来时由 g_progs 里的失败项挡掉重复编译）。只在提交线程上用。
// 为什么是 4096 桶（#1146）：一帧里不同的摘要有几百个，散进 512 个桶总有几个桶落进 ≥ 5 个键；4 路装不下 5 个键，
// 而一帧里又周期性地依次访问它们，轮转替换遇上周期访问就是这几个键每次都落空（真机世界场景稳态每窗仍有十来次，每次走一遍慢路径）。
// 桶数加到 4096，同一批键散得开；每笔仍只摸一个桶（4 路 × 32 字节 = 2 条缓存行）。
// 表在会话里第一次存项时按需分配（约 516 KB），rp_shader_reset 释放：不跑游戏的进程不背这一块。分配不到就不缓存，
// 每笔走慢路径，答案不变。
enum { LAST_BITS = 12, LAST_BUCKETS = 1 << LAST_BITS, LAST_WAYS = 4 };
struct last_ent { uint64_t key, session, generation; const struct rp_prog *prog; };
static struct last_tab { struct last_ent e[LAST_BUCKETS][LAST_WAYS]; uint8_t next[LAST_BUCKETS]; } *g_last_tab;
static struct last_ent *last_find(uint64_t key, uint64_t session, uint64_t generation) {
    if (!g_last_tab) return NULL;
    struct last_ent *b = g_last_tab->e[(key ^ (key >> 32)) & (LAST_BUCKETS - 1)];
    for (int i = 0; i < LAST_WAYS; i++)
        if (b[i].prog && b[i].key == key && b[i].session == session && b[i].generation == generation) return &b[i];
    return NULL;
}
static void last_put(uint64_t key, uint64_t session, uint64_t generation, const struct rp_prog *prog) {
    if (!g_last_tab && !(g_last_tab = calloc(1, sizeof *g_last_tab))) return;
    struct last_tab *t = g_last_tab;
    const unsigned b = (unsigned)((key ^ (key >> 32)) & (LAST_BUCKETS - 1)), w = t->next[b];
    t->next[b] = (uint8_t)((w + 1) & (LAST_WAYS - 1));
    t->e[b][w] = (struct last_ent){key, session, generation, prog};
}
// 快路读数（平台件读不到回放核心的 rp_perf）：开关 nvk_shader_cache_log=1 时每 4096 次取程序打一行，真机 A/B 看命中率
// 与两条路的平均耗时。计时只在开关打开时做（此时那点 clock_gettime 相对 3.5µs 可忽略）。
static uint64_t g_cache_calls, g_cache_hits, g_cache_misses, g_cache_hit_ns, g_cache_miss_ns, g_sig_recomp;
static uint64_t g_mark_calls, g_mark_hits, g_mark_misses, g_mark_hit_ns, g_mark_miss_ns, g_mark_sig;   // 上一窗读数：打的是每 4096 次这一窗，不累加
static int g_cache_log = -1;
static uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static void cache_log(void) {
    const uint64_t calls = g_cache_calls - g_mark_calls, hits = g_cache_hits - g_mark_hits, misses = g_cache_misses - g_mark_misses;
    const uint64_t hit_ns = g_cache_hit_ns - g_mark_hit_ns, miss_ns = g_cache_miss_ns - g_mark_miss_ns;
    char line[208];
    // sig = 这一窗重算组合槽事实摘要的次数（每个它 = 32 次随机访存 + 一次取锁）：它不随笔数涨就说明摘要缓存命中了。
    snprintf(line, sizeof line, "shader_cache window calls=%llu hits=%llu misses=%llu hit_permille=%llu sig=%llu hit_ns=%llu miss_ns=%llu",
        (unsigned long long)calls, (unsigned long long)hits, (unsigned long long)misses,
        (unsigned long long)(calls ? hits * 1000 / calls : 0), (unsigned long long)(g_sig_recomp - g_mark_sig),
        (unsigned long long)(hits ? hit_ns / hits : 0), (unsigned long long)(misses ? miss_ns / misses : 0));
    nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", line);
    g_mark_calls = g_cache_calls; g_mark_hits = g_cache_hits; g_mark_misses = g_cache_misses;
    g_mark_hit_ns = g_cache_hit_ns; g_mark_miss_ns = g_cache_miss_ns; g_mark_sig = g_sig_recomp;
}
// 计算程序快路的读数（同一个开关 nvk_shader_cache_log=1；单独计数，因为图形一帧上千次、计算一帧二十来次）：
// 每 256 次计算取程序（约十来帧）打一行：命中 / 未命中次数与两条路各自的平均耗时。
static uint64_t g_cs_hits, g_cs_misses, g_cs_hit_ns, g_cs_miss_ns;
static uint64_t g_mark_cs_hits, g_mark_cs_misses, g_mark_cs_hit_ns, g_mark_cs_miss_ns;
static int cache_log_on(void) { if (g_cache_log < 0) g_cache_log = nvn_plat_flag("nvk_shader_cache_log", 0); return g_cache_log; }
static void cs_cache_log(void) {
    const uint64_t hits = g_cs_hits - g_mark_cs_hits, misses = g_cs_misses - g_mark_cs_misses;
    const uint64_t hit_ns = g_cs_hit_ns - g_mark_cs_hit_ns, miss_ns = g_cs_miss_ns - g_mark_cs_miss_ns;
    char line[160];
    snprintf(line, sizeof line, "shader_cache_cs window calls=%llu hits=%llu misses=%llu hit_ns=%llu miss_ns=%llu",
        (unsigned long long)(hits + misses), (unsigned long long)hits, (unsigned long long)misses,
        (unsigned long long)(hits ? hit_ns / hits : 0), (unsigned long long)(misses ? miss_ns / misses : 0));
    nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", line);
    g_mark_cs_hits = g_cs_hits; g_mark_cs_misses = g_cs_misses; g_mark_cs_hit_ns = g_cs_hit_ns; g_mark_cs_miss_ns = g_cs_miss_ns;
}
uint64_t nvk_shader_cs_fast_hits(void) { return g_cs_hits; }   // 读数出口：计算快路累计命中次数（单测用；会话清空时归零）
// 快表的单测出口：键由测试自己造（用来钉「同一旧桶里的 5 个键能不能共存」），session / generation 固定为 1；allocated 说明表现在有没有分配
// （会话外 / 收尾后应当没有）。
int nvk_shader_last_allocated(void) { return g_last_tab != NULL; }
void nvk_shader_last_test_put(uint64_t key, const struct rp_prog *prog) { last_put(key, 1, 1, prog); }
const struct rp_prog *nvk_shader_last_test_find(uint64_t key) { const struct last_ent *e = last_find(key, 1, 1); return e ? e->prog : NULL; }
// 阶段键一律经核心导出接口取（rp_shader_stage_key）：没绑定 / 键缺失给全 0，不读可能过期的核心内部分量。
static const uint8_t *stage_key(unsigned st) {
    static uint8_t k[3][8], kc[8];
    if (st == 5) {   // 计算阶段不在 rp_shader_stage_key 的图形范围（0..4）里：直接读核心状态里的键（同 shader_resolve_compute 的 skey_ok 判据），没绑定给全 0
        if (rp_S.skey_ok >> 5 & 1) memcpy(kc, rp_S.skey[5], 8); else memset(kc, 0, 8);
        return kc;
    }
    unsigned i = st < 3 ? st : 0u;
    if (!rp_shader_stage_key(st, k[i])) memset(k[i], 0, 8);
    return k[i];
}

// 分离贴图槽（每阶段 128 格）的编译事实摘要，带缓存。这条路每笔绘制都走：每次都把两个阶段各 128 格扫一遍的话，
// 一款每帧 1480 笔的游戏「取程序」从 2.6 涨到 3.7 毫秒 / 帧（真机直接计时）。摘要只取决于这 128 格的句柄和它们在
// 贴图登记表里的内容，所以槽的代号（rp_tex_sep_gen）、登记表的代号（rp_tex_reg_gen）、池的起点和大小都没变时用上次的。
// 代号先读、再算：算的中途登记表变了，下次代号对不上会重算，不会把旧摘要配到新代号上。只在提交线程上用。
static uint64_t sep_fact_sig(int st) {
    static struct { uint64_t sg, rg, sig; int base, pn, ok; } c[6];   // 0..2 图形阶段，5 计算阶段
    const uint64_t sg = rp_tex_sep_gen[st], rg = rp_tex_reg_gen();
    if (c[st].ok && c[st].sg == sg && c[st].rg == rg && c[st].base == (int)rp_S.tex_base && c[st].pn == (int)rp_S.tex_pool_n) return c[st].sig;
    c[st].sig = rp_tex_fact_sig(&rp_S.tex[st][128], 128, rp_S.tex_base, rp_S.tex_pool_n);
    c[st].sg = sg; c[st].rg = rg; c[st].base = (int)rp_S.tex_base; c[st].pn = (int)rp_S.tex_pool_n; c[st].ok = 1;
    return c[st].sig;
}

// 组合贴图槽（每阶段 0..31 格）的编译事实摘要，带缓存（做法同上一条）：input_sig 每笔绘制都要这一份，
// 不缓存的话一笔要逐槽查一遍登记表 —— 两个阶段共 64 次随机访存 + 两次取锁，一帧 1114 笔就是 7 万次。
// 摘要只取决于这 32 格的句柄、它们在登记表里的内容（登记 / 注销 / 清空会推进 rp_tex_reg_gen）、池的起点和大小：
// 句柄逐个记下，代号与起点容量都对得上就沿用上次算的。代号先读、再比句柄：算的中途登记表变了，下次代号对不上
// 会重算，不会把旧摘要配到新代号上。只在提交线程上用。
struct comb_sig { uint64_t h[32], rg, sig; int base, pn; uint8_t ok; };
static struct comb_sig g_comb_sig[6];   // 0..2 图形阶段，5 计算阶段
static uint64_t combined_fact_sig(int st) {
    struct comb_sig *c = &g_comb_sig[st];
    const uint64_t rg = rp_tex_reg_gen();
    if (c->ok && c->rg == rg && c->base == (int)rp_S.tex_base && c->pn == (int)rp_S.tex_pool_n &&
        !memcmp(c->h, rp_S.tex[st], sizeof c->h)) return c->sig;
    g_sig_recomp++;
    c->sig = rp_tex_fact_sig(rp_S.tex[st], 32, rp_S.tex_base, rp_S.tex_pool_n);
    memcpy(c->h, rp_S.tex[st], sizeof c->h);
    c->rg = rg; c->base = (int)rp_S.tex_base; c->pn = (int)rp_S.tex_pool_n; c->ok = 1;
    return c->sig;
}
// 缓存有效高槽的事实摘要；被 image 遮住的 tex 变化不改变编译输入。
static struct { uint64_t handles[8], rg, sig; int base, pn; uint8_t ok; } g_high_sig[3];
static uint64_t graphics_high_fact_sig(int st) {
    uint64_t handles[8]; graphics_high_handles(st, handles);
    uint64_t rg = rp_tex_reg_gen();
    if (!g_high_sig[st].ok || g_high_sig[st].rg != rg || g_high_sig[st].base != (int)rp_S.tex_base ||
        g_high_sig[st].pn != (int)rp_S.tex_pool_n || memcmp(g_high_sig[st].handles, handles, sizeof handles)) {
        g_sig_recomp++;
        g_high_sig[st].sig = rp_tex_fact_sig(handles, 8, rp_S.tex_base, rp_S.tex_pool_n);
        memcpy(g_high_sig[st].handles, handles, sizeof handles);
        g_high_sig[st].rg = rg; g_high_sig[st].base = (int)rp_S.tex_base;
        g_high_sig[st].pn = (int)rp_S.tex_pool_n; g_high_sig[st].ok = 1;
    }
    return g_high_sig[st].sig;
}
// with_tex_facts=0：不混入贴图槽事实——只给「身份粒度」诊断当分组键用（同一份着色器 = 同一组）。
static uint64_t input_sig(const struct rp_shader_req *q, const struct s2a_direct_compile_context *pc, uint32_t stages, int with_tex_facts) {
    uint64_t h = 1469598103934665603ull;
    #define MIX(v) do { h ^= (uint64_t)(v); h *= 1099511628211ull; } while (0)
    // 来源登记冲突会毒化短键并推进代际；命中快表前也必须失效。
    MIX(s2a_source_epoch());
    MIX(stages); MIX(q->prim); MIX(q->rt_count); for (int i = 0; i < 8; i++) MIX(q->rt_format[i]);
    for (int st = 0; st < ((stages & 4u) ? 3 : 2); st++) { uint64_t k; memcpy(&k, stage_key((unsigned)st), 8); MIX(k); }
    MIX(rp_S.color.alpha_func); { uint32_t ar; float r = rp_S.color.alpha_func >= 1 && rp_S.color.alpha_func <= 7 ? rp_S.alpha_ref : 0.0f; memcpy(&ar, &r, 4); MIX(ar); }
    MIX(rp_S.ms.a2c ? 1 : 0); MIX(rp_S.ms.a2c_dither ? 1 : 0);
    { int dual = 0; for (int i = 0; i < 8; i++) if (src1(rp_S.blend[i].src_rgb) || src1(rp_S.blend[i].dst_rgb) || src1(rp_S.blend[i].src_a) || src1(rp_S.blend[i].dst_a)) dual = 1; MIX(dual); }
    MIX(rp_depth_zero()); MIX(rp_origin_lower_left()); MIX(pc->shader_draw_parameters);
    { int any = 0, un = 0; for (int st = 0; st < 5; st++) for (int i = 0; i < 16; i++) if (rp_S.ssbo[st][i].addr) { any = 1; if (pc->storage_buffer_offset_alignment && rp_S.ssbo[st][i].addr % pc->storage_buffer_offset_alignment) un = 1; } MIX(any); MIX(un); MIX(pc->storage_buffer_offset_alignment); }
    MIX(rp_S.nattr); for (int i = 0; i < 16; i++) MIX(i < rp_S.nattr ? rp_S.attr[i].fmt : 0);
    for (int st = 0; st < 3; st++) if (stages & (1u << st)) { uint32_t m = 0; for (int i = 0; i < 16; i++) if (rp_S.ubo[st][i].size) m |= 1u << i; MIX(m); }
    MIX(rp_S.tex_pool_n); MIX(rp_S.samp_pool_n); MIX(rp_S.tf_on); MIX(rp_S.tex_base);
    if (with_tex_facts) {
        MIX(combined_fact_sig(0)); MIX(combined_fact_sig(1));
        MIX(sep_fact_sig(0)); MIX(sep_fact_sig(1));
        // 几何阶段（VS|FS|GS）在重场景里极少出现，不进上面那张小表：直接算这 32 格的摘要（省下每次比 32 个句柄）。
        if (stages & 4u) MIX(rp_tex_fact_sig(rp_S.tex[2], 32, rp_S.tex_base, rp_S.tex_pool_n));
        if (stages & 4u) MIX(sep_fact_sig(2));
        for (int st = 0; st < 3; st++) if (stages & (1u << st)) MIX(graphics_high_fact_sig(st));
        if (g_ubo_need_n) for (int st = 0; st < 3; st++) if (st < 2 || (stages & 4u)) MIX(ubo_fact_sig(st));
    }
    #undef MIX
    return h;
}
static int resolve_slow(const struct rp_shader_req *q, const struct rp_prog **out, const struct s2a_direct_compile_context *pcp,
                        uint32_t stages, int nst, uint64_t group);

// ---------- 身份粒度诊断（开关 nvk_ident_diag，缺省 0；调试包里 debug.s2a.nvk_ident_diag=1 打开；只读计数，不改变任何语义） ----------
// 要回答的问题：同一份着色器（阶段键 + gfx 块 + 池级事实 + cbo 使用位，「同一组」）在本局被拆成了多少份
// 程序身份？拆出来的每一份都要单独读盘/解析/编译，所以「身份数 / 组数」就是重复量。删诊断时整块删掉。
// 缺省 0 的原因（#1328）：玩家包里 nvn_plat_flag 恒返回缺省值，缺省 1 就意味着玩家包每笔绘制多算一遍 input_sig（分组键）、
// 慢路径多记账、每 5 秒多打一行日志；「续 36」那轮测量已经出了结论（身份粒度不是主因），不该再留在玩家包的热路径上。
static int g_ident_diag = -1;
enum { IDG_SLOTS = 1024, IDG_SET = 16384 };
static struct { uint64_t key; uint32_t compiles, loads; } g_idg[IDG_SLOTS];
static uint64_t g_idg_set[IDG_SET];          // 身份集合（sha 折叠成 64 位；0 = 空）
static uint64_t g_idg_groups, g_idg_idents, g_idg_resolves, g_idg_compiles, g_idg_loads, g_idg_multi;
static uint64_t g_idg_last_ns;
static int idg_on(void) { if (g_ident_diag < 0) g_ident_diag = nvn_plat_flag("nvk_ident_diag", 0); return g_ident_diag; }
static void idg_work(uint64_t group, int compiled) {
    if (!group) return;
    if (compiled) g_idg_compiles++; else g_idg_loads++;
    unsigned s = (unsigned)((group ^ (group >> 32)) & (IDG_SLOTS - 1));
    for (unsigned k = 0; k < 8; k++) {
        unsigned idx = (s + k) & (IDG_SLOTS - 1);
        if (g_idg[idx].key == group) {
            if (compiled) { if (g_idg[idx].compiles == 1) g_idg_multi++; g_idg[idx].compiles++; } else g_idg[idx].loads++;
            return;
        }
        if (!g_idg[idx].key) { g_idg[idx].key = group; g_idg_groups++; if (compiled) g_idg[idx].compiles++; else g_idg[idx].loads++; return; }
    }
}
// 这份身份第一次被用到（不管它后来是预热命中、读盘还是现场编）：算进「身份数 / 组数」。
static void idg_note_ident(uint64_t group, const uint8_t sha[32]) {
    if (!group) return;
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 32; i++) { h ^= sha[i]; h *= 1099511628211ull; }
    h |= 1ull;
    g_idg_resolves++;
    unsigned s = (unsigned)((h ^ (h >> 32)) & (IDG_SET - 1));
    for (unsigned k = 0; k < 8; k++) {
        unsigned idx = (s + k) & (IDG_SET - 1);
        if (g_idg_set[idx] == h) break;
        if (!g_idg_set[idx]) { g_idg_set[idx] = h; g_idg_idents++; break; }
    }
    // 分组也要先登记（第一份身份出现时组可能是新的）
    s = (unsigned)((group ^ (group >> 32)) & (IDG_SLOTS - 1));
    for (unsigned k = 0; k < 8; k++) {
        unsigned idx = (s + k) & (IDG_SLOTS - 1);
        if (g_idg[idx].key == group) return;
        if (!g_idg[idx].key) { g_idg[idx].key = group; g_idg_groups++; return; }
    }
}
static void idg_log(void) {
    if (!idg_on()) return;
    const uint64_t now = now_ns();
    if (!g_idg_last_ns) { g_idg_last_ns = now; return; }
    if (now - g_idg_last_ns < 5000000000ull) return;
    g_idg_last_ns = now;
    char b[256];
    snprintf(b, sizeof b, "身份粒度：组 %llu、身份 %llu（身份/组 %llu‰）、慢路 %llu（读盘 %llu / 现场编 %llu）、编译≥2 的组 %llu",
             (unsigned long long)g_idg_groups, (unsigned long long)g_idg_idents,
             (unsigned long long)(g_idg_groups ? g_idg_idents * 1000 / g_idg_groups : 0),
             (unsigned long long)g_idg_resolves, (unsigned long long)g_idg_loads,
             (unsigned long long)g_idg_compiles, (unsigned long long)g_idg_multi);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", b);
}
// 用例钩子（#1328，宿主单测 nvk_shader_prog_lease_test.c 用）：返回身份粒度诊断此刻开没开；force_log 非 0 时先把
// 「上次打日志的时刻」拨到很早，再调一次 idg_log——诊断开着就会立刻打一行「身份粒度」，缺省（关）一行都不打。
// 平台替身的 nvn_plat_flag 取缺省值，所以读到的就是玩家包的状态。
int nvk_shader_ident_diag_test(int force_log);
int nvk_shader_ident_diag_test(int force_log) {
    if (force_log) { g_idg_last_ns = 1; idg_log(); }
    return idg_on();
}

static int shader_resolve_graphics(const struct rp_shader_req *q, const struct rp_prog **out) {
    *out = NULL;
    if (g_cache_log < 0) g_cache_log = nvn_plat_flag("nvk_shader_cache_log", 0);   // 只读一次：平台开关在进程内不变
    const uint64_t t0 = g_cache_log ? now_ns() : 0;
    uint32_t stages = rp_shader_graphics_stages();
    // 认两种：顶点 + 片元；只有顶点（只写深度的绘制：深度预通道、阴影图）。几何 / 细分阶段还没接
    if (stages != 7u && stages != 3u && stages != 1u) { char nm[32]; snprintf(nm, sizeof nm, "stages_%#x_not_vs_fs", stages); nvn_unimpl("shader", nm, NULL); return -1; }
    const int nst = stages == 7u ? 3 : stages == 3u ? 2 : 1;
    if (nst == 3) {
        nvn_gpu_caps caps = {0}; nvn_gpu_caps_get(&caps);
        if (!(caps.caps & NVN_GPU_CAP_GEOMETRY)) { nvn_unimpl("shader", "geometry_not_enabled", NULL); return -1; }
        if (q->prim != 0 && q->prim != 1 && q->prim != 4) { nvn_unimpl("shader", "geometry_topology_unimplemented", NULL); return -1; }
    }
    for (uint32_t st2 = 0; st2 < 5; st2++) if ((stages >> st2 & 1) && !rp_shader_stage_key(st2, NULL)) { nvn_unimpl("shader", "stage_key_missing", NULL); return -1; }
    if (!shader_texture_pool_ready()) return -1;
    struct s2a_direct_compile_context pc; memset(&pc, 0, sizeof pc);
    if (s2a_direct_provider_compile_context(&pc) || !pc.profile_json || !pc.profile_bytes) { nvn_unimpl("shader", "no_compile_context", NULL); return -1; }
    uint64_t key = input_sig(q, &pc, stages, 1);
    uint64_t group = idg_on() ? input_sig(q, &pc, stages, 0) : 0;   // 身份粒度诊断：同一份着色器的分组键
    static int check = -1; if (check < 0) check = nvn_plat_flag("nvk_resolve_check", 0);
    g_cache_calls++;
    struct last_ent *le = last_find(key, pc.session, pc.generation);
    if (le && le->prog->lib_bad) le = NULL;   // 绑定核对拒收的出库版：快路不落命中，走慢路取现编
    if (le) {
        *out = le->prog;   // 快路径不记 draw_hits：同一份程序会被画几千笔，计数口径是「份数」不是「笔数」
        if (check) { const struct rp_prog *chk = NULL; int rc = resolve_slow(q, &chk, &pc, stages, nst, 0); if (rc || chk != le->prog) red1("resolve_fast_path_mismatch", "快路径给的程序与慢路径不一致"); }
        g_cache_hits++;
        if (g_cache_log) { g_cache_hit_ns += now_ns() - t0; if (!(g_cache_calls & 4095)) cache_log(); }
        return 0;
    }
    int rc = resolve_slow(q, out, &pc, stages, nst, group);
    if (!rc) last_put(key, pc.session, pc.generation, *out);
    g_cache_misses++;
    if (g_cache_log) { g_cache_miss_ns += now_ns() - t0; if (!(g_cache_calls & 4095)) cache_log(); }
    return rc;
}

// [提交线程] 前台的结果合并入口：取各阶段的源快照（c1 来自它），再走 alias_try。源快照取不到 / 不完整 = 不合并（后面照旧由现编 / 读盘路径同样处理这件事）。
static struct entry *alias_try_fg(const uint8_t sha[32], const struct s2a_input_identity *id, int nst, int *adopted) {
    struct s2a_source_entry_view v[RP_PROG_STAGES]; struct alias_key k; int got = 0;
    memset(&k, 0, sizeof k); k.nst = nst;
    for (; got < nst; got++) {
        if (!s2a_source_acquire(id->keys[got], &v[got])) break;
        if (!v[got].complete) { s2a_source_release(v[got].ref); break; }
        if (v[got].c1 && v[got].c1_bytes >= NVK_PROG_REC_C1_MIN) { k.c1_ok |= (uint8_t)(1u << got); k.c1[got] = v[got].c1; k.c1_len[got] = v[got].c1_bytes; }   // 与 build() 的 c1 判据同源（整段）
    }
    struct entry *e = got == nst ? alias_try(sha, 0, &k, adopted) : NULL;
    for (int st = 0; st < got; st++) s2a_source_release(v[st].ref);
    return e;
}

// [提交线程] 真实用过的身份记进预热清单：uses++，并**把程序对象里真实的 c1 一起记进清单**。
// 这里原先只记身份（没有源快照，就没有 c1）。后果是：只要某份程序第一次是被「已发布对象命中」记进清单的，
// 清单里这条就永远 c1_ok=0；下一局预热按它建出来的程序也 c1_ok=0，而 rp_draw 对带 C1 绑定的绘制
// （火把/粒子等用 BRX 跳表的着色器）直接跳过 → 光消失、画面发黑。程序对象里就有 c1，直接抄过来即可。
// 「已发布命中」与「结果合并命中」（#1355）共用这一份。
static void prog_note_use(const uint8_t sha[32], uint32_t stages, int nst, const struct s2a_input_identity *id, const struct facts *f, const struct rp_prog *prog) {
    struct nvk_prog_rec rec; memset(&rec, 0, sizeof rec); memcpy(rec.identity, sha, 32); rec.stage_mask = stages;
    libpack_note_state(&rec, &f->gfx, prog);
    for (int st = 0; st < nst; st++) memcpy(rec.keys[st], id->keys[st], 8);
    for (int st = 0; st < nst; st++)
        if (prog->c1_ok & (1u << st)) (void)rec_c1_put(&rec, st, prog->c1[st], prog->c1_len[st]);
    // 程序对象本身也没有 c1（例如它是上一局按残缺清单预热出来的）：这一笔手上正好有源快照，补采一次，
    // 让清单在下一次落盘时被修好（这一局仍会跳过该笔，下一局不再跳）。
    // 源快照钉到记完清单才放：记录里只带指针，清单在 note 里拷自己的副本。
    struct s2a_source_entry_view v[RP_PROG_STAGES]; int held[RP_PROG_STAGES] = {0};
    if ((rec.c1_ok & (uint8_t)((1u << nst) - 1u)) != (uint8_t)((1u << nst) - 1u)) {
        for (int st = 0; st < nst && st < RP_PROG_STAGES; st++) {
            if (rec.c1_ok & (1u << st)) continue;
            if (s2a_source_acquire(id->keys[st], &v[st])) {
                held[st] = 1;
                if (v[st].complete) (void)rec_c1_put(&rec, st, v[st].c1, v[st].c1_bytes);
            }
        }
    }
    nvk_prog_warm_note(&rec);
    for (int st = 0; st < RP_PROG_STAGES; st++) if (held[st]) s2a_source_release(v[st].ref);
}

// [提交线程] 三段式的第二、三段：先查已发布与在途（不重复准备），否则捕获快照 → 工作 → 发布。
static int resolve_slow(const struct rp_shader_req *q, const struct rp_prog **out, const struct s2a_direct_compile_context *pcp,
                        uint32_t stages, int nst, uint64_t group) {
    struct s2a_direct_compile_context pc = *pcp;
    *out = NULL;
    if (!g_profile.ok || g_profile.session != pc.session || g_profile.generation != pc.generation) {
        sha256(pc.profile_json, pc.profile_bytes, g_profile.sha);
        g_profile.session = pc.session; g_profile.generation = pc.generation; g_profile.ok = 1;
    }
    static struct facts f;   // 提交线程独占；结构大，不放栈上
    fill_facts(q, &pc, &f, nst);
    struct s2a_input_identity id; memset(&id, 0, sizeof id);
    id.direct_abi = (int)pc.direct_abi; id.translator_closure = pc.translator_closure; memcpy(id.profile_sha, g_profile.sha, 32);
    id.stage_mask = stages;
    id.requires_linked_varying_interface = s2a_direct_requires_linked_varying_interface(stages);
    for (int st = 0; st < nst; st++) {
        memcpy(id.keys[st], stage_key((unsigned)st), 8);
        if (s2a_source_identity(stage_key((unsigned)st), id.source_sha[st], &id.source_code_bytes[st], &id.source_control_bytes[st])) { nvn_unimpl("shader", "source_snapshot_missing", NULL); return -1; }
        id.source_present |= (uint8_t)(1u << st);
    }
    memcpy(id.ubo_use, f.ubo_use, sizeof id.ubo_use);
    id.texture_array_length = (uint32_t)rp_S.tex_pool_n; id.sampler_array_length = (uint32_t)rp_S.samp_pool_n; id.transform_feedback_enabled = rp_S.tf_on;
    id.gfx = &f.gfx; id.textures = f.tex; id.texture_count = f.ntex;
    static uint8_t idbuf[S2A_INPUT_IDENTITY_MAX]; size_t idlen = 0; uint8_t sha[32];
    if (s2a_input_identity_encode(&id, idbuf, sizeof idbuf, &idlen)) { nvn_unimpl("shader", "identity_encode_failed", NULL); return -1; }
    sha256(idbuf, idlen, sha);
    idg_note_ident(group, sha);   // 身份粒度诊断：这份身份第一次被用到（group=0 时空转）

    // 1) 已发布：直接命中。命中路径只多一次锁和一次热点 ++（不填快照、不 pin 源）。
    int e_failed = 0, e_adopted = 0;
    pthread_mutex_lock(&g_progs_mu);
    struct entry *e = progs_get_adopted_locked(sha, &e_failed, &e_adopted);
    pthread_mutex_unlock(&g_progs_mu);
    if (e_adopted) nvk_prog_warm_stat_draw_hit();   // 预建对象第一次被真实 draw 采用（每份一次）
    if (e && e->prog.lib_bad) e = NULL;   // 绑定核对拒收的出库版：不落命中，下去取现编
    if (e) {
        if (e_failed) return -1;
        *out = &e->prog;
        prog_note_use(sha, stages, nst, &id, &f, &e->prog);
        return 0;
    }
    // 1b) 事实复用（#1187）：编译器只读了一部分绑定事实，事实相同的另一份身份可以直接用同一个程序。
    //   别名表与变体表只有提交线程读写（rp_shader_reset 清），不进 g_progs_mu；别名不持有程序，程序只归 g_progs。
    { const struct rp_prog *fa = nvn_hm_get(&g_fact_aliases, sha);
      if (fa && !fa->lib_bad) { *out = fa; return 0; } }
    uint8_t fact_base[32];
    const int reusable = fact_cache_shape(&id) && fact_cache_hash(&id, NULL, fact_base);
    if (reusable && (*out = fact_cache_find(&id, fact_base)) && !(*out)->lib_bad) { nvn_hm_put(&g_fact_aliases, sha, (void *)*out); return 0; }
    *out = NULL;   // lib_bad 命中不算：落到取库 / 现编链
    // 1c) 结果合并（#1355）：这个身份的缓存文件头里记的结果摘要，已经有常驻程序 → 登记成别名直接用（只读 88 字节头，不读负载、不建后端）。
    if (alias_on()) {
        int a_adopted = 0;
        struct entry *ae = alias_try_fg(sha, &id, nst, &a_adopted);
        if (a_adopted) nvk_prog_warm_stat_draw_hit();   // 合并到的正是预热带建的那份：第一次被真实 draw 采用
        if (ae && !ae->prog.lib_bad) {   // 合并到的是绑定核对拒收的出库版：不采用，下去取现编
            *out = &ae->prog;
            // 与下面「正常发布后」同一处登记：合并命中的身份也进事实复用表。否则只靠前台建过的程序登记事实，合并之后前台几乎不建了，
            // 「只差编译器没读的贴图事实」的新身份（盘上没有它的文件）就失去复用、退回现场编（真机一局 3 → 198 次）。
            if (reusable && ae->fact_ok) fact_cache_keep_mask(&id, fact_base, ae->fact_mask, &ae->prog);
            prog_note_use(sha, stages, nst, &id, &f, &ae->prog);
            prog_tally_log();
            return 0;
        }
    }
    // 2) 同身份在途：等它发布，不重复准备同一份程序。
    struct entry *win = NULL;
    int w = await_inflight(sha, &win);
    if (w < 0) return -1;
    if (w > 0 && !(win && win->prog.lib_bad)) {   // 发布的是绑定核对拒收的出库版：不用它，下去认领现编
        if (win->failed) return -1;
        *out = &win->prog;   // 预建对象的采用计数在 await_inflight 的锁内记（每份一次）
        // 同一处的 c1 记账（原因见 prog_note_use）：预建对象里有 c1 就记进清单。
        { struct nvk_prog_rec rec; memset(&rec, 0, sizeof rec); memcpy(rec.identity, sha, 32); rec.stage_mask = stages;
          libpack_note_state(&rec, &f.gfx, &win->prog);
          for (int st = 0; st < nst; st++) memcpy(rec.keys[st], id.keys[st], 8);
          for (int st = 0; st < nst; st++)
              if (win->prog.c1_ok & (1u << st)) (void)rec_c1_put(&rec, st, win->prog.c1[st], win->prog.c1_len[st]);
          nvk_prog_warm_note(&rec); }
        return 0;
    }
    // 3) 本线程接手：认领在途 → 捕获快照（读 rp_S 只在这里）→ 工作 → 发布。
    uint64_t epoch;
    pthread_mutex_lock(&g_progs_mu);
    int claim = inflight_claim_locked(sha, 0);
    epoch = g_progs_epoch;
    pthread_mutex_unlock(&g_progs_mu);
    if (claim == 0) {   // 竞态：刚好有人认领了同一身份，回头等它
        for (int attempt = 0; attempt < 3; attempt++) {
            w = await_inflight(sha, &win);
            if (w < 0) return -1;                    // 停局
            if (w > 0) {
                if (win && win->prog.lib_bad) { /* 对方发布的是被拒收的出库版：下去认领现编，不返回它 */ }
                else if (win && !win->failed) { *out = &win->prog; return 0; }
                else return -1;                    // 对方给的是失败项：与本线程自己失败同义
            }
            // 对方撤销了认领却没发布（快照失败 / 停局前中止）：重试认领，认领成功就自己同步做这一笔。
            pthread_mutex_lock(&g_progs_mu);
            claim = inflight_claim_locked(sha, 0);
            epoch = g_progs_epoch;
            pthread_mutex_unlock(&g_progs_mu);
            if (claim != 0) break;                   // 1 = 认领成功；-1 = 表满/停局（照旧不认领也做）
        }
        if (claim == 0) return -1;                   // 连续被抢：本笔取不到（与旧行为同义，不跳过语义）
    }
    if (claim < 0 && shader_stopped()) return -1;   // 停局：本笔照旧取不到（不编、不发布）
    int claimed = claim > 0;   // claim < 0 且没停局 = 在途表满：不认领照样做（只丢一次去重机会，不丢绘制）
    // 清单里有这个身份、本局却要自己同步准备：这就是「预热没被采用」的证据（不在清单里的新程序不算）。
    if (nvk_prog_warm_contains(sha)) nvk_prog_warm_stat_draw_miss();
    if (g_texpool_diag) g_pool_programs++;
    struct nvk_prog_rec rec;
    memset(&rec, 0, sizeof rec);
    memcpy(rec.identity, sha, 32);
    rec.stage_mask = stages;
    for (int st = 0; st < nst; st++) memcpy(rec.keys[st], id.keys[st], 8);
    struct entry *ne = NULL;
    const uint64_t dg_c0 = atomic_load(&g_n_compiled);
    struct work_snap *snap = snap_capture(&f, &id, sha, &pc, stages, nst);
    g_ubo_learned = 0;
    int pending = 0;
    if (!snap || !(ne = work_run(snap, q, &pending))) {
        snap_free(snap);
        if (claimed) inflight_abort(sha);
        return pending ? 1 : -1;
    }
    idg_work(group, atomic_load(&g_n_compiled) > dg_c0);   // 身份粒度诊断：这份身份是现场编出来的还是读盘/离线库拿到的
    idg_log();
    snap_free(snap);
    struct entry *have = NULL;
    int failed = 0;
    int pr = progs_publish(sha, ne, 0, epoch, &have, &failed);   // 认领在发布里结束（含被拒路径）
    if (pr < 0) { entry_free(ne); return -1; }   // 停局 / 换代：丢弃（发布前后都校验过）
    struct entry *use = pr > 0 ? ne : have;
    if (pr == 0) entry_free(ne);
    // 失败标志在发布锁内读好带出来：发布之后 entry 已归缓存，会话收尾（rp_shader_reset）可能回收它。
    if (failed) {
        // 编译器报缺的是用户常量缓冲里的句柄事实、且刚学到这个坐标：带上它当场重建请求（身份随事实变，不撞上面这份失败项）。
        // 每轮至少多学一个坐标、每份着色器至多 8 个，深度有界；同一笔绘制就能画出来，不必等下一帧。
        static int depth;
        if (!g_ubo_learned || depth >= UBO_NEED_COORDS) return -1;
        g_ubo_learned = 0; depth++;
        int rc = resolve_slow(q, out, pcp, stages, nst, group);
        depth--;
        return rc;
    }
    *out = &use->prog;
    if (reusable && use->fact_ok) fact_cache_keep_mask(&id, fact_base, use->fact_mask, &use->prog);
    // c1 整段从发布出去的程序对象抄（它就是按这次快照建的，逐字节一致；对象归缓存、活到会话收尾，快照已经放了）：
    //   预热产品必须与这次真实解析的程序逐位一致（rp_draw 对 c1 不齐直接跳过绘制）。
    for (int st = 0; st < nst; st++)
        if (use->prog.c1_ok & (1u << st)) (void)rec_c1_put(&rec, st, use->prog.c1[st], use->prog.c1_len[st]);
    libpack_note_state(&rec, &f.gfx, &use->prog);
    nvk_prog_warm_note(&rec);   // 真实 resolve 成功：记身份 + uses（含 c1 整段）
    prog_tally_log();
    return 0;
}

// ---------- 计算程序 ----------
// 输入的构造与身份用旧层留下的零件（nvn_vk_compute_request.c：只做输入，不碰设备），结果解析用 nvn_vk_program_parse_compute。
// 存储缓冲三种来源：NVN 存储槽（SBUFP）、别名某个用户 UBO 槽的本体（map 2 / UBOP）、
// 用户 UBO 里 8 字节裸指针（map 3 / UBOA，带字节偏移）。图像、间接派发：记账、取不到。
static struct entry *build_compute(const uint8_t *bytes, size_t len, const struct s2a_source_entry_view *src,
                                   const uint8_t identity[32], const uint8_t key[8], int lib, union lp_prepared *ready) {
    struct entry *e = calloc(1, sizeof *e);
    if (!e) {
        if (ready) { nvn_vk_program_free_compute(&ready->compute); free(ready); }
        return NULL;
    }
    e->prog.is_compute = 1;
    memcpy(e->prog.identity, identity, 32);
    static _Thread_local struct nvn_vk_compute_program cp; char why[192] = "";
    int parsed = 0;
    if (ready) { cp = ready->compute; free(ready); }
    else {
        memset(&cp, 0, sizeof cp);
        parsed = nvn_vk_program_parse_compute(bytes, len, &cp, why, sizeof why);
    }
    if (parsed) {
        char note[256]; with_program_key(note, sizeof note, why, key);
        if (parsed == NVK_PROGRAM_ARRAY_UNSUPPORTED) array_unsupported(note); else red1("compute_program_parse_failed", note);
        e->failed = 1; return e;
    }
    e->prog.support_bytes = cp.support_bytes; e->prog.spill_bytes = cp.spill_bytes;
    for (int i = 0; i < cp.binding_count && !e->failed; i++) {
        const struct fd_binding *b = &cp.bindings[i];
        if (e->prog.nent[0] == RP_PROG_ENT_MAX) { red1("too_many_bindings", "cs"); e->failed = 1; break; }
        nvn_gpu_layout_ent *le = &e->prog.ent[0][e->prog.nent[0]]; int ok = 0;
        if (b->kind == 1) {
            const struct nvn_vk_compute_storage *sg = &cp.storage[i];
            if (sg->index < 16) {
                memset(le, 0, sizeof *le); le->set = (uint8_t)b->set; le->binding = (uint16_t)b->binding; le->count = 1;
                le->cls = NVN_GPU_RES_SSBO; le->access = sg->writable ? NVN_GPU_ACC_RW : NVN_GPU_ACC_R;
                if (sg->mapping == NVK_STORAGE_NVN && !sg->offset) {
                    // NVN 存储槽：按槽取地址（核心按 NVN 阶段 5 的存储槽绑定解析）
                    le->src_kind = NVN_RB_SBUFP; le->src_reg = (uint16_t)sg->index; ok = 1;
                } else if (sg->mapping == NVK_STORAGE_UBO_PTR && !sg->offset) {
                    // map 2：这条存储绑定别名「用户 UBO 槽 r 的缓冲本体」，核心按 UBO 槽的地址与长度当 SSBO 绑
                    le->src_kind = NVN_RB_UBOP; le->src_reg = (uint16_t)sg->index; ok = 1;
                } else if (sg->mapping == NVK_STORAGE_UBO_ADDRESS) {
                    // map 3：UBO 槽 r 内偏移 off 处只放一个 8 字节 guest 裸指针；偏移的合法性在绑定侧按 UBO 实际长度核
                    le->src_kind = NVN_RB_UBOA; le->src_reg = (uint16_t)sg->index; le->src_off = sg->offset; ok = 1;
                }
            }
        } else ok = layout_ent(b, le);
        if (!ok) { char nm[56], wy[80]; const uint8_t *k = src && src->key ? src->key : identity;
            snprintf(nm, sizeof nm, "compute_binding_kind_%d_slot_%d_map_%d", b->kind, b->slot, b->kind == 1 ? (int)cp.storage[i].mapping : 0);
            with_program_key(wy, sizeof wy, "cs", k);
            red1(nm, wy); e->failed = 1; break; }
        // 贴图字在游戏用户常量缓冲里的类型 3 句柄（#1594 片 10）：同图形阶段，贴图字所在的常量槽 / 偏移和常量采样器来源进程序的 usrc 表（src_reg2 = 表下标），
        //   回放派发时从计算阶段绑的常量缓冲读贴图字（rp_draw.c rp_dispatch）
        if (le->src_kind == NVN_RB_TEXU && !usrc_put(&e->prog, b, le)) { red1("too_many_uniform_handles", "cs"); e->failed = 1; break; }
        if (le->cls == NVN_GPU_RES_TBUF && le->src_kind != NVN_RB_TBUF) { red1("texture_buffer_replay_unsupported", "cs"); e->failed = 1; break; }
        le->stages = 1u << NVN_GPU_SH_CS;
        if (b->kind == 2 || b->kind == 3) sampled_decl(cp.stage.spirv, cp.stage.spirv_bytes, (uint32_t)b->set, (uint32_t)b->binding,   // 3 = 存储图像：数值类与声明格式给回放选视图格式
            &e->prog.dtt[0][e->prog.nent[0]], &e->prog.dcls[0][e->prog.nent[0]], &e->prog.difmt[0][e->prog.nent[0]]);
        if (le->cls == NVN_GPU_RES_TBUF && e->prog.dtt[0][e->prog.nent[0]] != NVN_TEXTURE_TARGET_BUFFER + 1) {
            red1("tbuf_shader_type_unproven", "cs"); e->failed = 1; break;
        }
        if (le->cls == NVN_GPU_RES_IMAGE_TBUF && e->prog.dtt[0][e->prog.nent[0]] != NVN_TEXTURE_TARGET_BUFFER + 1) {
            red1("image_tbuf_shader_type_unproven", "cs"); e->failed = 1; break;
        }
        if (le->src_kind == NVN_RB_TEX && le->src_reg >= 32 && le->src_reg < 40 &&
            le->src_reg2 == UINT16_MAX && le->cls == NVN_GPU_RES_TEX_SAMP && le->count == 1)
            e->prog.sampler_unused[0][e->prog.nent[0]] = (uint8_t)s2a_spirv_sampler_unused(
                (const uint32_t *)cp.stage.spirv, cp.stage.spirv_bytes, (unsigned)b->set, (unsigned)b->binding);
        e->prog.nent[0]++;
    }
    if (!e->failed && !cp.stage.present) { red1("stage_missing_in_result", "cs"); e->failed = 1; }
    if (!e->failed) {
        nvn_gpu_layout L = {0}; L.ent = e->prog.ent[0]; L.nent = e->prog.nent[0]; L.support_bytes = cp.support_bytes; L.id_src = "s2a-direct-program-v1";
        nvn_gpu_shader_src ss = {0}; ss.stage = NVN_GPU_SH_CS; ss.kind = NVN_GPU_BLOB_SPIRV; ss.blob = cp.stage.spirv; ss.bytes = cp.stage.spirv_bytes; ss.entry = "main"; ss.layout = &L;
        ss.precompiled = (uint8_t)lib;
        if (key) memcpy(ss.key, key, 8);
        nvn_gpu_err err = {0};
        nvn_gpu_rc lr = nvn_gpu_shader_load(&ss, &e->prog.sh[0], &err);
        if (!nvn_gpu_ok(lr)) { red_load(lr, &ss, "compute_shader_load_failed", err.why); e->failed = 1; }
        prog_c1_take(&e->prog, 0, src->c1, src->c1_bytes);
    }
    nvn_vk_program_free_compute(&cp);
    return e;
}

// 计算程序的快路：与图形那条（input_sig + g_last_tab）同一套做法——先算「编译请求的全部输入」的摘要，命中就直接返回上次的 rp_prog*。
// 为什么值得：一款每帧 20 多个计算派发（GPU 剔除 / 后处理）的游戏，每次派发都走完整请求——32 + 128 个组合贴图槽逐槽取快照、
// 建编译请求（含整份 profile 的 JSON 解析与 SHA-256）、算身份、查表，真机上约 130 微秒 / 次、一帧 2.5 毫秒。
// 摘要 = 编译请求真正读的那些输入：来源代际、阶段键、用户常量缓冲「有没有」（掩码，不含地址 / 长度）、存储缓冲地址是否未对齐、
// 贴图池与采样器池的起点与容量、32 个组合槽与 128 个分离槽的编译事实摘要（格式 / 目标 / 深度，不含句柄本身）。
// 之前的键带着每次派发都不同的地址和句柄，再加每帧都推进的贴图登记代号，等于从不命中。
// 摘要只含「会进请求」的输入，所以同一个着色器换了绑定地址、换了同样格式的贴图，请求与身份逐字节不变，直接复用。
// 计算与图形共用 g_last_tab：摘要首项是计算专用的域标记，两边的摘要不会碰；rp_shader_reset 一并释放。
static uint64_t compute_input_sig(const struct s2a_direct_compile_context *pc) {
    uint64_t h = 1469598103934665603ull;
    #define MIX(v) do { h ^= (uint64_t)(v); h *= 1099511628211ull; } while (0)
    MIX(0xC5C5C5C5C5C5C5C5ull); MIX(s2a_source_epoch());   // 来源登记冲突会毒化短键并推进代际；命中快表前也必须失效。
    { uint64_t k; memcpy(&k, rp_S.skey[5], 8); MIX(k); }
    { uint32_t m = 0; for (int i = 0; i < 16; i++) if (rp_S.ubo[5][i].size) m |= 1u << i; MIX(m); }
    { int un = 0; for (int i = 0; i < 16; i++) if (rp_S.ssbo[5][i].addr && pc->storage_buffer_offset_alignment && rp_S.ssbo[5][i].addr % pc->storage_buffer_offset_alignment) un = 1; MIX(un); MIX(pc->storage_buffer_offset_alignment); }
    MIX(rp_S.tex_pool_n); MIX(rp_S.samp_pool_n); MIX(rp_S.tex_base);
    MIX(combined_fact_sig(5)); MIX(sep_fact_sig(5));
    { uint32_t m = 0; for (int i = 0; i < 8; i++) if (rp_S.img[5][i]) m |= 1u << i; MIX(m); MIX(rp_tex_fact_sig(rp_S.img[5], 8, rp_S.tex_base, rp_S.tex_pool_n)); }   // 图像事实（同请求：只出已绑的槽）
    if (g_ubo_need_n) MIX(ubo_fact_sig(5));   // 句柄在用户常量缓冲里的贴图（见 ubo_need_learn）：学过坐标的计算着色器，句柄指向的贴图换了事实就要换键；没学过任何坐标时一步不多走
    #undef MIX
    return h;
}
static int resolve_compute_slow(const struct rp_prog **out, const struct s2a_direct_compile_context *pcp);
// 记录实际计算程序；按与 libpack 同一口径保留存储缓冲对齐事实。
static void compute_warm_note(const struct rp_prog *p, const struct s2a_direct_compile_context *pc) {
    if (!nvn_plat_flag("nvk_compute_warm", 0)) return;
    struct nvk_prog_rec rec = {0};
    memcpy(rec.identity, p->identity, 32); rec.stage_mask = 32;
    memcpy(rec.keys[0], rp_S.skey[5], 8);
    if (p->c1_ok & 1) (void)rec_c1_put(&rec, 0, p->c1[0], p->c1_len[0]);
    int unaligned = 0;
    for (int i = 0; i < 16; i++)
        if (rp_S.ssbo[5][i].addr && pc->storage_buffer_offset_alignment &&
            rp_S.ssbo[5][i].addr % pc->storage_buffer_offset_alignment) unaligned = 1;
    rec.lp_ssbo_window = unaligned && p->lib_sourced;
    rec.lp_ok = !unaligned || rec.lp_ssbo_window ? 2 : 0;   // 非对齐且不是窗口库来的（现编出的）：预热不拿库当它的等价物
    nvk_prog_warm_note(&rec);
}
static int shader_resolve_compute(const struct rp_prog **out) {   // 快路在内层；外层 rp_shader_resolve_compute 做停局准入登记（#907）
    *out = NULL;
    if (!(rp_S.skey_ok >> 5 & 1)) { nvn_unimpl("shader", "compute_stage_key_missing", NULL); return -1; }
    if (!shader_texture_pool_ready()) return -1;
    struct s2a_direct_compile_context pc; memset(&pc, 0, sizeof pc);
    if (s2a_direct_provider_compile_context(&pc) || !pc.profile_json || !pc.profile_bytes) { nvn_unimpl("shader", "no_compile_context", NULL); return -1; }
    const int logging = cache_log_on();
    const uint64_t t0 = logging ? now_ns() : 0;
    const uint64_t key = compute_input_sig(&pc);
    static int check = -1, fast = -1;   // 两个调试开关只读一次（平台开关在进程内不变）：nvk_resolve_check 命中时再走慢路对账；nvk_cs_fast=0 关掉快路（真机 A/B 的「对照臂」，缺省开）
    if (check < 0) { check = nvn_plat_flag("nvk_resolve_check", 0); fast = nvn_plat_flag("nvk_cs_fast", 1) != 0; }
    struct last_ent *le = fast ? last_find(key, pc.session, pc.generation) : NULL;
    if (le && le->prog->lib_bad) le = NULL;   // 绑定核对拒收的出库版：快路不落命中，走慢路取现编
    if (le) {   // 不清任何状态：调用方只用 *out
        *out = le->prog;
        compute_warm_note(*out, &pc);
        if (check) { const struct rp_prog *chk = NULL; int rc = resolve_compute_slow(&chk, &pc); if (rc || chk != le->prog) red1("resolve_fast_path_mismatch", "快路径给的程序与慢路径不一致"); }
        g_cs_hits++;
        if (logging) { g_cs_hit_ns += now_ns() - t0; if (!((g_cs_hits + g_cs_misses) & 255)) cs_cache_log(); }
        return 0;
    }
    int rc = resolve_compute_slow(out, &pc);
    if (!rc && fast) last_put(key, pc.session, pc.generation, *out);
    if (!rc && *out) compute_warm_note(*out, &pc);
    g_cs_misses++;
    if (logging) { g_cs_miss_ns += now_ns() - t0; if (!((g_cs_hits + g_cs_misses) & 255)) cs_cache_log(); }
    return rc;
}
static int resolve_compute_slow(const struct rp_prog **out, const struct s2a_direct_compile_context *pcp) {
    struct s2a_direct_compile_context pc = *pcp;
    *out = NULL;
    static struct s2a_record_dispatch_state st; memset(&st, 0, sizeof st);
    memcpy(st.keys[0], rp_S.skey[5], 8); st.stages = 32;
    { uint32_t m = 0; for (int i = 0; i < 16; i++) if (rp_S.ubo[5][i].size) m |= 1u << i; st.ubo_use[5] = m << 3; }   // 用户常量缓冲从 c3 起
    st.tex_pool_n = (uint32_t)rp_S.tex_pool_n; st.samp_pool_n = (uint32_t)rp_S.samp_pool_n; st.tex_pool_seen = rp_S.tex_pool_n > 0; st.samp_pool_seen = rp_S.samp_pool_n > 0; st.tex_pool_base = rp_S.tex_base;
    // 贴图事实：计算阶段的 32 个组合槽，取法同图形（没绑过的槽读作池里的第 0 项）
    // 168 = 32 组合 + 128 分离 + 8 图像；再留 UBO_NEED_COORDS 项给句柄在用户常量缓冲里的贴图（ubo_need_learn 学到的坐标，每份着色器至多 8 个）
    enum { CS_TEX_N = 168 + UBO_NEED_COORDS };
    static struct s2a_compile_texture_src tex[CS_TEX_N]; static char tfmt[CS_TEX_N][S2A_TEX_FORMAT_MAX], tsmp[CS_TEX_N][S2A_TEX_SAMPLER_MAX]; int ntex = 0;
    for (int slot = 0; slot < 32; slot++) {
        uint64_t h = rp_S.tex[5][slot]; long id = h ? (long)(h & 0xFFFFF) : (long)rp_S.tex_base;
        tex_t t; view_t tv; int has_tv = 0; struct s2a_nvn_texture_fact nf; char why[96];
        if (rp_tex_snap_by_id((int)id, &t, &tv, &has_tv)) {
            if (request_texture_fact((uint32_t)(has_tv && tv.set_format ? tv.format : t.d.format), (uint32_t)(has_tv && tv.set_target ? tv.target : t.d.target), t.d.depth, &nf, why, sizeof why)) continue;
        } else s2a_nvn_default_texture_fact(id, rp_S.tex_base, (uint32_t)rp_S.tex_pool_n, &nf);
        memcpy(tfmt[ntex], nf.format, sizeof tfmt[ntex]); memcpy(tsmp[ntex], nf.sampler_type, sizeof tsmp[ntex]);
        tex[ntex] = (struct s2a_compile_texture_src){"cs", (uint32_t)(8 + 2 * slot), -1, tsmp[ntex], tfmt[ntex], nf.is_buffer, nf.normalized, 0, nf.array_length};
        ntex++;
    }
    for (int slot = 0; slot < 128; slot++) {
        uint64_t h = rp_S.tex[5][128 + slot]; long id = h ? (long)(h & 0xFFFFF) : (long)rp_S.tex_base;
        tex_t t; view_t tv; int has_tv = 0; struct s2a_nvn_texture_fact nf; char why[96];
        if (!rp_tex_snap_by_id((int)id, &t, &tv, &has_tv)) continue;
        if (request_texture_fact((uint32_t)(has_tv && tv.set_format ? tv.format : t.d.format),
                (uint32_t)(has_tv && tv.set_target ? tv.target : t.d.target), t.d.depth, &nf, why, sizeof why)) continue;
        memcpy(tfmt[ntex], nf.format, sizeof tfmt[ntex]); memcpy(tsmp[ntex], nf.sampler_type, sizeof tsmp[ntex]);
        tex[ntex] = (struct s2a_compile_texture_src){"cs", (uint32_t)(90 + 2 * slot), 2, tsmp[ntex], tfmt[ntex], nf.is_buffer, nf.normalized, 1, nf.array_length};
        ntex++;
    }
    // 图像事实：c2 图像句柄表 8 槽（字节 0x120 + 8k = 字 72 + 2k，同解析器的 72..86 与 iOS C2_IMG），句柄 = 贴图号（BindImages 已按池基址挪过）。
    //   着色器用立即句柄（cbufSlot −1）或从 c2 读（cbufSlot 2，编译器回落到 −1 的同字事实）取图像时，编译器要这一格的类型与格式（空轨 2nd 9000：cs handle 72）。
    //   没绑的槽、登记表里没有、格式表达不了的都不出事实：编译器如实报缺，不猜。
    for (int slot = 0; slot < 8; slot++) {
        uint64_t h = rp_S.img[5][slot];
        tex_t t; view_t tv; int has_tv = 0; struct s2a_nvn_texture_fact nf; char why[96];
        if (!h || !rp_tex_snap_by_id((int)(h & 0xFFFFF), &t, &tv, &has_tv)) continue;
        if (request_texture_fact((uint32_t)(has_tv && tv.set_format ? tv.format : t.d.format),
                (uint32_t)(has_tv && tv.set_target ? tv.target : t.d.target), t.d.depth, &nf, why, sizeof why)) continue;
        memcpy(tfmt[ntex], nf.format, sizeof tfmt[ntex]); memcpy(tsmp[ntex], nf.sampler_type, sizeof tsmp[ntex]);
        tex[ntex] = (struct s2a_compile_texture_src){"cs", (uint32_t)(72 + 2 * slot), -1, tsmp[ntex], tfmt[ntex], nf.is_buffer, nf.normalized, 0, nf.array_length};
        ntex++;
    }
    // 句柄在用户常量缓冲里的贴图（cbufSlot 3..18）：按编译器上一次报缺的坐标学（ubo_need_learn，cs 也认），之后这份着色器每次建请求都读那个字、按登记快照给事实。
    //   没学过任何坐标时（绝大多数计算着色器）不走这一步。读不到 / 没登记照图形口径处理（具名报告 / 默认事实），不猜类型。
    if (g_ubo_need_n) ntex = ubo_facts_into(5, tex, tfmt, tsmp, ntex, CS_TEX_N);
    struct nvk_compute_texture_facts tf = {tex, ntex, 1};
    int unaligned = 0;
    for (int i = 0; i < 16; i++) if (rp_S.ssbo[5][i].addr && pc.storage_buffer_offset_alignment && rp_S.ssbo[5][i].addr % pc.storage_buffer_offset_alignment) unaligned = 1;
    char ident[24]; { int o = 0; for (int b = 0; b < 8; b++) o += snprintf(ident + o, sizeof ident - (size_t)o, "%02x", rp_S.skey[5][b]); }
    static struct nvk_compute_request rq; memset(&rq, 0, sizeof rq); char why[256] = "";
    if (nvk_compute_request_prepare(&st, &pc, &tf, 1, unaligned, ident, &rq, why, sizeof why)) {
        red1("compute_request_failed", why);
        g_pool_request_rejected++;
        if (!strcmp(why, "missing or out-of-range descriptor-pool facts")) g_pool_request_facts++;
        else g_pool_request_other++;
        pool_reject_tick();
        return -1;
    }
    pthread_mutex_lock(&g_progs_mu);
    struct entry *e = progs_get_locked(rq.identity);
    if (e && e->prog.lib_bad) e = NULL;   // 绑定核对拒收的出库版：不落命中，下去取现编
    pthread_mutex_unlock(&g_progs_mu);
    if (!e) {
        if (g_texpool_diag) g_pool_programs++;
        uint8_t *bytes = NULL; size_t len = 0;
        const char *root = getenv("S2A_SHADER_REVISION_ROOT"), *cdir = getenv("S2A_DIRECT_CACHE_DIR");
        if (root && pc.profile_hash && pc.profile_hash[0] && !nvn_plat_flag("no_offline", 0)) {
            struct s2a_offline_request oq = {0}; memcpy(oq.identity, rq.identity, 32); oq.direct_abi = pc.direct_abi; oq.profile_hash = pc.profile_hash; oq.profile_declared = pc.offline_profile_available;
            struct s2a_offline_program op = {0};
            if (s2a_offline_library_fetch(root, &oq, &op, why, sizeof why) == S2A_OFFLINE_HIT) { bytes = op.bytes; len = op.len; atomic_fetch_add_explicit(&g_n_offline, 1, memory_order_relaxed); }
        }
        if (!bytes && cdir && !nvn_plat_flag("no_disk_cache", 0) && s2a_program_cache_load(cdir, rq.identity, &bytes, &len, NULL, why, sizeof why) == 0) atomic_fetch_add_explicit(&g_n_cache, 1, memory_order_relaxed);
        // 出包预编库：口径同图形那条（跨局缓存之后、现编之前；同一句柄、同一张不可用表）。键 = 计算阶段 8 字节控制键 rp_S.skey[5]
        struct entry *lpe = NULL;
        union lp_prepared *ready = NULL;
        struct stair_probe probe = stair_begin(!bytes, "fg");
        if (!bytes && libpack_fetch(rq.identity, NULL, 0, rp_S.skey[5], unaligned ? LIBPACK_SSBO_WINDOW : LIBPACK_ALIGNED, NULL, &pc, &bytes, &len, &ready, &probe)) {
            if (probe.start) probe.fetched = stair_us();
            lpe = build_compute(bytes, len, &rq.source, rq.identity, rp_S.skey[5], 1, ready);
            free(bytes); bytes = NULL; len = 0;
            stair_end(&probe, "fg", rq.identity, 32, lpe && !lpe->failed);
            if (lpe && !lpe->failed) { lpe->prog.lib_sourced = 1; libpack_note_hit(); }
            else { entry_free(lpe); lpe = NULL; libpack_mark_bad(rq.identity); }
        } else stair_end(&probe, "fg", rq.identity, 32, 0);
        if (!bytes && !lpe) {
            struct s2a_compile_outcome oc; memset(&oc, 0, sizeof oc);
            enum live_status status = live_compile(rq.session, rq.generation, rq.json, rq.json_bytes, rq.identity, 32u, NULL, &oc);
            if (status == LIVE_ABORT) {
                s2a_compile_outcome_free(&oc);
                nvk_compute_request_release(&rq);
                return -1;
            }
            if (status == LIVE_REJECTED) {
                atomic_fetch_add_explicit(&g_n_failed, 1, memory_order_relaxed);
                // 编译器报缺的是计算阶段用户常量缓冲里的句柄事实、且学到了新坐标：这次拒绝是学习的一步，下面带上它当场重建请求
                // （身份随事实变，不撞这份失败项），不记成「未接 compute_compile_failed」。学不到 / 重编到上限仍失败的，照常报红。
                const int learned = ubo_need_learn(oc.summary, (const uint8_t (*)[8])rp_S.skey, 6, 1u << 5);
                if (!learned) red1("compute_compile_failed", oc.summary);
                compiler_rejected(oc.summary, NULL);
                s2a_compile_outcome_free(&oc);
                e = calloc(1, sizeof *e);
                if (e) {
                    e->failed = 1; memcpy(e->prog.identity, rq.identity, 32);
                    pthread_mutex_lock(&g_progs_mu);
                    struct entry *have0 = progs_get_locked(rq.identity);
                    if (!have0) (void)nvn_hm_put(&g_progs, rq.identity, e);
                    pthread_mutex_unlock(&g_progs_mu);
                    if (have0) free(e);
                }
                nvk_compute_request_release(&rq);
                static int depth;   // 只提交线程进这里；每轮至少多学一个坐标、每份着色器至多 UBO_NEED_COORDS 个，深度有界（图形那条同款）
                if (!learned || depth >= UBO_NEED_COORDS) return -1;
                depth++;
                const int rc = resolve_compute_slow(out, pcp);
                depth--;
                return rc;
            }
            bytes = oc.bytes; len = oc.bytes_len; oc.bytes = NULL; s2a_compile_outcome_free(&oc);
            atomic_fetch_add_explicit(&g_n_compiled, 1, memory_order_relaxed);
            if (g_texpool_diag) { g_pool_compiled++; if (g_pool_attempts == 1 || !(g_pool_attempts % 50)) pool_stats("progress"); }
            if (cdir && !nvn_plat_flag("no_disk_cache", 0)) s2a_program_cache_store_async(cdir, rq.identity, bytes, len);
        }
        if (lpe) e = lpe;
        else { e = build_compute(bytes, len, &rq.source, rq.identity, rp_S.skey[5], 0, NULL); free(bytes); }
        pthread_mutex_lock(&g_progs_mu);
        struct entry *have = progs_get_locked(rq.identity);
        if (have && have->prog.lib_bad) { orphan_note(have); have = NULL; }   // 拒收旧项被现编版顶替：转孤儿表（借用方持有的指针仍有效），put 覆盖登记
        if (!have && e) (void)nvn_hm_put(&g_progs, rq.identity, e);
        pthread_mutex_unlock(&g_progs_mu);
        if (have) { entry_free(e); e = have; }
    }
    if (!e) { nvk_compute_request_release(&rq); return -1; }
    nvk_compute_request_release(&rq);
    if (e->failed) return -1;
    *out = &e->prog;
    return 0;
}

int rp_shader_resolve(const struct rp_shader_req *q, const struct rp_prog **out) {
    pool_log_flush();
    if (shader_enter()) { *out = NULL; nvn_unimpl("shader", "compile_stopped", NULL); return -1; }
    int rc = shader_resolve_graphics(q, out); shader_leave(); return rc;
}
int rp_shader_resolve_compute(const struct rp_prog **out) {
    if (shader_enter()) { *out = NULL; nvn_unimpl("shader", "compile_stopped", NULL); return -1; }
    int rc = shader_resolve_compute(out); shader_leave(); return rc;
}

static _Atomic uint64_t g_n_lib_reject;
// 绑定核对回退（rp_shader.h）：出库程序的声明在绑定期被发现与实际绑定不符——身份打进出库坏表
// （此后 libpack_fetch 一律 miss），并把已发布项标 lib_bad：各命中路径（发布项 / 事实复用 / 结果合并 /
// 在途等待 / 快路）统一绕过它，同身份的下一笔解析落到现编。标记过的对象可能仍被后台借用：
// 不在这里释放，由收尾的借用判据统一回收。
int rp_shader_lib_reject(const struct rp_prog *p) {
    if (!p || !p->lib_sourced) return 0;
    libpack_mark_bad(p->identity);
    pthread_mutex_lock(&g_progs_mu);
    ((struct rp_prog *)p)->lib_bad = 1;
    pthread_mutex_unlock(&g_progs_mu);
    uint64_t n = atomic_fetch_add(&g_n_lib_reject, 1) + 1;
    if (n <= 4 || !(n & 63)) { char b[160]; snprintf(b, sizeof b, "出库程序绑定核对拒收、退回现编（第 %llu 个，身份 %02x%02x%02x%02x…）", (unsigned long long)n,
        p->identity[0], p->identity[1], p->identity[2], p->identity[3]); nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", b); }
    return 1;
}
// Retained Android shutdown keeps the replay objects alive. Emit once, only after
// v5 has sealed and Stop has closed shader admission and drained the publishers.
void nvk_shader_stair_cut(void) {
    static _Atomic int emitted;
    if (!nvn_plat_flag("nvk_libpack_timing", 0) || !shader_stopped() || nvk_shader_inflight()) return;
    if (atomic_exchange(&emitted, 1)) return;
    char line[144];
    snprintf(line, sizeof line, "S2SB_CUT seq=%llu live=%llu time=%llu",
             (unsigned long long)atomic_load(&stair_sequence), (unsigned long long)atomic_load(&stair_exec_count),
             (unsigned long long)stair_us());
    nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", line);
}
void rp_shader_reset(void) {
    g_live_reason.recent_n = 0; atomic_store_explicit(&g_live_reason.said, 0, memory_order_relaxed);   // 现编原因日志按会话重新计（去重 / 条数）
    fact_cache_reset();
    if (g_pool_programs || g_pool_request_rejected) pool_stats("reset");
    // 后台预热一律不得越过这一步进会话收尾：先有界静默（置位后后台不再发布），再释放。
    // Lead 的调度器在 Stop 时已经调过一次 nvk_shader_warm_quiesce（幂等）；这里再调是为了不依赖调用顺序。
    // 静默判据现在同时含「程序借用」：借用方手里还握着 rp_prog*，这一步释放 entry 就是 UAF。
    if (nvk_shader_warm_quiesce(200)) nvn_plat_log(NVN_PLAT_LOG_RED, "warm", "【红】着色器收尾：预热带后台工作 / 程序借用没在预算内静默（迟到结果按代际丢弃；程序记录按下面的借用判据处理）");
    nvk_prog_warm_reset();
    pthread_mutex_lock(&g_progs_mu);
    // 收尾读数先取快照（正常收尾成功后计数清零，读数还得记在本局名下）。
    struct borrow_snap bs;
    bs.borrowed = g_borrowed; bs.peak = g_borrow_peak; bs.rejected = g_borrow_rejected;
    bs.unmatched = g_borrow_unmatched; bs.retained = g_retained_progs;
    g_progs_epoch++;   // 换代：之后任何在途构建都不得再发布（预热带会释放自己那份 entry）
    {   // 结果合并：同一个 entry 可能挂在多个身份键下（别名）。拥有者只留第一次见到的那一格，其余格置空——下面按格释放 / 计数时每个 entry 只算一次
        for (uint32_t i = 0; i < g_progs.cap; i++) { struct entry *e = (struct entry *)g_progs.vals[i]; if (e) e->seen = 1; }
        for (uint32_t i = 0; i < g_progs.cap; i++) {
            struct entry *e = (struct entry *)g_progs.vals[i];
            if (!e) continue;
            if (e->seen) e->seen = 0; else g_progs.vals[i] = NULL;
        }
    }
    if (bs.borrowed) {
        // 借用没归零：这些 entry 不能释放——借用方手里的 rp_prog* 指向它们，归还前仍要能读。
        // 表结构照样换成空表：旧 entry 的 nvn_gpu_shader 属于本世代设备，下一局按身份命中它就是悬垂句柄。
        // 旧 entry 整份泄漏到进程结束（只有「借用超时未归」这条异常路径会泄漏，每次收尾至多一批）；
        // 宁可泄漏，也不 UAF——归还之后这里也不再回收，因为迟到归还与回收之间没有可证明的顺序。
        uint64_t held = 0;
        for (uint32_t i = 0; i < g_progs.cap; i++) if (g_progs.vals[i]) held++;
        g_retained_progs += held;
        bs.retained = g_retained_progs;
    } else {
        for (uint32_t i = 0; i < g_progs.cap; i++) entry_free((struct entry *)g_progs.vals[i]);
        for (unsigned i = 0; i < g_n_orphan; i++) entry_free(g_orphan[i]);   // lib_bad 顶替下来的孤儿项：借用已归零，这里统一回收
        g_n_orphan = 0;
        g_borrow_peak = 0; g_borrow_rejected = 0; g_borrow_unmatched = 0;
        g_borrow_full_said = g_borrow_unmatched_said = 0;
    }
    free(g_progs.keys); free(g_progs.vals); nvn_hm_init(&g_progs, 32);
    free(g_res_tab.keys); free(g_res_tab.vals); nvn_hm_init(&g_res_tab, 32);   // 结果表只借用 entry，随上面的释放一起清
    memset(g_inflight, 0, sizeof g_inflight);
    g_inflight_n = g_inflight_bg = 0;
    g_warm_quiesced = 0;   // 下一局重新允许（open 也会清）
    pthread_mutex_unlock(&g_progs_mu);
    g_profile.ok = 0;
    // 快路的表随会话释放：里面存的是 rp_prog*，跨会话留着就是悬空指针。计算程序的快路也存在这张表里
    // （摘要首项是计算专用的域标记），这一次释放一并带走；下一次存项时再按需分配。
    free(g_last_tab); g_last_tab = NULL;
    memset(g_comb_sig, 0, sizeof g_comb_sig);
    memset(g_high_sig, 0, sizeof g_high_sig);
    memset(g_ubo_need, 0, sizeof g_ubo_need); g_ubo_need_n = 0; g_ubo_learned = 0; atomic_store(&g_ubo_default_fact, 0);
    memset(g_idg, 0, sizeof g_idg); memset(g_idg_set, 0, sizeof g_idg_set);
    g_idg_groups = g_idg_idents = g_idg_resolves = g_idg_compiles = g_idg_loads = g_idg_multi = 0; g_idg_last_ns = 0;
    g_cs_hits = g_cs_misses = g_cs_hit_ns = g_cs_miss_ns = 0; g_mark_cs_hits = g_mark_cs_misses = g_mark_cs_hit_ns = g_mark_cs_miss_ns = 0;
    g_cache_calls = g_cache_hits = g_cache_misses = g_cache_hit_ns = g_cache_miss_ns = 0; g_sig_recomp = 0;
    g_mark_calls = g_mark_hits = g_mark_misses = g_mark_hit_ns = g_mark_miss_ns = g_mark_sig = 0;
    g_texpool_logged = g_texpool_last_over_limit = g_texpool_diag = 0; g_pool_log_ms = 0; g_pool_log_dirty = 0;
    g_pool_programs = g_pool_attempts = g_pool_compiled = g_pool_rejected = g_pool_unsupported = 0;
    g_pool_request_rejected = g_pool_request_facts = g_pool_request_other = 0;
    g_pool_reason_count = 0; memset(g_pool_reasons, 0, sizeof g_pool_reasons);
    g_reject_lines = g_img_reject_lines = g_reject_total = g_img_reject_total = 0;
    // 收尾读数：被拒绝时 RED（未静默状态留在读数里），正常收尾 INFO（借用 0 也打，证明判据看过借用）。
    borrow_log(bs.borrowed ? NVN_PLAT_LOG_RED : NVN_PLAT_LOG_INFO, "reset", &bs);
}

// ---------- 对预热/管线配方的只读接口（rp_shader.h）----------
int rp_shader_prog_identity(const struct rp_prog *p, uint8_t out[32]) {
    if (!p || !out) return 0;
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= p->identity[i];
    if (!acc) return 0;   // 全 0 = 没有身份（失败项 / 还没算出来）：这种程序不进任何清单
    memcpy(out, p->identity, 32);
    return 1;
}
int rp_shader_prog_by_identity(const uint8_t identity[32], const struct rp_prog **out) {
    if (!identity || !out) return 0;
    *out = NULL;
    pthread_mutex_lock(&g_progs_mu);
    struct entry *e = progs_get_locked(identity);
    if (e && !e->failed) *out = &e->prog;
    pthread_mutex_unlock(&g_progs_mu);
    return *out ? 1 : 0;
}
// 借用：把「这个身份正在被用」登记进 g_progs_mu 下的借用表。借到的 rp_prog* 在归还前不会被
// rp_shader_reset 释放（收尾先等借用归零，超时就把记录改成进程级泄漏，见 rp_shader_reset）。
//   1 = 借到（*out 有效，跨线程只读；必须配一次 rp_shader_prog_return 同一个身份）
//   0 = 这个身份还没发布（含解析失败项：失败项不是可用程序，口径同 rp_shader_prog_by_identity）
//  -1 = 会话已停 / 参数错 / 借用表满（表满如实失败并计数，绝不静默降级成裸指针）
int rp_shader_prog_borrow(const uint8_t identity[32], const struct rp_prog **out) {
    if (out) *out = NULL;
    if (!identity || !out) {
        pthread_mutex_lock(&g_progs_mu); g_borrow_rejected++; pthread_mutex_unlock(&g_progs_mu);
        return -1;
    }
    pthread_mutex_lock(&g_progs_mu);
    if (shader_stopped()) { g_borrow_rejected++; pthread_mutex_unlock(&g_progs_mu); return -1; }
    struct entry *e = progs_get_locked(identity);
    if (!e || e->failed) { pthread_mutex_unlock(&g_progs_mu); return 0; }
    struct borrow_ent *slot = NULL;
    for (unsigned i = 0; i < BORROW_SLOTS && !slot; i++)
        if (g_borrow[i].n && !memcmp(g_borrow[i].identity, identity, 32)) slot = &g_borrow[i];
    if (!slot) for (unsigned i = 0; i < BORROW_SLOTS; i++)
        if (!g_borrow[i].n) { memset(&g_borrow[i], 0, sizeof g_borrow[i]); memcpy(g_borrow[i].identity, identity, 32); slot = &g_borrow[i]; break; }
    if (!slot) {
        g_borrow_rejected++;
        int said = g_borrow_full_said; g_borrow_full_said = 1;
        pthread_mutex_unlock(&g_progs_mu);
        if (!said) nvn_plat_log(NVN_PLAT_LOG_RED, "shader", "【红】程序借用表满（256 格）：这次不交给调用方裸指针，按取不到处理（读数里记「拒绝」）");
        return -1;
    }
    slot->n++;
    g_borrowed++;
    if (g_borrowed > g_borrow_peak) g_borrow_peak = g_borrowed;
    *out = &e->prog;
    pthread_mutex_unlock(&g_progs_mu);
    return 1;
}
// 归还：与一次成功的借用一一对应（参数是同一个身份）。归零时广播：预热静默 / 收尾正在
// g_progs_cv 上等「后台在途 + 借用」。没有对应借用的归还是调用方 bug：计数 + 报一次红，表不动。
void rp_shader_prog_return(const uint8_t identity[32]) {
    pthread_mutex_lock(&g_progs_mu);
    struct borrow_ent *slot = NULL;
    if (identity) for (unsigned i = 0; i < BORROW_SLOTS; i++)
        if (g_borrow[i].n && !memcmp(g_borrow[i].identity, identity, 32)) { slot = &g_borrow[i]; break; }
    if (!slot) {
        g_borrow_unmatched++;
        int said = g_borrow_unmatched_said; g_borrow_unmatched_said = 1;
        pthread_mutex_unlock(&g_progs_mu);
        if (!said) nvn_plat_log(NVN_PLAT_LOG_RED, "shader", "【红】程序借用：归还了一个没有对应借用的身份（调用方 bug，已计数；借用表不变）");
        return;
    }
    slot->n--;
    if (g_borrowed) g_borrowed--;
    if (!slot->n) memset(slot, 0, sizeof *slot);   // 空槽：身份不作数，下次借用整格重写
    // 归零这一下就是静默 / 收尾等的那个点；广播是幂等的，多叫醒几次无妨。
    pthread_cond_broadcast(&g_progs_cv);
    pthread_mutex_unlock(&g_progs_mu);
}
// 当前未归还借用数：[任意线程]。管线预热侧用它判断「现在收尾会不会被拒绝」。
int rp_shader_prog_borrowed(void) {
    pthread_mutex_lock(&g_progs_mu);
    int n = (int)g_borrowed;
    pthread_mutex_unlock(&g_progs_mu);
    return n;
}

// ---------- 预热：按跨会话清单重建成已发布的程序对象 ----------
// [后台]（Lead 的 nvn_warm.c 单线程调用）。语义：
//   0 = 已发布到 g_progs（同身份早已发布也算 0）；
//   1 = 该身份已被真实 draw 在途构建（同一张在途表）——立即返回，不占预热线程等编译；
//  -1 = 失败（跨局缓存/离线库里没有这份身份的结果字节，或 parse/建对象失败；已计数，本局不再重试）。
// 只按内容身份读盘 + parse + 建对象：清单里没有编译事实，预热不重建编译请求、不启动编译 worker；
// 现编请求只能由真实 draw/dispatch 捕获，预热读不到结果时让真实首用走需求路径。
int nvk_prog_warm_build(const struct nvk_prog_rec *rec) {
    if (!rec) return -1;
    int nst = stage_nst(rec->stage_mask);
    if (!nst) { warm_diag("程序预热：阶段掩码 %#x 不是 VS/FS/GS 组合，整条跳过", (unsigned)rec->stage_mask); nvk_prog_warm_stat_build(-1); return -1; }
    if (shader_stopped()) { warm_diag("程序预热：会话已置停局位，预热停止发布"); nvk_prog_warm_stat_build(-1); return -1; }   // 发布前校验：停局后不再建对象
    if (rec->stage_mask != 32u && alias_on()) {   // 结果合并目前只覆盖图形程序
        struct alias_key ak; memset(&ak, 0, sizeof ak);
        ak.nst = nst; ak.c1_ok = (uint8_t)(rec->c1_ok & (uint8_t)((1u << nst) - 1u));
        for (int st = 0; st < nst; st++) { ak.c1[st] = rec->c1[st]; ak.c1_len[st] = rec->c1_len[st]; }
        if (alias_try(rec->identity, 1, &ak, NULL)) { nvk_prog_warm_stat_build(0); return 0; }
    }
    uint64_t epoch;
    pthread_mutex_lock(&g_progs_mu);
    struct entry *have = progs_get_locked(rec->identity);
    if (have) {
        int failed = have->failed;
        pthread_mutex_unlock(&g_progs_mu);
        nvk_prog_warm_stat_build(failed ? -1 : 0);
        return failed ? -1 : 0;
    }
    if (inflight_find_locked(rec->identity)) {
        pthread_mutex_unlock(&g_progs_mu);
        nvk_prog_warm_stat_build(1);   // 真实 draw 正在构建同一身份：稍后重试，别在这里等
        return 1;
    }
    int claim = inflight_claim_locked(rec->identity, 1);
    epoch = g_progs_epoch;
    pthread_mutex_unlock(&g_progs_mu);
    if (claim < 0) { warm_diag("程序预热：认领失败（停局 / 预热已静默 / 在途表满）"); nvk_prog_warm_stat_build(-1); return -1; }
    // 建后端着色器 / 发布这段按「会话在工作」计数：停局会等它（有界），停局后不再开新的。
    // shader_enter 返回 0 = 已登记（与 rp_shader_resolve 的用法同一口径：非 0 才是停局拒绝）。
    // 这里曾经写成 !shader_enter()，等于「登记成功就中止」——真机上表现为预热 2727 条全部失败、建 0。
    if (shader_enter()) { warm_diag("程序预热：会话已置停局位，预热停止建设备对象"); inflight_abort(rec->identity); nvk_prog_warm_stat_build(-1); return -1; }
    struct entry *e = warm_work_run(rec, nst);
    shader_leave();
    // 清单记录缺 c1，但这份程序真的带 C1 绑定（BRX 跳表着色器：火把/粒子等）：**不能发布**。
    // 发布出去的程序 c1_ok=0，rp_draw 会把所有带 C1 绑定的绘制跳过（"c1_segment_not_captured"）——画面直接缺光/发黑。
    // 这里如实拒绝（计 retried），留给真实 draw 用源快照现场编：那次会把 c1 一起记进清单，下一局这条就修好了。
    if (e && !e->failed) {
        int need = 0;
        for (int st = 0; st < nst && st < RP_PROG_STAGES; st++) {
            if (e->prog.c1_ok & (1u << st)) continue;
            for (uint32_t i = 0; i < e->prog.nent[st] && i < RP_PROG_ENT_MAX; i++)
                if (e->prog.ent[st][i].src_kind == NVN_RB_C1) { need = 1; break; }
            if (need) break;
        }
        if (need) {
            static _Atomic unsigned n;
            unsigned k = atomic_fetch_add(&n, 1);
            if (k < 4) {
                char b[176];
                snprintf(b, sizeof b, "程序预热：清单 c1_ok=%#x 缺 C1 绑定所需的常量段，拒绝发布（留现场编，本次第 %u 条）",
                         (unsigned)rec->c1_ok, k + 1);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "warm", b);
            }
            entry_free(e);
            inflight_abort(rec->identity);
            nvk_prog_warm_stat_build(-1);
            return -1;   // 本局不再重试（重试也没用，清单要等真实 draw 把 c1 记回去）；真实 draw 会自己备出来
        }
    }
    if (!e) {   // 缓存/离线库里没有这份身份的结果字节：正常分支（不是 bug），真实 draw 到时发需求编译
        inflight_abort(rec->identity);
        nvk_prog_warm_stat_build(-1);
        return -1;
    }
    struct entry *used = NULL;
    int failed = 1;
    int pr = progs_publish(rec->identity, e, 1, epoch, &used, &failed);
    if (pr < 0) { warm_diag("程序预热：发布被拒（停局 / 换代 / 预热已静默）"); entry_free(e); nvk_prog_warm_stat_build(-1); return -1; }   // 停局 / 换代 / 已静默：丢弃
    if (pr == 0) entry_free(e);   // 首次结果为准（同身份早已发布，含失败项）
    int rc = failed ? -1 : 0;     // 失败标志在发布锁内读好带出来（发布后 entry 归缓存，不能再碰）
    nvk_prog_warm_stat_build(rc);
    return rc;
}

// [收尾线程] 预热线程退出后收拢后台在途构建：置静默（此后预热带一律不得发布），并最多等 budget_ms。
// 返回 0 = 已静；1 = 预算内没静（如实返回，调用方按保留会话处理）。有界：绝不等编译服务。
// 「静默」= 程序构建在途（g_inflight_bg）归零 **且** 程序借用归零：
//   · 前者只说明没有后台构建还在发布（迟到结果由代际 / 静默位丢弃）；
//   · 后者才说明没有消费者还握着 rp_prog*（rp_shader_reset 释放 entry 的前提）。
// 只等前者就是这条 P1：管线预热在用程序时 quiesce 返回 0，随后 reset 释放 entry → UAF。
int nvk_shader_warm_quiesce(uint32_t budget_ms) {
    uint64_t deadline = mono_ms() + budget_ms;
    pthread_mutex_lock(&g_progs_mu);
    g_warm_quiesced = 1;
    while (g_inflight_bg > 0 || g_borrowed > 0) {
        uint64_t now = mono_ms();
        if (now >= deadline) break;
        uint64_t left = deadline - now;
        uint32_t slice = left > 20 ? 20u : (uint32_t)left;
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += (long)slice * 1000000L;
        while (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        (void)pthread_cond_timedwait(&g_progs_cv, &g_progs_mu, &dl);
    }
    int busy = (int)(g_inflight_bg + g_borrowed);
    pthread_mutex_unlock(&g_progs_mu);
    return busy == 0 ? 0 : 1;
}
