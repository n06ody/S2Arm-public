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
#include <time.h>
#include <stdlib.h>
#include <string.h>

static nvn_gpu_err error;
static unsigned cases, failures, creates, presented, window_queries;
static VkExtent2D current, minimum, maximum, window, last_extent;
static VkSurfaceTransformFlagBitsKHR transform, last_transform;
static VkSurfaceTransformFlagsKHR supported;
static int suboptimal, idle_test, passes_test;
static int mailbox_on;
static enum { MODES_REAL, MODES_FIFO_ONLY, MODES_COUNT_ERROR, MODES_LIST_ERROR, MODES_INCOMPLETE } mode_probe;
static unsigned mode_queries;
static VkSurfaceKHR last_surface;
static VkPresentModeKHR last_mode;
static nvn_gpu_view source;
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
    if (!strcmp(name, "nvk_gpu_idle")) return idle_test;
    if (!strcmp(name, "nvk_gpu_passes")) return passes_test;
    if (!strcmp(name, "nvk_pipe_cache")) return 0;
    if (!strcmp(name, "nvk_present_mailbox") && mailbox_on) return 1;
    return dflt;
}
void nvn_plat_log(int level, const char *cls, const char *text) {
    (void)level; fprintf(stderr, "[%s] %s\n", cls ? cls : "", text ? text : "");
}
static void done(void *u, const nvn_gpu_done *d) { (void)u; (void)d; }

// Names, declarations and pointer types are checked against NDK 29 headers.
extern __typeof__(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) __real_vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
extern __typeof__(vkGetPhysicalDeviceSurfacePresentModesKHR) __real_vkGetPhysicalDeviceSurfacePresentModesKHR;
extern __typeof__(vkCreateSwapchainKHR) __real_vkCreateSwapchainKHR;
extern __typeof__(vkQueuePresentKHR) __real_vkQueuePresentKHR;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        VkPhysicalDevice p, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *c) {
    VkResult r = __real_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p, s, c);
    if (r != VK_SUCCESS) return r;
    // Every nonzero modeled size is inside the real headless surface limits.
    REQUIRE(c->minImageExtent.width <= 64 && c->minImageExtent.height <= 64);
    REQUIRE(c->maxImageExtent.width >= 4096 && c->maxImageExtent.height >= 4096);
    REQUIRE(c->supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR);
    c->currentExtent = current; c->minImageExtent = minimum; c->maxImageExtent = maximum;
    c->currentTransform = transform; c->supportedTransforms = supported;
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkGetPhysicalDeviceSurfacePresentModesKHR(
        VkPhysicalDevice p, VkSurfaceKHR s, uint32_t *n, VkPresentModeKHR *modes) {
    mode_queries++;
    if (mode_probe == MODES_REAL)
        return __real_vkGetPhysicalDeviceSurfacePresentModesKHR(p, s, n, modes);
    if (!modes) {
        if (mode_probe == MODES_COUNT_ERROR) return VK_ERROR_OUT_OF_HOST_MEMORY;
        *n = 1;
        return VK_SUCCESS;
    }
    if (mode_probe == MODES_LIST_ERROR) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!*n) return VK_INCOMPLETE;
    modes[0] = VK_PRESENT_MODE_FIFO_KHR;
    *n = 1;
    return mode_probe == MODES_INCOMPLETE ? VK_INCOMPLETE : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR *c,
        const VkAllocationCallbacks *a, VkSwapchainKHR *s) {
    creates++; last_extent = c->imageExtent; last_transform = c->preTransform;
    last_surface = c->surface; last_mode = c->presentMode;
    if ((mode_probe == MODES_FIFO_ONLY && c->presentMode != VK_PRESENT_MODE_FIFO_KHR) ||
        !c->imageExtent.width || !c->imageExtent.height ||
        c->imageExtent.width < minimum.width || c->imageExtent.width > maximum.width ||
        c->imageExtent.height < minimum.height || c->imageExtent.height > maximum.height ||
        !(supported & c->preTransform) || c->preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
        *s = VK_NULL_HANDLE; return VK_ERROR_INITIALIZATION_FAILED;
    }
    return __real_vkCreateSwapchainKHR(d, c, a, s);
}
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *p) {
    VkResult r = __real_vkQueuePresentKHR(q, p);
    if (r == VK_SUCCESS) { presented++; if (suboptimal) return VK_SUBOPTIMAL_KHR; }
    return r;
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkGetPhysicalDeviceSurfaceCapabilitiesKHR), PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR), "NDK caps signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkGetPhysicalDeviceSurfacePresentModesKHR), PFN_vkGetPhysicalDeviceSurfacePresentModesKHR), "NDK present modes signature");
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
static unsigned expect_mode(const char *name, VkPresentModeKHR expected, unsigned queries, int queried) {
    const unsigned calls = mode_queries - queries;
    const unsigned bad = last_mode != expected || (calls != 0) != queried;
    printf("wsi-present-mode case=%s bad=%u expected=%u actual=%u queries=%u\n",
           name, bad, (unsigned)expected, (unsigned)last_mode, calls);
    return bad;
}

static void present_one(nvn_gpu_surface s, uint64_t id) {
    nvn_gpu_drawable drawable = {0};
    GPU(nvn_gpu_drawable_acquire(s, 5000000000ull, NULL, &drawable, &error));
    const nvn_gpu_present_desc p = {.src = source, .filter = 1};
    GPU(nvn_gpu_present(s, &drawable, &p, id, NULL, NULL, &error));
}

