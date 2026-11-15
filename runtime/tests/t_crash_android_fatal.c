// 宿主单测：Android fatal 行写完后链回旧处置（不拆自己的处理器）或恢复处置并重放 / 重投，
// 子进程必须按原信号终止（WIFSIGNALED），而不是 _exit(128 + sig)；安装前已有处理器
//（系统 debuggerd handler / NativeAOT NRE 转换的替身）时必须优先链回，且链回后本库处置仍在位
//（11-05 BotW qbotw8e：fatal 拆处置后，另一线程的例行缺页顶罪成死因）。
// 链回按内核投递语义补掩码（sa_mask ∪ 本信号，SA_NODEFER 不加），旧处理器改写 ucontext /
// signal(SIG_DFL) 后返回、非 SA_SIGINFO 形态都要兜住；fatal 行只压「与上一行完全相同
// (sig,addr,pc)」的重复（自环形态），不耗预算。异步投递（raise / kill，si_code <= 0）
// 不能靠返回重放；先装本库处理器再 remember 不能自环。
// 帧链（#1555）：fatal 行沿 ucontext x29 逐层打返回地址 + 模块偏移，至多 16 层，fp 严格递增
// 且 16 字节对齐、每层落可读映射；违反即停标 chain=broken@N。伪造帧记录放静态区（真实可读
// 内存）、maps 文本手工构造做单元级例；端到端例把真实信号现场的 fp 换指伪链全链路验证。
// 直接包含生产实现 crash_android_fatal.inc；macOS / Linux 宿主都跑，不需要设备。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../src/native.h"
#include "../src/crash_android_fatal.inc"
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>

static int bad, n;
static void eq(const char *what, long long want, long long got) {
    n++;
    if (want != got) { printf("✗ %s：期望 %lld → 真结果 %lld\n", what, want, got); bad++; }
}

static void marker(const char *s) { ssize_t w = write(g_s2a_logfd, s, strlen(s)); (void)w; }

static void previous_handler(int sig, siginfo_t *si, void *context) {
    (void)sig; (void)si; (void)context;
    marker("previous-handler\n");
    _exit(42);
}

// NativeAOT NRE 转换替身：「认领」故障并跳回恢复点，进程存活——等价于运行时改写 ucontext 返回
static sigjmp_buf g_recover;
static volatile int g_converted;
static void converter_handler(int sig, siginfo_t *si, void *context) {
    (void)sig; (void)si; (void)context;
    marker("converter\n");
    siglongjmp(g_recover, (int)++g_converted);
}

// NativeAOT 真实路径替身：不改栈、只把 ucontext 的 pc 改指到续跑函数后正常返回——
// 内核 sigreturn 后落在 post_fault，进程续跑（覆盖「改写现场返回」的链回形态）
static void post_fault(void);
static void ctx_set_pc(void *context, uintptr_t pc) {
    ucontext_t *u = context;
#if defined(__APPLE__) && defined(__aarch64__)
    u->uc_mcontext->__ss.__pc = pc;
#elif defined(__APPLE__)
    u->uc_mcontext->__ss.__rip = pc;
#elif defined(__aarch64__)
    u->uc_mcontext.pc = pc;
#elif defined(__x86_64__)
    u->uc_mcontext.gregs[REG_RIP] = (greg_t)pc;
#else
    (void)u; (void)pc;
#endif
}
static void skip_handler(int sig, siginfo_t *si, void *context) {
    (void)sig; (void)si;
    marker("skip-handler\n");
    ctx_set_pc(context, (uintptr_t)&post_fault);
}
static void post_fault(void) {   // 被改写 pc 后经 sigreturn 落到这里；不返回
    marker("resumed\n");
    abort();
}

// bionic debuggerd 替身：记 tombstone 后 signal(SIG_DFL) 再返回——重放按新处置即死
static void dfl_handler(int sig, siginfo_t *si, void *context) {
    (void)si; (void)context;
    marker("dfl-handler\n");
    signal(sig, SIG_DFL);
}

// 非 SA_SIGINFO 旧处置替身：只有 sa_handler(int) 形态可用
static void plain_handler(int sig) {
    (void)sig;
    marker("plain-handler\n");
    _exit(43);
}

// 掩码替身：sa_mask 含 SIGUSR1；g_mask_want_sig = 期望的本信号阻塞态。
//   g_page 非空时修页返回（同时验证返回后掩码被还原、重放成功续跑）。
static volatile int g_mask_want_sig;
static void *g_page;
static void mask_handler(int sig, siginfo_t *si, void *context) {
    (void)si; (void)context;
    sigset_t cur;
    sigprocmask(SIG_SETMASK, NULL, &cur);
    int ok = sigismember(&cur, SIGUSR1) == 1 && sigismember(&cur, sig) == g_mask_want_sig;
    marker(ok ? "mask-ok\n" : "mask-bad\n");
    if (g_page) { mprotect(g_page, 4096, PROT_READ | PROT_WRITE); return; }
    _exit(44);
}

