// SPDX-License-Identifier: Apache-2.0
// 声明与登记目标相同时，用真实回放资源、Vulkan 视图和采样读回检查立方体的范围。
#define main backend_fixture_main
#define nvn_plat_flag backend_fixture_plat_flag
#define vkQueueSubmit backend_fixture_queue_submit
#define vkDestroyImageView backend_fixture_destroy_image_view
#include "t_backend_vk.c"
#undef vkDestroyImageView
#undef vkQueueSubmit
#undef nvn_plat_flag
#undef main
#include "rp_exec.h"
#include "rp_res.h"
#include "nvn_tex.h"
#include "nvn_render_audit.h"

int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_render_audit") && getenv("S2A_TEST_RENDER_AUDIT")) return 1;
    return backend_fixture_plat_flag(name, dflt);
}

int rp_tm_on;
static nvn_gpu_cmd active;
void rp_perf_add(int index, uint64_t value) { (void)index; (void)value; }
uint64_t rp_seg_serial(void) { return 1; }
uint64_t rp_sub_serial(void) { return 1; }
int rp_buf_gpu_pending(uint64_t addr, uint64_t size) { (void)addr; (void)size; return 0; }
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last_sub) {
    *last_sub = rp_sub_serial();
    return active;
}

static VkImageViewType seen_type;
static uint32_t seen_base, seen_count;
static VkImageView seen_view;

static _Atomic(VkImageView) guarded_view;
static _Atomic(VkFramebuffer) guarded_fb;
static atomic_uint early_views, early_fbs;
static atomic_int layer_change_pending, retired_views, retired_fbs;
static int watch_framebuffer;
static VkFramebuffer seen_framebuffer;
static VkSemaphore submit_gate;
static VkFence gated_fence;

static int fail_next_view, failed_views, fail_next_sampler, failed_samplers;
static int watch_descriptor_writes, invalid_descriptor_writes, dispatches;

VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateImageView(
    VkDevice device, const VkImageViewCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImageView *view);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateImageView(
    VkDevice device, const VkImageViewCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImageView *view) {
    if (fail_next_view) {
        fail_next_view = 0;
        failed_views++;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    const VkResult rc = __real_vkCreateImageView(device, info, allocator, view);
    if (rc == VK_SUCCESS) {
        seen_view = *view;
        seen_type = info->viewType;
        seen_base = info->subresourceRange.baseArrayLayer;
        seen_count = info->subresourceRange.layerCount;
    }
    return rc;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks *alloc) {
    if (view && view == atomic_load(&guarded_view)) {
        if (atomic_load(&layer_change_pending)) { atomic_fetch_add(&early_views, 1); return; }
        atomic_fetch_add(&retired_views, 1);
    }
    backend_fixture_destroy_image_view(device, view, alloc);
}

extern __typeof__(vkCreateFramebuffer) __real_vkCreateFramebuffer;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *info,
                                                          const VkAllocationCallbacks *alloc, VkFramebuffer *fb) {
    const VkResult result = __real_vkCreateFramebuffer(device, info, alloc, fb);
    if (watch_framebuffer && result == VK_SUCCESS && info->layers == 2) seen_framebuffer = *fb;
    return result;
}

