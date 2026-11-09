// F4.4 宿主回归：driver pipeline worker（fake Vulkan，不建真实 device）。
//
// 覆盖的独立期望：
//   1. 管线在 worker 线程创建，owner 只 take 结果；worker 不提交 queue；
//   2. 使用同一 VkDevice / VkPipelineCache；owner 直接创建与 worker 共用同一把 cache 锁；
//   3. 同配方去重（队列/在途/待取），不同配方串行；队列满准确失败；
//   4. 创建失败原样带回；结果只交付一次；
//   5. 设备换代：创建中换代的结果丢弃且不转交；提交代际不符准确拒绝；
//   6. 有界 Stop：创建中返回 1、迟到 pipeline 销毁、未退出前拒绝重启、退出后回收并可再启动；
//   7. 结果环满/Stop 时的丢弃计数来自真实销毁，不是推测。
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvn_vk_pipe_worker.h"

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)

// ---- fake Vulkan ----
static pthread_mutex_t g_fmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fcv = PTHREAD_COND_INITIALIZER;
static int g_block_create, g_create_entered, g_create_release, g_fail_next;
static int g_create_calls, g_module_calls, g_module_destroyed, g_pipeline_destroyed, g_queue_submits;
static pthread_t g_create_thread;
static VkDevice g_seen_device;
static VkPipelineCache g_seen_cache;
static uint32_t g_seen_stage_count;

static void fake_reset(void) {
    pthread_mutex_lock(&g_fmu);
    g_block_create = g_create_entered = g_create_release = g_fail_next = 0;
    g_create_calls = g_module_calls = g_module_destroyed = g_pipeline_destroyed = g_queue_submits = 0;
    g_seen_device = VK_NULL_HANDLE;
    g_seen_cache = VK_NULL_HANDLE;
    g_seen_stage_count = 0;
    pthread_mutex_unlock(&g_fmu);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *info,
                                                    const VkAllocationCallbacks *alloc, VkShaderModule *out) {
    (void)device; (void)alloc;
    if (!info || !out || !info->pCode || info->codeSize < 4) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&g_fmu);
    *out = (VkShaderModule)(uintptr_t)(0x2000 + ++g_module_calls);
    pthread_mutex_unlock(&g_fmu);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice device, VkShaderModule module,
                                                 const VkAllocationCallbacks *alloc) {
    (void)device; (void)module; (void)alloc;
    pthread_mutex_lock(&g_fmu);
    g_module_destroyed++;
    pthread_mutex_unlock(&g_fmu);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                         const VkGraphicsPipelineCreateInfo *infos,
                                                         const VkAllocationCallbacks *alloc, VkPipeline *out) {
    (void)alloc;
    if (!infos || count != 1 || !out) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&g_fmu);
    g_create_calls++;
    g_create_thread = pthread_self();
    g_seen_device = device;
    g_seen_cache = cache;
    g_seen_stage_count = infos->stageCount;
    g_create_entered = 1;
    pthread_cond_broadcast(&g_fcv);
    while (g_block_create && !g_create_release) pthread_cond_wait(&g_fcv, &g_fmu);
    int fail = g_fail_next;
    g_fail_next = 0;
    pthread_mutex_unlock(&g_fmu);
    if (fail) return VK_ERROR_DEVICE_LOST;
    *out = (VkPipeline)(uintptr_t)(0x3000 + g_create_calls);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice device, VkPipeline pipeline,
                                             const VkAllocationCallbacks *alloc) {
    (void)device; (void)pipeline; (void)alloc;
    pthread_mutex_lock(&g_fmu);
    g_pipeline_destroyed++;
    pthread_mutex_unlock(&g_fmu);
}
// worker 不能提交 queue：这个 fake 只用来证明调用次数为 0。
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits,
                                             VkFence fence) {
    (void)queue; (void)count; (void)submits; (void)fence;
    pthread_mutex_lock(&g_fmu);
    g_queue_submits++;
    pthread_mutex_unlock(&g_fmu);
    return VK_SUCCESS;
}

