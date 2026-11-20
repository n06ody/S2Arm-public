// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
//
// CPU 频率托底的判定表：纯函数，没有线程、没有系统调用，宿主单测按模拟时间线直接跑。
//
// 做什么：游戏跑不到自己的目标帧率时，让一条低优先级线程在游戏线程所在的那组核上保持满载，
// 使这一组核的调频器（walt / schedutil 一类按利用率定频的策略）不再把频率压在上限以下。
// 为什么有效：这类游戏的主力线程各自只占一个核的一小部分，没有哪一个核的利用率高到把整组核拉满，
// 调频器就把组频率压在上限以下（0100F2C0115B6000 在 Adreno 840 上 10 秒采样：4 条线程各占一个核的 17–35%、
// 合计约一个核，目标簇按时间加权的均频是 scaling_max_freq 的 75–78%）；同一组核上有一条一直满载的线程时，
// 组频率贴着上限（同一采样 100%，4 条线程合计降到约 80%：同样的活在高频下占的核时间更少）。
//
// 判定输入全是「别处已经在量」的量：累计上屏帧数、游戏登记的目标出图周期、前台 / 显示面、
// 热状态与省电模式，以及目标簇的实际频率与各簇 scaling_max_freq。三种模式（属性 debug.s2a.cpufloor，
// 只有调试构建 + debug.s2a.debug_env=1 才认，玩家包恒为自适应）：
//   0 关；1 常开（门开即托，不看帧率与自检，A/B 与诊断用）；其它含未设 = 自适应：
//   ① 实际出图帧率连续 3 个 1 秒窗口低于目标的 90%，且
//   ② 目标簇近 3 个窗口的平均频率没有贴着 scaling_max_freq（< 95%：已经顶在上限就没有东西可托，不出手），才托底；
//   托底后至少保持 60 秒，到期时窗口仍然慢就续期，不慢了才松开；
//   松开后 2 分钟内又慢回来，保持时间翻倍（封顶 600 秒）；托底中连续 5 个窗口一帧都没出（暂停 / 卡死）就松开。
//   两条自检，任何一条触发就松开（多烧一个核换不来东西，就是纯亏）：
//   · 上限在掉：托底中连续 2 个窗口里有任何一个簇的 scaling_max_freq 比托底前低了 5% 以上（系统因发热 / 功耗在压上限）
//     → 这一局不再出手；
//   · 没有收益：托底 5 秒后的 10 个出图窗口的帧率，没有比托底前那 3 个慢窗口高出 5%（瓶颈不在 CPU 频率，或者这一段在载入）
//     → 暂缓 60 秒再试；再没有收益就翻倍（120 / 240 / 480 秒，封顶 600 秒），量出收益清零。
//     不用「本局不再出手」：真机 6 个 auto 局的第一次出手都落在载入阶段（托底前 10.3–16.0 → 托底后 4.6–9.6 fps），
//     一票否决会让之后的游玩段再也托不上。
// 任何模式下：不在前台、没有显示面、已停局，都不托底。CPU 托底还受 ADPF 会话状态约束。
// 频率 / 上限读不到（输入为 0）= 对应的判据不参与，不拦：只剩帧率判据与「没有收益」自检。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { S2A_FLOOR_OFF = 0, S2A_FLOOR_ALWAYS = 1, S2A_FLOOR_AUTO = 2 };

