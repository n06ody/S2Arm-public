// Android 致命信号处理器的 fatal 行与收尾（crash.c 的 __ANDROID__ 分支包含；宿主单测直接包含同一份实现）。
// fatal 行：addr / pc / sp、pc 所在模块偏移，再沿 ucontext x29 帧链逐层打调用方返回地址 + 模块偏移
//   （tombstone 常常只展开到 libc，fatal 行这层帧链是看到调用方的唯一线索）。
// 收尾：写完 fatal 行后，旧处置是处理器时在同一信号现场直接链回（套用其 sa_mask / SA_NODEFER），
//   不拆自己的处置——本库各信号处理器全程在位，收尾窗口里其他线程的缺页仍走本库认领。
//   无旧处置可链（缺省 / 忽略 / 未记录）时回落：恢复安装前的处置，同步故障返回重放、异步重投，
//   让系统 debuggerd 照常收 tombstone；不能再用 _exit 抢在系统前面退出。
// 全部是 async-signal-safe 调用（open / read / close / write / sigaction / sigprocmask / raise / _exit
//   / fcntl / process_vm_readv / vm_read_overwrite）；不分配内存。
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/ucontext.h>
#if defined(__APPLE__)
#include <mach/mach.h>          // vm_read_overwrite：帧记录经内核读，读不到只回 0、不二次缺页
#elif defined(__linux__)
#include <sys/syscall.h>
#include <sys/uio.h>            // process_vm_readv，同上
#endif

// ===== 帧链（#1555）与 /proc/self/maps 快照 =====
// maps 快照静态缓冲 = 「预先映射好的缓冲」：安装时（remember 首调）先垫一份，打 fatal 行时
//   再尽力刷新；open 失败沿用上一份，首次即失败 len 为 0 → 行只打 pc/sp，不崩。
//   handler 里照旧只碰这一个文件的一次 open/read（系统调用层、无锁、不开新文件），
//   之后模块解析 / 可读性核查全部变成对这份缓冲的纯内存扫描。
#define S2A_MAPS_CAP (512u * 1024u)   // 超出截断只丢尾部映射的解析，不崩（128 KiB 时安卓大进程常截断到 pc 段之后：无 mod=、链 broken@0）
static char s2a_maps_buf[S2A_MAPS_CAP];
static size_t s2a_maps_len;           // 0 = 拿不到快照：fatal 行退化为 addr / pc / sp
static int s2a_maps_frozen;           // 单测注入快照后置 1，refresh 不再覆盖（生产恒 0）

