/* AMediaCodec 解码边界（#44 第二刀）。只做 ByteBuffer 模式、只吃 VP9。
 * 宿主单测用 tests/hostmc 的同名桩头编译——函数名/签名以 NDK 29 实头为准。 */
#include "s2a_mc.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaError.h>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#define S2A_MC_IN_TRIES 20u
#define S2A_MC_IN_US 10000
#define S2A_MC_OUT_TRIES 10u
#define S2A_MC_OUT_US 10000
/* 硬解器有管线延迟：收下输入几帧后才出首帧；show_frame=0 的隐藏帧天然
 * 不出图——「已入队未出图」计数会把活 codec 误判死，所以盯的是「连续零
 * 输出调用数」：任何一次出图清零，连续超过这个上界才当死 codec 判 LIMIT。 */
#define S2A_MC_MAX_EMPTY 32u

/* CodecCapabilities 的 COLOR_Format* 是 Java 侧常量，NDK 头不导出；
 * 这里钉 ByteBuffer 模式实际会遇到的值。
 * 0x7FA30C00 按 AOSP OMX_IVCommon.h 是 OMX_QCOM_COLOR_FormatYVU420SemiPlanar：
 * V 在偶数字节（NV21 排布），与 NV12 的 U 在前相反，解交织时目的平面对调。 */
enum {
    MC_COLOR_YUV420_PLANAR = 0x13,
    MC_COLOR_YUV420_PACKED_PLANAR = 0x14,
    MC_COLOR_YUV420_SEMIPLANAR = 0x15,
    MC_COLOR_YUV420_FLEXIBLE = 0x7F420888,
    MC_COLOR_QCOM_SEMIPLANAR = 0x7FA30C00
};

struct s2a_mc {
    AMediaCodec *codec;
    uint32_t codec_kind, width, height;
    uint32_t generation, empty_streak;
    int failed;
};

static int32_t mc_convert(s2a_mc *m, const uint8_t *buf, uint32_t size,
                          const s2a_mc_plane *dst, s2a_mc_diag *diag);

#if !defined(__ANDROID__)
int s2a_mc_test_hw;
#endif

static void diag_set(s2a_mc_diag *d, uint32_t stage, int32_t code, int32_t status)
{
    if (d) { d->stage = stage; d->code = code; d->media_status = status; }
}

static int mc_video_hw(void)
{
#if defined(__ANDROID__)
    char v[8] = {0};
    __system_property_get("debug.s2a.video_hw", v);
    return v[0] == '1';
#else
    return s2a_mc_test_hw;
#endif
}

#if defined(S2A_MC_TESTING) && !defined(__ANDROID__)
void s2a_mc_test_set_hw(int v) { s2a_mc_test_hw = v; }
#endif

static int mc_diag_ok(const s2a_mc_diag *d)
{
    return d && d->abi_version == S2A_MC_ABI && d->struct_size >= (uint32_t)sizeof(*d);
}

static void mc_set_size(AMediaFormat *fmt, uint32_t width, uint32_t height)
{
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/x-vnd.on2.vp9");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, (int32_t)width);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, (int32_t)height);
}

static AMediaCodec *mc_open(uint32_t width, uint32_t height, s2a_mc_diag *diag)
{
    AMediaCodec *codec = AMediaCodec_createDecoderByType("video/x-vnd.on2.vp9");
    if (!codec) { diag_set(diag, S2A_MC_STAGE_CREATE, S2A_MC_UNSUPPORTED, 0); return NULL; }
    AMediaFormat *fmt = AMediaFormat_new();
    media_status_t rc;
    if (!fmt) {
        rc = AMEDIA_ERROR_UNKNOWN;
    } else {
        mc_set_size(fmt, width, height);
        rc = AMediaCodec_configure(codec, fmt, NULL, NULL, 0);
    }
    if (fmt) AMediaFormat_delete(fmt);
    if (rc != AMEDIA_OK) {
        diag_set(diag, S2A_MC_STAGE_FORMAT, S2A_MC_BACKEND, (int32_t)rc);
        AMediaCodec_delete(codec);
        return NULL;
    }
    rc = AMediaCodec_start(codec);
    if (rc != AMEDIA_OK) {
        diag_set(diag, S2A_MC_STAGE_START, S2A_MC_BACKEND, (int32_t)rc);
        AMediaCodec_delete(codec);
        return NULL;
    }
    return codec;
}

