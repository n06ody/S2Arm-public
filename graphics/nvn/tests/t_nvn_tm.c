// 分时抽样（#1103）的宿主单测：回放热路径里的逐步计时（rp_draw 的各步、rp_pass_begin 的四段、rp_tex_resolve 的等锁 / 持锁）
// 改为每 N 帧只完整计 1 帧（nvk_tm_stride，缺省 32；nvk_tm_full=1 = 每帧都计 = 原行为），「分时」行里这几项按计时过的帧数求每帧值。
// 与 t_nvn_replay.c 共用同一份替身（重命名它的 main 后 include）。
// 覆盖：
//   ① 开关：缺省每 32 帧计 1 帧；nvk_tm_stride 可改并夹到 1..1024；nvk_tm_full=1 压过它；读一次后不再变，会话重开（rp_draw_reset）才重读。
//   ② 帧边界：rp_tm_frame_end 每调一次算一帧；采样帧 = 第 N、2N、3N… 帧（从 0 起数），RP_PERF_TM_FRAMES 数得对。
//   ③（Linux，假时钟）非采样帧里绘制 / 开通道 / 贴图解析一次时钟都不取、各步读数为 0；采样帧里照旧取、读数大于 0。
//   ④（Linux，假时钟）同一批帧：抽样（每 4 帧计 1 帧）与全量（每帧都计）打出的三行读数（绘制各步 / 开通道与取管线细分 / 贴图解析）逐字相同，
//      「计时抽样」说明行写着间隔与本窗口计了几帧。
// 期望值都由用例自己写下的输入直接得出，不经被测实现计算。
// 负对照：环境变量 T_NVN_TM_FULL=1 把「抽样」的几处强制成 nvk_tm_full=1（等于改动前每帧都计）→ ② ③ ④ 必须变红。
// 非 Linux 平台没有假时钟：只核对开关与帧边界（③ ④ 跳过，结尾那行会写明）。
#define main replay_fixture_main
#define nvn_plat_flag fixture_plat_flag
#define nvn_plat_log fixture_plat_log
#define plat_now_ns fixture_plat_now_ns
#define rp_shader_resolve fixture_shader_resolve
#include "t_nvn_replay.c"
#undef rp_shader_resolve
#undef plat_now_ns
#undef nvn_plat_log
#undef nvn_plat_flag
#undef main
#include <errno.h>
#include <limits.h>
#include <time.h>
#if defined(__linux__)
#include <unistd.h>
#include <sys/syscall.h>
#endif