static size_t s2a_maps_refresh(void) {
    if (s2a_maps_frozen) return s2a_maps_len;
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return s2a_maps_len;
    size_t n = 0;
    while (n < sizeof s2a_maps_buf) {
        ssize_t r = read(fd, s2a_maps_buf + n, sizeof s2a_maps_buf - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    if (n) s2a_maps_len = n;
    return s2a_maps_len;
}

// 一行 maps「lo-hi perms off dev inode path」解析；到 off 为止字段齐才回 1，path 可空。
// fid = dev:inode+inode+path 的原文区间：两个段是否同一文件只做文本比对，不解析数字（#1365 映像合并用）。
typedef struct { uint64_t lo, hi, off; int rd; const char *path; size_t plen;
                 const char *fid; size_t fidlen; } s2a_seg_t;
static uint64_t s2a_hexp(const char **p, const char *e) {
    uint64_t v = 0;
    for (; *p < e && ((**p >= '0' && **p <= '9') || (**p >= 'a' && **p <= 'f')); ++*p)
        v = v * 16 + (uint64_t)(**p <= '9' ? **p - '0' : **p - 'a' + 10);
    return v;
}
static int s2a_maps_line(const char *s, const char *e, s2a_seg_t *g) {
    const char *p = s;
    g->lo = s2a_hexp(&p, e);
    if (p >= e || *p != '-') return 0;
    ++p;
    g->hi = s2a_hexp(&p, e);
    while (p < e && *p == ' ') ++p;
    if (e - p < 4) return 0;
    g->rd = (*p == 'r');
    p += 4;
    while (p < e && *p == ' ') ++p;
    g->off = s2a_hexp(&p, e);
    while (p < e && *p == ' ') ++p;
    g->fid = p;
    for (int f = 0; f < 2 && p < e; ++f) { while (p < e && *p != ' ') ++p; while (p < e && *p == ' ') ++p; }   // dev、inode
    g->fidlen = (size_t)(e - g->fid);
    g->path = p; g->plen = (size_t)(e - p);
    return 1;
}
// 找含 a 的段；maps 段本不重叠，取最后一个命中与旧流式扫描口径一致。
static int s2a_maps_find(const char *m, size_t len, uint64_t a, s2a_seg_t *g) {
    const char *p = m, *e = m + len;
    int hit = 0;
    while (p < e) {
        const char *nl = memchr(p, '\n', (size_t)(e - p)), *le = nl ? nl : e;
        s2a_seg_t t;
        if (s2a_maps_line(p, le, &t) && a >= t.lo && a < t.hi) { *g = t; hit = 1; }
        p = nl ? nl + 1 : e;
    }
    return hit;
}
// 帧记录整 16 字节落在同一可读段内才算可读（未映射 / 不可读 / 跨段都不算；hi - a 不绕回）
static int s2a_maps_readok(const char *m, size_t len, uint64_t a) {
    s2a_seg_t g;
    return s2a_maps_find(m, len, a, &g) && g.rd && g.hi - a >= 16;
}

// ELF 映像合并（#1365）：同一文件（fid 文本相同 = 同 dev:inode+路径）、VA 相接（上段 hi == 本段
//   lo）、file offset 连续（本段 off == 上段 off + 上段长度）的相邻 maps 段算同一映像——
//   NativeAOT InitGSCookie 把 .rodata 一页 mprotect 成 r--，同一个 base.apk 映像裂成
//   r-x|r--|r-x 三段；合并后模块偏移从映像首段起算，不因中间那页断开。
//   命中段所属映像的首段写进 *img；口径与 find 同（段不重叠、末命中生效）。
static int s2a_maps_image(const char *m, size_t len, uint64_t a, s2a_seg_t *img) {
    const char *p = m, *e = m + len;
    s2a_seg_t prev = {0, 0, 0, 0, NULL, 0, NULL, 0}, run = prev;
    int have = 0, hit = 0;
    while (p < e) {
        const char *nl = memchr(p, '\n', (size_t)(e - p)), *le = nl ? nl : e;
        s2a_seg_t t;
        if (s2a_maps_line(p, le, &t)) {
            if (!have || t.lo != prev.hi || t.off != prev.off + prev.hi - prev.lo ||
                t.fidlen != prev.fidlen || memcmp(t.fid, prev.fid, t.fidlen))
                run = t;   // 不相接 / 不同文件 / offset 断：本段另起映像
            if (a >= t.lo && a < t.hi) { *img = run; hit = 1; }
            prev = t; have = 1;
        } else have = 0;   // 解析不了的行断开映像，后面的段不能接回前一个
        p = nl ? nl + 1 : e;
    }
    return hit;
}

// 帧记录经内核读：maps 核查后的 munmap 竞争由内核兜住，读不到回 0，不会二次缺页。
static int s2a_fatal_pread(uint64_t a, uint64_t rec[2]) {
#if defined(__APPLE__)
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)a, 16, (vm_address_t)rec, &got) == KERN_SUCCESS && got == 16;
#elif defined(__linux__)
    struct iovec l = {rec, 16}, r = {(void *)(uintptr_t)a, 16};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == 16;
#else
    (void)a; (void)rec; return 0;
#endif
}
// ucontext 取帧指针：aarch64 = x29，x86_64 = rbp；空 context / 其他架构回 0（不展开）。
static uint64_t s2a_fatal_ctx_fp(const void *context) {
    if (!context) return 0;
    const ucontext_t *u = context;
#if defined(__APPLE__) && defined(__aarch64__)
    return (uint64_t)u->uc_mcontext->__ss.__fp;
#elif defined(__APPLE__)
    return (uint64_t)u->uc_mcontext->__ss.__rbp;
#elif defined(__aarch64__)
    return (uint64_t)u->uc_mcontext.regs[29];
#elif defined(__x86_64__) && defined(REG_RBP)
    return (uint64_t)u->uc_mcontext.gregs[REG_RBP];
#else
    (void)u; return 0;
#endif
}

