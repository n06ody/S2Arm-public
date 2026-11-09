// First-use compile wait semantics shared between iOS (nvn_mtl.m pso_pend_*)
// and the Android direct Vulkan path. See nvn_first_use.h for the contract;
// every rule below cites the iOS clause it mirrors.
#include "nvn_first_use.h"
#include <string.h>

/* ---------------- render-target history ----------------
 * 128-bit keys so the storage table can key {mem,format} without collisions;
 * serial lookups pass key2 = 0. Values pack the two most recent frame tags
 * (newest << 32 | previous) exactly like iOS rt_seen_put. */
#define S2A_FU_RT_CAP 1024u
#define S2A_FU_RT_CAP_MASK (S2A_FU_RT_CAP - 1u)

struct s2a_fu_slot {
    uint64_t k1, k2;        /* k1==0 && k2==0 = 空槽 */
    uint32_t last, prev;
};

static struct s2a_fu_slot g_fresh[S2A_FU_RT_CAP];   /* serial ∈ this frame (last tag only) */
static struct s2a_fu_slot g_seen[S2A_FU_RT_CAP];    /* serial → last two frames */
static struct s2a_fu_slot g_seen_mem[S2A_FU_RT_CAP];/* {mem,fmt} → last two frames */
static uint64_t g_rt_frame;                          /* last ticked frame */
static int g_rt_valid;

static uint32_t s2a_fu_tag(uint64_t frame) { return (uint32_t)(frame + 1); }   /* +1：0 留给空槽/未画 */

static uint32_t s2a_fu_hash(uint64_t k1, uint64_t k2) {
    uint64_t h = k1 * 0x9E3779B97F4A7C15ull ^ k2;
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    return (uint32_t)h;
}

static struct s2a_fu_slot *s2a_fu_find(struct s2a_fu_slot *t, uint64_t k1, uint64_t k2) {
    uint32_t i = s2a_fu_hash(k1, k2) & S2A_FU_RT_CAP_MASK;
    for (uint32_t n = 0; n < S2A_FU_RT_CAP; n++) {
        struct s2a_fu_slot *s = &t[(i + n) & S2A_FU_RT_CAP_MASK];
        if (!s->k1 && !s->k2) return NULL;
        if (s->k1 == k1 && s->k2 == k2) return s;
    }
    return NULL;
}

static struct s2a_fu_slot *s2a_fu_insert(struct s2a_fu_slot *t, uint64_t k1, uint64_t k2) {
    uint32_t i = s2a_fu_hash(k1, k2) & S2A_FU_RT_CAP_MASK;
    for (uint32_t n = 0; n < S2A_FU_RT_CAP; n++) {
        struct s2a_fu_slot *s = &t[(i + n) & S2A_FU_RT_CAP_MASK];
        if (!s->k1 && !s->k2) { s->k1 = k1; s->k2 = k2; s->last = s->prev = 0; return s; }
        if (s->k1 == k1 && s->k2 == k2) return s;
    }
    return NULL;   /* 满：保守缺这条证据（判定按“不满足”走，不伪造） */
}

void s2a_fu_rt_reset(void) {
    memset(g_fresh, 0, sizeof g_fresh);
    memset(g_seen, 0, sizeof g_seen);
    memset(g_seen_mem, 0, sizeof g_seen_mem);
    g_rt_valid = 0;
}

void s2a_fu_rt_tick(uint64_t frame) {
    if (g_rt_valid && frame == g_rt_frame) return;
    memset(g_fresh, 0, sizeof g_fresh);
    if (!g_rt_valid || frame < g_rt_frame ||
        ((frame + 1) >> 32) != ((g_rt_frame + 1) >> 32)) {
        /* 重置/回退/32 位 tag 绕回：上一帧证据全部作废，与 iOS 同一分支。 */
        memset(g_seen, 0, sizeof g_seen);
        memset(g_seen_mem, 0, sizeof g_seen_mem);
    } else {
        const uint32_t f = s2a_fu_tag(frame);
        for (int t = 0; t < 2; t++) {
            struct s2a_fu_slot *h = t ? g_seen_mem : g_seen;
            for (uint32_t i = 0; i < S2A_FU_RT_CAP; i++) {
                struct s2a_fu_slot *s = &h[i];
                if ((s->k1 || s->k2) && s->last && (uint64_t)f - s->last > 2) {
                    s->k1 = s->k2 = 0; s->last = s->prev = 0;
                }
            }
        }
    }
    g_rt_frame = frame;
    g_rt_valid = 1;
}

void s2a_fu_rt_mark_fresh(uint64_t serial, uint64_t frame) {
    if (!serial) return;
    s2a_fu_rt_tick(frame);
    struct s2a_fu_slot *s = s2a_fu_insert(g_fresh, serial, 0);
    if (s) s->last = s2a_fu_tag(frame);
}

