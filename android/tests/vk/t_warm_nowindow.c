// SPDX-License-Identifier: Apache-2.0
// 窗口/普通程序共用同一真实绘制、存储缓冲和整池手算真值。
#define S2A_TEST_WARM_NOWINDOW 1
#define nvn_plat_flag window_default_flag
#define s2a_direct_provider_compile_context window_default_context
#define s2a_direct_bound_compile_any window_default_compiler
#include "t_replay_libpack_ssbo_window.c"
#undef main
#undef nvn_plat_flag
#undef s2a_direct_provider_compile_context
#undef s2a_direct_bound_compile_any
#include "nvn_vk_prog_warm.h"
#include "nvn_vk_pipe_warm.h"
#include "nvn_plat.h"

static const char *phase, *proof_root;
static char cache[512], closure[256], profile_hash[96];
static uint8_t *profile_bytes;
static size_t profile_size;
static int is_phase(const char *name) { return strcmp(phase, name) == 0; }

int nvn_plat_flag(const char *name, int fallback) {
    if (!strcmp(name, "nvk_warm_capture")) return is_phase("reference");
    if (!strcmp(name, "nvk_libpack_ssbo_window")) return is_phase("original");
    if (!strcmp(name, "nvk_warm_nowindow")) return is_phase("enabled") || is_phase("wrong-pipe");
    return window_default_flag(name, fallback);
}
const char *nvk_session_cache_dir(void) { return cache; }
const char *nvn_plat_dir(int which) { return which == NVN_PLAT_DIR_CACHE ? cache : NULL; }

static uint8_t *asset(const char *name, size_t *len) {
    char path[1024];
    if (snprintf(path, sizeof path, "%s/%s", proof_root, name) >= (int)sizeof path) return NULL;
    return read_file(path, len);
}
int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *c) {
    window_default_context(c);
    c->profile_json = (const char *)profile_bytes;
    c->profile_bytes = profile_size;
    c->profile_hash = profile_hash;
    c->translator_closure = closure;
    return 0;
}
static int32_t ordinary_compile(uint64_t session, uint64_t generation, s2a_direct_bytes req,
                                s2a_direct_emit_fn emit, void *sink) {
    compile_calls++;
    if (is_phase("request")) {
        char path[1024];
        snprintf(path, sizeof path, "%s/request.json", proof_root);
        FILE *f = fopen(path, "wb");
        if (!f) return -1;
        CHECK(fwrite(req.data, 1, req.bytes, f) == req.bytes, "export complete live request");
        CHECK(fclose(f) == 0, "close request");
        return -1;
    }
    size_t n = 0;
    uint8_t *captured = asset("request.json", &n);
    int exact = captured && n == req.bytes && !memcmp(captured, req.data, n);
    free(captured);
    CHECK(exact, "ordinary compiler input exactly matches compiled request");
    if (!exact) return -1;
    uint8_t *result = asset("compiled/program.json", &n);
    if (!result) return -1;
    int rc = emit(sink, (s2a_direct_bytes){result, n});
    free(result);
    return rc;
}
s2a_direct_compile_fn s2a_direct_bound_compile_any(void) { return ordinary_compile; }

