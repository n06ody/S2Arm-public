// iOS App 壳：UIKit 占主线程（s2a_front_run 覆盖 host_main.c 的弱缺省）。窗口与根视图在这里建；Metal 层与触屏 / 手柄由帧率能耗线、服务接入线挂到 s2a_ios_root_view()。
#include "s2a_loadprog.h"   // 前台激活门
#include <dlfcn.h>
#import "s2a_l10n.h"
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <AVFAudio/AVFAudio.h>
#import "controller.h"
#include "input_config.h"
#include "root_ready.h"
#import "vpad.h"
#import "save_transfer.h"
#import "app_settings.h"
#import "play_layout.h"
#import "notice.h"
#include "s2a_session.h"
#include "s2a_dguard.h"
#include "s2a_init_fail.h"   // 初启失败页的标题 / 按钮文案（与 launchers/shared/experience.v1.json init_fail_page 逐字一致；中英按 App 语言取，S2APICK）
#include "s2a_ui_text.h"   // GPU 卡住暂停页文案（S2A_GPU_STUCK_*，与 macos/front_mac.m 同一套）
#include "s2a_direct_prompt.h"   // 「切换游戏」确认框文案（规格 prompts，S2APICK）
#include "s2a_selfcheck_page.h"   // 起局自检提示页文案：C 层（selfcheck.c）递中文原宏，这里按 App 语言换成 <宏>_EN（host_text）
void s2a_loadpage_attach(UIView *root); void s2a_loadpage_front(void);   // loadpage.m：首次加载页 / 简短启动画面
#import <Metal/Metal.h>   // NSProcessPerformanceProfileSustained 定义在 Metal 的 MTLDeviceCertification.h（心跳打持续执行档用）
static _Atomic double g_brightness = -1;   // 屏幕亮度：主线程建场景 / 心跳时更新，渲染侧（hle_nvn.c 每 300 帧那行）只读缓存，不在别的线程碰 UIKit
void s2a_brightness_hold_log(void);   // 亮度保持（S2A_BRIGHTNESS）：心跳行带出整局实际亮度最小 / 最大值
#include "native.h"
#include "s2a_pad.h"
#include "s2a_boot.h"
#include "s2a_launch_ctx.h"
#include <sys/stat.h>   // 程序包清单修改时间（s2a_shell_game_sign_state）
#include <sys/mount.h>   // statfs（free_note）
#include "s2a_docs.h"   // s2a_container_docs：游戏库按款核资源包的进程级 Documents
#include "s2a_sesdiag.h"   // 会话标记：上一局是不是死在前台（启动前崩溃询问）
#include "s2a_unattended.h"   // 自动化局不弹崩溃询问
#include "settings.h"   // 浮层开关要在建视图 / Metal 设备之前先落环境变量（#153）
// 游戏库界面（shell 的 S2ArmShellHost，C 边界同 Sources/S2ArmShellBridge/include/s2a_shell.h）：make ios S2A_SHELLUI=1 才链进来，
//   缺省壳不带、为空指针（多款容器包配了不带库界面的壳 = 报红提示，不起游戏）。这里本地声明、不 include 那个头：缺省出壳不读 frontend-ios（壳输入闭包）
// 弱链接[ios]：s2a_shell_library_vc
// 弱链接[ios]：s2a_shell_set_play_handler
static void img_scan_if_library(void);   // 下面游戏库段：回前台时认领「文件」里新拷进来的 game.img
void *s2a_shell_library_vc(const char *catalogue_json_path) __attribute__((weak_import));
void s2a_shell_set_play_handler(void (*fn)(const char *game_id, void *ctx), void *ctx) __attribute__((weak_import));
// 弱链接[ios]：s2a_shell_refresh
// 弱链接[ios]：s2a_shell_test_cloud_picker
void s2a_shell_refresh(void) __attribute__((weak_import));
// 弱链接[ios]：s2a_shell_cloud_sync_vc
void *s2a_shell_cloud_sync_vc(const char *game_id, void (*done)(void *ctx), void *ctx) __attribute__((weak_import));
// 弱链接[ios]：s2a_shell_cloud_upload_background
void s2a_shell_cloud_upload_background(const char *game_id) __attribute__((weak_import));
void s2a_boot_rebase(const char *why);   // boot.c：起局计时起点改为此刻（多款容器点 Play；不进 native.h——它在抽取阶段键里，改它冷全库）
const char *s2a_host_pre_ui_checks(void);   // host_main.c：起 UIKit 前读游戏相关值的检查（多款容器在选游戏之后调）
#include <stdatomic.h>
#include <pthread.h>
// UIKit lifecycle events publish on the main thread, independently of the
// background-only GPU/guest pause policy. now_ns is also the NVN clock.
static _Atomic int display_focused;
static _Atomic uint64_t display_focus_since;
int dm_focus_now(void) { return atomic_load(&display_focused); }
uint64_t dm_focus_in_since(void) { return atomic_load(&display_focus_since); }
static void display_focus_publish(BOOL active) {
    if (active && !atomic_load(&display_focused)) atomic_store(&display_focus_since, now_ns());
    atomic_store(&display_focused, active);
}
static _Atomic int g_front_running;
static void s2a_ios_suspend(int on, const char *why);
static void s2a_ios_suspend_enqueue(int on, const char *why);
// 重启完成（runtime/src/session.c s2a_session_on_restart_done，启动时登记一次）：不管设置页 / 游戏 ExecuteProgram / 测试开关谁发起，
//   新局起来后都在这里放「重启首帧屏障」（s2a_session_guest_start_prepare 立的客体冻结 + GPU hold）。屏障防的是重建期间 App 进了后台、
//   新局在后台提交 GPU：所以只在 UIKit 确认前台活跃时放；不活跃就留给「回前台 / 获得焦点」那次 s2a_ios_suspend(0) 放
static void snap_result_check(void);
static void restart_done(unsigned generation) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ snap_result_check(); });   // 快照恢复中途失败后重新开局：告诉玩家
    dispatch_async(dispatch_get_main_queue(), ^{
        if (UIApplication.sharedApplication.applicationState == UIApplicationStateActive) s2a_ios_suspend_enqueue(0, "重启完成");
        else LOG("iOS 壳：会话 %u 重启完成，App 不在前台活跃，屏障留到回前台放", generation);
    });
}
// 快照恢复的结果（引擎每次恢复尝试都重写文档根下 snap_result.tsv：结果 / 分类 / 给玩家 / 时刻，runtime/src/snapshot.c s2a_snap_result_note）：
//   结果不是「恢复成功」也不是「需要确认」（那一类 vpad / 游戏库已当场问过）就把「给玩家」那句弹出来，弹完删文件不重复弹。
//   本局文档根找不到（游戏库里，还没起局）就看上一局那款的文档根（S2ALastGameId → <容器>/games/<号>/）
static void snap_result_check(void) {
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    char p[2048]; if (s2a_docs_path(p, sizeof p, "snap_result.tsv") > 0) [paths addObject:@(p)];
    NSString *last = [NSUserDefaults.standardUserDefaults stringForKey:@"S2ALastGameId"]; char d[1024];
    if (last.length && ![last containsString:@"/"] && s2a_container_docs(d, sizeof d) > 0) [paths addObject:[NSString stringWithFormat:@"%s/games/%@/snap_result.tsv", d, last]];
    for (NSString *f in paths) {
        NSString *text = [NSString stringWithContentsOfFile:f encoding:NSUTF8StringEncoding error:nil]; if (!text) continue;
        NSMutableDictionary<NSString *, NSString *> *kv = [NSMutableDictionary dictionary];
        for (NSString *line in [text componentsSeparatedByString:@"\n"]) { NSArray *c = [line componentsSeparatedByString:@"\t"]; if (c.count >= 2) kv[c[0]] = c[1]; }
        [NSFileManager.defaultManager removeItemAtPath:f error:nil];
        NSString *result = kv[@"结果"], *say = kv[@"给玩家"];
        if (!result.length || [result isEqualToString:@"恢复成功"] || [result isEqualToString:@"需要确认"]) return;
        LOG("快照：恢复结果「%s」（%s），告诉玩家", result.UTF8String, kv[@"分类"].UTF8String ?: "");
        UIAlertController *a = [UIAlertController alertControllerWithTitle:S2AL(@"snap.result.title") message:say.length && s2a_l10n_zh() ? say : S2AL(@"snap.result.generic") preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:S2AL(@"common.ok") style:UIAlertActionStyleDefault handler:nil]];
        UIView *s2a_ios_root_view(void); UIViewController *top = s2a_ios_root_view().window.rootViewController; while (top.presentedViewController) top = top.presentedViewController;
        [top presentViewController:a animated:YES completion:nil];
        return;
    }
}
// 设置页「立刻重启」的请求回调：只回设置页结果；屏障由 restart_done 放（控制线程先调它，两者都派主线程，先放屏障后回设置页）
static void settings_restart_result(int result, const char *detail, void *context) {
    void (^completion)(NSString *) = CFBridgingRelease(context);
    // detail 是 session.c 的内部原因（已进 host.log）：中文 App 照旧显示原文；英文 App 显示通用一句，内部原因不翻（#316）
    NSString *error = result ? (detail && s2a_l10n_zh() ? [NSString stringWithUTF8String:detail] : S2AL(@"app.restart.failed")) : nil;
    dispatch_async(dispatch_get_main_queue(), ^{ completion(error); });
}
// 切换游戏（多款容器）：一个进程只跑一款，换游戏 = 照退出流程收尾（等存档写完）、结束进程，再点开 App 就是游戏库（willConnect 容器分支）。
//   确认框同游戏内「存档管理」的退出确认（save_transfer.m s2a_ios_save_present）：先提醒在游戏里存档；收尾放后台队列，主线程不等
static void switch_game_present(UIViewController *presenter) {
    if (!presenter) return;
    presenter = s2a_top_presenter(presenter);
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:S2APICK(S2A_DPROMPT_SWITCH_GAME_TITLE) message:S2APICK(S2A_DPROMPT_SWITCH_GAME_BODY)
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:S2APICK(S2A_DPROMPT_HOME_QUIT_DECLINE) style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:S2APICK(S2A_DPROMPT_SWITCH_GAME_ACCEPT) style:UIAlertActionStyleDestructive handler:^(UIAlertAction *item) {
        (void)item; LOG("切换游戏：玩家确认，收尾退出，下次打开回到游戏库");
        void s2a_host_shutdown(const char *why);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ s2a_host_shutdown("切换游戏"); });
    }]];
    [presenter presentViewController:alert animated:YES completion:nil];
}
static void settings_attach_runtime(void) {
    S2ASettingsRuntime *runtime = [S2ASettingsRuntime new];
    runtime.manageSaves = ^(UIViewController *presenter) { s2a_ios_save_present(presenter); };
    if (s2a_launch_ctx_container()) runtime.switchGame = ^(UIViewController *presenter) { switch_game_present(presenter); };   // 单款包没有游戏库，不给
    runtime.canRestart = ^BOOL { return s2a_host_session_state() == S2A_SESSION_RUNNING; };
    runtime.restart = ^(void (^completion)(NSString *)) {
        void *context = (void *)CFBridgingRetain([completion copy]);
        int result = s2a_host_restart(settings_restart_result, context);
        if (!result) { s2a_controller_set_foreground(0); return; }
        void (^rejected)(NSString *) = CFBridgingRelease(context);
        NSString *error = result == -3 ? S2AL(@"app.restart.busy") : result == -2 ? S2AL(@"app.restart.unsupported") : S2AL(@"app.restart.noSession");
        rejected(error);
    };
    s2a_settings_set_runtime(runtime);
    // 测试开关 S2A_TEST_RESTART_AFTER=<秒>（11-04 阶段 0 验收「进程内重启」，真机局无人点设置页）：到点代按设置页「立刻重启」，走同一个 runtime.restart；
    //   进程内只触发一次（静态标记跨进程内重启保留，不会重启后再触发）。结果写 host.log，判据见 Windows出包线状态文件
    { static int armed; const char *e = getenv("S2A_TEST_RESTART_AFTER"); double sec = e ? atof(e) : 0;
      if (sec > 0 && !armed) { armed = 1; LOG("设置页：测试开关 S2A_TEST_RESTART_AFTER=%s，%.0f 秒后代按「立刻重启」", e, sec);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(sec * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            if (!runtime.canRestart()) { LOG("【红】测试重启：当前不能重启（会话不在运行）"); return; }
            LOG("测试重启：代按「立刻重启」");
            runtime.restart(^(NSString *err) { if (err) LOG("【红】测试重启失败：%s", err.UTF8String); else LOG("测试重启：完成，新会话已起"); }); }); } }
}
static void *deferred_host_thread(void *arg) {
    s2a_boot_entry entry = (s2a_boot_entry)arg;
    @autoreleasepool { entry(); }
    return NULL;
}
void s2a_host_shutdown(const char *why);
#include <mach/mach.h>
uint64_t s2a_ap_frames(void);   // ap_host.c：上屏帧数
void s2a_guest_run_mark(int running);   // vawin.c：「本局在跑」标记（前台写、后台 / 将终止删；下一局见标记还在 = 上局在前台死掉，强制开客体内存文件后备）
static unsigned long long s2a_footprint_mb(void) { task_vm_info_data_t v; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&v, &n) == KERN_SUCCESS ? v.phys_footprint >> 20 : 0; }
// 客体内存分区账（S2A_MEM_SPLIT=1，缺省关；11-05 0400486024DEC000读档进场景被系统杀）：
//   堆区当前段、别名区（MapPhysicalMemory）各扫一次常驻 / 压缩，窗口其余（代码、栈、线程局部、共享 / 传输内存等）= 窗口合计 − 两者，窗口外 ≈ 我方。
//   kinds：再带我方细分（图形层 nvn_be_mem_kinds：驻留集里的私有贴图 / 私有缓冲（上传暂存块为主）/ 游戏池零拷贝缓冲虚拟长度与其中常驻页 / 其它共享缓冲），
//   以及实占的内核分项（task_vm_info：匿名 internal、已压缩、图形账 graphics）。注意 mincore「常驻」≠ 计进实占：M5 Pro 实测把没写过的 1 GB
//   零拷贝缓冲放进驻留集，全机 wired +1024 MB、mincore 常驻 1024 MB，phys_footprint 不变（报告 41）；所以「窗口外 ≈ 实占 − 窗口合计」在池被钉住时偏小，看内核分项
static int mem_split_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("S2A_MEM_SPLIT"); on = e && *e && strcmp(e, "0"); } return on; }
static void mem_split_line(const char *tag, unsigned long long fp, uint64_t wr, uint64_t wo, int kinds) {
    uint64_t hl, hn, al, an, ph, hr, ho, ar, ao; s2a_mem_guest_split(&hl, &hn, &al, &an, &ph); s2a_range_resident(hl, hn, &hr, &ho); s2a_range_resident(al, an, &ar, &ao);
    uint64_t rest_r = wr > hr + ar ? wr - hr - ar : 0, rest_o = wo > ho + ao ? wo - ho - ao : 0;
    char k[640] = "";
    void nvn_be_mem_kinds(uint64_t *, uint64_t *, uint64_t *, uint64_t *, uint64_t *, uint64_t *) __attribute__((weak_import));   // 直连图形层（通用模式没有这个符号）
    if (kinds && nvn_be_mem_kinds) { uint64_t pt, pb, pl, pc, so, no; nvn_be_mem_kinds(&pt, &pb, &pl, &pc, &so, &no);
        snprintf(k, sizeof k, "｜我方细分（驻留集 %llu 个对象）：私有贴图 %llu MB、私有缓冲（上传暂存块为主）%llu MB、其它共享缓冲 %llu MB、游戏池零拷贝缓冲 %llu MB（其中常驻 / 压缩 %llu MB，含被驻留集钉住的未写页）",
                 (unsigned long long)no, (unsigned long long)(pt >> 20), (unsigned long long)(pb >> 20), (unsigned long long)(so >> 20), (unsigned long long)(pl >> 20), (unsigned long long)(pc >> 20)); }
    if (kinds) { task_vm_info_data_t v; mach_msg_type_number_t n = TASK_VM_INFO_COUNT; size_t o = strlen(k);
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&v, &n) == KERN_SUCCESS && o < sizeof k)
            snprintf(k + o, sizeof k - o, "｜内核分项：匿名 %llu MB、已压缩 %llu MB、图形账 %llu MB（其中压缩 %llu MB）", (unsigned long long)(v.internal >> 20), (unsigned long long)(v.compressed >> 20),
                     (unsigned long long)(v.ledger_tag_graphics_footprint >> 20), (unsigned long long)(v.ledger_tag_graphics_footprint_compressed >> 20)); }
    LOG("[%s] 帧 %llu 实占 %llu MB｜堆 已设 %llu MB：常驻 %llu MB、压缩 %llu MB｜物理映射区 已映射 %llu MB：常驻 %llu MB、压缩 %llu MB｜窗口其余 常驻 %llu MB、压缩 %llu MB｜窗口外（我方）≈ 实占 − 窗口合计 = %lld MB%s",
        tag, (unsigned long long)s2a_ap_frames(), fp, (unsigned long long)(hn >> 20), (unsigned long long)(hr >> 20), (unsigned long long)(ho >> 20), (unsigned long long)(ph >> 20), (unsigned long long)(ar >> 20), (unsigned long long)(ao >> 20),
        (unsigned long long)(rest_r >> 20), (unsigned long long)(rest_o >> 20), (long long)fp - (long long)((wr + wo) >> 20), k);
}
// 跳变采样（11-05 第二十二版读档局：约 4 秒内实占涨 3.5 GB 到 8192 MB 被杀，10 秒一次的心跳看不到构成）：后台线程每 100 ms 取一次实占，
//   比上次打印涨 ≥ 256 MB 就当场打一行分区账 + 我方细分（降回 256 MB 以下时只把基线跟下来，不打）。日志是逐行 write 进 host.log，
//   进程被系统 SIGKILL 时已 write 的行在内核页缓存里不丢，所以被杀前最后一跳的那行在。打一行的扫描（整窗 mincore + 驻留集分类）约毫秒级，只在跳变时做
enum { MEM_STEP_MB = 256, MEM_STEP_MS = 100 };
static void *mem_split_thread(void *arg) {
    (void)arg; pthread_setname_np("s2a-memstep");
    unsigned long long last = s2a_footprint_mb();
    for (;;) {
        struct timespec ts = {0, MEM_STEP_MS * 1000000L}; nanosleep(&ts, NULL);
        unsigned long long fp = s2a_footprint_mb(); if (!fp) continue;
        if (fp + MEM_STEP_MB <= last) { last = fp; continue; }
        if (fp < last + MEM_STEP_MB) continue;
        uint64_t wr, wo; s2a_window_resident(&wr, &wo);
        char tag[64]; snprintf(tag, sizeof tag, "内存跳变 +%llu MB", fp - last);
        mem_split_line(tag, fp, wr, wo, 1); last = fp;
    }
    return NULL;
}
static void mem_split_start(void) {
    static atomic_int started; if (atomic_exchange(&started, 1)) return;
    pthread_t th; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &a, mem_split_thread, NULL)) LOG("【红】 内存跳变采样线程起不来，只有 10 秒一次的心跳分区账");
    else LOG("内存跳变采样：每 %d ms 取一次实占，比上次打印涨 ≥ %d MB 就打一行「[内存跳变 …]」（S2A_MEM_SPLIT）", MEM_STEP_MS, MEM_STEP_MB);
    pthread_attr_destroy(&a);
}
static UIView *g_root;
static _Atomic int g_render_failed;
static _Atomic int g_gpu_stuck_page;   // GPU 卡住暂停页在屏上（s2a_ios_gpu_stuck）：回前台不解冻游戏线程，等玩家在页上选
static _Atomic int g_gpu_stopped;   // GPU 挂死安全停页在屏上（s2a_ios_gpu_stopped 置位）：停帧看门的提示不盖掉它
void s2a_guest_freeze(int on);                   // thread.c
void s2a_host_focus_set(int focused);            // os.c（接口 v2.15）
void s2a_host_sleep_set(int sleeping);           // os.c（接口 v2.15）
void nxgal_gpu_hold(int on, const char *why);    // gal_core.m
static _Atomic int g_bg;              // 进过后台、还没恢复
static _Atomic uint64_t g_fg_note;   // 最近一次「将回前台」通知的时刻（恢复时间线起点）
void s2a_resume_timeline(uint64_t t_note, uint64_t t_ds, uint64_t t_gpu, uint64_t t_thr, uint64_t t_done);   // ap_host.c
static void s2a_direct_gate(int on) {
    static void (*gate)(int); static int got;
    if (!got) { gate = (void (*)(int))dlsym(RTLD_DEFAULT, "s2a_direct_suspend_gate"); got = 1; }
    if (gate) gate(on);
}
static void s2a_direct_drain_or_resume(int on) {
    static void (*suspend)(int); static int got;
    if (!got) { suspend = (void (*)(int))dlsym(RTLD_DEFAULT, "s2a_direct_suspend"); got = 1; }
    if (suspend) suspend(on);
}
static void s2a_ios_suspend(int on, const char *why) {
    int transition = s2a_session_background_begin(on);
    if (transition == S2A_SESSION_LIFECYCLE_DEFERRED) {
        s2a_controller_set_foreground(0);
        if (on) atomic_store(&g_bg, 1);
        else if (!atomic_load(&g_render_failed) && s2a_host_session_state() == S2A_SESSION_RUNNING) {
            uint64_t t_ds, t_gpu, t_thr, t_done;
            if (s2a_session_guest_start_complete(&t_ds, &t_gpu, &t_thr, &t_done)) {
                s2a_controller_set_foreground(1);
                if (atomic_exchange(&g_bg, 0)) {
                    uint64_t t0 = atomic_load(&g_fg_note); if (!t0 || t0 > t_ds) t0 = t_ds;
                    s2a_resume_timeline(t0, t_ds, t_gpu, t_thr, t_done);
                }
                LOG("iOS 壳：重启期间进入后台后回前台 → 新会话已恢复");
            }
        }
        return;
    }
    if (transition == S2A_SESSION_LIFECYCLE_TERMINAL) {
        s2a_controller_set_foreground(0); return;
    }
    if (transition == S2A_SESSION_LIFECYCLE_BUSY) {
        // UIKit lifecycle notifications are main-thread serialized; retry only
        // if a future caller reaches here from a concurrent lifecycle source.
        s2a_controller_set_foreground(0);
        dispatch_async(dispatch_get_main_queue(), ^{
            UIApplicationState state = UIApplication.sharedApplication.applicationState;
            if ((on && state == UIApplicationStateBackground) || (!on && state != UIApplicationStateBackground))
                s2a_ios_suspend_enqueue(on, why);
        });
        return;
    }
    // A failed render startup is terminal for this session. Foregrounding must
    // not restart the guest behind the error page.
    if (atomic_load(&g_render_failed)) { s2a_controller_set_foreground(0); s2a_guest_freeze(1); s2a_session_background_end(); return; }
    int stuck = atomic_load(&g_gpu_stuck_page);   // GPU 卡住暂停页在屏上：回前台照常恢复图形与时钟，但游戏线程留在冻结、手柄不接，等玩家在页上选
    s2a_controller_set_foreground(!on && !stuck);
    // 焦点报给服务层（接口 v2.15，缺口扫描族 1）：服务层把它转成游戏的焦点消息（失焦 / 回到前台）；进后台的消息在回前台解冻后游戏才读到
    s2a_host_focus_set(!on); s2a_host_sleep_set(on);   // iOS 后台同时算睡眠（psc 休眠 / 唤醒请求）
    // 直连模式进出后台（双轨 §12.2，s2a_direct_suspend 在直连图形层 graphics/nvn/host_adapt.m）：运行时取符号，没链直连层的构建取不到就不调；通用模式下它是空操作
    uint64_t t_ds = now_ns();
    s2a_direct_drain_or_resume(on);
    // 游戏时钟（#495）：进后台最先冻、回前台最后放——解冻瞬间先跑的线程看到的是停住的时钟，不会看到后台那段时间
    if (on) { guest_clock_freeze_for(1, 1); s2a_guest_freeze(1); nxgal_gpu_hold(1, why); atomic_store(&g_bg, 1); LOG("iOS 壳：%s → 游戏时钟与线程冻结、GPU 停止提交", why); }
    else {
        uint64_t t1 = now_ns(); nxgal_gpu_hold(0, why); uint64_t t2 = now_ns(); if (!stuck) s2a_guest_freeze(0); guest_clock_freeze_for(1, 0); uint64_t t3 = now_ns();
        s2a_clock_catchup();   // 游戏日历（#863）：后台那段（含锁屏睡眠）补进 Ryujinx 稳态时钟；计数器仍从冻结那一刻接着走（#495）
        if (atomic_exchange(&g_bg, 0)) {   // 从后台回来的第一次（回前台 / 获得焦点两条通知都会到这里）：打恢复时间线（ap_host.c）
            uint64_t t0 = atomic_load(&g_fg_note); if (!t0 || t0 > t_ds) t0 = t_ds;
            s2a_resume_timeline(t0, t_ds, t1, t2, t3);
        }
        LOG("iOS 壳：%s → 恢复", why);
    }
    s2a_session_background_end();
}
static dispatch_queue_t s2a_lifecycle_queue(void) {
    static dispatch_queue_t queue; static dispatch_once_t once;
    dispatch_once(&once, ^{ queue = dispatch_queue_create("s2a.ios-lifecycle", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0)); });
    return queue;
}
// UIKit 的 scene-update 回调只做两件有界操作：同步关图形入口、登记一段后台执行时间；
// 等旧提交 / PSO 编译退出、排空 GPU、冻结客体都在串行生命周期队列上完成。前台恢复排在其后，不会在 drain 中途重开 gate。
static void s2a_ios_suspend_enqueue(int on, const char *why) {
    if (!on) { dispatch_async(s2a_lifecycle_queue(), ^{ s2a_ios_suspend(0, why); }); return; }
    s2a_direct_gate(1);   // 不等 g_inflight_api；通知返回前已经挡住新提交
    UIApplication *app = UIApplication.sharedApplication;
    __block UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
    task = [app beginBackgroundTaskWithName:@"S2A graphics suspend" expirationHandler:^{
        UIBackgroundTaskIdentifier expired = task; task = UIBackgroundTaskInvalid;
        LOG("【红】iOS 后台图形排空超过系统给的后台执行时间");
        if (expired != UIBackgroundTaskInvalid) [app endBackgroundTask:expired];
    }];
    dispatch_async(s2a_lifecycle_queue(), ^{
        s2a_ios_suspend(1, why);
        dispatch_async(dispatch_get_main_queue(), ^{
            UIBackgroundTaskIdentifier finished = task; task = UIBackgroundTaskInvalid;
            if (finished != UIBackgroundTaskInvalid) [app endBackgroundTask:finished];
        });
    });
}
// 提示 / 出错页（notice.m 画；11-02 维护者：满屏、一句人话 + 下一步、和加载页同一套样子、技术细节进诊断包不上屏）。窗口还没建时只记下，建窗口时显示
static NSString *g_msg;   // 正在屏上（或待显示）的提示，s2a_ios_clear_message 按它认
static NSArray *g_note;   // 待显示 / 正显示的提示页参数：@[符号, 标题, 正文, 进度, 进度小字, 是否给「故障报修」, (可选) 它是否主按钮]
static NSArray *g_note_alt;   // 第二个按钮：@[按钮字, 点了调的块]（GPU 卡住页的「退出」；别的提示页清掉）
static NSArray *g_note_go;   // 提示页上的「仍要启动」这类继续按钮：@[按钮字, 点了调的块]（s2a_ios_ask_continue 设，别的提示页清掉）
static UIView *g_note_view;
static UIView *g_help; static NSArray *g_help_args; static void (*g_help_recheck)(void);   // 缺资源包说明页（见 s2a_ios_show_pack_help）
void s2a_diag_report(void);
static void note_build(void) {
    [g_note_view removeFromSuperview]; g_note_view = nil;
    if (!g_root || !g_note) return;
    NSMutableArray *bs = [NSMutableArray array];
    if (g_note_go) { void (^go)(void) = g_note_go[1]; [bs addObject:s2a_notice_button(g_note_go[0], YES, ^(UIButton *b) { b.enabled = NO; go(); })]; }
    if (g_note_alt) { void (^alt)(void) = g_note_alt[1]; [bs addObject:s2a_notice_button(g_note_alt[0], NO, ^(UIButton *b) { b.enabled = NO; alt(); })]; }
    // 「故障报修」（诊断一键上传，11-13：取代原「复制诊断」「导出诊断包」两个按钮；上传失败时弹窗里退回导出）。
    //   初启失败页（g_note[6] 为真）它是主按钮：那页的诊断文字 host_main.c 已写进 host.log，随诊断包一起传
    if ([g_note[5] boolValue]) { BOOL primary = g_note.count > 6 && [g_note[6] boolValue];
        [bs addObject:s2a_notice_button(S2AL(@"diag.report.title"), primary, ^(UIButton *b) { (void)b; s2a_diag_report(); })]; }
    g_note_view = s2a_notice_page(g_root.bounds, g_note[0], g_note[1], g_note[2], [g_note[3] floatValue], [g_note[4] length] ? g_note[4] : nil, bs);
    [g_root addSubview:g_note_view];
    { void s2a_report_gear_attach(UIView *); s2a_report_gear_attach(g_note_view); }   // 左上角小齿轮：故障报修
}
// C 层递来的规格文案（起局自检提示页 host_main.c → s2a_selfcheck_page、初启失败页正文 init_fail.c）是 runtime/include 里的中文原宏；
//   英文 App 按逐字相等换成同名 <宏>_EN（#316）。不在表里的原样显示
static const char *host_text(const char *s) {
    static const char *const k[][2] = {
        {S2A_SELFCHECK_ENT_TITLE, S2A_SELFCHECK_ENT_TITLE_EN}, {S2A_SELFCHECK_ENT_BODY, S2A_SELFCHECK_ENT_BODY_EN},
        {S2A_SELFCHECK_PKG_TITLE, S2A_SELFCHECK_PKG_TITLE_EN}, {S2A_SELFCHECK_PKG_BODY, S2A_SELFCHECK_PKG_BODY_EN},
        {S2A_SELFCHECK_DEVICE_TITLE, S2A_SELFCHECK_DEVICE_TITLE_EN}, {S2A_SELFCHECK_DEVICE_BODY, S2A_SELFCHECK_DEVICE_BODY_EN},
        {S2A_SELFCHECK_AS_TITLE, S2A_SELFCHECK_AS_TITLE_EN}, {S2A_SELFCHECK_AS32_BODY, S2A_SELFCHECK_AS32_BODY_EN},
        {S2A_SELFCHECK_MEM_BODY, S2A_SELFCHECK_MEM_BODY_EN}, {S2A_SELFCHECK_GO, S2A_SELFCHECK_GO_EN},
        {S2A_INIT_FAIL_MSG_SERVICE, S2A_INIT_FAIL_MSG_SERVICE_EN}, {S2A_INIT_FAIL_MSG_RUNTIME, S2A_INIT_FAIL_MSG_RUNTIME_EN},
        {S2A_INIT_FAIL_MSG_GAME, S2A_INIT_FAIL_MSG_GAME_EN}, {S2A_INIT_FAIL_MSG_GENERIC, S2A_INIT_FAIL_MSG_GENERIC_EN},
        {S2A_INIT_FAIL_MSG_FILES, S2A_INIT_FAIL_MSG_FILES_EN}, {S2A_INIT_FAIL_NEXT, S2A_INIT_FAIL_NEXT_EN},
        {S2A_INIT_FAIL_MSG_DEVICE, S2A_INIT_FAIL_MSG_DEVICE_EN},   // #446 设备地址空间 / 内存不够
        {S2A_INIT_FAIL_MSG_HEAP, S2A_INIT_FAIL_MSG_HEAP_EN},       // D-NGH9 普通堆不够、已记下，再点一次开始
        {S2A_INIT_FAIL_MSG_EXEC, S2A_INIT_FAIL_MSG_EXEC_EN},       // #63639b 外置镜像不许可执行映射（LiveContainer 装法）
        {S2A_INIT_FAIL_MSG_SHADERS, S2A_INIT_FAIL_MSG_SHADERS_EN},   // direct_lib_missing：电脑装 / 自签导入两条路各说一句
    };
    if (!s || s2a_l10n_zh()) return s;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (!strcmp(s, k[i][0])) return k[i][1];
    return s;
}
// 提示页正显示着（diag.m 问「把上次的诊断发给我们？」前看：等它收起再问，不叠在一起）。主线程调
int s2a_ios_note_up(void) { return g_note_view.window != nil; }
// 反过来：那个询问正开着就等它收起再出提示页（每半秒看一次）。diag.m 不在输入工作台工程里时弱引用为空，直接出
int s2a_diag_asking(void) __attribute__((weak_import));
static void after_diag_prompt(void (^show)(void)) {
    if (s2a_diag_asking && s2a_diag_asking()) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{ after_diag_prompt(show); }); return; }
    show();
}
// 可从任意线程调（导入资源的后台线程用它刷进度）：主线程上直接改，别的线程派回主线程。done / total：total > 0 时画进度条；diag：给「导出诊断包」按钮
void s2a_ios_show_notice(const char *symbol, const char *title, const char *body, int done, int total, int diag) {
    title = host_text(title); body = host_text(body);
    NSString *sy = @(symbol), *t = @(title), *b = @(body ?: ""), *key = [NSString stringWithFormat:@"%@\n\n%@", t, b];
    NSArray *n = @[sy, t, b, @(total > 0 ? (float)done / total : -1.0f), total > 0 ? [NSString stringWithFormat:S2AL(@"notice.batch"), done < total ? done + 1 : total, total] : @"", @(diag != 0)];
    void (^show)(void) = ^{ g_msg = key; g_note = n; g_note_go = nil; g_note_alt = nil; [g_help removeFromSuperview]; g_help = nil; g_help_args = nil; note_build(); };
    if (diag) { void (^orig)(void) = show; show = ^{ after_diag_prompt(orig); }; }   // 带「发送诊断」的出错提示页（起局自检等）：询问开着就等它收起
    if (NSThread.isMainThread) show(); else dispatch_async(dispatch_get_main_queue(), show);
}
// 带「继续」按钮的提示页，调用线程等玩家点（v0.3.1：起局自检只差内存上限时「仍要启动」）。只在宿主工作线程上用（游戏库已在主线程跑 UIKit）：
//   主线程上等就把界面卡死了，返回 0（调用方照旧只给提示）。玩家点了 → 收起提示页、返回 1；不点就一直停在提示页（同原先）
int s2a_ios_ask_continue(const char *symbol, const char *title, const char *body, const char *go) {
    if (NSThread.isMainThread) return 0;
    title = host_text(title); body = host_text(body); go = host_text(go);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    NSString *sy = @(symbol), *t = @(title), *b = @(body ?: ""), *label = @(go), *key = [NSString stringWithFormat:@"%@\n\n%@", t, b];
    NSArray *n = @[sy, t, b, @(-1.0f), @"", @YES];
    dispatch_async(dispatch_get_main_queue(), ^{ after_diag_prompt(^{
        g_msg = key; g_note = n; g_note_alt = nil; [g_help removeFromSuperview]; g_help = nil; g_help_args = nil;
        g_note_go = @[label, ^{ g_note_go = nil; g_note = nil; g_msg = nil; [g_note_view removeFromSuperview]; g_note_view = nil; dispatch_semaphore_signal(done); }];
        note_build();
    }); });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    return 1;
}
// 老入口（停帧看门 ap_host.c 经 dlsym 调）：第一段当标题、其余当正文
// 可用空间按「重要用途」口径（含系统随时能清掉的空间，同「设置 → 储存空间」里的「可用」）：statfs 的 f_bavail 不含这部分，常小好几 GB，
//   拿它判「空间不够」会冤枉玩家（runtime/src/vawin.c 客体内存文件后备经 dlsym 调）。取不到回 0
uint64_t s2a_ios_free_important(const char *path) {
#if TARGET_OS_TV   // 电视没有「重要用途」口径（tvOS 不给这两个键），按 statfs 可用
    struct statfs sf; return statfs(path, &sf) ? 0 : (uint64_t)sf.f_bavail * sf.f_bsize;
#else
    NSNumber *n = nil;
    [[NSURL fileURLWithPath:@(path)] getResourceValue:&n forKey:NSURLVolumeAvailableCapacityForImportantUsageKey error:nil];
    return n.longLongValue > 0 ? (uint64_t)n.longLongValue : 0;
#endif
}
// 手机可用空间记给电脑端（v0.3.4，玩家反馈：「设置」里剩 70 多 GB，一键安装说只剩 2 GB）：电脑端经 AFC / lockdown 读到的空闲不含系统随时能清掉的空间，
//   只有手机上的 App 读得到「重要用途」口径。每次启动在后台读一次写进 Documents/s2a_free.tsv，一行：重要用途 \t 可选用途 \t statfs 空闲 \t 写入时刻（Unix 秒），字节数；
//   电脑端推包前读它（tools/push_pack.py free_from_phone）。读不到的项写 0
static void free_note(void) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        char p[1024], d[1000]; s2a_container_docs(d, sizeof d); snprintf(p, sizeof p, "%s/s2a_free.tsv", d);   // 固定在容器 Documents 下（多款容器也是这一份：可用空间是整机的）
        NSNumber *imp = nil, *opp = nil;
