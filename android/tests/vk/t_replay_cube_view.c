// SPDX-License-Identifier: Apache-2.0
// 声明与登记目标相同时，用真实回放资源、Vulkan 视图和采样读回检查立方体的范围。
#define main backend_fixture_main
#define nvn_plat_flag fixture_base_flag
#include "t_backend_vk.c"
#undef main
#undef nvn_plat_flag
#include "rp_exec.h"
#include "rp_res.h"
#include "nvn_tex.h"
#include "nvn_dirty.h"
#include "host/dm.h"
#include <sys/mman.h>
#include <unistd.h>

static void (*prepare_write)(void *, size_t);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { CHECK(fn != NULL, "dirty fault hook"); }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { prepare_write = fn; }

static int fixture_sparse_native;
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_sparse_native_mb")) return fixture_sparse_native < 0 ? -1 : fixture_sparse_native ? 0 : dflt;
    return fixture_base_flag(name, dflt);
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

VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateImageView(
    VkDevice device, const VkImageViewCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImageView *view);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateImageView(
    VkDevice device, const VkImageViewCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImageView *view) {
    const VkResult rc = __real_vkCreateImageView(device, info, allocator, view);
    if (rc == VK_SUCCESS) {
        seen_type = info->viewType;
        seen_base = info->subresourceRange.baseArrayLayer;
        seen_count = info->subresourceRange.layerCount;
    }
    return rc;
}

static uint64_t fixture_signal;
static void finish_expected(nvn_gpu_cmd cmd, int want) {
    const uint64_t signal = ++fixture_signal;
    nvn_gpu_err err = {0};
    const nvn_gpu_rc result = nvn_gpu_cmd_commit(cmd, signal, on_done, NULL, &err);
    CHECK(result.code == want, "submit code=%d expected=%d: %s", result.code, want, err.why);
    if (want == NVN_GPU_OK) CHECK(OK(nvn_gpu_fence_wait(signal, 5000000000ull, &err)), "wait: %s", err.why);
    else CHECK(strstr(err.why, "sparse_snapshot_write"), "sparse write must give a named rejection: %s", err.why);
}
static void finish(nvn_gpu_cmd cmd) { finish_expected(cmd, NVN_GPU_OK); }

