// 安卓侧直连安全网接线（接口见 dguard_android.h）：只放安卓自己的薄包装——属性开关、
//   宿主版本号、提示出口；失败计数 / 判据 / 记录文件全部在共用 runtime/src/dguard.c。
//   提示出口按弱符号取：生产由 bridge.cpp 经 JNI 弹窗，宿主测试给桩，独立探针没链就
//   只记日志照走（与 iOS 提示页同口径：选「仍然启动」才返回）。
#include "dguard_android.h"
#include "s2a_dguard.h"
#include "s2a_unattended.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#define DG_LOG(...) __android_log_print(ANDROID_LOG_INFO, "S2Arm", __VA_ARGS__)
#else
#define DG_LOG(...) ((void)0)
#endif

extern void s2a_android_dguard_prompt(const char *diag) __attribute__((weak));
static char g_host_build[64] = "-";

static uint64_t now_ns(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? 0 : (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

#ifdef S2A_DGUARD_TEST
extern int s2a_dguard_test_enabled(void);
#define gate_enabled s2a_dguard_test_enabled
#else
// 默认开：debug.s2a.dguard=0 起局前关掉整个安全网（读一次，不缓存——每局只调一次）。
static int gate_enabled(void) {
#if defined(__ANDROID__)
    char value[PROP_VALUE_MAX] = {0};
    return __system_property_get("debug.s2a.dguard", value) <= 0 || strcmp(value, "0") != 0;
#else
    return 1;
#endif
}
#endif

void s2a_android_host_build_set(const char *build) {
    if (build && *build) snprintf(g_host_build, sizeof g_host_build, "%s", build);
}

int s2a_android_dguard_launch(uint64_t program_id, const char *game_data_dir, unsigned direct_abi) {
    if (!gate_enabled()) { DG_LOG("直连安全网：debug.s2a.dguard=0，本局不武装"); return 0; }
    int fails = s2a_dguard_begin(program_id, s2a_dguard_pack_fp(game_data_dir), g_host_build);
    if (fails >= S2A_DGUARD_LIMIT) {
        // 起局时主程序还没装载，构建号取不到就记「-」；翻译链版本比构建号更能定位出包代际
        // （安卓离线库按 shaders/vulkan/direct/<abi>/<profileHash> 分代，调用方传已提交的 table.version；
        //   iOS/Mac 那条 DMSH_CHAIN 是 NVN 着色器库链号，安卓的对应物就是这个 ABI 号）
        char chain[32], diag[2048];
        snprintf(chain, sizeof chain, "vulkan-direct abi %u", direct_abi);
        s2a_dguard_diag(diag, sizeof diag, NULL, chain);
        DG_LOG("【红】直连安全网：本款直连已连续失败 %d 次，起局前提示玩家更新后重新出包；诊断：\n%s", fails, diag);
        if (s2a_unattended()) {
            DG_LOG("【红】直连安全网：无人值守局满限收局（S2A_AUTOPLAY），记录留着");
            return fails;   // 同 iOS failed_session(S2A_INIT_FAIL_E_DGUARD_LIMIT)：本局不起、记录不清
        }
        if (s2a_android_dguard_prompt) s2a_android_dguard_prompt(diag);
        else DG_LOG("直连安全网：提示出口没链（探针 / 宿主测试外不该到这里），按仍然启动继续");
    }
    s2a_dguard_arm(now_ns());
    return 0;
}

void s2a_android_dguard_foreground(int foreground) {
    s2a_dguard_background(!foreground, now_ns());
}

void s2a_android_dguard_memory_warning(void) {
    s2a_dguard_evidence("内存告警", 0);
}
