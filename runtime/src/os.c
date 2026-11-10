// 时钟与熵源。游戏能读到的系统计数器（mrs cntpct 门、svc GetSystemTick）都经 guest_ns；新架构不做时间倍率 / 确定时钟（旧库测试专用，删）。
#include <pthread.h>
#include <dlfcn.h>
#include "native.h"
#include "dev_facilities.h"
#include "s2a_docs.h"   // iOS 进程级容器 Documents（s2a_container_docs）
#include "../../platform/plat.h"
#include <stdatomic.h>

#include <fcntl.h>
#include <errno.h>
#if defined(__APPLE__)
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#endif
#if defined(__ANDROID__)
#include <limits.h>
#include <time.h>
#include <string.h>
#if __has_include(<sys/system_properties.h>)
#include <sys/system_properties.h>
#define S2A_SESSION_UUID_PROP 1
#else
#define PROP_VALUE_MAX 92   /* 替身头缺失的同长缓冲；bionic 恒 92（svc_mem.c 同款 has_include 模式） */
#endif
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if TARGET_OS_IPHONE
static void logfd_problem(const char *step, const char *name, int err) {
    // 直接写当前 stderr，不依赖可能正在切换的 logger，也不让诊断 IO 失败终止 App。
    char text[512]; int n = snprintf(text, sizeof text, "[s2a] 【红】日志%s（%s）失败 errno=%d；保留当前日志输出继续运行\n", step, name, err);
    if (n > 0) { size_t size = (size_t)n < sizeof text ? (size_t)n : sizeof text - 1; (void)write(2, text, size); }
}
// 每局一份（11-06 维护者：真机上两局测试之间人玩几次，旧的 host.prev / prev2 只留 3 份、名字不带时刻，弹丸V3 手测那局与 dev_run 冷装局互相挤掉）：
//   起局把上一局 host.log 按它的创建时刻（= 那局起始）改名进 Documents/logs/host-YYYYMMDD-HHMMSS.log，同秒加 _02、_03…（按文件名排序即按时间）；
//   归档最多 LOG_KEEP 份（加本局 host.log 共 10 份），归档总量超 LOG_KEEP_BYTES 从最旧删起（最新一份总留）。旧版的 host.prev.log / host.prev2.log 同样并进归档
enum { LOG_KEEP = 9 };
#define LOG_KEEP_BYTES (256ull << 20)
static int logname_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int log_archive(const char *docs, const char *from) {   // 没有这份返回 0；改名失败返回 -1（errno）
    char src[1100], dst[1200]; struct stat st; snprintf(src, sizeof src, "%s/%s", docs, from);
    if (stat(src, &st)) return errno == ENOENT ? 0 : -1;
    time_t t = st.st_birthtimespec.tv_sec > 0 ? st.st_birthtimespec.tv_sec : st.st_mtimespec.tv_sec;
    struct tm tm; char ts[32]; localtime_r(&t, &tm); strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);
    // 同秒取已有最大序号 + 1（不补空位：最旧那份被删后空出的名字排在同秒其余之前，补进去会被当成最旧当场删掉）
    int last = 0;
    for (int n = 1; n < 100; n++) {
        if (n == 1) snprintf(dst, sizeof dst, "%s/logs/host-%s.log", docs, ts); else snprintf(dst, sizeof dst, "%s/logs/host-%s_%02d.log", docs, ts, n);
        if (!access(dst, F_OK)) last = n;
    }
    if (last >= 99) { errno = EEXIST; return -1; }
    if (last == 0) snprintf(dst, sizeof dst, "%s/logs/host-%s.log", docs, ts); else snprintf(dst, sizeof dst, "%s/logs/host-%s_%02d.log", docs, ts, last + 1);
    return rename(src, dst);
}
static void log_prune(const char *dir) {
    DIR *d = opendir(dir); if (!d) return;
    char *names[256]; int n = 0; struct dirent *e;
    while (n < 256 && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l > 9 && !strncmp(e->d_name, "host-", 5) && !strcmp(e->d_name + l - 4, ".log") && (names[n] = strdup(e->d_name))) n++;
    }
    closedir(d); qsort(names, (size_t)n, sizeof *names, logname_cmp);
    unsigned long long total = 0; int keep = n;   // 从最新往旧数：份数 ≤ LOG_KEEP、总量 ≤ 上限
    for (int i = n - 1; i >= 0; i--) {
        char q[1400]; struct stat st; snprintf(q, sizeof q, "%s/%s", dir, names[i]);
        unsigned long long sz = stat(q, &st) ? 0 : (unsigned long long)st.st_size;
        if (n - i > LOG_KEEP || (i < n - 1 && total + sz > LOG_KEEP_BYTES)) break;
        total += sz; keep = i;
    }
    for (int i = 0; i < n; i++) { if (i < keep) { char q[1400]; snprintf(q, sizeof q, "%s/%s", dir, names[i]); unlink(q); } free(names[i]); }
}
static void logfd_redirect(const char *name, int rotate) {
    char p[1024];
    if (!getenv("HOME")) { logfd_problem("初始化（没有 HOME）", name, EINVAL); return; }
    char docs[1030]; s2a_container_docs(docs, sizeof docs); snprintf(p, sizeof p, "%s/%s", docs, name);   // host.log 进程级：容器 Documents，不随款
    if (rotate) {
        char dir[1040]; snprintf(dir, sizeof dir, "%s/logs", docs);
        if (mkdir(dir, 0755) && errno != EEXIST) { logfd_problem("建归档目录", "logs", errno); return; }
        (void)log_archive(docs, "host.prev2.log"); (void)log_archive(docs, "host.prev.log");
        if (log_archive(docs, "host.log")) { logfd_problem("轮换", "host.log", errno); return; }
        log_prune(dir);
    }
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (fd < 0) { logfd_problem("打开", name, errno); return; }
    int old_stderr = dup(2);
    if (old_stderr < 0) { int err = errno; close(fd); logfd_problem("保存 stderr", name, err); return; }
    if (dup2(fd, 2) < 0) { int err = errno; close(old_stderr); close(fd); logfd_problem("切换 stderr", name, err); return; }
    // 保留原生 logger 的 fd 编号：UIKit 线程正在写时不会遇到关闭再复用的窗口。
    if (g_s2a_logfd != 2 && dup2(fd, g_s2a_logfd) < 0) {
        int err = errno;
        if (dup2(old_stderr, 2) < 0) logfd_problem("恢复 stderr", name, errno);
        close(old_stderr); close(fd); logfd_problem("切换原生 fd", name, err); return;
    }
    close(old_stderr);
    if (fd > 2 && fd != g_s2a_logfd) close(fd);
}
#endif
// 只在 iOS 主线程调用：普通启动由构造函数进入；预热进程等 scene 激活再开始正式会话。
void s2a_log_session_start(void) {
#if TARGET_OS_IPHONE
    static int started; if (started) return; started = 1;
    logfd_redirect("host.log", 1);
    // 预热等待期间连控制台写线程也不起；激活后、C# 重定向标准流前再保留原始 stdout。
    { const char *c = getenv("S2A_LOG_CONSOLE"); if (c && *c == '1') s2a_logcon_start(dup(1)); }
    (void)s2a_dev_on();   // 开发设施开关在 C# 起来前定下并导出成 S2A_DEV_FACILITIES（C# 侧 MemDiag / AudioDiag 读同一个值）
#endif
}
// 开发设施总开关（11-07 质量线，维护者：壳里为开发测试集成的设施是能耗来源；方案见 cmdr/壳内开发设施盘点.md）。
//   判定只此一处：环境变量 S2A_DEV_FACILITIES（0 / 1，排查玩家问题时强制打开）> Info.plist 的 S2ADevFacilities（启动器给玩家出包 S2A_PACK_PURPOSE=user 时
//   pack/ipa_tail.py 写 "0"）> 缺省开（开发包 / Mac 开发局 / 普查都没有这个键）。管：停住现场看门（ap_host.c）、地址走势（vawin.c va_watch）、
//   C# 的 MemDiag 周期账与 AudioDiag。不管安全网：崩溃现场、直连异常看门、低区腾退、GPU 看门狗、帧率表写入照起
int s2a_dev_on(void) {
    static _Atomic int v = -1; int r = atomic_load(&v); if (r >= 0) return r;
    const char *e = getenv("S2A_DEV_FACILITIES"); int on = 1, has = 0;
    { int (*pl)(int *) = (int (*)(int *))dlsym(RTLD_DEFAULT, "s2a_dev_plist"); if (pl) has = pl(&on); }   // Info.plist 由 iOS 壳读（settings.m s2a_dev_plist）；按名字取：Mac 宿主 / 单测没有 = 没有这个键，链接不留未定义
    r = s2a_dev_decide(e, has, on);
    int exp = -1; if (atomic_compare_exchange_strong(&v, &exp, r)) {
        setenv("S2A_DEV_FACILITIES", r ? "1" : "0", 1);
        LOG("开发设施：%s（依据 %s）", r ? "开" : "关（玩家口径：停住现场 / 地址走势 / 内存周期账 / 音频周期账不起）", e && *e ? "环境变量 S2A_DEV_FACILITIES" : has ? "Info.plist S2ADevFacilities" : "缺省（开发包）");
    } else r = exp;
    return r;
}
// 早于 vawin.c（101）。未激活的预热进程不计入正式会话归档（每局一份、留 10 份），不能挤掉崩溃现场或触发 unknown 诊断提示。
__attribute__((constructor(100))) static void logfd_init(void) {
#if TARGET_OS_IPHONE
    if (s2a_process_prewarmed()) logfd_redirect("host.prewarm.log", 0);
    else s2a_log_session_start();
#endif
    int fd = dup(2); if (fd >= 0) g_s2a_logfd = fd;
}

