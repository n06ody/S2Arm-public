// Off by default. Only an alternate fragment pipeline in a diagnostic window
// sees the extra set. Set 0..2 and the original game descriptor writes are shared.
enum { LOOP_COLS = 16, LOOP_ROWS = 9, LOOP_HEADER_WORDS = 8, LOOP_CELL_WORDS = 10,
       LOOP_WORDS = LOOP_HEADER_WORDS + LOOP_COLS * LOOP_ROWS * LOOP_CELL_WORDS,
       LOOP_SAMPLE_FRAMES = 24, LOOP_MAX_WSI_FRAMES = 4 * LOOP_SAMPLE_FRAMES };

struct pass_loop_sample {
    nvn_gpu_buf output;
    VkDescriptorPool pool;
    uint32_t phase, expected;
    uint64_t offset, range;
    const char *sha; // shader held by this command's pipeline reference
    char request[650];
};

static void pass_loop_release(struct nvn_gpu_cmd_s *c) {
    struct pass_loop_sample *s = c->pass_loop;
    if (!s) return;
    if (s->pool) vkDestroyDescriptorPool(g.d, s->pool, NULL);
    nvn_gpu_buf_release(s->output, NULL, NULL);
    free(s);
}

static int pass_loop_count_valid(uint32_t word, uint32_t record, uint32_t raw,
                                 uint32_t iterations, uint32_t field, uint64_t range) {
    if (word == UINT32_MAX || record == UINT32_MAX)
        return word == UINT32_MAX && record == UINT32_MAX && !raw && !iterations;
    if (record > UINT16_MAX || iterations != (raw > 63 ? 63 : raw)) return 0;
    uint64_t nominal = (uint64_t)record * 576 + field;
    uint64_t effective = (uint64_t)word * sizeof(uint32_t);
    return effective >= nominal && effective - nominal < 64 && word < range / sizeof(uint32_t);
}

static void pass_loop_stop(void) {
    if (g.pass_loop_closed) return;
    g.pass_loop_closed = 1;
    atomic_store_explicit(&g.pass_loop_frame, 0, memory_order_release);
    char line[90];
    snprintf(line, sizeof line, "end frames=%u", g.pass_loop_completed);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "pass-loop", line);
}

static void pass_loop_read(const struct vk_segment *seg, int failed) {
    struct pass_loop_sample *s = seg->cmd ? seg->cmd->pass_loop : NULL;
    if (!s) return;
    char line[600];
    if (failed) {
        snprintf(line, sizeof line, "phase=%u submit=%llu failed=1", s->phase, (unsigned long long)seg->signal);
        nvn_plat_log(NVN_PLAT_LOG_WARN, "pass-loop", line);
        return;
    }
    const uint32_t *words = (const uint32_t *)s->output->mapped;
    uint32_t samples = 0, invalid = 0, overlap = 0, prefixes = 0, dref_pixels = 0, dref_iters = 0;
    uint32_t iters[2] = {0}, maxed[2] = {0}, exceeded[2] = {0}, read[2] = {0};
    uint64_t prefix_mask = 0;
    for (unsigned i = 0; i < LOOP_COLS * LOOP_ROWS; i++) {
        const uint32_t *v = words + LOOP_HEADER_WORDS + i * LOOP_CELL_WORDS;
        if (!v[0]) continue;
        if (v[0] != 1) { overlap++; continue; }
        if (!pass_loop_count_valid(v[8], v[6], v[4], v[1], 252, s->range) ||
            !pass_loop_count_valid(v[9], v[7], v[5], v[2], 508, s->range) || v[3] > v[2]) {
            invalid++;
            continue;
        }
        samples++;
        dref_pixels += v[3] != 0;
        dref_iters += v[3];
        for (unsigned j = 0; j < 2; j++) {
            iters[j] += v[1 + j];
            maxed[j] += v[1 + j] == 63;
            exceeded[j] += v[4 + j] > 63;
            read[j] += v[8 + j] != UINT32_MAX;
            if (v[8 + j] != UINT32_MAX) {
                uint32_t prefix = v[8 + j] * 4 - (v[6 + j] * 576 + (j ? 508 : 252));
                prefix_mask |= UINT64_C(1) << prefix;
            }
        }
    }
    for (uint64_t mask = prefix_mask; mask; mask &= mask - 1) prefixes++;
    snprintf(line, sizeof line,
             "phase=%u submit=%llu sha=%s vp=%u,%u,%u,%u expected=%u samples=%u overlap=%u invalid=%u "
             "b1_offset=%llu b1_range=%llu prefixes=%u prefix_mask=%016llx "
             "read252=%u iter252=%u at63_252=%u gt63_252=%u read508=%u iter508=%u at63_508=%u gt63_508=%u "
             "dref_pixels=%u dref_iters=%u",
             s->phase, (unsigned long long)seg->signal, s->sha, words[0], words[1], words[2], words[3],
             s->expected, samples, overlap, invalid, (unsigned long long)s->offset, (unsigned long long)s->range,
             prefixes, (unsigned long long)prefix_mask, read[0], iters[0], maxed[0], exceeded[0],
             read[1], iters[1], maxed[1], exceeded[1], dref_pixels, dref_iters);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "pass-loop", line);
    if (s->request[0]) {
        char info[740];
        snprintf(info, sizeof info, "request phase=%u submit=%llu %s", s->phase,
                 (unsigned long long)seg->signal, s->request);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "pass-loop", info);
    }
    if (++g.pass_loop_completed == LOOP_SAMPLE_FRAMES) pass_loop_stop();
}

