// 安卓存档原子态轮询（runtime/src/direct_prompt.c 的 __ANDROID__ 分支）宿主单测（#961）：期望值按口径手写。
// 会话状态换成桩；服务层出口 S2A_SaveAtomicState 按 -DS2A_TEST_PROVIDER 决定有没有（没有时弱引用为空，同出包缺导出）。
// 日志经 native.h 的 s2a_log_out 出口截下来数红行。每例 fork 一个子进程，被测函数的静态量从零开始。
#define S2A_LOGCAP_IMPL 1   // 本单元给 s2a_log_out 强定义（截日志），被测单元里的弱直写让位
#include "../../runtime/src/native.h"
#include "s2a_direct_prompt.h"
#include "s2a_session.h"
#include <sys/wait.h>
#include <unistd.h>

static int g_session = S2A_SESSION_RUNNING, g_atomic, g_atomic_calls, g_red_lines;
int s2a_host_session_state(void) { return g_session; }
#ifdef S2A_TEST_PROVIDER
// 静态并入同一 DSO 的服务出口（--exclude-libs 下不可见）：与生产弱引用同一链接方式
__attribute__((visibility("hidden"))) int S2A_SaveAtomicState(void) { g_atomic_calls++; return g_atomic; }
#endif
void s2a_log_out(int fd, const char *p, size_t n) {
    (void)fd;
    if (memmem(p, n, "【红】", strlen("【红】")) && memmem(p, n, "S2A_SaveAtomicState", strlen("S2A_SaveAtomicState"))) g_red_lines++;
    ssize_t w = write(2, p, n); (void)w;
}

static int fails, cases;
#define CASE(name, body) do { cases++; pid_t p = fork(); if (!p) { body; _exit(0); } int st; waitpid(p, &st, 0); \
    if (!WIFEXITED(st) || WEXITSTATUS(st)) { fails++; printf("✗ %s\n", name); } else printf("  ✓ %s\n", name); } while (0)
#define WANT(cond) do { if (!(cond)) { printf("    不符：%s（行 %d）\n", #cond, __LINE__); fflush(stdout); _exit(1); } } while (0)

int main(void) {
    CASE("会话没跑起来不问、不报红", { g_session = S2A_SESSION_STARTING; g_atomic = 2;
        WANT(s2a_save_atomic_poll() == 0); WANT(g_atomic_calls == 0); WANT(g_red_lines == 0); });
#ifdef S2A_TEST_PROVIDER
    CASE("有出口：一直原子不提示", { g_atomic = 0; for (int i = 0; i < 5; i++) WANT(s2a_save_atomic_poll() == 0);
        WANT(g_atomic_calls == 5); WANT(g_red_lines == 0); });
    CASE("有出口：首次非 0 返回原因位一次", { WANT(s2a_save_atomic_poll() == 0); g_atomic = 2; WANT(s2a_save_atomic_poll() == 2);
        g_atomic = 6; WANT(s2a_save_atomic_poll() == 0); WANT(s2a_save_atomic_poll() == 0); WANT(g_red_lines == 0); });
#else
    // 缺出口：不是已确认安全的 0，也不是原因位；每次都答未知，红行只打一次
    CASE("缺出口：会话跑起来后答未知、红一次", { for (int i = 0; i < 3; i++) WANT(s2a_save_atomic_poll() < 0);
        WANT(g_red_lines == 1); });
    CASE("缺出口：跑起来之前不报红，跑起来后才答未知", { g_session = S2A_SESSION_STARTING; WANT(s2a_save_atomic_poll() == 0);
        WANT(g_red_lines == 0); g_session = S2A_SESSION_RUNNING; WANT(s2a_save_atomic_poll() < 0); WANT(g_red_lines == 1); });
#endif
    if (fails) { printf("✗ save_atomic_poll：%d/%d 例不符\n", fails, cases); return 1; }
    printf("✓ save_atomic_poll：%d 例全符\n", cases);
    return 0;
}
