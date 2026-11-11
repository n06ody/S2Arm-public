// 直连逐帧帧时间（宿主，确定性）：环形语义、丢弃/计数可见、分片转储字节级核回。
// unity 引入实现文件：白盒测静态（g_fl / 开关缓存），另链接真实 sha256.c。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "nvn/nvn_vk_frame_log.c"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- emit 捕获：全部行进 blob（满环约 1000 行），另记首末行 ----
static char g_blob[2048 * 1024];   // v5: 32768×40B=1.31MB 载荷 → base64≈1.75MB + 分片前缀
static size_t g_bloblen;
static int g_nlines;
static char g_first[1024], g_last[1024];
static void cap_emit(void *ctx, const char *line) {
    (void)ctx;
    g_nlines++;
    if (g_nlines == 1) snprintf(g_first, sizeof g_first, "%s", line);
    snprintf(g_last, sizeof g_last, "%s", line);
    size_t l = strlen(line);
    if (g_bloblen + l + 1 < sizeof g_blob) {
        memcpy(g_blob + g_bloblen, line, l);
        g_bloblen += l;
        g_blob[g_bloblen++] = '\n';
    }
}

// ---- base64 解码（与实现端无关的独立解码器，对拍用） ----
static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
// 返回解码字节数；非法字符/长度返回 -1。out 至少 (len/4)*3 字节。
static long b64dec(const char *s, size_t len, uint8_t *out) {
    if (!len || len % 4) return -1;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 4) {
        int v0 = b64v(s[i]), v1 = b64v(s[i + 1]);
        int v2 = s[i + 2] == '=' ? -2 : b64v(s[i + 2]);
        int v3 = s[i + 3] == '=' ? -3 : b64v(s[i + 3]);
        if (v0 < 0 || v1 < 0 || v2 == -1 || v3 == -1) return -1;
        out[o++] = (uint8_t)((v0 << 2) | (v1 >> 4));
        if (v2 != -2) out[o++] = (uint8_t)((v1 << 4) | (v2 >> 2));
        if (v3 != -3) out[o++] = (uint8_t)((v2 << 6) | v3);
    }
    return (long)o;
}

static void fill(struct s2a_frame_log *fl, uint64_t n, uint64_t base_us, uint64_t step_us) {
    s2a_fl_reset(fl);
    for (uint64_t i = 0; i < n; i++) s2a_fl_note(fl, base_us + i * step_us);
}

// 期望载荷：n 条 16B LE 记录（最旧→最新），帧号从 first 起、步长 step；fus/fn 空 = 全 0。
static size_t expect_payload(uint8_t *out, uint64_t first, uint64_t n,
                             uint64_t base_us, uint64_t step_us,
                             const uint32_t *fus, const uint32_t *fn) {
    memset(out, 0, (size_t)n * S2A_FL_REC_BYTES);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t v = base_us + (first + i) * step_us;
        uint32_t u = fus ? fus[i] : 0, c = fn ? fn[i] : 0;
        for (int k = 0; k < 8; k++) out[i * S2A_FL_REC_BYTES + k] = (uint8_t)(v >> (k * 8));
        for (int k = 0; k < 4; k++) out[i * S2A_FL_REC_BYTES + 8 + k] = (uint8_t)(u >> (k * 8));
        for (int k = 0; k < 4; k++) out[i * S2A_FL_REC_BYTES + 12 + k] = (uint8_t)(c >> (k * 8));
        // bytes 16..23 = acquire/present（s2a_fl_note 写 0）；24..31 = gpu_us/gpu_seq、
        // 32..39 = disp_us/disp_seq；本测试二进制桩在关态返 0（视同缺席）→ UINT32_MAX。
        memset(out + i * S2A_FL_REC_BYTES + 24, 0xff, 16);
    }
    return (size_t)(n * S2A_FL_REC_BYTES);
}