#define S2A_FLOOR_TICK_NS        250000000ull    // 判定周期
#define S2A_FLOOR_WINDOW_NS     1000000000ull    // 量帧窗口
#define S2A_FLOOR_ENTER_WINDOWS  3               // 连续几个慢窗口才托底（也是「近几个窗口」的窗口数）
#define S2A_FLOOR_IDLE_WINDOWS   5               // 托底中连续几个空窗口（一帧未出）就松开
#define S2A_FLOOR_HOLD_MIN_NS   60000000000ull   // 托底至少保持多久才允许按「不慢了」松开
#define S2A_FLOOR_HOLD_MAX_NS  600000000000ull   // 反复被打回时保持时间翻倍的封顶
#define S2A_FLOOR_REARM_NS     120000000000ull   // 松开后多久之内再慢回来算「反复」
// 慢 = 窗口内实际帧率 < 目标帧率的 9/10
#define S2A_FLOOR_LATE_NUM 9
#define S2A_FLOOR_LATE_DEN 10
// 参与「上限」读数的簇数上限（按容量分组，每组取首核）；下标 0 恒为目标簇（托底线程所在的那一组）
#define S2A_FLOOR_MAX_CAPS 8
#define S2A_FLOOR_CEIL_PERMILLE      950         // 均频 ≥ 上限的 95% = 已顶在上限
#define S2A_FLOOR_CAP_DROP_PERMILLE  950         // 上限 < 托底前的 95% = 系统在压上限
#define S2A_FLOOR_CAP_DROP_WINDOWS   2           // 连续几个窗口都被压才算（单窗口抖动不算）
#define S2A_FLOOR_BENEFIT_SETTLE_NS 5000000000ull   // 托底后先等 5 秒（调频器跟上、线程迁移稳定）再量
#define S2A_FLOOR_BENEFIT_WINDOWS   10           // 量 10 个有出图的窗口
#define S2A_FLOOR_BENEFIT_MIN_PCT   5            // 帧率至少比托底前高 5%
#define S2A_FLOOR_NOGAIN_BACKOFF_NS      60000000000ull    // 没有收益后至少暂缓多久再试
#define S2A_FLOOR_NOGAIN_BACKOFF_MAX_NS 600000000000ull    // 连着没有收益：每次翻倍，封顶

enum { S2A_FLOOR_WHY_NONE = 0, S2A_FLOOR_WHY_GATE, S2A_FLOOR_WHY_MODE, S2A_FLOOR_WHY_DONE,
       S2A_FLOOR_WHY_IDLE, S2A_FLOOR_WHY_CAP_DROP, S2A_FLOOR_WHY_NO_GAIN, S2A_FLOOR_WHY_ADPF };
enum { S2A_FLOOR_BLOCK_NONE = 0, S2A_FLOOR_BLOCK_CEILING, S2A_FLOOR_BLOCK_LATCHED, S2A_FLOOR_BLOCK_BACKOFF,
       S2A_FLOOR_BLOCK_ADPF };
enum { S2A_FLOOR_BENCH_OFF = 0, S2A_FLOOR_BENCH_SETTLE, S2A_FLOOR_BENCH_MEASURE, S2A_FLOOR_BENCH_DONE };

struct s2a_floor_in {
    uint64_t now_ns;       // 单调时钟
    uint64_t frames;       // 累计上屏帧数（只增；无符号回绕按差值算）
    uint64_t period_ns;    // 游戏登记的目标出图周期，0 = 还没登记
    int mode;              // S2A_FLOOR_*
    int foreground, surface, stopped;
    int thermal;           // 共用口径 0..3，-1 = 没读到
    int low_power;
    // 目标簇的实际频率：「频率(kHz) × 时间」与「时间」两个累计量（只增，时间单位任意但两者一致）；
    // 窗口内的均频 = Δ频率时间 / Δ时间。读不到时两者都不动（Δ时间 = 0 = 这个窗口没有读数）。
    uint64_t fq_work, fq_time;
    int ncaps;             // 有效的簇数（0 = 没读到任何上限）
    uint32_t max_khz[S2A_FLOOR_MAX_CAPS];   // 各簇当前 scaling_max_freq（kHz），[0] = 目标簇；0 = 这个簇没读到
};

// 一个收完的窗口的摘要
struct s2a_floor_win {
    uint64_t df, dt;       // 窗口内出图帧数与时长（ns）
    uint64_t fw, ft;       // 窗口内 Δ(频率 × 时间)、Δ时间；ft == 0 = 没读到
    uint32_t cap[S2A_FLOOR_MAX_CAPS];   // 窗口收尾时各簇的 scaling_max_freq
};

