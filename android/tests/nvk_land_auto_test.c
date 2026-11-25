// Full present streams with independent fixed interval/pressure/workload inputs.
#include "nvn_vk_land_auto.h"
#include "../native/gfx_plat/flag_cache.h"
#include <stdio.h>
#define CHECK(x)                                      \
  do {                                                \
    if (!(x)) {                                       \
      fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); \
      return 1;                                       \
    }                                                 \
  } while (0)
#include "nvk_land_auto_fixture.h"

static int session_logs(int stop_at) {
  memset(&g, 0, sizeof g);
  g.land_auto = g.land_readback = 1;
  lines = 0;
  uint64_t now = LA_SECOND, draws = 0;
  for (unsigned i = 0; i < 70; i++) {
    now += LA_SECOND;
    nvn_gpu_land_auto_present(now, 1, 1, 0, 1);
  }
  CHECK(!lines && g.land_trial.qualified == 0 && !g.land_trial.mode);
  now += 6 * LA_SECOND;
  for (int i = 0; i < 2000 && g.land_trial.qualified < stop_at; i++) {
    int on = g.land_trial.mode;
    now += on ? 40000000 : 50000000;
    draws += 100;
    g.land_trial.counts.bytes += 100;
    if (on) {
      g.land_trial.counts.staged++;
      g.land_trial.counts.staged_bytes += 100;
    }
    land_auto_check_gap_locked(now);
    nvn_gpu_land_auto_present(now, 1, 1, draws, 1);
  }
  CHECK(g.land_trial.qualified == stop_at);
  if (stop_at == LA_WINDOWS) {
    CHECK(lines == 1 && strstr(decision, "phase=lock mode=1 reason=paired_gain"));
  } else {
    CHECK(!lines && g.land_trial.mode == (stop_at == LA_WARM));  // Zero samples, or a provisional on-window.
  }
  uint64_t locked = g.land_trial.decision_boottime_ns;
  nvn_vk_land_auto_report_exit();
  unsigned after_exit = lines;
  land_auto_exit_locked();
  CHECK(lines == after_exit && lines == (stop_at == LA_WINDOWS ? 2u : 1u));
  const char* stamp = strstr(decision, " decision_boottime_ns=");
  CHECK(stamp && strtoull(stamp + strlen(" decision_boottime_ns="), NULL, 10) == g.land_trial.decision_boottime_ns);
#ifdef CLOCK_BOOTTIME
  CHECK(g.land_trial.decision_boottime_ns > 0);
#endif
  if (stop_at == LA_WINDOWS) {
    CHECK(strstr(decision, "phase=exit mode=1 reason=paired_gain") &&
          strstr(decision, "qualified_windows=11 ") && strstr(decision, "w8R1=125/") &&
          locked == g.land_trial.decision_boottime_ns);
  } else {
    CHECK(strstr(decision, "phase=exit mode=0 reason=insufficient_samples") &&
          strstr(decision, stop_at ? "qualified_windows=7 " : "qualified_windows=0 ") && !g.land_trial.mode);
  }
  return 0;
}
enum interrupt { NONE, ELIGIBLE, NO_DRAW, SPARSE, GAP, STALE_EPOCH, BOUNDARY_BYTES, THERMAL };
static int direct_only(enum interrupt interruption) {
  memset(&g, 0, sizeof g);
  g.land_auto = g.land_readback = 1;
  lines = thermal = 0;
  uint64_t now = LA_SECOND, draws = 0;
  int expected_window = 10;  // Zero-based index of the eleventh full direct-only window.
  if (interruption != NONE) {
    // A whole eligible, drawless or sparse window at index 10 cannot count.
    expected_window += interruption == ELIGIBLE || interruption == NO_DRAW || interruption == SPARSE ? 11 : 10;
  }
  for (int window = 0; window <= expected_window; window++) {
    int interrupt = window == 10 ? interruption : NONE;
    if (interrupt == GAP)
      now += 5 * LA_SECOND;
    if (interrupt == THERMAL || interrupt == STALE_EPOCH) {
      thermal += interrupt == THERMAL;
      g.land_prior_live = interrupt == STALE_EPOCH;
      nvn_gpu_land_auto_present(++now, 1, 1, draws, 1);
      g.land_prior_live = 0;
    }
    if (interrupt == BOUNDARY_BYTES)
      g.land_trial.counts.bytes += 8192;  // Completed between the previous end and this baseline.
    int intervals = interrupt == SPARSE ? 25 : 300;
    for (int frame = 0; frame <= intervals; frame++) {
      now += (5 * LA_SECOND + intervals - 1) / intervals;
      draws += interrupt == NO_DRAW ? 0 : 1000;
      g.land_trial.counts.direct++;
      if (interrupt == ELIGIBLE)
        g.land_trial.counts.bytes += 8192;
      nvn_gpu_land_auto_present(now, 1, 1, draws, 1);
    }
    if (window < expected_window)
      CHECK(!lines && !g.land_trial.reason);
    else
      CHECK(lines == 1 && strstr(decision, "phase=lock mode=0 reason=no_completed_staging "));
  }
  CHECK(g.land_trial.qualified == (interruption == ELIGIBLE ? 1 : 0));
  while (now < 1800 * LA_SECOND) {
    now += 16666667;
    nvn_gpu_land_auto_present(now, 1, 1, ++draws, 1);
  }
  CHECK(lines == 1 && !g.land_trial.mode);
  uint64_t locked = g.land_trial.decision_boottime_ns;
  nvn_vk_land_auto_report_exit();
  land_auto_exit_locked();
  CHECK(lines == 2 && strstr(decision, "phase=exit mode=0 reason=no_completed_staging "));
  CHECK(g.land_trial.decision_boottime_ns == locked);
  return 0;
}
enum scenario {
  GAIN,
  LOSS,
  NOISE,
  NO_STAGE,
  PRESSURE,
  SECOND_LOSS,
  SECOND_EMPTY,
  HIGH_P99,
  WORKLOAD,
  SHORT_WINDOW,
  NO_GAIN,
  TAIL,
  STALE
};
static int run(enum scenario scenario, const char* reason) {
  struct la_trial t = {0};
  uint64_t now = LA_SECOND, draws = 0;
  // Cold start: sparse load presents for over a minute, then a full-rate black window.
  for (unsigned i = 0; i < 71; i++) {
    t.counts.bytes += 100;
    la_frame(&t, now += LA_SECOND, 1, 1, ++draws, 0);
  }
  CHECK(t.rejected > 0 && !t.qualified && !t.reason);
  la_frame(&t, now, 0, 0, draws, 1);
  for (unsigned i = 0; i < 301; i++)
    la_frame(&t, now += 20000000, 1, 1, draws, 0);
  CHECK(t.rejected > 0 && t.index == 0 && t.mode == 0 && !t.reason);
  la_frame(&t, now, 0, 0, draws, 1);  // Drain between the loading and field workload.
  const uint64_t gameplay = now;
  while (!t.reason && now - gameplay < 62 * LA_SECOND) {
    int on = t.mode;
    uint64_t dt = on ? 40000000 : 50000000, pressure = 1000000;
    if (scenario == NOISE)
      dt = on ? 46000000 : t.index < LA_AA && (t.index % 2) ? 45000000 : 50000000;
    if (scenario == LOSS || (scenario == SECOND_LOSS && t.index == LA_ON2))
      dt = on ? 60000000 : 50000000;
    if (scenario == NO_GAIN)
      dt = 50000000;
    if (scenario == SHORT_WINDOW)
      dt = 110000000;
    if (scenario == PRESSURE && on)
      pressure += 1000000;
    if (scenario == TAIL && on && t.start && now - t.start >= 4 * LA_SECOND)
      dt = 65000000;
    if (scenario == HIGH_P99 && on && t.n < 75 && t.n % 25 == 0)
      dt = 90000000;
    now += dt;
    draws += 100;
    t.counts.bytes += scenario == WORKLOAD && on ? 200 : 100;
    t.counts.spans++;
    t.counts.subs++;
    if (on && scenario != NO_STAGE && !(scenario == SECOND_EMPTY && t.index == LA_ON2)) {
      t.counts.staged++;
      t.counts.staged_bytes += 100;
    }
    la_frame(&t, now, pressure, pressure, draws, scenario == STALE && on);
  }
  if (!t.reason) la_end(&t);
  if (!t.reason || strcmp(t.reason, reason))
    fprintf(stderr, "case %d: expected %s got %s (index %d)\n", scenario, reason,
            t.reason ? t.reason : "none", t.index);
  CHECK(t.reason && !strcmp(t.reason, reason));
  CHECK(t.mode == (scenario == GAIN));
  if (scenario == GAIN) {
    CHECK(t.qualified == LA_WINDOWS);
    CHECK(t.windows[LA_BASE].frames == 100 && t.windows[LA_ON1].frames == 125);
    CHECK(t.windows[LA_BASE].m[LA_FPS] == 20 && t.windows[LA_ON1].m[LA_FPS] == 25);
    CHECK(t.windows[LA_WARM].work.staged && t.windows[LA_ON2].work.staged);
  }
  int locked = t.mode;
  la_frame(&t, now + 120 * LA_SECOND, 0, 0, draws, 0);
  CHECK(t.mode == locked);
  return 0;
}
int main(void) {
  CHECK(fc_value("nvk_land_readback", "auto") == 2 && fc_value("nvk_land_readback", "1") == 1);
  CHECK(fc_value("nvk_land_readback", "0") == 0 && fc_value("nvk_land_readback", "2") == 0);
  CHECK(fc_value("unrelated", "auto") == 0 && fc_value("unrelated", "12") == 12);
  struct la_trial t = {0};
  la_frame(&t, 90000000000ull, 0, 0, 0, 0);
  CHECK(t.start == 90000000000ull && !t.mode && !t.reason);
  la_end(&t);
  CHECK(!t.mode && !strcmp(t.reason, "insufficient_samples") && t.qualified == 0);
  for (int i = NONE; i <= THERMAL; i++)
    CHECK(!direct_only((enum interrupt)i));
  CHECK(!session_logs(0));
  CHECK(!session_logs(LA_WINDOWS));
  CHECK(!session_logs(LA_WARM));
  CHECK(!run(GAIN, "paired_gain"));
  CHECK(!run(LOSS, "fps_within_noise"));
  CHECK(!run(NO_GAIN, "fps_within_noise"));
  CHECK(!run(NOISE, "fps_within_noise"));
  CHECK(!run(NO_STAGE, "no_completed_staging"));
  CHECK(!run(SECOND_EMPTY, "no_completed_staging"));
  CHECK(!run(HIGH_P99, "frame_tail"));
  CHECK(!run(PRESSURE, "present_pressure"));
  CHECK(!run(SECOND_LOSS, "fps_within_noise"));
  CHECK(!run(WORKLOAD, "changed_workload"));
  CHECK(!run(SHORT_WINDOW, "insufficient_samples"));
  CHECK(!run(TAIL, "tail_fps"));
  CHECK(!run(STALE, "insufficient_samples"));
  puts(
      "PASS land auto: cold start, both pairs, AA noise, frame coverage, actual staging, tail, WSI, "
      "loading, 11 consecutive direct-only windows, retained exit and insufficient samples");
  return 0;
}
