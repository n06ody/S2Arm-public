/* mc 解码边界宿主测试：假 AMediaCodec 状态机喂编排出图，
 * 验证 s2a_mc.c 的开关门、NV12 解交织、平面拷贝、裁剪、有界等待与失败粘性。
 * 假实现只模拟契约层（队列/格式/缓冲），不是解码器——解码对错由真机层二验证。 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "media/NdkMediaCodec.h"
#include "../codecs/s2a_mc.h"

#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)
static int g_failures;

/* ---- 假 AMediaFormat：int/string 键值表 ---- */
struct AMediaFormat { int32_t iv[16]; const char *ik[16]; char sv[4][64]; const char *sk[4]; int ni, ns; };
const char* AMEDIAFORMAT_KEY_MIME = "mime";
const char* AMEDIAFORMAT_KEY_WIDTH = "width";
const char* AMEDIAFORMAT_KEY_HEIGHT = "height";
const char* AMEDIAFORMAT_KEY_COLOR_FORMAT = "color-format";
const char* AMEDIAFORMAT_KEY_SLICE_HEIGHT = "slice-height";
const char* AMEDIAFORMAT_KEY_STRIDE = "stride";
AMediaFormat* AMediaFormat_new(void) { return calloc(1, sizeof(AMediaFormat)); }
media_status_t AMediaFormat_delete(AMediaFormat *f) { free(f); return AMEDIA_OK; }
void AMediaFormat_setInt32(AMediaFormat *f, const char *k, int32_t v)
{
    if (!f || f->ni >= 16) return;
    f->ik[f->ni] = k; f->iv[f->ni] = v; f->ni++;
}
bool AMediaFormat_getInt32(AMediaFormat *f, const char *k, int32_t *out)
{
    if (!f || !out) return false;
    for (int i = 0; i < f->ni; i++) if (f->ik[i] == k || (f->ik[i] && k && strcmp(f->ik[i], k) == 0)) { *out = f->iv[i]; return true; }
    return false;
}
void AMediaFormat_setString(AMediaFormat *f, const char *k, const char *v)
{
    if (!f || f->ns >= 4 || strlen(v) >= 64) return;
    f->sk[f->ns] = k; strncpy(f->sv[f->ns], v, 63); f->ns++;
}

/* ---- 假 AMediaCodec：脚本化输出队列 ---- */
#define MC_MAX_OUT 1024
struct out_event {
    ssize_t kind;          /* <0 用 AMEDIACODEC_INFO_*；>=0 是缓冲下标 */
    const uint8_t *data;   /* 输出负载 */
    uint32_t size, offset;
    int32_t color, stride, slice_h, cl, cr, ct, cb;
    uint32_t flags;
    uint32_t fmask;        /* getOutputFormat 实际写哪些键：1=color 2=stride 4=slice 8=crop */
};
struct AMediaCodec {
    int deleted, configured, started, stopped;
    uint32_t width, height;
    uint8_t input[1 << 20];
    uint32_t input_len, input_used;
    int input_slots;
    uint64_t input_time;
    int queue_calls, dequeue_out_calls, released;
    struct out_event ev[MC_MAX_OUT];
    int nev, evi;
    int consume_inputs;              /* 假 codec 每次 dequeue 消化一个排队 AU（活 codec 模型） */
    /* pattern 模式：每第 pattern_mod 个 AU 模拟隐藏帧不出图，其余各产一帧 */
    int pattern_mod;
    const uint8_t *pattern_frame;
    uint32_t pattern_size;
    int32_t pattern_w, pattern_h, pattern_stride, pattern_slice;
};

static int g_create_null, g_configure_fail, g_start_fail;
static AMediaCodec *g_last;