extern __typeof__(vkDestroyFramebuffer) __real_vkDestroyFramebuffer;
VKAPI_ATTR void VKAPI_CALL __wrap_vkDestroyFramebuffer(VkDevice device, VkFramebuffer fb, const VkAllocationCallbacks *alloc) {
    if (fb && fb == atomic_load(&guarded_fb)) {
        if (atomic_load(&layer_change_pending)) { atomic_fetch_add(&early_fbs, 1); return; }
        atomic_fetch_add(&retired_fbs, 1);
    }
    __real_vkDestroyFramebuffer(device, fb, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *info, VkFence fence) {
    if (!submit_gate) return backend_fixture_queue_submit(queue, count, info, fence);
    const VkSemaphore gate = submit_gate;
    submit_gate = VK_NULL_HANDLE;
    if (count != 1 || info->pNext || info->waitSemaphoreCount || info->signalSemaphoreCount) {
        CHECK(0, "layer test needs one plain Vulkan submit");
        return backend_fixture_queue_submit(queue, count, info, fence);
    }
    const uint64_t one = 1;
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkTimelineSemaphoreSubmitInfo timeline = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &one,
    };
    VkSubmitInfo delayed = *info;
    delayed.pNext = &timeline;
    delayed.waitSemaphoreCount = 1;
    delayed.pWaitSemaphores = &gate;
    delayed.pWaitDstStageMask = &stage;
    gated_fence = fence;
    return backend_fixture_queue_submit(queue, 1, &delayed, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateSampler(
    VkDevice device, const VkSamplerCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkSampler *sampler);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateSampler(
    VkDevice device, const VkSamplerCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkSampler *sampler) {
    if (fail_next_sampler) {
        fail_next_sampler = 0;
        failed_samplers++;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return __real_vkCreateSampler(device, info, allocator, sampler);
}

VKAPI_ATTR void VKAPI_CALL __real_vkUpdateDescriptorSets(
    VkDevice device, uint32_t writes, const VkWriteDescriptorSet *write,
    uint32_t copies, const VkCopyDescriptorSet *copy);
VKAPI_ATTR void VKAPI_CALL __wrap_vkUpdateDescriptorSets(
    VkDevice device, uint32_t writes, const VkWriteDescriptorSet *write,
    uint32_t copies, const VkCopyDescriptorSet *copy) {
    if (watch_descriptor_writes) {
        const int before = invalid_descriptor_writes;
        for (uint32_t i = 0; i < writes; i++) {
            for (uint32_t j = 0; j < write[i].descriptorCount; j++) {
                switch (write[i].descriptorType) {
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    if (!write[i].pImageInfo[j].sampler) invalid_descriptor_writes++;
                    /* fall through */
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                    if (!write[i].pImageInfo[j].imageView) invalid_descriptor_writes++;
                    break;
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                    if (!write[i].pImageInfo[j].sampler) invalid_descriptor_writes++;
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                    if (!write[i].pBufferInfo[j].buffer) invalid_descriptor_writes++;
                    break;
                default:
                    break;
                }
            }
        }
        // Baseline probes the call boundary and stops before delivering an invalid write to Vulkan.
        if (invalid_descriptor_writes != before) return;
    }
    __real_vkUpdateDescriptorSets(device, writes, write, copies, copy);
}

VKAPI_ATTR void VKAPI_CALL __real_vkCmdDispatch(
    VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z);
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdDispatch(
    VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) {
    if (watch_descriptor_writes) {
        dispatches++;
        if (invalid_descriptor_writes) return;
    }
    __real_vkCmdDispatch(cmd, x, y, z);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __real_vkGetDeviceProcAddr(VkDevice device, const char *name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice device, const char *name) {
    if (!strcmp(name, "vkCmdDispatch")) return (PFN_vkVoidFunction)__wrap_vkCmdDispatch;
    return __real_vkGetDeviceProcAddr(device, name);
}

static uint64_t fixture_signal;
static void finish(nvn_gpu_cmd cmd) {
    const uint64_t signal = ++fixture_signal;
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_cmd_commit(cmd, signal, on_done, NULL, &err)), "submit: %s", err.why);
    CHECK(OK(nvn_gpu_fence_wait(signal, 5000000000ull, &err)), "wait: %s", err.why);
}

static void layered_clear(nvn_gpu_cmd cmd, nvn_gpu_view view, uint32_t layers, unsigned channel) {
    nvn_gpu_pass_desc pass = {.ncolor = 1, .w = 1, .h = 1, .layers = layers};
    pass.color[0].view = view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[channel] = pass.color[0].clear.f[3] = 1.f;
    nvn_gpu_pass_begin(cmd, &pass);
    nvn_gpu_pass_end(cmd);
}

static void check_layered_attachment_retirement(int inflight) {
    nvn_gpu_tex tex = make_tex_target(1, 1, 4, NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view view = make_view(tex);
    nvn_gpu_cmd first = begin_cmd(), second = begin_cmd();
    nvn_gpu_buf read = make_buf(6 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_err err = {0};
    const uint64_t first_signal = fixture_signal + 1;
    fixture_signal += 2;
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(OK(nvn_gpu_tex_upload(first, tex, 0, 3, NULL, green, 4, 4)), "initialize untouched fourth layer");
    seen_framebuffer = VK_NULL_HANDLE;
    watch_framebuffer = 1;
    layered_clear(first, view, 2, 0);
    watch_framebuffer = 0;
    VkImageView original = seen_view;
    VkFramebuffer framebuffer = seen_framebuffer;
    CHECK(original && framebuffer && seen_count == 2, "original two-layer view and framebuffer were created");
    for (uint32_t layer = 0; layer < 2; layer++) nvn_gpu_copy_tex_buf(first, tex, 0, layer, NULL, read, layer * 4, 4, 4);

    VkSemaphore gate = VK_NULL_HANDLE;
    if (inflight) {
        CHECK(g_dev.timeline_semaphore_enabled, "live submission test needs timeline semaphores");
        const VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        const VkSemaphoreCreateInfo create = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
        CHECK(vkCreateSemaphore(g_dev.device, &create, NULL, &gate) == VK_SUCCESS, "create live submission gate");
        submit_gate = gate;
        gated_fence = VK_NULL_HANDLE;
        CHECK(OK(nvn_gpu_cmd_commit(first, first_signal, on_done, NULL, &err)), "submit gated pass: %s", err.why);
        CHECK(gated_fence && vkGetFenceStatus(g_dev.device, gated_fence) == VK_NOT_READY, "first pass is pending on the real GPU queue");
    }

    atomic_store(&early_views, 0);
    atomic_store(&early_fbs, 0);
    atomic_store(&retired_views, 0);
    atomic_store(&retired_fbs, 0);
    atomic_store(&layer_change_pending, 1);
    atomic_store(&guarded_view, original);
    atomic_store(&guarded_fb, framebuffer);
    layered_clear(second, view, 3, 2);
    atomic_store(&layer_change_pending, 0);
    for (uint32_t layer = 0; layer < 4; layer++)
        nvn_gpu_copy_tex_buf(second, tex, 0, layer, NULL, read, (layer + 2) * 4, 4, 4);
    const unsigned view_attempts = atomic_load(&early_views), fb_attempts = atomic_load(&early_fbs);
    CHECK(!view_attempts && !fb_attempts, "%s layer change tried to destroy live view=%u framebuffer=%u",
          inflight ? "in-flight" : "recorded", view_attempts, fb_attempts);

    if (inflight && gate) {
        PFN_vkSignalSemaphoreKHR signal = (PFN_vkSignalSemaphoreKHR)vkGetDeviceProcAddr(g_dev.device, "vkSignalSemaphoreKHR");
        const VkSemaphoreSignalInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = gate, .value = 1};
        CHECK(signal && signal(g_dev.device, &info) == VK_SUCCESS, "unblock live submission");
    } else if (!inflight) {
        CHECK(OK(nvn_gpu_cmd_commit(first, first_signal, on_done, NULL, &err)), "submit recorded pre-change pass: %s", err.why);
    }
    CHECK(OK(nvn_gpu_cmd_commit(second, first_signal + 1, on_done, NULL, &err)), "submit post-change pass: %s", err.why);
    CHECK(OK(nvn_gpu_fence_wait(first_signal + 1, 5000000000ull, &err)), "retire both layered passes: %s", err.why);
    const uint8_t expected[24] = {
        255, 0, 0, 255, 255, 0, 0, 255,
        0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255,
        0, 255, 0, 255,
    };
    const void *actual = buf_cpu(read);
    CHECK(actual && !memcmp(actual, expected, sizeof expected), "both layer extents and untouched layer read back independently");
    if (!view_attempts && !fb_attempts) {
        CHECK(wait_until(&retired_views, 1, 5000) && atomic_load(&retired_views) == 1 && atomic_load(&retired_fbs) == 1,
              "old layered view and cached framebuffer each retired once before releasing the logical view");
    }
    atomic_store(&guarded_view, VK_NULL_HANDLE);
    atomic_store(&guarded_fb, VK_NULL_HANDLE);
    // On a failing baseline the spies withheld the invalid destroys. Free the
    // resulting test-owned handles only after the real queue has retired them.
    if (fb_attempts) __real_vkDestroyFramebuffer(g_dev.device, framebuffer, NULL);
    if (view_attempts) backend_fixture_destroy_image_view(g_dev.device, original, NULL);
    if (gate) vkDestroySemaphore(g_dev.device, gate, NULL);
    nvn_gpu_buf_release(read, NULL, NULL);
    nvn_gpu_view_release(view);
    nvn_gpu_tex_release(tex);
    printf("layer lifetime %s early_views=%u early_framebuffers=%u\n", inflight ? "in-flight" : "recorded", view_attempts, fb_attempts);
}

static void sample(const char *dir, const char *frag, nvn_gpu_view sv, uint8_t pixels[64 * 64 * 4]) {
    const nvn_gpu_layout_ent ents[] = {
        {.binding = 0, .cls = NVN_GPU_RES_TEX, .access = NVN_GPU_ACC_R, .count = 1, .stages = 1u << NVN_GPU_SH_FS},
        {.binding = 1, .cls = NVN_GPU_RES_SAMP, .access = NVN_GPU_ACC_R, .count = 1, .stages = 1u << NVN_GPU_SH_FS},
    };
    const nvn_gpu_layout fl = {.ent = ents, .nent = 2};
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vsm = NULL, *fsm = NULL;
    CHECK(load_shader_pair(dir, "tri.vert", frag, NULL, &fl, &vs, &fs, &vsm, &fsm), "%s shaders", frag);
    nvn_gpu_pipe_desc pd = {
        .poly.mode = NVN_POLYGON_MODE_FILL, .origin_ll = 1, .color.logic_op = NVN_LOGIC_OP_COPY,
        .cfmt[0] = NVN_FORMAT_RGBA8, .samples = 1, .prim = 4, .chmask.mask = 15,
        .attr = {{.fmt = NVN_FORMAT_RG32F}, {.fmt = NVN_FORMAT_RG32F, .offset = 8}},
        .stream[0].stride = 16,
    };
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    nvn_gpu_err err = {0};
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err)), "%s pipeline: %s", frag, err.why);
    const samp_par sp = {0};
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &err)), "%s sampler: %s", frag, err.why);
    const float verts[3][4] = {{-1, -1, 0, 0}, {3, -1, 2, 0}, {-1, 3, 0, 2}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_pass_desc pass = {.ncolor = 1, .w = 64, .h = 64, .layers = 1};
    pass.color[0].view = ov;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[3] = 1;
    nvn_gpu_pass_begin(active, &pass);
    nvn_gpu_set_pipe(active, pipe);
    const nvn_gpu_dyn dyn = {.origin_ll = 1, .nvp = 1, .vp = {{0, 0, 64, 64, 0, 1}},
                             .has_sc = 1, .sc = {0, 0, 64, 64}};
    nvn_gpu_set_dyn(active, &dyn);
    nvn_gpu_set_vbuf(active, 0, vb, 0);
    const nvn_gpu_binding binds[] = {
        {.ent = 0, .cls = NVN_GPU_RES_TEX, .u.view = sv},
        {.ent = 1, .cls = NVN_GPU_RES_SAMP, .u.samp = samp},
    };
    nvn_gpu_bind(active, NVN_GPU_SH_FS, binds, 2);
    const nvn_gpu_draw_args draw = {.prim = 4, .count = 3, .instances = 1};
    nvn_gpu_draw(active, &draw);
    nvn_gpu_pass_end(active);
    nvn_gpu_copy_tex_buf(active, out, 0, 0, NULL, rb, 0, 256, 16384);
    finish(active);
    active = NULL;
    memcpy(pixels, buf_cpu(rb), 64 * 64 * 4);
    nvn_gpu_view_release(ov);
    nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_samp_release(samp);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
    free(vsm);
    free(fsm);
}

