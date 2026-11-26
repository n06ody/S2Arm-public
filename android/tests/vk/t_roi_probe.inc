// SPDX-License-Identifier: Apache-2.0
#include "rp_exec.h"
#include "rp_res.h"

int rp_tm_on;
static nvn_gpu_cmd roi_fixture_cmd;
void rp_perf_add(int i, uint64_t v) {
    (void)i;
    (void)v;
}
uint64_t rp_seg_serial(void) { return 1; }
uint64_t rp_sub_serial(void) { return 1; }
int rp_buf_gpu_pending(uint64_t addr, uint64_t n) {
    (void)addr;
    (void)n;
    return 0;
}
nvn_gpu_cmd rp_xfer_cmd(uint64_t *serial) {
    *serial = 1;
    return roi_fixture_cmd;
}

// Both wrappers forward the original data unchanged. The observed descriptor
// handle and copy count come from actual Vulkan calls, independent of ROI logs.
static atomic_uint roi_pixel_copies;
static PFN_vkCmdCopyImageToBuffer roi_driver_copy;
static atomic_uint roi_full_copies;
static VKAPI_ATTR void VKAPI_CALL roi_count_copy(VkCommandBuffer cmd, VkImage img, VkImageLayout layout,
                                                 VkBuffer buf, uint32_t n, const VkBufferImageCopy *regions) {
    for (uint32_t i = 0; i < n; i++) {
        if (regions[i].imageExtent.width == 1 && regions[i].imageExtent.height == 1)
            atomic_fetch_add(&roi_pixel_copies, 1);
        if (regions[i].imageExtent.width == 16 && regions[i].imageExtent.height == 16)
            atomic_fetch_add(&roi_full_copies, 1);
    }
    roi_driver_copy(cmd, img, layout, buf, n, regions);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __real_vkGetDeviceProcAddr(VkDevice, const char *);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice device, const char *name) {
    PFN_vkVoidFunction original = __real_vkGetDeviceProcAddr(device, name);
    if (original && !strcmp(name, "vkCmdCopyImageToBuffer")) {
        roi_driver_copy = (PFN_vkCmdCopyImageToBuffer)original;
        return (PFN_vkVoidFunction)roi_count_copy;
    }
    return original;
}
VKAPI_ATTR void VKAPI_CALL __real_vkUpdateDescriptorSets(VkDevice, uint32_t, const VkWriteDescriptorSet *,
                                                         uint32_t, const VkCopyDescriptorSet *);