#if !TARGET_OS_TV   // 电视上两项都写 0（电脑端按 statfs 那一列）
        NSURL *u = [NSURL fileURLWithPath:NSHomeDirectory()];
        [u getResourceValue:&imp forKey:NSURLVolumeAvailableCapacityForImportantUsageKey error:nil];
        [u getResourceValue:&opp forKey:NSURLVolumeAvailableCapacityForOpportunisticUsageKey error:nil];
#endif
        struct statfs sf; unsigned long long av = statfs(NSHomeDirectory().fileSystemRepresentation, &sf) ? 0 : (unsigned long long)sf.f_bavail * sf.f_bsize;
        char t[1100]; snprintf(t, sizeof t, "%s.tmp", p); FILE *f = fopen(t, "w"); if (!f) return;
        fprintf(f, "%lld\t%lld\t%llu\t%ld\n", imp.longLongValue > 0 ? imp.longLongValue : 0, opp.longLongValue > 0 ? opp.longLongValue : 0, av, (long)time(NULL)); fclose(f);
        if (rename(t, p)) unlink(t);
        LOG("可用空间（记给电脑端 s2a_free.tsv）：重要用途 %lld MB、可选用途 %lld MB、statfs 空闲 %llu MB", imp.longLongValue >> 20, opp.longLongValue >> 20, av >> 20);
    });
}
void s2a_ios_show_message(const char *m) {
    if (atomic_load(&g_gpu_stopped) || atomic_load(&g_gpu_stuck_page)) return;   // GPU 安全停页 / 卡住暂停页在屏上：不盖掉（卡住页冻了游戏，停帧看门照样会到点）
    const char *nl = strchr(m, '\n'); char t[512]; size_t k = nl ? (size_t)(nl - m) : strlen(m); if (k >= sizeof t) k = sizeof t - 1;
    memcpy(t, m, k); t[k] = 0; const char *b = nl ? nl : ""; while (*b == '\n') b++;
    s2a_ios_show_notice("info.circle.fill", t, b, 0, 0, 0);
}
// 收掉 s2a_ios_show_message 给的那句话（任意线程）：只在屏上还是同一句时收（停帧看门帧又走了时调，审计 P1-4；别的提示不动）
void s2a_ios_clear_message(const char *m) {
    if (atomic_load(&g_gpu_stopped)) return;
    NSString *s = @(m);
    void (^clear)(void) = ^{
        NSString *cur = [g_msg stringByReplacingOccurrencesOfString:@"\n\n" withString:@""], *want = [s stringByReplacingOccurrencesOfString:@"\n" withString:@""];
        if (![cur isEqualToString:want]) return;
        g_msg = nil; g_note = nil; [g_note_view removeFromSuperview]; g_note_view = nil;
    };
    if (NSThread.isMainThread) clear(); else dispatch_async(dispatch_get_main_queue(), clear);
}
// 缺资源包 / 资源包不全的说明页（pack_apply.c 闸门与游戏 Break 时调，任意线程）：不起游戏、不退出。示意（Mac → 线 → iPhone）+ 三步 + 「我已放好，重新检查」。
//   10-26 维护者：「壳上做好说明别闪退，打开发现缺资源包就告诉用户把启动器导出的资源包导到手机上指定位置」。窗口还没建时记下，建窗口时显示
// 「重新检查」按下后一直禁用到宿主核完：就绪收起整页，没就绪重画整页（新按钮可点、正文开头说「刚刚查过，还是没有…」，pack_apply.c s2a_pack_help_detail）。
//   不用定时恢复：原先 0.6 秒后就恢复可点，宿主还没核完、页面原样，玩家以为按钮没生效（审计 P2-2）
static void help_build(NSString *title, NSString *detail, NSString *app) {
    [g_help removeFromSuperview]; g_help = nil;
    if (!g_root) return;
    UIButton *btn = s2a_notice_button(S2AL(@"app.packHelp.recheckPlaced"), YES, ^(UIButton *b) { b.enabled = NO; [b setTitle:S2AL(@"app.packHelp.checking") forState:UIControlStateDisabled];
        void (*f)(void) = g_help_recheck; if (f) f(); });
    g_help = s2a_notice_help_page(g_root.bounds, title, detail, app, btn); [g_root addSubview:g_help];
    { void s2a_report_gear_attach(UIView *); s2a_report_gear_attach(g_help); }   // 左上角小齿轮：故障报修
    // 测试开关（真机验收没法点屏）：S2A_PACK_RECHECK_AUTO=<秒> 每隔这么久代按一次「重新检查」，说明页收起即停
    { static int armed; const char *e = getenv("S2A_PACK_RECHECK_AUTO"); double sec = e ? atof(e) : 0;
      if (sec > 0 && !armed) { armed = 1; LOG("说明页：测试开关 S2A_PACK_RECHECK_AUTO=%s，每 %.0f 秒代按「重新检查」", e, sec);
          [NSTimer scheduledTimerWithTimeInterval:sec repeats:YES block:^(NSTimer *tm) {
              if (!g_help || !g_help_recheck) { [tm invalidate]; armed = 0; return; }
              LOG("说明页：代按「重新检查」（测试开关）"); g_help_recheck(); }]; } }
}
void s2a_ios_show_pack_help(const char *title, const char *detail, const char *app, void (*recheck)(void)) {
    NSString *t = [NSString stringWithUTF8String:title], *d = [NSString stringWithUTF8String:detail], *a = [NSString stringWithUTF8String:app];
    void (^show)(void) = ^{ g_help_recheck = recheck; g_help_args = @[t, d, a]; [g_note_view removeFromSuperview]; g_note_view = nil; g_note = nil; g_msg = nil; help_build(t, d, a); };
    if (NSThread.isMainThread) show(); else dispatch_async(dispatch_get_main_queue(), show);
}
void s2a_ios_hide_pack_help(void) {
    dispatch_async(dispatch_get_main_queue(), ^{ g_help_args = nil; g_help_recheck = NULL; [g_help removeFromSuperview]; g_help = nil; });
}
// 收掉提示页（任意线程）：资源导入就绪、当场接着起游戏时调（pack_apply.c gate_thread）。导入进度 / 「还在传」那几页是提示页不是说明页，
//   只收说明页的话提示页留在出图层上面盖住游戏（提示页 zPosition 2000，出图层在最底层）
void s2a_ios_hide_notice(void) {
    dispatch_async(dispatch_get_main_queue(), ^{ g_msg = nil; g_note = nil; [g_note_view removeFromSuperview]; g_note_view = nil; });
}
// Called on the render thread only for attach failure, before GPU emulation.
// Leave UIKit and diagnostic export alive instead of throwing across NativeAOT.
// 直连安全网提示页（host_main.c 起游戏主线程前、本款直连已连续失败满 3 次时调，宿主线程）：同一套提示页样子（notice.m），一句话 + 「导出诊断包」「仍然启动」。
//   诊断信息（失败记录）进 host.log、随诊断包带走，不上屏（11-02 维护者：内部词不上屏）。阻塞到玩家点「仍然启动」才返回；宿主在主线程上时也一样等（审计 P2-16），见 s2a_ios_wait_ui_signal
@interface S2ADguardTarget : NSObject { @public dispatch_semaphore_t go; UIView *page; BOOL pending, device; } @end
@implementation S2ADguardTarget
@end
static S2ADguardTarget *g_dguard_target;
// 提示页（主线程，g_root 已建）
static void dguard_build(S2ADguardTarget *t) {
    __weak S2ADguardTarget *w = t;
    UIButton *diag = s2a_notice_button(S2APICK(S2A_DGUARD_DIAG), NO, ^(UIButton *b) { (void)b; s2a_diag_report(); });
    UIButton *go = s2a_notice_button(S2APICK(S2A_DGUARD_GO), YES, ^(UIButton *b) { (void)b; S2ADguardTarget *x = w; if (!x) return; [x->page removeFromSuperview]; x->page = nil; dispatch_semaphore_signal(x->go); });
    UIView *page = t->device   // 上一局已知不是直连（这台设备给不了这款要的内存）：换说明，「仍然启动」照常
        ? s2a_notice_page(g_root.bounds, @"memorychip", S2APICK(S2A_DGUARD_DEVICE_TITLE), S2APICK(S2A_DGUARD_DEVICE_MSG), -1, nil, @[diag, go])
        : s2a_notice_page(g_root.bounds, @"exclamationmark.triangle.fill", S2APICK(S2A_DGUARD_TITLE), S2APICK(S2A_DGUARD_MSG), -1, nil, @[diag, go]);
    t->page = page; [g_root addSubview:page]; [g_root bringSubviewToFront:page];
    LOG("直连安全网：提示页已显示");
}
// 建窗口时调：窗口建好前就要提示的，现在补画
static void dguard_show_pending(void) {
    S2ADguardTarget *t = g_dguard_target;
    if (t && t->pending && g_root) { t->pending = NO; dguard_build(t); }
}
void s2a_ios_dguard_prompt(const char *diag, int device) {   // device = 1：上一局已知不是直连（diag 是原因），说这台设备内存不够
    S2ADguardTarget *t = [S2ADguardTarget new]; t->go = dispatch_semaphore_create(0); t->device = device != 0; g_dguard_target = t;
    LOG("直连安全网：诊断信息（随诊断包带走，不上屏）\n%s", diag);
    // 窗口还没建：记下，建好窗口时补画（dguard_show_pending），调用方照旧等玩家选；不放行（审计 P2-5：原先直接照常起局，玩家不知道在强行再试直连）
    void (^show)(void) = ^{ if (g_root) dguard_build(t); else { t->pending = YES; LOG("直连安全网：窗口还没建，建好窗口再画提示页"); } };
    // 主线程上当场画（不派队列：调用方若正跑在主队列的块里，派出去的块要等它返回才轮到）；等待同一个接口，主线程上转运行循环
    if (NSThread.isMainThread) show(); else dispatch_async(dispatch_get_main_queue(), show);
    s2a_ios_wait_ui_signal(t->go);
}
void s2a_ios_render_init_failed(void) {
    LOG("【红】iOS 渲染初始化失败：请求暂停游戏并显示错误，保留日志与诊断入口");
    // Serialize the terminal state with UIKit foreground notifications, so a
    // foreground callback cannot undo the failure's guest freeze.
    dispatch_async(dispatch_get_main_queue(), ^{
        atomic_store(&g_render_failed, 1);
        s2a_controller_set_foreground(0); s2a_guest_freeze(1);
        s2a_ios_show_notice("exclamationmark.triangle.fill", S2AL(@"app.renderFail.title").UTF8String, S2AL(@"app.renderFail.body").UTF8String, 0, 0, 1);
    });
    { int s2a_dump_threads(const char *why); s2a_dump_threads("iOS 出图层挂接失败"); }
    s2a_logcon_flush();
}
// GPU 卡住暂停页（11-13；graphics/nvn/host_adapt.m s2a_gpu_stuck 经 dlsym 调，Metal 完成回调线程）：连续 3 帧各过 3 秒，判据在 nvn_mtl.m gpu_frame_stuck。
//   冻游戏线程（不再往 GPU 送新活，已交的跑完）、手柄不接，盖「继续玩 / 退出」：继续 → 收页、解冻、清判据（s2a_gpu_stuck_resume）；
//   退出 → 同「切换游戏」收尾退出，下次打开回到游戏库。驱动报挂死的安全停页（下面）是终局、不给继续，在屏上时这页不盖它
void s2a_ios_gpu_stuck(const char *why) {
    (void)why;   // 原文已打进 host.log（nvn_mtl.m gpu_frame_stuck），不上屏
    dispatch_async(dispatch_get_main_queue(), ^{
        if (atomic_load(&g_gpu_stopped)) return;
        atomic_store(&g_gpu_stuck_page, 1);
        s2a_controller_set_foreground(0); s2a_guest_freeze(1);
        NSString *t = S2APICK(S2A_GPU_STUCK_TITLE), *b = S2APICK(S2A_GPU_STUCK_BODY);
        g_msg = [NSString stringWithFormat:@"%@\n\n%@", t, b]; g_note = @[@"hourglass", t, b, @(-1.0f), @"", @YES];
        g_note_go = @[S2APICK(S2A_GPU_STUCK_GO), ^{
            g_note_go = nil; g_note_alt = nil; g_note = nil; g_msg = nil; [g_note_view removeFromSuperview]; g_note_view = nil;
            void (*resume)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "s2a_gpu_stuck_resume"); if (resume) resume();
            atomic_store(&g_gpu_stuck_page, 0); s2a_guest_freeze(0);
            if (UIApplication.sharedApplication.applicationState == UIApplicationStateActive) s2a_controller_set_foreground(1);
            LOG("GPU 卡住暂停页：玩家选继续玩，游戏线程解冻");
        }];
        g_note_alt = @[S2APICK(S2A_GPU_STUCK_QUIT), ^{
            LOG("GPU 卡住暂停页：玩家选退出，收尾退出，下次打开回到游戏库");
            void s2a_host_shutdown(const char *why);
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ s2a_host_shutdown("GPU 卡住，玩家选退出"); });
        }];
        [g_help removeFromSuperview]; g_help = nil; g_help_args = nil; note_build();
        if (g_note_view) [g_root bringSubviewToFront:g_note_view];
        LOG("【红】iOS 壳：GPU 卡住 → 冻结游戏线程、显示「继续玩 / 退出」页（%s）", g_note_view ? "已显示" : "窗口还没建，建好窗口再画");
    });
    s2a_logcon_flush();
}
// GPU 挂死安全停页（11-07 Switch2 线 84；graphics/nvn/host_adapt.m s2a_gpu_safe_stop 经 dlsym 调，Metal 完成回调线程）：GPU 已在 nvn_mtl.m 那道门停交，这里冻游戏线程、
//   盖一张 UIKit 原生错误页（不经 Metal：上屏本身也是 GPU 提交），停在这页。复用渲染失败的终局标记：回前台不恢复游戏、不碰 GPU 闸门 / 后台排空（s2a_ios_suspend 早返），
//   之后停帧看门的提示（s2a_ios_show_message / clear_message）不盖掉它。玩家从后台划掉再开 = 新进程，唯一干净的重置
void s2a_ios_gpu_stopped(const char *why) {
    (void)why;   // 原文已由 nvn_mtl.m mtl_gpu_halt 打进 host.log（第一条错误原文、帧号、提交序号），不上屏
    atomic_store(&g_gpu_stopped, 1);
    LOG("【红】iOS 壳：GPU 挂死安全停 → 冻结游戏线程、显示错误页（UIKit），不再恢复本局");
    dispatch_async(dispatch_get_main_queue(), ^{
        atomic_store(&g_render_failed, 1);
        s2a_controller_set_foreground(0); s2a_guest_freeze(1);
        NSString *t = S2AL(@"app.gpuStop.title"), *b = S2AL(@"app.gpuStop.body");
        g_msg = [NSString stringWithFormat:@"%@\n\n%@", t, b]; g_note = @[@"exclamationmark.triangle.fill", t, b, @(-1.0f), @"", @YES];
        g_note_go = nil; g_note_alt = nil; atomic_store(&g_gpu_stuck_page, 0);   // 终局页盖过卡住页：不留「继续玩」
        [g_help removeFromSuperview]; g_help = nil; g_help_args = nil; note_build();
        if (g_note_view) [g_root bringSubviewToFront:g_note_view];
        LOG("GPU 挂死安全停页：%s", g_note_view ? "已显示" : "窗口还没建，建好窗口再画");
    });
    s2a_logcon_flush();
}
// 初启失败页（host_main.c failed_session，宿主线程；审计 #1534 P1-4）：初启失败 = 本进程终局，不在本进程重试。
//   原先直接 _exit，玩家只看到 App 自己消失；现在停在提示页：玩家话术一句 +「故障报修」，请玩家从后台划掉再打开（新进程是唯一干净的重置）。
//   body 由 s2a_init_fail_screen 按错误码给（只含玩家话术），这里只负责显示；内部摘要只在 diag 里（11-02 维护者：内部词不上屏）。
//   复用渲染失败的终局标记：回前台不恢复游戏、不再碰服务层焦点 / GPU 闸门（s2a_ios_suspend）
void s2a_ios_init_failed(const char *body, const char *diag) {
    // 正文是 s2a_init_fail_screen 拼的「<原因>\n<下一步>」两句原宏：英文 App 逐句换 <宏>_EN（host_text），拼法不变
    NSMutableArray *lines = [NSMutableArray array];
    for (NSString *s in [@(body ?: S2APICKC(S2A_INIT_FAIL_MSG_GENERIC)) componentsSeparatedByString:@"\n"]) [lines addObject:@(host_text(s.UTF8String))];
    NSString *t = S2APICK(S2A_INIT_FAIL_TITLE), *b = [lines componentsJoinedByString:@"\n"];
    (void)diag;   // 诊断文字 host_main.c 已写进 host.log，随「故障报修」的诊断包一起传，不上屏
    NSString *key = [NSString stringWithFormat:@"%@\n\n%@", t, b];
    dispatch_async(dispatch_get_main_queue(), ^{
        atomic_store(&g_render_failed, 1);
        s2a_controller_set_foreground(0); s2a_guest_freeze(1);
        g_msg = key; g_note = @[@"exclamationmark.triangle.fill", t, b, @(-1.0f), @"", @YES, @YES];
        [g_help removeFromSuperview]; g_help = nil; g_help_args = nil; note_build();
        if (g_note_view) [g_root bringSubviewToFront:g_note_view];
        LOG("初启失败页：%s", g_note_view ? "已显示" : "窗口还没建，建好窗口再画");
    });
}
UIView *s2a_ios_root_view(void) { return g_root; }
// 撑不住就问玩家（runtime/src/qprompt_host.c 调，宿主线程；判定 runtime/src/qprompt.c）：问过就记在本 App 设置（一款一个 App = 按游戏记住），
//   设置页「60 帧」「宽屏」开关读写的是同一个设置（S2AFps60 / S2AWidescreen），玩家随时改回。帧率选「切换」当场热切（s2a_fps_set_enabled(0)，游戏速度不变）；
//   宽屏选「切换」调满屏线 s2a_widen_force_native(1)（runtime/src/widen.c：本局钉成 1920×1080、热切到原生 16:9）：返回 1 = 已热切；
//   -1 = 游戏自己出宽图的款（第二层值槽装载时填死，裁中间会切掉贴边界面）→ 提示重启后生效（下次起局读 S2AWidescreen 关宽屏）
#include "settings.h"   // s2a_ios_settings_defaults：问过 / 原生 60 帧 / 自动退回都记在这一款（11-11，原先 App 级、串到所有游戏）
static NSString *qp_key(int kind) { return kind == 1 ? @"S2AFps60Asked" : @"S2AWidescreenAsked"; }
int s2a_ios_qp_asked(int kind) { return [s2a_ios_settings_defaults() boolForKey:qp_key(kind)]; }
// 本款原生 60 帧（runtime/src/qprompt_host.c 按运行时实际状态判定后调）：记进本 App 设置，设置页 60 帧那行据此显示「本款原生 60 帧」、开关置灰（app_settings.m）
void s2a_ios_fps_native60(int on) { [s2a_ios_settings_defaults() setBool:on != 0 forKey:@"S2AFpsNative60"]; }
void s2a_fps_set_enabled(int on);
int s2a_fps_hot_revert_ok(void);   // runtime/src/fps.c：1 = 运行中关 60 帧能完全回原帧率；0 = 游戏自己的限帧变量要重启才回（帧率表 w 行没写关值）
// 「切换到原生帧率」重启后生效（原生线 11-02：w 行写回的「原值」是初始化前的快照，八方2 / 巫师3 写 0 = 不限帧）：S2AFps60 已写 NO，新局帧率表起局就关。
//   能进程内重启就问要不要立刻重启（同设置页「立刻重启」s2a_host_restart）；重启失败或当前不能重启，提示从后台划掉再打开
static void qp_fps_restart_ask(void) {
    UIAlertController *r = [UIAlertController alertControllerWithTitle:S2AL(@"app.restartToApply")
        message:S2AL(@"app.qp.fpsRestart.message") preferredStyle:UIAlertControllerStyleAlert];
    [r addAction:[UIAlertAction actionWithTitle:S2AL(@"common.restartLater") style:UIAlertActionStyleCancel handler:^(UIAlertAction *x) { (void)x; LOG("撑不住就问玩家：原生帧率重启后生效，玩家选稍后"); }]];
    [r addAction:[UIAlertAction actionWithTitle:S2AL(@"common.restartNow") style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x;
        void (^done)(NSString *) = ^(NSString *error) {
            if (!error) { LOG("撑不住就问玩家：原生帧率，已进程内重启"); return; }
            LOG("撑不住就问玩家：原生帧率进程内重启没成：%s", error.UTF8String);
            UIAlertController *e = [UIAlertController alertControllerWithTitle:S2AL(@"app.qp.autoRestartFailed") message:[NSString stringWithFormat:S2AL(@"app.qp.autoRestartFailed.message"), error] preferredStyle:UIAlertControllerStyleAlert];
            [e addAction:[UIAlertAction actionWithTitle:S2AL(@"common.gotIt") style:UIAlertActionStyleCancel handler:nil]];
            UIViewController *p = g_root.window.rootViewController; while (p.presentedViewController) p = p.presentedViewController; [p presentViewController:e animated:YES completion:nil]; };
        void *context = (void *)CFBridgingRetain([done copy]);
        int result = s2a_host_session_state() == S2A_SESSION_RUNNING ? s2a_host_restart(settings_restart_result, context) : -1;
        if (!result) { s2a_controller_set_foreground(0); return; }
        void (^rejected)(NSString *) = CFBridgingRelease(context);
        rejected(result == -3 ? S2AL(@"app.restart.busy") : result == -2 ? S2AL(@"app.restart.unsupported") : S2AL(@"app.restart.noSession")); }]];
    UIViewController *p = g_root.window.rootViewController; while (p.presentedViewController) p = p.presentedViewController; [p presentViewController:r animated:YES completion:nil];
}
void s2a_dprompt_notice(NSString *title, NSString *body, double delay, double seconds);   // dprompt.m
// 档位梯子压回原生帧率 / 恢复（runtime/src/qladder_host.c，原因 4）：设置里的 60 帧开关仍显示玩家选的「开」，画面已回到原生帧率——横幅说清楚。
//   热 / 低电量自动降档（原因 1 / 2）11-13 删了（维护者「不读电量、不设自己挡自己的闸」），这里不再有对应文案
// 客体内存三档判定落到「物理内存也装不下」（runtime/src/vawin.c low_ram_notice，#964）：照样开局，游戏内提示一次（不拦）
void s2a_ios_low_ram_notice(void) {
    dispatch_async(dispatch_get_main_queue(), ^{ s2a_dprompt_notice(S2AL(@"app.lowRam.title"), S2AL(@"app.lowRam.body"), 3, 10); });
}
//   压回时带「再试 60 帧」按钮（#1688）：点了梯子回 60 帧档（s2a_ql_retry60）；下一局本来就从 60 帧起
void s2a_dprompt_ask(NSString *title, NSString *body, NSString *accept, NSString *decline, double delay, double seconds, void (^cb)(BOOL));   // dprompt.m
void s2a_ql_retry60(void);   // runtime/src/qladder_host.c
void s2a_ios_fps_capped(int reason) {
    if (reason != 4) { dispatch_async(dispatch_get_main_queue(), ^{ s2a_dprompt_notice(S2AL(@"cpatch.group.fps"), S2AL(@"app.fpsCap.restored"), 0, 6); }); return; }
    s2a_dprompt_ask(S2AL(@"cpatch.group.fps"), S2AL(@"app.fpsCap.ladder"), S2AL(@"app.fpsCap.retry"), S2AL(@"common.gotIt"), 0, 15, ^(BOOL accept) {
        if (accept) { LOG("档位梯子：玩家点了再试 60 帧"); s2a_ql_retry60(); } });
}
// 满屏自检自动退回（runtime/src/qprompt_host.c widen_auto_revert，11-06 满屏泛化）：记进本 App 设置（宽屏关 = 下次起局 16:9，设置页可改回），横幅一行告诉玩家
//   persist（11-06 起）：两次不同启动都触发才记住（runtime/src/widen_remember.c）；只一次的不写设置，下次启动重新试满屏
void s2a_ios_widen_auto(int hot, int why, int persist) {
    if (persist) { NSUserDefaults *d = s2a_ios_settings_defaults(); [d setBool:NO forKey:@"S2AWidescreen"]; [d setBool:YES forKey:qp_key(2)]; }
    // 记住关（之后起局都不开）：11-15 与 Mac「已关掉上次出问题的设置」、齿轮菜单状态句同一句 sgrade.notice.fullscreen_off
    NSString *body = persist ? S2AL(@"sgrade.notice.fullscreen_off") : (hot ? S2AL(@"app.widen.onceHot") : S2AL(@"app.widen.once"));
    (void)why; dispatch_async(dispatch_get_main_queue(), ^{ s2a_dprompt_notice(S2AL(@"app.widen.title"), body, 0, 6); });
}
void s2a_ios_qprompt(int kind, int secs) {   // 文案 维护者 11-02 定：「过去 XX 秒检测到自适应满屏/60帧不稳定，是否切换到原生画面比例/帧率。」
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *top = g_root.window.rootViewController; while (top.presentedViewController) top = top.presentedViewController;
        if (!top) { LOG("撑不住就问玩家：没有可弹窗的界面，这次不问"); return; }
        NSString *t = kind == 1 ? [NSString stringWithFormat:S2AL(@"app.qp.fps.title"), secs]
                                : [NSString stringWithFormat:S2AL(@"app.qp.widen.title"), secs];
        UIAlertController *a = [UIAlertController alertControllerWithTitle:t message:S2AL(@"app.qp.message") preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:S2AL(@"app.qp.switch") style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x;
            NSUserDefaults *d = s2a_ios_settings_defaults(); [d setBool:YES forKey:qp_key(kind)];
            if (kind == 1) { [d setBool:NO forKey:@"S2AFps60"];
                if (s2a_fps_hot_revert_ok()) { s2a_fps_set_enabled(0); LOG("撑不住就问玩家：玩家选还原原生帧率，60 帧已热切关"); return; }
                LOG("撑不住就问玩家：玩家选还原原生帧率，这款要重启后生效（帧率表有没写关值的 w 行）"); qp_fps_restart_ask(); return; }
            [d setBool:NO forKey:@"S2AWidescreen"];
            if (s2a_widen_force_native(1) > 0) { LOG("撑不住就问玩家：玩家选原生画面比例，已运行中退回 16:9"); return; }
            LOG("撑不住就问玩家：玩家选原生画面比例，这款要重启后生效");
            UIAlertController *r = [UIAlertController alertControllerWithTitle:S2AL(@"app.restartToApply") message:S2AL(@"app.qp.widenRestart.message") preferredStyle:UIAlertControllerStyleAlert];
            [r addAction:[UIAlertAction actionWithTitle:S2AL(@"common.gotIt") style:UIAlertActionStyleCancel handler:nil]];
            UIViewController *p = g_root.window.rootViewController; while (p.presentedViewController) p = p.presentedViewController; [p presentViewController:r animated:YES completion:nil]; }]];
        [a addAction:[UIAlertAction actionWithTitle:S2AL(@"app.qp.keep") style:UIAlertActionStyleCancel handler:^(UIAlertAction *x) { (void)x;
            [s2a_ios_settings_defaults() setBool:YES forKey:qp_key(kind)]; LOG("撑不住就问玩家：玩家选保持（%s）", kind == 1 ? "60 帧" : "宽屏"); }]];
        [top presentViewController:a animated:YES completion:nil];
        LOG("撑不住就问玩家：已弹「%s」", t.UTF8String);
    });
}
// 资源导入期间不让系统自动锁屏（锁屏后 App 被挂起、解包停住）；任意线程可调
// 保持亮屏按原因记（#1199）：1 = 游戏库后台解开资源包，2 = 游戏画面在前台。任一原因在就不自动锁屏，都没了才交还系统。
//   原先只有解包一个原因、开关直接写，玩的时候没人开（过场 / 接外接屏时 iPhone 照样自动锁）；两个原因各管各的，解包结束不会把游戏中的亮屏关掉。
//   App 退到后台时系统本来就不因它亮着；切回游戏它还在、接着生效。主机和原生手游的做法，不另设开关
static int g_awake_mask;
static void s2a_ios_awake_set(int bit, int on) {
    dispatch_async(dispatch_get_main_queue(), ^{
        g_awake_mask = on ? (g_awake_mask | bit) : (g_awake_mask & ~bit);
        UIApplication.sharedApplication.idleTimerDisabled = g_awake_mask != 0;
    });
}
void s2a_ios_keep_awake(int on) { s2a_ios_awake_set(1, on); }
// 根控制器共用方向 / 铰链 / 分屏区域；游戏层、触点、宽屏都读 play_layout 的同一份区域（屏幕比例与外接屏比例见 ios/play_layout.m screen_ratio）。
id s2a_ios_ext_register(UIViewController *vc);   // extdisplay.m：登记外接屏场景配件（iOS 27 起系统靠它才给外接屏场景）
@interface S2AGameVC : S2APlayViewController
@property (strong, nonatomic) id extDisplay;   // 外接屏场景配件的登记，跟本控制器同寿
@end
@implementation S2AGameVC
- (void)viewDidLoad { [super viewDidLoad]; self.extDisplay = s2a_ios_ext_register(self); }
// 游戏画面在前台就保持亮屏（#1199）；回游戏库 / 换游戏时根控制器被替换，这里收到消失、交还自动锁屏
- (void)viewDidAppear:(BOOL)animated { [super viewDidAppear:animated]; s2a_ios_awake_set(2, 1); }
- (void)viewDidDisappear:(BOOL)animated { [super viewDidDisappear:animated]; s2a_ios_awake_set(2, 0); }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (void)viewDidLayoutSubviews {   // 屏幕尺寸变了（10-27 Duo 合上切外屏、展开回内屏）：记下当前屏的比例；直连出图层按新尺寸重摆（graphics/nvn/host_adapt.m；通用模式没有这个符号）
    [super viewDidLayoutSubviews];
    void s2a_direct_relayout(void) __attribute__((weak_import));
    if (s2a_direct_relayout) s2a_direct_relayout();
    void nxgal_window_relayout_ios(void) __attribute__((weak_import));
    if (nxgal_window_relayout_ios) nxgal_window_relayout_ios();
}
- (void)viewSafeAreaInsetsDidChange {   // 方向与视图尺寸记进日志（真机验横屏用）
    [super viewSafeAreaInsetsDidChange]; UIEdgeInsets i = self.view.safeAreaInsets; CGSize z = self.view.bounds.size; UIScreen *sc = self.view.window.windowScene.screen ?: UIScreen.mainScreen; CGSize n = sc.nativeBounds.size;   // 当前窗口所在的屏（Duo 内屏亮着时 mainScreen 仍报外屏）
#if TARGET_OS_TV   // 电视只有横屏
    LOG("iOS 壳：朝向 横屏（电视），视图 %.0fx%.0f 点，屏幕 %.0fx%.0f 像素，屏幕宽（1080 高对应）%d；安全区 上 %.1f 左 %.1f 下 %.1f 右 %.1f",
        z.width, z.height, fmax(n.width, n.height), fmin(n.width, n.height), s2a_ios_screen_w(), i.top, i.left, i.bottom, i.right);
#else
    UIInterfaceOrientation o = self.view.window.windowScene.interfaceOrientation;
    LOG("iOS 壳：朝向 %s（%ld），视图 %.0fx%.0f 点，屏幕 %.0fx%.0f 像素，屏幕宽（1080 高对应）%d；安全区 上 %.1f 左 %.1f 下 %.1f 右 %.1f",
        UIInterfaceOrientationIsLandscape(o) ? "横屏" : UIInterfaceOrientationIsPortrait(o) ? "竖屏" : "未知", (long)o, z.width, z.height, fmax(n.width, n.height), fmin(n.width, n.height),
        s2a_ios_screen_w(), i.top, i.left, i.bottom, i.right);
#endif

}
@end
@interface S2AAppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
- (void)startMonitoring;
@end
@implementation S2AAppDelegate
// 静音键下照样出声（#789，系统设置「静音模式下也播放声音」S2APlayInSilentMode，缺省关 = 跟随静音键）：音频会话类别开 = Playback（不跟静音键），
//   关 = SoloAmbient（系统缺省，原先一直是它）。启动和每次回前台各设一次（玩家去「设置」改完切回来就生效）；放后台队列，
//   不在主线程启动早期抢 AVAudioSession（phone_motion.m：真机首次抢会话卡约 2 秒）。类别没变不重设
//   「与其他 App 混音」（#896，S2AMixWithOthers，缺省关）开 = Ambient：别的 App 的音乐 / 播客照放、和游戏混在一起，静音键只静音游戏；优先于「静音键下照样出声」
//   游戏库设置页（S2ArmShellHost ShellSettingsView「声音」）也能改：库与游戏同一进程，改完不经过回前台，听偏好变更即设（只在这两个值真变了才设）
static atomic_int g_audio_play = -1;   // 上次按哪组值设的（静音键下出声 | 混音 << 1；-1 = 还没设过）
static int audio_choice(void) { NSUserDefaults *d = NSUserDefaults.standardUserDefaults; return [d boolForKey:@"S2APlayInSilentMode"] | [d boolForKey:@"S2AMixWithOthers"] << 1; }
static void audio_category_apply(void) {
    int choice = audio_choice(); BOOL play = choice & 1, mix = (choice >> 1) & 1;
    atomic_store(&g_audio_play, choice);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        AVAudioSession *s = AVAudioSession.sharedInstance;
        NSString *want = mix ? AVAudioSessionCategoryAmbient : play ? AVAudioSessionCategoryPlayback : AVAudioSessionCategorySoloAmbient;
        if ([s.category isEqualToString:want]) return;
        NSError *e = nil; BOOL ok = [s setCategory:want error:&e];
        LOG("[声] 音频会话类别 → %s（%s）%s%s", mix ? "Ambient" : play ? "Playback" : "SoloAmbient", mix ? "与其他 App 混音、静音键只静音游戏" : play ? "静音键下照样出声" : "跟随静音键",
            ok ? "" : "，设不上：", ok ? "" : e.localizedDescription.UTF8String ?: "?");
    });
}
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts {
    display_focus_publish(app.applicationState == UIApplicationStateActive);
    free_note();
    if (!s2a_host_boot_deferred()) [self startMonitoring];
    return YES;
}
- (void)startMonitoring {
    display_focus_publish(UIApplication.sharedApplication.applicationState == UIApplicationStateActive);
    // iOS 27 场景生命周期（27.1 SDK 对链接新 SDK 的 App 强制 UIScene；没有场景代理 UIKit 会在
    // _UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption 启动即崩）：窗口 / 根视图 / 虚拟手柄挪到
    // S2ASceneDelegate 的 scene:willConnectToSession 里建（见下）。这里只留 App 级的电量 / 前后台 / 心跳 / 热。
    // 测试层契约第 4 节：每分钟一行「[电] 电量 <N>%，<充电中|已充满|未充电>」（只是读数：能耗记录用；11-13 起不再据此挡局）
    audio_category_apply();
    [NSNotificationCenter.defaultCenter addObserverForName:NSUserDefaultsDidChangeNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n;
        if (audio_choice() != atomic_load(&g_audio_play)) audio_category_apply(); }];   // 游戏库设置页里改的
    { void s2a_diag_boot(void); s2a_diag_boot(); }   // 导出诊断包（diag.m）：订阅 MetricKit、设置页开关回前台时检查