static int32_t mc_plane_ok(const s2a_mc_plane *p, uint32_t width, uint32_t height)
{
    return p && p->data && p->stride >= width &&
           p->capacity >= (uint64_t)p->stride * (height - 1) + width;
}

/* 源平面按 stride 逐行拷贝到 dst（dst stride 可与源不同）。 */
static void mc_copy_rows(const uint8_t *src, uint32_t src_stride, uint8_t *dst,
                         uint32_t dst_stride, uint32_t bytes_per_row, uint32_t rows)
{
    for (uint32_t r = 0; r < rows; r++) {
        memcpy(dst + (uint64_t)r * dst_stride, src + (uint64_t)r * src_stride, bytes_per_row);
    }
}

/* 交错 UV（NV12：U 在偶数字节）逐行解成两个独立半平面。 */
static void mc_split_uv(const uint8_t *src, uint32_t src_stride, uint8_t *du,
                        uint8_t *dv, uint32_t dst_stride, uint32_t pairs_per_row, uint32_t rows)
{
    for (uint32_t r = 0; r < rows; r++) {
        const uint8_t *s = src + (uint64_t)r * src_stride;
        uint8_t *u = du + (uint64_t)r * dst_stride;
        uint8_t *v = dv + (uint64_t)r * dst_stride;
        for (uint32_t x = 0; x < pairs_per_row; x++) {
            u[x] = s[2 * x];
            v[x] = s[2 * x + 1];
        }
    }
}

/* AMEDIAFORMAT_KEY_SLICE_HEIGHT 自 API 28 才有，minSdk 29 满足。 */
static int32_t mc_read_format(s2a_mc *m, s2a_mc_diag *diag,
                              int32_t *color, int32_t *stride, int32_t *slice_h,
                              int32_t *crop_l, int32_t *crop_r, int32_t *crop_t, int32_t *crop_b,
                              int *have_stride, int *have_slice)
{
    AMediaFormat *fmt = AMediaCodec_getOutputFormat(m->codec);
    if (!fmt) { diag_set(diag, S2A_MC_STAGE_DEQUEUE, S2A_MC_BACKEND, 0); return S2A_MC_BACKEND; }
    int32_t v = 0;
    *color = AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v) ? v : MC_COLOR_YUV420_SEMIPLANAR;
    *have_stride = AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_STRIDE, &v);
    *stride = *have_stride ? v : (int32_t)m->width;
    *have_slice = AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &v);
    *slice_h = *have_slice ? v : (int32_t)m->height;
    *crop_l = AMediaFormat_getInt32(fmt, "crop-left", &v) ? v : 0;
    *crop_r = AMediaFormat_getInt32(fmt, "crop-right", &v) ? v : (int32_t)m->width - 1;
    *crop_t = AMediaFormat_getInt32(fmt, "crop-top", &v) ? v : 0;
    *crop_b = AMediaFormat_getInt32(fmt, "crop-bottom", &v) ? v : (int32_t)m->height - 1;
    AMediaFormat_delete(fmt);
    return 0;
}

/* 有界排空输出：拿到一帧即返回（多余输出留在 codec 队列里给下次调用）。
 * 预算内没拿到不是错误——硬解管线延迟下「输入已收、帧未到」是可恢复态，
 * *produced 记 0 返回 0；真错误照常返回 <0。tries/us 由调用方定：
 * decode 走 S2A_MC_OUT_TRIES×S2A_MC_OUT_US，wait 按自带预算算。 */