static uint8_t colors[12][4];
static void register_storage(tex_t *t, int target, unsigned layers, uint64_t serial) {
    *t = (tex_t){.d = {.target = target, .format = NVN_FORMAT_RGBA8,
                      .width = 1, .height = 1, .depth = layers, .levels = 1}, .serial = serial};
    nvn_tex_layout(&t->d, &t->L);
    active = begin_cmd();
    nvn_gpu_tex src = rp_tex_resolve(t, active);
    CHECK(src != NULL, "cube storage target=%d layers=%u", target, layers);
    if (src) {
        for (unsigned i = 0; i < layers; i++)
            nvn_gpu_tex_upload(active, src, 0, i, NULL, colors[i], 4, 4);
    }
    finish(active);
    active = NULL;
}

static void check_sample(const char *dir, const char *label, int id, int want, const char *frag,
                         VkImageViewType type, unsigned base, unsigned count, int first_color) {
    active = begin_cmd();
    seen_count = 0;
    nvn_gpu_view view = rp_view_by_id_decl(id, want + 1, 0, 0, active, NULL);
    const int shape_ok = view != NULL && seen_type == type && seen_base == base && seen_count == count;
    CHECK(shape_ok, "%s binding: expected Vk view %d %u+%u, got %d %u+%u",
          label, type, base, count, seen_type, seen_base, seen_count);
    // 如果回放给了不匹配的维度，记录上面的失败，不向 Vulkan 提交不合法的描述符。
    if (!shape_ok) {
        finish(active);
        active = NULL;
        return;
    }
    uint8_t pixels[64 * 64 * 4];
    sample(dir, frag, view, pixels);
    static const uint8_t zero[4] = {0};
    const int n = type == VK_IMAGE_VIEW_TYPE_CUBE ? 6 : type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY ? 2 : 4;
    unsigned matched = 0;
    for (int i = 0; i < n; i++) {
        const int x = n == 6 ? 10 + (i % 3) * 22 : 16 + (i % 2) * 32;
        const int y = n == 6 ? 16 + (i / 3) * 32 : n == 2 ? 32 : 16 + (i / 2) * 32;
        // samplerCubeArray 的层索引超过最后一组时夹到最后一组。
        const int layer = n != 2 ? i : count == 6 ? 0 : i * 6;
        const uint8_t *expected = first_color < 0 ? zero : colors[first_color + layer];
        const uint8_t *pixel = pixels + (y * 64 + x) * 4;
        matched += memcmp(pixel, expected, 4) == 0;
        CHECK(memcmp(pixel, expected, 4) == 0,
              "%s pixel %d: expected %02x%02x%02x%02x, got %02x%02x%02x%02x", label, i,
              expected[0], expected[1], expected[2], expected[3], pixel[0], pixel[1], pixel[2], pixel[3]);
    }
    printf("%s pixels=%u/%d\n", label, matched, n);
}

