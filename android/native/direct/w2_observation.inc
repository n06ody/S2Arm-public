// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Included once by provider_vk.c. All windows, references and TLS share this implementation.
#include "w2_observation.h"

#if defined(S2A_ANDROID_VK_ENGINE) && S2A_ANDROID_VK_ENGINE && \
    defined(S2A_ANDROID_VK_BACKEND_NVN_VK) && S2A_ANDROID_VK_BACKEND_NVN_VK && \
    defined(S2A_ANDROID_DEBUG_ENV)
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct s2a_w2_window {
    struct s2a_w2_snapshot data;
    uint64_t references, receipt_revision;
    int owner_released;
};
static pthread_mutex_t w2_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int w2_enabled;
static struct s2a_w2_window *w2_current;
static uint64_t w2_instance;
static _Thread_local struct s2a_w2_scope *w2_scope;
static _Thread_local struct s2a_w2_shader *w2_shader;

static void w2_changed(struct s2a_w2_window *w) {
    if (w->data.revision != UINT64_MAX) ++w->data.revision;
    else ++w->data.overflow;
    w->receipt_revision = 0;
}
static void w2_gap(struct s2a_w2_window *w) {
    ++w->data.gaps;
    w2_changed(w);
}
static void w2_protocol(struct s2a_w2_window *w) {
    ++w->data.protocol_errors;
    ++w->data.rejects;
    w2_gap(w);
}
static void w2_unref(struct s2a_w2_window *w) {
    if (--w->references == 0) free(w);
}
static int w2_context_valid(const struct s2a_direct_compile_context *c) {
    if (!c || !c->session || !c->generation || !c->direct_abi ||
        !c->translator_closure || !c->translator_closure[0] ||
        strlen(c->translator_closure) >= 128 || !c->profile_hash ||
        strlen(c->profile_hash) != 71 || memcmp(c->profile_hash, "sha256-", 7)) return 0;
    for (int i = 7; i < 71; ++i) {
        char ch = c->profile_hash[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return 0;
    }
    return 1;
}
static int w2_context_matches(struct s2a_w2_window *w,
                               const struct s2a_direct_compile_context *c) {
    return w2_context_valid(c) && c->session == w->data.session &&
        c->generation == w->data.generation && c->direct_abi == w->data.direct_abi &&
        !strcmp(c->translator_closure, w->data.closure) &&
        !strcmp(c->profile_hash, w->data.profile_hash);
}
static int w2_metadata_complete(const struct s2a_w2_snapshot *d) {
    return d->state == S2A_W2_SEALED && d->stop_ok && d->drained &&
        !d->inflight_scopes && !d->inflight_shaders && d->shaders > 0 &&
        d->shaders == d->finished_shaders && d->observations == d->shaders &&
        d->accepted_builds == d->shaders && !d->gaps && !d->rejects &&
        !d->overflow && !d->late && !d->unbound && !d->missing_identity &&
        !d->cohort_mismatch && !d->build_failures && !d->protocol_errors &&
        !d->stop_failures && !d->drain_failures && !d->output_failures;
}
static void w2_copy_snapshot(struct s2a_w2_window *w, struct s2a_w2_snapshot *s) {
    *s = w->data;
    s->metadata_complete = w2_metadata_complete(s);
    s->window_complete = s->metadata_complete && w->receipt_revision == s->revision;
}

struct s2a_w2_window *s2a_w2_start(int enabled,
                                  const struct s2a_direct_compile_context *c) {
    int saved_errno = errno;
    if (!enabled || !w2_context_valid(c)) { errno = saved_errno; return NULL; }
    struct s2a_w2_window *w = calloc(1, sizeof *w);
    if (!w) { errno = saved_errno; return NULL; }
    pthread_mutex_lock(&w2_mutex);
    if (w2_current || w2_instance == UINT64_MAX) {
        if (w2_current) w2_protocol(w2_current);
        pthread_mutex_unlock(&w2_mutex);
        free(w); errno = saved_errno; return NULL;
    }
    w->references = 1;
    w->data.instance = ++w2_instance;
    w->data.revision = 1;
    w->data.session = c->session;
    w->data.generation = c->generation;
    w->data.direct_abi = c->direct_abi;
    memcpy(w->data.closure, c->translator_closure, strlen(c->translator_closure) + 1);
    memcpy(w->data.profile_hash, c->profile_hash, sizeof w->data.profile_hash);
    w->data.state = S2A_W2_ADMIT;
    w2_current = w;
    atomic_store_explicit(&w2_enabled, 1, memory_order_release);
    pthread_mutex_unlock(&w2_mutex);
    errno = saved_errno;
    return w;
}
void s2a_w2_close(struct s2a_w2_window *w) {
    int saved_errno = errno;
    if (w) {
        pthread_mutex_lock(&w2_mutex);
        if (w->data.state == S2A_W2_ADMIT) {
            w->data.state = S2A_W2_CLOSING;
            w2_changed(w);
        }
        pthread_mutex_unlock(&w2_mutex);
    }
    errno = saved_errno;
}
int s2a_w2_seal(struct s2a_w2_window *w, int stop_ok, int drained) {
    int saved_errno = errno, result = -1;
    if (w) {
        pthread_mutex_lock(&w2_mutex);
        if (!stop_ok) { ++w->data.stop_failures; w2_gap(w); }
        if (!drained) { ++w->data.drain_failures; w2_gap(w); }
        if (w->data.state == S2A_W2_ADMIT) w2_protocol(w);
        else if (w->data.inflight_scopes || w->data.inflight_shaders) {
            // Merely being in flight is not a gap: the old lease may still finish.
            result = -1;
        } else if (stop_ok && drained) {
            if (w->data.state != S2A_W2_SEALED) {
                w->data.stop_ok = 1;
                w->data.drained = 1;
                w->data.state = S2A_W2_SEALED;
                w2_changed(w);
            }
            result = 0;
        }
        pthread_mutex_unlock(&w2_mutex);
    }
    errno = saved_errno;
    return result;
}
void s2a_w2_release(struct s2a_w2_window *w) {
    int saved_errno = errno;
    if (w) {
        pthread_mutex_lock(&w2_mutex);
        if (!w->owner_released) {
            if (w->data.inflight_scopes || w->data.inflight_shaders) w2_protocol(w);
            w->owner_released = 1;
            if (w->data.state == S2A_W2_ADMIT) w->data.state = S2A_W2_CLOSING;
            if (w2_current == w) {
                w2_current = NULL;
                atomic_store_explicit(&w2_enabled, 0, memory_order_release);
            }
            w2_unref(w);
        }
        pthread_mutex_unlock(&w2_mutex);
    }
    errno = saved_errno;
}
void s2a_w2_scope_enter(struct s2a_w2_scope *s) {
    int saved_errno = errno;
    if (!s) { errno = saved_errno; return; }
    if (!atomic_load_explicit(&w2_enabled, memory_order_acquire) && !w2_scope) {
        memset(s, 0, sizeof *s); errno = saved_errno; return;
    }
    pthread_mutex_lock(&w2_mutex);
    if (w2_scope == s) {
        if (s->window) w2_protocol(s->window);
        s->blocked = 1;
    } else {
        memset(s, 0, sizeof *s);
        s->previous = w2_scope;
        s->entered = 1;
        s->window = w2_scope ? w2_scope->window : w2_current;
        if (s->window) {
            struct s2a_w2_window *w = s->window;
            ++w->references;
            ++w->data.inflight_scopes;
            ++w->data.scopes;
            if (w2_scope || w->data.state != S2A_W2_ADMIT || w->owner_released) {
                s->blocked = 1;
                if (w->data.state == S2A_W2_SEALED) ++w->data.late;
                w2_protocol(w);
            } else w2_changed(w);
        } else s->blocked = 1;
        w2_scope = s;
    }
    pthread_mutex_unlock(&w2_mutex);
    errno = saved_errno;
}
void s2a_w2_scope_expect_shader(struct s2a_w2_scope *s) {
    int saved_errno = errno;
    if (s && s->entered && s->window) {
        pthread_mutex_lock(&w2_mutex);
        if (w2_scope != s) w2_protocol(s->window);
        ++s->expected_shaders;
        w2_changed(s->window);
        pthread_mutex_unlock(&w2_mutex);
    }
    errno = saved_errno;
}
void s2a_w2_scope_leave(struct s2a_w2_scope *s, int success) {
    int saved_errno = errno;
    if (s && s->entered) {
        pthread_mutex_lock(&w2_mutex);
        struct s2a_w2_window *w = s->window;
        if (w) {
            if (!success || s->expected_shaders > s->begun_shaders) {
                ++w->data.rejects; w2_gap(w);
            }
            if (w2_scope != s || s->active_shaders) w2_protocol(w);
            // Broken callers cannot leave shader tokens pointing to an expired stack scope.
            for (struct s2a_w2_shader *t = w2_shader; t; t = t->previous)
                if (t->scope == s) t->scope = NULL;
            --w->data.inflight_scopes;
            w2_changed(w);
        }
        if (w2_scope == s) w2_scope = s->previous;
        else {
            for (struct s2a_w2_scope *p = w2_scope; p; p = p->previous)
                if (p->previous == s) { p->previous = s->previous; break; }
        }
        s->entered = 0;
        s->window = NULL;
        if (w) w2_unref(w);
        pthread_mutex_unlock(&w2_mutex);
    }
    errno = saved_errno;
}
void s2a_w2_shader_begin(struct s2a_w2_shader *t, enum s2a_w2_kind kind) {
    int saved_errno = errno;
    if (!t) { errno = saved_errno; return; }
    if (!atomic_load_explicit(&w2_enabled, memory_order_acquire) && !w2_scope && !w2_shader) {
        memset(t, 0, sizeof *t); errno = saved_errno; return;
    }
    pthread_mutex_lock(&w2_mutex);
    if (w2_shader == t) {
        if (t->window) w2_protocol(t->window);
        t->blocked = 1;
    } else {
        memset(t, 0, sizeof *t);
        t->previous = w2_shader;
        t->entered = 1;
        t->kind = (unsigned)kind;
        t->scope = w2_scope;
        t->window = w2_scope ? w2_scope->window : w2_current;
        if (t->window) {
            struct s2a_w2_window *w = t->window;
            ++w->references;
            ++w->data.shaders;
            ++w->data.inflight_shaders;
            if (t->scope) {
                ++t->scope->begun_shaders;
                ++t->scope->active_shaders;
            } else { ++w->data.unbound; t->blocked = 1; w2_gap(w); }
            if ((t->scope && t->scope->blocked) || w2_shader || w->owner_released ||
                (kind != S2A_W2_GRAPHICS && kind != S2A_W2_COMPUTE)) {
                t->blocked = 1; w2_protocol(w);
            }
            if (w->data.state == S2A_W2_SEALED) {
                t->blocked = 1; ++w->data.late; w2_gap(w);
            }
            w2_changed(w);
        } else t->blocked = 1;
        w2_shader = t;
    }
    pthread_mutex_unlock(&w2_mutex);
    errno = saved_errno;
}
void s2a_w2_shader_lookup(const struct s2a_direct_compile_context *c,
                          const uint8_t identity[32], enum s2a_w2_source source,
                          int accepted) {
    int saved_errno = errno;
    if (!atomic_load_explicit(&w2_enabled, memory_order_acquire) && !w2_shader) {
        errno = saved_errno; return;
    }
    pthread_mutex_lock(&w2_mutex);
    struct s2a_w2_shader *t = w2_shader;
    struct s2a_w2_window *w = t ? t->window : w2_current;
    if (w && (!t || t->blocked || t->lookup_seen)) {
        if (!t) ++w->data.unbound;
        if (w->data.state == S2A_W2_SEALED) ++w->data.late;
        w2_protocol(w);
    } else if (w) {
        t->lookup_seen = 1;
        if (!w2_context_matches(w, c)) {
            ++w->data.cohort_mismatch; ++w->data.rejects; t->rejected = 1;
            w2_gap(w);
            goto lookup_done; // Never attribute another cohort's identity to this window.
        }
        if (!identity) { ++w->data.missing_identity; accepted = 0; w2_gap(w); }
        else {
            ++w->data.observations;
            uint32_t i;
            for (i = 0; i < w->data.entry_count; ++i)
                if (w->data.entries[i].kind == t->kind &&
                    !memcmp(w->data.entries[i].identity, identity, 32)) break;
            if (i == w->data.entry_count) {
                if (i == S2A_W2_ENTRY_CAPACITY) { ++w->data.overflow; w2_gap(w); }
                else {
                    ++w->data.entry_count;
                    memcpy(w->data.entries[i].identity, identity, 32);
                    w->data.entries[i].kind = t->kind;
                }
            }
            if (source >= S2A_W2_MEMO && source <= S2A_W2_LIVE) {
                if (i < S2A_W2_ENTRY_CAPACITY) {
                    if (accepted) {
                        ++w->data.entries[i].source_counts[source];
                        t->lookup_slot = i + 1;
                        t->lookup_source = (unsigned)source + 1;
                    } else ++w->data.entries[i].rejected_source_counts[source];
                }
            } else {
                if (i < S2A_W2_ENTRY_CAPACITY) ++w->data.entries[i].unknown_rejects;
                accepted = 0; w2_gap(w);
            }
        }
        t->lookup_accepted = !!accepted;
        if (!accepted) { ++w->data.rejects; t->rejected = 1; w2_gap(w); }
        w2_changed(w);
    }
lookup_done:
    pthread_mutex_unlock(&w2_mutex);
    errno = saved_errno;
}
void s2a_w2_shader_end(struct s2a_w2_shader *t, int success) {
    int saved_errno = errno;
    if (t && t->entered) {
        pthread_mutex_lock(&w2_mutex);
        struct s2a_w2_window *w = t->window;
        if (w) {
            if (w2_shader != t) w2_protocol(w);
            int retry = success < 0 && w2_shader == t && t->scope && !t->blocked && !t->rejected &&
                t->lookup_seen && t->lookup_accepted && t->lookup_slot && t->lookup_source &&
                t->lookup_slot <= w->data.entry_count && t->lookup_source <= S2A_W2_SOURCE_COUNT &&
                w->data.shaders && w->data.observations && t->scope->begun_shaders &&
                w->data.entries[t->lookup_slot-1].source_counts[t->lookup_source-1];
            if (retry) {
                // 只是将同一 dispatch 推迟到同步后重建：撤销本次有效尝试的计数，
                // 真正的重建另记一次。已经出现的 reject/gap 绝不撤销。
                --w->data.shaders;
                --w->data.observations;
                --w->data.entries[t->lookup_slot-1].source_counts[t->lookup_source-1];
                --t->scope->begun_shaders;
            } else {
                if (success < 0) success = 0;
                if (!t->lookup_seen) { ++w->data.missing_identity; w2_gap(w); }
                if (!success) { ++w->data.build_failures; w2_gap(w); }
                if ((t->blocked || !t->lookup_seen || !success) && !t->rejected)
                    ++w->data.rejects;
                if (success && t->lookup_seen && t->lookup_accepted && !t->blocked)
                    ++w->data.accepted_builds;
                ++w->data.finished_shaders;
            }
            --w->data.inflight_shaders;
            if (t->scope) --t->scope->active_shaders;
            w2_changed(w);
        }
        if (w2_shader == t) w2_shader = t->previous;
        else {
            for (struct s2a_w2_shader *p = w2_shader; p; p = p->previous)
                if (p->previous == t) { p->previous = t->previous; break; }
        }
        t->entered = 0;
        t->window = NULL;
        if (w) w2_unref(w);
        pthread_mutex_unlock(&w2_mutex);
    }
    errno = saved_errno;
}
int s2a_w2_snapshot(struct s2a_w2_window *w, struct s2a_w2_snapshot *s) {
    int saved_errno = errno, result = -1;
    if (w && s) {
        pthread_mutex_lock(&w2_mutex);
        w2_copy_snapshot(w, s);
        pthread_mutex_unlock(&w2_mutex);
        result = 0;
    }
    errno = saved_errno;
    return result;
}
int s2a_w2_ack(struct s2a_w2_window *w, const struct s2a_w2_snapshot *s, int output_ok) {
    int saved_errno = errno, result = -1;
    if (w) {
        pthread_mutex_lock(&w2_mutex);
        if (!output_ok || !s || s->instance != w->data.instance ||
            s->revision != w->data.revision) {
            ++w->data.output_failures;
            w2_gap(w);
        } else if (w2_metadata_complete(&w->data)) {
            w->receipt_revision = s->revision;
            result = 0;
        }
        pthread_mutex_unlock(&w2_mutex);
    }
    errno = saved_errno;
    return result;
}

// Bounded serialization of metadata only. No filesystem or caller sink runs under w2_mutex.
struct w2_json { char *text; size_t used, capacity; int failed; };
static void w2_json_add(struct w2_json *j, const char *fmt, ...) {
    if (j->failed) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->text + j->used, j->capacity - j->used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= j->capacity - j->used) j->failed = 1;
    else j->used += (size_t)n;
}
static void w2_json_string(struct w2_json *j, const char *s) {
    w2_json_add(j, "\"");
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') w2_json_add(j, "\\%c", c);
        else if (c < 32 || c >= 127) w2_json_add(j, "\\u%04x", (unsigned)c);
        else w2_json_add(j, "%c", c);
    }
    w2_json_add(j, "\"");
}
static void w2_json_snapshot(struct w2_json *j, const struct s2a_w2_snapshot *s) {
    w2_json_add(j, "{\"schema\":\"s2a-w2-observation-v1\",\"instance\":%" PRIu64
        ",\"revision\":%" PRIu64 ",\"session\":%" PRIu64 ",\"generation\":%" PRIu64
        ",\"directABI\":%u,\"closure\":", s->instance, s->revision, s->session,
        s->generation, s->direct_abi);
    w2_json_string(j, s->closure);
    w2_json_add(j, ",\"profileHash\":"); w2_json_string(j, s->profile_hash);
    w2_json_add(j, ",\"state\":%u,\"sealed\":%s,\"drained\":%s,\"metadata_eligible\":%s,"
        "\"window_complete\":false,\"receipt_not_persisted\":true,"
        "\"request_set_complete\":\"not_proven\",\"strict_acceptance\":\"not_proven\","
        "\"gpu_execution\":\"not_proven\",\"w2_acceptance\":\"not_proven\",\"counts\":{",
        s->state, s->state == S2A_W2_SEALED ? "true" : "false", s->drained ? "true" : "false",
        s->metadata_complete ? "true" : "false");
#define W2_JSON_COUNT(name, sep) w2_json_add(j, "\"" #name "\":%" PRIu64 sep, s->name)
    W2_JSON_COUNT(scopes, ","); W2_JSON_COUNT(inflight_scopes, ",");
    W2_JSON_COUNT(shaders, ","); W2_JSON_COUNT(finished_shaders, ",");
    W2_JSON_COUNT(inflight_shaders, ","); W2_JSON_COUNT(observations, ",");
    W2_JSON_COUNT(accepted_builds, ","); W2_JSON_COUNT(gaps, ",");
    W2_JSON_COUNT(rejects, ","); W2_JSON_COUNT(overflow, ","); W2_JSON_COUNT(late, ",");
    W2_JSON_COUNT(unbound, ","); W2_JSON_COUNT(missing_identity, ",");
    W2_JSON_COUNT(cohort_mismatch, ","); W2_JSON_COUNT(build_failures, ",");
    W2_JSON_COUNT(protocol_errors, ","); W2_JSON_COUNT(stop_failures, ",");
    W2_JSON_COUNT(drain_failures, ","); W2_JSON_COUNT(output_failures, "},\"entries\":[");
#undef W2_JSON_COUNT
    for (uint32_t i = 0; i < s->entry_count; ++i) {
        const struct s2a_w2_entry *e = &s->entries[i];
        w2_json_add(j, "%s{\"identity\":\"", i ? "," : "");
        for (size_t k = 0; k < sizeof e->identity; ++k) w2_json_add(j, "%02x", e->identity[k]);
        w2_json_add(j, "\",\"kind\":\"%s\",\"sources\":{\"memo\":%" PRIu64
            ",\"memory\":%" PRIu64 ",\"offline\":%" PRIu64 ",\"private\":%" PRIu64
            ",\"live\":%" PRIu64 "},\"rejected_sources\":{\"memo\":%" PRIu64
            ",\"memory\":%" PRIu64 ",\"offline\":%" PRIu64 ",\"private\":%" PRIu64
            ",\"live\":%" PRIu64 ",\"unknown\":%" PRIu64 "}}", e->kind == S2A_W2_GRAPHICS ? "graphics" : "compute",
            e->source_counts[0], e->source_counts[1], e->source_counts[2],
            e->source_counts[3], e->source_counts[4], e->rejected_source_counts[0],
            e->rejected_source_counts[1], e->rejected_source_counts[2],
            e->rejected_source_counts[3], e->rejected_source_counts[4], e->unknown_rejects);
    }
    w2_json_add(j, "]}\n");
}
int s2a_w2_export(struct s2a_w2_window *w,
                   int (*writer)(void *, const char *, size_t), void *opaque) {
    int saved_errno = errno, result = -1;
    if (!w) { errno = saved_errno; return -1; }
    if (!writer) {
        (void)s2a_w2_ack(w, NULL, 0);
        errno = saved_errno; return -1;
    }
    struct s2a_w2_snapshot *s = malloc(sizeof *s);
    struct w2_json j = {NULL, 0, 65536, 0};
    j.text = malloc(j.capacity);
    pthread_mutex_lock(&w2_mutex);
    ++w->references;
    if (s) w2_copy_snapshot(w, s);
    pthread_mutex_unlock(&w2_mutex);
    if (s && j.text) {
        w2_json_snapshot(&j, s);
        if (!j.failed) result = writer(opaque, j.text, j.used);
    }
    // A successful incomplete snapshot is evidence, not an attempted complete receipt.
    // Old leases may legitimately finish during its I/O; that progress is not an I/O failure.
    if (result != 0 || (s && s->metadata_complete)) result = s2a_w2_ack(w, s, result == 0);
    else result = -1;
    free(j.text);
    free(s);
    pthread_mutex_lock(&w2_mutex);
    w2_unref(w);
    pthread_mutex_unlock(&w2_mutex);
    errno = saved_errno;
    return result;
}

#endif // Debug NVN Vulkan observation only.
