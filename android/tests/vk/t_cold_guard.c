// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "nvn_vk_cold_guard.h"
#include "nvn_vk_cold_asset.h"
#include "nvn_refl.h"
#include "vk_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int nvn_vk_cold_guard_scan(nvn_gpu_cmd cmd, nvn_gpu_pipe original, nvn_gpu_pipe variant,
                          const nvn_gpu_draw_args *draw, const struct nvn_gpu_cold_domain *domain,
                          const struct nvn_vk_cold_publication *published, uint32_t npublished) __attribute__((weak));
int nvn_vk_cold_guard_draw(nvn_gpu_cmd cmd, const nvn_gpu_draw_args *draw) __attribute__((weak));
int nvn_vk_cold_guard_auto(nvn_gpu_cmd, nvn_gpu_cmd, nvn_gpu_pipe, nvn_gpu_pipe, const nvn_gpu_draw_args *,
                           const struct nvn_gpu_cold_domain *) __attribute__((weak));
void nvn_vk_cold_mirror_track(nvn_gpu_buf) __attribute__((weak));
void nvn_vk_cold_mirror_host_write(nvn_gpu_buf, uint64_t, uint64_t) __attribute__((weak));
void nvn_gpu_vk_test_cold_overwrite(nvn_gpu_cmd, nvn_gpu_buf, uint64_t) __attribute__((weak));
void nvn_gpu_vk_test_cold_args(nvn_gpu_cmd c, nvn_gpu_buf dst, uint64_t off) __attribute__((weak));
void nvn_gpu_vk_test_cold_event_limit(unsigned limit) __attribute__((weak));

static nvn_gpu_err err;
static int dq, enabled = 1, diag;
static unsigned dispatches, indirects, binds, epoch_binds, bad;
static VkPipeline last_bind;
static PFN_vkCmdDispatch real_dispatch;
static PFN_vkCmdDrawIndirect real_indirect;
static PFN_vkCmdBindPipeline real_bind;

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "fixture setup line %d: %s (%s)\n", __LINE__, #x, err.why); exit(2); } } while (0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))

static VKAPI_ATTR void VKAPI_CALL observe_dispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z) {
    if (x == 1) dispatches++;  // The distinct fixture producer has two workgroups.
    real_dispatch(c, x, y, z);
}
static VKAPI_ATTR void VKAPI_CALL observe_indirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize off,
                                                  uint32_t count, uint32_t stride) {
    REQUIRE(count == 1 && stride == sizeof(VkDrawIndirectCommand));
    if (last_bind) epoch_binds++;
    last_bind = VK_NULL_HANDLE;
    indirects++;
    real_indirect(c, b, off, count, stride);
}
static VKAPI_ATTR void VKAPI_CALL observe_bind(VkCommandBuffer c, VkPipelineBindPoint point, VkPipeline p) {
    if (point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        last_bind = p;
        binds++;
    }
    real_bind(c, point, p);
}
PFN_vkVoidFunction __real_vkGetDeviceProcAddr(VkDevice d, const char *name);
PFN_vkVoidFunction __wrap_vkGetDeviceProcAddr(VkDevice d, const char *name) {
    PFN_vkVoidFunction real = __real_vkGetDeviceProcAddr(d, name);
    if (real && !strcmp(name, "vkCmdDispatch")) {
        real_dispatch = (PFN_vkCmdDispatch)real;
        return (PFN_vkVoidFunction)observe_dispatch;
    }
    if (real && !strcmp(name, "vkCmdDrawIndirect")) {
        real_indirect = (PFN_vkCmdDrawIndirect)real;
        return (PFN_vkVoidFunction)observe_indirect;
    }
    if (real && !strcmp(name, "vkCmdBindPipeline")) {
        real_bind = (PFN_vkCmdBindPipeline)real;
        return (PFN_vkVoidFunction)observe_bind;
    }
    return real;
}