static int32_t mc_drain(s2a_mc *m, const s2a_mc_plane *dst, s2a_mc_diag *diag,
                        int *produced, uint32_t tries, int64_t us)
{
    *produced = 0;
    for (uint32_t t = 0; t < tries; t++) {
        AMediaCodecBufferInfo info;
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(m->codec, &info, us);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) continue;
        if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED ||
            idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) continue;
        if (idx < 0) { diag_set(diag, S2A_MC_STAGE_DEQUEUE, S2A_MC_BACKEND, (int32_t)idx); return S2A_MC_BACKEND; }
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
            AMediaCodec_releaseOutputBuffer(m->codec, (size_t)idx, false);
            diag_set(diag, S2A_MC_STAGE_DEQUEUE, S2A_MC_STATE, 0);
            return S2A_MC_STATE;
        }
        size_t cap = 0;
        uint8_t *buf = AMediaCodec_getOutputBuffer(m->codec, (size_t)idx, &cap);
        int32_t rc = 0;
        if (!buf || info.offset < 0 || info.size < 0 ||
            (uint64_t)info.offset + (uint64_t)info.size > cap) {
            rc = S2A_MC_CONTRACT;
            diag_set(diag, S2A_MC_STAGE_OUTPUT, rc, 0);
        } else {
            rc = mc_convert(m, buf + info.offset, (uint32_t)info.size, dst, diag);
        }
        AMediaCodec_releaseOutputBuffer(m->codec, (size_t)idx, false);
        if (rc) return rc;
        m->empty_streak = 0;
        *produced = 1;
        return 0;
    }
    return 0;
}

static int32_t mc_convert(s2a_mc *m, const uint8_t *buf, uint32_t size,
                          const s2a_mc_plane *dst, s2a_mc_diag *diag)
{
    int32_t color, stride, slice_h, cl, cr, ct, cb;
    int have_stride, have_slice;
    int32_t rc = mc_read_format(m, diag, &color, &stride, &slice_h, &cl, &cr, &ct, &cb,
                                &have_stride, &have_slice);
    if (rc) return rc;
    /* 失败 diag 带出厂商实际报的色彩/几何，真机对照各机型 MediaCodec 行为用。 */
    if (diag) {
        diag->color_format = color;
        diag->out_stride = (uint32_t)stride;
        diag->out_slice_h = (uint32_t)slice_h;
    }
    uint32_t w = (uint32_t)(cr - cl + 1), h = (uint32_t)(cb - ct + 1);
    if (cr < cl || cb < ct || w != m->width || h != m->height || stride < (int32_t)w ||
        slice_h < (int32_t)m->height) {
        diag_set(diag, S2A_MC_STAGE_OUTPUT, S2A_MC_CONTRACT, 0);
        return S2A_MC_CONTRACT;
    }
    uint32_t uw = (w + 1) / 2, uh = (h + 1) / 2;
    if (!mc_plane_ok(&dst[0], w, h) || !mc_plane_ok(&dst[1], uw, uh) || !mc_plane_ok(&dst[2], uw, uh)) {
        diag_set(diag, S2A_MC_STAGE_COPY, S2A_MC_CONTRACT, 0);
        return S2A_MC_CONTRACT;
    }
    /* 布局按 MediaCodec ByteBuffer 约定：Y = stride*slice_h，chroma 紧随其后。
     * crop-left/right/top/bottom 是 MediaCodec 内置字符串键（NDK 头不导出常量），
     * 输出格式里以 int 键出现；缺省=全幅。chroma 裁剪偏移按各自平面行宽算；
     * 奇数 crop 左/上偏移按偶对齐（cl/2、ct/2），丢半像素色度相位——VP9 输出
     * crop 常见偶数，不补相位。
     * FLEXIBLE(0x7F420888) 语义是「布局只能经 Image 平面+pixelStride 查」，
     * ByteBuffer 里不保证半平面：只在 codec 明确报出 stride 和 slice-height
     * 时才按半平面解，缺键宁可 UNSUPPORTED 也不猜布局。 */
    int semi = (color == MC_COLOR_YUV420_SEMIPLANAR || color == MC_COLOR_QCOM_SEMIPLANAR ||
                (color == MC_COLOR_YUV420_FLEXIBLE && have_stride && have_slice));
    int planar = (color == MC_COLOR_YUV420_PLANAR || color == MC_COLOR_YUV420_PACKED_PLANAR);
    uint64_t y_off = (uint64_t)ct * stride + cl;
    uint64_t c_off = semi ? (uint64_t)stride * slice_h + (uint64_t)(ct / 2) * stride + (uint64_t)(cl / 2) * 2
                          : (uint64_t)stride * slice_h + (uint64_t)(ct / 2) * (uint32_t)(stride / 2) + (uint64_t)(cl / 2);
    uint64_t need_planar = (uint64_t)stride * slice_h + 2 * ((uint64_t)(stride / 2) * (slice_h / 2));
    uint64_t need_semi = (uint64_t)stride * slice_h + (uint64_t)stride * (slice_h / 2);
    const uint8_t *yp = buf + y_off;
    const uint8_t *cp = buf + c_off;
    if (semi) {
        if ((uint64_t)size < need_semi) {
            diag_set(diag, S2A_MC_STAGE_OUTPUT, S2A_MC_CONTRACT, 0);
            return S2A_MC_CONTRACT;
        }
        mc_copy_rows(yp, (uint32_t)stride, dst[0].data, dst[0].stride, w, h);
        if (color == MC_COLOR_QCOM_SEMIPLANAR)
            mc_split_uv(cp, (uint32_t)stride, dst[2].data, dst[1].data, dst[1].stride, uw, uh);
        else
            mc_split_uv(cp, (uint32_t)stride, dst[1].data, dst[2].data, dst[1].stride, uw, uh);
        return 0;
    }
    if (planar) {
        if ((uint64_t)size < need_planar) {
            diag_set(diag, S2A_MC_STAGE_OUTPUT, S2A_MC_CONTRACT, 0);
            return S2A_MC_CONTRACT;
        }
        uint32_t cstride = (uint32_t)(stride / 2);
        const uint8_t *vp = cp + (uint64_t)cstride * (slice_h / 2);
        mc_copy_rows(yp, (uint32_t)stride, dst[0].data, dst[0].stride, w, h);
        mc_copy_rows(cp, cstride, dst[1].data, dst[1].stride, uw, uh);
        mc_copy_rows(vp, cstride, dst[2].data, dst[2].stride, uw, uh);
        return 0;
    }
    diag_set(diag, S2A_MC_STAGE_OUTPUT, S2A_MC_UNSUPPORTED, 0);
    return S2A_MC_UNSUPPORTED;
}