// 行缓冲有界拼接：超长只截断不越界（snprintf 不在 async-signal-safe 清单，照手写）。
typedef struct { char *b; size_t n, cap; } s2a_w_t;
static void s2a_w_ch(s2a_w_t *w, char c) { if (w->n + 1 < w->cap) w->b[w->n++] = c; }
static void s2a_w_raw(s2a_w_t *w, const char *s) { for (; *s; ++s) s2a_w_ch(w, *s); }
static void s2a_w_mem(s2a_w_t *w, const char *s, size_t n) { for (size_t i = 0; i < n; ++i) s2a_w_ch(w, s[i]); }
static void s2a_w_hex(s2a_w_t *w, uint64_t v) {
    static const char h[] = "0123456789abcdef";
    for (int s = 60; s >= 0; s -= 4) s2a_w_ch(w, h[(v >> s) & 15]);
}
static void s2a_w_dec(s2a_w_t *w, unsigned v) {
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < (int)sizeof t);
    while (n) s2a_w_ch(w, t[--n]);
}

// ===== 一次性探针（只在探针分支，不进 PR）：宿主写客体内存的最近记录 + 已登记内存池 =====
// 写入方（回放落地 / 缓冲命令 / 计数器与事件 / 虚拟池拷页 / 背板打洞）各调一次 s2a_probe_gw_note；内存池登记 / 撤销调 s2a_probe_pool_note。
// 无锁环：下标原子递增，写完字段再写 seq（release）；致命信号里只读，seq 前后不一致的条目跳过。
#include <stdatomic.h>
#include <time.h>
#define S2A_PGW_N 512u
#define S2A_PPOOL_N 2048u
typedef struct { _Atomic uint64_t seq; uint64_t addr, len, ms; uint32_t src, tid; } s2a_pgw_t;
typedef struct { _Atomic uint64_t addr; uint64_t size, src; uint32_t kind; } s2a_ppool_t;
static s2a_pgw_t s2a_pgw[S2A_PGW_N];
static _Atomic uint64_t s2a_pgw_next;
static s2a_ppool_t s2a_ppool[S2A_PPOOL_N];
static const char *const s2a_pgw_src[] = {"?", "land", "update_ub", "copy_b2b", "clear_buf", "report", "event", "vpool_copy", "punch"};
static const char *const s2a_ppool_kind[] = {"?", "pool", "vpool", "valias"};
static uint32_t s2a_probe_tid(void) {
#if defined(__linux__)
    return (uint32_t)syscall(SYS_gettid);
#else
    return 0;
#endif
}
void s2a_probe_gw_note(uint64_t addr, uint64_t len, unsigned src) {
    uint64_t i = atomic_fetch_add_explicit(&s2a_pgw_next, 1, memory_order_relaxed);
    s2a_pgw_t *e = &s2a_pgw[i % S2A_PGW_N];
    atomic_store_explicit(&e->seq, 0, memory_order_relaxed);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    e->addr = addr; e->len = len; e->src = src; e->tid = s2a_probe_tid();
    e->ms = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    atomic_store_explicit(&e->seq, i + 1, memory_order_release);
}
// kind：1 = NVN 内存池，2 = 虚拟池自己的地址段，3 = 虚拟池别名（src = 映入的源地址）；on = 0 撤销同起点那项
void s2a_probe_pool_note(uint64_t addr, uint64_t size, uint64_t src, unsigned kind, int on) {
    if (!addr) return;
    for (unsigned i = 0; i < S2A_PPOOL_N; i++) {
        s2a_ppool_t *p = &s2a_ppool[i];
        if (on) {
            uint64_t z = 0;
            if (!atomic_compare_exchange_strong(&p->addr, &z, addr)) continue;
            p->size = size; p->src = src; p->kind = kind; return;
        }
        if (atomic_load(&p->addr) == addr && p->kind == kind) { p->size = 0; p->kind = 0; atomic_store(&p->addr, 0); return; }
    }
}
static int s2a_probe_hit(uint64_t a0, uint64_t n0, uint64_t lo, uint64_t hi) { return a0 < hi && lo < a0 + n0; }
static void s2a_probe_w_ent(s2a_w_t *w, const s2a_pgw_t *e, uint64_t seq, uint64_t next, uint64_t now) {
    s2a_w_ch(w, ' '); s2a_w_raw(w, e->src < sizeof s2a_pgw_src / sizeof *s2a_pgw_src ? s2a_pgw_src[e->src] : "?");
    s2a_w_ch(w, '@'); s2a_w_hex(w, e->addr); s2a_w_ch(w, '+'); s2a_w_hex(w, e->len);
    s2a_w_raw(w, " -"); s2a_w_dec(w, (unsigned)(next - seq)); s2a_w_raw(w, "写 -"); s2a_w_dec(w, (unsigned)(now > e->ms ? now - e->ms : 0));
    s2a_w_raw(w, "ms t"); s2a_w_dec(w, e->tid);
}
// 一行：每个关注地址 → 所在池 + 与 [r-0x200, r+0x200) 相交的写记录；再打最近 16 条写记录
void s2a_probe_gw_line(int fd, const uint64_t *regs, const char *const *names, int nr) {
    static char buf[8192];
    s2a_w_t w = {buf, 0, sizeof buf};
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    uint64_t next = atomic_load_explicit(&s2a_pgw_next, memory_order_acquire);
    s2a_w_raw(&w, "[s2a] probe gw: 写记录共 "); s2a_w_dec(&w, (unsigned)next);
    for (int r = 0; r < nr; r++) {
        uint64_t a = regs[r], lo = a > 0x200 ? a - 0x200 : 0, hi = a + 0x200;
        s2a_w_raw(&w, " | "); s2a_w_raw(&w, names[r]); s2a_w_ch(&w, '='); s2a_w_hex(&w, a); s2a_w_raw(&w, " 池:");
        int inpool = 0;
        for (unsigned i = 0; i < S2A_PPOOL_N; i++) {
            uint64_t pa = atomic_load(&s2a_ppool[i].addr), ps = s2a_ppool[i].size; unsigned k = s2a_ppool[i].kind;
            if (!pa || !ps) continue;
            int in = a >= pa && a < pa + ps, insrc = k == 3 && a >= s2a_ppool[i].src && a < s2a_ppool[i].src + ps;
            if (!in && !insrc) continue;
            inpool = 1; s2a_w_ch(&w, ' '); s2a_w_raw(&w, k < 4 ? s2a_ppool_kind[k] : "?"); s2a_w_raw(&w, insrc ? "源" : "");
            s2a_w_ch(&w, '@'); s2a_w_hex(&w, pa); s2a_w_ch(&w, '+'); s2a_w_hex(&w, ps);
        }
        if (!inpool) s2a_w_raw(&w, " 无");
        s2a_w_raw(&w, " 相交:");
        int hits = 0;
        uint64_t first = next > S2A_PGW_N ? next - S2A_PGW_N : 0;
        for (uint64_t q = next; q > first && hits < 12; q--) {
            const s2a_pgw_t *e = &s2a_pgw[(q - 1) % S2A_PGW_N];
            uint64_t s1 = atomic_load_explicit(&e->seq, memory_order_acquire);
            s2a_pgw_t c = {0}; c.addr = e->addr; c.len = e->len; c.ms = e->ms; c.src = e->src; c.tid = e->tid;
            if (s1 != q || atomic_load_explicit(&e->seq, memory_order_acquire) != s1) continue;
            if (!s2a_probe_hit(c.addr, c.len, lo, hi)) continue;
            s2a_probe_w_ent(&w, &c, s1, next, now); hits++;
        }
        if (!hits) s2a_w_raw(&w, " 无");
    }
    s2a_w_raw(&w, " | 最近:");
    for (uint64_t q = next, k = 0; q > 0 && k < 16; q--, k++) {
        const s2a_pgw_t *e = &s2a_pgw[(q - 1) % S2A_PGW_N];
        uint64_t s1 = atomic_load_explicit(&e->seq, memory_order_acquire);
        s2a_pgw_t c = {0}; c.addr = e->addr; c.len = e->len; c.ms = e->ms; c.src = e->src; c.tid = e->tid;
        if (s1 != q || atomic_load_explicit(&e->seq, memory_order_acquire) != s1) continue;
        s2a_probe_w_ent(&w, &c, s1, next, now);
    }
    s2a_w_ch(&w, '\n');
    ssize_t ignored = write(fd, buf, w.n); (void)ignored;
}
// 致命现场：这次崩溃里元素 x0、父对象 x20、链表头 x21、邻块 x24，外加 x19 / 故障地址
static void s2a_probe_gw_fatal(int fd, const void *context, uintptr_t addr) {
#if defined(__aarch64__) && !defined(__APPLE__)
    if (!context) return;
    const ucontext_t *u = context;
    uint64_t regs[6] = {u->uc_mcontext.regs[0], u->uc_mcontext.regs[19], u->uc_mcontext.regs[20], u->uc_mcontext.regs[21], u->uc_mcontext.regs[24], (uint64_t)addr};
    static const char *const names[6] = {"x0", "x19", "x20", "x21", "x24", "addr"};
    s2a_probe_gw_line(fd, regs, names, 6);
#else
    (void)fd; (void)context; (void)addr;
#endif
}

