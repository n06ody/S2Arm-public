#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "nvn_vk_pipe_worker.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// 单个 worker 实例（契约：一个进程一个 Vulkan device/pipeline cache）。所有字段只在 g_mu 下读写；
// 只有 vkCreateGraphicsPipelines 在 g_cache_mu 下、g_mu 外执行。
static struct {
    VkDevice device;
    VkPipelineCache cache;
    uint64_t generation;
    int started;
    int stop;
    int thread_running;
    int thread_done;
    pthread_t thread;
    struct nvk_queued_job { uint64_t id; struct nvk_pipe_job_spec spec; } queue[NVK_PW_QUEUE];
    int qhead, qcount;
    uint64_t next_job_id;
    int have_running;
    uint64_t running_job_id;
    struct nvk_pipe_recipe running_recipe;
    uint64_t running_generation;
    struct nvk_pipe_job_result results[NVK_PW_RESULTS];
    int rhead, rcount;
    struct nvn_vk_pipe_worker_stats stats;
} g_pw;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_cache_mu = PTHREAD_MUTEX_INITIALIZER;

// W2.7 longframe 归因：只记真实 vkCreateGraphicsPipelines 的次数与墙钟，命中缓存/复制配方不计。
// 读写都在 g_cache_mu 下，和创建调用同一把锁，读到的数不会跨在途创建。
static uint64_t g_pipe_attempts, g_pipe_failures, g_pipe_ns, g_pipe_ns_max;

static uint64_t pipe_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

void nvk_pipeline_long_stats(uint64_t *attempts, uint64_t *failures, uint64_t *ns, uint64_t *ns_max) {
    pthread_mutex_lock(&g_cache_mu);
    if (attempts) *attempts = g_pipe_attempts;
    if (failures) *failures = g_pipe_failures;
    if (ns) *ns = g_pipe_ns;
    if (ns_max) *ns_max = g_pipe_ns_max;
    pthread_mutex_unlock(&g_cache_mu);
}
static pthread_cond_t g_work_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER;

static void free_job_inputs(struct nvk_pipe_job_spec *j) {
    for (uint32_t i = 0; i < j->stage_count && i < NVK_PW_MAX_STAGES; i++)
        free((void *)j->stages[i].spirv);
    j->stage_count = 0;
    memset(j, 0, sizeof *j);
}

static int stage_bytes_ok(size_t bytes) {
    return bytes > 0 && bytes <= NVK_PW_MAX_SPIRV_BYTES && (bytes % 4) == 0;
}

