// 游戏内提示条（iOS 壳 ios/dprompt.m、Mac 游戏窗口 macos/front_mac.m）的共用文案与存档原子提交判定。
//   11-07 拆通用模式：原「通用 → 直连就绪提示重启」状态机与「切到高性能模式 / 切回通用模式」设置入口一并删掉。
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// 两端共用的文案（中文、短；每条英文同名 <宏>_EN，iOS 宿主按 App 语言取，与规格 en 逐字一致）
#define S2A_DPROMPT_OK             "知道了"
#define S2A_DPROMPT_OK_EN          "Got it"
// 存档不能原子提交（审计 P2-13 / P2-45）的提示：标题两端相同；正文前半句同一口径，建议按平台给（Mac 能挪游戏库，iPhone 只能等存完再退）
#define S2A_DPROMPT_SAVE_ATOMIC_TITLE "存档提醒"
#define S2A_DPROMPT_SAVE_ATOMIC_TITLE_EN "Save warning"
#define S2A_DPROMPT_SAVE_ATOMIC_MAC   "当前磁盘不能安全保存：请把游戏库挪到本机磁盘。保存过程中强退，可能留下写了一半的存档。"
#define S2A_DPROMPT_SAVE_ATOMIC_MAC_EN "This disk can't save safely: move the game library to this Mac's disk. Force-quitting while saving may leave a half-written save."
#define S2A_DPROMPT_SAVE_ATOMIC_IOS   "当前存储不能安全保存：保存过程中强退，可能留下写了一半的存档。请等游戏存完再退出。"
#define S2A_DPROMPT_SAVE_ATOMIC_IOS_EN "This storage can't save safely: force-quitting while saving may leave a half-written save. Wait for the game to finish saving before you quit."
// 触屏提示（ios/vpad.m，11-04 维护者：脑锻炼标题页只认触屏；11-09 维护者 删掉屏幕上方的「键 / 触」切换钮，切换在齿轮菜单「按键布局」）：首次提示条带「切到触屏」一点就切
#define S2A_DPROMPT_VPAD_TOUCH_TITLE   "这款游戏能用触屏"
#define S2A_DPROMPT_VPAD_TOUCH_TITLE_EN "This game supports touch"
#define S2A_DPROMPT_VPAD_TOUCH_BODY    "要点游戏自己的菜单，就切到触屏。左上角齿轮 → 按键布局，随时可以切回按键。"
#define S2A_DPROMPT_VPAD_TOUCH_BODY_EN "To tap the game's own menus, switch to touch. You can switch back to buttons anytime from the gear in the top-left corner → Button layout."
#define S2A_DPROMPT_VPAD_TOUCH_ACCEPT  "切到触屏"
#define S2A_DPROMPT_VPAD_TOUCH_ACCEPT_EN "Switch to touch"
#define S2A_DPROMPT_VPAD_TOUCH_DECLINE "先不用"
#define S2A_DPROMPT_VPAD_TOUCH_DECLINE_EN "Not now"
// 游戏声明触屏配置时自动切到仅触屏（ios/touch_config.m，只改本次运行）：提示一条，指齿轮菜单「按键布局」
#define S2A_DPROMPT_VPAD_TOUCH_AUTO    "已切到触屏操作：这款游戏用触屏点菜单。左上角齿轮 → 按键布局，可以切回按键。"
#define S2A_DPROMPT_VPAD_TOUCH_AUTO_EN "Switched to touch controls: this game uses touch for its menus. You can switch back to buttons from the gear in the top-left corner → Button layout."
// 切换游戏（一个进程只跑一款）：iOS 多款容器在局内设置页 / 齿轮菜单点它，确认后照退出流程存档收尾、结束进程，再点开 App 是游戏库（ios/app.m）；
//   Mac 宿主菜单栏点它（不二次确认，同菜单栏「退出」），收尾后启动器回到游戏库（macos/front_mac.m → pack/jobs/build.py launch → 启动器）。取消按钮用 S2A_DPROMPT_HOME_QUIT_DECLINE
#define S2A_DPROMPT_SWITCH_GAME_TITLE  "切换游戏"
#define S2A_DPROMPT_SWITCH_GAME_TITLE_EN "Switch Games"
#define S2A_DPROMPT_SWITCH_GAME_ACTION "结束这局，回到游戏库"
#define S2A_DPROMPT_SWITCH_GAME_ACTION_EN "End this game and return to the library"
#define S2A_DPROMPT_SWITCH_GAME_NOTE   "S2Arm 会关闭，再点开就回到游戏库。"
#define S2A_DPROMPT_SWITCH_GAME_NOTE_EN "S2Arm closes; open it again to return to the game library."
#define S2A_DPROMPT_SWITCH_GAME_BODY   "请先在游戏里存档。确认后这局结束、S2Arm 关闭，再点开 S2Arm 就回到游戏库，选另一款开始。退出前会等游戏把存档写完；没在游戏里存的进度会丢。"
#define S2A_DPROMPT_SWITCH_GAME_BODY_EN "Save in the game first. When you confirm, this game ends and S2Arm closes; open S2Arm again to return to the game library and pick another game. Quitting waits for the game to finish writing its save; progress you haven't saved in the game will be lost."
#define S2A_DPROMPT_SWITCH_GAME_ACCEPT "已存档，切换"
#define S2A_DPROMPT_SWITCH_GAME_ACCEPT_EN "Saved, switch"
// 实体手柄退出游戏（#276，macos/front_mac.m；按住 Home 2 秒或「+」「-」同按 2 秒弹出，按 A 退出、按 B 回到游戏）
#define S2A_DPROMPT_HOME_QUIT_TITLE   "退出游戏？"
#define S2A_DPROMPT_HOME_QUIT_TITLE_EN "Quit game?"
#define S2A_DPROMPT_HOME_QUIT_BODY    "按 A 退出，按 B 回到游戏。退出前会等游戏把存档写完；没在游戏里存的进度会丢。"
#define S2A_DPROMPT_HOME_QUIT_BODY_EN "Press A to quit or B to return to the game. Quitting waits for the game to finish writing its save; progress you haven't saved in the game will be lost."
#define S2A_DPROMPT_HOME_QUIT_ACCEPT  "退出"
#define S2A_DPROMPT_HOME_QUIT_ACCEPT_EN "Quit"
#define S2A_DPROMPT_HOME_QUIT_DECLINE "回到游戏"
#define S2A_DPROMPT_HOME_QUIT_DECLINE_EN "Back to game"
// 存档不能原子提交（审计 P2-13 界面半边、P2-45 iPhone 半边；Mac 与 iOS 共用这一个判定）：服务层导出 S2A_SaveAtomicState（services SaveTransferExports.cs，
//   可写存档退回逐字节复制的原因位：1 没登记原语 / 2 卷不能克隆或原子交换 / 4 工作目录准备失败）。前端主线程每秒调一次；
//   游戏会话跑起来之前不问服务层（iOS 的提示定时器早于宿主线程起）；第一次读到非 0 返回原因位，只返回一次，之后恒 0。
//   没有这条导出：安卓（链接期弱引用）在会话跑起来后返回 S2A_SAVE_ATOMIC_UNKNOWN（负数，每次都返回，红行只打一次），
//   它是「状态未知」，既不是已确认安全的 0，也不是原因位；Apple（dlsym）缺席仍恒 0
#define S2A_SAVE_ATOMIC_UNKNOWN (-1)
int s2a_save_atomic_poll(void);
#ifdef __cplusplus
}
#endif
