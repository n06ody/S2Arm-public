// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>

enum nvn_probe_result {
    NVN_PROBE_GUEST_READ = 1, NVN_PROBE_HOST_READ, NVN_PROBE_WRITE_FIRST,
    NVN_PROBE_NEXT_VERSION, NVN_PROBE_EXPIRED, NVN_PROBE_ALIAS, NVN_PROBE_NEIGHBOR, NVN_PROBE_UNAVAILABLE
};
struct nvn_probe_source { uint64_t submission; unsigned ops, stages, slots; };
struct nvn_probe_sample {
    struct nvn_probe_source source;
    uint64_t pool, write, offset, bytes, page_index, pages;
    unsigned armed, result;
};
typedef void (*nvn_probe_emit)(const struct nvn_probe_sample *);
void nvn_probe_before(const void *mem, size_t len, uint64_t pool, uint64_t write);
void nvn_probe_after(const void *mem, size_t len, uint64_t offset, uint64_t pool, uint64_t write, int overlap, struct nvn_probe_source source);
uint64_t nvn_probe_drain(nvn_probe_emit emit); // 返回本窗因槽满跳过的版本数
