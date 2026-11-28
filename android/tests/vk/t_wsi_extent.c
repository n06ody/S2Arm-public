// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real headless swapchains/presents under validation. The surface facts below
// model fixed/undefined extents and orientation changes; they do not prove
// Android compositor orientation. Invalid requests are intercepted strictly,
// never made acceptable by relaxing real-driver requirements.
#include "nvn_backend.h"
#include "vk_device.h"
#include "nvn_plat_vk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nvn_gpu_err error;
static unsigned cases, failures, creates, presented, window_queries;
static VkExtent2D current, minimum, maximum, window, last_extent;
static VkSurfaceTransformFlagBitsKHR transform, last_transform;
static VkSurfaceTransformFlagsKHR supported;
static int suboptimal;
static nvn_gpu_view source, srgb_source;
static struct s2a_vk_device device;
static VkFormat probe_format;
static VkImage probe_images[64];
static int probe_pixels, probe_crop, probe_upscale;
static unsigned pixel_cases, pixel_bad;
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "fixture failure line %d: %s (%s)\n", __LINE__, #x, error.why); exit(2); } } while (0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))

const char *const *nvn_plat_vk_instance_exts(uint32_t *n) {
    static const char *const exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    *n = 2; return exts;
}
VkResult nvn_plat_vk_surface_create(VkInstance i, void *native, VkSurfaceKHR *out) {
    (void)native; *out = VK_NULL_HANDLE;
    PFN_vkCreateHeadlessSurfaceEXT create = (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(i, "vkCreateHeadlessSurfaceEXT");
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkHeadlessSurfaceCreateInfoEXT ci = {0}; ci.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
    return create(i, &ci, NULL, out);
}
int nvn_plat_window_size(void *native, uint32_t *w, uint32_t *h) {
    (void)native; window_queries++;
    if (!window.width || !window.height) return 0;
    *w = window.width; *h = window.height; return 1;
}
void nvn_plat_safepoint(void) {}
int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_pipe_cache")) return 0;
    if (!strcmp(name, "nvk_upscale")) return probe_upscale;
    return dflt;
}
void nvn_plat_log(int level, const char *cls, const char *text) {
    (void)level; fprintf(stderr, "[%s] %s\n", cls ? cls : "", text ? text : "");
}
static void done(void *u, const nvn_gpu_done *d) { (void)u; (void)d; }

// Names, declarations and pointer types are checked against NDK 29 headers.
extern __typeof__(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) __real_vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
extern __typeof__(vkCreateSwapchainKHR) __real_vkCreateSwapchainKHR;
extern __typeof__(vkGetSwapchainImagesKHR) __real_vkGetSwapchainImagesKHR;
extern __typeof__(vkQueuePresentKHR) __real_vkQueuePresentKHR;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        VkPhysicalDevice p, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *c) {
    VkResult r = __real_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p, s, c);
    if (r != VK_SUCCESS) return r;
    // Every nonzero modeled size is inside the real headless surface limits.
    REQUIRE(c->minImageExtent.width <= 64 && c->minImageExtent.height <= 64);
    REQUIRE(c->maxImageExtent.width >= 4096 && c->maxImageExtent.height >= 4096);
    REQUIRE(c->supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR);
    if (probe_pixels) REQUIRE(c->supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    c->currentExtent = current; c->minImageExtent = minimum; c->maxImageExtent = maximum;
    c->currentTransform = transform; c->supportedTransforms = supported;
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR *c,
        const VkAllocationCallbacks *a, VkSwapchainKHR *s) {
    creates++; last_extent = c->imageExtent; last_transform = c->preTransform;
    if (!c->imageExtent.width || !c->imageExtent.height ||
        c->imageExtent.width < minimum.width || c->imageExtent.width > maximum.width ||
        c->imageExtent.height < minimum.height || c->imageExtent.height > maximum.height ||
        !(supported & c->preTransform) ||
        (c->preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR && !probe_pixels)) {
        *s = VK_NULL_HANDLE; return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!probe_pixels) return __real_vkCreateSwapchainKHR(d, c, a, s);
    // Headless exposes IDENTITY only. Observe the application's chosen preTransform above,
    // then hand the real driver an identity swapchain so we can read the pre-rotated pixels.
    VkSwapchainCreateInfoKHR headless = *c;
    headless.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    headless.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    probe_format = c->imageFormat;
    return __real_vkCreateSwapchainKHR(d, &headless, a, s);
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkGetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR s, uint32_t *n, VkImage *images) {
    VkResult r = __real_vkGetSwapchainImagesKHR(d, s, n, images);
    if (probe_pixels && r == VK_SUCCESS && images) {
        REQUIRE(*n <= sizeof probe_images / sizeof probe_images[0]);
        memcpy(probe_images, images, *n * sizeof *images);
    }
    return r;
}

