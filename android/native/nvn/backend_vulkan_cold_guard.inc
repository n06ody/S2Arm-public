// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "backend_vulkan_cold_guard_spv.h"

enum { COLD_WORDS_MAX = 65536 };
struct cold_push {
    uint32_t first_word, stride_words, count, vertex_count, first_vertex;
};

static void cold_guard_close(void) {
    if (g.cold_pipe) vkDestroyPipeline(g.d, g.cold_pipe, NULL);
    if (g.cold_layout) vkDestroyPipelineLayout(g.d, g.cold_layout, NULL);
    if (g.cold_dsl) vkDestroyDescriptorSetLayout(g.d, g.cold_dsl, NULL);
    g.cold_pipe = VK_NULL_HANDLE;
    g.cold_layout = VK_NULL_HANDLE;
    g.cold_dsl = VK_NULL_HANDLE;
}

static int cold_guard_open(void) {
    if (g.cold_pipe) return 1;
    if (g.cold_failed) return 0;
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    };
    const VkDescriptorSetLayoutCreateInfo descriptors = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings,
    };
    const VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(struct cold_push)};
    const VkPipelineLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &g.cold_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
    };
    const VkShaderModuleCreateInfo shader = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof cold_guard_spv, .pCode = cold_guard_spv,
    };
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult made = VK_ERROR_INITIALIZATION_FAILED;
    if (vkCreateDescriptorSetLayout(g.d, &descriptors, NULL, &g.cold_dsl) == VK_SUCCESS &&
        vkCreatePipelineLayout(g.d, &layout, NULL, &g.cold_layout) == VK_SUCCESS &&
        vkCreateShaderModule(g.d, &shader, NULL, &module) == VK_SUCCESS) {
        const VkComputePipelineCreateInfo pipeline = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
            .layout = g.cold_layout,
        };
        made = vkCreateComputePipelines(g.d, VK_NULL_HANDLE, 1, &pipeline, NULL, &g.cold_pipe);
        vkDestroyShaderModule(g.d, module, NULL);
    }
    if (made == VK_SUCCESS) return 1;
    cold_guard_close();
    g.cold_failed = 1;
    nvn_plat_log(NVN_PLAT_LOG_WARN, "gpu", "nvk-cold-guard selector creation failed");
    return 0;
}

static int cold_guard_direct(const nvn_gpu_draw_args *a) {
    return a && a->count && a->instances == 1 && !a->base_instance &&
           !a->ibuf && !a->ind && !a->cnt && !a->max_draws;
}

static int cold_guard_pipes(const struct nvn_gpu_pipe_s *a, const struct nvn_gpu_pipe_s *b,
                            uint32_t ent) {
    const nvn_gpu_stage fs = NVN_GPU_SH_FS;
    if (!a || !b || a == b || !a->pass || !b->pass || a->actual_samples != b->actual_samples ||
        !a->key.st[NVN_GPU_SH_VS].sh || !a->key.st[fs].sh || !b->key.st[fs].sh ||
        a->key.st[NVN_GPU_SH_GS].sh || a->key.st[NVN_GPU_SH_TCS].sh || a->key.st[NVN_GPU_SH_TES].sh ||
        ent >= a->stage_nent[fs] || a->stage_nent[fs] != b->stage_nent[fs] ||
        a->stage_push_off[fs] != b->stage_push_off[fs] || a->stage_push_bytes[fs] != b->stage_push_bytes[fs] ||
        a->nset != b->nset || memcmp(a->dsl, b->dsl, sizeof a->dsl) ||
        memcmp(a->stage_ents[fs], b->stage_ents[fs], a->stage_nent[fs] * sizeof *a->stage_ents[fs])) return 0;
    // The first version admits only read-only resource layouts. In
    // particular the vertex stage may not write over the scanned storage.
    for (int stage = NVN_GPU_SH_VS; stage <= NVN_GPU_SH_FS; stage++)
        for (uint32_t i = 0; i < a->stage_nent[stage]; i++)
            if (a->stage_ents[stage][i].access & NVN_GPU_ACC_W) return 0;
    const nvn_gpu_layout_ent *slot = &a->stage_ents[fs][ent];
    if (slot->cls != NVN_GPU_RES_SSBO || slot->count != 1 || slot->access != NVN_GPU_ACC_R) return 0;
    nvn_gpu_pipe_desc key = b->key;
    key.st[fs].sh = a->key.st[fs].sh;
    return same_pipe_desc(&a->key, &key);
}