// ---------- 开关与日志替身 ----------
static int g_tm_stride_flag = INT_MIN;   // INT_MIN = 没设（取产品缺省 32）
static int g_tm_full_flag;
static int g_hitch_flag, g_hitch_threshold_reads;
static uint64_t g_present_mono_ns = 5000000000ull;
uint64_t plat_now_ns(void) { return g_present_mono_ns; }
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_tm_stride") && g_tm_stride_flag != INT_MIN) return g_tm_stride_flag;
    if (!strcmp(name, "nvk_tm_full")) return g_tm_full_flag;
    if (!strcmp(name, "nvk_hitch_log")) return g_hitch_flag;
    if (!strcmp(name, "hitch_ms")) { g_hitch_threshold_reads++; return 25; }
    return fixture_plat_flag(name, dflt);
}
// 抓「分时」里要比对的几行（INFO 级；每个前缀留最后一条）
enum { L_DRAW, L_PASS, L_RES, L_NOTE, L_N };
static const char *const k_line_prefix[L_N] = {"分时（绘制各步，每帧毫秒）：", "分时（开通道与取管线细分，每帧毫秒）：", "分时（贴图解析，每帧）：", "分时（计时抽样）："};
static char g_line[L_N][1400];
static unsigned g_hitch_rows, g_hitch_details;
static char g_hitch_text[1280], g_hitch_detail[1280];
static unsigned g_hitch_program_rows;
static char g_hitch_program[1280];
static int g_delay_resolve;
int rp_shader_resolve(const struct rp_shader_req *q, const struct rp_prog **out) {
    if (g_delay_resolve) {
        struct timespec delay = {.tv_nsec = 35000000};
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
    }
    return fixture_shader_resolve(q, out);
}
void nvn_plat_log(int level, const char *cls, const char *text) {
    if (level == NVN_PLAT_LOG_INFO && !strncmp(text, "S2A_HITCH_PROGRAM ", sizeof "S2A_HITCH_PROGRAM " - 1)) {
        g_hitch_program_rows++;
        snprintf(g_hitch_program, sizeof g_hitch_program, "%s", text);
    }
    if (level == NVN_PLAT_LOG_INFO && !strcmp(cls, "present")) {
        if (!strncmp(text, "长帧 ", strlen("长帧 "))) {
            g_hitch_rows++;
            snprintf(g_hitch_text, sizeof g_hitch_text, "%s", text);
        } else if (!strncmp(text, "S2A_HITCH_REPLAY ", strlen("S2A_HITCH_REPLAY "))) {
            g_hitch_details++;
            snprintf(g_hitch_detail, sizeof g_hitch_detail, "%s", text);
        }
    }
    if (level == NVN_PLAT_LOG_INFO) for (int i = 0; i < L_N; i++) if (!strncmp(text, k_line_prefix[i], strlen(k_line_prefix[i]))) { snprintf(g_line[i], sizeof g_line[i], "%s", text); break; }
    fixture_plat_log(level, cls, text);
}
static void set_tm(int stride, int full) { g_tm_stride_flag = stride; g_tm_full_flag = full; rp_draw_reset(); }   // 会话重开才重读开关
// 负对照：T_NVN_TM_FULL=1 时「抽样」用例实际按每帧都计跑
static int neg_full(void) { const char *e = getenv("T_NVN_TM_FULL"); return e && *e && *e != '0'; }
static void set_sampled(int stride) { set_tm(stride, neg_full()); }

// ---------- 假时钟（仅 Linux）：开着时每读一次走 g_clk_step 纳秒，并数读了几次；关着时转给真时钟 ----------
#if defined(__linux__)
static int g_clk_fake; static uint64_t g_clk_ns = 1000000000000ull, g_clk_reads, g_clk_step = 50000;
int clock_gettime(clockid_t id, struct timespec *ts) {
    if (!g_clk_fake) return (int)syscall(SYS_clock_gettime, id, ts);
    g_clk_reads++; g_clk_ns += g_clk_step;
    ts->tv_sec = (time_t)(g_clk_ns / 1000000000ull); ts->tv_nsec = (long)(g_clk_ns % 1000000000ull);
    return 0;
}
static void clk_begin(void) { g_clk_reads = 0; g_clk_fake = 1; }
static uint64_t clk_reads(void) { return g_clk_reads; }
static void clk_end(void) { g_clk_fake = 0; }
#define HAVE_FAKE_CLOCK 1
#else
#define HAVE_FAKE_CLOCK 0
#endif

#if HAVE_FAKE_CLOCK
// 最小程序：没有绑定项（同 t_nvn_res.c 的 res_prog_min）
static void tm_prog_min(void) {
    memset(&g_fprog, 0, sizeof g_fprog);
    for (int st = 0; st < RP_PROG_STAGES; st++) for (int i = 0; i < RP_PROG_ENT_MAX; i++) g_fprog.ent[st][i].src_reg2 = UINT16_MAX;
    g_fprog.sh[0] = &g_fsh[0]; g_fprog.sh[1] = &g_fsh[1]; g_fprog.support_bytes = 2608; g_fprog.color_out_mask = 0xffffffffu;
}
#endif