static int wait_flag(int *flag, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 2) {
        pthread_mutex_lock(&g_fmu);
        int v = *flag;
        pthread_mutex_unlock(&g_fmu);
        if (v) return 1;
        usleep(2000);
    }
    return 0;
}

static void set_blocking(int on) {
    pthread_mutex_lock(&g_fmu);
    g_block_create = on;
    g_create_release = 0;
    g_create_entered = 0;
    pthread_mutex_unlock(&g_fmu);
}
static void release_block(void) {
    pthread_mutex_lock(&g_fmu);
    g_create_release = 1;
    pthread_cond_broadcast(&g_fcv);
    pthread_mutex_unlock(&g_fmu);
}
static int get_int(int *p) { pthread_mutex_lock(&g_fmu); int v = *p; pthread_mutex_unlock(&g_fmu); return v; }

static uint32_t g_vs[4] = {0x07230203u, 0u, 0u, 0u};
static uint32_t g_fs[4] = {0x07230203u, 1u, 0u, 0u};

static void make_spec(struct nvk_pipe_job_spec *s, uint64_t generation, unsigned variant) {
    memset(s, 0, sizeof *s);
    s->device_generation = generation;
    s->device = (VkDevice)(uintptr_t)0x1;
    s->cache = (VkPipelineCache)(uintptr_t)0x2;
    s->layout = (VkPipelineLayout)(uintptr_t)0x3;
    s->render_pass = (VkRenderPass)(uintptr_t)0x4;
    s->stage_count = 2;
    s->stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    s->stages[0].spirv = g_vs;
    s->stages[0].bytes = sizeof g_vs;
    snprintf(s->stages[0].entry, sizeof s->stages[0].entry, "main");
    s->stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    s->stages[1].spirv = g_fs;
    s->stages[1].bytes = sizeof g_fs;
    snprintf(s->stages[1].entry, sizeof s->stages[1].entry, "main");
    s->ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    s->vp.viewportCount = 1;
    s->vp.scissorCount = 1;
    s->viewport = (VkViewport){ 0, 0, 100, 50, 0, 1 };
    s->scissor = (VkRect2D){ { 0, 0 }, { 100, 50 } };
    s->rs.polygonMode = VK_POLYGON_MODE_FILL;
    s->rs.cullMode = VK_CULL_MODE_NONE;
    s->rs.lineWidth = 1.0f;
    s->ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    s->blend_count = 1;
    s->blend[0].colorWriteMask = 0xF;
    s->cb.attachmentCount = 1;
    s->dynamic_count = 2;
    s->dynamic[0] = VK_DYNAMIC_STATE_VIEWPORT;
    s->dynamic[1] = VK_DYNAMIC_STATE_SCISSOR;
    s->has_depth = 0;
    s->recipe.layout_policy = NVK_PIPE_LAYOUT_POLICY;
    s->recipe.rt_count = 1;
    s->recipe.prim = 3;
    s->recipe.device = 0x1;
    snprintf(s->recipe.request_id, sizeof s->recipe.request_id, "test-%u", variant);
    for (int i = 0; i < 32; i++) s->recipe.prog_sha[i] = (uint8_t)(variant + i);
}

static int take_one(struct nvk_pipe_job_result *r, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 2) {
        if (nvn_vk_pipe_worker_take(r) == 1) return 1;
        usleep(2000);
    }
    return 0;
}

