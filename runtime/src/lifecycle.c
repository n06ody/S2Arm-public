// 宿主生命周期：优雅退出（游戏 ExitProcess / Break、SIGTERM、iOS App 终止回调）。先停住游戏线程的系统调用，再让 C# 停声音 / 显卡，再写报红汇总，_exit（码见 s2a_host_exit_code，缺省 0）；1.5 秒兜底直接退。
// 弱链接[host,ios,nvntest]：nxgal_gpu_hold（通用渲染器 gal_core.m 提供；链接行按这行放行未定义，runtime/build.mk 自动收）
#include "native.h"
#if (defined(__ANDROID__) || defined(__OHOS__))
#include "../../android/native/runtime/platform/lifecycle_android.h"
#include <android/log.h>
#include <errno.h>
#include <time.h>
// 退出码 125 = 收尾未完成。收尾时日志管道另一端的读取线程可能已经停了，所以这几个失败分支
// 同时直接写 logcat（tag S2Arm），保证真实原因不随进程退出丢失。
#define S2A_EXIT_FAIL(...) __android_log_print(ANDROID_LOG_ERROR, "S2Arm", "[exit125] " __VA_ARGS__)
#else
#define S2A_EXIT_FAIL(...) ((void)0)
#include <dispatch/dispatch.h>
#endif
#include <dlfcn.h>
#include <signal.h>
#include <stdatomic.h>
#include <unistd.h>
#include "../include/s2a_direct_host.h"
void s2a_cs_shutdown(const char *why);
__attribute__((weak)) void nxgal_gpu_hold(int on, const char *why);   // gal_core.m
void s2a_svc_red_summary(void);
__attribute__((weak)) void s2a_trap_report(const char *when);   // traps.c；只编 lifecycle.c 的单测里没有它
unsigned s2a_unresolved_count(void);
// 收尾各阶段的累计耗时。1.5 秒兜底只会给出「超时」，不会说明停在哪一步；
// 每次收局都打一次这几行，真机上直接看出是哪一步越过了预算。
#define S2A_SHUTDOWN_STAGE(name, start) LOG("收局阶段 %s：累计 %.1f ms", (name), (double)(now_ns() - (start)) / 1e6)
#if (defined(__ANDROID__) || defined(__OHOS__))
int s2a_android_guest_writers_installed(void);   // runtime/fault/thread_android.inc（安卓专用；宿主夹具给替身）
// Only after terminal SVC park: a normal native shutdown caller cannot publish the
// memory-SVC admission-safe state. An existing writer may still be completing its
// transaction; retry acquisition instead of treating that contention as failed cleanup.
// The already-started process watchdog bounds this wait. All other errors stay fatal.
static int android_shutdown_writers_begin(void) {
    int error;
    while ((error = s2a_android_guest_shutdown_writers_begin()) == EBUSY) {
        const struct timespec slice = {0, 1000000};
        nanosleep(&slice, NULL);
    }
    return error;
}
#endif
// 收局退出码：0 = 正常收局；要让启动器知道「这局是被停下的、不是玩完了」的路径在收局前设（GPU 挂死安全停 = 86，同 gal_core.m 命令缓冲出错直接退的码）。
//   兜底的两次 _exit 和正常收尾都用它，否则安全停后进程照样 0 退出、启动器任务显示「操作完成」（#1257）
static _Atomic int g_exit_code;
void s2a_host_exit_code(int code) { atomic_store(&g_exit_code, code); }
void s2a_host_shutdown(const char *why) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    // 优雅收局先删「本局在跑」标记（D-D3J8，玩家反馈「有时起不来、划掉重启又好」）：标记原先只在进后台 / iOS 终止回调里删，
    //   游戏内「切换游戏」「退出游戏」「打开存档管理」、GPU 卡住选退出、游戏 ExitProcess / Break、SIGTERM 都走这里在前台 _exit，标记留着，
    //   下一局开同款被当成「上一局在前台死于内存」强制开客体内存文件后备（vawin.c s2a_guest_run_died；那款起局慢 6 倍，玩家看实占 0.1 GB 以为卡死又划掉）。
    //   走到这里就不是被系统杀：只有真被杀（SIGKILL）/ 崩（crash.c 改写成 crash）才留标记。按名字取（单测、安卓宿主不链 vawin.c 的没有它）
    { void (*run_mark)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "s2a_guest_run_mark"); if (run_mark) run_mark(0); }
    // 游玩时间（runtime/src/playtime.c）：收局把最后一次心跳之后攒的秒数写进去（收局走 _exit，不跑 atexit）；按名字取（只链本文件的单测没有它）
    { int (*pt_flush)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "s2a_playtime_flush"); if (pt_flush) pt_flush(); }
