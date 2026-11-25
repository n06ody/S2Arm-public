// SPDX-License-Identifier: Apache-2.0
// 实际保护宿主页，用故障指令的 PC 区分客体替身与宿主，读写方向取系统信号上下文。
#define _GNU_SOURCE 1
#include "../gfx/nvn/nvn_dirty.h"
#include "../gfx/nvn/nvn_land_probe.h"
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/ucontext.h>
#if defined(__linux__) && defined(__aarch64__)
#include <asm/sigcontext.h>
#endif

static int (*fault_hook)(void *, int, void *);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { fault_hook = fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { (void)fn; }
void dm_log(const char *fmt, ...) { (void)fmt; }

static uintptr_t fault_pc(void *ctx) {
    ucontext_t *u = ctx;
#if defined(__APPLE__) && defined(__aarch64__)
    return u->uc_mcontext->__ss.__pc;
#elif defined(__APPLE__) && defined(__x86_64__)
    return u->uc_mcontext->__ss.__rip;
#elif defined(__x86_64__)
    return u->uc_mcontext.gregs[REG_RIP];
#else
    return u->uc_mcontext.pc;
#endif
}
static void handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *u = ctx;
#if defined(__APPLE__) && defined(__aarch64__)
    int write = (u->uc_mcontext->__es.__esr >> 6) & 1;
#elif defined(__APPLE__) && defined(__x86_64__)
    int write = (u->uc_mcontext->__es.__err >> 1) & 1;
#elif defined(__x86_64__)
    int write = (u->uc_mcontext.gregs[REG_ERR] >> 1) & 1;
#else
    int write = -1;
    const unsigned char *p = u->uc_mcontext.__reserved;
    size_t left = sizeof u->uc_mcontext.__reserved;
    while (left >= sizeof(struct _aarch64_ctx)) {
        const struct _aarch64_ctx *h = (const void *)p;
        if (h->size < 16 || h->size % 16 || h->size > left) break;
        if (h->magic == ESR_MAGIC && h->size == sizeof(struct esr_context)) {
            write = (((const struct esr_context *)p)->esr >> 6) & 1; break;
        }
        p += h->size; left -= h->size;
    }
    if (write < 0) _exit(128 + sig);
#endif
    if (!fault_hook || !fault_hook(si->si_addr, write, ctx)) _exit(128 + sig);
}
static volatile unsigned sink;
#define PROBE_STRING_RAW(x) #x
#define PROBE_STRING(x) PROBE_STRING_RAW(x)
#define LOAD_LABEL PROBE_STRING(__USER_LABEL_PREFIX__) "land_probe_test_load"
extern const unsigned char land_probe_test_load[];
__attribute__((noinline)) static unsigned guest_load(volatile unsigned char *p) {
    unsigned value;
#if defined(__aarch64__)
    __asm__ volatile (".globl " LOAD_LABEL "\n" LOAD_LABEL ": ldrb %w0, [%1]" : "=r"(value) : "r"(p) : "memory");
#else
    __asm__ volatile (".globl " LOAD_LABEL "\n" LOAD_LABEL ": movzbl (%1), %0" : "=r"(value) : "r"(p) : "memory");
#endif
    return value;
}
int nvn_probe_guest_context(void *ctx) { return fault_pc(ctx) == (uintptr_t)land_probe_test_load; }
static struct nvn_probe_sample rows[256];
static void capture(const struct nvn_probe_sample *s) { if (s->write < 256) rows[s->write] = *s; }
static int checks, failed;
#define CK(x) do { checks++; if (!(x)) { fprintf(stderr, "RED %s\n", #x); failed++; } } while (0)
struct reader { volatile unsigned char *p; unsigned value; };
static atomic_int ready, start;
static void *read_once(void *arg) {
    struct reader *r = arg;
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&start)) sched_yield();
    r->value = guest_load(r->p);
    return NULL;
}
static void competing_reads(unsigned char *p) {
    for (int round = 0; round < 32; round++) {
        atomic_store(&ready, 0); atomic_store(&start, 0);
        nvn_probe_after(p, 4, 9, 18, 180 + round, 0, (struct nvn_probe_source){0});
        pthread_t threads[2]; struct reader args[2] = {{p, 0}, {p, 0}};
        for (int j = 0; j < 2; j++) if (pthread_create(&threads[j], NULL, read_once, &args[j])) exit(2);
        while (atomic_load(&ready) != 2) sched_yield();
        atomic_store(&start, 1);
        if (round & 1) CK(nvn_probe_drain(capture) == 0);
        for (int j = 0; j < 2; j++) pthread_join(threads[j], NULL);
        if (!(round & 1)) CK(nvn_probe_drain(capture) == 0);
        CK(args[0].value == 0x72 && args[1].value == 0x72);
        unsigned result = rows[180 + round].result;
        CK(result == NVN_PROBE_GUEST_READ || ((round & 1) && result == NVN_PROBE_EXPIRED));
    }
}
int main(void) {
    struct sigaction sa = {0}; sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask); sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    size_t pg = (size_t)getpagesize();
    unsigned char *m = mmap(NULL, 12 * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (m == MAP_FAILED) return 2;
    memset(m, 0x53, 12 * pg);
    nvn_dirty_add_range(m, 12 * pg);
    for (unsigned i = 1; i <= 6; i++) {
        unsigned char *p = m + i * pg + 9;
        nvn_probe_after(p, 4, i * pg + 9, 17, i, 0, (struct nvn_probe_source){0});
        switch (i) {
        case 1: sink = guest_load(p); CK(sink == 0x53); break;
        case 2: *(volatile unsigned char *)p = 0x62; CK(*p == 0x62); break;
        case 3: sink = guest_load(p + 8); break;
        case 4: nvn_probe_before(p, 4, 17, 23); *p = 0x44; break;
        case 5: sink = *(volatile unsigned char *)p; break;
        }
    }
    CK(nvn_probe_drain(capture) == 0);
    CK(rows[1].armed && rows[1].result == NVN_PROBE_GUEST_READ);
    CK(rows[2].armed && rows[2].result == NVN_PROBE_WRITE_FIRST);
    CK(rows[3].armed && rows[3].result == NVN_PROBE_NEIGHBOR);
    CK(rows[4].armed && rows[4].result == NVN_PROBE_NEXT_VERSION);
    CK(rows[5].armed && rows[5].result == NVN_PROBE_HOST_READ);
    CK(rows[6].armed && rows[6].result == NVN_PROBE_EXPIRED);
    CK(rows[1].pool == 17 && rows[1].offset == pg + 9 && rows[1].bytes == 4);
    // Same segment, four consecutive versions. The pages are chosen relative to
    // the segment, even when segment byte zero starts partway through a page.
    for (unsigned v = 10; v < 14; v++) {
        nvn_probe_before(m + 7, pg * 7, 17, v);
        nvn_probe_after(m + 7, pg * 7, 7, 17, v, 0, (struct nvn_probe_source){0});
    }
    CK(nvn_probe_drain(capture) == 0);
    CK(rows[10].pages == 8 && rows[10].page_index == 0 && rows[10].result == NVN_PROBE_NEXT_VERSION);
    CK(rows[11].page_index == 4 && rows[11].result == NVN_PROBE_NEXT_VERSION);
    CK(rows[12].page_index == 7 && rows[12].result == NVN_PROBE_NEXT_VERSION);
    CK(rows[13].page_index < 8 && rows[13].result == NVN_PROBE_EXPIRED);
    // An original write-protection owner must still observe the next write.
    nvn_dirty_protect(m, pg);
    uint64_t stamp = nvn_dirty_now();
    nvn_probe_after(m + 9, 4, 9, 17, 20, 0, (struct nvn_probe_source){0});
    sink = guest_load(m + 9); CK(!nvn_dirty_since(m, pg, stamp));
    *(volatile unsigned char *)(m + 9) = 0x72; CK(nvn_dirty_since(m, pg, stamp));
    // A newly registered overlapping pool retires the sample. A second arm
    // on the shared page must also refuse attribution, including a short tail.
    nvn_probe_after(m + pg + 9, 4, pg + 9, 17, 21, 0, (struct nvn_probe_source){0});
    nvn_dirty_add_range(m + pg + 128, 32);
    nvn_probe_after(m + pg + 9, 4, pg + 9, 17, 22, 0, (struct nvn_probe_source){0});
    nvn_dirty_remove_range(m + pg + 128);
    nvn_probe_after(m + 2 * pg + 9, 4, 2 * pg + 9, 17, 23, 1, (struct nvn_probe_source){0});
    nvn_probe_after(m + 3 * pg + 9, 4, 3 * pg + 9, 17, 24, 0, (struct nvn_probe_source){0});
    nvn_dirty_remove_range(m);
    nvn_dirty_add_range(m, 12 * pg);
    nvn_probe_after(m + 3 * pg + 9, 4, 3 * pg + 9, 18, 25, 0, (struct nvn_probe_source){0});
    sink = guest_load(m + 3 * pg + 9);
    CK(nvn_probe_drain(capture) == 0);
    CK(rows[20].result == NVN_PROBE_GUEST_READ);
    CK(rows[21].armed && rows[21].result == NVN_PROBE_ALIAS);
    CK(!rows[22].armed && rows[22].result == NVN_PROBE_ALIAS);
    CK(!rows[23].armed && rows[23].result == NVN_PROBE_ALIAS);
    CK(rows[24].armed && rows[24].result == NVN_PROBE_ALIAS);
    CK(rows[25].pool == 18 && rows[25].result == NVN_PROBE_GUEST_READ);
    CK(!fault_hook(m + 11 * pg, 0, NULL)); // never sampled: unrelated read not claimed
    for (unsigned v = 30; v < 160; v++) {
        nvn_probe_before(m + 9, 4, 18, v);
        nvn_probe_after(m + 9, 4, 9, 18, v, 0, (struct nvn_probe_source){0});
    }
    CK(nvn_probe_drain(capture) == 2);
    CK(rows[157].armed && rows[157].result == NVN_PROBE_NEXT_VERSION && !rows[158].armed);
    // Kernel copy-in has no userspace load instruction. This counterexample is
    // why this branch is an offline experiment: the probe can change syscall results.
    int fds[2];
    if (pipe(fds)) return 2;
    CK(write(fds[1], m + 9, 1) == 1);
    nvn_probe_after(m + 9, 4, 9, 18, 170, 0, (struct nvn_probe_source){0});
    errno = 0;
    CK(write(fds[1], m + 9, 1) == -1 && errno == EFAULT);
    CK(nvn_probe_drain(capture) == 0 && rows[170].result == NVN_PROBE_EXPIRED);
    CK(write(fds[1], m + 9, 1) == 1);
    close(fds[0]); close(fds[1]);
    competing_reads(m + 9);
    nvn_dirty_remove_range(m); munmap(m, 12 * pg);
    printf("land probe: %d checks, %d failures\n", checks, failed); return !!failed;
}
