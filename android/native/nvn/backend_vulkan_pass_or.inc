/* Diagnostic compute bindings are private to the probe. They do not touch
 * replay's graphics or compute descriptor bookkeeping. */
#include "backend_vulkan_pass_guard_or_spv.h"

enum { PASS_OR_SAMPLES = 24, PASS_OR_QUERY_COUNT = 4 * PASS_OR_SAMPLES };

static void pass_or_close(void) {
    if (g.pass_or_pipe) vkDestroyPipeline(g.d, g.pass_or_pipe, NULL);
    if (g.pass_or_layout) vkDestroyPipelineLayout(g.d, g.pass_or_layout, NULL);
    if (g.pass_or_dsl) vkDestroyDescriptorSetLayout(g.d, g.pass_or_dsl, NULL);
    g.pass_or_pipe = VK_NULL_HANDLE;
    g.pass_or_layout = VK_NULL_HANDLE;
    g.pass_or_dsl = VK_NULL_HANDLE;
}

static int pass_or_resources(void) {
    if (g.pass_or_pipe) return 1;
    if (g.pass_or_resource_failed) return 0;
    VkDescriptorSetLayoutBinding slots[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }
    };
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = slots
    };
    VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(struct nvn_vk_pass_or_domain)};
    VkPipelineLayoutCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g.pass_or_dsl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push
    };
    VkShaderModuleCreateInfo si = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof pass_guard_or_spv, .pCode = pass_guard_or_spv
    };
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(g.d, &di, NULL, &g.pass_or_dsl) == VK_SUCCESS &&
        vkCreatePipelineLayout(g.d, &li, NULL, &g.pass_or_layout) == VK_SUCCESS &&
        vkCreateShaderModule(g.d, &si, NULL, &module) == VK_SUCCESS) {
        VkComputePipelineCreateInfo pi = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = module, .pName = "main"},
            .layout = g.pass_or_layout
        };
        VkResult made = vkCreateComputePipelines(g.d, VK_NULL_HANDLE, 1, &pi, NULL, &g.pass_or_pipe);
        vkDestroyShaderModule(g.d, module, NULL);
        if (made == VK_SUCCESS) return 1;
    }
    pass_or_close();
    g.pass_or_resource_failed = 1;
    nvn_plat_log(NVN_PLAT_LOG_WARN, "pass-or", "probe pipeline creation failed");
    return 0;
}

static int pass_or_domain_ok(uint64_t range, struct nvn_vk_pass_or_domain d) {
    if (!d.count || d.count > 65536 || !d.stride || ((d.stride | d.field0 | d.field1) & 3u) ||
        d.field0 >= d.stride || d.field1 >= d.stride) return 0;
    const uint32_t last_field = d.field0 > d.field1 ? d.field0 : d.field1;
    const uint64_t end = (uint64_t)(d.count - 1) * d.stride + last_field + sizeof(uint32_t);
    return end <= range && end <= UINT32_MAX;
}

static void pass_or_buffer_barrier(nvn_gpu_cmd c, nvn_gpu_buf buf, uint64_t off, uint64_t len,
                                   VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                                   VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    const VkBufferMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = src_access, .dstAccessMask = dst_access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buf->buffer, .offset = off, .size = len
    };
    vkCmdPipelineBarrier(c->cb, src_stage, dst_stage, 0, 0, NULL, 1, &b, 0, NULL);
}

