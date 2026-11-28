// 崩溃现场（旧库 crash.c 改后搬，只留信号处理本身）：出错地址、pc / lr 换成模块内偏移、寄存器、帧指针回溯、线程表。
#include <errno.h>
#include "native.h"
#include "s2a_direct_host.h"
#include "s2a_docs.h"   // 崩溃落点标记写在文档根（s2a_crash_install 时算好路径，信号里只 open / write）
#include "../../platform/plat.h"
#include <signal.h>
#include <dlfcn.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <fcntl.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#else
#include <sys/uio.h>
#endif
#if (defined(__ANDROID__) || defined(__OHOS__))
#include "../../android/native/runtime/fault/fault.h"
#include <fcntl.h>
#include <stdatomic.h>
#endif

__attribute__((weak)) int s2a_trap_brk(void *ctx);   // traps.c；只编 crash.c 的单测 / 安卓测试壳里没有它
static void where(uint64_t a, char *b, size_t n) {   // 地址 → 「模块+偏移」或宿主符号
    if (s2a_guest_where(a, b, n)) return;   // 游戏模块：「模块+偏移（导出名+偏移）」（loader.c，#806：玩家那版 sdk 我们手里没有时也能读回溯）
    module_t *m = module_of((void *)a);
    if (m) {   // 补丁区：近端在映像尾（偏移 ≥ patch_off），FAR 在基址前（偏移为负，按有符号打，原先无符号相减打成 0xffff… 回绕值）
        int64_t off = (int64_t)(a - (uint64_t)m->base);
        int patch = s2a_in_patch(m, a);   // 任一补丁岛（双岛时另一个岛在基址前 / 键页后）
        snprintf(b, n, "%s%s%#llx%s", m->name, off < 0 ? "-" : "+", (unsigned long long)(off < 0 ? -off : off), patch ? "（补丁区）" : "");
        return;
    }
    Dl_info di; if (dladdr((void *)a, &di) && di.dli_sname) { snprintf(b, n, "%s+%#llx（宿主）", di.dli_sname, (unsigned long long)(a - (uint64_t)di.dli_saddr)); return; }
    snprintf(b, n, "%#llx", (unsigned long long)a);
}
void s2a_where(uint64_t a, char *b, size_t n) { where(a, b, n); }
// 读一帧 {上一帧 fp, 返回地址}：经内核读，帧指针坏了（未映射 / 不可读）回 0，不在崩溃处理里二次缺页（二次缺页 = 处理函数自己 _exit，后面现场全丢）。信号里可调
static int frame_read(uint64_t fp, uint64_t rec[2]) {
#if defined(__APPLE__)
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)fp, 16, (vm_address_t)rec, &got) == KERN_SUCCESS && got == 16;
#else
    struct iovec l = {rec, 16}, r = {(void *)(uintptr_t)fp, 16};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == 16;
#endif
}
// 帧指针链 → 返回地址（至多 max 个）：8 对齐、逐帧往高处走、每帧经 frame_read 读、返回地址非 0
static int s2a_frames(uint64_t fp, uint64_t *lr, int max) {
    int k = 0; uint64_t rec[2];
    while (k < max && fp && !(fp & 7) && frame_read(fp, rec) && rec[1]) { lr[k++] = rec[1]; if (rec[0] <= fp) break; fp = rec[0]; }   // 返回地址 0 = 链尾（不可读页经内核读出全 0 也落在这里）
    return k;
}
int s2a_frames_of(uint64_t fp, uint64_t *lr, int max) { return s2a_frames(fp, lr, max); }   // 别处取帧链用（只读内存、不分配、不取锁：可在挂住别的线程时调）
// 指令原文（#368：没有原件的款崩在游戏代码里，日志只有地址，定不了空指针是哪次调用的返回值、写的哪个字段）：
//   a 前 before 条、后 after 条 32 位指令字（内存里的，即改写后真正跑的那份），a 那条用 [ ] 标出；经内核读（同 frame_read），不可读写「读不到」。
//   离线用任意 AArch64 反汇编器就能读，不需要游戏原件；每处几十字节，不是游戏内容。信号里可调（不分配、不取锁）
void s2a_code_words(uint64_t a, int before, int after, char *b, size_t n) {
    uint32_t w[32]; int k = before + after + 1; size_t o = 0; b[0] = 0;
    if (before < 0 || after < 0 || k > 32 || (a & 3)) { snprintf(b, n, "（参数不对）"); return; }
    uint64_t from = a - 4ull * (uint64_t)before;
#if defined(__APPLE__)
    vm_size_t got = 0;
    int ok = vm_read_overwrite(mach_task_self(), (vm_address_t)from, 4 * (vm_size_t)k, (vm_address_t)w, &got) == KERN_SUCCESS && got == 4 * (vm_size_t)k;
#else
    struct iovec l = {w, 4 * (size_t)k}, r = {(void *)(uintptr_t)from, 4 * (size_t)k};
    int ok = process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)(4 * k);
#endif
    if (!ok) { snprintf(b, n, "读不到"); return; }
    for (int i = 0; i < k && o + 12 < n; i++)
        o += (size_t)snprintf(b + o, n - o, i == before ? "%s[%08x]" : "%s%08x", i ? " " : "", w[i]);
}
// 调用点 → 被调的是谁（11-13 诊断#59429a：游戏刚调完一个函数就拿返回值读 +0x18 崩了，原先只列帧链里的调用点、漏了 lr 那一次，
//   而且只有指令字，看不出 bl 进的 PLT 是哪个导入、绑到宿主接管还是 sdk）。site 那条是 bl：目标是 PLT 桩（adrp x16 / ldr x17, [x16, #o]）时
//   回 GOT 槽地址（*slot）；目标本身也回。不是 bl 回 0。只经内核读，不二次缺页；信号里可调
static int mem_read(uint64_t a, void *out, size_t n) {
#if defined(__APPLE__)
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)a, (vm_size_t)n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n;
#else
    struct iovec l = {out, n}, r = {(void *)(uintptr_t)a, n};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
