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

// 合成一帧：acquire → nsub 次提交 → present，全部用显式时间戳（确定性）
static void frame(uint64_t ticket, uint64_t base, int nsub) {
    s2a_ft_acquire(base + 1, base + 2, 0);
    for (int i = 0; i < nsub; i++) {
        s2a_ft_submit(base + 10 + i * 10, base + 11 + i * 10, base + 13 + i * 10, 100 + i);
        s2a_ft_seg_end(base + 15 + i * 10);
    }
    s2a_ft_present(ticket, base + 200, base + 205, 0);
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
        CHECK(r->sub[1].key == 101 && r->sub[1].seg_e == 1025);
        // 新开窗槽已清零
        CHECK(g_ft.rec[1].nsub == 0 && g_ft.rec[1].ticket == 0);
    }

    // 2) 归属：present 之间的提交计入后一帧；present 前的提交计入本帧
    reset();
    s2a_ft_submit(500, 501, 502, 55); s2a_ft_seg_end(503);   // 开窗帧的孤立提交
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
    s2a_ft_present(9, 500, 505, 0);
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
    s2a_ft_submit(700, 701, 702, 8);   // 模拟提交失败后无 commit 出口
    s2a_ft_present(1, 800, 805, -1000003000);   // VK_ERROR_OUT_OF_DATE_KHR
    CHECK(g_ft.rec[0].sub[0].seg_e == 0 && g_ft.rec[0].rep_e == 702);
    CHECK(g_ft.rec[0].pre_rc == -1000003000);

    // 6) 环满覆盖：kept 封顶、dropped 记、最旧被挤掉
    reset();
    {
        uint64_t n = S2A_FT_CAP + 10;
        for (uint64_t i = 0; i < n; i++) frame(i, 1000 + i * 1000, 0);
        CHECK(g_ft.kept == S2A_FT_CAP && g_ft.seq == n && g_ft.dropped == 10);
        CHECK(g_ft.rec[g_ft.w == 0 ? S2A_FT_CAP - 1 : g_ft.w - 1].ticket == n - 1);   // 最新帧在 w-1
    }

    // 7) 转储：meta/chunk/end 行 + 解码回字节 + 内容对拍
    reset();
    for (int i = 0; i < 64; i++) frame(i + 1, 1000 + i * 1000, 2);
    {
        g_bloblen = 0; g_nlines = 0; g_first[0] = g_last[0] = 0;
        s2a_ft_dump_to(&g_ft, cap_emit, NULL);
        CHECK(g_nlines >= 3);
        CHECK(strstr(g_first, "direct frame-trace meta: v=1 clock=CLOCK_MONOTONIC") == g_first);
        CHECK(strstr(g_last, "direct frame-trace end: frames=64") == g_last);
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

    // 8) 开销实测：开关 ON 的合成帧循环 ns/帧（<0.1 ms 预算的宿主量级证据）
    reset();
    {
        const int N = 200000;
        uint64_t t0 = t_ns();
        for (int i = 0; i < N; i++) {
            uint64_t b = s2a_ft_on() ? t_ns() : 0;   // 与挂点同形态：先查开关再计时
            if (b) s2a_ft_acquire(b, t_ns(), 0);
            for (int j = 0; j < 4; j++) { uint64_t q = b ? t_ns() : 0; if (q) { s2a_ft_submit(b, q, t_ns(), j); s2a_ft_seg_end(t_ns()); } }
            uint64_t p = b ? t_ns() : 0;
            if (p) s2a_ft_present((uint64_t)i, p, t_ns(), 0);
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
    s2a_ft_present(1, 1, 2, 0);   // 误调事件函数也有数据——开关关仍不许吐
    {
        int before = g_nlines;
        s2a_ft_dump();
        CHECK(g_nlines == before);
    }

    if (g_fail) { printf("FAIL %d / %d checks\n", g_fail, g_checks); return 1; }
    printf("nvn-ft-probe: ok (%d checks)\n", g_checks);
    return 0;
}
