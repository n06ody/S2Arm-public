// 生产请求同步交给真实 DirectCompiler CLI，结果经生产解析、回放绘制核心和 Vulkan。
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "rp_shader.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_exec.h"
#include "nvn_unimpl.h"
#include "direct/compile_request.h"
#include "direct/source_store.h"
#include "direct/s2a_direct.h"
#include "direct/sha256.h"
#include "direct/jv.h"
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

static uint8_t *request_data, *result_data, *profile_data;
static size_t request_len, result_len, profile_len;
static char closure[128]; static int compile_calls;
static const char *case_dir;
static prog_t guest_program; static uint8_t guest_keys[6][8], guest_stages[6];
static p_set_rt target; static tex_t target_tex; static nvn_gpu_view target_view;
int g_s2a_logfd = 2; int s2a_nvn_rast_flip;
uint64_t rp_cpu_write_gen;
uint64_t nvn_ga2ca(uint64_t a) { return a; }
int nvn_dpool_base(const void *p, int tex) { (void)p; (void)tex; return 0; }
const prog_t *nvn_prog_snap_get(uint64_t serial, uint8_t keys[6][8], uint8_t *ok, uint8_t stage[6]) {
    if (serial != guest_program.serial) return NULL;
    memcpy(keys, guest_keys, sizeof guest_keys); memcpy(stage, guest_stages, sizeof guest_stages); *ok = 7; return &guest_program;
}
int rp_depth_zero(void) { return 0; }
int rp_origin_lower_left(void) { return 0; }
void rp_perf_add(int index, uint64_t value) { (void)index; (void)value; }
uint64_t rp_seg_serial(void) { return 1; }
void rp_occl_draw(nvn_gpu_cmd c) { (void)c; }
int rp_post_gpu_write(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }
int rp_pool_find(uint64_t a, uint64_t n, const uint8_t **m, uint64_t *s) { (void)a; (void)n; (void)m; (void)s; return 0; }
int rp_buf_resolve(nvn_gpu_cmd c, uint64_t a, uint64_t n, nvn_gpu_buf *b, uint64_t *o) { (void)c; (void)a; (void)n; (void)b; (void)o; return 0; }
void rp_view_pin_begin(void) {}
void rp_view_pin_end(void) {}
nvn_gpu_view rp_view_by_id(int id, nvn_gpu_cmd c, const tex_t **t) { (void)id; (void)c; (void)t; return NULL; }
nvn_gpu_view rp_view_by_id_decl(int id, uint8_t tt, uint8_t cls, int elem, nvn_gpu_cmd c, const tex_t **t) { (void)id; (void)tt; (void)cls; (void)elem; (void)c; (void)t; return NULL; }
nvn_gpu_samp rp_samp_by_id(int id) { (void)id; return NULL; }
int rp_tex_snap_by_id(int id, tex_t *t, view_t *v, int *h) { (void)id; (void)t; (void)v; (void)h; return 0; }
uint64_t rp_tex_fact_sig(const uint64_t *h, int n, int b, int count) { (void)h; (void)n; (void)b; return (uint64_t)count; }
void rp_tex_mark_gpu_written(const tex_t *t) { (void)t; }
int rp_tex_debug_state(uint64_t s) { (void)s; return 0; }
nvn_gpu_tex rp_tex_backend_by_serial(uint64_t s) { (void)s; return NULL; }
const p_set_rt *rp_pass_target(void) { return &target; }
int rp_pass_slot_attached(int i) { return i == 0; }
void rp_pass_slot_size(int i, uint32_t *w, uint32_t *h) { (void)i; *w = *h = 4; }
void rp_pass_note_draw(int d) { (void)d; }
int rp_pass_begin(nvn_gpu_cmd c) {
    nvn_gpu_pass_desc pass = {0}; pass.ncolor = 1; pass.color[0].view = target_view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.w = pass.h = 4; pass.layers = 1; nvn_gpu_pass_begin(c, &pass); return 1;
}
void rp_pass_flush(nvn_gpu_cmd c) { nvn_gpu_pass_end(c); }
int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *c) {
    memset(c, 0, sizeof *c); c->session = 4; c->generation = 6; c->direct_abi = 1;
    c->profile_json = (const char *)profile_data; c->profile_bytes = profile_len; c->profile_hash = "";
    c->translator_closure = closure; c->storage_buffer_offset_alignment = 16; return 0;
}
static int32_t compiled_result(uint64_t session, uint64_t generation, s2a_direct_bytes req, s2a_direct_emit_fn emit, void *sink) {
    compile_calls++;
    if (session != 4 || generation != 6 || req.bytes != request_len || memcmp(req.data, request_data, request_len)) {
        fprintf(stderr, "FAIL: core production request differs from real compiler input\n"); return -1;
    }
    char req_path[1024], out_dir[1024], result_path[1024];
    snprintf(req_path, sizeof req_path, "%s/core-request.json", case_dir);
    snprintf(out_dir, sizeof out_dir, "%s/core-compiler", case_dir);
    snprintf(result_path, sizeof result_path, "%s/core-compiler/program.json", case_dir);
    FILE *f = fopen(req_path, "wb"); if (!f) return -1;
    if (fwrite(req.data, 1, (size_t)req.bytes, f) != (size_t)req.bytes || fclose(f)) return -1;
    char *const argv[] = {"dotnet", "/src/graphics/tools/direct/compiler/cli/bin/Release/net10.0/S2A.DirectCompiler.Cli.dll",
                          "compile", "--request", req_path, "--output", out_dir, NULL};
    pid_t pid; int status = 0;
    if (posix_spawnp(&pid, "dotnet", NULL, NULL, argv, environ) || waitpid(pid, &status, 0) != pid ||
        !WIFEXITED(status) || WEXITSTATUS(status)) return -1;
    size_t n = 0; uint8_t *compiled = read_file(result_path, &n);
    if (!compiled || n != result_len || memcmp(compiled, result_data, n)) { free(compiled); return -1; }
    s2a_direct_bytes out = {compiled, n}; int32_t rc = emit(sink, out); free(compiled); return rc;
}
s2a_direct_compile_fn s2a_direct_bound_compile_any(void) { return compiled_result; }
static void put32(uint8_t *p, uint32_t n) { p[0] = n; p[1] = n >> 8; p[2] = n >> 16; p[3] = n >> 24; }
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    case_dir = argv[1];
    char path[1024];
