// 逐帧时间探针实现（字段口径 / 归属模型 / 转储格式见 nvn_ft_probe.h 头注释）。
// 写者串行化靠调用方的 g_submit_mu（提交、呈现）与同线程序（acquire→present 同一线程），
// 环内不取锁；转储在 nvk_session_close 的 nvn_gpu_close 之后，写者已停。
#include "nvn_ft_probe.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

// 最近一条待补 seg_e 的 sub：commit_impl 在 vkQueueSubmit 点写记录、在 commit 出口补 seg_e。
// 提交路径全部持 g_submit_mu，指向「当前开窗记录最近一条 sub」是串行的。
static struct s2a_ft_sub *g_last_sub;

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

void s2a_ft_submit(uint64_t seg_b, uint64_t sub_b, uint64_t sub_e, uint64_t key) {
    struct s2a_ft_rec *r = &g_ft.rec[g_ft.w];
    if (r->nsub == 0) r->rep_b = seg_b;
    if (r->nkept < S2A_FT_SUB_MAX) {
        struct s2a_ft_sub *s = &r->sub[r->nkept++];
        s->seg_b = seg_b; s->sub_b = sub_b; s->sub_e = sub_e; s->seg_e = 0; s->key = key;
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

void s2a_ft_present(uint64_t ticket, uint64_t pre_b, uint64_t pre_e, int32_t rc) {
    struct s2a_ft_rec *r = &g_ft.rec[g_ft.w];
    r->ticket = ticket;
    r->acq_b = g_acq_b; r->acq_e = g_acq_e; r->acq_rc = g_acq_rc;
    g_acq_b = g_acq_e = 0; g_acq_rc = 0;
    r->pre_b = pre_b; r->pre_e = pre_e; r->pre_rc = rc;
    g_last_sub = NULL;
    g_ft.w = (g_ft.w + 1) % S2A_FT_CAP;
    if (g_ft.kept < S2A_FT_CAP) g_ft.kept++;
    else if (g_ft.dropped < UINT64_MAX) g_ft.dropped++;
    if (g_ft.seq < UINT64_MAX) g_ft.seq++;
    memset(&g_ft.rec[g_ft.w], 0, offsetof(struct s2a_ft_rec, sub));   // 新开窗槽：头部清零
}

// ---------------- 转储（logcat 分片 base64 + sha256，同 frame-log 例） ----------------

static uint32_t ft_slot(const struct s2a_ft_ring *ft, uint64_t i) {
    return (uint32_t)((ft->w + S2A_FT_CAP - ft->kept + i) % S2A_FT_CAP);
}

// 载荷第 j 字节（kept 条定长记录，最旧→最新），不在堆里重排整段。
static uint8_t ft_byte(const struct s2a_ft_ring *ft, uint64_t j) {
    const uint8_t *base = (const uint8_t *)&ft->rec[ft_slot(ft, j / sizeof(struct s2a_ft_rec))];
    return base[j % sizeof(struct s2a_ft_rec)];
}

#define S2A_FT_CHUNK 700
static const char ft_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void s2a_ft_dump_to(struct s2a_ft_ring *ft, void (*emit)(void *ctx, const char *line), void *ctx) {
    if (!ft || !emit) return;
    const uint64_t kept = ft->kept;
    const uint64_t bytes = kept * sizeof(struct s2a_ft_rec);
    const uint64_t b64len = ((bytes + 2) / 3) * 4;
    const unsigned chunks = (unsigned)((b64len + S2A_FT_CHUNK - 1) / S2A_FT_CHUNK);
    char line[800];
    char uuid_tail[128] = "";
#if defined(__ANDROID__)
    char uuid[PROP_VALUE_MAX];
    if (__system_property_get("debug.s2a.session_uuid", uuid) <= 0) strcpy(uuid, "-");
    snprintf(uuid_tail, sizeof uuid_tail, " uuid=%s", uuid);
#endif
    snprintf(line, sizeof line,
             "direct frame-trace meta: v=1 clock=CLOCK_MONOTONIC unit=ns endian=le rec=%u cap=%u "
             "recorded=%llu dropped=%llu sub_dropped=%llu first=%llu bytes=%llu chunks=%u%s",
             (unsigned)sizeof(struct s2a_ft_rec), S2A_FT_CAP, (unsigned long long)kept,
             (unsigned long long)ft->dropped, (unsigned long long)ft->sub_dropped,
             (unsigned long long)(ft->seq - kept), (unsigned long long)bytes, chunks, uuid_tail);
    emit(ctx, line);

    uint8_t stage[4096];
    uint8_t rem[3];
    unsigned rn = 0, cpos = 0, part = 0;
    char chunk[S2A_FT_CHUNK + 4];
    s2a_sha256 hash;
    s2a_sha256_init(&hash);
    uint64_t off = 0;
    while (off < bytes) {
        size_t n = (size_t)(bytes - off < sizeof stage ? bytes - off : sizeof stage);
        for (size_t i = 0; i < n; i++) stage[i] = ft_byte(ft, off + i);
        s2a_sha256_update(&hash, stage, n);
        off += n;
        size_t i = 0;
        while (i < n) {
            while (rn < 3 && i < n) rem[rn++] = stage[i++];
            if (rn < 3) break;
            chunk[cpos++] = ft_b64[rem[0] >> 2];
            chunk[cpos++] = ft_b64[((rem[0] & 3) << 4) | (rem[1] >> 4)];
            chunk[cpos++] = ft_b64[((rem[1] & 15) << 2) | (rem[2] >> 6)];
            chunk[cpos++] = ft_b64[rem[2] & 63];
            rn = 0;
            if (cpos == S2A_FT_CHUNK) {
                int m = snprintf(line, sizeof line, "direct frame-trace[%u/%u]:%.*s",
                                 part + 1, chunks, (int)cpos, chunk);
                if (m > 0) emit(ctx, line);
                cpos = 0;
                part++;
            }
        }
    }
    if (rn) {
        for (unsigned k = rn; k < 3; k++) rem[k] = 0;
        chunk[cpos++] = ft_b64[rem[0] >> 2];
        chunk[cpos++] = ft_b64[((rem[0] & 3) << 4) | (rem[1] >> 4)];
        chunk[cpos++] = rn == 1 ? '=' : ft_b64[((rem[1] & 15) << 2) | (rem[2] >> 6)];
        chunk[cpos++] = '=';
    }
    if (cpos) {
        int m = snprintf(line, sizeof line, "direct frame-trace[%u/%u]:%.*s",
                         part + 1, chunks, (int)cpos, chunk);
        if (m > 0) emit(ctx, line);
    }
    uint8_t digest[32];
    char hex[65];
    s2a_sha256_final(&hash, digest);
    s2a_sha256_hex(digest, hex);
    snprintf(line, sizeof line, "direct frame-trace end: frames=%llu dropped=%llu sha256=%s",
             (unsigned long long)kept, (unsigned long long)ft->dropped, hex);
    emit(ctx, line);
}

static void ft_platform_emit(void *ctx, const char *line) {
    (void)ctx;
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "S2Arm", "%s", line);
#else
    fprintf(stderr, "%s\n", line);
#endif
}

// 局末批量吐出：nvk_session_close 在 nvn_vk_frame_log_dump 旁弱引用；此刻写者已停。
// 开关关着 / 一帧没记过时什么都不做。
void s2a_ft_dump(void) {
    if (!s2a_ft_on() || !g_ft.kept) return;
    s2a_ft_dump_to(&g_ft, ft_platform_emit, NULL);
}