static int validate_spec(const struct nvk_pipe_job_spec *s, char *why, size_t why_size) {
    if (!s || !s->device || !s->cache || !s->layout || !s->render_pass) {
        if (why && why_size) snprintf(why, why_size, "pipeline job: missing device/cache/layout/render pass");
        return -1;
    }
    if (s->stage_count < 1 || s->stage_count > NVK_PW_MAX_STAGES) {
        if (why && why_size) snprintf(why, why_size, "pipeline job: invalid stage count");
        return -1;
    }
    int seen_vs = 0, seen_fs = 0;
    for (uint32_t i = 0; i < s->stage_count; i++) {
        const struct nvk_pipe_stage_spec *st = &s->stages[i];
        if (!st->module) {
            if (!st->spirv || !stage_bytes_ok(st->bytes)) {
                if (why && why_size) snprintf(why, why_size, "pipeline job: invalid SPIR-V bytes in stage %u", i);
                return -1;
            }
        } else if (st->spirv || st->bytes) {
            if (why && why_size) snprintf(why, why_size, "pipeline job: stage %u has both module and SPIR-V", i);
            return -1;
        }
        if (!st->entry[0] || !memchr(st->entry, 0, sizeof st->entry)) {
            if (why && why_size) snprintf(why, why_size, "pipeline job: empty shader entry in stage %u", i);
            return -1;
        }
        if (st->stage == VK_SHADER_STAGE_VERTEX_BIT) {
            if (seen_vs++) { if (why && why_size) snprintf(why, why_size, "pipeline job: duplicate vertex stage"); return -1; }
        } else if (st->stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
            if (seen_fs++) { if (why && why_size) snprintf(why, why_size, "pipeline job: duplicate fragment stage"); return -1; }
        } else {
            if (why && why_size) snprintf(why, why_size, "pipeline job: unsupported stage 0x%x (VS+FS only)",
                                          (unsigned)st->stage);
            return -1;
        }
    }
    if (s->binding_count > NVK_PW_MAX_BINDINGS || s->attr_count > NVK_PW_MAX_ATTRS ||
        s->blend_count > NVK_PW_MAX_BLEND || s->dynamic_count > NVK_PW_MAX_DYNAMIC) {
        if (why && why_size) snprintf(why, why_size, "pipeline job: fixed capacity exceeded");
        return -1;
    }
    if (s->vp.viewportCount > 1 || s->vp.scissorCount > 1) {
        if (why && why_size) snprintf(why, why_size, "pipeline job: viewport/scissor count must be <= 1");
        return -1;
    }
    // submit 只复制定长内容，不复制指针指向的数据：任何会进入驱动的 pNext/掩码指针都必须是 NULL，
    // 否则 worker 线程可能读到 owner 的栈/临时对象。
    if (s->ia.pNext || s->vp.pNext || s->rs.pNext || s->ms.pNext || s->cb.pNext || s->ds.pNext ||
        s->ms.pSampleMask) {
        if (why && why_size) snprintf(why, why_size, "pipeline job: pNext/pointer-bearing state must be NULL");
        return -1;
    }
    if (!s->blend_count && s->cb.attachmentCount) {
        if (why && why_size) snprintf(why, why_size, "pipeline job: blend attachment count mismatch");
        return -1;
    }
    return 0;
}

static void drop_result_locked(struct nvk_pipe_job_result *r) {
    if (r->pipeline && r->device) vkDestroyPipeline(r->device, r->pipeline, NULL);
    memset(r, 0, sizeof *r);
}

static void free_queued_locked(void) {
    while (g_pw.qcount > 0) {
        free_job_inputs(&g_pw.queue[g_pw.qhead].spec);
        g_pw.qhead = (g_pw.qhead + 1) % NVK_PW_QUEUE;
        g_pw.qcount--;
    }
}

static void free_results_locked(void) {
    while (g_pw.rcount > 0) {
        drop_result_locked(&g_pw.results[g_pw.rhead]);
        g_pw.rhead = (g_pw.rhead + 1) % NVK_PW_RESULTS;
        g_pw.rcount--;
    }
}

