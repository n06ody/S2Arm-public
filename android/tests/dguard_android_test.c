// 安卓接线单测（dguard_android.h）：共用 dguard.c 真跑、真写 $S2A_DOCS/wire/direct_guard；
//   只替换属性开关（S2A_DGUARD_TEST）与提示出口。期望值按 s2a_dguard.h 口径手写：
//   有失败证据的连续 3 次起局后第 4 次必须弹提示（有人局）或收局（无人值守局）；
//   开关关掉整局不武装；提示出口只在满限时调一次；宿主版本号变了从 0 数。
// 一次「起局」 = fork 一个子进程跑胶水函数后 _exit（同 runtime/tests/t_dguard.c 的口径）；
// 提示在孙进程里调，结果经管道传回。
#include "../native/dguard_android.h"
#include "../native/docs_android.h"
#include "s2a_dguard.h"
#include "s2a_docs.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails, enabled = 1, prompts;
static char last_diag[2048];
int s2a_dguard_test_enabled(void) { return enabled; }
// 提示出口：生产是 bridge.cpp 的 JNI 弹窗（玩家选「仍然启动」才返回），测试只记次数与诊断
void s2a_android_dguard_prompt(const char *diag) { prompts++; snprintf(last_diag, sizeof last_diag, "%s", diag); }

struct result { int rc, prompts; char diag[2048]; };

#define WANT(cond) do { if (!(cond)) { printf("    不符：%s（行 %d）\n", #cond, __LINE__); _exit(1); } } while (0)
#define CASE(name, body) do { char d[] = "/tmp/t_dguard_android.XXXXXX"; if (!mkdtemp(d)) { perror("mkdtemp"); return 1; } \
    setenv("S2A_DOCS", d, 1); unsetenv("S2A_AUTOPLAY"); enabled = 1; prompts = 0; \
    pid_t p = fork(); if (!p) { body; _exit(0); } int st; waitpid(p, &st, 0); \
    if (!WIFEXITED(st) || WEXITSTATUS(st)) { fails++; printf("✗ %s\n", name); } else printf("  ✓ %s\n", name); } while (0)
// 一次起局：孙进程里 body 跑完后把 launch 返回值存进 v，结果经管道传回
#define LAUNCH(res, body) do { int fd[2]; WANT(!pipe(fd)); pid_t q = fork(); \
    if (!q) { prompts = 0; last_diag[0] = 0; int v = 0; { body; } \
        struct result r; r.rc = v; r.prompts = prompts; snprintf(r.diag, sizeof r.diag, "%s", last_diag); \
        if (write(fd[1], &r, sizeof r) < 0) _exit(2); _exit(0); } \
    waitpid(q, NULL, 0); ssize_t g = read(fd[0], &res, sizeof res); WANT(g == (ssize_t)sizeof res); close(fd[0]); close(fd[1]); } while (0)
// 记录文件行数（含头行）：pending「?」行 = 已武装未定
static int record_rows(void) { char p[1200]; s2a_docs_path(p, sizeof p, "wire/direct_guard");
    FILE *f = fopen(p, "r"); if (!f) return -1; int n = 0; char line[256]; while (fgets(line, sizeof line, f)) n++; fclose(f); return n; }
static int record_pending(void) { char p[1200]; s2a_docs_path(p, sizeof p, "wire/direct_guard");
    FILE *f = fopen(p, "r"); if (!f) return 0; char line[256]; int pending = 0;
    while (fgets(line, sizeof line, f)) pending = line[0] == '?'; fclose(f); return pending; }
#define PID 0x0100152000022000ull