// 「模块名+库内偏移」：path 字段取最后 '/' 之后做模块名（addr2line -e lib.so 可直接用）。
//   img = a 所在 ELF 映像的首段（s2a_maps_image）：偏移 = a - 首段 lo + (首段 off - 库在容器内
//   的 data offset)。容器内嵌库（base.apk!/lib/… 这类 '!' 路径）的 data offset = 映像首段
//   off（首段恰是库在容器文件里的第一页）；独立 .so 的容器就是它自己、offset 为 0（不减）。
static void s2a_w_mod(s2a_w_t *w, const s2a_seg_t *img, uint64_t a) {
    const char *b = img->path; size_t bn = img->plen;
    int embedded = 0;
    for (size_t i = 0; i < img->plen; ++i) {
        if (img->path[i] == '/') { b = img->path + i + 1; bn = img->plen - i - 1; }
        if (img->path[i] == '!') embedded = 1;
    }
    s2a_w_mem(w, b, bn); s2a_w_ch(w, '+');
    s2a_w_hex(w, a - img->lo + img->off - (embedded ? img->off : 0));
}

// 安装顺序与 crash.c 的 android_install 共用这一份；s2a_android_fatal_remember 按同一顺序存原处置。
static const int s2a_android_fatal_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGABRT};
#define S2A_ANDROID_FATAL_SIGNAL_COUNT (sizeof s2a_android_fatal_signals / sizeof *s2a_android_fatal_signals)
static struct sigaction s2a_android_fatal_previous[S2A_ANDROID_FATAL_SIGNAL_COUNT];
static int s2a_android_fatal_previous_valid[S2A_ANDROID_FATAL_SIGNAL_COUNT];