VKAPI_ATTR void VKAPI_CALL __wrap_vkUpdateDescriptorSets(VkDevice device, uint32_t n,
                                                         const VkWriteDescriptorSet *writes, uint32_t nc,
                                                         const VkCopyDescriptorSet *copies) {
    for (uint32_t i = 0; i < n; i++)
        if (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
            for (uint32_t k = 0; k < writes[i].descriptorCount; k++)
                printf("ROI actual sampled view=%llu\n",
                       (unsigned long long)(uintptr_t)writes[i].pImageInfo[k].imageView);
    __real_vkUpdateDescriptorSets(device, n, writes, nc, copies);
}
// Three real presents of the same scene. Only the requested pixel of frame 2
// changes. Read both attachments independently, even when the probe is off.
static int roi_probe_only(int on, const char *dir) {
    char why[256] = {0};
    g_roi_fixture = on ? 2 : 1;
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) {
        fprintf(stderr, "ROI device: %s\n", why);
        return 2;
    }
    memset(&g_desc, 0, sizeof g_desc);
    g_desc.host_device = &g_dev;
    g_desc.app_label = "roi-probe";
    if (!open_backend())
        return 1;
    nvn_gpu_err e = {0};
    nvn_gpu_surface sf = NULL;
    CHECK(OK(nvn_gpu_surface_attach((void *)1, &sf, &e)), "ROI attach: %s", e.why);
    const uint32_t usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex hdr = make_tex(16, 16, NVN_FORMAT_RGBA16F, usage);
    nvn_gpu_tex depth = make_tex(16, 16, NVN_FORMAT_DEPTH32F, usage);
    nvn_gpu_tex screen = make_tex(16, 16, NVN_FORMAT_RGBA8, usage);
    nvn_gpu_view hv = make_view(hdr), dv = make_view(depth), sv = make_view(screen);
    nvn_gpu_tex composite = make_tex(16, 16, NVN_FORMAT_RGBA8, usage);
    nvn_gpu_view composite_view = make_view(composite);
    nvn_gpu_buf oracle = make_buf(4096, NVN_GPU_MEM_SHARED, NULL);
    samp_par sp = {0};
    nvn_gpu_samp sampler = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &sampler, &e)), "ROI sampler");
    nvn_gpu_tex array = make_tex_target(4, 4, 4, NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8, usage);
    nvn_gpu_tex volume = make_tex_target(4, 4, 4, NVN_TEXTURE_TARGET_3D, NVN_FORMAT_RGBA16F, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view av = make_view(array), vv = make_view(volume);
    float constants[9][4] = {{0}};
    constants[8][3] = 0.25f;
    nvn_gpu_buf coefficients = make_buf(sizeof constants, NVN_GPU_MEM_SHARED, constants);
    for (unsigned frame = 1; frame <= 3; ++frame) {
        nvn_gpu_cmd c = begin_cmd();
        unsigned copies_before = atomic_load(&roi_pixel_copies);
        unsigned full_before = atomic_load(&roi_full_copies);
        roi_fixture_cmd = c;
        // An older write, a neighbouring slice, and adjacent frames all have a
        // different value from the texel selected by the target fragment.
        uint16_t texels[4][4][4][4] = {0};
        // The generic 3D upload defaults to one Z slice. Declare the volume.
        const NVNcopyRegion extent = {.width = 4, .height = 4, .depth = 4};
        nvn_gpu_tex_upload(c, volume, 0, 0, &extent, texels, 32, sizeof texels);
        uint16_t mark[4] = {0x3800, 0xb400, 0x4000, 0x3c00};
        if (frame == 2) memcpy(texels[2][1][2], mark, sizeof mark);
        nvn_gpu_tex_upload(c, volume, 0, 0, &extent, texels, 32, sizeof texels);
        nvn_gpu_view zero = rp_view_by_ubo_handle_decl(0xfffff, NVN_TEXTURE_TARGET_CUBEMAP + 1, 0, c, NULL);
        CHECK(zero != NULL, "ROI real replay cube zero");
        nvn_gpu_pass_desc pass = {0};
        pass.ncolor = 1;
        pass.color[0].view = hv;
        pass.color[0].load = pass.depth.load = NVN_GPU_LOAD_CLEAR;
        pass.color[0].store = pass.depth.store = NVN_GPU_STORE_STORE;
        pass.color[0].clear.f[3] = 1;
        pass.depth.view = dv;
        pass.clear_depth = 0.75f;
        pass.w = pass.h = 16;
        pass.layers = 1;
        nvn_gpu_pass_begin(c, &pass);
        if (frame == 2) {
            nvn_gpu_clear rect = {0};
            rect.color = 0;
            rect.color_mask = 15;
            rect.value.f[0] = 2;
            rect.value.f[1] = -0.5f;
            rect.value.f[2] = 0.25f;
            rect.value.f[3] = 1;
            rect.x = 3;
            rect.y = 5;
            rect.w = rect.h = 1;
            nvn_gpu_clear_rect(c, &rect);
            rect.color = -1;
            rect.depth = 1;
            rect.depth_value = 0.125f;
            nvn_gpu_clear_rect(c, &rect);
        }
        nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c, hdr, 0, 0, NULL, oracle, 0, 128, 2048);
        nvn_gpu_copy_tex_buf(c, depth, 0, 0, NULL, oracle, 2048, 64, 1024);
        pass.color[0].view = sv;
        nvn_gpu_pass_begin(c, &pass);
        fs_draw_tex_frag_depth(c, zero, sampler, dir, "cube.frag", 1);
        fs_draw_tex_frag_depth(c, av, sampler, dir, "arr.frag", 1);
        fs_draw_tex_frag_depth(c, vv, sampler, dir, "tex3d.frag", 1);
        fs_draw_tex_frag_depth(c, hv, sampler, dir, "tri.frag", 1);
        nvn_gpu_pass_end(c);
        pass.depth.view = NULL;
        pass.color[0].view = sv;
        nvn_gpu_pass_begin(c, &pass);
        fs_draw_tex_frag_inputs(c, vv, sampler, dir, "roi_volume.frag", 0, coefficients);
        nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c, screen, 0, 0, NULL, oracle, 3072, 64, 1024);
        pass.color[0].view = composite_view;
        nvn_gpu_pass_begin(c, &pass);
        fs_draw_tex_frag_inputs(c, sv, sampler, dir, "tri_invert.frag", 0, NULL);
        nvn_gpu_pass_end(c);
        pass.color[0].view = sv;
        pass.color[0].clear.f[2] = 1;
        nvn_gpu_pass_begin(c, &pass);
        nvn_gpu_pass_end(c);
        CHECK(OK(nvn_gpu_cmd_commit(c, frame, on_done, NULL, &e)), "ROI commit: %s", e.why);
        CHECK(OK(nvn_gpu_fence_wait(frame, 5000000000ull, &e)), "ROI wait: %s", e.why);
        const uint8_t *bytes = buf_cpu(oracle);
        const uint8_t *result = bytes + 3072 + 4 * (5 * 16 + 3);
        unsigned half = frame == 2 ? 128 : 0;
        CHECK(result[0] == half && result[1] == 0 && result[2] == half &&
              result[3] == (frame == 2 ? 255 : 0),
              "ROI independent shader output frame=%u rgba=%u,%u,%u,%u", frame,
              result[0], result[1], result[2], result[3]);
        uint16_t rgba[4];
        float z;
        for (int x = 3; x <= 4; ++x) {
            const int px = 5 * 16 + x;
            memcpy(rgba, bytes + px * 8, sizeof rgba);
            memcpy(&z, bytes + 2048 + px * 4, sizeof z);
            int mark = frame == 2 && x == 3;
            CHECK(rgba[0] == (mark ? 0x4000 : 0) && rgba[1] == (mark ? 0xb800 : 0) &&
                      rgba[2] == (mark ? 0x3400 : 0) && rgba[3] == 0x3c00 && z == (mark ? 0.125f : 0.75f),
                  "ROI oracle frame=%u x=%d rgba=%x/%x/%x/%x z=%g", frame, x, rgba[0], rgba[1], rgba[2], rgba[3],
                  (double)z);
        }
        nvn_gpu_drawable drawable = {0};
        nvn_gpu_rc acquired = nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &drawable, &e);
        CHECK(OK(acquired), "ROI acquire: %s", e.why);
        if (!OK(acquired))
            break;
        nvn_gpu_present_desc present = {0};
        present.src = sv;
        CHECK(OK(nvn_gpu_present(sf, &drawable, &present, 100 + frame, on_shown, NULL, &e)), "ROI present: %s",
              e.why);
        unsigned added = atomic_load(&roi_pixel_copies) - copies_before;
        CHECK(atomic_load(&roi_full_copies) - full_before == 3,
              "ROI copy observer missed independent oracle copies");
        CHECK((added != 0) == (on && frame == 2), "ROI copies on=%d frame=%u got=%u", on, frame, added);
        printf("ROI Vulkan 1x1 copies on=%d frame=%u count=%u\n", on, frame, added);
    }
    rp_res_reset();
    nvn_gpu_view_release(av);
    nvn_gpu_view_release(vv);
    nvn_gpu_tex_release(array);
    nvn_gpu_tex_release(volume);
    nvn_gpu_samp_release(sampler);
    nvn_gpu_buf_release(coefficients, NULL, NULL);
    nvn_gpu_buf_release(oracle, NULL, NULL);
    nvn_gpu_view_release(sv);
    nvn_gpu_view_release(dv);
    nvn_gpu_view_release(hv);
    nvn_gpu_view_release(composite_view);
    nvn_gpu_tex_release(composite);
    nvn_gpu_tex_release(screen);
    nvn_gpu_tex_release(depth);
    nvn_gpu_tex_release(hdr);
    CHECK(OK(nvn_gpu_surface_detach(sf, 5000000000ull, &e)), "ROI detach: %s", e.why);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "ROI close: %s", e.why);
    s2a_vk_device_destroy(&g_dev, 1);
    printf("ROI oracle three presents, HDR/depth and adjacent pixel failures=%d\n", g_fail);
    return g_fail != 0;
}