static int make_pipeline(const struct nvk_pipe_job_spec *j, VkPipeline *out, char *why, size_t why_size) {
    *out = VK_NULL_HANDLE;
    VkShaderModule modules[NVK_PW_MAX_STAGES] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    int owned[NVK_PW_MAX_STAGES] = { 0, 0 };
    VkPipelineShaderStageCreateInfo stages[NVK_PW_MAX_STAGES];
    memset(stages, 0, sizeof stages);
    int rc = -1;
    for (uint32_t i = 0; i < j->stage_count; i++) {
        if (j->stages[i].module) {
            modules[i] = j->stages[i].module;
        } else {
            VkShaderModuleCreateInfo smci;
            memset(&smci, 0, sizeof smci);
            smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            smci.codeSize = j->stages[i].bytes;
            smci.pCode = j->stages[i].spirv;
            VkResult vr = vkCreateShaderModule(j->device, &smci, NULL, &modules[i]);
            if (vr != VK_SUCCESS) {
                if (why && why_size) snprintf(why, why_size, "vkCreateShaderModule failed (%d)", (int)vr);
                goto done;
            }
            owned[i] = 1;
        }
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = j->stages[i].stage;
        stages[i].module = modules[i];
        stages[i].pName = j->stages[i].entry;
    }
    {
        VkPipelineVertexInputStateCreateInfo vi;
        memset(&vi, 0, sizeof vi);
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = j->binding_count;
        vi.pVertexBindingDescriptions = j->binding_count ? j->bindings : NULL;
        vi.vertexAttributeDescriptionCount = j->attr_count;
        vi.pVertexAttributeDescriptions = j->attr_count ? j->attrs : NULL;
        VkPipelineInputAssemblyStateCreateInfo ia = j->ia;
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.pNext = NULL;
        VkPipelineViewportStateCreateInfo vp = j->vp;
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.pNext = NULL;
        vp.pViewports = vp.viewportCount ? &j->viewport : NULL;
        vp.pScissors = vp.scissorCount ? &j->scissor : NULL;
        VkPipelineRasterizationStateCreateInfo rs = j->rs;
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.pNext = NULL;
        VkPipelineMultisampleStateCreateInfo ms = j->ms;
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.pNext = NULL;
        VkPipelineColorBlendStateCreateInfo cb = j->cb;
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.pNext = NULL;
        cb.attachmentCount = j->blend_count;
        cb.pAttachments = j->blend_count ? j->blend : NULL;
        VkPipelineDepthStencilStateCreateInfo ds = j->ds;
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.pNext = NULL;
        VkPipelineDynamicStateCreateInfo dyn;
        memset(&dyn, 0, sizeof dyn);
        dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = j->dynamic_count;
        dyn.pDynamicStates = j->dynamic_count ? j->dynamic : NULL;
        VkGraphicsPipelineCreateInfo gpi;
        memset(&gpi, 0, sizeof gpi);
        gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpi.flags = j->flags;
        gpi.stageCount = j->stage_count;
        gpi.pStages = stages;
        gpi.pVertexInputState = &vi;
        gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vp;
        gpi.pRasterizationState = &rs;
        gpi.pMultisampleState = &ms;
        gpi.pColorBlendState = &cb;
        gpi.pDepthStencilState = j->has_depth ? &ds : NULL;
        gpi.pDynamicState = j->dynamic_count ? &dyn : NULL;
        gpi.layout = j->layout;
        gpi.renderPass = j->render_pass;
        gpi.subpass = j->subpass;
        gpi.basePipelineHandle = j->base_pipeline;
        gpi.basePipelineIndex = j->base_pipeline_index;
        // 计数与耗时都在这把锁下更新：读侧（nvk_pipeline_long_stats）也用同一把锁，
        // 之前解锁后再累加耗时，读侧拿到的是一半同步、一半不同步的账。
        uint64_t t0 = pipe_now_ns();
        pthread_mutex_lock(&g_cache_mu);
        VkResult vr = vkCreateGraphicsPipelines(j->device, j->cache, 1, &gpi, NULL, out);
        uint64_t cost = pipe_now_ns() - t0;
        g_pipe_attempts++;
        if (vr != VK_SUCCESS) g_pipe_failures++;
        g_pipe_ns += cost;
        if (cost > g_pipe_ns_max) g_pipe_ns_max = cost;
        pthread_mutex_unlock(&g_cache_mu);
        if (vr != VK_SUCCESS) {
            if (why && why_size) snprintf(why, why_size, "vkCreateGraphicsPipelines failed (%d)", (int)vr);
            goto done;
        }
    }
    rc = 0;
done:
    for (uint32_t i = 0; i < j->stage_count; i++)
        if (owned[i] && modules[i]) vkDestroyShaderModule(j->device, modules[i], NULL);
    return rc;
}