static int cold_guard_prefix(const struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p,
                             const struct nvn_gpu_cold_prefix *prefix, uint32_t *value) {
    *value = 0;
    if (!prefix->buf) return 1;
    if (prefix->fragment_ent >= p->stage_nent[NVN_GPU_SH_FS] || !prefix->mask || prefix->mask > 255 ||
        (prefix->mask & (prefix->mask + 1)) || (prefix->byte & 3) || prefix->binding.aspects ||
        (uint64_t)prefix->byte + sizeof *value > prefix->binding.bytes ||
        prefix->binding.bytes > g.caps.max_ubo_range ||
        prefix->binding.off % g.dev->properties.limits.minUniformBufferOffsetAlignment ||
        !prefix->buf->mapped) return 0;
    const nvn_gpu_layout_ent *slot = &p->stage_ents[NVN_GPU_SH_FS][prefix->fragment_ent];
    if (slot->cls != NVN_GPU_RES_UBO || slot->count != 1 || slot->src_kind != NVN_RB_DRV0 || slot->access != NVN_GPU_ACC_R) return 0;
    nvn_gpu_access_span actual;
    if (!nvn_gpu_buf_access_span(prefix->buf, prefix->binding.off, prefix->binding.bytes, &actual) ||
        actual.storage != prefix->binding.storage) return 0;
    for (uint32_t i = 0; i < c->nstaging; i++) {
        if (c->staging[i].buf != prefix->buf) continue;
        memcpy(value, prefix->buf->mapped + actual.off + prefix->byte, sizeof *value);
        *value &= prefix->mask;
        return 1;
    }
    return 0;
}

