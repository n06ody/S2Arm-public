// 客体内存文件后备的空间检查只记不拦（vawin.c guest_file_map，11-13 #894）：临时目录剩余空间不到 S2A_GUEST_FILE_MIN_FREE_MB 时照样接文件，只打一行黄。
//   原先不够就退回匿名内存：判了要接文件的都是匿名装不下的，退回去必定涨过单进程上限被系统收掉（iPhone 15 Pro Max 剩 9 GB < 12 GB，一款 Switch 2 游戏过场后闪退）。
// 期望（按开关语义手写）：门槛设成 1 << 40 MB（任何盘都不够）→ ① s2a_guest_fd() ≥ 0（照样接上文件）；② 窗口照常占到、放开 64 MB 写满，
//   实占增量 < 16 MB（文件映射的脏页不计实占；匿名会 ≥ 48 MB）
// 反证：旧实现（不够就不接）→ ① fd -1、② 实占增量 ≥ 48 MB
#include "../src/native.h"
#include <sys/mman.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
__attribute__((constructor(100))) static void need(void) { setenv("S2A_WIN_NEED_MB", "63488", 1); setenv("S2A_GUEST_FILE", "1", 1); setenv("S2A_GUEST_FILE_MIN_FREE_MB", "1099511627776", 1); setenv("S2A_GUEST_FILE_ANON_BUDGET_MB", "0", 1); }   // 匿名预算 0 = 旧行为（整段进文件）：本测验的是文件后备本身（溢出模式见 t_guest_file_overflow）   // 先于 vawin.c 的构造函数（101）
static long long fp(void) { task_vm_info_data_t i; mach_msg_type_number_t c = TASK_VM_INFO_COUNT; task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&i, &c); return (long long)i.phys_footprint; }
static int bad;
#define CHECK(c, ...) do { if (c) { printf("✓ "); } else { printf("✗ "); bad++; } printf(__VA_ARGS__); putchar('\n'); } while (0)
int main(void) {
    if (!s2a_window_ok()) { printf("✗ t_guest_file_nospace：窗口没占到\n"); return 1; }
    CHECK(s2a_guest_fd() >= 0, "① 空间不够：照样接文件（fd %d，期望 ≥ 0）", s2a_guest_fd());
    uint64_t b, n; s2a_window_get(&b, &n);
    uint8_t *q = (uint8_t *)(uintptr_t)(b + (4ull << 30)); size_t N = 64u << 20;
    if (mprotect(q, N, PROT_READ | PROT_WRITE)) { printf("✗ t_guest_file_nospace：放开 64 MB 失败\n"); return 1; }
    long long f0 = fp(); for (size_t i = 0; i < N; i += 64) q[i] = 1; long long df = fp() - f0;
    CHECK(df < (16ll << 20), "② 接了文件：窗口里写满 64 MB 实占 +%lld KB（期望 < 16384 KB）", df >> 10);
    printf("%s t_guest_file_nospace：%d 项不符\n", bad ? "✗" : "✓", bad);
    return bad != 0;
}