static int s2a_android_fatal_index(int sig) {
    for (size_t i = 0; i < S2A_ANDROID_FATAL_SIGNAL_COUNT; ++i)
        if (s2a_android_fatal_signals[i] == sig) return (int)i;
    return -1;
}

// 装自己的处理器之前记下原处置：系统先装的 debuggerd handler 在重放 / 重投时优先链回。
// fn = 本库处理器：若原处置已经是它（先装本库处理器再 remember），视为无效并回落 SIG_DFL，
//   否则恢复后重放会再次进本处理器、无限自环。不能用「非本库地址」判断——NativeAOT 静态链进同一个 .so。
static void s2a_android_fatal_remember(int sig, void (*fn)(int, siginfo_t *, void *)) {
    static int maps_warmed;
    if (!maps_warmed) { maps_warmed = 1; (void)s2a_maps_refresh(); }   // 安装期垫一份快照：崩溃时 open 失败仍有旧表
    int index = s2a_android_fatal_index(sig);
    if (index < 0) return;
    struct sigaction old;
    if (sigaction(sig, NULL, &old) == 0 && old.sa_sigaction != fn) {
        s2a_android_fatal_previous[index] = old;
        s2a_android_fatal_previous_valid[index] = 1;
    }
}

static int s2a_android_fatal_is_handler(const struct sigaction *sa) {
    return sa->sa_handler != SIG_DFL && sa->sa_handler != SIG_IGN;
}

