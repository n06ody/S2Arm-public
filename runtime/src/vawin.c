// 64 GB 地址窗口（方案 5a-1）：包括 iOS 预热在内，都在最早构造阶段占窗，客体地址空间全部区域在里面排。
// 只占地址、不占内存（footprint 不计）。必须早于 NativeAOT 运行时与 Metal 初始化，否则它们的大块预留可能先落进来。
// S2A_VAWIN_NORESERVE=1（诊断，5b-1 量法）：不占窗口，只在各时刻探测别人有没有映射进来。
#if defined(__ANDROID__) || defined(__OHOS__)
#include "../../android/native/runtime/platform/vawin_android.inc"
#else
#include "native.h"
#include "imgpack.h"
#include "as_type.h"
#include "s2a_docs.h"   // 文档根唯一取法
#include "s2a_launch_ctx.h"
#include <errno.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <TargetConditionals.h>
#ifdef __IPHONE_OS_VERSION_MIN_REQUIRED   // 按 iOS SDK 编才有（Mac SDK 标成不可用）；t_ios_prewarm 在 Mac 上把 TARGET_OS_IPHONE 改成 1 编本文件，不能按它判
#include <os/proc.h>   // os_proc_available_memory：客体内存文件后备的自动判定
#else
#include <dispatch/dispatch.h>   // Mac：系统内存压力监听（文件后备的安全阀，#306）
#endif
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>

static int g_ok;            // 1 = 窗口已占到（首选位置整块 64 GB，或退路：2^39 以下最大空洞里的一块，见 vawin_reserve）
static int g_noreserve;
static uint64_t g_base, g_size;
#if !TARGET_OS_OSX
static uint64_t g_snap_below;   // 即时存档（iOS）：占窗口时在宿主段最底部给命令记录区多留的长度（0 = 没留）——11-20 起不再从窗口里让，恒为 0（旧快照恢复走 cmd_arena_at_snapshot）
static uint64_t g_snap_out;     // 即时存档（iOS）、窗口放不下标准布局：命令记录区 + 虚拟池区放在窗口所在空洞之外的这处（0 = 没放）
#endif
static uint64_t g_keep_lo;   // 放不下标准布局时窗口下方留给宿主的那段起点（0 = 没有）：Mac 复现 iPhone 约束时不占它（iphone_va_fence）
static uint64_t g_res;       // 起局时预留的大小（收尾部前的 g_size）：「还给系统 / 交尾部分配的那段」= [g_base + g_size, g_base + g_res)
// 客体内存改文件后备（S2A_GUEST_FILE；11-05 时之笛 Switch 2 iPhone 读档进场景被单进程上限 8192 MB 杀）。开不开见 s2a_guest_file_wanted：
//   =0 关、其余非空值开；不设时 iOS 上本款应用内存池 > 起局时 os_proc_available_memory() 就开（装不下才付闪存写入的代价），Mac 不设 = 关（Mac 会换页，没有单进程上限）
//   iOS 判杀按 phys_footprint，匿名脏页（含压缩）全计；文件 MAP_SHARED 映射的脏页归文件页缓存，不计（M5 Pro 实测：同样写 2 GB，匿名 +2048 MB、文件 +1 MB；
//   本仓 Mac 小程序：写 512 MB +0.5 MB，逐页读未写页 +0.5 MB）。窗口占到后整段换成一个临时文件（建完立刻 unlink，进程退出 / 被杀时系统收回）的
//   MAP_SHARED 映射，偏移 = 地址 − 窗口基址，保护仍是 PROT_NONE，之后的 mprotect / vm_remap 别名 / 零拷贝 MTLBuffer 照旧（Mac 实测 GPU 读写、两处映射一致）。
//   还页（缩堆、解除物理映射、解别名）改成「打洞 + 原偏移重新映射成 PROT_NONE」：打洞把那段文件块还回去，再读是 0，同匿名占位的语义（s2a_guest_zero）。
//   写保护缺页在文件映射上同样是 SIGBUS（同匿名，Mac 实测）。代价：内存紧张时内核把脏页写回闪存（写量 / 帧时待真机量）
static int g_gfd = -1;
int s2a_guest_fd(void) { return g_gfd; }
// 溢出模式（11-16，docs/坑记录.md「文件后备改溢出模式」）：判开后不再整段换成文件——整段 MAP_SHARED 时 iOS 持续强制写回脏页（诊断 D-JETP：8 GB iPhone、
//   池 3285 MB 的一款 Switch 1 游戏每 10 秒写闪存 2–3.5 GB，判开时实占才约 1.4 GB、离上限还剩 6141 MB，客体全放匿名也装得下）。
//   改为窗口按 GCHUNK 分块，每块在「第一次放开成可读写」时定后备（s2a_guest_commit），定了不再变：已定为匿名的累计 < 预算 → 匿名（保持匿名占位）；
//   否则在这块还是不可访问、没有内容时整块换成文件（同偏移 = 地址 − 窗口基址）。预算：iOS 按本机本款上局实测（s2a_guest_next_budget），Mac = 物理内存 − 预留 − 余量 − 宿主开销（s2a_guest_anon_budget）。
//   预算 0（剩余不够 / S2A_GUEST_FILE_ANON_BUDGET_MB=0）= 旧行为：判开时整段进文件、全部块记成文件（g_gwhole）
#define GCHUNK (32ull << 20)
enum { GC_UNDEC = 0, GC_ANON = 1, GC_FILE = 2, GC_CARVE = 3 };   // 未定 / 匿名 / 文件 / 物理映射区挖回的匿名（第一次放开时才计进匿名累计）
static uint8_t *g_gchunk; static uint64_t g_gchunk_n;   // 每块一字节（窗口基址起）；改只在 g_gchunk_mu 下，读不加锁（定了只在窗口重置 / 放宽时清）
static int g_gwhole;   // 1 = 整段进文件（预算 0，旧行为）
static uint64_t g_anon_budget, g_anon_used;   // 匿名预算；已定为匿名的块累计字节
static pthread_mutex_t g_gchunk_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_gtouch;   // 每块一字节：游戏放开过（s2a_guest_commit 见过）= 1。心跳记需求（s2a_guest_file_live）只扫这些块（#1842 后续：整段 34 GB 每 10 秒扫一遍）
static int gchunk_alloc(uint8_t st) {
    free(g_gchunk); free(g_gtouch); g_gchunk_n = (g_size + GCHUNK - 1) / GCHUNK; g_gchunk = malloc(g_gchunk_n ? g_gchunk_n : 1); g_gtouch = calloc(g_gchunk_n ? g_gchunk_n : 1, 1);
    if (!g_gchunk || !g_gtouch) { free(g_gchunk); free(g_gtouch); g_gchunk = NULL; g_gtouch = NULL; g_gchunk_n = 0; return 0; }
    memset(g_gchunk, st, g_gchunk_n); return 1;
}
static uint8_t gc_get(uint64_t i) { return __atomic_load_n(&g_gchunk[i], __ATOMIC_ACQUIRE); }
static void gc_set(uint64_t i, uint8_t s) { __atomic_store_n(&g_gchunk[i], s, __ATOMIC_RELEASE); }
// 匿名预算（纯函数，t_guest_file_overflow 测）：e = S2A_GUEST_FILE_ANON_BUDGET_MB（设了按设的，0 = 旧行为）；否则 剩余 − 余量 − 宿主开销，不够取 0
uint64_t s2a_guest_anon_budget(const char *e, uint64_t room, uint64_t margin, uint64_t host) {
    if (e && *e) return strtoull(e, NULL, 10) << 20;
    return room > margin + host ? room - margin - host : 0;
}
// iOS 下一局的匿名预算（纯函数，t_guest_file_overflow 测；11-16 审）：宿主开销不能写死——全库文件后备局进程实占（≈宿主开销）中位约 2.1 GB、P90 约 3.8 GB、最大 5.8 GB，
//   按 2048 估，12 GB 机（剩余 8189）上的大款预算给到约 5.6 GB 匿名，再加 3–4 GB 宿主就超上限被系统结束，下一局同样预算再被结束。改成只用实测量：
//   ① e = S2A_GUEST_FILE_ANON_BUDGET_MB 设了按设的（最优先）；② 上一局在前台死掉（died）→ 0 = 整段进文件（旧行为；调用方同时清记录），被结束一次马上退回、不会连着被结束；
//   ③ 有本机记录（上局预算 prev_b、上局溢出模式下心跳读到的离上限剩余最小值 hmin）→ prev_b + (hmin − margin) − GCHUNK，限在 [0, room − margin]：
//      hmin < 余量就收紧，> 余量就放宽（每局最多放宽 hmin − margin − 一块）；④ 没有记录（首局）→ room − margin − first_host（首局宿主开销，缺省 4096 = 全库 P90）。
//   *src：0 显式 / 1 上局死掉 / 2 按上局实测 / 3 首局估计
uint64_t s2a_guest_next_budget(const char *e, uint64_t room, uint64_t margin, uint64_t first_host, int died, int have, uint64_t prev_b, uint64_t hmin, int *src) {
    if (e && *e) { *src = 0; return strtoull(e, NULL, 10) << 20; }
    if (died) { *src = 1; return 0; }
    uint64_t cap = room > margin ? room - margin : 0;
    if (have) {
        *src = 2;
        __int128 v = (__int128)prev_b + (__int128)hmin - (__int128)margin - (__int128)GCHUNK;
        return v <= 0 ? 0 : (uint64_t)v > cap ? cap : (uint64_t)v;
    }
    *src = 3;
    return room > margin + first_host ? room - margin - first_host : 0;
}
// 开不开（纯函数，t_guest_file_auto 测）：e = S2A_GUEST_FILE；pool = 本款应用内存池；room = 起局时离系统给本进程的内存上限还剩多少（os_proc_available_memory，
//   0 = 不知道上限，Mac 恒传 0）；host = 宿主自身开销估计；margin = 余量。设了按设的（"0" 关、其余开）；没设 → 上限已知且「池 + 宿主开销 + 余量」装不下才开。
//   11-06 维护者：原先只比「池 > 剩余」，漏算宿主开销——DOOM Eternal 池 3285 MB、宿主约 2258 MB（Mac 同局足迹 5543 MB），足迹按池算装得下、实际已逼近上限。
//   17 Pro Max（上限 8191 MB）上 Switch 1 款 3285 + 2560 + 512 = 6357 MB 装得下 → 不开、不付闪存写回代价；内存更小的机型、Switch 2 款（池 9 GiB）自动开兜底
// 新判据另起名、不改 native.h（它是抽取阶段键，改声明会让全款重抽取）；native.h 里的 3 参 s2a_guest_file_wanted 保留为「宿主开销与余量取 0」的旧口径
int s2a_guest_file_wanted2(const char *e, uint64_t pool, uint64_t room, uint64_t host, uint64_t margin) {
    return e && *e ? strcmp(e, "0") != 0 : room && pool + host + margin > room;
}
int s2a_guest_file_wanted(const char *e, uint64_t pool, uint64_t room) { return s2a_guest_file_wanted2(e, pool, room, 0, 0); }
// 11-07 卡顿线（火炬之光3 / WWZ 测试设备2 卡顿、整进程停 3–4 s，2 分钟写盘 6–7.5 GB）：剩余 6141 MB 的 iPhone 上 Switch 1 款「池 3285 + 宿主估计 2560 + 余量 512 = 6357」
//   一律判开（878 份日志里 22 款 16 款、130 / 234 局），实际进程实占多在 0.8–1.5 GB。池是客体能要的上界、2560 是 DOOM 的最坏宿主开销，两个最坏叠在一起。改为按实测：
//   ① 设了按设的；② 上限未知（Mac）关；③ 池本身 > 剩余 → 开（Switch 2 池 9 GiB，时之笛）；④ 本机本款记过实测峰值 seen（心跳记：进程实占 + 文件后备里客体已写的字节，
//   s2a_guest_mem_note）→ seen + 余量 > 剩余才开；⑤ 没记过 → 池 + 宿主估计 + 余量 > 剩余才开，宿主估计缺省 2048 MB（11-07 真机开着文件后备的局，进程实占≈宿主开销：
//   Switch 1 15 款 792–1545 MB 的 13 款、2348 / 2573 各 1 款、GRID 4125 MB；GRID 这类首局判关时由 ④ 在下一局纠正）
// ⑥ 安全阀（11-07 维护者）：上一局在前台时死掉（died：「在跑」标记还在，见 s2a_guest_run_mark）→ 这一局强制开。按估计 / 实测判了不开、实际超上限被系统杀的那一局，
//   峰值可能来不及记进 mem_peak（心跳 10 秒一次），不兜住就会每局判关、每局被杀；强制开的这一局记下真实峰值（含文件里的客体字节），下一局再按实测判
// 11-13 #783 改按实测：池（Switch 2 9 GiB）是客体能要的上界，不是实占。时之笛 iPad Pro M2（剩余 8189 MB）：判开的两局 30 分钟往闪存写 189 / 223 GB、
//   掉到 17 帧后卡死重启；碰巧没开的一局稳 30 帧、实占峰值 2118 MB。整段 MAP_SHARED 文件不是换页、是强制写回（坑记录 #306），只在真装不下时才开：
//   设了按设的；room 0（Mac 另判）关；上一局在前台死掉 / 坏记录 → 开；有实测（只记匿名内存局的实占，s2a_guest_mem_note）→ 实测 + 余量 > 剩余才开；
//   没实测 → 关：先用匿名内存，心跳记实占；真装不下被系统收掉的那一局留下「在跑」标记，下一局由 died 开
// 11-13 #964 三档（取代 wanted5 的「没实测就先匿名、死一次再开」：每台设备第一次玩 Switch 2 大款必先被系统杀一次）：
//   #783 的反例拿错了款——iPad 那局「稳 30 帧、匿名 2118 MB」是另一款 Switch 1 游戏；一款 Switch 2 大款（0400486024DEC000） 在那台 iPad（物理内存 7.5 GB）上 8 局全开文件后备、
//   每局写闪存 82–223 GB：游戏碰过的约 4.4 GB + 宿主约 3 GB 超过物理内存，系统边刷边逐出、再被写脏（换页抖动）。同一款在 Duo（物理 11.5 GB）上强制文件后备：
//   正式游戏每 10 秒写盘 0–8 MB、后备文件落盘 4.4 GB 不再涨。所以文件后备能不能用看物理内存，开不开看单进程上限，都不依赖历史：
//   设了按设的；room 0（Mac 另判）关；上局在前台死掉 / 坏记录 开；有实测按实测；没实测时池 ≤ 剩余 关（Switch 1 款，#783 ㊹ 的小内存机型不误开）；
//   池 > 剩余且物理内存 ≥ 池 + 宿主估计 开；物理内存也装不下 → 照样开并 *low = 1（起局提示玩家这台设备内存可能不够这款）。
//     两种都可能坏时按「能玩下去」选：那台 iPad（7.5 GB）上文件后备 8 局每局都玩到约 30 分钟（27000–41000 帧）才掉帧卡死；匿名内存这款要约 7.9 GB、
//     比它的物理内存还大，没有实测，按 iPhone 首局约 8 分钟就被系统结束看更早出局（11-13 维护者定：都装不下时提示 + 照样开文件后备）
int s2a_guest_file_wanted6(const char *e, uint64_t pool, uint64_t room, uint64_t phys, uint64_t seen, uint64_t host, uint64_t margin, int died, int *low) {
    *low = 0;
    if (e && *e) return strcmp(e, "0") != 0;
    if (!room) return 0;
    if (seen == UINT64_MAX) return 1;
    // 安全阀收窄（#2f79dd，11-15）：上局前台死掉本为兜「判了不开、实际超单进程上限被杀」——心跳 10 秒一次，最后那段涨幅可能没记进实测。
    //   本款有实测、且实测 + 两份余量（多一份给漏记的涨幅）仍不超剩余：那次死掉不是超上限（标记被别的原因留下，D-C73M 那次原因未定），不强制开，按实测判关。
    //   D-C73M 6 GB iPhone：实测 2372 + 1024 ≤ 4093 却被强制开，开局写盘 2.6 GB 后卡死在缺页
    if (died && !(seen && seen + 2 * margin <= room)) return 1;
    if (seen) return seen + margin > room;
    if (pool <= room) return 0;
    if (phys && phys >= pool + host) return 1;
    *low = 1; return 1;
}
static uint64_t phys_mem(void) { uint64_t ph = 0; size_t n = sizeof ph; return sysctlbyname("hw.memsize", &ph, &n, NULL, 0) ? 0 : ph; }
// 三档判定的一句理由（日志用）
static const char *tier_why(int on, int low, uint64_t seen, int died) {
    return died && on ? "上一局在前台时死掉，这一局开" : died ? "上一局在前台时死掉，但本款实测峰值 + 两份余量远不到上限（那次不是超上限），保持匿名内存" : seen == UINT64_MAX ? "实测记录读不出（坏记录），保守开"
         : seen ? (on ? "实测峰值 + 余量大于剩余，装不下" : "实测峰值 + 余量不大于剩余，装得下、保持匿名内存")
         : low ? "池大于剩余、物理内存也装不下池 + 宿主估计：照样开（能玩下去优先），提示玩家内存可能不够"
         : on ? "池大于剩余、物理内存装得下：开" : "池不大于剩余：装得下、保持匿名内存";
}
static int g_low_tier;   // 这一局判在「物理内存也装不下」那档（s2a_guest_file_carve 只在这档挖物理映射区）
static void low_ram_notice(uint64_t phys, uint64_t need) {
    g_low_tier = 1;
    LOG("【黄】客体内存：这台设备物理内存 %llu MB，装不下这款要的约 %llu MB（池 + 宿主估计），照样开局；可能卡顿或被系统结束", (unsigned long long)(phys >> 20), (unsigned long long)(need >> 20));
    void (*f)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "s2a_ios_low_ram_notice"); if (f) f();   // ios/app.m：游戏内提示条；单测 / 安卓没有
}
int s2a_guest_file_wanted5(const char *e, uint64_t room, uint64_t seen, uint64_t margin, int died) {
    if (e && *e) return strcmp(e, "0") != 0;
    if (!room) return 0;
    if (died || seen == UINT64_MAX) return 1;   // UINT64_MAX：坏记录（s2a_guest_mem_seen），保守开
    return seen && seen + margin > room;
}
int s2a_guest_file_wanted3(const char *e, uint64_t pool, uint64_t room, uint64_t seen, uint64_t host, uint64_t margin) {
    if (e && *e) return strcmp(e, "0") != 0;
    if (!room) return 0;
    if (pool > room) return 1;
    if (seen) return seen + margin > room;
    return pool + host + margin > room;
}
// 本款本机实测峰值的记录：$HOME/Documents/wire/<程序号大写 16 位>/mem_peak_anon（同 pso_user.tsv 的目录），一行十进制字节数 = 没开文件后备那几局的进程实占峰值。
//   旧记录 mem_peak 记的是「实占 + 文件里已落块的客体字节」，文件块只涨不降（懒清零后不打洞），开过一次就一直判开（#783），换名不再读它。程序号按名字取（单测只链 vawin.c）
static int mem_peak_path(char *out, size_t n);
// 「本局在跑」标记（同目录 running）：App 到前台时写、进后台 / 将终止时删（ios/app.m 调 s2a_guest_run_mark）。iOS 上玩家划掉 / 系统在后台回收都不走正常退出，
//   所以「干净」= 进了后台；标记还在 = 上一局在前台时死掉（被内存上限杀、崩溃）。预热进程没到前台不写，被系统回收不误判。崩溃也会留标记：多开一局文件后备，可接受
static int run_mark_path(char *out, size_t n) {
    const char *ov = getenv("S2A_GUEST_RUN_MARK"); if (ov && *ov) { snprintf(out, n, "%s", ov); return 1; }
    char p[1024]; if (!mem_peak_path(p, sizeof p)) return 0;
    char *sl = strrchr(p, '/'); if (!sl) return 0; *sl = 0; snprintf(out, n, "%s/running", p); return 1;
}
// 11-15（#909）：闪退不再算「死于内存」。我们的崩溃处理（crash.c crash_reraise）在按信号死之前把标记改写成「crash」：内存上限是系统 SIGKILL，
//   收不到信号、标记仍是 pid → 照旧开；标记写着 crash = 上一局是我们自己崩的（坏访问 / abort），与内存无关，不强制开、按实测判。
//   #909 iPhone 17 Pro Max：10 局里 2 局因上一局闪退被强制开，开着的局每 10 秒写闪存 2.7–3.4 GB、一局累计 21.9 GB，关着的局整局 19–34 MB
static char g_run_mark[1100];   // 本局写过的标记路径（崩溃处理里只能用异步信号安全的调用，路径事先存好）
int s2a_guest_run_died(void) {
    char p[1100], b[8] = {0}; if (!run_mark_path(p, sizeof p)) return 0;
    int fd = open(p, O_RDONLY); if (fd < 0) return 0;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd);
    return !(n >= 5 && !memcmp(b, "crash", 5));
}
int s2a_guest_run_crashed(void) { char p[1100]; struct stat st; return run_mark_path(p, sizeof p) && !stat(p, &st) && !s2a_guest_run_died(); }   // 只给日志说原因
void s2a_guest_run_mark_crash(void) {   // 异步信号安全：open / write / close
    if (!g_run_mark[0]) return;
    int fd = open(g_run_mark, O_WRONLY | O_TRUNC); if (fd < 0) return;
    ssize_t w = write(fd, "crash\n", 6); (void)w; close(fd);
}
void s2a_guest_run_mark(int running) {
    char p[1100]; if (!run_mark_path(p, sizeof p)) return;
    if (!running) { unlink(p); g_run_mark[0] = 0; return; }
    snprintf(g_run_mark, sizeof g_run_mark, "%s", p);
    { char d[1100]; snprintf(d, sizeof d, "%s", p); char *sl = strrchr(d, '/'); if (sl) { *sl = 0; mkdir(d, 0755); } }
    FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
}
// 构造期（占窗口、判文件后备开不开）用的程序号：need_from_npdm 从本款 main.npdm 直接读（svc_mem.c s2a_npdm_pid），不经图形模式（审计 P1-139：
//   那时 iOS 的 s2a_gfx_mode 还没放行、程序号恒 0，Mac 上调它会把模式和程序号 0 落定）。多款容器构造期没选游戏，保持 0（按最大池判，#103）
static uint64_t g_ctor_pid; static int g_attach_phase;
static int mem_peak_path(char *out, size_t n) {
    uint64_t (*pidf)(void) = (uint64_t (*)(void))dlsym(RTLD_DEFAULT, "s2a_npdm_program_id"); const char *ov = getenv("S2A_GUEST_MEM_PEAK");
    if (ov && *ov) { snprintf(out, n, "%s", ov); return 1; }   // 单测 / 诊断指定
    uint64_t pid = g_attach_phase ? g_ctor_pid : pidf ? pidf() : 0;   // 构造期只认 npdm 直读的；局中（心跳记峰值）照旧按当前程序号
    // 局中当前程序号还取不到（iOS 图形模式没放行时恒 0）→ 退回构造期从本款 main.npdm 读到的号（多款容器构造期没选游戏，那个号也是 0，不变）。
    //   开发乙 11-15 Duo 读数：启动上下文直接起容器里的一款，App 到前台写「在跑」标记时这里取不到号、整局没写标记，真被内存上限杀了下一局也认不出
    if (!pid) pid = g_ctor_pid;
    if (!pid) return 0;
    s2a_docs_path(out, n, "wire/%016llX/mem_peak_anon", (unsigned long long)pid); return 1;
}
// 读本款实测峰值：没有记录 → 0（按估计判）；记录在但读不出 / 为 0 → S2A_GUEST_SEEN_BAD（坏记录，按装不下判，保守开；性能线 A 审：不能因读坏文件判关）
#define S2A_GUEST_SEEN_BAD UINT64_MAX
uint64_t s2a_guest_mem_seen(void) {
    char p[1024]; if (!mem_peak_path(p, sizeof p)) return 0;
    FILE *f = fopen(p, "r"); if (!f) return errno == ENOENT ? 0 : S2A_GUEST_SEEN_BAD;
    unsigned long long v = 0; int ok = fscanf(f, "%llu", &v) == 1 && v; fclose(f); return ok ? v : S2A_GUEST_SEEN_BAD;
}
// 每局记自己的需求、下一局起局按上一局结算（#1634，11-19 维护者：峰值只增不减 + 开着文件后备的局不记 → 一局异常就永远判开，玩家每局写闪存 16–45 GB）。
//   本局需求 = 匿名内存的局：进程实占；开着文件后备的局：进程实占 + 文件块里「在内存或已落在后备文件里」的客体页（s2a_guest_file_live；
//     文件页不计实占，换成匿名就都要算）——两种局都量得出「匿名要多少」。
//     #1842（11-21）：原先只加「窗口里在内存的页」（mincore），已写回文件、又被系统逐出的客体页两头都不算：iPhone 15 Pro Max 一款大作开着文件后备的局量出 4181 MB，
//     把它上一局匿名实测的 5835 MB 往下改成 4181，下一局按 4181 + 512 ≤ 6140 判关，匿名内存进程实占 1.7–2.3 → 4.1 GB、3 分钟进热状态 2、30 → 26 帧
//   心跳把本局最大需求与心跳次数写进同目录 mem_peak_run（「字节 心跳数 文件后备」）。下一局起局（s2a_guest_mem_settle）按上一局结算进 mem_peak_anon：
//     ① 上一局在前台死掉、且需求 + 两份余量 > 剩余（像是撞了单进程上限）：记录取大、标「死过」——死过的记录只认够长的匿名局往下改，
//        文件后备局在内存紧的机器上页被逐出、量小了，不许它往下改，免得「改小 → 匿名被杀 → 开 → 又改小」来回；
//     ② 上一局干净（或死掉但离上限远，不是内存杀的）且够长（心跳 ≥ 30 次 = 5 分钟）：记录换成这一局的需求，可以往下改（一局异常下一局就纠正）；
//     ③ 太短的局：只往上改（菜单里待 30 秒的局代表不了进场景要多少）。
//   记录格式「字节[ died]」，旧记录（只有字节）读作没死过
enum { GMEM_BEATS_MIN = 30 };
static int run_peak_path(char *out, size_t n) {
    const char *ov = getenv("S2A_GUEST_MEM_PEAK_RUN"); if (ov && *ov) { snprintf(out, n, "%s", ov); return 1; }   // 单测指定
    char p[1024]; if (!mem_peak_path(p, sizeof p)) return 0;
    char *sl = strrchr(p, '/'); if (!sl) return 0; *sl = 0; snprintf(out, n, "%s/mem_peak_run", p); return 1;
}
static int write_line(const char *p, const char *line) {   // tmp + rename
    char t[1100]; { char d[1024]; snprintf(d, sizeof d, "%s", p); char *sl = strrchr(d, '/'); if (sl) { *sl = 0; mkdir(d, 0755); } }
    snprintf(t, sizeof t, "%s.tmp", p); FILE *f = fopen(t, "w"); if (!f) return -1;
    fputs(line, f); fclose(f); return rename(t, p);
}
static int rec_died(const char *p) { FILE *f = fopen(p, "r"); if (!f) return 0; unsigned long long v = 0; char w[16] = {0}; int n = fscanf(f, "%llu %15s", &v, w); fclose(f); return n == 2 && !strcmp(w, "died"); }
// 起局判文件后备之前调：把上一局的 mem_peak_run 结算进记录、删掉它。died = 上一局在前台死掉；room = 此刻离单进程上限的剩余；margin = 余量。返回一句说法（日志用，没有上一局记录给 NULL）
const char *s2a_guest_mem_settle(int died, uint64_t room, uint64_t margin) {
    static char why[200]; char rp[1100], pp[1024];
    if (!run_peak_path(rp, sizeof rp) || !mem_peak_path(pp, sizeof pp)) return NULL;
    FILE *f = fopen(rp, "r"); if (!f) return NULL;
    unsigned long long need = 0, beats = 0; int file = 0; int n = fscanf(f, "%llu %llu %d", &need, &beats, &file); fclose(f); unlink(rp);
    if (n < 2 || !need) return NULL;
    uint64_t rec = s2a_guest_mem_seen(); if (rec == S2A_GUEST_SEEN_BAD) rec = 0;
    int sticky = rec && rec_died(pp), mem_kill = died && need + 2 * margin > room, longrun = beats >= GMEM_BEATS_MIN;
    uint64_t v; int vs; const char *how;
    if (mem_kill) { v = need > rec ? need : rec; vs = 1; how = "上一局在前台死掉、需求逼近上限：取大、记「死过」"; }
    else if (longrun && !(sticky && file)) { v = need; vs = 0; how = need < rec ? "上一局够长：换成它的需求（往下改）" : "上一局够长：换成它的需求"; }
    else { v = need > rec ? need : rec; vs = sticky; how = sticky && file && longrun ? "记录死过、上一局开着文件后备：不往下改" : "上一局太短：只往上改"; }
    char line[64]; snprintf(line, sizeof line, vs ? "%llu died\n" : "%llu\n", (unsigned long long)v); write_line(pp, line);
    snprintf(why, sizeof why, "%s（上一局需求 %llu MB、%llu 次心跳%s；记录 %llu → %llu MB）", how, need >> 20, beats, file ? "、文件后备" : "", (unsigned long long)(rec >> 20), (unsigned long long)(v >> 20));
    return why;
}
// 文件后备局「换成匿名要多少」里的客体那份：文件块里在内存或已落在后备文件里的页（定义在块状态表后面，s2a_guest_file_live）
uint64_t s2a_guest_file_live(void);
// 心跳调（每 10 秒，后台串行队列）：footprint = 进程实占字节。记本局最大需求与心跳次数（见上）
static void span_note(void);
void s2a_guest_mem_note(uint64_t footprint) {
    span_note();   // 心跳顺带记本局跨度（#783）
    static uint64_t run_max, beats;
    uint64_t need = footprint + (g_gfd >= 0 ? s2a_guest_file_live() : 0);
    if (need > run_max) run_max = need;
    beats++;
    char p[1100], line[96]; if (!run_peak_path(p, sizeof p)) return;
    snprintf(line, sizeof line, "%llu %llu %d\n", (unsigned long long)run_max, (unsigned long long)beats, g_gfd >= 0); write_line(p, line);
}
// 本款本机的溢出模式预算记录（iOS，同 mem_peak_anon 目录 anon_budget）：一行「本局预算 MB  本局溢出模式下心跳读到的离上限剩余最小值 MB  本局起局时的剩余 MB」。
//   第三项（11-16 真机）：同一台 12 GB iPhone 两局起局剩余一局 8189、下一局 6141——上限跟着变，上局的「剩余最小值」是按上局上限量的，
//   下一局上限低了多少，可用余量就要扣多少（s2a_guest_overflow_budget_ios）；上限高了不加（保守）。旧的两项记录第三项当 0 = 不扣
//   起局按它定下一局预算（s2a_guest_next_budget）；上一局在前台死掉时清掉。剩余最小值由心跳更新（s2a_guest_budget_note），变小才写（tmp + rename）
static int budget_rec_path(char *out, size_t n) {
    char p[1024]; if (!mem_peak_path(p, sizeof p)) return 0;
    char *sl = strrchr(p, '/'); if (!sl) return 0; *sl = 0; snprintf(out, n, "%s/anon_budget", p); return 1;
}
int s2a_guest_budget_rec_read(uint64_t *b, uint64_t *hmin) {   // 两项都读到回 1（字节）
    char p[1100]; if (!budget_rec_path(p, sizeof p)) return 0;
    FILE *f = fopen(p, "r"); if (!f) return 0;
    unsigned long long x = 0, y = 0; int ok = fscanf(f, "%llu %llu", &x, &y) == 2; fclose(f);
    if (ok) { *b = (uint64_t)x << 20; *hmin = (uint64_t)y << 20; }
    return ok;
}
static uint64_t g_budget_room;   // 本局定预算时的起局剩余（写进记录第三项）
uint64_t s2a_guest_budget_rec_room(void) {   // 记录第三项（字节）；没有 / 旧记录回 0
    char p[1100]; if (!budget_rec_path(p, sizeof p)) return 0;
    FILE *f = fopen(p, "r"); if (!f) return 0;
    unsigned long long x = 0, y = 0, z = 0; int ok = fscanf(f, "%llu %llu %llu", &x, &y, &z) == 3; fclose(f);
    return ok ? (uint64_t)z << 20 : 0;
}
static void budget_rec_clear(void) { char p[1100]; if (budget_rec_path(p, sizeof p)) unlink(p); }
static int budget_rec_write(uint64_t b, uint64_t hmin) {
    char p[1100], t[1200]; if (!budget_rec_path(p, sizeof p)) return 0;
    { char d[1100]; snprintf(d, sizeof d, "%s", p); char *sl = strrchr(d, '/'); if (sl) { *sl = 0; mkdir(d, 0755); } }
    snprintf(t, sizeof t, "%s.tmp", p); FILE *f = fopen(t, "w"); if (!f) return 0;
    fprintf(f, "%llu %llu %llu\n", (unsigned long long)(b >> 20), (unsigned long long)(hmin >> 20), (unsigned long long)(g_budget_room >> 20)); fclose(f);
    return !rename(t, p);
}
// 起局（iOS）定溢出模式预算：读记录、上一局死掉就清，交 s2a_guest_next_budget；*pb / *ph 回读到的上局记录（日志用）。单测在 Mac 上直接调
uint64_t s2a_guest_overflow_budget_ios(const char *e, uint64_t room, uint64_t margin, int died, int *src, uint64_t *pb, uint64_t *ph) {
    *pb = *ph = 0; int have = !died && s2a_guest_budget_rec_read(pb, ph);
    uint64_t pr = have ? s2a_guest_budget_rec_room() : 0;
    if (have && pr > room) *ph = *ph > pr - room ? *ph - (pr - room) : 0;   // 这局上限比上局低：上局的剩余最小值按差额扣（*ph 回扣过的值，日志用）
    if (died) budget_rec_clear();
    g_budget_room = room;
    { const char *v = getenv("S2A_GUEST_FILE_OVERFLOW_HOST_MB"); uint64_t fh = (v && *v ? strtoull(v, NULL, 10) : 4096ull) << 20;
      return s2a_guest_next_budget(e, room, margin, fh, died, have, *pb, *ph, src); }
}
static uint64_t g_hmin = UINT64_MAX;   // 本局开着文件后备时心跳读到的离上限剩余最小值
static int g_rec_off;   // 1 = 显式设 S2A_GUEST_FILE_ANON_BUDGET_MB=0 的对照局：不写记录
// 心跳调（iOS，s2a_guest_backing_log 里，avail = os_proc_available_memory()）：开着文件后备时比小，变小才写记录（预算 = 本局的）。
//   整段模式（预算 0）也写（B 记 0）：上局死掉清了记录、这局整段，照写 B = 0 与 Hmin，下一局按整段模式下的实测余量回升（0 + Hmin − 余量 − 一块）；
//   否则算出 0 后不再有新记录，永远卡在整段。显式设 0 的对照局不写
void s2a_guest_budget_note(uint64_t avail) {
    if (g_gfd < 0 || !g_gchunk || g_rec_off || avail >= g_hmin) return;
    if (budget_rec_write(g_anon_budget, avail)) g_hmin = avail;
}
// 本款本机学到的地址跨度（#783）：同目录 span_peak，一行十进制字节数 = 局中碰到的最高地址 − 窗口基址（svc_mem.c s2a_mem_span_now，心跳每 10 秒比大才写）。
//   窗口放不下标准布局时按它给窗口、其余留给宿主（s2a_window_host_below）；没记录 / 读不出 = 0（照旧只留 HOST_BELOW）
static uint64_t read_u64(const char *p) { FILE *f = fopen(p, "r"); if (!f) return 0; unsigned long long v = 0; int ok = fscanf(f, "%llu", &v) == 1; fclose(f); return ok ? v : 0; }
static int span_path(char *out, size_t n, uint64_t pid) {   // pid = 0：按局中当前程序号（同 mem_peak 目录）
    const char *ov = getenv("S2A_WIN_SPAN_PEAK"); if (ov && *ov) { snprintf(out, n, "%s", ov); return 1; }   // 单测 / 诊断指定
    if (pid) { s2a_docs_path(out, n, "wire/%016llX/span_peak", (unsigned long long)pid); return 1; }
    if (!mem_peak_path(out, n)) return 0;
    char *sl = strrchr(out, '/'); if (!sl) return 0; snprintf(sl + 1, n - (size_t)(sl + 1 - out), "span_peak"); return 1;
}
uint64_t s2a_window_span_seen(uint64_t pid) { char p[1024]; return pid && span_path(p, sizeof p, pid) ? read_u64(p) : 0; }
static uint64_t g_span_learned;   // 本款（多款容器 = 各款最大，有一款没记录就 0）学到的跨度
uint64_t s2a_window_span_learned(void) { return g_span_learned; }   // 单测 t_vawin_container 读
// 自愈：窗口按学到的跨度收窄后布局仍放不下（svc_mem.c 起局判「连模块段、栈、堆前段都放不下」那一刻）——这份记录不可信，删掉，下一局退回缺省给法
void s2a_window_span_forget(void) {
    if (!g_span_learned) return;
    char p[1024]; if (!span_path(p, sizeof p, 0)) return;
    if (!unlink(p)) LOG("【黄】地址窗口：按学到的跨度 %llu MB 收窄后放不下本款布局，删掉这份记录（%s），下一局按缺省给窗口", (unsigned long long)(g_span_learned >> 20), p);
}
// 只记真跑起来的局（11-12 iPhone 实机实测回归）：一局刚开就死（缺直连库、崩、被杀）只碰到 2.26 GB，记下后下一局窗口按它缩到 5 GB，
//   连模块段、栈、堆前段都放不下、起不了局；起不来的局永远写不出更大的值，从此每次都起不来。出图满 SPAN_MIN_PRESENTS 次之后才记
#define SPAN_MIN_PRESENTS 300
static uint64_t g_need, g_fit;   // 定义见下（占窗口时定）；记跨度先要用 g_fit
// 已有记录还算不算数（#1143 续）：不小于空洞的记录装不进任何窗口（旧版把窗口外别名区的远端映射也算进跨度，某机记成 73 GB），按没有处理、允许覆盖，
//   否则它只增不减、每局宿主都只剩 3 GB。fit = 0（还没占窗口 / 单测）不判。纯函数，t_vawin_learned 测
int s2a_window_span_rec_ok(uint64_t rec, uint64_t fit) { return rec && (!fit || rec < fit); }
int s2a_window_span_worth(uint64_t presents) { return presents >= SPAN_MIN_PRESENTS; }   // 纯函数，t_vawin_learned 测
static void span_note(void) {   // s2a_mem_span_now 在 svc_mem.c、mtl_present_count 在直连图形层：按名字取（只链 vawin.c 的单测没有它们；取不到 = 不记）
    static uint64_t (*spanf)(void), (*presf)(void); static int looked;
    if (!looked) { spanf = (uint64_t (*)(void))dlsym(RTLD_DEFAULT, "s2a_mem_span_now"); presf = (uint64_t (*)(void))dlsym(RTLD_DEFAULT, "mtl_present_count"); looked = 1; }
    if (!presf || !s2a_window_span_worth(presf())) return;
    uint64_t now = spanf ? spanf() : 0; if (!now) return;
    char p[1024], t[1100]; if (!span_path(p, sizeof p, 0)) return;
    static uint64_t rec; static int loaded; if (!loaded) { rec = read_u64(p); if (!s2a_window_span_rec_ok(rec, g_fit)) rec = 0; loaded = 1; }
    if (now <= rec) return;
    { char d[1024]; snprintf(d, sizeof d, "%s", p); char *sl = strrchr(d, '/'); if (sl) { *sl = 0; mkdir(d, 0755); } }
    snprintf(t, sizeof t, "%s.tmp", p); FILE *f = fopen(t, "w"); if (!f) return;
    fprintf(f, "%llu\n", (unsigned long long)now); fclose(f);
    if (!rename(t, p)) rec = now;
}
// 低区耗尽死的局也记跨度（#1143，11-15 iPhone 读数）：多款容器里一款没有记录时按它自己的标准布局算（75964 MB），窗口一点不缩、下方只留 3 GiB 给宿主；
//   这款加载中几秒内低区碰底（pthread_create / 着色器库映射失败，最大空洞 6 MB）死在首帧前，心跳（10 秒一次、出图满 300 次才记）一次都赶不上，
//   永远学不到、永远死。改为崩溃那一刻低区确实碰底（最大空洞 < 64 MB）就把本局已碰到的跨度记下——这是它「至少要这么多」的下界，下一局窗口按它缩、其余给宿主；
//   碰得更高就再记大（心跳 / 下次崩溃都比大才写），缩过头布局放不下走起局自愈（s2a_window_span_widen）。别的原因早死（低区没碰底）不记，同 11-12 那条。
//   崩溃处理里只用 open / read / write / close / rename：路径和取跨度的函数在 C# 初始化后预先备好（s2a_window_span_arm，host_main.c 调）
#define SPAN_CRASH_LOW (64ull << 20)
int s2a_window_span_crash_worth(uint64_t low_largest) { return low_largest < SPAN_CRASH_LOW; }   // 纯函数，t_vawin_learned 测
static char g_span_crash_p[1024], g_span_crash_t[1100];
static uint64_t (*g_span_crash_f)(void);
void s2a_window_span_arm(void) {
    char p[1024]; if (!span_path(p, sizeof p, 0)) return;
    { char d[1024]; snprintf(d, sizeof d, "%s", p); char *sl = strrchr(d, '/'); if (sl) { *sl = 0; mkdir(d, 0755); } }
    g_span_crash_f = (uint64_t (*)(void))dlsym(RTLD_DEFAULT, "s2a_mem_span_now");
    snprintf(g_span_crash_t, sizeof g_span_crash_t, "%s.tmp", p); snprintf(g_span_crash_p, sizeof g_span_crash_p, "%s", p);
}
uint64_t s2a_window_span_crash(uint64_t low_largest) {   // 返回记下的跨度（没记 = 0）
    if (!g_span_crash_p[0] || !g_span_crash_f || !s2a_window_span_crash_worth(low_largest)) return 0;
    uint64_t now = g_span_crash_f(), rec = 0; if (!now) return 0;
    char b[32]; int fd = open(g_span_crash_p, O_RDONLY);
    if (fd >= 0) { ssize_t n = read(fd, b, sizeof b - 1); close(fd); for (ssize_t i = 0; i < n && b[i] >= '0' && b[i] <= '9'; i++) rec = rec * 10 + (uint64_t)(b[i] - '0'); }
    if (!s2a_window_span_rec_ok(rec, g_fit)) rec = 0;
    if (now <= rec) return 0;
    char d[24]; int k = (int)sizeof d; d[--k] = '\n'; uint64_t v = now; do { d[--k] = (char)('0' + v % 10); v /= 10; } while (v && k > 0);
    fd = open(g_span_crash_t, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) return 0;
    ssize_t w = write(fd, d + k, sizeof d - (size_t)k); close(fd);
    if (w != (ssize_t)(sizeof d - (size_t)k) || rename(g_span_crash_t, g_span_crash_p)) return 0;
    return now;
}
static uint64_t env_mb(const char *k, uint64_t def) { const char *v = getenv(k); return (v && *v ? strtoull(v, NULL, 10) : def) << 20; }
static int g_ctr;
static uint64_t g_ctr_pool;   // 多款容器：包内各款应用池的最大值（构造期还没选游戏，文件后备按最大的判）
uint64_t s2a_window_container_pool(void) { return g_ctr_pool; }
// Mac 开不开（纯函数，t_guest_file_auto 测；#306）：设了按设的（"0" 关、其余开）；没设 → ① 池本身大于「物理内存 − 预留」（room，0 = 读不到）就开：
//   池放不进物理内存，第一局就要兜底，不等压力记录（PR #353，wwmogu）；② 否则本机本款上一局到过系统内存压力「严重」档才开（池装得下但实际挤爆的那种）。
//   不加宿主开销估计、不看实测峰值：峰值含文件里已落块的客体字节，懒清零后只涨不降，开过一次就会一直判开（#353）
int s2a_guest_file_wanted_mac(const char *e, uint64_t pool, uint64_t room, int pressed) {
    if (e && *e) return strcmp(e, "0") != 0;
    return (room && pool > room) || pressed;
}
// 压力记录：同 mem_peak 目录的 mem_pressure（存在 = 到过「严重」档；单测 / 诊断用 S2A_GUEST_PRESSURE_MARK 指定）。记下后一直有效：内存不够是这台机器跑这款的属性
static int pressure_path(char *out, size_t n) {
    const char *ov = getenv("S2A_GUEST_PRESSURE_MARK"); if (ov && *ov) { snprintf(out, n, "%s", ov); return 1; }
    char p[1024]; if (!mem_peak_path(p, sizeof p)) return 0;
    char *sl = strrchr(p, '/'); if (!sl) return 0; *sl = 0; snprintf(out, n, "%s/mem_pressure", p); return 1;
}
int s2a_guest_pressure_seen(void) { char p[1100]; struct stat st; return pressure_path(p, sizeof p) && !stat(p, &st); }
static char g_pressure_path[1100];   // 起局时定（构造期按 npdm 程序号），压力回调里只写这一处
// 记一次「严重」：写记录（只写一次）。压力回调调；单测直接调
void s2a_guest_pressure_note(void) {
    static int done; if (done) return;
    if (!g_pressure_path[0] && !pressure_path(g_pressure_path, sizeof g_pressure_path)) { g_pressure_path[0] = 0; return; }
    done = 1;
    { char d[1100]; snprintf(d, sizeof d, "%s", g_pressure_path); char *sl = strrchr(d, '/'); if (sl) { *sl = 0; mkdir(d, 0755); } }
    FILE *f = fopen(g_pressure_path, "w"); if (f) { fprintf(f, "critical %ld\n", (long)time(NULL)); fclose(f); }
    LOG("【黄】系统内存压力到「严重」档：已记下（%s），下一局本款自动开客体内存文件后备（#306 安全阀）", f ? "写成" : "写不了记录");
}
#ifndef __IPHONE_OS_VERSION_MIN_REQUIRED
// Mac 匿名内存模式下监听系统内存压力（libdispatch，后台队列）；程序号未知（多款容器构造期）就不记
static void guest_pressure_watch(void) {
    if (!pressure_path(g_pressure_path, sizeof g_pressure_path)) { g_pressure_path[0] = 0; return; }
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0, DISPATCH_MEMORYPRESSURE_CRITICAL, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    if (!src) return;
    dispatch_source_set_event_handler(src, ^{ if (dispatch_source_get_data(src) & DISPATCH_MEMORYPRESSURE_CRITICAL) s2a_guest_pressure_note(); });
    dispatch_resume(src);
}
#endif
static void guest_file_map(uint64_t room);
// 装游戏前要整段换成文件（s2a_guest_file_retry）：只由下面多款容器「装游戏前按本款再判」判开时置位（剩余空间不再拦，见 guest_file_map）
static int g_gfile_pending;
// 多款容器起局时还没选游戏（程序号 0，读不到本款实测 / 「在跑」标记）：先用匿名内存、记下起局时的剩余，装游戏模块前按本款再判一次（s2a_guest_file_retry，#783）
static int g_gfile_late; static uint64_t g_gfile_room0;
static int g_gfile_died;   // 这一局判定时「上一局在前台死掉」（iOS；溢出模式预算按它退回整段进文件）
static uint64_t g_gfile_room;   // 文件后备判定时离单进程上限的剩余（s2a_guest_file_carve 按它决定挖不挖物理映射区）
static void guest_file_attach_(void);
static void guest_file_attach(void) { g_attach_phase = 1; guest_file_attach_(); g_attach_phase = 0; }
// 测试开关 S2A_TEST_BALLAST_MB=<N>（11-12，小内存机型问题在大内存设备上复现用；缺省不设 = 不占）：模拟给 App 少 N MB 内存的机型（#807 6 GB 机型约 4 GB）。
//   分两步：① 起局判文件后备时，离单进程上限的余量先扣掉 N MB（判定结果同小内存机型）；② 布局排完、装游戏模块之前（host_main.c load_session_modules）
//   才真占住并逐页写满 N MB 匿名内存、整局不还（实占与系统可用内存都少这么多，内存吃紧时内核换出文件后备脏页的时机也接近）。
//   不在起局时就占：那时地址窗口刚占好、布局还没排，几 GB 匿名映射会挤掉布局要的地址（11-12 实测：报「地址窗口连模块段都放不下」）。
//   只认环境变量，玩家从图标启动不带它
static uint64_t test_ballast_bytes(void) { const char *e = getenv("S2A_TEST_BALLAST_MB"); return e && *e ? strtoull(e, NULL, 10) << 20 : 0; }
void s2a_test_ballast_commit(void) {
    static int done; uint64_t n = test_ballast_bytes(); if (!n || done++) return;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); uint8_t *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { LOG("【红】测试压舱：占 %llu MB 匿名内存失败（errno=%d）", (unsigned long long)(n >> 20), errno); return; }
    for (uint64_t o = 0; o < n; o += 16384) p[o] = (uint8_t)(o >> 14 | 1);   // 每页写一字节：页真提交、算进实占（全零页会被压缩，写非零）