#endif
}
uint64_t s2a_call_target(uint64_t site, uint64_t *slot) {
    uint32_t w, st[2]; *slot = 0;
    if ((site & 3) || !mem_read(site, &w, 4) || (w & 0xFC000000u) != 0x94000000u) return 0;
    uint64_t t = site + (uint64_t)((int64_t)((int32_t)(w << 6) >> 6) * 4);
    if (mem_read(t, st, 8) && (st[0] & 0x9F00001Fu) == 0x90000010u && (st[1] & 0xFFC003FFu) == 0xF9400211u) {
        int64_t pg = (int64_t)((((uint64_t)(st[0] >> 5) & 0x7FFFF) << 2) | ((st[0] >> 29) & 3));
        pg = (pg << 43) >> 43;   // 21 位有符号页数
        *slot = (t & ~0xFFFull) + (uint64_t)(pg * 4096) + ((st[1] >> 10) & 0xFFF) * 8;
    }
    return t;
}
// GOT 槽 → 导入名：在含它的游戏模块的 RELA / JMPREL 里找 r_offset 落在这个槽的那条（装载器 relocate 同一张表，装完一直留在模块里）。找不到回 NULL
const char *s2a_import_name(uint64_t slot) {
    for (int i = 0; i < g_nmod; i++) {
        const module_t *m = &g_mod[i];
        if (!m->dyn || !m->symtab || !m->strtab || slot < (uint64_t)m->base || slot >= (uint64_t)m->base + m->size) continue;
        uint64_t tab[2] = {0}, sz[2] = {0};
        for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
            case 7: tab[0] = d->d_un.d_ptr; break; case 8: sz[0] = d->d_un.d_val; break;     // DT_RELA / DT_RELASZ
            case 23: tab[1] = d->d_un.d_ptr; break; case 2: sz[1] = d->d_un.d_val; break;    // DT_JMPREL / DT_PLTRELSZ
        }
        for (int t = 0; t < 2; t++) {
            const Elf64_Rela *r = (const Elf64_Rela *)(m->base + tab[t]);
            for (uint64_t k = 0; tab[t] && k < sz[t] / sizeof *r; k++)
                if ((uint64_t)m->base + r[k].r_offset == slot) { uint32_t si = (uint32_t)(r[k].r_info >> 32); return si ? m->strtab + m->symtab[si].st_name : NULL; }
        }
        return NULL;
    }
    return NULL;
}
#if defined(__APPLE__)
// 一处调用点的「被调的是谁」：「 → 导入 <名>，绑到 <宿主符号 / 模块+偏移>」或「 → 调 <模块+偏移>」；不是 bl 写空串
static void callee_text(uint64_t site, char *b, size_t n) {
    uint64_t slot, t = s2a_call_target(site, &slot), v = 0; char at[160]; b[0] = 0;
    if (!t) return;
    if (slot && mem_read(slot, &v, 8)) {
        const char *nm = s2a_import_name(slot); where(v, at, sizeof at);
        snprintf(b, n, " → 导入 %s，绑到 %s", nm ? nm : "（槽不在重定位表里）", at);
    } else { where(t, at, sizeof at); snprintf(b, n, " → 调 %s", at); }
}
// 崩溃完整报告里的指令原文（只 Mac / iOS 的完整报告调；安卓走 crash_android_fatal.inc）：pc 前 8 条后 2 条；
//   lr 那次调用（崩溃函数里最近一次调用，帧链里没有它：返回值是空的多半就是它）、帧链里落在游戏模块的前 8 个返回地址，各取调用那条（返回地址 −4）前 3 条到它本身，并写被调的是谁
static void code_note(uint64_t pc, uint64_t lr0, uint64_t fp) {
    char b[400], at[160], ce[400];
    where(pc, at, sizeof at); s2a_code_words(pc & ~3ull, 8, 2, b, sizeof b); LOG("指令 pc %s：%s", at, b);
    uint64_t lr[32]; int k = s2a_frames(fp, lr, 32), shown = 0;
    if (module_of((void *)(uintptr_t)lr0) && (k == 0 || lr[0] != lr0)) {
        where(lr0 - 4, at, sizeof at); s2a_code_words((lr0 - 4) & ~3ull, 3, 0, b, sizeof b); callee_text((lr0 - 4) & ~3ull, ce, sizeof ce);
        LOG("指令 lr 调用点 %s：%s%s", at, b, ce);
    }
    for (int i = 0; i < k && shown < 8; i++) {
        if (!module_of((void *)(uintptr_t)lr[i])) continue;
        where(lr[i] - 4, at, sizeof at); s2a_code_words((lr[i] - 4) & ~3ull, 3, 0, b, sizeof b); callee_text((lr[i] - 4) & ~3ull, ce, sizeof ce);
        LOG("指令 #%d 调用点 %s：%s%s", i, at, b, ce); shown++;
    }
}
#endif
void s2a_backtrace(uint64_t fp) {
    uint64_t lr[32]; int k = s2a_frames(fp, lr, 32);
    for (int i = 0; i < k; i++) { char b[160]; where(lr[i], b, sizeof b); LOG("  #%d %s", i, b); }
}
void s2a_thread_dump(void);
#include "../../graphics/track/s2a_track.h"
#if (defined(__ANDROID__) || defined(__OHOS__))
// The existing dispatcher remains the only owner. Managed/ART handlers may
// subsequently chain to it, so readiness is proved by fault_probe after managed
// attachment, never by comparing the top-level sigaction function pointer.
static atomic_int android_installed;
static pthread_once_t android_install_once = PTHREAD_ONCE_INIT;
static int android_install_error;
#include "crash_android_fatal.inc"
// SIGABRT 现场：fatal 行的 pc 通常落在 libc 的 abort/raise 里，guest 调用方只能看 lr。
// 这里在链回旧处置之前额外落一行（同一 fd），带 pc/lr/sp/fp/x0 与 pc/lr 是否在 guest 模块里；
// guest=1 说明 abort 由重编译的 guest 代码直接调用（游戏自己 assert），0 说明来自宿主/libc。
// 全部用 s2a_w_* 有界拼接 + 一次 write，与 fatal 行同一异步信号安全口径。
static void android_sigabrt_ctx(const ucontext_t *context) {
#if defined(__aarch64__)
    const mcontext_t *m = &context->uc_mcontext;
    char message[256];
    s2a_w_t w = {message, 0, sizeof message};
    s2a_w_raw(&w, "[s2a] Android SIGABRT ctx pc="); s2a_w_hex(&w, (uint64_t)m->pc);
    s2a_w_raw(&w, " lr="); s2a_w_hex(&w, (uint64_t)m->regs[30]);
    s2a_w_raw(&w, " sp="); s2a_w_hex(&w, (uint64_t)m->sp);
    s2a_w_raw(&w, " fp="); s2a_w_hex(&w, (uint64_t)m->regs[29]);
    s2a_w_raw(&w, " x0="); s2a_w_hex(&w, (uint64_t)m->regs[0]);
    s2a_w_raw(&w, " guest_pc="); s2a_w_dec(&w, module_of((void *)(uintptr_t)m->pc) != NULL);
    s2a_w_raw(&w, " guest_lr="); s2a_w_dec(&w, module_of((void *)(uintptr_t)m->regs[30]) != NULL);
    s2a_w_ch(&w, '\n');
    int flags = fcntl(g_s2a_logfd, F_GETFL);
    if (flags >= 0 && !fcntl(g_s2a_logfd, F_SETFL, flags | O_NONBLOCK)) {
        ssize_t ignored = write(g_s2a_logfd, message, w.n); (void)ignored;
    }
#else
    (void)context;
#endif
}
// SIGABRT 栈扫描：abort 的帧指针链常在 libc 里断掉（chain=broken@0），看不出谁调的 abort。
// 这里把 sp 起 4 KB 栈经内核读出，逐字找落在宿主库 / guest 模块代码里的值，按「库名+相对库基址偏移」
// 打印（离线 addr2line 可还原），至多 24 条。只在进程已决定死亡时跑，不追求异步信号安全。
static void android_sigabrt_scan(const ucontext_t *context) {
#if defined(__aarch64__)
    uint64_t buf[512];
    struct iovec l = {buf, sizeof buf}, r = {(void *)(uintptr_t)context->uc_mcontext.sp, sizeof buf};
    ssize_t got = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    if (got <= 0) return;
    int shown = 0;
    for (size_t i = 0; i < (size_t)got / 8 && shown < 24; i++) {
        uint64_t v = buf[i];
        if (v < 0x10000) continue;
        char b[200];
        if (module_of((void *)(uintptr_t)v)) { where(v, b, sizeof b); LOG("[s2a] abort 栈扫描 sp+%#zx: %s", i * 8, b); shown++; continue; }
        Dl_info di;
        if (!dladdr((void *)(uintptr_t)v, &di) || !di.dli_fname || !di.dli_fbase) continue;
        const char *base = strrchr(di.dli_fname, '/'); base = base ? base + 1 : di.dli_fname;
        LOG("[s2a] abort 栈扫描 sp+%#zx: %s+%#llx %s", i * 8, base, (unsigned long long)(v - (uint64_t)(uintptr_t)di.dli_fbase),
            di.dli_sname ? di.dli_sname : "");
        shown++;
    }
#else
    (void)context;
#endif
}
// android/native/direct/compile_request.c：崩溃线程正在现编时把那份请求落盘（#1501 D②，同苹果侧 s2a_dsh_crash_dump）。
//   链接期弱引用（安卓单库导出被 --exclude-libs 藏起，dlsym(RTLD_DEFAULT) 取不到）；只链运行时的单测没有它 = 空。
extern void s2a_compile_inflight_dump(void) __attribute__((weak));
static void android_crash_fatal(int sig, siginfo_t *si, const ucontext_t *context) {
    if (sig == SIGABRT) { android_sigabrt_ctx(context); android_sigabrt_scan(context); }
    if (s2a_compile_inflight_dump) s2a_compile_inflight_dump();   // 只 open / write / close；排在 fatal 行与链回之前（链回后可能不再回来）
    s2a_android_fatal(sig, si, context->uc_mcontext.pc, context->uc_mcontext.sp, (void *)context);
}
static _Atomic uint64_t g_sp_unaligned;   // 替游戏模拟的「栈指针未对齐」读写条数（收局汇总报，s2a_sp_unaligned_count；同苹果段）
uint64_t s2a_sp_unaligned_count(void) { return atomic_load(&g_sp_unaligned); }
static void on_crash(int sig, siginfo_t *si, void *context) {
    ucontext_t *u = context;
    // 双补丁岛 brk 兜底点（traps.c）：两岛都够不着的必改点，按原语义模拟后 pc += 4
    if (sig == SIGTRAP && s2a_trap_brk && s2a_trap_brk(context)) return;
    // 栈指针未对齐（ESR 类别 0x26）：Switch 不查、安卓内核查，替游戏做这一条读写（emul_sp.c 安卓段）。范围同苹果：
    //   游戏代码（含补丁区）+ 系统调用门进出两头（entry.S [sw_gate, sw_gate_end) 用客体 sp 借 32 字节）
    if (sig == SIGBUS) {
        unsigned ec;
        if (s2a_android_fault_ec(context, &ec) && ec == 0x26) {
            int s2a_emul_sp_unaligned(void *); extern void sw_gate(void); extern char sw_gate_end[];
            uint64_t pc0 = (uint64_t)u->uc_mcontext.pc;
            int gate = pc0 >= (uint64_t)(uintptr_t)sw_gate && pc0 < (uint64_t)(uintptr_t)sw_gate_end;
            if ((gate || module_of((void *)(uintptr_t)pc0)) && s2a_emul_sp_unaligned(context)) {
                atomic_fetch_add_explicit(&g_sp_unaligned, 1, memory_order_relaxed); return;
            }
        }
    }
    if (sig == SIGBUS || sig == SIGSEGV) {
        int write;
        if (s2a_android_fault_esr(context, &write)) {
            const int direct = s2a_direct_fault(si->si_addr, write, context);
            if (direct == 1) return;
            if (direct < 0) {
                android_crash_fatal(sig, si, u);
                return;
            }
        }
    }
    int guest = (sig == SIGBUS || sig == SIGSEGV) && module_of((void *)(uintptr_t)u->uc_mcontext.pc) != NULL;
    int handled = s2a_android_fault_handle(sig, si->si_addr, context, guest);
    if (handled == 1) return;
    // Zero is unclaimed; negative means a required readback could not safely
    // run. Neither may fall back to tracker unprotect and return to guest.
    android_crash_fatal(sig, si, u);
}
static void android_install(void) {
    s2a_android_fault_dispatch_reset();
    for (size_t i = 0; i < S2A_ANDROID_FATAL_SIGNAL_COUNT; ++i) {
        s2a_android_fatal_remember(s2a_android_fatal_signals[i], on_crash);
        int error = plat_fault_install(s2a_android_fatal_signals[i], on_crash, PLAT_FAULT_ONSTACK);
        if (error) { android_install_error = error; return; }
    }
    atomic_store_explicit(&android_installed, 1, memory_order_release);
}
int s2a_android_fault_dispatch_installed(void) {
    return atomic_load_explicit(&android_installed, memory_order_acquire);
}
void s2a_crash_thread_init(void) {
    int error = s2a_android_fault_thread_init();
    if (error) die("Android 缺页线程初始化失败 errno=%d", error);
}
void s2a_crash_install(void) {
    int error = pthread_once(&android_install_once, android_install);
    if (error || android_install_error) die("Android 信号安装失败 errno=%d", error ? error : android_install_error);
    s2a_crash_thread_init();
}
// Android currently uses the full-readback path. The Mach context instruction
// emulation benchmark is not an Android capability or readiness check.
void s2a_emul_store_bench(void) { LOG("Android 使用完整读回；未运行 Mach 指令模拟 benchmark"); }
#else
#include "s2a_emul_st.h"
#include <mach/mach_time.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include "s2a_emul_ld.h"
// 门的起止（entry.S）：弱引用放文件级——只链 crash.c 的单测没有 entry.S，强引用会让程序一加载就找不到符号；生产里两个符号都在
__attribute__((weak_import)) extern void sw_gate(void);
__attribute__((weak_import)) extern char sw_gate_end[];
// 拦写模拟自测（帧率能耗线 10-24，拦写模拟开着时（缺省开，S2A_TRACK_EMUL_STORE=0 关）起局跑一次）：一张只读测试页上连写 N 次，每次都「缺页 → 信号里经可写别名模拟 → 返回」，
// 总时长 / N = 每次缺页 + 模拟的全部成本（含内核投递信号与返回；写跟踪计数里的每次耗时不含这一段）。测试页的缺页在信号处理最前面认领，不进写跟踪
static _Atomic uint64_t g_esb_page;
static _Atomic uint64_t g_sp_unaligned;   // 替游戏模拟的「栈指针未对齐」读写条数（收局汇总报，s2a_sp_unaligned_count）
uint64_t s2a_sp_unaligned_count(void) { return atomic_load(&g_sp_unaligned); }
// 过期权限缺页（SIGBUS 写权限族：D-EKTW / D-ATFZ / D-QM9S / D-2V3R，四款四台，现场都是写权限错 ESR DFSC 0x0F、出事那块记录时却已 rw-）：
//   写跟踪类的钩子（nvn_dirty.c on_fault 只认还登记着的范围）没认领时，可能是这一页刚被别的线程合法放开——范围注销 / 内存池回收把页恢复可写、
//   同时摘掉登记，缺页处理晚了一步就找不到主人，按崩溃收了。翻译 / 权限类缺页（DFSC 0x04–0x0F；页刚从不可访问放开时是翻译错）且内核里这页此刻已有要的权限 → 重执行；
//   同一线程同一地址连续超过 8 次仍缺页就不再重试（不是过期，交回崩溃路径，免空转）
static _Atomic uint64_t g_stale_perm;
uint64_t s2a_stale_perm_count(void) { return atomic_load(&g_stale_perm); }
static __thread uint64_t t_stale_addr; static __thread unsigned t_stale_n;
static int stale_perm_retry(uint64_t fa, int w, uint32_t esr) {
    unsigned dfsc = esr & 0x3f; if (dfsc < 0x04 || dfsc > 0x0f) return 0;   // 只认翻译 / 访问标志 / 权限三类缺页（0x04–0x0F）；外部中止、对齐、TLB 冲突等不归这里
    vm_address_t a = (vm_address_t)(fa & ~(uint64_t)(vm_page_size - 1)); vm_size_t sz = 0; vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t c = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t o = MACH_PORT_NULL;
    if (vm_region_64(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &c, &o) != KERN_SUCCESS || a > (vm_address_t)fa) return 0;
    if (!(info.protection & (w ? VM_PROT_WRITE : VM_PROT_READ))) return 0;
    if (t_stale_addr == fa) { if (++t_stale_n > 8) { t_stale_n = 0; return 0; } } else { t_stale_addr = fa; t_stale_n = 1; }
    atomic_fetch_add_explicit(&g_stale_perm, 1, memory_order_relaxed); return 1;
}
void s2a_emul_store_bench(void) {
    const char *e = getenv("S2A_TRACK_EMUL_STORE"); if (e && *e == '0') return;
    uint8_t *p = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); if (p == MAP_FAILED) return;
    memset(p, 0, 0x4000); mprotect(p, 0x4000, PROT_READ); atomic_store(&g_esb_page, (uint64_t)(uintptr_t)p);
    enum { N = 2000 }; static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    uint64_t t0 = mach_absolute_time();
    for (int i = 0; i < N; i++) ((volatile uint32_t *)p)[(i * 17) & 0xFFF] = (uint32_t)i + 1;
    uint64_t ns = (mach_absolute_time() - t0) * tb.numer / tb.denom;
    atomic_store(&g_esb_page, 0);
    int ok = ((volatile uint32_t *)p)[((N - 1) * 17) & 0xFFF] == N;
    munmap(p, 0x4000);
    LOG("拦写模拟自测：每次缺页 + 模拟 %.2f µs（含信号投递与返回，%d 次%s）", ns / 1000.0 / N, N, ok ? "" : "；【红】写入值不对");
}
// 崩溃原始现场（两段式的第一段）：只用 write 与栈上缓冲——不取锁、不分配、不格式化浮点，崩在持日志锁 / malloc 锁的线程上也写得出来。
//   一行一项：信号、出错地址、pc / lr / sp / fp、x0–x28、帧链返回地址（经 frame_read 校验）、模块表（名、基址、大小、补丁区）——
//   离线按模块表把地址换成「模块+偏移」。第二段（下面的完整报告：符号、线程表、各种余量）尽力而为，在里面崩了这一段已经落盘
//   每行先拼进栈上一行缓冲、一次 write 落盘：同时崩的另一条线程也在写它的原始现场，按行整写才不会互相插断
typedef struct { char b[768]; size_t n; } raw_line;
static void raw_put(raw_line *l, const char *s) { while (*s && l->n < sizeof l->b - 1) l->b[l->n++] = *s++; }
static void raw_hex(raw_line *l, const char *label, uint64_t v) {
    static const char hx[] = "0123456789abcdef"; char b[24]; int k = 0; b[k++] = ' ';
    for (int sh = 60; sh >= 0; sh -= 4) b[k++] = hx[(v >> sh) & 15];
    b[k] = 0; raw_put(l, label); raw_put(l, b);
}
static void raw_end(raw_line *l) { l->b[l->n++] = '\n'; ssize_t w = write(g_s2a_logfd, l->b, l->n); (void)w; l->n = 0; }
static void crash_raw(int sig, siginfo_t *si, ucontext_t *uc, int other) {
    raw_line l = {.n = 0};
    raw_put(&l, other ? "[s2a] 【红】崩溃原始现场（另一条线程同时崩：只记这一段，等先崩的线程写完）" : "[s2a] 【红】崩溃原始现场");
    raw_hex(&l, " 信号", (uint64_t)sig); raw_hex(&l, " 地址", (uint64_t)(uintptr_t)si->si_addr);
    raw_hex(&l, " pc", uc->uc_mcontext->__ss.__pc); raw_hex(&l, " lr", uc->uc_mcontext->__ss.__lr); raw_hex(&l, " sp", uc->uc_mcontext->__ss.__sp); raw_hex(&l, " fp", uc->uc_mcontext->__ss.__fp); raw_end(&l);
    for (int i = 0; i < 29; i += 4) { raw_put(&l, "[s2a]   x"); for (int j = i; j < i + 4 && j < 29; j++) raw_hex(&l, "", uc->uc_mcontext->__ss.__x[j]); raw_end(&l); }
    uint64_t lr[32]; int k = s2a_frames(uc->uc_mcontext->__ss.__fp, lr, 32);
    raw_put(&l, "[s2a]   帧链"); for (int i = 0; i < k; i++) raw_hex(&l, "", lr[i]); raw_end(&l);
    if (other) return;   // 模块表先崩的那条已经列了
    for (int i = 0; i < g_nmod; i++) {
        char nm[17]; memcpy(nm, g_mod[i].name, 16); nm[16] = 0;
        raw_put(&l, "[s2a]   模块 "); raw_put(&l, nm); raw_hex(&l, " 基址", (uint64_t)(uintptr_t)g_mod[i].base); raw_hex(&l, " 大小", g_mod[i].size);
        raw_hex(&l, " 补丁区偏移", (uint64_t)g_mod[i].patch_off); raw_hex(&l, " 补丁区长", g_mod[i].patch_len);
        if (g_mod[i].patch2_len) { raw_hex(&l, " 补丁岛2偏移", (uint64_t)g_mod[i].patch2_off); raw_hex(&l, " 补丁岛2长", g_mod[i].patch2_len); }
        raw_end(&l);
    }
}
// 崩溃时寄存器指向的对象（#016fde：游戏对象成员里的「取对象」接口返回空、一用就读 0，崩溃现场只有寄存器，认不出成员指向哪个类）：
//   x19–x28（被调用者保存，多是 this / 外层对象）能读的地址往下 0x100 字节，每 8 字节：落在模块里写「模块+偏移」；是指针、它指向的首字在模块里 =
//   「对象 虚表 …」；再隔一层（指向的首字再指向的首字）在模块里写「→对象 虚表 …」。只经 mem_read 读（读不了跳过，不再缺页）
static void reg_objects(ucontext_t *uc) {
    for (int r = 19; r <= 28; r++) {
        uint64_t a = uc->uc_mcontext->__ss.__x[r], q[32];
        if (a < 0x10000 || !mem_read(a, q, sizeof q)) continue;
        char line[1536]; size_t at = (size_t)snprintf(line, sizeof line, "x%d %#llx：", r, (unsigned long long)a); int any = 0;
        for (int i = 0; i < 32 && at < sizeof line - 200; i++) {
            char w[160]; uint64_t v = q[i], v1 = 0, v2 = 0; const char *kind = NULL;
            if (v < 0x10000) continue;
            if (module_of((void *)(uintptr_t)v)) { where(v, w, sizeof w); kind = ""; }
            else if (mem_read(v, &v1, 8) && v1 >= 0x10000 && module_of((void *)(uintptr_t)v1)) { where(v1, w, sizeof w); kind = "对象 虚表 "; }
            else if (v1 >= 0x10000 && mem_read(v1, &v2, 8) && v2 >= 0x10000 && module_of((void *)(uintptr_t)v2)) { where(v2, w, sizeof w); kind = "→对象 虚表 "; }
            if (!kind) continue;
            at += (size_t)snprintf(line + at, sizeof line - at, " +%#x=%s%s", i * 8, kind, w); any = 1;
        }
        if (any) LOG("崩溃时寄存器指向：%s", line);
    }
}
// 出事那块内存（#523 / #815：游戏装完 NRO 后 SIGBUS 落在主程序 +0x579508，日志只有地址，定不了是只读还是没映射）：
//   缺页类信号（SIGBUS / SIGSEGV）打一行——读还是写（ESR 的 WnR 位）、地址所在（或之后第一块）映射的起止、当前 / 最大权限、标签、共享方式，块起点换成「模块+偏移」。
//   只用 Mach 调用和 LOG，不分配
// 11-14 D-G2JB：游戏的 SetHeapSize 因超出宿主地址窗口回了内存不足，它没理会、照样写堆首，崩出 SIGBUS——日志里只见总线错误。
//   出事地址落在最近一次地址越界的请求里时点名是它，指向起局「自检」行（窗口多大、有没有「提高内存上限」权限）
int s2a_va_short_last(uint64_t fa, int *num, uint64_t *a, uint64_t *n, uint64_t *count) __attribute__((weak));
uint64_t s2a_guest_x18_now(void) __attribute__((weak));   // thread.c
int s2a_phys_unmap_hit(uint64_t fa, uint64_t *a, uint64_t *n, uint64_t *back, uint64_t *ago_ms, uint64_t *total) __attribute__((weak));   // svc_mem.c
static void fault_region_note(int sig, siginfo_t *si, ucontext_t *uc) {
    if (sig != SIGBUS && sig != SIGSEGV) return;
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr, esr = uc->uc_mcontext->__es.__esr; const char *rw = (esr >> 6 & 1) ? "写" : "读";
    { int num; uint64_t a, n, k;
      if (s2a_va_short_last && s2a_va_short_last(fa, &num, &a, &n, &k))
          LOG("【红】崩在游戏没要到的地址上：svc %#x 早先要在 %#llx +%#llx 映射，超出宿主占到的地址窗口、回了内存不足（地址越界共 %llu 次），游戏没理会照样访问 %#llx。%s",
              num, (unsigned long long)a, (unsigned long long)n, (unsigned long long)k, (unsigned long long)fa,
              "地址窗口比游戏要的小，看起局「自检」行；手机上缺「提高内存上限」权限时按首启提示重签带权限的包"); }
    {   // 出事地址是游戏早先自己解除的物理映射（诊断#ff9a04）：命中 = 游戏拿着悬空指针；没命中最近 32 次 = 要么更早解除、要么从没映射 / 被我方弄丢
        uint64_t a = 0, n = 0, back = 0, ago = 0, total = 0;
        if (s2a_phys_unmap_hit && s2a_phys_unmap_hit(fa, &a, &n, &back, &ago, &total))
            LOG("出事地址 %#llx 在游戏自己解除的物理映射段里：倒数第 %llu 次 UnmapPhysicalMemory %#llx +%#llx（%llu ms 前；本局共解除 %llu 次）——游戏拿着已释放内存的指针",
                (unsigned long long)fa, (unsigned long long)back, (unsigned long long)a, (unsigned long long)n, (unsigned long long)ago, (unsigned long long)total);
        else if (s2a_phys_unmap_hit) LOG("出事地址 %#llx 不在最近 %llu 次 UnmapPhysicalMemory 的段里（本局共解除 %llu 次）", (unsigned long long)fa, (unsigned long long)(total < 32 ? total : 32), (unsigned long long)total); }
    // 游戏的 x18 在线程槽里（iOS 不许用 x18，补丁区转接代码从槽里取、用完写回）：补丁区里崩时出事指针常是它（诊断#ff9a04）
    if (s2a_guest_x18_now) LOG("游戏 x18（线程槽）= %#llx", (unsigned long long)s2a_guest_x18_now());
    vm_address_t r = (vm_address_t)fa; vm_size_t sz = 0; natural_t depth = 0; vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt;
    for (;;) {
        cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &r, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS) {
            LOG("出事那块内存：%s %#llx（ESR %#llx），这个地址及之后都没有映射", rw, (unsigned long long)fa, (unsigned long long)esr); return; }
        if (!info.is_submap) break;
        depth++;
    }
    char ws[160]; where((uint64_t)r, ws, sizeof ws);
    int p = info.protection, m = info.max_protection, in = fa >= (uint64_t)r && fa < (uint64_t)r + sz;
    LOG("出事那块内存：%s %#llx（ESR %#llx）%s [%#llx, %#llx) %llu KB，权限 当前 %c%c%c / 最大 %c%c%c，标签 %u，共享方式 %u，子映射深 %u；块起点 %s",
        rw, (unsigned long long)fa, (unsigned long long)esr, in ? "落在映射块" : "没有映射，之后第一块是", (unsigned long long)r, (unsigned long long)(r + sz), (unsigned long long)(sz >> 10),
        p & VM_PROT_READ ? 'r' : '-', p & VM_PROT_WRITE ? 'w' : '-', p & VM_PROT_EXECUTE ? 'x' : '-', m & VM_PROT_READ ? 'r' : '-', m & VM_PROT_WRITE ? 'w' : '-', m & VM_PROT_EXECUTE ? 'x' : '-',
        info.user_tag, info.share_mode, depth, ws);
}
// 写现场的线程（第一条崩的）。原先 `static int once; if (once++) _exit()`：第二条几乎同时崩的线程直接退出整个进程，
//   第一条的现场写到一半（10-29 1-2-Switch：host.log 断在一行中间、rc=138），且 once++ 不是原子的。现在：
//   第一条照旧写完两段再 _exit；它自己在第二段里又崩 = 立即 _exit（第一段已落盘，同原先）；
//   别的线程只写自己的原始现场（无锁、按行整写），然后停在这里等第一条 _exit，最多等 10 秒（第一条卡死时不让进程挂住）
static _Atomic(pthread_t) g_crash_owner;
static void crash_other_wait(int sig) {
    struct timespec t = {0, 50 * 1000 * 1000};
    for (int i = 0; i < 200; i++) nanosleep(&t, NULL);
    _exit(128 + sig);
}
// 现场写完后按默认处置重新抛一次同一信号（#141 玩家提议）：进程以「被信号结束」收场，macOS 的 ReportCrash 生成带完整回溯的 .ips
//   （控制台「崩溃报告」/ ~/Library/Logs/DiagnosticReports），iOS 进系统崩溃日志——原先 _exit(128 + sig) 是正常退出，系统不出报告。
//   处理函数里本信号是屏蔽的，先放开再抛；万一没结束（不该发生）照旧 _exit(128 + sig)。
//   读退出码的地方：shell 里被信号结束的 $? 仍是 128 + sig；作业层 Context.run 把 Python 的负返回码折回 128 + sig（pack/jobs/core.py）
static int g_crash_quiet;   // s2a_crash_install 时取（信号处理里不读环境）：native.h s2a_crash_quiet
__attribute__((weak)) uint64_t s2a_window_span_crash(uint64_t low_largest) { (void)low_largest; return 0; }   // vawin.c 强定义：低区碰底死的局记跨度（#1143）
// 崩溃落点（11-15 S 级优化「崩了自动关」归因）：<文档根>/crash_where 一行 game / patch / host——pc 或 lr 落在游戏模块（含补丁区）= game / patch，
//   两个都在宿主代码 = host。读的一方：Mac pack/jobs/cheats.py note_crash（收局后）、iOS ios/game_options.m（下次启动前），读完删。
//   宿主自己的崩溃第一次不替玩家关补丁（D-RRES：宿主贴图上传崩溃把自适应满屏关了，补丁没错，修好后也回不来）
static char g_where_path[1100];
static void crash_where_note(uint64_t pc, uint64_t lr) {
    if (!g_where_path[0] || g_crash_quiet) return;
    module_t *m = module_of((void *)pc), *ml = module_of((void *)lr);
    const char *w = m ? (s2a_in_patch(m, pc) ? "patch\n" : "game\n") : ml ? "game\n" : "host\n";
    int fd = open(g_where_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { (void)!write(fd, w, strlen(w)); close(fd); }
}
__attribute__((weak)) void s2a_guest_run_mark_crash(void) {}   // runtime/src/vawin.c 强定义：「在跑」标记改写成 crash（闪退不算死于内存，#909）
static void crash_reraise(int sig) {
    s2a_guest_run_mark_crash();
    if (g_crash_quiet) _exit(128 + sig);   // 辅助运行 / 静默起局：现场已写进日志，不按信号死（不弹系统「意外退出」框）
    signal(sig, SIG_DFL);
    sigset_t m; sigemptyset(&m); sigaddset(&m, sig); pthread_sigmask(SIG_UNBLOCK, &m, NULL);
    raise(sig);
    _exit(128 + sig);
}
// ---------- 写监视（S2A_WRITE_WATCH=<游戏地址>:<字节数>[,…]，#016fde；诊断，缺省关，至多 4 处）----------
// 对这些地址所在的 16 KB 页设只读；有人写到页内就缺页：写的地址落在监视区间（含前 8 字节，覆盖跨进区间的宽写）里就记一行
//   （第几次、线程、pc / lr 的模块偏移、区间里当前的 16 字节）并打回溯；不在区间只计数。随后放开这一页让这条写重执行，
//   后台线程约 1 ms 后重新设只读（窗口里别的写会漏，析构 / 重置这类少见写照抓）。页还没映射时后台每 1 ms 试一次。
//   用途：游戏堆分配确定的款，把「谁、在哪条指令上改了这个对象」直接抓出来（织梦岛 0x70f78af0c8 的包装对象被提前析构）
enum { WW_MAX = 4, WW_LOG = 32 };
// armed：0 = 放开着（可写），2 = 后台正在设只读，1 = 已设只读。缺页处理见到 1 或 2 都算「页拦着」（mprotect 已经或正在生效，这次缺页是真的）；
//   后台 0 → 2 → mprotect → 2 → 1，中途被缺页处理放开（置 0）就作废这一轮、下一轮重来——原先先 mprotect 再置 1，这一窗里的缺页按「别人刚放开」漏记
static struct { uint64_t a, n; _Atomic int armed, said; _Atomic uint64_t hits, other; } g_ww[WW_MAX]; static int g_nww;
// 放开后的重执行（11-24 门禁 t_crash ⑦ 多出「第 3 次被写」）：缺页处理放开这页、返回让这条写重执行，后台约 1 ms 后重新设只读；
//   负载高时这条线程被调度晚了，重执行前页已经又只读，同一次写再缺页一次，原先按新的一次记。每条线程记下自己最近一次放开的（pc、地址）：
//   下一次缺页还是同一 pc、同一地址，就是那次重执行——照样放开、不再计数不记行。表满（线程 > WW_TT）时照旧计数
enum { WW_TT = 64 };
static struct { _Atomic uint64_t tid; uint64_t pc, fa; } g_ww_t[WW_TT];
static int ww_tslot(uint64_t tid) {   // 本线程那一格（信号处理里用：无锁、不分配）；满了返回 -1
    for (int j = 0; j < WW_TT; j++) if (atomic_load(&g_ww_t[j].tid) == tid) return j;
    for (int j = 0; j < WW_TT; j++) { uint64_t z = 0; if (atomic_compare_exchange_strong(&g_ww_t[j].tid, &z, tid)) return j; }
    return -1;
}
static void *ww_thread(void *arg) {
    (void)arg; pthread_setname_np("s2a-write-watch");
    for (;;) {
        for (int i = 0; i < g_nww; i++) {
            int z = 0; if (!atomic_compare_exchange_strong(&g_ww[i].armed, &z, 2)) continue;
            uint64_t pg = g_ww[i].a & ~0x3FFFull; uint8_t b;
            if (!mem_read(pg, &b, 1) || mprotect((void *)(uintptr_t)pg, 0x4000, PROT_READ)) { int two = 2; atomic_compare_exchange_strong(&g_ww[i].armed, &two, 0); continue; }
            // 「已设只读」那行先写、再置 1：读 s2a_ww_armed 等到 1 的人（单测）这时一定已看得到这一行，不用另睡等日志落盘
            if (!atomic_load(&g_ww[i].said)) { atomic_store(&g_ww[i].said, 1); LOG("写监视：%#llx（%llu 字节）所在页已设只读", (unsigned long long)g_ww[i].a, (unsigned long long)g_ww[i].n); }
            int two = 2; atomic_compare_exchange_strong(&g_ww[i].armed, &two, 1);   // 这一轮中途被缺页处理放开（已是 0）就不改，下一轮重来
        }
        usleep(1000);
    }
    return NULL;
}
// 第 i 处监视的页此刻是否已设只读（单测读口：写之前等它为 1，不按墙钟睡 —— 11-19 原先睡 20～50 ms 当「已设好」，负载高时后台没跑到、写不缺页 → 假红）
int s2a_ww_armed(int i);
int s2a_ww_armed(int i) { return i >= 0 && i < g_nww ? atomic_load(&g_ww[i].armed) : -1; }
static void ww_start(void) {
    const char *e = getenv("S2A_WRITE_WATCH"); if (!e || !*e) return;
    for (const char *s = e; s && *s && g_nww < WW_MAX; ) {
        unsigned long long a = 0, n = 8; if (sscanf(s, "%llx:%llu", &a, &n) >= 1 && a) { g_ww[g_nww].a = a; g_ww[g_nww].n = n ? n : 8; g_nww++; }
        s = strchr(s, ','); if (s) s++;
    }
    pthread_t t; if (g_nww && !pthread_create(&t, NULL, ww_thread, NULL)) pthread_detach(t);
}
#ifdef S2A_UNIT_TEST
// 单测注入（t_crash ⑦b）：> 0 时，计了数的那次放开之后先在信号里等后台把页重新设只读、再返回（只一次）——即「放开后这条写还没来得及重执行，
//   后台已经重新设只读」，负载高时被调度晚了就是这样；重执行必然再缺页一次
_Atomic int g_ww_test_hold;
#endif
static int ww_fault(void *ctx, uint64_t fa, int w) {
    if (!g_nww || !w) return 0;
    for (int i = 0; i < g_nww; i++) {
        uint64_t pg = g_ww[i].a & ~0x3FFFull;
        if ((fa & ~0x3FFFull) != pg) continue;
        // 别的线程刚在这一页缺页、正在放开（armed 已清）：同样放开、重执行，不能当成「不是我的页」交给崩溃路径
        //   （iPhone 实测：被监视对象与对象池自旋锁同页，两条线程同时写，后到的一条 stlxr 落空成 SIGBUS）
        int counted = 0;
        uint64_t tid = 0, pc0 = ((ucontext_t *)ctx)->uc_mcontext->__ss.__pc; pthread_threadid_np(NULL, &tid);
        int ts = ww_tslot(tid), retry = ts >= 0 && g_ww_t[ts].pc == pc0 && g_ww_t[ts].fa == fa;   // 上一次放开的那条写在重执行
        if (ts >= 0) { g_ww_t[ts].pc = pc0; g_ww_t[ts].fa = fa; }
        if (!retry && atomic_load(&g_ww[i].armed) && fa + 8 > g_ww[i].a && fa < g_ww[i].a + g_ww[i].n) {
            uint64_t k = atomic_fetch_add(&g_ww[i].hits, 1) + 1; counted = 1;
            if (k <= WW_LOG) {
                ucontext_t *uc = ctx; uint64_t pc = uc->uc_mcontext->__ss.__pc, lr = uc->uc_mcontext->__ss.__lr, cur[2] = {0, 0};
                char bp[160], bl[160], tn[32] = ""; where(pc, bp, sizeof bp); where(lr, bl, sizeof bl); mem_read(g_ww[i].a, cur, sizeof cur); pthread_getname_np(pthread_self(), tn, sizeof tn);
                LOG("【红】写监视 %#llx 第 %llu 次被写：写 %#llx，线程 %s，pc %s lr %s；写前区间内 %016llx %016llx；回溯：", (unsigned long long)g_ww[i].a, (unsigned long long)k,
                    (unsigned long long)fa, tn[0] ? tn : "?", bp, bl, (unsigned long long)cur[0], (unsigned long long)cur[1]);
                s2a_backtrace(uc->uc_mcontext->__ss.__fp);
            }
        } else if (!retry) atomic_fetch_add(&g_ww[i].other, 1);
        atomic_store(&g_ww[i].armed, 0);
        mprotect((void *)(uintptr_t)pg, 0x4000, PROT_READ | PROT_WRITE);
#ifdef S2A_UNIT_TEST
        { int h = atomic_load(&g_ww_test_hold); if (counted && h > 0 && atomic_compare_exchange_strong(&g_ww_test_hold, &h, h - 1))
              for (int t = 0; t < 5000 && atomic_load(&g_ww[i].armed) != 1; t++) usleep(1000); }
#else
        (void)counted;
#endif
        return 1;
    }
    return 0;
}
static void (*g_dsh_crash_dump)(void);   // graphics/nvn/host/dsh_miss.inc s2a_dsh_crash_dump：现编时崩溃，落盘正在编的那支输入；装信号时 dlsym 取一次（信号处理里不调 dlsym），只链运行时的单测取不到 = 空
// 宿主代执行记录（11-21 王国之泪 main+0x8fe7d8：4 件玩家现场里 x8 / x22 / x25 与游戏指令流对不上，b30 之后只有读，
//   x22 却是 b34 之前的旧值——要分清是不是宿主在信号里改过这条线程的寄存器）。每线程最近 8 次「信号里替游戏做 / 改道」：
//   类别、pc、地址、指令，以及改了哪些通用寄存器（最多记 3 个：旧值 → 新值）。崩溃现场打崩的这条线程那几条。
//   预分配（线程初始化时，同 touch.c），信号处理里只读写、不分配；没分配到（非游戏线程）就不记
typedef struct { uint64_t t, pc, addr; uint32_t insn; uint8_t kind, nchg, reg[3]; uint64_t was[3], now[3]; } er_ev;
typedef struct { uint64_t r[32]; } er_regs;   // x0..x28、fp、lr、sp
typedef struct { uint32_t n; er_ev ev[8]; er_regs at3; int have3; } er_ring;   // at3：最近一次改道时的现场（改道还原要原样放回它）
static pthread_key_t g_erkey; static int g_erkey_ok;
static _Atomic uint64_t g_er_total[8];   // [7] = 起局拦写模拟自测（同一条信号路径，证明记录本身在工作；不进玩家现场的判断）   // 全进程按类别累计（崩溃报告里对照：本线程 0 次时，全进程有没有发生过）
__attribute__((constructor)) static void er_key_init(void) { g_erkey_ok = pthread_key_create(&g_erkey, free) == 0; }
static void er_thread_init(void) { if (g_erkey_ok && !pthread_getspecific(g_erkey)) pthread_setspecific(g_erkey, calloc(1, sizeof(er_ring))); }
static void er_snap(void *ctx, er_regs *o) {
    _STRUCT_ARM_THREAD_STATE64 *ss = &((ucontext_t *)ctx)->uc_mcontext->__ss;
    for (int k = 0; k < 29; k++) o->r[k] = ss->__x[k];
    o->r[29] = ss->__fp; o->r[30] = ss->__lr; o->r[31] = ss->__sp;
}
static void er_note(int kind, void *ctx, const er_regs *was, uint64_t pc, uint64_t addr) {
    if (kind > 0 && kind < 8) atomic_fetch_add_explicit(&g_er_total[kind], 1, memory_order_relaxed);
    if (kind == 7) return;   // 自测只计数，不占线程记录
    er_ring *g = g_erkey_ok ? pthread_getspecific(g_erkey) : NULL; if (!g) return;
    er_regs now; er_snap(ctx, &now);
    if (kind == 3) { g->at3 = *was; g->have3 = 1; }
    if (kind == 4 && g->have3) was = &g->at3;   // 改道还原：比的是「还原后」对「改道那一刻」，应 0 个不同；不为 0 = 还原错了
    er_ev *e = &g->ev[g->n++ % 8]; memset(e, 0, sizeof *e);
    e->t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); e->pc = pc; e->addr = addr; e->kind = (uint8_t)kind;
    e->insn = module_of((void *)pc) ? *(const uint32_t *)(uintptr_t)pc : 0;
    for (int k = 0; k < 32; k++) if (was->r[k] != now.r[k]) { if (e->nchg < 3) { e->reg[e->nchg] = (uint8_t)k; e->was[e->nchg] = was->r[k]; e->now[e->nchg] = now.r[k]; } if (e->nchg < 255) e->nchg++; }
}
static void er_totals(const char *when) {
    LOG("宿主代执行记录（全进程累计，%s）：读模拟 %llu、写模拟 %llu、改道回写 %llu、改道还原 %llu、远端代执行 %llu、栈未对齐代做 %llu（另：起局拦写自测 %llu 次，记录通路自证）", when,
        (unsigned long long)g_er_total[1], (unsigned long long)g_er_total[2], (unsigned long long)g_er_total[3],
        (unsigned long long)g_er_total[4], (unsigned long long)g_er_total[5], (unsigned long long)g_er_total[6], (unsigned long long)g_er_total[7]);
}
void s2a_er_totals_log(const char *when) { er_totals(when); }   // 收局也打一行（lifecycle.c），不靠崩溃
static void er_report(void) {
    er_totals("崩溃时");
    er_ring *g = g_erkey_ok ? pthread_getspecific(g_erkey) : NULL;
    if (!g) { LOG("宿主代执行记录：本线程没有（非游戏线程）"); return; }
    if (!g->n) { LOG("宿主代执行记录：本线程 0 次（这条线程没被宿主在信号里替做过读写 / 改道）"); return; }
    static const char *kn[] = {"?", "读模拟", "写模拟", "改道回写", "改道还原", "远端代执行", "栈未对齐代做"};
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    LOG("宿主代执行记录：本线程共 %u 次，最近 %u 次（新的在后；寄存器 29=fp 30=lr 31=sp）：", g->n, g->n < 8 ? g->n : 8);
    for (uint32_t i = g->n > 8 ? g->n - 8 : 0; i < g->n; i++) {
        er_ev *e = &g->ev[i % 8]; char w[160]; where(e->pc, w, sizeof w); char c[200] = ""; size_t o = 0;
        for (int k = 0; k < e->nchg && k < 3; k++) o += (size_t)snprintf(c + o, sizeof c - o, " x%d %llx→%llx", e->reg[k], (unsigned long long)e->was[k], (unsigned long long)e->now[k]);
        LOG("  %.3f 秒前 %s pc %s 指令 %08x 地址 %#llx 改了 %d 个寄存器%s", (double)(now - e->t) / 1e9, e->kind < 7 ? kn[e->kind] : "?", w, e->insn,
            (unsigned long long)e->addr, e->nchg, c);
    }
}
static void on_crash(int sig, siginfo_t *si, void *ctx) {
    // 先给 16 KB 写跟踪（graphics/track）认领：游戏 / 原生层写到被写保护的游戏页，跟踪模块记脏、放开后返回重执行那条指令。
    // 是否写取 ESR 的 WnR 位（数据中止 EC = 0x24 / 0x25 时有效）
    // 写跟踪 A 方案（touch.c）：跳板回写完的 brk——恢复缺页时的现场、重执行那条写
    { int s2a_touch_brk(void *); er_regs b; uint64_t p0 = ((ucontext_t *)ctx)->uc_mcontext->__ss.__pc; if (sig == SIGTRAP) er_snap(ctx, &b);
      if (sig == SIGTRAP && s2a_touch_brk(ctx)) { er_note(4, ctx, &b, ((ucontext_t *)ctx)->uc_mcontext->__ss.__pc, p0); return; } }
    // 诊断钩子（diag_brk.c，S2A_DIAG_DELAY / S2A_DIAG_STAMP，只 Mac）
    { int s2a_diag_brk(void *); if (sig == SIGTRAP && s2a_diag_brk(ctx)) return; }
    // 双补丁岛 brk 兜底点（traps.c）：两岛都够不着的必改点，按原语义模拟后 pc += 4
    if (sig == SIGTRAP && s2a_trap_brk && s2a_trap_brk(ctx)) return;
    if (sig == SIGBUS || sig == SIGSEGV) {
        uint32_t esr = ((ucontext_t *)ctx)->uc_mcontext->__es.__esr, ec = esr >> 26;
        // 栈指针未对齐（EC 0x26）：Switch 不查、苹果查，替游戏做这一条读写（emul_sp.c）。范围 = 游戏代码（含补丁区）+ 系统调用门进出两头
        //   （门用客体 sp 借 32 字节，entry.S [sw_gate, sw_gate_end)）；宿主自己的代码不会让 sp 不对齐，不在此列
        { int s2a_emul_sp_unaligned(void *); er_regs sb;
          uint64_t pc0 = ((ucontext_t *)ctx)->uc_mcontext->__ss.__pc;
          int gate = pc0 >= (uint64_t)(uintptr_t)sw_gate && pc0 < (uint64_t)(uintptr_t)sw_gate_end;
          if (ec == 0x26 && (gate || module_of((void *)pc0)) && (er_snap(ctx, &sb), s2a_emul_sp_unaligned(ctx))) { er_note(6, ctx, &sb, pc0, (uint64_t)si->si_addr); atomic_fetch_add_explicit(&g_sp_unaligned, 1, memory_order_relaxed); return; } }
        if (ec == 0x24 || ec == 0x25) {
            int w = (int)(esr >> 6 & 1); uint64_t pc = ((ucontext_t *)ctx)->uc_mcontext->__ss.__pc;
            if (ww_fault(ctx, (uint64_t)si->si_addr, w)) return;   // 写监视（S2A_WRITE_WATCH，诊断）：记下写的人后放开重执行
            { uint64_t bp = atomic_load(&g_esb_page), fa0 = (uint64_t)si->si_addr;   // 拦写模拟自测页（见 s2a_emul_store_bench）
              if (bp && w && (fa0 & ~0x3FFFull) == bp) { uint8_t *wa = s2a_alias_page_rw(bp); er_regs b; er_snap(ctx, &b); if (wa && s2a_emul_store(ctx, fa0, wa + (fa0 & 0x3FFF), NULL, NULL, NULL)) { er_note(7, ctx, &b, pc, fa0); return; } } }
            // 远端（far.c，乙布局）：别名区伸出宿主窗口那段一碰即缺页，在影子上代执行这条访存并记点
            { int s2a_far_fault(void *, uint64_t, int); er_regs b; er_snap(ctx, &b); if (s2a_far_fault(ctx, (uint64_t)si->si_addr, w)) { er_note(5, ctx, &b, pc, (uint64_t)si->si_addr); return; } }
            // 直连模式的缺页钩子（双轨，s2a_direct_host.h）：游戏代码 / 宿主代码两条分支都最先问；通用模式下没人登记，这里只读一个空指针
            const int direct = s2a_direct_fault(si->si_addr, w, ctx);
            if (direct == 1) return;
            if (direct < 0) goto report_fatal;
            // 游戏代码（模块原始代码 / 补丁区）写到 GPU 改过的页：改道到正常上下文先回写；宿主代码的缺页照原路径
            if (module_of((void *)pc)) {
                // 方案 B（帧率能耗线 10-23）：游戏读到读时回写的不可读页、碰的子页不是 GPU 改过的 → 从别名模拟这条读、pc 前进，页保持不可读（不整页回写，免 16 KB / 4 KB 假共享）。
                // 模拟不了（写、独占、原子、越页、别名建不出 / 起局自检不过时 s2a_alias_page 回 NULL）落到下面原路径
                uint64_t fa = (uint64_t)si->si_addr;
                if (!w && s2a_track_read_emulable(si->si_addr)) {
                    const uint8_t *al = s2a_alias_page(fa & ~0x3FFFull);
                    er_regs b; er_snap(ctx, &b);
                    if (al && s2a_emul_load(ctx, fa, al + (fa & 0x3FFF), s2a_track_emul_ok)) { er_note(1, ctx, &b, pc, fa); s2a_track_note_emul(); return; }
                    // 模拟不了：记类别与 pc（写跟踪每 10 秒一行报，缺页处模拟的覆盖率；al 为空记类别 -1）
                    s2a_track_note_emul_fail(al ? s2a_emul_class(*(const uint32_t *)(uintptr_t)pc) : -1, *(const uint32_t *)(uintptr_t)pc, pc);
                }
                // 拦写模拟（帧率能耗线 10-24，docs/研究/拦写模拟.md；缺省开，S2A_TRACK_EMUL_STORE=0 关）：游戏写到缓冲标的 GPU 改过子页、
                // 写的字节不碰 GPU 改过的字节 → 经可写别名替它写（同宽原子存储）、记「CPU 写过」位图、pc 前进，页保持拦截，不回写不等 GPU。
                // 任一条件不满足（store_ok 里判）→ 落到下面原路径（改道回写 → 重执行）
                er_regs eb;
                if (w && s2a_track_store_try(si->si_addr)) {
                    uint64_t t0 = mach_absolute_time(), ea = 0, len = 0;
                    uint8_t *wa = s2a_alias_page_rw(fa & ~0x3FFFull);
                    if (!wa) s2a_track_note_store_fail(6);
                    else if (er_snap(ctx, &eb), s2a_emul_store(ctx, fa, wa + (fa & 0x3FFF), s2a_track_store_ok, &ea, &len)) {
                        er_note(2, ctx, &eb, pc, fa);
                        static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
                        s2a_track_store_done(ea, len, (mach_absolute_time() - t0) * tb.numer / tb.denom); return; }
                    else if (!len) s2a_track_note_store_fail(7);   // 解码不认（认得但被 store_ok 拒的，原因已在 store_ok 里记）
                }
                uint64_t s2a_touch_skip(void); void s2a_touch_clear(void); int s2a_touch_redirect(void *, uint64_t);
                int r = s2a_track_fault_guest(si->si_addr, w, s2a_touch_skip());
                if (r == 2) { er_regs b; er_snap(ctx, &b); if (s2a_touch_redirect(ctx, (uint64_t)si->si_addr)) { er_note(3, ctx, &b, pc, (uint64_t)si->si_addr); return; } r = s2a_track_fault(si->si_addr, w); }
                s2a_touch_clear();
                if (r) return;
            } else if (s2a_track_fault(si->si_addr, w)) return;
        }
        { int s2a_det_watch_fault(uint64_t, uint64_t, uint64_t, uint64_t); ucontext_t *u = ctx;   // 确定性诊断写监视（S2A_DET_WATCH）
          if ((esr >> 6 & 1) && s2a_det_watch_fault((uint64_t)si->si_addr, u->uc_mcontext->__ss.__pc, u->uc_mcontext->__ss.__lr, u->uc_mcontext->__ss.__fp)) return; }
        if ((ec == 0x24 || ec == 0x25) && stale_perm_retry((uint64_t)si->si_addr, (int)(esr >> 6 & 1), esr)) return;   // 过期权限缺页：页已被别的线程放开，重执行（见 stale_perm_retry）
    }
report_fatal:;
    ucontext_t *uc = ctx; pthread_t self = pthread_self(), none = NULL;
    if (!atomic_compare_exchange_strong(&g_crash_owner, &none, self)) {
        if (pthread_equal(none, self)) _exit(128 + sig);   // 先崩的线程在写报告时自己又崩
        crash_raw(sig, si, uc, 1); crash_other_wait(sig);
    }
    uint64_t pc = uc->uc_mcontext->__ss.__pc, lr = uc->uc_mcontext->__ss.__lr, fp = uc->uc_mcontext->__ss.__fp;
    crash_where_note(pc, lr);
    crash_raw(sig, si, uc, 0);
    // 崩在现编着色器里（graphics/nvn/host/dsh_miss.inc，#467）：把正在编的那支输入落盘成 <键>.inflight.*，诊断包带走。只用 open / write，排在原始现场之后、完整报告之前
    { if (g_dsh_crash_dump) g_dsh_crash_dump(); }
    // 直连安全网：这局算失败（有证据）；动态取，单测没链 dguard.c。先按现场归因：出事地址落在游戏早先要、宿主给不出的那段地址里（超出地址窗口、svc 回了内存不足，
    //   游戏没理会照样访问，同 fault_region_note）= 已知不是直连——不计连续失败，下次起局说「这台设备内存不够」，不叫玩家更新重出包（D-GYNE：3 次都这么崩，误弹「更新后重新出包」）
    { int va = 0; if ((sig == SIGBUS || sig == SIGSEGV) && s2a_va_short_last) { int num; uint64_t a, n, k; va = s2a_va_short_last((uint64_t)(uintptr_t)si->si_addr, &num, &a, &n, &k); }
      if (va) { void (*kn)(const char *) = (void (*)(const char *))dlsym(RTLD_DEFAULT, "s2a_dguard_known"); if (kn) kn("地址超出窗口"); }
      else { void (*ev)(const char *, int) = (void (*)(const char *, int))dlsym(RTLD_DEFAULT, "s2a_dguard_evidence"); if (ev) ev("崩溃", 0); } }
    char bp[160], bl[160]; where(pc, bp, sizeof bp); where(lr, bl, sizeof bl);
    LOG("【红】CRASH 信号 %d 地址 %p pc %s lr %s sp %#llx", sig, si->si_addr, bp, bl, (unsigned long long)uc->uc_mcontext->__ss.__sp);
    { const char *(*wn)(void) = (const char *(*)(void))dlsym(RTLD_DEFAULT, "s2a_waived_note"); const char *w = wn ? wn() : NULL;   // waived.c
      if (w && *w) LOG("【红】之前这些校验没过（照常继续了）：%s", w); }
    for (int i = 0; i < 29; i += 4) LOG("  x%-2d %016llx %016llx %016llx %016llx", i, uc->uc_mcontext->__ss.__x[i], uc->uc_mcontext->__ss.__x[i + 1], i + 2 < 29 ? uc->uc_mcontext->__ss.__x[i + 2] : 0, i + 3 < 29 ? uc->uc_mcontext->__ss.__x[i + 3] : 0);
    er_report();
    s2a_backtrace(fp);
    code_note(pc, lr, fp);
    fault_region_note(sig, si, uc);
    reg_objects(uc);
    { uint64_t ht, hl, fs, fh; int fn; s2a_low_holes(&ht, &hl); s2a_fd_usage(&fn, &fs, &fh);   // 资源耗尽类崩溃先看这两样：低区地址 / 文件句柄
      LOG("崩溃时余量：低区空洞合计 %llu MB、最大一块 %llu MB；文件句柄已开 %d 个，软上限 %llu、硬上限 %llu", (unsigned long long)(ht >> 20), (unsigned long long)(hl >> 20), fn, (unsigned long long)fs, (unsigned long long)fh);
      uint64_t sp = s2a_window_span_crash(hl);   // 低区碰底死的局记下已碰到的跨度（vawin.c，#1143）
      if (sp) LOG("低区碰底：本局已碰到的地址跨度 %llu MB 记为本款学到的跨度，下一局窗口按它缩、其余给宿主", (unsigned long long)(sp >> 20)); }
    s2a_lowva_crash_note();   // 低区账（各家账面、保留量、拦不到的）
    { void s2a_vm_low(const char *); s2a_vm_low("崩溃时"); }   // 低区地址耗尽类崩溃（Metal / spirv_cross / 暂存缓冲）看当时谁占着
    { void s2a_guest_va_report(const char *); s2a_guest_va_report("崩溃时"); }   // 窗口里游戏真映射了多少、落在哪
    { void s2a_diag_stamp_report(void), s2a_lockwake_report(void); s2a_diag_stamp_report(); s2a_lockwake_report(); }
    { void s2a_svc_count_report(const char *); s2a_svc_count_report("崩溃"); }   // 诊断打点（S2A_DIAG_STAMP 设了才打）
    { void s2a_gate_ipc_recent(const char *); s2a_gate_ipc_recent("崩溃线程"); }   // 崩的这条线程最近 16 次 IPC（服务名 / 命令 / 结果，gate.c）
    LOG("线程表："); s2a_thread_dump();
    { void s2a_direct_wait_dump(void); s2a_direct_wait_dump(); }   // 直连模式：各等待点（s2a_crash_wait_dump_register 登记）
    { int s2a_dump_threads(const char *why); char w[64]; snprintf(w, sizeof w, "崩溃 信号 %d", sig); s2a_dump_threads(w); }
    void s2a_svc_red_summary(void); s2a_svc_red_summary();
    s2a_logcon_flush(); crash_reraise(sig);
}
// 每线程一份信号备用栈：游戏栈可能只有几 KiB，崩在上面时处理函数不能再往游戏栈压。内存从窗口尾部取（10-24 低区让给 GC；尾部放不下平台层自己 mmap）
static void *alt_tail_alloc(size_t n) { return s2a_tail_map(n, S2A_TAIL_ALTSTACK | S2A_TAIL_ONLY, PROT_READ | PROT_WRITE); }
static void alt_tail_release(void *p, size_t n) { s2a_tail_unmap(p, n, S2A_TAIL_ALTSTACK); }
void s2a_crash_thread_init(void) {
    plat_fault_altstack_set_mem(alt_tail_alloc, alt_tail_release);
    { void s2a_touch_thread_init(void); s2a_touch_thread_init(); }   // 写跟踪 A 方案的每线程保存区（信号处理里不分配）
    er_thread_init();   // 宿主代执行记录（信号里只记不分配）
    // 缺页处模拟读的只读别名槽（alias.c，每线程 16 KB）
    s2a_alias_thread_init();
    int e = plat_fault_altstack_init();
    if (e && e != ENOTSUP) LOG("【红】崩溃：备用栈安装失败 errno=%d", e);   // ENOTSUP = tvOS 没有备用栈（platform/apple/plat_apple.c），不算故障
}
void s2a_crash_install(void) {
    s2a_docs_path(g_where_path, sizeof g_where_path, "crash_where");
    int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGABRT};   // SIGABRT：die() 与 C# FailFast 都走 abort，要现场
    g_dsh_crash_dump = (void (*)(void))dlsym(RTLD_DEFAULT, "s2a_dsh_crash_dump");
    g_crash_quiet = s2a_crash_quiet();
    ww_start();
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) {
        int e = plat_fault_install(sigs[i], on_crash, PLAT_FAULT_ONSTACK);
        if (e) LOG("【红】崩溃：安装信号 %d 失败 errno=%d", sigs[i], e);
    }
    s2a_crash_thread_init();
}
#endif
