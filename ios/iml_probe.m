// 首启 IML 复核（D）：App 启动时进程内读自身签名里真实生效的权限，核实「提高内存上限」是否保留。
//   真值：进程自身生效的权限（SecTaskCopyValueForEntitlement(SecTaskCreateFromSelf)，内核按进程签名定档读的就是它）。
//     10-30 LiveContainer 实测：游戏以 dylib 跑在 LC 宿主进程里，权限按宿主签名定档（宿主带 IML → 6144 MB / 64 GB），而游戏可执行文件自己的签名里
//     没有权限槽（codesign 给改写后的 dylib 签权限会悄悄丢）——原先读主程序 Mach-O 签名（imlent.c s2a_ent_has）在 LC 下误报「丢了 IML」。
//     读进程权限失败（API 返回错误）时才退回读主程序签名；描述文件允许 ≠ 签名带上，iOS 沙盒也起不了 codesign / security，不读那两条路。
//   佐证：本次宿主实际拿到的地址上限（vawin.c s2a_va_top）与 2^39 以下最大一块（s2a_window_need 的 fit，构造函数占窗时已量好，这里不重新预留）。
//     带 IML 应是大地址档（上限 63 GiB、最大一块约 51 GiB）；不带约 15–17 GiB / 5.4 GiB（R02 布局_系统版本x权限.tsv）。大地址档 EVA 等权限也给，只作佐证。
//   结果写 Documents/diag/iml_probe.json（Mac 前端「拉回诊断」读 has_iml_ent，见 pack/jobs/diag.py）；丢了打日志一行【红】，游戏照常进（不中止）。
//   调用点：app.m s2a_front_run → s2a_iml_probe_check()（启动后立刻，不等进游戏）。
#import <Foundation/Foundation.h>
#include "s2a_docs.h"   // s2a_container_docs：容器文档根（Apple TV 在 Library/Caches/S2Arm）
#import "s2a_l10n.h"
#include <mach-o/ldsyms.h>
#include "native.h"
#include <CoreFoundation/CoreFoundation.h>
// Security.framework 里有、iOS 公开头没声明（LiveContainer 同用法）。运行时取（UIKit 已把 Security 带进进程）：主程序链接行不另加框架
#include <dlfcn.h>
typedef struct __SecTask *SecTaskRef;
// 进程自身是否带某项权限：1 带、0 不带、-1 读不到（API 出错）
static int proc_ent_has(const char *key) {
    SecTaskRef (*create)(CFAllocatorRef) = (SecTaskRef (*)(CFAllocatorRef))dlsym(RTLD_DEFAULT, "SecTaskCreateFromSelf");
    CFTypeRef (*copy)(SecTaskRef, CFStringRef, CFErrorRef *) = (CFTypeRef (*)(SecTaskRef, CFStringRef, CFErrorRef *))dlsym(RTLD_DEFAULT, "SecTaskCopyValueForEntitlement");
    if (!create || !copy) return -1;   // 取不到：调用方退回读主程序签名
    SecTaskRef t = create(NULL); if (!t) return -1;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8); CFErrorRef err = NULL;
    CFTypeRef v = copy(t, k, &err);
    int r = err ? -1 : v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)v) ? 1 : 0;
    if (v) CFRelease(v); if (err) CFRelease(err); CFRelease(k); CFRelease(t);
    return r;
}

#define IML_KEY "com.apple.developer.kernel.increased-memory-limit"
#define GIB (double)(1ull << 30)

// 这次安装的进程有没有「提高内存上限」权限：1 带、0 不带（丢了）、-1 读不到；-2 = 还没读。首启复核读一次记下，诊断询问（diag.m）复用，没读过就现读
static int g_iml_ent = -2;
static int iml_ent(void) {
#if TARGET_OS_TV
    // tvOS 没有这项权限（Xcode 能力表 Increased Memory Limit 只列 iOS / visionOS）：不适用，按「读不到」不判，
    //   库顶提示条、游戏里提示条、诊断询问都不出（#1456：电视上提示「重新签名带上权限」是一条做不到的路）
    g_iml_ent = -1;
#endif
    if (g_iml_ent == -2) { int e = proc_ent_has(IML_KEY); g_iml_ent = e >= 0 ? e : s2a_ent_has(&_mh_execute_header, IML_KEY); }
    return g_iml_ent;
}
// 上次异常退出后问「把上次的诊断发给我们？」时用（diag.m）：确定丢了才算（读不到不报，防误报）
int s2a_iml_lost(void) { return iml_ent() == 0; }
// 「丢了这项权限」每次打开只说一处、同一时刻只出一处（11-16）。会说的有四处：自检提示页（只差内存上限时「仍要启动」那页，host_main.c）、
//   上次异常退出后的诊断询问（diag.m）、游戏库顶上的提示条（s2a_shell_iml_notice）、游戏里的顶部提示条（s2a_iml_probe_check）。
//   谁先真显示谁说（s2a_iml_claim 取到 1 才说，取到 0 说明别处已说过）；只在真要显示时取，取了没显示出来就两处都不说
static int g_iml_told;
int s2a_iml_claim(void) { return s2a_iml_lost() && !__atomic_exchange_n(&g_iml_told, 1, __ATOMIC_SEQ_CST); }
// 诊断询问这次会问（diag.m，上次没正常退出；进游戏画面时定）：由它说，游戏里提示条让给它，免得提示条和询问框同时在屏幕上
int s2a_diag_will_ask(void) __attribute__((weak_import));
static int diag_will_ask(void) { return s2a_diag_will_ask && s2a_diag_will_ask(); }
// 游戏库顶上的提示（S2ArmShellHostModel，s2a_shell.h）：确定丢了、这次还没别处说过才给标题与一句短正文（详情在库里的「了解详情」页），调用方 free。
//   库里真画出这条提示时（s2a_shell_iml_notice_seen）记下（11-16 维护者：打开 App 就提示）
void s2a_shell_iml_notice_seen(void) { __atomic_store_n(&g_iml_told, 1, __ATOMIC_SEQ_CST); }
char *s2a_shell_iml_notice(void) {
    @autoreleasepool {
        if (!s2a_iml_lost() || __atomic_load_n(&g_iml_told, __ATOMIC_SEQ_CST)) return NULL;
        NSData *d = [NSJSONSerialization dataWithJSONObject:@{@"title": S2AL(@"iml.lost.title"), @"body": S2AL(@"iml.lost.short")} options:0 error:nil];
        if (!d) return NULL;
        char *p = malloc(d.length + 1); if (!p) return NULL;
        memcpy(p, d.bytes, d.length); p[d.length] = 0; return p;
    }
}