#ifdef __IPHONE_OS_VERSION_MIN_REQUIRED
    uint64_t room = os_proc_available_memory();
#else
    uint64_t room = 0;
#endif
    LOG("【黄】测试压舱：装模块前占住并写满 %llu MB 匿名内存（%.1f 秒），此刻离单进程内存上限还剩 %llu MB（S2A_TEST_BALLAST_MB，只给测试局）",
        (unsigned long long)(n >> 20), (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e9, (unsigned long long)(room >> 20));
}
static void guest_file_attach_(void) {
    // 池：s2a_app_pool（layout.c；Switch 3285 MiB、Switch 2 9 GiB）。此刻已定：上一步 need_from_npdm → s2a_npdm_window_need → npdm_read 认出 Switch 2 时就设了 9 GiB。
    //   它是游戏能向系统要的内存总量（SetHeapSize 上限 = 池 − 映像），客体脏页最多涨到这么多；窗口跨度（几十 GB 地址）不是内存，堆区随池定，game.tsv 的 app_pool 是图形层的、要到图形初始化才读。
    //   按名字取（dlsym）：单测只链 vawin.c、不链 layout.c；取不到记 0（自动判定按装得下 = 关）
    const char *e = getenv("S2A_GUEST_FILE");
    uint64_t (*poolf)(void) = (uint64_t (*)(void))dlsym(RTLD_DEFAULT, "s2a_app_pool"); uint64_t pool = poolf ? poolf() : 0;
    if (g_ctr && g_ctr_pool > pool) pool = g_ctr_pool;   // 多款容器：不按缺省池判，否则 Switch 2 款进局后内存超单进程上限被杀（#103）
#ifdef __IPHONE_OS_VERSION_MIN_REQUIRED
    uint64_t room = os_proc_available_memory(), ballast = test_ballast_bytes();   // 预热进程（ActivePrewarm）此刻的上限可能是后台档，偏小只会多开、不会漏开
    // 余量缺省 512 MB（S2A_GUEST_FILE_MARGIN_MB）；池只作日志里的上界，不作判据（#783，见 s2a_guest_file_wanted5）
    if (ballast) { room = room > ballast ? room - ballast : 1; LOG("【黄】测试压舱：文件后备判定按余量少 %llu MB 算（S2A_TEST_BALLAST_MB，装模块前才真占）", (unsigned long long)(ballast >> 20)); }
    uint64_t margin = env_mb("S2A_GUEST_FILE_MARGIN_MB", 512), host = env_mb("S2A_GUEST_FILE_HOST_MB", 2048), phys = phys_mem();
    int died = room && s2a_guest_run_died(), low = 0;
    { const char *st = room ? s2a_guest_mem_settle(died, room, margin) : NULL; if (st) LOG("客体内存实测记录：%s", st); }
    uint64_t seen = s2a_guest_mem_seen();
    if (room && s2a_guest_run_crashed()) LOG("客体内存文件后备：上一局在前台闪退（我们的崩溃处理记过现场，不是被内存上限杀），不强制开、按实测判");
    char pp0[1024]; int known = mem_peak_path(pp0, sizeof pp0);   // 多款容器还没选游戏：池是容器缺省、读不到本款记录，先不判（装游戏前按本款再判）
    int on = s2a_guest_file_wanted6(e, known || (e && *e) ? pool : 0, room, phys, seen, host, margin, died, &low);
    char pp[1024]; int late = !on && !(e && *e) && room && !mem_peak_path(pp, sizeof pp);   // 多款容器还没选游戏：装游戏前按本款再判
    const char *why = e && *e ? "按 S2A_GUEST_FILE 显式设定" : !room ? "自动：不知道单进程上限（Mac 会换页），缺省关"
                    : late ? "自动：还没选游戏（多款容器），先用匿名内存，装游戏前按本款实测再判"
                    : died && on ? "自动：上一局在前台时死掉（「在跑」标记还在：被内存上限杀 / 崩溃 / 测试局在前台被 devicectl 结束），这一局开"
                    : seen == UINT64_MAX ? "自动：本款实测峰值记录读不出（坏记录），保守开"
                    : tier_why(on, low, seen, died);
    LOG("客体内存文件后备：%s——%s；本款应用内存池 %llu MB、离单进程内存上限还剩 %llu MB%s、物理内存 %llu MB、宿主估计 %llu MB、本机本款实测峰值 %llu MB（0 = 没有）%s", on ? "开" : "关", why,
        (unsigned long long)(pool >> 20), (unsigned long long)(room >> 20), room ? "" : "（0 = 未知）", (unsigned long long)(phys >> 20), (unsigned long long)(host >> 20),
        (unsigned long long)(seen == UINT64_MAX ? 0 : seen >> 20), s2a_process_prewarmed() ? "；预热进程" : "");
    if (low && !late) low_ram_notice(phys, pool + host);
    g_gfile_room = room; g_gfile_died = died;
    if (late) { g_gfile_late = 1; g_gfile_room0 = room; return; }
    if (!on) return;
    guest_file_map(room);
#else
    // Mac 缺省匿名内存，池本身放不进物理内存、或本机本款上一局到过系统内存压力「严重」档时开文件后备（s2a_guest_file_wanted_mac；#306 / #353）。
    //   原先按「物理内存 − 6 GB」判（#41 / #68），16 GB 机器跑 Switch 2 款一律开，撞上 macOS 脏文件页上限：脏页过阈值后每缺一页都等内核写回一页，
    //   阈值随物理内存缩放（16 GB Mac 逐页写共享文件映射，2.2 GB 后 1.5 → 26 µs/页 = SSD 写速；128 GB 机器写 8 GB 无拐点；匿名 0.7 µs/页）。
    //   游戏内停 141 / 397 s、一局写盘 31 GB（#306 贡献者 M1 Pro 16 GB）。匿名内存只在真不够时由系统压缩 / 换出；真不够（#68：压缩器吃满、WindowServer 被杀）由压力记录兜住
    {
        uint64_t ph = 0, room = 0; size_t phn = sizeof ph, reserve = env_mb("S2A_GUEST_FILE_MAC_RESERVE_MB", 6144);
        if (!sysctlbyname("hw.memsize", &ph, &phn, NULL, 0) && ph > reserve) room = ph - reserve;
        int pressed = s2a_guest_pressure_seen(), on = s2a_guest_file_wanted_mac(e, pool, room, pressed);
        LOG("客体内存文件后备：%s——%s；本款应用内存池 %llu MB、物理内存 − 预留 %llu MB", on ? "开" : "关",
            e && *e ? "按 S2A_GUEST_FILE 显式设定" : room && pool > room ? "自动：池本身大于物理内存 − 预留，装不下"
            : pressed ? "自动：本机本款上一局到过系统内存压力「严重」档" : "自动：Mac 缺省匿名内存（系统按需压缩 / 换出）",
            (unsigned long long)(pool >> 20), (unsigned long long)(room >> 20));
        if (!on) { guest_pressure_watch(); return; }
        guest_file_map(room);   // 匿名预算按同一口径（物理内存 − 预留）算
        return;
    }
#endif
}
// 判了开之后接上临时文件（iOS / Mac 共用）：room = 起局时离单进程上限的剩余（Mac：物理内存 − 预留），按它定匿名预算。
//   预算 > 0：溢出模式——只建文件、定长到窗口大小，窗口保持匿名占位，块到第一次放开时再定（s2a_guest_commit）；预算 0：整段换成文件（旧行为）
static void guest_file_map(uint64_t room) {
    const char *td = getenv("TMPDIR"); char path[1024]; snprintf(path, sizeof path, "%s/s2a_guest_%d.bin", td && *td ? td : "/tmp", (int)getpid());
    // 闪存剩余空间只记、不拦（11-13 #894：门槛原是 12 GB「游戏写满整个池」的最坏情况，iPhone 15 Pro Max 剩 9 GB 就退回匿名内存——
    //   走到这里都是判了要接文件（实测装不下 / 上一局在前台死掉 / 显式要），匿名内存必定涨过单进程上限被系统收掉（一款 Switch 2 游戏开场过场后闪退、无崩溃报告）；
    //   接文件只有真把剩余空间写满才出事。文件稀疏，只占内核写回的块，最多涨到游戏弄脏的客体内存总量。剩余低于 S2A_GUEST_FILE_MIN_FREE_MB（缺省 12288）只打一行黄
    { const char *m = getenv("S2A_GUEST_FILE_MIN_FREE_MB"); uint64_t need = (m && *m ? strtoull(m, NULL, 10) : 12288ull) << 20; struct statfs sf;
      if (statfs(td && *td ? td : "/tmp", &sf)) LOG("【黄】客体内存文件后备：查不了临时目录剩余空间（errno=%d），照样接文件", errno);
      else {
          uint64_t avail = (uint64_t)sf.f_bavail * (uint64_t)sf.f_bsize;
          // iPhone：f_bavail 不含系统随时能清掉的空间（「设置 → 储存空间」的「可用」含它），常小好几 GB；有「重要用途」口径就取大的那个（ios/app.m）
          { uint64_t (*important)(const char *) = (uint64_t (*)(const char *))dlsym(RTLD_DEFAULT, "s2a_ios_free_important");
            uint64_t imp = important ? important(td && *td ? td : "/tmp") : 0; if (imp > avail) avail = imp; }
          if (avail < need) LOG("【黄】客体内存文件后备：临时目录剩余 %llu MB < 游戏最坏会写脏的 %llu MB（S2A_GUEST_FILE_MIN_FREE_MB）：照样接文件（匿名内存一定超单进程上限被系统收掉）；真写满剩余空间时这局会因写不进退出，腾出空间更稳",
                                (unsigned long long)(avail >> 20), (unsigned long long)(need >> 20));
      } }
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { LOG("【红】客体内存文件后备：建不了临时文件（errno=%d），照旧用匿名内存", errno); return; }
    unlink(path);
    if (ftruncate(fd, (off_t)g_size)) { LOG("【红】客体内存文件后备：文件定长 %llu MB 失败（errno=%d），照旧用匿名内存", (unsigned long long)(g_size >> 20), errno); close(fd); return; }
    const char *be = getenv("S2A_GUEST_FILE_ANON_BUDGET_MB");
    uint64_t margin = env_mb("S2A_GUEST_FILE_MARGIN_MB", 512), budget; char why[200];
#ifdef __IPHONE_OS_VERSION_MIN_REQUIRED   // iOS 有单进程上限：按本机本款实测定（s2a_guest_next_budget）
    { int src = 0; uint64_t pb = 0, ph = 0; budget = s2a_guest_overflow_budget_ios(be, room, margin, g_gfile_died, &src, &pb, &ph);
      if (src == 0) snprintf(why, sizeof why, "按 S2A_GUEST_FILE_ANON_BUDGET_MB 显式设定");
      else if (src == 1) snprintf(why, sizeof why, "上一局在前台死掉：退回整段进文件，清掉本款预算记录");
      else if (src == 2) snprintf(why, sizeof why, "按上局实测余量：上局预算 %llu MB + (上局离上限剩余最小 %llu MB（已按两局起局剩余之差扣过）− 余量 %llu MB) − 一块 %llu MB，上限 剩余 %llu − 余量",
                                  (unsigned long long)(pb >> 20), (unsigned long long)(ph >> 20), (unsigned long long)(margin >> 20), (unsigned long long)(GCHUNK >> 20), (unsigned long long)(room >> 20));
      else { const char *v = getenv("S2A_GUEST_FILE_OVERFLOW_HOST_MB");
          snprintf(why, sizeof why, "首局估计：剩余 %llu MB − 余量 %llu MB − 首局宿主开销 %llu MB（S2A_GUEST_FILE_OVERFLOW_HOST_MB）", (unsigned long long)(room >> 20),
                   (unsigned long long)(margin >> 20), v && *v ? strtoull(v, NULL, 10) : 4096ull); } }
#else   // Mac：没有单进程上限，剩余 = 物理内存 − 预留，宿主开销按 S2A_GUEST_FILE_HOST_MB（缺省 2048）
    { uint64_t host = env_mb("S2A_GUEST_FILE_HOST_MB", 2048); budget = s2a_guest_anon_budget(be, room, margin, host);
      if (be && *be) snprintf(why, sizeof why, "按 S2A_GUEST_FILE_ANON_BUDGET_MB 显式设定");
      else snprintf(why, sizeof why, "Mac：物理内存 − 预留 %llu MB − 余量 %llu MB − 宿主开销 %llu MB", (unsigned long long)(room >> 20), (unsigned long long)(margin >> 20), (unsigned long long)(host >> 20)); }
#endif
    g_rec_off = be && *be && !budget;   // 显式设 0 的对照局不写预算记录
    if (!gchunk_alloc(budget ? GC_UNDEC : GC_FILE)) { LOG("【红】客体内存文件后备：块状态表分配失败，照旧用匿名内存"); close(fd); return; }
    if (budget) {   // 溢出模式：窗口保持匿名占位
        g_gwhole = 0; g_anon_budget = budget; g_anon_used = 0; g_gfd = fd;
        LOG("客体内存文件后备：开（溢出模式）——匿名预算 %llu MB（%s），块 %llu MB；预算内的块用匿名内存，超出的块第一次放开时换成临时文件（已 unlink，稀疏，窗口 %#llx +%llu MB）",
            (unsigned long long)(budget >> 20), why, (unsigned long long)(GCHUNK >> 20), (unsigned long long)g_base, (unsigned long long)(g_size >> 20));
        return;
    }
    void *p = mmap((void *)g_base, g_size, PROT_NONE, MAP_FIXED | MAP_SHARED, fd, 0);
    if (p != (void *)g_base) {   // 失败时那段可能已被拆掉：盖回匿名占位
        int er = errno; mmap((void *)g_base, g_size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0); close(fd); free(g_gchunk); g_gchunk = NULL; free(g_gtouch); g_gtouch = NULL; g_gchunk_n = 0;
        LOG("【红】客体内存文件后备：窗口 %#llx +%llu MB 映射成文件失败（errno=%d），照旧用匿名内存", (unsigned long long)g_base, (unsigned long long)(g_size >> 20), er); return; }
    g_gwhole = 1; g_anon_budget = g_anon_used = 0; g_gfd = fd;
    LOG("客体内存文件后备：窗口 %#llx +%llu MB 映射到临时文件（已 unlink，稀疏），脏页不计进程实占（S2A_GUEST_FILE）；匿名预算 0（%s）：整段进文件（旧行为）",
        (unsigned long long)g_base, (unsigned long long)(g_size >> 20), why);
}
// 开局装模块前（host_main.c load_session_modules）：起局时判了要接文件、但空间不够的，此刻（解包已完）再查一次、够了就接上。
//   只在窗口还原封不动（整段一块 PROT_NONE 的匿名保留）时才换：换成文件是整段 MAP_FIXED，窗口里已经有东西就会被抹掉，那种情形不换、照旧匿名。
//   还是不够：报红，并在屏幕上告诉玩家差多少空间（不然这款几分钟后涨到单进程上限被系统收掉，没有崩溃报告，玩家只看到闪退）
#ifdef S2A_UNIT_TEST
// 快照相关的弱缺省已由本文件后部的通用定义提供（合并 upstream 后统一，勿在此重复定义）
void s2a_guest_file_test_pending(void) { g_gfile_pending = 1; }
void s2a_guest_file_test_late(void) { g_gfile_late = 1; }   // 单测（t_guest_file_retry ④）：造「多款容器、已选定游戏、要按本款再判」的状态   // 单测（t_guest_file_retry）：造「装游戏前要换成文件」的状态（真机只由多款容器按本款再判置位，Mac 上 room 0 走不到）
#endif
int s2a_guest_file_retry(void) {
    if (g_gfile_late && g_gfd < 0 && g_ok) {   // 多款容器：此刻已选定游戏，按本款实测 / 「在跑」标记再判（剩余用起局时那一刻的，和单款同口径）
        g_gfile_late = 0;
        uint64_t margin = env_mb("S2A_GUEST_FILE_MARGIN_MB", 512), host = env_mb("S2A_GUEST_FILE_HOST_MB", 2048), phys = phys_mem();
        { const char *st = s2a_guest_mem_settle(s2a_guest_run_died(), g_gfile_room0, margin); if (st) LOG("客体内存实测记录：%s", st); }
        uint64_t seen = s2a_guest_mem_seen();
        // 此刻已选定游戏、模块目录已是这一款，但布局还没按本款排（s2a_app_pool 仍是容器缺省）：按本款 main.npdm 取池（Switch 2 9 GiB），读不到退回 s2a_app_pool
        uint64_t pool = 0;
        { const char *(*dirf)(void) = (const char *(*)(void))dlsym(RTLD_DEFAULT, "s2a_module_dir"); uint64_t (*npf)(const char *) = (uint64_t (*)(const char *))dlsym(RTLD_DEFAULT, "s2a_npdm_app_pool");
          if (dirf && npf && dirf()) { char np[1100]; snprintf(np, sizeof np, "%s/main.npdm", dirf()); pool = npf(np); }
          if (!pool) { uint64_t (*poolf)(void) = (uint64_t (*)(void))dlsym(RTLD_DEFAULT, "s2a_app_pool"); pool = poolf ? poolf() : 0; } }
        int died = s2a_guest_run_died(), low = 0; g_gfile_died = died;
        int on = s2a_guest_file_wanted6(NULL, pool, g_gfile_room0, phys, seen, host, margin, died, &low);
        if (s2a_guest_run_crashed()) LOG("客体内存文件后备：上一局在前台闪退（我们的崩溃处理记过现场，不是被内存上限杀），不强制开、按实测判");
        LOG("客体内存文件后备（装游戏前按本款再判）：%s——%s；本款应用内存池 %llu MB、起局时离单进程内存上限还剩 %llu MB、物理内存 %llu MB、宿主估计 %llu MB、本机本款实测峰值 %llu MB（0 = 没有）",
            on ? "开" : "关", tier_why(on, low, seen, died), (unsigned long long)(pool >> 20), (unsigned long long)(g_gfile_room0 >> 20), (unsigned long long)(phys >> 20),
            (unsigned long long)(host >> 20), (unsigned long long)(seen == UINT64_MAX ? 0 : seen >> 20));
        if (low) low_ram_notice(phys, pool + host);
        // 补写「本局在跑」标记（died 已在上面读过，不会读到自己）：标记按程序号放目录，App 到前台时（ios/app.m）多款容器还没选游戏、程序号是 0，
        //   路径取不到就没写——诊断 D-4PMS（6 GB iPhone，多款容器）一款 Switch 2 大款连着 5 局都在前台被内存上限杀，每局再判仍是「实测峰值装得下、关」，
        //   安全阀一次没触发。此刻已选定游戏、程序号确定，在这里补；进后台 / 将终止时删照旧（那时也认得程序号）
        s2a_guest_run_mark(1);
        if (!on) return 0;
        g_gfile_pending = 1;   // 下面整段换成文件：窗口原封不动才换
    }
    if (!g_gfile_pending || g_gfd >= 0 || !g_ok) return 0;
    // 窗口起局后分成几段保留（构造时就不止一段），逐段看：全部仍是 PROT_NONE、非共享 = 还没放开过、没有内容
    for (vm_address_t a = (vm_address_t)g_base, end = (vm_address_t)(g_base + g_size); a < end; ) {
        vm_address_t ra = a; vm_size_t sz = 0; vm_region_basic_info_data_64_t bi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &ra, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &obj) != KERN_SUCCESS || ra >= end) break;
        if (bi.protection != VM_PROT_NONE || bi.shared) {
            LOG("【黄】客体内存文件后备：装游戏前想补接文件，但窗口里已有放开过的映射（%#llx +%llu MB，权限 %d），不换，照旧匿名内存", (unsigned long long)ra, (unsigned long long)(sz >> 20), bi.protection);
            g_gfile_pending = 0; return 0; }
        a = ra + sz;
    }
    guest_file_map(g_gfile_room);   // 匿名预算按起局时的剩余（多款容器 g_gfile_room0 与它同值；Mac / 单测造的状态为 0 → 预算 0、整段进文件）
    g_gfile_pending = 0;
    if (g_gfd >= 0) { LOG("客体内存文件后备：装游戏前按本款判开，接上了"); return 1; }
    return 0;
}
// 窗口外也要文件后备的一段（远端影子区堆区余量用完、改从宿主任意空闲地址分时，far.c）：文件往后接长，按新偏移映射到内核挑的地址（同匿名的落点规则，不多占窗口外地址）。
//   没开文件后备返回 MAP_FAILED（调用方照旧匿名）。这些段还页时不打洞（s2a_guest_zero 只认窗口），换匿名占位：语义同样是零，只是那几页回到匿名记账
static pthread_mutex_t g_gextra_mu = PTHREAD_MUTEX_INITIALIZER; static uint64_t g_gextra;
// 文件往后接长 len，返回这段的偏移；失败返回 UINT64_MAX
static uint64_t gextra_take(uint64_t len) {
    pthread_mutex_lock(&g_gextra_mu); uint64_t off = g_size + g_gextra; int ok = !ftruncate(g_gfd, (off_t)(off + len)); if (ok) g_gextra += len; pthread_mutex_unlock(&g_gextra_mu);
    return ok ? off : UINT64_MAX;
}
// 映射失败时退回刚接长的那段（审计 P2-264）：还在文件末尾才退、截回去；后面已有别人接上就留成空洞（稀疏文件，不占盘）
static void gextra_undo(uint64_t off, uint64_t len) {
    pthread_mutex_lock(&g_gextra_mu);
    if (g_size + g_gextra == off + len && !ftruncate(g_gfd, (off_t)off)) g_gextra -= len;
    pthread_mutex_unlock(&g_gextra_mu);
}
void *s2a_guest_map_any(uint64_t len) {
    if (g_gfd < 0 || !len) return MAP_FAILED;
    uint64_t off = gextra_take(len);
    if (off == UINT64_MAX) return MAP_FAILED;
    void *p = mmap(NULL, len, PROT_NONE, MAP_SHARED, g_gfd, (off_t)off);
    if (p == MAP_FAILED) gextra_undo(off, len);
    return p;
}
// 线性远端影子（far.c，#419）的一块也要文件后备：文件往后接长，按新偏移映射到 far.c 自己预留的线性影子区里的固定地址（那段是 far.c 占的不可访问预留，
//   MAP_FIXED 只盖它自己的地址）。没开文件后备回 1（调用方用预留里的匿名页）；成功回 0，失败回 -1
int s2a_guest_map_fixed(uint64_t a, uint64_t len) {
    if (g_gfd < 0) return 1;
    if (!len) return 0;
    uint64_t off = gextra_take(len);
    if (off == UINT64_MAX) return -1;
    if (mmap((void *)(uintptr_t)a, len, PROT_NONE, MAP_FIXED | MAP_SHARED, g_gfd, (off_t)off) == (void *)(uintptr_t)a) return 0;
    gextra_undo(off, len);
    return -1;
}
// 文件后备窗口里挖回匿名的一段（#964 a）：物理映射区（MapPhysicalMemory 的别名区）是游戏 NVN 内存池所在，GPU 每帧写的零拷贝缓冲都在这里——
//   11-13 Duo 正式游戏实测窗口脏页约 1.3 GB，最脏的 4 段里两段在别名区（各 130–185 MB）。放在文件里，物理内存小的机器（iPad 7.5 GB）脏页超回写阈值后
//   这些每帧都被改写的页会被反复刷盘（那台 iPad 文件后备局每 10 秒写 2–3 GB）。别名区保持匿名：热页不进文件，只有游戏堆走文件后备。
//   代价是这段的常驻页计进进程实占（Duo 正式游戏约 1.0–1.7 GB），iPad 档单进程上限 8 GB 装得下。下面两个口子按它分段：在这段里一律当「不在文件里」
//   （svc_mem.c 的懒清零与还页就走匿名那套）。S2A_GUEST_FILE_CARVE=0 不挖、=1 强制挖（对照用）；缺省只在「物理内存也装不下」那档挖
static uint64_t g_anon_lo, g_anon_hi;
static int reserve_hole(uint64_t lo, uint64_t hi) {   // [lo, hi) 放回不可访问的匿名占位（空洞补占位；MAP_FIXED 也盖掉原映射）；空段回 0；不成返回非 0
    return lo < hi && mmap((void *)(uintptr_t)lo, hi - lo, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) != (void *)(uintptr_t)lo;
}
static int in_anon(uint64_t a, uint64_t n) { return g_anon_hi > g_anon_lo && a < g_anon_hi && a + n > g_anon_lo; }
// 布局定下别名区后调（svc_mem.c，那时这段还没放开过）：与窗口的交集按宿主页向内取整，逐块确认仍是不可访问的保留（没有内容）才换成匿名占位
// 挖不挖（纯函数，单测直接测）：S2A_GUEST_FILE_CARVE=0 / 1 按设的；「物理内存也装不下」那档挖；离单进程上限剩余 ≥ S2A_GUEST_FILE_CARVE_ROOM_MB（缺省 7168）也挖——
//   11-14 实测：写盘几乎全来自物理映射区，而且写回的大多是游戏已经释放、内容作废的页（释放不打洞、重新给出时 memset 清零，内存一紧整段写回）：每次起局加载时一次写 7–17 GB
//   （一台测试 iPhone 两局 16.7 / 6.7 GB；Duo 挖 0 写 9.4 GB、挖 1 写 33 MB）。挖的代价是这段常驻计实占（Duo 15 分钟：实占峰 3370 → 6101 MB，离上限最少还剩 2122 MB）。挖了的实占峰是绝对量（约 6.1 GB = 文件后备峰约 3.4 GB + 挖区约 2.7 GB），门槛再留约 1 GB：起局剩余 ≥ 7 GB 才挖——
//   17 Pro Max 那档剩余约 6.1 GB，挖了会贴着上限，不挖（曾按 4 GB 定，算错）。剩余未知（0，Mac）不挖
int s2a_guest_carve_wanted(const char *e, int low, uint64_t room, uint64_t room_min) {
    if (e && *e) return *e != '0';
    if (low) return 1;
    return room && room >= room_min;
}
void s2a_guest_file_carve(uint64_t a, uint64_t n) {
    if (g_gfd < 0 || !n) return;
    const char *e = getenv("S2A_GUEST_FILE_CARVE");
    if (e && *e == '0') { LOG("客体内存文件后备：物理映射区照旧进文件（S2A_GUEST_FILE_CARVE=0，对照用）"); return; }
    // 挖不挖按 s2a_guest_carve_wanted（上面）：「物理内存也装不下」那档挖；起局时离单进程上限剩余 ≥ S2A_GUEST_FILE_CARVE_ROOM_MB（缺省 7168）也挖；
    //   S2A_GUEST_FILE_CARVE=1 / 0 强制。挖与不挖都写一行带剩余与原因（看玩家单用）
    uint64_t room_min = env_mb("S2A_GUEST_FILE_CARVE_ROOM_MB", 7168);
    if (!s2a_guest_carve_wanted(e, g_low_tier, g_gfile_room, room_min)) { LOG("客体内存文件后备：物理映射区照旧进文件（不是内存不够那档，离单进程上限剩余 %llu MB < %llu MB：挖了常驻会贴近上限）", (unsigned long long)(g_gfile_room >> 20), (unsigned long long)(room_min >> 20)); return; }
    const uint64_t pg = 0x4000;
    uint64_t lo = a > g_base ? a : g_base, hi = a + n < g_base + g_size ? a + n : g_base + g_size;
    lo = (lo + pg - 1) & ~(pg - 1); hi &= ~(pg - 1);
    if (lo >= hi) return;
    for (vm_address_t r = (vm_address_t)lo; r < hi; ) {
        vm_address_t ra = r; vm_size_t sz = 0; vm_region_basic_info_data_64_t bi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &ra, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &obj) != KERN_SUCCESS || ra >= hi) break;
        if (bi.protection != VM_PROT_NONE) { LOG("【黄】客体内存文件后备：物理映射区 %#llx 起已有放开过的映射（权限 %d），不挖，整段照旧进文件", (unsigned long long)ra, bi.protection); return; }
        r = ra + sz;
    }
    if (mmap((void *)(uintptr_t)lo, hi - lo, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) != (void *)(uintptr_t)lo) {
        if (!g_gwhole) {   // 溢出模式：这段原本就是匿名占位（块还未定），失败时那段可能已被拆掉，再补一次占位
            LOG("【红】客体内存文件后备：物理映射区换匿名占位失败（errno=%d），这段照旧按块定后备", errno);
            if (reserve_hole(lo, hi)) die("物理映射区补回匿名占位也失败 errno=%d", errno);
            return;
        }
        LOG("【红】客体内存文件后备：物理映射区换匿名占位失败（errno=%d），这段照旧进文件", errno);
        if (mmap((void *)(uintptr_t)lo, hi - lo, PROT_NONE, MAP_FIXED | MAP_SHARED, g_gfd, (off_t)(lo - g_base)) != (void *)(uintptr_t)lo) die("物理映射区换回文件映射也失败 errno=%d", errno);
        return;
    }
    g_anon_lo = lo; g_anon_hi = hi;
    // 整块落在挖区里的块标成「挖回的匿名」：不再换文件；第一次放开时计进匿名累计（按地址跨度计会把预算一次吃光——别名区跨度 8 / 64 GB，不是内存）。
    //   挖区两端不满一块的块照旧按块定（换文件时跳过挖区那部分，gchunk_to_file）
    if (g_gchunk) { pthread_mutex_lock(&g_gchunk_mu);
        for (uint64_t i = (lo - g_base + GCHUNK - 1) / GCHUNK; i < g_gchunk_n && g_base + (i + 1) * GCHUNK <= hi; i++) if (g_gchunk[i] != GC_ANON) gc_set(i, GC_CARVE);
        pthread_mutex_unlock(&g_gchunk_mu); }
    LOG("客体内存文件后备：物理映射区 %#llx +%llu MB 保持匿名内存（%s；游戏反复映射 / 释放的就是这段，进文件的话作废页也要写回闪存；游戏堆照旧走文件）", (unsigned long long)lo, (unsigned long long)((hi - lo) >> 20),
        e && *e ? "S2A_GUEST_FILE_CARVE=1 强制" : g_low_tier ? "物理内存也装不下那档" : "离单进程上限剩余够");
    if (!(e && *e) && !g_low_tier) LOG("客体内存文件后备：挖区判据 离单进程上限剩余 %llu MB ≥ %llu MB", (unsigned long long)(g_gfile_room >> 20), (unsigned long long)(room_min >> 20));
}
// 一块的窗口范围 [*lo, *hi)
//   窗口收尾（s2a_window_trim）只缩 g_size、块表不缩：表尾那些块落在窗口外，范围取空（不夹的话 hi < lo，心跳合计减成 2^64 附近，11-16 真机读数）
static void gchunk_span(uint64_t i, uint64_t *lo, uint64_t *hi) { uint64_t end = g_base + g_size, l = g_base + i * GCHUNK; *lo = l < end ? l : end; *hi = *lo + GCHUNK < end ? *lo + GCHUNK : end; }
// 换成文件（持 g_gchunk_mu）：[lo, hi) 里仍是匿名占位（不可访问、没有内容 = 没有后备对象、最大权限含读写）的段按同偏移盖成文件的不可访问映射；
//   别的段不动（装载器映射的模块 / 代码页、vm_remap 的别名、已有内容的页）——同旧版「整段先进文件、之后它们盖在上面」的结果；挖区那部分跳过。返回没换成的段数
static int map_file_none(uint64_t lo, uint64_t hi) {
    if (lo >= hi) return 0;
    if (mmap((void *)(uintptr_t)lo, hi - lo, PROT_NONE, MAP_FIXED | MAP_SHARED, g_gfd, (off_t)(lo - g_base)) == (void *)(uintptr_t)lo) return 0;
    if (reserve_hole(lo, hi)) die("客体内存文件后备：换文件失败后补回匿名占位也失败 errno=%d", errno);   // 失败时那段可能已被拆掉
    return 1;
}
static int gchunk_to_file(uint64_t i) {
    uint64_t cl, ch; gchunk_span(i, &cl, &ch); int bad = 0;
    for (vm_address_t a = (vm_address_t)cl; a < ch; ) {
        vm_address_t r = a; vm_size_t sz = 0; natural_t depth = 0; vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &r, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS || r >= ch) break;
        uint64_t lo = r > a ? r : a, hi = r + sz < ch ? r + sz : ch;
        a = r + sz;
        if (info.protection != VM_PROT_NONE || info.share_mode != SM_EMPTY || (info.max_protection & (VM_PROT_READ | VM_PROT_WRITE)) != (VM_PROT_READ | VM_PROT_WRITE)) continue;
        if (g_anon_hi > g_anon_lo && lo < g_anon_hi && hi > g_anon_lo) {   // 去掉挖区
            bad += map_file_none(lo, g_anon_lo > lo ? g_anon_lo : lo);
            bad += map_file_none(g_anon_hi < hi ? g_anon_hi : hi, hi);
        } else bad += map_file_none(lo, hi);
    }
    return bad;
}
// 窗口里 [a, a + n) 第一次放开成可读写之前调（svc_mem.c 各放开点）：覆盖到的每个「未定」块按预算定后备，定了不再变（见 GCHUNK 上的注）。
//   没开文件后备 / 整段进文件（预算 0）/ 不在窗口里：什么都不做。窗口外（远端影子，far.c）不走这里
void s2a_guest_commit(uint64_t a, uint64_t n) {
    if (g_gfd < 0 || !g_gchunk || !n) return;
    uint64_t end = g_base + g_size, lo = a > g_base ? a : g_base, hi = a + n < end ? a + n : end;
    if (lo >= hi) return;
    uint64_t i0 = (lo - g_base) / GCHUNK, i1 = (hi - 1 - g_base) / GCHUNK, i;
    for (i = i0; i <= i1; i++) if (!__atomic_load_n(&g_gtouch[i], __ATOMIC_RELAXED)) __atomic_store_n(&g_gtouch[i], 1, __ATOMIC_RELAXED);   // 放开过：心跳记需求要扫这块（整段模式也记）
    if (g_gwhole) return;
    for (i = i0; i <= i1; i++) { uint8_t st = gc_get(i); if (st == GC_UNDEC || st == GC_CARVE) break; }
    if (i > i1) return;   // 全定过（常态）：不加锁
    pthread_mutex_lock(&g_gchunk_mu);
    for (i = i0; i <= i1; i++) {
        uint8_t st = g_gchunk[i]; if (st == GC_ANON || st == GC_FILE) continue;
        uint64_t cl, ch; gchunk_span(i, &cl, &ch);
        if (st == GC_CARVE || g_anon_used < g_anon_budget) { g_anon_used += ch - cl; gc_set(i, GC_ANON); continue; }
        int bad = gchunk_to_file(i);
        if (bad) { static int said; if (said++ < 4) LOG("【红】客体内存文件后备：块 %#llx 有 %d 段换不成文件，那几段照旧匿名（errno=%d）", (unsigned long long)cl, bad, errno); }
        gc_set(i, GC_FILE);
    }
    pthread_mutex_unlock(&g_gchunk_mu);
}
// 文件块里的一段还页（持 g_gchunk_mu）：打洞 + 原偏移重映射成不可访问；打洞失败换匿名占位（再读同样是 0；块仍记文件，下次还页重新盖回文件）。失败回 -1
static int zero_file_piece(uint64_t lo, uint64_t hi) {
    if (lo >= hi) return 0;
    fpunchhole_t ph; memset(&ph, 0, sizeof ph); ph.fp_offset = (off_t)(lo - g_base); ph.fp_length = (off_t)(hi - lo);
    if (fcntl(g_gfd, F_PUNCHHOLE, &ph)) { static int once; if (!once++) LOG("【红】客体内存文件后备：打洞失败（errno=%d，%#llx +%#llx），这段改回匿名占位", errno, (unsigned long long)lo, (unsigned long long)(hi - lo));
        return reserve_hole(lo, hi) ? -1 : 0; }
    if (mmap((void *)(uintptr_t)lo, hi - lo, PROT_NONE, MAP_FIXED | MAP_SHARED, g_gfd, (off_t)(lo - g_base)) == (void *)(uintptr_t)lo) return 0;
    return reserve_hole(lo, hi) ? -1 : 0;
}
// [a, a + n) 在文件后备的窗口里、且至少有一段落在文件块（挖区外）：逐块处理——文件块的段打洞并按原偏移重新映射成不可访问（还文件块，再读是 0），
//   匿名 / 未定 / 挖区的段换匿名占位（同调用方的做法），返回 1；整段都不在文件块 / 不在窗口 / 没开文件后备返回 0（调用方照旧换匿名占位）。
//   文件块的段不交给调用方用匿名盖：那样块会悄悄变成匿名而状态表仍记文件
int s2a_guest_zero(uint64_t a, uint64_t n) {
    if (g_gfd < 0 || !g_gchunk || a < g_base || a + n > g_base + g_size || !n) return 0;
    uint64_t i0 = (a - g_base) / GCHUNK, i1 = (a + n - 1 - g_base) / GCHUNK, i; int any = 0;
    for (i = i0; i <= i1 && !any; i++) {
        if (gc_get(i) != GC_FILE) continue;
        uint64_t cl, ch; gchunk_span(i, &cl, &ch); uint64_t lo = cl > a ? cl : a, hi = ch < a + n ? ch : a + n;
        any = !(g_anon_hi > g_anon_lo && lo >= g_anon_lo && hi <= g_anon_hi);   // 整段在挖区里的不算
    }
    if (!any) return 0;
    pthread_mutex_lock(&g_gchunk_mu); int err = 0;
    for (i = i0; i <= i1 && !err; i++) {
        uint64_t cl, ch; gchunk_span(i, &cl, &ch); uint64_t lo = cl > a ? cl : a, hi = ch < a + n ? ch : a + n;
        if (g_gchunk[i] != GC_FILE) { err = reserve_hole(lo, hi); continue; }
        if (g_anon_hi > g_anon_lo && lo < g_anon_hi && hi > g_anon_lo) {   // 挖区那部分换匿名占位，两侧走文件
            uint64_t ml = g_anon_lo > lo ? g_anon_lo : lo, mh = g_anon_hi < hi ? g_anon_hi : hi;
            err = zero_file_piece(lo, ml) || reserve_hole(ml, mh) || zero_file_piece(mh, hi);
        } else err = zero_file_piece(lo, hi) != 0;
    }
    pthread_mutex_unlock(&g_gchunk_mu);
    return !err;
}
// [a, a + n) 全在文件块里（且不碰挖区）返回 1（没开 / 不在 / n 为 0 / 有一块是匿名或未定返回 0）。svc_mem.c 的懒清零（#258）按它判走不走
int s2a_guest_in_file(uint64_t a, uint64_t n) {
    if (g_gfd < 0 || !g_gchunk || !n || a < g_base || a + n > g_base + g_size || in_anon(a, n)) return 0;
    for (uint64_t i = (a - g_base) / GCHUNK, i1 = (a + n - 1 - g_base) / GCHUNK; i <= i1; i++) if (gc_get(i) != GC_FILE) return 0;
    return 1;
}
// 心跳调（ios/app.m、macos/heartbeat_mac.c，同 [写盘] 每 10 秒）：开着文件后备时报各状态的块合计
// 各状态块的字节合计（下标 GC_*；单测 ⑬ 核窗口收尾后合计 = 窗口大小）
void s2a_guest_backing_sum(uint64_t sum[4]) {
    sum[0] = sum[1] = sum[2] = sum[3] = 0;
    for (uint64_t i = 0; i < g_gchunk_n; i++) { uint64_t cl, ch; gchunk_span(i, &cl, &ch); sum[gc_get(i) & 3] += ch - cl; }
}
// 文件块里在内存或已落在后备文件里的客体字节（文件后备局的心跳记需求用，s2a_guest_mem_note）：逐个文件块，mincore 取在内存的页，
//   后备文件里已落块的段（苹果 F_LOG2PHYS_EXT，别处 SEEK_DATA / SEEK_HOLE；还页会打洞，落块 = 游戏此刻还占着的客体页），两者取并集按页计。
//   匿名块、挖区、未定块不算（匿名的已在进程实占里）。文件页不计进程实占：Mac 实测写脏 512 MB 共享文件页实占不变，写回后文件落块 512 MB
static uint64_t g_live_scanned;   // 单测：s2a_guest_file_live 累计扫过的块数
uint64_t s2a_guest_file_live_scanned(void) { return __atomic_load_n(&g_live_scanned, __ATOMIC_RELAXED); }
uint64_t s2a_guest_file_live(void) {
    if (g_gfd < 0 || !g_base || !g_size) return 0;
    size_t pg = (size_t)getpagesize(); uint64_t got = 0;
    static unsigned char vec[GCHUNK / 4096];   // 一块的页数上限（页 ≥ 4 KB）；心跳在后台串行队列里调，不并发
    for (uint64_t i = 0, nch = g_gchunk ? g_gchunk_n : (g_size + GCHUNK - 1) / GCHUNK; i < nch; i++) {
        if (g_gchunk && !g_gwhole && gc_get(i) != GC_FILE) continue;
        if (g_gtouch && !__atomic_load_n(&g_gtouch[i], __ATOMIC_RELAXED)) continue;   // 游戏没放开过的块：不可访问、没内容，不扫（整段 34 GB 窗口游戏常只用几 GB）
        __atomic_fetch_add(&g_live_scanned, 1, __ATOMIC_RELAXED);
        uint64_t cl, ch; gchunk_span(i, &cl, &ch);
        size_t np = (size_t)((ch - cl + pg - 1) / pg);
        if (mincore((void *)(uintptr_t)cl, (size_t)(ch - cl), (char *)vec)) memset(vec, 0, np);
        for (size_t k = 0; k < np; k++) vec[k] &= MINCORE_INCORE;
        off_t lo = (off_t)(cl - g_base), hi = (off_t)(ch - g_base), d = lo;
        while (d < hi) {   // 后备文件里已落块的段 → 记成「在」
#ifdef F_LOG2PHYS_EXT
            // APFS 上 SEEK_DATA 不分空洞（写过一次就整个文件报成数据，11-21 本机实测），逐段问逻辑 → 物理：设备偏移 -1 = 空洞（没落块 / 已打洞），段长精确
            struct log2phys l; memset(&l, 0, sizeof l); l.l2p_contigbytes = hi - d; l.l2p_devoffset = d;
            if (fcntl(g_gfd, F_LOG2PHYS_EXT, &l) || l.l2p_contigbytes <= 0) break;
            off_t h = d + l.l2p_contigbytes; if (h > hi) h = hi;
            if (l.l2p_devoffset < 0) { d = h; continue; }
#else
            d = lseek(g_gfd, d, SEEK_DATA); if (d < 0 || d >= hi) break;
            off_t h = lseek(g_gfd, d, SEEK_HOLE); if (h < 0 || h > hi) h = hi;
#endif
            for (size_t k = (size_t)((d - lo) / (off_t)pg), e = (size_t)((h - lo + (off_t)pg - 1) / (off_t)pg); k < e && k < np; k++) vec[k] = 1;
            d = h;
        }
        for (size_t k = 0; k < np; k++) {
            uint64_t a = cl + k * pg;
            if (vec[k] && !(g_anon_hi > g_anon_lo && a >= g_anon_lo && a < g_anon_hi)) got += pg;   // 挖区是匿名，已在实占里
        }
    }
    return got;
}
void s2a_guest_backing_log(void) {
    if (g_gfd < 0 || !g_gchunk) return;
#ifdef __IPHONE_OS_VERSION_MIN_REQUIRED
    s2a_guest_budget_note(os_proc_available_memory());   // 本局离上限剩余最小值 → 本款预算记录（下一局按它定预算）
#endif
    uint64_t sum[4]; s2a_guest_backing_sum(sum);
    LOG("[客体后备] %s：匿名块 %llu MB（已计 %llu MB / 预算 %llu MB）、文件块 %llu MB、挖区未放开 %llu MB、未定 %llu MB；本局离上限剩余最小 %lld MB（-1 = 没读过）", g_gwhole ? "整段进文件" : "溢出模式",
        (unsigned long long)(sum[GC_ANON] >> 20), (unsigned long long)(g_anon_used >> 20), (unsigned long long)(g_anon_budget >> 20),
        (unsigned long long)(sum[GC_FILE] >> 20), (unsigned long long)(sum[GC_CARVE] >> 20), (unsigned long long)(sum[GC_UNDEC] >> 20), g_hmin == UINT64_MAX ? -1ll : (long long)(g_hmin >> 20));
}
int s2a_window_ok(void) { return g_ok; }
void s2a_window_get(uint64_t *base, uint64_t *size) { *base = g_base; *size = g_size; }
int s2a_window_session_reset(void) {
    if (!g_ok || !g_size) return -1;
    // 只换客体区；尾部分配器中仍可能有宿主/运行时拥有的映射，不能清空。
    // 一律逐段盖，代码页先绕开（诊断#9f16d7 / #c85aa3，#1026）：原先先拿一次 MAP_FIXED 整段覆盖（文件后备是整段打洞重映射），覆盖范围含上一局装进窗口的签名代码页。
    //   iOS 27 上整段覆盖报 EACCES、代码页原样（11-02 读数 r-x / 最大 r-x）；iOS 26.3（D-RXF2）/ iPadOS 18.3.2（D-4N4F）上的读数是这些段成了「权限 0 / 最大 0」（推断是整段覆盖失败时被系统剥掉了权限）
    //   （与 11-03 munmap 外置代码页后留下的占位同形，见 loader.c span_has_signed_code 上的注），逐段那一轮认不出是代码、盖也盖不掉（errno=12），进程内重启失败，
    //   之后 Metal 回调线程 SIGBUS 闪退。先看段、再只盖非代码段，代码页从头到尾没被覆盖过，留给新局复用
    //   代码页判据：当前权限含执行，或最大权限有执行没写（imgpack.h s2a_prot_is_code）；那是装载器从 App 签名 __TEXT / 外置镜像包映射进窗口的客体代码，
    //   重启后同一款同一落点内容不变，装载器认出来直接复用（loader.c code_same）。段与段之间没映射的空洞也补回不可访问的占位（原先整段覆盖顺带补上）
    uint64_t end = g_base + g_size; vm_address_t a = g_base; int bad = 0, kept = 0; uint64_t kept_b = 0;
    // 文件后备溢出模式：整段文件先打洞（还盘），窗口逐段换回匿名占位（代码页照旧留着），块状态全清回「未定」、匿名累计清零——新局重新按预算定。
    //   整段进文件（预算 0，旧行为）照旧逐段打洞 + 重映射回文件（s2a_guest_zero）
    int ovf = g_gfd >= 0 && !g_gwhole && g_gchunk;
    if (ovf) { fpunchhole_t ph; memset(&ph, 0, sizeof ph); ph.fp_offset = 0; ph.fp_length = (off_t)g_size;
        if (fcntl(g_gfd, F_PUNCHHOLE, &ph)) LOG("【红】重建客体窗口：后备文件整段打洞失败（errno=%d），旧内容留在文件里（窗口照样换回匿名占位，读不到它）", errno); }
    while (a < end) {
        vm_address_t r = a; vm_size_t sz = 0; natural_t depth = 0; vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        int got = vm_region_recurse_64(mach_task_self(), &r, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) == KERN_SUCCESS;
        if (!got || r > end) r = end;
        if (r > a && reserve_hole(a, r) && bad++ < 32) LOG("【红】重建客体窗口：空洞 %#llx +%#llx 补不上占位（errno=%d）", (unsigned long long)a, (unsigned long long)(r - a), errno);
        if (r >= end) break;
        if (r < a) { sz -= a - r; r = a; }   // 段从窗口起点前开始：只管窗口里的那部分
        uint64_t n = r + sz > end ? end - r : sz;
        if (s2a_prot_is_code(info.protection, info.max_protection)) { kept++; kept_b += n; a = r + sz; continue; }   // 代码页判据见 imgpack.h（外置镜像包的文件页最大权限原是 rwx，映射时压回 r-x）
        void *q = !ovf && s2a_guest_zero(r, n) ? (void *)r : mmap((void *)r, n, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
        if (q != (void *)r && bad++ < 32)
            LOG("【红】重建客体窗口：盖不掉 %#llx +%#llx（errno=%d）：权限 %d / 最大 %d、标签 %u、共享方式 %u、子映射深 %u", (unsigned long long)r, (unsigned long long)n, errno,
                info.protection, info.max_protection, info.user_tag, info.share_mode, depth);
        a = r + sz;
    }
    if (ovf) { pthread_mutex_lock(&g_gchunk_mu); memset(g_gchunk, GC_UNDEC, g_gchunk_n); g_anon_used = 0; g_anon_lo = g_anon_hi = 0; pthread_mutex_unlock(&g_gchunk_mu);
        LOG("重建客体窗口：文件后备溢出模式块状态清回未定、匿名累计清零（预算 %llu MB）", (unsigned long long)(g_anon_budget >> 20)); }
    LOG("%s重建客体窗口：逐段盖完；签名代码页 %d 段 %llu KB 原样留着给新局复用，盖不掉 %d 段", bad ? "【红】" : "", kept, (unsigned long long)(kept_b >> 10), bad);
    return bad ? -1 : 0;
}
// 窗口之外还能给宿主大映射（Metal 大缓冲等）的地址：收尾还回的那段 + 窗口所在空洞其余部分（到 2^39 的空洞）。低区账按它判设不设限（lowva.c）
static void holes_in(uint64_t lo, uint64_t hi, uint64_t *total, uint64_t *largest);
uint64_t s2a_window_spare(void) { uint64_t t = 0, l = 0; if (!g_ok) return 0; holes_in(g_base + g_res, 1ull << 39, &t, &l); return (g_res - g_size) + t; }

// [lo, hi) 里的空洞用不可访问的保留占住（只占空洞，不盖已有映射：不带 MAP_FIXED 按提示放，落点不对就退还记 *bad）；累计段数 / 字节。
//   不用 mach_vm.h：iOS SDK 里它不可用（进 App 的代码按 iOS SDK 编），真机上也要占（S2A_VA_IOS26 在 iOS 27 测试设备上复现 iOS 26 的宿主地址预算）
static void fence_holes(uint64_t lo, uint64_t hi, uint64_t *tot, int *n, int *bad) {
    vm_address_t a = lo;
    while (a < hi) {
        vm_address_t r = a; vm_size_t sz = 0; natural_t depth = 0; vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        int ok = vm_region_recurse_64(mach_task_self(), &r, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) == KERN_SUCCESS;
        uint64_t he = ok && r < hi ? r : hi;   // 空洞 [a, he)
        if (he > a) {
            void *f = mmap((void *)a, he - a, PROT_NONE, MAP_PRIVATE | MAP_ANON, VM_MAKE_TAG(VM_MEMORY_GUARD), 0);
            if (f == (void *)a) { *tot += he - a; (*n)++; } else { if (f != MAP_FAILED) munmap(f, he - a); (*bad)++; }
        }
        if (!ok || r >= hi) break;
        a = r + sz;
    }
}
#if TARGET_OS_OSX
#include <mach/mach_vm.h>
// Mac 复现 iPhone 地址约束（缺省开，S2A_IPHONE_VA=0 关；10-20 八方2 / 巫师3 iPhone 地址耗尽，Mac 上从没复现过）：
// iPhone 进程里窗口外能用的只有 4 GB 起到 dyld 共享缓存区（0x1_8000_0000）之间约 2 GB——其上 [0x1_8000_0000, 0xF_C000_0000) 是共享缓存区（标签 32），
// [0xF_C000_0000, 0x70_0000_0000) 是内核保护区（标签 31），窗口尾之上没有地址。Mac 上把这些段里的空洞用不可访问、不可改权限的保留占住（只占空洞，不盖已有映射），
// 让 GC / Metal / 线程栈 / 暂存缓冲在 Mac 上也只能落进同样的低区，地址预算问题在 Mac 局上就现形
static void iphone_va_fence(void) {
    const char *e = getenv("S2A_IPHONE_VA"); if (e && *e == '0') { LOG("iPhone 地址约束：关（S2A_IPHONE_VA=0）"); return; }
    // Mac 的 malloc（xzm）大块从自己的地址提示往上找、不落低区（iPhone 上落低区）：高区只留 MALLOC_ROOM 给它，低区扣减仍由既有诊断变量指定。
    // 远端起点 = 窗口末 ≤ 2^39、跨度 ≤ S2A_FAR_SPAN；房间必须在这个上界之外，否则 far_set 会把它收回成 PROT_NONE。
    enum { MALLOC_ROOM_MB = 512 };
    const uint64_t room = (uint64_t)MALLOC_ROOM_MB << 20;
    const uint64_t room_base = (1ull << 39) + S2A_FAR_SPAN;
    struct { uint64_t lo, hi; } seg[] = {{0x180000000ull, g_keep_lo ? g_keep_lo : g_base}, {1ull << 39, room_base}, {room_base + room, MACH_VM_MAX_ADDRESS}};   // 窗口下方与 2^39 以下其余空洞照旧；高区只开放一个 room 大小的房间
    uint64_t tot = 0; int n = 0, bad = 0;
    for (size_t s = 0; s < sizeof seg / sizeof seg[0]; s++) fence_holes(seg[s].lo, seg[s].hi, &tot, &n, &bad);
    const char *dv = getenv("S2A_IPHONE_VA_DEDUCT_MB"); uint64_t ded = (uint64_t)(dv ? atoi(dv) : 0) << 20;
    void *dd = ded ? mmap(NULL, ded, PROT_NONE, MAP_PRIVATE | MAP_ANON, VM_MAKE_TAG(VM_MEMORY_GUARD), 0) : NULL;   // 低区对等扣减（落低区第一个够大的空洞）
    if (ded && (dd == MAP_FAILED || (uint64_t)dd >= 0x180000000ull)) { LOG("【红】iPhone 地址约束：低区对等扣减 %d MB 没落进低区（%p）", MALLOC_ROOM_MB, dd); bad++; }
    uint64_t ht, hl; s2a_low_holes(&ht, &hl);
    LOG("iPhone 地址约束：开（Mac 复现）——占住共享缓存区 / 保护区 / 窗口尾之上的空洞 %d 段 %llu GB%s；远端上界之外 %#llx 留 %d MB 给 Mac malloc、低区对等扣减 %llu MB；低区剩空洞合计 %llu MB、最大一块 %llu MB",
        n, (unsigned long long)(tot >> 30), bad ? "（有段没占上，见下）" : "", (unsigned long long)room_base, MALLOC_ROOM_MB, (unsigned long long)(ded >> 20), (unsigned long long)(ht >> 20), (unsigned long long)(hl >> 20));
    if (bad) LOG("【红】iPhone 地址约束：%d 段空洞没占上", bad);
}
#endif

static int g_probe_n = -1;   // 最近一次窗口探测数到的别人映射段数（-1 = 没探测过）
// 放行护栏（libs2a_vaguard.dylib，弱链接）：占窗口之前它拒掉了 macOS 27 malloc 落进窗口的大块保留；占过了（成或不成）或不占窗口（S2A_VAWIN_NORESERVE=1）都要放行，
//   否则之后要观察的窗口映射会一直被拒（10-24 #100 作者指出 NORESERVE 提前返回漏了这一步）
static void vaguard_release(void) {
#if TARGET_OS_OSX
    //   按名字取（dlsym）：单测 / 别的构建只链 vawin.c、不链护栏库也能链接
    { void (*vg_open)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "s2a_vaguard_open");
      unsigned (*vg_refused)(unsigned long long *) = (unsigned (*)(unsigned long long *))dlsym(RTLD_DEFAULT, "s2a_vaguard_refused");
      if (vg_open && vg_refused) { unsigned long long rb = 0; unsigned rn = vg_refused(&rb); vg_open();
          if (rn) LOG("地址窗口护栏：占窗口前拒了 %u 次落进窗口的固定地址映射，共 %llu MB（macOS 27 malloc 的大块保留，它会改走不保留的路径）", rn, rb >> 20); }
      else LOG("地址窗口护栏：没载入 libs2a_vaguard.dylib（应在可执行文件同目录）；macOS 27 上窗口可能被 malloc 占走"); }
#endif
}
int s2a_process_prewarmed(void) {
#if TARGET_OS_IPHONE
    const char *e = getenv("ActivePrewarm"); return e && !strcmp(e, "1");
#else
    return 0;
#endif
}
// 窗口 = 本款申请的地址跨度（10-24 维护者：「按照游戏申请的内存地址宽度自动判断……游戏申请多少就打进去多少」）：
//   跨度由包里的 main.npdm 定（地址空间类型 + SystemResourceSize → s2a_layout_need：系统资源为 0 的款 32 GB，非 0 的约 63.25 GB），
//   再加窗口尾部统一分配的 TL_TAKE（能放下才加），在系统给的 2^39 以下空洞里取一块：不带 MAP_FIXED、核对落点，不盖别人的映射；起点 1 GB 对齐。
//   放得下的空洞里取地址最高的（低区留给 GC 段与 Metal 小缓冲；iOS 27 / macOS 上就是 [0x70, 0x80) 那块）。
//   客体地址 = 宿主地址（恒等映射），布局按窗口基址算（layout.c），改写产物不含窗口地址，所以放哪都行；顶端 ≤ 2^39（HIPC 缓冲描述符只编得下 39 位，地址空间.md C7）。
//   以前写死 0x7000000000 + 64 GB：iPadOS 26.6.1（地址上限 0xfc0000000、最大连续块约 51 GB，#61）上一款都起不了，见坑记录
//   S2A_WIN_NEED_MB：跨度直接给（单测 / 不带 NPDM 的工具用）；读不到 NPDM 又没给 → 不占窗口、报红
#define TL_TAKE (2ull << 30)   // 窗口尾部统一分配接管这么多（见 s2a_window_trim）
#define TL_SMALL (256ull << 20)
#define MIN_WIN (2ull << 30)
#define HOST_BELOW (3ull << 30)   // 放不下标准布局时窗口下方留给宿主的地址（11-06：1 GB → 3 GB。iOS 上宿主可用地址 = 低区约 1.8 GB + 这一段；时之笛 Switch 2 版宿主峰值 1.7–2.1 GB，1 GB 时低区碰底在上屏路径崩。时之笛游戏最高只碰到窗口 +17.6 GB、异度3 碰到 +60.6 GB，3 GB 两者都装得下；≥3712 MB 会让 GC 上限改选分堆档吃掉收益）   // 窗口最少要这么多（模块 + 栈 + 堆前段放得下与否由布局判，layout.c）   // 窗口顶到空洞末尾时只接管这么多（九款尾部峰值 ≤ 202 MB，见 s2a_window_trim）
// 放不下标准布局时窗口下方留给宿主多少（#783，纯函数，t_vawin_learned 测）：没学到跨度（learned = 0）→ HOST_BELOW；学到了 → 窗口只给
//   learned × 1.25 + 2 GiB（1 GiB 向上取整，游戏后面还会碰得更高、留余量），这块空洞里其余都给宿主，至少 HOST_BELOW。
//   iPad Pro M2 iPadOS 27.2.1：64 GB 空洞放不下 76 GB 标准布局，固定留 3 GiB，宿主低区开局 3.4 GB、玩 10–40 分钟 mmap 碰底闪退；那款整局只碰到 16.4 GB
// 按学到的跨度该给多大窗口（纯函数，t_vawin_learned 测）：learned × 1.25 + 2 GiB，1 GiB 向上取整、至少 MIN_WIN；没学到（0）回 0 = 不限
uint64_t s2a_window_for_span(uint64_t learned) {
    const uint64_t G = 1ull << 30;
    if (!learned) return 0;
    uint64_t win = (learned + learned / 4 + 2 * G + G - 1) & ~(G - 1);
    return win < MIN_WIN ? MIN_WIN : win;
}
uint64_t s2a_window_host_below(uint64_t fit, uint64_t learned) {
    const uint64_t G = 1ull << 30;
    if (!learned) return HOST_BELOW;
    uint64_t win = s2a_window_for_span(learned);
    uint64_t host = fit > win ? (fit - win) & ~(G - 1) : 0;
    return host > HOST_BELOW ? host : HOST_BELOW;
}
// 小内存 iPad（4 GB 机，2^39 以下只有 6 GB 空洞）上 HOST_BELOW 固定 3 GiB 把窗口压到 3 GiB；
//   Switch 1 的 SDK 开局按应用池 3285 MB 调 SetHeapSize（不是按游戏文件大小，Ryujinx KSystemControl.GetApplicationPoolSize），
//   紧凑布局的堆起点在窗口 +约 380 MB，3 GiB 窗口放不下 3234 MB 堆 → 0xD001 → nninitStartup Break。
//   池是 NPDM 可认定的真值（Switch 1 3285 MiB / Switch 2 9 GiB），按它给窗口；下方宿主至少 2 GiB
//   （旧版 1 GiB 在时之笛 Switch 2 上低区碰底崩，3 GiB 是为大款留的，小款不该按大款扣）。
//   已学到更大跨度（#783）时不能反向压小：宿主按更大的那个值留。
#define HOST_BELOW_MIN (2ull << 30)
#define HEAP_FRONT (512ull << 20)   // 紧凑布局堆起点上界（模块段 + TLS/IO + 栈；实测 378 MB，留余量）
uint64_t s2a_window_host_below_for_pool(uint64_t fit, uint64_t learned, uint64_t pool) {
    uint64_t base = s2a_window_host_below(fit, learned);
    if (!pool) return base;
    uint64_t G = 1ull << 30;
    uint64_t want = (pool + HEAP_FRONT + G - 1) & ~(G - 1);
    uint64_t host = fit > want ? (fit - want) & ~(G - 1) : 0;
    if (host < HOST_BELOW_MIN) host = HOST_BELOW_MIN;
    return host < base ? host : base;
}
// 起局当场自愈（v0.3.4）：窗口按学到的跨度收窄后布局放不下（svc_mem.c layout_prepare 判 -2 那一刻）——删掉记录，窗口往下放宽回缺省给法（下方只留 HOST_BELOW 给宿主），
//   布局重算、本局照常起。原先只删记录、本局照样失败，玩家第二次打开才好（v0.3.3 已记下坏记录的玩家装新版后头一次仍失败）。
//   放宽的那段 [新起点, 窗口底) 起局后可能已被宿主映射占了一部分（lp_map 带提示往这里放直连库段等）：新起点取「已占映射末端（1 GiB 向上取整）」与「留宿主底 + HOST_BELOW」的大者，
//   不盖别人；放不宽（新起点到不了窗口底以下）回 0，照旧失败（记录已删，下一局按缺省）。纯函数部分 s2a_window_widen_lo，t_vawin_learned 测；端到端 t_vawin_widen 测
uint64_t s2a_window_widen_lo(uint64_t keep_lo, uint64_t base, uint64_t used_end) {
    const uint64_t G = 1ull << 30;
    uint64_t lo = keep_lo + HOST_BELOW, u = (used_end + G - 1) & ~(G - 1);
    if (u > lo) lo = u;
    return lo < base ? lo : 0;
}
int s2a_window_span_widen(void) {
    if (!g_ok || !g_span_learned || !g_keep_lo) return 0;   // 没按学到的跨度收窄：不是这条路径的问题
    s2a_window_span_forget();
    uint64_t learned = g_span_learned; g_span_learned = 0;
    if (g_gextra) { LOG("【红】地址窗口：想当场放宽，但文件后备已往窗口外接长（%llu MB），不放宽", (unsigned long long)(g_gextra >> 20)); return 0; }
    if (g_gfd >= 0 && !g_gwhole && g_gchunk) for (uint64_t i = 0; i < g_gchunk_n; i++) if (gc_get(i) != GC_UNDEC) {   // 块偏移按窗口基址算，放宽要换基址：有定过的块就不放宽
        LOG("【红】地址窗口：想当场放宽，但文件后备已有定过后备的块（%#llx），不放宽", (unsigned long long)(g_base + i * GCHUNK)); return 0; }
    for (vm_address_t a = (vm_address_t)g_base, end = (vm_address_t)(g_base + g_size); a < end; ) {   // 窗口还原封不动（布局前，整段 PROT_NONE）才放宽：要整段重映射
        vm_address_t ra = a; vm_size_t sz = 0; vm_region_basic_info_data_64_t bi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &ra, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &obj) != KERN_SUCCESS || ra >= end) break;
        if (bi.protection != VM_PROT_NONE) { LOG("【红】地址窗口：想当场放宽，但窗口里已有放开过的映射（%#llx +%llu MB），不放宽", (unsigned long long)ra, (unsigned long long)(sz >> 20)); return 0; }
        a = ra + sz;
    }
    uint64_t used = g_keep_lo;
    for (vm_address_t a = (vm_address_t)g_keep_lo;; ) {   // [留宿主底, 窗口底) 里已占映射的最高末端
        vm_size_t sz = 0; vm_region_basic_info_data_64_t bi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &obj) != KERN_SUCCESS || a >= g_base) break;
        if (a + sz > used) used = a + sz;
        a += sz;
    }
    uint64_t lo = s2a_window_widen_lo(g_keep_lo, g_base, used), end = g_base + g_size;
    if (!lo) { LOG("【红】地址窗口：想当场放宽，但窗口下方 %#llx 起已被宿主映射占到 %#llx，放不宽", (unsigned long long)g_keep_lo, (unsigned long long)used); return 0; }
    void *p = mmap((void *)lo, g_base - lo, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);   // 不带 MAP_FIXED：核对落点，不盖别人
    if (p != (void *)lo) { if (p != MAP_FAILED) munmap(p, g_base - lo);
        LOG("【红】地址窗口：想当场放宽，[%#llx, %#llx) 占不到（%s），不放宽", (unsigned long long)lo, (unsigned long long)g_base, p == MAP_FAILED ? "失败" : "落在别处"); return 0; }
    if (g_gfd >= 0 && g_gwhole) {   // 文件后备整段进文件（预算 0）：整段（含放宽的那段）重映射成文件，偏移 0 起（窗口还没有内容）
        void *q = ftruncate(g_gfd, (off_t)(end - lo)) ? MAP_FAILED : mmap((void *)lo, end - lo, PROT_NONE, MAP_FIXED | MAP_SHARED, g_gfd, 0);
        if (q != (void *)lo) { int er = errno; mmap((void *)lo, end - lo, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0); close(g_gfd); g_gfd = -1;
            LOG("【红】客体内存文件后备：窗口放宽后重映射成文件失败（errno=%d），照旧用匿名内存", er); } }
    else if (g_gfd >= 0 && ftruncate(g_gfd, (off_t)(end - lo))) {   // 溢出模式：窗口仍是匿名占位，只把文件接长到新窗口（块状态下面按新基址重建）
        LOG("【红】客体内存文件后备：窗口放宽后文件定长失败（errno=%d），照旧用匿名内存", errno); close(g_gfd); g_gfd = -1; }
    LOG("【黄】地址窗口：按学到的跨度 %llu MB 收窄后放不下本款布局——当场放宽：%#llx +%llu MB → %#llx +%llu MB（下方留宿主 %llu MB），本局照常起",
        (unsigned long long)(learned >> 20), (unsigned long long)g_base, (unsigned long long)(g_size >> 20), (unsigned long long)lo, (unsigned long long)((end - lo) >> 20), (unsigned long long)((lo - g_keep_lo) >> 20));
    g_base = lo; g_size = g_res = end - lo;
    if (g_gfd >= 0 && !gchunk_alloc(g_gwhole ? GC_FILE : GC_UNDEC)) die("客体内存文件后备：放宽后块状态表分配失败");
    if (g_gfd < 0) { free(g_gchunk); g_gchunk = NULL; free(g_gtouch); g_gtouch = NULL; g_gchunk_n = 0; }
    { void (*gc)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "s2a_gc_limit_reapply"); if (gc) gc(); }   // GC 上限是按放宽前的低区选的（gclimit.c），按现在的重选
    return 1;
}
static uint64_t g_need, g_fit;   // 本款要的跨度；2^39 以下最大能放的一块（1 GB 对齐后）——自检提示「本款需要 N GB，本机最多 M GB」用
void s2a_window_need(uint64_t *need, uint64_t *fit) { *need = g_need; *fit = g_fit; }
static uint64_t g_hole_top;   // 2^39 / 2^36 以下那次找空洞的上界（本款地址空间上界与系统地址上限取小；没算到 0）
static int g_hi36;            // 36 位款 2^36 以下放不下、又在 2^36 以上按 2^36 对齐的各块里找过（#1419）：g_fit 是两处合起来最大的一段
uint64_t s2a_window_hole_top(void) { return g_hole_top; }
int s2a_window_hi36_tried(void) { return g_hi36; }
// 窗口没占到时的 FATAL 文字（纯函数，t_vawin_fatal 测）：按本款跨度 need、最大能放的一块 fit、找空洞的上界 hole_top、是否找过 2^36 以上 hi36 说。
//   原先写死「首选位置整块 64 GB 与退路（2^39 以下 ≥ 32 GB）」，与实际判据对不上（#1419：36 位款 01007EF00011E000，macOS 27.3 beta 26C5047e 上 2^36 以下最大能放 0 MB）
void s2a_window_fatal_msg(char *out, size_t cap, uint64_t need, uint64_t fit, uint64_t hole_top, int hi36) {
    if (!need) { snprintf(out, cap, "【红】地址窗口：不知道本款要多大，没占窗口，起不了局（原因见上面「地址窗口」红行）"); return; }
    if (hi36) { snprintf(out, cap, "【红】地址窗口不足：本款是 36 位地址空间（NPDM 地址空间类型 1），2^36 以下与 2^36 以上按 2^36 对齐的一块都没占到；"
        "本款要 %llu MB，这些地方最大能放的一块 %llu MB，起不了局（见上面「地址窗口」红行）", (unsigned long long)(need >> 20), (unsigned long long)(fit >> 20)); return; }
    snprintf(out, cap, "【红】地址窗口不足：本款要 %llu MB，本机 %#llx 以下最大能放的一块 %llu MB，起不了局（见上面「地址窗口」红行）",
        (unsigned long long)(need >> 20), (unsigned long long)hole_top, (unsigned long long)(fit >> 20));
}
static char g_npdm[1100];   // 本款 main.npdm 路径（找不到可执行文件目录 / 模块目录时为空）：跨度算不出时按它说清原因
static int g_ctr, g_ctr_astype = -1;   // 多款容器构造期（launchctx.c ③）：g_ctr = 1；g_ctr_astype = 包内各款一致的地址空间类型（不一致 / 读不到 = -1）
static int astype_of(const char *npdm) {   // main.npdm 的地址空间类型（同 svc_mem.c npdm_read：+0x0C 位 1–3，只读头 16 字节）；读不到 / 不是 META 为 -1
    uint8_t h[0x10]; int fd = npdm && npdm[0] ? open(npdm, O_RDONLY) : -1; ssize_t n = fd >= 0 ? read(fd, h, sizeof h) : -1; if (fd >= 0) close(fd);
    return n == (ssize_t)sizeof h && !memcmp(h, "META", 4) ? (h[0x0C] >> 1) & 7 : -1;
}
// 一个模块目录（单款 = 启动上下文的模块目录 / Mac argv[1]；多款容器 = <App>/Games/<游戏号>）要的跨度：main.npdm + 模块段（svc_mem.c s2a_npdm_window_need），
//   多程序合集（s2a_programs.h）取各序号最大值——窗口每进程只占一次，ExecuteProgram 切到别的序号走进程内重启、不重占。
//   哪一支读不到（0 = 不知道要多大）就整体按 0 报，与单程序读不到同一口径；读不到的那份 main.npdm 记进 g_npdm（红行按它说原因）
static uint64_t need_dir(const char *dir) {
    snprintf(g_npdm, sizeof g_npdm, "%s/main.npdm", dir);
    uint64_t (*f)(const char *) = (uint64_t (*)(const char *))dlsym(RTLD_DEFAULT, "s2a_npdm_window_need");
    if (!f) return 0;
    uint64_t need = f(g_npdm), need0 = need;
    int (*count)(const char *) = (int (*)(const char *))dlsym(RTLD_DEFAULT, "s2a_programs_count");
    int (*pdir)(const char *, int, char *, size_t) = (int (*)(const char *, int, char *, size_t))dlsym(RTLD_DEFAULT, "s2a_programs_dir");
    void (*img)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "s2a_loader_image_program");   // iOS 镜像包里序号 n 的模块带前缀：算跨度时按该序号查
    int np = count && pdir ? count(dir) : 1;
    for (int i = 1; i < np && need; i++) {
        char d[1100], np_path[1200];
        if (pdir(dir, i, d, sizeof d)) { need = 0; break; }
        snprintf(np_path, sizeof np_path, "%s/main.npdm", d);
        if (img) img(i);
        uint64_t ni = f(np_path);
        if (img) img(0);
        LOG("地址窗口：多程序合集序号 %d 要 %llu MB（序号 0 要 %llu MB）", i, (unsigned long long)(ni >> 20), (unsigned long long)(need0 >> 20));
        if (!ni) { snprintf(g_npdm, sizeof g_npdm, "%s", np_path); need = 0; break; }
        if (ni > need) need = ni;
    }
    return need;
}
// 多款容器（launchctx.c ③，启动器线 11-07）：窗口必须在构造期、UIKit 之前占好，那时玩家还没选游戏——对 <App>/Games/ 下每一款按单款同一个 need_dir 算，取最大值。
//   iOS 上各款模块的非 FAR 补丁区长度取该款镜像包 Frameworks/S2AImages-<游戏号>.dylib 的偏移表（loader.c s2a_loader_span_images：只读表、不注册签名、不冻结上下文），
//   Mac 取模块目录的 <名>.patched.bin（同单款）。各款 NPDM 地址空间类型须一致（不同类型的窗口上界不同，装不进同一个窗口，组装侧 pack/ship_ext.py 同一判据先拒）：
//   不一致 / 任何一款读不到或算不出 / Games/ 下没有游戏 → 0（报红、不占窗口，自检页拦）。构造期逐款读：main.npdm 头 16 字节（类型）+ NPDM 头 0x80、ACID +0x300 处 4 字节、
//   ACI0 0x38（svc_mem.c npdm_read 不写本局状态的读法）+ 各模块 <名>.fixups.tsv（只有表头几行，实测约 240 字节）+ 镜像包头与偏移表（32 + load commands + 16 + 88 × 模块数）。
//   应用池（Switch 2 9 GiB）在每款算完后放回读之前的值：别款的池不带进下一款的算法，也不带进玩家选的那款（起局时 layout_prepare 按本款 NPDM 重新设）
// 通用壳（11-16）：程序可以不在 App 包里、只在程序包（<程序包根>/<游戏号>/program/，launchctx.c s2a_program_game_paths）——构造期也把这些款算进来：
//   游戏 = App 包 Games/ 下各款 ∪ 程序包根下带 program/catalogue.json 的各款；每款的随行目录与镜像包按起局同一套取法（有程序包取程序包，否则取 App 包内）。
//   原先只扫 App 包，只在程序包里的那款构造期算不到，窗口按别款占，进局可能装不下
#define CTR_MAX 256
static int ctr_add(char (*ids)[208], int *n, const char *gid) {
    for (int i = 0; i < *n; i++) if (!strcmp(ids[i], gid)) return 0;
    if (*n >= CTR_MAX) return -1;
    snprintf(ids[(*n)++], 208, "%s", gid); return 0;
}
uint64_t s2a_window_container_need(const char *bundle, const char *home) {
    g_ctr_astype = -1; g_ctr_pool = 0;
    int (*ppaths)(const char *, const char *, char *, char *, char *, size_t) = (int (*)(const char *, const char *, char *, char *, char *, size_t))dlsym(RTLD_DEFAULT, "s2a_program_game_paths");
    int (*proot)(char *, size_t, const char *) = (int (*)(char *, size_t, const char *))dlsym(RTLD_DEFAULT, "s2a_program_games_root");
    static char ids[CTR_MAX][208]; int nid = 0; struct dirent *e;
    char gd[1100]; snprintf(gd, sizeof gd, "%s/Games", bundle);
    DIR *d = opendir(gd);
    if (d) {
        while ((e = readdir(d))) { char dir[1400]; struct stat st;
            if (e->d_name[0] == '.' || strlen(e->d_name) > 200) continue;
            snprintf(dir, sizeof dir, "%s/%s", gd, e->d_name);
            if (!stat(dir, &st) && S_ISDIR(st.st_mode) && ctr_add(ids, &nid, e->d_name)) { LOG("【红】地址窗口：多款容器游戏超过 %d 款，不占窗口", CTR_MAX); closedir(d); return 0; } }
        closedir(d);
    } else if (errno != ENOENT) { LOG("【红】地址窗口：多款容器读不了 %s（errno %d），不知道要多大，不占窗口", gd, errno); return 0; }
    char pr[1100];
    if (proot && !proot(pr, sizeof pr, home) && (d = opendir(pr))) {
        while ((e = readdir(d))) { char cat[1400]; struct stat st;
            if (e->d_name[0] == '.' || strlen(e->d_name) > 200) continue;
            snprintf(cat, sizeof cat, "%s/%s/program/catalogue.json", pr, e->d_name);
            if (!stat(cat, &st) && S_ISREG(st.st_mode) && ctr_add(ids, &nid, e->d_name)) { LOG("【红】地址窗口：多款容器游戏超过 %d 款，不占窗口", CTR_MAX); closedir(d); return 0; } }
        closedir(d);
    }
    int (*span)(const char *, char *, size_t) = (int (*)(const char *, char *, size_t))dlsym(RTLD_DEFAULT, "s2a_loader_span_images");
    uint64_t (*poolf)(void) = (uint64_t (*)(void))dlsym(RTLD_DEFAULT, "s2a_app_pool");
    void (*pools)(uint64_t) = (void (*)(uint64_t))dlsym(RTLD_DEFAULT, "s2a_app_pool_set");
    uint64_t pool0 = poolf ? poolf() : 0, top = 0, span_max = 0; int n = 0, ast = -1, ok = 1; char first[256] = "";
    for (int gi = 0; ok && gi < nid; gi++) {
        const char *gid = ids[gi];
        char dir[1400], img[1400], why[400] = "";
        int fp = ppaths ? ppaths(home, gid, dir, img, why, sizeof why) : 0;   // dir / img 各 1400 字节 ≥ S2A_LCTX_PATH
        if (fp < 0) { LOG("【红】地址窗口：多款容器里 %s 的程序包读不了：%s，不占窗口", gid, why); ok = 0; break; }
        if (!fp) { snprintf(dir, sizeof dir, "%s/%s", gd, gid); snprintf(img, sizeof img, "%s/Frameworks/S2AImages-%s.dylib", bundle, gid); }
        // 通用壳：App 包的 Games/<号>/ 只放游戏库界面用的小文件、程序包还没推到这台（或推了一半、清单最后才写）——这款程序不在：跳过，
        //   不拖累别的款（原先整个容器「读不到 main.npdm、不占窗口」，已装好的款也起不来）。玩家点它时起局取路径报「App 不完整」（S2A_SELECT_APP_INCOMPLETE）
        if (!fp) { char np[1500]; struct stat ns; snprintf(np, sizeof np, "%s/main.npdm", dir);
            if (stat(np, &ns)) { LOG("【黄】地址窗口：多款容器里 %s 的程序不在这台（程序包没推到、App 包里也没有 main.npdm），构造期跳过，不影响别的款", gid); continue; } }
        snprintf(g_npdm, sizeof g_npdm, "%s/main.npdm", dir);
        int t = astype_of(g_npdm);
        if (t < 0) { LOG("【红】地址窗口：多款容器里 %s 读不到 main.npdm（或不是 META），不知道要多大，不占窗口", gid); ok = 0; break; }
        if (ast >= 0 && t != ast) {
            LOG("【红】地址窗口：多款容器里各款 NPDM 地址空间类型不一致（%s 类型 %d（%s）、%s 类型 %d（%s）），装不进同一个窗口，不占窗口",
                first, ast, s2a_as_type_bits((uint32_t)ast), gid, t, s2a_as_type_bits((uint32_t)t));
            ast = -1; ok = 0; break; }
        if (ast < 0) { ast = t; snprintf(first, sizeof first, "%s", gid); }
        if (span && span(img, why, sizeof why)) { LOG("【红】地址窗口：多款容器里 %s 的镜像包读不了偏移表：%s，不占窗口", gid, why); ok = 0; break; }
        uint64_t ni = need_dir(dir);
        if (span) span(NULL, NULL, 0);
        { uint64_t pg = poolf ? poolf() : 0; if (pg > g_ctr_pool) g_ctr_pool = pg; }
        if (pools && pool0) pools(pool0);
        if (!ni) { LOG("【红】地址窗口：多款容器里 %s 的跨度算不出（类型 %d（%s）；%s 读不到、模块段读不到或类型不支持），不占窗口", gid, t, s2a_as_type_bits((uint32_t)t), g_npdm); ok = 0; break; }
        LOG("地址窗口：多款容器 %s 要 %llu MB（地址空间类型 %d）", gid, (unsigned long long)(ni >> 20), t);
        // 学到的跨度在这款自己的文档根下（选了游戏后 S2A_DOCS = Documents/games/<游戏号>，心跳 span_note 写在那里）；构造期还没选游戏、S2A_DOCS 没设，
        //   原先按 Documents/wire/... 读，永远读不到：多款容器从来学不到跨度，窗口下方一直只留 3 GB（11-13 0400486024DEC000 三台设备开局约 2–4 分钟宿主地址耗尽崩溃）。
        //   这款文档根下没有时再看旧位置（单款包时代留下的记录）
        { uint64_t pid = strtoull(gid, NULL, 16), sl = 0; char sp[1400];
          if (pid) { s2a_docs_path(sp, sizeof sp, "games/%s/wire/%016llX/span_peak", gid, (unsigned long long)pid); sl = read_u64(sp); }
          if (!sl) sl = s2a_window_span_seen(pid);
          // 这款还没玩过、没有记录：它的跨度不会超过它自己的布局需求 ni，按 ni 算（原先整容器取 0，一款没玩过就把别款也打回 3 GB）。
          //   ni 比空洞还大的（乙布局款）照旧落到 s2a_window_host_below 的 HOST_BELOW
          if (!sl) sl = ni;
          if (sl > span_max) span_max = sl; }
        if (ni > top) top = ni;
        n++;
    }
    if (ok && !n) LOG("【红】地址窗口：多款容器 %s 下没有游戏（程序包里也没有），不占窗口", gd);
    g_ctr_astype = ast;
    if (!ok || !n) return 0;
    g_span_learned = span_max;
    g_npdm[0] = 0;
    LOG("地址窗口：多款容器 %d 款取最大 %llu MB（地址空间类型 %d）", n, (unsigned long long)(top >> 20), ast);
    return top;
}
static uint64_t need_from_npdm(void) {
    const char *e = getenv("S2A_WIN_NEED_MB");
    if (e && atoll(e) > 0) { char sp[1024]; if (getenv("S2A_WIN_SPAN_PEAK") && span_path(sp, sizeof sp, 0)) g_span_learned = read_u64(sp); return (uint64_t)atoll(e) << 20; }   // 单测：跨度与学到的跨度都直接给
    char dir[1024] = "";
#if TARGET_OS_IPHONE
    // iOS：main.npdm 在启动上下文的模块目录（单款兼容 = 可执行文件同目录，同改造前）
    // 多款容器（launchctx.c ③）：构造期玩家还没选游戏、上下文不冻结——对包内每一款算，取最大（s2a_window_container_need）
    if (s2a_launch_ctx_container()) { g_ctr = 1; return s2a_window_container_need(s2a_launch_ctx_bundle_dir(), getenv("HOME")); }
    const s2a_launch_ctx *lc = s2a_launch_ctx_get();
    if (!lc) { LOG("【红】地址窗口：启动上下文不可用（%s）", s2a_launch_ctx_error()); return 0; }
    strlcpy(dir, lc->module_dir, sizeof dir);
#else
    { int ac = *_NSGetArgc(); char **av = *_NSGetArgv(); if (ac < 2) return 0; strlcpy(dir, av[1], sizeof dir); }   // Mac：s2arm-host <模块目录>
#endif
    { uint64_t (*pf)(const char *) = (uint64_t (*)(const char *))dlsym(RTLD_DEFAULT, "s2a_npdm_pid");
      uint64_t (*pl)(const char *) = (uint64_t (*)(const char *))dlsym(RTLD_DEFAULT, "s2a_npdm_app_pool");
      char np[1100]; snprintf(np, sizeof np, "%s/main.npdm", dir);
      g_ctor_pid = pf ? pf(np) : 0;
      g_ctr_pool = pl ? pl(np) : 0; }   // 单款也按本款 NPDM 认应用池（小内存 iPad 按池给窗口）；多款在容器循环里取最大
    g_span_learned = s2a_window_span_seen(g_ctor_pid);
    return need_dir(dir);
}
// 跨度为 0 的红行按事实分（11-05 Minecraft: Story Mode 的 NPDM 是地址空间类型 1，被报成「读不到 main.npdm」）：
//   读不到 / 不是 META → 读不到；读得到、类型不是 3 → 报类型与位宽（布局只支持 39 位，layout.c）；类型 3 仍为 0 → 模块段算不出
static int g_astype = -1;   // 跨度为 0 时读到的 NPDM 地址空间类型（-1 = 没读到）：自检页按它说「暂不支持 36 位地址空间」还是「资源不完整」（selfcheck.c）
int s2a_window_astype(void) { return g_astype; }
static int npdm_astype(void) { return g_ctr ? g_ctr_astype : astype_of(g_npdm); }   // 本款（多款容器 = 包内各款一致的）地址空间类型
static void need_zero_red(void) {
    int t = npdm_astype(); g_astype = t;
    if (g_ctr) return;   // 多款容器：s2a_window_container_need 已按款报过红
    if (t < 0) { LOG("【红】地址窗口：读不到本款 main.npdm（可执行文件同目录 / 模块目录），不知道要多大，不占窗口"); return; }
    if (!s2a_as_type_top((uint32_t)t)) LOG("【红】地址窗口：NPDM 地址空间类型 %d（%s）暂不支持（布局只支持类型 1 = 36 位、3 = 39 位），不占窗口", t, s2a_as_type_bits((uint32_t)t));
    else LOG("【红】地址窗口：main.npdm 是类型 %d（%s），但算不出本款跨度（模块段读不到或太大），不占窗口", t, s2a_as_type_bits((uint32_t)t));
}
// [lo, hi) 里的空洞（lo = 0 从 4 GB 起；hi = 2^39 或地址上限），按 1 GB 对齐收成可用段；挑能放下 want 的地址最高的一段（都放不下 → *sz = 0），*fit / *fit_at 记最大一段（同大取地址高的），
//   *low_at 记放得下 low_want 的地址最低的一段（low_want = 0 不找）
static void pick_hole_in(uint64_t lo, uint64_t hi, uint64_t want, uint64_t *at, uint64_t *sz, uint64_t *fit, uint64_t *fit_at, uint64_t low_want, uint64_t *low_at) {
    const uint64_t G = 1ull << 30; uint64_t prev = lo > 0x100000000ull ? lo : 0x100000000ull; vm_address_t a = prev; *at = *sz = *fit = *fit_at = *low_at = 0;
    for (;;) {
        vm_size_t rs = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
        int ok = vm_region_64(mach_task_self(), &a, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) == KERN_SUCCESS;
        uint64_t ge = ok && a < hi ? a : hi, b = (prev + G - 1) & ~(G - 1), e = ge & ~(G - 1);
        if (ge > prev && e > b) { if (e - b >= *fit) { *fit = e - b; *fit_at = b; } if (e - b >= want && b >= *at) { *at = b; *sz = e - b; } if (low_want && e - b >= low_want && !*low_at) *low_at = b; }
        if (!ok || a >= hi) break;
        if (a + rs > prev) prev = a + rs;
        a += rs;
    }
}
static void pick_hole(uint64_t hi, uint64_t want, uint64_t *at, uint64_t *sz, uint64_t *fit, uint64_t *fit_at, uint64_t low_want, uint64_t *low_at) {
    pick_hole_in(0, hi, want, at, sz, fit, fit_at, low_want, low_at);
}
static void cmd_arena_reserve(void);
static void vawin_reserve(void) {
    const char *e = getenv("S2A_VAWIN_NORESERVE"); g_noreserve = e && *e == '1';
    if (g_noreserve) { s2a_window_probe("构造函数（不占窗口）"); vaguard_release(); return; }
    g_need = need_from_npdm();
    {   // 只读日志（11-03 大乱斗首局定因 / 宿主预留按实测补足的依据）：构造函数时低区 [4 GB, 6 GB) 空余，与主可执行文件映射大小（各段 vmsize，不含 __PAGEZERO）
        uint64_t lt = 0, ll = 0, ex = 0; holes_in(0x100000000ull, 0x180000000ull, &lt, &ll);
        const struct mach_header_64 *mh = (const struct mach_header_64 *)_NSGetMachExecuteHeader();   // 主可执行文件（crt_externs.h；构造函数时刻 dyld 映像表的序号 0 不一定是它）
        const struct load_command *lc = mh ? (const struct load_command *)(mh + 1) : NULL;
        for (uint32_t i = 0; lc && i < mh->ncmds; i++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize))
            if (lc->cmd == LC_SEGMENT_64 && strcmp(((const struct segment_command_64 *)lc)->segname, "__PAGEZERO")) ex += ((const struct segment_command_64 *)lc)->vmsize;
        LOG("地址窗口前：低区 [4 GB, 6 GB) 空余合计 %llu MB、最大一块 %llu MB；主可执行文件映射 %llu MB", (unsigned long long)(lt >> 20), (unsigned long long)(ll >> 20), (unsigned long long)(ex >> 20)); }
    if (!g_need) { vaguard_release(); need_zero_red(); return; }
    // 窗口顶不超过本款地址空间上界（as_type.h s2a_as_type_top）：36 位款（老 SDK 的 HIPC 描述符只编 36 位地址）取 2^36 以下的空洞，39 位款 2^39；
    //   S2A_WIN_NEED_MB 直接给跨度（没有 NPDM）按 39 位
    int ast = npdm_astype(); uint64_t astop = ast >= 0 && s2a_as_type_top((uint32_t)ast) ? s2a_as_type_top((uint32_t)ast) : 1ull << 39;
    const char *hb = astop == 1ull << 36 ? "2^36" : "2^39";
    uint64_t top = s2a_va_top(), hi = astop, at, sz; if (top && top < hi) hi = top;
    g_hole_top = hi;
    // S2A_VA_CAP_MB（Mac 诊断）：复现小地址机型（iOS 26 不开扩展寻址约 17.4 GiB）——取地址最低、放得下这么大的空洞当「本机最大一块」，
    //   窗口照下面「放不下」的取法占，之后到 2^39 的空洞占死：别名区伸出段落进 Mac 的 GPU 保留区（不可访问），同设备上伸出段越过地址上限
    const char *ce = getenv("S2A_VA_CAP_MB"); uint64_t cap = ce && atoll(ce) > 0 ? (uint64_t)atoll(ce) << 20 : 0, fat, lat, want;
    pick_hole(hi, g_need, &at, &sz, &g_fit, &fat, cap, &lat);
    if (cap) { if (lat) { fat = lat; g_fit = cap; } else g_fit = 0; sz = 0; }
    // 36 位款 2^36 以下放不下（#1419：macOS 27 / iOS 27 的 [6, 64) GB 是系统只读区与保护区，只剩 [4, 6) GB 零散 1.8 GB）：改在 2^36 以上找一块按 2^36 对齐的 64 GiB，
    //   窗口整段落在这一块里（as_type.h s2a_as_type_end）。游戏地址仍等于宿主地址；老 SDK 打包 IPC 描述符丢掉的 36 位以上由服务层按块补回（svc_mem.c s2a_ipc_addr39）。
    //   先找放得下整个标准布局的、地址最低的一块；都放不下就记各块里最大的一段，走下面「按需」那条
    if (ast == 1 && !cap && !sz && g_fit < MIN_WIN + HOST_BELOW) {
        g_hi36 = 1;
        const uint64_t B = 1ull << 36; uint64_t lim = top ? top : 1ull << 47, bfit = 0, bfat = 0;
        for (uint64_t k = B; k < lim && !sz; k += B) {
            uint64_t a2, s2, f2, fa2, la2; pick_hole_in(k, k + B < lim ? k + B : lim, g_need, &a2, &s2, &f2, &fa2, 0, &la2);
            if (s2) { at = a2; sz = s2; g_fit = f2; fat = fa2; }
            else if (f2 > bfit) { bfit = f2; bfat = fa2; }
        }
        if (!sz && bfit > g_fit) { g_fit = bfit; fat = bfat; }
        uint64_t blk = (sz ? at : fat) & ~(B - 1);
        if (sz || g_fit >= MIN_WIN + HOST_BELOW) { hb = "所在 2^36 块";
            LOG("地址窗口：36 位款 2^36 以下放不下，改放 2^36 以上按 2^36 对齐的一块 %#llx–%#llx（%s；IPC 描述符丢掉的 36 位以上由服务层按这一块补回）",
                (unsigned long long)blk, (unsigned long long)(blk + B), sz ? "放得下整个标准布局" : "按需：放最大一段"); }
    }
    // S2A_VA_IOS26=1（Mac 诊断 / iOS 27 测试设备复现）：模拟 iOS 26 大地址档——最大空洞 [0x300000000, 0xfc0000000)（共享区末到大地址档上限，52224 MiB = #61 读数），其上越过地址上限。
    //   Mac 上取同位置的 [0x3_0000_0000, 0xF_C000_0000)（护栏库拦住 xzm 的 24 GB 保留后构造函数时刻是空的），之上到 2^39 占死
    //   macOS 27（gate-host / Spare-host）的共享缓存铺满 [6, 57) GB，低区没有这段：改在 0x70 那块 64 GB 空洞开头模拟同样大小（别名区承诺末端会越过 2^39，
    //   游戏碰到的地址在 2^39 以内；QueryMemory 的地址空间上界按别名区末放宽，svc_mem.c free_span）
    { const char *e6 = getenv("S2A_VA_IOS26"); if (e6 && *e6 == '1') {
        const uint64_t lo = 0x300000000ull, n = 0xFC0000000ull - 0x300000000ull;
        void *t = mmap((void *)lo, n, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0); int low = t == (void *)lo; if (t != MAP_FAILED) munmap(t, n);
        fat = low ? lo : 0x7000000000ull; g_fit = cap = n; sz = 0;
        LOG("模拟 iOS 26 大地址档（S2A_VA_IOS26）：空洞放在 %#llx（%s）+%llu MB", (unsigned long long)fat, low ? "同 iOS 26 位置" : "macOS 27 / iOS 27 低区被共享缓存占满，改用 0x70 空洞开头", (unsigned long long)(n >> 20)); } }
    // 放不下标准布局不再起不了局（docs/方案_地址空间根修.md）：占最大一块、下方留 HOST_BELOW（3 GiB）给宿主（Metal / GC），布局小区在前、别名区伸出窗口（layout.c 乙）。
    //   宿主保留段不能放窗口上方：别名区伸出段覆盖那里，宿主放进去的东西会被游戏的远端访问静默读写
    { void (*lim)(uint64_t) = (void (*)(uint64_t))dlsym(RTLD_DEFAULT, "s2a_mem_span_limit"); if (lim && g_fit) lim(g_fit); }   // 学跨度只认空洞以内的地址（svc_mem.c）
    // 即时存档恢复：窗口必须与存档时同址同长（快照里全是游戏地址）。学到的跨度会改「窗口下方留多少」、布局时还会把窗口往下扩（s2a_window_widen_lo），
    //   两局之间学到的不同（一款大游戏 iPhone：存档局是容器里第一局、没学到，窗口 59392 MB；恢复局学到了上一局，窗口 5120 MB → 「窗口不符」拒绝），
    //   所以恢复局不用学到的跨度，并按清单「窗口」原样占（下面占之前覆盖 at / want）
    int s2a_snap_manifest_win(uint64_t *base, uint64_t *size); uint64_t sw_b = 0, sw_s = 0;
    int snap_win = s2a_snap_restoring() && !s2a_snap_manifest_win(&sw_b, &sw_s);
    if (snap_win && g_span_learned) { LOG("即时存档恢复：不用本机学到的跨度 %llu MB，窗口按快照原样占", (unsigned long long)(g_span_learned >> 20)); g_span_learned = 0; }
    if (g_span_learned && !s2a_window_span_rec_ok(g_span_learned, g_fit)) {
        LOG("【黄】地址窗口：本机学到的跨度 %llu MB 不小于这块空洞 %llu MB（旧版把窗口外的远端映射也算进去了），当作没学到、这一局重新学", (unsigned long long)(g_span_learned >> 20), (unsigned long long)(g_fit >> 20));
        g_span_learned = 0; }
    if (!sz && g_fit >= MIN_WIN + HOST_BELOW) {
        uint64_t pool = s2a_window_container_pool();   // 多款容器在构造期逐款读过 NPDM；单款 0 = 不知道，照旧 3 GiB
        uint64_t below = s2a_window_host_below_for_pool(g_fit, g_span_learned, pool);
        if (pool && below != HOST_BELOW) LOG("地址窗口：按本款应用池 %llu MB 定宿主保留 %llu MB（固定 %llu MB 时窗口放不下 SDK 开局要的堆）",
            (unsigned long long)(pool >> 20), (unsigned long long)(below >> 20), (unsigned long long)(HOST_BELOW >> 20));
#if !TARGET_OS_OSX
        // 即时存档：命令记录区 1 GB + 虚拟池区 2 GB。窗口本来就放不下标准布局，不再从窗口里让（11-20 维护者定：原先让出 3 GB，一款大游戏的 iPhone 窗口
        //   59392 MB 比游戏真用到的少 0.9 GB，落远端每秒几十万次代执行、停在第 0 帧，比没有快照更糟）。先放窗口所在空洞之外（[4 GB, 空洞起点) 里整块 ≥3 GB 的空洞顶端，
        //   低区其余照旧留给宿主）；放不下就这一局关掉快照、记原因（菜单里写给玩家），不拦起局。恢复局不在这里定：按快照清单原址占（cmd_arena_at_snapshot）
        { const char *rec = getenv("S2A_FRAME_REC"); uint64_t A = (1ull << 30) + (2ull << 30);
          if (!(rec && *rec) && s2a_snap_mode() && !s2a_snap_restoring()) {
              uint64_t oa = 0, os = 0, of = 0, ofa = 0, ol = 0;
              pick_hole_in(0x100000000ull, fat, A, &oa, &os, &of, &ofa, 0, &ol);
              if (os >= A) { g_snap_out = oa + os - A;
                  LOG("即时存档：窗口放不下标准布局，命令记录区与虚拟池区不从窗口里让，放窗口外 %#llx +%llu MB", (unsigned long long)g_snap_out, (unsigned long long)(A >> 20)); }
              else { s2a_snap_disable(S2A_SNAP_OFF_ADDRESS);
                  LOG("【黄】即时存档：这台设备的地址空间放不下这款游戏的标准布局，窗口外也没有整块 %llu MB 的空洞（最大 %llu MB）——这一局不开快照，游戏照常玩（不再从窗口里让 3 GB）",
                      (unsigned long long)(A >> 20), (unsigned long long)(of >> 20)); } } }
#endif
        at = fat + below; want = g_fit - below; g_keep_lo = fat;
        LOG("地址窗口按需：本款标准布局要 %llu MB，本机 %s 以下最大一块 %llu MB（%#llx 起，地址上限 %#llx）——照样起局，下方 %llu MB 留宿主、其余作窗口（本机学到的跨度 %llu MB，0 = 没学到）",
            (unsigned long long)(g_need >> 20), hb, (unsigned long long)(g_fit >> 20), (unsigned long long)fat, (unsigned long long)top, (unsigned long long)(below >> 20),
            (unsigned long long)(g_span_learned >> 20)); }
    else if (!sz) { vaguard_release();
        LOG("【红】地址窗口不足：本机 %s 以下最大能放的一块 %llu MB，连模块与小区都放不下（本款标准布局要 %llu MB，地址上限 %#llx），起不了局", hb, (unsigned long long)(g_fit >> 20), (unsigned long long)(g_need >> 20), (unsigned long long)top);
        s2a_window_probe("窗口放不下"); return; }
    else want = g_need + TL_TAKE <= sz ? g_need + TL_TAKE : sz;   // 尾部统一分配最多 2 GB，空洞里剩多少给多少（63.25 GB 的款在 64 GB 空洞里只剩 766 MB）
    if (snap_win && (sw_b != at || sw_s != want)) {
        LOG("即时存档恢复：窗口按快照占 %#llx +%llu MB（本局自己会选 %#llx +%llu MB）", (unsigned long long)sw_b, (unsigned long long)(sw_s >> 20), (unsigned long long)at, (unsigned long long)(want >> 20));
        at = sw_b; want = sw_s; }
    void *p = mmap((void *)at, want, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);   // 不带 MAP_FIXED：核对落点，不盖别人
    // errno 紧接 mmap 取：后面的 dlsym / 打日志会改它（10-24 #100 作者指出）；映射成功但落在别处时 errno 是旧值，不报
    int er = p == MAP_FAILED ? errno : 0;
    vaguard_release();   // 占过了（成或不成）才放行护栏（Mac：占之前它拒掉 macOS 27 malloc 落进窗口的大块保留）
    if (p != (void *)at) { if (p != MAP_FAILED) munmap(p, want);
        LOG("【红】地址窗口：空洞 %#llx +%llu MB 占不到（%s errno=%d）", (unsigned long long)at, (unsigned long long)(want >> 20), p == MAP_FAILED ? "失败" : "落在别处", er); s2a_window_probe("窗口没占到"); return; }
    g_ok = 1; g_base = at; g_size = g_res = want;
    guest_file_attach();
    LOG("窗口已占 %#llx +%llu MB（本款要 %llu MB + 尾部统一分配 %llu MB；所在空洞 %llu MB、%s 以下最大一块 %llu MB、地址上限 %#llx）", (unsigned long long)at, (unsigned long long)(want >> 20),
        (unsigned long long)(g_need >> 20), (unsigned long long)((want > g_need ? want - g_need : 0) >> 20), (unsigned long long)(sz >> 20), hb, (unsigned long long)(g_fit >> 20), (unsigned long long)top);
    if (cap) { uint64_t ft = 0; int fn = 0, fb = 0; fence_holes(at + want, 1ull << 39, &ft, &fn, &fb);   // 宿主保留段（窗口下方）由 iphone_va_fence 按低区规则处理
        LOG("地址窗口压到 %llu MB（S2A_VA_CAP_MB）：窗口之后到 2^39 的空洞 %d 段 %llu GB 已占死%s", (unsigned long long)(want >> 20), fn, (unsigned long long)(ft >> 30), fb ? "【红】有段没占上" : ""); }
    cmd_arena_reserve();   // 要在 iPhone 地址围栏之前：围栏把 2^39 之上的空洞几乎全占死（11-05 旷野之息录 / 放都报「2^39 之上没有 1024 MB 的空洞」）