// ---------- ① 开关 ----------
static void t_tm_flag(void) {
    set_tm(INT_MIN, 0); CHECK(rp_tm_stride() == 32 && rp_tm() == 0);   // 缺省：每 32 帧计 1 帧，第一帧不计
    set_tm(1, 0);       CHECK(rp_tm_stride() == 1 && rp_tm() == 1);    // 每帧都计：从头就开着
    set_tm(5, 0);       CHECK(rp_tm_stride() == 5 && rp_tm() == 0);
    set_tm(0, 0);       CHECK(rp_tm_stride() == 1);                    // < 1 夹到 1
    set_tm(-7, 0);      CHECK(rp_tm_stride() == 1);
    set_tm(5000, 0);    CHECK(rp_tm_stride() == 1024);                 // > 1024 夹到 1024
    set_tm(1024, 0);    CHECK(rp_tm_stride() == 1024);
    set_tm(5, 1);       CHECK(rp_tm_stride() == 1 && rp_tm() == 1);    // nvk_tm_full=1 压过 nvk_tm_stride
    set_tm(7, 0);
    CHECK(rp_tm_stride() == 7);
    g_tm_stride_flag = 3;                                              // 开关值变了，但没重开会话：读过一次就不再变
    CHECK(rp_tm_stride() == 7);
    rp_draw_reset();                                                   // 会话重开：重读
    CHECK(rp_tm_stride() == 3);
    set_tm(INT_MIN, 0);
}

// ---------- ② 帧边界与采样帧 ----------
static void t_tm_frames(void) {
    uint64_t pf[RP_PERF_N];
    set_sampled(4); rp_perf_take(pf);
    int timed[20], nt = 0;
    for (int f = 0; f <= 16; f++) { if (rp_tm() && nt < 20) timed[nt++] = f; rp_tm_frame_end(); }
    CHECK(nt == 4 && timed[0] == 4 && timed[1] == 8 && timed[2] == 12 && timed[3] == 16);   // 采样帧：第 4、8、12、16 帧（从 0 起数），第 0 帧不计
    rp_perf_take(pf); CHECK(pf[RP_PERF_TM_FRAMES] == 4);                                    // 每个采样帧结束时记一帧
    set_tm(1, 0); rp_perf_take(pf);
    int all = 1; for (int f = 0; f < 10; f++) { all &= rp_tm() != 0; rp_tm_frame_end(); }
    CHECK(all);                                                                             // 每帧都计：每帧都是采样帧
    rp_perf_take(pf); CHECK(pf[RP_PERF_TM_FRAMES] == 10);
    set_tm(INT_MIN, 0);
}