static void fatal_handler(int sig, siginfo_t *si, void *context) {
    s2a_android_fatal(sig, si, 0, 0, context);
}

// ———— 帧链（#1555）：伪造帧记录放静态区（真实可读内存），maps 文本手工构造 ————
static uint64_t g_chain[32][2] __attribute__((aligned(16)));
// g_chain 前 depth 个连成 {prev fp, ra} 链：ra 落在伪代码段 libgame.so，尾 fp = 0。
static void chain_build(int depth) {
    for (int i = 0; i < depth; ++i) {
        g_chain[i][0] = (uint64_t)&g_chain[i + 1];
        g_chain[i][1] = 0x7000000000 + (uint64_t)i * 0x40;
    }
    g_chain[depth - 1][0] = 0;
}
// 造一份 maps 文本：[flo,fhi) 标可读（帧记录区），两段伪代码段给返回地址解析模块偏移。
static size_t make_maps(char *b, size_t cap, uint64_t flo, uint64_t fhi) {
    int n = snprintf(b, cap,
        "%012llx-%012llx rw-p 00000000 00:00 0\n"
        "00007000000000-00007000100000 r-xp 00001000 00:00 0 /data/app/libgame.so\n"
        "00007000100000-00007000200000 r-xp 00020000 00:00 0 /data/app/libcore.so\n",
        (unsigned long long)flo, (unsigned long long)fhi);
    return n > 0 ? (size_t)n : 0;
}
// 造一份「映像裂开」的 maps 文本（#1365）：base.apk 内嵌库（'!' 路径）在 VA 上连排四段——
//   ELF 头 r--p、正文 r-xp、GS cookie 页被改成 r--p、正文续 r-xp，file offset 连续。
//   s3off / s3ino 可调：offset 断或 inode 不同都能验证「不合并」的负对照。
static size_t make_maps_split(char *b, size_t cap, uint64_t flo, uint64_t fhi,
                              uint64_t s3off, uint64_t s3ino) {
    int n = snprintf(b, cap,
        "%012llx-%012llx rw-p 00000000 00:00 0\n"
        "00007000000000-00007000008000 r--p 001c7000 fd:00 1234 /data/app/base.apk!/lib/arm64-v8a/libgame.so\n"
        "00007000008000-00007000016000 r-xp 001cf000 fd:00 1234 /data/app/base.apk!/lib/arm64-v8a/libgame.so\n"
        "00007000016000-00007000017000 r--p 001dd000 fd:00 1234 /data/app/base.apk!/lib/arm64-v8a/libgame.so\n"
        "00007000017000-00007000024000 r-xp %08llx fd:00 %llu /data/app/base.apk!/lib/arm64-v8a/libgame.so\n",
        (unsigned long long)flo, (unsigned long long)fhi,
        (unsigned long long)s3off, (unsigned long long)s3ino);
    return n > 0 ? (size_t)n : 0;
}
// ucontext fp 改指（端到端例把真实现场的 x29 换指伪链）；平台分支同 ctx_set_pc。
static void ctx_set_fp(void *context, uint64_t fp) {
    ucontext_t *u = context;
#if defined(__APPLE__) && defined(__aarch64__)
    u->uc_mcontext->__ss.__fp = fp;
#elif defined(__APPLE__)
    u->uc_mcontext->__ss.__rbp = fp;
#elif defined(__aarch64__)
    u->uc_mcontext.regs[29] = fp;
#elif defined(__x86_64__)
    u->uc_mcontext.gregs[REG_RBP] = (greg_t)fp;
#else
    (void)u; (void)fp;
#endif
}
static void fatal_chain_handler(int sig, siginfo_t *si, void *context) {
    ctx_set_fp(context, (uint64_t)&g_chain[0]);
    s2a_android_fatal(sig, si, 0, 0, context);
}
// 直接调 fatal 行函数（不走信号、不死进程）：g_s2a_logfd 指临时文件，读回输出。
static void emit_line(char *out, size_t cap, uint64_t pc, int chained, uint64_t fp,
                      const char *maps, size_t mlen) {
    char path[] = "/tmp/t_caf_line_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { out[0] = 0; return; }
    g_s2a_logfd = fd;
    s2a_android_fatal_line(11, 0xdead, pc, 0x7fff0000, chained, fp, maps, mlen);
    lseek(fd, 0, SEEK_SET);
    ssize_t k = read(fd, out, cap - 1);
    out[k > 0 ? k : 0] = 0;
    close(fd);
    unlink(path);
}
static int count_frames(const char *out) {   // 行里 fK= 记号个数 = 实际打出的层数
    int k = 0;
    for (int i = 0; i < 32; ++i) {
        char w[8];
        snprintf(w, sizeof w, " f%d=", i);
        if (strstr(out, w)) k++;
    }
    return k;
}