#if TARGET_OS_OSX
    iphone_va_fence();
#endif
    BOOT_MARK("进程创建 → 占窗口（dyld 装载 + 构造函数）");
}

void s2a_window_reserve(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, vawin_reserve);
}
// 多款容器构造期一款可算的游戏都没有（全新安装后才用 .s2arm 导入、或删光了再导入）：构造期跨度 0、不占窗口，玩家「导入 → 点开始」同一进程起局，
//   自检报「读不到本款的 main.npdm」，要重开 App 才能玩（11-18 iPhone 实机回归）。起宿主之前补一次：这时 s2a_window_container_need 扫得到刚导入的款，
//   按同一套取法占。构造期算出过跨度的（不论占没占成）不重来，已有游戏的进程照旧只在构造期占
// 构造期占过、但只够当时那几款的（窗口按它们的最大跨度占）：启动后又导入了一款要得更多的，起宿主前放掉旧窗口、按现在的最大值重占。
//   这时窗口里还什么都没放（布局、模块、尾部分配都在自检之后），放掉不碰任何东西。按需档（窗口已是本机最大一块、比要的小）重占也大不了，不动
int s2a_window_reserve_late(void) {
    if (g_noreserve) return g_ok;
    if (!g_ok && !g_need) {
        LOG("地址窗口：构造期容器里没有可算的游戏（启动后才导入），起宿主前按现在的游戏补占");
        vawin_reserve();
        return g_ok;
    }
    if (g_ok && g_ctr && g_size >= g_need) {
        uint64_t was = g_need, now = need_from_npdm();
        if (now > was) {
            LOG("地址窗口：构造期按 %llu MB 占的窗口 %#llx +%llu MB，启动后导入的款要 %llu MB，起宿主前放掉重占",
                (unsigned long long)(was >> 20), (unsigned long long)g_base, (unsigned long long)(g_res >> 20), (unsigned long long)(now >> 20));
            munmap((void *)g_base, g_res); g_ok = 0; g_base = g_size = g_res = 0;
            vawin_reserve();
        }
    }
    return g_ok;
}
// ---------- 命令记录区（帧录放专用，Mac）----------
// 帧录（S2A_FRAME_REC）把 NVN 命令记录宿主块（graphics/nvn hle_nvn.c 影子块）按原地址存进录像，帧回放（S2A_FRAME_REPLAY）是另一个进程、要在原地址放回。
//   块原先 mmap(NULL) 落在内核挑的低区，回放进程同址常被它自己的库 / 堆 / Metal 占着（7df29fdfc 后回放全红）；放窗口尾部也不够：窗口放不下标准布局的款没有尾部（11-05 哆啦A梦 1350 高，尾部 0 MB）。
//   所以录 / 放两种进程（S2A_CMD_ARENA=1 也开，给单测用）在构造期、窗口之后再占一块 CMD_ARENA：2^39 之上（客体地址够不着）、地址上限以下最高的空洞，同一台机器同一个宿主两次起进程落在同一处。
//   录的进程按顺序切块（s2a_cmd_arena_map）；放的进程只占不切（切了会占掉要放回的地址），回放按映射段 MAP_FIXED 放回。其余进程不占，命令记录照旧 mmap(NULL)
#define CMD_ARENA (1ull << 30)
// 紧接着同一块保留里再放 VP_ARENA 给虚拟池（graphics/nvn hle_nvn.c vpool_reserve）：虚拟池各有一段自己的地址（贴图 / 缓冲地址 = 池基址 + 偏移，写在游戏内存里），
//   原先 mmap(NULL) 落在内核挑的低区，回放进程同址占不回（11-05 旷野之息唯一的虚拟池 0x131310000+0x7300000，回放读到别的映射或 SIGBUS 在 pkg_upload）。只给地址、不建页
#define VP_ARENA (64ull << 30)
static uint64_t g_ca_base, g_ca_used, g_va_used, g_va_cap = VP_ARENA; static int g_ca_cut;
static pthread_mutex_t g_ca_mu = PTHREAD_MUTEX_INITIALIZER;
// 即时存档开关与快照清单读取的弱缺省（快照关、没有清单）：宿主与 iOS 壳链着 snapsw.c / snapshot.c，用它们的正式定义；
//   只编 vawin.c 的地址窗口单测（runtime/tests/mk/vawin*.mk、guest_file*.mk、ios_prewarm.mk 共 18 份）不链快照模块，取这里的缺省（同 egl.m s2a_gl_first_use 的写法）
__attribute__((weak)) int s2a_snap_mode(void) { return 0; }
__attribute__((weak)) void s2a_snap_disable(int why) { (void)why; }   // snapsw.c：这一局关掉即时存档（原因码给菜单写提示）
__attribute__((weak)) int s2a_snap_restoring(void) { return 0; }
__attribute__((weak)) int s2a_snap_manifest_win(uint64_t *base, uint64_t *size) { (void)base; (void)size; return -1; }
__attribute__((weak)) int s2a_snap_manifest_u64(const char *key, uint64_t *a, uint64_t *b) { (void)key; (void)a; (void)b; return -1; }
// 即时存档恢复：命令记录区 / 虚拟池区的地址写在游戏内存里（命令句柄、贴图 / 缓冲地址），必须与存档时同址。按快照清单里记的基址与长度直接占，
//   不靠两次进程「最高空洞」碰巧相同（跨版本时宿主的库与堆布局会变）。占到返回 1；清单里没有 / 占不到返回 0（走原来的选址，图形恢复核基址时会拒绝）
static int cmd_arena_at_snapshot(void) {
    uint64_t at = 0, len = 0;
    if (!s2a_snap_restoring() || s2a_snap_manifest_u64("命令记录区", &at, &len) || !at || len <= CMD_ARENA) return 0;
    void *m = mmap((void *)at, len, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m != (void *)at) { if (m != MAP_FAILED) munmap(m, len); LOG("【红】即时存档恢复：命令记录区占不到快照时的地址 %#llx +%llu MB", (unsigned long long)at, (unsigned long long)(len >> 20)); return 0; }
    g_ca_base = at; g_ca_cut = 1; g_va_cap = len - CMD_ARENA;
    LOG("即时存档恢复：命令记录区按快照占在 %#llx +%llu MB（其后虚拟池区 +%llu GB）", (unsigned long long)at, (unsigned long long)(CMD_ARENA >> 20), (unsigned long long)(g_va_cap >> 30));
    return 1;
}
static void cmd_arena_reserve(void) {   // 占窗口那一步在围栏前调；窗口没占到提前返回的路径由 vawin_boot 补调（只占一次）
    static int done; if (done++) return;
#if !TARGET_OS_OSX
    // 即时存档（不录帧）、窗口放不下标准布局：占窗口时已在宿主段最底部多留了命令记录区（g_snap_below，见 vawin_reserve），这里就地占上。
    //   不取「最高空洞」：那时最高空洞就是窗口下方的宿主保留段，1 GB + 2 GB 正好把它占满，Metal 没处放着色器库、首帧前 NSMallocException
    //   （一款大游戏的 iPhone 实测：「下方 3072 MB 留宿主」与「0x7000000000 +1024 MB、其后 +2 GB」严丝合缝）。也不能从窗口顶端切：窗口之上是别名区伸出段，
    //   游戏的远端访问会静默读写到那里的宿主内存。存档与恢复两次进程窗口落点相同（清单「窗口」项核），这块地址也相同
    if (g_snap_out) {   // 窗口放不下标准布局：命令记录区放在窗口所在空洞之外（见 vawin_reserve）
        uint64_t at = g_snap_out, A = (1ull << 30) + (2ull << 30);
        void *m = mmap((void *)at, A, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m != (void *)at) { if (m != MAP_FAILED) munmap(m, A); s2a_snap_disable(S2A_SNAP_OFF_ADDRESS);
            LOG("【黄】即时存档：窗口外 %#llx +%llu MB 占不到——这一局不开快照，游戏照常玩", (unsigned long long)at, (unsigned long long)(A >> 20)); return; }
        g_ca_base = at; g_ca_cut = 1; g_va_cap = A - CMD_ARENA;
        LOG("即时存档：命令记录区在窗口外 %#llx +%llu MB，其后虚拟池区 +%llu GB", (unsigned long long)at, (unsigned long long)(CMD_ARENA >> 20), (unsigned long long)(g_va_cap >> 30));
        return;
    }
    if (g_snap_below && g_keep_lo) {
        uint64_t at = g_keep_lo, A = g_snap_below;
        void *m = mmap((void *)at, A, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m != (void *)at) { if (m != MAP_FAILED) munmap(m, A); LOG("【红】即时存档：宿主段底部 %#llx +%llu MB 占不到，命令记录区没有，快照这一局存不了 / 恢复不了", (unsigned long long)at, (unsigned long long)(A >> 20)); return; }
        g_ca_base = at; g_ca_cut = 1; g_va_cap = A - CMD_ARENA;
        uint64_t mat = 0, mlen = 0;
        if (s2a_snap_restoring() && (s2a_snap_manifest_u64("命令记录区", &mat, &mlen) || mat != at || mlen != A))
            LOG("【红】即时存档恢复：命令记录区在 %#llx +%llu MB，快照时在 %#llx +%llu MB（窗口落点变了），图形恢复会拒绝", (unsigned long long)at, (unsigned long long)(A >> 20), (unsigned long long)mat, (unsigned long long)(mlen >> 20));
        LOG("即时存档：命令记录区在宿主段最底部 %#llx +%llu MB，其后虚拟池区 +%llu GB；窗口下方宿主保留段 %llu MB 不少",
            (unsigned long long)at, (unsigned long long)(CMD_ARENA >> 20), (unsigned long long)(g_va_cap >> 30), (unsigned long long)((g_base - g_keep_lo - A) >> 20));
        return;
    }
#endif
    if (cmd_arena_at_snapshot()) return;
#if TARGET_OS_OSX
    const char *rec = getenv("S2A_FRAME_REC"), *rp = getenv("S2A_FRAME_REPLAY"), *on = getenv("S2A_CMD_ARENA");
    int sn = s2a_snap_mode();   // 即时存档：存档 / 恢复也要命令记录区（命令句柄落点固定）；开关读唯一定义处（snapshot.c）
    int r = rec && *rec, p = rp && *rp; if (!r && !p && !(on && *on == '1') && !sn) return;
    uint64_t top = s2a_va_top(), at, sz, fit, fat, lat; if (!top) top = 1ull << 47;
    pick_hole(top, CMD_ARENA + VP_ARENA, &at, &sz, &fit, &fat, 0, &lat);
    if (sz) at = at + sz - CMD_ARENA - VP_ARENA;   // 最高那个空洞的顶端一段（pick_hole 给的是空洞起点）
    if (!sz || at < (1ull << 39)) { LOG("【红】命令记录区：2^39 之上没有 %llu MB 的空洞，帧录的命令记录 / 虚拟池回放放不回", (unsigned long long)((CMD_ARENA + VP_ARENA) >> 20)); return; }
    void *m = mmap((void *)at, CMD_ARENA + VP_ARENA, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m != (void *)at) { if (m != MAP_FAILED) munmap(m, CMD_ARENA + VP_ARENA); LOG("【红】命令记录区：%#llx 占不到", (unsigned long long)at); return; }
    g_ca_base = at; g_ca_cut = !p;
    LOG("命令记录区：%#llx +%llu MB，其后虚拟池区 +%llu GB（%s）", (unsigned long long)at, (unsigned long long)(CMD_ARENA >> 20), (unsigned long long)(VP_ARENA >> 30),
        p ? "帧回放：只占不切，录像里的块 / 虚拟池放回原地址" : "帧录：命令记录宿主块、虚拟池地址从这里按顺序切");
#else
    // iOS 帧录（11-06 旷野之息测试设备1）：命令记录块原先 mmap(NULL) 落在低区（0x169800000 一带），Mac 回放进程这段是它自己的库，放不回、整帧回放不了。
    //   录的进程同样占一块区，放在地址上限以下最高空洞的顶端（窗口尾之上；Mac 回放进程复现 iPhone 地址约束时这里是不可访问的保留，按原地址盖得上）。
    //   iOS 地址空间小（上限 2^39），虚拟池区按空洞剩余给，最多 16 GB；占不到就照旧不占（块回到 mmap(NULL)，回放可能放不回）
    //   即时存档：存档 / 恢复同样要命令记录区（命令句柄落点固定）；两次进程起来区落在同一地址才放得回（图形恢复核基址，不同即拒绝）
    const char *rec = getenv("S2A_FRAME_REC");
    if (!(rec && *rec) && !s2a_snap_mode()) return;
    uint64_t top = s2a_va_top(), at, sz, fit, fat, lat, vp = 16ull << 30; if (!top) return;
    pick_hole(top, CMD_ARENA + (1ull << 30), &at, &sz, &fit, &fat, 0, &lat);
    if (!sz) { LOG("【红】命令记录区（iOS 帧录）：没有 %llu MB 的空洞，命令记录块照旧落低区，Mac 回放可能放不回", (unsigned long long)((CMD_ARENA + (1ull << 30)) >> 20)); return; }
    if (sz - CMD_ARENA < vp) vp = (sz - CMD_ARENA) & ~((1ull << 30) - 1);
    at = at + sz - CMD_ARENA - vp;
    void *m = mmap((void *)at, CMD_ARENA + vp, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m != (void *)at) { if (m != MAP_FAILED) munmap(m, CMD_ARENA + vp); LOG("【红】命令记录区（iOS 帧录）：%#llx 占不到", (unsigned long long)at); return; }
    g_ca_base = at; g_ca_cut = 1; g_va_cap = vp;
    LOG("命令记录区（iOS 帧录）：%#llx +%llu MB，其后虚拟池区 +%llu GB（命令记录宿主块、虚拟池地址从这里按顺序切，Mac 回放按原地址放回）", (unsigned long long)at, (unsigned long long)(CMD_ARENA >> 20), (unsigned long long)(vp >> 30));
#endif
}
void *s2a_cmd_arena_map(uint64_t len) {   // 录的进程：切一段可读写的；没有区 / 放的进程 / 切满返回 NULL（调用方退回 mmap(NULL)）
    if (!g_ca_base || !g_ca_cut || !len) return NULL;
    uint64_t n = (len + 16383) & ~16383ull; void *r = NULL;
    pthread_mutex_lock(&g_ca_mu);
    if (g_ca_used + n <= CMD_ARENA) {
        void *m = mmap((void *)(g_ca_base + g_ca_used), n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (m == (void *)(g_ca_base + g_ca_used)) { r = m; g_ca_used += n; }
    } else { static int once; if (!once++) LOG("【红】命令记录区 %llu MB 切满：之后的命令记录宿主块回放放不回", (unsigned long long)(CMD_ARENA >> 20)); }
    pthread_mutex_unlock(&g_ca_mu);
    return r;
}
void *s2a_vp_arena_map(uint64_t len) {   // 录的进程：给一段不可访问的地址（区里本来就是本进程的 PROT_NONE 保留）；没有区 / 放的进程 / 切满返回 NULL
    if (!g_ca_base || !g_ca_cut || !len) return NULL;
    uint64_t n = (len + 16383) & ~16383ull; void *r = NULL;
    pthread_mutex_lock(&g_ca_mu);
    if (g_va_used + n <= g_va_cap) { r = (void *)(g_ca_base + CMD_ARENA + g_va_used); g_va_used += n; }
    else { static int once; if (!once++) LOG("【红】虚拟池区 %llu GB 切满：之后的虚拟池回放放不回", (unsigned long long)(g_va_cap >> 30)); }
    pthread_mutex_unlock(&g_ca_mu);
    return r;
}
int s2a_vp_arena_has(const void *p, uint64_t n) { uint64_t a = (uint64_t)(uintptr_t)p, b = g_ca_base + CMD_ARENA; return g_ca_base && a >= b && a + n <= b + g_va_cap; }
// 即时存档：命令记录区 / 虚拟池区的基址与已用量（存时记、恢复后接着切）
uint64_t s2a_snap_arena_len(void) { return g_ca_base ? CMD_ARENA + g_va_cap : 0; }   // 即时存档：命令记录区 + 虚拟池区的总长（记进清单，恢复按它同址占）
void s2a_snap_arena_get(uint64_t *base, uint64_t *used, uint64_t *vused) { pthread_mutex_lock(&g_ca_mu); *base = g_ca_base; *used = g_ca_used; *vused = g_va_used; pthread_mutex_unlock(&g_ca_mu); }
void s2a_snap_arena_set(uint64_t used, uint64_t vused) { pthread_mutex_lock(&g_ca_mu); if (used > g_ca_used) g_ca_used = used; if (vused > g_va_used) g_va_used = vused; pthread_mutex_unlock(&g_ca_mu); }
int s2a_cmd_arena_has(const void *p, uint64_t n) { uint64_t a = (uint64_t)(uintptr_t)p; return g_ca_base && a >= g_ca_base && a + n <= g_ca_base + CMD_ARENA; }
// 区里的块还回：盖回不可访问（地址不再切，录局里会话重建才走到）
void s2a_cmd_arena_unmap(void *p, uint64_t len) { if (s2a_cmd_arena_has(p, len)) mmap(p, len, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0); else munmap(p, len); }

// 纯虚拟地址预留不提交客体物理页；预热也必须赶在 UIKit / CoreAnimation 的大块预留之前占到整窗。
// 解包专用局（S2A_PACK_APPLY_ONLY=1，host_main.c：只解开推送来的资源包就退出，不起 C# 运行时、不装载游戏）不占客体窗口、不起命令记录区。
//   理由只有一条：解包局用不到客体内存，不占最省（地址、构造期时间）。11-07 曾把测试设备1 解包慢（16 GB 约 300 秒）归因到「匿名窗口在」，
//   14:39 同进程复测证否：匿名窗口在时 1 GB 写 2833 MB/s、解 16 GB 18.7 秒；上午慢局全在 05:46–06:51、快局全在 07:37 之后，窗口有无与时段完全重合（坑记录 11-07「机上解包约 50 MB/秒」）
static int apply_only(void) { const char *e = getenv("S2A_PACK_APPLY_ONLY"); return e && *e == '1'; }
__attribute__((constructor(101))) static void vawin_boot(void) {
    if (apply_only()) { vaguard_release(); LOG("地址窗口：解包专用局（S2A_PACK_APPLY_ONLY=1）不占客体窗口、不起命令记录区"); return; }   // 不占也要放行 Mac 护栏（同 S2A_VAWIN_NORESERVE）
    s2a_window_reserve();
    // 构造期没算出跨度（多款容器里还没有游戏：刚装好、启动后才导入）就先不占命令记录区：它按「最高空洞」占，小地址机型上最高空洞就是唯一能放窗口的那块
    //   （D-VTDQ iPad mini 6：地址上限 0x7180000000，[0x70_0000_0000, 上限) 这 6 GB 被 1 GB + 5 GB 占满，起宿主前补占窗口只剩 0 MB、起不了局）。
    //   补占（s2a_window_reserve_late → vawin_reserve）占好窗口后会接着占它，按窗口定的位置（窗口外 / 宿主段底部），放不下就这一局关快照
    if (g_need || g_noreserve) cmd_arena_reserve();
    else LOG("命令记录区：构造期还没有可算的游戏、窗口没占，先不占；起宿主前补占窗口后再占");
}

// 布局只用窗口前 size 字节时，把尾部还给系统（Metal 的大缓冲要用，见 layout.c）
// ---------- 窗口尾部统一分配（10-24 维护者批：异度神剑3 测试设备2 低区耗尽 OOM）----------
// GC 段模式只能用低区（新段落到尾部要扩卡表覆盖约 508 GB 跨度，必然失败，见坑记录 10-24），所以尾部留给没有簿记表的使用者：
//   Metal 共享缓冲（graphics/metal）、Ryujinx MemoryBlock（Ryujinx.Memory 补丁）、游戏线程宿主栈、信号备用栈、写跟踪页表叶子（graphics/track）。
// 做法（底子是帧率能耗线 perf/buf-tail 的 buftail.c，统一成这一个入口）：窗口收缩时尾部不还系统，保持窗口占位的 PROT_NONE；
//   分配 = 在保留段里 MAP_FIXED 映射匿名页（虚存标签按使用者，地址账可分开认出），释放 = MAP_FIXED 映射回 PROT_NONE（物理页随之归还）。
//   首次适配，空闲段按地址排序并合并。放不下 / 没接管时退回普通匿名映射（落低区），计「退回」；带 S2A_TAIL_ONLY 的调用方要 NULL 自己处理。
// 缺省开（地址摆放，不改语义）；S2A_TAIL=0 应急关：尾部照旧还系统，全部走退回路径
enum { TL_MAXSEG = 4096 };
#define TL_PAGE 16384ull
typedef struct { uint64_t a, n; } tl_seg;
static pthread_mutex_t g_tl_mu = PTHREAD_MUTEX_INITIALIZER;
static tl_seg g_tl_free[TL_MAXSEG]; static int g_tl_nfree;
static uint64_t g_tl_base, g_tl_size, g_tl_used, g_tl_peak, g_tl_segfull;
static struct { uint64_t live, peak, n, fb_n, fb_bytes, fb_live, fb_peak; } g_tl_u[S2A_TAIL_NUSER];   // fb_live / fb_peak：退回低区的那部分此刻在用 / 峰值（#0d51a7）
static const char *const g_tl_uname[S2A_TAIL_NUSER] = {"Metal 缓冲", "Ryujinx 内存块", "线程栈", "信号备用栈", "写跟踪页表", "托管大块暂存"};
static int tail_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("S2A_TAIL"); v = !(e && *e == '0'); } return v; }
static int tl_map(uint64_t a, uint64_t n, int prot, int user) {   // user < 0 = 还回（PROT_NONE、不打标签）
    void *p = mmap((void *)a, n, prot, MAP_PRIVATE | MAP_ANON | MAP_FIXED, user >= 0 ? VM_MAKE_TAG(S2A_TAIL_TAG0 + user) : -1, 0);
    return p == (void *)a ? 0 : -1;
}
void s2a_tail_init(uint64_t base, uint64_t size) {   // 接管 [base, base + size)：调用方保证是本进程的 PROT_NONE 保留；只接管一次
    pthread_mutex_lock(&g_tl_mu);
    uint64_t a = (base + TL_PAGE - 1) & ~(TL_PAGE - 1), e = (base + size) & ~(TL_PAGE - 1);
    if (!g_tl_size && e > a) { g_tl_base = a; g_tl_size = e - a; g_tl_free[0] = (tl_seg){a, e - a}; g_tl_nfree = 1; }
    pthread_mutex_unlock(&g_tl_mu);
}
int s2a_tail_has(const void *p) { uint64_t a = (uint64_t)p; return g_tl_size && a >= g_tl_base && a < g_tl_base + g_tl_size; }
void *s2a_tail_map(uint64_t len, int user, int prot) {
    int only = user & S2A_TAIL_ONLY; user &= ~S2A_TAIL_ONLY; if (user < 0 || user >= S2A_TAIL_NUSER) user = 0;
    if (!len) return NULL;
    uint64_t n = (len + TL_PAGE - 1) & ~(TL_PAGE - 1); void *r = NULL;
    pthread_mutex_lock(&g_tl_mu);
    for (int i = 0; i < g_tl_nfree; i++) if (g_tl_free[i].n >= n) {
        uint64_t a = g_tl_free[i].a;
        if (tl_map(a, n, prot, user)) break;
        if (g_tl_free[i].n == n) { memmove(&g_tl_free[i], &g_tl_free[i + 1], (size_t)(g_tl_nfree - i - 1) * sizeof *g_tl_free); g_tl_nfree--; }
        else { g_tl_free[i].a += n; g_tl_free[i].n -= n; }
        g_tl_used += n; if (g_tl_used > g_tl_peak) g_tl_peak = g_tl_used;
        g_tl_u[user].live += n; if (g_tl_u[user].live > g_tl_u[user].peak) g_tl_u[user].peak = g_tl_u[user].live; g_tl_u[user].n++; r = (void *)a; break;
    }
    if (!r) { g_tl_u[user].fb_n++; g_tl_u[user].fb_bytes += n; }
    pthread_mutex_unlock(&g_tl_mu);
    if (r || only) return r;
    // 退回：普通匿名映射（落低区）。照样打这个使用者的标签、记在用量（#0d51a7：窗口尾部没接管的款每一笔都走这里，原先不带标签，
    //   低区按标签合计里全算「标签 0」、低区账里全算「拦不到的」——长局低区从 2 GB 缩到 12 MB 时看不出是谁）
    void *p = mmap(NULL, len, prot, MAP_PRIVATE | MAP_ANON, VM_MAKE_TAG(S2A_TAIL_TAG0 + user), 0);
    if (p == MAP_FAILED) return NULL;
    pthread_mutex_lock(&g_tl_mu); g_tl_u[user].fb_live += n; if (g_tl_u[user].fb_live > g_tl_u[user].fb_peak) g_tl_u[user].fb_peak = g_tl_u[user].fb_live; pthread_mutex_unlock(&g_tl_mu);
    return p;
}
void s2a_tail_unmap(void *p, uint64_t len, int user) {   // 尾部里的映射回 PROT_NONE 并还进空闲表；不在尾部的（退回路径给的）照常 munmap
    if (!p || !len) return;
    user &= ~S2A_TAIL_ONLY; if (user < 0 || user >= S2A_TAIL_NUSER) user = 0;
    if (!s2a_tail_has(p)) {   // 退回路径给的（落低区）：照常 munmap，扣落低区在用量
        uint64_t m = (len + TL_PAGE - 1) & ~(TL_PAGE - 1); munmap(p, len);
        pthread_mutex_lock(&g_tl_mu); g_tl_u[user].fb_live = g_tl_u[user].fb_live >= m ? g_tl_u[user].fb_live - m : 0; pthread_mutex_unlock(&g_tl_mu);
        return; }
    uint64_t a = (uint64_t)p, n = (len + TL_PAGE - 1) & ~(TL_PAGE - 1);
    pthread_mutex_lock(&g_tl_mu);
    tl_map(a, n, PROT_NONE, -1);
    int i = 0; while (i < g_tl_nfree && g_tl_free[i].a < a) i++;
    int ml = i > 0 && g_tl_free[i - 1].a + g_tl_free[i - 1].n == a, mr = i < g_tl_nfree && a + n == g_tl_free[i].a;
    if (ml && mr) { g_tl_free[i - 1].n += n + g_tl_free[i].n; memmove(&g_tl_free[i], &g_tl_free[i + 1], (size_t)(g_tl_nfree - i - 1) * sizeof *g_tl_free); g_tl_nfree--; }
    else if (ml) g_tl_free[i - 1].n += n;
    else if (mr) { g_tl_free[i].a = a; g_tl_free[i].n += n; }
    else if (g_tl_nfree < TL_MAXSEG) { memmove(&g_tl_free[i + 1], &g_tl_free[i], (size_t)(g_tl_nfree - i) * sizeof *g_tl_free); g_tl_free[i] = (tl_seg){a, n}; g_tl_nfree++; }
    else g_tl_segfull++;   // 空闲段表满：这段地址不再复用（只丢地址、不丢物理页），收局可见
    g_tl_used -= n; g_tl_u[user].live = g_tl_u[user].live >= n ? g_tl_u[user].live - n : 0;
    pthread_mutex_unlock(&g_tl_mu);
}
// 尾部账一行：接管、已用 / 剩余 / 最大空闲块、峰值；按使用者：在用 / 峰值 / 次数 / 退回（次数、字节）
void s2a_tail_note(char *o, size_t cap) {
    pthread_mutex_lock(&g_tl_mu);
    int k;
    if (!g_tl_size) k = snprintf(o, cap, "窗口尾部分配：未接管（%s），各使用者全部落低区", tail_on() ? "尾部为 0 或窗口没占到" : "S2A_TAIL=0 应急关");
    else { uint64_t big = 0; for (int i = 0; i < g_tl_nfree; i++) if (g_tl_free[i].n > big) big = g_tl_free[i].n;
        k = snprintf(o, cap, "窗口尾部分配：接管 %llu MB，已用 %.1f MB、剩余 %.1f MB（最大空闲块 %.1f MB、空闲段 %d）、峰值 %.1f MB",
                     (unsigned long long)(g_tl_size >> 20), g_tl_used / 1048576.0, (g_tl_size - g_tl_used) / 1048576.0, big / 1048576.0, g_tl_nfree, g_tl_peak / 1048576.0); }
    // 按使用者：尾部里在用 / 峰值 / 次数，退回低区的次数、累计字节与此刻在用 / 峰值（未接管时只有后者）
    for (int u = 0; u < S2A_TAIL_NUSER && k > 0 && (size_t)k < cap; u++)
        if (g_tl_u[u].n || g_tl_u[u].fb_n)
            k += snprintf(o + k, cap - k, "；%s 在用 %.1f MB 峰值 %.1f MB 共 %llu 次、退回低区 %llu 次 %.1f MB（落低区在用 %.1f MB 峰值 %.1f MB）", g_tl_uname[u], g_tl_u[u].live / 1048576.0,
                          g_tl_u[u].peak / 1048576.0, (unsigned long long)g_tl_u[u].n, (unsigned long long)g_tl_u[u].fb_n, g_tl_u[u].fb_bytes / 1048576.0,
                          g_tl_u[u].fb_live / 1048576.0, g_tl_u[u].fb_peak / 1048576.0);
    if (g_tl_segfull && k > 0 && (size_t)k < cap) snprintf(o + k, cap - k, "；空闲段表满丢地址 %llu 次", (unsigned long long)g_tl_segfull);
    pthread_mutex_unlock(&g_tl_mu);
}
void s2a_window_trim(uint64_t size) {
    if (!g_ok || size >= g_size) return;
    if (tail_on()) {   // 尾部前 TL_TAKE 留着占位时的 PROT_NONE，交给 s2a_tail_map；再往后还系统
        // 只接管 TL_TAKE（10-24 怪猎包 ef1e9bde9 开局即崩 3/3：整段 32 GB 都接管后，Metal 自己建缓冲（不走 s2a_tail_map）只剩低区 ~1 GB，建 4 MB 共享块失败；
        //   九款实测尾部峰值 ≤ 202 MB，2 GB 够用，其余照旧还系统给 Metal）
        // 窗口顶到空洞末尾（系统资源非 0 的款，窗口 63.25 GB 在 64 GB 空洞里只剩 766 MB）时只接管 TL_SMALL：尾部占着的 PROT_NONE Metal / malloc 用不上，
        //   10-24 八方2 包 b6c1d4192 iPhone 尾部接管 766 MB、只用 121.8 MB，低区账少 644 MB → 命令分配器 5 MB ×6、管线归档 ×2 腾不出（以前的包这 766 MB 还系统）。
        //   窗口之上还有 ≥ 4 GB 空闲（系统资源为 0 的款）才接管 TL_TAKE。尾部用满退回低区，s2a_tail_note 记「退回」
        uint64_t above = 0, al = 0; holes_in(g_base + g_res, 1ull << 39, &above, &al);
        uint64_t cap = above >= (4ull << 30) ? TL_TAKE : TL_SMALL;
        uint64_t take = g_size - size < cap ? g_size - size : cap;
        s2a_tail_init(g_base + size, take);
        if (take < g_size - size && munmap((void *)(g_base + size + take), g_size - size - take)) die("还窗口尾部失败 errno=%d", errno);
        LOG("窗口收到前 %llu MB，尾部 %llu MB 留作统一尾部分配（Ryujinx 内存块 / 线程栈 / 备用栈；S2A_TAIL=0 关）、再往后 %llu MB 还给系统（Metal 缓冲）", (unsigned long long)(size >> 20),
            (unsigned long long)(take >> 20), (unsigned long long)((g_size - size - take) >> 20));
        g_size = size; return; }
    if (munmap((void *)(g_base + size), g_size - size)) die("还窗口尾部失败 errno=%d", errno);
    LOG("窗口收到前 %llu MB，尾部 %llu MB 还给系统", (unsigned long long)(size >> 20), (unsigned long long)((g_size - size) >> 20));
    g_size = size;
}

