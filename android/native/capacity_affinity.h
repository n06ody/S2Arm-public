// SPDX-License-Identifier: Apache-2.0
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
enum s2a_capacity_role {
    S2A_CAPACITY_REPLAY, S2A_CAPACITY_RECORD, S2A_CAPACITY_GUEST, S2A_CAPACITY_BACKGROUND, S2A_CAPACITY_AUDIO,
};
void s2a_capacity_enter(int role);
void s2a_capacity_leave(int role);
void s2a_capacity_nvk_record_enter(void);
void s2a_capacity_guest_enter(void);
void s2a_capacity_audio_enter(void);
void s2a_capacity_child(void);   // 新建的游戏线程进游戏代码前：继承来的若是某个角色收窄后的掩码，放回原来的宽掩码
void s2a_capacity_foreground(int foreground);
void s2a_capacity_check(void);
void s2a_capacity_stop(void);
#if defined(S2A_CAPACITY_TEST)
void s2a_capacity_test_reset_mode(void);
#endif
// Execute a synchronous operation on a verified wide child when
// its caller is one of the narrowed roles. 1 = executed, 0 = no action,
// negative errno = helper failed before invoking operation.
int s2a_capacity_call_wide(int (*operation)(void *), void *argument, int *result);

#ifdef __cplusplus
}
#endif