static int install_handler_fl(int sig, void (*fn)(int, siginfo_t *, void *), int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fn;
    sa.sa_flags = SA_SIGINFO | flags;
    sigemptyset(&sa.sa_mask);
    return sigaction(sig, &sa, NULL);
}
static int install_handler(int sig, void (*fn)(int, siginfo_t *, void *)) {
    return install_handler_fl(sig, fn, 0);
}
// 非 SA_SIGINFO 处置（sa_handler 单参形态）
static int install_plain(int sig, void (*fn)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    return sigaction(sig, &sa, NULL);
}
// sa_mask 带 SIGUSR1 的 SA_SIGINFO 处置（掩码替身专用）
static int install_masked(int sig, void (*fn)(int, siginfo_t *, void *), int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fn;
    sa.sa_flags = SA_SIGINFO | flags;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR1);
    return sigaction(sig, &sa, NULL);
}

enum { TRIG_ABORT, TRIG_NULL, TRIG_RAISE, TRIG_KILL };
enum { MODE_DEFAULT, MODE_CHAIN, MODE_SELF, MODE_CONVERT, MODE_SKIP, MODE_DFL,
       MODE_PLAIN, MODE_MASK_FIX, MODE_MASK_BLK, MODE_MASK_NOD,
       MODE_NOFD, MODE_FRAME_CHAIN };

static void segv_write(void) { volatile int *p = (volatile int *)0; *p = 1; }
static void segv_write_alt(void) { volatile int *p = (volatile int *)0x40; *p = 1; }
static void page_write(void) { volatile int *p = g_page; *p = 1; }

static pid_t child_crash(int sig, int trigger, int mode, int fd) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    g_s2a_logfd = fd;
    if (mode == MODE_NOFD) {   // fd 耗尽型崩溃：子进程全程不能 open，maps 必读不到 → 行只打 pc/sp
        struct rlimit z = {0, 0};
        setrlimit(RLIMIT_NOFILE, &z);
        s2a_maps_len = 0;
    }
    if (mode == MODE_FRAME_CHAIN) {   // 注入 maps 快照 + 5 层伪链；真实现场 x29 换指伪链走全链路
        s2a_maps_len = make_maps(s2a_maps_buf, sizeof s2a_maps_buf,
                                 (uint64_t)&g_chain[0] & ~0xFFFull,
                                 ((uint64_t)&g_chain[0] & ~0xFFFull) + 0x1000);
        s2a_maps_frozen = 1;
        chain_build(5);
        s2a_android_fatal_remember(sig, fatal_chain_handler);
        if (install_handler(sig, fatal_chain_handler)) _exit(92);
        segv_write();
        _exit(93);
    }
    if (mode == MODE_CHAIN && install_handler(sig, previous_handler)) _exit(90);
    if (mode == MODE_CONVERT && install_handler(sig, converter_handler)) _exit(90);
    if (mode == MODE_SKIP && install_handler(sig, skip_handler)) _exit(90);
    if (mode == MODE_DFL && install_handler(sig, dfl_handler)) _exit(90);
    if (mode == MODE_PLAIN && install_plain(sig, plain_handler)) _exit(90);
    if (mode == MODE_MASK_FIX || mode == MODE_MASK_BLK || mode == MODE_MASK_NOD) {
        g_mask_want_sig = (mode != MODE_MASK_NOD);   // NOD 期不阻塞本信号；FIX / BLK 期阻塞
        if (mode == MODE_MASK_FIX) {
            g_page = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (g_page == MAP_FAILED) _exit(97);
        }
        if (install_masked(sig, mask_handler, mode == MODE_MASK_NOD ? SA_NODEFER : 0)) _exit(90);
    }
    if (mode == MODE_SELF && install_handler(sig, fatal_handler)) _exit(91);
    s2a_android_fatal_remember(sig, fatal_handler);
    // 掩码判别例把本库处置装成 SA_NODEFER：内核投递不阻塞本信号，
    //   链回时「本信号该不该被阻塞」才能只由链回代码决定
    int own_nodefer = (mode == MODE_MASK_BLK || mode == MODE_MASK_NOD) ? SA_NODEFER : 0;
    if (install_handler_fl(sig, fatal_handler, own_nodefer)) _exit(92);
    if (mode == MODE_CONVERT || mode == MODE_SKIP) {   // 收尾的 SIGABRT 也要走本库 fatal
        s2a_android_fatal_remember(SIGABRT, fatal_handler);
        if (install_handler(SIGABRT, fatal_handler)) _exit(92);
    }
    if (mode == MODE_SELF) alarm(2);   // 自环时不能把测试挂死：2 秒后 SIGALRM 结束，判据会红
    if (mode == MODE_CONVERT) {
        alarm(3);   // 转换链断档时不能让测试挂死
        // 旧处置「认领」故障后进程存活（NRE 转换语义）：后续同信号必须仍走本库 fatal——
        // 若本库处置被拆掉，第二次缺页直接进旧处理器，没有第二行 fatal（BotW qbotw8e 顶罪机制）。
        // 去重只压「与上一行完全相同」：同址紧挨的第 4 次缺页没行；不同址的第 2 / 3 次各有行。
        switch (sigsetjmp(g_recover, 1)) {
        case 0: segv_write(); _exit(94);      // (11, addr 0)     → 行 1
        case 1: segv_write_alt(); _exit(95);  // (11, addr 0x40)  → 行 2（处置仍在位）
        case 2: segv_write(); _exit(97);      // (11, addr 0)     → 行 3（与上一行不同址）
        case 3: segv_write(); _exit(98);      // (11, addr 0)     → 与行 3 完全相同 → 压制
        default: abort();   // 第四次转换后结束：SIGABRT 走本库 fatal 收尾
        }
    }
    if (mode == MODE_SKIP) {
        alarm(3);   // pc 改写失灵时重放会自环，不能挂死
        segv_write();   // 替身把 pc 改指 post_fault：正常流程回不到这里
        _exit(96);
    }
    if (mode == MODE_MASK_FIX) {
        page_write();   // 替身修页后返回：重放成功、续走到这里
        sigset_t cur;
        sigprocmask(SIG_SETMASK, NULL, &cur);
        marker(sigismember(&cur, SIGUSR1) ? "mask-kept\n" : "mask-restored\n");
        _exit(45);
    }
    switch (trigger) {
    case TRIG_ABORT: abort(); break;
    case TRIG_NULL: segv_write(); break;
    case TRIG_RAISE: raise(sig); break;
    case TRIG_KILL: kill(getpid(), sig); break;
    }
    _exit(93);
}