AMediaCodec* AMediaCodec_createDecoderByType(const char *mime)
{
    if (g_create_null || strcmp(mime, "video/x-vnd.on2.vp9") != 0) return NULL;
    g_last = calloc(1, sizeof(AMediaCodec));
    if (g_last) { g_last->input_slots = 4; g_last->consume_inputs = 1; }
    return g_last;
}
media_status_t AMediaCodec_delete(AMediaCodec *c) { if (c) c->deleted = 1; free(c); return AMEDIA_OK; }
media_status_t AMediaCodec_configure(AMediaCodec *c, const AMediaFormat *f, ANativeWindow *s, AMediaCrypto *cr, uint32_t fl)
{
    (void)s; (void)cr; (void)fl;
    if (g_configure_fail) return AMEDIA_ERROR_INVALID_OPERATION;
    c->configured = 1;
    int32_t v;
    if (AMediaFormat_getInt32((AMediaFormat *)f, "width", &v)) c->width = (uint32_t)v;
    if (AMediaFormat_getInt32((AMediaFormat *)f, "height", &v)) c->height = (uint32_t)v;
    return AMEDIA_OK;
}
media_status_t AMediaCodec_start(AMediaCodec *c) { if (g_start_fail) return AMEDIA_ERROR_INVALID_OPERATION; c->started = 1; return AMEDIA_OK; }
media_status_t AMediaCodec_stop(AMediaCodec *c) { c->stopped = 1; return AMEDIA_OK; }
media_status_t AMediaCodec_flush(AMediaCodec *c) { (void)c; return AMEDIA_OK; }
uint8_t* AMediaCodec_getInputBuffer(AMediaCodec *c, size_t idx, size_t *sz) { (void)idx; *sz = sizeof(c->input); return c->input; }
uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec *c, size_t idx, size_t *sz)
{
    (void)c;
    const struct out_event *e = &c->ev[idx];
    *sz = e->size + e->offset;
    return (uint8_t *)e->data - e->offset;
}
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *c, int64_t us)
{
    (void)us;
    if (c->input_used >= (uint32_t)c->input_slots) return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    return 0;
}
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *c, size_t idx, off_t off, size_t size, uint64_t t, uint32_t fl)
{
    (void)idx; (void)off; (void)fl;
    c->queue_calls++; c->input_len = (uint32_t)size; c->input_used++; c->input_time = t;
    if (c->pattern_mod && (c->queue_calls % c->pattern_mod) != 0 && c->nev < MC_MAX_OUT) {
        struct out_event *e = &c->ev[c->nev++];
        memset(e, 0, sizeof(*e));
        e->kind = c->nev - 1;
        e->data = c->pattern_frame; e->size = c->pattern_size;
        e->color = 0x15; e->stride = c->pattern_stride; e->slice_h = c->pattern_slice;
        e->cr = c->pattern_w - 1; e->cb = c->pattern_h - 1;
    }
    return AMEDIA_OK;
}
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *c, AMediaCodecBufferInfo *info, int64_t us)
{
    (void)us;
    c->dequeue_out_calls++;
    /* 活 codec 持续消化压缩输入：输入缓冲还槽与出图解耦——隐藏帧不出图也还槽 */
    if (c->consume_inputs && c->input_used) c->input_used--;
    if (c->evi >= c->nev) return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    const struct out_event *e = &c->ev[c->evi++];
    if (e->kind >= 0) {
        info->offset = e->offset;
        info->size = (int32_t)e->size;
        info->flags = e->flags;
        info->presentationTimeUs = 0;
    }
    return e->kind;
}
AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec *c)
{
    AMediaFormat *f = AMediaFormat_new();
    for (int i = c->evi - 1; i >= 0 && i < c->nev; i = -1) {
        const struct out_event *e = &c->ev[i];
        if (e->kind < 0) continue;
        uint32_t fm = e->fmask ? e->fmask : 0xfu;
        if (fm & 1) AMediaFormat_setInt32(f, "color-format", e->color);
        if (fm & 2) AMediaFormat_setInt32(f, "stride", e->stride);
        if (fm & 4) AMediaFormat_setInt32(f, "slice-height", e->slice_h);
        if (fm & 8) {
            AMediaFormat_setInt32(f, "crop-left", e->cl);
            AMediaFormat_setInt32(f, "crop-right", e->cr);
            AMediaFormat_setInt32(f, "crop-top", e->ct);
            AMediaFormat_setInt32(f, "crop-bottom", e->cb);
        }
    }
    return f;
}
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec *c, size_t idx, bool render)
{
    (void)idx; (void)render;
    c->released++;
    return AMEDIA_OK;
}

/* ---- 帧源：确定性 NV12 / I420 图 ---- */
static uint8_t *make_nv12(uint32_t w, uint32_t h, uint32_t stride, uint32_t slice_h,
                          uint64_t *out_size, uint32_t pad)
{
    uint64_t total = (uint64_t)stride * slice_h * 3 / 2 + pad;
    uint8_t *b = malloc(total);
    memset(b, 0, total);
    for (uint32_t r = 0; r < h; r++)
        for (uint32_t x = 0; x < w; x++) b[r * stride + x] = (uint8_t)(0x20 + r * 16 + x / 16);
    uint8_t *uv = b + (uint64_t)stride * slice_h;
    for (uint32_t r = 0; r < h / 2; r++)
        for (uint32_t x = 0; x < w / 2; x++) {
            uv[r * stride + 2 * x] = (uint8_t)(0x80 + r * 4 + x);
            uv[r * stride + 2 * x + 1] = (uint8_t)(0x40 + r * 4 + x);
        }
    *out_size = total - pad;
    return b;
}
/* V 在前的半平面（OMX_QCOM_COLOR_FormatYVU420SemiPlanar = 0x7FA30C00，NV21 排布）。 */
static uint8_t *make_yvu(uint32_t w, uint32_t h, uint32_t stride, uint32_t slice_h, uint64_t *out_size)
{
    uint64_t total = (uint64_t)stride * slice_h * 3 / 2;
    uint8_t *b = malloc(total);
    memset(b, 0, total);
    for (uint32_t r = 0; r < h; r++)
        for (uint32_t x = 0; x < w; x++) b[r * stride + x] = (uint8_t)(0x20 + r * 16 + x / 16);
    uint8_t *vu = b + (uint64_t)stride * slice_h;
    for (uint32_t r = 0; r < h / 2; r++)
        for (uint32_t x = 0; x < w / 2; x++) {
            vu[r * stride + 2 * x] = (uint8_t)(0x40 + r * 4 + x);      /* V 在偶数字节 */
            vu[r * stride + 2 * x + 1] = (uint8_t)(0x80 + r * 4 + x);  /* U 在奇数字节 */
        }
    *out_size = total;
    return b;
}
static uint8_t *make_i420(uint32_t w, uint32_t h, uint32_t stride, uint32_t slice_h, uint64_t *out_size)
{
    uint64_t total = (uint64_t)stride * slice_h * 3 / 2;
    uint8_t *b = malloc(total);
    for (uint32_t r = 0; r < h; r++)
        for (uint32_t x = 0; x < w; x++) b[r * stride + x] = (uint8_t)(0x10 + r + x);
    uint8_t *u = b + (uint64_t)stride * slice_h, *v = u + (uint64_t)(stride / 2) * (slice_h / 2);
    for (uint32_t r = 0; r < h / 2; r++)
        for (uint32_t x = 0; x < w / 2; x++) { u[r * (stride / 2) + x] = (uint8_t)(0x90 + r + x); v[r * (stride / 2) + x] = (uint8_t)(0x60 + r + x); }
    *out_size = total;
    return b;
}