#if !defined(__ANDROID__) && !defined(__OHOS__)
    // 兜底先开（11-14 iPhone 实机 第 17515 帧卡死）：原先排在统计汇总之后，游戏卡死时收尾停在更早一步（日志只到「收到 自动玩家结束：优雅退出」，
    //   一行收局阶段都没有），兜底从没开，进程挂到设备锁收割 SIGKILL。带着音频会话被强杀之后，手机系统声音服务一直坏到重启。
    //   1.5 秒：照旧打汇总、冲日志再退；汇总或冲日志本身也可能等卡住线程持有的锁，2.5 秒再无条件 _exit
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{ LOG("【红】优雅退出 1.5 秒未完成，直接退出"); s2a_direct_summary(0); s2a_logcon_flush(); _exit(atomic_load(&g_exit_code)); });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2500 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{ _exit(atomic_load(&g_exit_code)); });
#endif
    // Native process exit seals the host even between managed session owners.
    // Generation zero is reserved for this unconditional terminal publication.
    void (*session_exit)(unsigned) = (void (*)(unsigned))dlsym(RTLD_DEFAULT, "s2a_session_process_exit");
    if (session_exit) session_exit(0);
    // #618：管线归档停写、删 .writing（graphics/nvn/gfx/nvn/pso_archive.h）：正常收局时写线程可能正写到一半，_exit 后留下的标记会被下一局当成 #756 的「死在写里」、从此不写
    { void (*par_exit)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "s2a_pso_archive_exit"); if (par_exit) par_exit(); }
#if (defined(__ANDROID__) || defined(__OHOS__))
    // 在任何诊断、锁或托管回调前开兜底；异常路径不能把超时记为成功退出。
    if (s2a_android_shutdown_watchdog_start()) { S2A_EXIT_FAIL("shutdown watchdog start failed"); _exit(125); }
#endif
    uint64_t t0 = now_ns(); LOG("收到 %s：优雅退出", why);
#if (defined(__ANDROID__) || defined(__OHOS__))
    // 直连模式阶段 1：先停止受理客体新的 Vulkan 调用：后面的停系统调用 / 关显卡期间客体不能再交帧，
    // 否则销毁 device 时仍有线程停在驱动里（真机上是驱动内部卡死或堆 abort）。
    // 返回 -1：本局从未配置直连（之后也不会再配置），没有可排空的驱动帧。
    int vk_direct;
    { int s2a_android_vk_quiesce(void); int inflight = s2a_android_vk_quiesce(); vk_direct = inflight >= 0;
      if (inflight > 0) LOG("收局：客体还有 %d 个Vulkan调用在驱动里，等待其返回", inflight); }
#endif
    // 退出路径不依赖前后台状态（10-20 测试设备2 结束进程后设备文件服务挂住，维护者要求核）：先放开「失去前台」时的游戏线程冻结与 GPU 闸门，
    // 后面停系统调用 / 关 C# 服务时不会有人等被冻结的线程或卡在闸门里的提交者（在后台时放开闸门的提交会被系统拒、走出错退出，本来就在退出）
    { void s2a_guest_freeze(int); s2a_guest_freeze(0); if (nxgal_gpu_hold) nxgal_gpu_hold(0, "退出"); }   // 渲染器库弱引用：不带 C# / 渲染器的宿主构建（runtime/gate.sh）里没有它
#if (defined(__ANDROID__) || defined(__OHOS__))
    // 阶段 2：在取消服务等待、SVC 终局 park 与 writer epoch 之前，有界排空已受理的 Vulkan 驱动帧。
    // 其中的反向 allocator 回调可能进 SVC 或取回虚拟核；终局门一旦发布，它会停在驱动帧中间，
    // 外层 token / WSI / 驱动内部锁永远不放。收局线程等待期间不碰 guest 内存，标为 census 安全并放核。
    // 排不完就不进入不可逆阶段：保留全部 Vulkan 资源，报红失败退出。
    // 失败时不调 host_wait_end：恢复虚拟核 / 过 barrier 可能阻塞到 watchdog，把 125 变 124 并吞掉红日志；
    // 本线程此后只会 _exit，保持 census 安全就是真实状态。
    if (vk_direct) { int s2a_android_vk_drain_admitted(void);
      s2a_android_guest_host_wait_begin();
      int error = s2a_android_vk_drain_admitted();
      if (error) {
          LOG("【红】收局：已受理的 Vulkan 调用未排空 errno=%d，不发布终局停机门，进程失败退出", error);
          _exit(125);
      }
      s2a_android_guest_host_wait_end();
      S2A_SHUTDOWN_STAGE("排空已受理Vulkan调用", t0); }
#endif
    { void s2a_track_stat_log(const char *); s2a_track_stat_log("收局"); }