// 客体窗口常驻（10-23 维护者：巫师3 实占 4.8 GB 里客体约 3.2 GB 只是推算，要读数）：mincore 按 1 GB 一段扫整个窗口，
//   数「在内存」页与「已压缩换出」页（客体内存是窗口里的匿名页，两者都计入进程实占）。向量用 64 KB 静态缓冲，不在低区另申请；没占窗口时两者 0
//   「在内存」有正对照（t_vawin：写 40 MB → +40960 KB）；「已压缩换出」靠 MINCORE_PAGED_OUT，未经正对照——
//   单测里逼不出压缩页（本机 madvise(MADV_PAGEOUT) 返回 ENOTSUP），读这一项时照实标「未经正对照」
// [lo, lo + len) 与窗口的交集里「在内存」/「已压缩换出」的字节（同一扫法；S2A_MEM_SPLIT 心跳分区账按堆区 / 物理映射区各扫一次）
static pthread_mutex_t g_resident_mu = PTHREAD_MUTEX_INITIALIZER;   // vec 是静态缓冲：心跳（主线程）与跳变采样线程（S2A_MEM_SPLIT）都会扫
static void range_resident_locked(uint64_t lo, uint64_t len, uint64_t *res, uint64_t *out);
void s2a_range_resident(uint64_t lo, uint64_t len, uint64_t *res, uint64_t *out) { pthread_mutex_lock(&g_resident_mu); range_resident_locked(lo, len, res, out); pthread_mutex_unlock(&g_resident_mu); }
static void range_resident_locked(uint64_t lo, uint64_t len, uint64_t *res, uint64_t *out) {
    static unsigned char vec[(1u << 30) / 16384];
    uint64_t pg = (uint64_t)getpagesize(), seg = sizeof vec * pg, r = 0, o = 0;
    if (seg > (1ull << 30)) seg = 1ull << 30;
    *res = *out = 0; if (!g_ok || !len) return;
    uint64_t a0 = lo > g_base ? lo : g_base, a1 = lo + len < g_base + g_size ? lo + len : g_base + g_size;
    a0 &= ~(pg - 1);
    for (uint64_t a = a0; a < a1; a += seg) {
        uint64_t n = a1 - a < seg ? a1 - a : seg;
        if (mincore((void *)a, n, (char *)vec)) continue;
        for (uint64_t i = 0, np = (n + pg - 1) / pg; i < np; i++) { if (vec[i] & MINCORE_INCORE) r++; else if (vec[i] & MINCORE_PAGED_OUT) o++; }
    }
    *res = r * pg; *out = o * pg;
}
void s2a_window_resident(uint64_t *res, uint64_t *out) { s2a_range_resident(g_base, g_size, res, out); }