// 同步故障（内核给的 si_code > 0）返回后指令重放；kill / raise / tgkill（si_code <= 0）
// 与 MTE 异步报告（SEGV_MTEAERR）不会重放，必须按原 siginfo 重投，否则进程打完 fatal 行继续跑。
static int s2a_android_fatal_replayable(const siginfo_t *si) {
#if defined(__APPLE__)
    // Darwin 的 si_code 不区分 kill 投递的 SIGSEGV（kill 也是 SEGV_ACCERR），宿主单测一律走重投；
    // 生产路径是 Android，下面按 si_code 区分同步 / 异步。
    (void)si;
    return 0;
#else
    if (!si || si->si_code <= 0) return 0;
#ifdef SEGV_MTEAERR
    if (si->si_code == SEGV_MTEAERR) return 0;
#endif
    return 1;
#endif
}

// 原样重投（bionic debuggerd resend_signal 同法）：保留 si_code / si_pid / si_value；
// 失败再 raise；都失败交回调用方 _exit。正常不返回。
static void s2a_android_fatal_resend(int sig, const siginfo_t *si) {
#if defined(__linux__) && defined(SYS_rt_tgsigqueueinfo)
    long tid = syscall(SYS_gettid);
    if (tid > 0 && syscall(SYS_rt_tgsigqueueinfo, getpid(), (pid_t)tid, sig, (siginfo_t *)si) == 0) return;
#else
    (void)si;
#endif
    (void)raise(sig);
}

// 恢复安装前的处置并解除阻塞。返回 1 = 同步故障，调用方返回后指令重放；
// 返回 0 = 已重投（正常不返回；返回则调用方 _exit(128 + sig) 兜底）。
static int s2a_android_fatal_restore(int sig, const siginfo_t *si) {
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    int index = s2a_android_fatal_index(sig);
    const struct sigaction *action = &dfl;
    if (index >= 0 && s2a_android_fatal_previous_valid[index] &&
        s2a_android_fatal_is_handler(&s2a_android_fatal_previous[index]))
        action = &s2a_android_fatal_previous[index];
    if (sigaction(sig, action, NULL) != 0) return 0;   // 恢复不了就交回调用方 _exit 兜底，不能返回重放成死循环
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, sig);
    if (sigprocmask(SIG_UNBLOCK, &unblock, NULL) != 0) return 0;
    if (s2a_android_fatal_replayable(si)) return 1;
    s2a_android_fatal_resend(sig, si);
    return 0;
}

