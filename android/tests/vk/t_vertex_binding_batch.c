// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define main feedback_main_unused
#include "t_feedback.c"
#undef main

static int batch = 1;
static unsigned binds[5];
static VkBuffer original[2], bound[16];
static VkDeviceSize offsets[16];
static VkBuffer *capture;
static PFN_vkCmdBindVertexBuffers real_bind;
static _Atomic unsigned retired_buffers;

int __real_nvn_plat_flag(const char *, int);
int __wrap_nvn_plat_flag(const char *name, int fallback) {
    if (!strcmp(name, "nvk_replay_cpu_cuts")) return batch == 1;
    return __real_nvn_plat_flag(name, fallback);
}

extern __typeof__(vkCreateBuffer) __real_vkCreateBuffer;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
                                                    const VkAllocationCallbacks *alloc, VkBuffer *buffer) {
    VkResult rc = __real_vkCreateBuffer(device, info, alloc, buffer);
    if (capture && rc == VK_SUCCESS) *capture = *buffer;
    return rc;
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateBuffer), PFN_vkCreateBuffer),
               "vkCreateBuffer signature");

static VKAPI_ATTR void VKAPI_CALL observe_bind(VkCommandBuffer cmd, uint32_t first, uint32_t count,
                                              const VkBuffer *buffers, const VkDeviceSize *offset) {
    REQUIRE(observed_draws < 5 && count && first < 16 && count <= 16 - first);
    binds[observed_draws]++;
    for (uint32_t i = 0; i < count; i++) { bound[first + i] = buffers[i]; offsets[first + i] = offset[i]; }
    real_bind(cmd, first, count, buffers, offset);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&observe_bind), PFN_vkCmdBindVertexBuffers),
               "vkCmdBindVertexBuffers signature");

static PFN_vkVoidFunction observe_command(const char *name) {
    return !strcmp(name, "vkCmdBindVertexBuffers") ? (PFN_vkVoidFunction)observe_bind : NULL;
}
static unsigned no_sample_sets(void) { return 0; }
static void retired(void *user) { (void)user; atomic_fetch_add(&retired_buffers, 1); }

static void check_draw(void) {
    for (unsigned slot = 0; slot < 16; slot++) {
        VkBuffer want = VK_NULL_HANDLE;
        VkDeviceSize off = slot * 4;
        if (slot == 0 || slot == 1 || slot == 3 || slot == 4 || slot == 15) want = original[0];
        if (observed_draws >= 2 && (slot == 0 || slot == 3 || slot == 4 || slot == 14)) {
            want = original[1]; off += 32;
        }
        if (observed_draws == 4) { want = original[1]; off = 64 + slot * 4; }
        REQUIRE(bound[slot] == want);
        if (want) REQUIRE(offsets[slot] == off);
    }
}

int main(int argc, char **argv) {
    REQUIRE(argc == 4);
    dq_mode = atoi(argv[2]);
    batch = atoi(argv[3]);
    struct s2a_vk_device device;
    char why[256];
    REQUIRE(!s2a_vk_device_create(&device, why, sizeof why));
    real_bind = (PFN_vkCmdBindVertexBuffers)__real_vkGetDeviceProcAddr(device.device, "vkCmdBindVertexBuffers");
    REQUIRE(real_bind);
    observer_reset();
    observed_command = observe_command;
    observed_sample_sets = no_sample_sets;
    observed_on_draw = check_draw;
    nvn_gpu_open_desc open = {.host_device = &device, .app_label = "vertex-binding-batch"};
    GPU(nvn_gpu_open(&open, &err));
    nvn_gpu_shader vs = shader(argv[1], "fullscreen.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = shader(argv[1], "solid_green.frag", NVN_GPU_SH_FS, NULL);
    nvn_gpu_pipe_desc pd = {0};
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.poly.mode = NVN_POLYGON_MODE_FILL;
    pd.color.logic_op = 3;
    pd.prim = 4;
    pd.samples = 1;
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    nvn_gpu_pipe pipe = NULL;
    GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));
    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 4;
    td.d.depth = td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER;
    nvn_gpu_tex tex;
    nvn_gpu_view view;
    GPU(nvn_gpu_tex_create(&td, &tex, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &view, &err));
    nvn_gpu_buf vb[2];
    for (unsigned i = 0; i < 2; i++) {
        capture = &original[i];
        GPU(nvn_gpu_buf_create(256, NVN_GPU_MEM_SHARED, NULL, &vb[i], &err));
        REQUIRE(original[i]);
    }
    capture = NULL;
    REQUIRE(original[0] != original[1]);
    nvn_gpu_cmd cmd;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    begin_pass(cmd, view, NVN_GPU_LOAD_CLEAR, 4);
    nvn_gpu_set_pipe(cmd, pipe);
    nvn_gpu_dyn dyn = {.nvp = 1, .has_sc = 1, .sc = {0, 0, 4, 4}};
    dyn.vp[0][2] = dyn.vp[0][3] = 4;
    dyn.vp[0][5] = 1;
    nvn_gpu_set_dyn(cmd, &dyn);
    const nvn_gpu_draw_args draw = {.prim = 4, .count = 3, .instances = 1};
    const unsigned slots[] = {0, 1, 3, 4, 15};
    for (unsigned i = 0; i < sizeof slots / sizeof *slots; i++) nvn_gpu_set_vbuf(cmd, slots[i], vb[0], slots[i] * 4);
    nvn_gpu_draw(cmd, &draw);
    nvn_gpu_draw(cmd, &draw);   // No changes.
    const unsigned changed[] = {0, 3, 4, 14};
    for (unsigned i = 0; i < sizeof changed / sizeof *changed; i++) nvn_gpu_set_vbuf(cmd, changed[i], vb[1], 32 + changed[i] * 4);
    nvn_gpu_set_vbuf(cmd, 15, NULL, 0);
    nvn_gpu_draw(cmd, &draw);   // Slot 1 and the empty gap still separate the ranges.
    nvn_gpu_set_vbuf(cmd, 15, vb[0], 60);
    nvn_gpu_draw(cmd, &draw);   // NULL had not overwritten the actual Vulkan binding.
    for (unsigned slot = 0; slot < 16; slot++) nvn_gpu_set_vbuf(cmd, slot, vb[1], 64 + slot * 4);
    nvn_gpu_draw(cmd, &draw);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_buf_release(vb[0], retired, NULL);
    nvn_gpu_buf_release(vb[1], retired, NULL);
    REQUIRE(atomic_load(&retired_buffers) == 0);
    GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    REQUIRE(observed_draws == 5);
    printf("vertex binding batch=%d dq=%d calls=%u/%u/%u/%u/%u draw-state=ok\n",
           batch, dq_mode, binds[0], binds[1], binds[2], binds[3], binds[4]);
    const unsigned want_fast[] = {3, 0, 3, 0, 1}, want_legacy[] = {5, 0, 4, 0, 16};
    REQUIRE(!memcmp(binds, batch ? want_fast : want_legacy, sizeof binds));
    nvn_gpu_view_release(view);
    nvn_gpu_tex_release(tex);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
    GPU(nvn_gpu_close(5000000000ull, &err));
    REQUIRE(atomic_load(&retired_buffers) == 2);
    REQUIRE(!s2a_vk_device_destroy(&device, 1));
    return 0;
}