static void hash_payload(const uint8_t *p, size_t n, char hex[65]) {
    uint8_t d[32];
    s2a_sha256 h;
    s2a_sha256_init(&h);
    s2a_sha256_update(&h, p, n);
    s2a_sha256_final(&h, d);
    s2a_sha256_hex(d, hex);
}

// emit 出的 meta + 分片 + end 全部核回：分片逐核序号与单行长度、拼回解码、
// 与独立算出的期望载荷逐字节比对，end 行 sha256 与独立哈希比对。
static void check_dump_roundtrip(struct s2a_frame_log *fl, uint64_t first, uint64_t n,
                                 uint64_t base_us, uint64_t step_us,
                                 const uint32_t *fus, const uint32_t *fn, const char *name) {
    printf("  case %-32s", name);
    uint8_t *want = malloc((size_t)(n ? n * S2A_FL_REC_BYTES : 1));
    uint8_t *got = malloc((size_t)(n ? n * S2A_FL_REC_BYTES : 1));
    size_t wn = expect_payload(want, first, n, base_us, step_us, fus, fn);
    char want_hex[65];
    hash_payload(want, wn, want_hex);
    g_nlines = 0;
    g_bloblen = 0;
    s2a_fl_dump(fl, cap_emit, NULL);
    CHECK(g_nlines >= 2);
    CHECK(strncmp(g_first, "direct frame-log meta: v=5 ", 27) == 0);
    CHECK(strstr(g_first, "rec=40") != NULL);
    {
        char needle[192];
        snprintf(needle, sizeof needle, "recorded=%llu presents=%llu dropped=%llu first=%llu bytes=%llu",
                 (unsigned long long)fl->kept, (unsigned long long)fl->seq,
                 (unsigned long long)fl->dropped, (unsigned long long)(fl->seq - fl->kept),
                 (unsigned long long)wn);
        CHECK(strstr(g_first, needle) != NULL);
        snprintf(needle, sizeof needle, "chunks=%u", (unsigned)g_nlines - 2);
        CHECK(strstr(g_first, needle) != NULL);
    }
    {
        // 分片行：逐一核序号前缀、按序拼回再解码；行数/长度都必须合上。
        size_t glen = 0, cursor = 0;
        int line_no = 0;
        unsigned parts = (unsigned)g_nlines - 2;
        char *all = malloc(wn * 4 / 3 + 16);   // base64 上限 ≈ 4/3 × 载荷
        CHECK(all != NULL);
        while (cursor < g_bloblen) {
            char *eol = memchr(g_blob + cursor, '\n', g_bloblen - cursor);
            size_t ll = eol ? (size_t)(eol - (g_blob + cursor)) : g_bloblen - cursor;
            const char *lp = g_blob + cursor;
            line_no++;
            if (line_no > 1 && line_no <= (int)parts + 1 && line_no < g_nlines) {
                char expect_head[64];
                snprintf(expect_head, sizeof expect_head, "direct frame-log[%d/%u]:", line_no - 1, parts);
                CHECK(ll > strlen(expect_head));
                CHECK(strncmp(lp, expect_head, strlen(expect_head)) == 0);
                const char *p = lp + strlen(expect_head);
                size_t pl = ll - strlen(expect_head);
                CHECK(pl > 0 && pl <= 700);
                if (glen + pl <= wn * 4 / 3 + 16) { memcpy(all + glen, p, pl); glen += pl; }
            }
            cursor += ll + 1;
        }
        long dn = glen ? b64dec(all, glen, got) : 0;
        CHECK(dn == (long)wn);
        CHECK(dn < 0 || wn == 0 || memcmp(got, want, wn) == 0);
        free(all);
    }
    {
        char tail[160];
        snprintf(tail, sizeof tail, "direct frame-log end: frames=%llu dropped=%llu sha256=%s",
                 (unsigned long long)fl->kept, (unsigned long long)fl->dropped, want_hex);
        CHECK(strcmp(g_last, tail) == 0);
    }
    free(want);
    free(got);
    printf("ok\n");
}