struct s2a_floor_state {
    int active;
    int forced;                        // 当前的「托底」来自常开模式
    int win_valid;
    int late_run, idle_run;            // 连续慢窗口数 / 托底中连续空窗口数
    uint64_t win_ns, win_frames;       // 当前窗口的起点时刻与起点处的累计帧数
    uint64_t win_fw, win_ft;           // 当前窗口起点处的累计频率时间 / 时间
    uint64_t since_ns, hold_ns;        // 本次托底的起点（续期时更新）与保持时长
    uint64_t released_ns, prev_hold_ns;// 上次按「不慢了」松开的时刻与当时的保持时长（0 = 没有）
    struct s2a_floor_win ring[S2A_FLOOR_ENTER_WINDOWS];   // 最近收完的窗口，ring[0] 最旧
    int ring_n;
    int latched;                       // 本局不再出手（上限被压）；开关切到 0 或 1 才清
    uint64_t nogain_until_ns;          // 没有收益之后：到这一刻之前不再试（0 = 没有）
    uint64_t nogain_backoff_ns;        // 上一次暂缓了多久（再没有收益就翻倍；量出收益清零；0 = 没有）
    int block, why;                    // 最近一次没出手的原因 / 最近一次松开的原因（日志用）
    int ceil_permille;                 // 最近一次「顶没顶在上限」的 均频 / 上限（千分数；-1 = 没读到）
    // 托底期间
    uint32_t base_cap[S2A_FLOOR_MAX_CAPS];  // 托底前近几个窗口里各簇上限的最小值（0 = 没读到）
    int base_n, drop_run;              // 对照的簇数 / 连续被压窗口数
    int cap_idx; uint32_t cap_from, cap_to;   // 触发「上限在掉」的簇与前后读数（日志用）
    int bench;                         // S2A_FLOOR_BENCH_*
    uint64_t pre_df, pre_dt, post_df, post_dt;   // 托底前 / 托底后两段的出图帧数与时长
    unsigned post_n;
    uint64_t pre_mfps, post_mfps;      // 判定时两段的帧率（毫帧 / 秒，日志用）
};

// 全部清零，连「本局不再出手」与「没有收益的暂缓」一起忘（开关被显式切到 0 / 1）
static inline void s2a_floor_reset_all(struct s2a_floor_state *st) { memset(st, 0, sizeof *st); }
// 一段判定 / 一次托底的状态清零，「本局不再出手」与暂缓留着（门开关、前后台切换不洗掉它们）
static inline void s2a_floor_reset_run(struct s2a_floor_state *st) {
    const int latched = st->latched;
    const uint64_t until = st->nogain_until_ns, backoff = st->nogain_backoff_ns;
    memset(st, 0, sizeof *st);
    st->latched = latched; st->nogain_until_ns = until; st->nogain_backoff_ns = backoff;
}

// 门：只看物理上托不了的（不在前台 / 没有显示面 / 已停）。热状态与省电模式不进门（11-13 维护者「不读电量、不设自己挡自己的闸」）：
//   真热到系统压频，「上限被压本局停用」那条照样会接住；两个读数只进日志
static inline int s2a_floor_gate_ok(const struct s2a_floor_in *in) {
    return in->foreground && in->surface && !in->stopped;
}

static inline void s2a_floor_open_window(struct s2a_floor_state *st, const struct s2a_floor_in *in) {
    st->win_ns = in->now_ns; st->win_frames = in->frames; st->win_valid = 1;
    st->win_fw = in->fq_work; st->win_ft = in->fq_time;
}

static inline void s2a_floor_ring_push(struct s2a_floor_state *st, const struct s2a_floor_win *w) {
    if (st->ring_n == S2A_FLOOR_ENTER_WINDOWS) {
        for (int i = 1; i < S2A_FLOOR_ENTER_WINDOWS; i++) st->ring[i - 1] = st->ring[i];
        st->ring_n--;
    }
    st->ring[st->ring_n++] = *w;
}

// 目标簇近几个窗口的均频 / 上限，千分数（可以超过 1000）；频率或上限读不到 / 数值不合理 = -1。
// 只统计「频率与上限都读到了」的窗口，分子分母取自同一批窗口。
static inline int s2a_floor_ceil_permille(const struct s2a_floor_state *st) {
    uint64_t fw = 0, ft = 0, capsum = 0, ncap = 0;
    for (int i = 0; i < st->ring_n; i++) {
        const struct s2a_floor_win *w = &st->ring[i];
        if (w->ft && w->cap[0]) { fw += w->fw; ft += w->ft; capsum += w->cap[0]; ncap++; }
    }
    if (!ft || !ncap) return -1;
    if (fw > (1ull << 44) || ft > (1ull << 30) || capsum > (1ull << 25)) return -1;   // 乘法不溢出的范围
    const uint64_t r = fw * ncap * 1000ull / (capsum * ft);
    return r > 2000 ? 2000 : (int)r;
}