#if HAVE_FAKE_CLOCK
// 一帧：开一次提交，回放「设目标 + 20 笔绘制」，再解析 100 次同一张 CPU 贴图（纪元快路），收尾；返回这一帧里取时钟的次数。
// 20 笔绘制各自什么状态都不变（整组沿用），每帧的取时钟模式一样，按帧比读数才有意义。
static tex_t g_tm_rt, g_tm_rt2, g_tm_cpu;
static stream g_tm_s;
enum { TM_STRESS_STREAMS = 4, TM_STRESS_PASSES = 6 };   // 压力帧：4 条流（单条流缓冲只有 4096 字节）× 每条 6 个通道（交替两个目标）、每笔绘制换一次图元（= 换管线）
static stream g_tm_s2[TM_STRESS_STREAMS];
static int g_tm_bad;   // 解析失败的次数（采样只管计时，不该影响解析结果）
static uint64_t tm_frame_streams(const stream *const *ss, int n) {
    const uint64_t r0 = clk_reads();
    nvn_be_sub_begin();
    for (int k = 0; k < n; k++) nvn_be_exec(ss[k]->b);
    for (int i = 0; i < 100; i++) g_tm_bad += rp_tex_resolve(&g_tm_cpu, &g_fake_cmd) == NULL;
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    return clk_reads() - r0;
}
static uint64_t tm_frame(void) { const stream *const ss[1] = {&g_tm_s}; return tm_frame_streams(ss, 1); }
static void tm_scene_open(void) {
    pass_fresh();
    g_resolve_rc = g_cresolve_rc = 0; g_gdraws = 0;
    tm_prog_min();
    memset(g_pmem, 0, sizeof g_pmem);
    g_tm_rt = mk_tex(950, 0, NVN_FORMAT_RGBA8, 64, 36, 1, 0);
    g_tm_cpu = mk_tex(951, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0); g_tm_cpu.mem = g_pmem;
    memset(&g_tm_s, 0, sizeof g_tm_s);
    put_set_rt(&g_tm_s, &g_tm_rt, NULL);
    for (int i = 0; i < 20; i++) { p_draw *d = put(&g_tm_s, OP_DRAW, sizeof *d); d->prim = 4; d->count = 3; d->instances = 1; }
    end(&g_tm_s);
    g_tm_rt2 = mk_tex(952, 0, NVN_FORMAT_RGBA8, 64, 36, 1, 0);
    memset(g_tm_s2, 0, sizeof g_tm_s2);
    for (int k = 0; k < TM_STRESS_STREAMS; k++) {
        for (int i = 0; i < TM_STRESS_PASSES; i++) {
            put_set_rt(&g_tm_s2[k], i & 1 ? &g_tm_rt2 : &g_tm_rt, NULL);
            p_draw *d = put(&g_tm_s2[k], OP_DRAW, sizeof *d); d->prim = i & 1 ? 5 : 4; d->count = 3; d->instances = 1;
        }
        end(&g_tm_s2[k]);
    }
}
static void tm_scene_close(void) { nvn_be_tex_release(&g_tm_rt); nvn_be_tex_release(&g_tm_rt2); nvn_be_tex_release(&g_tm_cpu); pass_fresh(); }

