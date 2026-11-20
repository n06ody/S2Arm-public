// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
//
// Adreno GPU 提频判定。沿用 9c6cc0ce6 的帧率、频率和前台门槛。
// 自动档在连续慢帧且 GPU 频率有余量时出手。上限下跌或没有收益时交还调频器。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { S2A_TURBO_OFF = 0, S2A_TURBO_ALWAYS = 1, S2A_TURBO_AUTO = 2 };

#define S2A_TURBO_TICK_NS        250000000ull    // 判定周期
#define S2A_TURBO_WINDOW_NS     1000000000ull    // 量帧窗口
#define S2A_TURBO_ENTER_WINDOWS  3               // 连续几个慢窗口才锁频（也是「近几个窗口」的窗口数）
#define S2A_TURBO_IDLE_WINDOWS   5               // 锁频中连续几个空窗口（一帧未出）就松开
#define S2A_TURBO_HOLD_MIN_NS   60000000000ull   // 锁频至少保持多久才允许按「不慢了」松开
#define S2A_TURBO_HOLD_MAX_NS  600000000000ull   // 反复被打回时保持时间翻倍的封顶
#define S2A_TURBO_REARM_NS     120000000000ull   // 松开后多久之内再慢回来算「反复」
// 慢 = 窗口内实际帧率 < 目标帧率的 9/10
#define S2A_TURBO_LATE_NUM 9
#define S2A_TURBO_LATE_DEN 10
#define S2A_TURBO_CEIL_PERMILLE      950         // 均频 ≥ 上限的 95% = 已顶在上限
#define S2A_TURBO_CAP_DROP_PERMILLE  950         // 上限 < 锁频前的 95% = 系统在压上限
#define S2A_TURBO_CAP_DROP_WINDOWS   2           // 连续几个窗口都被压才算（单窗口抖动不算）
#define S2A_TURBO_BENEFIT_SETTLE_NS 5000000000ull   // 锁频后先等 5 秒（调频器跟上、线程迁移稳定）再量
#define S2A_TURBO_BENEFIT_WINDOWS   10           // 量 10 个有出图的窗口
#define S2A_TURBO_BENEFIT_MIN_PCT   5            // 帧率至少比锁频前高 5%
#define S2A_TURBO_NOGAIN_BACKOFF_NS      60000000000ull    // 没有收益后至少暂缓多久再试
#define S2A_TURBO_NOGAIN_BACKOFF_MAX_NS 600000000000ull    // 连着没有收益：每次翻倍，封顶

enum { S2A_TURBO_WHY_NONE = 0, S2A_TURBO_WHY_GATE, S2A_TURBO_WHY_MODE, S2A_TURBO_WHY_DONE,
       S2A_TURBO_WHY_IDLE, S2A_TURBO_WHY_CAP_DROP, S2A_TURBO_WHY_NO_GAIN };
enum { S2A_TURBO_BLOCK_NONE = 0, S2A_TURBO_BLOCK_CEILING, S2A_TURBO_BLOCK_LATCHED, S2A_TURBO_BLOCK_BACKOFF };
enum { S2A_TURBO_BENCH_OFF = 0, S2A_TURBO_BENCH_SETTLE, S2A_TURBO_BENCH_MEASURE, S2A_TURBO_BENCH_DONE };

struct s2a_turbo_in {
    uint64_t now_ns;       // 单调时钟
    uint64_t frames;       // 累计上屏帧数（只增；无符号回绕按差值算）
    uint64_t period_ns;    // 游戏登记的目标出图周期，0 = 还没登记
    int mode;              // S2A_TURBO_*
    int foreground, surface, stopped;
    // GPU的实际频率：「频率(kHz) × 时间」与「时间」两个累计量（只增，时间单位任意但两者一致）；
    // 窗口内的均频 = Δ频率时间 / Δ时间。读不到时两者都不动（Δ时间 = 0 = 这个窗口没有读数）。
    uint64_t fq_work, fq_time;
    uint32_t max_khz;      // KGSL 当前频率上限（kHz）；0 = 没读到
};

