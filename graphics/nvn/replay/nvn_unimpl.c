#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

enum { UN_MAX = 256, UN_NAME = 48 };
static struct { char cls[12], name[UN_NAME]; uint64_t n; } g_un[UN_MAX];
static int g_nun, g_un_full;
static pthread_mutex_t g_un_mu = PTHREAD_MUTEX_INITIALIZER;
// Exit summaries must not wait for a worker holding g_un_mu. Publish only the diagnostic
// counters atomically; sequence marks a concurrent update as unstable, never as proven zero.
static _Atomic uint64_t g_diag_seq, g_diag_compile, g_diag_unresolved;
static _Atomic unsigned g_diag_kinds, g_diag_full;
static _Atomic int g_diag_on = -1;
static int diag_enabled(void) {
    int on = atomic_load_explicit(&g_diag_on, memory_order_relaxed);
    if (on < 0) {
        on = nvn_plat_flag("nvk_csd", 0) || nvn_plat_flag("nvk_csf", 0);
        atomic_store_explicit(&g_diag_on, on, memory_order_relaxed);
    }
    return on;
}

static int un_find(const char *cls, const char *name) {
    for (int i = 0; i < g_nun; i++) if (!strcmp(g_un[i].cls, cls) && !strcmp(g_un[i].name, name)) return i;
    return -1;
}

uint64_t nvn_unimpl(const char *cls, const char *name, const void *site) {
    if (!cls) cls = "?";
    if (!name) name = "?";
    char line[160]; int first = 0, full = 0; uint64_t n = 0; int diag = diag_enabled();
    pthread_mutex_lock(&g_un_mu);
    if (diag) atomic_fetch_add_explicit(&g_diag_seq, 1, memory_order_seq_cst);
    int i = un_find(cls, name);
    if (i < 0 && g_nun < UN_MAX) {
        i = g_nun++; first = 1;
        snprintf(g_un[i].cls, sizeof g_un[i].cls, "%s", cls); snprintf(g_un[i].name, sizeof g_un[i].name, "%s", name);
        g_un[i].n = 0;
    }
    if (i >= 0) n = ++g_un[i].n; else if (!g_un_full) { g_un_full = full = 1; }
    if (diag) {
        if (!strcmp(cls, "shader") && !strcmp(name, "compute_compile_failed")) atomic_fetch_add_explicit(&g_diag_compile, 1, memory_order_seq_cst);
        if (!strcmp(cls, "draw") && !strcmp(name, "compute_program_unresolved")) atomic_fetch_add_explicit(&g_diag_unresolved, 1, memory_order_seq_cst);
        atomic_store_explicit(&g_diag_kinds, (unsigned)g_nun, memory_order_seq_cst);
        atomic_store_explicit(&g_diag_full, (unsigned)g_un_full, memory_order_seq_cst);
        atomic_fetch_add_explicit(&g_diag_seq, 1, memory_order_seq_cst);
    }
    pthread_mutex_unlock(&g_un_mu);
    if (first) { snprintf(line, sizeof line, "未接：%s %s（首次，位置 %p）", cls, name, site); nvn_plat_log(NVN_PLAT_LOG_RED, "unimpl", line); }
    if (full) nvn_plat_log(NVN_PLAT_LOG_RED, "unimpl", "未接项超过 256 种，之后的新项不再计数");
    return n;
}

uint64_t nvn_unimpl_count(const char *cls, const char *name) {
    pthread_mutex_lock(&g_un_mu);
    int i = un_find(cls, name); uint64_t n = i < 0 ? 0 : g_un[i].n;
    pthread_mutex_unlock(&g_un_mu);
    return n;
}

size_t nvn_unimpl_dump(char *buf, size_t cap) {
    size_t w = 0;
    if (!buf || !cap) return 0;
    buf[0] = 0;
    pthread_mutex_lock(&g_un_mu);
    for (int i = 0; i < g_nun; i++) {
        char line[96]; int k = snprintf(line, sizeof line, "%s\t%s\t%llu\n", g_un[i].cls, g_un[i].name, (unsigned long long)g_un[i].n);
        if (k < 0 || w + (size_t)k + 1 > cap) break;
        memcpy(buf + w, line, (size_t)k + 1); w += (size_t)k;
    }
    pthread_mutex_unlock(&g_un_mu);
    return w;
}

int nvn_unimpl_kinds(void) { pthread_mutex_lock(&g_un_mu); int n = g_nun; pthread_mutex_unlock(&g_un_mu); return n; }

void nvn_unimpl_diag_summary(const char *phase, uint64_t frame) {
    if (!nvn_plat_flag("nvk_csd", 0) && !nvn_plat_flag("nvk_csf", 0)) return;
    uint64_t first = atomic_load_explicit(&g_diag_seq, memory_order_seq_cst);
    unsigned kinds = atomic_load_explicit(&g_diag_kinds, memory_order_seq_cst);
    uint64_t compile = atomic_load_explicit(&g_diag_compile, memory_order_seq_cst);
    uint64_t unresolved = atomic_load_explicit(&g_diag_unresolved, memory_order_seq_cst);
    unsigned full = atomic_load_explicit(&g_diag_full, memory_order_seq_cst);
    uint64_t last = atomic_load_explicit(&g_diag_seq, memory_order_seq_cst);
    char line[320]; snprintf(line, sizeof line,
        "csf unimpl summary phase=%s frame=%llu kinds=%u compute_compile_failed=%llu compute_program_unresolved=%llu snapshot_stable=%d table_overflow=%u",
        phase, (unsigned long long)frame, kinds, (unsigned long long)compile, (unsigned long long)unresolved, first == last && !(first & 1), full);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "csf", line);
}
void nvn_unimpl_reset(void) {
    pthread_mutex_lock(&g_un_mu);
    atomic_fetch_add_explicit(&g_diag_seq, 1, memory_order_seq_cst);
    g_nun = 0; g_un_full = 0;
    atomic_store_explicit(&g_diag_on, -1, memory_order_relaxed);
    atomic_store_explicit(&g_diag_compile, 0, memory_order_seq_cst); atomic_store_explicit(&g_diag_unresolved, 0, memory_order_seq_cst);
    atomic_store_explicit(&g_diag_kinds, 0, memory_order_seq_cst); atomic_store_explicit(&g_diag_full, 0, memory_order_seq_cst);
    atomic_fetch_add_explicit(&g_diag_seq, 1, memory_order_seq_cst);
    pthread_mutex_unlock(&g_un_mu);
}
