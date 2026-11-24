// nvn_plat.h 的安卓实现：日志进 logcat，开关读 debug.s2a.<名字>（只在调试入口打开时认），目录由会话给。
#include <stdatomic.h>
#include <pthread.h>
#include "nvn_plat.h"
#include <android/log.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>
#include "debug_gate.h"
#include "flag_cache.h"

const char *nvk_session_cache_dir(void);

void nvn_plat_log(int level, const char *cls, const char *text) {
    int prio = level == NVN_PLAT_LOG_RED ? ANDROID_LOG_ERROR : level == NVN_PLAT_LOG_WARN ? ANDROID_LOG_WARN : ANDROID_LOG_INFO;
    char identity[96] = "";
#if defined(__ANDROID__)
    if (text && (!strncmp(text, "S2A_HITCH_PROGRAM ", sizeof "S2A_HITCH_PROGRAM " - 1) ||
                 !strncmp(text, "S2A_HITCH_REPLAY ", sizeof "S2A_HITCH_REPLAY " - 1))) {
        // 与本局 host.log 首行和帧日志的 UUID 同源。取证端用已验证的本局 APK 摘要与它们联结。
        char uuid[PROP_VALUE_MAX] = "";
        if (__system_property_get("debug.s2a.session_uuid", uuid) != 32 ||
            strspn(uuid, "0123456789abcdef") != 32) strcpy(uuid, "-");
        snprintf(identity, sizeof identity, " uuid=%.32s pid=%ld tid=%ld", uuid, (long)getpid(), (long)gettid());
    }
#endif
    __android_log_print(prio, "S2Arm", "%snvk %s: %s%s", level == NVN_PLAT_LOG_RED ? "【红】" : "", cls ? cls : "", text ? text : "", identity);
    // 调试入口开着时同一行也写进本局的宿主日志文件（标准错误已接到 host-*.log）。系统日志在真机上会整段丢掉某个进程的行
    // （一晚上遇到五次「游戏在跑、logcat 里这个进程 0 行」，宿主日志文件完好），读数不能只押在它上面。
    if (s2a_debug_entries_open()) fprintf(stderr, "[s2a] %snvk %s: %s%s\n", level == NVN_PLAT_LOG_RED ? "【红】" : "", cls ? cls : "", text ? text : "", identity);
}