// fatal 行本体：addr / pc / sp 必打；maps 快照拿到才打 mod 与帧链，拿不到只留 pc/sp、不崩。
//   dladdr 有 linker 锁、信号处理器里可能死锁，模块解析照旧靠 /proc/self/maps——
//   现由调用方先快照进 s2a_maps_buf，这里只对快照纯内存扫描，不开新文件、不分配。
// 帧链：从 ucontext x29 起至多 S2A_FATAL_CHAIN_MAX 层，每层打返回地址 + 所在模块偏移。
//   每层先核 fp 16 字节对齐、严格递增（防环路 / 栈翻转）、[fp,fp+16) 全落某可读段，
//   再经内核读帧记录（核查后的 munmap 竞争由内核兜底）；任一不满足即停并标
//   chain=broken@K（K = 已打层数）。fp 为 0 / 读到返回地址为 0 算自然到顶，不标。
#define S2A_FATAL_CHAIN_MAX 16u   // 帧链层数上限（owner 定 16）
static void s2a_android_fatal_line(int sig, uintptr_t addr, uintptr_t pc, uintptr_t sp, int chained,
                                   uint64_t fp, const char *maps, size_t mlen) {
    char message[4096];   // 16 层帧 + 模块名最坏 ~2.3 KB；s2a_w_* 有界拼接兜底，超长只截断
    s2a_w_t w = {message, 0, sizeof message};
    s2a_w_raw(&w, "[s2a] Android fatal signal ");
    s2a_w_ch(&w, (char)('0' + sig / 10)); s2a_w_ch(&w, (char)('0' + sig % 10));
    const uint64_t values[] = {(uint64_t)addr, (uint64_t)pc, (uint64_t)sp};
    const char *labels[] = {" addr=", " pc=", " sp="};
    for (unsigned i = 0; i < 3; ++i) { s2a_w_raw(&w, labels[i]); s2a_w_hex(&w, values[i]); }
    if (mlen) {
        s2a_seg_t g;
        if (s2a_maps_image(maps, mlen, (uint64_t)pc, &g) && g.plen) { s2a_w_raw(&w, " mod="); s2a_w_mod(&w, &g, (uint64_t)pc); }
        uint64_t prev = 0, rec[2];
        unsigned k = 0; int broke = 0;
        while (fp && k < S2A_FATAL_CHAIN_MAX) {
            if ((fp & 15) || fp <= prev || !s2a_maps_readok(maps, mlen, fp) || !s2a_fatal_pread(fp, rec)) { broke = 1; break; }
            if (!rec[1]) break;
            s2a_w_raw(&w, " f"); s2a_w_dec(&w, k); s2a_w_ch(&w, '='); s2a_w_hex(&w, rec[1]);
            if (s2a_maps_image(maps, mlen, rec[1], &g) && g.plen) { s2a_w_ch(&w, '/'); s2a_w_mod(&w, &g, rec[1]); }
            prev = fp; fp = rec[0]; ++k;
        }
        if (broke) { s2a_w_raw(&w, " chain=broken@"); s2a_w_dec(&w, k); }
    }
    // 行尾标 chained = 本次将链回旧处置（旧处置可能「认领」返回让进程续跑；不标的是恢复重放路径）。
    if (chained) s2a_w_raw(&w, " chained");
    s2a_w_ch(&w, '\n');
    int flags = fcntl(g_s2a_logfd, F_GETFL);
    if (flags >= 0 && !fcntl(g_s2a_logfd, F_SETFL, flags | O_NONBLOCK)) {
        ssize_t ignored = write(g_s2a_logfd, message, w.n); (void)ignored;
    }
    // 同一行再落 red.log：脚本/无 root 取证优先读 red.log；实现只 open/write/fsync，
    // 与 fatal 行同一异步信号安全口径。
    // 只在 __ANDROID__ 下引用：red.log 是安卓侧文件（android/native/redlog.c 提供）；
    // 宿主 / 原生单测（make native-test-*，单个 .c 直接链）没有这个实现，若在 macOS 上留下未定义
    // 弱引用，ld64 会拒绝链接（ELF 允许，所以 Linux 单测看不出来）。Android 内部保持弱符号：
    // engine=OFF / 诊断目标没链 redlog.c 时自然跳过。
#if defined(__ANDROID__)
    {
        extern void s2a_android_redlog_write(const char *, size_t) __attribute__((weak));
        if (s2a_android_redlog_write) s2a_android_redlog_write(message, w.n);
    }
#endif
}

// 可链回 = 装库前记下的旧处置是有效处理器且有现场（context）可传。
static const struct sigaction *s2a_android_fatal_chainable(int sig, const void *context) {
    int index = s2a_android_fatal_index(sig);
    if (index < 0 || !s2a_android_fatal_previous_valid[index] || !context) return NULL;
    const struct sigaction *old = &s2a_android_fatal_previous[index];
    return s2a_android_fatal_is_handler(old) ? old : NULL;
}

