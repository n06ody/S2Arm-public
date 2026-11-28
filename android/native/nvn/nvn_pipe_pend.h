// 管线首用待定表（纯逻辑，无 Vulkan / 无设备）：按键记会话内「后台建管线」的状态。
// 逐条对齐 iOS graphics/nvn/gfx/nvn/nvn_mtl.m 的 pso_pend_state / pso_pend_take：
//   · NONE/COMPILING/READY/FAILED 四态（iOS pso_pend_state 的 0/1/2 + 失败终态）；
//   · 同键合并：同一次创建一份，等待者等同一份结果；
//   · 有界等待（iOS pso_pend_take 的 item 上限由调用方给 wait_ms）；
//   · late / stuck 项标（跨笔、跨帧都挡：late 只挡可跳绘制，stuck 挡一切）；
//   · 满表淘汰最久未触的已终态项；全是 COMPILING 才如实 FULL（不掩盖）。
// 线程：一把互斥 + 每项一个条件变量；等待期间释放互斥；本表从不回调调用方、不与调用方锁嵌套。
#pragma once
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVK_PP_MAX 256
#define NVK_PP_KEY_MAX 2560     // 必须 >= sizeof(nvn_gpu_pipe_desc)（实测 2360）；小于则整表停用（不合并、不越界）
#define NVK_PP_WHY 192          // 与 nvn_gpu_err.why 同长

enum { NVK_PP_NONE = 0, NVK_PP_COMPILING = 1, NVK_PP_READY = 2, NVK_PP_FAILED = 3 };  // 状态快照
enum { NVK_PP_ENTER_FULL = 0, NVK_PP_ENTER_BUILDER = 1, NVK_PP_ENTER_FOLLOW = 2 };   // enter 返回
enum { NVK_PP_WAIT_READY = 0, NVK_PP_WAIT_TIMEOUT = 1, NVK_PP_WAIT_FAILED = 2, NVK_PP_WAIT_ABSENT = 3 };

struct nvk_pp_ent {
    pthread_cond_t cv;
    uint8_t key[NVK_PP_KEY_MAX];
    void *obj;
    int code;
    char why[NVK_PP_WHY];
    uint8_t used, state, late, stuck;
    uint64_t last;
};

typedef struct nvk_pp {
    pthread_mutex_t mu;
    int ready;
    size_t key_bytes;
    uint64_t reset_seq;          // 仅 reset 递增；与等待者在 mu 下核对取消边界
    struct nvk_pp_ent ent[NVK_PP_MAX];
    uint64_t enters, followers, published, failed, timeouts_late, timeouts_stuck, full, evicted;
} nvk_pp;

void nvk_pp_init(nvk_pp *t, size_t key_bytes);
void nvk_pp_reset(nvk_pp *t);

// 1 = 你是构建者（成功/失败都必须且只能 publish 一次，或 abandon）；2 = 同键已有项（用 state/wait 取）；
// 0 = 表满/停用：这次不合并，自己建（调用方回退 SYNC）。eq 判两个 key 是否同一配方。
int  nvk_pp_enter(nvk_pp *t, const void *key, int (*eq)(const void *, const void *), int *handle);
int  nvk_pp_reclaim_ready(nvk_pp *t, int handle);   // 调用方确认管线已被淘汰，原 READY 槽重新认领
void nvk_pp_publish(nvk_pp *t, int handle, void *obj, int code, const char *why);
void nvk_pp_abandon(nvk_pp *t, int handle);   // 构建者放弃（入队失败回退 SYNC 时）

// 状态快照。READY 时 *obj 有效；FAILED 时 *code / *why 有效；其余置空。
int  nvk_pp_state(nvk_pp *t, const void *key, int (*eq)(const void *, const void *),
                  void **obj, int *code, char *why, size_t why_cap);

// 有界等待离开 COMPILING。wait_ms = 0：只查一次不等（SKIP 预算耗尽时的口径）。
// 等待期间发生 reset 返回 ABSENT，输出保持空；尚在 COMPILING 的预算到期仍返回 TIMEOUT。
int  nvk_pp_wait(nvk_pp *t, const void *key, int (*eq)(const void *, const void *), uint32_t wait_ms,
                 void **obj, int *code, char *why, size_t why_cap);

// late / stuck 项标（is_stuck=0 → late，否则 stuck）。项不在（已完成被淘汰）就无操作。
void nvk_pp_mark(nvk_pp *t, const void *key, int (*eq)(const void *, const void *), int is_stuck);
int  nvk_pp_marks(nvk_pp *t, const void *key, int (*eq)(const void *, const void *), int *late, int *stuck);

void nvk_pp_stats(nvk_pp *t, uint64_t *enters, uint64_t *followers, uint64_t *published, uint64_t *failed,
                  uint64_t *timeouts_late, uint64_t *timeouts_stuck, uint64_t *full, uint64_t *evicted);

#ifdef __cplusplus
}
#endif
