// 逐帧时间探针（宿主，确定性）：环语义 / 帧归属 / sub 溢出计数 / 待决 acquire 结转 /
// 分片转储字节级核回 / 开关开销实测（<0.1 ms/帧预算的宿主量级证据）。
// unity 引入实现文件：白盒测静态（g_ft / g_acq_* / g_last_sub / g_ft_on），另链接真实 sha256.c。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nvn/nvn_ft_probe.c"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint64_t t_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void reset(void) {
    memset(&g_ft, 0, sizeof g_ft);
    g_acq_b = g_acq_e = 0; g_acq_rc = 0; g_last_sub = NULL;
    memset(g_pid, 0, sizeof g_pid);
    g_dump_poll_ns = 0; g_dump_armed = 1;
    g_ft_on = 1;   // 白盒直接置开关（等价 debug.s2a.ft_probe=1 走通后的缓存值）
}

// ---- emit 捕获：全部行进 blob ----
static char g_blob[8 * 1024 * 1024];
static size_t g_bloblen;
static int g_nlines;
static char g_first[1024], g_last[1024];
static void cap_emit(void *ctx, const char *line) {
    (void)ctx;
    g_nlines++;
    if (g_nlines == 1) snprintf(g_first, sizeof g_first, "%s", line);
    snprintf(g_last, sizeof g_last, "%s", line);
    size_t l = strlen(line);
    if (g_bloblen + l + 1 < sizeof g_blob) { memcpy(g_blob + g_bloblen, line, l); g_bloblen += l; g_blob[g_bloblen++] = '\n'; }
    g_blob[g_bloblen] = 0;   // 行扫循环按 NUL 收尾——重灌小转储后不留上一次的大残留
}

static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
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

// 合成一帧：acquire → nsub 次提交（各段自带开段戳 = 其 cmd 的 ft_dec_b）→ present，全部用显式时间戳（确定性）
static void frame(uint64_t ticket, uint64_t base, int nsub) {
    s2a_ft_acquire(base + 1, base + 2, 0);
    for (int i = 0; i < nsub; i++) {
        s2a_ft_submit(base + 10 + i * 10, base + 11 + i * 10, base + 13 + i * 10, 100 + i, base + 5 + i * 10);
        s2a_ft_seg_end(base + 15 + i * 10);
    }
    s2a_ft_present(ticket, base + 200, base + 205, 0, 0);
}

