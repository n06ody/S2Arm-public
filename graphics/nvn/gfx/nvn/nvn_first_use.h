// First-use compile wait semantics shared between the iOS Metal backend
// (graphics/nvn/gfx/nvn/nvn_mtl.m: pso_pend_take / rt_seen / rt_fresh /
// pso_budget_left_ms) and the Android direct Vulkan path (nvn_vk_draw.c).
//
// Pure C, no platform calls and no locks: the iOS contract is that only the
// serialized submit/replay thread touches these tables, and the Android side
// holds its exec lock the same way. Semantics are copied clause by clause —
// a semantic change on either side must update both.
//
// The three limits, same defaults iOS uses (DM_NVN_PSO_*):
//   item_ms   - per in-flight compile: how long one draw may wait for it (2000)
//   frame_ms  - aggregate this frame's skippable draws may wait, total (100)
//   unsafe_ms - a draw that must not be skipped waits at most this long (10000)
//
// Skip-safety inputs (iOS skip_safe / first_skip_ok):
//   - neither stage writes storage (the caller proves it: iOS reads shader
//     reflection; Android uses the bound-SSBO / transform-feedback snapshot)
//   - every colour/depth target was started fresh this frame (a clear marked it)
//   - every target was drawn in the last two frames — by object serial, or by
//     any object over the same storage+format (BotW/TotK dynamic resolution
//     makes new texture objects each frame; iOS keeps the second table for it)
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- render-target history ----------------
 * Two open-addressing tables with a bounded capacity; not linked, single
 * thread by contract (same as iOS nvn_hmap usage here).
 *
 * fresh table: serials whose storage started fresh this frame (a clear op, or
 *   iOS pass-load-clear). Membership only — cleared on every frame tick.
 * seen tables: serial / {storage,format} -> last two frame tags the target
 *   was attached as a draw target, newest first. "Steady" = drawn in two
 *   distinct frames at most two apart (iOS rt_seen_had: prev && last-prev<=2).
 */
void     s2a_fu_rt_reset(void);

/* Advance the history to `frame` (any monotonically increasing frame serial).
 * Clears the fresh table; prunes seen entries older than two frames, and drops
 * everything on backward move / 32-bit tag wrap — same as rt_history_frame. */
void     s2a_fu_rt_tick(uint64_t frame);

/* Mark a target's contents as started fresh in `frame` (called at clear ops;
 * frame must be the last ticked frame). serial==0 is ignored. */
void     s2a_fu_rt_mark_fresh(uint64_t serial, uint64_t frame);

/* Mark a target as attached/drawn-to in `frame`. serial==0 ignored;
 * storage_key==0 disables the storage-table mark (iOS t->mem NULL). */
void     s2a_fu_rt_mark_seen(uint64_t serial, uint64_t storage_key, uint64_t fmt, uint64_t frame);

/* serial==0 answers 1, matching iOS rt_fresh_now(NULL) / rt_seen_steady(NULL). */
int      s2a_fu_rt_fresh_now(uint64_t serial, uint64_t frame);
int      s2a_fu_rt_seen_steady(uint64_t serial, uint64_t storage_key, uint64_t fmt, uint64_t frame);

/* Both tables full at once is capacity evidence, not a failure: inserts stop
 * and the predicate keeps answering from what is there (conservative). */
void     s2a_fu_rt_stats(uint32_t *fresh_n, uint32_t *seen_n, uint32_t *seen_mem_n);

/* ---------------- per-frame wait budget ----------------
 * iOS g_pp_bud_frame/g_pp_bud_ns: real measured wait nanoseconds accumulate
 * against one frame's allowance; the frame serial resets the accumulator. */
struct s2a_fu_budget {
    uint64_t frame;
    uint64_t spent_ns;
};

/* What this frame may still wait in ms; INT64_MAX without a frame budget.
 * `spent_ns` beyond frame_ms makes the answer 0, not negative — iOS clamps. */
int64_t  s2a_fu_budget_left_ms(struct s2a_fu_budget *b, uint64_t frame, uint32_t frame_ms);
void     s2a_fu_budget_spend(struct s2a_fu_budget *b, uint64_t frame, uint64_t waited_ns);

/* ---------------- wait decision ----------------
 * The verdict core of iOS pso_pend_take: the caller holds the item state
 * (ready/late/stuck) and performs the actual wait; this only decides. */
enum {
    S2A_FU_READY = 0,   /* 在途结果已就绪：取走并绘制 */
    S2A_FU_WAIT,        /* 限时等 wait_ms（0 = 无界）；超时未就绪 → timeout 指示的终局 */
    S2A_FU_SKIP,        /* 跳过本笔绘制 */
};

/* Terminal/timeout classification, iOS *timed_out aligned: 1 = the item passed
 * its own wait limit (late: every later skippable draw with it skips at once);
 * 2 = the frame's wait budget ran out (no late mark: a later frame may wait
 * for it again); 3 = a draw that could not be skipped passed the unsafe cap
 * (stuck: every later draw with it, safe or not, skips at once). */
enum {
    S2A_FU_T_NONE = 0,
    S2A_FU_T_LATE = 1,
    S2A_FU_T_BUDGET = 2,
    S2A_FU_T_STUCK = 3,
};

struct s2a_fu_verdict {
    int      action;    /* S2A_FU_* */
    int      timeout;   /* SKIP: its reason (also the flags to mark). WAIT: what
                         * a timeout wake with !ready must end as. */
    uint32_t wait_ms;   /* WAIT: this slice's cap; 0 = wait without a deadline */
};

/* One decision step, called again after each bounded wait returns.
 *   ready      - the in-flight compile produced a result
 *   late       - this item already passed its item limit once
 *   stuck      - this item already passed the unsafe cap once
 *   may_skip   - this draw satisfies skip_safe (caller's predicate)
 *   item_ms    - per-item wait cap (iOS pso_wait_ms; 0 = no limit)
 *   frame_ms   - per-frame wait budget (iOS pso_frame_ms; 0 = no budget)
 *   budget_left_ms - s2a_fu_budget_left_ms of this frame
 *   unsafe_ms  - unskippable wait cap (iOS pso_unsafe_wait_ms; 0 = no cap)
 *
 * Marks are reported through `timeout`, never written here: SKIP_LATE sets
 * late, SKIP_STUCK sets stuck, SKIP_BUDGET sets neither. */
struct s2a_fu_verdict s2a_fu_wait_decide(int ready, int late, int stuck, int may_skip,
                                         uint32_t item_ms, uint32_t frame_ms,
                                         int64_t budget_left_ms, uint32_t unsafe_ms);

#ifdef __cplusplus
}
#endif
