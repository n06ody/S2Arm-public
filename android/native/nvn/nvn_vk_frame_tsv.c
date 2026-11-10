// 见 nvn_vk_frame_tsv.h：iOS frames.tsv 同名同义的直连逐帧记录。快照口径同 iOS
//   ft_present：present 成功点取各源「累计量」，本行写与上一行的差分；首行 wall_us=0（iOS 首行同）。
//   各源都在弱符号后面：没链进来的源对应列整列不进表头（mask 在首次打开时定一次）。
#include "nvn_vk_frame_tsv.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#define TSV_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "S2Arm", __VA_ARGS__)
#define TSV_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "S2Arm", __VA_ARGS__)
#else
#define TSV_LOGI(...) fprintf(stderr, "s2a: " __VA_ARGS__), fputc('\n', stderr)
#define TSV_LOGW(...) fprintf(stderr, "s2a: " __VA_ARGS__), fputc('\n', stderr)
#endif

// ---------------- 数据源（生产弱符号；测试替身走同一缝） ----------------
// 快照：各源的累计量；mask 标哪个源存在——不存在的源整列不出表头。
enum {
    S2A_TSV_M_DRAWPASS = 1u << 0,   // draw_n / pass_n（first_draw 累计）
    S2A_TSV_M_PROG     = 1u << 1,   // xlate_n / pso_n / pso_us（编译与管线累计）
    S2A_TSV_M_OFFLINE  = 1u << 2,   // shader_miss（离线库未命中累计）
    S2A_TSV_M_RB       = 1u << 3,   // rb_us（读回 fence 等待累计）
    S2A_TSV_M_GPUQ     = 1u << 4,   // gpuq_us（提交路径 fence 等待，本模块自计）
};
struct s2a_tsv_snap {
    uint32_t mask;
    uint64_t draws, passes;
    uint64_t compiles, pipes, pipe_ns;
    uint64_t offline_miss;
    uint64_t rb_ns;
    uint64_t gpuq_ns;
};

static _Atomic uint64_t g_gpuq_ns;

#ifdef S2A_TSV_TEST
void s2a_tsv_test_probe(struct s2a_tsv_snap *s);
static void tsv_probe(struct s2a_tsv_snap *s) {
    memset(s, 0, sizeof *s);
    s2a_tsv_test_probe(s);
    s->mask |= S2A_TSV_M_GPUQ;
    s->gpuq_ns = atomic_load(&g_gpuq_ns);
}
#else
static void tsv_probe(struct s2a_tsv_snap *s) {
    memset(s, 0, sizeof *s);
    extern void nvn_vk_first_draw_draw_pass_totals(uint64_t *, uint64_t *) __attribute__((weak));
    if (nvn_vk_first_draw_draw_pass_totals) { nvn_vk_first_draw_draw_pass_totals(&s->draws, &s->passes); s->mask |= S2A_TSV_M_DRAWPASS; }
    extern void nvk_program_long_totals(uint64_t *, uint64_t *, uint64_t *, uint64_t *) __attribute__((weak));
    if (nvk_program_long_totals) { uint64_t cns = 0; nvk_program_long_totals(&s->compiles, &cns, &s->pipes, &s->pipe_ns); s->mask |= S2A_TSV_M_PROG; }
    extern void nvk_program_offline_totals(uint64_t *, uint64_t *, uint64_t *) __attribute__((weak));
    if (nvk_program_offline_totals) { uint64_t h = 0, r = 0; nvk_program_offline_totals(&h, &s->offline_miss, &r); s->mask |= S2A_TSV_M_OFFLINE; }
    extern uint64_t nvn_vk_readback_wait_ns_total(void) __attribute__((weak));
    if (nvn_vk_readback_wait_ns_total) { s->rb_ns = nvn_vk_readback_wait_ns_total(); s->mask |= S2A_TSV_M_RB; }
    s->mask |= S2A_TSV_M_GPUQ;
    s->gpuq_ns = atomic_load(&g_gpuq_ns);
}
#endif

// ---------------- 开关与文件 ----------------
static int g_on = -1;
static int tsv_enabled(void) {
    if (g_on < 0) {
#if defined(__ANDROID__) && defined(S2A_ANDROID_DEBUG_ENV)
        // 两道门与 native/debug_gate.h 同义（S2A_ANDROID_DEBUG_ENV 编译旗标 +
        //   debug.s2a.debug_env 属性）——本文件是图形层，不能反向 include 平台私有头
        //   （R-04，豁免表只减不增），所以直接读同名属性、语义对齐不引边。
        char b[PROP_VALUE_MAX] = {0};
        g_on = __system_property_get("debug.s2a.debug_env", b) == 1 && b[0] == '1'
               && __system_property_get("debug.s2a.frame_tsv", b) == 1 && b[0] == '1';
#elif defined(__ANDROID__)
        g_on = 0;   // 正式构建没有 S2A_ANDROID_DEBUG_ENV：编译期门关死，同 debug_gate.h
#else
        const char *e = getenv("debug.s2a.frame_tsv");
        g_on = (e && atoi(e)) ? 1 : 0;
#endif
    }
    return g_on;
}
int nvn_vk_frame_tsv_on(void) { return tsv_enabled(); }