// 旧处置是处理器时在同一信号现场直接链回，不拆自己的处置：NativeAOT / 系统 debuggerd 照常接棒，
//   收尾窗口里其他线程的例行写跟踪缺页仍走本库认领，不再落 debuggerd 顶罪（11-05 BotW qbotw8e：
//   原先恢复处置再重放，恢复瞬间起各信号的 s2a 处理器全部缺席，21255 NRE 走 fatal 后 nx-109 顶罪）。
// 调用前按内核投递语义补掩码：阻塞 old->sa_mask，未置 SA_NODEFER 再加本信号；处理器正常返回后还原。
//   debuggerd 注册时是全掩码，不补会让 GC 挂起 / 调试信号压在它上面。处理器 siglongjmp 离开时
//   掩码由它自己恢复（同内核投递），与这里无关。
// 返回语义：处理器返回 = POSIX 的「已处理重放」，调用方返回后出错指令重放。
//   - NativeAOT 认领受管故障（NRE 转换）：改写 ucontext 或 siglongjmp 回托管，进程续跑；
//   - bionic debuggerd：记 tombstone 后 signal(SIG_DFL) 并重投给本线程，然后返回——重放按
//     新处置即死、归因仍在真线程；它若先返回再被重投信号杀死，死因同样是本线程本信号；
//   残留窗口：链回的处理器若返回前用 sigaction 换掉本信号当前处置（debuggerd 的 SIG_DFL、
//   NativeAOT 恢复它自己的上一层），本信号自此脱离本库处置，换层后其他线程的同信号缺页
//   仍可能绕过认领——比原先「恢复瞬间起全信号裸奔到收局」窄得多，修不掉的记为限制。
static void s2a_android_fatal_chain(int sig, const struct sigaction *old,
                                    const siginfo_t *si, void *context) {
    sigset_t block = old->sa_mask, saved;
    if (!(old->sa_flags & SA_NODEFER)) sigaddset(&block, sig);
    sigprocmask(SIG_BLOCK, &block, &saved);
    if (old->sa_flags & SA_SIGINFO) old->sa_sigaction(sig, (siginfo_t *)si, context);
    else old->sa_handler(sig);
    sigprocmask(SIG_SETMASK, &saved, NULL);
}

// 刷屏只压「与上一行完全相同 (sig, addr, pc)」的重复——那才是自环形态（链回返回后指令重放、
//   同一处再缺页）。不能用全局行数预算：s2a 不认领的受管 NRE 也都走这里，N 次被认领的 NRE
//   会把预算耗光，之后的真 fatal 反而没行。无锁读写三个字段：并发真故障最坏多打一行或
//   漏压一行，不影响处置语义。压制只省日志，链回 / 恢复照常走。
static int s2a_android_fatal_dup(int sig, uintptr_t addr, uintptr_t pc) {
    static int last_sig;
    static uintptr_t last_addr, last_pc;
    if (last_sig == sig && last_addr == addr && last_pc == pc) return 1;
    last_sig = sig; last_addr = addr; last_pc = pc;
    return 0;
}

static void s2a_android_fatal(int sig, const siginfo_t *si, uintptr_t pc, uintptr_t sp, void *context) {
    uintptr_t addr = (uintptr_t)si->si_addr;
    const struct sigaction *old = s2a_android_fatal_chainable(sig, context);
    if (!s2a_android_fatal_dup(sig, addr, pc)) {
        size_t ml = s2a_maps_refresh();
        s2a_android_fatal_line(sig, addr, pc, sp, old != NULL, s2a_fatal_ctx_fp(context), s2a_maps_buf, ml);
        s2a_probe_gw_fatal(g_s2a_logfd, context, addr);   // 一次性探针
    }
    if (old) { s2a_android_fatal_chain(sig, old, si, context); return; }
    // 没有可链回的处理器（旧处置缺省 / 忽略）：恢复旧处置重放 / 重投，与现行一致
    if (s2a_android_fatal_restore(sig, si)) return;
    _exit(128 + sig);
}