static void check_attachment_view_failure(void) {
    nvn_gpu_tex tex = make_tex(1, 1, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    const view_t swizzle = {.set_swizzle = 1, .swz = {4, 3, 2, 5}};
    nvn_gpu_view view = NULL;
    nvn_gpu_err err = {0};
    const nvn_gpu_rc result = nvn_gpu_view_create(tex, &swizzle, &view, &err);
    CHECK(OK(result), "color view for attachment replacement: %s", err.why);
    if (OK(result)) {
        nvn_gpu_cmd cmd = begin_cmd();
        nvn_gpu_pass_desc pass = {.ncolor = 1, .w = 1, .h = 1, .layers = 1};
        pass.color[0].view = view;
        pass.color[0].load = NVN_GPU_LOAD_CLEAR;
        pass.color[0].store = NVN_GPU_STORE_STORE;
        const int before = failed_views;
        fail_next_view = 1;
        nvn_gpu_pass_begin(cmd, &pass);
        fail_next_view = 0;
        CHECK(failed_views == before + 1, "identity color attachment view failure was injected");
        nvn_gpu_cmd_abort(cmd);
        nvn_gpu_view_release(view);
    }
    nvn_gpu_tex_release(tex);
}

static void check_storage_view_failure(const char *dir) {
    const nvn_gpu_layout_ent entry = {
        .cls = NVN_GPU_RES_IMAGE, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS,
    };
    const nvn_gpu_layout layout = {.ent = &entry, .nent = 1};
    char path[512];
    snprintf(path, sizeof path, "%s/compute_read_image.comp.spv", dir);
    size_t bytes = 0;
    void *blob = read_file(path, &bytes);
    CHECK(blob != NULL, "storage failure shader loaded from fixture");
    if (!blob) return;
    const nvn_gpu_shader_src src = {
        .stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV, .blob = blob, .bytes = bytes,
        .entry = "main", .layout = &layout,
    };
    nvn_gpu_err err = {0};
    nvn_gpu_shader shader = NULL;
    const nvn_gpu_rc loaded = nvn_gpu_shader_load(&src, &shader, &err);
    CHECK(OK(loaded), "storage failure shader: %s", err.why);
    free(blob);
    if (!OK(loaded)) return;
    const nvn_gpu_stage_desc stage = {.sh = shader};
    nvn_gpu_pipe pipe = NULL;
    const nvn_gpu_rc built = nvn_gpu_cpipe_get(&stage, NVN_GPU_PIPE_SYNC, &pipe, &err);
    CHECK(OK(built), "storage failure pipeline: %s", err.why);
    if (OK(built)) {
        nvn_gpu_tex tex = make_tex(1, 1, NVN_FORMAT_RGBA8, NVN_GPU_TEX_STORAGE);
        nvn_gpu_view plain = make_view(tex);
        nvn_gpu_cmd good = begin_cmd();
        nvn_gpu_set_pipe(good, pipe);
        const nvn_gpu_binding valid = {.cls = NVN_GPU_RES_IMAGE, .u.view = plain};
        nvn_gpu_bind(good, NVN_GPU_SH_CS, &valid, 1);
        watch_descriptor_writes = 1;
        nvn_gpu_dispatch(good, pipe, 1, 1, 1, NULL, 0);
        watch_descriptor_writes = 0;
        CHECK(dispatches == 1 && invalid_descriptor_writes == 0, "valid storage dispatch passed the call spies");
        finish(good);
        nvn_gpu_view_release(plain);

        const view_t swizzle = {.set_swizzle = 1, .swz = {4, 3, 2, 5}};
        nvn_gpu_view view = NULL;
        const nvn_gpu_rc viewed = nvn_gpu_view_create(tex, &swizzle, &view, &err);
        CHECK(OK(viewed), "sampled view for identity replacement: %s", err.why);
        if (OK(viewed)) {
            nvn_gpu_cmd cmd = begin_cmd();
            nvn_gpu_set_pipe(cmd, pipe);
            const nvn_gpu_binding binding = {.cls = NVN_GPU_RES_IMAGE, .u.view = view};
            nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &binding, 1);
            const int before = failed_views;
            const int dispatches_before = dispatches, done_before = atomic_load(&g_ndone);
            fail_next_view = watch_descriptor_writes = 1;
            nvn_gpu_dispatch(cmd, pipe, 1, 1, 1, NULL, 0);
            watch_descriptor_writes = fail_next_view = 0;
            CHECK(failed_views == before + 1, "identity storage view failure was injected");
            CHECK(invalid_descriptor_writes == 0, "failed identity storage view reached vkUpdateDescriptorSets");
            CHECK(dispatches == dispatches_before, "failed identity storage view reached vkCmdDispatch");
            const uint64_t signal = 1024;
            const nvn_gpu_rc committed = nvn_gpu_cmd_commit(cmd, signal, on_done, NULL, &err);
            CHECK(committed.code == NVN_GPU_E_OOM && strstr(err.why, "resource=storage_image") &&
                  strstr(err.why, "set=0 binding=0 element=0") && strstr(err.why, "VkResult=-1"),
                  "failed identity storage view preserved VkResult and binding: %d %s", committed.code, err.why);
            const struct done_rec *done = wait_signal(signal, done_before, 5000);
            CHECK(done && done->status == NVN_GPU_DONE_FAILED && done->code == NVN_GPU_E_OOM,
                  "failed identity storage view retired as a failed segment");
            printf("storage view fault: null writes=%d fault dispatches=%d result=%d %s\n",
                   invalid_descriptor_writes, dispatches - dispatches_before, committed.code, err.why);
            nvn_gpu_view_release(view);
        }
        nvn_gpu_tex_release(tex);
    }
    nvn_gpu_shader_release(shader);
}