static void read_rotated_pixels(VkQueue q, VkImage image) {
    VkDevice d = device.device;
    const VkDeviceSize bytes = (VkDeviceSize)last_extent.width * last_extent.height * 4;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buf;
    REQUIRE(vkCreateBuffer(d, &bi, NULL, &buf) == VK_SUCCESS);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d, buf, &req);
    const uint32_t type = s2a_vk_memory_type(&device, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    REQUIRE(type != UINT32_MAX);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size, .memoryTypeIndex = type};
    VkDeviceMemory mem;
    REQUIRE(vkAllocateMemory(d, &ai, NULL, &mem) == VK_SUCCESS);
    REQUIRE(vkBindBufferMemory(d, buf, mem, 0) == VK_SUCCESS);
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                 .queueFamilyIndex = device.queue_family};
    VkCommandPool pool;
    REQUIRE(vkCreateCommandPool(d, &pi, NULL, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    REQUIRE(vkAllocateCommandBuffers(d, &ci, &cb) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    REQUIRE(vkBeginCommandBuffer(cb, &begin) == VK_SUCCESS);
    VkImageMemoryBarrier bar = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {last_extent.width, last_extent.height, 1}};
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
    bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; bar.dstAccessMask = 0;
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &bar);
    REQUIRE(vkEndCommandBuffer(cb) == VK_SUCCESS);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    REQUIRE(vkCreateFence(d, &fi, NULL, &fence) == VK_SUCCESS);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    REQUIRE(vkQueueSubmit(q, 1, &si, fence) == VK_SUCCESS);
    REQUIRE(vkWaitForFences(d, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS);
    void *mapped = NULL;
    REQUIRE(vkMapMemory(d, mem, 0, bytes, 0, &mapped) == VK_SUCCESS);
    const unsigned char *data = mapped;
    const unsigned char rgb[5][4] = {{255,0,0,255}, {0,255,0,255}, {0,0,255,255}, {255,255,0,255}, {0,0,0,255}};
    const int bgra = probe_format == VK_FORMAT_B8G8R8A8_UNORM;
    REQUIRE(bgra || probe_format == VK_FORMAT_R8G8B8A8_UNORM);
    unsigned bad = 0;
    for (uint32_t y = 0; y < last_extent.height; y++) for (uint32_t x = 0; x < last_extent.width; x++) {
        uint32_t u = x, v = y;
        if (transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) { u = y; v = 63 - x; }
        if (transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) { u = 95 - x; v = 63 - y; }
        if (transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) { u = 95 - y; v = x; }
        unsigned quadrant = (u >= 48) + 2 * (v >= 32);
        if (probe_crop == 1) quadrant = u >= 8 && u < 80 && v >= 4 && v < 52 ? (u >= 44) + 2 * (v < 28) : 4;
        const unsigned char *p = data + 4 * (y * last_extent.width + x);
        const unsigned char *want = rgb[quadrant];
        if (probe_crop == 2) bad += abs((int)p[0] - 64) > 1 || abs((int)p[1] - 64) > 1 || abs((int)p[2] - 64) > 1 || p[3] != 255;
        else bad += p[0] != want[bgra ? 2 : 0] || p[1] != want[1] || p[2] != want[bgra ? 0 : 2] || p[3] != want[3];
    }
    pixel_cases++; pixel_bad += bad;
    printf("wsi-pixels transform=0x%x crop=%d good=%u/%u bad=%u\n", (unsigned)transform, probe_crop,
           last_extent.width * last_extent.height - bad, last_extent.width * last_extent.height, bad);
    vkUnmapMemory(d, mem);
    vkDestroyFence(d, fence, NULL);
    vkDestroyCommandPool(d, pool, NULL);
    vkDestroyBuffer(d, buf, NULL);
    vkFreeMemory(d, mem, NULL);
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *p) {
    if (probe_pixels) read_rotated_pixels(q, probe_images[p->pImageIndices[0]]);
    VkResult r = __real_vkQueuePresentKHR(q, p);
    if (r == VK_SUCCESS) { presented++; if (suboptimal) return VK_SUBOPTIMAL_KHR; }
    return r;
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkGetPhysicalDeviceSurfaceCapabilitiesKHR), PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR), "NDK caps signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateSwapchainKHR), PFN_vkCreateSwapchainKHR), "NDK swapchain signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkQueuePresentKHR), PFN_vkQueuePresentKHR), "NDK present signature");