#if defined(__APPLE__)
    { void (*er)(const char *) = (void (*)(const char *))dlsym(RTLD_DEFAULT, "s2a_er_totals_log"); if (er) er("收局"); }   // 宿主代执行全进程累计（crash.c，苹果；只链本文件的单测取不到）
#endif
    { uint64_t s2a_track_protected_pages(void); LOG("写跟踪累计保护页 %llu 页（直连模式下应为 0：写保护只归直连写跟踪）", (unsigned long long)s2a_track_protected_pages()); }
    { void s2a_direct_res_report(void); s2a_direct_res_report(); }   // 直连模式：资源行（s2a_res_report_register 登记）
    { void s2a_sched_stat_log(const char *); s2a_sched_stat_log("收局"); }   // S2A_CORES 调度计数（虚拟核关时不打）
    { int s2a_handle_peak(void); LOG("句柄表峰值 %d 格（Switch 上限 1024）", s2a_handle_peak()); }
    { void s2a_yield_stat_log(const char *); s2a_yield_stat_log("收局"); }   // 让步退避计数（S2A_YIELD_BACKOFF 关时不打）
    // 诊断打点收局汇总（diag_brk.c，S2A_DIAG_STAMP 没设时不打）
#if (!defined(__ANDROID__) && !defined(__OHOS__))
    // Android 不链接 Apple BRK 诊断对象；真实 fault dispatcher 不以空汇总替代它。
    { void s2a_diag_stamp_report(void); s2a_diag_stamp_report(); }
#endif
    { void s2a_lockwake_report(void); s2a_lockwake_report(); }
    if (s2a_trap_report) s2a_trap_report("收局");   // 双补丁岛 brk 兜底点命中（traps.c）
    { void s2a_svc_count_report(const char *); s2a_svc_count_report("收局"); }
    { void s2a_guest_va_report(const char *); s2a_guest_va_report("收局"); }
#if (defined(__ANDROID__) || defined(__OHOS__))
    // 单线程（基线）：地址余量仍在这里、不限时；拆线程挪到 StopGpu 之后、有界（见下）
    if (!s2a_android_shutdown_threaded()) { void s2a_vm_low(const char *); s2a_vm_low("收局"); }
#else
    { void s2a_vm_low(const char *); s2a_vm_low("收局"); }
#endif
    s2a_lowva_peak_report("收局");
    S2A_SHUTDOWN_STAGE("统计与诊断汇总", t0);
    // 先让阻塞在服务等待里的调用立刻返回（C# 导出 S2A_CancelWaits，按名字取：不带 C# 的宿主构建里没有），再停游戏线程的系统调用、等在 C# 里的走完
#if (defined(__ANDROID__) || defined(__OHOS__))
    // NativeAOT 与 core 静态并入同一 DSO；隐藏归档符号不会进入 dlsym 的动态表。
    // 强引用也保证缺少真实取消实现时链接失败，不能悄悄跳过服务等待排空。
    S2A_CancelWaits();
#else
    { void (*cancel)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "S2A_CancelWaits"); if (cancel) cancel(); }
#endif
    S2A_SHUTDOWN_STAGE("取消服务等待", t0);
    { int s2a_svc_park_all(int wait_ms); int left = s2a_svc_park_all(300);   // 先停游戏线程的系统调用、等在 C# 里的走完，再关服务层
      if (left) {
#if (defined(__ANDROID__) || defined(__OHOS__))
          // 仍有调用使用服务对象时不销毁它们；整个进程退出后才允许启动下一局。
          LOG("【红】收局：还有 %d 个系统调用在 C# 里没回（等了 300 ms），进程失败退出", left);
          S2A_EXIT_FAIL("services still in flight: %d (park_all 300 ms)", left);
          _exit(125);
#else
          LOG("【红】收局：还有 %d 个系统调用在 C# 里没回（等了 300 ms），照常关停", left);
#endif
      } }
    S2A_SHUTDOWN_STAGE("停住系统调用", t0);
#if (defined(__ANDROID__) || defined(__OHOS__))
    // SVC 排空只涵盖服务入口；普通 guest 访存仍可触发 tracking fault→GPU 回调。
    // 真实线程 roster 的 writer epoch 同时挡住新 guest 入口，并等既有回调返回。
    // 成功后不 end：所有 guest 保持停稳到 _exit，绝不在 Dispose 后恢复它们。
    // “从未安装停车通道”与“装上了但没停稳”必须分开：这里只问 installed（装过没有），
    // 不看当前 SIGURG 处理器是不是我们自己的——被驱动/别的库覆盖时 guest 线程还在跑，
    // 那种情况必须照旧走停稳等待并由 android_shutdown_writers_begin() 返回 ENOTSUP 准确红退。
    { int tracking_installed = s2a_android_guest_writers_installed();
      if (!tracking_installed) LOG("收局：本局从未安装 guest 写跟踪停车通道（不同于“装过又被覆盖”），跳过停稳等待");
      int error = tracking_installed ? android_shutdown_writers_begin() : 0;
      if (error) {
          LOG("【红】收局：guest / fault 未停稳 errno=%d，保留服务资源并失败退出", error);
          S2A_EXIT_FAIL("guest/fault writers not settled: error=%d", error);
          _exit(125);
      } }
    S2A_SHUTDOWN_STAGE("停稳访存writer", t0);