static int pass_or_record(nvn_gpu_cmd c, nvn_gpu_buf src, uint64_t off, uint64_t range,
                          nvn_gpu_buf dst, uint64_t dst_off, struct nvn_vk_pass_or_domain domain,
                          VkQueryPool qp, uint32_t q) {
    const uint64_t result_bytes = sizeof(struct nvn_vk_pass_or_value);
    const uint64_t align = g.dev->properties.limits.minStorageBufferOffsetAlignment;
    if (!c || c->in_pass || !rc_is(c->sticky, NVN_GPU_OK) || !src || !dst || src == dst || !dst->mapped ||
        off > src->bytes || range > src->bytes - off || range > g.caps.max_ssbo_range ||
        dst_off > dst->bytes || result_bytes > dst->bytes - dst_off ||
        (align && (off % align || dst_off % align)) || !pass_or_domain_ok(range, domain)) return 0;
    if (!pass_or_resources()) return 0;
    if (!c->pass_or_pool) {
        const VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * PASS_OR_SAMPLES};
        const VkDescriptorPoolCreateInfo pi = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = PASS_OR_SAMPLES,
            .poolSizeCount = 1, .pPoolSizes = &pool_size
        };
        if (vkCreateDescriptorPool(g.d, &pi, NULL, &c->pass_or_pool) != VK_SUCCESS) return 0;
    }
    const VkDescriptorSetAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = c->pass_or_pool,
        .descriptorSetCount = 1, .pSetLayouts = &g.pass_or_dsl
    };
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(g.d, &ai, &set) != VK_SUCCESS) return 0;

    cmd_ref(c, src, REF_BUF);
    cmd_ref(c, dst, REF_BUF);
    if (!rc_is(c->sticky, NVN_GPU_OK)) return 0;
    const VkDescriptorBufferInfo b[2] = {{src->buffer, off, range}, {dst->buffer, dst_off, result_bytes}};
    VkWriteDescriptorSet writes[2] = {0};
    for (uint32_t i = 0; i < 2; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &b[i];
    }
    vkUpdateDescriptorSets(g.d, 2, writes, 0, NULL);
    // b1 may have been written by either a shader or an ordered transfer.
    // Replay submits both, including init_cb, on the same queue before this read.
    pass_or_buffer_barrier(c, src, off, range, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    domain.stride /= 4;
    domain.field0 /= 4;
    domain.field1 /= 4;
    es_reset(c);   // Private push layout may overlap a guest range.
    if (c->desc_point == 2) c->desc_layout = VK_NULL_HANDLE;
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.pass_or_pipe);
    vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.pass_or_layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(c->cb, g.pass_or_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof domain, &domain);
    if (qp) {
        vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qp, q);
        vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qp, q + 1);
        vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qp, q + 2);
    }
    vkCmdDispatch(c->cb, 1, 1, 1);
    if (qp) vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qp, q + 3);
    // Include subsequent device commands in the execution dependency: the
    // guest's later writes already order its fragment reads, not this extra
    // compute read. The memory dependency publishes only the private result.
    pass_or_buffer_barrier(c, dst, dst_off, result_bytes, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    return 1;
}

int nvn_vk_pass_or_record(nvn_gpu_cmd cmd, nvn_gpu_buf src, uint64_t off, uint64_t range,
                          nvn_gpu_buf dst, uint64_t dst_off, struct nvn_vk_pass_or_domain domain) {
    return pass_or_record(cmd, src, off, range, dst, dst_off, domain, VK_NULL_HANDLE, 0);
}

enum pass_or_reason {
    OR_READY, OR_NO_BOUNDARY, OR_UNPUBLISHED, OR_RANGE, OR_BIND_MISMATCH, OR_FAILED
};
struct pass_or_sample {
    uint32_t seq, q;
    enum pass_or_reason reason;
    const uint8_t *mapped;
};
struct pass_or_cmd {
    struct pass_or_sample samples[PASS_OR_SAMPLES];
    struct pass_or_sample candidate;
    nvn_gpu_buf buf;
    uint64_t off, len;
    uint32_t prefix, n, next_query;
    VkQueryPool qp;
};
static pthread_mutex_t g_pass_or_mu = PTHREAD_MUTEX_INITIALIZER;

static void pass_or_clock_open(void) {
    const struct s2a_vk_device *d = g.dev;
    if (!d->properties.limits.timestampComputeAndGraphics ||
        !isfinite(d->properties.limits.timestampPeriod) || d->properties.limits.timestampPeriod <= 0) return;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d->physical, &count, NULL);
    if (d->queue_family >= count) return;
    VkQueueFamilyProperties *family = calloc(count, sizeof *family);
    if (!family) return;
    vkGetPhysicalDeviceQueueFamilyProperties(d->physical, &count, family);
    if (d->queue_family < count) g.pass_or.ticks = family[d->queue_family].timestampValidBits;
    free(family);
}

int nvn_vk_pass_or_select(nvn_gpu_shader fs, uint32_t vertices, uint32_t instances,
                          uint32_t w, uint32_t h, uint32_t colors, uint32_t slots[3]) {
    if (!g.pass_or.delay || !atomic_load_explicit(&g.pass_or.armed, memory_order_acquire) ||
        g.pass_or.reserved >= PASS_OR_SAMPLES || !fs || fs->capture_digest_prefix != g.guard.sha_prefix ||
        vertices != (uint32_t)g.pass_capture_vertices || instances != 1 || w != (uint32_t)g.pass_capture_w ||
        h != (uint32_t)g.pass_capture_h || colors != (uint32_t)g.pass_capture_colors) return 0;
    if (slots) {
        slots[0] = g.guard.data_slot;
        slots[1] = g.guard.prefix_slot;
        slots[2] = g.guard.prefix_offset;
    }
    return 1;
}