static void reset(int undefined) {
    current = undefined ? (VkExtent2D){UINT32_MAX, UINT32_MAX} : (VkExtent2D){320, 180};
    minimum = (VkExtent2D){64, 64}; maximum = (VkExtent2D){4096, 4096};
    window = (VkExtent2D){320, 180}; transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    supported = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; suboptimal = 1;
}
static nvn_gpu_surface attach(uint32_t w, uint32_t h) {
    nvn_gpu_surface s = NULL; unsigned before = creates;
    GPU(nvn_gpu_surface_attach((void *)1, &s, &error));
    REQUIRE(s && creates == before + 1 && last_extent.width == w && last_extent.height == h);
    REQUIRE(last_transform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR);
    return s;
}
static void result(const char *name, unsigned bad, unsigned frames, unsigned rebuild) {
    cases++; failures += !!bad;
    printf("wsi-extent case=%s bad=%u frames=%u rebuilt=%u\n", name, !!bad, frames, rebuild);
}
// Expect an effective-size/transform change within 31 SUBOPTIMAL frames. A
// stable surface must remain usable for 40 frames (crosses two capability polls).
static void frames(const char *name, nvn_gpu_surface s, int stale, uint32_t w, uint32_t h) {
    unsigned count = 0, outdated = 0, rebuilt = 0, before = creates;
    uint64_t gen = nvn_gpu_surface_gen(s);
    for (; count < (stale ? 31u : 40u); count++) {
        nvn_gpu_drawable drawable = {0};
        nvn_gpu_rc r = nvn_gpu_drawable_acquire(s, 5000000000ull, NULL, &drawable, &error);
        if (r.code == NVN_GPU_E_OUTDATED) { outdated = 1; break; }
        GPU(r);
        nvn_gpu_present_desc p = {0}; p.src = source; p.filter = 1;
        GPU(nvn_gpu_present(s, &drawable, &p, count + 1, NULL, NULL, &error));
    }
    unsigned bad = outdated != (unsigned)stale || creates != before;
    if (outdated && stale) {
        GPU(nvn_gpu_surface_rebuild(s, &error)); rebuilt++;
        nvn_gpu_drawable drawable = {0};
        GPU(nvn_gpu_drawable_acquire(s, 5000000000ull, NULL, &drawable, &error));
        bad |= drawable.w != w || drawable.h != h || drawable.gen != gen + 1 ||
               creates != before + 1 || last_transform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        nvn_gpu_drawable_release(s, &drawable);
    }
    GPU(nvn_gpu_surface_detach(s, 5000000000ull, &error));
    result(name, bad, count, rebuilt);
}
static void reject(const char *name, int code, const char *why) {
    nvn_gpu_surface s = NULL; unsigned before = creates;
    memset(&error, 0, sizeof error);
    nvn_gpu_rc r = nvn_gpu_surface_attach((void *)1, &s, &error);
    unsigned bad = r.code != code || s != NULL || creates != before || (why && !strstr(error.why, why));
    printf("wsi-extent reject=%s expected_rc=%d actual_rc=%d creates=%u why=%s\n", name, code, r.code, creates - before, error.why);
    if (s) GPU(nvn_gpu_surface_detach(s, 5000000000ull, &error));
    result(name, bad, 0, 0);
}
static void zero_then_restore(int undefined) {
    reset(undefined); nvn_gpu_surface s = attach(320, 180);
    uint64_t gen = nvn_gpu_surface_gen(s); unsigned before = creates, rebuilt = 0;
    if (undefined) window.width = 0;
    else current = minimum = maximum = (VkExtent2D){0, 0};
    nvn_gpu_rc r = nvn_gpu_surface_rebuild(s, &error);
    unsigned bad = r.code != NVN_GPU_E_OUTDATED || creates != before || nvn_gpu_surface_gen(s) != gen;
    printf("wsi-extent zero-rebuild undefined=%d rc=%d creates=%u gen=%llu expected_gen=%llu\n", undefined, r.code, creates - before,
           (unsigned long long)nvn_gpu_surface_gen(s), (unsigned long long)gen);
    if (nvn_gpu_surface_gen(s) == gen) {
        reset(undefined);
        if (undefined) window = (VkExtent2D){180, 320};
        else current = (VkExtent2D){180, 320};
        GPU(nvn_gpu_surface_rebuild(s, &error)); rebuilt++;
        nvn_gpu_drawable drawable = {0};
        GPU(nvn_gpu_drawable_acquire(s, 5000000000ull, NULL, &drawable, &error));
        bad |= drawable.w != 180 || drawable.h != 320 || drawable.gen != gen + 1;
        nvn_gpu_present_desc p = {0}; p.src = source; p.filter = 1;
        GPU(nvn_gpu_present(s, &drawable, &p, 1, NULL, NULL, &error));
    }
    GPU(nvn_gpu_surface_detach(s, 5000000000ull, &error));
    result(undefined ? "zero-window-restore" : "zero-restore", bad, 0, rebuilt);
}