#define READ(dst, len, suffix) do { snprintf(path, sizeof path, "%s/%s", argv[1], suffix); dst = read_file(path, &len); if (!dst) return 2; } while (0)
    READ(request_data, request_len, "request.json"); READ(result_data, result_len, "compiled/program.json"); READ(profile_data, profile_len, "profile.json");
    struct s2a_jv root = {(const char *)request_data, request_len}, val;
    if (s2a_jv_obj_find(root, "translatorClosure", &val) || s2a_jv_str(val, closure, sizeof closure)) return 2;
    memset(&guest_program, 0, sizeof guest_program); guest_program.magic = MAGIC('P','R','O','G'); guest_program.serial = 101; guest_program.nstage = 3;
    memset(guest_stages, 0xff, sizeof guest_stages);
    const char *names[3] = {"vs", "fs", "gs"};
    for (unsigned st = 0; st < 3; st++) {
        size_t n = 0; snprintf(path, sizeof path, "%s/maxwell/%s.bin", argv[1], names[st]); uint8_t *code = read_file(path, &n); if (!code) return 2;
        uint8_t ctl[0x760] = {0}, digest[32]; put32(ctl + 0x20, sizeof ctl);
        s2a_sha256 h; s2a_sha256_init(&h); s2a_sha256_update(&h, ctl, sizeof ctl); s2a_sha256_update(&h, code, n); s2a_sha256_final(&h, digest);
        if (s2a_source_keep(digest, st, code, (uint32_t)n, ctl, sizeof ctl)) return 2;
        memcpy(guest_keys[st], digest, 8); guest_stages[st] = (uint8_t)st; free(code);
    }
    char why[256] = {0}; if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "gs-real-core";
    if (!open_backend()) return 1;
    nvn_gpu_caps caps; nvn_gpu_caps_get(&caps); CHECK(caps.caps & NVN_GPU_CAP_GEOMETRY, "actual GS probe enabled");
    nvn_gpu_tex tex = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER); target_view = make_view(tex);
    nvn_gpu_buf rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    memset(&target_tex, 0, sizeof target_tex); target_tex.d.target = NVN_TEXTURE_TARGET_2D; target_tex.d.format = NVN_FORMAT_RGBA8;
    target_tex.d.width = target_tex.d.height = 4; target_tex.d.depth = 1; target_tex.d.levels = 1; target_tex.serial = 7;
    memset(&target, 0, sizeof target); target.n = 1; target.color[0] = &target_tex;
    rp_state_reset(); p_bind_program_id bind = {&guest_program, guest_program.serial, 0x1f, 0};
    CHECK(rp_state_op(OP_BIND_PROGRAM, &bind, sizeof bind + 8), "OP_BIND_PROGRAM accepted");
    dpool_t tp = {0}, sp = {0}; tp.n = 7; sp.n = 9; p_set_pool t = {&tp}, s = {&sp};
    CHECK(rp_state_op(OP_SET_TEX_POOL, &t, sizeof t) && rp_state_op(OP_SET_SAMP_POOL, &s, sizeof s), "descriptor pools accepted");
    nvn_gpu_cmd cmd = begin_cmd(); p_draw draw = {0}; draw.prim = 0; draw.count = draw.instances = 1;
    rp_draw(cmd, &draw, NULL, &draw); nvn_gpu_pass_end(cmd); nvn_gpu_copy_tex_buf(cmd, tex, 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_err e = {0}; nvn_gpu_rc r = nvn_gpu_cmd_commit(cmd, 1, on_done, NULL, &e); CHECK(OK(r), "core commit %d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(1, 5000000000ull, &e); CHECK(OK(r), "core wait %d %s", r.code, e.why);
    uint64_t drawn = 0, skipped = 0; rp_draw_stats(&drawn, &skipped); CHECK(compile_calls == 1 && drawn == 1 && skipped == 0, "one exact request and one replay draw");
    for (int i = 0; i < 16; i++) { const uint8_t green[4] = {0,255,0,255}; const uint8_t *v = buf_cpu(rb) + 4*i;
        CHECK(!memcmp(v, green, 4), "core pixel %d got %u,%u,%u,%u", i, v[0],v[1],v[2],v[3]); }
    rp_shader_reset(); nvn_gpu_view_release(target_view); nvn_gpu_tex_release(tex); nvn_gpu_buf_release(rb, NULL, NULL);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "close core session"); s2a_vk_device_destroy(&g_dev, 1);
    free(request_data); free(result_data); free(profile_data);
    printf("core Maxwell GS: 16 pixels, failures=%d\n", g_fail); return g_fail ? 1 : 0;
}