static void sample(const char *dir, const char *frag, nvn_gpu_view sv, int format, void *pixels) {
    const size_t row = 64 * (format == NVN_FORMAT_RG32F ? 8 : 4);
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
        .cfmt[0] = format, .samples = 1, .prim = 4, .chmask.mask = 15,
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
    nvn_gpu_buf rb = make_buf(row * 64, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_tex out = make_tex(64, 64, format, NVN_GPU_TEX_RENDER);
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
    nvn_gpu_copy_tex_buf(active, out, 0, 0, NULL, rb, 0, row, row * 64);
    finish(active);
    active = NULL;
    memcpy(pixels, buf_cpu(rb), row * 64);
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
    sample(dir, frag, view, NVN_FORMAT_RGBA8, pixels);
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

// Read back the fragment output in the source format so exposure above 1 is
// preserved. Each CPU store uses the write-preparation hook from nvn_dirty.
static void sample_mixed_pool(const char *dir) {
    const size_t page = (size_t)getpagesize(), size = 3 * page;
    uint8_t *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mem != MAP_FAILED, "mixed pool mmap");
    if (mem == MAP_FAILED) return;
    pool_t pool = {.mem = mem, .size = size, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    nvn_dirty_add_range(mem, size);
    nvn_be_pool_gpu_written(&pool);
    tex_t rt = {.serial = 100, .pool = &pool, .mem = mem,
                .d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
                      .width = 1, .height = 1, .depth = 1, .levels = 1}};
    nvn_tex_layout(&rt.d, &rt.L);
    active = begin_cmd();
    nvn_gpu_pass_desc pass = {.ncolor = 1, .w = 1, .h = 1, .layers = 1};
    pass.color[0].view = rp_view_for_attach(&rt, NULL, active);
    pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[3] = 1;
    CHECK(pass.color[0].view != NULL, "mixed pool color target");
    if (pass.color[0].view) {
        nvn_gpu_pass_begin(active, &pass);
        nvn_gpu_pass_end(active);
        rp_tex_mark_gpu_written(&rt);
    }
    finish(active);
    const float exposure[][2] = {{1.175f, 0.880f}, {0.625f, 1.25f}};
    const uint8_t color[][4] = {{51, 102, 153, 255}, {16, 32, 64, 128}};
    for (int kind = 0; kind < 2; ++kind) {
        tex_t src = rt;
        src.serial = 101 + kind; src.off = page * (kind + 1); src.mem = mem + src.off;
        src.d.format = kind == 0 ? NVN_FORMAT_RG32F : NVN_FORMAT_RGBA8;
        src.d.flags = NVN_TEXTURE_FLAGS_LINEAR; src.d.stride = 32;
        nvn_tex_layout(&src.d, &src.L);
        const size_t bytes = kind == 0 ? sizeof exposure[0] : sizeof color[0];
        for (int phase = 0; phase < 2; ++phase) {
            const void *want = kind == 0 ? (const void *)exposure[phase] : (const void *)color[phase];
            CHECK(prepare_write != NULL, "mixed pool write preparation registered");
            if (!prepare_write) break;
            prepare_write(src.mem, bytes);
            memcpy(src.mem, want, bytes);
            active = begin_cmd();
            nvn_gpu_view view = rp_view_for(&src, NULL, active);
            CHECK(view != NULL, "mixed pool sample view");
            if (!view) { finish(active); break; }
            uint8_t pixels[64 * 64 * 8];
            sample(dir, "tri.frag", view, src.d.format, pixels);
            unsigned matched = 0;
            for (size_t i = 0; i < 64 * 64; ++i) matched += !memcmp(pixels + i * bytes, want, bytes);
            CHECK(matched == 4096, "mixed pool format=%u phase=%d pixels=%u/4096", src.d.format, phase, matched);
            printf("mixed pool format=%u phase=%d pixels=%u/4096\n", src.d.format, phase, matched);
        }
    }
    rp_res_reset();
    nvn_dirty_remove_range(mem); munmap(mem, size);
}
#include "sparse_residency_test.h"

// Independently exercise source decoding, destination encoding, and integer
// clear through views of UNORM storage. Neighbor layers carry a sentinel.
static void check_override_writer(const char *which) {
    const int clear = !strcmp(which, "clear"), decode = !strcmp(which, "decode");
    const uint32_t usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_VIEW_FMT | (clear ? NVN_GPU_TEX_STORAGE : 0);
    nvn_gpu_tex src = make_tex(2, 2, NVN_FORMAT_RGBA8, usage);
    nvn_gpu_tex dst = make_tex_target(2, 2, 2, NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8, usage);
    const uint8_t gray[16] = {128,128,128,61,128,128,128,61,128,128,128,61,128,128,128,61};
    uint8_t seed[16]; memset(seed, 0xa3, sizeof seed);
    const view_t src_type = {.set_format = decode, .format = NVN_FORMAT_RGBA8_SRGB};
    const view_t dst_type = {.set_format = !decode, .format = clear ? NVN_FORMAT_RGBA8UI : NVN_FORMAT_RGBA8_SRGB,
                             .set_layers = 1, .min_layer = 1, .nlayers = 1};
    nvn_gpu_err err = {0};
    nvn_gpu_view sv = NULL, dv = NULL;
    CHECK(OK(nvn_gpu_view_create(src, &src_type, &sv, &err)), "source view: %s", err.why);
    CHECK(OK(nvn_gpu_view_create(dst, &dst_type, &dv, &err)), "destination view: %s", err.why);
    nvn_gpu_buf read = make_buf(32, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, src, 0, 0, NULL, gray, 8, 16);
    CHECK(nvn_gpu_cmd_record_ok(c), "source upload");
    for (unsigned l = 0; l < 2; l++) nvn_gpu_tex_upload(c, dst, 0, l, NULL, seed, 8, 16);
    CHECK(nvn_gpu_cmd_record_ok(c), "dest upload");
    const uint32_t ints[4] = {19, 37, 71, 131};
    const NVNcopyRegion box = {0,0,0,2,2,1};
    if (clear) nvn_gpu_clear_view(c, dv, NULL, ints, 1);
    else nvn_gpu_blit(c, sv, &box, dv, &box, 0);
    for (unsigned l = 0; l < 2; l++) nvn_gpu_copy_tex_buf(c, dst, 0, l, NULL, read, l * 16, 8, 16);
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &err);
    CHECK(OK(rc), "override %s submit: %d %s", which, rc.code, err.why);
    if (OK(rc)) {
        rc = nvn_gpu_fence_wait(1, 5000000000ull, &err);
        CHECK(OK(rc), "override %s wait: %s", which, err.why);
    }
    if (OK(rc)) {
        const uint8_t *buf = buf_cpu(read);
        unsigned wrong = 0, neighbor = 0;
        for (unsigned i = 0; i < 16; i++) {
            int expected = clear ? (int)ints[i % 4] : i % 4 == 3 ? 61 : decode ? 55 : 188;
            wrong += abs((int)buf[16 + i] - expected) > (!clear && i % 4 != 3);
            neighbor += buf[i] != seed[i];
        }
        printf("override %s data_errors=%u/16 neighbor_errors=%u/16\n", which, wrong, neighbor);
        CHECK(!wrong && !neighbor, "override %s wrong pixel bytes", which);
    }
    nvn_gpu_buf_release(read, NULL, NULL);
    if (sv) nvn_gpu_view_release(sv);
    if (dv) nvn_gpu_view_release(dv);
    nvn_gpu_tex_release(src);
    nvn_gpu_tex_release(dst);
}