const char *const *nvn_plat_vk_instance_exts(uint32_t *n) { *n = 0; return NULL; }
VkResult nvn_plat_vk_surface_create(VkInstance i, void *w, VkSurfaceKHR *s) { return VK_ERROR_EXTENSION_NOT_PRESENT; }
int nvn_plat_window_size(void *w, uint32_t *x, uint32_t *y) { return 0; }
void nvn_plat_safepoint(void) {}
void nvn_plat_log(int l, const char *cat, const char *line) { fprintf(stderr, "[%s] %s\n", cat, line); }
int nvn_plat_flag(const char *name, int def) {
    if (!strcmp(name, "nvk_dq")) return dq;
    if (enabled && !strcmp(name, "nvk_cold_guard")) return 1;
    if (!strcmp(name, "nvk_cold_guard_diag")) return diag;
    return def;
}
static void done(void *user, const nvn_gpu_done *d) { REQUIRE(d->status == NVN_GPU_DONE_OK); }

static void *load_bytes(const char *dir, const char *name, size_t *bytes) {
    char path[1024];
    REQUIRE(snprintf(path, sizeof path, "%s/%s", dir, name) < (int)sizeof path);
    FILE *f = fopen(path, "rb");
    REQUIRE(f && !fseek(f, 0, SEEK_END));
    long size = ftell(f);
    REQUIRE(size > 0);
    *bytes = (size_t)size;
    rewind(f);
    void *data = malloc(*bytes);
    REQUIRE(data && fread(data, 1, *bytes, f) == *bytes);
    fclose(f);
    return data;
}

static nvn_gpu_shader load_shader(const void *code, size_t size, nvn_gpu_stage stage, const nvn_gpu_layout *layout) {
    const nvn_gpu_shader_src desc = {.stage = stage, .kind = NVN_GPU_BLOB_SPIRV, .entry = "main",
        .blob = code, .bytes = size, .layout = layout};
    nvn_gpu_shader shader;
    GPU(nvn_gpu_shader_load(&desc, &shader, &err));
    return shader;
}

static nvn_gpu_shader load(const char *dir, const char *name, nvn_gpu_stage stage, const nvn_gpu_layout *layout) {
    size_t bytes;
    void *data = load_bytes(dir, name, &bytes);
    nvn_gpu_shader shader = load_shader(data, bytes, stage, layout);
    free(data);
    return shader;
}

enum { WIDTH = 8, HEIGHT = 4, IMAGE_BYTES = WIDTH * HEIGHT * 4, OFF = 256, FIELD_WORD = 3, DRIVER_BYTE = 16, DRIVER_MASK = 15, DRIVER_VALUE = 4,
       FIRST_WORD = FIELD_WORD + DRIVER_VALUE / 4, STRIDE = 5, COUNT = 65,
       WORDS = FIRST_WORD + (COUNT - 1) * STRIDE + 1, TRANSFER_BYTES = (WORDS - 1) * 4 };
enum mode { BASE, AA, AB, MISSING_FRONT, MISSING_TAIL, SHORT, BIND_CHANGED, PREFIX_CHANGED,
            AUTO, AUTO_HOST, AUTO_MISSING_FRONT, AUTO_MISSING_TAIL, AUTO_PRE, AUTO_COMMITTED, AUTO_ABORT, AUTO_CHANGED };
static const uint32_t formats[] = {NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA8UI};

static nvn_gpu_pipe make_pipe(nvn_gpu_shader vs, nvn_gpu_shader fs) {
    nvn_gpu_pipe_desc p = {0};
    p.poly.mode = NVN_POLYGON_MODE_FILL;
    p.color.logic_op = 3;
    p.prim = 4;
    p.samples = 1;
    p.chmask.mask = 255;
    p.cfmt[0] = formats[0]; p.cfmt[1] = formats[1];
    p.st[NVN_GPU_SH_VS].sh = vs; p.st[NVN_GPU_SH_FS].sh = fs;
    nvn_gpu_pipe pipe;
    GPU(nvn_gpu_pipe_get(&p, NVN_GPU_PIPE_SYNC, &pipe, &err));
    return pipe;
}

struct comparison {
    unsigned armed, selected, selector_dispatch, indirect, paired_binds, all_binds, bad_args;
    uint8_t pixels[2 * IMAGE_BYTES];
};