// [lo, hi) 里没被映射的地址：合计与最大一块
static void holes_in(uint64_t lo, uint64_t hi, uint64_t *total, uint64_t *largest) {
    // 小地址档上限以上可能未被 Mach 映射表覆盖，不代表进程真能映射；GC 不得把它算成可用预算。
    uint64_t top = s2a_va_top();
    if (top && hi > top) hi = top;
    if (lo >= hi) return;
    uint64_t prev = lo; vm_address_t a = lo;
    for (;;) {
        vm_size_t sz = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
        int ok = vm_region_64(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) == KERN_SUCCESS;
        uint64_t ge = ok && a < hi ? a : hi;
        if (ge > prev) { *total += ge - prev; if (ge - prev > *largest) *largest = ge - prev; }
        if (!ok || a >= hi) break;
        prev = a + sz; a += sz;
    }
}
// 宿主可用空洞（名字沿用「低区」）：4 GB 到窗口底，加上窗口收尾后还给系统的那段尾部（10-21 占满窗口款缩尾部还回约 766 MB，宿主映射在低区满了后落进去）。
// iPhone 上 Metal ≤256 MB 的共享缓冲、文件映射、线程栈都从这两段取；起局 / 心跳 / 崩溃现场 / 低区账都按它
void s2a_low_holes(uint64_t *total, uint64_t *largest) {
    *total = *largest = 0;
    holes_in(0x100000000ull, g_ok ? g_base : 1ull << 39, total, largest);
    if (g_ok) holes_in(g_base + g_size, 1ull << 39, total, largest);   // 窗口之上到 2^39：收尾还回的那段 + 所在空洞的其余部分（Metal 大缓冲落这里）
}
// 宿主高位区：窗口外、GC 新段用不上的宿主地址（GC 段模式的新段要和已有段连成一个跨度，落到这里簿记表要几 GB 连续地址、必然 OOM，见坑记录 10-24 异度神剑3）。
//   乙（窗口放不下标准布局，g_keep_lo）：窗口下方留给宿主的 [g_keep_lo, g_base)；窗口之上是别名区伸出段（远端），不给。
//   甲：收尾后的窗口之上到 2^39 / 地址上限；尾部统一分配占着的那段已映射，带提示的 mmap 自然跳过。
//   内核给 mmap(NULL) 挑最低的空洞，这两段要等 4 GB 起那段低区满了才轮到：大块、GC 之外的宿主映射（直连库段，graphics/nvn/lib_pack.inc lp_map）
//   带地址提示 mmap（不带 MAP_FIXED，内核从提示往上找第一个放得下的空洞）并核落点。不预先占住：占住了 Metal / malloc 在低区满后就落不进来
//   （10-24 怪猎尾部整段接管、开局即崩，坑记录）。没占到窗口回 0。调用方各自声明（graphics/nvn nvn_mtl.m 弱定义），不进 native.h：它是抽取阶段的冷输入
int s2a_host_hi_range(uint64_t *lo, uint64_t *hi) {
    *lo = *hi = 0; if (!g_ok) return 0;
    if (g_keep_lo) { *lo = g_keep_lo; *hi = g_base; return 1; }
    uint64_t top = s2a_va_top(), h = 1ull << 39; if (top && top < h) h = top;
    if (g_base + g_size >= h) return 0;
    *lo = g_base + g_size; *hi = h; return 1;
}