// ---------- ③ 非采样帧不取时钟 ----------
static void t_tm_gating(void) {
    uint64_t pf[RP_PERF_N];
    tm_scene_open();
    set_sampled(2);                                    // 每 2 帧计 1 帧：第 0、1 帧不计，第 2 帧计
    clk_begin();
    tm_frame(); rp_tm_frame_end();                     // 第 0 帧（首次：新建贴图、开通道，不进比较）
    rp_perf_take(pf);
    const uint64_t quiet = tm_frame();                 // 第 1 帧：非采样帧
    rp_perf_take(pf);
    uint64_t quiet_sum = 0;
    const int steps[] = {RP_PERF_D_PROG, RP_PERF_D_PREP, RP_PERF_D_TEX, RP_PERF_D_PIPE, RP_PERF_D_DYN, RP_PERF_D_BIND, RP_PERF_D_DRAW, RP_PERF_D_TAIL, RP_PERF_D_PASS, RP_PERF_D_PGET,
                         RP_PERF_PB_VIEW, RP_PERF_PB_BAR, RP_PERF_PB_BE, RP_PERF_PB_MARK, RP_PERF_RES_LOCK, RP_PERF_RES_INIT};
    for (size_t i = 0; i < sizeof steps / sizeof *steps; i++) quiet_sum += pf[steps[i]];
    CHECK(quiet_sum == 0);                             // 非采样帧：各步 / 开通道 / 贴图解析的读数一点都没记
    rp_tm_frame_end();                                 // 第 1 帧结束 → 下一帧是采样帧
    CHECK(rp_tm() == 1);
    const uint64_t loud = tm_frame();                  // 第 2 帧：采样帧
    rp_perf_take(pf);
    int each_pos = 1; for (size_t i = 0; i < sizeof steps / sizeof *steps; i++) if (steps[i] != RP_PERF_D_PGET) each_pos &= pf[steps[i]] > 0;   // 取管线只在换管线时发生，整组沿用的帧里没有
    CHECK(each_pos);                                   // 采样帧：各步读数都有
    // 取时钟次数：非采样帧只剩提交 / 回放入口出口那几次（与是否抽样无关）；
    // 采样帧至少多出 20 笔绘制 × 每笔 10 次（入口 + 9 步）+ 100 次贴图解析 × 3 次（还有开通道、取管线那几次，这里不卡死个数）
    CHECK(quiet <= 16);
    CHECK(loud - quiet >= 20 * 10 + 100 * 3);
    CHECK(g_tm_bad == 0);
    // 压力帧：4 次 exec × 6 个通道（交替两个目标）、每笔绘制换一次图元 = 24 个通道 + 24 次换管线。
    // 非采样帧取时钟的次数不能随通道数 / 换管线次数涨：开通道的计时、取管线前后的计时只要有一处漏了门控，这里就会多出 24 次或更多。
    // 对照组：同样 4 次 exec、但每次只是一个通道里 20 笔不换管线的绘制（取时钟的底数随 exec 次数走，与通道数 / 换管线次数无关）。
    const stream *const ref4[TM_STRESS_STREAMS] = {&g_tm_s, &g_tm_s, &g_tm_s, &g_tm_s};
    const stream *const str4[TM_STRESS_STREAMS] = {&g_tm_s2[0], &g_tm_s2[1], &g_tm_s2[2], &g_tm_s2[3]};
    rp_tm_frame_end();                                 // 第 2 帧结束 → 第 3 帧是非采样帧
    CHECK(rp_tm() == 0);
    const uint64_t quiet_ref = tm_frame_streams(ref4, TM_STRESS_STREAMS);   // 第 3 帧（非采样）：对照组
    rp_perf_take(pf);
    rp_tm_frame_end();                                 // → 第 4 帧是采样帧：压力帧第一次跑（建通道 / 管线），同时量采样帧里这些步骤取了多少次
    CHECK(rp_tm() == 1);
    const uint64_t loud_s = tm_frame_streams(str4, TM_STRESS_STREAMS);
    rp_perf_take(pf);
    rp_tm_frame_end();                                 // → 第 5 帧：非采样帧，压力帧
    CHECK(rp_tm() == 0);
    const uint64_t quiet_s = tm_frame_streams(str4, TM_STRESS_STREAMS);
    rp_perf_take(pf);
    quiet_sum = 0; for (size_t i = 0; i < sizeof steps / sizeof *steps; i++) quiet_sum += pf[steps[i]];
    CHECK(quiet_sum == 0);
    CHECK(g_gdraws >= 3 * 20 + 20 * TM_STRESS_STREAMS + 2 * TM_STRESS_STREAMS * TM_STRESS_PASSES);   // 压力帧的绘制真的画了（不是被当成坏记录丢掉后「安静」）
    // 多 20 个通道、24 次换管线：非采样帧里只允许多出「收通道前后各取一次时钟」（rp_pass_set_rt 的 SETRT_END_NS，没纳入抽样：世界稳态每帧约 240 次，占取时钟总数 1%）：每次切目标 2 次。
    CHECK(quiet_s <= quiet_ref + 2 * (TM_STRESS_STREAMS * TM_STRESS_PASSES) + 2);
    CHECK(loud_s >= quiet_s + 24 * 4);                 // 采样帧里这些步骤照旧计时（每个通道至少 4 次、每次换管线 2 次，这里只卡下限）
    if (getenv("T_NVN_TM_VERBOSE")) printf("  [数据] 非采样帧取时钟 %llu 次，采样帧 %llu 次；4 次 exec 对照组非采样 %llu 次；压力帧：非采样 %llu 次，采样 %llu 次；g_gdraws=%d\n",
                                           (unsigned long long)quiet, (unsigned long long)loud, (unsigned long long)quiet_ref, (unsigned long long)quiet_s, (unsigned long long)loud_s, (int)g_gdraws);
    clk_end();
    tm_scene_close();
    set_tm(INT_MIN, 0);
}