static enum cold_diag_reason cold_guard_coverage(const struct nvn_gpu_cold_domain *d, uint64_t first_word,
                                                const struct nvn_vk_cold_publication *pub, uint32_t n,
                                                VkPipelineStageFlags *stages, VkAccessFlags *access,
                                                VkDeviceSize *first, VkDeviceSize *bytes) {
    nvn_gpu_access_span bound;
    if (!d || !d->buf || !d->buf->memory || !d->count || d->count > COLD_WORDS_MAX ||
        !d->stride_words || d->binding.aspects ||
        !nvn_gpu_buf_access_span(d->buf, d->binding.off, d->binding.bytes, &bound) ||
        bound.storage != d->binding.storage || bound.bytes > g.caps.max_ssbo_range ||
        bound.off % g.dev->properties.limits.minStorageBufferOffsetAlignment) return COLD_DIAG_SSBO_BOUNDS;
    const uint64_t last = first_word + (uint64_t)(d->count - 1) * d->stride_words;
    if (last > UINT32_MAX || (last + 1) * sizeof(uint32_t) > bound.bytes) return COLD_DIAG_SSBO_BOUNDS;
    if (!n) return COLD_DIAG_PUBLICATION_GAP;
    if (!pub) return COLD_DIAG_PUBLICATION_INVALID;
    *first = bound.off + first_word * sizeof(uint32_t);
    const uint64_t end = bound.off + (last + 1) * sizeof(uint32_t);
    *bytes = end - *first;
    uint64_t cursor = *first;
    for (uint32_t i = 0; i < n && cursor < end; i++) {
        const nvn_gpu_access_span *s = &pub[i].span;
        if (s->storage != bound.storage || s->aspects || !s->bytes || !buf_range(d->buf, s->off, s->bytes))
            return COLD_DIAG_PUBLICATION_INVALID;
        if (s->off + s->bytes <= cursor) continue;
        if (s->off > cursor) return COLD_DIAG_PUBLICATION_GAP;
        const unsigned writer = pub[i].writers;
        if (!writer || (writer & ~(COLD_READ_HOST | COLD_READ_TRANSFER | COLD_READ_COMPUTE | COLD_READ_GRAPHICS)))
            return COLD_DIAG_PUBLICATION_INVALID;
        if (writer & COLD_READ_HOST) {
            *stages |= VK_PIPELINE_STAGE_HOST_BIT;
            *access |= VK_ACCESS_HOST_WRITE_BIT;
        }
        if (writer & COLD_READ_TRANSFER) {
            *stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
            *access |= VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        if (writer & COLD_READ_COMPUTE) {
            *stages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            *access |= VK_ACCESS_SHADER_WRITE_BIT;
        }
        if (writer & COLD_READ_GRAPHICS) {
            *stages |= VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
            *access |= VK_ACCESS_SHADER_WRITE_BIT;
        }
        cursor = s->off + s->bytes;
    }
    return cursor >= end ? COLD_DIAG_OK : COLD_DIAG_PUBLICATION_GAP;
}

static VkDescriptorSet cold_guard_descriptors(struct nvn_gpu_cmd_s *c) {
    if (c->cold_sets == COLD_SETS_PER_SEGMENT) return VK_NULL_HANDLE;
    if (!c->cold_pool) {
        const VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * COLD_SETS_PER_SEGMENT};
        const VkDescriptorPoolCreateInfo pool = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = COLD_SETS_PER_SEGMENT, .poolSizeCount = 1, .pPoolSizes = &size,
        };
        if (vkCreateDescriptorPool(g.d, &pool, NULL, &c->cold_pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    const VkDescriptorSetAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = c->cold_pool, .descriptorSetCount = 1, .pSetLayouts = &g.cold_dsl,
    };
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(g.d, &alloc, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    c->cold_sets++;
    return set;
}

static int cold_guard_scan(nvn_gpu_cmd c, nvn_gpu_pipe a, nvn_gpu_pipe b, const nvn_gpu_draw_args *draw,
                          const struct nvn_gpu_cold_domain *d, const struct nvn_vk_cold_publication *pub,
                          uint32_t n) {
    if (!c || !g.cold_enabled) return 0;
    if (!rc_is(c->sticky, NVN_GPU_OK)) return cold_diag_reject(c, COLD_DIAG_CMD_ERROR);
    c->cold.original = NULL;
    if (!d) return cold_diag_reject(c, COLD_DIAG_DOMAIN);
    if (c->in_pass) return cold_diag_reject(c, COLD_DIAG_PASS_STATE);
    if (c->qpool) return cold_diag_reject(c, COLD_DIAG_QUERY);
    if (diag_draw_mode()) return cold_diag_reject(c, COLD_DIAG_DRAW_DIAG);
    if (!cold_guard_direct(draw)) return cold_diag_reject(c, COLD_DIAG_DRAW_SHAPE);
    if (!cold_guard_pipes(a, b, d->fragment_ent) || draw->prim != a->key.prim)
        return cold_diag_reject(c, COLD_DIAG_PIPELINES);
    uint32_t prefix_value;
    if (!cold_guard_prefix(c, a, &d->prefix, &prefix_value) || d->buf == d->prefix.buf)
        return cold_diag_reject(c, COLD_DIAG_PREFIX);
    const uint64_t first_word = d->first_word + (uint64_t)prefix_value / 4;
    VkPipelineStageFlags producer_stages = 0;
    VkAccessFlags producer_access = 0;
    VkDeviceSize first, bytes;
    const enum cold_diag_reason coverage =
        cold_guard_coverage(d, first_word, pub, n, &producer_stages, &producer_access, &first, &bytes);
    if (coverage != COLD_DIAG_OK) return cold_diag_reject(c, coverage);
    if (!cold_guard_open()) return cold_diag_reject(c, COLD_DIAG_SELECTOR);
    if (g.dev->properties.limits.minStorageBufferOffsetAlignment > UINT32_MAX)
        return cold_diag_reject(c, COLD_DIAG_ALIGNMENT);
    const uint32_t align = (uint32_t)g.dev->properties.limits.minStorageBufferOffsetAlignment;
    // Rebase the private descriptor to the smallest aligned interval that
    // contains every scanned word. Publish that same interval, including any
    // alignment padding Vulkan's synchronization validator attributes to it.
    const VkDeviceSize read_off = first - first % align;
    const VkDeviceSize read_bytes = first + bytes - read_off;
    const VkDeviceSize result_bytes = 2 * sizeof(VkDrawIndirectCommand);
    nvn_gpu_slice result;
    nvn_gpu_err err;
    if (!nvn_gpu_ok(nvn_gpu_arena_alloc(c, result_bytes, align > 4 ? align : 4, &result, &err)))
        return cold_diag_reject(c, COLD_DIAG_ARENA);
    VkDescriptorSet set = cold_guard_descriptors(c);
    if (!set) return cold_diag_reject(c, COLD_DIAG_DESCRIPTORS);
    cmd_ref(c, d->buf, REF_BUF);
    if (!rc_is(c->sticky, NVN_GPU_OK)) return cold_diag_reject(c, COLD_DIAG_CMD_ERROR);

    const VkDescriptorBufferInfo buffers[] = {
        {d->buf->buffer, read_off, read_bytes},
        {result.buf->buffer, result.off, result_bytes},
    };
    VkWriteDescriptorSet writes[2] = {0};
    for (uint32_t i = 0; i < 2; i++) {
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
            .dstBinding = i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &buffers[i]};
    }
    vkUpdateDescriptorSets(g.d, 2, writes, 0, NULL);
    const VkBufferMemoryBarrier read = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = producer_access,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = d->buf->buffer, .offset = read_off, .size = read_bytes,
    };
    vkCmdPipelineBarrier(c->cb, producer_stages, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1, &read, 0, NULL);
    const struct cold_push push = {(uint32_t)((first - read_off) / 4), d->stride_words, d->count, draw->count, draw->first};
    es_reset(c);
    if (c->desc_point == 2) c->desc_layout = VK_NULL_HANDLE;
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.cold_pipe);
    vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.cold_layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(c->cb, g.cold_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(c->cb, 1, 1, 1);
    const VkBufferMemoryBarrier indirect = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = result.buf->buffer, .offset = result.off, .size = result_bytes,
    };
    // The execution dependency also orders any later writes to the scanned
    // source after the compute read, including when the result is abandoned.
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 1, &indirect, 0, NULL);
    c->cold.original = a;
    c->cold.variant = b;
    c->cold.domain = *d;
    c->cold.draw = *draw;
    c->cold.result = result;
    c->cold.prefix_value = prefix_value;
    c->cold.source_generation = __atomic_load_n(&d->buf->cold_generation, __ATOMIC_RELAXED);
    return 1;
}

