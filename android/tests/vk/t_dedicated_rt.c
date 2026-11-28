// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define main feedback_main_unused
#include "t_feedback.c"
#undef main

struct allocation_case {
    VkImage image, dedicated_image;
    VkDeviceMemory memory;
    int active, prefers, requires, queried, allocations;
};
static struct allocation_case probe;
static _Atomic(VkDeviceMemory) expect_free;
static _Atomic int freed;
static int prefer_rt, deny_dedicated;

int __real_nvn_plat_flag(const char *, int);
int __wrap_nvn_plat_flag(const char *name, int fallback) {
    if (!strcmp(name, "nvk_dedicated_rt_alloc")) return prefer_rt;
    return __real_nvn_plat_flag(name, fallback);
}

extern __typeof__(vkCreateImage) __real_vkCreateImage;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateImage(VkDevice device, const VkImageCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImage *image) {
    VkResult result = __real_vkCreateImage(device, info, allocator, image);
    if (probe.active && result == VK_SUCCESS) {
        REQUIRE(!probe.image);
        probe.image = *image;
    }
    return result;
}

extern __typeof__(vkGetImageMemoryRequirements2) __real_vkGetImageMemoryRequirements2;
VKAPI_ATTR void VKAPI_CALL __wrap_vkGetImageMemoryRequirements2(VkDevice device,
    const VkImageMemoryRequirementsInfo2 *info, VkMemoryRequirements2 *requirements) {
    __real_vkGetImageMemoryRequirements2(device, info, requirements);
    if (!probe.active || info->image != probe.image) return;
    REQUIRE(requirements->sType == VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2);
    VkBaseOutStructure *next = requirements->pNext;
    while (next && next->sType != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) next = next->pNext;
    REQUIRE(next);
    VkMemoryDedicatedRequirements *dedicated = (VkMemoryDedicatedRequirements *)next;
    dedicated->requiresDedicatedAllocation = probe.requires;
    dedicated->prefersDedicatedAllocation = probe.prefers || probe.requires;
    probe.queried++;
}

extern __typeof__(vkAllocateMemory) __real_vkAllocateMemory;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
    if (probe.active) {
        const VkBaseInStructure *next = info->pNext;
        while (next && next->sType != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) next = next->pNext;
        probe.allocations++;
        if (next) {
            const VkMemoryDedicatedAllocateInfo *dedicated = (const VkMemoryDedicatedAllocateInfo *)next;
            REQUIRE(dedicated->buffer == VK_NULL_HANDLE);
            probe.dedicated_image = dedicated->image;
            if (deny_dedicated) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
    }
    return __real_vkAllocateMemory(device, info, allocator, memory);
}

extern __typeof__(vkBindImageMemory) __real_vkBindImageMemory;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkBindImageMemory(VkDevice device, VkImage image,
    VkDeviceMemory memory, VkDeviceSize offset) {
    if (probe.active && image == probe.image) {
        REQUIRE(!offset);
        probe.memory = memory;
    }
    return __real_vkBindImageMemory(device, image, memory, offset);
}

extern __typeof__(vkFreeMemory) __real_vkFreeMemory;
VKAPI_ATTR void VKAPI_CALL __wrap_vkFreeMemory(VkDevice device, VkDeviceMemory memory,
    const VkAllocationCallbacks *allocator) {
    if (atomic_load(&expect_free) == memory) atomic_store(&freed, 1);
    __real_vkFreeMemory(device, memory, allocator);
}

