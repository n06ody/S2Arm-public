// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

static const char *const cold_diag_names[COLD_DIAG_REASONS] = {
    [COLD_DIAG_DOMAIN] = "domain",
    [COLD_DIAG_UNTRACKED] = "untracked_storage",
    [COLD_DIAG_PRE_IS_MAIN] = "pre_is_main",
    [COLD_DIAG_MAIN_EVENTS_LOST] = "main_events_lost",
    [COLD_DIAG_PRE_EVENTS_LOST] = "pre_events_lost",
    [COLD_DIAG_PRE_ERROR] = "pre_error",
    [COLD_DIAG_SESSION_WRITES_LOST] = "session_writes_lost",
    [COLD_DIAG_EVENT_LIMIT] = "committed_event_limit",
    [COLD_DIAG_EVENT_OOM] = "committed_event_oom",
    [COLD_DIAG_HOST_SPAN_LIMIT] = "host_span_limit",
    [COLD_DIAG_COMMITTED_SPAN_LIMIT] = "committed_span_limit",
    [COLD_DIAG_SNAPSHOT_SPAN_LIMIT] = "snapshot_span_limit",
    [COLD_DIAG_CMD_ERROR] = "command_error",
    [COLD_DIAG_PASS_STATE] = "pass_open",
    [COLD_DIAG_QUERY] = "query",
    [COLD_DIAG_DRAW_DIAG] = "draw_diagnostic",
    [COLD_DIAG_DRAW_SHAPE] = "draw_shape",
    [COLD_DIAG_PIPELINES] = "pipelines",
    [COLD_DIAG_PREFIX] = "prefix",
    [COLD_DIAG_SSBO_BOUNDS] = "ssbo_bounds",
    [COLD_DIAG_PUBLICATION_INVALID] = "publication_invalid",
    [COLD_DIAG_PUBLICATION_GAP] = "publication_gap",
    [COLD_DIAG_SELECTOR] = "selector",
    [COLD_DIAG_ALIGNMENT] = "alignment",
    [COLD_DIAG_ARENA] = "arena",
    [COLD_DIAG_DESCRIPTORS] = "descriptors",
    [COLD_DIAG_DRAW_STATE] = "draw_state",
    [COLD_DIAG_SOURCE_CHANGED] = "source_changed",
    [COLD_DIAG_SSBO_CHANGED] = "ssbo_changed",
    [COLD_DIAG_PREFIX_CHANGED] = "prefix_changed",
};

static inline int cold_diag_reject(struct nvn_gpu_cmd_s *c, enum cold_diag_reason reason) {
    if (!g.cold_diag_enabled ||
        atomic_fetch_add_explicit(&g.cold_diag_counts[reason], 1, memory_order_relaxed)) return 0;
    if (c && !c->cold_diag_record)
        c->cold_diag_record = atomic_fetch_add_explicit(&g.cold_diag_records, 1, memory_order_relaxed) + 1;
    const uint32_t present = atomic_load_explicit(&g.cold_present_count, memory_order_relaxed);
    char line[192];
    snprintf(line, sizeof line, "nvk-cold-guard-diag first reason=%s record=%llu presents_before=%u",
             cold_diag_names[reason], (unsigned long long)(c ? c->cold_diag_record : 0), present);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    return 0;
}

static void cold_diag_enqueue_choice(struct nvn_gpu_cmd_s *c, const nvn_gpu_draw_args *draw) {
    if (!c->cold_diag_choices)
        c->cold_diag_choices = calloc(COLD_SETS_PER_SEGMENT, sizeof *c->cold_diag_choices);
    if (!c->cold_diag_choices || c->cold_diag_nchoices == COLD_SETS_PER_SEGMENT) {
        atomic_fetch_add_explicit(&g.cold_diag_unread, 1, memory_order_relaxed);
        return;
    }
    c->cold_diag_choices[c->cold_diag_nchoices++] = (struct cold_diag_choice){
        c->cold.result.cpu, draw->count, draw->first,
    };
}

static void cold_diag_finish(struct nvn_gpu_cmd_s *c, uint64_t signal) {
    // The selector writes into this command's coherent arena. The normal
    // commit already publishes GPU writes to HOST before the successful
    // fence wait. Read before finish_cmd recycles the arena.
    for (uint32_t i = 0; i < c->cold_diag_nchoices; i++) {
        const struct cold_diag_choice *choice = &c->cold_diag_choices[i];
        VkDrawIndirectCommand args[2];
        memcpy(args, choice->mapped, sizeof args);
        if (args[0].vertexCount != choice->vertex_count || args[1].vertexCount != choice->vertex_count ||
            args[0].firstVertex != choice->first_vertex || args[1].firstVertex != choice->first_vertex ||
            args[0].firstInstance || args[1].firstInstance ||
            args[0].instanceCount > 1 || args[1].instanceCount != 1 - args[0].instanceCount) {
            g.cold_diag_invalid++;
        } else if (args[0].instanceCount) {
            g.cold_diag_a++;
        } else {
            g.cold_diag_b++;
        }
    }
    g.cold_diag_through_signal = signal;
    c->cold_diag_nchoices = 0;
}

static void cold_diag_retire(struct nvn_gpu_cmd_s *c) {
    if (c->cold_diag_record) {
        char line[160];
        snprintf(line, sizeof line, "nvk-cold-guard-diag segment record=%llu signal=%llu submitted=%d",
                 (unsigned long long)c->cold_diag_record, (unsigned long long)c->segment.signal,
                 c->segment.submitted);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    }
    if (c->cold_diag_nchoices)  // Abandoned segment or failed fence wait.
        atomic_fetch_add_explicit(&g.cold_diag_unread, c->cold_diag_nchoices, memory_order_relaxed);
    free(c->cold_diag_choices);
}

// The completion worker owns the choice tallies. The final snapshot runs
// after joining it. Reject counters also include any more recent recording.
static void cold_diag_report(uint64_t wsi_seq, int final) {
    char line[256];
    const uint64_t unread = atomic_load_explicit(&g.cold_diag_unread, memory_order_relaxed);
    snprintf(line, sizeof line,
             "nvk-cold-guard-diag summary final=%d wsi_done=%llu choice_through_signal=%llu select_a=%llu select_b=%llu invalid=%llu unread=%llu",
             final, (unsigned long long)wsi_seq, (unsigned long long)g.cold_diag_through_signal,
             (unsigned long long)g.cold_diag_a, (unsigned long long)g.cold_diag_b,
             (unsigned long long)g.cold_diag_invalid, (unsigned long long)unread);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    for (unsigned i = COLD_DIAG_OK + 1; i < COLD_DIAG_REASONS; i++) {
        const uint64_t count = atomic_load_explicit(&g.cold_diag_counts[i], memory_order_relaxed);
        if (!count) continue;
        snprintf(line, sizeof line, "nvk-cold-guard-diag count reason=%s count=%llu",
                 cold_diag_names[i], (unsigned long long)count);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    }
}