static struct comparison run_case(nvn_gpu_pipe a, nvn_gpu_pipe b, nvn_gpu_pipe producer, enum mode mode,
                                  unsigned data, uint64_t *signal) {
    nvn_gpu_cmd c;
    GPU(nvn_gpu_cmd_begin(NULL, &c, &err));
    dispatches = indirects = binds = epoch_binds = 0;
    last_bind = VK_NULL_HANDLE;
    nvn_gpu_tex tex[2];
    nvn_gpu_view views[2];
    nvn_gpu_pass_desc pass = {.ncolor = 2, .w = WIDTH, .h = HEIGHT, .layers = 1};
    for (unsigned i = 0; i < 2; i++) {
        nvn_gpu_tex_desc td = {
            .d = {.target = NVN_TEXTURE_TARGET_2D, .format = formats[i], .width = WIDTH, .height = HEIGHT,
                  .depth = 1, .levels = 1},
            .usage = NVN_GPU_TEX_RENDER,
        };
        GPU(nvn_gpu_tex_create(&td, &tex[i], &err));
        GPU(nvn_gpu_view_create(tex[i], NULL, &views[i], &err));
        pass.color[i] = (nvn_gpu_att){.view = views[i], .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE};
    }
    nvn_gpu_buf src, sentinel, alt = NULL, out;
    const int automatic = mode >= AUTO;
    uint32_t initial[(OFF / 4) + WORDS] = {0};
    GPU(nvn_gpu_buf_create(sizeof initial, NVN_GPU_MEM_SHARED, initial, &src, &err));
    if (automatic && nvn_vk_cold_mirror_track) nvn_vk_cold_mirror_track(src);
    GPU(nvn_gpu_buf_create(OFF + WORDS * 4, NVN_GPU_MEM_DEVICE, NULL, &sentinel, &err));
    if (mode == BIND_CHANGED) GPU(nvn_gpu_buf_create(OFF + WORDS * 4, NVN_GPU_MEM_DEVICE, NULL, &alt, &err));
    uint32_t words[WORDS];
    // Poison stride holes. They are addressable SSBO bytes but do not count.
    for (unsigned i = 0; i < WORDS; i++) words[i] = 0xf00u;
    for (unsigned i = 0; i < COUNT; i++)
        words[FIRST_WORD + i * STRIDE] = data == 1 || (data == 2 && i == COUNT - 1) ? 3 : 0;
    if (mode == PREFIX_CHANGED) {
        words[FIELD_WORD] = words[FIRST_WORD];
        words[WORDS - 2] = words[WORDS - 1];
    }
    nvn_gpu_cmd pre = NULL;
    if (mode == AUTO_PRE || mode == AUTO_COMMITTED || mode == AUTO_ABORT) {
        const nvn_gpu_cmd_desc pd = {.label = "nvn submit pre"};
        GPU(nvn_gpu_cmd_begin(&pd, &pre, &err));
    }
    const size_t skip_front = mode == AUTO_MISSING_FRONT ? (FIRST_WORD + 1) * sizeof(uint32_t) : 0;
    const size_t init_end = !automatic || mode == AUTO_MISSING_TAIL ? TRANSFER_BYTES : sizeof words;
    if (mode == AUTO_HOST) {
        memcpy(nvn_gpu_buf_cpu(src) + OFF, words, sizeof words);
        if (nvn_vk_cold_mirror_host_write) nvn_vk_cold_mirror_host_write(src, OFF, sizeof words);
    } else {
        nvn_gpu_buf_write(pre ? pre : c, src, OFF + skip_front, (uint8_t *)words + skip_front, init_end - skip_front);
    }
    if (mode == AUTO_COMMITTED || mode == AUTO_ABORT) {
        if (mode == AUTO_ABORT) nvn_gpu_cmd_abort(pre);
        else GPU(nvn_gpu_cmd_commit(pre, ++*signal, done, NULL, &err));
        pre = NULL;
    }
    const uint32_t tail = words[WORDS - 1];
    if (alt) nvn_gpu_buf_write(c, alt, OFF, words, sizeof words);
    words[FIRST_WORD] = words[FIELD_WORD] = 7;
    words[WORDS - 1] = words[WORDS - 2] = 9;
    nvn_gpu_buf_write(c, sentinel, OFF, words, sizeof words);
    nvn_gpu_caps caps;
    nvn_gpu_caps_get(&caps);
    const uint32_t tail_at = OFF + TRANSFER_BYTES;
    const uint32_t producer_at = tail_at - tail_at % caps.min_ssbo_align;
    const nvn_gpu_binding target = {.cls = NVN_GPU_RES_SSBO,
        .u.b = {src, producer_at, tail_at + sizeof(uint32_t) - producer_at}};
    const uint32_t push[] = {(tail_at - producer_at) / 4, tail};
    nvn_gpu_set_pipe(c, producer);
    nvn_gpu_bind(c, NVN_GPU_SH_CS, &target, 1);
    nvn_gpu_push(c, NVN_GPU_SH_CS, 0, push, sizeof push);
    nvn_gpu_dispatch(c, producer, 2, 1, 1, NULL, 0);

    nvn_gpu_slice driver, other_driver;
    enum { DRIVER_BYTES = 32 };
    GPU(nvn_gpu_arena_alloc(c, DRIVER_BYTES, OFF, &driver, &err));
    uint32_t driver_words[8] = {0};
    driver_words[DRIVER_BYTE / 4] = DRIVER_VALUE;
    memcpy(driver.cpu, driver_words, sizeof driver_words);
    other_driver = driver;
    if (mode == PREFIX_CHANGED) {
        GPU(nvn_gpu_arena_alloc(c, DRIVER_BYTES, OFF, &other_driver, &err));
        driver_words[DRIVER_BYTE / 4] = 0;
        memcpy(other_driver.cpu, driver_words, sizeof driver_words);
    }
    struct nvn_gpu_cold_domain domain = {
        .buf = src, .first_word = FIELD_WORD, .stride_words = STRIDE, .count = COUNT,
        .prefix = {.buf = driver.buf, .fragment_ent = 1, .byte = DRIVER_BYTE, .mask = DRIVER_MASK},
    };
    REQUIRE(nvn_gpu_buf_access_span(driver.buf, driver.off, DRIVER_BYTES, &domain.prefix.binding));
    struct nvn_vk_cold_publication pub[2] = {{.writers = COLD_READ_TRANSFER}, {.writers = COLD_READ_COMPUTE}};
    REQUIRE(nvn_gpu_buf_access_span(src, OFF, sizeof words, &domain.binding));
    REQUIRE(nvn_gpu_buf_access_span(src, OFF, TRANSFER_BYTES, &pub[0].span));
    REQUIRE(nvn_gpu_buf_access_span(src, OFF + TRANSFER_BYTES, sizeof words - TRANSFER_BYTES, &pub[1].span));
    const nvn_gpu_draw_args draw = {.prim = 4, .count = 3, .instances = 1, .first = 5};
    struct comparison r = {0};
    if (mode == SHORT) domain.binding.bytes -= sizeof(uint32_t);
    if (mode == BIND_CHANGED) nvn_gpu_barrier(c, 0);
    if (automatic && nvn_vk_cold_guard_auto)
        r.armed = nvn_vk_cold_guard_auto(c, pre, a, b, &draw, &domain);
    if (!automatic && mode != BASE && nvn_vk_cold_guard_scan) {
        r.armed = nvn_vk_cold_guard_scan(c, a, b, &draw, &domain,
            mode == MISSING_FRONT ? &pub[1] : pub, mode == MISSING_FRONT || mode == MISSING_TAIL ? 1 : 2);
    }
    // Test the extra read's ordering before any pass or existing transfer
    // barrier can mask a missing WAR dependency on the abandoned scan.
    if (r.armed && mode == BIND_CHANGED && nvn_gpu_vk_test_cold_overwrite)
        nvn_gpu_vk_test_cold_overwrite(c, src, tail_at);
    if (mode == AUTO_CHANGED) nvn_gpu_buf_write(c, src, tail_at, &tail, sizeof tail);
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, a);
    nvn_gpu_binding bind[] = {
        {.cls = NVN_GPU_RES_SSBO, .u.b = {alt ? alt : src, OFF, sizeof words}},
        {.ent = 1, .cls = NVN_GPU_RES_UBO, .u.b = {other_driver.buf, other_driver.off, DRIVER_BYTES}},
    };
    nvn_gpu_bind(c, NVN_GPU_SH_FS, bind, 2);
    nvn_gpu_dyn dyn = {.has_sc = 1, .sc = {0, 0, WIDTH - 1, HEIGHT}};
    nvn_gpu_set_dyn(c, &dyn);
    if (nvn_vk_cold_guard_draw) r.selected = nvn_vk_cold_guard_draw(c, &draw);
    if (!r.selected) nvn_gpu_draw(c, &draw);
    // A direct draw immediately after the pair checks that PSO, SSBO and
    // dynamic state still encode correctly. Use different data for the sentinel.
    dyn.sc[0] = WIDTH - 1; dyn.sc[2] = 1;
    nvn_gpu_set_dyn(c, &dyn);
    bind[0].u.b.buf = sentinel;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, bind, 2);
    nvn_gpu_draw(c, &draw);
    nvn_gpu_pass_end(c);
    enum { ARGS_OFFSET = 2 * IMAGE_BYTES };
    GPU(nvn_gpu_buf_create(ARGS_OFFSET + 2 * sizeof(VkDrawIndirectCommand), NVN_GPU_MEM_SHARED, NULL, &out, &err));
    if (r.armed && nvn_gpu_vk_test_cold_args) nvn_gpu_vk_test_cold_args(c, out, ARGS_OFFSET);
    for (unsigned i = 0; i < 2; i++) {
        nvn_gpu_copy_tex_buf(c, tex[i], 0, 0, NULL, out, i * IMAGE_BYTES, WIDTH * 4, IMAGE_BYTES);
        nvn_gpu_view_release(views[i]);
        nvn_gpu_tex_release(tex[i]);
    }
    nvn_gpu_buf_release(src, NULL, NULL);
    nvn_gpu_buf_release(sentinel, NULL, NULL);
    if (alt) nvn_gpu_buf_release(alt, NULL, NULL);
    if (pre) GPU(nvn_gpu_cmd_commit(pre, ++*signal, done, NULL, &err));
    GPU(nvn_gpu_cmd_commit(c, ++*signal, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(*signal, 5000000000ull, &err));
    r.selector_dispatch = dispatches;
    r.indirect = indirects;
    r.paired_binds = epoch_binds;
    r.all_binds = binds;
    const uint8_t *memory = nvn_gpu_buf_cpu(out);
    REQUIRE(memory);
    memcpy(r.pixels, memory, sizeof r.pixels);
    if (r.armed) {
        VkDrawIndirectCommand actual[2];
        memcpy(actual, memory + ARGS_OFFSET, sizeof actual);
        const VkDrawIndirectCommand want[] = {{3, data != 0, 5, 0}, {3, data == 0, 5, 0}};
        r.bad_args = memcmp(actual, want, sizeof want) != 0;
    }
    nvn_gpu_buf_release(out, NULL, NULL);
    return r;
}