int nvn_vk_cold_guard_auto(nvn_gpu_cmd c, nvn_gpu_cmd pre, nvn_gpu_pipe a, nvn_gpu_pipe b,
                          const nvn_gpu_draw_args *draw, const struct nvn_gpu_cold_domain *d) {
    if (!g.cold_enabled || !c) return 0;
    atomic_fetch_add_explicit(&g.cold_attempts, 1, memory_order_relaxed);
    c->cold.original = NULL;
    if (!d || !d->buf) return cold_diag_reject(c, COLD_DIAG_DOMAIN);
    if (!d->buf->cold_ledger) return cold_diag_reject(c, COLD_DIAG_UNTRACKED);
    if (c == pre) return cold_diag_reject(c, COLD_DIAG_PRE_IS_MAIN);
    if (c->cold_events_lost) return cold_diag_reject(c, COLD_DIAG_MAIN_EVENTS_LOST);
    if (pre && pre->cold_events_lost) return cold_diag_reject(c, COLD_DIAG_PRE_EVENTS_LOST);
    if (pre && !rc_is(pre->sticky, NVN_GPU_OK)) return cold_diag_reject(c, COLD_DIAG_PRE_ERROR);
    struct cold_ledger snapshot;
    pthread_mutex_lock(&g_submit_mu);
    if (g.cold_writes_lost) {
        pthread_mutex_unlock(&g_submit_mu);
        return cold_diag_reject(c, COLD_DIAG_SESSION_WRITES_LOST);
    }
    snapshot = *d->buf->cold_ledger;
    const uint64_t epoch = g.cold_epoch;
    pthread_mutex_unlock(&g_submit_mu);
    const uint64_t main_epoch = cold_replay_events(pre, epoch, d->buf, &snapshot);
    cold_replay_events(c, main_epoch, d->buf, &snapshot);
    struct nvn_vk_cold_publication writes[COLD_LEDGER_SPANS];
    for (unsigned i = 0; i < snapshot.n; i++) {
        const struct cold_interval *s = &snapshot.span[i];
        writes[i] = (struct nvn_vk_cold_publication){
            .span = {.storage = d->binding.storage, .off = s->lo, .bytes = s->hi - s->lo},
            .writers = s->writers,
        };
    }
    const int armed = cold_guard_scan(c, a, b, draw, d, writes, snapshot.n);
    if (armed) atomic_fetch_add_explicit(&g.cold_armed, 1, memory_order_relaxed);
    return armed;
}

static int cold_guard_has_binding(const struct nvn_gpu_cmd_s *c, uint32_t ent, uint8_t cls,
                                  nvn_gpu_buf buf, const nvn_gpu_access_span *span) {
    const nvn_gpu_binding *bound = NULL;
    for (uint32_t i = 0; i < c->nbinds[NVN_GPU_SH_FS]; i++) {
        const nvn_gpu_binding *b = &c->binds[NVN_GPU_SH_FS][i].b;
        if (b->ent != ent || b->elem) continue;
        if (bound) return 0;
        bound = b;
    }
    return bound && bound->cls == cls && bound->u.b.buf == buf &&
           bound->u.b.off == span->off && bound->u.b.len == span->bytes;
}