// 文件句柄用量：已开句柄数（fcntl 逐号探到 min(软上限, 4096)，信号处理里也能调）+ RLIMIT_NOFILE 软 / 硬上限。
// 10-21 巫师3 归档随机开不了：地址与句柄两种耗尽都要现场读数；按需开分片的片数上限按「软上限 − 已开」算（gal 层 dlsym 取）
void s2a_fd_usage(int *open_n, uint64_t *soft, uint64_t *hard) {
    struct rlimit rl = {0, 0}; getrlimit(RLIMIT_NOFILE, &rl); *soft = rl.rlim_cur; *hard = rl.rlim_max;
    int lim = rl.rlim_cur < 4096 ? (int)rl.rlim_cur : 4096, n = 0;
    for (int fd = 0; fd < lim; fd++) if (fcntl(fd, F_GETFD) != -1) n++;
    *open_n = n;
}

// 窗口外映射归账（诊断，起局打一次）：iPhone 上 VAMM 游戏占满窗口时窗口外只剩约 1.7 GB，C# 初始化吃掉约 1 GB，之后建线程 / 开着色器库 / 暂存缓冲相继失败
// （10-20 巫师3 测试设备2：C# 初始化后 589 MB，到第 103 号线程只剩 58 MB）。打最大的 12 块（地址、大小、当前 / 最大权限、内核用途标签）与按标签合计前 8 类
void s2a_vm_top(const char *when) {
    enum { TOPN = 12, NTAG = 256 };
    struct { uint64_t a, n; int p, mp; unsigned tag; } top[TOPN]; int nt = 0; uint64_t bytag[NTAG] = {0}, tot = 0;
    uint64_t wlo = g_ok ? g_base : 0, whi = g_ok ? g_base + g_size : 0;
    vm_address_t a = 0x100000000ull;
    for (;;) {
        vm_size_t sz = 0; natural_t depth = 0; vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &a, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS) break;
        if (a >= wlo && a < whi) { a = whi; continue; }   // 窗口本身不算（本进程占位 / 游戏地址空间）
        unsigned tag = info.user_tag < NTAG ? info.user_tag : 0; bytag[tag] += sz; tot += sz;
        int k = nt < TOPN ? nt++ : -1;
        if (k < 0) { int m = 0; for (int i = 1; i < TOPN; i++) if (top[i].n < top[m].n) m = i; if (top[m].n < sz) k = m; }
        if (k >= 0) { top[k].a = a; top[k].n = sz; top[k].p = info.protection; top[k].mp = info.max_protection; top[k].tag = info.user_tag; }
        a += sz;
    }
    char b[1600]; int o = snprintf(b, sizeof b, "窗口外映射 [%s]：合计 %llu MB；最大 %d 块：", when, (unsigned long long)(tot >> 20), nt);
    for (int r = 0; r < nt; r++) { int m = r; for (int i = r + 1; i < nt; i++) if (top[i].n > top[m].n) m = i; if (m != r) { __typeof__(top[0]) x = top[r]; top[r] = top[m]; top[m] = x; }
        o += snprintf(b + o, sizeof b - (size_t)o, " %#llx %llu MB p%d/%d 标签 %u；", (unsigned long long)top[r].a, (unsigned long long)(top[r].n >> 20), top[r].p, top[r].mp, top[r].tag); }
    LOG("%s", b);
    o = snprintf(b, sizeof b, "窗口外映射 [%s] 按用途标签合计前 8：", when);
    for (int r = 0; r < 8; r++) { int m = -1; for (int i = 0; i < NTAG; i++) if (bytag[i] && (m < 0 || bytag[i] > bytag[m])) m = i; if (m < 0) break;
        o += snprintf(b + o, sizeof b - (size_t)o, " 标签 %d %llu MB；", m, (unsigned long long)(bytag[m] >> 20)); bytag[m] = 0; }
    LOG("%s", b);
}

