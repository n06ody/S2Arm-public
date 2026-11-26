// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Included by nvn_unimpl.c so both the backend and replay use one session budget.
enum { AUDIT_KEYS = 64, AUDIT_REASON_BYTES = 64, AUDIT_PRESENT_INTERVAL = 300 };

static const char *const audit_kind_names[NVN_RA_KIND_COUNT] = {
    "descriptor", "view", "attachment", "skip", "unimpl", "sticky", "segment", "read_undefined", "potential_read"
};
static const struct nvn_render_audit_binding audit_no_binding = {
    .stage = -1, .set = -1, .binding = -1, .element = -1
};
static struct {
    struct audit_key {
        enum nvn_render_audit_kind kind;
        struct nvn_render_audit_binding binding;
        char reason[AUDIT_REASON_BYTES];
    } keys[AUDIT_KEYS];
    uint64_t total[NVN_RA_KIND_COUNT];
    uint64_t presents, repeats, overflow, empty;
    unsigned key_count, potential_keys;
} audit;
static pthread_mutex_t audit_mu = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local struct { uint64_t submit; unsigned op; } audit_context;
int nvn_render_audit_enabled;

static int audit_same_binding(const struct nvn_render_audit_binding *a, const struct nvn_render_audit_binding *b) {
    return a->program == b->program && a->stage == b->stage && a->set == b->set &&
           a->binding == b->binding && a->element == b->element && a->format == b->format &&
           a->target == b->target && a->aspect == b->aspect && a->level == b->level && a->layer == b->layer;
}

static void audit_format_summary(char *line, size_t cap, const char *phase) {
    snprintf(line, cap,
             "S2A_RENDER_AUDIT phase=%s presents=%llu distinct=%u repeats=%llu overflow=%llu empty=%llu descriptor=%llu view=%llu attachment=%llu skip=%llu unimpl=%llu sticky=%llu segment=%llu read_undefined=%llu potential_read=%llu",
             phase, (unsigned long long)audit.presents, audit.key_count, (unsigned long long)audit.repeats,
             (unsigned long long)audit.overflow, (unsigned long long)audit.empty,
             (unsigned long long)audit.total[NVN_RA_DESCRIPTOR], (unsigned long long)audit.total[NVN_RA_VIEW],
             (unsigned long long)audit.total[NVN_RA_ATTACHMENT], (unsigned long long)audit.total[NVN_RA_SKIP],
             (unsigned long long)audit.total[NVN_RA_UNIMPL], (unsigned long long)audit.total[NVN_RA_STICKY],
             (unsigned long long)audit.total[NVN_RA_SEGMENT],
             (unsigned long long)audit.total[NVN_RA_UNDEFINED_READ],
             (unsigned long long)audit.total[NVN_RA_POTENTIAL_READ]);
}

void nvn_render_audit_begin(int enabled) {
    if (!enabled) return;
    pthread_mutex_lock(&audit_mu);
    memset(&audit, 0, sizeof audit);
    __atomic_store_n(&nvn_render_audit_enabled, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&audit_mu);
    nvn_render_audit_context(0, 0);
}

void nvn_render_audit_end(void) {
    char line[480];
    pthread_mutex_lock(&audit_mu);
    __atomic_store_n(&nvn_render_audit_enabled, 0, __ATOMIC_RELEASE);
    audit_format_summary(line, sizeof line, "final");
    pthread_mutex_unlock(&audit_mu);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "render_audit", line);
}

void nvn_render_audit_context(uint64_t submit, unsigned op) {
    audit_context.submit = submit;
    audit_context.op = op;
}

void nvn_render_audit_get_context(uint64_t *submit, unsigned *op) {
    *submit = audit_context.submit;
    *op = audit_context.op;
}

void nvn_render_audit_present(void) {
    char line[480];
    pthread_mutex_lock(&audit_mu);
    int tick = nvn_render_audit_on() && !(++audit.presents % AUDIT_PRESENT_INTERVAL);
    if (tick) audit_format_summary(line, sizeof line, "progress");
    pthread_mutex_unlock(&audit_mu);
    if (tick) nvn_plat_log(NVN_PLAT_LOG_INFO, "render_audit", line);
}

void nvn_render_audit_empty(uint32_t count) {
    pthread_mutex_lock(&audit_mu);
    if (nvn_render_audit_on()) audit.empty += count;
    pthread_mutex_unlock(&audit_mu);
}

static void audit_note(enum nvn_render_audit_kind kind, const char *reason,
                       const struct nvn_render_audit_binding *binding, const char *detail,
                       uint64_t submit, unsigned op, uint64_t fence) {
    if (!binding) binding = &audit_no_binding;
    pthread_mutex_lock(&audit_mu);
    if (!nvn_render_audit_on()) { pthread_mutex_unlock(&audit_mu); return; }
    audit.total[kind]++;
    for (unsigned i = 0; i < audit.key_count; i++) {
        const struct audit_key *key = &audit.keys[i];
        if (key->kind == kind && audit_same_binding(binding, &key->binding) &&
            !strncmp(reason, key->reason, sizeof key->reason - 1)) {
            audit.repeats++;
            pthread_mutex_unlock(&audit_mu);
            return;
        }
    }
    if (audit.key_count == AUDIT_KEYS || (kind == NVN_RA_POTENTIAL_READ && audit.potential_keys == 16)) {
        audit.overflow++;
        pthread_mutex_unlock(&audit_mu);
        return;
    }
    struct audit_key *key = &audit.keys[audit.key_count++];
    if (kind == NVN_RA_POTENTIAL_READ) audit.potential_keys++;
    key->kind = kind;
    key->binding = *binding;
    snprintf(key->reason, sizeof key->reason, "%s", reason);
    const uint64_t frame = audit.presents;
    pthread_mutex_unlock(&audit_mu);

    char line[512];
    snprintf(line, sizeof line,
             "S2A_RENDER_AUDIT kind=%s reason=%.63s frame=%llu submit=%llu op=%u fence=%llu program=%016llx stage=%d set=%d binding=%d element=%d format=%u target=%u aspect=%#x mip=%u layer=%u detail=%.180s",
             audit_kind_names[kind], reason, (unsigned long long)frame, (unsigned long long)submit, op,
             (unsigned long long)fence, (unsigned long long)binding->program, binding->stage, binding->set,
             binding->binding, binding->element, binding->format, binding->target, binding->aspect,
             binding->level, binding->layer, detail ? detail : "");
    nvn_plat_log(kind == NVN_RA_POTENTIAL_READ ? NVN_PLAT_LOG_INFO : NVN_PLAT_LOG_RED, "render_audit", line);
}

void nvn_render_audit_event(enum nvn_render_audit_kind kind, const char *reason,
                            const struct nvn_render_audit_binding *binding, const char *detail) {
    audit_note(kind, reason, binding, detail, audit_context.submit, audit_context.op, 0);
}

void nvn_render_audit_event_at(enum nvn_render_audit_kind kind, const char *reason,
                               const struct nvn_render_audit_binding *binding, const char *detail,
                               uint64_t submit, unsigned op, uint64_t fence) {
    audit_note(kind, reason, binding, detail, submit, op, fence);
}

void nvn_render_audit_segment(int code, uint64_t fence, const char *detail) {
    char reason[32];
    snprintf(reason, sizeof reason, "segment_failed/rc=%d", code);
    audit_note(NVN_RA_SEGMENT, reason, NULL, detail, 0, 0, fence);
}
