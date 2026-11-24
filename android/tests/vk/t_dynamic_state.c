// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define main feedback_main_unused
#include "t_feedback.c"
#undef main
#include "nvn_vk_pipe_warm.h"
#include "rp_shader.h"

extern uint64_t nvn_vk_dbg_pipe_stat(int which);
extern void nvn_vk_set_draw_pipe(nvn_gpu_cmd, nvn_gpu_pipe, const nvn_gpu_pipe_desc *) __attribute__((weak));

enum { DRAWS = 12, TILE = 4, WIDTH = DRAWS * TILE, BYTES = WIDTH * TILE * 4 };

static const uint8_t warm_identity[32] = {1};
static struct rp_prog warm_program;
static int warm_borrowed, hide_features;
static unsigned queried_dynamic, creates;

int rp_shader_prog_borrow(const uint8_t identity[32], const struct rp_prog **out) {
    REQUIRE(!warm_borrowed && !memcmp(identity, warm_identity, sizeof warm_identity));
    warm_borrowed = 1;
    *out = &warm_program;
    return 1;
}

void rp_shader_prog_return(const uint8_t identity[32]) {
    REQUIRE(warm_borrowed && !memcmp(identity, warm_identity, sizeof warm_identity));
    warm_borrowed = 0;
}

extern __typeof__(vkGetPhysicalDeviceFeatures2) __real_vkGetPhysicalDeviceFeatures2;
VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *features) {
    __real_vkGetPhysicalDeviceFeatures2(pd, features);
    for (VkBaseOutStructure *s = features->pNext; s; s = s->pNext) {
        switch (s->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT:
            queried_dynamic |= 1;
            if (hide_features) ((VkPhysicalDeviceExtendedDynamicStateFeaturesEXT *)s)->extendedDynamicState = VK_FALSE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT:
            queried_dynamic |= 2;
            if (hide_features) ((VkPhysicalDeviceExtendedDynamicState2FeaturesEXT *)s)->extendedDynamicState2 = VK_FALSE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT:
            queried_dynamic |= 4;
            if (hide_features) ((VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT *)s)->vertexInputDynamicState = VK_FALSE;
            break;
        default: break;
        }
    }
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkGetPhysicalDeviceFeatures2), PFN_vkGetPhysicalDeviceFeatures2),
               "vkGetPhysicalDeviceFeatures2 signature");

extern __typeof__(vkCreateDevice) __real_vkCreateDevice;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                                    const VkAllocationCallbacks *alloc, VkDevice *device) {
    for (const VkBaseInStructure *node = info->pNext; node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT) {
            const VkPhysicalDeviceExtendedDynamicState2FeaturesEXT *eds2 = (const void *)node;
            REQUIRE(eds2->extendedDynamicState2 == VK_TRUE);
            REQUIRE(eds2->extendedDynamicState2LogicOp == VK_FALSE);
            REQUIRE(eds2->extendedDynamicState2PatchControlPoints == VK_FALSE);
        }
    }
    return __real_vkCreateDevice(physical, info, alloc, device);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateDevice), PFN_vkCreateDevice),
               "vkCreateDevice signature");

extern __typeof__(vkCreateGraphicsPipelines) __real_vkCreateGraphicsPipelines;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                               const VkGraphicsPipelineCreateInfo *info,
                                                               const VkAllocationCallbacks *alloc, VkPipeline *pipes) {
    const VkResult result = __real_vkCreateGraphicsPipelines(device, cache, count, info, alloc, pipes);
    if (result == VK_SUCCESS) creates += count;
    return result;
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateGraphicsPipelines), PFN_vkCreateGraphicsPipelines),
               "vkCreateGraphicsPipelines signature");

static unsigned no_sample_sets(void) { return 0; }

