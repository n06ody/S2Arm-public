/* 宿主桩：NDK 29 media/NdkMediaFormat.h 的签名单元（供 mc_host_test 编译 s2a_mc.c）。 */
#ifndef S2A_HOSTMC_NDK_MEDIA_FORMAT_H
#define S2A_HOSTMC_NDK_MEDIA_FORMAT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "NdkMediaError.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AMediaFormat AMediaFormat;
extern const char* AMEDIAFORMAT_KEY_MIME;
extern const char* AMEDIAFORMAT_KEY_WIDTH;
extern const char* AMEDIAFORMAT_KEY_HEIGHT;
extern const char* AMEDIAFORMAT_KEY_COLOR_FORMAT;
extern const char* AMEDIAFORMAT_KEY_SLICE_HEIGHT;
extern const char* AMEDIAFORMAT_KEY_STRIDE;
AMediaFormat* AMediaFormat_new(void);
media_status_t AMediaFormat_delete(AMediaFormat*);
bool AMediaFormat_getInt32(AMediaFormat*, const char *name, int32_t *out);
void AMediaFormat_setInt32(AMediaFormat*, const char* name, int32_t value);
void AMediaFormat_setString(AMediaFormat*, const char* name, const char* value);
#ifdef __cplusplus
}
#endif
#endif