static s2a_mc_diag diag(void) { s2a_mc_diag d = {0}; d.abi_version = S2A_MC_ABI; d.struct_size = sizeof(d); return d; }
static s2a_mc_diag *fresh(void) { static s2a_mc_diag d; d = diag(); return &d; }
static s2a_mc_plane planes(uint8_t *y, uint8_t *u, uint8_t *v, uint32_t ws, uint32_t w, uint32_t h, s2a_mc_plane out[3])
{
    (void)w;
    out[0] = (s2a_mc_plane){y, (uint64_t)ws * h, ws, 0};
    out[1] = (s2a_mc_plane){u, (uint64_t)ws / 2 * ((h + 1) / 2), ws / 2, 0};
    out[2] = (s2a_mc_plane){v, (uint64_t)ws / 2 * ((h + 1) / 2), ws / 2, 0};
    return out[0];
}
static void fake_reset(void)
{
    g_create_null = g_configure_fail = g_start_fail = 0;
    g_last = NULL;
    s2a_mc_test_set_hw(0);
}
static void push_nv12(AMediaCodec *c, const uint8_t *b, uint32_t size,
                      uint32_t stride, uint32_t slice_h, uint32_t w, uint32_t h)
{
    struct out_event *e = &c->ev[c->nev++];
    memset(e, 0, sizeof(*e));
    e->kind = c->nev - 1;
    e->data = b; e->size = size;
    e->color = 0x15; e->stride = (int32_t)stride; e->slice_h = (int32_t)slice_h;
    e->cl = 0; e->ct = 0; e->cr = (int32_t)w - 1; e->cb = (int32_t)h - 1;
}

