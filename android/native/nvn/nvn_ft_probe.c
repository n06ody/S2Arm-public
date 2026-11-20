// 逐帧时间探针实现（字段口径 / 归属模型 / 完整性 / 转储格式见 nvn_ft_probe.h 头注释）。
// 写者串行化靠调用方的 g_submit_mu（提交、呈现）与同线程序（acquire→present 同一线程），
// 环内不取锁；转储在 nvk_session_close 的 nvn_gpu_close 之后，写者已停。
#include "nvn_ft_probe.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#include "debug_gate.h"
#endif

static struct s2a_ft_ring g_ft;
static int g_ft_on = -1;   // 开关缓存：属性一次性读入，之后每次事件只比标志（ns 级）

// 待决 acquire：归属帧未定（可能掉帧），present 闭合记录时结算进该帧。
// nvk 路径上 acquire 与 present 同线程（rp_present 的 present_impl 顺序调），平写即可。
static uint64_t g_acq_b, g_acq_e;
static int32_t g_acq_rc;

// dec_b 不走全局待决槽（qa-dena #477910：并存 cmd——主 g_cmd 期间 rp_xfer_cmd
// 另开 g_pre——会互相覆盖起点，首段消耗后其余段丢 0）：开段戳存进各自
// nvn_gpu_cmd_s.ft_dec_b，提交由调用方按本段 cmd 显式带来；非回放段传 0。

// 最近一条待补 seg_e 的 sub：commit_impl 在 vkQueueSubmit 点写记录、在 commit 出口补 seg_e。
static struct s2a_ft_sub *g_last_sub;

// presentID → 记录槽映射：present 闭合时按 timing_id 登记，display-timing 回报时回填
// actual_present。回报在 present 之后几帧内到达，映射深度 256 足够；
// 校验 id 与 ticket 双符防跨代串写（presentID 按交换链代重置）。
#define S2A_FT_PID_CAP 256u
static struct { uint32_t id, slot; uint64_t ticket; } g_pid[S2A_FT_PID_CAP];

int s2a_ft_on(void) {
    if (g_ft_on < 0) {
#if defined(__ANDROID__)
        char b[PROP_VALUE_MAX] = {0};
        g_ft_on = s2a_debug_entries_open()
                  && __system_property_get("debug.s2a.ft_probe", b) == 1 && b[0] == '1';
#else
        const char *e = getenv("debug.s2a.ft_probe");
        g_ft_on = (e && atoi(e)) ? 1 : 0;
#endif
    }
    return g_ft_on;
}

void s2a_ft_acquire(uint64_t b, uint64_t e, int32_t rc) {
    g_acq_b = b; g_acq_e = e; g_acq_rc = rc;
}

// 手动转储（laze #478473）：BACK 只停 Activity、nvk_session_close 不跑时转储没有出口——
// 每秒至多查一次 debug.s2a.ft_dump，读到 1 吐一次；属性回 0 才重新武装。轮询在提交/呈现
// 线程上做、不另起线程：环没有锁，跨线程边写边转储会撕裂记录。
static uint64_t g_dump_poll_ns;
static int g_dump_armed = 1;
static int ft_prop_one(const char *name) {
#if defined(__ANDROID__)
    char b[PROP_VALUE_MAX] = {0};
    return __system_property_get(name, b) == 1 && b[0] == '1';
#else
    const char *e = getenv(name);
    return e && e[0] == '1';
#endif
}
static void ft_dump_poll(uint64_t now) {
    if (g_dump_poll_ns && now - g_dump_poll_ns < 1000000000ull) return;
    g_dump_poll_ns = now;
    if (ft_prop_one("debug.s2a.ft_dump")) {
        if (g_dump_armed) { g_dump_armed = 0; s2a_ft_dump(); }
    } else g_dump_armed = 1;
}

void s2a_ft_submit(uint64_t seg_b, uint64_t sub_b, uint64_t sub_e, uint64_t key, uint64_t dec_b) {
    struct s2a_ft_rec *r = &g_ft.rec[g_ft.w];
    if (r->nsub == 0) r->rep_b = seg_b;
    if (dec_b && (!r->dec_b || dec_b < r->dec_b)) r->dec_b = dec_b;   // 帧级取窗口内最早开段点
    if (r->nkept < S2A_FT_SUB_MAX) {
        struct s2a_ft_sub *s = &r->sub[r->nkept++];
        s->dec_b = dec_b; s->seg_b = seg_b; s->sub_b = sub_b; s->sub_e = sub_e; s->seg_e = 0; s->key = key;
        g_last_sub = s;
    } else {
        g_last_sub = NULL;
        g_ft.sub_dropped++;
    }
    r->nsub++;
    r->rep_e = sub_e;   // seg_end 补写后换成真正的 commit 出口
}

