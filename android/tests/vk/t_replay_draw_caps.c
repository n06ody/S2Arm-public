// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define main zero_attr_fixture_main
#include "t_replay_zero_attr.c"
#undef main

enum { TARGET_W = 64, TARGET_H = 64, TARGET_BYTES = TARGET_W * TARGET_H * 4 };
static int hide_rgb8sn;

VKAPI_ATTR void VKAPI_CALL __real_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat, VkFormatProperties *);
VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice dev, VkFormat fmt, VkFormatProperties *p) {
    __real_vkGetPhysicalDeviceFormatProperties(dev, fmt, p);
    if (hide_rgb8sn && fmt == VK_FORMAT_R8G8B8_SNORM) p->bufferFeatures &= ~VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
}

static void draw_case(nvn_gpu_tex tex, const p_draw *direct, const p_draw_idx *indexed, uint64_t signal, uint8_t *pixels) {
    nvn_gpu_buf rb = buffer(TARGET_BYTES, NULL);
    GPU(nvn_gpu_cmd_begin(NULL, &active, &err));
    serial++;
    pass(active, 1);
    rp_draw(active, direct, indexed, direct ? (const void *)direct : (const void *)indexed);
    rp_pass_flush(active);
    nvn_gpu_copy_tex_buf(active, tex, 0, 0, NULL, rb, 0, TARGET_W * 4, TARGET_BYTES);
    GPU(nvn_gpu_cmd_commit(active, signal, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(signal, 5000000000ull, &err));
    memcpy(pixels, nvn_gpu_buf_cpu(rb), TARGET_BYTES);
    nvn_gpu_buf_release(rb, NULL, NULL);
}

static void report_solid(const char *name, const uint8_t *pixels, uint8_t red, unsigned draws, int skips) {
    unsigned bad = 0;
    for (unsigned i = 0; i < TARGET_BYTES; i += 4)
        if (pixels[i] != red || pixels[i+1] || pixels[i+2] || pixels[i+3] != 255) bad++;
    printf("%s pixels=%u/%u draws=%u skips=%d\n", name, TARGET_W * TARGET_H - bad, TARGET_W * TARGET_H,
           emitted - draws, nvn_unimpl_kinds());
    if (bad || emitted - draws != (unsigned)!skips || nvn_unimpl_kinds() != skips) failures++;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    struct s2a_vk_device dev;
    char why[256];
    if (s2a_vk_device_create(&dev, why, sizeof why)) { fprintf(stderr, "%s\n", why); return 2; }
    printf("device: %s (%s)\n", dev.properties.deviceName, dev.driver_name);
    nvn_gpu_open_desc od = {.host_device = &dev, .app_label = "real-replay-draw-caps"};
    GPU(nvn_gpu_open(&od, &err));
    prog.sh[0] = shader(argv[1], "tri3.vert", NVN_GPU_SH_VS, NULL);
    prog.sh[1] = shader(argv[1], "tri_solid.frag", NVN_GPU_SH_FS, NULL);
    prog.attr_mask = 1;
    prog.color_out_mask = 15;
    nvn_gpu_tex_desc td = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
                           .width = TARGET_W, .height = TARGET_H, .depth = 1, .levels = 1}, .usage = NVN_GPU_TEX_RENDER};
    nvn_gpu_tex tex = NULL;
    GPU(nvn_gpu_tex_create(&td, &tex, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &target_view, &err));
    target_tex.d = td.d;
    target_tex.serial = 1;
    target.n = 1;
    target.color[0] = &target_tex;
    rp_state_reset();
    rp_draw_reset();
    rp_S.nattr = rp_S.nstream = 1;
    rp_S.color.logic_op = NVN_LOGIC_OP_COPY;
    rp_S.chmask.mask = 15;
    rp_S.poly.mode = NVN_POLYGON_MODE_FILL;
    rp_S.vbuf[0].address = (uintptr_t)guest;
    uint8_t got[TARGET_BYTES], expect[TARGET_BYTES];

    const int32_t scaled[4][4] = {{9,9,0,7}, {-1,-1,0,7}, {3,-1,0,7}, {-1,3,0,7}};
    memcpy(guest, scaled, sizeof scaled);
    if (getenv("S2A_DRAW_CAPS_WRONG_VERTEX")) memcpy(guest + sizeof scaled[0], scaled, sizeof scaled[0]);
    rp_S.attr[0].fmt = NVN_FORMAT_RGB32_I2F;
    REQUIRE((rp_fmt_caps(NVN_FORMAT_RGB32_I2F) & (NVN_GPU_FMT_VERTEX | NVN_GPU_FMT_VERTEX_CPU)) == NVN_GPU_FMT_VERTEX_CPU);
    rp_S.stream[0].stride = sizeof scaled[0];
    rp_S.vbuf[0].size = sizeof scaled;
    p_draw direct = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .first = 1, .count = 3, .instances = 1};
    unsigned before = emitted;
    nvn_unimpl_reset();
    draw_case(tex, &direct, NULL, 1, got);
    report_solid("DRAW_CAPS cpu-i2f", got, 255, before, 0);

    const int8_t rgb[6][4] = {{-127,-127,0,7}, {127,-127,0,7}, {-127,127,0,7},
                             {-127,127,0,7}, {127,-127,0,7}, {127,127,0,7}};
    memcpy(guest, rgb, sizeof rgb);
    hide_rgb8sn = 1;
    rp_pso_dirty = 1;
    rp_S.attr[0].fmt = NVN_FORMAT_RGB8SN;
    REQUIRE((rp_fmt_caps(NVN_FORMAT_RGB8SN) & (NVN_GPU_FMT_VERTEX | NVN_GPU_FMT_VERTEX_CPU)) == NVN_GPU_FMT_VERTEX_CPU);
    rp_S.stream[0].stride = sizeof rgb[0];
    rp_S.vbuf[0].size = sizeof rgb;
    direct.first = 0;
    direct.count = 6;
    before = emitted;
    nvn_unimpl_reset();
    draw_case(tex, &direct, NULL, 2, got);
    report_solid("DRAW_CAPS cpu-rgb8sn", got, 255, before, 0);
    const uint16_t quad_indices[] = {0, 1, 2, 3, 4, 5};
    memcpy(guest + 128, quad_indices, sizeof quad_indices);
    const p_draw_idx cpu_indexed = {.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.itype=1,.count=6,.instances=1,
                                    .ibuf=(uintptr_t)(guest + 128)};
    before = emitted;
    nvn_unimpl_reset();
    draw_case(tex, NULL, &cpu_indexed, 3, got);
    report_solid("DRAW_CAPS cpu-rgb8sn-indexed", got, 255, before, 0);
    const uint32_t indirect_args[4] = {6, 1, 0, 0};
    memcpy(guest + 192, indirect_args, sizeof indirect_args);
    const p_draw cpu_indirect = {.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.ind=(uintptr_t)(guest + 192)};
    before = emitted;
    nvn_unimpl_reset();
    draw_case(tex, &cpu_indirect, NULL, 4, got);
    report_solid("DRAW_CAPS cpu-indirect-refused", got, 0, before, 1);
    hide_rgb8sn = 0;

    float line_pos[256][3] = {{-0.875f,-0.875f,0}, {-0.125f,-0.875f,0}, {-0.125f,-0.125f,0},
                              { 0.125f, 0.125f,0}, { 0.875f, 0.125f,0}, { 0.875f, 0.875f,0}};
    memcpy(guest, line_pos, sizeof line_pos);
    const uint8_t loops[] = {0,1,2,255,3,4,5,255};
    const uint16_t lines[] = {0,1,1,2,2,0,3,4,4,5,5,3};
    memcpy(guest + sizeof line_pos, loops, sizeof loops);
    memcpy(guest + sizeof line_pos + sizeof loops, lines, sizeof lines);
    rp_S.attr[0].fmt = NVN_FORMAT_RGB32F;
    rp_S.stream[0].stride = sizeof line_pos[0];
    rp_S.vbuf[0].size = sizeof line_pos;
    rp_S.restart = (p_prim_restart){.on=1,.index=0x100ff};
    rp_pso_dirty = 1;
    p_draw_idx indexed = {.prim=NVN_DRAW_PRIMITIVE_LINE_LOOP,.itype=0,.count=sizeof loops,.instances=1,
                          .ibuf=(uintptr_t)(guest + sizeof line_pos)};
    nvn_unimpl_reset();
    draw_case(tex, NULL, &indexed, 5, got);
    rp_S.restart.on = 0;
    indexed.prim = NVN_DRAW_PRIMITIVE_LINES;
    indexed.itype = 1;
    indexed.count = sizeof lines / sizeof lines[0];
    indexed.ibuf += sizeof loops;
    draw_case(tex, NULL, &indexed, 6, expect);
    unsigned line_bad = 0, lit = 0;
    for (unsigned i = 0; i < TARGET_BYTES; i += 4) {
        if (memcmp(got+i, expect+i, 4)) line_bad++;
        if (expect[i]) lit++;
    }
    printf("DRAW_CAPS indexed-loop pixels=%u/%u lit=%u\n", TARGET_W * TARGET_H - line_bad, TARGET_W * TARGET_H, lit);
    if (line_bad || !lit || nvn_unimpl_kinds()) failures++;

    const float triangle[3][3] = {{-1,-1,0}, {3,-1,0}, {-1,3,0}};
    memcpy(guest, triangle, sizeof triangle);
    rp_S.vbuf[0].size = sizeof triangle;
    rp_S.color.blend_en = 1;
    rp_S.blend[0] = (blend_st){.src_rgb=NVN_BLEND_FUNC_ONE,.dst_rgb=NVN_BLEND_FUNC_ZERO,
        .src_a=NVN_BLEND_FUNC_ONE,.dst_a=NVN_BLEND_FUNC_ZERO,.eq_rgb=NVN_BLEND_EQUATION_ADD,
        .eq_a=NVN_BLEND_EQUATION_ADD,.adv_mode=NVN_BLEND_ADVANCED_MODE_MULTIPLY};
    rp_pso_dirty = 1;
    direct.count = 3;
    before = emitted;
    nvn_unimpl_reset();
    draw_case(tex, &direct, NULL, 7, got);
    report_solid("DRAW_CAPS advanced-refused", got, 0, before, 1);

    rp_draw_reset();
    rp_buf_reset();
    nvn_gpu_shader_release(prog.sh[0]);
    nvn_gpu_shader_release(prog.sh[1]);
    nvn_gpu_view_release(target_view);
    nvn_gpu_tex_release(tex);
    GPU(nvn_gpu_close(5000000000ull, &err));
    REQUIRE(!s2a_vk_device_destroy(&dev, 1));
    printf("DRAW_CAPS failures=%u\n", failures);
    return failures != 0;
}