extern uint64_t s2a_vk_easu_hits(void);
static unsigned rotated_rebuild(nvn_gpu_surface s) {
    const uint64_t gen = nvn_gpu_surface_gen(s);
    nvn_gpu_drawable dr;
    GPU(nvn_gpu_drawable_acquire(s, 5000000000ull, NULL, &dr, &error));
    const nvn_gpu_present_desc pd = {.src = source};
    transform = supported = VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
    suboptimal = 1;
    probe_pixels = 0;   // 这一帧仍属于旧交换链，只借真实 present 报次优
    GPU(nvn_gpu_present(s, &dr, &pd, 1, NULL, NULL, &error));
    probe_pixels = 1;
    nvn_gpu_rc r = nvn_gpu_drawable_acquire(s, 5000000000ull, NULL, &dr, &error);
    if (r.code != NVN_GPU_E_OUTDATED) {
        if (nvn_gpu_ok(r)) nvn_gpu_drawable_release(s, &dr);
        return 1;
    }
    GPU(nvn_gpu_surface_rebuild(s, &error));
    suboptimal = 0;
    unsigned bad = last_transform != transform || last_extent.width != 64 || last_extent.height != 96 ||
                   nvn_gpu_surface_gen(s) != gen + 1;
    GPU(nvn_gpu_drawable_acquire(s, 5000000000ull, NULL, &dr, &error));
    probe_crop = 0;
    bad |= dr.w != 96 || dr.h != 64;
    GPU(nvn_gpu_present(s, &dr, &pd, 1, NULL, NULL, &error));
    probe_crop = 2;
    const nvn_gpu_present_desc srgb = {.src = srgb_source, .filter = 1};
    for (probe_upscale = 0; probe_upscale < 2; probe_upscale++) {
        GPU(nvn_gpu_drawable_acquire(s, 5000000000ull, NULL, &dr, &error));
        const uint64_t hits = s2a_vk_easu_hits();
        GPU(nvn_gpu_present(s, &dr, &srgb, 1, NULL, NULL, &error));
        bad |= s2a_vk_easu_hits() - hits != (unsigned)probe_upscale;
    }
    probe_upscale = 0;
    printf("wsi-rotated-rebuild gen=%llu->%llu extent=%ux%u bad=%u\n", (unsigned long long)gen,
           (unsigned long long)nvn_gpu_surface_gen(s), last_extent.width, last_extent.height, !!bad);
    return bad;
}

