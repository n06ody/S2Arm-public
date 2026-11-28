// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

// Exercise real VM permissions and actual CPU/kernel memory accesses.
// The Darwin fixture uses siglongjmp to run the redirect callback off the signal
// stack. Android's register restore is covered separately by its XSTACK suite.
#define _GNU_SOURCE
#include "../gfx/nvn/nvn_dirty.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

#if defined(S2A_READ_PROBE_BASELINE)
#define nvn_dirty_read_probe_set_redirect(fn) ((void)(fn))
#define nvn_dirty_read_probe_arm(addr, len) ((void)(addr), (void)(len), (size_t)0)
#define nvn_dirty_read_probe_access_begin(scope, addr, len, write) ((void)(scope), (void)(addr), (void)(len), (void)(write))
#define nvn_dirty_read_probe_access_end(scope) ((void)(scope))
#define nvn_dirty_read_probe_stats(out) ((void)(out))
#endif

#if defined(S2A_READ_PROBE_BASELINE) || defined(S2A_READ_PROBE_PIN_BASELINE)
#define nvn_dirty_read_probe_pin_export(addr, len, write) ((void)(addr), (void)(len), (void)(write))
#endif

static int (*fault_hook)(void *, int, void *);
static void (*write_prep)(void *, size_t);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { fault_hook = fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { write_prep = fn; }
void dm_log(const char *fmt, ...) { (void)fmt; }

static sigjmp_buf retry;
static volatile sig_atomic_t read_faults, write_faults, redirected, resume_ready;
static void (*resume_callback)(void *);
static void *resume_arg;

static int redirect(void *context, void (*callback)(void *), void *arg) {
    if (!context || !resume_ready) return 0;
    resume_callback = callback;
    resume_arg = arg;
    redirected++;
    siglongjmp(retry, 1);
}
static void fault(int sig, siginfo_t *info, void *context) {
#if defined(__APPLE__) && defined(__aarch64__)
    const uint32_t esr = ((ucontext_t *)context)->uc_mcontext->__es.__esr;
    const int data_abort = (esr >> 26) == 0x24 || (esr >> 26) == 0x25;
    const int is_write = (esr >> 6) & 1;
    const int memory_fault = info->si_code > 0;
#elif defined(__linux__) && defined(__x86_64__)
    const greg_t error = ((ucontext_t *)context)->uc_mcontext.gregs[REG_ERR];
    const int data_abort = !(error & 16);
    const int is_write = (error >> 1) & 1;
    const int memory_fault = info->si_code == SEGV_ACCERR;
#else
#error Supply native access direction from the real signal frame.
#endif
    if (memory_fault && data_abort) {
        if (is_write) write_faults++;
        else read_faults++;
        if (fault_hook && fault_hook(info->si_addr, is_write, context) == 1) return;
    }
    _exit(128 + sig);
}
static void retry_callback(void) {
    if (!resume_callback) _exit(125);
    resume_callback(resume_arg);
    resume_callback = NULL;
}
static uint8_t load_byte(volatile uint8_t *p) {
    resume_ready = 1;
    if (sigsetjmp(retry, 1)) retry_callback();
    const uint8_t out = *p;
    resume_ready = 0;
    return out;
}
static void store_byte(volatile uint8_t *p, uint8_t value) {
    resume_ready = 1;
    if (sigsetjmp(retry, 1)) retry_callback();
    *p = value;
    resume_ready = 0;
}

static unsigned checks, failures;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "【红】%d %s\n", __LINE__, #cond); } } while (0)

#ifdef S2A_LAND_DEFER_BASELINE
#define nvn_dirty_land_defer_install(source, completed) ((void)(source), (void)(completed), (size_t)0)
#define nvn_dirty_land_defer_remaining(source) ((void)(source), (size_t)0)
#endif

#if !defined(S2A_READ_PROBE_BASELINE) && !defined(S2A_READ_PROBE_PIN_BASELINE)
static void test_deferred_pages(size_t ps) {
    const size_t bytes = 8 * ps;
    char name[32];
    snprintf(name, sizeof name, "/s2a-land-%ld", (long)getpid());
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || shm_unlink(name) || ftruncate(fd, (off_t)bytes)) _exit(2);
    uint8_t *guest = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    uint8_t *alias = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    uint8_t *old = mmap(NULL, 2 * ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint8_t *next = mmap(NULL, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (guest == MAP_FAILED || alias == MAP_FAILED || old == MAP_FAILED || next == MAP_FAILED) _exit(2);
    memset(guest, 0x31, bytes);
    memset(old, 0x7b, 2 * ps);
    memset(next, 0x9c, ps);
    nvn_dirty_add_range(guest, bytes);
    struct nvn_dirty_deferred_source first = {
        .destination = guest + ps, .alias = alias + ps, .completed_bytes = old, .length = 2 * ps, .version = 17,
    };
    struct nvn_dirty_deferred_source later = {
        .destination = guest + 2 * ps, .alias = alias + 2 * ps, .completed_bytes = next, .length = ps, .version = 18,
    };
    size_t installed = nvn_dirty_land_defer_install(&first, 1);
    if (!installed) memcpy(guest + ps, old, 2 * ps);   // pre-change eager path
    CHECK(installed == 2 && nvn_dirty_land_defer_remaining(&first) == 2);
    CHECK(alias[ps] == 0x31 && alias[2 * ps] == 0x31);  // skipped actual guest copy
    CHECK(nvn_dirty_land_defer_install(&later, 0) == 0);
#ifndef S2A_LAND_DEFER_BASELINE
    CHECK(later.reject == NVN_DEFER_BAD_SOURCE);
#endif
    CHECK(nvn_dirty_land_defer_remaining(&first) == 2 && nvn_dirty_land_defer_remaining(&later) == 0);
    CHECK(alias[2 * ps] == 0x31);
    CHECK(nvn_dirty_land_defer_install(&later, 1) == 0);  // completion alone is not a full overwrite proof
#ifndef S2A_LAND_DEFER_BASELINE
    CHECK(later.reject == NVN_DEFER_PAGE_STATE);
#endif
    CHECK(nvn_dirty_land_defer_remaining(&first) == 2);
    later.replace_verified = 1;
    CHECK(nvn_dirty_land_defer_install(&later, 1) == 1);
    CHECK(nvn_dirty_land_defer_remaining(&first) == 1 && nvn_dirty_land_defer_remaining(&later) == 1);
    CHECK(alias[2 * ps] == 0x31);   // replacing a full page did not copy the old version
    CHECK(load_byte(guest + ps) == 0x7b && nvn_dirty_land_defer_remaining(&first) == 0);
    CHECK(nvn_dirty_land_defer_remaining(&later) == 1 && alias[2 * ps] == 0x31);
    CHECK(load_byte(guest + 2 * ps) == 0x9c && nvn_dirty_land_defer_remaining(&later) == 0);
    CHECK(alias[ps - 1] == 0x31 && alias[3 * ps] == 0x31);

    nvn_dirty_set_chunk_pages(4);
    uint8_t *block = guest + 4 * ps;
    nvn_dirty_protect(block, 1);
    sig_atomic_t faults = write_faults;
    const uint64_t chunk_stamp = nvn_dirty_now();
    store_byte(block, 0x40);
    CHECK(nvn_dirty_since(block + 3 * ps, ps, chunk_stamp));
    for (size_t i = 1; i < 4; i++) store_byte(block + i * ps, 0x40 + i);
    CHECK(write_faults == faults + 1);

    struct nvn_dirty_deferred_source neighbor = {
        .destination = block + 2 * ps, .alias = alias + 6 * ps,
        .completed_bytes = next, .length = ps, .version = 19,
    };
    CHECK(nvn_dirty_land_defer_install(&neighbor, 1) == 1);
    nvn_dirty_protect(block, 1);
    CHECK(nvn_dirty_land_defer_remaining(&neighbor) == 1 && alias[6 * ps] == 0x42);
    faults = write_faults;
    store_byte(block, 0x50);
    store_byte(block + ps, 0x51);
    CHECK(write_faults == faults + 1);
    CHECK(nvn_dirty_land_defer_remaining(&neighbor) == 1 && alias[6 * ps] == 0x42);
    write_prep(block + 3 * ps, 1);
    store_byte(block + 3 * ps, 0x53);
    CHECK(write_faults == faults + 1);
    CHECK(nvn_dirty_land_defer_remaining(&neighbor) == 1 && alias[6 * ps] == 0x42);
    const sig_atomic_t reads = read_faults;
    CHECK(load_byte(block + 2 * ps) == 0x9c && read_faults == reads + 1);
    CHECK(nvn_dirty_land_defer_remaining(&neighbor) == 0 && alias[6 * ps] == 0x9c);

#ifndef S2A_LAND_DEFER_BASELINE
    struct nvn_dirty_read_probe_stats stats;
    nvn_dirty_read_probe_stats(&stats);
    CHECK(stats.defer_materialized == 3 && stats.defer_by_reason[NVN_DEFER_GUEST_READ] == 3);
    CHECK(stats.defer_replaced == 1);

    memset(old, 0xcc, 2 * ps);
    struct nvn_dirty_deferred_source overlap = {
        .destination = guest + ps, .alias = alias + ps, .completed_bytes = old, .length = 2 * ps, .version = 20,
    };
    CHECK(nvn_dirty_land_defer_install(&overlap, 1) == 2);
    CHECK(nvn_dirty_land_defer_flush_range(&overlap, (uintptr_t)guest + ps + 3, 1, NVN_DEFER_GPU_WRITE));
    CHECK(!nvn_dirty_land_defer_owns_page(&overlap, (uintptr_t)guest + ps));
    CHECK(nvn_dirty_land_defer_owns_page(&overlap, (uintptr_t)guest + 2 * ps));
    CHECK(nvn_dirty_land_defer_remaining(&overlap) == 1 && alias[ps] == 0xcc && alias[2 * ps] == 0x9c);
    const uint64_t before_open = nvn_dirty_now();
    CHECK(load_byte(guest + 2 * ps) == 0xcc && !nvn_dirty_land_defer_remaining(&overlap));
    store_byte(guest + 2 * ps, 0xe1);
    CHECK(nvn_dirty_since(guest + 2 * ps, ps, before_open));  // handing the page back to CPU invalidates mirror ownership
    nvn_dirty_read_probe_stats(&stats);
    CHECK(stats.defer_by_reason[NVN_DEFER_GPU_WRITE] == 1);
#endif

    nvn_dirty_remove_range(guest);
    nvn_dirty_set_chunk_pages(0);
    munmap(guest, bytes);
    munmap(alias, bytes);
    munmap(old, 2 * ps);
    munmap(next, ps);
}
#endif

int main(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = fault;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &sa, NULL) || sigaction(SIGBUS, &sa, NULL)) return 2;
    const size_t ps = (size_t)getpagesize();
    uint8_t *p = mmap(NULL, 4 * ps, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) return 2;
    p[0] = 0x11;
    p[ps] = 0x22;
    p[2 * ps] = 0x33;
    p[3 * ps] = 0x44;

    nvn_dirty_add_range(p, 4 * ps);
    CHECK(nvn_dirty_read_probe_arm(p, 4 * ps) == 0);  // default off
    CHECK(load_byte(p) == 0x11 && read_faults == 0);
    nvn_dirty_read_probe_set_redirect(redirect);

    // Exact interval [p+1, p+3*ps+1): only pages 1 and 2 are complete.
    CHECK(nvn_dirty_read_probe_arm(p + 1, 3 * ps) == 2);
    CHECK(load_byte(p) == 0x11 && load_byte(p + 3 * ps) == 0x44);
    CHECK(read_faults == 0 && write_faults == 0);
    CHECK(load_byte(p + ps) == 0x22 && read_faults == 1 && redirected == 1);
    CHECK(load_byte(p + ps) == 0x22 && read_faults == 1);
    const uint64_t stamp = nvn_dirty_now();
    store_byte(p + 2 * ps, 0x63);
    CHECK(write_faults == 1 && redirected == 2);
    CHECK(load_byte(p + 2 * ps) == 0x63 && read_faults == 1);
    CHECK(nvn_dirty_since(p + 2 * ps, ps, stamp));
    CHECK(!nvn_dirty_since(p + ps, ps, stamp));   // read alone is no write

    int pipe_ends[2];
    if (pipe(pipe_ends)) return 2;
    CHECK(nvn_dirty_read_probe_arm(p + 2 * ps, ps) == 1);
    errno = 0;
    CHECK(write(pipe_ends[1], p + 2 * ps, 1) == -1 && errno == EFAULT);
    CHECK(read_faults == 1 && write_faults == 1);  // kernel copy can fail without a user fault
    struct nvn_dirty_probe_access access;
    nvn_dirty_read_probe_access_begin(&access, p + 2 * ps, 1, 0);
    CHECK(nvn_dirty_read_probe_arm(p + 2 * ps, ps) == 0);  // kernel scope excludes rearming
    CHECK(write(pipe_ends[1], p + 2 * ps, 1) == 1);
    nvn_dirty_read_probe_access_end(&access);
    CHECK(nvn_dirty_read_probe_arm(p + 3 * ps, ps) == 1);
    nvn_dirty_read_probe_access_begin(&access, p + 3 * ps, 1, 1);
    write_prep(p + 3 * ps, 1);  // existing pre-write tracking remains in effect
    CHECK(read(pipe_ends[0], p + 3 * ps, 1) == 1 && p[3 * ps] == 0x63);
    nvn_dirty_read_probe_access_end(&access);
    close(pipe_ends[0]);
    close(pipe_ends[1]);

    nvn_dirty_protect(p, ps);
    CHECK(nvn_dirty_read_probe_arm(p, ps) == 0);  // existing RO tracking owner is preserved
    CHECK(load_byte(p) == 0x11);
    store_byte(p, 0x51);
    CHECK(redirected == 2);  // ordinary dirty write is still resolved by the original path
    CHECK(nvn_dirty_read_probe_arm(p + ps, ps) == 1);
    nvn_dirty_remove_range(p);
    CHECK(load_byte(p + ps) == 0x22 && read_faults == 1);  // retiring pool leaves no probe protections
    struct nvn_dirty_read_probe_stats stats = {0};
    nvn_dirty_read_probe_stats(&stats);
    CHECK(stats.armed == 5 && stats.read == 1 && stats.write == 1);
    CHECK(stats.host_read == 1 && stats.host_write == 1 && stats.censored == 1);
    CHECK(stats.failed == 0 && stats.mp_calls == 9 && stats.mp_ns > 0);
    munmap(p, 4 * ps);

    uint8_t *direct = mmap(NULL, 2 * ps, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (direct == MAP_FAILED || pipe(pipe_ends)) return 2;
    direct[2] = 0x68;
    direct[ps + 2] = 0x73;
    nvn_dirty_read_probe_pin_export(direct + 2, 1, 0);
    nvn_dirty_add_range(direct, 2 * ps);
    CHECK(nvn_dirty_read_probe_arm(direct, ps) == 0);  // exported before the pool existed
    CHECK(write(pipe_ends[1], direct + 2, 1) == 1);
    CHECK(nvn_dirty_read_probe_arm(direct + ps, ps) == 1);  // adjacent page remains eligible
    nvn_dirty_read_probe_pin_export(direct + ps + 2, 1, 0);
    CHECK(write(pipe_ends[1], direct + ps + 2, 1) == 1);  // export cancels active probe
    // The same kernel buffer is allowed later without a fresh export or Dispose.
    CHECK(write(pipe_ends[1], direct + ps + 2, 1) == 1);
    CHECK(nvn_dirty_read_probe_arm(direct + ps, ps) == 0);
    nvn_dirty_remove_range(direct);
    nvn_dirty_add_range(direct, 2 * ps);
    CHECK(nvn_dirty_read_probe_arm(direct, 2 * ps) == 0);  // old raw views survived the pool
    nvn_dirty_remove_range(direct);
    close(pipe_ends[1]);
    close(pipe_ends[0]);
#if !defined(S2A_READ_PROBE_BASELINE) && !defined(S2A_READ_PROBE_PIN_BASELINE)
    // Keep the retired-but-exported address reserved until this independent
    // case finishes: the session deliberately still excludes that address.
    test_deferred_pages(ps);
#endif
    munmap(direct, 2 * ps);
    printf("t_dirty_read_probe: %u 项，红 %u（%zu B 宿主页）\n", checks, failures, ps);
    return failures != 0;
}