void nvn_vk_pass_or_preflight(nvn_gpu_cmd c, nvn_gpu_buf buf, uint64_t off,
                              uint64_t len, uint32_t prefix, int prefix_known, int natural_boundary) {
    if (!c->pass_or) c->pass_or = calloc(1, sizeof *c->pass_or);
    struct pass_or_cmd *p = c->pass_or;
    if (!p) return;
    p->candidate = (struct pass_or_sample){.reason = OR_NO_BOUNDARY, .q = UINT32_MAX};
    if (!natural_boundary || c->in_pass) return;
    p->candidate.reason = OR_UNPUBLISHED;
    if (!buf || buf->mem_kind != NVN_GPU_MEM_SHARED || !prefix_known) return;
    prefix = (prefix & g.guard.prefix_mask) & ~3u;
    const struct nvn_vk_pass_or_domain d = {g.guard.records, g.guard.stride, g.guard.field + prefix, g.pass_or.field1 + prefix};
    p->candidate.reason = OR_RANGE;
    if (!pass_or_domain_ok(len, d)) return;

    p->candidate.reason = OR_FAILED;
    nvn_gpu_slice out;
    if (!nvn_gpu_ok(nvn_gpu_arena_alloc(c, sizeof(struct nvn_vk_pass_or_value), g.caps.min_ssbo_align, &out, NULL))) return;
    memset(out.cpu, 0, sizeof(struct nvn_vk_pass_or_value));
    if (g.pass_or.ticks && !p->qp && !p->next_query) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = PASS_OR_QUERY_COUNT};
        if (vkCreateQueryPool(g.d, &qi, NULL, &p->qp) == VK_SUCCESS)
            vkCmdResetQueryPool(c->cb, p->qp, 0, PASS_OR_QUERY_COUNT);
    }
    const uint32_t q = p->next_query;
    const VkQueryPool qp = p->qp && q + 4 <= PASS_OR_QUERY_COUNT ? p->qp : VK_NULL_HANDLE;
    if (!pass_or_record(c, buf, off, len, out.buf, out.off, d, qp, q)) return;
    if (qp) { p->next_query += 4; p->candidate.q = q; }
    p->candidate.reason = OR_READY;
    p->candidate.mapped = out.cpu;
    p->buf = buf;
    p->off = off;
    p->len = len;
    p->prefix = prefix;
}

static void pass_or_confirm(nvn_gpu_cmd c, const struct nvn_gpu_pipe_s *pipe, uint32_t vertices) {
    const unsigned st = NVN_GPU_SH_FS;
    const struct nvn_gpu_shader_s *fs = pipe->key.st[st].sh;
    if (!nvn_vk_pass_or_select(pipe->key.st[st].sh, vertices, 1, c->pass_w, c->pass_h,
                                (uint32_t)__builtin_popcount(c->pass_color_mask), NULL)) {
        if (c->pass_or) c->pass_or->candidate.reason = OR_BIND_MISMATCH;
        return;
    }
    if (!c->pass_or) c->pass_or = calloc(1, sizeof *c->pass_or);
    struct pass_or_cmd *p = c->pass_or;
    if (!p) return;
    struct pass_or_sample sample = p->candidate;
    if (!sample.mapped && sample.reason == OR_READY) sample.reason = OR_UNPUBLISHED;
    p->candidate = (struct pass_or_sample){.reason = OR_UNPUBLISHED, .q = UINT32_MAX};
    if (sample.reason == OR_READY) {
        int bound = 0, prefix = 0;
        for (uint32_t i = 0; i < c->nbinds[st]; i++) {
            const nvn_gpu_binding *b = &c->binds[st][i].b;
            if (b->elem || b->ent >= pipe->stage_nent[st]) continue;
            const nvn_gpu_layout_ent *e = &pipe->stage_ents[st][b->ent];
            const uint32_t slot = (uint32_t)e->set << 16 | e->binding;
            if (slot == g.guard.data_slot && e->cls == NVN_GPU_RES_SSBO && !(e->access & NVN_GPU_ACC_W))
                bound = b->u.b.buf == p->buf && b->u.b.off == p->off && b->u.b.len == p->len;
            if (slot == g.guard.prefix_slot && e->cls == NVN_GPU_RES_UBO && e->src_kind == NVN_RB_DRV0) {
                uint32_t word = 0;
                prefix = pass_guard_prefix(c, b, &word) && ((word & g.guard.prefix_mask) & ~3u) == p->prefix;
            }
        }
        if (!sample.mapped || !bound || !prefix) sample.reason = OR_BIND_MISMATCH;
    }
    if (p->n >= PASS_OR_SAMPLES) return;
    if (!g.pass_or.reserved) {
        char line[112];
        snprintf(line, sizeof line, "target sha=%s samples=%u", fs->capture_sha, PASS_OR_SAMPLES);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "pass-or", line);
    }
    sample.seq = ++g.pass_or.reserved;
    p->samples[p->n++] = sample;
}