// 托底中：有没有簇的当前上限比托底前低了 5% 以上（任何一边读不到的簇不算）。返回 1 并给出簇下标。
static inline int s2a_floor_caps_dropped(const struct s2a_floor_state *st, const struct s2a_floor_in *in, int *idx) {
    int n = st->base_n < in->ncaps ? st->base_n : in->ncaps;
    if (n > S2A_FLOOR_MAX_CAPS) n = S2A_FLOOR_MAX_CAPS;
    for (int i = 0; i < n; i++) {
        if (!st->base_cap[i] || !in->max_khz[i]) continue;
        if ((uint64_t)in->max_khz[i] * 1000ull < (uint64_t)st->base_cap[i] * S2A_FLOOR_CAP_DROP_PERMILLE) { *idx = i; return 1; }
    }
    return 0;
}

// 托底后的帧率是否比托底前高出 MIN_PCT：post_df / post_dt ≥ pre_df / pre_dt × (100 + MIN_PCT) / 100（整数交叉相乘）。
static inline int s2a_floor_gain_ok(uint64_t pre_df, uint64_t pre_dt, uint64_t post_df, uint64_t post_dt) {
    return post_df * pre_dt * 100ull >= pre_df * post_dt * (100ull + S2A_FLOOR_BENEFIT_MIN_PCT);
}
static inline uint64_t s2a_floor_mfps(uint64_t df, uint64_t dt_ns) { return dt_ns ? df * 1000000000000ull / dt_ns : 0; }

// 松开一次托底（不洗掉窗口 / 环 / 本局的「不再出手」）
static inline void s2a_floor_release(struct s2a_floor_state *st, int why) {
    st->active = 0; st->late_run = st->idle_run = 0; st->bench = S2A_FLOOR_BENCH_OFF; st->drop_run = 0; st->why = why;
}