static void pass_loop_frame(const struct vk_segment *s, int failed) {
    if (g.pass_loop_delay <= 0 || !s->wsi_seq) return;
    if (!g.pass_loop_deadline) {
        g.pass_loop_deadline = now_ns() + (uint64_t)g.pass_loop_delay * UINT64_C(1000000000);
        return;
    }
    if (g.pass_loop_closed || failed || now_ns() < g.pass_loop_deadline) return;
    uint32_t epoch = ++g.pass_loop_frames;
    if (epoch > LOOP_MAX_WSI_FRAMES) { pass_loop_stop(); return; }
    atomic_store_explicit(&g.pass_loop_frame, epoch, memory_order_release);
    if (epoch == 1) nvn_plat_log(NVN_PLAT_LOG_INFO, "pass-loop", "begin grid=16x9 phases=24 gputime=0");
}

// Provided by the diagnostic compile provider and the exercising host fixture.
extern const char *nvk_pass_loop_dir(void) __attribute__((weak));

static VkShaderModule pass_loop_module(const struct nvn_gpu_shader_s *fs) {
    const char *dir = nvk_pass_loop_dir ? nvk_pass_loop_dir() : NULL;
    char path[4096];
    int len = dir ? snprintf(path, sizeof path, "%s/pass-loop-%s.spv", dir, fs->capture_sha) : -1;
    if (len < 0 || (size_t)len >= sizeof path) return VK_NULL_HANDLE;
    FILE *f = fopen(path, "rb");
    if (!f) return VK_NULL_HANDLE;
    const size_t cap = 256 * 1024;
    uint32_t *words = malloc(cap);
    size_t size = words ? fread(words, 1, cap, f) : 0;
    VkShaderModule module = VK_NULL_HANDLE;
    if (size >= 20 && size < cap && (size & 3) == 0 && words[0] == 0x07230203 && feof(f) && !ferror(f)) {
        VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                      .codeSize = size, .pCode = words};
        if (vkCreateShaderModule(g.d, &ci, NULL, &module) == VK_SUCCESS) {
            char report[220], hash[65];
            uint8_t digest[32];
            s2a_sha256 ctx;
            s2a_sha256_init(&ctx);
            s2a_sha256_update(&ctx, words, size);
            s2a_sha256_final(&ctx, digest);
            s2a_sha256_hex(digest, hash);
            snprintf(report, sizeof report, "source_sha=%s probe_sha=%s", fs->capture_sha, hash);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "pass-loop", report);
        } else module = VK_NULL_HANDLE;
    }
    fclose(f);
    free(words);
    return module;
}