// 低区文件映射按目录合计（10-21 巫师3：只读文件映射比账上的归档多约 335 MB，要知道是谁的文件）：逐块（只认文件页）proc_regionfilename，按所在目录累加字节与文件块数，打前 12。
// proc_regionfilename 不在 iOS SDK 头文件里：第一次（C# 初始化后，非信号上下文）按名字取，取不到就不打；崩溃处理里只用已取到的指针、不再 dlsym
static int (*g_regfn)(int, uint64_t, void *, uint32_t); static int g_regfn_tried;
// 按目录 + 扩展名合计（10-21 巫师3：同一 shaders 目录里管线归档 .bin 与离线库 .metallib 要分开看）；扫 [lo, hi) 与窗口尾部 [lo2, hi2)（还给系统的那段，没有就传 0, 0）
static void s2a_vm_low_files(const char *when, uint64_t lo, uint64_t hi, uint64_t lo2, uint64_t hi2) {
    if (!g_regfn_tried && strcmp(when, "崩溃时")) { g_regfn_tried = 1; g_regfn = (int (*)(int, uint64_t, void *, uint32_t))dlsym(RTLD_DEFAULT, "proc_regionfilename"); }
    if (!g_regfn) return;
    enum { NDIR = 48 }; static struct { char dir[200]; uint64_t bytes; int n; } t[NDIR]; int nt = 0; uint64_t other = 0;
    memset(t, 0, sizeof t);
    char path[1024];
    for (int seg = 0; seg < 2; seg++) {
    vm_address_t a = seg ? lo2 : lo; uint64_t end = seg ? hi2 : hi;
    for (; a < end;) {
        vm_size_t sz = 0; natural_t depth = 0; vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &a, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS || a >= end || (!seg && info.user_tag == 32)) break;
        // 只认文件页（external_pager）：proc_regionfilename 对匿名区返回它后面那个文件映射的名字（Mac 实测：malloc / Metal 区被记到下一个文件名下，
        // 8 MB 的文件映射合计出 148 MB），10-21 之前这里的按目录合计因此虚高
        int r = info.external_pager ? g_regfn(getpid(), a, path, sizeof path) : 0;
        if (r > 0) {
            path[r < (int)sizeof path ? r : (int)sizeof path - 1] = 0;
            char *sl = strrchr(path, '/'), *dot = sl ? strrchr(sl, '.') : NULL; char ext[16] = "";
            if (dot && strlen(dot) < sizeof ext) snprintf(ext, sizeof ext, "%s", dot);
            if (sl && sl != path) { *sl = 0; if (ext[0] && strlen(path) + strlen(ext) + 3 < sizeof path) strcat(strcat(path, "/*"), ext); }
            int i = 0; for (; i < nt && strncmp(t[i].dir, path, sizeof t[i].dir - 1); i++) {}
            if (i == nt) { if (nt < NDIR) { snprintf(t[nt].dir, sizeof t[nt].dir, "%s", path); nt++; } else { other += sz; a += sz; continue; } }
            t[i].bytes += sz; t[i].n++;
        }
        a += sz;
    }
    }
    for (int k = 0; k < 12 && k < nt; k++) {   // 选出最大的打（就地选择，不分配）
        int m = k; for (int j = k + 1; j < nt; j++) if (t[j].bytes > t[m].bytes) m = j;
        if (m != k) { typeof(t[0]) x = t[k]; t[k] = t[m]; t[m] = x; }
        if (t[k].bytes < (1ull << 20)) break;
        LOG("  低区文件映射 [%s] %6llu MB（%d 块）%s", when, (unsigned long long)(t[k].bytes >> 20), t[k].n, t[k].dir);
    }
    if (other) LOG("  低区文件映射 [%s] 其余目录（超过 %d 个）%llu MB", when, NDIR, (unsigned long long)(other >> 20));
}