int main(void)
{
    /* ① 开关关：probe/create 一律 UNSUPPORTED，不碰 codec */
    fake_reset();
    s2a_mc_diag d = diag();
    CHECK(s2a_mc_probe(S2A_MC_ABI, S2A_MC_CODEC_VP9, 64, 64, &d) == S2A_MC_UNSUPPORTED && g_last == NULL,
          "hw off: probe unsupported, no codec created");
    s2a_mc_config cfg = {S2A_MC_ABI, sizeof(cfg), S2A_MC_CODEC_VP9, 64, 64, 0};
    s2a_mc *m = NULL;
    CHECK(s2a_mc_create(&cfg, &m, &d) == S2A_MC_UNSUPPORTED && m == NULL && g_last == NULL,
          "hw off: create unsupported");
    CHECK(d.stage == S2A_MC_STAGE_PROBE || d.stage == S2A_MC_STAGE_CREATE, "diag stage recorded");

    /* ② 开关开但平台没有 VP9 codec：probe/create UNSUPPORTED */
    fake_reset();
    s2a_mc_test_set_hw(1);
    g_create_null = 1;
    d = diag();
    CHECK(s2a_mc_probe(S2A_MC_ABI, S2A_MC_CODEC_VP9, 64, 64, &d) == S2A_MC_UNSUPPORTED,
          "no codec: probe unsupported");
    CHECK(s2a_mc_create(&cfg, &m, &d) == S2A_MC_UNSUPPORTED, "no codec: create unsupported");

    /* ③ configure/start 失败传播 media_status */
    fake_reset(); s2a_mc_test_set_hw(1); g_configure_fail = 1;
    d = diag();
    CHECK(s2a_mc_create(&cfg, &m, &d) == S2A_MC_BACKEND && d.media_status == AMEDIA_ERROR_INVALID_OPERATION,
          "configure fail propagates");
    fake_reset(); s2a_mc_test_set_hw(1); g_start_fail = 1;
    d = diag();
    CHECK(s2a_mc_create(&cfg, &m, &d) == S2A_MC_BACKEND, "start fail propagates");

    /* ④ NV12 出图 → 解交织进调用方三平面；输入原样进队列 */
    fake_reset(); s2a_mc_test_set_hw(1);
    {
        uint32_t w = 64, h = 32, stride = 80, slice_h = 32;
        uint64_t fsz;
        uint8_t *frame = make_nv12(w, h, stride, slice_h, &fsz, 0);
        cfg.width = w; cfg.height = h;
        d = diag();
        CHECK(s2a_mc_create(&cfg, &m, &d) == 0 && g_last->configured && g_last->started, "create ok");
        AMediaCodec *c = g_last;
        c->ev[c->nev++] = (struct out_event){AMEDIACODEC_INFO_TRY_AGAIN_LATER, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        c->ev[c->nev++] = (struct out_event){AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        push_nv12(c, frame, (uint32_t)fsz, stride, slice_h, w, h);
        static uint8_t y[80 * 32], u[40 * 16], v[40 * 16];
        memset(y, 0xee, sizeof(y)); memset(u, 0xee, sizeof(u)); memset(v, 0xee, sizeof(v));
        s2a_mc_plane dst[3];
        planes(y, u, v, 80, w, h, dst);
        uint8_t au[5] = {0xde, 0xad, 0xbe, 0xef, 0x42};
        d = diag();
        CHECK(s2a_mc_decode(m, au, sizeof(au), w, h, dst, 7, &d) == 0, "nv12 decode ok");
        CHECK(c->input_used == 0 && c->queue_calls == 1 && memcmp(c->input, au, sizeof(au)) == 0,
              "input au queued verbatim");
        CHECK(c->released == 1, "output buffer released");
        CHECK(d.decoded == 1 && d.output == 1 && d.output_token == 7 && d.width == w && d.height == h,
              "diag reports decoded frame");
        int ok = 1;
        for (uint32_t r = 0; r < h && ok; r++)
            for (uint32_t x = 0; x < w; x++)
                if (y[r * 80 + x] != (uint8_t)(0x20 + r * 16 + x / 16)) ok = 0;
        for (uint32_t r = 0; r < h / 2 && ok; r++)
            for (uint32_t x = 0; x < w / 2; x++)
                if (u[r * 40 + x] != (uint8_t)(0x80 + r * 4 + x) || v[r * 40 + x] != (uint8_t)(0x40 + r * 4 + x)) ok = 0;
        CHECK(ok, "nv12 deinterleaved into u/v planes");
        CHECK(s2a_mc_destroy(m, &d) == 0 && c->stopped && c->deleted, "destroy stops+deletes");
        free(frame);
    }

    /* ⑤ I420 平面输出 → 逐行拷贝 */
    fake_reset(); s2a_mc_test_set_hw(1);
    {
        uint32_t w = 32, h = 32, stride = 40, slice_h = 40;
        uint64_t fsz;
        uint8_t *frame = make_i420(w, h, stride, slice_h, &fsz);
        cfg.width = w; cfg.height = h;
        d = diag();
        CHECK(s2a_mc_create(&cfg, &m, &d) == 0, "planar create");
        AMediaCodec *c = g_last;
        struct out_event *e = &c->ev[c->nev++];
        memset(e, 0, sizeof(*e));
        e->kind = 0; e->data = frame; e->size = (uint32_t)fsz;
        e->color = 0x13; e->stride = (int32_t)stride; e->slice_h = (int32_t)slice_h;
        e->cr = (int32_t)w - 1; e->cb = (int32_t)h - 1;
        static uint8_t y[32 * 32], u[16 * 16], v[16 * 16];
        s2a_mc_plane dst[3];
        planes(y, u, v, 32, w, h, dst);
        uint8_t au[3] = {1, 2, 3};
        CHECK(s2a_mc_decode(m, au, sizeof(au), w, h, dst, 8, fresh()) == 0, "planar decode ok");
        int ok = y[5 * 32 + 7] == (uint8_t)(0x10 + 5 + 7) && u[3 * 16 + 9] == (uint8_t)(0x90 + 3 + 9) &&
                 v[3 * 16 + 9] == (uint8_t)(0x60 + 3 + 9);
        CHECK(ok, "i420 planes copied");
        s2a_mc_destroy(m, &d);
        free(frame);
    }

    /* ⑥ crop 区域 ≠ 全幅：只拷贝 crop 内可见区 */
    fake_reset(); s2a_mc_test_set_hw(1);
    {
        uint32_t w = 32, h = 16, stride = 48, slice_h = 24;
        uint64_t fsz;
        uint8_t *frame = make_nv12(stride, slice_h, stride, slice_h, &fsz, 0);
        cfg.width = w; cfg.height = h;
        d = diag();
        CHECK(s2a_mc_create(&cfg, &m, &d) == 0, "crop create");
        AMediaCodec *c = g_last;
        struct out_event *e = &c->ev[c->nev++];
        memset(e, 0, sizeof(*e));
        e->kind = 0; e->data = frame; e->size = (uint32_t)fsz;
        e->color = 0x15; e->stride = (int32_t)stride; e->slice_h = (int32_t)slice_h;
        e->cl = 4; e->ct = 2; e->cr = (int32_t)(4 + w - 1); e->cb = (int32_t)(2 + h - 1);
        static uint8_t y[32 * 16], u[16 * 8], v[16 * 8];
        memset(y, 0, sizeof(y)); memset(u, 0, sizeof(u)); memset(v, 0, sizeof(v));
        s2a_mc_plane dst[3];
        planes(y, u, v, 32, w, h, dst);
        uint8_t au[2] = {9, 9};
        CHECK(s2a_mc_decode(m, au, sizeof(au), w, h, dst, 9, fresh()) == 0, "crop decode ok");
        /* 源 Y(ct=2,cl=4) → dst[0][0]；源 UV 从 (stride*slice_h + (ct/2)*stride + (cl/2)*2) 开始 */
        uint8_t *sy = frame + 2 * stride + 4;
        uint8_t *suv = frame + (uint64_t)stride * slice_h + 1 * stride + 4;
        int ok = 1;
        for (uint32_t r = 0; r < h && ok; r++)
            for (uint32_t x = 0; x < w; x++)
                if (y[r * 32 + x] != sy[r * stride + x]) ok = 0;
        for (uint32_t r = 0; r < h / 2 && ok; r++)
            for (uint32_t x = 0; x < w / 2; x++)
                if (u[r * 16 + x] != suv[r * stride + 2 * x] || v[r * 16 + x] != suv[r * stride + 2 * x + 1]) ok = 0;
        CHECK(ok, "crop region copied");
        s2a_mc_destroy(m, &d);
        free(frame);
    }

    /* ⑦ 输出尺寸变化 → RECONFIG，不判死 */
    fake_reset(); s2a_mc_test_set_hw(1);
    {
        cfg.width = 64; cfg.height = 64;
        d = diag();
        CHECK(s2a_mc_create(&cfg, &m, &d) == 0, "reconfig create");
        s2a_mc_plane dst[3];
        static uint8_t y[64 * 64], u[32 * 32], v[32 * 32];
        planes(y, u, v, 64, 64, 64, dst);
        uint8_t au[2] = {1, 1};
        CHECK(s2a_mc_decode(m, au, sizeof(au), 32, 32, dst, 1, fresh()) == S2A_MC_RECONFIG,
              "size change → RECONFIG");
        CHECK(g_last->queue_calls == 0, "reconfig happens before queueing");
        /* 同尺寸再来：脚本输出跟上即可正常解 */
        uint64_t fsz;
        uint8_t *frame = make_nv12(64, 64, 64, 64, &fsz, 0);
        push_nv12(g_last, frame, (uint32_t)fsz, 64, 64, 64, 64);
        CHECK(s2a_mc_decode(m, au, sizeof(au), 64, 64, dst, 2, fresh()) == 0, "same size still decodes");
        s2a_mc_destroy(m, &d);
        free(frame);
    }

    /* ⑧ 死 codec 占着输入缓冲不还：输入槽耗尽 → LIMIT@INPUT 粘性 */
    fake_reset(); s2a_mc_test_set_hw(1);
    {
        cfg.width = 64; cfg.height = 64;
        d = diag();
        CHECK(s2a_mc_create(&cfg, &m, &d) == 0, "stall create");
        g_last->consume_inputs = 0;   /* 死 codec：占着输入缓冲不消化 */
        s2a_mc_plane dst[3];
        static uint8_t y[64 * 64], u[32 * 32], v[32 * 32];
        planes(y, u, v, 64, 64, 64, dst);
        uint8_t au[4] = {1, 2, 3, 4};
        s2a_mc_diag pd = diag();
        for (int i = 0; i < 4; i++) {
            pd = diag();
            CHECK(s2a_mc_decode(m, au, sizeof(au), 64, 64, dst, (uint32_t)i, &pd) == 0 &&
                  pd.output == 0, "pending input: accepted, no output");
        }
        CHECK(g_last->queue_calls == 4, "pending inputs all queued");
        pd = diag();
        CHECK(s2a_mc_decode(m, au, sizeof(au), 64, 64, dst, 4, &pd) == S2A_MC_LIMIT,
              "input slots exhausted → LIMIT");
        CHECK(pd.stage == S2A_MC_STAGE_INPUT, "limit at input stage");
        CHECK(s2a_mc_decode(m, au, sizeof(au), 64, 64, dst, 5, fresh()) == S2A_MC_STATE,
              "failed context stays failed");
        s2a_mc_destroy(m, &d);
    }

    /* ⑨ 未知色彩格式 → 契约失败且粘性 */
    fake_reset(); s2a_mc_test_set_hw(1);
    {
        cfg.width = 32; cfg.height = 32;
        d = diag();
        CHECK(s2a_mc_create(&cfg, &m, &d) == 0, "color create");
        AMediaCodec *c = g_last;
        static uint8_t junk[64 * 1024];
        memset(junk, 0x77, sizeof(junk));
        struct out_event *e = &c->ev[c->nev++];
        memset(e, 0, sizeof(*e));
        e->kind = 0; e->data = junk; e->size = sizeof(junk);
        e->color = 0xdead; e->stride = 32; e->slice_h = 32; e->cr = 31; e->cb = 31;
        s2a_mc_plane dst[3];
        static uint8_t y[32 * 32], u[16 * 16], v[16 * 16];
        planes(y, u, v, 32, 32, 32, dst);
        uint8_t au[2] = {5, 5};
        CHECK(s2a_mc_decode(m, au, sizeof(au), 32, 32, dst, 5, fresh()) == S2A_MC_UNSUPPORTED,
              "unknown color → unsupported");
        CHECK(c->released == 1, "bad buffer still released");
        s2a_mc_destroy(m, &d);
    }

    /* ⑩ EOS 输出 → STATE，不写出图 */
    fake_reset(); s2a_mc_test_set_hw(1);
    {
        cfg.width = 32; cfg.height = 32;
        d = diag();
        CHECK(s2a_mc_create(&cfg, &m, &d) == 0, "eos create");
        AMediaCodec *c = g_last;
        struct out_event *e = &c->ev[c->nev++];
        memset(e, 0, sizeof(*e));
        e->kind = 0; e->flags = AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
        s2a_mc_plane dst[3];
        static uint8_t y[32 * 32], u[16 * 16], v[16 * 16];
        planes(y, u, v, 32, 32, 32, dst);
        uint8_t au[2] = {6, 6};
        CHECK(s2a_mc_decode(m, au, sizeof(au), 32, 32, dst, 6, fresh()) == S2A_MC_STATE,
              "eos → state failure");
        s2a_mc_destroy(m, &d);
    }

    /* ⑫ QCOM 半平面 0x7FA30C00 是 V 在前（YVU/NV21）：U/V 不得对调 */
    fake_reset(); s2a_mc_test_set_hw(1);
    {
        uint32_t w = 64, h = 32, stride = 80, slice_h = 32;
        uint64_t fsz;
        uint8_t *frame = make_yvu(w, h, stride, slice_h, &fsz);
        cfg.width = w; cfg.height = h;
        d = diag();
        CHECK(s2a_mc_create(&cfg, &m, &d) == 0, "yvu create");
        AMediaCodec *c = g_last;
        struct out_event *e = &c->ev[c->nev++];
        memset(e, 0, sizeof(*e));
        e->kind = 0; e->data = frame; e->size = (uint32_t)fsz;
        e->color = 0x7FA30C00; e->stride = (int32_t)stride; e->slice_h = (int32_t)slice_h;
        e->cr = (int32_t)w - 1; e->cb = (int32_t)h - 1;
        static uint8_t y[80 * 32], u[40 * 16], v[40 * 16];
        memset(u, 0xee, sizeof(u)); memset(v, 0xee, sizeof(v));
        s2a_mc_plane dst[3];
        planes(y, u, v, 80, w, h, dst);
        uint8_t au[3] = {7, 7, 7};
        CHECK(s2a_mc_decode(m, au, sizeof(au), w, h, dst, 11, fresh()) == 0, "yvu decode ok");
        int ok = 1;
        for (uint32_t r = 0; r < h / 2 && ok; r++)
            for (uint32_t x = 0; x < w / 2; x++)
                if (u[r * 40 + x] != (uint8_t)(0x80 + r * 4 + x) ||
                    v[r * 40 + x] != (uint8_t)(0x40 + r * 4 + x)) ok = 0;
        CHECK(ok, "yvu deinterleave: u/v not swapped");
        s2a_mc_destroy(m, &d);
        free(frame);
    }

    /* ⑬ 管线延迟：首个输入收下、本次无输出（rc=0, output=0），下一调用取到排队的帧 */
    fake_reset(); s2a_mc_test_set_hw(1);
    {
        uint32_t w = 64, h = 32, stride = 80, slice_h = 32;
        uint64_t fsz;
        uint8_t *frame = make_nv12(w, h, stride, slice_h, &fsz, 0);
        cfg.width = w; cfg.height = h;
        d = diag();
        CHECK(s2a_mc_create(&cfg, &m, &d) == 0, "latency create");
        AMediaCodec *c = g_last;
        /* 12 个 TRY_AGAIN 事件排在一个出图前面：输出队列横跨多次 decode 调用 */
        for (int i = 0; i < 12; i++) {
            struct out_event *e = &c->ev[c->nev++];
            memset(e, 0, sizeof(*e));
            e->kind = AMEDIACODEC_INFO_TRY_AGAIN_LATER;
        }
        push_nv12(c, frame, (uint32_t)fsz, stride, slice_h, w, h);
        s2a_mc_plane dst[3];
        static uint8_t y[80 * 32], u[40 * 16], v[40 * 16];
        planes(y, u, v, 80, w, h, dst);
        uint8_t au[3] = {1, 1, 1};
        s2a_mc_diag pd = diag();
        CHECK(s2a_mc_decode(m, au, sizeof(au), w, h, dst, 1, &pd) == 0 && pd.output == 0,
              "call 1: input accepted, output pending");
        pd = diag();
        CHECK(s2a_mc_decode(m, au, sizeof(au), w, h, dst, 2, &pd) == 0 && pd.output == 1,
              "call 2: earlier frame delivered");
        CHECK(g_last->queue_calls == 2 && y[0] != 0, "second call queued + surface written");
        s2a_mc_destroy(m, &d);
        free(frame);
    }

    /* ⑭ FLEXIBLE(0x7F420888)：stride/slice-height 缺键不接受；键齐全按半平面处理 */
    fake_reset(); s2a_mc_test_set_hw(1);
    {
        uint32_t w = 64, h = 32, stride = 80, slice_h = 32;
        uint64_t fsz;
        uint8_t *frame = make_nv12(w, h, stride, slice_h, &fsz, 0);
        cfg.width = w; cfg.height = h;
        d = diag();
        CHECK(s2a_mc_create(&cfg, &m, &d) == 0, "flex create");
        AMediaCodec *c = g_last;
        struct out_event *e = &c->ev[c->nev++];
        memset(e, 0, sizeof(*e));
        e->kind = 0; e->data = frame; e->size = (uint32_t)fsz;
        e->color = 0x7F420888; e->fmask = 1 | 8;   /* 有 color/crop，无 stride/slice-height */
        e->cr = (int32_t)w - 1; e->cb = (int32_t)h - 1;
        s2a_mc_plane dst[3];
        static uint8_t y[80 * 32], u[40 * 16], v[40 * 16];
        planes(y, u, v, 80, w, h, dst);
        uint8_t au[3] = {2, 2, 2};
        s2a_mc_diag fd = diag();
        CHECK(s2a_mc_decode(m, au, sizeof(au), w, h, dst, 12, &fd) == S2A_MC_UNSUPPORTED,
              "flexible w/o stride+slice → unsupported");
        s2a_mc_destroy(m, &d);
        /* 键齐全：同一格式按半平面正常解 */
        fake_reset(); s2a_mc_test_set_hw(1);
        CHECK(s2a_mc_create(&cfg, &m, &d) == 0, "flex create 2");
        c = g_last;
        e = &c->ev[c->nev++];
        memset(e, 0, sizeof(*e));
        e->kind = 0; e->data = frame; e->size = (uint32_t)fsz;
        e->color = 0x7F420888; e->fmask = 0xf;
        e->stride = (int32_t)stride; e->slice_h = (int32_t)slice_h;
        e->cr = (int32_t)w - 1; e->cb = (int32_t)h - 1;
        CHECK(s2a_mc_decode(m, au, sizeof(au), w, h, dst, 13, fresh()) == 0,
              "flexible w/ keys → semiplanar decode");
        CHECK(u[0] == 0x80 && v[0] == 0x40, "flexible deinterleaved");
        s2a_mc_destroy(m, &d);
        free(frame);
    }

    /* ⑮ 输入槽给足也救不了死 codec：连续零输出上界（32）→ LIMIT 粘性 */
    fake_reset(); s2a_mc_test_set_hw(1);
    {
        cfg.width = 64; cfg.height = 64;
        d = diag();
        CHECK(s2a_mc_create(&cfg, &m, &d) == 0, "inflight create");
        g_last->input_slots = 40;
        s2a_mc_plane dst[3];
        static uint8_t y[64 * 64], u[32 * 32], v[32 * 32];
        planes(y, u, v, 64, 64, 64, dst);
        uint8_t au[2] = {3, 3};
        int first_fail = -1, fail_stage = -1;
        for (int i = 0; i < 34; i++) {
            s2a_mc_diag pd = diag();
            int32_t rc = s2a_mc_decode(m, au, sizeof(au), 64, 64, dst, (uint32_t)i, &pd);
            if (first_fail < 0 && rc == S2A_MC_LIMIT) { first_fail = i; fail_stage = pd.stage; break; }
            if (first_fail < 0) CHECK(rc == 0 && pd.output == 0, "in-flight pending ok");
        }
        CHECK(first_fail == 32, "empty-streak LIMIT at 33rd silent call");
        CHECK(fail_stage == S2A_MC_STAGE_DEQUEUE, "empty-streak LIMIT at dequeue stage");
        s2a_mc_destroy(m, &d);
    }

    /* ⑯ 零星隐藏帧不累计误判：每第 8 个 AU 不出图（show_frame=0 模型），
     * 300 个 AU 全程无失败、非隐藏帧照常出图——「已入队未出图」计数会在
     * 第 33 个隐藏帧（AU 264）把活 codec 误判 LIMIT。 */
    fake_reset(); s2a_mc_test_set_hw(1);
    {
        uint32_t w = 64, h = 32, stride = 80, slice_h = 32;
        uint64_t fsz;
        uint8_t *frame = make_nv12(w, h, stride, slice_h, &fsz, 0);
        cfg.width = w; cfg.height = h;
        d = diag();
        CHECK(s2a_mc_create(&cfg, &m, &d) == 0, "sparse-hidden create");
        AMediaCodec *c = g_last;
        c->pattern_mod = 8;
        c->pattern_frame = frame; c->pattern_size = (uint32_t)fsz;
        c->pattern_w = (int32_t)w; c->pattern_h = (int32_t)h;
        c->pattern_stride = (int32_t)stride; c->pattern_slice = (int32_t)slice_h;
        s2a_mc_plane dst[3];
        static uint8_t y[80 * 32], u[40 * 16], v[40 * 16];
        planes(y, u, v, 80, w, h, dst);
        uint8_t au[4] = {4, 4, 4, 4};
        int fails = 0, produced = 0;
        for (int i = 0; i < 300; i++) {
            s2a_mc_diag pd = diag();
            int32_t rc = s2a_mc_decode(m, au, sizeof(au), w, h, dst, (uint32_t)i, &pd);
            if (rc != 0) fails++;
            else if (pd.output) produced++;
        }
        CHECK(fails == 0, "sparse hidden frames: no failure across 300 AUs");
        CHECK(produced == 263, "sparse hidden: 300-37 frames produced");
        CHECK(g_last->queue_calls == 300, "all AUs queued");
        s2a_mc_destroy(m, &d);
        free(frame);
    }

    /* ⑰ wait：不阻塞轮询拿不到→出图拿到；超帧第二帧由 wait(0) 续取 */
    fake_reset(); s2a_mc_test_set_hw(1);
    {
        cfg.width = 64; cfg.height = 64;
        d = diag();
        CHECK(s2a_mc_create(&cfg, &m, &d) == 0, "wait create");
        s2a_mc_plane dst[3];
        static uint8_t y[64 * 64], u[32 * 32], v[32 * 32];
        planes(y, u, v, 64, 64, 64, dst);
        uint8_t au[4] = {7, 7, 7, 7};
        s2a_mc_diag wd = diag();
        CHECK(s2a_mc_wait(m, dst, 0, &wd) == 0 && wd.output == 0, "empty wait(0) polls nothing");
        /* 一个 AU 排两帧（超帧）：decode 取第一帧，wait(0) 取第二帧 */
        uint64_t fsz;
        uint8_t *frame = make_nv12(64, 64, 64, 64, &fsz, 0);
        uint8_t *frame2 = make_nv12(64, 64, 64, 64, &fsz, 0);
        frame2[0] = 0x55;
        push_nv12(g_last, frame, (uint32_t)fsz, 64, 64, 64, 64);
        push_nv12(g_last, frame2, (uint32_t)fsz, 64, 64, 64, 64);
        CHECK(s2a_mc_decode(m, au, sizeof(au), 64, 64, dst, 20, fresh()) == 0, "superframe decode");
        CHECK(y[0] == 0x20 && g_last->released == 1, "first frame drained");
        wd = diag();
        CHECK(s2a_mc_wait(m, dst, 0, &wd) == 0 && wd.output == 1, "wait(0) picks up queued frame");
        CHECK(y[0] == 0x55, "second frame payload is the latest");
        wd = diag();
        CHECK(s2a_mc_wait(m, dst, 0, &wd) == 0 && wd.output == 0, "queue dry after both");
        s2a_mc_destroy(m, &d);
        free(frame); free(frame2);
    }

    /* ⑱ provenance / ABI 守卫 */
    CHECK(strcmp(s2a_mc_provenance(), S2A_MC_PROVENANCE) == 0, "provenance marker");
    s2a_mc_config bad = {0, sizeof(bad), S2A_MC_CODEC_VP9, 64, 64, 0};
    CHECK(s2a_mc_create(&bad, &m, fresh()) == S2A_MC_ARGUMENT, "bad abi rejected");
    CHECK(s2a_mc_probe(S2A_MC_ABI, 999, 64, 64, fresh()) == S2A_MC_UNSUPPORTED,
          "unknown codec rejected");

    if (g_failures) { printf("mc_host_test: %d failures\n", g_failures); return 1; }
    printf("mc_host_test: ok (prop gate, nv12 deinterleave, planar copy, crop, reconfig, pending output, qcom yvu, flexible gate, input-starve bound, empty-streak bound, sparse hidden frames, eos, sticky fail)\n");
    return 0;
}