void s2a_ft_seg_end(uint64_t seg_e) {
    if (!g_last_sub) return;
    g_last_sub->seg_e = seg_e;
    g_ft.rec[g_ft.w].rep_e = seg_e;
    g_last_sub = NULL;
}

void s2a_ft_present(uint64_t ticket, uint64_t pre_b, uint64_t pre_e, int32_t rc, uint32_t timing_id) {
    ft_dump_poll(pre_b);
    uint32_t closed = g_ft.w;
    struct s2a_ft_rec *r = &g_ft.rec[closed];
    r->ticket = ticket;
    r->acq_b = g_acq_b; r->acq_e = g_acq_e; r->acq_rc = g_acq_rc;
    g_acq_b = g_acq_e = 0; g_acq_rc = 0;
    r->pre_b = pre_b; r->pre_e = pre_e; r->pre_rc = rc;
    r->timing_id = timing_id;
    if (timing_id) {
        uint32_t i = timing_id % S2A_FT_PID_CAP;
        g_pid[i].id = timing_id; g_pid[i].slot = closed; g_pid[i].ticket = ticket;
    }
    g_last_sub = NULL;
    g_ft.w = (g_ft.w + 1) % S2A_FT_CAP;
    if (g_ft.seq < UINT64_MAX) g_ft.seq++;
    if (g_ft.kept + 1 < S2A_FT_CAP) {
        g_ft.kept++;
    } else {
        // 环上永远给开窗留一槽：kept 上限 CAP-1。满环时新窗落在最旧闭合记录上，
        // 先摘出（dropped 记账）再清零——否则该槽仍按「最旧一条」被转储，
        // first_ticket/dec_b 全 0（qa-dena #477910）。
        if (g_ft.dropped < UINT64_MAX) g_ft.dropped++;
    }
    memset(&g_ft.rec[g_ft.w], 0, offsetof(struct s2a_ft_rec, sub));   // 新开窗槽：头部清零
}

void s2a_ft_present_actual(uint32_t timing_id, uint64_t actual) {
    if (!timing_id) return;
    uint32_t i = timing_id % S2A_FT_PID_CAP;
    if (g_pid[i].id != timing_id) return;
    struct s2a_ft_rec *r = &g_ft.rec[g_pid[i].slot];
    if (r->ticket != g_pid[i].ticket) return;   // 槽已被环覆盖复用（跨代/长跑）
    r->actual_present = actual;
}

// ---------------- 转储（应用私有目录文件；logcat 只留一行 done/FAILED 摘要） ----------------
// 文件契约 v=3（probe6 #482300）：定长头 + kept 条定长记录（最旧→最新，LE）。
//   一次性往 logcat 灌几千行被系统丢片（meta 宣称 6496 片只到 153 片、无 end）——
//   传输改走 files/ft_dump-<last_ticket>.bin，写完 fsync 后 logcat 打一行摘要，
//   QA 用 run-as 只读拉。完整性仍不用摘要：头部带 kept/bytes/首末 ticket/dropped，
//   记录自带连续 ticket，核回时校验 总长=kept×rec、帧数=kept、ticket 单调。
//
// 文件头（全部 LE）：
struct s2a_ft_file_head {
    char     magic[8];          /* "S2AFT\0\0\0" */
    uint32_t version;           /* 3：文件形态（记录口径与 v2 相同） */
    uint32_t rec_size;          /* sizeof(struct s2a_ft_rec) */
    uint64_t kept, dropped, sub_dropped;
    uint64_t seq_base;          /* ft->seq − kept：首条的全局 present 序号（同旧 meta first=） */
    uint64_t first_ticket, last_ticket;
    uint64_t bytes;             /* 载荷字节 = kept × rec_size */
    char     clock[24];         /* "CLOCK_MONOTONIC" */
    char     unit[8];           /* "ns" */
    char     endian[8];         /* "le" */
    char     uuid[96];          /* debug.s2a.session_uuid 或 "-" */
};
_Static_assert(sizeof(struct s2a_ft_file_head) == 208, "ft file head layout");

static uint32_t ft_slot(const struct s2a_ft_ring *ft, uint64_t i) {
    return (uint32_t)((ft->w + S2A_FT_CAP - ft->kept + i) % S2A_FT_CAP);
}