#if defined(__ANDROID__)
// Android 私有启动接缝：runtime 单次准入后、loader / NativeAOT 服务启动前调用。
// constructor 尚不知道 app 私有目录；此处不替换 LOG，也不在信号处理里调用 Android logger。
__attribute__((visibility("hidden"))) int s2a_android_session_log_start(const char *state_dir, uint64_t session) {
    if (!state_dir || state_dir[0] != '/' || !session) return EINVAL;
    // 保留 constructor 的独立 fd 编号。后续 StdErrAdapter 可以接管 fd 2，原生日志不进它的异步管道。
    if (g_s2a_logfd <= STDERR_FILENO) return EBADF;
    char path[PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/host-%llu-%d-XXXXXX.log", state_dir,
        (unsigned long long)session, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof path) return ENAMETOOLONG;
    int targets[] = {g_s2a_logfd, STDERR_FILENO, STDOUT_FILENO};
    int saved[] = {-1, -1, -1}, fd = -1, error = 0, bound = 0;
    for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++) {
        saved[i] = fcntl(targets[i], F_DUPFD_CLOEXEC, 3);
        if (saved[i] < 0) { error = errno; goto done; }
    }
    // 唯一文件、0600、追加写，重启不截断上一局。目录由实际 GameSession 创建并校验。
    fd = mkstemps(path, 4);
    if (fd < 0) { error = errno; goto done; }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_APPEND) < 0) {
        error = errno; goto done;
    }
    // 本局身份：运行器起局前 setprop debug.s2a.session_uuid <uuid>，写进首行供取证
    // 按内容绑本局（文件名/PID/mtime 都证明不了「这局」——同 PID 旧文件全靠它区分）。
    // 属性没置上/读到空 → uuid=-（取证端判 unproven，不冒充 bound）。
    char uuid[PROP_VALUE_MAX];