int main(void) {
    // 开关关掉：launch 返回 0、不武装（没有记录文件）、不弹提示
    CASE("debug.s2a.dguard=0：整局不武装", {
        enabled = 0; struct result r;
        LAUNCH(r, v = s2a_android_dguard_launch(PID, NULL, 1));
        WANT(r.rc == 0 && r.prompts == 0 && record_rows() < 0); });

    // 正常起局：begin（0 次）→ arm → 记录落盘成待定行
    CASE("正常起局：武装落盘待定", {
        struct result r;
        LAUNCH(r, v = s2a_android_dguard_launch(PID, NULL, 1));
        WANT(r.rc == 0 && r.prompts == 0 && record_rows() == 2 && record_pending()); });

    // 两次内存告警 + 一次正常结束：待定行下局丢弃，只数有证据的
    CASE("只数有证据的局", {
        struct result r;
        for (int i = 0; i < 2; i++) LAUNCH(r, v = s2a_android_dguard_launch(PID, NULL, 1); s2a_android_dguard_memory_warning());
        WANT(r.rc == 0);   // 第 2 次起局 begin 读到 1
        LAUNCH(r, v = s2a_android_dguard_launch(PID, NULL, 1));   // 正常退（没证据）：待定行留下，下局读到时丢
        LAUNCH(r, v = s2a_android_dguard_launch(PID, NULL, 1));   // 这局 begin 丢掉上行待定、arm 写新待定
        WANT(record_rows() == 4);   // 头 + 2 条内存告警 + 本次待定
        WANT(record_pending()); });

    // 连续崩 3 次（崩溃证据）：第 4 次起局弹提示一次、诊断带「连续失败 3 次」，然后照旧武装
    CASE("连崩 3 次：弹提示后仍武装", {
        struct result r;
        for (int i = 0; i < 3; i++) LAUNCH(r, v = s2a_android_dguard_launch(PID, NULL, 1); s2a_dguard_evidence("崩溃", 0));
        LAUNCH(r, v = s2a_android_dguard_launch(PID, NULL, 1));
        WANT(r.rc == 0 && r.prompts == 1 && strstr(r.diag, "连续失败 3 次") && record_pending()); });

    // 无人值守局满限：不弹提示、返回非 0 收局、不 arm（记录不再长）
    CASE("S2A_AUTOPLAY 满限：收局不武装", {
        struct result r;
        for (int i = 0; i < 3; i++) LAUNCH(r, v = s2a_android_dguard_launch(PID, NULL, 1); s2a_dguard_evidence("崩溃", 0));
        setenv("S2A_AUTOPLAY", "1", 1);
        LAUNCH(r, v = s2a_android_dguard_launch(PID, NULL, 1));
        WANT(r.rc != 0 && r.prompts == 0 && record_rows() == 4 && !record_pending()); });

    // 宿主版本号变了 = 另一个包：满限记录不复用，照常武装
    CASE("换宿主版本号从 0 数", {
        struct result r;
        s2a_android_host_build_set("v1");
        for (int i = 0; i < 3; i++) LAUNCH(r, v = s2a_android_dguard_launch(PID, NULL, 1); s2a_dguard_evidence("崩溃", 0));
        s2a_android_host_build_set("v2");
        LAUNCH(r, v = s2a_android_dguard_launch(PID, NULL, 1));
        WANT(r.rc == 0 && r.prompts == 0 && record_pending()); });

    // 前台内存告警按证据转正：armed + 告警 → 下局 begin 数到 1
    CASE("前台内存告警记一次失败", {
        struct result r;
        LAUNCH(r, v = s2a_android_dguard_launch(PID, NULL, 1); s2a_android_dguard_memory_warning());
        // 下一局起时 begin 应读到 1（armed 行已转正为证据行）
        LAUNCH(r, v = s2a_android_dguard_launch(PID, NULL, 1));
        WANT(r.rc == 0 && record_rows() == 3);   // 头 + 1 条告警 + 本次待定
        WANT(record_pending()); });

    // 前后台出口不崩、不记证据（生命周期包装只管转发）
    CASE("前后台切换：转发不报错", {
        struct result r;
        LAUNCH(r, v = s2a_android_dguard_launch(PID, NULL, 1); s2a_android_dguard_foreground(0); s2a_android_dguard_foreground(1));
        WANT(r.rc == 0 && record_pending()); });

    // S2A_DOCS/HOME 都没有（安卓现状）：armed + 证据的 save() 落空，下局还是 0——修前红面
    CASE("S2A_DOCS 未设：武装落空下局 0 行", {
        struct result r;
        for (int i = 0; i < 2; i++)
            LAUNCH(r, unsetenv("S2A_DOCS"); setenv("HOME", "/nonexistent-s2a-home", 1);
                   v = s2a_android_dguard_launch(PID, NULL, 1); s2a_dguard_evidence("崩溃", 0);
                   WANT(record_rows() < 0));   // save() 写不进：连头行都没有
        WANT(r.rc == 0 && r.prompts == 0); });

    // docs_bind 绑上本局 state 根（= S2A_DOCS）：跨局计数恢复，3 崩后第 4 局弹提示——修后绿面
    CASE("docs_bind 后跨局计数满限弹提示", {
        struct result r;
        LAUNCH(r, unsetenv("S2A_DOCS"); setenv("HOME", "/nonexistent-s2a-home", 1);
               WANT(s2a_android_docs_bind("relative/path") == EINVAL);   // 非绝对路径拒掉
               WANT(s2a_android_docs_bind(d) == 0);
               WANT(getenv("S2A_DOCS") && !strcmp(getenv("S2A_DOCS"), d)); v = 0);
        for (int i = 0; i < 3; i++)
            LAUNCH(r, unsetenv("S2A_DOCS"); s2a_android_docs_bind(d);
                   v = s2a_android_dguard_launch(PID, NULL, 1); s2a_dguard_evidence("崩溃", 0));
        LAUNCH(r, unsetenv("S2A_DOCS"); s2a_android_docs_bind(d); v = s2a_android_dguard_launch(PID, NULL, 1));
        WANT(r.rc == 0 && r.prompts == 1 && strstr(r.diag, "连续失败 3 次") && record_pending()); });

    if (fails) { printf("dguard android glue: %d 例不符\n", fails); return 1; }
    printf("dguard android glue: passed\n");
    return 0;
}
