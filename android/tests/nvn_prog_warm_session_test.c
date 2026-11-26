// Two ordinary sessions through real replay/shader shutdown and warm-cache code.
// No GPU objects; unrelated replay resets are observed with stubs.
#define _POSIX_C_SOURCE 200809L
#include "nvn_vk_prog_warm.h"
#include "nvn_plat.h"
#include "nvn_backend.h"
#include "direct/compile_request.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *directory;
static int checks, failures;
static unsigned red;
static void check(int pass, const char *name) {
    checks++;
    if (!pass) { failures++; fprintf(stderr, "FAIL: %s\n", name); }
}
const char *nvn_plat_dir(int which) { return which == NVN_PLAT_DIR_CACHE ? directory : NULL; }
int nvn_plat_flag(const char *name, int fallback) { return !strcmp(name, "nvk_warm_max") ? 3 : fallback; }
void nvn_plat_log(int level, const char *cls, const char *message) {
    (void)cls; (void)message; if (level == NVN_PLAT_LOG_RED) red++;
}
int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *out) {
    memset(out, 0, sizeof *out);
    out->session = 1; out->generation = 1; out->direct_abi = 7;
    out->profile_json = "session-isolation"; out->profile_bytes = strlen(out->profile_json);
    out->translator_closure = "synthetic";
    return 0;
}
static struct nvk_prog_rec rec(unsigned id, unsigned uses, unsigned mark) {
    struct nvk_prog_rec v = {0};
    v.identity[0] = (uint8_t)id;
    static uint8_t c1buf[256][NVK_PROG_REC_C1_MIN];   // c1 整段：记录只带指针，按 mark 各用一块
    v.uses = uses; v.stage_mask = 1; v.c1_ok = 1;
    memset(c1buf[mark & 255], (int)mark, NVK_PROG_REC_C1_MIN); v.c1[0] = c1buf[mark & 255]; v.c1_len[0] = NVK_PROG_REC_C1_MIN;
    return v;
}

static unsigned siblings[7];
int nvn_be_shutdown_session(void);
void rp_pool_stats_end(void) { siblings[0]++; }
void rp_exec_reset(void) { siblings[1]++; }
void rp_xq_stop(void) {}
void rp_xq_start(void) {}
void rp_draw_reset(void) { siblings[2]++; }
void rp_pass_reset(void) { siblings[3]++; }
void rp_res_reset(void) { siblings[4]++; }
void rp_buf_reset(void) { siblings[5]++; }
void rp_state_reset(void) { siblings[6]++; }
void nvn_gpu_shader_release(nvn_gpu_shader shader) { (void)shader; abort(); }
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    struct nvk_prog_rec a=rec(1,1,10), persisted=rec(2,1,20), b=rec(3,1,30), out={0};
    struct nvk_prog_warm_stats stats;
    directory=argv[1];
    nvk_prog_warm_note(&persisted);
    nvk_prog_warm_save();
    nvk_prog_warm_note(&a);
    check(nvk_prog_warm_load()==1, "session A reloads trusted persisted entry");
    check(nvk_prog_warm_contains(a.identity) && nvk_prog_warm_pending()==1, "session A note and plan exist");
    check(nvn_be_shutdown_session()==0,"real replay shutdown returns success");
    nvk_prog_warm_stats(&stats);
    check(!stats.recorded && !stats.loaded && !stats.rejected && !stats.pending,"real reset clears stats and plan");
    check(!nvk_prog_warm_contains(a.identity) && !nvk_prog_warm_contains(persisted.identity),"real reset clears live table");
    directory=argv[2];
    nvk_prog_warm_note(&b);
    check(nvk_prog_warm_load()==0,"new cache directory has no manifest");
    check(nvk_prog_warm_contains(b.identity),"session B missing load preserves current note");
    check(!nvk_prog_warm_contains(a.identity) && nvk_prog_warm_take(&out)==0,"session B has no previous-session record or plan");
    check(nvn_be_shutdown_session()==0 && !nvk_prog_warm_contains(b.identity),"second real shutdown clears session B");
    for (unsigned i=0;i<7;i++) check(siblings[i]==2,"each unrelated reset hook called twice");
    check(red==0,"no shutdown error diagnostic");
    printf("two normal sessions: checks=%d failures=%d\n",checks,failures);
    return failures?1:0;
}
