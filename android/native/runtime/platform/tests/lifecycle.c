// 生命周期错误传播/顺序单测；guest 停稳证明另见真实 thread + fault 交叉测试。
#include "native.h"
#include "../lifecycle_android.h"
#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <time.h>

void s2a_host_shutdown(const char *why);
void s2a_host_exit_code(int code);
void s2a_term_install(void);
// 宿主没有 logcat；保留生产失败诊断到 stderr，不替换生命周期实现或退出码。
int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio; (void)tag;
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(stderr, fmt, ap);
    va_end(ap); fputc('\n', stderr);
    return n;
}
static const char *mode;
static int threaded_mode;   // 模式名带 -threaded：宿主记的拆线程开关为开
int s2a_android_host_threaded(void) { return threaded_mode; }
static pthread_t main_thread;
static _Atomic unsigned stopped;
static int writers_held;
static int cancelled;
// Fixture-only providers: Vulkan runtime behavior is exercised by direct-vk tests.
static int vk_quiesced, host_waiting, vk_drained;
int s2a_android_vk_quiesce(void) { if (cancelled) _exit(95); vk_quiesced = 1; return !strcmp(mode, "vk-unconfigured") ? -1 : 0; }
void s2a_android_guest_host_wait_begin(void) { if (host_waiting) _exit(97); host_waiting = 1; }
void s2a_android_guest_host_wait_end(void) { if (!host_waiting) _exit(97); host_waiting = 0; }
static void mark(const char *s);
int s2a_android_vk_drain_admitted(void) {
    // Admission closed, no terminal gate yet, owner published census-safe.
    if (!vk_quiesced || cancelled || !host_waiting || vk_drained) _exit(98);
    vk_drained = 1; mark("vk-drain\n");
    return !strcmp(mode, "vk-drain-fail") ? EBUSY : 0;
}
void s2a_direct_summary(int complete) {
    // 从未安装停车通道的一局没有 writer epoch 要停稳（与 writers-fail = “装过但没停稳”区分）。
    if (!complete || (!writers_held && strcmp(mode, "writers-never-installed"))) _exit(96);
}
static void mark(const char *s) { if (write(STDOUT_FILENO, s, strlen(s)) < 0) _exit(80); }
uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void s2a_guest_freeze(int on) { if (on) _exit(81); mark("unfreeze\n"); }
void nxgal_gpu_hold(int on, const char *why) { (void)why; if (on) _exit(82); }
void s2a_track_stat_log(const char *s) { (void)s; }
uint64_t s2a_track_protected_pages(void) { return 0; }
void s2a_direct_res_report(void) {}
void s2a_sched_stat_log(const char *s) { (void)s; }
int s2a_handle_peak(void) { return 0; }
void s2a_yield_stat_log(const char *s) { (void)s; }
void s2a_lockwake_report(void) {}
void s2a_svc_count_report(const char *s) { (void)s; }
void s2a_guest_va_report(const char *s) { (void)s; }
// 地址余量（读 /proc/self/maps）：maps-stuck 模拟 mmap_lock 被挤占、读不完
void s2a_lowva_peak_report(const char *s) { (void)s; }
void s2a_vm_low(const char *s) {
    (void)s;
    mark("vm-low\n");
    if (!strcmp(mode, "maps-stuck")) for (;;) pause();
}
void S2A_CancelWaits(void) {
    // Unconfigured direct Vulkan skips phase 2 entirely; otherwise it must finish first.
    if (vk_drained == !strcmp(mode, "vk-unconfigured") || host_waiting) _exit(99);
    cancelled = 1; mark("cancel\n");
}
int s2a_svc_park_all(int wait_ms) {
    if (wait_ms != 300) _exit(83);
    if (!cancelled) _exit(93);
    mark("park\n");
    return !strcmp(mode, "inflight");
}
// 收局只问“停车通道是否装过”（installed）：默认返回 1 = 装过，照旧走停稳等待；
// mode=writers-never-installed 时返回 0，覆盖“从未安装 → 跳过停稳等待”这一支。
int s2a_android_guest_writers_installed(void) {
    return strcmp(mode, "writers-never-installed") != 0;
}
int s2a_android_guest_shutdown_writers_begin(void) {
    mark("writers\n");
    if (!strcmp(mode, "writers-fail")) return EIO;
    writers_held = 1;
    return 0;
}
void s2a_cs_shutdown(const char *why) {
    (void)why;
    // 从未安装停车通道的一局：按定义没有 writer epoch 要停稳，跳过等待后照常进 cs-shutdown。
    if (!writers_held && strcmp(mode, "writers-never-installed")) _exit(92);
    if (atomic_fetch_add(&stopped, 1)) _exit(84);
    if (!strcmp(mode, "signal") && pthread_equal(pthread_self(), main_thread)) _exit(85);
    mark("cs-shutdown\n");
    if (!strcmp(mode, "hang")) for (;;) pause();
}
void s2a_android_guest_writers_describe(char *out, size_t cap) { snprintf(out, cap, "fixture writers"); }
int s2a_android_runtime_shutdown_result(void) {
    // 同上：从未安装停车通道的一局没有 writer epoch 要停稳，不是“没停稳”。
    if (atomic_load(&stopped) != 1 || (!writers_held && strcmp(mode, "writers-never-installed"))) _exit(94);
    mark("cleanup-result\n");
    return !strcmp(mode, "cleanup-fail") ? EIO : 0;
}
#ifdef S2A_ANDROID_PGO_GEN
void s2a_android_pgo_write(void) { mark("pgo-write\n"); }
#endif
void s2a_widen_scan(void) { mark("summary\n"); }
void s2a_svc_red_summary(void) {}
void s2a_det_summary(void) {}
unsigned s2a_unresolved_count(void) { return 0; }