static void exercise_audit_budget(void) {
    NVN_RA_IF(
        struct nvn_render_audit_binding site = {.stage = NVN_GPU_SH_CS, .set = 1};
        nvn_render_audit_event(NVN_RA_DESCRIPTOR, "fixture_repeat", &site, NULL);
        nvn_render_audit_event(NVN_RA_DESCRIPTOR, "fixture_repeat", &site, NULL);
        site.binding = 1;
        nvn_render_audit_event(NVN_RA_DESCRIPTOR, "fixture_repeat", &site, NULL);
        for (int i = 0; i < 80; i++) {
            site.binding = i;
            nvn_render_audit_event(NVN_RA_DESCRIPTOR, "fixture_unique", &site, NULL);
        }
        for (int i = 0; i < 300; i++) nvn_render_audit_present();
    );
}

static void check_empty_and_unavailable(const char *dir, const tex_t *cube) {
    nvn_be_set_tex_pool_n(16);
    active = begin_cmd();
    nvn_gpu_view empty = rp_view_by_id_decl(10, NVN_TEXTURE_TARGET_CUBEMAP + 1, 0, 1, active, NULL);
    CHECK(empty != NULL, "unregistered array slot has a valid typed zero view");
    if (empty) {
        uint8_t pixels[64 * 64 * 4];
        sample(dir, "cube.frag", empty, pixels);
        static const uint8_t zero[4];
        CHECK(memcmp(pixels + (16 * 64 + 10) * 4, zero, sizeof zero) == 0, "legitimate empty slot reads zero");
    } else { nvn_gpu_cmd_abort(active); active = NULL; }

    nvn_be_reg_tex(9, cube, NULL);
    active = begin_cmd();
    const int before = failed_views;
    fail_next_view = 1;
    nvn_gpu_view failed = rp_view_by_id_decl(9, NVN_TEXTURE_TARGET_CUBEMAP + 1, 0, 1, active, NULL);
    fail_next_view = 0;
    CHECK(failed_views == before + 1, "registered view creation failure was injected");
    CHECK(failed == NULL, "registered array texture failure was replaced with a zero texture");
    nvn_gpu_cmd_abort(active);
    active = NULL;

    const samp_par params = {0};
    nvn_be_reg_samp(8, &params);
    CHECK(rp_sampler_for_query() != NULL, "pool dummy sampler initialized");
    const char *why = NULL;
    nvn_gpu_pool_table table = rp_pool_table(NULL, 1, 9, 1, 0, 0, &why);
    CHECK(table != NULL, "empty sampler pool slot uses the valid dummy: %s", why ? why : "");
    fail_next_sampler = 1;
    table = rp_pool_table(NULL, 1, 8, 1, 0, 0, &why);
    fail_next_sampler = 0;
    CHECK(failed_samplers == 1, "registered sampler creation failure was injected");
    CHECK(table == NULL, "registered pool sampler failure was replaced with an empty sampler");
}

