// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define main feedback_main_unused
#define __wrap_vkWaitForFences observed_wait_for_fences
#include "t_feedback.c"
#undef main
#undef __wrap_vkWaitForFences

static unsigned create_calls;
static _Atomic unsigned destroy_calls;
static VkPipeline newest;
static _Atomic(VkPipeline) last_destroyed;
static pthread_mutex_t fence_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fence_cv = PTHREAD_COND_INITIALIZER;
static int hold_fence;

static unsigned inject_count;
static VkResult inject_result;
static int limit;

extern __typeof__(vkCreateGraphicsPipelines) __real_vkCreateGraphicsPipelines;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache,
    uint32_t count, const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *alloc,
    VkPipeline *pipes) {
    REQUIRE(count == 1);
    create_calls++;
    if (inject_count) {
        inject_count--;
        *pipes = VK_NULL_HANDLE;
        return inject_result;
    }
    VkResult result = __real_vkCreateGraphicsPipelines(device, cache, count, infos, alloc, pipes);
    if (result == VK_SUCCESS) newest = pipes[0];
    return result;
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateGraphicsPipelines),
                                            PFN_vkCreateGraphicsPipelines), "vkCreateGraphicsPipelines signature");

extern __typeof__(vkDestroyPipeline) __real_vkDestroyPipeline;
VKAPI_ATTR void VKAPI_CALL __wrap_vkDestroyPipeline(VkDevice device, VkPipeline pipeline,
                                                     const VkAllocationCallbacks *alloc) {
    atomic_store(&last_destroyed, pipeline);
    atomic_fetch_add(&destroy_calls, 1);
    __real_vkDestroyPipeline(device, pipeline, alloc);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkDestroyPipeline),
                                            PFN_vkDestroyPipeline), "vkDestroyPipeline signature");

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkWaitForFences(VkDevice device, uint32_t count,
    const VkFence *fences, VkBool32 all, uint64_t timeout) {
    pthread_mutex_lock(&fence_mu);
    while (hold_fence) pthread_cond_wait(&fence_cv, &fence_mu);
    pthread_mutex_unlock(&fence_mu);
    return observed_wait_for_fences(device, count, fences, all, timeout);
}

int __real_nvn_plat_flag(const char *, int);
int __wrap_nvn_plat_flag(const char *name, int fallback) {
    if (!strcmp(name, "nvk_pipe_limit")) return limit;
    return __real_nvn_plat_flag(name, fallback);
}

static unsigned no_sample_sets(void) { return 0; }

static nvn_gpu_pipe make_pipe(nvn_gpu_pipe_desc desc, unsigned flags) {
    // 只改变颜色写掩码，四条管线使用同一附件和着色器布局。
    desc.chmask.mask = flags;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&desc, NVN_GPU_PIPE_SYNC, &pipe, &err));
    REQUIRE(pipe);
    return pipe;
}

static nvn_gpu_pipe dynamic_alias(nvn_gpu_pipe_desc desc, unsigned flags, nvn_gpu_pipe owner) {
    const unsigned calls = create_calls;
    desc.poly.offset_en = 1u << NVN_POLYGON_MODE_FILL;
    nvn_gpu_pipe alias = make_pipe(desc, flags);
    REQUIRE(alias != owner && create_calls == calls);
    return alias;
}

static void draw_quad(nvn_gpu_cmd cmd, nvn_gpu_pipe pipe) {
    nvn_gpu_dyn dyn = {.nvp = 1, .has_sc = 1, .sc = {0, 0, 4, 4}};
    dyn.vp[0][2] = dyn.vp[0][3] = 4;
    dyn.vp[0][5] = 1;
    nvn_gpu_set_dyn(cmd, &dyn);
    nvn_gpu_set_pipe(cmd, pipe);
    const nvn_gpu_draw_args draw = {.prim = 4, .count = 3, .instances = 1};
    nvn_gpu_draw(cmd, &draw);
}