static void pass_loop_build(struct nvn_gpu_pipe_s *p, const nvn_gpu_pipe_desc *d,
                            const VkGraphicsPipelineCreateInfo *gp, const VkPipelineLayoutCreateInfo *pli,
                            uint32_t ssbo_total, uint32_t ssbo_fs, uint32_t fs_resources, uint32_t fs_images) {
    const struct nvn_gpu_shader_s *fs = d->st[NVN_GPU_SH_FS].sh;
    if (g.pass_loop_delay <= 0 || !fs || fs->capture_key != g.pass_capture_key ||
        gp->pColorBlendState->attachmentCount != (uint32_t)g.pass_capture_colors || d->dfmt || d->sfmt || d->samples > 1)
        return;
    const VkPhysicalDeviceLimits *lim = &g.dev->properties.limits;
    if (!g.dev->core_features.fragmentStoresAndAtomics || p->nset != 3 || lim->maxBoundDescriptorSets < 4 ||
        ssbo_total >= lim->maxDescriptorSetStorageBuffers || ssbo_fs >= lim->maxPerStageDescriptorStorageBuffers ||
        fs_resources >= lim->maxPerStageResources ||
        ssbo_fs + fs_images + gp->pColorBlendState->attachmentCount >= lim->maxFragmentCombinedOutputResources ||
        fs->capture_flags & (PASS_FS_KILL | PASS_FS_DEMOTE | PASS_FS_DEPTH_OR_MASK | PASS_FS_STORAGE)) {
        nvn_plat_log(NVN_PLAT_LOG_WARN, "pass-loop", "alternate pipeline rejected by layout or feature limits");
        return;
    }
    VkShaderModule module = pass_loop_module(fs);
    if (!module) {
        nvn_plat_log(NVN_PLAT_LOG_WARN, "pass-loop", "exact source diagnostic module missing or invalid");
        return;
    }
    VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    if (vkCreateDescriptorSetLayout(g.d, &dl, NULL, &p->loop_dsl) == VK_SUCCESS) {
        VkDescriptorSetLayout layouts[4] = {p->dsl[0], p->dsl[1], p->dsl[2], p->loop_dsl};
        VkPipelineLayoutCreateInfo li = *pli;
        li.setLayoutCount = 4;
        li.pSetLayouts = layouts;
        if (vkCreatePipelineLayout(g.d, &li, NULL, &p->loop_layout) == VK_SUCCESS) {
            VkPipelineShaderStageCreateInfo stages[5];
            memcpy(stages, gp->pStages, gp->stageCount * sizeof stages[0]);
            for (uint32_t i = 0; i < gp->stageCount; i++)
                if (stages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) stages[i].module = module;
            VkGraphicsPipelineCreateInfo alt = *gp;
            alt.pStages = stages;
            alt.layout = p->loop_layout;
            if (vkCreateGraphicsPipelines(g.d, VK_NULL_HANDLE, 1, &alt, NULL, &p->loop_pipe) != VK_SUCCESS)
                p->loop_pipe = VK_NULL_HANDLE;
        } else p->loop_layout = VK_NULL_HANDLE;
    } else p->loop_dsl = VK_NULL_HANDLE;
    vkDestroyShaderModule(g.d, module, NULL);
    if (!p->loop_pipe) nvn_plat_log(NVN_PLAT_LOG_WARN, "pass-loop", "alternate pipeline creation failed");
}

static uint32_t pass_loop_expected(const uint32_t header[LOOP_HEADER_WORDS], const VkRect2D *sc) {
    uint32_t horizontal = 0, vertical = 0;
    for (uint32_t i = 0; i < LOOP_COLS; i++) {
        int64_t x = header[0] + (i * 8 + header[4]) * header[2] / (LOOP_COLS * 8);
        horizontal += x >= sc->offset.x && x < (int64_t)sc->offset.x + sc->extent.width;
    }
    for (uint32_t i = 0; i < LOOP_ROWS; i++) {
        int64_t y = header[1] + (i * 8 + header[5]) * header[3] / (LOOP_ROWS * 8);
        vertical += y >= sc->offset.y && y < (int64_t)sc->offset.y + sc->extent.height;
    }
    return horizontal * vertical;
}