static void *fence_writer(void *arg) {
    struct s2a_frame_log *fl = arg;
    for (int i = 0; i < 10000; i++) s2a_fl_fence(fl, 1000);
    return NULL;
}
static void concurrent_snapshot_test(void) {
    static struct s2a_frame_log fl;
    s2a_fl_reset(&fl);
    pthread_t workers[4];
    for (int i = 0; i < 4; i++) CHECK(pthread_create(&workers[i], NULL, fence_writer, &fl) == 0);
    for (int i = 0; i < 2000; i++) s2a_fl_note_timed(&fl, (uint64_t)i + 1, 1500, 2700);
    for (int i = 0; i < 4; i++) CHECK(pthread_join(workers[i], NULL) == 0);
    s2a_fl_note(&fl, 2001);
    uint64_t total = 0;
    for (uint64_t i = 0; i < fl.kept; i++) {
        CHECK(s2a_fl_fence_us(&fl, i) == s2a_fl_fence_n(&fl, i));
        total += s2a_fl_fence_n(&fl, i);
        if (i < 2000) CHECK(fl.rec[i].acquire_us == 1 && fl.rec[i].present_us == 2);
        CHECK(fl.rec[i].gpu_us == UINT32_MAX && fl.rec[i].gpu_seq == UINT32_MAX);   // 无 gpu_last → 未采集
        CHECK(fl.rec[i].disp_us == UINT32_MAX && fl.rec[i].disp_seq == UINT32_MAX); // dt 桩关态 → 未采集
    }
    CHECK(total == 40000);
}

// gpu 字段用例：强符号桩覆盖 s2a_fl_note_timed 里的弱符号——剧本化（ms, seq），
// 核 gpu_us/gpu_seq 与「同一结算帧只报一次」去重。
static double g_stub_ms; static uint64_t g_stub_seq; static int g_stub_on;
int nvn_vk_first_draw_gpu_last(double *ms, int *passes, uint64_t *frame) {
    if (!g_stub_on) return 0;
    if (ms) *ms = g_stub_ms; if (passes) *passes = 0; if (frame) *frame = g_stub_seq;
    return 1;
}
static void gpu_field_test(void) {
    struct s2a_frame_log fl;
    s2a_fl_reset(&fl);
    g_stub_on = 1; g_stub_ms = 8.25; g_stub_seq = 7;
    s2a_fl_note(&fl, 1000);
    CHECK(fl.rec[0].gpu_us == 8250 && fl.rec[0].gpu_seq == 7);
    s2a_fl_note(&fl, 2000);   // 同一结算帧 → 去重写未采集
    CHECK(fl.rec[1].gpu_us == UINT32_MAX && fl.rec[1].gpu_seq == UINT32_MAX);
    g_stub_seq = 8; g_stub_ms = 1.5;
    s2a_fl_note(&fl, 3000);
    CHECK(fl.rec[2].gpu_us == 1500 && fl.rec[2].gpu_seq == 8);
    g_stub_on = 0;
    s2a_fl_note(&fl, 4000);   // 读数不可用 → 未采集，不写 0
    CHECK(fl.rec[3].gpu_us == UINT32_MAX && fl.rec[3].gpu_seq == UINT32_MAX);
    g_stub_on = 1; g_stub_seq = 9; g_stub_ms = 2.0;   // 新结算帧 → 再报
    s2a_fl_note(&fl, 5000);
    CHECK(fl.rec[4].gpu_us == 2000 && fl.rec[4].gpu_seq == 9);
    g_stub_on = 0;
}