static void *worker_main(void *unused) {
    (void)unused;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        while (!g_pw.stop && g_pw.qcount == 0) pthread_cond_wait(&g_work_cv, &g_mu);
        if (g_pw.stop) break;
        uint64_t job_id = g_pw.queue[g_pw.qhead].id;
        struct nvk_pipe_job_spec job = g_pw.queue[g_pw.qhead].spec;
        uint64_t current_generation = g_pw.generation;
        memset(&g_pw.queue[g_pw.qhead], 0, sizeof g_pw.queue[g_pw.qhead]);
        g_pw.qhead = (g_pw.qhead + 1) % NVK_PW_QUEUE;
        g_pw.qcount--;
        g_pw.have_running = 1;
        g_pw.running_job_id = job_id;
        g_pw.running_recipe = job.recipe;
        g_pw.running_generation = job.device_generation;
        pthread_mutex_unlock(&g_mu);

        int stale = (job.device_generation != current_generation);
        int drop_result = stale;
        VkPipeline pipeline = VK_NULL_HANDLE;
        char why[192] = "";
        int ok = 0;
        if (stale) {
            snprintf(why, sizeof why, "device generation changed before creation");
        } else if (make_pipeline(&job, &pipeline, why, sizeof why) == 0) {
            ok = 1;
        }
        const int driver_ok = ok;
        const int driver_failed = !stale && !ok;

        pthread_mutex_lock(&g_mu);
        g_pw.have_running = 0;
        if (g_pw.stop) {
            // Stop：迟到结果丢弃，不转交 owner。
            if (pipeline) { vkDestroyPipeline(job.device, pipeline, NULL); g_pw.stats.dropped++; }
            if (driver_ok) g_pw.stats.created++;
            else if (driver_failed) g_pw.stats.create_failed++;
            free_job_inputs(&job);
            g_pw.thread_done = 1;
            pthread_cond_broadcast(&g_done_cv);
            pthread_mutex_unlock(&g_mu);
            return NULL;
        }
        if (ok && job.device_generation != g_pw.generation) {
            // 创建期间换代：旧设备的结果不能交给 owner。
            vkDestroyPipeline(job.device, pipeline, NULL);
            pipeline = VK_NULL_HANDLE;
            ok = 0;
            g_pw.stats.dropped++;
            g_pw.stats.rejected_stale++;
            drop_result = 1;   // 换代后的迟到结果不转交（owner 不该再等旧代际）
            snprintf(why, sizeof why, "device generation changed while creating; pipeline discarded");
        }
        if (stale) g_pw.stats.rejected_stale++;
        if (ok && g_pw.rcount >= NVK_PW_RESULTS) {
            vkDestroyPipeline(job.device, pipeline, NULL);
            pipeline = VK_NULL_HANDLE;
            ok = 0;
            g_pw.stats.dropped++;
            snprintf(why, sizeof why, "pipeline result ring full; created pipeline discarded");
        }
        if (driver_ok) g_pw.stats.created++;
        else if (driver_failed) g_pw.stats.create_failed++;
        if (!drop_result && g_pw.rcount < NVK_PW_RESULTS && (ok || why[0])) {
            struct nvk_pipe_job_result *r = &g_pw.results[(g_pw.rhead + g_pw.rcount) % NVK_PW_RESULTS];
            memset(r, 0, sizeof *r);
            r->job_id = job_id;
            r->device_generation = job.device_generation;
            r->recipe = job.recipe;
            r->pipeline = pipeline;
            r->ok = ok;
            r->device = job.device;
            snprintf(r->why, sizeof r->why, "%s", why);
            g_pw.rcount++;
        } else if (pipeline) {
            vkDestroyPipeline(job.device, pipeline, NULL);
            g_pw.stats.dropped++;
        }
        pthread_cond_broadcast(&g_done_cv);
        free_job_inputs(&job);
    }
    g_pw.thread_done = 1;
    pthread_cond_broadcast(&g_done_cv);
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