static unsigned test_present_modes(VkPhysicalDevice physical) {
    reset(0);
    suboptimal = 0;
    unsigned queries = mode_queries;
    nvn_gpu_surface s = attach(320, 180);
    unsigned bad = expect_mode("default-fifo", VK_PRESENT_MODE_FIFO_KHR, queries, 0);

    // 正向枚举和建链都由真实 ICD 完成。负控只从驱动事实里隐藏可选模式，仍向真实 driver 建 FIFO。
    VkPresentModeKHR modes[32];
    uint32_t n = 32;
    REQUIRE(__real_vkGetPhysicalDeviceSurfacePresentModesKHR(physical, last_surface, &n, modes) == VK_SUCCESS);
    int has_mailbox = 0;
    for (uint32_t i = 0; i < n; i++) has_mailbox |= modes[i] == VK_PRESENT_MODE_MAILBOX_KHR;
    REQUIRE(has_mailbox);
    GPU(nvn_gpu_surface_detach(s, 5000000000ull, &error));

    mailbox_on = 1;
    queries = mode_queries;
    s = attach(320, 180);
    bad += expect_mode("advertised-mailbox", VK_PRESENT_MODE_MAILBOX_KHR, queries, 1);
    for (uint64_t id = 1; id <= 4; id++) present_one(s, id);

    queries = mode_queries;
    mode_probe = MODES_FIFO_ONLY;
    GPU(nvn_gpu_surface_rebuild(s, &error));
    bad += expect_mode("rebuild-fifo-only", VK_PRESENT_MODE_FIFO_KHR, queries, 1);
    present_one(s, 5);
    GPU(nvn_gpu_surface_detach(s, 5000000000ull, &error));

    const char *const names[] = {"missing-mailbox", "count-error", "list-error", "incomplete-without-mailbox"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        mode_probe = MODES_FIFO_ONLY + i;
        queries = mode_queries;
        s = attach(320, 180);
        bad += expect_mode(names[i], VK_PRESENT_MODE_FIFO_KHR, queries, 1);
        present_one(s, 6 + i);
        GPU(nvn_gpu_surface_detach(s, 5000000000ull, &error));
    }
    printf("wsi-present-mode cases=7 failures=%u real_presents=%u\n", bad, presented);
    return bad;
}

int main(int argc, char **argv) {
    passes_test = argc == 2 && !strcmp(argv[1], "--gpu-idle-passes");
    idle_test = passes_test || (argc == 2 && !strcmp(argv[1], "--gpu-idle"));
    struct s2a_vk_device device = {0}; char why[256] = {0};
    REQUIRE(!s2a_vk_device_create(&device, why, sizeof why));
    nvn_gpu_open_desc desc = {0}; desc.host_device = &device; desc.app_label = "wsi-extent-test";
    GPU(nvn_gpu_open(&desc, &error));
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = 64; td.d.height = 64; td.d.depth = 1; td.d.levels = 1;
    td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER | NVN_GPU_TEX_CPU_UPLOAD;
    nvn_gpu_tex tex = NULL; GPU(nvn_gpu_tex_create(&td, &tex, &error));
    GPU(nvn_gpu_view_create(tex, NULL, &source, &error));
    unsigned char pixels[64 * 64 * 4]; memset(pixels, 128, sizeof pixels);
    nvn_gpu_cmd cmd = NULL; GPU(nvn_gpu_cmd_begin(NULL, &cmd, &error));
    nvn_gpu_tex_upload(cmd, tex, 0, 0, NULL, pixels, 64 * 4, sizeof pixels);
    GPU(nvn_gpu_cmd_commit(cmd, 1, done, NULL, &error));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &error));

    if (argc == 2 && !strcmp(argv[1], "--present-modes")) {
        const unsigned bad = test_present_modes(device.physical);
        nvn_gpu_view_release(source); nvn_gpu_tex_release(tex);
        GPU(nvn_gpu_close(5000000000ull, &error)); REQUIRE(!s2a_vk_device_destroy(&device, 1));
        return bad ? 1 : 0;
    }
    if (idle_test) {
        // guest upload, guest, present, guest. The last gap is after the present.
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &error));
        GPU(nvn_gpu_cmd_commit(cmd, 2, done, NULL, &error));
        reset(0);
        nvn_gpu_surface surface = attach(320, 180);
        nvn_gpu_drawable drawable = {0};
        GPU(nvn_gpu_drawable_acquire(surface, 5000000000ull, NULL, &drawable, &error));
        nvn_gpu_present_desc p = {0}; p.src = source; p.filter = 1;
        GPU(nvn_gpu_present(surface, &drawable, &p, 1, NULL, NULL, &error));
        GPU(nvn_gpu_surface_detach(surface, 5000000000ull, &error));
        struct timespec delay = {5, 100000000};
        nanosleep(&delay, NULL);
        GPU(nvn_gpu_cmd_begin(NULL, &cmd, &error));
        GPU(nvn_gpu_cmd_commit(cmd, 3, done, NULL, &error));
        GPU(nvn_gpu_fence_wait_at(3, 5000000000ull, &error, NVN_GPU_WAIT_SYNC));
        nvn_gpu_view_release(source); nvn_gpu_tex_release(tex);
        GPU(nvn_gpu_close(5000000000ull, &error));
        REQUIRE(!s2a_vk_device_destroy(&device, 1));
        return 0;
    }

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
    reset(0); transform = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR; supported = transform;
    reject("missing-identity", NVN_GPU_E_UNSUPPORTED, "identity");
    zero_then_restore(0); zero_then_restore(1);
    nvn_gpu_view_release(source); nvn_gpu_tex_release(tex);
    GPU(nvn_gpu_close(5000000000ull, &error)); REQUIRE(!s2a_vk_device_destroy(&device, 1));
    printf("wsi-extent cases=%u failures=%u real_presents=%u\n", cases, failures, presented);
    return failures ? 1 : 0;
}