int nvn_vk_cold_guard_draw(nvn_gpu_cmd c, const nvn_gpu_draw_args *draw) {
    if (!c || !c->cold.original) return 0;
    nvn_gpu_pipe a = c->cold.original;
    c->cold.original = NULL;
    if (!rc_is(c->sticky, NVN_GPU_OK) || !cold_guard_direct(draw) || !c->in_pass ||
        c->q_active >= 0 || c->pass_occl || c->pipe != a || draw->prim != c->cold.draw.prim ||
        draw->first != c->cold.draw.first || draw->count != c->cold.draw.count)
        return cold_diag_reject(c, COLD_DIAG_DRAW_STATE);
    const struct nvn_gpu_cold_domain *d = &c->cold.domain;
    if (c->cold.source_generation != __atomic_load_n(&d->buf->cold_generation, __ATOMIC_RELAXED))
        return cold_diag_reject(c, COLD_DIAG_SOURCE_CHANGED);
    if (!cold_guard_has_binding(c, d->fragment_ent, NVN_GPU_RES_SSBO, d->buf, &d->binding))
        return cold_diag_reject(c, COLD_DIAG_SSBO_CHANGED);
    if (d->prefix.buf) {
        uint32_t prefix_value;
        if (!cold_guard_has_binding(c, d->prefix.fragment_ent, NVN_GPU_RES_UBO, d->prefix.buf, &d->prefix.binding) ||
            !cold_guard_prefix(c, a, &d->prefix, &prefix_value) || prefix_value != c->cold.prefix_value)
            return cold_diag_reject(c, COLD_DIAG_PREFIX_CHANGED);
    }
    nvn_gpu_draw_args act = *draw;
    act.ind = c->cold.result.buf;
    act.ind_off = c->cold.result.off;
    act.stride = sizeof(VkDrawIndirectCommand);
    nvn_gpu_draw(c, &act);
    if (!rc_is(c->sticky, NVN_GPU_OK)) return 1;
    // The layouts are compatible. Reuse the encoded descriptors, push data
    // and dynamic state. A second prepare pass could reopen a feedback pass.
    nvn_gpu_pipe b = c->cold.variant;
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, b->pipe);
    c->es.gpipe = b->pipe;
    c->es_count[ES_PIPE]++;
    vkCmdDrawIndirect(c->cb, act.ind->buffer, act.ind_off + act.stride, 1, act.stride);
    loss_note_program(c, b);
    atomic_fetch_add_explicit(&g.cold_pairs, 1, memory_order_relaxed);
    if (g.cold_diag_enabled) cold_diag_enqueue_choice(c, draw);
    return 1;
}

static void cold_guard_presented(void) {
    if (!g.cold_enabled) return;
    const uint32_t present = atomic_fetch_add_explicit(&g.cold_present_count, 1, memory_order_relaxed) + 1;
    if (present % 120) return;
    const uint64_t attempt = atomic_load_explicit(&g.cold_attempts, memory_order_relaxed);
    const uint64_t scan = atomic_load_explicit(&g.cold_armed, memory_order_relaxed);
    const uint64_t pair = atomic_load_explicit(&g.cold_pairs, memory_order_relaxed);
    char line[160];
    snprintf(line, sizeof line, "nvk-cold-guard present=%u attempts=%llu scans=%llu pairs=%llu",
             present, (unsigned long long)attempt, (unsigned long long)scan, (unsigned long long)pair);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
}

#ifdef S2A_NVK_COLD_GUARD_HOST_TEST
int nvn_vk_cold_guard_scan(nvn_gpu_cmd c, nvn_gpu_pipe a, nvn_gpu_pipe b, const nvn_gpu_draw_args *draw,
                          const struct nvn_gpu_cold_domain *d, const struct nvn_vk_cold_publication *pub,
                          uint32_t n) {
    return cold_guard_scan(c, a, b, draw, d, pub, n);
}

void nvn_gpu_vk_test_cold_overwrite(nvn_gpu_cmd c, nvn_gpu_buf dst, uint64_t off) {
    vkCmdFillBuffer(c->cb, dst->buffer, off, sizeof(uint32_t), 0);
}
void nvn_gpu_vk_test_cold_args(nvn_gpu_cmd c, nvn_gpu_buf dst, uint64_t off) {
    nvn_gpu_copy_buf(c, c->cold.result.buf, c->cold.result.off, dst, off, 2 * sizeof(VkDrawIndirectCommand));
}
#endif
