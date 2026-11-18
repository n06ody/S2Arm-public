// 在编槽宿主测试（#1501 D②）：翻译器现编时崩溃，崩溃处理调 s2a_compile_inflight_dump，
// 把崩溃线程正在编的那份请求原样落成 S2A_FRAME_LOG_DIR/compile-inflight-<pid>-<槽>.log。
// 用生产 compile_request.c 编宿主二进制（-D__ANDROID__ + 桩 <sys/system_properties.h>，桩在本文件里）。
// 替身编译器是原创合成请求，不涉及游戏数据。场景：
//   ① 没配目录时崩溃：什么都不落；
//   ② 子进程里替身编译器真 SIGSEGV，信号处理调 dump 后按默认处置终止：文件 = 头一行 + 请求逐字节；
//   ③ 正常编完：不落盘；
//   ④ 编译器自己接住了信号（dump 已落）、照常返回：返回后文件删掉；
//   ⑤ 别的线程在编时本线程崩：不冒领那份请求。
// 用法：compile_inflight_test <临时目录>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "direct/compile_request.h"
#include "direct/s2a_direct.h"
#include "direct/sha256.h"

#if !defined(__ANDROID__)
#error "本测试要 -D__ANDROID__ 走生产代码读 session_uuid 那一支"
#endif
int __system_property_get(const char *name, char *out) {
    const char *v = !strcmp(name, "debug.s2a.session_uuid") ? "uuid-inflight-test" : "";
    strcpy(out, v);
    return (int)strlen(v);
}

static int fails, checks;
#define CHECK(cond, what) do { checks++; if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static const char g_req[] = "{\"schema\":\"s2a-direct-compile-v1\",\"note\":\"synthetic inflight fixture\",\"bytes\":\"00ff10\"}";
static const char g_result[] = "{\"schema\":\"s2a-direct-result-v1\"}";
static char g_dir[512];

static int count_files(const char *prefix) {
    DIR *d = opendir(g_dir); if (!d) return -1;
    int n = 0; struct dirent *e;
    while ((e = readdir(d))) if (!strncmp(e->d_name, prefix, strlen(prefix))) n++;
    closedir(d); return n;
}
static void clear_dir(void) {
    DIR *d = opendir(g_dir); if (!d) return;
    struct dirent *e; char p[1024];
    while ((e = readdir(d))) if (e->d_name[0] != '.') { snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name); unlink(p); }
    closedir(d);
}
static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)len + 1); size_t got = fread(b, 1, (size_t)len, f); fclose(f);
    b[got] = 0; *n = got; return b;
}

static int32_t crash_compile(uint64_t s, uint64_t g, s2a_direct_bytes r, s2a_direct_emit_fn emit, void *sink) {
    (void)s; (void)g; (void)r; (void)emit; (void)sink;
    *(volatile int *)(uintptr_t)8 = 1;   // 真缺页（不是 raise）：同翻译器里的空指针解引用
    return 0;
}
static int g_mid_seen = -1;
static int32_t survive_compile(uint64_t s, uint64_t g, s2a_direct_bytes r, s2a_direct_emit_fn emit, void *sink) {
    (void)s; (void)g; (void)r;
    s2a_compile_inflight_dump();   // 等于信号处理跑过、编译器把信号接住后照常往下走
    g_mid_seen = count_files("compile-inflight-");
    s2a_direct_bytes out = { (const uint8_t *)g_result, sizeof g_result - 1 };
    return emit(sink, out);
}
static int32_t echo_compile(uint64_t s, uint64_t g, s2a_direct_bytes r, s2a_direct_emit_fn emit, void *sink) {
    (void)s; (void)g; (void)r;
    s2a_direct_bytes out = { (const uint8_t *)g_result, sizeof g_result - 1 };
    return emit(sink, out);
}
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_in_compile, g_release;
static int32_t block_compile(uint64_t s, uint64_t g, s2a_direct_bytes r, s2a_direct_emit_fn emit, void *sink) {
    (void)s; (void)g; (void)r;
    pthread_mutex_lock(&g_mu); g_in_compile = 1; pthread_cond_broadcast(&g_cv);
    while (!g_release) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    s2a_direct_bytes out = { (const uint8_t *)g_result, sizeof g_result - 1 };
    return emit(sink, out);
}

static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si; (void)ctx;
    s2a_compile_inflight_dump();   // 同 crash.c android_crash_fatal 的调用点；SA_RESETHAND 之后返回再缺页一次 = 默认处置终止
}
static void *ctxp(int k) { return (void *)(uintptr_t)(0x1000 + k); }

// 子进程：绑会崩的替身、执行一次；按原信号终止。返回子进程 pid（父进程里 waitpid 核）。
static pid_t crash_child(int with_dir, int k) {
    pid_t pid = fork();
    if (pid) return pid;
    if (!with_dir) unsetenv("S2A_FRAME_LOG_DIR");
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO | SA_RESETHAND; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    if (s2a_direct_bind_compiler_v1(ctxp(k), crash_compile)) _exit(3);
    struct s2a_compile_outcome out;
    s2a_compile_request_execute(7, 3, g_req, sizeof g_req - 1, &out);
    _exit(4);   // 走到这里 = 没崩
}
static int died_by_fault(pid_t pid) {
    int st = 0; if (waitpid(pid, &st, 0) != pid) return 0;
    return WIFSIGNALED(st) && (WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGBUS);
}

