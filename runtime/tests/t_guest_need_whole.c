// 整段进文件时心跳记需求只扫游戏放开过的块（vawin.c s2a_guest_file_live + g_gtouch，#1842 后续）。期望按构造手推：
//   整段模式（S2A_GUEST_FILE_ANON_BUDGET_MB=0）：窗口全部块记成文件。放开并写 块 1 整块 32 MB + 块 2 前 16 MB，其余块从没放开过。
//   ① 在内存 ∪ 已落块 = 48 MB；② 这一次只扫了 2 块（修前每 10 秒扫整个窗口：本测 63488 MB 窗口约 1984 块）；
//   ③ 写回 + 作废内存页后仍 48 MB、仍只扫 2 块
#include "../src/native.h"
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
__attribute__((constructor(100))) static void need(void) {
    setenv("S2A_WIN_NEED_MB", "63488", 1); setenv("S2A_GUEST_FILE", "1", 1); setenv("S2A_GUEST_FILE_MIN_FREE_MB", "1", 1); setenv("S2A_GUEST_FILE_ANON_BUDGET_MB", "0", 1);
}
void s2a_guest_commit(uint64_t a, uint64_t n); uint64_t s2a_guest_file_live(void); uint64_t s2a_guest_file_live_scanned(void);
static int bad;
#define CHECK(c, ...) do { if (c) printf("✓ "); else { printf("✗ "); bad++; } printf(__VA_ARGS__); putchar('\n'); } while (0)
#define MB (1ull << 20)
#define GCH (32ull << 20)
int main(void) {
    if (!s2a_window_ok() || s2a_guest_fd() < 0) { printf("✗ t_guest_need_whole：窗口没占到 / 没开文件后备\n"); return 1; }
    uint64_t b, n; s2a_window_get(&b, &n);
    uint64_t B = b + GCH, C = b + 2 * GCH;
    s2a_guest_commit(B, GCH); s2a_guest_commit(C, 16 * MB);
    if (mprotect((void *)(uintptr_t)B, GCH + 16 * MB, PROT_READ | PROT_WRITE)) { printf("✗ 放开失败\n"); return 1; }
    memset((void *)(uintptr_t)B, 5, GCH + 16 * MB);
    uint64_t s0 = s2a_guest_file_live_scanned(), v = s2a_guest_file_live(), s1 = s2a_guest_file_live_scanned();
    CHECK(v == 48 * MB, "① 整段模式写 32 + 16 MB：48 MB（得 %llu MB）", (unsigned long long)(v / MB));
    CHECK(s1 - s0 == 2, "② 只扫放开过的 2 块（得 %llu 块；窗口共 %llu 块）", (unsigned long long)(s1 - s0), (unsigned long long)((n + GCH - 1) / GCH));
    msync((void *)(uintptr_t)B, GCH + 16 * MB, MS_SYNC); msync((void *)(uintptr_t)B, GCH + 16 * MB, MS_INVALIDATE);
    s0 = s2a_guest_file_live_scanned(); v = s2a_guest_file_live(); s1 = s2a_guest_file_live_scanned();
    CHECK(v == 48 * MB && s1 - s0 == 2, "③ 写回 + 逐出后：48 MB、扫 2 块（得 %llu MB、%llu 块）", (unsigned long long)(v / MB), (unsigned long long)(s1 - s0));
    printf("t_guest_need_whole：%s\n", bad ? "有失败" : "全过");
    return bad ? 1 : 0;
}