static void record_storage_access(nvn_gpu_cmd cmd, const char *dir, const char *file, nvn_gpu_view view, int write) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.spv", dir, file);
    size_t bytes = 0;
    void *blob = read_file(path, &bytes);
    CHECK(blob != NULL, "read audit shader loaded: %s", file);
    if (!blob) return;
    const nvn_gpu_layout_ent entries[] = {
        {.binding = 0, .cls = NVN_GPU_RES_IMAGE, .access = write ? NVN_GPU_ACC_W : NVN_GPU_ACC_R, .count = 1, .stages = 1u << NVN_GPU_SH_CS},
        {.binding = 1, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS},
    };
    const nvn_gpu_layout layout = {.ent = entries, .nent = write ? 1 : 2};
    const nvn_gpu_shader_src src = {
        .stage = NVN_GPU_SH_CS, .kind = NVN_GPU_BLOB_SPIRV, .blob = blob, .bytes = bytes, .entry = "main", .layout = &layout,
    };
    nvn_gpu_shader shader = NULL;
    nvn_gpu_err err = {0};
    const nvn_gpu_rc loaded = nvn_gpu_shader_load(&src, &shader, &err);
    free(blob);
    CHECK(OK(loaded), "read audit shader: %s", err.why);
    if (!OK(loaded)) return;
    const nvn_gpu_stage_desc stage = {.sh = shader};
    nvn_gpu_pipe pipe = NULL;
    const nvn_gpu_rc built = nvn_gpu_cpipe_get(&stage, NVN_GPU_PIPE_SYNC, &pipe, &err);
    CHECK(OK(built), "read audit compute pipeline: %s", err.why);
    if (OK(built)) {
        nvn_gpu_buf output = write ? NULL : make_buf(16, NVN_GPU_MEM_SHARED, NULL);
        const nvn_gpu_binding bind[] = {
            {.ent = 0, .cls = NVN_GPU_RES_IMAGE, .u.view = view},
            {.ent = 1, .cls = NVN_GPU_RES_SSBO, .u.b = {.buf = output, .len = 16}},
        };
        nvn_gpu_set_pipe(cmd, pipe);
        nvn_gpu_bind(cmd, NVN_GPU_SH_CS, bind, layout.nent);
        nvn_gpu_dispatch(cmd, pipe, 0, 1, 1, NULL, 0);  // Legal no-op with real bindings.
        nvn_gpu_dispatch(cmd, pipe, 1, 1, 1, NULL, 0);
        if (output) nvn_gpu_buf_release(output, NULL, NULL);
    }
    nvn_gpu_shader_release(shader);
}