int32_t s2a_mc_probe(uint32_t abi, uint32_t codec, uint32_t width, uint32_t height, s2a_mc_diag *diag)
{
    if (!mc_diag_ok(diag) || abi != S2A_MC_ABI || !width || !height) return S2A_MC_ARGUMENT;
    if (codec != S2A_MC_CODEC_VP9 || !mc_video_hw()) {
        diag_set(diag, S2A_MC_STAGE_PROBE, S2A_MC_UNSUPPORTED, 0);
        return S2A_MC_UNSUPPORTED;
    }
    AMediaCodec *probe = AMediaCodec_createDecoderByType("video/x-vnd.on2.vp9");
    if (!probe) {
        diag_set(diag, S2A_MC_STAGE_PROBE, S2A_MC_UNSUPPORTED, 0);
        return S2A_MC_UNSUPPORTED;
    }
    AMediaCodec_delete(probe);
    return 0;
}

int32_t s2a_mc_create(const s2a_mc_config *cfg, s2a_mc **out, s2a_mc_diag *diag)
{
    if (!cfg || !out || !mc_diag_ok(diag) || cfg->abi_version != S2A_MC_ABI ||
        cfg->struct_size < (uint32_t)sizeof(*cfg) || !cfg->width || !cfg->height) return S2A_MC_ARGUMENT;
    *out = NULL;
    if (cfg->codec != S2A_MC_CODEC_VP9 || !mc_video_hw()) {
        diag_set(diag, S2A_MC_STAGE_CREATE, S2A_MC_UNSUPPORTED, 0);
        return S2A_MC_UNSUPPORTED;
    }
    s2a_mc *m = calloc(1, sizeof(*m));
    if (!m) return S2A_MC_NOMEM;
    m->codec = mc_open(cfg->width, cfg->height, diag);
    if (!m->codec) { free(m); return diag->code; }
    m->codec_kind = cfg->codec;
    m->width = cfg->width;
    m->height = cfg->height;
    *out = m;
    return 0;
}

