// 存档不能原子提交的判定（接口见 runtime/include/s2a_direct_prompt.h；iOS 壳与 Mac 游戏窗口共用）。只在前端主线程上调。
#include "native.h"
#include "s2a_direct_prompt.h"
#include "s2a_session.h"
#include <dlfcn.h>

int s2a_save_atomic_poll(void) {
    static int looked, warned, unknown; static int (*state)(void);
    if (warned || s2a_host_session_state() != S2A_SESSION_RUNNING) return 0;
#if defined(__ANDROID__)
    // 安卓：服务层静态并入同一个 DSO（--exclude-libs,ALL），dlsym 找不到隐藏导出、恒为 NULL，提示从不出现。改链接期弱引用：
    //   出包的单库里由归档解析（core-link 审计守：归档定义的导出在库里仍未解析就失败）；不带服务层的门禁链接里为空，照旧不提示
    { extern int S2A_SaveAtomicState(void) __attribute__((weak)); if (!looked) { looked = 1; state = S2A_SaveAtomicState; } }
    // 缺席不等于「已确认能原子提交」（#961）：返回未知、红行只打一次；不提示、不挡游戏
    if (!state) {
        if (!unknown) { unknown = 1; LOG("【红】存档：服务层没有 S2A_SaveAtomicState（未链服务层或缺这条导出），存档能否原子提交未知，不提示"); }
        return S2A_SAVE_ATOMIC_UNKNOWN;
    }
#else
    if (!looked) { looked = 1; state = (int (*)(void))dlsym(RTLD_DEFAULT, "S2A_SaveAtomicState"); }
#endif
    int bits = state ? state() : 0;
    if (!bits) return 0;
    warned = 1;
    LOG("存档：不能原子提交（原因位 %d：1 没登记原语 / 2 卷不能克隆或原子交换 / 4 工作目录准备失败），提示玩家", bits);
    return bits;
}