int nvn_vk_pipe_worker_start(VkDevice device, VkPipelineCache cache, uint64_t device_generation,
                             char *why, size_t why_size) {
    if (!device || !cache) {
        if (why && why_size) snprintf(why, why_size, "pipeline worker: no device/cache");
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    if (g_pw.started) {
        int same = g_pw.device == device && g_pw.cache == cache && g_pw.generation == device_generation;
        pthread_mutex_unlock(&g_mu);
        if (!same && why && why_size) snprintf(why, why_size, "pipeline worker: already started for another device/cache");
        return same ? 0 : -1;
    }
    if (g_pw.thread_running) {
        if (!g_pw.thread_done) {
            pthread_mutex_unlock(&g_mu);
            if (why && why_size) snprintf(why, why_size, "pipeline worker: previous worker still creating pipelines");
            return -1;
        }
        pthread_join(g_pw.thread, NULL);   // 已置 done：不会阻塞
        g_pw.thread_running = 0;
        g_pw.thread_done = 0;
        free_queued_locked();
        free_results_locked();
    }
    g_pw.device = device;
    g_pw.cache = cache;
    g_pw.generation = device_generation;
    g_pw.stop = 0;
    g_pw.started = 1;
    g_pw.qhead = g_pw.qcount = 0;
    g_pw.rhead = g_pw.rcount = 0;
    g_pw.next_job_id = 0;
    g_pw.have_running = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);   // 驱动建管线也可能深调用；虚拟内存按需提交
    int cr = pthread_create(&g_pw.thread, &attr, worker_main, NULL);
    pthread_attr_destroy(&attr);
    if (cr != 0 && pthread_create(&g_pw.thread, NULL, worker_main, NULL) != 0) {
        g_pw.started = 0;
        pthread_mutex_unlock(&g_mu);
        if (why && why_size) snprintf(why, why_size, "pipeline worker: thread creation failed");
        return -1;
    }
    g_pw.thread_running = 1;
    g_pw.thread_done = 0;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

int nvn_vk_pipe_worker_active(void) {
    pthread_mutex_lock(&g_mu);
    int active = g_pw.started && !g_pw.stop;
    pthread_mutex_unlock(&g_mu);
    return active;
}

void nvn_vk_pipe_worker_set_generation(uint64_t device_generation) {
    pthread_mutex_lock(&g_mu);
    g_pw.generation = device_generation;
    pthread_mutex_unlock(&g_mu);
}

uint64_t nvn_vk_pipe_worker_generation(void) {
    pthread_mutex_lock(&g_mu);
    uint64_t gen = g_pw.generation;
    pthread_mutex_unlock(&g_mu);
    return gen;
}

int nvn_vk_pipe_worker_submit(const struct nvk_pipe_job_spec *spec, uint64_t *job_id,
                              char *why, size_t why_size) {
    if (job_id) *job_id = 0;
    if (validate_spec(spec, why, why_size)) return -1;
    pthread_mutex_lock(&g_mu);
    if (!g_pw.started) {
        pthread_mutex_unlock(&g_mu);
        if (why && why_size) snprintf(why, why_size, "pipeline worker: not running");
        return -3;
    }
    if (spec->device != g_pw.device || spec->cache != g_pw.cache ||
        spec->device_generation != g_pw.generation) {
        pthread_mutex_unlock(&g_mu);
        if (why && why_size) snprintf(why, why_size, "pipeline worker: device/cache/generation mismatch");
        return -4;
    }
    // 同配方去重：队列、在途、待取结果都算“已经有一次创建”。owner 不需要自己再做一遍。
    for (int i = 0; i < g_pw.qcount; i++) {
        const struct nvk_queued_job *q = &g_pw.queue[(g_pw.qhead + i) % NVK_PW_QUEUE];
        if (q->spec.device_generation == spec->device_generation &&
            nvk_pipe_recipe_match(&q->spec.recipe, &spec->recipe)) {
            g_pw.stats.deduped++;
            if (job_id) *job_id = 0;
            pthread_mutex_unlock(&g_mu);
            return 1;
        }
    }
    if (g_pw.have_running && g_pw.running_generation == spec->device_generation &&
        nvk_pipe_recipe_match(&g_pw.running_recipe, &spec->recipe)) {
        g_pw.stats.deduped++;
        if (job_id) *job_id = g_pw.running_job_id;
        pthread_mutex_unlock(&g_mu);
        return 1;
    }
    for (int i = 0; i < g_pw.rcount; i++) {
        struct nvk_pipe_job_result *r = &g_pw.results[(g_pw.rhead + i) % NVK_PW_RESULTS];
        if (r->device_generation == spec->device_generation && nvk_pipe_recipe_match(&r->recipe, &spec->recipe)) {
            g_pw.stats.deduped++;
            if (job_id) *job_id = r->job_id;
            pthread_mutex_unlock(&g_mu);
            return 1;
        }
    }
    if (g_pw.qcount >= NVK_PW_QUEUE) {
        g_pw.stats.queue_full++;
        pthread_mutex_unlock(&g_mu);
        if (why && why_size) snprintf(why, why_size, "pipeline worker: queue full");
        return -2;
    }
    // 先复制指针内容，失败时不能留下半个 job。
    struct nvk_pipe_job_spec job = *spec;
    for (uint32_t i = 0; i < spec->stage_count; i++) {
        void *copy = malloc(spec->stages[i].bytes);
        if (!copy) {
            for (uint32_t k = 0; k < i; k++) free((void *)job.stages[k].spirv);
            pthread_mutex_unlock(&g_mu);
            if (why && why_size) snprintf(why, why_size, "pipeline worker: out of memory copying SPIR-V");
            return -1;
        }
        memcpy(copy, spec->stages[i].spirv, spec->stages[i].bytes);
        job.stages[i].spirv = (const uint32_t *)copy;
    }
    uint64_t id = ++g_pw.next_job_id;
    g_pw.queue[(g_pw.qhead + g_pw.qcount) % NVK_PW_QUEUE].id = id;
    g_pw.queue[(g_pw.qhead + g_pw.qcount) % NVK_PW_QUEUE].spec = job;
    g_pw.qcount++;
    if (job_id) *job_id = id;
    g_pw.stats.submitted++;
    pthread_cond_signal(&g_work_cv);
    pthread_mutex_unlock(&g_mu);
    return 0;
}

int nvn_vk_pipe_worker_take(struct nvk_pipe_job_result *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&g_mu);
    if (g_pw.rcount == 0) { pthread_mutex_unlock(&g_mu); return 0; }
    *out = g_pw.results[g_pw.rhead];
    memset(&g_pw.results[g_pw.rhead], 0, sizeof g_pw.results[g_pw.rhead]);
    g_pw.rhead = (g_pw.rhead + 1) % NVK_PW_RESULTS;
    g_pw.rcount--;
    g_pw.stats.taken++;
    pthread_mutex_unlock(&g_mu);
    return 1;
}