static void rotated_case(VkSurfaceTransformFlagBitsKHR orientation, int undefined) {
    reset(undefined);
    current = undefined ? current : (VkExtent2D){96, 64};
    window = (VkExtent2D){96, 64};
    supported = transform = orientation;
    suboptimal = 0;
    probe_pixels = 1;
    nvn_gpu_surface s = NULL;
    memset(&error, 0, sizeof error);
    nvn_gpu_rc r = nvn_gpu_surface_attach((void *)1, &s, &error);
    const int swapped = orientation == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR || orientation == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
    unsigned bad = !nvn_gpu_ok(r) || !s;
    if (!bad) {
        bad |= last_extent.width != (swapped ? 64u : 96u) || last_extent.height != (swapped ? 96u : 64u) || last_transform != orientation;
        if (!bad) for (probe_crop = 0; probe_crop < 2; probe_crop++) {
            nvn_gpu_drawable dr = {0};
            GPU(nvn_gpu_drawable_acquire(s, 5000000000ull, NULL, &dr, &error));
            bad |= dr.w != 96 || dr.h != 64;
            nvn_gpu_present_desc pd = {.src = source};
            if (probe_crop) {
                memcpy(pd.crop, (int32_t[]){8, 8, 48, 48}, sizeof pd.crop);
                memcpy(pd.dst, (int32_t[]){8, 4, 72, 48}, sizeof pd.dst);
                pd.flip_y = 1;
            }
            GPU(nvn_gpu_present(s, &dr, &pd, 1, NULL, NULL, &error));
        }
        if (!bad && orientation == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) bad |= rotated_rebuild(s);
        GPU(nvn_gpu_surface_detach(s, 5000000000ull, &error));
    }
    printf("wsi-transform transform=0x%x undefined=%d rc=%d bad=%u why=%s\n", (unsigned)orientation, undefined, r.code, !!bad, error.why);
    probe_pixels = 0;
    result("pretransform", bad, 0, 0);
}

