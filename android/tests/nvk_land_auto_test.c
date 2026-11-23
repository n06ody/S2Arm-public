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
  t.arm = now;
  t.armed = 1;
  while (!t.reason && now - t.arm < 61 * LA_SECOND) {
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
  if (!t.reason || strcmp(t.reason, reason))
    fprintf(stderr, "case %d: expected %s got %s (index %d)\n", scenario, reason,
            t.reason ? t.reason : "none", t.index);
  CHECK(t.reason && !strcmp(t.reason, reason));
  CHECK(t.mode == (scenario == GAIN));
  if (scenario == GAIN) {
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
  CHECK(la_arm_edge(0, 141, 141));
  CHECK(!la_arm_edge(141, 141, 141) && !la_arm_edge(0, 142, 141) && !la_arm_edge(0, 0, 141));
  CHECK(fc_value("nvk_land_readback", "auto") == 2 && fc_value("nvk_land_readback", "1") == 1);
  CHECK(fc_value("nvk_land_readback", "0") == 0 && fc_value("nvk_land_readback", "2") == 0);
  CHECK(fc_value("unrelated", "auto") == 0 && fc_value("unrelated", "12") == 12);
  struct la_trial t = {0};
  la_frame(&t, 90000000000ull, 0, 0, 0, 0);
  CHECK(!t.armed && !t.mode && !t.reason);
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
  CHECK(!run(SHORT_WINDOW, "budget"));
  CHECK(!run(TAIL, "tail_fps"));
  CHECK(!run(STALE, "budget"));
  puts(
      "PASS land auto: arm gate, both pairs, AA noise, frame coverage, actual staging, tail, WSI, "
      "load and budget");
  return 0;
}