static void *other_thread(void *arg) {
    (void)arg;
    struct s2a_compile_outcome out;
    int rc = s2a_compile_request_execute(9, 1, g_req, sizeof g_req - 1, &out);
    if (!rc) s2a_compile_outcome_free(&out);
    return (void *)(intptr_t)rc;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "用法：%s <临时目录>\n", argv[0]); return 2; }
    snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    setenv("S2A_FRAME_LOG_DIR", g_dir, 1);
    char sha[65]; { s2a_sha256 c; uint8_t d[32]; s2a_sha256_init(&c); s2a_sha256_update(&c, g_req, sizeof g_req - 1); s2a_sha256_final(&c, d); s2a_sha256_hex(d, sha); }

    // ① 没配目录（子进程里取消环境变量，父进程还没初始化过）
    clear_dir();
    pid_t p1 = crash_child(0, 1);
    CHECK(died_by_fault(p1), "① 子进程按缺页信号终止");
    CHECK(count_files("compile-inflight-") == 0, "① 没配目录：崩溃不落盘");

    // ② 配了目录的真崩溃
    clear_dir();
    pid_t p2 = crash_child(1, 2);
    CHECK(died_by_fault(p2), "② 子进程按缺页信号终止（dump 后照常走默认处置）");
    char path[1024]; snprintf(path, sizeof path, "%s/compile-inflight-%ld-0.log", g_dir, (long)p2);
    size_t n = 0; char *got = slurp(path, &n);
    CHECK(got != NULL, "② 崩溃后落下 compile-inflight-<pid>-0.log");
    CHECK(count_files("compile-inflight-") == 1, "② 只落一份");
    if (got) {
        char want_head[512];
        int hn = snprintf(want_head, sizeof want_head,
                          "[s2a] compile-inflight v1 pid=%ld session=7 generation=3 bytes=%zu sha256=%s uuid=uuid-inflight-test\n",
                          (long)p2, sizeof g_req - 1, sha);
        CHECK(n == (size_t)hn + sizeof g_req - 1, "② 文件长度 = 头一行 + 请求字节数");
        CHECK(n >= (size_t)hn && !memcmp(got, want_head, (size_t)hn), "② 头一行带 pid / session / generation / 字节数 / sha256 / session_uuid");
        CHECK(n == (size_t)hn + sizeof g_req - 1 && !memcmp(got + hn, g_req, sizeof g_req - 1), "② 头之后逐字节等于请求 JSON");
        free(got);
    }

    // ③ 正常编完不落盘
    clear_dir();
    CHECK(!s2a_direct_bind_compiler_v1(ctxp(3), echo_compile), "③ 绑替身");
    struct s2a_compile_outcome out;
    CHECK(s2a_compile_request_execute(7, 3, g_req, sizeof g_req - 1, &out) == 0, "③ 正常编完");
    s2a_compile_outcome_free(&out);
    CHECK(count_files("compile-inflight-") == 0, "③ 正常路径零写盘");
    CHECK(!s2a_direct_unbind_compiler_v1(ctxp(3)), "③ 解绑");

    // ④ 编译器接住了信号、照常返回
    clear_dir();
    CHECK(!s2a_direct_bind_compiler_v1(ctxp(4), survive_compile), "④ 绑替身");
    CHECK(s2a_compile_request_execute(7, 3, g_req, sizeof g_req - 1, &out) == 0, "④ 照常编完");
    s2a_compile_outcome_free(&out);
    CHECK(g_mid_seen == 1, "④ 编译中 dump 确实落了一份（替身在编译器里看到）");
    CHECK(count_files("compile-inflight-") == 0, "④ 编译返回后删掉，不留假现场");
    CHECK(!s2a_direct_unbind_compiler_v1(ctxp(4)), "④ 解绑");

    // ⑤ 别的线程在编时，本线程（没在编）崩：不冒领
    clear_dir();
    CHECK(!s2a_direct_bind_compiler_v1(ctxp(5), block_compile), "⑤ 绑替身");
    pthread_t th; pthread_create(&th, NULL, other_thread, NULL);
    pthread_mutex_lock(&g_mu); while (!g_in_compile) pthread_cond_wait(&g_cv, &g_mu); pthread_mutex_unlock(&g_mu);
    s2a_compile_inflight_dump();
    CHECK(count_files("compile-inflight-") == 0, "⑤ 别的线程正在编的那份不由本线程落盘");
    pthread_mutex_lock(&g_mu); g_release = 1; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    void *rc; pthread_join(th, &rc);
    CHECK(rc == NULL, "⑤ 别的线程照常编完");
    CHECK(count_files("compile-inflight-") == 0, "⑤ 编完也没有文件");

    clear_dir();
    if (fails) { printf("compile_inflight: %d 项红 / %d 项\n", fails, checks); return 1; }
    printf("compile_inflight: %d 项检查，0 项红\n", checks);
    return 0;
}
