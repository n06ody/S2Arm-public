// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

enum { BC7_CHUNK = 16384, BC7_PUBLISH_FRAMES = 8 };
#define BC7_PENDING_BYTES (64ull << 20)

static void bc7_invalidate(nvn_gpu_tex t) {
    pthread_mutex_lock(&g_mu);
    if (t->bc7_upload || t->bc7_ready || (g.bc7_job.src == t && g.bc7_job.generation == t->bc7_generation))
        bc7_count(BC7_INVALIDATE);
    if (t->bc7_tracking != BC7_NEW) t->bc7_tracking = BC7_RETIRED;
    t->bc7_generation++;
    t->bc7_upload = NULL;
    t->bc7_ready = 0;
    if (atomic_load(&g.bc7_promotable) == t) atomic_store(&g.bc7_promotable, NULL);
    pthread_mutex_unlock(&g_mu);
}

static void bc7_note_upload(struct nvn_gpu_cmd_s *c, nvn_gpu_tex t) {
    pthread_mutex_lock(&g_mu);
    if (t->bc7_tracking == BC7_NEW) {
        t->bc7_tracking = BC7_WATCHING;
        t->bc7_upload = c;
        t->bc7_upload_next = c->bc7_uploaded;
        c->bc7_uploaded = t;
        t->refs++; // finish_cmd removes the list before releasing its command references
        bc7_count(BC7_WATCH);
    }
    pthread_mutex_unlock(&g_mu);
}

// 仅由实际录制的绘制/派发采样绑定调用；同一次呈现内的重复采样不能使候选入队。
static void bc7_sample_locked(nvn_gpu_tex t, uint64_t present) {
    if (!t->bc7_ready || t->released || g.closing || g.failed || g.device_lost) return;
    const uint64_t stamp = present + 1, prior = t->bc7_last_sample;
    if (prior == stamp) return;
    t->bc7_last_sample = stamp;
    if (!prior) {
        bc7_count(BC7_FIRST_SAMPLE);
        return;
    }
    if (g.bc7_job.src) {
        bc7_count(BC7_BUSY_SKIP);
        return;
    }
    t->bc7_ready = 0;
    t->bc7_tracking = BC7_ENCODING;
    t->refs++;
    g.bc7_job.src = t;
    g.bc7_job.generation = t->bc7_generation;
    atomic_store(&g.bc7_has_job, 1);
    bc7_count(BC7_NOMINATE);
    if (t->bc7_nominations++) bc7_count(BC7_REQUEUE);
}

static int bc7_job_current(const struct bc7_job *job) {
    return job->src && !job->src->released && job->generation == job->src->bc7_generation &&
           !g.failed && !g.device_lost && !g.closing;
}

static void bc7_finish_locked(struct nvn_gpu_cmd_s *c, int executed) {
    while (c->bc7_uploaded) {
        nvn_gpu_tex t = c->bc7_uploaded;
        c->bc7_uploaded = t->bc7_upload_next;
        t->bc7_upload_next = NULL;
        if (t->bc7_upload == c) {
            t->bc7_upload = NULL;
            t->bc7_ready = executed && !t->released && !g.failed && !g.device_lost;
        }
        unref_locked(&g, t, REF_TEX);
    }
    if (executed) for (size_t i = 0; i < c->bc7_sample_cap; i++) {
        const struct bc7_draw_sample sample = c->bc7_samples[i];
        if (!sample.tex) continue;
        bc7_sample_locked(sample.tex, sample.first);
        if (sample.last != sample.first) bc7_sample_locked(sample.tex, sample.last);
    }
    if (!c->bc7_work) return;
    struct bc7_job *job = &g.bc7_job;
    if (!executed || !bc7_job_current(job) || *(const uint32_t *)g.bc7_output->mapped) {
        job->state = BC7_REJECTED;
    } else if (job->level == job->src->levels) {
        job->present = atomic_load(&g.bc7_present);
        job->state = BC7_COMPLETE;
        atomic_store(&g.bc7_promotable, job->src);
    } else {
        job->state = BC7_IDLE;
    }
}

// 只在没有等待栅栏的块时解绑。调用方在 g_mu 外归还未发布的目标。
static nvn_gpu_tex bc7_drop_locked(void) {
    struct bc7_job *job = &g.bc7_job;
    nvn_gpu_tex dst = job->dst;
    atomic_store(&g.bc7_promotable, NULL);
    job->src->bc7_tracking = BC7_RETIRED;
    unref_locked(&g, job->src, REF_TEX);
    memset(job, 0, sizeof *job);
    atomic_store(&g.bc7_has_job, 0);
    return dst;
}

int nvn_gpu_tex_bc7_ready(nvn_gpu_tex src) {
    return src && atomic_load(&g.bc7_promotable) == src;
}

