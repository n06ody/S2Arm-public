// Only attached to recorded direct draws. No GPU commands, pipeline variants
// or GPU timestamps. Accumulators below are owned by the replay submit thread.
enum { PASS_GUARD_DRAWS = 24 };

static int pass_guard_prefix(const struct nvn_gpu_cmd_s *c, const nvn_gpu_binding *b, uint32_t *word) {
    const struct nvn_gpu_buf_s *buf = b->u.b.buf;
    const uint64_t end = (uint64_t)g.guard.prefix_offset + sizeof *word;
    if (!buf || !buf->mapped || end > b->u.b.len || b->u.b.off > buf->bytes || end > buf->bytes - b->u.b.off)
        return 0;
    // A direct draw's NVN_RB_DRV0 is assembled by the CPU in append-only
    // command arena storage. A guest UBO, or a GPU-generated indirect DRV0,
    // does not provide this proof.
    for (uint32_t i = 0; i < c->nstaging; i++) {
        if (c->staging[i].buf != buf) continue;
        memcpy(word, buf->mapped + b->u.b.off + g.guard.prefix_offset, sizeof *word);
        return 1;
    }
    return 0;
}

static void pass_guard_draw(const struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p, uint32_t vertices) {
    if (!atomic_load_explicit(&g.guard.armed, memory_order_acquire) || g.guard.draws >= PASS_GUARD_DRAWS) return;
    const unsigned st = NVN_GPU_SH_FS;
    const struct nvn_gpu_shader_s *fs = p->key.st[st].sh;
    if (!fs || fs->capture_digest_prefix != g.guard.sha_prefix ||
        vertices != (uint32_t)g.pass_capture_vertices || c->pass_w != (uint32_t)g.pass_capture_w ||
        c->pass_h != (uint32_t)g.pass_capture_h ||
        (unsigned)__builtin_popcount(c->pass_color_mask) != (unsigned)g.pass_capture_colors) return;

    const nvn_gpu_binding *data = NULL;
    uint32_t word = g.guard.prefix_mask;
    int prefix_known = 0, writable = 0;
    for (uint32_t i = 0; i < c->nbinds[st]; i++) {
        const nvn_gpu_binding *b = &c->binds[st][i].b;
        if (b->elem || b->ent >= p->stage_nent[st]) continue;
        const nvn_gpu_layout_ent *e = &p->stage_ents[st][b->ent];
        const uint32_t slot = (uint32_t)e->set << 16 | e->binding;
        if (slot == g.guard.data_slot && e->cls == NVN_GPU_RES_SSBO) {
            data = b;
            writable = (e->access & NVN_GPU_ACC_W) != 0;
        }
        if (slot == g.guard.prefix_slot && e->cls == NVN_GPU_RES_UBO && e->src_kind == NVN_RB_DRV0)
            prefix_known = pass_guard_prefix(c, b, &word);
    }
    // A missing prefix still allows a range-only check at its largest possible
    // value. Guard word accesses round the byte address down to a word.
    const uint32_t field = g.guard.field + ((word & g.guard.prefix_mask) & ~3u);
    struct rp_guard_observation obs = {.prefix = prefix_known};
    if (data) rp_buf_guard_probe(data->u.b.buf, data->u.b.off, data->u.b.len, field,
                                 g.guard.stride, g.guard.records, prefix_known && !writable, &obs);
    obs.prefix = prefix_known;
    if (writable) { obs.alias = 1; obs.cpu_writer = 0; }
    g.guard.ranges += obs.range;
    g.guard.prefixes += obs.prefix;
    g.guard.versions[obs.version]++;
    g.guard.uploads[obs.upload]++;
    g.guard.gpu_writer += obs.gpu_writer;
    g.guard.cpu_writer += obs.cpu_writer;
    g.guard.writer_unknown += !obs.gpu_writer && !obs.cpu_writer;
    g.guard.pending += obs.pending;
    g.guard.landing += obs.landing;
    g.guard.alias += obs.alias;
    g.guard.zero[obs.zero]++;
    if (++g.guard.draws != PASS_GUARD_DRAWS) return;
    char line[640];
    snprintf(line, sizeof line,
             "summary sha=%s draws=%u range_ok=%u prefix_drv0=%u version_single=%u version_multiple=%u version_unknown=%u "
             "upload_direct=%u upload_ordered=%u upload_mixed=%u upload_unknown=%u "
             "writer_cpu=%u writer_gpu_seen=%u writer_unknown=%u pending=%u landing=%u alias=%u "
             "all_zero=%u nonzero=%u zero_unknown=%u",
             fs->capture_sha, g.guard.draws, g.guard.ranges, g.guard.prefixes,
             g.guard.versions[RP_GUARD_VERSION_SINGLE], g.guard.versions[RP_GUARD_VERSION_MULTIPLE],
             g.guard.versions[RP_GUARD_VERSION_UNKNOWN], g.guard.uploads[RP_GUARD_UPLOAD_DIRECT],
             g.guard.uploads[RP_GUARD_UPLOAD_ORDERED], g.guard.uploads[RP_GUARD_UPLOAD_MIXED],
             g.guard.uploads[RP_GUARD_UPLOAD_UNKNOWN], g.guard.cpu_writer, g.guard.gpu_writer, g.guard.writer_unknown,
             g.guard.pending, g.guard.landing, g.guard.alias, g.guard.zero[RP_GUARD_ALL_ZERO],
             g.guard.zero[RP_GUARD_NONZERO], g.guard.zero[RP_GUARD_ZERO_UNKNOWN]);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "pass-guard", line);
}