static void pass_or_finish(nvn_gpu_cmd c, int failed) {
    struct pass_or_cmd *p = c->pass_or;
    if (!p || !p->n) return;
    pthread_mutex_lock(&g_pass_or_mu);
    for (uint32_t i = 0; i < p->n; i++) {
        const struct pass_or_sample *s = &p->samples[i];
        enum pass_or_reason reason = failed ? OR_FAILED : s->reason;
        struct nvn_vk_pass_or_value value = {0};
        if (reason == OR_READY) {
            memcpy(&value, s->mapped, sizeof value);
            if (value.valid != 1) reason = OR_FAILED;
        }
        uint64_t ns = 0, blank_ns = 0;
        int timestamp = 0;
        if (reason == OR_READY && p->qp && s->q != UINT32_MAX) {
            uint64_t q[4][2] = {{0}};
            VkResult r = vkGetQueryPoolResults(g.d, p->qp, s->q, 4, sizeof q, q, sizeof q[0],
                                               VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (r == VK_SUCCESS && q[0][1] && q[1][1] && q[2][1] && q[3][1]) {
                const uint64_t mask = s2a_ts_valid_mask(g.pass_or.ticks);
                const double period = g.dev->properties.limits.timestampPeriod;
                ns = (uint64_t)((double)((q[3][0] - q[2][0]) & mask) * period);
                blank_ns = (uint64_t)((double)((q[1][0] - q[0][0]) & mask) * period);
                timestamp = 1;
            }
        }
        if (reason == OR_READY) {
            g.pass_or.zeros[0] += value.field0 == 0;
            g.pass_or.zeros[1] += value.field1 == 0;
            g.pass_or.nonzeros[0] += value.field0 != 0;
            g.pass_or.nonzeros[1] += value.field1 != 0;
        } else g.pass_or.unknown[reason]++;
        g.pass_or.timestamps += timestamp;
        char line[256];
        snprintf(line, sizeof line, "draw=%u reason=%u nonzero=%u/%u ts_valid=%d dispatch_ns=%llu blank_ns=%llu",
                 s->seq, (unsigned)reason, value.field0 != 0, value.field1 != 0, timestamp,
                 (unsigned long long)ns, (unsigned long long)blank_ns);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "pass-or", line);
        if (++g.pass_or.processed == PASS_OR_SAMPLES) {
            snprintf(line, sizeof line, "summary draws=%u field0_zero=%u field0_nonzero=%u field1_zero=%u field1_nonzero=%u "
                     "unknown_no_boundary=%u unpublished=%u range=%u mismatch=%u failed=%u ts_valid=%u",
                     g.pass_or.processed, g.pass_or.zeros[0], g.pass_or.nonzeros[0], g.pass_or.zeros[1], g.pass_or.nonzeros[1],
                     g.pass_or.unknown[OR_NO_BOUNDARY], g.pass_or.unknown[OR_UNPUBLISHED], g.pass_or.unknown[OR_RANGE],
                     g.pass_or.unknown[OR_BIND_MISMATCH], g.pass_or.unknown[OR_FAILED], g.pass_or.timestamps);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "pass-or", line);
        }
    }
    p->n = 0;
    pthread_mutex_unlock(&g_pass_or_mu);
}

static void pass_or_free(nvn_gpu_cmd c) {
    if (!c->pass_or) return;
    if (c->pass_or->n) pass_or_finish(c, 1);
    if (c->pass_or->qp) vkDestroyQueryPool(g.d, c->pass_or->qp, NULL);
    free(c->pass_or);
}