// 每个判定周期调一次，返回此刻该不该托底；状态都在 st 里。
static inline int s2a_floor_step(struct s2a_floor_state *st, const struct s2a_floor_in *in) {
    if (in->mode == S2A_FLOOR_OFF) {
        const int was = st->active;
        s2a_floor_reset_all(st); st->why = was ? S2A_FLOOR_WHY_MODE : S2A_FLOOR_WHY_NONE;
        return 0;
    }
    if (!s2a_floor_gate_ok(in)) {
        const int was = st->active;
        s2a_floor_reset_run(st); st->why = was ? S2A_FLOOR_WHY_GATE : S2A_FLOOR_WHY_NONE;
        return 0;
    }
    if (in->mode == S2A_FLOOR_ALWAYS) {
        if (!st->forced) { s2a_floor_reset_all(st); st->forced = 1; }   // 常开是显式指令：不带「本局不再出手」
        st->active = 1;
        return 1;
    }
    if (st->forced) s2a_floor_reset_run(st);                            // 从常开切回自适应：从零开始
    if (!st->win_valid || in->now_ns < st->win_ns) {   // 第一次，或时钟倒退：重开窗口，连续数不带过去
        st->late_run = st->idle_run = 0;               // （窗口环不用清：出手要连着 3 个新的慢窗口，环会被新窗口填满）
        if (st->active) st->since_ns = in->now_ns;
        // 暂缓到点的时刻是按旧时钟记的：时钟倒退后最多再等一个暂缓长度，不会被倒退的那一段拖长
        if (st->nogain_until_ns > in->now_ns + st->nogain_backoff_ns) st->nogain_until_ns = in->now_ns + st->nogain_backoff_ns;
        s2a_floor_open_window(st, in);
        return st->active;
    }
    const uint64_t dt = in->now_ns - st->win_ns;
    if (dt < S2A_FLOOR_WINDOW_NS) return st->active;
    const uint64_t df = in->frames - st->win_frames;   // 无符号差：计数回绕也对
    const int idle = df == 0;
    // 帧率 < 目标 * 9/10 ⇔ df / dt < 0.9 / period ⇔ df * period * 10 < dt * 9（整数，无浮点）
    const int late = in->period_ns && df &&
        df * in->period_ns * S2A_FLOOR_LATE_DEN < dt * S2A_FLOOR_LATE_NUM;
    struct s2a_floor_win w;
    memset(&w, 0, sizeof w);
    w.df = df; w.dt = dt;
    if (in->fq_time > st->win_ft && in->fq_work >= st->win_fw) {   // 累计只增且有进展才算读到（计数被清零 / 回退 = 这个窗口没读数）
        w.fw = in->fq_work - st->win_fw; w.ft = in->fq_time - st->win_ft;
    }
    for (int i = 0; i < in->ncaps && i < S2A_FLOOR_MAX_CAPS; i++) w.cap[i] = in->max_khz[i];
    s2a_floor_ring_push(st, &w);
    s2a_floor_open_window(st, in);

    if (!st->active) {
        if (late) { if (st->late_run < 1000) st->late_run++; } else st->late_run = 0;
        if (st->late_run < S2A_FLOOR_ENTER_WINDOWS) { st->block = S2A_FLOOR_BLOCK_NONE; return 0; }
        if (st->latched) { st->block = S2A_FLOOR_BLOCK_LATCHED; return 0; }
        if (in->now_ns < st->nogain_until_ns) { st->block = S2A_FLOOR_BLOCK_BACKOFF; return 0; }   // 刚试过没有收益：暂缓期内不再试；慢窗口数留着，到点接着判
        st->ceil_permille = s2a_floor_ceil_permille(st);
        if (st->ceil_permille >= S2A_FLOOR_CEIL_PERMILLE) {   // 已顶在上限：没有东西可托。慢窗口数留着，下个窗口接着判
            st->block = S2A_FLOOR_BLOCK_CEILING;
            return 0;
        }
        st->block = S2A_FLOOR_BLOCK_NONE;
        const uint64_t doubled = st->prev_hold_ns * 2 > S2A_FLOOR_HOLD_MAX_NS ? S2A_FLOOR_HOLD_MAX_NS : st->prev_hold_ns * 2;
        st->hold_ns = st->released_ns && in->now_ns - st->released_ns <= S2A_FLOOR_REARM_NS && doubled > S2A_FLOOR_HOLD_MIN_NS
            ? doubled : S2A_FLOOR_HOLD_MIN_NS;
        st->since_ns = in->now_ns; st->active = 1; st->why = S2A_FLOOR_WHY_NONE;
        st->late_run = st->idle_run = 0;
        // 托底前的账：近几个慢窗口的帧率（给「有没有收益」做对照）、各簇上限的最小值（给「上限在不在掉」做对照）
        st->pre_df = st->pre_dt = st->post_df = st->post_dt = 0; st->post_n = 0;
        for (int i = 0; i < st->ring_n; i++) { st->pre_df += st->ring[i].df; st->pre_dt += st->ring[i].dt; }
        st->base_n = in->ncaps < S2A_FLOOR_MAX_CAPS ? in->ncaps : S2A_FLOOR_MAX_CAPS;
        for (int c = 0; c < S2A_FLOOR_MAX_CAPS; c++) {
            uint32_t m = 0;
            for (int i = 0; i < st->ring_n; i++) { const uint32_t v = st->ring[i].cap[c]; if (v && (!m || v < m)) m = v; }
            st->base_cap[c] = m;
        }
        st->drop_run = 0; st->bench = S2A_FLOOR_BENCH_SETTLE;
        return 1;
    }
    st->idle_run = idle ? st->idle_run + 1 : 0;
    if (st->idle_run >= S2A_FLOOR_IDLE_WINDOWS) {      // 没在出图：松开，不算「反复」
        s2a_floor_release(st, S2A_FLOOR_WHY_IDLE); st->released_ns = st->prev_hold_ns = 0;
        return 0;
    }
    int idx = 0;
    if (s2a_floor_caps_dropped(st, in, &idx)) {        // 自检一：系统在压上限，多烧的核换来了降档——松开，本局不再出手
        if (++st->drop_run >= S2A_FLOOR_CAP_DROP_WINDOWS) {
            st->cap_idx = idx; st->cap_from = st->base_cap[idx]; st->cap_to = in->max_khz[idx];
            s2a_floor_release(st, S2A_FLOOR_WHY_CAP_DROP); st->latched = 1; st->released_ns = st->prev_hold_ns = 0;
            return 0;
        }
    } else st->drop_run = 0;
    if (st->bench == S2A_FLOOR_BENCH_SETTLE) {         // 自检二：先等调频器跟上，再量 10 个出图窗口
        if (in->now_ns - st->since_ns >= S2A_FLOOR_BENEFIT_SETTLE_NS) st->bench = S2A_FLOOR_BENCH_MEASURE;
    } else if (st->bench == S2A_FLOOR_BENCH_MEASURE) {
        if (!idle) { st->post_df += df; st->post_dt += dt; st->post_n++; }
        if (st->post_n >= S2A_FLOOR_BENEFIT_WINDOWS) {
            st->pre_mfps = s2a_floor_mfps(st->pre_df, st->pre_dt); st->post_mfps = s2a_floor_mfps(st->post_df, st->post_dt);
            if (!s2a_floor_gain_ok(st->pre_df, st->pre_dt, st->post_df, st->post_dt)) {   // 帧率没动：瓶颈不在 CPU 频率（或这一段在载入）
                const uint64_t twice = st->nogain_backoff_ns * 2;
                st->nogain_backoff_ns = !st->nogain_backoff_ns ? S2A_FLOOR_NOGAIN_BACKOFF_NS
                                      : twice > S2A_FLOOR_NOGAIN_BACKOFF_MAX_NS ? S2A_FLOOR_NOGAIN_BACKOFF_MAX_NS : twice;
                st->nogain_until_ns = in->now_ns + st->nogain_backoff_ns;
                s2a_floor_release(st, S2A_FLOOR_WHY_NO_GAIN); st->released_ns = st->prev_hold_ns = 0;
                return 0;
            }
            st->bench = S2A_FLOOR_BENCH_DONE;
            st->nogain_backoff_ns = st->nogain_until_ns = 0;   // 量出收益了：之前攒的暂缓清零
        }
    }
    if (in->now_ns - st->since_ns >= st->hold_ns) {
        if (late) st->since_ns = in->now_ns;            // 带着托底还慢（但比托底前快，自检已确认）：续期
        else {
            s2a_floor_release(st, S2A_FLOOR_WHY_DONE); st->late_run = 0;
            st->released_ns = in->now_ns; st->prev_hold_ns = st->hold_ns;
        }
    }
    return st->active;
}