int nvn_vk_pipe_worker_wait(const struct nvk_pipe_recipe *recipe, uint64_t device_generation,
                            uint32_t deadline_ms, struct nvk_pipe_job_result *out) {
    if (!recipe || !out) return -1;
    memset(out, 0, sizeof *out);
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += deadline_ms / 1000;
    dl.tv_nsec += (long)(deadline_ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g_mu);
    for (;;) {
        // 结果环里找本配方的同代际结果：命中即从环中取出，其余结果保持 FIFO 不动。
        for (int i = 0; i < g_pw.rcount; i++) {
            int idx = (g_pw.rhead + i) % NVK_PW_RESULTS;
            struct nvk_pipe_job_result *r = &g_pw.results[idx];
            if (r->device_generation == device_generation &&
                nvk_pipe_recipe_match(&r->recipe, recipe)) {
                *out = *r;
                for (int k = i; k + 1 < g_pw.rcount; k++) {
                    int from = (g_pw.rhead + k + 1) % NVK_PW_RESULTS;
                    int to = (g_pw.rhead + k) % NVK_PW_RESULTS;
                    g_pw.results[to] = g_pw.results[from];
                    memset(&g_pw.results[from], 0, sizeof g_pw.results[from]);
                }
                g_pw.rcount--;
                g_pw.stats.taken++;
                pthread_mutex_unlock(&g_mu);
                return 1;
            }
        }
        // 证据检查：本配方还在队列或在途才算“在等的东西存在”，否则返回 -1 让 owner
        // 不空等（提交被拒/结果环挤出/stop 清场都会造成无证据）。
        int evidence = 0;
        for (int i = 0; i < g_pw.qcount; i++) {
            const struct nvk_queued_job *q = &g_pw.queue[(g_pw.qhead + i) % NVK_PW_QUEUE];
            if (q->spec.device_generation == device_generation &&
                nvk_pipe_recipe_match(&q->spec.recipe, recipe)) { evidence = 1; break; }
        }
        if (!evidence && g_pw.have_running && g_pw.running_generation == device_generation &&
            nvk_pipe_recipe_match(&g_pw.running_recipe, recipe)) evidence = 1;
        if (!evidence) { pthread_mutex_unlock(&g_mu); return -1; }
        if (deadline_ms == 0 ||
            pthread_cond_timedwait(&g_done_cv, &g_mu, &dl) == ETIMEDOUT) {
            pthread_mutex_unlock(&g_mu);
            return 0;
        }
    }
}

