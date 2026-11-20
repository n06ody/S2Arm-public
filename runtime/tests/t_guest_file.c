// 客体内存文件后备（vawin.c guest_file_attach / s2a_guest_zero，S2A_GUEST_FILE=1）。
// 背景：11-05 时之笛 Switch 2 iPhone 读档进场景被单进程上限 8192 MB 杀；iOS 判杀按 phys_footprint，匿名脏页全计，文件 MAP_SHARED 映射的脏页不计。
// 期望（按宿主内核行为手写：同机小程序实测匿名写 512 MB 实占 +512 MB、文件映射写 512 MB +0.5 MB；文件打洞后读 0）：
//   ① 窗口文件后备：s2a_guest_fd() ≥ 0；窗口里放开 256 MB 写满，实占增量 < 16 MB（匿名会 +256 MB）
//   ② 还页：s2a_guest_zero 那 64 MB 之后放开再读全是 0，且这段在还页后不可访问（mprotect 才能读）
//   ③ 别名：vm_remap（copy = FALSE，同 svc_mem.c alias_map）把窗口里一页映到别处，两边写互相看得见
//   ④ 还页不影响邻居：还的那段之外紧挨着的页内容不变
#include "../src/native.h"
#include <sys/mman.h>
#include <mach/mach.h>
#include <string.h>
#include <unistd.h>
__attribute__((constructor(100))) static void need(void) { setenv("S2A_WIN_NEED_MB", "63488", 1); setenv("S2A_GUEST_FILE", "1", 1); setenv("S2A_GUEST_FILE_MIN_FREE_MB", "1", 1); setenv("S2A_GUEST_FILE_ANON_BUDGET_MB", "0", 1); }   // 匿名预算 0 = 旧行为（整段进文件）：本测验的是文件后备本身（溢出模式见 t_guest_file_overflow）   // 空间门槛压到 1 MB：门禁机临时盘余量不定，这里只验接上后的行为（空间不够见 t_guest_file_nospace）   // 先于 vawin.c 的构造函数（101）
static long long fp(void) { task_vm_info_data_t i; mach_msg_type_number_t c = TASK_VM_INFO_COUNT; task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&i, &c); return (long long)i.phys_footprint; }
static int bad;
#define CHECK(c, ...) do { if (c) { printf("✓ "); } else { printf("✗ "); bad++; } printf(__VA_ARGS__); putchar('\n'); } while (0)
int main(void) {
    if (!s2a_window_ok()) { printf("✗ t_guest_file：窗口没占到\n"); return 1; }
    uint64_t b, n; s2a_window_get(&b, &n); size_t pg = (size_t)getpagesize();
    CHECK(s2a_guest_fd() >= 0, "① 窗口换成文件后备（fd %d）", s2a_guest_fd());
    {   // ⓪ 匿名预算 0 = 旧行为：判开时整段进文件——没放开过的任意地址都已在文件里，放开前不必按块定（s2a_guest_commit 不改什么）
        void s2a_guest_commit(uint64_t, uint64_t); uint64_t far = b + (20ull << 30);
        CHECK(s2a_guest_in_file(b, 0x4000) == 1 && s2a_guest_in_file(far, 0x4000) == 1 && s2a_guest_in_file(b + n - 0x4000, 0x4000) == 1, "⓪ 预算 0：窗口首 / 中 / 尾没放开过的地址都已在文件里");
        s2a_guest_commit(far, 64u << 20); uint8_t *f = (uint8_t *)(uintptr_t)far;
        if (mprotect(f, 0x4000, PROT_READ | PROT_WRITE)) CHECK(0, "⓪ 放开失败");
        else { f[9] = 0x3c; uint8_t c = 0; CHECK(pread(s2a_guest_fd(), &c, 1, (off_t)(far + 9 - b)) == 1 && c == 0x3c, "⓪ 预算 0：第一次放开写入就在文件里（文件字节 %#x）", c); mprotect(f, 0x4000, PROT_NONE); }
    }
    uint8_t *q = (uint8_t *)(uintptr_t)(b + (4ull << 30)); size_t N = 256u << 20;
    if (mprotect(q, N, PROT_READ | PROT_WRITE)) { printf("✗ t_guest_file：放开 256 MB 失败\n"); return 1; }
    long long f0 = fp(); for (size_t i = 0; i < N; i += 64) q[i] = (uint8_t)(i >> 6) | 1; long long df = fp() - f0;
    CHECK(df < (16ll << 20), "① 窗口里写满 256 MB：实占 +%lld KB（期望 < 16384 KB；匿名内存会 +262144 KB）", df >> 10);
    uint8_t *z = q + (64u << 20);
    CHECK(s2a_guest_zero((uint64_t)(uintptr_t)z, 64u << 20) == 1, "② 还页 64 MB（打洞 + 重映射）");
    CHECK(mprotect(z, 64u << 20, PROT_READ | PROT_WRITE) == 0, "② 还过的段重新放开");
    size_t nz = 0; for (size_t i = 0; i < (64u << 20); i += 64) nz += z[i] != 0;
    CHECK(nz == 0, "② 还过的段读回全 0（非 0 %zu 处）", nz);
    CHECK(z[-64] == (uint8_t)(((64u << 20) - 64) >> 6 | 1) && z[64u << 20] == (uint8_t)(((128u << 20)) >> 6 | 1), "④ 还页段两侧的邻居内容不变（%#x %#x）", z[-64], z[64u << 20]);
    vm_address_t a = 0; vm_prot_t cur, max;
    kern_return_t kr = vm_remap(mach_task_self(), &a, pg, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t)q, FALSE, &cur, &max, VM_INHERIT_SHARE);
    if (kr == KERN_SUCCESS) { uint8_t *al = (uint8_t *)a; al[5] = 0xAB; q[7] = 0xCD;
        CHECK(q[5] == 0xAB && al[7] == 0xCD, "③ vm_remap 别名：别处写 → 窗口读 %#x（期望 0xab），窗口写 → 别处读 %#x（期望 0xcd）", q[5], al[7]); }
    else CHECK(0, "③ vm_remap 别名失败 kr=%d", kr);
    {   // ⑤ 窗口外的文件后备段（远端影子区溢出，far.c）：能映射、写不计实占
        uint8_t *x = s2a_guest_map_any(64u << 20);
        CHECK(x != MAP_FAILED && !mprotect(x, 64u << 20, PROT_READ | PROT_WRITE), "⑤ 窗口外文件后备段 64 MB 映射并放开");
        if (x != MAP_FAILED) { long long g0 = fp(); for (size_t i = 0; i < (64u << 20); i += 64) x[i] = 3; long long dg = fp() - g0;
            CHECK(dg < (8ll << 20) && x[12345 * 64] == 3, "⑤ 写满 64 MB：实占 +%lld KB（期望 < 8192 KB）", dg >> 10); }
    }
    {   // ⑥' 挖不挖（s2a_guest_carve_wanted，11-14）：期望按规则手写——显式设的照办；内存不够那档挖；剩余 ≥ 门槛挖；剩余未知（0）/ 不够不挖
        int s2a_guest_carve_wanted(const char *, int, uint64_t, uint64_t); const uint64_t G = 1ull << 30, MIN = 7 * G;
        CHECK(s2a_guest_carve_wanted("0", 1, 8 * G, MIN) == 0, "⑥' S2A_GUEST_FILE_CARVE=0：不挖（哪怕内存不够那档、剩余够）");
        CHECK(s2a_guest_carve_wanted("1", 0, 0, MIN) == 1, "⑥' S2A_GUEST_FILE_CARVE=1：挖（哪怕剩余未知）");
        CHECK(s2a_guest_carve_wanted(NULL, 1, 1 * G, MIN) == 1, "⑥' 内存不够那档：挖");
        CHECK(s2a_guest_carve_wanted(NULL, 0, 8189ull << 20, MIN) == 1, "⑥' Duo 档剩余 8189 MB ≥ 7168：挖");
        CHECK(s2a_guest_carve_wanted(NULL, 0, 6141ull << 20, MIN) == 0, "⑥' 17 Pro Max 档剩余 6141 MB < 7168：不挖（挖了实占峰约 6.1 GB，贴着上限）");
        CHECK(s2a_guest_carve_wanted(NULL, 0, 3000ull << 20, MIN) == 0, "⑥' 剩余 3000 MB < 7168：不挖");
        CHECK(s2a_guest_carve_wanted(NULL, 0, 0, MIN) == 0, "⑥' 剩余未知（0，Mac）：不挖");
    }
    {   // ⑥ 物理映射区挖回匿名（#964 a，s2a_guest_file_carve）：从没放开过的一段换成匿名占位后，这段「不在文件里」（懒清零 / 还页走匿名），
        //   写进去的计实占、不进文件（文件同偏移读回 0）；段外照旧在文件里。已放开过的段（① 那 256 MB）拒挖、照旧在文件里
        void s2a_guest_file_carve(uint64_t, uint64_t); const uint64_t C = b + (8ull << 30), CN = 256ull << 20;
        s2a_guest_file_carve((uint64_t)(uintptr_t)q, 16u << 20);
        CHECK(s2a_guest_in_file((uint64_t)(uintptr_t)q, 0x4000) == 1, "⑥ 已放开过的段拒挖：照旧在文件里");
        s2a_guest_file_carve(C, CN);
        CHECK(s2a_guest_in_file(C, 0x4000) == 1, "⑥ 缺省（不是「物理内存也装不下」那档）不挖：照旧在文件里");
        setenv("S2A_GUEST_FILE_CARVE", "1", 1); s2a_guest_file_carve(C, CN);
        CHECK(s2a_guest_in_file(C, 0x4000) == 0 && s2a_guest_in_file(C + CN - 0x4000, 0x4000) == 0, "⑥ S2A_GUEST_FILE_CARVE=1 强制挖：挖回的段首尾都不在文件里");
        CHECK(s2a_guest_in_file(C - 0x4000, 0x4000) == 1 && s2a_guest_in_file(C + CN, 0x4000) == 1, "⑥ 紧挨着的段外照旧在文件里");
        CHECK(s2a_guest_zero(C, 0x4000) == 0, "⑥ 挖回的段还页不打洞（回 0，调用方换匿名占位）");
        uint8_t *c = (uint8_t *)(uintptr_t)C; size_t M = 64u << 20;
        if (mprotect(c, M, PROT_READ | PROT_WRITE)) CHECK(0, "⑥ 放开挖回的段失败");
        else { long long g0 = fp(); for (size_t i = 0; i < M; i += 64) c[i] = 7; long long dg = fp() - g0;
            uint8_t buf[4096]; ssize_t r = pread(s2a_guest_fd(), buf, sizeof buf, (off_t)(C - b)); size_t nz = 0; for (ssize_t i = 0; i < r; i++) nz += buf[i] != 0;
            CHECK(dg > (48ll << 20), "⑥ 挖回的段写 64 MB：实占 +%lld KB（匿名计实占，期望 > 49152 KB）", dg >> 10);
            CHECK(r == (ssize_t)sizeof buf && nz == 0 && c[64] == 7, "⑥ 后备文件同偏移读回全 0（数据没进文件，非 0 %zu 处，读 %zd 字节）", nz, r); }
    }
    {   // ⑦ 显式设 S2A_GUEST_FILE_ANON_BUDGET_MB=0 的对照局不写预算记录（vawin.c s2a_guest_budget_note）：心跳报剩余 3000 MB 后，记录目录里没有 anon_budget
        void s2a_guest_budget_note(uint64_t); char d[] = "/tmp/t_gf_recXXXXXX", pk[300], rp[300];
        if (!mkdtemp(d)) CHECK(0, "⑦ 建临时目录失败");
        else { snprintf(pk, sizeof pk, "%s/mem_peak_anon", d); snprintf(rp, sizeof rp, "%s/anon_budget", d); setenv("S2A_GUEST_MEM_PEAK", pk, 1);
            s2a_guest_budget_note(3000ull << 20);
            CHECK(access(rp, F_OK) != 0, "⑦ 显式设 0 的对照局：心跳后没写预算记录（%s）", access(rp, F_OK) ? "没有" : "有");
            unlink(rp); rmdir(d); unsetenv("S2A_GUEST_MEM_PEAK"); } }
    printf("%s t_guest_file：%d 项不符\n", bad ? "✗" : "✓", bad);
    return bad != 0;
}