// #528 第 15 项：会话标记钩子取 files 目录的那一处（真宿主是 bridge.cpp 的 configureGpuDriver）。这里取环境变量：
//   没设时为空，钩子什么都不做、不打日志，和基线逐行比对不受影响。
const char *s2a_android_gpu_driver_files_dir(void) { return getenv("S2A_TEST_FILES_DIR"); }
// 基线的 lifecycle_android.c 没有它：弱引用，只在 sesmark 模式里用
int s2a_android_sesmark_clear(const char *files_dir) __attribute__((weak));

int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *arg) {
    if (!strcmp(mode, "watchdog-fail") || !strcmp(mode, "term-thread-fail")) return EAGAIN;
    return __real_pthread_create(t, a, f, arg);
}
int __real_pipe2(int fd[2], int flags);
int __wrap_pipe2(int fd[2], int flags) {
    if (!strcmp(mode, "term-pipe-fail")) { errno = EMFILE; return -1; }
    return __real_pipe2(fd, flags);
}

int main(int argc, char **argv) {
    if (argc != 2) return 86;
    mode = argv[1]; main_thread = pthread_self();
    static char base_mode[64];
    size_t length = strlen(mode);
    if (length > 9 && length < sizeof base_mode + 9 && !strcmp(mode + length - 9, "-threaded")) {
        threaded_mode = 1;
        memcpy(base_mode, mode, length - 9);
        base_mode[length - 9] = 0;
        mode = base_mode;
    }
    if (!strcmp(mode, "term-thread-fail") || !strcmp(mode, "term-pipe-fail")) {
        struct sigaction before, after;
        sigaction(SIGTERM, NULL, &before);
        int result = s2a_android_term_install();
        sigaction(SIGTERM, NULL, &after);
        if (before.sa_handler != after.sa_handler) return 87;
        return result == (!strcmp(mode, "term-thread-fail") ? EAGAIN : EMFILE) ? 0 : 88;
    }
    if (!strcmp(mode, "signal")) {
        s2a_term_install(); s2a_term_install();
        struct sigaction action;
        sigaction(SIGTERM, NULL, &action);
        if (!(action.sa_flags & SA_ONSTACK)) return 89;
        raise(SIGTERM);
        for (;;) pause();
    }
    if (!strcmp(mode, "sesmark")) {   // 只测删标记的判定本身：打印返回值，不收局
        if (!s2a_android_sesmark_clear) return 97;
        printf("sesmark=%d\n", s2a_android_sesmark_clear(getenv("S2A_TEST_FILES_DIR")));
        return 0;
    }
    if (!strcmp(mode, "full-stderr")) {
        int pipe_fd[2];
        if (pipe2(pipe_fd, O_NONBLOCK)) return 90;
        char bytes[4096] = {0};
        while (write(pipe_fd[1], bytes, sizeof bytes) > 0) {}
        fcntl(pipe_fd[1], F_SETFL, 0);
        dup2(pipe_fd[1], STDERR_FILENO);
        // 保留读端但从不读：共享 LOG 将真堵塞，watchdog 必须独立退出。
    }
    if (!strcmp(mode, "error-stop")) s2a_host_exit_code(86);
    s2a_host_shutdown("fixture shutdown");
    return 91;
}