// 一个收完的窗口的摘要
struct s2a_turbo_win {
    uint64_t df, dt;       // 窗口内出图帧数与时长（ns）
    uint64_t fw, ft;       // 窗口内 Δ(频率 × 时间)、Δ时间；ft == 0 = 没读到
    uint32_t cap;        // 窗口收尾时的 GPU 频率上限
};

struct s2a_turbo_state {
    int active;
    int forced;                        // 当前的「锁频」来自常开模式
    int win_valid;
    int late_run, idle_run;            // 连续慢窗口数 / 锁频中连续空窗口数
    uint64_t win_ns, win_frames;       // 当前窗口的起点时刻与起点处的累计帧数
    uint64_t win_fw, win_ft;           // 当前窗口起点处的累计频率时间 / 时间
    uint64_t since_ns, hold_ns;        // 本次锁频的起点（续期时更新）与保持时长
    uint64_t released_ns, prev_hold_ns;// 上次按「不慢了」松开的时刻与当时的保持时长（0 = 没有）
    struct s2a_turbo_win ring[S2A_TURBO_ENTER_WINDOWS];   // 最近收完的窗口，ring[0] 最旧
    int ring_n;
    int latched;                       // 本局不再出手（上限被压）；开关切到 0 或 1 才清
    uint64_t nogain_until_ns;          // 没有收益之后：到这一刻之前不再试（0 = 没有）
    uint64_t nogain_backoff_ns;        // 上一次暂缓了多久（再没有收益就翻倍；量出收益清零；0 = 没有）
    int block, why;                    // 最近一次没出手的原因 / 最近一次松开的原因（日志用）
    int ceil_permille;                 // 最近一次「顶没顶在上限」的 均频 / 上限（千分数；-1 = 没读到）
    // 锁频期间
    uint32_t base_cap;                 // 锁频前近几个窗口里上限的最小值
    int drop_run;
    uint32_t cap_from, cap_to;         // 触发限频退场时的两侧读数
    int bench;                         // S2A_TURBO_BENCH_*
    uint64_t pre_df, pre_dt, post_df, post_dt;   // 锁频前 / 锁频后两段的出图帧数与时长
    unsigned post_n;
    uint64_t pre_mfps, post_mfps;      // 判定时两段的帧率（毫帧 / 秒，日志用）
};

// 全部清零，连「本局不再出手」与「没有收益的暂缓」一起忘（开关被显式切到 0 / 1）
static inline void s2a_turbo_reset_all(struct s2a_turbo_state *st) { memset(st, 0, sizeof *st); }
// 一段判定 / 一次锁频的状态清零，「本局不再出手」与暂缓留着（门开关、前后台切换不洗掉它们）
static inline void s2a_turbo_reset_run(struct s2a_turbo_state *st) {
    const int latched = st->latched;
    const uint64_t until = st->nogain_until_ns, backoff = st->nogain_backoff_ns;
    memset(st, 0, sizeof *st);
    st->latched = latched; st->nogain_until_ns = until; st->nogain_backoff_ns = backoff;
}

static inline int s2a_turbo_gate_ok(const struct s2a_turbo_in *in) {
    return in->foreground && in->surface && !in->stopped;
}

static inline void s2a_turbo_open_window(struct s2a_turbo_state *st, const struct s2a_turbo_in *in) {
    st->win_ns = in->now_ns; st->win_frames = in->frames; st->win_valid = 1;
    st->win_fw = in->fq_work; st->win_ft = in->fq_time;
}

static inline void s2a_turbo_ring_push(struct s2a_turbo_state *st, const struct s2a_turbo_win *w) {
    if (st->ring_n == S2A_TURBO_ENTER_WINDOWS) {
        for (int i = 1; i < S2A_TURBO_ENTER_WINDOWS; i++) st->ring[i - 1] = st->ring[i];
        st->ring_n--;
    }
    st->ring[st->ring_n++] = *w;
}