#if !TARGET_OS_TV   // 电视没有电池
    UIDevice.currentDevice.batteryMonitoringEnabled = YES;
    void (^battery)(NSTimer *) = ^(NSTimer *t) { (void)t; UIDevice *d = UIDevice.currentDevice; UIDeviceBatteryState st = d.batteryState;
        LOG("[电] 电量 %d%%，%s", (int)lroundf(d.batteryLevel * 100), st == UIDeviceBatteryStateCharging ? "充电中" : st == UIDeviceBatteryStateFull ? "已充满" : "未充电"); };
    battery(nil); [NSTimer scheduledTimerWithTimeInterval:60 repeats:YES block:battery];
#endif
    // 前后台（10-20 测试设备1 一款第一方动作冒险游戏切桌面后 GPU 拒收后台提交、渲染器按出错退出）：iOS 只禁后台提交 GPU，进后台 = 游戏线程在系统调用出入口冻结 + GPU 闸门关
    // （通知返回前只同步关入口；已在图形 API 里的调用与 GPU 排空在后台任务 + 串行生命周期队列完成）；回前台 = 排在排空之后放开，游戏接着跑，不重开
    // 只失去焦点（系统弹框 / 控制中心 / 来电横幅，仍在前台）不冻结：10-21 测试设备2 一款第一方动作冒险游戏起局 3 秒系统浮层失焦，旧做法冻到收局 54 秒没出首帧；
    // 同一浮层下随后起的暗黑3 从没「回前台」过、一直出帧——失焦期间画面照常可提交，冻结只该跟后台走
    // 用通知不用委托方法：场景式 / 旧式生命周期都发这几条
    s2a_session_on_restart_done(restart_done);   // 任何发起方的进程内重启完成都由壳放重启首帧屏障（见 restart_done）
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ snap_result_check(); });   // 兜住「恢复失败、进程直接退出」：下次打开时说
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; display_focus_publish(NO); LOG("iOS 壳：失去焦点（仍在前台，游戏照跑）"); }];
    [nc addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; display_focus_publish(NO); s2a_dguard_background(1, now_ns()); { int s2a_playtime_flush(void); s2a_playtime_flush(); } { void s2a_diag_session_fg(int); s2a_diag_session_fg(0); } s2a_ios_suspend_enqueue(1, "进后台"); s2a_guest_run_mark(0); }];   // 直连安全网：后台被杀 / 玩家划掉不算崩
    // 直连安全网：前台收到内存告警算这局的失败证据（之后被系统按内存杀掉进不了崩溃处理）；玩满 60 秒照样清零，进后台撤回
    [nc addObserverForName:UIApplicationDidReceiveMemoryWarningNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n;
        if (UIApplication.sharedApplication.applicationState != UIApplicationStateBackground) s2a_dguard_evidence("内存告警", 0); }];
    [nc addObserverForName:UIApplicationWillEnterForegroundNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n;
        display_focus_publish(NO);
        atomic_store(&g_fg_note, now_ns()); s2a_dguard_background(0, now_ns());
        img_scan_if_library();   // 多款容器挂着库：玩家可能刚在「文件」里拷进 game.img
        audio_category_apply();   // 玩家可能刚在「设置」里改了静音键下出声
        // 回前台时可能停在失焦（浮层还在），等不到「获得焦点」：主线程下一拍状态已离开后台就放开
        dispatch_async(dispatch_get_main_queue(), ^{ if (UIApplication.sharedApplication.applicationState != UIApplicationStateBackground) s2a_ios_suspend_enqueue(0, "回前台"); }); }];
    [nc addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; display_focus_publish(UIApplication.sharedApplication.applicationState == UIApplicationStateActive); { void s2a_diag_session_fg(int); s2a_diag_session_fg(1); } s2a_ios_suspend_enqueue(0, "获得焦点"); s2a_guest_run_mark(1); }];
    // 测试开关 S2A_TEST_LIFECYCLE=<秒>（宿主生命周期自测，11-11 v0.3.0 局中失焦崩溃之后加；环境变量只有 devicectl / Xcode 起 App 能给，玩家碰不到）：
    //   启动后这么多秒（起局、虚拟手柄已显示），在主线程上按真机顺序发系统的前后台通知——失去焦点 → 获得焦点（短暂失焦，如下拉横幅）→
    //   进后台 → 回前台 → 获得焦点，走的是上面同一套处理函数与生命周期队列（s2a_ios_suspend_enqueue）。同时设 S2A_STRICT_MAIN=1：
    //   任何只许主线程的 UIKit 入口（S2A_MAIN_ONLY）在后台线程被调就当场中止。每步打「生命周期自测：…」，走完打「生命周期自测：完成」
    {
        const char *e = getenv("S2A_TEST_LIFECYCLE"); double after = e ? atof(e) : 0;
        if (after > 0) {
            setenv("S2A_STRICT_MAIN", "1", 1);
            NSArray<NSNotificationName> *steps = @[UIApplicationWillResignActiveNotification, UIApplicationDidBecomeActiveNotification,
                                                  UIApplicationDidEnterBackgroundNotification, UIApplicationWillEnterForegroundNotification,
                                                  UIApplicationDidBecomeActiveNotification];
            LOG("生命周期自测：%.0f 秒后开始，共 %d 步（S2A_STRICT_MAIN=1）", after, (int)steps.count);
            for (NSUInteger k = 0; k <= steps.count; k++)
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((after + 3.0 * k) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                    if (k == steps.count) { LOG("生命周期自测：完成（%d 步都走完，进程还在）", (int)steps.count); return; }
                    LOG("生命周期自测：第 %d 步 %s", (int)k + 1, k == 0 ? "失去焦点" : k == 2 ? "进后台" : k == 3 ? "回前台" : "获得焦点");
                    [nc postNotificationName:steps[k] object:UIApplication.sharedApplication];
                });
        }
    }
    // 心跳：每 10 秒一行，被系统直接杀（SIGKILL 不进崩溃处理）时也能看到死前最后的帧号 / 热状态 / 内存（10-20 维护者 一款第一方动作冒险游戏闪退，日志 12 秒后无一行）
    // 低区余量（10-21 巫师3 玩约 2 分钟低区耗尽、Metal 申请失败崩溃）：每行带上，局中能看到往下掉的过程
    [NSTimer scheduledTimerWithTimeInterval:10 repeats:YES block:^(NSTimer *t) { (void)t;
#if !TARGET_OS_TV   // 电视没有屏幕亮度
        g_brightness = UIScreen.mainScreen.brightness;   // 自动亮度 / 手动调过也跟上（主线程）
#endif
        s2a_brightness_hold_log();
        // 前后台状态只能在主线程读；其余（整窗 mincore 扫描、分区账、能耗、输入延迟）放后台串行队列：11-05 0400486024DEC000客体内存文件后备后，
        //   整窗 64 GB 的 mincore 一次要数秒，在主线程上跑让系统卡顿检测连报 2–6 秒（MetricKit 卡顿诊断栈：startMonitoring 心跳块 → s2a_window_resident / mem_split_line）
        int app_state = MIN((int)UIApplication.sharedApplication.applicationState, 2);
        static dispatch_queue_t hb_q; static dispatch_once_t hb_once; dispatch_once(&hb_once, ^{ hb_q = dispatch_queue_create("s2a.heartbeat", DISPATCH_QUEUE_SERIAL); });
        dispatch_async(hb_q, ^{
        uint64_t lt, ll, fs, fh, wr = 0, wo = 0; int fn; s2a_low_holes(&lt, &ll); s2a_fd_usage(&fn, &fs, &fh);
        // 整窗 mincore（64 GB）只在内存分区诊断（S2A_MEM_SPLIT=1）开时扫（11-06 合入运维 / 性能线 A：Duo 热状态 2 下一次扫到心跳间隔超 30 秒，
        //   每 10 秒在后台吃秒级 CPU、分段持有进程地址映射锁，执行器还据此误判「出图停」）；缺省只报实占（phys_footprint，系统杀进程按它判），窗口常驻记「未扫」
        int scan = mem_split_on(); if (scan) s2a_window_resident(&wr, &wo);
        char wins[160]; if (scan) snprintf(wins, sizeof wins, "，客体窗口常驻 %llu MB、已压缩换出 %llu MB", (unsigned long long)(wr >> 20), (unsigned long long)(wo >> 20)); else snprintf(wins, sizeof wins, "，客体窗口常驻 未扫（S2A_MEM_SPLIT=1 才扫）");
        LOG("[心跳] 帧 %llu，热状态 %ld（0 正常 1 一般 2 严重 3 危急），进程实占 %llu MB，%s，持续执行档 %d（1 = 系统按稳态功率限制本进程，要权限 sustained-execution），低电量模式 %d，低区空洞合计 %llu MB、最大一块 %llu MB，文件句柄 %d / 软上限 %llu%s", (unsigned long long)s2a_ap_frames(), (long)NSProcessInfo.processInfo.thermalState,
            s2a_footprint_mb(), (const char *[]){"前台", "前台失焦", "后台"}[app_state],
            (int)[NSProcessInfo.processInfo hasPerformanceProfile:NSProcessPerformanceProfileSustained], (int)NSProcessInfo.processInfo.lowPowerModeEnabled, (unsigned long long)(lt >> 20), (unsigned long long)(ll >> 20), fn, (unsigned long long)fs, wins);
        // 本局需求（vawin.c s2a_guest_mem_note：匿名局记实占、文件后备局记实占 + 窗口里在内存的客体页）：下一局起局结算进实测记录，按它判文件后备开不开（#1634）
        { void s2a_guest_mem_note(uint64_t footprint); s2a_guest_mem_note((uint64_t)s2a_footprint_mb() << 20); }
        int s2a_disk_io(uint64_t *rd, uint64_t *wr, uint64_t *pageins);   // energy.c（只此一处用，不进 native.h：那是出包抽取阶段的键）
        // 写盘量（11-05 能耗线，客体内存文件后备的代价）：本进程累计与近 10 秒，内存紧张时内核把文件脏页写回闪存的量在这里看
        { static uint64_t last, lr, lp; static int have; uint64_t w, r, p;
          if (!s2a_disk_io(&r, &w, &p)) { LOG("[写盘] 进程累计 %llu MB，近 10 秒 %llu MB（客体内存文件后备 %s）；近 10 秒读盘 %llu MB、缺页读入 %llu 次", (unsigned long long)(w >> 20), (unsigned long long)(have && w >= last ? (w - last) >> 20 : 0), s2a_guest_fd() >= 0 ? "开" : "关",
              (unsigned long long)(have && r >= lr ? (r - lr) >> 20 : 0), (unsigned long long)(have && p >= lp ? p - lp : 0)); last = w; lr = r; lp = p; have = 1; } }
        { void s2a_guest_backing_log(void); s2a_guest_backing_log(); }   // vawin.c：文件后备开着时报匿名块 / 文件块各多少（溢出模式，没开不打）
        // 客体内存分区账（S2A_MEM_SPLIT=1，缺省关）：心跳每 10 秒一行，另起 100 ms 采样线程抓跳变（mem_split_*）
        if (mem_split_on()) { mem_split_line("内存分区", s2a_footprint_mb(), wr, wo, 0); mem_split_start(); }
        s2a_energy_log();
        { int s2a_playtime_flush(void); s2a_playtime_flush(); }   // 游玩时间同节拍落盘（runtime/src/playtime.c）：闪退 / 被杀最多丢这 10 秒
        s2a_pad_latency latency; s2a_pad_get_latency(&latency, 1);
        if (latency.samples) LOG("[输入延迟] %llu 次变化：快照到HID提交 平均 %.3f ms / 最大 %.3f ms；读取到提交 平均 %.3f ms / 最大 %.3f ms（不含蓝牙和画面显示）",
            (unsigned long long)latency.samples, latency.host_total_us / (1000.0 * latency.samples), latency.host_max_us / 1000.0,
            latency.read_total_us / (1000.0 * latency.samples), latency.read_max_us / 1000.0);
        });
    }];   // 能耗与输入延迟同节拍，不逐帧写日志。
    [NSNotificationCenter.defaultCenter addObserverForName:NSProcessInfoThermalStateDidChangeNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n;
        LOG("[热] 热状态变为 %ld（0 正常 1 一般 2 严重 3 危急），帧 %llu", (long)NSProcessInfo.processInfo.thermalState, (unsigned long long)s2a_ap_frames()); }];
}
#if !TARGET_OS_TV
// 应用层放开全部方向，由各页面自己的 supportedInterfaceOrientations 决定（系统取两者交集）。原先写死「除倒置外」：Duo 内屏是可缩放窗口、游戏页要全部方向
//   （play_layout.m resizable），倒置竖屏被这里挡掉，摄像头那侧朝下拿时画面一直是倒的（#1364）。普通 iPhone 的游戏页仍只给横屏，其他页面系统缺省本来就不含倒置
- (UIInterfaceOrientationMask)application:(UIApplication *)app supportedInterfaceOrientationsForWindow:(UIWindow *)w { return UIInterfaceOrientationMaskAll; }
#endif
- (void)applicationWillTerminate:(UIApplication *)app {
    s2a_guest_run_mark(0);   // 正常终止不算「前台死掉」
    if (s2a_host_boot_pending()) return;
    s2a_controller_set_foreground(0); s2a_host_shutdown("iOS 终止回调");
}
@end
// iOS 27 场景生命周期代理：窗口在这里建并绑 windowScene。Info.plist 的 UIApplicationSceneManifest 指向本类。
// 前后台仍走 app delegate 里注册的 NSNotification（场景式 / 旧式都发这几条），不另挪场景回调。
@interface S2ASceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@property BOOL managingSaves;
@property BOOL previousAsked;                       // 本进程已判过「要不要回到上一份存档」（ios/save_transfer.m s2a_ios_previous_save_prompt）
@property BOOL cloudSynced;                         // 本进程已跑过起局前的 iCloud 存档同步
@property BOOL cloudSyncing;                        // 同步页正盖着：期间切走再回来不起宿主（11-09 审查：曾在同步页底下直接起局）
@property(strong) UIViewController *previousRoot;   // 同步页盖住之前的根（游戏界面），同步完换回
@end
// 亮度保持（10-30 维护者）：只在起局设一次留不住——测试设备开着自动亮度，0.5 设下去随后被系统拉回 0.20–0.33（10-30 暗黑3 测试设备2 一局），
//   那局整机瓦数就不是 0.5 口径。设了 S2A_BRIGHTNESS 时每 2 秒、以及亮度变化通知时重设回目标；整局实际亮度的最小 / 最大值在每次检查时
//   记下，由心跳行与收局行带出（tools/quality_check.py 读「亮度保持」行：偏离设定超过 0.05 判局无效）
static double g_bright_target = -1, g_bright_min = 2, g_bright_max = -1; static unsigned g_bright_resets;
#if !TARGET_OS_TV   // 电视没有屏幕亮度：保持亮度这一套不编（S2A_BRIGHTNESS 在电视上不起作用）
static void brightness_check(UIScreen *scr) {
    double now = scr.brightness;
    if (now < g_bright_min) g_bright_min = now;
    if (now > g_bright_max) g_bright_max = now;
    if (fabs(now - g_bright_target) > 0.005) { scr.brightness = g_bright_target; g_bright_resets++; }
    g_brightness = scr.brightness;
}
static void brightness_hold(UIScreen *scr, double b) {
    g_bright_target = b;
    [NSTimer scheduledTimerWithTimeInterval:2 repeats:YES block:^(NSTimer *t) { (void)t; brightness_check(scr); }];
    [NSNotificationCenter.defaultCenter addObserverForName:UIScreenBrightnessDidChangeNotification object:scr queue:NSOperationQueue.mainQueue
                                                usingBlock:^(NSNotification *n) { (void)n; brightness_check(scr); }];
}
#endif
// 收局 / 心跳都打：「亮度保持：设定 0.50，实际最小 0.50 最大 0.50，重设 N 次」（没设 S2A_BRIGHTNESS 不打）
void s2a_brightness_hold_log(void) {
    if (g_bright_target < 0) return;
    LOG("亮度保持：设定 %.2f，实际最小 %.2f 最大 %.2f，重设 %u 次", g_bright_target, g_bright_min, g_bright_max, g_bright_resets);
}
// 主屏长按快捷操作（Info.plist UIApplicationShortcutItems，定义在 ios/shortcuts.json，runtime/build.mk 写入）：
//   维护者 11-02「App 不是可以在主界面摁住弹出快捷设置么」——10-27 runtime/home-actions 分支做过、没合入 main；这里只接 main 现成的三条流程
//   冷启动（App 没开时选）：置开关，走原有打开流程；热启动（游戏开着时选）：就地执行
void s2a_diag_export(void);
void s2a_diag_open_menu(void);
// 主屏快捷操作（ios/shortcuts.json）。多款容器打开先是游戏库、游戏界面点了 Play 才建，原先三项都挂在游戏界面上，
//   在库里要么没反应、要么弹游戏内的退出框（11-11 宿主梳理）。现在：诊断包不依赖游戏界面、点了就导出分享；设置在库里直达
//   游戏库设置（单款壳仍开游戏内设置）；「存档管理」删掉（库里不知道是哪款），改在长按游戏卡片里
static _Atomic int g_game_picked;   // 本进程已选过游戏（选上之后才放行 sceneDidBecomeActive 起宿主）
static _Atomic int g_open_shell_settings;   // 冷启动点了「设置」：游戏库出现时打开游戏库设置（S2ArmShellHostView 经 s2a_shell_take_open_settings 取走）
int s2a_shell_take_open_settings(void) { return atomic_exchange(&g_open_shell_settings, 0); }   // s2a_shell.h；主线程
#if !TARGET_OS_TV   // 电视没有主屏快捷项
static void home_shortcut(NSString *type, BOOL cold, UIViewController *presenter) {
    (void)presenter;
    LOG("主屏快捷操作：%s（%s）", type.UTF8String, cold ? "冷启动" : "运行中");
    BOOL library = s2a_launch_ctx_container() && !atomic_load(&g_game_picked);
    if ([type isEqualToString:@"dev.s2arm.diag"]) {
        // 冷启动等窗口和界面起来（1 秒）再导出；运行中直接导出。diag.m 在库里取游戏库窗口弹分享面板
        if (cold) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{ s2a_diag_export(); });
        else s2a_diag_export();
    } else if ([type isEqualToString:@"dev.s2arm.settings"]) {
        if (library) {
            // 游戏库设置：冷启动记下、库界面出现时打开；运行中直接通知库界面（ShellUI LibraryView 收 S2AShellOpenSettings）
            if (cold) atomic_store(&g_open_shell_settings, 1);
            else [NSNotificationCenter.defaultCenter postNotificationName:@"S2AShellOpenSettings" object:nil];
        } else if (cold) setenv("S2A_OPEN_SETTINGS", "2", 1);   // 单款壳 / 游戏里：游戏界面建好后 2 秒开设置页（diag.m 同一条路）
        else s2a_diag_open_menu();
    }
}
#endif
// 多款容器（launchctx.c ③，11-07 P2 单款启动门）：场景先挂游戏库；Play → s2a_launch_ctx_select → 起局前检查 → 换游戏界面 → 现行起宿主流程。一进程一款
//   （g_game_picked 声明在上面 home_shortcut 前）
static void top_alert(UIWindow *w, NSString *title, NSString *body) {
    UIViewController *top = w.rootViewController; while (top.presentedViewController) top = top.presentedViewController;
    UIAlertController *a = [UIAlertController alertControllerWithTitle:title message:body preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:S2AL(@"common.ok") style:UIAlertActionStyleDefault handler:nil]];
    [top presentViewController:a animated:YES completion:nil];
}
@interface S2ASceneDelegate ()
- (void)enterGame;
- (void)bootFromScene;
- (void)bootFromSceneState;
@end
static void library_play_go(const char *gid, S2ASceneDelegate *sd);
// 崩了能恢复（11-13 维护者改定：补丁 / 金手指是玩家主动开的，开局前不再问；崩了下次进来自动关掉、告诉玩家关了什么，想用去设置里再开）：
//   上一局死在前台、死的就是这一款（会话标记是整个 App 一份，runtime/src/sesdiag.c；靠上次点开始的那款 S2ALastGameId 认）、当时开着金手指 /
//   玩家开的未验证社区补丁（ios/game_options.m）→ 启动这一局之前关掉它们（社区补丁要在装载前关才不再带进这一局），进游戏后横幅告诉玩家。
//   记下已处理，进游戏后 diag.m 不再处理一遍（只问发诊断）
//   v0.4.1：只关出事的那份（game_options.m s2a_ios_crash_pick_for），并记下这局开着的（s2a_ios_crash_session_begin）。
//   多款容器一律在这里处理（g_s2a_crash_asked 先置上）：原先上一局死的是别的游戏时这里提前返回、没置标记，进游戏后 diag.m 会把这一款开着的关掉
NSArray<NSDictionary *> *s2a_ios_crash_decide_for(NSString *gid);
void s2a_ios_crash_session_begin(NSString *gid, BOOL prev_clean);
void s2a_ios_crash_turn_off_for(NSString *gid, NSArray<NSDictionary *> *items);
extern int g_s2a_crash_asked;
void s2a_dprompt_notice(NSString *title, NSString *body, double delay, double seconds);   // dprompt.m
static void crash_auto_off(const char *gid) {
    g_s2a_crash_asked = 1;
    NSString *g = @(gid);
    char d[1024]; if (s2a_container_docs(d, sizeof d) <= 0) return;
    BOOL same = [[NSUserDefaults.standardUserDefaults stringForKey:@"S2ALastGameId"] isEqualToString:g], fg = s2a_sesmark_prev(d) == S2A_SESMARK_FG;
    NSArray<NSDictionary *> *items = same && fg ? s2a_ios_crash_decide_for(g) : @[];   // 宿主自己崩第一次不关（game_options.m）
    if (items.count) s2a_ios_crash_turn_off_for(g, items);
    s2a_ios_crash_session_begin(g, same && !fg);   // 关完再记这局开着的；上一局是这款且好好收局 → 那局开着的升为「好的」
    if (!items.count) return;
    NSString *names = [[items valueForKey:@"name"] componentsJoinedByString:S2AL(@"common.listSeparator")];
    LOG("崩溃恢复：%s 上一局开着 %s 时死在前台，这一局启动前已关掉（玩家可在设置里再开）", gid, names.UTF8String);
    { void s2a_diag_note_crash_off(NSString *); s2a_diag_note_crash_off(names); }   // 进游戏后诊断询问（diag.m，上次死在前台必问）一并说，不另弹横幅
}
// ---------- 0.6.0 快照：游戏库长按卡片「回到上次存下的这一刻」 ----------
// 有没有：开关关着（起局不带存档开关）一律 0（菜单里就没有这一项）；开着时看这款快照目录里有没有完整清单
static void *g_snap_play_ctx;
int s2a_shell_snapshot_available(const char *gid) {
    int s2a_snap_saving(void), s2a_snap_dir_for(const char *, char *, size_t), s2a_snap_exists(const char *);
    char d[2048]; return gid && *gid && s2a_snap_saving() && !s2a_snap_dir_for(gid, d, sizeof d) && s2a_snap_exists(d);
}
// 游戏库「从这一刻继续」：有完整快照的那款给出快照目录（宿主模型每秒重判时读里面的缩略图和保存时刻）；开关关着 / 没有完整快照回 -1
int s2a_shell_snapshot_dir(const char *gid, char *out, unsigned long size) {
    int s2a_snap_saving(void), s2a_snap_dir_for(const char *, char *, size_t), s2a_snap_exists(const char *);
    char d[2048];
    if (!gid || !*gid || !out || !s2a_snap_saving() || s2a_snap_dir_for(gid, d, sizeof d) || !s2a_snap_exists(d)) return -1;
    return snprintf(out, size, "%s", d) < (int)size ? 0 : -1;
}
// 游戏库长按「删除这一刻」（玩家已确认）：连同缩略图、清单一起删，空间腾出来（runtime/src/snapdel.c）
int s2a_shell_delete_snapshot(const char *gid) {
    int s2a_snap_dir_for(const char *, char *, size_t), s2a_snap_delete(const char *);
    char d[2048]; if (!gid || !*gid || s2a_snap_dir_for(gid, d, sizeof d)) return -1;
    int rc = s2a_snap_delete(d);
    LOG("游戏库：%s 删除存下的这一刻 → %s", gid, rc ? "没删干净" : "已删");
    return rc;
}
static void library_play(const char *gid, void *ctx);
static void snap_library_go(NSString *gid, NSString *dir, BOOL rollback) {
    setenv("S2A_SNAP_RESTORE", dir.fileSystemRepresentation, 1);   // 起局（host_run）时读：核对 → 起服务层 → 按快照恢复，不从头起游戏
    if (rollback) setenv("S2A_SNAP_SAVE_ROLLBACK", "1", 1); else unsetenv("S2A_SNAP_SAVE_ROLLBACK");
    LOG("游戏库：%s 回到上次存下的这一刻（%s）", gid.UTF8String, rollback ? "玩家确认退回存档" : "存档没变过");
    library_play(gid.UTF8String, g_snap_play_ctx);
}
void s2a_shell_restore_snapshot(const char *gid) {   // 主线程
    int s2a_snap_dir_for(const char *, char *, size_t), s2a_snap_exists(const char *), s2a_snap_save_changed(const char *);
    char d[2048]; if (!gid || !*gid || !g_snap_play_ctx || s2a_snap_dir_for(gid, d, sizeof d) || !s2a_snap_exists(d)) return;
    NSString *g = @(gid), *dir = @(d);
    // 存快照之后游戏又存过档：回去会把游戏存档退回那一刻——先问（当前存档会先整棵备份）；没变过直接回
    if (s2a_snap_save_changed(d) != 1) { snap_library_go(g, dir, NO); return; }
    S2ASceneDelegate *sd = (__bridge S2ASceneDelegate *)g_snap_play_ctx;
    UIAlertController *a = [UIAlertController alertControllerWithTitle:S2AL(@"vpad.snap.saveChanged.title") message:S2AL(@"vpad.snap.saveChanged.body") preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:S2AL(@"common.cancel") style:UIAlertActionStyleCancel handler:nil]];
    [a addAction:[UIAlertAction actionWithTitle:S2AL(@"vpad.snap.saveChanged.go") style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *x) { snap_library_go(g, dir, YES); }]];
    UIViewController *p = sd.window.rootViewController; while (p.presentedViewController) p = p.presentedViewController; [p presentViewController:a animated:YES completion:nil];
}
extern NSString *g_s2a_prev_last_game;   // ios/save_transfer.m 定义。多款容器：这次点开始之前上一局是哪款（library_play_go 覆盖 S2ALastGameId 前记下）；单款 App 一直 nil
static void library_play(const char *gid, void *ctx) {   // 主线程（s2a_shell.h 约定）
    S2ASceneDelegate *sd = (__bridge S2ASceneDelegate *)ctx;
    if (!atomic_load(&g_game_picked)) crash_auto_off(gid);
    library_play_go(gid, sd);
}
// 程序约定号对不上本宿主的款（游戏号 → 1 / 0）：s2a_shell_game_needs_host_update 的缓存，点开被拒时也写进来
static NSMutableDictionary<NSString *, NSNumber *> *g_host_update;
// 程序包签名本机不认的款：点开被拒（S2A_SELECT_NEEDS_RESIGN）时记下这款程序包清单的修改时间（纳秒）。程序包换过（电脑重推、手机重签）时间就变，
//   不再算被拒；手机上导入了签名证书（S2ASigningCertChanged）全部清掉：起局时用新证书重签，不用重开 App。清单读不到记 -1（同样一变就清）
static NSMutableDictionary<NSString *, NSNumber *> *g_reinstall;
static long long program_cat_mtime(const char *gid) {
    char root[1024], p[1300]; struct stat st;
    if (s2a_program_games_root(root, sizeof root, getenv("HOME")) || snprintf(p, sizeof p, "%s/%s/program/catalogue.json", root, gid) >= (int)sizeof p || stat(p, &st)) return -1;
    return (long long)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
}
// 每款签名状态（s2a_shell.h s2a_shell_game_sign_state；判据 ios/sign_state.m）：库每秒重判，按（程序包清单修改时间、证书代数、被拒没有）缓存，变了才重读那几个小文件。
//   证书代数：手机上导入了证书（cert_import.m 发 S2ASigningCertChanged）加一，同时清被拒记录、让库立刻重判 → 卡片自己回「可以玩」
int s2a_sign_state_at(const char *home, const char *catalogue, int rejected);   // ios/sign_state.m
static NSMutableDictionary<NSString *, NSArray<NSNumber *> *> *g_sign_cache;   // 游戏号 → [清单时间, 证书代数, 被拒, 状态]
static long g_cert_gen;
static void sign_watch_certs(void) {
    static int once; if (once++) return;
    [NSNotificationCenter.defaultCenter addObserverForName:@"S2ASigningCertChanged" object:nil queue:NSOperationQueue.mainQueue usingBlock:^(__unused NSNotification *n) {
        g_cert_gen++; NSUInteger k = g_reinstall.count; [g_reinstall removeAllObjects];
        LOG("游戏库：签名证书换了，清掉 %lu 款「签名被拒」记录、重判各款签名状态", (unsigned long)k);
        if (s2a_shell_refresh) s2a_shell_refresh();
    }];
}
int s2a_shell_game_sign_state(const char *gid) {
    if (!gid || !*gid) return 0;
    sign_watch_certs();
    long long mt = program_cat_mtime(gid); NSNumber *rj = g_reinstall[@(gid)];
    if (rj && rj.longLongValue != mt) { [g_reinstall removeObjectForKey:@(gid)]; rj = nil; LOG("游戏库：%s 的程序包换过了，不再算签名被拒", gid); }
    if (!g_sign_cache) g_sign_cache = [NSMutableDictionary dictionary];
    NSArray<NSNumber *> *c = g_sign_cache[@(gid)];
    if (c && c[0].longLongValue == mt && c[1].longValue == g_cert_gen && c[2].boolValue == (rj != nil)) return c[3].intValue;
    char root[1024], p[1300]; int st = 0;
    if (mt >= 0 && !s2a_program_games_root(root, sizeof root, getenv("HOME")) && snprintf(p, sizeof p, "%s/%s/program/catalogue.json", root, gid) < (int)sizeof p)
        st = s2a_sign_state_at(getenv("HOME"), p, rj != nil);
    else st = rj ? 5 : 0;
    if (!c || c[3].intValue != st) LOG("游戏库：%s 签名状态 %d（0 可玩 / 1 没证书 / 2 证书团队不符 / 3 证书已换 / 4 签名格式老 / 5 只能电脑重装）", gid, st);
    g_sign_cache[@(gid)] = @[@(mt), @(g_cert_gen), @(rj != nil), @(st)];
    return st;
}
static void library_play_go(const char *gid, S2ASceneDelegate *sd) {
    if (atomic_load(&g_game_picked)) {
        LOG("游戏库：同一进程第二次 Play %s，拒绝（一进程一款）", gid);
        top_alert(sd.window, S2AL(@"app.library.reopen.title"), S2AL(@"app.library.reopen.message")); return;
    }
    const char *why = s2a_launch_ctx_select(gid);
    // why 是 launchctx.c 的内部原因（路径 / errno，已进 host.log「选游戏…被拒」）：屏上按类别说人话 + 下一步，原文只进日志 / 诊断（v0.3.1）
    if (why) {
        int kind = s2a_launch_ctx_select_kind();
        if (kind == S2A_SELECT_NEEDS_RESIGN) {   // 签名本机不认（通用壳 11-16）：不弹窗，回游戏库按原因标（s2a_shell_game_sign_state：手机上缺能用的证书 → 「需要签名证书」直接导；有证书仍被拒 → 电脑上重装）
            if (!g_reinstall) g_reinstall = [NSMutableDictionary dictionary];
            g_reinstall[@(gid)] = @(program_cat_mtime(gid));
            LOG("游戏库：%s 的程序库签名本机不认，记下、库按原因重判（签名状态 %d）", gid, s2a_shell_game_sign_state(gid));
            if (s2a_shell_refresh) s2a_shell_refresh();
            return;
        }
        if (kind == S2A_SELECT_NEEDS_UPDATE) {
            // 程序按别的约定号出：不弹窗，回游戏库在这款卡片上标「需要在电脑上更新」（预判没读到时的兜底；原因已进 host.log）
            if (!g_host_update) g_host_update = [NSMutableDictionary dictionary];
            g_host_update[@(gid)] = @1;
            if (s2a_shell_refresh) s2a_shell_refresh();
            return;
        }
        top_alert(sd.window, S2AL(@"app.library.cantOpen"), kind == S2A_SELECT_APP_INCOMPLETE ? S2AL(@"app.library.cantOpen.incomplete")
                  : kind == S2A_SELECT_STORAGE ? S2AL(@"app.library.cantOpen.storage") : S2AL(@"app.library.cantOpen.message"));
        return;
    }
    atomic_store(&g_game_picked, 1);
    g_s2a_prev_last_game = [NSUserDefaults.standardUserDefaults stringForKey:@"S2ALastGameId"] ?: @"";   // 覆盖前记下上一局是哪款：起局前「回到上一份存档」按它认上一局死的是不是这一款（save_transfer.m）
    [NSUserDefaults.standardUserDefaults setObject:@(gid) forKey:@"S2ALastGameId"];   // 诊断包在库里导出时认这一款（diag.m last_container_game_root）
    s2a_boot_rebase("点 Play（多款容器选了游戏、启动上下文已冻结）");   // 起局计时从这里算，库界面停留不计
    LOG("游戏库：选了 %s，%s", gid, s2a_host_pre_ui_checks());
    [sd enterGame];
}
// 游戏库按款核资源包（11-07 Duo「游戏没能启动：系统功能没能准备好」：库里对资源包不在机上的款仍给 Play，服务层读 资源包/exefs/main.npdm 抛 DirectoryNotFound）。
//   库界面（S2ArmShellHost）经 s2a_shell.h 的这两个口问 / 报；这里是强定义，盖过 S2ArmShellBridge 里的弱缺省。判据同起局前的导入闸门（pack_apply.c s2a_import_state，
//   按本款文档根 Documents/games/<游戏号>）：就绪或已拷到待解开（Play 后闸门解开）= 可开始；不在 / 不完整 = 库里「需要游戏数据」、点了给下面的说明
static __weak UIWindow *g_lib_window;
int s2a_import_state_game_quick(const char *proc_docs, const char *gid);   // pack_apply.c
static int game_pack_state(const char *gid) { char d[1024]; s2a_container_docs(d, sizeof d); return s2a_import_state_game_quick(d, gid); }
static void test_picker(void);
void s2a_shell_library_appeared(void) { static int once; if (!once++) { BOOT_MARK("游戏库界面可操作"); test_picker(); } }
int s2a_game_install_state(const char *proc_docs, const char *gid);   // pack_apply.c
int s2a_shell_game_install_state(const char *gid) { char d[1024]; s2a_container_docs(d, sizeof d); return s2a_game_install_state(d, gid); }
// 程序约定号对不上本宿主（runtime s2a_launch_ctx_game_needs_update）：每款读一次清单后记下，点开被拒的（S2A_SELECT_NEEDS_UPDATE）也记上；
//   库每秒重判只查这里。主线程（s2a_shell.h）
int s2a_shell_game_needs_host_update(const char *gid) {
    if (!gid || !*gid) return 0;
    if (!g_host_update) g_host_update = [NSMutableDictionary dictionary];
    NSString *k = @(gid); NSNumber *v = g_host_update[k];
    if (!v) { v = @(s2a_launch_ctx_game_needs_update(gid)); g_host_update[k] = v; if (v.intValue) LOG("游戏库：%s 的程序约定号对不上本宿主，卡片标「需要在电脑上更新」", gid); }
    return v.intValue;
}
// 库里后台解开传完的那款（装机阶段 2，s2a_shell.h）：同一条 s2a_pack_apply（解包日志、.applied、清旧文件都同起局闸门），同时只解一款；
//   解的时候关自动锁屏（锁屏 = 挂起 = 解包停住），进度经 s2a_pack_apply_progress 记下给库画蒙版，解完回主线程让库重判
unsigned s2a_pack_repair_tree(const char *dir); int s2a_pack_apply(const char *docs); extern void (*s2a_pack_apply_progress)(int done, int total); const char *s2a_pack_apply_failure(void); const char *s2a_pack_apply_code(void);   // pack_apply.c
// 装机 / 导入失败按码说人话（11-11 维护者：原文和码只进诊断，不上屏）：码表 launchers/shared/experience.v1.json errors.items，
//   「标题 + 下一步」由 launchers/shared/experience_strings.py 出包时生成进包里的 S2ArmHost.strings（键 error.<码>.title / .next，仓库表里不放）。
//   规格里查不到的码一律按 ios_install_unknown（键是拼出来的，不走 S2AL 字面量）
static NSString *install_error_text(const char *code) {
    NSString *(^get)(NSString *, NSString *) = ^NSString *(NSString *c, NSString *part) {
        NSString *k = [NSString stringWithFormat:@"error.%@.%@", c, part], *v = [NSBundle.mainBundle localizedStringForKey:k value:@"" table:@"S2ArmHost"];
        return v.length && ![v isEqualToString:k] ? v : nil; };
    NSString *c = code && *code ? @(code) : @"ios_install_unknown", *title = get(c, @"title"), *next = get(c, @"next");
    if (!title || !next) { c = @"ios_install_unknown"; title = get(c, @"title"); next = get(c, @"next"); }
    return [NSString stringWithFormat:S2AL(@"error.sentence"), title ?: c, next ?: @""];
}
static char g_inst_gid[32]; static atomic_int g_inst_busy, g_inst_done, g_inst_total;
static void inst_progress(int done, int total) { atomic_store(&g_inst_done, done); atomic_store(&g_inst_total, total); }
// 单一 IMG 的分段进度（pack_apply.c s2a_pack_apply_phase，v0.3.0）：1 解开推送包（包数）/ 2 从 game.img 解小文件（字节）/ 3 核对机上文件（文件数）。
//   弱引用：引擎还没有这个回调时（旧 pack_apply.c）不挂，库照旧按 s2a_pack_apply_progress 画一段进度
extern void (*s2a_pack_apply_phase)(int phase, uint64_t done, uint64_t total) __attribute__((weak_import));
static atomic_int g_inst_phase; static _Atomic uint64_t g_inst_pdone, g_inst_ptotal;
static void inst_phase(int phase, uint64_t done, uint64_t total) { atomic_store(&g_inst_phase, phase); atomic_store(&g_inst_pdone, done); atomic_store(&g_inst_ptotal, total); }
static NSMutableDictionary<NSString *, NSString *> *g_inst_fail;   // 主线程：游戏号 → 最后一次解开失败的原因（成功后清掉）
int s2a_shell_install_phase(const char *gid, int *phase, double *fraction) {   // 主线程
    if (!atomic_load(&g_inst_busy) || !gid || strcmp(gid, g_inst_gid)) return 0;
    int p = atomic_load(&g_inst_phase); uint64_t done = atomic_load(&g_inst_pdone), total = atomic_load(&g_inst_ptotal);
    if (!p) { done = (uint64_t)atomic_load(&g_inst_done); total = (uint64_t)atomic_load(&g_inst_total); }
    *phase = p; *fraction = total ? (double)done / (double)total : -1; return 1;
}
const char *s2a_shell_install_failure(const char *gid) {   // 主线程
    static char out[600]; NSString *why = gid ? g_inst_fail[@(gid)] : nil;
    if (!why.length) return NULL;
    strlcpy(out, why.UTF8String, sizeof out); return out;
}
static atomic_int g_games_repairing = 1;   // 首次清点容器 / 资源包挪位（img_scan_bg）完成之前算「在查」：冷启动库先于扫描出现时也不抢着解包
int s2a_shell_install_game(const char *gid) {   // 主线程（s2a_shell.h 约定）
    if (atomic_load(&g_games_repairing)) return 0;   // 还在清点 / 挪资源包：先不解，库下一秒重判再起
    if (!gid || strlen(gid) != 16 || strchr(gid, '/') || atomic_exchange(&g_inst_busy, 1)) return 0;
    strlcpy(g_inst_gid, gid, sizeof g_inst_gid); atomic_store(&g_inst_done, 0); atomic_store(&g_inst_total, 0);
    atomic_store(&g_inst_phase, 0); atomic_store(&g_inst_pdone, 0); atomic_store(&g_inst_ptotal, 0);
    char d[1024]; s2a_container_docs(d, sizeof d);
    NSString *docs = [NSString stringWithFormat:@"%s/games/%s", d, gid], *name = @(gid);
    LOG("游戏库：后台解开 %s", gid); s2a_ios_keep_awake(1);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        s2a_pack_apply_progress = inst_progress; if (&s2a_pack_apply_phase) s2a_pack_apply_phase = inst_phase;
        int rc = s2a_pack_apply(docs.fileSystemRepresentation);
        s2a_pack_apply_progress = NULL; if (&s2a_pack_apply_phase) s2a_pack_apply_phase = NULL;
        NSString *why = rc ? install_error_text(s2a_pack_apply_code()) : nil;   // 屏上一句人话；原文与码进日志
        if (rc) LOG("【红】游戏库：解开 %s 没成（%s）：%s", name.UTF8String, s2a_pack_apply_code(), s2a_pack_apply_failure()); else LOG("游戏库：%s 解开了", name.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!g_inst_fail) g_inst_fail = [NSMutableDictionary new];
            if (why.length) g_inst_fail[name] = why; else [g_inst_fail removeObjectForKey:name];   // 卡片上「安装没完成」下面显示这一句，「重试」成功后清掉
            s2a_ios_keep_awake(0); g_inst_gid[0] = 0; atomic_store(&g_inst_busy, 0); s2a_shell_refresh();
        });
    });
    return 1;
}
int s2a_shell_install_wait(void) { return atomic_load(&g_games_repairing) ? 2 : atomic_load(&g_inst_busy) ? 1 : 0; }   // 主线程（s2a_shell.h）
int s2a_shell_install_progress(const char *gid, int *done, int *total) {   // 主线程
    if (!atomic_load(&g_inst_busy) || !gid || strcmp(gid, g_inst_gid)) return 0;
    *done = atomic_load(&g_inst_done); *total = atomic_load(&g_inst_total); return 1;
}
// 「需要游戏数据 / 重新检查」：快判（.applied 等于清单）不成立时 game_pack_state 退回逐文件核对，一款约 10 万文件要几秒，放主线程会被 iOS 看门狗强杀
//   （#438 0x8BADF00D：0.2.1 回前台主线程核对 > 2.5 秒，系统结束请求 5 秒到点）。核对挪到后台队列，核完回主线程刷新 / 给说明；
//   超过 0.3 秒还没核完就先弹「正在检查…」（快判成立时几毫秒就回来，不闪这一下）。同时只核一款，核着时再点不重复起
static atomic_int g_pack_checking;
static UIViewController *lib_top(void) { UIViewController *top = g_lib_window.rootViewController; while (top.presentedViewController) top = top.presentedViewController; return top; }
// 测试开关 S2A_TEST_PICKER=cheats|saves|cloud（复现 / 复验「点金手指导入、云存档选文件夹就卡死」）：库界面可操作后自动弹出和按钮同一个系统文件选择器
//   （cheats = vpad_cheats.m s2a_cheats_import_picker、cloud = 壳 CloudSaves.swift 同一个、saves = save_transfer.m 导入那个配置），
//   日志记弹出 / 弹完的时刻，60 秒内主线程每卡住 200 ms 以上记一行（看门狗线程每 100 ms 往主线程投一次）。不设开关时什么都不做
#if TARGET_OS_TV   // 电视没有系统文件选择器
static void test_picker(void) {}
#else
UIDocumentPickerViewController *s2a_cheats_import_picker(void);   // vpad_cheats.m
void s2a_shell_test_cloud_picker(void) __attribute__((weak_import));   // 壳 CloudSaves.swift（缺省出壳不带库界面时没有）
@interface S2ATestPickerDelegate : NSObject <UIDocumentPickerDelegate> @end
@implementation S2ATestPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController *)c didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls { LOG("测试选择器：选了 %lu 个", (unsigned long)urls.count); }
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)c { LOG("测试选择器：取消"); }
@end
static void test_picker(void) {
    const char *kind = getenv("S2A_TEST_PICKER");
    if (!kind || !*kind) return;
    static S2ATestPickerDelegate *delegate; delegate = [S2ATestPickerDelegate new];
    double t0 = CFAbsoluteTimeGetCurrent();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{   // 看门狗：主线程多久没回应
        for (int i = 0; i < 600; i++) {
            double sent = CFAbsoluteTimeGetCurrent();
            __block _Atomic int back = 0;
            dispatch_async(dispatch_get_main_queue(), ^{ back = 1; });
            usleep(100000);
            if (!back) {
                while (!back) usleep(10000);
                double stall = (CFAbsoluteTimeGetCurrent() - sent) * 1000;
                if (stall >= 200) LOG("测试选择器：主线程卡住 %.0f ms（弹出后 %.1f 秒起）", stall, sent - t0);
            }
        }
        LOG("测试选择器：看门狗 60 秒结束");
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (!strcmp(kind, "cloud")) {
            if (!s2a_shell_test_cloud_picker) { LOG("测试选择器：这个壳没带库界面，没有云存档选择器"); return; }
            LOG("测试选择器：弹出 cloud（库可操作后 %.1f 秒）", CFAbsoluteTimeGetCurrent() - t0);
            s2a_shell_test_cloud_picker();
            return;
        }
        UIDocumentPickerViewController *p;
        if (!strcmp(kind, "saves")) {
            p = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:YES];
            p.directoryURL = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
        } else {
            p = s2a_cheats_import_picker();
        }
        p.delegate = delegate;
        double t1 = CFAbsoluteTimeGetCurrent();
        LOG("测试选择器：弹出 %s（库可操作后 %.1f 秒）", kind, t1 - t0);
        [lib_top() presentViewController:p animated:YES completion:^{ LOG("测试选择器：弹完 %.0f ms", (CFAbsoluteTimeGetCurrent() - t1) * 1000); }];
    });
}
#endif
static void library_pack_help_show(NSString *gid, BOOL again, int st);
// 玩家把 game.img 放进「文件」里 S2Arm 的「放这里导入」文件夹（v0.3.0，自签 / 侧载玩家）：挪进本款资源包根（pack_apply.c s2a_img_scan）。
//   冷启动挂库、回前台、库里「重新检查」时扫；已选游戏（起局后）不扫。挪好了让库重判
int s2a_img_scan(const char *proc_docs);   // pack_apply.c
void s2a_cert_scan_import_dirs(void);   // cert_import.m：「放这里导入」里的 .p12 / .pfx（签名证书，问密码后导入）
BOOL s2a_cert_handle_url(NSURL *url);   // cert_import.m：SideStore 交回签名证书的回调 s2arm://sidestore-cert
const char *s2a_img_scan_failure(void); const char *s2a_img_scan_code(void);   // pack_apply.c：最近一次认领失败的原因 / 码（空串 = 没失败）
const char *s2a_img_scan_noshaders(void);   // pack_apply.c：这次认领里不带着色器库的文件名（空串 = 没有）
const char *s2a_img_scan_skipped(void);   // pack_apply.c：这次扫描跳过的着色器更新包（和当前游戏版本不配套）文件名（空串 = 没有）
static int img_scan_now(void) {   // 后台线程。几个入口（挂库 / 回前台 / 文件夹有写入 / 库里「重新检查」）可能同时进来：一次只扫一个，免得两边去挪同一个文件
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    if (atomic_load(&g_game_picked)) return 0;
    pthread_mutex_lock(&mu);
    char d[1024]; s2a_container_docs(d, sizeof d); int n = atomic_load(&g_game_picked) ? 0 : s2a_img_scan(d);
    pthread_mutex_unlock(&mu);
    if (n > 0) LOG("资源导入：从「文件」里认出并放好 %d 个游戏资源包（game.img）", n);
    return n;
}
// 认领失败（格式号不认识、挪不动等，pack_apply.c s2a_img_scan_failure）：在游戏库上弹一次原因。同一句只弹一次，回前台重扫不刷屏
static void img_scan_report(void) {   // 主线程
    // 认领的文件不带着色器库（资源包文件夹里的 game.img 按设计不带）：玩家该用导出的完整 .img，缺着色器能跑只是兜底——告诉他文件不完整、该换哪一份，不拦（同一个文件名只说一次）
    { static char told[256]; const char *f = s2a_img_scan_noshaders();
      if (f && *f && strcmp(f, told) && g_lib_window) {
          strlcpy(told, f, sizeof told);
          UIAlertController *a = [UIAlertController alertControllerWithTitle:S2AL(@"img.noShaders.title")
              message:[NSString stringWithFormat:S2AL(@"img.noShaders.body"), @(f)] preferredStyle:UIAlertControllerStyleAlert];
          [a addAction:[UIAlertAction actionWithTitle:S2AL(@"common.gotIt") style:UIAlertActionStyleCancel handler:nil]];
          [lib_top() presentViewController:a animated:YES completion:nil];
          return;   // 一次只弹一个；认领失败的那句下次回前台再说
      } }
    // 着色器更新包和当前游戏版本不配套（维护者 11-17：替玩家判，不套用、不报错）：说一句已跳过，游戏照常玩（同一个文件只说一次）
    { static char told[256]; const char *f = s2a_img_scan_skipped();
      if (f && *f && strcmp(f, told) && g_lib_window) {
          strlcpy(told, f, sizeof told);
          UIAlertController *a = [UIAlertController alertControllerWithTitle:S2AL(@"img.updateSkipped.title")
              message:[NSString stringWithFormat:S2AL(@"img.updateSkipped.body"), @(f)] preferredStyle:UIAlertControllerStyleAlert];
          [a addAction:[UIAlertAction actionWithTitle:S2AL(@"common.gotIt") style:UIAlertActionStyleCancel handler:nil]];
          [lib_top() presentViewController:a animated:YES completion:nil];
          return;
      } }
    static char shown[600]; const char *why = s2a_img_scan_failure();
    if (!why || !*why || !strcmp(why, shown) || !g_lib_window) return;
    strlcpy(shown, why, sizeof shown);
    UIAlertController *a = [UIAlertController alertControllerWithTitle:S2AL(@"img.scanFailed.title") message:install_error_text(s2a_img_scan_code()) preferredStyle:UIAlertControllerStyleAlert];   // 原文已进日志（img_scan_dir）
    [a addAction:[UIAlertAction actionWithTitle:S2AL(@"common.gotIt") style:UIAlertActionStyleCancel handler:nil]];
    [lib_top() presentViewController:a animated:YES completion:nil];
}
// App 容器里属主不是 App 的条目（#569；维护者 11-15 定：不迁移、不驻留、不靠电脑）：App 改不了也删不掉，只有删除重装能清。
//   每进程第一次扫之前清点一次（pack_apply.c s2a_foreign_scan），读数进日志 / 诊断；只在真有影响时对玩家说：占用 ≥ 2 GB 时在库上说一次占了多少、
//   为什么删不掉、怎么办（先导出存档 → 删除重装 S2Arm → 导回存档）。写不进导致装不上 / 起不了的，由解包失败码 ios_install_dir_unwritable 那条说
unsigned s2a_foreign_scan(const char *home, uint64_t *bytes, char *line, size_t n);   // pack_apply.c
static uint64_t g_foreign_bytes;
static void foreign_report(void) {   // 主线程
    if (g_foreign_bytes < (2ull << 30) || !g_lib_window) return;
    NSString *gb = [NSString stringWithFormat:@"%.1f", g_foreign_bytes / 1073741824.0];
    UIAlertController *a = [UIAlertController alertControllerWithTitle:S2AL(@"container.locked.title")
        message:[NSString stringWithFormat:S2AL(@"container.locked.body"), gb] preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:S2AL(@"common.gotIt") style:UIAlertActionStyleCancel handler:nil]];
    [lib_top() presentViewController:a animated:YES completion:nil];
}
// 资源包挪出 Documents（v0.3.0，pack_apply.c s2a_pack_migrate）：#569 修复之后、认领之前，每进程第一次扫时做；迁移期间同 #569 不起解包。
//   新位置（Library/Application Support/S2Arm）标「不进 iCloud 备份」（几十 GB 的资源包不该进备份，电脑上都有）；建好「放这里导入」文件夹（名字跟界面语言）
int s2a_pack_migrate(const char *proc_docs, int *failed); const char *s2a_pack_migrate_failure(void);   // pack_apply.c
extern const char *const s2a_img_import_dirs[2];   // pack_apply.c：认领扫的两个名字（中 / 英）
static void pack_layout_prepare(const char *proc_docs) {   // 后台线程
    int failed = 0, moved = s2a_pack_migrate(proc_docs, &failed);
    if (moved || failed) LOG("资源包迁移：挪进 Library %d 款%s%s", moved, failed ? "，失败 " : "", failed ? s2a_pack_migrate_failure() : "");
    char base[1100]; if (s2a_pack_base(base, sizeof base, proc_docs) > 0) {
        NSURL *u = [NSURL fileURLWithPath:@(base) isDirectory:YES]; NSError *e = nil;
        [NSFileManager.defaultManager createDirectoryAtURL:u withIntermediateDirectories:YES attributes:nil error:nil];
        if (![u setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&e]) LOG("【红】资源包目录标「不进备份」没成：%s", e.localizedDescription.UTF8String);
    }
    // 导入文件夹：名字按界面语言取（img.importFolder，两份字符串表的值必须是 s2a_img_import_dirs 里的两个名字之一，认领才扫得到）；另一个名字的已在就不再建
    NSString *name = S2AL(@"img.importFolder"); BOOL known = NO, exists = NO;
    for (int i = 0; i < 2; i++) {
        known |= [name isEqualToString:@(s2a_img_import_dirs[i])];
        exists |= [NSFileManager.defaultManager fileExistsAtPath:[@(proc_docs) stringByAppendingPathComponent:@(s2a_img_import_dirs[i])]];
    }
    if (!known) LOG("【红】导入文件夹名「%s」不在认领扫的名单里（pack_apply.c s2a_img_import_dirs），不建", name.UTF8String);
    else if (!exists) [NSFileManager.defaultManager createDirectoryAtPath:[@(proc_docs) stringByAppendingPathComponent:name] withIntermediateDirectories:YES attributes:nil error:nil];
}
int s2a_dump_threads(const char *why);   // runtime/src/tdump.c
static void import_watch_start(void);
// 扫描走自己的串行队列、按「玩家在等」的档位（USER_INITIATED）：玩家看着游戏库等导入，原先 UTILITY 档在手机忙时排不上（11-17 测试设备：
//   冷启动后台块打完「容器内属主」一行就再没下文，导入函数没被调用）。启动维护（属主清点 / 补写权限 / 迁移 / 程序包清理）15 秒没做完打一次全线程现场，卡在哪一步直接看得到
static dispatch_queue_t img_scan_queue(void) {
    static dispatch_queue_t q; static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("s2a.img-scan", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0)); });
    return q;
}
static void img_scan_bg(void) {
    dispatch_async(img_scan_queue(), ^{
        static atomic_int checked; int r = 0;
        if (!atomic_exchange(&checked, 1) && !atomic_load(&g_game_picked)) { char d[1024]; s2a_container_docs(d, sizeof d);
            static atomic_int maint_done;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                if (!atomic_load(&maint_done)) { LOG("【红】启动维护 15 秒没做完（属主清点 / 补写权限 / 迁移 / 程序包清理），导入在它后面等：打全线程现场"); s2a_dump_threads("启动维护 15 秒没做完"); } });
            char line[1600]; uint64_t fb = 0; if (s2a_foreign_scan(NSHomeDirectory().fileSystemRepresentation, &fb, line, sizeof line)) { LOG("【黄】%s", line); g_foreign_bytes = fb; r = 1; }
            { unsigned fixed = s2a_pack_repair_tree(d);   // 先补 App 自己名下缺写权限的目录 / 文件（D-A76N），电脑端下一次推送就写得进
              NSString *lib = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/S2Arm"]; fixed += s2a_pack_repair_tree(lib.fileSystemRepresentation);
              if (fixed) LOG("启动：%u 个目录 / 文件是 App 自己的但缺写权限，已补回属主读写（D-A76N）", fixed); }
            pack_layout_prepare(d);
            int gc = s2a_program_gc(NSHomeDirectory().fileSystemRepresentation, time(NULL));   // 程序包旧版本目录（电脑端换上新版后留下的、推断了的）：启动时清，一进程一次
            if (gc) LOG("程序包清理：本次启动清掉 %d 项", gc);
            atomic_store(&maint_done, 1);
            dispatch_async(dispatch_get_main_queue(), ^{ import_watch_start(); }); }   // 导入文件夹这时已建好：开始盯着它
        atomic_store(&g_games_repairing, 0);
        int n = img_scan_now();
        dispatch_async(dispatch_get_main_queue(), ^{ if (n > 0 && s2a_shell_refresh) s2a_shell_refresh(); if (r) foreign_report(); if (!r || g_foreign_bytes < (2ull << 30)) img_scan_report(); if (!atomic_load(&g_game_picked)) s2a_cert_scan_import_dirs(); });   // 同一刻只弹一个框：迁移那次先说迁移，认领失败下次回前台重扫再说
    });
}
static void img_scan_if_library(void) { if (g_lib_window) img_scan_bg(); }
// App 开着时玩家往「放这里导入」里放文件也要导入（原先只在冷启动挂库 / 回前台时扫，开着放进去不动）：盯两个导入文件夹和 Documents 本身
//   （文件夹被删了重建也认）的写入 / 改名 / 新增条目，静下来 1 秒再扫一次；还在拷的大文件由导入那边判「还在拷」、拷完最后一次写入再触发。已选游戏后 img_scan_now 不扫
static void import_watch_start(void) {   // 主线程
    static NSMutableDictionary<NSString *, dispatch_source_t> *watch; static dispatch_block_t pending;
    if (!watch) watch = [NSMutableDictionary new];
    char d[1024]; s2a_container_docs(d, sizeof d);
    NSMutableArray<NSString *> *paths = [NSMutableArray arrayWithObject:@(d)];
    for (int i = 0; i < 2; i++) [paths addObject:[@(d) stringByAppendingPathComponent:@(s2a_img_import_dirs[i])]];
    for (NSString *p in paths) {
        if (watch[p]) continue;
        int fd = open(p.fileSystemRepresentation, O_EVTONLY); if (fd < 0) continue;   // 还没有这个名字的文件夹：Documents 那条源在它建出来时会再进来
        dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, (uintptr_t)fd,
            DISPATCH_VNODE_WRITE | DISPATCH_VNODE_EXTEND | DISPATCH_VNODE_LINK | DISPATCH_VNODE_RENAME | DISPATCH_VNODE_DELETE, dispatch_get_main_queue());
        dispatch_source_set_event_handler(src, ^{
            unsigned long ev = dispatch_source_get_data(src);
            if (ev & (DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME)) { dispatch_source_cancel(src); [watch removeObjectForKey:p]; }   // 文件夹本身没了 / 改名：撤掉，下次再挂
            if (pending) dispatch_block_cancel(pending);
            pending = dispatch_block_create(0, ^{ pending = nil; import_watch_start(); img_scan_if_library(); });
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), pending);
        });
        dispatch_source_set_cancel_handler(src, ^{ close(fd); });
        watch[p] = src; dispatch_resume(src);
    }
}
static void library_pack_help(NSString *gid, BOOL again) {   // 主线程
    if (atomic_exchange(&g_pack_checking, 1)) { LOG("游戏库：%s 正在核对资源包，这次点击不重复起", gid.UTF8String); return; }
    __block BOOL done = NO; __block UIAlertController *wait = nil;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.3 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (done) return;
        UIViewController *top = lib_top(); if (!top) return;
        wait = [UIAlertController alertControllerWithTitle:S2AL(@"app.packHelp.checking") message:nil preferredStyle:UIAlertControllerStyleAlert];
        [top presentViewController:wait animated:YES completion:nil];
    });
    CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        img_scan_now();   // 玩家可能刚把 game.img 拷进「文件」：先认领再核
        int st = game_pack_state(gid.UTF8String);
        double ms = (CFAbsoluteTimeGetCurrent() - t0) * 1000;
        dispatch_async(dispatch_get_main_queue(), ^{
            done = YES; atomic_store(&g_pack_checking, 0);
            LOG("游戏库：%s 后台核对资源包 %.0f 毫秒，结果 %d（主线程没有等）", gid.UTF8String, ms, st);
            if (wait && wait.presentingViewController) [wait dismissViewControllerAnimated:NO completion:^{ library_pack_help_show(gid, again, st); }];
            else library_pack_help_show(gid, again, st);
        });
    });
}
static void library_pack_help_show(NSString *gid, BOOL again, int st) {   // 主线程
    if (st == 0 || st == 1) { LOG("游戏库：%s 资源包已就绪，刷新库界面", gid.UTF8String); if (s2a_shell_refresh) s2a_shell_refresh(); return; }
    BOOL broken = st == 3;   // S2A_IMPORT_BROKEN
    LOG("游戏库：%s 资源包%s，给导入说明%s", gid.UTF8String, broken ? "不完整" : "不在机上", again ? "（重新检查后仍不行）" : "");
    NSString *body = broken
        ? S2AL(@"app.library.packBroken")
        : S2AL(@"app.library.packMissing");
    if (again) body = [S2AL(@"app.library.stillMissing") stringByAppendingString:body];
    UIViewController *top = lib_top();
    if (!top) { LOG("【红】游戏库：没有窗口，导入说明画不出来"); return; }
    UIAlertController *a = [UIAlertController alertControllerWithTitle:broken ? S2AL(@"app.library.broken.title") : S2AL(@"app.library.missing.title") message:body preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:S2AL(@"common.later") style:UIAlertActionStyleCancel handler:^(UIAlertAction *x) { (void)x; if (s2a_shell_refresh) s2a_shell_refresh(); }]];
    [a addAction:[UIAlertAction actionWithTitle:S2AL(@"app.packHelp.recheck") style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x; library_pack_help(gid, YES); }]];
    [top presentViewController:a animated:YES completion:nil];
}
void s2a_shell_pick_game_data(const char *gid) {   // 主线程（s2a_shell.h 约定）
    if (!gid || !*gid) return;
    library_pack_help(@(gid), NO);
}
// 同步页完了（同步成 / 失败玩家点开始 / 冲突先不处理）：换回游戏界面，接着走存档管理 → 起宿主（主线程）
static void cloud_sync_done(void *ctx) {
    S2ASceneDelegate *sd = (__bridge S2ASceneDelegate *)ctx;
    sd.cloudSyncing = NO;
    if (sd.previousRoot) { sd.window.rootViewController = sd.previousRoot; sd.previousRoot = nil; }
    // App 正在后台：不起宿主，等系统下次进前台时再起；前台（激活或弹框盖着未激活）就接着起，开跑前由前台激活门等激活
    UISceneActivationState a = sd.window.windowScene.activationState;
    if (a == UISceneActivationStateForegroundActive || a == UISceneActivationStateForegroundInactive) [sd bootFromSceneState];
}
// 主屏小组件点封面（s2arm://play/<游戏号>，11-16 维护者：小组件进 0.5.0；Info.plist 的 CFBundleURLTypes 见 runtime/build.mk）：
//   游戏库还开着 → 同玩家点「开始」（library_play：崩溃恢复 → 起局前检查 → 起宿主；资源不在手机上的款照常说明为什么开不了）；
//   这一进程已经在玩（一进程一款）→ 只把 App 拉到前台，不弹「再开一款」的提示
// 小游戏包 .s2arm（0.6.0 通用壳，电脑「导出」出的两件套之一）：从「文件」或别的 App 用 S2Arm 打开（Info.plist 的文档类型见 runtime/build.mk）→
//   后台导入这一款（pack_apply.c s2a_s2arm_import：程序进 program/、派生件进 资源包/），导好刷新游戏库；game.img 照常放「放这里导入」认领。
//   系统拷进 Documents/Inbox 的那份导完删掉；原地打开的（别处的原文件）不动。失败按码说人话（同导入文件夹认领失败）
int s2a_s2arm_import(const char *proc_docs, const char *src, char *gid_out, size_t gid_n); const char *s2a_s2arm_import_code(void);   // pack_apply.c
static void s2arm_open(NSURL *url) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        BOOL scoped = [url startAccessingSecurityScopedResource];
        char d[1024], gid[17] = ""; s2a_container_docs(d, sizeof d);
        int r = s2a_s2arm_import(d, url.fileSystemRepresentation, gid, sizeof gid);
        NSString *code = r < 0 ? @(s2a_s2arm_import_code()) : nil;
        if (scoped) [url stopAccessingSecurityScopedResource];
        if (!r && [url.path containsString:@"/Documents/Inbox/"]) [NSFileManager.defaultManager removeItemAtURL:url error:nil];
        LOG(".s2arm 打开：%s → %s", url.lastPathComponent.UTF8String, !r ? gid : r > 0 ? "还没拷完，稍后再打开一次" : code.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!r) { if (s2a_shell_refresh) s2a_shell_refresh(); return; }
            if (r < 0 && g_lib_window) top_alert(g_lib_window, S2AL(@"img.scanFailed.title"), install_error_text(code.UTF8String));
        });
    });
}
// 库卡片「导入 .s2arm…」（0.6.0 自签走查第 9、10 条：只放了 .img 的款、0.5 老款，在手机上就能补程序，不只给电脑路线）：
//   文件选择器任何文件都能选（分享来的 .s2arm 扩展名可能被改），选好交 s2arm_open 导入；选的是别的款照样导（日志记一行）
#if !TARGET_OS_TV
@interface S2AS2armPicker : NSObject <UIDocumentPickerDelegate> @property (copy) NSString *gid; @end
@implementation S2AS2armPicker
- (void)documentPicker:(UIDocumentPickerViewController *)c didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)c; NSURL *u = urls.firstObject; if (!u) return;
    LOG("游戏库：%s 的「导入 .s2arm…」选了 %s", self.gid.UTF8String, u.lastPathComponent.UTF8String);
    s2arm_open(u);
}
@end
void s2a_shell_pick_s2arm(const char *gid) {   // 主线程（s2a_shell.h）
    static S2AS2armPicker *delegate; if (!delegate) delegate = [S2AS2armPicker new];
    delegate.gid = gid ? @(gid) : @"";
    UIDocumentPickerViewController *p = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:YES];
    p.delegate = delegate; UIViewController *top = lib_top();
    if (!top) { LOG("【红】游戏库：没有窗口，.s2arm 选择器开不出来"); return; }
    [top presentViewController:p animated:YES completion:nil];
}
#endif
static void widget_open(NSURL *url, void *ctx) {
    if (url.isFileURL && [url.pathExtension.lowercaseString isEqualToString:@"s2arm"]) { s2arm_open(url); return; }
    if (!url || ![url.scheme isEqualToString:@"s2arm"] || ![url.host isEqualToString:@"play"]) return;
    NSString *gid = url.lastPathComponent.uppercaseString;
    if (gid.length != 16 || [gid rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"0123456789ABCDEF"].invertedSet].location != NSNotFound) {
        LOG("主屏小组件：链接里的游戏号不对（%s），不处理", url.absoluteString.UTF8String); return;
    }
    if (!s2a_launch_ctx_container() || atomic_load(&g_game_picked) || !s2a_shell_set_play_handler) {
        LOG("主屏小组件：点了 %s，这一进程已在游戏里（或不是多款容器），只回到前台", gid.UTF8String); return;
    }
    LOG("主屏小组件：点了 %s，同游戏库「开始」", gid.UTF8String);
    library_play(gid.UTF8String, ctx);
}
@implementation S2ASceneDelegate
- (void)scene:(UIScene *)scene openURLContexts:(NSSet<UIOpenURLContext *> *)contexts {
    (void)scene;
    if (s2a_cert_handle_url(contexts.anyObject.URL)) return;   // SideStore 交回签名证书（0.6.0，cert_import.m）
    widget_open(contexts.anyObject.URL, (__bridge void *)self);
}
// 多款容器：根控制器先是游戏库；壳没链库界面（缺省 make ios）时给提示页，不起游戏
- (void)showLibrary {
    if (!s2a_shell_library_vc || !s2a_shell_set_play_handler) {
        UIViewController *vc = [S2AGameVC new]; vc.view.backgroundColor = UIColor.blackColor; g_root = vc.view; self.window.rootViewController = vc;
        LOG("【红】多款容器：这个壳没带游戏库界面（出壳要 make ios S2A_SHELLUI=1），不起游戏");
        s2a_ios_show_notice("exclamationmark.triangle.fill", S2AL(@"app.noLibrary.title").UTF8String, S2AL(@"app.noLibrary.body").UTF8String, 0, 0, 1);
        return;
    }
    NSString *cat = [@(s2a_launch_ctx_bundle_dir()) stringByAppendingPathComponent:@"s2a_catalogue.json"];
    s2a_shell_set_play_handler(library_play, (__bridge void *)self);
    g_snap_play_ctx = (__bridge void *)self;   // 快照：游戏库「回到上次存下的这一刻」走同一条起局路
    g_lib_window = self.window;
    self.window.rootViewController = (__bridge_transfer UIViewController *)s2a_shell_library_vc(cat.fileSystemRepresentation);
    LOG("游戏库：已挂库界面（清单 %s）%s", cat.UTF8String, s2a_process_prewarmed() ? "，预热进程只显示库、不起宿主" : "");
    img_scan_bg();
    // 测试开关（真机自动化点不到库界面的「开始」，同 diag.m S2A_OPEN_SETTINGS）：S2A_LIBRARY_AUTOPLAY=<程序号> 库界面挂上 2 秒后替玩家点这款的开始，
    //   走的就是玩家点开始那条路（library_play：崩溃恢复 → 起局前检查 → 起宿主），用来在多款容器里验证起局前的处理
    { const char *e = getenv("S2A_LIBRARY_AUTOPLAY");
      if (e && *e && !s2a_process_prewarmed()) { char *gid = strdup(e); S2ASceneDelegate *sd = self;
          dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
              LOG("游戏库：测试开关 S2A_LIBRARY_AUTOPLAY=%s，替玩家点开始", gid); library_play(gid, (__bridge void *)sd); free(gid); }); } }
}
// Play 选上之后：换成游戏界面（同单款 willConnect 建的那套：加载页、提示页、安全网提示），再走同一条起宿主路（存档管理 → startHost）
- (void)enterGame {
    UIViewController *vc = [S2AGameVC new]; vc.view.backgroundColor = UIColor.blackColor; g_root = vc.view; g_note_view = nil;
    self.window.rootViewController = vc;
    { const char *e = getenv("S2A_PACK_APPLY_ONLY"); if (!g_msg && !(e && *e == '1')) s2a_loadpage_attach(vc.view); }
    note_build();
    if (g_help_args) help_build(g_help_args[0], g_help_args[1], g_help_args[2]);
    dguard_show_pending();
    [self bootFromSceneState];
}
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session;
#if !TARGET_OS_TV   // 电视没有主屏快捷项
    if (options.shortcutItem) home_shortcut(options.shortcutItem.type, YES, nil);