// 只有 CPU 托底受 ADPF 约束。会话关闭后重新量取窗口，保留本局退避记录。
static inline int s2a_floor_cpu_step(struct s2a_floor_state *st, const struct s2a_floor_in *in, int adpf_active) {
    if (!adpf_active || in->mode == S2A_FLOOR_OFF) return s2a_floor_step(st, in);
    const int was_active = st->active;
    s2a_floor_reset_run(st);
    st->why = was_active ? S2A_FLOOR_WHY_ADPF : S2A_FLOOR_WHY_NONE;
    st->block = S2A_FLOOR_BLOCK_ADPF;
    return 0;
}

// 选哪一组核托底：按 cpu_capacity 分组（容量相同 = 同一档核），取「核数 × 容量」最大的一组，
// 总容量相同取容量更高的；游戏线程多数落在这一组。返回 CPU 位掩码（最多 64 个核）；
// 容量缺失 / 为 0 / 只有一档 / 超过 64 个核 = 返回 0，调用方不钉核。
static inline uint64_t s2a_floor_pick_cpus(const unsigned *cap, int n) {
    if (!cap || n <= 0 || n > 64) return 0;
    for (int i = 0; i < n; i++) if (!cap[i]) return 0;
    uint64_t best_mask = 0, best_score = 0; unsigned best_cap = 0; int groups = 0;
    for (int i = 0; i < n; i++) {
        int seen = 0;
        for (int j = 0; j < i; j++) if (cap[j] == cap[i]) { seen = 1; break; }
        if (seen) continue;
        groups++;
        uint64_t mask = 0, cnt = 0;
        for (int j = i; j < n; j++) if (cap[j] == cap[i]) { mask |= 1ull << j; cnt++; }
        const uint64_t score = cnt * cap[i];
        if (score > best_score || (score == best_score && cap[i] > best_cap)) {
            best_score = score; best_cap = cap[i]; best_mask = mask;
        }
    }
    return groups < 2 ? 0 : best_mask;
}

