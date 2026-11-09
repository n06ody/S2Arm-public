/* 宿主桩：NDK 29 media/NdkMediaError.h 的签名单元（供 mc_host_test 编译 s2a_mc.c）。 */
#ifndef S2A_HOSTMC_NDK_MEDIA_ERROR_H
#define S2A_HOSTMC_NDK_MEDIA_ERROR_H
#ifdef __cplusplus
extern "C" {
#endif
typedef enum media_status_t {
    AMEDIA_OK = 0,
    AMEDIA_ERROR_UNKNOWN = -10000,
    AMEDIA_ERROR_MALFORMED = -10001,
    AMEDIA_ERROR_UNSUPPORTED = -10002,
    AMEDIA_ERROR_INVALID_OBJECT = -10003,
    AMEDIA_ERROR_INVALID_PARAMETER = -10004,
    AMEDIA_ERROR_INVALID_OPERATION = -10005,
    AMEDIA_ERROR_END_OF_STREAM = -10006,
    AMEDIA_ERROR_IO = -10007,
    AMEDIA_ERROR_WOULD_BLOCK = -10008,
    AMEDIA_DRM_ERROR_BASE = -20000
} media_status_t;
#ifdef __cplusplus
}
#endif
#endif