// 写转储文件：头 + kept 条记录，fflush 后 fsync 再 fclose。成功返回写入总字节数，失败 −1、why 带原因。
long s2a_ft_dump_file(struct s2a_ft_ring *ft, const char *path, char *why, size_t why_size) {
    if (!ft || !path || !path[0]) { snprintf(why, why_size, "no path"); return -1; }
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(why, why_size, "open failed (errno=%d)", errno); return -1; }
    const uint64_t kept = ft->kept;
    const struct s2a_ft_rec *r0 = kept ? &ft->rec[ft_slot(ft, 0)] : NULL;
    const struct s2a_ft_rec *rN = kept ? &ft->rec[ft_slot(ft, kept - 1)] : NULL;
    struct s2a_ft_file_head h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "S2AFT\0\0", 8);
    h.version = 3;
    h.rec_size = (uint32_t)sizeof(struct s2a_ft_rec);
    h.kept = kept; h.dropped = ft->dropped; h.sub_dropped = ft->sub_dropped;
    h.seq_base = ft->seq - kept;
    h.first_ticket = r0 ? r0->ticket : 0;
    h.last_ticket = rN ? rN->ticket : 0;
    h.bytes = kept * sizeof(struct s2a_ft_rec);
    snprintf(h.clock, sizeof h.clock, "CLOCK_MONOTONIC");
    snprintf(h.unit, sizeof h.unit, "ns");
    snprintf(h.endian, sizeof h.endian, "le");
    snprintf(h.uuid, sizeof h.uuid, "-");
#if defined(__ANDROID__)
    char uuid[PROP_VALUE_MAX];
    if (__system_property_get("debug.s2a.session_uuid", uuid) > 0)
        snprintf(h.uuid, sizeof h.uuid, "%s", uuid);
#endif
    long total = (long)sizeof h;
    int ok = fwrite(&h, sizeof h, 1, f) == 1;
    for (uint64_t i = 0; ok && i < kept; i++) {
        ok = fwrite(&ft->rec[ft_slot(ft, i)], sizeof(struct s2a_ft_rec), 1, f) == 1;
        total += sizeof(struct s2a_ft_rec);
    }
    if (ok && fflush(f)) ok = 0;
    if (ok && fsync(fileno(f))) ok = 0;
    if (fclose(f) && ok) ok = 0;
    if (!ok) { snprintf(why, why_size, "write/fsync failed (errno=%d)", errno); return -1; }
    return total;
}

static const char *ft_files_dir(void) {
#if defined(__ANDROID__)
    // 应用私有 files 目录（NativeBridge.configureGpuDriver 存下的 getFilesDir()；
    //   同 sesmark/驱动目录同一条路，bridge.cpp 提供，弱引用防未配置构建）
    extern const char *s2a_android_gpu_driver_files_dir(void) __attribute__((weak));
    return s2a_android_gpu_driver_files_dir ? s2a_android_gpu_driver_files_dir() : NULL;
#else
    return getenv("S2A_FT_DUMP_DIR");   // 宿主用例指定的转储目录
#endif
}

static void ft_platform_emit(const char *line) {
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "S2Arm", "%s", line);
#else
    fprintf(stderr, "%s\n", line);
#endif
}

// 局末/手动吐出：nvk_session_close 在 nvn_vk_frame_log_dump 旁弱引用；此刻写者已停。
// 开关关着 / 一帧没记过时什么都不做。logcat 只留一行：done 路径/长度/记录数（失败带原因）。
void s2a_ft_dump(void) {
    if (!s2a_ft_on() || !g_ft.kept) return;
    const struct s2a_ft_rec *r0 = &g_ft.rec[ft_slot(&g_ft, 0)];
    const struct s2a_ft_rec *rN = &g_ft.rec[ft_slot(&g_ft, g_ft.kept - 1)];
    const char *dir = ft_files_dir();
    char path[512], why[160] = "", line[768];
    if (!dir || !dir[0] ||
        snprintf(path, sizeof path, "%s/ft_dump-%llu.bin", dir, (unsigned long long)rN->ticket) >= (int)sizeof path) {
        snprintf(line, sizeof line, "direct frame-trace file: FAILED no files dir (records=%llu kept, path=%s)",
                 (unsigned long long)g_ft.kept, dir ? dir : "(null)");
        ft_platform_emit(line);
        return;
    }
    long n = s2a_ft_dump_file(&g_ft, path, why, sizeof why);
    if (n < 0) {
        snprintf(line, sizeof line, "direct frame-trace file: FAILED %s: %s (records=%llu)",
                 path, why, (unsigned long long)g_ft.kept);
    } else {
        snprintf(line, sizeof line,
                 "direct frame-trace file: done path=%s bytes=%ld records=%llu dropped=%llu first_ticket=%llu last_ticket=%llu",
                 path, n, (unsigned long long)g_ft.kept, (unsigned long long)g_ft.dropped,
                 (unsigned long long)r0->ticket, (unsigned long long)rN->ticket);
    }
    ft_platform_emit(line);
}
