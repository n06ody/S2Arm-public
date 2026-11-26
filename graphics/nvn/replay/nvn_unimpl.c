#include "nvn_unimpl.h"
#define S2A_RENDER_AUDIT_IMPLEMENTATION
#include "nvn_render_audit.h"
#include "nvn_plat.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#if NVN_RENDER_AUDIT_SUPPORTED
#include "nvn_render_audit.inc"
#endif

enum { UN_MAX = 256, UN_NAME = 48 };
static struct { char cls[12], name[UN_NAME]; uint64_t n; } g_un[UN_MAX];
static int g_nun, g_un_full;
static pthread_mutex_t g_un_mu = PTHREAD_MUTEX_INITIALIZER;

static int un_find(const char *cls, const char *name) {
    for (int i = 0; i < g_nun; i++) if (!strcmp(g_un[i].cls, cls) && !strcmp(g_un[i].name, name)) return i;
    return -1;
}

uint64_t nvn_unimpl(const char *cls, const char *name, const void *site) {
    if (!cls) cls = "?";
    if (!name) name = "?";
    NVN_RA_IF(
        char reason[64];
        snprintf(reason, sizeof reason, "%s/%s", cls, name);
        nvn_render_audit_event(NVN_RA_UNIMPL, reason, NULL, NULL);
    );
    char line[160]; int first = 0, full = 0; uint64_t n = 0;
    pthread_mutex_lock(&g_un_mu);
    int i = un_find(cls, name);
    if (i < 0 && g_nun < UN_MAX) {
        i = g_nun++; first = 1;
        snprintf(g_un[i].cls, sizeof g_un[i].cls, "%s", cls); snprintf(g_un[i].name, sizeof g_un[i].name, "%s", name);
        g_un[i].n = 0;
    }
    if (i >= 0) n = ++g_un[i].n; else if (!g_un_full) { g_un_full = full = 1; }
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

void nvn_unimpl_reset(void) { pthread_mutex_lock(&g_un_mu); g_nun = 0; g_un_full = 0; pthread_mutex_unlock(&g_un_mu); }
