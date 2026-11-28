// 直连模式的系统层接点（10-24 双轨，原生层线建；约定见 wire/silksong-c 分支 docs/设计_直连架构.md §12.2）。
// 直连线在 graphics/nvn/host_adapt.c 里把直连层的宿主名字（dm_fault_hook_register 等）适配到这里。每样独占资源只有一个主人：
//   缺页信号：crash.c 两条分支（游戏代码 / 宿主代码）都先问这里登记的钩子，再走 S2Arm 写跟踪；
//   写前通知：S2Arm 所有「宿主即将写游戏内存」（C# GuestMemory 写、svc 拷出、内核替游戏写缓冲）都先经 graphics/track 的三个写入口，登记的回调挂在那里；
//   写保护：直连模式下 graphics/track 不保护任何页（s2a_track_protected_pages() 收局打出，直连局恒 0）。
// 每个登记口只收一个回调：重复登记同一个无害，登记第二个不同的回调直接报错退出（两个主人就是错）
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// 缺页钩子：信号上下文里调。1 = 已处理或已改道；0 = 未领；负值 = 已领但无法安全处理，走原崩溃路径。
// 钩子本身须信号安全，不分配、不加锁。
typedef int (*s2a_fault_hook_fn)(void *addr, int is_write, void *uctx);
void s2a_fault_hook_register(s2a_fault_hook_fn fn);
int s2a_direct_fault(void *addr, int is_write, void *uctx);
// 写前回调：宿主写 [addr, addr+len) 之前调，正常上下文。回写回调期间也会调到：回调里不许再写游戏内存、不许拿 RegionHandle 的锁
typedef void (*s2a_guest_write_prep_fn)(uint64_t addr, uint64_t len);
void s2a_guest_write_prep_register(s2a_guest_write_prep_fn fn);
// 崩溃现场：crash.c 打完线程表后调一次（对应上游 crash_set_wait_dump：打出各等待点）
void s2a_crash_wait_dump_register(void (*fn)(void));
// 资源行：地址余量巡检（每 30 秒）与收局时调（对应上游 dm_res_report_register）
void s2a_res_report_register(void (*fn)(void));
// 直连现编收局汇总：normal=1 为正常出口、0 为 1.5 秒超时出口。
// 回调只读成功/失败/跳过原子计数并打印，不等工作线程。
void s2a_direct_summary_register(void (*fn)(int normal));
void s2a_direct_summary(int normal);
// 阻塞放核：直连层里游戏线程要等 GPU / 上屏 / 管线前调 begin、等完调 end（runtime/src/thread.c），放开 / 取回虚拟核（S2A_CORES）；可嵌套，非游戏线程空操作
void s2a_core_block_begin(void);
void s2a_core_block_end(void);
// 回客体代码前（entry.S xs_entry 里 blr 之后调，只有切过栈那条路径）：惰性取核时点，S2A_LAZY_CORE 关时空转。
// 不挂尾跳分支：无槽的纯宿主线程 / 宿主→宿主嵌套都不回客体代码。
void s2a_guest_return(void);

#ifdef __cplusplus
}
#endif