static FILE *g_f;
static uint32_t g_cols;
static struct s2a_tsv_snap g_prev;
static double g_prev_s;
static int g_rows;

static int tsv_open(void) {
    const char *dir = getenv("S2A_FRAME_LOG_DIR");
    if (!dir || dir[0] != '/') {
        TSV_LOGW("frame-tsv: S2A_FRAME_LOG_DIR unset or not absolute; frame TSV disabled this session");
        return -1;
    }
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/frame-tsv-%ld.tsv", dir, (long)getpid());
    if (n <= 0 || (size_t)n >= sizeof path) return -1;
    FILE *f = fopen(path, "w");
    if (!f) { TSV_LOGW("frame-tsv: open %s failed errno=%d", path, errno); return -1; }
    // 表头只收本构建里源存在的列（iOS 同口径：拿不到整列不出现）。gpuq_us 源是本模块
    //   自己的 fence 账，恒定可写。
    struct s2a_tsv_snap s; tsv_probe(&s); g_cols = s.mask;
    size_t o = 0; char head[160];
    o += (size_t)snprintf(head + o, sizeof head - o, "frame\twall_us");
    if (s.mask & S2A_TSV_M_DRAWPASS) o += (size_t)snprintf(head + o, sizeof head - o, "\tdraw_n\tpass_n");
    if (s.mask & S2A_TSV_M_PROG)     o += (size_t)snprintf(head + o, sizeof head - o, "\txlate_n\tpso_n\tpso_us");
    if (s.mask & S2A_TSV_M_OFFLINE)  o += (size_t)snprintf(head + o, sizeof head - o, "\tshader_miss");
    if (s.mask & S2A_TSV_M_RB)       o += (size_t)snprintf(head + o, sizeof head - o, "\trb_us");
    o += (size_t)snprintf(head + o, sizeof head - o, "\tgpuq_us\n");
    (void)fwrite(head, 1, o, f);
    g_f = f; g_prev = s; g_prev_s = 0; g_rows = 0;
    TSV_LOGI("frame-tsv → %s（列 %s）", path, head);
    return 0;
}

void nvn_vk_frame_tsv_present(uint64_t frame, double now_s) {
    if (!tsv_enabled()) return;
    if (!g_f && tsv_open()) return;
    struct s2a_tsv_snap s; tsv_probe(&s);
    char row[200]; size_t o = 0;
    o += (size_t)snprintf(row + o, sizeof row - o, "%llu\t%.0f",
                          (unsigned long long)frame,
                          g_rows ? (now_s - g_prev_s) * 1e6 : 0.0);
    if (g_cols & S2A_TSV_M_DRAWPASS)
        o += (size_t)snprintf(row + o, sizeof row - o, "\t%llu\t%llu",
                              (unsigned long long)(s.draws - g_prev.draws),
                              (unsigned long long)(s.passes - g_prev.passes));
    if (g_cols & S2A_TSV_M_PROG)
        o += (size_t)snprintf(row + o, sizeof row - o, "\t%llu\t%llu\t%llu",
                              (unsigned long long)(s.compiles - g_prev.compiles),
                              (unsigned long long)(s.pipes - g_prev.pipes),
                              (unsigned long long)((s.pipe_ns - g_prev.pipe_ns) / 1000));
    if (g_cols & S2A_TSV_M_OFFLINE)
        o += (size_t)snprintf(row + o, sizeof row - o, "\t%llu",
                              (unsigned long long)(s.offline_miss - g_prev.offline_miss));
    if (g_cols & S2A_TSV_M_RB)
        o += (size_t)snprintf(row + o, sizeof row - o, "\t%llu",
                              (unsigned long long)((s.rb_ns - g_prev.rb_ns) / 1000));
    o += (size_t)snprintf(row + o, sizeof row - o, "\t%llu\n",
                          (unsigned long long)((s.gpuq_ns - g_prev.gpuq_ns) / 1000));
    (void)fwrite(row, 1, o, g_f);
    fflush(g_f);   // 同 iOS ft_row：每行落盘，崩溃不丢尾帧
    g_prev = s; g_prev_s = now_s; g_rows++;
}

void nvn_vk_frame_tsv_fence_wait(uint64_t wait_ns) {
    atomic_fetch_add_explicit(&g_gpuq_ns, wait_ns, memory_order_relaxed);
}
