// 所有试测状态与活动段计数归 g_mu。平台状态在拿 g_mu 前读取。
#pragma weak dm_thermal_now
#pragma weak s2a_android_host_lifecycle
extern int dm_thermal_now(void);
extern int s2a_android_host_lifecycle(uint64_t*);
extern uint64_t nvk_shader_live_compile_total(void) __attribute__((weak));

static void land_auto_log_locked(int exiting) {
  struct la_trial* t = &g.land_trial;
  int phase = exiting ? 2 : 1;
  if (!t->reason || g.land_reported >= phase)
    return;
  if (!g.land_reported) {
#ifdef CLOCK_BOOTTIME
    struct timespec stamp;
    if (clock_gettime(CLOCK_BOOTTIME, &stamp) == 0)
      t->decision_boottime_ns = (uint64_t)stamp.tv_sec * LA_SECOND + stamp.tv_nsec;
#endif
  }
  g.land_reported = phase;
  char msg[3900];
  int pos = snprintf(
      msg, sizeof msg,
      "回写自动决策 phase=%s mode=%d reason=%s decision_boottime_ns=%llu qualified_windows=%d invalid=%d AA_pairs=%d gain_gate=%.3f "
      "window=frames/fps/p99ms/tailfps/tailp99ms/acqMean,p95,p99ms/presentMean,p95,p99ms/"
      "eligibleB,spans,subs/staged,bytes/direct/allocs/drawsPerFrame/long33,long100,tail33,tail100",
      exiting ? "exit" : "lock", t->mode, t->reason, (unsigned long long)t->decision_boottime_ns, t->qualified, t->rejected, t->windows[LA_AA - 1].frames ? LA_AA / 2 : 0,
      fmax(.05, t->noise[LA_FPS]));
  for (int i = 0; i < LA_WINDOWS; i++) {
    const struct la_window* w = &t->windows[i];
    const double* m = w->m;
    int n = snprintf(msg + pos, sizeof msg - (size_t)pos,
                     " w%dR%d=%u/%.2f/%.2f/%.2f/%.2f/%.2f,%.2f,%.2f/%.2f,%.2f,%.2f/%llu,%llu,%llu/"
                     "%llu,%llu/%llu/%llu/%.1f/%.3f,%.3f,%.3f,%.3f",
                     i, la_on_window(i), w->frames, m[LA_FPS], m[LA_P99], m[LA_TAIL_FPS],
                     m[LA_TAIL_P99], m[LA_ACQ], m[LA_ACQ95], m[LA_ACQ99], m[LA_PRESENT],
                     m[LA_PRESENT95], m[LA_PRESENT99], (unsigned long long)w->work.bytes,
                     (unsigned long long)w->work.spans, (unsigned long long)w->work.subs,
                     (unsigned long long)w->work.staged, (unsigned long long)w->work.staged_bytes,
                     (unsigned long long)w->work.direct, (unsigned long long)w->work.allocs,
                     w->draws, m[LA_LONG33], m[LA_LONG100], m[LA_TAIL33], m[LA_TAIL100]);
    if (n < 0 || (size_t)n >= sizeof msg - (size_t)pos)
      break;
    pos += n;
  }
  nvn_plat_log(NVN_PLAT_LOG_INFO, "land-readback", msg);
}
static void land_auto_exit_locked(void) {
  if (g.land_auto) {
    la_end(&g.land_trial);
    land_auto_log_locked(1);
  }
}
static void land_auto_check_gap_locked(uint64_t now) {
  struct la_trial* t = &g.land_trial;
  if (!t->reason && t->mode && t->prev && now > t->prev && now - t->prev >= 5 * LA_SECOND) {
    la_discard(t);
    t->prev = 0;
    g.land_prior_live = g.land_live_count;
  }
}
void nvn_gpu_land_auto_present(uint64_t now, uint64_t acquire, uint64_t present, uint64_t draws,
                               int target) {
  if (!g.land_auto || atomic_load_explicit(&g.land_reported, memory_order_relaxed))
    return;
  uint64_t life = 0;
  int foreground = s2a_android_host_lifecycle ? s2a_android_host_lifecycle(&life) : -1;
  int thermal = dm_thermal_now ? dm_thermal_now() : -1;
  uint64_t compile = nvk_shader_live_compile_total ? nvk_shader_live_compile_total() : 0;
  pthread_mutex_lock(&g_mu);
  struct la_trial* t = &g.land_trial;
  if (g.closing || t->reason) {
    pthread_mutex_unlock(&g_mu);
    return;
  }
  int previous_epoch = t->epoch;
  if (!g.land_readback) {
    la_lock(t, 0, LA_REASON_OFF_NO_CACHED_TYPE);
  } else if (foreground != 1 || thermal < 0 || !nvk_shader_live_compile_total ||
             (t->environment_set && (life != t->life || thermal != t->thermal ||
                                    compile != t->compile || target != t->target))) {
    // Drop observations across a lifecycle, frequency-class, compile or pacing
    // change. Previous paired windows cannot be compared across that boundary.
    if (t->start || t->qualified || t->recovering) {
      t->rejected++;
      t->start = t->prev = t->n = 0;
      t->qualified = t->index = t->recovering = t->mode = 0;
      memset(t->windows, 0, sizeof t->windows);
      memset(t->noise, 0, sizeof t->noise);
      t->epoch++;
    }
    t->environment_set = 0;
  } else {
    t->environment_set = 1;
    t->life = life;
    t->thermal = thermal;
    t->compile = compile;
    t->target = target;
    la_frame(t, now, acquire, present, draws, g.land_prior_live != 0);
  }
  if (t->epoch != previous_epoch)
    g.land_prior_live = g.land_live_count;
  land_auto_log_locked(0);
  pthread_mutex_unlock(&g_mu);
}
