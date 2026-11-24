// Included by the actual WSI fixture. Every arm records a real pipeline and
// submits a real draw; only its optional observations or ticket differ.
#include "direct/sha256.h"
#include <errno.h>
#include <unistd.h>

static int deny_done_link, fail_frame_sync;
extern __typeof__(link) __real_link;
extern __typeof__(fsync) __real_fsync;

int __wrap_link(const char *src, const char *dst) {
    size_t n = strlen(dst);
    if (deny_done_link && n >= 5 && !strcmp(dst + n - 5, ".done")) {
        errno = EACCES;
        return -1;
    }
    return __real_link(src, dst);
}

int __wrap_fsync(int fd) {
    if (fail_frame_sync) {
        char proc[64], target[4096];
        snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(proc, target, sizeof target - 1);
        if (n >= 0) {
            target[n] = 0;
            if (strstr(target, "/.sf-part-")) {
                errno = ENOSPC;
                return -1;
            }
        }
    }
    return __real_fsync(fd);
}


static int frame_roi_flip;
static int frame_test_present(nvn_gpu_surface surface, nvn_gpu_view src, uint64_t seq, uint64_t ticket) {
    nvn_gpu_err e = {0};
    nvn_gpu_drawable dr = {0};
    if (!OK(nvn_gpu_drawable_acquire(surface, 1000000000ull, NULL, &dr, &e))) return 0;
    nvn_gpu_present_desc pd = {.src = src, .observation_seq = seq};
    if (frame_roi_flip) {
        memcpy(pd.crop,(int32_t[]){4,4,56,56},sizeof pd.crop);
        memcpy(pd.dst,(int32_t[]){16,10,288,160},sizeof pd.dst);
        pd.flip_y=1;
    }
    return OK(nvn_gpu_present(surface, &dr, &pd, ticket, on_shown, NULL, &e));
}

#include "rp_shader.h"
static uint32_t frame_test_choice(const uint8_t request[32], const uint8_t owner[32], nvn_gpu_shader fs,
                                  uint8_t route, uint8_t refusal) {
    struct rp_shader_choice choice={.source=RP_SOURCE_LIVE,.route=route,.refusal=refusal,.keys={{3},{4}},
                                    .cbuf=2,.word=90,.stage=NVN_GPU_SH_FS,.requested_class=1};
    return nvn_gpu_program_choice_note ? nvn_gpu_program_choice_note(request,owner,fs,0,&choice) : 0;
}
#include "t_frame_resources.inc"

static unsigned long long test_proc_birth(void) {
    FILE *f = fopen("/proc/self/stat", "r");
    char line[4096];
    if (!f || !fgets(line, sizeof line, f)) return 0;
    fclose(f);
    char *save = NULL, *token = strtok_r(strrchr(line, ')') + 1, " ", &save);
    for (int field = 3; token && field < 22; field++) token = strtok_r(NULL, " ", &save);
    return token ? strtoull(token, NULL, 10) : 0;
}