// 跑一个子进程，读回日志。返回 wait status。
static int run_case(int sig, int trigger, int mode, char *out, size_t cap) {
    char path[] = "/tmp/t_crash_android_fatal_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { out[0] = 0; return -1; }
    pid_t c = child_crash(sig, trigger, mode, fd);
    int ws = 0;
    if (c > 0) waitpid(c, &ws, 0);
    lseek(fd, 0, SEEK_SET);
    ssize_t k = read(fd, out, cap - 1);
    out[k > 0 ? k : 0] = 0;
    close(fd);
    unlink(path);
    return ws;
}

static int count_lines(const char *out) {
    int lines = 0;
    for (const char *p = out; (p = strstr(p, "[s2a] Android fatal signal ")); p++) lines++;
    return lines;
}

static int count_sig(const char *out, int sig) {
    char want[48];
    snprintf(want, sizeof want, "[s2a] Android fatal signal %02d", sig);
    int lines = 0;
    for (const char *p = out; (p = strstr(p, want)); p++) lines++;
    return lines;
}

static int count_str(const char *out, const char *what) {
    int hits = 0;
    for (const char *p = out; (p = strstr(p, what)); p++) hits++;
    return hits;
}

static void check_fatal_line(const char *out, int sig) {
    char want[48];
    snprintf(want, sizeof want, "[s2a] Android fatal signal %02d addr=", sig);
    const char *line = strstr(out, "[s2a] Android fatal signal ");
    eq("fatal 行写出且信号号正确", 1, strstr(out, want) != NULL);
    eq("fatal 行带 pc / sp", 1, line && strstr(line, " pc=") != NULL && strstr(line, " sp=") != NULL);
    eq("fatal 行以换行结束", 1, line && strchr(line, '\n') != NULL);
}

static void check_signal(int sig, int ws, const char *what) {
    eq(what, 1, WIFSIGNALED(ws));
    eq(what, sig, WIFSIGNALED(ws) ? WTERMSIG(ws) : -1);
}