int main(int argc, char **argv) {
    const int writer = (argc == 5 || argc == 6) && !strcmp(argv[2], "sparse-write");
    const int override = argc == 4 && !strcmp(argv[2], "override");
    if (argc != 2 && !writer && !override) return 2;
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    g_desc = (nvn_gpu_open_desc){.host_device = &g_dev, .app_label = "replay-cube-view"};
    if (!open_backend()) return 2;
    if (override) {
        check_override_writer(argv[3]);
        nvn_gpu_err err = {0};
        CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "override close: %s", err.why);
        s2a_vk_device_destroy(&g_dev, 1);
        return g_fail ? 1 : 0;
    }
    if (writer) {
        const int native = atoi(argv[4]);
        if (!native || sparse_fixture_probe()) {
            if (argc == 6 && (!strcmp(argv[3], "attachment") || !strcmp(argv[3], "storage")))
                check_sparse_pass_writer(argv[1], argv[3], native, argv[5]);
            else if (argc == 5)
                check_sparse_writer(argv[3], native);
            else CHECK(0, "unknown sparse writer case");
        }
        else CHECK(0, "required native sparse support missing");
        nvn_gpu_err err = {0};
        CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "sparse writer close: %s", err.why);
        s2a_vk_device_destroy(&g_dev, 1);
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
    sample_mixed_pool(argv[1]);
    rp_res_reset();
    const int sparse_native_supported = sparse_fixture_probe();
    check_sparse_residency(0);
    if (sparse_native_supported) check_sparse_residency(1);
    check_large_sparse(sparse_native_supported);
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "close: %s", err.why);
    s2a_vk_device_destroy(&g_dev, 1);
    printf("replay cube view failures=%d\n", g_fail);
    return g_fail ? 1 : 0;
}