// ---------- ④ 抽样与全量打出的读数逐字相同 ----------
// 每帧：开通道 + 20 笔绘制 + 100 次贴图解析 + 上屏。热身一帧（新建贴图 / 首次开通道）后清读数，再跑 16 帧；
// 最后一帧上屏前把假时钟拨快 6 秒，触发 5 秒一次的「分时」打印。
static void tm_run_lines(int stride, int full, char out[L_N][1400]) {
    tm_scene_open();
    set_tm(stride, full);
    rp_present_set_surface(&g_fake_surf, 120);
    for (int i = 0; i < L_N; i++) g_line[i][0] = 0;
    clk_begin();
    uint64_t pf[RP_PERF_N];
    tm_frame(); nvn_be_present(&g_tm_rt, 0, NULL);     // 热身帧
    rp_perf_take(pf);
    for (int f = 1; f <= 16; f++) {
        tm_frame();
        if (f == 16) g_clk_ns += 6000000000ull;
        nvn_be_present(&g_tm_rt, 0, NULL);
    }
    clk_end();
    for (int i = 0; i < L_N; i++) memcpy(out[i], g_line[i], sizeof g_line[i]);
    rp_present_set_surface(NULL, 0);
    tm_scene_close();
    rp_present_reset();
    set_tm(INT_MIN, 0);
}
static void t_tm_lines(void) {
    static char full[L_N][1400], samp[L_N][1400];
    tm_run_lines(1, 1, full);                          // 全量：nvk_tm_full=1，每帧都计
    tm_run_lines(4, neg_full(), samp);                 // 抽样：每 4 帧计 1 帧（负对照时强制成全量）
    CHECK(g_tm_bad == 0);
    for (int i = 0; i < L_NOTE; i++) CHECK(full[i][0] && samp[i][0]);                 // 三行读数都打出来了
    for (int i = 0; i < L_NOTE; i++) CHECK(!strcmp(full[i], samp[i]));                // 抽样与全量逐字相同（按计时过的帧数求每帧值）
    CHECK(strstr(full[L_NOTE], "每 1 帧完整计 1 帧，本窗口计了 16 帧") != NULL);      // 说明行：全量 = 每帧都计、16 帧
    CHECK(strstr(samp[L_NOTE], "每 4 帧完整计 1 帧，本窗口计了 4 帧") != NULL);       // 抽样：第 4、8、12、16 帧
    if (getenv("T_NVN_TM_VERBOSE")) { printf("  [全量] %s\n  [抽样] %s\n  [说明] %s\n", full[L_DRAW], samp[L_DRAW], samp[L_NOTE]); }
}
#endif

static int t_tm_hitch(int enabled) {
    g_hitch_flag = enabled;
    pass_fresh();
    rp_present_reset();
    set_tm(32, 0);
    CHECK(rp_tm_stride() == (enabled ? 1u : 32u));
    rp_present_set_surface(&g_fake_surf, 120);
    tex_t rt = mk_tex(953, 0, NVN_FORMAT_RGBA8, 64, 36, 1, 0);
    CHECK(nvn_be_present(&rt, 0, NULL) == 1);
    g_present_mono_ns += 24000000;
    CHECK(nvn_be_present(&rt, 0, NULL) == 2);
    g_present_mono_ns += 25000000;
    CHECK(nvn_be_present(&rt, 0, NULL) == 3);
    CHECK(g_hitch_rows == 0 && g_hitch_details == 0);
    stream replay = {0};
    end(&replay);
    submit1(&replay);
    fake_complete(NVN_GPU_DONE_OK);
    g_present_mono_ns += 25000001;
    CHECK(nvn_be_present(&rt, 0, NULL) == 4);
    CHECK(g_hitch_rows == (unsigned)enabled && g_hitch_details == (unsigned)enabled);
    if (enabled) {
        CHECK(strstr(g_hitch_text, "提交 1 次") != NULL);
        CHECK(strstr(g_hitch_detail, "S2A_HITCH_REPLAY v=1 mono_ns=5074000001 ") == g_hitch_detail);
    }
    g_present_mono_ns += 25000000;
    CHECK(nvn_be_present(&rt, 0, NULL) == 5);
    CHECK(g_hitch_rows == (unsigned)enabled && g_hitch_details == (unsigned)enabled);
    CHECK(g_fl_presents == 5 && g_hitch_threshold_reads == enabled);
    if (g_hitch_details) puts(g_hitch_detail);
    printf("t_nvn_tm hitch=%d: %d checks, %d failures\n", enabled, g_checks, g_fail);
    return g_fail ? 1 : 0;
}

