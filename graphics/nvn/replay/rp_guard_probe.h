#ifndef S2A_RP_GUARD_PROBE_H
#define S2A_RP_GUARD_PROBE_H

#include "nvn_backend.h"

enum rp_guard_upload { RP_GUARD_UPLOAD_UNKNOWN, RP_GUARD_UPLOAD_DIRECT, RP_GUARD_UPLOAD_ORDERED, RP_GUARD_UPLOAD_MIXED };
enum rp_guard_version { RP_GUARD_VERSION_UNKNOWN, RP_GUARD_VERSION_SINGLE, RP_GUARD_VERSION_MULTIPLE };
enum rp_guard_zero { RP_GUARD_ZERO_UNKNOWN, RP_GUARD_ALL_ZERO, RP_GUARD_NONZERO };
struct rp_guard_observation {
    uint8_t range, prefix, upload, version, pending, landing, alias, gpu_writer, cpu_writer, zero;
};

void rp_buf_guard_probe(nvn_gpu_buf buf, uint64_t off, uint64_t len,
                        uint32_t field, uint32_t stride, uint32_t count, int prefix_known,
                        struct rp_guard_observation *out);

int rp_pool_guard_alias(const void *owner, uint64_t start, uint64_t end);

#endif