#endif
    UIWindowScene *ws = (UIWindowScene *)scene;
    // 屏幕亮度（S2A_BRIGHTNESS=0–1，缺省不动；帧率能耗线 10-26）：整机瓦数含屏幕，和小菠萝「亮度 0.5」读数同口径对比时用。
    //   放在建场景这里：sceneDidBecomeActive 非预热路径会早返（10-26 R41 三局都没设上）
#if !TARGET_OS_TV
    { const char *e = getenv("S2A_BRIGHTNESS"); double b = e && *e ? atof(e) : -1;
      if (b >= 0 && b <= 1) { ws.screen.brightness = b; LOG("屏幕亮度设为 %.2f（S2A_BRIGHTNESS）", b); brightness_hold(ws.screen, b); } }
    g_brightness = ws.screen.brightness;
#endif
    self.window = [[UIWindow alloc] initWithWindowScene:ws];
    if (s2a_launch_ctx_container() && !atomic_load(&g_game_picked)) {   // 多款容器：先出游戏库，不建游戏界面、不挂加载页
        [self showLibrary];
#if TARGET_OS_TV
        // 电视上的焦点 / 遥控器只送给 key window；iOS 那套「窗口只显示、不成为 key」不适用（否则画面可见但遥控器完全进不来）
        [self.window makeKeyAndVisible];
#else
        { const char *e = getenv("S2A_NO_KEY_WINDOW"); if (e && !strcmp(e, "0")) [self.window makeKeyAndVisible]; else self.window.hidden = NO; }
#endif
        { NSURL *u = options.URLContexts.anyObject.URL; S2ASceneDelegate *sd = self;   // 冷启动是点主屏小组件来的：库界面挂上后再开（同点「开始」）
          if (u) dispatch_async(dispatch_get_main_queue(), ^{ if (!s2a_cert_handle_url(u)) widget_open(u, (__bridge void *)sd); }); }   // SideStore 交回签名证书冷启动拉起：同开着时
        LOG("iOS 壳：窗口已建 %.0fx%.0f（场景，多款容器游戏库）", self.window.bounds.size.width, self.window.bounds.size.height);
        return;
    }
    UIViewController *vc = [S2AGameVC new]; vc.view.backgroundColor = UIColor.blackColor; g_root = vc.view;
    self.window.rootViewController = vc;
    // 首次加载页 / 简短启动画面（loadpage.m）：点开立即出现；自检不过的提示、只解包模式不进游戏，不挂
    { const char *e = getenv("S2A_PACK_APPLY_ONLY"); if (!g_msg && !(e && *e == '1')) s2a_loadpage_attach(vc.view); }
    // 窗口只显示、不成为 key 窗口（缺省；S2A_NO_KEY_WINDOW=0 回到 makeKeyAndVisible）。10-26 测试设备2 全量系统日志：游戏模式开局弹游戏浮层，浮层收起后键盘事件转发目标回到
    //   我们 key 窗口所在的场景，UIKit 重建键盘代理、拉起听写（AFDictationConnection）查网络，包名禁网即弹「无线数据」框。触摸按落点分发、手柄走 GameController，都不靠 key 窗口
    //   10-26 测试设备1 同包对位：不成为 key 的两局浮层照出、App 进程听写 / 网络检测 0 行；对照局 3 行