// GPU近几个窗口的均频 / 上限，千分数（可以超过 1000）；频率或上限读不到 / 数值不合理 = -1。
// 只统计「频率与上限都读到了」的窗口，分子分母取自同一批窗口。
static inline int s2a_turbo_ceil_permille(const struct s2a_turbo_state *st) {
    uint64_t fw = 0, ft = 0, capsum = 0, ncap = 0;
    for (int i = 0; i < st->ring_n; i++) {
        const struct s2a_turbo_win *w = &st->ring[i];
        if (w->ft && w->cap) { fw += w->fw; ft += w->ft; capsum += w->cap; ncap++; }
    }
    if (!ft || !ncap) return -1;
    if (fw > (1ull << 44) || ft > (1ull << 30) || capsum > (1ull << 25)) return -1;   // 乘法不溢出的范围
    const uint64_t r = fw * ncap * 1000ull / (capsum * ft);
    return r > 2000 ? 2000 : (int)r;
}

// 两侧上限都可读且下降超过 5% 才算被压。
static inline int s2a_turbo_cap_dropped(const struct s2a_turbo_state *st, const struct s2a_turbo_in *in) {
    return st->base_cap && in->max_khz &&
        (uint64_t)in->max_khz * 1000ull < (uint64_t)st->base_cap * S2A_TURBO_CAP_DROP_PERMILLE;
}

// 锁频后的帧率是否比锁频前高出 MIN_PCT：post_df / post_dt ≥ pre_df / pre_dt × (100 + MIN_PCT) / 100（整数交叉相乘）。
static inline int s2a_turbo_gain_ok(uint64_t pre_df, uint64_t pre_dt, uint64_t post_df, uint64_t post_dt) {
    return post_df * pre_dt * 100ull >= pre_df * post_dt * (100ull + S2A_TURBO_BENEFIT_MIN_PCT);
}
static inline uint64_t s2a_turbo_mfps(uint64_t df, uint64_t dt_ns) { return dt_ns ? df * 1000000000000ull / dt_ns : 0; }

// 松开一次锁频（不洗掉窗口 / 环 / 本局的「不再出手」）
static inline void s2a_turbo_release(struct s2a_turbo_state *st, int why) {
    st->active = 0; st->late_run = st->idle_run = 0; st->bench = S2A_TURBO_BENCH_OFF; st->drop_run = 0; st->why = why;
}