int main(int argc, char **argv) {
    REQUIRE(argc == 6);
    dq_mode = atoi(argv[2]);
    observed_sample_sets = no_sample_sets;
    const unsigned expected = (unsigned)atoi(argv[3]);
    const unsigned requested = (unsigned)atoi(argv[5]);
    hide_features = getenv("S2A_TEST_HIDE_EDS_FEATURES") != NULL;
    struct s2a_vk_device device;
    char why[256] = {0};
    int create_rc = s2a_vk_device_create(&device, why, sizeof why);
    if (create_rc) fprintf(stderr, "device: %s\n", why);
    REQUIRE(!create_rc);
    REQUIRE(queried_dynamic == requested);
    const unsigned enabled = device.eds1_enabled | (device.eds2_enabled << 1) | (device.vertex_input_dynamic_enabled << 2);
    REQUIRE(enabled == (hide_features ? 0 : requested));
    nvn_gpu_open_desc open = {.host_device = &device, .app_label = "dynamic-state"};
    GPU(nvn_gpu_open(&open, &err));

    nvn_gpu_shader vs = shader(argv[1], "dynamic_input.vert", NVN_GPU_SH_VS, NULL);
    nvn_gpu_shader fs = shader(argv[1], "solid_green.frag", NVN_GPU_SH_FS, NULL);
    nvn_gpu_pipe_desc base = {
        .cfmt = {NVN_FORMAT_RGBA8}, .samples = 1, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES,
        .dfmt = NVN_FORMAT_DEPTH24_STENCIL8, .sfmt = NVN_FORMAT_DEPTH24_STENCIL8,
        .poly = {.mode = NVN_POLYGON_MODE_FILL}, .color = {.logic_op = NVN_LOGIC_OP_COPY},
        .chmask = {.mask = 15}, .attr = {{.fmt = NVN_FORMAT_RG32F}},
        .stream = {{.stride = 16}},
    };
    base.st[NVN_GPU_SH_VS].sh = vs;
    base.st[NVN_GPU_SH_FS].sh = fs;

    nvn_gpu_tex_desc td = {
        .d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
              .width = WIDTH, .height = TILE, .depth = 1, .levels = 1},
        .usage = NVN_GPU_TEX_RENDER,
    };
    nvn_gpu_tex tex;
    nvn_gpu_view view;
    GPU(nvn_gpu_tex_create(&td, &tex, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &view, &err));
    td.d.format = base.dfmt;
    nvn_gpu_tex ds;
    nvn_gpu_view ds_view;
    GPU(nvn_gpu_tex_create(&td, &ds, &err));
    GPU(nvn_gpu_view_create(ds, NULL, &ds_view, &err));
    nvn_gpu_buf vb, rb;
    GPU(nvn_gpu_buf_create(256, NVN_GPU_MEM_SHARED, NULL, &vb, &err));
    GPU(nvn_gpu_buf_create(BYTES, NVN_GPU_MEM_SHARED, NULL, &rb, &err));
    const float positions[3][2] = {{-1, -1}, {3, -1}, {-1, 3}};
    const uint16_t half[3][2] = {{0xbc00, 0xbc00}, {0x4200, 0xbc00}, {0xbc00, 0x4200}};
    for (unsigned v = 0; v < 3; v++) {
        memcpy(nvn_gpu_buf_cpu(vb) + v * 16, positions[v], sizeof positions[v]);
        memcpy(nvn_gpu_buf_cpu(vb) + 64 + v * 32, positions[v], sizeof positions[v]);
        memcpy(nvn_gpu_buf_cpu(vb) + 160 + v * 16, half[v], sizeof half[v]);
    }

    const uint64_t before = nvn_vk_dbg_pipe_stat(1);
    // Feed the unchanged on-disk recipe through the actual warm loader. The
    // first draw uses a different cull mode, even when it reuses this PSO.
    nvn_gpu_pipe_desc warm = base;
    warm.poly.cull = NVN_FACE_FRONT_AND_BACK;
    warm_program.sh[NVN_GPU_SH_VS] = vs;
    warm_program.sh[NVN_GPU_SH_FS] = fs;
    struct nvk_pipe_rec record;
    nvk_pipe_warm_rec_of(&warm, warm_identity, &record);
    const uint64_t warm_before = nvn_gpu_pipe_warm_created();
    const uint64_t hits_before = nvn_gpu_pipe_warm_hits();
    REQUIRE(!nvk_pipe_warm_build(&record));
    REQUIRE(!warm_borrowed && nvn_gpu_pipe_warm_created() == warm_before + 1);
    nvn_gpu_cmd cmd;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    nvn_gpu_pass_desc pass = {
        .ncolor = 1, .color = {{.view = view, .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE}},
        .depth = {.view = ds_view, .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE},
        .stencil = {.view = ds_view, .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE},
        .clear_depth = 0.25f, .clear_stencil = 1,
        .w = WIDTH, .h = TILE, .layers = 1,
    };
    pass.color[0].clear.f[3] = 1;
    nvn_gpu_pass_begin(cmd, &pass);
    for (unsigned i = 0; i < DRAWS; i++) {
        nvn_gpu_pipe_desc pd = base;
        uint64_t offset = 0;
        switch (i) {
        case 1: pd.poly.cull = NVN_FACE_FRONT_AND_BACK; break;
        case 2: pd.poly.offset_en = 1u << NVN_POLYGON_MODE_FILL; break;
        case 3: pd.stream[0].stride = 32; offset = 64; break;
        case 4: pd.attr[0].fmt = NVN_FORMAT_RG16F; offset = 160; break;
        case 5: pd.prim = NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP; break;
        case 6: pd.ds.dt = pd.ds.dw = 1; pd.ds.df = NVN_DEPTH_FUNC_LESS; break;
        case 7: pd.ds.st = 1; pd.ds.sf_f = pd.ds.sf_b = NVN_STENCIL_FUNC_NEVER; break;
        case 8: pd.rast_discard = 1; break;
        case 9: pd.poly.cull = NVN_FACE_BACK; pd.poly.front = NVN_FRONT_FACE_CW; break;
        case 10: pd.poly.cull = NVN_FACE_BACK; pd.poly.front = NVN_FRONT_FACE_CCW; break;
        case 11: pd.prim = NVN_DRAW_PRIMITIVE_LINES; break;
        default: break;
        }
        nvn_gpu_pipe pipe = NULL;
        GPU(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &err));
        if (i == 0) REQUIRE(nvn_gpu_pipe_warm_hits() == hits_before + ((enabled & 1) != 0));
        if (nvn_vk_set_draw_pipe) nvn_vk_set_draw_pipe(cmd, pipe, &pd);
        else nvn_gpu_set_pipe(cmd, pipe);
        nvn_gpu_set_vbuf(cmd, 0, vb, offset);
        nvn_gpu_dyn dyn = {.nvp = 1, .has_sc = 1, .sc = {(int)i * TILE, 0, TILE, TILE}};
        dyn.vp[0][2] = WIDTH;
        dyn.vp[0][3] = TILE;
        dyn.vp[0][5] = 1;
        nvn_gpu_set_dyn(cmd, &dyn);
        // A single vertex under line-list topology cannot rasterize a line.
        const nvn_gpu_draw_args draw = {.prim = pd.prim, .count = i == 11 ? 1 : 3, .instances = 1};
        nvn_gpu_draw(cmd, &draw);
    }
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, NULL, rb, 0, WIDTH * 4, BYTES);
    GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    REQUIRE(observed_draws == DRAWS);
    const uint8_t *pixels = nvn_gpu_buf_cpu(rb);
    for (unsigned y = 0; y < TILE; y++) {
        for (unsigned x = 0; x < WIDTH; x++) {
            const unsigned tile = x / TILE;
            const unsigned empty = (1u << 1 | 1u << 6 | 1u << 7 | 1u << 8 | 1u << 9 | 1u << 11);
            const uint8_t want[4] = {0, (empty >> tile & 1) ? 0 : 255, 0, 255};
            REQUIRE(!memcmp(pixels + (y * WIDTH + x) * 4, want, sizeof want));
        }
    }
    FILE *out = fopen(argv[4], "wb");
    REQUIRE(out && fwrite(pixels, 1, BYTES, out) == BYTES && !fclose(out));
    const unsigned created = (unsigned)(nvn_vk_dbg_pipe_stat(1) - before);
    REQUIRE(creates == created && nvn_gpu_pipe_warm_hits() == hits_before + 1);
    printf("dynamic state: dq=%d requested=%u enabled=%u warm-hits=1 draws=%u bytes=%u pso=%u expected=%u\n",
           dq_mode, requested, enabled, DRAWS, BYTES, creates, expected);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_view_release(view);
    nvn_gpu_tex_release(tex);
    nvn_gpu_view_release(ds_view);
    nvn_gpu_tex_release(ds);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
    GPU(nvn_gpu_close(5000000000ull, &err));
    s2a_vk_device_destroy(&device, 1);
    if (created != expected) {
        fprintf(stderr, "【红】dynamic state PSO count: expected=%u actual=%u\n", expected, created);
        return 1;
    }
    return 0;
}