// 一次性探针（探针分支）：写记录与池表 → 致命现场一行；相交 / 不相交 / 池内 / 别名源 / 环绕
static void probe_line(char *out, size_t cap, const uint64_t *regs, const char *const *names, int nr) {
    int pf[2]; if (pipe(pf)) { out[0] = 0; return; }
    s2a_probe_gw_line(pf[1], regs, names, nr); close(pf[1]);
    ssize_t k = read(pf[0], out, cap - 1); out[k > 0 ? k : 0] = 0; close(pf[0]);
}
static void probe_case(char *out, size_t cap) {
    s2a_probe_pool_note(0x10f7000000, 0x1000000, 0, 1, 1);           // 一个池
    s2a_probe_pool_note(0x7398000000, 0x10000, 0x10ed720000, 3, 1);  // 虚拟池别名：源是游戏堆里的 0x10ed720000
    s2a_probe_gw_note(0x10ed724960, 0x40, 2);                        // 压到元素
    for (int i = 0; i < 600; i++) s2a_probe_gw_note(0x5000000 + (uint64_t)i * 0x1000, 0x10, 4);   // 远处，且转过一圈：最早的那条被冲掉
    s2a_probe_gw_note(0x10f7233c00, 0x20, 1);                        // 压到父对象
    uint64_t regs[3] = {0x10ed724970, 0x10f7233c40, 0x2000000000};
    const char *const names[3] = {"x0", "x20", "x24"};
    probe_line(out, cap, regs, names, 3);
    eq("探针：总写数", 1, strstr(out, "写记录共 602") != NULL);
    eq("探针：转过一圈后压到元素的那条已冲掉（x0 相交 无）", 1, strstr(out, "x0=00000010ed724970 池: valias源@0000007398000000+0000000000010000 相交: 无") != NULL);
    eq("探针：父对象在池内且相交落地写", 1, strstr(out, "x20=00000010f7233c40 池: pool@00000010f7000000+0000000001000000 相交: land@00000010f7233c00+0000000000000020 -0写") != NULL);
    eq("探针：远处地址不在池、不相交（负对照）", 1, strstr(out, "x24=0000002000000000 池: 无 相交: 无") != NULL);
    eq("探针：最近 16 条", 16, count_str(strstr(out, "| 最近:") ? strstr(out, "| 最近:") : "", " clear_buf@") + count_str(strstr(out, "| 最近:") ? strstr(out, "| 最近:") : "", " land@"));
    s2a_probe_gw_note(0x10ed724968, 0x10, 3);                        // 再压元素一次：这回在环里
    probe_line(out, cap, regs, names, 3);
    eq("探针：元素相交 copy_b2b", 1, strstr(out, "x0=00000010ed724970 池: valias源@0000007398000000+0000000000010000 相交: copy_b2b@00000010ed724968+0000000000000010 -0写") != NULL);
    s2a_probe_pool_note(0x10f7000000, 0, 0, 1, 0);                   // 池撤销后不再算池内
    probe_line(out, cap, regs, names, 3);
    eq("探针：池撤销后父对象不在池", 1, strstr(out, "x20=00000010f7233c40 池: 无") != NULL);
}

