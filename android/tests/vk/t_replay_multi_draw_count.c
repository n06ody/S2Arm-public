// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// OP_MULTI_DRAW 解码后的真实回放。第 1025 笔是绿色，计数之外的第 1026 笔是蓝色。
#define main indirect_fixture_main
#define rp_pool_find indirect_fixture_pool_find
#define rp_perf_add indirect_fixture_perf_add
#define rp_seg_serial indirect_fixture_seg_serial
#define rp_sub_serial indirect_fixture_sub_serial
#define rp_sub_fence indirect_fixture_sub_fence
#define rp_fence_done indirect_fixture_fence_done
#define rp_xfer_cmd indirect_fixture_xfer_cmd
#define rp_occl_draw indirect_fixture_occl_draw
#define rp_post_gpu_write indirect_fixture_post_gpu_write
#define __wrap_nvn_gpu_draw indirect_fixture_wrapped_draw
#include "t_replay_indirect.c"
#undef rp_pool_find
#undef rp_perf_add
#undef rp_seg_serial
#undef rp_sub_serial
#undef rp_sub_fence
#undef rp_fence_done
#undef rp_xfer_cmd
#undef rp_occl_draw
#undef rp_post_gpu_write
#undef __wrap_nvn_gpu_draw
#undef main
#include "nvn_mtl.h"
#include "nvn_plat.h"

enum {
    ARG_OFF = 32, COUNT_OFF = 16, ARG_STRIDE = 32, MAX_DRAWS = 1026,
    VERT_OFF = 33792, INDEX_OFF = 34816, PIXEL_BYTES = 64 * 64 * 4,
};
static alignas(4096) uint8_t range_guest[9 * 4096];
static int hide_count;
static int observe_clamp;
static nvn_gpu_buf clamp_count;
static uint64_t clamp_count_off;
static unsigned main_segments;
static atomic_uint array_calls, indexed_calls;
static PFN_vkCmdDrawIndirectCountKHR native_arrays;
static PFN_vkCmdDrawIndexedIndirectCountKHR native_indexed;

void __wrap_nvn_gpu_draw(nvn_gpu_cmd c, const nvn_gpu_draw_args *args) {
    if (observe_clamp && args->cnt) {
        clamp_count = args->cnt;
        clamp_count_off = args->cnt_off;
    }
    indirect_fixture_wrapped_draw(c, args);
}

nvn_gpu_rc __real_nvn_gpu_cmd_begin(const nvn_gpu_cmd_desc *, nvn_gpu_cmd *, nvn_gpu_err *);
nvn_gpu_rc __wrap_nvn_gpu_cmd_begin(const nvn_gpu_cmd_desc *desc, nvn_gpu_cmd *cmd, nvn_gpu_err *error) {
    nvn_gpu_rc rc = __real_nvn_gpu_cmd_begin(desc, cmd, error);
    // 实际 rp_exec.c 拥有这些段。夹具只取主段给受测 draw 和拷出，不接管栅栏。
    if (nvn_gpu_ok(rc) && desc && desc->label && !strcmp(desc->label, "nvn submit")) {
        active = *cmd;
        main_segments++;
    }
    return rc;
}

void rp_pass_end(nvn_gpu_cmd c) { rp_pass_flush(c); }
const char *nvn_plat_dir(int which) { (void)which; return NULL; }
void rp_res_xfer_aborted(uint64_t sub) { (void)sub; REQUIRE(0); }
int s2a_guest_tick_from_host_ns(uint64_t ns, uint64_t *out) { (void)ns; (void)out; REQUIRE(0); return 0; }
uint64_t s2a_guest_tick(void) { REQUIRE(0); return 0; }
int s2a_nvn_render_cond(void) { REQUIRE(0); return 0; }