// 每个判定周期调一次，返回此刻该不该锁频；状态都在 st 里。
static inline int s2a_turbo_step(struct s2a_turbo_state *st, const struct s2a_turbo_in *in) {
    if (in->mode == S2A_TURBO_OFF) {
        const int was = st->active;
        s2a_turbo_reset_all(st); st->why = was ? S2A_TURBO_WHY_MODE : S2A_TURBO_WHY_NONE;
        return 0;
    }
    if (!s2a_turbo_gate_ok(in)) {
        const int was = st->active;
        s2a_turbo_reset_run(st); st->why = was ? S2A_TURBO_WHY_GATE : S2A_TURBO_WHY_NONE;
        return 0;
    }
    if (in->mode == S2A_TURBO_ALWAYS) {
        if (!st->forced) { s2a_turbo_reset_all(st); st->forced = 1; }   // 常开是显式指令：不带「本局不再出手」
        st->active = 1;
        return 1;
    }
    if (st->forced) s2a_turbo_reset_run(st);                            // 从常开切回自适应：从零开始
    if (!st->win_valid || in->now_ns < st->win_ns) {   // 第一次，或时钟倒退：重开窗口，连续数不带过去
        st->late_run = st->idle_run = 0;               // （窗口环不用清：出手要连着 3 个新的慢窗口，环会被新窗口填满）
        if (st->active) st->since_ns = in->now_ns;
        // 暂缓到点的时刻是按旧时钟记的：时钟倒退后最多再等一个暂缓长度，不会被倒退的那一段拖长
        if (st->nogain_until_ns > in->now_ns + st->nogain_backoff_ns) st->nogain_until_ns = in->now_ns + st->nogain_backoff_ns;
        s2a_turbo_open_window(st, in);
        return st->active;
    }
    const uint64_t dt = in->now_ns - st->win_ns;
    if (dt < S2A_TURBO_WINDOW_NS) return st->active;
    const uint64_t df = in->frames - st->win_frames;   // 无符号差：计数回绕也对
    const int idle = df == 0;
    // 帧率 < 目标 * 9/10 ⇔ df / dt < 0.9 / period ⇔ df * period * 10 < dt * 9（整数，无浮点）
    const int late = in->period_ns && df &&
        df * in->period_ns * S2A_TURBO_LATE_DEN < dt * S2A_TURBO_LATE_NUM;
    struct s2a_turbo_win w;
    memset(&w, 0, sizeof w);
    w.df = df; w.dt = dt;
    if (in->fq_time > st->win_ft && in->fq_work >= st->win_fw) {   // 累计只增且有进展才算读到（计数被清零 / 回退 = 这个窗口没读数）
        w.fw = in->fq_work - st->win_fw; w.ft = in->fq_time - st->win_ft;
    }
    w.cap = in->max_khz;
    s2a_turbo_ring_push(st, &w);
    s2a_turbo_open_window(st, in);

    if (!st->active) {
        if (late) { if (st->late_run < 1000) st->late_run++; } else st->late_run = 0;
        if (st->late_run < S2A_TURBO_ENTER_WINDOWS) { st->block = S2A_TURBO_BLOCK_NONE; return 0; }
        if (st->latched) { st->block = S2A_TURBO_BLOCK_LATCHED; return 0; }
        if (in->now_ns < st->nogain_until_ns) { st->block = S2A_TURBO_BLOCK_BACKOFF; return 0; }   // 刚试过没有收益：暂缓期内不再试；慢窗口数留着，到点接着判
        st->ceil_permille = s2a_turbo_ceil_permille(st);
        if (st->ceil_permille >= S2A_TURBO_CEIL_PERMILLE) {   // 已顶在上限：没有东西可托。慢窗口数留着，下个窗口接着判
            st->block = S2A_TURBO_BLOCK_CEILING;
            return 0;
        }
        st->block = S2A_TURBO_BLOCK_NONE;
        const uint64_t doubled = st->prev_hold_ns * 2 > S2A_TURBO_HOLD_MAX_NS ? S2A_TURBO_HOLD_MAX_NS : st->prev_hold_ns * 2;
        st->hold_ns = st->released_ns && in->now_ns - st->released_ns <= S2A_TURBO_REARM_NS && doubled > S2A_TURBO_HOLD_MIN_NS
            ? doubled : S2A_TURBO_HOLD_MIN_NS;
        st->since_ns = in->now_ns; st->active = 1; st->why = S2A_TURBO_WHY_NONE;
        st->late_run = st->idle_run = 0;
        // 锁频前的账：近几个慢窗口的帧率（给「有没有收益」做对照）、各簇上限的最小值（给「上限在不在掉」做对照）
        st->pre_df = st->pre_dt = st->post_df = st->post_dt = 0; st->post_n = 0;
        for (int i = 0; i < st->ring_n; i++) { st->pre_df += st->ring[i].df; st->pre_dt += st->ring[i].dt; }
        st->base_cap = 0;
        for (int i = 0; in->max_khz && i < st->ring_n; i++) {
            const uint32_t cap = st->ring[i].cap;
            if (cap && (!st->base_cap || cap < st->base_cap)) st->base_cap = cap;
        }
        st->drop_run = 0; st->bench = S2A_TURBO_BENCH_SETTLE;
        return 1;
    }
    st->idle_run = idle ? st->idle_run + 1 : 0;
    if (st->idle_run >= S2A_TURBO_IDLE_WINDOWS) {      // 没在出图：松开，不算「反复」
        s2a_turbo_release(st, S2A_TURBO_WHY_IDLE); st->released_ns = st->prev_hold_ns = 0;
        return 0;
    }
    if (s2a_turbo_cap_dropped(st, in)) {        // 自检一：系统在压上限，多烧的核换来了降档——松开，本局不再出手
        if (++st->drop_run >= S2A_TURBO_CAP_DROP_WINDOWS) {
            st->cap_from = st->base_cap; st->cap_to = in->max_khz;
            s2a_turbo_release(st, S2A_TURBO_WHY_CAP_DROP); st->latched = 1; st->released_ns = st->prev_hold_ns = 0;
            return 0;
        }
    } else st->drop_run = 0;
    if (st->bench == S2A_TURBO_BENCH_SETTLE) {         // 自检二：先等调频器跟上，再量 10 个出图窗口
        if (in->now_ns - st->since_ns >= S2A_TURBO_BENEFIT_SETTLE_NS) st->bench = S2A_TURBO_BENCH_MEASURE;
    } else if (st->bench == S2A_TURBO_BENCH_MEASURE) {
        if (!idle) { st->post_df += df; st->post_dt += dt; st->post_n++; }
        if (st->post_n >= S2A_TURBO_BENEFIT_WINDOWS) {
            st->pre_mfps = s2a_turbo_mfps(st->pre_df, st->pre_dt); st->post_mfps = s2a_turbo_mfps(st->post_df, st->post_dt);
            if (!s2a_turbo_gain_ok(st->pre_df, st->pre_dt, st->post_df, st->post_dt)) {   // 帧率没动：瓶颈不在 GPU 频率（或这一段在载入）
                const uint64_t twice = st->nogain_backoff_ns * 2;
                st->nogain_backoff_ns = !st->nogain_backoff_ns ? S2A_TURBO_NOGAIN_BACKOFF_NS
                                      : twice > S2A_TURBO_NOGAIN_BACKOFF_MAX_NS ? S2A_TURBO_NOGAIN_BACKOFF_MAX_NS : twice;
                st->nogain_until_ns = in->now_ns + st->nogain_backoff_ns;
                s2a_turbo_release(st, S2A_TURBO_WHY_NO_GAIN); st->released_ns = st->prev_hold_ns = 0;
                return 0;
            }
            st->bench = S2A_TURBO_BENCH_DONE;
            st->nogain_backoff_ns = st->nogain_until_ns = 0;   // 量出收益了：之前攒的暂缓清零
        }
    }
    if (in->now_ns - st->since_ns >= st->hold_ns) {
        if (late) st->since_ns = in->now_ns;            // 带着锁频还慢（但比锁频前快，自检已确认）：续期
        else {
            s2a_turbo_release(st, S2A_TURBO_WHY_DONE); st->late_run = 0;
            st->released_ns = in->now_ns; st->prev_hold_ns = st->hold_ns;
        }
    }
    return st->active;
}