// disp 字段用例：强符号桩覆盖弱符号 nvn_vk_dt_last_gap——剧本化（已确认对的
// 较新帧号, 间隔 ns），核 disp_us/disp_seq 与「同一对已确认对只报一次」去重。
static uint64_t g_dt_frame; static uint64_t g_dt_gap_ns; static int g_dt_on;
int nvn_vk_dt_last_gap(uint64_t *frame, uint64_t *gap_ns) {
    if (!g_dt_on) return 0;
    if (frame) *frame = g_dt_frame; if (gap_ns) *gap_ns = g_dt_gap_ns;
    return 1;
}
static void disp_field_test(void) {
    struct s2a_frame_log fl;
    s2a_fl_reset(&fl);
    g_dt_on = 1; g_dt_frame = 11; g_dt_gap_ns = 16666667;
    s2a_fl_note(&fl, 1000);
    CHECK(fl.rec[0].disp_us == 16667 && fl.rec[0].disp_seq == 11);
    s2a_fl_note(&fl, 2000);   // 同一对已确认对 → 去重写未采集
    CHECK(fl.rec[1].disp_us == UINT32_MAX && fl.rec[1].disp_seq == UINT32_MAX);
    g_dt_frame = 12; g_dt_gap_ns = 33333333;
    s2a_fl_note(&fl, 3000);
    CHECK(fl.rec[2].disp_us == 33333 && fl.rec[2].disp_seq == 12);
    g_dt_on = 0;              // 拿不到新确认 → 未采集，不写 0 不写 CPU 间隔
    s2a_fl_note(&fl, 4000);
    CHECK(fl.rec[3].disp_us == UINT32_MAX && fl.rec[3].disp_seq == UINT32_MAX);
    g_dt_on = 1; g_dt_frame = 13; g_dt_gap_ns = 500000;
    s2a_fl_note(&fl, 5000);   // 新确认对 → 再报
    CHECK(fl.rec[4].disp_us == 500 && fl.rec[4].disp_seq == 13);
    g_dt_on = 0;
}