static VKAPI_ATTR void VKAPI_CALL record_arrays(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off,
        VkBuffer count, VkDeviceSize count_off, uint32_t max, uint32_t stride) {
    atomic_fetch_add(&array_calls, 1);
    native_arrays(cb, b, off, count, count_off, max, stride);
}
static VKAPI_ATTR void VKAPI_CALL record_indexed(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off,
        VkBuffer count, VkDeviceSize count_off, uint32_t max, uint32_t stride) {
    atomic_fetch_add(&indexed_calls, 1);
    native_indexed(cb, b, off, count, count_off, max, stride);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __real_vkGetDeviceProcAddr(VkDevice, const char *);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice device, const char *name) {
    PFN_vkVoidFunction fn = __real_vkGetDeviceProcAddr(device, name);
    if (fn && !strcmp(name, "vkCmdDrawIndirectCountKHR")) {
        native_arrays = (PFN_vkCmdDrawIndirectCountKHR)fn;
        return (PFN_vkVoidFunction)record_arrays;
    }
    if (fn && !strcmp(name, "vkCmdDrawIndexedIndirectCountKHR")) {
        native_indexed = (PFN_vkCmdDrawIndexedIndirectCountKHR)fn;
        return (PFN_vkVoidFunction)record_indexed;
    }
    return fn;
}

int rp_pool_find(uint64_t addr, uint64_t len, const uint8_t **mem, uint64_t *size) {
    const uint64_t base = (uintptr_t)range_guest;
    if (addr >= base && len <= sizeof range_guest && addr - base <= sizeof range_guest - len) {
        if (mem) *mem = range_guest;
        if (size) *size = sizeof range_guest;
        return 1;
    }
    return indirect_fixture_pool_find(addr, len, mem, size);
}

VKAPI_ATTR VkResult VKAPI_CALL __real_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical, const char *layer, uint32_t *count, VkExtensionProperties *out);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical, const char *layer, uint32_t *count, VkExtensionProperties *out) {
    if (!hide_count || layer) return __real_vkEnumerateDeviceExtensionProperties(physical, layer, count, out);
    VkExtensionProperties props[512];
    uint32_t available = sizeof props / sizeof props[0];
    VkResult rc = __real_vkEnumerateDeviceExtensionProperties(physical, layer, &available, props);
    if (rc != VK_SUCCESS) return rc;
    uint32_t visible = 0;
    for (uint32_t i = 0; i < available; i++) {
        if (!strcmp(props[i].extensionName, VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME)) continue;
        props[visible++] = props[i];
    }
    if (!out) { *count = visible; return VK_SUCCESS; }
    const uint32_t written = *count < visible ? *count : visible;
    memcpy(out, props, written * sizeof *out);
    *count = written;
    return written < visible ? VK_INCOMPLETE : VK_SUCCESS;
}

struct count_case {
    const char *name;
    int prim, itype, slow_segments;
    unsigned max, count, arg_off, stride, expect_khr;
};

static unsigned count_offset(const struct count_case *test) {
    return COUNT_OFF + (test->arg_off & 3);
}

static void prepare_guest(const struct count_case *test, uint8_t restart) {
    memset(range_guest, 0, sizeof range_guest);
    uint32_t *words = (void *)range_guest;
    words[0] = test->count;
    words[1] = count_offset(test);
    const uint32_t poison = test->count ? 0 : MAX_DRAWS;
    memcpy(range_guest + count_offset(test), &poison, sizeof poison);
    const unsigned records[3] = {0, 1024, 1025};
    const unsigned corners[6] = {0, 1, 2, 2, 1, 3};
    const unsigned quad_corners[4] = {0, 1, 3, 2};
    const int quad = test->prim == NVN_DRAW_PRIMITIVE_QUADS;
    const unsigned vertices_per_draw = quad ? 4 : 6;
    float (*vertices)[8] = (void *)(range_guest + VERT_OFF);
    uint32_t *indices = (void *)(range_guest + INDEX_OFF);
    for (unsigned region = 0; region < 3; region++) {
        const uint32_t arg[5] = {restart ? 9u : vertices_per_draw, 1, region * (restart ? 9u : 6u), 0, 0};
        memcpy(range_guest + test->arg_off + records[region] * test->stride, arg, test->itype < 0 ? 16 : 20);
        for (unsigned j = 0; j < vertices_per_draw; j++) {
            const unsigned k = quad ? quad_corners[j] : corners[j], i = region * 6 + j;
            vertices[i][0] = -1 + region * 0.5f + ((k & 2) ? 0.5f : 0);
            vertices[i][1] = (k & 1) ? 1 : -1;
            vertices[i][3] = vertices[i][4 + region] = vertices[i][7] = 1;
            if (test->itype == NVN_INDEX_TYPE_UNSIGNED_BYTE) {
                if (!restart) range_guest[INDEX_OFF + i] = (uint8_t)i;
            }
            else indices[i] = i;
        }
        if (restart) {
            const uint8_t first = (uint8_t)(region * 6);
            const uint8_t strip[9] = {first, restart, first, first + 1, first + 2,
                                      restart, first + 3, first + 4, first + 5};
            memcpy(range_guest + INDEX_OFF + region * sizeof strip, strip, sizeof strip);
        }
    }
}

