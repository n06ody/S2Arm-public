// 安卓侧直连安全网接线：共用 runtime/src/dguard.c 的武装 / 生命周期出口（接口与口径见
//   runtime/include/s2a_dguard.h）。本头只放安卓自己的薄包装，判定全部在共用件里。
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// NVN 直连已提交、客体代码还没跑之前调（runtime.c 的 direct 分支，起局语义同 iOS
//   host_main.c 的 begin/arm 时机）：debug.s2a.dguard=0 时整体不武装（起局时读一次属性）。
//   direct_abi = 已提交直连表的 ABI 版本（frozen->table.version），写进诊断的翻译链一栏
//   （安卓离线库按 <abi> 分代；平台侧不引图形内部头，版本号由调用方给）。
//   连续失败满限时：无人值守局（S2A_AUTOPLAY，同 iOS failed_session 口径）返回非 0 收局、
//   不 arm；有人局弹提示，玩家选「仍然启动」后照旧 arm。
//   返回 0 = 继续起局；非 0 = 本局应收（按 runtime 的 failed 通道收尾）。
int s2a_android_dguard_launch(uint64_t program_id, const char *game_data_dir, unsigned direct_abi);
// 前后台切换（bridge.cpp setForeground）：前台满 60 秒清零只数前台时间，进后台停表。
void s2a_android_dguard_foreground(int foreground);
// 前台收到内存告警（Java onTrimMemory 经 JNI）：按共用件口径记一次失败证据。
void s2a_android_dguard_memory_warning(void);
// 宿主版本号（Java BuildConfig 经 JNI 在起局前传一次）：dguard 记录用它区分宿主，
// 换了宿主从 0 数（同 iOS Info.plist S2ACommit 的口径；不传记「-」，同包换版本不复位）。
void s2a_android_host_build_set(const char *build);
#ifdef __cplusplus
}
#endif