static void _write_result(int ent, uint64_t fit, uint64_t top, int jumbo) {
    NSString *docs = ({ char _d[1024]; s2a_container_docs(_d, sizeof _d); @(_d); });
    NSString *dir = [docs stringByAppendingPathComponent:@"diag"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSDictionary *rec = @{
        @"has_iml_ent":  ent < 0 ? NSNull.null : @(ent == 1),   // null = 读不到签名里的权限（不当缺失）
        @"iml_key":      @(IML_KEY),
        @"window_gb":    fit ? @(round(fit / GIB * 10) / 10) : NSNull.null,   // 2^39 以下最大一块
        @"va_top_gb":    top ? @(round(top / GIB * 10) / 10) : NSNull.null,   // 地址上限
        @"va_jumbo":     jumbo < 0 ? NSNull.null : @(jumbo == 1),
        @"bundle_id":    NSBundle.mainBundle.bundleIdentifier ?: @"",
        @"ios":          @((int)NSProcessInfo.processInfo.operatingSystemVersion.majorVersion),
        @"ts":           @((long long)[[NSDate date] timeIntervalSince1970]),
    };
    NSData *json = [NSJSONSerialization dataWithJSONObject:rec options:NSJSONWritingPrettyPrinted error:nil];
    [json writeToFile:[dir stringByAppendingPathComponent:@"iml_probe.json"] atomically:YES];
}

void s2a_iml_probe_check(void) {
    @autoreleasepool {
        int ent = proc_ent_has(IML_KEY), from_proc = ent >= 0;
        if (!from_proc) ent = s2a_ent_has(&_mh_execute_header, IML_KEY);   // 进程权限读不到才退回读主程序签名
#if TARGET_OS_TV
        ent = -1;   // tvOS 没有这项权限：不判、不提示（同 iml_ent）
#endif
        g_iml_ent = ent;
        uint64_t need = 0, fit = 0, top = s2a_va_top(); s2a_window_need(&need, &fit);
        int jumbo = s2a_va_jumbo(top);
        _write_result(ent, fit, top, jumbo);
        const char *va = jumbo < 0 ? "地址上限读不到" : jumbo ? "大地址档" : "小地址档";
        if (ent < 0)
            LOG("[IML] 首启复核：读不到自身签名里的权限（无签名段 / 无权限槽），不判；%s，上限 %.1f GB、最大一块 %.1f GB", va, top / GIB, fit / GIB);
        else if (ent)
            LOG("[IML] 首启复核：%s带提高内存上限权限；%s，上限 %.1f GB、最大一块 %.1f GB%s", from_proc ? "进程" : "签名", va, top / GIB, fit / GIB,
                jumbo == 0 ? "【黄】带权限却只拿到小地址档，与 R02 不符" : "");
        else
            LOG("【红】[IML] 首启复核：这台机上装的包签名丢了「提高内存上限」权限（%s）；%s，上限 %.1f GB、最大一块 %.1f GB。"
                "游戏继续运行，超出窗口走远端翻译；请在电脑上的 S2Arm 点游戏库第一格「S2Arm」里的「续签 S2Arm」重装，或用 Xcode / AltStore Classic 2.2 或更新版重签后重装（或 GetMoreRam 给 App ID 补权限后重装）",
                IML_KEY, va, top / GIB, fit / GIB);
        // 玩家也要知道（审计 P2-3：原先只写日志）：不挡操作（dprompt.m 顶部提示条，起局 3 秒后）。单款壳没有游戏库，每次打开都在游戏里提示（11-16 维护者：别永久静默）；
        //   到点再定：这次已有别处说过（库顶提示条 / 自检提示页 / 诊断询问）或诊断询问还要问，就不弹
        if (ent == 0)
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                if (diag_will_ask() || !s2a_iml_claim()) return;
                void s2a_dprompt_notice(NSString *title, NSString *body, double delay, double seconds);
                s2a_dprompt_notice(S2AL(@"iml.lost.title"), S2AL(@"iml.lost.body"), 0, 15);
            });
    }
}