int main(void) {
    static char out[8192];
    // ① SIGABRT：abort() 必须按 SIGABRT 终止，不是 exit(134)；无旧处置可链 → 行尾无 chained
    int ws = run_case(SIGABRT, TRIG_ABORT, MODE_DEFAULT, out, sizeof out);
    check_fatal_line(out, SIGABRT);
    check_signal(SIGABRT, ws, "① abort：按 SIGABRT 终止");
    eq("① 恢复重放路径：行尾无 chained 标记", 0, count_str(out, " chained"));
    // ② SIGSEGV：空指针写必须按 SIGSEGV 终止，不是 exit(139)
    ws = run_case(SIGSEGV, TRIG_NULL, MODE_DEFAULT, out, sizeof out);
    check_fatal_line(out, SIGSEGV);
    check_signal(SIGSEGV, ws, "② 空指针写：按 SIGSEGV 终止");
    // ③ 异步 raise(SIGFPE)：si_code <= 0，返回不会重放，必须原样重投
    ws = run_case(SIGFPE, TRIG_RAISE, MODE_DEFAULT, out, sizeof out);
    check_fatal_line(out, SIGFPE);
    check_signal(SIGFPE, ws, "③ raise(SIGFPE)：按 SIGFPE 终止");
    // ④ 异步 kill(SIGSEGV)：si_code = SI_USER，同上
    ws = run_case(SIGSEGV, TRIG_KILL, MODE_DEFAULT, out, sizeof out);
    check_fatal_line(out, SIGSEGV);
    check_signal(SIGSEGV, ws, "④ kill(SIGSEGV)：按 SIGSEGV 终止");
    // ⑤ 安装前已有处理器：优先链回它（debuggerd handler 的替身），而不是恢复后落到 SIG_DFL
    ws = run_case(SIGSEGV, TRIG_NULL, MODE_CHAIN, out, sizeof out);
    check_fatal_line(out, SIGSEGV);
    eq("⑤ 链回原处理器：原处理器被调用", 1, strstr(out, "previous-handler") != NULL);
    eq("⑤ 链回原处理器：原处理器的退出码", 42, WIFEXITED(ws) ? WEXITSTATUS(ws) : -1);
    eq("⑤ 链回路径：行尾带 chained 标记", 1, count_str(out, " chained"));
    // ⑥ 自环防护：先装本库 handler 再 remember，原处置应视为无效并回落 SIG_DFL（不是无限重放）
    ws = run_case(SIGSEGV, TRIG_NULL, MODE_SELF, out, sizeof out);
    check_signal(SIGSEGV, ws, "⑥ 先装本库 handler：仍按 SIGSEGV 终止（不是 SIGALRM）");
    eq("⑥ 先装本库 handler：只写一行 fatal（没有自环）", 1, count_lines(out));
    // ⑦ 转换链回存活后处置仍在位 + 去重（顶罪机制与刷屏压制回归）：
    //    旧处置 siglongjmp 认领 4 次缺页，第 4 次与第 3 次 (sig,addr,pc) 完全相同 → 压制；
    //    修前第二行缺失（缺页直接进旧处理器），全放行打 4 行——3 行 SEGV + 1 行 SIGABRT 收尾
    ws = run_case(SIGSEGV, TRIG_NULL, MODE_CONVERT, out, sizeof out);
    eq("⑦ 转换链回：SEGV 的 fatal 行数", 3, count_sig(out, SIGSEGV));
    eq("⑦ 转换链回：addr 0 的 SEGV 行（缺页 1 / 3 各一行）", 2, count_str(out, "signal 11 addr=0000000000000000"));
    eq("⑦ 转换链回：addr 0x40 的 SEGV 行（缺页 2）", 1, count_str(out, "signal 11 addr=0000000000000040"));
    eq("⑦ 转换链回：SEGV 行都带 chained", 3, count_str(out, " chained"));
    eq("⑦ 转换链回：收尾 SIGABRT 的 fatal 行数", 1, count_sig(out, SIGABRT));
    eq("⑦ 转换链回：旧处理器被链回 4 次（压制只省日志、不省链回）", 4, count_str(out, "converter\n"));
    check_signal(SIGABRT, ws, "⑦ 转换链回：按 SIGABRT 终止");
    // ⑧ ucontext pc 改写后返回（NativeAOT 真实路径替身）：进程经 sigreturn 续跑到 post_fault，
    //    再 abort 收尾——证明链回不依赖 siglongjmp 形态
    ws = run_case(SIGSEGV, TRIG_NULL, MODE_SKIP, out, sizeof out);
    eq("⑧ pc 改写链回：SEGV 的 fatal 行数", 1, count_sig(out, SIGSEGV));
    eq("⑧ pc 改写链回：替身被链回", 1, count_str(out, "skip-handler\n"));
    eq("⑧ pc 改写链回：sigreturn 后续跑到 post_fault", 1, count_str(out, "resumed\n"));
    check_signal(SIGABRT, ws, "⑧ pc 改写链回：按 SIGABRT 终止");
    // ⑨ 旧处置 signal(SIG_DFL) 后返回（bionic debuggerd 形态）：重放按新处置即死，
    //    归因仍在真故障线程本信号
    ws = run_case(SIGSEGV, TRIG_NULL, MODE_DFL, out, sizeof out);
    eq("⑨ DFL 链回：SEGV 的 fatal 行数", 1, count_sig(out, SIGSEGV));
    eq("⑨ DFL 链回：替身被链回", 1, count_str(out, "dfl-handler\n"));
    check_signal(SIGSEGV, ws, "⑨ DFL 链回：重放后按 SIGSEGV 终止");
    // ⑩ 非 SA_SIGINFO 旧处置：链回必须走 sa_handler(sig) 形态
    ws = run_case(SIGSEGV, TRIG_NULL, MODE_PLAIN, out, sizeof out);
    eq("⑩ 单参链回：SEGV 的 fatal 行数", 1, count_sig(out, SIGSEGV));
    eq("⑩ 单参链回：单参处理器被调用", 1, count_str(out, "plain-handler\n"));
    eq("⑩ 单参链回：处理器的退出码", 43, WIFEXITED(ws) ? WEXITSTATUS(ws) : -1);
    // ⑪ 掩码套用 + 返回还原：替身 sa_mask={SIGUSR1} 无 SA_NODEFER → 替身里 SIGUSR1 与本信号
    //    都阻塞；替身修页返回 → 掩码还原、重放成功续走。
    //    （PROT_NONE 页访问：macOS 给 SIGBUS、Linux / 安卓给 SIGSEGV——按平台取，11-05 Linux CTest 上用 SIGBUS 装替身没被调到、⑪ 三项红）
#ifdef __APPLE__
    ws = run_case(SIGBUS, TRIG_NULL, MODE_MASK_FIX, out, sizeof out);
#else
    ws = run_case(SIGSEGV, TRIG_NULL, MODE_MASK_FIX, out, sizeof out);
#endif
    eq("⑪ 掩码：替身里 sa_mask∪本信号 都阻塞", 1, count_str(out, "mask-ok\n"));
    eq("⑪ 掩码：无 mask-bad", 0, count_str(out, "mask-bad\n"));
    eq("⑪ 掩码：处理器返回后掩码还原", 1, count_str(out, "mask-restored\n"));
    eq("⑪ 掩码：重放续走后退出码", 45, WIFEXITED(ws) ? WEXITSTATUS(ws) : -1);
    // ⑫ 掩码补本信号：本库处置装 SA_NODEFER（内核不阻塞本信号）+ 替身无 SA_NODEFER
    //    → 替身里本信号必须被链回补上（期望阻塞）；不加就是红
    ws = run_case(SIGSEGV, TRIG_NULL, MODE_MASK_BLK, out, sizeof out);
    eq("⑫ 掩码：替身无 NODEFER → 本信号被补上", 1, count_str(out, "mask-ok\n"));
    eq("⑫ 掩码：无 mask-bad", 0, count_str(out, "mask-bad\n"));
    eq("⑫ 掩码：替身退出码", 44, WIFEXITED(ws) ? WEXITSTATUS(ws) : -1);
    // ⑬ 掩码免本信号：本库处置装 SA_NODEFER + 替身 SA_NODEFER → 替身里本信号必须仍不阻塞
    //    （期望不阻塞）；无脑加进去就是红
    ws = run_case(SIGSEGV, TRIG_NULL, MODE_MASK_NOD, out, sizeof out);
    eq("⑬ 掩码：替身 SA_NODEFER → 本信号不被加", 1, count_str(out, "mask-ok\n"));
    eq("⑬ 掩码：无 mask-bad", 0, count_str(out, "mask-bad\n"));
    eq("⑬ 掩码：替身退出码", 44, WIFEXITED(ws) ? WEXITSTATUS(ws) : -1);
    // ⑭ 帧链已知深度：5 层伪链全打出，每层返回地址 + 模块偏移逐字可核，无 broken
    static char mapsbuf[1024];
    uint64_t glo = (uint64_t)&g_chain[0] & ~0xFFFull;
    size_t ml = make_maps(mapsbuf, sizeof mapsbuf, glo, glo + 0x1000);
    chain_build(5);
    emit_line(out, sizeof out, 0x7000000080, 0, (uint64_t)&g_chain[0], mapsbuf, ml);
    eq("⑭ pc 所在模块偏移照打", 1, strstr(out, " mod=libgame.so+0000000000001080") != NULL);
    for (int i = 0; i < 5; ++i) {
        char w[96], lbl[48];
        snprintf(w, sizeof w, " f%d=%016llx/libgame.so+%016llx",
                 i, 0x7000000000ULL + (unsigned)i * 0x40, 0x1000ULL + (unsigned)i * 0x40);
        snprintf(lbl, sizeof lbl, "⑭ 第 %d 层返回地址+模块偏移", i);
        eq(lbl, 1, strstr(out, w) != NULL);
    }
    eq("⑭ 恰好 5 层", 5, count_frames(out));
    eq("⑭ 无 broken 标记", 0, count_str(out, "chain=broken"));
    // ⑮ 负对照：起始 x29 即坏（未 16 对齐）→ 0 层 + chain=broken@0，pc/sp 照打不崩；
    //    链中途折返（fp 不严格递增）→ 打出 3 层后标 broken@3
    emit_line(out, sizeof out, 0x7000000080, 0, (uint64_t)&g_chain[0] + 8, mapsbuf, ml);
    eq("⑮ 坏 x29 不打帧", 0, count_frames(out));
    eq("⑮ 坏 x29 标 broken@0", 1, count_str(out, " chain=broken@0"));
    eq("⑮ 坏 x29 pc/sp 照打", 1, strstr(out, " pc=") != NULL && strstr(out, " sp=") != NULL);
    chain_build(5);
    g_chain[2][0] = (uint64_t)&g_chain[1];
    emit_line(out, sizeof out, 0x7000000080, 0, (uint64_t)&g_chain[0], mapsbuf, ml);
    eq("⑮ 折返链打 3 层", 3, count_frames(out));
    eq("⑮ 折返链标 broken@3", 1, count_str(out, " chain=broken@3"));
    // ⑯ 帧链指向未映射页停在第一层：maps 只给第 0 帧这 16 字节可读，
    //    g_chain[0] 的 fp 递增且对齐、只是落在映射外 → 打 1 层 + chain=broken@1
    chain_build(5);
    ml = make_maps(mapsbuf, sizeof mapsbuf, (uint64_t)&g_chain[0], (uint64_t)&g_chain[0] + 16);
    emit_line(out, sizeof out, 0x7000000080, 0, (uint64_t)&g_chain[0], mapsbuf, ml);
    eq("⑯ 只打出第一层", 1, count_frames(out));
    eq("⑯ 标 broken@1", 1, count_str(out, " chain=broken@1"));
    // ⑰ 上限 16：20 层链只打 16 层；到上限截断是自然停、不标 broken
    chain_build(20);
    ml = make_maps(mapsbuf, sizeof mapsbuf, glo, glo + 0x1000);
    emit_line(out, sizeof out, 0x7000000080, 0, (uint64_t)&g_chain[0], mapsbuf, ml);
    eq("⑰ 到 16 层上限", 16, count_frames(out));
    eq("⑰ 无 f16", 0, strstr(out, " f16=") != NULL);
    eq("⑰ 上限截断不标 broken", 0, count_str(out, "chain=broken"));
    // ⑱ maps 拿不到（len 0）：行只打 addr / pc / sp，无 mod、无帧，不崩
    emit_line(out, sizeof out, 0x7000000080, 0, (uint64_t)&g_chain[0], mapsbuf, 0);
    eq("⑱ pc/sp 照打", 1, strstr(out, " pc=") != NULL && strstr(out, " sp=") != NULL);
    eq("⑱ 无 mod", 0, count_str(out, " mod="));
    eq("⑱ 无帧", 0, count_frames(out));
    // ⑲ 端到端 maps 读不到：子进程 RLIMIT_NOFILE=0 → open 必失败 → 只打 pc/sp、仍按原信号死
    ws = run_case(SIGSEGV, TRIG_NULL, MODE_NOFD, out, sizeof out);
    check_fatal_line(out, SIGSEGV);
    eq("⑲ 无 mod", 0, count_str(out, " mod="));
    eq("⑲ 无帧", 0, count_frames(out));
    check_signal(SIGSEGV, ws, "⑲ maps 读不到仍按 SIGSEGV 终止");
    // ⑳ 端到端帧链：真实 SIGSEGV 现场 x29 换指 5 层伪链 → 帧随 fatal 行打出、按原信号死
    ws = run_case(SIGSEGV, TRIG_NULL, MODE_FRAME_CHAIN, out, sizeof out);
    eq("⑳ 端到端 5 层", 5, count_frames(out));
    eq("⑳ 端到端第 0 层模块偏移", 1, strstr(out, " f0=0000007000000000/libgame.so+0000000000001000") != NULL);
    eq("⑳ 端到端第 4 层模块偏移", 1, strstr(out, " f4=0000007000000100/libgame.so+0000000000001100") != NULL);
    eq("⑳ 端到端无 broken", 0, count_str(out, "chain=broken"));
    check_signal(SIGSEGV, ws, "⑳ 端到端按 SIGSEGV 终止");
    // ㉑ 映像合并（#1365）：base.apk 内嵌库四段 r--p|r-xp|r--p|r-xp、VA 连排且 file offset
    //    连续 → 同一 ELF 映像；pc 与帧返回地址都换算回库内偏移，不因中间 r-- 页断开。
    //    pc=0x7000023040 的库内偏移 = pc - 映像首段 lo = 0x23040（APK 文件内口径会是 0x1ea040）
    ml = make_maps_split(mapsbuf, sizeof mapsbuf, glo, glo + 0x1000, 0x1de000, 1234);
    chain_build(2);
    g_chain[0][1] = 0x700000f100;   // 映像第二段（r-xp）
    g_chain[1][1] = 0x7000023040;   // 映像第四段（r-- 页之后）
    emit_line(out, sizeof out, 0x7000023040, 0, (uint64_t)&g_chain[0], mapsbuf, ml);
    eq("㉑ pc 换算回库内偏移", 1, strstr(out, " mod=libgame.so+0000000000023040") != NULL);
    eq("㉑ f0 库内偏移", 1, strstr(out, " f0=000000700000f100/libgame.so+000000000000f100") != NULL);
    eq("㉑ f1 跨 r-- 页仍是库内偏移", 1, strstr(out, " f1=0000007000023040/libgame.so+0000000000023040") != NULL);
    eq("㉑ 不打 APK 文件内偏移", 0, count_str(out, "+00000000001ea040"));
    // ㉒ 负对照：file offset 断（期望 001de000、实填 00999000）→ 末段不并入映像、按本段
    //    起算偏移（pc - 本段 lo = 0xc040）；同路径同 offset 但 inode 不同（另一文件）也不合并
    ml = make_maps_split(mapsbuf, sizeof mapsbuf, glo, glo + 0x1000, 0x999000, 1234);
    emit_line(out, sizeof out, 0x7000023040, 0, (uint64_t)&g_chain[0], mapsbuf, ml);
    eq("㉒ offset 断不合并：pc 按本段起算", 1, strstr(out, " mod=libgame.so+000000000000c040") != NULL);
    eq("㉒ offset 断不合并：f1 同口径", 1, strstr(out, " f1=0000007000023040/libgame.so+000000000000c040") != NULL);
    eq("㉒ offset 断不合并：无映像口径偏移", 0, count_str(out, "+0000000000023040"));
    ml = make_maps_split(mapsbuf, sizeof mapsbuf, glo, glo + 0x1000, 0x1de000, 5678);
    emit_line(out, sizeof out, 0x7000023040, 0, (uint64_t)&g_chain[0], mapsbuf, ml);
    eq("㉒ inode 不同不合并：pc 按本段起算", 1, strstr(out, " mod=libgame.so+000000000000c040") != NULL);
    eq("㉒ inode 不同不合并：无映像口径偏移", 0, count_str(out, "+0000000000023040"));
    probe_case(out, sizeof out);
    if (bad) { printf("✗ t_crash_android_fatal：%d/%d 项不符\n", bad, n); return 1; }
    printf("✓ t_crash_android_fatal：%d 项全符（fatal 行写出；abort / 空指针写 / 异步 raise / kill 按原信号终止；原处理器链回；链回后本库处置仍在位；同址重复行压制；pc 改写 / SIG_DFL 返回 / 单参 / 掩码替身全覆盖；先装本库 handler 不自环；帧链已知深度 / 坏 x29 / 未映射页 / 16 上限 / maps 读不到降级；裂映像合并回库内偏移 / offset 断与 inode 异不合并全覆盖）\n", n);
    return 0;
}