void s2a_fu_rt_mark_seen(uint64_t serial, uint64_t storage_key, uint64_t fmt, uint64_t frame) {
    if (!serial) return;
    s2a_fu_rt_tick(frame);
    const uint32_t f = s2a_fu_tag(frame);
    struct s2a_fu_slot *s = s2a_fu_insert(g_seen, serial, 0);
    /* iOS rt_seen_put：同帧重复 mark 不滚动 prev。 */
    if (s && s->last != f) { s->prev = s->last; s->last = f; }
    if (storage_key) {
        struct s2a_fu_slot *m = s2a_fu_insert(g_seen_mem, storage_key, fmt);
        if (m && m->last != f) { m->prev = m->last; m->last = f; }
    }
}

int s2a_fu_rt_fresh_now(uint64_t serial, uint64_t frame) {
    if (!serial) return 1;
    s2a_fu_rt_tick(frame);
    struct s2a_fu_slot *s = s2a_fu_find(g_fresh, serial, 0);
    return s && s->last == s2a_fu_tag(frame);
}

static int s2a_fu_seen_had(const struct s2a_fu_slot *t, uint64_t k1, uint64_t k2) {
    struct s2a_fu_slot *s = s2a_fu_find((struct s2a_fu_slot *)t, k1, k2);
    /* iOS rt_seen_had：prev 非 0 且最近两次标帧间隔 ≤ 2。 */
    return s && s->prev && s->last - s->prev <= 2;
}

int s2a_fu_rt_seen_steady(uint64_t serial, uint64_t storage_key, uint64_t fmt, uint64_t frame) {
    if (!serial) return 1;
    s2a_fu_rt_tick(frame);
    if (s2a_fu_seen_had(g_seen, serial, 0)) return 1;
    return storage_key && s2a_fu_seen_had(g_seen_mem, storage_key, fmt);
}

void s2a_fu_rt_stats(uint32_t *fresh_n, uint32_t *seen_n, uint32_t *seen_mem_n) {
    uint32_t n[3] = {0, 0, 0};
    for (uint32_t i = 0; i < S2A_FU_RT_CAP; i++) {
        if (g_fresh[i].k1 || g_fresh[i].k2) n[0]++;
        if (g_seen[i].k1 || g_seen[i].k2) n[1]++;
        if (g_seen_mem[i].k1 || g_seen_mem[i].k2) n[2]++;
    }
    if (fresh_n) *fresh_n = n[0];
    if (seen_n) *seen_n = n[1];
    if (seen_mem_n) *seen_mem_n = n[2];
}

/* ---------------- per-frame wait budget ---------------- */
int64_t s2a_fu_budget_left_ms(struct s2a_fu_budget *b, uint64_t frame, uint32_t frame_ms) {
    if (b->frame != frame) { b->frame = frame; b->spent_ns = 0; }
    if (!frame_ms) return INT64_MAX;              /* iOS：budget 关闭时无限 */
    int64_t left = (int64_t)frame_ms - (int64_t)(b->spent_ns / 1000000ull);
    return left > 0 ? left : 0;
}

void s2a_fu_budget_spend(struct s2a_fu_budget *b, uint64_t frame, uint64_t waited_ns) {
    if (b->frame != frame) { b->frame = frame; b->spent_ns = 0; }
    b->spent_ns += waited_ns;
}

/* ---------------- wait decision ----------------
 * iOS pso_pend_take 逐条对应：
 *   may_skip && late && !done → 立即超时（late 只挡可跳绘制）
 *   stuck && !done            → 立即超时（stuck 挡一切绘制）
 *   may_skip && !done         → 本帧预算裁本片 wait；预算空 → 立即 SKIP(BUDGET)
 *   等待超时后：ms < item_ms 且 frame_ms → BUDGET；否则 late（可跳）/stuck（不可跳）
 */
struct s2a_fu_verdict s2a_fu_wait_decide(int ready, int late, int stuck, int may_skip,
                                         uint32_t item_ms, uint32_t frame_ms,
                                         int64_t budget_left_ms, uint32_t unsafe_ms) {
    struct s2a_fu_verdict v = { S2A_FU_SKIP, S2A_FU_T_NONE, 0 };
    if (ready) { v.action = S2A_FU_READY; return v; }
    if (may_skip && late) { v.timeout = S2A_FU_T_LATE; return v; }
    if (stuck) { v.timeout = S2A_FU_T_STUCK; return v; }
    int64_t ms = may_skip ? (int64_t)item_ms : (int64_t)unsafe_ms;
    if (may_skip) {
        int64_t left = budget_left_ms < 0 ? 0 : budget_left_ms;
        if (left < (ms ? ms : INT64_MAX)) {   /* iOS：无界 item 也按预算余量裁 */
            ms = left;
            if (!ms) { v.timeout = S2A_FU_T_BUDGET; return v; }
        }
    }
    v.action = S2A_FU_WAIT;
    v.wait_ms = ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
    if (ms < item_ms && frame_ms) v.timeout = S2A_FU_T_BUDGET;
    else v.timeout = may_skip ? S2A_FU_T_LATE : S2A_FU_T_STUCK;
    return v;
}