int main(void) {
    char why[192] = "";
    VkDevice dev = (VkDevice)(uintptr_t)0x1;
    VkPipelineCache cache = (VkPipelineCache)(uintptr_t)0x2;
    uint64_t gen = 7;
    fake_reset();

    CHECK(nvn_vk_pipe_worker_active() == 0, "worker is not active before start");
    CHECK(nvn_vk_pipe_worker_submit(NULL, NULL, why, sizeof why) != 0, "null submit is refused");
    CHECK(nvn_vk_pipe_worker_start(dev, cache, gen, why, sizeof why) == 0, "worker starts");
    CHECK(nvn_vk_pipe_worker_active() == 1, "worker is active");
    CHECK(nvn_vk_pipe_worker_start(dev, cache, gen, why, sizeof why) == 0, "start is idempotent");
    CHECK(nvn_vk_pipe_worker_start((VkDevice)(uintptr_t)0x9, cache, gen, why, sizeof why) == -1,
          "a different device is refused");
    CHECK(nvn_vk_pipe_worker_generation() == gen, "generation recorded");

    // 0) 输入校验：只有定长内容会被复制，任何指针型状态/不合法阶段都准确拒绝。
    {
        struct nvk_pipe_job_spec bad;
        make_spec(&bad, gen, 0);
        VkSampleMask mask = 0xF;
        bad.ms.pSampleMask = &mask;
        CHECK(nvn_vk_pipe_worker_submit(&bad, NULL, why, sizeof why) == -1, "pointer-bearing sample mask is rejected");
        make_spec(&bad, gen, 0);
        bad.vp.pNext = &mask;
        CHECK(nvn_vk_pipe_worker_submit(&bad, NULL, why, sizeof why) == -1, "pNext state is rejected");
        make_spec(&bad, gen, 0);
        bad.stages[0].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
        CHECK(nvn_vk_pipe_worker_submit(&bad, NULL, why, sizeof why) == -1, "unsupported stage is rejected");
        make_spec(&bad, gen, 0);
        bad.stages[0].bytes = 6;
        CHECK(nvn_vk_pipe_worker_submit(&bad, NULL, why, sizeof why) == -1, "non-word SPIR-V length is rejected");
        make_spec(&bad, gen, 0);
        bad.stage_count = 0;
        CHECK(nvn_vk_pipe_worker_submit(&bad, NULL, why, sizeof why) == -1, "missing stages are rejected");
    }

    // 1) 基本创建：worker 线程创建，owner take，worker 不提交 queue。
    struct nvk_pipe_job_spec spec;
    make_spec(&spec, gen, 1);
    uint64_t job_id = 0;
    CHECK(nvn_vk_pipe_worker_submit(&spec, &job_id, why, sizeof why) == 0 && job_id != 0, "submit accepted");
    struct nvk_pipe_job_result res;
    CHECK(take_one(&res, 3000), "owner takes the created pipeline");
    CHECK(res.ok == 1 && res.pipeline != VK_NULL_HANDLE, "pipeline created successfully");
    CHECK(res.job_id == job_id, "result carries the submit job id");
    CHECK(res.device_generation == gen && nvk_pipe_recipe_match(&res.recipe, &spec.recipe),
          "result carries generation and the exact recipe");
    CHECK(!pthread_equal(g_create_thread, pthread_self()), "pipeline is created on the worker thread, not the owner");
    CHECK(g_seen_device == dev && g_seen_cache == cache, "worker uses the registered device and pipeline cache");
    CHECK(g_seen_stage_count == 2, "both compiled stages are handed to the driver");
    CHECK(get_int(&g_queue_submits) == 0, "the pipeline worker never submits to the queue");
    int destroyed_before = get_int(&g_pipeline_destroyed);
    nvn_vk_pipe_worker_release(&res);
    CHECK(get_int(&g_pipeline_destroyed) == destroyed_before + 1, "releasing an uninstalled result destroys it");
    CHECK(nvn_vk_pipe_worker_take(&res) == 0, "a result is delivered only once");

    // 1b) owner 已建好的程序模块可以复用：worker 不再自己建/销毁 module。
    {
        make_spec(&spec, gen, 14);
        spec.stages[0].module = (VkShaderModule)(uintptr_t)0x11;
        spec.stages[0].spirv = NULL;
        spec.stages[0].bytes = 0;
        spec.stages[1].module = (VkShaderModule)(uintptr_t)0x12;
        spec.stages[1].spirv = NULL;
        spec.stages[1].bytes = 0;
        int mods_before = get_int(&g_module_calls);
        int destroyed_before = get_int(&g_module_destroyed);
        CHECK(nvn_vk_pipe_worker_submit(&spec, &job_id, why, sizeof why) == 0, "owner-module job accepted");
        CHECK(take_one(&res, 3000) && res.ok == 1, "pipeline created from owner shader modules");
        CHECK(get_int(&g_module_calls) == mods_before, "worker does not recreate owner shader modules");
        CHECK(get_int(&g_module_destroyed) == destroyed_before, "worker does not destroy owner shader modules");
        nvn_vk_pipe_worker_release(&res);
    }

    // 2) 同配方去重 + 队列容量：worker 被 fake 驱动卡住时，第二个同配方不重复创建。
    set_blocking(1);
    make_spec(&spec, gen, 2);
    CHECK(nvn_vk_pipe_worker_submit(&spec, &job_id, why, sizeof why) == 0, "blocked job accepted");
    CHECK(wait_flag(&g_create_entered, 3000), "worker entered the blocked create");
    struct nvk_pipe_job_spec dup = spec;
    CHECK(nvn_vk_pipe_worker_submit(&dup, &job_id, why, sizeof why) == 1, "same recipe is deduped");
    int accepted = 0;
    for (unsigned v = 3; v < 8; v++) {
        make_spec(&spec, gen, v);
        int rc = nvn_vk_pipe_worker_submit(&spec, &job_id, why, sizeof why);
        if (rc == 0) accepted++;
        else if (rc == -2) break;
    }
    CHECK(accepted == NVK_PW_QUEUE, "queue accepts exactly its capacity while one job is running");
    make_spec(&spec, gen, 9);
    CHECK(nvn_vk_pipe_worker_submit(&spec, &job_id, why, sizeof why) == -2, "queue full is an accurate failure");
    release_block();
    pthread_mutex_lock(&g_fmu);
    g_block_create = 0;
    pthread_mutex_unlock(&g_fmu);
    int got = 0;
    for (int i = 0; i < accepted + 1; i++) {
        if (take_one(&res, 5000)) { CHECK(res.ok == 1, "queued pipeline created"); got++; nvn_vk_pipe_worker_release(&res); }
    }
    CHECK(got == accepted + 1, "every accepted job produces exactly one result");
    struct nvn_vk_pipe_worker_stats st;
    nvn_vk_pipe_worker_stats(&st);
    CHECK(st.deduped >= 1 && st.queue_full >= 1, "dedupe/queue-full counters are actual events");

    // 3) 创建失败原样带回。
    pthread_mutex_lock(&g_fmu);
    g_fail_next = 1;
    pthread_mutex_unlock(&g_fmu);
    make_spec(&spec, gen, 10);
    CHECK(nvn_vk_pipe_worker_submit(&spec, &job_id, why, sizeof why) == 0, "failing job accepted");
    CHECK(take_one(&res, 3000) && res.ok == 0 && strstr(res.why, "vkCreateGraphicsPipelines failed"),
          "driver failure is returned to the owner");
    nvn_vk_pipe_worker_release(&res);

    // 4) 设备换代：创建中换代的结果丢弃，不转交旧代际 pipeline。
    set_blocking(1);
    make_spec(&spec, gen, 11);
    CHECK(nvn_vk_pipe_worker_submit(&spec, &job_id, why, sizeof why) == 0, "job accepted before generation change");
    CHECK(wait_flag(&g_create_entered, 3000), "create entered before generation change");
    nvn_vk_pipe_worker_set_generation(gen + 1);
    release_block();
    usleep(100 * 1000);
    CHECK(nvn_vk_pipe_worker_take(&res) == 0, "a stale-generation pipeline is not handed to the owner");
    nvn_vk_pipe_worker_stats(&st);
    CHECK(st.rejected_stale >= 1 && st.dropped >= 1, "stale pipeline is destroyed and counted");
    gen++;
    pthread_mutex_lock(&g_fmu);
    g_block_create = 0;
    pthread_mutex_unlock(&g_fmu);
    make_spec(&spec, gen, 12);
    struct nvk_pipe_job_spec stale = spec;
    struct nvk_pipe_job_spec wrong_gen = spec;
    wrong_gen.device_generation = gen - 1;
    (void)stale;
    CHECK(nvn_vk_pipe_worker_submit(&wrong_gen, &job_id, why, sizeof why) == -4,
          "submit with a stale generation is refused");

    // 5) owner 直接创建与 worker 共用 VkPipelineCache 锁。
    pthread_mutex_lock(&g_fmu);
    g_create_entered = 0;
    pthread_mutex_unlock(&g_fmu);
    nvn_vk_pipe_worker_cache_lock();
    CHECK(nvn_vk_pipe_worker_submit(&spec, &job_id, why, sizeof why) == 0, "job accepted while owner holds cache lock");
    usleep(50 * 1000);
    CHECK(get_int(&g_create_entered) == 0, "worker cannot create while the owner holds the cache lock");
    nvn_vk_pipe_worker_cache_unlock();
    CHECK(take_one(&res, 3000) && res.ok == 1, "worker creates after the cache lock is released");
    nvn_vk_pipe_worker_release(&res);

    // 5b) 配方键控等待（C4b）：按配方取结果/在途超时/无证据/代际过滤/环内抽取。
    {
        // 无证据：从未提交的配方立即 -1，不让 owner 空等。
        struct nvk_pipe_job_result wres;
        struct nvk_pipe_recipe never;
        make_spec(&spec, gen, 40);
        never = spec.recipe;
        CHECK(nvn_vk_pipe_worker_wait(&never, gen, 50, &wres) == -1,
              "wait on a recipe with no evidence returns -1");
        CHECK(nvn_vk_pipe_worker_wait(NULL, gen, 0, &wres) == -1, "null recipe is refused");

        // 在途超时：job 被 fake 卡住时 wait 到点返回 0，不取别人的结果。
        set_blocking(1);
        make_spec(&spec, gen, 41);
        CHECK(nvn_vk_pipe_worker_submit(&spec, &job_id, why, sizeof why) == 0, "wait-target job accepted");
        CHECK(wait_flag(&g_create_entered, 3000), "wait-target create entered");
        CHECK(nvn_vk_pipe_worker_wait(&spec.recipe, gen, 60, &wres) == 0,
              "in-flight recipe wait times out with 0");
        // 别的配方（无证据）即使在同一 worker 下也 -1。
        struct nvk_pipe_job_spec other;
        make_spec(&other, gen, 42);
        CHECK(nvn_vk_pipe_worker_wait(&other.recipe, gen, 0, &wres) == -1,
              "a different recipe still has no evidence");
        release_block();
        pthread_mutex_lock(&g_fmu);
        g_block_create = 0;
        pthread_mutex_unlock(&g_fmu);
        CHECK(nvn_vk_pipe_worker_wait(&spec.recipe, gen, 3000, &wres) == 1,
              "completed recipe is taken by its own wait");
        CHECK(wres.ok == 1 && wres.pipeline != VK_NULL_HANDLE &&
              nvk_pipe_recipe_match(&wres.recipe, &spec.recipe),
              "wait result carries the matching recipe");
        nvn_vk_pipe_worker_release(&wres);

        // 环内抽取：两条不同配方完成后，wait 第二条只取走它（FIFO 里另一条留下）。
        make_spec(&spec, gen, 43);
        struct nvk_pipe_job_spec spec2;
        make_spec(&spec2, gen, 44);
        CHECK(nvn_vk_pipe_worker_submit(&spec, &job_id, why, sizeof why) == 0, "ring job A accepted");
        CHECK(nvn_vk_pipe_worker_submit(&spec2, &job_id, why, sizeof why) == 0, "ring job B accepted");
        CHECK(nvn_vk_pipe_worker_wait(&spec2.recipe, gen, 3000, &wres) == 1,
              "wait extracts the matching result out of the ring");
        CHECK(nvk_pipe_recipe_match(&wres.recipe, &spec2.recipe), "extracted result is recipe B");
        nvn_vk_pipe_worker_release(&wres);
        CHECK(take_one(&res, 3000) && nvk_pipe_recipe_match(&res.recipe, &spec.recipe),
              "the other result stays in FIFO order for take()");
        nvn_vk_pipe_worker_release(&res);

        // 代际过滤：环里只有旧代际结果时，本代际 wait 看不到它（-1，不误取）。
        nvn_vk_pipe_worker_set_generation(gen + 1);
        make_spec(&spec, gen + 1, 45);
        CHECK(nvn_vk_pipe_worker_submit(&spec, &job_id, why, sizeof why) == 0, "next-gen job accepted");
        CHECK(nvn_vk_pipe_worker_wait(&spec.recipe, gen, 3000, &wres) == -1,
              "old-generation wait does not match a next-generation result");
        CHECK(nvn_vk_pipe_worker_wait(&spec.recipe, gen + 1, 3000, &wres) == 1,
              "same-generation wait takes the result");
        nvn_vk_pipe_worker_release(&wres);

        // 失败结果也按配方转交（ok=0 带 why），不让 owner 把失败当在途。
        pthread_mutex_lock(&g_fmu);
        g_fail_next = 1;
        pthread_mutex_unlock(&g_fmu);
        make_spec(&spec, gen + 1, 46);
        CHECK(nvn_vk_pipe_worker_submit(&spec, &job_id, why, sizeof why) == 0, "failing wait job accepted");
        CHECK(nvn_vk_pipe_worker_wait(&spec.recipe, gen + 1, 3000, &wres) == 1 &&
              wres.ok == 0 && wres.why[0],
              "a failed create reaches the recipe wait with its reason");
        nvn_vk_pipe_worker_release(&wres);
        nvn_vk_pipe_worker_set_generation(gen);
    }

    // 6) 有界 Stop：创建中返回 1；迟到结果销毁；未退出前拒绝重启；退出后回收并可再启动。
    set_blocking(1);
    pthread_mutex_lock(&g_fmu);
    g_create_entered = 0;
    pthread_mutex_unlock(&g_fmu);
    make_spec(&spec, gen, 13);
    CHECK(nvn_vk_pipe_worker_submit(&spec, &job_id, why, sizeof why) == 0, "job accepted before stop");
    CHECK(wait_flag(&g_create_entered, 3000), "create in flight before stop");
    int dropped_before = get_int(&g_pipeline_destroyed);
    CHECK(nvn_vk_pipe_worker_stop(30, why, sizeof why) == 1, "bounded stop reports an in-flight create");
    CHECK(strstr(why, "still creating") != NULL, "bounded stop explains why it did not join");
    CHECK(nvn_vk_pipe_worker_active() == 0, "stop refuses new demand immediately");
    CHECK(nvn_vk_pipe_worker_start(dev, cache, gen, why, sizeof why) == -1,
          "restart is refused until the in-flight create has exited");
    release_block();
    pthread_mutex_lock(&g_fmu);
    g_block_create = 0;
    pthread_mutex_unlock(&g_fmu);
    CHECK(nvn_vk_pipe_worker_stop(3000, why, sizeof why) == 0, "a later bounded stop reclaims the exited worker");
    CHECK(get_int(&g_pipeline_destroyed) > dropped_before, "late pipeline is destroyed, not handed out");
    CHECK(nvn_vk_pipe_worker_take(&res) == 0, "no late result is delivered after stop");
    CHECK(nvn_vk_pipe_worker_start(dev, cache, gen, why, sizeof why) == 0, "worker restarts after reclamation");

    CHECK(nvn_vk_pipe_worker_stop(1000, why, sizeof why) == 0, "idle stop completes");
    nvn_vk_pipe_worker_stats(&st);
    CHECK(st.created >= 7, "created count reflects actual successful driver creates");
    CHECK(st.dropped >= 2, "dropped count reflects actual destroyed pipelines");

    if (fails) { printf("pipe worker: %d FAILURES\n", fails); return 1; }
    printf("pipe worker: ok (worker-side create, handoff, dedupe, capacity, generation, bounded stop, cache lock)\n");
    return 0;
}