// 低区 + 窗口尾部按用途标签合计（10-21 巫师3 测试设备2 进场骤降 1.2 GB，Metal 记账的只涨 68 MB：要分清 GPU 驱动区 100 / 无标签 0 / 线程栈 30 / malloc 的各自涨多少）：
// 前 6 个标签 + 文件页合计写进 out，一行。只读 vm_region，不分配，低区账骤降时调
void s2a_low_tags(char *out, size_t cap) {
    enum { NTAG = 256 }; uint64_t bytag[NTAG] = {0}, file = 0;
    uint64_t lo2 = g_ok ? g_base + g_size : 0;
    for (int seg = 0; seg < 2; seg++) {
        vm_address_t a = seg ? lo2 : 0x100000000ull; uint64_t end = seg ? (lo2 ? 1ull << 39 : 0) : g_ok ? g_base : 1ull << 39;
        while (a < end) {
            vm_size_t sz = 0; natural_t depth = 0; vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
            if (vm_region_recurse_64(mach_task_self(), &a, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS || a >= end || (!seg && info.user_tag == 32)) break;
            uint64_t e = a + sz < end ? sz : end - a; bytag[info.user_tag < NTAG ? info.user_tag : 0] += e; if (info.external_pager) file += e;
            a += sz;
        }
    }
    int o = snprintf(out, cap, "按标签（含尾部）：文件页 %llu MB", (unsigned long long)(file >> 20));
    static const char *const vname[S2A_VMTAG_END - S2A_VMTAG_VPOOL] = {"Switch 2 虚拟池预留", "远端影子区", "游戏共享内存", "宿主其他匿名映射"};   // s2a_lowva.h S2A_VMTAG_*
    for (int r = 0; r < 10 && o > 0 && (size_t)o < cap; r++) { int m = -1; for (int i = 0; i < NTAG; i++) if (bytag[i] && (m < 0 || bytag[i] > bytag[m])) m = i; if (m < 0) break;
        if (m >= S2A_TAIL_TAG0 && m < S2A_TAIL_TAG0 + S2A_TAIL_NUSER) o += snprintf(out + o, cap - (size_t)o, "、%s（标签 %d）%llu MB", g_tl_uname[m - S2A_TAIL_TAG0], m, (unsigned long long)(bytag[m] >> 20));
        else if (m >= S2A_VMTAG_VPOOL && m < S2A_VMTAG_END) o += snprintf(out + o, cap - (size_t)o, "、%s（标签 %d）%llu MB", vname[m - S2A_VMTAG_VPOOL], m, (unsigned long long)(bytag[m] >> 20));
        else o += snprintf(out + o, cap - (size_t)o, "、标签 %d %llu MB", m, (unsigned long long)(bytag[m] >> 20));
        bytag[m] = 0; }
}

// 低区文件映射按目录合计单独打（低区账骤降时调；10-26 巫师3 测试设备1：文件页 60 秒涨约 400 MB，只在 C# 初始化后列过一次、崩溃时宿主自己崩了没打出来，
//   看不出是谁的文件）。范围同低区全列：4 GB 起到窗口，加窗口尾部
void s2a_low_files(const char *when) {
    uint64_t hi = g_ok ? g_base : 1ull << 39, tlo = g_ok && g_size < g_res ? g_base + g_size : 0, thi = tlo ? g_base + g_res : 0;
    s2a_vm_low_files(when, 0x100000000ull, hi, tlo, thi);
}

// 低区全列（10-20 八方2 / 巫师3 iPhone 地址耗尽）：iPhone 上窗口外能用的只有 4 GB 起到 dyld 共享缓存区（标签 32，0x1_8000_0000 起约 57 GB）之间，
// 再往上到窗口是内核保护区（标签 31）。把这段逐块列全（同标签同权限的相邻块合并，空洞也列），不截断，按块对上是谁的：GC 保留 / Ryujinx DRAM / 服务堆 / 游戏模块 / Metal / 线程栈
// 逐块列 [lo, hi)（同标签同权限相邻块合并，≥1 MB 逐块列、更小的只计数，空洞也列）；stop_shared：遇到共享缓存区（标签 32）就停。返回已映射 / 空 / 末端
static void vm_list(const char *name, const char *when, uint64_t lo, uint64_t hi, int stop_shared, uint64_t *used_out, uint64_t *hole_out, uint64_t *end_out, int *nb_out, int *ns_out, uint64_t *small_out) {
    uint64_t prev = lo, ca = 0, cn = 0; int cp = -1, cmp_ = -1; unsigned ctag = 0; int nb = 0, ns = 0; uint64_t used = 0, hole = 0, small = 0;
    #define FLUSH() do { if (cn >= (1ull << 20)) { LOG("  %s %#llx %6llu MB p%d/%d 标签 %u", name, (unsigned long long)ca, (unsigned long long)(cn >> 20), cp, cmp_, ctag); nb++; } else if (cn) { small += cn; ns++; } cn = 0; } while (0)
    vm_address_t a = lo;
    for (;;) {
        vm_size_t sz = 0; natural_t depth = 0; vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        int ok = vm_region_recurse_64(mach_task_self(), &a, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) == KERN_SUCCESS;
        int stop = !ok || a >= hi || (stop_shared && info.user_tag == 32);
        uint64_t ge = stop ? (ok && a < hi ? a : hi) : a;
        if (ge > prev) { FLUSH(); if (ge - prev >= (1ull << 20)) LOG("  %s %#llx %6llu MB 空", name, (unsigned long long)prev, (unsigned long long)((ge - prev) >> 20)); hole += ge - prev; }
        if (stop) break;
        if (a + sz > hi) sz = (vm_size_t)(hi - a);
        if (cn && ca + cn == a && cp == info.protection && cmp_ == info.max_protection && ctag == info.user_tag) cn += sz;
        else { FLUSH(); ca = a; cn = sz; cp = info.protection; cmp_ = info.max_protection; ctag = info.user_tag; }
        used += sz; prev = a + sz; a += sz;
    }
    FLUSH();
    #undef FLUSH
    *used_out = used; *hole_out = hole; *end_out = prev; *nb_out = nb; *ns_out = ns; *small_out = small;
}
void s2a_vm_low(const char *when) {
    uint64_t lo = 0x100000000ull, hi = g_ok ? g_base : 1ull << 39, prev, used, hole, small; int nb, ns;
    LOG("低区全列 [%s]：起 %#llx（同标签同权限相邻块合并，≥1 MB 逐块列、更小的只计数；p 当前/最大权限；空 = 未映射）", when, (unsigned long long)lo);
    vm_list("低区", when, lo, hi, 1, &used, &hole, &prev, &nb, &ns, &small);
    // 乙布局窗口下方留给宿主的那段（[g_keep_lo, g_base)）：低区满了宿主映射按「从低往高第一个放得下」落到这里。原先上面那段遇到共享缓存区就停、这段从来不列，
    //   11-13 时之笛（0400486024DEC000）三台设备开局约 2–4 分钟宿主地址耗尽崩溃（低区 + 这段合计从 3380 MB 跌到 74 MB，进程实占只有 1–4 GB），看不到是谁占了这段
    if (g_ok && g_keep_lo && g_keep_lo < g_base) { uint64_t u2, h2, e2, s2; int nb2, ns2;
        LOG("窗口下方宿主段全列 [%s]：%#llx +%llu MB", when, (unsigned long long)g_keep_lo, (unsigned long long)((g_base - g_keep_lo) >> 20));
        vm_list("下方", when, g_keep_lo, g_base, 0, &u2, &h2, &e2, &nb2, &ns2, &s2);
        LOG("窗口下方宿主段全列 [%s] 完：已映射 %llu MB（逐块列 %d 块；<1 MB 的 %d 块合计 %llu MB 未列）、空 %llu MB", when, (unsigned long long)(u2 >> 20), nb2, ns2, (unsigned long long)(s2 >> 20), (unsigned long long)(h2 >> 20)); }
    { void (*gn)(char *, size_t) = (void (*)(char *, size_t))dlsym(RTLD_DEFAULT, "nxgal_va_note"); if (gn) { char g[1400]; gn(g, sizeof g); LOG("渲染器地址账 [%s]：%s", when, g); } }
    // 窗口尾部（缩尾部后还给系统的那段）：低区满了宿主映射落进去，崩溃现场要看它还剩多少
    uint64_t tlo = g_ok && g_size < g_res ? g_base + g_size : 0, thi = tlo ? g_base + g_res : 0;
    if (tlo) { uint64_t th = 0, tl = 0; holes_in(tlo, thi, &th, &tl);
        LOG("窗口尾部 [%s]：%#llx +%llu MB，已映射 %llu MB、空 %llu MB（最大一块 %llu MB）", when, (unsigned long long)tlo, (unsigned long long)((thi - tlo) >> 20),
            (unsigned long long)((thi - tlo - th) >> 20), (unsigned long long)(th >> 20), (unsigned long long)(tl >> 20)); }
    { char tb[900]; s2a_tail_note(tb, sizeof tb); LOG("%s [%s]", tb, when); }
    s2a_vm_low_files(when, lo, hi, tlo, thi);
    if (g_ok && g_keep_lo && g_keep_lo < g_base) { LOG("（下面一组文件映射是窗口下方宿主段 %#llx 起的 [%s]）", (unsigned long long)g_keep_lo, when); s2a_vm_low_files(when, g_keep_lo, g_base, 0, 0); }
    { void (*mn)(const char *) = (void (*)(const char *))dlsym(RTLD_DEFAULT, "s2a_msample_note"); if (mn) mn(when); }   // malloc 按调用点采样（S2A_MALLOC_SAMPLE=1 才有，见 msample.c）
    LOG("低区全列 [%s] 完：止于 %#llx，已映射 %llu MB（逐块列 %d 块；<1 MB 的 %d 块合计 %llu MB 未列）、空 %llu MB", when, (unsigned long long)prev, (unsigned long long)(used >> 20), nb, ns, (unsigned long long)(small >> 20), (unsigned long long)(hole >> 20));
}

// 探测：列出窗口内的全部映射。
// 占到时窗口归本进程独占（内核不会把别人的映射放进已占的地址），只核「整 64 GB 仍在」——内核按最多 128 MB 一段记账，所以是连续多段；
// 没占（诊断模式）时列出谁映射进来了：这就是 5b-1 要量的「运行时 / Metal 会不会抢窗口」
// 系统给本进程的地址上限（TASK_VM_INFO 的 max_address，rev2 起有）；读不到回 0
uint64_t s2a_va_top(void) {
    task_vm_info_data_t vi; mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &c) == KERN_SUCCESS && c >= TASK_VM_INFO_REV2_COUNT ? vi.max_address : 0;
}
void s2a_window_probe(const char *when) {
    if (!g_ok) { g_probe_n = 0; LOG("窗口探测 [%s]：没有窗口", when); return; }
    uint64_t wb = g_base, ws = g_size;
    vm_address_t a = wb; uint64_t end = wb + ws, covered = 0, expect = wb; int n = 0, contiguous = 1;
    char buf[768]; size_t k = 0; buf[0] = 0;
    while (a < end) {
        vm_size_t sz = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS || a >= end) break;
        n++; if (a != expect) contiguous = 0;
        uint64_t e = a + sz > end ? end : a + sz; covered += e - a; expect = e;
        if (!g_ok && k < sizeof buf - 64) k += (size_t)snprintf(buf + k, sizeof buf - k, " %#lx+%#lx/p%d", (unsigned long)a, (unsigned long)sz, info.protection);
        a += sz;
    }
    g_probe_n = g_ok ? -1 : n;
    if (g_ok) LOG("窗口探测 [%s]：本进程占位%s（%d 段，覆盖 %llu MB）", when, contiguous && covered == ws ? "完整" : "【红】不完整", n, (unsigned long long)(covered >> 20));
    else LOG("窗口探测 [%s]：未占窗口，窗口内别人的映射 %d 段 %llu MB%s", when, n, (unsigned long long)(covered >> 20), buf);
}

// 地址余量走势（诊断，每 30 秒一行）：低区空洞合计 / 最大块 + 渲染器地址账——看低区是缓涨到顶还是有界
static void *va_watch(void *a) { s2a_thread_name("s2a-va-watch");
    (void)a; void (*gn)(char *, size_t) = (void (*)(char *, size_t))dlsym(RTLD_DEFAULT, "nxgal_va_note");
    void (*rr)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "s2a_direct_res_report");   // direct_host.c；只链 vawin 的单测里取不到就不调
    for (int t = 30;; t += 30) { sleep(30); uint64_t ht, hl; s2a_low_holes(&ht, &hl); char g[1400] = ""; if (gn) gn(g, sizeof g);
        LOG("地址余量 第 %d 秒：低区空洞合计 %llu MB、最大一块 %llu MB；%s", t, (unsigned long long)(ht >> 20), (unsigned long long)(hl >> 20), g);
        if (rr) rr(); }   // 直连模式：资源行（s2a_res_report_register 登记），跟着地址余量每 30 秒一次
    return NULL;
}
void s2a_va_watch_start(void) { static int started; if (started) return; pthread_t th; if (!pthread_create(&th, NULL, va_watch, NULL)) { started = 1; pthread_detach(th); } }

// 托管堆 OutOfMemoryException 现场（服务层 MemDiag.InstallOomDump 第一次撞到时按名字调；10-24 异度神剑3 真机：低区还有 580 MB 整块、GC 却连 32 MB 新段都拿不到）。
//   打低区全列、窗口尾部到 2^39 的逐块（GC 新段若落进窗口尾部，卡表 / 标记位图要覆盖到这里）、窗口外最大块与按标签合计。只读 vm_region、不在托管堆上分配
void s2a_oom_dump(const char *when) {
    s2a_vm_low(when);
    uint64_t lo = g_ok ? g_base + g_size : 1ull << 39, hi = 1ull << 39;
    LOG("窗口尾部逐块 [%s]：%#llx 起到 2^39（窗口收尾还给系统的那段；同标签同权限相邻块合并，≥1 MB 逐块列）", when, (unsigned long long)lo);
    vm_address_t a = lo; uint64_t prev = lo;
    for (;;) {
        vm_size_t sz = 0; natural_t depth = 0; vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        int ok = vm_region_recurse_64(mach_task_self(), &a, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) == KERN_SUCCESS;
        uint64_t ge = ok && a < hi ? a : hi;
        if (ge > prev && ge - prev >= (1ull << 20)) LOG("  尾部 %#llx %6llu MB 空", (unsigned long long)prev, (unsigned long long)((ge - prev) >> 20));
        if (!ok || a >= hi) break;
        if (sz >= (1ull << 20)) LOG("  尾部 %#llx %6llu MB p%d/%d 标签 %u", (unsigned long long)a, (unsigned long long)(sz >> 20), info.protection, info.max_protection, info.user_tag);
        prev = a + sz; a += sz;
    }
    s2a_vm_top(when);
    char b[768]; s2a_low_tags(b, sizeof b); LOG("低区账 [%s]：%s", when, b);
}
#endif