static void check_read_definition(const char *dir) {
    const uint32_t usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_STORAGE;
    nvn_gpu_tex fresh = make_tex(1, 1, NVN_FORMAT_RGBA8, usage);
    nvn_gpu_tex black = make_tex(1, 1, NVN_FORMAT_RGBA8, usage);
    nvn_gpu_view fresh_view = make_view(fresh), black_view = make_view(black);
    const uint8_t poison[8] = {0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd};
    nvn_gpu_buf read = make_buf(sizeof poison, NVN_GPU_MEM_SHARED, poison);
    const float black_rgba[4] = {0};
    const uint32_t black_uint[4] = {0};
    nvn_gpu_cmd discarded = begin_cmd();
    nvn_gpu_clear_view(discarded, fresh_view, black_rgba, black_uint, 0);
    nvn_gpu_cmd_abort(discarded);  // A recorded but abandoned clear never defined the image.
    nvn_gpu_cmd cmd = begin_cmd();
    NVN_RA_IF(nvn_render_audit_context(17, OP_COPY_T2B));

    // The first copy consumes uninitialized pixels. Its result is unspecified;
    // only the diagnostic is asserted. The readback below asserts real black.
    nvn_gpu_copy_tex_buf(cmd, fresh, 0, 0, NULL, read, 0, 4, 4);
    nvn_gpu_pass_desc load = {.ncolor = 1, .w = 1, .h = 1, .layers = 1};
    load.color[0].view = fresh_view;
    load.color[0].load = NVN_GPU_LOAD_LOAD;
    load.color[0].store = NVN_GPU_STORE_STORE;
    nvn_gpu_pass_begin(cmd, &load);
    nvn_gpu_pass_end(cmd);
    NVN_RA_IF(nvn_render_audit_context(17, OP_DISPATCH));
    record_storage_access(cmd, dir, "audit_storage_read.comp", fresh_view, 0);
    record_storage_access(cmd, dir, "audit_storage_read_dynamic.comp", fresh_view, 0);

    nvn_gpu_clear_view(cmd, black_view, black_rgba, black_uint, 0);
    load.color[0].view = black_view;
    nvn_gpu_pass_begin(cmd, &load);
    nvn_gpu_pass_end(cmd);
    record_storage_access(cmd, dir, "audit_storage_read.comp", black_view, 0);
    nvn_gpu_copy_tex_buf(cmd, black, 0, 0, NULL, read, 4, 4, 4);
    finish(cmd);

    const uint8_t *actual = buf_cpu(read);
    const uint8_t zero[4] = {0};
    CHECK(actual && !memcmp(actual + 4, zero, sizeof zero), "defined black must read back exactly zero");
    nvn_gpu_buf_release(read, NULL, NULL);
    nvn_gpu_view_release(fresh_view);
    nvn_gpu_view_release(black_view);
    nvn_gpu_tex_release(fresh);
    nvn_gpu_tex_release(black);
}

static void check_storage_write_outcome(const char *dir) {
    tex_t previous = {.serial = 100, .d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
        .width = 1, .height = 1, .depth = 1, .levels = 1, .flags = NVN_TEXTURE_FLAGS_IMAGE}};
    nvn_tex_layout(&previous.d, &previous.L);
    tex_t rejected = previous;
    rejected.serial++;
    active = begin_cmd();
    nvn_gpu_view view = rp_view_for(&previous, NULL, active);
    CHECK(view, "replay storage write view");
    if (!view) { nvn_gpu_cmd_abort(active); active = NULL; return; }
    nvn_gpu_buf read = make_buf(4, NVN_GPU_MEM_SHARED, NULL);
    record_storage_access(active, dir, "compute_read_image.comp", view, 1);
    CHECK(nvn_gpu_cmd_record_ok(active), "record valid storage producer");
    rp_tex_mark_gpu_written(active, &previous);
    nvn_gpu_copy_tex_buf(active, nvn_gpu_view_texture(view), 0, 0, NULL, read, 0, 4, 4);
    finish(active);
    const uint8_t green[4] = {0, 255, 0, 255};
    const void *actual = buf_cpu(read);
    CHECK(actual && !memcmp(actual, green, sizeof green), "successful storage producer wrote the shader's green texel");
    nvn_gpu_buf_release(read, NULL, NULL);

    active = begin_cmd();
    view = rp_view_for(&rejected, NULL, active);
    CHECK(view, "storage write view for failed segment");
    if (!view) { nvn_gpu_cmd_abort(active); active = NULL; return; }
    record_storage_access(active, dir, "compute_read_image.comp", view, 1);
    CHECK(nvn_gpu_cmd_record_ok(active), "failed segment actually recorded the storage producer");
    rp_tex_mark_gpu_written(active, &rejected);
    CHECK(nvn_be_tex_gpu_owned(rejected.serial), "same recording sees its storage writer");
    const int done_before = atomic_load(&g_ndone);
    const uint64_t failed_signal = ++fixture_signal;
    nvn_gpu_err err = {0};
    nvn_gpu_vk_test_set_fault(1);
    CHECK(nvn_gpu_cmd_commit(active, failed_signal, on_done, NULL, &err).code == NVN_GPU_E_DEVICE,
          "storage producer segment was rejected: %s", err.why);
    const struct done_rec *done = wait_signal(failed_signal, done_before, 5000);
    CHECK(done && done->status == NVN_GPU_DONE_FAILED, "failed storage producer retired");
    const int previous_owned = nvn_be_tex_gpu_owned(previous.serial), rejected_owned = nvn_be_tex_gpu_owned(rejected.serial);
    CHECK(previous_owned && !rejected_owned, "storage write ownership survived only for the completed segment");
    printf("storage writer outcome: completed_owner=%d failed_owner=%d\n", previous_owned, rejected_owned);
    active = NULL;
}