static int t_tm_hitch_program(int enabled) {
    g_hitch_flag = enabled;
    pass_fresh();
    set_tm(32, 0);
    stream s = {0};
    q_env(&s);
    memset(g_fprog.identity, 0x7a, sizeof g_fprog.identity);
    q_draw(&s);
    end(&s);

    nvn_be_set_frame(46);
    int draws = g_gdraws;
    submit1(&s);
    fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == draws + 1 && g_hitch_program_rows == 0);

    rp_draw_reset();                         // 下一笔必须重新进入程序解析
    nvn_be_set_frame(47);
    g_delay_resolve = 1;
    submit1(&s);
    fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == draws + 2);
    CHECK(g_hitch_program_rows == (unsigned)enabled);
    if (enabled) {
        CHECK(strstr(g_hitch_program, " frame=47 ") != NULL);
        CHECK(strstr(g_hitch_program, " program=7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a ") != NULL);
        const char *pre = strstr(g_hitch_program, " pre_ns=");
        const char *resolve = strstr(g_hitch_program, " resolve_ns=");
        const char *post = strstr(g_hitch_program, " post_ns=");
        CHECK(pre && resolve && post);
        CHECK(resolve && strtoull(resolve + sizeof " resolve_ns=" - 1, NULL, 10) >= 35000000);
        unsigned long long start = 0, end = 0;
        CHECK(sscanf(g_hitch_program, "S2A_HITCH_PROGRAM v=1 mono_ns=%llu start_ns=%llu", &end, &start) == 2);
        CHECK(pre && resolve && post && start > 0 && end > start &&
              end - start == strtoull(pre + sizeof " pre_ns=" - 1, NULL, 10) +
                             strtoull(resolve + sizeof " resolve_ns=" - 1, NULL, 10) +
                             strtoull(post + sizeof " post_ns=" - 1, NULL, 10));
    }
    if (g_hitch_program_rows) puts(g_hitch_program);

    rp_draw_reset();
    nvn_be_set_frame(48);
    g_resolve_rc = 1;
    submit1(&s);
    fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_gdraws == draws + 2 && g_hitch_program_rows == 2u * (unsigned)enabled);
    if (enabled) {
        CHECK(strstr(g_hitch_program, " frame=48 ") != NULL);
        CHECK(strstr(g_hitch_program, " called=1 rc=1 program=- ") != NULL);
        CHECK(strstr(g_hitch_program, " post_ns=0 ") != NULL);
    }
    printf("t_nvn_tm hitch-program=%d: %d checks, %d failures\n", enabled, g_checks, g_fail);
    return g_fail ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--hitch-program-on")) return t_tm_hitch_program(1);
    if (argc == 2 && !strcmp(argv[1], "--hitch-program-off")) return t_tm_hitch_program(0);
    if (argc == 2 && !strcmp(argv[1], "--hitch-on")) return t_tm_hitch(1);
    if (argc == 2 && !strcmp(argv[1], "--hitch-off")) return t_tm_hitch(0);
    t_tm_flag();
    t_tm_frames();
#if HAVE_FAKE_CLOCK
    t_tm_gating();
    t_tm_lines();
#endif
    printf("t_nvn_tm：%d 项检查，%d 项红%s\n", g_checks, g_fail, HAVE_FAKE_CLOCK ? "" : "（本平台没有假时钟：取时钟次数与读数两类断言跳过）");
    return g_fail ? 1 : 0;
}