static VkPipeline pass_loop_draw(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p,
                                 const nvn_gpu_draw_args *a, const VkViewport *vp, const VkRect2D *sc) {
    if (!p->loop_pipe || c->pass_loop || c->pass_loop_drawn || c->pass_occl || c->feedback_read_mask ||
        c->pass_layers != 1 || c->pass_samples > 1 || a->ind || a->ibuf || a->instances > 1 ||
        a->count != (uint32_t)g.pass_capture_vertices || c->pass_w != (uint32_t)g.pass_capture_w ||
        c->pass_h != (uint32_t)g.pass_capture_h || c->pass_ncolor != (uint32_t)g.pass_capture_colors)
        return VK_NULL_HANDLE;
    uint32_t epoch = atomic_load_explicit(&g.pass_loop_frame, memory_order_acquire);
    if (!epoch || epoch <= g.pass_loop_claimed || g.pass_loop_selected == LOOP_SAMPLE_FRAMES) return VK_NULL_HANDLE;
    g.pass_loop_claimed = epoch;
    uint32_t phase = g.pass_loop_selected + 1;
    const nvn_gpu_binding *b1 = NULL;
    for (uint32_t i = 0; i < c->nbinds[NVN_GPU_SH_FS]; i++) {
        const nvn_gpu_binding *b = &c->binds[NVN_GPU_SH_FS][i].b;
        if (b->ent >= p->stage_nent[NVN_GPU_SH_FS]) continue;
        const nvn_gpu_layout_ent *e = &p->stage_ents[NVN_GPU_SH_FS][b->ent];
        if (e->cls == NVN_GPU_RES_SSBO && e->set == 1 && e->binding == 1 && !b->elem && e->count <= 1) {
            if (b1) goto rejected;
            b1 = b;
        }
    }
    float top = vp->height < 0 ? vp->y + vp->height : vp->y;
    float h = vp->height < 0 ? -vp->height : vp->height;
    const float geometry[4] = {vp->x, top, vp->width, h};
    uint32_t header[LOOP_HEADER_WORDS] = {0};
    for (unsigned i = 0; i < 4; i++) {
        if (!isfinite(geometry[i]) || geometry[i] < 0 || geometry[i] > 16384) goto rejected;
        header[i] = (uint32_t)geometry[i];
        if ((float)header[i] != geometry[i]) goto rejected;
    }
    if (!b1 || !b1->u.b.buf || !buf_range(b1->u.b.buf, b1->u.b.off, b1->u.b.len) ||
        header[2] < LOOP_COLS * 4 || header[3] < LOOP_ROWS * 4 ||
        header[0] + header[2] > c->pass_w || header[1] + header[3] > c->pass_h) goto rejected;
    header[4] = 1 + 2 * ((phase - 1) % 4);
    header[5] = 1 + 2 * (((phase - 1) / 4) % 4);
    struct pass_loop_sample *s = calloc(1, sizeof *s);
    if (!s) goto rejected;
    c->pass_loop = s;
    s->sha = p->key.st[NVN_GPU_SH_FS].sh->capture_sha;
    s->phase = phase;
    s->expected = pass_loop_expected(header, sc);
    s->offset = b1->u.b.off;
    s->range = b1->u.b.len;
    const size_t bytes = LOOP_WORDS * sizeof(uint32_t);
    if (!rc_is(nvn_gpu_buf_create(bytes, NVN_GPU_MEM_SHARED, NULL, &s->output, NULL), NVN_GPU_OK)) goto cleanup;
    memset(s->output->mapped, 0, bytes);
    memcpy(s->output->mapped, header, sizeof header);
    VkDescriptorPoolSize ds = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ds};
    if (vkCreateDescriptorPool(g.d, &dp, NULL, &s->pool) != VK_SUCCESS) goto cleanup;
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = s->pool, .descriptorSetCount = 1, .pSetLayouts = &p->loop_dsl};
    if (vkAllocateDescriptorSets(g.d, &ai, &set) != VK_SUCCESS) goto cleanup;
    VkDescriptorBufferInfo bi = {s->output->buffer, 0, bytes};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
        .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &bi};
    vkUpdateDescriptorSets(g.d, 1, &write, 0, NULL);
    vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->loop_layout, 3, 1, &set, 0, NULL);
    g.pass_loop_selected = phase;
    return p->loop_pipe;
cleanup:
    pass_loop_release(c);
    c->pass_loop = NULL;
rejected:
    nvn_plat_log(NVN_PLAT_LOG_WARN, "pass-loop", "target draw missing unique set1/b1, supported geometry or readback resources");
    return VK_NULL_HANDLE;
}

// Replay supplies the true guest-facing request for the draw just encoded.
// The optional callback comes from the Android compile provider.
struct rp_shader_req;
int nvk_pass_loop_facts(const struct rp_shader_req *, char *, size_t) __attribute__((weak));
int nvk_pass_loop_context(nvn_gpu_cmd c, const struct rp_shader_req *q, const uint8_t program[32]);
int nvk_pass_loop_context(nvn_gpu_cmd c, const struct rp_shader_req *q, const uint8_t program[32]) {
    if (!c || !c->pass_loop || c->pass_loop->request[0]) return 0;
    if (!q) return 1;
    struct pass_loop_sample *s = c->pass_loop;
    char identity[65];
    s2a_sha256_hex(program, identity);
    int n = snprintf(s->request, sizeof s->request, "selected_identity=%s ", identity);
    if (n < 0 || (size_t)n >= sizeof s->request) return 0;
    if (!nvk_pass_loop_facts || !nvk_pass_loop_facts(q, s->request + n, sizeof s->request - (size_t)n))
        snprintf(s->request + n, sizeof s->request - (size_t)n, "facts=unavailable");
    return 1;
}