nvn_gpu_tex nvn_gpu_tex_bc7_take(nvn_gpu_tex src) {
    if (!nvn_gpu_tex_bc7_ready(src)) return NULL;
    pthread_mutex_lock(&g_mu);
    nvn_gpu_tex dst = NULL;
    struct bc7_job *job = &g.bc7_job;
    if (job->src == src && job->state == BC7_COMPLETE && bc7_job_current(job)) {
        dst = bc7_drop_locked();
        bc7_count(BC7_PUBLISH);
    }
    pthread_mutex_unlock(&g_mu);
    return dst;
}

static int bc7_scratch(void) {
    if (g.bc7_set) return 1;
    const uint64_t bytes = BC7_CHUNK * 16u;
    nvn_gpu_err err = {0};
    if (!g.bc7_input && !rc_is(buf_create_impl(bytes, NVN_GPU_MEM_DEVICE, NULL, 0, &g.bc7_input, &err), NVN_GPU_OK))
        return 0;
    if (!g.bc7_output && !rc_is(buf_create_impl(bytes + 16, NVN_GPU_MEM_SHARED, NULL, 1, &g.bc7_output, &err), NVN_GPU_OK))
        return 0;
    if (!g.bc7_tables && !rc_is(buf_create_impl(sizeof nvk_bc7_hybrid_tables, NVN_GPU_MEM_SHARED,
                                               nvk_bc7_hybrid_tables, 1, &g.bc7_tables, &err), NVN_GPU_OK))
        return 0;
    const VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    const VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size,
    };
    if (vkCreateDescriptorPool(g.d, &pool, NULL, &g.bc7_pool) != VK_SUCCESS) return 0;
    const VkDescriptorSetAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = g.bc7_pool, .descriptorSetCount = 1, .pSetLayouts = &g_bc7_astc.dsl,
    };
    if (vkAllocateDescriptorSets(g.d, &ai, &g.bc7_set) != VK_SUCCESS) {
        vkDestroyDescriptorPool(g.d, g.bc7_pool, NULL);
        g.bc7_pool = VK_NULL_HANDLE;
        g.bc7_set = VK_NULL_HANDLE;
        return 0;
    }
    const VkDescriptorBufferInfo buffers[] = {
        {g.bc7_input->buffer, 0, bytes}, {g.bc7_output->buffer, 0, bytes + 16},
        {g.bc7_tables->buffer, 0, sizeof nvk_bc7_hybrid_tables},
    };
    VkWriteDescriptorSet writes[3] = {0};
    for (uint32_t i = 0; i < 3; i++) {
        writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g.bc7_set, .dstBinding = i,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffers[i],
        };
    }
    vkUpdateDescriptorSets(g.d, 3, writes, 0, NULL);
    return 1;
}

static int bc7_admit(struct bc7_job *job) {
    uint64_t offset, row, plane, total;
    uint32_t layers;
    nvn_gpu_err err = {0};
    if (!bc_shadow_layout(job->src, 16, 0, &offset, &row, &plane, &layers, &total) ||
        row > BC7_CHUNK * 16u || total > BC7_PENDING_BYTES / 2 || !bc_astc_can_encode(BC7_CHUNK) ||
        !rc_is(bc_new_storage(job->src, NVN_GPU_TEX_BC7_VALIDATED, &job->dst, &err), NVN_GPU_OK) ||
        !bc_astc_image(job->dst) || !bc_shadow_create(job->dst, total) ||
        job->dst->mem_size + job->dst->bc_shadow_mem_size > BC7_PENDING_BYTES) return 0;
    return bc7_scratch();
}

