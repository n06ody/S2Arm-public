// SPDX-License-Identifier: Apache-2.0
// One session-wide cached switch; the context itself belongs to the caller's
// thread and contains only copied keys, so a queued snapshot owns its data.
#include "nvn_vuid_diag.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#if (defined(__ANDROID__) && defined(S2A_ANDROID_DEBUG_ENV)) || defined(S2A_VUID_CONTEXT_TEST)
static _Atomic int enabled;
static _Atomic uint64_t sequence;
static _Thread_local struct s2a_vuid_context current;
void s2a_vuid_start(int on) { atomic_store_explicit(&enabled, !!on, memory_order_release); }
int s2a_vuid_enabled(void) { return atomic_load_explicit(&enabled, memory_order_acquire); }
uint64_t s2a_vuid_next(void) { return atomic_fetch_add_explicit(&sequence, 1, memory_order_relaxed) + 1; }
void s2a_vuid_exchange(const struct s2a_vuid_context *in, struct s2a_vuid_context *out) {
    if (out) *out = current;
    current = in ? *in : (struct s2a_vuid_context){0};
}
const struct s2a_vuid_context *s2a_vuid_current(void) { return &current; }

// A callback is synchronous with the API currently being called on this thread;
// submission and driver threads never inherit a guessed key. A scope names its
// program, the message names its entry point: "exact" needs both, a scope whose
// entry point is not one this scope makes is "partial" and prints the entry.
static size_t api_of(const char *msg, char *fn, size_t cap) {
    size_t n = 0;
    if (msg && msg[0] == 'v' && msg[1] == 'k')
        while (msg[n] && msg[n] != '(' && msg[n] != ' ' && msg[n] != ':' && n + 1 < cap) { fn[n] = msg[n]; n++; }
    if (!msg || msg[n] != '(') n = 0;
    fn[n] = 0; return n;
}
static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }
static int api_expected(const struct s2a_vuid_context *c, const char *fn) {
    if (!fn[0]) return 0;
    if (c->api[0]) return !strncmp(fn, c->api, sizeof c->api);   // a queued record replays exactly one entry point
    if (c->kind == S2A_VUID_COMPILE) return !strcmp(fn, "vkCreateShaderModule");
    if (c->kind == S2A_VUID_PIPE) return !strcmp(fn, "vkCreateGraphicsPipelines") || !strcmp(fn, "vkCreateComputePipelines");
    // The backend's draw / dispatch call binds this draw's resources and then draws
    int binds = !strcmp(fn, "vkUpdateDescriptorSets") || !strcmp(fn, "vkCmdBindDescriptorSets") || starts(fn, "vkCmdPushDescriptorSet") ||
                !strcmp(fn, "vkCmdBindPipeline") || !strcmp(fn, "vkCmdPushConstants");
    if (c->kind == S2A_VUID_DRAW) return binds || starts(fn, "vkCmdDraw") || starts(fn, "vkCmdSet") || !strcmp(fn, "vkCmdBindVertexBuffers") || !strcmp(fn, "vkCmdBindIndexBuffer");
    if (c->kind == S2A_VUID_DISPATCH) return binds || starts(fn, "vkCmdDispatch");
    return 0;
}
enum { VUID_SHADER_MAX = 8192 };   // open addressing; a full table drops the note (the pipeline line then says unknown)
static struct { const void *sh; uint8_t st, key[8]; } shaders[VUID_SHADER_MAX];
static pthread_mutex_t shaders_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned shader_slot(const void *sh) { return (unsigned)(((uintptr_t)sh >> 4) * 2654435761u) % VUID_SHADER_MAX; }
void s2a_vuid_note_shader(const void *sh, unsigned st, const uint8_t *key) {
    if (!s2a_vuid_enabled() || !sh || !key || st > 5) return;
    pthread_mutex_lock(&shaders_mu);
    for (unsigned k = 0, i = shader_slot(sh); k < VUID_SHADER_MAX; k++, i = (i + 1) % VUID_SHADER_MAX)
        if (!shaders[i].sh || shaders[i].sh == sh) { shaders[i].sh = sh; shaders[i].st = (uint8_t)st; memcpy(shaders[i].key, key, 8); break; }
    pthread_mutex_unlock(&shaders_mu);
}
void s2a_vuid_forget_shader(const void *sh) {
    if (!sh) return;
    pthread_mutex_lock(&shaders_mu);
    unsigned i = shader_slot(sh), k = 0;
    for (; k < VUID_SHADER_MAX && shaders[i].sh && shaders[i].sh != sh; k++) i = (i + 1) % VUID_SHADER_MAX;
    if (k < VUID_SHADER_MAX && shaders[i].sh == sh) {   // backward-shift deletion keeps every probe chain intact
        for (unsigned j = (i + 1) % VUID_SHADER_MAX; shaders[j].sh; j = (j + 1) % VUID_SHADER_MAX) {
            unsigned h = shader_slot(shaders[j].sh);
            if ((j > i && (h <= i || h > j)) || (j < i && h <= i && h > j)) { shaders[i] = shaders[j]; i = j; }
        }
        shaders[i].sh = NULL;
    }
    pthread_mutex_unlock(&shaders_mu);
}
int s2a_vuid_pipe_begin(const void *const *sh, unsigned n, struct s2a_vuid_context *old) {
    if (!s2a_vuid_enabled() || !sh) return 0;
    struct s2a_vuid_context c = {.kind = S2A_VUID_PIPE, .sequence = s2a_vuid_next()};
    pthread_mutex_lock(&shaders_mu);
    for (unsigned s = 0; s < n; s++) if (sh[s]) {
        unsigned i = shader_slot(sh[s]), k = 0;
        for (; k < VUID_SHADER_MAX && shaders[i].sh && shaders[i].sh != sh[s]; k++) i = (i + 1) % VUID_SHADER_MAX;
        unsigned st = k < VUID_SHADER_MAX && shaders[i].sh == sh[s] ? shaders[i].st : s;
        c.bound |= (uint8_t)(1u << st);
        if (k < VUID_SHADER_MAX && shaders[i].sh == sh[s]) { memcpy(c.keys[st], shaders[i].key, 8); c.stages |= (uint8_t)(1u << st); }
    }
    pthread_mutex_unlock(&shaders_mu);
    s2a_vuid_exchange(&c, old); return 1;
}
enum { VUID_CONTEXT_MAX = 512 };
static struct { int32_t id; struct s2a_vuid_context c; uint64_t n; int api_ok; } seen[VUID_CONTEXT_MAX];
static unsigned seen_n;
static uint64_t overflow;
static pthread_mutex_t seen_mu = PTHREAD_MUTEX_INITIALIZER;
void s2a_vuid_reset_report(void) {
    pthread_mutex_lock(&seen_mu); seen_n = 0; overflow = 0; pthread_mutex_unlock(&seen_mu);
}
int s2a_vuid_observe(int32_t id, const char *name, const char *msg, char *out, size_t cap) {
    if (!s2a_vuid_enabled() || !out || !cap) return 0;
    struct s2a_vuid_context c = current;
    char fn[48]; api_of(msg, fn, sizeof fn);
    const int api_ok = c.kind != S2A_VUID_NONE && api_expected(&c, fn);
    if (c.kind == S2A_VUID_NONE) c = (struct s2a_vuid_context){0};
    uint64_t sequence = c.sequence, frame = c.frame; c.sequence = c.frame = 0; // count by key, not by draw number
    pthread_mutex_lock(&seen_mu);
    unsigned i = 0;
    for (; i < seen_n; i++) if (seen[i].id == id && seen[i].c.kind == c.kind && seen[i].c.stages == c.stages &&
        seen[i].c.bound == c.bound && seen[i].api_ok == api_ok && !memcmp(seen[i].c.keys, c.keys, sizeof c.keys)) break;
    uint64_t n;
    int full = 0;
    if (i == seen_n && seen_n == VUID_CONTEXT_MAX) { n = ++overflow; full = 1; }
    else {
        if (i == seen_n) { seen[i].id = id; seen[i].c = c; seen[i].n = 0; seen[i].api_ok = api_ok; seen_n++; }
        n = ++seen[i].n;
    }
    pthread_mutex_unlock(&seen_mu);
    if (n & (n - 1)) return 0;
    if (full) {
        snprintf(out, cap, "vk 校验程序关联：表满，未记录关联累计 %llu 次", (unsigned long long)n);
        return 1;
    }
    const char *quality = !c.kind || !c.stages ? "unknown" : c.bound == c.stages && api_ok ? "exact" : "partial";
    int used = snprintf(out, cap, "vk 校验程序关联 %s %s kind=%u api=%s%s frame=%llu seq=%llu count=%llu", name ? name : "?", quality,
                        c.kind, fn[0] ? fn : "?", c.kind && !api_ok ? "(不是本范围的入口)" : "", (unsigned long long)frame, (unsigned long long)sequence, (unsigned long long)n);
    size_t at = used > 0 && (size_t)used < cap ? (size_t)used : cap - 1;
    for (unsigned st = 0; st < 6 && at < cap - 1; st++) if (c.bound & (1u << st)) {
        int len = snprintf(out + at, cap - at, " st%u=", st);
        if (len < 0 || (size_t)len >= cap - at) { at = cap - 1; break; }
        at += (size_t)len;
        if (!(c.stages & (1u << st))) {
            len = snprintf(out + at, cap - at, "unknown");
            if (len < 0 || (size_t)len >= cap - at) { at = cap - 1; break; }
            at += (size_t)len;
        } else for (unsigned j = 0; j < 8 && at + 2 < cap; j++) {
            len = snprintf(out + at, cap - at, "%02x", c.keys[st][j]);
            if (len != 2) { at = cap - 1; break; }
            at += 2;
        }
    }
    if (n == 1 && at < cap - 1) snprintf(out + at, cap - at, " message=%.700s", msg ? msg : "");
    return 1;
}
#endif
