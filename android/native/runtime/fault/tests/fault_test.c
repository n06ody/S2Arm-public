#define _GNU_SOURCE
#include "../fault.h"
#ifdef S2A_NVN_PROBE_FIXTURE
#include "graphics/nvn/gfx/nvn/nvn_dirty.h"
#include "s2a_direct_host.h"
#endif
#include "../../../guest_signal.h"
#include <asm/sigcontext.h>
#include "../sigframe_compat.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

extern const uint32_t s2a_android_fault_return[];
void s2a_android_fault_body(uint64_t);
int s2a_fault_register_probe(void *);
uint64_t s2a_redzone_store(uint64_t *);
void s2a_fault_test_guest_stack(void (*body)(void), void *top);
// Fixture supplies the existing xstack slot ABI, not a second production xstack.
// The real signal test uses actual pthread bounds and a distinct guest mapping.
uint64_t g_xs_key;
struct xs_slot { uint64_t cur, top, bottom, gtop; };
static struct xs_slot host_slot;
static uintptr_t host_low, host_high;
static pthread_t guest_thread;
static int checks, touch_count, leave_count, enter_count;
static int tracker_result = 2, guest_fault_calls, host_fault_calls, tracker_write;
static uint64_t tracker_skip;
static volatile sig_atomic_t in_handler, real_fault, real_restore, callback_wrong;
static unsigned char *page;
static size_t page_size;
static void check(int yes, const char *name) {
    checks++;
    if (!yes) { fprintf(stderr, "FAIL %s\n", name); exit(1); }
}
// guest_signal.c logs forwarded faults through the app red log; the fixture only needs the symbol.
void s2a_android_redlog_write(const char *line, size_t n) { (void)line; (void)n; }
void s2a_core_leave(void) { leave_count++; }
void s2a_core_enter(void) { enter_count++; }
int s2a_track_fault_guest(void *address, int write, uint64_t skip) {
    (void)address; guest_fault_calls++; tracker_write = write; tracker_skip = skip;
    return tracker_result;
}
int s2a_track_fault(void *address, int write) {
    (void)address; (void)write; host_fault_calls++; return 0;
}
void s2a_track_touch(uint64_t address) {
    touch_count++;
    uintptr_t local = (uintptr_t)&address;
    if (local < host_low || local >= host_high || !pthread_equal(guest_thread, pthread_self())) callback_wrong = 1;
    __asm__ volatile(
        "movi v0.16b, #0\n"
        "movi v1.16b, #0\n"
        "movi v2.16b, #0\n"
        "movi v3.16b, #0\n"
        "movi v4.16b, #0\n"
        "movi v5.16b, #0\n"
        "movi v6.16b, #0\n"
        "movi v7.16b, #0\n"
        "movi v8.16b, #0\n"
        "movi v9.16b, #0\n"
        "movi v10.16b, #0\n"
        "movi v11.16b, #0\n"
        "movi v12.16b, #0\n"
        "movi v13.16b, #0\n"
        "movi v14.16b, #0\n"
        "movi v15.16b, #0\n"
        "movi v16.16b, #0\n"
        "movi v17.16b, #0\n"
        "movi v18.16b, #0\n"
        "movi v19.16b, #0\n"
        "movi v20.16b, #0\n"
        "movi v21.16b, #0\n"
        "movi v22.16b, #0\n"
        "movi v23.16b, #0\n"
        "movi v24.16b, #0\n"
        "movi v25.16b, #0\n"
        "movi v26.16b, #0\n"
        "movi v27.16b, #0\n"
        "movi v28.16b, #0\n"
        "movi v29.16b, #0\n"
        "movi v30.16b, #0\n"
        "movi v31.16b, #0\n"
        : : : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
    if (in_handler) callback_wrong = 1;
    if (page && address == (uintptr_t)page && mprotect(page, page_size, PROT_READ | PROT_WRITE)) _exit(121);
}
static void make_context(ucontext_t *context) {
    memset(context, 0, sizeof *context);
    sigemptyset(&context->uc_sigmask);
    for (unsigned i = 0; i < 31; i++) context->uc_mcontext.regs[i] = 0x100000000ull + i;
    context->uc_mcontext.pc = 0x12345678;
    context->uc_mcontext.sp = 0x87654320;
    context->uc_mcontext.pstate = 0xa0000000;
    struct fpsimd_context *fp = (void *)context->uc_mcontext.__reserved;
    fp->head.magic = FPSIMD_MAGIC;
    fp->head.size = sizeof *fp;
    fp->fpcr = 0x400000;
    fp->fpsr = 0x8000000;
    for (unsigned i = 0; i < sizeof fp->vregs; i++) ((unsigned char *)fp->vregs)[i] = (unsigned char)(i * 7 + 3);
    struct esr_context *esr = (void *)((unsigned char *)fp + sizeof *fp);
    esr->head.magic = ESR_MAGIC;
    esr->head.size = sizeof *esr;
    esr->esr = (0x24ull << 26) | (1 << 6);
}
static void add_inline_sme(ucontext_t *context) {
    struct tpidr2_context *tp = (void *)(context->uc_mcontext.__reserved
        + sizeof(struct fpsimd_context) + sizeof(struct esr_context));
    tp->head.magic = TPIDR2_MAGIC; tp->head.size = sizeof *tp; tp->tpidr2 = 0x123456789abcdef0ull;
    struct za_context *za = (void *)(tp + 1);
    za->head.magic = ZA_MAGIC; za->head.size = sizeof *za; za->vl = 16;
}
static void inline_sme_cases(int enabled) {
    ucontext_t context; int write;
    make_context(&context); add_inline_sme(&context);
    // #573 / #736: SME-capable cores put TPIDR2 + header-only ZA in every signal frame; default must accept.
    check(s2a_android_fault_esr(&context, &write) && write, "inline TPIDR2 and inactive ZA accepted by default");
    check(s2a_android_fault_unsupported_record() == 0, "no unsupported record reported while accepted");
    check(s2a_android_fault_configure_inline_sme(2) == EINVAL, "invalid experiment value rejected");
    check(!s2a_android_fault_configure_inline_sme(0), "opt out before runtime thread initialization");
    check(!s2a_android_fault_esr(&context, &write), "opted-out SME frame is not claimed");
    check(s2a_android_fault_unsupported_record() == TPIDR2_MAGIC, "first unclaimable record is reported (TPIDR2)");
    check(!s2a_android_fault_configure_inline_sme(1), "opt back in before runtime thread initialization");
    check(s2a_android_fault_esr(&context, &write) && write, "inline TPIDR2 and inactive ZA retain valid ESR");
    {   // One frame the way an SME + FP8 + POE kernel builds it: FPSIMD, ESR, TPIDR2, header-only ZA, FPMR, POE.
        // Hand expectation: every record is inline and fixed-size, so the ESR (data abort, WnR = 1) is found and the
        // fault is claimable; with the opt-out (old default) the first SME record makes it unclaimable.
        ucontext_t all; make_context(&all); add_inline_sme(&all);
        struct za_context *za0 = (void *)(all.uc_mcontext.__reserved + sizeof(struct fpsimd_context)
            + sizeof(struct esr_context) + sizeof(struct tpidr2_context));
        struct fpmr_context *fm0 = (void *)(za0 + 1);
        fm0->head.magic = FPMR_MAGIC; fm0->head.size = sizeof *fm0;
        struct poe_context *po0 = (void *)(fm0 + 1);
        po0->head.magic = POE_MAGIC; po0->head.size = sizeof *po0;
        check(s2a_android_fault_esr(&all, &write) && write, "full SME/FPMR/POE frame claimable by default");
        check(!s2a_android_fault_configure_inline_sme(0), "opt out to replay the old default");
        check(!s2a_android_fault_esr(&all, &write), "old default refuses the same frame");
        check(!s2a_android_fault_configure_inline_sme(1), "back to default");
        za0->head.size = 32;
        check(!s2a_android_fault_esr(&all, &write), "ZA with live matrix payload still refused");
    }
    {   // FPMR and POE: inline fixed-size snapshots newer kernels append; accepted and copied intact
        ucontext_t more; make_context(&more);
        struct fpmr_context *fm = (void *)(more.uc_mcontext.__reserved + sizeof(struct fpsimd_context) + sizeof(struct esr_context));
        fm->head.magic = FPMR_MAGIC; fm->head.size = sizeof *fm; fm->fpmr = 0x55;
        struct poe_context *po = (void *)(fm + 1);
        po->head.magic = POE_MAGIC; po->head.size = sizeof *po; po->por_el0 = 0x77;
        check(s2a_android_fault_esr(&more, &write) && write, "inline FPMR and POE retain valid ESR");
        po->head.size = 32;
        check(!s2a_android_fault_esr(&more, &write), "malformed POE size rejected");
        // Streaming SVE live at the fault cannot be redirected into plain host code
        make_context(&more);
        struct sve_context *sv = (void *)(more.uc_mcontext.__reserved + sizeof(struct fpsimd_context) + sizeof(struct esr_context));
        sv->head.magic = SVE_MAGIC; sv->head.size = sizeof *sv; sv->vl = 16; sv->flags = SVE_SIG_FLAG_SM;
        check(!s2a_android_fault_esr(&more, &write), "streaming SVE frame refused");
        sv->flags = 0;
        check(s2a_android_fault_esr(&more, &write) && write, "non-streaming SVE header accepted");
    }
    struct tpidr2_context *tp = (void *)(context.uc_mcontext.__reserved
        + sizeof(struct fpsimd_context) + sizeof(struct esr_context));
    struct za_context *za = (void *)(tp + 1);
    za->vl = 0;
    check(!s2a_android_fault_esr(&context, &write), "invalid ZA vector length rejected");
    za->vl = 16; za->__reserved[0] = 1;
    check(!s2a_android_fault_esr(&context, &write), "unknown ZA fields rejected");
    za->__reserved[0] = 0; za->head.size = 32;
    check(!s2a_android_fault_esr(&context, &write), "active ZA payload requires separate support");
    za->head.size = sizeof *za; za->head.magic = TPIDR2_MAGIC;
    check(!s2a_android_fault_esr(&context, &write), "duplicate TPIDR2 rejected");
    za->head.magic = 0x12340000;
    check(!s2a_android_fault_esr(&context, &write), "unknown context still rejected");
    za->head.magic = EXTRA_MAGIC;
    check(!s2a_android_fault_esr(&context, &write), "out-of-line context still rejected");
    za->head.magic = ZA_MAGIC; tp->head.size = 32;
    check(!s2a_android_fault_esr(&context, &write), "malformed TPIDR2 size rejected");
    check(!s2a_android_fault_configure_inline_sme(enabled), "select process experiment before freeze");
}
static struct esr_context *esr_of(ucontext_t *u) {
    return (void *)((unsigned char *)u->uc_mcontext.__reserved + sizeof(struct fpsimd_context));
}
static void rejected(ucontext_t *u, int expected, const char *name) {
    ucontext_t before = *u;
    check(s2a_android_fault_redirect(u, 77) == -expected, name);
    check(!memcmp(u, &before, sizeof *u), "reject leaves live frame unchanged");
}
#ifdef S2A_NVN_PROBE_FIXTURE
// System-level dependencies outside this fixture's scope. The production direct
// hook registration, adapter and XSTACK are linked. No test substitutes mprotect
// or decodes read/write from a fixture flag.
static s2a_guest_write_prep_fn prep_write;
void s2a_track_set_write_prep(s2a_guest_write_prep_fn fn) { prep_write = fn; }
void s2a_track_write_quiet(uint64_t addr, uint64_t len) { if (prep_write) prep_write(addr, len); }
void dm_log(const char *fmt, ...) { (void)fmt; }

static void land_probe_operations(void) {
    uint8_t *p = mmap(NULL, 3 * page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(p != MAP_FAILED, "map exact range for landing access probe");
    p[0] = 0x19;
    p[page_size] = 0x28;
    p[2 * page_size] = 0x37;
    nvn_dirty_add_range(p, 3 * page_size);
    nvn_dirty_read_probe_set_redirect(s2a_android_fault_redirect_call);
    check(nvn_dirty_read_probe_arm(p + 1, 2 * page_size) == 1, "arm only the full page in exact range");
    check(*(volatile uint8_t *)(p + page_size) == 0x28, "real read retry sees eager bytes");
    check(nvn_dirty_read_probe_arm(p + page_size, page_size) == 1, "arm full page again");
    *(volatile uint8_t *)(p + page_size) = 0x42;
    struct nvn_dirty_read_probe_stats stats;
    nvn_dirty_read_probe_stats(&stats);
    check(stats.armed == 2 && stats.read == 1 && stats.write == 1, "read and write classified from real ESR");
    check(stats.failed == 0 && stats.mp_calls == 4, "two real arm/restore pairs");
    check(p[0] == 0x19 && p[page_size] == 0x42 && p[2 * page_size] == 0x37, "adjacent pages unchanged");

    int fds[2];
    check(!pipe(fds), "create real kernel copy fixture");
    check(nvn_dirty_read_probe_arm(p + page_size, page_size) == 1, "arm before kernel copy");
    int before_kernel = real_restore;
    errno = 0;
    check(write(fds[1], p + page_size, 1) == -1 && errno == EFAULT, "kernel copy-in bypasses user signal");
    check(real_restore == before_kernel, "EFAULT did not enter XSTACK");
    struct nvn_dirty_probe_access access;
    nvn_dirty_read_probe_access_begin(&access, p + page_size, 1, 0);
    check(nvn_dirty_read_probe_arm(p + page_size, page_size) == 0, "no rearm with kernel access pending");
    check(write(fds[1], p + page_size, 1) == 1, "same kernel copy-in succeeds within access scope");
    nvn_dirty_read_probe_access_end(&access);
    check(nvn_dirty_read_probe_arm(p + 2 * page_size, page_size) == 1, "arm before kernel destination lease");
    nvn_dirty_read_probe_access_begin(&access, p + 2 * page_size, 1, 1);
    check(read(fds[0], p + 2 * page_size, 1) == 1 && p[2 * page_size] == 0x42, "scoped kernel copy-out");
    nvn_dirty_read_probe_access_end(&access);
    close(fds[0]);
    close(fds[1]);
    nvn_dirty_read_probe_stats(&stats);
    check(stats.armed == 4 && stats.read == 1 && stats.write == 1, "kernel copies excluded from CPU fault totals");
    check(stats.host_read == 1 && stats.host_write == 1 && stats.censored == 0, "host scope counts first access separately");
    check(stats.failed == 0 && stats.mp_calls == 8, "four real protection arm/restore pairs");
    printf("NVN land probe: page_bytes=%zu cpu_read=1 cpu_write=1 kernel_preflight_read=1 kernel_preflight_write=1 protection_calls=8 protection_ns=%llu\n",
           page_size, (unsigned long long)stats.mp_ns);
    nvn_dirty_remove_range(p);
    check(!munmap(p, 3 * page_size), "unmap retired fixture range");
}
#endif
static void handler(int sig, siginfo_t *si, void *pointer) {
    in_handler = 1;
    if (sig == SIGTRAP) {
        if (s2a_android_fault_handle(sig, si->si_addr, pointer, 0) != 1) _exit(122);
        real_restore++;
    } else {
#ifdef S2A_NVN_PROBE_FIXTURE
        int write;
        if (s2a_android_fault_esr(pointer, &write)) {
            const int handled = s2a_direct_fault(si->si_addr, write, pointer);
            if (handled < 0) _exit(123);
            if (handled == 1) { in_handler = 0; return; }
        }
#endif
        int guest = si->si_addr == page;
        if (s2a_android_fault_handle(sig, si->si_addr, pointer, guest) != 1) _exit(124);
        if (guest) real_fault++;
    }
    in_handler = 0;
}
static void *thread_test(void *unused) {
    (void)unused;
    check(!s2a_android_fault_thread_ready(), "new pthread has no state");
    check(!s2a_android_fault_thread_init(), "second pthread initialization");
    check(!s2a_android_fault_thread_ready(), "state without xstack is not ready");
    check(!s2a_android_fault_thread_fini(), "second pthread finalization");
    s2a_android_altstack_free();
    return NULL;
}
static void guest_operations(void) {
    uintptr_t local = (uintptr_t)&local;
    check(local < host_low || local >= host_high, "actual guest execution outside host stack");
    *(volatile unsigned char *)page = 0x5a;
    check(page[0] == 0x5a && real_fault == 1 && real_restore == 1, "normal touch then exact instruction retry");
    check(!mprotect(page, page_size, PROT_READ), "rearm real register probe");
    check(s2a_fault_register_probe(page) == 1, "real all 32 SIMD, FPCR/FPSR and callee GPR preserved");
    check(!mprotect(page, page_size, PROT_READ), "rearm redzone probe");
    check(s2a_redzone_store((uint64_t *)page) == 0x3fff, "real guest redzone 112 bytes preserved");
}
int main(int argc, char **argv) {
    // Default ON (#573); S2A_TEST_INLINE_SME=0 exercises the debug opt-out path.
    int inline_sme = !getenv("S2A_TEST_INLINE_SME") || strcmp(getenv("S2A_TEST_INLINE_SME"), "0");
    inline_sme_cases(inline_sme);
    (void)argv;
    pthread_key_t key;
    check(!pthread_key_create(&key, NULL), "fixture xstack key");
    g_xs_key = key;
    guest_thread = pthread_self();
    pthread_attr_t attr; void *bottom; size_t length;
    check(!pthread_getattr_np(guest_thread, &attr), "actual pthread attributes");
    check(!pthread_attr_getstack(&attr, &bottom, &length), "actual pthread stack bounds");
    pthread_attr_destroy(&attr);
    host_low = (uintptr_t)bottom; host_high = host_low + length;
    uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    host_slot.bottom = host_low; host_slot.cur = host_slot.top = (sp - 4096) & ~15ull;
    check(!pthread_setspecific(key, &host_slot), "fixture existing xstack slot");
    if (argc > 1) { check(!s2a_android_fault_thread_init(), "real-only initialization"); goto real; }
    ucontext_t original, live;
    make_context(&original);
    if (inline_sme) add_inline_sme(&original);
    live = original;
    rejected(&live, EINVAL, "unprepared thread rejects");
    check(!s2a_android_fault_thread_init(), "initialize actual altstack and save state");
    check(!s2a_android_fault_thread_init(), "idle initialization idempotent");
    check(s2a_android_fault_thread_ready(), "state and real host xstack are ready");
    check(s2a_android_fault_configure_inline_sme(!inline_sme) == EBUSY, "runtime experiment cannot change after thread initialization");
    tracker_result = 0;
    check(!s2a_android_fault_handle(SIGSEGV, (void *)77, &live, 1), "tracker zero continues original dispatcher");
    tracker_result = 1;
    check(s2a_android_fault_handle(SIGBUS, (void *)77, &live, 1) == 1, "tracker one retries without callback");
    check(tracker_write == 1 && !tracker_skip && !touch_count && !memcmp(&live, &original, sizeof live), "immediate retry keeps original context");
    tracker_result = 3;
    check(s2a_android_fault_handle(SIGSEGV, (void *)77, &live, 1) == -EPROTO, "unknown tracker result fails closed");
    tracker_result = 2;
    check(!s2a_android_fault_handle(SIGSEGV, (void *)77, &live, 0) && host_fault_calls == 1, "host PC uses existing host tracker only");
    int calls = guest_fault_calls;
    check(!s2a_android_fault_handle(SIGILL, (void *)77, &live, 1) && guest_fault_calls == calls, "unrelated signal continues original dispatcher");
    int write = -1;
    check(s2a_android_fault_esr(&live, &write) == 1 && write == 1, "ESR write");
    esr_of(&live)->esr &= ~(1ull << 6);
    check(s2a_android_fault_esr(&live, &write) == 1 && write == 0, "ESR read");
    esr_of(&live)->esr = 0;
    check(s2a_android_fault_esr(&live, &write) == 0, "non-data-abort not guessed");
    live = original;
    esr_of(&live)->head.size = 0xfffffff0;
    rejected(&live, EINVAL, "record overrun");
    live = original;
    esr_of(&live)->head.magic = EXTRA_MAGIC;
    rejected(&live, ENOTSUP, "external context cannot retain expired pointer");
    live = original;
    esr_of(&live)->head.magic = FPSIMD_MAGIC;
    esr_of(&live)->head.size = sizeof(struct fpsimd_context);
    rejected(&live, EINVAL, "duplicate FPSIMD");
    live = original;
    sigaddset(&live.uc_sigmask, SIGTRAP);
    rejected(&live, ENOTSUP, "blocked return trap");
    live = original;
    pthread_setspecific(key, NULL);
    rejected(&live, ENXIO, "missing xstack rejects before mutation");
    check(s2a_android_fault_handle(SIGSEGV, (void *)77, &live, 1) == -ENXIO && host_fault_calls == 1,
        "failed required redirect NEVER unprotects via host tracker");
    pthread_setspecific(key, &host_slot);
    host_slot.cur = host_slot.bottom;
    rejected(&live, ENXIO, "invalid cursor rejects before mutation");
    host_slot.cur = host_slot.top;
    check(s2a_android_fault_redirect(&live, 77) == 1, "redirect");
    check(live.uc_mcontext.sp > host_low && live.uc_mcontext.sp < host_slot.top && !(live.uc_mcontext.sp & 15), "aligned borrowed host stack");
    check(host_slot.cur == live.uc_mcontext.sp, "xstack cursor reserves active callback frame");
    check(s2a_android_fault_thread_fini() == EBUSY, "no disposal while pending");
    ucontext_t nested = original;
    rejected(&nested, EBUSY, "nested defer rejected");
    check(s2a_android_fault_restore(&nested) == 0, "unrelated trap untouched");
    nested.uc_mcontext.pc = (uintptr_t)s2a_android_fault_return;
    check(s2a_android_fault_restore(&nested) == -EINVAL, "cannot restore before callback");
    s2a_android_fault_body(77);
    check(s2a_android_fault_restore(&nested) == 1, "own completed trap restores");
    check(host_slot.cur == host_slot.top, "completed restore returns original xstack cursor");
    check(!memcmp(&nested, &original, sizeof nested), "all GPR/SP/PC/PSTATE/FP/SIMD/mask bytes restore");
    check(touch_count == 1 && leave_count == 1 && enter_count == 1, "normal callback uses scheduler leave/enter");
    pthread_t thread;
    check(!pthread_create(&thread, NULL, thread_test, NULL), "spawn second real pthread");
    check(!pthread_join(thread, NULL), "join second real pthread");
    check(s2a_android_fault_thread_ready(), "peer finalization did not alter this thread's state");

real:
    // Real kernel delivery and rt_sigreturn, not a synthetic handler call.
    ; int prior_touches = touch_count;
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    page = mmap(NULL, page_size, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(page != MAP_FAILED, "real protected mapping");
    struct sigaction old_segv, old_trap;
    check(!s2a_android_sigaction(SIGSEGV, handler, &old_segv), "fixture dispatcher owns SIGSEGV");
    check(!s2a_android_sigaction(SIGTRAP, handler, &old_trap), "fixture dispatcher owns SIGTRAP");
    size_t guest_length = 256 * 1024;
    void *guest = mmap(NULL, guest_length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(guest != MAP_FAILED, "separate guest stack");
    s2a_fault_test_guest_stack(guest_operations, (char *)guest + guest_length);
    check(!callback_wrong && touch_count == prior_touches + 3, "real touch outside handler on original pthread host stack");
    check(host_slot.cur == host_slot.top, "real kernel returns restored xstack cursor");
    check(real_fault == 3 && real_restore == 3 && !callback_wrong, "three independent kernel returns");
    check(tracker_skip == 0, "same page later faults never inherit a stale address-only skip");
    check(!s2a_android_fault_dispatch_ready(), "installation alone is not final dispatch proof");
    int touches = touch_count;
    check(!s2a_android_fault_probe(), "startup probe traverses same real handler and rt_sigreturn");
    check(s2a_android_fault_dispatch_ready() && touch_count == touches, "final proof does not fake GPU callback");
    s2a_android_fault_dispatch_reset();
    check(!s2a_android_fault_dispatch_ready(), "handler owner can invalidate proof before replacement");
    check(!s2a_android_fault_probe(), "repeat final proof after handler lifecycle change");
#ifdef S2A_NVN_PROBE_FIXTURE
    const int before_landing_restore = real_restore;
    s2a_fault_test_guest_stack(land_probe_operations, (char *)guest + guest_length);
    check(real_restore == before_landing_restore + 2, "landing probe used two existing XSTACK returns");
    check(touch_count == touches && host_slot.cur == host_slot.top, "no generic tracker callback or borrowed stack leak");
#endif
    sigaction(SIGTRAP, &old_trap, NULL);
    sigaction(SIGSEGV, &old_segv, NULL);
    munmap(page, page_size); page = NULL;
    munmap(guest, guest_length);
    check(!s2a_android_fault_thread_fini(), "finalize idle state");
    check(!s2a_android_fault_thread_ready(), "not ready after finalization");
    s2a_android_altstack_free();
    pthread_setspecific(key, NULL);
    pthread_key_delete(key);
    printf("PASS %d checks; real AArch64 signal -> normal touch -> BRK -> retry\n", checks);
    return 0;
}
