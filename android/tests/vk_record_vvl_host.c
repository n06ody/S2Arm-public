// SPDX-License-Identifier: Apache-2.0
// Host platform support for the Vulkan record test. The same declarations are
// checked against NDK 29 headers before this file is linked on the host.
#include <android/log.h>
#include <sys/system_properties.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef S2A_RECORD_DIAG_SIGNAL
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
static void report_segv(int sig, siginfo_t *info, void *context) {
    (void)context;
    dprintf(STDERR_FILENO, "record fixture SIGSEGV address=%p\n", info->si_addr);
    void *stack[32];
    int n = backtrace(stack, 32);
    backtrace_symbols_fd(stack, n, STDERR_FILENO);
    _exit(128 + sig);
}
__attribute__((constructor)) static void install_segv_report(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = report_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
}
#endif

int __system_property_get(const char *name, char *value) {
    // Match direct_c3_vk_probe.sh: the standalone replay has no frame-completion
    // hook, so its existing query checks explicitly use immediate mode.
    const char *v = strcmp(name, "debug.s2a.query_wait") == 0 ? "1" : "";
    strcpy(value, v);
    return (int)strlen(v);
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio;
    int prefix = fprintf(stderr, "[%s] ", tag);
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stderr, fmt, ap);
    va_end(ap);
    int end = fputc('\n', stderr);
    return prefix < 0 || n < 0 || end == EOF ? -EPERM : 1;
}