#if defined(S2A_ANDROID_DEBUG_ENV) && defined(__ANDROID__)   // 与 debug_gate.h 的第一道门同一个条件：玩家包里 s2a_debug_entries_open() 编译期恒假，下面整段不存在
// 调试包：每个线程缓存开关读数 FLAG_TTL_NS（见 flag_cache.h：回放热路径上每次调用读两次系统属性 + 拼键名，占回放线程样本的 6–12%）；
// 过期重读，adb setprop 改开关最迟一个 TTL 后生效。
enum { FLAG_TTL_NS = 250 * 1000 * 1000 };
static int plat_flag_read(const char *name, int *val, void *user) {
    (void)user;
    if (!s2a_debug_entries_open()) return 0;
    char key[PROP_NAME_MAX + 32], v[PROP_VALUE_MAX] = {0};
    snprintf(key, sizeof key, "debug.s2a.%s", name);
    if (__system_property_get(key, v) <= 0 || !v[0]) return 0;
    *val = atoi(v);
    return 1;
}
int nvn_plat_flag(const char *name, int dflt) {
    static __thread struct fc_tab tab;
    if (!name) return dflt;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return fc_get(&tab, name, dflt, (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec, FLAG_TTL_NS, plat_flag_read, NULL);
}
#else
int nvn_plat_flag(const char *name, int dflt) { (void)name; return dflt; }   // 玩家包：开关一律取缺省（s2a_debug_entries_open() 编译期恒假）
#endif

// 回放循环的安全点（nvn_plat.h）：运行时改内存映射要所有游戏线程停下，回放所在的游戏线程一次提交可连续跑数秒，
// 在命令之间让它停到事务结束（android/native/runtime/fault/thread_android.inc）。诊断目标不链运行时：弱符号。
extern void s2a_android_guest_host_safepoint(void) __attribute__((weak));
void nvn_plat_safepoint(void) { if (s2a_android_guest_host_safepoint) s2a_android_guest_host_safepoint(); }

const char *nvn_plat_dir(int which) {
    if (which == NVN_PLAT_DIR_CACHE) return nvk_session_cache_dir();
    if (which == NVN_PLAT_DIR_DIAG) return getenv("S2A_DIRECT_DIAG_DIR");
    return NULL;
}

// 前端还直接按名字要的两个宿主入口（hle_nvn.c / nvn2.c）
const char *nvn_diag_path(const char *p, char *buf, size_t n) { (void)buf; (void)n; return p; }   // 诊断输出路径：原样返回，同非 iOS 的苹果宿主
int dm_opmode(void) { const char *d = getenv("S2A_DOCKED"); return d && d[0] == '1' && !d[1]; }   // 主机 / 掌机档：S2A_DOCKED=1 主机，其余掌机

// ---------- 每款参数 ----------
// 共用前端按 DM_GAME_* 读每款参数（graphics/nvn/host/game_profile.h）。没有随包档 / 没写的键按 Switch 真实语义的缺省
// （game_tsv.h s2a_game_tsv_defaults：条件渲染照实求值、CPU 写进池的贴图要上传、动态句柄着色器、段内 GPU 时间戳都开）。
// 这里是强定义；vk_engine_host_shim.c 的弱桩是全 0，等于把这些全关掉——贴图在首次使用之后才被游戏填进内容的那些永远传不上来
// （块压缩贴图停在全 0：ASTC 解成错误色洋红，DXT 解成全透明）。随包 game.tsv 由当前已激活 revision 提供。
#include "../../../graphics/nvn/host/game_profile.h"

// ---------- 调试：进程内取样剖析（开关 debug.s2a.nvk_prof=1；默认关） ----------
// 帧时间的大头在图形调用之外时，要知道 CPU 花在哪个模块。系统的剖析器要先放开一个系统属性，这里不动系统设置：
// 按进程 CPU 时间每毫秒来一个 SIGPROF，落在谁身上就记谁当时的指令地址（处理函数里只写一个环，不加锁、不分配）；
// 另有一条线程每 5 秒把这段时间的样本按模块、按 4 KB 页归拢，各打一行。只回答「CPU 时间花在哪」，等待不算在内。
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
// 开关值 = 2 时另外做「精确 pc 直方图」：每个样本再记线程号，全程累计（线程, pc）→ 次数，每 5 秒把前 8000 项整份重写到
// 缓存目录 nvkprof_pcs.txt（线程号 线程名 模块 偏移 次数）。页粒度只能说「哪一页最热」，精确 pc 配上带符号 so 的 addr2line
// 才能落到函数 / 内联展开 / 行，并且能把回放线程和别的线程分开。只在值 ≥ 2 时多做事；缺省关。
// 开关值 ≥ 3 时再记「调用方」：落在 libc / 驱动 / 内存拷贝里的样本只能说「时间花在 memmove」，要知道是游戏代码的哪一处调的。
// 每个样本另存 LR 和帧指针链上的前 3 个返回地址；5 秒一次归拢时取其中第一个落在主模块（本 so）里的地址，
// 按（线程, pc, 调用方）累计，前 8000 项整份重写到缓存目录 nvkprof_callers.txt（线程号 线程名 pc 模块 pc 偏移 调用方偏移 次数）。
// 帧指针链上的内存读取不能直接解引用（驱动里的优化代码把 x29 当通用寄存器用，链会指到任意地址）：用 write() 往管道里写这 16 字节，
// 坏地址只返回 EFAULT、不会崩；回放线程以外的信号并发进来时拿不到探针（忙），那个样本只记 LR。
enum { PROF_RING = 1 << 16, PCH_N = 1 << 17, PCH_TOP = 8000, PROF_CH = 3 };
static uintptr_t g_prof_ring[PROF_RING]; static uint32_t g_prof_tid[PROF_RING]; static _Atomic uint64_t g_prof_w;
static uintptr_t g_prof_lr[PROF_RING], g_prof_ch[PROF_RING][PROF_CH];
static int g_prof_mode, g_prof_pipe[2] = {-1, -1};
#if defined(__aarch64__) && !defined(__APPLE__)   // 只有下面信号处理函数的 aarch64 分支用到；宿主单测（x86 / 苹果芯片）也编本文件，不留未用的静态定义（-Werror）
static atomic_flag g_prof_busy = ATOMIC_FLAG_INIT;
static int prof_peek16(uintptr_t addr, uintptr_t out[2]) {   // 读 addr 处 16 字节（前一个是上一帧的帧指针，后一个是返回地址）
    if ((addr & 7) || addr < 0x10000) return 0;
    if (write(g_prof_pipe[1], (const void *)addr, 16) != 16) return 0;
    return read(g_prof_pipe[0], out, 16) == 16;
}
#endif
static void prof_sig(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si;
    uintptr_t lr = 0, ch[PROF_CH] = {0, 0, 0};
#if defined(__aarch64__) && !defined(__APPLE__)   // 苹果芯片的宿主上也会编这个文件（宿主单测）：那边的信号上下文是另一种布局，取不到就记 0
    const mcontext_t *mc = &((ucontext_t *)uc)->uc_mcontext;
    uintptr_t pc = (uintptr_t)mc->pc; uint32_t tid = (uint32_t)gettid();
    if (g_prof_mode >= 3) {
        lr = (uintptr_t)mc->regs[30];
        if (g_prof_pipe[1] >= 0 && !atomic_flag_test_and_set_explicit(&g_prof_busy, memory_order_acquire)) {
            const int se = errno; uintptr_t fp = (uintptr_t)mc->regs[29], sp = (uintptr_t)mc->sp, fr[2];
            for (int k = 0; k < PROF_CH && fp >= sp && fp - sp < ((uintptr_t)8 << 20); k++) {
                if (!prof_peek16(fp, fr)) break;
                ch[k] = fr[1];
                if (fr[0] <= fp) break;
                fp = fr[0];
            }
            errno = se; atomic_flag_clear_explicit(&g_prof_busy, memory_order_release);
        }
    }
#else
    uintptr_t pc = 0; uint32_t tid = 0; (void)uc;
#endif
    const uint64_t slot = atomic_fetch_add_explicit(&g_prof_w, 1, memory_order_relaxed) & (PROF_RING - 1);
    g_prof_ring[slot] = pc; g_prof_tid[slot] = tid;
    g_prof_lr[slot] = lr; for (int k = 0; k < PROF_CH; k++) g_prof_ch[slot][k] = ch[k];
}
static struct pch_ent { uintptr_t pc; uint32_t tid, n; } g_pch[PCH_N];
static size_t g_pch_total, g_pch_lost;
static void pch_add(uintptr_t pc, uint32_t tid) {
    g_pch_total++;
    const size_t h = (size_t)(((uint64_t)pc * 0x9E3779B97F4A7C15ull ^ ((uint64_t)tid << 32)) >> 20) & (PCH_N - 1);
    for (size_t k = 0; k < 64; k++) {
        struct pch_ent *e = &g_pch[(h + k) & (PCH_N - 1)];
        if (!e->n) { e->pc = pc; e->tid = tid; e->n = 1; return; }
        if (e->pc == pc && e->tid == tid) { e->n++; return; }
    }
    g_pch_lost++;
}
static int pch_cmp(const void *a, const void *b) { uint32_t x = ((const struct pch_ent *)a)->n, y = ((const struct pch_ent *)b)->n; return x < y ? 1 : x > y ? -1 : 0; }
static void pch_dump(void) {
    const char *dir = nvn_plat_dir(NVN_PLAT_DIR_CACHE); if (!dir) return;
    static struct pch_ent *tmp; if (!tmp) tmp = (struct pch_ent *)malloc(sizeof g_pch); if (!tmp) return;
    size_t m = 0; for (size_t i = 0; i < PCH_N; i++) if (g_pch[i].n) tmp[m++] = g_pch[i];
    qsort(tmp, m, sizeof *tmp, pch_cmp);
    char pth[320], pt2[336]; snprintf(pth, sizeof pth, "%s/nvkprof_pcs.txt", dir); snprintf(pt2, sizeof pt2, "%s.tmp", pth);
    FILE *f = fopen(pt2, "w"); if (!f) return;
    fprintf(f, "# total=%zu unique=%zu lost=%zu\n", g_pch_total, m, g_pch_lost);
    struct { uint32_t tid; char name[24]; } tn[64]; int ntn = 0;
    for (size_t i = 0; i < m && i < PCH_TOP; i++) {
        int k = 0; for (; k < ntn; k++) if (tn[k].tid == tmp[i].tid) break;
        if (k == ntn && ntn < 64) {
            char p[64]; snprintf(p, sizeof p, "/proc/self/task/%u/comm", tmp[i].tid); tn[ntn].tid = tmp[i].tid; tn[ntn].name[0] = 0;
            FILE *c = fopen(p, "r"); if (c) { if (fgets(tn[ntn].name, sizeof tn[ntn].name, c)) { size_t L = strlen(tn[ntn].name); if (L && tn[ntn].name[L - 1] == '\n') tn[ntn].name[L - 1] = 0; } fclose(c); }
            ntn++;
        }
        const char *tname = k < ntn && tn[k].name[0] ? tn[k].name : "?";
        const char *mod = "游戏"; unsigned long off = (unsigned long)tmp[i].pc; Dl_info di;
        if (dladdr((void *)tmp[i].pc, &di) && di.dli_fname) { const char *sl = strrchr(di.dli_fname, '/'); mod = sl ? sl + 1 : di.dli_fname; off = (unsigned long)(tmp[i].pc - (uintptr_t)di.dli_fbase); }
        fprintf(f, "%u\t%s\t%s\t%#lx\t%u\n", tmp[i].tid, tname, mod, off, tmp[i].n);
    }
    fclose(f); rename(pt2, pth);
}
// ---- nvk_prof ≥ 3：调用方直方图（说明见上面的枚举处）----
static uintptr_t g_main_lo, g_main_hi;   // 本 so 的装载范围，开剖析时算一次
static int main_range_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; const uintptr_t base = *(const uintptr_t *)data;
    if ((uintptr_t)info->dlpi_addr != base) return 0;
    uintptr_t hi = base;
    for (int i = 0; i < info->dlpi_phnum; i++) if (info->dlpi_phdr[i].p_type == PT_LOAD) { uintptr_t e = base + (uintptr_t)info->dlpi_phdr[i].p_vaddr + (uintptr_t)info->dlpi_phdr[i].p_memsz; if (e > hi) hi = e; }
    g_main_hi = hi; return 1;
}
static void main_range_init(void) {
    Dl_info di; if (!dladdr((void *)&main_range_cb, &di) || !di.dli_fbase) return;
    uintptr_t base = (uintptr_t)di.dli_fbase; g_main_lo = base; dl_iterate_phdr(main_range_cb, &base);
}
static uintptr_t prof_pick_caller(uintptr_t pc, uintptr_t lr, const uintptr_t *ch) {   // pc 不在本 so 里时，LR / 帧链上第一个落在本 so 里的返回地址
    if (pc >= g_main_lo && pc < g_main_hi) return 0;
    if (lr >= g_main_lo && lr < g_main_hi) return lr;
    for (int k = 0; k < PROF_CH; k++) if (ch[k] >= g_main_lo && ch[k] < g_main_hi) return ch[k];
    return 0;
}
static struct cch_ent { uintptr_t pc, caller; uint32_t tid, n; } g_cch[PCH_N];
static size_t g_cch_total, g_cch_lost;
static void cch_add(uintptr_t pc, uintptr_t caller, uint32_t tid) {
    g_cch_total++;
    const size_t h = (size_t)(((uint64_t)pc * 0x9E3779B97F4A7C15ull ^ ((uint64_t)caller * 0xC2B2AE3D27D4EB4Full) ^ ((uint64_t)tid << 32)) >> 20) & (PCH_N - 1);
    for (size_t k = 0; k < 64; k++) {
        struct cch_ent *e = &g_cch[(h + k) & (PCH_N - 1)];
        if (!e->n) { e->pc = pc; e->caller = caller; e->tid = tid; e->n = 1; return; }
        if (e->pc == pc && e->caller == caller && e->tid == tid) { e->n++; return; }
    }
    g_cch_lost++;
}
static int cch_cmp(const void *a, const void *b) { uint32_t x = ((const struct cch_ent *)a)->n, y = ((const struct cch_ent *)b)->n; return x < y ? 1 : x > y ? -1 : 0; }
static void prof_tname(uint32_t tid, char *out, size_t cap) {
    char p[64]; snprintf(p, sizeof p, "/proc/self/task/%u/comm", tid); out[0] = 0;
    FILE *c = fopen(p, "r"); if (!c) { snprintf(out, cap, "?"); return; }
    if (fgets(out, (int)cap, c)) { size_t L = strlen(out); if (L && out[L - 1] == '\n') out[L - 1] = 0; }
    fclose(c); if (!out[0]) snprintf(out, cap, "?");
}
static void cch_dump(void) {
    const char *dir = nvn_plat_dir(NVN_PLAT_DIR_CACHE); if (!dir) return;
    static struct cch_ent *tmp; if (!tmp) tmp = (struct cch_ent *)malloc(sizeof g_cch); if (!tmp) return;
    size_t m = 0; for (size_t i = 0; i < PCH_N; i++) if (g_cch[i].n) tmp[m++] = g_cch[i];
    qsort(tmp, m, sizeof *tmp, cch_cmp);
    char pth[320], pt2[336]; snprintf(pth, sizeof pth, "%s/nvkprof_callers.txt", dir); snprintf(pt2, sizeof pt2, "%s.tmp", pth);
    FILE *f = fopen(pt2, "w"); if (!f) return;
    fprintf(f, "# total=%zu unique=%zu lost=%zu main=%#lx-%#lx\n", g_cch_total, m, g_cch_lost, (unsigned long)g_main_lo, (unsigned long)g_main_hi);
    struct { uint32_t tid; char name[24]; } tn[64]; int ntn = 0;
    for (size_t i = 0; i < m && i < PCH_TOP; i++) {
        int k = 0; for (; k < ntn; k++) if (tn[k].tid == tmp[i].tid) break;
        if (k == ntn && ntn < 64) { tn[ntn].tid = tmp[i].tid; prof_tname(tmp[i].tid, tn[ntn].name, sizeof tn[ntn].name); ntn++; }
        const char *tname = k < ntn ? tn[k].name : "?";
        const char *mod = "游戏"; unsigned long off = (unsigned long)tmp[i].pc; Dl_info di;
        if (dladdr((void *)tmp[i].pc, &di) && di.dli_fname) { const char *sl = strrchr(di.dli_fname, '/'); mod = sl ? sl + 1 : di.dli_fname; off = (unsigned long)(tmp[i].pc - (uintptr_t)di.dli_fbase); }
        fprintf(f, "%u\t%s\t%s\t%#lx\t%#lx\t%u\n", tmp[i].tid, tname, mod, off, tmp[i].caller ? (unsigned long)(tmp[i].caller - g_main_lo) : 0ul, tmp[i].n);
    }
    fclose(f); rename(pt2, pth);
}
// 线程调度账（nvk_prof ≥ 1，每 5 秒一行）：各线程这 5 秒里「在核上跑」「排队等核」各占墙钟多少（其余 = 睡眠 / 阻塞在锁和 GPU 上），
// 以及自愿切换（阻塞）/ 被抢占的次数。CPU 采样只说「花在哪」，看不见等待；回放线程是在跑、在等核、还是在等别人，看这一行。
// 读 /proc/self/task/<tid>/schedstat（运行纳秒 / 排队纳秒 / 时间片数）和 status（名字、切换次数）；内核不给就静默。
static void prof_sched_report(void) {
    enum { MAXT = 512 };
    struct thr { uint32_t tid; uint64_t run, wait, sl; long vol, invol; char name[20]; };
    static struct thr prev[MAXT], cur[MAXT]; static int np; static uint64_t prev_ns;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); const uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    DIR *d = opendir("/proc/self/task"); if (!d) return;
    int nc = 0; struct dirent *de;
    while ((de = readdir(d)) && nc < MAXT) {
        const uint32_t tid = (uint32_t)atoi(de->d_name); if (!tid) continue;
        char p[64]; unsigned long long a = 0, b = 0, c = 0; snprintf(p, sizeof p, "/proc/self/task/%u/schedstat", tid);
        FILE *f = fopen(p, "r"); if (!f) continue;
        const int got = fscanf(f, "%llu %llu %llu", &a, &b, &c); fclose(f); if (got != 3) continue;
        struct thr *t = &cur[nc]; t->tid = tid; t->run = a; t->wait = b; t->sl = c; t->vol = t->invol = -1; t->name[0] = 0;
        snprintf(p, sizeof p, "/proc/self/task/%u/status", tid); f = fopen(p, "r");
        if (f) { char ln[160]; while (fgets(ln, sizeof ln, f)) {
                if (!strncmp(ln, "Name:", 5)) (void)sscanf(ln + 5, "%19s", t->name);
                else if (!strncmp(ln, "voluntary_ctxt_switches:", 24)) t->vol = atol(ln + 24);
                else if (!strncmp(ln, "nonvoluntary_ctxt_switches:", 27)) t->invol = atol(ln + 27); }
            fclose(f); }
        nc++;
    }
    closedir(d);
    const double wall = prev_ns ? (double)(now - prev_ns) : 0.0;
    if (wall > 0) {
        struct { int i; double run; } order[MAXT]; int no = 0;
        for (int i = 0; i < nc; i++) { for (int k = 0; k < np; k++) if (prev[k].tid == cur[i].tid) { order[no].i = i; order[no].run = (double)(cur[i].run - prev[k].run); no++; break; } }
        for (int i = 0; i < no && i < 8; i++) { int bi = i; for (int j = i + 1; j < no; j++) if (order[j].run > order[bi].run) bi = j; if (bi != i) { __typeof__(order[0]) sw_ = order[i]; order[i] = order[bi]; order[bi] = sw_; } }
        char line[900]; int o = snprintf(line, sizeof line, "线程调度（近 %.1f 秒墙钟，前 8 个线程）：", wall / 1e9);
        for (int r = 0; r < no && r < 8 && o < (int)sizeof line - 120; r++) {
            const struct thr *t = &cur[order[r].i], *q = NULL; for (int k = 0; k < np; k++) if (prev[k].tid == t->tid) { q = &prev[k]; break; }
            if (!q) continue;
            o += snprintf(line + o, sizeof line - (size_t)o, "%s%u %s 在核上 %.0f%% 等核 %.1f%% 自愿切换 %.0f/s 被抢占 %.0f/s", r ? "；" : "", t->tid, t->name[0] ? t->name : "?",
                          100.0 * (double)(t->run - q->run) / wall, 100.0 * (double)(t->wait - q->wait) / wall,
                          t->vol >= 0 && q->vol >= 0 ? (double)(t->vol - q->vol) * 1e9 / wall : -1.0, t->invol >= 0 && q->invol >= 0 ? (double)(t->invol - q->invol) * 1e9 / wall : -1.0);
        }
        __android_log_print(ANDROID_LOG_INFO, "S2Arm", "nvk prof: %s", line);
    }
    memcpy(prev, cur, (size_t)nc * sizeof cur[0]); np = nc; prev_ns = now;
}
static int prof_cmp(const void *a, const void *b) { uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b; return x < y ? -1 : x > y; }
static void *prof_report(void *arg) {
    (void)arg; uint64_t r = 0; static uintptr_t buf[PROF_RING], raw[PROF_RING];
    for (;;) {
        sleep(5);
        uint64_t w = atomic_load_explicit(&g_prof_w, memory_order_relaxed); if (w - r > PROF_RING) r = w - PROF_RING;
        size_t n = (size_t)(w - r); if (!n) continue;
        for (size_t i = 0; i < n; i++) { raw[i] = g_prof_ring[(r + i) & (PROF_RING - 1)]; buf[i] = raw[i] & ~(uintptr_t)0xfff; }   // 按 4 KB 页
        if (g_prof_mode >= 2) { for (size_t i = 0; i < n; i++) pch_add(raw[i], g_prof_tid[(r + i) & (PROF_RING - 1)]); pch_dump(); }
        if (g_prof_mode >= 3) { for (size_t i = 0; i < n; i++) { const size_t s = (size_t)((r + i) & (PROF_RING - 1)); cch_add(raw[i], prof_pick_caller(raw[i], g_prof_lr[s], g_prof_ch[s]), g_prof_tid[s]); } cch_dump(); }
        prof_sched_report();
        r = w; qsort(buf, n, sizeof *buf, prof_cmp);
        struct { uintptr_t page; size_t n; } top[12]; int nt = 0;
        struct { char name[40]; size_t n; } mod[16]; int nm = 0;
        for (size_t i = 0; i < n;) { size_t j = i; while (j < n && buf[j] == buf[i]) j++;
            size_t cnt = j - i; Dl_info di; const char *nm0 = "游戏代码";
            if (dladdr((void *)buf[i], &di) && di.dli_fname) { const char *sl = strrchr(di.dli_fname, '/'); nm0 = sl ? sl + 1 : di.dli_fname; }
            int k = 0; for (; k < nm; k++) if (!strncmp(mod[k].name, nm0, sizeof mod[k].name - 1)) break;
            if (k == nm && nm < 16) { snprintf(mod[nm].name, sizeof mod[nm].name, "%s", nm0); mod[nm].n = 0; nm++; }
            if (k < nm) mod[k].n += cnt;
            int at = nt < 12 ? nt : -1; if (at < 0) { int mi = 0; for (int q = 1; q < 12; q++) if (top[q].n < top[mi].n) mi = q; if (top[mi].n < cnt) at = mi; } else nt++;
            if (at >= 0) { top[at].page = buf[i]; top[at].n = cnt; }
            i = j; }
        char line[900]; int o = snprintf(line, sizeof line, "剖析：5 秒 %zu 个样本（每个 1 毫秒 CPU）；按模块：", n);
        for (int pass = 0; pass < nm && pass < 8; pass++) { int bi = -1; for (int k = 0; k < nm; k++) if (mod[k].n && (bi < 0 || mod[k].n > mod[bi].n)) bi = k; if (bi < 0) break;
            o += snprintf(line + o, sizeof line - (size_t)o, " %s %.0f%%", mod[bi].name, 100.0 * (double)mod[bi].n / (double)n); mod[bi].n = 0; }
        __android_log_print(ANDROID_LOG_INFO, "S2Arm", "nvk prof: %s", line);
        o = snprintf(line, sizeof line, "剖析：最热的页：");
        for (int pass = 0; pass < nt && pass < 10; pass++) { int bi = -1; for (int k = 0; k < nt; k++) if (top[k].n && (bi < 0 || top[k].n > top[bi].n)) bi = k; if (bi < 0) break;
            Dl_info di; int ok = dladdr((void *)top[bi].page, &di) && di.dli_fname; const char *sl = ok ? strrchr(di.dli_fname, '/') : NULL;
            o += snprintf(line + o, sizeof line - (size_t)o, " %s+%#lx %.1f%%;", ok ? (sl ? sl + 1 : di.dli_fname) : "游戏", (unsigned long)(ok ? top[bi].page - (uintptr_t)di.dli_fbase : top[bi].page), 100.0 * (double)top[bi].n / (double)n); top[bi].n = 0; }
        __android_log_print(ANDROID_LOG_INFO, "S2Arm", "nvk prof: %s", line);
        {   // 最热的那一页：它落在进程的哪段映射里（/proc/self/maps 的那一行），页里最热的几个地址
            uintptr_t hot = 0; size_t hn = 0;
            for (size_t i = 0; i < n;) { size_t j = i; while (j < n && buf[j] == buf[i]) j++; if (j - i > hn) { hn = j - i; hot = buf[i]; } i = j; }
            char map[200] = "?"; FILE *f = fopen("/proc/self/maps", "r");
            if (f) { char ln[400]; while (fgets(ln, sizeof ln, f)) { unsigned long a = 0, b = 0; if (sscanf(ln, "%lx-%lx", &a, &b) == 2 && hot >= a && hot < b) { size_t L = strlen(ln); if (L && ln[L - 1] == '\n') ln[L - 1] = 0; snprintf(map, sizeof map, "%s", ln); break; } } fclose(f); }
            size_t m = 0; for (size_t i = 0; i < n; i++) if ((raw[i] & ~(uintptr_t)0xfff) == hot) raw[m++] = raw[i];
            qsort(raw, m, sizeof *raw, prof_cmp);
            struct { uintptr_t pc; size_t n; } tp[6]; int np = 0;
            for (size_t i = 0; i < m;) { size_t j = i; while (j < m && raw[j] == raw[i]) j++;
                int at = np < 6 ? np : -1; if (at < 0) { int mi = 0; for (int q = 1; q < 6; q++) if (tp[q].n < tp[mi].n) mi = q; if (tp[mi].n < j - i) at = mi; } else np++;
                if (at >= 0) { tp[at].pc = raw[i]; tp[at].n = j - i; }
                i = j; }
            o = snprintf(line, sizeof line, "剖析：最热页 %#lx（%zu 个样本）在映射 [%s]；页内：", (unsigned long)hot, hn, map);
            for (int k = 0; k < np; k++) { uint32_t ins = 0; memcpy(&ins, (const void *)tp[k].pc, 4); o += snprintf(line + o, sizeof line - (size_t)o, " +%#lx×%zu(指令 %08x)", (unsigned long)(tp[k].pc - hot), tp[k].n, ins); }
            __android_log_print(ANDROID_LOG_INFO, "S2Arm", "nvk prof: %s", line);
            // 这一页连同前后各一页的原始字节落到缓存目录（同一页只落一次），拿回去反汇编看是哪个函数
            static uintptr_t dumped[8]; static int nd; int seen = 0; for (int k = 0; k < nd; k++) if (dumped[k] == hot) seen = 1;
            const char *dir = nvn_plat_dir(NVN_PLAT_DIR_CACHE);
            if (!seen && nd < 8 && dir && hn * 4 > n) { dumped[nd++] = hot; char pth[320]; snprintf(pth, sizeof pth, "%s/nvkprof_%lx.bin", dir, (unsigned long)(hot - 0x1000));
                FILE *o2 = fopen(pth, "wb"); if (o2) { fwrite((const void *)(hot - 0x1000), 1, 0x3000, o2); fclose(o2); __android_log_print(ANDROID_LOG_INFO, "S2Arm", "nvk prof: 已落盘 %s", pth); } }
        }
    }
    return NULL;
}
static void prof_start_once(void) {
    static _Atomic int started; if (atomic_exchange(&started, 1) || !(g_prof_mode = nvn_plat_flag("nvk_prof", 0))) return;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = prof_sig; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
    if (g_prof_mode >= 3) { main_range_init(); if (pipe(g_prof_pipe)) g_prof_pipe[0] = g_prof_pipe[1] = -1; else for (int k = 0; k < 2; k++) { (void)fcntl(g_prof_pipe[k], F_SETFL, O_NONBLOCK); (void)fcntl(g_prof_pipe[k], F_SETFD, FD_CLOEXEC); } }
    if (sigaction(SIGPROF, &sa, NULL)) return;
    struct itimerval it = {{0, 1000}, {0, 1000}}; setitimer(ITIMER_PROF, &it, NULL);
    pthread_t t; if (!pthread_create(&t, NULL, prof_report, NULL)) pthread_detach(t);
    __android_log_print(ANDROID_LOG_INFO, "S2Arm", "nvk prof: 进程内取样剖析已开（每毫秒 CPU 一个样本）");
}

