#pragma once
#include "wsi_lease.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Cold-start only. Once prepare freezes a session, different choices are EBUSY.
int s2a_android_vk_select(int direct);
int s2a_android_vk_session_prepare(const struct s2a_wsi_api *, const char *game_dir, const char *state_dir);
int s2a_android_vk_skip_module(const char *name);
int s2a_android_vk_set_layer_query(int64_t (*query)(void));
int s2a_android_vk_session_stop(void);
uint64_t s2a_android_vk_present_count(void);
uint64_t s2a_android_nvn_present_count(void);
// 本局重定位是否问过 nvnBootstrapLoader（NVN 客体事实）。runtime 用它与 vk-direct 实验开关对账。
int s2a_android_vk_nvn_guest(void);
#ifdef __cplusplus
}
#endif