void nvn_vk_pipe_worker_release(struct nvk_pipe_job_result *out) {
    if (!out) return;
    if (out->pipeline && out->device) vkDestroyPipeline(out->device, out->pipeline, NULL);
    memset(out, 0, sizeof *out);
}

int nvn_vk_pipe_worker_stop(uint32_t budget_ms, char *why, size_t why_size) {
    pthread_mutex_lock(&g_mu);
    if (!g_pw.started && !g_pw.thread_running) { pthread_mutex_unlock(&g_mu); return 0; }
    g_pw.started = 0;
    g_pw.stop = 1;
    pthread_cond_broadcast(&g_work_cv);
    pthread_cond_broadcast(&g_done_cv);
    if (budget_ms > 0 && g_pw.thread_running && !g_pw.thread_done) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += budget_ms / 1000;
        dl.tv_nsec += (long)(budget_ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        while (g_pw.thread_running && !g_pw.thread_done) {
            if (pthread_cond_timedwait(&g_done_cv, &g_mu, &dl) == ETIMEDOUT) break;
        }
    }
    if (g_pw.thread_running && !g_pw.thread_done) {
        // 仍在建管线：保留输入/设备/缓存，迟到结果在 worker 内销毁；未退出前 start 明确拒绝。
        if (why && why_size)
            snprintf(why, why_size, "pipeline worker: still creating after %u ms", budget_ms);
        pthread_mutex_unlock(&g_mu);
        return 1;
    }
    int joined = 0;
    if (g_pw.thread_running) {
        pthread_t th = g_pw.thread;
        g_pw.thread_running = 0;
        pthread_mutex_unlock(&g_mu);
        pthread_join(th, NULL);   // 已置 done：不阻塞
        pthread_mutex_lock(&g_mu);
        joined = 1;
    }
    free_queued_locked();
    free_results_locked();
    g_pw.thread_done = 0;
    g_pw.stop = 0;
    g_pw.device = VK_NULL_HANDLE;
    g_pw.cache = VK_NULL_HANDLE;
    (void)joined;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

void nvn_vk_pipe_worker_cache_lock(void) { pthread_mutex_lock(&g_cache_mu); }
void nvn_vk_pipe_worker_cache_unlock(void) { pthread_mutex_unlock(&g_cache_mu); }

void nvn_vk_pipe_worker_stats(struct nvn_vk_pipe_worker_stats *out) {
    if (!out) return;
    pthread_mutex_lock(&g_mu);
    *out = g_pw.stats;
    pthread_mutex_unlock(&g_mu);
}