#if TARGET_OS_TV
    [self.window makeKeyAndVisible];   // 电视焦点 / 遥控器只送给 key window
#else
    { const char *e = getenv("S2A_NO_KEY_WINDOW"); if (e && !strcmp(e, "0")) [self.window makeKeyAndVisible]; else { self.window.hidden = NO; LOG("iOS 壳：窗口只显示、不成为 key 窗口"); } }
#endif
    note_build();
    if (g_help_args) help_build(g_help_args[0], g_help_args[1], g_help_args[2]);
    dguard_show_pending();
    LOG("iOS 壳：窗口已建 %.0fx%.0f（场景）", self.window.bounds.size.width, self.window.bounds.size.height);
    if (!s2a_host_boot_pending()) [self prepareGameView];
}
- (void)prepareGameView {
    UIViewController *vc = self.window.rootViewController;
    // 浮层开关要赶在 s2a_hud_attach 与建 Metal 设备之前生效；host_run 里按同口径还会再兜底一次（#153）。
    s2a_ios_apply_overlay_settings(NSUserDefaults.standardUserDefaults);
    [vc.view setNeedsUpdateConstraints]; [vc.view layoutIfNeeded];
    settings_attach_runtime();
    if (!g_msg) { s2a_vpad_attach(vc.view); void s2a_dprompt_attach(UIView *root); s2a_dprompt_attach(vc.view); }   // 触屏虚拟手柄（vpad.m）；游戏内提示条（dprompt.m）
    // 四指长按 1.5 秒打开设置；提示页（缺资源 / 导入失败）保留诊断按钮。
    { void s2a_diag_attach(UIViewController *vc); s2a_diag_attach(vc); }
    if (!g_msg) { void s2a_hud_attach(UIView *root); s2a_hud_attach(vc.view); }   // 开发浮层（hud.m，S2A_HUD=1 才开）
    s2a_loadpage_front();   // 加载页压回手柄 / 浮层上面（首帧前不露出手柄）
    if (g_help) [g_root bringSubviewToFront:g_help];   // 老路径说明页先于手柄建，压回最上
    s2a_ios_root_view_ready();   // RenderLoop can now attach below the virtual controls.
}
#if !TARGET_OS_TV
- (void)windowScene:(UIWindowScene *)windowScene performActionForShortcutItem:(UIApplicationShortcutItem *)item completionHandler:(void (^)(BOOL))done {
    (void)windowScene; UIViewController *top = self.window.rootViewController; while (top.presentedViewController) top = top.presentedViewController;
    home_shortcut(item.type, NO, top); done(YES);
}
#endif
// 游戏进行中切到后台：开了云存档、本机进度比云端新就上传（S2ArmShellHost 要后台收尾时间、只上传不下载；云端更新 / 冲突留给下次起局前同步）。
//   宿主还没起（游戏库 / 起局前同步页 / 存档管理页）不传：那时存档没动过
- (void)sceneDidEnterBackground:(UIScene *)scene {
    (void)scene;
    if (s2a_host_boot_pending() || self.cloudSyncing || self.managingSaves || !s2a_shell_cloud_upload_background) return;
    const s2a_launch_ctx *lc = s2a_launch_ctx_get();
    s2a_shell_cloud_upload_background(lc && lc->game_id[0] ? lc->game_id : "");
}
// 场景进前台（还没激活：系统弹框可能盖着）就开始起宿主：加载照做，起游戏主线程前由前台激活门（loadprog.c s2a_load_front_wait）等激活
- (void)sceneWillEnterForeground:(UIScene *)scene {
    s2a_load_front_state(((UIWindowScene *)scene).activationState == UISceneActivationStateForegroundActive ? 2 : 1);
    [self bootFromScene];
}
- (void)sceneDidBecomeActive:(UIScene *)scene {
    (void)scene;
    s2a_load_front_state(2);
    [self bootFromScene];
}
// 起宿主的那条路（游戏库 Play、云同步完、进前台、激活都走这里；领取续体只一次，重复进来是空操作）：门的状态按场景此刻实况设
- (void)bootFromSceneState {
    UISceneActivationState a = self.window.windowScene.activationState;
    if (a == UISceneActivationStateForegroundActive) s2a_load_front_state(2);
    else if (a == UISceneActivationStateForegroundInactive) s2a_load_front_state(1);
    [self bootFromScene];
}
- (void)bootFromScene {
    if (!s2a_host_boot_pending()) return;
    if (s2a_launch_ctx_container() && !atomic_load(&g_game_picked)) return;   // 多款容器：等游戏库 Play（预热进程同样只显示库）
    if (self.managingSaves || self.cloudSyncing) return;
    // iCloud 存档同步（玩家在库设置里开了云存档才有页）：起宿主前、存档管理之前跑一次；有没做完的存档事务时先交存档管理页恢复，这次不同步
    if (!self.cloudSynced && s2a_shell_cloud_sync_vc && !s2a_ios_save_management_requested()) {
        self.cloudSynced = YES;
        const s2a_launch_ctx *lc = s2a_launch_ctx_get();
        void *vc = s2a_shell_cloud_sync_vc(lc && lc->game_id[0] ? lc->game_id : "", cloud_sync_done, (__bridge void *)self);
        if (!vc) LOG("云存档：不同步（没开云存档，或原因见上面「S2Arm 云存档」行）");
        if (vc) {
            LOG("云存档：起宿主前同步");
            self.cloudSyncing = YES;
            self.previousRoot = self.window.rootViewController;
            self.window.rootViewController = (__bridge_transfer UIViewController *)vc;
            return;
        }
    }
    if (s2a_ios_save_management_requested()) {
        self.managingSaves = YES;
        __weak S2ASceneDelegate *weakSelf = self;
        self.window.rootViewController = s2a_ios_save_manager(^{
            S2ASceneDelegate *owner = weakSelf;
            if (!owner) return;
            UIViewController *game = [S2AGameVC new]; game.view.backgroundColor = UIColor.blackColor;
            g_root = game.view; g_note_view = nil; owner.window.rootViewController = game; dguard_show_pending();
            s2a_loadpage_attach(game.view);
            owner.managingSaves = NO; [owner startHost];
        });
        return;
    }
    // 上一局死在前台、这款有上一份存档：起宿主前问要不要回到上一份（不拦；玩家选完再起局）
    if (!self.previousAsked) {
        self.previousAsked = YES;
        int s2a_ios_previous_save_prompt(UIViewController *, void (^)(void));
        __weak S2ASceneDelegate *weakSelf = self;
        if (s2a_ios_previous_save_prompt(self.window.rootViewController, ^{ [weakSelf startHost]; })) return;
    }
    [self startHost];
}
- (void)startHost {
    s2a_boot_entry entry = s2a_host_boot_take();
    if (!entry) return;   // 先领取，再启动；后续激活只走现有恢复路径
    { void s2a_ios_test_save_op(void); s2a_ios_test_save_op(); }   // 测试开关 S2A_TEST_SAVE_OP（ios/save_transfer.m）：游戏已选定、宿主还没起，导出 / 导回一次存档
    int inputError = s2a_ios_input_config_load();
    if (inputError) {   // 11-17 维护者：校验不过不再拒启。默认键位照常起，横幅点名原因（这条只在开发 / 自动化起局设了 S2A_INPUT_CONFIG 时出现）
        LOG("【黄】iOS 输入配置读不懂（%d: %s），这局用默认键位照常启动", inputError, s2a_input_config_error_message(inputError));
        s2a_dprompt_notice(S2AL(@"dprompt.input.bad.title"), [NSString stringWithFormat:S2AL(@"dprompt.input.bad.body"), @(s2a_input_config_error_message(inputError))], 3, 12);
    }
    s2a_log_session_start();
    BOOT_MARK("预热 → 场景激活");
    { uint64_t base, size; s2a_window_get(&base, &size);
      const char *ao = getenv("S2A_PACK_APPLY_ONLY");
      if (ao && *ao == '1') LOG("预热激活：解包专用局，没占客体窗口（vawin.c）");
      else LOG("预热激活：沿用构造函数窗口 %#llx +%#llx（%s）", (unsigned long long)base, (unsigned long long)size, s2a_window_ok() ? "已占整窗" : "占窗失败，交由自检提示"); }
    [(S2AAppDelegate *)UIApplication.sharedApplication.delegate startMonitoring];
    [self prepareGameView];
    { void s2a_iml_probe_check(void); s2a_iml_probe_check(); }   // D：首启 IML 复核（iml_probe.m）：核实 IML 权限是否保留，丢了打日志并写 diag/iml_probe.json
    LOG("预热启动：前台已激活，启动宿主");
    // C# 启动含大块栈上结构；不能用 iOS pthread 默认的 512 KB 栈（HID SharedMemory.Create 会越界）。
    pthread_t th; pthread_attr_t at; int rc = pthread_attr_init(&at);
    if (rc) die("预热宿主线程属性初始化失败（%d）", rc);
    rc = pthread_attr_setstacksize(&at, 8 << 20);
    if (rc) die("预热宿主线程 8 MB 栈设置失败（%d）", rc);
    rc = pthread_create(&th, &at, deferred_host_thread, (void *)entry); pthread_attr_destroy(&at);
    if (rc) die("预热宿主线程起不来（pthread_create=%d）", rc);
    pthread_detach(th);
}
@end
// 界面（UIKit）已在主线程跑着 = 预热路径：s2a_front_run 会立即返回、调用的宿主线程接着往下走（资源导入闸门据此在起线程前就定「有没有人等」）
int s2a_front_is_running(void) { return atomic_load(&g_front_running); }
void s2a_front_run(void (*start_host)(void)) {
    // 预热路径已在主线程运行 UIKit；宿主工作线程的提示页 / 导入 / 正常收尾不能再次进入 UIApplicationMain。
    if (atomic_exchange(&g_front_running, 1)) return;
    if (start_host && s2a_host_boot_defer(start_host, 1) < 0) die("宿主启动续体冲突");
    @autoreleasepool { char *argv[] = {"S2Arm", NULL}; UIApplicationMain(1, argv, nil, NSStringFromClass([S2AAppDelegate class])); }
}

