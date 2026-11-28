// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

static pthread_mutex_t g_bc7_diag_mu = PTHREAD_MUTEX_INITIALIZER;

static void bc7_diag_summary_locked(uint64_t now, int final) {
    if (!final && now < atomic_load_explicit(&g.bc7_diag_next, memory_order_relaxed)) return;
    atomic_store_explicit(&g.bc7_diag_next, now + UINT64_C(5000000000), memory_order_relaxed);
    const uint64_t *n = g.bc7_diag_counts;
    char line[768];
    snprintf(line, sizeof line, "summary final=%d whole=%llu bc_bytes=%llu hit=%llu hit_bytes=%llu "
             "unique=%u unique_bytes=%llu unique_hit=%u unique_hit_bytes=%llu identity_lost=%llu "
             "missing_pack=%llu quality_reject=%llu key_miss=%llu non_whole=%llu sparse=%llu "
             "low_cap=%llu disabled=%llu plain=%llu layout=%llu bad_payload=%llu resource=%llu",
             final, (unsigned long long)g.bc7_diag_whole, (unsigned long long)g.bc7_diag_bytes,
             (unsigned long long)n[BC7_DIAG_HIT], (unsigned long long)g.bc7_diag_hit_bytes,
             g.bc7_diag_unique, (unsigned long long)g.bc7_diag_unique_bytes,
             g.bc7_diag_unique_hit, (unsigned long long)g.bc7_diag_unique_hit_bytes,
             (unsigned long long)g.bc7_diag_lost,
             (unsigned long long)n[BC7_DIAG_NO_PACK], (unsigned long long)n[BC7_DIAG_QUALITY],
             (unsigned long long)n[BC7_DIAG_KEY], (unsigned long long)n[BC7_DIAG_PARTIAL],
             (unsigned long long)n[BC7_DIAG_SPARSE], (unsigned long long)n[BC7_DIAG_LOW_CAP],
             (unsigned long long)n[BC7_DIAG_DISABLED], (unsigned long long)n[BC7_DIAG_PLAIN],
             (unsigned long long)n[BC7_DIAG_LAYOUT], (unsigned long long)n[BC7_DIAG_PAYLOAD],
             (unsigned long long)n[BC7_DIAG_RESOURCE]);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "bc7-workset", line);
}

static void bc7_diag_note(const struct nvn_gpu_tex_s *t, const uint8_t *key,
                          uint64_t bytes, enum bc7_diag_reason reason) {
    if (!g.bc7_diag_on) return;
    static const char *const names[] = {
        "hit", "missing_pack", "quality_reject", "key_miss", "non_whole", "sparse",
        "low_cap", "disabled", "plain", "layout", "bad_payload", "resource",
    };
    pthread_mutex_lock(&g_bc7_diag_mu);
    g.bc7_diag_counts[reason]++;
    if (key) {
        g.bc7_diag_whole++;
        g.bc7_diag_bytes += bytes;
        if (reason == BC7_DIAG_HIT) g.bc7_diag_hit_bytes += bytes;
        struct bc7_diag_key *seen = NULL;
        if (g.bc7_diag_seen) {
            const uint32_t mask = BC7_DIAG_SLOTS - 1;
            uint32_t i = (uint32_t)nvn_hm_hash(key, 32) & mask;
            for (; g.bc7_diag_seen[i].state; i = (i + 1) & mask) {
                if (!memcmp(g.bc7_diag_seen[i].key, key, 32)) {
                    seen = &g.bc7_diag_seen[i];
                    break;
                }
            }
            if (!seen && g.bc7_diag_unique < BC7_DIAG_SLOTS * 3 / 4) {
                seen = &g.bc7_diag_seen[i];
                memcpy(seen->key, key, 32);
                seen->state = 1;
                g.bc7_diag_unique++;
                g.bc7_diag_unique_bytes += bytes;
                char line[224], hex[65];
                static const char digits[] = "0123456789abcdef";
                for (uint32_t j = 0; j < 32; j++) {
                    hex[2*j] = digits[key[j] >> 4];
                    hex[2*j+1] = digits[key[j] & 15];
                }
                hex[64] = 0;
                snprintf(line, sizeof line, "image key=%s width=%u height=%u levels=%u layers=%u bc_bytes=%llu first=%s",
                         hex, t->w, t->h, t->levels, t->layers, (unsigned long long)bytes, names[reason]);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "bc7-workset", line);
            }
        }
        if (!seen) g.bc7_diag_lost++;
        else if (reason == BC7_DIAG_HIT && seen->state == 1) {
            seen->state = 2;
            g.bc7_diag_unique_hit++;
            g.bc7_diag_unique_hit_bytes += bytes;
        }
    }
    bc7_diag_summary_locked(now_ns(), 0);
    pthread_mutex_unlock(&g_bc7_diag_mu);
}