// 各档核的首核（读各簇频率上限用）：目标组排第 0 个，其余按核号升序。返回个数（≤ max）；
// target_mask 为 0（没分组 / 读不到容量）= 只返回核 0。
static inline int s2a_floor_cap_cpus(const unsigned *cap, int n, uint64_t target_mask, int *out, int max) {
    if (!out || max < 1) return 0;
    if (!cap || n <= 0 || n > 64 || !target_mask) { out[0] = 0; return 1; }
    int t = 0;
    while (t < 63 && !(target_mask >> t & 1)) t++;
    int k = 0;
    out[k++] = t;
    for (int i = 0; i < n && k < max; i++) {
        if (cap[i] == cap[t]) continue;                 // 目标组已排在第 0
        int seen = 0;
        for (int j = 0; j < i; j++) if (cap[j] == cap[i]) { seen = 1; break; }
        if (!seen) out[k++] = i;
    }
    return k;
}

// 解析 cpufreq 的 stats/time_in_state：每行「频率(kHz) 时间(10 ms 单位)」，各行累计。
// 给出 Σ(频率 × 时间) 与 Σ时间；返回行数，格式不认 / 没有行 / 数值离谱 = 0。
static inline int s2a_floor_parse_tis(const char *s, size_t len, uint64_t *work, uint64_t *time) {
    uint64_t w = 0, t = 0;
    int lines = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
        if (i >= len || s[i] == '\0') break;
        uint64_t khz = 0, ticks = 0; int d1 = 0, d2 = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9') { khz = khz * 10 + (uint64_t)(s[i] - '0'); i++; d1++; if (khz > (1ull << 24)) return 0; }
        while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') { ticks = ticks * 10 + (uint64_t)(s[i] - '0'); i++; d2++; if (ticks > (1ull << 32)) return 0; }
        if (!d1 || !d2) return 0;
        while (i < len && s[i] != '\n') i++;           // 有的内核每行后面还有别的列：忽略
        if (++lines > 128) return 0;
        w += khz * ticks; t += ticks;
    }
    if (!lines) return 0;
    *work = w; *time = t;
    return lines;
}

// ---- 帧间隔直方图（只出读数，不参与判定）----
// 桶上沿（ms）：≤17（60 帧档）/ ≤25 / ≤34（30 帧档）/ ≤42 / ≤51 / ≤67 / ≤100 / 超过 100。
// 每帧一次计数，每 5 秒由托底线程取走并打一行：开关前后同一份读数，A/B 不用外部抓取工具。
#define S2A_FLOOR_NBUCKET 8
#define S2A_FLOOR_SLOW_BUCKET 3      // 下标 ≥ 3（> 34 ms）的桶算「慢帧」：30 帧档错过一个刷新
#define S2A_FLOOR_STAT_NS 5000000000ull

static inline unsigned s2a_floor_bucket_limit_ms(int i) {
    static const unsigned lim[S2A_FLOOR_NBUCKET - 1] = {17, 25, 34, 42, 51, 67, 100};
    return i >= 0 && i < S2A_FLOOR_NBUCKET - 1 ? lim[i] : 0;   // 0 = 最后一桶，没有上沿
}
static inline int s2a_floor_bucket(uint64_t interval_ns) {
    for (int i = 0; i < S2A_FLOOR_NBUCKET - 1; i++)
        if (interval_ns <= (uint64_t)s2a_floor_bucket_limit_ms(i) * 1000000ull) return i;
    return S2A_FLOOR_NBUCKET - 1;
}
// 第 q（千分数，p99 = 990）分位所在的桶下标；没有样本返回 -1。
static inline int s2a_floor_hist_quantile(const unsigned *h, unsigned q_permille) {
    uint64_t total = 0;
    for (int i = 0; i < S2A_FLOOR_NBUCKET; i++) total += h[i];
    if (!total) return -1;
    uint64_t rank = (total * q_permille + 999) / 1000;   // 向上取整：至少覆盖这么多个样本
    if (rank < 1) rank = 1;
    uint64_t cum = 0;
    for (int i = 0; i < S2A_FLOOR_NBUCKET; i++) { cum += h[i]; if (cum >= rank) return i; }
    return S2A_FLOOR_NBUCKET - 1;
}
static inline uint64_t s2a_floor_hist_slow(const unsigned *h) {
    uint64_t n = 0;
    for (int i = S2A_FLOOR_SLOW_BUCKET; i < S2A_FLOOR_NBUCKET; i++) n += h[i];
    return n;
}
