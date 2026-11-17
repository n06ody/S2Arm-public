// 着色器来源（graphics/nvn/replay/rp_shader.h 的实现）：回放状态 → 编译事实 → 内容身份 →
//   包内离线库 / 跨局缓存 / 现场编译（C# 翻译器）→ 解析成每阶段 SPIR-V + 绑定表 → 后端着色器。
// 事实与身份的口径沿用保留零件 nvn/nvn_vk_compile_facts.c（它的入参是旧回放器的绘制快照，这里直接从 rp_S 填同一组事实）。
// 三段式：提交线程只捕获「不可变输入快照」（读 rp_S 填事实 / 算内容身份 / pin 住源引用）；工作函数只吃快照
// （离线库 → 跨局缓存 → 现场编译 → parse + 建后端着色器），同一身份在途只有一份；最后在锁里校验会话未停 /
// 代际未变才发布到 g_progs。预热带（nvn/nvn_warm.c）按跨会话清单在后台重建热点程序，真实 draw 直接命中；
// 取不到就照旧同步做，不跳过任何绘制。
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
#include "direct/input_identity.h"
#include "direct/source_store.h"
#include "direct/texture_facts.h"
#include "direct/offline_library.h"
#include "direct/libpack_vk.h"
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

// One atomic linearizes session stop with shader admission. The low bits
// retain every active resolver until it has finished using the session, not
// merely until the managed compiler returns.
#define NVK_SHADER_STOP_BIT (1u << 31)
static _Atomic unsigned g_shader_admission;
void nvk_shader_stop(void) { atomic_fetch_or_explicit(&g_shader_admission, NVK_SHADER_STOP_BIT, memory_order_acq_rel); }
unsigned nvk_shader_inflight(void) { return atomic_load_explicit(&g_shader_admission, memory_order_acquire) & ~NVK_SHADER_STOP_BIT; }
// 会话级在途 / 静默 / 代际清场（定义在下面的状态变量之后：open 在文件前面，用前向声明）。
static void shader_session_state_reset(void);
int nvk_shader_open(void) {
    unsigned expected = NVK_SHADER_STOP_BIT;
    if (atomic_compare_exchange_strong_explicit(&g_shader_admission, &expected, 0, memory_order_acq_rel, memory_order_acquire)) {
        shader_session_state_reset();   // 新会话：在途 / 静默位 / 代际清一遍（上一局的后台结果不得落进新会话）
        return 0;
    }
    return expected == 0 ? 0 : -1;
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
static _Atomic uint64_t g_n_alias_fg, g_n_alias_bg, g_n_alias_probe, g_n_alias_nores, g_n_alias_nofile;   // 合并（前台 / 预热带）/ 读头次数 / 头好但没有比对键一致的常驻程序 / 读头失败
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
    free(e);
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
// 发布。返回 1 = 插入（entry 归 g_progs）；0 = 同身份已存在（*existing 给出，调用方释放自己的 entry）；
// -1 = 丢弃（停局 / 代际变了 / 预热已静默；调用方释放自己的 entry）。首次结果为准：失败项与成功项互不覆盖
// （与原有 g_progs 语义一致：同一份输入只编一次，失败也不反复编）。
static int progs_publish(const uint8_t identity[32], struct entry *e, int from_warm, uint64_t epoch, struct entry **existing, int *failed) {
    if (existing) *existing = NULL;
    if (failed) *failed = 0;
    pthread_mutex_lock(&g_progs_mu);
    struct entry *have = progs_get_locked(identity);
    int rc;
    if (have) { if (existing) *existing = have; if (failed) *failed = have->failed; inflight_done_locked(identity); rc = 0; }
    else if (shader_stopped() || epoch != g_progs_epoch || (from_warm && g_warm_quiesced)) {
        // 发布被拒（停局 / 换代 / 预热已静默）：认领必须在这里结束——否则在途表里留一条永不完成的记录，
        // 之后同身份的 draw 会在 await_inflight 上一直等到停局，预热静默检查也会一直报「有在途」。
        inflight_done_locked(identity);
        rc = -1;
    } else {
        e->warm = from_warm;
        if (failed) *failed = e->failed;
        (void)nvn_hm_put(&g_progs, identity, e);
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
struct alias_key { int nst; uint8_t c1_ok; const uint8_t *c1[RP_PROG_STAGES]; };   // c1[st] 仅当 c1_ok 的位 st 为 1 时有效（长度 0x100）
// 比对键 = SHA-256(结果摘要 ‖ 负载长度 ‖ 阶段数 ‖ c1 有无位 ‖ 各有 c1 阶段的 c1[0x100])。键里每一项都改一下 → 键就变 → 不合并（见 nvk_shader_alias_test.c 的逐项负对照）。
// 有无位单独编进去：否则「只有阶段 0 有这段 c1」与「只有阶段 1 有同样的 c1」字节流相同。
static void alias_key_hash(const uint8_t res[32], uint64_t rlen, const struct alias_key *k, uint8_t out[32]) {
    uint8_t buf[32 + 8 + 2 + RP_PROG_STAGES * 0x100];
    size_t n = 0;
    memcpy(buf, res, 32); n += 32;
    for (int i = 0; i < 8; i++) buf[n++] = (uint8_t)(rlen >> (8 * i));
    buf[n++] = (uint8_t)k->nst;
    const uint8_t mask = (uint8_t)((1u << k->nst) - 1u);
    buf[n++] = (uint8_t)(k->c1_ok & mask);
    for (int st = 0; st < k->nst; st++)
        if (k->c1_ok & (1u << st)) { memcpy(buf + n, k->c1[st], 0x100); n += 0x100; }
    sha256(buf, n, out);
}
// 建好的程序挂上自己的比对键（结果摘要来自缓存文件头已校验的值 / 现编结果现算；阶段数与 c1 取自程序对象本身，与 build 用的是同一份）。
static void entry_set_res(struct entry *e, const uint8_t rsha[32], uint64_t rlen, int nst) {
    struct alias_key k; memset(&k, 0, sizeof k);
    k.nst = nst; k.c1_ok = e->prog.c1_ok;
    for (int st = 0; st < nst && st < RP_PROG_STAGES; st++) k.c1[st] = e->prog.c1[st];
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
    if (s2a_program_cache_probe(cdir, identity, res, &rlen, why, sizeof why)) { atomic_fetch_add_explicit(&g_n_alias_nofile, 1, memory_order_relaxed); return NULL; }
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
static void pool_stats(const char *phase) {
    if (!g_texpool_diag) return;
    char line[320];
    snprintf(line, sizeof line, "texture_pool_program_stats phase=%s programs=%llu compile_attempts=%llu compile_successes=%llu compile_rejected=%llu array_unsupported=%llu request_rejected=%llu",
        phase, (unsigned long long)g_pool_programs, (unsigned long long)g_pool_attempts,
        (unsigned long long)g_pool_compiled, (unsigned long long)g_pool_rejected,
        (unsigned long long)g_pool_unsupported, (unsigned long long)g_pool_request_rejected);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", line);
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
    // 借用读数跟着同一份诊断走：管线预热在用程序时，「借用」非 0 就是收尾必须等的那部分。
    { struct borrow_snap bs; borrow_snap_take(&bs); borrow_log(NVN_PLAT_LOG_INFO, phase, &bs); }
}
/* The normal Android process exit calls this after services/GPU stop. It does
 * not pass through rp_shader_reset, which belongs to session retirement. */
static void pool_final_summary(int normal) {
    if (normal && (g_pool_programs || g_pool_request_rejected)) pool_stats("final");
}
/* The producer's failure summary embeds its s2a-direct-error-v1 JSON. Missing,
 * truncated or malformed errors stay explicitly unclassified. */
static void compiler_rejected(const char *summary) {
    if (!g_texpool_diag) return;
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
    if (g_pool_rejected == 1 || !(g_pool_rejected % 50)) pool_stats("progress");
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

struct facts {
    struct s2a_compile_graphics_src gfx;
    struct s2a_compile_texture_src tex[S2A_COMPILE_TEXTURE_MAX]; int ntex;
    char tfmt[S2A_COMPILE_TEXTURE_MAX][S2A_TEX_FORMAT_MAX], tsmp[S2A_COMPILE_TEXTURE_MAX][S2A_TEX_SAMPLER_MAX];
    uint32_t ubo_use[6];
};

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
static int ubo_need_learn(const char *summary, const uint8_t (*keys)[8], int nst) {
    if (!summary || !strstr(summary, "\"code\":\"MISSING_FACT\"")) return 0;
    const char *m = strstr(summary, " has no source (stage=");
    if (!m) return 0;
    m += strlen(" has no source (stage=");
    int st = !strncmp(m, "vs,", 3) ? 0 : !strncmp(m, "fs,", 3) ? 1 : !strncmp(m, "gs,", 3) ? 2 : -1;
    unsigned word = 0; int cbuf = 0; char close = 0;
    if (st < 0 || st >= nst || sscanf(m + 3, "handle=%u,cbufSlot=%d%c", &word, &cbuf, &close) != 3 || close != ')') return 0;
    if (cbuf < 3 || cbuf > 18 || word >= 0x10000 / 4) return 0;
    struct ubo_need *e = ubo_need_at(st, keys[st], 1);
    if (!e) { red1("ubo_handle_need_table_full", "着色器的常量缓冲句柄坐标表满"); return 0; }
    for (unsigned i = 0; i < e->n; i++) if (e->c[i].word == word && e->c[i].cbuf == cbuf) return 0;
    if (e->n == UBO_NEED_COORDS) { red1("ubo_handle_need_coords_full", "一份着色器读的常量缓冲句柄超过 8 个"); return 0; }
    e->c[e->n].word = (uint16_t)word; e->c[e->n].cbuf = (uint8_t)cbuf; e->n++;
    return 1;
}
// 常量缓冲 cbuf（c3 起 = 用户槽 cbuf − 3）字下标 word 处的句柄 → 贴图号。-1 = 槽没绑 / 越界 / 不在登记过的池里
static long ubo_tex_id(int st, unsigned cbuf, unsigned word) {
    const unsigned reg = cbuf - 3, off = word * 4;
    if (reg >= 16) return -1;
    const p_bind_buf *u = &rp_S.ubo[st][reg];
    if (!u->addr || (uint64_t)off + 4 > u->size || !rp_pool_find(u->addr + off, 4, NULL, NULL)) {
        char b[200];
        snprintf(b, sizeof b, "常量缓冲句柄读不到：%s c%u 第 %u 字，槽地址 0x%llx 大小 %llu（%s）", st == 0 ? "vs" : st == 1 ? "fs" : "gs", cbuf, word,
                 (unsigned long long)u->addr, (unsigned long long)u->size, !u->addr ? "槽没绑" : (uint64_t)off + 4 > u->size ? "越界" : "不在登记过的池里");
        red1("ubo_handle_unreadable", b);
        return -1;
    }
    uint32_t h; memcpy(&h, (const void *)(uintptr_t)(u->addr + off), 4);
    return (long)(h & 0xFFFFF) + rp_S.tex_base;
}
static int request_texture_fact(uint32_t fmt, uint32_t target, uint32_t layers, struct s2a_nvn_texture_fact *out, char *why, size_t why_size);
static void ubo_facts_add(struct facts *f, int st) {
    const struct ubo_need *e = ubo_need_find(st);
    for (unsigned i = 0; e && i < e->n && f->ntex < S2A_COMPILE_TEXTURE_MAX; i++) {
        long id = ubo_tex_id(st, e->c[i].cbuf, e->c[i].word);
        tex_t t; view_t tv; int has_tv = 0; struct s2a_nvn_texture_fact nf; char why[96];
        if (id < 0) continue;   // ubo_tex_id 已具名报过
        if (!rp_tex_snap_by_id((int)id, &t, &tv, &has_tv)) {
            char b[160]; snprintf(b, sizeof b, "常量缓冲句柄指向的贴图取不到：c%u 第 %u 字 → 贴图号 %ld（贴图池基址 %d）", e->c[i].cbuf, e->c[i].word, id, (int)rp_S.tex_base);
            red1("ubo_handle_no_texture", b); continue;
        }
        if (request_texture_fact((uint32_t)(has_tv && tv.set_format ? tv.format : t.d.format), (uint32_t)(has_tv && tv.set_target ? tv.target : t.d.target), t.d.depth, &nf, why, sizeof why)) {
            char b[240]; snprintf(b, sizeof b, "常量缓冲句柄的贴图出不了编译事实：贴图号 %ld 格式 0x%x 目标 %u：%s", id, (unsigned)(has_tv && tv.set_format ? tv.format : t.d.format), (unsigned)(has_tv && tv.set_target ? tv.target : t.d.target), why);
            red1("ubo_handle_no_fact", b); continue;
        }
        int n = f->ntex++;
        memcpy(f->tfmt[n], nf.format, sizeof f->tfmt[n]); memcpy(f->tsmp[n], nf.sampler_type, sizeof f->tsmp[n]);
        f->tex[n] = (struct s2a_compile_texture_src){st == 0 ? "vs" : st == 1 ? "fs" : "gs", e->c[i].word, e->c[i].cbuf, f->tsmp[n], f->tfmt[n], nf.is_buffer, nf.normalized, 0, nf.array_length};
    }
}
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
        } else UMIX(2);
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
        } else {
            int in_pool = id >= rp_S.tex_base && (uint64_t)(id - rp_S.tex_base) < (uint64_t)rp_S.tex_pool_n;
            memset(&nf, 0, sizeof nf); snprintf(nf.format, sizeof nf.format, "Unknown");
            snprintf(nf.sampler_type, sizeof nf.sampler_type, "%s", in_pool ? "1d" : "2d"); nf.normalized = !in_pool; nf.array_length = 1;
        }
        int n = f->ntex++;
        memcpy(f->tfmt[n], nf.format, sizeof f->tfmt[n]); memcpy(f->tsmp[n], nf.sampler_type, sizeof f->tsmp[n]);
        f->tex[n] = (struct s2a_compile_texture_src){(const char *const[]){"vs", "fs", "gs"}[st], (uint32_t)(8 + 2 * slot), -1, f->tsmp[n], f->tfmt[n], nf.is_buffer, nf.normalized, 0, nf.array_length};
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
    if (b->kind == 2 && !b->texel_buffer && b->count > 1 && b->slot == 0) { e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEX; e->src_reg = 0; e->count = b->count; return 1; }
    if (b->kind == 2 && !b->texel_buffer && b->tex_slot >= 0) { e->cls = NVN_GPU_RES_TEX_SAMP; e->src_kind = NVN_RB_TEX; e->src_reg = (uint16_t)b->tex_slot; e->src_reg2 = b->samp_slot >= 0 ? (uint16_t)b->samp_slot : UINT16_MAX; return 1; }
    // 存储缓冲：只认「NVN 存储槽」这一种来源（反射里的 nvnSlot）。地址来自 UBO 内 SbDescriptor / c1 段的要逐笔读
    // 常量缓冲内容，首版还没接，准确报红而不是猜槽位。
    if (b->kind == 1 && b->storage_kind == 1 && b->storage_slot >= 0 && b->storage_slot < 16) {
        e->cls = NVN_GPU_RES_SSBO;
        e->src_kind = NVN_RB_SBUFP;
        e->src_reg = (uint16_t)b->storage_slot;
        if (b->storage_writable) e->access = NVN_GPU_ACC_R | NVN_GPU_ACC_W;
        return 1;
    }
    // 图像：只接计算阶段的单个存储图像（解析器给出 NVN 图像槽 0..7，放在 tex_slot）；回放按槽取 BindImage 的句柄建存储视图
    //   （rp_draw.c rp_dispatch NVN_RB_IMG）。图形阶段的图像回放还没接，绘制时按名跳过（compute_image_shape）。
    if (b->kind == 3 && b->count == 1 && b->tex_slot >= 0 && b->tex_slot < 8) {
        e->cls = NVN_GPU_RES_IMAGE; e->src_kind = NVN_RB_IMG; e->src_reg = (uint16_t)b->tex_slot;
        e->access = NVN_GPU_ACC_R | NVN_GPU_ACC_W;
        return 1;
    }
    return 0;   // 图形阶段的图像、存储缓冲的其它地址来源：随对应命令接
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
    else if (b->samp_src == FD_SAMP_ID) { u->samp_kind = NVN_RB_SAMPI; u->samp_reg = b->samp_val; }
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
static struct entry *build(const uint8_t *bytes, size_t len, const struct s2a_source_entry_view views[3], int nst,
                           const uint8_t identity[32]) {
    struct entry *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    memcpy(e->prog.identity, identity, 32);
    struct fd_program fp; char why[192] = "";
    memset(&fp, 0, sizeof fp);
    int parsed = nvn_vk_program_parse_graphics_stages(bytes, len, &fp, nst == 1, why, sizeof why);
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
    for (int st = 0; st < nst && st < RP_PROG_STAGES; st++) if (fp.stage[st].binst_unread) e->prog.binst_unread |= (uint8_t)(1u << st);
    e->prog.z_raw = depth_clip_cap() == 1;   // 同 fill_facts：有管线裁剪深度时所有图形程序都不改写 z（预编库的程序另在取用处置 1）
    e->prog.color_out_mask = nst >= 2 && fp.stage[1].present ? fp.stage[1].fragment_output_map : 0xffffffffu;
    { static _Atomic int said; if (nst >= 2 && nvn_plat_flag("nvk_drawlog", 0) && atomic_fetch_add(&said, 1) < 40) { char m[96]; snprintf(m, sizeof m, "片元输出映射 %#x（片元代码 %zu 字节）", (unsigned)e->prog.color_out_mask, fp.stage[1].spirv_bytes); nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", m); } }
    for (int st = 0; st < nst && !e->failed; st++) {
        if (!fp.stage[st].present) { red1("stage_missing_in_result", st ? "fs" : "vs"); e->failed = 1; break; }
        for (int i = 0; i < fp.binding_count && !e->failed; i++) {
            const struct fd_binding *b = &fp.bindings[i];
            if (!(b->is_support || b->stage_idx == st)) continue;
            if (e->prog.nent[st] == RP_PROG_ENT_MAX) { red1("too_many_bindings", ""); e->failed = 1; break; }
            nvn_gpu_layout_ent *le = &e->prog.ent[st][e->prog.nent[st]];
            if (!layout_ent(b, le)) { char nm[40]; snprintf(nm, sizeof nm, "binding_kind_%d_slot_%d", b->kind, b->slot); red1(nm, b->stage); e->failed = 1; break; }
            if ((le->src_kind == NVN_RB_TEXU || le->src_kind == NVN_RB_TBUFU) && !usrc_put(&e->prog, b, le)) { red1("too_many_uniform_handles", b->stage); e->failed = 1; break; }
            if (le->cls == NVN_GPU_RES_TBUF && le->src_kind != NVN_RB_TBUF) { red1("texture_buffer_replay_unsupported", b->stage); e->failed = 1; break; }
            le->stages = b->is_support ? (1u << nst) - 1u : 1u << st;
            if (b->kind == 2) sampled_decl(fp.stage[st].spirv, fp.stage[st].spirv_bytes, (uint32_t)b->set, (uint32_t)b->binding,
                &e->prog.dtt[st][e->prog.nent[st]], &e->prog.dcls[st][e->prog.nent[st]], NULL);
            if (le->cls == NVN_GPU_RES_TBUF && e->prog.dtt[st][e->prog.nent[st]] != NVN_TEXTURE_TARGET_BUFFER + 1) {
                red1("tbuf_shader_type_unproven", b->stage); e->failed = 1; break;
            }
            e->prog.nent[st]++;
        }
        if (e->failed) break;
        nvn_gpu_layout L = {0}; L.ent = e->prog.ent[st]; L.nent = e->prog.nent[st]; L.support_bytes = fp.support_bytes; L.id_src = "s2a-direct-program-v1";
        nvn_gpu_shader_src src = {0}; src.stage = (nvn_gpu_stage)st; src.kind = NVN_GPU_BLOB_SPIRV;
        src.blob = fp.stage[st].spirv; src.bytes = fp.stage[st].spirv_bytes; src.entry = "main"; src.layout = &L;
        nvn_gpu_err err = {0};
        nvn_gpu_rc lr = nvn_gpu_shader_load(&src, &e->prog.sh[st], &err);
        if (!nvn_gpu_ok(lr)) { red_load(lr, &src, "shader_load_failed", err.why); e->failed = 1; }
        if (views[st].c1 && views[st].c1_bytes >= 0x100) { memcpy(e->prog.c1[st], views[st].c1, 0x100); e->prog.c1_ok |= (uint8_t)(1u << st); }
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
static int libpack_state_ok(const struct s2a_compile_graphics_src *g) {
    return (!g->depth_mode || depth_clip_cap()) && !g->y_negate && (!g->alpha_test_compare || !strcmp(g->alpha_test_compare, "Always")) &&
           !g->alpha_to_coverage_enable && !g->dual_source_blend &&
           !(g->has_unaligned_ssbo && g->unaligned_ssbo);   // 预编库按 Metal 翻译、存储缓冲一律当对齐：Vulkan 绑定要往下对齐到 minStorageBufferOffsetAlignment，偏差只有现编（读 c0 的偏移）会补
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
    int lp_ok;                               // libpack_state_ok（快照时按事实算好：工作线程不碰 rp_S）
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
    s->stages = stages; s->nst = nst; s->lp_ok = libpack_state_ok(&s->f.gfx);
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
static nvn_hmap g_lp_bad; static int g_lp_bad_init;
static _Atomic uint64_t g_n_libpack, g_n_libpack_bad;
static struct s2a_libpack *libpack_for_locked(const char *root) {
    static struct s2a_libpack *lp; static char lp_root[1024]; static int lp_red;
    if (!lp || strcmp(lp_root, root)) {
        char dir[1200]; snprintf(dir, sizeof dir, "%s/shaders/direct/b-%s-vk", root, DMSH_CHAIN);
        struct s2a_libpack *np = NULL; int orc = s2a_libpack_open(&np, dir);
        if (orc == 1) { s2a_libpack_close(lp); lp = np; snprintf(lp_root, sizeof lp_root, "%s", root);
            char b[1300]; snprintf(b, sizeof b, "出包预编库已打开：%s", dir); nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", b); }
        else { lp = NULL; lp_root[0] = 0; if (orc < 0 && !lp_red++) red1("libpack_container_rejected", dir); }
    }
    return lp;
}
// 取一份（图形：keys 是 vs / fs 两阶段键；计算：ckey 非空）。1 = 取到（*bytes 归调用方 free）
static int libpack_fetch(const uint8_t identity[32], const uint8_t (*gkeys)[8], int nst, const uint8_t *ckey, int state_ok,
                         const struct s2a_direct_compile_context *pc, uint8_t **bytes, size_t *len) {
    const char *root = getenv("S2A_SHADER_REVISION_ROOT");
    if (!root || (!ckey && (nst != 2 || !state_ok)) || nvn_plat_flag("no_libpack", 0)) return 0;
    char lw[192] = ""; int lrc = S2A_LIBPACK_MISS;
    pthread_mutex_lock(&g_lp_mu);
    if (!g_lp_bad_init) { nvn_hm_init(&g_lp_bad, 32); g_lp_bad_init = 1; }
    struct s2a_libpack *lp = nvn_hm_get(&g_lp_bad, identity) ? NULL : libpack_for_locked(root);
    if (lp) {
        if (ckey) lrc = s2a_libpack_compute_v1(lp, ckey, (uint32_t)pc->direct_abi, pc->profile_hash, pc->translator_closure, bytes, len, lw, sizeof lw);
        else {
            uint8_t keys[6][8]; memset(keys, 0, sizeof keys);
            memcpy(keys[0], gkeys[0], 8); memcpy(keys[1], gkeys[1], 8);
            lrc = s2a_libpack_program_v1(lp, keys, 0x3, (uint32_t)pc->direct_abi, pc->profile_hash, pc->translator_closure, bytes, len, lw, sizeof lw);
        }
    }
    pthread_mutex_unlock(&g_lp_mu);
    if (!lp) return 0;
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
// 程序累计一行：出包预编 / 离线库 / 跨局缓存 / 现场编 合计第 1 个和每 50 个打一次（每发布一份新程序合计正好加 1）
static void prog_tally_log(void) {
    uint64_t n = atomic_load(&g_n_libpack) + atomic_load(&g_n_offline) + atomic_load(&g_n_cache) + atomic_load(&g_n_compiled) + atomic_load(&g_n_alias_fg);
    if (n != 1 && (!n || n % 50)) return;
    char b[420]; int bo = snprintf(b, sizeof b, "程序累计：出包预编 命中 %llu / 没有 %llu / 坏 %llu / 不可用 %llu、离线库 %llu、跨局缓存 %llu、现场编 %llu、失败 %llu",
        (unsigned long long)atomic_load(&g_n_libpack), (unsigned long long)s2a_libpack_misses(), (unsigned long long)s2a_libpack_rejects(),
        (unsigned long long)atomic_load(&g_n_libpack_bad), (unsigned long long)atomic_load(&g_n_offline), (unsigned long long)atomic_load(&g_n_cache),
        (unsigned long long)atomic_load(&g_n_compiled), (unsigned long long)atomic_load(&g_n_failed));
    if (alias_on() && bo > 0 && (size_t)bo < sizeof b) {   // 结果合并读数（开着才出）
        uint32_t resn = 0;
        pthread_mutex_lock(&g_progs_mu); resn = g_res_tab.n; pthread_mutex_unlock(&g_progs_mu);
        snprintf(b + bo, sizeof b - (size_t)bo, "、结果合并 前台 %llu / 预热 %llu（读头 %llu、没有可合并的常驻程序 %llu、读头失败 %llu；常驻比对键 %u 个）",
            (unsigned long long)atomic_load(&g_n_alias_fg), (unsigned long long)atomic_load(&g_n_alias_bg), (unsigned long long)atomic_load(&g_n_alias_probe),
            (unsigned long long)atomic_load(&g_n_alias_nores), (unsigned long long)atomic_load(&g_n_alias_nofile), (unsigned)resn);
    }
    nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", b);
}

// [提交 / 后台] 只吃快照：离线库 → 跨局缓存 →（仅提交线程）现场编译 → parse + 建后端着色器。
// background 时没有结果字节就返回 NULL（清单里没有编译事实，编译请求只能由提交线程的那一份快照建；
// 也不去启动编译 worker：nvk 层没有需求编译 worker，启动它会改变会话编译拓扑）。
// 返回失败项 entry（failed=1）表示编译失败——调用方按「同一份输入不再反复编」发布它。
static struct entry *work_run(const struct work_snap *s, int background) {
    uint8_t *bytes = NULL; size_t len = 0; char why[256] = "";
    uint8_t rsha[32]; int have_res = 0;   // 结果摘要（跨局缓存读到的已校验摘要 / 现编结果现算的）：挂在 entry 上给结果合并用，离线库 / 预编库来的不挂
    const char *root = getenv("S2A_SHADER_REVISION_ROOT"), *cdir = getenv("S2A_DIRECT_CACHE_DIR");
    if (root && s->pc.profile_hash && s->pc.profile_hash[0] && !nvn_plat_flag("no_offline", 0)) {
        struct s2a_offline_request oq = {0};
        memcpy(oq.identity, s->identity, 32);
        oq.direct_abi = (uint32_t)s->pc.direct_abi;
        oq.profile_hash = s->pc.profile_hash;
        oq.profile_declared = s->pc.offline_profile_available;
        struct s2a_offline_program op = {0};
        if (s2a_offline_library_fetch(root, &oq, &op, why, sizeof why) == S2A_OFFLINE_HIT) {
            bytes = op.bytes; len = op.len;
            atomic_fetch_add_explicit(&g_n_offline, 1, memory_order_relaxed);
        }
    }
    if (!bytes && cdir && !nvn_plat_flag("no_disk_cache", 0) &&
        s2a_program_cache_load(cdir, s->identity, &bytes, &len, rsha, why, sizeof why) == 0) {
        atomic_fetch_add_explicit(&g_n_cache, 1, memory_order_relaxed);
        have_res = 1;
    }
    if (!bytes && libpack_fetch(s->identity, (const uint8_t (*)[8])s->keys, s->nst, NULL, s->lp_ok, &s->pc, &bytes, &len)) {
        struct entry *le = build(bytes, len, s->views, s->nst, s->identity);
        if (le) le->prog.z_raw = 1;
        free(bytes); bytes = NULL; len = 0;
        if (le && !le->failed) { libpack_note_hit(); return le; }
        if (le) entry_free(le);
        libpack_mark_bad(s->identity);
    }
    if (!bytes && !background) {
        // 现场编译：提交线程同步做（取不到就地做，不跳过绘制）。
        static struct s2a_compile_request_input in;   // 提交线程独占
        memset(&in, 0, sizeof in);
        in.session = s->pc.session; in.generation = s->pc.generation;
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
        if (g_texpool_diag) g_pool_attempts++;
        if (!shader_compile_admit()) { free(json); nvn_unimpl("shader", "compile_stopped", NULL); return NULL; }
        int ran = s2a_compile_request_execute(in.session, in.generation, json, jl, &oc);
        free(json);
        if (ran || !oc.bytes || !oc.bytes_len) {
            atomic_fetch_add_explicit(&g_n_failed, 1, memory_order_relaxed);
            // 编译器报缺的是用户常量缓冲里的句柄事实、且学到了新坐标：resolve_slow 会带上它当场重编，这次拒绝是学习的一步，
            // 不记成「未接 compile_failed」（不然每份着色器第一次见这类句柄都多几条假红）。学不到 / 重编到上限仍失败的，下一次照常报红。
            if (ubo_need_learn(oc.summary, (const uint8_t (*)[8])s->keys, s->nst)) g_ubo_learned = 1;
            else red1("compile_failed", oc.summary);
            compiler_rejected(oc.summary);
            s2a_compile_outcome_free(&oc);
            struct entry *fe = (struct entry *)calloc(1, sizeof *fe);
            if (fe) { fe->failed = 1; memcpy(fe->prog.identity, s->identity, 32); }
            return fe;
        }
        bytes = oc.bytes; len = oc.bytes_len; oc.bytes = NULL;
        s2a_compile_outcome_free(&oc);
        atomic_fetch_add_explicit(&g_n_compiled, 1, memory_order_relaxed);
        if (g_texpool_diag) { g_pool_compiled++; if (g_pool_attempts == 1 || !(g_pool_attempts % 50)) pool_stats("progress"); }
        if (cdir && !nvn_plat_flag("no_disk_cache", 0)) s2a_program_cache_store_async(cdir, s->identity, bytes, len);
        if (alias_on()) { sha256(bytes, len, rsha); have_res = 1; }   // 与缓存文件头里写的是同一个量（整份负载的 SHA-256）
    }
    if (!bytes) return NULL;
    struct entry *e = build(bytes, len, s->views, s->nst, s->identity);
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
static struct entry *build_compute(const uint8_t *, size_t, const struct s2a_source_entry_view *, const uint8_t[32]);

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
    for (int st = 0; st < nst && st < 3; st++)
        if (rec->c1_ok & (1u << st)) { views[st].c1 = rec->c1[st]; views[st].c1_bytes = NVK_PROG_REC_C1_BYTES; }
    if (!bytes && libpack_fetch(rec->identity, compute ? NULL : (const uint8_t (*)[8])rec->keys,
                                compute ? 0 : nst, compute ? rec->keys[0] : NULL, rec->lp_ok == 2, &pc, &bytes, &len)) {
        struct entry *le = compute ? build_compute(bytes, len, &views[0], rec->identity) : build(bytes, len, views, nst, rec->identity);
        if (le && !compute) le->prog.z_raw = 1;
        free(bytes); bytes = NULL; len = 0;
        if (le && !le->failed) { libpack_note_hit(); return le; }
        if (le) entry_free(le);
        libpack_mark_bad(rec->identity);
    }
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
    struct entry *e = compute ? build_compute(bytes, len, &views[0], rec->identity) : build(bytes, len, views, nst, rec->identity);
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
    static uint8_t k[3][8];
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
        if (v[got].c1 && v[got].c1_bytes >= NVK_PROG_REC_C1_BYTES) { k.c1_ok |= (uint8_t)(1u << got); k.c1[got] = v[got].c1; }   // 与 build() 的 c1 判据同源
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
    struct nvk_prog_rec rec; memset(&rec, 0, sizeof rec); memcpy(rec.identity, sha, 32); rec.stage_mask = stages; rec.lp_ok = libpack_state_ok(&f->gfx) ? 2 : 0;
    for (int st = 0; st < nst; st++) memcpy(rec.keys[st], id->keys[st], 8);
    rec.c1_ok = (uint8_t)(prog->c1_ok & (uint8_t)((1u << nst) - 1u));
    for (int st = 0; st < nst; st++)
        if (rec.c1_ok & (1u << st)) memcpy(rec.c1[st], prog->c1[st], NVK_PROG_REC_C1_BYTES);
    // 程序对象本身也没有 c1（例如它是上一局按残缺清单预热出来的）：这一笔手上正好有源快照，补采一次，
    // 让清单在下一次落盘时被修好（这一局仍会跳过该笔，下一局不再跳）。
    if ((rec.c1_ok & (uint8_t)((1u << nst) - 1u)) != (uint8_t)((1u << nst) - 1u)) {
        for (int st = 0; st < nst; st++) {
            if (rec.c1_ok & (1u << st)) continue;
            struct s2a_source_entry_view v;
            if (s2a_source_acquire(id->keys[st], &v)) {
                if (v.complete && v.c1 && v.c1_bytes >= NVK_PROG_REC_C1_BYTES) {
                    memcpy(rec.c1[st], v.c1, NVK_PROG_REC_C1_BYTES);
                    rec.c1_ok |= (uint8_t)(1u << st);
                }
                s2a_source_release(v.ref);
            }
        }
    }
    nvk_prog_warm_note(&rec);
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
    if (e) {
        if (e_failed) return -1;
        *out = &e->prog;
        prog_note_use(sha, stages, nst, &id, &f, &e->prog);
        return 0;
    }
    // 1b) 事实复用（#1187）：编译器只读了一部分绑定事实，事实相同的另一份身份可以直接用同一个程序。
    //   别名表与变体表只有提交线程读写（rp_shader_reset 清），不进 g_progs_mu；别名不持有程序，程序只归 g_progs。
    if ((*out = nvn_hm_get(&g_fact_aliases, sha))) return 0;
    uint8_t fact_base[32];
    const int reusable = fact_cache_shape(&id) && fact_cache_hash(&id, NULL, fact_base);
    if (reusable && (*out = fact_cache_find(&id, fact_base))) { nvn_hm_put(&g_fact_aliases, sha, (void *)*out); return 0; }
    // 1c) 结果合并（#1355）：这个身份的缓存文件头里记的结果摘要，已经有常驻程序 → 登记成别名直接用（只读 88 字节头，不读负载、不建后端）。
    if (alias_on()) {
        int a_adopted = 0;
        struct entry *ae = alias_try_fg(sha, &id, nst, &a_adopted);
        if (a_adopted) nvk_prog_warm_stat_draw_hit();   // 合并到的正是预热带建的那份：第一次被真实 draw 采用
        if (ae) {
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
    if (w > 0) {
        if (win->failed) return -1;
        *out = &win->prog;   // 预建对象的采用计数在 await_inflight 的锁内记（每份一次）
        // 同一处的 c1 记账（原因见 prog_note_use）：预建对象里有 c1 就记进清单。
        { struct nvk_prog_rec rec; memset(&rec, 0, sizeof rec); memcpy(rec.identity, sha, 32); rec.stage_mask = stages; rec.lp_ok = libpack_state_ok(&f.gfx) ? 2 : 0;
          for (int st = 0; st < nst; st++) memcpy(rec.keys[st], id.keys[st], 8);
          rec.c1_ok = (uint8_t)(win->prog.c1_ok & (uint8_t)((1u << nst) - 1u));
          for (int st = 0; st < nst; st++)
              if (rec.c1_ok & (1u << st)) memcpy(rec.c1[st], win->prog.c1[st], NVK_PROG_REC_C1_BYTES);
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
                if (win && !win->failed) { *out = &win->prog; return 0; }
                return -1;                           // 对方给的是失败项：与本线程自己失败同义
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
    rec.stage_mask = stages; rec.lp_ok = libpack_state_ok(&f.gfx) ? 2 : 0;
    for (int st = 0; st < nst; st++) memcpy(rec.keys[st], id.keys[st], 8);
    struct entry *ne = NULL;
    const uint64_t dg_c0 = atomic_load(&g_n_compiled);
    struct work_snap *snap = snap_capture(&f, &id, sha, &pc, stages, nst);
    g_ubo_learned = 0;
    if (!snap || !(ne = work_run(snap, 0))) {
        snap_free(snap);
        if (claimed) inflight_abort(sha);
        return -1;
    }
    idg_work(group, atomic_load(&g_n_compiled) > dg_c0);   // 身份粒度诊断：这份身份是现场编出来的还是读盘/离线库拿到的
    idg_log();
    // 快照里的 c1 逐字节抄进记录：预热产品必须与这次真实解析的程序逐位一致（rp_draw 对 c1 不齐直接跳过绘制）。
    for (int st = 0; st < nst; st++)
        if (snap->views[st].c1 && snap->views[st].c1_bytes >= NVK_PROG_REC_C1_BYTES) {
            rec.c1_ok |= (uint8_t)(1u << st);
            memcpy(rec.c1[st], snap->views[st].c1, NVK_PROG_REC_C1_BYTES);
        }
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
    nvk_prog_warm_note(&rec);   // 真实 resolve 成功：记身份 + uses（含 c1）
    prog_tally_log();
    return 0;
}

// ---------- 计算程序 ----------
// 输入的构造与身份用旧层留下的零件（nvn_vk_compute_request.c：只做输入，不碰设备），结果解析用 nvn_vk_program_parse_compute。
// 存储缓冲三种来源：NVN 存储槽（SBUFP）、别名某个用户 UBO 槽的本体（map 2 / UBOP）、
// 用户 UBO 里 8 字节裸指针（map 3 / UBOA，带字节偏移）。图像、间接派发：记账、取不到。
static struct entry *build_compute(const uint8_t *bytes, size_t len, const struct s2a_source_entry_view *src,
                                   const uint8_t identity[32]) {
    struct entry *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->prog.is_compute = 1;
    memcpy(e->prog.identity, identity, 32);
    static _Thread_local struct nvn_vk_compute_program cp; char why[192] = "";
    memset(&cp, 0, sizeof cp);
    int parsed = nvn_vk_program_parse_compute(bytes, len, &cp, why, sizeof why);
    if (parsed) {
        if (parsed == NVK_PROGRAM_ARRAY_UNSUPPORTED) array_unsupported(why); else red1("compute_program_parse_failed", why);
        e->failed = 1; return e;
    }
    e->prog.support_bytes = cp.support_bytes;
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
        if (!ok) { char nm[56]; snprintf(nm, sizeof nm, "compute_binding_kind_%d_slot_%d_map_%d", b->kind, b->slot, b->kind == 1 ? (int)cp.storage[i].mapping : 0); red1(nm, "cs"); e->failed = 1; break; }
        if (le->cls == NVN_GPU_RES_TBUF && le->src_kind != NVN_RB_TBUF) { red1("texture_buffer_replay_unsupported", "cs"); e->failed = 1; break; }
        le->stages = 1u << NVN_GPU_SH_CS;
        if (b->kind == 2 || b->kind == 3) sampled_decl(cp.stage.spirv, cp.stage.spirv_bytes, (uint32_t)b->set, (uint32_t)b->binding,   // 3 = 存储图像：数值类与声明格式给回放选视图格式
            &e->prog.dtt[0][e->prog.nent[0]], &e->prog.dcls[0][e->prog.nent[0]], &e->prog.difmt[0][e->prog.nent[0]]);
        if (le->cls == NVN_GPU_RES_TBUF && e->prog.dtt[0][e->prog.nent[0]] != NVN_TEXTURE_TARGET_BUFFER + 1) {
            red1("tbuf_shader_type_unproven", "cs"); e->failed = 1; break;
        }
        e->prog.nent[0]++;
    }
    if (!e->failed && !cp.stage.present) { red1("stage_missing_in_result", "cs"); e->failed = 1; }
    if (!e->failed) {
        nvn_gpu_layout L = {0}; L.ent = e->prog.ent[0]; L.nent = e->prog.nent[0]; L.support_bytes = cp.support_bytes; L.id_src = "s2a-direct-program-v1";
        nvn_gpu_shader_src ss = {0}; ss.stage = NVN_GPU_SH_CS; ss.kind = NVN_GPU_BLOB_SPIRV; ss.blob = cp.stage.spirv; ss.bytes = cp.stage.spirv_bytes; ss.entry = "main"; ss.layout = &L;
        nvn_gpu_err err = {0};
        nvn_gpu_rc lr = nvn_gpu_shader_load(&ss, &e->prog.sh[0], &err);
        if (!nvn_gpu_ok(lr)) { red_load(lr, &ss, "compute_shader_load_failed", err.why); e->failed = 1; }
        if (src->c1 && src->c1_bytes >= 0x100) { memcpy(e->prog.c1[0], src->c1, 0x100); e->prog.c1_ok |= 1; }
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
    rec.c1_ok = p->c1_ok & 1;
    if (rec.c1_ok) memcpy(rec.c1[0], p->c1[0], NVK_PROG_REC_C1_BYTES);
    int unaligned = 0;
    for (int i = 0; i < 16; i++)
        if (rp_S.ssbo[5][i].addr && pc->storage_buffer_offset_alignment &&
            rp_S.ssbo[5][i].addr % pc->storage_buffer_offset_alignment) unaligned = 1;
    rec.lp_ok = unaligned ? 0 : 2;
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
    static struct s2a_compile_texture_src tex[168]; static char tfmt[168][S2A_TEX_FORMAT_MAX], tsmp[168][S2A_TEX_SAMPLER_MAX]; int ntex = 0;
    for (int slot = 0; slot < 32; slot++) {
        uint64_t h = rp_S.tex[5][slot]; long id = h ? (long)(h & 0xFFFFF) : (long)rp_S.tex_base;
        tex_t t; view_t tv; int has_tv = 0; struct s2a_nvn_texture_fact nf; char why[96];
        if (rp_tex_snap_by_id((int)id, &t, &tv, &has_tv)) {
            if (request_texture_fact((uint32_t)(has_tv && tv.set_format ? tv.format : t.d.format), (uint32_t)(has_tv && tv.set_target ? tv.target : t.d.target), t.d.depth, &nf, why, sizeof why)) continue;
        } else {
            int in_pool = id >= rp_S.tex_base && (uint64_t)(id - rp_S.tex_base) < (uint64_t)rp_S.tex_pool_n;
            memset(&nf, 0, sizeof nf); snprintf(nf.format, sizeof nf.format, "Unknown");
            snprintf(nf.sampler_type, sizeof nf.sampler_type, "%s", in_pool ? "1d" : "2d"); nf.normalized = !in_pool; nf.array_length = 1;
        }
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
    struct nvk_compute_texture_facts tf = {tex, ntex, 1};
    int unaligned = 0;
    for (int i = 0; i < 16; i++) if (rp_S.ssbo[5][i].addr && pc.storage_buffer_offset_alignment && rp_S.ssbo[5][i].addr % pc.storage_buffer_offset_alignment) unaligned = 1;
    char ident[24]; { int o = 0; for (int b = 0; b < 8; b++) o += snprintf(ident + o, sizeof ident - (size_t)o, "%02x", rp_S.skey[5][b]); }
    static struct nvk_compute_request rq; memset(&rq, 0, sizeof rq); char why[256] = "";
    if (nvk_compute_request_prepare(&st, &pc, &tf, 1, unaligned, ident, &rq, why, sizeof why)) {
        red1("compute_request_failed", why);
        if (g_texpool_diag) {
            g_pool_request_rejected++;
            if (!strcmp(why, "missing or out-of-range descriptor-pool facts")) g_pool_request_facts++;
            else g_pool_request_other++;
            if (g_pool_request_rejected == 1 || !(g_pool_request_rejected % 50)) pool_stats("progress");
        }
        return -1;
    }
    pthread_mutex_lock(&g_progs_mu);
    struct entry *e = progs_get_locked(rq.identity);
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
        if (!bytes && libpack_fetch(rq.identity, NULL, 0, rp_S.skey[5], !unaligned, &pc, &bytes, &len)) {
            lpe = build_compute(bytes, len, &rq.source, rq.identity);
            free(bytes); bytes = NULL; len = 0;
            if (lpe && !lpe->failed) libpack_note_hit();
            else { entry_free(lpe); lpe = NULL; libpack_mark_bad(rq.identity); }
        }
        if (!bytes && !lpe) {
            struct s2a_compile_outcome oc; memset(&oc, 0, sizeof oc);
            if (g_texpool_diag) g_pool_attempts++;
            if (!shader_compile_admit()) { nvk_compute_request_release(&rq); nvn_unimpl("shader", "compile_stopped", NULL); return -1; }
            int ran = s2a_compile_request_execute(rq.session, rq.generation, rq.json, rq.json_bytes, &oc);
            if (ran || !oc.bytes || !oc.bytes_len) {
                atomic_fetch_add_explicit(&g_n_failed, 1, memory_order_relaxed);
                red1("compute_compile_failed", oc.summary); compiler_rejected(oc.summary);
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
                return -1;
            }
            bytes = oc.bytes; len = oc.bytes_len; oc.bytes = NULL; s2a_compile_outcome_free(&oc);
            atomic_fetch_add_explicit(&g_n_compiled, 1, memory_order_relaxed);
            if (g_texpool_diag) { g_pool_compiled++; if (g_pool_attempts == 1 || !(g_pool_attempts % 50)) pool_stats("progress"); }
            if (cdir && !nvn_plat_flag("no_disk_cache", 0)) s2a_program_cache_store_async(cdir, rq.identity, bytes, len);
        }
        if (lpe) e = lpe;
        else { e = build_compute(bytes, len, &rq.source, rq.identity); free(bytes); }
        pthread_mutex_lock(&g_progs_mu);
        struct entry *have = progs_get_locked(rq.identity);
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
    if (shader_enter()) { *out = NULL; nvn_unimpl("shader", "compile_stopped", NULL); return -1; }
    int rc = shader_resolve_graphics(q, out); shader_leave(); return rc;
}
int rp_shader_resolve_compute(const struct rp_prog **out) {
    if (shader_enter()) { *out = NULL; nvn_unimpl("shader", "compile_stopped", NULL); return -1; }
    int rc = shader_resolve_compute(out); shader_leave(); return rc;
}

void rp_shader_reset(void) {
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
    memset(g_ubo_need, 0, sizeof g_ubo_need); g_ubo_need_n = 0; g_ubo_learned = 0;
    memset(g_idg, 0, sizeof g_idg); memset(g_idg_set, 0, sizeof g_idg_set);
    g_idg_groups = g_idg_idents = g_idg_resolves = g_idg_compiles = g_idg_loads = g_idg_multi = 0; g_idg_last_ns = 0;
    g_cs_hits = g_cs_misses = g_cs_hit_ns = g_cs_miss_ns = 0; g_mark_cs_hits = g_mark_cs_misses = g_mark_cs_hit_ns = g_mark_cs_miss_ns = 0;
    g_cache_calls = g_cache_hits = g_cache_misses = g_cache_hit_ns = g_cache_miss_ns = 0; g_sig_recomp = 0;
    g_mark_calls = g_mark_hits = g_mark_misses = g_mark_hit_ns = g_mark_miss_ns = g_mark_sig = 0;
    g_texpool_logged = g_texpool_last_over_limit = g_texpool_diag = 0;
    g_pool_programs = g_pool_attempts = g_pool_compiled = g_pool_rejected = g_pool_unsupported = 0;
    g_pool_request_rejected = g_pool_request_facts = g_pool_request_other = 0;
    g_pool_reason_count = 0; memset(g_pool_reasons, 0, sizeof g_pool_reasons);
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
// 真到现场编译一律留给提交线程的真实 draw（取不到就照旧同步做，不跳过绘制）。
int nvk_prog_warm_build(const struct nvk_prog_rec *rec) {
    if (!rec) return -1;
    int nst = stage_nst(rec->stage_mask);
    if (!nst) { warm_diag("程序预热：阶段掩码 %#x 不是 VS/FS/GS 组合，整条跳过", (unsigned)rec->stage_mask); nvk_prog_warm_stat_build(-1); return -1; }
    if (shader_stopped()) { warm_diag("程序预热：会话已置停局位，预热停止发布"); nvk_prog_warm_stat_build(-1); return -1; }   // 发布前校验：停局后不再建对象
    if (rec->stage_mask != 32u && alias_on()) {   // 结果合并目前只覆盖图形程序
        struct alias_key ak; memset(&ak, 0, sizeof ak);
        ak.nst = nst; ak.c1_ok = (uint8_t)(rec->c1_ok & (uint8_t)((1u << nst) - 1u));
        for (int st = 0; st < nst; st++) ak.c1[st] = rec->c1[st];
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
    if (!e) {   // 缓存/离线库里没有这份身份的结果字节：正常分支（不是 bug），真实 draw 到时同步编
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