static void check(const char *tag, struct comparison r, const struct comparison *ref,
                  int armed, int selected, unsigned data) {
    const unsigned count = data != 0 ? 3 : 0;
    unsigned wrong = 0;
    for (unsigned i = 0; i < IMAGE_BYTES; i += 4) {
        const unsigned x = i / 4 % WIDTH;
        const unsigned sample = x == WIDTH - 1 ? 9 : (data == 2 && x < 4 ? 0 : count);
        const uint8_t expect0[] = {11 + sample, 5, 57, 255};
        const uint8_t expect1[] = {23 + sample, 71, 5, 199};
        wrong += memcmp(r.pixels + i, expect0, 4) != 0;
        wrong += memcmp(r.pixels + IMAGE_BYTES + i, expect1, 4) != 0;
    }
    bad += wrong != 0 || (ref && memcmp(r.pixels, ref->pixels, sizeof r.pixels)) ||
        r.armed != (unsigned)armed || r.selected != (unsigned)selected ||
        r.selector_dispatch != (unsigned)armed || r.indirect != (selected ? 2u : 0u) ||
        r.paired_binds != (selected ? 2u : 0u) || r.all_binds != (selected ? 3u : 1u) || r.bad_args;
    printf("cold-guard dq=%d %s data=%u armed=%u selected=%u dispatch=%u indirect=%u binds=%u/%u args_bad=%u pixel_bad=%u\n",
        dq, tag, data, r.armed, r.selected, r.selector_dispatch, r.indirect, r.paired_binds, r.all_binds, r.bad_args, wrong);
}

