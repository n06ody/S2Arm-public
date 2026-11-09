// C4a 宿主回归：共用首遇部件（graphics/nvn/gfx/nvn/nvn_first_use.c）——
// 逐条对齐 iOS pso_pend_take / rt_seen / rt_fresh / pso_budget 语义：
//   * late 只挡可跳绘制；stuck 挡一切绘制；
//   * 可跳：本片等 min(item, 帧预算余量)；超时若被预算裁 → BUDGET，否则 → LATE；
//   * 不可跳：不花帧预算，按 unsafe_ms 一片等到头；超时 → STUCK；
//   * rt_seen：同一 serial 在两帧标过、间隔 ≤2 → steady；storage+fmt 键顶同语义（BotW 动态分辨率）；
//   * rt_fresh：当帧成员（clear 标过）；跨帧清空；serial==0 答 1（iOS NULL 语义）。
#include "nvn_first_use.h"
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(cond, what) do { checks++; if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)

int main(void) {
    /* ---- 判定表 ---- */
    struct s2a_fu_verdict v;

    v = s2a_fu_wait_decide(1, 0, 0, 1, 2000, 100, 100, 10000);
    CHECK(v.action == S2A_FU_READY, "ready → READY");

    v = s2a_fu_wait_decide(0, 0, 0, 1, 2000, 100, 5000, 10000);
    CHECK(v.action == S2A_FU_WAIT && v.wait_ms == 2000 && v.timeout == S2A_FU_T_LATE,
          "可跳+预算大于 item：等满 item_ms，超时→LATE");

    v = s2a_fu_wait_decide(0, 0, 0, 1, 2000, 100, 30, 10000);
    CHECK(v.action == S2A_FU_WAIT && v.wait_ms == 30 && v.timeout == S2A_FU_T_BUDGET,
          "可跳+预算30ms：等30ms，超时→BUDGET（不记 late）");

    v = s2a_fu_wait_decide(0, 0, 0, 1, 2000, 100, 0, 10000);
    CHECK(v.action == S2A_FU_SKIP && v.timeout == S2A_FU_T_BUDGET,
          "可跳+预算空：立即跳过，原因 BUDGET");

    v = s2a_fu_wait_decide(0, 1, 0, 1, 2000, 100, 100, 10000);
    CHECK(v.action == S2A_FU_SKIP && v.timeout == S2A_FU_T_LATE,
          "可跳+late：立跳");

    v = s2a_fu_wait_decide(0, 0, 1, 1, 2000, 100, 100, 10000);
    CHECK(v.action == S2A_FU_SKIP && v.timeout == S2A_FU_T_STUCK,
          "可跳+stuck：立跳（stuck 挡一切）");

    v = s2a_fu_wait_decide(0, 0, 1, 0, 2000, 100, 100, 10000);
    CHECK(v.action == S2A_FU_SKIP && v.timeout == S2A_FU_T_STUCK,
          "不可跳+stuck：也立跳");

    v = s2a_fu_wait_decide(0, 1, 0, 0, 2000, 100, 100, 10000);
    CHECK(v.action == S2A_FU_WAIT && v.wait_ms == 10000 && v.timeout == S2A_FU_T_STUCK,
          "不可跳+late：late 不挡不可跳绘制，仍等 unsafe_ms 一片，超时→STUCK");

    v = s2a_fu_wait_decide(0, 0, 0, 0, 2000, 100, 0, 10000);
    CHECK(v.action == S2A_FU_WAIT && v.wait_ms == 10000 && v.timeout == S2A_FU_T_STUCK,
          "不可跳：预算空也不裁（不花帧预算），等 unsafe_ms，超时→STUCK");

    v = s2a_fu_wait_decide(0, 0, 0, 1, 0, 100, 50, 10000);
    CHECK(v.action == S2A_FU_WAIT && v.wait_ms == 50 && v.timeout == S2A_FU_T_LATE,
          "item_ms=0（无界）：被预算裁到 50ms；超时分类按 iOS 公式 ms<item_ms → LATE");

    v = s2a_fu_wait_decide(0, 0, 0, 1, 0, 0, INT64_MAX, 10000);
    CHECK(v.action == S2A_FU_WAIT && v.wait_ms == 0, "item=0 且无帧预算：无界等待");

    v = s2a_fu_wait_decide(0, 0, 0, 0, 2000, 100, 100, 0);
    CHECK(v.action == S2A_FU_WAIT && v.wait_ms == 0,
          "unsafe_ms=0（无上限）：不可跳无界等待（永不超时）");

    /* ---- 帧预算记账（iOS g_pp_bud_ns：实等纳秒累计进本帧） ---- */
    struct s2a_fu_budget b; memset(&b, 0, sizeof b);
    CHECK(s2a_fu_budget_left_ms(&b, 10, 100) == 100, "新帧预算满 100ms");
    s2a_fu_budget_spend(&b, 10, 40ull * 1000000);
    CHECK(s2a_fu_budget_left_ms(&b, 10, 100) == 60, "花 40ms 剩 60ms");
    s2a_fu_budget_spend(&b, 10, 200ull * 1000000);
    CHECK(s2a_fu_budget_left_ms(&b, 10, 100) == 0, "超支夹到 0 不负数");
    CHECK(s2a_fu_budget_left_ms(&b, 11, 100) == 100, "下一帧预算重置");
    CHECK(s2a_fu_budget_left_ms(&b, 11, 0) == INT64_MAX, "frame_ms=0：无预算上限");

    /* ---- rt 履历 ---- */
    s2a_fu_rt_reset();
    s2a_fu_rt_tick(100);
    /* serial 7：第 98/99 帧画过（last-prev≤2）、本帧清过 → 可跳证据齐 */
    s2a_fu_rt_mark_seen(7, 0xA000, 0x20, 98);
    s2a_fu_rt_mark_seen(7, 0xA000, 0x20, 99);
    s2a_fu_rt_tick(100);
    s2a_fu_rt_mark_fresh(7, 100);
    CHECK(s2a_fu_rt_fresh_now(7, 100), "serial 7 本帧清过 → fresh");
    CHECK(s2a_fu_rt_seen_steady(7, 0xA000, 0x20, 100), "serial 7 98/99 帧都画过 → steady");

    /* serial 8：只画过一帧 → 不 steady */
    s2a_fu_rt_mark_seen(8, 0xB000, 0x20, 100);
    CHECK(!s2a_fu_rt_seen_steady(8, 0xB000, 0x20, 100), "serial 8 只本帧见过 → 不 steady");

    /* serial 9 每帧换新对象：serial 表只留单帧证据（不 steady），storage+fmt 键跨帧命中（BotW 动态分辨率路径） */
    s2a_fu_rt_mark_seen(9, 0xC000, 0x20, 99);
    s2a_fu_rt_mark_seen(77, 0xC000, 0x20, 99);   /* 同一存储换个 serial——serial 表各自只 1 帧 */
    s2a_fu_rt_tick(100);
    s2a_fu_rt_mark_seen(99, 0xC000, 0x20, 100);
    s2a_fu_rt_mark_seen(88, 0xC000, 0x20, 100);
    CHECK(!s2a_fu_rt_seen_steady(9, 0, 0, 100), "serial 9 只标过一帧（serial 表）→ 不 steady");
    CHECK(s2a_fu_rt_seen_steady(99, 0xC000, 0x20, 100), "serial 99：serial 表 1 帧 + storage 键两帧 → steady");
    CHECK(!s2a_fu_rt_seen_steady(99, 0xC000, 0x21, 100), "serial 99 换 fmt：storage 键不命中 → 不 steady");
    CHECK(s2a_fu_rt_seen_steady(9, 0xC000, 0x20, 100), "serial 9 凭 storage 键 → steady");

    /* fresh 不跨帧 */
    s2a_fu_rt_tick(101);
    CHECK(!s2a_fu_rt_fresh_now(7, 101), "serial 7 上一帧清的，本帧不算 fresh");

    /* 间隔 >2 帧的 seen 不算 steady；条目被 prune 后也不 rebound */
    s2a_fu_rt_mark_seen(10, 0, 0, 99);
    s2a_fu_rt_mark_seen(10, 0, 0, 100);
    s2a_fu_rt_tick(101);
    CHECK(s2a_fu_rt_seen_steady(10, 0, 0, 101), "serial 10：99/100 标过、间隔1 → 第101帧仍 steady（last-prev≤2）");
    s2a_fu_rt_tick(200);
    CHECK(!s2a_fu_rt_seen_steady(10, 0, 0, 200), "serial 10：prune 后无证据");

    /* serial==0 答 1（iOS NULL 目标语义：没有目标不挡跳绘） */
    CHECK(s2a_fu_rt_fresh_now(0, 200) && s2a_fu_rt_seen_steady(0, 0, 0, 200),
          "serial==0 → fresh/steady 都答 1");

    printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