int main(void) {
    // 1) 基本帧记录：字段全落、归属正确
    reset();
    frame(7, 1000, 2);
    CHECK(g_ft.kept == 1 && g_ft.seq == 1 && g_ft.dropped == 0);
    {
        const struct s2a_ft_rec *r = &g_ft.rec[0];
        CHECK(r->ticket == 7);
        CHECK(r->acq_b == 1001 && r->acq_e == 1002 && r->acq_rc == 0);
        CHECK(r->pre_b == 1200 && r->pre_e == 1205 && r->pre_rc == 0);
        CHECK(r->rep_b == 1010 && r->rep_e == 1015 + 10);   // 首段 seg_b → 末段 seg_e
        CHECK(r->nsub == 2 && r->nkept == 2);
        CHECK(r->sub[0].key == 100 && r->sub[0].seg_b == 1010 && r->sub[0].sub_b == 1011 && r->sub[0].sub_e == 1013 && r->sub[0].seg_e == 1015);
        CHECK(r->sub[0].dec_b == 1005 && r->sub[1].dec_b == 1015);   // 各段解码开工点
        CHECK(r->dec_b == 1005);   // 帧回放起点 = 首段 dec_b
        CHECK(r->sub[1].key == 101 && r->sub[1].seg_e == 1025);
        // 新开窗槽已清零
        CHECK(g_ft.rec[1].nsub == 0 && g_ft.rec[1].ticket == 0);
    }

    // 2) 归属：present 之间的提交计入后一帧；present 前的提交计入本帧
    reset();
    s2a_ft_submit(500, 501, 502, 55, 490); s2a_ft_seg_end(503);   // 开窗帧的孤立提交（自带开段戳 490）
    frame(1, 1000, 1);
    frame(2, 2000, 3);
    CHECK(g_ft.kept == 2);
    {
        const struct s2a_ft_rec *r0 = &g_ft.rec[0];
        CHECK(r0->ticket == 1 && r0->nsub == 2);   // 孤立提交 + 本帧 1 次
        CHECK(r0->rep_b == 500);                    // 窗口起点 = 最早的孤立提交
        const struct s2a_ft_rec *r1 = &g_ft.rec[1];
        CHECK(r1->ticket == 2 && r1->nsub == 3 && r1->rep_b == 2010 && r1->rep_e == 2035);
    }

    // 3) 待决 acquire 结转：acquire 后掉帧（无 present），下一次成功帧携带末次 acq
    reset();
    s2a_ft_acquire(300, 301, 4);     // SUBOPTIMAL —— 假设随后掉帧
    s2a_ft_acquire(400, 402, 0);     // 下一次成功帧的 acquire（覆盖待决槽）
    s2a_ft_present(9, 500, 505, 0, 0);
    CHECK(g_ft.kept == 1);
    CHECK(g_ft.rec[0].acq_b == 400 && g_ft.rec[0].acq_e == 402 && g_ft.rec[0].acq_rc == 0);

    // 4) sub 溢出：nsub 记全，nkept 封顶，sub_dropped 记丢弃
    reset();
    frame(1, 1000, S2A_FT_SUB_MAX + 4);
    CHECK(g_ft.rec[0].nsub == S2A_FT_SUB_MAX + 4);
    CHECK(g_ft.rec[0].nkept == S2A_FT_SUB_MAX);
    CHECK(g_ft.sub_dropped == 4);
    CHECK(g_ft.rec[0].rep_e == 1013 + (S2A_FT_SUB_MAX + 3) * 10);   // rep_e 跟到末条提交（含满表丢弃的）

    // 5) 提交未成段（无 seg_end）：seg_e=0、rep_e 留在 sub_e
    reset();
    s2a_ft_submit(700, 701, 702, 8, 690);   // 模拟提交失败后无 commit 出口
    s2a_ft_present(1, 800, 805, -1000003000, 0);   // VK_ERROR_OUT_OF_DATE_KHR
    CHECK(g_ft.rec[0].sub[0].seg_e == 0 && g_ft.rec[0].rep_e == 702);
    CHECK(g_ft.rec[0].pre_rc == -1000003000);

    // 5b) 并存段负控（qa-dena #477910）：主 g_cmd(B, 880 开) 期间 rp_xfer_cmd 另开 g_pre(A, 900 开)。
    //     旧全局单槽实现：A 开段覆盖 B 的戳、A 提交携走后 B 丢 0——现在戳随各自 cmd：
    //     A 先交(910) 携 900、B 后交(920) 仍携 880，帧级取最早 = 880。
    //     内嵌/校准段仍不带戳（dec_b=0）。
    reset();
    s2a_ft_submit(910, 911, 912, 42, 900);                       // g_pre(A) 先提交，携自己的 900
    s2a_ft_seg_end(913);
    s2a_ft_submit(915, 916, 917, S2A_FT_KEY_PRESENT | 7, 0);     // 呈现内嵌段——无开段戳
    s2a_ft_submit(918, 919, 920, 0, 0);                          // 校准提交（key=0）
    s2a_ft_submit(921, 922, 923, 43, 880);                       // 主 g_cmd(B) 后提交，携自己的 880
    s2a_ft_seg_end(924);
    s2a_ft_present(1, 940, 945, 0, 0);
    {
        const struct s2a_ft_rec *r = &g_ft.rec[0];
        CHECK(r->nsub == 4 && r->nkept == 4);
        CHECK(r->sub[0].dec_b == 900 && r->sub[3].dec_b == 880);   // 并存段各携各戳、后交的不丢
        CHECK(r->sub[1].dec_b == 0 && r->sub[2].dec_b == 0);       // 非回放段无开段点
        CHECK(r->dec_b == 880);                                    // 帧回放起点 = 窗口内最早开段（非最先提交）
        CHECK(r->rep_b == 910);                                    // commit 窗仍从最早提交起
    }

    // 5c) presentID 回填：timing_id 登记槽位，回报按 id 对号写 actual_present；错 id 不写
    reset();
    s2a_ft_present(11, 100, 105, 0, 3001);   // 帧 11 用 presentID 3001
    s2a_ft_present(12, 200, 205, 0, 3002);   // 帧 12 用 presentID 3002
    s2a_ft_present_actual(9999, 777);        // 未登记的 id —— 丢弃
    s2a_ft_present_actual(3001, 555);        // 帧 11 的回报
    CHECK(g_ft.rec[0].ticket == 11 && g_ft.rec[0].timing_id == 3001 && g_ft.rec[0].actual_present == 555);
    CHECK(g_ft.rec[1].ticket == 12 && g_ft.rec[1].timing_id == 3002 && g_ft.rec[1].actual_present == 0);
    s2a_ft_present(13, 300, 305, 0, 0);      // 无 timing（不支持/未开开关）——不参与映射
    CHECK(g_ft.rec[2].timing_id == 0 && g_ft.rec[2].actual_present == 0);

    // 6) 环边界（qa-dena #477910）：开窗恒占一槽 → 有效 kept = CAP-1；
    //    满环转储的最旧条必须是真闭合记录（旧实现 first_ticket=0）。
    reset();
    {
        char want[160];
        // 6a) CAP-1 帧正满（kept=CAP-1、dropped=0）：转储 first/last 全真
        for (uint64_t i = 0; i < S2A_FT_CAP - 1; i++) frame(i + 1, 1000 + i * 1000, 0);
        CHECK(g_ft.kept == S2A_FT_CAP - 1 && g_ft.seq == S2A_FT_CAP - 1 && g_ft.dropped == 0);
        g_bloblen = 0; g_nlines = 0; g_first[0] = g_last[0] = 0;
        s2a_ft_dump_to(&g_ft, cap_emit, NULL);
        snprintf(want, sizeof want, "frames=%u", S2A_FT_CAP - 1);
        CHECK(strstr(g_last, want));
        snprintf(want, sizeof want, "first_ticket=1 last_ticket=%u", S2A_FT_CAP - 1);
        CHECK(strstr(g_last, want));
        snprintf(want, sizeof want, "bytes=%llu", (unsigned long long)((S2A_FT_CAP - 1) * sizeof(struct s2a_ft_rec)));
        CHECK(strstr(g_last, want));
        // 6b) CAP 帧：第一次满挤，最旧 ticket=1 被丢
        frame(S2A_FT_CAP, 1000 + (S2A_FT_CAP - 1) * 1000, 0);
        CHECK(g_ft.kept == S2A_FT_CAP - 1 && g_ft.dropped == 1);
        g_bloblen = 0; g_nlines = 0; g_first[0] = g_last[0] = 0;
        s2a_ft_dump_to(&g_ft, cap_emit, NULL);
        snprintf(want, sizeof want, "first_ticket=2 last_ticket=%u", S2A_FT_CAP);
        CHECK(strstr(g_last, want));
        // 6c) CAP+10 帧：稳定满环，dropped=11、最新帧在 w-1
        for (uint64_t i = S2A_FT_CAP; i < S2A_FT_CAP + 10; i++) frame(i + 1, 1000 + i * 1000, 0);
        CHECK(g_ft.kept == S2A_FT_CAP - 1 && g_ft.seq == S2A_FT_CAP + 10 && g_ft.dropped == 11);
        CHECK(g_ft.rec[g_ft.w == 0 ? S2A_FT_CAP - 1 : g_ft.w - 1].ticket == S2A_FT_CAP + 10);   // 最新帧在 w-1
        g_bloblen = 0; g_nlines = 0; g_first[0] = g_last[0] = 0;
        s2a_ft_dump_to(&g_ft, cap_emit, NULL);
        snprintf(want, sizeof want, "first_ticket=12 last_ticket=%u", S2A_FT_CAP + 10);
        CHECK(strstr(g_last, want));
    }

    // 7) 转储：meta/chunk/end 行 + 解码回字节 + 内容对拍
    reset();
    for (int i = 0; i < 64; i++) frame(i + 1, 1000 + i * 1000, 2);
    {
        g_bloblen = 0; g_nlines = 0; g_first[0] = g_last[0] = 0;
        s2a_ft_dump_to(&g_ft, cap_emit, NULL);
        CHECK(g_nlines >= 3);
        CHECK(strstr(g_first, "direct frame-trace meta: v=2 clock=CLOCK_MONOTONIC") == g_first);
        CHECK(strstr(g_last, "direct frame-trace end: frames=64") == g_last);
        CHECK(strstr(g_last, "bytes=") && strstr(g_last, "first_ticket=1") && strstr(g_last, "last_ticket=64"));
        CHECK(strstr(g_blob, "sha256") == NULL);   // 完整性只用序号/长度/条数/顺序，载荷不做摘要
        // 收集 chunk 载荷、解码、与记录字节序对拍
        static uint8_t dec[4 * 1024 * 1024];
        size_t dec_len = 0;
        char *p = g_blob;
        while (p && *p) {
            char *nl = strchr(p, '\n');
            size_t ll = nl ? (size_t)(nl - p) : strlen(p);
            char *col = memchr(p, ':', ll);
            if (col && strncmp(p, "direct frame-trace[", 19) == 0) {
                long n = b64dec(col + 1, ll - (size_t)(col + 1 - p), dec + dec_len);
                CHECK(n > 0);
                if (n > 0) dec_len += (size_t)n;
            }
            p = nl ? nl + 1 : p + ll;
        }
        CHECK(dec_len == 64 * sizeof(struct s2a_ft_rec));
        if (dec_len == 64 * sizeof(struct s2a_ft_rec)) {
            for (int i = 0; i < 64; i++) {
                const struct s2a_ft_rec *d = (const struct s2a_ft_rec *)(dec + (size_t)i * sizeof *d);
                CHECK(d->ticket == (uint64_t)(i + 1));
                CHECK(d->nsub == 2 && d->sub[1].key == 101);
                if (g_fail) break;
            }
        }
    }

    // 7b) 缺帧可判：ticket 3,4 未 present（掉帧/失败帧不闭合）——
    //     end 行 frames 与 first..last 跨度不等即检出，不需摘要
    reset();
    frame(1, 1000, 0); frame(2, 2000, 0); frame(5, 3000, 0); frame(6, 4000, 0);
    {
        g_bloblen = 0; g_nlines = 0; g_first[0] = g_last[0] = 0;
        s2a_ft_dump_to(&g_ft, cap_emit, NULL);
        CHECK(strstr(g_last, "frames=4") && strstr(g_last, "first_ticket=1") && strstr(g_last, "last_ticket=6"));
        // QA 侧：last-first+1=6 ≠ frames=4 → 3,4 缺帧；记录内 ticket 序列同样断档可定位
    }

    // 8) 开销实测：开关 ON 的合成帧循环 ns/帧（<0.1 ms 预算的宿主量级证据）
    reset();
    {
        const int N = 200000;
        uint64_t t0 = t_ns();
        for (int i = 0; i < N; i++) {
            uint64_t b = s2a_ft_on() ? t_ns() : 0;   // 与挂点同形态：先查开关再计时
            if (b) s2a_ft_acquire(b, t_ns(), 0);
            for (int j = 0; j < 4; j++) { uint64_t q = b ? t_ns() : 0; if (q) { s2a_ft_submit(b, q, t_ns(), j, q - 1); s2a_ft_seg_end(t_ns()); } }
            uint64_t p = b ? t_ns() : 0;
            if (p) s2a_ft_present((uint64_t)i, p, t_ns(), 0, 0);
        }
        uint64_t dt = t_ns() - t0;
        double ns_per = (double)dt / N;
        printf("    ft probe overhead: %.0f ns/frame (%d frames, 4 submits each)\n", ns_per, N);
        CHECK(ns_per < 100000.0);   // 预算 0.1 ms/帧——宿主量级证据（设备端同构代码路径）
    }

    // 9) 开关 OFF：空环静默；有数据但开关关也静默
    reset();
    g_ft_on = 0;
    CHECK(s2a_ft_on() == 0);
    {
        int before = g_nlines;
        s2a_ft_dump();
        CHECK(g_nlines == before);
    }
    s2a_ft_present(1, 1, 2, 0, 0);   // 误调事件函数也有数据——开关关仍不许吐
    {
        int before = g_nlines;
        s2a_ft_dump();
        CHECK(g_nlines == before);
    }

    // 10) ft_dump 手动转储（laze #478473）：debug.s2a.ft_dump=1 → present 线程 1 Hz 节流查到即吐、
    //     回 0 重新武装；不另起线程、不跨线程动环
    reset();
    g_ft_on = 1;
    setenv("debug.s2a.ft_dump", "1", 1);
    CHECK(g_dump_armed == 1 && g_dump_poll_ns == 0);
    frame(1, 1000, 1);                         // pre_b=1200：首次查 → 吐 → 卸武装（转储走 stderr）
    CHECK(g_dump_armed == 0 && g_dump_poll_ns == 1200);
    frame(2, 2000, 1);                         // <1s 不再查属性（查也只吐一次）
    CHECK(g_dump_poll_ns == 1200 && g_dump_armed == 0);
    setenv("debug.s2a.ft_dump", "0", 1);
    frame(4, 1000001000ull, 1);                // pre_b=1000001200 ≥上次+1s → 查 → 0 → 再武装，不吐
    CHECK(g_dump_armed == 1 && g_dump_poll_ns == 1000001200ull);
    setenv("debug.s2a.ft_dump", "1", 1);
    frame(5, 2000001000ull, 1);                // pre_b=2000001200 ≥1s → 查 → 再吐
    CHECK(g_dump_armed == 0 && g_dump_poll_ns == 2000001200ull);
    unsetenv("debug.s2a.ft_dump");

    if (g_fail) { printf("FAIL %d / %d checks\n", g_fail, g_checks); return 1; }
    printf("nvn-ft-probe: ok (%d checks)\n", g_checks);
    return 0;
}