static nvn_gpu_tex make_image(unsigned format, unsigned usage, int prefers, int requires,
    int dedicated, const char *label) {
    nvn_gpu_tex_desc desc = {0};
    desc.d.width = desc.d.height = 4;
    desc.d.depth = desc.d.levels = 1;
    desc.d.target = NVN_TEXTURE_TARGET_2D;
    desc.d.format = format;
    desc.usage = usage;
    probe = (struct allocation_case){.active = 1, .prefers = prefers, .requires = requires};
    nvn_gpu_tex result = NULL;
    GPU(nvn_gpu_tex_create(&desc, &result, &err));
    probe.active = 0;
    printf("%s query=%d alloc=%d dedicated=%d expected=%d\n",
           label, probe.queried, probe.allocations, !!probe.dedicated_image, dedicated);
    REQUIRE(probe.image && probe.memory);
    REQUIRE(probe.dedicated_image == (dedicated ? probe.image : VK_NULL_HANDLE));
    if (prefers || requires) REQUIRE(probe.queried == 1);
    if (dedicated) REQUIRE(probe.allocations == 1);
    return result;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2);
    prefer_rt = atoi(argv[1]);
    struct s2a_vk_device device;
    char why[256];
    REQUIRE(!s2a_vk_device_create(&device, why, sizeof why));
    const nvn_gpu_open_desc open = {.host_device = &device, .app_label = "dedicated-rt"};
    GPU(nvn_gpu_open(&open, &err));
    const unsigned rt = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;

    nvn_gpu_tex plain = make_image(NVN_FORMAT_RGBA8, rt, 0, 0, 0, "color no preference");
    VkDeviceMemory recycled = probe.memory;
    const int plain_queries = probe.queried;
    nvn_gpu_tex_release(plain);

    nvn_gpu_tex color = make_image(NVN_FORMAT_RGBA8, rt, 1, 0, prefer_rt, "color preferred");
    REQUIRE((probe.memory != recycled) == prefer_rt);
    REQUIRE(probe.allocations == prefer_rt && plain_queries == 1);
    if (prefer_rt) {
        deny_dedicated = 1;
        nvn_gpu_tex fallback = make_image(NVN_FORMAT_RGBA8, rt, 1, 0, 1, "color preferred allocation denied");
        REQUIRE(probe.memory == recycled);
        nvn_gpu_tex_release(fallback);
        deny_dedicated = 0;
    }
    nvn_gpu_tex depth = make_image(NVN_FORMAT_DEPTH32F, rt, 1, 0, prefer_rt, "depth preferred");
    nvn_gpu_tex stencil = make_image(NVN_FORMAT_DEPTH24_STENCIL8, NVN_GPU_TEX_RENDER, 1, 0, prefer_rt,
                                      "depth-stencil preferred");
    nvn_gpu_tex_release(stencil);
    nvn_gpu_tex preferred_sample = make_image(NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED, 1, 0, 0, "sample preference");
    nvn_gpu_tex_release(preferred_sample);
    nvn_gpu_tex required_sample = make_image(NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED, 0, 1, 1, "sample required");
    expect_free = probe.memory;
    nvn_gpu_tex_release(required_sample);
    REQUIRE(atomic_load(&freed));
    expect_free = VK_NULL_HANDLE;

    nvn_gpu_view cv, dv;
    GPU(nvn_gpu_view_create(color, NULL, &cv, &err));
    GPU(nvn_gpu_view_create(depth, NULL, &dv, &err));
    nvn_gpu_buf buffer;
    GPU(nvn_gpu_buf_create(128, NVN_GPU_MEM_SHARED, NULL, &buffer, &err));

    nvn_gpu_cmd cmd;
    GPU(nvn_gpu_cmd_begin(NULL, &cmd, &err));
    begin_pass(cmd, cv, NVN_GPU_LOAD_CLEAR, 4);
    nvn_gpu_pass_end(cmd);
    const float depth_clear[4] = {0.5f, 0, 0, 0};
    nvn_gpu_clear_view(cmd, dv, depth_clear, NULL, 0);
    nvn_gpu_copy_tex_buf(cmd, color, 0, 0, NULL, buffer, 0, 16, 64);
    nvn_gpu_copy_tex_buf(cmd, depth, 0, 0, NULL, buffer, 64, 16, 64);
    GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    const unsigned char *bytes = nvn_gpu_buf_cpu(buffer);
    for (unsigned i = 0; i < 16; i++) {
        REQUIRE(bytes[i * 4] == 64 && !bytes[i * 4 + 1] && !bytes[i * 4 + 2] && bytes[i * 4 + 3] == 255);
        uint32_t depth_bits;
        memcpy(&depth_bits, bytes + 64 + i * 4, 4);
        REQUIRE(depth_bits == 0x3f000000u);
    }
    nvn_gpu_view_release(cv);
    nvn_gpu_view_release(dv);
    nvn_gpu_tex_release(color);
    nvn_gpu_tex_release(depth);
    nvn_gpu_buf_release(buffer, NULL, NULL);
    GPU(nvn_gpu_close(5000000000ull, &err));
    REQUIRE(!s2a_vk_device_destroy(&device, 1));
    printf("dedicated RT: preference=%d color=16/16 depth=16/16 cache=ok\n", prefer_rt);
    return 0;
}