// 直连图形层（graphics/nvn hle_nvn.c）读设备热状态 / 低电量模式的强定义：那边只有弱定义（热状态 -1、低电量 0，给 Mac 与单测用），
//   iOS 壳此前没给强定义，热状态帧率帽和帧率台阶里按热状态 / 低电量的判断在 iPhone 上都拿到默认值、等于没生效（10-26 帧率能耗线移植热状态帧率帽时查出）。
//   NSProcessInfo 的这两个属性可在任意线程读
double dm_brightness_now(void) { return g_brightness; }
int dm_thermal_now(void) { return (int)NSProcessInfo.processInfo.thermalState; }
// 界面语言（runtime/include/s2a_ui_text.h，#316）：C 层递上来的提示 / 横幅按它取中文原宏或 _EN；判据同 S2AL 字符串表（ios/s2a_l10n.h）。盖过各 .c 的弱缺省
int s2a_ui_zh(void) { return s2a_l10n_zh(); }
// 界面语言玩家选过（App 设置「界面语言」，S2AUILanguageKey）：进程一起来先把本 App 的 AppleLanguages 设成它（字符串表 / 游戏库 / 系统弹框随之），跟随系统就清掉覆盖。
//   游戏语言不经界面语言传（11-13：界面只有简 / 繁 / 英，日 / 韩语设备会被落成英语、导入的 Switch 存档名字乱码）：服务层缺省读本 App 的首选语言列表
//   （Host.cs PickLanguage → SystemLocale.FromPreferred），选过界面语言时列表第一项就是它，跟随系统时是设备系统语言；玩家逐款选的游戏语言走 S2A_SYSTEM_LANGUAGE（settings.m）
__attribute__((constructor)) static void s2a_ui_language_env(void) { @autoreleasepool { (void)s2a_l10n_apply_choice(); } }
int dm_low_power_now(void) { return NSProcessInfo.processInfo.lowPowerModeEnabled ? 1 : 0; }