int main(void) {
    char why[256] = {0};
    REQUIRE(!s2a_vk_device_create(&device, why, sizeof why));
    nvn_gpu_open_desc desc = {0}; desc.host_device = &device; desc.app_label = "wsi-extent-test";
    GPU(nvn_gpu_open(&desc, &error));
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = 64; td.d.height = 64; td.d.depth = 1; td.d.levels = 1;
    td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER | NVN_GPU_TEX_CPU_UPLOAD;
    nvn_gpu_tex tex = NULL; GPU(nvn_gpu_tex_create(&td, &tex, &error));
    GPU(nvn_gpu_view_create(tex, NULL, &source, &error));
    td.d.format = NVN_FORMAT_RGBA8_SRGB;
    nvn_gpu_tex srgb_tex = NULL; GPU(nvn_gpu_tex_create(&td, &srgb_tex, &error));
    GPU(nvn_gpu_view_create(srgb_tex, NULL, &srgb_source, &error));
    unsigned char pixels[64 * 64 * 4];
    const unsigned char quadrants[4][4] = {{255,0,0,255}, {0,255,0,255}, {0,0,255,255}, {255,255,0,255}};
    for (unsigned y = 0; y < 64; y++) for (unsigned x = 0; x < 64; x++)
        memcpy(pixels + 4 * (y * 64 + x), quadrants[(x >= 32) + 2 * (y >= 32)], 4);
    nvn_gpu_cmd cmd = NULL; GPU(nvn_gpu_cmd_begin(NULL, &cmd, &error));
    nvn_gpu_tex_upload(cmd, tex, 0, 0, NULL, pixels, 64 * 4, sizeof pixels);
    unsigned char srgb_pixels[64 * 64 * 4];
    for (unsigned i = 0; i < sizeof srgb_pixels; i += 4)
        memcpy(srgb_pixels + i, (const unsigned char[]){64,64,64,255}, 4);
    nvn_gpu_tex_upload(cmd, srgb_tex, 0, 0, NULL, srgb_pixels, 64 * 4, sizeof srgb_pixels);
    GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &error));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &error));

    reset(0); window = (VkExtent2D){640, 360}; unsigned queries = window_queries;
    nvn_gpu_surface s = attach(320, 180); window = (VkExtent2D){800, 450};
    frames("fixed-ignores-window", s, 0, 320, 180); REQUIRE(window_queries == queries);
    reset(1); s = attach(320, 180); frames("undefined-stable", s, 0, 320, 180);
    reset(1); s = attach(320, 180); window = (VkExtent2D){180, 320};
    frames("undefined-resize", s, 1, 180, 320);
    reset(1); maximum = (VkExtent2D){256, 256}; window = (VkExtent2D){640, 360};
    s = attach(256, 256); window = (VkExtent2D){800, 450}; frames("clamped-stable", s, 0, 256, 256);
    reset(1); maximum = (VkExtent2D){256, 256}; window = (VkExtent2D){640, 360};
    s = attach(256, 256); window = (VkExtent2D){180, 320}; frames("clamped-resize", s, 1, 180, 256);
    reset(0); s = attach(320, 180); current = (VkExtent2D){180, 320}; frames("fixed-resize", s, 1, 180, 320);
    reset(0); transform = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR; supported |= transform;
    s = attach(320, 180); frames("rotation-90-stable", s, 0, 320, 180);
    reset(0); s = attach(320, 180); transform = VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR; supported |= transform;
    current = (VkExtent2D){180, 320}; frames("rotation-270-change", s, 1, 180, 320);
    reset(0); current = minimum = maximum = (VkExtent2D){0, 0}; reject("zero-extent", NVN_GPU_E_REVOKED, NULL);
    reset(1); window.width = 0; reject("zero-window", NVN_GPU_E_REVOKED, NULL);
    reset(0); transform = VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_BIT_KHR; supported = transform;
    reject("mirror", NVN_GPU_E_UNSUPPORTED, "preTransform");
    reset(0); transform = VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR; supported = transform;
    reject("inherit", NVN_GPU_E_UNSUPPORTED, "preTransform");
    reset(0); transform = (VkSurfaceTransformFlagBitsKHR)(VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR);
    supported |= transform; reject("multiple-transform-bits", NVN_GPU_E_UNSUPPORTED, "preTransform");
    zero_then_restore(0); zero_then_restore(1);
    rotated_case(VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, 0);
    rotated_case(VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR, 0);
    rotated_case(VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR, 0);
    rotated_case(VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR, 1);
    nvn_gpu_view_release(source); nvn_gpu_tex_release(tex);
    nvn_gpu_view_release(srgb_source); nvn_gpu_tex_release(srgb_tex);
    GPU(nvn_gpu_close(5000000000ull, &error)); REQUIRE(!s2a_vk_device_destroy(&device, 1));
    printf("wsi-extent cases=%u failures=%u real_presents=%u\n", cases, failures, presented);
    printf("wsi-pixels cases=%u bad=%u\n", pixel_cases, pixel_bad);
    return failures || pixel_bad || pixel_cases != 11 ? 1 : 0;
}
