/* 宿主桩：NDK 29 media/NdkMediaCodec.h 的签名单元（供 mc_host_test 编译 s2a_mc.c）。
 * 函数签名与 NDK 29 实头逐一对齐；宿主测试提供同名假实现。 */
#ifndef S2A_HOSTMC_NDK_MEDIA_CODEC_H
#define S2A_HOSTMC_NDK_MEDIA_CODEC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "NdkMediaError.h"
#include "NdkMediaFormat.h"
#ifdef __cplusplus
extern "C" {
#endif
struct AMediaCodecBufferInfo {
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
};
typedef struct AMediaCodecBufferInfo AMediaCodecBufferInfo;
enum {
    AMEDIACODEC_BUFFER_FLAG_KEY_FRAME = 1,
    AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG = 2,
    AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM = 4,
    AMEDIACODEC_BUFFER_FLAG_PARTIAL_FRAME = 8,
    AMEDIACODEC_BUFFER_FLAG_MUXER_DATA = 16,
    AMEDIACODEC_BUFFER_FLAG_DECODE_ONLY = 32,
    AMEDIACODEC_INFO_TRY_AGAIN_LATER = -1,
    AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED = -2,
    AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED = -3
};
typedef struct AMediaCodec AMediaCodec;
typedef struct ANativeWindow ANativeWindow;
typedef struct AMediaCrypto AMediaCrypto;
AMediaCodec* AMediaCodec_createDecoderByType(const char *mime_type);
media_status_t AMediaCodec_delete(AMediaCodec*);
media_status_t AMediaCodec_configure(AMediaCodec*, const AMediaFormat* format,
                                     ANativeWindow* surface, AMediaCrypto *crypto, uint32_t flags);
media_status_t AMediaCodec_start(AMediaCodec*);
media_status_t AMediaCodec_stop(AMediaCodec*);
media_status_t AMediaCodec_flush(AMediaCodec*);
uint8_t* AMediaCodec_getInputBuffer(AMediaCodec*, size_t idx, size_t *out_size);
uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec*, size_t idx, size_t *out_size);
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec*, int64_t timeoutUs);
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec*, size_t idx,
                                            off_t offset, size_t size,
                                            uint64_t time, uint32_t flags);
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec*, AMediaCodecBufferInfo *info,
                                        int64_t timeoutUs);
AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec*);
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec*, size_t idx, bool render);
#ifdef __cplusplus
}
#endif
#endif