#endif
    s2a_cs_shutdown(why);
    S2A_SHUTDOWN_STAGE("托管服务与显卡关停", t0);
#if (defined(__ANDROID__) || defined(__OHOS__))
    // void 回调返回不代表托管清理成功；只能接受同一 session 的真实 Stopped ack。
    // helper 同时处理从未尝试 services Start 的早期退出，并保留具体失败日志。
    { int error = s2a_android_runtime_shutdown_result();
      if (error) {
          LOG("【红】收局：服务清理未确认 error=%d，进程失败退出", error);
          S2A_EXIT_FAIL("services cleanup not confirmed: error=%d", error);
          _exit(125);
      } }
    S2A_SHUTDOWN_STAGE("清理结果核验", t0);
#ifdef S2A_ANDROID_PGO_GEN
    if (atomic_load(&g_exit_code) == 0) {
        s2a_android_pgo_write();   // 收局调用 _exit，Clang 的 atexit 写盘不会执行。
        S2A_SHUTDOWN_STAGE("PGO 写盘", t0);
    }
#endif
#endif
#if (defined(__ANDROID__) || defined(__OHOS__))
    // 拆线程：地址余量要读 /proc/self/maps（拿 mmap_lock 读锁），收局前段游戏 / GPU 线程还在 mprotect / mmap，真机 9000 读了 862 ms、
    // 两局读不完被 1.5 s 兜底结束，收局请求都没发出。挪到服务层 Cleanup / StopGpu 之后（游戏线程已停稳、GPU 已停），并且有界：超时打一行黄、不等
    if (s2a_android_shutdown_threaded()) s2a_android_shutdown_vm_low();
#endif
    { void s2a_widen_scan(void); s2a_widen_scan(); }   // 宽屏点首次命中：收局前补扫一次
    s2a_svc_red_summary();
#if !defined(__ANDROID__) && !defined(__OHOS__)
    { void s2a_punch_summary(void); s2a_punch_summary(); }   // #258 文件后备懒清零读数
#endif
    { void s2a_det_summary(void); s2a_det_summary(); }
    LOG("未解析导入：%u 个", s2a_unresolved_count());
    s2a_direct_summary(1);   // _exit 不跑 atexit；显式打印现编汇总，不改变停机语义
#if defined(__ANDROID__) || defined(__OHOS__)
    // 安卓：钩子在同一个单库里（android/native/runtime/platform/lifecycle_android.c），dlsym(RTLD_DEFAULT) 取不到库内符号
    //   （docs/坑记录.md「安卓单库里 dlsym(RTLD_DEFAULT) 找不到服务层导出」），改链接期弱引用；不带它的宿主单测链接里为空，照旧跳过。
    // 弱链接[android]：s2a_sesmark_clear_home（只有安卓 NDK 单库与 Linux 宿主单测链这一支，都不走 ld64，不需要放行参数）
    { extern void s2a_sesmark_clear_home(void) __attribute__((weak)); if (s2a_sesmark_clear_home) s2a_sesmark_clear_home(); }   // 会话标记：只删 Java 干净收尾写的 end（#528 第 15 项）
#else
    // 会话标记（sesdiag.c）：正常收尾删掉，下次启动不问「上次没正常退出」。游戏自己中止（Break）的改写成「游戏中止」：下次启动问发诊断
    //   （#351：PRAGMATA 缺 Switch 2 音频服务命令、游戏 Abort，原先这里照删，玩家看到闪退却拿不到诊断包），但不当成被杀，不关设置（diag.m）
    { int brk = why && !strcmp(why, "游戏 Break");
      void (*mk)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, brk ? "s2a_sesmark_abort_home" : "s2a_sesmark_clear_home"); if (mk) mk(); }
#endif
    LOG("优雅退出完成（%.3f 秒）", (now_ns() - t0) / 1e9);
    s2a_logcon_flush(); _exit(atomic_load(&g_exit_code));
}
void s2a_term_install(void) {
#if (defined(__ANDROID__) || defined(__OHOS__))
    int error = s2a_android_term_install();
    if (error) die("Android SIGTERM 收尾线程安装失败 errno=%d", error);
#else
    signal(SIGTERM, SIG_IGN);   // 交给 GCD 信号源在普通线程上收尾
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    dispatch_source_set_event_handler(src, ^{ s2a_host_shutdown("SIGTERM"); });
    dispatch_resume(src);
#endif
}