#ifdef S2A_SESSION_UUID_PROP
    if (__system_property_get("debug.s2a.session_uuid", uuid) <= 0)
#endif
        strcpy(uuid, "-");
    if (dprintf(fd, "[s2a] Android 会话日志：session=%llu pid=%d uuid=%s file=%s\n",
        (unsigned long long)session, (int)getpid(), uuid, path) < 0) { error = errno; goto done; }
    // stdout 收 Ryujinx ConsoleLogTarget；stderr 收 Console.Error；native fd 保留原始行。
    for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++) {
        if (dup2(fd, targets[i]) < 0) { error = errno; goto done; }
        bound++;
    }
done:
    if (error) {
        // 部分 dup2 失败时恢复已改动的描述符；不能让下一阶段在半接线状态继续。
        for (int i = 0; i < bound; i++) if (dup2(saved[i], targets[i]) < 0) abort();
    }
    if (fd >= 0) close(fd);
    for (size_t i = 0; i < sizeof saved / sizeof saved[0]; i++) if (saved[i] >= 0) close(saved[i]);
    return error;
}
#endif

uint64_t now_ns(void) {
    return plat_now_ns();
}
// 游戏时钟：Switch 挂起时系统计数器停，唤醒后从挂起那一刻接着走（#495：这里原先直接返回壁钟，切后台 N 秒回来游戏看到时间跳 N 秒，
//   剧情动画按时间补帧在两帧之间来回跳）。按原因冻结（位图：1 后台、2 系统键盘），全部原因解除才接着走；g_clk_lost = 累计冻掉的纳秒
static _Atomic unsigned g_clk_fz;       // 冻结原因位图；非 0 = 冻结中
static _Atomic uint64_t g_clk_lost;     // 壁钟 − 游戏时间
static _Atomic uint64_t g_clk_fv;       // 冻结期间读到的游戏时间（冻结那一刻）；0 = 冻结方还没写好
static pthread_mutex_t g_clk_mu = PTHREAD_MUTEX_INITIALIZER;   // 冻结 / 解冻串行（读不加锁）
// 单调：冻结先置位、后写冻结值，读者算完壁钟再看一次位——看到没冻结的那次读，壁钟一定早于冻结时刻；
//   解冻先改 lost（= 现在 − 冻结值，接着冻结值走）、后清位，读者前后两次读到的 lost 不同就重来
uint64_t guest_ns(void) {
    for (;;) {
        uint64_t lost = atomic_load(&g_clk_lost);
        if (atomic_load(&g_clk_fz)) { uint64_t fv = atomic_load(&g_clk_fv); if (fv) return fv; continue; }
        uint64_t t = now_ns();
        if (!atomic_load(&g_clk_fz) && lost == atomic_load(&g_clk_lost)) return t - lost;
    }
}
void guest_clock_freeze_for(int reason, int on) {
    unsigned bit = 1u << (reason > 0 && reason < 32 ? reason : 1);
    pthread_mutex_lock(&g_clk_mu);
    unsigned prev = atomic_load(&g_clk_fz);
    if (on && !prev) { atomic_store(&g_clk_fv, 0); atomic_store(&g_clk_fz, bit); atomic_store(&g_clk_fv, now_ns() - atomic_load(&g_clk_lost)); }
    else if (on) atomic_store(&g_clk_fz, prev | bit);
    else if ((prev & bit) && !(prev & ~bit)) { atomic_store(&g_clk_lost, now_ns() - atomic_load(&g_clk_fv)); atomic_store(&g_clk_fz, 0); }
    else atomic_store(&g_clk_fz, prev & ~bit);
    pthread_mutex_unlock(&g_clk_mu);
}
void guest_clock_freeze(int on) { guest_clock_freeze_for(1, on); }
// 前后台 / 焦点与底座（缺口扫描族 1 / 族 2，维护者 11-01 派体验线）：平台壳是唯一写者（iOS 前后台、Mac 失焦），
//   服务层每拍读它们、变了就转成游戏的焦点消息 / ChangeDockedModeState；游戏要不要「失焦就暂停」由服务层按 SetOutOfFocusSuspendingEnabled
//   写回，平台壳失焦时按它冻结（iOS 后台本来就整体冻结，不看它）
static _Atomic int g_host_focus = 1, g_focus_suspend = 1, g_console_mode = -1;   // 失焦暂停缺省开（同 AppletStateMgr）
void s2a_host_focus_set(int focused) { atomic_store(&g_host_focus, focused ? 1 : 0); }
int s2a_host_focused(void) { return atomic_load(&g_host_focus); }            // 导出给 C#（接口 v2.15）
void s2a_focus_suspend_set(int on) { atomic_store(&g_focus_suspend, on ? 1 : 0); }   // 导出给 C#（接口 v2.15）
int s2a_focus_suspend(void) { return atomic_load(&g_focus_suspend); }
// 底座：-1 = 平台壳没设过（服务层用起局值 S2A_DOCKED）；0 掌机、1 主机
void s2a_console_mode_set(int docked) { atomic_store(&g_console_mode, docked ? 1 : 0); }
int s2a_console_mode(void) { return atomic_load(&g_console_mode); }         // 导出给 C#（接口 v2.15）
// 睡眠（与失焦分开）：iOS 进后台、Mac 系统睡眠 = 1；服务层据此向 psc 发休眠 / 唤醒请求（Mac 切窗口只失焦、不算睡眠）
static _Atomic int g_host_sleep;
void s2a_host_sleep_set(int sleeping) { atomic_store(&g_host_sleep, sleeping ? 1 : 0); }
int s2a_host_sleeping(void) { return atomic_load(&g_host_sleep); }         // 导出给 C#（接口 v2.15）
// 系统计数器：Switch 19.2 MHz；Switch 2 31.25 MHz（svc_mem.c 按 NPDM 起局时设，游戏线程起来前只设一次）
static uint64_t g_tick_hz = 19200000;
void s2a_guest_tick_hz_set(uint64_t hz) { g_tick_hz = hz; }
uint64_t s2a_guest_tick_hz(void) { return g_tick_hz; }       // 导出给 C#（接口 v2.16）：时间服务按它换算
uint64_t s2a_ns_to_tick(uint64_t ns) { return (uint64_t)((unsigned __int128)ns * g_tick_hz / 1000000000u); }
uint64_t guest_tick(void) { return s2a_ns_to_tick(guest_ns()); }
uint64_t s2a_guest_tick(void) { return guest_tick(); }       // 导出给 C#（接口 v2.8）：与 cntpct 门 / GetSystemTick 同一个计数器
// 含睡眠的单调钟（#863）：苹果 CLOCK_MONOTONIC 睡眠照走（plat_now_ns 的 mach_absolute_time 不走），Linux / 安卓用 CLOCK_BOOTTIME。弱定义：单测换成假钟
__attribute__((weak)) uint64_t s2a_cont_ns(void) {
    struct timespec ts;
#ifdef CLOCK_BOOTTIME
    if (clock_gettime(CLOCK_BOOTTIME, &ts)) return 0;
#else
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
int64_t s2a_clock_unseen_ns(void) { return (int64_t)(s2a_cont_ns() - guest_ns()); }
static void (*_Atomic g_clk_catchup)(void);
void s2a_clock_register_catchup(void (*cb)(void)) { atomic_store(&g_clk_catchup, cb); }
void s2a_clock_catchup(void) { void (*cb)(void) = atomic_load(&g_clk_catchup); if (cb) cb(); }

// 熵源：缺省真随机。S2A_FIXED_ENTROPY=1（测试层用：每局同一组随机内容，如动森的岛形与居民，路线才能复用）改成固定种子的 splitmix64 流。
// 出口：libc getentropy（直通表）、GetInfo RandomEntropy；C# 侧的随机服务（csrng）自己取，不经这里
static int fixed_entropy(void) { static int c = -1; if (c < 0) { const char *e = getenv("S2A_FIXED_ENTROPY"); c = e && *e == '1'; } return c; }
static _Atomic uint64_t g_rng = 0x5357444554524e47ull;
// 全部客体 / 服务读者退出后，新局继承当前宿主冻结原因（后台 / 键盘），
// 但不继承旧局累计暂停时长或固定熵流的游标。
void s2a_os_session_reset(void) {
    pthread_mutex_lock(&g_clk_mu);
    atomic_store(&g_clk_lost, 0);
    atomic_store(&g_clk_fv, atomic_load(&g_clk_fz) ? now_ns() : 0);
    pthread_mutex_unlock(&g_clk_mu);
    atomic_store(&g_rng, 0x5357444554524e47ull);
}
uint64_t s2a_entropy64(void) {
    if (!fixed_entropy()) return ((uint64_t)arc4random() << 32) | arc4random();
    uint64_t z = atomic_fetch_add(&g_rng, 0x9E3779B97F4A7C15ull) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31);
}
int s2a_getentropy(void *p, size_t n) {
    if (!fixed_entropy()) { arc4random_buf(p, n); return 0; }
    for (size_t i = 0; i < n; i += 8) { uint64_t v = s2a_entropy64(); memcpy((uint8_t *)p + i, &v, n - i < 8 ? n - i : 8); }
    return 0;
}