static int test_draw_frame(const char *dir, const char *arm) {
    const char *session = "01020304050607080910111213141516", *nonce = "00112233445566778899aabbccddeeff";
    const char *other = "ffeeddccbbaa99887766554433221100";
    setenv("S2A_TEST_DRAW_FRAME_SESSION", session, 1);
    unsigned long long birth = test_proc_birth();
    long pid = (long)getpid();
    char mailbox[512], partial[520], final[640], alternate[640], stem[448];
    snprintf(stem, sizeof stem, "%s/sf-%ld-%llu-%s", getenv("S2A_FRAME_LOG_DIR"), pid, birth, session);
    snprintf(mailbox, sizeof mailbox, "%s.request", stem);
    snprintf(partial, sizeof partial, "%s.part", mailbox);
    snprintf(final, sizeof final, "%s-%s.done", stem, nonce);
    snprintf(alternate, sizeof alternate, "%s-%s.done", stem, other);
    frame_test_target = !strncmp(arm,"res-",4) ? 2 : strcmp(arm, "off") != 0;
    if (!strncmp(arm,"res-choice",10) || !strncmp(arm,"res-roi",7)) frame_test_target=3;
    int collision = !strcmp(arm, "collision") || !strcmp(arm, "collision-link");
    deny_done_link = !strcmp(arm, "res-roi-hardlink") || !strcmp(arm, "collision");
    fail_frame_sync = !strcmp(arm, "seal-fsync");
    frame_roi_flip=!strcmp(arm,"res-roi-flip");
    if (!open_backend()) return 2;
    nvn_gpu_err e = {0};
    nvn_gpu_surface surface = NULL;
    if (!OK(nvn_gpu_surface_attach((void *)1, &surface, &e))) return 2;
    nvn_gpu_tex tex = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view view = make_view(tex);
    nvn_gpu_pass_desc pass = {.w=64, .h=64, .layers=1, .ncolor=1};
    pass.color[0].view = view;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_pass_end(c);
    CHECK(OK(nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e)), "frame setup submit");
    if (frame_test_target==3) {
        nvn_gpu_shader vs=NULL, fs=NULL;
        void *vm=NULL, *fm=NULL;
        if (!load_shader_pair(dir,"replay_front_face.vert","solid_green.frag",NULL,NULL,&vs,&fs,&vm,&fm)) return 2;
        uint8_t request[32]={8}, owner[32]={9};
        frame_test_choice(request,owner,fs,RP_ROUTE_BUILD,RP_LIB_NUMERIC_MISMATCH);
        frame_test_choice(request,owner,fs,RP_ROUTE_BUILD,RP_LIB_NUMERIC_MISMATCH);
        nvn_gpu_shader_release(fs); nvn_gpu_shader_release(vs); free(vm); free(fm);
    }
    if (!strncmp(arm,"res-roi",7)) {
        char roi[768];
        snprintf(roi,sizeof roi,"%s-%s.roi",stem,nonce);
        FILE *plan=fopen(roi,"wx");
        if (!plan) return 2;
        // Pixel-space reference and five non-overlapping rectangles (x,y,w,h).
        fprintf(plan,"v=1 pid=%ld birth=%llu session=%s nonce=%s reference=64x64\n",pid,birth,session,nonce);
        fputs("0 8 8 2 2\n1 13 9 2 2\n2 22 18 2 2\n3 37 31 2 2\n4 51 43 2 2\n",plan);
        if (fclose(plan)) return 2;
    }
    FILE *request_file = fopen(partial, "wx");
    if (!birth || !request_file) return 2;
    fprintf(request_file, "v=2 pid=%ld birth=%llu session=%s nonce=%s\n",
            pid + !strcmp(arm, "pid"), birth + !strcmp(arm, "birth"),
            !strcmp(arm, "session") ? other : session, !strcmp(arm, "nonce") ? "invalid" : nonce);
    if (fclose(request_file)) return 2;
    CHECK(frame_test_present(surface, view, 10, 39), "present while only partial request exists");
    CHECK(!*frame_test_log && access(final, F_OK), "partial request cannot trigger or complete");
    if (strcmp(arm, "absent") && link(partial, mailbox)) return 2;
    if (collision) {
        FILE *existing = fopen(final,"wx");
        if (!existing || fputs("prior",existing)<0 || fclose(existing)) return 2;
    }
    nvn_gpu_cmd early = !strcmp(arm, "cross") ? begin_cmd() : NULL;
    CHECK(frame_test_present(surface, view, 11, 40), "frame opening present");

    if (!strncmp(arm, "res-", 4)) return test_resource_chain(dir, arm, final, surface, tex, view);

    nvn_gpu_shader sh[3] = {0}; void *vb = NULL, *fb = NULL;
    if (!load_shader_pair(dir, "replay_front_face.vert", "solid_green.frag", NULL, NULL,
                          &sh[0], &sh[1], &vb, &fb)) return 2;
    nvn_gpu_pipe_desc pd = {0};
    pd.st[0].sh = sh[0]; pd.st[1].sh = sh[1];
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4;
    pd.chmask.mask = 15; pd.poly.mode = NVN_POLYGON_MODE_FILL;
    nvn_gpu_pipe pipe = NULL;
    if (!OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e))) return 2;
    uint8_t request[32] = {1}, owner[32] = {2};
    nvn_gpu_draw_observation o = {12, request, owner, sh, 0, 0};
    if (!strcmp(arm, "module")) sh[1] = sh[0];
    if (!strcmp(arm, "seq")) o.seq++;
    nvn_gpu_draw_args a = {.prim=4, .count=3, .instances=1, .observation=&o};
    c = early ? early : begin_cmd();
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    CHECK(OK(nvn_gpu_cmd_commit(c, 2, on_done, NULL, &e)), "frame draw submit");
    if (!strcmp(arm,"switched")) setenv("S2A_TEST_DRAW_FRAME_SESSION", other, 1);
    CHECK(frame_test_present(surface, view, 12, !strcmp(arm, "ticket") ? 42 : 41), "frame closing present");
    CHECK(g_shown_status == NVN_GPU_SHOWN_UNKNOWN, "accepted by WSI, display time unknown");
    CHECK(OK(nvn_gpu_fence_wait(2, 5000000000ull, &e)), "frame draw fence");
    const char *reason = (!strcmp(arm,"pid") || !strcmp(arm,"birth") || !strcmp(arm,"session") ||
                          !strcmp(arm,"nonce")) ? "missing=trigger_identity" :
                         (!strcmp(arm,"switched") || collision || fail_frame_sync) ? "missing=write" : !strcmp(arm, "module") ? "missing=pipeline_module" :
                         !strcmp(arm, "seq") ? "missing=draw_seq" :
                         !strcmp(arm, "ticket") ? "missing=present_identity" :
                         !strcmp(arm, "cross") ? "missing=segment_boundary" : NULL;
    if (reason) CHECK(strstr(frame_test_log, reason) && !strstr(frame_test_log, "file="),
                      "frame %s expected %s got %s", arm, reason, frame_test_log);
    else if (!frame_test_target || !strcmp(arm, "absent")) CHECK(!*frame_test_log, "frame off emitted a receipt");
    else {
        char leaf[192] = {0}, path[1024];
        CHECK(sscanf(frame_test_log, "seq=12 file=%191s", leaf) == 1, "frame receipt %s", frame_test_log);
        snprintf(path, sizeof path, "%s/%s", getenv("S2A_FRAME_LOG_DIR"), leaf);
        CHECK(!strcmp(path, final), "completion name must bind lifetime and nonce");
        FILE *f = fopen(path, "r");
        if (!f) return 2;
        char line[768], vs_sha[65], fs_sha[65], req[65], own[65], expected[768];
        s2a_sha256_hex(request, req); s2a_sha256_hex(owner, own);
        const char *names[2] = {"replay_front_face.vert", "solid_green.frag"};
        for (int st = 0; st < 2; st++) {
            size_t size = 0; uint8_t digest[32];
            snprintf(path, sizeof path, "%s/%s.spv", dir, names[st]);
            void *bytes = read_file(path, &size);
            if (!bytes) return 2;
            s2a_sha256 hash; s2a_sha256_init(&hash); s2a_sha256_update(&hash, bytes, size);
            s2a_sha256_final(&hash, digest); s2a_sha256_hex(digest, st ? fs_sha : vs_sha);
            free(bytes);
        }
        CHECK(fgets(line, sizeof line, f) && strstr(line, "seq=12 ticket=41 ") &&
              strstr(line, "queue_present=accepted display=unproven rows=1"), "frame WSI identity");
        CHECK(fgets(line, sizeof line, f) && !strncmp(line, "seq\tsubmit\t", 11), "frame header");
        snprintf(expected, sizeof expected, "12\t2\t0\t0\t%s\t%s\t0\t%s\t%s\t-\n", req, own, vs_sha, fs_sha);
        CHECK(fgets(line, sizeof line, f) && !strcmp(line, expected), "frame actual stages and alias identities");
        long bytes = ftell(f);
        CHECK(bytes > 0 && !fseek(f, 0, SEEK_SET), "sealed offset");
        char body[2048];
        if (bytes <= 0 || bytes >= (long)sizeof body || fread(body, 1, (size_t)bytes, f) != (size_t)bytes) return 2;
        body[bytes] = 0;
        char identity[192];
        snprintf(identity, sizeof identity, "v=2 pid=%ld birth=%llu session=%s nonce=%s ", pid, birth, session, nonce);
        CHECK(!strncmp(body, identity, strlen(identity)), "sealed process identity");
        s2a_sha256 sha; uint8_t digest[32]; char hex[65];
        s2a_sha256_init(&sha); s2a_sha256_update(&sha, body, (size_t)bytes);
        s2a_sha256_final(&sha, digest); s2a_sha256_hex(digest, hex);
        snprintf(expected, sizeof expected, "sealed bytes=%ld sha256=%s\n", bytes, hex);
        CHECK(fgets(line,sizeof line,f) && !strcmp(line,expected), "sealed length/SHA");
        CHECK(fgetc(f) == EOF && !ferror(f), "frame exactly one row"); fclose(f);
    }
    if (reason && !strcmp(reason, "missing=write")) {
        const char *stage = fail_frame_sync ? "seal_fsync" :
                            !strcmp(arm, "switched") ? "seal_identity" :
                            deny_done_link ? "publish_rename" : "publish_link";
        int err = fail_frame_sync ? ENOSPC : !strcmp(arm, "switched") ? ESTALE : EEXIST;
        char actual[64] = {0};
        int reported = 0;
        size_t bytes = 0;
        CHECK(sscanf(frame_test_log, "seq=12 missing=write stage=%63s errno=%d bytes=%zu",
                     actual, &reported, &bytes) == 3 && !strcmp(actual, stage) &&
              reported == err && bytes > 0, "frame %s expected %s errno=%d bytes>0 got %s",
              arm, stage, err, frame_test_log);
    }
    if (collision) {
        char sentinel[8] = {0}; FILE *prior = fopen(final,"r");
        CHECK(prior && fread(sentinel,1,sizeof sentinel,prior)==5 && !strcmp(sentinel,"prior"), "never overwrite");
        if (prior) fclose(prior);
    } else if (reason || !frame_test_target || !strcmp(arm,"absent")) {
        CHECK(access(final,F_OK) != 0, "no complete file on refusal");
    } else {
        char receipt[256]; snprintf(receipt,sizeof receipt,"%s",frame_test_log);
        CHECK(frame_test_present(surface, view, 13, 42), "repeated request");
        FILE *replace = fopen(mailbox,"w");
        if (!replace) return 2;
        fprintf(replace,"v=2 pid=%ld birth=%llu session=%s nonce=%s\n",pid,birth,session,other);
        fclose(replace);
        CHECK(frame_test_present(surface, view, 14, 43) && frame_test_present(surface, view, 15, 44),
              "later request cannot re-arm");
        CHECK(!strcmp(frame_test_log,receipt) && access(alternate,F_OK), "consume once per lifetime");
    }
    nvn_gpu_shader_release(pd.st[0].sh); nvn_gpu_shader_release(pd.st[1].sh);
    free(vb); free(fb); nvn_gpu_view_release(view); nvn_gpu_tex_release(tex);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "frame close");
    fprintf(stderr, "draw-frame arm=%s failures=%d receipt=%s\n", arm, g_fail, frame_test_log);
    return g_fail != 0;
}