static int bc7_record_chunk(struct nvn_gpu_cmd_s *c, struct bc7_job *job) {
    uint64_t offset, row, plane, total;
    uint32_t layers;
    if (!bc_shadow_layout(job->src, 16, job->level, &offset, &row, &plane, &layers, &total) ||
        !lay_get(c, job->dst, 1)) return 0;
    uint32_t width = job->src->w >> job->level, height = job->src->h >> job->level;
    if (!width) width = 1;
    if (!height) height = 1;
    const uint32_t rows = (height + 3) / 4;
    uint32_t nrows = (uint32_t)(BC7_CHUNK * 16u / row);
    if (nrows > rows - job->row) nrows = rows - job->row;
    const VkDeviceSize at = offset + job->layer * plane + job->row * row, bytes = nrows * row;
    const uint32_t count = (uint32_t)(bytes / 16);
    const uint32_t y = job->row * 4, end_y = (job->row + nrows == rows) ? height : y + nrows * 4;
    VkBuffer input = g.bc7_input->buffer, output = g.bc7_output->buffer;
    full_barrier(c);
    const VkBufferCopy staging = {at, 0, bytes};
    const VkBufferCopy shadow = {at, at, bytes};
    vkCmdCopyBuffer(c->cb, job->src->bc_sbuf, input, 1, &staging);
    vkCmdCopyBuffer(c->cb, job->src->bc_sbuf, job->dst->bc_sbuf, 1, &shadow);
    vkCmdFillBuffer(c->cb, output, 0, 16, 0);
    mem_barrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_bc7_astc.pipe);
    vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_bc7_astc.layout, 0, 1, &g.bc7_set, 0, NULL);
    vkCmdPushConstants(c->cb, g_bc7_astc.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof count, &count);
    const int timing = bc_time_begin(c, BC_TIME_ASTC, NVN_FORMAT_BPTC_UNORM, count);
    vkCmdDispatch(c->cb, (count + 63) / 64, 1, 1);
    bc_time_end(c, timing);
    // 失败块也写入私有图像区域。该图只有在所有段自然完成且状态均为零后才能取走。
    mem_barrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    image_barrier(c, job->dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkBufferImageCopy copy = {
        .bufferOffset = 16, .imageOffset = {0, (int32_t)y, 0}, .imageExtent = {width, end_y - y, 1},
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, job->level, job->layer, 1},
    };
    vkCmdCopyBufferToImage(c->cb, output, job->dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    // commit 的 HOST 屏障让映射的状态字可供自然 fence 完成线程读取。
    c->bc7_work = 1;
    job->row += nrows;
    if (job->row == rows) {
        job->row = 0;
        if (++job->layer == layers) {
            job->layer = 0;
            job->level++;
        }
    }
    return 1;
}

int nvn_gpu_bc7_step(nvn_gpu_cmd c) {
    if (!g.bc7_astc_on || !atomic_load(&g.bc7_has_job) || !c || c->in_pass || !rc_is(c->sticky, NVN_GPU_OK)) return 0;
    const uint64_t present = atomic_load(&g.bc7_present);
    if (!present || present == g.bc7_last_step) return 0;
    g.bc7_last_step = present;

    pthread_mutex_lock(&g_mu);
    struct bc7_job *job = &g.bc7_job;
    nvn_gpu_tex discarded = NULL;
    if (job->state == BC7_SUBMITTED) {
        bc7_count(BC7_FENCE_SKIP);
        pthread_mutex_unlock(&g_mu);
        return 0;
    }
    if (job->src && (!bc7_job_current(job) || job->state == BC7_REJECTED ||
                     (job->state == BC7_COMPLETE && present >= job->present && present - job->present >= BC7_PUBLISH_FRAMES))) {
        bc7_count(BC7_REJECT);
        discarded = bc7_drop_locked();
    }
    const int pending = job->src && job->state == BC7_IDLE;
    const int announced = job->state == BC7_COMPLETE && !job->announced;
    if (announced) job->announced = 1;
    if (pending) job->state = BC7_SUBMITTED;
    pthread_mutex_unlock(&g_mu);
    if (discarded) nvn_gpu_tex_release(discarded);
    if (!pending) return announced;

    // 分配与录制都在提交线程；计算或预算不合适时保留首用的 RGBA8。
    g_bc7_in_step = g.bc_time_on;
    int recorded = (job->dst || bc7_admit(job)) && bc7_record_chunk(c, job);
    g_bc7_in_step = 0;
    if (!recorded) {
        pthread_mutex_lock(&g_mu);
        job->state = BC7_REJECTED;
        pthread_mutex_unlock(&g_mu);
    }
    return 0;
}

#ifdef S2A_BC7_ASYNC_TEST
void nvn_vk_test_bc7_present(void) { atomic_fetch_add(&g.bc7_present, 1); }

// 无 ASTC 能力的 CI 也运行生产完成/代际守卫。物理图像与 Vulkan 编码另走真实设备夹具。
int nvn_vk_test_bc7_completion(int executed, unsigned status, int stale) {
    struct nvn_gpu_tex_s source = {.refs = 1, .levels = 1}, candidate = {0};
    struct nvn_gpu_buf_s readback = {.mapped = (uint8_t *)&status};
    struct nvn_gpu_cmd_s command = {.bc7_work = 1};
    pthread_mutex_lock(&g_mu);
    g.bc7_job = (struct bc7_job){.src = &source, .dst = &candidate, .level = 1, .state = BC7_SUBMITTED};
    g.bc7_output = &readback;
    pthread_mutex_unlock(&g_mu);
    int result = nvn_gpu_tex_bc7_take(&source) ? 2 : 0;
    if (stale) bc7_invalidate(&source);
    pthread_mutex_lock(&g_mu);
    bc7_finish_locked(&command, executed);
    pthread_mutex_unlock(&g_mu);
    if (nvn_gpu_tex_bc7_take(&source) == &candidate) result |= 1;
    pthread_mutex_lock(&g_mu);
    if (g.bc7_job.src) bc7_drop_locked();
    g.bc7_output = NULL;
    pthread_mutex_unlock(&g_mu);
    return result;
}
#endif