int main(int argc, char **argv) {
    const int read_audit = argc == 3 && !strcmp(argv[2], "read-audit");
    if (argc != 2 && !read_audit) return 2;
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    g_desc = (nvn_gpu_open_desc){.host_device = &g_dev, .app_label = "replay-cube-view"};
    if (!open_backend()) return 2;
    if (read_audit) {
        check_read_definition(argv[1]);
        check_storage_write_outcome(argv[1]);
        rp_res_reset();
        nvn_gpu_err err = {0};
        CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "close read audit: %s", err.why);
        s2a_vk_device_destroy(&g_dev, 1);
        printf("read audit workload failures=%d\n", g_fail);
        return g_fail ? 1 : 0;
    }
    for (unsigned i = 0; i < 12; i++) {
        colors[i][0] = 17 + 11 * i;
        colors[i][1] = 31 + 13 * i;
        colors[i][2] = 47 + 17 * i;
        colors[i][3] = 255;
    }
    tex_t cube, array, single;
    register_storage(&cube, NVN_TEXTURE_TARGET_CUBEMAP, 6, 1);
    register_storage(&array, NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 12, 2);
    register_storage(&single, NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 6, 3);
    view_t v = {.set_target = 1, .target = NVN_TEXTURE_TARGET_CUBEMAP, .set_layers = 1, .nlayers = 6};
    nvn_be_reg_tex(1, &cube, NULL);
    v.min_layer = 6;
    nvn_be_reg_tex(2, &array, &v);
    v.min_layer = 1;
    v.nlayers = 5;
    nvn_be_reg_tex(3, &cube, &v);
    nvn_be_reg_tex(8, &cube, &v);
    v.target = NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    v.nlayers = 7;
    nvn_be_reg_tex(4, &array, &v);
    nvn_be_reg_tex(5, &array, NULL);
    nvn_be_reg_tex(6, &single, NULL);
    v.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    v.nlayers = 5;
    nvn_be_reg_tex(7, &cube, &v);

    rp_decl_probe_begin();
    const nvn_gpu_view probed = rp_view_by_id_decl(8, NVN_TEXTURE_TARGET_CUBEMAP + 1, 0, 0, NULL, NULL);
    CHECK(rp_decl_probe_end() == 1 && probed == NULL, "partial cube must fail library declaration probe");
    CHECK(rp_view_by_id_decl(8, NVN_TEXTURE_TARGET_CUBEMAP + 1, 0, 0, NULL, NULL) == NULL,
          "partial cube must not bind a non-cube when no zero fallback can be created");

    check_sample(argv[1], "complete cube", 1, NVN_TEXTURE_TARGET_CUBEMAP, "cube.frag", VK_IMAGE_VIEW_TYPE_CUBE, 0, 6, 0);
    check_sample(argv[1], "offset cube", 2, NVN_TEXTURE_TARGET_CUBEMAP, "cube.frag", VK_IMAGE_VIEW_TYPE_CUBE, 6, 6, 6);
    check_sample(argv[1], "partial cube", 3, NVN_TEXTURE_TARGET_CUBEMAP, "cube.frag", VK_IMAGE_VIEW_TYPE_CUBE, 0, 6, -1);
    check_sample(argv[1], "partial cube array", 4, NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, "cubearr.frag",
                 VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 0, 6, -1);
    check_sample(argv[1], "complete cube array", 5, NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, "cubearr.frag",
                 VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 0, 12, 0);
    check_sample(argv[1], "single cube array", 6, NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, "cubearr.frag",
                 VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 0, 6, 0);
    check_sample(argv[1], "five-layer 2D array control", 7, NVN_TEXTURE_TARGET_2D_ARRAY, "arr.frag",
                 VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, 5, 1);
    check_empty_and_unavailable(argv[1], &cube);
    check_layered_attachment_retirement(0);
    check_layered_attachment_retirement(1);
    check_attachment_view_failure();
    check_storage_view_failure(argv[1]);
    exercise_audit_budget();
    rp_res_reset();
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "close: %s", err.why);
    s2a_vk_device_destroy(&g_dev, 1);
    printf("replay cube view failures=%d\n", g_fail);
    return g_fail ? 1 : 0;
}
