// Session-local readback experiment. All counters and sample times are monotonic.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
enum {
  LA_AA = 6,
  LA_BASE = 6,
  LA_WARM = 7,
  LA_ON1 = 8,
  LA_ON2 = 9,
  LA_OFF2 = 10,
  LA_WINDOWS = 11,
  LA_MAX_FRAMES = 2048,
  LA_MIN_FRAMES = 50
};
#define LA_SECOND UINT64_C(1000000000)
enum la_metric {
  LA_FPS,
  LA_TAIL_FPS,
  LA_P99,
  LA_TAIL_P99,
  LA_LONG33,
  LA_LONG100,
  LA_TAIL33,
  LA_TAIL100,
  LA_ACQ,
  LA_ACQ95,
  LA_ACQ99,
  LA_PRESENT,
  LA_PRESENT95,
  LA_PRESENT99,
  LA_METRICS
};
struct la_counts {
  uint64_t bytes, spans, subs, staged, staged_bytes, direct, allocs;
};
struct la_window {
  double m[LA_METRICS], draws;
  struct la_counts work;
  unsigned frames;
};
struct la_frame {
  uint64_t dt, acquire, present;
};
struct la_trial {
  struct la_window windows[LA_WINDOWS];
  double noise[LA_METRICS];
  struct la_frame frames[LA_MAX_FRAMES];
  struct la_counts counts, initial;
  uint64_t arm, start, prev, draws, life, compile;
  int thermal, target, index, armed, mode, rejected, epoch;
  unsigned n;
  const char* reason;
};
static inline int la_arm_edge(int previous, int signal, int pid) {
  return pid > 0 && previous != pid && signal == pid;
}
static inline void la_lock(struct la_trial* t, int on, const char* reason) {
  if (t->reason)
    return;
  t->mode = on;
  t->reason = reason;
}
static inline int la_on_window(int index) {
  return index >= LA_WARM && index <= LA_ON2;
}
static inline int la_cmp(const void* a, const void* b) {
  uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
  return (x > y) - (x < y);
}
static inline double la_quantile(const uint64_t* sorted, unsigned n, unsigned percent) {
  return sorted[(n * percent + 99) / 100 - 1] / 1e6;
}
static inline int la_close(double a, double b) {
  return a > 0 && b > 0 && fabs(a - b) <= .15 * fmin(a, b);
}
static inline int la_comparable(const struct la_window* a, const struct la_window* b) {
  return a->frames && b->frames && la_close(a->draws, b->draws) &&
         la_close((double)a->work.bytes / a->frames, (double)b->work.bytes / b->frames);
}
static inline const char* la_calibrate(struct la_trial* t) {
  for (int i = 0; i < LA_AA; i += 2) {
    const struct la_window *a = &t->windows[i], *b = a + 1;
    if (!la_comparable(a, b))
      return "AA_workload";
    for (unsigned k = 0; k < LA_METRICS; k++) {
      double delta = fabs(a->m[k] - b->m[k]);
      if (k == LA_FPS)
        delta /= fmin(a->m[k], b->m[k]);
      // Nearest-rank p95 across three distinct R0/R0 pairs is the maximum.
      t->noise[k] = fmax(t->noise[k], delta);
    }
  }
  return NULL;
}
static inline const char* la_compare(const struct la_window* off, const struct la_window* on,
                                     const double* noise) {
  if (!on->work.staged || !on->work.staged_bytes)
    return "no_completed_staging";
  if (!la_comparable(off, on))
    return "changed_workload";
  if (on->m[LA_FPS] <= off->m[LA_FPS] * (1 + fmax(.05, noise[LA_FPS])))
    return "fps_within_noise";
  if (off->m[LA_TAIL_FPS] - on->m[LA_TAIL_FPS] > noise[LA_TAIL_FPS])
    return "tail_fps";
  for (unsigned k = LA_P99; k < LA_METRICS; k++)
    if (on->m[k] - off->m[k] > noise[k])
      return k >= LA_ACQ ? "present_pressure" : "frame_tail";
  return NULL;
}
static inline void la_finish_window(struct la_trial* t, uint64_t now, uint64_t draws) {
  struct la_window* w = &t->windows[t->index];
  unsigned n = t->n, tail = n;
  uint64_t elapsed = 0, tail_ns = 0, acq = 0, present = 0;
  uint64_t v[LA_MAX_FRAMES];
  for (unsigned i = 0; i < n; i++) {
    struct la_frame f = t->frames[i];
    // Last one second of complete intervals; a straddling interval is excluded.
    if (elapsed >= now - t->start - LA_SECOND && tail == n)
      tail = i;
    elapsed += f.dt;
    if (i >= tail) {
      tail_ns += f.dt;
      w->m[LA_TAIL33] += f.dt > 33000000;
      w->m[LA_TAIL100] += f.dt > 100000000;
    }
    w->m[LA_LONG33] += f.dt > 33000000;
    w->m[LA_LONG100] += f.dt > 100000000;
    acq += f.acquire;
    present += f.present;
    v[i] = f.dt;
  }
  if (tail == n || !tail_ns || draws < t->draws) {
    la_lock(t, 0, "tail_coverage");
    return;
  }
  qsort(v + tail, n - tail, sizeof *v, la_cmp);
  w->m[LA_TAIL_P99] = la_quantile(v + tail, n - tail, 99);
  qsort(v, n, sizeof *v, la_cmp);
  w->m[LA_P99] = la_quantile(v, n, 99);
  for (unsigned i = 0; i < n; i++)
    v[i] = t->frames[i].acquire;
  qsort(v, n, sizeof *v, la_cmp);
  w->m[LA_ACQ95] = la_quantile(v, n, 95);
  w->m[LA_ACQ99] = la_quantile(v, n, 99);
  for (unsigned i = 0; i < n; i++)
    v[i] = t->frames[i].present;
  qsort(v, n, sizeof *v, la_cmp);
  w->m[LA_PRESENT95] = la_quantile(v, n, 95);
  w->m[LA_PRESENT99] = la_quantile(v, n, 99);
  w->m[LA_ACQ] = acq / (1e6 * n);
  w->m[LA_PRESENT] = present / (1e6 * n);
  w->m[LA_FPS] = n * 1e9 / elapsed;
  w->m[LA_TAIL_FPS] = (n - tail) * 1e9 / tail_ns;
  w->m[LA_LONG33] /= n;
  w->m[LA_LONG100] /= n;
  w->m[LA_TAIL33] /= n - tail;
  w->m[LA_TAIL100] /= n - tail;
  w->frames = n;
  w->draws = (double)(draws - t->draws) / n;
#define LA_DELTA(k) w->work.k = t->counts.k - t->initial.k
  LA_DELTA(bytes);
  LA_DELTA(spans);
  LA_DELTA(subs);
  LA_DELTA(staged);
  LA_DELTA(staged_bytes);
  LA_DELTA(direct);
  LA_DELTA(allocs);
#undef LA_DELTA
  const char* reason = NULL;
  if (t->index == LA_AA - 1)
    reason = la_calibrate(t);
  if (t->index == LA_WARM && !w->work.staged)
    reason = "no_completed_staging";
  if ((t->index == LA_ON1 || t->index == LA_ON2) && w->work.allocs)
    reason = "staging_growth";
  if (t->index == LA_OFF2) {
    for (unsigned i = 0; i < LA_AA; i++)
      if (!la_comparable(&t->windows[i], &t->windows[LA_BASE]))
        reason = "AA_workload";
    if (!la_comparable(&t->windows[LA_BASE], w))
      reason = "changed_workload";
    if (!reason)
      reason = la_compare(&t->windows[LA_BASE], &t->windows[LA_ON1], t->noise);
    if (!reason)
      reason = la_compare(w, &t->windows[LA_ON2], t->noise);
    la_lock(t, !reason, reason ? reason : "paired_gain");
  } else if (reason)
    la_lock(t, 0, reason);
  else {
    t->index++;
    int mode = la_on_window(t->index);
    t->mode = mode;
    t->epoch++;
  }
}
static inline void la_frame(struct la_trial* t, uint64_t now, uint64_t acquire, uint64_t present,
                            uint64_t draws, int stale) {
  if (!t->armed || t->reason)
    return;
  if (now < t->arm || now - t->arm >= 60 * LA_SECOND) {
    la_lock(t, 0, "budget");
    return;
  }
  if (stale) {
    t->start = 0;
    t->n = 0;
    return;
  }
  if (!t->start) {
    t->start = t->prev = now;
    t->draws = draws;
    t->initial = t->counts;
    t->n = 0;
    return;
  }
  if (now <= t->prev || t->n >= LA_MAX_FRAMES) {
    la_lock(t, 0, "coverage");
    return;
  }
  t->frames[t->n++] = (struct la_frame){now - t->prev, acquire, present};
  t->prev = now;
  if (now - t->start < 5 * LA_SECOND)
    return;
  if (t->n < LA_MIN_FRAMES)
    t->rejected++;
  else
    la_finish_window(t, now, draws);
  t->start = 0;
  t->n = 0;
}