int main(int argc, char **argv) {
    REQUIRE(argc == 4);
    const int mode = atoi(argv[3]);
    dq = atoi(argv[2]);
    enabled = mode != 0;
    diag = mode == 2;
    struct s2a_vk_device dev;
    char why[256];
    if (s2a_vk_device_create(&dev, why, sizeof why)) { fprintf(stderr, "%s\n", why); return 2; }
    const nvn_gpu_open_desc open = {.host_device = &dev, .app_label = "cold-guard"};
    GPU(nvn_gpu_open(&open, &err));
    const nvn_gpu_layout_ent slots[] = {
        {.cls = NVN_GPU_RES_SSBO, .count = 1, .access = NVN_GPU_ACC_R, .stages = 1u << NVN_GPU_SH_FS,
         .src_kind = NVN_RB_SBUFP},
        {.cls = NVN_GPU_RES_UBO, .count = 1, .access = NVN_GPU_ACC_R, .stages = 1u << NVN_GPU_SH_FS,
         .binding = 1, .src_kind = NVN_RB_DRV0},
    };
    const nvn_gpu_layout layout = {.ent = slots, .nent = 2};
    nvn_gpu_shader vs = load(argv[1], "cold-guard-vs.spv", NVN_GPU_SH_VS, NULL);
    uint8_t identity[32], original_sha[32];
    for (unsigned i = 0; i < sizeof identity; i++) identity[i] = (uint8_t)(i + 1);
    size_t original_bytes, asset_bytes;
    void *original_code = load_bytes(argv[1], "cold-guard-a.spv", &original_bytes);
    uint8_t *asset = load_bytes(argv[1], "cold-guard.asset", &asset_bytes);
    cold_asset_digest(original_code, original_bytes, original_sha);
    struct nvk_cold_asset verified;
    REQUIRE(nvk_cold_asset_parse(asset, asset_bytes, identity, original_sha, &layout, &verified));
    REQUIRE(verified.recipe.first_word == FIELD_WORD && verified.recipe.stride_words == STRIDE &&
            verified.recipe.count == COUNT && verified.recipe.prefix_ent == 1 &&
            verified.recipe.prefix_byte == DRIVER_BYTE && verified.recipe.prefix_mask == DRIVER_MASK);
    nvn_gpu_shader fs[] = {load_shader(original_code, original_bytes, NVN_GPU_SH_FS, &layout),
                           load_shader(original_code, original_bytes, NVN_GPU_SH_FS, &layout),
                           load_shader(verified.variant, verified.variant_size, NVN_GPU_SH_FS, &layout)};
    identity[0] ^= 1;
    REQUIRE(!nvk_cold_asset_parse(asset, asset_bytes, identity, original_sha, &layout, &verified));
    identity[0] ^= 1;
    original_sha[0] ^= 1;
    REQUIRE(!nvk_cold_asset_parse(asset, asset_bytes, identity, original_sha, &layout, &verified));
    original_sha[0] ^= 1;
    asset[120] ^= 1;
    REQUIRE(!nvk_cold_asset_parse(asset, asset_bytes, identity, original_sha, &layout, &verified));
    asset[120] ^= 1;
    asset[asset_bytes - 1] ^= 1;
    REQUIRE(!nvk_cold_asset_parse(asset, asset_bytes, identity, original_sha, &layout, &verified));
    asset[asset_bytes - 1] ^= 1;
    const nvn_gpu_layout wrong_layout = {.ent = slots + 1, .nent = 1};
    REQUIRE(!nvk_cold_asset_parse(asset, asset_bytes, identity, original_sha, &wrong_layout, &verified));
    free(asset);
    free(original_code);
    nvn_gpu_pipe a = make_pipe(vs, fs[0]), aa = make_pipe(vs, fs[1]), b = make_pipe(vs, fs[2]);
    REQUIRE(a != aa && a != b && aa != b);
    const nvn_gpu_layout_ent producer_slot = {
        .cls = NVN_GPU_RES_SSBO, .count = 1, .access = NVN_GPU_ACC_W, .stages = 1u << NVN_GPU_SH_CS,
    };
    const nvn_gpu_layout producer_layout = {.ent = &producer_slot, .nent = 1, .push_bytes = 8};
    nvn_gpu_shader cs = load(argv[1], "cold-guard-producer.spv", NVN_GPU_SH_CS, &producer_layout);
    nvn_gpu_stage_desc stage = {.sh = cs};
    nvn_gpu_pipe producer;
    GPU(nvn_gpu_cpipe_get(&stage, NVN_GPU_PIPE_SYNC, &producer, &err));
    uint64_t signal = 0;
    for (unsigned data = 0; data < 3; data++) {
        const struct comparison ref = run_case(a, b, producer, BASE, data, &signal);
        check("direct", ref, NULL, 0, 0, data);
        check("A/A", run_case(a, aa, producer, AA, data, &signal), &ref, enabled, enabled, data);
        check("A/B", run_case(a, b, producer, AB, data, &signal), &ref, enabled, enabled, data);
        if (data != 2) continue;
        check("missing-front", run_case(a, b, producer, MISSING_FRONT, data, &signal), &ref, 0, 0, data);
        check("missing-tail", run_case(a, b, producer, MISSING_TAIL, data, &signal), &ref, 0, 0, data);
        check("short-binding", run_case(a, b, producer, SHORT, data, &signal), &ref, 0, 0, data);
        check("changed-binding", run_case(a, b, producer, BIND_CHANGED, data, &signal), &ref, enabled, 0, data);
        check("changed-prefix", run_case(a, b, producer, PREFIX_CHANGED, data, &signal), &ref, enabled, 0, data);
        check("auto-mixed", run_case(a, b, producer, AUTO, data, &signal), &ref, enabled, enabled, data);
        check("auto-host", run_case(a, b, producer, AUTO_HOST, data, &signal), &ref, enabled, enabled, data);
        check("auto-pre", run_case(a, b, producer, AUTO_PRE, data, &signal), &ref, enabled, enabled, data);
        check("auto-committed", run_case(a, b, producer, AUTO_COMMITTED, data, &signal), &ref, enabled, enabled, data);
        check("auto-missing-front", run_case(a, b, producer, AUTO_MISSING_FRONT, 0, &signal), NULL, 0, 0, 0);
        check("auto-missing-tail", run_case(a, b, producer, AUTO_MISSING_TAIL, data, &signal), &ref, 0, 0, data);
        check("auto-aborted-pre", run_case(a, b, producer, AUTO_ABORT, data, &signal), &ref, 0, 0, data);
        check("auto-changed-source", run_case(a, b, producer, AUTO_CHANGED, data, &signal), &ref, enabled, 0, data);
    }
    if (diag && nvn_gpu_vk_test_cold_event_limit) {
        nvn_gpu_vk_test_cold_event_limit(1);
        check("auto-event-loss", run_case(a, b, producer, AUTO, 2, &signal), NULL, 0, 0, 2);
        nvn_gpu_vk_test_cold_event_limit(0);
        check("auto-session-loss", run_case(a, b, producer, AUTO, 2, &signal), NULL, 0, 0, 2);
    }
    for (unsigned i = 0; i < 3; i++) nvn_gpu_shader_release(fs[i]);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(cs);
    GPU(nvn_gpu_close(5000000000ull, &err));
    REQUIRE(!s2a_vk_device_destroy(&dev, 1));
    printf("cold-guard failures=%u\n", bad);
    return bad != 0;
}