static int guest_sources(void) {
    memset(&guest_program, 0, sizeof guest_program);
    guest_program.magic = MAGIC('P','R','O','G'); guest_program.serial = 100; guest_program.nstage = 2;
    memset(guest_stages, 0xff, sizeof guest_stages);
    for (int st = 0; st < 2; ++st) {
        size_t n = 0;
        uint8_t *code = asset(st ? "fs.bin" : "stages/window_vs.bin", &n);
        if (!code) return 0;
        uint8_t ctl[0x760] = {0}, digest[32];
        put32(ctl + 0x20, sizeof ctl);
        s2a_sha256 h;
        s2a_sha256_init(&h); s2a_sha256_update(&h, ctl, sizeof ctl);
        s2a_sha256_update(&h, code, n); s2a_sha256_final(&h, digest);
        int ok = s2a_source_keep(digest, st, code, (uint32_t)n, ctl, sizeof ctl) == 0;
        free(code);
        if (!ok) return 0;
        memcpy(guest_keys[st], digest, 8); guest_stages[st] = st;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    phase = argv[1]; proof_root = argv[2]; case_dir = proof_root;
    snprintf(cache, sizeof cache, "%s/%s", proof_root, phase);
    if (mkdir(cache, 0700)) return 2;
    profile_bytes = asset("profile.json", &profile_size);
    struct s2a_jv field;
    if (!profile_bytes || s2a_jv_obj_find((struct s2a_jv){(const char *)profile_bytes, profile_size}, "profileHash", &field) ||
        s2a_jv_str(field, profile_hash, sizeof profile_hash)) return 2;
    size_t n = 0;
    uint8_t *cs = asset("closure.txt", &n);
    if (!cs || n >= sizeof closure) return 2;
    memcpy(closure, cs, n); free(cs);
    if (!guest_sources()) return 2;
    char why[256] = "";
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "%s\n", why); return 2; }
    printf("device: %s\n", g_dev.properties.deviceName);
    if (g_dev.properties.limits.minStorageBufferOffsetAlignment > 16 || !open_session(phase, 16, 16)) return 2;
    char path[1024];
    if (is_phase("original")) {
        snprintf(path, sizeof path, "%s/stages", proof_root);
        if (load_vsrc(&syn, path, "window_vs.key")) return 2;
        snprintf(path, sizeof path, "%s/libpack_window.frag.spv", argv[3]);
        fs_spv = read_file(path, &fs_n);
        if (!fs_spv) return 2;
        make_library("original", &syn, V_WINDOW, NULL, 0, fs_spv, fs_n);
    } else {
        snprintf(path, sizeof path, "%s/lp-original", proof_root);
        setenv("S2A_SHADER_REVISION_ROOT", path, 1);
    }
    bind_program(phase);
    const uint32_t offset = SB_BASE + 20, source = 0xcafef00d, value = 0x12345678;
    for (unsigned i = 0; i < GUEST_BYTES; i += 4) word_put(i, rnd());
    word_put(offset + 0x20, source); word_put(UBO_OFF, value);
    uint8_t expected[GUEST_BYTES];
    memcpy(expected, guest, sizeof guest);
    memcpy(expected + offset + 0x40, &source, 4); memcpy(expected + offset, &value, 4);
    p_bind_buf ubo = {0, 0, (uintptr_t)guest + UBO_OFF, 16}, ssbo = {0, 0, (uintptr_t)guest + offset, SB_BOUND};
    CHECK(rp_state_op(OP_BIND_UBO, &ubo, sizeof ubo) && rp_state_op(OP_BIND_SSBO, &ssbo, sizeof ssbo), "bind guest buffers");
    int ab = is_phase("enabled") || is_phase("disabled") || is_phase("wrong-pipe");
    int missing = is_phase("disabled"), wrong = is_phase("wrong-pipe");
    CHECK(nvk_prog_warm_load() == ab && nvk_pipe_warm_load() == ab, "load expected program and pipe recipes");
    if (ab) {
        struct nvk_prog_rec program = {0};
        struct nvk_pipe_rec pipe = {0};
        CHECK(nvk_prog_warm_take(&program) && program.lp_ssbo_window, "original window-tagged program record");
        CHECK(nvk_pipe_warm_take(&pipe), "take real recorded pipeline");
        int prepared = nvk_prog_warm_build(&program);
        CHECK((prepared == 0) == !missing, "ordinary warm result %d", prepared);
        if (!missing && prepared == 0) {
            uint64_t initial = nvn_vk_dbg_pipe_stat(1);
            CHECK(nvk_pipe_warm_build(&pipe) == 0, "warm creates driver pipeline");
            CHECK(nvn_vk_dbg_pipe_stat(1) == initial + 1, "one pipeline in cache after warm");
        }
    }
    uint64_t before = nvn_vk_dbg_pipe_stat(1);
    int drew = draw_and_land(phase, NULL, NULL);
    if (is_phase("request")) {
        CHECK(!drew && compile_calls == 1, "exported one ordinary request without drawing");
    } else {
        uint64_t added = nvn_vk_dbg_pipe_stat(1) - before;
        unsigned differences = 0;
        for (unsigned i = 0; i < GUEST_BYTES; ++i) {
            if (expected[i] != guest[i] && differences++ < 8)
                fprintf(stderr, "guest offset %#x: got %02x expected %02x\n", i, guest[i], expected[i]);
        }
        CHECK(drew && differences == 0, "real replay draw and all 32768 guest bytes");
        CHECK(compile_calls == (is_phase("original") || is_phase("enabled") || wrong ? 0 : 1), "foreground translations %d", compile_calls);
        CHECK(added == (uint64_t)!is_phase("enabled"), "first draw added %llu pipeline(s)", (unsigned long long)added);
        CHECK(!is_phase("enabled") || nvn_gpu_pipe_warm_hits() == 1, "first draw adopted warm pipeline");
        printf("phase=%s translations=%d draw_pipelines=%llu guest_bytes_match=%d\n", phase, compile_calls,
               (unsigned long long)added, drew && !memcmp(expected, guest, sizeof guest));
        nvk_prog_warm_save(); nvk_pipe_warm_save();
    }
    close_session(phase);
    s2a_vk_device_destroy(&g_dev, 1);
    return g_fail != 0;
}