// ---- 帧间隔直方图（只出读数，不参与判定）----
// 桶上沿（ms）：≤17（60 帧档）/ ≤25 / ≤34（30 帧档）/ ≤42 / ≤51 / ≤67 / ≤100 / 超过 100。
// 每帧一次计数，每 5 秒由监控线程取走并打一行：开关前后同一份读数，A/B 不用外部抓取工具。
#define S2A_TURBO_NBUCKET 8
#define S2A_TURBO_SLOW_BUCKET 3      // 下标 ≥ 3（> 34 ms）的桶算「慢帧」：30 帧档错过一个刷新
#define S2A_TURBO_STAT_NS 5000000000ull

static inline unsigned s2a_turbo_bucket_limit_ms(int i) {
    static const unsigned lim[S2A_TURBO_NBUCKET - 1] = {17, 25, 34, 42, 51, 67, 100};
    return i >= 0 && i < S2A_TURBO_NBUCKET - 1 ? lim[i] : 0;   // 0 = 最后一桶，没有上沿
}
static inline int s2a_turbo_bucket(uint64_t interval_ns) {
    for (int i = 0; i < S2A_TURBO_NBUCKET - 1; i++)
        if (interval_ns <= (uint64_t)s2a_turbo_bucket_limit_ms(i) * 1000000ull) return i;
    return S2A_TURBO_NBUCKET - 1;
}
// 第 q（千分数，p99 = 990）分位所在的桶下标；没有样本返回 -1。
static inline int s2a_turbo_hist_quantile(const unsigned *h, unsigned q_permille) {
    uint64_t total = 0;
    for (int i = 0; i < S2A_TURBO_NBUCKET; i++) total += h[i];
    if (!total) return -1;
    uint64_t rank = (total * q_permille + 999) / 1000;   // 向上取整：至少覆盖这么多个样本
    if (rank < 1) rank = 1;
    uint64_t cum = 0;
    for (int i = 0; i < S2A_TURBO_NBUCKET; i++) { cum += h[i]; if (cum >= rank) return i; }
    return S2A_TURBO_NBUCKET - 1;
}
static inline uint64_t s2a_turbo_hist_slow(const unsigned *h) {
    uint64_t n = 0;
    for (int i = S2A_TURBO_SLOW_BUCKET; i < S2A_TURBO_NBUCKET; i++) n += h[i];
    return n;
}