int32_t s2a_mc_decode(s2a_mc *m, const uint8_t *au, uint32_t len,
                      uint32_t width, uint32_t height, const s2a_mc_plane *dst,
                      uint32_t token, s2a_mc_diag *diag)
{
    if (!m || !au || !len || !dst || !mc_diag_ok(diag)) return S2A_MC_ARGUMENT;
    if (m->failed) { diag_set(diag, S2A_MC_STAGE_INPUT, S2A_MC_STATE, 0); return S2A_MC_STATE; }
    diag->input_token = token;
    diag->generation = ++m->generation;
    if (width != m->width || height != m->height) {
        diag_set(diag, S2A_MC_STAGE_INPUT, S2A_MC_RECONFIG, 0);
        return S2A_MC_RECONFIG;
    }
    int32_t result = S2A_MC_BACKEND;
    ssize_t idx = -1;
    for (uint32_t tries = 0; tries < S2A_MC_IN_TRIES; tries++) {
        idx = AMediaCodec_dequeueInputBuffer(m->codec, S2A_MC_IN_US);
        if (idx >= 0) break;
        if (idx != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            result = S2A_MC_BACKEND;
            diag_set(diag, S2A_MC_STAGE_INPUT, result, (int32_t)idx);
            goto fail;
        }
    }
    if (idx < 0) {
        result = S2A_MC_LIMIT;
        diag_set(diag, S2A_MC_STAGE_INPUT, result, 0);
        goto fail;
    }
    {
        size_t cap = 0;
        uint8_t *in = AMediaCodec_getInputBuffer(m->codec, (size_t)idx, &cap);
        if (!in || len > cap) {
            result = S2A_MC_CONTRACT;
            diag_set(diag, S2A_MC_STAGE_INPUT, result, (int32_t)cap);
            goto fail;
        }
        memcpy(in, au, len);
        media_status_t qrc = AMediaCodec_queueInputBuffer(m->codec, (size_t)idx, 0, len, token, 0);
        if (qrc != AMEDIA_OK) {
            result = S2A_MC_BACKEND;
            diag_set(diag, S2A_MC_STAGE_INPUT, result, (int32_t)qrc);
            goto fail;
        }
    }
    diag->consumed = len;
    int produced = 0;
    result = mc_drain(m, dst, diag, &produced, S2A_MC_OUT_TRIES, S2A_MC_OUT_US);
    if (result) goto fail;
    if (!produced && ++m->empty_streak > S2A_MC_MAX_EMPTY) {
        result = S2A_MC_LIMIT;
        diag_set(diag, S2A_MC_STAGE_DEQUEUE, result, 0);
        goto fail;
    }
    diag->decoded = 1;
    diag->output = (uint32_t)produced;
    if (produced) diag->output_token = token;   /* token 记的是本次调用的输入，不是出图那帧的 */
    diag->width = m->width;
    diag->height = m->height;
    return 0;
fail:
    m->failed = 1;
    diag->width = m->width;
    diag->height = m->height;
    return result;
}

int32_t s2a_mc_wait(s2a_mc *m, const s2a_mc_plane *dst, uint32_t budget_us, s2a_mc_diag *diag)
{
    if (!m || !diag) return S2A_MC_ARGUMENT;
    memset(diag, 0, sizeof(*diag));
    diag->width = m->width;
    diag->height = m->height;
    if (m->failed) { diag_set(diag, S2A_MC_STAGE_INPUT, S2A_MC_STATE, 0); return S2A_MC_STATE; }
    /* budget_us=0 单次不阻塞轮询；否则按 OUT_US 切片有界等。不出图不是
     * 错误（output=0 返回 0），也不算连续零输出调用——那不是 decode。 */
    uint32_t tries = budget_us == 0 ? 1 : (budget_us + S2A_MC_OUT_US - 1) / S2A_MC_OUT_US;
    int64_t us = budget_us == 0 ? 0 : S2A_MC_OUT_US;
    int produced = 0;
    int32_t result = mc_drain(m, dst, diag, &produced, tries, us);
    if (result) { m->failed = 1; return result; }
    diag->output = (uint32_t)produced;
    return 0;
}

int32_t s2a_mc_destroy(s2a_mc *m, s2a_mc_diag *diag)
{
    (void)diag;
    if (!m) return S2A_MC_ARGUMENT;
    if (m->codec) {
        AMediaCodec_stop(m->codec);
        AMediaCodec_delete(m->codec);
        m->codec = NULL;
    }
    free(m);
    return 0;
}

const char *s2a_mc_provenance(void) { return S2A_MC_PROVENANCE; }