int main(int argc, char **argv) {
    REQUIRE(argc == 4 || argc == 5);
    const int dynamic = argc == 5;
    dq_mode = atoi(argv[2]);
    limit = atoi(argv[3]);
    observer_reset();
    observed_sample_sets = no_sample_sets;
    struct s2a_vk_device device;
    char why[256];
    REQUIRE(!s2a_vk_device_create(&device, why, sizeof why));
    if (dynamic) REQUIRE(device.eds2_enabled);
    const nvn_gpu_open_desc open = {.host_device = &device, .app_label = "graphics-pipe-budget"};
    GPU(nvn_gpu_open(&open, &err));
    nvn_gpu_pipe_desc desc = {.cfmt = {NVN_FORMAT_RGBA8}, .prim = 4, .samples = 1};
    desc.poly.mode = NVN_POLYGON_MODE_FILL;
    desc.color.logic_op = 3;
    desc.st[NVN_GPU_SH_VS].sh = shader(argv[1], "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    desc.st[NVN_GPU_SH_FS].sh = shader(argv[1], "solid_green.frag", NVN_GPU_SH_FS, NULL);
    nvn_gpu_pipe first = make_pipe(desc, 15);
    const VkPipeline pinned = newest;
    if (dynamic) first = dynamic_alias(desc, 15, first);
    const unsigned base_destroys = destroy_calls;
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 4;
    td.d.depth = td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER;
    nvn_gpu_tex tex;
    nvn_gpu_view view;
    nvn_gpu_buf rb;
    GPU(nvn_gpu_tex_create(&td, &tex, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &view, &err));
    GPU(nvn_gpu_buf_create(64, NVN_GPU_MEM_SHARED, NULL, &rb, &err));

    nvn_gpu_cmd cmd;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    begin_pass(cmd, view, NVN_GPU_LOAD_CLEAR, 4);
    draw_quad(cmd, first);

    desc.chmask.mask = 3;
    nvn_gpu_pipe denied = NULL;
    const unsigned before = create_calls;
    inject_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    inject_count = 1;
    REQUIRE(nvn_gpu_pipe_get(&desc, NVN_GPU_PIPE_SYNC, &denied, &err).code == NVN_GPU_E_OOM);
    REQUIRE(!denied && create_calls == before + 1 && destroy_calls == base_destroys);

    nvn_gpu_pipe old = make_pipe(desc, 7);
    const VkPipeline oldest = newest;
    if (dynamic) old = dynamic_alias(desc, 7, old);
    make_pipe(desc, 6);
    printf("budget before OOM: cap=%d expected evictions=1 actual=%u\n", limit, destroy_calls - base_destroys);
    REQUIRE(destroy_calls == base_destroys + 1 && last_destroyed == oldest && last_destroyed != pinned);

    inject_result = dq_mode ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_OUT_OF_HOST_MEMORY;
    inject_count = 1;
    const unsigned retry_before = create_calls;
    make_pipe(desc, 5);
    REQUIRE(create_calls == retry_before + 2 && destroy_calls == base_destroys + 2 && last_destroyed != pinned);

    desc.chmask.mask = 4;
    unsigned attempts = create_calls;
    unsigned evictions = atomic_load(&destroy_calls);
    inject_result = VK_ERROR_INITIALIZATION_FAILED;
    inject_count = 1;
    REQUIRE(nvn_gpu_pipe_get(&desc, NVN_GPU_PIPE_SYNC, &denied, &err).code == NVN_GPU_E_COMPILE);
    REQUIRE(!denied && create_calls == attempts + 1 && destroy_calls == evictions);
    attempts = create_calls;
    inject_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    inject_count = 2;
    REQUIRE(nvn_gpu_pipe_get(&desc, NVN_GPU_PIPE_SYNC, &denied, &err).code == NVN_GPU_E_OOM);
    REQUIRE(!denied && create_calls == attempts + 2 && destroy_calls == evictions + 1 && last_destroyed != pinned);

    // 底层对象淘汰后，旧句柄仍能在正常绘制入口重新取得对象。
    attempts = create_calls;
    draw_quad(cmd, old);
    REQUIRE(create_calls == attempts + 1);
    draw_quad(cmd, first);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, NULL, rb, 0, 16, 64);
    pthread_mutex_lock(&fence_mu);
    hold_fence = 1;
    pthread_mutex_unlock(&fence_mu);
    GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
    const unsigned while_pending = atomic_load(&destroy_calls);
    make_pipe(desc, 8);
    REQUIRE(atomic_load(&destroy_calls) == while_pending);
    pthread_mutex_lock(&fence_mu);
    hold_fence = 0;
    pthread_cond_broadcast(&fence_cv);
    pthread_mutex_unlock(&fence_mu);
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    // 时间值先发布，完成线程随后归还本段引用并收回超额的旧管线。
    for (unsigned try = 0; atomic_load(&destroy_calls) == while_pending && try < 500; try++) {
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    REQUIRE(atomic_load(&destroy_calls) == while_pending + 1);
    const unsigned char *bytes = nvn_gpu_buf_cpu(rb);
    for (unsigned i = 0; i < 16; i++) {
        REQUIRE(bytes[i * 4] == 0 && bytes[i * 4 + 1] == 255
                && bytes[i * 4 + 2] == 0 && bytes[i * 4 + 3] == 255);
    }
    printf("graphics pipe budget: pixels=16/16 no-victim=ok oom-retry=ok in-flight=protected dynamic-alias=%d\n", dynamic);
    nvn_gpu_view_release(view);
    nvn_gpu_tex_release(tex);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(desc.st[NVN_GPU_SH_VS].sh);
    nvn_gpu_shader_release(desc.st[NVN_GPU_SH_FS].sh);
    GPU(nvn_gpu_close(5000000000ull, &err));
    REQUIRE(!s2a_vk_device_destroy(&device, 1));
    return 0;
}