int main(void) {
    concurrent_snapshot_test();
    gpu_field_test();
    disp_field_test();
    struct s2a_frame_log fl;

    printf("  case %-32s", "note_order_first");
    fill(&fl, 5, 1000000, 16000);
    CHECK(fl.seq == 5 && fl.kept == 5 && fl.dropped == 0 && s2a_fl_first(&fl) == 0);
    CHECK(s2a_fl_at(&fl, 0) == 1000000 && s2a_fl_at(&fl, 4) == 1000000 + 4 * 16000);
    CHECK(s2a_fl_at(&fl, 5) == 0);   // 越界
    printf("ok\n");

    printf("  case %-32s", "exactly_cap_no_drop");
    fill(&fl, S2A_FL_CAP, 5000, 1000);
    CHECK(fl.kept == S2A_FL_CAP && fl.dropped == 0 && s2a_fl_first(&fl) == 0);
    CHECK(s2a_fl_at(&fl, S2A_FL_CAP - 1) == 5000 + (S2A_FL_CAP - 1) * 1000);
    printf("ok\n");

    printf("  case %-32s", "ring_wrap_drop_oldest");
    fill(&fl, S2A_FL_CAP + 10, 777, 3);
    CHECK(fl.kept == S2A_FL_CAP && fl.dropped == 10 && fl.seq == (uint64_t)S2A_FL_CAP + 10);
    CHECK(s2a_fl_first(&fl) == 10);
    CHECK(s2a_fl_at(&fl, 0) == 777 + 10 * 3);                          // 现存最旧 = 第 10 帧
    CHECK(s2a_fl_at(&fl, S2A_FL_CAP - 1) == 777 + ((uint64_t)S2A_FL_CAP + 9) * 3); // 最新
    printf("ok\n");

    // ---- 分片转储（emit 捕获 → 拼回 → 独立解码/哈希）----
    s2a_fl_reset(&fl);
    check_dump_roundtrip(&fl, 0, 0, 0, 0, NULL, NULL, "dump_zero_present_meta");
    fill(&fl, 1, 999000, 0);
    check_dump_roundtrip(&fl, 0, 1, 999000, 0, NULL, NULL, "dump_single");
    fill(&fl, 40, 123456789, 16666);
    check_dump_roundtrip(&fl, 0, 40, 123456789, 16666, NULL, NULL, "dump_40_frames");
    fill(&fl, S2A_FL_CAP + 37, 64000, 8333);
    check_dump_roundtrip(&fl, 37, S2A_FL_CAP, 64000, 8333, NULL, NULL, "dump_full_ring_999_chunks");
    fill(&fl, 3, 0, 0);
    check_dump_roundtrip(&fl, 0, 3, 0, 0, NULL, NULL, "dump_zero_timestamps");
    fill(&fl, 2, UINT64_MAX - 20, 11);   // 时间戳接近 u64 顶（不进位）：编码照常
    check_dump_roundtrip(&fl, 0, 2, UINT64_MAX - 20, 11, NULL, NULL, "dump_near_u64max_ts");

    printf("  case %-32s", "dup_timestamp_stored");
    s2a_fl_reset(&fl);
    s2a_fl_note(&fl, 424242);
    s2a_fl_note(&fl, 424242);   // 同一微秒戳（重复帧）：两条都记，异常留给解析端判
    CHECK(fl.kept == 2 && s2a_fl_at(&fl, 0) == 424242 && s2a_fl_at(&fl, 1) == 424242);
    printf("ok\n");

    printf("  case %-32s", "fence_snapshot_reset");
    s2a_fl_reset(&fl);
    s2a_fl_fence(&fl, 1500);          // 1.5 µs
    s2a_fl_fence(&fl, 700);           // 共 2200 ns → 记录存 2 µs
    s2a_fl_note(&fl, 1000);
    s2a_fl_fence(&fl, 5000000);
    s2a_fl_note(&fl, 2000);           // rec1: 上一帧之后的 1 次 5000 µs
    s2a_fl_note(&fl, 3000);           // rec2: 无等待
    CHECK(s2a_fl_fence_us(&fl, 0) == 2 && s2a_fl_fence_n(&fl, 0) == 2);
    CHECK(s2a_fl_fence_us(&fl, 1) == 5000 && s2a_fl_fence_n(&fl, 1) == 1);
    CHECK(s2a_fl_fence_us(&fl, 2) == 0 && s2a_fl_fence_n(&fl, 2) == 0);
    CHECK(fl.fence_ns_acc == 0 && fl.fence_n_acc == 0);   // 快照即清零
    CHECK(s2a_fl_fence_us(&fl, 9) == 0 && s2a_fl_fence_n(&fl, 9) == 0);   // 越界
    printf("ok\n");

    printf("  case %-32s", "fence_u32_clamp");
    s2a_fl_reset(&fl);
    atomic_store(&fl.fence_ns_acc, UINT64_C(5000000000) * 1000);   // 超 u32 的 µs 量
    atomic_store(&fl.fence_n_acc, UINT64_C(6000000000));
    s2a_fl_note(&fl, 7);
    CHECK(s2a_fl_fence_us(&fl, 0) == UINT32_MAX && s2a_fl_fence_n(&fl, 0) == UINT32_MAX);
    printf("ok\n");

    printf("  case %-32s", "fence_survives_ring_wrap");
    fill(&fl, S2A_FL_CAP + 5, 100, 1);
    s2a_fl_fence(&fl, 123000);
    s2a_fl_note(&fl, 999);            // 第 CAP+6 帧，落进环、把最旧的一格顶掉
    CHECK(s2a_fl_fence_n(&fl, S2A_FL_CAP - 1) == 1);
    CHECK(s2a_fl_fence_us(&fl, S2A_FL_CAP - 1) == 123);
    CHECK(s2a_fl_at(&fl, S2A_FL_CAP - 1) == 999);
    printf("ok\n");

    {
        // fence 字段进载荷：4 帧各带 (i+1) 次、每次 (i+1)*1000 µs，整段字节级核回。
        uint32_t fus[4], fns[4];
        s2a_fl_reset(&fl);
        for (int i = 0; i < 4; i++) {
            for (int k = 0; k <= i; k++) s2a_fl_fence(&fl, 1000000);
            s2a_fl_note(&fl, 500 + i);
            fus[i] = (uint32_t)(i + 1) * 1000;
            fns[i] = (uint32_t)(i + 1);
        }
        check_dump_roundtrip(&fl, 0, 4, 500, 1, fus, fns, "dump_with_fence_fields");
    }

    printf("  case %-32s", "counters_saturate_no_wrap");
    fill(&fl, S2A_FL_CAP + 10, 777, 3);   // 合法状态：kept=CAP、w=10、dropped=10
    fl.seq = UINT64_MAX - 1;
    fl.dropped = UINT64_MAX - 1;          // 把两个计数抬到顶附近（注入构造）
    s2a_fl_note(&fl, 999);
    CHECK(fl.seq == UINT64_MAX && fl.dropped == UINT64_MAX && fl.kept == S2A_FL_CAP);
    CHECK(s2a_fl_at(&fl, S2A_FL_CAP - 1) == 999);          // 新帧照常落环
    CHECK(s2a_fl_at(&fl, 0) == 777 + 11 * 3);              // 最旧的又被覆盖一格
    s2a_fl_note(&fl, 1000);
    CHECK(fl.seq == UINT64_MAX && fl.dropped == UINT64_MAX);   // 双计数都封顶不回绕
    printf("ok\n");

    printf("  case %-32s", "site_window_split_and_reset");
    s2a_fl_reset(&fl);
    s2a_fl_fence_site(&fl, 2000000, 11262);
    s2a_fl_fence_site(&fl, 3000000, 11262);
    s2a_fl_fence_site(&fl, 9000000, 1);
    for (unsigned i = 0; i < S2A_FL_SITE_WINDOW - 1; i++) s2a_fl_note(&fl, i);
    CHECK(!fl.sites_pending && s2a_fl_fence_n(&fl, 0) == 3 && s2a_fl_fence_us(&fl, 0) == 14000);
    s2a_fl_note(&fl, 119);
    CHECK(fl.sites_pending && fl.site_frames == 0);
    CHECK(fl.sites_ready[0].id == 11262 && fl.sites_ready[0].count == 2 && fl.sites_ready[0].ns == 5000000);
    CHECK(fl.sites_ready[1].id == 1 && fl.sites_ready[1].count == 1 && fl.sites_ready[1].ns == 9000000);
    CHECK(fl.sites[0].count == 0 && fl.sites[1].count == 0);
    s2a_fl_fence_site(&fl, 1000, 2);
    CHECK(fl.sites[0].id == 2 && fl.sites[0].count == 1 && fl.sites[0].ns == 1000);
    printf("ok\n");

    printf("  case %-32s", "enable_gate_default_off");
    unsetenv("debug.s2a.frame_log");
    g_fl_on = -1;   // unity：复位开关缓存再验（生产里属性一次性读入缓存）
    s2a_fl_reset(&g_fl);
    nvn_vk_frame_log_present();
    nvn_vk_frame_log_fence_site(200000, 2);
    nvn_vk_frame_log_fence_wait(99999);   // 关态：fence 账也不动
    CHECK(g_fl.seq == 0 && g_fl.fence_n_acc == 0);
    printf("ok\n");

    printf("  case %-32s", "enable_gate_on_records");
    setenv("debug.s2a.frame_log", "1", 1);
    g_fl_on = -1;
    struct timespec t0, t1;
#if defined(CLOCK_BOOTTIME)
    clock_gettime(CLOCK_BOOTTIME, &t0);
#else
    clock_gettime(CLOCK_MONOTONIC, &t0);
#endif
    nvn_vk_frame_log_fence_wait(2500000);   // 开态：fence 等待累计进帧内账
    nvn_vk_frame_log_present();
#if defined(CLOCK_BOOTTIME)
    clock_gettime(CLOCK_BOOTTIME, &t1);
#else
    clock_gettime(CLOCK_MONOTONIC, &t1);
#endif
    CHECK(g_fl.seq == 1 && g_fl.kept == 1);
    CHECK(s2a_fl_fence_n(&g_fl, 0) == 1 && s2a_fl_fence_us(&g_fl, 0) == 2500);
    {
        uint64_t lo = (uint64_t)t0.tv_sec * 1000000ull + (uint64_t)t0.tv_nsec / 1000ull;
        uint64_t hi = (uint64_t)t1.tv_sec * 1000000ull + (uint64_t)t1.tv_nsec / 1000ull;
        CHECK(s2a_fl_at(&g_fl, 0) >= lo && s2a_fl_at(&g_fl, 0) <= hi);
    }
    printf("ok\n");

    printf("  case %-32s", "on_gate_off_no_timing_no_account");
    s2a_fl_reset(&g_fl);
    unsetenv("debug.s2a.frame_log");
    g_fl_on = -1;
    // 关态：vk_record.c/vk_clear.c 的调用点先判 on()——返 0 时连计时都不做、账也不进。
    CHECK(nvn_vk_frame_log_on() == 0);
    if (nvn_vk_frame_log_on()) nvn_vk_frame_log_fence_wait(999999);
    CHECK(g_fl.seq == 0 && g_fl.fence_n_acc == 0 && g_fl.fence_ns_acc == 0);
    printf("ok\n");

    printf("  case %-32s", "on_gate_on_accounts_wait");
    setenv("debug.s2a.frame_log", "1", 1);
    g_fl_on = -1;
    CHECK(nvn_vk_frame_log_on() == 1);
    if (nvn_vk_frame_log_on()) nvn_vk_frame_log_fence_wait(2000000);
    CHECK(g_fl.fence_n_acc == 1 && g_fl.fence_ns_acc == 2000000);
    nvn_vk_frame_log_present();          // 开态记账照常：present 时快照进记录并清零
    CHECK(g_fl.seq == 1 && g_fl.fence_n_acc == 0 && g_fl.fence_ns_acc == 0);
    CHECK(s2a_fl_fence_n(&g_fl, 0) == 1 && s2a_fl_fence_us(&g_fl, 0) == 2000);
    printf("ok\n");

    printf("  case %-32s", "dump_resets_singleton");
    nvn_vk_frame_log_dump();   // emit 走平台通道（宿主 stderr）；只核复位语义
    CHECK(g_fl.seq == 0 && g_fl.kept == 0 && g_fl.w == 0 && g_fl.dropped == 0);
    printf("ok\n");

    printf("  case %-32s", "file_matches_log_dump");
    fill(&fl, 2, 1000, 16666);
    g_bloblen = 0; g_nlines = 0;
    s2a_fl_dump(&fl, cap_emit, NULL);
    FILE *capture = tmpfile();
    CHECK(capture != NULL);
    if (capture) {
        s2a_fl_dump(&fl, s2a_fl_platform_emit, capture);
        CHECK(fflush(capture) == 0);
        CHECK(fseek(capture, 0, SEEK_SET) == 0);
        char buf[4096]; size_t n = fread(buf, 1, sizeof buf, capture);
        CHECK(n == g_bloblen && memcmp(buf, g_blob, n) == 0);
        CHECK(fclose(capture) == 0);
    }
    printf("ok\n");

    printf("  case %-32s", "dump_empty_no_output");
    s2a_fl_reset(&g_fl);   // 开关开着但一局 0 次 present：连 meta 也不打（seq==0 守卫）
    g_nlines = 0;
    nvn_vk_frame_log_dump();
    CHECK(g_nlines == 0);
    printf("ok\n");

    printf("nvn_vk_frame_log_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