// 只在整份文件读完后调用共用解析器；读取错误 / 分配失败不能把半份参数当成成功。
static int prof_read(const char *path, s2a_game_tsv *out, char *err, size_t en) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char *text = NULL, chunk[4096]; size_t len = 0, n; int ok = 1;
    while ((n = fread(chunk, 1, sizeof chunk, f)) != 0) {
        if (len > SIZE_MAX - n - 1) { ok = 0; break; }
        char *next = realloc(text, len + n + 1);
        if (!next) { ok = 0; break; }
        text = next; memcpy(text + len, chunk, n); len += n; text[len] = 0;
    }
    if (ferror(f)) ok = 0;
    if (fclose(f)) ok = 0;
    if (ok) s2a_game_tsv_parse(text, out, err, en);
    free(text); return ok;
}

const s2a_nvn_profile *s2a_nvn_prof(void) {
    prof_start_once();
    static s2a_nvn_profile p;
    static char *loaded_root;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    // provider 在激活前设置此根；revision 内容不可变。换局换根必须重读，不能沿用上一款。
    // 与会话激活一样，环境变量只在无运行中游戏时更换；调用方不得跨换局持有此参数指针。
    pthread_mutex_lock(&mu);
    const char *root = getenv("S2A_SHADER_REVISION_ROOT");
    if (!root) root = "";
    if (!loaded_root || strcmp(loaded_root, root)) {
        char *new_root = strdup(root);
        if (!new_root) {
            // 无法缓存身份时不发布可被下一位读者改写的 p；返回只初始化一次的缺省快照。
            static s2a_nvn_profile fallback; static int fallback_ready;
            if (!fallback_ready) { s2a_game_tsv_defaults(&fallback.t); fallback_ready = 1; }
            nvn_plat_log(NVN_PLAT_LOG_WARN, "game.tsv", "无法分配参数路径，全按缺省");
            pthread_mutex_unlock(&mu); return &fallback;
        }
        s2a_game_tsv_defaults(&p.t);
        const char suffix[] = "/shaders/direct/game.tsv";
        size_t len = strlen(root); char err[200] = ""; int got = 0;
        char *path = len <= SIZE_MAX - sizeof suffix ? malloc(len + sizeof suffix) : NULL;
        if (*root && path) {
            memcpy(path, root, len); memcpy(path + len, suffix, sizeof suffix);
            got = prof_read(path, &p.t, err, sizeof err);
        }
        free(path);
        nvn_plat_log(*err ? NVN_PLAT_LOG_RED : got ? NVN_PLAT_LOG_INFO : NVN_PLAT_LOG_WARN,
                     "game.tsv", *err ? err : got ? "已读取随包参数；未写的键按缺省" : "随包参数缺失或无法读取，全按缺省");
        free(loaded_root); loaded_root = new_root;
    }
    pthread_mutex_unlock(&mu);
    return &p;
}
int s2a_nvn_render_cond(void) { return s2a_nvn_prof()->t.render_cond; }   // 回放核心问：条件渲染开没开