static void run_count_range(struct s2a_vk_device *device, const char *dir, const struct count_case *test, uint8_t restart) {
    nvn_gpu_open_desc open = {.host_device = device, .app_label = "multi-draw-count-range"};
    GPU(nvn_gpu_open(&open, &err));
    prepare_guest(test, restart);

    memset(&prog, 0, sizeof prog);
    prog.sh[0] = shader(dir, "quad.vert", NVN_GPU_SH_VS, NULL);
    prog.sh[1] = shader(dir, "quad.frag", NVN_GPU_SH_FS, NULL);
    prog.attr_mask = 3;
    prog.color_out_mask = 15;
    nvn_gpu_layout_ent ent = {.cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_RW,
                              .stages = 1u << NVN_GPU_SH_CS, .count = 1};
    nvn_gpu_layout layout = {.ent = &ent, .nent = 1};
    nvn_gpu_shader cs = shader(dir, "count_range.comp", NVN_GPU_SH_CS, &layout);
    nvn_gpu_stage_desc stage = {.sh = cs};
    nvn_gpu_pipe producer = NULL;
    GPU(nvn_gpu_cpipe_get(&stage, NVN_GPU_PIPE_SYNC, &producer, &err));
    nvn_gpu_shader_release(cs);

    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 64;
    td.d.depth = td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER;
    nvn_gpu_tex tex = NULL;
    GPU(nvn_gpu_tex_create(&td, &tex, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &target_view, &err));
    nvn_gpu_buf rb = buffer(PIXEL_BYTES + sizeof(uint32_t), NULL);
    clamp_count = NULL;
    target_tex = (tex_t){.d = td.d, .serial = 1};
    target = (p_set_rt){.n = 1, .color = {&target_tex}};

    const uint64_t base = (uintptr_t)range_guest;
    rp_state_reset();
    rp_draw_reset();
    nvn_unimpl_reset();
    rp_S.nattr = 2;
    rp_S.nstream = 1;
    rp_S.attr[0].fmt = rp_S.attr[1].fmt = NVN_FORMAT_RGBA32F;
    rp_S.attr[1].offset = 16;
    rp_S.stream[0].stride = 32;
    rp_S.vbuf[0].address = base + VERT_OFF;
    rp_S.vbuf[0].size = 18 * 32;
    rp_S.color.logic_op = 3;
    rp_S.chmask.mask = 15;
    rp_S.poly.mode = NVN_POLYGON_MODE_FILL;

    active = NULL;
    main_segments = 0;
    const unsigned prev_arrays = atomic_load(&array_calls), prev_indexed = atomic_load(&indexed_calls);
    nvn_be_sub_begin();
    REQUIRE(active);
    nvn_gpu_buf mirror = NULL;
    uint64_t off = 0;
    REQUIRE(rp_buf_resolve(active, base, COUNT_OFF + 8, &mirror, &off));
    const uint64_t written_bytes = (count_offset(test) & 3) ? 8 : 4;
    REQUIRE(rp_post_gpu_write_prefix(base + COUNT_OFF, written_bytes, written_bytes));
    nvn_gpu_binding ssbo = {.cls = NVN_GPU_RES_SSBO};
    ssbo.u.b.buf = mirror;
    ssbo.u.b.off = off;
    ssbo.u.b.len = COUNT_OFF + 8;
    nvn_gpu_set_pipe(active, producer);
    nvn_gpu_bind(active, NVN_GPU_SH_CS, &ssbo, 1);
    nvn_gpu_dispatch(active, producer, 1, 1, 1, NULL, 0);
    nvn_gpu_barrier(active, NVN_BARRIER_ORDER_INDIRECT_DATA_BIT | NVN_BARRIER_INVALIDATE_SHADER_BIT);

    pass(active, 1);
    rp_pass_flush(active);
    const p_multi_draw draw = {.ind = base + test->arg_off, .cnt = base + count_offset(test),
                               .ibuf = test->itype >= 0 ? base + INDEX_OFF : 0, .stride = test->stride,
                               .max = (int32_t)test->max, .prim = test->prim, .itype = test->itype};
    rp_S.restart.on = restart != 0;
    rp_S.restart.index = restart;
    rp_multi_draw(active, &draw, &draw);
    rp_pass_flush(active);
    nvn_gpu_copy_tex_buf(active, tex, 0, 0, NULL, rb, 0, 256, PIXEL_BYTES);
    if (observe_clamp && !hide_count) {
        REQUIRE(clamp_count && (clamp_count != mirror || clamp_count_off != off + count_offset(test)));
        nvn_gpu_copy_buf(active, clamp_count, clamp_count_off, rb, PIXEL_BYTES, sizeof(uint32_t));
    }
    const uint64_t fence = nvn_be_sub_commit();
    GPU(nvn_gpu_fence_wait(fence, 5000000000ull, &err));

    uint32_t result_count = 0;
    memcpy(&result_count, (const uint8_t *)nvn_gpu_buf_cpu(mirror) + off + count_offset(test), sizeof result_count);
    REQUIRE(result_count == test->count);
    const uint8_t *out = nvn_gpu_buf_cpu(rb);
    if (observe_clamp && !hide_count) {
        uint32_t sent;
        memcpy(&sent, out + PIXEL_BYTES, sizeof sent);
        REQUIRE(sent == test->max);
        printf("multi-draw-count finite-limit: GPU count %u, actual KHR count %u\n", result_count, sent);
    }
    unsigned bad[4] = {0};
    for (unsigned region = 0; region < 4; region++) {
        uint8_t expected[4] = {0, 0, 0, 255};
        if (test->count && region < 2) expected[region] = 255;
        for (unsigned y = 2; y < 62; y++) for (unsigned x = 2; x < 14; x++) {
            if (memcmp(out + 4 * (y * 64 + region * 16 + x), expected, sizeof expected)) bad[region]++;
        }
    }
    const unsigned calls = atomic_load(&array_calls) - prev_arrays + atomic_load(&indexed_calls) - prev_indexed;
    printf("multi-draw-count %s %s KHR=%d calls=%u segments=%u GPU-count=%u max=%u bad first=%u draw1025=%u beyond-count=%u clear=%u\n",
           test->name, test->itype >= 0 ? "elements" : "arrays", !hide_count, calls, main_segments,
           result_count, test->max, bad[0], bad[1], bad[2], bad[3]);
    failures += bad[0] + bad[1] + bad[2] + bad[3];
    if (calls != (hide_count ? 0 : test->expect_khr) || main_segments != (unsigned)(1 + test->slow_segments)) failures++;

    rp_draw_reset();
    rp_buf_reset();
    nvn_gpu_shader_release(prog.sh[0]);
    nvn_gpu_shader_release(prog.sh[1]);
    nvn_gpu_view_release(target_view);
    nvn_gpu_tex_release(tex);
    nvn_gpu_buf_release(rb, NULL, NULL);
    GPU(nvn_gpu_close(5000000000ull, &err));
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2 || (argc == 3 && !strcmp(argv[2], "--hide-khr")));
    hide_count = argc == 3;
    struct s2a_vk_device device;
    char why[256];
    if (s2a_vk_device_create(&device, why, sizeof why)) {
        fprintf(stderr, "multi-draw-count device: %s\n", why);
        return 2;
    }
    REQUIRE(device.draw_indirect_count_enabled == !hide_count);
    REQUIRE(device.properties.limits.maxDrawIndirectCount >= MAX_DRAWS);
    printf("multi-draw-count device=%s DQ=%d\n", device.properties.deviceName, !!getenv("S2A_REPLAY_INDIRECT_DQ"));
    const struct count_case cases[] = {
        {"native", NVN_DRAW_PRIMITIVE_TRIANGLES, -1, 0, MAX_DRAWS, 1025, ARG_OFF, ARG_STRIDE, 1},
        {"zero", NVN_DRAW_PRIMITIVE_TRIANGLES, -1, 0, MAX_DRAWS, 0, ARG_OFF, ARG_STRIDE, 1},
        {"native", NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_INT, 0, MAX_DRAWS, 1025, ARG_OFF, ARG_STRIDE, 1},
        {"zero", NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_INT, 0, MAX_DRAWS, 0, ARG_OFF, ARG_STRIDE, 1},
        {"byte-stride", NVN_DRAW_PRIMITIVE_TRIANGLES, -1, 0, MAX_DRAWS, 1025, ARG_OFF + 1, 31, 0},
        {"byte-stride", NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_INT, 0, MAX_DRAWS, 1025, ARG_OFF + 1, 31, 0},
        {"huge-max", NVN_DRAW_PRIMITIVE_TRIANGLES, -1, 2, INT32_MAX, 1025, ARG_OFF, ARG_STRIDE, 0},
        {"huge-max", NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_INT, 3, INT32_MAX, 1025, ARG_OFF, ARG_STRIDE, 0},
        {"quads", NVN_DRAW_PRIMITIVE_QUADS, -1, 2, MAX_DRAWS, 1025, ARG_OFF, ARG_STRIDE, 0},
        {"quads", NVN_DRAW_PRIMITIVE_QUADS, NVN_INDEX_TYPE_UNSIGNED_INT, 3, MAX_DRAWS, 1025, ARG_OFF, ARG_STRIDE, 0},
        {"u8", NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_BYTE, 3, MAX_DRAWS, 1025, ARG_OFF, ARG_STRIDE, 0},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) run_count_range(&device, argv[1], &cases[i], 0);
    REQUIRE(device.primitive_topology_list_restart_enabled);
    const struct count_case restart = {
        "u8-list-restart", NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_BYTE,
        3, MAX_DRAWS, 1025, ARG_OFF, ARG_STRIDE, 0,
    };
    run_count_range(&device, argv[1], &restart, 15);
    // 缩小后端的能力快照，模拟实际设备上限较小；仍用真实 KHR 和 GPU 拷回核对新计数。
    const uint32_t physical_limit = device.properties.limits.maxDrawIndirectCount;
    device.properties.limits.maxDrawIndirectCount = MAX_DRAWS;
    observe_clamp = 1;
    const struct count_case limited = {
        "finite-limit", NVN_DRAW_PRIMITIVE_TRIANGLES, -1, 0, 1025, 4096, ARG_OFF, ARG_STRIDE, 1,
    };
    run_count_range(&device, argv[1], &limited, 0);
    device.properties.limits.maxDrawIndirectCount = physical_limit;
    const uint64_t dq = nvn_vk_dbg_dq_recs();
    REQUIRE(getenv("S2A_REPLAY_INDIRECT_DQ") ? dq > 0 : dq == 0);
    REQUIRE(!s2a_vk_device_destroy(&device, 1));
    printf("multi-draw-count pixel-failures=%u\n", failures);
    return failures ? 1 : 0;
}
