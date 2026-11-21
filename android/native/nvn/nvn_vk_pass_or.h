#ifndef S2A_NVNVK_PASS_OR_H
#define S2A_NVNVK_PASS_OR_H

#include "nvn_backend.h"

struct nvn_vk_pass_or_domain { uint32_t count, stride, field0, field1; };
struct nvn_vk_pass_or_value { uint32_t field0, field1, valid; };

int nvn_vk_pass_or_record(nvn_gpu_cmd cmd, nvn_gpu_buf src, uint64_t off, uint64_t range,
                          nvn_gpu_buf dst, uint64_t dst_off, struct nvn_vk_pass_or_domain domain);

#endif
